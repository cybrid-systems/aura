// @category: integration
// @reason: Issue #708 AOT hot-reload refcount swap + region/panic multi-fiber safety

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <print>
#include <random>
#include <string>
#include <thread>

#include "aura_jit_bridge.h"
#include "serve/scheduler.h"
#include "serve/worker.h"

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

extern "C" void aura_evaluator_test_push_mutation_checkpoint();
extern "C" void aura_evaluator_test_pop_mutation_checkpoint();

namespace aura_issue_708_detail {
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

static std::int64_t reload_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:aot-reload-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::int64_t checkpoint_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:aot-checkpoint-version-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_708_detail

int main() {
    using namespace aura_issue_708_detail;
    using aura::serve::Fiber;
    using aura::serve::Scheduler;
    using aura::serve::YieldReason;

    std::println("=== Issue #708: AOT hot-reload refcount + region/panic safety ===");

    aura::compiler::CompilerService cs;

    // AC1: query:aot-reload-stats hash fields
    {
        std::println("\n--- AC1: query:aot-reload-stats ---");
        auto stats = cs.eval("(query:aot-reload-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:aot-reload-stats returns hash");
        CHECK(reload_stat(cs, "reload-attempts") >= 0, "reload-attempts present");
        CHECK(reload_stat(cs, "reload-success") >= 0, "reload-success present");
        CHECK(reload_stat(cs, "stale-rejected") >= 0, "stale-rejected present");
        CHECK(reload_stat(cs, "refcount-swaps") >= 0, "refcount-swaps present");
        CHECK(reload_stat(cs, "region-violations") >= 0, "region-violations present");
        CHECK(reload_stat(cs, "deopt-on-steal") >= 0, "deopt-on-steal present");
        CHECK(reload_stat(cs, "concurrent-safe-reloads") >= 0, "concurrent-safe-reloads present");
    }

    // AC2: query:aot-checkpoint-version-stats hash fields
    {
        std::println("\n--- AC2: query:aot-checkpoint-version-stats ---");
        auto stats = cs.eval("(query:aot-checkpoint-version-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:aot-checkpoint-version-stats returns hash");
        CHECK(checkpoint_stat(cs, "checkpoint-version-drifts") >= 0,
              "checkpoint-version-drifts present");
        CHECK(checkpoint_stat(cs, "deopt-on-steal") >= 0, "deopt-on-steal present");
        CHECK(checkpoint_stat(cs, "func-table-epoch-swaps") >= 0, "func-table-epoch-swaps present");
    }

    const auto attempts_before = reload_stat(cs, "reload-attempts");
    const auto drifts_before = checkpoint_stat(cs, "checkpoint-version-drifts");

    // AC3: reload attempts bump + stale reject on bad paths
    {
        std::println("\n--- AC3: reload attempt + stale reject ---");
        (void)aura_reload_aot_module(nullptr, 0);
        (void)aura_reload_aot_module("/tmp/aura_708_no_such_module.so", 99);
        const auto attempts_after = reload_stat(cs, "reload-attempts");
        CHECK(attempts_after >= attempts_before + 2,
              std::format("reload-attempts grew ({} -> {})", attempts_before, attempts_after));

        aura_set_aot_defuse_version(10);
        aura_set_aot_emit_region_mask(0xA);
        aura_set_aot_region_mask(0xB);
        (void)aura_reload_aot_module("/tmp/aura_708_no_such_module.so", 10);
        const auto region_viol = reload_stat(cs, "region-violations");
        CHECK(region_viol >= 0, "region-violations readable after region mask set");
        aura_set_aot_region_mask(0);
        aura_set_aot_defuse_version(0);
    }

    // AC4: checkpoint version drift probe
    {
        std::println("\n--- AC4: checkpoint version drift probe ---");
        aura_set_aot_defuse_version(100);
        const bool drift = aura_aot_probe_checkpoint_version(1, 0);
        CHECK(drift, "aura_aot_probe_checkpoint_version detects defuse drift");
        const auto drifts_after = checkpoint_stat(cs, "checkpoint-version-drifts");
        CHECK(drifts_after > drifts_before, std::format("checkpoint-version-drifts grew ({} -> {})",
                                                        drifts_before, drifts_after));
        aura_aot_record_deopt_on_steal();
        CHECK(reload_stat(cs, "deopt-on-steal") >= 1, "deopt-on-steal bumped");
        aura_set_aot_defuse_version(0);
    }

    // AC5: multi-fiber steal/yield during concurrent reload attempts
    {
        std::println("\n--- AC5: multi-fiber + concurrent reload attempts ---");
        Scheduler sched(8);
        std::atomic<int> done{0};
        std::atomic<int> reload_ok{0};
        constexpr int k_fibers = 32;
        std::thread reload_worker([&] {
            for (int i = 0; i < 20; ++i) {
                if (aura_reload_aot_module("/tmp/aura_708_stress.so",
                                           static_cast<std::uint64_t>(i)))
                    reload_ok.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        for (int f = 0; f < k_fibers; ++f) {
            sched.spawn_with_affinity(
                [&, f]() {
                    std::mt19937 rng(static_cast<unsigned>(708u + f));
                    for (int i = 0; i < 25; ++i) {
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
        reload_worker.join();
        CHECK(done.load() == k_fibers,
              std::format("all {} fibers completed (got {})", k_fibers, done.load()));
        CHECK(reload_stat(cs, "reload-attempts") > attempts_before,
              "reload-attempts grew under concurrent fiber load");
        CHECK(reload_ok.load() == 0,
              "no false-positive reload success on missing module under stress");
    }

    // AC6: stats:count
    {
        std::println("\n--- AC6: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) == 84,
              "stats:count == 84");
    }

    // AC7: fiber stress — concurrent eval + AOT query primitives
    {
        std::println("\n--- AC7: fiber stress ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 20;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(typecheck-current)");
                (void)cs.eval("(mutate:request-gc-safepoint)");
                auto r1 = cs.eval("(query:aot-reload-stats)");
                auto r2 = cs.eval("(query:aot-checkpoint-version-stats)");
                if (r1 && aura::compiler::types::is_hash(*r1) && r2 &&
                    aura::compiler::types::is_hash(*r2)) {
                    ok_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(
            ok_count.load() > 0,
            std::format("fiber stress produced {} successful AOT stats queries", ok_count.load()));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}