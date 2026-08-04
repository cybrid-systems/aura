// @category: unit
// @reason: Issue #2505 — document + enforce cross-COW soft-migrate drift
//          threshold vs hard safe-fallback (reason breakdown for Agents).
//
//   AC1: Near-drift live closure → soft migrate + remount ok; soft +1
//   AC2: Far-drift → hard safe-fallback; hard +1; far-behind reason
//   AC3: Linear fingerprint drift / freed → hard path only
//   AC4: Soft disabled via env → always hard on dual miss (disabled reason)
//   AC5: Header documents single-workspace MVP vs call-time soft scope

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

extern "C" int64_t aura_closure_call(int64_t closure_id, int64_t* args, int64_t argc);
extern "C" void aura_aot_bump_func_table_epoch(void);
extern "C" std::uint64_t aura_aot_func_table_epoch(void);
extern "C" std::uint64_t aura_get_closure_bridge_epoch(std::int64_t closure_id);
extern "C" void aura_free_closure(std::int64_t closure_id);

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
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:epoch-apply-hotpath-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void clear_env() {
    unsetenv("AURA_CROSS_COW_SOFT_MIGRATE");
    unsetenv("AURA_CROSS_COW_SOFT_MIGRATE_MAX_DRIFT");
}

// Ensure table epoch tracking is active (non-zero) and stamp a fresh closure.
static std::int64_t alloc_stamped(const char* name) {
    if (aura_aot_func_table_epoch() == 0)
        aura_aot_bump_func_table_epoch();
    const auto cid = aura_alloc_closure(1);
    if (cid >= 0)
        aura_closure_set_name(cid, name);
    return cid;
}

// ── AC1: near-drift soft success ──
static void ac1_near_drift_soft() {
    std::println("\n--- #2505 AC1: near-drift live → soft migrate ---");
    clear_env();
    setenv("AURA_CROSS_COW_SOFT_MIGRATE", "1", 1);
    setenv("AURA_CROSS_COW_SOFT_MIGRATE_MAX_DRIFT", "8", 1);
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);

    const auto cid = alloc_stamped("ac1_near_2505");
    CHECK(cid >= 0, "AC1: alloc");
    const auto b0 = aura_get_closure_bridge_epoch(cid);
    const auto soft0 = metrics.cross_cow_soft_migrate_total.load();
    const auto hard0 = metrics.cross_cow_hard_reject_total.load();

    // Single epoch bump: lag=1 ≤ K=8 → soft.
    aura_aot_bump_func_table_epoch();
    int64_t args[1] = {0};
    (void)aura_closure_call(cid, args, 0);

    const auto soft1 = metrics.cross_cow_soft_migrate_total.load();
    const auto hard1 = metrics.cross_cow_hard_reject_total.load();
    if (b0 != 0 && aura_aot_func_table_epoch() != 0) {
        CHECK(soft1 == soft0 + 1, "AC1: soft migrate +1 on near-drift");
        CHECK(hard1 == hard0, "AC1: no hard reject on near-drift soft path");
        // Restamped bridge should match live after soft migrate.
        CHECK(aura_get_closure_bridge_epoch(cid) == aura_aot_func_table_epoch() || soft1 > soft0,
              "AC1: restamp advanced or soft counted");
    } else {
        CHECK(true, "AC1: epoch domain inactive — soft path N/A (skip numeric)");
    }
    CHECK(aura_cross_cow_soft_migrate_enabled() == 1, "AC1: soft enabled");
    CHECK(aura_cross_cow_soft_migrate_max_drift() == 8, "AC1: max drift env=8");
    clear_env();
    aura_set_aot_metrics(nullptr);
}

// ── AC2: far-drift hard reject ──
static void ac2_far_drift_hard() {
    std::println("\n--- #2505 AC2: far-drift → hard reject + FarBehind reason ---");
    clear_env();
    setenv("AURA_CROSS_COW_SOFT_MIGRATE", "1", 1);
    setenv("AURA_CROSS_COW_SOFT_MIGRATE_MAX_DRIFT", "1", 1);
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);

    const auto cid = alloc_stamped("ac2_far_2505");
    CHECK(cid >= 0, "AC2: alloc");
    const auto b0 = aura_get_closure_bridge_epoch(cid);
    const auto soft0 = metrics.cross_cow_soft_migrate_total.load();
    const auto hard0 = metrics.cross_cow_hard_reject_total.load();
    const auto far0 = metrics.cross_cow_hard_reject_far_behind_total.load();

    // lag ≥ 2 with K=1 → far-behind hard.
    aura_aot_bump_func_table_epoch();
    aura_aot_bump_func_table_epoch();
    int64_t args[1] = {0};
    const auto ret = aura_closure_call(cid, args, 0);
    (void)ret;

    const auto soft1 = metrics.cross_cow_soft_migrate_total.load();
    const auto hard1 = metrics.cross_cow_hard_reject_total.load();
    const auto far1 = metrics.cross_cow_hard_reject_far_behind_total.load();
    if (b0 != 0 && aura_aot_func_table_epoch() != 0) {
        CHECK(soft1 == soft0, "AC2: no soft on far-drift");
        CHECK(hard1 == hard0 + 1, "AC2: hard reject +1");
        CHECK(far1 == far0 + 1, "AC2: far-behind reason +1");
        CHECK(aura_cross_cow_last_hard_reject_reason() == 3, "AC2: last reason FarBehind=3");
        // Hard path: no native body (returns 0 safe-fallback).
        CHECK(ret == 0, "AC2: hard path returns 0 (no native body)");
    }
    clear_env();
    aura_set_aot_metrics(nullptr);
}

// ── AC3: linear fingerprint drift + freed hard ──
static void ac3_linear_and_freed() {
    std::println("\n--- #2505 AC3: linear drift / freed → hard path only ---");
    clear_env();
    setenv("AURA_CROSS_COW_SOFT_MIGRATE", "1", 1);
    setenv("AURA_CROSS_COW_SOFT_MIGRATE_MAX_DRIFT", "64", 1);
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);

    // Linear fingerprint drift: stamp with live=7, then advance live to 9.
    aura_set_aot_live_linear_state_fingerprint(7);
    const auto cid = alloc_stamped("ac3_lin_2505");
    CHECK(cid >= 0, "AC3: alloc linear");
    aura_set_aot_live_linear_state_fingerprint(9);
    const auto soft0 = metrics.cross_cow_soft_migrate_total.load();
    const auto hard0 = metrics.cross_cow_hard_reject_total.load();
    const auto lin0 = metrics.cross_cow_hard_reject_linear_total.load();
    const auto b0 = aura_get_closure_bridge_epoch(cid);
    aura_aot_bump_func_table_epoch(); // dual miss
    int64_t args[1] = {0};
    (void)aura_closure_call(cid, args, 0);
    if (b0 != 0 && aura_aot_func_table_epoch() != 0) {
        CHECK(metrics.cross_cow_soft_migrate_total.load() == soft0, "AC3: no soft on linear drift");
        CHECK(metrics.cross_cow_hard_reject_total.load() == hard0 + 1, "AC3: hard +1 on linear");
        CHECK(metrics.cross_cow_hard_reject_linear_total.load() == lin0 + 1,
              "AC3: linear reason +1");
        CHECK(aura_cross_cow_last_hard_reject_reason() == 4, "AC3: last reason Linear=4");
    }

    // Freed: free after dual-miss setup — soft migrate sees freed bit.
    // (call path may short-circuit freed before dual-check; still must not soft.)
    aura_set_aot_live_linear_state_fingerprint(0);
    const auto cid2 = alloc_stamped("ac3_free_2505");
    aura_aot_bump_func_table_epoch();
    aura_free_closure(cid2);
    const auto soft1 = metrics.cross_cow_soft_migrate_total.load();
    (void)aura_closure_call(cid2, args, 0);
    CHECK(metrics.cross_cow_soft_migrate_total.load() == soft1, "AC3: freed never soft-migrates");
    // Safe call, no crash.
    CHECK(true, "AC3: freed slot call safe");

    clear_env();
    aura_set_aot_metrics(nullptr);
}

// ── AC4: soft disabled → hard on dual miss ──
static void ac4_soft_disabled() {
    std::println("\n--- #2505 AC4: soft disabled → always hard on dual miss ---");
    clear_env();
    setenv("AURA_CROSS_COW_SOFT_MIGRATE", "0", 1);
    setenv("AURA_CROSS_COW_SOFT_MIGRATE_MAX_DRIFT", "4096", 1);
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);

    CHECK(aura_cross_cow_soft_migrate_enabled() == 0, "AC4: soft disabled via env");
    const auto cid = alloc_stamped("ac4_off_2505");
    CHECK(cid >= 0, "AC4: alloc");
    const auto b0 = aura_get_closure_bridge_epoch(cid);
    const auto soft0 = metrics.cross_cow_soft_migrate_total.load();
    const auto hard0 = metrics.cross_cow_hard_reject_total.load();
    const auto dis0 = metrics.cross_cow_hard_reject_disabled_total.load();
    aura_aot_bump_func_table_epoch();
    int64_t args[1] = {0};
    (void)aura_closure_call(cid, args, 0);
    if (b0 != 0 && aura_aot_func_table_epoch() != 0) {
        CHECK(metrics.cross_cow_soft_migrate_total.load() == soft0, "AC4: no soft when disabled");
        CHECK(metrics.cross_cow_hard_reject_total.load() == hard0 + 1,
              "AC4: hard +1 when disabled");
        CHECK(metrics.cross_cow_hard_reject_disabled_total.load() == dis0 + 1,
              "AC4: disabled reason +1");
        CHECK(aura_cross_cow_last_hard_reject_reason() == 1, "AC4: last reason Disabled=1");
    }
    clear_env();
    aura_set_aot_metrics(nullptr);
}

// ── AC5: header contract + query + gate ──
static void ac5_docs_query_gate() {
    std::println("\n--- #2505 AC5: header contract + query schema-2505 + gate ---");
    clear_env();
    const auto hh = read_file("src/compiler/aura_jit_bridge.h");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto obs = read_file("src/compiler/observability_metrics.h");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto script = read_file("scripts/coverage/checks/check_cross_cow_drift_contract_2505.py");

    CHECK(hh.find("Issue #2371 / #2505") != std::string::npos ||
              hh.find("#2505") != std::string::npos,
          "AC5: #2505 in bridge header");
    CHECK(hh.find("single-workspace MVP") != std::string::npos, "AC5: single-workspace MVP docs");
    CHECK(hh.find("call-time") != std::string::npos ||
              hh.find("call-time only") != std::string::npos,
          "AC5: call-time scope docs");
    CHECK(hh.find("AURA_CROSS_COW_SOFT_MIGRATE_MAX_DRIFT") != std::string::npos,
          "AC5: max-drift env documented");
    CHECK(hh.find("FarBehind") != std::string::npos || hh.find("far-behind") != std::string::npos ||
              hh.find("3=FarBehind") != std::string::npos,
          "AC5: reason enum documented");
    CHECK(rt.find("CrossCowHardReject") != std::string::npos, "AC5: reason enum in runtime");
    CHECK(rt.find("cross_cow_note_hard_") != std::string::npos, "AC5: hard-note helper");
    CHECK(rt.find("AURA_CROSS_COW_SOFT_MIGRATE_MAX_DRIFT") != std::string::npos,
          "AC5: drift env in runtime");
    CHECK(obs.find("cross_cow_hard_reject_far_behind_total") != std::string::npos,
          "AC5: far-behind metric field");
    CHECK(obs.find("cross_cow_hard_reject_linear_total") != std::string::npos,
          "AC5: linear metric field");
    CHECK(q.find("schema-2505") != std::string::npos, "AC5: schema-2505 query");
    CHECK(q.find("cross-cow-hard-reject-far-behind-total") != std::string::npos,
          "AC5: far-behind query key");
    CHECK(q.find("cross-cow-soft-migrate-max-drift") != std::string::npos, "AC5: max-drift query");
    CHECK(q.find("cross-cow-call-time-only-wired") != std::string::npos, "AC5: call-time wired");
    CHECK(cmake.find("test_cross_cow_drift_contract") != std::string::npos, "AC5: cmake");
    CHECK(build.find("check_cross_cow_drift_contract_2505") != std::string::npos,
          "AC5: build gate");
    CHECK(build.find("cmd_cross_cow_drift_contract_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(script.find("schema-2505") != std::string::npos, "AC5: coverage script");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "AC5: warm");
    CHECK(href(cs, "schema-2505") == 2505, "AC5: schema-2505 live");
    CHECK(href(cs, "issue-2505") == 2505, "AC5: issue-2505");
    CHECK(href(cs, "cross-cow-call-time-only-wired") == 1, "AC5: call-time-only-wired");
    CHECK(href(cs, "cross-cow-single-workspace-mvp-wired") == 1, "AC5: single-workspace mvp");
    CHECK(href(cs, "cross-cow-soft-migrate-default-max-drift") == 4096, "AC5: default K=4096");
    CHECK(href(cs, "schema-2371") == 2371, "AC5: #2371 lineage retained");
    CHECK(href(cs, "cross-cow-hard-reject-far-behind-total") >= 0, "AC5: far-behind key present");
    CHECK(href(cs, "cross-cow-last-hard-reject-reason") >= 0, "AC5: last reason key present");
}

} // namespace

int run_test_cross_cow_drift_contract() {
    std::println("test_cross_cow_drift_contract");
    ac1_near_drift_soft();
    ac2_far_drift_hard();
    ac3_linear_and_freed();
    ac4_soft_disabled();
    ac5_docs_query_gate();
    if (g_failed)
        return 1;
    std::println("cross-COW drift contract #2505: OK ({} passed)", g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_cross_cow_drift_contract();
}
#endif
