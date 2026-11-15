// service_snapshot.cpp — CompilerService::snapshot moved out of the module
// interface (#5242). Move-only: the body is unchanged.

module;

#include "observability_snapshot.h" // CompilerSnapshot
#include "shape.h"                  // shape::make_fn_key

module aura.compiler.service;

import std;

namespace aura::compiler {

CompilerSnapshot CompilerService::snapshot() const {
    IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
    CompilerSnapshot s;
    s.deopt_count = metrics_.deopt_count.load(std::memory_order_relaxed);
    s.specialization_hits = metrics_.specialization_hits.load(std::memory_order_relaxed);
    s.specialization_misses = metrics_.specialization_misses.load(std::memory_order_relaxed);
    s.shape_changes_observed = metrics_.shape_changes_observed.load(std::memory_order_relaxed);
    s.jit_compilations = metrics_.jit_compilations.load(std::memory_order_relaxed);
    s.jit_compile_misses = metrics_.jit_compile_misses.load(std::memory_order_relaxed);
    s.jit_cache_evictions = metrics_.jit_cache_evictions.load(std::memory_order_relaxed);
    s.aot_emits = metrics_.aot_emits.load(std::memory_order_relaxed);
    s.aot_fallbacks = metrics_.aot_fallbacks.load(std::memory_order_relaxed);
    s.arena_bytes_used = metrics_.arena_bytes_used.load(std::memory_order_relaxed);
    s.arena_bytes_peak = metrics_.arena_bytes_peak.load(std::memory_order_relaxed);
    // Issue #250: atomic-batch observability.
    s.atomic_batch_count = evaluator_.atomic_batch_count();
    s.atomic_batch_ops_total = evaluator_.atomic_batch_ops_total();
    s.atomic_batch_rollbacks = evaluator_.atomic_batch_rollbacks();
    s.atomic_batch_bumps_saved_total = evaluator_.atomic_batch_bumps_saved_total();
    // Issue #252: closure dual-path observability. Read
    // from the shared CompilerMetrics struct (also bumped
    // by the IR's IROpcode::Call/Apply). The Evaluator's
    // apply_closure writes to the same struct via the
    // compiler_metrics_ pointer set in the constructor.
    s.closure_calls_total = metrics_.closure_calls_total.load(std::memory_order_relaxed);
    s.closure_ffi_calls = metrics_.closure_ffi_calls.load(std::memory_order_relaxed);
    s.closure_tw_calls = metrics_.closure_tw_calls.load(std::memory_order_relaxed);
    s.closure_ir_calls = metrics_.closure_ir_calls.load(std::memory_order_relaxed);
    s.closure_bridge_calls = metrics_.closure_bridge_calls.load(std::memory_order_relaxed);
    s.closure_stale_returns = metrics_.closure_stale_returns.load(std::memory_order_relaxed);
    // Issue #253: linear-move elision count (lifetime total).
    s.linear_elide_count = metrics_.linear_elide_count.load(std::memory_order_relaxed);
    // Issue #433: dead coercion elimination
    // (lifetime total of CastOps eliminated by the
    // DeadCoercionEliminationPass).
    s.dead_coercion_eliminated_total =
        metrics_.dead_coercion_eliminated_total.load(std::memory_order_relaxed);
    // Issue #508: elapsed_us + kept_for_debug exposure.
    s.dead_coercion_elapsed_us_total =
        metrics_.dead_coercion_elapsed_us_total.load(std::memory_order_relaxed);
    s.dead_coercion_kept_for_debug_total =
        metrics_.dead_coercion_kept_for_debug_total.load(std::memory_order_relaxed);
    // Issue #629: zero-overhead coercion path observability.
    s.coercion_castop_emitted_total =
        metrics_.coercion_castop_emitted_total.load(std::memory_order_relaxed);
    s.coercion_type_prop_hits_total =
        metrics_.coercion_type_prop_hits_total.load(std::memory_order_relaxed);
    s.coercion_narrow_evidence_hits_total =
        metrics_.coercion_narrow_evidence_hits_total.load(std::memory_order_relaxed);
    s.coercion_zerooverhead_win_total =
        metrics_.coercion_zerooverhead_win_total.load(std::memory_order_relaxed);
    s.coercion_post_narrow_elim_opportunities_total =
        metrics_.coercion_post_narrow_elim_opportunities_total.load(std::memory_order_relaxed);
    s.coercion_narrow_blame_chain_hits_total =
        metrics_.coercion_narrow_blame_chain_hits_total.load(std::memory_order_relaxed);
    s.coercion_cast_elim_from_narrow_total =
        metrics_.coercion_cast_elim_from_narrow_total.load(std::memory_order_relaxed);
    // Issue #487: dirty propagation + IR re-lower
    // observability. Mirror the 2 lifetime
    // counters and compute the derived trigger
    // rate (basis points: should_relower /
    // affected_subtree * 10000).
    s.should_relower_total = metrics_.should_relower_total.load(std::memory_order_relaxed);
    s.affected_subtree_total = metrics_.affected_subtree_total.load(std::memory_order_relaxed);
    if (s.affected_subtree_total > 0) {
        s.dirty_trigger_rate_bp = static_cast<std::uint64_t>(
            ::aura::core::consts::basis_points(s.should_relower_total, s.affected_subtree_total));
    } else {
        s.dirty_trigger_rate_bp = 0;
    }
    // Issue #1474 / #1495: per-block re-lower observability.
    // Mirror incremental_relower_blocks_total + full_relower_count
    // and derive dirty_block_ratio_bp from the existing
    // ir_soa_block_dirty_hits_total /
    // ir_soa_relower_blocks_saved_total pair.
    s.incremental_relower_blocks_total =
        metrics_.incremental_relower_blocks_total.load(std::memory_order_relaxed);
    s.full_relower_count = metrics_.relower_full_called_count.load(std::memory_order_relaxed);
    {
        const std::uint64_t hits =
            metrics_.ir_soa_block_dirty_hits_total.load(std::memory_order_relaxed);
        const std::uint64_t saved =
            metrics_.ir_soa_relower_blocks_saved_total.load(std::memory_order_relaxed);
        const std::uint64_t sum = hits + saved;
        s.dirty_block_ratio_bp =
            static_cast<std::uint64_t>(::aura::core::consts::basis_points(hits, sum));
    }
    // Issue #387: Type Dependency Graph observability.
    // Mirrors the 3 atomics; derives type_dep_graph_hit_rate_bp.
    s.type_dep_graph_lookups = metrics_.type_dep_graph_lookups.load(std::memory_order_relaxed);
    s.type_dep_graph_hits = metrics_.type_dep_graph_hits.load(std::memory_order_relaxed);
    s.type_dep_graph_size = metrics_.type_dep_graph_size.load(std::memory_order_relaxed);
    if (s.type_dep_graph_lookups > 0) {
        s.type_dep_graph_hit_rate_bp = static_cast<std::uint64_t>(
            ::aura::core::consts::basis_points(s.type_dep_graph_hits, s.type_dep_graph_lookups));
    } else {
        s.type_dep_graph_hit_rate_bp = 0;
    }
    // Issue #254: IR SoA dual-emit counters (lifetime total).
    s.ir_soa_instructions_emitted =
        metrics_.ir_soa_instructions_emitted.load(std::memory_order_relaxed);
    s.ir_soa_functions_emitted = metrics_.ir_soa_functions_emitted.load(std::memory_order_relaxed);
    // Issue #255: reference stability observability. Read
    // the live counters from the workspace FlatAST (they
    // live on FlatAST, not on CompilerMetrics — the bump
    // sites are in ast.ixx where they have direct access
    // to the generation_ + node_gen_ state). If no
    // workspace is set (e.g. pre-init), all counts stay 0.
    if (auto* ws_flat = evaluator_.workspace_flat()) {
        s.bump_generation_count = ws_flat->bump_generation_count();
        s.is_valid_check_count = ws_flat->is_valid_check_count();
        s.stable_ref_invalidations = ws_flat->stable_ref_invalidations();
        s.atomic_batch_commits = ws_flat->atomic_batch_commits_v();
        // Issue #343: long-term stability observability.
        // current_generation is the live value of
        // FlatAST::generation_ (uint16_t, 1..65535);
        // generation_wrap_count is the lifetime total
        // of wrap-arounds; node_gen_stale_access_count
        // is the lifetime total of stale NodeId
        // accesses. These are all read from the
        // workspace FlatAST (they live there, not on
        // CompilerMetrics).
        s.current_generation = ws_flat->current_generation();
        s.generation_wrap_count = ws_flat->generation_wrap_count();
        s.node_gen_stale_access_count = ws_flat->node_gen_stale_access_count();
        // Issue #368: current wrap_epoch_ so AI agents can
        // checkpoint / compact before the next wrap.
        s.current_wrap_epoch = ws_flat->wrap_epoch();
        // Issue #369/370: structural rollback + safe-view
        // counters.
        s.structural_rollback_success = ws_flat->structural_rollback_success();
        s.structural_rollback_besteffort = ws_flat->structural_rollback_besteffort();
        s.children_safe_view_count = ws_flat->children_safe_view_count();
        s.parent_safe_view_count = ws_flat->parent_safe_view_count();
        // Issue #1281/#1282: children topology restore + wrap restamp.
        s.children_topology_restore_count = ws_flat->children_topology_restore_count();
        // Issue #1502: parent_ topology rebuild after failed boundary.
        s.parent_topology_restore_count = ws_flat->parent_topology_restore_count();
        s.auto_restamp_on_wrap_count = ws_flat->auto_restamp_on_wrap_count();
        // Issue #256: AST operation observability counters
        // (children/parent_of/mark_dirty_upward call counts
        // + total nodes marked dirty).
        s.children_call_count = ws_flat->children_call_count();
        s.parent_of_call_count = ws_flat->parent_of_call_count();
        s.mark_dirty_upward_call_count = ws_flat->mark_dirty_upward_call_count();
        s.mark_dirty_total_nodes = ws_flat->mark_dirty_total_nodes();
    }
    // Issue #258 / #1797: multi-mutation incremental typecheck
    // observability. Read the 4 type-cache counters as one
    // snapshot so derived ratios are not mixed across epochs.
    {
        const auto tc_snap = metrics_.snapshot_type_cache_stats();
        s.typecheck_cache_hits_total = tc_snap.hits;
        s.typecheck_cache_misses_total = tc_snap.misses;
        s.typecheck_stale_cache_total = tc_snap.stale;
        s.typecheck_gen_saved_total = tc_snap.gen_saved;
    }
    s.delta_solve_time_us = metrics_.delta_solve_time_us.load(std::memory_order_relaxed);
    const std::uint64_t total = s.typecheck_cache_hits_total + s.typecheck_cache_misses_total +
                                s.typecheck_stale_cache_total;
    if (total > 0) {
        s.multi_mutation_recompute_ratio_bp = static_cast<std::uint64_t>(
            ::aura::core::consts::basis_points(s.typecheck_cache_misses_total, total));
    } else {
        s.multi_mutation_recompute_ratio_bp = 0;
    }
    // Issue #412: gen_saved_ratio_bp from the same snapshot.
    const std::uint64_t gen_total = s.typecheck_stale_cache_total + s.typecheck_gen_saved_total;
    if (gen_total > 0) {
        s.typecheck_gen_saved_ratio_bp = static_cast<std::uint64_t>(
            ::aura::core::consts::basis_points(s.typecheck_gen_saved_total, gen_total));
    } else {
        s.typecheck_gen_saved_ratio_bp = 0;
    }
    // Issue #412 follow-up #1: mirror the 2
    // per-binding gen counters and compute the
    // derived hit ratio (basis points:
    // per_binding_gen_hits / (per_binding_gen_hits +
    // stale_cache) * 10000). 0 when no per-binding
    // hits have happened yet.
    s.per_binding_gen_hits_total =
        metrics_.per_binding_gen_hits_total.load(std::memory_order_relaxed);
    // The per_binding_gen_bumps_total counter comes
    // from the workspace FlatAST (it's bumped on
    // every mark_dirty_upward on a binding node).
    // Read from the workspace if available. The
    // accumulator is a separate field (NOT
    // per_binding_gen_bumps_total in CompilerMetrics)
    // because snapshot() is const and we can't
    // fetch_add on a const atomic. The accumulator
    // is mutable so the lifetime total persists
    // across snapshots.
    std::uint64_t binding_gen_bumps_from_flat = 0;
    if (auto* ws = evaluator_.workspace_flat()) {
        binding_gen_bumps_from_flat = ws->binding_gen_bumps_total();
    }
    if (binding_gen_bumps_from_flat > last_per_binding_gen_bumps_) {
        per_binding_gen_bumps_acc_ += binding_gen_bumps_from_flat - last_per_binding_gen_bumps_;
        last_per_binding_gen_bumps_ = binding_gen_bumps_from_flat;
    }
    s.per_binding_gen_bumps_total = per_binding_gen_bumps_acc_;
    const std::uint64_t pb_total = s.per_binding_gen_hits_total + s.typecheck_stale_cache_total;
    if (pb_total > 0) {
        s.per_binding_gen_hit_ratio_bp = static_cast<std::uint64_t>(
            ::aura::core::consts::basis_points(s.per_binding_gen_hits_total, pb_total));
    } else {
        s.per_binding_gen_hit_ratio_bp = 0;
    }
    // Issue #1872: 0–100 hit-rate mirror of hit_ratio_bp for
    // dashboards (same formula as incremental_locality_hit_rate).
    metrics_.per_binding_gen_hit_rate.store(s.per_binding_gen_hit_ratio_bp / 100u,
                                            std::memory_order_relaxed);
    // Issue #413: invalidation trace records count.
    // Read from the workspace FlatAST (the trace
    // vector is per-FlatAST, lifetime = FlatAST
    // lifetime). Accumulate via the same pattern as
    // per_binding_gen_bumps_total.
    std::uint64_t trace_records_from_flat = 0;
    if (auto* ws = evaluator_.workspace_flat()) {
        trace_records_from_flat = ws->invalidation_trace_records_total();
    }
    if (trace_records_from_flat > last_invalidation_trace_records_) {
        invalidation_trace_records_acc_ +=
            trace_records_from_flat - last_invalidation_trace_records_;
        last_invalidation_trace_records_ = trace_records_from_flat;
    }
    s.invalidation_trace_records_total = invalidation_trace_records_acc_;
    // Issue #386: mirror the 3 narrowing
    // observability counters and compute the
    // derived applied ratio (basis points:
    // applied / (applied + skipped) * 10000).
    // 0 when no narrowing has happened yet.
    s.narrowing_applied_total = metrics_.narrowing_applied_total.load(std::memory_order_relaxed);
    s.narrowing_skipped_total = metrics_.narrowing_skipped_total.load(std::memory_order_relaxed);
    s.narrowing_reanalyzed_total =
        metrics_.narrowing_reanalyzed_total.load(std::memory_order_relaxed);
    // Issue #338: mirror the 2 and/or precision
    // counters.
    s.and_or_meet_uses_total = metrics_.and_or_meet_uses_total.load(std::memory_order_relaxed);
    s.and_or_join_uses_total = metrics_.and_or_join_uses_total.load(std::memory_order_relaxed);
    // Issue #2148: mirror precise meet hits (TypeRegistry bumps
    // the same atomic via set_meet_precision_counter).
    s.meet_precision_hit_total = metrics_.meet_precision_hit_total.load(std::memory_order_relaxed);
    // Issue #434: per-node occurrence dirty recovery.
    s.narrowing_dirty_recovery_total =
        metrics_.narrowing_dirty_recovery_total.load(std::memory_order_relaxed);
    // Issue #390: schema cache observability.
    s.schema_cache_lookups_total =
        metrics_.schema_cache_lookups_total.load(std::memory_order_relaxed);
    s.schema_cache_hits_total = metrics_.schema_cache_hits_total.load(std::memory_order_relaxed);
    if (s.schema_cache_lookups_total > 0) {
        s.schema_cache_hit_rate_bp = static_cast<std::uint64_t>(::aura::core::consts::basis_points(
            s.schema_cache_hits_total, s.schema_cache_lookups_total));
    } else {
        s.schema_cache_hit_rate_bp = 0;
    }
    // Issue #409: fine-grained constraint
    // dependency tracking observability. Mirror
    // the 2 lifetime counters and compute the
    // derived ratio (basis points: processed /
    // total * 10000).
    s.delta_constraints_processed_total =
        metrics_.delta_constraints_processed_total.load(std::memory_order_relaxed);
    s.delta_constraints_total = metrics_.delta_constraints_total.load(std::memory_order_relaxed);
    s.delta_conflict_reverify_total =
        metrics_.delta_conflict_reverify_total.load(std::memory_order_relaxed);
    s.delta_conflict_detected_total =
        metrics_.delta_conflict_detected_total.load(std::memory_order_relaxed);
    s.reverify_truncated_total = metrics_.reverify_truncated_total.load(std::memory_order_relaxed);
    s.constraint_blame_chain_complete_total =
        metrics_.constraint_blame_chain_complete_total.load(std::memory_order_relaxed);
    s.constraint_reverify_narrow_hits_total =
        metrics_.constraint_reverify_narrow_hits_total.load(std::memory_order_relaxed);
    s.constraint_reverify_timeout_prevented_total =
        metrics_.constraint_reverify_timeout_prevented_total.load(std::memory_order_relaxed);
    s.constraint_stale_blame_invalidation_total =
        metrics_.constraint_stale_blame_invalidation_total.load(std::memory_order_relaxed);
    s.solve_delta_full_solve_fallback_total =
        metrics_.solve_delta_full_solve_fallback_total.load(std::memory_order_relaxed);
    if (s.delta_constraints_total > 0) {
        s.delta_solve_constraints_ratio_bp =
            static_cast<std::uint64_t>(::aura::core::consts::basis_points(
                s.delta_constraints_processed_total, s.delta_constraints_total));
    } else {
        s.delta_solve_constraints_ratio_bp = 0;
    }
    // Issue #341: match + Occurrence Typing
    // observability. Mirror the 2 lifetime
    // counters and compute the derived ratio
    // (basis points: narrowed / total * 10000).
    s.match_subject_narrowed_total =
        metrics_.match_subject_narrowed_total.load(std::memory_order_relaxed);
    s.match_subject_total = metrics_.match_subject_total.load(std::memory_order_relaxed);
    if (s.match_subject_total > 0) {
        s.match_narrowed_ratio_bp = static_cast<std::uint64_t>(::aura::core::consts::basis_points(
            s.match_subject_narrowed_total, s.match_subject_total));
    } else {
        s.match_narrowed_ratio_bp = 0;
    }
    // Issue #612: ADT/match exhaustiveness post-mutation
    // reliability observability.
    s.adt_exhaust_rechecks_total =
        metrics_.adt_exhaust_rechecks_total.load(std::memory_order_relaxed);
    s.adt_variant_mutate_impacts_total =
        metrics_.adt_variant_mutate_impacts_total.load(std::memory_order_relaxed);
    s.adt_stale_exhaust_prevented_total =
        metrics_.adt_stale_exhaust_prevented_total.load(std::memory_order_relaxed);
    s.adt_occurrence_narrow_in_match_total =
        metrics_.adt_occurrence_narrow_in_match_total.load(std::memory_order_relaxed);
    s.adt_pattern_narrow_refreshes_total =
        metrics_.adt_pattern_narrow_refreshes_total.load(std::memory_order_relaxed);
    s.adt_non_exhaustive_caught_total =
        metrics_.adt_non_exhaustive_caught_total.load(std::memory_order_relaxed);
    s.adt_pattern_provenance_complete_total =
        metrics_.adt_pattern_provenance_complete_total.load(std::memory_order_relaxed);
    s.hardware_backend_hook_calls_total =
        metrics_.hardware_backend_hook_calls_total.load(std::memory_order_relaxed);
    s.commercial_reemits_total = metrics_.commercial_reemits_total.load(std::memory_order_relaxed);
    s.feedback_mutate_hits_total =
        metrics_.feedback_mutate_hits_total.load(std::memory_order_relaxed);
    s.ppa_savings_total = metrics_.ppa_savings_total.load(std::memory_order_relaxed);
    s.verification_loop_success_total =
        metrics_.verification_loop_success_total.load(std::memory_order_relaxed);
    s.sv_emit_parse_success_total =
        metrics_.sv_emit_parse_success_total.load(std::memory_order_relaxed);
    s.sv_emit_parse_fail_total = metrics_.sv_emit_parse_fail_total.load(std::memory_order_relaxed);
    s.commercial_simulator_runs_total =
        metrics_.commercial_simulator_runs_total.load(std::memory_order_relaxed);
    s.sv_diff_emits_total = metrics_.sv_diff_emits_total.load(std::memory_order_relaxed);
    s.sva_structured_mutate_hits_total =
        metrics_.sva_structured_mutate_hits_total.load(std::memory_order_relaxed);
    s.primitive_skeleton_generations_total =
        metrics_.primitive_skeleton_generations_total.load(std::memory_order_relaxed);
    // Issue #342: narrowing provenance
    // observability. Mirrors the lifetime
    // counter in CompilerMetrics.
    s.narrowing_provenance_total =
        metrics_.narrowing_provenance_total.load(std::memory_order_relaxed);
    s.occurrence_stale_refreshes_total =
        metrics_.occurrence_stale_refreshes_total.load(std::memory_order_relaxed);
    s.occurrence_blame_chain_complete_total =
        metrics_.occurrence_blame_chain_complete_total.load(std::memory_order_relaxed);
    // Issue #639: narrow blame + stale invalidation.
    s.narrow_stale_caught_total =
        metrics_.narrow_stale_caught_total.load(std::memory_order_relaxed);
    s.narrow_blame_attached_total =
        metrics_.narrow_blame_attached_total.load(std::memory_order_relaxed);
    s.narrow_invalidation_post_mutate_total =
        metrics_.narrow_invalidation_post_mutate_total.load(std::memory_order_relaxed);
    if (auto* ws = evaluator_.workspace_flat()) {
        s.narrow_invalidation_post_mutate_total += ws->narrow_invalidation_post_mutate_count();
    }
    s.narrow_safe_fallback_total =
        metrics_.narrow_safe_fallback_total.load(std::memory_order_relaxed);
    // Issue #627: bidirectional check-mode narrow observability.
    s.check_mode_narrow_hits_total =
        metrics_.check_mode_narrow_hits_total.load(std::memory_order_relaxed);
    s.synthesize_check_switch_count_total =
        metrics_.synthesize_check_switch_count_total.load(std::memory_order_relaxed);
    s.post_mutate_narrow_consistency_total =
        metrics_.post_mutate_narrow_consistency_total.load(std::memory_order_relaxed);
    s.stale_check_narrow_prevented_total =
        metrics_.stale_check_narrow_prevented_total.load(std::memory_order_relaxed);
    // Issue #383: ConstraintSystem worklist +
    // consistent_unify observability. Mirror
    // the 3 lifetime counters in
    // CompilerMetrics.
    s.consistent_unify_total = metrics_.consistent_unify_total.load(std::memory_order_relaxed);
    s.consistent_subtype_total = metrics_.consistent_subtype_total.load(std::memory_order_relaxed);
    s.worklist_restart_total = metrics_.worklist_restart_total.load(std::memory_order_relaxed);
    // Issue #385: Let-Poly caching observability.
    // Mirror the 3 lifetime counters and compute
    // the derived dedup ratio (basis points:
    // dedup_hits / register * 10000).
    s.poly_register_total = metrics_.poly_register_total.load(std::memory_order_relaxed);
    s.poly_dedup_hits_total = metrics_.poly_dedup_hits_total.load(std::memory_order_relaxed);
    s.poly_instantiate_total = metrics_.poly_instantiate_total.load(std::memory_order_relaxed);
    if (s.poly_register_total > 0) {
        s.poly_dedup_ratio_bp = static_cast<std::uint64_t>(
            ::aura::core::consts::basis_points(s.poly_dedup_hits_total, s.poly_register_total));
    } else {
        s.poly_dedup_ratio_bp = 0;
    }
    const std::uint64_t narrow_total = s.narrowing_applied_total + s.narrowing_skipped_total;
    if (narrow_total > 0) {
        s.narrowing_applied_ratio_bp = static_cast<std::uint64_t>(
            ::aura::core::consts::basis_points(s.narrowing_applied_total, narrow_total));
    } else {
        s.narrowing_applied_ratio_bp = 0;
    }
    // Issue #259: type metadata propagation observability.
    // Read lifetime totals from CompilerMetrics, compute
    // the derived coverage (basis points: 0-10000).
    s.ir_instructions_total = metrics_.ir_instructions_total.load(std::memory_order_relaxed);
    s.ir_instructions_with_type_total =
        metrics_.ir_instructions_with_type_total.load(std::memory_order_relaxed);
    if (s.ir_instructions_total > 0) {
        s.type_propagation_coverage_bp =
            static_cast<std::uint64_t>(::aura::core::consts::basis_points(
                s.ir_instructions_with_type_total, s.ir_instructions_total));
    } else {
        s.type_propagation_coverage_bp = 0;
    }
    // Issue #410: per-symbol dirty observability. Mirror
    // the 2 lifetime counters and compute the derived
    // reduction ratio (basis points). The ancestor-avg is
    // computed from the mark_dirty_total_nodes /
    // mark_dirty_upward_call_count ratio that the
    // observable already exposes (Issue #256). When that
    // avg is 0 (no calls yet) the derived ratio stays 0.
    s.per_symbol_dirty_lookups_total =
        metrics_.per_symbol_dirty_lookups_total.load(std::memory_order_relaxed);
    s.per_symbol_dirty_uses_total =
        metrics_.per_symbol_dirty_uses_total.load(std::memory_order_relaxed);
    if (s.per_symbol_dirty_lookups_total > 0 && s.mark_dirty_upward_call_count > 0) {
        const std::uint64_t avg_ancestor_depth =
            s.mark_dirty_total_nodes / s.mark_dirty_upward_call_count;
        const std::uint64_t est_ancestor_uses =
            avg_ancestor_depth * s.per_symbol_dirty_lookups_total;
        s.per_symbol_dirty_reduction_bp = static_cast<std::uint64_t>(
            ::aura::core::consts::basis_points(s.per_symbol_dirty_uses_total, est_ancestor_uses));
    } else {
        s.per_symbol_dirty_reduction_bp = 0;
    }
    // Issue #411: post-mutation auto-incremental typecheck
    // observability. Mirror the 2 lifetime counters and
    // compute the derived average (basis points:
    // re_inferred * 10000 / max(auto_invocations, 1)).
    // The average is 0 when no auto-invocations have
    // happened yet (i.e. before any typed_mutate or when
    // the mode is set to Lazy/Disabled).
    s.incremental_typecheck_auto_invocations_total =
        metrics_.incremental_typecheck_auto_invocations_total.load(std::memory_order_relaxed);
    s.incremental_typecheck_re_inferred_total =
        metrics_.incremental_typecheck_re_inferred_total.load(std::memory_order_relaxed);
    if (s.incremental_typecheck_auto_invocations_total > 0) {
        s.incremental_typecheck_avg_re_inferred_bp = static_cast<std::uint64_t>(
            ::aura::core::consts::basis_points(s.incremental_typecheck_re_inferred_total,
                                               s.incremental_typecheck_auto_invocations_total));
    } else {
        s.incremental_typecheck_avg_re_inferred_bp = 0;
    }
    // Issue #411 follow-up #1: mirror the 4 per-symbol
    // counters and compute the derived path-share
    // metric. 0 when no re-inference has happened yet.
    s.per_symbol_reinfer_used_total =
        metrics_.per_symbol_reinfer_used_total.load(std::memory_order_relaxed);
    s.per_symbol_reinfer_visited_total =
        metrics_.per_symbol_reinfer_visited_total.load(std::memory_order_relaxed);
    s.ancestor_reinfer_used_total =
        metrics_.ancestor_reinfer_used_total.load(std::memory_order_relaxed);
    s.ancestor_reinfer_visited_total =
        metrics_.ancestor_reinfer_visited_total.load(std::memory_order_relaxed);
    const std::uint64_t total_visited =
        s.per_symbol_reinfer_visited_total + s.ancestor_reinfer_visited_total;
    if (total_visited > 0) {
        s.per_symbol_path_share_bp = static_cast<std::uint64_t>(
            ::aura::core::consts::basis_points(s.per_symbol_reinfer_visited_total, total_visited));
    } else {
        s.per_symbol_path_share_bp = 0;
    }
    // Issue #411 fu1 follow-up #2: mirror the 3
    // per-DefUseIndex counters and compute the
    // derived average (basis points:
    // per_defuse_index_visited / max(per_defuse_index_used, 1) * 10000).
    // 0 when no per-DefUseIndex invocations have
    // happened yet.
    s.per_defuse_index_used_total =
        metrics_.per_defuse_index_used_total.load(std::memory_order_relaxed);
    s.per_defuse_index_visited_total =
        metrics_.per_defuse_index_visited_total.load(std::memory_order_relaxed);
    s.per_defuse_index_walk_fallback_total =
        metrics_.per_defuse_index_walk_fallback_total.load(std::memory_order_relaxed);
    if (s.per_defuse_index_used_total > 0) {
        s.per_defuse_index_visited_avg_bp =
            static_cast<std::uint64_t>(::aura::core::consts::basis_points(
                s.per_defuse_index_visited_total, s.per_defuse_index_used_total));
    } else {
        s.per_defuse_index_visited_avg_bp = 0;
    }
    // Issue #247: populate marker distribution by walking
    // workspace_flat_->marker_column(). We grab a
    // shared_lock on workspace_mtx_ to keep the flat
    // pointer stable during the read. If no workspace is
    // set (workspace_flat_ is null), all counts stay 0.
    //
    // Note: snapshot() is const but lock_workspace_shared is
    // non-const (it modifies the mutex's internal state). We
    // const_cast the Evaluator reference. This is safe
    // because the shared_mutex's internal state is mutable
    // (its lock/unlock operations don't change the logical
    // contents of the Evaluator).
    //
    // Re-entrancy: snapshot() can be called from the IR
    // interpreter via Aura primitives (e.g. (query:*) that
    // compute hash fields from runtime stats). If the
    // interpreter runs while set-code / restore-ast /
    // mutate holds workspace_mtx_ unique_lock, calling
    // lock_workspace_shared() here would deadlock
    // (EDEADLK / "Resource deadlock avoided"). Use
    // try_lock_shared() and skip the marker read on
    // contention — the marker counts are observability
    // hints, not correctness state, so a missed read is
    // acceptable.
    {
        auto& eval_mut = const_cast<Evaluator&>(evaluator_);
        if (eval_mut.try_lock_workspace_shared()) {
            struct UnlockGuard : ::aura::core::NoCopy {
                Evaluator& e;
                ~UnlockGuard() { e.unlock_workspace_shared(); }
            } guard{{}, eval_mut};
            const auto* wf = eval_mut.workspace_flat();
            if (wf) {
                const auto& markers = wf->marker_column();
                s.marker_total_count = markers.size();
                for (auto m : markers) {
                    auto val = static_cast<int>(m);
                    if (val == 0)
                        ++s.marker_user_count;
                    else if (val == 1)
                        ++s.marker_macro_introduced_count;
                    else if (val == 2)
                        ++s.marker_bool_literal_count;
                }
            }
        }
        // else: contended — the caller already holds unique_lock
        // (e.g. set-code, restore-ast, mutate:*). Skip marker read.
    }
    // Populate per-function metrics from the JIT cache.
    // Issue #968: fill total_calls / deopt / hit / miss / hit_rate /
    // specialized_for from ShapeProfiler + global specialization counters
    // (previously left at zero → --metrics / evo-explain always zeros).
    {
        std::shared_lock cache_read(jit_cache_mtx_);
        const auto hits = metrics_.specialization_hits.load(std::memory_order_relaxed);
        const auto misses = metrics_.specialization_misses.load(std::memory_order_relaxed);
        for (auto& [name, entry] : jit_cache_) {
            FnMetrics fm;
            fm.name = name;
            fm.has_shape_map = entry.has_shape_map;
            auto fn_key = shape::make_fn_key(session_id_, name);
            auto sm = shape_profiler_.metrics(fn_key);
            fm.total_calls = sm.total_calls;
            fm.deopt_count = sm.deopt_count;
            // Distribute global specialization hit/miss proportionally is
            // hard without per-fn counters; surface profile stability as
            // hit when shape-stable, miss otherwise when profiled.
            if (sm.total_calls > 0) {
                if (shape_profiler_.is_stable(fn_key)) {
                    fm.hit_count =
                        sm.total_calls > sm.deopt_count ? sm.total_calls - sm.deopt_count : 0;
                    fm.miss_count = sm.deopt_count;
                } else {
                    fm.hit_count = 0;
                    fm.miss_count = sm.total_calls;
                }
                const auto denom = fm.hit_count + fm.miss_count;
                fm.hit_rate = denom > 0
                                  ? static_cast<double>(fm.hit_count) / static_cast<double>(denom)
                                  : 0.0;
                fm.specialized_for =
                    static_cast<std::uint32_t>(shape_profiler_.dominant_shape(fn_key));
            } else if (entry.has_shape_map) {
                // Compiled with shape map but not yet profiled: still mark
                // specialized path available using global hit/miss ratio.
                fm.hit_count = hits;
                fm.miss_count = misses;
                const auto denom = hits + misses;
                fm.hit_rate =
                    denom > 0 ? static_cast<double>(hits) / static_cast<double>(denom) : 0.0;
            }
            s.functions.push_back(std::move(fm));
        }
        // Issue #1674: surface render hot-path activity as synthetic FnMetrics
        // so --metrics / agents see non-zero total_calls for present even when
        // the function is not in the JIT IR cache (EDSL primitive path).
        {
            FnMetrics rf;
            rf.name = "terminal-present-batch";
            rf.has_shape_map = false;
            rf.total_calls = metrics_.terminal_present_batch_total.load(std::memory_order_relaxed);
            const auto skips = metrics_.render_hotpath_skip_total.load(std::memory_order_relaxed);
            if (rf.total_calls > 0) {
                rf.miss_count = skips <= rf.total_calls ? skips : rf.total_calls;
                rf.hit_count = rf.total_calls > rf.miss_count ? rf.total_calls - rf.miss_count : 0;
                const auto denom = rf.hit_count + rf.miss_count;
                rf.hit_rate = denom > 0
                                  ? static_cast<double>(rf.hit_count) / static_cast<double>(denom)
                                  : 0.0;
                rf.deopt_count = metrics_.render_jit_deopt_applied.load(std::memory_order_relaxed);
                s.functions.push_back(std::move(rf));
            }
        }
    }
    return s;
}

} // namespace aura::compiler
