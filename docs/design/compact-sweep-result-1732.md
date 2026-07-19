# Typed CompactSweepResult for compact_sweep (#1732)

**Issue:** [#1732](https://github.com/cybrid-systems/aura/issues/1732)  
**Sibling:** [#963](https://github.com/cybrid-systems/aura/issues/963)  
**Files:** `evaluator.ixx`, `evaluator_gc.cpp`, `serve_async.cpp`  
**Status:** P2 type-safety — opaque `void*` return forced unsafe casts.

## Fix (Option B)

```cpp
struct CompactSweepResult {
    std::size_t strings_freed = 0;
    std::size_t pairs_freed = 0;
    std::size_t closures_freed = 0;
    std::size_t fiber_results_freed = 0;
};
CompactSweepResult compact_sweep(void* sweep_buffers); // by value
```

- Layout = 4×`size_t` (matches `GCSweepResultMsg`).
- Null buffers → zeroed result (no heap allocation).
- `serve_async` uses the typed result; messaging `g_gc_sweep` still
  heap-copies into `GCSweepResultMsg*` at the bridge edge.

## Tests

`tests/test_compact_sweep_result_1732.cpp` + updated GC integration tests.
