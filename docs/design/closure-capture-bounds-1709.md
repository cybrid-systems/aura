# aura_closure_capture multi-vector bounds (#1709)

**Issue:** [#1709](https://github.com/cybrid-systems/aura/issues/1709)  
**Files:** `aura_jit_runtime.cpp`  
**Status:** P1 UAF/correctness — capture must use canonical slot size.

## Problem

Capture only checked `cid < g_closure_envs.size()`, not
`g_closure_func_ids.size()`. Desynced parallel columns could write env
data for a non-allocated func_ids slot.

## Fix

```cpp
static bool closure_slot_in_bounds(size_t cid) {
  return cid < g_closure_func_ids.size() && cid < g_closure_envs.size();
}
// capture: if (!closure_slot_in_bounds(cid)) return;
// + freed-bit refuse (already present)
// NDEBUG: assert_closure_vectors_consistent() on alloc
```

## Tests

`tests/test_closure_capture_bounds_1709.cpp`
