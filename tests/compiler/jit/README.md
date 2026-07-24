# tests/jit/

JIT / AOT hot-update / metrics regression drivers.

## Suites

| Target | Role |
|--------|------|
| `test_jit_aot_hot_update_batch` | AOT mangle/reload/versioning (#544), incremental hot-update (#1640), stdlib hot-update (#1370), re-emit closure deps (#1480), steal-boundary metrics (#1641) |
| `test_jit_metrics` | AuraJIT::Metrics + per-function cache (#114) — **custom** non-module LLVM target + `test_jit_metrics_stub.cpp` |

## Build / run

```bash
ninja -C build test_jit_aot_hot_update_batch test_jit_metrics
./build/test_jit_aot_hot_update_batch
./build/test_jit_metrics
```

Prefer extending `test_jit_aot_hot_update_batch` over a new `tests/jit/test_*.cpp`.
Do not fold `test_jit_metrics` into the module batch (timespec / no-`import std` constraint).
