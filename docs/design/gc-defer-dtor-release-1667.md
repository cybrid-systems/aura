# PanicCheckpoint GC defer dtor exception-safety (#1667)

**Issue:** [#1667](https://github.com/cybrid-systems/aura/issues/1667)  
**Builds on:** #1489 · #63723 · #1662  
**Status:** P1 correctness — process-wide `g_gc_defer_pending_panic_depth` must not leak.

## Problem

`#1489` arms process-wide GC defer via `arm_gc_defer_for_pending_panic()` on
`save_panic_checkpoint()`. `commit_panic_checkpoint()` / successful
`restore_panic_checkpoint()` call `release_gc_defer_for_pending_panic()`.

`~Evaluator()` did **not** release. Mid-window destroy (exception, test
teardown, early return without commit) left `g_gc_defer_pending_panic_depth`
elevated until process exit — permanent GC deferral for all Evaluators.

## Failure modes

1. **Dtor mid-window** — save → exception / scope exit → depth stuck at N+1  
2. **commit throws after arm, before release** — was release-last  
3. **restore** — success path releases; failure keeps arm (checkpoint live); dtor covers abandoned windows

## Fix

| Site | Change |
|------|--------|
| `~Evaluator()` | Always `release_gc_defer_for_pending_panic()` (idempotent, noexcept) |
| `commit_panic_checkpoint()` | Release **first**, then clear snapshots / bump counters |
| `restore_panic_checkpoint()` | Keep success-only release; dtor covers abandon |

No separate RAII type required: per-Evaluator `gc_defer_armed_for_panic_cp_`
already makes arm/release idempotent; dtor is the exception-safety backstop
(same pattern as #63723 yield/query unbind and #1662 arena owner clear).

## Tests

`tests/test_gc_defer_dtor_release_1667.cpp` — AC1–6
