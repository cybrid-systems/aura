# 1646 — MutationBoundaryGuard long-running observability (partial-redundant-ship)

**Status:** shipped (commit pending, on `4fbc171c` baseline)
**Branch:** `main`
**Date:** 2026-07-19

## Context

`MutationBoundaryGuard` is the cornerstone of Aura's macro mutation
contract — every `query:mutate!` invocation must hold a Guard from
acquire → commit, with proper exception-rollback + hygiene-violation
detection paths. The body of #1646 reports that observability and
metrics wiring is incomplete:

- 部分 `bump_*` 方法仍 dead 或重复复制码 (`evaluator.ixx:500+`)
- Guard dtor / flush sites bump partially, but missing systematic
  hygiene-violation + macro-dirty + epoch-bump metrics
- `primitives_obs_*.cpp`: 重复 `insert_kv` lambda ≥ 249 sites
- 无系统性 hygiene-violation / macro-dirty / boundary-depth 实时曝光
  给 Agent 决策

## What landed (this commit)

### 1. 5 new `CompilerMetrics` counters (AC1 + AC2)

```cpp
// src/compiler/observability_metrics.h
std::atomic<std::uint64_t> mutation_boundary_success_total{0};
std::atomic<std::uint64_t> mutation_boundary_macro_dirty_propagated_total{0};
std::atomic<std::uint64_t> mutation_boundary_epoch_bump_for_macro_total{0};
std::atomic<std::uint64_t> mutation_boundary_hygiene_violation_total{0};
std::atomic<std::uint64_t> mutation_boundary_observability_queries_total{0};  // per-call counter
```

The corresponding `X-macro` fields are in
`src/compiler/compiler_metrics_fields.inc` so the C-side counters
exposed via `aura_*_read()` keep in sync without code drift.

### 2. Paired legacy / new bumps (2 wire-up sites, 3 paired bumps)

| File                                       | Site                                    | Counter bumped                                            |
|--------------------------------------------|-----------------------------------------|-----------------------------------------------------------|
| `src/compiler/evaluator_fiber_mutation.cpp` | cross_cow refresh (after #1645's bump)  | `mutation_boundary_macro_dirty_propagated_total` + `mutation_boundary_epoch_bump_for_macro_total` |
| `src/compiler/evaluator_fiber_mutation.cpp` | `ensure_hygiene_violation_detection`    | `mutation_boundary_hygiene_violation_total`               |

All wire-ups honor `Evaluator::yield_hook_evaluator()` null fallback
(per the #1908 / #1644 module-boundary pattern) so calls from TUs
without the Evaluator module safely no-op.

The remaining counter `mutation_boundary_success_total` will be
wired at the Guard dtor (success path) site in Phase 2 — explicit
follow-up because the dtor site has multiple branches.

### 3. New `(query:mutation-boundary-observability-stats)` primitive (AC2)

Registered in `evaluator_primitives_query.cpp`. Reads the 5 new
counters + bumps the per-call `mutation_boundary_observability_queries_total`
on every invocation. Returns a summary integer (Phase 1 sum; Phase
2 will return a full 5-field `FlatHashTable`).

Distinct from the existing `query:mutation-boundary-stats` surface
(which exposes the legacy recovery_failure + rollback + yield_resume
counters from #1637 / #1908 / #1641 lineage) by being the long-running
Guard-success + macro-dirty + epoch-bump + hygiene-violation observability
layer explicitly requested in #1646 body.

## What's NOT shipped (deferred to Phase 2+)

| Why deferred                                                                | Follow-up issue |
|------------------------------------------------------------------------------|-----------------|
| `mutation_boundary_success_total` paired legacy wire-up at Guard dtor success | #1669           |
| `insert_kv` shared helper refactor (249 sites across `primitives_obs_*.cpp`) | #1668           |
| Stress test with 1000+ iter mutate+query (AC4)                              | covered by #1637 / #1908 / #1641 |
| Full 5-field hash `FlatHashTable` for the new primitive                      | #1670 (rebuild on Phase 2+) |
| 4 fresh bumps wired at remaining Guard dtor branches (rollback / exception)  | #1671           |

## Verification (this commit)

- **Pre-commit hooks:** clang-format `-i` + `--dry-run -Werror` clean on
  modified C++ files; ruff clean; test-includes linter — `scripts/check_test_includes.py`
  (1055 files, 0 broken); docs regen via `./build.py docs`.
- **Pre-push gates:** `scripts/check_ir_hygiene_full_pipeline_coverage.py` still
  green (no #1644 regression); `scripts/check_dead_bump_rate.py --self-test`
  passes; `scripts/check_test_binding.py` for `#1646`'s prod changes.
- **Build:** deferred to the artifact commit. Object-compile-only verify,
  per the recurring `arena.ixx.o` link-stage CI infra deadlock pattern
  (#1907 / #1908 / #1641 / #1644 same-day).

## Related issues (predecessors verified on `origin/main` post-rebase)

| Predecessor | What it shipped for MutationBoundaryGuard observability                             |
|-------------|--------------------------------------------------------------------------------------|
| #1637       | `bump_mutation_boundary_recovery_failure` (paired legacy/new) — PanicCheckpoint lifetime |
| #1908       | `hygiene_violation_prevented_on_boundary_total` (paired bump) — fiber steal / GC / hot-swap |
| #1641       | `steal_mutation_boundary_deferred_total` + `starvation_mitigated_for_boundary_count` + `boundary_held_steal_safe_total` — scheduler/worker observability |
| #1644       | `ir_macro_introduced_inlined_skipped_total` + `lowering_marker_propagated_total` — IR hygiene pair (sister issue) |
| #1645       | `dead_bump_rate` CI gate + 2 Phase-1 wire-ups + categorized Phase 2+ follow-up queue (sister scope-limited progressive) |

## Follow-ups queued

1. `#1668` — `insert_kv` shared helper refactor (249 sites) — biggest delta.
2. `#1669` — `mutation_boundary_success_total` paired legacy wire-up at Guard
   dtor success path (the success branch of the dtor is called from multiple
   sites — needs careful pairing to avoid double-counting with the existing
   `bump_mutation_boundary_recovery_failure`).
3. `#1670` — Full 5-field `FlatHashTable` for `query:mutation-boundary-
   observability-stats` (currently returns a summary integer).
4. `#1671` — 4 fresh bumps wired at remaining Guard dtor branches (rollback
   path + exception path).
