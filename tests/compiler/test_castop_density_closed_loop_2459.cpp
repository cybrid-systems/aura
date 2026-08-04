// @category: unit
// @reason: Issue #2459 — production CastOp density closed-loop (streak +
//          MutateTypeGate pressure after force-JIT).
//
//   AC1: Soft path — no gate reject; optional force-JIT only under HARD
//   AC2: production/HARD + N consecutive unannotated over-budget → gate reject
//   AC3: under budget → streak resets, zero gate fire
//   AC4: first over-budget under production → force-JIT, no reject yet
//   AC5: schema-2459 + lineage #2287/#2319/#2358 + source-cite

#include "test_harness.hpp"

#include "compiler/castop_density_policy.hh"
#include "compiler/mutate_type_gate.hh"
#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"

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
using aura::compiler::castop_density::apply_density_closed_loop;
using aura::compiler::castop_density::apply_hard_policy;
using aura::compiler::castop_density::density_streak;
using aura::compiler::castop_density::gate_reject_total;
using aura::compiler::castop_density::reset_streak_for_test;
using aura::compiler::mutate_type_gate::MutateTypeGate;
using aura::compiler::mutate_type_gate::set_mode;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::reset_for_test;
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

static void seed_unannotated(CompilerMetrics& m) {
    m.coercion_castop_emitted_total.store(100, std::memory_order_relaxed);
    m.dead_coercion_elim_total.store(1, std::memory_order_relaxed);
}

// ── AC1: Soft — no gate reject ──
static void ac1_soft_no_gate_reject() {
    std::println("\n--- #2459 AC1: Soft path no MutateTypeGate reject ---");
    reset_for_test();
    apply_dev_audit_defaults();
    set_mode(MutateTypeGate::Soft);
    reset_streak_for_test();

    CompilerMetrics m;
    seed_unannotated(m);
    const auto rej0 = m.castop_density_gate_reject_total.load();
    const auto act0 = m.castop_density_hard_action_total.load();

    // Soft: hard_override=0, production_override=0
    auto r = apply_density_closed_loop(m, /*dens=*/5000, /*budget=*/1500, /*unann=*/true,
                                       /*hard=*/0, /*production=*/0);
    CHECK(!r.force_jit, "AC1: soft no force-JIT");
    CHECK(!r.gate_reject, "AC1: soft no gate reject");
    CHECK(m.castop_density_hard_action_total.load() == act0, "AC1: action unchanged");
    CHECK(m.castop_density_gate_reject_total.load() == rej0, "AC1: gate_reject unchanged");
    CHECK(density_streak() == 0, "AC1: streak stays 0 under soft");
}

// ── AC4 then AC2: first force-JIT, second gate reject ──
static void ac4_then_ac2_streak_gate() {
    std::println("\n--- #2459 AC4+AC2: first force-JIT, streak→reject ---");
    reset_for_test();
    apply_production_audit_defaults();
    set_mode(MutateTypeGate::Hard);
    reset_streak_for_test();

    CompilerMetrics m;
    seed_unannotated(m);

    // AC4: first over-budget under production
    auto r1 = apply_density_closed_loop(m, 5000, 1500, true, /*hard=*/-1, /*production=*/1);
    CHECK(r1.force_jit, "AC4: force-JIT on first over-budget");
    CHECK(!r1.gate_reject, "AC4: no gate reject on first fire");
    CHECK(r1.streak == 1, "AC4: streak=1");
    CHECK(m.castop_density_hard_action_total.load() >= 1, "AC4: hard_action bumped");

    // AC2: second consecutive unannotated over-budget (default thr=2)
    auto r2 = apply_density_closed_loop(m, 5000, 1500, true, /*hard=*/-1, /*production=*/1);
    CHECK(r2.force_jit, "AC2: force-JIT still fires");
    CHECK(r2.streak >= 2, "AC2: streak ≥ threshold");
    CHECK(r2.gate_reject, "AC2: gate reject after streak");
    CHECK(m.castop_density_gate_reject_total.load() >= 1, "AC2: gate_reject_total ≥ 1");
}

// ── AC3: under budget resets streak ──
static void ac3_under_budget_reset() {
    std::println("\n--- #2459 AC3: under budget resets streak ---");
    reset_for_test();
    apply_production_audit_defaults();
    set_mode(MutateTypeGate::Hard);
    reset_streak_for_test();

    CompilerMetrics m;
    seed_unannotated(m);
    (void)apply_density_closed_loop(m, 5000, 1500, true, 1, 1);
    CHECK(density_streak() == 1, "AC3: streak=1 after one over-budget");

    auto r = apply_density_closed_loop(m, /*dens=*/800, /*budget=*/1500, true, 1, 1);
    CHECK(!r.force_jit, "AC3: under budget no force-JIT");
    CHECK(!r.gate_reject, "AC3: under budget no reject");
    CHECK(r.streak == 0, "AC3: streak reset to 0");
    CHECK(density_streak() == 0, "AC3: process streak 0");
    CHECK(m.castop_density_streak.load() == 0, "AC3: metrics streak 0");
}

// ── AC5: schema + source ──
static void ac5_schema_source() {
    std::println("\n--- #2459 AC5: schema + source-cite ---");
    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);
    metrics.castop_density_streak.store(2, std::memory_order_relaxed);
    metrics.castop_density_gate_reject_total.store(3, std::memory_order_relaxed);
    metrics.castop_density_production_default_wired.store(1, std::memory_order_relaxed);
    metrics.castop_density_budget_bp.store(1500, std::memory_order_relaxed);
    metrics.last_castop_density_bp.store(800, std::memory_order_relaxed);

    CHECK(href(cs, "schema-2459") == 2459, "AC5: schema-2459");
    CHECK(href(cs, "issue-2459") == 2459, "AC5: issue-2459");
    CHECK(href(cs, "castop-density-streak") == 2, "AC5: streak key");
    CHECK(href(cs, "castop-density-gate-reject-total") == 3, "AC5: gate-reject key");
    CHECK(href(cs, "castop-density-production-default-wired") == 1, "AC5: production wired");
    // Lineage
    CHECK(href(cs, "schema-2358") == 2358, "AC5: schema-2358 retained");
    CHECK(href(cs, "schema-2319") == 2319, "AC5: schema-2319 retained");
    CHECK(href(cs, "castop-annotation-hint") == 0, "AC5: soft hint key present");

    auto pol = read_file("src/compiler/castop_density_policy.hh");
    auto sd = read_file("src/compiler/service_dirty.cpp");
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(pol.find("Issue #2459") != std::string::npos, "AC5: policy cites #2459");
    CHECK(pol.find("apply_density_closed_loop") != std::string::npos, "AC5: closed-loop helper");
    CHECK(pol.find("AURA_CASTOP_DENSITY_STREAK_GATE") != std::string::npos, "AC5: streak env");
    CHECK(sd.find("Issue #2319 / #2358 / #2459") != std::string::npos ||
              sd.find("#2459") != std::string::npos,
          "AC5: service_dirty cites #2459");
    CHECK(q.find("schema-2459") != std::string::npos, "AC5: query schema");
    CHECK(q.find("castop-density-streak") != std::string::npos, "AC5: query streak");

    // Backward-compat: apply_hard_policy still works (#2358)
    reset_streak_for_test();
    CompilerMetrics m2;
    seed_unannotated(m2);
    CHECK(apply_hard_policy(m2, 5000, 1500, /*hard_override=*/1),
          "AC5: apply_hard_policy still fires");
}

} // namespace

int run_test_castop_density_closed_loop_2459() {
    std::println("=== Issue #2459: CastOp density production closed-loop ===");
    ac1_soft_no_gate_reject();
    ac4_then_ac2_streak_gate();
    ac3_under_budget_reset();
    ac5_schema_source();
    std::println("\n=== #2459 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_castop_density_closed_loop_2459();
}
#endif
