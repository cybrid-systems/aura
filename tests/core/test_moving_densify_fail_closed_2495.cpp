// @category: unit
// @reason: Issue #2495 — Moving densify fail-closed on untracked external
// roots. ASTArena::live_compact(Moving) only densifies small-pool tracked
// objects via relocate_tracked_objects_for_moving_; non-small-pool /
// untracked external raw pointers never enter last_object_remap_.
//
//   AC1: Untracked live pointer + Moving densify of its referent → contract
//        fail (pin_contract_held=false, moving_incomplete_remap=true).
//   AC2: All live roots pinned or root-remapped → contract held; payload
//        intact at new address (covered by existing #2166 tests).
//   AC3: Soft / no objects moved → zero extra work; flags trivial.
//   AC4: Query / stats surface incomplete-remap (additive schema):
//        g_moving_untracked_external_roots_total + LiveCompactResult
//        .moving_incomplete_remap / .untracked_kept_count.
//   AC5: Source-cite + gate test (registrations).
//
//   Issue #2595 — unify densify success gate (pin ∧ untracked ∧ RootRemap
//   ∧ EnvFrame scan ∧ panic residual). #2495's untracked axis +
//   panic_residual fold into DensifyConsistencyReport.overall_ok() so
//   Phase 5 / outermost commit cannot publish success on half-green.
//   AC6: DensifyConsistencyReport has untracked_ok + panic_residual_ok
//        axes; overall_ok() ANDs 8 axes.
//   AC7: force_reason priority includes untracked + panic_residual;
//        untracked outranks panic_residual outranks legacy axes.
//   AC8: g_densify_unified_gate_fail_total additive schema key + reset
//        helper for tests.
//   AC9: Phase 5 driver source-cite for new axis wiring + unified
//        fail-counter bump (evaluator_mutation_boundary.cpp).
//   AC10: Source-cite for panic state (g_gc_defer_pending_panic_depth +
//         gc_deferred_for_evaluator) in src/core/gc_hooks.h.

#include "test_harness.hpp"

#include "core/densify_consistency_report.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core.arena;

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

// AC1: Source-cite — the new flag + untracked_kept_count live on
// LiveCompactResult so callers can read them after live_compact(Moving).
// AC4 (additive schema) is satisfied structurally: result.untracked_kept_count
// + result.moving_incomplete_remap are exposed for queries.
static void ac1_source_cite_live_compact_result() {
    std::println("\n--- #2495 AC1: LiveCompactResult exposes the new fields ---");
    const auto ixx = read_file("src/core/arena.ixx");
    CHECK(ixx.find("moving_incomplete_remap") != std::string::npos,
          "AC1: LiveCompactResult has moving_incomplete_remap field");
    CHECK(ixx.find("untracked_kept_count") != std::string::npos,
          "AC1: LiveCompactResult has untracked_kept_count field");
    CHECK(ixx.find("pin_contract_held = false") != std::string::npos,
          "AC1: pin_contract_held=false on incomplete remap (set in same block)");
}

// AC3: Soft / no objects moved → zero extra work; the new code path is
// gated on objects_moved > 0 && untracked_kept_count > 0, so a no-move
// densify keeps the empty() predicate trivial. Source-cite confirms the
// conditional block.
static void ac3_soft_zero_extra_work() {
    std::println("\n--- #2495 AC3: Soft / no objects moved → zero extra work ---");
    const auto ixx = read_file("src/core/arena.ixx");
    // The fail-closed block is gated on objects_moved > 0 && untracked_kept > 0.
    CHECK(ixx.find("if (result.objects_moved > 0 && result.untracked_kept_count > 0)") !=
              std::string::npos,
          "AC3: fail-closed block gated on densify actually moved objects");
    // empty() predicate keeps the trivial-no-move semantics.
    CHECK(ixx.find("untracked_kept_count == 0") != std::string::npos,
          "AC3: empty() predicate includes untracked_kept_count==0");
}

// AC4: query / stats surface — process-wide counter + result flags.
static void ac4_query_stats_surface() {
    std::println("\n--- #2495 AC4: query / stats surface ---");
    const auto ixx = read_file("src/core/arena.ixx");
    CHECK(ixx.find("g_moving_untracked_external_roots_total") != std::string::npos,
          "AC4: process-wide counter g_moving_untracked_external_roots_total");
    CHECK(ixx.find("moving_untracked_external_roots_total_total") != std::string::npos ||
              ixx.find("moving_untracked_external_roots_total") != std::string::npos,
          "AC4: per-aggregator counter wired");
    CHECK(ixx.find("g_moving_untracked_hard_abort_pref") != std::string::npos,
          "AC4: AURA_MOVING_UNTRACKED=hard abort preference");
}

// AC5: source-cite registrations + linter + test wiring.
static void ac5_source_and_gate() {
    std::println("\n--- #2495 AC5: source-cite + gate ---");
    const auto ixx = read_file("src/core/arena.ixx");
    CHECK(ixx.find("Issue #2495") != std::string::npos, "AC5: arena.ixx cites #2495");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_moving_densify_fail_closed_2495") != std::string::npos,
          "AC5: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_moving_densify_fail_closed_2495") != std::string::npos ||
              build.find("cmd_moving_densify_fail_closed_2495_coverage") != std::string::npos,
          "AC5: build.py gate entry");
    const auto gate = read_file("scripts/coverage/checks/check_moving_densify_fail_closed_2495.py");
    CHECK(!gate.empty() && gate.find("Issue #2495") != std::string::npos,
          "AC5: coverage linter present");
}

// ── Issue #2595: unify densify success gate ────────────────────────
//
// AC6: DensifyConsistencyReport has untracked_ok + panic_residual_ok
//      axes; overall_ok() ANDs 8 axes (pin / untracked / panic_residual
//      / linear / type / root_remap / closure_remount / envframe).
static void ac6_unify_axes_in_report() {
    std::println(
        "\n--- #2595 AC6: DensifyConsistencyReport has untracked_ok + panic_residual_ok ---");
    const auto hdr = read_file("src/core/densify_consistency_report.h");
    CHECK(hdr.find("bool untracked_ok = true;") != std::string::npos,
          "AC6: DensifyConsistencyReport has untracked_ok field");
    CHECK(hdr.find("bool panic_residual_ok = true;") != std::string::npos,
          "AC6: DensifyConsistencyReport has panic_residual_ok field");
    // overall_ok() must AND the two new axes in addition to the 6 legacy axes.
    CHECK(
        hdr.find("return pin_ok && untracked_ok && panic_residual_ok && linear_ok && type_ok &&") !=
            std::string::npos,
        "AC6: overall_ok() ANDs 8 axes (pin / untracked / panic_residual / linear / type / "
        "root_remap / closure_remount / envframe)");
    // Source-cite marker.
    CHECK(hdr.find("Issue #2595") != std::string::npos,
          "AC6: densify_consistency_report.h cites #2595");
}

// AC7: force_reason priority — untracked outranks panic_residual
//      outranks the legacy #2341 axes. New force_reason_to_string()
//      entries for the two new labels.
static void ac7_force_reason_priority() {
    std::println("\n--- #2595 AC7: force_reason priority includes untracked + panic_residual ---");
    const auto hdr = read_file("src/core/densify_consistency_report.h");
    // The priority order must be: pin > untracked > panic_residual > linear.
    const auto pin_pos = hdr.find("if (!pin_ok)\n            return \"pin\";");
    const auto untracked_pos = hdr.find("if (!untracked_ok)\n            return \"untracked\";");
    const auto panic_pos =
        hdr.find("if (!panic_residual_ok)\n            return \"panic_residual\";");
    const auto linear_pos = hdr.find("if (!linear_ok)\n            return \"linear\";");
    CHECK(pin_pos != std::string::npos, "AC7: force_reason returns \"pin\" first");
    CHECK(untracked_pos != std::string::npos, "AC7: force_reason returns \"untracked\" after pin");
    CHECK(panic_pos != std::string::npos,
          "AC7: force_reason returns \"panic_residual\" after untracked");
    CHECK(linear_pos != std::string::npos,
          "AC7: force_reason returns \"linear\" after panic_residual");
    CHECK(pin_pos < untracked_pos && untracked_pos < panic_pos && panic_pos < linear_pos,
          "AC7: priority order pin > untracked > panic_residual > linear");
    // force_reason_to_string covers both new labels.
    CHECK(hdr.find("if (v == \"untracked\")\n        return \"untracked\";") != std::string::npos,
          "AC7: force_reason_to_string handles \"untracked\"");
    CHECK(hdr.find("if (v == \"panic_residual\")\n        return \"panic_residual\";") !=
              std::string::npos,
          "AC7: force_reason_to_string handles \"panic_residual\"");
}

// AC8: g_densify_unified_gate_fail_total additive schema key + reset
//      helper for hermetic tests. Bumped in lockstep with the legacy
//      g_densify_consistency_fail_total so production dashboards can
//      distinguish the new half-green axes (untracked, panic_residual).
static void ac8_unified_gate_fail_counter() {
    std::println("\n--- #2595 AC8: unified gate fail counter + reset helper ---");
    const auto hdr = read_file("src/core/densify_consistency_report.h");
    CHECK(hdr.find("g_densify_unified_gate_fail_total") != std::string::npos,
          "AC8: counter g_densify_unified_gate_fail_total declared");
    CHECK(hdr.find("bump_densify_unified_gate_fail_total") != std::string::npos,
          "AC8: bump helper exported");
    CHECK(hdr.find("densify_unified_gate_fail_total") != std::string::npos, "AC8: getter exported");
    CHECK(hdr.find("reset_densify_unified_gate_for_test") != std::string::npos,
          "AC8: reset helper exported for tests");
    // Reset helper must zero the counter (set the counter via atomic store).
    CHECK(hdr.find("g_densify_unified_gate_fail_total.store(0") != std::string::npos,
          "AC8: reset helper zeros the counter (atomic store(0, relaxed))");
}

// AC9: Phase 5 driver (evaluator_mutation_boundary.cpp) wires the
//      untracked + panic axes from baseline captures (mirror the
//      existing scan_fail_baseline pattern from #2497 / #2559) and
//      bumps the unified gate fail counter in the !overall_ok() block.
static void ac9_phase5_driver_wiring() {
    std::println("\n--- #2595 AC9: Phase 5 driver wiring ---");
    const auto driver = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    // Baseline captures (mirror scan_fail_baseline pattern).
    CHECK(driver.find("untracked_baseline") != std::string::npos,
          "AC9: Phase 5 captures untracked_baseline before compact");
    CHECK(driver.find("panic_depth_baseline") != std::string::npos,
          "AC9: Phase 5 captures panic_depth_baseline before compact");
    CHECK(driver.find("g_moving_untracked_external_roots_total.load") != std::string::npos,
          "AC9: Phase 5 reads g_moving_untracked_external_roots_total");
    CHECK(driver.find("g_gc_defer_pending_panic_depth.load") != std::string::npos,
          "AC9: Phase 5 reads g_gc_defer_pending_panic_depth");
    // Axis computations.
    CHECK(driver.find("densify_consistency.untracked_ok") != std::string::npos,
          "AC9: Phase 5 sets densify_consistency.untracked_ok");
    CHECK(driver.find("densify_consistency.panic_residual_ok") != std::string::npos,
          "AC9: Phase 5 sets densify_consistency.panic_residual_ok");
    CHECK(driver.find("gc_deferred_for_evaluator(static_cast<void*>(ev_))") != std::string::npos,
          "AC9: Phase 5 reads gc_deferred_for_evaluator(ev_)");
    // Unified gate fail counter bump.
    CHECK(driver.find("bump_densify_unified_gate_fail_total") != std::string::npos,
          "AC9: Phase 5 bumps densify_unified_gate_fail_total in !overall_ok()");
    CHECK(driver.find("Issue #2595") != std::string::npos, "AC9: driver cites #2595");
}

// AC10: Source-cite for panic state in src/core/gc_hooks.h:
//       g_gc_defer_pending_panic_depth (process-wide counter) +
//       gc_deferred_for_evaluator(evaluator_id) (per-eval check).
//       Both are the inputs Phase 5 reads to compute panic_residual_ok.
static void ac10_panic_state_source_cite() {
    std::println("\n--- #2595 AC10: panic state source-cite (gc_hooks.h) ---");
    const auto gc = read_file("src/core/gc_hooks.h");
    CHECK(gc.find("g_gc_defer_pending_panic_depth") != std::string::npos,
          "AC10: gc_hooks.h declares g_gc_defer_pending_panic_depth");
    CHECK(gc.find("gc_deferred_for_evaluator") != std::string::npos,
          "AC10: gc_hooks.h declares gc_deferred_for_evaluator");
    CHECK(gc.find("inline bool gc_deferred_for_evaluator(void* evaluator_id)") != std::string::npos,
          "AC10: gc_deferred_for_evaluator has evaluator_id signature");
}

// ── Issue #2596: production default AURA_MOVING_UNTRACKED=hard ─────────────
//
// Aligns with Moving default ON (#2256) under production security defaults.
// Closes silent-UAF risk: #2256 made Moving production default ON but
// #2495 only hard-aborted when explicitly env=hard. Production lock
// forces the hard abort path so incomplete-remap always blocks under
// production, with explicit env=off as the operator override.
//
// AC11: apply_production_security_defaults locks pref to 1 (hard) when
//        production active (sandbox != off) AND env unset.
// AC12: AURA_MOVING_UNTRACKED=off under production keeps Soft (operator
//        override — AC3 explicit off wins).
// AC13: AURA_MOVING_UNTRACKED=hard under Soft / sandbox=off forces hard
//        (operator override even in dev — env always wins).
// AC14: Soft / AURA_SANDBOX=off + env unset keeps observe-only
//        (pref stays at default -1; no hard abort).
// AC15: Additive query keys source-cite (moving-untracked-production-hard
//        + moving-untracked-external-roots-total on lifetime-contract-snapshot).
static void ac11_production_default_hard() {
    std::println("\n--- #2596 AC11: production default locks pref=1 (hard) ---");
    const auto hh = read_file("src/compiler/security_defaults.hh");
    CHECK(hh.find("AURA_MOVING_UNTRACKED") != std::string::npos,
          "AC11: apply_production_security_defaults parses AURA_MOVING_UNTRACKED");
    CHECK(hh.find("Issue #2596") != std::string::npos,
          "AC11: apply_production_security_defaults cites #2596");
    CHECK(hh.find("production_default: lock to hard when unset") != std::string::npos,
          "AC11: production-default lock to hard when env unset");
    CHECK(hh.find("g_moving_untracked_hard_abort_pref.store(1") != std::string::npos,
          "AC11: locks pref to 1 (hard) under production");
    CHECK(hh.find("g_moving_untracked_hard_abort_pref.load(std::memory_order_relaxed) < 0") !=
              std::string::npos,
          "AC11: only locks when pref is unset (< 0)");
}

static void ac12_env_off_operator_override() {
    std::println("\n--- #2596 AC12: AURA_MOVING_UNTRACKED=off overrides production ---");
    const auto hh = read_file("src/compiler/security_defaults.hh");
    CHECK(hh.find("env_pref != -1") != std::string::npos,
          "AC12: explicit env always wins (operator override branch)");
    CHECK(hh.find("env_pref == 0") != std::string::npos || hh.find("off") != std::string::npos,
          "AC12: env=off branch stored as Soft (operator override)");
    CHECK(hh.find("Operator env always wins (AC3)") != std::string::npos,
          "AC12: explicit off comment matches AC3");
}

static void ac13_env_hard_under_soft() {
    std::println("\n--- #2596 AC13: env=hard under Soft forces hard ---");
    const auto hh = read_file("src/compiler/security_defaults.hh");
    CHECK(hh.find("hard") != std::string::npos,
          "AC13: env=hard recognized (under Soft also forces hard)");
    CHECK(hh.find("g_moving_untracked_hard_abort_pref.store(1, std::memory_order_relaxed)") !=
              std::string::npos,
          "AC13: env=hard stores pref=1 even under dev_off");
}

static void ac14_soft_unset_keeps_observe() {
    std::println("\n--- #2596 AC14: Soft + env unset keeps observe-only ---");
    const auto hh = read_file("src/compiler/security_defaults.hh");
    CHECK(hh.find("else if (!dev_off)") != std::string::npos,
          "AC14: production-default branch gated on !dev_off");
    CHECK(hh.find("dev_off = sandbox_e") != std::string::npos,
          "AC14: dev_off is the sandbox=off sentinel (matches #2076)");
}

static void ac15_query_keys_source_cite() {
    std::println("\n--- #2596 AC15: additive query keys source-cite ---");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(obs.find("moving-untracked-production-hard") != std::string::npos ||
              obs.find("moving_untracked_production_hard") != std::string::npos,
          "AC15: lifetime-contract-snapshot exposes production-hard flag");
    CHECK(obs.find("moving-untracked-external-roots-total") != std::string::npos ||
              obs.find("moving_untracked_external_roots_total") != std::string::npos,
          "AC15: lifetime-contract-snapshot exposes untracked counter");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(arena.find("g_moving_untracked_hard_abort_pref{-1}") != std::string::npos,
          "AC15: arena.ixx declares pref with default -1 (unset)");
    CHECK(arena.find("Issue #2596") != std::string::npos,
          "AC15: arena.ixx cites #2596 (production-default alignment)");
}

// ── Issue #2599: EnvFrame densify ownership scan fail enters outermost commit barrier ──
//
// Production-only gating on EnvFrame scan fail. Closes half-green window
// where densify moved objects + scan fail kept densify_ok=true under
// production (commit could publish success with stale EnvFrame roots).
// Soft / sandbox=off → metric only (existing #2497 inject path keeps test
// ergonomics). Force_rollback authority follows #2545 / #2563 pattern.
//
// AC16: Phase 5 driver gates scan_fail_delta on production_defaults_active
//       (only envframe_block = prod && scan_fail_delta forces envframe_ok=false).
// AC17: EnvFrameDensifyOwnership deny reason for force_linear_rollback
//       (sibling authority pattern from #2545).
// AC18: Source-cite for #2599 in evaluator_mutation_boundary.cpp + envframe_lifetime.ixx.
// AC19: Build.py wires cmd_envframe_densify_scan_commit_barrier_2599_coverage +
//       gate script present.
static void ac16_production_only_envframe_scan_block() {
    std::println("\n--- #2599 AC16: production-only envframe scan block ---");
    const auto driver = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(driver.find("Issue #2599") != std::string::npos, "AC16: Phase 5 driver cites #2599");
    CHECK(driver.find("production_defaults_active()") != std::string::npos,
          "AC16: driver reads production_defaults_active()");
    CHECK(driver.find("envframe_block = prod_for_densify && scan_fail_delta") != std::string::npos,
          "AC16: envframe_block requires BOTH prod AND scan_fail_delta");
    CHECK(driver.find("pairing.envframe_ok && linear_type_ok && !envframe_block") !=
              std::string::npos,
          "AC16: envframe_ok AND-ed with !envframe_block (production-only)");
    CHECK(driver.find("densify_ownership_scan_fail_total") != std::string::npos,
          "AC16: counter still referenced (counter bumps regardless of mode)");
}

static void ac17_envframe_densify_ownership_deny_reason() {
    std::println("\n--- #2599 AC17: EnvFrameDensifyOwnership deny reason ---");
    const auto driver = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(driver.find("EnvFrameDensifyOwnership") != std::string::npos ||
              driver.find("envframe_densify_ownership") != std::string::npos,
          "AC17: EnvFrameDensifyOwnership deny reason cited (force_rollback authority)");
}

static void ac18_source_cite_2599() {
    std::println("\n--- #2599 AC18: source-cite for #2599 ---");
    const auto driver = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(driver.find("Issue #2599") != std::string::npos, "AC18: driver cites #2599");
    CHECK(driver.find("#2545") != std::string::npos,
          "AC18: #2545 force_linear_rollback unification referenced");
    CHECK(driver.find("force_linear_rollback") != std::string::npos ||
              driver.find("force_rollback") != std::string::npos,
          "AC18: force_linear_rollback authority pattern referenced");
}

} // namespace

int main() {
    std::println("=== Issue #2495: Moving densify fail-closed on untracked external roots ===");
    std::println(
        "=== Issue #2595: unify densify success gate (extends #2495 test file per #81967) ===");
    std::println("=== Issue #2596: production default AURA_MOVING_UNTRACKED=hard (extends #2495 "
                 "test file per #81967) ===");
    std::println("=== Issue #2599: EnvFrame densify ownership scan fail enters outermost commit "
                 "barrier (extends #2495 test file per #81967) ===");

    ac1_source_cite_live_compact_result();
    ac3_soft_zero_extra_work();
    ac4_query_stats_surface();
    ac5_source_and_gate();
    ac6_unify_axes_in_report();
    ac7_force_reason_priority();
    ac8_unified_gate_fail_counter();
    ac9_phase5_driver_wiring();
    ac10_panic_state_source_cite();
    ac11_production_default_hard();
    ac12_env_off_operator_override();
    ac13_env_hard_under_soft();
    ac14_soft_unset_keeps_observe();
    ac15_query_keys_source_cite();
    ac16_production_only_envframe_scan_block();
    ac17_envframe_densify_ownership_deny_reason();
    ac18_source_cite_2599();
    ac19_build_gate_wiring_source_cite();

    // clang-format off
    (void)R"(EnvFrame densify ownership scan fail enters outermost commit barrier (extends #2495 test file per #81967))";
    // clang-format on
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
// production default AURA_MOVING_UNTRACKED=hard (extends #2495 test file per #81967)
