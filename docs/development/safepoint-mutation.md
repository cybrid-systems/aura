# Safepoint × Mutation Behavior

Issue #1364 documents and instruments the interaction between GC safepoints
and mutation primitives. The race is **benign** (no UB): `workspace_mtx_`
still serializes AST writes. Telemetry lets AI agents detect “too often in
safepoint while mutating.”

## When does a GC safepoint occur?

1. GC collector `request()` + `collect()` broadcasts a safepoint.
2. Fibers arrive via `Fiber::check_gc_safepoint()` (hooked from arena
   `allocate_raw()` through `gc_hooks::safepoint_check()`).
3. After all workers stop, `GCCollector::collect()` holds
   `aura::gc_hooks::ScopedSafepoint` for mark + sweep.
4. `g_arena_safepoint_active` is true for that STW window.
5. `resume_from_gc()` runs after the scoped guard ends.

## Predicate

```cpp
#include "core/gc_hooks.h"
bool stw = aura::gc_hooks::in_gc_safepoint();
```

## What happens to mutation primitives during safepoint?

| Case | Behavior |
|------|----------|
| In-progress mutation (holds `workspace_mtx_` write) | Completes under the lock. Other fibers are at safepoint and cannot take the write lock until release. |
| New mutation while `in_gc_safepoint()` | May still acquire `workspace_mtx_` after STW ends / on rare overlap. Counts as `mutation_in_safepoint_total++` and `safepoint_collision_total++` on outermost `MutationBoundaryGuard` entry. |
| Fiber at safepoint while holding mutation depth | Counts as `safepoint_yield_on_mutation_total++` (process-wide via `note_safepoint_yield_on_mutation`). |

**No UB.** Correctness relies on existing lock ordering, not on refusing mutations during STW.

## Monitoring

```scheme
(query:safepoint-mutation-stats)
;; → hash with:
;;   in-gc-safepoint                (0/1 snapshot)
;;   mutation-in-safepoint-total
;;   safepoint-yield-on-mutation-total
;;   safepoint-collision-total
```

### Guidance for agents

If `mutation-in-safepoint-total / mutation-boundary wraps` is high for long periods:

- Prefer shorter mutate batches between yields.
- Avoid long-held outermost `MutationBoundaryGuard` across alloc-heavy loops.
- Existing GC safepoint deferral stats (`query:gc-safepoint-stats`) remain complementary.

## Related

- #1256 GC safepoint mutation metrics (fiber wait attribution)
- #439 `query:gc-safepoint-stats`
- Memory Safety Review 2026-07-11 §1.6 / Q3
