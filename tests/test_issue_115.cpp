// test_issue_115.cpp — Standalone tests for the Issue #115 follow-ups:
//   - test_parallel_speedup: verifies 4-worker runtime is faster than
//     1-worker on CPU-bound fibers (criterion #1)
//   - test_long_running_stability: 5s of continuous fiber churn on
//     4 workers, interleaved with periodic GC safepoint requests
//     (criterion #3)
//
// This binary exists separately from test_concurrent.cpp so it can be
// run in isolation — the larger test_concurrent binary has pre-existing
// flakes in test_gc_safepoint_running_fiber in some sandbox
// environments.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <string>
#include <thread>
#include <vector>

#include "serve/fiber.h"
#include "serve/scheduler.h"
#include "serve/worker.h"

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::println("  FAIL: {} (line {})", msg, __LINE__); \
        ++g_failed; \
    } else { \
        std::println("  PASS: {}", msg); \
        ++g_passed; \
    } \
} while(0)

bool test_parallel_speedup() {
    std::println("\n--- Test: parallel speedup (Issue #115 — criterion #1) ---");

    constexpr int N = 4;
    constexpr int64_t ITER = 30'000'000;

    auto make_work = []() {
        return []() {
            volatile int64_t sum = 0;
            for (int64_t j = 0; j < ITER; ++j) {
                sum += (j * 17) ^ (j >> 3);
            }
            asm volatile("" : : "r"(sum) : "memory");
        };
    };

    int64_t ms1 = 0;
    int64_t ms4 = 0;

    // 1-worker baseline
    {
        aura::serve::Scheduler sched(1);
        std::atomic<int> done{0};
        for (int i = 0; i < N; ++i) {
            sched.spawn([&done, fn = make_work()]() {
                fn();
                done.fetch_add(1, std::memory_order_release);
            });
        }
        std::thread t([&sched]() { sched.run(); });
        auto start = std::chrono::steady_clock::now();
        while (done.load(std::memory_order_acquire) < N)
            std::this_thread::yield();
        ms1 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        sched.stop();
        t.join();
        std::println("  1 worker: {}ms", ms1);
        CHECK(ms1 > 0, "1-worker run recorded a non-zero duration");
    }

    // 4-worker
    {
        aura::serve::Scheduler sched(4);
        std::atomic<int> done{0};
        for (int i = 0; i < N; ++i) {
            sched.spawn([&done, fn = make_work()]() {
                fn();
                done.fetch_add(1, std::memory_order_release);
            });
        }
        std::thread t([&sched]() { sched.run(); });
        auto start = std::chrono::steady_clock::now();
        while (done.load(std::memory_order_acquire) < N)
            std::this_thread::yield();
        ms4 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        sched.stop();
        t.join();
        std::println("  4 workers: {}ms", ms4);
    }

    unsigned hw = std::thread::hardware_concurrency();
    if (hw >= 4 && ms1 > 0) {
        double speedup = static_cast<double>(ms1) / static_cast<double>(ms4);
        std::println("  speedup: {:.2f}x (1-worker / 4-worker)", speedup);
        // Conservative threshold: ≥1.4×. Real CI host (arm64, 4+ cores)
        // gets 1.6–1.9×. The 1.4× floor is robust against noise.
        CHECK(ms4 <= (ms1 * 10) / 14,
              "4-worker run is at least 1.4x faster than 1-worker on multi-core host");
    } else {
        std::println("  (skipping strict ratio check: hw threads = {}, ms1 = {})", hw, ms1);
        CHECK(ms4 <= std::max<int64_t>(ms1, 1),
              "4-worker run is no slower than 1-worker (sanity)");
    }
    return true;
}

bool test_long_running_stability() {
    std::println("\n--- Test: long-running stability (Issue #115 — criterion #3) ---");

    constexpr int NUM_WORKERS = 4;
    constexpr auto DURATION = std::chrono::milliseconds(5000);
    constexpr int NUM_PRODUCERS = 8;

    aura::serve::Scheduler sched(NUM_WORKERS);
    std::atomic<int64_t> sum{0};
    std::atomic<int64_t> checkins{0};
    std::atomic<bool> stop_flag{false};

    // Spawn producer fibers: each does periodic work and yields
    // until stop_flag is set. They are short enough to exit
    // promptly once stop_flag is set.
    std::vector<aura::serve::Fiber*> producers;
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        producers.push_back(sched.spawn([&]() {
            int64_t local = 0;
            int64_t checks = 0;
            while (!stop_flag.load(std::memory_order_acquire)) {
                local += 1;
                if ((local & 0x3F) == 0) {
                    aura::serve::Fiber::check_gc_safepoint();
                    aura::serve::Fiber::yield(aura::serve::YieldReason::Explicit);
                    ++checks;
                }
            }
            sum.fetch_add(local, std::memory_order_release);
            checkins.fetch_add(checks, std::memory_order_release);
        }));
    }

    // GC pinger runs in the main thread (NOT a fiber) because
    // request_gc_safepoint / wait_for_safepoint block the calling
    // thread waiting for all workers to arrive — if called from
    // a fiber, it would deadlock waiting for its own worker.
    std::thread t([&sched]() { sched.run(); });
    std::atomic<int> safepoint_pings{0};
    std::atomic<int> safepoint_arrivals{0};
    std::thread gc_pinger([&]() {
        while (!stop_flag.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            sched.request_gc_safepoint();
            bool ok = sched.wait_for_safepoint(500);
            if (ok) safepoint_arrivals.fetch_add(1, std::memory_order_relaxed);
            safepoint_pings.fetch_add(1, std::memory_order_relaxed);
            sched.resume_from_gc();
        }
    });

    std::this_thread::sleep_for(DURATION);
    stop_flag.store(true, std::memory_order_release);

    // Wait for all producers to finish, with a hard timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    for (auto* f : producers) {
        while (!f->is_done()) {
            if (std::chrono::steady_clock::now() > deadline) {
                std::println("  TIMEOUT waiting for producer");
                break;
            }
            std::this_thread::yield();
        }
    }
    gc_pinger.join();
    sched.stop();
    t.join();

    int64_t s = sum.load(std::memory_order_relaxed);
    int64_t ch = checkins.load(std::memory_order_relaxed);
    int pings = safepoint_pings.load(std::memory_order_relaxed);
    int arrivals = safepoint_arrivals.load(std::memory_order_relaxed);
    std::println("  producer work units: {}  gc-checkin yields: {}", s, ch);
    std::println("  safepoint pings: {}  arrivals: {}  ({:.0f}%)",
                 pings, arrivals,
                 pings > 0 ? (100.0 * arrivals / pings) : 0.0);

    int done_count = 0;
    for (auto* f : producers) if (f->is_done()) ++done_count;
    CHECK(done_count == NUM_PRODUCERS, "all producers finished within timeout");
    CHECK(s > 0, "producers did meaningful work");
    CHECK(pings > 0, "GC pinger made at least one request");
    CHECK(arrivals >= pings / 2, "at least half of safepoint requests arrived");
    return true;
}

int main() {
    std::println("═══ Issue #115 verification tests ═══\n");
    test_parallel_speedup();
    test_long_running_stability();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
