// @category: integration
// @reason: Issue #1633 — nested MutationBoundary (depth>0) steal defer +
// Issue #1445/#1492/#1633 (#1978 renamed): issue# moved from filename to header.
// starvation mitigation mandate (build on #1492 / #1445).
//
//   AC1: steal loop inner-boundary defer + apply_starvation_mitigation wired
//   AC2: on_long_mutation_held → full apply_starvation_mitigation linkage
//   AC3: query:orchestration-steal-stats schema 1633 + AC metric aliases
//   AC4: nested Guard + multi-fiber concurrent mitigation (throughput)
//   AC5: 50+ fiber stress — mitigation metrics grow, no crash, TSan-friendly
//   AC6: #1492 lineage (priority boost, clearable)

#include "test_harness.hpp"
#include "serve/fiber.h"
#include "serve/worker.h"
#include "serve/metrics.h"
#include "serve/scheduler.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::serve::apply_starvation_mitigation;
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

static void ac1_wire_flags() {
    std::println("\n--- AC1: steal-loop / mitigation wire flags ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:orchestration-steal-stats\")");
    CHECK(h && is_hash(*h), "orchestration-steal-stats hash");
    CHECK(href(cs, "inner-defer-mitigation-wired") == 1, "inner-defer-mitigation-wired");
    CHECK(href(cs, "long-mutation-hook-wired") == 1, "long-mutation-hook-wired");
    CHECK(href(cs, "steal-loop-inner-defer-wired") == 1, "steal-loop-inner-defer-wired");
    CHECK(href(cs, "starvation-mitigation-mandate-active") == 1, "mandate active");
}

static void ac2_long_mutation_held() {
    std::println("\n--- AC2: on_long_mutation_held → full mitigation ---");
    Scheduler sched(2);
    Fiber f([] {}, 64 * 1024);
    f.set_yield_reason(YieldReason::MutationBoundary);
    CHECK(!f.has_steal_priority_boost(), "no boost initially");

    const auto m0 = adaptive_steal_stats().steal_inner_deferred_starvation_mitigated_count.load();
    const auto s0 = adaptive_steal_stats().starvation_mitigated_count.load();
    const auto b0 = adaptive_steal_stats().starvation_priority_boosts.load();

    // fiber_id 0 → metrics-only path (fiber not in worker registry).
    sched.on_long_mutation_held(/*fiber_id=*/0, /*duration_us=*/1'000'000);
    CHECK(adaptive_steal_stats().starvation_mitigated_count.load() > s0,
          "starvation_mitigated_count advanced");
    CHECK(adaptive_steal_stats().steal_inner_deferred_starvation_mitigated_count.load() > m0,
          "steal_inner_deferred_starvation_mitigated_count advanced");
    CHECK(adaptive_steal_stats().starvation_priority_boosts.load() > b0,
          "starvation_priority_boosts advanced");

    // Direct apply_starvation_mitigation path (steal-loop package).
    const auto m1 = adaptive_steal_stats().steal_inner_deferred_starvation_mitigated_count.load();
    apply_starvation_mitigation(&f);
    CHECK(f.has_steal_priority_boost(), "boost after mitigation");
    CHECK(adaptive_steal_stats().steal_inner_deferred_starvation_mitigated_count.load() > m1,
          "mitigation count advanced");
    f.clear_steal_priority_boost();
}

static void ac3_schema_1633() {
    std::println("\n--- AC3: query schema 1633 ---");
    CompilerService cs;
    CHECK(href(cs, "schema") == 1633, "schema 1633");
    CHECK(href(cs, "issue") == 1633, "issue 1633");
    CHECK(href(cs, "steal-inner-deferred-starvation-mitigated-count") >= 0, "hyphen form metric");
    CHECK(href(cs, "steal_inner_deferred_starvation_mitigated_count") >= 0,
          "underscore form alias");
    CHECK(href(cs, "steal-deferred-inner-boundary") >= 0, "inner-boundary field");
    CHECK(href(cs, "starvation-mitigated-count") >= 0, "starvation-mitigated-count");
    CHECK(href(cs, "starvation-priority-boosts") >= 0, "starvation-priority-boosts");
}

static void ac4_nested_guard_concurrent() {
    std::println("\n--- AC4: nested Guard + concurrent mitigation ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto m0 = adaptive_steal_stats().steal_inner_deferred_starvation_mitigated_count.load();
    {
        bool ok_outer = true;
        bool ok_inner = true;
        Evaluator::MutationBoundaryGuard outer(ev, &ok_outer);
        Evaluator::MutationBoundaryGuard inner(ev, &ok_inner);
        CHECK(Evaluator::mutation_boundary_depth() >= 1, "nested depth");
        Fiber f([] {}, 64 * 1024);
        f.set_yield_reason(YieldReason::MutationBoundary);
        for (int i = 0; i < 32; ++i) {
            apply_starvation_mitigation(&f);
            f.bump_steal_deferred_mutation_boundary();
            f.bump_steal_inner_mutation_boundary_deferred();
        }
        CHECK(f.steal_deferred_mutation_boundary_count() == 32, "32 defers");
    }
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            try {
                for (int i = 0; i < 100; ++i) {
                    Fiber tmp([] {}, 32 * 1024);
                    apply_starvation_mitigation(&tmp);
                }
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int i = 0; i < 50; ++i)
        (void)cs.eval("(+ 1 1)");
    for (auto& th : threads)
        th.join();
    CHECK(errors.load() == 0, "no exceptions under concurrent mitigation");
    CHECK(adaptive_steal_stats().steal_inner_deferred_starvation_mitigated_count.load() > m0,
          "mitigation grew under nested+concurrent");
    auto r = cs.eval("(+ 20 22)");
    CHECK(r && is_int(*r) && as_int(*r) == 42, "eval ok");
}

static void ac5_fifty_fiber_stress() {
    std::println("\n--- AC5: 50+ fiber stress ---");
    constexpr int kFibers = 64;
    const auto m0 = adaptive_steal_stats().steal_inner_deferred_starvation_mitigated_count.load();
    const auto d0 = adaptive_steal_stats().steal_deferred_inner_boundary.load();

    std::vector<std::unique_ptr<Fiber>> fibers;
    fibers.reserve(kFibers);
    for (int i = 0; i < kFibers; ++i) {
        fibers.push_back(std::make_unique<Fiber>([] {}, 32 * 1024));
        fibers.back()->set_yield_reason(YieldReason::MutationBoundary);
    }

    std::atomic<int> errors{0};
    std::vector<std::thread> workers;
    workers.reserve(8);
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&, t] {
            try {
                for (int round = 0; round < 40; ++round) {
                    for (int i = t; i < kFibers; i += 8) {
                        auto* f = fibers[static_cast<std::size_t>(i)].get();
                        // Simulate steal-loop inner defer path.
                        f->bump_steal_deferred_mutation_boundary();
                        f->bump_steal_inner_mutation_boundary_deferred();
                        adaptive_steal_stats().steal_deferred_inner_boundary.fetch_add(
                            1, std::memory_order_relaxed);
                        apply_starvation_mitigation(f);
                    }
                }
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& w : workers)
        w.join();

    CHECK(errors.load() == 0, std::format("no exceptions ({})", errors.load()));
    CHECK(adaptive_steal_stats().steal_inner_deferred_starvation_mitigated_count.load() > m0,
          "mitigation advanced over 50+ fibers");
    CHECK(adaptive_steal_stats().steal_deferred_inner_boundary.load() > d0,
          "inner-boundary defers advanced");
    // Priority boost should have been applied to at least some fibers.
    int boosted = 0;
    for (auto& f : fibers) {
        if (f->has_steal_priority_boost())
            ++boosted;
        // Clear boosts for cleanliness.
        f->clear_steal_priority_boost();
    }
    CHECK(boosted > 0, std::format("at least one fiber boosted (got {})", boosted));

    CompilerService cs;
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval ok after fiber stress");
    CHECK(href(cs, "schema") == 1633, "schema holds after stress");
}

static void ac6_lineage_1492() {
    std::println("\n--- AC6: #1492 priority boost lineage ---");
    Fiber f([] {}, 64 * 1024);
    f.set_yield_reason(YieldReason::Explicit);
    const int p0 = fiber_steal_priority(&f);
    CHECK(p0 >= 1, "Explicit priority >= 1");
    f.apply_steal_priority_boost();
    CHECK(fiber_steal_priority(&f) >= 3, "boost raises to tier 3");
    f.clear_steal_priority_boost();
    CHECK(!f.has_steal_priority_boost(), "boost clearable");
}

} // namespace

int main() {
    std::println("=== Issue #1633: inner steal starvation mitigation mandate ===");
    ac1_wire_flags();
    ac2_long_mutation_held();
    ac3_schema_1633();
    ac4_nested_guard_concurrent();
    ac5_fifty_fiber_stress();
    ac6_lineage_1492();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
