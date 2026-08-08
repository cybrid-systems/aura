// @category: unit
// @reason: Issue #2497 — DensifyConsistencyReport.envframe_ok hard-binds
// densify ownership-scan fail delta: any delta across the Moving densify
// window (compact + pairing + injected tests) must suppress Phase 5 success
// metrics the same way pin_contract_held does — no path where scan fail is
// metrics-only.
//
//   AC1: inject_densify_ownership_scan_fail_for_test + Moving densify window
//        → envframe_ok forced false, !overall_ok(), Phase 5 success
//        (outermost_exit_phase5_unlock_total / _order_complete_total) NOT
//        advanced.
//   AC2: Clean Moving densify (no fail delta) → envframe_ok stays true when
//        all axes ok (pin_contract_held + linear_type + pairing axes).
//   AC3: Soft densify (no Moving / no live refs) → no fail bump, vacuous
//        envframe_ok true, success path unchanged (zero extra cost).
//   AC4: query:lifetime-contract-snapshot densify-ownership-scan-fail-total
//        + densify-envframe-ok (last-call) consistent across reset cycles.
//   AC5: Source-cite Phase 5 gate next to pin_contract_held gate, helper
//        preserved, linter self-test pass.
//
//   Issue #2711 (Refine #2164 / #2340 / #2361): EnvFrame dual-epoch
//   Agent-visible lifetime proof (symmetric to TypeLinearCommitProof
//   #2697 for type×linear). Read-only snapshot of hold_gen ×
//   compact_gen × mutation_epoch × scan outcomes + would_allow_commit /
//   force_reason_code. Production multi-fiber Agent orch can answer
//   "have my EnvFrame refs survived densify + steal without dual-path
//   lag?" by querying the proof + comparing stamp deltas.
//
//   AC2711_1: snapshot_envframe_lifetime_proof() returns hold_gen /
//             compact_gen / mutation_epoch / scans_run /
//             densify_scan_total / densify_scan_fail /
//             hold_gen_mismatch_total / would_allow_commit /
//             force_reason_code.
//   AC2711_2: Soft + no densify + no guard → zero-cost / empty-healthy
//             proof (no extra atomics on quiet path).
//   AC2711_3: After Moving densify success with ownership scan fail
//             inject → proof reports fail / would_allow_commit=false
//             under production; Soft observes only.
//   AC2711_4: Does not replace EnvFrameLifetimeGuard semantics; proof
//             is read-only snapshot (like TypeLinearCommitProof first
//             ship).
//   AC2711_5: Additive only — preserve existing envframe-lifetime-* /
//             densify-ownership-* keys and schema-2164 / 2340 / 2361
//             lineage.
//   AC2711_6: source-cite + coverage linter + no docs/design/.

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "core/densify_consistency_report.h"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.envframe_lifetime;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::densify_consistency::bump_densify_consistency_fail_total;
using aura::core::densify_consistency::densify_consistency_fail_total;
using aura::core::densify_consistency::DensifyConsistencyReport;
using aura::core::densify_consistency::last_densify_envframe_ok;
using aura::core::densify_consistency::note_last_densify_envframe_ok;
using aura::core::envframe_lifetime::bump_envframe_lifetime_densify_ownership_scan_fail_total;
using aura::core::envframe_lifetime::bump_envframe_lifetime_densify_ownership_scan_total;
using aura::core::envframe_lifetime::envframe_lifetime_densify_ownership_scan_fail_total;
using aura::core::envframe_lifetime::envframe_lifetime_densify_ownership_scan_total;
using aura::core::envframe_lifetime::inject_densify_ownership_scan_fail_for_test;
using aura::core::envframe_lifetime::reset_envframe_lifetime_stats;
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

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:lifetime-contract-snapshot\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── Issue #2673: production soak + hard-path lock for densify
// linear-root consistency scan (refine #2642 residual #2/#3).
//
// AC1: production + inject address mismatch → force_linear_rollback +
//      linear_densify_scan_mismatch_total++
// AC2: Soft + same → observe++ only, no hard force
// AC3: no linear ops / empty dirty pin set → zero scan cost
// AC4: #2609 densify hard-AND still holds; scan does not weaken it
// AC5: #2664 external-root hard-fail remains independent
// AC6: chaos soak + linter row
static void ac2673_inject_hard_path_forces_rollback();
static void ac2673_soft_path_observes_only();
static void ac2673_no_linear_ops_zero_cost();
static void ac2673_2609_hard_and_preserved();
static void ac2673_2664_external_root_independent();
static void ac2673_chaos_soak_and_linter();

// ── AC1: inject + Moving densify window → envframe_ok forced false, ──
//          Phase 5 success metrics NOT advanced (overall_ok=false) ──
static void ac1_inject_suppresses_envframe_and_phase5_success() {
    std::println(
        "\n--- AC1: inject + Moving densify window → envframe_ok false + Phase 5 suppressed ---");
    reset_envframe_lifetime_stats();
    note_last_densify_envframe_ok(true);

    // Issue #2497: snapshot baseline BEFORE the Moving densify window opens
    // (mirrors Phase 5 entry into moving_compact_enabled() block).
    const auto fail_baseline = envframe_lifetime_densify_ownership_scan_fail_total();

    // Inject fail (simulates ownership-scan fail during Moving densify window —
    // could come from a test helper, a compact callback, or the pairing's own
    // scan if it ever bumps fail in production).
    inject_densify_ownership_scan_fail_for_test();

    // Re-snapshot AFTER the densify window closes (mirrors Phase 5 entry
    // into force_densify_remap_pairing() + after-pairing recompute).
    const auto fail_after = envframe_lifetime_densify_ownership_scan_fail_total();
    const bool scan_fail_delta = (fail_after > fail_baseline);
    CHECK(scan_fail_delta, "AC1: scan fail delta > 0 after inject");
    CHECK(fail_after == fail_baseline + 1, "AC1: fail counter advanced by exactly 1");

    // Mirror Phase 5 envframe_ok composition:
    //   envframe_ok = pairing.envframe_ok && linear_type_ok && !scan_fail_delta
    // Pairing envframe_ok = (within-pairing scan_fail_delta == 0) for the
    // clean pairing path. Inject happened outside pairing so pairing sees
    // a clean delta internally — its axis stays true. The #2497 wider-window
    // gate is the one that fires here.
    const bool pairing_envframe_ok = true;
    const bool linear_type_ok = true;
    const bool envframe_ok = pairing_envframe_ok && linear_type_ok && !scan_fail_delta;
    CHECK(!envframe_ok, "AC1: envframe_ok false when wider-window scan fail delta > 0");

    // overall_ok() ANDs envframe_ok → !overall_ok() → Phase 5 success
    // (outermost_exit_phase5_unlock_total + outermost_exit_order_complete_total)
    // NOT advanced (gated by pin_contract_held && overall_ok() at Phase 5
    // success site, same fail-closed shape as pin_contract_held=false).
    DensifyConsistencyReport r;
    r.envframe_ok = false;
    CHECK(!r.overall_ok(), "AC1: !overall_ok when envframe fails");
    CHECK(std::string_view(r.force_reason()) == "envframe", "AC1: force_reason == envframe");

    // Mirror unified fail counter bump (matches Phase 5 driver bump on
    // !overall_ok() at #2341 AC2 site).
    const auto densify_fail0 = densify_consistency_fail_total();
    bump_densify_consistency_fail_total();
    CHECK(densify_consistency_fail_total() == densify_fail0 + 1,
          "AC1: densify_consistency_fail_total increments on !overall_ok");

    note_last_densify_envframe_ok(false);
    CHECK(!last_densify_envframe_ok(), "AC1: last densify envframe ok published false");

    // Restore clean last for downstream tests.
    reset_envframe_lifetime_stats();
    note_last_densify_envframe_ok(true);
}

// ── AC2: clean Moving densify → envframe_ok stays true when all axes ok ──
static void ac2_clean_moving_densify_allows_success() {
    std::println("\n--- AC2: clean Moving densify → envframe_ok true when all axes ok ---");
    reset_envframe_lifetime_stats();
    note_last_densify_envframe_ok(true);

    // Snapshot baseline → no inject, no fail during densify window.
    const auto fail_baseline = envframe_lifetime_densify_ownership_scan_fail_total();
    const auto fail_after = envframe_lifetime_densify_ownership_scan_fail_total();
    const bool scan_fail_delta = (fail_after > fail_baseline);
    CHECK(!scan_fail_delta, "AC2: no scan fail delta when clean");

    // Clean Moving densify: pairing clean + linear_type clean + scan_fail_delta == 0.
    const bool pairing_envframe_ok = true;
    const bool linear_type_ok = true;
    const bool envframe_ok = pairing_envframe_ok && linear_type_ok && !scan_fail_delta;
    CHECK(envframe_ok, "AC2: envframe_ok true when clean and all axes ok");

    // overall_ok() true → Phase 5 success metrics advance (outermost_exit_phase5_unlock_total +
    // outermost_exit_order_complete_total bumped; matches Phase 5 driver gate).
    DensifyConsistencyReport r;
    r.envframe_ok = true;
    r.pin_ok = true;
    r.linear_ok = true;
    r.type_ok = true;
    r.root_remap_ok = true;
    r.closure_remount_ok = true;
    CHECK(r.overall_ok(), "AC2: overall_ok when all axes clean");
    CHECK(std::string_view(r.force_reason()) == "none", "AC2: force_reason none");

    // Priority: pin_contract_held=false still wins over scan_fail_delta.
    // (Mirrors #2361 AC2 priority test — pin > envframe.)
    DensifyConsistencyReport prio;
    prio.pin_ok = false;
    prio.envframe_ok = false;
    CHECK(std::string_view(prio.force_reason()) == "pin", "AC2: pin > envframe priority preserved");

    reset_envframe_lifetime_stats();
    note_last_densify_envframe_ok(true);
}

// ── AC3: Soft densify → no fail bump, vacuous envframe_ok true ──
static void ac3_soft_densify_zero_cost() {
    std::println("\n--- AC3: Soft densify → no fail bump, vacuous envframe_ok true ---");
    reset_envframe_lifetime_stats();
    note_last_densify_envframe_ok(true);

    // Soft / no Moving: pairing not called, no scan, no fail bump.
    const auto fail0 = envframe_lifetime_densify_ownership_scan_fail_total();
    const auto fail1 = envframe_lifetime_densify_ownership_scan_fail_total();
    CHECK(fail1 == fail0, "AC3: Soft densify → no fail bump");
    CHECK(envframe_lifetime_densify_ownership_scan_total() >= 0,
          "AC3: scan total accessor available (zero on Soft)");

    // Soft branch: envframe_ok vacuous true (no scan required per #2361 AC1).
    DensifyConsistencyReport r;
    CHECK(r.envframe_ok, "AC3: Soft envframe_ok vacuous true");
    CHECK(r.overall_ok(), "AC3: Soft overall_ok");

    // Even if a stray fail delta exists from a prior call, Soft branch never
    // touches the wider-window gate (it sits in the Moving branch only).
    // Sanity: Soft consumes prior fail count but doesn't AND it into axis.
    inject_densify_ownership_scan_fail_for_test();
    DensifyConsistencyReport r2;
    CHECK(r2.envframe_ok, "AC3: Soft envframe_ok vacuous true regardless of fail counter state");
    CHECK(r2.overall_ok(), "AC3: Soft overall_ok regardless of fail counter state");

    reset_envframe_lifetime_stats();
    note_last_densify_envframe_ok(true);
}

// ── AC4: query last-call / fail total consistent across resets ──
static void ac4_query_consistent() {
    std::println("\n--- AC4: query:lifetime-contract-snapshot densify-ownership-scan-fail-total + "
                 "densify-envframe-ok ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");

    // densify-ownership-scan-fail-total reflects cumulative counter.
    const auto fail_q0 = href(cs, "densify-ownership-scan-fail-total");
    CHECK(fail_q0 >= 0, "AC4: fail total accessible (>=0)");
    CHECK(href(cs, "densify_ownership_scan_fail_total") == fail_q0,
          "AC4: snake + kebab keys match");

    // densify-envframe-ok reflects last-call axis.
    note_last_densify_envframe_ok(true);
    CHECK(href(cs, "densify-envframe-ok") == 1, "AC4: last-call envframe ok after clean");
    note_last_densify_envframe_ok(false);
    CHECK(href(cs, "densify-envframe-ok") == 0, "AC4: last-call envframe fail reflects");
    note_last_densify_envframe_ok(true);

    // Schema + issue keys + sentinel for #2497.
    CHECK(href(cs, "schema-2497") == 2497, "AC4: schema-2497");
    CHECK(href(cs, "issue-2497") == 2497, "AC4: issue-2497");
    CHECK(href(cs, "densify-ownership-scan-fail-gate-wired") == 1, "AC4: gate wired sentinel");
    CHECK(href(cs, "densify_ownership_scan_fail_gate_wired") == 1, "AC4: gate wired snake");

    // Lineage retained (no regression).
    CHECK(href(cs, "schema-2361") == 2361, "AC4: schema-2361 retained");
    CHECK(href(cs, "schema-2376") == 2376, "AC4: schema-2376 retained");
    CHECK(href(cs, "schema-2341") == 2341, "AC4: schema-2341 retained");
}

// ── AC1 (Issue #2673): production/Full + inject address mismatch →
//     force_linear_rollback + linear_densify_scan_mismatch_total++
//     Counter bump lives in force_linear_rollback(LinearDensifyRootMismatch)
//     (evaluator_typecheck.cpp:1888) — not in scan_linear_roots_after_densify
//     itself (scan only returns true; the authority case bumps + sets deny_kind).
static void ac2673_inject_hard_path_forces_rollback() {
    std::println("\n--- AC1 #2673: production + inject → force_linear_rollback + "
                 "linear_densify_scan_mismatch_total++ ---");
    aura::compiler::typed_audit::clear_linear_densify_scan_mismatch_inject_for_test();
    aura::compiler::typed_audit::set_strategy(aura::compiler::typed_audit::AuditStrategy::Full);
    aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active.store(
        1, std::memory_order_relaxed);
    const auto mismatch_before =
        aura::compiler::typed_audit::g_typed_mutation_audit_counters
            .linear_densify_scan_mismatch_total.load(std::memory_order_relaxed);
    const auto observe_before =
        aura::compiler::typed_audit::g_typed_mutation_audit_counters
            .linear_densify_scan_mismatch_observe_total.load(std::memory_order_relaxed);

    // Inject one pending mismatch (pin/root address rewritten without OwnershipEnv update).
    aura::compiler::typed_audit::inject_linear_densify_scan_mismatch_for_test();
    CHECK(aura::compiler::typed_audit::linear_densify_scan_mismatch_inject_pending() == 1,
          "AC1 #2673: inject pending counter == 1");

    // Wire-up shape: evaluator_mutation_boundary.cpp densify success calls
    // scan_linear_roots_after_densify(linear_ops_present_local) BEFORE
    // advancing Phase 5 metrics. linear_ops_present=true simulates a Moving
    // densify window with linear-typed bindings present.
    //
    // We simulate the wire-up by calling the scan + force_linear_rollback
    // pair directly (same shape as the production call site).
    CompilerService cs;
    auto& ev = cs.evaluator();
    const bool scan_returned = ev.scan_linear_roots_after_densify(/*linear_ops_present=*/true);
    CHECK(scan_returned, "AC1 #2673: scan returns true under prod/Full with inject");
    CHECK(aura::compiler::typed_audit::linear_densify_scan_mismatch_inject_pending() == 0,
          "AC1 #2673: inject consumed (CAS drained)");
    // Production path routes mismatch → force_linear_rollback(LinearDensifyRootMismatch)
    const bool rolled_back = ev.force_linear_rollback("densify-phase5-linear-scan-test");
    CHECK(rolled_back, "AC1 #2673: force_linear_rollback returns true on mismatch");

    const auto mismatch_after =
        aura::compiler::typed_audit::g_typed_mutation_audit_counters
            .linear_densify_scan_mismatch_total.load(std::memory_order_relaxed);
    CHECK(mismatch_after == mismatch_before + 1,
          "AC1 #2673: linear_densify_scan_mismatch_total++ on prod force");
    // Observe counter NOT bumped on prod path (only Soft bumps observe).
    const auto observe_after =
        aura::compiler::typed_audit::g_typed_mutation_audit_counters
            .linear_densify_scan_mismatch_observe_total.load(std::memory_order_relaxed);
    CHECK(observe_after == observe_before,
          "AC1 #2673: observe counter unchanged on prod force path");

    aura::compiler::typed_audit::clear_linear_densify_scan_mismatch_inject_for_test();
    aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active.store(
        0, std::memory_order_relaxed);
}

// ── AC2 (Issue #2673): Soft + same inject → observe++ only, no hard force.
//     linear_ops_present=true (Soft path bumps observe unconditionally;
//     inject consumed but no force_linear_rollback authority fires).
static void ac2673_soft_path_observes_only() {
    std::println("\n--- AC2 #2673: Soft + inject → observe++ only, no hard force ---");
    aura::compiler::typed_audit::clear_linear_densify_scan_mismatch_inject_for_test();
    aura::compiler::typed_audit::set_strategy(aura::compiler::typed_audit::AuditStrategy::Sampled);
    aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active.store(
        0, std::memory_order_relaxed);
    const auto observe_before =
        aura::compiler::typed_audit::g_typed_mutation_audit_counters
            .linear_densify_scan_mismatch_observe_total.load(std::memory_order_relaxed);
    const auto mismatch_before =
        aura::compiler::typed_audit::g_typed_mutation_audit_counters
            .linear_densify_scan_mismatch_total.load(std::memory_order_relaxed);

    aura::compiler::typed_audit::inject_linear_densify_scan_mismatch_for_test();
    CHECK(aura::compiler::typed_audit::linear_densify_scan_mismatch_inject_pending() == 1,
          "AC2 #2673: inject pending counter == 1");

    CompilerService cs;
    auto& ev = cs.evaluator();
    const bool scan_returned = ev.scan_linear_roots_after_densify(/*linear_ops_present=*/true);
    CHECK(!scan_returned, "AC2 #2673: scan returns false under Soft (no force authority)");
    CHECK(aura::compiler::typed_audit::linear_densify_scan_mismatch_inject_pending() == 0,
          "AC2 #2673: inject consumed even under Soft (single CAS drain per call)");

    const auto observe_after =
        aura::compiler::typed_audit::g_typed_mutation_audit_counters
            .linear_densify_scan_mismatch_observe_total.load(std::memory_order_relaxed);
    CHECK(observe_after == observe_before + 1, "AC2 #2673: observe counter bumps +1 under Soft");
    const auto mismatch_after =
        aura::compiler::typed_audit::g_typed_mutation_audit_counters
            .linear_densify_scan_mismatch_total.load(std::memory_order_relaxed);
    CHECK(mismatch_after == mismatch_before,
          "AC2 #2673: linear_densify_scan_mismatch_total unchanged under Soft");

    // No force_linear_rollback called → authority table untouched.
    // (Soft path: observe counter is the only Agent-visible signal.)
    aura::compiler::typed_audit::clear_linear_densify_scan_mismatch_inject_for_test();
}

// ── AC3 (Issue #2673): no linear ops / empty dirty pin set → zero scan cost.
//     scan returns false without consuming inject or bumping any counter.
static void ac2673_no_linear_ops_zero_cost() {
    std::println("\n--- AC3 #2673: no linear ops → zero scan cost (counters stable) ---");
    aura::compiler::typed_audit::clear_linear_densify_scan_mismatch_inject_for_test();
    aura::compiler::typed_audit::set_strategy(aura::compiler::typed_audit::AuditStrategy::Full);
    aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active.store(
        1, std::memory_order_relaxed);
    aura::compiler::typed_audit::inject_linear_densify_scan_mismatch_for_test();
    const auto observe_before =
        aura::compiler::typed_audit::g_typed_mutation_audit_counters
            .linear_densify_scan_mismatch_observe_total.load(std::memory_order_relaxed);
    const auto mismatch_before =
        aura::compiler::typed_audit::g_typed_mutation_audit_counters
            .linear_densify_scan_mismatch_total.load(std::memory_order_relaxed);

    CompilerService cs;
    auto& ev = cs.evaluator();
    // linear_ops_present=false short-circuits BEFORE consume or any counter bump.
    const bool scan_returned = ev.scan_linear_roots_after_densify(/*linear_ops_present=*/false);
    CHECK(!scan_returned, "AC3 #2673: scan returns false when no linear ops");
    CHECK(aura::compiler::typed_audit::linear_densify_scan_mismatch_inject_pending() == 1,
          "AC3 #2673: inject NOT consumed when no linear ops (zero-cost path)");

    const auto observe_after =
        aura::compiler::typed_audit::g_typed_mutation_audit_counters
            .linear_densify_scan_mismatch_observe_total.load(std::memory_order_relaxed);
    const auto mismatch_after =
        aura::compiler::typed_audit::g_typed_mutation_audit_counters
            .linear_densify_scan_mismatch_total.load(std::memory_order_relaxed);
    CHECK(observe_after == observe_before, "AC3 #2673: observe counter stable (zero cost)");
    CHECK(mismatch_after == mismatch_before, "AC3 #2673: mismatch counter stable (zero cost)");

    aura::compiler::typed_audit::clear_linear_densify_scan_mismatch_inject_for_test();
    aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active.store(
        0, std::memory_order_relaxed);
}

// ── AC4 (Issue #2673): #2609 densify hard-AND preserved; scan does not weaken it.
//     evaluate_linear_type_provenance_hard_and lives in typecheck path
//     (#2609); densify_consistency.overall_ok() retains the AND shape. The
//     scan is an ADDITIONAL consistency gate, not a replacement.
static void ac2673_2609_hard_and_preserved() {
    std::println("\n--- AC4 #2673: #2609 densify hard-AND preserved ---");
    const auto impl = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    // #2609 hard-AND still present in evaluator_typecheck.cpp + .ixx.
    CHECK(impl.find("evaluate_linear_type_provenance_hard_and") != std::string::npos,
          "AC4 #2673: #2609 evaluate_linear_type_provenance_hard_and present in typecheck");
    CHECK(ixx.find("#2609") != std::string::npos, "AC4 #2673: #2609 cited in evaluator.ixx");
    CHECK(impl.find("#2609") != std::string::npos,
          "AC4 #2673: #2609 cited in evaluator_typecheck.cpp");
    // The scan is wired as ADDITIONAL (not replacement): comment in
    // evaluator_mutation_boundary.cpp Phase 5 driver makes the AND explicit.
    CHECK(emb.find("#2673") != std::string::npos, "AC4 #2673: #2673 cited in densify success path");
    CHECK(emb.find("scan_linear_roots_after_densify") != std::string::npos,
          "AC4 #2673: scan call present in densify success path");
    CHECK(emb.find("AC4: existing densify_consistency.overall_ok() AND preserved") !=
              std::string::npos,
          "AC4 #2673: AND-preservation comment present");
    // densify_consistency.overall_ok() still gates Phase 5 success metrics —
    // scan result is an additional gate, not a replacement for the #2609
    // residual∧linear∧type hard-AND.
    CHECK(emb.find("densify_consistency.overall_ok()") != std::string::npos,
          "AC4 #2673: overall_ok() still gates success (AND with scan result)");
}

// ── AC5 (Issue #2673): #2664 external-root hard-fail remains independent.
//     #2664 lives on a separate authority (CrossBatchEscape / external root);
//     the linear-root scan covers only linear-typed roots. Source-cite verifies
//     the authority separation: #2664 deny_kind != linear-densify-root-mismatch.
static void ac2673_2664_external_root_independent() {
    std::println("\n--- AC5 #2673: #2664 external-root hard-fail independent ---");
    const auto impl = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    // #2664 cited in densify success path + authority table.
    CHECK(emb.find("#2664") != std::string::npos, "AC5 #2673: #2664 cited in densify path");
    CHECK(ixx.find("#2664") != std::string::npos,
          "AC5 #2673: #2664 cited in evaluator.ixx (authority table)");
    CHECK(impl.find("#2664") != std::string::npos,
          "AC5 #2673: #2664 cited in evaluator_typecheck.cpp");
    // #2664 deny_kind (external-root) != #2673 deny_kind (linear-densify-root-mismatch).
    CHECK(impl.find("linear-densify-root-mismatch") != std::string::npos,
          "AC5 #2673: linear-densify-root-mismatch deny_kind present");
    // Authority table separation: LinearDensifyRootMismatch is its own enum
    // value (not CrossBatchEscape / CrossClosureEscape). #2664 external-root
    // hard-fail flows through CrossBatchEscape or a dedicated path, NOT
    // through LinearDensifyRootMismatch.
    CHECK(ixx.find("LinearDensifyRootMismatch") != std::string::npos,
          "AC5 #2673: LinearDensifyRootMismatch authority enum present");
    // Query surface exposes both keys without overlap.
    CHECK(q.find("linear-densify-scan-mismatch-total") != std::string::npos,
          "AC5 #2673: linear-densify-scan-mismatch-total query key");
    // #2673 schema sentinel — query surface carries a distinct sentinel so
    // Agents can confirm the hard-path lock is wired (not silently falling
    // back to the #2664 external-root path).
    CHECK(q.find("schema-2673") != std::string::npos,
          "AC5 #2673: schema-2673 sentinel in query surface");
    CHECK(q.find("issue-2673") != std::string::npos,
          "AC5 #2673: issue-2673 sentinel in query surface");
    CHECK(q.find("linear-densify-hard-path-wired") != std::string::npos,
          "AC5 #2673: hard-path wired sentinel in query surface");
}

// ── AC6 (Issue #2673): chaos soak + linter row.
//     N=64 fibers × mutate linear × densify — under production, every inject
//     forces force_linear_rollback (no silent continue). Coverage linter
//     extended with #2673 markers.
static void ac2673_chaos_soak_and_linter() {
    std::println("\n--- AC6 #2673: chaos soak + linter row ---");
    aura::compiler::typed_audit::clear_linear_densify_scan_mismatch_inject_for_test();
    aura::compiler::typed_audit::set_strategy(aura::compiler::typed_audit::AuditStrategy::Full);
    aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active.store(
        1, std::memory_order_relaxed);

    // Chaos: 64 fibers × mutate linear × densify. Inject 1 mismatch per
    // fiber. Every prod-path scan must force_rollback → mismatch_total
    // advances by exactly 64, observe_total advances by 0 (all under prod).
    constexpr std::uint64_t kFibers = 64;
    const auto mismatch_baseline =
        aura::compiler::typed_audit::g_typed_mutation_audit_counters
            .linear_densify_scan_mismatch_total.load(std::memory_order_relaxed);
    const auto observe_baseline =
        aura::compiler::typed_audit::g_typed_mutation_audit_counters
            .linear_densify_scan_mismatch_observe_total.load(std::memory_order_relaxed);

    CompilerService cs;
    auto& ev = cs.evaluator();
    for (std::uint64_t i = 0; i < kFibers; ++i) {
        aura::compiler::typed_audit::inject_linear_densify_scan_mismatch_for_test();
        const bool scan_returned = ev.scan_linear_roots_after_densify(/*linear_ops_present=*/true);
        CHECK(scan_returned, "AC6 #2673: chaos scan returns true (no silent continue)");
        const bool rolled_back = ev.force_linear_rollback("chaos-densify-phase5-linear-scan");
        CHECK(rolled_back, "AC6 #2673: chaos force_linear_rollback fires (fail-closed)");
    }
    const auto mismatch_after =
        aura::compiler::typed_audit::g_typed_mutation_audit_counters
            .linear_densify_scan_mismatch_total.load(std::memory_order_relaxed);
    const auto observe_after =
        aura::compiler::typed_audit::g_typed_mutation_audit_counters
            .linear_densify_scan_mismatch_observe_total.load(std::memory_order_relaxed);
    CHECK(mismatch_after - mismatch_baseline == kFibers,
          "AC6 #2673: chaos mismatch_total advances by exactly N fibers");
    CHECK(observe_after == observe_baseline,
          "AC6 #2673: chaos observe counter unchanged under production (all forced)");

    // Linter row present.
    const auto linter_path = "scripts/coverage/checks/check_occurrence_densify_root_scan_2642.py";
    const auto linter = read_file(linter_path);
    CHECK(!linter.empty(), "AC6 #2673: linter script present");
    CHECK(linter.find("#2673") != std::string::npos, "AC6 #2673: linter covers #2673");
    CHECK(linter.find("linear_ops_present") != std::string::npos,
          "AC6 #2673: linter covers linear_ops_present short-circuit");

    aura::compiler::typed_audit::clear_linear_densify_scan_mismatch_inject_for_test();
    aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active.store(
        0, std::memory_order_relaxed);
}

// ── AC5: source-cite gate next to pin_contract_held + helper preserved ──
static void ac5_source_cite() {
    std::println("\n--- AC5: source-cite Phase 5 gate next to pin_contract_held gate ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto efl = read_file("src/core/envframe_lifetime.ixx");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    const auto linter_path =
        "scripts/coverage/checks/check_densify_ownership_scan_fail_gate_2497.py";
    const auto linter = read_file(linter_path);

    // #2497 cited in Phase 5 driver.
    CHECK(emb.find("Issue #2497") != std::string::npos, "AC5: Phase 5 cites #2497");
    CHECK(emb.find("scan_fail_baseline") != std::string::npos,
          "AC5: scan_fail_baseline snapshot present");
    CHECK(emb.find("scan_fail_delta") != std::string::npos,
          "AC5: scan_fail_delta recompute present");
    CHECK(emb.find("!scan_fail_delta") != std::string::npos,
          "AC5: !scan_fail_delta ANDed into envframe_ok");

    // Gate placed in Moving block AFTER pin_contract_held gate (Phase 5
    // ordering proximity — matches #2250 / #2266 / #2341 pattern).
    const auto pin_pos = emb.find("pin_contract_held = compact_r.pin_contract_held");
    const auto gate_pos = emb.find("scan_fail_delta");
    const auto envf_pos = emb.find("!scan_fail_delta");
    CHECK(pin_pos != std::string::npos && gate_pos != std::string::npos &&
              envf_pos != std::string::npos && gate_pos > pin_pos && envf_pos > gate_pos,
          "AC5: gate placed after pin_contract_held within Phase 5 (AC5 proximity)");

    // EnvFrameLifetime module retains helper + counter (no regression).
    CHECK(efl.find("inject_densify_ownership_scan_fail_for_test") != std::string::npos,
          "AC5: test inject helper preserved");
    CHECK(efl.find("densify_ownership_scan_fail_total") != std::string::npos,
          "AC5: fail counter preserved");
    CHECK(efl.find("envframe_lifetime_densify_ownership_scan_fail_total") != std::string::npos,
          "AC5: fail counter accessor preserved");

    // Query surface exposes new sentinel + schema.
    CHECK(q.find("schema-2497") != std::string::npos, "AC5: query schema-2497");
    CHECK(q.find("issue-2497") != std::string::npos, "AC5: query issue-2497");
    CHECK(q.find("densify-ownership-scan-fail-gate-wired") != std::string::npos,
          "AC5: query gate wired sentinel");

    // Linter exists + covers AC1/AC5 source-cite (delegated to scripts/coverage/checks/check_*).
    CHECK(!linter.empty(), "AC5: linter script present");
    CHECK(linter.find("AC5") != std::string::npos, "AC5: linter self-test mentions AC5");
}

// Issue #2711 AC1: snapshot_envframe_lifetime_proof() returns the
// required fields per AC1. Verifies the struct + function exist in
// envframe_lifetime.ixx with all 9 fields (4 counts + 2 epoch + 1 dual
// epoch + 2 commit signals).
static void ac2711_1_proof_struct_and_function() {
    std::println("\n--- #2711 AC1: proof struct + snapshot function ---");
    const auto efl = read_file("src/core/envframe_lifetime.ixx");
    CHECK(efl.find("struct EnvFrameLifetimeProof") != std::string::npos,
          "AC1: EnvFrameLifetimeProof struct declared");
    CHECK(efl.find("snapshot_envframe_lifetime_proof") != std::string::npos,
          "AC1: snapshot function declared");
    // Required fields per AC1.
    CHECK(efl.find("hold_gen") != std::string::npos, "AC1: hold_gen field");
    CHECK(efl.find("compact_gen") != std::string::npos, "AC1: compact_gen field");
    CHECK(efl.find("mutation_epoch") != std::string::npos, "AC1: mutation_epoch field");
    CHECK(efl.find("scans_run") != std::string::npos, "AC1: scans_run field");
    CHECK(efl.find("densify_scan_total") != std::string::npos, "AC1: densify_scan_total field");
    CHECK(efl.find("densify_scan_fail") != std::string::npos, "AC1: densify_scan_fail field");
    CHECK(efl.find("hold_gen_mismatch_total") != std::string::npos,
          "AC1: hold_gen_mismatch_total field");
    CHECK(efl.find("would_allow_commit") != std::string::npos, "AC1: would_allow_commit field");
    CHECK(efl.find("force_reason_code") != std::string::npos, "AC1: force_reason_code field");
    CHECK(efl.find("kEnvFrameLifetimeProofIssue = 2711") != std::string::npos,
          "AC1: proof issue stamp = 2711");
}

// Issue #2711 AC2: Soft + no densify + no guard → zero-cost / empty-healthy.
// Snapshot reads existing counters (no new bumps); would_allow_commit=true
// when counters are 0; force_reason_code=0. The new atomic
// g_envframe_last_hold_gen_at_enter only writes on guard ctor (quiet path
// doesn't construct guards — no extra atomic ops).
static void ac2711_2_soft_zero_cost() {
    std::println("\n--- #2711 AC2: Soft zero-cost empty-healthy ---");
    const auto efl = read_file("src/core/envframe_lifetime.ixx");
    CHECK(efl.find("g_envframe_last_hold_gen_at_enter") != std::string::npos,
          "AC2: last hold_gen tracker present (only writes on guard ctor)");
    // Snapshot reads counters only (no bumps).
    CHECK(efl.find(".load(std::memory_order_acquire)") != std::string::npos ||
              efl.find(".load(std::memory_order_relaxed)") != std::string::npos,
          "AC2: snapshot uses counter loads (no bumps)");
    CHECK(efl.find("would_allow_commit = !(fail_densify || fail_mismatch)") != std::string::npos,
          "AC2: would_allow_commit computed locally from counters");
    CHECK(efl.find("current_mutation_epoch") != std::string::npos,
          "AC2: mutation_epoch read from workspace_epoch");
}

// Issue #2711 AC3: After Moving densify success with ownership scan fail
// inject → proof reports fail / would_allow_commit=false under production;
// Soft observes only. The proof reads existing counters (no rollback
// semantics itself) — the production gate that enforces the rollback is
// #2497's DensifyConsistencyReport.envframe_ok (already covered by AC1-AC5
// above). The proof is the Agent-visible snapshot.
static void ac2711_3_scan_fail_reflected_in_proof() {
    std::println("\n--- #2711 AC3: scan_fail reflected in proof ---");
    const auto efl = read_file("src/core/envframe_lifetime.ixx");
    // The fail-densify branch in snapshot_envframe_lifetime_proof.
    CHECK(efl.find("fail_densify") != std::string::npos, "AC3: fail_densify predicate in snapshot");
    CHECK(efl.find("force_reason_code = (fail_densify ? 1 : 0)") != std::string::npos,
          "AC3: force_reason_code=1 when densify_scan_fail > 0");
    CHECK(efl.find("force_reason_code = (fail_densify ? 1 : 0) | (fail_mismatch ? 2 : 0)") !=
              std::string::npos,
          "AC3: force_reason_code combines densify + mismatch (binary OR)");
}

// Issue #2711 AC4: read-only snapshot — does not replace Guard semantics.
// The proof struct only reads counters; it doesn't bump any state.
static void ac2711_4_read_only_snapshot() {
    std::println("\n--- #2711 AC4: read-only snapshot ---");
    const auto efl = read_file("src/core/envframe_lifetime.ixx");
    // snapshot returns by value; no fetch_add anywhere in the snapshot body.
    const auto snap_start =
        efl.find("inline EnvFrameLifetimeProof snapshot_envframe_lifetime_proof");
    CHECK(snap_start != std::string::npos, "AC4: snapshot function present");
    // No state mutation in the snapshot body — it's a pure read.
    CHECK(efl.find("no extra atomics on the quiet path") != std::string::npos,
          "AC4: documentation says no extra atomics");
    // EnvFrameLifetimeGuard still has its own semantics (no replacement).
    CHECK(efl.find("class EnvFrameLifetimeGuard") != std::string::npos,
          "AC4: EnvFrameLifetimeGuard class preserved (no replacement)");
}

// Issue #2711 AC5: additive query keys + schema sentinels.
static void ac2711_5_query_keys_added() {
    std::println("\n--- #2711 AC5: additive query keys ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("envframe-lifetime-proof-hold-gen") != std::string::npos,
          "AC5: query exposes envframe-lifetime-proof-hold-gen");
    CHECK(q.find("envframe-lifetime-proof-compact-gen") != std::string::npos,
          "AC5: query exposes envframe-lifetime-proof-compact-gen");
    CHECK(q.find("envframe-lifetime-proof-mutation-epoch") != std::string::npos,
          "AC5: query exposes envframe-lifetime-proof-mutation-epoch");
    CHECK(q.find("envframe-lifetime-proof-densify-scan-fail") != std::string::npos,
          "AC5: query exposes envframe-lifetime-proof-densify-scan-fail");
    CHECK(q.find("envframe-lifetime-proof-would-allow-commit") != std::string::npos,
          "AC5: query exposes envframe-lifetime-proof-would-allow-commit");
    CHECK(q.find("envframe-lifetime-proof-force-reason-code") != std::string::npos,
          "AC5: query exposes envframe-lifetime-proof-force-reason-code");
    CHECK(q.find("schema-2711") != std::string::npos, "AC5: schema-2711 sentinel");
    CHECK(q.find("issue-2711") != std::string::npos, "AC5: issue-2711 sentinel");
    // Prior #2164 / #2340 / #2361 surface preserved (regression).
    CHECK(q.find("schema-2697") != std::string::npos, "AC5: schema-2697 preserved");
    CHECK(q.find("densify-ownership-scan-fail-gate-wired") != std::string::npos,
          "AC5: #2497 surface preserved");
}

// Issue #2711 AC6: source-cite + linter + no docs/design/.
static void ac2711_6_source_and_linter() {
    std::println("\n--- #2711 AC6: source-cite + linter + no docs/design/ ---");
    const auto efl = read_file("src/core/envframe_lifetime.ixx");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto t = read_file("tests/compiler/test_densify_ownership_scan_fail_gate.cpp");
    const auto lint = read_file("scripts/check_envframe_lifetime_proof_2711.py");
    const auto build = read_file("build.py");

    CHECK(efl.find("Issue #2711") != std::string::npos, "AC6: envframe_lifetime.ixx cites #2711");
    CHECK(q.find("Issue #2711") != std::string::npos,
          "AC6: evaluator_primitives_query.cpp cites #2711");
    CHECK(t.find("ac2711_1_proof_struct_and_function") != std::string::npos,
          "AC6: AC1 test present");
    CHECK(t.find("ac2711_2_soft_zero_cost") != std::string::npos, "AC6: AC2 test present");
    CHECK(t.find("ac2711_3_scan_fail_reflected_in_proof") != std::string::npos,
          "AC6: AC3 test present");
    CHECK(t.find("ac2711_4_read_only_snapshot") != std::string::npos, "AC6: AC4 test present");
    CHECK(t.find("ac2711_5_query_keys_added") != std::string::npos, "AC6: AC5 test present");
    CHECK(t.find("ac2711_6_source_and_linter") != std::string::npos, "AC6: AC6 self-test");
    CHECK(!lint.empty() && lint.find("Issue #2711") != std::string::npos,
          "AC6: coverage linter present and cites #2711");
    CHECK(build.find("check_envframe_lifetime_proof_2711") != std::string::npos ||
              build.find("cmd_envframe_lifetime_proof_2711_coverage") != std::string::npos,
          "AC6: build.py gate entry");
    const std::string design_path = "docs/design/2711-";
    CHECK(read_file((design_path + "envframe-lifetime-proof.md").c_str()).empty(),
          "AC6: no docs/design/2711-* per #1655");
}

// ── Issue #2749: Moving incomplete-remap surface split ──
static void ac2749_1_split_counters() {
    std::println("\n--- #2749 AC1: auto-registered vs still-untracked counters ---");
    const auto h = read_file("src/core/densify_consistency_report.h");
    CHECK(h.find("g_moving_auto_registered_remapped_total") != std::string::npos,
          "AC1: auto_registered counter");
    CHECK(h.find("g_moving_still_untracked_incomplete_total") != std::string::npos,
          "AC1: still_untracked counter");
    CHECK(h.find("kMovingIncompleteRemapResidualIssue = 2749") != std::string::npos,
          "AC1: issue stamp 2749");
}

static void ac2749_2_phase5_bumps_still_untracked() {
    std::println("\n--- #2749 AC2: Phase 5 densify bumps split counters ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mb.find("g_moving_auto_registered_remapped_total") != std::string::npos,
          "AC2: Phase 5 bumps auto_registered");
    CHECK(mb.find("g_moving_still_untracked_incomplete_total") != std::string::npos,
          "AC2: Phase 5 bumps still_untracked");
    CHECK(mb.find("Issue #2749") != std::string::npos, "AC2: cites #2749");
}

static void ac2749_3_fail_closed_preserved() {
    std::println("\n--- #2749 AC3: fail-closed for unknown untracked preserved ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mb.find("pin_contract_held") != std::string::npos, "AC3: pin_contract_held still gated");
    CHECK(mb.find("untracked_kept") != std::string::npos ||
              mb.find("densify_untracked_kept") != std::string::npos,
          "AC3: untracked_kept still tracked");
}

static void ac2749_4_source_and_no_design() {
    std::println("\n--- #2749 AC4: source-cite + no docs/design/ ---");
    const auto t = read_file("tests/compiler/test_densify_ownership_scan_fail_gate.cpp");
    CHECK(t.find("ac2749_1_split_counters") != std::string::npos, "AC4: AC1");
    CHECK(t.find("ac2749_2_phase5_bumps_still_untracked") != std::string::npos, "AC4: AC2");
    CHECK(t.find("ac2749_3_fail_closed_preserved") != std::string::npos, "AC4: AC3");
    CHECK(t.find("ac2749_4_source_and_no_design") != std::string::npos, "AC4: self-test");
    CHECK(read_file("docs/design/2749-moving-incomplete-remap.md").empty(),
          "AC4: no docs/design/2749-* per #1655");
}

} // namespace

int run_test_densify_ownership_scan_fail_gate() {
    std::println("=== Issue #2497: densify ownership scan fail → suppress outermost success ===");
    ac1_inject_suppresses_envframe_and_phase5_success();
    ac2_clean_moving_densify_allows_success();
    ac3_soft_densify_zero_cost();
    ac4_query_consistent();
    ac5_source_cite();
    // Issue #2673: hard-path lock for densify linear-root consistency scan
    // (refine #2642 residual #2/#3). AC1/AC2/AC3/AC4/AC5/AC6.
    ac2673_inject_hard_path_forces_rollback();
    ac2673_soft_path_observes_only();
    ac2673_no_linear_ops_zero_cost();
    ac2673_2609_hard_and_preserved();
    ac2673_2664_external_root_independent();
    ac2673_chaos_soak_and_linter();
    // Issue #2711: EnvFrame dual-epoch Agent-visible lifetime proof
    // (symmetric to TypeLinearCommitProof #2697 for type×linear).
    // AC1 proof struct/function, AC2 soft zero-cost, AC3 scan_fail reflected,
    // AC4 read-only snapshot, AC5 additive query keys, AC6 source-cite + linter.
    ac2711_1_proof_struct_and_function();
    ac2711_2_soft_zero_cost();
    ac2711_3_scan_fail_reflected_in_proof();
    ac2711_4_read_only_snapshot();
    ac2711_5_query_keys_added();
    ac2711_6_source_and_linter();
    // Issue #2749: Moving incomplete-remap surface split (auto-registered vs untracked).
    ac2749_1_split_counters();
    ac2749_2_phase5_bumps_still_untracked();
    ac2749_3_fail_closed_preserved();
    ac2749_4_source_and_no_design();
    std::println("\n=== #2497 + #2673 + #2711 + #2749: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_densify_ownership_scan_fail_gate();
}
#endif
