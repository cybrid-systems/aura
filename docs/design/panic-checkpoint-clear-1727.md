# Clear panic checkpoint on cross-evaluator discriminator skip (#1727)

**Issue:** [#1727](https://github.com/cybrid-systems/aura/issues/1727)  
**Sibling:** [#1393](https://github.com/cybrid-systems/aura/issues/1393), [#1489](https://github.com/cybrid-systems/aura/issues/1489)  
**Files:** `panic_checkpoint_raii.ixx`, `evaluator.ixx`  
**Status:** P1 — discriminator mismatch skipped restore but left
`panic_safe_*` + GC defer armed.

## Fix

1. `Evaluator::clear_panic_checkpoint()` — wipe fields +
   `release_gc_defer_for_pending_panic()` (shared by commit).
2. `PanicCheckpointHost::clear` fn pointer wired from
   `panic_checkpoint_host`.
3. `PanicCheckpointGuard` dtor mismatch path: bump
   `restores_discriminator_failed`, call `clear(ctx)`, bump
   `restores_discriminator_cleared`.

## Tests

`tests/test_panic_checkpoint_clear_1727.cpp`
