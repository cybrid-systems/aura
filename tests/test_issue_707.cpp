// @category: integration
// @reason: Issue #707 bounded per-fiber stack pool + panic/steal re-stamp

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <print>
#include <random>
#include <string>
#include <thread>

#include "serve/scheduler.h"
#include "serve/worker.h"

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

extern "C" void aura_evaluator_test_push_mutation_checkpoint();
extern "C" void aura_evaluator_test_pop_mutation_checkpoint();

namespace aura_issue_707_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t stat_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:per-fiber-stack-pool-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_707_detail

int main() {
    using namespace aura_issue_707_detail;
    using aura::serve::Fiber;
    using aura::serve::Scheduler;
    using aura::serve::YieldReason;

    std::println("=== Issue #707: Per-fiber stack pool + re-stamp ===");

    aura::compiler::CompilerService cs;

    // AC1: query:per-fiber-stack-pool-stats hash fields
    {
        std::println("\n--- AC1: query:per-fiber-stack-pool-stats ---");
        auto stats = cs.eval("(query:per-fiber-stack-pool-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:per-fiber-stack-pool-stats returns hash");
        CHECK(stat_int(cs, "pool-hits") >= 0, "pool-hits present");
        CHECK(stat_int(cs, "lazy-allocs") >= 0, "lazy-allocs present");
        CHECK(stat_int(cs, "max-depth") >= 0, "max-depth present");
        CHECK(stat_int(cs, "churn-reductions") >= 0, "churn-reductions present");
        CHECK(stat_int(cs, "size-mismatches-caught") >= 0, "size-mismatches-caught present");
        CHECK(stat_int(cs, "growth-warnings") >= 0, "growth-warnings present");
        CHECK(stat_int(cs, "restamps") >= 0, "restamps present");
    }

    const auto pool_before = stat_int(cs, "pool-hits");
    const auto lazy_before = stat_int(cs, "lazy-allocs");

    // AC2: 2×100-fiber waves with scheduler teardown between waves so
    // ~Fiber returns vectors to the pool; wave-2 acquisitions reuse.
    {
        std::println("\n--- AC2: 200-fiber pool reuse (2 waves) ---");
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
        const auto lazy_after_wave0 = stat_int(cs, "lazy-allocs");
        const auto pool_after_wave0 = stat_int(cs, "pool-hits");
        run_wave(1);

        const auto pool_after = stat_int(cs, "pool-hits");
        const auto lazy_after = stat_int(cs, "lazy-allocs");
        const auto pool_delta = pool_after - pool_before;
        const auto lazy_delta = lazy_after - lazy_before;
        const auto wave2_lazy = lazy_after - lazy_after_wave0;
        const auto wave2_pool = pool_after - pool_after_wave0;
        const auto wave2_total = wave2_lazy + wave2_pool;
        const int wave2_reuse_pct =
            wave2_total > 0 ? static_cast<int>(100 * wave2_pool / wave2_total) : 0;
        std::println("  pool_delta={} lazy_delta={} wave2_lazy={} wave2_pool={} wave2_reuse={}% "
                     "max_depth={}",
                     pool_delta, lazy_delta, wave2_lazy, wave2_pool, wave2_reuse_pct,
                     stat_int(cs, "max-depth"));
        CHECK(lazy_delta <= 128,
              std::format("lazy allocs bounded ({} <= 128)", lazy_delta));
        CHECK(wave2_lazy <= 8,
              std::format("wave-2 lazy allocs minimal ({} <= 8)", wave2_lazy));
        CHECK(wave2_reuse_pct >= 80,
              std::format("wave-2 pool reuse >= 80% (got {}%)", wave2_reuse_pct));
        CHECK(stat_int(cs, "churn-reductions") >= pool_delta,
              "churn-reductions tracks pool hits");
    }

    // AC3: mixed steal/yield/panic cycles — metrics grow, max_depth bounded
    {
        std::println("\n--- AC3: steal/yield mixed cycles ---");
        const auto restamp_before = stat_int(cs, "restamps");
        Scheduler sched(8);
        std::atomic<int> done{0};
        constexpr int k_fibers = 32;
        for (int f = 0; f < k_fibers; ++f) {
            sched.spawn_with_affinity(
                [&, f]() {
                    std::mt19937 rng(static_cast<unsigned>(707u + f));
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
        CHECK(stat_int(cs, "max-depth") <= 64,
              std::format("max-depth capped (got {})", stat_int(cs, "max-depth")));
        const auto restamp_after = stat_int(cs, "restamps");
        CHECK(restamp_after >= restamp_before,
              std::format("restamps monotonic ({} -> {})", restamp_before, restamp_after));
    }

    // AC4: stats:count
    {
        std::println("\n--- AC4: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) == 80,
              "stats:count == 80");
    }

    // AC5: fiber stress — concurrent query under scheduler load
    {
        std::println("\n--- AC5: fiber stress ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 20;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(typecheck-current)");
                (void)cs.eval("(mutate:request-gc-safepoint)");
                auto r = cs.eval("(query:per-fiber-stack-pool-stats)");
                if (r && aura::compiler::types::is_hash(*r))
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() > 0,
              std::format("fiber stress produced {} successful pool-stats queries",
                          ok_count.load()));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}