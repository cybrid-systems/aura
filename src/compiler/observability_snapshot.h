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
    // Issue #433: dead coercion elimination
    // observability. Mirrors the
    // dead_coercion_eliminated_total counter in
    // CompilerMetrics. The dce_hit_rate_bp derived
    // metric is eliminated / (eliminated + remaining)
    // — measures what share of CastOps were
    // eliminated. We don't have a "remaining" counter
    // yet (would require scanning the IR post-pass
    // to count remaining CastOps); the follow-up
    // will add it.
std::uint64_t dead_coercion_eliminated_total = 0;
    // Issue #508: cumulative microseconds spent in the
    // DeadCoercionEliminationPass across all pipeline runs.
    std::uint64_t dead_coercion_elapsed_us_total = 0;
    // Issue #508: total CastOps NOT elided because
    // keep_for_debug was true. Lets users see what the pass
    // WOULD have eliminated.
    std::uint64_t dead_coercion_kept_for_debug_total = 0;
    // Issue #629: zero-overhead coercion path observability.
    // Mirrors the 4 lifetime counters in CompilerMetrics.
    std::uint64_t coercion_castop_emitted_total = 0;
    std::uint64_t coercion_type_prop_hits_total = 0;
    std::uint64_t coercion_narrow_evidence_hits_total = 0;
    std::uint64_t coercion_zerooverhead_win_total = 0;
    // Issue #691: CoercionMap + NarrowingRecord provenance linkage.
    std::uint64_t coercion_post_narrow_elim_opportunities_total = 0;
    std::uint64_t coercion_narrow_blame_chain_hits_total = 0;
    std::uint64_t coercion_cast_elim_from_narrow_total = 0;
    // Issue #487: dirty propagation + IR re-lower
    // observability. Mirrors the 2 lifetime
    // counters in CompilerMetrics. The derived
    // dirty_trigger_rate_bp is should_relower /
    // affected_subtree * 10000 — measures how
    // often the dirty path actually triggers a
    // re-lower (vs. just enumerating affected
    // nodes).
    std::uint64_t should_relower_total = 0;
    std::uint64_t affected_subtree_total = 0;
    std::uint64_t dirty_trigger_rate_bp = 0;
    // Issue #387: Type Dependency Graph observability.
    // Mirrors CompilerMetrics::{type_dep_graph_lookups,
    // type_dep_graph_hits, type_dep_graph_size}.
    std::uint64_t type_dep_graph_lookups = 0;
    std::uint64_t type_dep_graph_hits = 0;
    std::uint64_t type_dep_graph_size = 0;
    // Derived: hit rate in basis points (0-10000). High
    // hit rate means most lookups find dependent nodes
    // (the graph is useful); low hit rate means the
    // graph has many empty entries (TypeVars that
    // unified away).
    std::uint64_t type_dep_graph_hit_rate_bp = 0;
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
    // Issue #343: long-term stability observability.
    // - current_generation: the live value of
    //   FlatAST::generation_ (uint16_t, 1..65535).
    //   Useful for AI agents deciding when to
    //   checkpoint before the next wrap.
    // - generation_wrap_count: lifetime total of
    //   uint16_t wrap-arounds (every 65K bumps).
    // - node_gen_stale_access_count: lifetime total
    //   of raw NodeId accesses that hit a stale
    //   node_gen_ (a mutation advanced the per-node
    //   generation past the accessor's read).
    //   Pre-#343 these were only accessible via
    //   (query:stable-ref-stats) which returns the
    //   SUM; post-#343 the snapshot exposes them
    //   individually so the AI Agent can react to
    //   each category independently.
    std::uint16_t current_generation = 0;
    std::uint64_t generation_wrap_count = 0;
    std::uint64_t node_gen_stale_access_count = 0;
    // Issue #368: current wrap_epoch_ (bumped per
    // generation_ wrap). uint32_t; needs ~2.6e14 mutates to
    // wrap. AI agents can read this via (ast:generation-stats)
    // to checkpoint / compact before the next wrap.
    std::uint32_t current_wrap_epoch = 0;
    // Issue #369: per-category counters for the structural
    // rollback dispatcher in try_rollback_structural_child_op.
    // structural_rollback_success: mutations whose children_
    // column was fully restored (parent + child_idx +
    // old/new_value data present + valid).
    // structural_rollback_besteffort: mutations whose
    // op_name aliased to a known structural op but lacked
    // the field data (legacy add_mutation() calls that
    // haven't been migrated to
    // add_structural_mutation_log_entry yet).
    std::uint64_t structural_rollback_success = 0;
    std::uint64_t structural_rollback_besteffort = 0;
    // Issue #370: lifetime-safe (SafePCVSpan) view count.
    // Bumped in FlatAST::children_safe(NodeId). Lets AI
    // agents audit how much of their code uses the safe view
    // vs the raw std::span from children(NodeId) (which is the
    // dangerous pattern across mutate boundaries).
    std::uint64_t children_safe_view_count = 0;
    // Issue #678: parent_safe_view call counter.
    std::uint64_t parent_safe_view_count = 0;
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
    // Issue #412 follow-up #1: per-binding gen
    // observability. Mirrors the 2 lifetime counters in
    // CompilerMetrics. The derived
    // per_binding_gen_hit_ratio_bp = per_binding_gen_hits /
    // (per_binding_gen_hits + stale_cache) * 10000
    // measures the share of stale rejections rescued by
    // the per-binding check (post-#412 follow-up #1).
    std::uint64_t per_binding_gen_hits_total = 0;
    std::uint64_t per_binding_gen_bumps_total = 0;
    std::uint64_t per_binding_gen_hit_ratio_bp = 0;
    // Issue #413: invalidation trace records count
    // (lifetime total). Mirrors the
    // invalidation_trace_records_total in
    // CompilerMetrics. The mutation_log size and the
    // invalidation trace size are usually equal — every
    // mutation that targets a binding produces one
    // invalidation record. Mismatch indicates a mutation
    // that didn't target a binding (sub-expression
    // mutations only bump the global gen, no binding to
    // trace).
    std::uint64_t invalidation_trace_records_total = 0;
    // Issue #386: narrowing observability. Mirrors
    // the 3 lifetime counters in CompilerMetrics. The
    // applied_ratio_bp = applied / (applied + skipped)
    // * 10000 measures narrowing effectiveness.
    std::uint64_t narrowing_applied_total = 0;
    std::uint64_t narrowing_skipped_total = 0;
    std::uint64_t narrowing_reanalyzed_total = 0;
    std::uint64_t narrowing_applied_ratio_bp = 0;
    // Issue #338: and/or precision observability.
    // Mirrors the 2 lifetime counters in
    // CompilerMetrics. meet_uses + join_uses is the
    // total number of times the new helpers fired
    // in the and/or branches (the total
    // intersection + union work done by the
    // precision-improved paths).
    std::uint64_t and_or_meet_uses_total = 0;
    std::uint64_t and_or_join_uses_total = 0;
    // Issue #434: per-node occurrence dirty
    // recovery (lifetime total). Mirrors the
    // narrowing_dirty_recovery_total counter in
    // CompilerMetrics.
    std::uint64_t narrowing_dirty_recovery_total = 0;
    // Issue #390: per-node schema cache observability.
    // Mirrors the 2 lifetime counters in
    // CompilerMetrics. The hit rate (basis points) is
    // hits / lookups * 10000.
    std::uint64_t schema_cache_lookups_total = 0;
    std::uint64_t schema_cache_hits_total = 0;
    std::uint64_t schema_cache_hit_rate_bp = 0;
    // Issue #409: fine-grained constraint dependency
    // tracking observability. Mirrors the 2 lifetime
    // counters in CompilerMetrics. The derived
    // delta_solve_constraints_ratio_bp is
    // processed / total * 10000 — measures how much
    // the reverse map prunes. A low ratio means
    // the filter is doing useful work.
    std::uint64_t delta_constraints_processed_total = 0;
    std::uint64_t delta_constraints_total = 0;
    // Issue #466: solve_delta cross-delta conflict observability.
    std::uint64_t delta_conflict_reverify_total = 0;
    std::uint64_t delta_conflict_detected_total = 0;
    // Issue #690: constraint typed-mutation reverify + blame completeness.
    std::uint64_t reverify_truncated_total = 0;
    std::uint64_t constraint_blame_chain_complete_total = 0;
    // Issue #628: solve_delta full-solve fallback observability.
    std::uint64_t solve_delta_full_solve_fallback_total = 0;
    std::uint64_t delta_solve_constraints_ratio_bp = 0;
    // Issue #341: match + Occurrence Typing
    // observability. Mirrors the 2 lifetime counters
    // in CompilerMetrics. The derived
    // match_narrowed_ratio_bp is narrowed / total *
    // 10000 — measures how often narrowing feeds
    // into match exhaustiveness.
    std::uint64_t match_subject_narrowed_total = 0;
    std::uint64_t match_subject_total = 0;
    std::uint64_t match_narrowed_ratio_bp = 0;
    // Issue #612: ADT/match exhaustiveness post-mutation
    // reliability observability.
    std::uint64_t adt_exhaust_rechecks_total = 0;
    std::uint64_t adt_variant_mutate_impacts_total = 0;
    std::uint64_t adt_stale_exhaust_prevented_total = 0;
    std::uint64_t adt_occurrence_narrow_in_match_total = 0;
    // Issue #692: ADT/match typed-mutation pattern provenance refresh.
    std::uint64_t adt_pattern_narrow_refreshes_total = 0;
    std::uint64_t adt_non_exhaustive_caught_total = 0;
    std::uint64_t adt_pattern_provenance_complete_total = 0;
    // Issue #342: narrowing blame/provenance
    // observability. Mirrors the lifetime counter
    // in CompilerMetrics. The provenance fields
    // (predicate_name + source_cond_id) are
    // populated by analyze_predicate_flat
    // post-#342.
    std::uint64_t narrowing_provenance_total = 0;
    // Issue #537 / #518 Phase 2: occurrence narrowing refresh.
    std::uint64_t occurrence_stale_refreshes_total = 0;
    std::uint64_t occurrence_blame_chain_complete_total = 0;
    // Issue #639: narrow blame + stale invalidation observability.
    std::uint64_t narrow_stale_caught_total = 0;
    std::uint64_t narrow_blame_attached_total = 0;
    std::uint64_t narrow_invalidation_post_mutate_total = 0;
    std::uint64_t narrow_safe_fallback_total = 0;
    // Issue #627: bidirectional check-mode narrow observability.
    std::uint64_t check_mode_narrow_hits_total = 0;
    std::uint64_t synthesize_check_switch_count_total = 0;
    std::uint64_t post_mutate_narrow_consistency_total = 0;
    std::uint64_t stale_check_narrow_prevented_total = 0;
    // Issue #383: ConstraintSystem worklist + consistent_
    // unify observability. Mirrors the 3 lifetime
    // counters in CompilerMetrics.
    std::uint64_t consistent_unify_total = 0;
    std::uint64_t consistent_subtype_total = 0;
    std::uint64_t worklist_restart_total = 0;
    // Issue #385: mutation-aware Let-Poly caching
    // observability. Mirrors the 3 lifetime
    // counters in CompilerMetrics. The derived
    // poly_dedup_ratio_bp is dedup_hits /
    // register * 10000 — measures cache
    // effectiveness. 0 when no register calls
    // have happened.
    std::uint64_t poly_register_total = 0;
    std::uint64_t poly_dedup_hits_total = 0;
    std::uint64_t poly_instantiate_total = 0;
    std::uint64_t poly_dedup_ratio_bp = 0;
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
