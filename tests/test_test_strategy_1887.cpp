// @category: unit
// @reason: Issue #1887 — test_strategy hot-path matrix + self-mod metrics
//
// AC1: matrix inventory (8 scenarios) + P0 anchors #1624/#1627
// AC2: note_hotpath_scenario + coverage_hit_rate_bp
// AC3: self-mod loop SLO stamps
// AC4: StrategyProfile selection
// AC5: query:test-strategy-stats schema 1887
// AC6: module import aura.test.strategy resolves

#include "test_harness.hpp"
#include "test/test_strategy.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.test.strategy;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;
using aura::test::note_strategy_profile;
using aura::test::note_strategy_scenario;
using aura::test::note_strategy_self_mod_loop;

namespace strat = aura::test::strategy;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:test-strategy-stats\") '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void ac1_matrix_and_p0() {
    std::println("\n--- AC1: matrix inventory + P0 anchors ---");
    CHECK(strat::kTestStrategyIssue == 1887, "issue 1887");
    CHECK(strat::kHotPathScenarioCount == 8, "8 scenarios");
    CHECK(strat::kHotPathMatrix[0].related_issue_primary == 1624, "P0 #1624");
    CHECK(strat::kHotPathMatrix[0].related_issue_secondary == 1627, "P0 #1627");
    CHECK(strat::scenario_name(strat::HotPathScenario::MutateStealGcOldClosure) ==
              "mutate-steal-gc-old-closure",
          "scenario name");
    // Secondary row also links production-readiness work
    CHECK(
        strat::kHotPathMatrix[static_cast<std::size_t>(strat::HotPathScenario::InvalidateJitDeopt)]
                .related_issue_primary == 1623,
        "invalidate row #1623");
}

void ac2_coverage_hits() {
    std::println("\n--- AC2: scenario hits + coverage bp ---");
    strat::reset_for_test();
    note_strategy_scenario(strat::HotPathScenario::MutateStealGcOldClosure, true);
    note_strategy_scenario(strat::HotPathScenario::InvalidateJitDeopt, true);
    note_strategy_scenario(strat::HotPathScenario::FiberGuardShapeEpoch, true);
    note_strategy_scenario(strat::HotPathScenario::TypedMutationInvariant, true);
    CHECK(strat::scenarios_hit_unique() == 4, "4 unique");
    CHECK(strat::coverage_hit_rate_bp() == 5000, "50% = 5000 bp");
    CHECK(strat::hotpath_coverage_slo_met(), "coverage SLO met at 50%");
    CHECK(strat::g_test_strategy_counters.total_hits.load() == 4, "total hits 4");
}

void ac3_self_mod_slo() {
    std::println("\n--- AC3: self-mod loop SLO ---");
    strat::reset_for_test();
    for (int i = 0; i < 1000; ++i)
        note_strategy_self_mod_loop(true);
    CHECK(strat::g_test_strategy_counters.self_mod_loops.load() == 1000, "1000 loops");
    CHECK(strat::self_mod_slo_met(), "self-mod SLO met");
    note_strategy_self_mod_loop(false);
    CHECK(strat::g_test_strategy_counters.self_mod_loops_fail.load() == 1, "1 fail loop");
}

void ac4_profiles() {
    std::println("\n--- AC4: StrategyProfile ---");
    strat::reset_for_test();
    note_strategy_profile(strat::StrategyProfile::AiSelfMod);
    CHECK(strat::g_test_strategy_counters.profile_selections.load() == 4, "AiSelfMod selects 4");
    CHECK(strat::profile_includes(strat::StrategyProfile::HotPathCore,
                                  strat::HotPathScenario::MutateStealGcOldClosure),
          "HotPathCore includes mutate-steal");
    CHECK(!strat::profile_includes(strat::StrategyProfile::Minimal,
                                   strat::HotPathScenario::RenderHotpathMutation),
          "Minimal excludes render");
}

void ac5_query(CompilerService& cs) {
    std::println("\n--- AC5: query:test-strategy-stats ---");
    strat::reset_for_test();
    // Drive full matrix for coverage SLO
    for (int i = 0; i < strat::kHotPathScenarioCount; ++i)
        note_strategy_scenario(static_cast<strat::HotPathScenario>(i), true);
    for (int i = 0; i < 1000; ++i)
        note_strategy_self_mod_loop(true);

    auto h = cs.eval("(engine:metrics \"query:test-strategy-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1887, "schema 1887");
    CHECK(href(cs, "active") == 1, "active");
    CHECK(href(cs, "matrix-count") == 8, "matrix-count");
    CHECK(href(cs, "coverage-hit-rate-bp") == 10000, "full coverage bp");
    CHECK(href(cs, "self-mod-slo-met") == 1, "self-mod-slo-met");
    CHECK(href(cs, "hotpath-coverage-slo-met") == 1, "hotpath-coverage-slo-met");
    CHECK(href(cs, "p0-anchor-a") == 1624, "p0-anchor-a");
    CHECK(href(cs, "p0-anchor-b") == 1627, "p0-anchor-b");
    CHECK(href(cs, "total-hits") == 8, "total-hits");
}

void ac6_module_constants() {
    std::println("\n--- AC6: module re-export ---");
    // import aura.test.strategy already active — constants must match header
    CHECK(aura::test::strategy::kTestStrategySchema == 1887, "module schema");
    CHECK(static_cast<int>(aura::test::strategy::HotPathScenario::Count) == 8, "module Count");
}

} // namespace

int main() {
    std::println("=== Issue #1887: test_strategy hot-path / self-mod ===");
    CompilerService cs;
    ac1_matrix_and_p0();
    ac2_coverage_hits();
    ac3_self_mod_slo();
    ac4_profiles();
    ac5_query(cs);
    ac6_module_constants();
    std::println("\n=== #1887: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
