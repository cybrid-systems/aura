// test_per_fiber_stack_pool_high_concurrency.cpp — Issue #652:
// Bounded per-fiber MutationStack + YieldCheckpoint pool for
// high-concurrency AI agent orchestration (steal/yield/panic).
//
// Non-duplicative with #707 (same pool ship), #588 (per-fiber
// stack sync), #542 (mutation steal safety), #592 (panic resume).
//
//   - AC1:  query:per-fiber-stack-pool-stats reachable (schema 652)
//   - AC2:  200+ fiber waves — pool reuse >= 80%
//   - AC3:  lazy allocs bounded under high concurrency
//   - AC4:  steal/yield/panic cycles — max-depth capped
//   - AC5:  churn-reductions tracks pool hits
//   - AC6:  multi-round scheduler — stats monotonic
//   - AC7:  query regression (scheduler-stealbudget, panic-fiber)
//
// Uses Scheduler + Fiber for real steal/yield pressure.

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "serve/scheduler.h"
#include "serve/worker.h"

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

extern "C" void aura_evaluator_test_push_mutation_checkpoint();
extern "C" void aura_evaluator_test_pop_mutation_checkpoint();

namespace aura_652_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::serve::Fiber;
using aura::serve::Scheduler;
using aura::serve::YieldReason;

static std::int64_t hash_int(CompilerService& cs, const std::string& key) {
    auto r =
        cs.eval("(hash-ref (engine:metrics \"query:per-fiber-stack-pool-stats\") \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto hits = hash_int(cs, "pool-hits");
    const auto lazy = hash_int(cs, "lazy-allocs");
    const auto depth = hash_int(cs, "max-depth");
    const auto churn = hash_int(cs, "churn-reductions");
    if (hits < 0 || lazy < 0 || depth < 0 || churn < 0)
        return -1;
    return hits + lazy + depth + churn;
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:per-fiber-stack-pool-stats (schema 652) ---");
    auto h = cs.eval("(engine:metrics \"query:per-fiber-stack-pool-stats\")");
    CHECK(h && is_hash(*h), "per-fiber-stack-pool-stats returns hash");
    CHECK(hash_int(cs, "schema") == 652, "schema == 652");
    CHECK(hash_int(cs, "pool-hits") >= 0, "pool-hits present");
    CHECK(hash_int(cs, "lazy-allocs") >= 0, "lazy-allocs present");
    CHECK(hash_int(cs, "max-depth") >= 0, "max-depth present");
    CHECK(hash_int(cs, "churn-reductions") >= 0, "churn-reductions present");

    const auto pool_before = hash_int(cs, "pool-hits");
    const auto lazy_before = hash_int(cs, "lazy-allocs");

    std::println("\n--- AC2: 200+ fiber pool reuse (2 waves) ---");
    constexpr int k_per_wave = 100;
    auto run_wave = [&](int wave) {
        Scheduler sched(8);
        std::atomic<int> done{0};
        for (int f = 0; f < k_per_wave; ++f) {
            sched.spawn([&]() {
                aura_evaluator_test_push_mutation_checkpoint();
                Fiber::yield(YieldReason::MutationBoundary);
                aura_evaluator_test_pop_mutation_checkpoint();
                Fiber::yield(YieldReason::Explicit);
                done.fetch_add(1, std::memory_order_relaxed);
            });
        }
        std::thread io([&sched]() { sched.run(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        while (done.load() < k_per_wave && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        sched.stop();
        io.join();
        CHECK(done.load() == k_per_wave,
              std::format("wave {} completed ({}/{})", wave, done.load(), k_per_wave));
    };

    run_wave(0);
    const auto lazy_after_wave0 = hash_int(cs, "lazy-allocs");
    const auto pool_after_wave0 = hash_int(cs, "pool-hits");
    run_wave(1);

    const auto pool_after = hash_int(cs, "pool-hits");
    const auto lazy_after = hash_int(cs, "lazy-allocs");
    const auto pool_delta = pool_after - pool_before;
    const auto lazy_delta = lazy_after - lazy_before;
    const auto wave2_lazy = lazy_after - lazy_after_wave0;
    const auto wave2_pool = pool_after - pool_after_wave0;
    const auto wave2_total = wave2_lazy + wave2_pool;
    const int wave2_reuse_pct =
        wave2_total > 0 ? static_cast<int>(100 * wave2_pool / wave2_total) : 0;
    std::println("  pool_delta={} lazy_delta={} wave2_lazy={} wave2_pool={} wave2_reuse={}%",
                 pool_delta, lazy_delta, wave2_lazy, wave2_pool, wave2_reuse_pct);

    std::println("\n--- AC3: lazy allocs bounded ---");
    CHECK(lazy_delta <= 128, std::format("lazy allocs bounded ({} <= 128)", lazy_delta));
    CHECK(wave2_lazy <= 8, std::format("wave-2 lazy allocs minimal ({} <= 8)", wave2_lazy));

    std::println("\n--- AC2/AC5: pool reuse + churn tracking ---");
    CHECK(wave2_reuse_pct >= 80,
          std::format("wave-2 pool reuse >= 80% (got {}%)", wave2_reuse_pct));
    CHECK(hash_int(cs, "churn-reductions") >= pool_delta, "churn-reductions tracks pool hits");

    std::println("\n--- AC4: steal/yield/panic mixed cycles ---");
    const auto restamp_before = hash_int(cs, "restamps");
    Scheduler sched(8);
    std::atomic<int> done{0};
    constexpr int k_fibers = 32;
    for (int f = 0; f < k_fibers; ++f) {
        sched.spawn_with_affinity(
            [&, f]() {
                std::mt19937 rng(static_cast<unsigned>(652u + f));
                std::uniform_int_distribution<int> coin(0, 3);
                for (int i = 0; i < 30; ++i) {
                    if (coin(rng) == 0) {
                        aura_evaluator_test_push_mutation_checkpoint();
                        Fiber::yield(YieldReason::MutationBoundary);
                        aura_evaluator_test_pop_mutation_checkpoint();
                    } else if (coin(rng) == 1) {
                        Fiber::yield(YieldReason::MutationBoundary);
                    } else {
                        Fiber::yield(YieldReason::Explicit);
                    }
                }
                done.fetch_add(1, std::memory_order_relaxed);
            },
            f % 2);
    }
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    while (done.load() < k_fibers && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    sched.stop();
    io.join();
    CHECK(done.load() == k_fibers, "mixed steal/yield fibers completed");
    CHECK(hash_int(cs, "max-depth") <= 64,
          std::format("max-depth capped (got {})", hash_int(cs, "max-depth")));
    const auto restamp_after = hash_int(cs, "restamps");
    CHECK(restamp_after >= restamp_before,
          std::format("restamps monotonic ({} -> {})", restamp_before, restamp_after));

    std::println("\n--- AC6: multi-round scheduler stats monotonic ---");
    const auto stats6a = stats_sum(cs);
    run_wave(2);
    const auto stats6b = stats_sum(cs);
    std::println("  pool stats sum: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "pool stats monotonic over extra wave");

    std::println("\n--- AC7: query regression ---");
    auto steal = cs.eval("(engine:metrics \"query:scheduler-stealbudget-adaptive-stats\")");
    auto panic = cs.eval("(engine:metrics \"query:panic-checkpoint-fiber-stats\")");
    CHECK(steal && is_hash(*steal), "scheduler-stealbudget-adaptive-stats regression");
    CHECK(panic && is_hash(*panic), "panic-checkpoint-fiber-stats regression");
}

} // namespace aura_652_detail

int aura_issue_per_fiber_stack_pool_high_concurrency_run() {
    aura::compiler::CompilerService cs;
    aura_652_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_per_fiber_stack_pool_high_concurrency_run();
}
#endif
