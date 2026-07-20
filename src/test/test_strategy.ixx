// test_strategy.ixx — Issue #1887: module facade for hot-path test strategy.
//
// Purpose: `import aura.test.strategy` for tests that prefer modules.
// Implementation authority: test/test_strategy.h (shared with production query).
// Layer: test (not on the production Compiler dependency DAG as a required edge)
//
// See: tests/STRATEGY.md

module;

#include "test/test_strategy.h"

export module aura.test.strategy;

export namespace aura::test::strategy {

// Re-export inventory.
using ::aura::test::strategy::kHotPathMinCoverageBp;
using ::aura::test::strategy::kHotPathScenarioCount;
using ::aura::test::strategy::kSelfModMinLoopsSlo;
using ::aura::test::strategy::kTestStrategyIssue;
using ::aura::test::strategy::kTestStrategyPhase;
using ::aura::test::strategy::kTestStrategySchema;

using ::aura::test::strategy::HotPathMatrixEntry;
using ::aura::test::strategy::HotPathScenario;
using ::aura::test::strategy::kHotPathMatrix;
using ::aura::test::strategy::matrix_entry;
using ::aura::test::strategy::scenario_name;

using ::aura::test::strategy::g_test_strategy_counters;
using ::aura::test::strategy::TestStrategyCounters;

using ::aura::test::strategy::coverage_hit_rate_bp;
using ::aura::test::strategy::hotpath_coverage_slo_met;
using ::aura::test::strategy::note_hotpath_scenario;
using ::aura::test::strategy::note_profile_selection;
using ::aura::test::strategy::note_self_mod_loop;
using ::aura::test::strategy::reset_for_test;
using ::aura::test::strategy::scenarios_hit_unique;
using ::aura::test::strategy::self_mod_slo_met;

using ::aura::test::strategy::profile_includes;
using ::aura::test::strategy::select_profile;
using ::aura::test::strategy::StrategyProfile;

} // namespace aura::test::strategy
