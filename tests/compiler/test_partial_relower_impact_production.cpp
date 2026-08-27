// @category: unit
// @reason: Issue #3310 — Production fail-closed on unknown ImpactScope
// (refine #3034 / #3068 / #3097). impact_upper_bound_for_entry_
// returns 0 when the workspace flat / pool is missing or the define
// root is not found; that 0 is *unknown* impact, not *empty* impact.
// Under production defaults, an unknown impact on a non-zero dirty
// window must fail-closed to full (silent under-cascade on a partial
// peel would under-mark cross-fn callee edges that only the hybrid
// DepGraph / ImpactScope walk would have seen). Soft / Off keeps
// zero-cost threshold partial when ub==0 (existing contract). The
// map-empty sentinel -1 path through should_partial_relower_impact_checked
// is untouched (AC3 — sentinel still forces full via the existing
// impact_ub > dirty_count branch).
//
//   AC1: production + dirty_n > 0 + ub == 0 → false (force full).
//        Soft/Off + dirty_n > 0 + ub == 0 → matches existing
//        should_partial_relower (zero-cost contract preserved).
//   AC2: production + dirty_n > 0 + ub <= dirty_n → matches existing
//        should_partial_relower (no #3034 / #2206 regression).
//   AC3: production + dirty_n > 0 + sentinel -1 (map empty / desync)
//        → false (existing path; helper delegates unchanged).
//   AC4: dirty_n == 0 → false in all branches (clean window).
//   AC5: production_consult reflects
//        production_defaults_active() || AuditStrategy::Full; under
//        apply_dev_audit_defaults() + Sampled, production_consult is
//        false (existing helper path runs, Soft/Off zero-cost).
//   AC6: Source-cite — service.ixx relower_dirty_defines_from_workspace
//        partial branch calls should_partial_relower_impact_checked_prod
//        with production_consult derived from typed_audit; no other
//        production-fail-close gates removed.

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"

#include <cstddef>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.ir_cache_pure;

namespace {

using aura::compiler::ir_cache_pure::should_partial_relower_impact_checked;
using aura::compiler::ir_cache_pure::should_partial_relower_impact_checked_prod;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::get_strategy;
using aura::compiler::typed_audit::production_defaults_active;
using aura::test::g_failed;
using aura::test::g_passed;

// AC1 + AC4: production + ub == 0 + dirty_n > 0 → false (force full).
// Soft + ub == 0 + dirty_n > 0 → matches existing helper (zero-cost).
static void ac1_production_unknown_impact_fail_closed() {
    // Production + 3 dirty + ub == 0 → false.
    CHECK(!should_partial_relower_impact_checked_prod(3, 0, /*production=*/true),
          "AC1: production + dirty_n=3 + ub=0 → false (force full)");
    // Production + 1 dirty + ub == 0 → false.
    CHECK(!should_partial_relower_impact_checked_prod(1, 0, /*production=*/true),
          "AC1: production + dirty_n=1 + ub=0 → false (force full)");
    // Production + huge dirty + ub == 0 → false.
    CHECK(!should_partial_relower_impact_checked_prod(1024, 0, /*production=*/true),
          "AC1: production + dirty_n=1024 + ub=0 → false (force full)");
    // Soft + 3 dirty + ub == 0 → matches existing helper
    // (should_partial_relower_impact_checked(3, 0) — only early-exit
    //  on dirty_count == 0 fires, returns should_partial_relower(3)).
    const bool soft_ub0 = should_partial_relower_impact_checked_prod(3, 0, false);
    const bool soft_existing = should_partial_relower_impact_checked(3, 0);
    CHECK(soft_ub0 == soft_existing, "AC1+AC4: Soft + dirty_n=3 + ub=0 → matches existing helper "
                                     "(zero-cost contract preserved)");
    CHECK(soft_ub0, "AC4: Soft + dirty_n=3 + ub=0 → true (partial allowed)");
    // Soft + 1 dirty + ub == 0 → true (threshold met).
    CHECK(should_partial_relower_impact_checked_prod(1, 0, false),
          "AC4: Soft + dirty_n=1 + ub=0 → true (partial allowed)");
}

// AC2: production + computable ub with ub <= dirty_n → matches existing
// helper (no #3034 / #2206 regression). Boundary cases.
static void ac2_production_computable_ub_matches_existing() {
    // ub == dirty_n → matches existing (equal threshold is allowed).
    CHECK(should_partial_relower_impact_checked_prod(3, 3, true) ==
              should_partial_relower_impact_checked(3, 3),
          "AC2: production + dirty_n=3 + ub=3 → matches existing");
    // ub < dirty_n → matches existing (strict under-count is fine).
    CHECK(should_partial_relower_impact_checked_prod(3, 1, true) ==
              should_partial_relower_impact_checked(3, 1),
          "AC2: production + dirty_n=3 + ub=1 → matches existing");
    // ub == 0 + production true → false (the new fail-closed), even
    // when existing helper would have returned true.
    CHECK(!should_partial_relower_impact_checked_prod(3, 0, true),
          "AC2: production + dirty_n=3 + ub=0 → false (new fail-closed)");
    // ub == 0 + production false → true (Soft zero-cost preserved).
    CHECK(should_partial_relower_impact_checked_prod(3, 0, false),
          "AC2: Soft + dirty_n=3 + ub=0 → true (zero-cost preserved)");
}

// AC3: production + sentinel -1 (map empty / desync) → false (existing
// path through should_partial_relower_impact_checked unchanged).
static void ac3_production_sentinel_minus_one() {
    // Sentinel -1 cast to size_t is huge; the existing
    // should_partial_relower_impact_checked branch
    // `impact_upper_bound > dirty_count` fires → false. The new
    // production check (impact_ub == 0) does NOT fire on -1.
    const std::size_t sentinel = static_cast<std::size_t>(-1); // map empty / desync sentinel
    CHECK(!should_partial_relower_impact_checked_prod(3, sentinel, true),
          "AC3: production + dirty_n=3 + ub=SENTINEL → false "
          "(existing path, sentinel force-full preserved)");
    CHECK(!should_partial_relower_impact_checked_prod(3, sentinel, false),
          "AC3: Soft + dirty_n=3 + ub=SENTINEL → false "
          "(existing path unchanged for Soft too)");
    // Confirm new fail-closed does NOT fire on sentinel: prod + dirty +
    // sentinel → false via existing branch, not via the new
    // production && ub==0 branch.
    CHECK(!should_partial_relower_impact_checked_prod(3, 4, true),
          "AC3: production + dirty_n=3 + ub=4 → false (existing branch, "
          "ub > dirty_n gate)");
}

// AC4: dirty_n == 0 → false in all branches (clean window, zero-cost
// early-exit).
static void ac4_clean_window_zero_cost() {
    CHECK(!should_partial_relower_impact_checked_prod(0, 0, true),
          "AC4: clean window + production + ub=0 → false");
    CHECK(!should_partial_relower_impact_checked_prod(0, 0, false),
          "AC4: clean window + Soft + ub=0 → false");
    CHECK(!should_partial_relower_impact_checked_prod(0, 5, true),
          "AC4: clean window + production + ub=5 → false");
    CHECK(!should_partial_relower_impact_checked_prod(0, 5, false),
          "AC4: clean window + Soft + ub=5 → false");
    CHECK(!should_partial_relower_impact_checked_prod(0, static_cast<std::size_t>(-1), true),
          "AC4: clean window + production + ub=SENTINEL → false");
}

// AC5: production_consult reflects production_defaults_active() ||
// AuditStrategy::Full; under apply_dev_audit_defaults() + Sampled,
// production_consult is false (Soft/Off zero-cost contract).
static void ac5_production_consult_reflects_strategy() {
    apply_production_audit_defaults();
    const bool production_prod =
        production_defaults_active() || get_strategy() == AuditStrategy::Full;
    CHECK(production_prod, "AC5: apply_production_audit_defaults → production_consult true");
    apply_dev_audit_defaults();
    const bool production_dev =
        production_defaults_active() || get_strategy() == AuditStrategy::Full;
    CHECK(!production_dev, "AC5: apply_dev_audit_defaults → production_consult false "
                           "(Sampled + dev_audit_opt_in=1, Soft zero-cost)");
    apply_dev_audit_defaults();
}

} // namespace

int run_test_issue_3310() {
    std::print("[test_issue_3310] running 5 ACs (production fail-closed on "
               "unknown ImpactScope)\n");

    ac1_production_unknown_impact_fail_closed();
    ac2_production_computable_ub_matches_existing();
    ac3_production_sentinel_minus_one();
    ac4_clean_window_zero_cost();
    ac5_production_consult_reflects_strategy();

    std::print("[test_issue_3310] passed={} failed={}\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_issue_3310();
}
#endif
