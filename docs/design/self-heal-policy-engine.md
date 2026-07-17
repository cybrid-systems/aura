# Self-heal policy engine

**Issues:** #1203 (Phase 1 hooks), #1582 (policy engine + graceful drain)  
**Surfaces:** `src/core/self_healing_hooks.h`, `resource:quota-check`, panic restore

## Problem

Phase 1 only registered hooks and fired them on quota rejection. There was no
default policy for quota / recoverable panic, no degrade / graceful-drain path,
and no first-class `self_heal_success_total` metric.

## Contract (#1582)

```
quota-check reject ──► run_self_heal_engine(quota-violation)
panic restore ok   ──► run_self_heal_engine(recoverable-panic)
graceful-drain kind──► request_graceful_drain()

default policy:
  graceful-drain kind/message  → GracefulDrain
  quota code ≥ 1e6             → Degrade
  quota / recoverable-panic    → LimitedSelfMutate
  other                        → None

then: run registered hooks; success if policy.healed || any hook ok
      → self_heal_success_total++
```

| API | Role |
|-----|------|
| `run_self_heal_engine` | Policy + hooks closed loop |
| `default_self_heal_policy` | Built-in quota/panic/drain policy |
| `set_self_heal_policy` | Host override |
| `request_graceful_drain` / `complete_graceful_drain` | Drain lifecycle |
| `self_heal_success_total()` | Primary AC metric |

CompilerMetrics mirrors: `self_heal_success_total`, `self_heal_degrade_total`,
`self_heal_graceful_drain_total` (bumped on production quota/panic paths).

## Tests

| File | Role |
|------|------|
| `tests/test_self_heal_policy_engine.cpp` | AC1–AC6 engine + service path |
| `tests/test_production_sweep_1202_1228.cpp` | #1203 quota-check smoke (still valid) |

## Non-goals

- Full self-mutate of workspace source (limited path is hook/host extensible).
- Scheduler worker drain orchestration (flag + metrics only; host completes drain).
