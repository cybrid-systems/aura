// @category: unit
// @reason: Issue #2428 — arm_defer uses fetch_or so first-arm metrics
//          bump exactly once per bit 0→1 (no load+or double-count).
//
//   AC1: arm_defer uses fetch_or (no separate load before note)
//   AC2: concurrent arm_*_defer same reason — TSan-friendly
//   AC3: first-arm metric +1 once per 0→1 (not per concurrent arm)
//   AC4: nested arm / GC depth correctness preserved

#include "test_harness.hpp"

#include "core/gc_hooks.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;

namespace {

using aura::test::g_failed;
using aura::test::g_passed;
namespace gh = aura::gc_hooks;

void drain_ffi() {
    while (gh::ffi_pin_defer_depth() > 0)
        gh::release_ffi_pin_defer();
}
void drain_render() {
    while (gh::render_pin_defer_depth() > 0)
        gh::release_render_pin_defer();
}
void drain_mutation() {
    while (gh::mutation_hold_defer_depth() > 0)
        gh::release_mutation_hold_defer();
}
void drain_panic() {
    while (gh::g_gc_defer_pending_panic_depth.load(std::memory_order_relaxed) > 0)
        gh::release_gc_defer_pending_panic();
}

} // namespace

int main() {
    std::println("=== Issue #2428: arm_defer fetch_or first-arm metrics ===");

    // ── AC3/AC4 single-thread first-arm + nested ───────────────────
    {
        std::println("\n--- #2428 AC3 + #2428 AC4: first-arm once, nested no bump ---");
        drain_ffi();
        drain_render();
        drain_mutation();
        drain_panic();
        gh::reset_gc_defer_arm_metrics_for_test();

        const auto ffi0 = gh::g_gc_defer_arm_ffi_pin_total.load(std::memory_order_relaxed);
        const auto any0 = gh::g_gc_defer_any_total.load(std::memory_order_relaxed);

        gh::arm_ffi_pin_defer(); // first arm → metric +1
        CHECK(gh::ffi_pin_defer_depth() == 1, "AC4: depth 1");
        CHECK(gh::g_gc_defer_arm_ffi_pin_total.load() == ffi0 + 1, "AC3: first arm +1");
        CHECK(gh::g_gc_defer_any_total.load() == any0 + 1, "AC3: any +1 on empty→nonempty");

        gh::arm_ffi_pin_defer(); // nested → no first-arm bump
        CHECK(gh::ffi_pin_defer_depth() == 2, "AC4: depth 2");
        CHECK(gh::g_gc_defer_arm_ffi_pin_total.load() == ffi0 + 1, "AC3: nested no bump");
        CHECK(gh::g_gc_defer_any_total.load() == any0 + 1, "AC3: any still once");

        gh::release_ffi_pin_defer();
        gh::release_ffi_pin_defer();
        CHECK(gh::ffi_pin_defer_depth() == 0, "AC4: depth 0 after release");
        // Re-arm after clear → another first-arm
        gh::arm_ffi_pin_defer();
        CHECK(gh::g_gc_defer_arm_ffi_pin_total.load() == ffi0 + 2, "AC3: re-arm after clear +1");
        gh::release_ffi_pin_defer();
    }

    // ── AC1 multi-reason sequential ────────────────────────────────
    {
        std::println("\n--- #2428 AC1: each reason first-arm once ---");
        drain_ffi();
        drain_render();
        drain_mutation();
        drain_panic();
        gh::reset_gc_defer_arm_metrics_for_test();

        gh::arm_gc_defer_pending_panic();
        gh::arm_ffi_pin_defer();
        gh::arm_render_pin_defer();
        gh::arm_mutation_hold_defer();

        CHECK(gh::g_gc_defer_arm_panic_total.load() == 1, "AC1: panic first-arm 1");
        CHECK(gh::g_gc_defer_arm_ffi_pin_total.load() == 1, "AC1: ffi first-arm 1");
        CHECK(gh::g_gc_defer_arm_render_pin_total.load() == 1, "AC1: render first-arm 1");
        CHECK(gh::g_gc_defer_arm_mutation_hold_total.load() == 1, "AC1: mutation first-arm 1");
        // any_total: only first bit that made mask non-empty (panic first)
        CHECK(gh::g_gc_defer_any_total.load() == 1, "AC1: any once for empty→nonempty");

        drain_panic();
        drain_ffi();
        drain_render();
        drain_mutation();
    }

    // ── AC2/AC3 concurrent same-reason arms ────────────────────────
    {
        std::println("\n--- #2428 AC2 + #2428 AC3: concurrent arm_ffi_pin_defer ---");
        drain_ffi();
        gh::reset_gc_defer_arm_metrics_for_test();
        const auto base = gh::g_gc_defer_arm_ffi_pin_total.load(std::memory_order_relaxed);

        constexpr int kThreads = 4;
        constexpr int kArmsPerThread = 200;
        std::vector<std::thread> threads;
        std::atomic<std::uint64_t> err{0};
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&]() {
                try {
                    for (int i = 0; i < kArmsPerThread; ++i)
                        gh::arm_ffi_pin_defer();
                } catch (...) {
                    err.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : threads)
            th.join();

        const auto depth = gh::ffi_pin_defer_depth();
        const auto arms = gh::g_gc_defer_arm_ffi_pin_total.load(std::memory_order_relaxed);
        std::println("  depth={} arm_metric={} base={} err={}", depth, arms, base, err.load());
        CHECK(err.load() == 0, "AC2: no exceptions");
        CHECK(depth == static_cast<std::uint32_t>(kThreads * kArmsPerThread),
              "AC4: depth == total concurrent arms");
        // Critical AC3: first-arm metric must be exactly +1 despite 800 concurrent arms.
        CHECK(arms == base + 1, "AC3: first-arm metric +1 under concurrent race");

        // Drain all
        for (std::uint32_t i = 0; i < depth; ++i)
            gh::release_ffi_pin_defer();
        CHECK(gh::ffi_pin_defer_depth() == 0, "AC4: fully drained");
        CHECK(!gh::should_defer_destructive_gc() ||
                  (gh::defer_reasons_snapshot() &
                   static_cast<std::uint32_t>(gh::GcDeferReason::FfiPin)) == 0,
              "AC4: FfiPin bit clear after drain");
    }

    // ── AC2 concurrent mixed reasons ───────────────────────────────
    {
        std::println("\n--- #2428 AC2: concurrent mixed arm reasons ---");
        drain_ffi();
        drain_render();
        drain_mutation();
        drain_panic();
        gh::reset_gc_defer_arm_metrics_for_test();

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> ops{0};
        std::atomic<std::uint64_t> err{0};
        std::vector<std::thread> threads;
        threads.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    gh::arm_ffi_pin_defer();
                    gh::release_ffi_pin_defer();
                    ops.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    err.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        threads.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    gh::arm_render_pin_defer();
                    gh::release_render_pin_defer();
                    ops.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    err.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        threads.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    gh::arm_mutation_hold_defer();
                    gh::release_mutation_hold_defer();
                    ops.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    err.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        threads.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    gh::arm_gc_defer_pending_panic();
                    gh::release_gc_defer_pending_panic();
                    ops.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    err.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        stop.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();

        std::println("  ops={} ffi_arm={} render_arm={} mut_arm={} panic_arm={} err={}", ops.load(),
                     gh::g_gc_defer_arm_ffi_pin_total.load(),
                     gh::g_gc_defer_arm_render_pin_total.load(),
                     gh::g_gc_defer_arm_mutation_hold_total.load(),
                     gh::g_gc_defer_arm_panic_total.load(), err.load());
        CHECK(ops.load() > 0, "AC2: concurrent mixed arms progressed");
        CHECK(err.load() == 0, "AC2: no exceptions under mixed concurrent arm/release");
        // Metrics should be positive and not absurdly inflated vs ops
        // (each cycle can bump at most once when bit was clear).
        CHECK(gh::g_gc_defer_arm_ffi_pin_total.load() <= ops.load(),
              "AC3: ffi first-arm count bounded by ops");
        drain_ffi();
        drain_render();
        drain_mutation();
        drain_panic();
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
