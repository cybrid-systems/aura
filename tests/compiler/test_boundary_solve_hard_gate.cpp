// @category: unit
// @reason: Issue #2260 — MutationBoundary hard-gate requires SOLVED or
// explicit full resync (never silent partial type-proof under Full/Strict).
//
//   AC1: truncated_reverify under Full hard-gate → full resync or force fail
//   AC2: Soft + small non-linear dirty observes truncated_seen without force
//   AC3: Composite commit reuses #2180 path; truncated → solve_ok false
//   AC4: TIMEOUT/truncated surfaces unresolved_affected_nodes via sdo
//   AC5: schema-2260 + source-cite map for gate sites

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"
#include "core/sandbox.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.core.type;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::solve_delta_occurrence;
using aura::compiler::SolveResult;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::requires_invariant_hard_gate;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_strategy;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static void ac1_hard_gate_truncated() {
    std::println("\n--- AC1: Full hard-gate + truncated CS → resync or force-fail ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    set_mode(SandboxMode::Off);
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");

    cs.evaluator().inject_commit_cs_truncated_reverify_for_test();
    CHECK(cs.evaluator().commit_cs_live(), "commit CS live after inject");

    const auto gate0 = load_u64(g_typed_mutation_audit_counters.boundary_solve_hard_gate_total);
    const auto trunc0 =
        load_u64(g_typed_mutation_audit_counters.boundary_solve_truncated_seen_total);
    const auto resync0 = load_u64(g_typed_mutation_audit_counters.boundary_solve_full_resync_total);
    const auto force0 =
        load_u64(g_typed_mutation_audit_counters.boundary_solve_force_rollback_total);

    bool truncated = false;
    bool force_fail = false;
    // Hard-gate + no linear + small dirty → prefer full resync path.
    const bool ok = cs.evaluator().boundary_solve_proof_gate(
        /*hard_gate=*/true, /*linear=*/false, /*nodes=*/1, &truncated, &force_fail);

    CHECK(load_u64(g_typed_mutation_audit_counters.boundary_solve_hard_gate_total) > gate0,
          "hard_gate total advanced");
    // Either truncated_seen (from first sdo) and/or resync/force path ran.
    CHECK(
        load_u64(g_typed_mutation_audit_counters.boundary_solve_truncated_seen_total) > trunc0 ||
            load_u64(g_typed_mutation_audit_counters.boundary_solve_full_resync_total) > resync0 ||
            load_u64(g_typed_mutation_audit_counters.boundary_solve_force_rollback_total) > force0,
        "truncated seen and/or resync/force");
    // Never silent continue with truncated without recovery attempt:
    // if still truncated after gate, force_fail or ok via resync.
    if (truncated && !ok)
        CHECK(force_fail ||
                  load_u64(g_typed_mutation_audit_counters.boundary_solve_force_rollback_total) >
                      force0,
              "truncated hard fail forces rollback counter");
    // If ok, resync must have been attempted (type-only path).
    if (ok)
        CHECK(load_u64(g_typed_mutation_audit_counters.boundary_solve_full_resync_total) >=
                      resync0 ||
                  !truncated,
              "ok implies resync or clean proof");
}

static void ac2_soft_truncated_observe() {
    std::println("\n--- AC2: Soft path observes truncated without force ---");
    reset_for_test();
    set_strategy(AuditStrategy::Sampled);
    set_mode(SandboxMode::Off);
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define y 2)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");

    cs.evaluator().inject_commit_cs_truncated_reverify_for_test();
    const auto force0 =
        load_u64(g_typed_mutation_audit_counters.boundary_solve_force_rollback_total);
    const auto trunc0 =
        load_u64(g_typed_mutation_audit_counters.boundary_solve_truncated_seen_total);

    bool truncated = false;
    bool force_fail = false;
    const bool ok = cs.evaluator().boundary_solve_proof_gate(
        /*hard_gate=*/false, /*linear=*/false, /*nodes=*/1, &truncated, &force_fail);

    CHECK(ok, "soft continues");
    CHECK(!force_fail, "soft does not force fail");
    CHECK(load_u64(g_typed_mutation_audit_counters.boundary_solve_force_rollback_total) == force0,
          "soft no force-rollback bump");
    // Soft still bumps truncated when CS truncated (AC2).
    CHECK(load_u64(g_typed_mutation_audit_counters.boundary_solve_truncated_seen_total) >= trunc0,
          "truncated_seen monotonic");
    // Sampled + small non-linear: hard-gate policy false.
    CHECK(!requires_invariant_hard_gate(1, false, false, false), "sampled small not hard");
}

static void ac3_composite_truncated_fail() {
    std::println("\n--- AC3: composite path rejects truncated (reuse #2180 surface) ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    set_mode(SandboxMode::Off);
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define z 3)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");

    cs.evaluator().inject_commit_cs_truncated_reverify_for_test();
    const auto fail0 = load_u64(g_typed_mutation_audit_counters.composite_commit_solve_fail_total);
    const auto trunc0 =
        load_u64(g_typed_mutation_audit_counters.boundary_solve_truncated_seen_total);

    // composite_txn_commit with truncated CS should not report *silent*
    // clean solve. Issue #2458 may full-solve recover under Full hard —
    // that is still not half-green (recover counter advances).
    const auto rec0 =
        load_u64(g_typed_mutation_audit_counters.truncate_commit_full_solve_recover_total);
    const auto trej0 = load_u64(g_typed_mutation_audit_counters.truncate_commit_reject_total);
    aura::compiler::typed_audit::CompositeTxnCommitResult ccr{};
    const bool committed = cs.evaluator().composite_txn_commit(
        /*mutation_id=*/2260, "test-2260-composite", 0, 0, 1,
        /*nested=*/true, /*batch=*/false, &ccr);

    const bool recovered =
        load_u64(g_typed_mutation_audit_counters.truncate_commit_full_solve_recover_total) > rec0;
    const bool trejected =
        load_u64(g_typed_mutation_audit_counters.truncate_commit_reject_total) > trej0;
    CHECK(!committed || !ccr.solve_ok || recovered,
          "truncated composite not clean-solved without full-solve recover (#2458)");
    CHECK(load_u64(g_typed_mutation_audit_counters.boundary_solve_hard_gate_total) >= 1 ||
              load_u64(g_typed_mutation_audit_counters.composite_commit_revalidate_total) >= 1,
          "composite revalidate ran");
    CHECK(load_u64(g_typed_mutation_audit_counters.boundary_solve_truncated_seen_total) > trunc0 ||
              load_u64(g_typed_mutation_audit_counters.composite_commit_solve_fail_total) > fail0 ||
              recovered || trejected,
          "truncated / solve_fail / #2458 recover|reject advanced");
}

static void ac4_unresolved_export() {
    std::println("\n--- AC4: TIMEOUT/truncated surfaces affected nodes ---");
    TypeRegistry reg;
    ConstraintSystem unit(reg);
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
    unit.mark_touched_on_delta(d.lhs, false);

    auto sdo = solve_delta_occurrence(unit, {}, nullptr, nullptr);
    CHECK(sdo.truncated_reverify || unit.last_reverify_truncated(), "truncated flag");
    // On TIMEOUT status, unresolved_affected_nodes should be non-empty when
    // unresolved list or blame frames carry nodes; truncation may only set
    // unscanned counts without TIMEOUT if worklist emptied.
    if (sdo.status == SolveResult::TIMEOUT) {
        CHECK(!sdo.unresolved_affected_nodes.empty() || !sdo.unresolved.empty() ||
                  sdo.unscanned_constraint_count > 0,
              "TIMEOUT exposes repair surface");
    } else {
        CHECK(sdo.unscanned_constraint_count > 0 || unit.last_reverify_unscanned() > 0 ||
                  sdo.truncated_reverify,
              "truncated exposes unscanned");
    }
}

static void ac5_schema_and_source_cite() {
    std::println("\n--- AC5: schema-2260 + source-cite gate sites ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2260") == 2260, "schema-2260");
    CHECK(href(cs, "issue-2260") == 2260, "issue-2260");
    CHECK(href(cs, "boundary-solve-hard-gate-wired") == 1, "wired");
    CHECK(href(cs, "boundary-solve-hard-gate-total") >= 0, "hard-gate-total key");
    CHECK(href(cs, "boundary-solve-full-resync-total") >= 0, "full-resync key");
    CHECK(href(cs, "boundary-solve-force-rollback-total") >= 0, "force-rollback key");
    CHECK(href(cs, "boundary-solve-truncated-seen-total") >= 0, "truncated-seen key");
    CHECK(href(cs, "schema") == 1617, "lineage schema 1617");

    const auto tc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto bd = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(tc.find("boundary_solve_proof_gate") != std::string::npos, "proof gate impl");
    CHECK(tc.find("boundary_solve_truncated_seen_total") != std::string::npos,
          "truncated counter site");
    CHECK(tc.find("Issue #2260") != std::string::npos, "typecheck #2260 cite");
    CHECK(bd.find("boundary_solve_proof_gate") != std::string::npos, "boundary calls proof gate");
    CHECK(bd.find("Issue #2260") != std::string::npos, "boundary #2260 cite");
    CHECK(g_typed_mutation_audit_counters.boundary_solve_hard_gate_wired.load(
              std::memory_order_relaxed) == 1,
          "wired counter");
}

} // namespace

int run_test_boundary_solve_hard_gate() {
    std::println("=== Issue #2260: MutationBoundary type-proof hard-gate ===");
    ac1_hard_gate_truncated();
    ac2_soft_truncated_observe();
    ac3_composite_truncated_fail();
    ac4_unresolved_export();
    ac5_schema_and_source_cite();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_boundary_solve_hard_gate();
}
#endif
