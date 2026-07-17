# Inner MutationBoundary steal defer + starvation mitigation (#1492)

**Related:** #1479 (fiber-steal hardening), #1445 (steal-boost hook), #1254 (inner defer), #783 (outermost stats)

## Problem

Work-steal must **not** move a fiber that yielded inside a nested `MutationBoundaryGuard` (`depth > 0`). Simple defer is correct for safety but can starve other agent fibers under long nested mutations.

## Contract

```
try_steal(victim fiber):
  if stealable && outermost-safe (depth==0):
      steal; clear_steal_priority_boost(); return success
  if stealable && yield==MutationBoundary:
      bump_deferred
      if is_at_inner_mutation_boundary():   // depth > 0
          bump_deferred_inner
          apply_starvation_mitigation(fiber)  // #1492
      else if deferred_count > 3:
          threshold boost (#1270 / #1445)
      put fiber back; continue
```

### `apply_starvation_mitigation(fiber)`

1. `fiber->apply_steal_priority_boost()` — one-shot priority lift once fiber becomes outermost-safe  
2. Bump `deferred_pressure_boosts` (adaptive StealBudget stays alert)  
3. Bump `starvation_priority_boosts` / `steal_priority_boost_triggered`  
4. Bump **`steal_inner_deferred_starvation_mitigated_count`** (#1492 AC3)

### Long-mutation linkage

`Scheduler::on_long_mutation_held` also bumps `steal_inner_deferred_starvation_mitigated_count` and boosts the long-holding fiber (when resolvable by id), correlating Guard-exit long-hold telemetry with steal-path fairness.

### Priority

`fiber_steal_priority` raises a boosted, steal-safe fiber to tier 3 (LLM-tail). Boost is cleared on successful steal.

## Metrics (`query:orchestration-steal-stats`, schema **1492**)

| Field | Meaning |
|-------|---------|
| `steal-deferred-inner-boundary` | Raw inner defers |
| `steal-inner-deferred-starvation-mitigated-count` | Mitigation applications |
| `starvation-priority-boosts` / `steal-priority-boost-triggered` | Boost path |
| `starvation-mitigated-count` | Long-mutation hook events |

## Tests

- `tests/test_issue_1492.cpp` — API, mitigation, metrics, nested Guard stress  
- `tests/test_orchestration_steal_boost.cpp` — schema + no spurious boost regression  
