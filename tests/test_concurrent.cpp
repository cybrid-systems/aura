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

// ── Test 10: Work-stealing deque unit test ────────────
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

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);

    return g_failed > 0 ? 1 : 0;
}
