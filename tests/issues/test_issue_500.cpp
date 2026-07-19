// @category: integration
// @reason: Issue #500 — Work-stealing + MutationBoundary outermost depth safety

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <thread>

#include "serve/scheduler.h"
#include "serve/worker.h"

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

extern "C" std::size_t aura_evaluator_mutation_boundary_depth();
extern "C" void aura_evaluator_test_push_mutation_checkpoint();
extern "C" void aura_evaluator_test_pop_mutation_checkpoint();

namespace aura_issue_500_detail {
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

static std::int64_t snap_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:work-steal-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_500_detail

int aura_issue_500_run() {
    using namespace aura_issue_500_detail;
    using aura::serve::Fiber;
    using aura::serve::Scheduler;
    using aura::serve::YieldReason;

    std::println("=== Issue #500: Work-stealing + MutationBoundary safety ===");

    aura::compiler::CompilerService cs;

    // AC1: query:work-steal-stats fields
    {
        std::println("\n--- AC1: query:work-steal-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:work-steal-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:work-steal-stats returns hash");
        CHECK(snap_stat(cs, "steal-attempts") >= 0, "steal-attempts present");
        CHECK(snap_stat(cs, "steal-successes") >= 0, "steal-successes present");
        CHECK(snap_stat(cs, "steal-deferred-mutation") >= 0, "steal-deferred-mutation present");
        CHECK(snap_stat(cs, "mutation-boundary-depth") >= 0, "mutation-boundary-depth present");
        CHECK(snap_stat(cs, "fiber-migration-attempts") >= 0, "fiber-migration-attempts present");
        CHECK(snap_stat(cs, "ring-steal-attempts") >= 0, "ring-steal-attempts present");
        CHECK(snap_stat(cs, "work-steal-total") >= 0, "work-steal-total present");
        CHECK(aura_evaluator_mutation_boundary_depth() == 0,
              "mutation_boundary_depth == 0 with no active guard");
    }

    const auto mig_before = snap_stat(cs, "fiber-migration-attempts");
    const auto ring_before = snap_stat(cs, "ring-steal-attempts");

    // AC2: concurrent steal under mixed MutationBoundary yields
    {
        std::println("\n--- AC2: concurrent work-stealing stress ---");
        Scheduler sched(8);
        std::atomic<int> done{0};
        std::atomic<std::uint64_t> deferred_total{0};
        std::atomic<std::uint64_t> success_total{0};
        std::mutex mtx;
        constexpr int k_fibers = 16;
        for (int i = 0; i < k_fibers; ++i) {
            sched.spawn_with_affinity(
                [&, i]() {
                    for (int j = 0; j < 20; ++j) {
                        if ((i + j) % 3 == 0) {
                            aura_evaluator_test_push_mutation_checkpoint();
                            Fiber::yield(YieldReason::MutationBoundary);
                            aura_evaluator_test_pop_mutation_checkpoint();
                        } else {
                            Fiber::yield(YieldReason::MutationBoundary);
                        }
                    }
                    if (aura::serve::g_current_fiber) {
                        std::lock_guard lock(mtx);
                        deferred_total.fetch_add(
                            aura::serve::g_current_fiber->steal_deferred_mutation_boundary_count());
                        success_total.fetch_add(
                            aura::serve::g_current_fiber->steal_success_count());
                    }
                    done.fetch_add(1);
                },
                i % 2);
        }
        std::thread io([&sched]() { sched.run(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
        while (done.load() < k_fibers && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        sched.stop();
        io.join();
        CHECK(done.load() == k_fibers,
              std::format("all {} fibers completed (got {})", k_fibers, done.load()));
        CHECK(snap_stat(cs, "fiber-migration-attempts") > mig_before,
              std::format("fiber-migration-attempts grew ({} -> {})", mig_before,
                          snap_stat(cs, "fiber-migration-attempts")));
    }

    // AC3: outermost MutationBoundary is steal-safe predicate
    {
        std::println("\n--- AC3: outermost MutationBoundary steal-safe ---");
        Scheduler sched(2);
        std::atomic<bool> checked{false};
        sched.spawn([&]() {
            if (aura::serve::g_current_fiber) {
                aura::serve::g_current_fiber->set_yield_reason(YieldReason::MutationBoundary);
                CHECK(aura::serve::g_current_fiber->is_at_mutation_boundary_safe(),
                      "outermost MutationBoundary depth==0 is steal-safe");
                checked.store(true);
            }
            Fiber::yield(YieldReason::Explicit);
        });
        std::thread io([&sched]() { sched.run(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!checked.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        sched.stop();
        io.join();
        CHECK(checked.load(), "outermost steal-safe predicate exercised");
    }

    // AC4: ring-neighbor steal attempts recorded
    {
        std::println("\n--- AC4: ring-neighbor steal metrics ---");
        CHECK(snap_stat(cs, "ring-steal-attempts") >= ring_before,
              std::format("ring-steal-attempts monotonic ({} -> {})", ring_before,
                          snap_stat(cs, "ring-steal-attempts")));
    }

    // AC5: scheduler-stealbudget + fiber-migration regression
    {
        std::println("\n--- AC5: related stats regression ---");
        auto stealbudget =
            cs.eval("(engine:metrics \"query:scheduler-stealbudget-adaptive-stats\")");
        CHECK(stealbudget && aura::compiler::types::is_hash(*stealbudget),
              "scheduler-stealbudget-adaptive-stats regression");
        auto fms = cs.eval("(engine:metrics \"query:fiber-migration-stats\")");
        CHECK(fms && aura::compiler::types::is_int(*fms), "fiber-migration-stats regression");
    }

    // AC6: stats:count
    {
        std::println("\n--- AC6: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 97,
              "stats:count >= 97");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_500_run();
}
#endif
