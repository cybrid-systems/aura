# Root test classification

**Generated:** 2026-07-21 by `scripts/classify_root_tests.py`
**Content hash:** `3be7599554a56e98`
**Companion:** [`legacy_test_inventory.md`](legacy_test_inventory.md) (#1957 issues/)

## Purpose

Probe `tests/test_*.cpp` (root), classify into theme buckets that match `tests/domain/` and family batches, flag near-dups / supersessions, and drive streamline waves **without losing coverage**.

## Snapshot

| Location | Count |
|----------|------:|
| `tests/test_*.cpp` (root) | 437 |
| `tests/domain/**/test_*.cpp` | 18 |
| Near-dup name clusters (≥2) | 10 |

### Domain suite anchors (coverage homes)

- `test_domain_fiber_orchestration.cpp` → **fiber_orch** — dest: `tests/domain/test_domain_fiber_orchestration.cpp + fiber_resume_batch`
- `test_domain_hygiene_dirty.cpp` → **edsl_hygiene** — dest: `tests/domain/test_domain_hygiene_dirty.cpp + macro_reflect_batch`
- `test_domain_typed_mutate.cpp` → **mutation_dirty** — dest: `tests/domain/test_domain_typed_mutate.cpp + mutation_boundary_batch`
- `test_obs_schema_matrix.cpp` → **observability** — dest: `tests/domain/test_obs_schema_matrix.cpp + cases/obs_schema_cases.hpp`
- `arena/test_arena_batch.cpp` → **arena_compaction** — dest: `tests/domain/arena/ (batch pilots already live)`
- `arena/test_arena_defrag_concurrent.cpp` → **arena_compaction** — dest: `tests/domain/arena/ (batch pilots already live)`
- `arena/test_compact_batch.cpp` → **arena_compaction** — dest: `tests/domain/arena/ (batch pilots already live)`
- `arena/test_compact_sweep_batch.cpp` → **arena_compaction** — dest: `tests/domain/arena/ (batch pilots already live)`
- `arena/test_gc_batch.cpp` → **arena_compaction** — dest: `tests/domain/arena/ (batch pilots already live)`

## Theme distribution (root)

| Theme | Count | Preferred destination |
|-------|------:|------------------------|
| `arena_compaction` | 7 | tests/domain/arena/ (batch pilots already live) |
| `mutation_dirty` | 118 | tests/domain/test_domain_typed_mutate.cpp + mutation_boundary_batch |
| `fiber_orch` | 38 | tests/domain/test_domain_fiber_orchestration.cpp + fiber_resume_batch |
| `linear_ownership` | 2 | tests/test_linear_ownership_batch.cpp → domain/ |
| `edsl_hygiene` | 19 | tests/domain/test_domain_hygiene_dirty.cpp + macro_reflect_batch |
| `jit_incremental` | 7 | future domain/jit/ (heavy JIT stays EXCLUDE or root) |
| `shape_soa` | 1 | tests/test_soa_batch.cpp → domain/ |
| `observability` | 180 | tests/domain/test_obs_schema_matrix.cpp + cases/obs_schema_cases.hpp |
| `stdlib` | 5 | tests/suite/ + focused root integration (datetime, hot-update) |
| `compiler_core` | 58 | keep root or future domain/compiler/ |
| `uncategorized` | 2 | manual triage |

## Action summary (streamline plan)

| Action | Count | Meaning |
|--------|------:|---------|
| `keep` | 375 | Retain root binary for now |
| `keep_batch_exclude` | 18 | Family batch; already EXCLUDE_FROM_ALL convention |
| `keep_behavioral` | 17 | Has behavioral ACs beyond schema flags |
| `fold_obs_fieldlist` | 13 | Pure production flag gate → FieldListCase |
| `candidate_obs_fold` | 12 | Thin schema probe → fold into obs matrix cases |
| `keep_link_stub` | 1 | Link-only helper (not a test suite) |
| `superseded_exclude` | 1 | Covered by domain suite / later issue — exclude or delete |

## Wave status (streamline implementation)

| Wave | Status | What shipped |
|------|--------|--------------|
| 0 | **done** | Empty Phase-2 stubs deleted; `open_issues_phase1_batch` EXCLUDE |
| 1 | **done** | Thin probes → `obs_schema_cases.hpp` FieldList; selfevo/stdlib EXCLUDE |
| 2 | **done** | `test_domain_production_sweep` + `production_sweep_cases.hpp`; ~27 prod EXCLUDE |
| 3 | **done** | Near-dup supersession EXCLUDE (1636, fine_dirty, 1622, …) |
| 4 | **done** | Root `test_issue_1943…1956` → `tests/domain/` |

Prefer **extend domain/** over new root binaries (see tests/README.md).

## Near-dup name clusters

Name-normalized groups (strip issue suffix / task / closed_loop). Not always redundant — inspect AC headers before merging.

### `test_ai` (2)
- `test_ai_closedloop_orch_readiness_1597.cpp` — theme=`fiber_orch` action=`keep` (230L)
- `test_ai_closedloop_readiness_1593.cpp` — theme=`observability` action=`keep` (171L)

### `test_cpp26_contracts` (2)
- `test_cpp26_contracts_hotpath_1620.cpp` — theme=`observability` action=`keep` (146L)
- `test_cpp26_contracts_hotpath_arena_soa_value_shape_pass.cpp` — theme=`observability` action=`keep` (122L)

### `test_fine_dirty_relower` (2)
- `test_fine_dirty_relower.cpp` — theme=`mutation_dirty` action=`keep` (96L)
- `test_fine_dirty_relower_1915.cpp` — theme=`observability` action=`keep` (225L)

### `test_incremental_typed_selfmod_dirty_narrowing` (2)
- `test_incremental_typed_selfmod_dirty_narrowing.cpp` — theme=`mutation_dirty` action=`keep` (282L)
- `test_incremental_typed_selfmod_dirty_narrowing_task1.cpp` — theme=`mutation_dirty` action=`keep` (291L)

### `test_query_pattern_hygiene_mandate` (2)
- `test_query_pattern_hygiene_mandate_1636.cpp` — theme=`edsl_hygiene` action=`keep` (187L)
- `test_query_pattern_hygiene_mandate_1892.cpp` — theme=`edsl_hygiene` action=`keep` (203L)

### `test_render` (2)
- `test_render_hotpath_observability_1674.cpp` — theme=`observability` action=`keep` (150L)
- `test_render_hotpath_stability_under_mutation.cpp` — theme=`observability` action=`keep` (173L)

### `test_resource_quota` (2)
- `test_resource_quota.cpp` — theme=`fiber_orch` action=`keep` (220L)
- `test_resource_quota_hotpath.cpp` — theme=`observability` action=`keep` (229L)

### `test_stable_ref_provenance_fiber_cow` (2)
- `test_stable_ref_provenance_fiber_cow.cpp` — theme=`mutation_dirty` action=`keep` (316L)
- `test_stable_ref_provenance_fiber_cow_task1.cpp` — theme=`mutation_dirty` action=`keep` (319L)

### `test_typed_mutation_audit` (2)
- `test_typed_mutation_audit.cpp` — theme=`mutation_dirty` action=`keep` (192L)
- `test_typed_mutation_audit_hotpath_1894.cpp` — theme=`mutation_dirty` action=`keep` (177L)

### `test_value_encoding_v2_dispatch_contracts` (2)
- `test_value_encoding_v2_dispatch_contracts_1622.cpp` — theme=`observability` action=`keep` (178L)
- `test_value_encoding_v2_dispatch_contracts_task4.cpp` — theme=`observability` action=`keep` (383L)

## Superseded / alias map

| Root file | Prefer |
|-----------|--------|
| `test_open_issues_phase1_batch.cpp` | domain/test_obs_schema_matrix.cpp (alias / jit_late3 bundle; EXCLUDE_FROM_ALL) |

## Per-theme file list (root)

### arena_compaction (7)

| File | Lines | Action | Notes |
|------|------:|--------|-------|
| `test_closure_bridge_lifetime_safety.cpp` | 216 | `keep` | no automatic streamline |
| `test_ir.cpp` | 4841 | `keep` | no automatic streamline |
| `test_pcv_heap_use_after_free.cpp` | 33 | `keep` | no automatic streamline |
| `test_render_memory_predictability_1675.cpp` | 167 | `keep` | no automatic streamline |
| `test_render_primitives.cpp` | 259 | `keep` | no automatic streamline |
| `test_terminal_lifecycle.cpp` | 164 | `keep` | no automatic streamline |
| `test_tl_arena_capacity.cpp` | 180 | `keep` | no automatic streamline |

### mutation_dirty (118)

| File | Lines | Action | Notes |
|------|------:|--------|-------|
| `test_adt_match_exhaust_post_mutate_reliability.cpp` | 208 | `keep` | no automatic streamline |
| `test_adt_match_exhaustiveness_incremental_task2.cpp` | 156 | `keep` | no automatic streamline |
| `test_aot_bridge_checkpoint_version_steal.cpp` | 169 | `keep` | no automatic streamline |
| `test_aot_hotupdate_typed_audit_1882.cpp` | 148 | `keep` | no automatic streamline |
| `test_aot_metrics_lazy.cpp` | 114 | `keep` | no automatic streamline |
| `test_aot_region_per_eval.cpp` | 163 | `keep` | no automatic streamline |
| `test_apply_closure_envframe_soa_1660.cpp` | 228 | `keep` | no automatic streamline |
| `test_atomic_batch_metadata_1893.cpp` | 182 | `keep` | no automatic streamline |
| `test_atomic_batch_rollback_closed_loop_529.cpp` | 110 | `keep` | no automatic streamline |
| `test_atomic_batch_rollback_fiber_task1.cpp` | 314 | `keep` | no automatic streamline |
| `test_atomic_batch_snapshot_stable_ref_ai_loops.cpp` | 184 | `keep` | no automatic streamline |
| `test_bidirectional_check_occurrence_narrow_post_mutate.cpp` | 164 | `keep` | no automatic streamline |
| `test_blame_chain_completeness_1873.cpp` | 195 | `keep` | no automatic streamline |
| `test_capability_gating.cpp` | 248 | `keep` | no automatic streamline |
| `test_clear_instruction_dirty_guard_1853.cpp` | 118 | `keep` | no automatic streamline |
| `test_closure_batch.cpp` | 548 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_coercion_dead_elim_castop_flow_zerooverhead.cpp` | 229 | `keep` | no automatic streamline |
| `test_composite_typed_mutate.cpp` | 335 | `keep` | no automatic streamline |
| `test_constraint_system_solve_delta_cross_delta_task2.cpp` | 211 | `keep` | no automatic streamline |
| `test_constraintsystem_solve_delta_touched_roots_509.cpp` | 193 | `keep` | no automatic streamline |
| `test_core_builtins_review.cpp` | 143 | `keep` | no automatic streamline |
| `test_dead_coercion_batch.cpp` | 660 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_dep_graph_concurrent.cpp` | 172 | `keep` | no automatic streamline |
| `test_depth_slot_instance_id_1746.cpp` | 137 | `keep` | no automatic streamline |
| `test_dirty_delta_present.cpp` | 237 | `keep` | no automatic streamline |
| `test_dirty_propagation_cascade.cpp` | 272 | `keep` | no automatic streamline |
| `test_dirty_short_circuit_api.cpp` | 65 | `keep` | no automatic streamline |
| `test_edsl_concurrent_fiber_boundary_task1.cpp` | 322 | `keep` | no automatic streamline |
| `test_edsl_concurrent_query_mutate.cpp` | 233 | `keep` | no automatic streamline |
| `test_edsl_core_stability_cow_atomic_query_mutate.cpp` | 134 | `keep` | no automatic streamline |
| `test_envframe_truncate_guard_dual_epoch_1927.cpp` | 249 | `keep` | no automatic streamline |
| `test_epoch_apply_hotpath_1598.cpp` | 293 | `keep` | no automatic streamline |
| `test_fiber_mutation_steal_safety.cpp` | 423 | `keep` | no automatic streamline |
| `test_fiber_steal_panic_checkpoint_nested_gc.cpp` | 98 | `keep` | no automatic streamline |
| `test_fine_dirty_relower.cpp` | 96 | `keep` | no automatic streamline |
| `test_followups.cpp` | 336 | `keep` | no automatic streamline |
| `test_generation_epoch_closed_loop_414.cpp` | 100 | `keep` | no automatic streamline |
| `test_guard_dtor_batch_metrics_1747.cpp` | 155 | `keep` | no automatic streamline |
| `test_guard_dtor_invariant_noexcept_1766.cpp` | 121 | `keep` | no automatic streamline |
| `test_guard_enter_ts_optional_1764.cpp` | 121 | `keep` | no automatic streamline |
| `test_guard_hold_max_cas_1765.cpp` | 152 | `keep` | no automatic streamline |
| `test_guard_move_ownership_1767.cpp` | 113 | `keep` | no automatic streamline |
| `test_hw_bitvec_register_guard_1850.cpp` | 121 | `keep` | no automatic streamline |
| `test_incremental_type_batch.cpp` | 475 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_incremental_typed_selfmod_dirty_narrowing.cpp` | 282 | `keep` | no automatic streamline |
| `test_incremental_typed_selfmod_dirty_narrowing_task1.cpp` | 291 | `keep` | no automatic streamline |
| `test_invalidate_cascade_order.cpp` | 194 | `keep` | no automatic streamline |
| `test_ir_soa_dual_emit.cpp` | 138 | `keep` | no automatic streamline |
| `test_issues_809_817_batch.cpp` | 178 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_issues_819_829_batch.cpp` | 205 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_let_poly_solve_delta_1617.cpp` | 204 | `keep` | no automatic streamline |
| `test_linear_batch.cpp` | 689 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_linear_ownership_post_mutate_1949.cpp` | 154 | `keep` | no automatic streamline |
| `test_linear_ownership_postmutate_guard_steal_envframe.cpp` | 125 | `keep` | no automatic streamline |
| `test_lock_order_closures_env_1664.cpp` | 211 | `keep` | no automatic streamline |
| `test_macro_hygiene_fiber_panic_aot_soa_self_evo.cpp` | 161 | `keep` | no automatic streamline |
| `test_macro_reflect_batch.cpp` | 590 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_marker_metadata_lock_1783.cpp` | 189 | `keep` | no automatic streamline |
| `test_mbp_macro_no_break_1745.cpp` | 137 | `keep` | no automatic streamline |
| `test_module_boundary_1885.cpp` | 141 | `keep` | no automatic streamline |
| `test_mutate_batch.cpp` | 1446 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_mutate_cross_thread_migration.cpp` | 212 | `keep` | no automatic streamline |
| `test_mutation_boundary_batch.cpp` | 644 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_mutation_guard_typed_error.cpp` | 187 | `keep` | no automatic streamline |
| `test_mutation_log_query_race.cpp` | 200 | `keep` | no automatic streamline |
| `test_mutation_provenance.cpp` | 187 | `keep` | no automatic streamline |
| `test_mutation_rollback_coverage_400.cpp` | 182 | `keep` | no automatic streamline |
| `test_mutation_systemic_guard_1897.cpp` | 227 | `keep` | no automatic streamline |
| `test_mutator_dispatch_stats_lock_1849.cpp` | 212 | `keep` | no automatic streamline |
| `test_narrowing_dirty_query_1779.cpp` | 115 | `keep` | no automatic streamline |
| `test_occurrence_dirty_blame_post_mutate_467.cpp` | 147 | `keep` | no automatic streamline |
| `test_occurrence_dirty_cycle_guard_1682.cpp` | 126 | `keep` | no automatic streamline |
| `test_occurrence_mutate_narrowing.cpp` | 349 | `keep` | no automatic streamline |
| `test_occurrence_narrow_blame_stale_invalidation_post_mutate.cpp` | 147 | `keep` | no automatic streamline |
| `test_occurrence_typing_blame_post_mutate_recovery.cpp` | 171 | `keep` | no automatic streamline |
| `test_occurrence_typing_blame_post_mutate_task2.cpp` | 176 | `keep` | no automatic streamline |
| `test_open_issues_phase1_batch.cpp` | 160 | `superseded_exclude` | domain/test_obs_schema_matrix.cpp (alias / jit_late3 bundle; EXCLUDE_FROM_ALL) |
| `test_panic_checkpoint_clear_1727.cpp` | 174 | `keep` | no automatic streamline |
| `test_panic_checkpoint_fiber_resume_safety.cpp` | 398 | `keep` | no automatic streamline |
| `test_per_defuse_batch.cpp` | 347 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_predicate_memo_partial_evict_1872.cpp` | 157 | `keep` | no automatic streamline |
| `test_production_sweep_1251_1255.cpp` | 90 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_production_sweep_1336_1348.cpp` | 162 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_provenance_blame_hygiene_1877.cpp` | 270 | `keep` | no automatic streamline |
| `test_query_mutate_consistency.cpp` | 139 | `keep` | no automatic streamline |
| `test_quota_edge_cases.cpp` | 224 | `keep` | no automatic streamline |
| `test_reflect_batch.cpp` | 603 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_runtime_mutation_boundary_steal_safety.cpp` | 360 | `keep` | no automatic streamline |
| `test_safepoint_mutation.cpp` | 179 | `keep` | no automatic streamline |
| `test_scan_skip_freed_closures_1665.cpp` | 177 | `keep` | no automatic streamline |
| `test_scheduler_gc_defer_pending_panic_steal.cpp` | 280 | `keep` | no automatic streamline |
| `test_scheduler_gc_safepoint_mutation_coordination.cpp` | 550 | `keep` | no automatic streamline |
| `test_self_evolution_chaos_stable_674.cpp` | 153 | `keep` | no automatic streamline |
| `test_seva_demo_metrics_1841.cpp` | 84 | `keep` | no automatic streamline |
| `test_solve_delta_locality_1871.cpp` | 160 | `keep` | no automatic streamline |
| `test_spec_jit.cpp` | 665 | `keep` | no automatic streamline |
| `test_stable_ref_cow_batch_1912.cpp` | 375 | `keep` | no automatic streamline |
| `test_stable_ref_cow_subworkspace_concurrent_ai.cpp` | 163 | `keep` | no automatic streamline |
| `test_stable_ref_cross_cow_provenance_enforcement.cpp` | 166 | `keep` | no automatic streamline |
| `test_stable_ref_full_provenance_enforcement.cpp` | 242 | `keep` | no automatic streamline |
| `test_stable_ref_provenance_fiber_cow.cpp` | 316 | `keep` | no automatic streamline |
| `test_stable_ref_provenance_fiber_cow_task1.cpp` | 319 | `keep` | no automatic streamline |
| `test_stable_ref_provenance_mandate_1630.cpp` | 296 | `keep` | no automatic streamline |
| `test_stale_closure_fallback.cpp` | 283 | `keep` | no automatic streamline |
| `test_stale_ref_string_heap_1681.cpp` | 95 | `keep` | no automatic streamline |
| `test_subtree_bump_guard_1847.cpp` | 123 | `keep` | no automatic streamline |
| `test_task2_refinement_closed_loop_495.cpp` | 134 | `keep` | no automatic streamline |
| `test_typechecker_incremental_dependency_occurrence_dirty_post_mutate.cpp` | 147 | `keep` | no automatic streamline |
| `test_typechecker_incremental_guard_steal_fidelity.cpp` | 162 | `keep` | no automatic streamline |
| `test_typechecker_incremental_locality_1923.cpp` | 254 | `keep` | no automatic streamline |
| `test_typed_mutation_audit.cpp` | 192 | `keep` | no automatic streamline |
| `test_typed_mutation_audit_hotpath_1894.cpp` | 177 | `keep` | no automatic streamline |
| `test_typed_mutation_invariant_audit_1614.cpp` | 184 | `keep` | no automatic streamline |
| `test_typesystem_solve_delta_occurrence_priority_heavy_mutate.cpp` | 210 | `keep` | no automatic streamline |
| `test_typesystem_typed_mutate_incremental_gaps.cpp` | 132 | `keep` | no automatic streamline |
| `test_unify_invalidate_try_acquire_1634.cpp` | 222 | `keep` | no automatic streamline |
| `test_verify_dirty_totals_snapshot_1840.cpp` | 139 | `keep` | no automatic streamline |

### fiber_orch (38)

| File | Lines | Action | Notes |
|------|------:|--------|-------|
| `test_agent_fingerprint_atomic_1730.cpp` | 150 | `keep` | no automatic streamline |
| `test_ai_closedloop_orch_readiness_1597.cpp` | 230 | `keep` | no automatic streamline |
| `test_auto_evolve_tick_no_dbg_1712.cpp` | 95 | `keep` | no automatic streamline |
| `test_concurrent.cpp` | 3723 | `keep` | no automatic streamline |
| `test_evolve_analytics_parse_1724.cpp` | 127 | `keep` | no automatic streamline |
| `test_evolve_name_collision_1726.cpp` | 172 | `keep` | no automatic streamline |
| `test_fiber_join.cpp` | 178 | `keep` | no automatic streamline |
| `test_fiber_join_linear.cpp` | 267 | `keep` | no automatic streamline |
| `test_find_after_parens_1723.cpp` | 108 | `keep` | no automatic streamline |
| `test_intend_closure_live_1719.cpp` | 121 | `keep` | no automatic streamline |
| `test_intend_heap_slots_1721.cpp` | 129 | `keep` | no automatic streamline |
| `test_jit_concurrent_compile.cpp` | 162 | `keep` | no automatic streamline |
| `test_multi_fiber_mailbox.cpp` | 237 | `keep` | no automatic streamline |
| `test_orch_agent_spawn.cpp` | 186 | `keep` | no automatic streamline |
| `test_orch_observability_1881.cpp` | 271 | `keep` | no automatic streamline |
| `test_orch_quota_integration_1880.cpp` | 249 | `keep` | no automatic streamline |
| `test_orch_resource_quota_1600.cpp` | 222 | `keep` | no automatic streamline |
| `test_orch_stable_ref_lifecycle_1879.cpp` | 240 | `keep` | no automatic streamline |
| `test_parallel_intend_primitive.cpp` | 166 | `keep` | no automatic streamline |
| `test_parallel_orch.cpp` | 332 | `keep` | no automatic streamline |
| `test_parallel_orchestration_stress_1602.cpp` | 290 | `keep` | no automatic streamline |
| `test_per_fiber_mutation_safepoint.cpp` | 276 | `keep` | no automatic streamline |
| `test_per_fiber_stack_pool_high_concurrency.cpp` | 196 | `keep` | no automatic streamline |
| `test_production_sweep_1202_1228.cpp` | 87 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_query_namespace_audit.cpp` | 262 | `keep` | no automatic streamline |
| `test_resource_quota.cpp` | 220 | `keep` | no automatic streamline |
| `test_resource_quota_module.cpp` | 183 | `keep` | no automatic streamline |
| `test_self_heal_policy_engine.cpp` | 181 | `keep` | no automatic streamline |
| `test_set_arena_atomic_owner_1663.cpp` | 235 | `keep` | no automatic streamline |
| `test_strategies_mtx_1722.cpp` | 126 | `keep` | no automatic streamline |
| `test_strategy_intend_mutex_1720.cpp` | 135 | `keep` | no automatic streamline |
| `test_strategy_set_errors_1714.cpp` | 132 | `keep` | no automatic streamline |
| `test_stress_alloc_storage_lock.cpp` | 117 | `keep` | no automatic streamline |
| `test_synthesize_json_parse_1715.cpp` | 227 | `keep` | no automatic streamline |
| `test_synthesize_optimize_prng_1716.cpp` | 105 | `keep` | no automatic streamline |
| `test_terminal_concurrent.cpp` | 174 | `keep` | no automatic streamline |
| `test_top_errors_stoi_1725.cpp` | 149 | `keep` | no automatic streamline |
| `test_try_probe_heap_slot_1718.cpp` | 109 | `keep` | no automatic streamline |

### linear_ownership (2)

| File | Lines | Action | Notes |
|------|------:|--------|-------|
| `test_hardware_resource_linear_ownership.cpp` | 270 | `keep` | no automatic streamline |
| `test_type_propagation_dead_coercion_1874.cpp` | 209 | `keep` | no automatic streamline |

### edsl_hygiene (19)

| File | Lines | Action | Notes |
|------|------:|--------|-------|
| `test_allow_macro_inline_per_eval_1780.cpp` | 120 | `keep` | no automatic streamline |
| `test_edsl_self_evolution_marker_dirty_guard_task6.cpp` | 125 | `keep` | no automatic streamline |
| `test_error_merr.cpp` | 38 | `keep` | no automatic streamline |
| `test_hygiene_protected_metadata_lock_1838.cpp` | 97 | `keep` | no automatic streamline |
| `test_hygiene_violation_closed_loop_422.cpp` | 123 | `keep` | no automatic streamline |
| `test_ir_cache_v2.cpp` | 67 | `keep` | no automatic streamline |
| `test_ir_closure_provenance_1616.cpp` | 140 | `keep` | no automatic streamline |
| `test_ir_hygiene_propagation_1610.cpp` | 142 | `keep` | no automatic streamline |
| `test_ir_macro_hygiene_e2e_1891.cpp` | 190 | `keep` | no automatic streamline |
| `test_macro_hygiene_contract_closed_loop_420.cpp` | 120 | `keep` | no automatic streamline |
| `test_pattern_macro_filter_closed_loop_421.cpp` | 116 | `keep` | no automatic streamline |
| `test_query_hygiene_provenance_1914.cpp` | 273 | `keep` | no automatic streamline |
| `test_query_pattern_hygiene_index_sv.cpp` | 319 | `keep` | no automatic streamline |
| `test_query_pattern_hygiene_index_task1.cpp` | 330 | `keep` | no automatic streamline |
| `test_query_pattern_hygiene_mandate_1636.cpp` | 187 | `keep` | no automatic streamline |
| `test_query_pattern_hygiene_mandate_1892.cpp` | 203 | `keep` | no automatic streamline |
| `test_subtree_counter_shared_lock_1848.cpp` | 178 | `keep` | no automatic streamline |
| `test_tag_arity_index_perf.cpp` | 171 | `keep` | no automatic streamline |
| `test_workspace_marker_macro_max_1678.cpp` | 98 | `keep` | no automatic streamline |

### jit_incremental (7)

| File | Lines | Action | Notes |
|------|------:|--------|-------|
| `test_aot_hot_update_incremental.cpp` | 200 | `keep` | no automatic streamline |
| `test_aot_hotupdate_versioning.cpp` | 283 | `keep` | no automatic streamline |
| `test_hot_update_stdlib.cpp` | 181 | `keep` | no automatic streamline |
| `test_incremental_aot_closure_deps.cpp` | 352 | `keep` | no automatic streamline |
| `test_jit_metrics.cpp` | 227 | `keep` | no automatic streamline |
| `test_jit_metrics_stub.cpp` | 16 | `keep_link_stub` | link-only stub (16L) |
| `test_orchestration_steal_boundary.cpp` | 208 | `keep` | no automatic streamline |

### shape_soa (1)

| File | Lines | Action | Notes |
|------|------:|--------|-------|
| `test_shape.cpp` | 782 | `keep` | no automatic streamline |

### observability (180)

| File | Lines | Action | Notes |
|------|------:|--------|-------|
| `test_ai_closedloop_readiness_1593.cpp` | 171 | `keep` | no automatic streamline |
| `test_aot_incremental_reemit_1930.cpp` | 319 | `keep` | no automatic streamline |
| `test_aot_mangle_top.cpp` | 141 | `keep` | no automatic streamline |
| `test_aot_reload_primitive.cpp` | 186 | `keep` | no automatic streamline |
| `test_aot_stats_null_metrics_1843.cpp` | 101 | `keep` | no automatic streamline |
| `test_arena_auto_compact_fiber_defag_shape_dirty_closedloop.cpp` | 184 | `keep` | no automatic streamline |
| `test_arena_auto_compact_intelligent_1919.cpp` | 261 | `keep` | no automatic streamline |
| `test_ast_column_compaction_closed_loop_416.cpp` | 116 | `keep` | no automatic streamline |
| `test_ast_ops_stats_workspace_lock_1852.cpp` | 120 | `keep` | no automatic streamline |
| `test_atomic_batch_dispatch_1899.cpp` | 196 | `keep` | no automatic streamline |
| `test_atomic_batch_pattern_1913.cpp` | 271 | `keep` | no automatic streamline |
| `test_atomic_batch_tenant_1878.cpp` | 226 | `keep` | no automatic streamline |
| `test_bidirectional_stats.cpp` | 214 | `keep` | no automatic streamline |
| `test_blame_tracking_typed_mutate_1924.cpp` | 241 | `keep` | no automatic streamline |
| `test_build_kv_hash_compile07_1844.cpp` | 95 | `keep` | no automatic streamline |
| `test_build_kv_hash_dedup_1787.cpp` | 97 | `keep` | no automatic streamline |
| `test_capability_effects_enforcement.cpp` | 252 | `keep` | no automatic streamline |
| `test_closedloop_stats_hash_cap_1795.cpp` | 102 | `candidate_obs_fold` | thin schema probe (102L) — consider FieldListCase |
| `test_closure_bridge_lifetime_1929.cpp` | 266 | `keep` | no automatic streamline |
| `test_closure_view_lifetime_1888.cpp` | 160 | `keep` | no automatic streamline |
| `test_closure_view_uaf_guard_1926.cpp` | 210 | `keep` | no automatic streamline |
| `test_commercial_production_readiness_closed_loop_634.cpp` | 104 | `keep` | no automatic streamline |
| `test_compile_primitive_guard_1896.cpp` | 210 | `keep` | no automatic streamline |
| `test_compiler_core_incremental_selfmod_gaps.cpp` | 137 | `keep` | no automatic streamline |
| `test_compiler_metrics_ownership_1835.cpp` | 87 | `keep` | no automatic streamline |
| `test_compiler_runtime_production_readiness_closed_loop_441.cpp` | 110 | `keep` | no automatic streamline |
| `test_compiler_service_ownership_1839.cpp` | 87 | `keep` | no automatic streamline |
| `test_concept_constraints.cpp` | 161 | `keep` | no automatic streamline |
| `test_constraintsystem_solve_delta_clean_conflict_detection.cpp` | 204 | `keep` | no automatic streamline |
| `test_cpp26_contracts_hotpath_1620.cpp` | 146 | `keep` | no automatic streamline |
| `test_cpp26_contracts_hotpath_arena_soa_value_shape_pass.cpp` | 122 | `keep` | no automatic streamline |
| `test_dead_coercion_elision_narrow_evidence.cpp` | 156 | `keep` | no automatic streamline |
| `test_dead_coercion_elision_narrow_mutation_1925.cpp` | 261 | `keep` | no automatic streamline |
| `test_defuse_version_closed_loop_419.cpp` | 117 | `keep` | no automatic streamline |
| `test_dirty_propagation_cost_closed_loop_408.cpp` | 99 | `keep` | no automatic streamline |
| `test_dirty_reason_verification_propagation_415.cpp` | 94 | `keep` | no automatic streamline |
| `test_eda_production_infra.cpp` | 184 | `keep` | no automatic streamline |
| `test_eda_sv_verification_closedloop_stress.cpp` | 199 | `keep` | no automatic streamline |
| `test_edsl_query_mutate_commercial_closed_loop_636.cpp` | 108 | `keep` | no automatic streamline |
| `test_engine_metrics_facade.cpp` | 149 | `keep` | no automatic streamline |
| `test_envframe_bridge_invalidate_1916.cpp` | 281 | `keep` | no automatic streamline |
| `test_envframe_dualpath_stale_closed_loop_418.cpp` | 115 | `keep` | no automatic streamline |
| `test_envframe_resolve_distinction_1890.cpp` | 191 | `keep` | no automatic streamline |
| `test_envframe_truncate_epoch_1889.cpp` | 205 | `keep` | no automatic streamline |
| `test_epoch_apply_mandate_1632.cpp` | 284 | `keep` | no automatic streamline |
| `test_eval_relower_hotpath_1623.cpp` | 200 | `keep` | no automatic streamline |
| `test_fiber_macro_hygiene_refresh_1612.cpp` | 166 | `keep` | no automatic streamline |
| `test_fiber_resume_batch.cpp` | 472 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_fine_dirty_relower_1915.cpp` | 225 | `keep` | no automatic streamline |
| `test_followup_smoke.cpp` | 93 | `keep` | no automatic streamline |
| `test_guard_panic_reflect_fiber_resume_task6.cpp` | 133 | `keep` | no automatic streamline |
| `test_highperf_cpp26_gaps_arena_soa_value_shape_pass.cpp` | 130 | `keep` | no automatic streamline |
| `test_incremental_effectiveness_snapshot_fail_1854.cpp` | 124 | `keep` | no automatic streamline |
| `test_incremental_perblock_closure_bridge_safety.cpp` | 134 | `keep` | no automatic streamline |
| `test_incremental_relower_batch.cpp` | 575 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_inline_pass_stats_atomic_1827.cpp` | 117 | `keep` | no automatic streamline |
| `test_inner_steal_starvation_1633.cpp` | 235 | `keep` | no automatic streamline |
| `test_invalidate_consistency_1627.cpp` | 237 | `keep` | no automatic streamline |
| `test_invalidations_stats_workspace_lock_1851.cpp` | 114 | `keep` | no automatic streamline |
| `test_ir_metadata_interpreter_jit_closed_loop_403.cpp` | 107 | `keep` | no automatic streamline |
| `test_ir_soa_dual_emit_flag_1629.cpp` | 184 | `keep` | no automatic streamline |
| `test_ir_soa_incremental_closed_loop_404.cpp` | 105 | `keep` | no automatic streamline |
| `test_ir_soa_phase2_adoption_1920.cpp` | 228 | `keep` | no automatic streamline |
| `test_jit_consistency.cpp` | 297 | `keep` | no automatic streamline |
| `test_jit_critical_coverage_1917.cpp` | 221 | `keep` | no automatic streamline |
| `test_jit_full_opcode_coverage_1658.cpp` | 172 | `keep` | no automatic streamline |
| `test_linear_boundary_consistency_1568.cpp` | 231 | `keep` | no automatic streamline |
| `test_linear_live_closure_walk_1895.cpp` | 221 | `keep` | no automatic streamline |
| `test_linear_ownership_batch.cpp` | 1139 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_linear_ownership_occurrence_predicate_mutate.cpp` | 143 | `keep` | no automatic streamline |
| `test_linear_walk_active_closures_1928.cpp` | 246 | `keep` | no automatic streamline |
| `test_list_vector_soa_hotpath_ai_loops.cpp` | 179 | `keep` | no automatic streamline |
| `test_longrunning_infra_primitives.cpp` | 165 | `keep` | no automatic streamline |
| `test_longrunning_recovery_latency.cpp` | 166 | `keep` | no automatic streamline |
| `test_lookup_stats_impl_heterogeneous_1671.cpp` | 103 | `keep` | no automatic streamline |
| `test_macro_hygiene_closedloop_health_1613.cpp` | 165 | `keep` | no automatic streamline |
| `test_module_export_cache_1680.cpp` | 123 | `candidate_obs_fold` | thin schema probe (123L) — consider FieldListCase |
| `test_mutation_audit_wal.cpp` | 259 | `keep` | no automatic streamline |
| `test_mutation_boundary_guard_1931.cpp` | 226 | `keep` | no automatic streamline |
| `test_mutation_guard_try_acquire_1628.cpp` | 216 | `keep` | no automatic streamline |
| `test_mutation_hold_time.cpp` | 172 | `keep` | no automatic streamline |
| `test_observability_tier_table_1670.cpp` | 116 | `keep` | no automatic streamline |
| `test_occ_cache_stats_wired_1781.cpp` | 103 | `keep` | no automatic streamline |
| `test_optimization_passes_contracts.cpp` | 234 | `keep` | no automatic streamline |
| `test_orchestration_steal_boost.cpp` | 75 | `keep` | no automatic streamline |
| `test_pass_contracts_hotpath_closed_loop_406.cpp` | 106 | `keep` | no automatic streamline |
| `test_pattern_structural_index_closed_loop_423.cpp` | 130 | `keep` | no automatic streamline |
| `test_persist_basic.cpp` | 180 | `keep` | no automatic streamline |
| `test_post_steal_closed_loop_1592.cpp` | 168 | `keep` | no automatic streamline |
| `test_primitive_meta_self_describing_closed_loop_480.cpp` | 101 | `candidate_obs_fold` | thin schema probe (101L) — consider FieldListCase |
| `test_primitive_resource_quota_stats.cpp` | 127 | `candidate_obs_fold` | thin schema probe (127L) — consider FieldListCase |
| `test_primitives_capture_contract.cpp` | 114 | `candidate_obs_fold` | thin schema probe (114L) — consider FieldListCase |
| `test_primitives_hotpath_registry_slo.cpp` | 109 | `candidate_obs_fold` | thin schema probe (109L) — consider FieldListCase |
| `test_primitives_registry_core_consistency_583.cpp` | 115 | `keep` | no automatic streamline |
| `test_primitives_surface_convergence.cpp` | 145 | `keep` | no automatic streamline |
| `test_production_hardening_1072_1096.cpp` | 103 | `fold_obs_fieldlist` | pure schema/flag production sweep → FieldListCase |
| `test_production_hardening_985_1013.cpp` | 91 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_production_roadmap_closed_loop_520.cpp` | 115 | `keep` | no automatic streamline |
| `test_production_safety_1047_1071.cpp` | 94 | `fold_obs_fieldlist` | pure schema/flag production sweep → FieldListCase |
| `test_production_safety_1097_1122.cpp` | 94 | `fold_obs_fieldlist` | pure schema/flag production sweep → FieldListCase |
| `test_production_stability_1014_1046.cpp` | 119 | `keep` | no automatic streamline |
| `test_production_sweep_1123_1140.cpp` | 100 | `fold_obs_fieldlist` | pure schema/flag production sweep → FieldListCase |
| `test_production_sweep_1144_1148.cpp` | 78 | `fold_obs_fieldlist` | pure schema/flag production sweep → FieldListCase |
| `test_production_sweep_1158_1176.cpp` | 100 | `fold_obs_fieldlist` | pure schema/flag production sweep → FieldListCase |
| `test_production_sweep_1177_1201.cpp` | 87 | `fold_obs_fieldlist` | pure schema/flag production sweep → FieldListCase |
| `test_production_sweep_1229_1240.cpp` | 87 | `fold_obs_fieldlist` | pure schema/flag production sweep → FieldListCase |
| `test_production_sweep_1241_1245.cpp` | 82 | `fold_obs_fieldlist` | pure schema/flag production sweep → FieldListCase |
| `test_production_sweep_1246_1250.cpp` | 72 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_production_sweep_1256_1260.cpp` | 86 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_production_sweep_1261_1265.cpp` | 97 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_production_sweep_1266_1270.cpp` | 93 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_production_sweep_1271_1275.cpp` | 75 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_production_sweep_1276_1280.cpp` | 112 | `fold_obs_fieldlist` | pure schema/flag production sweep → FieldListCase |
| `test_production_sweep_1281_1285.cpp` | 108 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_production_sweep_1286_1290.cpp` | 105 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_production_sweep_1291_1295.cpp` | 137 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_production_sweep_1296_1300.cpp` | 163 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_production_sweep_1301_1305.cpp` | 105 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_production_sweep_1306_1310.cpp` | 104 | `fold_obs_fieldlist` | pure schema/flag production sweep → FieldListCase |
| `test_production_sweep_1311_1315.cpp` | 96 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_production_sweep_1316_1320.cpp` | 133 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_production_sweep_1321_1324.cpp` | 101 | `fold_obs_fieldlist` | pure schema/flag production sweep → FieldListCase |
| `test_production_sweep_1325_1330.cpp` | 113 | `fold_obs_fieldlist` | pure schema/flag production sweep → FieldListCase |
| `test_production_sweep_1331_1343.cpp` | 142 | `keep_behavioral` | production family with behavioral extras (batch later) |
| `test_prompt6_epoch_atomic_visibility_fiber_steal.cpp` | 180 | `keep` | no automatic streamline |
| `test_prompt6_linear_jit_l2_post_invalidate_arena_gc.cpp` | 109 | `keep` | no automatic streamline |
| `test_query_pattern_hygiene_1609.cpp` | 157 | `keep` | no automatic streamline |
| `test_query_pattern_hygiene_macrointroduced.cpp` | 315 | `keep` | no automatic streamline |
| `test_raw_pointer_safety_1898.cpp` | 203 | `keep` | no automatic streamline |
| `test_render_dispatch_linear_epoch_1676.cpp` | 158 | `keep` | no automatic streamline |
| `test_render_ffi_hotpath.cpp` | 245 | `keep` | no automatic streamline |
| `test_render_hotpath_observability_1674.cpp` | 150 | `keep` | no automatic streamline |
| `test_render_hotpath_stability_under_mutation.cpp` | 173 | `keep` | no automatic streamline |
| `test_render_mutation_checkpoint.cpp` | 131 | `keep` | no automatic streamline |
| `test_render_pass_incremental.cpp` | 239 | `keep` | no automatic streamline |
| `test_render_telemetry.cpp` | 151 | `keep` | no automatic streamline |
| `test_resource_quota_hotpath.cpp` | 229 | `keep` | no automatic streamline |
| `test_resource_quota_manager_1618.cpp` | 229 | `keep` | no automatic streamline |
| `test_resource_quota_wired.cpp` | 245 | `keep` | no automatic streamline |
| `test_runtime_concurrent_full_cycle_chaos.cpp` | 152 | `keep` | no automatic streamline |
| `test_runtime_observability_correlated_stats_673.cpp` | 142 | `candidate_obs_fold` | thin schema probe (142L) — consider FieldListCase |
| `test_safe_snapshot_umbrella_1856.cpp` | 123 | `keep` | no automatic streamline |
| `test_safe_yield_orchestration_1635.cpp` | 187 | `keep` | no automatic streamline |
| `test_sandbox_capability_enforce_1876.cpp` | 226 | `keep` | no automatic streamline |
| `test_scheduler_llm_bottleneck_adaptive_steal_gc.cpp` | 147 | `keep` | no automatic streamline |
| `test_self_evo_stats_1909.cpp` | 176 | `keep` | no automatic streamline |
| `test_self_evolution_loop_stats_1883.cpp` | 113 | `keep` | no automatic streamline |
| `test_selfevo_bugfix_941_967.cpp` | 96 | `candidate_obs_fold` | thin schema probe (96L) — consider FieldListCase |
| `test_shape_jit_pass_deopt_incremental_closedloop_ai_mutate.cpp` | 136 | `keep` | no automatic streamline |
| `test_shape_linear_collaborative_pass_1661.cpp` | 214 | `keep` | no automatic streamline |
| `test_shape_profiler_burst_closed_loop_407.cpp` | 108 | `keep` | no automatic streamline |
| `test_shape_profiler_stability_deopt_fiber_task4.cpp` | 375 | `keep` | no automatic streamline |
| `test_soa_view_enforcement_1918.cpp` | 262 | `keep` | no automatic streamline |
| `test_stable_ref_cow_fiber_closed_loop_527.cpp` | 118 | `keep` | no automatic streamline |
| `test_stable_ref_workspace_tree_closed_loop_424.cpp` | 132 | `keep` | no automatic streamline |
| `test_static_reflect_selfmod_validation_task6_594.cpp` | 114 | `keep` | no automatic streamline |
| `test_stats_catalog_drift_1672.cpp` | 113 | `keep` | no automatic streamline |
| `test_stats_facade_bench.cpp` | 86 | `candidate_obs_fold` | thin schema probe (86L) — consider FieldListCase |
| `test_stats_module_unification.cpp` | 305 | `keep` | no automatic streamline |
| `test_stdlib_production_review_923_940.cpp` | 85 | `candidate_obs_fold` | thin schema probe (85L) — consider FieldListCase |
| `test_task6_production_readiness_closed_loop_514.cpp` | 137 | `keep` | no automatic streamline |
| `test_tenant_isolation_enforcement.cpp` | 227 | `keep` | no automatic streamline |
| `test_terminal_deprecation.cpp` | 138 | `candidate_obs_fold` | thin schema probe (138L) — consider FieldListCase |
| `test_terminal_render_production_1673.cpp` | 164 | `keep` | no automatic streamline |
| `test_test_strategy_1887.cpp` | 141 | `keep` | no automatic streamline |
| `test_tier_dispatch.cpp` | 114 | `candidate_obs_fold` | thin schema probe (114L) — consider FieldListCase |
| `test_type_cache_stats_snapshot_1797.cpp` | 134 | `keep` | no automatic streamline |
| `test_type_prop_invariant_correlation_1884.cpp` | 160 | `keep` | no automatic streamline |
| `test_typesystem_type_propagation_jit_l2_typed_mutate.cpp` | 150 | `keep` | no automatic streamline |
| `test_unified_invalidation_1607.cpp` | 215 | `keep` | no automatic streamline |
| `test_value_encoding_v2_dispatch_contracts_1622.cpp` | 178 | `keep` | no automatic streamline |
| `test_value_encoding_v2_dispatch_contracts_task4.cpp` | 383 | `keep` | no automatic streamline |
| `test_walk_batch.cpp` | 393 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_zero_copy_arena.cpp` | 233 | `keep` | no automatic streamline |

### stdlib (5)

| File | Lines | Action | Notes |
|------|------:|--------|-------|
| `test_atomic_swap_stdlib.cpp` | 144 | `keep` | no automatic streamline |
| `test_datetime_date_string_1910.cpp` | 137 | `keep` | no automatic streamline |
| `test_datetime_shadow_1911.cpp` | 148 | `keep` | no automatic streamline |
| `test_spec_runtime.cpp` | 183 | `keep` | no automatic streamline |
| `test_synthesize_namespace_demotion.cpp` | 258 | `keep` | no automatic streamline |

### compiler_core (58)

| File | Lines | Action | Notes |
|------|------:|--------|-------|
| `test_arithmetic_int64_safety.cpp` | 65 | `keep` | no automatic streamline |
| `test_ast_workspace_modules.cpp` | 211 | `keep` | no automatic streamline |
| `test_aura_result_error_policy.cpp` | 108 | `keep` | no automatic streamline |
| `test_auto_evolve_closure_live_1713.cpp` | 158 | `keep` | no automatic streamline |
| `test_bidirectional_annotation.cpp` | 124 | `keep` | no automatic streamline |
| `test_bridge_epoch_strict.cpp` | 177 | `keep` | no automatic streamline |
| `test_bugfix_968_984.cpp` | 65 | `keep` | no automatic streamline |
| `test_closure_free.cpp` | 179 | `keep` | no automatic streamline |
| `test_commit_panic_bridge_epoch_1728.cpp` | 114 | `keep` | no automatic streamline |
| `test_compile02_no_dup_imports_1857.cpp` | 116 | `keep` | no automatic streamline |
| `test_compiler_closure_env_safety_post_invalidate.cpp` | 286 | `keep` | no automatic streamline |
| `test_compiler_root_epoch_gc_safety_post_invalidate.cpp` | 121 | `keep` | no automatic streamline |
| `test_consolidated_production_priority_517.cpp` | 102 | `keep` | no automatic streamline |
| `test_contracts.cpp` | 87 | `keep` | no automatic streamline |
| `test_coverage_holes_workspace_lock_1816.cpp` | 108 | `keep` | no automatic streamline |
| `test_env_batch.cpp` | 723 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_env_lookup_batch.cpp` | 289 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_envframe_stableid.cpp` | 190 | `keep` | no automatic streamline |
| `test_envframe_truncate_guard_1948.cpp` | 175 | `keep` | no automatic streamline |
| `test_gc_evaluator_integration.cpp` | 247 | `keep` | no automatic streamline |
| `test_hash_iter_invalidation.cpp` | 134 | `keep` | no automatic streamline |
| `test_inline_pass_stats_unpack_1784.cpp` | 145 | `keep` | no automatic streamline |
| `test_inline_typecheck_exception_1769.cpp` | 116 | `keep` | no automatic streamline |
| `test_jit_closure_cache_race_1707.cpp` | 155 | `keep` | no automatic streamline |
| `test_lock_hierarchy.cpp` | 312 | `keep` | no automatic streamline |
| `test_matcher_stable_captures_1695.cpp` | 125 | `keep` | no automatic streamline |
| `test_module_loader_dead_heap_circular_1692.cpp` | 114 | `keep` | no automatic streamline |
| `test_module_prefix_dead_heap_1693.cpp` | 119 | `keep` | no automatic streamline |
| `test_pair_unchecked_safety_1710.cpp` | 123 | `keep` | no automatic streamline |
| `test_panic_checkpoint_raii.cpp` | 198 | `keep` | no automatic streamline |
| `test_per_symbol_dirty_cycle_guard_1786.cpp` | 179 | `keep` | no automatic streamline |
| `test_per_symbol_dirty_pool_lock_1785.cpp` | 99 | `keep` | no automatic streamline |
| `test_prim_call_count_clamp_1711.cpp` | 116 | `keep` | no automatic streamline |
| `test_prompt2_6_impact_scope_quote_lambda_bridge_env.cpp` | 169 | `keep` | no automatic streamline |
| `test_prompt6_full_memory_safety_fuzz_stress.cpp` | 418 | `keep` | no automatic streamline |
| `test_propagate_marker_cycle_guard_1782.cpp` | 139 | `keep` | no automatic streamline |
| `test_query_dispatch.cpp` | 119 | `keep` | no automatic streamline |
| `test_query_pattern_concurrent.cpp` | 189 | `keep` | no automatic streamline |
| `test_relower_strategy_cache_lock_1855.cpp` | 130 | `keep` | no automatic streamline |
| `test_render_ai_native_template_1677.cpp` | 146 | `keep` | no automatic streamline |
| `test_resolve_env_frame_detailed_1756.cpp` | 125 | `keep` | no automatic streamline |
| `test_set_workspace_flat_1729.cpp` | 156 | `keep` | no automatic streamline |
| `test_shapeprofiler_stability_deopt_jit_mutate.cpp` | 342 | `keep` | no automatic streamline |
| `test_soa_batch.cpp` | 920 | `keep_batch_exclude` | family batch driver (EXCLUDE_FROM_ALL convention) |
| `test_stdlib_infrastructure.cpp` | 244 | `keep` | no automatic streamline |
| `test_task4_highperf_full_hotpath_matrix.cpp` | 433 | `keep` | no automatic streamline |
| `test_terminal_ansi_emit.cpp` | 155 | `keep` | no automatic streamline |
| `test_terminal_input.cpp` | 186 | `keep` | no automatic streamline |
| `test_terminal_rgb.cpp` | 154 | `keep` | no automatic streamline |
| `test_truncate_env_bridge_epoch_1739.cpp` | 129 | `keep` | no automatic streamline |
| `test_try_lock_workspace_lock_order_1768.cpp` | 126 | `keep` | no automatic streamline |
| `test_type_registry_ownership_1837.cpp` | 95 | `keep` | no automatic streamline |
| `test_verify_parse_shared_helper_1771.cpp` | 108 | `keep` | no automatic streamline |
| `test_workspace_delete_child_1770.cpp` | 110 | `keep` | no automatic streamline |
| `test_workspace_dispatch.cpp` | 130 | `keep` | no automatic streamline |
| `test_workspace_swap_guard_1717.cpp` | 114 | `keep` | no automatic streamline |

### uncategorized (2)

| File | Lines | Action | Notes |
|------|------:|--------|-------|
| `test_harness_pilot.cpp` | 20 | `keep` | no automatic streamline |
| `test_primitives_init.cpp` | 31 | `keep` | no automatic streamline |

## Streamline roadmap (historical)

Waves 0–4 applied — see **Wave status** above. Further reductions:
fold remaining `candidate_obs_fold` keepers; rename `domain/test_issue_*`
to `test_domain_<theme>_*.cpp`; promote more root keeps into theme suites.

## Related

- Policy: [`tests/README.md`](README.md)
- Domain rules: [`domain/README.md`](domain/README.md)
- Issues inventory: [`legacy_test_inventory.md`](legacy_test_inventory.md)
- Coarse 5-bucket map: [`domain_classification.md`](domain_classification.md)

