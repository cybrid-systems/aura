// evaluator_primitives_compile_03.cpp — Issue #909: peeled compile registration
// aura.compiler.evaluator module partition.

module;

#include "runtime_shared.h"
#include "observability_metrics.h"
#include "per_defuse_index.h"
#include "hash_meta.h"
#include "basis_points.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.mutators;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.value;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.pass_manager;
import aura.compiler.query;
import aura.compiler.hardware_backend;
import aura.compiler.sv_ir;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_keyword;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

// Issue #909 compile part 24 (orig 1889-1947)
void CompilePrims::register_compile_p24(PrimRegistrar add, Evaluator& ev) {

    // (query:dirty-nodes :reason "X") — Issue #344:
    // returns a pair-list of NodeIds that have the
    // target DirtyReason bit set. X is one of:
    //   "general" | "constraint" | "occurrence" |
    //   "ownership" | "coercion" | "struct" |
    //   "defuse" | "ppa-hint"
    // The pair-list is in NodeId order (smallest
    // first) so the caller can iterate with
    // (car / cdr) or take (length ...) of the
    // sublist. Returns the empty list when no
    // workspace is loaded or the reason is
    // unknown.
    add("query:dirty-nodes", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        const auto& reason_name = ev.string_heap_[idx];
        std::uint8_t mask = 0;
        if (reason_name == "general")
            mask = 0x01;
        else if (reason_name == "constraint")
            mask = 0x02;
        else if (reason_name == "occurrence")
            mask = 0x04;
        else if (reason_name == "ownership")
            mask = 0x08;
        else if (reason_name == "coercion")
            mask = 0x10;
        else if (reason_name == "struct")
            mask = 0x20;
        else if (reason_name == "defuse")
            mask = 0x40;
        else if (reason_name == "ppa-hint")
            mask = 0x80;
        else
            return make_void();
        // Walk the dirty column; collect NodeIds
        // with the target bit set. Returns the
        // pair-list in NodeId order (smallest first).
        const auto view = ws->dirty_view();
        EvalValue list = make_void();
        for (std::size_t id = view.size(); id-- > 0;) {
            if (view[id] & mask) {
                auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back(std::to_string(id));
                auto p_idx = ev.pairs_.size();
                Pair tmp{make_string(sidx), list};
                ev.pairs_.push_back(std::move(tmp));
                list = make_pair(p_idx);
            }
        }
        return list;
    });
}

// Issue #909 compile part 25 (orig 1948-1999)
void CompilePrims::register_compile_p25(PrimRegistrar add, Evaluator& ev) {

    // (compile:dep-edges) — Issue #196: total number of edges
    // in the dep_graph_ map. Each edge means "this define
    // depends on that define"; mutations to the target cascade
    // to invalidate the source via invalidate_function BFS.
    // Returns 0 if no hook is installed.
    add("compile:dep-edges", [&ev](const auto&) -> EvalValue {
        if (!ev.get_incremental_stats_fn_)
            return make_int(0);
        auto packed = ev.get_incremental_stats_fn_();
        return make_int(static_cast<std::int64_t>(packed & 0xFFFF));
    });

    // (compile:block-dirty-count name) — Issue #196: total
    // number of dirty blocks across all functions in the
    // named define's IR cache entry. Returns 0 if no hook
    // is installed or the entry doesn't exist. Use case:
    // an EDSL agent can measure "did the previous mutation
    // actually re-lower anything?" by reading this primitive
    // before and after a mutation cycle.
    add("compile:block-dirty-count", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(0);
        if (!ev.get_dirty_block_count_fn_)
            return make_int(0);
        return make_int(
            static_cast<std::int64_t>(ev.get_dirty_block_count_fn_(ev.string_heap_[idx].c_str())));
    });

    // (compile:func-block-dirty-count name func-idx) —
    // Issue #196: dirty block count for a specific function
    // in the named define's IR cache entry. Returns 0 if
    // no hook, the entry doesn't exist, or func-idx is
    // out of range.
    add("compile:func-block-dirty-count", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1])) {
            return make_int(0);
        }
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(0);
        auto fidx = as_int(a[1]);
        if (fidx < 0)
            return make_int(0);
        if (!ev.get_func_dirty_block_count_fn_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev.get_func_dirty_block_count_fn_(
            ev.string_heap_[idx].c_str(), static_cast<std::size_t>(fidx))));
    });
}

// Issue #909 compile part 26 (orig 2000-2072)
void CompilePrims::register_compile_p26(PrimRegistrar add, Evaluator& ev) {

    // (compile:block-dirty? name func-idx block-idx) —
    // Issue #196: returns #t if the specific (function,
    // block) is dirty in the named define's IR cache entry.
    // Returns #f otherwise. Use case: fine-grained
    // "did THIS block change?" query for the smarter
    // re-lower (Phase 5 follow-up).
    add("compile:block-dirty?", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 3 || !is_string(a[0]) || !is_int(a[1]) || !is_int(a[2])) {
            return make_bool(false);
        }
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        auto fidx = as_int(a[1]);
        auto bidx = as_int(a[2]);
        if (fidx < 0 || bidx < 0)
            return make_bool(false);
        if (!ev.is_block_dirty_fn_)
            return make_bool(false);
        return make_bool(ev.is_block_dirty_fn_(ev.string_heap_[idx].c_str(),
                                               static_cast<std::size_t>(fidx),
                                               static_cast<std::uint32_t>(bidx)));
    });

    // (compile:mark-block-dirty! name func-idx block-idx) —
    // Issue #196: fine-grained mark a single (function, block)
    // dirty in the named define's IR cache entry. Returns
    // #t on success, #f if the entry doesn't exist or the
    // hook is not installed. Use case: the smarter
    // re-lower (Phase 5 follow-up) marks only the affected
    // blocks rather than the whole entry.
    add("compile:mark-block-dirty!", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 3 || !is_string(a[0]) || !is_int(a[1]) || !is_int(a[2])) {
            return make_bool(false);
        }
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        auto fidx = as_int(a[1]);
        auto bidx = as_int(a[2]);
        if (fidx < 0 || bidx < 0)
            return make_bool(false);
        if (!ev.mark_block_dirty_fn_)
            return make_bool(false);
        return make_bool(ev.mark_block_dirty_fn_(ev.string_heap_[idx].c_str(),
                                                 static_cast<std::size_t>(fidx),
                                                 static_cast<std::uint32_t>(bidx)));
    });

    // (compile:clear-block-dirty! name func-idx block-idx) —
    // Issue #196: clear a single (function, block) dirty bit
    // in the named define's IR cache entry. Returns #t on
    // success, #f if the entry doesn't exist or the hook is
    // not installed. Use case: the smarter re-lower clears
    // the dirty bit after re-lowering a block.
    add("compile:clear-block-dirty!", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 3 || !is_string(a[0]) || !is_int(a[1]) || !is_int(a[2])) {
            return make_bool(false);
        }
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        auto fidx = as_int(a[1]);
        auto bidx = as_int(a[2]);
        if (fidx < 0 || bidx < 0)
            return make_bool(false);
        if (!ev.clear_block_dirty_fn_)
            return make_bool(false);
        return make_bool(ev.clear_block_dirty_fn_(ev.string_heap_[idx].c_str(),
                                                  static_cast<std::size_t>(fidx),
                                                  static_cast<std::uint32_t>(bidx)));
    });
}

// Issue #909 compile part 27 (orig 2073-2131)
void CompilePrims::register_compile_p27(PrimRegistrar add, Evaluator& ev) {

    // Issue #460: (compile:is-instruction-dirty? name func-idx
    // block-idx instr-idx) — per-instruction dirty query
    // (mirrors is-block-dirty? for the per-instruction level).
    // Returns #t if (i, b, k) is marked dirty in the named
    // define's IR cache entry, #f otherwise.
    add("compile:is-instruction-dirty?", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 4 || !is_string(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !is_int(a[3]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        if (!ev.is_instruction_dirty_fn_)
            return make_bool(false);
        return make_bool(ev.is_instruction_dirty_fn_(
            ev.string_heap_[idx].c_str(), static_cast<std::size_t>(as_int(a[1])),
            static_cast<std::uint32_t>(as_int(a[2])), static_cast<std::uint32_t>(as_int(a[3]))));
    });

    // Issue #460: (compile:mark-instruction-dirty! name
    // func-idx block-idx instr-idx) — per-instruction dirty
    // marker. Returns #t on success, #f if no hook.
    add("compile:mark-instruction-dirty!", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 4 || !is_string(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !is_int(a[3]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        if (!ev.mark_instruction_dirty_fn_)
            return make_bool(false);
        return make_bool(ev.mark_instruction_dirty_fn_(
            ev.string_heap_[idx].c_str(), static_cast<std::size_t>(as_int(a[1])),
            static_cast<std::uint32_t>(as_int(a[2])), static_cast<std::uint32_t>(as_int(a[3]))));
    });

    // Issue #460: (compile:clear-instruction-dirty! name
    // func-idx block-idx instr-idx) — per-instruction clear.
    add("compile:clear-instruction-dirty!", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 4 || !is_string(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !is_int(a[3]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        if (!ev.clear_instruction_dirty_fn_)
            return make_bool(false);
        return make_bool(ev.clear_instruction_dirty_fn_(
            ev.string_heap_[idx].c_str(), static_cast<std::size_t>(as_int(a[1])),
            static_cast<std::uint32_t>(as_int(a[2])), static_cast<std::uint32_t>(as_int(a[3]))));
    });

    // Issue #460: (query:compiler-incremental-stats) — return
    // the current partial-relower / impact-scope counters.
    // P0 ship: returns the partial_relower_count as an int.
    // Follow-up: returns a 3-tuple
    // (partial-relower impact-scope-calls total-affected-blocks).
    add("query:compiler-incremental-stats", [&ev](const auto& a) -> EvalValue {
        (void)a;
        return make_int(static_cast<std::int64_t>(ev.get_partial_relower_count()));
    });
}

// Issue #909 compile part 28 (orig 2132-2233)
void CompilePrims::register_compile_p28(PrimRegistrar add, Evaluator& ev) {

    // Issue #426 / #293: (query:compiler-cache-stats) —
    // return the current per-block dirty summary across the
    // compiler cache as a 3-tuple
    // (total-dirty-blocks total-dirty-functions
    //  incremental-candidates). The "incremental
    // candidates" count is the number of functions whose
    // dirty block count falls in [1..7] (per
    // estimate_relower_blocks); 8+ dirty blocks is
    // "full re-lower" territory and 0 is "clean".
    //
    // AI Agents can use the 3-tuple to decide whether the
    // next (compile:relower) should be incremental
    // (incremental-candidates > 0) or full (otherwise).
    add("query:compiler-cache-stats", [&ev](const auto& a) -> EvalValue {
        (void)a;
        auto* svc_void = ev.compiler_service();
        if (!svc_void)
            return make_int(0);
        auto* svc = static_cast<aura::compiler::CompilerService*>(svc_void);
        // Build 3-tuple as nested pair-of-pairs:
        // ((dirty-blocks . dirty-functions) . incremental-candidates)
        std::int64_t dirty_blocks = static_cast<std::int64_t>(svc->total_dirty_block_count());
        std::int64_t dirty_funcs = static_cast<std::int64_t>(svc->total_dirty_func_count());
        std::int64_t incr_cands = static_cast<std::int64_t>(svc->total_incremental_candidates());
        auto p1 = ev.pairs_.size();
        ev.pairs_.push_back({make_int(dirty_blocks), make_int(dirty_funcs)});
        auto outer = ev.pairs_.size();
        ev.pairs_.push_back({make_pair(p1), make_int(incr_cands)});
        return make_pair(outer);
    });

    // Issue #298: (query:incremental-effectiveness) — return a
    // 4-tuple aggregating compiler-pipeline observability
    // metrics for self-evolution loops. The 4 elements:
    //
    //   1. recompile-ratio: dirty-defines / total-defines (×10000
    //      for basis-point precision). 0 = no dirty defines, 10000
    //      = all dirty. Ratio > 5000 indicates a wide
    //      invalidation; agents should investigate mutation scope.
    //
    //   2. cascade-depth: max mark_dirty_upward depth seen in the
    //      last eval cycle. Deeper cascades indicate mutations that
    //      ripple through many parents. Used to detect
    //      unexpectedly wide invalidation chains.
    //
    //   3. bridge-overhead: total closure_bridge_calls (sum of
    //      bridge invocations across all defines). Use this to
    //      quantify how often the IR ↔ tree-walker fallback path
    //      is exercised.
    //
    //   4. fallback-frequency: count of fallback triggers (sum
    //      over all reasons: special forms, EDSL primitives,
    //      macros). 0 = pure IR path. High counts indicate the
    //      mutation touched a tree-walker-only construct.
    //
    // The 4 elements are returned as a flat 4-tuple
    // (e1 . (e2 . (e3 . e4))) so callers can destructure with
    // standard Aura car/cdr. All values are int (basis points
    // for ratio, raw counts for the rest).
    add("query:incremental-effectiveness", [&ev](const auto& a) -> EvalValue {
        (void)a;
        auto* svc_void = ev.compiler_service();
        if (!svc_void)
            return make_int(0);
        auto* svc = static_cast<aura::compiler::CompilerService*>(svc_void);

        std::int64_t ratio_bp = 0;
        std::int64_t cascade_depth = 0;
        std::int64_t bridge_overhead = 0;
        std::int64_t fallback_freq = 0;

        // Read the latest snapshot. Wrapped in try/catch so a
        // service-side throw doesn't propagate as an
        // unhandled error value.
        try {
            auto snap = svc->snapshot();
            auto total_defines = svc->ir_cache_v2_size();
            auto dirty_funcs = svc->total_dirty_func_count();
            if (total_defines > 0) {
                ratio_bp = (dirty_funcs * ::aura::compiler::kBasisPointScale) /
                           static_cast<std::int64_t>(total_defines);
            }
            cascade_depth = static_cast<std::int64_t>(snap.mark_dirty_total_nodes);
            bridge_overhead = static_cast<std::int64_t>(snap.closure_bridge_calls);
            fallback_freq =
                static_cast<std::int64_t>(snap.closure_tw_calls + snap.closure_ffi_calls);
        } catch (...) {
            // [SILENCE-PRIM-#615] Service-side metric snapshot
            // failure; zeros are already initialized.
        }

        // Build 4-tuple as nested pairs (right-associated):
        // (ratio . (cascade . (bridge . fallback)))
        auto p1 = ev.pairs_.size();
        ev.pairs_.push_back({make_int(bridge_overhead), make_int(fallback_freq)});
        auto p2 = ev.pairs_.size();
        ev.pairs_.push_back({make_int(cascade_depth), make_pair(p1)});
        auto p3 = ev.pairs_.size();
        ev.pairs_.push_back({make_int(ratio_bp), make_pair(p2)});
        return make_pair(p3);
    });
}

// Issue #909 compile part 29 (orig 2234-2291)
void CompilePrims::register_compile_p29(PrimRegistrar add, Evaluator& ev) {

    // Issue #293: (compile:relower-strategy <function-name>)
    // — returns a symbol describing the optimal re-lower
    // strategy for a cached function:
    //   'none — function is clean (0 dirty blocks)
    //   'incremental — 1..7 dirty blocks (per
    //     estimate_relower_blocks), targeted re-lower is
    //     cheaper than full
    //   'full — 8+ dirty blocks, full re-lower is on par
    //     with incremental (no point doing fine-grained work)
    //   'unknown — function not in the cache
    //
    // The 'full' threshold is conservative (8+ blocks is
    // half of the typical function size); agents that need
    // a different threshold can use the
    // query:compiler-cache-stats 3-tuple and decide
    // themselves. This primitive is the convenient default.
    add("compile:relower-strategy", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        auto* svc_void = ev.compiler_service();
        if (!svc_void)
            return make_bool(false);
        auto* svc = static_cast<aura::compiler::CompilerService*>(svc_void);
        const std::string& fname = ev.string_heap_[idx];
        // Look up the entry in ir_cache_v2_
        auto it = svc->ir_cache_v2_find(fname);
        if (!it) {
            // Function not in cache — return 'unknown symbol
            auto sym_idx = ev.keyword_table_.size();
            ev.keyword_table_.push_back("unknown");
            return make_keyword(sym_idx);
        }
        std::size_t dirty = it->dirty_block_count();
        const char* tag = nullptr;
        if (dirty == 0)
            tag = "none";
        else if (dirty < 8)
            tag = "incremental";
        else
            tag = "full";
        auto sym_idx = ev.keyword_table_.size();
        ev.keyword_table_.push_back(tag);
        return make_keyword(sym_idx);
    });

    // Issue #459: (query:atomic-batch-stats) — return
    // the current nested-atomic-batch observability counters.
    // P0 ship: returns the atomic_batch_steal_violation_
    // count as an int. Follow-up: returns a 2-tuple
    // (steal-violations gc-bumps-lost).
    add("query:atomic-batch-stats", [&ev](const auto& a) -> EvalValue {
        (void)a;
        return make_int(static_cast<std::int64_t>(ev.get_atomic_batch_steal_violation()));
    });
}

// Issue #909 compile part 30 (orig 2292-2346)
void CompilePrims::register_compile_p30(PrimRegistrar add, Evaluator& ev) {

    // Issue #437: (verify:assertion-failed node-id
    // [assert-name-string]) — Mark the given AST node with
    // the kAssertionDirty verify bit. The optional 2nd arg
    // is the assertion name (string, for observability
    // only — the P0 ship doesn't propagate the name into
    // the dirty bitmask). Returns the new verify_dirty
    // bitmask for the node (0 if no bits set after apply,
    // or the bitmask). On any failure (bad args, no
    // workspace) returns #f.
    add("verify:assertion-failed", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        auto node_id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (node_id >= ws->size())
            return make_bool(false);
        ws->apply_verify_dirty_bits(node_id, aura::ast::FlatAST::kAssertionDirty);
        return make_int(static_cast<std::int64_t>(ws->verify_dirty(node_id)));
    });

    // Issue #437: (verify:report-coverage node-id
    // [coverage-hole-name-string]) — Mark the given AST
    // node with the kCoverageDirty verify bit. Same
    // signature + return-value convention as
    // verify:assertion-failed.
    add("verify:report-coverage", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        auto node_id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (node_id >= ws->size())
            return make_bool(false);
        ws->apply_verify_dirty_bits(node_id, aura::ast::FlatAST::kCoverageDirty);
        return make_int(static_cast<std::int64_t>(ws->verify_dirty(node_id)));
    });

    // Issue #437: (query:verify-dirty-stats) — return
    // the current per-reason verify-dirty counters as
    // a 4-tuple (assertion coverage sva formal-cex).
    // P0 ship: returns an integer = sum of all four
    // counters. Follow-up: returns the 4-tuple.
    add("query:verify-dirty-stats", [&ev](const auto& a) -> EvalValue {
        (void)a;
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_int(0);
        auto sum = ws->verify_assertion_dirty_total() + ws->verify_coverage_dirty_total() +
                   ws->verify_sva_dirty_total() + ws->verify_formal_cex_dirty_total();
        return make_int(static_cast<std::int64_t>(sum));
    });
}

// Issue #909 compile part 31 (orig 2347-2403)
void CompilePrims::register_compile_p31(PrimRegistrar add, Evaluator& ev) {

    // Issue #437: (compile:verify-dirty? node-id) — query
    // the verify_dirty_ bitmask for a node. Returns the
    // bitmask (0 if not set or no bits). #f on bad args.
    add("compile:verify-dirty?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        auto node_id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (node_id >= ws->size())
            return make_bool(false);
        return make_int(static_cast<std::int64_t>(ws->verify_dirty(node_id)));
    });

    // Issue #290: helper for the macro_dirty_ primitives.
    // The macro_dirty_ column lives on the flat where
    // macro_expand_all / clone_macro_body actually run — that
    // is the per-eval current flat (current_flat_), NOT the
    // persistent workspace_flat_. The workspace holds the
    // pre-expansion source; the eval flat holds the cloned
    // result. Re-resolve each call so we see the most recent
    // eval flat (the lambda captures the helper by value, so
    // the helper itself is fine, but it forwards to live
    // ev.current_flat() / ev.workspace_flat() at every call).
    // Issue #290: (compile:macro-dirty? node-id) — query the
    // macro_dirty_ bitmask for a node. Returns the bitmask
    // (0 if not set or no bits). #f on bad args.
    // Bit 0 = kMacroExpansion (cloned by clone_macro_body),
    // bit 1 = kMacroSelfModify (touched by a self-evolution
    // step).
    add("compile:macro-dirty?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = CompilePrims::pick_macro_flat(ev);
        if (!ws)
            return make_bool(false);
        auto node_id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (node_id >= ws->size())
            return make_bool(false);
        return make_int(static_cast<std::int64_t>(ws->macro_dirty(node_id)));
    });

    // Issue #290: (compile:macro-dirty-count) — number of
    // nodes with any macro_dirty_ bit set on the eval flat
    // where macro expansion actually ran. #f on no flat.
    add("compile:macro-dirty-count", [&ev](const auto&) -> EvalValue {
        auto* ws = CompilePrims::pick_macro_flat(ev);
        if (!ws)
            return make_bool(false);
        return make_int(static_cast<std::int64_t>(ws->macro_dirty_count()));
    });
}

} // namespace aura::compiler::primitives_detail
