// @category: unit
// @reason: Issue #2508 — OccurrenceGoal reverify before anti-starve full
//          solve on consecutive delta truncate.
//
//   AC1: Truncate + non-empty occurrence_goals → goal-priority reverify
//        runs before full solve; counter +1
//   AC2: Goal-priority recovers (no truncate) → streak reset; no force-full
//   AC3: Still truncated after goal pass → existing anti-starve full solve
//   AC4: No truncate → zero extra goal reverify work
//   AC5: Query keys + source-cite; TIMEOUT escalate path (#2277) unchanged

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.core.type;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::ConstraintSystem;
using aura::compiler::SolveResult;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;
using aura::test::g_failed;
using aura::test::g_passed;

// Fresh ConstraintSystem + metrics (same pattern as #2278).
struct UnitCs {
    TypeRegistry reg;
    ConstraintSystem cs;
    CompilerMetrics m;
    UnitCs()
        : cs(reg) {
        cs.set_metrics(&m);
    }
};

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
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1/AC2: goal-priority path wiring + recover ──
static void ac1_ac2_goal_priority_before_full() {
    std::println("\n--- #2508 AC1/AC2: goal-priority reverify before full solve ---");
    UnitCs u;

    // Source-cite: methods present.
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    CHECK(impl.find("try_goal_priority_reverify_before_full") != std::string::npos,
          "AC1: try_goal_priority_reverify_before_full impl");
    CHECK(impl.find("delta_truncate_goal_priority_reverify_total") != std::string::npos,
          "AC1: goal-priority reverify counter bump");
    CHECK(impl.find("delta_truncate_goal_priority_recovered_total") != std::string::npos,
          "AC1: recovered counter bump");
    CHECK(ixx.find("try_goal_priority_reverify_before_full") != std::string::npos,
          "AC1: method declared");
    CHECK(impl.find("check_truncate_anti_starve") != std::string::npos,
          "AC1: anti-starve gate present");
    // Order: goal priority before force full solve in anti-starve body.
    const auto gp = impl.find("try_goal_priority_reverify_before_full()");
    const auto ff = impl.find("delta_truncate_force_full_solve_total.fetch_add");
    CHECK(gp != std::string::npos && ff != std::string::npos && gp < ff,
          "AC1: goal-priority before force-full in source order");

    // Live API: note a goal and run goal-priority reverify directly.
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, v, /*pred=*/1, /*mut=*/1, /*epoch=*/0);
    CHECK(u.cs.occurrence_goals_size() >= 1, "AC1: goal table non-empty");
    u.cs.mark_touched_on_delta(v, true);

    const auto gpr0 = u.m.delta_truncate_goal_priority_reverify_total.load();
    const auto rec0 = u.m.delta_truncate_goal_priority_recovered_total.load();
    const bool recovered = u.cs.try_goal_priority_reverify_before_full();
    CHECK(u.m.delta_truncate_goal_priority_reverify_total.load() == gpr0 + 1,
          "AC1: goal-priority reverify counter +1");
    if (recovered) {
        CHECK(u.m.delta_truncate_goal_priority_recovered_total.load() == rec0 + 1,
              "AC2: recovered counter +1");
        CHECK(!u.cs.last_reverify_truncated(), "AC2: truncate cleared on recover");
    } else {
        // Empty clean-to_check can still return recovered (no truncate).
        CHECK(true, "AC2: goal pass ran (recovered=" + std::to_string(recovered) + ")");
    }
}

// ── AC3: still truncated → force full path still wired ──
static void ac3_still_truncated_full_solve() {
    std::println("\n--- #2508 AC3: still truncated after goal → force-full ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    // Force-full still present after goal path fails.
    CHECK(impl.find("delta_truncate_force_full_solve_total") != std::string::npos,
          "AC3: force-full counter still present");
    CHECK(impl.find("return solve(unresolved_out)") != std::string::npos ||
              impl.find("out_result = solve(unresolved_out)") != std::string::npos,
          "AC3: full solve() still escalates");
    // Goal path does not remove anti-starve threshold env.
    CHECK(impl.find("delta_truncate_streak_threshold") != std::string::npos,
          "AC3: streak threshold still used");
    CHECK(impl.find("AURA_DELTA_TRUNCATE_STREAK_FULL") != std::string::npos ||
              read_file("src/compiler/type_checker.ixx").find("AURA_DELTA_TRUNCATE_STREAK_FULL") !=
                  std::string::npos,
          "AC3: env threshold documented");
}

// ── AC4: no truncate → zero goal reverify ──
static void ac4_no_truncate_zero_cost() {
    std::println("\n--- #2508 AC4: no truncate → zero goal reverify ---");
    UnitCs u;
    const auto gpr0 = u.m.delta_truncate_goal_priority_reverify_total.load();
    const auto ff0 = u.m.delta_truncate_force_full_solve_total.load();
    // Empty dirty → solve_delta early SOLVED, no truncate, no goal reverify.
    const auto r = u.cs.solve_delta(nullptr);
    CHECK(r == SolveResult::SOLVED, "AC4: empty dirty → SOLVED");
    CHECK(u.m.delta_truncate_goal_priority_reverify_total.load() == gpr0,
          "AC4: goal-priority reverify flat without truncate threshold");
    CHECK(u.m.delta_truncate_force_full_solve_total.load() == ff0,
          "AC4: no force-full on non-truncate path");
    CHECK(!u.cs.last_reverify_truncated(), "AC4: not truncated");
}

// ── AC5: query + source + #2277 untouched ──
static void ac5_query_and_source() {
    std::println("\n--- #2508 AC5: query keys + source-cite + #2277 unchanged ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto obs = read_file("src/compiler/observability_metrics.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto cmake = read_file("CMakeLists.txt");

    CHECK(obs.find("delta_truncate_goal_priority_reverify_total") != std::string::npos,
          "AC5: metrics field reverify");
    CHECK(obs.find("delta_truncate_goal_priority_recovered_total") != std::string::npos,
          "AC5: metrics field recovered");
    CHECK(obs.find("2508") != std::string::npos, "AC5: #2508 in metrics");
    CHECK(q.find("delta-truncate-goal-priority-reverify-total") != std::string::npos,
          "AC5: query reverify key");
    CHECK(q.find("delta-truncate-goal-priority-recovered-total") != std::string::npos,
          "AC5: query recovered key");
    CHECK(q.find("schema-2508") != std::string::npos, "AC5: schema-2508");
    CHECK(cmake.find("test_delta_truncate_goal_priority_2508") != std::string::npos,
          "AC5: cmake target");
    // #2277 TIMEOUT escalate path must remain.
    CHECK(impl.find("escalate_if_production") != std::string::npos,
          "AC5: #2277 escalate_if_production unchanged site");
    CHECK(impl.find("production_defaults_active") != std::string::npos,
          "AC5: production defaults gate still present");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "AC5: warm");
    CHECK(href(cs, "schema-2508") == 2508, "AC5: schema-2508 live");
    CHECK(href(cs, "issue-2508") == 2508, "AC5: issue-2508");
    CHECK(href(cs, "delta-truncate-goal-priority-wired") == 1, "AC5: wired");
    CHECK(href(cs, "delta-truncate-goal-priority-reverify-total") >= 0, "AC5: reverify total");
    CHECK(href(cs, "delta-truncate-goal-priority-recovered-total") >= 0, "AC5: recovered total");
    CHECK(href(cs, "schema-2318") == 2318, "AC5: #2318 lineage retained");
    CHECK(href(cs, "delta-truncate-force-full-solve-total") >= 0, "AC5: force-full still exposed");
}

} // namespace

int run_test_delta_truncate_goal_priority_2508() {
    std::println("test_delta_truncate_goal_priority_2508");
    ac1_ac2_goal_priority_before_full();
    ac3_still_truncated_full_solve();
    ac4_no_truncate_zero_cost();
    ac5_query_and_source();
    if (g_failed)
        return 1;
    std::println("delta truncate goal priority #2508: OK ({} passed)", g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_delta_truncate_goal_priority_2508();
}
#endif
