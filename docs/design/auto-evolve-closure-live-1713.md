# Validate auto-evolve ClosureIds before apply (#1713)

**Issue:** [#1713](https://github.com/cybrid-systems/aura/issues/1713)  
**Files:** `evaluator_primitives_agent.cpp`, `observability_metrics.h`  
**Status:** P1 UAF — stored detect/fix ClosureIds must stay live.

## Problem

`auto-evolve-loop` stores raw ClosureIds; `auto-evolve-tick` called
`apply_closure` without checking that the slots were still live after
`(closure:free! ...)`. Same risk on `auto-evolve-once` if args are
freed between type-check and apply.

## Fix

Dual live-gate before each apply:

1. **TW:** `find_active_closure(cid)` (durable free via `erase_active_closure`)
2. **JIT:** `aura_closure_exists(id) && !aura_closure_is_freed(id)`

Never call `aura_closure_is_freed` alone on TW ids (OOR ⇒ "freed").

On dead handle: stop the loop, clear stored ids, bump
`agent_closure_freed_during_tick`, return `#f` / `0`.

## Lifetime contract

Closures passed to `auto-evolve-loop` / `auto-evolve-once` must remain
live for the duration of the loop / once call. Freeing either stops
the next tick gracefully.

## Tests

`tests/test_auto_evolve_closure_live_1713.cpp`
