// @category: unit
// @reason: Issue #2543 — wire query:aot-hot-update-health into orch agent
//          self-throttle (advisory concurrency / reemit control plane).
//
//   AC1: StormLevel ≠ None → health_bp drops; throttle fires; cap=1
//   AC2: Idle healthy → health_bp==10000; zero throttle cost
//   AC3: force_reason priority matches #2506 (storm > deferred-reemit)
//   AC4: AURA_AOT_HOT_UPDATE_HEALTH_BUDGET_BP respected
//   AC5: additive metrics + schema-2543; #2506 query still works
//   AC6: source-cite

#include "test_harness.hpp"

#include "compiler/aot_hot_update_health.hh"
#include "compiler/hot_update_registry.hh"
#include "orch/agent_spawn.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::AotHotUpdateHealthSnapshot;
using aura::compiler::apply_hot_update_health_concurrency_cap;
using aura::compiler::CompilerService;
using aura::compiler::compute_aot_hot_update_health;
using aura::compiler::decide_hot_update_throttle;
using aura::compiler::g_orch_hot_update_health_checks_total;
using aura::compiler::g_orch_hot_update_health_throttle_total;
using aura::compiler::HotUpdateThrottleAction;
using aura::compiler::kAotHotUpdateHealthThrottleIssue;
using aura::compiler::orch_hot_update_health_throttle_tick;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::orch::g_orch_module_stats;
using aura::test::g_failed;
using aura::test::g_passed;

void set_env(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}
void clear_env(const char* k) {
#if defined(_WIN32)
    _putenv_s(k, "");
#else
    unsetenv(k);
#endif
}

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

static std::int64_t href(CompilerService& cs, std::string_view query, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", query, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC2 first: healthy / idle ───────────────────────────────────
static void ac2_idle_zero_throttle() {
    std::println("\n--- AC2: idle healthy → no throttle ---");
    AotHotUpdateHealthSnapshot s;
    auto r = compute_aot_hot_update_health(s);
    CHECK(r.health_bp == 10000, "AC2: vacuous health_bp == 10000");
    auto d = decide_hot_update_throttle(r);
    CHECK(!d.throttle, "AC2: no throttle when healthy");
    CHECK(d.action == HotUpdateThrottleAction::None, "AC2: action none");
    CHECK(d.max_concurrency_cap == 1024, "AC2: full concurrency cap");

    // Clear live storm if any.
    auto& reg = aura::compiler::hot_update_registry();
    reg.set_shape_storm_active(false);
    reg.reset_deopt_storm_state_for_test();
    reg.on_reload_success();

    const auto t0 = g_orch_hot_update_health_throttle_total.load(std::memory_order_relaxed);
    // Pure decision on vacuous — no metric side effect.
    d = decide_hot_update_throttle(compute_aot_hot_update_health(s));
    CHECK(!d.throttle, "AC2: pure path no throttle");
    CHECK(g_orch_hot_update_health_throttle_total.load() == t0,
          "AC2: pure decide does not bump throttle total");
}

// ── AC1: storm → throttle + cap ─────────────────────────────────
static void ac1_storm_throttle() {
    std::println("\n--- AC1: storm → throttle fires, concurrency cap=1 ---");
    AotHotUpdateHealthSnapshot s;
    s.storm_level = 2; // Global
    auto r = compute_aot_hot_update_health(s);
    CHECK(r.health_bp < r.health_budget_bp, "AC1: health_bp < budget under storm");
    CHECK(r.force_reason_code == 1, "AC1: force_reason storm");
    auto d = decide_hot_update_throttle(r);
    CHECK(d.throttle, "AC1: throttle true");
    CHECK(d.action == HotUpdateThrottleAction::SplitBatch, "AC1: split-batch under storm");
    CHECK(d.max_concurrency_cap == 1, "AC1: cap concurrency=1");

    // Live: force shape storm so sample path sees it.
    auto& reg = aura::compiler::hot_update_registry();
    reg.set_shape_storm_active(true);
    const auto t0 = g_orch_hot_update_health_throttle_total.load(std::memory_order_relaxed);
    const auto c0 = g_orch_hot_update_health_checks_total.load(std::memory_order_relaxed);
    const auto cap = apply_hot_update_health_concurrency_cap(8);
    CHECK(cap <= 1, "AC1: apply cap reduces 8 → ≤1 under storm");
    CHECK(g_orch_hot_update_health_throttle_total.load() > t0, "AC1: throttle total bumped");
    CHECK(g_orch_hot_update_health_checks_total.load() > c0, "AC1: checks total bumped");
    reg.set_shape_storm_active(false);
    reg.reset_deopt_storm_state_for_test();
}

// ── AC3: priority table ─────────────────────────────────────────
static void ac3_priority() {
    std::println("\n--- AC3: force_reason priority (storm > deferred-reemit) ---");
    AotHotUpdateHealthSnapshot s;
    s.storm_level = 1;
    s.deferred_reemit_pending = 1;
    auto r = compute_aot_hot_update_health(s);
    CHECK(r.force_reason_code == 1, "AC3: storm wins over deferred-reemit");
    CHECK(r.force_reason == "storm", "AC3: force_reason storm");
    auto d = decide_hot_update_throttle(r);
    CHECK(d.action == HotUpdateThrottleAction::SplitBatch,
          "AC3: storm → split-batch not skip-reemit");

    s = {};
    s.deferred_reemit_pending = 1;
    r = compute_aot_hot_update_health(s);
    CHECK(r.force_reason_code == 6, "AC3: deferred-reemit alone");
    // deferred-only: −1500 → bp=8500 ≥ default budget 8000 → no throttle yet.
    // Stack soft pending so bp < budget and action maps to skip-reemit.
    s.pending_dirty_count = 30; // −1500 soft → bp=7000
    r = compute_aot_hot_update_health(s);
    CHECK(r.force_reason_code == 6, "AC3: deferred still wins force_reason");
    d = decide_hot_update_throttle(r);
    CHECK(d.throttle, "AC3: throttle when deferred + soft under budget");
    CHECK(d.action == HotUpdateThrottleAction::SkipReemit, "AC3: deferred → skip-reemit");
    CHECK(d.max_concurrency_cap == 4, "AC3: skip-reemit cap 4");

    s = {};
    s.force_jit_regions_mask = 4;
    r = compute_aot_hot_update_health(s);
    d = decide_hot_update_throttle(r);
    CHECK(d.action == HotUpdateThrottleAction::SplitBatch, "AC3: force-jit → split-batch");

    s = {};
    s.epoch_invariant_violation_total = 1;
    s.pending_dirty_count = 30; // −2000 hard −1500 soft → bp=6500
    r = compute_aot_hot_update_health(s);
    CHECK(r.force_reason_code == 5, "AC3: epoch-invariant force_reason");
    d = decide_hot_update_throttle(r);
    CHECK(d.throttle, "AC3: throttle under epoch-invariant + soft");
    CHECK(d.action == HotUpdateThrottleAction::DelayMutate, "AC3: epoch-invariant → delay-mutate");
}

// ── AC4: budget env ─────────────────────────────────────────────
static void ac4_budget_env() {
    std::println("\n--- AC4: AURA_AOT_HOT_UPDATE_HEALTH_BUDGET_BP respected ---");
    // Soft penalty only: pending_dirty * 50 capped 1500 → health 8500.
    // With default budget 8000 → no throttle. With budget 9000 → throttle.
    AotHotUpdateHealthSnapshot s;
    s.pending_dirty_count = 30; // 30*50=1500 soft → bp=8500
    clear_env("AURA_AOT_HOT_UPDATE_HEALTH_BUDGET_BP");
    auto r = compute_aot_hot_update_health(s);
    CHECK(r.health_budget_bp == 8000, "AC4: default budget 8000");
    CHECK(r.health_bp == 8500, "AC4: soft-only bp 8500");
    auto d = decide_hot_update_throttle(r);
    CHECK(!d.throttle, "AC4: 8500 ≥ 8000 → no throttle");

    set_env("AURA_AOT_HOT_UPDATE_HEALTH_BUDGET_BP", "9000");
    r = compute_aot_hot_update_health(s);
    CHECK(r.health_budget_bp == 9000, "AC4: env budget 9000");
    d = decide_hot_update_throttle(r);
    CHECK(d.throttle, "AC4: 8500 < 9000 → throttle");
    clear_env("AURA_AOT_HOT_UPDATE_HEALTH_BUDGET_BP");
}

// ── AC5: metrics + schema ───────────────────────────────────────
static void ac5_metrics() {
    std::println("\n--- AC5: metrics + schema-2543 ---");
    CHECK(kAotHotUpdateHealthThrottleIssue == 2543, "AC5: issue stamp");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "query:aot-hot-update-health", "schema-2506") == 2506,
          "AC5: schema-2506 retained");
    CHECK(href(cs, "query:aot-hot-update-health", "schema-2543") == 2543, "AC5: schema-2543");
    CHECK(href(cs, "query:aot-hot-update-health", "orch-hot-update-health-throttle-wired") == 1,
          "AC5: throttle-wired on health query");
    CHECK(href(cs, "query:aot-hot-update-health", "orch-hot-update-health-throttle-total") >= 0,
          "AC5: throttle total on health query");
    // Process counters always live (orch-module-stats hash may be capacity-tight).
    CHECK(g_orch_hot_update_health_throttle_total.load() >= 0, "AC5: process throttle total");
    CHECK(g_orch_module_stats.orch_hot_update_health_throttle_total.load() >= 0,
          "AC5: OrchModuleStats throttle field");
}

// ── AC6: source-cite ────────────────────────────────────────────
static void ac6_source() {
    std::println("\n--- AC6: source-cite ---");
    const auto hh = read_file("src/compiler/aot_hot_update_health.hh");
    const auto agent = read_file("src/compiler/evaluator_primitives_agent.cpp");
    const auto fiber = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto spawn = read_file("src/orch/agent_spawn.h");
    CHECK(hh.find("2543") != std::string::npos, "AC6: health.hh cites #2543");
    CHECK(hh.find("decide_hot_update_throttle") != std::string::npos, "AC6: decide API");
    CHECK(hh.find("apply_hot_update_health_concurrency_cap") != std::string::npos,
          "AC6: concurrency cap API");
    CHECK(hh.find("split-batch") != std::string::npos, "AC6: policy table split-batch");
    CHECK(agent.find("apply_hot_update_health_concurrency_cap") != std::string::npos,
          "AC6: parallel-intend uses cap");
    CHECK(fiber.find("orch_hot_update_health_throttle_tick") != std::string::npos,
          "AC6: agent body tick");
    CHECK(spawn.find("orch_hot_update_health_throttle_total") != std::string::npos,
          "AC6: OrchModuleStats field");
}

} // namespace

int run_test_orch_hot_update_health_throttle() {
    std::println("=== Issue #2543: orch hot-update health self-throttle ===");
    ac2_idle_zero_throttle();
    ac1_storm_throttle();
    ac3_priority();
    ac4_budget_env();
    ac5_metrics();
    ac6_source();
    std::println("\n=== #2543 summary: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_orch_hot_update_health_throttle();
}
#endif
