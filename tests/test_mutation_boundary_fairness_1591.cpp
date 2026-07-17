// @category: integration
// @reason: Issue #1591 — mutation-boundary safe-yield + per-fiber depth +
// steal starvation mitigation fairness surfaces (schema 1591).
//
//   AC1: query:mutation-boundary-safe-yield schema 1591 + fairness fields
//   AC2: query:per-fiber-mutation-depth-stats schema 1591
//   AC3: query:mutation-boundary-fairness-stats unifies depth/hold/steal
//   AC4: safe-yield under depth 0 vs skipped-held under Guard
//   AC5: steal_inner_deferred_starvation_mitigated_count still visible
//   AC6: concurrent fibers + explicit safe-yield probes (no crash)

#include "test_harness.hpp"
#include "serve/metrics.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, const char* prim, const char* key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", prim, key));
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
}

static void ac1_safe_yield_schema() {
    std::println("\n--- AC1: safe-yield schema 1591 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:mutation-boundary-safe-yield\")");
    CHECK(h && is_hash(*h), "safe-yield returns hash");
    CHECK(href(cs, "query:mutation-boundary-safe-yield", "schema") == 1591, "schema 1591");
    CHECK(href(cs, "query:mutation-boundary-safe-yield", "issue") == 1591, "issue 1591");
    CHECK(href(cs, "query:mutation-boundary-safe-yield", "boundary-depth") >= 0, "depth");
    CHECK(href(cs, "query:mutation-boundary-safe-yield", "avg-hold-time-us") >= 0, "avg hold");
    CHECK(href(cs, "query:mutation-boundary-safe-yield",
               "steal-inner-deferred-starvation-mitigated-count") >= 0,
          "steal mitigation field");
    auto st = cs.eval("(engine:metrics \"query:mutation-boundary-safe-yield-stats\")");
    CHECK(st && is_hash(*st), "stats hash");
    CHECK(href(cs, "query:mutation-boundary-safe-yield-stats", "schema") == 1591,
          "stats schema 1591");
}

static void ac2_depth_stats() {
    std::println("\n--- AC2: per-fiber-mutation-depth-stats ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:per-fiber-mutation-depth-stats\")");
    CHECK(h && is_hash(*h), "depth-stats hash");
    CHECK(href(cs, "query:per-fiber-mutation-depth-stats", "schema") == 1591, "schema 1591");
    CHECK(href(cs, "query:per-fiber-mutation-depth-stats", "lifetime-max") >= 0, "lifetime-max");
    CHECK(href(cs, "query:per-fiber-mutation-depth-stats", "live-depth") >= 0, "live-depth");
    CHECK(href(cs, "query:per-fiber-mutation-depth-stats",
               "safepoint-wait-while-mutation-held-us") >= 0,
          "safepoint wait field");
}

static void ac3_fairness_dashboard() {
    std::println("\n--- AC3: mutation-boundary-fairness-stats ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:mutation-boundary-fairness-stats\")");
    CHECK(h && is_hash(*h), "fairness hash");
    CHECK(href(cs, "query:mutation-boundary-fairness-stats", "schema") == 1591, "schema 1591");
    CHECK(href(cs, "query:mutation-boundary-fairness-stats", "boundary-depth") >= 0, "depth");
    CHECK(href(cs, "query:mutation-boundary-fairness-stats", "per-fiber-stack-depth-max") >= 0,
          "per-fiber max");
    CHECK(href(cs, "query:mutation-boundary-fairness-stats",
               "steal-inner-deferred-starvation-mitigated-count") >= 0,
          "steal mitigation");
    CHECK(href(cs, "query:mutation-boundary-fairness-stats",
               "mutation-stack-depth-histogram-samples") >= 0,
          "histogram samples");
}

static void ac4_yield_vs_held() {
    std::println("\n--- AC4: yield when free vs skip when held ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    // Free path
    const int rc0 = ev.try_safe_yield_at_boundary(0);
    CHECK(rc0 == 0, "safe yield at depth 0 returns 0");
    bool ok = true;
    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        Evaluator::MutationBoundaryGuard guard(ev, &ok);
#pragma GCC diagnostic pop
        const int rc1 = ev.try_safe_yield_at_boundary(0);
        CHECK(rc1 == 1, "safe yield under Guard returns 1 (skipped-held)");
    }
    const int rc2 = ev.try_safe_yield_at_boundary(0);
    CHECK(rc2 == 0, "safe yield after Guard returns 0");
}

static void ac5_steal_metric_surface() {
    std::println("\n--- AC5: steal starvation metric surface ---");
    CompilerService cs;
    auto& s = aura::serve::metrics::adaptive_steal_stats();
    const auto m0 = s.steal_inner_deferred_starvation_mitigated_count.load();
    s.steal_inner_deferred_starvation_mitigated_count.fetch_add(1, std::memory_order_relaxed);
    CHECK(href(cs, "query:mutation-boundary-fairness-stats",
               "steal-inner-deferred-starvation-mitigated-count") >=
              static_cast<std::int64_t>(m0 + 1),
          "fairness sees steal mitigation bump");
    CHECK(href(cs, "query:orchestration-steal-stats",
               "steal-inner-deferred-starvation-mitigated-count") >=
              static_cast<std::int64_t>(m0 + 1),
          "orchestration-steal-stats still exposes mitigation");
}

static void ac6_concurrent_probes() {
    std::println("\n--- AC6: concurrent fairness probes ---");
    CompilerService cs;
    std::atomic<int> done{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&cs, &done] {
            for (int i = 0; i < 20; ++i) {
                (void)cs.evaluator().try_safe_yield_at_boundary(0);
                (void)cs.eval("(engine:metrics \"query:mutation-boundary-fairness-stats\")");
            }
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& th : threads)
        th.join();
    CHECK(done.load() == 4, "all probe threads finished");
}

} // namespace

int main() {
    std::println("=== test_mutation_boundary_fairness_1591 (#1591) ===");
    ac1_safe_yield_schema();
    ac2_depth_stats();
    ac3_fairness_dashboard();
    ac4_yield_vs_held();
    ac5_steal_metric_surface();
    ac6_concurrent_probes();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
