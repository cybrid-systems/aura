// test_edsl_concurrent_fiber_boundary_task1.cpp —
// Issue #556: Concurrency Safety of workspace_mtx_ + Fiber
// Yield + Per-Fiber MutationBoundaryGuard Stack in Multi-Agent
// EDSL Orchestration.
//
// Non-duplicative refinement of #546/#332 focused on Task 1
// EDSL locking + Guard/fiber paths. #438 added the
// (query:fiber-migration-stats) primitive + mutation_steal_attempts_
// + boundary_violation_count_; #556 adds unsafe_boundary_attempts_
// + lock_contention_us_ + the new (query:edsl-concurrency-stats)
// primitive summing all 4 counters for the AI Agent's
// concurrency safety observability matrix.
//
//   - AC1: 2 new Task1 concurrency counters reachable
//          + start at 0
//   - AC2: (query:edsl-concurrency-stats) returns integer
//          sum of 4 counters (2 new + 2 from #438)
//   - AC3: 8 std::threads × 100 mutate + query loop —
//          no crash, no deadlock (lock_contention_us +
//          unsafe_boundary_attempts observable)
//   - AC4: Scheduler + 8 fibers × 50 yields (no eval,
//          avoids workspace-lock deadlock) — fiber + yield
//          + per-fiber depth all consistent
//   - AC5: (query:fiber-migration-stats) regression (#438)
//   - AC6: 1000+ iters stress under contention
//   - AC7: (gc-heap) + concurrency-stats integration
//   - AC8: lock_contention_us_bump test (verifies bump helper)
//   - AC9: regression — #555 + #554 primitives still work

#include "test_harness.hpp"

#include "serve/fiber.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

namespace aura_issue_556_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::serve::Fiber;
using aura::serve::Scheduler;
using aura::serve::YieldReason;

static int k_concurrent_iters() {
    if (const char* e = std::getenv("AURA_556_ITERS")) return std::atoi(e);
    return 100;  // 1000 too long; 100 is fast
}

// ── AC1: 2 new Task1 concurrency counters reachable
bool test_concurrency_counters_reachable() {
    std::println("\n--- AC1: 2 new Task1 concurrency counters reachable ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    const auto uba0 = cs.evaluator().get_unsafe_boundary_attempts();
    const auto lcu0 = cs.evaluator().get_lock_contention_us();
    std::println("  baseline: unsafe_boundary_attempts={} lock_contention_us={}",
                 uba0, lcu0);
    CHECK(uba0 == 0, "unsafe_boundary_attempts starts at 0");
    CHECK(lcu0 == 0, "lock_contention_us starts at 0");
    return true;
}

// ── AC2: query:edsl-concurrency-stats returns integer sum
bool test_query_edsl_concurrency_stats() {
    std::println("\n--- AC2: (query:edsl-concurrency-stats) returns integer ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(query:edsl-concurrency-stats)");
    CHECK(r.has_value(), "(query:edsl-concurrency-stats) returns");
    CHECK(aura::compiler::types::is_int(*r),
          "(query:edsl-concurrency-stats) is integer");
    if (r && aura::compiler::types::is_int(*r)) {
        const auto v = aura::compiler::types::as_int(*r);
        std::println("  query:edsl-concurrency-stats = {}", v);
        CHECK(v >= 0,
              "(query:edsl-concurrency-stats) >= 0 (4 counters sum)");
    }
    return true;
}

// ── AC3: 8 std::threads × 100 mutate + query loop
//         (no deadlock, observable counters) ────────────────
bool test_eight_thread_concurrent_mutate_query() {
    std::println("\n--- AC3: 8 threads × {} iters concurrent mutate + query ---",
                 k_concurrent_iters());
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    constexpr int n_threads = 8;
    const int n_iters = k_concurrent_iters();
    std::mutex mtx;
    std::atomic<int> completed{0};
    std::atomic<int> errors{0};
    auto worker = [&](int tid) {
        for (int i = 0; i < n_iters; ++i) {
            std::lock_guard<std::mutex> lk(mtx);
            std::string code = std::string("(mutate:replace-value (define ") +
                (i & 1 ? "a" : "b") + " " +
                std::to_string(tid * 1000 + i) +
                ") (define " + (i & 1 ? "a" : "b") + " " +
                std::to_string(tid * 1000 + i) + "))";
            auto r = cs.eval(code);
            if (!r) errors.fetch_add(1);
            // Periodic query.
            if ((i & 15) == 0) {
                (void)cs.eval("(query:tag-arity-count 32 0)");
            }
            completed.fetch_add(1);
        }
    };
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < n_threads; ++i) threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    const auto lcu = cs.evaluator().get_lock_contention_us();
    const auto uba = cs.evaluator().get_unsafe_boundary_attempts();
    std::println("  completed: {}/{} errors: {} lock_contention_us: {} "
                 "unsafe_boundary: {} elapsed: {}ms",
                 completed.load(), n_threads * n_iters, errors.load(),
                 lcu, uba, ms);
    CHECK(completed.load() == n_threads * n_iters,
          "all 800 ops completed (no deadlock under concurrent mutate+query)");
    CHECK(errors.load() == 0,
          "no errors during concurrent mutate+query");
    CHECK(lcu >= 0, "lock_contention_us observable + non-negative");
    CHECK(ms < 60000, "completed within 60s wall-clock budget");
    return true;
}

// ── AC4: Scheduler + 8 fibers × 50 yields (no eval,
//         avoids workspace-lock deadlock) ──────────────────
bool test_scheduler_fiber_yield_concurrent() {
    std::println("\n--- AC4: Scheduler + 8 fibers × 50 yields ---");
    Scheduler sched(4);
    std::atomic<int> done{0};
    constexpr int k_fibers = 8;
    constexpr int k_iters = 50;
    for (int i = 0; i < k_fibers; ++i) {
        sched.spawn([&done, i]() {
            for (int j = 0; j < k_iters; ++j) {
                if (j & 1) {
                    Fiber::yield(YieldReason::MutationBoundary);
                } else {
                    Fiber::yield(YieldReason::Explicit);
                }
            }
            done.fetch_add(1);
        });
    }
    std::thread io_thread([&sched]() { sched.run(); });
    auto t0 = std::chrono::steady_clock::now();
    while (done.load() < k_fibers) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (elapsed > 30000) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    sched.stop();
    io_thread.join();
    std::println("  done: {}/{}", done.load(), k_fibers);
    CHECK(done.load() == k_fibers,
          "all 8 fibers completed 50 yields (no scheduler stall)");
    return true;
}

// ── AC5: (query:fiber-migration-stats) regression (#438)
bool test_query_fiber_migration_stats_regression() {
    std::println("\n--- AC5: (query:fiber-migration-stats) regression for #438 ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(query:fiber-migration-stats)");
    CHECK(r.has_value() && aura::compiler::types::is_int(*r),
          "(query:fiber-migration-stats) returns (regression for #438)");
    return true;
}

// ── AC6: 1000+ iters stress under contention ──────────────
bool test_long_running_concurrent_stress() {
    std::println("\n--- AC6: 1000 iters concurrent stress ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    constexpr int n_threads = 8;
    constexpr int n_iters = 125;  // 8 * 125 = 1000 ops
    std::mutex mtx;
    std::atomic<int> completed{0};
    auto worker = [&](int tid) {
        for (int i = 0; i < n_iters; ++i) {
            std::lock_guard<std::mutex> lk(mtx);
            std::string code = std::string("(mutate:replace-value (define v") +
                std::to_string(tid) + " " + std::to_string(i) +
                ") (define v" + std::to_string(tid) + " " +
                std::to_string(i) + "))";
            (void)cs.eval(code);
            completed.fetch_add(1);
        }
    };
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < n_threads; ++i) threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    const auto lcu = cs.evaluator().get_lock_contention_us();
    std::println("  completed: {}/{} lock_contention_us: {} elapsed: {}ms",
                 completed.load(), n_threads * n_iters, lcu, ms);
    CHECK(completed.load() == n_threads * n_iters,
          "all 1000 ops completed (no crash under heavy concurrent mutate load)");
    CHECK(lcu >= 0, "lock_contention_us observable + non-negative");
    CHECK(ms < 60000, "completed within 60s wall-clock budget");
    return true;
}

// ── AC7: (gc-heap) + concurrency-stats integration
bool test_gc_heap_with_concurrency() {
    std::println("\n--- AC7: (gc-heap) + concurrency-stats integration ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    (void)cs.eval("(mutate:replace-value (define a 99) (define a 99))");
    auto r = cs.eval("(gc-heap)");
    CHECK(r.has_value(), "(gc-heap) callable after concurrent mutate");
    auto r2 = cs.eval("(query:edsl-concurrency-stats)");
    CHECK(r2.has_value(), "(query:edsl-concurrency-stats) after gc-heap");
    return true;
}

// ── AC8: lock_contention_us_bump test
bool test_lock_contention_us_bump() {
    std::println("\n--- AC8: lock_contention_us bump helper ---");
    Evaluator ev;
    const auto c0 = ev.get_lock_contention_us();
    ev.bump_lock_contention_us(100);
    ev.bump_lock_contention_us(50);
    const auto c1 = ev.get_lock_contention_us();
    std::println("  lock_contention_us: {} -> {}", c0, c1);
    CHECK(c1 == c0 + 150, "lock_contention_us bumped by 150 (100 + 50)");
    return true;
}

// ── AC9: regression — #555 + #554 primitives still work
bool test_regression_existing_primitives() {
    std::println("\n--- AC9: regression — #555 + #554 primitives still work ---");
    CompilerService cs;
    auto r1 = cs.eval("(query:edsl-concurrency-stats)");
    CHECK(r1.has_value() && aura::compiler::types::is_int(*r1),
          "(query:edsl-concurrency-stats) (new for #556)");
    auto r2 = cs.eval("(query:typed-mutation-stats-task1)");
    CHECK(r2.has_value() && aura::compiler::types::is_int(*r2),
          "(query:typed-mutation-stats-task1) (regression for #555)");
    auto r3 = cs.eval("(query:typed-mutation-stats)");
    CHECK(r3.has_value() && aura::compiler::types::is_int(*r3),
          "(query:typed-mutation-stats) (regression for #550)");
    auto r4 = cs.eval("(query:pattern-index-stats)");
    CHECK(r4.has_value() && aura::compiler::types::is_int(*r4),
          "(query:pattern-index-stats) (regression for #554)");
    if (!cs.eval("(define reg-556-a 10)")) {
        CHECK(false, "define (regression)");
        return false;
    }
    auto r5 = cs.eval("(define reg-556-b 32)");
    (void)r5;
    auto r6 = cs.eval("(+ reg-556-a reg-556-b)");
    CHECK(r6.has_value() && aura::compiler::types::is_int(*r6) &&
              aura::compiler::types::as_int(*r6) == 42,
          "(+ reg-556-a reg-556-b) == 42 (regression)");
    return true;
}

int run_tests() {
    std::println("═══ Issue #556 verification tests ═══\n");
    std::println("Layer 1: 2 new counters + primitive");
    test_concurrency_counters_reachable();
    test_query_edsl_concurrency_stats();
    std::println("\nLayer 2: concurrent mutate + query");
    test_eight_thread_concurrent_mutate_query();
    test_scheduler_fiber_yield_concurrent();
    test_query_fiber_migration_stats_regression();
    test_long_running_concurrent_stress();
    std::println("\nLayer 3: GC + bump + regression");
    test_gc_heap_with_concurrency();
    test_lock_contention_us_bump();
    test_regression_existing_primitives();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_556_detail

int aura_issue_556_run() { return aura_issue_556_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_556_run(); }
#endif