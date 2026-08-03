# Legacy test inventory

**Issue:** [#1957](https://github.com/cybrid-systems/aura/issues/1957)
**Generated:** 2026-08-04 by `scripts/inventory_legacy_tests.py`
**Status:** living document — re-run the script after consolidations.

## Purpose

Categorize legacy per-issue regression tests so we can migrate them in batches into the preferred `tests/core/` structure (and existing family batch drivers under `tests/test_*_batch.cpp`).

`tests/issues/` **removed** (#1957). Prefer theme/domain batches; do not reintroduce per-issue mains.

## Scope snapshot

| Location | Count | Notes |
|----------|------:|-------|
| `tests/issues/test_issue_*.cpp` | 0 | Legacy per-issue mains / bundle members |
| `tests/test_*.cpp` (issue-oriented) | 0 | Numbered root tests + `*_batch` drivers |
| `tests/core/test_*.cpp` | 720 | Preferred destination suites |
| **Total scanned** | **720** | |

### Related artifacts

- Coarser 5-bucket Phase-2 map: [`tests_classification.md`](domain_classification.md) (`scripts/classify_test_issues.py`)
- Link/bundle profiles: [`tests/fixtures/issue_link_profiles.json`](fixtures/issue_link_profiles.json)
- Domain CMake: [`cmake/AuraDomainTests.cmake`](../cmake/AuraDomainTests.cmake)
- Test layout rules: [`tests/README.md`](README.md)

## Theme buckets (8 + uncategorized)

Classification uses the **filename + first 50 lines** (keywords and filename token boosts). Ties break toward earlier themes in the priority order below.

| Theme | Title | Issues | Root | Domain | Total | Migration priority |
|-------|-------|-------:|-----:|-------:|------:|--------------------|
| `arena_compaction` | Arena / compaction / GC | 0 | 0 | 75 | 75 | P0 — well-contained, batch drivers already exist |
| `mutation_dirty` | Mutation / dirty propagation / provenance | 0 | 0 | 208 | 208 | P0 — high volume; strong domain suite foothold |
| `fiber_orch` | Fiber / orchestration / steal / Guard | 0 | 0 | 90 | 90 | P1 — domain suite already collapses many obs gates |
| `linear_ownership` | Linear ownership / borrow / consume | 0 | 0 | 19 | 19 | P1 — small, already partially batched |
| `edsl_hygiene` | EDSL / macro hygiene / reflect | 0 | 0 | 41 | 41 | P1 — domain hygiene suite exists |
| `jit_incremental` | JIT / AOT / incremental relower | 0 | 0 | 74 | 74 | P2 — link-profile heavy; migrate AC smoke first |
| `shape_soa` | Shape / SoA / column layout | 0 | 0 | 45 | 45 | P2 — small-medium; soa_batch precedent |
| `observability` | Observability / metrics / query:*-stats | 0 | 0 | 126 | 126 | P2 — often thin schema probes; collapse into obs matrix |
| `uncategorized` | Uncategorized / mixed | 0 | 0 | 42 | 42 | P3 — review case-by-case |

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
- Existing `*_batch` drivers (migration milestones): **66**

### Multi-file issue groups (consolidate first)


### Smallest issue tests (triage for obs-matrix fold or drop)


### Batch drivers already present

- `tests/core/test_arena_batch.cpp` → theme `arena_compaction`
- `tests/compiler/test_atomic_batch_core_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_atomic_batch_rollback_closed_loop.cpp` → theme `mutation_dirty`
- `tests/compiler/test_atomic_batch_rollback_fiber_task1.cpp` → theme `mutation_dirty`
- `tests/compiler/test_atomic_batch_snapshot_stable_ref_ai_loops.cpp` → theme `mutation_dirty`
- `tests/compiler/test_batch_dirty_cascade_2522.cpp` → theme `mutation_dirty`
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
- `tests/core/test_flatast_atomic_lock_batch.cpp` → theme `shape_soa`
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
- `tests/compiler/test_query_and_replace_batch_2527.cpp` → theme `mutation_dirty`
- `tests/compiler/test_query_pattern_batch.cpp` → theme `edsl_hygiene`
- `tests/compiler/test_reflect_batch.cpp` → theme `edsl_hygiene`
- `tests/reflect/test_reflect_hygiene_unit_batch.cpp` → theme `edsl_hygiene`
- `tests/reflect/test_reflect_macro_hygiene_batch.cpp` → theme `edsl_hygiene`
- `tests/reflect/test_reflect_pattern_hygiene_batch.cpp` → theme `edsl_hygiene`
- `tests/core/test_resource_quota_batch.cpp` → theme `arena_compaction`
- `tests/compiler/test_shape_soa_unit_batch.cpp` → theme `shape_soa`
- `tests/core/test_soa_batch.cpp` → theme `shape_soa`
- `tests/compiler/test_stable_ref_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_stable_ref_cow_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_stable_ref_provenance_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_typechecker_incremental_batch.cpp` → theme `jit_incremental`
- `tests/compiler/test_walk_batch.cpp` → theme `mutation_dirty`

### Domain suites (do not regress; extend these)

- `tests/compiler/test_adaptive_cascade_depth_partial_thr_2209.cpp`
- `tests/compiler/test_adaptive_partial_relower_threshold_2112.cpp`
- `tests/compiler/test_adaptive_reverify_limit_2146.cpp`
- `tests/core/test_add_node_builder_contract_2445.cpp`
- `tests/compiler/test_adt_exhaustiveness_audit_2223.cpp`
- `tests/compiler/test_adt_hard_gate_exhaustiveness_2264.cpp`
- `tests/compiler/test_adt_match_exhaust_post_mutate_reliability.cpp`
- `tests/compiler/test_adt_match_exhaustiveness_incremental_task2.cpp`
- `tests/compiler/test_adt_match_goal_table_2564.cpp`
- `tests/compiler/test_aether_denseness_residual_2578.cpp`
- `tests/orch/test_agent_apply_mutex_2158.cpp`
- `tests/orch/test_agent_ask_2231.cpp`
- `tests/orch/test_agent_ask_typed_corr_2538.cpp`
- `tests/orch/test_agent_failure_policy_2229.cpp`
- `tests/orch/test_agent_max_no_yield_2540.cpp`
- `tests/orch/test_agent_name_table_isolation_2078.cpp`
- `tests/orch/test_agent_scope_2083.cpp`
- `tests/orch/test_agent_scope_hierarchy_2537.cpp`
- `tests/compiler/test_anonymous_residual_stable_id_policy_2605.cpp`
- `tests/compiler/test_aot_anonymous_closure_policy_2238.cpp`
- `tests/compiler/test_aot_bridge_checkpoint_version_steal.cpp`
- `tests/compiler/test_aot_hot_update_health_2506.cpp`
- `tests/compiler/test_aot_incremental_reemit.cpp`
- `tests/compiler/test_aot_jit_joint_versioning_2046.cpp`
- `tests/compiler/test_aot_mangle_top.cpp`
- `tests/compiler/test_aot_region_per_eval.cpp`
- `tests/compiler/test_aot_reload_primitive.cpp`
- `tests/compiler/test_aot_shell_c0_escape.cpp`
- `tests/compiler/test_aot_stats_null_metrics.cpp`
- `tests/compiler/test_aot_version_triple_2306.cpp`
- `tests/compiler/test_apply_closure_envframe_soa.cpp`
- `tests/core/test_arena_adaptive_compact_2059.cpp`
- `tests/core/test_arena_auto_compact_fiber_defag_shape_dirty_closedloop.cpp`
- `tests/core/test_arena_auto_compact_intelligent.cpp`
- `tests/core/test_arena_batch.cpp`
- `tests/core/test_arena_compact_hook_concurrent.cpp`
- `tests/core/test_arena_compact_hook_stats_2381.cpp`
- `tests/core/test_arena_compact_notify_lifecycle_2438.cpp`
- `tests/core/test_arena_concurrent_mutex.cpp`
- `tests/core/test_arena_defrag.cpp`
- `tests/core/test_arena_dtor_clears_hooks_2382.cpp`
- `tests/core/test_arena_lifecycle.cpp`
- `tests/compiler/test_arithmetic_int64_safety.cpp`
- `tests/compiler/test_ast_column_compaction_closed_loop.cpp`
- `tests/core/test_ast_concurrency.cpp`
- `tests/core/test_ast_ops_stats_workspace_lock.cpp`
- `tests/reflect/test_ast_pod_reflect_b3.cpp`
- `tests/compiler/test_ast_workspace_modules.cpp`
- `tests/compiler/test_atomic_batch_core_batch.cpp`
- `tests/compiler/test_atomic_batch_rollback_closed_loop.cpp`
- `tests/compiler/test_atomic_batch_rollback_fiber_task1.cpp`
- `tests/compiler/test_atomic_batch_snapshot_stable_ref_ai_loops.cpp`
- `tests/serve/test_atomic_mark_bitvector_2117.cpp`
- `tests/stdlib/test_atomic_swap_stdlib.cpp`
- `tests/compiler/test_audit_mid_fallback_slo_2594.cpp`
- `tests/compiler/test_audit_mutation_id_unify_2493.cpp`
- `tests/compiler/test_audit_ring_publish_2530.cpp`
- `tests/compiler/test_audit_wal_force_multi_tenant_2150.cpp`
- `tests/compiler/test_aura_jit_unused_fn_lock_2475.cpp`
- `tests/compiler/test_aura_result_error_policy.cpp`
- `tests/compiler/test_aura_sandbox_env_2076.cpp`
- `tests/compiler/test_batch_dirty_cascade_2522.cpp`
- `tests/compiler/test_bidirectional_annotation.cpp`
- `tests/compiler/test_bidirectional_match_check_2348.cpp`
- `tests/core/test_bidirectional_stats.cpp`
- `tests/core/test_binding_gens_atomic_2417.cpp`
- `tests/compiler/test_blame_complete_commit_gate_2221.cpp`
- `tests/compiler/test_blame_occurrence_agent_ratios.cpp`
- `tests/compiler/test_blame_soft_recover_2561.cpp`
- `tests/compiler/test_blame_stamp_on_degrade.cpp`
- `tests/compiler/test_blame_tracking_typed_mutate.cpp`
- `tests/compiler/test_boundary_solve_hard_gate_2260.cpp`
- `tests/serve/test_boundary_yield_steal_metrics_2119.cpp`
- `tests/compiler/test_bugfix_968.cpp`
- `tests/compiler/test_build_kv_hash_batch.cpp`
- `tests/compiler/test_cache_entry_version_stamp.cpp`
- `tests/reflect/test_cache_header_magic_a2.cpp`
- `tests/compiler/test_cache_stamp_restamp_contract_2183.cpp`
- `tests/compiler/test_cap_write_effect_matrix_2532.cpp`
- `tests/core/test_capability_audit_publish_2425.cpp`
- `tests/compiler/test_capability_effect_force_2072.cpp`
- `tests/core/test_capability_effect_stats_snapshot_2430.cpp`
- `tests/compiler/test_capability_gating.cpp`
- `tests/compiler/test_capability_high_risk_promote_2489.cpp`
- `tests/core/test_capability_registry_snapshot_2426.cpp`
- `tests/core/test_capability_sandbox_batch.cpp`
- `tests/core/test_capability_single_use_consume_2586.cpp`
- `tests/compiler/test_capability_string_matrix_unify_2387.cpp`
- `tests/compiler/test_capability_unified_2077.cpp`
- `tests/compiler/test_cascade_incremental_pass_suite_2044.cpp`
- `tests/compiler/test_cascade_skip_metrics_2106.cpp`
- `tests/compiler/test_castop_density_closed_loop_2459.cpp`
- `tests/compiler/test_castop_density_hard_2358.cpp`
- `tests/compiler/test_channel_rendezvous_2483.cpp`
- `tests/serve/test_chaos_mutate_steal_gc_mailbox_2352.cpp`
- `tests/serve/test_chaos_steal_mutation_gc.cpp`
- `tests/core/test_clear_macro_dirty_concurrent_2442.cpp`
- `tests/compiler/test_closedloop_stats_hash_cap.cpp`
- `tests/compiler/test_closure_batch.cpp`
- `tests/compiler/test_closure_bridge_lifetime.cpp`
- `tests/compiler/test_closure_call_must_deopt_toctou_2472.cpp`
- `tests/compiler/test_closure_cow_gen_stamp_2547.cpp`
- `tests/compiler/test_closure_free.cpp`
- `tests/compiler/test_closure_view_batch.cpp`
- `tests/compiler/test_coercion_ban_weak_ir_2261.cpp`
- `tests/compiler/test_coercion_dead_elim_castop_flow_zerooverhead.cpp`
- `tests/compiler/test_coercion_dual_require_2562.cpp`
- `tests/compiler/test_coercion_prov_slo_2558.cpp`
- `tests/compiler/test_coercion_provenance_fast_strict_2147.cpp`
- `tests/compiler/test_coercion_provenance_miss_force_audit_2102.cpp`
- `tests/compiler/test_coercion_reject_production_defaults_2185.cpp`
- `tests/compiler/test_coercion_stamp_at_add_2512.cpp`
- `tests/compiler/test_command_line_cap_io_read_2478.cpp`
- `tests/compiler/test_commercial_tenant_profile_2584.cpp`
- `tests/compiler/test_commit_readiness_score_2553.cpp`
- `tests/compiler/test_compact_policy_2500.cpp`
- `tests/compiler/test_compile02_no_dup_imports.cpp`
- `tests/compiler/test_compile_primitive_guard.cpp`
- `tests/compiler/test_compiler_closure_env_safety_post_invalidate.cpp`
- `tests/compiler/test_compiler_core_incremental_selfmod_gaps.cpp`
- `tests/core/test_compiler_metrics_ownership.cpp`
- `tests/compiler/test_compiler_service_ownership.cpp`
- `tests/compiler/test_composite_commit_cs_reuse_2180.cpp`
- `tests/compiler/test_composite_cs_signature_matrix_2509.cpp`
- `tests/compiler/test_composite_nested_txn_invariant_audit.cpp`
- `tests/compiler/test_composite_txn_commit_2105.cpp`
- `tests/compiler/test_composite_typed_mutate.cpp`
- `tests/compiler/test_comprehensive_live_closure_expire_2042.cpp`
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
- `tests/compiler/test_cross_cow_drift_contract_2505.cpp`
- `tests/compiler/test_cross_cow_soft_migrate_2371.cpp`
- `tests/stdlib/test_datetime.cpp`
- `tests/compiler/test_dead_coercion_batch.cpp`
- `tests/compiler/test_dead_coercion_columnar_2431.cpp`
- `tests/compiler/test_dead_coercion_dirty_cone_2556.cpp`
- `tests/compiler/test_dead_coercion_elim.cpp`
- `tests/compiler/test_dead_coercion_elision_narrow_batch.cpp`
- `tests/compiler/test_dead_coercion_layered_2282.cpp`
- `tests/compiler/test_dead_coercion_pipeline_wire.cpp`
- `tests/core/test_defines_referencing_sym_2448.cpp`
- `tests/compiler/test_defuse_version_closed_loop.cpp`
- `tests/compiler/test_delta_truncate_goal_priority_2508.cpp`
- `tests/compiler/test_densify_envframe_ok_2361.cpp`
- `tests/compiler/test_densify_last_call_axes_2376.cpp`
- `tests/compiler/test_densify_ownership_scan_fail_gate_2497.cpp`
- `tests/compiler/test_densify_remap_pairing_2368.cpp`
- `tests/compiler/test_densify_root_closure_closed_loop_2365.cpp`
- `tests/core/test_dep_graph_concurrent.cpp`
- `tests/compiler/test_dep_graph_hybrid_cascade_2110.cpp`
- `tests/compiler/test_dep_graph_partial_relower_threshold.cpp`
- `tests/serve/test_depth_safe_mutation_boundary_steal_2115.cpp`
- `tests/compiler/test_dirty_aware_shape_linear_passes_2130.cpp`
- `tests/compiler/test_dirty_cascade_optimize.cpp`
- `tests/core/test_dirty_column_lock_2423.cpp`
- `tests/compiler/test_dirty_propagation_cascade.cpp`
- `tests/compiler/test_dirty_propagation_cost_closed_loop.cpp`
- `tests/compiler/test_dirty_reason_verification_propagation.cpp`
- `tests/compiler/test_dispatch_required_effects_2152.cpp`
- `tests/compiler/test_dual_path_desync_hard_fail_2116.cpp`
- `tests/compiler/test_edsl_concurrent_fiber_boundary_task1.cpp`
- `tests/compiler/test_edsl_concurrent_query_mutate.cpp`
- `tests/compiler/test_edsl_core_stability_cow_atomic_query_mutate.cpp`
- `tests/compiler/test_edsl_query_mutate_commercial_closed_loop.cpp`
- `tests/compiler/test_edsl_validate_or_refresh_2186.cpp`
- `tests/compiler/test_effect_epoch_mutation_unify_2149.cpp`
- `tests/compiler/test_emit_object_deprecated_2477.cpp`
- `tests/compiler/test_engine_metrics_facade.cpp`
- `tests/reflect/test_enum_name_table_c1.cpp`
- `tests/compiler/test_env_batch.cpp`
- `tests/compiler/test_env_lookup_batch.cpp`
- `tests/compiler/test_envframe_bridge_invalidate.cpp`
- `tests/compiler/test_envframe_dualpath_stale_closed_loop.cpp`
- `tests/compiler/test_envframe_epoch_batch.cpp`
- `tests/compiler/test_envframe_ownership_steal_densify_2362.cpp`
- `tests/compiler/test_envframe_resolve_distinction.cpp`
- `tests/compiler/test_envframe_truncate_epoch.cpp`
- `tests/core/test_envframe_truncate_guard_dual_epoch.cpp`
- `tests/compiler/test_epoch_apply_batch.cpp`
- `tests/compiler/test_epoch_bump_invariant_2304.cpp`
- `tests/compiler/test_epoch_invariant_complete_2501.cpp`
- `tests/compiler/test_epoch_invariant_soft_prod_2541.cpp`
- `tests/compiler/test_epoch_invariant_walk_2366.cpp`
- `tests/reflect/test_error_kind_names_wire.cpp`
- `tests/reflect/test_error_merr.cpp`
- `tests/compiler/test_escape_gate_steal_densify_clear_2507.cpp`
- `tests/compiler/test_escape_move_elision_gate_2263.cpp`
- `tests/compiler/test_eval_current_no_auto_fix_2484.cpp`
- `tests/compiler/test_eval_relower_hotpath.cpp`
- `tests/compiler/test_exhausted_min_dirty_reemit_2544.cpp`
- `tests/orch/test_failure_policy_bridge_2539.cpp`
- `tests/serve/test_fiber_concurrent_unit_batch.cpp`
- `tests/serve/test_fiber_integration_batch.cpp`
- `tests/compiler/test_fiber_macro_hygiene_refresh.cpp`
- `tests/serve/test_fiber_migration_refresh_2194.cpp`
- `tests/serve/test_fiber_mutation_steal_safety.cpp`
- `tests/orch/test_fiber_native_keepalive_2159.cpp`
- `tests/serve/test_fiber_orch_core_batch.cpp`
- `tests/serve/test_fiber_orch_parallel_quota_batch.cpp`
- `tests/serve/test_fiber_reclaim_orphan_release_2498.cpp`
- `tests/serve/test_fiber_reclaim_safety.cpp`
- `tests/compiler/test_fiber_resume_batch.cpp`
- `tests/serve/test_fiber_resume_state.cpp`
- `tests/serve/test_fiber_steal_panic_checkpoint_nested_gc.cpp`
- `tests/serve/test_fiber_strategy_evolve_batch.cpp`
- `tests/serve/test_fiber_synthesize_batch.cpp`
- `tests/compiler/test_fine_dirty_relower.cpp`
- `tests/core/test_fixup_deltas_2392.cpp`
- `tests/reflect/test_flat_instr_reflect_b2.cpp`
- `tests/core/test_flatast_add_node_lock_2413.cpp`
- `tests/core/test_flatast_atomic_lock_batch.cpp`
- `tests/core/test_flatast_soa_read_guard_2488.cpp`
- `tests/compiler/test_followup_smoke.cpp`
- `tests/compiler/test_followups.cpp`
- `tests/core/test_force_compact_hard_mutex_2157.cpp`
- `tests/compiler/test_force_jit_repromote_2502.cpp`
- `tests/compiler/test_frame_budget_cascade_isolation_2137.cpp`
- `tests/compiler/test_full_strategy_partial_recovery.cpp`
- `tests/serve/test_gc_batch.cpp`
- `tests/compiler/test_gc_closures_mtx_flush_sweep_2473.cpp`
- `tests/serve/test_gc_compact_batch.cpp`
- `tests/serve/test_gc_compact_sweep_batch.cpp`
- `tests/compiler/test_gc_coord_scope_2131.cpp`
- `tests/core/test_gc_defer_arm_fetch_or_2428.cpp`
- `tests/core/test_gc_defer_mutation_hold_2204.cpp`
- `tests/core/test_gc_defer_overflow_policy_atomic_2429.cpp`
- `tests/core/test_gc_defer_reconcile_cas_2437.cpp`
- `tests/core/test_gc_evaluator_integration.cpp`
- `tests/compiler/test_gc_heap_cells_clear_2486.cpp`
- `tests/serve/test_gc_mark_size_inject_2084.cpp`
- `tests/core/test_general_object_pin_2298.cpp`
- `tests/core/test_general_object_pin_adopt_2363.cpp`
- `tests/core/test_general_object_pin_coverage_gate_2496.cpp`
- `tests/core/test_get_nodeview_snapshot_2453.cpp`
- `tests/compiler/test_grant_bound_mid_force_2531.cpp`
- `tests/compiler/test_grant_epoch_fiber_bind_2055.cpp`
- `tests/compiler/test_grant_epoch_invalidation_2074.cpp`
- `tests/compiler/test_grant_epoch_retain_restricted_2529.cpp`
- `tests/compiler/test_grant_epoch_retain_window_2154.cpp`
- `tests/compiler/test_grant_macro_self_evo_stamp_2386.cpp`
- `tests/core/test_guard_dtor_batch_metrics.cpp`
- `tests/compiler/test_guard_exit_occurrence_refresh_2144.cpp`
- `tests/serve/test_guard_panic_reflect_fiber_resume_task6.cpp`
- `tests/compiler/test_hard_fiber_isolation_2151.cpp`
- `tests/compiler/test_hard_fiber_restricted_2536.cpp`
- `tests/compiler/test_hard_gate_full_strict_2145.cpp`
- `tests/compiler/test_hardware_resource_linear_ownership.cpp`
- `tests/core/test_has_on_compact_hook_lock_2383.cpp`
- `tests/core/test_hash_iter_invalidation.cpp`
- `tests/compiler/test_highperf_cpp26_gaps_arena_soa_value_shape_pass.cpp`
- `tests/core/test_highperf_full_hotpath_matrix.cpp`
- `tests/compiler/test_hot_contract_placement_2435.cpp`
- `tests/compiler/test_hot_contract_unify_2142.cpp`
- `tests/compiler/test_hot_pass_dirty_soa_2060.cpp`
- `tests/compiler/test_hot_pass_hard_dod_2434.cpp`
- `tests/compiler/test_hot_pass_pure_wrap_2258.cpp`
- `tests/compiler/test_hot_strategy_2582.cpp`
- `tests/compiler/test_hot_update_cascade_dirty_reemit.cpp`
- `tests/core/test_hotpath_matrix_batch.cpp`
- `tests/compiler/test_hygiene_checkpoint_2099.cpp`
- `tests/reflect/test_hygiene_diagnostic_2167.cpp`
- `tests/compiler/test_hygiene_mutate_closed_loop.cpp`
- `tests/core/test_incoming_parent_dirty_atomic_2416.cpp`
- `tests/compiler/test_incremental_effectiveness_snapshot_fail.cpp`
- `tests/compiler/test_incremental_perblock_closure_bridge_safety.cpp`
- `tests/compiler/test_incremental_relower_batch.cpp`
- `tests/core/test_incremental_restamp_2061.cpp`
- `tests/compiler/test_incremental_soundness_oracle_2113.cpp`
- `tests/compiler/test_incremental_type_batch.cpp`
- `tests/compiler/test_incremental_typed_selfmod_dirty_narrowing.cpp`
- `tests/compiler/test_inline_pass_batch.cpp`
- `tests/compiler/test_inline_typecheck_exception.cpp`
- `tests/serve/test_inner_steal_starvation.cpp`
- `tests/compiler/test_instr_impact_minimal_dirty_2126.cpp`
- `tests/compiler/test_instr_level_impact_scope.cpp`
- `tests/compiler/test_instr_level_relower_pass_2133.cpp`
- `tests/compiler/test_instruction_level_impact_partial_2109.cpp`
- `tests/compiler/test_invalidate_cascade_order.cpp`
- `tests/compiler/test_invalidate_consistency.cpp`
- `tests/compiler/test_invalidations_stats_workspace_lock.cpp`
- `tests/compiler/test_ir.cpp`
- `tests/reflect/test_ir_cache_v2.cpp`
- `tests/compiler/test_ir_const_string_intern_2573.cpp`
- `tests/compiler/test_ir_metadata_interpreter_jit_closed_loop.cpp`
- `tests/compiler/test_ir_optimize_type_info_chain_2471.cpp`
- `tests/reflect/test_ir_pod_phase4_2291.cpp`
- `tests/compiler/test_ir_soa_dual_emit_batch.cpp`
- `tests/compiler/test_ir_soa_incremental_closed_loop.cpp`
- `tests/compiler/test_ir_soa_layout_stamp_2432.cpp`
- `tests/serve/test_is_stealable_snapshot_gate_2549.cpp`
- `tests/compiler/test_isolation_audit_mid_2156.cpp`
- `tests/compiler/test_isolation_stamp_resolve_2224.cpp`
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
- `tests/compiler/test_jit_dual_string_heap_2575.cpp`
- `tests/compiler/test_jit_full_opcode_coverage.cpp`
- `tests/compiler/test_jit_interpreter_equivalence_oracle_2210.cpp`
- `tests/compiler/test_jit_macro_deopt_hygiene_2100.cpp`
- `tests/compiler/test_jit_macro_introduced_preserve.cpp`
- `tests/compiler/test_jit_metrics.cpp`
- `tests/compiler/test_jit_metrics_stub.cpp`
- `tests/orch/test_join_drain_reclaim_2227.cpp`
- `tests/serve/test_join_drain_timeout_2153.cpp`
- `tests/compiler/test_json_parse_number_exception_2480.cpp`
- `tests/compiler/test_json_parse_object_grow_2481.cpp`
- `tests/core/test_last_validated_generation_atomic_2394.cpp`
- `tests/compiler/test_layout_stamp_2170.cpp`
- `tests/compiler/test_layout_stamp_equality_8field_2519.cpp`
- `tests/compiler/test_let_poly_solve_delta.cpp`
- `tests/compiler/test_lifetime_contract_snapshot_2300.cpp`
- `tests/compiler/test_linear_batch.cpp`
- `tests/compiler/test_linear_boundary_consistency.cpp`
- `tests/compiler/test_linear_cross_closure_escape_2563.cpp`
- `tests/compiler/test_linear_enforce_boundary_align_2222.cpp`
- `tests/compiler/test_linear_enforce_production_defaults_2182.cpp`
- `tests/compiler/test_linear_enforce_strict_2103.cpp`
- `tests/compiler/test_linear_enforce_strict_default_2207.cpp`
- `tests/compiler/test_linear_escape_commit_hardblock_2108.cpp`
- `tests/compiler/test_linear_force_unified_2545.cpp`
- `tests/compiler/test_linear_gc_window_2043.cpp`
- `tests/compiler/test_linear_live_closure_walk.cpp`
- `tests/compiler/test_linear_ownership_batch.cpp`
- `tests/compiler/test_linear_ownership_occurrence_predicate_mutate.cpp`
- `tests/compiler/test_linear_ownership_postmutate_guard_steal_envframe.cpp`
- `tests/compiler/test_linear_partial_revalidate_2460.cpp`
- `tests/compiler/test_linear_pin_moving_compact_2280.cpp`
- `tests/compiler/test_linear_provenance_steal_gc_closed_loop.cpp`
- `tests/compiler/test_linear_runtime_violation.cpp`
- `tests/compiler/test_linear_state_stamp_apply_2129.cpp`
- `tests/compiler/test_linear_synth_boundary_authority_2514.cpp`
- `tests/compiler/test_linear_synth_violation_2357.cpp`
- `tests/compiler/test_linear_three_layer_wire_2559.cpp`
- `tests/compiler/test_linear_walk_active_closures.cpp`
- `tests/compiler/test_list_end_of_list_void_2482.cpp`
- `tests/compiler/test_list_vector_soa_hotpath_ai_loops.cpp`
- `tests/compiler/test_live_closure_full_restamp_2542.cpp`
- `tests/compiler/test_live_closure_stable_id_only_2369.cpp`
- `tests/compiler/test_load_cap_io_read_2485.cpp`
- `tests/core/test_lock_hierarchy.cpp`
- `tests/compiler/test_lock_order_audit_2316.cpp`
- `tests/compiler/test_lock_order_audit_2354.cpp`
- `tests/compiler/test_lock_order_closures_env.cpp`
- `tests/compiler/test_lock_order_production_soft_2557.cpp`
- `tests/compiler/test_longrunning_infra_primitives.cpp`
- `tests/compiler/test_longrunning_recovery_latency.cpp`
- `tests/compiler/test_lookup_stats_impl_heterogeneous.cpp`
- `tests/compiler/test_macro_cross_flat_hygiene_2235.cpp`
- `tests/core/test_macro_dirty_bits_lock_2441.cpp`
- `tests/compiler/test_macro_fiber_hygiene.cpp`
- `tests/compiler/test_macro_hygiene_closedloop_health.cpp`
- `tests/compiler/test_macro_hygiene_depth_concurrent_obs.cpp`
- `tests/compiler/test_macro_hygiene_fiber_panic_aot_soa_self_evo.cpp`
- `tests/compiler/test_macro_hygiene_limits_2101.cpp`
- `tests/compiler/test_macro_intro_restamp.cpp`
- `tests/compiler/test_macro_reflect_batch.cpp`
- `tests/compiler/test_macro_restamp_after_flat.cpp`
- `tests/compiler/test_macro_schema_dirty_propagate.cpp`
- `tests/compiler/test_macro_self_evo_capability.cpp`
- `tests/orch/test_mailbox_bp_admit_2228.cpp`
- `tests/orch/test_mailbox_bp_admit_default_2535.cpp`
- `tests/serve/test_mailbox_hold_exit_drain_2511.cpp`
- `tests/serve/test_mailbox_hold_starvation_hard_2551.cpp`
- `tests/serve/test_mailbox_recv_mutation_boundary_2188.cpp`
- `tests/serve/test_mailbox_tenant_principal_2592.cpp`
- `tests/core/test_marker_metadata_lock.cpp`
- `tests/compiler/test_matcher_stable_captures.cpp`
- `tests/compiler/test_memo_goal_epoch_health_2359.cpp`
- `tests/core/test_module_boundary.cpp`
- `tests/compiler/test_module_export_cache.cpp`
- `tests/compiler/test_module_export_display_2572.cpp`
- `tests/compiler/test_module_load_tail_export_2570.cpp`
- `tests/compiler/test_module_loader_dead_heap_circular.cpp`
- `tests/compiler/test_module_partition_map_2524.cpp`
- `tests/compiler/test_module_prefix_dead_heap.cpp`
- `tests/compiler/test_module_rebind_residual_2579.cpp`
- `tests/compiler/test_module_require_freevar_2566.cpp`
- `tests/core/test_moving_compact_2166.cpp`
- `tests/core/test_moving_densify_fail_closed_2495.cpp`
- `tests/compiler/test_must_deopt_before_next_call_2128.cpp`
- `tests/compiler/test_mutate_batch.cpp`
- `tests/compiler/test_mutate_capability_force_2052.cpp`
- `tests/compiler/test_mutate_cross_thread_migration.cpp`
- `tests/serve/test_mutate_mailbox_starvation_throttle_2587.cpp`
- `tests/compiler/test_mutate_type_gate_2219.cpp`
- `tests/compiler/test_mutation_aot_unit_batch.cpp`
- `tests/compiler/test_mutation_audit_wal.cpp`
- `tests/compiler/test_mutation_boundary_batch.cpp`
- `tests/serve/test_mutation_boundary_guard.cpp`
- `tests/compiler/test_mutation_concurrency_health_2379.cpp`
- `tests/compiler/test_mutation_contention_2040.cpp`
- `tests/serve/test_mutation_guard_try_acquire.cpp`
- `tests/compiler/test_mutation_guard_try_acquire_2124.cpp`
- `tests/compiler/test_mutation_guard_unit_batch.cpp`
- `tests/compiler/test_mutation_hold_estimate_2405.cpp`
- `tests/compiler/test_mutation_hold_hard_timeout_2199.cpp`
- `tests/compiler/test_mutation_hold_live_2517.cpp`
- `tests/compiler/test_mutation_hold_slo_2349.cpp`
- `tests/serve/test_mutation_hold_time.cpp`
- `tests/core/test_mutation_log_cow_copy_2457.cpp`
- `tests/compiler/test_mutation_log_pressure_2201.cpp`
- `tests/compiler/test_mutation_log_query_race.cpp`
- `tests/compiler/test_mutation_memory_blame_2196.cpp`
- `tests/compiler/test_mutation_occurrence_dirty_batch.cpp`
- `tests/compiler/test_mutation_provenance.cpp`
- `tests/compiler/test_mutation_rollback_coverage.cpp`
- `tests/serve/test_mutation_safety_snapshot_steal_2184.cpp`
- `tests/compiler/test_mutation_systemic_guard.cpp`
- `tests/compiler/test_mutation_typed_audit_batch.cpp`
- `tests/compiler/test_mutator_dispatch_stats_lock.cpp`
- `tests/compiler/test_named_closure_stable_id_at_create_2550.cpp`
- `tests/core/test_node_meta_bounds_2410.cpp`
- `tests/core/test_node_meta_gap_2411.cpp`
- `tests/reflect/test_node_tag_align_b1.cpp`
- `tests/reflect/test_obs_json_to_json_a1.cpp`
- `tests/compiler/test_obs_metrics_smoke_batch.cpp`
- `tests/compiler/test_obs_schema_matrix.cpp`
- `tests/compiler/test_observability_tier_table.cpp`
- `tests/compiler/test_occ_cache_stats_wired.cpp`
- `tests/compiler/test_occurrence_cache_key_2461.cpp`
- `tests/compiler/test_occurrence_dirty_blame_post_mutate.cpp`
- `tests/compiler/test_occurrence_goal_epoch_table_2278.cpp`
- `tests/compiler/test_occurrence_mutate_narrowing.cpp`
- `tests/compiler/test_occurrence_provenance_chain_completeness.cpp`
- `tests/compiler/test_occurrence_typing_blame_post_mutate_recovery.cpp`
- `tests/compiler/test_occurrence_typing_blame_post_mutate_task2.cpp`
- `tests/reflect/test_opcode_info_align_a3.cpp`
- `tests/reflect/test_opcode_reflect_2289.cpp`
- `tests/core/test_open_issues_meta_batch.cpp`
- `tests/compiler/test_open_issues_phase1_batch.cpp`
- `tests/compiler/test_optimization_passes_contracts.cpp`
- `tests/orch/test_orch_admission_decay.cpp`
- `tests/serve/test_orch_agent_mutation_boundary_2118.cpp`
- `tests/compiler/test_orch_hot_update_health_throttle_2543.cpp`
- `tests/orch/test_orch_obs_facade_2589.cpp`
- `tests/orch/test_orch_scope_2588.cpp`
- `tests/serve/test_orch_soft_boundary_unified_2515.cpp`
- `tests/serve/test_orchestration_steal_boost.cpp`
- `tests/serve/test_orphan_reap_stress.cpp`
- `tests/compiler/test_outermost_exit_order_2120.cpp`
- `tests/core/test_pair_slot_lock.cpp`
- `tests/core/test_pair_unchecked_safety.cpp`
- `tests/core/test_panic_checkpoint_batch.cpp`
- `tests/serve/test_panic_checkpoint_fiber_resume_safety.cpp`
- `tests/compiler/test_panic_defer_after_densify_2364.cpp`
- `tests/orch/test_parallel_intend_pure_2163.cpp`
- `tests/orch/test_parallel_intend_pure_contract_2230.cpp`
- `tests/core/test_param_annot_mutation_contract_2450.cpp`
- `tests/core/test_param_begin_count_publish_2451.cpp`
- `tests/core/test_param_data_mutation_contract_2449.cpp`
- `tests/compiler/test_partial_cone_cap_2560.cpp`
- `tests/compiler/test_partial_cs_single_source_2262.cpp`
- `tests/compiler/test_partial_recompile_single_evict_2476.cpp`
- `tests/compiler/test_partial_relower_cascade_2041.cpp`
- `tests/compiler/test_partial_relower_storm_gate_2190.cpp`
- `tests/compiler/test_pass_contracts_hotpath_closed_loop.cpp`
- `tests/compiler/test_pattern_structural_index_closed_loop.cpp`
- `tests/compiler/test_pcv_children_safe_default_migration.cpp`
- `tests/core/test_pcv_exclusive_with_set_2140.cpp`
- `tests/core/test_pcv_tls_default_on_2521.cpp`
- `tests/core/test_pcv_tls_scratch_2406.cpp`
- `tests/core/test_pcv_unique_hotpath_2058.cpp`
- `tests/compiler/test_per_defuse_batch.cpp`
- `tests/serve/test_per_fiber_stack_pool_high_concurrency.cpp`
- `tests/orch/test_per_scope_bp_admit_2591.cpp`
- `tests/compiler/test_per_symbol_dirty_cycle_guard.cpp`
- `tests/core/test_per_symbol_dirty_pool_lock.cpp`
- `tests/compiler/test_pereval_reemit_region_independence_2606.cpp`
- `tests/core/test_persist_basic.cpp`
- `tests/compiler/test_persistent_typechecker_2220.cpp`
- `tests/compiler/test_post_compact_lifecycle_2436.cpp`
- `tests/compiler/test_post_densify_linear_type_revalidate_2353.cpp`
- `tests/compiler/test_post_mutate_push_cascade.cpp`
- `tests/serve/test_post_steal_closed_loop.cpp`
- `tests/compiler/test_post_steal_linear_revalidate_2197.cpp`
- `tests/compiler/test_predicate_meet_join_lattice_2148.cpp`
- `tests/compiler/test_predicate_memo_boundary_selective_2104.cpp`
- `tests/core/test_prim_call_count_clamp.cpp`
- `tests/compiler/test_primcall_narg_2576.cpp`
- `tests/compiler/test_primcall_str_intern_2577.cpp`
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
- `tests/compiler/test_production_security_defaults_2053.cpp`
- `tests/compiler/test_production_stability_1014.cpp`
- `tests/compiler/test_production_sweep.cpp`
- `tests/serve/test_production_sweep.cpp`
- `tests/compiler/test_prompt2_6_impact_scope_quote_lambda_bridge_env.cpp`
- `tests/serve/test_prompt6_epoch_atomic_visibility_fiber_steal.cpp`
- `tests/compiler/test_prompt6_full_memory_safety_fuzz_stress.cpp`
- `tests/compiler/test_prompt6_linear_jit_l2_post_invalidate_arena_gc.cpp`
- `tests/compiler/test_propagate_marker_cycle_guard.cpp`
- `tests/compiler/test_provenance_blame_hygiene.cpp`
- `tests/compiler/test_query_and_replace_batch_2527.cpp`
- `tests/compiler/test_query_by_marker_provenance_2242.cpp`
- `tests/compiler/test_query_dispatch.cpp`
- `tests/compiler/test_query_epoch_contract_2192.cpp`
- `tests/compiler/test_query_hygiene_default_2525.cpp`
- `tests/compiler/test_query_index_composite_2403.cpp`
- `tests/compiler/test_query_mutate_consistency.cpp`
- `tests/compiler/test_query_namespace_audit.cpp`
- `tests/compiler/test_query_pattern_batch.cpp`
- `tests/compiler/test_query_pattern_default_hygiene_2123.cpp`
- `tests/compiler/test_query_pattern_hygiene_macrointroduced.cpp`
- `tests/compiler/test_quota_edge_cases.cpp`
- `tests/core/test_raii_guard_flatast_lifetime_2454.cpp`
- `tests/core/test_raw_pointer_safety.cpp`
- `tests/compiler/test_reemit_mutation_boundary_handshake_2114.cpp`
- `tests/compiler/test_reemit_production_default_defer_2205.cpp`
- `tests/compiler/test_reemit_production_default_defer_2208.cpp`
- `tests/compiler/test_refinement_closed_loop.cpp`
- `tests/compiler/test_reflect_batch.cpp`
- `tests/reflect/test_reflect_hygiene_agent_diagnostics.cpp`
- `tests/reflect/test_reflect_hygiene_unit_batch.cpp`
- `tests/reflect/test_reflect_isolation_2290.cpp`
- `tests/reflect/test_reflect_macro_hygiene_batch.cpp`
- `tests/reflect/test_reflect_pattern_hygiene_batch.cpp`
- `tests/compiler/test_regex_redos_timeout_2479.cpp`
- `tests/core/test_region_dense_atomic_2443.cpp`
- `tests/compiler/test_region_priority_deopt_throttle_2132.cpp`
- `tests/compiler/test_reload_recovery_query_2367.cpp`
- `tests/compiler/test_relower_fallback_reason_2193.cpp`
- `tests/compiler/test_relower_strategy_cache_lock.cpp`
- `tests/compiler/test_remount_force_deopt_2503.cpp`
- `tests/compiler/test_require_effect_auto_isolation_2490.cpp`
- `tests/compiler/test_require_effect_live_mid_2384.cpp`
- `tests/core/test_reset_slot_parent_edges_2412.cpp`
- `tests/serve/test_residual_defer_steal_hard_and_2546.cpp`
- `tests/serve/test_residual_force_safepoint_2533.cpp`
- `tests/compiler/test_residual_gc_defer_assert_2211.cpp`
- `tests/core/test_resource_quota_batch.cpp`
- `tests/compiler/test_rest_param_hygiene_2169.cpp`
- `tests/compiler/test_rest_param_hygiene_self_evo.cpp`
- `tests/compiler/test_rest_param_nested_qq_hygiene_2239.cpp`
- `tests/core/test_restamp_lazy_align_atomic_2421.cpp`
- `tests/core/test_restamp_sla_observability_2528.cpp`
- `tests/core/test_restore_children_structural_lock_2455.cpp`
- `tests/core/test_restricted_unset_principal_2385.cpp`
- `tests/compiler/test_reverify_expand_2356.cpp`
- `tests/compiler/test_rollback_by_marker_2237.cpp`
- `tests/core/test_root_epoch_gc_safety_post_invalidate.cpp`
- `tests/compiler/test_root_remap_pass_2267.cpp`
- `tests/compiler/test_root_remap_pin_contract_unified_2499.cpp`
- `tests/compiler/test_runtime_concurrent_full_cycle_chaos.cpp`
- `tests/serve/test_runtime_mutation_boundary_steal_safety.cpp`
- `tests/compiler/test_runtime_observability_correlated_stats.cpp`
- `tests/compiler/test_safe_snapshot_umbrella.cpp`
- `tests/serve/test_safe_yield_orchestration.cpp`
- `tests/serve/test_safepoint_mutation.cpp`
- `tests/core/test_sandbox_mode_atomic_2427.cpp`
- `tests/compiler/test_scan_skip_freed_closures.cpp`
- `tests/serve/test_scheduler_gc_defer_pending_panic_steal.cpp`
- `tests/serve/test_scheduler_gc_safepoint_mutation_coordination.cpp`
- `tests/serve/test_scheduler_llm_bottleneck_adaptive_steal_gc.cpp`
- `tests/compiler/test_security_audit_fold_2388.cpp`
- `tests/compiler/test_security_audit_trail_2075.cpp`
- `tests/compiler/test_security_audit_unify_2054.cpp`
- `tests/compiler/test_security_audit_wal_force_restricted_2492.cpp`
- `tests/compiler/test_security_event_wal_replay_2225.cpp`
- `tests/compiler/test_security_health_2389.cpp`
- `tests/compiler/test_security_posture_trail_2534.cpp`
- `tests/orch/test_security_schedule_gate_2590.cpp`
- `tests/compiler/test_self_evo_stats.cpp`
- `tests/serve/test_self_evolution_chaos_stable.cpp`
- `tests/compiler/test_self_evolution_loop_stats.cpp`
- `tests/serve/test_self_heal_policy_engine.cpp`
- `tests/compiler/test_selfevo_bugfix_941.cpp`
- `tests/core/test_set_arena_atomic_owner.cpp`
- `tests/core/test_set_workspace_flat.cpp`
- `tests/compiler/test_setcode_rebind_survive_2569.cpp`
- `tests/compiler/test_shape.cpp`
- `tests/compiler/test_shape_high_mutation_storm_2433.cpp`
- `tests/compiler/test_shape_jit_pass_deopt_incremental_closedloop_ai_mutate.cpp`
- `tests/compiler/test_shape_linear_collaborative_pass.cpp`
- `tests/compiler/test_shape_profiler_burst_closed_loop.cpp`
- `tests/compiler/test_shape_profiler_concurrency_2141.cpp`
- `tests/compiler/test_shape_profiler_stability_deopt_fiber_task4.cpp`
- `tests/compiler/test_shape_soa_unit_batch.cpp`
- `tests/compiler/test_shape_storm_adaptive_2526.cpp`
- `tests/compiler/test_shape_storm_partial_relower_2212.cpp`
- `tests/compiler/test_shapeprofiler_stability_deopt_jit_mutate.cpp`
- `tests/compiler/test_side_effect_inherit_2057.cpp`
- `tests/compiler/test_side_effect_security_gate_hardfail_2494.cpp`
- `tests/compiler/test_soa_ban_residual_aos_bridge_2520.cpp`
- `tests/core/test_soa_batch.cpp`
- `tests/compiler/test_soa_cascade_instr_dirty_sync.cpp`
- `tests/core/test_soa_column_atomic_2440.cpp`
- `tests/compiler/test_soa_dirty_aware_pipeline_2143.cpp`
- `tests/compiler/test_soa_generation_fence_2111.cpp`
- `tests/compiler/test_soa_partial_desync_gate_2181.cpp`
- `tests/compiler/test_soa_single_entry_dirty_sync_2139.cpp`
- `tests/compiler/test_soa_view_enforcement.cpp`
- `tests/compiler/test_solve_delta_epoch_filter.cpp`
- `tests/compiler/test_solve_delta_unresolved_export_2107.cpp`
- `tests/compiler/test_source_to_ir_desync_recovery_2206.cpp`
- `tests/compiler/test_source_to_ir_map_consistency_2045.cpp`
- `tests/serve/test_spawn_quota_no_leak_2155.cpp`
- `tests/compiler/test_spec_jit.cpp`
- `tests/stdlib/test_spec_runtime.cpp`
- `tests/compiler/test_specjit_per_eval_storm_isolation_2370.cpp`
- `tests/compiler/test_specjit_pereval_storm_e2e_2504.cpp`
- `tests/compiler/test_stable_ref_batch.cpp`
- `tests/compiler/test_stable_ref_cow_batch.cpp`
- `tests/compiler/test_stable_ref_cow_refresh_failclosed_2393.cpp`
- `tests/compiler/test_stable_ref_export_validate_2404.cpp`
- `tests/compiler/test_stable_ref_pin_lifecycle_2189.cpp`
- `tests/compiler/test_stable_ref_provenance_batch.cpp`
- `tests/serve/test_stable_ref_provenance_fiber_cow.cpp`
- `tests/core/test_stable_ref_tenant_capture_2125.cpp`
- `tests/compiler/test_stable_ref_tenant_mandate_2056.cpp`
- `tests/core/test_stable_ref_wire_endian_2395.cpp`
- `tests/compiler/test_stable_ref_wire_v2_2198.cpp`
- `tests/compiler/test_stale_closure_fallback.cpp`
- `tests/core/test_stale_ref_string_heap.cpp`
- `tests/compiler/test_static_reflect_selfmod_validation_task6.cpp`
- `tests/compiler/test_stats_catalog_drift.cpp`
- `tests/compiler/test_stats_facade_bench.cpp`
- `tests/compiler/test_stats_module_unification.cpp`
- `tests/stdlib/test_stdlib_infrastructure.cpp`
- `tests/compiler/test_stdlib_production_review_923.cpp`
- `tests/serve/test_steal_complete_gc_defer_2203.cpp`
- `tests/serve/test_steal_complete_restamp_txn_2510.cpp`
- `tests/serve/test_steal_complete_strong_entry_2377.cpp`
- `tests/serve/test_steal_layout_stamp_2351.cpp`
- `tests/serve/test_steal_safety_ticket_2518.cpp`
- `tests/serve/test_steal_snapshot_hard_invariant_2346.cpp`
- `tests/serve/test_steal_snapshot_soft_production_lock_2372.cpp`
- `tests/compiler/test_storm_isolation_2236.cpp`
- `tests/core/test_stress_alloc_storage_lock.cpp`
- `tests/core/test_stringpool_buf_fragmentation_lock_2409.cpp`
- `tests/core/test_stringpool_bytes_total_lock_2408.cpp`
- `tests/core/test_stringpool_concurrent_intern.cpp`
- `tests/core/test_structural_metadata_lock_order_2418.cpp`
- `tests/core/test_subtree_dirty_bounds_2424.cpp`
- `tests/core/test_subtree_gen_atomic_2422.cpp`
- `tests/core/test_subtree_uses_sym_template_bloat_2456.cpp`
- `tests/compiler/test_subtype_constraint_meet_2195.cpp`
- `tests/core/test_summary_flags_guard_2415.cpp`
- `tests/core/test_summary_recompute_sym_2414.cpp`
- `tests/compiler/test_symbol_eq_2568.cpp`
- `tests/stdlib/test_synthesize_namespace_demotion.cpp`
- `tests/compiler/test_sys_open_path_harden_2487.cpp`
- `tests/core/test_tag_arity_index_lock_2419.cpp`
- `tests/core/test_tag_arity_key_hash_2420.cpp`
- `tests/core/test_tenant_isolation_enforcement.cpp`
- `tests/compiler/test_tenant_scope_fiber_mandate_2491.cpp`
- `tests/compiler/test_test_strategy.cpp`
- `tests/compiler/test_tier_dispatch.cpp`
- `tests/compiler/test_timeout_repair_rich_roots_2548.cpp`
- `tests/core/test_transaction_guard_2555.cpp`
- `tests/compiler/test_tree_walker_fallback_strict_2213.cpp`
- `tests/compiler/test_truncate_commit_gate_2458.cpp`
- `tests/compiler/test_try_catch_bind_2567.cpp`
- `tests/core/test_try_lock_workspace_lock_order.cpp`
- `tests/core/test_type_cache_stats_snapshot.cpp`
- `tests/compiler/test_type_dep_epoch_prune_2355.cpp`
- `tests/compiler/test_type_dep_partial_merge_2283.cpp`
- `tests/compiler/test_type_dirty_cone_dep_graph_2191.cpp`
- `tests/compiler/test_type_dirty_txn_order_2516.cpp`
- `tests/compiler/test_type_freshness_steal_densify_2552.cpp`
- `tests/compiler/test_type_prop_invariant_correlation.cpp`
- `tests/compiler/test_type_propagation_dead_coercion.cpp`
- `tests/core/test_type_registry_ownership.cpp`
- `tests/compiler/test_type_system_health_2350.cpp`
- `tests/compiler/test_type_system_health_next_action_2462.cpp`
- `tests/compiler/test_type_timeout_repair_2284.cpp`
- `tests/compiler/test_typechecker_incremental_batch.cpp`
- `tests/compiler/test_typed_mutation_audit_decision_2281.cpp`
- `tests/compiler/test_typesystem_solve_delta_occurrence_priority_heavy_mutate.cpp`
- `tests/compiler/test_typesystem_type_propagation_jit_l2_typed_mutate.cpp`
- `tests/compiler/test_typesystem_typed_mutate_incremental_gaps.cpp`
- `tests/compiler/test_unified_invalidation.cpp`
- `tests/compiler/test_unify_invalidate_try_acquire.cpp`
- `tests/core/test_validate_node_no_abort_2390.cpp`
- `tests/core/test_validate_post_restore_soa_2391.cpp`
- `tests/compiler/test_value_encoding_v2_dispatch_contracts.cpp`
- `tests/compiler/test_value_tag_hot_path_2259.cpp`
- `tests/core/test_verification_dirty_bits_lock_2439.cpp`
- `tests/compiler/test_verify_parse_shared_helper.cpp`
- `tests/compiler/test_walk_batch.cpp`
- `tests/compiler/test_while_define_oneshot_2571.cpp`
- `tests/compiler/test_workload_adaptive_relower_2127.cpp`
- `tests/compiler/test_workspace_delete_child.cpp`
- `tests/compiler/test_workspace_dispatch.cpp`
- `tests/core/test_workspace_isolation_wire_2073.cpp`
- `tests/core/test_workspace_lock_reentrancy.cpp`
- `tests/compiler/test_workspace_mtx_contention_2523.cpp`
- `tests/compiler/test_workspace_region_concurrency_2121.cpp`
- `tests/core/test_workspace_state_lock.cpp`
- `tests/compiler/test_workspace_swap_guard.cpp`
- `tests/compiler/test_write_string_escape_2574.cpp`
- `tests/serve/test_yield_while_mutation_held_2200.cpp`

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

### `arena_compaction` — Arena / compaction / GC (75)

**Target:** tests/core/ (extend compact/gc family; see test_arena_batch / test_hotpath_matrix_batch)

**Priority:** P0 — well-contained, batch drivers already exist

#### domain/ (75)

- `tests/compiler/test_adt_match_exhaust_post_mutate_reliability.cpp` (—) [domain_suite, theme_compiler] — test_adt_match_exhaust_post_mutate_reliability.cpp — Issue #612:
- `tests/orch/test_agent_name_table_isolation_2078.cpp` (#2078) [domain_suite, theme_orch] — AC1: source cites #2078; no process-static OrchAgentNameTable;
- `tests/orch/test_agent_scope_2083.cpp` (#2083) [large, domain_suite, theme_orch] — test_agent_scope_2083.cpp — Issue #2083 AgentScope + #2161 watch_all
- `tests/core/test_arena_adaptive_compact_2059.cpp` (#2059) [domain_suite, theme_core] — AC1: compute_adaptive_headroom varies with mutation vs deopt storm
- `tests/core/test_arena_auto_compact_fiber_defag_shape_dirty_closedloop.cpp` (—) [domain_suite, theme_core] — (aura_issue_arena_auto_compact_fiber_defag_shape_dirty_closedloop_run). Stays at tests/core/ per
- `tests/core/test_arena_auto_compact_intelligent.cpp` (—) [domain_suite, theme_core] — Issue #1242/#1621/#187/#1919/#300 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_arena_batch.cpp` (—) [large, batch_driver, domain_suite, theme_core] — tests/core/test_arena_batch.cpp — consolidated arena batch driver. EXCLUDE_FROM_ALL.
- `tests/core/test_arena_compact_hook_concurrent.cpp` (—) [small, domain_suite, theme_core] — test_arena_compact_hook_concurrent.cpp — Issue #1989: ASTArena::on_compact_hook_
- `tests/core/test_arena_compact_hook_stats_2381.cpp` (#2381) [domain_suite, theme_core] — AC1: N=4 threads invoke compact_hook concurrently → TSAN clean path
- `tests/core/test_arena_compact_notify_lifecycle_2438.cpp` (#2438) [domain_suite, theme_core] — AC1: Documented invariant on hooks + clear_arena_compact_notify_hooks
- `tests/core/test_arena_concurrent_mutex.cpp` (—) [small, domain_suite, theme_core] — test_arena_concurrent_mutex.cpp — Issue #1988: ArenaGroup::arenas_ concurrent access.
- `tests/core/test_arena_defrag.cpp` (—) [domain_suite, theme_core] — tests/core/test_arena_defrag.cpp — Issue #1390: request_defrag + safepoint contract test.
- `tests/core/test_arena_dtor_clears_hooks_2382.cpp` (#2382) [domain_suite, theme_core] — AC1: After dtor, hook callables destroyed (shared_ptr capture use_count)
- `tests/core/test_arena_lifecycle.cpp` (—) [domain_suite, theme_core] — test_arena_lifecycle.cpp — Merged #1947/#1954 + #300 + #1359 (Anqi 2026-07-21).
- `tests/compiler/test_ast_column_compaction_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #261/#405/#414/#416 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_bidirectional_annotation.cpp` (—) [domain_suite, theme_compiler] — tests/test_bidirectional_annotation.cpp — Issue #1413: True
- `tests/compiler/test_closure_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_closure_batch.cpp
- `tests/compiler/test_compact_policy_2500.cpp` (#2500) [domain_suite, theme_compiler] — AC1: Table-driven pure compute_compact_policy (fixture → mode)
- `tests/stdlib/test_datetime.cpp` (—) [domain_suite, theme_stdlib] — test_datetime.cpp — Merged datetime stdlib tests (#1978).
- `tests/compiler/test_densify_remap_pairing_2368.cpp` (#2368) [domain_suite, theme_compiler] — AC1: Soft densify → pairing not forced, axes vacuous true, dual-epoch ok
- `tests/compiler/test_env_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_env_batch.cpp
- `tests/compiler/test_envframe_epoch_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_envframe_epoch_batch.cpp — EnvFrame / bridge_epoch batch driver.
- `tests/compiler/test_envframe_truncate_epoch.cpp` (—) [large, domain_suite, theme_compiler] — Issue #1842/#1889 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_envframe_truncate_guard_dual_epoch.cpp` (—) [domain_suite, theme_core] — Issue #1739/#1842/#1889/#1927/#1948 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_epoch_apply_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_epoch_apply_batch.cpp — epoch_apply pair dup-merge (R19 phase 15).
- `tests/compiler/test_epoch_bump_invariant_2304.cpp` (#2304) [domain_suite, theme_compiler] — Issue #2304 — post-bump hard invariant walk infrastructure.
- `tests/compiler/test_epoch_invariant_soft_prod_2541.cpp` (#2541) [domain_suite, theme_compiler] — AC1: production Restricted, AURA_EPOCH_INVARIANT unset → mode == 1
- `tests/compiler/test_epoch_invariant_walk_2366.cpp` (#2366) [domain_suite, theme_compiler] — AC1: Soft off → zero walks (single mode load)
- `tests/compiler/test_escape_move_elision_gate_2263.cpp` (#2263) [large, domain_suite, theme_compiler] — AC1: escape-after-move binding → MoveOp emitted; blocked counter bumps
- `tests/core/test_force_compact_hard_mutex_2157.cpp` (#2157) [domain_suite, theme_core] — LifetimePin + EnvFrameLifetimeGuard (no gen bump / pin invalidate while held).
- `tests/serve/test_gc_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — tests/serve/test_gc_batch.cpp — GC batch driver (arena theme; default-build).
- `tests/compiler/test_gc_closures_mtx_flush_sweep_2473.cpp` (#2473) [domain_suite, theme_compiler] — AC1: concurrent gc_root_count + register_active_closure + compact_sweep
- `tests/serve/test_gc_compact_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — tests/serve/test_gc_compact_batch.cpp — GC compact family batch driver.
- `tests/serve/test_gc_compact_sweep_batch.cpp` (—) [batch_driver, domain_suite, theme_serve] — tests/serve/test_gc_compact_sweep_batch.cpp — GC compact sweep batch driver.
- `tests/compiler/test_gc_coord_scope_2131.cpp` (#2131) [domain_suite, theme_compiler] — AC1: source cites #2131; GcCoordScope + PrePin/Cascade/PostAudit order
- `tests/core/test_gc_defer_arm_fetch_or_2428.cpp` (#2428) [domain_suite, theme_core] — AC1: arm_defer uses fetch_or (no separate load before note)
- `tests/core/test_gc_defer_overflow_policy_atomic_2429.cpp` (#2429) [domain_suite, theme_core] — AC1: policy check + arm/reject atomic under g_gc_defer_armed_mtx
- `tests/core/test_gc_defer_reconcile_cas_2437.cpp` (#2437) [domain_suite, theme_core] — AC1: Concurrent arm + reconcile does NOT clear another thread's Panic bit
- `tests/core/test_gc_evaluator_integration.cpp` (—) [domain_suite, theme_core] — test_gc_evaluator_integration.cpp — Issue #113 verification
- `tests/compiler/test_gc_heap_cells_clear_2486.cpp` (#2486) [domain_suite, theme_compiler] — AC1: after seeding cells + gc-heap, cells().size() == 0
- `tests/serve/test_gc_mark_size_inject_2084.cpp` (#2084) [domain_suite, theme_serve] — AC1: mark_from_roots with injected sizes → MarkBitVector size == heap size
- `tests/core/test_general_object_pin_2298.cpp` (#2298) [domain_suite, theme_core] — AC1: Non-render buffer pin-or-remap; validate succeeds after densify.
- `tests/core/test_general_object_pin_adopt_2363.cpp` (#2363) [domain_suite, theme_core] — AC1: wire_general_object_create_pair pins both buffers + bumps wire
- `tests/compiler/test_grant_epoch_retain_restricted_2529.cpp` (#2529) [domain_suite, theme_compiler] — AC1: Restricted + no multi-tenant → K==16
- `tests/compiler/test_grant_epoch_retain_window_2154.cpp` (#2154) [domain_suite, theme_compiler] — AC1: K=0 → no auto advance (identical to #2074 manual-only)
- `tests/compiler/test_grant_macro_self_evo_stamp_2386.cpp` (#2386) [domain_suite, theme_compiler] — AC1: After grant_macro_self_evo, grant_epoch non-zero (= Mutation epoch)
- `tests/core/test_has_on_compact_hook_lock_2383.cpp` (#2383) [domain_suite, theme_core] — AC1: All three has_* methods take their respective mutexes (source)
- `tests/core/test_highperf_full_hotpath_matrix.cpp` (—) [domain_suite, theme_core] — test_task4_highperf_full_hotpath_matrix.cpp — Issue #607:
- `tests/compiler/test_hot_contract_placement_2435.cpp` (#2435) [domain_suite, theme_compiler] — AC1: Production default: hot-loop contracts OFF (or observe)
- `tests/compiler/test_ir.cpp` (—) [large, domain_suite, theme_compiler] — 
- `tests/serve/test_issue_1990.cpp` (#1990) [small, domain_suite, theme_serve] — test_issue_1990.cpp — Issue #1990 / B-009: (gc-temp) and (gc-stats)
- `tests/serve/test_issue_1991.cpp` (#1991) [small, domain_suite, theme_serve] — test_issue_1991.cpp — Issue #1991 / B-010: (gc) primitive clears
- `tests/serve/test_issue_1993.cpp` (#1993) [domain_suite, theme_serve] — test_issue_1993.cpp — Issue #1993 (D-001): (gc-heap) direct-clear
- `tests/compiler/test_layout_stamp_2170.cpp` (#2170) [domain_suite, theme_compiler] — API for cross-subsystem epoch coherence (P1, MemorySafety-Review,
- `tests/compiler/test_memo_goal_epoch_health_2359.cpp` (#2359) [domain_suite, theme_compiler] — AC1: Two successive queries without mutate return identical epoch
- `tests/core/test_moving_compact_2166.cpp` (#2166) [large, domain_suite, theme_core] — Issue #2342 (Refine #2166): sharded LifetimePin registry (Option 1
- `tests/core/test_moving_densify_fail_closed_2495.cpp` (#2495) [domain_suite, theme_core] — AC1: Untracked live pointer + Moving densify of its referent → contract
- `tests/compiler/test_occurrence_cache_key_2461.cpp` (#2461) [domain_suite, theme_compiler] — AC1: same shape + epoch → second visit is structural key hit
- `tests/compiler/test_occurrence_goal_epoch_table_2278.cpp` (#2278) [domain_suite, theme_compiler] — AC1: clear_blame_context does NOT wipe OccurrenceGoal table
- `tests/compiler/test_outermost_exit_order_2120.cpp` (#2120) [domain_suite, theme_compiler] — Documented success exit order (AC5):
- `tests/compiler/test_post_compact_lifecycle_2436.cpp` (#2436) [domain_suite, theme_compiler] — AC1: Documented ordered lifecycle in post_compact_lifecycle.hh
- `tests/compiler/test_prompt6_linear_jit_l2_post_invalidate_arena_gc.cpp` (—) [domain_suite, theme_compiler] — test_prompt6_linear_jit_l2_post_invalidate_arena_gc.cpp — Issue #740:
- `tests/compiler/test_query_epoch_contract_2192.cpp` (#2192) [domain_suite, theme_compiler] — AC1: QueryEpoch defined; stamped on primary workspace queries
- `tests/compiler/test_quota_edge_cases.cpp` (—) [domain_suite, theme_compiler] — AC1: boundary 0→1 transition (unlimited → bounded reject)
- `tests/core/test_resource_quota_batch.cpp` (—) [large, batch_driver, domain_suite, theme_core] — tests/core/test_resource_quota_batch.cpp
- `tests/core/test_root_epoch_gc_safety_post_invalidate.cpp` (—) [domain_suite, theme_core] — test_compiler_root_epoch_gc_safety_post_invalidate.cpp — Issue #599:
- `tests/compiler/test_root_remap_pass_2267.cpp` (#2267) [domain_suite, theme_compiler] — capture rewrite after Moving densify. Verifies AC1–AC5 from #2294
- `tests/compiler/test_root_remap_pin_contract_unified_2499.cpp` (#2499) [domain_suite, theme_compiler] — contract. Phase 5 reads compact_r.pin_contract_held but loses the
- `tests/compiler/test_security_event_wal_replay_2225.cpp` (#2225) [domain_suite, theme_compiler] — AC1: ring ≥ 1024; ring-wrap-total increments when N>1024 denies
- `tests/compiler/test_security_health_2389.cpp` (#2389) [domain_suite, theme_compiler] — AC1: Fresh / vacuous → health_bp high / force-reason ok
- `tests/core/test_set_arena_atomic_owner.cpp` (—) [domain_suite, theme_core] — test_set_arena_atomic_owner.cpp — Issue #1663
- `tests/compiler/test_solve_delta_epoch_filter.cpp` (—) [small, domain_suite, theme_compiler] — Issue #2065 — solve_delta epoch filter test.
- `tests/serve/test_spawn_quota_no_leak_2155.cpp` (#2155) [domain_suite, theme_serve] — AC1: Quota reject never calls agent_names_->put (C++ + Aura)
- `tests/core/test_stringpool_buf_fragmentation_lock_2409.cpp` (#2409) [domain_suite, theme_core] — AC1: 4 writers intern + 4 readers buf_fragmentation (no crash)
- `tests/compiler/test_type_dep_epoch_prune_2355.cpp` (#2355) [domain_suite, theme_compiler] — AC1: After set_cache_epoch(e+1), edges stamped at epoch e (e>0) drop;

### `mutation_dirty` — Mutation / dirty propagation / provenance (208)

**Target:** tests/core/test_mutation_boundary_batch (domain/ pilot abandoned in R1)

**Priority:** P0 — high volume; strong domain suite foothold

#### domain/ (208)

- `tests/core/test_add_node_builder_contract_2445.cpp` (#2445) [domain_suite, theme_core] — AC1: single-threaded add_* path unchanged (builders work)
- `tests/compiler/test_adt_exhaustiveness_audit_2223.cpp` (#2223) [domain_suite, theme_compiler] — AC1: InvariantAuditResult::adt_ok + counters wired
- `tests/compiler/test_adt_match_exhaustiveness_incremental_task2.cpp` (—) [domain_suite, theme_compiler] — test_adt_match_exhaustiveness_incremental_task2.cpp
- `tests/compiler/test_adt_match_goal_table_2564.cpp` (#2564) [domain_suite, theme_compiler] — reverify roots for Soft delta fidelity.
- `tests/compiler/test_aether_denseness_residual_2578.cpp` (#2578) [domain_suite, theme_compiler] — AC1: orch:parallel 2-arg typechecks (dotted-rest + namespaced .aura-type)
- `tests/compiler/test_atomic_batch_core_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — R19 phase4 dup-merge — atomic-batch core trio: Issue #1899 (dispatch + STRONG atomicity) + Issue
- `tests/compiler/test_atomic_batch_rollback_closed_loop.cpp` (—) [batch_driver, domain_suite, theme_compiler] — Issue #192/#459/#529/#553 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_atomic_batch_rollback_fiber_task1.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_atomic_batch_rollback_fiber_task1.cpp —
- `tests/compiler/test_atomic_batch_snapshot_stable_ref_ai_loops.cpp` (—) [batch_driver, domain_suite, theme_compiler] — - AC1: workspace:snapshot + workspace:rollback-to primitives
- `tests/compiler/test_audit_mid_fallback_slo_2594.cpp` (#2594) [domain_suite, theme_compiler] — tests/compiler/test_audit_mid_fallback_slo_2594.cpp
- `tests/compiler/test_audit_mutation_id_unify_2493.cpp` (#2493) [domain_suite, theme_compiler] — AC1: require_effect deny under Restricted → SE.mutation_id matches
- `tests/compiler/test_audit_ring_publish_2530.cpp` (#2530) [domain_suite, theme_compiler] — AC1: both kAuditRing == 1024
- `tests/compiler/test_audit_wal_force_multi_tenant_2150.cpp` (#2150) [domain_suite, theme_compiler] — AC1: AURA_MULTI_TENANT=1 without WAL env → enabled + forced metric > 0
- `tests/compiler/test_aura_sandbox_env_2076.cpp` (#2076) [domain_suite, theme_compiler] — Issue #2076 — production default Restricted sandbox + Agent-readable
- `tests/compiler/test_batch_dirty_cascade_2522.cpp` (#2522) [batch_driver, domain_suite, theme_compiler] — AC1: Batch API exists; one generation bump per call regardless of N
- `tests/compiler/test_blame_complete_commit_gate_2221.cpp` (#2221) [domain_suite, theme_compiler] — AC1: Production defaults enable reject-on-miss + require-blame-on-commit;
- `tests/compiler/test_blame_occurrence_agent_ratios.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2030; ratio keys on self-evo-stats + fidelity-stats
- `tests/compiler/test_blame_soft_recover_2561.cpp` (#2561) [domain_suite, theme_compiler] — AC1: Sampled incomplete → recover restores dual fields OR escalate
- `tests/compiler/test_boundary_solve_hard_gate_2260.cpp` (#2260) [domain_suite, theme_compiler] — AC1: truncated_reverify under Full hard-gate → full resync or force fail
- `tests/compiler/test_cap_write_effect_matrix_2532.cpp` (#2532) [small, domain_suite, theme_compiler] — AC1: workspace/fiber map to non-None effects
- `tests/core/test_capability_audit_publish_2425.cpp` (#2425) [domain_suite, theme_core] — AC1: reader sees fully-written entry or prior complete entry (never torn)
- `tests/core/test_capability_effect_stats_snapshot_2430.cpp` (#2430) [domain_suite, theme_core] — AC1: snapshot consistent under concurrent metric writers
- `tests/core/test_capability_registry_snapshot_2426.cpp` (#2426) [domain_suite, theme_core] — AC1: snapshot consistent under concurrent policy writers
- `tests/core/test_capability_sandbox_batch.cpp` (—) [large, batch_driver, domain_suite, theme_core] — tests/core/test_capability_sandbox_batch.cpp
- `tests/compiler/test_capability_string_matrix_unify_2387.cpp` (#2387) [domain_suite, theme_compiler] — AC1: Registry-only grant mutate → has_capability("mutate") true
- `tests/compiler/test_capability_unified_2077.cpp` (#2077) [domain_suite, theme_compiler] — AC1: has_capability("mutate") under Strict consults the effect matrix,
- `tests/compiler/test_cascade_skip_metrics_2106.cpp` (#2106) [domain_suite, theme_compiler] — AC1: summary-dirty cascade skip → cascade_skip_subtree_total via metrics
- `tests/compiler/test_castop_density_closed_loop_2459.cpp` (#2459) [domain_suite, theme_compiler] — AC1: Soft path — no gate reject; optional force-JIT only under HARD
- `tests/core/test_clear_macro_dirty_concurrent_2442.cpp` (#2442) [domain_suite, theme_core] — AC1: concurrent clear_macro_dirty_all + macro_dirty(id) no torn reads
- `tests/compiler/test_closure_bridge_lifetime.cpp` (—) [domain_suite, theme_compiler] — Issue #1888/#1895/#1926/#1928/#1929/#1947 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_coercion_ban_weak_ir_2261.cpp` (#2261) [domain_suite, theme_compiler] — AC1: Sampled + no active_mutation_id / no log → no CoercionNode; miss reject;
- `tests/compiler/test_coercion_dead_elim_castop_flow_zerooverhead.cpp` (—) [domain_suite, theme_compiler] — test_coercion_dead_elim_castop_flow_zerooverhead.cpp
- `tests/compiler/test_coercion_dual_require_2562.cpp` (#2562) [domain_suite, theme_compiler] — AC1: Production / dual-require + incomplete dual → drop, counter++, no node
- `tests/compiler/test_coercion_prov_slo_2558.cpp` (#2558) [domain_suite, theme_compiler] — AC1: production + miss storm → bp < SLO → force pending; consume forces audit
- `tests/compiler/test_coercion_provenance_fast_strict_2147.cpp` (#2147) [domain_suite, theme_compiler] — AC1: both fields set at add → chain_walk_total does not increase (fast path)
- `tests/compiler/test_coercion_provenance_miss_force_audit_2102.cpp` (#2102) [domain_suite, theme_compiler] — AC1: blank predicate+mutation → miss total; force-audit on boundary exit
- `tests/compiler/test_coercion_reject_production_defaults_2185.cpp` (#2185) [domain_suite, theme_compiler] — AC1: Production defaults + incomplete chain → no CoercionNode;
- `tests/compiler/test_coercion_stamp_at_add_2512.cpp` (#2512) [domain_suite, theme_compiler] — AC1: active mid set → entry mid non-zero before apply; fast_path advances
- `tests/compiler/test_commercial_tenant_profile_2584.cpp` (#2584) [domain_suite, theme_compiler] — AC1: No AURA_COMMERCIAL_TENANT — Restricted default soft (#2536 regression)
- `tests/compiler/test_compiler_closure_env_safety_post_invalidate.cpp` (—) [domain_suite, theme_compiler] — test_compiler_closure_env_safety_post_invalidate.cpp —
- `tests/compiler/test_composite_nested_txn_invariant_audit.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2027; composite counters + partial recover helpers
- `tests/compiler/test_composite_txn_commit_2105.cpp` (#2105) [domain_suite, theme_compiler] — AC1: Nested/atomic_batch success path runs ordered revalidate before clean
- `tests/compiler/test_composite_typed_mutate.cpp` (—) [domain_suite, theme_compiler] — Issue #1408: Inline no-op stubs for aura::jit::AuraJIT::invalidate_prefix
- `tests/compiler/test_comprehensive_live_closure_expire_2042.cpp` (#2042) [domain_suite, theme_compiler] — Issue #2042 — comprehensive live IRClosure / tree-walker / PrimCall
- `tests/compiler/test_constraint_solver_surface_cross_delta.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2028; solve_delta_occurrence +
- `tests/compiler/test_constraint_system_solve_delta_cross_delta_task2.cpp` (—) [domain_suite, theme_compiler] — test_constraint_system_solve_delta_cross_delta_task2.cpp
- `tests/core/test_coverage_holes_workspace_lock.cpp` (—) [domain_suite, theme_core] — Issue #1816 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_dead_coercion_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_dead_coercion_batch.cpp
- `tests/compiler/test_dead_coercion_dirty_cone_2556.cpp` (#2556) [domain_suite, theme_compiler] — AC1: Partial cone → DCE only dirty blocks; cone-skips > 0 on multi-block fn
- `tests/core/test_defines_referencing_sym_2448.cpp` (#2448) [domain_suite, theme_core] — AC1: well-formed unique-name case still finds referencing Defines
- `tests/core/test_dep_graph_concurrent.cpp` (—) [domain_suite, theme_core] — test_dep_graph_concurrent.cpp — Issue #1376:
- `tests/compiler/test_dep_graph_hybrid_cascade_2110.cpp` (#2110) [domain_suite, theme_compiler] — DepGraph (hybrid cascade). Extended by Issue #2187 — block/instr
- `tests/serve/test_depth_safe_mutation_boundary_steal_2115.cpp` (#2115) [domain_suite, theme_serve] — AC1: Holding MutationBoundary (depth>0) fiber is never steal-safe
- `tests/compiler/test_dirty_aware_shape_linear_passes_2130.cpp` (#2130) [domain_suite, theme_compiler] — AC1: ShapeAwareFold with dirty mask only processes dirty blocks
- `tests/compiler/test_dirty_cascade_optimize.cpp` (—) [small, domain_suite, theme_compiler] — Issue #2063 — Dirty cascade subtree-skip (summary-dirty early-exit) test.
- `tests/core/test_dirty_column_lock_2423.cpp` (#2423) [domain_suite, theme_core] — AC1: concurrent mark_dirty + dirty_nodes_in_range (shared/exclusive lock)
- `tests/compiler/test_dirty_propagation_cascade.cpp` (—) [domain_suite, theme_compiler] — AC1: cascade_mark_dirty / propagate_closure BFS marks all dependents
- `tests/compiler/test_dirty_propagation_cost_closed_loop.cpp` (—) [small, domain_suite, theme_compiler] — Issue #398/#399/#408/#415 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_dirty_reason_verification_propagation.cpp` (—) [small, domain_suite, theme_compiler] — Issue #344/#415/#437/#469 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_edsl_core_stability_cow_atomic_query_mutate.cpp` (—) [domain_suite, theme_compiler] — test_edsl_core_stability_cow_atomic_query_mutate.cpp — Issue #655:
- `tests/compiler/test_edsl_query_mutate_commercial_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #552/#619/#634/#635/#636 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_edsl_validate_or_refresh_2186.cpp` (#2186) [domain_suite, theme_compiler] — AC1: query:children/node/parent/*-stable/node-marker/node-provenance
- `tests/compiler/test_effect_epoch_mutation_unify_2149.cpp` (#2149) [domain_suite, theme_compiler] — AC1: check_and_record_effect stamps EffectProvenance.epoch from
- `tests/compiler/test_envframe_bridge_invalidate.cpp` (—) [domain_suite, theme_compiler] — Issue #1916 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_envframe_dualpath_stale_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #417/#418/#543/#602 (#1978 renamed): issue# moved from filename to header.
- `tests/reflect/test_error_kind_names_wire.cpp` (—) [domain_suite, theme_reflect] — Wire C1 into business: P2996 validates name tables used by
- `tests/compiler/test_exhausted_min_dirty_reemit_2544.cpp` (#2544) [large, domain_suite, theme_compiler] — AC1: Continuous Defuse fail to exhaust → force-JIT mask set +
- `tests/compiler/test_followup_smoke.cpp` (—) [small, followup, domain_suite, theme_compiler] — tests/test_followup_smoke.cpp — Smoke test for follow-up ship
- `tests/compiler/test_followups.cpp` (—) [followup, domain_suite, theme_compiler] — (mutation-log:diff / dirty:summary /
- `tests/compiler/test_full_strategy_partial_recovery.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2029; partial_recovery_* counters + boundary path
- `tests/core/test_gc_defer_mutation_hold_2204.cpp` (#2204) [domain_suite, theme_core] — AC1: During outermost Guard body, should_defer_destructive_gc()==true
- `tests/compiler/test_grant_bound_mid_force_2531.cpp` (#2531) [small, domain_suite, theme_compiler] — AC1: Restricted grant → bound_mutation_id != 0
- `tests/compiler/test_grant_epoch_fiber_bind_2055.cpp` (#2055) [domain_suite, theme_compiler] — AC1: Grant always carries non-zero grant_epoch matching mutation epoch
- `tests/compiler/test_grant_epoch_invalidation_2074.cpp` (#2074) [domain_suite, theme_compiler] — Issue #2074 — mutation-bound CapabilityGrant + epoch invalidation
- `tests/core/test_guard_dtor_batch_metrics.cpp` (—) [batch_driver, domain_suite, theme_core] — Issue #1747 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_guard_exit_occurrence_refresh_2144.cpp` (#2144) [domain_suite, theme_compiler] — AC1: multi-round mutate on if-predicate binding → selective invalidate
- `tests/compiler/test_hard_gate_full_strict_2145.cpp` (#2145) [domain_suite, theme_compiler] — AC1: Full + injected use-after-move / Moved → rollback; mutation not visible
- `tests/compiler/test_hot_pass_dirty_soa_2060.cpp` (#2060) [domain_suite, theme_compiler] — AC1: DirtySoAEntryPass + kRequireDirtySoAEntry on production wraps
- `tests/compiler/test_hot_pass_hard_dod_2434.cpp` (#2434) [domain_suite, theme_compiler] — AC1: All production pack stages HotPassDodCompliant (or explicit Legacy)
- `tests/compiler/test_hot_pass_pure_wrap_2258.cpp` (#2258) [domain_suite, theme_compiler] — AC1: Pipeline registration rejects non-HotPassDodCompliant dirty/inc packs
- `tests/core/test_hotpath_matrix_batch.cpp` (—) [large, batch_driver, domain_suite, theme_core] — test_hotpath_matrix_batch.cpp — Domain suite batch: behavioral gates.
- `tests/compiler/test_hygiene_checkpoint_2099.cpp` (#2099) [domain_suite, theme_compiler] — AC1: save → mutate (introduce new MacroIntroduced node) → restore → pre-save
- `tests/compiler/test_hygiene_mutate_closed_loop.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2037; enforce_macro_hygiene_mutate_hotpath +
- `tests/core/test_incoming_parent_dirty_atomic_2416.cpp` (#2416) [domain_suite, theme_core] — AC1: flag is atomic (behavior: concurrent mark + ensure/collect)
- `tests/compiler/test_incremental_typed_selfmod_dirty_narrowing.cpp` (—) [domain_suite, theme_compiler] — test_incremental_typed_selfmod_dirty_narrowing.cpp — Merged #509/#518/#526/#536/#537/#550 +
- `tests/compiler/test_invalidate_cascade_order.cpp` (—) [domain_suite, theme_compiler] — test_invalidate_cascade_order.cpp — Issue #1378:
- `tests/compiler/test_isolation_audit_mid_2156.cpp` (#2156) [domain_suite, theme_compiler] — AC1: Isolation deny SecurityEvent.mutation_id is Mutation epoch space,
- `tests/compiler/test_issues_819_829_batch.cpp` (#819) [batch_driver, domain_suite, theme_compiler] — test_issues_819_829_batch.cpp — Phase 1 close for Issues #819–#829.
- `tests/core/test_last_validated_generation_atomic_2394.cpp` (#2394) [domain_suite, theme_core] — AC1: 4 threads validate_with_provenance on same ref — no race (TSAN)
- `tests/compiler/test_linear_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_linear_batch.cpp
- `tests/compiler/test_linear_enforce_boundary_align_2222.cpp` (#2222) [domain_suite, theme_compiler] — AC1: Production → process Strict; sandbox-off → Soft; boundary enter
- `tests/compiler/test_linear_enforce_production_defaults_2182.cpp` (#2182) [domain_suite, theme_compiler] — AC1: Production defaults → Strict; incomplete trail → hard error
- `tests/compiler/test_linear_enforce_strict_2103.cpp` (#2103) [domain_suite, theme_compiler] — AC1: Soft + incomplete trail → incomplete metric; ok continues
- `tests/compiler/test_linear_enforce_strict_default_2207.cpp` (#2207) [domain_suite, theme_compiler] — AC1: Default mode is Strict; Soft only via set_linear_enforce_mode(Soft)
- `tests/compiler/test_linear_force_unified_2545.cpp` (#2545) [domain_suite, theme_compiler] — AC1: Production/strict synth → force-rollback; soft recovery skipped;
- `tests/compiler/test_linear_gc_window_2043.cpp` (#2043) [domain_suite, theme_compiler] — Issue #2043 — Linear ownership post-mutate enforcement + GC/fiber
- `tests/compiler/test_linear_ownership_postmutate_guard_steal_envframe.cpp` (—) [domain_suite, theme_compiler] — test_linear_ownership_postmutate_guard_steal_envframe.cpp — Issue #800:
- `tests/compiler/test_linear_provenance_steal_gc_closed_loop.cpp` (—) [domain_suite, theme_compiler] — consistency closed-loop (shared validate_linear_provenance).
- `tests/compiler/test_linear_synth_boundary_authority_2514.cpp` (#2514) [domain_suite, theme_compiler] — AC1: Production synth hard-fail → boundary/hard-gate force-rollback;
- `tests/core/test_lock_hierarchy.cpp` (—) [domain_suite, theme_core] — the lock-hierarchy contract documented in Issue #1388.
- `tests/core/test_macro_dirty_bits_lock_2441.cpp` (#2441) [domain_suite, theme_core] — AC1: concurrent apply_macro_dirty_bits(same_id, same_reasons) → +1 metric
- `tests/compiler/test_macro_intro_restamp.cpp` (—) [domain_suite, theme_compiler] — tests/compiler/test_macro_restamp_after_flat.cpp (which covers
- `tests/compiler/test_macro_schema_dirty_propagate.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2098 + file-level atomic + C-linkage reader +
- `tests/core/test_marker_metadata_lock.cpp` (—) [domain_suite, theme_core] — Issue #1783 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_module_boundary.cpp` (—) [domain_suite, theme_core] — Issue #1885 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_module_rebind_residual_2579.cpp` (#2579) [domain_suite, theme_compiler] — AC1: set-code multi-define (define g (f)) binds call result, not procedure
- `tests/compiler/test_module_require_freevar_2566.cpp` (#2566) [domain_suite, theme_compiler] — AC1: (require "std/mutate" all:) inside non-std module → closures
- `tests/compiler/test_mutate_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_mutate_batch.cpp
- `tests/compiler/test_mutate_capability_force_2052.cpp` (#2052) [domain_suite, theme_compiler] — AC1: Source: add_mutate calls check_and_record_effect + check_workspace_isolation
- `tests/compiler/test_mutate_cross_thread_migration.cpp` (—) [domain_suite, theme_compiler] — test_mutate_cross_thread_migration.cpp — Issue #1373:
- `tests/serve/test_mutate_mailbox_starvation_throttle_2587.cpp` (#2587) [domain_suite, theme_serve] — tests/serve/test_mutate_mailbox_starvation_throttle_2587.cpp
- `tests/compiler/test_mutate_type_gate_2219.cpp` (#2219) [domain_suite, theme_compiler] — AC1: Soft default / Hard production; schema-2219 query surface
- `tests/compiler/test_mutation_audit_wal.cpp` (—) [domain_suite, theme_compiler] — append/rotate, full effect/tenant/epoch fields, replay into ring,
- `tests/compiler/test_mutation_boundary_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_mutation_boundary_batch.cpp
- `tests/serve/test_mutation_boundary_guard.cpp` (—) [domain_suite, theme_serve] — Issue #1747/#1897/#1931/#1950 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_mutation_concurrency_health_2379.cpp` (#2379) [domain_suite, theme_compiler] — AC1: Query returns health-bp + force-reason + components + schema/wired
- `tests/serve/test_mutation_guard_try_acquire.cpp` (—) [domain_suite, theme_serve] — Issue #1547/#1556/#1590/#1628 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_mutation_guard_try_acquire_2124.cpp` (#2124) [domain_suite, theme_compiler] — AC1: check_mutation_guard_coverage.py --strict → 0 legacy ctor residual
- `tests/compiler/test_mutation_guard_unit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_mutation_guard_unit_batch.cpp — consolidated mutation-theme drivers
- `tests/compiler/test_mutation_hold_estimate_2405.cpp` (#2405) [domain_suite, theme_compiler] — AC1: Query returns budget/slo + recent hold distribution (no side effects)
- `tests/compiler/test_mutation_hold_hard_timeout_2199.cpp` (#2199) [domain_suite, theme_compiler] — AC1: Strict on + synthetic long mutate → outermost exit fails,
- `tests/compiler/test_mutation_hold_live_2517.cpp` (#2517) [domain_suite, theme_compiler] — AC1: outermost enter/exit maintain live max probe
- `tests/compiler/test_mutation_hold_slo_2349.cpp` (#2349) [domain_suite, theme_compiler] — AC1: Production + hold > SLO → success_flag=false; violation counter
- `tests/core/test_mutation_log_cow_copy_2457.cpp` (#2457) [domain_suite, theme_core] — AC1: copy shares log sizes (no deep-copy isolation until write)
- `tests/compiler/test_mutation_log_pressure_2201.cpp` (#2201) [domain_suite, theme_compiler] — AC1: Stats report log size, compact totals, pressure flag/score
- `tests/compiler/test_mutation_log_query_race.cpp` (—) [domain_suite, theme_compiler] — test_mutation_log_query_race.cpp — Issue #1389:
- `tests/compiler/test_mutation_memory_blame_2196.cpp` (#2196) [domain_suite, theme_compiler] — AC1: Single EDSL query returns structured blame/memory for node
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
- `tests/orch/test_parallel_intend_pure_2163.cpp` (#2163) [domain_suite, theme_orch] — thunks; mutating thunks fail pure-contract; FailurePolicy still works.
- `tests/orch/test_parallel_intend_pure_contract_2230.cpp` (#2230) [large, domain_suite, theme_orch] — (pure_unlocked_applies / pure_fallback_locked / pure_contract_violated)
- `tests/core/test_param_annot_mutation_contract_2450.cpp` (#2450) [domain_suite, theme_core] — AC1: single-thread add_lambda with annotations coherent
- `tests/core/test_param_data_mutation_contract_2449.cpp` (#2449) [domain_suite, theme_core] — AC1: single-threaded add_lambda / set_lambda_params unchanged
- `tests/compiler/test_partial_cone_cap_2560.cpp` (#2560) [domain_suite, theme_compiler] — AC1: soft overflow metric + cap path source-cite (≤ soft or overflow)
- `tests/compiler/test_partial_cs_single_source_2262.cpp` (#2262) [domain_suite, theme_compiler] — AC1: N consecutive infer_flat_partial → import_total += N; solve sees roots
- `tests/compiler/test_partial_recompile_single_evict_2476.cpp` (#2476) [domain_suite, theme_compiler] — AC1: partial_recompile does not call invalidate(name)
- `tests/core/test_pcv_exclusive_with_set_2140.cpp` (#2140) [domain_suite, theme_core] — AC1: with_set exclusive → no alloc (same storage, with_set_exclusive metric)
- `tests/compiler/test_per_symbol_dirty_cycle_guard.cpp` (—) [domain_suite, theme_compiler] — Issue #1786 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_per_symbol_dirty_pool_lock.cpp` (—) [domain_suite, theme_core] — Issue #1785 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_persistent_typechecker_2220.cpp` (#2220) [domain_suite, theme_compiler] — AC1: N post-mutate typechecks reuse one TypeChecker (pointer stable)
- `tests/compiler/test_post_mutate_push_cascade.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2038; push_post_mutate_incremental_cascade +
- `tests/compiler/test_post_steal_linear_revalidate_2197.cpp` (#2197) [domain_suite, theme_compiler] — AC1: Steal under Strict + incomplete linear provenance → hard fail /
- `tests/compiler/test_predicate_memo_boundary_selective_2104.cpp` (#2104) [domain_suite, theme_compiler] — AC1: mutate binding used in one predicate → only that memo entry drops
- `tests/compiler/test_production_readiness_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_production_readiness_batch.cpp
- `tests/compiler/test_production_security_defaults_2053.cpp` (#2053) [domain_suite, theme_compiler] — AC1: apply_production_security_defaults → Restricted (unset AURA_SANDBOX)
- `tests/compiler/test_provenance_blame_hygiene.cpp` (—) [domain_suite, theme_compiler] — Issue #1877 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_query_and_replace_batch_2527.cpp` (#2527) [batch_driver, domain_suite, theme_compiler] — Issue #2527: mutate:query-and-replace-batch — first-class sugar
- `tests/compiler/test_query_by_marker_provenance_2242.cpp` (#2242) [domain_suite, theme_compiler] — AC1: all 3 individual primitives are registered and return schema=2242
- `tests/compiler/test_query_mutate_consistency.cpp` (—) [domain_suite, theme_compiler] — test_query_mutate_consistency.cpp — Issue #1374:
- `tests/compiler/test_reemit_mutation_boundary_handshake_2114.cpp` (#2114) [domain_suite, theme_compiler] — Handshake policy for Agent / plugin authors (AC5 / #2205):
- `tests/compiler/test_require_effect_auto_isolation_2490.cpp` (#2490) [domain_suite, theme_compiler] — AC1: Restricted + tenant principal unset + require_effect(Mutate) →
- `tests/compiler/test_require_effect_live_mid_2384.cpp` (#2384) [domain_suite, theme_compiler] — AC1: Grant Mutate bound_mutation_id=M; require_effect outside → deny
- `tests/core/test_reset_slot_parent_edges_2412.cpp` (#2412) [domain_suite, theme_core] — AC1: edges empty after every reset, even when index is dirty
- `tests/compiler/test_residual_gc_defer_assert_2211.cpp` (#2211) [large, domain_suite, theme_compiler] — AC1: Success path of outermost exit leaves defer_reasons_snapshot()==0
- `tests/core/test_restamp_sla_observability_2528.cpp` (#2528) [obs_named, domain_suite, theme_core] — AC1: After forced wrap, query surface reports restamp-us / nodes /
- `tests/core/test_restricted_unset_principal_2385.cpp` (#2385) [domain_suite, theme_core] — AC1: Restricted + tenant=0 + Mutate side-effect → deny + IsolationDeny
- `tests/core/test_sandbox_mode_atomic_2427.cpp` (#2427) [domain_suite, theme_core] — AC1: sandbox_mode is atomic-backed (AtomicEffectSandboxMode)
- `tests/compiler/test_security_audit_fold_2388.cpp` (#2388) [domain_suite, theme_compiler] — AC1: >128 effect denies under enabled SecurityEvent WAL → after
- `tests/compiler/test_security_audit_trail_2075.cpp` (#2075) [domain_suite, theme_compiler] — Issue #2075 — unified SecurityEvent schema + default-on mutation/effect audit WAL.
- `tests/compiler/test_security_audit_unify_2054.cpp` (#2054) [domain_suite, theme_compiler] — AC1: check_and_record_effect allow + deny both append SecurityEvent
- `tests/compiler/test_security_audit_wal_force_restricted_2492.cpp` (#2492) [domain_suite, theme_compiler] — AC1: Fresh process, default Restricted, no multi-tenant env → Security
- `tests/compiler/test_setcode_rebind_survive_2569.cpp` (#2569) [domain_suite, theme_compiler] — closures or hash telemetry (Aether closed-loop agent state).
- `tests/compiler/test_shape_jit_pass_deopt_incremental_closedloop_ai_mutate.cpp` (—) [domain_suite, theme_compiler] — test_shape_jit_pass_deopt_incremental_closedloop_ai_mutate.cpp — Issue #744:
- `tests/compiler/test_side_effect_inherit_2057.cpp` (#2057) [domain_suite, theme_compiler] — AC1: PrimMeta carries required_effects / effect_enforced_in_body / security_exempt
- `tests/compiler/test_soa_cascade_instr_dirty_sync.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2034; force_soa_instruction_dirty_sync +
- `tests/compiler/test_soa_dirty_aware_pipeline_2143.cpp` (#2143) [domain_suite, theme_compiler] — AC1: SoaDirtyAwarePass / DirtyAwarePass concepts compile; negative fails
- `tests/compiler/test_soa_single_entry_dirty_sync_2139.cpp` (#2139) [domain_suite, theme_compiler] — AC1: production sites use finish_cascade / finish_dirty_sync; no bare
- `tests/compiler/test_source_to_ir_map_consistency_2045.cpp` (#2045) [domain_suite, theme_compiler] — AC1: source cites #2045; rebuild_or_patch + pure helpers + consistency
- `tests/compiler/test_stable_ref_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_stable_ref_batch.cpp
- `tests/compiler/test_stable_ref_cow_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — Issue #1912 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_stable_ref_cow_refresh_failclosed_2393.cpp` (#2393) [domain_suite, theme_compiler] — AC1: unpinned cow_epoch mismatch → refresh_if_stale returns false;
- `tests/compiler/test_stable_ref_export_validate_2404.cpp` (#2404) [domain_suite, theme_compiler] — AC1: Agent export sites (export_ref, query:stable-ref, query:ensure-ref,
- `tests/compiler/test_stable_ref_pin_lifecycle_2189.cpp` (#2189) [domain_suite, theme_compiler] — AC1: EDSL pin-stable-refs / unpin-stable-refs / with-pinned registered
- `tests/compiler/test_stable_ref_provenance_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_stable_ref_provenance_batch.cpp — test_stable_ref 3-merge (R19 phase 20).
- `tests/serve/test_stable_ref_provenance_fiber_cow.cpp` (—) [domain_suite, theme_serve] — test_stable_ref_provenance_fiber_cow.cpp — Merged #457/#497/#527/#540/#549 + #551/#552 (#1978).
- `tests/core/test_stable_ref_tenant_capture_2125.cpp` (#2125) [domain_suite, theme_core] — AC1: Source cites #2125; make_ref stamps when isolation principal active
- `tests/compiler/test_stable_ref_tenant_mandate_2056.cpp` (#2056) [domain_suite, theme_compiler] — AC1: make_stamped_ref / stamp_stable_ref set tenant_id from principal
- `tests/core/test_stable_ref_wire_endian_2395.cpp` (#2395) [domain_suite, theme_core] — AC1: round-trip serialize → deserialize recovers full ref
- `tests/compiler/test_stable_ref_wire_v2_2198.cpp` (#2198) [domain_suite, theme_compiler] — AC1: v2 round-trips tenant_id, fiber_id, boundary_pinned,
- `tests/core/test_stale_ref_string_heap.cpp` (—) [domain_suite, theme_core] — Issue #1681 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_stringpool_bytes_total_lock_2408.cpp` (#2408) [domain_suite, theme_core] — AC1: 4 threads concurrent intern + string_bytes_total (no crash; TSan clean)
- `tests/core/test_subtree_dirty_bounds_2424.cpp` (#2424) [domain_suite, theme_core] — AC1: no OOB on dirty_ (bounds use dirty_.size() only)
- `tests/core/test_summary_recompute_sym_2414.cpp` (#2414) [domain_suite, theme_core] — AC1: recompute(pool) sets keyword + query:/mutate: bits
- `tests/compiler/test_type_dirty_cone_dep_graph_2191.cpp` (#2191) [domain_suite, theme_compiler] — AC1: Mutate callee B → type cone of callers + IR cascade share
- `tests/compiler/test_type_dirty_txn_order_2516.cpp` (#2516) [domain_suite, theme_compiler] — AC1: Source-cite single ordered sequence on all production partial paths
- `tests/compiler/test_type_system_health_2350.cpp` (#2350) [domain_suite, theme_compiler] — AC1: Score definition (header + pure compute)
- `tests/compiler/test_type_timeout_repair_2284.cpp` (#2284) [domain_suite, theme_compiler] — test_type_timeout_repair_2284.cpp
- `tests/compiler/test_typed_mutation_audit_decision_2281.cpp` (#2281) [domain_suite, theme_compiler] — (≥12 cells per AC4) + the query schema sentinels (AC2/AC3).
- `tests/compiler/test_typesystem_solve_delta_occurrence_priority_heavy_mutate.cpp` (—) [domain_suite, theme_compiler] — test_typesystem_solve_delta_occurrence_priority_heavy_mutate.cpp — Issue #745:
- `tests/compiler/test_typesystem_type_propagation_jit_l2_typed_mutate.cpp` (—) [domain_suite, theme_compiler] — test_typesystem_type_propagation_jit_l2_typed_mutate.cpp — Issue #746:
- `tests/compiler/test_typesystem_typed_mutate_incremental_gaps.cpp` (—) [domain_suite, theme_compiler] — test_typesystem_typed_mutate_incremental_gaps.cpp — Issue #659:
- `tests/compiler/test_unify_invalidate_try_acquire.cpp` (—) [domain_suite, theme_compiler] — Issue #1476/#1547/#1628/#1634 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_value_encoding_v2_dispatch_contracts.cpp` (—) [domain_suite, theme_compiler] — Issue #1622/#571/#723 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_verification_dirty_bits_lock_2439.cpp` (#2439) [domain_suite, theme_core] — AC1: concurrent apply_verification_dirty_bits(same_id, same_reasons) → +1 metric
- `tests/compiler/test_walk_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_walk_batch.cpp
- `tests/core/test_workspace_isolation_wire_2073.cpp` (#2073) [domain_suite, theme_core] — Issue #2073 — wire check_workspace_isolation + stamp_ref_tenant on
- `tests/core/test_workspace_lock_reentrancy.cpp` (—) [domain_suite, theme_core] — test_wave1_workspace_lock_reentrancy.cpp — Wave1 B-03 / B-09
- `tests/compiler/test_workspace_region_concurrency_2121.cpp` (#2121) [domain_suite, theme_compiler] — AC1: source cites #2121 + documents region strategy
- `tests/core/test_workspace_state_lock.cpp` (—) [domain_suite, theme_core] — tests/core/test_workspace_state_lock.cpp — Issue #1994 (F-004):` (workspace-state)` and

### `fiber_orch` — Fiber / orchestration / steal / Guard (90)

**Target:** tests/core/test_fiber_resume_batch (domain/ pilot abandoned in R1)

**Priority:** P1 — domain suite already collapses many obs gates

#### domain/ (90)

- `tests/orch/test_agent_apply_mutex_2158.cpp` (#2158) [domain_suite, theme_orch] — AC1: No process-static mutex on orch spawn apply path (grep clean).
- `tests/orch/test_agent_ask_typed_corr_2538.cpp` (#2538) [domain_suite, theme_orch] — AC1: corr_id match without payload text parse (MailKind + correlation_id)
- `tests/orch/test_agent_failure_policy_2229.cpp` (#2229) [domain_suite, theme_orch] — AC1: AgentFailurePolicy available under aura::orch; StallPolicy
- `tests/orch/test_agent_max_no_yield_2540.cpp` (#2540) [domain_suite, theme_orch] — Issue #2585 — production default + opt-out (AURA_AGENT_MAX_NO_YIELD_MS=0).
- `tests/orch/test_agent_scope_hierarchy_2537.cpp` (#2537) [domain_suite, theme_orch] — AC1: parent / children links via spawn_child (unique_ptr, not static table)
- `tests/compiler/test_aot_bridge_checkpoint_version_steal.cpp` (—) [domain_suite, theme_compiler] — test_aot_bridge_checkpoint_version_steal.cpp — Issue #653:
- `tests/serve/test_boundary_yield_steal_metrics_2119.cpp` (#2119) [domain_suite, theme_serve] — AC1: high-frequency MB yield → yield_mutation_boundary_total + hold_ns
- `tests/compiler/test_capability_high_risk_promote_2489.cpp` (#2489) [domain_suite, theme_compiler] — AC1: Registry-only grant self-evo → has_capability true without relying
- `tests/serve/test_chaos_mutate_steal_gc_mailbox_2352.cpp` (#2352) [large, domain_suite, theme_serve] — Issue #2380 — nightly production-concurrency profile: lock-order canary +
- `tests/serve/test_chaos_steal_mutation_gc.cpp` (—) [domain_suite, theme_serve] — test_chaos_steal_mutation_gc_2315.cpp — Issue #2315:
- `tests/compiler/test_compile_primitive_guard.cpp` (—) [domain_suite, theme_compiler] — Issue #1896 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_concurrent.cpp` (—) [large, domain_suite, theme_serve] — test_concurrent.cpp — Concurrency model unit tests
- `tests/compiler/test_edsl_concurrent_fiber_boundary_task1.cpp` (—) [domain_suite, theme_compiler] — test_edsl_concurrent_fiber_boundary_task1.cpp —
- `tests/compiler/test_edsl_concurrent_query_mutate.cpp` (—) [domain_suite, theme_compiler] — test_edsl_concurrent_query_mutate.cpp — Issue #332
- `tests/compiler/test_env_lookup_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_env_lookup_batch.cpp — batch driver for Env::lookup family.
- `tests/compiler/test_envframe_ownership_steal_densify_2362.cpp` (#2362) [domain_suite, theme_compiler] — AC1: Live set — register/inject populates live_env_frame_refs();
- `tests/compiler/test_escape_gate_steal_densify_clear_2507.cpp` (#2507) [domain_suite, theme_compiler] — AC1: Publish escape block under key K → clear_for_eval → re-lower →
- `tests/serve/test_fiber_concurrent_unit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — test_fiber_concurrent_unit_batch.cpp — light concurrent units
- `tests/serve/test_fiber_integration_batch.cpp` (—) [batch_driver, domain_suite, theme_serve] — tests/serve/test_fiber_integration_batch.cpp — closure-bridge Cycle-4 integration (Issue #226).
- `tests/serve/test_fiber_migration_refresh_2194.cpp` (#2194) [domain_suite, theme_serve] — AC1: Every resume after cross-worker steal runs
- `tests/serve/test_fiber_mutation_steal_safety.cpp` (—) [domain_suite, theme_serve] — test_fiber_mutation_steal_safety.cpp — Issue #542:
- `tests/orch/test_fiber_native_keepalive_2159.cpp` (#2159) [domain_suite, theme_orch] — AC1: Default keepalive_interval_ms=0 remains zero-cost (no helper fiber).
- `tests/serve/test_fiber_orch_core_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — test_fiber_orch_core_batch.cpp — consolidated fiber-theme drivers
- `tests/serve/test_fiber_orch_parallel_quota_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — test_fiber_orch_parallel_quota_batch.cpp — consolidated fiber-theme drivers
- `tests/serve/test_fiber_reclaim_orphan_release_2498.cpp` (#2498) [domain_suite, theme_serve] — AC1: Non-yielding body + hard reclaim → Fiber::release_orphan_roots()
- `tests/serve/test_fiber_reclaim_safety.cpp` (—) [domain_suite, theme_serve] — AC1: Fiber::is_done() now strictly requires state_==Done
- `tests/compiler/test_fiber_resume_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_fiber_resume_batch.cpp — batch driver for fiber resume post-steal family.
- `tests/serve/test_fiber_resume_state.cpp` (—) [domain_suite, theme_serve] — AC1: Fiber::resume() returns early if state_ == Done
- `tests/serve/test_fiber_steal_panic_checkpoint_nested_gc.cpp` (—) [small, domain_suite, theme_serve] — tests/test_fiber_steal_panic_checkpoint_nested_gc.cpp — Issue #1446
- `tests/serve/test_fiber_strategy_evolve_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — test_fiber_strategy_evolve_batch.cpp — consolidated fiber-theme drivers
- `tests/serve/test_fiber_synthesize_batch.cpp` (—) [batch_driver, domain_suite, theme_serve] — test_fiber_synthesize_batch.cpp — consolidated fiber-theme drivers
- `tests/serve/test_guard_panic_reflect_fiber_resume_task6.cpp` (—) [domain_suite, theme_serve] — test_guard_panic_reflect_fiber_resume_task6.cpp — Issue #596:
- `tests/compiler/test_hard_fiber_isolation_2151.cpp` (#2151) [domain_suite, theme_compiler] — AC1: hard_fiber_isolation=false → fiber mismatch allow + metric only
- `tests/compiler/test_hard_fiber_restricted_2536.cpp` (#2536) [domain_suite, theme_compiler] — AC1: Restricted default soft — fiber A grant, fiber B allow + mismatch metric
- `tests/serve/test_inner_steal_starvation.cpp` (—) [domain_suite, theme_serve] — Issue #1445/#1492/#1633 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_is_stealable_snapshot_gate_2549.cpp` (#2549) [domain_suite, theme_serve] — AC1: is_stealable() false when depth>0 or held under MutationBoundary
- `tests/serve/test_issue_1992.cpp` (#1992) [domain_suite, theme_serve] — test_issue_1992.cpp — Issue #1992 (C-001): Fiber::mutation_stack_storage_
- `tests/compiler/test_issues_809_817_batch.cpp` (#809) [batch_driver, domain_suite, theme_compiler] — test_issues_809_817_batch.cpp — Phase 1 close for Issues #809–#817.
- `tests/orch/test_join_drain_reclaim_2227.cpp` (#2227) [domain_suite, theme_orch] — AC1: residual + reclaim counters bump when non-yielding body +
- `tests/serve/test_join_drain_timeout_2153.cpp` (#2153) [domain_suite, theme_serve] — AC1: Default drain_ms=2000 preserves #2082 (Ok path / yielding body)
- `tests/compiler/test_lock_order_audit_2354.cpp` (#2354) [domain_suite, theme_compiler] — AC1: Audit off → zero atomics on acquire (TLS depth still tracked —
- `tests/compiler/test_lock_order_closures_env.cpp` (—) [domain_suite, theme_compiler] — Issue #1664 (#1978 renamed): issue# moved from filename to header.
- `tests/orch/test_mailbox_bp_admit_default_2535.cpp` (#2535) [domain_suite, theme_orch] — AC1: no env → resolve_mailbox_bp_admit_threshold() == 32
- `tests/serve/test_mailbox_hold_exit_drain_2511.cpp` (#2511) [domain_suite, theme_serve] — AC1: outermost Guard exit calls drain_deferred_under_budget (source-cite)
- `tests/serve/test_mailbox_hold_starvation_hard_2551.cpp` (#2551) [domain_suite, theme_serve] — AC1: Production/Strict + residual after budget → hard counter + flag
- `tests/serve/test_mailbox_recv_mutation_boundary_2188.cpp` (#2188) [large, domain_suite, theme_serve] — Issue #2347 — Strict/hard audit + optional Guard-window threshold
- `tests/serve/test_mailbox_tenant_principal_2592.cpp` (#2592) [domain_suite, theme_serve] — tests/serve/test_mailbox_tenant_principal_2592.cpp
- `tests/serve/test_mutation_safety_snapshot_steal_2184.cpp` (#2184) [large, domain_suite, theme_serve] — AC1: mutation_safety_snapshot used by is_at_mutation_boundary_safe +
- `tests/orch/test_orch_admission_decay.cpp` (—) [domain_suite, theme_orch] — AC1: decay window — BP event → counter > 0 → spawn denied
- `tests/serve/test_orch_agent_mutation_boundary_2118.cpp` (#2118) [domain_suite, theme_serve] — AC1: soft-boundary agent body → depth>0, is_at_mutation_boundary_safe false
- `tests/orch/test_orch_scope_2588.cpp` (#2588) [domain_suite, theme_orch] — tests/orch/test_orch_scope_2588.cpp
- `tests/serve/test_orch_soft_boundary_unified_2515.cpp` (#2515) [domain_suite, theme_serve] — AC1: soft 进入/退出必 publish mirrors（source-cite）—
- `tests/serve/test_orchestration_steal_boost.cpp` (—) [domain_suite, theme_serve] — tests/test_orchestration_steal_boost.cpp — Issue #1445 / #1492
- `tests/serve/test_orphan_reap_stress.cpp` (—) [domain_suite, theme_serve] — AC1: orphan_mutex_ held for minimal time (just iterate + decide
- `tests/serve/test_panic_checkpoint_fiber_resume_safety.cpp` (—) [domain_suite, theme_serve] — test_panic_checkpoint_fiber_resume_safety.cpp — Issue #592:
- `tests/compiler/test_pcv_children_safe_default_migration.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2036; children_ is PersistentChildVector; children_default
- `tests/core/test_pcv_tls_default_on_2521.cpp` (#2521) [domain_suite, theme_core] — AC1: Production default enables TLS; AURA_PCV_TLS=0 / test override off
- `tests/core/test_pcv_tls_scratch_2406.cpp` (#2406) [domain_suite, theme_core] — AC1: Soft / default: behavior identical (TLS off → same cow_alloc path)
- `tests/compiler/test_per_defuse_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_per_defuse_batch.cpp — batch driver for per_defuse_index family.
- `tests/serve/test_per_fiber_stack_pool_high_concurrency.cpp` (—) [domain_suite, theme_serve] — test_per_fiber_stack_pool_high_concurrency.cpp — Issue #652:
- `tests/orch/test_per_scope_bp_admit_2591.cpp` (#2591) [domain_suite, theme_orch] — tests/orch/test_per_scope_bp_admit_2591.cpp
- `tests/compiler/test_post_densify_linear_type_revalidate_2353.cpp` (#2353) [domain_suite, theme_compiler] — AC1: Ordered phase helper runs after Moving densify (or stamp-mismatch
- `tests/serve/test_prompt6_epoch_atomic_visibility_fiber_steal.cpp` (—) [domain_suite, theme_serve] — test_prompt6_epoch_atomic_visibility_fiber_steal.cpp — Issue #739:
- `tests/compiler/test_prompt6_full_memory_safety_fuzz_stress.cpp` (—) [domain_suite, theme_compiler] — test_prompt6_full_memory_safety_fuzz_stress.cpp — Issue #602:
- `tests/compiler/test_propagate_marker_cycle_guard.cpp` (—) [domain_suite, theme_compiler] — Issue #1679/#1682/#1782 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_residual_defer_steal_hard_and_2546.cpp` (#2546) [domain_suite, theme_serve] — AC1: Hard + residual non-zero after clear → fiber Cancel+Done; hard-fail +1
- `tests/serve/test_residual_force_safepoint_2533.cpp` (#2533) [small, domain_suite, theme_serve] — AC1: mark_reclaimed requests force_safepoint + cancel
- `tests/core/test_restore_children_structural_lock_2455.cpp` (#2455) [domain_suite, theme_core] — AC1: restore_children without external guard restores correctly (self-locks)
- `tests/compiler/test_runtime_concurrent_full_cycle_chaos.cpp` (—) [domain_suite, theme_compiler] — test_runtime_concurrent_full_cycle_chaos.cpp — Issue #755:
- `tests/serve/test_runtime_mutation_boundary_steal_safety.cpp` (—) [domain_suite, theme_serve] — test_runtime_mutation_boundary_steal_safety.cpp — Issue #588:
- `tests/serve/test_safe_yield_orchestration.cpp` (—) [domain_suite, theme_serve] — Issue #1504/#1591/#1635 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_safepoint_mutation.cpp` (—) [domain_suite, theme_serve] — test_safepoint_mutation.cpp — Issue #1364: safepoint × mutation telemetry
- `tests/serve/test_scheduler_gc_defer_pending_panic_steal.cpp` (—) [large, domain_suite, theme_serve] — AC1: pending checkpoint → GCCollector::request deferred; collect skips
- `tests/serve/test_scheduler_gc_safepoint_mutation_coordination.cpp` (—) [domain_suite, theme_serve] — test_scheduler_gc_safepoint_mutation_coordination.cpp —
- `tests/serve/test_scheduler_llm_bottleneck_adaptive_steal_gc.cpp` (—) [domain_suite, theme_serve] — test_scheduler_llm_bottleneck_adaptive_steal_gc.cpp — Issue #754:
- `tests/orch/test_security_schedule_gate_2590.cpp` (#2590) [domain_suite, theme_orch] — tests/orch/test_security_schedule_gate_2590.cpp
- `tests/serve/test_steal_complete_gc_defer_2203.cpp` (#2203) [domain_suite, theme_serve] — AC1: try_steal_from success always invokes aura_evaluator_on_steal_complete
- `tests/serve/test_steal_complete_restamp_txn_2510.cpp` (#2510) [domain_suite, theme_serve] — AC1: on_steal_complete is the sole restamp entry (source-cite + gate)
- `tests/serve/test_steal_complete_strong_entry_2377.cpp` (#2377) [domain_suite, theme_serve] — legacy residual-less path under production).
- `tests/serve/test_steal_layout_stamp_2351.cpp` (#2351) [domain_suite, theme_serve] — AC1: Steal with matching stamp → no mismatch bump
- `tests/serve/test_steal_safety_ticket_2518.cpp` (#2518) [domain_suite, theme_serve] — AC1: snapshot carries ticket; resume mismatch → hard-fail
- `tests/serve/test_steal_snapshot_hard_invariant_2346.cpp` (#2346) [domain_suite, theme_serve] — (fail-closed canary). Soft: mismatch metric only. Hard: mark-failed.
- `tests/serve/test_steal_snapshot_soft_production_lock_2372.cpp` (#2372) [domain_suite, theme_serve] — + require force-deopt ABI under production Soft lock.
- `tests/core/test_stress_alloc_storage_lock.cpp` (—) [domain_suite, theme_core] — test_stress_alloc_storage_lock.cpp — Issue #1397
- `tests/core/test_structural_metadata_lock_order_2418.cpp` (#2418) [domain_suite, theme_core] — AC1: documented order + CombinedStructuralMetadataWriteGuard
- `tests/compiler/test_tenant_scope_fiber_mandate_2491.cpp` (#2491) [domain_suite, theme_compiler] — AC1: Fiber with assigned_tenant_id=42 → body capability_tenant_id()
- `tests/core/test_transaction_guard_2555.cpp` (#2555) [domain_suite, theme_core] — AC1: Scaffold simulation removed; host try_acquire/release required
- `tests/compiler/test_type_freshness_steal_densify_2552.cpp` (#2552) [domain_suite, theme_compiler] — AC1: Steal success + old-epoch goals → stale_vs_epoch(new)==0 after fence
- `tests/compiler/test_workspace_swap_guard.cpp` (—) [domain_suite, theme_compiler] — tests/compiler/test_workspace_swap_guard.cpp — Issue #1717: synthesize:optimize swap-guard test.
- `tests/serve/test_yield_while_mutation_held_2200.cpp` (#2200) [domain_suite, theme_serve] — AC1: Under live outermost Guard, yield() / yield(reason) do not

### `linear_ownership` — Linear ownership / borrow / consume (19)

**Target:** tests/compiler/test_linear_ownership_batch.cpp (R1 src/-aligned)

**Priority:** P1 — small, already partially batched

#### domain/ (19)

- `tests/core/test_capability_single_use_consume_2586.cpp` (#2586) [domain_suite, theme_core] — tests/core/test_capability_single_use_consume_2586.cpp
- `tests/compiler/test_commit_readiness_score_2553.cpp` (#2553) [domain_suite, theme_compiler] — AC1: Clean SOLVED + linear + blame + !trunc → bp=10000, ok, allow
- `tests/compiler/test_compiler_service_ownership.cpp` (—) [small, domain_suite, theme_compiler] — Issue #1835/#1837/#1839 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_densify_last_call_axes_2376.cpp` (#2376) [domain_suite, theme_compiler] — for envframe + closure remount axes (seal #2361/#2365 last-call contract).
- `tests/compiler/test_densify_ownership_scan_fail_gate_2497.cpp` (#2497) [domain_suite, theme_compiler] — metrics the same way pin_contract_held does — no path where scan fail is
- `tests/compiler/test_hardware_resource_linear_ownership.cpp` (—) [domain_suite, theme_compiler] — test_hardware_resource_linear_ownership.cpp — Issue #306:
- `tests/compiler/test_lifetime_contract_snapshot_2300.cpp` (#2300) [domain_suite, theme_compiler] — for pin / linear / EnvFrame / GC-defer / residual contract.
- `tests/compiler/test_linear_cross_closure_escape_2563.cpp` (#2563) [domain_suite, theme_compiler] — AC1: dirty linear free-captured by Lambda → discovered; hard forces
- `tests/compiler/test_linear_escape_commit_hardblock_2108.cpp` (#2108) [domain_suite, theme_compiler] — AC1: Cross-batch escape → commit fails; blocked + escape counters
- `tests/compiler/test_linear_ownership_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_linear_ownership_batch.cpp
- `tests/compiler/test_linear_ownership_occurrence_predicate_mutate.cpp` (—) [domain_suite, theme_compiler] — test_linear_ownership_occurrence_predicate_mutate.cpp — Issue #747:
- `tests/compiler/test_linear_partial_revalidate_2460.cpp` (#2460) [domain_suite, theme_compiler] — AC1: Dirty linear ownership fail path surfaces during partial under
- `tests/compiler/test_linear_pin_moving_compact_2280.cpp` (#2280) [domain_suite, theme_compiler] — linear pin contract). Issue #2293 — AOT JIT path registers linear
- `tests/compiler/test_linear_runtime_violation.cpp` (—) [small, domain_suite, theme_compiler] — Issue #2067 — Linear Ownership runtime enforcement test.
- `tests/compiler/test_linear_state_stamp_apply_2129.cpp` (#2129) [domain_suite, theme_compiler] — AC1: mangle_aot_name stamps _lN when linear_state != 0 (host tracks)
- `tests/compiler/test_linear_synth_violation_2357.cpp` (#2357) [domain_suite, theme_compiler] — AC1: Double-move / can_move fail path is first-class (note_linear_synth
- `tests/compiler/test_linear_three_layer_wire_2559.cpp` (#2559) [domain_suite, theme_compiler] — AC1: Boundary exit cites force_linear_rollback (unified linear deny)
- `tests/core/test_pcv_unique_hotpath_2058.cpp` (#2058) [domain_suite, theme_core] — AC1: cow_set unique → in-place (no new storage, use_count stays 1)
- `tests/core/test_type_registry_ownership.cpp` (—) [small, domain_suite, theme_core] — Issue #1835/#1837 (#1978 renamed): issue# moved from filename to header.

### `edsl_hygiene` — EDSL / macro hygiene / reflect (41)

**Target:** tests/core/test_macro_reflect_batch (domain/ pilot abandoned in R1)

**Priority:** P1 — domain hygiene suite exists

#### domain/ (41)

- `tests/reflect/test_ast_pod_reflect_b3.cpp` (—) [domain_suite, theme_reflect] — Wave B3: small AST public PODs via auto_serialize / to_json.
- `tests/reflect/test_cache_header_magic_a2.cpp` (—) [small, domain_suite, theme_reflect] — Wave A2: CacheHeader::magic[8] round-trips via auto_serialize;
- `tests/compiler/test_contracts.cpp` (—) [small, domain_suite, theme_compiler] — tests/compiler/test_contracts.cpp — Issue #83: C++26 contract_assert + trailing pre/post
- `tests/reflect/test_enum_name_table_c1.cpp` (—) [domain_suite, theme_reflect] — Wave C1: generic enum_name_table API across several domain enums.
- `tests/reflect/test_error_merr.cpp` (—) [small, domain_suite, theme_reflect] — test_error_merr.cpp — Pilot for centralized make_merr (refactor Step 0.1+)
- `tests/reflect/test_flat_instr_reflect_b2.cpp` (—) [domain_suite, theme_reflect] — Wave B2: FlatInstruction auto_serialize round-trip + IR field overlap.
- `tests/reflect/test_hygiene_diagnostic_2167.cpp` (#2167) [domain_suite, theme_reflect] — AC1: (query:hygiene-diagnostic node-id) returns structured hash schema 2167
- `tests/reflect/test_ir_cache_v2.cpp` (—) [small, domain_suite, theme_reflect] — tests/test_ir_cache_v2.cpp
- `tests/reflect/test_ir_pod_phase4_2291.cpp` (#2291) [phase_slice, domain_suite, theme_reflect] — Issue #2291 — Phase 4 kickoff: pure-POD IR types via reflection.
- `tests/reflect/test_issue_178.cpp` (#178) [small, early_issue, domain_suite, theme_reflect] — test_issue_178.cpp — Issue #178 / #268: production NodeView
- `tests/reflect/test_issue_178_reflect.cpp` (#178) [early_issue, domain_suite, theme_reflect] — Non-module TU: P2996 reflection (Issue #268).
- `tests/compiler/test_macro_cross_flat_hygiene_2235.cpp` (#2235) [domain_suite, theme_compiler] — warning. Single-flat path stays zero-overhead (AC4 contract
- `tests/compiler/test_macro_fiber_hygiene.cpp` (—) [large, domain_suite, theme_compiler] — AC1: source cites #2097 + FiberHygieneStats + get_fiber_hygiene_metrics
- `tests/compiler/test_macro_hygiene_closedloop_health.cpp` (—) [domain_suite, theme_compiler] — Issue #1501/#1589/#1593/#1609/#1613 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_macro_hygiene_depth_concurrent_obs.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2021; peak / in-flight atomics + snapshot helper
- `tests/compiler/test_macro_hygiene_fiber_panic_aot_soa_self_evo.cpp` (—) [domain_suite, theme_compiler] — test_macro_hygiene_fiber_panic_aot_soa_self_evo.cpp — Issue #654:
- `tests/compiler/test_macro_hygiene_limits_2101.cpp` (#2101) [domain_suite, theme_compiler] — AC1: lower runtime cap → expand at depth=cap+1 clamps/NULL_NODE + metric;
- `tests/compiler/test_macro_reflect_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_macro_reflect_batch.cpp — batch driver for macro+reflect+self-evo family.
- `tests/compiler/test_macro_restamp_after_flat.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2019 + restamp_macro_introduced_generations
- `tests/compiler/test_macro_self_evo_capability.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2023; MacroSelfEvoPolicy + check_macro_self_evo
- `tests/core/test_node_meta_gap_2411.cpp` (#2411) [domain_suite, theme_core] — AC1: gap entry tag is 0x0C sentinel and is_gap == true
- `tests/reflect/test_node_tag_align_b1.cpp` (—) [small, domain_suite, theme_reflect] — Wave B1: NodeTag P2996 identifiers ↔ kNodeTagNames alignment.
- `tests/reflect/test_opcode_info_align_a3.cpp` (—) [small, domain_suite, theme_reflect] — Wave A3: IROpcode PascalCase (P2996) ↔ display kebab table alignment.
- `tests/reflect/test_opcode_reflect_2289.cpp` (#2289) [domain_suite, theme_reflect] — Issue #2289: GCC 16.1 reflection workaround cleanup.
- `tests/compiler/test_prompt2_6_impact_scope_quote_lambda_bridge_env.cpp` (—) [domain_suite, theme_compiler] — test_prompt2_6_impact_scope_quote_lambda_bridge_env.cpp — Issue #741:
- `tests/compiler/test_query_hygiene_default_2525.cpp` (#2525) [domain_suite, theme_compiler] — AC1: Default query:pattern / query:filter skip MacroIntroduced; include
- `tests/compiler/test_query_pattern_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_query_pattern_batch.cpp — query_pattern pair dup-merge (R19 phase 13).
- `tests/compiler/test_query_pattern_default_hygiene_2123.cpp` (#2123) [domain_suite, theme_compiler] — MacroIntroduced linkage (production "code as memory" contract).
- `tests/compiler/test_query_pattern_hygiene_macrointroduced.cpp` (—) [domain_suite, theme_compiler] — test_query_pattern_hygiene_macrointroduced.cpp — Issue #593:
- `tests/compiler/test_reflect_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_reflect_batch.cpp
- `tests/reflect/test_reflect_hygiene_agent_diagnostics.cpp` (—) [domain_suite, theme_reflect] — reflect:provenance-blame for expand → diagnose → mutate closed loops.
- `tests/reflect/test_reflect_hygiene_unit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_reflect] — test_edsl_hygiene_unit_batch.cpp — consolidated edsl hygiene drivers
- `tests/reflect/test_reflect_isolation_2290.cpp` (#2290) [small, domain_suite, theme_reflect] — Issue #2290: P2996 placement smoke (g++ 16.1.0).
- `tests/reflect/test_reflect_macro_hygiene_batch.cpp` (—) [large, batch_driver, domain_suite, theme_reflect] — test_edsl_macro_hygiene_batch.cpp — consolidated edsl hygiene drivers
- `tests/reflect/test_reflect_pattern_hygiene_batch.cpp` (—) [large, batch_driver, domain_suite, theme_reflect] — test_edsl_pattern_hygiene_batch.cpp — consolidated edsl hygiene drivers
- `tests/compiler/test_rest_param_hygiene_2169.cpp` (#2169) [domain_suite, theme_compiler] — AC1: source cites #2169; always gensym rest; process serial
- `tests/compiler/test_rest_param_hygiene_self_evo.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2018; rest pre-scan + dotted preserve + metric
- `tests/compiler/test_rest_param_nested_qq_hygiene_2239.cpp` (#2239) [domain_suite, theme_compiler] — AC1: source cites #2239; counters + v_read accessors + helper +
- `tests/compiler/test_rollback_by_marker_2237.cpp` (#2237) [domain_suite, theme_compiler] — - AC1: existing primitives registered + callable
- `tests/compiler/test_static_reflect_selfmod_validation_task6.cpp` (—) [domain_suite, theme_compiler] — Issue #454/#551/#587/#594 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_symbol_eq_2568.cpp` (#2568) [domain_suite, theme_compiler] — AC1: (eq? 'commit 'commit) → #t  (interned short-str cache)

### `jit_incremental` — JIT / AOT / incremental relower (74)

**Target:** domain suite for incremental_*; keep heavy JIT in issue bundles

**Priority:** P2 — link-profile heavy; migrate AC smoke first

#### domain/ (74)

- `tests/compiler/test_adaptive_cascade_depth_partial_thr_2209.cpp` (#2209) [domain_suite, theme_compiler] — AC1: After enough samples, high cascade-depth raises the threshold.
- `tests/compiler/test_adaptive_partial_relower_threshold_2112.cpp` (#2112) [domain_suite, theme_compiler] — AC1: Cold-start stays at default 8 until enough samples
- `tests/compiler/test_aot_anonymous_closure_policy_2238.cpp` (#2238) [domain_suite, theme_compiler] — AC1: anonymous + aura_closure_check_aot_stable_id_policy under
- `tests/compiler/test_aot_hot_update_health_2506.cpp` (#2506) [domain_suite, theme_compiler] — AC1: Idle healthy → health-bp high, force-reason ok, recovery_active=0
- `tests/compiler/test_aot_incremental_reemit.cpp` (—) [large, domain_suite, theme_compiler] — Issue #1480/#1930/#1943/#1952/#2013 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_aot_jit_joint_versioning_2046.cpp` (#2046) [domain_suite, theme_compiler] — bridge_epoch (joint epoch contract after soft/hard invalidate).
- `tests/compiler/test_aot_mangle_top.cpp` (—) [domain_suite, theme_compiler] — test_aot_mangle_top.cpp — Issue #1369 / #2015:
- `tests/compiler/test_aot_region_per_eval.cpp` (—) [domain_suite, theme_compiler] — test_aot_region_per_eval.cpp — Issue #1367 (standalone; ACs drift under current aot: API)
- `tests/compiler/test_aot_reload_primitive.cpp` (—) [large, domain_suite, theme_compiler] — test_aot_reload_primitive.cpp — Issue #1366: (aot:reload) Aura wrappers
- `tests/compiler/test_aot_shell_c0_escape.cpp` (—) [domain_suite, theme_compiler] — test_issue_1997.cpp -- runtime smoke test for B-002 / #1997
- `tests/compiler/test_aot_version_triple_2306.cpp` (#2306) [domain_suite, theme_compiler] — Issue #2306 — (query:aot-version-triple) Agent-facing read-only
- `tests/compiler/test_aura_jit_unused_fn_lock_2475.cpp` (#2475) [domain_suite, theme_compiler] — AC1: fn_lock removed from compile()
- `tests/compiler/test_build_kv_hash_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_build_kv_hash_batch.cpp — build_kv_hash pair dup-merge (R19 phase 8).
- `tests/compiler/test_cache_stamp_restamp_contract_2183.cpp` (#2183) [domain_suite, theme_compiler] — AC1: restamp_cache_entry + restamp_cache_entry_live_ on store/partial
- `tests/compiler/test_capability_gating.cpp` (—) [domain_suite, theme_compiler] — Issue #1416: Inline no-op stubs for aura::jit::AuraJIT::invalidate_prefix
- `tests/compiler/test_cascade_incremental_pass_suite_2044.cpp` (#2044) [domain_suite, theme_compiler] — Issue #2044 — Pass pipeline fully incremental on cascade re-lower
- `tests/compiler/test_castop_density_hard_2358.cpp` (#2358) [domain_suite, theme_compiler] — AC1: HARD=0 + dens>budget → no hard_action, no force-JIT side effect
- `tests/compiler/test_closure_call_must_deopt_toctou_2472.cpp` (#2472) [domain_suite, theme_compiler] — AC1: multi-step free+realloc under concurrent MustDeopt callers —
- `tests/compiler/test_closure_cow_gen_stamp_2547.cpp` (#2547) [domain_suite, theme_compiler] — AC1: Alloc under gen G → soft-eligible while live gen == G
- `tests/compiler/test_compiler_core_incremental_selfmod_gaps.cpp` (—) [domain_suite, theme_compiler] — test_compiler_core_incremental_selfmod_gaps.cpp — Issue #657:
- `tests/compiler/test_concept_constraints.cpp` (—) [domain_suite, theme_compiler] — AC1: module exports all Pass-related concepts
- `tests/compiler/test_dead_coercion_pipeline_wire.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2025; PassKind::DeadCoercion + DeadCoercionPass +
- `tests/compiler/test_dep_graph_partial_relower_threshold.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2032; get/set_partial_relower_threshold; reject counter
- `tests/compiler/test_emit_object_deprecated_2477.cpp` (#2477) [domain_suite, theme_compiler] — AC1: emit_object returns false
- `tests/compiler/test_epoch_invariant_complete_2501.cpp` (#2501) [domain_suite, theme_compiler] — AC1: Soft inject stale AOT slot → violation count; hard clears fn_ptr
- `tests/compiler/test_force_jit_repromote_2502.cpp` (#2502) [domain_suite, theme_compiler] — AC1: force-JIT Defuse → N successful reemits, no storm → bit cleared
- `tests/compiler/test_hot_strategy_2582.cpp` (#2582) [domain_suite, theme_compiler] — AC1: hot-strategy:swap! rebinds named strategy; call sees new body
- `tests/compiler/test_hot_update_cascade_dirty_reemit.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2035; notify_hot_update_after_cascade_ +
- `tests/compiler/test_incremental_effectiveness_snapshot_fail.cpp` (—) [domain_suite, theme_compiler] — Issue #1669/#1854/#1856 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_incremental_perblock_closure_bridge_safety.cpp` (—) [domain_suite, theme_compiler] — test_incremental_perblock_closure_bridge_safety.cpp — Issue #600:
- `tests/compiler/test_incremental_relower_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_incremental_relower_batch.cpp — batch driver for incremental_relower family.
- `tests/core/test_incremental_restamp_2061.cpp` (#2061) [domain_suite, theme_core] — Issue #2061 — incremental restamp observability for generation wrap.
- `tests/compiler/test_incremental_soundness_oracle_2113.cpp` (#2113) [domain_suite, theme_compiler] — Enable docs (AC5):
- `tests/compiler/test_incremental_type_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_incremental_type_batch.cpp — batch driver for incremental_type family.
- `tests/compiler/test_inline_pass_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_inline_pass_batch.cpp — inline_pass pair dup-merge (R19 phase 14).
- `tests/compiler/test_instr_level_relower_pass_2133.cpp` (#2133) [domain_suite, theme_compiler] — AC1: has_instr_precision / instr_level_eligible under threshold
- `tests/compiler/test_jit_aot_hot_update_unit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_jit_aot_hot_update_batch.cpp — consolidated AOT hot-update + steal-boundary drivers
- `tests/compiler/test_jit_batch_deopt_clear.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_issue_1996.cpp — Issue #1996 (B-003): `g_batch_deopt_jit` raw
- `tests/compiler/test_jit_closure_cache_race.cpp` (—) [domain_suite, theme_compiler] — Issue #1707 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_jit_concurrent_compile.cpp` (—) [domain_suite, theme_compiler] — test_jit_concurrent_compile.cpp — Issue #114 concurrent compile stress
- `tests/compiler/test_jit_consistency.cpp` (—) [domain_suite, theme_compiler] — test_jit_consistency.cpp — Issue #427: JIT ↔ IRInterpreter
- `tests/compiler/test_jit_critical_coverage.cpp` (—) [domain_suite, theme_compiler] — Issue #1658/#1917 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_jit_dual_string_heap_2575.cpp` (#2575) [domain_suite, theme_compiler] — AC1: (display (string-append "A" "B")) → AB under default JIT
- `tests/compiler/test_jit_full_opcode_coverage.cpp` (—) [domain_suite, theme_compiler] — Issue #1289/#1512/#1658/#427/#532 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_jit_interpreter_equivalence_oracle_2210.cpp` (#2210) [domain_suite, theme_compiler] — AC1: Oracle is zero-cost when disabled (mode 0/2).
- `tests/compiler/test_jit_macro_deopt_hygiene_2100.cpp` (#2100) [domain_suite, theme_compiler] — AC1: Expand → IR attrs (source_marker/provenance/source_ast_node_id) present
- `tests/compiler/test_jit_macro_introduced_preserve.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2022; side-table + FunctionMeta + FlatFunction fields
- `tests/compiler/test_jit_metrics.cpp` (—) [domain_suite, theme_compiler] — test_jit_metrics.cpp — Issue #114 JIT observability + per-function cache tests
- `tests/compiler/test_jit_metrics_stub.cpp` (—) [small, domain_suite, theme_compiler] — test_jit_metrics_stub.cpp — Stub for the JIT test.
- `tests/compiler/test_live_closure_stable_id_only_2369.cpp` (#2369) [domain_suite, theme_compiler] — AC1: positive — stable_func_id present → remap, name-fallback counter 0
- `tests/compiler/test_lock_order_audit_2316.cpp` (#2316) [domain_suite, theme_compiler] — test_lock_order_audit_2316.cpp — Issue #2316:
- `tests/compiler/test_module_partition_map_2524.cpp` (#2524) [domain_suite, theme_compiler] — AC1: Measurable reduction OR clear partition map (pass_manager facade
- `tests/compiler/test_optimization_passes_contracts.cpp` (—) [domain_suite, theme_compiler] — AC1: 4 core passes satisfy Pass / DirtyAware / PureAnalysis where applicable
- `tests/compiler/test_orch_hot_update_health_throttle_2543.cpp` (#2543) [domain_suite, theme_compiler] — AC1: StormLevel ≠ None → health_bp drops; throttle fires; cap=1
- `tests/core/test_pair_slot_lock.cpp` (—) [domain_suite, theme_core] — test_pair_slot_lock.cpp -- runtime smoke test for B-024 / #1998
- `tests/compiler/test_partial_relower_cascade_2041.cpp` (#2041) [domain_suite, theme_compiler] — Issue #2041 — Partial re-lower + JIT hot-swap end-to-end on
- `tests/compiler/test_partial_relower_storm_gate_2190.cpp` (#2190) [domain_suite, theme_compiler] — AC1: Global storm + small dirty → full + forced_full metric
- `tests/compiler/test_pereval_reemit_region_independence_2606.cpp` (#2606) [domain_suite, theme_compiler] — AC1: Dual eval; dirty candidates A+B under reemit owner A → only A
- `tests/compiler/test_primcall_narg_2576.cpp` (#2576) [domain_suite, theme_compiler] — AC1: string-append 3 strings → ABC under default JIT
- `tests/compiler/test_reemit_production_default_defer_2205.cpp` (#2205) [domain_suite, theme_compiler] — AC1: Production default (reset / process init) → policy Defer;
- `tests/compiler/test_reemit_production_default_defer_2208.cpp` (#2208) [domain_suite, theme_compiler] — AC1: Default policy is Defer; SoftEnter requires explicit set.
- `tests/compiler/test_region_priority_deopt_throttle_2132.cpp` (#2132) [domain_suite, theme_compiler] — AC1: should_throttle_reemit(region) vs no-arg global decision
- `tests/compiler/test_reload_recovery_query_2367.cpp` (#2367) [domain_suite, theme_compiler] — AC1: soft empty path — idle recovery → recovery-active=0, zeros free
- `tests/compiler/test_relower_fallback_reason_2193.cpp` (#2193) [domain_suite, theme_compiler] — AC1: RelowerFallbackReason enum defined
- `tests/compiler/test_relower_strategy_cache_lock.cpp` (—) [domain_suite, theme_compiler] — Issue #1839/#1855 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_shape_storm_partial_relower_2212.cpp` (#2212) [domain_suite, theme_compiler] — AC1: When StormLevel has Shape bit, partial is preferred for a wider
- `tests/compiler/test_spec_jit.cpp` (—) [large, domain_suite, theme_compiler] — test_spec_jit.cpp — Unit tests for L1 type specialization (Phase 2, #53)
- `tests/stdlib/test_spec_runtime.cpp` (—) [domain_suite, theme_stdlib] — test_spec_runtime.cpp — Runtime tests for L2 specialization (Phase 3, #53)
- `tests/compiler/test_specjit_per_eval_storm_isolation_2370.cpp` (#2370) [domain_suite, theme_compiler] — AC1: Soft / Global path — process-wide stamp path unchanged
- `tests/compiler/test_specjit_pereval_storm_e2e_2504.cpp` (#2504) [domain_suite, theme_compiler] — AC1: Dual eval, PerEval — storm in B does not clear A's specializations
- `tests/compiler/test_storm_isolation_2236.cpp` (#2236) [domain_suite, theme_compiler] — too — "prefer per-region hard too" per the issue AC2 note).
- `tests/compiler/test_typechecker_incremental_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_typechecker_incremental_batch.cpp — typechecker_incremental pair dup-merge
- `tests/compiler/test_workload_adaptive_relower_2127.cpp` (#2127) [domain_suite, theme_compiler] — AC1: default base=8 compatible with #2032 (no forced signals)
- `tests/compiler/test_write_string_escape_2574.cpp` (#2574) [domain_suite, theme_compiler] — AC1: (write "a\"b") → "a\"b" under default JIT path

### `shape_soa` — Shape / SoA / column layout (45)

**Target:** tests/core/test_soa_batch.cpp (no move needed)

**Priority:** P2 — small-medium; soa_batch precedent

#### domain/ (45)

- `tests/compiler/test_apply_closure_envframe_soa.cpp` (—) [domain_suite, theme_compiler] — Issue #1365/#1475/#1511/#1626/#1632/#1660 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_ast_concurrency.cpp` (—) [domain_suite, theme_core] — Issue #2444 — region_by_sym_dense_ concurrent set_function_region +
- `tests/core/test_binding_gens_atomic_2417.cpp` (#2417) [domain_suite, theme_core] — AC1: atomic shared_ptr snapshot for readers
- `tests/compiler/test_cpp26_contracts_hotpath_arena_soa_value_shape_pass.cpp` (—) [domain_suite, theme_compiler] — test_cpp26_contracts_hotpath_arena_soa_value_shape_pass.cpp — Issue #742:
- `tests/compiler/test_dead_coercion_columnar_2431.cpp` (#2431) [domain_suite, theme_compiler] — AC1: residual_aos_bridge_total unchanged by DCE SoA path; columnar_total bumps
- `tests/core/test_fixup_deltas_2392.cpp` (#2392) [domain_suite, theme_core] — AC1: valid deltas → absolute children restored correctly
- `tests/core/test_flatast_add_node_lock_2413.cpp` (#2413) [domain_suite, theme_core] — AC1: class contract documents flatast_mutex_ reader invariant
- `tests/core/test_flatast_atomic_lock_batch.cpp` (—) [batch_driver, domain_suite, theme_core] — test_flatast_atomic_lock_batch — domain batch for FlatAST / SoA /
- `tests/core/test_flatast_soa_read_guard_2488.cpp` (#2488) [domain_suite, theme_core] — AC1: public SoAReadGuard / SoAWriteGuard / get_soa_safe / try_acquire_*
- `tests/core/test_get_nodeview_snapshot_2453.cpp` (#2453) [domain_suite, theme_core] — AC1: sequential get() is self-consistent (tag matches payload)
- `tests/compiler/test_highperf_cpp26_gaps_arena_soa_value_shape_pass.cpp` (—) [domain_suite, theme_compiler] — test_highperf_cpp26_gaps_arena_soa_value_shape_pass.cpp — Issue #658:
- `tests/compiler/test_hot_contract_unify_2142.cpp` (#2142) [domain_suite, theme_compiler] — AC1: policy documented in cpp26_contract_stats.h (AURA_HOT_CONTRACT)
- `tests/compiler/test_ir_soa_dual_emit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — tests/compiler/test_ir_soa_dual_emit_batch.cpp — IR SoA dual-emit family dup-merge (R19 phase
- `tests/compiler/test_ir_soa_incremental_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #254/#403/#404/#506 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_layout_stamp_equality_8field_2519.cpp` (#2519) [domain_suite, theme_compiler] — AC1: operator== compares all 8 fields; shape-only / ir-only mismatch
- `tests/compiler/test_list_vector_soa_hotpath_ai_loops.cpp` (—) [domain_suite, theme_compiler] — test_list_vector_soa_hotpath_ai_loops.cpp — Issue #752:
- `tests/compiler/test_matcher_stable_captures.cpp` (—) [domain_suite, theme_compiler] — Issue #1695 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_param_begin_count_publish_2451.cpp` (#2451) [domain_suite, theme_core] — (count last after arena fill) under post-parse contract.
- `tests/core/test_raii_guard_flatast_lifetime_2454.cpp` (#2454) [domain_suite, theme_core] — AC1: scoped StructuralMutationGuard / ReaderLockGuard work
- `tests/core/test_region_dense_atomic_2443.cpp` (#2443) [domain_suite, theme_core] — AC1: concurrent writer + reader does not tear dense uint8 cells
- `tests/core/test_restamp_lazy_align_atomic_2421.cpp` (#2421) [domain_suite, theme_core] — AC1: flag is atomic (store/load with acquire/release)
- `tests/core/test_set_workspace_flat.cpp` (—) [domain_suite, theme_core] — Issue #1729 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_shape.cpp` (—) [large, domain_suite, theme_compiler] — test_shape.cpp — Unit tests for shape infrastructure (Phase 1, #53)
- `tests/compiler/test_shape_high_mutation_storm_2433.cpp` (#2433) [domain_suite, theme_compiler] — AC1: production default applies kHighMutationPreset knobs (no env)
- `tests/compiler/test_shape_profiler_burst_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #406/#407/#570/#605 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_shape_profiler_concurrency_2141.cpp` (#2141) [domain_suite, theme_compiler] — AC1: docs model A (shared_mutex) in shape_profiler.h
- `tests/compiler/test_shape_profiler_stability_deopt_fiber_task4.cpp` (—) [domain_suite, theme_compiler] — test_shape_profiler_stability_deopt_fiber_task4.cpp — Issue #570:
- `tests/compiler/test_shape_soa_unit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_shape_soa_unit_batch.cpp — Wave 36+ (#1957) shape_soa theme
- `tests/compiler/test_shape_storm_adaptive_2526.cpp` (#2526) [domain_suite, theme_compiler] — AC1: Compact-only sequences do not alone drive process-global storm
- `tests/compiler/test_shapeprofiler_stability_deopt_jit_mutate.cpp` (—) [domain_suite, theme_compiler] — test_shapeprofiler_stability_deopt_jit_mutate.cpp — Issue #605:
- `tests/compiler/test_soa_ban_residual_aos_bridge_2520.cpp` (#2520) [domain_suite, theme_compiler] — AC1: production SoA-only forbids to_aos_view without allow
- `tests/core/test_soa_batch.cpp` (—) [large, batch_driver, domain_suite, theme_core] — test_soa_batch.cpp
- `tests/core/test_soa_column_atomic_2440.cpp` (#2440) [domain_suite, theme_core] — AC1: concurrent reader + writer does not tear (epoch / stale / dirty)
- `tests/compiler/test_soa_generation_fence_2111.cpp` (#2111) [domain_suite, theme_compiler] — AC1: SoA functions expose generation; bump on mark_dirty
- `tests/compiler/test_soa_partial_desync_gate_2181.cpp` (#2181) [domain_suite, theme_compiler] — AC1: gate_partial_soa_dirty_sync_ + relower_define_blocks entry
- `tests/compiler/test_soa_view_enforcement.cpp` (—) [domain_suite, theme_compiler] — Issue #1241/#1517/#1619/#1918 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_subtree_gen_atomic_2422.cpp` (#2422) [domain_suite, theme_core] — AC1: element reads are atomic (no torn uint16; 32-bit cells)
- `tests/core/test_subtree_uses_sym_template_bloat_2456.cpp` (#2456) [domain_suite, theme_core] — AC1: subtree_uses_sym finds Variable uses / no false positives
- `tests/core/test_summary_flags_guard_2415.cpp` (#2415) [domain_suite, theme_core] — AC1: summary_flags_ documents GUARDED_BY N/A + atomic model
- `tests/core/test_tag_arity_index_lock_2419.cpp` (#2419) [domain_suite, theme_core] — AC1: find_by_tag_arity under shared map lock (after ensure)
- `tests/core/test_tag_arity_key_hash_2420.cpp` (#2420) [domain_suite, theme_core] — AC1: hash packs fields and applies splitmix-style finalizer (source-cite)
- `tests/core/test_validate_node_no_abort_2390.cpp` (#2390) [domain_suite, theme_core] — AC1: validate_post_restore with corrupt gen returns PostRestoreReport
- `tests/core/test_validate_post_restore_soa_2391.cpp` (#2391) [domain_suite, theme_core] — AC1: sym_id_ size != tag_.size() → PostRestoreReport size-mismatch
- `tests/compiler/test_value_tag_hot_path_2259.cpp` (#2259) [domain_suite, theme_compiler] — AC1: Pure is_* (is_fixnum_hot / is_int) match classify; single low2 path
- `tests/compiler/test_workspace_delete_child.cpp` (—) [domain_suite, theme_compiler] — tests/compiler/test_workspace_delete_child.cpp — Issue #1770: WorkspaceTree delete_child test.

### `observability` — Observability / metrics / query:*-stats (126)

**Target:** tests/compiler/test_obs_schema_matrix.cpp + tests/compiler/obs_schema_cases.hpp

**Priority:** P2 — often thin schema probes; collapse into obs matrix

#### domain/ (126)

- `tests/compiler/test_adaptive_reverify_limit_2146.cpp` (#2146) [domain_suite, theme_compiler] — AC1: dirty_count > 300 → adaptive limit > 256; planted CONFLICT found
- `tests/compiler/test_adt_hard_gate_exhaustiveness_2264.cpp` (#2264) [domain_suite, theme_compiler] — AC1: Full hard-gate + non-exhaustive inject → adt_ok=false; suite fails;
- `tests/orch/test_agent_ask_2231.cpp` (#2231) [domain_suite, theme_orch] — AC1 (#2231/#2401): Target uses agent_reply → agent_ask returns ok +
- `tests/compiler/test_anonymous_residual_stable_id_policy_2605.cpp` (#2605) [domain_suite, theme_compiler] — AC1: Named create → sid≠0; reemit soak → residual_backfill does not grow
- `tests/compiler/test_aot_stats_null_metrics.cpp` (—) [small, domain_suite, theme_compiler] — Issue #1835/#1843 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_ast_ops_stats_workspace_lock.cpp` (—) [domain_suite, theme_core] — Issue #1729/#1851/#1852 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_atomic_mark_bitvector_2117.cpp` (#2117) [domain_suite, theme_serve] — AC1: multi-thread concurrent set same/adjacent bits → all set (no lost update)
- `tests/compiler/test_bidirectional_match_check_2348.cpp` (#2348) [domain_suite, theme_compiler] — AC1: Match check-mode — annotated (match ...) bodies checked under
- `tests/core/test_bidirectional_stats.cpp` (—) [domain_suite, theme_core] — tests/test_bidirectional_stats.cpp — Issue #1420 AC3:
- `tests/compiler/test_blame_stamp_on_degrade.cpp` (—) [small, domain_suite, theme_compiler] — Issue #2064 — blame / provenance stamping on Dynamic degrade + CoercionMap
- `tests/compiler/test_blame_tracking_typed_mutate.cpp` (—) [domain_suite, theme_compiler] — Issue #1617/#1924 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_bugfix_968.cpp` (#968) [small, domain_suite, theme_compiler] — Issue #957/#968/#982/#984 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_cache_entry_version_stamp.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2033; CacheEntryVersionStamp + kRelowerBridgeEpoch
- `tests/compiler/test_capability_effect_force_2072.cpp` (#2072) [domain_suite, theme_compiler] — Issue #2072 — force check_and_record_effect on all side-effect primitives.
- `tests/compiler/test_closedloop_stats_hash_cap.cpp` (—) [small, domain_suite, theme_compiler] — Issue #1795 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_closure_view_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_closure_view_batch.cpp — closure_view pair dup-merge (R19 phase 17).
- `tests/core/test_compiler_metrics_ownership.cpp` (—) [small, domain_suite, theme_core] — Issue #1835 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_composite_cs_signature_matrix_2509.cpp` (#2509) [domain_suite, theme_compiler] — AC1: expected_partial + empty CS → hard-miss under production (#2345)
- `tests/compiler/test_constraintsystem_solve_delta_clean_conflict_detection.cpp` (—) [domain_suite, theme_compiler] — test_constraintsystem_solve_delta_clean_conflict_detection.cpp
- `tests/core/test_cpp26_contracts_hotpath.cpp` (—) [domain_suite, theme_core] — Issue #1321/#1519/#1620/#742 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_cross_cow_drift_contract_2505.cpp` (#2505) [domain_suite, theme_compiler] — AC1: Near-drift live closure → soft migrate + remount ok; soft +1
- `tests/compiler/test_cross_cow_soft_migrate_2371.cpp` (#2371) [domain_suite, theme_compiler] — AC1: soft migrate enabled by default; stale within drift → restamp + continue
- `tests/compiler/test_dead_coercion_elim.cpp` (—) [small, domain_suite, theme_compiler] — Issue #2066 — DeadCoercionElimination IR-layer CastOp elision test.
- `tests/compiler/test_dead_coercion_elision_narrow_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — tests/compiler/test_dead_coercion_elision_narrow_batch.cpp — dead_coercion_elision_narrow pair
- `tests/compiler/test_dead_coercion_layered_2282.cpp` (#2282) [domain_suite, theme_compiler] — test_dead_coercion_layered_2282.cpp
- `tests/compiler/test_defuse_version_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #189/#417/#419/#456 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_delta_truncate_goal_priority_2508.cpp` (#2508) [domain_suite, theme_compiler] — AC1: Truncate + non-empty occurrence_goals → goal-priority reverify
- `tests/compiler/test_densify_envframe_ok_2361.cpp` (#2361) [domain_suite, theme_compiler] — AC1: Soft / no Moving densify → envframe_ok stays true (vacuous)
- `tests/compiler/test_densify_root_closure_closed_loop_2365.cpp` (#2365) [domain_suite, theme_compiler] — AC1: Soft / no Moving densify → root_remap_ok + closure_remount_ok true
- `tests/compiler/test_dispatch_required_effects_2152.cpp` (#2152) [domain_suite, theme_compiler] — Issue #2583 — Hard path: every non-zero required_effects call goes
- `tests/compiler/test_dual_path_desync_hard_fail_2116.cpp` (#2116) [domain_suite, theme_compiler] — AC1: inject desync → hard path; metric++; materialize bindings empty
- `tests/compiler/test_engine_metrics_facade.cpp` (—) [domain_suite, theme_compiler] — AC1: (engine:metrics) returns hash with nested groups + ≥200 metric fields
- `tests/compiler/test_envframe_resolve_distinction.cpp` (—) [domain_suite, theme_compiler] — Issue #1708/#1709/#1754/#1756/#1890 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_eval_relower_hotpath.cpp` (—) [domain_suite, theme_compiler] — Issue #1506/#1601/#1605/#1623 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_fiber_macro_hygiene_refresh.cpp` (—) [domain_suite, theme_compiler] — Issue #1490/#1592/#1608/#1612 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_fine_dirty_relower.cpp` (—) [domain_suite, theme_compiler] — test_fine_dirty_relower.cpp — Issue #1657 (standalone; bump metrics ACs drift)
- `tests/compiler/test_frame_budget_cascade_isolation_2137.cpp` (#2137) [domain_suite, theme_compiler] — AC1: under FrameBudget / render hotpath, non-render cascade deferred
- `tests/compiler/test_inline_typecheck_exception.cpp` (—) [domain_suite, theme_compiler] — Issue #1769 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_instr_impact_minimal_dirty_2126.cpp` (#2126) [minimal, domain_suite, theme_compiler] — AC1: nested lambda free-var body-only → no mark_all_blocks_dirty;
- `tests/compiler/test_instr_level_impact_scope.cpp` (—) [domain_suite, theme_compiler] — AC1: source cites #2031; ImpactScope::InstrRef + SourceIrLoc + affected_instrs
- `tests/compiler/test_instruction_level_impact_partial_2109.cpp` (#2109) [domain_suite, theme_compiler] — AC1: compute_impact_scope returns non-empty affected_instrs / affected_insts
- `tests/compiler/test_invalidate_consistency.cpp` (—) [domain_suite, theme_compiler] — Issue #1496/#1607/#1627 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_invalidations_stats_workspace_lock.cpp` (—) [domain_suite, theme_compiler] — Issue #1729/#1851 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_ir_metadata_interpreter_jit_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #403/#506/#570/#598 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_ir_soa_layout_stamp_2432.cpp` (#2432) [domain_suite, theme_compiler] — AC1: generation advance after stamp → resume mismatch + fence hit
- `tests/compiler/test_isolation_stamp_resolve_2224.cpp` (#2224) [domain_suite, theme_compiler] — AC1: export_ref / export_ref_safe stamp tenant + fiber (Phase A mandate)
- `tests/compiler/test_let_poly_solve_delta.cpp` (—) [domain_suite, theme_compiler] — Issue #1617/#745/#798 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_linear_boundary_consistency.cpp` (—) [domain_suite, theme_compiler] — Issue #1568 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_linear_live_closure_walk.cpp` (—) [domain_suite, theme_compiler] — Issue #1557/#1568/#1596/#1659/#1895 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_linear_walk_active_closures.cpp` (—) [domain_suite, theme_compiler] — Issue #1895/#1928 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_live_closure_full_restamp_2542.cpp` (#2542) [domain_suite, theme_compiler] — AC1: N named closures + reemit → epoch_restamp_total ≥ N
- `tests/compiler/test_lock_order_production_soft_2557.cpp` (#2557) [domain_suite, theme_compiler] — AC1: apply_production_lock_order_default(false) → soft; inversion bumps metrics
- `tests/compiler/test_longrunning_infra_primitives.cpp` (—) [domain_suite, theme_compiler] — test_longrunning_infra_primitives.cpp — Issue #753:
- `tests/compiler/test_longrunning_recovery_latency.cpp` (—) [domain_suite, theme_compiler] — AC1: panic-restore path instruments recovery latency
- `tests/compiler/test_lookup_stats_impl_heterogeneous.cpp` (—) [small, domain_suite, theme_compiler] — Issue #1671 (#1978 renamed): issue# moved from filename to header.
- `tests/orch/test_mailbox_bp_admit_2228.cpp` (#2228) [domain_suite, theme_orch] — AC1: Spawn soft reject — fill a mailbox to high_water (triggers
- `tests/compiler/test_module_export_cache.cpp` (—) [domain_suite, theme_compiler] — Issue #1680 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_must_deopt_before_next_call_2128.cpp` (#2128) [domain_suite, theme_compiler] — AC1: flag set on remap miss; aura_closure_call force-deopts (no silent native)
- `tests/compiler/test_mutation_aot_unit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_mutation_aot_unit_batch.cpp — consolidated mutation-theme drivers
- `tests/compiler/test_mutation_contention_2040.cpp` (#2040) [domain_suite, theme_compiler] — Issue #2040 — high-concurrency observability for Guard hold +
- `tests/serve/test_mutation_hold_time.cpp` (—) [domain_suite, theme_serve] — test_mutation_hold_time.cpp — Issue #1375:
- `tests/compiler/test_named_closure_stable_id_at_create_2550.cpp` (#2550) [domain_suite, theme_compiler] — AC1: Every named closure after create/set_name has stable_func_id != 0
- `tests/reflect/test_obs_json_to_json_a1.cpp` (—) [small, domain_suite, theme_reflect] — Wave A1: snapshot_to_json / fn_metrics_to_json via reflect to_json.
- `tests/compiler/test_obs_metrics_smoke_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_obs_metrics_smoke_batch.cpp — consolidated observability schema smokes
- `tests/compiler/test_obs_schema_matrix.cpp` (—) [domain_suite, theme_compiler] — test_obs_schema_matrix.cpp — Domain suite: observability + production schemas
- `tests/compiler/test_observability_tier_table.cpp` (—) [obs_named, domain_suite, theme_compiler] — Issue #1670 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_occ_cache_stats_wired.cpp` (—) [domain_suite, theme_compiler] — Issue #1781 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_open_issues_meta_batch.cpp` (—) [batch_driver, domain_suite, theme_core] — Issue #514/#515/#516/#517/#520 (#1978 renamed): issue# moved from filename to header.
- `tests/orch/test_orch_obs_facade_2589.cpp` (#2589) [domain_suite, theme_orch] — tests/orch/test_orch_obs_facade_2589.cpp
- `tests/compiler/test_panic_defer_after_densify_2364.cpp` (#2364) [domain_suite, theme_compiler] — AC1: Soft / no densify / no panic → free path (zero clear/rearm)
- `tests/compiler/test_pass_contracts_hotpath_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #381/#406/#506/#571 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_pattern_structural_index_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #211/#421/#423/#547/#554 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_post_steal_closed_loop.cpp` (—) [domain_suite, theme_serve] — Issue #1592 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_predicate_meet_join_lattice_2148.cpp` (#2148) [domain_suite, theme_compiler] — AC1: (and (number? x) (integer? x)) refines to Int, not Dynamic
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
- `tests/compiler/test_query_index_composite_2403.cpp` (#2403) [domain_suite, theme_compiler] — AC1: Constrained pattern (tag+arity±marker) hits composite index;
- `tests/core/test_raw_pointer_safety.cpp` (—) [domain_suite, theme_core] — Issue #1898 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_refinement_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #432/#467/#495/#509/#574 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_regex_redos_timeout_2479.cpp` (#2479) [domain_suite, theme_compiler] — AC1: well-formed regex succeeds within budget
- `tests/compiler/test_remount_force_deopt_2503.cpp` (#2503) [domain_suite, theme_compiler] — AC1: Unmapped densify candidate → remount_or_force_deopt returns 0,
- `tests/compiler/test_reverify_expand_2356.cpp` (#2356) [domain_suite, theme_compiler] — occurrence / let-poly priority roots (between bounded reverify and
- `tests/compiler/test_runtime_observability_correlated_stats.cpp` (—) [obs_named, domain_suite, theme_compiler] — test_runtime_observability_correlated_stats_673.cpp — Issue #673:
- `tests/compiler/test_safe_snapshot_umbrella.cpp` (—) [domain_suite, theme_compiler] — Issue #1839/#1856 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_scan_skip_freed_closures.cpp` (—) [domain_suite, theme_compiler] — Issue #1665 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_security_posture_trail_2534.cpp` (#2534) [small, domain_suite, theme_compiler] — AC1: posture returns schema-2534
- `tests/compiler/test_self_evo_stats.cpp` (—) [domain_suite, theme_compiler] — Issue #1909 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_self_evolution_chaos_stable.cpp` (—) [domain_suite, theme_serve] — test_self_evolution_chaos_stable_674.cpp — Issue #674:
- `tests/compiler/test_self_evolution_loop_stats.cpp` (—) [domain_suite, theme_compiler] — Issue #1883 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_self_heal_policy_engine.cpp` (—) [domain_suite, theme_serve] — test_self_heal_policy_engine.cpp — standalone (flaky/failing ACs under batch link)
- `tests/compiler/test_selfevo_bugfix_941.cpp` (#941) [small, domain_suite, theme_compiler] — test_selfevo_bugfix_941_967.cpp — Issues #941–#967 Phase 1
- `tests/compiler/test_shape_linear_collaborative_pass.cpp` (—) [domain_suite, theme_compiler] — Issue #1531/#1661/#462/#606 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_solve_delta_unresolved_export_2107.cpp` (#2107) [large, domain_suite, theme_compiler] — AC1: Synthetic over-limit → TIMEOUT + non-empty unresolved
- `tests/compiler/test_source_to_ir_desync_recovery_2206.cpp` (#2206) [domain_suite, theme_compiler] — AC1: After intentional map desync, recovery patches/rebuilds and
- `tests/compiler/test_stale_closure_fallback.cpp` (—) [domain_suite, theme_compiler] — AC1: apply_closure after mark_define_dirty / epoch bump →
- `tests/compiler/test_stats_catalog_drift.cpp` (—) [domain_suite, theme_compiler] — Issue #1672 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_stats_facade_bench.cpp` (—) [small, domain_suite, theme_compiler] — Issue #1434 stretch: facade vs N direct stats dispatches.
- `tests/compiler/test_stats_module_unification.cpp` (—) [domain_suite, theme_compiler] — test_stats_module_unification.cpp — Issue #560:
- `tests/compiler/test_stdlib_production_review_923.cpp` (#923) [small, domain_suite, theme_compiler] — test_stdlib_production_review_923_940.cpp — Issues #923–#940 Phase 1
- `tests/compiler/test_subtype_constraint_meet_2195.cpp` (#2195) [domain_suite, theme_compiler] — AC1: SUBTYPE goals in solve_delta; CONFLICT exports kind=SUBTYPE
- `tests/core/test_tenant_isolation_enforcement.cpp` (—) [domain_suite, theme_core] — capability cross-tenant grant, provenance deny, Strict sandbox link,
- `tests/compiler/test_test_strategy.cpp` (—) [domain_suite, theme_compiler] — Issue #1623/#1624/#1627/#1887 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_tier_dispatch.cpp` (—) [domain_suite, theme_compiler] — test_tier_dispatch.cpp — Issue #1356: HotTierTable for kPrimPerfHot primitives
- `tests/compiler/test_timeout_repair_rich_roots_2548.cpp` (#2548) [domain_suite, theme_compiler] — AC1: TIMEOUT with live occurrence goals → suggested set includes
- `tests/compiler/test_tree_walker_fallback_strict_2213.cpp` (#2213) [domain_suite, theme_compiler] — AC1: Under Forbidden (production-strict), needs_tree_walker → HardError
- `tests/core/test_type_cache_stats_snapshot.cpp` (—) [domain_suite, theme_core] — Issue #1797 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_type_dep_partial_merge_2283.cpp` (#2283) [domain_suite, theme_compiler] — test_type_dep_partial_merge_2283.cpp
- `tests/compiler/test_type_prop_invariant_correlation.cpp` (—) [domain_suite, theme_compiler] — Issue #1884 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_type_propagation_dead_coercion.cpp` (—) [domain_suite, theme_compiler] — test_type_propagation_dead_coercion.cpp — Issue #1874 (#1978 renamed):
- `tests/compiler/test_type_system_health_next_action_2462.cpp` (#2462) [domain_suite, theme_compiler] — for Agent closed-loop.
- `tests/compiler/test_unified_invalidation.cpp` (—) [domain_suite, theme_compiler] — Issue #1448/#1476/#1496/#1607 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_verify_parse_shared_helper.cpp` (—) [domain_suite, theme_compiler] — Issue #1771 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_workspace_mtx_contention_2523.cpp` (#2523) [domain_suite, theme_compiler] — AC1: Source cites #2523; residual strategy documented

### `uncategorized` — Uncategorized / mixed (42)

**Target:** manual triage before domain placement

**Priority:** P3 — review case-by-case

#### domain/ (42)

- `tests/compiler/test_arithmetic_int64_safety.cpp` (—) [small, domain_suite, theme_compiler] — test_arithmetic_int64_safety.cpp — Issues #1150–#1156 Phase 1
- `tests/compiler/test_ast_workspace_modules.cpp` (—) [domain_suite, theme_compiler] — test_ast_workspace_modules.cpp — Issue #563:
- `tests/stdlib/test_atomic_swap_stdlib.cpp` (—) [domain_suite, theme_stdlib] — test_atomic_swap_stdlib.cpp — Issue #1380:
- `tests/compiler/test_aura_result_error_policy.cpp` (—) [domain_suite, theme_compiler] — test_aura_result_error_policy.cpp — Issues #807 + #808:
- `tests/compiler/test_channel_rendezvous_2483.cpp` (#2483) [domain_suite, theme_compiler] — AC1: rendezvous send blocks until concurrent recv
- `tests/compiler/test_closure_free.cpp` (—) [domain_suite, theme_compiler] — test_closure_free.cpp — Issue #1361: aura_free_closure + ID reuse
- `tests/compiler/test_command_line_cap_io_read_2478.cpp` (#2478) [domain_suite, theme_compiler] — AC1: sandbox + no io-read → capability denied error
- `tests/compiler/test_compile02_no_dup_imports.cpp` (—) [domain_suite, theme_compiler] — Issue #1857 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_composite_commit_cs_reuse_2180.cpp` (#2180) [domain_suite, theme_compiler] — AC1: inject type conflict into commit CS → solve_fail + reject
- `tests/compiler/test_core_builtins_review.cpp` (—) [domain_suite, theme_compiler] — test_core_builtins_review.cpp — Issue #564:
- `tests/compiler/test_eval_current_no_auto_fix_2484.cpp` (#2484) [domain_suite, theme_compiler] — AC1: last form lambda → closure returned unchanged
- `tests/orch/test_failure_policy_bridge_2539.cpp` (#2539) [domain_suite, theme_orch] — AC1: to_agent_policy(FailurePolicy, max_restarts=...) callable under aura::orch
- `tests/core/test_general_object_pin_coverage_gate_2496.cpp` (#2496) [domain_suite, theme_core] — AC1: Linter fails when a listed inventory site lacks wire call
- `tests/core/test_hash_iter_invalidation.cpp` (—) [domain_suite, theme_core] — test_hash_iter_invalidation.cpp - Issue #1398:
- `tests/compiler/test_ir_const_string_intern_2573.cpp` (#2573) [domain_suite, theme_compiler] — AC1: IR-path loop with a string literal: heap growth O(1) not O(N)
- `tests/compiler/test_ir_optimize_type_info_chain_2471.cpp` (#2471) [domain_suite, theme_compiler] — AC1: X→0→5 multi-step chain remaps uses to terminal source (not MAX)
- `tests/compiler/test_json_parse_number_exception_2480.cpp` (#2480) [domain_suite, theme_compiler] — AC1: oversized integer → error (not crash)
- `tests/compiler/test_json_parse_object_grow_2481.cpp` (#2481) [domain_suite, theme_compiler] — AC1: 8-key object retains all keys
- `tests/compiler/test_list_end_of_list_void_2482.cpp` (#2482) [domain_suite, theme_compiler] — AC1: (null? 0) → false; (null? (list)) → true
- `tests/compiler/test_load_cap_io_read_2485.cpp` (#2485) [domain_suite, theme_compiler] — AC1: sandbox + no io-read → capability denied error
- `tests/compiler/test_module_export_display_2572.cpp` (#2572) [domain_suite, theme_compiler] — AC1: issue repro — (require) + multi-display export prints prefix + arg
- `tests/compiler/test_module_load_tail_export_2570.cpp` (#2570) [domain_suite, theme_compiler] — AC1: tail defines always export after require
- `tests/compiler/test_module_loader_dead_heap_circular.cpp` (—) [domain_suite, theme_compiler] — Issue #1488/#1692 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_module_prefix_dead_heap.cpp` (—) [domain_suite, theme_compiler] — Issue #1488/#1693 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_node_meta_bounds_2410.cpp` (#2410) [domain_suite, theme_core] — AC1: meta(NodeTag{}) returns well-defined sentinel (no UB)
- `tests/compiler/test_open_issues_phase1_batch.cpp` (—) [phase_slice, batch_driver, domain_suite, theme_compiler] — test_open_issues_phase1_batch.cpp — legacy alias for the domain suite.
- `tests/core/test_pair_unchecked_safety.cpp` (—) [domain_suite, theme_core] — Issue #1710 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_panic_checkpoint_batch.cpp` (—) [batch_driver, domain_suite, theme_core] — tests/core/test_panic_checkpoint_batch.cpp
- `tests/core/test_persist_basic.cpp` (—) [domain_suite, theme_core] — test_persist_basic.cpp — Issue #1381:
- `tests/core/test_prim_call_count_clamp.cpp` (—) [small, domain_suite, theme_core] — AC1: count > max clamped
- `tests/compiler/test_primcall_str_intern_2577.cpp` (#2577) [domain_suite, theme_compiler] — AC1: N× (string-append x "!") — eval heap growth ≪ N
- `tests/compiler/test_query_namespace_audit.cpp` (—) [domain_suite, theme_compiler] — test_query_namespace_audit.cpp — Issue #562:
- `tests/compiler/test_side_effect_security_gate_hardfail_2494.cpp` (#2494) [domain_suite, theme_compiler] — AC1: Intentionally broken fixture prim (side-effect name, no
- `tests/stdlib/test_stdlib_infrastructure.cpp` (—) [domain_suite, theme_stdlib] — test_stdlib_infrastructure.cpp — Issue #565:
- `tests/core/test_stringpool_concurrent_intern.cpp` (—) [domain_suite, theme_core] — Issue #2062 — StringPool thread-safe intern test.
- `tests/stdlib/test_synthesize_namespace_demotion.cpp` (—) [domain_suite, theme_stdlib] — test_synthesize_namespace_demotion.cpp — Issue #561:
- `tests/compiler/test_sys_open_path_harden_2487.cpp` (#2487) [domain_suite, theme_compiler] — AC1: (sys-open "/proc/self/mem") → -1 (path_is_denied)
- `tests/compiler/test_truncate_commit_gate_2458.cpp` (#2458) [domain_suite, theme_compiler] — AC1: Soft Sampled + truncated → observe; commit_ok allows; no reject
- `tests/compiler/test_try_catch_bind_2567.cpp` (#2567) [domain_suite, theme_compiler] — AC1: (try (no-such-fn …) (catch (e) (string? e))) → #t; list/cons usable
- `tests/core/test_try_lock_workspace_lock_order.cpp` (—) [domain_suite, theme_core] — Issue #1768 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_while_define_oneshot_2571.cpp` (#2571) [domain_suite, theme_compiler] — AC1: issue repro — nested while with (define x 0) yields count=6
- `tests/compiler/test_workspace_dispatch.cpp` (—) [domain_suite, theme_compiler] — tests/compiler/test_workspace_dispatch.cpp — Issue #1437: workspace :op dispatch contract test.

## Regenerating

```bash
python3 scripts/inventory_legacy_tests.py
python3 scripts/inventory_legacy_tests.py --check
```

The coarser Phase-2 5-domain classifier remains available as `scripts/classify_test_issues.py` for historical comparison; **this inventory (#1957) is the planning source of truth** for domain migration.
