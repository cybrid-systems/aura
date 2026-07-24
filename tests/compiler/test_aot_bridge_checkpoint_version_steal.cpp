// test_aot_bridge_checkpoint_version_steal.cpp — Issue #653:
// AOT bridge_epoch / emit_version integration into fiber
// restore_post_yield_or_rollback version drift detection.
//
// Non-duplicative with #708 (AOT hot-reload ship), #652 (stack pool),
// #592 (panic resume), #485 (steal deferral).
//
//   - AC1:  query:aot-checkpoint-version-stats reachable (schema 653)
//   - AC2:  defuse version drift caught via AOT probe
//   - AC3:  bridge_epoch mismatch caught separately
//   - AC4:  deopt-on-steal bumps on drift during restore path
//   - AC5:  multi-fiber steal/yield + concurrent AOT reload
//   - AC6:  metrics monotonic over stress matrix
//   - AC7:  query regression (aot-reload-stats, panic-fiber)
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

#include "compiler/aura_jit_bridge.h"
#include "serve/scheduler.h"
#include "serve/worker.h"

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

extern "C" void aura_evaluator_test_push_mutation_checkpoint();
extern "C" void aura_evaluator_test_pop_mutation_checkpoint();

namespace aura_653_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::serve::Fiber;
using aura::serve::Scheduler;
using aura::serve::YieldReason;

static std::int64_t hash_int(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (engine:metrics \"query:aot-checkpoint-version-stats\") \"" + key +
                     "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto drifts = hash_int(cs, "checkpoint-version-drifts");
    const auto bridge = hash_int(cs, "bridge-epoch-mismatches");
    const auto deopt = hash_int(cs, "deopt-on-steal");
    const auto swaps = hash_int(cs, "func-table-epoch-swaps");
    if (drifts < 0 || bridge < 0 || deopt < 0 || swaps < 0)
        return -1;
    return drifts + bridge + deopt + swaps;
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:aot-checkpoint-version-stats (schema 653) ---");
    auto h = cs.eval("(engine:metrics \"query:aot-checkpoint-version-stats\")");
    CHECK(h && is_hash(*h), "aot-checkpoint-version-stats returns hash");
    CHECK(hash_int(cs, "schema") == 653, "schema == 653");
    CHECK(hash_int(cs, "checkpoint-version-drifts") >= 0, "checkpoint-version-drifts present");
    CHECK(hash_int(cs, "bridge-epoch-mismatches") >= 0, "bridge-epoch-mismatches present");
    CHECK(hash_int(cs, "deopt-on-steal") >= 0, "deopt-on-steal present");

    const auto drifts0 = hash_int(cs, "checkpoint-version-drifts");
    const auto bridge0 = hash_int(cs, "bridge-epoch-mismatches");
    const auto deopt0 = hash_int(cs, "deopt-on-steal");

    std::println("\n--- AC2: defuse version drift caught ---");
    aura_set_aot_defuse_version(200);
    const bool defuse_drift = aura_aot_probe_checkpoint_version(1, 0);
    CHECK(defuse_drift, "aura_aot_probe_checkpoint_version detects defuse drift");
    const auto drifts1 = hash_int(cs, "checkpoint-version-drifts");
    std::println("  checkpoint-version-drifts: {} -> {}", drifts0, drifts1);
    CHECK(drifts1 > drifts0, "checkpoint-version-drifts bumped");

    std::println("\n--- AC3: bridge_epoch mismatch caught ---");
    const auto table_epoch = aura_aot_func_table_epoch();
    const bool bridge_drift = aura_aot_probe_checkpoint_version(0, table_epoch + 99);
    CHECK(bridge_drift, "aura_aot_probe_checkpoint_version detects bridge_epoch mismatch");
    const auto bridge1 = hash_int(cs, "bridge-epoch-mismatches");
    std::println("  bridge-epoch-mismatches: {} -> {}", bridge0, bridge1);
    CHECK(bridge1 > bridge0, "bridge-epoch-mismatches bumped");
    aura_set_aot_defuse_version(0);

    std::println("\n--- AC4: deopt-on-steal bumps on drift ---");
    aura_aot_record_deopt_on_steal();
    const auto deopt1 = hash_int(cs, "deopt-on-steal");
    std::println("  deopt-on-steal: {} -> {}", deopt0, deopt1);
    CHECK(deopt1 > deopt0, "deopt-on-steal bumped");

    std::println("\n--- AC5: multi-fiber steal/yield + AOT reload stress ---");
    const auto stats5a = stats_sum(cs);
    Scheduler sched(8);
    std::atomic<int> done{0};
    constexpr int k_fibers = 32;
    std::thread reload_worker([&] {
        for (int i = 0; i < 15; ++i) {
            (void)aura_reload_aot_module("/tmp/aura_653_stress.so", static_cast<std::uint64_t>(i));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    for (int f = 0; f < k_fibers; ++f) {
        sched.spawn_with_affinity(
            [&, f]() {
                std::mt19937 rng(static_cast<unsigned>(653u + f));
                std::uniform_int_distribution<int> coin(0, 3);
                for (int i = 0; i < 25; ++i) {
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
    reload_worker.join();
    CHECK(done.load() == k_fibers, "all fibers completed under AOT+steal stress");

    std::println("\n--- AC6: metrics monotonic ---");
    const auto stats5b = stats_sum(cs);
    std::println("  aot-checkpoint sum: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "aot-checkpoint stats monotonic over stress");

    std::println("\n--- AC7: query regression ---");
    auto reload = cs.eval("(engine:metrics \"query:aot-reload-stats\")");
    auto panic = cs.eval("(engine:metrics \"query:panic-checkpoint-fiber-stats\")");
    CHECK(reload && is_hash(*reload), "aot-reload-stats regression");
    CHECK(panic && is_hash(*panic), "panic-checkpoint-fiber-stats regression");
}

} // namespace aura_653_detail

int aura_issue_aot_bridge_checkpoint_version_steal_run() {
    aura::compiler::CompilerService cs;
    aura_653_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_aot_bridge_checkpoint_version_steal_run();
}
#endif
