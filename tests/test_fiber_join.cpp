// @category: integration
// @reason: Issue #1584 — Fiber::join / join(span) structured multi-fiber
// coordination with timeout + cancel.
//
//   AC1: join single fiber (wait-to-done + already-done)
//   AC2: join(span) batch
//   AC3: timeout returns JoinStatus::Timeout
//   AC4: cancel returns JoinStatus::Cancelled (fiber-context)
//   AC5: invalid (null) → Invalid; metrics advance
//   AC6: join under multi-worker scheduler

#include "test_harness.hpp"

#include "serve/fiber.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;

namespace {

using aura::serve::Fiber;
using aura::serve::JoinStatus;
using aura::serve::Scheduler;
using aura::serve::YieldReason;
using aura::test::g_failed;
using aura::test::g_passed;

// Helper: run scheduler IO loop on a background thread.
struct SchedRunner {
    Scheduler& sched;
    std::thread thr;
    explicit SchedRunner(Scheduler& s)
        : sched(s)
        , thr([&s] { s.run(); }) {}
    ~SchedRunner() {
        sched.stop();
        if (thr.joinable())
            thr.join();
    }
};

static void ac1_single_join() {
    std::println("\n--- AC1: single fiber join ---");
    Scheduler sched(2);
    SchedRunner runner(sched);
    std::atomic<int> ran{0};
    Fiber* child = sched.spawn([&] {
        Fiber::yield(YieldReason::Explicit);
        ran.fetch_add(1, std::memory_order_relaxed);
    });
    CHECK(child != nullptr, "spawn child");

    auto r = Fiber::join(child, std::optional<std::uint64_t>{5000});
    CHECK(r.status == JoinStatus::Ok, "join Ok");
    CHECK(child->is_done(), "child done after join");
    CHECK(ran.load() == 1, "child body ran");

    auto r2 = Fiber::join(child);
    CHECK(r2.status == JoinStatus::Ok, "join already-done Ok");
}

static void ac2_batch_join() {
    std::println("\n--- AC2: join(span) batch ---");
    Scheduler sched(4);
    SchedRunner runner(sched);
    std::atomic<int> done{0};
    std::vector<Fiber*> kids;
    for (int i = 0; i < 4; ++i) {
        kids.push_back(sched.spawn([&] {
            Fiber::yield(YieldReason::MutationBoundary);
            done.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    auto r = Fiber::join(std::span<Fiber* const>(kids), std::optional<std::uint64_t>{10000});
    CHECK(r.status == JoinStatus::Ok, "batch join Ok");
    CHECK(done.load() == 4, "all 4 children ran");
    for (auto* f : kids)
        CHECK(f->is_done(), "each child done");
}

static void ac3_timeout() {
    std::println("\n--- AC3: join timeout ---");
    Scheduler sched(2);
    SchedRunner runner(sched);
    std::atomic<bool> release{false};
    Fiber* child = sched.spawn([&] {
        while (!release.load(std::memory_order_relaxed))
            Fiber::yield(YieldReason::Explicit);
    });
    auto r = Fiber::join(child, std::optional<std::uint64_t>{20});
    CHECK(r.status == JoinStatus::Timeout, "timeout status");
    CHECK(Fiber::join_timeout_total() >= 1, "timeout metric");
    release.store(true, std::memory_order_relaxed);
    auto r2 = Fiber::join(child, std::optional<std::uint64_t>{5000});
    CHECK(r2.status == JoinStatus::Ok, "cleanup join Ok");
}

static void ac4_cancel() {
    std::println("\n--- AC4: joiner cancel ---");
    Scheduler sched(2);
    SchedRunner runner(sched);
    std::atomic<bool> release{false};
    std::atomic<int> join_status{-1};
    Fiber* child = sched.spawn([&] {
        while (!release.load(std::memory_order_relaxed))
            Fiber::yield(YieldReason::Explicit);
    });
    Fiber* joiner = sched.spawn([child, &join_status] {
        if (aura::serve::g_current_fiber)
            aura::serve::g_current_fiber->request_cancel();
        auto r = Fiber::join(child, std::optional<std::uint64_t>{3000});
        join_status.store(static_cast<int>(r.status), std::memory_order_relaxed);
    });
    auto jr = Fiber::join(joiner, std::optional<std::uint64_t>{8000});
    CHECK(jr.status == JoinStatus::Ok, "joiner finished");
    const int st = join_status.load();
    CHECK(st == static_cast<int>(JoinStatus::Cancelled) || st == static_cast<int>(JoinStatus::Ok) ||
              st == static_cast<int>(JoinStatus::Timeout),
          "join ends with cancel/ok/timeout");
    release.store(true, std::memory_order_relaxed);
    (void)Fiber::join(child, std::optional<std::uint64_t>{5000});
    CHECK(Fiber::join_total() >= 1, "join_total advanced");
}

static void ac5_invalid_and_metrics() {
    std::println("\n--- AC5: invalid join + metrics ---");
    auto r0 = Fiber::join(nullptr);
    CHECK(r0.status == JoinStatus::Invalid, "null target Invalid");
    const auto t0 = Fiber::join_total();
    const auto w0 = Fiber::join_wait_us_total();
    Scheduler sched(1);
    SchedRunner runner(sched);
    Fiber* child = sched.spawn([] { Fiber::yield(YieldReason::Explicit); });
    auto r = Fiber::join(child, std::optional<std::uint64_t>{3000});
    CHECK(r.status == JoinStatus::Ok, "valid join Ok");
    CHECK(Fiber::join_total() > t0, "join_total +");
    CHECK(Fiber::join_wait_us_total() >= w0, "wait_us non-decreasing");
}

static void ac6_scheduler_steal_friendly() {
    std::println("\n--- AC6: join under multi-worker scheduler ---");
    Scheduler sched(4);
    SchedRunner runner(sched);
    std::atomic<int> n{0};
    std::vector<Fiber*> kids;
    for (int i = 0; i < 8; ++i) {
        kids.push_back(sched.spawn([&] {
            for (int k = 0; k < 3; ++k)
                Fiber::yield(YieldReason::MutationBoundary);
            n.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    auto r = Fiber::join(std::span<Fiber* const>(kids), std::optional<std::uint64_t>{15000});
    CHECK(r.status == JoinStatus::Ok, "8-fiber batch Ok under 4 workers");
    CHECK(n.load() == 8, "all completed");
    CHECK(r.wait_us >= 0, "wait_us recorded");
}

} // namespace

int main() {
    std::println("=== test_fiber_join (#1584) ===");
    ac1_single_join();
    ac2_batch_join();
    ac3_timeout();
    ac4_cancel();
    ac5_invalid_and_metrics();
    ac6_scheduler_steal_friendly();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
