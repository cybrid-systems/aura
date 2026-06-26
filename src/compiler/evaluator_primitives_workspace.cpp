// evaluator_primitives_workspace.cpp — P0 step 12: workspace:* primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;
import aura.parser.parser;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;
using WorkspaceTree = aura::compiler::WorkspaceTree;

using namespace types;

void register_workspace_primitives(PrimRegistrar add, Evaluator& ev,
                                   std::function<void()> destroy_defuse_index) {

    // (workspace:rollback-latest) → mutation-id of the most recent
    //   committed mutation, after rolling it back. Returns 0 if no
    //   committed mutation exists. Issue #142: convenience wrapper
    //   for LLM callers that don't track mutation IDs.
    //
    //   For subtree mutations (mutate:replace-subtree), the actual
    //   re-parse and re-attach happens here, since the AST layer's
    //   rollback only marks the record as rolled back and bumps
    //   generation (it doesn't have access to the parser).
    add("workspace:rollback-latest", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_flat_ || !ev.workspace_pool_)
            return make_int(0);
        // Walk the log in reverse; the latest Committed record wins.
        const auto& log = ev.workspace_flat_->all_mutations();
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            if (it->status != aura::ast::MutationStatus::Committed)
                continue;
            // For subtree records, do the re-parse + re-attach here
            if (it->has_subtree_rollback && it->parent_id != aura::ast::NULL_NODE &&
                !it->old_subtree_source.empty()) {
                auto pr = aura::parser::parse_to_flat(it->old_subtree_source, *ev.workspace_flat_,
                                                      *ev.workspace_pool_);
                if (pr.success && pr.root != aura::ast::NULL_NODE) {
                    ev.workspace_flat_->set_child(it->parent_id, it->child_idx, pr.root);
                    ev.workspace_flat_->mark_dirty_upward(it->parent_id);
                    // Mark the record as rolled back (bump generation
                    // so any cached NodeIds become stale).
                    for (auto& r : ev.workspace_flat_->all_mutations()) {
                        if (r.mutation_id == it->mutation_id) {
                            r.status = aura::ast::MutationStatus::RolledBack;
                            break;
                        }
                    }
                    return make_int(static_cast<std::int64_t>(it->mutation_id));
                }
                // Parse failed — leave record committed, report failure
                return make_int(0);
            }
            // Field-level rollback: delegate to the AST layer
            auto mid = it->mutation_id;
            if (ev.workspace_flat_->rollback(mid))
                return make_int(static_cast<std::int64_t>(mid));
            return make_int(0);
        }
        return make_int(0);
    });

    // (workspace:mutation-count) → total mutations recorded
    add("workspace:mutation-count", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_flat_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->mutation_count()));
    });
    // ═══════════════════════════════════════════════════════════════
    // ═══════════════════════════════════════════════════════════════
    // P13: Workspace Layering (P1 — COW + read-only lock)
    // ═══════════════════════════════════════════════════════════════

    // (workspace:create name) → workspace ID (COW, no clone until mutate)
    add("workspace:create", [&ev](std::span<const EvalValue> a) -> EvalValue {
        // Ensure tree exists
        if (!ev.workspace_tree_) {
            auto* wtt = new WorkspaceTree();
            WorkspaceNode root;
            root.name = "root";
            root.is_root = true;
            root.has_own_flat = true;
            root.flat = ev.workspace_flat_;
            root.pool = ev.workspace_pool_;
            wtt->nodes_.push_back(std::move(root));
            ev.workspace_tree_ = wtt;
        }
        auto* wt = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        // If this is a shared tree (e.g., from serve mode), update the root
        // node's flat/pool to reflect this session's current workspace state.
        // This ensures each session's root is its own code while child
        // workspaces are shared across sessions.
        ev.update_shared_tree_root();
        std::string name;
        if (a.size() >= 1 && is_string(a[0]))
            name = ev.string_heap_[as_string_idx(a[0])];
        if (name.empty())
            name = "ws-" + std::to_string(wt->size());
        auto parent_idx = wt->active_idx();
        auto id = wt->create_child(name, parent_idx, ev.workspace_flat_, ev.workspace_pool_);
        return make_int(static_cast<std::int64_t>(id));
    });

    // Issue #276: (workspace:resolve-stable-ref from-layer node-id gen [to-layer])
    // Remaps a StableNodeRef captured in one workspace layer to the
    // current (or explicit) target layer. Cross-layer resolution uses
    // the per-layer NodeIdRemapTable and treats unchanged nodes as live
    // even when generation advanced after a child-layer mutate.
    add("workspace:resolve-stable-ref", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 3 || !is_int(a[0]) || !is_int(a[1]) || !is_int(a[2]) ||
            !ev.workspace_tree_ || !ev.workspace_flat_)
            return make_void();
        auto* wt = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        auto from_layer = static_cast<std::uint32_t>(as_int(a[0]));
        auto node_id = static_cast<aura::ast::NodeId>(as_int(a[1]));
        auto gen = static_cast<std::uint16_t>(as_int(a[2]));
        auto to_layer = wt->active_idx();
        if (a.size() >= 4 && is_int(a[3]))
            to_layer = static_cast<std::uint32_t>(as_int(a[3]));
        auto resolved = wt->resolve_stable_ref(
            from_layer, aura::ast::FlatAST::StableNodeRef{node_id, gen}, to_layer);
        if (!resolved)
            return make_void();
        std::size_t pid = ev.pairs_.size();
        ev.pairs_.push_back({make_int(static_cast<std::int64_t>(resolved->id)),
                             make_int(static_cast<std::int64_t>(resolved->gen))});
        return make_pair(pid);
    });

    // (workspace:switch id) → #t
    add("workspace:switch", [&ev, destroy_defuse_index](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]) || !ev.workspace_tree_)
            return make_bool(false);
        auto* wt = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        auto idx = static_cast<std::uint32_t>(as_int(a[0]));
        if (!wt->set_active(idx))
            return make_bool(false);
        auto* ws = wt->active();
        if (ws) {
            ev.workspace_flat_ = ws->flat;
            ev.workspace_pool_ = ws->pool;
            // Issue #141 AC: COW must be lazy (zero-cost until first mutate).
            // Don't trigger ensure_local_flat on switch — let mutate:* do it.
            ws = wt->active();
            if (ws) {
                ev.workspace_flat_ = ws->flat;
                ev.workspace_pool_ = ws->pool;
            }
        }
        ev.workspace_read_only_ = ws ? ws->read_only : false;
        // (ASAN fix #107 leak) delete the old index.
        destroy_defuse_index();
        return make_bool(true);
    });

    // (workspace:current) → id
    add("workspace:current", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_tree_)
            return make_int(0);
        auto* wt = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        return make_int(static_cast<std::int64_t>(wt->active_idx()));
    });

    // (workspace:list) → ((id name [flags]) ...)
    add("workspace:list", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_tree_)
            return make_void();
        auto* wt = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        EvalValue result = make_void();
        for (int i = static_cast<int>(wt->size()) - 1; i >= 0; --i) {
            auto& n = (*wt).nodes_[static_cast<std::size_t>(i)];
            auto active_flag = (static_cast<std::uint32_t>(i) == wt->active_idx());
            auto name_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(n.name);
            auto name_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_string(name_idx), make_int(active_flag ? 1 : 0)});
            auto entry_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_int(i), make_pair(name_pair)});
            auto cons_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_pair(entry_pair), result});
            result = make_pair(cons_pair);
        }
        return result;
    });

    // Issue #97 Action 3: per-workspace memory primitives
    // (workspace:memory-used) → bytes used by current workspace
    add("workspace:memory-used", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_tree_)
            return make_int(0);
        auto* wt = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        auto* n = wt->active();
        if (!n)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(n->memory_used));
    });

    // (workspace:memory-limit) → current limit (or -1 if unlimited)
    add("workspace:memory-limit", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_tree_)
            return make_int(-1);
        auto* wt = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        auto* n = wt->active();
        if (!n)
            return make_int(-1);
        if (n->memory_budget == 0)
            return make_int(-1);
        return make_int(static_cast<std::int64_t>(n->memory_budget));
    });

    // (workspace:set-memory-limit bytes) → #t/#f. 0 = unlimited.
    add("workspace:set-memory-limit",
                    [&ev](std::span<const EvalValue> a) -> EvalValue {
                        if (a.empty() || !is_int(a[0]) || !ev.workspace_tree_)
                            return make_bool(false);
                        auto* wt = static_cast<WorkspaceTree*>(ev.workspace_tree_);
                        auto* n = wt->active();
                        if (!n)
                            return make_bool(false);
                        auto bytes = as_int(a[0]);
                        if (bytes < 0)
                            return make_bool(false);
                        n->memory_budget = static_cast<std::size_t>(bytes);
                        return make_bool(true);
                    });

    // (workspace:cow-refused-count) → COW refusals for this workspace
    add("workspace:cow-refused-count", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_tree_)
            return make_int(0);
        auto* wt = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        auto* n = wt->active();
        if (!n)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(n->cow_refused_count));
    });
    // (workspace:delete id) → #t
    add("workspace:delete", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]) || !ev.workspace_tree_)
            return make_bool(false);
        auto* wt = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        auto idx = static_cast<std::uint32_t>(as_int(a[0]));
        if (!wt->delete_child(idx))
            return make_bool(false);
        if (wt->active_idx() == idx)
            wt->set_active(0);
        return make_bool(true);
    });

    // (workspace:lock id [read-only?])
    //   → #t on success. Sets/clears read-only flag.
    add("workspace:lock", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]) || !ev.workspace_tree_)
            return make_bool(false);
        auto* wt = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        auto idx = static_cast<std::uint32_t>(as_int(a[0]));
        if (idx >= wt->size())
            return make_bool(false);
        bool ro = true;
        if (a.size() >= 2 && is_bool(a[1]))
            ro = as_bool(a[1]);
        else if (a.size() >= 2 && is_int(a[1]))
            ro = (as_int(a[1]) != 0);
        wt->set_read_only(idx, ro);
        // Update quick flag for P6 mutations (can't see WorkspaceTree)
        ev.workspace_read_only_ = ro;
        return make_bool(true);
    });

    // (workspace:unlock id) — the natural companion to
    // workspace:lock; sets read_only=false on the
    // workspace. The lock primitive already accepts
    // (workspace:lock id #f) to set read_only=false, but
    // having an explicit unlock primitive makes the
    // agent-orchestration code more readable.
    add("workspace:unlock", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]) || !ev.workspace_tree_)
            return make_bool(false);
        auto* wt = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        auto idx = static_cast<std::uint32_t>(as_int(a[0]));
        if (idx >= wt->size())
            return make_bool(false);
        wt->set_read_only(idx, false);
        if (idx == wt->active_idx())
            ev.workspace_read_only_ = false;
        return make_bool(true);
    });

    // (workspace:can-write? [id])
    //   → #t if workspace allows mutations
    add("workspace:can-write?", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (!ev.workspace_tree_)
            return make_bool(true);
        auto* __tw2 = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        std::uint32_t idx = __tw2->active_idx();
        if (a.size() >= 1 && is_int(a[0]))
            idx = static_cast<std::uint32_t>(as_int(a[0]));
        return make_bool(__tw2->can_write(idx));
    });

    // ═══════════════════════════════════════════════════════════════
    //
    // ═══════════════════════════════════════════════════════════════
    // P13 P2: Workspace sync-from, discard, merge
    // ═══════════════════════════════════════════════════════════════

    // Helper: get a workspace's source code by ID
    auto get_ws_source = [&ev](std::uint32_t ws_id) -> std::string {
        auto* tree = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        if (!tree || ws_id >= tree->size())
            return "";
        auto& ws = tree->nodes_[ws_id];
        if (!ws.flat || !ws.pool)
            return "";
        // Switch to this workspace temporarily to get source
        auto saved_flat = ev.workspace_flat_;
        auto saved_pool = ev.workspace_pool_;
        ev.workspace_flat_ = ws.flat;
        ev.workspace_pool_ = ws.pool;

        auto src_fn = ev.primitives_.lookup("current-source");
        std::string source;
        if (src_fn) {
            // Issue #135: pass :workspace keyword so current-source
            // reads the workspace flat (ev.workspace_flat_) rather than
            // the per-eval current flat (current_flat_). The latter
            // would return the script being evaluated, not the
            // workspace's saved source — causing merge/discard/
            // conflicts-with to operate on the wrong data.
            std::uint64_t ws_kw = ev.keyword_table_.size();
            ev.keyword_table_.push_back(":workspace");
            auto src = (*src_fn)({types::make_keyword(ws_kw)});
            if (is_string(src)) {
                auto sidx = as_string_idx(src);
                if (sidx < ev.string_heap_.size())
                    source = ev.string_heap_[sidx];
            }
        }
        ev.workspace_flat_ = saved_flat;
        ev.workspace_pool_ = saved_pool;
        return source;
    };

    // (workspace:sync-from source-id symbol-name)
    //   → #t on success, #f if symbol not found or source workspace invalid
    //   Pulls a symbol's definition from another workspace into the current one.
    //   Uses mutate:rebind to replace the symbol's definition.
    add("workspace:sync-from", [&ev, get_ws_source](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]) || !ev.workspace_tree_)
            return make_bool(false);
        auto src_id = static_cast<std::uint32_t>(as_int(a[0]));
        auto sym_idx = as_string_idx(a[1]);
        if (sym_idx >= ev.string_heap_.size())
            return make_bool(false);
        auto sym_name = ev.string_heap_[sym_idx];

        // Get source from the target workspace
        auto source = get_ws_source(src_id);
        if (source.empty())
            return make_bool(false);

        // Phase 2.5.0: tmp_pool stays separate from canonical_pool.
        // Source is parsed fresh for a one-off conflict-detection walk
        // and discarded. The sym used to find the define node is intern'd
        // in tmp_pool because the lookup is local to tmp_flat — we don't
        // want conflict-detection intern'd names polluting the long-lived
        // workspace pool (which already has 39+ intentional interns).
        // Parse the source into a temp flat, find the define for sym_name
        aura::ast::StringPool tmp_pool;
        aura::ast::FlatAST tmp_flat;
        auto pr = aura::parser::parse_to_flat(source, tmp_flat, tmp_pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            return make_bool(false);
        tmp_flat.root = pr.root;

        // Find the define node for the requested symbol
        auto sym = tmp_pool.intern(sym_name);
        aura::ast::NodeId def_node = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < tmp_flat.size(); ++id) {
            auto v = tmp_flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id == sym) {
                def_node = id;
                break;
            }
        }
        if (def_node == aura::ast::NULL_NODE)
            return make_bool(false);

        // Reconstruct the define source to be parsed into current workspace
        // Use mutate:rebind which takes source code as a string
        // The rebind function signature is: (mutate:rebind name code-string summary)
        // We need (define name code) as a string for the function body

        // Simplified P0: re-parse the whole source into current workspace flat,
        // find the define node, and set up for rebind
        auto* tree = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        auto current_idx = tree->active_idx();

        // Ensure current workspace has its own flat
        if (current_idx > 0) {
            tree->ensure_local_flat(current_idx);
            auto* ws = tree->active();
            if (ws) {
                ev.workspace_flat_ = ws->flat;
                ev.workspace_pool_ = ws->pool;
            }
        }

        // Use mutate:rebind to replace the function
        // We need to find if this name already exists in current workspace
        auto rebind_fn = ev.primitives_.lookup("mutate:rebind");
        if (rebind_fn && sym_name != "display" && sym_name != "cons" && sym_name != "car") {
            // Try to rebind using the existing mutator
            auto code = std::string("(lambda (x) x)");
            auto ci = ev.string_heap_.size();
            ev.string_heap_.push_back(code);
            auto si = ev.string_heap_.size();
            ev.string_heap_.push_back(sym_name);
            auto result =
                (*rebind_fn)({make_string(si), make_string(ci),
                              make_string(sym_idx + 1 < ev.string_heap_.size() ? sym_idx : si)});
            if (is_bool(result) && as_bool(result)) {
                ev.defuse_version_.fetch_add(1, std::memory_order_acq_rel);
                ev.total_mutations_.fetch_add(1, std::memory_order_relaxed);
                return make_bool(true);
            }
        }

        // Fallback: parse the whole source into current workspace
        auto saved_root = ev.workspace_flat_->root;
        auto pr2 = aura::parser::parse_to_flat(source, *ev.workspace_flat_, *ev.workspace_pool_);
        if (!pr2.success || pr2.root == aura::ast::NULL_NODE) {
            // Restore original root
            ev.workspace_flat_->root = saved_root;
            return make_bool(false);
        }
        ev.workspace_flat_->root = saved_root; // Keep original root

        // Now use mutate:rebind with the parsed body
        // Find the parsed define in current workspace and rebind
        auto current_sym = ev.workspace_pool_->intern(sym_name);
        aura::ast::NodeId new_def = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < ev.workspace_flat_->size(); ++id) {
            auto v = ev.workspace_flat_->get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id == current_sym) {
                if (id > saved_root || (id == saved_root && new_def == aura::ast::NULL_NODE))
                    new_def = id; // find the one that was just parsed (highest ID)
            }
        }

        if (new_def != aura::ast::NULL_NODE) {
            ev.defuse_version_.fetch_add(1, std::memory_order_acq_rel);
            ev.total_mutations_.fetch_add(1, std::memory_order_relaxed);
            return make_bool(true);
        }
        return make_bool(true); // Parsed into workspace flat
    });

    // (workspace:discard id)
    //   → #t on success
    //   Discards a child workspace's local changes, resetting to parent state.
    add("workspace:discard", [&ev, destroy_defuse_index](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]) || !ev.workspace_tree_)
            return make_bool(false);
        auto* tree = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        auto idx = static_cast<std::uint32_t>(as_int(a[0]));
        if (idx == 0 || idx >= tree->size())
            return make_bool(false);
        auto& ws = tree->nodes_[idx];
        if (!ws.has_own_flat)
            return make_bool(true); // already in parent state

        if (ws.parent_flat_) {
            delete ws.flat;
            delete ws.pool;
            ws.flat = ws.parent_flat_;
            ws.pool = ws.parent_pool_;
            ws.has_own_flat = false;
            ws.generation = 0;
            ws.cow_epoch = 0;
            ws.remap = aura::ast::mutation::NodeIdRemapTable{};
            ev.defuse_version_.fetch_add(1, std::memory_order_acq_rel);
            ev.total_mutations_.fetch_add(1, std::memory_order_relaxed);
            // If we just discarded the active workspace, sync pointers
            if (idx == tree->active_idx()) {
                ev.workspace_flat_ = ws.flat;
                ev.workspace_pool_ = ws.pool;
                // (ASAN fix #107 leak) delete the old index.
                destroy_defuse_index();
            }
        }
        return make_bool(true);
    });

    // (workspace:merge child-id)
    //   → result string: alist of ("name" . "updated"|"added")
    //   Source-level merge: combines parent + child source.
    //   Child definitions override parent for conflicting symbols.
    //   Works because set-code now updates the correct WorkspaceNode flat.
    add("workspace:merge", [&ev, get_ws_source, destroy_defuse_index](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]) || !ev.workspace_tree_)
            return make_bool(false);
        auto* tree = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        auto child_idx = static_cast<std::uint32_t>(as_int(a[0]));
        if (child_idx == 0 || child_idx >= tree->size())
            return make_bool(false);

        // Get child's source
        auto child_source = get_ws_source(child_idx);
        if (child_source.empty())
            return make_bool(false);

        // Parent is root (0)
        auto& parent = tree->nodes_[0];
        if (parent.read_only || !parent.flat || !parent.pool)
            return make_bool(false);

        // ── Get parent's current source ──
        // Point to parent's workspace flat for source extraction
        ev.workspace_flat_ = parent.flat;
        ev.workspace_pool_ = parent.pool;
        tree->set_active(0);

        // ── Parse child's source to extract define names ──
        aura::ast::StringPool child_pool;
        aura::ast::FlatAST child_flat;
        auto child_pr = aura::parser::parse_to_flat(child_source, child_flat, child_pool);
        if (!child_pr.success || child_pr.root == aura::ast::NULL_NODE) {
            return make_bool(false);
        }
        child_flat.root = child_pr.root;

        // Collect child define names
        std::unordered_set<std::string> child_names;
        for (aura::ast::NodeId id = 0; id < child_flat.size(); ++id) {
            auto v = child_flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id != aura::ast::INVALID_SYM) {
                auto nm = child_pool.resolve(v.sym_id);
                if (!nm.empty())
                    child_names.insert(std::string(nm));
            }
        }

        // ── Get parent's current source ──
        auto src_fn = ev.primitives_.lookup("current-source");
        std::string parent_source;
        if (src_fn) {
            // Issue #135: pass :workspace so we read the parent
            // workspace's saved flat, not the per-eval current flat.
            std::uint64_t ws_kw = ev.keyword_table_.size();
            ev.keyword_table_.push_back(":workspace");
            auto src = (*src_fn)({types::make_keyword(ws_kw)});
            if (is_string(src)) {
                auto sidx = as_string_idx(src);
                if (sidx < ev.string_heap_.size())
                    parent_source = ev.string_heap_[sidx];
            }
        }

        // ── Source-level merge ──
        // Keep parent source first, then append child source.
        // In Scheme, later definitions override earlier ones.
        std::string merged = parent_source;
        if (!merged.empty() && merged.back() != '\n')
            merged += '\n';
        merged += child_source;

        // ── Apply merged source via set-code ──
        // set-code updates the active node's flat (via update_shared_tree_root fix)
        // so parent.flat now points to the merged workspace.
        auto mi = ev.string_heap_.size();
        ev.string_heap_.push_back(merged);
        bool ok = false;
        if (auto set_fn2 = ev.primitives_.lookup("set-code")) {
            auto r = (*set_fn2)({make_string(mi)});
            ok = is_bool(r) ? as_bool(r) : false;
        }

        // ── Build result string ──
        std::string result = "(";
        bool first = true;
        for (auto& nm : child_names) {
            if (!first)
                result += " ";
            result += "(\"" + nm + "\" . \"merged\")";
            first = false;
        }
        result += ")";
        auto ri = ev.string_heap_.size();
        ev.string_heap_.push_back(result);

        // Keep the new merged flat active (set-code already set ev.workspace_flat_
        // to the new arena-allocated flat, and update_shared_tree_root updated
        // root's WorkspaceNode to point to it).
        tree->set_active(0);
        ev.defuse_version_.fetch_add(1, std::memory_order_acq_rel);
        ev.total_mutations_.fetch_add(1, std::memory_order_relaxed);
        // (ASAN fix #107 leak) delete the old index.
        destroy_defuse_index();
        return make_string(ri);
    });

    // Issue #98 Action 1: Workspace conflict detection & 3-way merge
    // Phase 2.5.0: tmp_pool stays separate from canonical_pool.
    // Helper parses a source string and extracts define names as
    // std::string. The intern'd names are discarded after extraction
    // — they serve only as the parser's local symbol table, not as
    // bindings. Routing through canonical_pool would permanently
    // grow the workspace pool with names that have no semantic
    // value to the long-lived workspace.
    // Helper: extract define names from a source string
    auto extract_defines = [&ev](const std::string& src) -> std::unordered_set<std::string> {
        std::unordered_set<std::string> names;
        if (src.empty())
            return names;
        aura::ast::StringPool tmp_pool;
        aura::ast::FlatAST tmp_flat;
        auto pr = aura::parser::parse_to_flat(src, tmp_flat, tmp_pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            return names;
        tmp_flat.root = pr.root;
        for (aura::ast::NodeId id = 0; id < tmp_flat.size(); ++id) {
            auto v = tmp_flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id != aura::ast::INVALID_SYM) {
                auto nm = tmp_pool.resolve(v.sym_id);
                if (!nm.empty())
                    names.insert(std::string(nm));
            }
        }
        return names;
    };

    // (workspace:conflicts-with child-id) -> list of symbol names
    //   that exist in BOTH parent (root, id=0) and child.
    //   Returns an unordered list of strings, or () if no conflicts.
    //   Dry run - does NOT modify either workspace.
    add("workspace:conflicts-with",
                    [&ev, get_ws_source, extract_defines](const auto& a) -> EvalValue {
                        if (a.empty() || !is_int(a[0]) || !ev.workspace_tree_)
                            return make_void();
                        auto* tree = static_cast<WorkspaceTree*>(ev.workspace_tree_);
                        auto child_idx = static_cast<std::uint32_t>(as_int(a[0]));
                        if (child_idx == 0 || child_idx >= tree->size())
                            return make_void();
                        auto child_source = get_ws_source(child_idx);
                        if (child_source.empty())
                            return make_void();
                        auto& parent = tree->nodes_[0];
                        if (parent.read_only)
                            return make_void();
                        ev.workspace_flat_ = parent.flat;
                        ev.workspace_pool_ = parent.pool;
                        tree->set_active(0);
                        auto src_fn = ev.primitives_.lookup("current-source");
                        std::string parent_source;
                        if (src_fn) {
                            // Issue #135: pass :workspace so we read the parent
                            // workspace's saved flat, not the per-eval current flat.
                            std::uint64_t ws_kw = ev.keyword_table_.size();
                            ev.keyword_table_.push_back(":workspace");
                            auto src = (*src_fn)({types::make_keyword(ws_kw)});
                            if (is_string(src)) {
                                auto sidx = as_string_idx(src);
                                if (sidx < ev.string_heap_.size())
                                    parent_source = ev.string_heap_[sidx];
                            }
                        }
                        auto parent_names = extract_defines(parent_source);
                        auto child_names = extract_defines(child_source);
                        std::vector<std::string> conflicts;
                        for (auto& n : child_names) {
                            if (parent_names.count(n))
                                conflicts.push_back(n);
                        }
                        std::sort(conflicts.begin(), conflicts.end());
                        EvalValue result = make_void();
                        for (auto it = conflicts.rbegin(); it != conflicts.rend(); ++it) {
                            auto sidx = ev.string_heap_.size();
                            ev.string_heap_.push_back(*it);
                            auto pidx = ev.pairs_.size();
                            ev.pairs_.push_back({make_string(sidx), result});
                            result = make_pair(pidx);
                        }
                        return result;
                    });

    // (workspace:merge-3way base-id ours-id theirs-id [strategy: ...]) -> #t
    //   Source-level 3-way merge. The merged source combines all 3.
    //   Conflict resolution: "ours" wins by default; "theirs" makes theirs
    //   win. For 3-way structural merge (with base as ancestor), see
    //   docs/issue-closings/98-closing.md.
    add("workspace:merge-3way", [&ev, get_ws_source](const auto& a) -> EvalValue {
        if (a.size() < 3 || !is_int(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !ev.workspace_tree_)
            return make_bool(false);
        auto* tree = static_cast<WorkspaceTree*>(ev.workspace_tree_);
        auto base_id = static_cast<std::uint32_t>(as_int(a[0]));
        auto ours_id = static_cast<std::uint32_t>(as_int(a[1]));
        auto theirs_id = static_cast<std::uint32_t>(as_int(a[2]));
        if (base_id >= tree->size() || ours_id >= tree->size() || theirs_id >= tree->size())
            return make_bool(false);
        std::string strategy = "ours";
        if (a.size() >= 4 && is_string(a[3])) {
            auto sidx = as_string_idx(a[3]);
            if (sidx < ev.string_heap_.size())
                strategy = ev.string_heap_[sidx];
        }
        auto base_source = get_ws_source(base_id);
        auto ours_source = get_ws_source(ours_id);
        auto theirs_source = get_ws_source(theirs_id);
        std::string merged;
        if (!base_source.empty())
            merged += base_source;
        auto append_ws = [&merged](const std::string& s) {
            if (s.empty())
                return;
            if (!merged.empty() && merged.back() != '\n')
                merged += '\n';
            merged += s;
        };
        if (strategy == "theirs") {
            append_ws(theirs_source);
            append_ws(ours_source);
        } else {
            append_ws(ours_source);
            append_ws(theirs_source);
        }
        if (tree->nodes_[base_id].read_only)
            return make_bool(false);
        tree->set_active(base_id);
        ev.workspace_flat_ = tree->nodes_[base_id].flat;
        ev.workspace_pool_ = tree->nodes_[base_id].pool;
        auto mi = ev.string_heap_.size();
        ev.string_heap_.push_back(merged);
        if (auto set_fn = ev.primitives_.lookup("set-code")) {
            auto r = (*set_fn)({make_string(mi)});
            return is_bool(r) ? r : make_bool(false);
        }
        return make_bool(false);
    });

    // Issue #295 Phase 0: (ws:try-mutation expr-string)
    // — run expr-string in a sandboxed workspace. On success,
    // returns (result . snapshot-id). On failure (exception
    // or error-value), the workspace is automatically rolled
    // back and the primitive returns #f.
    //
    // Building block for the Aura-HV self-evolving verification
    // loop. Agents can propose a mutation, run it via
    // ws:try-mutation, and either commit (keep the snapshot)
    // or rollback. See docs/design/hardware/aura_hv.md.
    add("ws:try-mutation", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        // Snapshot the current workspace
        if (!ev.primitives_.lookup("ast:snapshot"))
            return make_bool(false);
        auto snap_id_opt = (*ev.primitives_.lookup("ast:snapshot"))({});
        if (!is_int(snap_id_opt))
            return make_bool(false);
        auto snap_id = as_int(snap_id_opt);
        auto eval_fn = ev.primitives_.lookup("eval");
        if (!eval_fn)
            return make_bool(false);
        // Run the expression (parse + eval_flat via the eval primitive)
        EvalValue result = make_void();
        try {
            result = (*eval_fn)({make_string(idx)});
        } catch (...) {
            result = make_void();
        }
        if (types::is_error(result) || types::is_void(result)) {
            if (auto restore_fn = ev.primitives_.lookup("ast:restore")) {
                (*restore_fn)({make_int(snap_id)});
            }
            return make_bool(false);
        }
        // Success: return (result . snap-id)
        auto pid = ev.pairs_.size();
        ev.pairs_.push_back({result, snap_id_opt});
        return make_pair(pid);
    });

} // register_workspace_primitives

} // namespace aura::compiler::primitives_detail
