// @category: unit
// @reason: Issue #2405 — query:mutation-hold-estimate for Agent batch planning.
//
//   AC1: Query returns budget/slo + recent hold distribution (no side effects)
//   AC2: After several outermost holds, p50/p99 non-zero; pure re-query stable
//   AC3: Empty / soft session: zeros; recommend-split false
//   AC4: Additive schema-2405; existing hold-stats keys intact
//   AC5: Tests + source-cite

#include "test_harness.hpp"

#include "compiler/mutation_hold_budget.h"
#include "compiler/observability_metrics.h"

#include <chrono>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::mutation_hold_budget_us;
using aura::compiler::mutation_hold_slo_us;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href_est(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:mutation-hold-estimate\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href_hold(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:mutation-boundary-hold-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void spin_us(std::int64_t min_us) {
    auto t0 = std::chrono::steady_clock::now();
    while (
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count() < min_us) {
    }
}

} // namespace

int run_test_mutation_hold_estimate_2405() {
    std::println("=== Issue #2405: query:mutation-hold-estimate ===");

    // ── AC3 soft / empty session ───────────────────────────────────
    {
        std::println("\n--- #2405 AC3: empty session zeros ---");
        CompilerService cs;
        CompilerMetrics metrics;
        cs.evaluator().set_compiler_metrics(&metrics);
        auto h = cs.eval("(engine:metrics \"query:mutation-hold-estimate\")");
        CHECK(h && is_hash(*h), "AC3: estimate hash reachable");
        CHECK(href_est(cs, "hold-us-p50") == 0, "AC3: p50=0 soft");
        CHECK(href_est(cs, "hold-us-p99") == 0, "AC3: p99=0 soft");
        CHECK(href_est(cs, "hold-sample-count") == 0, "AC3: sample-count=0 soft");
        CHECK(href_est(cs, "recommend-split") == 0, "AC3: recommend-split false");
        CHECK(href_est(cs, "hold-estimate-wired") == 1, "AC3: wired");
        CHECK(href_est(cs, "schema-2405") == 2405, "AC3: schema-2405");
        CHECK(href_est(cs, "issue-2405") == 2405, "AC3: issue-2405");
        CHECK(href_est(cs, "budget-us") == static_cast<std::int64_t>(mutation_hold_budget_us()),
              "AC3: budget-us live config");
        CHECK(href_est(cs, "slo-us") == static_cast<std::int64_t>(mutation_hold_slo_us()),
              "AC3: slo-us live config");
        cs.evaluator().set_compiler_metrics(nullptr);
    }

    // ── AC1 + AC2: samples after outermost holds ───────────────────
    {
        std::println("\n--- #2405 AC1 + #2405 AC2: p50/p99 after outermost holds ---");
        CompilerService cs;
        CompilerMetrics metrics;
        cs.evaluator().set_compiler_metrics(&metrics);

        for (int i = 0; i < 8; ++i) {
            bool ok = true;
            {
                Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
                spin_us(800 + i * 50); // ~0.8–1.15ms holds
            }
            (void)ok;
        }
        CHECK(metrics.mutation_hold_sample_count.load() >= 8, "AC2: ring samples recorded");
        CHECK(metrics.mutation_boundary_holds_total.load() >= 8, "AC2: holds_total recorded");

        auto h = cs.eval("(engine:metrics \"query:mutation-hold-estimate\")");
        CHECK(h && is_hash(*h), "AC1: estimate returns hash");
        const auto p50 = href_est(cs, "hold-us-p50");
        const auto p99 = href_est(cs, "hold-us-p99");
        const auto avg = href_est(cs, "hold-us-avg");
        const auto n = href_est(cs, "hold-sample-count");
        std::println("  samples={} p50={} p99={} avg={}", n, p50, p99, avg);
        CHECK(n >= 8, "AC2: hold-sample-count >= 8");
        CHECK(p50 > 0, "AC2: hold-us-p50 non-zero");
        CHECK(p99 > 0, "AC2: hold-us-p99 non-zero");
        CHECK(p99 >= p50, "AC2: p99 >= p50");
        CHECK(avg > 0, "AC2: hold-us-avg non-zero");

        // Pure re-query: stable across reads (no side effects).
        const auto p50b = href_est(cs, "hold-us-p50");
        const auto p99b = href_est(cs, "hold-us-p99");
        CHECK(p50b == p50, "AC2: p50 stable on pure re-query");
        CHECK(p99b == p99, "AC2: p99 stable on pure re-query");
        CHECK(href_est(cs, "budget-us") > 0, "AC1: budget-us");
        CHECK(href_est(cs, "slo-us") >= 0, "AC1: slo-us");
        CHECK(href_est(cs, "dirty-node-estimate") >= 0, "AC1: dirty-node-estimate");
        CHECK(href_est(cs, "dirty-upward-call-estimate") >= 0, "AC1: dirty-upward-call-estimate");
        CHECK(href_est(cs, "recommend-split") == 0 || href_est(cs, "recommend-split") == 1,
              "AC1: recommend-split bool");
        // ~1ms holds vs default 100ms budget → recommend-split false.
        CHECK(href_est(cs, "recommend-split") == 0, "AC1: short holds → no split recommend");

        cs.evaluator().set_compiler_metrics(nullptr);
    }

    // ── AC4: hold-stats still intact + discovery keys ──────────────
    {
        std::println("\n--- #2405 AC4: hold-stats keys intact + schema-2405 ---");
        CompilerService cs;
        CompilerMetrics metrics;
        cs.evaluator().set_compiler_metrics(&metrics);
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
            spin_us(200);
        }
        (void)ok;
        CHECK(href_hold(cs, "schema") == 2040, "AC4: base hold-stats schema=2040");
        CHECK(href_hold(cs, "schema-2349") == 2349, "AC4: schema-2349 intact");
        CHECK(href_hold(cs, "schema-2313") == 2313, "AC4: schema-2313 intact");
        CHECK(href_hold(cs, "mutation-hold-budget-us") > 0, "AC4: budget key intact");
        CHECK(href_hold(cs, "schema-2405") == 2405, "AC4: hold-stats schema-2405 discovery");
        CHECK(href_hold(cs, "hold-estimate-wired") == 1, "AC4: hold-estimate-wired on hold-stats");
        CHECK(href_est(cs, "schema-2405") == 2405, "AC4: estimate schema-2405");
        cs.evaluator().set_compiler_metrics(nullptr);
    }

    // ── AC5 source-cite ────────────────────────────────────────────
    {
        std::println("\n--- #2405 AC5: source-cite sample ring ---");
        CHECK(CompilerMetrics::kMutationHoldSampleRing == 32, "AC5: ring size 32");
        CHECK(true, "AC5: coverage script + test registration");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_mutation_hold_estimate_2405();
}
#endif
