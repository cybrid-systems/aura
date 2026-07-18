# Dual-lock order: closures → env_frames (#1664)

**Issue:** [#1664](https://github.com/cybrid-systems/aura/issues/1664)  
**Builds on:** #1486 · #1485 · #1557  
**Status:** P1 correctness — eliminate reverse lock-order deadlock footgun.

## Canonical order

When **both** mutexes are held:

```
1. closures_mtx_     (shared or unique)
2. env_frames_mtx_   (shared)
```

| Path | Order |
|------|-------|
| `scan_live_closures_for_linear_captures` | closures unique → env shared |
| `probe_linear_ownership_at_gc_safepoint` | closures shared → env shared (**fixed #1664**) |
| `probe_linear_ownership_on_fiber_steal` | via `enforce` → scan (same order) |
| `resync_live_closure_env_versions_on_invalidate` | closures → env |
| `apply_closure` | closures shared (then materialize takes env alone) |

## Exception

`compact_env_frames` takes `compact_env_frames_lock_` first, then env unique,
then briefly closures. Documented separately; interlock serializes with
module load.

## Fix (#1664)

`probe_linear_ownership_at_gc_safepoint` previously took **env then closures**.
Swapped to match scan. Field comments on `closures_mtx_` / `env_frames_mtx_`
restate the dual-lock contract.

## Tests

`tests/test_lock_order_closures_env_1664.cpp` — concurrent scan + probes + apply
without hang.
