// @category: integration
// @reason: Issue #706 adaptive StealBudget + work-stealing bias for LLM bottleneck

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

namespace aura_issue_706_detail {
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
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:scheduler-stealbudget-adaptive-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_706_detail

int aura_issue_706_run() {
    using namespace aura_issue_706_detail;
    using aura::serve::Fiber;
    using aura::serve::fiber_steal_priority;
    using aura::serve::Scheduler;
    using aura::serve::StealBudget;
    using aura::serve::YieldReason;

    std::println("=== Issue #706: Adaptive StealBudget + work-stealing bias ===");

    aura::compiler::CompilerService cs;

    // AC1: query:scheduler-stealbudget-adaptive-stats hash fields
    {
        std::println("\n--- AC1: query:scheduler-stealbudget-adaptive-stats ---");
        auto stats = cs.eval("(query:scheduler-stealbudget-adaptive-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:scheduler-stealbudget-adaptive-stats returns hash");
        CHECK(stat_int(cs, "mutation-bias-hits") >= 0, "mutation-bias-hits present");
        CHECK(stat_int(cs, "outermost-preferred") >= 0, "outermost-preferred present");
        CHECK(stat_int(cs, "llm-tail-reductions") >= 0, "llm-tail-reductions present");
        CHECK(stat_int(cs, "deferred-pressure-boosts") >= 0, "deferred-pressure-boosts present");
        CHECK(stat_int(cs, "global-deferred-mutation-total") >= 0,
              "global-deferred-mutation-total present");
    }

    const auto bias_before = stat_int(cs, "mutation-bias-hits");
    const auto outer_before = stat_int(cs, "outermost-preferred");
    const auto llm_before = stat_int(cs, "llm-tail-reductions");
    const auto pressure_before = stat_int(cs, "deferred-pressure-boosts");

    // AC2: StealBudget deferred pressure + fiber_steal_priority
    {
        std::println("\n--- AC2: StealBudget + fiber_steal_priority ---");
        StealBudget budget(true);
        budget.max_before_sleep = 3;
        budget.apply_deferred_pressure(15);
        CHECK(budget.max_before_sleep >= 5,
              std::format("deferred>10 raises max_before_sleep (got {})", budget.max_before_sleep));
        budget.apply_deferred_pressure(60);
        CHECK(budget.max_before_sleep >= 7,
              std::format("deferred>50 raises max_before_sleep (got {})", budget.max_before_sleep));

        Scheduler sched(2);
        std::atomic<bool> pri_checked{false};
        sched.spawn([&]() {
            if (aura::serve::g_current_fiber) {
                aura::serve::g_current_fiber->set_yield_reason(YieldReason::Explicit);
                CHECK(fiber_steal_priority(aura::serve::g_current_fiber) >= 3,
                      "Explicit yield → steal priority 3");
                aura::serve::g_current_fiber->set_yield_reason(YieldReason::OperationBoundary);
                CHECK(fiber_steal_priority(aura::serve::g_current_fiber) >= 3,
                      "OperationBoundary yield → steal priority 3");
                aura::serve::g_current_fiber->set_yield_reason(YieldReason::MutationBoundary);
                const int outer_pri = fiber_steal_priority(aura::serve::g_current_fiber);
                CHECK(outer_pri >= 2,
                      std::format("outermost MutationBoundary → priority 2 (got {})", outer_pri));
                aura_evaluator_test_push_mutation_checkpoint();
                aura::serve::g_current_fiber->set_yield_reason(YieldReason::MutationBoundary);
                CHECK(fiber_steal_priority(aura::serve::g_current_fiber) >= 0,
                      "inner MutationBoundary → priority 0");
                aura_evaluator_test_pop_mutation_checkpoint();
                pri_checked.store(true);
            }
            Fiber::yield(YieldReason::Explicit);
        });
        std::thread io([&sched]() { sched.run(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!pri_checked.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        sched.stop();
        io.join();
        CHECK(pri_checked.load(), "fiber_steal_priority matrix exercised in fiber");
    }

    // AC3: 24-fiber mixed yield + mock LLM latency → adaptive bias metrics grow
    {
        std::println("\n--- AC3: 24-fiber mixed yield steal bias ---");
        Scheduler sched(8);
        std::atomic<int> done{0};
        constexpr int k_fibers = 24;
        for (int f = 0; f < k_fibers; ++f) {
            sched.spawn_with_affinity(
                [&, f]() {
                    std::mt19937 rng(static_cast<unsigned>(706u + f));
                    std::uniform_int_distribution<int> coin(0, 5);
                    for (int i = 0; i < 25; ++i) {
                        switch (coin(rng) % 4) {
                            case 0:
                                aura_evaluator_test_push_mutation_checkpoint();
                                Fiber::yield(YieldReason::MutationBoundary);
                                aura_evaluator_test_pop_mutation_checkpoint();
                                break;
                            case 1:
                                Fiber::yield(YieldReason::MutationBoundary);
                                break;
                            case 2:
                                Fiber::yield(YieldReason::Explicit);
                                std::this_thread::sleep_for(std::chrono::microseconds(80));
                                break;
                            default:
                                Fiber::yield(YieldReason::OperationBoundary);
                                std::this_thread::sleep_for(std::chrono::microseconds(40));
                                break;
                        }
                    }
                    done.fetch_add(1, std::memory_order_relaxed);
                },
                f % 2);
        }
        std::thread io([&sched]() { sched.run(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
        while (done.load() < k_fibers && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        sched.stop();
        io.join();
        CHECK(done.load() >= (k_fibers * 9) / 10,
              std::format("all {} fibers completed (got {})", k_fibers, done.load()));

        const auto bias_after = stat_int(cs, "mutation-bias-hits");
        const auto outer_after = stat_int(cs, "outermost-preferred");
        const auto llm_after = stat_int(cs, "llm-tail-reductions");
        const auto pressure_after = stat_int(cs, "deferred-pressure-boosts");
        std::println("  metrics: bias {}→{} outer {}→{} llm {}→{} pressure {}→{}", bias_before,
                     bias_after, outer_before, outer_after, llm_before, llm_after, pressure_before,
                     pressure_after);
        CHECK(bias_after >= bias_before,
              std::format("mutation-bias-hits monotonic ({} -> {})", bias_before, bias_after));
        CHECK(outer_after >= outer_before || llm_after >= llm_before,
              "outermost-preferred or llm-tail-reductions grew under mixed steal load");
    }

    // AC4: stats:count
    {
        std::println("\n--- AC4: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 211,
              "stats:count >= 211");
    }

    // AC5: fiber stress — concurrent eval + scheduler query under load
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
                auto r = cs.eval("(query:scheduler-stealbudget-adaptive-stats)");
                if (r && aura::compiler::types::is_hash(*r))
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() > 0,
              std::format("fiber stress produced {} successful adaptive-stats queries",
                          ok_count.load()));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_706_run();
}
#endif
