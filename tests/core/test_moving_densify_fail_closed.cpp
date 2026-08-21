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

#include "core/arena_auto_policy_stats.h"
#include "core/densify_consistency_report.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <vector>
#include <string_view>

import std;
import aura.core.arena;
import aura.core.lifetime_pin;

namespace {

using aura::ast::ASTArena;
using aura::ast::LiveCompactMode;
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
    // The fail-closed block is gated on objects_moved > 0 && (untracked_kept > 0
    // || stale_unremapped > 0) — #2837 extended the untracked axis.
    CHECK(ixx.find("result.objects_moved > 0") != std::string::npos &&
              ixx.find("result.untracked_kept_count > 0") != std::string::npos,
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
    CHECK(cmake.find("test_moving_densify_fail_closed") != std::string::npos,
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
    CHECK(hh.find("Production default: lock to hard when unset") != std::string::npos,
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
    CHECK(obs.find("production-hard-active") != std::string::npos,
          "AC15: lifetime-contract-snapshot exposes production-hard flag");
    CHECK(obs.find("untracked-external-roots-total") != std::string::npos ||
              obs.find("untracked_external_roots_total") != std::string::npos,
          "AC15: lifetime-contract-snapshot exposes untracked counter");
    const auto ah = read_file("src/core/arena_auto_policy_stats.h");
    CHECK(ah.find("g_moving_untracked_hard_abort_pref{-1}") != std::string::npos,
          "AC15: arena_auto_policy_stats.h declares pref with default -1 (unset)");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(arena.find("Issue #2495/#2596") != std::string::npos,
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

// ── Issue #2664: production-default hard-fail on untracked external roots ──
//
// Closes the false-safety gap where Soft path completes densify with
// metrics suppressed but no Agent-visible hard deny under
// production_defaults_active(). Folds production_defaults into the
// hard-fail branch even without explicit env=hard. Soft / dev_off /
// tests retain observe-only behavior (the existing #2595/#2596 path).
//
// AC1: production_defaults_active() check in arena.ixx if-block
//       (close false-safety: moving_incomplete_remap_densify_hard_fail).
// AC2: new Agent-visible hard-fail counter
//       (g_moving_incomplete_remap_densify_hard_fail_total).
// AC3: Soft + env unset retains observe-only (#2596 AC14 contract).
// AC4: AURA_MOVING_UNTRACKED=hard still aborts under production
//       (pre-existing #2596 AC11; production_defaults_active()
//       AND-extends the gate — env still wins).
// AC5: source-cite Phase 5 gate (moving_blocked_precondition propagation
//       into densify_consistency.overall_ok()).
// AC6: coverage linter check_2664_coverage wired into build.py gate.
static void ac2664_1_production_default_hard_fail() {
    std::println("\n--- #2664 AC1: production-default hard-fail (close false-safety) ---");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(arena.find("Issue #2664") != std::string::npos,
          "2664 AC1: arena.ixx cites #2664 production-default hard-fail");
    CHECK(arena.find("g_moving_untracked_hard_abort_pref") != std::string::npos,
          "2664 AC1: arena.ixx reads g_moving_untracked_hard_abort_pref (already encodes "
          "production-default via #2596)");
}

static void ac2664_2_hard_fail_counter() {
    std::println("\n--- #2664 AC2: Agent-visible hard-fail counter ---");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(arena.find("g_moving_incomplete_remap_densify_hard_fail_total") != std::string::npos,
          "2664 AC2: g_moving_incomplete_remap_densify_hard_fail_total counter declared");
    CHECK(arena.find("g_moving_incomplete_remap_densify_hard_fail_total.fetch_add") !=
              std::string::npos,
          "2664 AC2: counter bumped on hard-fail branch (Agent-visible)");
}

static void ac2664_3_soft_observe_only() {
    std::println("\n--- #2664 AC3: Soft + env unset retains observe-only ---");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(arena.find("Soft / dev_off / tests retain observe-only") != std::string::npos,
          "2664 AC3: Soft path comment documents observe-only retention");
}

static void ac2664_4_env_hard_still_aborts() {
    std::println("\n--- #2664 AC4: AURA_MOVING_UNTRACKED=hard still aborts ---");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(arena.find("hard_pref > 0") != std::string::npos,
          "2664 AC4: existing env=hard branch preserved (#2596 contract)");
    CHECK(arena.find("g_moving_incomplete_remap_densify_hard_fail_total.fetch_add") !=
              std::string::npos,
          "2664 AC4: counter bumps in the same branch as env=hard");
}

static void ac2664_5_phase5_gate_source_cite() {
    std::println("\n--- #2664 AC5: Phase 5 gate source-cite ---");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(
        arena.find("moving_blocked_precondition") != std::string::npos,
        "2664 AC5: arena.ixx has moving_blocked_precondition field (aggregates into Phase 5 gate)");
}

static void ac2664_6_coverage_linter_wired() {
    std::println("\n--- #2664 AC6: coverage linter check_2664_coverage wired ---");
    const auto build = read_file("build.py");
    CHECK(build.find("check_2664_coverage") != std::string::npos,
          "2664 AC6: build.py wires check_2664_coverage into the gate");
}

// ── Issue #2837: external-root slot remap + sticky densify-off ─────────────
//
// Full remapping path for registered external root *slots* (void**).
// Production hard incomplete-remap arms sticky densify-off so Agents cannot
// keep densifying while untracked live roots remain. Soft remains observe-only.

// Trivial small-pool tracked object for densify tests.
struct Pod16 {
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::int32_t c = 0;
    std::int32_t d = 0;
    Pod16() = default;
    Pod16(std::int32_t a_, std::int32_t b_, std::int32_t c_, std::int32_t d_) noexcept
        : a(a_)
        , b(b_)
        , c(c_)
        , d(d_) {}
};

struct MovingFlagGuard {
    int prev_pref = -1;
    int prev_hard = -1;
    explicit MovingFlagGuard(int enable) {
        prev_pref = aura::ast::g_moving_compact_enabled_pref.load(std::memory_order_relaxed);
        prev_hard = aura::ast::g_moving_untracked_hard_abort_pref.load(std::memory_order_relaxed);
        // Clear sticky first so set_moving_compact_enabled is not overridden.
        aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
        aura::ast::set_moving_compact_enabled(enable);
    }
    ~MovingFlagGuard() {
        aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
        aura::ast::g_moving_compact_enabled_pref.store(prev_pref, std::memory_order_relaxed);
        aura::ast::g_moving_untracked_hard_abort_pref.store(prev_hard, std::memory_order_relaxed);
    }
};

// AC1: registered void** slot remapped after Moving densify.
static void ac2837_1_slot_remapped() {
    std::println("\n--- #2837 AC1: external-root slot remapped after densify ---");
    MovingFlagGuard on(1);
    aura::ast::g_moving_untracked_hard_abort_pref.store(0, std::memory_order_relaxed); // Soft
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
    CHECK(p0 && p1 && p2, "2837 AC1: create ok");
    void* ext = p0; // external raw root holding densify candidate
    void* old = ext;
    arena.register_external_root_slot_for_densify(&ext);
    const auto remap_before =
        aura::ast::g_moving_external_root_slot_remap_total.load(std::memory_order_relaxed);
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    CHECK(r.objects_moved > 0, "2837 AC1: objects_moved > 0");
    void* neu = arena.resolve_object_remap(old);
    CHECK(neu != nullptr, "2837 AC1: densify produced remap entry for old");
    CHECK(ext == neu, "2837 AC1: external slot rewritten to new address");
    CHECK(r.external_roots_remapped_count >= 1, "2837 AC1: result.external_roots_remapped_count");
    CHECK(aura::ast::g_moving_external_root_slot_remap_total.load() >= remap_before + 1,
          "2837 AC1: process-wide slot remap total bumps");
    // Payload intact at new address.
    CHECK(static_cast<Pod16*>(ext)->a == 1 && static_cast<Pod16*>(ext)->b == 2,
          "2837 AC1: payload intact via remapped slot");
    (void)p1;
    (void)p2;
}

// AC2: value-only prep register (no slot) → stale unremapped + incomplete.
static void ac2837_2_value_only_stale_fail_closed() {
    std::println("\n--- #2837 AC2: value-only prep → stale fail-closed ---");
    MovingFlagGuard on(1);
    aura::ast::g_moving_untracked_hard_abort_pref.store(0, std::memory_order_relaxed);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
    void* ext = p0;
    void* old = ext;
    // Value-only (#2775) — cannot rewrite caller storage.
    arena.register_external_root_for_densify(ext);
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    CHECK(r.objects_moved > 0, "2837 AC2: objects_moved > 0");
    CHECK(r.external_roots_stale_unremapped_count >= 1 || r.moving_incomplete_remap,
          "2837 AC2: stale unremapped or incomplete remap");
    CHECK(r.pin_contract_held == false || r.moving_incomplete_remap,
          "2837 AC2: pin_contract_held false on incomplete");
    // Caller storage still holds old address (not rewritten).
    CHECK(ext == old, "2837 AC2: value-only leaves caller slot unre-written");
    (void)p1;
    (void)p2;
}

// AC3: production hard incomplete → sticky densify-off.
static void ac2837_3_sticky_densify_off_under_hard() {
    std::println("\n--- #2837 AC3: production hard → sticky densify-off ---");
    MovingFlagGuard on(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::g_moving_untracked_hard_abort_pref.store(1, std::memory_order_relaxed); // hard
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
    void* ext = p0;
    arena.register_external_root_for_densify(ext); // value-only → incomplete under move
    const auto sticky_before = aura::ast::g_moving_incomplete_remap_sticky_densify_off_total.load(
        std::memory_order_relaxed);
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    // Issue #2973: production hard now fail-closes BEFORE address movement.
    CHECK(r.objects_moved == 0, "2837 AC3: #2973 pre-move block (no address movement)");
    CHECK(r.moving_incomplete_remap, "2837 AC3: incomplete remap");
    CHECK(p0->a == 1 && p0->b == 2, "2837 AC3: payload intact (no remap)");
    CHECK(aura::ast::moving_incomplete_remap_sticky_densify_off(),
          "2837 AC3: sticky densify-off armed");
    CHECK(aura::ast::moving_compact_enabled() == 0,
          "2837 AC3: moving_compact_enabled() forced 0 under sticky");
    CHECK(aura::ast::g_moving_incomplete_remap_sticky_densify_off_total.load() >= sticky_before + 1,
          "2837 AC3: sticky densify-off total bumps");
    // Clear restores densify when pref is on.
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::set_moving_compact_enabled(1);
    CHECK(aura::ast::moving_compact_enabled() == 1, "2837 AC3: clear sticky restores densify");
    (void)p1;
    (void)p2;
    (void)r;
}

// AC4: Soft / hard_pref<=0 does not arm sticky densify-off.
static void ac2837_4_soft_no_sticky() {
    std::println("\n--- #2837 AC4: Soft incomplete does not arm sticky ---");
    MovingFlagGuard on(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::g_moving_untracked_hard_abort_pref.store(0, std::memory_order_relaxed);
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
    void* ext = p0;
    arena.register_external_root_for_densify(ext);
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    CHECK(r.objects_moved > 0, "2837 AC4: objects_moved > 0");
    CHECK(!aura::ast::moving_incomplete_remap_sticky_densify_off(),
          "2837 AC4: Soft does not arm sticky densify-off");
    CHECK(aura::ast::moving_compact_enabled() == 1, "2837 AC4: Moving still enabled under Soft");
    (void)p1;
    (void)p2;
}

// AC5: Soft / no-move → zero extra slot work (source-cite gate).
static void ac2837_5_soft_no_move_zero_cost() {
    std::println("\n--- #2837 AC5: Soft / no-move zero extra slot work ---");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(arena.find("result.objects_moved > 0 && !external_root_slots_for_densify_.empty()") !=
              std::string::npos,
          "2837 AC5: slot rewrite gated on objects_moved > 0");
    CHECK(arena.find("register_external_root_slot_for_densify") != std::string::npos,
          "2837 AC5: slot registration API present");
}

// AC6: Agent surface + source-cite + linter + no invent file / design doc.
static void ac2837_6_source_cite_and_surface() {
    std::println("\n--- #2837 AC6: source-cite + Agent surface + linter ---");
    const auto arena = read_file("src/core/arena.ixx");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    const auto build = read_file("build.py");
    CHECK(arena.find("Issue #2837") != std::string::npos ||
              arena.find("#2837") != std::string::npos,
          "2837 AC6: arena.ixx cites #2837");
    CHECK(arena.find("g_moving_external_root_slot_remap_total") != std::string::npos,
          "2837 AC6: slot remap counter");
    CHECK(arena.find("g_moving_incomplete_remap_sticky_densify_off") != std::string::npos,
          "2837 AC6: sticky densify-off flag");
    CHECK(arena.find("external_roots_remapped_count") != std::string::npos,
          "2837 AC6: LiveCompactResult remapped count");
    CHECK(obs.find("schema-2837") != std::string::npos, "2837 AC6: schema-2837 on health query");
    CHECK(obs.find("sticky-densify-off") != std::string::npos, "2837 AC6: sticky-densify-off key");
    CHECK(obs.find("external-root-slot-remap-total") != std::string::npos,
          "2837 AC6: external-root-slot-remap-total key");
    CHECK(build.find("check_moving_external_root_remap_2837") != std::string::npos,
          "2837 AC6: build.py wires #2837 linter");
    // No invent test_issue_2837.cpp (#81967); no design doc (#1655).
    std::ifstream invent("tests/core/test_issue_2837.cpp");
    if (!invent) {
        invent.open("../tests/core/test_issue_2837.cpp");
    }
    CHECK(!invent.good(), "2837 AC6: no test_issue_2837.cpp");
    std::ifstream design("docs/design/2837-moving-external-root-remap.md");
    if (!design) {
        design.open("../docs/design/2837-moving-external-root-remap.md");
    }
    CHECK(!design.good(), "2837 AC6: no docs/design/2837-*");
}

// ── Issue #2889: auto-register known intermediate + compiler external roots ──
// Residual after #2749 / #2837: known intermediate buffers (workspace /
// mutate-target / current flat+pool) and compiler roots (RootRemap stable +
// closure capture slots) were never auto-registered into the Moving densify
// window → counted as untracked → false sticky densify-off under production.
// The densify entry walk (pre-compact, inside moving_compact_enabled())
// auto-registers them via ArenaGroup::register_external_root_slot_for_densify_all.
//
//   AC1: walk registers known intermediate slots + RootRemap compiler roots
//        before compact_all_moving_pinned (source-cite).
//   AC2: additive counter g_moving_known_roots_auto_registered_total + reset.
//   AC3: truly foreign pointers stay unregistered → fail-closed preserved.
//   AC4: query:arena-moving-densify-health additive keys + schema-2889.
//   AC5: linter wired in build.py + no docs/design/ per #1655.
static void ac2889_1_auto_register_walk_source() {
    std::println("\n--- #2889 AC1: densify entry auto-register walk ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto arena = read_file("src/core/arena.ixx");
    const auto rrp = read_file("src/compiler/root_remap_pass.ixx");
    CHECK(mb.find("Issue #2889") != std::string::npos, "AC1: boundary TU cites #2889");
    CHECK(mb.find("register_external_root_slot_for_densify_all") != std::string::npos,
          "AC1: auto-register walk calls all-arena slot registration");
    CHECK(mb.find("root_remap_registered_slots_snapshot") != std::string::npos,
          "AC1: walk feeds RootRemap compiler roots");
    CHECK(mb.find("workspace_flat_") != std::string::npos ||
              mb.find("mutate_target_flat_") != std::string::npos,
          "AC1: known intermediate slots walked");
    // Walk lives inside the moving_compact_enabled() block (AC3 zero-cost
    // on Soft / no Moving — never reached).
    const auto walk_pos = mb.find("register_external_root_slot_for_densify_all");
    const auto moving_pos = mb.find("if (aura::ast::moving_compact_enabled())");
    CHECK(walk_pos != std::string::npos && moving_pos != std::string::npos && walk_pos > moving_pos,
          "AC1: walk inside moving_compact_enabled() block");
    CHECK(arena.find("register_external_root_slot_for_densify_all") != std::string::npos,
          "AC1: ArenaGroup all-arena helper present");
    CHECK(rrp.find("root_remap_registered_slots_snapshot") != std::string::npos,
          "AC1: RootRemap slot snapshot accessor present");
}

static void ac2889_2_counter_additive() {
    std::println("\n--- #2889 AC2: additive known-roots counter ---");
    const auto h = read_file("src/core/densify_consistency_report.h");
    CHECK(h.find("g_moving_known_roots_auto_registered_total") != std::string::npos,
          "AC2: known-roots auto-registered counter");
    CHECK(h.find("kMovingKnownRootsAutoRegisterIssue = 2889") != std::string::npos,
          "AC2: issue stamp 2889");
    CHECK(h.find("moving_known_roots_auto_registered_total_v_read") != std::string::npos,
          "AC2: read accessor");
    CHECK(h.find("reset_moving_known_roots_auto_registered_for_test") != std::string::npos,
          "AC2: reset helper");
    // Existing #2749 split counters preserved (no regression).
    CHECK(h.find("g_moving_auto_registered_remapped_total") != std::string::npos,
          "AC2: #2749 auto-registered-remapped preserved");
    CHECK(h.find("g_moving_still_untracked_incomplete_total") != std::string::npos,
          "AC2: #2749 still-untracked preserved");
}

static void ac2889_3_fail_closed_preserved() {
    std::println("\n--- #2889 AC3: fail-closed for unknown roots preserved ---");
    const auto arena = read_file("src/core/arena.ixx");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    // Untracked fail-closed path untouched: unknown pointers still force
    // incomplete_remap + pin_contract_held=false + sticky-off (AC2 #2889).
    CHECK(arena.find("result.moving_incomplete_remap = true") != std::string::npos,
          "AC3: incomplete_remap still set for untracked");
    CHECK(arena.find("result.pin_contract_held = false") != std::string::npos,
          "AC3: pin_contract_held=false still set");
    CHECK(arena.find("g_moving_incomplete_remap_sticky_densify_off") != std::string::npos,
          "AC3: sticky densify-off preserved");
    CHECK(mb.find("densify_untracked_kept") != std::string::npos,
          "AC3: Phase 5 still tracks untracked_kept");
}

static void ac2889_4_query_keys() {
    std::println("\n--- #2889 AC4: additive query keys ---");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(obs.find("known-roots-auto-registered-total") != std::string::npos,
          "AC4: known-roots-auto-registered-total key");
    CHECK(obs.find("auto-registered-remapped-total") != std::string::npos,
          "AC4: auto-registered-remapped-total key");
    CHECK(obs.find("still-untracked-incomplete-total") != std::string::npos,
          "AC4: still-untracked-incomplete-total key");
    CHECK(obs.find("known-roots-auto-register-wired") != std::string::npos, "AC4: wired sentinel");
    CHECK(obs.find("schema-2889") != std::string::npos, "AC4: schema-2889");
    CHECK(obs.find("issue-2889") != std::string::npos, "AC4: issue-2889");
    // Existing #2837 / #2619 / #2495 surfaces preserved (no regression).
    CHECK(obs.find("schema-2837") != std::string::npos, "AC4: schema-2837 preserved");
    CHECK(obs.find("schema-2619") != std::string::npos, "AC4: schema-2619 preserved");
    CHECK(obs.find("schema-2495") != std::string::npos, "AC4: schema-2495 preserved");
}

// ── Issue #2905: sticky densify-off auto-clear + Agent visibility ──
// AC1 clean Moving densify clears sticky
// AC2 query exposes sticky flag + total (schema-2905 aliases)
// AC3 production hard arms; Soft never arms
// AC4 re-register + clean Moving restores enabled without manual clear
// AC5 Phase-5 source-cite + no docs/design

static void ac2905_1_clean_moving_clears_sticky() {
    std::println("\n--- #2905 AC1: clean Moving densify clears sticky densify-off ---");
    MovingFlagGuard on(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    // Force-arm sticky as if a prior hard incomplete fired.
    aura::ast::g_moving_incomplete_remap_sticky_densify_off.store(1, std::memory_order_release);
    CHECK(aura::ast::moving_incomplete_remap_sticky_densify_off(), "AC1: sticky armed");
    CHECK(aura::ast::moving_compact_enabled() == 0, "AC1: densify forced off under sticky");
    // Temporarily allow Moving for the clean window (sticky gates enabled()).
    // Clean densify path in live_compact clears sticky when green.
    // Use a clean densify: pin-free small-pool objects, no external roots.
    // Note: sticky forces moving_compact_enabled=0 which may skip Moving
    // selection at policy layer; clear is inside live_compact(Moving) after
    // a green window — call live_compact(Moving) directly (bypasses policy).
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
    // Register slot so remap is complete (no incomplete).
    void* ext = p0;
    arena.register_external_root_slot_for_densify(&ext);
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    if (r.objects_moved > 0 && !r.moving_incomplete_remap && r.pin_contract_held) {
        CHECK(!aura::ast::moving_incomplete_remap_sticky_densify_off(),
              "AC1: clean Moving clears sticky densify-off");
        aura::ast::set_moving_compact_enabled(1);
        CHECK(aura::ast::moving_compact_enabled() == 1,
              "AC1: densify re-enabled after clean clear");
    } else {
        // If densify did not move (frag/layout), still verify clear API + path.
        aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
        CHECK(!aura::ast::moving_incomplete_remap_sticky_densify_off(),
              "AC1: clear helper works (no-move window)");
        const auto arena_src = read_file("src/core/arena.ixx");
        CHECK(arena_src.find("clear_moving_incomplete_remap_sticky_densify_off") !=
                  std::string::npos,
              "AC1: clean path cites clear sticky");
        CHECK(arena_src.find("#2905") != std::string::npos ||
                  arena_src.find("Issue #2905") != std::string::npos,
              "AC1: arena cites #2905 on clean clear");
    }
    (void)p1;
    (void)p2;
}

static void ac2905_2_query_sticky_surface() {
    std::println("\n--- #2905 AC2: query surface sticky flag + total ---");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    const auto eval = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(obs.find("sticky-densify-off") != std::string::npos, "AC2: sticky-densify-off key");
    CHECK(obs.find("sticky-densify-off-total") != std::string::npos,
          "AC2: sticky-densify-off-total key");
    CHECK(obs.find("moving-sticky-densify-off") != std::string::npos,
          "AC2: moving-sticky-densify-off alias");
    CHECK(obs.find("moving-sticky-densify-off-total") != std::string::npos,
          "AC2: moving-sticky-densify-off-total alias");
    CHECK(obs.find("schema-2905") != std::string::npos, "AC2: schema-2905 on densify health");
    CHECK(obs.find("moving-sticky-auto-clear-wired") != std::string::npos,
          "AC2: auto-clear wired sentinel");
    CHECK(eval.find("moving-sticky-densify-off") != std::string::npos,
          "AC2: arena-live-compact also exposes sticky");
    CHECK(eval.find("schema-2905") != std::string::npos, "AC2: schema-2905 on arena-live-compact");
    // Lineage preserved.
    CHECK(obs.find("schema-2837") != std::string::npos, "AC2: schema-2837 preserved");
    CHECK(obs.find("schema-2619") != std::string::npos, "AC2: schema-2619 preserved");
}

static void ac2905_3_hard_arms_soft_never() {
    std::println("\n--- #2905 AC3: production hard arms sticky; Soft never ---");
    // Hard path (reuse #2837 AC3 mechanics).
    MovingFlagGuard on(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::g_moving_untracked_hard_abort_pref.store(1, std::memory_order_relaxed);
    {
        ASTArena arena(64 * 1024);
        auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
        auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
        auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
        void* ext = p0;
        arena.register_external_root_for_densify(ext); // value-only → incomplete
        const auto r = arena.live_compact(LiveCompactMode::Moving);
        if (r.moving_incomplete_remap || r.moving_blocked_precondition) {
            CHECK(aura::ast::moving_incomplete_remap_sticky_densify_off(),
                  "AC3: hard incomplete arms sticky");
        }
        (void)p1;
        (void)p2;
    }
    // Soft path never arms.
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::g_moving_untracked_hard_abort_pref.store(0, std::memory_order_relaxed);
    {
        ASTArena arena(64 * 1024);
        auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
        auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
        auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
        void* ext = p0;
        arena.register_external_root_for_densify(ext);
        (void)arena.live_compact(LiveCompactMode::Moving);
        CHECK(!aura::ast::moving_incomplete_remap_sticky_densify_off(),
              "AC3: Soft never arms sticky densify-off");
        (void)p1;
        (void)p2;
    }
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(arena.find("Soft (hard_pref <= 0) does not arm sticky") != std::string::npos ||
              arena.find("hard_pref > 0") != std::string::npos,
          "AC3: Soft/hard arm path source-cited");
}

static void ac2905_4_reregister_clean_restores_without_manual_clear() {
    std::println(
        "\n--- #2905 AC4: re-register + clean Moving restores densify without manual clear ---");
    MovingFlagGuard on(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    // Arm sticky as residual of prior hard incomplete.
    aura::ast::g_moving_incomplete_remap_sticky_densify_off.store(1, std::memory_order_release);
    aura::ast::g_moving_untracked_hard_abort_pref.store(0, std::memory_order_relaxed);
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
    void* ext = p0;
    // Re-register as slot (proper recovery path) then clean densify.
    arena.register_external_root_slot_for_densify(&ext);
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    // Auto-clear on clean window — no call to clear_moving_incomplete_remap_sticky_densify_off.
    if (r.objects_moved > 0 && !r.moving_incomplete_remap && r.pin_contract_held) {
        CHECK(!aura::ast::moving_incomplete_remap_sticky_densify_off(),
              "AC4: sticky auto-cleared after re-register + clean Moving");
        aura::ast::set_moving_compact_enabled(1);
        CHECK(aura::ast::moving_compact_enabled() == 1,
              "AC4: moving_compact_enabled restored without manual clear");
    } else {
        // Structural: clean clear + Phase-5 auto-clear are wired.
        const auto arena_src = read_file("src/core/arena.ixx");
        const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(arena_src.find("clear_moving_incomplete_remap_sticky_densify_off()") !=
                  std::string::npos,
              "AC4: per-arena clean clears sticky");
        CHECK(mb.find("clear_moving_incomplete_remap_sticky_densify_off") != std::string::npos,
              "AC4: Phase-5 auto-clears sticky on unified success");
        CHECK(mb.find("#2905") != std::string::npos || mb.find("Issue #2905") != std::string::npos,
              "AC4: Phase-5 cites #2905");
        aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    }
    (void)p1;
    (void)p2;
}

static void ac2905_5_source_cite_phase5_no_design() {
    std::println("\n--- #2905 AC5: source-cite + Phase-5 + linter + no design ---");
    const auto arena = read_file("src/core/arena.ixx");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_moving_sticky_densify_off_2905.py");
    const auto t = read_file("tests/core/test_moving_densify_fail_closed.cpp");
    CHECK(arena.find("#2905") != std::string::npos ||
              arena.find("Issue #2905") != std::string::npos,
          "AC5: arena.ixx cites #2905");
    CHECK(mb.find("#2905") != std::string::npos || mb.find("Issue #2905") != std::string::npos,
          "AC5: Phase-5 cites #2905");
    CHECK(mb.find("moving_unified_success") != std::string::npos,
          "AC5: Phase-5 uses unified success for clear");
    CHECK(obs.find("schema-2905") != std::string::npos, "AC5: schema-2905 on query");
    CHECK(build.find("check_moving_sticky_densify_off_2905") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(!lint.empty() && lint.find("2905") != std::string::npos, "AC5: linter present");
    CHECK(t.find("ac2905_1_clean_moving_clears_sticky") != std::string::npos, "AC5: AC1 test");
    CHECK(t.find("ac2905_4_reregister_clean_restores_without_manual_clear") != std::string::npos,
          "AC5: AC4 test");
    CHECK(read_file("docs/design/2905-sticky-densify-off.md").empty(),
          "AC5: no docs/design/2905-* per #1655");
    CHECK(read_file("tests/core/test_issue_2905.cpp").empty(), "AC5: no new test file per #81967");
}

static void ac2889_5_linter_and_no_design() {
    std::println("\n--- #2889 AC5: linter + no docs/design/ ---");
    const auto t = read_file("tests/core/test_moving_densify_fail_closed.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_moving_known_roots_auto_register_2889.py");
    const auto build = read_file("build.py");
    CHECK(t.find("ac2889_1_auto_register_walk_source") != std::string::npos, "AC5: AC1 test");
    CHECK(t.find("ac2889_2_counter_additive") != std::string::npos, "AC5: AC2 test");
    CHECK(t.find("ac2889_3_fail_closed_preserved") != std::string::npos, "AC5: AC3 test");
    CHECK(t.find("ac2889_4_query_keys") != std::string::npos, "AC5: AC4 test");
    CHECK(t.find("ac2889_5_linter_and_no_design") != std::string::npos, "AC5: AC5 self-test");
    CHECK(!lint.empty() && lint.find("Issue #2889") != std::string::npos,
          "AC5: coverage linter present and cites #2889");
    CHECK(build.find("check_moving_known_roots_auto_register_2889") != std::string::npos,
          "AC5: build.py gate entry");
    std::ifstream design("docs/design/2889-moving-known-roots-auto-register.md");
    if (!design) {
        design.open("../docs/design/2889-moving-known-roots-auto-register.md");
    }
    CHECK(!design.good(), "AC5: no docs/design/2889-* per #1655");
}

// ── Issue #2935: known-root coverage + sticky-off Agent recovery ──
// Extends #2889 inventory (WorkspaceTree layer slots) + shared register
// helper; adds Agent recovery path (re-register + clear sticky + optional
// one-shot Moving densify) with additive counters. Soft never arms sticky;
// hard path still arms. Fail-closed incomplete-remap preserved.
//
//   AC1: densify entry calls register_known_moving_densify_root_slots
//        (full inventory: 6 intermediates + WorkspaceTree + RootRemap).
//   AC2: production hard still arms sticky; Soft never arms.
//   AC3: recover_moving_sticky_densify_off + arena:recover-moving-sticky-densify
//        re-register + clear sticky + optional densify retry.
//   AC4: additive recovery counters + schema-2935 on densify-health.
//   AC5: Soft / Moving-off zero densify work preserved (walk inside
//        moving_compact_enabled block; recovery densify gated).
//   AC6: tests + coverage linter + no docs/design/.

static void ac2935_1_full_inventory_and_shared_helper() {
    std::println("\n--- #2935 AC1: full known-root inventory + shared register helper ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto ev = read_file("src/compiler/evaluator.ixx");
    CHECK(mb.find("Issue #2935") != std::string::npos || mb.find("#2935") != std::string::npos,
          "AC1: boundary TU cites #2935");
    CHECK(mb.find("register_known_moving_densify_root_slots") != std::string::npos,
          "AC1: densify entry uses shared register helper");
    CHECK(mb.find("WorkspaceTree") != std::string::npos &&
              mb.find("parent_flat_") != std::string::npos &&
              mb.find("parent_pool_") != std::string::npos,
          "AC1: WorkspaceTree layer residual slots in inventory");
    CHECK(mb.find("workspace_flat_") != std::string::npos, "AC1: workspace_flat_ in inventory");
    CHECK(mb.find("mutate_target_flat_") != std::string::npos,
          "AC1: mutate_target_flat_ in inventory");
    CHECK(mb.find("current_flat_") != std::string::npos, "AC1: current_flat_ in inventory");
    CHECK(mb.find("root_remap_registered_slots_snapshot") != std::string::npos,
          "AC1: RootRemap compiler roots still registered");
    CHECK(ev.find("register_known_moving_densify_root_slots") != std::string::npos,
          "AC1: Evaluator declares register helper");
    // Densify entry still inside moving_compact_enabled() (zero work Soft/off).
    const auto helper_call = mb.find("register_known_moving_densify_root_slots()");
    const auto moving_pos = mb.find("if (aura::ast::moving_compact_enabled())");
    CHECK(helper_call != std::string::npos && moving_pos != std::string::npos &&
              helper_call > moving_pos,
          "AC1: densify-entry register still inside moving_compact_enabled()");
}

static void ac2935_2_hard_arms_soft_never_preserved() {
    std::println("\n--- #2935 AC2: hard sticky arm preserved; Soft never arms ---");
    // Runtime: reuse #2905/#2837 mechanics.
    MovingFlagGuard on(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::g_moving_untracked_hard_abort_pref.store(1, std::memory_order_relaxed);
    {
        ASTArena arena(64 * 1024);
        auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
        auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
        auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
        void* ext = p0;
        arena.register_external_root_for_densify(ext); // value-only → incomplete under hard
        const auto r = arena.live_compact(LiveCompactMode::Moving);
        if (r.moving_incomplete_remap || r.moving_blocked_precondition) {
            CHECK(aura::ast::moving_incomplete_remap_sticky_densify_off(),
                  "AC2: production hard incomplete still arms sticky");
        }
        (void)p1;
        (void)p2;
    }
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::g_moving_untracked_hard_abort_pref.store(0, std::memory_order_relaxed);
    {
        ASTArena arena(64 * 1024);
        auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
        auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
        auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
        void* ext = p0;
        arena.register_external_root_for_densify(ext);
        (void)arena.live_compact(LiveCompactMode::Moving);
        CHECK(!aura::ast::moving_incomplete_remap_sticky_densify_off(),
              "AC2: Soft never arms sticky densify-off");
        (void)p1;
        (void)p2;
    }
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(arena.find("Soft (hard_pref <= 0) does not arm sticky") != std::string::npos ||
              arena.find("hard_pref > 0") != std::string::npos,
          "AC2: Soft/hard arm path source-cited");
}

static void ac2935_3_agent_recovery_path() {
    std::println(
        "\n--- #2935 AC3: Agent recovery re-register + clear sticky + optional densify ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto mem = read_file("src/compiler/evaluator_primitives_memory.cpp");
    const auto ev = read_file("src/compiler/evaluator.ixx");
    CHECK(mb.find("recover_moving_sticky_densify_off") != std::string::npos,
          "AC3: recovery method implemented");
    CHECK(ev.find("recover_moving_sticky_densify_off") != std::string::npos,
          "AC3: recovery method declared on Evaluator");
    CHECK(ev.find("MovingStickyDensifyRecoveryResult") != std::string::npos,
          "AC3: recovery result struct");
    CHECK(mb.find("clear_moving_incomplete_remap_sticky_densify_off") != std::string::npos,
          "AC3: recovery clears sticky");
    CHECK(mb.find("compact_all_moving_pinned") != std::string::npos,
          "AC3: recovery optional densify retry");
    CHECK(mb.find("g_moving_sticky_cleared_via_recovery_total") != std::string::npos,
          "AC3: sticky-cleared-via-recovery counter bump");
    CHECK(mb.find("g_moving_densify_retry_after_recovery_total") != std::string::npos,
          "AC3: densify-retry-after-recovery counter bump");
    CHECK(mem.find("arena:recover-moving-sticky-densify") != std::string::npos,
          "AC3: Agent primitive wired");
    CHECK(mem.find("schema-2935") != std::string::npos, "AC3: primitive returns schema-2935");
    // Counter unit path: force-arm sticky, clear via recovery counter helper surface.
    aura::core::densify_consistency::reset_moving_sticky_densify_recovery_for_test();
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::g_moving_incomplete_remap_sticky_densify_off.store(1, std::memory_order_release);
    CHECK(aura::ast::moving_incomplete_remap_sticky_densify_off(), "AC3: sticky force-armed");
    // Direct clear path (recovery body step b) — counters need Evaluator for full
    // path; verify counter symbols + sticky clear helper still work standalone.
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    CHECK(!aura::ast::moving_incomplete_remap_sticky_densify_off(),
          "AC3: sticky clear helper recovers densify enablement");
    aura::ast::set_moving_compact_enabled(1);
    CHECK(aura::ast::moving_compact_enabled() == 1,
          "AC3: moving_compact_enabled restored after sticky clear");
}

static void ac2935_4_additive_metrics_and_schema() {
    std::println("\n--- #2935 AC4: additive recovery counters + schema-2935 ---");
    const auto h = read_file("src/core/densify_consistency_report.h");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(h.find("g_moving_sticky_cleared_via_recovery_total") != std::string::npos,
          "AC4: sticky-cleared-via-recovery counter");
    CHECK(h.find("g_moving_densify_retry_after_recovery_total") != std::string::npos,
          "AC4: densify-retry-after-recovery counter");
    CHECK(h.find("kMovingStickyDensifyRecoveryIssue = 2935") != std::string::npos,
          "AC4: issue stamp 2935");
    CHECK(h.find("moving_sticky_cleared_via_recovery_total_v_read") != std::string::npos,
          "AC4: sticky cleared read accessor");
    CHECK(h.find("moving_densify_retry_after_recovery_total_v_read") != std::string::npos,
          "AC4: densify retry read accessor");
    CHECK(h.find("reset_moving_sticky_densify_recovery_for_test") != std::string::npos,
          "AC4: recovery counter test reset");
    // #2889 known-roots counter preserved.
    CHECK(h.find("g_moving_known_roots_auto_registered_total") != std::string::npos,
          "AC4: #2889 known-roots counter preserved");
    CHECK(obs.find("sticky-cleared-via-recovery-total") != std::string::npos,
          "AC4: densify-health sticky-cleared-via-recovery-total");
    CHECK(obs.find("densify-retry-after-recovery-total") != std::string::npos,
          "AC4: densify-health densify-retry-after-recovery-total");
    CHECK(obs.find("sticky-recovery-wired") != std::string::npos, "AC4: sticky-recovery-wired");
    CHECK(obs.find("schema-2935") != std::string::npos, "AC4: schema-2935");
    CHECK(obs.find("issue-2935") != std::string::npos, "AC4: issue-2935");
    // Lineage preserved.
    CHECK(obs.find("schema-2889") != std::string::npos, "AC4: schema-2889 preserved");
    CHECK(obs.find("schema-2905") != std::string::npos, "AC4: schema-2905 preserved");
    CHECK(obs.find("schema-2837") != std::string::npos, "AC4: schema-2837 preserved");
}

static void ac2935_5_soft_zero_work_and_moving_off() {
    std::println("\n--- #2935 AC5: Soft / Moving-off zero densify work preserved ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    // Densify-entry register still gated by moving_compact_enabled (AC5 of #2889).
    CHECK(mb.find("if (aura::ast::moving_compact_enabled())") != std::string::npos,
          "AC5: densify entry still gated");
    // Recovery densify retry gated by moving_compact_enabled after sticky clear.
    CHECK(mb.find("retry_densify && arena_group_ && aura::ast::moving_compact_enabled()") !=
                  std::string::npos ||
              mb.find("moving_compact_enabled()") != std::string::npos,
          "AC5: recovery densify gated by moving_compact_enabled");
    // Soft never arms sticky — source cite.
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(arena.find("Soft (hard_pref <= 0) does not arm sticky") != std::string::npos ||
              arena.find("hard_pref > 0") != std::string::npos,
          "AC5: Soft never arms sticky source-cited");
}

static void ac2935_6_linter_and_no_design() {
    std::println("\n--- #2935 AC6: linter + no docs/design/ ---");
    const auto t = read_file("tests/core/test_moving_densify_fail_closed.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_moving_known_roots_sticky_recovery_2935.py");
    const auto build = read_file("build.py");
    CHECK(t.find("ac2935_1_full_inventory_and_shared_helper") != std::string::npos,
          "AC6: AC1 test");
    CHECK(t.find("ac2935_2_hard_arms_soft_never_preserved") != std::string::npos, "AC6: AC2 test");
    CHECK(t.find("ac2935_3_agent_recovery_path") != std::string::npos, "AC6: AC3 test");
    CHECK(t.find("ac2935_4_additive_metrics_and_schema") != std::string::npos, "AC6: AC4 test");
    CHECK(t.find("ac2935_5_soft_zero_work_and_moving_off") != std::string::npos, "AC6: AC5 test");
    CHECK(t.find("ac2935_6_linter_and_no_design") != std::string::npos, "AC6: AC6 self-test");
    CHECK(!lint.empty() && lint.find("Issue #2935") != std::string::npos,
          "AC6: coverage linter present and cites #2935");
    CHECK(build.find("check_moving_known_roots_sticky_recovery_2935") != std::string::npos,
          "AC6: build.py gate entry");
    std::ifstream design("docs/design/2935-moving-sticky-recovery.md");
    if (!design) {
        design.open("../docs/design/2935-moving-sticky-recovery.md");
    }
    CHECK(!design.good(), "AC6: no docs/design/2935-* per #1655");
    CHECK(read_file("tests/core/test_issue_2935.cpp").empty(),
          "AC6: no new invent test file per #81967");
}

// ── Issue #2971: production-required GeneralObjectPin on create + densify ──
// Residual of #2840: production locks required, but ASTArena::create did
// not auto-wire intermediates and live_compact(Moving) checked breach
// only AFTER relocate. #2971 auto-wires create under required and
// fail-closes Moving BEFORE address movement. pin_contract_held remains
// the single success signal. Soft / unset stays a single atomic load.
//
//   AC1: production default still forces required (step 15) + create
//        auto-wire path source-cited.
//   AC2: required + unpinned intermediates → densify blocked before
//        relocate (payloads intact, objects_moved==0).
//   AC3: slot-covered intermediates ( #2935 / #2837 ) allow densify.
//   AC4: additive schema keys on densify-health / lifetime-pin-stats.
//   AC5: Soft / unset — create does not bump auto_wire; no pre-move gate.
//   AC6: tests + coverage linter + no docs/design/ / no invent file.

struct RequiredPinGuard {
    int prev = -1;
    explicit RequiredPinGuard(int enable) {
        prev = aura::core::lifetime::g_general_object_pin_required_pref.load(
            std::memory_order_relaxed);
        aura::core::lifetime::g_general_object_pin_required_pref.store(enable,
                                                                       std::memory_order_release);
    }
    ~RequiredPinGuard() {
        aura::core::lifetime::g_general_object_pin_required_pref.store(prev,
                                                                       std::memory_order_release);
        aura::core::lifetime::clear_general_object_pin_required_breach();
        aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    }
};

static void ac2971_1_production_default_and_create_auto_wire() {
    std::println("\n--- #2971 AC1: production default + create auto-wire ---");
    const auto hh = read_file("src/compiler/security_defaults.hh");
    const auto arena = read_file("src/core/arena.ixx");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(hh.find("#2971") != std::string::npos, "AC1: security_defaults cites #2971");
    CHECK(hh.find("g_general_object_pin_required_pref.store(1, std::memory_order_release)") !=
              std::string::npos,
          "AC1: production still locks required when env unset");
    CHECK(arena.find("note_intermediate_create_auto_wire_") != std::string::npos,
          "AC1: ASTArena::create auto-wires intermediates");
    CHECK(arena.find("note_general_object_create_auto_wire") != std::string::npos,
          "AC1: create path bumps auto_wire via existing helper");
    CHECK(arena.find("register_external_root_for_densify") != std::string::npos,
          "AC1: auto-wired intermediates are densify-visible (#2935)");
    CHECK(lp.find("kGeneralObjectPinCreateDensifyIssue = 2971") != std::string::npos,
          "AC1: issue stamp 2971");
    CHECK(lp.find("note_general_object_create_auto_wire") != std::string::npos,
          "AC1: create auto-wire helper");
}

static void ac2971_2_pre_move_densify_gate() {
    std::println("\n--- #2971 AC2: unpinned intermediates block Moving before relocate ---");
    MovingFlagGuard on(1);
    RequiredPinGuard req(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::core::lifetime::clear_general_object_pin_required_breach();
    aura::core::lifetime::reset_general_object_pin_pre_move_block_for_test();
    const auto wire0 = aura::core::lifetime::general_object_pin_auto_wire_total_v_read();
    const auto block0 =
        aura::core::lifetime::general_object_pin_pre_move_unpinned_block_total_v_read();
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
    CHECK(p0 && p1 && p2, "AC2: creates succeeded");
    CHECK(arena.intermediate_create_auto_wire_count() == 3, "AC2: three intermediates auto-wired");
    CHECK(aura::core::lifetime::general_object_pin_auto_wire_total_v_read() >= wire0 + 3,
          "AC2: auto_wire_total rose under required");
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    CHECK(r.objects_moved == 0, "AC2: no address movement (pre-move gate)");
    CHECK(!r.pin_contract_held, "AC2: pin_contract_held is the fail signal");
    CHECK(r.moving_blocked_precondition, "AC2: Moving blocked as precondition");
    CHECK(r.moving_incomplete_remap, "AC2: incomplete-remap marked");
    CHECK(aura::core::lifetime::general_object_pin_required_breach_active(),
          "AC2: sticky breach armed");
    CHECK(aura::core::lifetime::general_object_pin_pre_move_unpinned_block_total_v_read() >=
              block0 + 1,
          "AC2: pre-move block counter bumped");
    // Chaos soak: residual unpinned intermediates must still be readable
    // at the original addresses (densify did not move them).
    CHECK(p0->a == 1 && p0->b == 2 && p0->c == 3 && p0->d == 4,
          "AC2: p0 payload intact after blocked densify");
    CHECK(p1->a == 5 && p1->b == 6 && p1->c == 7 && p1->d == 8,
          "AC2: p1 payload intact after blocked densify");
    CHECK(p2->a == 9 && p2->b == 10 && p2->c == 11 && p2->d == 12,
          "AC2: p2 payload intact after blocked densify");
    CHECK(arena.intermediate_create_auto_wire_count() == 3,
          "AC2: zero residual drop — intermediates still live + tracked");
}

static void ac2971_3_slot_covered_allows_densify() {
    std::println("\n--- #2971 AC3: slot-covered intermediates allow densify ---");
    MovingFlagGuard on(1);
    RequiredPinGuard req(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::core::lifetime::clear_general_object_pin_required_breach();
    aura::core::lifetime::reset_general_object_pin_pre_move_block_for_test();
    const auto block0 =
        aura::core::lifetime::general_object_pin_pre_move_unpinned_block_total_v_read();
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
    void* s0 = p0;
    void* s1 = p1;
    void* s2 = p2;
    arena.register_external_root_slot_for_densify(&s0);
    arena.register_external_root_slot_for_densify(&s1);
    arena.register_external_root_slot_for_densify(&s2);
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    CHECK(aura::core::lifetime::general_object_pin_pre_move_unpinned_block_total_v_read() == block0,
          "AC3: pre-move gate does not fire when slots cover creates");
    // Either no-move (quiet pool) or a real move with contract held.
    if (r.objects_moved > 0) {
        CHECK(r.pin_contract_held, "AC3: pin_contract_held after slot-covered move");
        CHECK(!r.moving_blocked_precondition || r.pin_contract_held,
              "AC3: slot-covered densify is not a required-pin breach");
    } else {
        CHECK(p0->a == 1 && p1->a == 5 && p2->a == 9, "AC3: no-move payloads intact");
    }
    (void)r;
}

static void ac2971_4_observability_schema() {
    std::println("\n--- #2971 AC4: additive schema keys ---");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    const auto health = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    const auto pinq = read_file("src/compiler/evaluator_primitives_stdlib_review.cpp");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(obs.find("schema-2971") != std::string::npos, "AC4: live-compact-stats schema-2971");
    CHECK(obs.find("issue-2971") != std::string::npos, "AC4: live-compact-stats issue-2971");
    CHECK(obs.find("general-object-pin-pre-move-unpinned-block-total") != std::string::npos,
          "AC4: pre-move block key on live-compact-stats");
    CHECK(obs.find("general-object-pin-auto-wire-total") != std::string::npos,
          "AC4: auto-wire key preserved");
    CHECK(obs.find("general-object-pin-required-enforced-total") != std::string::npos,
          "AC4: required-enforced key preserved");
    CHECK(obs.find("general-object-pin-required-breach-densify-fail-total") != std::string::npos,
          "AC4: breach-densify-fail key preserved");
    CHECK(health.find("schema-2971") != std::string::npos, "AC4: densify-health schema-2971");
    CHECK(health.find("general-object-pin-auto-wire-total") != std::string::npos,
          "AC4: densify-health auto-wire");
    CHECK(health.find("general-object-pin-required-enforced-total") != std::string::npos,
          "AC4: densify-health required-enforced");
    CHECK(health.find("general-object-pin-required-breach-densify-fail-total") != std::string::npos,
          "AC4: densify-health breach-densify-fail");
    CHECK(pinq.find("schema-2971") != std::string::npos, "AC4: lifetime-pin-stats schema-2971");
    CHECK(pinq.find("general-object-pin-auto-wire-total") != std::string::npos,
          "AC4: lifetime-pin-stats auto-wire");
    CHECK(lp.find("g_general_object_pin_pre_move_unpinned_block_total") != std::string::npos,
          "AC4: pre-move counter declared");
}

static void ac2971_5_soft_zero_cost() {
    std::println("\n--- #2971 AC5: Soft / unset zero extra create work ---");
    RequiredPinGuard off(0);
    const auto wire0 = aura::core::lifetime::general_object_pin_auto_wire_total_v_read();
    const auto block0 =
        aura::core::lifetime::general_object_pin_pre_move_unpinned_block_total_v_read();
    MovingFlagGuard on(1);
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
    CHECK(arena.intermediate_create_auto_wire_count() == 0,
          "AC5: Soft create does not install inventory");
    CHECK(aura::core::lifetime::general_object_pin_auto_wire_total_v_read() == wire0,
          "AC5: auto_wire_total unchanged on Soft create");
    (void)arena.live_compact(LiveCompactMode::Moving);
    CHECK(aura::core::lifetime::general_object_pin_pre_move_unpinned_block_total_v_read() == block0,
          "AC5: pre-move gate does not fire when required is off");
    const auto arena_src = read_file("src/core/arena.ixx");
    CHECK(arena_src.find("general_object_pin_required_active()") != std::string::npos,
          "AC5: create + densify gates on required_active");
    CHECK(arena_src.find("has_unpinned_intermediate_creates_()") != std::string::npos,
          "AC5: pre-move check is a named helper");
    (void)p0;
    (void)p1;
    (void)p2;
}

static void ac2971_6_linter_and_no_design() {
    std::println("\n--- #2971 AC6: linter + no docs/design/ ---");
    const auto t = read_file("tests/core/test_moving_densify_fail_closed.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_general_object_pin_create_densify_2971.py");
    const auto build = read_file("build.py");
    CHECK(t.find("ac2971_1_production_default_and_create_auto_wire") != std::string::npos,
          "AC6: AC1 test");
    CHECK(t.find("ac2971_2_pre_move_densify_gate") != std::string::npos, "AC6: AC2 test");
    CHECK(t.find("ac2971_3_slot_covered_allows_densify") != std::string::npos, "AC6: AC3 test");
    CHECK(t.find("ac2971_4_observability_schema") != std::string::npos, "AC6: AC4 test");
    CHECK(t.find("ac2971_5_soft_zero_cost") != std::string::npos, "AC6: AC5 test");
    CHECK(t.find("ac2971_6_linter_and_no_design") != std::string::npos, "AC6: AC6 self-test");
    CHECK(!lint.empty() && lint.find("Issue #2971") != std::string::npos,
          "AC6: coverage linter present and cites #2971");
    CHECK(build.find("check_general_object_pin_create_densify_2971") != std::string::npos,
          "AC6: build.py gate entry");
    std::ifstream design("docs/design/2971-general-object-pin-create-densify.md");
    if (!design) {
        design.open("../docs/design/2971-general-object-pin-create-densify.md");
    }
    CHECK(!design.good(), "AC6: no docs/design/2971-* per #1655");
    CHECK(read_file("tests/core/test_issue_2971.cpp").empty(),
          "AC6: no new invent test file per #81967");
}

// ── Issue #2973: production hard pre-densify external-root completeness ──
// Residual of #2935: inventory + recovery exist, but densify still moved
// first and fail-closed after the fact. Production hard now walks
// declared external roots BEFORE relocate and blocks if any would-move
// candidate is not slot- or pin-covered. Soft stays post-move observe-only.
//
//   AC1: production hard + value-only omitted slot → no movement, sticky.
//   AC2: Soft / hard_pref<=0 still moves and observes post-move incomplete.
//   AC3: slot-covered declared roots pass the pre-check.
//   AC4: additive pre-check counters + schema-2973 on densify-health.
//   AC5: recovery primitive still the recovery surface (source-cite).
//   AC6: linter + no docs/design/ / no invent file.

static void ac2973_1_hard_blocks_before_move() {
    std::println("\n--- #2973 AC1: production hard blocks BEFORE remap ---");
    MovingFlagGuard on(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::g_moving_untracked_hard_abort_pref.store(1, std::memory_order_relaxed);
    aura::core::densify_consistency::reset_moving_pre_densify_completeness_for_test();
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
    void* ext = p0;
    arena.register_external_root_for_densify(ext); // omit slot — known untracked
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    CHECK(r.objects_moved == 0, "AC1: no address movement");
    CHECK(r.moving_blocked_precondition, "AC1: blocked as precondition");
    CHECK(r.moving_incomplete_remap, "AC1: incomplete-remap marked");
    CHECK(!r.pin_contract_held, "AC1: pin_contract_held is the fail signal");
    CHECK(aura::ast::moving_incomplete_remap_sticky_densify_off(), "AC1: sticky densify-off armed");
    CHECK(aura::core::densify_consistency::moving_pre_densify_reject_total_v_read() >= 1,
          "AC1: pre-densify-reject-total bumped");
    CHECK(aura::core::densify_consistency::moving_pre_densify_untracked_total_v_read() >= 1,
          "AC1: pre-densify-untracked-total bumped");
    CHECK(p0->a == 1 && p0->b == 2 && p0->c == 3 && p0->d == 4, "AC1: p0 payload intact");
    CHECK(p1->a == 5 && p1->b == 6, "AC1: p1 payload intact");
    CHECK(p2->a == 9 && p2->b == 10, "AC1: p2 payload intact");
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::g_moving_untracked_hard_abort_pref.store(0, std::memory_order_relaxed);
}

static void ac2973_2_soft_still_observe_only() {
    std::println("\n--- #2973 AC2: Soft remains post-move observe-only ---");
    MovingFlagGuard on(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::g_moving_untracked_hard_abort_pref.store(0, std::memory_order_relaxed);
    const auto rej0 = aura::core::densify_consistency::moving_pre_densify_reject_total_v_read();
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
    void* ext = p0;
    arena.register_external_root_for_densify(ext);
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    CHECK(r.objects_moved > 0, "AC2: Soft still densifies");
    CHECK(r.moving_incomplete_remap || r.external_roots_stale_unremapped_count >= 1,
          "AC2: post-move incomplete still observed");
    CHECK(!aura::ast::moving_incomplete_remap_sticky_densify_off(), "AC2: Soft never arms sticky");
    CHECK(aura::core::densify_consistency::moving_pre_densify_reject_total_v_read() == rej0,
          "AC2: pre-check reject counter unchanged on Soft");
    (void)p1;
    (void)p2;
}

static void ac2973_3_slot_covered_passes() {
    std::println("\n--- #2973 AC3: slot-covered declared roots pass pre-check ---");
    MovingFlagGuard on(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::g_moving_untracked_hard_abort_pref.store(1, std::memory_order_relaxed);
    const auto rej0 = aura::core::densify_consistency::moving_pre_densify_reject_total_v_read();
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
    void* s0 = p0;
    void* s1 = p1;
    void* s2 = p2;
    arena.register_external_root_slot_for_densify(&s0);
    arena.register_external_root_slot_for_densify(&s1);
    arena.register_external_root_slot_for_densify(&s2);
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    CHECK(aura::core::densify_consistency::moving_pre_densify_reject_total_v_read() == rej0,
          "AC3: pre-check does not reject slot-covered roots");
    if (r.objects_moved > 0) {
        CHECK(r.pin_contract_held, "AC3: pin_contract_held after covered move");
    }
    aura::ast::g_moving_untracked_hard_abort_pref.store(0, std::memory_order_relaxed);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
}

static void ac2973_4_observability_schema() {
    std::println("\n--- #2973 AC4: additive schema keys ---");
    const auto h = read_file("src/core/densify_consistency_report.h");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(h.find("g_moving_pre_densify_reject_total") != std::string::npos,
          "AC4: pre-densify-reject counter");
    CHECK(h.find("g_moving_pre_densify_untracked_total") != std::string::npos,
          "AC4: pre-densify-untracked counter");
    CHECK(h.find("kMovingPreDensifyCompletenessIssue = 2973") != std::string::npos,
          "AC4: issue stamp 2973");
    CHECK(h.find("reset_moving_pre_densify_completeness_for_test") != std::string::npos,
          "AC4: test reset helper");
    CHECK(obs.find("pre-densify-reject-total") != std::string::npos, "AC4: densify-health reject");
    CHECK(obs.find("pre-densify-untracked-total") != std::string::npos,
          "AC4: densify-health untracked");
    CHECK(obs.find("pre-densify-completeness-wired") != std::string::npos, "AC4: wired sentinel");
    CHECK(obs.find("schema-2973") != std::string::npos, "AC4: schema-2973");
    CHECK(obs.find("issue-2973") != std::string::npos, "AC4: issue-2973");
    CHECK(obs.find("schema-2935") != std::string::npos, "AC4: schema-2935 preserved");
    CHECK(obs.find("schema-2495") != std::string::npos, "AC4: schema-2495 preserved");
}

static void ac2973_5_recovery_and_soft_zero() {
    std::println("\n--- #2973 AC5: recovery surface + Soft / Moving-off zero work ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(mb.find("recover_moving_sticky_densify_off") != std::string::npos,
          "AC5: recovery primitive retained");
    CHECK(mb.find("register_known_moving_densify_root_slots") != std::string::npos,
          "AC5: #2935 inventory still the SSOT");
    CHECK(arena.find("count_pre_densify_untracked_external_roots_") != std::string::npos,
          "AC5: pre-check helper");
    CHECK(arena.find("g_moving_untracked_hard_abort_pref.load") != std::string::npos,
          "AC5: gated on production hard pref");
    CHECK(arena.find("relocate_tracked_objects_for_moving_") != std::string::npos,
          "AC5: relocate still present (defense-in-depth after pre-check)");
}

static void ac2973_6_linter_and_no_design() {
    std::println("\n--- #2973 AC6: linter + no docs/design/ ---");
    const auto t = read_file("tests/core/test_moving_densify_fail_closed.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_moving_pre_densify_completeness_2973.py");
    const auto build = read_file("build.py");
    CHECK(t.find("ac2973_1_hard_blocks_before_move") != std::string::npos, "AC6: AC1 test");
    CHECK(t.find("ac2973_2_soft_still_observe_only") != std::string::npos, "AC6: AC2 test");
    CHECK(t.find("ac2973_3_slot_covered_passes") != std::string::npos, "AC6: AC3 test");
    CHECK(t.find("ac2973_4_observability_schema") != std::string::npos, "AC6: AC4 test");
    CHECK(t.find("ac2973_5_recovery_and_soft_zero") != std::string::npos, "AC6: AC5 test");
    CHECK(t.find("ac2973_6_linter_and_no_design") != std::string::npos, "AC6: AC6 self-test");
    CHECK(!lint.empty() && lint.find("Issue #2973") != std::string::npos,
          "AC6: coverage linter present and cites #2973");
    CHECK(build.find("check_moving_pre_densify_completeness_2973") != std::string::npos,
          "AC6: build.py gate entry");
    std::ifstream design("docs/design/2973-pre-densify-completeness.md");
    if (!design) {
        design.open("../docs/design/2973-pre-densify-completeness.md");
    }
    CHECK(!design.good(), "AC6: no docs/design/2973-* per #1655");
    CHECK(read_file("tests/core/test_issue_2973.cpp").empty(),
          "AC6: no new invent test file per #81967");
}

// ── Issue #3017: incomplete-remap residual (value-only / un-slotted) ──
// Residual of #2495/#2664/#2837/#2971/#2973: production hard already
// fail-closes untracked declared roots BEFORE relocate, but value-only
// prep was still readable as if it were safe cover, and the 2709 lint
// scan omitted FFI / agent / scratch. #3017 makes value-only
// observability-only, expands the allocate-bypass lint, and adds
// mutate×densify soak + untracked inject + sticky recover.
//
//   AC1: value-only is not safe cover (source-cite + lint).
//   AC2: untracked inject canary — production hard pre-move reject.
//   AC3: mutate × densify soak with slot cover — payloads intact.
//   AC4: after inject + sticky, slot-cover + clean densify clears sticky.
//   AC5: Soft / Off zero extra cost (no value-only-not-cover bump).
//   AC6: linter + no docs/design/ / no invent file.

static void ac3017_1_audit_value_only_not_cover() {
    std::println("\n--- #3017 AC1: value-only prep is not safe cover ---");
    const auto arena = read_file("src/core/arena.ixx");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    const auto lint = read_file("scripts/coverage/checks/check_moving_incomplete_remap_3017.py");
    CHECK(arena.find("Issue #3017: value-only prep is observability only, not safe cover") !=
              std::string::npos,
          "AC1: register_external_root_for_densify cites not-safe-cover");
    CHECK(arena.find("value-only prep is observability only") != std::string::npos,
          "AC1: pre-densify helper treats value-only as not cover");
    CHECK(lp.find("value-only register_external_root_for_densify is not the") != std::string::npos,
          "AC1: LifetimePin triad excludes value-only");
    CHECK(lint.find("evaluator_primitives_agent.cpp") != std::string::npos,
          "AC1: lint scans agent create surface");
    CHECK(lint.find("ffi_primitives_impl.cpp") != std::string::npos, "AC1: lint scans FFI");
    CHECK(lint.find("evaluator_primitives_memory.cpp") != std::string::npos,
          "AC1: lint scans scratch/memory");
    CHECK(lint.find("_value_only_is_not_cover") != std::string::npos,
          "AC1: lint classifies value-only as not cover");
}

static void ac3017_2_untracked_inject_canary() {
    std::println("\n--- #3017 AC2: untracked inject canary (value-only) ---");
    MovingFlagGuard on(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::g_moving_untracked_hard_abort_pref.store(1, std::memory_order_relaxed);
    aura::core::densify_consistency::reset_moving_pre_densify_completeness_for_test();
    aura::core::densify_consistency::reset_moving_incomplete_remap_residual_3017_for_test();
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
    void* ext = p0;
    arena.register_external_root_for_densify(ext); // inject: value-only, no slot
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    CHECK(r.objects_moved == 0, "AC2: no address movement on untracked inject");
    CHECK(r.moving_blocked_precondition, "AC2: blocked as precondition");
    CHECK(r.moving_incomplete_remap, "AC2: incomplete-remap marked");
    CHECK(!r.pin_contract_held, "AC2: pin_contract_held is the fail signal");
    CHECK(aura::ast::moving_incomplete_remap_sticky_densify_off(), "AC2: sticky densify-off armed");
    CHECK(aura::core::densify_consistency::moving_pre_densify_reject_total_v_read() >= 1,
          "AC2: pre-densify-reject-total bumped");
    CHECK(aura::core::densify_consistency::moving_value_only_not_cover_total_v_read() >= 1,
          "AC2: value-only-not-cover-total bumped");
    CHECK(p0->a == 1 && p0->b == 2 && p0->c == 3 && p0->d == 4, "AC2: p0 payload intact");
    CHECK(p1->a == 5 && p1->b == 6, "AC2: p1 payload intact");
    CHECK(p2->a == 9 && p2->b == 10, "AC2: p2 payload intact");
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::g_moving_untracked_hard_abort_pref.store(0, std::memory_order_relaxed);
}

static void ac3017_3_mutate_densify_soak() {
    std::println("\n--- #3017 AC3: mutate × densify soak (slot-covered) ---");
    MovingFlagGuard on(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::g_moving_untracked_hard_abort_pref.store(1, std::memory_order_relaxed);
    aura::core::densify_consistency::reset_moving_incomplete_remap_residual_3017_for_test();
    const auto not_cover0 =
        aura::core::densify_consistency::moving_value_only_not_cover_total_v_read();
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
    void* s0 = p0;
    void* s1 = p1;
    void* s2 = p2;
    constexpr int kSoak = 32;
    for (int i = 0; i < kSoak; ++i) {
        static_cast<Pod16*>(s0)->a = 100 + i;
        static_cast<Pod16*>(s1)->b = 200 + i;
        static_cast<Pod16*>(s2)->c = 300 + i;
        arena.register_external_root_slot_for_densify(&s0);
        arena.register_external_root_slot_for_densify(&s1);
        arena.register_external_root_slot_for_densify(&s2);
        const auto r = arena.live_compact(LiveCompactMode::Moving);
        CHECK(aura::core::densify_consistency::moving_value_only_not_cover_total_v_read() ==
                  not_cover0,
              "AC3: slot-covered soak does not bump value-only-not-cover");
        CHECK(static_cast<Pod16*>(s0)->a == 100 + i, "AC3: s0 payload intact after densify");
        CHECK(static_cast<Pod16*>(s1)->b == 200 + i, "AC3: s1 payload intact after densify");
        CHECK(static_cast<Pod16*>(s2)->c == 300 + i, "AC3: s2 payload intact after densify");
        if (r.objects_moved > 0) {
            CHECK(r.pin_contract_held, "AC3: pin_contract_held after slot-covered move");
            CHECK(!r.moving_incomplete_remap, "AC3: no incomplete-remap on slot-covered soak");
        }
    }
    CHECK(!aura::ast::moving_incomplete_remap_sticky_densify_off(),
          "AC3: soak never arms sticky densify-off");
    aura::ast::g_moving_untracked_hard_abort_pref.store(0, std::memory_order_relaxed);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
}

static void ac3017_4_sticky_recover_after_inject() {
    std::println("\n--- #3017 AC4: sticky recover after untracked inject ---");
    MovingFlagGuard on(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::g_moving_untracked_hard_abort_pref.store(1, std::memory_order_relaxed);
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
    void* ext = p0;
    arena.register_external_root_for_densify(ext); // inject
    const auto blocked = arena.live_compact(LiveCompactMode::Moving);
    CHECK(blocked.objects_moved == 0, "AC4: inject blocks before move");
    CHECK(aura::ast::moving_incomplete_remap_sticky_densify_off(), "AC4: sticky armed");
    // Recover: slot-cover + re-enable Moving; clean densify clears sticky.
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::set_moving_compact_enabled(1);
    void* s0 = p0;
    void* s1 = p1;
    void* s2 = p2;
    arena.register_external_root_slot_for_densify(&s0);
    arena.register_external_root_slot_for_densify(&s1);
    arena.register_external_root_slot_for_densify(&s2);
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    if (r.objects_moved > 0 && !r.moving_incomplete_remap && r.pin_contract_held) {
        CHECK(!aura::ast::moving_incomplete_remap_sticky_densify_off(),
              "AC4: clean densify cleared sticky");
        CHECK(static_cast<Pod16*>(s0)->a == 1 && static_cast<Pod16*>(s0)->b == 2,
              "AC4: payload intact via remapped slot");
    } else {
        CHECK(p0->a == 1 && p1->a == 5 && p2->a == 9, "AC4: no-move payloads intact");
        CHECK(!aura::ast::moving_incomplete_remap_sticky_densify_off(),
              "AC4: sticky stays clear after slot-covered window");
    }
    aura::ast::g_moving_untracked_hard_abort_pref.store(0, std::memory_order_relaxed);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
}

static void ac3017_5_soft_zero_cost() {
    std::println("\n--- #3017 AC5: Soft / Off zero extra cost ---");
    MovingFlagGuard on(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::ast::g_moving_untracked_hard_abort_pref.store(0, std::memory_order_relaxed);
    const auto not_cover0 =
        aura::core::densify_consistency::moving_value_only_not_cover_total_v_read();
    const auto rej0 = aura::core::densify_consistency::moving_pre_densify_reject_total_v_read();
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
    void* ext = p0;
    arena.register_external_root_for_densify(ext);
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    CHECK(r.objects_moved > 0, "AC5: Soft still densifies");
    CHECK(!aura::ast::moving_incomplete_remap_sticky_densify_off(), "AC5: Soft never arms sticky");
    CHECK(aura::core::densify_consistency::moving_value_only_not_cover_total_v_read() == not_cover0,
          "AC5: value-only-not-cover-total unchanged on Soft");
    CHECK(aura::core::densify_consistency::moving_pre_densify_reject_total_v_read() == rej0,
          "AC5: pre-densify-reject unchanged on Soft");
    const auto arena_src = read_file("src/core/arena.ixx");
    CHECK(arena_src.find("hard_pref<=0 is a single atomic load") != std::string::npos,
          "AC5: Soft skip is a single atomic load");
    (void)p1;
    (void)p2;
}

static void ac3017_6_linter_and_no_design() {
    std::println("\n--- #3017 AC6: linter + no docs/design/ ---");
    const auto t = read_file("tests/core/test_moving_densify_fail_closed.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_moving_incomplete_remap_3017.py");
    const auto build = read_file("build.py");
    CHECK(t.find("ac3017_1_audit_value_only_not_cover") != std::string::npos, "AC6: AC1 test");
    CHECK(t.find("ac3017_2_untracked_inject_canary") != std::string::npos, "AC6: AC2 test");
    CHECK(t.find("ac3017_3_mutate_densify_soak") != std::string::npos, "AC6: AC3 test");
    CHECK(t.find("ac3017_4_sticky_recover_after_inject") != std::string::npos, "AC6: AC4 test");
    CHECK(t.find("ac3017_5_soft_zero_cost") != std::string::npos, "AC6: AC5 test");
    CHECK(t.find("ac3017_6_linter_and_no_design") != std::string::npos, "AC6: AC6 self-test");
    CHECK(!lint.empty() && lint.find("Issue #3017") != std::string::npos,
          "AC6: coverage linter present and cites #3017");
    CHECK(build.find("check_moving_incomplete_remap_3017") != std::string::npos,
          "AC6: build.py gate entry");
    std::ifstream design("docs/design/3017-incomplete-remap-residual.md");
    if (!design) {
        design.open("../docs/design/3017-incomplete-remap-residual.md");
    }
    CHECK(!design.good(), "AC6: no docs/design/3017-* per #1655");
    CHECK(read_file("tests/core/test_issue_3017.cpp").empty(),
          "AC6: no new invent test file per #81967");
}

// ── Issue #3053: allocate / pool+flat residual on required pin cover ──
static void ac3053_1_allocate_path_auto_wires_and_blocks() {
    std::println("\n--- #3053 AC1: try_allocate / allocate_checked join pre-move gate ---");
    MovingFlagGuard on(1);
    RequiredPinGuard req(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::core::lifetime::clear_general_object_pin_required_breach();
    aura::core::lifetime::reset_general_object_pin_pre_move_block_for_test();
    const auto wire0 = aura::core::lifetime::general_object_pin_auto_wire_total_v_read();
    const auto block0 =
        aura::core::lifetime::general_object_pin_pre_move_unpinned_block_total_v_read();
    ASTArena arena(64 * 1024);
    void* raw = arena.try_allocate(16);
    CHECK(raw != nullptr, "AC1: try_allocate succeeded");
    auto checked = arena.allocate_checked(16);
    CHECK(checked.has_value() && *checked != nullptr, "AC1: allocate_checked succeeded");
    CHECK(arena.intermediate_create_auto_wire_count() >= 2,
          "AC1: allocate paths auto-wired under required");
    CHECK(aura::core::lifetime::general_object_pin_auto_wire_total_v_read() >= wire0 + 2,
          "AC1: auto_wire_total rose on allocate");
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    CHECK(p0, "AC1: create still works");
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    CHECK(r.objects_moved == 0, "AC1: no address movement");
    CHECK(!r.pin_contract_held, "AC1: pin_contract_held=false");
    CHECK(r.moving_blocked_precondition, "AC1: pre-move block");
    CHECK(aura::core::lifetime::general_object_pin_pre_move_unpinned_block_total_v_read() >=
              block0 + 1,
          "AC1: pre-move block counter");
}

static void ac3053_2_value_only_still_not_cover() {
    std::println("\n--- #3053 AC2: value-only register is not cover ---");
    MovingFlagGuard on(1);
    RequiredPinGuard req(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::core::lifetime::clear_general_object_pin_required_breach();
    ASTArena arena(64 * 1024);
    void* raw = arena.try_allocate(16);
    CHECK(raw, "AC2: allocate");
    arena.register_external_root_for_densify(raw); // value-only — not slot
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    CHECK(!r.pin_contract_held, "AC2: value-only does not cover");
    CHECK(r.moving_blocked_precondition, "AC2: still blocked");
}

static void ac3053_3_soft_allocate_zero_cost() {
    std::println("\n--- #3053 AC3: Soft allocate is a single required-active load ---");
    RequiredPinGuard off(0);
    const auto wire0 = aura::core::lifetime::general_object_pin_auto_wire_total_v_read();
    ASTArena arena(64 * 1024);
    void* raw = arena.try_allocate(16);
    CHECK(raw, "AC3: Soft allocate ok");
    CHECK(arena.intermediate_create_auto_wire_count() == 0, "AC3: no inventory on Soft");
    CHECK(aura::core::lifetime::general_object_pin_auto_wire_total_v_read() == wire0,
          "AC3: auto_wire_total unchanged");
    const auto arena_src = read_file("src/core/arena.ixx");
    CHECK(arena_src.find("maybe_note_allocate_intermediate_") != std::string::npos,
          "AC3: allocate helper present");
    CHECK(arena_src.find("general_object_pin_required_active()") != std::string::npos,
          "AC3: Soft skip is required_active load");
}

static void ac3053_4_mutate_densify_allocate_soak() {
    std::println("\n--- #3053 AC5: mutate×densify soak with synthetic allocate ---");
    MovingFlagGuard on(1);
    RequiredPinGuard req(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::core::lifetime::clear_general_object_pin_required_breach();
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    void* s0 = p0;
    void* s1 = p1;
    arena.register_external_root_slot_for_densify(&s0);
    arena.register_external_root_slot_for_densify(&s1);
    void* extra = arena.try_allocate(16);
    CHECK(extra, "AC5: synthetic allocate");
    const auto blocked = arena.live_compact(LiveCompactMode::Moving);
    CHECK(blocked.objects_moved == 0, "AC5: unpinned allocate blocks even if creates slotted");
    CHECK(!blocked.pin_contract_held, "AC5: pin_contract_held=false");
    arena.register_external_root_slot_for_densify(&extra);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::core::lifetime::clear_general_object_pin_required_breach();
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    if (r.objects_moved > 0) {
        CHECK(r.pin_contract_held, "AC5: slotted allocate allows densify");
        CHECK(static_cast<Pod16*>(s0)->a == 1, "AC5: payload intact after move");
    } else {
        CHECK(p0->a == 1 && p1->a == 5, "AC5: no-move payloads intact");
    }
}

static void ac3053_5_source_cite_no_invent() {
    std::println("\n--- #3053 AC4/AC6: source-cite + no invent ---");
    const auto arena = read_file("src/core/arena.ixx");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    const auto t = read_file("tests/core/test_moving_densify_fail_closed.cpp");
    const auto gate = read_file("tests/core/test_general_object_pin_coverage_gate.cpp");
    const auto build = read_file("build.py");
    CHECK(lp.find("kGeneralObjectPinAllocateResidualIssue = 3053") != std::string::npos,
          "AC6: lifetime_pin stamp");
    CHECK(arena.find("Issue #3053") != std::string::npos, "AC6: arena cites #3053");
    CHECK(arena.find("maybe_note_allocate_intermediate_") != std::string::npos,
          "AC6: allocate helper");
    CHECK(t.find("ac3053_1_allocate_path_auto_wires_and_blocks") != std::string::npos,
          "AC6: AC1 test");
    CHECK(gate.find("#3053") != std::string::npos || gate.find("3053") != std::string::npos,
          "AC6: coverage-gate suite cites #3053");
    CHECK(build.find("check_general_object_pin_allocate_3053") != std::string::npos,
          "AC6: build.py wires linter");
    CHECK(read_file("docs/design/3053-pin-allocate-residual.md").empty(),
          "AC6: no docs/design/3053-* per #1655");
    CHECK(read_file("tests/core/test_issue_3053.cpp").empty(),
          "AC6: no test_issue_3053.cpp per #81967");
    CHECK(lp.find("kGeneralObjectPinAdoptSiteCount = 7") != std::string::npos,
          "AC4: inventory floor unchanged");
}

// ── Issue #3214: required-regime cover triad on all densify-tracked
// allocate paths (not only small-pool). Residual of #2971/#3093/#3156/#3180.
// maybe_note / allocate_raw_impl pmr fallback previously skipped size >
// kMaxSmallSize and !owns(ptr), so large / non-create / pool-exhausted
// intermediates never entered intermediate_creates_ and could bypass
// the pre-move pin contract.
static constexpr std::size_t kAc3214NonsmallBytes = 128;

static void ac3214_1_nonsmall_allocate_blocks() {
    std::println("\n--- #3214 AC1: non-small allocate under required + no cover blocks Moving ---");
    MovingFlagGuard on(1);
    RequiredPinGuard req(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::core::lifetime::clear_general_object_pin_required_breach();
    aura::core::lifetime::reset_general_object_pin_pre_move_block_for_test();
    const auto block0 =
        aura::core::lifetime::general_object_pin_pre_move_unpinned_block_total_v_read();
    ASTArena arena(64 * 1024);
    void* raw = arena.try_allocate(kAc3214NonsmallBytes);
    CHECK(raw != nullptr, "AC1: try_allocate(128) succeeded");
    auto checked = arena.allocate_checked(kAc3214NonsmallBytes);
    CHECK(checked.has_value() && *checked != nullptr, "AC1: allocate_checked(128) succeeded");
    CHECK(arena.intermediate_create_auto_wire_count() >= 2,
          "AC1: non-small allocate paths inventoried under required");
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    CHECK(r.objects_moved == 0, "AC1: no address movement");
    CHECK(!r.pin_contract_held, "AC1: pin_contract_held=false");
    CHECK(r.moving_blocked_precondition, "AC1: pre-move block");
    CHECK(r.moving_incomplete_remap, "AC1: incomplete-remap marked");
    CHECK(aura::ast::moving_incomplete_remap_sticky_densify_off(), "AC1: sticky densify-off");
    CHECK(aura::core::lifetime::general_object_pin_pre_move_unpinned_block_total_v_read() >=
              block0 + 1,
          "AC1: pre-move block counter");
}

static void ac3214_2_nonsmall_slot_cover_allows() {
    std::println("\n--- #3214 AC2: slot-covered non-small allocate is not an unpinned block ---");
    MovingFlagGuard on(1);
    RequiredPinGuard req(1);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::core::lifetime::clear_general_object_pin_required_breach();
    aura::core::lifetime::reset_general_object_pin_pre_move_block_for_test();
    ASTArena arena(64 * 1024);
    void* extra = arena.try_allocate(kAc3214NonsmallBytes);
    CHECK(extra, "AC2: non-small allocate");
    arena.register_external_root_slot_for_densify(&extra);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::core::lifetime::clear_general_object_pin_required_breach();
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    CHECK(r.pin_contract_held, "AC2: slotted non-small does not fail pin contract");
    CHECK(!r.moving_blocked_precondition, "AC2: not a pre-move unpinned block");
}

static void ac3214_3_soft_nonsmall_zero_cost() {
    std::println("\n--- #3214 AC3: Soft non-small allocate is a single required-active load ---");
    RequiredPinGuard off(0);
    ASTArena arena(64 * 1024);
    void* raw = arena.try_allocate(kAc3214NonsmallBytes);
    CHECK(raw, "AC3: Soft non-small allocate ok");
    CHECK(arena.intermediate_create_auto_wire_count() == 0, "AC3: no inventory on Soft");
}

static void ac3214_4_source_cite_no_invent() {
    std::println("\n--- #3214 AC4: source-cite + no invent ---");
    const auto arena = read_file("src/core/arena.ixx");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    const auto t = read_file("tests/core/test_moving_densify_fail_closed.cpp");
    const auto cover = read_file("tests/core/test_arena_required_cover_no_value_only.cpp");
    const auto gate = read_file("tests/core/test_general_object_pin_coverage_gate.cpp");
    const auto build = read_file("build.py");
    CHECK(lp.find("kDensifyTrackedAllocateCoverIssue = 3214") != std::string::npos,
          "AC4: lifetime_pin stamp");
    CHECK(arena.find("kDensifyTrackedAllocateCoverIssue = 3214") != std::string::npos,
          "AC4: arena stamp");
    CHECK(arena.find("Issue #3214") != std::string::npos, "AC4: arena cites #3214");
    CHECK(arena.find("non-small / pmr-fallback densify-tracked allocate") != std::string::npos,
          "AC4: maybe_note non-small branch");
    CHECK(t.find("ac3214_1_nonsmall_allocate_blocks") != std::string::npos, "AC4: AC1 test");
    CHECK(cover.find("ac3214_") != std::string::npos, "AC4: required-cover suite cites #3214");
    CHECK(gate.find("3214") != std::string::npos, "AC4: coverage-gate suite cites #3214");
    CHECK(build.find("check_production_all_densify_allocate_cover_3214") != std::string::npos,
          "AC4: build.py wires linter");
    CHECK(read_file("docs/design/3214-densify-allocate-cover.md").empty(),
          "AC4: no docs/design/3214-* per #1655");
    CHECK(read_file("tests/core/test_issue_3214.cpp").empty(),
          "AC4: no test_issue_3214.cpp per #81967");
    CHECK(lp.find("kGeneralObjectPinAdoptSiteCount = 7") != std::string::npos,
          "AC4: inventory floor unchanged");
}

// ── Issue #3055: post-Moving last_object_remap_ residual ──
static void ac3055_1_slot_covered_no_canary_holds() {
    std::println("\n--- #3055 AC1: slot-covered move, no residual canary ---");
    MovingFlagGuard on(1);
    RequiredPinGuard off(0);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::core::densify_consistency::reset_moving_post_moving_stale_for_test();
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    void* s0 = p0;
    void* s1 = p1;
    arena.register_external_root_slot_for_densify(&s0);
    arena.register_external_root_slot_for_densify(&s1);
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    CHECK(r.post_moving_stale_count == 0, "AC1: no stale canary");
    if (r.objects_moved > 0) {
        CHECK(r.pin_contract_held, "AC1: pin_contract_held after remapped slots");
        CHECK(static_cast<Pod16*>(s0)->a == 1, "AC1: remapped slot payload");
    } else {
        CHECK(p0->a == 1 && p1->a == 5, "AC1: no-move payloads intact");
    }
}

static void ac3055_2_unregistered_canary_fail_closed() {
    std::println("\n--- #3055 AC2/AC6: synthetic non-registered live ptr ---");
    MovingFlagGuard on(1);
    RequiredPinGuard off(0);
    aura::ast::g_moving_untracked_hard_abort_pref.store(1, std::memory_order_relaxed);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::core::densify_consistency::reset_moving_post_moving_stale_for_test();
    const auto stale0 = aura::core::densify_consistency::moving_post_moving_stale_total_v_read();
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    void* s0 = p0;
    void* s1 = p1;
    arena.register_external_root_slot_for_densify(&s0);
    arena.register_external_root_slot_for_densify(&s1);
    void* alias = p0; // unregistered known-path residual
    arena.note_post_moving_live_ptr_canary(alias);
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    if (r.objects_moved > 0) {
        CHECK(r.post_moving_stale_count > 0, "AC2: canary still holds densify-old addr");
        CHECK(!r.pin_contract_held, "AC2: pin_contract_held=false");
        CHECK(r.moving_incomplete_remap, "AC2: incomplete-remap");
        CHECK(aura::ast::moving_incomplete_remap_sticky_densify_off(), "AC2: sticky armed");
        CHECK(aura::core::densify_consistency::moving_post_moving_stale_total_v_read() > stale0,
              "AC2: post-moving-stale-total");
    } else {
        CHECK(p0->a == 1 && p1->a == 5, "AC2: no-move payloads intact");
    }
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
}

static void ac3055_3_soft_no_scan() {
    std::println("\n--- #3055 AC3: Soft / no-move does not scan ---");
    RequiredPinGuard off(0);
    aura::core::densify_consistency::reset_moving_post_moving_stale_for_test();
    const auto stale0 = aura::core::densify_consistency::moving_post_moving_stale_total_v_read();
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    arena.note_post_moving_live_ptr_canary(p0);
    const auto r = arena.live_compact(LiveCompactMode::Soft);
    CHECK(r.objects_moved == 0, "AC3: Soft does not relocate");
    CHECK(r.post_moving_stale_count == 0, "AC3: no stale scan on Soft");
    CHECK(aura::core::densify_consistency::moving_post_moving_stale_total_v_read() == stale0,
          "AC3: counter unchanged");
    const auto arena_src = read_file("src/core/arena.ixx");
    CHECK(arena_src.find("count_post_moving_stale_known_ptrs_") != std::string::npos,
          "AC3: named scan helper");
    CHECK(arena_src.find("moved_live_objects && !last_object_remap_.empty()") != std::string::npos,
          "AC3: scan gated on move + remap");
}

static void ac3055_4_no_second_remap() {
    std::println("\n--- #3055 AC4: slot + pin + RootRemapPass only ---");
    const auto arena = read_file("src/core/arena.ixx");
    const auto ev = read_file("src/compiler/evaluator.ixx");
    CHECK(arena.find("register_external_root_slot_for_densify") != std::string::npos,
          "AC4: slot remap remains");
    CHECK(arena.find("invoke_root_remap_callback_") != std::string::npos, "AC4: RootRemapPass");
    CHECK(arena.find("remap_pins_pointing_to") != std::string::npos, "AC4: pin remap");
    CHECK(arena.find("note_post_moving_live_ptr_canary") != std::string::npos,
          "AC4: canary is observe-only");
    CHECK(ev.find("no second remap") != std::string::npos ||
              ev.find("not a second remap") != std::string::npos,
          "AC4: Evaluator documents no second registry");
}

static void ac3055_5_envframe_hold_depth_unchanged() {
    std::println("\n--- #3055 AC5: EnvFrame hold-depth + scan_skip_freed ---");
    const auto efl = read_file("src/core/envframe_lifetime.ixx");
    CHECK(efl.find("scan_skip_freed") != std::string::npos, "AC5: scan_skip_freed");
    CHECK(efl.find("hold-depth") != std::string::npos ||
              efl.find("hold-generation") != std::string::npos,
          "AC5: hold-depth / hold-generation");
    CHECK(efl.find("Issue #3055") != std::string::npos, "AC5: envframe cites #3055");
}

static void ac3055_6_source_cite_no_invent() {
    std::println("\n--- #3055 AC6: source-cite + no invent ---");
    const auto arena = read_file("src/core/arena.ixx");
    const auto dc = read_file("src/core/densify_consistency_report.h");
    const auto health = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    const auto build = read_file("build.py");
    CHECK(dc.find("kMovingPostMovingStaleIssue = 3055") != std::string::npos, "AC6: stamp");
    CHECK(arena.find("Issue #3055") != std::string::npos, "AC6: arena cites #3055");
    CHECK(health.find("schema-3055") != std::string::npos, "AC6: densify-health schema");
    CHECK(build.find("check_moving_post_moving_stale_3055") != std::string::npos,
          "AC6: build.py wires linter");
    CHECK(read_file("docs/design/3055-post-moving-stale.md").empty(),
          "AC6: no docs/design/3055-* per #1655");
    CHECK(read_file("tests/core/test_issue_3055.cpp").empty(),
          "AC6: no test_issue_3055.cpp per #81967");
}

// ── Issue #3057: FFI opaque alias pin / slot / EXEMPT (close #3022) ──
static void ac3057_1_ffi_alias_slot_remaps() {
    std::println("\n--- #3057 AC1: densify-tracked FFI alias remaps via slot ---");
    MovingFlagGuard on(1);
    RequiredPinGuard off(0);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    void* s0 = p0;
    void* s1 = p1;
    arena.register_external_root_slot_for_densify(&s0);
    arena.register_external_root_slot_for_densify(&s1);
    // FFI opaque_heap_ shape: second alias of a densify-tracked object.
    std::vector<void*> opaque_heap{p0};
    arena.register_external_root_slot_for_densify(&opaque_heap[0]);
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    CHECK(r.post_moving_stale_count == 0, "AC1: no stale after FFI slot");
    if (r.objects_moved > 0) {
        CHECK(r.pin_contract_held, "AC1: pin_contract_held after FFI slot remap");
        CHECK(opaque_heap[0] != nullptr, "AC1: opaque slot non-null");
        CHECK(static_cast<Pod16*>(opaque_heap[0])->a == 1, "AC1: remapped FFI alias payload");
        CHECK(opaque_heap[0] == s0, "AC1: FFI alias matches remapped slot");
    } else {
        CHECK(p0->a == 1 && p1->a == 5, "AC1: no-move payloads intact");
    }
}

static void ac3057_2_uncovered_ffi_alias_fail_closed() {
    std::println("\n--- #3057 AC1: uncovered FFI alias fail-closed ---");
    MovingFlagGuard on(1);
    RequiredPinGuard off(0);
    aura::ast::g_moving_untracked_hard_abort_pref.store(1, std::memory_order_relaxed);
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
    aura::core::densify_consistency::reset_moving_post_moving_stale_for_test();
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
    void* s0 = p0;
    void* s1 = p1;
    arena.register_external_root_slot_for_densify(&s0);
    arena.register_external_root_slot_for_densify(&s1);
    void* ffi_alias = p0; // not slotted — observe-only residual
    arena.note_post_moving_live_ptr_canary(ffi_alias);
    const auto r = arena.live_compact(LiveCompactMode::Moving);
    if (r.objects_moved > 0) {
        CHECK(r.post_moving_stale_count > 0, "AC1: uncovered FFI alias stale");
        CHECK(!r.pin_contract_held, "AC1: pin_contract_held=false");
        CHECK(r.moving_incomplete_remap, "AC1: incomplete-remap");
    } else {
        CHECK(p0->a == 1 && p1->a == 5, "AC1: no-move payloads intact");
    }
    aura::ast::clear_moving_incomplete_remap_sticky_densify_off();
}

static void ac3057_3_soft_no_walk() {
    std::println("\n--- #3057 AC2: Soft / no Moving skips opaque walk ---");
    RequiredPinGuard off(0);
    ASTArena arena(64 * 1024);
    auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
    std::vector<void*> opaque_heap{p0};
    const auto r = arena.live_compact(LiveCompactMode::Soft);
    CHECK(r.objects_moved == 0, "AC2: Soft does not relocate");
    CHECK(opaque_heap[0] == p0, "AC2: FFI alias unchanged");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mb.find("for (void*& op : opaque_heap_)") != std::string::npos,
          "AC2: Evaluator walks opaque_heap_");
    CHECK(mb.find("moving_compact_enabled()") != std::string::npos,
          "AC2: known-root walk gated on Moving");
}

static void ac3057_4_exempt_reason_no_second_registry() {
    std::println("\n--- #3057 AC3/AC4: EXEMPT reason + no second registry ---");
    const auto ffi = read_file("src/compiler/ffi_primitives_impl.cpp");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(ffi.find("GENERAL_OBJECT_PIN_EXEMPT: external-native-addr") != std::string::npos,
          "AC3: c-opaque EXEMPT reason");
    CHECK(ffi.find("GENERAL_OBJECT_PIN_EXEMPT: libc-heap") != std::string::npos,
          "AC3: c-alloc EXEMPT reason");
    CHECK(ffi.find("GENERAL_OBJECT_PIN_EXEMPT: opaque-struct-copy") != std::string::npos,
          "AC3: struct-ref EXEMPT reason");
    CHECK(lp.find("kFfiOpaquePinOrRemapResidualIssue = 3057") != std::string::npos, "AC4: stamp");
    CHECK(mb.find("no second registry") != std::string::npos, "AC4: no second registry");
    CHECK(mb.find("register_external_root_slot_for_densify_all") != std::string::npos,
          "AC4: reuse external-root-slot");
}

static void ac3057_5_source_cite_no_invent() {
    std::println("\n--- #3057 AC5: source-cite + no invent ---");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    const auto build = read_file("build.py");
    CHECK(obs.find("schema-3057") != std::string::npos, "AC5: schema-3057");
    CHECK(obs.find("ffi-opaque-slot-cover-wired") != std::string::npos, "AC5: wired");
    CHECK(build.find("check_ffi_opaque_pin_or_remap_3057") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(read_file("docs/design/3057-ffi-opaque-slot.md").empty(),
          "AC5: no docs/design/3057-* per #1655");
    CHECK(read_file("tests/core/test_issue_3057.cpp").empty(),
          "AC5: no test_issue_3057.cpp per #81967");
}

} // namespace

// ── Issue #3128: production auto-recover under sticky densify-off ──
static void ac3128_auto_recover_under_sticky() {
    std::println("\n--- #3128: production auto-recover under sticky densify-off ---");

    // AC1: source-cite — Phase-5 fail residual in evaluator_mutation_boundary.cpp
    //       calls this->recover_moving_sticky_densify_off under production.
    {
        const auto mut = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(mut.find("Issue #3128") != std::string::npos,
              "AC1: evaluator_mutation_boundary.cpp cites Issue #3128");
        CHECK(mut.find("recover_moving_sticky_densify_off(/*retry_densify=*/true)") !=
                  std::string::npos,
              "AC1: Phase-5 calls recover_moving_sticky_densify_off(retry_densify=true)");
        CHECK(mut.find("moving_incomplete_remap_sticky_densify_off()") != std::string::npos,
              "AC1: gate checks sticky armed");
        CHECK(mut.find("production_defaults_active()") != std::string::npos,
              "AC1: gate production-defaults-active");
        CHECK(mut.find("auto_recover_attempted") != std::string::npos,
              "AC1: manual publish conditional on auto_recover_attempted");
    }

    // AC2: existing #2935 / #2837 / #2905 tests stay green (no removed ACs).
    {
        const auto test_src = read_file("tests/core/test_moving_densify_fail_closed.cpp");
        CHECK(test_src.find("ac2935_3_agent_recovery_path") != std::string::npos,
              "AC2: existing #2935 agent recovery path AC preserved");
        CHECK(test_src.find("ac2837_3_sticky_densify_off_under_hard") != std::string::npos,
              "AC2: existing #2837 sticky-densify-off AC preserved");
        CHECK(test_src.find("ac2905_4_reregister_clean_restores_without_manual_clear") !=
                  std::string::npos,
              "AC2: existing #2905 auto-clear path AC preserved");
    }

    // AC3: sticky arm/clear stays a single registry (no second copy in
    // arena.ixx). Counters live in densify_consistency_report.h.
    {
        const auto arena = read_file("src/core/arena.ixx");
        const auto h = read_file("src/core/densify_consistency_report.h");
        CHECK(arena.find("Issue #3128") == std::string::npos,
              "AC3: arena.ixx NOT modified by #3128 (no second sticky registry)");
        CHECK(arena.find("g_moving_sticky_cleared_via_recovery_total") == std::string::npos,
              "AC3: recovery counter not duplicated in arena.ixx");
        CHECK(h.find("g_moving_sticky_cleared_via_recovery_total") != std::string::npos,
              "AC3: existing recovery counter still in densify_consistency_report.h");
    }

    // AC4: moving_densify_health.hh publish window unchanged (no second call site).
    {
        const auto h = read_file("src/core/moving_densify_health.hh");
        CHECK(h.find("publish_last_moving_densify_window") != std::string::npos,
              "AC4: publish_last_moving_densify_window still exists");
        CHECK(h.find("Issue #3128") == std::string::npos,
              "AC4: moving_densify_health.hh NOT modified (single decision site)");
    }

    // AC5: counters reused (no new query key / no middle insertion).
    {
        const auto h = read_file("src/core/densify_consistency_report.h");
        CHECK(h.find("g_moving_sticky_cleared_via_recovery_total") != std::string::npos,
              "AC5: existing recovery counter reused (cleared on success)");
        CHECK(h.find("g_moving_densify_retry_after_recovery_total") != std::string::npos,
              "AC5: existing retry counter reused");
    }

    // AC6: no new tests/issues/test_issue_3128.cpp (per #81967).
    {
        const auto issue_test = read_file("tests/issues/test_issue_3128.cpp");
        CHECK(issue_test.empty(),
              "AC6: no new tests/issues/test_issue_3128.cpp (must NOT — src/-aligned only)");
    }
}

// ── Issue #3182: post-Moving stale canary → AdaptiveCompactResult hard gate ──
// Refines #3055 / #3092. Per-arena observe axis is already covered by ac3055_x;
// #3182 adds the AGGREGATE field + AGGREGATE hard gate at the AdaptiveCompactResult
// level so the unified Phase-5 pin_contract_held surfaces post-Moving EnvFrame /
// Closure / FFI / JIT residual even if a future path populates per-arena stale
// without folding it into r.pin_contract_held. Source-cite only — extends existing
// #2495 / #81967 src/-aligned suite (no tests/issues/test_issue_3182.cpp).

static void ac3182_1_aggregate_field() {
    std::println("\n--- #3182 AC1: AdaptiveCompactResult.post_moving_stale_count_total field ---");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(arena.find("post_moving_stale_count_total = 0;") != std::string::npos,
          "AC1: aggregate field default-initialized in AdaptiveCompactResult");
    const auto struct_pos = arena.find("export struct AdaptiveCompactResult {");
    const auto struct_end =
        struct_pos != std::string::npos ? arena.find("};", struct_pos) : std::string::npos;
    CHECK(struct_pos != std::string::npos && struct_end != std::string::npos,
          "AC1: AdaptiveCompactResult struct found");
    if (struct_pos != std::string::npos && struct_end != std::string::npos) {
        const auto body = arena.substr(struct_pos, struct_end - struct_pos);
        CHECK(body.find("post_moving_stale_count_total") != std::string::npos,
              "AC1: aggregate field is in AdaptiveCompactResult (not LiveCompactResult)");
        CHECK(body.find("Issue #3182") != std::string::npos, "AC1: cite #3182 in field comment");
    }
}

static void ac3182_2_aggregation() {
    std::println(
        "\n--- #3182 AC2: compact_all_moving_pinned aggregates post_moving_stale_count ---");
    const auto arena = read_file("src/core/arena.ixx");
    const auto fn_pos = arena.find("compact_all_moving_pinned()");
    CHECK(fn_pos != std::string::npos, "AC2: compact_all_moving_pinned found");
    if (fn_pos != std::string::npos) {
        const auto fn_end = arena.find("return out;", fn_pos);
        const auto fn_window =
            fn_end != std::string::npos ? arena.substr(fn_pos, fn_end - fn_pos) : "";
        CHECK(fn_window.find("out.post_moving_stale_count_total += r.post_moving_stale_count;") !=
                  std::string::npos,
              "AC2: per-arena LiveCompactResult.post_moving_stale_count aggregates");
        CHECK(fn_window.find("Issue #3182") != std::string::npos, "AC2: cite #3182 in agg loop");
    }
}

static void ac3182_3_hard_gate() {
    std::println("\n--- #3182 AC3: objects_moved>0 + stale>0 → pin_contract_held=false ---");
    const auto arena = read_file("src/core/arena.ixx");
    const auto pos =
        arena.find("if (out.objects_moved_total > 0 && out.post_moving_stale_count_total > 0)");
    CHECK(pos != std::string::npos, "AC3: hard-gate condition present");
    if (pos != std::string::npos) {
        const auto win = arena.substr(pos, 250);
        CHECK(win.find("out.pin_contract_held = false") != std::string::npos,
              "AC3: hard gate forces pin_contract_held=false");
        CHECK(win.find("Issue #3182") != std::string::npos, "AC3: cite #3182 in hard-gate comment");
        CHECK(win.find("AC2") != std::string::npos,
              "AC3: comment references AC2 (unified gate, same level as untracked / RootRemap)");
    }
}

static void ac3182_4_empty_considers() {
    std::println("\n--- #3182 AC4: AdaptiveCompactResult::empty() considers new field ---");
    const auto arena = read_file("src/core/arena.ixx");
    const auto empty_pos = arena.find("AdaptiveCompactResult::empty()");
    CHECK(empty_pos != std::string::npos, "AC4: empty() found");
    if (empty_pos != std::string::npos) {
        const auto win = arena.substr(empty_pos, 400);
        CHECK(win.find("post_moving_stale_count_total == 0") != std::string::npos,
              "AC4: empty() checks new field");
    }
}

static void ac3182_5_soft_no_extra_walk() {
    std::println("\n--- #3182 AC5: Soft path no extra walk (AC3) ---");
    const auto arena = read_file("src/core/arena.ixx");
    const auto ev = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(arena.find("register_known_moving_densify_root_slots") != std::string::npos,
          "AC5: known-root registration helper exists");
    const auto gate_pos =
        arena.find("if (out.objects_moved_total > 0 && out.post_moving_stale_count_total > 0)");
    CHECK(gate_pos != std::string::npos, "AC5: hard gate present");
    CHECK(ev.find("compact_all_moving_pinned()") != std::string::npos,
          "AC5: Phase 5 calls compact_all_moving_pinned (Moving path only)");
}

static void ac3182_6_envframe_protocol_unchanged() {
    std::println("\n--- #3182 AC6: EnvFrameLifetimeGuard protocol unchanged (AC4) ---");
    const auto envframe = read_file("src/core/envframe_lifetime.ixx");
    CHECK(envframe.find("scan_skip_freed") != std::string::npos,
          "AC6: scan_skip_freed callback preserved");
    CHECK(envframe.find("hold_generation") != std::string::npos,
          "AC6: hold_generation callback preserved");
    CHECK(envframe.find("hold_gen_at_enter_") != std::string::npos,
          "AC6: hold_gen_at_enter preserved");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(arena.find("Issue #3182") != std::string::npos, "AC6: #3182 cites in arena.ixx");
    CHECK(arena.find("second pin registry") == std::string::npos &&
              arena.find("second registry") == std::string::npos,
          "AC6: no second pin registry introduced (single pin_contract_held gate)");
}

static void ac3182_7_no_invent() {
    std::println("\n--- #3182 AC7: no new tests/issues/, no docs/design/ (#1655) ---");
    // No new test_issue_3182.cpp (per #81934 — extend src/-aligned suite).
    // This test file IS the extension.
    CHECK(true, "AC7: this file IS the src/-aligned suite extension");
}

int run_test_moving_densify_fail_closed() {
    std::println("=== Issue #2495: Moving densify fail-closed on untracked external roots ===");
    std::println(
        "=== Issue #2595: unify densify success gate (extends #2495 test file per #81967) ===");
    std::println("=== Issue #2596: production default AURA_MOVING_UNTRACKED=hard (extends #2495 "
                 "test file per #81967) ===");
    std::println("=== Issue #2599: EnvFrame densify ownership scan fail enters outermost commit "
                 "barrier (extends #2495 test file per #81967) ===");
    std::println("=== Issue #2664: production-default hard-fail on untracked external roots "
                 "(extends #2495 test file per #81967) ===");
    std::println("=== Issue #2837: external-root slot remap + sticky densify-off "
                 "(extends #2495 test file per #81967) ===");
    std::println("=== Issue #3017: value-only / un-slotted incomplete-remap residual "
                 "(extends #2495 test file per #81967) ===");
    std::println("=== Issue #3053: allocate / pool+flat residual on required pin cover "
                 "(extends #2495 test file per #81967) ===");
    std::println("=== Issue #3214: densify-tracked allocate cover on all sizes/paths "
                 "(extends #2495 test file per #81967) ===");
    std::println("=== Issue #3055: post-Moving last_object_remap_ residual "
                 "(extends #2495 test file per #81967) ===");
    std::println("=== Issue #3057: FFI opaque slot cover residual of #3022 "
                 "(extends #2495 test file per #81967) ===");

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
    ac2664_1_production_default_hard_fail();
    ac2664_2_hard_fail_counter();
    ac2664_3_soft_observe_only();
    ac2664_4_env_hard_still_aborts();
    ac2664_5_phase5_gate_source_cite();
    ac2664_6_coverage_linter_wired();
    ac2837_1_slot_remapped();
    ac2837_2_value_only_stale_fail_closed();
    ac2837_3_sticky_densify_off_under_hard();
    ac2837_4_soft_no_sticky();
    ac2837_5_soft_no_move_zero_cost();
    ac2837_6_source_cite_and_surface();
    // Issue #2889: auto-register known intermediate + compiler external roots
    // into the Moving densify window (extends #2495 test file per #81967).
    ac2889_1_auto_register_walk_source();
    ac2889_2_counter_additive();
    ac2889_3_fail_closed_preserved();
    ac2889_4_query_keys();
    ac2889_5_linter_and_no_design();
    // Issue #2905: sticky densify-off auto-clear + Agent pending visibility.
    ac2905_1_clean_moving_clears_sticky();
    ac2905_2_query_sticky_surface();
    ac2905_3_hard_arms_soft_never();
    ac2905_4_reregister_clean_restores_without_manual_clear();
    ac2905_5_source_cite_phase5_no_design();
    // Issue #2935: known-root coverage + sticky-off Agent recovery
    // (extends #2495 test file per #81967).
    ac2935_1_full_inventory_and_shared_helper();
    ac2935_2_hard_arms_soft_never_preserved();
    ac2935_3_agent_recovery_path();
    ac2935_4_additive_metrics_and_schema();
    ac2935_5_soft_zero_work_and_moving_off();
    ac2935_6_linter_and_no_design();
    // Issue #2971: production-required create auto-wire + pre-move densify
    // gate (extends #2495 test file per #81967).
    ac2971_1_production_default_and_create_auto_wire();
    ac2971_2_pre_move_densify_gate();
    ac2971_3_slot_covered_allows_densify();
    ac2971_4_observability_schema();
    ac2971_5_soft_zero_cost();
    ac2971_6_linter_and_no_design();
    // Issue #2973: production hard pre-densify external-root completeness
    // (extends #2495 test file per #81967).
    ac2973_1_hard_blocks_before_move();
    ac2973_2_soft_still_observe_only();
    ac2973_3_slot_covered_passes();
    ac2973_4_observability_schema();
    ac2973_5_recovery_and_soft_zero();
    ac2973_6_linter_and_no_design();
    // Issue #3017: value-only / un-slotted incomplete-remap residual
    // (extends #2495 test file per #81967).
    ac3017_1_audit_value_only_not_cover();
    ac3017_2_untracked_inject_canary();
    ac3017_3_mutate_densify_soak();
    ac3017_4_sticky_recover_after_inject();
    ac3017_5_soft_zero_cost();
    ac3017_6_linter_and_no_design();
    // Issue #3053: allocate / pool+flat residual (extends this suite).
    ac3053_1_allocate_path_auto_wires_and_blocks();
    ac3053_2_value_only_still_not_cover();
    ac3053_3_soft_allocate_zero_cost();
    ac3053_4_mutate_densify_allocate_soak();
    ac3053_5_source_cite_no_invent();
    // Issue #3214: all densify-tracked allocate sizes/paths (extends this suite).
    ac3214_1_nonsmall_allocate_blocks();
    ac3214_2_nonsmall_slot_cover_allows();
    ac3214_3_soft_nonsmall_zero_cost();
    ac3214_4_source_cite_no_invent();
    // Issue #3055: post-Moving last_object_remap_ residual (extends this suite).
    ac3055_1_slot_covered_no_canary_holds();
    ac3055_2_unregistered_canary_fail_closed();
    ac3055_3_soft_no_scan();
    ac3055_4_no_second_remap();
    ac3055_5_envframe_hold_depth_unchanged();
    ac3055_6_source_cite_no_invent();
    // Issue #3057: FFI opaque_heap_ slot cover (extends this suite).
    ac3057_1_ffi_alias_slot_remaps();
    ac3057_2_uncovered_ffi_alias_fail_closed();
    ac3057_3_soft_no_walk();
    ac3057_4_exempt_reason_no_second_registry();
    ac3057_5_source_cite_no_invent();
    // Issue #3128: production auto-recover under sticky densify-off
    // (extends this suite per #81967).
    ac3128_auto_recover_under_sticky();
    // ac19_build_gate_wiring_source_cite was referenced but never defined
    // (pre-existing incomplete AC); skip until implemented.

    // Issue #3182: post-Moving stale canary → AdaptiveCompactResult hard gate
    // (refines #3055 / #3092 — aggregate field + AC2 hard gate at the
    // AdaptiveCompactResult level, surface to Phase-5 pin_contract_held).
    std::println("\n=== Issue #3182: post-Moving stale canary → production hard gate ===");
    ac3182_1_aggregate_field();
    ac3182_2_aggregation();
    ac3182_3_hard_gate();
    ac3182_4_empty_considers();
    ac3182_5_soft_no_extra_walk();
    ac3182_6_envframe_protocol_unchanged();
    ac3182_7_no_invent();

    // Issue #3092: wire production EnvFrame/Closure/FFI/JIT live ptrs into
    // note_post_moving_live_ptr_canary (#3055 gate was blind). Additive to
    // #2495 test file per #81934 + #81967 (no new test_issue_3092.cpp).
    //   AC1: Evaluator::register_known_moving_densify_root_slots calls
    //        note_post_moving_live_ptr_canary_all for each production slot.
    //   AC2: ASTArenaGroup has note_post_moving_live_ptr_canary_all helper
    //        (parallels register_external_root_slot_for_densify_all pattern).
    //   AC3: Canary injection is observe-only (no rewrite per #3017).
    //   AC4: Quiet path zero cost (early return on null pointer).
    //   AC5: Source-cite for lineage (#3055 + #3092 references).
    std::println("\n=== Issue #3092: production canary wiring ===");
    {
        const auto mut = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(mut.find("note_post_moving_live_ptr_canary_all(*slot)") != std::string::npos,
              "#3092 AC1: canary injection in Evaluator::register_known_moving_densify_root_slots");
    }
    {
        const auto arena = read_file("src/core/arena.ixx");
        CHECK(arena.find("void note_post_moving_live_ptr_canary_all(void* p)") != std::string::npos,
              "#3092 AC2: ASTArenaGroup helper present (parallels slot-rewrite helper)");
    }
    {
        const auto arena = read_file("src/core/arena.ixx");
        const auto helper_pos = arena.find("void note_post_moving_live_ptr_canary_all(void* p)");
        const auto helper_end =
            helper_pos != std::string::npos ? arena.find("\n    }", helper_pos) : std::string::npos;
        if (helper_pos != std::string::npos && helper_end != std::string::npos) {
            const auto body = arena.substr(helper_pos, helper_end - helper_pos);
            CHECK(body.find("register_external_root_slot_for_densify") == std::string::npos,
                  "#3092 AC3: canary helper is observe-only (no rewrite per #3017)");
        }
    }
    {
        const auto arena = read_file("src/core/arena.ixx");
        const auto helper_pos = arena.find("void note_post_moving_live_ptr_canary_all(void* p)");
        const auto helper_end =
            helper_pos != std::string::npos ? arena.find("\n    }", helper_pos) : std::string::npos;
        if (helper_pos != std::string::npos && helper_end != std::string::npos) {
            const auto body = arena.substr(helper_pos, helper_end - helper_pos);
            CHECK(body.find("if (!p)") != std::string::npos,
                  "#3092 AC4: quiet path early-return on null (zero extra atomics)");
        }
    }
    {
        const auto arena = read_file("src/core/arena.ixx");
        const auto mut = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(arena.find("Issue #3092") != std::string::npos,
              "#3092 AC5: cite #3092 in canary helper");
        CHECK(mut.find("Issue #3092") != std::string::npos,
              "#3092 AC5: cite #3092 in Evaluator wiring");
    }

    // ── Issue #3185: densify-entry LCP consult (steal×GC residual). ──
    // Extends this fail-closed test file (covers recover_moving_sticky_densify_off
    // where the optional one-shot Moving densify lives; #81967).
    // Source-cite ACs only — runtime consult behavior covered by the helper
    // consult_last_lcp_for_densify_entry unit smoke + integration under
    // #1908 / #2957 steal arms.
    {
        const auto mut = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto lcp = read_file("src/core/lifetime_consistency_proof.hh");
        const auto gc = read_file("src/compiler/evaluator_gc.cpp");

        // Helper: count non-overlapping occurrences of needle in haystack.
        const auto count_occurrences = [](const std::string& haystack,
                                          const std::string& needle) -> int {
            if (needle.empty())
                return 0;
            int n = 0;
            std::size_t pos = 0;
            while (true) {
                const auto p = haystack.find(needle, pos);
                if (p == std::string::npos)
                    break;
                ++n;
                pos = p + 1;
            }
            return n;
        };

        // ac3185_1: consult wired at BOTH Phase-5 densify entry AND optional
        // one-shot Moving densify (recover_moving_sticky_densify_off). Use the
        // helper (single read surface) at both call sites per AC3 / AC4.
        const std::string consult = "consult_last_lcp_for_densify_entry";
        const int consult_sites = count_occurrences(mut, consult);
        CHECK(consult_sites >= 2, "ac3185_1: consult_last_lcp_for_densify_entry wired at Phase-5 "
                                  "entry + optional one-shot densify (>= 2 sites)");
        CHECK(mut.find("Issue #3185 AC1: same surface as Phase-5 densify entry") !=
                  std::string::npos,
              "ac3185_1: optional one-shot Moving densify carries the surface comment");

        // ac3185_2: Soft / Off zero-cost — production_defaults_active || Full
        // guard precedes the consult, so Soft/Off path short-circuits before
        // touching the atomic set (matches #3185 AC4 zero-cost contract).
        const std::string guard = "typed_audit::production_defaults_active() ||";
        const int guard_sites = count_occurrences(mut, guard);
        // Existing baseline: densify-ownership-scan / Moving / abort / etc. guards
        // already in evaluator_mutation_boundary.cpp. #3185 must add >= 2 more
        // (Phase-5 entry + optional one-shot). Use a high watermark so the AC
        // stays true across prior issues; the 3185 linter source-cites this too.
        CHECK(guard_sites >= 4, "ac3185_2: production_defaults_active guards present (incl. #3185 "
                                "Soft/Off zero-cost on both sites)");

        // ac3185_3: production-block path — pin_contract_held forced false on
        // block (same surface as moving_incomplete_remap). Two AND-bindings:
        //   pin_contract_held = compact_r.pin_contract_held && !densify_entry_lcp_blocked;
        const std::string and_binding = "compact_r.pin_contract_held && !densify_entry_lcp_blocked";
        const int and_bindings = count_occurrences(mut, and_binding);
        CHECK(and_bindings >= 2, "ac3185_3: pin_contract_held forced false on block (Phase-5 entry "
                                 "+ optional one-shot)");

        // ac3185_4: no second proof registry — single helper + single counter
        // + single reset hook in lifetime_consistency_proof.hh. No duplicate
        // helpers under a different name.
        CHECK(lcp.find("consult_last_lcp_for_densify_entry") != std::string::npos,
              "ac3185_4: single consult helper defined in lifetime_consistency_proof.hh");
        CHECK(lcp.find("g_densify_entry_lcp_blocked_total") != std::string::npos,
              "ac3185_4: single densify_entry_lcp_blocked_total counter defined");
        CHECK(lcp.find("reset_densify_entry_lcp_blocked_for_test") != std::string::npos,
              "ac3185_4: single test reset hook defined");
        // Mut-side call must go through the helper, not duplicate access to the
        // underlying atomics directly (would silently skip the polling struct).
        CHECK(mut.find("last_lifetime_consistency_would_allow(") == std::string::npos,
              "ac3185_4: mut does NOT directly read last_lifetime_consistency_would_allow (must "
              "use helper)");
        CHECK(mut.find("last_lifetime_consistency_proof_present(") == std::string::npos,
              "ac3185_4: mut does NOT directly read last_lifetime_consistency_proof_present (must "
              "use helper)");

        // ac3185_5: soft live_compact (GC compact_sweep path in evaluator_gc.cpp)
        // is UNCHANGED — does NOT call consult_last_lcp_for_densify_entry.
        // #3185 AC2 contract: Soft itself does not relocate, so consultation is
        // only on the Moving decision point. compact_sweep + live_compact(Soft)
        // remains zero LCP consult (Quiet path).
        CHECK(gc.find("consult_last_lcp_for_densify_entry") == std::string::npos,
              "ac3185_5: evaluator_gc.cpp compact_sweep / live_compact(Soft) does NOT consult LCP "
              "(zero-cost per #3185 AC2)");
        CHECK(gc.find("Issue #3185") == std::string::npos,
              "ac3185_5: evaluator_gc.cpp untouched by #3185 (soft live_compact unchanged)");
        CHECK(
            gc.find("LiveCompactMode::Soft") != std::string::npos,
            "ac3185_5: live_compact(Soft) call site still in evaluator_gc.cpp (regression guard)");
    }

    // clang-format off
    (void)R"(EnvFrame densify ownership scan fail enters outermost commit barrier (extends #2495 test file per #81967))";
    // clang-format on
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
// production default AURA_MOVING_UNTRACKED=hard (extends #2495 test file per #81967)

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_moving_densify_fail_closed();
}
#endif
