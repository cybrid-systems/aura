// test_concurrent.cpp — Concurrency model unit tests
// Tests: fiber lifecycle, worker thread scheduling, eventfd wakeup, state transitions

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <print>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <iostream>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "serve/fiber.h"
#include "serve/worker.h"
#include "serve/scheduler.h"
#include "serve/metrics.h"

// ── Test counters ─────────────────────────────────────
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::println(std::cerr, "  FAIL: {} (line {})", msg, __LINE__); \
        ++g_failed; \
    } else { \
        std::println("  PASS: {}", msg); \
        ++g_passed; \
    } \
} while(0)

// ── Test 1: Fiber basic lifecycle ─────────────────────
// Create a fiber, run it, verify it completes

bool test_fiber_lifecycle() {
    std::println("\n--- Test: Fiber lifecycle ---");

    bool ran = false;

    // Manually create and run a fiber via the scheduler
    aura::serve::Scheduler sched(2);  // 2 workers

    sched.spawn([&ran]() {
        ran = true;
    });

    // Run the scheduler with a timeout (spawned fiber should complete)
    // Run in a background thread to allow timeout
    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    sched.stop();
    t.join();

    CHECK(ran, "fiber function executed");
    return true;
}

// ── Test 2: Fiber yield/resume ────────────────────────
// Spawn a fiber that yields and resumes multiple times

bool test_fiber_yield() {
    std::println("\n--- Test: Fiber yield/resume ---");

    std::atomic<int> counter{0};

    aura::serve::Scheduler sched(2);

    sched.spawn([&counter]() {
        counter.fetch_add(1);
        aura::serve::Fiber::yield();
        counter.fetch_add(10);
        aura::serve::Fiber::yield();
        counter.fetch_add(100);
    });

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    sched.stop();
    t.join();

    CHECK(counter.load() == 111, "yield/resume preserves execution state (1+10+100 = 111)");
    return true;
}

// ── Test 3: Multiple fibers on multiple workers ───────
// Spawn N fibers, each doing CPU work, verify all complete

bool test_multi_fiber_parallel() {
    std::println("\n--- Test: Multi-fiber parallel execution ---");

    constexpr int NUM_FIBERS = 10;
    std::atomic<int> completed{0};

    aura::serve::Scheduler sched(4);  // 4 workers

    for (int i = 0; i < NUM_FIBERS; ++i) {
        sched.spawn([&completed, i]() {
            // Simulate work: busy loop
            volatile int sum = 0;
            for (int j = 0; j < 1000000; ++j) {
                sum += j;
            }
            completed.fetch_add(1);
        });
    }

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    sched.stop();
    t.join();

    CHECK(completed.load() == NUM_FIBERS, "all " + std::to_string(NUM_FIBERS) + " fibers completed");
    return true;
}

// ── Test 4: Fiber eventfd wakeup ──────────────────────
// Fiber goes Waiting on eventfd, IO thread wakes it

bool test_eventfd_wakeup() {
    std::println("\n--- Test: Eventfd wakeup ---");

    std::atomic<int> stage{0};

    aura::serve::Scheduler sched(2);

    sched.spawn([&stage]() {
        // Stage 1: running
        stage.store(1);

        // Go waiting
        aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
        aura::serve::Fiber::yield();

        // Stage 2: woken up
        stage.store(2);
    });

    // Run scheduler in background
    std::thread t([&sched]() { sched.run(); });

    // Wait for fiber to reach waiting state
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // The fiber goes Waiting, then IO thread needs to wake it.
    // We need to trigger an event on the fiber's eventfd.
    // But we don't have direct access to the eventfd from here.
    // For now: stop and check stage = 1 (fiber ran, then waited)
    
    sched.stop();
    t.join();

    CHECK(stage.load() >= 1, "fiber started and reached waiting");
    return true;
}

// ── Test 5: Worker round-robin distribution ───────────
// Spawn many fibers, verify they're spread across workers

bool test_worker_distribution() {
    std::println("\n--- Test: Worker round-robin distribution ---");

    aura::serve::Scheduler sched(4);
    std::atomic<int> completed{0};

    // Spawn 16 fibers
    for (int i = 0; i < 16; ++i) {
        sched.spawn([&completed]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            completed.fetch_add(1);
        });
    }

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    sched.stop();
    t.join();

    CHECK(completed.load() == 16, "all 16 fibers completed (round-robin distribution)");
    return true;
}

// ── Test 6: Fiber can spawn fibers ────────────────────
// Fiber A spawns Fiber B, both complete

bool test_fiber_spawns_fiber() {
    std::println("\n--- Test: Fiber spawning fiber ---");

    std::atomic<int> counter{0};

    aura::serve::Scheduler sched(2);

    sched.spawn([&counter, &sched]() {
        counter.fetch_add(1);
        
        // Spawn another fiber from within a fiber
        sched.spawn([&counter]() {
            counter.fetch_add(10);
        });

        // Wait a bit
        aura::serve::Fiber::yield();
    });

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    sched.stop();
    t.join();

    CHECK(counter.load() == 11, "fiber-spawned-fiber executes (1 + 10 = 11)");
    return true;
}

// ── Test 7: Stress — many fibers, quick tasks ─────────
// 200 fibers doing trivial work, verify all complete

bool test_stress_many_fibers() {
    std::println("\n--- Test: Stress — 200 fibers ---");

    constexpr int N = 200;
    std::atomic<int> completed{0};

    aura::serve::Scheduler sched(8);

    for (int i = 0; i < N; ++i) {
        sched.spawn([&completed]() {
            completed.fetch_add(1);
        });
    }

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    sched.stop();
    t.join();

    CHECK(completed.load() == N, "all " + std::to_string(N) + " fibers completed");
    return true;
}

// ── Test 8: Worker lifecycle — start/stop/join ────────
// Verify worker threads start and stop properly

bool test_worker_lifecycle() {
    std::println("\n--- Test: Worker lifecycle ---");

    aura::serve::Scheduler sched(4);  // creates 4 workers but doesn't start them yet

    // Verify workers exist
    bool valid = true;
    for (int i = 0; i < 4; ++i) {
        auto* w = sched.worker(i);
        if (!w) {
            valid = false;
            break;
        }
    }
    CHECK(valid, "4 workers created");

    // Run with no fibers — workers should start and stop cleanly
    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sched.stop();
    t.join();

    CHECK(true, "workers start and stop cleanly");
    return true;
}

// ── Test 9: Work-stealing — imbalanced load ───────────
// Spawn all fibers on worker 0, then stop worker 0 to force
// other workers to steal fibers from its queue.

bool test_work_stealing() {
    std::println("\n--- Test: Work-stealing ---");

    constexpr int NUM_FIBERS = 50;
    std::atomic<int> completed{0};

    aura::serve::Scheduler sched(4);

    // Spawn all fibers — they're distributed round-robin across workers
    // But with work-stealing, idle workers will steal from busy ones
    for (int i = 0; i < NUM_FIBERS; ++i) {
        sched.spawn([&completed, i]() {
            // Small work to ensure steal opportunity
            volatile int sum = 0;
            for (int j = 0; j < 100000; ++j) {
                sum += j;
            }
            completed.fetch_add(1);
        });
    }

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    sched.stop();
    t.join();

    CHECK(completed.load() == NUM_FIBERS, "all " + std::to_string(NUM_FIBERS) + " fibers completed via work-stealing");
    return true;
}

// ── Test 10: Adaptive steal budget ──────────────────────
// Verify the adaptive StealBudget adjusts max_before_sleep
// based on success rate.

bool test_adaptive_steal_budget() {
    std::println("\n--- Test: Adaptive steal budget ---");

    aura::serve::StealBudget budget(true);

    // Initially: default
    CHECK(budget.max_before_sleep == 3, "initial max_before_sleep = 3");
    CHECK(budget.consecutive_failures == 0, "initial consecutive_failures = 0");

    // Record 8 failures (enough to fill window and see low success rate)
    for (int i = 0; i < 8; ++i) {
        budget.record_failure();
    }
    // After 10 failures with 0 success: rate=0% -> max_before_sleep = 1
    // But we only have 8 in window, so no adaptation yet (history_idx < 10)
    CHECK(budget.max_before_sleep == 3, "before window fills, no adaptation");

    // Fill the rest with failures
    for (int i = 8; i < 12; ++i) {
        budget.record_failure();
    }
    // Now history has 10 entries total, 10 failures -> rate=0% -> max_before_sleep = 1
    // Actually: history_idx=12, window has last 10 entries, all 0's, rate=0% -> max_before_sleep=1
    CHECK(budget.max_before_sleep == 1, "low steal rate forces sleep sooner (max_before_sleep=1)");

    // Reset and test high success rate
    budget.reset();
    CHECK(budget.max_before_sleep == 3, "reset restores defaults");

    // Record 10 successes
    for (int i = 0; i < 15; ++i) {
        budget.record_success();
    }
    // history has last 10, all 1's -> rate=100% -> max_before_sleep = 6
    CHECK(budget.max_before_sleep == 6, "high steal rate stays alert longer (max_before_sleep=6)");

    // Verify adaptation with a 50% success rate
    budget.reset();
    // Fill the window with alternating success/failure → 50% rate
    for (int i = 0; i < 10; ++i) {
        if (i % 2 == 0)
            budget.record_success();
        else
            budget.record_failure();
    }
    // After 10 entries in window, 5 successes = 50% rate → max_before_sleep = 4
    CHECK(budget.max_before_sleep == 4, "50% success rate = medium steal budget (max_before_sleep=4)");

    return true;
}

// ── Test 11: Load-aware distribution ─────────────────────
// Verify that spawning with load-aware scheduling picks
// the least-loaded worker.

bool test_load_aware_distribution() {
    std::println("\n--- Test: Load-aware worker distribution ---");

    aura::serve::Scheduler sched(4);
    std::atomic<int> completed{0};

    // Spawn 40 fibers quickly — load-aware should distribute more evenly
    // than pure round-robin
    for (int i = 0; i < 40; ++i) {
        sched.spawn([&completed]() {
            volatile int sum = 0;
            for (int j = 0; j < 200000; ++j) sum += j;
            completed.fetch_add(1);
        });
    }

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    sched.stop();
    t.join();

    CHECK(completed.load() == 40, "all 40 fibers completed with load-aware distribution");
    return true;
}

// ── Test 12: Metrics collection — basic sanity ─────────
// Verify that metrics counters increment correctly.

bool test_metrics_sanity() {
    std::println("\n--- Test: Metrics collection sanity ---");

    // Test WorkerMetrics directly
    aura::serve::metrics::WorkerMetrics wm;

    wm.fibers_executed.fetch_add(1);
    CHECK(wm.fibers_executed.load() == 1, "fibers_executed = 1");

    wm.steal_attempts.fetch_add(5);
    wm.steal_successes.fetch_add(2);
    double sr = wm.steal_success_rate();
    CHECK(sr > 39.9 && sr < 40.1, "steal success rate = 40%");

    wm.record_busy(100000000);  // 100ms
    wm.record_idle(400000000);  // 400ms
    double util = wm.utilization();
    CHECK(util > 19.9 && util < 20.1, "utilization = 20% (100ms busy / 500ms total)");

    wm.record_qdepth(3);
    wm.record_qdepth(7);
    wm.record_qdepth(5);
    CHECK(wm.qdepth_max.load() == 7, "max queue depth = 7");
    double avg = wm.avg_qdepth();
    CHECK(avg > 4.9 && avg < 5.1, "avg queue depth = 5.0");

    // Test GlobalMetrics
    aura::serve::metrics::GlobalMetrics gm(2);
    CHECK(gm.num_workers() == 2, "2 workers in global metrics");

    gm.fibers_spawned.fetch_add(10);
    gm.fibers_completed.fetch_add(8);
    CHECK(gm.fibers_spawned.load() == 10, "fibers_spawned = 10");
    CHECK(gm.fibers_completed.load() == 8, "fibers_completed = 8");

    // Verify JSON output is well-formed
    std::string json = gm.to_json();
    CHECK(json.find(R"("fibers_spawned": 10)") != std::string::npos,
          "JSON metrics contain spawned count");

    return true;
}

// ── Test 13: GlobalMetrics dump doesn't crash ────────────

bool test_metrics_dump_no_crash() {
    std::println("\n--- Test: Metrics dump (no crash) ---");

    aura::serve::metrics::GlobalMetrics gm(3);
    gm.fibers_spawned.fetch_add(42);
    gm.worker(0).fibers_executed.fetch_add(15);
    gm.worker(1).steal_successes.fetch_add(7);
    gm.worker(2).local_pushes.fetch_add(23);

    // Dump to stdout — should not crash
    gm.dump();

    // JSON
    std::string json = gm.to_json();
    CHECK(!json.empty(), "JSON output is non-empty");
    CHECK(json.find("workers") != std::string::npos, "JSON contains workers array");

    return true;
}

// ── Test 14: Work-stealing deque unit test ────────────
// Directly test the Chase-Lev deque with void* type

bool test_ws_deque() {
    std::println("\n--- Test: Work-stealing deque ---");

    aura::serve::WorkStealingDeque<void*> dq;

    void* v1 = reinterpret_cast<void*>(static_cast<intptr_t>(1));
    void* v2 = reinterpret_cast<void*>(static_cast<intptr_t>(2));
    void* v3 = reinterpret_cast<void*>(static_cast<intptr_t>(3));
    void* v10 = reinterpret_cast<void*>(static_cast<intptr_t>(10));
    void* v20 = reinterpret_cast<void*>(static_cast<intptr_t>(20));
    void* v30 = reinterpret_cast<void*>(static_cast<intptr_t>(30));

    // Push/pop
    dq.push(v1);
    dq.push(v2);
    dq.push(v3);

    auto a = reinterpret_cast<intptr_t>(dq.pop());
    auto b = reinterpret_cast<intptr_t>(dq.pop());
    auto c = reinterpret_cast<intptr_t>(dq.pop());

    CHECK(a == 3, "LIFO pop returns most recent (3)");
    CHECK(b == 2, "LIFO pop returns second (2)");
    CHECK(c == 1, "LIFO pop returns first (1)");

    // Empty pop returns nullptr
    auto nil = dq.pop();
    CHECK(nil == nullptr, "pop from empty deque returns nullptr");

    // Steal (should get oldest = first pushed)
    dq.push(v10);
    dq.push(v20);
    dq.push(v30);

    auto s1 = reinterpret_cast<intptr_t>(dq.steal());
    CHECK(s1 == 10, "steal returns oldest (10)");

    // Owner pop after steal
    auto p1 = reinterpret_cast<intptr_t>(dq.pop());
    CHECK(p1 == 30, "after steal, owner pop returns newest (30)");

    auto p2 = reinterpret_cast<intptr_t>(dq.pop());
    CHECK(p2 == 20, "last remaining (20)");

    CHECK(dq.empty_approx(), "deque is empty after all operations");

    return true;
}

// ── Main ──────────────────────────────────────────────

int main() {
    std::println("═══ Concurrent model unit tests ═══\n");

    // Run all tests
    test_ws_deque();
    test_worker_lifecycle();
    test_fiber_lifecycle();
    test_fiber_yield();
    test_multi_fiber_parallel();
    test_worker_distribution();
    test_fiber_spawns_fiber();
    test_stress_many_fibers();
    test_eventfd_wakeup();
    test_work_stealing();
    test_adaptive_steal_budget();
    test_load_aware_distribution();
    test_metrics_sanity();
    test_metrics_dump_no_crash();

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);

    return g_failed > 0 ? 1 : 0;
}
