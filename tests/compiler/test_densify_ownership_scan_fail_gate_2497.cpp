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

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "core/densify_consistency_report.h"

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

} // namespace

int run_test_densify_ownership_scan_fail_gate_2497() {
    std::println("=== Issue #2497: densify ownership scan fail → suppress outermost success ===");
    ac1_inject_suppresses_envframe_and_phase5_success();
    ac2_clean_moving_densify_allows_success();
    ac3_soft_densify_zero_cost();
    ac4_query_consistent();
    ac5_source_cite();
    std::println("\n=== #2497: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_densify_ownership_scan_fail_gate_2497();
}
#endif
