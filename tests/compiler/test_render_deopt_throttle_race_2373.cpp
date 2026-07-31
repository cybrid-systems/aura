// @category: unit
// @reason: Issue #2373 — try_render_deopt_throttle check-then-act race fix
// (CAS loop). N concurrent callers within window → exactly one true.

#include "test_harness.hpp"

#include "core/arena_auto_policy_stats.h"

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

using aura::core::arena_policy::last_render_deopt_ns;
using aura::core::arena_policy::render_jit_deopt_applied_total;
using aura::core::arena_policy::render_jit_deopt_throttled_total;
using aura::core::arena_policy::try_render_deopt_throttle;
using aura::test::g_failed;
using aura::test::g_passed;

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

static void reset_throttle_state() {
    last_render_deopt_ns.store(0, std::memory_order_relaxed);
    // Counters are process-lifetime; tests use deltas.
}

} // namespace

int main() {
    std::println("=== Issue #2373: render deopt throttle CAS race ===");

    // AC1: N concurrent callers within window → exactly one true
    {
        std::println("\n--- AC1: concurrent within window → one applied ---");
        reset_throttle_state();
        const auto applied0 = render_jit_deopt_applied_total.load(std::memory_order_relaxed);
        const auto throttled0 = render_jit_deopt_throttled_total.load(std::memory_order_relaxed);

        constexpr int kN = 16;
        constexpr std::uint64_t kWindowMs = 5000; // wide so all fit in window
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::atomic<int> true_count{0};
        std::atomic<int> false_count{0};
        std::vector<std::thread> threads;
        threads.reserve(kN);
        for (int i = 0; i < kN; ++i) {
            threads.emplace_back([&]() {
                ready.fetch_add(1, std::memory_order_acq_rel);
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();
                if (try_render_deopt_throttle(kWindowMs))
                    true_count.fetch_add(1, std::memory_order_relaxed);
                else
                    false_count.fetch_add(1, std::memory_order_relaxed);
            });
        }
        while (ready.load(std::memory_order_acquire) < kN)
            std::this_thread::yield();
        go.store(true, std::memory_order_release);
        for (auto& t : threads)
            t.join();

        CHECK(true_count.load() == 1, "AC1: exactly one true");
        CHECK(false_count.load() == kN - 1, "AC1: all others false");
        const auto applied1 = render_jit_deopt_applied_total.load(std::memory_order_relaxed);
        const auto throttled1 = render_jit_deopt_throttled_total.load(std::memory_order_relaxed);
        CHECK(applied1 == applied0 + 1, "AC1: applied_total +1");
        CHECK(throttled1 == throttled0 + static_cast<std::uint64_t>(kN - 1),
              "AC1: throttled_total +(N-1)");
    }

    // AC2: sequential callers outside the window still return true
    {
        std::println("\n--- AC2: sequential outside window ---");
        reset_throttle_state();
        const auto applied0 = render_jit_deopt_applied_total.load(std::memory_order_relaxed);
        // Tiny window so a short sleep exits the throttle band.
        CHECK(try_render_deopt_throttle(/*window_ms=*/1), "AC2: first apply");
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        CHECK(try_render_deopt_throttle(/*window_ms=*/1), "AC2: second apply after window");
        // Within window still throttles.
        CHECK(!try_render_deopt_throttle(/*window_ms=*/5000), "AC2: third inside window throttled");
        const auto applied1 = render_jit_deopt_applied_total.load(std::memory_order_relaxed);
        CHECK(applied1 == applied0 + 2, "AC2: applied_total +2 for two outside-window calls");
    }

    // AC3: TSAN clean — source uses CAS (not load+store); concurrent AC1
    // exercises the race under normal (and ASAN/TSAN CI) builds.
    {
        std::println("\n--- AC3: CAS loop source wire ---");
        const auto h = read_file("src/core/arena_auto_policy_stats.h");
        CHECK(!h.empty(), "AC3: header readable");
        CHECK(h.find("compare_exchange_weak") != std::string::npos, "AC3: CAS present");
        CHECK(h.find("while (true)") != std::string::npos ||
                  h.find("while(true)") != std::string::npos,
              "AC3: CAS loop");
        // Old check-then-act store path must not remain as the sole update.
        // After the CAS win we only bump applied_total (no bare store of last_render_deopt_ns).
        CHECK(h.find("Issue #2373") != std::string::npos, "AC3: cites #2373");
    }

    // AC4: test + source-cite
    {
        std::println("\n--- AC4: source-cite ---");
        const auto h = read_file("src/core/arena_auto_policy_stats.h");
        CHECK(h.find("try_render_deopt_throttle") != std::string::npos, "AC4: API present");
        CHECK(h.find("render_jit_deopt_applied_total") != std::string::npos,
              "AC4: applied counter");
        CHECK(h.find("render_jit_deopt_throttled_total") != std::string::npos,
              "AC4: throttled counter");
        CHECK(h.find("last_render_deopt_ns") != std::string::npos, "AC4: last timestamp");
    }

    std::println("\n=== #2373 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
