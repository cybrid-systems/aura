# PanicCheckpoint ↔ GC defer closed-loop

**Issues:** #1489 (wire-up), #651 (metrics scaffolding), #1479 / #1014 / #1446 (re-pin)  
**Surfaces:** `gc_hooks`, `block_gc_for_pending_checkpoint_trampoline`, `request_gc_safepoint`, `compact_sweep`, `GCCollector::collect`

## Problem

While a `PanicCheckpoint` is live (outermost `MutationBoundaryGuard` save → commit/restore), GC `compact_sweep` can erase closures / reclaim heap state that the checkpoint and pinned COW / `StableNodeRef` / `EnvFrame` still depend on — especially across fiber steal + yield.

Pre-#1489, `block_gc_for_pending_checkpoint_trampoline` only bumped a counter (TODO: real defer).

## Contract

```
save_panic_checkpoint()  ──arm──►  gc_hooks depth++
Fiber::yield(MutationBoundary) + pending  ──►  re-arm (idempotent) + signal metric
request_gc_safepoint()   ──if depth>0 or has_cp──►  return 1 (deferred)
compact_sweep()          ──if deferred──►  skip reclaim, note skip
GCCollector::collect()   ──if deferred──►  abort cycle (no STW sweep)
commit / restore success ──release──►  depth--
```

| Layer | Behavior |
|-------|----------|
| `gc_hooks::g_gc_defer_pending_panic_depth` | Process-wide; multi-evaluator composable |
| `Evaluator::gc_defer_armed_for_panic_cp_` | Per-evaluator idempotent arm |
| Metrics | `gc_panic_pending_deferral_total`, `gc_blocked_by_panic_total`, `gc_panic_conflict_resolved_total` via `query:gc-panic-deferral-stats` |
| Re-pin | Unchanged: `on_arena_compact_hook` / `re_pin_cow_children_from_snapshot` still run after window closes |

## AC map (#1489)

| AC | Shipped |
|----|---------|
| 1 Trampoline sends defer signal | Arm + `note_gc_defer_pending_panic_signal` + metrics |
| 2 Avoid compact_sweep in pending window | Early return in `compact_sweep` + `collect()` abort |
| 3 Restore GC after recovery | `release_gc_defer_for_pending_panic` on commit/restore |
| 4 Concurrent steal/panic/GC stress | Unit/integration in `test_issue_1489` (+ existing fiber re-pin tests) |
| 5 Integration with #1479/#1014 re-pin | AC6: re_pin callable under pending; compact skip prevents UAF |

## Tests

| File | Role |
|------|------|
| `tests/test_issue_1489.cpp` | Arm/release, request defer, sweep skip, metrics, re_pin |
| `tests/test_issue_651.cpp` | Stats primitive shape (schema 651) |
| `tests/test_fiber_steal_panic_checkpoint_nested_gc.cpp` | Re-pin smoke |

## Non-goals

- Moving live object defrag under the same gate (arena defrag still uses re-pin hooks).
- Changing PanicCheckpoint save/restore semantics beyond GC arm/release.
