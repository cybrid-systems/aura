# Issue #1654 — Bridge epoch std::atomic / C11 _Atomic for C++/C memory model safety

## 来源

Production Code Review (2026-07-19): bridge_epoch concurrency audit.
Building on the existing infrastructure laid by #1485 (C2-wire fix-up
intent — bump_bridge_epoch + dual-check), #1476 (dual-epoch primitive),
#1508 / #1491 (JIT closure dual-freshness), #1537 (LLVM IR-level Apply
prologue dual-epoch helpers), and the existing per-Fiber / per-Compiler
Metrics surfaces for the bridge_epoch (aura_jit_bridge.cpp).

## 问题描述

`src/compiler/aura_jit_bridge.cpp` 的 `g_current_bridge_epoch` 是
`static std::uint64_t = 0`,setter / getter 是普通赋值 / 读。
`src/compiler/aura_jit_bridge_stub.cpp` 的 `_stub` 版本同型。
`lib/runtime.c` 的 `g_current_bridge_epoch` 是
`static unsigned long long = 0`,setter / getter 是普通赋值 / 读。

C++ 标准 ([intro.races]) 明确把 plain `uint64_t` 的并发读写标记为
**data race → undefined behavior**。即使在 x86 上写读 64-bit 通常
是 atomic 的:

- 在 weakly-ordered 架构(ARM, POWER)上,plain read 可能返回 stale 或
  torn 值(实际上 ARMv8 64-bit aligned read 不会被 tear,但还是 UB)。
- compiler optimization 可能 reorder / cache plain read 到 register
  (e.g. 编译器发现 setter / getter 是 plain 字段,可能 hoist load 到
  loop entry)。

这导致 #1485 C2-wire fix-up intent 的 2-check 在 jit side
(`AuraJIT::aura_closure_call` 检查 `AuraClosure::bridge_epoch` vs
`aura_get_current_bridge_epoch()`)在 cross-fiber 并发下不可靠:
- set: `service.ixx::bump_bridge_epoch()` 在 fiber A
- read: `lib/runtime.c` 或 `aura_jit_bridge.cpp` 在 fiber B
- 两个 plain 字段的并发读写 = C++ UB

## 代码证据 (code anchors)

### 修复前

```cpp
// src/compiler/aura_jit_bridge.cpp — pre-#1654
static std::uint64_t g_current_bridge_epoch = 0;
extern "C" void aura_set_current_bridge_epoch(std::uint64_t v) {
    g_current_bridge_epoch = v;  // plain write = UB if concurrent
}
extern "C" std::uint64_t aura_get_current_bridge_epoch(void) {
    return g_current_bridge_epoch;  // plain read = UB if concurrent
}
```

```cpp
// src/compiler/aura_jit_bridge_stub.cpp — pre-#1654
static std::uint64_t g_current_bridge_epoch_stub = 0;
extern "C" __attribute__((weak)) void aura_set_current_bridge_epoch(std::uint64_t v) {
    g_current_bridge_epoch_stub = v;
}
extern "C" __attribute__((weak)) std::uint64_t aura_get_current_bridge_epoch(void) {
    return g_current_bridge_epoch_stub;
}
```

```c
// lib/runtime.c — pre-#1654
static unsigned long long g_current_bridge_epoch = 0;
void aura_set_current_bridge_epoch(unsigned long long v) {
    g_current_bridge_epoch = v;
}
unsigned long long aura_get_current_bridge_epoch(void) {
    return g_current_bridge_epoch;
}
```

### 修复后 (Issue #1654 hardened)

```cpp
// src/compiler/aura_jit_bridge.cpp — post-#1654
// Issue #1654: std::atomic<std::uint64_t> replaces the plain uint64_t to close
// the C++ memory model data race.
static std::atomic<std::uint64_t> g_current_bridge_epoch{0};

extern "C" void aura_set_current_bridge_epoch(std::uint64_t v) {
    g_current_bridge_epoch.store(v, std::memory_order_release);
}

extern "C" std::uint64_t aura_get_current_bridge_epoch(void) {
    return g_current_bridge_epoch.load(std::memory_order_acquire);
}
```

```cpp
// src/compiler/aura_jit_bridge_stub.cpp — post-#1654
// Issue #1654: same std::atomic conversion — uniform atomicity with production
// impl so test binaries that don't link the production impl still benefit.
static std::atomic<std::uint64_t> g_current_bridge_epoch_stub{0};

extern "C" __attribute__((weak)) void aura_set_current_bridge_epoch(std::uint64_t v) {
    g_current_bridge_epoch_stub.store(v, std::memory_order_release);
}

extern "C" __attribute__((weak)) std::uint64_t aura_get_current_bridge_epoch(void) {
    return g_current_bridge_epoch_stub.load(std::memory_order_acquire);
}
```

```c
// lib/runtime.c — post-#1654
// Issue #1654: _Atomic + atomic_*_explicit mirrors the C++ std::atomic
// conversion — C11 atomics give the same release/acquire protocol as
// std::atomic. <stdatomic.h> provides the type + API.
#include <stdatomic.h>  // added for _Atomic + atomic_*_explicit

static _Atomic unsigned long long g_current_bridge_epoch = 0;
void aura_set_current_bridge_epoch(unsigned long long v) {
    atomic_store_explicit(&g_current_bridge_epoch, v, memory_order_release);
}
unsigned long long aura_get_current_bridge_epoch(void) {
    return atomic_load_explicit(&g_current_bridge_epoch, memory_order_acquire);
}
```

## 精确改动位置 (file-by-file)

1. **src/compiler/aura_jit_bridge.cpp** — `static std::uint64_t g_current_bridge_epoch`
   → `static std::atomic<std::uint64_t> g_current_bridge_epoch{0}`;
   setter: plain write → `.store(v, std::memory_order_release)`;
   getter: plain read → `.load(std::memory_order_acquire)`.
   2-line comment explaining the rationale (memory model UB + weakly-ordered
   architectures + #1485 C2-wire intent).

2. **src/compiler/aura_jit_bridge_stub.cpp** — same conversion pattern,
   uniform atomicity with production impl (so test binaries that don't link
   the production impl still get the same release/acquire protocol). 2-line
   comment.

3. **lib/runtime.c** — `#include <stdatomic.h>` added; `static unsigned long
   long g_current_bridge_epoch` → `static _Atomic unsigned long long
   g_current_bridge_epoch`; setter: plain write → `atomic_store_explicit(&,
   v, memory_order_release)`; getter: plain read → `atomic_load_explicit(&,
   memory_order_acquire)`.

4. **tests/test_issue_1654.cpp** (new, ~280 lines, 10 ACs source-driven +
   runtime baseline + concurrent stress).

5. **scripts/check_bridge_epoch_atomic_coverage.py** (new, ~200 lines,
   10 ACs linter).

6. **CMakeLists.txt** — add `aura_add_issue_test(test_issue_1654)` +
   `aura_issue_test_link_llvm_jit(test_issue_1654)` entries.

## 验收标准 (AC)

| AC | Verification |
| --- | --- |
| AC1 | `test_issue_1654` AC1: `aura_jit_bridge.cpp` uses `std::atomic<std::uint64_t>` (legacy plain `uint64_t` removed) |
| AC2 | AC2: `aura_jit_bridge.cpp` setter / getter use `.store(release)` + `.load(acquire)` |
| AC3 | AC3: `aura_jit_bridge_stub.cpp` uses `std::atomic<std::uint64_t>` |
| AC4 | AC4: `aura_jit_bridge_stub.cpp` setter / getter use `.store(release)` + `.load(acquire)` |
| AC5 | AC5: `lib/runtime.c` uses `_Atomic unsigned long long` (legacy plain `unsigned long long` removed) |
| AC6 | AC6: `lib/runtime.c` setter / getter use `atomic_store_explicit(release)` + `atomic_load_explicit(acquire)` |
| AC7 | AC7: `lib/runtime.c` includes `<stdatomic.h>` |
| AC8 | AC8: legacy `extern "C"` signatures preserved (no ABI change — call sites in `tests/test_issue_1485.cpp` AC10 still compile) |
| AC9 | AC9: cross-layer baseline roundtrip — `aura_set_current_bridge_epoch(42) → 42`, `(7) → 7`, `(0) → 0` (last-write-wins + reset) |
| AC10 | AC10: concurrent stress — 4 threads × 10000 iters concurrent set+get, all reads in valid range (no torn reads) |

10 linter ACs in `scripts/check_bridge_epoch_atomic_coverage.py`:
production-code wire-up checks for the 3 wire-up sites + atomic type
presence + release/acquire ordering + `<stdatomic.h>` include + legacy
`extern "C"` signature preservation.

## 预期收益

- **Closes the C++ memory model UB** that #1485's C2-wire fix-up intent
  was supposed to defend against. The 2-check now has authoritative
  cross-fiber safety (release on set, acquire on read).
- **C-side parity** — `lib/runtime.c` is also used by the JIT runtime
  (`aura_jit_runtime.cpp:880` `aura_closure_call` wrapper) and by
  AOT-emitted C registration code. C11 atomics give the same release /
  acquire protocol as C++ `std::atomic` on the C side.
- **Weakly-ordered architecture support** — ARM / POWER read is now
  guaranteed atomic + ordered. The previous plain uint64_t was UB on
  these architectures even if in practice rarely torn.
- **Builds on #1485 + #1476** — does not change the bridge_epoch
  semantics, only the storage type. All existing observability metrics
  (per-Fiber `bump_cross_fiber_mutation_safe_steal`, per-CompilerMetrics
  `closure_bridge_epoch_safety_enforced`, etc.) continue to work.
- **Test coverage** — 10 ACs including concurrent stress (4 threads ×
  10K iters) that exercises the release/acquire protocol under
  contention.

## 优先级

**P1** (production-readiness, weakly-ordered architecture safety, C++
memory model UB closure)

## 标签

P1, aot, jit, bridge, epoch, atomicity, memory-model,
production-readiness, weakly-ordered, arm, power

## 相关 Issues

- #1485 (C2-wire fix-up intent — bump_bridge_epoch + dual-check)
- #1476 (dual-epoch primitive — std::atomic release/acquire pattern already
  used in service.ixx::bump_bridge_epoch)
- #1508 / #1491 (JIT closure dual-freshness)
- #1537 (LLVM IR-level Apply prologue dual-epoch helpers)
- #1365 (stamp bridge_epoch + strict is_bridge_stale)
- #1368 (per-evaluator AOT state multi-agent isolation)

## 验证方式

- `tests/test_issue_1654.cpp`: 10 ACs all green (source-driven + runtime
  baseline roundtrip + concurrent stress)
- `scripts/check_bridge_epoch_atomic_coverage.py`: 10 ACs all green
- pre-commit hooks: clang-format clean, `./build.py docs` regen clean,
  `gen_docs.py --check` verify clean
- pre-push gates: primitive surface freeze (no new primitives added —
  existing bridge_epoch surface extended within 521 budget per #1734
  raise) + test-registry (#1572) + test binding + coverage (#1453)
- Same PR cycle as #1640 / #1641: edit → build → run tests → descriptive
  commit → push `main` (direct, no PR review per MEMORY.md workflow).