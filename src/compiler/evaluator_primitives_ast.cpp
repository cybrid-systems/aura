// evaluator_primitives_ast.cpp — P0 step 13: ast:* primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.parser.parser;

namespace aura::compiler::primitives_detail {

using WorkspaceTree = aura::compiler::WorkspaceTree;
using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;
using DefUseSummaryStats = std::optional<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>>;

using namespace types;

void register_ast_primitives(PrimRegistrar add, Evaluator& ev,
                             std::function<void()> destroy_defuse_index,
                             std::function<DefUseSummaryStats()> defuse_summary_stats) {

    // Helper: line-based LCS diff (Myers-like, simplified)
    // Returns list of (tag . line) entries
    auto line_diff = [&ev](const std::string& old_text, const std::string& new_text) -> EvalValue {
        // Split into lines
        auto split_lines = [](const std::string& s) -> std::vector<std::string> {
            std::vector<std::string> lines;
            std::string cur;
            for (auto c : s) {
                if (c == '\n') {
                    lines.push_back(std::move(cur));
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            if (!cur.empty())
                lines.push_back(std::move(cur));
            if (lines.empty())
                lines.push_back("");
            return lines;
        };

        auto a = split_lines(old_text);
        auto b = split_lines(new_text);
        int m = (int)a.size(), n = (int)b.size();

        // Build LCS table (2 rows for memory efficiency)
        // Use short int to keep table small; m,n rarely exceed 500
        std::vector<int> prev(n + 1, 0), cur(n + 1, 0);
        for (int i = 1; i <= m; ++i) {
            cur[0] = i;
            for (int j = 1; j <= n; ++j) {
                if (a[i - 1] == b[j - 1])
                    cur[j] = prev[j - 1];
                else
                    cur[j] = 1 + std::min({prev[j], cur[j - 1], prev[j - 1]});
            }
            std::swap(prev, cur);
        }

        // Backtrack to produce diff
        std::vector<std::tuple<char, std::string>> diff_entries; // '=', '-', '+'
        int i = m, j = n;
        auto saved_prev = prev;
        (void)saved_prev;
        // We need the full table for backtracking. Build it.
        std::vector<std::vector<int>> table(m + 1, std::vector<int>(n + 1, 0));
        for (int i2 = 1; i2 <= m; ++i2) {
            for (int j2 = 1; j2 <= n; ++j2) {
                if (a[i2 - 1] == b[j2 - 1])
                    table[i2][j2] = table[i2 - 1][j2 - 1] + 1;
                else
                    table[i2][j2] = std::max(table[i2 - 1][j2], table[i2][j2 - 1]);
            }
        }

        while (i > 0 || j > 0) {
            if (i > 0 && j > 0 && a[i - 1] == b[j - 1]) {
                diff_entries.push_back({'=', a[i - 1]});
                --i;
                --j;
            } else if (j > 0 && (i == 0 || table[i][j - 1] >= table[i - 1][j])) {
                diff_entries.push_back({'+', b[j - 1]});
                --j;
            } else {
                diff_entries.push_back({'-', a[i - 1]});
                --i;
            }
        }
        std::reverse(diff_entries.begin(), diff_entries.end());

        // Convert to Aura list
        EvalValue result = make_void();
        for (auto it = diff_entries.rbegin(); it != diff_entries.rend(); ++it) {
            auto [tag, line] = *it;
            std::string kw_str = ":same";
            if (tag == '-')
                kw_str = ":removed";
            else if (tag == '+')
                kw_str = ":added";

            // Lookup or create keyword
            auto kw_idx = ev.keyword_table_.size();
            for (std::size_t ki = 0; ki < ev.keyword_table_.size(); ++ki) {
                if (ev.keyword_table_[ki] == kw_str) {
                    kw_idx = ki;
                    break;
                }
            }
            if (kw_idx == ev.keyword_table_.size())
                ev.keyword_table_.push_back(kw_str);

            auto line_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(line);
            auto line_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_keyword(kw_idx), make_string(line_idx)});
            auto cons_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_pair(line_pair), result});
            result = make_pair(cons_pair);
        }
        return result;
    };

    // (ast:snapshot ["name"])
    //   → integer snapshot ID (or -1 on failure)
    //   Stores current workspace source code as a named checkpoint.
    //   Names are optional; unnamed snapshots get auto-generated names.
    //
    // (#107 part 6) Also stores a direct deep-copy of the workspace's
    // FlatAST and StringPool. ast:restore prefers the direct copy
    // (lossless, no reparse) and falls back to the source string
    // only when the direct copy is missing (e.g. for snapshots
    // taken before this feature shipped).
    add("ast:snapshot", [&ev](std::span<const EvalValue> a) -> EvalValue {
        std::unique_lock<std::shared_mutex> wlock(ev.workspace_mtx_);
        if (!ev.workspace_flat_ || !ev.workspace_pool_)
            return make_int(-1);

        // Get current source
        auto src_fn = ev.primitives_.lookup("current-source");
        if (!src_fn)
            return make_int(-1);
        auto src = (*src_fn)({});
        if (!is_string(src))
            return make_int(-1);
        auto src_idx = as_string_idx(src);
        if (src_idx >= ev.string_heap_.size())
            return make_int(-1);
        auto source = ev.string_heap_[src_idx];

        // Optional name
        std::string name;
        if (a.size() >= 1 && is_string(a[0])) {
            auto name_idx = as_string_idx(a[0]);
            if (name_idx < ev.string_heap_.size())
                name = ev.string_heap_[name_idx];
        }

        // (#107 part 6) Take a direct deep copy of the workspace's
        // flat + pool. The snapshot's std::unique_ptr<FlatAST> uses
        // std::pmr::get_default_resource() (heap); the data is
        // self-contained and does not alias the workspace. On
        // restore, we copy-assign back into the workspace pool/flat
        // (whose pmr vectors use the arena allocator) — the arena
        // ends up with the restored data, and the snapshot retains
        // its own heap copy for subsequent restores.
        // Issue #261: reclaim unreachable NodeId slots before the
        // deep copy so long-running sessions don't amplify dead
        // slots into every snapshot.
        ev.workspace_flat_->recycle_dead_nodes();

        Evaluator::FlatSnapshot fs;
        try {
            fs.flat = std::make_unique<aura::ast::FlatAST>();
            fs.pool = std::make_unique<aura::ast::StringPool>();
            *fs.flat = *ev.workspace_flat_;
            *fs.pool = *ev.workspace_pool_;
            fs.has_flat = true;
            fs.flat_generation = ev.workspace_flat_->generation();
            fs.flat_size = ev.workspace_flat_->size();
            if (ev.workspace_tree_) {
                auto* wt = static_cast<WorkspaceTree*>(ev.workspace_tree_);
                if (auto* node = wt->active())
                    fs.cow_epoch = node->cow_epoch;
            }
        } catch (...) {
            // OOM during deep copy — store the source anyway so the
            // snapshot at least exists for diff / fallback restore.
            fs.has_flat = false;
        }

        auto id = ev.snapshot_sources_.size();
        ev.snapshot_sources_.push_back(source);
        ev.snapshot_names_.push_back(name);
        ev.snapshot_flats_.push_back(std::move(fs));
        return make_int(static_cast<std::int64_t>(id));
    });

    // ── Issue #107 part 3: AST versioning protocol primitive ──────────
    // (ast:version) — return a snapshot of the AST's current versioning
    // state under the ev.workspace_mtx_ (shared lock).
    //
    // The protocol surface returned is:
    //   (defuse-version . (dirty-define-name ...) . (all-define-name ...))
    //   e.g.   (5 . ("f" "g") . ("f" "g" "h"))
    //
    // defuse-version is a monotonic counter incremented by every
    // mutate:* primitive. Caches (DefUseIndex, IR cache v2, JIT
    // symbol table) use it to detect staleness: a cache entry
    // recorded under version N is invalid once version > N.
    //
    // dirty-define-name is the list of top-level defines whose IR
    // cache entry is currently marked dirty (i.e., needs re-lower
    // on next eval-current :jit).
    //
    // all-define-name is the list of all top-level defines in the
    // workspace. The LLM can diff against the previous call's
    // list to detect new / removed defines.
    //
    // This primitive is the LLM-facing surface for the
    // incremental-cache invalidation protocol. A concurrent
    // mutate that mutates between defuse-version read and
    // dirty-list read would be racy in a non-locked
    // implementation; the shared_lock makes the read atomic.
    add("ast:version", [&ev](const auto&) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        if (!ev.workspace_flat_ || !ev.workspace_pool_)
            return make_void();

        // Walk the workspace's top-level defines, collecting
        // names + dirty state. The walk is O(N) in the workspace
        // size; acceptable for occasional LLM version queries.
        std::vector<std::string> all_names;
        std::vector<std::string> dirty_names;
        auto& flat = *ev.workspace_flat_;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag != aura::ast::NodeTag::Define)
                continue;
            auto name_str = std::string(ev.workspace_pool_->resolve(v.sym_id));
            if (name_str.empty())
                continue;
            all_names.push_back(name_str);
            if (ev.is_define_dirty_fn_ && ev.is_define_dirty_fn_(name_str))
                dirty_names.push_back(name_str);
        }

        // Build the result list:
        //   (defuse-version . (dirty ...) . (all ...))
        auto str_idx = [&](const std::string& s) -> EvalValue {
            auto idx = ev.string_heap_.size();
            ev.string_heap_.push_back(s);
            return make_string(idx);
        };
        // Build (all ...) and (dirty ...) lists as nested pairs.
        auto list_of = [&](const std::vector<std::string>& v) -> EvalValue {
            EvalValue r = make_void();
            for (auto it = v.rbegin(); it != v.rend(); ++it) {
                auto pid = ev.pairs_.size();
                ev.pairs_.push_back({str_idx(*it), r});
                r = make_pair(pid);
            }
            return r;
        };
        auto dirty_list = list_of(dirty_names);
        auto all_list = list_of(all_names);
        // Pack: (defuse-version . (dirty ...) . (all ...))
        auto v_pid = ev.pairs_.size();
        ev.pairs_.push_back({make_int(static_cast<std::int64_t>(
                                 ev.defuse_version_.load(std::memory_order_relaxed))),
                             dirty_list});
        auto v_pair = make_pair(v_pid);
        auto final_pid = ev.pairs_.size();
        ev.pairs_.push_back({v_pair, all_list});
        return make_pair(final_pid);
    });

    // (ast:defs)
    //   → ((name . node-id) ...)  top-level (define ...) entries.
    //   Each pair is (symbol-name . node-id-of-define). Used by EDSL
    //   workflows that want to enumerate workspace top-level
    //   bindings without walking node tags manually.
    //   (#108 part 2) Added because lib/std/adaptive.aura documents
    //   (ast:defs) as a core primitive, but it was never wired up.
    //   EDSL benchmark (edsl-snapshot-multi etc.) hits 'unbound
    //   variable: ast:defs' otherwise.
    add("ast:defs", [&ev](const auto&) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        if (!ev.workspace_flat_ || !ev.workspace_pool_)
            return make_void();
        auto& flat = *ev.workspace_flat_;
        // Build the (name . node-id) alist in reverse, then return.
        // Order is workspace order (top-level defines are at the
        // top of the flat; later defines come later).
        EvalValue result = make_void();
        // Walk in reverse so that pushes to the front of the list
        // produce the same order as the flat.
        for (std::int64_t id = (std::int64_t)flat.size() - 1; id >= 0; --id) {
            auto v = flat.get(static_cast<aura::ast::NodeId>(id));
            if (v.tag != aura::ast::NodeTag::Define)
                continue;
            auto name_str = std::string(ev.workspace_pool_->resolve(v.sym_id));
            if (name_str.empty())
                continue;
            auto name_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(name_str);
            // (name . id) pair
            auto entry_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_string(name_idx), make_int(static_cast<std::int64_t>(id))});
            // cons onto result
            auto cons_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_pair(entry_pair), result});
            result = make_pair(cons_pair);
        }
        return result;
    });

    // (ast:nodes)
    //   → (node-id ...)  all node IDs in the workspace, in flat order.
    //   (#108 part 2) Same fix as ast:defs. Documented in
    //   adaptive.aura but never wired up.
    add("ast:nodes", [&ev](const auto&) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        if (!ev.workspace_flat_)
            return make_void();
        auto& flat = *ev.workspace_flat_;
        EvalValue result = make_void();
        for (std::int64_t id = (std::int64_t)flat.size() - 1; id >= 0; --id) {
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_int(id), result});
            result = make_pair(pid);
        }
        return result;
    });
    // (ast:list-snapshots)
    //   → ((id "name") ...)  list of (snapshot-id . name) pairs
    add("ast:list-snapshots", [&ev](const auto&) -> EvalValue {
        EvalValue result = make_void();
        for (int i = (int)ev.snapshot_sources_.size() - 1; i >= 0; --i) {
            auto name_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(ev.snapshot_names_[i].empty() ? std::format("snapshot-{}", i)
                                                                    : ev.snapshot_names_[i]);
            // Pair: (id . name)
            auto entry_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_int(i), make_string(name_idx)});
            // Cons with result list
            auto cons_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_pair(entry_pair), result});
            result = make_pair(cons_pair);
        }
        return result;
    });

    // (ast:restore id)
    //   → true on success
    //   Replaces current workspace with a previously snapshotted state.
    //   Caches (def-use, incremental compile state) are invalidated.
    //
    // (#107 part 6) Two restore paths:
    //   1. Direct: copy-assign the snapshot's FlatAST/StringPool into
    //      the workspace's pool/flat. Lossless (mutation_log_,
    //      type_id_, value_cache_ all preserved) and fast (no reparse).
    //   2. Fallback: re-parse the stored source string via set-code.
    //      Used when the snapshot is older (no direct copy) or the
    //      direct copy is missing. Loses any metadata that the parser
    //      doesn't reproduce.
    // We try the direct path first; on failure (e.g. workspace
    // pool/flat is null, which shouldn't happen but be defensive)
    // we fall through to the source path.
    add("ast:restore", [&ev, destroy_defuse_index](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto id = static_cast<std::size_t>(as_int(a[0]));
        if (id >= ev.snapshot_sources_.size())
            return make_bool(false);

        // Clear any previous restore error / eval cache
        ev.last_set_code_error_kind_.clear();
        ev.last_set_code_error_msg_.clear();
        ev.last_eval_current_result_.reset();

        // (#107 part 6) Direct path: copy from snapshot's flat/pool
        // into the workspace's flat/pool. This is the lossless
        // restore path — SymIds, mutation_log_, type_id_, value_cache_
        // are all preserved bit-for-bit.
        bool did_direct = false;
        {
            std::unique_lock<std::shared_mutex> wlock(ev.workspace_mtx_);
            if (ev.workspace_read_only_)
                return make_bool(false);
            if (id < ev.snapshot_flats_.size() && ev.snapshot_flats_[id].has_flat &&
                ev.snapshot_flats_[id].flat && ev.snapshot_flats_[id].pool && ev.workspace_flat_ &&
                ev.workspace_pool_) {
                try {
                    *ev.workspace_flat_ = *ev.snapshot_flats_[id].flat;
                    *ev.workspace_pool_ = *ev.snapshot_flats_[id].pool;
                    // Issue #261: recycle any slots that became
                    // unreachable across restore iterations.
                    ev.workspace_flat_->recycle_dead_nodes();
                    ev.update_shared_tree_root();
                    // Issue #263: verify generation/span consistency after restore.
                    ev.last_post_restore_report_ = ev.workspace_flat_->validate_post_restore();
                    ev.last_post_restore_violations_ = ev.last_post_restore_report_.violations;
                    // Invalidate caches (same as set-code)
                    // (ASAN fix #107 leak) delete the old index.
                    destroy_defuse_index();
                    ev.defuse_affected_syms_.clear();
                    if (ev.mark_all_defines_dirty_fn_)
                        ev.mark_all_defines_dirty_fn_();
                    if (ev.pre_cache_workspace_defines_fn_)
                        ev.pre_cache_workspace_defines_fn_();
                    did_direct = true;
                } catch (...) {
                    // Fall through to source-based restore
                    did_direct = false;
                }
            }
        }
        if (did_direct)
            return make_bool(true);

        // Source-based fallback (existing behavior). Holds its own
        // unique_lock inside set-code; we must not be holding ours
        // (we released above by scope).
        auto& source = ev.snapshot_sources_[id];
        auto source_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(source);

        auto set_fn = ev.primitives_.lookup("set-code");
        if (!set_fn)
            return make_bool(false);
        auto result = (*set_fn)({make_string(source_idx)});
        return make_bool(is_bool(result) ? as_bool(result) : false);
    });

    // (ast:diff [id])
    //   → ((tag . line) ...)
    //   Compares current source vs a snapshot. If no id given, compares
    //   versus the most recent snapshot. Tags: :same / :removed / :added
    add("ast:diff", [&ev, line_diff](const auto& a) -> EvalValue {
        if (!ev.workspace_flat_ || !ev.workspace_pool_)
            return make_void();

        std::size_t id;
        if (a.empty() || !is_int(a[0])) {
            // Default to most recent snapshot
            if (ev.snapshot_sources_.empty())
                return make_void();
            id = ev.snapshot_sources_.size() - 1;
        } else {
            id = static_cast<std::size_t>(as_int(a[0]));
            if (id >= ev.snapshot_sources_.size())
                return make_void();
        }

        auto& old_source = ev.snapshot_sources_[id];

        // Get current source
        auto src_fn = ev.primitives_.lookup("current-source");
        if (!src_fn)
            return make_void();
        auto src = (*src_fn)({});
        if (!is_string(src))
            return make_void();
        auto src_idx = as_string_idx(src);
        if (src_idx >= ev.string_heap_.size())
            return make_void();
        auto& new_source = ev.string_heap_[src_idx];

        return line_diff(old_source, new_source);
    });
    auto str_list_to_pairs = [&ev](std::span<const std::string> items) -> EvalValue {
        EvalValue list = make_void();
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            auto idx = ev.string_heap_.size();
            ev.string_heap_.push_back(*it);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(idx), list});
            list = make_pair(pid);
        }
        return list;
    };

    // (ast:summary)
    //   → ((:key value) ...)  association list
    //   Returns structural summary of the current workspace:
    //     :total-nodes    — total AST node count
    //     :by-tag         — list of (tag-name . count)
    //     :mutation-count — number of applied mutations
    //     :scopes         — number of lexical scopes (from def-use cache)
    //     :defs           — total tracked definitions
    //     :uses           — total tracked variable uses
    //     :source-length  — source code character count
    add("ast:summary", [&ev, str_list_to_pairs, defuse_summary_stats](const auto&) -> EvalValue {
        if (!ev.workspace_flat_ || !ev.workspace_pool_)
            return make_void();

        auto& flat = *ev.workspace_flat_;
        auto total_nodes = flat.size();

        // Count nodes by type
        std::unordered_map<std::string, std::uint64_t> type_counts;
        for (aura::ast::NodeId id = 0; id < total_nodes; ++id) {
            auto v = flat.get(id);
            auto& m = aura::ast::meta(v.tag);
            if (m.name != "<gap>" && m.name != "LiteralInt") {
                type_counts[std::string(m.name)]++;
            } else if (m.name == "LiteralInt") {
                type_counts["LiteralInt"]++;
            }
        }

        // Ensure tag names that might map to wrong tag due to gap sentinels
        // The gap entries use LiteralInt tag so they miscount; fix here.
        // Actually, the meta function handles this correctly — it returns
        // the meta for a specific tag, not for LiteralInt in general.
        // Re-scan using tag value directly:
        type_counts.clear();
        for (aura::ast::NodeId id = 0; id < total_nodes; ++id) {
            auto v = flat.get(id);
            switch (v.tag) {
                case aura::ast::NodeTag::LiteralInt:
                    type_counts["LiteralInt"]++;
                    break;
                case aura::ast::NodeTag::LiteralFloat:
                    type_counts["LiteralFloat"]++;
                    break;
                case aura::ast::NodeTag::LiteralString:
                    type_counts["LiteralString"]++;
                    break;
                case aura::ast::NodeTag::Variable:
                    type_counts["Variable"]++;
                    break;
                case aura::ast::NodeTag::Call:
                    type_counts["Call"]++;
                    break;
                case aura::ast::NodeTag::IfExpr:
                    type_counts["IfExpr"]++;
                    break;
                case aura::ast::NodeTag::Lambda:
                    type_counts["Lambda"]++;
                    break;
                case aura::ast::NodeTag::Let:
                    type_counts["Let"]++;
                    break;
                case aura::ast::NodeTag::LetRec:
                    type_counts["LetRec"]++;
                    break;
                case aura::ast::NodeTag::Define:
                    type_counts["Define"]++;
                    break;
                case aura::ast::NodeTag::Begin:
                    type_counts["Begin"]++;
                    break;
                case aura::ast::NodeTag::Set:
                    type_counts["Set"]++;
                    break;
                case aura::ast::NodeTag::Quote:
                    type_counts["Quote"]++;
                    break;
                case aura::ast::NodeTag::Pair:
                    type_counts["Pair"]++;
                    break;
                case aura::ast::NodeTag::Export:
                    type_counts["Export"]++;
                    break;
                case aura::ast::NodeTag::TypeAnnotation:
                    type_counts["TypeAnnotation"]++;
                    break;
                case aura::ast::NodeTag::Coercion:
                    type_counts["Coercion"]++;
                    break;
                case aura::ast::NodeTag::Linear:
                    type_counts["Linear"]++;
                    break;
                case aura::ast::NodeTag::Move:
                    type_counts["Move"]++;
                    break;
                case aura::ast::NodeTag::Borrow:
                    type_counts["Borrow"]++;
                    break;
                case aura::ast::NodeTag::MutBorrow:
                    type_counts["MutBorrow"]++;
                    break;
                case aura::ast::NodeTag::Drop:
                    type_counts["Drop"]++;
                    break;
                default:
                    type_counts["Other"]++;
                    break;
            }
        }

        // Get def-use index info if available
        std::uint64_t n_scopes = 0, n_defs = 0, n_uses = 0;
        if (auto stats = defuse_summary_stats()) {
            n_scopes = std::get<0>(*stats);
            n_defs = std::get<1>(*stats);
            n_uses = std::get<2>(*stats);
        }

        // Get mutation count
        auto n_mutations = flat.mutation_count();

        // Get source length (via current-source)
        std::uint64_t source_len = 0;
        auto src_fn = ev.primitives_.lookup("current-source");
        if (src_fn) {
            auto src = (*src_fn)({});
            if (is_string(src)) {
                auto sidx = as_string_idx(src);
                if (sidx < ev.string_heap_.size())
                    source_len = ev.string_heap_[sidx].size();
            }
        }

        // Build by-tag list: ((tag-name . count) ...)
        EvalValue by_tag_list = make_void();
        // Sort tags alphabetically for deterministic output
        std::vector<std::pair<std::string, std::uint64_t>> sorted_tags;
        for (auto& [name, count] : type_counts)
            sorted_tags.push_back({name, count});
        std::sort(sorted_tags.begin(), sorted_tags.end());
        for (auto it = sorted_tags.rbegin(); it != sorted_tags.rend(); ++it) {
            auto name_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(it->first);
            auto entry_pair = ev.pairs_.size();
            ev.pairs_.push_back(
                {make_string(name_idx), make_int(static_cast<std::int64_t>(it->second))});
            auto cons_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_pair(entry_pair), by_tag_list});
            by_tag_list = make_pair(cons_pair);
        }

        // Build full result as alist: ((:key value) ...)
        auto add_entry = [&](const std::string& key, EvalValue val) -> std::uint64_t {
            auto key_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(key);
            auto entry_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_string(key_idx), val});
            return entry_pair;
        };

        auto cvt = [&](std::uint64_t n) -> EvalValue {
            auto idx = ev.string_heap_.size();
            ev.string_heap_.push_back(std::to_string(n));
            return make_string(idx);
        };
        std::uint64_t entry_ids[7];
        entry_ids[0] = add_entry(":total-nodes", cvt(total_nodes));
        entry_ids[1] = add_entry(":mutation-count", cvt(n_mutations));
        entry_ids[2] = add_entry(":source-length", cvt(source_len));
        entry_ids[3] = add_entry(":by-tag", by_tag_list);
        entry_ids[4] = add_entry(":scopes", cvt(n_scopes));
        entry_ids[5] = add_entry(":defs", cvt(n_defs));
        entry_ids[6] = add_entry(":uses", cvt(n_uses));

        EvalValue result = make_void();
        for (int ei = 6; ei >= 0; --ei) {
            auto cons_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_pair(entry_ids[ei]), result});
            result = make_pair(cons_pair);
        }
        return result;
    });

    // (ast:validate-ownership) — Validate ownership invariants after mutations
    // Returns an alist: ((:pass true/false) (:notes (...)))
    // Each note has: (:node <id> :message <str> :kind <str>)
    add("ast:validate-ownership", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_flat_ || !ev.workspace_pool_) {
            // Return early with pass=true if no workspace
            return make_bool(true);
        }

        auto& flat = *ev.workspace_flat_;
        auto& pool = *ev.workspace_pool_;

        // Collect bindings in the AST that involve ownership operations
        std::unordered_set<std::string> ownership_bindings;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if ((v.tag == aura::ast::NodeTag::Move || v.tag == aura::ast::NodeTag::Borrow ||
                 v.tag == aura::ast::NodeTag::MutBorrow || v.tag == aura::ast::NodeTag::Drop) &&
                !v.children.empty()) {
                auto inner_v = flat.get(v.child(0));
                if (inner_v.tag == aura::ast::NodeTag::Variable) {
                    auto var_name = std::string(pool.resolve(inner_v.sym_id));
                    if (!var_name.empty())
                        ownership_bindings.insert(var_name);
                }
            }
        }

        if (ownership_bindings.empty()) {
            // Build entry manually (add_entry lambda is defined later)
            auto mk_entry = [&](const std::string& k, EvalValue v) -> std::uint64_t {
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                auto ep = ev.pairs_.size();
                ev.pairs_.push_back({make_string(kidx), v});
                return ep;
            };
            auto val_pass = mk_entry(":pass", make_bool(true));
            auto val_notes = mk_entry(":notes", make_void());
            uint64_t empty_ids[2] = {val_notes, val_pass};
            EvalValue empty_result = make_void();
            for (int ei = 1; ei >= 0; --ei) {
                auto cons_pair = ev.pairs_.size();
                ev.pairs_.push_back({make_pair(empty_ids[ei]), empty_result});
                empty_result = make_pair(cons_pair);
            }
            return empty_result;
        }

        std::vector<aura::compiler::OwnershipNote> notes;
        bool pass = aura::compiler::OwnershipEnv::validate_ownership(flat, pool, flat.root,
                                                                     ownership_bindings, notes);

        // Build result alist
        // ((:pass true/false) (:notes ((:node N :message M :kind K) ...)))
        auto add_entry = [&](const std::string& key, EvalValue val) -> std::uint64_t {
            auto key_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(key);
            auto entry_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_string(key_idx), val});
            return entry_pair;
        };

        // Build notes list
        EvalValue notes_list = make_void();
        for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
            auto& note = *it;
            auto node_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(std::to_string(note.node));
            auto msg_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(note.message);
            auto kind_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(note.kind);

            // Build note alist: ((:node N :message M :kind K))
            auto entry_node = add_entry(":node", make_string(node_idx));
            auto entry_msg = add_entry(":message", make_string(msg_idx));
            auto entry_kind = add_entry(":kind", make_string(kind_idx));

            // Chain as: (((:kind K) ((:message M) ((:node N) ()))) ())
            // Proper alist: ((:node N) (:message M) (:kind K))
            auto pair3 = ev.pairs_.size();
            ev.pairs_.push_back({make_string(kind_idx), make_void()});
            auto pair2 = ev.pairs_.size();
            ev.pairs_.push_back({make_string(msg_idx), make_pair(pair3)});
            auto pair1 = ev.pairs_.size();
            ev.pairs_.push_back({make_string(node_idx), make_pair(pair2)});

            auto cons_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_pair(pair1), notes_list});
            notes_list = make_pair(cons_pair);
        }

        auto entry_pass = add_entry(":pass", make_bool(pass));
        auto entry_notes = add_entry(":notes", notes_list);
        uint64_t entry_ids[2] = {entry_notes, entry_pass};
        EvalValue result = make_void();
        for (int ei = 1; ei >= 0; --ei) {
            auto cons_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_pair(entry_ids[ei]), result});
            result = make_pair(cons_pair);
        }
        return result;
    });

    // (ast:validate-nodes) — Validate all nodes against NodeMeta invariants
    // Returns an alist: ((:pass . #t/#f) (:total . N) (:errors . ((:node N :message M) ...)))
    add("ast:validate-nodes", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_flat_) {
            return make_bool(true);
        }

        auto& flat = *ev.workspace_flat_;
        std::vector<aura::ast::FlatAST::ValidationError> errors;
        auto count = flat.validate_all_nodes(errors);

        // Build alist: ((:pass . #t/#f) (:total . N) (:errors . ...))
        auto add_entry = [&](const std::string& key, EvalValue val) -> std::uint64_t {
            auto key_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(key);
            auto entry_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_string(key_idx), val});
            return entry_pair;
        };

        // Build errors list: ((:node N :message M) ...)
        EvalValue errors_list = make_void();
        for (auto it = errors.rbegin(); it != errors.rend(); ++it) {
            auto& e = *it;
            auto node_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(std::to_string(e.node));
            auto msg_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(e.message);

            auto pair_msg = ev.pairs_.size();
            ev.pairs_.push_back({make_string(msg_idx), make_void()});
            auto pair_node = ev.pairs_.size();
            ev.pairs_.push_back({make_string(node_idx), make_pair(pair_msg)});
            auto cons_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_pair(pair_node), errors_list});
            errors_list = make_pair(cons_pair);
        }

        auto val_pass = add_entry(":pass", make_bool(count == 0));
        auto val_total_k = ev.string_heap_.size();
        ev.string_heap_.push_back(":total");
        auto val_total_v = ev.string_heap_.size();
        ev.string_heap_.push_back(std::to_string(count));
        auto entry_total = ev.pairs_.size();
        ev.pairs_.push_back({make_string(val_total_k), make_string(val_total_v)});
        auto entry_errors = add_entry(":errors", errors_list);

        uint64_t eids[3] = {entry_errors, entry_total, val_pass};
        EvalValue result = make_void();
        for (int ei = 2; ei >= 0; --ei) {
            auto cons_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_pair(eids[ei]), result});
            result = make_pair(cons_pair);
        }
        return result;
    });
    // (ast:stable-ref node-id) — Issue #191: capture a StableNodeRef
    // (id + current generation) for the given node. Returns a
    // 2-element pair (id . gen) that can be passed to
    // (ast:ref-valid?) and (ast:ref-get). Use this in EDSL code
    // to safely hold a reference to a node across multiple
    // mutation / query calls.
    add("ast:stable-ref", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_void();
        if (!ev.workspace_flat_)
            return make_void();
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto ref = ev.workspace_flat_->make_ref(id);
        // Pack as (id . gen) pair
        std::size_t pid = ev.pairs_.size();
        ev.pairs_.push_back({make_int(static_cast<std::int64_t>(ref.id)),
                             make_int(static_cast<std::int64_t>(ref.gen))});
        return make_pair(pid);
    });
    // (ast:ref-valid? id gen) — Issue #191: returns #t if the
    // (id, gen) pair is still a valid stable reference. The ref
    // is valid iff:
    //   - id != NULL_NODE
    //   - id is in-bounds
    //   - id's node_gen_ matches gen
    //   - FlatAST's current generation matches gen
    // (the last check is what catches "a structural mutation
    // happened between capture and use" — even if the slot
    // still exists, the structural relationship has changed).
    add("ast:ref-valid?", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1]))
            return make_bool(false);
        if (!ev.workspace_flat_)
            return make_bool(false);
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto gen = static_cast<std::uint16_t>(as_int(a[1]));
        return make_bool(ev.workspace_flat_->is_valid(aura::ast::FlatAST::StableNodeRef{id, gen}));
    });
    // (ast:ref-get id gen) — Issue #191: safely get a node
    // from a stable reference. Returns the node as a value if
    // the ref is valid; void if stale. Useful for EDSL code
    // that wants to query a node it stored earlier without
    // crashing on stale refs.
    //
    // For now, the returned value is a placeholder string
    // containing the node's tag name (so callers can at least
    // see "this is what kind of node it was" even if they
    // can't get the full NodeView through Aura values).
    add("ast:ref-get", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1]))
            return make_void();
        if (!ev.workspace_flat_)
            return make_void();
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto gen = static_cast<std::uint16_t>(as_int(a[1]));
        auto opt = ev.workspace_flat_->get_safe(aura::ast::FlatAST::StableNodeRef{id, gen});
        if (!opt)
            return make_void();
        // Return the tag name as a string for observability
        std::string tag_name = "?";
        switch (opt->tag) {
            case aura::ast::NodeTag::LiteralInt:
                tag_name = "LiteralInt";
                break;
            case aura::ast::NodeTag::LiteralFloat:
                tag_name = "LiteralFloat";
                break;
            case aura::ast::NodeTag::LiteralString:
                tag_name = "LiteralString";
                break;
            case aura::ast::NodeTag::Variable:
                tag_name = "Variable";
                break;
            case aura::ast::NodeTag::Call:
                tag_name = "Call";
                break;
            case aura::ast::NodeTag::IfExpr:
                tag_name = "IfExpr";
                break;
            case aura::ast::NodeTag::Lambda:
                tag_name = "Lambda";
                break;
            case aura::ast::NodeTag::Let:
                tag_name = "Let";
                break;
            case aura::ast::NodeTag::LetRec:
                tag_name = "LetRec";
                break;
            case aura::ast::NodeTag::Define:
                tag_name = "Define";
                break;
            case aura::ast::NodeTag::Begin:
                tag_name = "Begin";
                break;
            case aura::ast::NodeTag::Set:
                tag_name = "Set";
                break;
            case aura::ast::NodeTag::Quote:
                tag_name = "Quote";
                break;
            case aura::ast::NodeTag::MacroDef:
                tag_name = "MacroDef";
                break;
            default:
                tag_name = "Node";
                break;
        }
        std::string s = std::string("<node:") + tag_name + " id=" + std::to_string(id) + ">";
        std::size_t sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(s);
        return types::make_string(sidx);
    });

    // Issue #291: (ast:ref-mutation-id id gen [mutation_id workspace_id])
    // — extract the mutation_id_at_capture from a serialized
    // StableNodeRef. Returns the integer; #f if the buffer
    // doesn't match the #291 magic. The serialized form is the
    // 4-tuple (id gen mutation_id workspace_id); callers
    // unpack and pass the mutation_id directly.
    add("ast:ref-mutation-id", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 4 || !is_int(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !is_int(a[3]))
            return make_bool(false);
        return make_int(as_int(a[2]));
    });

    // Issue #291: (ast:ref-workspace-id id gen mutation_id workspace_id)
    // — extract the workspace_id (WorkspaceTree layer index)
    // from a serialized StableNodeRef. Returns the integer.
    add("ast:ref-workspace-id", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 4 || !is_int(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !is_int(a[3]))
            return make_bool(false);
        return make_int(as_int(a[3]));
    });

    // Issue #291: (ast:ref-serialize id gen mutation_id workspace_id)
    // — pack a StableNodeRef into a 24-byte blob. Returns the
    // blob as a string (the caller persists it however they
    // like — base64, file, etc.). Returns #f on bad args.
    // The blob format is fixed-size (see kStableRefSerializedSize)
    // and includes a 4-byte magic header so
    // (ast:ref-deserialize) can reject pre-#291 buffers.
    add("ast:ref-serialize", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 4 || !is_int(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !is_int(a[3]))
            return make_bool(false);
        if (!ev.workspace_flat_)
            return make_bool(false);
        aura::ast::FlatAST::StableNodeRef ref{};
        ref.id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        ref.gen = static_cast<std::uint16_t>(as_int(a[1]));
        ref.mutation_id_at_capture = static_cast<std::uint64_t>(as_int(a[2]));
        ref.workspace_id = static_cast<std::uint32_t>(as_int(a[3]));
        std::uint8_t buf[aura::ast::FlatAST::kStableRefSerializedSize];
        auto n = ev.workspace_flat_->serialize_stable_ref(ref, buf);
        std::string s(reinterpret_cast<const char*>(buf), n);
        std::size_t sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(s);
        return types::make_string(sidx);
    });

    // Issue #291: (ast:ref-deserialize string) — unpack a
    // 24-byte blob back to a 4-tuple (id gen mutation_id
    // workspace_id). The tuple is returned as a pair of pairs
    // ((id . gen) . (mutation_id . workspace_id)) for Aura
    // pair-list consumption. Returns #f if the magic doesn't
    // match or the buffer is the wrong size.
    add("ast:ref-deserialize", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        if (!ev.workspace_flat_)
            return make_bool(false);
        auto sidx = as_string_idx(a[0]);
        if (sidx >= ev.string_heap_.size())
            return make_bool(false);
        const std::string& blob = ev.string_heap_[sidx];
        if (blob.size() < aura::ast::FlatAST::kStableRefSerializedSize)
            return make_bool(false);
        aura::ast::FlatAST::StableNodeRef ref{};
        if (!ev.workspace_flat_->deserialize_stable_ref(
                reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size(), ref))
            return make_bool(false);
        // Return as a nested pair: ((id . gen) . (mut_id . ws_id))
        auto pair_id_gen = ev.pairs_.size();
        ev.pairs_.push_back({make_int(static_cast<std::int64_t>(ref.id)),
                             make_int(static_cast<std::int64_t>(ref.gen))});
        auto pair_mut_ws = ev.pairs_.size();
        ev.pairs_.push_back({make_int(static_cast<std::int64_t>(ref.mutation_id_at_capture)),
                             make_int(static_cast<std::int64_t>(ref.workspace_id))});
        auto outer = ev.pairs_.size();
        ev.pairs_.push_back({make_pair(pair_id_gen), make_pair(pair_mut_ws)});
        return make_pair(outer);
    });
    // (ast:stable-refs-valid? refs) — Issue #278: bulk validity
    // check for a list of (id . gen) stable-ref pairs. Returns a
    // list of booleans (#t #f #t ...) in the same order as the
    // input. Useful for AI agent code that has captured a batch
    // of stable-refs (e.g. across a long query→mutate cycle) and
    // wants to quickly check which ones are still valid before
    // using any of them.
    //
    //   (ast:stable-refs-valid? '((1 . 5) (2 . 5) (3 . 6)))
    //   → (#t #t #f)   ; if gen 6 was bumped by a recent mutation
    //
    // Each input element must be a (id . gen) pair where both car
    // and cdr are integers (the shape returned by ast:stable-ref).
    // Malformed entries yield #f. Empty input returns ().
    add("ast:stable-refs-valid?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !ev.workspace_flat_)
            return make_void();
        auto& flat = *ev.workspace_flat_;
        EvalValue result = make_void();
        // Walk the input list. The list is structured as a chain of
        // cons cells: each cons cell has car=element, cdr=next cons
        // cell (or void for end-of-list). We process elements in
        // forward order by collecting them in a vector first, then
        // cons-ing the results in reverse.
        std::vector<EvalValue> elements;
        EvalValue cur = a[0];
        while (is_pair(cur)) {
            auto cons_idx = as_pair_idx(cur);
            auto& cons = ev.pairs_[cons_idx];
            // cons.car is the current element; cons.cdr is the rest.
            elements.push_back(cons.car);
            cur = cons.cdr;
        }
        // Build the result list by cons-ing in reverse (so order is preserved).
        for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
            EvalValue valid_ev = make_bool(false);
            // The element should be a (id . gen) pair where both
            // car and cdr are integers. If it's anything else
            // (e.g. a non-pair, or a pair of strings), yield #f.
            if (is_pair(*it)) {
                auto ref_idx = as_pair_idx(*it);
                auto& ref = ev.pairs_[ref_idx];
                if (is_int(ref.car) && is_int(ref.cdr)) {
                    auto id = static_cast<aura::ast::NodeId>(as_int(ref.car));
                    auto gen = static_cast<std::uint16_t>(as_int(ref.cdr));
                    valid_ev = make_bool(flat.is_valid(aura::ast::FlatAST::StableNodeRef{id, gen}));
                }
            }
            auto cons_pair = ev.pairs_.size();
            ev.pairs_.push_back({valid_ev, result});
            result = make_pair(cons_pair);
        }
        return result;
    });
    // (ast:generation) — Issue #191: return the current
    // generation counter. Used to inspect when a structural
    // mutation has happened (compare the returned value to
    // a previously-captured one).
    add("ast:generation", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_flat_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->generation()));
    });

    // Issue #261: NodeId lifecycle primitives for long-running
    // AI query→mutate→eval loops.

    // (ast:recycle-nodes) — mark unreachable slots free for reuse.
    add("ast:recycle-nodes", [&ev](const auto&) -> EvalValue {
        std::unique_lock<std::shared_mutex> wlock(ev.workspace_mtx_);
        if (!ev.workspace_flat_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->recycle_dead_nodes()));
    });

    // (ast:compact-nodes) — densify live nodes into 0..n-1 slots.
    add("ast:compact-nodes", [&ev](const auto&) -> EvalValue {
        std::unique_lock<std::shared_mutex> wlock(ev.workspace_mtx_);
        if (!ev.workspace_flat_)
            return make_int(0);
        const auto reclaimed = ev.workspace_flat_->compact_nodes();
        // Issue #490: compact remaps NodeIds — drop stale Evaluator
        // index entries, then optionally eager-rebuild per policy.
        ev.invalidate_tag_arity_index();
        ev.maybe_eager_rebuild_pattern_index_after_cow();
        return make_int(static_cast<std::int64_t>(reclaimed));
    });

    // (ast:validate-post-restore) — Issue #263: check generation_ /
    // node_gen_ / parent-child span consistency. Returns a hash with
    // violations, generation, live-nodes, free-slots.
    add("ast:validate-post-restore", [&ev](const auto&) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        if (!ev.workspace_flat_)
            return make_void();
        auto report = ev.workspace_flat_->validate_post_restore();
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"violations", make_int(static_cast<std::int64_t>(report.violations))},
            {"generation", make_int(static_cast<std::int64_t>(report.generation))},
            {"live-nodes", make_int(static_cast<std::int64_t>(report.live_nodes))},
            {"free-slots", make_int(static_cast<std::int64_t>(report.free_slots))},
        };
        return ev.build_ast_lifecycle_hash(kv);
    });

    // (ast:post-restore-stats) — Issue #263: result of the most recent
    // ast:restore direct-path validation (0 violations = consistent).
    add("ast:post-restore-stats", [&ev](const auto&) -> EvalValue {
        auto& r = ev.last_post_restore_report_;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"violations", make_int(static_cast<std::int64_t>(r.violations))},
            {"generation", make_int(static_cast<std::int64_t>(r.generation))},
            {"live-nodes", make_int(static_cast<std::int64_t>(r.live_nodes))},
            {"free-slots", make_int(static_cast<std::int64_t>(r.free_slots))},
        };
        return ev.build_ast_lifecycle_hash(kv);
    });

    // (ast:node-lifecycle-stats) — fragmentation + counter hash.
    add("ast:node-lifecycle-stats", [&ev](const auto&) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        if (!ev.workspace_flat_)
            return make_void();
        auto stats = ev.workspace_flat_->node_lifecycle_stats();
        auto frag_bp = static_cast<std::int64_t>(stats.fragmentation_ratio * 10000.0);
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"total-slots", make_int(static_cast<std::int64_t>(stats.total_slots))},
            {"live-nodes", make_int(static_cast<std::int64_t>(stats.live_nodes))},
            {"free-slots", make_int(static_cast<std::int64_t>(stats.free_slots))},
            {"fragmentation-ratio-bp", make_int(frag_bp)},
            {"recycle-total",
             make_int(static_cast<std::int64_t>(ev.workspace_flat_->node_recycle_total()))},
            {"slot-reuse-count",
             make_int(static_cast<std::int64_t>(ev.workspace_flat_->node_slot_reuse_count()))},
            {"compact-total",
             make_int(static_cast<std::int64_t>(ev.workspace_flat_->node_compact_total()))},
        };
        return ev.build_ast_lifecycle_hash(kv);
    });
}

} // namespace aura::compiler::primitives_detail
