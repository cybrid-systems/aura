# scan_live_closures vs free/tombstone (#1665)

**Issue:** [#1665](https://github.com/cybrid-systems/aura/issues/1665)  
**Builds on:** #1361 · #1545 · #1486 · #1664  
**Status:** P1 correctness — no re-scan of dead TW slots / counter inflation.

## Two heaps (do not mix)

| Heap | Free mechanism | Scan |
|------|----------------|------|
| **JIT runtime** (`g_closure_*` in `aura_jit_runtime.cpp`) | `aura_free_closure` → `g_closure_freed[id]=1` | N/A to TW scan |
| **Tree-walker** `Evaluator::closures_` | erase / GC sweep / `erase_active_closure` | `scan_live_closures_*` |

`aura_closure_is_freed` treats OOB ids as freed — **cannot** gate TW scan
(would skip live TW entries whose ids fall outside the JIT table).

## Fix (#1665)

1. **Tombstone skip**: if `cl.bridge_epoch == 0` (force_drop / prior mark),
   still count linear capture for audit, but **do not** re-mark or bump
   `linear_live_closures_marked_invalid_total`.
2. **TW free = erase**: `erase_active_closure(id)` + `closure:free!` on
   Closure values erases from `closures_` (was no-op).

## Tests

`tests/test_scan_skip_freed_closures_1665.cpp`

## Related

- #1361 free bitmap (JIT only)
- #1656 extern accessors vs freed bitmap (JIT path)
