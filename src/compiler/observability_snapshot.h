// observability_snapshot.h — POD snapshot of observability state
// (Issue #62 Iter 3). The atomic fields in CompilerMetrics are
// not reflect-friendly (the framework's template only handles
// built-in types). The snapshot is a plain-POD copy, populated
// on demand by CompilerService::snapshot(). Then auto_to_json
// serializes it cleanly.

#ifndef AURA_COMPILER_OBSERVABILITY_SNAPSHOT_H
#define AURA_COMPILER_OBSERVABILITY_SNAPSHOT_H

#include <cstdint>
#include <string>
#include <vector>

#include "observability_metrics.h" // for FnMetrics

namespace aura::compiler {

// Plain-POD snapshot of CompilerMetrics. Populated by atomic loads.
struct CompilerSnapshot {
    std::uint64_t deopt_count = 0;
    std::uint64_t specialization_hits = 0;
    std::uint64_t specialization_misses = 0;
    std::uint64_t shape_changes_observed = 0;
    std::uint64_t jit_compilations = 0;
    std::uint64_t jit_compile_misses = 0;
    std::uint64_t jit_cache_evictions = 0;
    std::uint64_t aot_emits = 0;
    std::uint64_t aot_fallbacks = 0;
    std::uint64_t arena_bytes_used = 0;
    std::uint64_t arena_bytes_peak = 0;
    // Issue #250: atomic-batch observability. atomics are
    // loaded with relaxed order in CompilerService::snapshot().
    // - atomic_batch_count: total successful batches
    // - atomic_batch_ops_total: total ops across all batches
    // - atomic_batch_rollbacks: total rollbacks
    // - atomic_batch_bumps_saved_total: how many per-op
    //   generation bumps the batches suppressed (lifetime)
    std::uint64_t atomic_batch_count = 0;
    std::uint64_t atomic_batch_ops_total = 0;
    std::uint64_t atomic_batch_rollbacks = 0;
    std::uint64_t atomic_batch_bumps_saved_total = 0;
    // Issue #252: closure dual-path observability. Counters
    // for the 3 dispatch paths in apply_closure + stale-bridge
    // returns (Issue #223). The fast-path optimization in
    // #252 follow-ups will be measured by changes in these.
    // - closure_calls_total: every apply_closure call
    // - closure_ffi_calls: FFI-dispatched
    // - closure_tw_calls: tree-walker closures_ map hit
    // - closure_bridge_calls: closure_bridge_ (IR/JIT)
    // - closure_stale_returns: stale-bridge nullopt returns
    std::uint64_t closure_calls_total = 0;
    std::uint64_t closure_ffi_calls = 0;
    std::uint64_t closure_tw_calls = 0;
    std::uint64_t closure_bridge_calls = 0;
    std::uint64_t closure_ir_calls = 0;
    std::uint64_t closure_stale_returns = 0;
    // Issue #253: lifetime total of MoveOp instructions elided
    // by TypeSpecializationWrap (when source has
    // linear_ownership_state == Owned). Mirrors
    // CompilerMetrics::linear_elide_count.
    std::uint64_t linear_elide_count = 0;
    // Issue #254: IR SoA dual-emit counters (lifetime total).
    // Mirrors CompilerMetrics::ir_soa_instructions_emitted +
    // CompilerMetrics::ir_soa_functions_emitted.
    std::uint64_t ir_soa_instructions_emitted = 0;
    std::uint64_t ir_soa_functions_emitted = 0;
    // Issue #255: reference stability observability. Mirrors
    // CompilerMetrics::{bump_generation_count, is_valid_check_count,
    // stable_ref_invalidations, atomic_batch_commits}.
    std::uint64_t bump_generation_count = 0;
    std::uint64_t is_valid_check_count = 0;
    std::uint64_t stable_ref_invalidations = 0;
    std::uint64_t atomic_batch_commits = 0;
    // Issue #256: AST operation observability. Mirrors
    // CompilerMetrics::{children_call_count,
    // parent_of_call_count, mark_dirty_upward_call_count,
    // mark_dirty_total_nodes}.
    std::uint64_t children_call_count = 0;
    std::uint64_t parent_of_call_count = 0;
    std::uint64_t mark_dirty_upward_call_count = 0;
    std::uint64_t mark_dirty_total_nodes = 0;
    // Issue #258: multi-mutation incremental type checking
    // observability. Mirrors CompilerMetrics::{typecheck_cache_hits_total,
    // typecheck_cache_misses_total, typecheck_stale_cache_total,
    // delta_solve_time_us}. multi_mutation_recompute_ratio is
    // computed at snapshot read time as cache_misses /
    // (hits + misses + stale) in basis points (0-10000).
    std::uint64_t typecheck_cache_hits_total = 0;
    std::uint64_t typecheck_cache_misses_total = 0;
    std::uint64_t typecheck_stale_cache_total = 0;
    // Issue #412: mirror of typecheck_gen_saved_total +
    // derived gen_saved_ratio_bp (basis points: gen_saved /
    // (stale_cache + gen_saved) * 10000). 0 when neither
    // counter has been bumped.
    std::uint64_t typecheck_gen_saved_total = 0;
    std::uint64_t typecheck_gen_saved_ratio_bp = 0;
    std::uint64_t delta_solve_time_us = 0;
    std::uint64_t multi_mutation_recompute_ratio_bp = 0;
    // Issue #259: type metadata propagation observability.
    // Mirrors CompilerMetrics::{ir_instructions_total,
    // ir_instructions_with_type_total}. The derived
    // type_propagation_coverage_bp is computed at snapshot
    // read time as with_type*10000/total in basis points.
    std::uint64_t ir_instructions_total = 0;
    std::uint64_t ir_instructions_with_type_total = 0;
    std::uint64_t type_propagation_coverage_bp = 0;
    // Issue #410: per-symbol dirty observability. Mirrors
    // CompilerMetrics::{per_symbol_dirty_lookups_total,
    // per_symbol_dirty_uses_total}. The derived
    // per_symbol_dirty_reduction_bp is computed at snapshot
    // read time: uses_total * 10000 / (avg ancestor
    // propagation depth * lookups_total). Higher = bigger
    // savings from per-symbol vs ancestor-only.
    std::uint64_t per_symbol_dirty_lookups_total = 0;
    std::uint64_t per_symbol_dirty_uses_total = 0;
    std::uint64_t per_symbol_dirty_reduction_bp = 0;
    // Issue #411: post-mutation auto-incremental typecheck
    // observability. Mirrors
    // CompilerMetrics::{incremental_typecheck_auto_invocations_total,
    // incremental_typecheck_re_inferred_total}. The derived
    // incremental_typecheck_avg_re_inferred_bp is computed
    // at snapshot read time as
    // re_inferred * 10000 / max(auto_invocations, 1) —
    // average re-inferred nodes per auto-invocation, in
    // basis points. The follow-up per-symbol wiring (Issue
    // #410 Phase 2/2) will reduce this metric.
    std::uint64_t incremental_typecheck_auto_invocations_total = 0;
    std::uint64_t incremental_typecheck_re_inferred_total = 0;
    std::uint64_t incremental_typecheck_avg_re_inferred_bp = 0;
    // Issue #411 follow-up #1: per-symbol re-inference path
    // observability. Mirrors the 4 lifetime counters in
    // CompilerMetrics. The derived
    // per_symbol_path_share_bp = per_symbol_visited /
    // (per_symbol_visited + ancestor_visited) * 10000 is the
    // share of re-inference work that went through the
    // per-symbol (fast) path. The follow-up #410 Phase 2/2
    // (O(uses) DefUseIndex routing) will push this further
    // toward 100% by replacing the O(n) per_symbol walk with
    // an O(uses) indexed lookup.
    std::uint64_t per_symbol_reinfer_used_total = 0;
    std::uint64_t per_symbol_reinfer_visited_total = 0;
    std::uint64_t ancestor_reinfer_used_total = 0;
    std::uint64_t ancestor_reinfer_visited_total = 0;
    std::uint64_t per_symbol_path_share_bp = 0;
    // Issue #411 fu1 follow-up #2: per-DefUseIndex tracker
    // observability. Mirrors the 3 lifetime counters in
    // CompilerMetrics. The derived
    // per_defuse_index_visited_avg =
    // per_defuse_index_visited / max(per_defuse_index_used, 1)
    // tells the user the average re-inferred node count
    // per per-DefUseIndex mutation. The follow-up
    // per-DefUseIndex walk reduction (issue #411 fu1
    // follow-up #3) will reduce this metric further by
    // replacing the O(n) walk with an O(uses) indexed
    // lookup.
    std::uint64_t per_defuse_index_used_total = 0;
    std::uint64_t per_defuse_index_visited_total = 0;
    std::uint64_t per_defuse_index_walk_fallback_total = 0;
    std::uint64_t per_defuse_index_visited_avg_bp = 0;
    // Issue #247: SyntaxMarker distribution in the current
    // workspace. Populated by CompilerService::snapshot() by
    // walking workspace_flat_->marker_column() (when set).
    // - marker_user_count: nodes written by the user
    // - marker_macro_introduced_count: nodes inserted by hygienic
    //   macros (clone_macro_body)
    // - marker_bool_literal_count: auto-generated #t / #f nodes
    // - marker_total_count: total nodes in the marker column
    std::uint64_t marker_user_count = 0;
    std::uint64_t marker_macro_introduced_count = 0;
    std::uint64_t marker_bool_literal_count = 0;
    std::uint64_t marker_total_count = 0;
    // Per-function metrics (built up from jit_cache_ + shape metrics)
    std::vector<FnMetrics> functions;
};

} // namespace aura::compiler

#endif // AURA_COMPILER_OBSERVABILITY_SNAPSHOT_H
