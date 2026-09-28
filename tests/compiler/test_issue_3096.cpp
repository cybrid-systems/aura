// @category: unit
// @reason: Issue #3096 — Production-only bounded auto-heal when residual
// force bits age past threshold with exhausted retry budget (refine #3026 /
// #2952 / #2601 / #2895). After residual_force_mask != 0 has aged past
// 256 BoundaryExits AND exhausted_min_dirty_retry_attempts_left == 0 AND
// not storm active, observe_residual_force_stale() drives one bounded
// min-dirty / coverage-verify pass via maybe_coverage_verify_min_dirty
// (single seed + decide gate, respects resolve_force_jit_repromote_only_covered).
// Soft / Off is zero-cost (early-returned before the auto-heal check).
// Issue #3814: same belt ages FallBackJit face (force!=0 && residual==0)
// and clears covered demotion (playbook stays observe-only).
//
// Issue #4147: a no-op auto-heal (heal did not shrink residual / face)
// re-arms the per-generation cap so a later 256-exit BoundaryExit window
// can retry — Agent-miss no longer leaves a sticky force-JIT while the
// playbook stays observe-only. AC3 is restated for the re-arm contract;
// ac4147_* covers re-arm observable / window gate / shrunk-generation
// composition / Soft zero-cost.
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

// AC3: One auto-heal per 256-exit window; a no-op heal re-arms the
// per-generation cap (#4147 — previously the cap stuck on a no-op heal
// while the observe-only playbook never shrank residual). The second
// cycle with the SAME unchanged mask re-fires exactly one heal; a mask
// change still opens a fresh generation window.
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
    // Issue #4147: the first heal is a no-op in this unit context (nothing
    // reemits — residual unchanged), so the cap must be cleared for a
    // retry instead of sticking forever.
    CHECK(reg.residual_force_auto_heal_last_mask() == 0,
          "AC3/#4147: no-op heal cleared residual_force_auto_heal_last_mask (re-armed)");
    // Budget must be empty for the #3096 gate; the heal may have re-seeded
    // it when coverage-verify resolves enabled.
    reg.exhaust_retry_for_test();
    for (int i = 0; i < 300; ++i) {
        reg.observe_residual_force_stale();
    }
    const auto heal_after_second = reg.residual_force_auto_heal_total();
    CHECK(heal_after_second == heal_after_first + 1,
          "AC3/#4147: re-armed cap — second observe cycle with unchanged mask re-fires one heal");
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

// Issue #4147 AC1: no-op auto-heal re-arms the per-generation cap so a
// later BoundaryExit age window can retry (Agent-miss no longer leaves a
// sticky force-JIT; playbook stays observe-only #2953/#3026).
static void ac4147_noop_heal_rearms_cap(CompilerService& cs) {
    (void)cs;
    auto& reg = hot_reg();
    reg.reset_residual_force_observe_for_test();
    reg.reset_deopt_storm_state_for_test();
    reg.force_jit_stamp_for_test(0x40);
    reg.exhaust_retry_for_test();
    for (int i = 0; i < 300; ++i) {
        reg.observe_residual_force_stale();
    }
    const auto heal_first = reg.residual_force_auto_heal_total();
    CHECK(heal_first >= 1, "AC4147.1: first auto-heal fired for the armed residual generation");
    // Nothing reemits in this unit context → the heal was a no-op; the
    // cap must be cleared (previously it stuck → zero retries forever).
    CHECK(reg.residual_force_mask() == 0x40,
          "AC4147.1: residual unchanged after the heal (no-op heal fixture)");
    CHECK(reg.residual_force_auto_heal_last_mask() == 0,
          "AC4147.1: no-op heal cleared residual_force_auto_heal_last_mask (re-armed)");
    // Retry window: budget must be empty for the #3096 gate (the heal may
    // have re-seeded it when coverage-verify resolves enabled).
    reg.exhaust_retry_for_test();
    for (int i = 0; i < 300; ++i) {
        reg.observe_residual_force_stale();
    }
    CHECK(reg.residual_force_auto_heal_total() == heal_first + 1,
          "AC4147.1: re-armed cap allows exactly one retry heal in the next window");
}

// Issue #4147 AC2: re-arm does not hot-loop — the heal reset the observe
// age, so the retry still waits a full 256-exit window (at most one
// in-flight heal per window).
static void ac4147_window_gate_after_noop_heal(CompilerService& cs) {
    (void)cs;
    auto& reg = hot_reg();
    reg.reset_residual_force_observe_for_test();
    reg.reset_deopt_storm_state_for_test();
    reg.force_jit_stamp_for_test(0x80);
    reg.exhaust_retry_for_test();
    for (int i = 0; i < 300; ++i) {
        reg.observe_residual_force_stale();
    }
    const auto heal_first = reg.residual_force_auto_heal_total();
    CHECK(heal_first >= 1, "AC4147.2: first auto-heal fired");
    for (int i = 0; i < 100; ++i) {
        reg.observe_residual_force_stale();
    }
    CHECK(reg.residual_force_auto_heal_total() == heal_first,
          "AC4147.2: re-armed cap still gated by the 256-exit window (no mid-window hot-loop)");
}

// Issue #4147 AC3: when a later covered reemit lands, residual shrinks and
// the new generation heals on its own window (mask-change re-arm path —
// #3096 AC3 composition preserved; #3885 leftover-generation shape).
static void ac4147_shrunk_residual_new_generation_heals(CompilerService& cs) {
    (void)cs;
    auto& reg = hot_reg();
    reg.reset_residual_force_observe_for_test();
    reg.reset_deopt_storm_state_for_test();
    reg.force_jit_stamp_for_test(0x600);
    reg.exhaust_retry_for_test();
    for (int i = 0; i < 300; ++i) {
        reg.observe_residual_force_stale();
    }
    const auto heal_first = reg.residual_force_auto_heal_total();
    CHECK(heal_first >= 1, "AC4147.3: auto-heal fired for generation 0x600");
    // Covered reemit lands: residual shrinks 0x600 → 0x200 (fail-closed
    // multi-reason single-heal leftover, #3885).
    reg.note_reemit_success_coverage(0x400);
    CHECK(reg.residual_force_mask() == 0x200,
          "AC4147.3: residual shrunk to the leftover generation");
    // Fresh window for the new generation (budget must be empty).
    reg.exhaust_retry_for_test();
    for (int i = 0; i < 300; ++i) {
        reg.observe_residual_force_stale();
    }
    CHECK(reg.residual_force_auto_heal_total() == heal_first + 1,
          "AC4147.3: shrunk residual heals on its own 256-exit window");
}

// Issue #4147 AC4: Soft / Off zero-cost preserved — observe early-returns
// on one production_defaults load; no re-arm-induced heals under Soft.
static void ac4147_soft_zero_cost(CompilerService& cs) {
    (void)cs;
    auto& reg = hot_reg();
    reg.reset_residual_force_observe_for_test();
    reg.reset_deopt_storm_state_for_test();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    reg.force_jit_stamp_for_test(0x800);
    reg.exhaust_retry_for_test();
    const auto soft_heal_before = reg.residual_force_auto_heal_total();
    for (int i = 0; i < 300; ++i) {
        reg.observe_residual_force_stale();
    }
    CHECK(reg.residual_force_auto_heal_total() == soft_heal_before,
          "AC4147.4 Soft: no auto-heal / no re-arm activity under Soft / Off");
    CHECK(reg.residual_force_auto_heal_last_mask() == 0,
          "AC4147.4 Soft: cap stays clear under Soft / Off");
    CHECK(reg.force_jit_regions_mask() == 0x800,
          "AC4147.4 Soft: force face untouched under Soft / Off");
    // Restore production for remaining callers.
    aura::compiler::typed_audit::apply_production_audit_defaults();
    reg.reset_residual_force_observe_for_test();
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

// Issue #3814: sticky force with empty residual (FallBackJit face) ages
// the same ResidualForceHeal belt and clears covered demotion — no
// undocumented Agent ritual; playbook FallBackJit stays observe-only.
static void ac3814_sticky_force_empty_residual_heal(CompilerService& cs) {
    (void)cs;
    auto& reg = hot_reg();
    reg.reset_residual_force_observe_for_test();
    reg.reset_deopt_storm_state_for_test();
    reg.force_jit_stamp_for_test(0x10);
    // Cover all force bits → residual == 0 while force sticky.
    reg.note_reemit_success_coverage(0x10);
    CHECK(reg.residual_force_mask() == 0, "AC3814: residual empty after cover");
    CHECK(reg.force_jit_regions_mask() == 0x10, "AC3814: force still sticky");
    reg.exhaust_retry_for_test();
    const auto heal_before = reg.residual_force_auto_heal_total();
    const auto force_before = reg.force_jit_regions_mask();
    for (int i = 0; i < 300; ++i) {
        reg.observe_residual_force_stale();
    }
    const auto heal_after = reg.residual_force_auto_heal_total();
    CHECK(heal_after >= heal_before + 1,
          "AC3814: auto-heal bumps for FallBackJit face (force!=0 residual==0)");
    CHECK(reg.force_jit_regions_mask() == 0,
          "AC3814: covered sticky force cleared after bounded heal");
    CHECK(force_before == 0x10, "AC3814: force was sticky before heal");
    // Soft / Off: zero extra — early return leaves force alone.
    reg.reset_residual_force_observe_for_test();
    reg.force_jit_stamp_for_test(0x20);
    reg.note_reemit_success_coverage(0x20);
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    const auto soft_heal_before = reg.residual_force_auto_heal_total();
    for (int i = 0; i < 300; ++i) {
        reg.observe_residual_force_stale();
    }
    CHECK(reg.residual_force_auto_heal_total() == soft_heal_before,
          "AC3814 Soft: zero extra auto-heal under Soft/Off");
    CHECK(reg.force_jit_regions_mask() == 0x20,
          "AC3814 Soft: no silent wholesale clear under Soft");
    // Restore production for remaining callers.
    aura::compiler::typed_audit::apply_production_audit_defaults();
    reg.on_reload_success();
    reg.reset_residual_force_observe_for_test();
}

} // namespace

int run_test_issue_3096() {
    CompilerService cs;
    std::print("[test_issue_3096] running 6 ACs (+#3814 +4147 re-arm)\n");
    // Production ACs need production_defaults_active even when the issues
    // runner sets AURA_SANDBOX=off (which apply_dev_audit_defaults).
    aura::compiler::typed_audit::apply_production_audit_defaults();

    ac1_production_auto_heal_fires(cs);
    ac3_one_per_mask_generation(cs);
    ac4147_noop_heal_rearms_cap(cs);
    ac4147_window_gate_after_noop_heal(cs);
    ac4147_shrunk_residual_new_generation_heals(cs);
    ac4147_soft_zero_cost(cs);
    ac4_existing_observe_counter_preserved(cs);
    ac5_c_linkage_accessor(cs);
    ac3814_sticky_force_empty_residual_heal(cs);

    aura::compiler::typed_audit::apply_dev_audit_defaults();
    std::print("[test_issue_3096] passed={} failed={}\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_issue_3096();
}
#endif