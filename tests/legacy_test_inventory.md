# Legacy test inventory

**Issue:** [#1957](https://github.com/cybrid-systems/aura/issues/1957)
**Generated:** 2026-07-25 by `scripts/inventory_legacy_tests.py`
**Status:** living document — re-run the script after consolidations.

## Purpose

Categorize legacy per-issue regression tests so we can migrate them in batches into the preferred `tests/core/` structure (and existing family batch drivers under `tests/test_*_batch.cpp`).

`tests/issues/` **removed** (#1957). Prefer theme/domain batches; do not reintroduce per-issue mains.

## Scope snapshot

| Location | Count | Notes |
|----------|------:|-------|
| `tests/issues/test_issue_*.cpp` | 0 | Legacy per-issue mains / bundle members |
| `tests/test_*.cpp` (issue-oriented) | 0 | Numbered root tests + `*_batch` drivers |
| `tests/core/test_*.cpp` | 344 | Preferred destination suites |
| **Total scanned** | **344** | |

### Related artifacts

- Coarser 5-bucket Phase-2 map: [`tests_classification.md`](domain_classification.md) (`scripts/classify_test_issues.py`)
- Link/bundle profiles: [`tests/fixtures/issue_link_profiles.json`](fixtures/issue_link_profiles.json)
- Domain CMake: [`cmake/AuraDomainTests.cmake`](../cmake/AuraDomainTests.cmake)
- Test layout rules: [`tests/README.md`](README.md)

## Theme buckets (8 + uncategorized)

Classification uses the **filename + first 50 lines** (keywords and filename token boosts). Ties break toward earlier themes in the priority order below.

| Theme | Title | Issues | Root | Domain | Total | Migration priority |
|-------|-------|-------:|-----:|-------:|------:|--------------------|
| `arena_compaction` | Arena / compaction / GC | 0 | 0 | 35 | 35 | P0 — well-contained, batch drivers already exist |
| `mutation_dirty` | Mutation / dirty propagation / provenance | 0 | 0 | 83 | 83 | P0 — high volume; strong domain suite foothold |
| `fiber_orch` | Fiber / orchestration / steal / Guard | 0 | 0 | 36 | 36 | P1 — domain suite already collapses many obs gates |
| `linear_ownership` | Linear ownership / borrow / consume | 0 | 0 | 7 | 7 | P1 — small, already partially batched |
| `edsl_hygiene` | EDSL / macro hygiene / reflect | 0 | 0 | 21 | 21 | P1 — domain hygiene suite exists |
| `jit_incremental` | JIT / AOT / incremental relower | 0 | 0 | 32 | 32 | P2 — link-profile heavy; migrate AC smoke first |
| `shape_soa` | Shape / SoA / column layout | 0 | 0 | 17 | 17 | P2 — small-medium; soa_batch precedent |
| `observability` | Observability / metrics / query:*-stats | 0 | 0 | 86 | 86 | P2 — often thin schema probes; collapse into obs matrix |
| `uncategorized` | Uncategorized / mixed | 0 | 0 | 27 | 27 | P3 — review case-by-case |

## Patterns, harness usage, coupling

### Harness / entry-point patterns (`tests/issues/` only)

| Pattern | Count | Meaning |
|---------|------:|---------|

### `@category` distribution (issues/)


### Top includes (first 50 lines, issues/)


### Top module imports (first 50 lines, issues/)


### Coupling notes

1. **CompilerService-heavy** (~majority of issues/): most legacy tests are integration-style closed loops (eval → mutate → query stats). Domain migration should keep a shared CS fixture, not re-copy setup.
2. **Observability dual-path**: many files named `*_observability.cpp` or probing `query:*-stats` / `engine:metrics`. Prefer folding into `tests/compiler/obs_schema_cases.hpp` + `test_obs_schema_matrix.cpp`.
3. **Bundle link profiles** (`light` / `jit` / `fiber` / `*_late*`): physical file location still `tests/issues/`; migration must update `issue_link_profiles.json` / CMake when deleting sources.
4. **Internal headers**: direct includes of `compiler/observability_metrics.h`, `serve/fiber.h`, `compiler/aura_jit*.h` couple tests to private surfaces — domain suites should prefer public query/primitives where possible.
5. **Existing consolidation path**: family `*_batch.cpp` drivers under `tests/` (listed in `AuraDomainTests.cmake`) are the intermediate step; domain suites are the long-term home.

## Multi-file issues, phase slices, low-value signals

- Issue numbers with **multiple** `tests/issues/` files: **0**
- Phase-slice files (`*_phase*`): **0**
- Small files (< 4 KiB, possible thin probes): **0**
- Existing `*_batch` drivers (migration milestones): **65**

### Multi-file issue groups (consolidate first)


### Smallest issue tests (triage for obs-matrix fold or drop)


### Batch drivers already present

- `tests/core/test_arena_batch.cpp` → theme `arena_compaction`
- `tests/compiler/test_atomic_batch_core_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_atomic_batch_rollback_closed_loop.cpp` → theme `mutation_dirty`
- `tests/compiler/test_atomic_batch_rollback_fiber_task1.cpp` → theme `mutation_dirty`
- `tests/compiler/test_atomic_batch_snapshot_stable_ref_ai_loops.cpp` → theme `mutation_dirty`
- `tests/compiler/test_build_kv_hash_batch.cpp` → theme `jit_incremental`
- `tests/core/test_capability_sandbox_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_closure_batch.cpp` → theme `arena_compaction`
- `tests/compiler/test_closure_view_batch.cpp` → theme `observability`
- `tests/compiler/test_dead_coercion_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_dead_coercion_elision_narrow_batch.cpp` → theme `observability`
- `tests/compiler/test_env_batch.cpp` → theme `arena_compaction`
- `tests/compiler/test_env_lookup_batch.cpp` → theme `fiber_orch`
- `tests/compiler/test_envframe_epoch_batch.cpp` → theme `arena_compaction`
- `tests/compiler/test_epoch_apply_batch.cpp` → theme `arena_compaction`
- `tests/serve/test_fiber_concurrent_unit_batch.cpp` → theme `fiber_orch`
- `tests/serve/test_fiber_integration_batch.cpp` → theme `fiber_orch`
- `tests/serve/test_fiber_orch_core_batch.cpp` → theme `fiber_orch`
- `tests/serve/test_fiber_orch_parallel_quota_batch.cpp` → theme `fiber_orch`
- `tests/compiler/test_fiber_resume_batch.cpp` → theme `fiber_orch`
- `tests/serve/test_fiber_strategy_evolve_batch.cpp` → theme `fiber_orch`
- `tests/serve/test_fiber_synthesize_batch.cpp` → theme `fiber_orch`
- `tests/serve/test_gc_batch.cpp` → theme `arena_compaction`
- `tests/serve/test_gc_compact_batch.cpp` → theme `arena_compaction`
- `tests/serve/test_gc_compact_sweep_batch.cpp` → theme `arena_compaction`
- `tests/core/test_guard_dtor_batch_metrics.cpp` → theme `mutation_dirty`
- `tests/core/test_hotpath_matrix_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_incremental_relower_batch.cpp` → theme `jit_incremental`
- `tests/compiler/test_incremental_type_batch.cpp` → theme `jit_incremental`
- `tests/compiler/test_inline_pass_batch.cpp` → theme `jit_incremental`
- `tests/compiler/test_ir_soa_dual_emit_batch.cpp` → theme `shape_soa`
- `tests/compiler/test_issues_809_817_batch.cpp` → theme `fiber_orch`
- `tests/compiler/test_issues_819_829_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_jit_aot_hot_update_unit_batch.cpp` → theme `jit_incremental`
- `tests/compiler/test_jit_batch_deopt_clear.cpp` → theme `jit_incremental`
- `tests/compiler/test_linear_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_linear_ownership_batch.cpp` → theme `linear_ownership`
- `tests/compiler/test_macro_reflect_batch.cpp` → theme `edsl_hygiene`
- `tests/compiler/test_mutate_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_mutation_aot_unit_batch.cpp` → theme `observability`
- `tests/compiler/test_mutation_boundary_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_mutation_guard_unit_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_mutation_occurrence_dirty_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_mutation_typed_audit_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_obs_metrics_smoke_batch.cpp` → theme `observability`
- `tests/core/test_open_issues_meta_batch.cpp` → theme `observability`
- `tests/compiler/test_open_issues_phase1_batch.cpp` → theme `uncategorized`
- `tests/core/test_panic_checkpoint_batch.cpp` → theme `uncategorized`
- `tests/compiler/test_per_defuse_batch.cpp` → theme `fiber_orch`
- `tests/compiler/test_production_readiness_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_query_pattern_batch.cpp` → theme `edsl_hygiene`
- `tests/compiler/test_reflect_batch.cpp` → theme `edsl_hygiene`
- `tests/reflect/test_reflect_hygiene_unit_batch.cpp` → theme `edsl_hygiene`
- `tests/reflect/test_reflect_macro_hygiene_batch.cpp` → theme `edsl_hygiene`
- `tests/reflect/test_reflect_pattern_hygiene_batch.cpp` → theme `edsl_hygiene`
- `tests/renderer/test_render_batch.cpp` → theme `arena_compaction`
- `tests/core/test_resource_quota_batch.cpp` → theme `arena_compaction`
- `tests/compiler/test_shape_soa_unit_batch.cpp` → theme `shape_soa`
- `tests/core/test_soa_batch.cpp` → theme `shape_soa`
- `tests/compiler/test_stable_ref_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_stable_ref_cow_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_stable_ref_provenance_batch.cpp` → theme `mutation_dirty`
- `tests/repl/test_terminal_domain_batch.cpp` → theme `uncategorized`
- `tests/compiler/test_typechecker_incremental_batch.cpp` → theme `jit_incremental`
- `tests/compiler/test_walk_batch.cpp` → theme `mutation_dirty`

### Domain suites (do not regress; extend these)

- `tests/compiler/test_adt_match_exhaust_post_mutate_reliability.cpp`
- `tests/compiler/test_adt_match_exhaustiveness_incremental_task2.cpp`
- `tests/renderer/test_ai_closedloop_readiness.cpp`
- `tests/compiler/test_aot_bridge_checkpoint_version_steal.cpp`
- `tests/compiler/test_aot_incremental_reemit.cpp`
- `tests/compiler/test_aot_mangle_top.cpp`
- `tests/compiler/test_aot_region_per_eval.cpp`
- `tests/compiler/test_aot_reload_primitive.cpp`
- `tests/compiler/test_aot_shell_c0_escape.cpp`
- `tests/compiler/test_aot_stats_null_metrics.cpp`
- `tests/compiler/test_apply_closure_envframe_soa.cpp`
- `tests/core/test_arena_auto_compact_fiber_defag_shape_dirty_closedloop.cpp`
- `tests/core/test_arena_auto_compact_intelligent.cpp`
- `tests/core/test_arena_batch.cpp`
- `tests/core/test_arena_compact_hook_concurrent.cpp`
- `tests/core/test_arena_concurrent_mutex.cpp`
- `tests/core/test_arena_defrag.cpp`
- `tests/core/test_arena_lifecycle.cpp`
- `tests/compiler/test_arithmetic_int64_safety.cpp`
- `tests/compiler/test_ast_column_compaction_closed_loop.cpp`
- `tests/core/test_ast_ops_stats_workspace_lock.cpp`
- `tests/compiler/test_ast_workspace_modules.cpp`
- `tests/compiler/test_atomic_batch_core_batch.cpp`
- `tests/compiler/test_atomic_batch_rollback_closed_loop.cpp`
- `tests/compiler/test_atomic_batch_rollback_fiber_task1.cpp`
- `tests/compiler/test_atomic_batch_snapshot_stable_ref_ai_loops.cpp`
- `tests/stdlib/test_atomic_swap_stdlib.cpp`
- `tests/compiler/test_aura_result_error_policy.cpp`
- `tests/compiler/test_auto_evolve_closure_live.cpp`
- `tests/compiler/test_bidirectional_annotation.cpp`
- `tests/core/test_bidirectional_stats.cpp`
- `tests/compiler/test_blame_stamp_on_degrade.cpp`
- `tests/compiler/test_blame_tracking_typed_mutate.cpp`
- `tests/compiler/test_bugfix_968.cpp`
- `tests/compiler/test_build_kv_hash_batch.cpp`
- `tests/renderer/test_camera_rays.cpp`
- `tests/compiler/test_capability_gating.cpp`
- `tests/core/test_capability_sandbox_batch.cpp`
- `tests/compiler/test_closedloop_stats_hash_cap.cpp`
- `tests/compiler/test_closure_batch.cpp`
- `tests/compiler/test_closure_bridge_lifetime.cpp`
- `tests/compiler/test_closure_free.cpp`
- `tests/compiler/test_closure_view_batch.cpp`
- `tests/compiler/test_coercion_dead_elim_castop_flow_zerooverhead.cpp`
- `tests/compiler/test_compile02_no_dup_imports.cpp`
- `tests/compiler/test_compile_primitive_guard.cpp`
- `tests/compiler/test_compiler_closure_env_safety_post_invalidate.cpp`
- `tests/compiler/test_compiler_core_incremental_selfmod_gaps.cpp`
- `tests/core/test_compiler_metrics_ownership.cpp`
- `tests/compiler/test_compiler_service_ownership.cpp`
- `tests/compiler/test_composite_nested_txn_invariant_audit.cpp`
- `tests/compiler/test_composite_typed_mutate.cpp`
- `tests/compiler/test_concept_constraints.cpp`
- `tests/serve/test_concurrent.cpp`
- `tests/compiler/test_constraint_solver_surface_cross_delta.cpp`
- `tests/compiler/test_constraint_system_solve_delta_cross_delta_task2.cpp`
- `tests/compiler/test_constraintsystem_solve_delta_clean_conflict_detection.cpp`
- `tests/compiler/test_contracts.cpp`
- `tests/compiler/test_core_builtins_review.cpp`
- `tests/core/test_coverage_holes_workspace_lock.cpp`
- `tests/core/test_cpp26_contracts_hotpath.cpp`
- `tests/compiler/test_cpp26_contracts_hotpath_arena_soa_value_shape_pass.cpp`
- `tests/stdlib/test_datetime.cpp`
- `tests/compiler/test_dead_coercion_batch.cpp`
- `tests/compiler/test_dead_coercion_elim.cpp`
- `tests/compiler/test_dead_coercion_elision_narrow_batch.cpp`
- `tests/compiler/test_dead_coercion_pipeline_wire.cpp`
- `tests/compiler/test_defuse_version_closed_loop.cpp`
- `tests/core/test_dep_graph_concurrent.cpp`
- `tests/compiler/test_dirty_cascade_optimize.cpp`
- `tests/compiler/test_dirty_delta_present.cpp`
- `tests/compiler/test_dirty_propagation_cascade.cpp`
- `tests/compiler/test_dirty_propagation_cost_closed_loop.cpp`
- `tests/compiler/test_dirty_reason_verification_propagation.cpp`
- `tests/compiler/test_edsl_concurrent_fiber_boundary_task1.cpp`
- `tests/compiler/test_edsl_concurrent_query_mutate.cpp`
- `tests/compiler/test_edsl_core_stability_cow_atomic_query_mutate.cpp`
- `tests/compiler/test_edsl_query_mutate_commercial_closed_loop.cpp`
- `tests/compiler/test_engine_metrics_facade.cpp`
- `tests/compiler/test_env_batch.cpp`
- `tests/compiler/test_env_lookup_batch.cpp`
- `tests/compiler/test_envframe_bridge_invalidate.cpp`
- `tests/compiler/test_envframe_dualpath_stale_closed_loop.cpp`
- `tests/compiler/test_envframe_epoch_batch.cpp`
- `tests/compiler/test_envframe_resolve_distinction.cpp`
- `tests/compiler/test_envframe_truncate_epoch.cpp`
- `tests/core/test_envframe_truncate_guard_dual_epoch.cpp`
- `tests/compiler/test_epoch_apply_batch.cpp`
- `tests/reflect/test_error_merr.cpp`
- `tests/compiler/test_eval_relower_hotpath.cpp`
- `tests/serve/test_fiber_concurrent_unit_batch.cpp`
- `tests/serve/test_fiber_integration_batch.cpp`
- `tests/compiler/test_fiber_macro_hygiene_refresh.cpp`
- `tests/serve/test_fiber_mutation_steal_safety.cpp`
- `tests/serve/test_fiber_orch_core_batch.cpp`
- `tests/serve/test_fiber_orch_parallel_quota_batch.cpp`
- `tests/compiler/test_fiber_resume_batch.cpp`
- `tests/serve/test_fiber_steal_panic_checkpoint_nested_gc.cpp`
- `tests/serve/test_fiber_strategy_evolve_batch.cpp`
- `tests/serve/test_fiber_synthesize_batch.cpp`
- `tests/compiler/test_fine_dirty_relower.cpp`
- `tests/compiler/test_followup_smoke.cpp`
- `tests/compiler/test_followups.cpp`
- `tests/compiler/test_full_strategy_partial_recovery.cpp`
- `tests/serve/test_gc_batch.cpp`
- `tests/serve/test_gc_compact_batch.cpp`
- `tests/serve/test_gc_compact_sweep_batch.cpp`
- `tests/core/test_gc_evaluator_integration.cpp`
- `tests/core/test_guard_dtor_batch_metrics.cpp`
- `tests/serve/test_guard_panic_reflect_fiber_resume_task6.cpp`
- `tests/compiler/test_hardware_resource_linear_ownership.cpp`
- `tests/core/test_hash_iter_invalidation.cpp`
- `tests/compiler/test_highperf_cpp26_gaps_arena_soa_value_shape_pass.cpp`
- `tests/core/test_highperf_full_hotpath_matrix.cpp`
- `tests/core/test_hotpath_matrix_batch.cpp`
- `tests/compiler/test_incremental_effectiveness_snapshot_fail.cpp`
- `tests/compiler/test_incremental_perblock_closure_bridge_safety.cpp`
- `tests/compiler/test_incremental_relower_batch.cpp`
- `tests/compiler/test_incremental_type_batch.cpp`
- `tests/compiler/test_incremental_typed_selfmod_dirty_narrowing.cpp`
- `tests/compiler/test_inline_pass_batch.cpp`
- `tests/compiler/test_inline_typecheck_exception.cpp`
- `tests/serve/test_inner_steal_starvation.cpp`
- `tests/compiler/test_invalidate_cascade_order.cpp`
- `tests/compiler/test_invalidate_consistency.cpp`
- `tests/compiler/test_invalidations_stats_workspace_lock.cpp`
- `tests/compiler/test_ir.cpp`
- `tests/reflect/test_ir_cache_v2.cpp`
- `tests/compiler/test_ir_metadata_interpreter_jit_closed_loop.cpp`
- `tests/compiler/test_ir_soa_dual_emit_batch.cpp`
- `tests/compiler/test_ir_soa_incremental_closed_loop.cpp`
- `tests/reflect/test_issue_178.cpp`
- `tests/reflect/test_issue_178_reflect.cpp`
- `tests/serve/test_issue_1990.cpp`
- `tests/serve/test_issue_1991.cpp`
- `tests/serve/test_issue_1992.cpp`
- `tests/serve/test_issue_1993.cpp`
- `tests/compiler/test_issues_809_817_batch.cpp`
- `tests/compiler/test_issues_819_829_batch.cpp`
- `tests/compiler/test_jit_aot_hot_update_unit_batch.cpp`
- `tests/compiler/test_jit_batch_deopt_clear.cpp`
- `tests/compiler/test_jit_closure_cache_race.cpp`
- `tests/compiler/test_jit_concurrent_compile.cpp`
- `tests/compiler/test_jit_consistency.cpp`
- `tests/compiler/test_jit_critical_coverage.cpp`
- `tests/compiler/test_jit_full_opcode_coverage.cpp`
- `tests/compiler/test_jit_macro_introduced_preserve.cpp`
- `tests/compiler/test_jit_metrics.cpp`
- `tests/compiler/test_jit_metrics_stub.cpp`
- `tests/compiler/test_let_poly_solve_delta.cpp`
- `tests/compiler/test_linear_batch.cpp`
- `tests/compiler/test_linear_boundary_consistency.cpp`
- `tests/compiler/test_linear_live_closure_walk.cpp`
- `tests/compiler/test_linear_ownership_batch.cpp`
- `tests/compiler/test_linear_ownership_occurrence_predicate_mutate.cpp`
- `tests/compiler/test_linear_ownership_postmutate_guard_steal_envframe.cpp`
- `tests/compiler/test_linear_provenance_steal_gc_closed_loop.cpp`
- `tests/compiler/test_linear_runtime_violation.cpp`
- `tests/compiler/test_linear_walk_active_closures.cpp`
- `tests/compiler/test_list_vector_soa_hotpath_ai_loops.cpp`
- `tests/core/test_lock_hierarchy.cpp`
- `tests/compiler/test_lock_order_closures_env.cpp`
- `tests/compiler/test_longrunning_infra_primitives.cpp`
- `tests/compiler/test_longrunning_recovery_latency.cpp`
- `tests/compiler/test_lookup_stats_impl_heterogeneous.cpp`
- `tests/compiler/test_macro_hygiene_closedloop_health.cpp`
- `tests/compiler/test_macro_hygiene_depth_concurrent_obs.cpp`
- `tests/compiler/test_macro_hygiene_fiber_panic_aot_soa_self_evo.cpp`
- `tests/compiler/test_macro_reflect_batch.cpp`
- `tests/compiler/test_macro_restamp_after_flat.cpp`
- `tests/compiler/test_macro_self_evo_capability.cpp`
- `tests/core/test_marker_metadata_lock.cpp`
- `tests/compiler/test_matcher_stable_captures.cpp`
- `tests/core/test_module_boundary.cpp`
- `tests/compiler/test_module_export_cache.cpp`
- `tests/compiler/test_module_loader_dead_heap_circular.cpp`
- `tests/compiler/test_module_prefix_dead_heap.cpp`
- `tests/compiler/test_mutate_batch.cpp`
- `tests/compiler/test_mutate_cross_thread_migration.cpp`
- `tests/compiler/test_mutation_aot_unit_batch.cpp`
- `tests/compiler/test_mutation_audit_wal.cpp`
- `tests/compiler/test_mutation_boundary_batch.cpp`
- `tests/serve/test_mutation_boundary_guard.cpp`
- `tests/serve/test_mutation_guard_try_acquire.cpp`
- `tests/compiler/test_mutation_guard_unit_batch.cpp`
- `tests/serve/test_mutation_hold_time.cpp`
- `tests/compiler/test_mutation_log_query_race.cpp`
- `tests/compiler/test_mutation_occurrence_dirty_batch.cpp`
- `tests/compiler/test_mutation_provenance.cpp`
- `tests/compiler/test_mutation_rollback_coverage.cpp`
- `tests/compiler/test_mutation_systemic_guard.cpp`
- `tests/compiler/test_mutation_typed_audit_batch.cpp`
- `tests/compiler/test_mutator_dispatch_stats_lock.cpp`
- `tests/compiler/test_obs_metrics_smoke_batch.cpp`
- `tests/compiler/test_obs_schema_matrix.cpp`
- `tests/compiler/test_observability_tier_table.cpp`
- `tests/compiler/test_occ_cache_stats_wired.cpp`
- `tests/compiler/test_occurrence_dirty_blame_post_mutate.cpp`
- `tests/compiler/test_occurrence_mutate_narrowing.cpp`
- `tests/compiler/test_occurrence_provenance_chain_completeness.cpp`
- `tests/compiler/test_occurrence_typing_blame_post_mutate_recovery.cpp`
- `tests/compiler/test_occurrence_typing_blame_post_mutate_task2.cpp`
- `tests/core/test_open_issues_meta_batch.cpp`
- `tests/compiler/test_open_issues_phase1_batch.cpp`
- `tests/compiler/test_optimization_passes_contracts.cpp`
- `tests/serve/test_orchestration_steal_boost.cpp`
- `tests/core/test_pair_slot_lock.cpp`
- `tests/core/test_pair_unchecked_safety.cpp`
- `tests/core/test_panic_checkpoint_batch.cpp`
- `tests/serve/test_panic_checkpoint_fiber_resume_safety.cpp`
- `tests/compiler/test_pass_contracts_hotpath_closed_loop.cpp`
- `tests/compiler/test_pattern_structural_index_closed_loop.cpp`
- `tests/compiler/test_per_defuse_batch.cpp`
- `tests/serve/test_per_fiber_stack_pool_high_concurrency.cpp`
- `tests/compiler/test_per_symbol_dirty_cycle_guard.cpp`
- `tests/core/test_per_symbol_dirty_pool_lock.cpp`
- `tests/core/test_persist_basic.cpp`
- `tests/renderer/test_pixel_framebuffer.cpp`
- `tests/serve/test_post_steal_closed_loop.cpp`
- `tests/core/test_prim_call_count_clamp.cpp`
- `tests/compiler/test_primitive_meta_self_describing_closed_loop.cpp`
- `tests/core/test_primitive_resource_quota_stats.cpp`
- `tests/compiler/test_primitives_capture_contract.cpp`
- `tests/compiler/test_primitives_hotpath_registry_slo.cpp`
- `tests/compiler/test_primitives_registry_core_consistency.cpp`
- `tests/compiler/test_primitives_surface_convergence.cpp`
- `tests/compiler/test_production_hardening_985.cpp`
- `tests/compiler/test_production_readiness_batch.cpp`
- `tests/compiler/test_production_roadmap_closed_loop.cpp`
- `tests/compiler/test_production_safety.cpp`
- `tests/compiler/test_production_safety_1047.cpp`
- `tests/compiler/test_production_safety_1097.cpp`
- `tests/compiler/test_production_stability_1014.cpp`
- `tests/compiler/test_production_sweep.cpp`
- `tests/serve/test_production_sweep.cpp`
- `tests/compiler/test_prompt2_6_impact_scope_quote_lambda_bridge_env.cpp`
- `tests/serve/test_prompt6_epoch_atomic_visibility_fiber_steal.cpp`
- `tests/compiler/test_prompt6_full_memory_safety_fuzz_stress.cpp`
- `tests/compiler/test_prompt6_linear_jit_l2_post_invalidate_arena_gc.cpp`
- `tests/compiler/test_propagate_marker_cycle_guard.cpp`
- `tests/compiler/test_provenance_blame_hygiene.cpp`
- `tests/compiler/test_query_dispatch.cpp`
- `tests/compiler/test_query_mutate_consistency.cpp`
- `tests/compiler/test_query_namespace_audit.cpp`
- `tests/compiler/test_query_pattern_batch.cpp`
- `tests/compiler/test_query_pattern_hygiene_macrointroduced.cpp`
- `tests/compiler/test_quota_edge_cases.cpp`
- `tests/core/test_raw_pointer_safety.cpp`
- `tests/compiler/test_refinement_closed_loop.cpp`
- `tests/compiler/test_reflect_batch.cpp`
- `tests/reflect/test_reflect_hygiene_agent_diagnostics.cpp`
- `tests/reflect/test_reflect_hygiene_unit_batch.cpp`
- `tests/reflect/test_reflect_macro_hygiene_batch.cpp`
- `tests/reflect/test_reflect_pattern_hygiene_batch.cpp`
- `tests/compiler/test_relower_strategy_cache_lock.cpp`
- `tests/renderer/test_render3d_primitives.cpp`
- `tests/renderer/test_render_ai_native_template.cpp`
- `tests/renderer/test_render_batch.cpp`
- `tests/compiler/test_render_dispatch_linear_epoch.cpp`
- `tests/renderer/test_render_ffi_hotpath.cpp`
- `tests/renderer/test_render_hotpath_observability.cpp`
- `tests/renderer/test_render_hotpath_stability_under_mutation.cpp`
- `tests/renderer/test_render_mutation_checkpoint.cpp`
- `tests/renderer/test_render_pass_incremental.cpp`
- `tests/renderer/test_render_telemetry.cpp`
- `tests/core/test_resource_quota_batch.cpp`
- `tests/compiler/test_rest_param_hygiene_self_evo.cpp`
- `tests/core/test_root_epoch_gc_safety_post_invalidate.cpp`
- `tests/compiler/test_runtime_concurrent_full_cycle_chaos.cpp`
- `tests/serve/test_runtime_mutation_boundary_steal_safety.cpp`
- `tests/compiler/test_runtime_observability_correlated_stats.cpp`
- `tests/compiler/test_safe_snapshot_umbrella.cpp`
- `tests/serve/test_safe_yield_orchestration.cpp`
- `tests/serve/test_safepoint_mutation.cpp`
- `tests/compiler/test_scan_skip_freed_closures.cpp`
- `tests/serve/test_scheduler_gc_defer_pending_panic_steal.cpp`
- `tests/serve/test_scheduler_gc_safepoint_mutation_coordination.cpp`
- `tests/serve/test_scheduler_llm_bottleneck_adaptive_steal_gc.cpp`
- `tests/compiler/test_self_evo_stats.cpp`
- `tests/serve/test_self_evolution_chaos_stable.cpp`
- `tests/compiler/test_self_evolution_loop_stats.cpp`
- `tests/serve/test_self_heal_policy_engine.cpp`
- `tests/compiler/test_selfevo_bugfix_941.cpp`
- `tests/core/test_set_arena_atomic_owner.cpp`
- `tests/core/test_set_workspace_flat.cpp`
- `tests/compiler/test_seva_demo_metrics.cpp`
- `tests/compiler/test_shape.cpp`
- `tests/compiler/test_shape_jit_pass_deopt_incremental_closedloop_ai_mutate.cpp`
- `tests/compiler/test_shape_linear_collaborative_pass.cpp`
- `tests/compiler/test_shape_profiler_burst_closed_loop.cpp`
- `tests/compiler/test_shape_profiler_stability_deopt_fiber_task4.cpp`
- `tests/compiler/test_shape_soa_unit_batch.cpp`
- `tests/compiler/test_shapeprofiler_stability_deopt_jit_mutate.cpp`
- `tests/core/test_soa_batch.cpp`
- `tests/compiler/test_soa_view_enforcement.cpp`
- `tests/compiler/test_solve_delta_epoch_filter.cpp`
- `tests/compiler/test_spec_jit.cpp`
- `tests/stdlib/test_spec_runtime.cpp`
- `tests/compiler/test_stable_ref_batch.cpp`
- `tests/compiler/test_stable_ref_cow_batch.cpp`
- `tests/compiler/test_stable_ref_provenance_batch.cpp`
- `tests/serve/test_stable_ref_provenance_fiber_cow.cpp`
- `tests/compiler/test_stale_closure_fallback.cpp`
- `tests/core/test_stale_ref_string_heap.cpp`
- `tests/compiler/test_static_reflect_selfmod_validation_task6.cpp`
- `tests/compiler/test_stats_catalog_drift.cpp`
- `tests/compiler/test_stats_facade_bench.cpp`
- `tests/compiler/test_stats_module_unification.cpp`
- `tests/stdlib/test_stdlib_infrastructure.cpp`
- `tests/compiler/test_stdlib_production_review_923.cpp`
- `tests/core/test_stress_alloc_storage_lock.cpp`
- `tests/stdlib/test_synthesize_namespace_demotion.cpp`
- `tests/core/test_tenant_isolation_enforcement.cpp`
- `tests/renderer/test_terminal_concurrent.cpp`
- `tests/renderer/test_terminal_deprecation.cpp`
- `tests/repl/test_terminal_domain_batch.cpp`
- `tests/renderer/test_terminal_lifecycle.cpp`
- `tests/renderer/test_terminal_render_production.cpp`
- `tests/compiler/test_test_strategy.cpp`
- `tests/compiler/test_tier_dispatch.cpp`
- `tests/core/test_try_lock_workspace_lock_order.cpp`
- `tests/core/test_type_cache_stats_snapshot.cpp`
- `tests/compiler/test_type_prop_invariant_correlation.cpp`
- `tests/compiler/test_type_propagation_dead_coercion.cpp`
- `tests/core/test_type_registry_ownership.cpp`
- `tests/compiler/test_typechecker_incremental_batch.cpp`
- `tests/compiler/test_typesystem_solve_delta_occurrence_priority_heavy_mutate.cpp`
- `tests/compiler/test_typesystem_type_propagation_jit_l2_typed_mutate.cpp`
- `tests/compiler/test_typesystem_typed_mutate_incremental_gaps.cpp`
- `tests/compiler/test_unified_invalidation.cpp`
- `tests/compiler/test_unify_invalidate_try_acquire.cpp`
- `tests/compiler/test_value_encoding_v2_dispatch_contracts.cpp`
- `tests/compiler/test_verify_parse_shared_helper.cpp`
- `tests/renderer/test_voxel_frame.cpp`
- `tests/renderer/test_voxel_raycast.cpp`
- `tests/renderer/test_voxel_shade.cpp`
- `tests/renderer/test_voxel_volume.cpp`
- `tests/compiler/test_walk_batch.cpp`
- `tests/compiler/test_workspace_delete_child.cpp`
- `tests/compiler/test_workspace_dispatch.cpp`
- `tests/core/test_workspace_lock_reentrancy.cpp`
- `tests/core/test_workspace_state_lock.cpp`
- `tests/compiler/test_workspace_swap_guard.cpp`
- `tests/core/test_zero_copy_arena.cpp`

## Migration priority roadmap

Suggested order starts with well-contained groups (per #1957) and leverages existing batch/domain footholds. Each wave should:

1. Pick a theme slice (or multi-file issue group).
2. Port ACs into a domain suite or family batch driver.
3. Delete or EXCLUDE the old `test_issue_*.cpp` + update bundles/CMake.
4. Re-run this inventory script; commit the refreshed markdown.

| Wave | Theme / slice | Why first | Suggested follow-up issue |
|-----:|---------------|-----------|---------------------------|
| 1 | `arena_compaction` + compact/gc batches | Contained core; `test_compact_batch` / `test_compact_sweep_batch` / `test_gc_batch` exist | Open: *Migrate arena/compaction issue tests → domain* |
| 2 | Multi-file phase groups (#436, #435, #501, #411) | Obvious consolidate wins (same issue, many mains) | Open: *Collapse phase/followup issue test clusters* |
| 3 | `mutation_dirty` thin obs probes | Largest issues/ bucket; domain typed-mutate + mutation_boundary batch | Open: *Mutation/dirty issue tests → domain* |
| 4 | `fiber_orch` remaining gates | Domain fiber orchestration suite already swallowed #810/#812/#813/#875-style checks | Open: *Finish fiber/orch obs migration* |
| 5 | `linear_ownership` + `shape_soa` | Small counts; batch drivers exist | Open: *Linear + SoA batch → domain* |
| 6 | `edsl_hygiene` | Domain hygiene suite + macro_reflect batch | Open: *Hygiene/EDSL issue tests → domain* |
| 7 | `observability` schema-only files | Fold into `obs_schema_cases.hpp` matrix | Open: *Obs schema matrix completion* |
| 8 | `jit_incremental` smoke ACs | Keep heavy JIT stress in bundles; move light AC gates only | Open: *JIT/incremental AC smoke → domain* |
| 9 | `uncategorized` + early_issue (<#200) | Manual triage; some may be obsolete vs suite/regression | Open: *Legacy early-issue triage* |

### Acceptance checkpoints per wave

- No new `test_issue_*.cpp` introduced.
- Domain or batch binary covers former ACs (or intentional drop documented).
- `python3 scripts/inventory_legacy_tests.py --check` stays green after refresh.
- Bundle profiles / CMake targets updated when sources removed.

## Per-theme file lists

Files listed as ``location/name`` with issue id and one-line summary.

### `arena_compaction` — Arena / compaction / GC (35)

**Target:** tests/core/ (extend compact/gc family; see test_arena_batch / test_hotpath_matrix_batch)

**Priority:** P0 — well-contained, batch drivers already exist

#### domain/ (35)

- `tests/compiler/test_adt_match_exhaust_post_mutate_reliability.cpp` (—) [domain_suite, theme_compiler] — test_adt_match_exhaust_post_mutate_reliability.cpp — Issue #612:
- `tests/core/test_arena_auto_compact_fiber_defag_shape_dirty_closedloop.cpp` (—) [domain_suite, theme_core] — (aura_issue_arena_auto_compact_fiber_defag_shape_dirty_closedloop_run). Stays at tests/core/ per
- `tests/core/test_arena_auto_compact_intelligent.cpp` (—) [domain_suite, theme_core] — Issue #1242/#1621/#187/#1919/#300 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_arena_batch.cpp` (—) [large, batch_driver, domain_suite, theme_core] — tests/core/test_arena_batch.cpp — consolidated arena batch driver. EXCLUDE_FROM_ALL.
- `tests/core/test_arena_compact_hook_concurrent.cpp` (—) [small, domain_suite, theme_core] — test_arena_compact_hook_concurrent.cpp — Issue #1989: ASTArena::on_compact_hook_
- `tests/core/test_arena_concurrent_mutex.cpp` (—) [small, domain_suite, theme_core] — test_arena_concurrent_mutex.cpp — Issue #1988: ArenaGroup::arenas_ concurrent access.
- `tests/core/test_arena_defrag.cpp` (—) [domain_suite, theme_core] — tests/core/test_arena_defrag.cpp — Issue #1390: request_defrag + safepoint contract test.
- `tests/core/test_arena_lifecycle.cpp` (—) [domain_suite, theme_core] — test_arena_lifecycle.cpp — Merged #1947/#1954 + #300 + #1359 (Anqi 2026-07-21).
- `tests/compiler/test_ast_column_compaction_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #261/#405/#414/#416 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_bidirectional_annotation.cpp` (—) [domain_suite, theme_compiler] — tests/test_bidirectional_annotation.cpp — Issue #1413: True
- `tests/compiler/test_closure_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_closure_batch.cpp
- `tests/stdlib/test_datetime.cpp` (—) [domain_suite, theme_stdlib] — test_datetime.cpp — Merged datetime stdlib tests (#1978).
- `tests/compiler/test_env_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_env_batch.cpp
- `tests/compiler/test_envframe_epoch_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_envframe_epoch_batch.cpp — EnvFrame / bridge_epoch batch driver.
- `tests/compiler/test_envframe_truncate_epoch.cpp` (—) [domain_suite, theme_compiler] — Issue #1842/#1889 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_envframe_truncate_guard_dual_epoch.cpp` (—) [domain_suite, theme_core] — Issue #1739/#1842/#1889/#1927/#1948 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_epoch_apply_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_epoch_apply_batch.cpp — epoch_apply pair dup-merge (R19 phase 15).
- `tests/serve/test_gc_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — tests/serve/test_gc_batch.cpp — GC batch driver (arena theme; default-build).
- `tests/serve/test_gc_compact_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — tests/serve/test_gc_compact_batch.cpp — GC compact family batch driver.
- `tests/serve/test_gc_compact_sweep_batch.cpp` (—) [batch_driver, domain_suite, theme_serve] — tests/serve/test_gc_compact_sweep_batch.cpp — GC compact sweep batch driver.
- `tests/core/test_gc_evaluator_integration.cpp` (—) [domain_suite, theme_core] — test_gc_evaluator_integration.cpp — Issue #113 verification
- `tests/core/test_highperf_full_hotpath_matrix.cpp` (—) [domain_suite, theme_core] — test_task4_highperf_full_hotpath_matrix.cpp — Issue #607:
- `tests/compiler/test_ir.cpp` (—) [large, domain_suite, theme_compiler] — 
- `tests/serve/test_issue_1990.cpp` (#1990) [small, domain_suite, theme_serve] — test_issue_1990.cpp — Issue #1990 / B-009: (gc-temp) and (gc-stats)
- `tests/serve/test_issue_1991.cpp` (#1991) [small, domain_suite, theme_serve] — test_issue_1991.cpp — Issue #1991 / B-010: (gc) primitive clears
- `tests/serve/test_issue_1993.cpp` (#1993) [domain_suite, theme_serve] — test_issue_1993.cpp — Issue #1993 (D-001): (gc-heap) direct-clear
- `tests/compiler/test_prompt6_linear_jit_l2_post_invalidate_arena_gc.cpp` (—) [domain_suite, theme_compiler] — test_prompt6_linear_jit_l2_post_invalidate_arena_gc.cpp — Issue #740:
- `tests/compiler/test_quota_edge_cases.cpp` (—) [domain_suite, theme_compiler] — AC1: boundary 0→1 transition (unlimited → bounded reject)
- `tests/renderer/test_render_batch.cpp` (—) [batch_driver, domain_suite, theme_renderer] — test_render_batch.cpp — Merged #1675 + #1559 (Anqi 2026-07-21).
- `tests/core/test_resource_quota_batch.cpp` (—) [large, batch_driver, domain_suite, theme_core] — tests/core/test_resource_quota_batch.cpp
- `tests/core/test_root_epoch_gc_safety_post_invalidate.cpp` (—) [domain_suite, theme_core] — test_compiler_root_epoch_gc_safety_post_invalidate.cpp — Issue #599:
- `tests/core/test_set_arena_atomic_owner.cpp` (—) [domain_suite, theme_core] — test_set_arena_atomic_owner.cpp — Issue #1663
- `tests/compiler/test_solve_delta_epoch_filter.cpp` (—) [small, domain_suite, theme_compiler] — Issue #2065 — solve_delta epoch filter test.
- `tests/renderer/test_terminal_lifecycle.cpp` (—) [domain_suite, theme_renderer] — test_terminal_lifecycle.cpp — Issue #1352: delete/compact + use-after-delete
- `tests/core/test_zero_copy_arena.cpp` (—) [domain_suite, theme_core] — integration; no pair-alloc growth over 10k presents; concurrent fiber/thread.

### `mutation_dirty` — Mutation / dirty propagation / provenance (83)

**Target:** tests/core/test_mutation_boundary_batch (domain/ pilot abandoned in R1)

**Priority:** P0 — high volume; strong domain suite foothold

#### domain/ (83)

- `tests/compiler/test_adt_match_exhaustiveness_incremental_task2.cpp` (—) [domain_suite, theme_compiler] — test_adt_match_exhaustiveness_incremental_task2.cpp
- `tests/compiler/test_atomic_batch_core_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — R19 phase4 dup-merge — atomic-batch core trio: Issue #1899 (dispatch + STRONG atomicity) + Issue
- `tests/compiler/test_atomic_batch_rollback_closed_loop.cpp` (—) [batch_driver, domain_suite, theme_compiler] — Issue #192/#459/#529/#553 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_atomic_batch_rollback_fiber_task1.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_atomic_batch_rollback_fiber_task1.cpp —
- `tests/compiler/test_atomic_batch_snapshot_stable_ref_ai_loops.cpp` (—) [batch_driver, domain_suite, theme_compiler] — - AC1: workspace:snapshot + workspace:rollback-to primitives
- `tests/core/test_capability_sandbox_batch.cpp` (—) [large, batch_driver, domain_suite, theme_core] — tests/core/test_capability_sandbox_batch.cpp
- `tests/compiler/test_closure_bridge_lifetime.cpp` (—) [domain_suite, theme_compiler] — Issue #1888/#1895/#1926/#1928/#1929/#1947 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_coercion_dead_elim_castop_flow_zerooverhead.cpp` (—) [domain_suite, theme_compiler] — test_coercion_dead_elim_castop_flow_zerooverhead.cpp
- `tests/compiler/test_compiler_closure_env_safety_post_invalidate.cpp` (—) [domain_suite, theme_compiler] — test_compiler_closure_env_safety_post_invalidate.cpp —
- `tests/compiler/test_composite_nested_txn_invariant_audit.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2027; composite counters + partial recover helpers
- `tests/compiler/test_composite_typed_mutate.cpp` (—) [domain_suite, theme_compiler] — Issue #1408: Inline no-op stubs for aura::jit::AuraJIT::invalidate_prefix
- `tests/compiler/test_constraint_solver_surface_cross_delta.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2028; solve_delta_occurrence +
- `tests/compiler/test_constraint_system_solve_delta_cross_delta_task2.cpp` (—) [domain_suite, theme_compiler] — test_constraint_system_solve_delta_cross_delta_task2.cpp
- `tests/core/test_coverage_holes_workspace_lock.cpp` (—) [domain_suite, theme_core] — Issue #1816 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_dead_coercion_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_dead_coercion_batch.cpp
- `tests/core/test_dep_graph_concurrent.cpp` (—) [domain_suite, theme_core] — test_dep_graph_concurrent.cpp — Issue #1376:
- `tests/compiler/test_dirty_cascade_optimize.cpp` (—) [small, domain_suite, theme_compiler] — Issue #2063 — Dirty cascade subtree-skip (summary-dirty early-exit) test.
- `tests/compiler/test_dirty_delta_present.cpp` (—) [domain_suite, theme_compiler] — skip rate >60% under sparse mutations, metrics avg/p99, mutation guard.
- `tests/compiler/test_dirty_propagation_cascade.cpp` (—) [domain_suite, theme_compiler] — AC1: cascade_mark_dirty / propagate_closure BFS marks all dependents
- `tests/compiler/test_dirty_propagation_cost_closed_loop.cpp` (—) [small, domain_suite, theme_compiler] — Issue #398/#399/#408/#415 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_dirty_reason_verification_propagation.cpp` (—) [small, domain_suite, theme_compiler] — Issue #344/#415/#437/#469 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_edsl_core_stability_cow_atomic_query_mutate.cpp` (—) [domain_suite, theme_compiler] — test_edsl_core_stability_cow_atomic_query_mutate.cpp — Issue #655:
- `tests/compiler/test_edsl_query_mutate_commercial_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #552/#619/#634/#635/#636 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_envframe_bridge_invalidate.cpp` (—) [domain_suite, theme_compiler] — Issue #1916 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_envframe_dualpath_stale_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #417/#418/#543/#602 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_followup_smoke.cpp` (—) [small, followup, domain_suite, theme_compiler] — tests/test_followup_smoke.cpp — Smoke test for follow-up ship
- `tests/compiler/test_followups.cpp` (—) [followup, domain_suite, theme_compiler] — (mutation-log:diff / dirty:summary /
- `tests/compiler/test_full_strategy_partial_recovery.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2029; partial_recovery_* counters + boundary path
- `tests/core/test_guard_dtor_batch_metrics.cpp` (—) [batch_driver, domain_suite, theme_core] — Issue #1747 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_hotpath_matrix_batch.cpp` (—) [large, batch_driver, domain_suite, theme_core] — test_hotpath_matrix_batch.cpp — Domain suite batch: behavioral gates.
- `tests/compiler/test_incremental_typed_selfmod_dirty_narrowing.cpp` (—) [domain_suite, theme_compiler] — test_incremental_typed_selfmod_dirty_narrowing.cpp — Merged #509/#518/#526/#536/#537/#550 +
- `tests/compiler/test_invalidate_cascade_order.cpp` (—) [domain_suite, theme_compiler] — test_invalidate_cascade_order.cpp — Issue #1378:
- `tests/compiler/test_issues_819_829_batch.cpp` (#819) [batch_driver, domain_suite, theme_compiler] — test_issues_819_829_batch.cpp — Phase 1 close for Issues #819–#829.
- `tests/compiler/test_linear_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_linear_batch.cpp
- `tests/compiler/test_linear_ownership_postmutate_guard_steal_envframe.cpp` (—) [domain_suite, theme_compiler] — test_linear_ownership_postmutate_guard_steal_envframe.cpp — Issue #800:
- `tests/compiler/test_linear_provenance_steal_gc_closed_loop.cpp` (—) [domain_suite, theme_compiler] — consistency closed-loop (shared validate_linear_provenance).
- `tests/core/test_lock_hierarchy.cpp` (—) [domain_suite, theme_core] — the lock-hierarchy contract documented in Issue #1388.
- `tests/core/test_marker_metadata_lock.cpp` (—) [domain_suite, theme_core] — Issue #1783 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_module_boundary.cpp` (—) [domain_suite, theme_core] — Issue #1885 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_mutate_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_mutate_batch.cpp
- `tests/compiler/test_mutate_cross_thread_migration.cpp` (—) [domain_suite, theme_compiler] — test_mutate_cross_thread_migration.cpp — Issue #1373:
- `tests/compiler/test_mutation_audit_wal.cpp` (—) [domain_suite, theme_compiler] — append/rotate, full effect/tenant/epoch fields, replay into ring,
- `tests/compiler/test_mutation_boundary_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_mutation_boundary_batch.cpp
- `tests/serve/test_mutation_boundary_guard.cpp` (—) [domain_suite, theme_serve] — Issue #1747/#1897/#1931/#1950 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_mutation_guard_try_acquire.cpp` (—) [domain_suite, theme_serve] — Issue #1547/#1556/#1590/#1628 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_mutation_guard_unit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_mutation_guard_unit_batch.cpp — consolidated mutation-theme drivers
- `tests/compiler/test_mutation_log_query_race.cpp` (—) [domain_suite, theme_compiler] — test_mutation_log_query_race.cpp — Issue #1389:
- `tests/compiler/test_mutation_occurrence_dirty_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_mutation_occurrence_dirty_batch.cpp — consolidated mutation-theme drivers
- `tests/compiler/test_mutation_provenance.cpp` (—) [domain_suite, theme_compiler] — tests/test_mutation_provenance.cpp — Issue #1412: Compound
- `tests/compiler/test_mutation_rollback_coverage.cpp` (—) [domain_suite, theme_compiler] — Issue #213/#266/#369/#400/#553 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_mutation_systemic_guard.cpp` (—) [domain_suite, theme_compiler] — Issue #1818/#1897 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_mutation_typed_audit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_mutation_typed_audit_batch.cpp — consolidated mutation-theme drivers
- `tests/compiler/test_mutator_dispatch_stats_lock.cpp` (—) [domain_suite, theme_compiler] — Issue #1849 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_occurrence_dirty_blame_post_mutate.cpp` (—) [domain_suite, theme_compiler] — test_occurrence_dirty_blame_post_mutate.cpp — restored standalone (AC drift under batch co-link)
- `tests/compiler/test_occurrence_mutate_narrowing.cpp` (—) [domain_suite, theme_compiler] — test_occurrence_mutate_narrowing.cpp — Issue #518 P0 Phase 1:
- `tests/compiler/test_occurrence_provenance_chain_completeness.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2024; fill_coercion_provenance_chain + sentinel
- `tests/compiler/test_occurrence_typing_blame_post_mutate_recovery.cpp` (—) [domain_suite, theme_compiler] — test_occurrence_typing_blame_post_mutate_recovery.cpp — restored standalone (AC drift under batch
- `tests/compiler/test_occurrence_typing_blame_post_mutate_task2.cpp` (—) [domain_suite, theme_compiler] — test_occurrence_typing_blame_post_mutate_task2.cpp — restored standalone (AC drift under batch
- `tests/compiler/test_per_symbol_dirty_cycle_guard.cpp` (—) [domain_suite, theme_compiler] — Issue #1786 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_per_symbol_dirty_pool_lock.cpp` (—) [domain_suite, theme_core] — Issue #1785 (#1978 renamed): issue# moved from filename to header.
- `tests/renderer/test_pixel_framebuffer.cpp` (—) [domain_suite, theme_renderer] — test_pixel_framebuffer.cpp — Issue #1980 / Epic #1979
- `tests/compiler/test_production_readiness_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_production_readiness_batch.cpp
- `tests/compiler/test_provenance_blame_hygiene.cpp` (—) [domain_suite, theme_compiler] — Issue #1877 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_query_mutate_consistency.cpp` (—) [domain_suite, theme_compiler] — test_query_mutate_consistency.cpp — Issue #1374:
- `tests/renderer/test_render3d_primitives.cpp` (—) [domain_suite, theme_renderer] — test_render3d_primitives.cpp — Issue #1986 / Epic #1979
- `tests/renderer/test_render_ai_native_template.cpp` (—) [domain_suite, theme_renderer] — Issue #1677 (#1978 renamed): issue# moved from filename to header.
- `tests/renderer/test_render_mutation_checkpoint.cpp` (—) [domain_suite, theme_renderer] — test_render_mutation_checkpoint.cpp — Issue #1355: lightweight mutation in render hot path
- `tests/compiler/test_shape_jit_pass_deopt_incremental_closedloop_ai_mutate.cpp` (—) [domain_suite, theme_compiler] — test_shape_jit_pass_deopt_incremental_closedloop_ai_mutate.cpp — Issue #744:
- `tests/compiler/test_stable_ref_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_stable_ref_batch.cpp
- `tests/compiler/test_stable_ref_cow_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — Issue #1912 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_stable_ref_provenance_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_stable_ref_provenance_batch.cpp — test_stable_ref 3-merge (R19 phase 20).
- `tests/serve/test_stable_ref_provenance_fiber_cow.cpp` (—) [domain_suite, theme_serve] — test_stable_ref_provenance_fiber_cow.cpp — Merged #457/#497/#527/#540/#549 + #551/#552 (#1978).
- `tests/core/test_stale_ref_string_heap.cpp` (—) [domain_suite, theme_core] — Issue #1681 (#1978 renamed): issue# moved from filename to header.
- `tests/renderer/test_terminal_render_production.cpp` (—) [domain_suite, theme_renderer] — Issue #1673 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_typesystem_solve_delta_occurrence_priority_heavy_mutate.cpp` (—) [domain_suite, theme_compiler] — test_typesystem_solve_delta_occurrence_priority_heavy_mutate.cpp — Issue #745:
- `tests/compiler/test_typesystem_type_propagation_jit_l2_typed_mutate.cpp` (—) [domain_suite, theme_compiler] — test_typesystem_type_propagation_jit_l2_typed_mutate.cpp — Issue #746:
- `tests/compiler/test_typesystem_typed_mutate_incremental_gaps.cpp` (—) [domain_suite, theme_compiler] — test_typesystem_typed_mutate_incremental_gaps.cpp — Issue #659:
- `tests/compiler/test_unify_invalidate_try_acquire.cpp` (—) [domain_suite, theme_compiler] — Issue #1476/#1547/#1628/#1634 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_value_encoding_v2_dispatch_contracts.cpp` (—) [domain_suite, theme_compiler] — Issue #1622/#571/#723 (#1978 renamed): issue# moved from filename to header.
- `tests/renderer/test_voxel_frame.cpp` (—) [domain_suite, theme_renderer] — test_voxel_frame.cpp — Issue #1985 / Epic #1979
- `tests/compiler/test_walk_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_walk_batch.cpp
- `tests/core/test_workspace_lock_reentrancy.cpp` (—) [domain_suite, theme_core] — test_wave1_workspace_lock_reentrancy.cpp — Wave1 B-03 / B-09
- `tests/core/test_workspace_state_lock.cpp` (—) [domain_suite, theme_core] — tests/core/test_workspace_state_lock.cpp — Issue #1994 (F-004):` (workspace-state)` and

### `fiber_orch` — Fiber / orchestration / steal / Guard (36)

**Target:** tests/core/test_fiber_resume_batch (domain/ pilot abandoned in R1)

**Priority:** P1 — domain suite already collapses many obs gates

#### domain/ (36)

- `tests/compiler/test_aot_bridge_checkpoint_version_steal.cpp` (—) [domain_suite, theme_compiler] — test_aot_bridge_checkpoint_version_steal.cpp — Issue #653:
- `tests/compiler/test_compile_primitive_guard.cpp` (—) [domain_suite, theme_compiler] — Issue #1896 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_concurrent.cpp` (—) [large, domain_suite, theme_serve] — test_concurrent.cpp — Concurrency model unit tests
- `tests/compiler/test_edsl_concurrent_fiber_boundary_task1.cpp` (—) [domain_suite, theme_compiler] — test_edsl_concurrent_fiber_boundary_task1.cpp —
- `tests/compiler/test_edsl_concurrent_query_mutate.cpp` (—) [domain_suite, theme_compiler] — test_edsl_concurrent_query_mutate.cpp — Issue #332
- `tests/compiler/test_env_lookup_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_env_lookup_batch.cpp — batch driver for Env::lookup family.
- `tests/serve/test_fiber_concurrent_unit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — test_fiber_concurrent_unit_batch.cpp — light concurrent units
- `tests/serve/test_fiber_integration_batch.cpp` (—) [batch_driver, domain_suite, theme_serve] — tests/serve/test_fiber_integration_batch.cpp — closure-bridge Cycle-4 integration (Issue #226).
- `tests/serve/test_fiber_mutation_steal_safety.cpp` (—) [domain_suite, theme_serve] — test_fiber_mutation_steal_safety.cpp — Issue #542:
- `tests/serve/test_fiber_orch_core_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — test_fiber_orch_core_batch.cpp — consolidated fiber-theme drivers
- `tests/serve/test_fiber_orch_parallel_quota_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — test_fiber_orch_parallel_quota_batch.cpp — consolidated fiber-theme drivers
- `tests/compiler/test_fiber_resume_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_fiber_resume_batch.cpp — batch driver for fiber resume post-steal family.
- `tests/serve/test_fiber_steal_panic_checkpoint_nested_gc.cpp` (—) [small, domain_suite, theme_serve] — tests/test_fiber_steal_panic_checkpoint_nested_gc.cpp — Issue #1446
- `tests/serve/test_fiber_strategy_evolve_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — test_fiber_strategy_evolve_batch.cpp — consolidated fiber-theme drivers
- `tests/serve/test_fiber_synthesize_batch.cpp` (—) [batch_driver, domain_suite, theme_serve] — test_fiber_synthesize_batch.cpp — consolidated fiber-theme drivers
- `tests/serve/test_guard_panic_reflect_fiber_resume_task6.cpp` (—) [domain_suite, theme_serve] — test_guard_panic_reflect_fiber_resume_task6.cpp — Issue #596:
- `tests/serve/test_inner_steal_starvation.cpp` (—) [domain_suite, theme_serve] — Issue #1445/#1492/#1633 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_issue_1992.cpp` (#1992) [domain_suite, theme_serve] — test_issue_1992.cpp — Issue #1992 (C-001): Fiber::mutation_stack_storage_
- `tests/compiler/test_issues_809_817_batch.cpp` (#809) [batch_driver, domain_suite, theme_compiler] — test_issues_809_817_batch.cpp — Phase 1 close for Issues #809–#817.
- `tests/compiler/test_lock_order_closures_env.cpp` (—) [domain_suite, theme_compiler] — Issue #1664 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_orchestration_steal_boost.cpp` (—) [small, domain_suite, theme_serve] — tests/test_orchestration_steal_boost.cpp — Issue #1445 / #1492
- `tests/serve/test_panic_checkpoint_fiber_resume_safety.cpp` (—) [domain_suite, theme_serve] — test_panic_checkpoint_fiber_resume_safety.cpp — Issue #592:
- `tests/compiler/test_per_defuse_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_per_defuse_batch.cpp — batch driver for per_defuse_index family.
- `tests/serve/test_per_fiber_stack_pool_high_concurrency.cpp` (—) [domain_suite, theme_serve] — test_per_fiber_stack_pool_high_concurrency.cpp — Issue #652:
- `tests/serve/test_prompt6_epoch_atomic_visibility_fiber_steal.cpp` (—) [domain_suite, theme_serve] — test_prompt6_epoch_atomic_visibility_fiber_steal.cpp — Issue #739:
- `tests/compiler/test_prompt6_full_memory_safety_fuzz_stress.cpp` (—) [domain_suite, theme_compiler] — test_prompt6_full_memory_safety_fuzz_stress.cpp — Issue #602:
- `tests/compiler/test_propagate_marker_cycle_guard.cpp` (—) [domain_suite, theme_compiler] — Issue #1679/#1682/#1782 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_runtime_concurrent_full_cycle_chaos.cpp` (—) [domain_suite, theme_compiler] — test_runtime_concurrent_full_cycle_chaos.cpp — Issue #755:
- `tests/serve/test_runtime_mutation_boundary_steal_safety.cpp` (—) [domain_suite, theme_serve] — test_runtime_mutation_boundary_steal_safety.cpp — Issue #588:
- `tests/serve/test_safe_yield_orchestration.cpp` (—) [domain_suite, theme_serve] — Issue #1504/#1591/#1635 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_safepoint_mutation.cpp` (—) [domain_suite, theme_serve] — test_safepoint_mutation.cpp — Issue #1364: safepoint × mutation telemetry
- `tests/serve/test_scheduler_gc_defer_pending_panic_steal.cpp` (—) [domain_suite, theme_serve] — AC1: pending checkpoint → GCCollector::request deferred; collect skips
- `tests/serve/test_scheduler_gc_safepoint_mutation_coordination.cpp` (—) [domain_suite, theme_serve] — test_scheduler_gc_safepoint_mutation_coordination.cpp —
- `tests/serve/test_scheduler_llm_bottleneck_adaptive_steal_gc.cpp` (—) [domain_suite, theme_serve] — test_scheduler_llm_bottleneck_adaptive_steal_gc.cpp — Issue #754:
- `tests/core/test_stress_alloc_storage_lock.cpp` (—) [domain_suite, theme_core] — test_stress_alloc_storage_lock.cpp — Issue #1397
- `tests/compiler/test_workspace_swap_guard.cpp` (—) [domain_suite, theme_compiler] — tests/compiler/test_workspace_swap_guard.cpp — Issue #1717: synthesize:optimize swap-guard test.

### `linear_ownership` — Linear ownership / borrow / consume (7)

**Target:** tests/compiler/test_linear_ownership_batch.cpp (R1 src/-aligned)

**Priority:** P1 — small, already partially batched

#### domain/ (7)

- `tests/compiler/test_compiler_service_ownership.cpp` (—) [small, domain_suite, theme_compiler] — Issue #1835/#1837/#1839 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_hardware_resource_linear_ownership.cpp` (—) [domain_suite, theme_compiler] — test_hardware_resource_linear_ownership.cpp — Issue #306:
- `tests/compiler/test_linear_ownership_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_linear_ownership_batch.cpp
- `tests/compiler/test_linear_ownership_occurrence_predicate_mutate.cpp` (—) [domain_suite, theme_compiler] — test_linear_ownership_occurrence_predicate_mutate.cpp — Issue #747:
- `tests/compiler/test_linear_runtime_violation.cpp` (—) [small, domain_suite, theme_compiler] — Issue #2067 — Linear Ownership runtime enforcement test.
- `tests/compiler/test_render_dispatch_linear_epoch.cpp` (—) [domain_suite, theme_compiler] — Issue #1676 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_type_registry_ownership.cpp` (—) [small, domain_suite, theme_core] — Issue #1835/#1837 (#1978 renamed): issue# moved from filename to header.

### `edsl_hygiene` — EDSL / macro hygiene / reflect (21)

**Target:** tests/core/test_macro_reflect_batch (domain/ pilot abandoned in R1)

**Priority:** P1 — domain hygiene suite exists

#### domain/ (21)

- `tests/compiler/test_contracts.cpp` (—) [small, domain_suite, theme_compiler] — tests/compiler/test_contracts.cpp — Issue #83: C++26 contract_assert + trailing pre/post
- `tests/reflect/test_error_merr.cpp` (—) [small, domain_suite, theme_reflect] — test_error_merr.cpp — Pilot for centralized make_merr (refactor Step 0.1+)
- `tests/reflect/test_ir_cache_v2.cpp` (—) [small, domain_suite, theme_reflect] — tests/test_ir_cache_v2.cpp
- `tests/reflect/test_issue_178.cpp` (#178) [small, early_issue, domain_suite, theme_reflect] — test_issue_178.cpp — Issue #178 / #268: production NodeView
- `tests/reflect/test_issue_178_reflect.cpp` (#178) [early_issue, domain_suite, theme_reflect] — Non-module TU: P2996 reflection (Issue #268).
- `tests/compiler/test_macro_hygiene_closedloop_health.cpp` (—) [domain_suite, theme_compiler] — Issue #1501/#1589/#1593/#1609/#1613 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_macro_hygiene_depth_concurrent_obs.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2021; peak / in-flight atomics + snapshot helper
- `tests/compiler/test_macro_hygiene_fiber_panic_aot_soa_self_evo.cpp` (—) [domain_suite, theme_compiler] — test_macro_hygiene_fiber_panic_aot_soa_self_evo.cpp — Issue #654:
- `tests/compiler/test_macro_reflect_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_macro_reflect_batch.cpp — batch driver for macro+reflect+self-evo family.
- `tests/compiler/test_macro_restamp_after_flat.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2019 + restamp_macro_introduced_generations
- `tests/compiler/test_macro_self_evo_capability.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2023; MacroSelfEvoPolicy + check_macro_self_evo
- `tests/compiler/test_prompt2_6_impact_scope_quote_lambda_bridge_env.cpp` (—) [domain_suite, theme_compiler] — test_prompt2_6_impact_scope_quote_lambda_bridge_env.cpp — Issue #741:
- `tests/compiler/test_query_pattern_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_query_pattern_batch.cpp — query_pattern pair dup-merge (R19 phase 13).
- `tests/compiler/test_query_pattern_hygiene_macrointroduced.cpp` (—) [domain_suite, theme_compiler] — test_query_pattern_hygiene_macrointroduced.cpp — Issue #593:
- `tests/compiler/test_reflect_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_reflect_batch.cpp
- `tests/reflect/test_reflect_hygiene_agent_diagnostics.cpp` (—) [domain_suite, theme_reflect] — reflect:provenance-blame for expand → diagnose → mutate closed loops.
- `tests/reflect/test_reflect_hygiene_unit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_reflect] — test_edsl_hygiene_unit_batch.cpp — consolidated edsl hygiene drivers
- `tests/reflect/test_reflect_macro_hygiene_batch.cpp` (—) [large, batch_driver, domain_suite, theme_reflect] — test_edsl_macro_hygiene_batch.cpp — consolidated edsl hygiene drivers
- `tests/reflect/test_reflect_pattern_hygiene_batch.cpp` (—) [large, batch_driver, domain_suite, theme_reflect] — test_edsl_pattern_hygiene_batch.cpp — consolidated edsl hygiene drivers
- `tests/compiler/test_rest_param_hygiene_self_evo.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2018; rest pre-scan + dotted preserve + metric
- `tests/compiler/test_static_reflect_selfmod_validation_task6.cpp` (—) [domain_suite, theme_compiler] — Issue #454/#551/#587/#594 (#1978 renamed): issue# moved from filename to header.

### `jit_incremental` — JIT / AOT / incremental relower (32)

**Target:** domain suite for incremental_*; keep heavy JIT in issue bundles

**Priority:** P2 — link-profile heavy; migrate AC smoke first

#### domain/ (32)

- `tests/compiler/test_aot_incremental_reemit.cpp` (—) [large, domain_suite, theme_compiler] — Issue #1480/#1930/#1943/#1952/#2013 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_aot_mangle_top.cpp` (—) [domain_suite, theme_compiler] — test_aot_mangle_top.cpp — Issue #1369 / #2015:
- `tests/compiler/test_aot_region_per_eval.cpp` (—) [domain_suite, theme_compiler] — test_aot_region_per_eval.cpp — Issue #1367 (standalone; ACs drift under current aot: API)
- `tests/compiler/test_aot_reload_primitive.cpp` (—) [domain_suite, theme_compiler] — test_aot_reload_primitive.cpp — Issue #1366: (aot:reload) Aura wrappers
- `tests/compiler/test_aot_shell_c0_escape.cpp` (—) [domain_suite, theme_compiler] — test_issue_1997.cpp -- runtime smoke test for B-002 / #1997
- `tests/compiler/test_build_kv_hash_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_build_kv_hash_batch.cpp — build_kv_hash pair dup-merge (R19 phase 8).
- `tests/compiler/test_capability_gating.cpp` (—) [domain_suite, theme_compiler] — Issue #1416: Inline no-op stubs for aura::jit::AuraJIT::invalidate_prefix
- `tests/compiler/test_compiler_core_incremental_selfmod_gaps.cpp` (—) [domain_suite, theme_compiler] — test_compiler_core_incremental_selfmod_gaps.cpp — Issue #657:
- `tests/compiler/test_concept_constraints.cpp` (—) [domain_suite, theme_compiler] — AC1: module exports all Pass-related concepts
- `tests/compiler/test_dead_coercion_pipeline_wire.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2025; PassKind::DeadCoercion + DeadCoercionPass +
- `tests/compiler/test_incremental_effectiveness_snapshot_fail.cpp` (—) [domain_suite, theme_compiler] — Issue #1669/#1854/#1856 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_incremental_perblock_closure_bridge_safety.cpp` (—) [domain_suite, theme_compiler] — test_incremental_perblock_closure_bridge_safety.cpp — Issue #600:
- `tests/compiler/test_incremental_relower_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_incremental_relower_batch.cpp — batch driver for incremental_relower family.
- `tests/compiler/test_incremental_type_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_incremental_type_batch.cpp — batch driver for incremental_type family.
- `tests/compiler/test_inline_pass_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_inline_pass_batch.cpp — inline_pass pair dup-merge (R19 phase 14).
- `tests/compiler/test_jit_aot_hot_update_unit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_jit_aot_hot_update_batch.cpp — consolidated AOT hot-update + steal-boundary drivers
- `tests/compiler/test_jit_batch_deopt_clear.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_issue_1996.cpp — Issue #1996 (B-003): `g_batch_deopt_jit` raw
- `tests/compiler/test_jit_closure_cache_race.cpp` (—) [domain_suite, theme_compiler] — Issue #1707 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_jit_concurrent_compile.cpp` (—) [domain_suite, theme_compiler] — test_jit_concurrent_compile.cpp — Issue #114 concurrent compile stress
- `tests/compiler/test_jit_consistency.cpp` (—) [domain_suite, theme_compiler] — test_jit_consistency.cpp — Issue #427: JIT ↔ IRInterpreter
- `tests/compiler/test_jit_critical_coverage.cpp` (—) [domain_suite, theme_compiler] — Issue #1658/#1917 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_jit_full_opcode_coverage.cpp` (—) [domain_suite, theme_compiler] — Issue #1289/#1512/#1658/#427/#532 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_jit_macro_introduced_preserve.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2022; side-table + FunctionMeta + FlatFunction fields
- `tests/compiler/test_jit_metrics.cpp` (—) [domain_suite, theme_compiler] — test_jit_metrics.cpp — Issue #114 JIT observability + per-function cache tests
- `tests/compiler/test_jit_metrics_stub.cpp` (—) [small, domain_suite, theme_compiler] — test_jit_metrics_stub.cpp — Stub for the JIT test.
- `tests/compiler/test_optimization_passes_contracts.cpp` (—) [domain_suite, theme_compiler] — AC1: 4 core passes satisfy Pass / DirtyAware / PureAnalysis where applicable
- `tests/core/test_pair_slot_lock.cpp` (—) [domain_suite, theme_core] — test_pair_slot_lock.cpp -- runtime smoke test for B-024 / #1998
- `tests/core/test_prim_call_count_clamp.cpp` (—) [small, domain_suite, theme_core] — Issue #1711 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_relower_strategy_cache_lock.cpp` (—) [domain_suite, theme_compiler] — Issue #1839/#1855 (#1978 renamed): issue# moved from filename to header.
- `tests/renderer/test_render_pass_incremental.cpp` (—) [domain_suite, theme_renderer] — AC1: RenderPass satisfies DirtyAware + SoAView + JITFriendly + Incremental
- `tests/stdlib/test_spec_runtime.cpp` (—) [domain_suite, theme_stdlib] — test_spec_runtime.cpp — Runtime tests for L2 specialization (Phase 3, #53)
- `tests/compiler/test_typechecker_incremental_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_typechecker_incremental_batch.cpp — typechecker_incremental pair dup-merge

### `shape_soa` — Shape / SoA / column layout (17)

**Target:** tests/core/test_soa_batch.cpp (no move needed)

**Priority:** P2 — small-medium; soa_batch precedent

#### domain/ (17)

- `tests/compiler/test_apply_closure_envframe_soa.cpp` (—) [domain_suite, theme_compiler] — Issue #1365/#1475/#1511/#1626/#1632/#1660 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_cpp26_contracts_hotpath_arena_soa_value_shape_pass.cpp` (—) [domain_suite, theme_compiler] — test_cpp26_contracts_hotpath_arena_soa_value_shape_pass.cpp — Issue #742:
- `tests/compiler/test_highperf_cpp26_gaps_arena_soa_value_shape_pass.cpp` (—) [domain_suite, theme_compiler] — test_highperf_cpp26_gaps_arena_soa_value_shape_pass.cpp — Issue #658:
- `tests/compiler/test_ir_soa_dual_emit_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_ir_soa_dual_emit_batch.cpp — IR SoA dual-emit family dup-merge (R19 phase
- `tests/compiler/test_ir_soa_incremental_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #254/#403/#404/#506 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_list_vector_soa_hotpath_ai_loops.cpp` (—) [domain_suite, theme_compiler] — test_list_vector_soa_hotpath_ai_loops.cpp — Issue #752:
- `tests/compiler/test_matcher_stable_captures.cpp` (—) [domain_suite, theme_compiler] — Issue #1695 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_set_workspace_flat.cpp` (—) [domain_suite, theme_core] — Issue #1729 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_shape.cpp` (—) [large, domain_suite, theme_compiler] — test_shape.cpp — Unit tests for shape infrastructure (Phase 1, #53)
- `tests/compiler/test_shape_profiler_burst_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #406/#407/#570/#605 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_shape_profiler_stability_deopt_fiber_task4.cpp` (—) [domain_suite, theme_compiler] — test_shape_profiler_stability_deopt_fiber_task4.cpp — Issue #570:
- `tests/compiler/test_shape_soa_unit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_shape_soa_unit_batch.cpp — Wave 36+ (#1957) shape_soa theme
- `tests/compiler/test_shapeprofiler_stability_deopt_jit_mutate.cpp` (—) [domain_suite, theme_compiler] — test_shapeprofiler_stability_deopt_jit_mutate.cpp — Issue #605:
- `tests/core/test_soa_batch.cpp` (—) [large, batch_driver, domain_suite, theme_core] — test_soa_batch.cpp
- `tests/compiler/test_soa_view_enforcement.cpp` (—) [domain_suite, theme_compiler] — Issue #1241/#1517/#1619/#1918 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_spec_jit.cpp` (—) [large, domain_suite, theme_compiler] — test_spec_jit.cpp — Unit tests for L1 type specialization (Phase 2, #53)
- `tests/compiler/test_workspace_delete_child.cpp` (—) [domain_suite, theme_compiler] — tests/compiler/test_workspace_delete_child.cpp — Issue #1770: WorkspaceTree delete_child test.

### `observability` — Observability / metrics / query:*-stats (86)

**Target:** tests/compiler/test_obs_schema_matrix.cpp + tests/compiler/obs_schema_cases.hpp

**Priority:** P2 — often thin schema probes; collapse into obs matrix

#### domain/ (86)

- `tests/renderer/test_ai_closedloop_readiness.cpp` (—) [domain_suite, theme_renderer] — Issue #1591/#1592/#1593 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_aot_stats_null_metrics.cpp` (—) [small, domain_suite, theme_compiler] — Issue #1835/#1843 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_ast_ops_stats_workspace_lock.cpp` (—) [domain_suite, theme_core] — Issue #1729/#1851/#1852 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_auto_evolve_closure_live.cpp` (—) [domain_suite, theme_compiler] — Issue #1713 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_bidirectional_stats.cpp` (—) [domain_suite, theme_core] — tests/test_bidirectional_stats.cpp — Issue #1420 AC3:
- `tests/compiler/test_blame_stamp_on_degrade.cpp` (—) [small, domain_suite, theme_compiler] — Issue #2064 — blame / provenance stamping on Dynamic degrade + CoercionMap
- `tests/compiler/test_blame_tracking_typed_mutate.cpp` (—) [domain_suite, theme_compiler] — Issue #1617/#1924 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_bugfix_968.cpp` (#968) [small, domain_suite, theme_compiler] — Issue #957/#968/#982/#984 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_closedloop_stats_hash_cap.cpp` (—) [small, domain_suite, theme_compiler] — Issue #1795 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_closure_view_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_closure_view_batch.cpp — closure_view pair dup-merge (R19 phase 17).
- `tests/core/test_compiler_metrics_ownership.cpp` (—) [small, domain_suite, theme_core] — Issue #1835 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_constraintsystem_solve_delta_clean_conflict_detection.cpp` (—) [domain_suite, theme_compiler] — test_constraintsystem_solve_delta_clean_conflict_detection.cpp
- `tests/core/test_cpp26_contracts_hotpath.cpp` (—) [domain_suite, theme_core] — Issue #1321/#1519/#1620/#742 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_dead_coercion_elim.cpp` (—) [small, domain_suite, theme_compiler] — Issue #2066 — DeadCoercionElimination IR-layer CastOp elision test.
- `tests/compiler/test_dead_coercion_elision_narrow_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_dead_coercion_elision_narrow_batch.cpp — dead_coercion_elision_narrow pair
- `tests/compiler/test_defuse_version_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #189/#417/#419/#456 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_engine_metrics_facade.cpp` (—) [domain_suite, theme_compiler] — AC1: (engine:metrics) returns hash with nested groups + ≥200 metric fields
- `tests/compiler/test_envframe_resolve_distinction.cpp` (—) [domain_suite, theme_compiler] — Issue #1708/#1709/#1754/#1756/#1890 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_eval_relower_hotpath.cpp` (—) [domain_suite, theme_compiler] — Issue #1506/#1601/#1605/#1623 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_fiber_macro_hygiene_refresh.cpp` (—) [domain_suite, theme_compiler] — Issue #1490/#1592/#1608/#1612 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_fine_dirty_relower.cpp` (—) [domain_suite, theme_compiler] — test_fine_dirty_relower.cpp — Issue #1657 (standalone; bump metrics ACs drift)
- `tests/compiler/test_inline_typecheck_exception.cpp` (—) [domain_suite, theme_compiler] — Issue #1769 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_invalidate_consistency.cpp` (—) [domain_suite, theme_compiler] — Issue #1496/#1607/#1627 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_invalidations_stats_workspace_lock.cpp` (—) [domain_suite, theme_compiler] — Issue #1729/#1851 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_ir_metadata_interpreter_jit_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #403/#506/#570/#598 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_let_poly_solve_delta.cpp` (—) [domain_suite, theme_compiler] — Issue #1617/#745/#798 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_linear_boundary_consistency.cpp` (—) [domain_suite, theme_compiler] — Issue #1568 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_linear_live_closure_walk.cpp` (—) [domain_suite, theme_compiler] — Issue #1557/#1568/#1596/#1659/#1895 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_linear_walk_active_closures.cpp` (—) [domain_suite, theme_compiler] — Issue #1895/#1928 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_longrunning_infra_primitives.cpp` (—) [domain_suite, theme_compiler] — test_longrunning_infra_primitives.cpp — Issue #753:
- `tests/compiler/test_longrunning_recovery_latency.cpp` (—) [domain_suite, theme_compiler] — AC1: panic-restore path instruments recovery latency
- `tests/compiler/test_lookup_stats_impl_heterogeneous.cpp` (—) [small, domain_suite, theme_compiler] — Issue #1671 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_module_export_cache.cpp` (—) [domain_suite, theme_compiler] — Issue #1680 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_mutation_aot_unit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_mutation_aot_unit_batch.cpp — consolidated mutation-theme drivers
- `tests/serve/test_mutation_hold_time.cpp` (—) [domain_suite, theme_serve] — test_mutation_hold_time.cpp — Issue #1375:
- `tests/compiler/test_obs_metrics_smoke_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_obs_metrics_smoke_batch.cpp — consolidated observability schema smokes
- `tests/compiler/test_obs_schema_matrix.cpp` (—) [domain_suite, theme_compiler] — test_obs_schema_matrix.cpp — Domain suite: observability + production schemas
- `tests/compiler/test_observability_tier_table.cpp` (—) [obs_named, domain_suite, theme_compiler] — Issue #1670 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_occ_cache_stats_wired.cpp` (—) [domain_suite, theme_compiler] — Issue #1781 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_open_issues_meta_batch.cpp` (—) [batch_driver, domain_suite, theme_core] — Issue #514/#515/#516/#517/#520 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_pass_contracts_hotpath_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #381/#406/#506/#571 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_pattern_structural_index_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #211/#421/#423/#547/#554 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_post_steal_closed_loop.cpp` (—) [domain_suite, theme_serve] — Issue #1592 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_primitive_meta_self_describing_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #478/#480/#560/#583 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_primitive_resource_quota_stats.cpp` (—) [domain_suite, theme_core] — AC1: primitive returns hash with 5 integer fields (incl. schema)
- `tests/compiler/test_primitives_capture_contract.cpp` (—) [domain_suite, theme_compiler] — test_primitives_capture_contract.cpp — Issue #751:
- `tests/compiler/test_primitives_hotpath_registry_slo.cpp` (—) [domain_suite, theme_compiler] — test_primitives_hotpath_registry_slo.cpp — Issue #805:
- `tests/compiler/test_primitives_registry_core_consistency.cpp` (—) [domain_suite, theme_compiler] — Issue #478/#560/#583 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_primitives_surface_convergence.cpp` (—) [domain_suite, theme_compiler] — test_primitives_surface_convergence.cpp — Issue #1448 SlimSurface
- `tests/compiler/test_production_hardening_985.cpp` (#985) [small, domain_suite, theme_compiler] — test_production_hardening_985_1013.cpp — Issues #985–#1013 Phase 1
- `tests/compiler/test_production_roadmap_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #441/#514/#520/#634/#635 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_production_safety.cpp` (—) [domain_suite, theme_compiler] — test_production_safety.cpp — Merged #1047-#1071 + #1097-#1122 (#1978).
- `tests/compiler/test_production_safety_1047.cpp` (#1047) [small, domain_suite, theme_compiler] — Issue #1047/#1050/#1054/#1071 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_production_safety_1097.cpp` (#1097) [small, domain_suite, theme_compiler] — Issue #1097/#1104/#1122 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_production_stability_1014.cpp` (#1014) [domain_suite, theme_compiler] — Issue #1014/#1015/#1020/#1036/#1039/#1046 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_production_sweep.cpp` (—) [large, domain_suite, theme_compiler] — test_production_sweep.cpp — Merged #1123-#1343 (#1978).
- `tests/serve/test_production_sweep.cpp` (—) [small, domain_suite, theme_serve] — test_production_sweep.cpp — fiber production sweep (standalone; SIGSEGV in batch)
- `tests/compiler/test_query_dispatch.cpp` (—) [small, domain_suite, theme_compiler] — Issue #1435 (query :op) unified dispatcher
- `tests/core/test_raw_pointer_safety.cpp` (—) [domain_suite, theme_core] — Issue #1898 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_refinement_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #432/#467/#495/#509/#574 (#1978 renamed): issue# moved from filename to header.
- `tests/renderer/test_render_ffi_hotpath.cpp` (—) [domain_suite, theme_renderer] — c-render-bind / c-render-draw / c-present-batch / c-ansi-emit, micro-benchmark.
- `tests/renderer/test_render_hotpath_observability.cpp` (—) [obs_named, domain_suite, theme_renderer] — Issue #1674/#1676 (#1978 renamed): issue# moved from filename to header.
- `tests/renderer/test_render_hotpath_stability_under_mutation.cpp` (—) [domain_suite, theme_renderer] — high-frequency mutate + present; no deopt storm; AOT hit rate observable.
- `tests/compiler/test_runtime_observability_correlated_stats.cpp` (—) [obs_named, domain_suite, theme_compiler] — test_runtime_observability_correlated_stats_673.cpp — Issue #673:
- `tests/compiler/test_safe_snapshot_umbrella.cpp` (—) [domain_suite, theme_compiler] — Issue #1839/#1856 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_scan_skip_freed_closures.cpp` (—) [domain_suite, theme_compiler] — Issue #1665 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_self_evo_stats.cpp` (—) [domain_suite, theme_compiler] — Issue #1909 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_self_evolution_chaos_stable.cpp` (—) [domain_suite, theme_serve] — test_self_evolution_chaos_stable_674.cpp — Issue #674:
- `tests/compiler/test_self_evolution_loop_stats.cpp` (—) [domain_suite, theme_compiler] — Issue #1883 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_self_heal_policy_engine.cpp` (—) [domain_suite, theme_serve] — test_self_heal_policy_engine.cpp — standalone (flaky/failing ACs under batch link)
- `tests/compiler/test_selfevo_bugfix_941.cpp` (#941) [small, domain_suite, theme_compiler] — test_selfevo_bugfix_941_967.cpp — Issues #941–#967 Phase 1
- `tests/compiler/test_seva_demo_metrics.cpp` (—) [small, domain_suite, theme_compiler] — Issue #1720/#1835/#1840/#1841 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_shape_linear_collaborative_pass.cpp` (—) [domain_suite, theme_compiler] — Issue #1531/#1661/#462/#606 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_stale_closure_fallback.cpp` (—) [domain_suite, theme_compiler] — AC1: apply_closure after mark_define_dirty / epoch bump →
- `tests/compiler/test_stats_catalog_drift.cpp` (—) [domain_suite, theme_compiler] — Issue #1672 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_stats_facade_bench.cpp` (—) [small, domain_suite, theme_compiler] — Issue #1434 stretch: facade vs N direct stats dispatches.
- `tests/compiler/test_stats_module_unification.cpp` (—) [domain_suite, theme_compiler] — test_stats_module_unification.cpp — Issue #560:
- `tests/compiler/test_stdlib_production_review_923.cpp` (#923) [small, domain_suite, theme_compiler] — test_stdlib_production_review_923_940.cpp — Issues #923–#940 Phase 1
- `tests/core/test_tenant_isolation_enforcement.cpp` (—) [domain_suite, theme_core] — capability cross-tenant grant, provenance deny, Strict sandbox link,
- `tests/compiler/test_test_strategy.cpp` (—) [domain_suite, theme_compiler] — Issue #1623/#1624/#1627/#1887 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_tier_dispatch.cpp` (—) [domain_suite, theme_compiler] — test_tier_dispatch.cpp — Issue #1356: HotTierTable for kPrimPerfHot primitives
- `tests/core/test_type_cache_stats_snapshot.cpp` (—) [domain_suite, theme_core] — Issue #1797 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_type_prop_invariant_correlation.cpp` (—) [domain_suite, theme_compiler] — Issue #1884 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_type_propagation_dead_coercion.cpp` (—) [domain_suite, theme_compiler] — test_type_propagation_dead_coercion.cpp — Issue #1874 (#1978 renamed):
- `tests/compiler/test_unified_invalidation.cpp` (—) [domain_suite, theme_compiler] — Issue #1448/#1476/#1496/#1607 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_verify_parse_shared_helper.cpp` (—) [domain_suite, theme_compiler] — Issue #1771 (#1978 renamed): issue# moved from filename to header.

### `uncategorized` — Uncategorized / mixed (27)

**Target:** manual triage before domain placement

**Priority:** P3 — review case-by-case

#### domain/ (27)

- `tests/compiler/test_arithmetic_int64_safety.cpp` (—) [small, domain_suite, theme_compiler] — test_arithmetic_int64_safety.cpp — Issues #1150–#1156 Phase 1
- `tests/compiler/test_ast_workspace_modules.cpp` (—) [domain_suite, theme_compiler] — test_ast_workspace_modules.cpp — Issue #563:
- `tests/stdlib/test_atomic_swap_stdlib.cpp` (—) [domain_suite, theme_stdlib] — test_atomic_swap_stdlib.cpp — Issue #1380:
- `tests/compiler/test_aura_result_error_policy.cpp` (—) [domain_suite, theme_compiler] — test_aura_result_error_policy.cpp — Issues #807 + #808:
- `tests/renderer/test_camera_rays.cpp` (—) [domain_suite, theme_renderer] — test_camera_rays.cpp — Issue #1981 / Epic #1979
- `tests/compiler/test_closure_free.cpp` (—) [domain_suite, theme_compiler] — test_closure_free.cpp — Issue #1361: aura_free_closure + ID reuse
- `tests/compiler/test_compile02_no_dup_imports.cpp` (—) [domain_suite, theme_compiler] — Issue #1857 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_core_builtins_review.cpp` (—) [domain_suite, theme_compiler] — test_core_builtins_review.cpp — Issue #564:
- `tests/core/test_hash_iter_invalidation.cpp` (—) [domain_suite, theme_core] — test_hash_iter_invalidation.cpp - Issue #1398:
- `tests/compiler/test_module_loader_dead_heap_circular.cpp` (—) [domain_suite, theme_compiler] — Issue #1488/#1692 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_module_prefix_dead_heap.cpp` (—) [domain_suite, theme_compiler] — Issue #1488/#1693 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_open_issues_phase1_batch.cpp` (—) [phase_slice, batch_driver, domain_suite, theme_compiler] — test_open_issues_phase1_batch.cpp — legacy alias for the domain suite.
- `tests/core/test_pair_unchecked_safety.cpp` (—) [domain_suite, theme_core] — Issue #1710 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_panic_checkpoint_batch.cpp` (—) [batch_driver, domain_suite, theme_core] — tests/core/test_panic_checkpoint_batch.cpp
- `tests/core/test_persist_basic.cpp` (—) [domain_suite, theme_core] — test_persist_basic.cpp — Issue #1381:
- `tests/compiler/test_query_namespace_audit.cpp` (—) [domain_suite, theme_compiler] — test_query_namespace_audit.cpp — Issue #562:
- `tests/renderer/test_render_telemetry.cpp` (—) [domain_suite, theme_renderer] — test_render_telemetry.cpp — Issue #1357: per-prim latency + frame time histogram
- `tests/stdlib/test_stdlib_infrastructure.cpp` (—) [domain_suite, theme_stdlib] — test_stdlib_infrastructure.cpp — Issue #565:
- `tests/stdlib/test_synthesize_namespace_demotion.cpp` (—) [domain_suite, theme_stdlib] — test_synthesize_namespace_demotion.cpp — Issue #561:
- `tests/renderer/test_terminal_concurrent.cpp` (—) [domain_suite, theme_renderer] — test_terminal_concurrent.cpp — Issue #1352 (standalone; free-corruption when co-linked)
- `tests/renderer/test_terminal_deprecation.cpp` (—) [domain_suite, theme_renderer] — test_terminal_deprecation.cpp — Issue #1351: deprecate 7 no-op terminal:* primitives
- `tests/repl/test_terminal_domain_batch.cpp` (—) [batch_driver, domain_suite, theme_repl] — test_terminal_domain_batch.cpp — terminal domain batch driver.
- `tests/core/test_try_lock_workspace_lock_order.cpp` (—) [domain_suite, theme_core] — Issue #1768 (#1978 renamed): issue# moved from filename to header.
- `tests/renderer/test_voxel_raycast.cpp` (—) [domain_suite, theme_renderer] — test_voxel_raycast.cpp — Issue #1983 / Epic #1979
- `tests/renderer/test_voxel_shade.cpp` (—) [domain_suite, theme_renderer] — test_voxel_shade.cpp — Issue #1984 / Epic #1979
- `tests/renderer/test_voxel_volume.cpp` (—) [domain_suite, theme_renderer] — test_voxel_volume.cpp — Issue #1982 / Epic #1979
- `tests/compiler/test_workspace_dispatch.cpp` (—) [domain_suite, theme_compiler] — tests/compiler/test_workspace_dispatch.cpp — Issue #1437: workspace :op dispatch contract test.

## Regenerating

```bash
python3 scripts/inventory_legacy_tests.py
python3 scripts/inventory_legacy_tests.py --check
```

The coarser Phase-2 5-domain classifier remains available as `scripts/classify_test_issues.py` for historical comparison; **this inventory (#1957) is the planning source of truth** for domain migration.
