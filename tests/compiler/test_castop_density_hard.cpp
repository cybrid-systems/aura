// @category: unit
// @reason: Issue #2358 — opt-in hard CastOp density policy (force-JIT
// codegen degrade when dens > budget; mutate still succeeds).
//
//   AC1: HARD=0 + dens>budget → no hard_action, no force-JIT side effect
//   AC2: HARD=1 + dens>budget → hard_action_total ≥ 1 + force-JIT mask
//   AC3: Mutate still succeeds under HARD=1 (policy is codegen, not type)
//   AC4: dens ≤ budget → zero hard action
//   AC5: schema-2358 additive; #2287/#2319 keys preserved; source-cite

#include "test_harness.hpp"

#include "compiler/castop_density_policy.hh"
#include "compiler/hot_update_registry.hh"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::hot_update_registry;
using aura::compiler::castop_density::apply_hard_policy;
using aura::compiler::castop_density::g_hot_residual_density_keep_total;
using aura::compiler::castop_density::g_hot_residual_nonidentity_total;
using aura::compiler::castop_density::g_hot_residual_relower_total;
using aura::compiler::castop_density::g_hot_residual_soft_must_deopt_pending;
using aura::compiler::castop_density::g_hot_residual_soft_must_deopt_total;
using aura::compiler::castop_density::hard_env_enabled;
using aura::compiler::castop_density::hot_residual_soft_must_deopt_pending;
using aura::compiler::castop_density::kCastOpHotResidualNonidentityIssue;
using aura::compiler::castop_density::kCastOpHotResidualSoftMustDeoptIssue;
using aura::compiler::castop_density::note_hot_residual_nonidentity_castops;
using aura::compiler::castop_meta::castop_typed_meta_phase_c_deopt_total;
using aura::compiler::castop_meta::castop_typed_meta_phase_c_lags;
using aura::compiler::castop_meta::castop_typed_meta_phase_c_wired;
using aura::compiler::castop_meta::clear_castop_typed_meta_for_test;
using aura::compiler::castop_meta::kCastOpTypedMetaPhaseCIssue;
using aura::compiler::castop_meta::lookup_castop_typed_meta;
using aura::compiler::castop_meta::make_site_key;
using aura::compiler::castop_meta::reset_castop_typed_meta_phase_c_for_test;
using aura::compiler::castop_meta::set_castop_typed_meta_phase_c_wired_for_test;
using aura::compiler::castop_meta::stamp_castop_typed_meta;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:castop-density-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href_layered(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:dead-coercion-layered-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: soft default — no force-JIT action ──
static void ac1_hard_off_soft_only() {
    std::println("\n--- AC1: HARD=0 + dens>budget → no hard_action ---");
    CompilerMetrics m;
    m.last_castop_density_bp.store(5000, std::memory_order_relaxed);
    m.castop_density_budget_bp.store(1500, std::memory_order_relaxed);
    const auto act0 = m.castop_density_hard_action_total.load();
    const auto mask0 = hot_update_registry().reload_recovery_state().force_jit_regions_mask;
    // hard_override=0 forces soft even if env set.
    CHECK(!apply_hard_policy(m, /*dens=*/5000, /*budget=*/1500, /*hard_override=*/0),
          "AC1: soft override returns false");
    CHECK(m.castop_density_hard_action_total.load() == act0, "AC1: hard_action unchanged");
    CHECK(m.castop_density_hard_enabled.load() == 0, "AC1: hard_enabled=0");
    CHECK(hot_update_registry().reload_recovery_state().force_jit_regions_mask == mask0,
          "AC1: force_jit mask unchanged");
}

// ── AC2: HARD=1 over budget → action + force-JIT ──
static void ac2_hard_on_force_jit() {
    std::println("\n--- AC2: HARD=1 + dens>budget → hard_action + force-JIT ---");
    CompilerMetrics m;
    // Make unannotated Dynamic heuristic fire for #2319 reject metric too.
    m.coercion_castop_emitted_total.store(100, std::memory_order_relaxed);
    m.dead_coercion_elim_total.store(1, std::memory_order_relaxed);
    const auto act0 = m.castop_density_hard_action_total.load();
    const auto rej0 = m.castop_density_hard_reject_total.load();
    CHECK(apply_hard_policy(m, /*dens=*/5000, /*budget=*/1500, /*hard_override=*/1),
          "AC2: hard policy fires");
    CHECK(m.castop_density_hard_action_total.load() == act0 + 1, "AC2: hard_action_total +1");
    CHECK(m.castop_density_hard_enabled.load() == 1, "AC2: hard_enabled=1");
    CHECK(m.castop_density_hard_wired.load() == 1, "AC2: hard_wired=1");
    CHECK(m.castop_density_hard_reject_total.load() >= rej0 + 1,
          "AC2: unannotated residual also bumps hard_reject (#2319 lineage)");
    const auto mask = hot_update_registry().reload_recovery_state().force_jit_regions_mask;
    CHECK(mask != 0, "AC2: force_jit_regions_mask non-zero after force-JIT");
}

// ── AC3: mutate still succeeds under HARD (codegen policy only) ──
static void ac3_mutate_still_succeeds() {
    std::println("\n--- AC3: mutate succeeds under HARD=1 ---");
    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);
    // Soft path: hard policy does not gate eval.
    CHECK(cs.eval("(+ 1 1)").has_value(), "AC3: basic eval ok");
    CHECK(cs.eval("(let ((x 1)) x)").has_value(), "AC3: let eval ok");
    // Simulate hard fire mid-session — eval still works.
    (void)apply_hard_policy(metrics, 9000, 1500, /*hard_override=*/1);
    CHECK(cs.eval("(+ 2 3)").has_value(), "AC3: eval after hard action still succeeds");
}

// ── AC4: under budget → no action ──
static void ac4_under_budget_zero_extra() {
    std::println("\n--- AC4: dens ≤ budget → no hard action ---");
    CompilerMetrics m;
    const auto act0 = m.castop_density_hard_action_total.load();
    CHECK(!apply_hard_policy(m, /*dens=*/800, /*budget=*/1500, /*hard_override=*/1),
          "AC4: under budget returns false even if HARD on");
    CHECK(m.castop_density_hard_action_total.load() == act0, "AC4: action total unchanged");
    CHECK(m.castop_density_hard_enabled.load() == 1, "AC4: enabled still recorded");
}

// ── AC5: query + source-cite ──
static void ac5_query_and_source() {
    std::println("\n--- AC5: schema-2358 + source-cite ---");
    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);
    metrics.castop_density_hard_action_total.store(3, std::memory_order_relaxed);
    metrics.castop_density_hard_enabled.store(1, std::memory_order_relaxed);
    metrics.castop_density_budget_bp.store(1500, std::memory_order_relaxed);
    metrics.last_castop_density_bp.store(800, std::memory_order_relaxed);

    CHECK(href(cs, "schema-2358") == 2358, "AC5: schema-2358");
    CHECK(href(cs, "issue-2358") == 2358, "AC5: issue-2358");
    CHECK(href(cs, "castop-density-hard-action-wired") == 1, "AC5: wired");
    CHECK(href(cs, "castop-density-hard-action-total") == 3, "AC5: action-total");
    CHECK(href(cs, "castop-density-hard-enabled") == 1, "AC5: hard-enabled");
    // Lineage
    CHECK(href(cs, "schema-2319") == 2319, "AC5: schema-2319 retained");
    CHECK(href(cs, "castop-annotation-hint") == 0, "AC5: soft hint still works");
    CHECK(href(cs, "castop-density-over-budget-total") >= 0, "AC5: #2287 over-budget key");

    const auto sd = read_file("src/compiler/service_dirty.cpp");
    const auto pol = read_file("src/compiler/castop_density_policy.hh");
    const auto q = ::aura::test::aura_query_prims_source() +
                   read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    const auto met = read_file("src/compiler/observability_metrics.h");
    CHECK(pol.find("apply_hard_policy") != std::string::npos, "AC5: policy helper");
    CHECK(pol.find("on_force_jit_for_reason") != std::string::npos, "AC5: force-JIT call");
    CHECK(pol.find("Issue #2358") != std::string::npos ||
              sd.find("Issue #2358") != std::string::npos,
          "AC5: cites #2358");
    CHECK(sd.find("apply_hard_policy") != std::string::npos, "AC5: service_dirty wires policy");
    CHECK(q.find("schema-2358") != std::string::npos, "AC5: query schema");
    CHECK(met.find("castop_density_hard_action_total") != std::string::npos, "AC5: metrics field");
    CHECK(!hard_env_enabled(0), "AC5: hard_env_enabled(0) false");
    CHECK(hard_env_enabled(1), "AC5: hard_env_enabled(1) true");
}

// ── Issue #3046: residual non-identity CastOp density keep ──
static void ac3046_residual_nonidentity() {
    std::println("\n--- #3046: residual non-identity CastOp ---");
    CHECK(kCastOpHotResidualNonidentityIssue == 3046, "3046: issue stamp");
    CHECK(note_hot_residual_nonidentity_castops(0) == 0, "3046 AC3: Quiet leftover 0");
    const auto n0 = g_hot_residual_nonidentity_total.load();
    const auto k0 = g_hot_residual_density_keep_total.load();
    CHECK(note_hot_residual_nonidentity_castops(2, nullptr, /*production=*/0) == 2,
          "3046 AC3: Soft observe");
    CHECK(g_hot_residual_nonidentity_total.load() == n0 + 2, "3046 AC3: Soft counter");
    CHECK(g_hot_residual_density_keep_total.load() == k0, "3046 AC3: Soft no keep");
    CHECK(note_hot_residual_nonidentity_castops(1, nullptr, /*production=*/1) == 1,
          "3046 AC2: Production leftover");
    CHECK(g_hot_residual_density_keep_total.load() > k0, "3046 AC2: density-policy keep");
    CHECK(read_file("src/compiler/castop_density_policy.hh").find("#3046") != std::string::npos,
          "3046: policy cites #3046");
    CHECK(read_file("src/compiler/coercion_map.ixx").find("kCoercionBlameHfLagIssue") !=
              std::string::npos,
          "3046: coercion_map session face");
}

// ── Issue #3084: Soft residual CastOp must force deopt (no relower) ──
static void ac3084_1_soft_residual_must_deopt() {
    std::println("\n--- #3084 AC1: Soft leftover → MustDeopt, no relower ---");
    CHECK(kCastOpHotResidualSoftMustDeoptIssue == 3084, "3084 AC1: issue stamp");
    const auto n0 = g_hot_residual_nonidentity_total.load();
    const auto d0 = g_hot_residual_soft_must_deopt_total.load();
    const auto k0 = g_hot_residual_density_keep_total.load();
    const auto r0 = g_hot_residual_relower_total.load();
    g_hot_residual_soft_must_deopt_pending.store(0, std::memory_order_relaxed);
    CHECK(note_hot_residual_nonidentity_castops(2, nullptr, /*production=*/0) == 2,
          "3084 AC1: Soft leftover returned");
    CHECK(g_hot_residual_nonidentity_total.load() == n0 + 2, "3084 AC1: observe counter");
    CHECK(g_hot_residual_soft_must_deopt_total.load() == d0 + 1, "3084 AC1: MustDeopt total");
    CHECK(hot_residual_soft_must_deopt_pending(), "3084 AC1: pending MustDeopt");
    CHECK(g_hot_residual_density_keep_total.load() == k0, "3084 AC1: Soft no density-keep");
    CHECK(g_hot_residual_relower_total.load() == r0, "3084 AC1: Soft no relower");
    const auto pol = read_file("src/compiler/castop_density_policy.hh");
    CHECK(pol.find("Issue #3084") != std::string::npos, "3084 AC1: policy cites #3084");
    CHECK(pol.find("aura_jit_batch_deopt_for") != std::string::npos ||
              pol.find("on_stale_deopt") != std::string::npos,
          "3084 AC1: force-deopt equivalent");
}

static void ac3084_2_production_relower_unchanged() {
    std::println("\n--- #3084 AC2: Production still density-keep + relower ---");
    const auto k0 = g_hot_residual_density_keep_total.load();
    const auto r0 = g_hot_residual_relower_total.load();
    CHECK(note_hot_residual_nonidentity_castops(1, nullptr, /*production=*/1) == 1,
          "3084 AC2: Production leftover");
    CHECK(g_hot_residual_density_keep_total.load() > k0, "3084 AC2: density-policy keep");
    CHECK(g_hot_residual_relower_total.load() > r0, "3084 AC2: force-JIT/relower");
    const auto pol = read_file("src/compiler/castop_density_policy.hh");
    CHECK(pol.find("on_force_jit_for_reason") != std::string::npos, "3084 AC2: Production relower");
}

static void ac3084_3_quiet_zero() {
    std::println("\n--- #3084 AC3: leftover == 0 → zero extra ---");
    const auto n0 = g_hot_residual_nonidentity_total.load();
    const auto d0 = g_hot_residual_soft_must_deopt_total.load();
    const auto k0 = g_hot_residual_density_keep_total.load();
    CHECK(note_hot_residual_nonidentity_castops(0) == 0, "3084 AC3: Quiet 0");
    CHECK(g_hot_residual_nonidentity_total.load() == n0, "3084 AC3: observe quiet");
    CHECK(g_hot_residual_soft_must_deopt_total.load() == d0, "3084 AC3: MustDeopt quiet");
    CHECK(g_hot_residual_density_keep_total.load() == k0, "3084 AC3: keep quiet");
    const auto opt = read_file("src/compiler/optimization_passes.ixx");
    CHECK(opt.find("leftover == 0") != std::string::npos ||
              opt.find("leftover==0") != std::string::npos,
          "3084 AC3: Soft sweep Quiet leftover==0");
}

static void ac3084_4_blame_default_unchanged() {
    std::println("\n--- #3084 AC4: Soft blame-complete default unchanged ---");
    const auto pol = read_file("src/compiler/coercion_provenance_policy.hh");
    CHECK(pol.find("require_blame_complete_on_commit = false (#2221 observe-only)") !=
              std::string::npos,
          "3084 AC4: Soft default observe-only");
    CHECK(pol.find("require_blame_complete_on_commit = true under Restricted/Strict") !=
              std::string::npos,
          "3084 AC4: Restricted/Strict still force");
    CHECK(read_file("src/compiler/castop_density_policy.hh")
                  .find("require_blame_complete_on_commit") == std::string::npos,
          "3084 AC4: residual path does not flip blame policy");
}

static void ac3084_5_schema_and_linter() {
    std::println("\n--- #3084 AC5: schema + linter + no invent ---");
    const auto q = ::aura::test::aura_query_prims_source() +
                   read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    CHECK(q.find("schema-3084") != std::string::npos, "3084 AC5: schema-3084");
    CHECK(q.find("hot-residual-soft-must-deopt-total") != std::string::npos,
          "3084 AC5: MustDeopt key");
    CHECK(q.find("schema-3046") != std::string::npos, "3084 AC5: lineage #3046");
    CHECK(read_file("src/compiler/castop_density_policy.hh")
                  .find("kCastOpHotResidualSoftMustDeoptIssue = 3084") != std::string::npos,
          "3084 AC5: issue stamp");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "3084 AC5: warm");
    CHECK(href_layered(cs, "schema-3084") == 3084, "3084 AC5: live schema-3084");
    CHECK(href_layered(cs, "issue-3084") == 3084, "3084 AC5: live issue-3084");
    CHECK(href_layered(cs, "hot-residual-soft-must-deopt-wired") == 1, "3084 AC5: wired");
    CHECK(href_layered(cs, "hot-residual-soft-must-deopt-total") >= 0, "3084 AC5: total");
    CHECK(href_layered(cs, "schema-3046") == 3046, "3084 AC5: schema-3046 preserved");
    const auto t = read_file("tests/compiler/test_castop_density_hard.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_hot_residual_soft_must_deopt_3084.py");
    const auto build = read_file("build.py");
    CHECK(t.find("ac3084_1_soft_residual_must_deopt") != std::string::npos, "3084 AC5: AC1");
    CHECK(t.find("ac3084_2_production_relower_unchanged") != std::string::npos, "3084 AC5: AC2");
    CHECK(t.find("ac3084_3_quiet_zero") != std::string::npos, "3084 AC5: AC3");
    CHECK(t.find("ac3084_4_blame_default_unchanged") != std::string::npos, "3084 AC5: AC4");
    CHECK(!lint.empty() && lint.find("Issue #3084") != std::string::npos, "3084 AC5: linter");
    CHECK(build.find("check_hot_residual_soft_must_deopt_3084") != std::string::npos,
          "3084 AC5: build.py");
    CHECK(read_file("tests/compiler/test_issue_3084.cpp").empty(),
          "3084 AC5: no invent test_issue_3084");
    CHECK(read_file("docs/design/3084-hot-residual-soft-must-deopt.md").empty(),
          "3084 AC5: no docs/design/");
}

// ── #3107: Soft residual on a hot function → force-relower ────────────
//
// Closes the "Soft MustDeopt-only" window where leftover CastOp could
// still execute before the deopt barrier fires under AI multi-round
// mutate (canary / AURA_SANDBOX=off). Production path unchanged
// (#3046 / #3084 — density-keep + force-JIT/relower). Soft path now
// bumps a separate g_hot_residual_soft_relower_total counter when the
// caller has identified a specific hot function (fn_name provided); the
// existing aura_jit_batch_deopt_for call (already in the Soft branch)
// is the force-relower mechanism — we just track it separately so
// production-soak / agent-self-modify gates can observe.

static void ac3107_1_soft_residual_force_relower() {
    std::println("\n--- #3107 AC1: Production path unchanged (#3046/#3084) ---");
    const auto pol = read_file("src/compiler/castop_density_policy.hh");
    CHECK(pol.find("kCastOpHotResidualSoftRelowerIssue = 3107") != std::string::npos,
          "3107 AC1: issue stamp #3107");
    CHECK(pol.find("g_hot_residual_soft_relower_total") != std::string::npos,
          "3107 AC1: new additive counter");
    // Production branch unchanged: density-keep + force-JIT/relower
    CHECK(pol.find("g_hot_residual_density_keep_total") != std::string::npos,
          "3107 AC1: Production density-keep unchanged");
    CHECK(pol.find("on_force_jit_for_reason(AotReloadFail::Other)") != std::string::npos,
          "3107 AC1: Production force-JIT path unchanged");
    // Soft branch: when fn_name is provided, bump the new counter AND
    // call aura_jit_batch_deopt_for (the force-relower mechanism).
    CHECK(pol.find("g_hot_residual_soft_relower_total.fetch_add(1") != std::string::npos,
          "3107 AC1: Soft bump on fn_name");
    CHECK(pol.find("aura_jit_batch_deopt_for(fn_name, 0)") != std::string::npos,
          "3107 AC1: aura_jit_batch_deopt_for retained as force-relower mechanism");
}

static void ac3107_2_hot_fn_force_relower() {
    std::println("\n--- #3107 AC2: Soft hot-function force-relower (via fn_name) ---");
    // Soft branch must still mark MustDeopt (belt-and-suspenders) AND
    // additionally bump the soft-relower counter + call deopt batch.
    const auto pol = read_file("src/compiler/castop_density_policy.hh");
    CHECK(pol.find("g_hot_residual_soft_must_deopt_total") != std::string::npos,
          "3107 AC2: Soft MustDeopt counter unchanged");
    CHECK(pol.find("g_hot_residual_soft_must_deopt_pending") != std::string::npos,
          "3107 AC2: Soft MustDeopt pending flag unchanged");
    // Verify the counter actually bumps under Soft path with fn_name.
    // Use note_hot_residual_nonidentity_castops with leftover>0, soft
    // (production_override=0), and fn_name set.
    const auto before = aura::compiler::castop_density::g_hot_residual_soft_relower_total.load(
        std::memory_order_relaxed);
    aura::compiler::castop_density::note_hot_residual_nonidentity_castops(
        /*leftover=*/3, /*m=*/nullptr, /*production_override=*/0, /*fn_name=*/"hot_fn_3107");
    const auto after = aura::compiler::castop_density::g_hot_residual_soft_relower_total.load(
        std::memory_order_relaxed);
    CHECK(after > before,
          "3107 AC2: Soft fn_name residual bumps g_hot_residual_soft_relower_total");
    // And MustDeopt counter also bumps (belt-and-suspenders).
    const auto md_before =
        aura::compiler::castop_density::g_hot_residual_soft_must_deopt_total.load(
            std::memory_order_relaxed);
    aura::compiler::castop_density::note_hot_residual_nonidentity_castops(
        /*leftover=*/1, /*m=*/nullptr, /*production_override=*/0, /*fn_name=*/"hot_fn_3107_md");
    const auto md_after = aura::compiler::castop_density::g_hot_residual_soft_must_deopt_total.load(
        std::memory_order_relaxed);
    CHECK(md_after > md_before, "3107 AC2: Soft fn_name residual also bumps MustDeopt counter");
}

static void ac3107_3_quiet_zero_cost() {
    std::println("\n--- #3107 AC3: Quiet leftover==0 zero-cost ---");
    const auto before = aura::compiler::castop_density::g_hot_residual_soft_relower_total.load(
        std::memory_order_relaxed);
    // leftover=0 → returns 0 immediately, no counter bump.
    const auto ret = aura::compiler::castop_density::note_hot_residual_nonidentity_castops(
        /*leftover=*/0);
    CHECK(ret == 0, "3107 AC3: leftover==0 returns 0");
    const auto after = aura::compiler::castop_density::g_hot_residual_soft_relower_total.load(
        std::memory_order_relaxed);
    CHECK(after == before, "3107 AC3: leftover==0 does not bump soft-relower counter");
}

static void ac3107_4_additive_counter_only() {
    std::println("\n--- #3107 AC4: additive counter only, no new dirty bits ---");
    const auto pol = read_file("src/compiler/castop_density_policy.hh");
    // The diff must be only additive (one new std::atomic counter + one
    // new std::atomic wired flag + one new constexpr issue stamp + the
    // Soft-branch fetch_add). No new permanent dirty bits on Quiet path.
    CHECK(pol.find("g_hot_residual_soft_relower_total") != std::string::npos,
          "3107 AC4: additive counter");
    CHECK(pol.find("g_hot_residual_soft_relower_wired{1}") != std::string::npos,
          "3107 AC4: additive wired flag");
    CHECK(pol.find("kCastOpHotResidualSoftRelowerIssue = 3107") != std::string::npos,
          "3107 AC4: additive issue stamp");
}

static void ac3107_5_schema_and_linter() {
    std::println("\n--- #3107 AC5: schema + linter + no invent ---");
    const auto pol = read_file("src/compiler/castop_density_policy.hh");
    CHECK(pol.find("kCastOpHotResidualSoftRelowerIssue = 3107") != std::string::npos,
          "3107 AC5: issue stamp");
    const auto lint = read_file("scripts/coverage/checks/check_hot_residual_soft_relower_3107.py");
    const auto build = read_file("build.py");
    CHECK(!lint.empty() && lint.find("Issue #3107") != std::string::npos,
          "3107 AC5: 3107 linter exists");
    CHECK(build.find("check_hot_residual_soft_relower_3107") != std::string::npos,
          "3107 AC5: build.py wires 3107 linter");
    CHECK(read_file("tests/compiler/test_issue_3107.cpp").empty(),
          "3107 AC5: no invent test_issue_3107 (per #81967)");
    CHECK(read_file("docs/design/3107-hot-residual-soft-relower.md").empty(),
          "3107 AC5: no docs/design/ (per #1655)");
    // Lineage: 3046 + 3084 must still pass (counter names preserved).
    CHECK(pol.find("kCastOpHotResidualNonidentityIssue = 3046") != std::string::npos,
          "3107 AC5: #3046 lineage preserved");
    CHECK(pol.find("kCastOpHotResidualSoftMustDeoptIssue = 3084") != std::string::npos,
          "3107 AC5: #3084 lineage preserved");
}

} // namespace

// ── Issue #3140 Phase C: JIT deopt on missing / aging typed-meta ────────
// under Production only. Soft observe unchanged (AC2); Quiet epoch-match
// zero-cost (AC3); additive counter only (AC4); source-cite castop_typed_meta.h
// + castop_density_policy.hh (AC5).

static void ac3140_1_production_missing_meta_deopt() {
    std::println("\n--- #3140 AC1: Production + missing typed-meta → deopt ---");
    clear_castop_typed_meta_for_test();
    reset_castop_typed_meta_phase_c_for_test();
    set_castop_typed_meta_phase_c_wired_for_test(true);
    const auto before = castop_typed_meta_phase_c_deopt_total.load(std::memory_order_relaxed);
    const bool fired = aura::compiler::castop_density::castop_typed_meta_phase_c_hot_entry_deopt(
        /*site_key=*/0xDEADBEEFu, /*current_stamp=*/42, /*fn_name=*/"hot_fn_3140_miss",
        /*production_override=*/1);
    CHECK(fired, "3140 AC1: Production + missing meta → deopt fired");
    CHECK(castop_typed_meta_phase_c_deopt_total.load(std::memory_order_relaxed) == before + 1,
          "3140 AC1: counter bumps once on deopt");
    const auto pol = read_file("src/compiler/castop_density_policy.hh");
    CHECK(pol.find("Issue #3140 Phase C") != std::string::npos,
          "3140 AC1: policy cites #3140 Phase C");
    CHECK(pol.find("castop_typed_meta_phase_c_hot_entry_deopt") != std::string::npos,
          "3140 AC1: hot entry helper present");
    CHECK(pol.find("aura_jit_batch_deopt_for(fn_name, current_stamp)") != std::string::npos,
          "3140 AC1: aura_jit_batch_deopt_for wired as force-relower");
    const auto meta = read_file("src/compiler/castop_typed_meta.h");
    CHECK(meta.find("castop_typed_meta_phase_c_deopt_total") != std::string::npos,
          "3140 AC1: counter in castop_typed_meta.h");
    CHECK(meta.find("castop_typed_meta_phase_c_lags") != std::string::npos,
          "3140 AC1: lag helper in castop_typed_meta.h");
    CHECK(meta.find("epoch_or_mid") != std::string::npos, "3140 AC1: epoch_or_mid field");
    CHECK(meta.find("kCastOpTypedMetaPhaseCIssue = 3140") != std::string::npos,
          "3140 AC1: issue stamp");
    const auto site = make_site_key(1, 2, 3);
    stamp_castop_typed_meta(site, 11, 22, 33, 44); // no epoch
    const auto m = lookup_castop_typed_meta(site);
    CHECK(m.has_value(), "3140 AC1: legacy stamp overload still inserts");
    CHECK(m->epoch_or_mid == 0, "3140 AC1: legacy stamp leaves epoch_or_mid=0");
}

static void ac3140_2_production_epoch_lag_deopt() {
    std::println("\n--- #3140 AC2: Production + epoch lag → deopt ---");
    clear_castop_typed_meta_for_test();
    reset_castop_typed_meta_phase_c_for_test();
    set_castop_typed_meta_phase_c_wired_for_test(true);
    const auto site = make_site_key(7, 8, 9);
    stamp_castop_typed_meta(site, 1, 2, 3, 4, /*epoch_or_mid=*/10);
    const auto before = castop_typed_meta_phase_c_deopt_total.load(std::memory_order_relaxed);
    const bool fired = aura::compiler::castop_density::castop_typed_meta_phase_c_hot_entry_deopt(
        site, /*current_stamp=*/20, /*fn_name=*/"hot_fn_3140_lag", /*production_override=*/1);
    CHECK(fired, "3140 AC2: Production + epoch_lag → deopt fired");
    CHECK(castop_typed_meta_phase_c_deopt_total.load(std::memory_order_relaxed) == before + 1,
          "3140 AC2: counter bumps on lag");
    stamp_castop_typed_meta(site, 1, 2, 3, 4, /*epoch_or_mid=*/20);
    const auto quiet_before = castop_typed_meta_phase_c_deopt_total.load(std::memory_order_relaxed);
    const bool quiet_fired =
        aura::compiler::castop_density::castop_typed_meta_phase_c_hot_entry_deopt(
            site, /*current_stamp=*/20, /*fn_name=*/"hot_fn_3140_quiet",
            /*production_override=*/1);
    CHECK(!quiet_fired, "3140 AC2: epoch==current → no deopt (AC3 Quiet)");
    CHECK(castop_typed_meta_phase_c_deopt_total.load(std::memory_order_relaxed) == quiet_before,
          "3140 AC2: Quiet no counter bump");
}

static void ac3140_3_soft_path_zero_cost() {
    std::println("\n--- #3140 AC3: Soft path unchanged ---");
    clear_castop_typed_meta_for_test();
    reset_castop_typed_meta_phase_c_for_test();
    set_castop_typed_meta_phase_c_wired_for_test(true);
    const auto before = castop_typed_meta_phase_c_deopt_total.load(std::memory_order_relaxed);
    const auto fired = aura::compiler::castop_density::castop_typed_meta_phase_c_hot_entry_deopt(
        /*site_key=*/0xCAFEBABEu, /*current_stamp=*/99, /*fn_name=*/"hot_fn_3140_soft",
        /*production_override=*/0);
    CHECK(!fired, "3140 AC3: Soft path → no deopt fired (AC2)");
    CHECK(castop_typed_meta_phase_c_deopt_total.load(std::memory_order_relaxed) == before,
          "3140 AC3: Soft path → zero counter bump");
    const auto before_missing = aura::compiler::castop_meta::castop_typed_meta_missing_total.load(
        std::memory_order_relaxed);
    (void)lookup_castop_typed_meta(0xCAFEBABEu);
    const auto after_missing = aura::compiler::castop_meta::castop_typed_meta_missing_total.load(
        std::memory_order_relaxed);
    CHECK(after_missing == before_missing + 1,
          "3140 AC3: Soft missing_total++ still on lookup_castop_typed_meta");
}

static void ac3140_4_quiet_epoch_match_zero_extra() {
    std::println("\n--- #3140 AC4: Quiet epoch-match → zero extra atomics ---");
    clear_castop_typed_meta_for_test();
    reset_castop_typed_meta_phase_c_for_test();
    set_castop_typed_meta_phase_c_wired_for_test(true);
    const auto site = make_site_key(2, 4, 6);
    stamp_castop_typed_meta(site, 1, 2, 3, 4, /*epoch_or_mid=*/100);
    const auto before_zero = castop_typed_meta_phase_c_deopt_total.load(std::memory_order_relaxed);
    const bool fired_zero =
        aura::compiler::castop_density::castop_typed_meta_phase_c_hot_entry_deopt(
            site, /*current_stamp=*/0, /*fn_name=*/"hot_fn_3140_zero", /*production_override=*/1);
    CHECK(!fired_zero, "3140 AC4: current_stamp==0 → Quiet (AC3)");
    CHECK(castop_typed_meta_phase_c_deopt_total.load(std::memory_order_relaxed) == before_zero,
          "3140 AC4: zero current_stamp → no counter bump");
    set_castop_typed_meta_phase_c_wired_for_test(false);
    const auto before_off = castop_typed_meta_phase_c_deopt_total.load(std::memory_order_relaxed);
    const bool fired_off =
        aura::compiler::castop_density::castop_typed_meta_phase_c_hot_entry_deopt(
            site, /*current_stamp=*/200, /*fn_name=*/"hot_fn_3140_off", /*production_override=*/1);
    CHECK(!fired_off, "3140 AC4: wired=0 → no deopt");
    CHECK(castop_typed_meta_phase_c_deopt_total.load(std::memory_order_relaxed) == before_off,
          "3140 AC4: wired=0 → no counter bump");
    set_castop_typed_meta_phase_c_wired_for_test(true);
    clear_castop_typed_meta_for_test();
    reset_castop_typed_meta_phase_c_for_test();
}

static void ac3140_5_additive_counter_and_source_cite() {
    std::println("\n--- #3140 AC5: additive counter + source-cite + linter ---");
    CHECK(kCastOpTypedMetaPhaseCIssue == 3140, "3140 AC5: issue stamp constant");
    CHECK(castop_typed_meta_phase_c_wired.load(std::memory_order_relaxed) == 1,
          "3140 AC5: wired flag default 1");
    const auto pol = read_file("src/compiler/castop_density_policy.hh");
    CHECK(pol.find("Issue #3140 Phase C") != std::string::npos, "3140 AC5: policy cite");
    CHECK(pol.find("castop_typed_meta_phase_c_deopt_total") != std::string::npos,
          "3140 AC5: counter referenced in policy");
    CHECK(pol.find("production_path_enabled") != std::string::npos,
          "3140 AC5: gate uses production_path_enabled");
    const auto meta = read_file("src/compiler/castop_typed_meta.h");
    CHECK(meta.find("Issue #3140 Phase C") != std::string::npos, "3140 AC5: meta cite");
    CHECK(meta.find("epoch_or_mid < current_stamp") != std::string::npos,
          "3140 AC5: lag comparator present");
    CHECK(meta.find("AC1: missing meta") != std::string::npos, "3140 AC5: missing → lag doc");
    CHECK(meta.find("AC3 Quiet") != std::string::npos, "3140 AC5: Quiet doc");
    const auto low = read_file("src/compiler/lowering_impl.cpp");
    CHECK(low.find("last_type_linear_commit_proof_stamp_v_read") != std::string::npos,
          "3140 AC5: lowering plumbs current stamp");
    CHECK(low.find("Issue #3140 Phase C") != std::string::npos, "3140 AC5: lowering cite");
    CHECK(read_file("scripts/coverage/checks/check_castop_typed_meta_phase_c_3140.py").size() > 0,
          "3140 AC5: linter exists");
    CHECK(read_file("build.py").find("check_castop_typed_meta_phase_c_3140.py") !=
              std::string::npos,
          "3140 AC5: build.py wires 3140 linter");
    CHECK(read_file("scripts/coverage/manifests/3140.json").size() > 0,
          "3140 AC5: manifest 3140.json exists");
    CHECK(!std::filesystem::exists("docs/design/3140-castop-typed-meta-phase-c.md"),
          "3140 AC5: no docs/design/3140-*.md");
    CHECK(!std::filesystem::exists("tests/issues/test_issue_3140.cpp"),
          "3140 AC5: no tests/issues/test_issue_3140.cpp");
}

int run_test_castop_density_hard() {
    std::println("=== Issue #2358: CastOp density HARD force-JIT policy ===");
    ac5_query_and_source();
    ac1_hard_off_soft_only();
    ac4_under_budget_zero_extra();
    ac2_hard_on_force_jit();
    ac3_mutate_still_succeeds();
    ac3046_residual_nonidentity();
    ac3084_1_soft_residual_must_deopt();
    ac3084_2_production_relower_unchanged();
    ac3084_3_quiet_zero();
    ac3084_4_blame_default_unchanged();
    ac3084_5_schema_and_linter();
    ac3107_1_soft_residual_force_relower();
    ac3107_2_hot_fn_force_relower();
    ac3107_3_quiet_zero_cost();
    ac3107_4_additive_counter_only();
    ac3107_5_schema_and_linter();
    ac3140_1_production_missing_meta_deopt();
    ac3140_2_production_epoch_lag_deopt();
    ac3140_3_soft_path_zero_cost();
    ac3140_4_quiet_epoch_match_zero_extra();
    ac3140_5_additive_counter_and_source_cite();
    std::println("\n=== #2358/#3046/#3084/#3107/#3140: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_castop_density_hard();
}
#endif
