# Closure dual-epoch apply enforcement

**Issues:** #1475, #1490, #1491, **#1558**, #1485, #1508  
**Related:** #1557 (walk_active_closures), linear post-mutate (#1478)

## Problem

AI multi-round `mutate:rebind` / `set-body` (and concurrent fiber steal / GC compact) can leave live tree-walker Closures and JIT closures with:

- `bridge_epoch` ≠ `current_bridge_epoch()`
- EnvFrame `version_` < `defuse_version_`

Calling those closures without a dual check risks UAF of `flat*` / `pool*` or wrong linear captures.

## Contract (every apply entry)

| Path | Gate | On stale |
|------|------|----------|
| `apply_closure` map (TW) | `closure_needs_safe_fallback` = bridge + env + linear | bridge re-dispatch / nullopt; metrics |
| `apply_closure` race window pre-`eval_flat` | re-check bridge + **env frame** (#1558) | same |
| `apply_closure` local-miss IR bridge | `invoke_closure_bridge_checked` | IR recovery |
| JIT `aura_closure_call` / OpApply | `aura_is_jit_closure_fresh` (#1508/#1491) | deopt + return 0 |

```
if bridge_stale || env_stale || linear_violation:
    bump stale_closure_prevented / closure_epoch_mismatch_fallback
    try bridge recovery OR refuse (no dangling eval)
```

## Fiber resume / steal (#1490 / #1558)

| Hook | Action |
|------|--------|
| `Fiber::resume` → `aura_evaluator_resume_fiber_migration` | `transfer_mutation_stack` → refresh + `probe_and_repin_linear_on_steal` |
| `aura_evaluator_post_resume_refresh` | second `refresh_stale_frames_after_steal` + repin |
| Metrics | `post_steal_refresh_count` |

## Metrics (Agent-facing)

| Name | Meaning |
|------|---------|
| `stale_closure_prevented` | Stale catch at apply entry |
| `closure_epoch_mismatch_fallback` | Epoch dual-check → safe fallback |
| `post_steal_refresh_count` | Steal/resume EnvFrame refresh runs |
| `compiler_closure_safe_fallbacks` | Safe fallback dispatches |
| `jit_closure_stale_deopt_total` | JIT dual-check deopts |

## Tests

- `tests/test_issue_1475.cpp` — pure `is_env_frame_stale`
- `tests/test_issue_1490.cpp` — post-steal refresh
- `tests/test_issue_1491.cpp` — apply + JIT dual-check + 1000 concurrent
- `tests/test_issue_1558.cpp` — closed-loop stress (mutate → apply + metrics + resume refresh)
