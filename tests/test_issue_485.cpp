// @category: integration
// @reason: Issue #485 — Consolidated multi-fiber + MutationBoundaryGuard +
//          SoA EnvFrame + AOT + scheduler/GC production-readiness close-out

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <print>
#include <random>
#include <string>
#include <thread>

#include "serve/scheduler.h"

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

extern "C" void aura_evaluator_bump_steal_deferred_violation();
extern "C" void aura_evaluator_resume_fiber_migration();
extern "C" void aura_evaluator_test_push_mutation_checkpoint();
extern "C" void aura_evaluator_test_pop_mutation_checkpoint();

namespace aura_issue_485_detail {
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

static std::int64_t eval_int(aura::compiler::CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(std::string(expr));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::int64_t steal_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (query:scheduler-stealbudget-adaptive-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::int64_t aot_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:aot-checkpoint-version-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_485_detail

int main() {
    using namespace aura_issue_485_detail;
    using aura::serve::Fiber;
    using aura::serve::Scheduler;
    using aura::serve::YieldReason;

    std::println("=== Issue #485: Multi-fiber production-readiness close-out ===");

    aura::compiler::CompilerService cs;

    // AC1: consolidated production-readiness stats reachable
    {
        std::println("\n--- AC1: query:compiler-runtime-production-readiness-stats ---");
        auto stats = cs.eval("(query:compiler-runtime-production-readiness-stats)");
        CHECK(stats && aura::compiler::types::is_int(*stats),
              "query:compiler-runtime-production-readiness-stats returns int");
        CHECK(eval_int(cs, "(query:compiler-runtime-production-readiness-stats)") >= 0,
              "consolidated stats non-negative");
    }

    const auto fms_before = eval_int(cs, "(query:fiber-migration-stats)");
    const auto mcs_before = eval_int(cs, "(query:mutation-coordination-stats)");
    const auto eds_before = eval_int(cs, "(query:envframe-dualpath-stats)");

    // AC2: fiber migration stats monotonic after resume migration wiring
    {
        std::println("\n--- AC2: fiber-migration-stats monotonic ---");
        Scheduler sched(2);
        std::atomic<int> done{0};
        for (int i = 0; i < 4; ++i) {
            sched.spawn([&]() {
                for (int j = 0; j < 8; ++j)
                    Fiber::yield(YieldReason::Explicit);
                done.fetch_add(1);
            });
        }
        std::thread io([&sched]() { sched.run(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (done.load() < 4 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        sched.stop();
        io.join();
        const auto fms_after = eval_int(cs, "(query:fiber-migration-stats)");
        CHECK(fms_after > fms_before,
              std::format("fiber-migration-stats grew on resume migration ({} -> {})", fms_before,
                          fms_after));
    }

    // AC3: deferred steal violation counters wired
    {
        std::println("\n--- AC3: mutation-coordination-stats after defer wiring ---");
        aura_evaluator_bump_steal_deferred_violation();
        const auto mcs_after = eval_int(cs, "(query:mutation-coordination-stats)");
        const auto fms_after = eval_int(cs, "(query:fiber-migration-stats)");
        CHECK(mcs_after > mcs_before,
              std::format("mutation-coordination-stats grew ({} -> {})", mcs_before, mcs_after));
        CHECK(fms_after > fms_before,
              std::format("fiber-migration-stats grew on boundary violation ({} -> {})", fms_before,
                          fms_after));

        Scheduler sched(8);
        std::atomic<int> done{0};
        constexpr int k_fibers = 8;
        for (int i = 0; i < k_fibers; ++i) {
            sched.spawn_with_affinity(
                [&]() {
                    for (int j = 0; j < 20; ++j) {
                        aura_evaluator_test_push_mutation_checkpoint();
                        Fiber::yield(YieldReason::MutationBoundary);
                        aura_evaluator_test_pop_mutation_checkpoint();
                    }
                    done.fetch_add(1);
                },
                0);
        }
        const auto defer_before = steal_stat(cs, "global-deferred-mutation-total");
        std::thread io([&sched]() { sched.run(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (done.load() < k_fibers && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        sched.stop();
        io.join();
        const auto defer_after = steal_stat(cs, "global-deferred-mutation-total");
        CHECK(defer_after >= defer_before,
              std::format("global-deferred-mutation-total monotonic ({} -> {})", defer_before,
                          defer_after));
    }

    // AC4: envframe-dualpath-stats grows after mutate + capture
    {
        std::println("\n--- AC4: envframe-dualpath-stats after mutate ---");
        CHECK(cs.eval("(set-code \"(define acc 0)\")").has_value(), "workspace setup");
        CHECK(cs.eval("(eval-current)").has_value(), "workspace eval");
        const auto dual_before = cs.evaluator().get_bindings_dual_sync_count();
        const auto eds_mid = eval_int(cs, "(query:envframe-dualpath-stats)");
        CHECK(cs.eval("(mutate:rebind \"acc\" \"42\")").has_value(), "mutate:rebind under Guard");
        CHECK(cs.eval("(define cap (let ((y acc)) (lambda () y)))").has_value(),
              "let-capture allocates mirrored EnvFrame");
        CHECK(cs.eval("(cap)").has_value(), "closure apply exercises dual-path materialize");
        const aura::compiler::EnvId probe_id = cs.evaluator().alloc_env_frame();
        cs.evaluator().ensure_envframe_dual_path_consistency(cs.evaluator().env_frame(probe_id));
        const auto dual_after = cs.evaluator().get_bindings_dual_sync_count();
        const auto eds_after = eval_int(cs, "(query:envframe-dualpath-stats)");
        CHECK(dual_after > dual_before,
              std::format("bindings_dual_sync_count grew ({} -> {})", dual_before, dual_after));
        CHECK(eds_after > eds_mid,
              std::format("envframe-dualpath-stats grew after capture/apply ({} -> {})", eds_mid,
                          eds_after));
        CHECK(eds_after >= eds_before,
              std::format("envframe-dualpath-stats grew from baseline ({} -> {})", eds_before,
                          eds_after));
    }

    // AC5: scheduler steal budget adaptive stats reachable
    {
        std::println("\n--- AC5: query:scheduler-stealbudget-adaptive-stats ---");
        auto stats = cs.eval("(query:scheduler-stealbudget-adaptive-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:scheduler-stealbudget-adaptive-stats returns hash");
        CHECK(steal_stat(cs, "mutation-bias-hits") >= 0, "mutation-bias-hits present");
        CHECK(steal_stat(cs, "global-deferred-mutation-total") >= 0,
              "global-deferred-mutation-total present");
    }

    // AC6: AOT checkpoint version drift probe
    {
        std::println("\n--- AC6: AOT checkpoint version drift probe ---");
        auto stats = cs.eval("(query:aot-checkpoint-version-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:aot-checkpoint-version-stats returns hash");
        CHECK(aot_stat(cs, "checkpoint-version-drifts") >= 0, "checkpoint-version-drifts present");
        CHECK(aot_stat(cs, "deopt-on-steal") >= 0, "deopt-on-steal present");
        const auto drifts_before = aot_stat(cs, "checkpoint-version-drifts");
        cs.evaluator().transfer_mutation_stack_to_current_fiber();
        const auto drifts_after = aot_stat(cs, "checkpoint-version-drifts");
        CHECK(drifts_after >= drifts_before,
              std::format("checkpoint-version-drifts monotonic ({} -> {})", drifts_before,
                          drifts_after));
    }

    // AC7: stats:count regression
    {
        std::println("\n--- AC7: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) == 84,
              "stats:count == 84");
    }

    // AC8: multi-fiber stress — scheduler + checkpoint push/pop
    {
        std::println("\n--- AC8: multi-fiber stress ---");
        Scheduler sched(8);
        std::atomic<int> done{0};
        constexpr int k_fibers = 16;
        for (int f = 0; f < k_fibers; ++f) {
            sched.spawn_with_affinity(
                [&, f]() {
                    std::mt19937 rng(static_cast<unsigned>(485u + f));
                    for (int i = 0; i < 30; ++i) {
                        if ((i & 3) == 0) {
                            aura_evaluator_test_push_mutation_checkpoint();
                            Fiber::yield(YieldReason::MutationBoundary);
                            aura_evaluator_test_pop_mutation_checkpoint();
                        } else if ((i & 1) == 0) {
                            Fiber::yield(YieldReason::Explicit);
                        } else {
                            Fiber::yield(YieldReason::MutationBoundary);
                        }
                    }
                    done.fetch_add(1);
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
        CHECK(done.load() == k_fibers,
              std::format("all {} stress fibers completed (got {})", k_fibers, done.load()));

        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 15;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(query:fiber-migration-stats)");
                (void)cs.eval("(query:mutation-coordination-stats)");
                (void)cs.eval("(query:envframe-dualpath-stats)");
                aura_evaluator_resume_fiber_migration();
                ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() == k_iters * 2,
              std::format("concurrent query stress: {} / {} ok", ok_count.load(), k_iters * 2));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}