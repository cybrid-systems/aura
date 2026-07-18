# Unchecked pair car/cdr safety (#1710)

**Issue:** [#1710](https://github.com/cybrid-systems/aura/issues/1710)  
**Files:** `aura_jit_runtime.cpp`, `runtime_shared.h`  
**Status:** P0 UAF — raw `g_pair_slots[id]` without lock/bounds.

## Problem

`aura_pair_car_unchecked` / `cdr_unchecked` did a no-lock raw index,
relying only on JIT L2 defuse checks. Concurrent `aura_alloc_pair`
realloc or a skipped emit check → silent UAF in hot-update windows.

## Fix

1. Optional TLS L2 defuse stamp (`aura_pair_l2_stamp_defuse`) vs live
   `aura_get_defuse_version()` → bump `unchecked_pair_fallback_total`
2. Always `aura_lock_workspace_read()` before table access
3. Bounds + null-slot check; return 0 on OOB (same as slow path)

## Metric

`aura_unchecked_pair_fallback_total()`

## Tests

`tests/test_pair_unchecked_safety_1710.cpp`
