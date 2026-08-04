// @category: unit
// @reason: Issue #2458 — production commit gate on truncated reverify +
//          incomplete blame (anti half-green under Soft vs HARD).
//
//   AC1: Soft Sampled + truncated → observe; commit_ok allows; no reject
//   AC2: Full/production HARD + truncated → full-solve attempt; reject or recover
//   AC3: incomplete non-vacuous blame under HARD same policy surface
//   AC4: happy path SOLVED !truncated complete → zero full-solve (no recover/reject)
//   AC5: schema-2458 + source-cite commit_ok_after_delta_snapshot / composite

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"
#include "core/sandbox.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.core.type;

namespace {

using aura::compiler::commit_ok_after_delta_snapshot;
using aura::compiler::CompilerService;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::solve_delta_occurrence;
using aura::compiler::SolveDeltaOccurrenceResult;
using aura::compiler::SolveResult;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_strategy;
using aura::compiler::typed_audit::truncate_commit_hard_enabled;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
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

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static void make_truncated_cs(ConstraintSystem& unit) {
    unit.force_reverify_limit_for_test(8);
    auto shared = unit.fresh_var();
    unit.mark_touched_on_delta(shared, false);
    for (int i = 0; i < 40; ++i) {
        auto o = unit.fresh_var();
        Constraint c;
        c.kind = Constraint::EQUAL;
        c.lhs = shared;
        c.rhs = o;
        c.affected_node = static_cast<std::uint32_t>(100 + i);
        unit.add(c);
    }
    Constraint d;
    d.kind = Constraint::EQUAL;
    d.lhs = unit.fresh_var();
    d.rhs = unit.fresh_var();
    d.source_mutation_id = 1;
    d.affected_node = 999;
    unit.add_delta(d);
}

static void ac1_soft_observe() {
    std::println("\n--- #2458 AC1: Soft truncated → observe, allow ---");
    reset_for_test();
    set_strategy(AuditStrategy::Sampled);
    set_mode(SandboxMode::Off);
    CHECK(!truncate_commit_hard_enabled(), "AC1: Soft hard-gate off");

    TypeRegistry reg;
    ConstraintSystem unit(reg);
    make_truncated_cs(unit);
    auto sdo = solve_delta_occurrence(unit, {}, nullptr, nullptr);
    CHECK(sdo.truncated_reverify || unit.last_reverify_truncated(), "AC1: truncated after sdo");

    const auto obs0 = load_u64(g_typed_mutation_audit_counters.truncate_commit_observe_total);
    const auto rej0 = load_u64(g_typed_mutation_audit_counters.truncate_commit_reject_total);
    const auto rec0 =
        load_u64(g_typed_mutation_audit_counters.truncate_commit_full_solve_recover_total);

    auto gate = commit_ok_after_delta_snapshot(unit, &sdo, nullptr);
    CHECK(gate.allow, "AC1: Soft allows commit");
    CHECK(gate.observed, "AC1: Soft observes");
    CHECK(!gate.rejected, "AC1: Soft no reject");
    CHECK(!gate.attempted_full_solve, "AC1: Soft no full solve");
    CHECK(load_u64(g_typed_mutation_audit_counters.truncate_commit_observe_total) > obs0,
          "AC1: observe counter bumped");
    CHECK(load_u64(g_typed_mutation_audit_counters.truncate_commit_reject_total) == rej0,
          "AC1: reject unchanged");
    CHECK(load_u64(g_typed_mutation_audit_counters.truncate_commit_full_solve_recover_total) ==
              rec0,
          "AC1: recover unchanged");
}

static void ac2_hard_full_solve() {
    std::println("\n--- #2458 AC2: HARD truncated → full-solve attempt ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    set_mode(SandboxMode::Off);
    CHECK(truncate_commit_hard_enabled(), "AC2: Full enables hard");

    TypeRegistry reg;
    ConstraintSystem unit(reg);
    make_truncated_cs(unit);
    auto sdo = solve_delta_occurrence(unit, {}, nullptr, nullptr);
    CHECK(sdo.truncated_reverify || unit.last_reverify_truncated(), "AC2: truncated");

    const auto rec0 =
        load_u64(g_typed_mutation_audit_counters.truncate_commit_full_solve_recover_total);
    const auto rej0 = load_u64(g_typed_mutation_audit_counters.truncate_commit_reject_total);

    auto gate = commit_ok_after_delta_snapshot(unit, &sdo, nullptr);
    CHECK(gate.attempted_full_solve, "AC2: full solve attempted");
    // Either recover (rare complete blame) or reject — never silent allow without attempt.
    CHECK(gate.recovered || gate.rejected, "AC2: recover or reject after hard");
    if (gate.rejected)
        CHECK(!gate.allow, "AC2: reject denies");
    if (gate.recovered)
        CHECK(gate.allow, "AC2: recover allows");
    CHECK(load_u64(g_typed_mutation_audit_counters.truncate_commit_full_solve_recover_total) >
                  rec0 ||
              load_u64(g_typed_mutation_audit_counters.truncate_commit_reject_total) > rej0,
          "AC2: recover or reject counter advanced");
}

static void ac3_hard_incomplete_blame() {
    std::println("\n--- #2458 AC3: incomplete non-vacuous blame under HARD ---");
    reset_for_test();
    apply_production_audit_defaults();
    set_mode(SandboxMode::Off);
    CHECK(truncate_commit_hard_enabled(), "AC3: production hard on");

    TypeRegistry reg;
    ConstraintSystem unit(reg);
    // Force truncated partial blame (non-vacuous incomplete).
    make_truncated_cs(unit);
    auto sdo = solve_delta_occurrence(unit, {}, nullptr, nullptr);
    const auto& blame = unit.last_blame_chain();
    const bool incomplete = !blame.is_complete() &&
                            (!blame.frames.empty() || blame.partial || blame.truncated_reverify);
    CHECK(incomplete || sdo.truncated_reverify, "AC3: incomplete or truncated surface");

    auto gate = commit_ok_after_delta_snapshot(unit, &sdo, nullptr);
    CHECK(gate.attempted_full_solve, "AC3: full solve under HARD");
    CHECK(gate.rejected || gate.recovered, "AC3: reject or recover (no silent half-green)");
}

static void ac4_happy_path() {
    std::println("\n--- #2458 AC4: happy path — no extra full solve ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    set_mode(SandboxMode::Off);

    TypeRegistry reg;
    ConstraintSystem unit(reg);
    auto a = unit.fresh_var();
    auto b = unit.fresh_var();
    Constraint c;
    c.kind = Constraint::EQUAL;
    c.lhs = a;
    c.rhs = b;
    unit.add_delta(c);
    auto sdo = solve_delta_occurrence(unit, {}, nullptr, nullptr);
    CHECK(sdo.status == SolveResult::SOLVED, "AC4: SOLVED");
    CHECK(!sdo.truncated_reverify && !unit.last_reverify_truncated(), "AC4: not truncated");

    const auto rec0 =
        load_u64(g_typed_mutation_audit_counters.truncate_commit_full_solve_recover_total);
    const auto rej0 = load_u64(g_typed_mutation_audit_counters.truncate_commit_reject_total);
    const auto obs0 = load_u64(g_typed_mutation_audit_counters.truncate_commit_observe_total);

    auto gate = commit_ok_after_delta_snapshot(unit, &sdo, nullptr);
    CHECK(gate.allow, "AC4: allow");
    CHECK(!gate.attempted_full_solve, "AC4: no full solve");
    CHECK(!gate.observed && !gate.rejected && !gate.recovered, "AC4: quiet happy path");
    CHECK(load_u64(g_typed_mutation_audit_counters.truncate_commit_full_solve_recover_total) ==
              rec0,
          "AC4: recover unchanged");
    CHECK(load_u64(g_typed_mutation_audit_counters.truncate_commit_reject_total) == rej0,
          "AC4: reject unchanged");
    CHECK(load_u64(g_typed_mutation_audit_counters.truncate_commit_observe_total) == obs0,
          "AC4: observe unchanged");
}

static void ac5_source_and_schema() {
    std::println("\n--- #2458 AC5: schema + source cite ---");
    auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    auto ixx = read_file("src/compiler/type_checker.ixx");
    auto impl = read_file("src/compiler/type_checker_impl.cpp");
    auto aud = read_file("src/compiler/typed_mutation_audit.h");
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(etc.find("Issue #2458") != std::string::npos, "AC5: typecheck cites #2458");
    CHECK(etc.find("commit_ok_after_delta_snapshot") != std::string::npos,
          "AC5: composite/boundary gate wired");
    CHECK(ixx.find("commit_ok_after_delta_snapshot") != std::string::npos, "AC5: export helper");
    CHECK(impl.find("commit_ok_after_delta_snapshot") != std::string::npos, "AC5: impl helper");
    CHECK(aud.find("truncate_commit_observe_total") != std::string::npos, "AC5: counters");
    CHECK(aud.find("AURA_TRUNCATE_COMMIT_HARD") != std::string::npos, "AC5: env HARD");
    CHECK(q.find("schema-2458") != std::string::npos, "AC5: schema-2458");
    CHECK(q.find("truncate-commit-observe-total") != std::string::npos, "AC5: query observe");
    CHECK(q.find("truncate-commit-reject-total") != std::string::npos, "AC5: query reject");
    CHECK(q.find("truncate-commit-full-solve-recover-total") != std::string::npos,
          "AC5: query recover");
    // Lineage keys retained
    CHECK(q.find("schema-2308") != std::string::npos, "AC5: #2308 intact");
    CHECK(q.find("schema-2277") != std::string::npos, "AC5: #2277 intact");
}

} // namespace

int run_test_truncate_commit_gate_2458() {
    std::println("=== Issue #2458: truncate-commit gate ===");
    ac1_soft_observe();
    ac2_hard_full_solve();
    ac3_hard_incomplete_blame();
    ac4_happy_path();
    ac5_source_and_schema();
    std::println("\n=== #2458 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_truncate_commit_gate_2458();
}
#endif
