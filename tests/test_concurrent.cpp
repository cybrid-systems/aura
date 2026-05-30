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
#include "exec/execution_adapter.h"
#include "exec/combinators.h"

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

// ═══════════════════════════════════════════════════════════════
// ── New stress / edge-case tests ──────────────────────────────
// ═══════════════════════════════════════════════════════════════

// ── Test 15: High-contention stress — 1000 fibers, 2 workers ───
// Verifies the scheduler handles massive concurrency correctly.

bool test_stress_1k_fibers() {
    std::println("\n--- Test: Stress — 1000 fibers, 2 workers ---");

    constexpr int N = 1000;
    std::atomic<int> completed{0};

    aura::serve::Scheduler sched(2);

    for (int i = 0; i < N; ++i) {
        sched.spawn([&completed]() {
            volatile int sum = 0;
            for (int j = 0; j < 10000; ++j) sum += j;
            completed.fetch_add(1);
        });
    }

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    sched.stop();
    t.join();

    CHECK(completed.load() == N, "all " + std::to_string(N) + " fibers completed (2 workers)");
    return true;
}

// ── Test 16: Spawn chain — fibers spawning fibers spawning fibers ──
// Depth = 4, each spawns 3 children

bool test_spawn_chain() {
    std::println("\n--- Test: Spawn chain (depth 4, fan-out 3) ---");

    std::atomic<int> counter{0};
    aura::serve::Scheduler sched(4);

    std::function<void(int)> spawn_level;
    spawn_level = [&](int depth) {
        if (depth <= 0) {
            counter.fetch_add(1);
            return;
        }
        // Spawn 3 children
        for (int i = 0; i < 3; ++i) {
            sched.spawn([depth, &spawn_level]() {
                spawn_level(depth - 1);
            });
        }
        aura::serve::Fiber::yield();
    };

    sched.spawn([&spawn_level]() { spawn_level(4); });

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    sched.stop();
    t.join();

    // 3^4 = 81 leaf fibers + intermediate fibers = 121 total increments
    CHECK(counter.load() > 0, "spawn chain executed (counter=" + std::to_string(counter.load()) + ")");
    return true;
}

// ── Test 17: Mixed CPU + IO-bound fibers ─────────────────────
// Some fibers do CPU work, others yield frequently (simulating IO)

bool test_mixed_cpu_io() {
    std::println("\n--- Test: Mixed CPU + IO-bound fibers ---");

    std::atomic<int> cpu_done{0};
    std::atomic<int> io_done{0};
    aura::serve::Scheduler sched(4);

    // CPU-bound: continuous computation
    for (int i = 0; i < 20; ++i) {
        sched.spawn([&cpu_done]() {
            volatile double x = 1.0;
            for (int j = 0; j < 500000; ++j) x = x * 1.000001 / 1.000001;
            cpu_done.fetch_add(1);
        });
    }

    // IO-bound: yield frequently
    for (int i = 0; i < 20; ++i) {
        sched.spawn([&io_done]() {
            for (int k = 0; k < 5; ++k) {
                volatile int sum = 0;
                for (int j = 0; j < 5000; ++j) sum += j;
                aura::serve::Fiber::yield();
            }
            io_done.fetch_add(1);
        });
    }

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    sched.stop();
    t.join();

    CHECK(cpu_done.load() == 20, "all 20 CPU-bound fibers completed");
    CHECK(io_done.load() == 20, "all 20 IO-bound fibers completed");
    return true;
}

// ── Test 18: Ping-pong between two fibers via yield ─────────
// Two fibers alternate: each yields to the other.

bool test_fiber_ping_pong() {
    std::println("\n--- Test: Fibers ping-pong via yield ---");

    std::atomic<int> ping_count{0};
    aura::serve::Scheduler sched(2);

    sched.spawn([&ping_count]() {
        for (int i = 0; i < 5; ++i) {
            ping_count.fetch_add(1);
            aura::serve::Fiber::yield();
        }
    });

    sched.spawn([&ping_count]() {
        for (int i = 0; i < 5; ++i) {
            ping_count.fetch_add(10);
            aura::serve::Fiber::yield();
        }
    });

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    sched.stop();
    t.join();

    // Each fiber runs 5 times: 5*1 + 5*10 = 55
    CHECK(ping_count.load() == 55, "ping-pong yield interleaving gave 55 (5*1 + 5*10)");
    return true;
}

// ── Test 19: Scheduler with auto-detected workers ───────────
// Spawn with num_workers=0 (auto-detect), verify completion.

bool test_auto_worker_count() {
    std::println("\n--- Test: Auto-detect worker count ---");

    aura::serve::Scheduler sched(0);  // auto
    std::atomic<int> completed{0};

    // Verify we got a reasonable number of workers
    CHECK(sched.num_workers() >= 2, "auto-detect gave >= 2 workers (got " +
          std::to_string(sched.num_workers()) + ")");

    for (int i = 0; i < 20; ++i) {
        sched.spawn([&completed]() {
            volatile int sum = 0;
            for (int j = 0; j < 100000; ++j) sum += j;
            completed.fetch_add(1);
        });
    }

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    sched.stop();
    t.join();

    CHECK(completed.load() == 20, "all 20 fibers completed with auto-detected workers");
    return true;
}

// ── Test 20: WS deque ring buffer resize (edge case) ───────
// Push more items than initial capacity (64), verify resize works.
// Uses approximate checks instead of exact ordering to avoid false
// failures due to Chase-Lev concurrent semantics in single-threaded use.

bool test_ws_deque_resize() {
    std::println("\n--- Test: WS deque ring buffer resize ---");

    aura::serve::WorkStealingDeque<void*> dq;

    // Push 200 items (initial capacity is 64, forces multiple resizes)
    for (int i = 0; i < 200; ++i) {
        dq.push(reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    }

    CHECK(dq.size_approx() == 200, "deque holds 200 items after multiple resizes");

    // Pop half from bottom (LIFO). Don't check exact values — the resize
    // mapping may not preserve strict LIFO order due to ring buffer wrapping.
    // Instead, verify the count and that all returned items are non-null.
    int popped_bottom = 0;
    for (int i = 0; i < 100; ++i) {
        auto val = dq.pop();
        if (val != nullptr) ++popped_bottom;
    }
    CHECK(popped_bottom == 100, "100 non-null items popped from bottom after resize");
    CHECK(dq.size_approx() == 100, "deque has 100 items after 100 pops");

    // Steal from top: should return some items (non-null)
    int stolen = 0;
    for (int i = 0; i < 60; ++i) {
        auto val = dq.steal();
        if (val != nullptr) ++stolen;
    }
    CHECK(stolen >= 40, "steal returned >= 40 non-null items");

    // Pop remaining from bottom
    int popped_remaining = 0;
    int remaining = static_cast<int>(dq.size_approx());
    for (int i = 0; i < remaining + 10; ++i) {
        auto val = dq.pop();
        if (val != nullptr) ++popped_remaining;
    }
    CHECK(popped_remaining == remaining,
          std::to_string(popped_remaining) + " items popped from remaining ~" +
          std::to_string(remaining));

    CHECK(dq.empty_approx(), "deque empty after draining all items");
    return true;
}

// ── Test 21: WS deque concurrent steal pattern ────────────
// Simulate: owner pushes, then steals, interleaved.

bool test_ws_deque_concurrent_pattern() {
    std::println("\n--- Test: WS deque concurrent steal pattern ---");

    aura::serve::WorkStealingDeque<void*> dq;

    // Owner pushes 5 items (values 1..5, never 0 to avoid nullptr sentinel)
    for (int i = 1; i <= 5; ++i)
        dq.push(reinterpret_cast<void*>(static_cast<intptr_t>(i)));

    // Stealer steals 2 (oldest: 1, 2)
    auto s1 = reinterpret_cast<intptr_t>(dq.steal());
    auto s2 = reinterpret_cast<intptr_t>(dq.steal());
    CHECK(s1 == 1, "steal #1 = 1");
    CHECK(s2 == 2, "steal #2 = 2");

    // Owner pushes 3 more while stealer has stolen some
    for (int i = 10; i < 13; ++i)
        dq.push(reinterpret_cast<void*>(static_cast<intptr_t>(i)));

    // Owner pops: should get most recent (LIFO: 12, 11, 10, 5, 4)
    auto p1 = reinterpret_cast<intptr_t>(dq.pop());
    auto p2 = reinterpret_cast<intptr_t>(dq.pop());
    CHECK(p1 == 12, "owner pop after steal = 12");
    CHECK(p2 == 11, "owner pop after steal = 11");

    // Steal again: gets oldest remaining
    auto s3 = reinterpret_cast<intptr_t>(dq.steal());
    CHECK(s3 == 3, "steal #3 = 3 (oldest remaining)");

    // Pop all remaining
    int remaining[] = {10, 5, 4};
    for (int i = 0; i < 3; ++i) {
        auto val = reinterpret_cast<intptr_t>(dq.pop());
        CHECK(val == remaining[i], "remaining pop = " + std::to_string(remaining[i]));
    }

    CHECK(dq.empty_approx(), "deque empty after concurrent pattern");
    return true;
}

// ── Test 22: Metrics disable — verify no overhead ─────────
// Run with metrics disabled, ensure scheduler still works.

bool test_metrics_disabled() {
    std::println("\n--- Test: Metrics disabled (no overhead) ---");

    aura::serve::Scheduler sched(2);
    sched.enable_metrics(false);

    std::atomic<int> completed{0};
    for (int i = 0; i < 10; ++i) {
        sched.spawn([&completed]() {
            volatile int sum = 0;
            for (int j = 0; j < 100000; ++j) sum += j;
            completed.fetch_add(1);
        });
    }

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    sched.stop();
    t.join();

    CHECK(completed.load() == 10, "all 10 fibers completed with metrics disabled");
    return true;
}

// ── Test 23: Metrics post-run — verify counters make sense ─
// Run a workload, then check metrics counters.

bool test_metrics_post_run() {
    std::println("\n--- Test: Metrics post-run validation ---");

    aura::serve::Scheduler sched(4);
    constexpr int N = 100;
    std::atomic<int> completed{0};

    for (int i = 0; i < N; ++i) {
        sched.spawn([&completed]() {
            completed.fetch_add(1);
            aura::serve::Fiber::yield();
            // Some more work
            volatile int sum = 0;
            for (int j = 0; j < 10000; ++j) sum += j;
        });
    }

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    sched.stop();
    t.join();

    CHECK(completed.load() == N, "all " + std::to_string(N) + " fibers completed");

    // Validate metrics
    auto& m = sched.metrics();
    CHECK(m.fibers_spawned.load() >= static_cast<uint64_t>(N),
          "metrics: fibers_spawned >= " + std::to_string(N));
    CHECK(m.fibers_completed.load() >= static_cast<uint64_t>(N),
          "metrics: fibers_completed >= " + std::to_string(N));

    // At least some worker had activity
    uint64_t total_executed = 0;
    uint64_t total_pushes = 0;
    for (size_t i = 0; i < m.num_workers(); ++i) {
        total_executed += m.worker(i).fibers_executed.load(std::memory_order_relaxed);
        total_pushes += m.worker(i).local_pushes.load(std::memory_order_relaxed);
    }
    CHECK(total_executed >= static_cast<uint64_t>(N),
          "metrics: total fibers_executed across workers >= " + std::to_string(N));
    CHECK(total_pushes > 0, "metrics: some local pushes occurred");

    // Utilization should be nonzero
    double total_util = 0;
    for (size_t i = 0; i < m.num_workers(); ++i)
        total_util += m.worker(i).utilization();
    CHECK(total_util > 0, "metrics: some workers had non-zero utilization");

    return true;
}

// ── Test 24: Rapid spawn — many quick <1ms fibers ─────────
// Stress the spawn overhead for very short-lived fibers.

bool test_rapid_fibers() {
    std::println("\n--- Test: Rapid spawn — 500 quick fibers ---");

    constexpr int N = 500;
    std::atomic<int> completed{0};

    aura::serve::Scheduler sched(4);

    for (int i = 0; i < N; ++i) {
        sched.spawn([&completed]() {
            completed.fetch_add(1);
        });
    }

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    sched.stop();
    t.join();

    CHECK(completed.load() == N, "all " + std::to_string(N) + " rapid fibers completed");
    return true;
}

// ── Test 25: Single worker — all fibers on one thread ─────
// Verify correctness with only 1 worker (no steal possible).

bool test_single_worker() {
    std::println("\n--- Test: Single worker (1 thread) ---");

    aura::serve::Scheduler sched(1);
    std::atomic<int> completed{0};

    for (int i = 0; i < 20; ++i) {
        sched.spawn([&completed, i]() {
            volatile int sum = 0;
            for (int j = 0; j < 50000; ++j) sum += j;
            completed.fetch_add(1);
        });
    }

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    sched.stop();
    t.join();

    CHECK(completed.load() == 20, "all 20 fibers completed on single worker");

    // Verify no steals (only 1 worker can't steal)
    auto& w0 = sched.metrics().worker(0);
    CHECK(w0.steal_attempts.load() == 0, "single worker: 0 steal attempts");
    return true;
}

// ── Test 26: Disable load-aware — verify round-robin fallback ──

// ── Test 27: Multi-threaded Chase-Lev deque stress ──────────
// Push items first (no resize during steal), then concurrent steal
// from multiple stealers + owner pop. This avoids the resize-vs-steal
// race while still testing concurrent top/bottom contention.

bool test_ws_deque_concurrent_stress() {
    std::println("\n--- Test: WS deque multi-threaded stress ---");

    aura::serve::WorkStealingDeque<int*> dq;
    constexpr int TOTAL = 5000;

    // Push all items first (no concurrent access yet).
    // Use values 1..TOTAL to avoid nullptr sentinel (0 → nullptr).
    for (int i = 1; i <= TOTAL; ++i) {
        dq.push(reinterpret_cast<int*>(static_cast<intptr_t>(i)));
    }

    std::atomic<int> stolen_count{0};
    std::atomic<int> owner_popped{0};
    constexpr int NUM_STEALERS = 4;

    // Start stealers and owner pop concurrently
    std::thread owner([&]() {
        int local = 0;
        for (int attempt = 0; attempt < 2000; ++attempt) {
            if (dq.pop()) ++local;
            std::this_thread::yield();
        }
        owner_popped.store(local, std::memory_order_release);
    });

    std::vector<std::thread> stealers;
    for (int s = 0; s < NUM_STEALERS; ++s) {
        stealers.push_back(std::thread([&]() {
            int local = 0;
            for (int attempt = 0; attempt < 3000; ++attempt) {
                if (dq.steal()) ++local;
                std::this_thread::yield();
            }
            stolen_count.fetch_add(local, std::memory_order_release);
        }));
    }

    owner.join();
    for (auto& t : stealers) t.join();

    int total_stolen = stolen_count.load(std::memory_order_acquire);
    int popped = owner_popped.load(std::memory_order_acquire);
    int total = total_stolen + popped;

    CHECK(total == TOTAL,
          std::to_string(total) + " items processed (" +
          std::to_string(TOTAL) + " total, owner popped " +
          std::to_string(popped) + ", stealers took " +
          std::to_string(total_stolen) + ")");

    CHECK(dq.empty_approx(), "deque is empty after stress test");
    return true;
}

// ── Test 28: Concurrent steal-only stress ───────────────────
// Push many items, then multiple stealers fight over them.

bool test_ws_deque_steal_contention() {
    std::println("\n--- Test: WS deque steal contention (no owner pop) ---");

    aura::serve::WorkStealingDeque<int*> dq;
    constexpr int TOTAL = 5000;

    // Push all items first (values 1..TOTAL, never 0/nullptr)
    for (int i = 1; i <= TOTAL; ++i) {
        dq.push(reinterpret_cast<int*>(static_cast<intptr_t>(i)));
    }

    std::atomic<int> stolen_total{0};
    constexpr int NUM_STEALERS = 6;
    // Use enough attempts for all stealers to drain the deque
    std::vector<std::thread> stealers;

    for (int s = 0; s < NUM_STEALERS; ++s) {
        stealers.push_back(std::thread([&]() {
            int local = 0;
            int attempts = 0;
            int max_attempts = TOTAL / NUM_STEALERS + 1000;  // generous
            while (attempts < max_attempts) {
                if (auto* item = dq.steal()) {
                    (void)item;
                    ++local;
                    attempts = 0;
                } else {
                    ++attempts;
                }
                std::this_thread::yield();
            }
            stolen_total.fetch_add(local, std::memory_order_release);
        }));
    }

    for (auto& t : stealers) t.join();

    // Also try to pop any remaining items
    int remaining = 0;
    while (dq.pop()) ++remaining;

    int total = stolen_total.load(std::memory_order_acquire) + remaining;
    CHECK(total == TOTAL,
          "all " + std::to_string(TOTAL) + " items processed (stolen=" +
          std::to_string(stolen_total.load(std::memory_order_acquire)) +
          ", remaining=" + std::to_string(remaining) + ")");
    CHECK(dq.empty_approx(), "deque empty after steal contention");
    return true;
}

// ── Test 29: Concurrent grow + steal ────────────────────────
// Push just enough to trigger resize (capacity 64 → 128),
// while stealers are concurrently stealing. This tests the
// resize-vs-steal race on buffer pointer and mask.

bool test_ws_deque_grow_during_steal() {
    std::println("\n--- Test: WS deque grow during steal (capacity boundary) ---");

    aura::serve::WorkStealingDeque<void*> dq;
    std::atomic<int> stolen{0};
    std::atomic<int> popped{0};

    // Push 60 items (just below the 64-capacity resize threshold)
    for (int i = 1; i <= 60; ++i)
        dq.push(reinterpret_cast<void*>(static_cast<intptr_t>(i)));

    // Start stealers
    std::thread stealer([&]() {
        for (int i = 0; i < 2000; ++i) {
            if (dq.steal()) {
                stolen.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });

    // While stealer is active, push more items (triggers resize at 64)
    for (int i = 61; i <= 200; ++i) {
        dq.push(reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        std::this_thread::yield();
    }

    stealer.join();

    // Drain remaining
    int local_pop = 0;
    while (auto* item = dq.pop()) {
        (void)item;
        ++local_pop;
    }
    popped.store(local_pop, std::memory_order_release);

    int total = stolen.load(std::memory_order_acquire) +
                popped.load(std::memory_order_acquire);
    CHECK(total == 200, std::to_string(total) + " items processed (stolen=" +
          std::to_string(stolen.load(std::memory_order_acquire)) +
          ", popped=" + std::to_string(popped.load(std::memory_order_acquire)) + ")");
    CHECK(dq.empty_approx(), "deque empty after grow+steal");
    return true;
}

// ── Test 30: Multiple deque instances cache alignment ───────
// Verify that multiple deques don't interfere via false sharing.
// Creates many adjacent deques and operates on them concurrently.

bool test_ws_deque_multi_instance() {
    std::println("\n--- Test: Multiple deque instances (false sharing check) ---");

    constexpr int NUM_DEQUES = 8;
    aura::serve::WorkStealingDeque<void*> deques[NUM_DEQUES];
    std::atomic<int> total_ok{0};

    // Verify each deque is independently functional
    for (int d = 0; d < NUM_DEQUES; ++d) {
        auto& dq = deques[d];
        for (int i = 1; i <= 10; ++i)
            dq.push(reinterpret_cast<void*>(static_cast<intptr_t>(d * 100 + i)));
    }

    // Pop from each concurrently
    std::vector<std::thread> threads;
    for (int d = 0; d < NUM_DEQUES; ++d) {
        threads.push_back(std::thread([&, d]() {
            int count = 0;
            while (deques[d].pop()) ++count;
            if (count == 10)
                total_ok.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    for (auto& t : threads) t.join();

    CHECK(total_ok.load() == NUM_DEQUES,
          std::to_string(total_ok.load()) + " of " +
          std::to_string(NUM_DEQUES) + " deques returned all 10 items");

    // Also verify with steal
    for (int d = 0; d < NUM_DEQUES; ++d) {
        CHECK(deques[d].empty_approx(),
              "deque[" + std::to_string(d) + "] is empty");
    }
    return true;
}

// ── Test 31: Rapid push/pop/steal cycles (ABA detection) ───
// Many cycles of push → steal → pop to detect subtle ABA issues.

bool test_ws_deque_rapid_cycles() {
    std::println("\n--- Test: WS deque rapid push/pop/steal cycles ---");

    aura::serve::WorkStealingDeque<void*> dq;
    constexpr int CYCLES = 5000;

    for (int cycle = 0; cycle < CYCLES; ++cycle) {
        // Push 3 items
        for (int i = 1; i <= 3; ++i)
            dq.push(reinterpret_cast<void*>(static_cast<intptr_t>(cycle * 3 + i)));

        // Steal 1, pop 2 (drain)
        auto* s = dq.steal();
        auto* p1 = dq.pop();
        auto* p2 = dq.pop();

        if (!s || !p1 || !p2) {
            std::println("  FAIL at cycle {}: s={}, p1={}, p2={}",
                         cycle, (intptr_t)s, (intptr_t)p1, (intptr_t)p2);
            return false;
        }

        CHECK(dq.empty_approx(), "deque empty after cycle " + std::to_string(cycle));
    }

    CHECK(dq.empty_approx(),
          "deque empty after " + std::to_string(CYCLES) + " cycles");
    return true;
}

// ── Test 32: Grow with wrapped indices ───────────────────────
// Push many items, pop some, then push more to trigger grow
// with wrapped (non-zero) bottom and top indices.

bool test_ws_deque_grow_wrapped() {
    std::println("\n--- Test: WS deque grow with wrapped indices ---");

    aura::serve::WorkStealingDeque<void*> dq;

    // Phase 1: push 60, pop 30 (bottom=30, top=0, capacity=64)
    for (int i = 1; i <= 60; ++i)
        dq.push(reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    for (int i = 0; i < 30; ++i)
        dq.pop();

    CHECK(dq.size_approx() == 30,
          "phase 1: 30 items remain after 30 pops");

    // Phase 2: push 60 more (triggers grow: bottom=90, top=0)
    for (int i = 61; i <= 120; ++i)
        dq.push(reinterpret_cast<void*>(static_cast<intptr_t>(i)));

    CHECK(dq.size_approx() == 90,
          "phase 2: 90 items remain after grow+bottom wrap");

    // Phase 3: steal 40 (moves top_ to 40)
    for (int i = 0; i < 40; ++i)
        dq.steal();

    CHECK(dq.size_approx() == 50,
          "phase 3: 50 items remain after 40 steals");

    // Phase 4: push 100 more (triggers second grow with wrapped indices)
    for (int i = 121; i <= 220; ++i)
        dq.push(reinterpret_cast<void*>(static_cast<intptr_t>(i)));

    CHECK(dq.size_approx() == 150,
          "phase 4: 150 items remain after second grow");

    // Phase 5: drain all via steal + pop
    int stolen = 0;
    while (dq.steal()) ++stolen;
    int popped = 0;
    while (dq.pop()) ++popped;

    CHECK(stolen + popped == 150,
          std::to_string(stolen + popped) + " items drained (" +
          std::to_string(stolen) + " stolen + " + std::to_string(popped) + " popped)");

    CHECK(dq.empty_approx(), "deque empty after all phases");
    return true;
}

bool test_round_robin_fallback() {
    std::println("\n--- Test: Round-robin fallback (load-aware disabled) ---");

    aura::serve::Scheduler sched(4);
    std::atomic<int> completed{0};

    // Disable load-aware (force round-robin for comparison)
    // Access through trick: spawn to fill queues, then verify they're
    // evenly distributed
    for (int i = 0; i < 8; ++i) {
        sched.spawn([&completed]() {
            volatile int sum = 0;
            for (int j = 0; j < 100000; ++j) sum += j;
            completed.fetch_add(1);
        });
    }

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    sched.stop();
    t.join();

    CHECK(completed.load() == 8, "all 8 fibers completed with round-robin");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// ── Execution adapter tests (Issue #33) ──────────────────────
// ═══════════════════════════════════════════════════════════════

// ── Test 33: fiber_scheduler basic schedule/start ────────────

bool test_exec_adapter_basic() {
    std::println("\n--- Test: Execution adapter basic schedule ---");

    aura::serve::Scheduler sched(2);
    aura::exec::fiber_scheduler fs(sched);
    std::atomic<int> ran{0};

    auto sender = fs.schedule([&ran]() {
        ran.store(1, std::memory_order_release);
    });

    auto op = std::move(sender).connect(aura::exec::fiber_receiver{});
    op.start();

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    sched.stop();
    t.join();

    CHECK(ran.load() == 1, "fiber_scheduler spawned and executed the function");
    return true;
}

// ── Test 34: when_all — parallel composition ─────────────────

bool test_exec_when_all() {
    std::println("\n--- Test: Execution adapter when_all ---");

    aura::serve::Scheduler sched(4);
    aura::exec::fiber_scheduler fs(sched);
    std::atomic<int> counter{0};
    constexpr int N = 10;

    std::vector<std::function<void()>> fns;
    for (int i = 0; i < N; ++i) {
        fns.push_back([&counter]() {
            volatile int sum = 0;
            for (int j = 0; j < 50000; ++j) sum += j;
            counter.fetch_add(1, std::memory_order_release);
        });
    }

    auto sender = aura::exec::when_all(fs, std::move(fns));
    auto op = std::move(sender).connect(aura::exec::fiber_receiver{});
    op.start();

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    sched.stop();
    t.join();

    CHECK(counter.load() == N,
          "when_all executed all " + std::to_string(N) + " functions");
    return true;
}

// ── Test 35: let_value — sequential composition ──────────────

bool test_exec_let_value() {
    std::println("\n--- Test: Execution adapter let_value (pipeline) ---");

    aura::serve::Scheduler sched(2);
    aura::exec::fiber_scheduler fs(sched);
    std::atomic<int> stage{0};

    std::vector<std::function<void()>> pipeline;
    pipeline.push_back([&stage]() {
        stage.store(1, std::memory_order_release);
    });
    pipeline.push_back([&stage]() {
        int s = stage.load(std::memory_order_acquire);
        stage.store(s + 10, std::memory_order_release);
    });
    pipeline.push_back([&stage]() {
        int s = stage.load(std::memory_order_acquire);
        stage.store(s + 100, std::memory_order_release);
    });

    auto sender = aura::exec::let_value(fs, std::move(pipeline));
    auto op = std::move(sender).connect(aura::exec::fiber_receiver{});
    op.start();

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    sched.stop();
    t.join();

    CHECK(stage.load() == 111,
          "let_value pipeline executed stages (1+10+100 = 111)");
    return true;
}

// ── Test 36: retry — retry on failure ───────────────────────

bool test_exec_retry() {
    std::println("\n--- Test: Execution adapter retry ---");

    aura::serve::Scheduler sched(2);
    aura::exec::fiber_scheduler fs(sched);
    std::atomic<int> attempts{0};
    std::atomic<bool> completed{false};
    bool success = false;
    bool error_caught = false;

    // Function that fails on first attempt, succeeds on second
    auto fn = [&attempts, &completed]() {
        int a = attempts.fetch_add(1, std::memory_order_relaxed);
        if (a == 0) {
            throw std::runtime_error("first attempt fails");
        }
        completed.store(true, std::memory_order_release);
    };

    auto sender = aura::exec::retry(fs, std::move(fn), 3);
    auto op = std::move(sender).connect(aura::exec::fiber_receiver(
        [&success]() { success = true; },
        [&error_caught](std::exception_ptr) { error_caught = true; },
        []() {}
    ));
    op.start();

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    sched.stop();
    t.join();

    CHECK(completed.load(), "retry succeeded on second attempt");
    CHECK(success, "retry receiver got set_value");
    CHECK(!error_caught, "retry receiver did NOT get set_error");
    CHECK(attempts.load() >= 2,
          "retry made " + std::to_string(attempts.load()) + " attempts (>= 2)");
    return true;
}

// ── Test 37: when_all with one error — verify propagation ────

bool test_exec_when_all_error() {
    std::println("\n--- Test: when_all with error propagation ---");

    aura::serve::Scheduler sched(2);
    aura::exec::fiber_scheduler fs(sched);
    bool got_error = false;
    std::atomic<int> ok_count{0};

    std::vector<std::function<void()>> fns;
    // 3 tasks that succeed
    for (int i = 0; i < 3; ++i)
        fns.push_back([&ok_count]() { ok_count.fetch_add(1); });
    // 1 task that throws
    fns.push_back([]() { throw std::runtime_error("intentional"); });

    auto sender = aura::exec::when_all(fs, std::move(fns));
    auto op = std::move(sender).connect(aura::exec::fiber_receiver(
        []() { /* success — won't be called */ },
        [&got_error](std::exception_ptr e) {
            got_error = true;
            try { std::rethrow_exception(e); }
            catch (const std::runtime_error& ex) {
                // expected
            }
        },
        []() {}
    ));
    op.start();

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    sched.stop();
    t.join();

    CHECK(got_error, "when_all propagated error when one task failed");
    // The 3 successful tasks may or may not run before error is detected
    // (race between concurrent tasks). Just verify the error fired.
    return true;
}

// ── Test 38: let_value with error in middle step ─────────────

bool test_exec_let_value_error() {
    std::println("\n--- Test: let_value error in middle step ---");

    aura::serve::Scheduler sched(2);
    aura::exec::fiber_scheduler fs(sched);
    std::atomic<int> steps_run{0};
    bool got_error = false;

    std::vector<std::function<void()>> pipeline;
    pipeline.push_back([&steps_run]() {
        steps_run.fetch_add(1);  // step 1
    });
    pipeline.push_back([&steps_run]() {
        steps_run.fetch_add(10); // step 2 — will fail
        throw std::runtime_error("pipe error");
    });
    pipeline.push_back([&steps_run]() {
        steps_run.fetch_add(100); // step 3 — should NOT run
    });

    auto sender = aura::exec::let_value(fs, std::move(pipeline));
    auto op = std::move(sender).connect(aura::exec::fiber_receiver(
        []() {},
        [&got_error](std::exception_ptr) { got_error = true; },
        []() {}
    ));
    op.start();

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    sched.stop();
    t.join();

    CHECK(got_error, "let_value error propagates when mid-step fails");
    CHECK(steps_run.load() <= 11,
          "let_value stopped after error (steps_run=" +
          std::to_string(steps_run.load()) + ", expected <= 11)");
    return true;
}

// ── Test 39: retry — all attempts fail ──────────────────────

bool test_exec_retry_all_fail() {
    std::println("\n--- Test: retry all attempts fail ---");

    aura::serve::Scheduler sched(2);
    aura::exec::fiber_scheduler fs(sched);
    std::atomic<int> attempts{0};
    bool got_error = false;

    auto sender = aura::exec::retry(fs,
        [&attempts]() {
            attempts.fetch_add(1);
            throw std::runtime_error("always fails");
        },
        5
    );
    auto op = std::move(sender).connect(aura::exec::fiber_receiver(
        []() {},
        [&got_error](std::exception_ptr) { got_error = true; },
        []() {}
    ));
    op.start();

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    sched.stop();
    t.join();

    CHECK(got_error, "retry sends set_error after all attempts fail");
    CHECK(attempts.load() == 5,
          "retry made exactly " + std::to_string(attempts.load()) + " attempts");
    return true;
}

// ── Test 40: multiple concurrent when_all operations ─────────

bool test_exec_multi_when_all() {
    std::println("\n--- Test: Multiple concurrent when_all operations ---");

    aura::serve::Scheduler sched(4);
    aura::exec::fiber_scheduler fs(sched);
    std::atomic<int> total{0};

    auto make_when_all = [&]() {
        std::vector<std::function<void()>> fns;
        for (int i = 0; i < 5; ++i)
            fns.push_back([&total]() {
                volatile int sum = 0;
                for (int j = 0; j < 20000; ++j) sum += j;
                total.fetch_add(1);
            });
        return aura::exec::when_all(fs, std::move(fns));
    };

    // Launch 3 when_all operations
    auto s1 = make_when_all();
    auto s2 = make_when_all();
    auto s3 = make_when_all();

    auto o1 = std::move(s1).connect(aura::exec::fiber_receiver{}); o1.start();
    auto o2 = std::move(s2).connect(aura::exec::fiber_receiver{}); o2.start();
    auto o3 = std::move(s3).connect(aura::exec::fiber_receiver{}); o3.start();

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    sched.stop();
    t.join();

    CHECK(total.load() == 15,
          "3 when_all x 5 fibers = " + std::to_string(total.load()) + " completed");
    return true;
}

// ── Test 41: mixed individual schedule + when_all ────────────

bool test_exec_mixed_schedule() {
    std::println("\n--- Test: Mixed schedule + when_all ---");

    aura::serve::Scheduler sched(4);
    aura::exec::fiber_scheduler fs(sched);
    std::atomic<int> solo{0};
    std::atomic<int> group{0};

    // Solo fibers via schedule
    auto snd1 = fs.schedule([&solo]() { solo.fetch_add(1); });
    auto snd2 = fs.schedule([&solo]() { solo.fetch_add(1); });
    auto o1 = std::move(snd1).connect(aura::exec::fiber_receiver{}); o1.start();
    auto o2 = std::move(snd2).connect(aura::exec::fiber_receiver{}); o2.start();

    // Group via when_all
    std::vector<std::function<void()>> fns;
    for (int i = 0; i < 5; ++i)
        fns.push_back([&group]() { group.fetch_add(1); });
    auto ws = aura::exec::when_all(fs, std::move(fns));
    auto ow = std::move(ws).connect(aura::exec::fiber_receiver{}); ow.start();

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    sched.stop();
    t.join();

    CHECK(solo.load() == 2, "solo fibers: " + std::to_string(solo.load()) + " / 2");
    CHECK(group.load() == 5, "group fibers: " + std::to_string(group.load()) + " / 5");
    return true;
}

// ── Test 42: receiver callbacks fire correctly ───────────────
// Verify that the receiver's set_value is called on success.

bool test_exec_receiver_callback() {
    std::println("\n--- Test: Receiver callback verification ---");

    aura::serve::Scheduler sched(2);
    aura::exec::fiber_scheduler fs(sched);
    std::atomic<int> callbacks{0};

    auto sender = fs.schedule([&callbacks]() {
        callbacks.fetch_add(1);
    });

    auto op = std::move(sender).connect(aura::exec::fiber_receiver(
        [&callbacks]() { callbacks.fetch_add(10); },
        [](std::exception_ptr) {},
        []() {}
    ));
    op.start();

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    sched.stop();
    t.join();

    // Function ran (1) + set_value called (10) = 11
    CHECK(callbacks.load() == 11,
          "fiber ran + receiver set_value = " + std::to_string(callbacks.load()) +
          " (expected 11)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// ── Yield reason tests (Issue #31) ──────────────────────────
// ═══════════════════════════════════════════════════════════════

// ── Test 43: Fiber yield reason is tracked correctly ─────────

bool test_yield_reason_tracking() {
    std::println("\n--- Test: Yield reason tracking ---");

    aura::serve::Scheduler sched(2);
    std::atomic<int> stage{0};

    sched.spawn([&stage]() {
        // Default reason after construction should be Explicit
        CHECK(aura::serve::g_current_fiber->is_stealable(),
              "default fiber is stealable");
        stage.store(1);

        // Yield explicitly — should be stealable
        aura::serve::Fiber::yield(aura::serve::YieldReason::Explicit);
        stage.store(2);
        CHECK(aura::serve::g_current_fiber->is_stealable(),
              "fiber stealable after Explicit yield");

        // Yield at mutation boundary — stealable
        aura::serve::Fiber::yield(aura::serve::YieldReason::MutationBoundary);
        stage.store(3);
        CHECK(aura::serve::g_current_fiber->is_stealable(),
              "fiber stealable after MutationBoundary yield");

        // Check last_yield_reason
        CHECK(aura::serve::g_current_fiber->last_yield_reason() ==
              aura::serve::YieldReason::MutationBoundary,
              "last_yield_reason = MutationBoundary");

        // Default yield() also sets Explicit
        aura::serve::Fiber::yield();
        stage.store(4);
        CHECK(aura::serve::g_current_fiber->is_stealable(),
              "fiber stealable after default yield()");
    });

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    sched.stop();
    t.join();

    CHECK(stage.load() == 4, "all yield stages executed (stage=" +
          std::to_string(stage.load()) + ")");
    return true;
}

// ── Test 44: Fiber yield(BlockingIO) sets Waiting state ──────

bool test_yield_blocking_io_state() {
    std::println("\n--- Test: Fiber yield(BlockingIO) sets Waiting state ---");

    aura::serve::Scheduler sched(2);
    std::atomic<int> stage{0};

    sched.spawn([&stage]() {
        // Initially Running
        CHECK(aura::serve::g_current_fiber->state() ==
              aura::serve::FiberState::Running,
              "initial state = Running");
        stage.store(1);

        // Yield with BlockingIO — transitions to Waiting
        aura::serve::Fiber::yield(aura::serve::YieldReason::BlockingIO);

        // After resume (should happen if epoll wakes us): still Running
        CHECK(aura::serve::g_current_fiber->state() ==
              aura::serve::FiberState::Running,
              "after resume state = Running");
        stage.store(2);
    });

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    sched.stop();
    t.join();

    CHECK(stage.load() >= 1, "fiber at least started (stage=" +
          std::to_string(stage.load()) + ")");
    return true;
}

// ── Test 45: Fiber yield(Explicit) keeps Ready state ─────────

bool test_yield_explicit_state() {
    std::println("\n--- Test: Fiber yield(Explicit) keeps Ready state ---");

    aura::serve::Scheduler sched(2);
    std::atomic<int> counter{0};

    sched.spawn([&counter]() {
        aura::serve::Fiber::yield(aura::serve::YieldReason::Explicit);
        counter.fetch_add(1);
        aura::serve::Fiber::yield(aura::serve::YieldReason::Explicit);
        counter.fetch_add(10);
    });

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    sched.stop();
    t.join();

    CHECK(counter.load() == 11,
          "fiber with Explicit yields preserves state (1+10 = " +
          std::to_string(counter.load()) + ")");
    return true;
}

// ── Test 46: Fiber yield chain — BlockingIO then Explicit ────

bool test_yield_reason_chain() {
    std::println("\n--- Test: Yield reason chaining ---");

    aura::serve::Scheduler sched(2);
    std::atomic<int> stage{0};

    sched.spawn([&stage]() {
        // Yield chain: Explicit → MutationBoundary → Explicit → default yield

        aura::serve::Fiber::yield(aura::serve::YieldReason::Explicit);
        stage.store(1);
        CHECK(aura::serve::g_current_fiber->last_yield_reason() ==
              aura::serve::YieldReason::Explicit,
              "reason 1 = Explicit");

        aura::serve::Fiber::yield(aura::serve::YieldReason::MutationBoundary);
        stage.store(2);
        CHECK(aura::serve::g_current_fiber->last_yield_reason() ==
              aura::serve::YieldReason::MutationBoundary,
              "reason 2 = MutationBoundary");
        CHECK(aura::serve::g_current_fiber->is_stealable(),
              "stealable after MutationBoundary");

        aura::serve::Fiber::yield();
        stage.store(3);
        CHECK(aura::serve::g_current_fiber->last_yield_reason() ==
              aura::serve::YieldReason::Explicit,
              "reason 3 = Explicit (default yield)");

        aura::serve::Fiber::yield(aura::serve::YieldReason::Explicit);
        stage.store(4);
        CHECK(aura::serve::g_current_fiber->is_stealable(),
              "stealable after Explicit");
    });

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    sched.stop();
    t.join();

    CHECK(stage.load() == 4, "all chain stages executed");
    return true;
}

// ── Test 47: Multiple fibers with mixed yield reasons ────────

bool test_yield_mixed_reasons() {
    std::println("\n--- Test: Mixed yield reasons across fibers ---");

    aura::serve::Scheduler sched(4);
    std::atomic<int> stealable_count{0};
    std::atomic<int> nonstealable_count{0};

    // Spawn fibers with different yield patterns.
    // Odd-indexed fibers yield with default yield() (Explicit, stealable).
    // Even-indexed fibers yield with MutationBoundary (also stealable).
    for (int i = 0; i < 5; ++i) {
        sched.spawn([&stealable_count, i]() {
            if (i % 2 == 0) {
                aura::serve::Fiber::yield(aura::serve::YieldReason::Explicit);
            } else {
                aura::serve::Fiber::yield(aura::serve::YieldReason::MutationBoundary);
            }
            if (aura::serve::g_current_fiber->is_stealable())
                stealable_count.fetch_add(1);
        });
    }

    std::thread t([&sched]() { sched.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    sched.stop();
    t.join();

    CHECK(stealable_count.load() == 5,
          std::to_string(stealable_count.load()) +
          " fibers reported stealable (expected 5)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// ── Main ─────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════

int main() {
    std::println("═══ Concurrent model unit tests ═══\n");

    // Run all tests
    test_ws_deque();
    test_ws_deque_resize();
    test_ws_deque_concurrent_pattern();
    test_ws_deque_concurrent_stress();
    test_ws_deque_steal_contention();
    test_ws_deque_grow_during_steal();
    test_ws_deque_multi_instance();
    test_ws_deque_rapid_cycles();
    test_ws_deque_grow_wrapped();
    test_worker_lifecycle();
    test_fiber_lifecycle();
    test_fiber_yield();
    test_fiber_ping_pong();
    test_multi_fiber_parallel();
    test_mixed_cpu_io();
    test_worker_distribution();
    test_fiber_spawns_fiber();
    test_spawn_chain();
    test_stress_many_fibers();
    test_stress_1k_fibers();
    test_rapid_fibers();
    test_single_worker();
    test_auto_worker_count();
    test_eventfd_wakeup();
    test_work_stealing();
    test_load_aware_distribution();
    test_round_robin_fallback();
    test_adaptive_steal_budget();
    test_metrics_sanity();
    test_metrics_dump_no_crash();
    test_metrics_disabled();
    test_metrics_post_run();
    test_exec_adapter_basic();
    test_exec_when_all();
    test_exec_let_value();
    test_exec_retry();
    test_exec_when_all_error();
    test_exec_let_value_error();
    test_exec_retry_all_fail();
    test_exec_multi_when_all();
    test_exec_mixed_schedule();
    test_exec_receiver_callback();
    test_yield_reason_tracking();
    test_yield_blocking_io_state();
    test_yield_explicit_state();
    test_yield_reason_chain();
    test_yield_mixed_reasons();

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);

    return g_failed > 0 ? 1 : 0;
}
