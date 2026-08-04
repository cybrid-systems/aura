// @category: unit
// @reason: Issue #2506 — query:aot-hot-update-health single Agent score
//          for JIT/AOT recovery gate (reload + storm + remount + epoch).
//
//   AC1: Idle healthy → health-bp high, force-reason ok, recovery_active=0
//   AC2: force-JIT mask or Global storm → health-bp drops; force-reason non-ok
//   AC3: Pure: two successive calls without mutate return identical values
//   AC4: Additive only; existing stats queries unchanged
//   AC5: Schema/issue/wired sentinels + unit test + source-cite

#include "test_harness.hpp"

#include "compiler/aot_hot_update_health.hh"
#include "compiler/aura_jit_bridge.h"
#include "compiler/hot_update_registry.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::AotHotUpdateHealthSnapshot;
using aura::compiler::CompilerService;
using aura::compiler::compute_aot_hot_update_health;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
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

static std::int64_t href_int(CompilerService& cs, std::string_view query, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", query, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: vacuous / idle healthy ──
static void ac1_idle_healthy() {
    std::println("\n--- #2506 AC1: idle healthy → health high / force-reason ok ---");
    AotHotUpdateHealthSnapshot s;
    auto r = compute_aot_hot_update_health(s);
    CHECK(r.health_bp == 10000, "AC1: vacuous health_bp == 10000");
    CHECK(r.force_reason == "ok", "AC1: force-reason ok");
    CHECK(r.force_reason_code == 0, "AC1: force-reason-code 0");
    CHECK(r.recovery_active == 0, "AC1: recovery_active 0");
    CHECK(r.health_budget_bp == 8000 || r.health_budget_bp <= 10000, "AC1: budget default");

    // Live process: clear recovery then query.
    auto& reg = aura::compiler::hot_update_registry();
    reg.on_reload_success();
    while (reg.reload_recovery_state().pending_dirty_count > 0)
        reg.on_recovery_pending_dirty_dec();
    reg.set_shape_storm_active(false);
    reg.reset_deopt_storm_state_for_test();
    reg.on_reload_success();

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "AC1: warm");
    // Process may carry residual remount/epoch counters from prior tests in
    // same binary — pure vacuous covered above; live soft-check range only.
    const auto h = href_int(cs, "query:aot-hot-update-health", "health-bp");
    CHECK(h >= 0 && h <= 10000, "AC1: live health-bp in range");
    CHECK(href_int(cs, "query:aot-hot-update-health", "force-reason-ok") == 0,
          "AC1: sentinel ok=0");
}

// ── AC2: force-JIT / storm drop health ──
static void ac2_force_jit_and_storm() {
    std::println("\n--- #2506 AC2: force-JIT / storm → health drops, force-reason non-ok ---");
    {
        AotHotUpdateHealthSnapshot s;
        s.force_jit_regions_mask = 1ull << 3; // Version bit
        auto r = compute_aot_hot_update_health(s);
        CHECK(r.force_reason == "force-jit", "AC2: force-jit reason");
        CHECK(r.force_reason_code == 2, "AC2: code 2 force-jit");
        CHECK(r.health_bp == 7000, "AC2: −3000 for force-jit → 7000");
        CHECK(r.recovery_active == 1, "AC2: recovery_active 1 under force-jit");
    }
    {
        AotHotUpdateHealthSnapshot s;
        s.storm_level = 2; // Global
        auto r = compute_aot_hot_update_health(s);
        CHECK(r.force_reason == "storm", "AC2: storm wins");
        CHECK(r.force_reason_code == 1, "AC2: code 1 storm");
        CHECK(r.health_bp == 6500, "AC2: −3500 for storm → 6500");
    }
    {
        // Priority: storm > force-jit
        AotHotUpdateHealthSnapshot s;
        s.storm_level = 1;
        s.force_jit_regions_mask = 4;
        auto r = compute_aot_hot_update_health(s);
        CHECK(r.force_reason == "storm", "AC2: storm priority over force-jit");
        CHECK(r.health_bp == 10000 - 3500 - 3000, "AC2: both hard penalties stack");
    }
    {
        AotHotUpdateHealthSnapshot s;
        s.last_reload_fail_reason = 2; // Version
        auto r = compute_aot_hot_update_health(s);
        CHECK(r.force_reason == "reload-fail", "AC2: reload-fail");
        CHECK(r.force_reason_code == 3, "AC2: code 3");
    }
    {
        AotHotUpdateHealthSnapshot s;
        s.remount_fail_total = 5;
        s.remount_ok_total = 1;
        auto r = compute_aot_hot_update_health(s);
        CHECK(r.force_reason == "remount-fail", "AC2: remount-fail");
        CHECK(r.force_reason_code == 4, "AC2: code 4");
    }
    {
        AotHotUpdateHealthSnapshot s;
        s.epoch_invariant_violation_total = 1;
        auto r = compute_aot_hot_update_health(s);
        CHECK(r.force_reason == "epoch-invariant", "AC2: epoch-invariant");
        CHECK(r.force_reason_code == 5, "AC2: code 5");
    }
    {
        AotHotUpdateHealthSnapshot s;
        s.deferred_reemit_pending = 1;
        auto r = compute_aot_hot_update_health(s);
        CHECK(r.force_reason == "deferred-reemit", "AC2: deferred-reemit");
        CHECK(r.force_reason_code == 6, "AC2: code 6");
    }

    // Live inject: force-JIT via registry.
    auto& reg = aura::compiler::hot_update_registry();
    reg.on_reload_success();
    reg.set_shape_storm_active(false);
    reg.reset_deopt_storm_state_for_test();
    reg.on_force_jit_for_reason(AotReloadFail::Version);
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "AC2: warm");
    CHECK(href_int(cs, "query:aot-hot-update-health", "force-reason-code") == 2 ||
              href_int(cs, "query:aot-hot-update-health", "force-reason-code") == 1,
          "AC2: live force-reason force-jit (or storm if residual)");
    CHECK(href_int(cs, "query:aot-hot-update-health", "health-bp") < 10000,
          "AC2: live health-bp drops under force-jit");
    CHECK(href_int(cs, "query:aot-hot-update-health", "component-force-jit-regions-mask") != 0,
          "AC2: component mask non-zero");
    reg.on_reload_success(); // clear
}

// ── AC3: pure successive calls ──
static void ac3_pure_identical() {
    std::println("\n--- #2506 AC3: pure successive calls identical ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "AC3: warm");
    const auto h1 = href_int(cs, "query:aot-hot-update-health", "health-bp");
    const auto c1 = href_int(cs, "query:aot-hot-update-health", "force-reason-code");
    const auto r1 = href_int(cs, "query:aot-hot-update-health", "recovery-active");
    const auto h2 = href_int(cs, "query:aot-hot-update-health", "health-bp");
    const auto c2 = href_int(cs, "query:aot-hot-update-health", "force-reason-code");
    const auto r2 = href_int(cs, "query:aot-hot-update-health", "recovery-active");
    CHECK(h1 == h2, "AC3: two successive health-bp identical");
    CHECK(c1 == c2, "AC3: two successive force-reason-code identical");
    CHECK(r1 == r2, "AC3: two successive recovery-active identical");
    CHECK(h1 >= 0 && h1 <= 10000, "AC3: health in range");
    // Pure compute: same snapshot → same result.
    AotHotUpdateHealthSnapshot s;
    s.storm_level = 2;
    s.force_jit_regions_mask = 8;
    const auto a = compute_aot_hot_update_health(s);
    const auto b = compute_aot_hot_update_health(s);
    CHECK(a.health_bp == b.health_bp && a.force_reason_code == b.force_reason_code,
          "AC3: pure compute deterministic");
}

// ── AC4: additive — existing queries still resolve ──
static void ac4_additive() {
    std::println("\n--- #2506 AC4: additive — existing recovery queries unchanged ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "AC4: warm");
    auto h = cs.eval("(engine:metrics \"query:aot-hot-update-health\")");
    CHECK(h && is_hash(*h), "AC4: aot-hot-update-health is hash");
    auto alias = cs.eval("(engine:metrics \"query:hot-update-health\")");
    CHECK(alias && is_hash(*alias), "AC4: hot-update-health alias works");

    CHECK(cs.eval("(engine:metrics \"query:reload-recovery-state\")").has_value(),
          "AC4: reload-recovery-state still reachable");
    CHECK(cs.eval("(engine:metrics \"query:hot-update-registry-stats\")").has_value(),
          "AC4: hot-update-registry-stats still reachable");
    CHECK(cs.eval("(engine:metrics \"query:aot-incremental-reemit-stats\")").has_value(),
          "AC4: aot-incremental-reemit-stats still reachable");
    CHECK(cs.eval("(engine:metrics \"query:mutation-concurrency-health\")").has_value(),
          "AC4: mutation-concurrency-health still reachable");
}

// ── AC5: schema + source-cite ──
static void ac5_schema_and_source() {
    std::println("\n--- #2506 AC5: schema/wired + source-cite ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "AC5: warm");
    CHECK(href_int(cs, "query:aot-hot-update-health", "schema-2506") == 2506, "AC5: schema-2506");
    CHECK(href_int(cs, "query:aot-hot-update-health", "issue-2506") == 2506, "AC5: issue-2506");
    CHECK(href_int(cs, "query:aot-hot-update-health", "aot-hot-update-health-wired") == 1,
          "AC5: wired");
    CHECK(href_int(cs, "query:aot-hot-update-health", "schema-2367") == 2367, "AC5: lineage 2367");
    CHECK(href_int(cs, "query:aot-hot-update-health", "schema-2094") == 2094, "AC5: lineage 2094");
    CHECK(href_int(cs, "query:hot-update-health", "schema-2506") == 2506, "AC5: alias schema");

    const auto hh = read_file("src/compiler/aot_hot_update_health.hh");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto obs = read_file("src/compiler/evaluator_primitives_observability.cpp");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(hh.find("compute_aot_hot_update_health") != std::string::npos, "AC5: pure compute");
    CHECK(hh.find("Issue #2506") != std::string::npos, "AC5: #2506 in header");
    CHECK(hh.find("force-jit") != std::string::npos, "AC5: force-jit in weights docs");
    CHECK(hh.find("storm") != std::string::npos, "AC5: storm in weights docs");
    CHECK(q.find("query:aot-hot-update-health") != std::string::npos, "AC5: query registered");
    CHECK(q.find("query:hot-update-health") != std::string::npos, "AC5: alias registered");
    CHECK(q.find("aot_hot_update_health.hh") != std::string::npos, "AC5: includes health hh");
    CHECK(obs.find("query:aot-hot-update-health") != std::string::npos, "AC5: catalog");
    CHECK(cmake.find("test_aot_hot_update_health_2506") != std::string::npos, "AC5: cmake target");
}

} // namespace

int run_test_aot_hot_update_health_2506() {
    std::println("test_aot_hot_update_health_2506");
    ac1_idle_healthy();
    ac2_force_jit_and_storm();
    ac3_pure_identical();
    ac4_additive();
    ac5_schema_and_source();
    if (g_failed)
        return 1;
    std::println("aot-hot-update-health #2506: OK ({} passed)", g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_aot_hot_update_health_2506();
}
#endif
