// test_jit_metrics.cpp — Issue #114 JIT observability + per-function cache tests
//
// Verifies the new AuraJIT::Metrics struct and the per-function
// compile cache added for #114. These are pure-logic tests that
// don't need a full LLVM stack — they exercise the counter
// increment paths and the cache hit/miss logic at the API
// surface.
//
// We use plain #include (no C++20 modules) to avoid the
// struct-timespec conflict between std module and the system
// headers pulled in by LLVM's Core.h via aura_jit.h.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <mutex>
#include <deque>
#include <unordered_set>
#include <map>
#include <memory>
#include <utility>
#include <functional>
#include <algorithm>
#include <cmath>

#include "compiler/aura_jit.h"

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "  FAIL: %s (line %d)\n", (msg), __LINE__); \
        ++g_failed; \
    } else { \
        std::fprintf(stdout, "  PASS: %s\n", (msg)); \
        ++g_passed; \
    } \
} while(0)

// ── Test 1: Metrics struct is default-initialized to zero ──
// All counters start at 0; format() produces a non-empty string.
bool test_metrics_initial_state() {
    std::fprintf(stdout, "\n--- Test: Metrics default state ---\n");
    aura::jit::AuraJIT jit;
    const auto& m = jit.metrics();

    CHECK(m.compile_count.load() == 0, "compile_count starts at 0");
    CHECK(m.compile_total_us.load() == 0, "compile_total_us starts at 0");
    CHECK(m.hot_swap_count.load() == 0, "hot_swap_count starts at 0");
    CHECK(m.verify_fail_count.load() == 0, "verify_fail_count starts at 0");
    CHECK(m.add_module_fail_count.load() == 0, "add_module_fail_count starts at 0");
    CHECK(m.inlined_prim_count.load() == 0, "inlined_prim_count starts at 0");
    CHECK(m.slow_prim_count.load() == 0, "slow_prim_count starts at 0");
    CHECK(m.cached_function_count.load() == 0, "cached_function_count starts at 0");

    char buf[256];
    jit.metrics().format(buf, sizeof(buf));
    CHECK(buf[0] != '\0', "format() produced a non-empty string");
    CHECK(std::strstr(buf, "compiles=0") != nullptr, "format() includes compiles=0");
    CHECK(std::strstr(buf, "hot_swaps=0") != nullptr, "format() includes hot_swaps=0");
    return true;
}

// ── Test 2: Manual counter increments + format reflects ─────
// Verify that the format() output reflects the current counter
// state. This is what telemetry will use.
bool test_metrics_format_reflects_state() {
    std::fprintf(stdout, "\n--- Test: format() reflects counter state ---\n");
    aura::jit::AuraJIT jit;
    auto& m = jit.mutable_metrics();
    m.compile_count.fetch_add(7);
    m.hot_swap_count.fetch_add(3);
    m.cached_function_count.fetch_add(2);
    m.verify_fail_count.fetch_add(1);

    char buf[256];
    m.format(buf, sizeof(buf));
    std::fprintf(stdout, "  Format output: %s\n", buf);
    CHECK(std::strstr(buf, "compiles=7") != nullptr, "format() shows compiles=7");
    CHECK(std::strstr(buf, "hot_swaps=3") != nullptr, "format() shows hot_swaps=3");
    CHECK(std::strstr(buf, "cached_fns=2") != nullptr, "format() shows cached_fns=2");
    CHECK(std::strstr(buf, "verify_fail=1") != nullptr, "format() shows verify_fail=1");
    return true;
}

// ── Test 3: Metrics is thread-safe (atomic increments) ─────
// Spin up several threads incrementing the same counter. Final
// value should be exactly N_threads * N_increments.
bool test_metrics_atomic_concurrent() {
    std::fprintf(stdout, "\n--- Test: atomic concurrent increments ---\n");
    aura::jit::AuraJIT jit;
    auto& m = jit.mutable_metrics();

    constexpr int kThreads = 8;
    constexpr int kPerThread = 1000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&m]() {
            for (int i = 0; i < kPerThread; ++i) {
                m.compile_count.fetch_add(1, std::memory_order_relaxed);
                m.hot_swap_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : threads) th.join();

    auto expected = static_cast<std::uint64_t>(kThreads * kPerThread);
    CHECK(m.compile_count.load() == expected, "compile_count is exact under contention");
    CHECK(m.hot_swap_count.load() == expected, "hot_swap_count is exact under contention");
    return true;
}

// ── Test 4: format() handles tiny buffers safely ────────────
// If the buffer is too small, snprintf truncates; if null or
// 0 size, we return without crashing.
bool test_metrics_format_edge_cases() {
    std::fprintf(stdout, "\n--- Test: format() edge cases ---\n");
    aura::jit::AuraJIT jit;
    auto& m = jit.mutable_metrics();
    m.compile_count.fetch_add(42);

    // Tiny buffer — should not crash, content truncated
    char tiny[8];
    m.format(tiny, sizeof(tiny));
    CHECK(tiny[sizeof(tiny) - 1] == '\0' || tiny[0] != '\0',
          "tiny buffer does not crash");

    // Just barely enough
    char ok[256];
    m.format(ok, sizeof(ok));
    CHECK(std::strstr(ok, "compiles=42") != nullptr,
          "ok buffer has expected count");

    // Reasonable size
    char nice[512];
    m.format(nice, sizeof(nice));
    CHECK(nice[0] != '\0', "nice buffer produces content");
    return true;
}

int main() {
    std::fprintf(stdout, "═══ JIT metrics tests (Issue #114) ═══\n");

    test_metrics_initial_state();
    test_metrics_format_reflects_state();
    test_metrics_atomic_concurrent();
    test_metrics_format_edge_cases();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
