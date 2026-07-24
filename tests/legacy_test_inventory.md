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
| `tests/domain/test_*.cpp` | 71 | Preferred destination suites |
| **Total scanned** | **71** | |

### Related artifacts

- Coarser 5-bucket Phase-2 map: [`tests/domain_classification.md`](domain_classification.md) (`scripts/classify_test_issues.py`)
- Link/bundle profiles: [`tests/fixtures/issue_link_profiles.json`](fixtures/issue_link_profiles.json)
- Domain CMake: [`cmake/AuraDomainTests.cmake`](../cmake/AuraDomainTests.cmake)
- Test layout rules: [`tests/README.md`](README.md)

## Theme buckets (8 + uncategorized)

Classification uses the **filename + first 50 lines** (keywords and filename token boosts). Ties break toward earlier themes in the priority order below.

| Theme | Title | Issues | Root | Domain | Total | Migration priority |
|-------|-------|-------:|-----:|-------:|------:|--------------------|
| `arena_compaction` | Arena / compaction / GC | 0 | 0 | 12 | 12 | P0 — well-contained, batch drivers already exist |
| `mutation_dirty` | Mutation / dirty propagation / provenance | 0 | 0 | 7 | 7 | P0 — high volume; strong domain suite foothold |
| `fiber_orch` | Fiber / orchestration / steal / Guard | 0 | 0 | 5 | 5 | P1 — domain suite already collapses many obs gates |
| `linear_ownership` | Linear ownership / borrow / consume | 0 | 0 | 1 | 1 | P1 — small, already partially batched |
| `edsl_hygiene` | EDSL / macro hygiene / reflect | 0 | 0 | 9 | 9 | P1 — domain hygiene suite exists |
| `jit_incremental` | JIT / AOT / incremental relower | 0 | 0 | 7 | 7 | P2 — link-profile heavy; migrate AC smoke first |
| `shape_soa` | Shape / SoA / column layout | 0 | 0 | 5 | 5 | P2 — small-medium; soa_batch precedent |
| `observability` | Observability / metrics / query:*-stats | 0 | 0 | 9 | 9 | P2 — often thin schema probes; collapse into obs matrix |
| `uncategorized` | Uncategorized / mixed | 0 | 0 | 16 | 16 | P3 — review case-by-case |

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
- Existing `*_batch` drivers (migration milestones): **15**

### Multi-file issue groups (consolidate first)


### Smallest issue tests (triage for obs-matrix fold or drop)


### Batch drivers already present

- `tests/core/test_arena_batch.cpp` → theme `arena_compaction`
- `tests/compiler/test_env_batch.cpp` → theme `arena_compaction`
- `tests/compiler/test_env_lookup_batch.cpp` → theme `fiber_orch`
- `tests/compiler/test_envframe_epoch_batch.cpp` → theme `arena_compaction`
- `tests/serve/test_fiber_integration_batch.cpp` → theme `fiber_orch`
- `tests/serve/test_gc_batch.cpp` → theme `arena_compaction`
- `tests/serve/test_gc_compact_batch.cpp` → theme `arena_compaction`
- `tests/serve/test_gc_compact_sweep_batch.cpp` → theme `arena_compaction`
- `tests/core/test_hotpath_matrix_batch.cpp` → theme `mutation_dirty`
- `tests/compiler/test_jit_batch_deopt_clear.cpp` → theme `jit_incremental`
- `tests/reflect/test_reflect_hygiene_unit_batch.cpp` → theme `edsl_hygiene`
- `tests/reflect/test_reflect_macro_hygiene_batch.cpp` → theme `edsl_hygiene`
- `tests/reflect/test_reflect_pattern_hygiene_batch.cpp` → theme `edsl_hygiene`
- `tests/core/test_soa_batch.cpp` → theme `shape_soa`
- `tests/repl/test_terminal_domain_batch.cpp` → theme `uncategorized`

### Domain suites (do not regress; extend these)

- `tests/compiler/test_aot_shell_c0_escape.cpp`
- `tests/core/test_arena_batch.cpp`
- `tests/core/test_arena_defrag.cpp`
- `tests/compiler/test_arithmetic_int64_safety.cpp`
- `tests/compiler/test_ast_workspace_modules.cpp`
- `tests/stdlib/test_atomic_swap_stdlib.cpp`
- `tests/compiler/test_aura_result_error_policy.cpp`
- `tests/compiler/test_auto_evolve_closure_live.cpp`
- `tests/compiler/test_bidirectional_annotation.cpp`
- `tests/compiler/test_bugfix_968.cpp`
- `tests/compiler/test_closure_free.cpp`
- `tests/compiler/test_compile02_no_dup_imports.cpp`
- `tests/compiler/test_compiler_closure_env_safety_post_invalidate.cpp`
- `tests/core/test_compiler_root_epoch_gc_safety_post_invalidate.cpp`
- `tests/core/test_consolidated_production_priority.cpp`
- `tests/core/test_contracts.cpp`
- `tests/core/test_coverage_holes_workspace_lock.cpp`
- `tests/stdlib/test_datetime.cpp`
- `tests/compiler/test_env_batch.cpp`
- `tests/compiler/test_env_lookup_batch.cpp`
- `tests/compiler/test_envframe_epoch_batch.cpp`
- `tests/reflect/test_error_merr.cpp`
- `tests/serve/test_fiber_integration_batch.cpp`
- `tests/serve/test_gc_batch.cpp`
- `tests/serve/test_gc_compact_batch.cpp`
- `tests/serve/test_gc_compact_sweep_batch.cpp`
- `tests/core/test_gc_evaluator_integration.cpp`
- `tests/core/test_hash_iter_invalidation.cpp`
- `tests/core/test_hotpath_matrix_batch.cpp`
- `tests/compiler/test_inline_pass_stats_unpack.cpp`
- `tests/compiler/test_inline_typecheck_exception.cpp`
- `tests/reflect/test_ir_cache_v2.cpp`
- `tests/reflect/test_issue_178.cpp`
- `tests/reflect/test_issue_178_reflect.cpp`
- `tests/compiler/test_jit_batch_deopt_clear.cpp`
- `tests/compiler/test_jit_closure_cache_race.cpp`
- `tests/core/test_lock_hierarchy.cpp`
- `tests/compiler/test_matcher_stable_captures.cpp`
- `tests/compiler/test_module_loader_dead_heap_circular.cpp`
- `tests/compiler/test_module_prefix_dead_heap.cpp`
- `tests/compiler/test_obs_schema_matrix.cpp`
- `tests/core/test_pair_slot_lock.cpp`
- `tests/core/test_pair_unchecked_safety.cpp`
- `tests/core/test_panic_checkpoint_raii.cpp`
- `tests/compiler/test_per_symbol_dirty_cycle_guard.cpp`
- `tests/core/test_per_symbol_dirty_pool_lock.cpp`
- `tests/core/test_prim_call_count_clamp.cpp`
- `tests/compiler/test_prompt2_6_impact_scope_quote_lambda_bridge_env.cpp`
- `tests/compiler/test_prompt6_full_memory_safety_fuzz_stress.cpp`
- `tests/compiler/test_propagate_marker_cycle_guard.cpp`
- `tests/compiler/test_query_dispatch.cpp`
- `tests/compiler/test_query_pattern_concurrent.cpp`
- `tests/reflect/test_reflect_hygiene_unit_batch.cpp`
- `tests/reflect/test_reflect_macro_hygiene_batch.cpp`
- `tests/reflect/test_reflect_pattern_hygiene_batch.cpp`
- `tests/compiler/test_relower_strategy_cache_lock.cpp`
- `tests/renderer/test_render_ai_native_template.cpp`
- `tests/core/test_set_workspace_flat.cpp`
- `tests/compiler/test_shapeprofiler_stability_deopt_jit_mutate.cpp`
- `tests/core/test_soa_batch.cpp`
- `tests/stdlib/test_spec_runtime.cpp`
- `tests/stdlib/test_stdlib_infrastructure.cpp`
- `tests/stdlib/test_synthesize_namespace_demotion.cpp`
- `tests/core/test_task4_highperf_full_hotpath_matrix.cpp`
- `tests/repl/test_terminal_domain_batch.cpp`
- `tests/core/test_try_lock_workspace_lock_order.cpp`
- `tests/core/test_type_registry_ownership.cpp`
- `tests/compiler/test_verify_parse_shared_helper.cpp`
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

### `arena_compaction` — Arena / compaction / GC (12)

**Target:** tests/domain/ (extend compact/gc family; see test_compact_*_batch)

**Priority:** P0 — well-contained, batch drivers already exist

#### domain/ (12)

- `tests/core/test_arena_batch.cpp` (—) [large, batch_driver, domain_suite, theme_core] — tests/domain/arena/test_arena_batch.cpp — relocated for #1959 arena pilot
- `tests/core/test_arena_defrag.cpp` (—) [domain_suite, theme_core] — tests/domain/arena/test_arena_defrag_concurrent.cpp — relocated for #1959 arena pilot
- `tests/compiler/test_bidirectional_annotation.cpp` (—) [domain_suite, theme_compiler] — tests/test_bidirectional_annotation.cpp — Issue #1413: True
- `tests/core/test_compiler_root_epoch_gc_safety_post_invalidate.cpp` (—) [domain_suite, theme_core] — test_compiler_root_epoch_gc_safety_post_invalidate.cpp — Issue #599:
- `tests/stdlib/test_datetime.cpp` (—) [domain_suite, theme_stdlib] — test_datetime.cpp — Merged datetime stdlib tests (#1978).
- `tests/compiler/test_env_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_env_batch.cpp
- `tests/compiler/test_envframe_epoch_batch.cpp` (—) [large, batch_driver, domain_suite, theme_compiler] — test_envframe_epoch_batch.cpp — EnvFrame / bridge_epoch batch driver.
- `tests/serve/test_gc_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — tests/domain/arena/test_gc_batch.cpp — relocated for #1959 arena pilot
- `tests/serve/test_gc_compact_batch.cpp` (—) [large, batch_driver, domain_suite, theme_serve] — tests/domain/arena/test_compact_batch.cpp — relocated for #1959 arena pilot
- `tests/serve/test_gc_compact_sweep_batch.cpp` (—) [batch_driver, domain_suite, theme_serve] — tests/domain/arena/test_compact_sweep_batch.cpp — relocated for #1959 arena pilot
- `tests/core/test_gc_evaluator_integration.cpp` (—) [domain_suite, theme_core] — test_gc_evaluator_integration.cpp — Issue #113 verification
- `tests/core/test_task4_highperf_full_hotpath_matrix.cpp` (—) [domain_suite, theme_core] — test_task4_highperf_full_hotpath_matrix.cpp — Issue #607:

### `mutation_dirty` — Mutation / dirty propagation / provenance (7)

**Target:** tests/domain/test_domain_typed_mutate.cpp + mutation_boundary batch

**Priority:** P0 — high volume; strong domain suite foothold

#### domain/ (7)

- `tests/compiler/test_compiler_closure_env_safety_post_invalidate.cpp` (—) [domain_suite, theme_compiler] — test_compiler_closure_env_safety_post_invalidate.cpp —
- `tests/core/test_coverage_holes_workspace_lock.cpp` (—) [domain_suite, theme_core] — Issue #1816 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_hotpath_matrix_batch.cpp` (—) [large, batch_driver, domain_suite, theme_core] — test_domain_gates_batch.cpp — Domain suite batch: behavioral gates.
- `tests/core/test_lock_hierarchy.cpp` (—) [domain_suite, theme_core] — the lock-hierarchy contract documented in Issue #1388.
- `tests/compiler/test_per_symbol_dirty_cycle_guard.cpp` (—) [domain_suite, theme_compiler] — Issue #1786 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_per_symbol_dirty_pool_lock.cpp` (—) [domain_suite, theme_core] — Issue #1785 (#1978 renamed): issue# moved from filename to header.
- `tests/renderer/test_render_ai_native_template.cpp` (—) [domain_suite, theme_renderer] — Issue #1677 (#1978 renamed): issue# moved from filename to header.

### `fiber_orch` — Fiber / orchestration / steal / Guard (5)

**Target:** tests/domain/test_domain_fiber_orchestration.cpp + fiber_resume batch

**Priority:** P1 — domain suite already collapses many obs gates

#### domain/ (5)

- `tests/compiler/test_env_lookup_batch.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_env_lookup_batch.cpp — batch driver for Env::lookup family.
- `tests/serve/test_fiber_integration_batch.cpp` (—) [batch_driver, domain_suite, theme_serve] — tests/domain/test_fiber_integration_batch.cpp — Wave 8 of #1957 migration.
- `tests/compiler/test_prompt6_full_memory_safety_fuzz_stress.cpp` (—) [domain_suite, theme_compiler] — test_prompt6_full_memory_safety_fuzz_stress.cpp — Issue #602:
- `tests/compiler/test_propagate_marker_cycle_guard.cpp` (—) [domain_suite, theme_compiler] — Issue #1679/#1682/#1782 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_workspace_swap_guard.cpp` (—) [domain_suite, theme_core] — Issue #1717 (#1978 renamed): issue# moved from filename to header.

### `linear_ownership` — Linear ownership / borrow / consume (1)

**Target:** tests/test_linear_ownership_batch.cpp → domain/

**Priority:** P1 — small, already partially batched

#### domain/ (1)

- `tests/core/test_type_registry_ownership.cpp` (—) [small, domain_suite, theme_core] — Issue #1835/#1837 (#1978 renamed): issue# moved from filename to header.

### `edsl_hygiene` — EDSL / macro hygiene / reflect (9)

**Target:** tests/domain/test_domain_hygiene_dirty.cpp + macro_reflect batch

**Priority:** P1 — domain hygiene suite exists

#### domain/ (9)

- `tests/core/test_contracts.cpp` (—) [small, domain_suite, theme_core] — tests/test_contracts.cpp
- `tests/reflect/test_error_merr.cpp` (—) [small, domain_suite, theme_reflect] — test_error_merr.cpp — Pilot for centralized make_merr (refactor Step 0.1+)
- `tests/reflect/test_ir_cache_v2.cpp` (—) [small, domain_suite, theme_reflect] — tests/test_ir_cache_v2.cpp
- `tests/reflect/test_issue_178.cpp` (#178) [small, early_issue, domain_suite, theme_reflect] — test_issue_178.cpp — Issue #178 / #268: production NodeView
- `tests/reflect/test_issue_178_reflect.cpp` (#178) [early_issue, domain_suite, theme_reflect] — Non-module TU: P2996 reflection (Issue #268).
- `tests/compiler/test_prompt2_6_impact_scope_quote_lambda_bridge_env.cpp` (—) [domain_suite, theme_compiler] — test_prompt2_6_impact_scope_quote_lambda_bridge_env.cpp — Issue #741:
- `tests/reflect/test_reflect_hygiene_unit_batch.cpp` (—) [large, batch_driver, domain_suite, theme_reflect] — test_edsl_hygiene_unit_batch.cpp — consolidated edsl hygiene drivers
- `tests/reflect/test_reflect_macro_hygiene_batch.cpp` (—) [large, batch_driver, domain_suite, theme_reflect] — test_edsl_macro_hygiene_batch.cpp — consolidated edsl hygiene drivers
- `tests/reflect/test_reflect_pattern_hygiene_batch.cpp` (—) [large, batch_driver, domain_suite, theme_reflect] — test_edsl_pattern_hygiene_batch.cpp — consolidated edsl hygiene drivers

### `jit_incremental` — JIT / AOT / incremental relower (7)

**Target:** domain suite for incremental_*; keep heavy JIT in issue bundles

**Priority:** P2 — link-profile heavy; migrate AC smoke first

#### domain/ (7)

- `tests/compiler/test_aot_shell_c0_escape.cpp` (—) [domain_suite, theme_compiler] — test_issue_1997.cpp -- runtime smoke test for B-002 / #1997
- `tests/compiler/test_jit_batch_deopt_clear.cpp` (—) [batch_driver, domain_suite, theme_compiler] — test_issue_1996.cpp — Issue #1996 (B-003): `g_batch_deopt_jit` raw
- `tests/compiler/test_jit_closure_cache_race.cpp` (—) [domain_suite, theme_compiler] — Issue #1707 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_pair_slot_lock.cpp` (—) [domain_suite, theme_core] — test_issue_1998.cpp -- runtime smoke test for B-024 / #1998
- `tests/core/test_prim_call_count_clamp.cpp` (—) [small, domain_suite, theme_core] — Issue #1711 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_relower_strategy_cache_lock.cpp` (—) [domain_suite, theme_compiler] — Issue #1839/#1855 (#1978 renamed): issue# moved from filename to header.
- `tests/stdlib/test_spec_runtime.cpp` (—) [domain_suite, theme_stdlib] — test_spec_runtime.cpp — Runtime tests for L2 specialization (Phase 3, #53)

### `shape_soa` — Shape / SoA / column layout (5)

**Target:** tests/test_soa_batch.cpp → domain/

**Priority:** P2 — small-medium; soa_batch precedent

#### domain/ (5)

- `tests/compiler/test_matcher_stable_captures.cpp` (—) [domain_suite, theme_compiler] — Issue #1695 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_set_workspace_flat.cpp` (—) [domain_suite, theme_core] — Issue #1729 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_shapeprofiler_stability_deopt_jit_mutate.cpp` (—) [domain_suite, theme_compiler] — test_shapeprofiler_stability_deopt_jit_mutate.cpp — Issue #605:
- `tests/core/test_soa_batch.cpp` (—) [large, batch_driver, domain_suite, theme_core] — test_soa_batch.cpp
- `tests/core/test_workspace_delete_child.cpp` (—) [domain_suite, theme_core] — Issue #1770 (#1978 renamed): issue# moved from filename to header.

### `observability` — Observability / metrics / query:*-stats (9)

**Target:** tests/domain/test_obs_schema_matrix.cpp + cases/obs_schema_cases.hpp

**Priority:** P2 — often thin schema probes; collapse into obs matrix

#### domain/ (9)

- `tests/compiler/test_auto_evolve_closure_live.cpp` (—) [domain_suite, theme_compiler] — Issue #1713 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_bugfix_968.cpp` (#968) [small, domain_suite, theme_compiler] — Issue #957/#968/#982/#984 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_consolidated_production_priority.cpp` (—) [domain_suite, theme_core] — Issue #514/#515/#516/#517/#520 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_inline_pass_stats_unpack.cpp` (—) [domain_suite, theme_compiler] — Issue #1784 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_inline_typecheck_exception.cpp` (—) [domain_suite, theme_compiler] — Issue #1769 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_obs_schema_matrix.cpp` (—) [domain_suite, theme_compiler] — test_obs_schema_matrix.cpp — Domain suite: observability + production schemas
- `tests/compiler/test_query_dispatch.cpp` (—) [small, domain_suite, theme_compiler] — Issue #1435 (query :op) unified dispatcher
- `tests/compiler/test_verify_parse_shared_helper.cpp` (—) [domain_suite, theme_compiler] — Issue #1771 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_workspace_dispatch.cpp` (—) [domain_suite, theme_core] — Issue #1437 (workspace :op) unified dispatcher

### `uncategorized` — Uncategorized / mixed (16)

**Target:** manual triage before domain placement

**Priority:** P3 — review case-by-case

#### domain/ (16)

- `tests/compiler/test_arithmetic_int64_safety.cpp` (—) [small, domain_suite, theme_compiler] — test_arithmetic_int64_safety.cpp — Issues #1150–#1156 Phase 1
- `tests/compiler/test_ast_workspace_modules.cpp` (—) [domain_suite, theme_compiler] — test_ast_workspace_modules.cpp — Issue #563:
- `tests/stdlib/test_atomic_swap_stdlib.cpp` (—) [domain_suite, theme_stdlib] — test_atomic_swap_stdlib.cpp — Issue #1380:
- `tests/compiler/test_aura_result_error_policy.cpp` (—) [domain_suite, theme_compiler] — test_aura_result_error_policy.cpp — Issues #807 + #808:
- `tests/compiler/test_closure_free.cpp` (—) [domain_suite, theme_compiler] — test_closure_free.cpp — Issue #1361: aura_free_closure + ID reuse
- `tests/compiler/test_compile02_no_dup_imports.cpp` (—) [domain_suite, theme_compiler] — Issue #1857 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_hash_iter_invalidation.cpp` (—) [domain_suite, theme_core] — test_hash_iter_invalidation.cpp - Issue #1398:
- `tests/compiler/test_module_loader_dead_heap_circular.cpp` (—) [domain_suite, theme_compiler] — Issue #1488/#1692 (#1978 renamed): issue# moved from filename to header.
- `tests/compiler/test_module_prefix_dead_heap.cpp` (—) [domain_suite, theme_compiler] — Issue #1488/#1693 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_pair_unchecked_safety.cpp` (—) [domain_suite, theme_core] — Issue #1710 (#1978 renamed): issue# moved from filename to header.
- `tests/core/test_panic_checkpoint_raii.cpp` (—) [domain_suite, theme_core] — test_panic_checkpoint_raii.cpp — Issue #1363: wire PanicCheckpointGuard to Evaluator
- `tests/compiler/test_query_pattern_concurrent.cpp` (—) [domain_suite, theme_compiler] — test_query_pattern_concurrent.cpp — Issue #1372:
- `tests/stdlib/test_stdlib_infrastructure.cpp` (—) [domain_suite, theme_stdlib] — test_stdlib_infrastructure.cpp — Issue #565:
- `tests/stdlib/test_synthesize_namespace_demotion.cpp` (—) [domain_suite, theme_stdlib] — test_synthesize_namespace_demotion.cpp — Issue #561:
- `tests/repl/test_terminal_domain_batch.cpp` (—) [batch_driver, domain_suite, theme_repl] — test_terminal_domain_batch.cpp — terminal domain batch driver.
- `tests/core/test_try_lock_workspace_lock_order.cpp` (—) [domain_suite, theme_core] — Issue #1768 (#1978 renamed): issue# moved from filename to header.

## Regenerating

```bash
python3 scripts/inventory_legacy_tests.py
python3 scripts/inventory_legacy_tests.py --check
```

The coarser Phase-2 5-domain classifier remains available as `scripts/classify_test_issues.py` for historical comparison; **this inventory (#1957) is the planning source of truth** for domain migration.
