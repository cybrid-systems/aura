// @category: unit
// @reason: Issue #2349 — outermost hold SLO circuit-breaker (production
// default fail path). Bounds GC/steal tail under AI agent long mutate.
//
//   AC1: Production + hold > SLO → success_flag=false; violation counter
//   AC2: Soft / sandbox → metric only; mutate may still succeed
//   AC3: Hold ≤ SLO → zero force-fail (existing hold metrics only)
//   AC4: AURA_MUTATION_HOLD_SLO_US=0 disables; query schema-2349 keys
//   AC5: Tests + decision table + source-cite

#include "test_harness.hpp"

#include "compiler/mutation_hold_budget.h"
#include "compiler/observability_metrics.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
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
using aura::compiler::mutation_hold_slo_soft_mode;
using aura::compiler::mutation_hold_slo_us;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

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

static std::int64_t href(CompilerService& cs, std::string_view key) {
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

// ── AC1: Production force-fail on SLO breach ──
static void ac1_production_force_fail() {
    std::println("\n--- AC1: Production + hold > SLO → force-fail ---");
    // Force production path (not Soft).
    unsetenv("AURA_MUTATION_HOLD_SLO_SOFT");
    unsetenv("AURA_SANDBOX");
    setenv("AURA_MUTATION_HOLD_SLO_US", "2000", 1); // 2ms SLO

    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);

    const auto viol0 = metrics.mutation_hold_slo_violation_total.load();
    const auto rollback0 = metrics.mutation_boundary_rollbacks_total.load();

    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        spin_us(5'000); // > 2ms SLO
    }
    CHECK(!ok, "AC1: success_flag forced false");
    CHECK(metrics.mutation_hold_slo_violation_total.load() > viol0,
          "AC1: mutation_hold_slo_violation_total bumped");
    CHECK(metrics.mutation_boundary_rollbacks_total.load() > rollback0,
          "AC1: rollback path on success=false");

    cs.evaluator().set_compiler_metrics(nullptr);
    unsetenv("AURA_MUTATION_HOLD_SLO_US");
}

// ── AC2: Soft metric-only ──
static void ac2_soft_metric_only() {
    std::println("\n--- AC2: Soft/sandbox → metric only ---");
    setenv("AURA_MUTATION_HOLD_SLO_SOFT", "1", 1);
    setenv("AURA_MUTATION_HOLD_SLO_US", "2000", 1);
    CHECK(mutation_hold_slo_soft_mode(), "AC2: soft mode active");

    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);

    const auto viol0 = metrics.mutation_hold_slo_violation_total.load();
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        spin_us(5'000);
    }
    CHECK(ok, "AC2: Soft does not force-fail");
    CHECK(metrics.mutation_hold_slo_violation_total.load() > viol0,
          "AC2: violation counter still bumps");

    cs.evaluator().set_compiler_metrics(nullptr);
    unsetenv("AURA_MUTATION_HOLD_SLO_SOFT");
    unsetenv("AURA_MUTATION_HOLD_SLO_US");
}

// ── AC3: under SLO zero force ──
static void ac3_under_slo_no_force() {
    std::println("\n--- AC3: hold ≤ SLO → no force-fail ---");
    unsetenv("AURA_MUTATION_HOLD_SLO_SOFT");
    unsetenv("AURA_SANDBOX");
    setenv("AURA_MUTATION_HOLD_SLO_US", "50000", 1); // 50ms

    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);

    const auto viol0 = metrics.mutation_hold_slo_violation_total.load();
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        spin_us(500); // well under 50ms
    }
    CHECK(ok, "AC3: under-SLO flag stays true");
    CHECK(metrics.mutation_hold_slo_violation_total.load() == viol0,
          "AC3: no SLO violation under threshold");
    CHECK(metrics.mutation_boundary_holds_total.load() >= 1, "AC3: hold sample still recorded");

    cs.evaluator().set_compiler_metrics(nullptr);
    unsetenv("AURA_MUTATION_HOLD_SLO_US");
}

// ── AC4: disable + query keys ──
static void ac4_disable_and_query() {
    std::println("\n--- AC4: SLO=0 disables; query schema-2349 ---");
    setenv("AURA_MUTATION_HOLD_SLO_US", "0", 1);
    CHECK(mutation_hold_slo_us() == 0, "AC4: SLO 0 disables circuit");

    unsetenv("AURA_MUTATION_HOLD_SLO_SOFT");
    unsetenv("AURA_SANDBOX");

    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);

    const auto viol0 = metrics.mutation_hold_slo_violation_total.load();
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        spin_us(5'000); // would exceed default 100ms if enabled; SLO=0 → no-op
    }
    CHECK(ok, "AC4: disabled SLO never force-fails");
    CHECK(metrics.mutation_hold_slo_violation_total.load() == viol0,
          "AC4: no violation when disabled");

    // Query surface (warm eval for metrics registration).
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2349") == 2349, "schema-2349");
    CHECK(href(cs, "issue-2349") == 2349, "issue-2349");
    CHECK(href(cs, "mutation-hold-slo-wired") == 1, "slo-wired");
    CHECK(href(cs, "mutation-hold-slo-us") == 0, "slo-us=0 queryable");
    CHECK(href(cs, "mutation-hold-slo-violation-total") >= 0, "violation-total queryable");
    // Lineage retained.
    CHECK(href(cs, "schema-2313") == 2313, "schema-2313 retained");
    CHECK(href(cs, "schema-2199") == 2199, "schema-2199 retained");

    cs.evaluator().set_compiler_metrics(nullptr);
    unsetenv("AURA_MUTATION_HOLD_SLO_US");
}

// ── AC5: source-cite + decision table ──
static void ac5_source_cite() {
    std::println("\n--- AC5: source-cite + decision table ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto bud = read_file("src/compiler/mutation_hold_budget.h");
    const auto met = read_file("src/compiler/observability_metrics.h");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(emb.find("Issue #2349") != std::string::npos, "AC5: dtor cites #2349");
    CHECK(emb.find("mutation_hold_slo_us") != std::string::npos, "AC5: dtor uses SLO accessor");
    CHECK(emb.find("mutation_hold_slo_violation_total") != std::string::npos,
          "AC5: dtor bumps violation");
    CHECK(emb.find("Decision table") != std::string::npos ||
              emb.find("decision table") != std::string::npos ||
              bud.find("Decision table") != std::string::npos,
          "AC5: decision table documented");
    CHECK(bud.find("AURA_MUTATION_HOLD_SLO_US") != std::string::npos, "AC5: SLO env");
    CHECK(bud.find("mutation_hold_slo_soft_mode") != std::string::npos, "AC5: soft helper");
    CHECK(met.find("mutation_hold_slo_violation_total") != std::string::npos, "AC5: metrics");
    CHECK(q.find("schema-2349") != std::string::npos, "AC5: query schema-2349");
    CHECK(q.find("mutation-hold-slo-violation-total") != std::string::npos, "AC5: query key");
    // Does not invent a second timer — reuses outermost dtor hold sample.
    CHECK(emb.find("no second timer") != std::string::npos ||
              emb.find("Reuses this outermost dtor hold sample") != std::string::npos,
          "AC5: no second timer");
}

} // namespace

int main() {
    std::println("=== Issue #2349: outermost hold SLO circuit-breaker ===");
    ac1_production_force_fail();
    ac2_soft_metric_only();
    ac3_under_slo_no_force();
    ac4_disable_and_query();
    ac5_source_cite();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
