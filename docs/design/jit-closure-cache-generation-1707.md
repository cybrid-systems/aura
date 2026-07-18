# Closure inline cache generation seqlock (#1707)

**Issue:** [#1707](https://github.com/cybrid-systems/aura/issues/1707)  
**Files:** `aura_jit_runtime.cpp`, `runtime_shared.h`, `aura_jit_bridge.h`  
**Status:** P0 safety — torn reads on `g_closure_cache` during hot-update.

## Problem

`ClosureCacheEntry` was a plain struct. Invalidate (including under
shared table lock on the stale-deopt path) and fast-path readers could
observe torn `fn` / metadata under concurrent mutation/hot-swap.

## Fix (Option B)

```cpp
struct ClosureCacheEntry {
  std::atomic<uint64_t> generation; // even=stable, odd=write
  int64_t closure_id;
  fn_ptr fn;
  ...
};
```

- Writers: `fetch_add(1)` → write fields → `fetch_add(1)` (release)
- Readers: load g1 (acquire); if odd miss; snapshot fields; load g2;
  if g1!=g2 bump `closure_cache_generation_mismatch_total` and slow path

## Metric

`aura_closure_cache_generation_mismatch_total()`

## Tests

`tests/test_jit_closure_cache_race_1707.cpp`
