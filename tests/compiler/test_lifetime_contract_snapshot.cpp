// @category: unit
// @reason: Issue #2300 — query:lifetime-contract-snapshot pure Agent surface
// for pin / linear / EnvFrame / GC-defer / residual contract.
//
//   AC1: Idle process → lifetime-contract-ok=1, reasons empty, depths 0.
//   AC2: Arm MutationHold + pin one linear root → mask + live counts;
//        pure (second call identical).
//   AC3: Inject pin-contract fail total → ok=0 + force-reason pin-miss.
//   AC4: Existing query keys unchanged; new keys additive with schema/issue.
//   AC5: Unit matrix + source-cite; no mutate side effects.
//
//   Issue #2341 (Refine #2300): unified post-densify consistency probe
//   (pin + RootRemap + linear + closure remount) layered on top of
//   lifetime-contract-snapshot. Refines #2266 · #2280 · #2294 · #2297
//   · #2295 · #2300; production review (2026-07-29) 建议 5.
//   AC_2340_1: DensifyConsistencyReport default-constructed report is
//              overall_ok + force_reason=="none".
//   AC_2341_2: Per-axis failure drives force_reason priority
//              (pin > linear > type > root_remap > closure > envframe > none).
//   AC_2341_3: densify_consistency_fail_total counter is queryable
//              + process-level atomic.
//   AC_2341_4: query:lifetime-contract-snapshot exposes #2341 keys
//              (schema-2341 + issue-2341 + densify-consistency-ok +
//              densify-force-reason-code + per-axis kebab+snake).
//   AC_2341_5: source-cite DensifyConsistencyReport + last_root_remap
//              _any_fail + Phase 5 driver + query surface.

#include "test_harness.hpp"

#include "core/densify_consistency_report.h" // Issue #2341
#include "core/gc_hooks.h"
#include "core/lifetime_contract.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.core.lifetime_pin;
import aura.core.envframe_lifetime;
import aura.compiler.root_remap_pass; // Issue #2341: last_root_remap_any_fail
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::densify_consistency::bump_densify_consistency_fail_total;
using aura::core::densify_consistency::densify_consistency_fail_total;
using aura::core::densify_consistency::DensifyConsistencyReport;
using aura::core::lifetime::g_linear_pin_miss_total;
using aura::core::lifetime::g_moving_compact_pin_contract_fail_total;
using aura::core::lifetime::linear_root_snapshot;
using aura::core::lifetime::live_pin_count;
using aura::core::lifetime::pin_linear_root;
using aura::core::lifetime::reset_linear_roots_for_test;
using aura::core::lifetime::unpin_linear_root;
using aura::core::lifetime_contract::kLifetimeContractIssue;
using aura::core::lifetime_contract::LifetimeContractSnapshot;
using aura::core::lifetime_contract::make_lifetime_contract_snapshot;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

std::int64_t href(CompilerService& cs, const char* q, const char* key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

LifetimeContractSnapshot live_snapshot(std::uint64_t residual_hard = 0) {
    const auto linear = linear_root_snapshot();
    return make_lifetime_contract_snapshot(
        static_cast<std::uint64_t>(live_pin_count()), static_cast<std::uint64_t>(linear.live_count),
        aura::core::envframe_lifetime::active_guard_depth(),
        aura::gc_hooks::defer_reasons_snapshot(),
        aura::core::lifetime::lifetime_pin_contract_fail_total(), linear.pin_miss_total,
        residual_hard, aura::gc_hooks::gc_defer_orphan_cleared_on_steal_total(),
        aura::core::lifetime_contract::residual_defer_policy_from_env(),
        aura::core::lifetime_contract::moving_compact_enabled_from_env());
}

void ac5_source_cite() {
    std::println("\n--- AC5: source-cite pure formula + query ---");
    auto h = read_file("src/core/lifetime_contract.h");
    auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(h.find("make_lifetime_contract_snapshot") != std::string::npos, "AC5: pure helper");
    CHECK(h.find("lifetime-contract-ok") != std::string::npos ||
              h.find("ok = (moving_pin_contract_fail_total") != std::string::npos ||
              h.find("Formula") != std::string::npos,
          "AC5: ok formula documented");
    CHECK(h.find("pin-miss") != std::string::npos && h.find("linear-miss") != std::string::npos,
          "AC5: force_reason codes documented");
    CHECK(kLifetimeContractIssue == 2300, "AC5: kLifetimeContractIssue == 2300");
    CHECK(q.find("query:lifetime-contract-snapshot") != std::string::npos, "AC5: query registered");
    CHECK(q.find("schema-2300") != std::string::npos && q.find("issue-2300") != std::string::npos,
          "AC5: schema-2300 / issue-2300");
    CHECK(q.find("lifetime-contract-wired") != std::string::npos, "AC5: wired sentinel");
    // AC4: existing subsystem queries retained (additive).
    CHECK(q.find("query:gc-defer-reason-stats") != std::string::npos,
          "AC4: gc-defer-reason-stats retained");
}

void ac1_idle_ok() {
    std::println("\n--- AC1: idle process ok ---");
    // Best-effort: clear linear roots for isolation.
    reset_linear_roots_for_test();
    // Do not clear pin registry / gc defer globally if other tests share process;
    // classify from pure zeros for matrix + soft-check live when clean.
    auto idle = make_lifetime_contract_snapshot(
        /*pins=*/0, /*linear=*/0, /*env=*/0, /*defer=*/0,
        /*pin_fail=*/0, /*lin_miss=*/0, /*residual=*/0, /*orphan=*/0,
        /*policy=*/1, /*moving=*/0);
    CHECK(idle.ok, "AC1: pure idle → ok");
    CHECK(idle.force_reason_code == 0, "AC1: force_reason_code=0");
    CHECK(std::string_view(idle.force_reason) == "none", "AC1: force_reason=none");
    CHECK(idle.lifetime_pin_live_count == 0, "AC1: pins 0");
    CHECK(idle.linear_pin_live_count == 0, "AC1: linear 0");
    CHECK(idle.envframe_active_guard_depth == 0, "AC1: envframe depth 0");
    CHECK(idle.gc_defer_reasons_mask == 0, "AC1: defer mask empty");

    CompilerService cs;
    const auto ok = href(cs, "query:lifetime-contract-snapshot", "lifetime-contract-ok");
    // If process is polluted, ok may be 0; still require query present.
    CHECK(ok == 0 || ok == 1, "AC1: query returns ok 0|1");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "schema-2300") == 2300, "AC1: schema-2300");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "lifetime-contract-wired") == 1,
          "AC1: wired=1");
}

void ac2_armed_hold_and_linear() {
    std::println("\n--- AC2: MutationHold + linear root live counts (pure) ---");
    reset_linear_roots_for_test();
    int dummy = 42;
    pin_linear_root(&dummy);
    aura::gc_hooks::arm_mutation_hold_defer();

    const auto s1 = live_snapshot();
    CHECK(s1.linear_pin_live_count >= 1, "AC2: linear-pin-live-count >= 1");
    CHECK((s1.gc_defer_reasons_mask &
           static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::MutationHold)) != 0,
          "AC2: MutationHold bit armed in mask");
    // Pure: second call identical (no bumps inside snapshot).
    const auto s2 = live_snapshot();
    CHECK(s2.linear_pin_live_count == s1.linear_pin_live_count, "AC2: pure linear count stable");
    CHECK(s2.gc_defer_reasons_mask == s1.gc_defer_reasons_mask, "AC2: pure mask stable");
    CHECK(s2.lifetime_pin_live_count == s1.lifetime_pin_live_count, "AC2: pure pin count stable");

    CompilerService cs;
    const auto live_lin = href(cs, "query:lifetime-contract-snapshot", "linear-pin-live-count");
    CHECK(live_lin >= 1, "AC2: query linear-pin-live-count >= 1");
    const auto mask = href(cs, "query:lifetime-contract-snapshot", "gc-defer-reasons-mask");
    CHECK(mask >= 0 &&
              (static_cast<std::uint32_t>(mask) &
               static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::MutationHold)) != 0,
          "AC2: query mask has MutationHold");

    aura::gc_hooks::release_mutation_hold_defer();
    unpin_linear_root(&dummy);
    reset_linear_roots_for_test();
}

void ac3_pin_contract_fail() {
    std::println("\n--- AC3: pin-contract fail → ok=0 pin-miss ---");
    // Inject via pure make (no need to corrupt process atomics permanently).
    auto bad = make_lifetime_contract_snapshot(
        /*pins=*/0, /*linear=*/0, /*env=*/0, /*defer=*/0,
        /*pin_fail=*/3, /*lin_miss=*/0, /*residual=*/0, /*orphan=*/0,
        /*policy=*/1, /*moving=*/0);
    CHECK(!bad.ok, "AC3: ok=0 on pin-contract fail");
    CHECK(std::string_view(bad.force_reason) == "pin-miss", "AC3: force_reason=pin-miss");
    CHECK(bad.force_reason_code == 1, "AC3: force_reason_code=1");

    // Priority: pin-miss wins over linear-miss.
    auto prio = make_lifetime_contract_snapshot(0, 0, 0, 0, /*pin_fail=*/1, /*lin_miss=*/9,
                                                /*residual=*/1, /*orphan=*/1, 1, 0);
    CHECK(!prio.ok, "AC3: ok=0 with multi miss");
    CHECK(std::string_view(prio.force_reason) == "pin-miss", "AC3: pin-miss priority");

    auto lin = make_lifetime_contract_snapshot(0, 0, 0, 0, 0, /*lin_miss=*/2, 0, 0, 1, 0);
    CHECK(!lin.ok, "AC3: linear-miss → ok=0");
    CHECK(std::string_view(lin.force_reason) == "linear-miss", "AC3: linear-miss reason");

    auto res = make_lifetime_contract_snapshot(0, 0, 0, 0, 0, 0, /*residual=*/1, 0, 1, 0);
    CHECK(!res.ok, "AC3: residual hard fail → ok=0");
    CHECK(std::string_view(res.force_reason) == "residual", "AC3: residual reason");

    // defer-orphan is attention only — ok stays 1.
    auto orphan = make_lifetime_contract_snapshot(0, 0, 0, 0, 0, 0, 0, /*orphan=*/5, 1, 0);
    CHECK(orphan.ok, "AC3: orphan alone keeps ok=1");
    CHECK(std::string_view(orphan.force_reason) == "defer-orphan", "AC3: defer-orphan reason");
    (void)g_moving_compact_pin_contract_fail_total;
    (void)g_linear_pin_miss_total;
}

void ac4_query_additive() {
    std::println("\n--- AC4: additive keys on live query ---");
    CompilerService cs;
    CHECK(href(cs, "query:lifetime-contract-snapshot", "lifetime-pin-live-count") >= 0,
          "AC4: lifetime-pin-live-count");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "linear-pin-live-count") >= 0,
          "AC4: linear-pin-live-count");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "envframe-active-guard-depth") >= 0,
          "AC4: envframe-active-guard-depth");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "gc-defer-reasons-mask") >= 0,
          "AC4: gc-defer-reasons-mask");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "residual-defer-policy") >= 0,
          "AC4: residual-defer-policy");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "moving-compact-enabled") >= 0,
          "AC4: moving-compact-enabled");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "moving-pin-contract-fail-total") >= 0,
          "AC4: moving-pin-contract-fail-total");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "force-reason-code") >= 0,
          "AC4: force-reason-code");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "issue-2300") == 2300, "AC4: issue-2300");
    // Sibling queries still present (AC4 existing keys unchanged).
    CHECK(href(cs, "query:gc-defer-reason-stats", "schema-2088") == 2088 ||
              href(cs, "query:gc-defer-reason-stats", "arm-panic-total") >= 0 ||
              href(cs, "query:gc-defer-reason-stats", "gc-defer-any-total") >= 0,
          "AC4: gc-defer-reason-stats still answers");
}

} // namespace

// Issue #2341 AC_2341_1: default-constructed DensifyConsistencyReport
// is overall_ok + force_reason=="none". Mirrors the soft / empty remap
// / no Moving trivially-ok contract. Sets baseline for AC_2341_2.
void ac2341_1_report_default_ok() {
    std::println("\n--- AC_2341_1: DensifyConsistencyReport default-ok ---");
    DensifyConsistencyReport r;
    CHECK(r.pin_ok, "AC_2341_1.1: pin_ok default true");
    CHECK(r.linear_ok, "AC_2341_1.2: linear_ok default true");
    CHECK(r.type_ok, "AC_2341_1.2b: type_ok default true (#2353)");
    CHECK(r.root_remap_ok, "AC_2341_1.3: root_remap_ok default true");
    CHECK(r.closure_remount_ok, "AC_2341_1.4: closure_remount_ok default true");
    CHECK(r.envframe_ok, "AC_2341_1.5: envframe_ok default true");
    CHECK(r.overall_ok(), "AC_2341_1.6: overall_ok default true");
    const auto* reason = r.force_reason();
    CHECK(reason != nullptr, "AC_2341_1.7: force_reason non-null");
    if (reason) {
        CHECK(std::string_view(reason) == "none", "AC_2341_1.8: force_reason default == 'none'");
    }
}

// Issue #2341 AC_2341_2: per-axis failure drives force_reason priority.
// pin > linear > type > root_remap > closure > envframe > none (#2353 type).
// Each axis failure flips overall_ok → false AND the most-severe failing
// axis drives the priority.
void ac2341_2_force_reason_priority() {
    std::println("\n--- AC_2341_2: force_reason priority "
                 "pin>linear>type>root_remap>closure>envframe ---");
    // Only pin fail: reason == "pin".
    {
        DensifyConsistencyReport r;
        r.pin_ok = false;
        CHECK(!r.overall_ok(), "AC_2341_2.1: pin-only fail → !overall_ok");
        CHECK(std::string_view(r.force_reason()) == "pin",
              "AC_2341_2.2: pin-only fail → force_reason == 'pin'");
    }
    // Only linear fail: reason == "linear".
    {
        DensifyConsistencyReport r;
        r.linear_ok = false;
        CHECK(!r.overall_ok(), "AC_2341_2.3: linear-only fail → !overall_ok");
        CHECK(std::string_view(r.force_reason()) == "linear",
              "AC_2341_2.4: linear-only fail → force_reason == 'linear'");
    }
    // Only type fail: reason == "type" (#2353).
    {
        DensifyConsistencyReport r;
        r.type_ok = false;
        CHECK(!r.overall_ok(), "AC_2341_2.4b: type-only fail → !overall_ok");
        CHECK(std::string_view(r.force_reason()) == "type",
              "AC_2341_2.4c: type-only fail → force_reason == 'type'");
    }
    // Only root_remap fail: reason == "root_remap".
    {
        DensifyConsistencyReport r;
        r.root_remap_ok = false;
        CHECK(!r.overall_ok(), "AC_2341_2.5: root_remap-only fail → !overall_ok");
        CHECK(std::string_view(r.force_reason()) == "root_remap",
              "AC_2341_2.6: root_remap-only fail → force_reason == 'root_remap'");
    }
    // Only closure fail: reason == "closure".
    {
        DensifyConsistencyReport r;
        r.closure_remount_ok = false;
        CHECK(!r.overall_ok(), "AC_2341_2.7: closure-only fail → !overall_ok");
        CHECK(std::string_view(r.force_reason()) == "closure",
              "AC_2341_2.8: closure-only fail → force_reason == 'closure'");
    }
    // Only envframe fail: reason == "envframe".
    {
        DensifyConsistencyReport r;
        r.envframe_ok = false;
        CHECK(!r.overall_ok(), "AC_2341_2.9: envframe-only fail → !overall_ok");
        CHECK(std::string_view(r.force_reason()) == "envframe",
              "AC_2341_2.10: envframe-only fail → force_reason == 'envframe'");
    }
    // Priority: all axes fail → reason == "pin" (most severe).
    {
        DensifyConsistencyReport r;
        r.pin_ok = false;
        r.linear_ok = false;
        r.type_ok = false;
        r.root_remap_ok = false;
        r.closure_remount_ok = false;
        r.envframe_ok = false;
        CHECK(std::string_view(r.force_reason()) == "pin",
              "AC_2341_2.11: all-axis fail → force_reason == 'pin' (priority)");
    }
    // Priority: linear + type fail → linear wins.
    {
        DensifyConsistencyReport r;
        r.linear_ok = false;
        r.type_ok = false;
        CHECK(std::string_view(r.force_reason()) == "linear",
              "AC_2341_2.12: linear+type fail → force_reason == 'linear'");
    }
}

// Issue #2341 AC_2341_3: densify_consistency_fail_total counter is
// queryable + process-level atomic + bumps monotonically.
void ac2341_3_counter_queryable() {
    std::println("\n--- AC_2341_3: densify_consistency_fail_total counter ---");
    const auto before = densify_consistency_fail_total();
    CHECK(before >= 0, "AC_2341_3.1: densify_consistency_fail_total queryable + >= 0");
    bump_densify_consistency_fail_total();
    const auto after = densify_consistency_fail_total();
    CHECK(after == before + 1, "AC_2341_3.2: bump_densify_consistency_fail_total increments by 1");
}

// Issue #2341 AC_2341_4: query:lifetime-contract-snapshot extends with
// #2341 keys. kebab + snake aliases per axis; overall ok flag;
// force_reason_code (priority int); fail-total counter; sentinel;
// schema + issue sentinels.
void ac2341_4_query_schema(CompilerService& cs) {
    std::println("\n--- AC_2341_4: query:lifetime-contract-snapshot #2341 surface ---");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "schema-2341") == 2341,
          "AC_2341_4.1: schema-2341 == 2341");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "issue-2341") == 2341,
          "AC_2341_4.2: issue-2341 == 2341");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "densify-consistency-wired") == 1,
          "AC_2341_4.3: densify-consistency-wired == 1 (proves #2341 wired)");
    // Per-axis kebab + snake (booleans, default 1 at idle).
    CHECK(href(cs, "query:lifetime-contract-snapshot", "densify-pin-ok") >= 0,
          "AC_2341_4.4: densify-pin-ok reachable (kebab)");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "densify_pin_ok") >= 0,
          "AC_2341_4.5: densify_pin_ok reachable (snake)");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "densify-linear-ok") >= 0,
          "AC_2341_4.6: densify-linear-ok reachable (kebab)");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "densify-type-ok") >= 0,
          "AC_2341_4.6b: densify-type-ok reachable (kebab, #2353)");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "densify_type_ok") >= 0,
          "AC_2341_4.6c: densify_type_ok reachable (snake, #2353)");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "densify-root-remap-ok") >= 0,
          "AC_2341_4.7: densify-root-remap-ok reachable (kebab)");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "densify-closure-remount-ok") >= 0,
          "AC_2341_4.8: densify-closure-remount-ok reachable (kebab)");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "densify-envframe-ok") >= 0,
          "AC_2341_4.9: densify-envframe-ok reachable (kebab)");
    // Overall + reason + counter.
    CHECK(href(cs, "query:lifetime-contract-snapshot", "densify-consistency-ok") >= 0,
          "AC_2341_4.10: densify-consistency-ok reachable (overall)");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "densify-force-reason-code") >= 0,
          "AC_2341_4.11: densify-force-reason-code reachable (priority int)");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "densify-consistency-fail-total") >= 0,
          "AC_2341_4.12: densify-consistency-fail-total reachable (counter)");
}

// Issue #2341 AC_2341_5: source-cite grep verifier. Each #2341 file
// must contain the Issue #2341 cite + the contract surface (header
// struct + atomic + accessor + bump helper + getter + report gate +
// query keys + ac2341_* test functions).
void ac2341_5_source_cite() {
    std::println("\n--- AC_2341_5: Issue #2341 source-cite across 5 files ---");
    auto check = [](const std::filesystem::path& p, std::initializer_list<const char*> needles,
                    std::string_view tag) {
        if (!std::filesystem::exists(p)) {
            CHECK(false, std::format("AC_2341_5: {} not found", p.string()).c_str());
            return;
        }
        std::ifstream in(p);
        std::stringstream buf;
        buf << in.rdbuf();
        const auto txt = buf.str();
        for (const auto* needle : needles) {
            CHECK(txt.find(needle) != std::string::npos,
                  std::format("AC_2341_5: {} contains {}", tag, needle).c_str());
        }
    };
    check(std::filesystem::path(AURA_SOURCE_DIR) / "src/core/densify_consistency_report.h",
          {"Issue #2341", "DensifyConsistencyReport", "force_reason",
           "g_densify_consistency_fail_total", "bump_densify_consistency_fail_total",
           "densify_consistency_fail_total", "densify_consistency_hard_contract_enabled"},
          "densify_consistency_report.h");
    check(std::filesystem::path(AURA_SOURCE_DIR) / "src/compiler/root_remap_pass.ixx",
          {"Issue #2341", "g_last_root_remap_any_fail", "last_root_remap_any_fail"},
          "root_remap_pass.ixx");
    check(std::filesystem::path(AURA_SOURCE_DIR) / "src/compiler/evaluator_mutation_boundary.cpp",
          {"Issue #2341", "DensifyConsistencyReport", "bump_densify_consistency_fail_total",
           "AURA_DENSIFY_CONTRACT"},
          "evaluator_mutation_boundary.cpp");
    check(std::filesystem::path(AURA_SOURCE_DIR) / "src/compiler/evaluator_primitives_obs_jit.cpp",
          {"Issue #2341", "densify-consistency-ok", "densify-force-reason-code",
           "densify-consistency-fail-total", "schema-2341", "issue-2341"},
          "evaluator_primitives_obs_jit.cpp");
}

int run_test_lifetime_contract_snapshot() {
    std::println("=== Issue #2300 + #2341: lifetime-contract-snapshot + densify consistency ===");
    ac5_source_cite();
    ac1_idle_ok();
    ac3_pin_contract_fail();
    ac2_armed_hold_and_linear();
    ac4_query_additive();
    ac2341_1_report_default_ok();
    ac2341_2_force_reason_priority();
    ac2341_3_counter_queryable();
    {
        CompilerService cs;
        ac2341_4_query_schema(cs);
    }
    ac2341_5_source_cite();
    std::println("\n=== #2300 + #2341: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
// build-fix binding refresh for obs_jit (#1453)

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_lifetime_contract_snapshot();
}
#endif
