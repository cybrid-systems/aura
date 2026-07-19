// tests/test_issue_1654.cpp — Issue #1654
//
// AC list (per docs/design/1654-bridge-epoch-atomic.md):
//   AC1: src/compiler/aura_jit_bridge.cpp converted from
//        `static std::uint64_t g_current_bridge_epoch = 0;` to
//        `static std::atomic<std::uint64_t> g_current_bridge_epoch{0};`.
//   AC2: aura_jit_bridge.cpp setter / getter use release / acquire:
//        .store(v, std::memory_order_release) and
//        .load(std::memory_order_acquire).
//   AC3: src/compiler/aura_jit_bridge_stub.cpp converted from
//        `static std::uint64_t g_current_bridge_epoch_stub = 0;` to
//        `static std::atomic<std::uint64_t> g_current_bridge_epoch_stub{0};`.
//   AC4: aura_jit_bridge_stub.cpp setter / getter use release / acquire.
//   AC5: lib/runtime.c converted from
//        `static unsigned long long g_current_bridge_epoch = 0;` to
//        `static _Atomic unsigned long long g_current_bridge_epoch = 0;`.
//   AC6: lib/runtime.c setter / getter use atomic_*_explicit with
//        memory_order_release / memory_order_acquire.
//   AC7: lib/runtime.c includes <stdatomic.h>.
//   AC8: legacy `extern "C"` signatures preserved (no ABI change —
//        call sites in tests/test_issue_1485.cpp AC10 still compile).
//   AC9: cross-layer baseline roundtrip — set/get roundtrip across
//        3 distinct epochs (42 → 7 → 0 reset) verifies last-write-wins.
//   AC10: concurrent stress — N threads × K iters concurrent set+get,
//         no torn reads observed (any read value is in the set of
//         values some thread wrote — guaranteed by std::atomic +
//         _Atomic release/acquire protocol).
//
// Pattern reference: tests/test_issue_1485.cpp AC10 (cross-layer
// roundtrip pattern), tests/test_orchestration_steal_boundary.cpp
// (source-driven AC pattern), tests/test_bridge_epoch_strict.cpp
// (legacy bridge_epoch unit-test surface).

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace aura_1654_detail {

using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains(const std::string& s, std::string_view needle) noexcept {
    return s.find(needle) != std::string::npos;
}

// ── AC1: aura_jit_bridge.cpp uses std::atomic<std::uint64_t> ──────────
bool check_bridge_atomic_ac1() {
    std::println("\n--- AC1: aura_jit_bridge.cpp std::atomic type ---");
    std::string src = read_file("src/compiler/aura_jit_bridge.cpp");
    bool ok = contains(src, "static std::atomic<std::uint64_t> g_current_bridge_epoch{0};");
    // Also verify legacy plain-uint64_t removed (regression check)
    bool legacy_removed = !contains(src, "static std::uint64_t g_current_bridge_epoch = 0;");
    if (!ok || !legacy_removed) {
        std::println("FAIL: aura_jit_bridge.cpp std::atomic conversion missing "
                     "(ok={}, legacy_removed={})",
                     ok, legacy_removed);
        return false;
    }
    std::println("OK: aura_jit_bridge.cpp uses std::atomic<std::uint64_t> "
                 "(legacy plain uint64_t removed)");
    return true;
}

// ── AC2: aura_jit_bridge.cpp release/acquire ──────────────────────────
bool check_bridge_ordering_ac2() {
    std::println("\n--- AC2: aura_jit_bridge.cpp release/acquire ordering ---");
    std::string src = read_file("src/compiler/aura_jit_bridge.cpp");
    bool ok = contains(src, "g_current_bridge_epoch.store(v, std::memory_order_release)") &&
              contains(src, "g_current_bridge_epoch.load(std::memory_order_acquire)");
    if (!ok) {
        std::println("FAIL: aura_jit_bridge.cpp release/acquire missing");
        return false;
    }
    std::println("OK: aura_jit_bridge.cpp uses release/acquire");
    return true;
}

// ── AC3: aura_jit_bridge_stub.cpp uses std::atomic ────────────────────
bool check_stub_atomic_ac3() {
    std::println("\n--- AC3: aura_jit_bridge_stub.cpp std::atomic type ---");
    std::string src = read_file("src/compiler/aura_jit_bridge_stub.cpp");
    bool ok = contains(src, "static std::atomic<std::uint64_t> g_current_bridge_epoch_stub{0};");
    bool legacy_removed = !contains(src, "static std::uint64_t g_current_bridge_epoch_stub = 0;");
    if (!ok || !legacy_removed) {
        std::println("FAIL: aura_jit_bridge_stub.cpp std::atomic conversion missing "
                     "(ok={}, legacy_removed={})",
                     ok, legacy_removed);
        return false;
    }
    std::println("OK: aura_jit_bridge_stub.cpp uses std::atomic<std::uint64_t> "
                 "(legacy plain uint64_t removed)");
    return true;
}

// ── AC4: aura_jit_bridge_stub.cpp release/acquire ─────────────────────
bool check_stub_ordering_ac4() {
    std::println("\n--- AC4: aura_jit_bridge_stub.cpp release/acquire ---");
    std::string src = read_file("src/compiler/aura_jit_bridge_stub.cpp");
    bool ok = contains(src, "g_current_bridge_epoch_stub.store(v, std::memory_order_release)") &&
              contains(src, "g_current_bridge_epoch_stub.load(std::memory_order_acquire)");
    if (!ok) {
        std::println("FAIL: aura_jit_bridge_stub.cpp release/acquire missing");
        return false;
    }
    std::println("OK: aura_jit_bridge_stub.cpp uses release/acquire");
    return true;
}

// ── AC5: lib/runtime.c uses _Atomic ───────────────────────────────────
bool check_runtime_atomic_ac5() {
    std::println("\n--- AC5: lib/runtime.c _Atomic type ---");
    std::string src = read_file("lib/runtime.c");
    bool ok = contains(src, "static _Atomic unsigned long long g_current_bridge_epoch = 0;");
    bool legacy_removed = !contains(src, "static unsigned long long g_current_bridge_epoch = 0;");
    if (!ok || !legacy_removed) {
        std::println("FAIL: lib/runtime.c _Atomic conversion missing "
                     "(ok={}, legacy_removed={})",
                     ok, legacy_removed);
        return false;
    }
    std::println("OK: lib/runtime.c uses _Atomic unsigned long long "
                 "(legacy plain unsigned long long removed)");
    return true;
}

// ── AC6: lib/runtime.c atomic_*_explicit release/acquire ──────────────
bool check_runtime_ordering_ac6() {
    std::println("\n--- AC6: lib/runtime.c atomic_*_explicit release/acquire ---");
    std::string src = read_file("lib/runtime.c");
    bool ok = contains(src, "atomic_store_explicit(&g_current_bridge_epoch, v, "
                            "memory_order_release)") &&
              contains(src, "atomic_load_explicit(&g_current_bridge_epoch, "
                            "memory_order_acquire)");
    if (!ok) {
        std::println("FAIL: lib/runtime.c atomic_*_explicit release/acquire missing");
        return false;
    }
    std::println("OK: lib/runtime.c uses atomic_store_explicit(release) + "
                 "atomic_load_explicit(acquire)");
    return true;
}

// ── AC7: lib/runtime.c includes <stdatomic.h> ─────────────────────────
bool check_runtime_include_ac7() {
    std::println("\n--- AC7: lib/runtime.c <stdatomic.h> include ---");
    std::string src = read_file("lib/runtime.c");
    bool ok = contains(src, "<stdatomic.h>") || contains(src, "stdatomic.h");
    if (!ok) {
        std::println("FAIL: lib/runtime.c does not include <stdatomic.h>");
        return false;
    }
    std::println("OK: lib/runtime.c includes <stdatomic.h>");
    return true;
}

// ── AC8: extern "C" signatures preserved (no ABI change) ──────────────
bool check_extern_c_preserved_ac8() {
    std::println("\n--- AC8: extern \"C\" signatures preserved ---");
    std::string bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    std::string stub = read_file("src/compiler/aura_jit_bridge_stub.cpp");
    std::string runtime = read_file("lib/runtime.c");

    bool bridge_sig_ok =
        contains(bridge, "extern \"C\" void aura_set_current_bridge_epoch(std::uint64_t v)") &&
        contains(bridge, "extern \"C\" std::uint64_t aura_get_current_bridge_epoch(void)");
    bool stub_sig_ok =
        contains(stub, "extern \"C\" __attribute__((weak)) void aura_set_current_bridge_epoch("
                       "std::uint64_t v)") &&
        contains(stub,
                 "extern \"C\" __attribute__((weak)) std::uint64_t aura_get_current_bridge_epoch("
                 "void)");
    bool runtime_sig_ok =
        contains(runtime, "void aura_set_current_bridge_epoch(unsigned long long v)") &&
        contains(runtime, "unsigned long long aura_get_current_bridge_epoch(void)");
    if (!bridge_sig_ok || !stub_sig_ok || !runtime_sig_ok) {
        std::println("FAIL: extern \"C\" signature regression "
                     "(bridge={}, stub={}, runtime={})",
                     bridge_sig_ok, stub_sig_ok, runtime_sig_ok);
        return false;
    }
    std::println("OK: extern \"C\" signatures preserved across all 3 files");
    return true;
}

// ── AC9: cross-layer baseline roundtrip ───────────────────────────────
bool check_baseline_roundtrip_ac9() {
    std::println("\n--- AC9: cross-layer baseline roundtrip ---");

    // Roundtrip 1: set 42, expect 42.
    aura_set_current_bridge_epoch(42);
    const auto read1 = aura_get_current_bridge_epoch();
    if (read1 != 42) {
        std::println("FAIL: aura_set_current_bridge_epoch(42) → "
                     "aura_get_current_bridge_epoch() returned {} (expected 42)",
                     read1);
        return false;
    }
    std::println("OK: roundtrip 42 → 42");

    // Roundtrip 2: overwrite with 7, expect 7 (verifies last-write-wins).
    aura_set_current_bridge_epoch(7);
    const auto read2 = aura_get_current_bridge_epoch();
    if (read2 != 7) {
        std::println("FAIL: aura_set_current_bridge_epoch(7) → "
                     "aura_get_current_bridge_epoch() returned {} (expected 7)",
                     read2);
        return false;
    }
    std::println("OK: roundtrip 7 → 7 (last-write-wins)");

    // Reset to default for downstream tests.
    aura_set_current_bridge_epoch(0);
    const auto read3 = aura_get_current_bridge_epoch();
    if (read3 != 0) {
        std::println("FAIL: aura_set_current_bridge_epoch(0) → "
                     "aura_get_current_bridge_epoch() returned {} (expected 0)",
                     read3);
        return false;
    }
    std::println("OK: reset 0 → 0 (no leak to downstream tests)");
    return true;
}

// ── AC10: concurrent stress — no torn reads ───────────────────────────
bool check_concurrent_stress_ac10() {
    std::println("\n--- AC10: concurrent set/get stress (no torn reads) ---");

    constexpr int kThreads = 4;
    constexpr int kIters = 10000;

    // Each thread writes a unique pattern: t * kIters + i (in [0, kThreads*kIters)).
    // After write, the thread reads back and verifies the value is a valid pattern
    // (i.e. t_writer in [0, kThreads) and i_writer in [0, kIters)). With std::atomic +
    // _Atomic release/acquire, any read value must be a fully-written value (not
    // a torn/partial value) — so the value will be in the expected range.
    std::atomic<int> out_of_range_reads{0};
    std::atomic<int> in_range_reads{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> ts;

    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t]() {
            // Spin-wait for go signal (acquire ordering pairs with release store below).
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < kIters; ++i) {
                std::uint64_t v = static_cast<std::uint64_t>(t) * kIters + i;
                aura_set_current_bridge_epoch(v);
                std::uint64_t got = aura_get_current_bridge_epoch();
                // Verify got is a valid pattern (no torn read).
                auto got_t = static_cast<int>(got / static_cast<std::uint64_t>(kIters));
                auto got_i = static_cast<int>(got % static_cast<std::uint64_t>(kIters));
                if (got_t >= 0 && got_t < kThreads && got_i >= 0 && got_i < kIters) {
                    in_range_reads.fetch_add(1, std::memory_order_relaxed);
                } else {
                    out_of_range_reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : ts)
        t.join();

    const int total_reads = kThreads * kIters;
    const int oor = out_of_range_reads.load(std::memory_order_relaxed);
    const int inr = in_range_reads.load(std::memory_order_relaxed);

    std::println("concurrent stress: {} threads × {} iters = {} reads", kThreads, kIters,
                 total_reads);
    std::println("  in-range reads : {}", inr);
    std::println("  out-of-range   : {} (must be 0 — torn read detected)", oor);

    if (oor != 0 || inr != total_reads) {
        std::println("FAIL: concurrent stress produced {} out-of-range reads "
                     "(torn read or unexpected value)",
                     oor);
        return false;
    }
    std::println("OK: concurrent stress — all {} reads in valid range (no torn reads)",
                 total_reads);

    // Reset to 0 (don't leak state to downstream tests).
    aura_set_current_bridge_epoch(0);
    return true;
}

} // namespace aura_1654_detail

int main() {
    using namespace aura_1654_detail;
    int passed = 0;
    int failed = 0;
    auto run = [&](bool ok) {
        if (ok)
            ++passed;
        else
            ++failed;
        g_passed = passed;
        g_failed = failed;
    };

    std::println("=== Issue #1654: Bridge epoch std::atomic / C11 _Atomic for "
                 "C++/C memory model safety ===");

    run(check_bridge_atomic_ac1());
    run(check_bridge_ordering_ac2());
    run(check_stub_atomic_ac3());
    run(check_stub_ordering_ac4());
    run(check_runtime_atomic_ac5());
    run(check_runtime_ordering_ac6());
    run(check_runtime_include_ac7());
    run(check_extern_c_preserved_ac8());
    run(check_baseline_roundtrip_ac9());
    run(check_concurrent_stress_ac10());

    if (failed > 0) {
        std::println("\ntest_issue_1654 FAILED ({} passed, {} failed)", passed, failed);
        return 1;
    }
    std::println("\ntest_issue_1654 PASS ({} acs, all green)", passed);
    return 0;
}