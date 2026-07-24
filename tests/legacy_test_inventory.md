# Legacy test inventory

**Issue:** [#1957](https://github.com/cybrid-systems/aura/issues/1957)
**Generated:** 2026-07-24 by `scripts/inventory_legacy_tests.py`
**Status:** living document — re-run the script after consolidations.

## Purpose

Categorize legacy per-issue regression tests so we can migrate them in batches into the preferred `tests/domain/` structure (and existing family batch drivers under `tests/test_*_batch.cpp`).

`tests/issues/` **removed** (#1957). Prefer theme/domain batches; do not reintroduce per-issue mains.

## Scope snapshot

| Location | Count | Notes |
|----------|------:|-------|
| `tests/issues/test_issue_*.cpp` | 0 | Legacy per-issue mains / bundle members |
| `tests/test_*.cpp` (issue-oriented) | 0 | Numbered root tests + `*_batch` drivers |
| `tests/domain/test_*.cpp` | 182 | Preferred destination suites |
| **Total scanned** | **182** | |

### Related artifacts

- Coarser 5-bucket Phase-2 map: [`tests/domain_classification.md`](domain_classification.md) (`scripts/classify_test_issues.py`)
- Link/bundle profiles: [`tests/fixtures/issue_link_profiles.json`](fixtures/issue_link_profiles.json)
- Domain CMake: [`cmake/AuraDomainTests.cmake`](../cmake/AuraDomainTests.cmake)
- Test layout rules: [`tests/README.md`](README.md)

## Theme buckets (8 + uncategorized)

Classification uses the **filename + first 50 lines** (keywords and filename token boosts). Ties break toward earlier themes in the priority order below.

| Theme | Title | Issues | Root | Domain | Total | Migration priority |
|-------|-------|-------:|-----:|-------:|------:|--------------------|
| `arena_compaction` | Arena / compaction / GC | 0 | 0 | 23 | 23 | P0 — well-contained, batch drivers already exist |
| `mutation_dirty` | Mutation / dirty propagation / provenance | 0 | 0 | 58 | 58 | P0 — high volume; strong domain suite foothold |
| `fiber_orch` | Fiber / orchestration / steal / Guard | 0 | 0 | 28 | 28 | P1 — domain suite already collapses many obs gates |
| `linear_ownership` | Linear ownership / borrow / consume | 0 | 0 | 1 | 1 | P1 — small, already partially batched |
| `edsl_hygiene` | EDSL / macro hygiene / reflect | 0 | 0 | 12 | 12 | P1 — domain hygiene suite exists |
| `jit_incremental` | JIT / AOT / incremental relower | 0 | 0 | 11 | 11 | P2 — link-profile heavy; migrate AC smoke first |
| `shape_soa` | Shape / SoA / column layout | 0 | 0 | 8 | 8 | P2 — small-medium; soa_batch precedent |
| `observability` | Observability / metrics / query:*-stats | 0 | 0 | 20 | 20 | P2 — often thin schema probes; collapse into obs matrix |
| `uncategorized` | Uncategorized / mixed | 0 | 0 | 21 | 21 | P3 — review case-by-case |

## Patterns, harness usage, coupling

### Harness / entry-point patterns (`tests/issues/` only)

| Pattern | Count | Meaning |
|---------|------:|---------|

### `@category` distribution (issues/)


### Top includes (first 50 lines, issues/)


### Top module imports (first 50 lines, issues/)


### Coupling notes

1. **CompilerService-heavy** (~majority of issues/): most legacy tests are integration-style closed loops (eval → mutate → query stats). Domain migration should keep a shared CS fixture, not re-copy setup.
2. **Observability dual-path**: many files named `*_observability.cpp` or probing `query:*-stats` / `engine:metrics`. Prefer folding into `tests/domain/cases/obs_schema_cases.hpp` + `test_obs_schema_matrix.cpp`.
3. **Bundle link profiles** (`light` / `jit` / `fiber` / `*_late*`): physical file location still `tests/issues/`; migration must update `issue_link_profiles.json` / CMake when deleting sources.
4. **Internal headers**: direct includes of `compiler/observability_metrics.h`, `serve/fiber.h`, `compiler/aura_jit*.h` couple tests to private surfaces — domain suites should prefer public query/primitives where possible.
5. **Existing consolidation path**: family `*_batch.cpp` drivers under `tests/` (listed in `AuraDomainTests.cmake`) are the intermediate step; domain suites are the long-term home.

## Multi-file issues, phase slices, low-value signals

- Issue numbers with **multiple** `tests/issues/` files: **0**
- Phase-slice files (`*_phase*`): **0**
- Small files (< 4 KiB, possible thin probes): **0**
- Existing `*_batch` drivers (migration milestones): **42**

### Multi-file issue groups (consolidate first)


### Smallest issue tests (triage for obs-matrix fold or drop)


### Batch drivers already present

- `tests/core/test_arena_batch.cpp` → theme `arena_compaction`
- `tests/compiler/test_atomic_batch_metadata.cpp` → theme `mutation_dirty`
- `tests/compiler/test_atomic_batch_rollback_closed_loop.cpp` → theme `mutation_dirty`
- `tests/compiler/test_atomic_batch_rollback_fiber_task1.cpp` → theme `mutation_dirty`
- `tests/compiler/test_atomic_batch_snapshot_stable_ref_ai_loops.cpp` → theme `mutation_dirty`
- `tests/compiler/test_closure_batch.cpp` → theme `arena_compaction`
- `tests/compiler/test_dead_coercion_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_env_batch.cpp` → theme `arena_compaction`
- `tests/compiler/test_env_lookup_batch.cpp` → theme `fiber_orch`
- `tests/compiler/test_envframe_epoch_batch.cpp` → theme `arena_compaction`
- `tests/serve/test_fiber_concurrent_unit_batch.cpp` → theme `fiber_orch`
- `tests/serve/test_fiber_integration_batch.cpp` → theme `fiber_orch`
- `tests/serve/test_fiber_orch_core_batch.cpp` → theme `fiber_orch`
- `tests/serve/test_fiber_orch_parallel_quota_batch.cpp` → theme `fiber_orch`
- `tests/serve/test_fiber_strategy_evolve_batch.cpp` → theme `fiber_orch`
- `tests/serve/test_fiber_synthesize_batch.cpp` → theme `fiber_orch`
- `tests/serve/test_gc_batch.cpp` → theme `arena_compaction`
- `tests/serve/test_gc_compact_batch.cpp` → theme `arena_compaction`
- `tests/serve/test_gc_compact_sweep_batch.cpp` → theme `arena_compaction`
- `tests/core/test_guard_dtor_batch_metrics.cpp` → theme `mutation_dirty`
- `tests/core/test_hotpath_matrix_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_incremental_type_batch.cpp` → theme `jit_incremental`
- `tests/compiler/test_issues_809_817_batch.cpp` → theme `fiber_orch`
- `tests/compiler/test_issues_819_829_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_jit_batch_deopt_clear.cpp` → theme `jit_incremental`
- `tests/compiler/test_linear_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_macro_reflect_batch.cpp` → theme `edsl_hygiene`
- `tests/compiler/test_mutate_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_mutation_aot_unit_batch.cpp` → theme `observability`
- `tests/compiler/test_mutation_boundary_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_mutation_guard_unit_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_mutation_occurrence_dirty_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_mutation_typed_audit_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_open_issues_phase1_batch.cpp` → theme `uncategorized`
- `tests/compiler/test_per_defuse_batch.cpp` → theme `fiber_orch`
- `tests/compiler/test_reflect_batch.cpp` → theme `edsl_hygiene`
- `tests/reflect/test_reflect_hygiene_unit_batch.cpp` → theme `edsl_hygiene`
- `tests/reflect/test_reflect_macro_hygiene_batch.cpp` → theme `edsl_hygiene`
- `tests/reflect/test_reflect_pattern_hygiene_batch.cpp` → theme `edsl_hygiene`
- `tests/core/test_soa_batch.cpp` → theme `shape_soa`
- `tests/compiler/test_stable_ref_cow_batch.cpp` → theme `mutation_dirty`
- `tests/repl/test_terminal_domain_batch.cpp` → theme `uncategorized`

### Domain suites (do not regress; extend these)

- `tests/compiler/test_adt_match_exhaust_post_mutate_reliability.cpp`
- `tests/compiler/test_adt_match_exhaustiveness_incremental_task2.cpp`
- `tests/compiler/test_aot_bridge_checkpoint_version_steal.cpp`
- `tests/compiler/test_aot_region_per_eval.cpp`
- `tests/compiler/test_aot_shell_c0_escape.cpp`
- `tests/compiler/test_apply_closure_envframe_soa.cpp`
- `tests/core/test_arena_batch.cpp`
- `tests/core/test_arena_compact_hook_concurrent.cpp`
- `tests/core/test_arena_concurrent_mutex.cpp`
- `tests/core/test_arena_defrag.cpp`
- `tests/compiler/test_arithmetic_int64_safety.cpp`
- `tests/compiler/test_ast_workspace_modules.cpp`
- `tests/compiler/test_atomic_batch_metadata.cpp`
- `tests/compiler/test_atomic_batch_rollback_closed_loop.cpp`
- `tests/compiler/test_atomic_batch_rollback_fiber_task1.cpp`
- `tests/compiler/test_atomic_batch_snapshot_stable_ref_ai_loops.cpp`
- `tests/stdlib/test_atomic_swap_stdlib.cpp`
- `tests/compiler/test_aura_result_error_policy.cpp`
- `tests/compiler/test_auto_evolve_closure_live.cpp`
- `tests/compiler/test_bidirectional_annotation.cpp`
- `tests/compiler/test_bugfix_968.cpp`
- `tests/compiler/test_capability_gating.cpp`
- `tests/compiler/test_closure_batch.cpp`
- `tests/compiler/test_closure_free.cpp`
- `tests/compiler/test_coercion_dead_elim_castop_flow_zerooverhead.cpp`
- `tests/compiler/test_compile02_no_dup_imports.cpp`
- `tests/compiler/test_compiler_closure_env_safety_post_invalidate.cpp`
- `tests/core/test_compiler_root_epoch_gc_safety_post_invalidate.cpp`
- `tests/compiler/test_composite_typed_mutate.cpp`
- `tests/serve/test_concurrent.cpp`
- `tests/core/test_consolidated_production_priority.cpp`
- `tests/compiler/test_constraint_system_solve_delta_cross_delta_task2.cpp`
- `tests/core/test_contracts.cpp`
- `tests/compiler/test_core_builtins_review.cpp`
- `tests/core/test_coverage_holes_workspace_lock.cpp`
- `tests/stdlib/test_datetime.cpp`
- `tests/compiler/test_dead_coercion_batch.cpp`
- `tests/core/test_dep_graph_concurrent.cpp`
- `tests/compiler/test_dirty_delta_present.cpp`
- `tests/compiler/test_dirty_propagation_cascade.cpp`
- `tests/compiler/test_edsl_concurrent_fiber_boundary_task1.cpp`
- `tests/compiler/test_edsl_concurrent_query_mutate.cpp`
- `tests/compiler/test_edsl_core_stability_cow_atomic_query_mutate.cpp`
- `tests/compiler/test_env_batch.cpp`
- `tests/compiler/test_env_lookup_batch.cpp`
- `tests/compiler/test_envframe_epoch_batch.cpp`
- `tests/core/test_envframe_truncate_guard_dual_epoch.cpp`
- `tests/compiler/test_epoch_apply_hotpath.cpp`
- `tests/reflect/test_error_merr.cpp`
- `tests/serve/test_fiber_concurrent_unit_batch.cpp`
- `tests/serve/test_fiber_integration_batch.cpp`
- `tests/serve/test_fiber_mutation_steal_safety.cpp`
- `tests/serve/test_fiber_orch_core_batch.cpp`
- `tests/serve/test_fiber_orch_parallel_quota_batch.cpp`
- `tests/serve/test_fiber_steal_panic_checkpoint_nested_gc.cpp`
- `tests/serve/test_fiber_strategy_evolve_batch.cpp`
- `tests/serve/test_fiber_synthesize_batch.cpp`
- `tests/compiler/test_fine_dirty_relower.cpp`
- `tests/compiler/test_followups.cpp`
- `tests/serve/test_gc_batch.cpp`
- `tests/serve/test_gc_compact_batch.cpp`
- `tests/serve/test_gc_compact_sweep_batch.cpp`
- `tests/core/test_gc_evaluator_integration.cpp`
- `tests/core/test_guard_dtor_batch_metrics.cpp`
- `tests/core/test_hash_iter_invalidation.cpp`
- `tests/core/test_hotpath_matrix_batch.cpp`
- `tests/compiler/test_incremental_type_batch.cpp`
- `tests/compiler/test_incremental_typed_selfmod_dirty_narrowing.cpp`
- `tests/compiler/test_inline_pass_stats_unpack.cpp`
- `tests/compiler/test_inline_typecheck_exception.cpp`
- `tests/compiler/test_invalidate_cascade_order.cpp`
- `tests/reflect/test_ir_cache_v2.cpp`
- `tests/compiler/test_ir_soa_dual_emit.cpp`
- `tests/reflect/test_issue_178.cpp`
- `tests/reflect/test_issue_178_reflect.cpp`
- `tests/serve/test_issue_1990.cpp`
- `tests/serve/test_issue_1991.cpp`
- `tests/serve/test_issue_1992.cpp`
- `tests/serve/test_issue_1993.cpp`
- `tests/core/test_issue_1994.cpp`
- `tests/compiler/test_issues_809_817_batch.cpp`
- `tests/compiler/test_issues_819_829_batch.cpp`
- `tests/compiler/test_jit_batch_deopt_clear.cpp`
- `tests/compiler/test_jit_closure_cache_race.cpp`
- `tests/compiler/test_jit_concurrent_compile.cpp`
- `tests/compiler/test_let_poly_solve_delta.cpp`
- `tests/compiler/test_linear_batch.cpp`
- `tests/compiler/test_linear_ownership_postmutate_guard_steal_envframe.cpp`
- `tests/core/test_lock_hierarchy.cpp`
- `tests/compiler/test_lock_order_closures_env.cpp`
- `tests/compiler/test_macro_hygiene_fiber_panic_aot_soa_self_evo.cpp`
- `tests/compiler/test_macro_reflect_batch.cpp`
- `tests/core/test_marker_metadata_lock.cpp`
- `tests/compiler/test_matcher_stable_captures.cpp`
- `tests/core/test_module_boundary.cpp`
- `tests/compiler/test_module_loader_dead_heap_circular.cpp`
- `tests/compiler/test_module_prefix_dead_heap.cpp`
- `tests/compiler/test_mutate_batch.cpp`
- `tests/compiler/test_mutate_cross_thread_migration.cpp`
- `tests/compiler/test_mutation_aot_unit_batch.cpp`
- `tests/compiler/test_mutation_boundary_batch.cpp`
- `tests/compiler/test_mutation_guard_unit_batch.cpp`
- `tests/compiler/test_mutation_log_query_race.cpp`
- `tests/compiler/test_mutation_occurrence_dirty_batch.cpp`
- `tests/compiler/test_mutation_provenance.cpp`
- `tests/compiler/test_mutation_rollback_coverage.cpp`
- `tests/compiler/test_mutation_systemic_guard.cpp`
- `tests/compiler/test_mutation_typed_audit_batch.cpp`
- `tests/compiler/test_mutator_dispatch_stats_lock.cpp`
- `tests/compiler/test_obs_schema_matrix.cpp`
- `tests/compiler/test_occurrence_dirty_blame_post_mutate.cpp`
- `tests/compiler/test_occurrence_mutate_narrowing.cpp`
- `tests/compiler/test_occurrence_typing_blame_post_mutate_recovery.cpp`
- `tests/compiler/test_occurrence_typing_blame_post_mutate_task2.cpp`
- `tests/compiler/test_open_issues_phase1_batch.cpp`
- `tests/core/test_pair_slot_lock.cpp`
- `tests/core/test_pair_unchecked_safety.cpp`
- `tests/core/test_panic_checkpoint_clear.cpp`
- `tests/serve/test_panic_checkpoint_fiber_resume_safety.cpp`
- `tests/core/test_panic_checkpoint_raii.cpp`
- `tests/compiler/test_per_defuse_batch.cpp`
- `tests/serve/test_per_fiber_stack_pool_high_concurrency.cpp`
- `tests/compiler/test_per_symbol_dirty_cycle_guard.cpp`
- `tests/core/test_per_symbol_dirty_pool_lock.cpp`
- `tests/core/test_prim_call_count_clamp.cpp`
- `tests/serve/test_production_sweep.cpp`
- `tests/compiler/test_prompt2_6_impact_scope_quote_lambda_bridge_env.cpp`
- `tests/compiler/test_prompt6_full_memory_safety_fuzz_stress.cpp`
- `tests/compiler/test_propagate_marker_cycle_guard.cpp`
- `tests/compiler/test_provenance_blame_hygiene.cpp`
- `tests/compiler/test_query_dispatch.cpp`
- `tests/compiler/test_query_mutate_consistency.cpp`
- `tests/compiler/test_query_namespace_audit.cpp`
- `tests/compiler/test_query_pattern_concurrent.cpp`
- `tests/compiler/test_quota_edge_cases.cpp`
- `tests/compiler/test_reflect_batch.cpp`
- `tests/reflect/test_reflect_hygiene_unit_batch.cpp`
- `tests/reflect/test_reflect_macro_hygiene_batch.cpp`
- `tests/reflect/test_reflect_pattern_hygiene_batch.cpp`
- `tests/compiler/test_relower_strategy_cache_lock.cpp`
- `tests/renderer/test_render_ai_native_template.cpp`
- `tests/serve/test_runtime_mutation_boundary_steal_safety.cpp`
- `tests/serve/test_safepoint_mutation.cpp`
- `tests/compiler/test_scan_skip_freed_closures.cpp`
- `tests/serve/test_scheduler_gc_defer_pending_panic_steal.cpp`
- `tests/serve/test_scheduler_gc_safepoint_mutation_coordination.cpp`
- `tests/serve/test_self_evolution_chaos_stable.cpp`
- `tests/serve/test_self_heal_policy_engine.cpp`
- `tests/core/test_set_arena_atomic_owner.cpp`
- `tests/core/test_set_workspace_flat.cpp`
- `tests/compiler/test_seva_demo_metrics.cpp`
- `tests/compiler/test_shapeprofiler_stability_deopt_jit_mutate.cpp`
- `tests/core/test_soa_batch.cpp`
- `tests/compiler/test_spec_jit.cpp`
- `tests/stdlib/test_spec_runtime.cpp`
- `tests/compiler/test_stable_ref_cow_batch.cpp`
- `tests/compiler/test_stable_ref_cow_subworkspace_concurrent_ai.cpp`
- `tests/compiler/test_stable_ref_cross_cow_provenance_enforcement.cpp`
- `tests/compiler/test_stable_ref_full_provenance_enforcement.cpp`
- `tests/serve/test_stable_ref_provenance_fiber_cow.cpp`
- `tests/compiler/test_stable_ref_provenance_mandate.cpp`
- `tests/compiler/test_stale_closure_fallback.cpp`
- `tests/core/test_stale_ref_string_heap.cpp`
- `tests/stdlib/test_stdlib_infrastructure.cpp`
- `tests/core/test_stress_alloc_storage_lock.cpp`
- `tests/stdlib/test_synthesize_namespace_demotion.cpp`
- `tests/compiler/test_task2_refinement_closed_loop.cpp`
- `tests/core/test_task4_highperf_full_hotpath_matrix.cpp`
- `tests/renderer/test_terminal_concurrent.cpp`
- `tests/repl/test_terminal_domain_batch.cpp`
- `tests/core/test_try_lock_workspace_lock_order.cpp`
- `tests/core/test_type_registry_ownership.cpp`
- `tests/compiler/test_typechecker_incremental_guard_steal_fidelity.cpp`
- `tests/compiler/test_typechecker_incremental_locality.cpp`
- `tests/compiler/test_typesystem_solve_delta_occurrence_priority_heavy_mutate.cpp`
- `tests/compiler/test_typesystem_typed_mutate_incremental_gaps.cpp`
- `tests/compiler/test_unify_invalidate_try_acquire.cpp`
- `tests/compiler/test_verify_parse_shared_helper.cpp`
- `tests/core/test_wave1_workspace_lock_reentrancy.cpp`
- `tests/core/test_workspace_delete_child.cpp`
- `tests/core/test_workspace_dispatch.cpp`
- `tests/core/test_workspace_swap_guard.cpp`

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

### `arena_compaction` — Arena / compaction / GC (23)

**Target:** tests/domain/ (extend compact/gc family; see test_compact_*_batch)

**Priority:** P0 — well-contained, batch drivers already exist

#### domain/ (23)

- `tests/compiler/test_adt_match_exhaust_post_mutate_reliability.cpp` (—) [domain_suite, theme_compiler] — test_adt_match_exhaust_post_mutate_reliability.cpp — Issue #612:
- `tests/core/test_arena_batch.cpp` (—) [large, batch_driver, domain_suite, theme_core] — tests/domain/arena/test_arena_batch.cpp — relocated for #1959 arena pilot
- `tests/core/test_arena_compact_hook_concurrent.cpp` (—) [small, domain_suite, theme_core] — test_arena_compact_hook_concurrent.cpp — Issue #1989: ASTArena::on_compact_hook_
- `tests/core/test_arena_concurrent_mutex.cpp` (—) [small, domain_suite, theme_core] — test_arena_concurrent_mutex.cpp — Issue #1988: ArenaGroup::arenas_ concurrent access.
- `tests/core/test_arena_defrag.cpp` (—) [domain_suite, theme_core] — tests/domain/arena/test_arena_defrag_concurrent.cpp — relocated for #1959 arena pilot
- `tests/compiler/test_bidirectional_annotation.cpp` (—) [domain_suite, theme_compiler] — tests/test_bidirectional_annotation.cpp — Issue #1413: True
- `tests/compiler/test_closure_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_closure_batch.cpp
- `tests/core/test_compiler_root_epoch_gc_safety_post_invalidate.cpp` (—) [domain_suite, theme_core] — test_compiler_root_epoch_gc_safety_post_invalidate.cpp — Issue #599:
- `tests/stdlib/test_datetime.cpp` (—) [domain_suite, theme_stdlib] — test_datetime.cpp — Merged datetime stdlib tests (#1978).
- `tests/compiler/test_env_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_env_batch.cpp
- `tests/compiler/test_envframe_epoch_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_envframe_epoch_batch.cpp — EnvFrame / bridge_epoch batch driver.
- `tests/core/test_envframe_truncate_guard_dual_epoch.cpp` (—) [domain_suite, theme_core] — Issue #1739/#1842/#1889/#1927/#1948 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_epoch_apply_hotpath.cpp` (—) [domain_suite, theme_compiler] — Issue #1475/#1496/#1558/#1598 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_gc_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — tests/domain/arena/test_gc_batch.cpp — relocated for #1959 arena pilot
- `tests/serve/test_gc_compact_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — tests/domain/arena/test_compact_batch.cpp — relocated for #1959 arena pilot
- `tests/serve/test_gc_compact_sweep_batch.cpp` (—) [batch_driver, domain_suite, theme_serve] — tests/domain/arena/test_compact_sweep_batch.cpp — relocated for #1959 arena pilot
- `tests/core/test_gc_evaluator_integration.cpp` (—) [domain_suite, theme_core] — test_gc_evaluator_integration.cpp — Issue #113 verification
- `tests/serve/test_issue_1990.cpp` (#1990) [small, domain_suite, theme_serve] — test_issue_1990.cpp — Issue #1990 / B-009: (gc-temp) and (gc-stats)
- `tests/serve/test_issue_1991.cpp` (#1991) [small, domain_suite, theme_serve] — test_issue_1991.cpp — Issue #1991 / B-010: (gc) primitive clears
- `tests/serve/test_issue_1993.cpp` (#1993) [domain_suite, theme_serve] — test_issue_1993.cpp — Issue #1993 (D-001): (gc-heap) direct-clear
- `tests/compiler/test_quota_edge_cases.cpp` (—) [domain_suite, theme_compiler] — AC1: boundary 0→1 transition (unlimited → bounded reject)
- `tests/core/test_set_arena_atomic_owner.cpp` (—) [domain_suite, theme_core] — test_set_arena_atomic_owner.cpp — Issue #1663
- `tests/core/test_task4_highperf_full_hotpath_matrix.cpp` (—) [domain_suite, theme_core] — test_task4_highperf_full_hotpath_matrix.cpp — Issue #607:

### `mutation_dirty` — Mutation / dirty propagation / provenance (58)

**Target:** tests/domain/test_domain_typed_mutate.cpp + mutation_boundary batch

**Priority:** P0 — high volume; strong domain suite foothold

#### domain/ (58)

- `tests/compiler/test_adt_match_exhaustiveness_incremental_task2.cpp` (—) [domain_suite, theme_compiler] — test_adt_match_exhaustiveness_incremental_task2.cpp
- `tests/compiler/test_atomic_batch_metadata.cpp` (—) [batch_driver, domain_suite, theme_compiler] — Issue #1649/#1680/#1893 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_atomic_batch_rollback_closed_loop.cpp` (—) [batch_driver, domain_suite, theme_compiler] — Issue #192/#459/#529/#553 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_atomic_batch_rollback_fiber_task1.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_atomic_batch_rollback_fiber_task1.cpp —
- `tests/compiler/test_atomic_batch_snapshot_stable_ref_ai_loops.cpp` (—) [batch_driver, domain_suite, theme_compiler] — - AC1: workspace:snapshot + workspace:rollback-to primitives
- `tests/compiler/test_coercion_dead_elim_castop_flow_zerooverhead.cpp` (—) [domain_suite, theme_compiler] — test_coercion_dead_elim_castop_flow_zerooverhead.cpp
- `tests/compiler/test_compiler_closure_env_safety_post_invalidate.cpp` (—) [domain_suite, theme_compiler] — test_compiler_closure_env_safety_post_invalidate.cpp —
- `tests/compiler/test_composite_typed_mutate.cpp` (—) [domain_suite, theme_compiler] — Issue #1408: Inline no-op stubs for aura::jit::AuraJIT::invalidate_prefix
- `tests/compiler/test_constraint_system_solve_delta_cross_delta_task2.cpp` (—) [domain_suite, theme_compiler] — test_constraint_system_solve_delta_cross_delta_task2.cpp
- `tests/core/test_coverage_holes_workspace_lock.cpp` (—) [domain_suite, theme_core] — Issue #1816 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_dead_coercion_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_dead_coercion_batch.cpp
- `tests/core/test_dep_graph_concurrent.cpp` (—) [domain_suite, theme_core] — test_dep_graph_concurrent.cpp — Issue #1376:
- `tests/compiler/test_dirty_delta_present.cpp` (—) [domain_suite, theme_compiler] — skip rate >60% under sparse mutations, metrics avg/p99, mutation guard.
- `tests/compiler/test_dirty_propagation_cascade.cpp` (—) [domain_suite, theme_compiler] — AC1: cascade_mark_dirty / propagate_closure BFS marks all dependents
- `tests/compiler/test_edsl_core_stability_cow_atomic_query_mutate.cpp` (—) [domain_suite, theme_compiler] — test_edsl_core_stability_cow_atomic_query_mutate.cpp — Issue #655:
- `tests/compiler/test_followups.cpp` (—) [followup, domain_suite, theme_compiler] — (mutation-log:diff / dirty:summary /
- `tests/core/test_guard_dtor_batch_metrics.cpp` (—) [batch_driver, domain_suite, theme_core] — Issue #1747 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_hotpath_matrix_batch.cpp` (—) [large, batch_driver, domain_suite, theme_core] — test_domain_gates_batch.cpp — Domain suite batch: behavioral gates.
- `tests/compiler/test_incremental_typed_selfmod_dirty_narrowing.cpp` (—) [domain_suite, theme_compiler] — test_incremental_typed_selfmod_dirty_narrowing.cpp — Merged #509/#518/#526/#536/#537/#550 +
- `tests/compiler/test_invalidate_cascade_order.cpp` (—) [domain_suite, theme_compiler] — test_invalidate_cascade_order.cpp — Issue #1378:
- `tests/core/test_issue_1994.cpp` (#1994) [domain_suite, theme_core] — test_issue_1994.cpp — Issue #1994 (F-004): `(workspace-state)` and
- `tests/compiler/test_issues_819_829_batch.cpp` (#819) [batch_driver, domain_suite, theme_compiler] — test_issues_819_829_batch.cpp — Phase 1 close for Issues #819–#829.
- `tests/compiler/test_linear_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_linear_batch.cpp
- `tests/compiler/test_linear_ownership_postmutate_guard_steal_envframe.cpp` (—) [domain_suite, theme_compiler] — test_linear_ownership_postmutate_guard_steal_envframe.cpp — Issue #800:
- `tests/core/test_lock_hierarchy.cpp` (—) [domain_suite, theme_core] — the lock-hierarchy contract documented in Issue #1388.
- `tests/core/test_marker_metadata_lock.cpp` (—) [domain_suite, theme_core] — Issue #1783 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_module_boundary.cpp` (—) [domain_suite, theme_core] — Issue #1885 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_mutate_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_mutate_batch.cpp
- `tests/compiler/test_mutate_cross_thread_migration.cpp` (—) [domain_suite, theme_compiler] — test_mutate_cross_thread_migration.cpp — Issue #1373:
- `tests/compiler/test_mutation_boundary_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_mutation_boundary_batch.cpp
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
- `tests/compiler/test_occurrence_typing_blame_post_mutate_recovery.cpp` (—) [domain_suite, theme_compiler] — test_occurrence_typing_blame_post_mutate_recovery.cpp — restored standalone (AC drift under batch
- `tests/compiler/test_occurrence_typing_blame_post_mutate_task2.cpp` (—) [domain_suite, theme_compiler] — test_occurrence_typing_blame_post_mutate_task2.cpp — restored standalone (AC drift under batch
- `tests/compiler/test_per_symbol_dirty_cycle_guard.cpp` (—) [domain_suite, theme_compiler] — Issue #1786 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_per_symbol_dirty_pool_lock.cpp` (—) [domain_suite, theme_core] — Issue #1785 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_provenance_blame_hygiene.cpp` (—) [domain_suite, theme_compiler] — Issue #1877 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_query_mutate_consistency.cpp` (—) [domain_suite, theme_compiler] — test_query_mutate_consistency.cpp — Issue #1374:
- `tests/renderer/test_render_ai_native_template.cpp` (—) [domain_suite, theme_renderer] — Issue #1677 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_stable_ref_cow_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — Issue #1912 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_stable_ref_cow_subworkspace_concurrent_ai.cpp` (—) [domain_suite, theme_compiler] — - AC1: query:stable-ref-boundary-stats-hash reachable (schema 738)
- `tests/compiler/test_stable_ref_cross_cow_provenance_enforcement.cpp` (—) [domain_suite, theme_compiler] — test_stable_ref_cross_cow_provenance_enforcement.cpp — Issue #818:
- `tests/compiler/test_stable_ref_full_provenance_enforcement.cpp` (—) [domain_suite, theme_compiler] — ensure_valid_or_refresh, auto-refresh counters, epoch fence, 1000-iter
- `tests/serve/test_stable_ref_provenance_fiber_cow.cpp` (—) [domain_suite, theme_serve] — test_stable_ref_provenance_fiber_cow.cpp — Merged #457/#497/#527/#540/#549 + #551/#552 (#1978).
- `tests/compiler/test_stable_ref_provenance_mandate.cpp` (—) [domain_suite, theme_compiler] — Issue #1500/#1564/#1630 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_stale_ref_string_heap.cpp` (—) [domain_suite, theme_core] — Issue #1681 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_typesystem_solve_delta_occurrence_priority_heavy_mutate.cpp` (—) [domain_suite, theme_compiler] — test_typesystem_solve_delta_occurrence_priority_heavy_mutate.cpp — Issue #745:
- `tests/compiler/test_typesystem_typed_mutate_incremental_gaps.cpp` (—) [domain_suite, theme_compiler] — test_typesystem_typed_mutate_incremental_gaps.cpp — Issue #659:
- `tests/compiler/test_unify_invalidate_try_acquire.cpp` (—) [domain_suite, theme_compiler] — Issue #1476/#1547/#1628/#1634 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_wave1_workspace_lock_reentrancy.cpp` (—) [domain_suite, theme_core] — test_wave1_workspace_lock_reentrancy.cpp — Wave1 B-03 / B-09

### `fiber_orch` — Fiber / orchestration / steal / Guard (28)

**Target:** tests/domain/test_domain_fiber_orchestration.cpp + fiber_resume batch

**Priority:** P1 — domain suite already collapses many obs gates

#### domain/ (28)

- `tests/compiler/test_aot_bridge_checkpoint_version_steal.cpp` (—) [domain_suite, theme_compiler] — test_aot_bridge_checkpoint_version_steal.cpp — Issue #653:
- `tests/serve/test_concurrent.cpp` (—) [large, domain_suite, theme_serve] — test_concurrent.cpp — Concurrency model unit tests
- `tests/compiler/test_edsl_concurrent_fiber_boundary_task1.cpp` (—) [domain_suite, theme_compiler] — test_edsl_concurrent_fiber_boundary_task1.cpp —
- `tests/compiler/test_edsl_concurrent_query_mutate.cpp` (—) [domain_suite, theme_compiler] — test_edsl_concurrent_query_mutate.cpp — Issue #332
- `tests/compiler/test_env_lookup_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_env_lookup_batch.cpp — batch driver for Env::lookup family.
- `tests/serve/test_fiber_concurrent_unit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — test_fiber_concurrent_unit_batch.cpp — light concurrent units
- `tests/serve/test_fiber_integration_batch.cpp` (—) [batch_driver, domain_suite, theme_serve] — tests/domain/test_fiber_integration_batch.cpp — Wave 8 of #1957 migration.
- `tests/serve/test_fiber_mutation_steal_safety.cpp` (—) [domain_suite, theme_serve] — test_fiber_mutation_steal_safety.cpp — Issue #542:
- `tests/serve/test_fiber_orch_core_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — test_fiber_orch_core_batch.cpp — consolidated fiber-theme drivers
- `tests/serve/test_fiber_orch_parallel_quota_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — test_fiber_orch_parallel_quota_batch.cpp — consolidated fiber-theme drivers
- `tests/serve/test_fiber_steal_panic_checkpoint_nested_gc.cpp` (—) [small, domain_suite, theme_serve] — tests/test_fiber_steal_panic_checkpoint_nested_gc.cpp — Issue #1446
- `tests/serve/test_fiber_strategy_evolve_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — test_fiber_strategy_evolve_batch.cpp — consolidated fiber-theme drivers
- `tests/serve/test_fiber_synthesize_batch.cpp` (—) [batch_driver, domain_suite, theme_serve] — test_fiber_synthesize_batch.cpp — consolidated fiber-theme drivers
- `tests/serve/test_issue_1992.cpp` (#1992) [domain_suite, theme_serve] — test_issue_1992.cpp — Issue #1992 (C-001): Fiber::mutation_stack_storage_
- `tests/compiler/test_issues_809_817_batch.cpp` (#809) [batch_driver, domain_suite, theme_compiler] — test_issues_809_817_batch.cpp — Phase 1 close for Issues #809–#817.
- `tests/compiler/test_lock_order_closures_env.cpp` (—) [domain_suite, theme_compiler] — Issue #1664 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_panic_checkpoint_fiber_resume_safety.cpp` (—) [domain_suite, theme_serve] — test_panic_checkpoint_fiber_resume_safety.cpp — Issue #592:
- `tests/compiler/test_per_defuse_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_per_defuse_batch.cpp — batch driver for per_defuse_index family.
- `tests/serve/test_per_fiber_stack_pool_high_concurrency.cpp` (—) [domain_suite, theme_serve] — test_per_fiber_stack_pool_high_concurrency.cpp — Issue #652:
- `tests/compiler/test_prompt6_full_memory_safety_fuzz_stress.cpp` (—) [domain_suite, theme_compiler] — test_prompt6_full_memory_safety_fuzz_stress.cpp — Issue #602:
- `tests/compiler/test_propagate_marker_cycle_guard.cpp` (—) [domain_suite, theme_compiler] — Issue #1679/#1682/#1782 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_runtime_mutation_boundary_steal_safety.cpp` (—) [domain_suite, theme_serve] — test_runtime_mutation_boundary_steal_safety.cpp — Issue #588:
- `tests/serve/test_safepoint_mutation.cpp` (—) [domain_suite, theme_serve] — test_safepoint_mutation.cpp — Issue #1364: safepoint × mutation telemetry
- `tests/serve/test_scheduler_gc_defer_pending_panic_steal.cpp` (—) [domain_suite, theme_serve] — AC1: pending checkpoint → GCCollector::request deferred; collect skips
- `tests/serve/test_scheduler_gc_safepoint_mutation_coordination.cpp` (—) [domain_suite, theme_serve] — test_scheduler_gc_safepoint_mutation_coordination.cpp —
- `tests/core/test_stress_alloc_storage_lock.cpp` (—) [domain_suite, theme_core] — test_stress_alloc_storage_lock.cpp — Issue #1397
- `tests/compiler/test_typechecker_incremental_guard_steal_fidelity.cpp` (—) [domain_suite, theme_compiler] — test_typechecker_incremental_guard_steal_fidelity.cpp — Issue #798:
- `tests/core/test_workspace_swap_guard.cpp` (—) [domain_suite, theme_core] — Issue #1717 (#1978 renamed): issue# moved from filename to header.

### `linear_ownership` — Linear ownership / borrow / consume (1)

**Target:** tests/test_linear_ownership_batch.cpp → domain/

**Priority:** P1 — small, already partially batched

#### domain/ (1)

- `tests/core/test_type_registry_ownership.cpp` (—) [small, domain_suite, theme_core] — Issue #1835/#1837 (#1978 renamed): issue# moved from filename to header.

### `edsl_hygiene` — EDSL / macro hygiene / reflect (12)

**Target:** tests/domain/test_domain_hygiene_dirty.cpp + macro_reflect batch

**Priority:** P1 — domain hygiene suite exists

#### domain/ (12)

- `tests/core/test_contracts.cpp` (—) [small, domain_suite, theme_core] — tests/test_contracts.cpp
- `tests/reflect/test_error_merr.cpp` (—) [small, domain_suite, theme_reflect] — test_error_merr.cpp — Pilot for centralized make_merr (refactor Step 0.1+)
- `tests/reflect/test_ir_cache_v2.cpp` (—) [small, domain_suite, theme_reflect] — tests/test_ir_cache_v2.cpp
- `tests/reflect/test_issue_178.cpp` (#178) [small, early_issue, domain_suite, theme_reflect] — test_issue_178.cpp — Issue #178 / #268: production NodeView
- `tests/reflect/test_issue_178_reflect.cpp` (#178) [early_issue, domain_suite, theme_reflect] — Non-module TU: P2996 reflection (Issue #268).
- `tests/compiler/test_macro_hygiene_fiber_panic_aot_soa_self_evo.cpp` (—) [domain_suite, theme_compiler] — test_macro_hygiene_fiber_panic_aot_soa_self_evo.cpp — Issue #654:
- `tests/compiler/test_macro_reflect_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_macro_reflect_batch.cpp — batch driver for macro+reflect+self-evo family.
- `tests/compiler/test_prompt2_6_impact_scope_quote_lambda_bridge_env.cpp` (—) [domain_suite, theme_compiler] — test_prompt2_6_impact_scope_quote_lambda_bridge_env.cpp — Issue #741:
- `tests/compiler/test_reflect_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_reflect_batch.cpp
- `tests/reflect/test_reflect_hygiene_unit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_reflect] — test_edsl_hygiene_unit_batch.cpp — consolidated edsl hygiene drivers
- `tests/reflect/test_reflect_macro_hygiene_batch.cpp` (—) [large, batch_driver, domain_suite, theme_reflect] — test_edsl_macro_hygiene_batch.cpp — consolidated edsl hygiene drivers
- `tests/reflect/test_reflect_pattern_hygiene_batch.cpp` (—) [large, batch_driver, domain_suite, theme_reflect] — test_edsl_pattern_hygiene_batch.cpp — consolidated edsl hygiene drivers

### `jit_incremental` — JIT / AOT / incremental relower (11)

**Target:** domain suite for incremental_*; keep heavy JIT in issue bundles

**Priority:** P2 — link-profile heavy; migrate AC smoke first

#### domain/ (11)

- `tests/compiler/test_aot_region_per_eval.cpp` (—) [domain_suite, theme_compiler] — test_aot_region_per_eval.cpp — Issue #1367 (standalone; ACs drift under current aot: API)
- `tests/compiler/test_aot_shell_c0_escape.cpp` (—) [domain_suite, theme_compiler] — test_issue_1997.cpp -- runtime smoke test for B-002 / #1997
- `tests/compiler/test_capability_gating.cpp` (—) [domain_suite, theme_compiler] — Issue #1416: Inline no-op stubs for aura::jit::AuraJIT::invalidate_prefix
- `tests/compiler/test_incremental_type_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_incremental_type_batch.cpp — batch driver for incremental_type family.
- `tests/compiler/test_jit_batch_deopt_clear.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_issue_1996.cpp — Issue #1996 (B-003): `g_batch_deopt_jit` raw
- `tests/compiler/test_jit_closure_cache_race.cpp` (—) [domain_suite, theme_compiler] — Issue #1707 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_jit_concurrent_compile.cpp` (—) [domain_suite, theme_compiler] — test_jit_concurrent_compile.cpp — Issue #114 concurrent compile stress
- `tests/core/test_pair_slot_lock.cpp` (—) [domain_suite, theme_core] — test_issue_1998.cpp -- runtime smoke test for B-024 / #1998
- `tests/core/test_prim_call_count_clamp.cpp` (—) [small, domain_suite, theme_core] — Issue #1711 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_relower_strategy_cache_lock.cpp` (—) [domain_suite, theme_compiler] — Issue #1839/#1855 (#1978 renamed): issue# moved from filename to header.
- `tests/stdlib/test_spec_runtime.cpp` (—) [domain_suite, theme_stdlib] — test_spec_runtime.cpp — Runtime tests for L2 specialization (Phase 3, #53)

### `shape_soa` — Shape / SoA / column layout (8)

**Target:** tests/test_soa_batch.cpp → domain/

**Priority:** P2 — small-medium; soa_batch precedent

#### domain/ (8)

- `tests/compiler/test_apply_closure_envframe_soa.cpp` (—) [domain_suite, theme_compiler] — Issue #1365/#1475/#1511/#1626/#1632/#1660 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_ir_soa_dual_emit.cpp` (—) [domain_suite, theme_compiler] — test_ir_soa_dual_emit.cpp — Issue #1377:
- `tests/compiler/test_matcher_stable_captures.cpp` (—) [domain_suite, theme_compiler] — Issue #1695 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_set_workspace_flat.cpp` (—) [domain_suite, theme_core] — Issue #1729 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_shapeprofiler_stability_deopt_jit_mutate.cpp` (—) [domain_suite, theme_compiler] — test_shapeprofiler_stability_deopt_jit_mutate.cpp — Issue #605:
- `tests/core/test_soa_batch.cpp` (—) [large, batch_driver, domain_suite, theme_core] — test_soa_batch.cpp
- `tests/compiler/test_spec_jit.cpp` (—) [large, domain_suite, theme_compiler] — test_spec_jit.cpp — Unit tests for L1 type specialization (Phase 2, #53)
- `tests/core/test_workspace_delete_child.cpp` (—) [domain_suite, theme_core] — Issue #1770 (#1978 renamed): issue# moved from filename to header.

### `observability` — Observability / metrics / query:*-stats (20)

**Target:** tests/domain/test_obs_schema_matrix.cpp + cases/obs_schema_cases.hpp

**Priority:** P2 — often thin schema probes; collapse into obs matrix

#### domain/ (20)

- `tests/compiler/test_auto_evolve_closure_live.cpp` (—) [domain_suite, theme_compiler] — Issue #1713 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_bugfix_968.cpp` (#968) [small, domain_suite, theme_compiler] — Issue #957/#968/#982/#984 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_consolidated_production_priority.cpp` (—) [domain_suite, theme_core] — Issue #514/#515/#516/#517/#520 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_fine_dirty_relower.cpp` (—) [domain_suite, theme_compiler] — test_fine_dirty_relower.cpp — Issue #1657 (standalone; bump metrics ACs drift)
- `tests/compiler/test_inline_pass_stats_unpack.cpp` (—) [domain_suite, theme_compiler] — Issue #1784 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_inline_typecheck_exception.cpp` (—) [domain_suite, theme_compiler] — Issue #1769 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_let_poly_solve_delta.cpp` (—) [domain_suite, theme_compiler] — Issue #1617/#745/#798 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_mutation_aot_unit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_mutation_aot_unit_batch.cpp — consolidated mutation-theme drivers
- `tests/compiler/test_obs_schema_matrix.cpp` (—) [domain_suite, theme_compiler] — test_obs_schema_matrix.cpp — Domain suite: observability + production schemas
- `tests/serve/test_production_sweep.cpp` (—) [small, domain_suite, theme_serve] — test_production_sweep.cpp — fiber production sweep (standalone; SIGSEGV in batch)
- `tests/compiler/test_query_dispatch.cpp` (—) [small, domain_suite, theme_compiler] — Issue #1435 (query :op) unified dispatcher
- `tests/compiler/test_scan_skip_freed_closures.cpp` (—) [domain_suite, theme_compiler] — Issue #1665 (#1978 renamed): issue# moved from filename to header.
- `tests/serve/test_self_evolution_chaos_stable.cpp` (—) [domain_suite, theme_serve] — test_self_evolution_chaos_stable_674.cpp — Issue #674:
- `tests/serve/test_self_heal_policy_engine.cpp` (—) [domain_suite, theme_serve] — test_self_heal_policy_engine.cpp — standalone (flaky/failing ACs under batch link)
- `tests/compiler/test_seva_demo_metrics.cpp` (—) [small, domain_suite, theme_compiler] — Issue #1720/#1835/#1840/#1841 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_stale_closure_fallback.cpp` (—) [domain_suite, theme_compiler] — AC1: apply_closure after mark_define_dirty / epoch bump →
- `tests/compiler/test_task2_refinement_closed_loop.cpp` (—) [domain_suite, theme_compiler] — Issue #432/#467/#495/#509/#574 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_typechecker_incremental_locality.cpp` (—) [domain_suite, theme_compiler] — Issue #1617/#1923 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_verify_parse_shared_helper.cpp` (—) [domain_suite, theme_compiler] — Issue #1771 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_workspace_dispatch.cpp` (—) [domain_suite, theme_core] — Issue #1437 (workspace :op) unified dispatcher

### `uncategorized` — Uncategorized / mixed (21)

**Target:** manual triage before domain placement

**Priority:** P3 — review case-by-case

#### domain/ (21)

- `tests/compiler/test_arithmetic_int64_safety.cpp` (—) [small, domain_suite, theme_compiler] — test_arithmetic_int64_safety.cpp — Issues #1150–#1156 Phase 1
- `tests/compiler/test_ast_workspace_modules.cpp` (—) [domain_suite, theme_compiler] — test_ast_workspace_modules.cpp — Issue #563:
- `tests/stdlib/test_atomic_swap_stdlib.cpp` (—) [domain_suite, theme_stdlib] — test_atomic_swap_stdlib.cpp — Issue #1380:
- `tests/compiler/test_aura_result_error_policy.cpp` (—) [domain_suite, theme_compiler] — test_aura_result_error_policy.cpp — Issues #807 + #808:
- `tests/compiler/test_closure_free.cpp` (—) [domain_suite, theme_compiler] — test_closure_free.cpp — Issue #1361: aura_free_closure + ID reuse
- `tests/compiler/test_compile02_no_dup_imports.cpp` (—) [domain_suite, theme_compiler] — Issue #1857 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_core_builtins_review.cpp` (—) [domain_suite, theme_compiler] — test_core_builtins_review.cpp — Issue #564:
- `tests/core/test_hash_iter_invalidation.cpp` (—) [domain_suite, theme_core] — test_hash_iter_invalidation.cpp - Issue #1398:
- `tests/compiler/test_module_loader_dead_heap_circular.cpp` (—) [domain_suite, theme_compiler] — Issue #1488/#1692 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_module_prefix_dead_heap.cpp` (—) [domain_suite, theme_compiler] — Issue #1488/#1693 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_open_issues_phase1_batch.cpp` (—) [phase_slice, batch_driver, domain_suite, theme_compiler] — test_open_issues_phase1_batch.cpp — legacy alias for the domain suite.
- `tests/core/test_pair_unchecked_safety.cpp` (—) [domain_suite, theme_core] — Issue #1710 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_panic_checkpoint_clear.cpp` (—) [domain_suite, theme_core] — Issue #1727 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_panic_checkpoint_raii.cpp` (—) [domain_suite, theme_core] — test_panic_checkpoint_raii.cpp — Issue #1363: wire PanicCheckpointGuard to Evaluator
- `tests/compiler/test_query_namespace_audit.cpp` (—) [domain_suite, theme_compiler] — test_query_namespace_audit.cpp — Issue #562:
- `tests/compiler/test_query_pattern_concurrent.cpp` (—) [domain_suite, theme_compiler] — test_query_pattern_concurrent.cpp — Issue #1372:
- `tests/stdlib/test_stdlib_infrastructure.cpp` (—) [domain_suite, theme_stdlib] — test_stdlib_infrastructure.cpp — Issue #565:
- `tests/stdlib/test_synthesize_namespace_demotion.cpp` (—) [domain_suite, theme_stdlib] — test_synthesize_namespace_demotion.cpp — Issue #561:
- `tests/renderer/test_terminal_concurrent.cpp` (—) [domain_suite, theme_renderer] — test_terminal_concurrent.cpp — Issue #1352 (standalone; free-corruption when co-linked)
- `tests/repl/test_terminal_domain_batch.cpp` (—) [batch_driver, domain_suite, theme_repl] — test_terminal_domain_batch.cpp — terminal domain batch driver.
- `tests/core/test_try_lock_workspace_lock_order.cpp` (—) [domain_suite, theme_core] — Issue #1768 (#1978 renamed): issue# moved from filename to header.

## Regenerating

```bash
python3 scripts/inventory_legacy_tests.py
python3 scripts/inventory_legacy_tests.py --check
```

The coarser Phase-2 5-domain classifier remains available as `scripts/classify_test_issues.py` for historical comparison; **this inventory (#1957) is the planning source of truth** for domain migration.
