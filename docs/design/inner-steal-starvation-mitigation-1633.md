# Nested MutationBoundary steal defer + starvation mitigation mandate (#1633)

**Issue:** [#1633](https://github.com/cybrid-systems/aura/issues/1633)  
**Builds on:** #1492 · #1445 · #1254 · #783  
**Status:** Production mandate — inner defer always mitigates; long-hold hooks scheduler.

## Problem

Work-steal must not move a fiber inside a nested `MutationBoundaryGuard`
(`depth > 0`). Simple defer is safe but can starve other agent fibers under
long nested mutations if priority is never boosted.

## Contract

```
WorkerThread::try_steal:
  if stealable && outermost-safe (depth==0):
      steal; clear_steal_priority_boost(); return success
  if stealable && yield==MutationBoundary:
      bump_deferred
      if is_at_inner_mutation_boundary():   // depth > 0
          bump_deferred_inner
          apply_starvation_mitigation(fiber)  // #1492 / #1633 MANDATE
      else if deferred_count > 3:
          threshold boost (#1270 / #1445)
      put fiber back; continue

MutationBoundaryGuard dtor (hold > long_mutation_threshold_us):
  aura_invoke_long_mutation_scheduler_hook(fiber_id, duration_us)
    → g_scheduler->on_long_mutation_held(fiber_id, duration)
         → apply_starvation_mitigation(fiber) when resolvable by id
         → else bump metrics only

Scheduler::run:
  aura_set_long_mutation_scheduler_hook(trampoline)  // #1633 wire
  … event loop …
  aura_set_long_mutation_scheduler_hook(nullptr)
```

### `apply_starvation_mitigation(fiber)`

1. `fiber->apply_steal_priority_boost()` — one-shot lift once fiber becomes outermost-safe  
2. Bump `deferred_pressure_boosts`  
3. Bump `starvation_priority_boosts` / `steal_priority_boost_triggered`  
4. Bump **`steal_inner_deferred_starvation_mitigated_count`**

## Metrics (`query:orchestration-steal-stats`, schema **1633**)

| Field | Meaning |
|-------|---------|
| `steal-deferred-inner-boundary` | Raw inner defers |
| `steal-inner-deferred-starvation-mitigated-count` | Mitigation applications |
| `steal_inner_deferred_starvation_mitigated_count` | Alias (#1633 issue body) |
| `starvation-mitigated-count` | Long-mutation hook events |
| `inner-defer-mitigation-wired` / `long-mutation-hook-wired` | 1 |
| `schema` | **1633** (lineage 1492 / 1445) |

## Tests

| File | Role |
|------|------|
| `tests/test_inner_steal_starvation_1633.cpp` | **#1633** AC + 50+ fiber stress |
| `tests/test_issue_1492.cpp` | Original API / mitigation |
| `tests/test_orchestration_steal_boost.cpp` | Schema lineage |

## Related

- `docs/design/inner-steal-starvation-mitigation.md` (#1492)
- `docs/design/mutation-boundary-fairness.md`
