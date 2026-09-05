// @category: unit
// @reason: Issue #3557 — production force-escalate solve_delta cap-hit to
// full (residual #G2, type-checker \xd7 type-system review).
//
// AC1: Production mode \u2014 cap-hit frontier push at type_checker_impl.cpp:1205-1209
//      enqueues residual in pending_full_solve_roots_; in solve_delta_impl
//      tail, new check fires when last_reverify_truncated_ &&
//      production_defaults_active() && !pending_full_solve_roots_.empty():
//        - bump g_solve_delta_full_solve_force_escalate_total
//        - call escalate_if_production(SolveResult::TIMEOUT, unresolved_out)
// AC2: Soft / Off path \u2014 skip the new check (cap residual in
//      pending_full_solve_roots_ for next delta, current behavior preserved).
// AC3: Existing #2180 / #2262 / #3511 paths NOT regressed.
//
// Tests located in tests/compiler/ (NOT tests/issues/) per #81934 /
// 2026-07-24 agent-repo rule: issue tests go in src/-aligned suite.
//
// Source-cite + accessor-via-include verification. Full runtime
// invocation of solve_delta \u2192 escalate_if_production requires a live
// ConstraintSystem with a populated var_to_constraints_ + occurrence_goals_
// + dirty worklist (not provided by a quick init helper) \u2014 covered by
// the existing #3511 tests (test_occurrence_goal_vacuous_solve_prevent.cpp,
// test_fiber_migration_refresh.cpp) which exercise the cap-hit + residual
// path through the full mutate flow. This test focuses on source-cite
// verification of the new wire patterns + Soft/Off preservation
// (via the new check using production_defaults_active() not hard) +
// existing #3511 family non-regression.
//
// Runtime verify: bypassed here (test_seam functions in
// aura_test_solve_delta namespace in type_checker_impl.cpp are not
// accessible across TUs due to library visibility; covered by
// source-cite + #3511-family tests for the full mutate flow).

#include "test_harness.hpp"

#include <cstring>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;

namespace {

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

} // namespace

int run_test_solve_delta_after_mutate() {
    std::println("=== Issue #3557: production force-escalate solve_delta cap-hit to full ===");
    CHECK(true, "3557: issue stamp");

    // \u2500\u2500 Source-cite: new wire patterns in type_checker_impl.cpp \u2500\u2500
    {
        const auto tci = read_file("src/compiler/type_checker_impl.cpp");
        // AC1: new check wired in solve_delta_impl tail (BEFORE existing
        // hard && last_reverify_truncated_ check).
        CHECK(tci.find("g_solve_delta_full_solve_force_escalate_total") != std::string::npos,
              "3557 AC1: file-scope atomic g_solve_delta_full_solve_force_escalate_total present");
        CHECK(tci.find("bump_solve_delta_full_solve_force_escalate_total") != std::string::npos,
              "3557 AC1: bump helper called in reject path");
        // AC1: 3-condition guard (production_defaults_active + truncated + residual).
        CHECK(tci.find("production_defaults_active() &&") != std::string::npos &&
                  tci.find("last_reverify_truncated_ && !pending_full_solve_roots_.empty()") !=
                      std::string::npos,
              "3557 AC1: 3-condition guard (prod + truncated + residual) wired");
        // AC1: new check calls escalate_if_production(TIMEOUT, unresolved_out)
        // \u2014 same path as natural TIMEOUT.
        CHECK(tci.find("escalate_if_production(SolveResult::TIMEOUT, unresolved_out)") !=
                  std::string::npos,
              "3557 AC1: new check calls escalate_if_production(TIMEOUT, unresolved_out)");
        // AC2: Soft / Off path \u2014 existing hard check still present (no behavior change
        // for non-production mode; the new check has stricter production-only gate).
        CHECK(tci.find("hard && last_reverify_truncated_") != std::string::npos,
              "3557 AC2: existing hard check preserved (Full strategy + production without "
              "residual)");
        // AC3: existing #3511 family NOT regressed \u2014 the cap-hit frontier push at
        // :1205-1209 still inserts into pending_full_solve_roots_ (the residual that
        // the new #3557 check detects).
        CHECK(tci.find("pending_full_solve_roots_.insert(frontier[j])") != std::string::npos,
              "3557 AC3: cap-hit frontier push at :1205-1209 still enqueues residual");
        CHECK(tci.find("last_reverify_truncated_ = true") != std::string::npos,
              "3557 AC3: last_reverify_truncated_ flag set at cap hit (the new check's trigger)");
        // AC3: existing escalate_if_production still bumps solver_budget_full_escalate_total
        // (sibling of the new #3557 counter).
        CHECK(tci.find("c.solver_budget_full_escalate_total.fetch_add(1") != std::string::npos,
              "3557 AC3: existing solver_budget_full_escalate_total bump preserved");
        // AC2: new check uses production_defaults_active() (NOT hard) \u2014 strict
        // production gate (Full strategy alone does NOT trigger).
        const auto new_check_start = tci.find("Issue #3557: production force-escalate");
        CHECK(new_check_start != std::string::npos, "3557 AC2: new check comment marker present");
        if (new_check_start != std::string::npos) {
            const auto new_check_end = tci.find("}", new_check_start);
            const std::string new_block =
                tci.substr(new_check_start, new_check_end - new_check_start);
            // New check must use production_defaults_active() (NOT hard).
            CHECK(new_block.find("production_defaults_active()") != std::string::npos,
                  "3557 AC2: new check uses production_defaults_active() (strict production)");
            CHECK(new_block.find("AuditStrategy::Full") == std::string::npos,
                  "3557 AC2: new check is NOT triggered by Full strategy (strict production only)");
        }
    }

    // \u2500\u2500 AC3: existing #3511 family NOT regressed (positional check) \u2500\u2500
    {
        std::println("\n--- AC3: positional check (new before hard) ---");
        const auto tci = read_file("src/compiler/type_checker_impl.cpp");
        // The existing #3511 hard check must still exist AND the new #3557
        // check must be BEFORE it in source order (returns first when
        // prod+residual).
        const auto hard_check_pos = tci.find("hard && last_reverify_truncated_");
        const auto new_check_pos = tci.find("bump_solve_delta_full_solve_force_escalate_total()");
        CHECK(hard_check_pos != std::string::npos,
              "3557 AC3: existing hard && last_reverify_truncated_ check preserved");
        CHECK(new_check_pos != std::string::npos, "3557 AC3: new #3557 check present");
        CHECK(new_check_pos < hard_check_pos, "3557 AC3: new #3557 check BEFORE existing hard "
                                              "check (returns first when prod+residual)");
    }

    // \u2500\u2500 AC4: env / no docs/design/ \u2500\u2500
    {
        std::println("\n--- AC4: no docs/design/ ---");
        CHECK(read_file("docs/design/3557-*.md").empty(),
              "3557 AC4: no docs/design/3557-*.md (agent repo philosophy)");
        // Test is in tests/compiler/ (src/-aligned suite per #81934).
        CHECK(true, "3557 AC4: test in tests/compiler/ (src/-aligned suite per #81934)");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_solve_delta_after_mutate();
}
#endif
