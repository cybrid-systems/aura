// @category: unit
// @reason: Issue #2437 — reconcile_gc_defer_bits_after_clear TOCTOU: concurrent
//          arm must not lose its just-set Panic bit.
//
//   AC1: Concurrent arm + reconcile does NOT clear another thread's Panic bit
//   AC2: Multi-thread arm + reconcile stress (TSan-friendly)
//   AC3: Bit-vs-depth invariant: Panic bit clear iff depth == 0 (steady state)
//   AC4: #2296 reconcile still clears orphan bit when depth stays 0

#include "test_harness.hpp"

#include "core/gc_hooks.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;

namespace {

using aura::test::g_failed;
using aura::test::g_passed;
namespace gh = aura::gc_hooks;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

void drain_panic() {
    while (gh::g_gc_defer_pending_panic_depth.load(std::memory_order_relaxed) > 0)
        gh::release_gc_defer_pending_panic();
    // Ensure bit clear after drain
    (void)gh::reconcile_gc_defer_bits_after_clear();
}

bool panic_bit_set() {
    const auto mask = gh::g_gc_defer_reasons.load(std::memory_order_acquire);
    return (mask & static_cast<std::uint32_t>(gh::GcDeferReason::Panic)) != 0;
}

bool bit_depth_invariant_holds() {
    const auto depth = gh::g_gc_defer_pending_panic_depth.load(std::memory_order_acquire);
    const bool bit = panic_bit_set();
    // Invariant (steady): depth == 0 ⇒ bit clear; depth > 0 ⇒ bit set.
    if (depth == 0)
        return !bit;
    return bit;
}

} // namespace

int run_test_gc_defer_reconcile_cas_2437() {
    std::println("=== Issue #2437: reconcile_gc_defer CAS + repair ===");

    // ── AC4: orphan bit cleared when depth stays 0 ─────────────────
    {
        std::println("\n--- #2437 AC4: orphan Panic bit cleared (depth 0) ---");
        drain_panic();
        // Force orphan: set Panic bit without depth (simulate race lag).
        (void)gh::arm_defer(gh::GcDeferReason::Panic);
        CHECK(gh::g_gc_defer_pending_panic_depth.load() == 0, "AC4: depth still 0");
        CHECK(panic_bit_set(), "AC4: orphan bit set");
        const auto fixed = gh::reconcile_gc_defer_bits_after_clear();
        CHECK(fixed == 1, "AC4: reconcile fixed 1");
        CHECK(!panic_bit_set(), "AC4: bit cleared");
        CHECK(gh::g_gc_defer_pending_panic_depth.load() == 0, "AC4: depth still 0");
        CHECK(bit_depth_invariant_holds(), "AC4: invariant holds");
    }

    // ── AC1: concurrent arm wins — bit not cleared while depth > 0 ─
    {
        std::println("\n--- #2437 AC1: concurrent arm preserves Panic bit ---");
        drain_panic();
        // Seed orphan bit so reconcile wants to clear.
        (void)gh::arm_defer(gh::GcDeferReason::Panic);
        CHECK(panic_bit_set() && gh::g_gc_defer_pending_panic_depth.load() == 0,
              "AC1: orphan setup");

        std::atomic<bool> start{false};
        std::atomic<int> arm_done{0};
        std::atomic<int> rec_done{0};

        // Armer: wait, then arm depth+bit (like production arm_gc_defer_pending_panic).
        std::thread armer([&] {
            while (!start.load(std::memory_order_acquire)) {
            }
            gh::arm_gc_defer_pending_panic();
            arm_done.fetch_add(1, std::memory_order_release);
        });

        // Reconciler: wait, then reconcile (must not leave depth>0 with bit clear).
        std::thread reconciler([&] {
            while (!start.load(std::memory_order_acquire)) {
            }
            // Spin a little to interleave with armer.
            for (int i = 0; i < 50; ++i)
                (void)gh::reconcile_gc_defer_bits_after_clear();
            rec_done.fetch_add(1, std::memory_order_release);
        });

        start.store(true, std::memory_order_release);
        armer.join();
        reconciler.join();

        CHECK(arm_done.load() == 1 && rec_done.load() == 1, "AC1: both finished");
        CHECK(gh::g_gc_defer_pending_panic_depth.load() == 1, "AC1: depth 1 after arm");
        CHECK(panic_bit_set(), "AC1: Panic bit still set (not cleared under concurrent arm)");
        CHECK(bit_depth_invariant_holds(), "AC1: invariant after concurrent arm+reconcile");
        drain_panic();
    }

    // ── AC2: multi-thread stress (4 arm + 4 reconcile) ─────────────
    {
        std::println("\n--- #2437 AC2: 4×4 concurrent arm + reconcile ---");
        drain_panic();
        constexpr int kThreads = 4;
        constexpr int kIters = 2000;
        std::atomic<bool> start{false};
        std::atomic<int> invariant_fails{0};
        std::vector<std::thread> threads;
        threads.reserve(kThreads * 2);

        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                while (!start.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < kIters; ++i) {
                    gh::arm_gc_defer_pending_panic();
                    // Brief hold so reconciler can race.
                    if ((i & 7) == 0)
                        std::this_thread::yield();
                    gh::release_gc_defer_pending_panic();
                }
            });
            threads.emplace_back([&] {
                while (!start.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < kIters; ++i) {
                    (void)gh::reconcile_gc_defer_bits_after_clear();
                    // Sample invariant under load (may transiently fail mid-op;
                    // only count if depth>0 and bit clear after a quiet load).
                    const auto d =
                        gh::g_gc_defer_pending_panic_depth.load(std::memory_order_acquire);
                    const bool bit = panic_bit_set();
                    if (d > 0 && !bit) {
                        // Re-sample with a quiet window — mid arm/release can
                        // look inconsistent for one load pair under 4×4 stress.
                        for (int s = 0; s < 3; ++s) {
                            std::this_thread::yield();
                            std::this_thread::sleep_for(std::chrono::microseconds(20));
                            const auto ds =
                                gh::g_gc_defer_pending_panic_depth.load(std::memory_order_acquire);
                            const bool bits = panic_bit_set();
                            if (!(ds > 0 && !bits))
                                break; // transient; recovered
                            if (s == 2)
                                invariant_fails.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();
        drain_panic();
        CHECK(invariant_fails.load() == 0, "AC2: no steady-state depth>0 with Panic bit clear");
        CHECK(bit_depth_invariant_holds(), "AC2: invariant after stress");
        CHECK(gh::g_gc_defer_pending_panic_depth.load() == 0, "AC2: depth drained");
        CHECK(!panic_bit_set(), "AC2: bit clear after drain");
    }

    // ── AC3: steady-state invariant helpers ────────────────────────
    {
        std::println("\n--- #2437 AC3: bit-vs-depth invariant ---");
        drain_panic();
        CHECK(bit_depth_invariant_holds(), "AC3: empty state");
        gh::arm_gc_defer_pending_panic();
        CHECK(bit_depth_invariant_holds(), "AC3: after arm");
        gh::arm_gc_defer_pending_panic(); // nested
        CHECK(bit_depth_invariant_holds(), "AC3: nested arm");
        gh::release_gc_defer_pending_panic();
        CHECK(bit_depth_invariant_holds(), "AC3: after one release");
        gh::release_gc_defer_pending_panic();
        CHECK(bit_depth_invariant_holds(), "AC3: after full release");
        // Reconcile no-op when consistent
        CHECK(gh::reconcile_gc_defer_bits_after_clear() == 0, "AC3: no-op when consistent");
    }

    // ── Source-cite CAS + repair ───────────────────────────────────
    {
        std::println("\n--- #2437 source-cite ---");
        auto gh = read_file("src/core/gc_hooks.h");
        CHECK(gh.find("Issue #2437") != std::string::npos, "source-cite #2437");
        CHECK(gh.find("compare_exchange_strong") != std::string::npos &&
                  gh.find("reconcile_gc_defer_bits_after_clear") != std::string::npos,
              "CAS in reconcile");
        CHECK(gh.find("g_gc_defer_bit_reconcile_aborted_total") != std::string::npos,
              "aborted counter");
        CHECK(gh.find("arm_defer(GcDeferReason::Panic)") != std::string::npos, "repair re-arm");
    }

    drain_panic();
    std::println("\n=== #2437 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_gc_defer_reconcile_cas_2437();
}
#endif
