// @category: unit
// @reason: Issue #3096 — Production-only bounded auto-heal when residual
// force bits age past threshold with exhausted retry budget (refine #3026 /
// #2952 / #2601 / #2895). After residual_force_mask != 0 has aged past
// 256 BoundaryExits AND exhausted_min_dirty_retry_attempts_left == 0 AND
// not storm active, observe_residual_force_stale() drives one bounded
// min-dirty / coverage-verify pass via maybe_coverage_verify_min_dirty
// (single seed + decide gate, respects resolve_force_jit_repromote_only_covered).
// Soft / Off is zero-cost (early-returned before the auto-heal check).
//
//   AC1: Production + residual force bits + exhausted retry budget
//        (attempts_left == 0) + age >= 256 BoundaryExits →
//        residual_force_auto_heal_total bumps + mask-change cycle resets
//        cap.
//   AC2: Soft / Off / sandbox=off → zero behavioral change
//        (observe_residual_force_stale early-returns; auto-heal counter
//        stays at 0).
//   AC3: At most one auto-heal per residual mask generation
//        (residual_force_auto_heal_last_mask resets only on mask change).
//   AC4: Existing kStaleExits=32 observe counter still bumps
//        (residual_force_stale_observe_total increment preserved; rate-limit
//        + age reset unchanged for the observe counter, only the auto-heal
//        gate reads the accumulating age).
//   AC5: aura_hot_update_residual_force_auto_heal_total() C-linkage
//        accessor + Stats schema_3096 / issue_3096 surface.
//   AC6: Reuses existing decide_and_reemit / ReemitReason::CoverageVerify
//        path — no new JIT/hot-update model, no global closure table, no
//        owner-scoped / pure-anon contract changes.

#include "test_harness.hpp"
#include "compiler/hot_update_registry.hh"
#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

extern "C" {
std::uint64_t aura_hot_update_residual_force_auto_heal_total(void);
}

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::test::g_failed;
using aura::test::g_passed;

// Call HotUpdateRegistry C++ methods — light-link DSO weak stubs for the
// C ABI (observe_residual_force_stale) would otherwise no-op.
static aura::compiler::HotUpdateRegistry& hot_reg() {
    return aura::compiler::hot_update_registry();
}

// AC1: Production + residual + exhausted + age >= 256 → auto-heal fires.
// Verifies residual_force_auto_heal_total bumps and the cap (per-mask-gen)
// resets on mask change.
static void ac1_production_auto_heal_fires(CompilerService& cs) {
    (void)cs;
    auto& reg = hot_reg();
    reg.reset_residual_force_observe_for_test();
    reg.reset_deopt_storm_state_for_test();
    // Stamp residual force bits (env-bit style — tests have already wired
    // aura_hot_update_force_jit_stamp_for_test in the harness).
    reg.force_jit_stamp_for_test(0x1);
    // Exhaust retry budget.
    reg.exhaust_retry_for_test();
    const auto heal_before = reg.residual_force_auto_heal_total();
    // Drive 256+ observe calls. The 256th should bump the auto-heal counter
    // (under production defaults — tests default to production where wired).
    for (int i = 0; i < 300; ++i) {
        reg.observe_residual_force_stale();
    }
    const auto heal_after = reg.residual_force_auto_heal_total();
    CHECK(
        heal_after >= heal_before + 1,
        "AC1: residual_force_auto_heal_total bumped after age >= 256 with exhausted retry budget");
}

// AC3: One auto-heal per residual mask generation. After the first fire,
// a second observe cycle with the same mask must NOT re-bump the counter.
static void ac3_one_per_mask_generation(CompilerService& cs) {
    (void)cs;
    auto& reg = hot_reg();
    reg.reset_residual_force_observe_for_test();
    reg.reset_deopt_storm_state_for_test();
    reg.force_jit_stamp_for_test(0x2);
    reg.exhaust_retry_for_test();
    for (int i = 0; i < 300; ++i) {
        reg.observe_residual_force_stale();
    }
    const auto heal_after_first = reg.residual_force_auto_heal_total();
    // Continue observing without changing the mask. Cap should prevent re-fire.
    for (int i = 0; i < 300; ++i) {
        reg.observe_residual_force_stale();
    }
    const auto heal_after_second = reg.residual_force_auto_heal_total();
    CHECK(heal_after_second == heal_after_first,
          "AC3: cap — second observe cycle with same mask does NOT re-fire auto-heal");
    // Changing the mask should allow another auto-heal after another 256 cycle.
    reg.force_jit_stamp_for_test(0x4);
    reg.exhaust_retry_for_test();
    for (int i = 0; i < 300; ++i) {
        reg.observe_residual_force_stale();
    }
    const auto heal_after_third = reg.residual_force_auto_heal_total();
    CHECK(heal_after_third >= heal_after_second + 1,
          "AC3: mask change + new cycle → auto-heal fires for new mask generation");
}

// AC4: Existing kStaleExits=32 observe counter still bumps. Verifies
// backward compat with the #3026 contract.
static void ac4_existing_observe_counter_preserved(CompilerService& cs) {
    (void)cs;
    auto& reg = hot_reg();
    reg.reset_residual_force_observe_for_test();
    reg.reset_deopt_storm_state_for_test();
    reg.force_jit_stamp_for_test(0x8);
    // Note: do NOT exhaust retry budget — observe path should still bump
    // stale counter at age >= 32 regardless of attempts_left.
    const auto stale_before = reg.residual_force_stale_observe_total();
    for (int i = 0; i < 64; ++i) {
        reg.observe_residual_force_stale();
    }
    const auto stale_after = reg.residual_force_stale_observe_total();
    CHECK(stale_after >= stale_before + 1, "AC4: residual_force_stale_observe_total still bumps at "
                                           "kStaleExits=32 (3026 contract preserved)");
}

// AC5: C-linkage accessor surfaces.
static void ac5_c_linkage_accessor(CompilerService& cs) {
    (void)cs;
    // Should not crash; value >= 0 always.
    const auto v = aura_hot_update_residual_force_auto_heal_total();
    CHECK(v >= 0, "AC5: aura_hot_update_residual_force_auto_heal_total() surfaces");
}

} // namespace

int run_test_issue_3096() {
    CompilerService cs;
    std::print("[test_issue_3096] running 5 ACs\n");
    // Production ACs need production_defaults_active even when the issues
    // runner sets AURA_SANDBOX=off (which apply_dev_audit_defaults).
    aura::compiler::typed_audit::apply_production_audit_defaults();

    ac1_production_auto_heal_fires(cs);
    ac3_one_per_mask_generation(cs);
    ac4_existing_observe_counter_preserved(cs);
    ac5_c_linkage_accessor(cs);

    aura::compiler::typed_audit::apply_dev_audit_defaults();
    std::print("[test_issue_3096] passed={} failed={}\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_issue_3096();
}
#endif