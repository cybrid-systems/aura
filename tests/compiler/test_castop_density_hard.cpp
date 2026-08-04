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
using aura::compiler::castop_density::hard_env_enabled;
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
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
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

} // namespace

int run_test_castop_density_hard() {
    std::println("=== Issue #2358: CastOp density HARD force-JIT policy ===");
    ac5_query_and_source();
    ac1_hard_off_soft_only();
    ac4_under_budget_zero_extra();
    ac2_hard_on_force_jit();
    ac3_mutate_still_succeeds();
    std::println("\n=== #2358: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_castop_density_hard();
}
#endif
