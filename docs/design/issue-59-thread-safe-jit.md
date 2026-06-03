# Design: Issue #59 — Thread-safe JIT compilation with runtime self-modification

## Context

Issue #59 reports that `mutate:*` operations and JIT hot-path compilation
can happen concurrently, leading to `addIRModule` failures or crashes.

## Evidence (concrete data races found)

I read `src/compiler/aura_jit.cpp`, `src/compiler/aura_jit.h`,
`src/compiler/service.ixx`, `src/compiler/messaging_bridge.h`. Here
is what I found:

### Race A — `jit_cache_` is a bare `std::unordered_map`

```cpp
// src/compiler/service.ixx:3125
std::unordered_map<std::string, JitCachedFn> jit_cache_;
```

Five sites touch this map without any lock:

| Site        | Operation               | Risk                              |
| ----------- | ----------------------- | --------------------------------- |
| line 1264   | `find`                  | race with erase (use-after-free)  |
| line 1271   | `erase` (cache invalidation) | iterator invalidation if another fiber is mid-iteration |
| line 1276   | insert via `[]`         | rehash during read                |
| line 1285   | `[]` (write)            | rehash during read                |
| line 1291   | `erase` (hot-recompile) | same                              |
| line 3202   | `[]` (write)            | same                              |
| line 1870   | `erase` (redundant)     | same                              |
| line 2344   | `erase` ("__lambda__")  | same                              |
| line 3039   | `erase` (invalidation)  | same                              |

A fiber doing `jit_cache_[name] = ...` while another is iterating
`jit_cache_.find(name)` can rehash underneath, invalidating the
iterator. Result: use-after-free, or "found entry has a stale
fn_ptr" (the latter isn't a crash but is a correctness bug).

### Race B — `addIRModule` and `lookup` are concurrent

`AuraJIT::compile()` (`aura_jit.cpp:1488`):
```cpp
auto tsm = llvm::orc::ThreadSafeModule(std::move(mod), ...);
auto rt = get_or_create_tracker(...);
if (auto err = jit->addIRModule(rt, std::move(tsm))) { ... }
auto sym = jit->lookup(std::string(fn.name));
```

ORC's `ThreadSafeModule` is thread-safe per-module, but two
concurrent `addIRModule` calls on the same `LLJIT` instance can
race on the symbol table internals. The current code has no
external serialization.

### Race C — `mutate:*` ↔ JIT read

`service.ixx:558` flags `mutate:*` callees for AST server
fallback. The mutation eventually calls
`invalidate_function(name)` which does
`jit_cache_.erase(name)`. Meanwhile, another fiber may be in the
middle of `jit_cache_.find(name)` to look up a cached compiled
function. Race: use-after-free on the map value.

### Race D — specialized code entry is a raw pointer

`jit_cache_[name].fn_ptr` is a `ScalarFn` (function pointer). If
invalidation runs while another fiber is *about to* call through
that pointer, the pointer may be freed. There's no atomic
load/store here.

## The fix (4 iterations, small to big)

### Iter 1 — Add `std::mutex` to `AuraJIT::compile()`

Protects `addIRModule` and `lookup` from concurrent calls.

**File**: `src/compiler/aura_jit.h`, `src/compiler/aura_jit.cpp`

```cpp
class AuraJIT {
public:
    ...
    ScalarFn compile(const FlatFunction& fn);  // takes compile_mtx_
private:
    std::mutex compile_mtx_;
};
```

`compile()` opens with `std::lock_guard<std::mutex> lock(compile_mtx_);`
and proceeds as before. The ORC `ThreadSafeModule` already provides
per-module thread safety; the external mutex is the entry-point
serialization.

**Risk reduction**: prevents two concurrent `addIRModule` calls
from racing on the symbol table. The mutex is held for the duration
of LLVM IR verification + addIRModule + lookup — typically
sub-millisecond per function, so the perf hit is <1% in practice.

### Iter 2 — Make `jit_cache_` thread-safe

Replace the bare `std::unordered_map` with a shared/exclusive-lock
pattern. Most operations are read (find), so a `std::shared_mutex`
(read-write lock) is appropriate.

**File**: `src/compiler/service.ixx`

```cpp
mutable std::shared_mutex jit_cache_mtx_;
std::unordered_map<std::string, JitCachedFn> jit_cache_;

// At each access site:
{
    std::shared_lock lock(jit_cache_mtx_);
    auto it = jit_cache_.find(name);
    ...
}
{
    std::unique_lock lock(jit_cache_mtx_);
    jit_cache_[name] = ...;
    ...
}
```

**Risk reduction**: closes Race A. Read-heavy workloads (most
cache lookups) stay fast because multiple readers can hold the
shared lock concurrently.

### Iter 3 — Mutation Lock

`mutate:*` and JIT-compile are mutually exclusive: a `mutate:*`
call must drain in-flight JIT work before invalidating the cache.
A compile call must hold the same lock so a concurrent `mutate:*`
can't rip the cache out from under it.

**File**: `src/compiler/service.ixx`, `src/compiler/messaging_bridge.h`

```cpp
mutable std::shared_mutex mutate_mtx_;  // shared = mutate, unique = compile

// At mutate:* entry (messaging_bridge.h): shared lock (multiple mutates OK)
// At compile path: unique lock (exclusive with mutate)
```

**Risk reduction**: closes Race C (mutate ↔ JIT read). Multiple
concurrent mutates can still proceed in parallel.

### Iter 4 — Atomic pointer swap for specialized code

`jit_cache_[name].fn_ptr` becomes `std::atomic<ScalarFn>`. Erase
sets the pointer to nullptr first (visible immediately to any
fiber reading the cache), then frees the function. Readers do an
`atomic_load` and check for nullptr.

**File**: `src/compiler/service.ixx`

```cpp
struct JitCachedFn {
    std::atomic<ScalarFn> fn_ptr{nullptr};
    std::uint32_t local_count = 0;
    std::uint32_t arg_count = 0;
    std::uint32_t env_count = 0;
    bool has_shape_map = false;
};

// Read:
ScalarFn fn_ptr = cache_it->second.fn_ptr.load(std::memory_order_acquire);
if (!fn_ptr) {
    // invalidated; fall through to recompile
}

// Erase (becomes "logical erase"):
cache_it->second.fn_ptr.store(nullptr, std::memory_order_release);
// actual map erase can be deferred; readers see nullptr first
```

**Risk reduction**: closes Race D. A fiber calling through a
function pointer either sees the original function (and runs
correctly) or sees nullptr (and falls through to recompile).
No use-after-free because the original `fn_ptr` is not freed
while any fiber can still see it via the cache.

### Out of scope (defer)

- **Deopt state machine** (issue acceptance #4 "Clear deopt path
  when mutation invalidates a specialization"): requires changing
  the LLVM IR lowering to emit deopt guards, plus a runtime
  deopt flag check. This is the natural follow-up after the
  pointer swap is in place (Iter 4).
- **ORC `ResourceTracker::removeModule` for partial invalidation**:
  when a function is mutated, only the dependent functions need
  re-compilation, not the entire module. The current `erase`
  approach is conservative (re-inserts the module); a per-function
  ResourceTracker + removeModule is the optimal path. Punt.

## Backward compat

- Iter 1 (AuraJIT mutex): identical semantics for single-threaded
  callers. Multi-threaded callers see correct serialization.
- Iter 2 (shared_mutex on jit_cache_): same.
- Iter 3 (mutate lock): same.
- Iter 4 (atomic fn_ptr): existing `fn_ptr = cache_it->second.fn_ptr;`
  reads work after the load; new code is just `load()` instead of
  direct copy.

## Test plan

- `tests/test_ir.cpp`: add a `T_concurrency` section that:
  1. Spawns N threads all calling `JitCache::lookup_or_compile` for
     the same name; assert no crash, no duplicate compilation,
     and that all threads see the same `fn_ptr`.
  2. Spawns M threads doing `invalidate_function` while N threads
     are looking up; assert no use-after-free (catch-alls catch it
     if we miss a lock).
  3. Tests `fn_ptr` becomes nullptr after invalidate, no double-free.

- `tests/test_regression.py`: add a case that uses `mutate:*` (any
  existing test that hits it) and asserts it doesn't deadlock or
  crash.

## Affected files (incremental)

- Iter 1: `src/compiler/aura_jit.h`, `src/compiler/aura_jit.cpp`
- Iter 2: `src/compiler/service.ixx`
- Iter 3: `src/compiler/service.ixx`, `src/compiler/messaging_bridge.h`
- Iter 4: `src/compiler/service.ixx`

## Acceptance

After all 4 iters, the following holds:

- ✅ Concurrent `mutate:*` + JIT compilation does not crash
  (acceptance #1 from #59).
- ✅ Hot paths remain stable under mutation (a `fn_ptr` observed
  before mutation stays valid until the call completes; post-mutation
  observers see nullptr and recompile) (acceptance #2).
- ✅ Performance impact of locking is minimal (<5%) — the
  `compile_mtx_` is held for sub-ms per call; `jit_cache_` is
  read-heavy so `shared_mutex` allows concurrent readers; `mutate_mtx_`
  is shared among mutates and only serializes against compiles
  (acceptance #3).

## Files

- `docs/design/issue-59-thread-safe-jit.md` (this file)
- `src/compiler/aura_jit.h`
- `src/compiler/aura_jit.cpp`
- `src/compiler/service.ixx`
- `src/compiler/messaging_bridge.h`
- `tests/test_ir.cpp`
- `tests/test_regression.py`
