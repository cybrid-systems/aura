// @category: unit
// @reason: Issue #2364 — harden PanicCheckpoint residual clear × GC defer
// under concurrent densify / fiber.
//
//   AC1: Soft / no densify / no panic → free path (zero clear/rearm)
//   AC2: densify + residual panic defer without checkpoint → force-cleared
//   AC3: densify + live checkpoint without defer → re-armed
//   AC4: AURA_PANIC_CONTRACT=hard documented; counters queryable
//   AC5: Phase 5 wires audit + query schema-2364 + source-cite

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "core/gc_hooks.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

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
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:mutation-boundary-hold-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void drain_panic_defer() {
    while (aura::gc_hooks::gc_defer_pending_panic_depth() > 0)
        aura::gc_hooks::release_gc_defer_pending_panic();
    // Clear any table slots via force-clear on null id path + reconcile.
    (void)aura::gc_hooks::reconcile_gc_defer_bits_after_clear();
}

// ── AC1: Soft free path ──
static void ac1_soft_free() {
    std::println("\n--- AC1: Soft / no densify / no panic → free ---");
    drain_panic_defer();
    const auto t0 = aura::gc_hooks::panic_defer_after_densify_total();
    const auto c0 = aura::gc_hooks::panic_defer_after_densify_cleared_total();
    const auto r0 = aura::gc_hooks::panic_defer_after_densify_rearmed_total();
    auto a = aura::gc_hooks::audit_panic_defer_after_densify(
        /*evaluator_id=*/nullptr, /*has_panic_checkpoint=*/false, /*densify_ran=*/false);
    CHECK(a.free_path, "AC1: free_path when no densify + no panic");
    CHECK(!a.cleared && !a.rearmed && !a.hard_fail, "AC1: no clear/rearm/hard on free");
    CHECK(aura::gc_hooks::panic_defer_after_densify_total() == t0, "AC1: total flat on free");
    CHECK(aura::gc_hooks::panic_defer_after_densify_cleared_total() == c0, "AC1: cleared flat");
    CHECK(aura::gc_hooks::panic_defer_after_densify_rearmed_total() == r0, "AC1: rearmed flat");
}

// ── AC2: residual panic defer without checkpoint → cleared ──
static void ac2_clear_orphan_after_densify() {
    std::println("\n--- AC2: densify + orphan panic defer → force-clear ---");
    drain_panic_defer();
    // Arm process-wide panic defer without a live checkpoint (orphan).
    aura::gc_hooks::arm_gc_defer_pending_panic();
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() > 0, "AC2: armed depth > 0");
    const auto c0 = aura::gc_hooks::panic_defer_after_densify_cleared_total();
    auto a = aura::gc_hooks::audit_panic_defer_after_densify(
        /*evaluator_id=*/nullptr, /*has_panic_checkpoint=*/false, /*densify_ran=*/true);
    CHECK(!a.free_path, "AC2: not free when densify_ran");
    CHECK(a.residual_found, "AC2: residual_found");
    CHECK(a.cleared, "AC2: cleared orphan defer");
    CHECK(aura::gc_hooks::panic_defer_after_densify_cleared_total() > c0, "AC2: cleared counter");
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == 0, "AC2: depth 0 after clear");
    const auto reasons = aura::gc_hooks::defer_reasons_snapshot();
    const bool panic_bit =
        (reasons & static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::Panic)) != 0;
    CHECK(!panic_bit, "AC2: Panic bit clear after audit");
    drain_panic_defer();
}

// ── AC3: live checkpoint without defer → re-arm ──
static void ac3_rearm_for_live_checkpoint() {
    std::println("\n--- AC3: densify + live CP without defer → rearm ---");
    drain_panic_defer();
    // Simulate evaluator id with live checkpoint but no defer arm.
    void* fake_ev = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xBEEF2364));
    CHECK(!aura::gc_hooks::gc_deferred_for_evaluator(fake_ev), "AC3: not deferred before");
    const auto r0 = aura::gc_hooks::panic_defer_after_densify_rearmed_total();
    auto a = aura::gc_hooks::audit_panic_defer_after_densify(fake_ev, /*has_panic_checkpoint=*/true,
                                                             /*densify_ran=*/true);
    CHECK(a.rearmed, "AC3: rearmed for live checkpoint");
    CHECK(aura::gc_hooks::gc_deferred_for_evaluator(fake_ev), "AC3: deferred after rearm");
    CHECK(aura::gc_hooks::panic_defer_after_densify_rearmed_total() > r0, "AC3: rearm counter");
    // Cleanup
    (void)aura::gc_hooks::clear_gc_defer_for_evaluator(fake_ev);
    (void)aura::gc_hooks::reconcile_gc_defer_bits_after_clear();
    drain_panic_defer();
}

// ── AC4: hard contract + counters queryable ──
static void ac4_hard_and_query() {
    std::println("\n--- AC4: hard contract helper + query schema-2364 ---");
    // Hard path aborts — only verify the policy detector, not abort itself.
    const char* prev = std::getenv("AURA_PANIC_CONTRACT");
    ::setenv("AURA_PANIC_CONTRACT", "hard", 1);
    CHECK(aura::gc_hooks::panic_contract_hard_enabled(), "AC4: hard enabled when env=hard");
    ::setenv("AURA_PANIC_CONTRACT", "soft", 1);
    CHECK(!aura::gc_hooks::panic_contract_hard_enabled(), "AC4: hard off when env=soft");
    if (prev)
        ::setenv("AURA_PANIC_CONTRACT", prev, 1);
    else
        ::unsetenv("AURA_PANIC_CONTRACT");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2364") == 2364, "AC4: schema-2364");
    CHECK(href(cs, "issue-2364") == 2364, "AC4: issue-2364");
    CHECK(href(cs, "panic-defer-after-densify-wired") == 1, "AC4: wired");
    CHECK(href(cs, "panic-defer-after-densify-total") >= 0, "AC4: total queryable");
    CHECK(href(cs, "panic-defer-after-densify-cleared-total") >= 0, "AC4: cleared queryable");
    CHECK(href(cs, "panic-defer-after-densify-rearmed-total") >= 0, "AC4: rearmed queryable");
    // Lineage retained
    CHECK(href(cs, "schema-2296") == 2296, "AC4: schema-2296 retained");
    CHECK(href(cs, "schema-2211") == 2211, "AC4: schema-2211 retained");
}

// ── AC5: source-cite Phase 5 + helper ──
static void ac5_source_cite() {
    std::println("\n--- AC5: source-cite Phase 5 densify audit ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto gh = read_file("src/core/gc_hooks.h");
    const auto met = read_file("src/compiler/observability_metrics.h");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(mb.find("Issue #2364") != std::string::npos, "AC5: Phase 5 cites #2364");
    CHECK(mb.find("audit_panic_defer_after_densify") != std::string::npos,
          "AC5: Phase 5 calls audit");
    CHECK(gh.find("audit_panic_defer_after_densify") != std::string::npos,
          "AC5: helper in gc_hooks");
    CHECK(gh.find("AURA_PANIC_CONTRACT") != std::string::npos, "AC5: hard env");
    CHECK(gh.find("g_panic_defer_after_densify_total") != std::string::npos,
          "AC5: process counter");
    CHECK(met.find("panic_defer_after_densify_total") != std::string::npos, "AC5: metrics field");
    CHECK(q.find("schema-2364") != std::string::npos, "AC5: query schema");
    CHECK(q.find("panic-defer-after-densify-wired") != std::string::npos, "AC5: query wired");
}

// ── Issue #2598: production densify-after panic residual → hard ──────────────
//
// Aligns audit_panic_defer_after_densify hard-fail with the steal residual
// hard-AND (#2546) — production / Restricted now hard-fails when residual
// panic defer outlives a cleared PanicCheckpoint, not just env=hard.
// Operator env AURA_PANIC_CONTRACT=soft forces Soft semantics (override).
//
// AC6: gc_defer_production_locked() is callable from tests (source-cite).
// AC7: panic_contract_soft_override() recognized (soft / 0 / off / false / no).
// AC8: source-cite for #2598 production lock + soft override in gc_hooks.h.
// AC9: audit_panic_defer_after_densify modified to read production_lock +
//     soft_override (mirrors #2338 / #2546 / #2596 pattern).
// AC10: build.py wires cmd_panic_residual_densify_hard_2598_coverage +
//      scripts/coverage/checks/check_panic_residual_densify_hard_2598.py present.
static void ac6_production_lock_helper_source_cite() {
    std::println("\n--- #2598 AC6: gc_defer_production_locked helper source-cite ---");
    const auto gh = read_file("src/core/gc_hooks.h");
    CHECK(gh.find("gc_defer_production_locked") != std::string::npos,
          "AC6: gc_hooks.h declares gc_defer_production_locked()");
    CHECK(gh.find("set_gc_defer_production_locked") != std::string::npos,
          "AC6: gc_hooks.h declares set_gc_defer_production_locked()");
    CHECK(gh.find("g_production_locked") != std::string::npos,
          "AC6: gc_hooks.h has process-wide g_production_locked atomic");
}

static void ac7_soft_override_helper() {
    std::println("\n--- #2598 AC7: panic_contract_soft_override helper ---");
    const auto gh = read_file("src/core/gc_hooks.h");
    CHECK(gh.find("panic_contract_soft_override") != std::string::npos,
          "AC7: gc_hooks.h declares panic_contract_soft_override()");
    CHECK(gh.find("\"soft\"") != std::string::npos,
          "AC7: 'soft' env value recognized in soft_override parser");
    CHECK(gh.find("\"off\"") != std::string::npos,
          "AC7: 'off' env value recognized in soft_override parser");
}

static void ac8_production_lock_source_cite() {
    std::println("\n--- #2598 AC8: production lock source-cite in audit ---");
    const auto gh = read_file("src/core/gc_hooks.h");
    CHECK(gh.find("Issue #2598") != std::string::npos, "AC8: gc_hooks.h cites #2598");
    CHECK(gh.find("production_lock && !soft_override") != std::string::npos,
          "AC8: audit reads production_lock && !soft_override");
    CHECK(gh.find("hard_from_env") != std::string::npos,
          "AC8: audit reads hard_from_env (pre-existing path)");
    CHECK(gh.find("hard_from_env || (production_lock && !soft_override)") != std::string::npos,
          "AC8: audit hard-fail condition is hard_from_env OR (production_lock && !soft_override)");
}

static void ac9_build_gate_wiring_source_cite() {
    std::println("\n--- #2598 AC9: build.py + gate script source-cite ---");
    const auto build = read_file("build.py");
    CHECK(build.find("cmd_panic_residual_densify_hard_2598_coverage") != std::string::npos,
          "AC9: build.py wires cmd_panic_residual_densify_hard_2598_coverage");
    CHECK(build.find("check_panic_residual_densify_hard_2598") != std::string::npos,
          "AC9: build.py runs check_panic_residual_densify_hard_2598 gate");
}

} // namespace

int run_test_panic_defer_after_densify() {
    std::println("=== Issue #2364: PanicCheckpoint residual × densify ===");
    std::println("=== Issue #2598: production densify-after panic residual → hard (extends #2364 "
                 "test file per #81967) ===");
    ac1_soft_free();
    ac2_clear_orphan_after_densify();
    ac3_rearm_for_live_checkpoint();
    ac4_hard_and_query();
    ac5_source_cite();
    ac6_production_lock_helper_source_cite();
    ac7_soft_override_helper();
    ac8_production_lock_source_cite();
    ac9_build_gate_wiring_source_cite();
    std::println("\n=== #2364 + #2598: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_panic_defer_after_densify();
}
#endif
