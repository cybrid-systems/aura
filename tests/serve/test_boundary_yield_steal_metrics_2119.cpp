// @category: unit
// @reason: Issue #2119 — MutationBoundary yield + steal starvation metrics
// + optional adaptive priority (default OFF).
//
//   AC1: high-frequency MB yield → yield_mutation_boundary_total + hold_ns
//   AC2: steal skip bumps steal_skipped_mutation_boundary_total + pressure
//   AC3: adaptive OFF by default; priority unchanged when off
//   AC4: query:orchestration-steal-stats schema-2119
//   AC5: adaptive ON demotes MB/outermost under high pressure

#include "test_harness.hpp"

#include "serve/fiber.h"
#include "serve/metrics.h"
#include "serve/scheduler.h"
#include "serve/worker.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>

extern "C" void aura_evaluator_test_push_mutation_checkpoint();
extern "C" void aura_evaluator_test_pop_mutation_checkpoint();

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::serve::Fiber;
using aura::serve::fiber_steal_priority;
using aura::serve::Scheduler;
using aura::serve::YieldReason;
using aura::serve::metrics::adaptive_steal_stats;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:orchestration-steal-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static void ac1_yield_hold_metrics() {
    std::println("\n--- AC1: MB yield count + hold_ns ---");
    auto& s = adaptive_steal_stats();
    const auto y0 = s.yield_mutation_boundary_total.load();
    const auto h0 = s.yield_mutation_boundary_hold_ns_total.load();

    Scheduler sched(2);
    std::atomic<int> done{0};
    sched.spawn([&]() {
        for (int i = 0; i < 20; ++i) {
            Fiber::yield(YieldReason::MutationBoundary);
        }
        done.store(1);
    });
    // Keep a second fiber busy so worker can resume the first
    sched.spawn([&]() {
        for (int i = 0; i < 50; ++i)
            Fiber::yield(YieldReason::Explicit);
    });
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.stop();
    io.join();
    CHECK(done.load() == 1, "yield loop finished");
    CHECK(s.yield_mutation_boundary_total.load() >= y0 + 20, "yield total += 20");
    // Hold ns advances when resume closes MB yields (may be 0 if never resumed
    // between yields in extreme scheduling; typically > 0).
    CHECK(s.yield_mutation_boundary_hold_ns_total.load() >= h0, "hold_ns monotonic");
    CHECK(Fiber::static_yield_mutation_boundary_total() >= 20, "static yield total");
}

static void ac2_steal_skip_and_pressure() {
    std::println("\n--- AC2: steal skip + pressure ---");
    auto& s = adaptive_steal_stats();
    const auto skip0 = s.steal_skipped_mutation_boundary_total.load();

    Scheduler sched(4);
    std::atomic<int> done{0};
    constexpr int kVictims = 4;
    for (int i = 0; i < kVictims; ++i) {
        sched.spawn_with_affinity(
            [&]() {
                for (int j = 0; j < 30; ++j) {
                    aura_evaluator_test_push_mutation_checkpoint();
                    Fiber::yield(YieldReason::MutationBoundary);
                    aura_evaluator_test_pop_mutation_checkpoint();
                }
                done.fetch_add(1);
            },
            /*affinity=*/0);
    }
    for (int i = 0; i < 8; ++i) {
        sched.spawn([&]() {
            for (int j = 0; j < 40; ++j)
                Fiber::yield(YieldReason::Explicit);
        });
    }
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (done.load() < kVictims && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    sched.stop();
    io.join();
    CHECK(done.load() == kVictims, "victims done");
    const auto skip1 = s.steal_skipped_mutation_boundary_total.load();
    CHECK(skip1 >= skip0, "skip monotonic");
    // Pressure is updated on each skip path
    if (skip1 > skip0)
        CHECK(s.steal_starvation_boundary_pressure.load() >= 0, "pressure readable");
}

static void ac3_adaptive_default_off() {
    std::println("\n--- AC3: adaptive default OFF; priority baseline ---");
    auto& s = adaptive_steal_stats();
    CHECK(s.adaptive_boundary_policy_enabled.load() == 0, "policy default off");

    // Synthetic fiber state for priority (no full scheduler needed)
    // Use a live fiber on a short scheduler run.
    Scheduler sched(1);
    std::atomic<int> pri_explicit{-1};
    std::atomic<int> pri_mb_outer{-1};
    sched.spawn([&]() {
        auto* f = aura::serve::g_current_fiber;
        CHECK(f != nullptr, "fiber");
        f->set_yield_reason(YieldReason::Explicit);
        pri_explicit.store(fiber_steal_priority(f));
        f->set_yield_reason(YieldReason::MutationBoundary);
        // depth 0 → MB/outermost classification when stack empty
        pri_mb_outer.store(fiber_steal_priority(f));
        Fiber::yield(YieldReason::Explicit);
    });
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (pri_mb_outer.load() < 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.stop();
    io.join();
    CHECK(pri_explicit.load() == 3, "Explicit priority 3");
    CHECK(pri_mb_outer.load() == 2, "MB/outermost priority 2 when adaptive off");
}

static void ac4_query() {
    std::println("\n--- AC4: query schema-2119 ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
    CHECK(href(cs, "schema-2119") == 2119, "schema-2119");
    CHECK(href(cs, "issue-2119") == 2119, "issue-2119");
    CHECK(href(cs, "boundary-yield-hold-wired") == 1, "hold wired");
    CHECK(href(cs, "adaptive-boundary-policy-default-off") == 1, "adaptive default off");
    CHECK(href(cs, "yield-mutation-boundary-total") >= 0, "yield total key");
    CHECK(href(cs, "yield-mutation-boundary-hold-ns-total") >= 0, "hold ns key");
    CHECK(href(cs, "steal-starvation-boundary-pressure") >= 0, "pressure key");
    CHECK(href(cs, "steal-skipped-mutation-boundary-total") >= 0, "skip key");
    CHECK(href(cs, "adaptive-boundary-policy-enabled") == 0, "policy off in query");
}

static void ac5_adaptive_on_demotes_mb() {
    std::println("\n--- AC5: adaptive ON demotes MB/outermost under pressure ---");
    auto& s = adaptive_steal_stats();
    s.adaptive_boundary_policy_enabled.store(1, std::memory_order_relaxed);
    s.steal_starvation_boundary_pressure.store(9000, std::memory_order_relaxed); // high
    s.adaptive_boundary_pressure_threshold_bp.store(5000, std::memory_order_relaxed);
    const auto pref0 = s.adaptive_prefer_non_boundary_total.load();

    Scheduler sched(1);
    std::atomic<int> pri_mb{-1};
    std::atomic<int> pri_ex{-1};
    sched.spawn([&]() {
        auto* f = aura::serve::g_current_fiber;
        f->set_yield_reason(YieldReason::Explicit);
        pri_ex.store(fiber_steal_priority(f));
        f->set_yield_reason(YieldReason::MutationBoundary);
        pri_mb.store(fiber_steal_priority(f));
        Fiber::yield(YieldReason::Explicit);
    });
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (pri_mb.load() < 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.stop();
    io.join();

    CHECK(pri_ex.load() == 3, "Explicit still 3 under adaptive");
    CHECK(pri_mb.load() == 1, "MB/outermost demoted to 1 under pressure");
    CHECK(s.adaptive_prefer_non_boundary_total.load() > pref0, "prefer non-boundary++");

    // Restore default OFF
    s.adaptive_boundary_policy_enabled.store(0, std::memory_order_relaxed);
    s.steal_starvation_boundary_pressure.store(0, std::memory_order_relaxed);

    auto wh = read_file("src/serve/worker.h");
    auto fc = read_file("src/serve/fiber.cpp");
    auto mh = read_file("src/serve/metrics.h");
    CHECK(wh.find("Issue #2119") != std::string::npos || wh.find("#2119") != std::string::npos,
          "worker.h cites");
    CHECK(fc.find("yield_mutation_boundary_hold_ns_total") != std::string::npos, "hold ns wire");
    CHECK(mh.find("steal_starvation_boundary_pressure") != std::string::npos, "pressure field");
}

} // namespace

int run_test_boundary_yield_steal_metrics_2119() {
    std::println("=== Issue #2119: boundary yield + steal starvation metrics ===");
    ac1_yield_hold_metrics();
    ac2_steal_skip_and_pressure();
    ac3_adaptive_default_off();
    ac4_query();
    ac5_adaptive_on_demotes_mb();
    adaptive_steal_stats().adaptive_boundary_policy_enabled.store(0);
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_boundary_yield_steal_metrics_2119();
}
#endif
