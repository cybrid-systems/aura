// @category: unit
// @reason: Issue #2366 — complete per-entry stale-stamp detection + live
// closure MustDeopt walk after epoch bump (#2304 follow-up).
//
//   AC1: Soft off → zero walks (single mode load)
//   AC2: Soft on + inject live generation-behind AOT slot → violation metric
//   AC3: Soft on + clean bump → walk runs, violations 0
//   AC4: Hard mode wired (abort path source-cite; not executed in suite)
//   AC5: source-cite walk body + tests/gate + schema-2366

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.typed_mutation_audit_pass; // Issue #2640: ac2640_* use typed_audit::apply_*_audit_defaults (module exports the typed_audit namespace)

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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
    // Production surface for #2366 is query:aot-stats (p91; hosts
    // schema-2271/#2299 + epoch-invariant keys).
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"query:aot-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: off path free ──
static void ac1_soft_off() {
    std::println("\n--- AC1: mode off → zero walk cost ---");
    aura_set_epoch_invariant_mode(0);
    CHECK(aura_epoch_invariant_mode() == 0, "AC1: mode 0");
    const auto w0 = aura_epoch_invariant_walks_total_v_read();
    const auto v0 = aura_epoch_invariant_violation_total_v_read();
    CompilerService cs;
    // Bump with mode off — should not advance process walk counters via note_walk
    // (service may still have local flag off).
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    CHECK(aura_epoch_invariant_walks_total_v_read() == w0, "AC1: walks flat when off");
    CHECK(aura_epoch_invariant_violation_total_v_read() == v0, "AC1: violations flat when off");
}

// ── AC2: soft + inject stale AOT slot ──
static void ac2_soft_detect_stale_aot() {
    std::println("\n--- AC2: soft mode detects live generation-behind AOT slot ---");
    aura_set_epoch_invariant_mode(1); // soft
    CHECK(aura_epoch_invariant_mode() == 1, "AC2: mode soft");
    CompilerService cs;
    cs.set_epoch_invariant_mode(1);
    // Inject after a bump so table epoch advanced and slot lags.
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    const auto w0 = aura_epoch_invariant_walks_total_v_read();
    const auto v0 = aura_epoch_invariant_violation_total_v_read();
    aura_aot_inject_live_stale_slot_for_test(7);
    CHECK(aura_aot_count_live_generation_behind_slots() >= 1, "AC2: inject counts as behind");
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    const auto w1 = aura_epoch_invariant_walks_total_v_read();
    const auto v1 = aura_epoch_invariant_violation_total_v_read();
    CHECK(w1 > w0, "AC2: walk ran under soft");
    CHECK(v1 > v0, "AC2: violation metric advanced on stale slot");
    aura_aot_clear_slot_for_test(7);
    aura_set_epoch_invariant_mode(0);
    cs.set_epoch_invariant_mode(0);
}

// ── AC3: soft clean path ──
static void ac3_soft_clean() {
    std::println("\n--- AC3: soft mode clean bump → walk, 0 new AOT violations ---");
    aura_aot_clear_slot_for_test(7);
    aura_set_epoch_invariant_mode(1);
    CompilerService cs;
    cs.set_epoch_invariant_mode(1);
    // Clear any leftover inject from process state
    for (int i = 0; i < 16; ++i)
        aura_aot_clear_slot_for_test(i);
    const auto w0 = aura_epoch_invariant_walks_total_v_read();
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    const auto w1 = aura_epoch_invariant_walks_total_v_read();
    CHECK(w1 > w0, "AC3: walk advanced");
    // No inject → AOT behind count 0; IR/closure may still be empty.
    CHECK(aura_aot_count_live_generation_behind_slots() == 0, "AC3: no live behind slots");
    aura_set_epoch_invariant_mode(0);
    cs.set_epoch_invariant_mode(0);
}

// ── AC4 hard path source + mode ──
static void ac4_hard_mode() {
    std::println("\n--- AC4: hard mode wiring (no abort in suite) ---");
    aura_set_epoch_invariant_hard_enabled(1);
    CHECK(aura_epoch_invariant_mode() == 2, "AC4: hard_enabled → mode 2");
    aura_set_epoch_invariant_hard_enabled(0);
    CHECK(aura_epoch_invariant_mode() == 0, "AC4: hard_enabled(0) → mode 0");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("mode >= 2") != std::string::npos ||
              svc.find("std::abort()") != std::string::npos,
          "AC4: hard abort path in service.ixx");
    CHECK(svc.find("[#2366]") != std::string::npos, "AC4: hard fail message cites #2366");
}

// ── AC5 source-cite + query ──
static void ac5_source_and_query() {
    std::println("\n--- AC5: source-cite + query schema-2366 ---");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto br = read_file("src/compiler/aura_jit_bridge.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");

    CHECK(svc.find("Issue #2366") != std::string::npos ||
              svc.find("#2304 / #2366") != std::string::npos,
          "AC5: service cites #2366");
    CHECK(svc.find("aura_aot_count_live_generation_behind_slots") != std::string::npos,
          "AC5: AOT per-entry walk");
    CHECK(svc.find("must_deopt_before_next_call") != std::string::npos, "AC5: MustDeopt walk");
    CHECK(svc.find("version_stamp_.bridge_epoch") != std::string::npos, "AC5: IR stamp check");
    CHECK(br.find("aura_aot_inject_live_stale_slot_for_test") != std::string::npos,
          "AC5: inject helper");
    CHECK(br.find("aura_set_epoch_invariant_mode") != std::string::npos, "AC5: mode setter");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2366") == 2366, "AC5: schema-2366");
    CHECK(href(cs, "issue-2366") == 2366, "AC5: issue-2366");
    CHECK(href(cs, "epoch-invariant-wired") == 1, "AC5: epoch-invariant-wired");
    CHECK(href(cs, "epoch-invariant-mode") >= 0, "AC5: mode queryable");
    CHECK(href(cs, "schema-2304") == 2304, "AC5: schema-2304 retained");
    CHECK(q.find("schema-2366") != std::string::npos, "AC5: query source-cite schema-2366");
}

// ── Issue #2640 AC1: production + Soft + inject → walk runs, behind count drops to 0 ──
static void ac2640_periodic_walk_clears_stale() {
    std::println("\n--- #2640 AC1: periodic walk clears injected stale slot ---");
    // Enable production defaults + Soft mode + short period for test.
    aura::compiler::typed_audit::apply_production_audit_defaults();
    aura_set_epoch_invariant_mode(1);
    aura_set_epoch_invariant_periodic_period_ms(50);
    // Clear any leftover inject from previous tests.
    for (int i = 0; i < 16; ++i)
        aura_aot_clear_slot_for_test(i);
    const auto w0 = aura_epoch_invariant_periodic_walks_total_v_read();
    // Inject stale slot.
    aura_aot_inject_live_stale_slot_for_test(11);
    CHECK(aura_aot_count_live_generation_behind_slots() >= 1, "AC1: stale slot injected");
    // Trigger the periodic walk hook directly.
    aura_periodic_epoch_invariant_walk_if_due();
    CHECK(aura_epoch_invariant_periodic_walks_total_v_read() > w0,
          "AC1: periodic walks_total advanced");
    CHECK(aura_aot_count_live_generation_behind_slots() == 0,
          "AC1: behind count cleared by physical invalidate");
    // Cleanup
    aura_aot_clear_slot_for_test(11);
    aura_set_epoch_invariant_mode(0);
    aura_set_epoch_invariant_periodic_period_ms(0);
    aura::compiler::typed_audit::apply_dev_audit_defaults();
}

// ── Issue #2640 AC2: Soft/Off/mode=0 → no periodic walk ──
static void ac2640_off_mode_skips_walk() {
    std::println("\n--- #2640 AC2: mode off / wrong mode / off-skip counters ---");
    aura::compiler::typed_audit::apply_production_audit_defaults();
    aura_set_epoch_invariant_periodic_period_ms(50);
    // Force last_walk_at_ms to 0 so first call would otherwise walk.
    aura_set_epoch_invariant_mode(0); // Off
    const auto w_before = aura_epoch_invariant_periodic_walks_total_v_read();
    const auto skip_off_before = aura_epoch_invariant_periodic_skipped_off_total_v_read();
    aura_periodic_epoch_invariant_walk_if_due();
    CHECK(aura_epoch_invariant_periodic_walks_total_v_read() == w_before,
          "AC2: Off mode → no walk");
    // Hard mode
    aura_set_epoch_invariant_mode(2);
    const auto skip_wrong_before = aura_epoch_invariant_periodic_skipped_wrong_mode_total_v_read();
    aura_periodic_epoch_invariant_walk_if_due();
    CHECK(aura_epoch_invariant_periodic_skipped_wrong_mode_total_v_read() > skip_wrong_before,
          "AC2: Hard mode → skipped_wrong_mode_total bumps");
    CHECK(aura_epoch_invariant_periodic_walks_total_v_read() == w_before,
          "AC2: Hard mode → no walk");
    // Disabled (period_ms=0)
    aura_set_epoch_invariant_mode(1);
    aura_set_epoch_invariant_periodic_period_ms(0);
    const auto skip_disabled_before = aura_epoch_invariant_periodic_skipped_disabled_total_v_read();
    aura_periodic_epoch_invariant_walk_if_due();
    CHECK(aura_epoch_invariant_periodic_skipped_disabled_total_v_read() > skip_disabled_before,
          "AC2: period=0 → skipped_disabled_total bumps");
    aura_set_epoch_invariant_periodic_period_ms(50);
    // sandbox=off (sandbox=dev) → production_defaults_active=false → skip_off
    aura_set_epoch_invariant_mode(1);
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    const auto skip_off2_before = aura_epoch_invariant_periodic_skipped_off_total_v_read();
    aura_periodic_epoch_invariant_walk_if_due();
    CHECK(aura_epoch_invariant_periodic_skipped_off_total_v_read() > skip_off2_before,
          "AC2: sandbox=off → skipped_off_total bumps");
    // Cleanup
    aura_set_epoch_invariant_mode(0);
    aura_set_epoch_invariant_periodic_period_ms(0);
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    (void)skip_off_before;
}

// ── Issue #2640 AC3: existing #2541 semantics preserved ──
static void ac2640_2541_semantics_preserved() {
    std::println("\n--- #2640 AC3: existing #2541 walk semantics preserved ---");
    // The existing aura_epoch_invariant_must_deopt_stale_live_closures path
    // (called via service.ixx run_epoch_invariant_if_enabled under Soft) is
    // still in effect: bump epochs → walks_total advances + closure_must_deopt
    // advances on injected stale closures.
    aura::compiler::typed_audit::apply_production_audit_defaults();
    aura_set_epoch_invariant_mode(1);
    CompilerService cs;
    cs.set_epoch_invariant_mode(1);
    // Service-driven walk still works (Issue #2366 path).
    const auto w_before = aura_epoch_invariant_walks_total_v_read();
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    CHECK(aura_epoch_invariant_walks_total_v_read() > w_before,
          "AC3: existing #2366/2541 walks_total still advances under service bump");
    aura_set_epoch_invariant_mode(0);
    cs.set_epoch_invariant_mode(0);
    aura::compiler::typed_audit::apply_dev_audit_defaults();
}

// ── Issue #2640 AC4: cost bounded; no walk on every mutation ──
static void ac2640_rate_limit_amortizes() {
    std::println("\n--- #2640 AC4: rate limit amortizes many calls ---");
    aura::compiler::typed_audit::apply_production_audit_defaults();
    aura_set_epoch_invariant_mode(1);
    // 1s period — within a sub-second test window, only first call walks.
    aura_set_epoch_invariant_periodic_period_ms(1000);
    const auto w_before = aura_epoch_invariant_periodic_walks_total_v_read();
    const auto skip_before = aura_epoch_invariant_periodic_skipped_rate_limited_total_v_read();
    // First call: walks.
    aura_periodic_epoch_invariant_walk_if_due();
    const auto w_after_first = aura_epoch_invariant_periodic_walks_total_v_read();
    CHECK(w_after_first > w_before, "AC4: first call walks");
    // 99 subsequent calls within window: rate-limited, no walk.
    for (int i = 0; i < 99; ++i)
        aura_periodic_epoch_invariant_walk_if_due();
    CHECK(aura_epoch_invariant_periodic_skipped_rate_limited_total_v_read() >= 99,
          "AC4: 99 subsequent calls → 99 rate-limited skips");
    CHECK(aura_epoch_invariant_periodic_walks_total_v_read() == w_after_first,
          "AC4: no additional walks within period");
    // Cleanup
    aura_set_epoch_invariant_mode(0);
    aura_set_epoch_invariant_periodic_period_ms(0);
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    (void)skip_before;
}

// ── Issue #2640 AC5: counters + query surface (schema additive) ──
static void ac2640_counters_and_query() {
    std::println("\n--- #2640 AC5: counters + query surface (schema additive) ---");
    aura::compiler::typed_audit::apply_production_audit_defaults();
    aura_set_epoch_invariant_mode(1);
    aura_set_epoch_invariant_periodic_period_ms(50);
    aura_periodic_epoch_invariant_walk_if_due();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura_set_epoch_invariant_mode(0);
    aura_set_epoch_invariant_periodic_period_ms(0);
    // Bump some counters to query.
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "epoch-invariant-periodic-wired") == 1, "AC5: wired flag present");
    CHECK(href(cs, "schema-2640") == 2640, "AC5: schema-2640");
    CHECK(href(cs, "issue-2640") == 2640, "AC5: issue-2640");
    CHECK(href(cs, "component-epoch-invariant-periodic-walks-total") >= 0,
          "AC5: periodic-walks-total queryable");
    CHECK(href(cs, "component-epoch-invariant-periodic-period-ms") >= 0,
          "AC5: period-ms queryable");
    CHECK(href(cs, "schema-2366") == 2366, "AC5: prior schema-2366 retained");
    CHECK(href(cs, "schema-2506") == 2506, "AC5: prior schema-2506 retained");
}

// ── Issue #2640 AC6: src-aligned test + coverage gate ──
static void ac2640_source_and_linter() {
    std::println("\n--- #2640 AC6: source-cite + linter gate ---");
    const auto br = read_file("src/compiler/aura_jit_bridge.cpp");
    const auto brh = read_file("src/compiler/aura_jit_bridge.h");
    const auto brs = read_file("src/compiler/aura_jit_bridge_stub.cpp");
    const auto dtor = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_epoch_invariant_periodic_coverage.py");
    const auto build = read_file("build.py");
    const auto t = read_file("tests/compiler/test_epoch_invariant_walk.cpp");

    CHECK(br.find("Issue #2640") != std::string::npos, "AC6: bridge cites #2640");
    CHECK(br.find("aura_periodic_epoch_invariant_walk_if_due") != std::string::npos,
          "AC6: bridge hook present");
    CHECK(br.find("AURA_EPOCH_INVARIANT_PERIOD_MS") != std::string::npos,
          "AC6: env var driven period");
    CHECK(brh.find("aura_periodic_epoch_invariant_walk_if_due") != std::string::npos,
          "AC6: header decl present");
    CHECK(brs.find("aura_periodic_epoch_invariant_walk_if_due") != std::string::npos,
          "AC6: stub present");
    CHECK(dtor.find("aura_periodic_epoch_invariant_walk_if_due") != std::string::npos,
          "AC6: dtor wire-up");
    CHECK(dtor.find("Issue #2640") != std::string::npos, "AC6: dtor cites #2640");
    CHECK(q.find("schema-2640") != std::string::npos, "AC6: query schema-2640");
    CHECK(q.find("epoch-invariant-periodic-wired") != std::string::npos, "AC6: query wired flag");
    CHECK(t.find("ac2640_periodic_walk_clears_stale") != std::string::npos,
          "AC6: AC1 test present");
    CHECK(t.find("ac2640_off_mode_skips_walk") != std::string::npos, "AC6: AC2 test present");
    CHECK(t.find("ac2640_2541_semantics_preserved") != std::string::npos, "AC6: AC3 test present");
    CHECK(t.find("ac2640_rate_limit_amortizes") != std::string::npos, "AC6: AC4 test present");
    CHECK(t.find("ac2640_counters_and_query") != std::string::npos, "AC6: AC5 test present");
    CHECK(t.find("ac2640_source_and_linter") != std::string::npos, "AC6: AC6 self-test");
    CHECK(lint.find("2640") != std::string::npos, "AC6: linter covers #2640");
    CHECK(build.find("epoch_invariant_periodic_coverage") != std::string::npos,
          "AC6: linter wired into build.py");
}

} // namespace

int run_test_epoch_invariant_walk() {
    std::println("=== Issue #2366: epoch invariant per-entry + MustDeopt walk ===");
    ac1_soft_off();
    ac2_soft_detect_stale_aot();
    ac3_soft_clean();
    ac4_hard_mode();
    ac5_source_and_query();
    std::println(
        "\n=== #2640: production Restricted default periodic epoch-invariant soft walk ===");
    ac2640_periodic_walk_clears_stale();
    ac2640_off_mode_skips_walk();
    ac2640_2541_semantics_preserved();
    ac2640_rate_limit_amortizes();
    ac2640_counters_and_query();
    ac2640_source_and_linter();
    std::println("\n=== #2366 + #2640: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_epoch_invariant_walk();
}
#endif
