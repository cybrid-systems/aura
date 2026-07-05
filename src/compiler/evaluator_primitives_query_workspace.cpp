// evaluator_primitives_query_workspace.cpp — P0 step 9: workspace AST query primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.type;
import aura.core.mutation;
import aura.compiler.value;
import aura.compiler.matcher;
import aura.parser.parser;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;
using MakeErrorVal = std::function<EvalValue(const std::string&, const std::string&)>;

using namespace types;

struct WorkspaceQueryState {
    std::shared_mutex& workspace_mtx;
    aura::ast::FlatAST*& workspace_flat;
    aura::ast::StringPool*& workspace_pool;
    void*& type_registry;
    std::vector<std::string>& keyword_table;
    std::pmr::vector<Pair>& pairs;
    std::pmr::vector<std::string>& string_heap;
    aura::ast::ASTArena*& temp_arena;
    std::unordered_map<std::uint64_t, std::vector<aura::ast::NodeId>>& tag_arity_index;
    // Issue #371: pair with tag_arity_index_mtx_ on Evaluator.
    // query:pattern's fast path acquires a shared_lock around
    // the `find` + bucket iteration; build / sync / invalidate
    // take unique_lock internally (paired).
    std::shared_mutex& tag_arity_index_mtx;
    std::function<aura::ast::StringPool*()> canonical_pool;
    std::function<void()> build_tag_arity_index;
};

void register_workspace_query_primitives(
    PrimRegistrar add, std::shared_mutex& workspace_mtx, aura::ast::FlatAST*& workspace_flat,
    aura::ast::StringPool*& workspace_pool, void*& type_registry,
    std::vector<std::string>& keyword_table, std::pmr::vector<Pair>& pairs,
    std::pmr::vector<std::string>& string_heap, aura::ast::ASTArena*& temp_arena,
    std::unordered_map<std::uint64_t, std::vector<aura::ast::NodeId>>& tag_arity_index,
    // Issue #371: shared_mutex around tag_arity_index. See
    // WorkspaceQueryState::tag_arity_index_mtx above.
    std::shared_mutex& tag_arity_index_mtx, std::function<aura::ast::StringPool*()> canonical_pool,
    std::function<void()> build_tag_arity_index, MakeErrorVal mev, Evaluator& ev) {
    WorkspaceQueryState ws{workspace_mtx,       workspace_flat, workspace_pool,
                           type_registry,       keyword_table,  pairs,
                           string_heap,         temp_arena,     tag_arity_index,
                           tag_arity_index_mtx, canonical_pool, build_tag_arity_index};

    // (query:find name) — Find all node IDs with matching symbol name
    add("query:find", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (query:find name)");
        auto idx = as_string_idx(a[0]);
        if (idx >= ws.string_heap.size())
            return mev("bad-arg", "name string index out of range");
        if (!ws.workspace_flat || !ws.workspace_pool)
            return mev("no-workspace", "no workspace AST loaded");
        auto& flat = *ws.workspace_flat;
        auto name = ws.string_heap[idx];
        // Phase 2.5.0: route via ws.canonical_pool() (== workspace_pool, explicit).
        auto sym = ws.canonical_pool()->intern(name);
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.sym_id == sym) {
                auto pid = ws.pairs.size();
                ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                result = make_pair(pid);
            }
        }
        return result;
    });

    // (query:children node-id) — Get children node IDs
    add("query:children", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]) || !ws.workspace_flat)
            return mev("bad-arg", "usage: (query:children node-id)");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (node >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(node) + " >= flat size " +
                                           std::to_string(flat.size()));
        auto v = flat.get(node);
        EvalValue result = make_void();
        for (std::size_t i = v.children.size(); i > 0; --i) {
            auto pid = ws.pairs.size();
            ws.pairs.push_back({make_int(static_cast<std::int64_t>(v.child(i - 1))), result});
            result = make_pair(pid);
        }
        return result;
    });

    // Issue #249: (query:children-stable node-id) — Get children
    // as a list of (node-id . generation) stable-ref pairs. Use
    // this instead of (query:children ...) when the result is
    // stored in a variable that may be used after a mutate call.
    // The captured generation lets validate the ref later (via
    // (mutate:check-stable-ref) or pass it back to a mutate
    // primitive that supports stable-ref inputs.
    add("query:children-stable", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]) || !ws.workspace_flat)
            return mev("bad-arg", "usage: (query:children-stable node-id)");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (node >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(node) + " >= flat size " +
                                           std::to_string(flat.size()));
        // Issue #398: truly zero-allocation path (no local
        // vector). The result is a list of (id . gen) pairs
        // — each child takes 3 entries in ws.pairs:
        //   - (gen, nil)
        //   - (id, ^prev)
        //   - (pair_ev, ^next-list-node)
        // We pre-allocate 3*N entries directly in ws.pairs
        // (no temp vector), fill them via the callback, then
        // thread the list-node cdrs in a second O(N) walk.
        auto gen = flat.generation();
        std::size_t n = flat.stable_child_count(node);
        if (n == 0)
            return make_void();
        const auto base = ws.pairs.size();
        // Pre-allocate 3*N slots with placeholder pairs. The
        // exact car / cdr values are filled in below; the
        // placeholders keep the indices stable.
        for (std::size_t i = 0; i < 3 * n; ++i) {
            ws.pairs.push_back({make_void(), make_void()});
        }
        // Fill (gen, nil) and (id, ^gen-pair) for each child.
        // The list-node cdr is filled in the second loop.
        std::size_t i = 0;
        flat.for_each_stable_child(node, [&](aura::ast::FlatAST::StableNodeRef ref) {
            const auto gen_idx = static_cast<int>(base + 3 * i);
            const auto pair_idx = static_cast<int>(base + 3 * i + 1);
            const auto list_idx = static_cast<int>(base + 3 * i + 2);
            ws.pairs[gen_idx] = {make_int(static_cast<std::int64_t>(gen)), make_void()};
            ws.pairs[pair_idx] = {make_int(static_cast<std::int64_t>(ref.id)), make_pair(gen_idx)};
            // The list-node cdr is filled below (we don't know
            // the next list-node index until the loop ends).
            ws.pairs[list_idx] = {make_pair(pair_idx), make_void()};
            ++i;
        });
        // Thread the list-node cdrs: list[i].cdr = list[i+1]
        // for i in 0..N-2; list[N-1].cdr = nil (already set).
        for (std::size_t j = 0; j + 1 < n; ++j) {
            const auto list_idx = static_cast<int>(base + 3 * j + 2);
            const auto next_idx = static_cast<int>(base + 3 * (j + 1) + 2);
            ws.pairs[list_idx].cdr = make_pair(next_idx);
        }
        // The final result is the first list-node.
        return make_pair(static_cast<int>(base + 2));
    });

    // Issue #249: (query:parent-stable node-id) — Get the
    // parent as a (node-id . generation) stable-ref pair. Returns
    // an empty list if the node has no parent.
    add("query:parent-stable", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]) || !ws.workspace_flat)
            return mev("bad-arg", "usage: (query:parent-stable node-id)");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (node >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(node) + " >= flat size " +
                                           std::to_string(flat.size()));
        auto pref = flat.parent_stable(node);
        if (pref.id == aura::ast::NULL_NODE)
            return make_void();
        auto gen = flat.generation();
        // Build (parent-id . gen) pair
        auto gen_pid = ws.pairs.size();
        ws.pairs.push_back({make_int(static_cast<std::int64_t>(gen)), make_void()});
        auto pair_pid = ws.pairs.size();
        ws.pairs.push_back({make_int(static_cast<std::int64_t>(pref.id)), make_pair(gen_pid)});
        return make_pair(pair_pid);
    });

    // (query:root) — Return the current workspace root node ID, or #f if no workspace
    add("query:root", [ws, mev](const auto&) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        if (ws.workspace_flat->root == aura::ast::NULL_NODE)
            return mev("no-root", "workspace AST has no root node");
        return make_int(static_cast<std::int64_t>(ws.workspace_flat->root));
    });


    // (query:node node-id) — Get node details as list (tag value type sym-id)
    add("query:node", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (query:node node-id)");
        if (!ws.workspace_flat || !ws.workspace_pool)
            return mev("no-workspace", "no workspace AST loaded");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (node >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(node) + " >= flat size " +
                                           std::to_string(flat.size()));
        auto v = flat.get(node);

        // Build result: list of (tag-id value sym-name children-count)
        EvalValue result = make_void();

        // children-count
        auto pid = ws.pairs.size();
        ws.pairs.push_back({make_int(static_cast<std::int64_t>(v.children.size())), result});
        result = make_pair(pid);

        // value (int or string content for string literals)
        if (v.has_int() && v.tag != aura::ast::NodeTag::LiteralString) {
            pid = ws.pairs.size();
            ws.pairs.push_back({make_int(v.int_value), result});
            result = make_pair(pid);
        } else if (v.sym_id != aura::ast::INVALID_SYM) {
            auto sym_name = std::string(ws.workspace_pool->resolve(v.sym_id));
            auto sid = ws.string_heap.size();
            ws.string_heap.push_back(sym_name);
            pid = ws.pairs.size();
            ws.pairs.push_back({make_string(sid), result});
            result = make_pair(pid);
        }

        // tag-id (integer tag == NodeTag enum value)
        pid = ws.pairs.size();
        ws.pairs.push_back({make_int(static_cast<std::int64_t>(v.tag)), result});
        result = make_pair(pid);

        return result;
    });

    // Issue #235: (query:stable-ref node-id) — Returns a
    // stable reference to a node as (node-id . current-gen).
    // The stable-ref can be passed back to mutate:* primitives
    // which will verify the generation matches before applying
    // the mutation. If the generation has changed (i.e., a
    // structural mutation has happened since the ref was
    // captured), the primitive fails with a "stale-ref" error
    // instead of silently operating on a wrong node.
    //
    // The result is a 2-element list `(id . gen)` so the agent
    // can capture it as a single value and pass it through
    // multi-round edit pipelines.
    add("query:stable-ref", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (query:stable-ref node-id)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (node >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(node) + " >= flat size " +
                                           std::to_string(flat.size()));
        auto gen = flat.generation();
        // Build (node-id . gen) pair
        auto gen_pid = ws.pairs.size();
        ws.pairs.push_back({make_int(static_cast<std::int64_t>(gen)), make_void()});
        auto pair_pid = ws.pairs.size();
        ws.pairs.push_back({make_int(static_cast<std::int64_t>(node)), make_pair(gen_pid)});
        return make_pair(pair_pid);
    });

    // Issue #347 follow-up #1 / #393: (query:ref-valid? stable-ref)
    // — Verify a stable-ref (the (id . gen) pair shape returned by
    // (query:stable-ref) / (query:children-stable) /
    // (query:parent-stable)) is still valid in the current
    // workspace. Returns #t iff the slot at `id` is in-bounds AND
    // its stored generation matches `gen` AND the wrap_epoch
    // matches (Issue #368 second-wrap protection).
    //
    // Uses FlatAST::is_valid_id_gen() — the flat-style check from
    // #393 — rather than the strict is_valid(ref) used by the
    // older (ast:ref-valid? id gen) from #191. The flat-style
    // check ONLY consults the slot's stored generation, so it is
    // NOT invalidated by unrelated subtree bumps (use
    // (query:ref-valid?* strict) for the strict global-gen check).
    // This makes it the right primitive for scoped-invalidated
    // workspaces (EDA, AI agent multi-subtree loops).
    //
    // Companion note: (ast:ref-valid? id gen) from #191 still
    // works and uses the strict is_valid() check; it's the right
    // primitive for "has the global state changed since capture?"
    // but produces false positives in scoped-invalidated
    // workspaces.
    add("query:ref-valid?", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_pair(a[0]))
            return mev("bad-arg", "usage: (query:ref-valid? (id . gen))");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        auto& flat = *ws.workspace_flat;
        // Unpack the (id . gen) pair. Same shape as
        // (query:children-stable)'s return value.
        auto& outer = ws.pairs[as_pair_idx(a[0])];
        if (!is_int(outer.car))
            return mev("bad-arg", "stable-ref car must be a node id (int)");
        auto id = static_cast<aura::ast::NodeId>(as_int(outer.car));
        if (!is_pair(outer.cdr))
            return mev("bad-arg", "stable-ref cdr must be a pair (gen . nil)");
        auto& inner = ws.pairs[as_pair_idx(outer.cdr)];
        if (!is_int(inner.car))
            return mev("bad-arg", "stable-ref gen must be an int");
        auto gen = static_cast<std::uint16_t>(as_int(inner.car));
        // Use the flat-style #393 helper: slot check only,
        // respects scoped invalidation via wrap_epoch.
        return make_bool(flat.is_valid_id_gen(id, gen));
    });

    // (query:calls name) — Find all call sites of a named function
    add("query:calls", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        // Issue #278 follow-up: 0-arg call returns ALL
        // call sites in the workspace (regardless of
        // callee). 1-arg call (legacy) still filters by
        // the named function — preserves pre-#278 caller
        // contracts.
        if (a.size() > 1 || (a.size() == 1 && !is_string(a[0])))
            return mev("bad-arg", "usage: (query:calls) or (query:calls name)");
        if (!ws.workspace_flat || !ws.workspace_pool)
            return mev("no-workspace", "no workspace AST loaded");
        aura::ast::SymId sym = aura::ast::INVALID_SYM;
        if (a.size() == 1) {
            auto idx = as_string_idx(a[0]);
            if (idx >= ws.string_heap.size())
                return mev("bad-arg", "name string index out of range");
            auto& flat = *ws.workspace_flat;
            (void)flat;
            auto name = ws.string_heap[idx];
            // Phase 2.5.0: route via ws.canonical_pool() (== workspace_pool, explicit).
            sym = ws.canonical_pool()->intern(name);
        }
        auto& flat = *ws.workspace_flat;
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag != aura::ast::NodeTag::Call || v.children.empty())
                continue;
            if (sym != aura::ast::INVALID_SYM) {
                auto callee = flat.get(v.child(0));
                if (callee.tag != aura::ast::NodeTag::Variable || callee.sym_id != sym)
                    continue;
            }
            auto pid = ws.pairs.size();
            ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
            result = make_pair(pid);
        }
        return result;
    });

    // Issue #278: (query:defines) — return all define
    // sites in the workspace. 0-arg returns every
    // (define ...) node regardless of name. 1-arg
    // filters by name (preserves the (query:calls name)
    // symmetry for AI agent workflows).
    add("query:defines", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.size() > 1 || (a.size() == 1 && !is_string(a[0])))
            return mev("bad-arg", "usage: (query:defines) or (query:defines name)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        aura::ast::SymId sym = aura::ast::INVALID_SYM;
        if (a.size() == 1 && ws.workspace_pool) {
            auto idx = as_string_idx(a[0]);
            if (idx >= ws.string_heap.size())
                return mev("bad-arg", "name string index out of range");
            sym = ws.canonical_pool()->intern(ws.string_heap[idx]);
        }
        auto& flat = *ws.workspace_flat;
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag != aura::ast::NodeTag::Define)
                continue;
            if (sym != aura::ast::INVALID_SYM && v.sym_id != sym)
                continue;
            auto pid = ws.pairs.size();
            ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
            result = make_pair(pid);
        }
        return result;
    });

    // ═══════════════════════════════════════════════════════════════
    // P1: Query/Transform EDSL 扩展
    // ═══════════════════════════════════════════════════════════════

    // (query:parent node-id) — Find parent node IDs (nodes whose children include this ID)
    add("query:parent", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (query:parent node-id)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        auto target = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (target >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(target) + " >= flat size " +
                                           std::to_string(flat.size()));
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                if (v.child(ci) == target) {
                    auto pid = ws.pairs.size();
                    ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                    result = make_pair(pid);
                    break;
                }
            }
        }
        return result;
    });

    // (query:siblings node-id) — Find sibling node IDs (other children of same parent)
    add("query:siblings", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (query:siblings node-id)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        auto target = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (target >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(target) + " >= flat size " +
                                           std::to_string(flat.size()));
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            bool parent_of_target = false;
            for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                if (v.child(ci) == target) {
                    parent_of_target = true;
                    break;
                }
            }
            if (parent_of_target) {
                for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                    auto child = v.child(ci);
                    if (child != target) {
                        auto pid = ws.pairs.size();
                        ws.pairs.push_back({make_int(static_cast<std::int64_t>(child)), result});
                        result = make_pair(pid);
                    }
                }
            }
        }
        return result;
    });

    // ═══════════════════════════════════════════════════════════════
    // P8a: Query/Transform EDSL — combined filter/where (P1)
    // ═══════════════════════════════════════════════════════════════

    // (where :field value) — Create a predicate for query:filter.
    // Supported fields:
    //   :node-type  — match NodeTag name (e.g. 'Call, 'Define, 'LiteralInt)
    //   :callee     — for Call nodes, match callee Variable name
    //   :has-param  — node has a parameter with given name
    //   :defined-by — node is a Define with given name
    //   :tag        — alias for :node-type
    //   :has-child  — node has at least one child with the given NodeTag name
    //   :depth      — node is at the given depth from root (e.g. "0" = root)
    //   :marker     — Issue #244: match SyntaxMarker name
    //   :syntax-marker — Issue #267: alias for :marker
    //                 ("User" / "MacroIntroduced" / "BoolLiteral").
    //                 Useful for EDSL queries that want to operate
    //                 only on macro-introduced code, or only on
    //                 user-written code, after hygienic macro
    //                 expansion.
    //
    // Returns a predicate descriptor (a tagged pair) that query:filter
    // applies to each candidate node.
    add("query:where", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.size() < 2 || !is_keyword(a[0]) || !is_string(a[1]))
            return mev("bad-arg", "usage: (where :field-name value-string)");
        if (!ws.workspace_flat || !ws.workspace_pool)
            return mev("no-workspace", "no workspace AST loaded");
        auto field_idx = as_keyword_idx(a[0]);
        if (field_idx >= ws.keyword_table.size())
            return mev("bad-arg", "unknown keyword");
        auto field_name = ws.keyword_table[field_idx];
        auto val_idx = as_string_idx(a[1]);
        if (val_idx >= ws.string_heap.size())
            return mev("bad-arg", "value string index out of range");
        auto value = ws.string_heap[val_idx];
        auto& flat = *ws.workspace_flat;
        auto& pool = *ws.workspace_pool;

        // Store the predicate as a pair: (field-name value-sym)
        auto field_keyword_idx = ws.keyword_table.size();
        ws.keyword_table.push_back(field_name);
        auto val_sym = pool.intern(value);
        auto val_string_idx = ws.string_heap.size();
        ws.string_heap.push_back(value);

        // Encode as (key:pair key:pair) where car=field keyword, cdr=value string ref
        // This tagged structure is opaque to users but query:filter knows how to apply it.
        auto val_pair = ws.pairs.size();
        ws.pairs.push_back({make_keyword(field_keyword_idx), make_string(val_string_idx)});
        return make_pair(val_pair);
    });

    // (query:filter predicate ...) — Filter workspace nodes matching ALL predicates.
    // Each predicate is created by (where :field value).
    // Returns a list of matching node IDs.
    //
    // Usage:
    //   (query:filter (where :node-type "Call") (where :callee "sort"))
    //   → all Call nodes where the callee is "sort"
    //
    //   (query:filter (where :defined-by "fib") (where :node-type "Lambda"))
    //   → the body Lambda of (define fib ...)
    add("query:filter", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty())
            return mev("bad-arg", "usage: (query:filter predicate ...)");
        if (!ws.workspace_flat || !ws.workspace_pool)
            return mev("no-workspace", "no workspace AST loaded");
        auto& flat = *ws.workspace_flat;
        auto& pool = *ws.workspace_pool;

        // Issue #425: top-level :hygiene / :skip-macro-introduced
        // hygiene gate. When enabled (default #f → opt-in to keep
        // existing behavior; opt-in #t skips MacroIntroduced nodes
        // from the result set BEFORE predicate evaluation), the
        // filter silently drops any node whose marker is
        // SyntaxMarker::MacroIntroduced. This is the natural
        // EDSL surface for the "AI mutate shouldn't touch macro
        // expansion" use case from the issue body — the agent
        // calls (query:filter :hygiene #t ...) and gets back
        // only user-written nodes, no need to remember the
        // (:marker "User") predicate idiom.
        //
        // The :hygiene keyword takes a boolean (or int-as-bool)
        // value. The :skip-macro-introduced keyword is a
        // discoverable alias — same semantics, more explicit
        // name for readers unfamiliar with the hygiene terminology.
        //
        // The gate runs BEFORE the predicate loop, so any (where ...)
        // predicates still apply on top of the hygiene filter.
        bool hygiene_skip_macro = false;
        // Issue #425: track which arg indices are consumed by
        // top-level keyword parsing so the predicate parser can
        // skip them. Without this, the predicate loop would
        // re-encounter :hygiene / #f and reject them as
        // "malformed predicate" (not a (where ...) pair).
        std::vector<bool> arg_consumed(a.size(), false);
        for (std::size_t ai = 0; ai < a.size(); ++ai) {
            if (is_keyword(a[ai])) {
                auto kidx = as_keyword_idx(a[ai]);
                if (kidx >= ws.keyword_table.size())
                    return mev("bad-arg", "unknown keyword");
                auto kw = ws.keyword_table[kidx];
                if (kw == ":hygiene" || kw == ":skip-macro-introduced") {
                    bool v = true;
                    arg_consumed[ai] = true;
                    if (ai + 1 < a.size() && (is_bool(a[ai + 1]) || is_int(a[ai + 1]))) {
                        if (is_bool(a[ai + 1]))
                            v = as_bool(a[ai + 1]);
                        else
                            v = (as_int(a[ai + 1]) != 0);
                        arg_consumed[ai + 1] = true;
                        ++ai;
                    }
                    hygiene_skip_macro = v;
                } else {
                    // Re-emit a bad-arg for unknown top-level
                    // keyword (we already validated that each
                    // non-keyword arg is a (where ...) pair
                    // below). The predicate parser would have
                    // already produced a "malformed predicate"
                    // for a stray keyword at this point, but we
                    // surface it earlier with a clearer message.
                    return mev("bad-arg", std::string("unknown top-level keyword: ") + kw);
                }
            }
        }

        // Collect predicates from arguments (each is a (where ...) pair)
        struct Predicate {
            std::string field;
            std::string value;
        };
        std::vector<Predicate> predicates;

        for (std::size_t ai = 0; ai < a.size(); ++ai) {
            // Issue #425: skip args consumed by the top-level
            // keyword parser (e.g. :hygiene + its bool value).
            if (arg_consumed[ai])
                continue;
            if (!is_pair(a[ai]))
                return mev("bad-arg", "each predicate must be a (where ...) pair");
            auto pair_idx = as_pair_idx(a[ai]);
            auto car = ws.pairs[pair_idx].car;
            auto cdr = ws.pairs[pair_idx].cdr;
            if (!is_keyword(car) || !is_string(cdr))
                return mev("bad-arg", "malformed predicate");
            auto kidx = as_keyword_idx(car);
            auto sidx = as_string_idx(cdr);
            if (kidx >= ws.keyword_table.size() || sidx >= ws.string_heap.size())
                return mev("bad-arg", "predicate field/value out of range");
            predicates.push_back({ws.keyword_table[kidx], ws.string_heap[sidx]});
        }

        if (predicates.empty() && !hygiene_skip_macro)
            return mev("bad-arg", "at least one predicate required");

        // Iterate all workspace nodes, applying all predicates
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            // Issue #425: hygiene gate. Drop MacroIntroduced nodes
            // BEFORE predicate evaluation so the predicate list
            // doesn't have to repeat the (:marker "User")
            // idiom on every query. The `flat.marker(id)` call
            // is O(1) (one vector lookup) and the early-continue
            // is cheaper than a chain of 5+ where clauses for
            // the common EDSL self-evolution query patterns.
            if (hygiene_skip_macro && flat.marker(id) == aura::ast::SyntaxMarker::MacroIntroduced)
                continue;
            auto v = flat.get(id);
            bool match = true;

            for (auto& p : predicates) {
                if (p.field == ":node-type" || p.field == ":tag") {
                    // Match NodeTag name
                    bool found = false;
                    for (auto& m : aura::ast::kNodeMeta) {
                        if (m.name == p.value && m.name != "<gap>") {
                            if (v.tag == m.tag)
                                found = true;
                            break;
                        }
                    }
                    if (!found) {
                        match = false;
                        break;
                    }
                } else if (p.field == ":callee") {
                    // For Call nodes, match callee Variable name
                    if (v.tag == aura::ast::NodeTag::Call && !v.children.empty()) {
                        auto callee = flat.get(v.child(0));
                        if (callee.tag != aura::ast::NodeTag::Variable ||
                            pool.resolve(callee.sym_id) != p.value) {
                            match = false;
                            break;
                        }
                    } else {
                        match = false;
                        break;
                    }
                } else if (p.field == ":defined-by" || p.field == ":defines") {
                    // Match Define nodes by name
                    if (v.tag == aura::ast::NodeTag::Define) {
                        auto name = pool.resolve(v.sym_id);
                        if (name != p.value) {
                            match = false;
                            break;
                        }
                    } else {
                        match = false;
                        break;
                    }
                } else if (p.field == ":has-param") {
                    // Check if node has a parameter with the given name
                    bool found_param = false;
                    for (auto pid : v.params) {
                        if (pool.resolve(pid) == p.value) {
                            found_param = true;
                            break;
                        }
                    }
                    if (!found_param) {
                        match = false;
                        break;
                    }
                } else if (p.field == ":has-child") {
                    // Check if node has at least one child with the given NodeTag name
                    aura::ast::NodeTag child_tag = static_cast<aura::ast::NodeTag>(-1);
                    bool found_tag = false;
                    for (auto& m : aura::ast::kNodeMeta) {
                        if (m.name == p.value && m.name != "<gap>") {
                            child_tag = m.tag;
                            found_tag = true;
                            break;
                        }
                    }
                    if (!found_tag) {
                        match = false;
                        break;
                    }
                    bool has_child = false;
                    for (auto cid : v.children) {
                        if (cid != aura::ast::NULL_NODE && flat.get(cid).tag == child_tag) {
                            has_child = true;
                            break;
                        }
                    }
                    if (!has_child) {
                        match = false;
                        break;
                    }
                } else if (p.field == ":depth") {
                    // Check if node is at the given depth from root
                    int target_depth = 0;
                    try {
                        target_depth = std::stoi(p.value);
                    } catch (...) {
                        // [SILENCE-PRIM-#615] Non-numeric :depth
                        // predicate silently de-selects the node rather
                        // than raising — documented parse-tolerant
                        // filter behavior across all ws predicates.
                        match = false;
                        break;
                    }
                    if (target_depth < 0) {
                        match = false;
                        break;
                    }
                    // Starting from this node, walk up via children_of to count depth
                    int actual_depth = 0;
                    aura::ast::NodeId cur = id;
                    while (cur != 0) { // root is always NodeId 0
                        // Find parent by scanning all nodes for one that has cur as child
                        aura::ast::NodeId parent = aura::ast::NULL_NODE;
                        for (aura::ast::NodeId pid = 0; pid < flat.size(); ++pid) {
                            auto pv = flat.get(pid);
                            for (auto cid : pv.children) {
                                if (cid == cur) {
                                    parent = pid;
                                    break;
                                }
                            }
                            if (parent != aura::ast::NULL_NODE)
                                break;
                        }
                        if (parent == aura::ast::NULL_NODE)
                            break;
                        cur = parent;
                        ++actual_depth;
                    }
                    if (actual_depth != target_depth) {
                        match = false;
                        break;
                    }
                } else if (p.field == ":marker" || p.field == ":syntax-marker") {
                    // Issue #244 / #267: match SyntaxMarker by name.
                    // The marker column is populated by clone_macro_body
                    // (Issue #190) and persists in ws.workspace_flat across
                    // mutations. Marker names (case-sensitive):
                    //   "User"           — code the user wrote directly
                    //   "MacroIntroduced" — code inserted by a hygienic macro
                    //   "BoolLiteral"    — auto-generated #t / #f nodes
                    auto m = flat.marker(id);
                    const char* mname = nullptr;
                    switch (m) {
                        case aura::ast::SyntaxMarker::User:
                            mname = "User";
                            break;
                        case aura::ast::SyntaxMarker::MacroIntroduced:
                            mname = "MacroIntroduced";
                            break;
                        case aura::ast::SyntaxMarker::BoolLiteral:
                            mname = "BoolLiteral";
                            break;
                    }
                    if (!mname || p.value != mname) {
                        match = false;
                        break;
                    }
                } else {
                    return mev("unknown-field",
                               std::string("unknown where field: \"") + p.field + "\"");
                }
            }

            if (match) {
                auto pid = ws.pairs.size();
                ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                result = make_pair(pid);
            }
        }
        return result;
    });

    // (query:node-type tag-name) — Find all nodes with a given NodeTag name
    // Tag names: LiteralInt, Variable, Call, IfExpr, Lambda, Let, LetRec,
    //            Define, Begin, Set, Quote, LiteralString, TypeAnnotation,
    //            Coercion, LiteralFloat, MacroDef
    add("query:node-type", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (query:node-type tag-name)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        auto idx = as_string_idx(a[0]);
        if (idx >= ws.string_heap.size())
            return mev("bad-arg", "tag name string index out of range");
        auto target_name = ws.string_heap[idx];
        auto& flat = *ws.workspace_flat;

        // Convert tag name to NodeTag enum
        aura::ast::NodeTag target_tag = static_cast<aura::ast::NodeTag>(-1);
        bool found_tag = false;
        for (auto& m : aura::ast::kNodeMeta) {
            if (m.name == target_name && m.name != "<gap>") {
                target_tag = m.tag;
                found_tag = true;
                break;
            }
        }
        if (!found_tag)
            return mev("unknown-tag", std::string("unknown node type \"") + target_name + "\"");

        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            if (flat.get(id).tag == target_tag) {
                auto pid = ws.pairs.size();
                ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                result = make_pair(pid);
            }
        }
        return result;
    });

    // Issue #278: (query:node-marker node-id) — return
    // the SyntaxMarker name for a node as a string.
    // Returns "User" / "MacroIntroduced" / "BoolLiteral"
    // (the canonical Aura-level names). Returns empty
    // string on out-of-range or unknown marker.
    add("query:node-marker", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (query:node-marker node-id)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (id >= flat.size())
            return mev("out-of-range", "node ID >= flat size");
        auto marker = flat.marker(id);
        const char* name = "User";
        switch (marker) {
            case aura::ast::SyntaxMarker::User:
                name = "User";
                break;
            case aura::ast::SyntaxMarker::MacroIntroduced:
                name = "MacroIntroduced";
                break;
            case aura::ast::SyntaxMarker::BoolLiteral:
                name = "BoolLiteral";
                break;
        }
        auto idx = ws.string_heap.size();
        ws.string_heap.push_back(name);
        return make_string(idx);
    });

    // Issue #454: (query:reflect-node-members node-id) — reflection
    // bridge for FlatAST/SyntaxMarker introspection. Returns an alist
    // of (field-name . value) pairs describing the node's SoA fields
    // without hard-coded EDSL tag switches:
    //   tag-name, tag-id, marker, type-id, dirty, children-count
    // plus tag-specific members (sym for Define/Let, int-value for
    // LiteralInt, etc.).
    add("query:reflect-node-members", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (query:reflect-node-members node-id)");
        if (!ws.workspace_flat || !ws.workspace_pool)
            return mev("no-workspace", "no workspace AST loaded");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (node >= flat.size())
            return mev("out-of-range", "node ID >= flat size");
        auto v = flat.get(node);

        auto marker_name = "User";
        switch (flat.marker(node)) {
            case aura::ast::SyntaxMarker::User:
                marker_name = "User";
                break;
            case aura::ast::SyntaxMarker::MacroIntroduced:
                marker_name = "MacroIntroduced";
                break;
            case aura::ast::SyntaxMarker::BoolLiteral:
                marker_name = "BoolLiteral";
                break;
        }

        EvalValue result = make_void();
        auto append_field = [&](const std::string& fname, EvalValue val) {
            auto nidx = ws.string_heap.size();
            ws.string_heap.push_back(fname);
            auto entry = ws.pairs.size();
            ws.pairs.push_back({make_string(nidx), val});
            auto cons = ws.pairs.size();
            ws.pairs.push_back({make_pair(entry), result});
            result = make_pair(cons);
        };

        append_field("tag-name", [&]() {
            auto tidx = ws.string_heap.size();
            ws.string_heap.push_back(std::string(aura::ast::meta(v.tag).name));
            return make_string(tidx);
        }());
        append_field("tag-id", make_int(static_cast<std::int64_t>(v.tag)));
        append_field("marker", [&]() {
            auto midx = ws.string_heap.size();
            ws.string_heap.push_back(marker_name);
            return make_string(midx);
        }());
        append_field("type-id", make_int(static_cast<std::int64_t>(flat.type_id(node))));
        append_field("dirty", make_int(static_cast<std::int64_t>(flat.dirty(node))));
        append_field("children-count", make_int(static_cast<std::int64_t>(v.children.size())));

        if (v.has_name()) {
            auto sym = std::string(ws.workspace_pool->resolve(v.sym_id));
            auto sidx = ws.string_heap.size();
            ws.string_heap.push_back(sym);
            append_field("sym", make_string(sidx));
        }
        if (v.has_int() && v.tag == aura::ast::NodeTag::LiteralInt) {
            append_field("int-value", make_int(v.int_value));
        }
        if (v.tag == aura::ast::NodeTag::Define && !v.children.empty()) {
            append_field("body-node", make_int(static_cast<std::int64_t>(v.child(0))));
        }
        if ((v.tag == aura::ast::NodeTag::Let || v.tag == aura::ast::NodeTag::LetRec) &&
            v.children.size() >= 2) {
            append_field("init-node", make_int(static_cast<std::int64_t>(v.child(0))));
            append_field("body-node", make_int(static_cast<std::int64_t>(v.child(1))));
        }

        return result;
    });

    // Issue #278: (query:ref-counts node-id) — return the
    // number of AST nodes whose children include this node-id
    // (i.e. the number of direct parents in the AST). 0 if
    // the node has no references (typical for a root-like
    // node that's never embedded elsewhere). Used by AI agents
    // to gauge how "central" a node is before mutating it
    // (high ref-counts → wider invalidation potential).
    add("query:ref-counts", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (query:ref-counts node-id)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        auto target = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (target >= flat.size())
            return make_int(0);
        std::int64_t count = 0;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                if (v.child(ci) == target) {
                    ++count;
                    break; // each id counts at most once
                }
            }
        }
        return make_int(count);
    });

    // Issue #278: (query:defines-by-marker marker-name) —
    // return all Define nodes whose SyntaxMarker matches
    // the given name. Same marker vocabulary as
    // (query:by-marker): "User" / "MacroIntroduced" /
    // "BoolLiteral". Composes the (query:defines) +
    // (query:by-marker) data without a stdlib wrapper
    // (the Aura-level (define ...) for colon-prefixed
    // names hits a flat-evaluator closure issue, so the
    // C++ engine primitive is the canonical implementation).
    add("query:defines-by-marker", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (query:defines-by-marker marker-name)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        auto idx = as_string_idx(a[0]);
        if (idx >= ws.string_heap.size())
            return mev("bad-arg", "marker name string index out of range");
        const auto& marker_name = ws.string_heap[idx];
        aura::ast::SyntaxMarker target;
        if (marker_name == "User") {
            target = aura::ast::SyntaxMarker::User;
        } else if (marker_name == "MacroIntroduced") {
            target = aura::ast::SyntaxMarker::MacroIntroduced;
        } else if (marker_name == "BoolLiteral") {
            target = aura::ast::SyntaxMarker::BoolLiteral;
        } else {
            return mev("unknown-marker", std::string("unknown marker: \"") + marker_name +
                                             "\" (expected User / MacroIntroduced / BoolLiteral)");
        }
        auto& flat = *ws.workspace_flat;
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.marker == target) {
                auto pid = ws.pairs.size();
                ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                result = make_pair(pid);
            }
        }
        return result;
    });

    // Issue #278: (query:calls-by-marker marker-name) —
    // return all Call nodes whose SyntaxMarker matches
    // the given name. C++ engine primitive (same
    // rationale as query:defines-by-marker).
    add("query:calls-by-marker", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (query:calls-by-marker marker-name)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        auto idx = as_string_idx(a[0]);
        if (idx >= ws.string_heap.size())
            return mev("bad-arg", "marker name string index out of range");
        const auto& marker_name = ws.string_heap[idx];
        aura::ast::SyntaxMarker target;
        if (marker_name == "User") {
            target = aura::ast::SyntaxMarker::User;
        } else if (marker_name == "MacroIntroduced") {
            target = aura::ast::SyntaxMarker::MacroIntroduced;
        } else if (marker_name == "BoolLiteral") {
            target = aura::ast::SyntaxMarker::BoolLiteral;
        } else {
            return mev("unknown-marker", std::string("unknown marker: \"") + marker_name +
                                             "\" (expected User / MacroIntroduced / BoolLiteral)");
        }
        auto& flat = *ws.workspace_flat;
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::Call && v.marker == target) {
                auto pid = ws.pairs.size();
                ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                result = make_pair(pid);
            }
        }
        return result;
    });

    // (query:by-marker marker-name) — Issue #244: general marker
    // query. Returns all nodes with the given SyntaxMarker.
    // marker-name is a string: "User" / "MacroIntroduced" /
    // "BoolLiteral". The opposite of (query:macro-introduced)
    // (which hard-codes MacroIntroduced); this primitive lets
    // callers query by any marker.
    //
    // Returns a list of NodeIds (same encoding as
    // query:node-type / query:filter).
    //
    // Optional 2nd arg: integer limit N. Only the first N
    // matches are returned. Default: no limit.
    add("query:by-marker", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || a.size() > 2 || !is_string(a[0]))
            return mev(
                "bad-arg",
                "usage: (query:by-marker marker-name) or (query:by-marker marker-name limit-int)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");

        auto idx = as_string_idx(a[0]);
        if (idx >= ws.string_heap.size())
            return mev("bad-arg", "marker name string index out of range");
        auto marker_name = ws.string_heap[idx];

        // Map name → SyntaxMarker enum
        aura::ast::SyntaxMarker target;
        if (marker_name == "User") {
            target = aura::ast::SyntaxMarker::User;
        } else if (marker_name == "MacroIntroduced") {
            target = aura::ast::SyntaxMarker::MacroIntroduced;
        } else if (marker_name == "BoolLiteral") {
            target = aura::ast::SyntaxMarker::BoolLiteral;
        } else {
            return mev("unknown-marker", std::string("unknown marker name: \"") + marker_name +
                                             "\" (expected User / MacroIntroduced / BoolLiteral)");
        }

        std::int64_t limit = -1;
        if (a.size() == 2) {
            if (!is_int(a[1]))
                return mev("bad-arg", "limit must be an integer");
            limit = as_int(a[1]);
            if (limit < 0)
                return mev("bad-arg", "limit must be non-negative");
        }

        auto& flat = *ws.workspace_flat;
        EvalValue result = make_void();
        std::int64_t emitted = 0;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            if (limit >= 0 && emitted >= limit)
                break;
            if (flat.marker(id) == target) {
                auto pid = ws.pairs.size();
                ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                result = make_pair(pid);
                ++emitted;
            }
        }
        return result;
    });

    // (query:macro-introduced) — Issue #244: shortcut for
    // (query:filter (where :marker "MacroIntroduced")).
    // Returns a list of NodeIds whose SyntaxMarker is
    // SyntaxMarker::MacroIntroduced (i.e. code inserted by
    // a hygienic macro via clone_macro_body). Used by
    // AI agents and self-mod code to identify which AST
    // nodes are macro-introduced (and thus might need
    // hygiene-safe handling when mutating).
    //
    // Returns a list of node IDs (same encoding as
    // query:node-type / query:filter).
    //
    // Optional arg: an integer limit N. If provided, only
    // the first N matching node IDs are returned. Default
    // (no arg) returns all matches. Useful for the common
    // "do any macro-introduced nodes exist?" check
    // (pass limit=1).
    add("query:macro-introduced", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.size() > 1)
            return mev("bad-arg",
                       "usage: (query:macro-introduced) or (query:macro-introduced limit-int)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");

        std::int64_t limit = -1; // -1 = no limit
        if (a.size() == 1) {
            if (!is_int(a[0]))
                return mev("bad-arg", "limit must be an integer");
            limit = as_int(a[0]);
            if (limit < 0)
                return mev("bad-arg", "limit must be non-negative");
        }

        auto& flat = *ws.workspace_flat;
        EvalValue result = make_void();
        std::int64_t emitted = 0;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            if (limit >= 0 && emitted >= limit)
                break;
            if (flat.is_macro_introduced(id)) {
                auto pid = ws.pairs.size();
                ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                result = make_pair(pid);
                ++emitted;
            }
        }
        return result;
    });

    // (query:marker-stats) — Issue #247: aggregate SyntaxMarker
    // distribution in the workspace. Returns a 4-element list:
    //   (user-count macro-introduced-count bool-literal-count total-count)
    //
    // Equivalent to (syntax-marker-counts) but returns a simple
    // list instead of a hash table. Use this when you want to
    // pipe the result through list operations (e.g., assert all
    // counts are > 0 except for some category). For dashboards
    // and key/value lookups, (syntax-marker-counts) is the
    // better choice.
    //
    // Returns a proper list of 4 integers:
    //   (user macro-introduced bool-literal total)
    //
    // If no workspace is set, returns '().
    add("query:marker-stats", [ws, mev](const auto&) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");

        std::size_t user = 0, macro = 0, bool_lit = 0, total = 0;
        const auto& markers = ws.workspace_flat->marker_column();
        for (auto m : markers) {
            ++total;
            auto val = static_cast<int>(m);
            if (val == 0)
                ++user;
            else if (val == 1)
                ++macro;
            else if (val == 2)
                ++bool_lit;
        }
        // Build a proper 4-element list: (user macro bool total).
        // Use make_pair chaining (cdr-then-prepend pattern).
        // The list reads (total . (bool . (macro . (user . ())))) — i.e.
        // car=total, cdr=(bool ...). But we want (user macro bool total).
        // So build in reverse and then return as-is (the result is
        // a list whose first element is total). The Aura code can
        // destructure via pattern match.
        //
        // Actually, building the list in REVERSE order (push each
        // onto the head) would give (total . (bool . (macro . (user))))
        // which when iterated left-to-right yields total, bool, macro,
        // user. That's the wrong order. Build FORWARD: prepend each
        // new element to the previous list.
        EvalValue result = make_void();
        auto append = [&](std::int64_t v) {
            auto pid = ws.pairs.size();
            ws.pairs.push_back({make_int(v), result});
            result = make_pair(pid);
        };
        append(static_cast<std::int64_t>(user));
        append(static_cast<std::int64_t>(macro));
        append(static_cast<std::int64_t>(bool_lit));
        append(static_cast<std::int64_t>(total));
        // Now result is (user (macro (bool (total . ()))))
        return result;
    });

    // (query:schema-of-marker marker-name) — Issue #248:
    // return (NodeId . type-name) pairs for nodes with the given
    // SyntaxMarker AND a non-zero type_id_ (i.e., the type
    // checker has inferred a type for them).
    //
    // Use this to introspect macro-introduced code's inferred
    // types. Example:
    //   (query:schema-of-marker "MacroIntroduced")
    //   → ((3 . "int") (5 . "lambda") ...)
    //
    // Optional 2nd arg: integer limit N. Caps the result list
    // at N entries.
    //
    // If no workspace is set, returns '().
    //
    // Note: this is the *observability* side of #248. The
    // enforcement side (using the schema to reject mutations
    // that would break macro type invariants) is deferred.
    add("query:schema-of-marker", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || a.size() > 2 || !is_string(a[0]))
            return mev("bad-arg", "usage: (query:schema-of-marker marker-name) or "
                                  "(query:schema-of-marker marker-name limit-int)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        if (!ws.type_registry)
            return mev("no-registry", "no type registry available");

        auto idx = as_string_idx(a[0]);
        if (idx >= ws.string_heap.size())
            return mev("bad-arg", "marker name string index out of range");
        auto marker_name = ws.string_heap[idx];

        // Map name → SyntaxMarker enum
        aura::ast::SyntaxMarker target;
        if (marker_name == "User") {
            target = aura::ast::SyntaxMarker::User;
        } else if (marker_name == "MacroIntroduced") {
            target = aura::ast::SyntaxMarker::MacroIntroduced;
        } else if (marker_name == "BoolLiteral") {
            target = aura::ast::SyntaxMarker::BoolLiteral;
        } else {
            return mev("unknown-marker", std::string("unknown marker name: \"") + marker_name +
                                             "\" (expected User / MacroIntroduced / BoolLiteral)");
        }

        std::int64_t limit = -1;
        if (a.size() == 2) {
            if (!is_int(a[1]))
                return mev("bad-arg", "limit must be an integer");
            limit = as_int(a[1]);
            if (limit < 0)
                return mev("bad-arg", "limit must be non-negative");
        }

        auto& flat = *ws.workspace_flat;
        auto& treg = *static_cast<aura::core::TypeRegistry*>(ws.type_registry);
        EvalValue result = make_void();
        std::int64_t emitted = 0;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            if (limit >= 0 && emitted >= limit)
                break;
            if (flat.marker(id) != target)
                continue;
            // Issue #248: only include nodes with a non-zero
            // type_id_ (i.e., the type checker has inferred a
            // type for them). Nodes with type_id_ == 0 haven't
            // been typed yet — they're "schema-less" in the
            // sense that we don't know their expected type.
            const auto type_id_value = flat.type_id(id);
            if (type_id_value == 0)
                continue;
            // Convert uint32_t to TypeId. The FlatAST stores
            // just the index; TypeId requires (index, generation).
            // The type checker uses generation=1 as a default
            // (see type_checker_impl.cpp:1505: TypeId{index, 1}).
            std::string type_name;
            try {
                auto tname = treg.name_of(aura::core::TypeId{type_id_value, 1});
                type_name = std::string(tname);
            } catch (...) {
                // [SILENCE-PRIM-#615] type_name fallback for unknown
                // / unregistered TypeId — display-only path; caller
                // handles empty via the "<unnamed>" branch below.
                type_name = "<unknown>";
            }
            if (type_name.empty())
                type_name = "<unnamed>";
            // Build the pair: (NodeId . type-name)
            auto name_idx = ws.string_heap.size();
            ws.string_heap.push_back(type_name);
            auto name_ev = make_string(name_idx);
            auto pid = ws.pairs.size();
            ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), name_ev});
            auto pair_ev = make_pair(pid);
            // Prepend to result
            auto list_pid = ws.pairs.size();
            ws.pairs.push_back({pair_ev, result});
            result = make_pair(list_pid);
            ++emitted;
        }
        return result;
    });

    // ═══════════════════════════════════════════════════════════════
    // P8: Query/Transform EDSL 扩展 — pattern matching
    // ═══════════════════════════════════════════════════════════════

    // (query:pattern "expr") — Find all nodes matching a structural pattern
    //
    // Pattern syntax:
    //   (+ 1 2)     — exact match: Call("+", 1, 2)
    //   (+ 1 ...)   — wildcard: "..." matches any single subtree
    //   fib         — matches a Variable named "fib"
    //
    // The pattern is parsed as an S-expression. A Variable named "..." acts as
    // wildcard and matches any single node or subtree.
    //
    // Optional keywords (Issue #267 / #486):
    //   :include-macro-introduced [#t|#f]
    //   :allow-macro-introduced [#t|#f]  — discoverable alias (#486)
    // When absent or #f, macro-introduced root positions are skipped
    // (Issue #140 hygiene default). When #t, they are included.
    add("query:pattern", [ws, mev, &ev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty())
            return mev("bad-arg",
                       "usage: (query:pattern expr [:include-macro-introduced [#t]]"
                       " [:allow-macro-introduced [#t]] [:nested-arity [#t|#f]]"
                       " [:strict-arity [#t]] [:with-markers [#t]])");
        if (!ws.workspace_flat || !ws.workspace_pool)
            return mev("no-workspace", "no workspace AST loaded");

        bool have_pattern = false;
        std::size_t pattern_string_idx = 0;
        bool include_macro_introduced = false;
        // Issue #289 / #481: nested-arity / Kleene-star ellipsis.
        // Default (#t) is Kleene (`...` consumes 0..N consecutive
        // children). Set `:nested-arity #f` or `:strict-arity #t` to
        // opt back into the pre-#289 strict single-subtree wildcard
        // behavior (`...` consumes exactly 1 child). The :strict-arity
        // keyword is a discoverable alias for :nested-arity #f —
        // both flip the matcher to position-by-position matching.
        // NOTE: mutate:replace-pattern still uses its own strict
        // matcher (#482 will share the matcher and align the two
        // primitives). Until that lands, the query↔mutate semantics
        // differ in the default Kleene mode — pass :strict-arity #t
        // to query:pattern if you need to query the same node set
        // mutate:replace-pattern will see.
        bool nested_arity = true;
        // Issue #289: result format. Default (false) preserves the
        // pre-#289 result shape — a flat list of NodeIds. When
        // #t, each result item is a (NodeId . marker-int) pair so
        // agents can see which matches came from macro-expanded
        // code without a separate provenance query.
        bool with_markers = false;
        for (std::size_t ai = 0; ai < a.size(); ++ai) {
            if (is_string(a[ai])) {
                if (have_pattern)
                    return mev("bad-arg", "query:pattern: multiple pattern strings");
                have_pattern = true;
                pattern_string_idx = as_string_idx(a[ai]);
                if (pattern_string_idx >= ws.string_heap.size())
                    return mev("bad-arg", "pattern string index out of range");
            } else if (is_keyword(a[ai])) {
                auto kidx = as_keyword_idx(a[ai]);
                if (kidx >= ws.keyword_table.size())
                    return mev("bad-arg", "unknown keyword");
                auto kw = ws.keyword_table[kidx];
                // Issue #289: shared bool/optional-int flag consumer
                // for the three new keyword args. `target` defaults
                // to #t when the value is omitted (just keyword alone).
                auto consume_bool = [&](bool& target) {
                    target = true;
                    if (ai + 1 < a.size() && (is_bool(a[ai + 1]) || is_int(a[ai + 1]))) {
                        if (is_bool(a[ai + 1]))
                            target = as_bool(a[ai + 1]);
                        else
                            target = (as_int(a[ai + 1]) != 0);
                        ++ai;
                    }
                };
                if (kw == ":include-macro-introduced" || kw == ":allow-macro-introduced") {
                    consume_bool(include_macro_introduced);
                } else if (kw == ":respect-hygiene") {
                    // Issue #547: discoverable alias for
                    // :include-macro-introduced. Same semantics
                    // (skip MacroIntroduced by default for
                    // hygiene safety); the keyword reads more
                    // naturally for the EDSL self-evolution
                    // use case.
                    bool v = false;
                    if (ai + 1 < a.size() && (is_bool(a[ai + 1]) || is_int(a[ai + 1]))) {
                        if (is_bool(a[ai + 1]))
                            v = as_bool(a[ai + 1]);
                        else
                            v = (as_int(a[ai + 1]) != 0);
                        ++ai;
                    }
                    include_macro_introduced = v;
                } else if (kw == ":nested-arity") {
                    consume_bool(nested_arity);
                } else if (kw == ":strict-arity") {
                    // Issue #481: discoverable alias for the strict
                    // single-subtree wildcard mode (pre-#289 default).
                    // Equivalent to `:nested-arity #f`.
                    bool v = true;
                    if (ai + 1 < a.size() && (is_bool(a[ai + 1]) || is_int(a[ai + 1]))) {
                        if (is_bool(a[ai + 1]))
                            v = as_bool(a[ai + 1]);
                        else
                            v = (as_int(a[ai + 1]) != 0);
                        ++ai;
                    }
                    nested_arity = !v;
                } else if (kw == ":with-markers") {
                    consume_bool(with_markers);
                } else {
                    return mev("bad-arg", std::string("unknown query:pattern keyword: ") + kw);
                }
            } else {
                return mev("bad-arg",
                           "usage: (query:pattern expr [:include-macro-introduced [#t]]"
                           " [:allow-macro-introduced [#t]] [:nested-arity [#t|#f]]"
                           " [:strict-arity [#t]] [:with-markers [#t]])");
            }
        }
        if (!have_pattern)
            return mev("bad-arg", "query:pattern: missing pattern string");
        auto idx = pattern_string_idx;

        // Phase 2.5.0: pat_pool stays separate from canonical_pool.
        // Patterns parse into a fresh FlatAST + pool per call (ws.temp_arena
        // reclaims at gc-temp). The wildcard "..." sym is intern'd in
        // pat_pool so pat_node.sym_id comparisons work — sharing the
        // canonical pool would mix pattern-specific garbage into the long-
        // lived workspace, and the pattern's sym_ids would clash with
        // workspace sym_ids (different ASTs, same pool). Documented
        // exception to the canonical-pool migration — see commit 14682c5.
        // Parse pattern string into its own FlatAST (separate from workspace).
        // Use ws.temp_arena so (gc-temp) reclaims it per call.
        auto alloc = ws.temp_arena->allocator();
        auto* pat_pool = ws.temp_arena->create<aura::ast::StringPool>(alloc);
        auto* pat_flat = ws.temp_arena->create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(ws.string_heap[idx], *pat_flat, *pat_pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            return make_void();

        // Intern "..." in the pattern pool for wildcard matching.
        // (Symbol comparison keeps the wildcard + capture paths
        // orthogonal — "?x" is a capture, "..." is the legacy
        // single-subtree / new Kleene-star wildcard, never both.)
        auto wildcard_sym = pat_pool->intern("...");

        // Issue #482: use the shared QueryMatcher from query_matcher.hh.
        // Same matcher used by mutate:replace-pattern, so the two
        // primitives agree on which nodes match a pattern regardless
        // of :nested-arity mode.
        aura::compiler::QueryMatcher matcher(ws.workspace_flat, ws.workspace_pool, pat_flat,
                                             pat_pool, wildcard_sym, nested_arity,
                                             !include_macro_introduced);

        // ─── Per-match state (Issue #289, refactored in #482) ───────
        // Captures are stored as an insertion-ordered vector (linear
        // scan lookup is fine — typical patterns have < 10 captures;
        // the alternative unordered_map adds complexity for
        // save/restore during backtracking). Backtracking uses a
        // savepoint pattern: snapshot the current size, restore by
        // truncating the vector.


        auto& flat = *ws.workspace_flat;
        EvalValue result = make_void();

        // Issue #292: guard predicate support. After
        // match_subtree returns true, check pending_guards_ on
        // the matcher. If a guard is pending, build a let
        // expression that binds each ?capture to its captured
        // NodeId (as int), eval the guard string, and accept
        // the match only if the guard returns truthy. The
        // captures used here are pat-side sym_ids (e.g. "?x")
        // which the matcher recorded during match_subtree;
        // we resolve them via pat_pool to get the binding
        // name.
        auto check_guard = [&]() -> bool {
            if (!matcher.has_pending_guard())
                return true;
            const auto& pg = matcher.take_pending_guard();
            // Build let source. Each capture: (name value).
            // The captured value is the workspace node's value
            // (LiteralInt int_value) if applicable, else the
            // NodeId. This makes the guard expression natural
            // to write — the user does (> ?x 0) not (> <node-id> 0).
            std::string let_src = "(let (";
            for (const auto& kv : pg.captures) {
                // Skip sentinel sym_id 0 (wildcard captures).
                if (kv.first == 0)
                    continue;
                auto name = pat_pool->resolve(kv.first);
                if (name.empty() || name[0] != '?')
                    continue;
                int64_t bind_value = static_cast<int64_t>(kv.second);
                if (kv.second < ws.workspace_flat->size()) {
                    auto wsn = ws.workspace_flat->get(kv.second);
                    if (wsn.tag == aura::ast::NodeTag::LiteralInt) {
                        bind_value = wsn.int_value;
                    }
                }
                let_src += "(" + std::string(name) + " " + std::to_string(bind_value) + ")";
            }
            let_src += ") " + pg.guard_expr + ")";
            // Parse the let source into a temp flat and eval
            // it in top_env. This is the same parse_to_flat +
            // eval_flat pipeline used elsewhere in this file
            // (see pattern parse above); we use ws.temp_arena
            // so (gc-temp) reclaims it per call.
            auto alloc = ws.temp_arena->allocator();
            auto* guard_pool = ws.temp_arena->create<aura::ast::StringPool>(alloc);
            auto* guard_flat = ws.temp_arena->create<aura::ast::FlatAST>(alloc);
            auto pr = aura::parser::parse_to_flat(let_src, *guard_flat, *guard_pool);
            bool ok = false;
            if (pr.success && pr.root != aura::ast::NULL_NODE) {
                auto gr = ev.eval_flat(*guard_flat, *guard_pool, pr.root, ev.top_env());
                if (gr) {
                    auto& gv = *gr;

                    // Truthy = non-zero int, non-#f, non-void.
                    if (types::is_int(gv))
                        ok = (types::as_int(gv) != 0);
                    else if (types::is_bool(gv))
                        ok = types::as_bool(gv);
                    else if (types::is_pair(gv))
                        ok = true;
                }
            }

            matcher.clear_pending_guard();
            return ok;
        };

        // Issue #186 Phase 1: pre-compute the pattern's children
        // count once. The outer loop can then skip nodes whose
        // children count doesn't match the pattern's children
        // count, avoiding the recursive descent into subtrees
        // that can't possibly match. For large ASTs (500-5000
        // nodes) this is a real win — the O(N × Depth) becomes
        // O(N) when the pattern's children count is a strong
        // filter.
        auto pat_root_node = pat_flat->get(pr.root);
        const std::size_t pat_child_count = pat_root_node.children.size();
        const bool pat_root_is_wildcard = pat_root_node.tag == aura::ast::NodeTag::Variable &&
                                          pat_root_node.sym_id == wildcard_sym;
        // Issue #289: pre-compute whether the pattern contains any
        // "..." descendant. In Kleene mode + ellipsis anywhere in
        // the pattern, the workspace arity is not fixed, so the
        // (tag, arity) index fast path can't prune by arity — we
        // must do a full walk. In the default strict mode the
        // arity is still fixed (single-subtree wildcard), so the
        // index helps regardless of ellipsis presence.
        const bool pat_has_ellipsis = matcher.pat_has_ellipsis_rec(pr.root);
        // Issue #292: (:guard <sub-pat> "expr") wrappers have
        // tag=Call, arity=2-3 — the index fast path would skip
        // positions whose (tag, arity) doesn't match. Force
        // slow path for guard wrappers.
        const bool pat_is_guard = matcher.is_guard_root(pr.root);
        const bool use_index_fast_path =
            !pat_root_is_wildcard && !pat_is_guard && (!nested_arity || !pat_has_ellipsis);


        // Walk every node in workspace and try matching at each position.
        // Issue #140: skip nodes with SyntaxMarker::MacroIntroduced
        // (the matcher's root position is the user-written top-level
        // code, not the macro-expanded body). Hygiene correctness:
        // matching a macro-introduced call as if it were user code
        // would be misleading. The pattern only matches user-written
        // code by default. Issue #267: pass :include-macro-introduced
        // #t to opt in to matching macro-introduced root positions.
        //
        // Issue #186: also skip nodes whose children count doesn't
        // match the pattern's children count (the pattern's
        // children are the only subtree that can match). This is
        // a quick early-exit that's safe because:
        //   - If pat_root is a wildcard "..." → no constraint
        //   - Otherwise, the children count must match exactly
        //     (verified later in match_subtree's default case)
        //
        // Issue #211: use the (tag, arity) index to skip
        // non-matching nodes BEFORE the recursive descent.
        // For patterns where the root's (tag, arity) is rare
        // (e.g., looking for `(+ 1 2)` in a workspace with
        // mostly `define`s and `lambda`s), this is a massive
        // speedup vs. the O(N) full walk.
        //
        // The index is built lazily on first use and cached
        // per-workspace (invalidated when ws.workspace_flat is
        // changed via set_workspace_flat).
        if (use_index_fast_path) {
            // Index lookup: find all nodes whose (tag, arity)
            // matches the pattern's root.
            //
            // Issue #371: build takes unique_lock internally,
            // we then drop back to shared_lock for the actual
            // `find` + bucket iteration. The window between
            // build's lock release and our shared_lock acquire
            // admits a semantic race (another fiber can
            // invalidate + rebuild with a different gen), but
            // the worst case is `find` returning end() — safe
            // (no UB on hash table). We accept that gap for the
            // scope of #371; closing it is a follow-up
            // (combined build+read under a single unique_lock
            // would defeat reader parallelism).
            ws.build_tag_arity_index();
            std::shared_lock<std::shared_mutex> rlock(ws.tag_arity_index_mtx);
            const std::uint32_t pat_tag_val = static_cast<std::uint32_t>(pat_root_node.tag);
            const std::uint64_t pat_key = (static_cast<std::uint64_t>(pat_tag_val) << 32) |
                                          static_cast<std::uint64_t>(pat_child_count);
            auto it = ws.tag_arity_index.find(pat_key);
            if (it == ws.tag_arity_index.end()) {
                // No nodes match the pattern's (tag, arity).
                // Skip the full walk.
                ev.bump_pattern_structural_index_miss();
                return make_void();
            }
            const auto& bucket = it->second;
            ev.bump_pattern_structural_index_hit();
            ev.bump_total_query_calls();
            for (aura::ast::NodeId id : bucket) {
                if (id >= flat.size())
                    continue;
                if (!include_macro_introduced && flat.is_macro_introduced(id)) {
                    // Issue #458: hygiene-skip stats.
                    ev.bump_macro_introduced_skipped_in_query();
                    continue;
                }
                // Issue #289: fresh per-match state. Captures
                // and depth are reset so a failed match doesn't
                // pollute the next position's attempt.
                matcher.state.captures.clear();
                matcher.state.depth = 0;
                if (matcher.match_subtree(id, pr.root)) {
                    // Issue #292: guard predicate check.
                    if (!check_guard())
                        continue;
                    // Issue #289: result format. With
                    // :with-markers #t, store a (NodeId . marker-int)
                    // pair per match so agents can see which
                    // matches came from macro-expanded code.
                    // Default (false) keeps the pre-#289 shape —
                    // flat list of NodeIds.
                    EvalValue item;
                    if (with_markers) {
                        auto nid_int = make_int(static_cast<std::int64_t>(id));
                        auto marker_int = make_int(static_cast<std::int64_t>(flat.marker(id)));
                        auto pair_pid = ws.pairs.size();
                        ws.pairs.push_back({nid_int, marker_int});
                        item = make_pair(pair_pid);
                    } else {
                        item = make_int(static_cast<std::int64_t>(id));
                    }
                    auto pid = ws.pairs.size();
                    ws.pairs.push_back({item, result});
                    result = make_pair(pid);
                }
            }
        } else {
            // Full walk (Kleene + ellipsis, or wildcard root).
            ev.bump_total_query_calls();
            for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
                if (!include_macro_introduced && flat.is_macro_introduced(id)) {
                    ev.bump_macro_introduced_skipped_in_query();
                    continue;
                }
                // Issue #484: skip orphan nodes (parent_ == NULL
                // and not the workspace root). After
                // mutate:replace-pattern, the OLD matched child
                // has its parent_ cleared by set_child, leaving
                // it orphaned in the flat. Such orphans should
                // not be returned by query:pattern — they are
                // no longer reachable from the workspace tree.
                //
                // Issue #484 follow-up: marker-based
                // disambiguation. MacroIntroduced-marker
                // orphans are macro-expanded bodies cloned by
                // clone_macro_body whose cloned top got
                // MacroIntroduced marker but whose
                // macro_expand_all call failed to splice them
                // into the workspace (a separate bug — see
                // #484 follow-up). Allowing them through keeps
                // these bodies queryable. User-marker orphans
                // are typically mutate-replaced children (the
                // genuine lost-from-tree case) and should be
                // excluded.
                //
                // Edge case: if the flat has no root set (root
                // == NULL_NODE, e.g. test fixture that builds a
                // bare flat without a workspace root), every
                // node is by definition orphan. Don't skip any
                // — the caller (test fixture) is intentionally
                // exercising index operations on orphan-like
                // nodes.
                if (flat.root != aura::ast::NULL_NODE && id != flat.root &&
                    flat.parent_of(id) == aura::ast::NULL_NODE && !flat.is_macro_introduced(id))
                    continue;
                matcher.state.captures.clear();
                matcher.state.depth = 0;
                if (matcher.match_subtree(id, pr.root)) {
                    // Issue #292: guard predicate check.
                    if (!check_guard())
                        continue;
                    EvalValue item;
                    if (with_markers) {
                        auto nid_int = make_int(static_cast<std::int64_t>(id));
                        auto marker_int = make_int(static_cast<std::int64_t>(flat.marker(id)));
                        auto pair_pid = ws.pairs.size();
                        ws.pairs.push_back({nid_int, marker_int});
                        item = make_pair(pair_pid);
                    } else {
                        item = make_int(static_cast<std::int64_t>(id));
                    }
                    auto pid = ws.pairs.size();
                    ws.pairs.push_back({item, result});
                    result = make_pair(pid);
                }
            }
        }

        // Issue #421: sync recursive hygiene skips + verify
        // default-hygiene results never surface MacroIntroduced
        // node ids (post query-split contract).
        if (matcher.recursive_macro_skipped() > 0) {
            ev.bump_pattern_recursive_macro_skipped(matcher.recursive_macro_skipped());
        }
        if (!include_macro_introduced) {
            ev.verify_pattern_result_hygiene(flat, result, with_markers);
        }
        return result;
    });

    // Issue #282: (query:provenance-of var-name) — return the
    // list of NarrowingRecords that narrowed `var-name`. Each
    // entry is a hash with keys: :predicate, :refined-type,
    // :if-node, :cond-node, :is-negation, :narrow-evidence,
    // :capture-epoch, :record-id. Empty list if no narrowing
    // found or no workspace loaded.
    //
    // Use case: AI agent debugging "why does x have type T in
    // this branch?" → calls (query:provenance-of "x") to get
    // the predicates that refined x.
    add("query:provenance-of", [ws, mev](const auto& a) -> EvalValue {
        if (a.size() != 1 || !ws.workspace_flat) {
            mev("bad-arg", "query:provenance-of expects 1 string arg");
            return make_void();
        }
        if (!is_string(a[0])) {
            mev("bad-arg", "query:provenance-of: arg must be a string");
            return make_void();
        }
        auto idx = as_string_idx(a[0]);
        if (idx >= ws.string_heap.size()) {
            return make_void();
        }
        std::string var_name = ws.string_heap[idx];
        const auto& flat = *ws.workspace_flat;
        const auto& log = flat.all_narrowings();
        EvalValue result = make_void();
        // Cons the results in reverse so they appear in
        // application order.
        std::vector<EvalValue> entries;
        for (const auto& rec : log) {
            if (rec.var_name != var_name)
                continue;
            // Build the entry hash. 8-slot is enough for 7 fields
            // (one slot of slack).
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                continue;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"predicate", make_string(ws.string_heap.size())},
                {"refined-type", make_string(ws.string_heap.size() + 1)},
                {"if-node", make_int(static_cast<std::int64_t>(rec.if_node))},
                {"cond-node", make_int(static_cast<std::int64_t>(rec.cond_node))},
                {"is-negation", make_bool(rec.is_negation)},
                {"narrow-evidence", make_int(static_cast<std::int64_t>(rec.narrow_evidence))},
                {"capture-epoch", make_int(static_cast<std::int64_t>(rec.capture_epoch))},
                {"record-id", make_int(static_cast<std::int64_t>(rec.record_id))},
                {"stale", make_bool(rec.stale)},
            };
            // Push the strings to the heap.
            ws.string_heap.push_back(rec.predicate_src);
            ws.string_heap.push_back(rec.refined_type_str);
            // Insert into the hash.
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto cap = ht->capacity;
            bool ok = true;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ws.string_heap.size();
                ws.string_heap.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < cap; ++at) {
                    auto idx = ((h >> 1) + at) & (cap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                FlatHashTable::destroy(ht);
                continue;
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            auto cons_pair = ws.pairs.size();
            ws.pairs.push_back({make_hash(hidx), result});
            result = make_pair(cons_pair);
        }
        return result;
    });

    // Issue #282 follow-up #2: (query:provenance-of *) — list all
    // variables that have at least one narrowing provenance entry.
    // Returns a list of strings (variable names). Empty if no
    // workspace or no narrowings recorded.
    //
    // Use case: AI agent asking "what variables have been
    // narrowed in this scope?" without knowing the names up
    // front. Pairs with (query:provenance-of var) to drill
    // into specific narrowings.
    add("query:provenance-of*", [ws, mev](const auto& a) -> EvalValue {
        if (a.size() != 0 || !ws.workspace_flat) {
            mev("bad-arg", "query:provenance-of* expects 0 args");
            return make_void();
        }
        const auto& flat = *ws.workspace_flat;
        const auto& log = flat.all_narrowings();
        // Use a set to dedupe (the same var may have multiple
        // entries; we just want the distinct names).
        std::unordered_set<std::string> seen;
        std::vector<std::string> names;
        for (const auto& rec : log) {
            if (seen.insert(rec.var_name).second) {
                names.push_back(rec.var_name);
            }
        }
        // Cons the result in reverse so names appear in
        // application order.
        EvalValue result = make_void();
        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            std::size_t sidx = ws.string_heap.size();
            ws.string_heap.push_back(*it);
            auto cons_pair = ws.pairs.size();
            ws.pairs.push_back({make_string(sidx), result});
            result = make_pair(cons_pair);
        }
        return result;
    });

    // Issue #282 follow-up #5: (query:narrowings-at-mutation mutation-id) —
    // return the list of NarrowingRecord capture_epochs that
    // were LIVE at the time of the given mutation. A narrowing
    // is "live at mutation M" if its capture_epoch <= M's
    // mutation_epoch. Returns a list of (record_id . predicate)
    // pairs (the minimal info needed to identify the narrowing).
    //
    // Use case: AI agent asks "what narrowings were in effect
    // when mutation M happened?" — gives context for blame
    // queries.
    add("query:narrowings-at-mutation", [ws, mev](const auto& a) -> EvalValue {
        if (a.size() != 1 || !ws.workspace_flat) {
            mev("bad-arg", "query:narrowings-at-mutation expects 1 int arg");
            return make_void();
        }
        if (!is_int(a[0])) {
            mev("bad-arg", "query:narrowings-at-mutation: arg must be an int");
            return make_void();
        }
        auto target_mid = static_cast<std::uint64_t>(as_int(a[0]));
        const auto& flat = *ws.workspace_flat;
        const auto& log = flat.all_narrowings();
        EvalValue result = make_void();
        std::vector<EvalValue> entries;
        for (const auto& rec : log) {
            // A narrowing captured at epoch E is "live at" any
            // mutation with id >= E. The mutation_epoch_ in
            // CompilerService roughly tracks the mutation_id,
            // so we use capture_epoch as a proxy.
            if (rec.capture_epoch > target_mid)
                continue;
            // Build a hash with :record-id + :predicate + :var.
            std::size_t sidx_v = ws.string_heap.size();
            ws.string_heap.push_back(rec.var_name);
            std::size_t sidx_p = ws.string_heap.size();
            ws.string_heap.push_back(rec.predicate_src);
            std::size_t k_id = ws.string_heap.size();
            ws.string_heap.push_back("record-id");
            std::size_t k_v = ws.string_heap.size();
            ws.string_heap.push_back("var");
            std::size_t k_p = ws.string_heap.size();
            ws.string_heap.push_back("predicate");
            std::size_t k_e = ws.string_heap.size();
            ws.string_heap.push_back("capture-epoch");
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                continue;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"record-id", make_int(static_cast<std::int64_t>(rec.record_id))},
                {"var", make_string(sidx_v)},
                {"predicate", make_string(sidx_p)},
                {"capture-epoch", make_int(static_cast<std::int64_t>(rec.capture_epoch))},
            };
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto cap = ht->capacity;
            bool ok = true;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                EvalValue key_ev = make_string(k == "record-id"   ? k_id
                                               : k == "var"       ? k_v
                                               : k == "predicate" ? k_p
                                                                  : k_e);
                bool inserted = false;
                for (std::size_t at = 0; at < cap; ++at) {
                    auto idx = ((h >> 1) + at) & (cap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                FlatHashTable::destroy(ht);
                continue;
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            entries.push_back(make_hash(hidx));
        }
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            auto cons_pair = ws.pairs.size();
            ws.pairs.push_back({*it, result});
            result = make_pair(cons_pair);
        }
        return result;
    });

    // Issue #279 follow-up #4: (register-predicate! name
    // type-name) — register a custom Occurrence Typing
    // predicate. After this, (if (name x) body) refines x to
    // the named type in the then-branch (same path as built-in
    // predicates like string?, pair?, list?).
    //
    // The mapping is stored in the aura.core.mutation module's
    // process-global registry (declared in mutation.ixx,
    // defined in mutation_impl.cpp). Both this module and the
    // type_checker module share the same definition.
    add("register-predicate!",
        [&ws, &string_heap, &pairs](std::span<const EvalValue> a) -> EvalValue {
            (void)ws;
            (void)pairs;
            if (a.size() != 2 || !is_string(a[0]) || !is_string(a[1])) {
                return make_void();
            }
            auto nidx = as_string_idx(a[0]);
            auto tidx = as_string_idx(a[1]);
            if (nidx >= string_heap.size() || tidx >= string_heap.size()) {
                return make_void();
            }
            aura::ast::mutation::register_custom_predicate(string_heap[nidx], string_heap[tidx]);
            return make_bool(true);
        });
}

} // namespace aura::compiler::primitives_detail
