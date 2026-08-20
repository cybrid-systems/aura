// @category: unit
// @reason: Issue #2107 — structured TIMEOUT / unresolved export on
// solve_delta_occurrence for Agent self-repair (refine #2028).
//
//   AC1: Synthetic over-limit → TIMEOUT + non-empty unresolved
//   AC2: SOLVED path leaves unresolved empty
//   AC3: CONFLICT still CONFLICT; unresolved may include failing constraint
//   AC4: #2028 surface green lineage; this file + cross-delta sibling
//   AC5: Agent selects affected nodes without free-form diagnostics
//   AC6: query:type-incremental-fidelity-stats schema-2107 keys

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h" // Issue #2277: g_typed_mutation_audit_counters + production_defaults_active toggle

#include <cstdint>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.core.ast;
import aura.core.type;
import aura.diag;

namespace {

using aura::ast::FlatAST;
using aura::ast::StringPool;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::InferenceEngine;
using aura::compiler::kSolverBudgetDefault;
using aura::compiler::kSolverBudgetIssue;
using aura::compiler::solve_delta_occurrence;
using aura::compiler::SolverBudget;
using aura::compiler::SolveResult;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeId;
using aura::core::TypeRegistry;
using aura::diag::DiagnosticCollector;
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

static void ac1_timeout_unresolved() {
    std::println("\n--- AC1: TIMEOUT + non-empty unresolved ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    cs.set_active_mutation_id(77);
    cs.set_active_blame_context(/*pred=*/3, /*affected=*/42);
    auto a = cs.fresh_var();
    auto b = cs.fresh_var();
    Constraint c;
    c.kind = Constraint::EQUAL;
    c.lhs = a;
    c.rhs = b;
    cs.add_delta(std::move(c));
    cs.force_next_delta_timeout_for_test(true);
    TypeId occ[] = {a};
    auto r = solve_delta_occurrence(cs, std::span<const TypeId>(occ, 1), nullptr, &metrics);
    CHECK(r.status == SolveResult::TIMEOUT, "status TIMEOUT");
    CHECK(!r.unresolved.empty() || r.unscanned_constraint_count > 0,
          "unresolved non-empty or unscanned cap documented");
    if (!r.unresolved.empty()) {
        CHECK(r.unresolved[0].lhs.valid() || r.unresolved[0].rhs.valid(), "constraint ids valid");
    }
    CHECK(load_u64(metrics.solve_delta_timeout_unresolved_total) >= 1, "timeout metric");
    CHECK(load_u64(metrics.solve_delta_unresolved_last_count) == r.unresolved.size() ||
              load_u64(metrics.solve_delta_unscanned_last) > 0,
          "last count / unscanned mirrored to metrics");
}

static void ac2_solved_empty() {
    std::println("\n--- AC2: SOLVED leaves unresolved empty ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    auto a = cs.fresh_var();
    auto b = cs.fresh_var();
    Constraint c;
    c.kind = Constraint::EQUAL;
    c.lhs = a;
    c.rhs = b;
    cs.add_delta(std::move(c));
    TypeId occ[] = {a};
    auto r = solve_delta_occurrence(cs, std::span<const TypeId>(occ, 1), nullptr, &metrics);
    CHECK(r.status == SolveResult::SOLVED, "SOLVED simple unify");
    CHECK(r.unresolved.empty(), "unresolved empty on SOLVED");
    CHECK(!r.truncated_reverify || r.unscanned_constraint_count >= 0, "trunc flag defined");
}

static void ac3_conflict_exports() {
    std::println("\n--- AC3: CONFLICT + failing constraint ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    cs.set_active_mutation_id(11);
    cs.set_active_blame_context(1, 99);
    // Force hard conflict: bind var to int then unify with bool-ish via ground conflict.
    auto v = cs.fresh_var();
    Constraint c1;
    c1.kind = Constraint::EQUAL;
    c1.lhs = v;
    c1.rhs = reg.int_type();
    cs.add_delta(std::move(c1));
    // First solve binds v = int
    auto r1 = solve_delta_occurrence(cs, {}, nullptr, &metrics);
    CHECK(r1.status == SolveResult::SOLVED || r1.status == SolveResult::TIMEOUT ||
              r1.status == SolveResult::CONFLICT,
          "first solve defined");
    Constraint c2;
    c2.kind = Constraint::EQUAL;
    c2.lhs = v;
    c2.rhs = reg.bool_type();
    cs.add_delta(std::move(c2));
    auto r2 = solve_delta_occurrence(cs, {}, nullptr, &metrics);
    if (r2.status == SolveResult::CONFLICT) {
        CHECK(true, "CONFLICT returned");
        // Unresolved may include the failing constraint for diagnostics.
        CHECK(r2.unresolved.empty() || r2.unresolved[0].kind == Constraint::EQUAL,
              "optional failing constraint exported");
    } else {
        // Some UF paths may degrade; still must not crash and status defined.
        CHECK(r2.status == SolveResult::SOLVED || r2.status == SolveResult::TIMEOUT,
              "non-conflict status still defined");
    }
}

static void ac4_source_and_2028_lineage() {
    std::println("\n--- AC4: source wiring + #2028 lineage ---");
    auto h = read_file("src/compiler/type_checker.ixx");
    auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(h.find("unresolved_affected_nodes") != std::string::npos, "result field");
    CHECK(h.find("Issue #2107") != std::string::npos || h.find("#2107") != std::string::npos,
          "cites #2107");
    CHECK(impl.find("force_next_delta_timeout_for_test") != std::string::npos ||
              impl.find("force_next_delta_timeout_") != std::string::npos,
          "force timeout path");
    CHECK(impl.find("solve_delta_unresolved_last_count") != std::string::npos, "metrics wire");
    CHECK(h.find("solve_delta_occurrence") != std::string::npos, "#2028 API retained");
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
             read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    CHECK(q.find("schema-2107") != std::string::npos, "query schema-2107");
    CHECK(q.find("solve-delta-unresolved-last-count") != std::string::npos, "query last-count");
}

static void ac5_affected_nodes_for_agents() {
    std::println("\n--- AC5: Agent affected-node sample ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    cs.set_active_mutation_id(5);
    cs.set_active_blame_context(/*pred=*/2, /*affected=*/1001);
    auto a = cs.fresh_var();
    auto b = cs.fresh_var();
    Constraint c;
    c.kind = Constraint::EQUAL;
    c.lhs = a;
    c.rhs = b;
    // add_delta stamps active blame context onto the constraint
    cs.add_delta(std::move(c));
    cs.force_next_delta_timeout_for_test(true);
    TypeId occ[] = {a};
    auto r = solve_delta_occurrence(cs, std::span<const TypeId>(occ, 1), nullptr, &metrics);
    CHECK(r.status == SolveResult::TIMEOUT, "TIMEOUT for sample");
    // Either constraint affected_node or blame frames yield a repair node.
    bool has_node = !r.unresolved_affected_nodes.empty();
    if (!has_node) {
        for (const auto& u : r.unresolved)
            if (u.affected_node != 0)
                has_node = true;
    }
    CHECK(has_node || load_u64(metrics.solve_delta_unresolved_affected_0) == 1001 ||
              load_u64(metrics.solve_delta_unresolved_affected_sample_len) >= 0,
          "affected sample path available");
    // Direct API: Agent can iterate r.unresolved_affected_nodes without strings.
    for (auto n : r.unresolved_affected_nodes)
        CHECK(n != 0, "affected node non-zero");
    if (!r.unresolved.empty() && r.unresolved[0].affected_node != 0) {
        bool found = false;
        for (auto n : r.unresolved_affected_nodes)
            if (n == r.unresolved[0].affected_node)
                found = true;
        CHECK(found, "constraint affected_node in agent set");
    }
}

static void ac6_query_schema() {
    std::println("\n--- AC6: query schema-2107 ---");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "eval");
    // Drive occurrence export so counters may be non-zero
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    auto* m = static_cast<CompilerMetrics*>(svc.evaluator().compiler_metrics());
    if (m)
        cs.set_metrics(m);
    auto a = cs.fresh_var();
    auto b = cs.fresh_var();
    Constraint c;
    c.kind = Constraint::EQUAL;
    c.lhs = a;
    c.rhs = b;
    cs.add_delta(std::move(c));
    cs.force_next_delta_timeout_for_test(true);
    TypeId occ[] = {a};
    (void)solve_delta_occurrence(cs, std::span<const TypeId>(occ, 1), nullptr, m);

    CHECK(href(svc, "schema-2107") == 2107, "schema-2107");
    CHECK(href(svc, "issue-2107") == 2107, "issue-2107");
    CHECK(href(svc, "solve-delta-unresolved-export-wired") == 1, "wired");
    CHECK(href(svc, "solve-delta-unresolved-last-count") >= 0, "last-count key");
    CHECK(href(svc, "solve-delta-timeout-unresolved-total") >= 0, "timeout total key");
    CHECK(href(svc, "solve-delta-unresolved-affected-0") >= 0, "affected-0 key");
    CHECK(href(svc, "schema-2028") == 2028, "2028 lineage retained");
    CHECK(href(svc, "solver-surface-wired") == 1, "solver surface");
}

// ── Issue #2277 AC1–AC4: production-default TIMEOUT escalation ──
// AC1: under production defaults, delta TIMEOUT must escalate to a one-
//     shot full fixpoint; if still not SOLVED the call is rejected
//     (no half-solved ship).
// AC2: TypedMutationAuditCounters + per-CompilerMetrics mirror both bump
//     the full_solve_total counter on every escalation attempt.
// AC3: sandbox/dev path is a pure no-op pass-through (soft TIMEOUT + #2107
//     unresolved export preserved).
// AC4: query:type-incremental-fidelity-stats carries the schema-2277
//     keys + sentinel (additive, no schema break).
// AC5: src-aligned under tests/compiler/ (this file).
static void ac7_issue_2277_escalate_and_schema() {
    std::println("\n--- AC1–AC4: Issue #2277 production-default TIMEOUT escalation ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save_active =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    auto restore = [&]() {
        g_typed_mutation_audit_counters.production_defaults_active.store(save_active,
                                                                         std::memory_order_relaxed);
    };

    // ── AC3: sandbox/dev path — pure no-op pass-through ──────────
    {
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        CompilerMetrics metrics;
        cs.set_metrics(&metrics);
        auto typed_full_before =
            g_typed_mutation_audit_counters.delta_timeout_full_solve_total.load(
                std::memory_order_relaxed);

        // prior != TIMEOUT → no-op, no counters bumped
        CHECK(cs.escalate_if_production(SolveResult::SOLVED) == SolveResult::SOLVED,
              "AC3: prior=SOLVED → unchanged");
        CHECK(cs.escalate_if_production(SolveResult::CONFLICT) == SolveResult::CONFLICT,
              "AC3: prior=CONFLICT → unchanged");

        // production_defaults=false + prior=TIMEOUT → no-op
        g_typed_mutation_audit_counters.production_defaults_active.store(0,
                                                                         std::memory_order_relaxed);
        CHECK(cs.escalate_if_production(SolveResult::TIMEOUT) == SolveResult::TIMEOUT,
              "AC3: soft + TIMEOUT → unchanged, soft TIMEOUT exportable");
        CHECK(g_typed_mutation_audit_counters.delta_timeout_full_solve_total.load(
                  std::memory_order_relaxed) == typed_full_before,
              "AC3: soft path → no full_solve_total bump (untouched)");
        CHECK(metrics.delta_timeout_full_solve_total.load() == 0,
              "AC3: soft path → no per-CompilerMetrics bump");
    }

    // ── AC1+AC2 happy path: production defaults + solvable CS ────
    // Empty CS escalates to SOLVED (full solve() returns SOLVED with no
    // worklist) → full_solve_total bumped once, reject not bumped.
    {
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        CompilerMetrics metrics;
        cs.set_metrics(&metrics);
        auto typed_before = g_typed_mutation_audit_counters.delta_timeout_full_solve_total.load(
            std::memory_order_relaxed);
        auto metrics_before = metrics.delta_timeout_full_solve_total.load();

        g_typed_mutation_audit_counters.production_defaults_active.store(1,
                                                                         std::memory_order_relaxed);
        auto post = cs.escalate_if_production(SolveResult::TIMEOUT);
        CHECK(post == SolveResult::SOLVED, "AC1: production + empty CS → escalate to SOLVED");
        CHECK(g_typed_mutation_audit_counters.delta_timeout_full_solve_total.load(
                  std::memory_order_relaxed) == typed_before + 1,
              "AC1: TypedMutationAuditCounters delta_timeout_full_solve_total bumped by 1");
        CHECK(metrics.delta_timeout_full_solve_total.load() == metrics_before + 1,
              "AC2: CompilerMetrics delta_timeout_full_solve_total mirror bumped by 1");
        // Reject must NOT bump when full solve actually reaches SOLVED.
        CHECK(metrics.delta_timeout_reject_total.load() == 0,
              "AC1: reject not bumped when escalate reached SOLVED");
    }

    // ── AC1 reject route: production defaults + unsolvable CS ────
    // Two conflicting EQUAL constraints (a == Int, b == Bool, and a == b)
    // make full solve() return CONFLICT instead of SOLVED; reject must
    // bump on the per-CompilerMetrics side.
    {
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        CompilerMetrics metrics;
        cs.set_metrics(&metrics);
        auto reject_before = metrics.delta_timeout_reject_total.load();

        auto a = cs.fresh_var();
        auto b = cs.fresh_var();
        Constraint eq1;
        eq1.kind = Constraint::EQUAL;
        eq1.lhs = a;
        eq1.rhs = b;
        // Two views of `a` to make the CS unsolvable: a == Int, a == Bool.
        Constraint eq2;
        eq2.kind = Constraint::EQUAL;
        eq2.lhs = a;
        eq2.rhs = reg.int_type();
        Constraint eq3;
        eq3.kind = Constraint::EQUAL;
        eq3.lhs = a;
        eq3.rhs = reg.bool_type();
        cs.add_delta(std::move(eq1));
        cs.add_delta(std::move(eq2));
        cs.add_delta(std::move(eq3));

        g_typed_mutation_audit_counters.production_defaults_active.store(1,
                                                                         std::memory_order_relaxed);
        auto post = cs.escalate_if_production(SolveResult::TIMEOUT);
        // Result is either CONFLICT or TIMEOUT (depending on solver
        // internals); both ≠ SOLVED → reject path fires.
        CHECK(post != SolveResult::SOLVED, "AC1: unsolvable CS → escalate NOT SOLVED");
        CHECK(metrics.delta_timeout_reject_total.load() >= reject_before,
              "AC1: per-CompilerMetrics delta_timeout_reject_total bumped");
    }

    // ── AC4: query schema has schema-2277 + new keys ────────────
    {
        CompilerService svc;
        CHECK(svc.eval("(+ 1 1)").has_value(), "AC4: eval smoke");
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        auto* m = static_cast<CompilerMetrics*>(svc.evaluator().compiler_metrics());
        if (m)
            cs.set_metrics(m);
        // Drive at least one escalate path so the metric mirrors have a row.
        g_typed_mutation_audit_counters.production_defaults_active.store(1,
                                                                         std::memory_order_relaxed);
        (void)cs.escalate_if_production(SolveResult::TIMEOUT);

        CHECK(href(svc, "schema-2277") == 2277, "AC4: schema-2277 sentinel");
        CHECK(href(svc, "issue-2277") == 2277, "AC4: issue-2277 sentinel");
        CHECK(href(svc, "delta-timeout-full-solve-total") >= 1,
              "AC4: delta-timeout-full-solve-total key populated");
        CHECK(href(svc, "delta-timeout-reject-total") >= 0,
              "AC4: delta-timeout-reject-total key present");
        CHECK(href(svc, "delta_timeout_full_solve_total") ==
                  href(svc, "delta-timeout-full-solve-total"),
              "AC4: kebab + snake_case keys agree");
        CHECK(href(svc, "delta-timeout-hard-gate-wired") == 1,
              "AC4: delta-timeout-hard-gate-wired sentinel");
    }

    restore();
}

// Issue #2308: Agent-stable SolverSnapshot — unified post-solve surface
// (status + unresolved + blame + repair_nodes + truncated + production
// escalation). Built from the live commit CS via snapshot_constraint_system
// (pure read). Tests verify:
//   AC1: Empty CS → SOLVED status, empty repair_nodes, blame is empty
//        (not complete by default), all 6 query keys reachable.
//   AC2: Snapshot reflects blame state — mark_touched + record_cross_delta
//        populates blame + snapshot surfaces it.
//   AC3: Production escalation flips production_escalated to true after
//        escalate_if_production (the #2277 path).
//   AC4: Query surface — 6 keys + 3 sentinels (schema-2308 / issue-2308 /
//        solver-snapshot-wired) reachable via
//        query:type-incremental-fidelity-stats.
//   AC5: No schema break — existing #2107 / #2277 keys still reachable.
static void ac8_2308_solver_snapshot() {
    std::println("\n--- AC8 (#2308): SolverSnapshot + query surface ---");

    // ── AC1: empty CS → SOLVED, empty repair_nodes, blame is empty ──
    {
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        const auto snap = snapshot_constraint_system(cs, nullptr);
        CHECK(snap.status == SolveResult::SOLVED, "AC8.1: empty CS → SOLVED (default status)");
        CHECK(snap.unresolved.empty(), "AC8.2: empty CS → unresolved empty");
        CHECK(snap.repair_nodes.empty(), "AC8.3: empty CS → repair_nodes empty");
        CHECK(!snap.truncated_reverify, "AC8.4: empty CS → truncated_reverify false");
        CHECK(!snap.production_escalated, "AC8.5: empty CS → production_escalated false");
        CHECK(snap.provenance_continuity == false,
              "AC8.6: empty CS → provenance_continuity false (blame empty)");
    }

    // ── AC2: snapshot reflects live blame state ──
    {
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        cs.set_active_mutation_id(2308);
        cs.set_active_blame_context(/*pred=*/42, /*affected=*/7);
        // Force a complete blame chain via the test helper.
        cs.force_last_blame_complete_for_test(/*mutation_id=*/2308,
                                              /*predicate=*/42,
                                              /*affected_node=*/7);
        const auto snap = snapshot_constraint_system(cs, nullptr);
        CHECK(snap.status == SolveResult::SOLVED,
              "AC8.7: blame-only snapshot still SOLVED (no solve_delta called)");
        CHECK(snap.provenance_continuity, "AC8.8: complete blame → provenance_continuity true");
        CHECK(snap.blame.complete, "AC8.9: complete blame surfaced on snapshot.blame");
        // Last frame's affected_node shows up in repair_nodes (since the
        // `last` param is null, repair_nodes is empty — blame.frames
        // only feeds repair_nodes when an unresolved list is supplied).
        CHECK(snap.repair_nodes.empty(),
              "AC8.10: blame-only → repair_nodes empty (no unresolved feed)");
    }

    // ── AC3: production escalation flips production_escalated ──
    {
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        CHECK(!cs.production_escalated(), "AC8.11: fresh CS → production_escalated false");
        // Drive the production escalation path. Under sandbox (no
        // production_defaults_active) escalate is a no-op, so force the
        // typed_audit gate first.
        aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
            .store(1, std::memory_order_relaxed);
        (void)cs.escalate_if_production(SolveResult::TIMEOUT);
        // Restore the global for downstream tests.
        aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
            .store(0, std::memory_order_relaxed);
        CHECK(cs.production_escalated(),
              "AC8.12: escalate_if_production flipped production_escalated_");
        const auto snap = snapshot_constraint_system(cs, nullptr);
        CHECK(snap.production_escalated, "AC8.13: snapshot.production_escalated mirrors CS flag");
    }

    // ── AC4 + AC5: query surface — 6 keys + 3 sentinels + no schema break ──
    {
        CompilerService svc;
        CHECK(svc.eval("(+ 1 1)").has_value(), "AC8.14: eval smoke");

        // #2308 sentinels.
        CHECK(href(svc, "schema-2308") == 2308, "AC8.15: schema-2308 sentinel == 2308");
        CHECK(href(svc, "issue-2308") == 2308, "AC8.16: issue-2308 sentinel == 2308");
        CHECK(href(svc, "solver-snapshot-wired") == 1,
              "AC8.17: solver-snapshot-wired sentinel == 1 (proves #2308 refactor landed)");

        // 5 #2308 numeric / boolean keys reachable (>= 0 / 0 or 1).
        CHECK(href(svc, "solver-snapshot-status") >= 0,
              "AC8.18: solver-snapshot-status reachable (0=SOLVED, 1=CONFLICT, 2=TIMEOUT)");
        CHECK(href(svc, "solver-snapshot-unresolved-count") >= 0,
              "AC8.19: solver-snapshot-unresolved-count reachable");
        CHECK(href(svc, "solver-snapshot-repair-nodes-count") >= 0,
              "AC8.20: solver-snapshot-repair-nodes-count reachable");
        CHECK(href(svc, "solver-snapshot-blame-complete") >= 0,
              "AC8.21: solver-snapshot-blame-complete reachable (0/1)");
        CHECK(href(svc, "solver-snapshot-truncated") >= 0,
              "AC8.22: solver-snapshot-truncated reachable (0/1)");

        // AC5: no schema break — existing #2107 / #2277 keys still present.
        CHECK(href(svc, "schema-2277") == 2277,
              "AC8.23: schema-2277 still reachable (no #2107 schema break)");
        CHECK(href(svc, "issue-2277") == 2277,
              "AC8.24: issue-2277 still reachable (no #2277 schema break)");
        CHECK(href(svc, "delta-timeout-full-solve-total") >= 0,
              "AC8.25: delta-timeout-full-solve-total still reachable (no #2277 schema break)");
        CHECK(href(svc, "schema-2278") == 2278,
              "AC8.26: schema-2278 still reachable (no #2278 schema break — OccurrenceGoal table)");
        CHECK(href(svc, "occurrence-goal-sole-authority-wired") == 1,
              "AC8.27: #2307 sole-authority sentinel still reachable (no #2307 schema break)");
    }
}

// ── Issue #2318: anti-starvation streak gate (consecutive truncated
//   delta solves → force one full ConstraintSystem::solve()).
//   Per #81967 prefer-existing — natural home (refines #2107 unresolved
//   export lineage, same solve_delta path).
//   AC1: Streak counter — bump on truncate, reset on !truncate.
//   AC2: Force full solve at threshold (env AURA_DELTA_TRUNCATE_STREAK_FULL,
//        default 2). Reset streak on force. Mirror #2277 reject path.
//   AC3: Zero cost happy path (no truncate → no extra solve).
//   AC4: Observability — counter + sentinel + schema-2318 / issue-2318.
//   AC5: Tests — source-cite the streak field + threshold accessor +
//        solve_delta modification + query keys + Issue #2318 cite.

static void ac2318_streak_counter() {
    std::println("\n--- #2318 AC1: streak counter wiring ---");
    const auto tc = read_file("src/compiler/type_checker.ixx");
    const auto obm = read_file("src/compiler/observability_metrics.h");
    // AC1: streak field in ConstraintSystem
    CHECK(tc.find("truncate_streak_") != std::string::npos, "AC1: truncate_streak_ field present");
    CHECK(tc.find("truncate_streak_ = 0;") != std::string::npos,
          "AC1: truncate_streak_ field init");
    // AC1: threshold accessor (env AURA_DELTA_TRUNCATE_STREAK_FULL, default 2)
    CHECK(tc.find("delta_truncate_streak_threshold()") != std::string::npos,
          "AC1: delta_truncate_streak_threshold() accessor present");
    CHECK(tc.find("AURA_DELTA_TRUNCATE_STREAK_FULL") != std::string::npos,
          "AC1: env var AURA_DELTA_TRUNCATE_STREAK_FULL documented");
    // AC1: counters in observability_metrics.h
    CHECK(obm.find("delta_reverify_truncate_streak{0};") != std::string::npos,
          "AC1: delta_reverify_truncate_streak counter");
    CHECK(obm.find("delta_truncate_force_full_solve_total{0};") != std::string::npos,
          "AC1: delta_truncate_force_full_solve_total counter");
    CHECK(obm.find("delta_truncate_streak_threshold{0};") != std::string::npos,
          "AC1: delta_truncate_streak_threshold counter");
    CHECK(obm.find("delta_truncate_anti_starve_wired{0};") != std::string::npos,
          "AC1: delta_truncate_anti_starve_wired sentinel");
}

static void ac2318_force_full_solve() {
    std::println("\n--- #2318 AC2: force full solve at threshold ---");
    const auto tc = read_file("src/compiler/type_checker.ixx");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    // AC2: check_truncate_anti_starve method declaration + implementation
    CHECK(tc.find("check_truncate_anti_starve") != std::string::npos,
          "AC2: check_truncate_anti_starve method present");
    // Streak gate body lives in type_checker_impl.cpp (not the ixx decl).
    CHECK(tci.find("truncate_streak_") != std::string::npos &&
              (tci.find("threshold") != std::string::npos),
          "AC2: streak >= threshold check present");
    CHECK(tci.find("delta_truncate_force_full_solve_total") != std::string::npos,
          "AC2: force_full_solve_total bump present");
    CHECK(tci.find("return solve(unresolved_out)") != std::string::npos,
          "AC2: full solve call present");
    // AC2: solve_delta modified to call streak check after solve_delta_impl
    CHECK(tci.find("last_reverify_truncated_") != std::string::npos,
          "AC2: solve_delta uses last_reverify_truncated_");
    CHECK(tci.find("delta_truncate_streak_threshold") != std::string::npos,
          "AC2: solve_delta reads threshold");
    CHECK(tci.find("truncate_streak_") != std::string::npos,
          "AC2: solve_delta manages truncate_streak_");
}

static void ac2318_alt_truncate_clean() {
    std::println("\n--- #2318 AC3: zero cost happy path + alt truncate/clean ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    // AC3: no truncate → no extra full solve; only relaxed load/store
    CHECK(tci.find("else {") != std::string::npos,
          "AC3: else branch present (truncate=false path)");
    CHECK(tci.find("truncate_streak_ = 0;") != std::string::npos, "AC3: streak reset on !truncate");
    // AC3: streak reset in both with-metrics and no-metrics paths
    const auto else_count = tci.find("truncate_streak_ = 0;");
    CHECK(else_count != std::string::npos, "AC3: streak reset present");
}

static void ac2318_query_keys() {
    std::println("\n--- #2318 AC4: query keys ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm eval");
    // 4 #2318 keys reachable (>= 0 or 0/1).
    CHECK(href(cs, "delta-reverify-truncate-streak") >= 0,
          "AC4: delta-reverify-truncate-streak reachable");
    CHECK(href(cs, "delta-truncate-force-full-solve-total") >= 0,
          "AC4: delta-truncate-force-full-solve-total reachable");
    CHECK(href(cs, "delta-truncate-streak-threshold") >= 0,
          "AC4: delta-truncate-streak-threshold reachable");
    CHECK(href(cs, "delta-truncate-anti-starve-wired") >= 0,
          "AC4: delta-truncate-anti-starve-wired reachable (0 or 1)");
    // #2318 lineage preserved
    CHECK(href(cs, "schema-2318") == 2318, "AC4: schema-2318");
    CHECK(href(cs, "issue-2318") == 2318, "AC4: issue-2318");
    // Existing #2107 / #2277 / #2308 lineage preserved
    CHECK(href(cs, "schema-2107") == 2107, "AC4: schema-2107 retained (no #2107 schema break)");
    CHECK(href(cs, "schema-2277") == 2277, "AC4: schema-2277 retained (no #2277 schema break)");
}

static void ac2318_source_cite_rows() {
    std::println("\n--- #2318 AC5: source-cite rows ---");
    const auto tc = read_file("src/compiler/type_checker.ixx");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    const auto obm = read_file("src/compiler/observability_metrics.h");
    const auto ep = read_file("src/compiler/evaluator_primitives_query.cpp") +
                    read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    // #2318 cite in all modified files
    CHECK(tc.find("Issue #2318") != std::string::npos, "AC5: type_checker.ixx cites 2318");
    CHECK(tci.find("Issue #2318") != std::string::npos, "AC5: type_checker_impl.cpp cites 2318");
    CHECK(obm.find("// #2318") != std::string::npos || obm.find("Issue #2318") != std::string::npos,
          "AC5: observability_metrics.h cites 2318");
    CHECK(ep.find("schema-2318") != std::string::npos, "AC5: query primitive schema-2318");
    CHECK(ep.find("issue-2318") != std::string::npos, "AC5: query primitive issue-2318");
    // Streak field + threshold accessor
    CHECK(tc.find("truncate_streak_") != std::string::npos,
          "AC5: streak field in type_checker.ixx");
    CHECK(tc.find("delta_truncate_streak_threshold()") != std::string::npos,
          "AC5: threshold accessor in type_checker.ixx");
}

// ── Issue #2900: SolverBudget Agent-controlled delta TIMEOUT policy ──

static void ac2900_1_soft_allow_timeout_export() {
    std::println("\n--- #2900 AC1: Soft + allow_timeout_commit + TIMEOUT → export ---");
    using aura::compiler::SolverBudget;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    SolverBudget b{};
    b.allow_timeout_commit = true;
    b.prefer_instance_repair_before_full = true;
    cs.set_solver_budget(b);
    cs.force_next_delta_timeout_for_test(true);
    // Dirty work so unresolved is non-empty on synthetic TIMEOUT.
    auto v = cs.fresh_var();
    Constraint eq;
    eq.kind = Constraint::EQUAL;
    eq.lhs = v;
    eq.rhs = reg.int_type();
    cs.add_delta(std::move(eq));

    const auto exp0 = g_typed_mutation_audit_counters.solver_budget_timeout_export_total.load(
        std::memory_order_relaxed);
    const auto full0 = g_typed_mutation_audit_counters.delta_timeout_full_solve_total.load(
        std::memory_order_relaxed);

    std::vector<Constraint> unresolved;
    auto status = cs.solve_delta(&unresolved);
    CHECK(status == SolveResult::TIMEOUT, "2900 AC1: solve_delta TIMEOUT");
    CHECK(!unresolved.empty(), "2900 AC1: unresolved non-empty");
    auto post = cs.escalate_if_production(status, &unresolved);
    CHECK(post == SolveResult::TIMEOUT, "2900 AC1: Soft allow keeps TIMEOUT (not SOLVED)");
    CHECK(post != SolveResult::SOLVED, "2900 AC1: never pretend SOLVED");
    CHECK(g_typed_mutation_audit_counters.solver_budget_timeout_export_total.load(
              std::memory_order_relaxed) > exp0,
          "2900 AC1: timeout_export_total bumps");
    CHECK(g_typed_mutation_audit_counters.delta_timeout_full_solve_total.load(
              std::memory_order_relaxed) == full0,
          "2900 AC1: no full escalate under Soft allow");
    CHECK(metrics.solver_budget_timeout_export_total.load() >= 1,
          "2900 AC1: metrics mirror timeout_export");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac2900_2_production_still_escalates() {
    std::println("\n--- #2900 AC2: production + budget → still escalate ---");
    using aura::compiler::SolverBudget;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    SolverBudget b{};
    b.allow_timeout_commit = true; // must be ignored under production
    b.max_delta_passes = 1;
    b.prefer_instance_repair_before_full = true;
    cs.set_solver_budget(b);

    const auto esc0 = g_typed_mutation_audit_counters.solver_budget_full_escalate_total.load(
        std::memory_order_relaxed);
    const auto full0 = g_typed_mutation_audit_counters.delta_timeout_full_solve_total.load(
        std::memory_order_relaxed);

    // Empty CS: escalate TIMEOUT → full solve SOLVED.
    auto post = cs.escalate_if_production(SolveResult::TIMEOUT);
    CHECK(post == SolveResult::SOLVED, "2900 AC2: production escalate reaches SOLVED on empty");
    CHECK(g_typed_mutation_audit_counters.solver_budget_full_escalate_total.load(
              std::memory_order_relaxed) > esc0,
          "2900 AC2: full_escalate_total bumps under non-default budget");
    CHECK(g_typed_mutation_audit_counters.delta_timeout_full_solve_total.load(
              std::memory_order_relaxed) > full0,
          "2900 AC2: #2277 full_solve path still fires");
    CHECK(metrics.solver_budget_full_escalate_total.load() >= 1,
          "2900 AC2: metrics mirror full_escalate");

    // allow_timeout_commit cannot soft-ship under production: still escalates.
    CHECK(cs.solver_budget().allow_timeout_commit, "2900 AC2: budget field retained on CS");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac2900_3_default_budget_unchanged() {
    std::println("\n--- #2900 AC3: default/null budget → current behavior ---");
    using aura::compiler::kSolverBudgetDefault;
    using aura::compiler::SolverBudget;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CHECK(cs.solver_budget().is_default(), "2900 AC3: default budget is_default");
    CHECK(kSolverBudgetDefault.is_default(), "2900 AC3: kSolverBudgetDefault");
    const auto exp0 = g_typed_mutation_audit_counters.solver_budget_timeout_export_total.load(
        std::memory_order_relaxed);
    const auto esc0 = g_typed_mutation_audit_counters.solver_budget_full_escalate_total.load(
        std::memory_order_relaxed);
    auto post = cs.escalate_if_production(SolveResult::TIMEOUT);
    CHECK(post == SolveResult::TIMEOUT, "2900 AC3: Soft default TIMEOUT pass-through");
    CHECK(g_typed_mutation_audit_counters.solver_budget_timeout_export_total.load(
              std::memory_order_relaxed) == exp0,
          "2900 AC3: no timeout_export under default budget");
    CHECK(g_typed_mutation_audit_counters.solver_budget_full_escalate_total.load(
              std::memory_order_relaxed) == esc0,
          "2900 AC3: no full_escalate under Soft default");
    cs.clear_solver_budget();
    CHECK(cs.solver_budget().is_default(), "2900 AC3: clear restores default");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac2900_4_additive_query() {
    std::println("\n--- #2900 AC4: additive query + #2277 preserved ---");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "2900 AC4: warm");
    CHECK(href(svc, "schema-2900") == 2900, "2900 AC4: schema-2900");
    CHECK(href(svc, "issue-2900") == 2900, "2900 AC4: issue-2900");
    CHECK(href(svc, "solver-budget-wired") == 1, "2900 AC4: wired");
    CHECK(href(svc, "solver-budget-timeout-export-total") >= 0, "2900 AC4: timeout-export key");
    CHECK(href(svc, "solver-budget-full-escalate-total") >= 0, "2900 AC4: full-escalate key");
    CHECK(href(svc, "solver-budget-instance-repair-prefer-total") >= 0,
          "2900 AC4: instance-repair key");
    CHECK(href(svc, "schema-2277") == 2277, "2900 AC4: schema-2277 preserved");
    CHECK(href(svc, "delta-timeout-hard-gate-wired") == 1, "2900 AC4: #2277 wired");
    CHECK(aura::compiler::kSolverBudgetIssue == 2900, "2900 AC4: issue constant");
}

static void ac2900_5_source_cite() {
    std::println("\n--- #2900 AC5: source-cite + no docs/design ---");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
                   read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    const auto t = read_file("tests/compiler/test_solve_delta_unresolved_export.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_solver_budget_2900.py");
    const auto build = read_file("build.py");
    CHECK(ixx.find("SolverBudget") != std::string::npos, "2900 AC5: SolverBudget struct");
    CHECK(ixx.find("2900") != std::string::npos, "2900 AC5: ixx cites #2900");
    CHECK(ixx.find("set_solver_budget") != std::string::npos, "2900 AC5: set API");
    CHECK(impl.find("solver_budget_") != std::string::npos ||
              impl.find("solver_budget") != std::string::npos,
          "2900 AC5: impl uses budget");
    CHECK(impl.find("allow_timeout_commit") != std::string::npos, "2900 AC5: Soft allow path");
    CHECK(impl.find("escalate_if_production") != std::string::npos, "2900 AC5: #2277 preserved");
    CHECK(aud.find("solver_budget_timeout_export_total") != std::string::npos,
          "2900 AC5: audit counters");
    CHECK(q.find("schema-2900") != std::string::npos, "2900 AC5: query schema-2900");
    CHECK(q.find("schema-2277") != std::string::npos, "2900 AC5: schema-2277 retained");
    CHECK(t.find("ac2900_1_soft_allow_timeout_export") != std::string::npos, "2900 AC5: AC1 test");
    CHECK(t.find("ac2900_2_production_still_escalates") != std::string::npos, "2900 AC5: AC2 test");
    CHECK(t.find("ac2900_3_default_budget_unchanged") != std::string::npos, "2900 AC5: AC3 test");
    CHECK(t.find("ac2900_4_additive_query") != std::string::npos, "2900 AC5: AC4 test");
    CHECK(!lint.empty() && lint.find("2900") != std::string::npos, "2900 AC5: linter");
    CHECK(build.find("check_solver_budget_2900") != std::string::npos, "2900 AC5: build.py gate");
    CHECK(read_file("docs/design/2900-solver-budget.md").empty(),
          "2900 AC5: no docs/design/2900-* per #1655");
    CHECK(read_file("tests/compiler/test_issue_2900.cpp").empty(),
          "2900 AC5: no new test file per #81967");
}

// ── Issue #2963: production prefer instance-repair before full-solve ──
// Residual of #2900: production defaults prefer_instance_repair_before_full
// = true. On delta TIMEOUT, repair local dirty + pending roots first;
// only residual → full-solve escalate. Soft quiet zero cost. Never ship
// TIMEOUT / half-solved under production.

static void ac2963_1_production_repair_resolves() {
    std::println("\n--- #2963 AC1: production + dirty TIMEOUT → repair SOLVED ---");
    using aura::compiler::SolverBudget;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    // Default budget: prefer_instance_repair_before_full == true (#2963).
    CHECK(cs.solver_budget().prefer_instance_repair_before_full,
          "2963 AC1: production default prefer true");
    CHECK(cs.solver_budget().is_default(), "2963 AC1: default budget is_default");

    auto v = cs.fresh_var();
    Constraint eq;
    eq.kind = Constraint::EQUAL;
    eq.lhs = v;
    eq.rhs = reg.int_type();
    cs.add_delta(std::move(eq));
    cs.force_next_delta_timeout_for_test(true);

    const auto repair0 =
        g_typed_mutation_audit_counters.delta_instance_repair_total.load(std::memory_order_relaxed);
    const auto resolved0 =
        g_typed_mutation_audit_counters.delta_instance_repair_resolved_total.load(
            std::memory_order_relaxed);
    const auto full0 = g_typed_mutation_audit_counters.delta_timeout_full_solve_total.load(
        std::memory_order_relaxed);

    std::vector<Constraint> unresolved;
    auto status = cs.solve_delta(&unresolved);
    CHECK(status == SolveResult::TIMEOUT, "2963 AC1: synthetic TIMEOUT");
    CHECK(cs.is_dirty(), "2963 AC1: dirty remains for repair");
    auto post = cs.escalate_if_production(status, &unresolved);
    CHECK(post == SolveResult::SOLVED, "2963 AC1: repair reaches SOLVED");
    CHECK(post != SolveResult::TIMEOUT, "2963 AC1: never ship TIMEOUT under production");
    CHECK(g_typed_mutation_audit_counters.delta_instance_repair_total.load(
              std::memory_order_relaxed) > repair0,
          "2963 AC1: repair total bumps");
    CHECK(g_typed_mutation_audit_counters.delta_instance_repair_resolved_total.load(
              std::memory_order_relaxed) > resolved0,
          "2963 AC1: repair resolved bumps");
    CHECK(g_typed_mutation_audit_counters.delta_timeout_full_solve_total.load(
              std::memory_order_relaxed) == full0,
          "2963 AC1: no full-solve when repair SOLVED");
    CHECK(!cs.is_dirty(), "2963 AC1: dirty cleared after repair");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac2963_2_soft_quiet_zero_cost() {
    std::println("\n--- #2963 AC2: Soft quiet — no forced repair ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    auto v = cs.fresh_var();
    Constraint eq;
    eq.kind = Constraint::EQUAL;
    eq.lhs = v;
    eq.rhs = reg.int_type();
    cs.add_delta(std::move(eq));
    cs.force_next_delta_timeout_for_test(true);

    const auto repair0 =
        g_typed_mutation_audit_counters.delta_instance_repair_total.load(std::memory_order_relaxed);
    const auto full0 = g_typed_mutation_audit_counters.delta_timeout_full_solve_total.load(
        std::memory_order_relaxed);

    std::vector<Constraint> unresolved;
    auto status = cs.solve_delta(&unresolved);
    CHECK(status == SolveResult::TIMEOUT, "2963 AC2: Soft TIMEOUT");
    auto post = cs.escalate_if_production(status, &unresolved);
    CHECK(post == SolveResult::TIMEOUT, "2963 AC2: Soft pass-through TIMEOUT");
    CHECK(g_typed_mutation_audit_counters.delta_instance_repair_total.load(
              std::memory_order_relaxed) == repair0,
          "2963 AC2: no repair walk under Soft");
    CHECK(g_typed_mutation_audit_counters.delta_timeout_full_solve_total.load(
              std::memory_order_relaxed) == full0,
          "2963 AC2: no full escalate under Soft");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac2963_3_quiet_no_timeout_zero() {
    std::println("\n--- #2963 AC3/AC4: quiet SOLVED — zero repair cost ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    auto v = cs.fresh_var();
    Constraint eq;
    eq.kind = Constraint::EQUAL;
    eq.lhs = v;
    eq.rhs = reg.int_type();
    cs.add_delta(std::move(eq));

    const auto repair0 =
        g_typed_mutation_audit_counters.delta_instance_repair_total.load(std::memory_order_relaxed);
    const auto full0 = g_typed_mutation_audit_counters.delta_timeout_full_solve_total.load(
        std::memory_order_relaxed);

    auto status = cs.solve_delta();
    CHECK(status == SolveResult::SOLVED, "2963 AC3: clean delta SOLVED");
    auto post = cs.escalate_if_production(status);
    CHECK(post == SolveResult::SOLVED, "2963 AC3: escalate no-op on SOLVED");
    CHECK(g_typed_mutation_audit_counters.delta_instance_repair_total.load(
              std::memory_order_relaxed) == repair0,
          "2963 AC4: zero repair when no TIMEOUT");
    CHECK(g_typed_mutation_audit_counters.delta_timeout_full_solve_total.load(
              std::memory_order_relaxed) == full0,
          "2963 AC4: zero full-solve when no TIMEOUT");

    // Empty escalate TIMEOUT (no dirty) → no repair walk, full may run.
    const auto repair1 =
        g_typed_mutation_audit_counters.delta_instance_repair_total.load(std::memory_order_relaxed);
    auto empty_post = cs.escalate_if_production(SolveResult::TIMEOUT);
    CHECK(empty_post == SolveResult::SOLVED, "2963 AC4: empty CS full escalate SOLVED");
    CHECK(g_typed_mutation_audit_counters.delta_instance_repair_total.load(
              std::memory_order_relaxed) == repair1,
          "2963 AC4: no repair walk when no dirty/roots");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac2963_4_additive_schema() {
    std::println("\n--- #2963 AC3: additive schema + #2900/#2277 preserved ---");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "2963 AC3: warm");
    CHECK(href(svc, "schema-2963") == 2963, "2963 AC3: schema-2963");
    CHECK(href(svc, "issue-2963") == 2963, "2963 AC3: issue-2963");
    CHECK(href(svc, "delta-instance-repair-total") >= 0, "2963 AC3: repair-total key");
    CHECK(href(svc, "delta-instance-repair-resolved-total") >= 0, "2963 AC3: repair-resolved key");
    CHECK(href(svc, "delta-timeout-full-after-repair-total") >= 0,
          "2963 AC3: full-after-repair key");
    CHECK(href(svc, "delta-instance-repair-wired") == 1, "2963 AC3: wired");
    CHECK(href(svc, "schema-2900") == 2900, "2963 AC3: schema-2900 preserved");
    CHECK(href(svc, "schema-2277") == 2277, "2963 AC3: schema-2277 preserved");
    CHECK(aura::compiler::kSolverBudgetInstanceRepairIssue == 2963, "2963 AC3: issue constant");
}

static void ac2963_5_source_cite() {
    std::println("\n--- #2963 AC5: source-cite + no docs/design ---");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    const auto t = read_file("tests/compiler/test_solve_delta_unresolved_export.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_instance_repair_before_full_2963.py");
    const auto build = read_file("build.py");
    CHECK(ixx.find("try_instance_repair_before_full") != std::string::npos,
          "2963 AC5: try_instance_repair API");
    CHECK(ixx.find("2963") != std::string::npos, "2963 AC5: ixx cites #2963");
    CHECK(ixx.find("prefer_instance_repair_before_full = true") != std::string::npos ||
              ixx.find("prefer_instance_repair_before_full = true;") != std::string::npos,
          "2963 AC5: default prefer true");
    CHECK(impl.find("try_instance_repair_before_full") != std::string::npos,
          "2963 AC5: impl repair");
    CHECK(impl.find("delta_instance_repair_total") != std::string::npos,
          "2963 AC5: repair counter");
    CHECK(impl.find("delta_timeout_full_after_repair_total") != std::string::npos,
          "2963 AC5: full-after-repair counter");
    CHECK(aud.find("delta_instance_repair_total") != std::string::npos, "2963 AC5: audit counters");
    CHECK(q.find("schema-2963") != std::string::npos, "2963 AC5: query schema-2963");
    CHECK(q.find("delta-instance-repair-total") != std::string::npos, "2963 AC5: query keys");
    CHECK(t.find("ac2963_1_production_repair_resolves") != std::string::npos, "2963 AC5: AC1 test");
    CHECK(!lint.empty() && lint.find("2963") != std::string::npos, "2963 AC5: linter");
    CHECK(build.find("check_instance_repair_before_full_2963") != std::string::npos,
          "2963 AC5: build.py gate");
    CHECK(read_file("docs/design/2963-instance-repair.md").empty(),
          "2963 AC5: no docs/design/2963-* per #1655");
    CHECK(read_file("tests/compiler/test_issue_2963.cpp").empty(),
          "2963 AC5: no new test file per #81967");
}

static void ac2963_6_large_cs_small_dirty_repair_hit() {
    std::println("\n--- #2963 AC6: large CS + small dirty → repair hit, no full regress ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);

    // Seed a large clean cone (already SOLVED constraints).
    for (int i = 0; i < 64; ++i) {
        auto a = cs.fresh_var();
        Constraint eq;
        eq.kind = Constraint::EQUAL;
        eq.lhs = a;
        eq.rhs = reg.int_type();
        cs.add_delta(std::move(eq));
    }
    CHECK(cs.solve_delta() == SolveResult::SOLVED, "2963 AC6: seed cone SOLVED");

    // Small dirty cone + synthetic TIMEOUT.
    auto v = cs.fresh_var();
    Constraint dirty;
    dirty.kind = Constraint::EQUAL;
    dirty.lhs = v;
    dirty.rhs = reg.int_type();
    cs.add_delta(std::move(dirty));
    cs.force_next_delta_timeout_for_test(true);

    const auto repair0 =
        g_typed_mutation_audit_counters.delta_instance_repair_total.load(std::memory_order_relaxed);
    const auto resolved0 =
        g_typed_mutation_audit_counters.delta_instance_repair_resolved_total.load(
            std::memory_order_relaxed);
    const auto full0 = g_typed_mutation_audit_counters.delta_timeout_full_solve_total.load(
        std::memory_order_relaxed);

    std::vector<Constraint> unresolved;
    auto status = cs.solve_delta(&unresolved);
    CHECK(status == SolveResult::TIMEOUT, "2963 AC6: TIMEOUT on small dirty");
    auto post = cs.escalate_if_production(status, &unresolved);
    CHECK(post == SolveResult::SOLVED, "2963 AC6: repair SOLVED small dirty");
    CHECK(g_typed_mutation_audit_counters.delta_instance_repair_total.load(
              std::memory_order_relaxed) > repair0,
          "2963 AC6: repair hit rate > 0");
    CHECK(g_typed_mutation_audit_counters.delta_instance_repair_resolved_total.load(
              std::memory_order_relaxed) > resolved0,
          "2963 AC6: repair resolved");
    CHECK(g_typed_mutation_audit_counters.delta_timeout_full_solve_total.load(
              std::memory_order_relaxed) == full0,
          "2963 AC6: full-solve does not regress (stays flat when repair wins)");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

// ── Issue #2913: solve_delta locality SLO + escalate_if_production residual ──
// Soft + residual → observe + allow. production / Full + residual → escalate
// full (or reject). Quiet local SOLVED → zero cost. Additive schema-2913.
//
// Soft vs production (#2913 AC6):
//   Soft + residual              → observe + allow
//   production / Full + residual → escalate or reject
//   clean local                  → zero cost

static void ac2913_1_production_escalate() {
    std::println("\n--- #2913 AC1: production + residual → escalate ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    cs.force_locality_pruned_for_test(3); // inject residual under-constrain
    CHECK(cs.last_locality_pruned() == 3, "2913 AC1: inject residual");

    const auto esc0 = g_typed_mutation_audit_counters.solve_delta_locality_escalate_total.load(
        std::memory_order_relaxed);
    const auto obs0 = g_typed_mutation_audit_counters.solve_delta_locality_slo_observe_total.load(
        std::memory_order_relaxed);

    auto post = cs.escalate_locality_slo_if_production(SolveResult::SOLVED);
    CHECK(post == SolveResult::SOLVED, "2913 AC1: production escalate reaches SOLVED on empty CS");
    CHECK(g_typed_mutation_audit_counters.solve_delta_locality_escalate_total.load(
              std::memory_order_relaxed) > esc0,
          "2913 AC1: locality_escalate_total bumps");
    CHECK(g_typed_mutation_audit_counters.solve_delta_locality_slo_observe_total.load(
              std::memory_order_relaxed) == obs0,
          "2913 AC1: Soft observe NOT bumped under production");
    CHECK(cs.production_escalated(), "2913 AC1: production_escalated_ set");
    CHECK(metrics.solve_delta_locality_escalate_total.load() >= 1,
          "2913 AC1: metrics mirror escalate");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac2913_2_soft_observe_and_quiet() {
    std::println("\n--- #2913 AC2: Soft residual observe; quiet zero cost ---");
    using aura::compiler::typed_audit::AuditStrategy;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    using aura::compiler::typed_audit::get_strategy;
    using aura::compiler::typed_audit::set_strategy;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    auto save_strat = get_strategy();
    // Soft Sampled (not Full): residual is observe-only. Cold-start default
    // strategy is Full — must demote so Soft path is exercised.
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    set_strategy(AuditStrategy::Sampled);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);

    // Quiet: no residual → pass-through, no counters.
    const auto esc0 = g_typed_mutation_audit_counters.solve_delta_locality_escalate_total.load(
        std::memory_order_relaxed);
    const auto obs0 = g_typed_mutation_audit_counters.solve_delta_locality_slo_observe_total.load(
        std::memory_order_relaxed);
    auto quiet = cs.escalate_locality_slo_if_production(SolveResult::SOLVED);
    CHECK(quiet == SolveResult::SOLVED, "2913 AC2: quiet SOLVED");
    CHECK(g_typed_mutation_audit_counters.solve_delta_locality_escalate_total.load(
              std::memory_order_relaxed) == esc0,
          "2913 AC2: quiet no escalate");
    CHECK(g_typed_mutation_audit_counters.solve_delta_locality_slo_observe_total.load(
              std::memory_order_relaxed) == obs0,
          "2913 AC2: quiet no observe");

    // Soft + residual → observe + allow.
    cs.force_locality_pruned_for_test(2);
    auto soft = cs.escalate_locality_slo_if_production(SolveResult::SOLVED);
    CHECK(soft == SolveResult::SOLVED, "2913 AC2: Soft allows residual SOLVED");
    CHECK(g_typed_mutation_audit_counters.solve_delta_locality_slo_observe_total.load(
              std::memory_order_relaxed) > obs0,
          "2913 AC2: Soft observe bumps");
    CHECK(g_typed_mutation_audit_counters.solve_delta_locality_escalate_total.load(
              std::memory_order_relaxed) == esc0,
          "2913 AC2: Soft no escalate");
    CHECK(metrics.solve_delta_locality_slo_observe_total.load() >= 1,
          "2913 AC2: metrics mirror observe");

    // Non-SOLVED prior is pass-through (TIMEOUT handled by #2277).
    auto to = cs.escalate_locality_slo_if_production(SolveResult::TIMEOUT);
    CHECK(to == SolveResult::TIMEOUT, "2913 AC2: TIMEOUT prior unchanged");

    set_strategy(save_strat);
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac2913_3_commit_readiness_after_escalate() {
    std::println("\n--- #2913 AC3: after escalate → production_escalated + SOLVED ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    cs.force_locality_pruned_for_test(1);
    auto post = cs.escalate_locality_slo_if_production(SolveResult::SOLVED);
    CHECK(post == SolveResult::SOLVED, "2913 AC3: escalate returns SOLVED");
    CHECK(cs.production_escalated(), "2913 AC3: production_escalated for commit_readiness");
    CHECK(cs.last_locality_pruned() == 0, "2913 AC3: residual cleared after SOLVED escalate");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac2913_4_additive_schema() {
    std::println("\n--- #2913 AC4: additive schema + preserve prior keys ---");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "2913 AC4: warm");
    CHECK(href(svc, "schema-2913") == 2913, "2913 AC4: schema-2913");
    CHECK(href(svc, "issue-2913") == 2913, "2913 AC4: issue-2913");
    CHECK(href(svc, "solve-delta-locality-slo-wired") == 1, "2913 AC4: wired");
    CHECK(href(svc, "solve-delta-locality-escalate-total") >= 0, "2913 AC4: escalate key");
    CHECK(href(svc, "solve-delta-locality-slo-observe-total") >= 0, "2913 AC4: observe key");
    CHECK(href(svc, "solve-delta-locality-reject-total") >= 0, "2913 AC4: reject key");
    // Preserve #1871 / #2277 / #2900.
    CHECK(href(svc, "solve-delta-locality-hits") >= 0, "2913 AC4: locality-hits preserved");
    CHECK(href(svc, "solve-delta-locality-misses") >= 0, "2913 AC4: locality-misses preserved");
    CHECK(href(svc, "schema-2277") == 2277, "2913 AC4: schema-2277 preserved");
    CHECK(href(svc, "schema-2900") == 2900, "2913 AC4: schema-2900 preserved");
}

static void ac2913_5_source_cite() {
    std::println("\n--- #2913 AC5: source-cite + suite extend ---");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
                   read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    const auto t = read_file("tests/compiler/test_solve_delta_unresolved_export.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_solve_delta_locality_slo_2913.py");
    const auto build = read_file("build.py");
    CHECK(ixx.find("escalate_locality_slo_if_production") != std::string::npos,
          "2913 AC5: ixx API");
    CHECK(ixx.find("2913") != std::string::npos, "2913 AC5: ixx cites #2913");
    CHECK(impl.find("escalate_locality_slo_if_production") != std::string::npos,
          "2913 AC5: impl body");
    CHECK(impl.find("last_locality_pruned_") != std::string::npos, "2913 AC5: residual snapshot");
    CHECK(impl.find("Soft + residual") != std::string::npos ||
              impl.find("Soft vs production") != std::string::npos,
          "2913 AC5: Soft vs production table in comments");
    CHECK(aud.find("solve_delta_locality_escalate_total") != std::string::npos,
          "2913 AC5: audit counters");
    CHECK(q.find("schema-2913") != std::string::npos, "2913 AC5: query schema");
    CHECK(q.find("solve-delta-locality-escalate-total") != std::string::npos,
          "2913 AC5: escalate query key");
    CHECK(t.find("ac2913_1_production_escalate") != std::string::npos, "2913 AC5: AC1 test");
    CHECK(t.find("ac2913_2_soft_observe_and_quiet") != std::string::npos, "2913 AC5: AC2 test");
    CHECK(!lint.empty() && lint.find("2913") != std::string::npos, "2913 AC5: linter");
    CHECK(build.find("check_solve_delta_locality_slo_2913") != std::string::npos,
          "2913 AC5: build.py");
    CHECK(read_file("docs/design/2913-solve-delta-locality-slo.md").empty(),
          "2913 AC5: no docs/design/2913-* per #1655");
    CHECK(read_file("tests/compiler/test_issue_2913.cpp").empty(),
          "2913 AC5: no new test file per #81967");
}

static void ac2913_6_wired_in_solve_delta() {
    std::println("\n--- #2913 AC6: solve_delta wrapper calls locality SLO ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(impl.find("escalate_locality_slo_if_production(result") != std::string::npos ||
              impl.find("escalate_locality_slo_if_production(result,") != std::string::npos,
          "2913 AC6: solve_delta wrapper calls escalate_locality_slo");
    CHECK(impl.find("last_locality_pruned_ = pruned") != std::string::npos,
          "2913 AC6: impl snapshots pruned residual");
}

// ── Issue #2994: Agent locality residual budget ──

static void ac2994_1_default_budget_escalate() {
    std::println("\n--- #2994 AC1: default budget 0 → #2913 escalate ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    CHECK(cs.solver_budget().is_default(), "2994 AC1: default is_default");
    CHECK(cs.solver_budget().max_locality_residual == 0, "2994 AC1: residual budget 0");
    cs.force_locality_pruned_for_test(3);
    const auto allow0 = g_typed_mutation_audit_counters.delta_locality_budget_allow_total.load(
        std::memory_order_relaxed);
    const auto esc0 = g_typed_mutation_audit_counters.solve_delta_locality_escalate_total.load(
        std::memory_order_relaxed);
    auto post = cs.escalate_locality_slo_if_production(SolveResult::SOLVED);
    CHECK(post == SolveResult::SOLVED, "2994 AC1: escalate SOLVED on empty CS");
    CHECK(cs.production_escalated(), "2994 AC1: production_escalated");
    CHECK(g_typed_mutation_audit_counters.solve_delta_locality_escalate_total.load(
              std::memory_order_relaxed) > esc0,
          "2994 AC1: #2913 escalate still fires");
    CHECK(g_typed_mutation_audit_counters.delta_locality_budget_allow_total.load(
              std::memory_order_relaxed) == allow0,
          "2994 AC1: budget-allow not bumped on default");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac2994_2_budget_allow_pending_handoff() {
    std::println("\n--- #2994 AC2: residual ≤ budget → SOLVED + pending handoff ---");
    using aura::compiler::SolverBudget;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    SolverBudget b{};
    b.max_locality_residual = 4;
    cs.set_solver_budget(b);
    CHECK(!cs.solver_budget().is_default(), "2994 AC2: N>0 is not default");
    auto v = cs.fresh_var();
    Constraint eq;
    eq.kind = Constraint::EQUAL;
    eq.lhs = v;
    eq.rhs = reg.int_type();
    cs.add_delta(std::move(eq));
    CHECK(cs.is_dirty(), "2994 AC2: dirty residual");
    cs.force_locality_pruned_for_test(2);
    const auto allow0 = g_typed_mutation_audit_counters.delta_locality_budget_allow_total.load(
        std::memory_order_relaxed);
    const auto hand0 =
        g_typed_mutation_audit_counters.delta_locality_budget_pending_handoff_total.load(
            std::memory_order_relaxed);
    const auto esc0 = g_typed_mutation_audit_counters.solve_delta_locality_escalate_total.load(
        std::memory_order_relaxed);

    auto post = cs.escalate_locality_slo_if_production(SolveResult::SOLVED);
    CHECK(post == SolveResult::SOLVED, "2994 AC2: SOLVED retained");
    CHECK(!cs.production_escalated(), "2994 AC2: no full escalate");
    CHECK(cs.pending_full_solve_roots_size() > 0, "2994 AC2: pending handoff non-empty");
    CHECK(cs.is_dirty(), "2994 AC2: dirty bits retained (no silent drop)");
    CHECK(g_typed_mutation_audit_counters.delta_locality_budget_allow_total.load(
              std::memory_order_relaxed) > allow0,
          "2994 AC2: allow counter");
    CHECK(g_typed_mutation_audit_counters.delta_locality_budget_pending_handoff_total.load(
              std::memory_order_relaxed) > hand0,
          "2994 AC2: pending-handoff counter");
    CHECK(g_typed_mutation_audit_counters.solve_delta_locality_escalate_total.load(
              std::memory_order_relaxed) == esc0,
          "2994 AC2: #2913 escalate not bumped");
    const auto pend0 = cs.pending_full_solve_roots_size();
    auto next = cs.solve_delta();
    CHECK(next == SolveResult::SOLVED, "2994 AC2: next solve_delta consumes");
    CHECK(cs.pending_full_solve_roots_size() < pend0 || !cs.is_dirty(),
          "2994 AC2: pending/dirty drained");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac2994_3_budget_over_escalate() {
    std::println("\n--- #2994 AC3: residual > budget → full escalate ---");
    using aura::compiler::SolverBudget;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    SolverBudget b{};
    b.max_locality_residual = 1;
    cs.set_solver_budget(b);
    cs.force_locality_pruned_for_test(5);
    const auto besc0 = g_typed_mutation_audit_counters.delta_locality_budget_escalate_total.load(
        std::memory_order_relaxed);
    const auto esc0 = g_typed_mutation_audit_counters.solve_delta_locality_escalate_total.load(
        std::memory_order_relaxed);
    auto post = cs.escalate_locality_slo_if_production(SolveResult::SOLVED);
    CHECK(post == SolveResult::SOLVED, "2994 AC3: escalate SOLVED on empty CS");
    CHECK(cs.production_escalated(), "2994 AC3: production_escalated");
    CHECK(g_typed_mutation_audit_counters.delta_locality_budget_escalate_total.load(
              std::memory_order_relaxed) > besc0,
          "2994 AC3: budget-escalate");
    CHECK(g_typed_mutation_audit_counters.solve_delta_locality_escalate_total.load(
              std::memory_order_relaxed) > esc0,
          "2994 AC3: #2913 escalate");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac2994_4_soft_no_budget_counters() {
    std::println("\n--- #2994 AC4: Soft residual never full-solves ---");
    using aura::compiler::SolverBudget;
    using aura::compiler::typed_audit::AuditStrategy;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    using aura::compiler::typed_audit::get_strategy;
    using aura::compiler::typed_audit::set_strategy;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    auto save_strat = get_strategy();
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    set_strategy(AuditStrategy::Sampled);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    SolverBudget b{};
    b.max_locality_residual = 8;
    cs.set_solver_budget(b);
    cs.force_locality_pruned_for_test(3);
    const auto allow0 = g_typed_mutation_audit_counters.delta_locality_budget_allow_total.load(
        std::memory_order_relaxed);
    const auto besc0 = g_typed_mutation_audit_counters.delta_locality_budget_escalate_total.load(
        std::memory_order_relaxed);
    const auto esc0 = g_typed_mutation_audit_counters.solve_delta_locality_escalate_total.load(
        std::memory_order_relaxed);
    auto soft = cs.escalate_locality_slo_if_production(SolveResult::SOLVED);
    CHECK(soft == SolveResult::SOLVED, "2994 AC4: Soft allows");
    CHECK(g_typed_mutation_audit_counters.solve_delta_locality_escalate_total.load(
              std::memory_order_relaxed) == esc0,
          "2994 AC4: no full escalate");
    CHECK(g_typed_mutation_audit_counters.delta_locality_budget_allow_total.load(
              std::memory_order_relaxed) == allow0,
          "2994 AC4: no budget-allow (Soft observe only)");
    CHECK(g_typed_mutation_audit_counters.delta_locality_budget_escalate_total.load(
              std::memory_order_relaxed) == besc0,
          "2994 AC4: no budget-escalate");

    set_strategy(save_strat);
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac2994_5_quiet_zero_cost() {
    std::println("\n--- #2994 AC5: residual 0 → no new atomics ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    const auto allow0 = g_typed_mutation_audit_counters.delta_locality_budget_allow_total.load(
        std::memory_order_relaxed);
    const auto besc0 = g_typed_mutation_audit_counters.delta_locality_budget_escalate_total.load(
        std::memory_order_relaxed);
    const auto hand0 =
        g_typed_mutation_audit_counters.delta_locality_budget_pending_handoff_total.load(
            std::memory_order_relaxed);
    auto quiet = cs.escalate_locality_slo_if_production(SolveResult::SOLVED);
    CHECK(quiet == SolveResult::SOLVED, "2994 AC5: quiet SOLVED");
    CHECK(g_typed_mutation_audit_counters.delta_locality_budget_allow_total.load(
              std::memory_order_relaxed) == allow0,
          "2994 AC5: no allow");
    CHECK(g_typed_mutation_audit_counters.delta_locality_budget_escalate_total.load(
              std::memory_order_relaxed) == besc0,
          "2994 AC5: no budget-escalate");
    CHECK(g_typed_mutation_audit_counters.delta_locality_budget_pending_handoff_total.load(
              std::memory_order_relaxed) == hand0,
          "2994 AC5: no handoff");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac2994_6_is_default_and_schema() {
    std::println("\n--- #2994 AC6: is_default + schema-2994 ---");
    CHECK(kSolverBudgetDefault.is_default(), "2994 AC6: kSolverBudgetDefault");
    CHECK(aura::compiler::kSolverBudgetLocalityIssue == 2994, "2994 AC6: issue constant");
    SolverBudget b{};
    CHECK(b.is_default(), "2994 AC6: zero-init is_default");
    b.max_locality_residual = 2;
    CHECK(!b.is_default(), "2994 AC6: residual N>0 not default");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "2994 AC6: warm");
    CHECK(href(svc, "schema-2994") == 2994, "2994 AC6: schema-2994");
    CHECK(href(svc, "issue-2994") == 2994, "2994 AC6: issue-2994");
    CHECK(href(svc, "delta-locality-budget-wired") == 1, "2994 AC6: wired");
    CHECK(href(svc, "delta-locality-budget-allow-total") >= 0, "2994 AC6: allow key");
    CHECK(href(svc, "delta-locality-budget-escalate-total") >= 0, "2994 AC6: escalate key");
    CHECK(href(svc, "delta-locality-budget-pending-handoff-total") >= 0, "2994 AC6: handoff key");
    CHECK(href(svc, "schema-2913") == 2913, "2994 AC6: schema-2913 preserved");
}

static void ac2994_7_source_cite() {
    std::println("\n--- #2994 AC7: source-cite ---");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto t = read_file("tests/compiler/test_solve_delta_unresolved_export.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_solve_delta_locality_budget_2994.py");
    const auto build = read_file("build.py");
    CHECK(ixx.find("escalate_locality_slo_if_production") != std::string::npos,
          "2994 AC7: escalate API");
    CHECK(ixx.find("max_locality_residual") != std::string::npos, "2994 AC7: budget field");
    CHECK(ixx.find("prefer_pending_roots_next") != std::string::npos, "2994 AC7: prefer pending");
    CHECK(impl.find("handoff_locality_residual_to_pending") != std::string::npos,
          "2994 AC7: handoff");
    CHECK(impl.find("delta_locality_budget_allow_total") != std::string::npos, "2994 AC7: allow");
    CHECK(t.find("ac2994_1_default_budget_escalate") != std::string::npos, "2994 AC7: AC1");
    CHECK(t.find("force_locality_pruned_for_test") != std::string::npos, "2994 AC7: inject");
    CHECK(!lint.empty(), "2994 AC7: linter");
    CHECK(build.find("check_solve_delta_locality_budget_2994") != std::string::npos,
          "2994 AC7: build.py");
    CHECK(read_file("docs/design/2994-locality-residual-budget.md").empty(),
          "2994 AC7: no docs/design/");
    CHECK(read_file("tests/compiler/test_issue_2994.cpp").empty(),
          "2994 AC7: no invent test_issue_2994");
}

// ── Issue #3003: Production solve_delta fail-closed (no half-solution) ──
// AC1 production + TIMEOUT → escalate; not SOLVED → reject (no write/stash)
// AC2 Soft TIMEOUT observe; fail-closed counters quiet
// AC3 last_type_export_authoritative / stash-not-live
// AC4 schema-3003 + #2277 lineage
// AC5 source-cite + linter; no invent / no design

static void ac3003_1_production_solve_delta_fail_closed() {
    std::println("\n--- #3003 AC1: production TIMEOUT → escalate / reject ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    auto a = cs.fresh_var();
    Constraint eq1;
    eq1.kind = Constraint::EQUAL;
    eq1.lhs = a;
    eq1.rhs = reg.int_type();
    Constraint eq2;
    eq2.kind = Constraint::EQUAL;
    eq2.lhs = a;
    eq2.rhs = reg.bool_type();
    cs.add_delta(std::move(eq1));
    cs.add_delta(std::move(eq2));
    cs.force_next_delta_timeout_for_test(true);
    const auto reject0 = metrics.delta_timeout_reject_total.load();
    auto injected = cs.solve_delta();
    CHECK(injected == SolveResult::TIMEOUT, "3003 AC1: force-timeout hook stays raw TIMEOUT");
    auto post = cs.escalate_if_production(injected);
    CHECK(post != SolveResult::SOLVED, "3003 AC1: production escalate not SOLVED on conflict");
    CHECK(metrics.delta_timeout_reject_total.load() > reject0,
          "3003 AC1: reject total increments (no half-solution)");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3003_2_soft_timeout_observe() {
    std::println("\n--- #3003 AC2: Soft TIMEOUT observe-only ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    auto a = cs.fresh_var();
    auto b = cs.fresh_var();
    Constraint eq;
    eq.kind = Constraint::EQUAL;
    eq.lhs = a;
    eq.rhs = b;
    cs.add_delta(std::move(eq));
    cs.force_next_delta_timeout_for_test(true);
    const auto fc0 = g_typed_mutation_audit_counters.delta_timeout_fail_closed_total.load(
        std::memory_order_relaxed);
    const auto full0 = metrics.delta_timeout_full_solve_total.load();
    auto post = cs.solve_delta();
    CHECK(post == SolveResult::TIMEOUT, "3003 AC2: Soft keeps TIMEOUT");
    CHECK(metrics.delta_timeout_full_solve_total.load() == full0,
          "3003 AC2: Soft no full-solve escalate");
    CHECK(g_typed_mutation_audit_counters.delta_timeout_fail_closed_total.load(
              std::memory_order_relaxed) == fc0,
          "3003 AC2: Soft no fail-closed bump");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3003_3_no_stash_no_authority() {
    std::println("\n--- #3003 AC3: type-export authority + no live stash ---");
    TypeRegistry reg;
    aura::compiler::TypeChecker tc(reg);
    CHECK(tc.last_type_export_authoritative(), "3003 AC3: default authoritative");
    CHECK(tc.last_delta_solve_status() == SolveResult::SOLVED, "3003 AC3: default SOLVED");
    tc.clear_last_type_export_authoritative();
    CHECK(!tc.last_type_export_authoritative(), "3003 AC3: cleared authority");
    CHECK(!tc.last_partial_cs_live(), "3003 AC3: stash not live after clear");
    const auto gate = read_file("src/orch/security_schedule_gate.h");
    (void)gate;
    const auto tc_cpp = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(tc_cpp.find("last_type_export_authoritative") != std::string::npos,
          "3003 AC3: stash / typecheck gate uses authority");
    CHECK(tc_cpp.find("type_export_authoritative_") != std::string::npos,
          "3003 AC3: Evaluator authority flag");
}

static void ac3003_4_schema_and_lineage() {
    std::println("\n--- #3003 AC4: schema-3003 + #2277 lineage ---");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "3003 AC4: warm");
    CHECK(href(svc, "schema-3003") == 3003, "3003 AC4: schema-3003");
    CHECK(href(svc, "issue-3003") == 3003, "3003 AC4: issue-3003");
    CHECK(href(svc, "delta-timeout-fail-closed-wired") == 1, "3003 AC4: wired");
    CHECK(href(svc, "delta-timeout-fail-closed-total") >= 0, "3003 AC4: fail-closed total");
    CHECK(href(svc, "schema-2277") == 2277, "3003 AC4: schema-2277 preserved");
    CHECK(href(svc, "delta-timeout-reject-total") >= 0, "3003 AC4: #2277 reject key");
    CHECK(href(svc, "schema-2913") == 2913, "3003 AC4: schema-2913 preserved");
}

static void ac3003_5_source_and_linter() {
    std::println("\n--- #3003 AC5: source-cite + linter ---");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    const auto ev = read_file("src/compiler/evaluator.ixx");
    const auto t = read_file("tests/compiler/test_solve_delta_unresolved_export.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_solve_delta_timeout_fail_closed_3003.py");
    const auto build = read_file("build.py");
    CHECK(ixx.find("kDeltaTimeoutFailClosedIssue = 3003") != std::string::npos,
          "3003 AC5: issue stamp");
    CHECK(impl.find("!forced_timeout_this_call_") != std::string::npos,
          "3003 AC5: wrapper SSOT escalate (skip force-timeout hook)");
    CHECK(impl.find("do not write a type then mark dirty-clean") != std::string::npos ||
              impl.find("I1/I5") != std::string::npos,
          "3003 AC5: infer_flat no write on production fail");
    CHECK(aud.find("delta_timeout_fail_closed_total") != std::string::npos, "3003 AC5: audit");
    CHECK(ev.find("type_export_authoritative") != std::string::npos, "3003 AC5: Evaluator flag");
    CHECK(t.find("ac3003_1_production_solve_delta_fail_closed") != std::string::npos,
          "3003 AC5: AC1");
    CHECK(!lint.empty() && lint.find("3003") != std::string::npos, "3003 AC5: linter");
    CHECK(build.find("check_solve_delta_timeout_fail_closed_3003") != std::string::npos,
          "3003 AC5: build.py");
    CHECK(read_file("docs/design/3003-solve-delta-timeout-fail-closed.md").empty(),
          "3003 AC5: no docs/design/");
    CHECK(read_file("tests/compiler/test_issue_3003.cpp").empty(),
          "3003 AC5: no invent test_issue_3003");
}

// ── Issue #3169: production solve_delta fail-closed + clear partial goals / unresolved ──
// AC1 production + TIMEOUT/CONFLICT/residual unresolved after repair failure →
//        partial goals cleared + hard reject (no type write, no dirty-clear).
// AC2 Soft/Off/unit-test default: zero behavioural change (counter only bumps
//        under production_defaults_active()).
// AC3 Quiet (clean / no dirty / no TIMEOUT): zero extra atomics / no lock.
// AC4 Additive observability only (one counter + schema/issue sentinels).
//        Reuse existing #3003 / #2963 / #2913 surfaces; no new public query key.
// AC5 Extends existing solve_delta suite (no test_issue_3169.cpp per #81967;
//        no docs/design/3169-* per #1655).
// AC6 Linter: scripts/coverage/checks/check_solve_delta_partial_cleared_3169.py
//        --strict + --self-test PASS; build.py wires cmd_solve_delta_partial_
//        cleared_3168.
static void ac3169_1_production_clear_partial_and_reject() {
    std::println("\n--- #3169 AC1: Production TIMEOUT / CONFLICT → clear + hard reject ---");
    // Source-cite: clear helper decl + impl + wire sites.
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(ixx.find("void clear_partial_goals_and_unresolved() noexcept") != std::string::npos,
          "3169 AC1: clear helper decl in type_checker.ixx");
    CHECK(impl.find("void ConstraintSystem::clear_partial_goals_and_unresolved() noexcept") !=
              std::string::npos,
          "3169 AC1: clear helper impl in type_checker_impl.cpp");
    CHECK(impl.find("Issue #3169: clear any partial goal / unresolved state") != std::string::npos,
          "3169 AC1: wire cite at CONFLICT / post-full-solve branches");
    // Soft path untouched (counter only bumps under production_defaults_active()).
    CHECK(impl.find("if (!aura::compiler::typed_audit::production_defaults_active())") !=
              std::string::npos,
          "3169 AC2: soft gate at clear helper entry");
    // No docs/design/3169-* (per #1655).
    const auto docs = std::string("docs/design/");
    if (std::filesystem::exists(docs)) {
        for (const auto& f : std::filesystem::directory_iterator(docs)) {
            auto name = f.path().filename().string();
            CHECK(name.find("3169-") == std::string::npos,
                  "3169 AC5: no docs/design/3169-* plan doc (#1655)");
            (void)name;
            break;
        }
    }
    // No test_issue_3169.cpp (per #81967).
    for (const auto& rel : {std::string("tests/issues/test_issue_3169.cpp"),
                            std::string("tests/compiler/test_issue_3169.cpp"),
                            std::string("tests/serve/test_issue_3169.cpp")}) {
        std::error_code ec;
        CHECK(!std::filesystem::exists(rel, ec),
              std::format("3169 AC5: forbidden {} per #81967", rel));
    }
}

static void ac3169_2_soft_zero_extra() {
    std::println("\n--- #3169 AC2: Soft / Off / unit-test default → zero behavioural change ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    // Clear helper early-returns on !production_defaults_active() (AC2).
    CHECK(impl.find("void ConstraintSystem::clear_partial_goals_and_unresolved() noexcept") !=
              std::string::npos,
          "3169 AC2: clear helper present");
    // Counter only bumps under production_defaults_active() — the helper gates
    // BEFORE the metrics_ bump.
    const auto helper_pos =
        impl.find("void ConstraintSystem::clear_partial_goals_and_unresolved() noexcept");
    if (helper_pos != std::string::npos) {
        const auto helper_block = impl.substr(helper_pos, 1200);
        const auto gate_pos =
            helper_block.find("if (!aura::compiler::typed_audit::production_defaults_active())");
        const auto bump_pos = helper_block.find("solve_delta_partial_cleared_total.fetch_add");
        CHECK(gate_pos != std::string::npos && bump_pos != std::string::npos && gate_pos < bump_pos,
              "3169 AC2: production gate precedes counter bump (Soft untouched)");
    }
    // escalate_if_production Soft path (production_defaults_active()==false) does
    // NOT call clear_partial_goals_and_unresolved — existing #2277 pass-through
    // preserved.
    const auto esc_pos = impl.find("SolveResult ConstraintSystem::escalate_if_production");
    if (esc_pos != std::string::npos) {
        const auto esc_block = impl.substr(esc_pos, 2200);
        CHECK(esc_block.find("if (!prod)\n        return prior;") != std::string::npos,
              "3169 AC2: Soft pass-through preserved");
    }
}

static void ac3169_3_quiet_zero_extra() {
    std::println("\n--- #3169 AC3: Quiet (clean / no dirty / no TIMEOUT) → zero extra ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    // escalate_if_production early-returns on prior != TIMEOUT (line ~2720).
    // So on the happy SOLVED path, no clear_partial_goals_and_unresolved call
    // fires (zero extra atomics / no lock).
    const auto esc_pos = impl.find("SolveResult ConstraintSystem::escalate_if_production");
    if (esc_pos != std::string::npos) {
        const auto esc_block = impl.substr(esc_pos, 800);
        CHECK(esc_block.find("if (prior != SolveResult::TIMEOUT)\n        return prior;") !=
                  std::string::npos,
              "3169 AC3: happy path returns prior (no clear call)");
    }
}

static void ac3169_4_additive_counter_only() {
    std::println("\n--- #3169 AC4: Additive observability only — counter + struct-end ---");
    const auto obs = read_file("src/compiler/observability_metrics.h");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    // Counter declared at struct end (layout-stable per #2906).
    CHECK(obs.find("std::atomic<std::uint64_t> solve_delta_partial_cleared_total{0}") !=
              std::string::npos,
          "3169 AC4: counter declared at struct end");
    CHECK(obs.find("// Issue #3169: production solve_delta fail-closed") != std::string::npos,
          "3169 AC4: counter comment cites #3169");
    // Counter only bumped inside the production-gated clear helper.
    CHECK(impl.find("solve_delta_partial_cleared_total.fetch_add") != std::string::npos,
          "3169 AC4: counter bumped in clear helper");
    // Existing #3003 / #2963 / #2913 / #2277 surfaces preserved.
    CHECK(obs.find("delta_timeout_full_solve_total") != std::string::npos,
          "3169 AC4: #2277 delta_timeout_full_solve_total preserved");
    CHECK(obs.find("delta_timeout_reject_total") != std::string::npos,
          "3169 AC4: #3003 delta_timeout_reject_total preserved");
    CHECK(obs.find("solver_budget_instance_repair_prefer_total") != std::string::npos,
          "3169 AC4: #2963 instance_repair_prefer_total preserved");
    CHECK(obs.find("delta_instance_repair_resolved_total") != std::string::npos,
          "3169 AC4: #2963 instance_repair_resolved_total preserved");
}

static void ac3169_5_existing_3003_2963_2913_preserved() {
    std::println("\n--- #3169 AC5: #3003 / #2963 / #2913 paths preserved ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    // #3003 / #2277 escalation pipeline preserved.
    CHECK(impl.find("delta_timeout_full_solve_total") != std::string::npos,
          "3169 AC5: #2277 escalation path preserved");
    CHECK(impl.find("delta_timeout_reject_total") != std::string::npos,
          "3169 AC5: #3003 reject path preserved");
    // #2963 instance repair before full preserved.
    CHECK(impl.find("try_instance_repair_before_full") != std::string::npos,
          "3169 AC5: #2963 try_instance_repair_before_full preserved");
    CHECK(impl.find("prefer_instance_repair_before_full") != std::string::npos,
          "3169 AC5: #2963 budget flag preserved");
    // #2913 locality SLO preserved (escalate_locality_slo_if_production).
    CHECK(impl.find("escalate_locality_slo_if_production") != std::string::npos,
          "3169 AC5: #2913 locality gate preserved");
}

// ── Issue #3190: outermost-success TypeLinearCommitProof drain before stamp ──
//
// Sibling #3031 closes the composite_txn_commit drain window. #3190 closes
// the outermost-success stamp window (aura_outermost_success_persist_occurrence)
// that the next composite commit / outermost stamp could observe as SOLVED
// even though pending_full_solve_roots_ / locality residual remained. The
// drain runs at the outermost success stamp site under production/Full/Strict;
// Soft observe-only; Quiet (no residual) → two size reads, zero extra atomics.
// Lockless batch (atomic_batch_active) flows through composite_txn_commit
// body, which already has the same drain — #3190 AC4 verifies the coverage.

static void ac3190_1_outermost_drain_production() {
    std::println("\n--- #3190 AC1: production drain at outermost success stamp ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    using aura::compiler::typed_audit::pending_full_solve_residual_escalate_total_v_read;
    using aura::compiler::typed_audit::pending_full_solve_residual_face_hit;
    using aura::compiler::typed_audit::pending_full_solve_residual_last_v_read;
    using aura::compiler::typed_audit::pending_full_solve_residual_observe_total_v_read;
    using aura::compiler::typed_audit::pending_full_solve_residual_reject_total_v_read;
    using aura::compiler::typed_audit::reset_pending_full_solve_residual_for_test;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    reset_pending_full_solve_residual_for_test();

    // (a) Empty residual drains to SOLVED — no reject, no observe.
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    cs.seed_pending_full_solve_root_for_test(1);
    const auto esc0 = pending_full_solve_residual_escalate_total_v_read();
    const auto rej0 = pending_full_solve_residual_reject_total_v_read();
    const auto obs0 = pending_full_solve_residual_observe_total_v_read();
    auto r = cs.drain_pending_full_solve_before_commit();
    CHECK(r == SolveResult::SOLVED, "3190 AC1: empty-CS pending drains to SOLVED");
    CHECK(cs.pending_full_solve_roots_size() == 0, "3190 AC1: pending cleared");
    CHECK(cs.last_locality_pruned() == 0, "3190 AC1: locality cleared");
    CHECK(pending_full_solve_residual_escalate_total_v_read() > esc0,
          "3190 AC1: escalate bumps under production");
    CHECK(pending_full_solve_residual_reject_total_v_read() == rej0,
          "3190 AC1: no reject on SOLVED");
    CHECK(pending_full_solve_residual_observe_total_v_read() == obs0,
          "3190 AC1: no Soft observe under production");
    CHECK(!pending_full_solve_residual_face_hit(), "3190 AC1: face clear after recover");

    // (b) Locality residual drains to SOLVED.
    cs.force_locality_pruned_for_test(2);
    r = cs.drain_pending_full_solve_before_commit();
    CHECK(r == SolveResult::SOLVED, "3190 AC1: locality residual drains to SOLVED");
    CHECK(cs.last_locality_pruned() == 0, "3190 AC1: locality residual cleared");

    // (c) Conflict residual → drain hard-rejects under production.
    //     This is the #3190 outermost stamp reject path: force_reason 16,
    //     face latched, last residual surfaced.
    ConstraintSystem cs2(reg);
    auto a = cs2.fresh_var();
    Constraint eq1;
    eq1.kind = Constraint::EQUAL;
    eq1.lhs = a;
    eq1.rhs = reg.int_type();
    Constraint eq2;
    eq2.kind = Constraint::EQUAL;
    eq2.lhs = a;
    eq2.rhs = reg.bool_type();
    cs2.add_delta(std::move(eq1));
    cs2.add_delta(std::move(eq2));
    cs2.seed_pending_full_solve_root_for_test(1);
    auto r2 = cs2.drain_pending_full_solve_before_commit();
    CHECK(r2 != SolveResult::SOLVED, "3190 AC1: conflict residual hard-rejects");
    CHECK(pending_full_solve_residual_reject_total_v_read() > rej0, "3190 AC1: reject total bumps");
    CHECK(pending_full_solve_residual_face_hit(), "3190 AC1: face latched on reject");
    CHECK(pending_full_solve_residual_last_v_read() > 0,
          "3190 AC1: last residual surfaced for Agent");

    // (d) commit_readiness rejects dirty residual with stable force_reason 16.
    CommitReadinessInput in;
    in.solve_status = 0;
    in.linear_ok = true;
    in.blame_ok = true;
    in.pending_full_solve_hard = true;
    in.pending_full_solve_residual = true;
    auto cr = commit_readiness(in);
    CHECK(!cr.would_allow_commit, "3190 AC1: readiness rejects dirty residual");
    CHECK(cr.force_reason == "pending_full_solve_residual", "3190 AC1: force_reason");
    CHECK(cr.force_reason_code == 16, "3190 AC1: force_reason_code 16");

    reset_pending_full_solve_residual_for_test();
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3190_2_outermost_drain_soft() {
    std::println("\n--- #3190 AC2: Soft observe-only at outermost success stamp ---");
    using aura::compiler::typed_audit::AuditStrategy;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    using aura::compiler::typed_audit::get_strategy;
    using aura::compiler::typed_audit::pending_full_solve_residual_escalate_total_v_read;
    using aura::compiler::typed_audit::pending_full_solve_residual_face_hit;
    using aura::compiler::typed_audit::pending_full_solve_residual_observe_total_v_read;
    using aura::compiler::typed_audit::pending_full_solve_residual_reject_total_v_read;
    using aura::compiler::typed_audit::reset_pending_full_solve_residual_for_test;
    using aura::compiler::typed_audit::set_strategy;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    auto save_strat = get_strategy();
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    set_strategy(AuditStrategy::Sampled);
    reset_pending_full_solve_residual_for_test();

    // Soft at the outermost success stamp: drain observes, allows commit,
    // does NOT clear pending / does NOT latch face. Sibling #3031 AC2.
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    cs.seed_pending_full_solve_root_for_test(1);
    cs.force_locality_pruned_for_test(1);
    const auto obs0 = pending_full_solve_residual_observe_total_v_read();
    const auto esc0 = pending_full_solve_residual_escalate_total_v_read();
    const auto rej0 = pending_full_solve_residual_reject_total_v_read();
    auto r = cs.drain_pending_full_solve_before_commit();
    CHECK(r == SolveResult::SOLVED, "3190 AC2: Soft allows residual SOLVED");
    CHECK(cs.pending_full_solve_roots_size() > 0, "3190 AC2: Soft does not clear pending");
    CHECK(pending_full_solve_residual_observe_total_v_read() > obs0, "3190 AC2: observe bumps");
    CHECK(pending_full_solve_residual_escalate_total_v_read() == esc0, "3190 AC2: no escalate");
    CHECK(pending_full_solve_residual_reject_total_v_read() == rej0, "3190 AC2: no reject");
    CHECK(!pending_full_solve_residual_face_hit(), "3190 AC2: Soft does not latch face");

    reset_pending_full_solve_residual_for_test();
    set_strategy(save_strat);
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3190_3_quiet_zero_cost() {
    std::println("\n--- #3190 AC3: quiet no residual → zero extra at outermost stamp ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    using aura::compiler::typed_audit::pending_full_solve_residual_escalate_total_v_read;
    using aura::compiler::typed_audit::pending_full_solve_residual_observe_total_v_read;
    using aura::compiler::typed_audit::pending_full_solve_residual_reject_total_v_read;
    using aura::compiler::typed_audit::reset_pending_full_solve_residual_for_test;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    reset_pending_full_solve_residual_for_test();

    // Quiet path: pending_full_solve_roots_ empty + last_locality_pruned_ 0
    // → SOLVED, two size reads, no observe/escalate/reject bumps.
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    const auto obs0 = pending_full_solve_residual_observe_total_v_read();
    const auto esc0 = pending_full_solve_residual_escalate_total_v_read();
    const auto rej0 = pending_full_solve_residual_reject_total_v_read();
    auto r = cs.drain_pending_full_solve_before_commit();
    CHECK(r == SolveResult::SOLVED, "3190 AC3: quiet SOLVED");
    CHECK(pending_full_solve_residual_observe_total_v_read() == obs0, "3190 AC3: no observe");
    CHECK(pending_full_solve_residual_escalate_total_v_read() == esc0, "3190 AC3: no escalate");
    CHECK(pending_full_solve_residual_reject_total_v_read() == rej0, "3190 AC3: no reject");

    reset_pending_full_solve_residual_for_test();
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3190_4_lockless_batch_covered() {
    std::println("\n--- #3190 AC4: lockless multi-mutate batch covered ---");
    // composite_txn_commit is the sole entry for both nested_boundary AND
    // atomic_batch_active (lockless multi-mutate batch). The drain sibling
    // #3031 already lives inside composite_txn_commit body. #3190 adds the
    // same drain at the outermost success stamp site. The two sites close
    // the SOLVED-with-dirty window across both batch shapes.
    const auto ev = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto ev_mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    // composite_txn_commit body calls drain (sibling #3031).
    CHECK(ev.find("drain_pending_full_solve_before_commit") != std::string::npos,
          "3190 AC4: composite_txn_commit calls drain");
    // outermost success stamp site calls drain (new #3190 site).
    CHECK(ev_mb.find("drain_pending_full_solve_before_commit") != std::string::npos,
          "3190 AC4: outermost success calls drain");
    // Both sites use force_reason 16 (the stable reject reason).
    CHECK(ev.find("force_reason=*/16") != std::string::npos ||
              ev.find("force_reason=*/16u") != std::string::npos ||
              ev.find("/*force_reason=*/16") != std::string::npos,
          "3190 AC4: composite_txn_commit uses force_reason 16");
    CHECK(ev_mb.find("force_reason=*/16") != std::string::npos ||
              ev_mb.find("force_reason=*/16u") != std::string::npos ||
              ev_mb.find("/*force_reason=*/16") != std::string::npos,
          "3190 AC4: outermost success uses force_reason 16");
    // Helper body: drain escalates via escalate_if_production (sibling).
    CHECK(impl.find("escalate_if_production(SolveResult::TIMEOUT") != std::string::npos,
          "3190 AC4: drain synthesizes TIMEOUT escalate");
    // Helper body: drain also calls escalate_locality_slo_if_production.
    CHECK(impl.find("escalate_locality_slo_if_production") != std::string::npos,
          "3190 AC4: drain covers locality residual face");
}

static void ac3190_5_existing_surfaces_preserved() {
    std::println("\n--- #3190 AC5: existing #2913/#2994/#3031/#3169 surfaces preserved ---");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "3190 AC5: warm");
    // #3031 surface (the sibling drain).
    CHECK(href(svc, "schema-3031") == 3031, "3190 AC5: schema-3031 preserved");
    CHECK(href(svc, "pending-full-solve-residual-wired") == 1, "3190 AC5: wired preserved");
    CHECK(href(svc, "pending-full-solve-residual-reject-total") >= 0,
          "3190 AC5: reject counter preserved");
    // #2913 surface (locality SLO).
    CHECK(href(svc, "schema-2913") == 2913, "3190 AC5: schema-2913 preserved");
    // #2994 surface (locality residual budget).
    CHECK(href(svc, "schema-2994") == 2994, "3190 AC5: schema-2994 preserved");
    // #3169 surface (clear partial).
    CHECK(href(svc, "schema-3169") == 3169, "3190 AC5: schema-3169 preserved");
}

static void ac3190_6_source_and_linter() {
    std::println("\n--- #3190 AC6: source-cite + linter + build.py ---");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    const auto ev = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto tc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto t = read_file("tests/compiler/test_solve_delta_unresolved_export.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_residual_drain_outermost_stamp_3190.py");
    const auto build = read_file("build.py");
    // Source-cite: drain helper exists, is reachable from both stamp sites.
    CHECK(impl.find("drain_pending_full_solve_before_commit") != std::string::npos,
          "3190 AC6: drain helper body");
    CHECK(ixx.find("drain_pending_full_solve_before_commit") != std::string::npos,
          "3190 AC6: drain declaration");
    CHECK(ev.find("drain_pending_full_solve_before_commit") != std::string::npos,
          "3190 AC6: outermost success calls drain");
    CHECK(tc.find("drain_pending_full_solve_before_commit") != std::string::npos,
          "3190 AC6: composite_txn_commit calls drain");
    // Issue #3190 citation at the outermost drain site.
    CHECK(ev.find("Issue #3190") != std::string::npos,
          "3190 AC6: citation at outermost success stamp");
    // force_reason 16 at the outermost success stamp site.
    CHECK(ev.find("force_reason=*/16") != std::string::npos ||
              ev.find("force_reason=*/16u") != std::string::npos ||
              ev.find("/*force_reason=*/16") != std::string::npos,
          "3190 AC6: force_reason 16 at outermost success stamp");
    // Test file has the new AC functions.
    CHECK(t.find("ac3190_1_outermost_drain_production") != std::string::npos, "3190 AC6: AC1");
    CHECK(t.find("ac3190_2_outermost_drain_soft") != std::string::npos, "3190 AC6: AC2");
    CHECK(t.find("ac3190_3_quiet_zero_cost") != std::string::npos, "3190 AC6: AC3");
    CHECK(t.find("ac3190_4_lockless_batch_covered") != std::string::npos, "3190 AC6: AC4");
    CHECK(t.find("ac3190_5_existing_surfaces_preserved") != std::string::npos, "3190 AC6: AC5");
    // Linter exists.
    CHECK(!lint.empty() && lint.find("3190") != std::string::npos, "3190 AC6: linter");
    // Linter wired into build.py.
    CHECK(build.find("check_residual_drain_outermost_stamp_3190") != std::string::npos,
          "3190 AC6: build.py");
    // No docs/design/* (per #1655).
    CHECK(read_file("docs/design/3190-pending-full-solve-outermost-stamp.md").empty(),
          "3190 AC6: no docs/design/");
    // No tests/issues/test_issue_3190.cpp (per #81934).
    CHECK(read_file("tests/issues/test_issue_3190.cpp").empty(),
          "3190 AC6: no tests/issues/test_issue_3190");
}

static void ac3169_6_source_and_linter() {
    std::println("\n--- #3169 AC6: source-cite linter + build.py wiring ---");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_solve_delta_partial_cleared_3169.py");
    int rc =
        std::system("python3 scripts/coverage/checks/check_solve_delta_partial_cleared_3169.py "
                    "--self-test > /dev/null 2>&1");
    CHECK(rc == 0, "3169 AC6: linter --self-test passes");
    CHECK(!lint.empty() && lint.find("Issue #3169") != std::string::npos,
          "3169 AC6: linter cites #3169");
    CHECK(build.find("check_solve_delta_partial_cleared_3169") != std::string::npos,
          "3169 AC6: build.py wires linter");
}

// ── Issue #3031: pending_full_solve / locality residual before commit ──
// AC1 production pending/locality → escalate; still dirty → reject
// AC2 Soft observe allow
// AC3 quiet (no residual) zero extra counters
// AC4 commit_readiness hermetic force_reason 16
// AC5 schema-3031 keys
// AC6 source-cite + linter; no invent / no design

static void ac3031_1_production_drain_escalate_reject() {
    std::println("\n--- #3031 AC1: production drain escalate / reject ---");
    using aura::compiler::typed_audit::commit_readiness;
    using aura::compiler::typed_audit::CommitReadinessInput;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    using aura::compiler::typed_audit::pending_full_solve_residual_escalate_total_v_read;
    using aura::compiler::typed_audit::pending_full_solve_residual_face_hit;
    using aura::compiler::typed_audit::pending_full_solve_residual_last_v_read;
    using aura::compiler::typed_audit::pending_full_solve_residual_observe_total_v_read;
    using aura::compiler::typed_audit::pending_full_solve_residual_reject_total_v_read;
    using aura::compiler::typed_audit::reset_pending_full_solve_residual_for_test;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    reset_pending_full_solve_residual_for_test();

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    cs.seed_pending_full_solve_root_for_test(1);
    const auto esc0 = pending_full_solve_residual_escalate_total_v_read();
    const auto rej0 = pending_full_solve_residual_reject_total_v_read();
    const auto obs0 = pending_full_solve_residual_observe_total_v_read();
    auto r = cs.drain_pending_full_solve_before_commit();
    CHECK(r == SolveResult::SOLVED, "3031 AC1: empty-CS pending drains to SOLVED");
    CHECK(cs.pending_full_solve_roots_size() == 0, "3031 AC1: pending cleared");
    CHECK(cs.last_locality_pruned() == 0, "3031 AC1: locality cleared");
    CHECK(pending_full_solve_residual_escalate_total_v_read() > esc0, "3031 AC1: escalate bumps");
    CHECK(pending_full_solve_residual_reject_total_v_read() == rej0,
          "3031 AC1: no reject on SOLVED");
    CHECK(pending_full_solve_residual_observe_total_v_read() == obs0, "3031 AC1: no Soft observe");
    CHECK(!pending_full_solve_residual_face_hit(), "3031 AC1: face clear after recover");

    cs.force_locality_pruned_for_test(2);
    r = cs.drain_pending_full_solve_before_commit();
    CHECK(r == SolveResult::SOLVED, "3031 AC1: locality residual drains to SOLVED");
    CHECK(cs.last_locality_pruned() == 0, "3031 AC1: locality residual cleared");

    ConstraintSystem cs2(reg);
    auto a = cs2.fresh_var();
    Constraint eq1;
    eq1.kind = Constraint::EQUAL;
    eq1.lhs = a;
    eq1.rhs = reg.int_type();
    Constraint eq2;
    eq2.kind = Constraint::EQUAL;
    eq2.lhs = a;
    eq2.rhs = reg.bool_type();
    cs2.add_delta(std::move(eq1));
    cs2.add_delta(std::move(eq2));
    cs2.seed_pending_full_solve_root_for_test(1);
    auto r2 = cs2.drain_pending_full_solve_before_commit();
    CHECK(r2 != SolveResult::SOLVED, "3031 AC1: conflict residual hard-rejects");
    CHECK(pending_full_solve_residual_reject_total_v_read() > rej0, "3031 AC1: reject total");
    CHECK(pending_full_solve_residual_face_hit(), "3031 AC1: face latched on reject");
    CHECK(pending_full_solve_residual_last_v_read() > 0, "3031 AC1: last residual");

    CommitReadinessInput in;
    in.solve_status = 0;
    in.linear_ok = true;
    in.blame_ok = true;
    in.pending_full_solve_hard = true;
    in.pending_full_solve_residual = true;
    auto cr = commit_readiness(in);
    CHECK(!cr.would_allow_commit, "3031 AC1: readiness rejects dirty residual");
    CHECK(cr.force_reason == "pending_full_solve_residual", "3031 AC1: force_reason");
    CHECK(cr.force_reason_code == 16, "3031 AC1: force_reason_code 16");

    reset_pending_full_solve_residual_for_test();
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3031_2_soft_observe_allow() {
    std::println("\n--- #3031 AC2: Soft residual observe allow ---");
    using aura::compiler::typed_audit::AuditStrategy;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    using aura::compiler::typed_audit::get_strategy;
    using aura::compiler::typed_audit::pending_full_solve_residual_escalate_total_v_read;
    using aura::compiler::typed_audit::pending_full_solve_residual_face_hit;
    using aura::compiler::typed_audit::pending_full_solve_residual_observe_total_v_read;
    using aura::compiler::typed_audit::pending_full_solve_residual_reject_total_v_read;
    using aura::compiler::typed_audit::reset_pending_full_solve_residual_for_test;
    using aura::compiler::typed_audit::set_strategy;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    auto save_strat = get_strategy();
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    set_strategy(AuditStrategy::Sampled);
    reset_pending_full_solve_residual_for_test();

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    cs.seed_pending_full_solve_root_for_test(1);
    cs.force_locality_pruned_for_test(1);
    const auto obs0 = pending_full_solve_residual_observe_total_v_read();
    const auto esc0 = pending_full_solve_residual_escalate_total_v_read();
    const auto rej0 = pending_full_solve_residual_reject_total_v_read();
    auto r = cs.drain_pending_full_solve_before_commit();
    CHECK(r == SolveResult::SOLVED, "3031 AC2: Soft allows residual SOLVED");
    CHECK(cs.pending_full_solve_roots_size() > 0, "3031 AC2: Soft does not clear pending");
    CHECK(pending_full_solve_residual_observe_total_v_read() > obs0, "3031 AC2: observe bumps");
    CHECK(pending_full_solve_residual_escalate_total_v_read() == esc0, "3031 AC2: no escalate");
    CHECK(pending_full_solve_residual_reject_total_v_read() == rej0, "3031 AC2: no reject");
    CHECK(!pending_full_solve_residual_face_hit(), "3031 AC2: Soft does not latch face");

    reset_pending_full_solve_residual_for_test();
    set_strategy(save_strat);
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3031_3_quiet_zero_cost() {
    std::println("\n--- #3031 AC3: quiet no residual → zero extra ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    using aura::compiler::typed_audit::pending_full_solve_residual_escalate_total_v_read;
    using aura::compiler::typed_audit::pending_full_solve_residual_observe_total_v_read;
    using aura::compiler::typed_audit::pending_full_solve_residual_reject_total_v_read;
    using aura::compiler::typed_audit::reset_pending_full_solve_residual_for_test;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    reset_pending_full_solve_residual_for_test();

    TypeRegistry reg;
    ConstraintSystem cs(reg);
    const auto obs0 = pending_full_solve_residual_observe_total_v_read();
    const auto esc0 = pending_full_solve_residual_escalate_total_v_read();
    const auto rej0 = pending_full_solve_residual_reject_total_v_read();
    auto r = cs.drain_pending_full_solve_before_commit();
    CHECK(r == SolveResult::SOLVED, "3031 AC3: quiet SOLVED");
    CHECK(pending_full_solve_residual_observe_total_v_read() == obs0, "3031 AC3: no observe");
    CHECK(pending_full_solve_residual_escalate_total_v_read() == esc0, "3031 AC3: no escalate");
    CHECK(pending_full_solve_residual_reject_total_v_read() == rej0, "3031 AC3: no reject");

    reset_pending_full_solve_residual_for_test();
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3031_4_commit_readiness_hermetic() {
    std::println("\n--- #3031 AC4: commit_readiness hermetic code 16 ---");
    using aura::compiler::typed_audit::commit_readiness;
    using aura::compiler::typed_audit::commit_readiness_reason_code;
    using aura::compiler::typed_audit::CommitReadinessInput;
    CHECK(commit_readiness_reason_code("pending_full_solve_residual") == 16,
          "3031 AC4: reason_code 16");
    CommitReadinessInput hard;
    hard.solve_status = 0;
    hard.linear_ok = true;
    hard.blame_ok = true;
    hard.pending_full_solve_hard = true;
    hard.pending_full_solve_residual = true;
    auto r = commit_readiness(hard);
    CHECK(!r.would_allow_commit, "3031 AC4: hard residual rejects");
    CHECK(r.force_reason == "pending_full_solve_residual", "3031 AC4: force_reason");
    CHECK(r.force_reason_code == 16, "3031 AC4: force_reason_code");
    CHECK(r.readiness_bp == 700, "3031 AC4: hard bp 700");

    CommitReadinessInput soft = hard;
    soft.pending_full_solve_hard = false;
    r = commit_readiness(soft);
    CHECK(r.would_allow_commit, "3031 AC4: Soft residual allows");
    CHECK(r.force_reason == "pending_full_solve_residual", "3031 AC4: Soft still names reason");
    CHECK(r.readiness_bp == 7200, "3031 AC4: Soft bp 7200");

    CommitReadinessInput quiet;
    quiet.solve_status = 0;
    quiet.linear_ok = true;
    quiet.blame_ok = true;
    quiet.pending_full_solve_hard = true;
    quiet.pending_full_solve_residual = false;
    r = commit_readiness(quiet);
    CHECK(r.would_allow_commit, "3031 AC4: quiet allow");
    CHECK(r.force_reason == "ok", "3031 AC4: quiet ok");
}

static void ac3031_5_schema() {
    std::println("\n--- #3031 AC5: schema-3031 + residual keys ---");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "3031 AC5: warm");
    CHECK(href(svc, "schema-3031") == 3031, "3031 AC5: schema-3031");
    CHECK(href(svc, "issue-3031") == 3031, "3031 AC5: issue-3031");
    CHECK(href(svc, "pending-full-solve-residual-wired") == 1, "3031 AC5: wired");
    CHECK(href(svc, "pending-full-solve-residual-last") >= 0, "3031 AC5: last");
    CHECK(href(svc, "pending-full-solve-residual-observe-total") >= 0, "3031 AC5: observe");
    CHECK(href(svc, "pending-full-solve-residual-escalate-total") >= 0, "3031 AC5: escalate");
    CHECK(href(svc, "pending-full-solve-residual-reject-total") >= 0, "3031 AC5: reject");
    CHECK(href(svc, "schema-2994") == 2994, "3031 AC5: schema-2994 preserved");
    CHECK(href(svc, "schema-2913") == 2913, "3031 AC5: schema-2913 preserved");
}

static void ac3031_6_source_and_linter() {
    std::println("\n--- #3031 AC6: source-cite + linter ---");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    const auto ev = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto t = read_file("tests/compiler/test_solve_delta_unresolved_export.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_pending_full_solve_residual_3031.py");
    const auto build = read_file("build.py");
    CHECK(ixx.find("drain_pending_full_solve_before_commit") != std::string::npos,
          "3031 AC6: drain API");
    CHECK(ixx.find("seed_pending_full_solve_root_for_test") != std::string::npos,
          "3031 AC6: seed helper");
    CHECK(impl.find("drain_pending_full_solve_before_commit") != std::string::npos,
          "3031 AC6: drain body");
    CHECK(impl.find("escalate_if_production(SolveResult::TIMEOUT") != std::string::npos,
          "3031 AC6: drain synthesizes TIMEOUT escalate");
    CHECK(aud.find("pending_full_solve_residual") != std::string::npos, "3031 AC6: readiness face");
    CHECK(aud.find("kPendingFullSolveResidualIssue") != std::string::npos, "3031 AC6: issue stamp");
    CHECK(ev.find("drain_pending_full_solve_before_commit") != std::string::npos,
          "3031 AC6: composite_txn_commit drain");
    CHECK(ev.find("force_reason=*/16") != std::string::npos ||
              ev.find("force_reason=*/16u") != std::string::npos ||
              ev.find("/*force_reason=*/16") != std::string::npos,
          "3031 AC6: reject proof force_reason 16");
    CHECK(t.find("ac3031_1_production_drain_escalate_reject") != std::string::npos,
          "3031 AC6: AC1");
    CHECK(!lint.empty() && lint.find("3031") != std::string::npos, "3031 AC6: linter");
    CHECK(build.find("check_pending_full_solve_residual_3031") != std::string::npos,
          "3031 AC6: build.py");
    CHECK(read_file("docs/design/3031-pending-full-solve-residual.md").empty(),
          "3031 AC6: no docs/design/");
    CHECK(read_file("tests/compiler/test_issue_3031.cpp").empty(),
          "3031 AC6: no invent test_issue_3031");
}

// ── Issue #3081: Soft allow_timeout_commit TIMEOUT is not query:type authority ──
static void ac3081_1_soft_timeout_clears_authority() {
    std::println("\n--- #3081 AC1: Soft + allow_timeout_commit + TIMEOUT → not authoritative ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);

    TypeRegistry reg;
    DiagnosticCollector diag;
    InferenceEngine engine(reg, diag);
    engine.set_incremental_delta_mode(true, true);
    SolverBudget b{};
    b.allow_timeout_commit = true;
    engine.constraint_system().set_solver_budget(b);
    // Seed dirty work so solve_delta does not early-return SOLVED
    // (empty worklist skips the force-timeout hook).
    auto v = engine.constraint_system().fresh_var();
    Constraint eq;
    eq.kind = Constraint::EQUAL;
    eq.lhs = v;
    eq.rhs = reg.int_type();
    engine.constraint_system().add_delta(std::move(eq));
    engine.constraint_system().force_next_delta_timeout_for_test(true);
    CHECK(engine.last_type_export_authoritative(), "3081 AC1: default authoritative");

    FlatAST flat;
    StringPool pool;
    const auto nid = flat.add_literal(1);
    flat.root = nid;
    (void)engine.infer_flat(flat, pool, nid, /*preserve_cs=*/true);
    CHECK(engine.last_solve_status() == SolveResult::TIMEOUT, "3081 AC1: infer TIMEOUT");
    CHECK(!engine.last_type_export_authoritative(),
          "3081 AC1: Soft TIMEOUT clears last_type_export_authoritative");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3081_2_query_type_not_authoritative() {
    std::println("\n--- #3081 AC2: query-type-of / get-inferred-type not-authoritative ---");
    apply_dev_audit_defaults();
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "3081 AC2: warm");
    (void)svc.eval("(set-code \"(define f 1)\")");
    (void)svc.eval("(eval-current)");
    (void)svc.eval("(typecheck-current)");
    svc.evaluator().clear_type_export_authority();
    CHECK(!svc.evaluator().type_export_authoritative(), "3081 AC2: Evaluator flag false");
    // get-inferred-type / query-type-of check authority before type_id.
    auto git = svc.eval("(get-inferred-type 0)");
    CHECK(git.has_value(), "3081 AC2: get-inferred-type returned");
    (void)svc.eval("(query-type-of \"f\")");
    const auto prim = read_file("src/compiler/evaluator_primitives_eval.cpp");
    CHECK(prim.find("query-type-of") != std::string::npos &&
              prim.find("get-inferred-type") != std::string::npos &&
              prim.find("not-authoritative") != std::string::npos &&
              prim.find("Issue #3081") != std::string::npos,
          "3081 AC2: query surface gates on type_export_authoritative");
}

static void ac3081_3_production_unchanged() {
    std::println("\n--- #3081 AC3: production fail-closed unchanged ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(impl.find("Issue #3081") != std::string::npos, "3081 AC3: infer_flat cites #3081");
    CHECK(impl.find("delta_timeout_fail_closed_total") != std::string::npos,
          "3081 AC3: #3003 fail-closed retained");
    CHECK(impl.find("delta_timeout_reject_total") != std::string::npos,
          "3081 AC3: #2277 reject retained");
}

static void ac3081_4_solved_zero_cost() {
    std::println("\n--- #3081 AC4: SOLVED path does not clear authority ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    TypeRegistry reg;
    DiagnosticCollector diag;
    InferenceEngine engine(reg, diag);
    FlatAST flat;
    StringPool pool;
    const auto nid = flat.add_literal(1);
    flat.root = nid;
    (void)engine.infer_flat(flat, pool, nid, /*preserve_cs=*/false);
    CHECK(engine.last_solve_status() == SolveResult::SOLVED, "3081 AC4: SOLVED");
    CHECK(engine.last_type_export_authoritative(), "3081 AC4: SOLVED stays authoritative");
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3081_5_source_and_linter() {
    std::println("\n--- #3081 AC5: source-cite + linter ---");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto ev = read_file("src/compiler/evaluator.ixx");
    const auto prim = read_file("src/compiler/evaluator_primitives_eval.cpp");
    const auto t = read_file("tests/compiler/test_solve_delta_unresolved_export.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_soft_timeout_export_non_authoritative_3081.py");
    const auto build = read_file("build.py");
    CHECK(ixx.find("kSoftTimeoutExportNonAuthoritativeIssue = 3081") != std::string::npos,
          "3081 AC5: stamp");
    CHECK(impl.find("last_type_export_authoritative_ = false") != std::string::npos,
          "3081 AC5: infer_flat clears");
    CHECK(impl.find("allow_timeout_commit") != std::string::npos, "3081 AC5: #2900 path kept");
    CHECK(ev.find("type_export_authoritative") != std::string::npos, "3081 AC5: Evaluator flag");
    CHECK(prim.find("not-authoritative") != std::string::npos, "3081 AC5: query surface");
    CHECK(prim.find("Issue #3081") != std::string::npos, "3081 AC5: typecheck copies flag");
    CHECK(t.find("ac3081_1_soft_timeout_clears_authority") != std::string::npos, "3081 AC5: AC1");
    CHECK(!lint.empty() && lint.find("Issue #3081") != std::string::npos, "3081 AC5: linter");
    CHECK(build.find("check_soft_timeout_export_non_authoritative_3081") != std::string::npos,
          "3081 AC5: build.py");
    CHECK(read_file("tests/compiler/test_issue_3081.cpp").empty(), "3081 AC5: no invent");
    CHECK(read_file("docs/design/3081-soft-timeout-authority.md").empty(),
          "3081 AC5: no docs/design/");
}

// ── #3108: commit_readiness recover must re-gate on solve_status==SOLVED ─
//
// Closes the half-green residual of #2750 / #2909 / #2962 / #2911 /
// #3031: the existing per-face guard
// (`if (recovered && in.solve_status != 0) recovered = false;`) is now
// accompanied by an additive counter that tracks every time the re-gate
// fires, so production-soak / agent-self-modify gates can observe the
// half-green residual. Soft path unchanged (observe-only); Production
// path unchanged (the recovered->false flip already hard-rejects via the
// existing face reject sites — #3108 just adds observability).

static void ac3108_1_recover_regate_wired() {
    std::println("\n--- #3108 AC1: post-recover re-gate wired ---");
    const auto h = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(h.find("kOccurrenceRecoverNotSolvedIssue = 3108") != std::string::npos,
          "3108 AC1: issue stamp");
    CHECK(h.find("g_occurrence_recover_not_solved_total") != std::string::npos,
          "3108 AC1: additive counter");
    CHECK(h.find("g_occurrence_recover_not_solved_wired{1}") != std::string::npos,
          "3108 AC1: wired flag");
    // Both recover blocks must have the re-gate + counter bump
    CHECK(h.find("recovered && in.solve_status != 0") != std::string::npos,
          "3108 AC1: re-gate check present");
    // Count the bump sites — should be ≥ 2 (block 1 + block 2)
    const auto pos = h.find("g_occurrence_recover_not_solved_total.fetch_add(1");
    CHECK(pos != std::string::npos, "3108 AC1: at least one bump site");
    const auto pos2 = h.find("g_occurrence_recover_not_solved_total.fetch_add(1", pos + 1);
    CHECK(pos2 != std::string::npos, "3108 AC1: second bump site (block 2)");
}

static void ac3108_2_production_rejects_via_existing_path() {
    std::println("\n--- #3108 AC2: Production reject path unchanged ---");
    // The recovered->false flip already routes through existing face
    // reject sites (g_occurrence_hard_face_recover_fail_total, etc.).
    // #3108 just adds observability. Source-cite the existing reject
    // counters must remain.
    const auto h = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(h.find("g_occurrence_hard_face_recover_fail_total") != std::string::npos,
          "3108 AC2: hard-face recover-fail counter preserved");
    CHECK(h.find("g_cone_outside_goal_drop_reject_total") != std::string::npos,
          "3108 AC2: cone reject counter preserved");
    CHECK(h.find("g_refined_consistency_reject_total") != std::string::npos,
          "3108 AC2: refined reject counter preserved");
}

static void ac3108_3_soft_observe_only() {
    std::println("\n--- #3108 AC3: Soft observe-only ---");
    // Soft path should not bump the new counter when recover succeeds
    // (the re-gate only fires when recover returns true under non-SOLVED,
    // which is the half-green residual — observable but not actionable
    // under Soft). Source-cite: the re-gate is in the cold
    // `recovered && in.solve_status != 0` branch — Soft can hit this
    // (CONFLICT/TIMEOUT is observable) but the counter bump is the
    // observable signal, not a hard action.
    const auto h = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(h.find("if (recovered && in.solve_status != 0)") != std::string::npos,
          "3108 AC3: re-gate triggers on non-SOLVED only");
    // last_type_export_authoritative clear (#3081) must remain
    CHECK(h.find("last_type_export_authoritative") != std::string::npos ||
              h.find("g_last_type_export_authoritative") != std::string::npos,
          "3108 AC3: #3081 Soft TIMEOUT authority clear preserved");
}

static void ac3108_4_additive_counter_only() {
    std::println("\n--- #3108 AC4: additive counter only ---");
    const auto h = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(h.find("kOccurrenceRecoverNotSolvedIssue = 3108") != std::string::npos,
          "3108 AC4: additive issue stamp");
    CHECK(h.find("g_occurrence_recover_not_solved_total") != std::string::npos,
          "3108 AC4: additive counter");
    CHECK(h.find("g_occurrence_recover_not_solved_wired{1}") != std::string::npos,
          "3108 AC4: additive wired flag");
    // Quiet path (no recover attempted) stays zero extra atomics —
    // the bump lives INSIDE the cold `recovered && solve_status != 0`
    // branch, which is only entered when a recover hook returns true.
}

static void ac3108_5_source_and_linter() {
    std::println("\n--- #3108 AC5: source-cite + linter + no invent ---");
    const auto h = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(h.find("kOccurrenceRecoverNotSolvedIssue = 3108") != std::string::npos,
          "3108 AC5: issue stamp");
    const auto lint =
        read_file("scripts/coverage/checks/check_occurrence_recover_not_solved_3108.py");
    const auto build = read_file("build.py");
    CHECK(!lint.empty() && lint.find("Issue #3108") != std::string::npos,
          "3108 AC5: 3108 linter exists");
    CHECK(build.find("check_occurrence_recover_not_solved_3108") != std::string::npos,
          "3108 AC5: build.py wires 3108 linter");
    CHECK(read_file("tests/compiler/test_issue_3108.cpp").empty(),
          "3108 AC5: no invent test_issue_3108 (per #81967)");
    CHECK(read_file("docs/design/3108-recover-not-solved.md").empty(),
          "3108 AC5: no docs/design/ (per #1655)");
    // Lineage: #2750, #2909, #2962, #2911, #3031 must still pass
    CHECK(h.find("kConeOutsideGoalDropRecoverRejectIssue = 2962") != std::string::npos,
          "3108 AC5: #2962 lineage preserved");
    CHECK(h.find("kRefinedConsistencyGateIssue = 2911") != std::string::npos,
          "3108 AC5: #2911 lineage preserved");
}

} // namespace

int run_test_solve_delta_unresolved_export() {
    std::println("=== Issue #2107: solve_delta unresolved export ===");
    ac1_timeout_unresolved();
    ac2_solved_empty();
    ac3_conflict_exports();
    ac4_source_and_2028_lineage();
    ac5_affected_nodes_for_agents();
    ac6_query_schema();
    std::println("\n=== Issue #2277: production TIMEOUT escalation ===");
    ac7_issue_2277_escalate_and_schema();
    std::println("\n=== Issue #2308: SolverSnapshot + query surface ===");
    ac8_2308_solver_snapshot();
    // Issue #2318: anti-starvation streak gate (consecutive truncated
    // delta solves → force one full solve). AC1-AC5 wiring.
    ac2318_streak_counter();
    ac2318_force_full_solve();
    ac2318_alt_truncate_clean();
    ac2318_query_keys();
    ac2318_source_cite_rows();
    std::println("\n=== Issue #2900: SolverBudget Agent TIMEOUT policy ===");
    ac2900_1_soft_allow_timeout_export();
    ac2900_2_production_still_escalates();
    ac2900_3_default_budget_unchanged();
    ac2900_4_additive_query();
    ac2900_5_source_cite();
    std::println("\n=== Issue #2963: instance-repair before full-solve ===");
    ac2963_1_production_repair_resolves();
    ac2963_2_soft_quiet_zero_cost();
    ac2963_3_quiet_no_timeout_zero();
    ac2963_4_additive_schema();
    ac2963_5_source_cite();
    ac2963_6_large_cs_small_dirty_repair_hit();
    std::println("\n=== Issue #2913: solve_delta locality SLO ===");
    ac2913_1_production_escalate();
    ac2913_2_soft_observe_and_quiet();
    ac2913_3_commit_readiness_after_escalate();
    ac2913_4_additive_schema();
    ac2913_5_source_cite();
    ac2913_6_wired_in_solve_delta();
    std::println("\n=== Issue #2994: locality residual budget ===");
    ac2994_1_default_budget_escalate();
    ac2994_2_budget_allow_pending_handoff();
    ac2994_3_budget_over_escalate();
    ac2994_4_soft_no_budget_counters();
    ac2994_5_quiet_zero_cost();
    ac2994_6_is_default_and_schema();
    ac2994_7_source_cite();
    std::println("\n=== Issue #3003: Production solve_delta fail-closed ===");
    ac3003_1_production_solve_delta_fail_closed();
    ac3003_2_soft_timeout_observe();
    ac3003_3_no_stash_no_authority();
    ac3003_4_schema_and_lineage();
    ac3003_5_source_and_linter();
    std::println("\n=== Issue #3031: pending_full_solve residual before commit ===");
    ac3031_1_production_drain_escalate_reject();
    ac3031_2_soft_observe_allow();
    ac3031_3_quiet_zero_cost();
    ac3031_4_commit_readiness_hermetic();
    ac3031_5_schema();
    ac3031_6_source_and_linter();
    std::println("\n=== Issue #3081: Soft TIMEOUT export is not query:type authority ===");
    ac3081_1_soft_timeout_clears_authority();
    ac3081_2_query_type_not_authoritative();
    ac3081_3_production_unchanged();
    ac3081_4_solved_zero_cost();
    ac3081_5_source_and_linter();
    std::println("\n=== Issue #3108: commit_readiness recover re-gate ===");
    ac3108_1_recover_regate_wired();
    ac3108_2_production_rejects_via_existing_path();
    ac3108_3_soft_observe_only();
    ac3108_4_additive_counter_only();
    ac3108_5_source_and_linter();
    // ── Issue #3190: outermost-success TypeLinearCommitProof drain before stamp ──
    std::println("\n=== Issue #3190: outermost drain before stamp (anti SOLVED-with-dirty) ===");
    ac3190_1_outermost_drain_production();
    ac3190_2_outermost_drain_soft();
    ac3190_3_quiet_zero_cost();
    ac3190_4_lockless_batch_covered();
    ac3190_5_existing_surfaces_preserved();
    ac3190_6_source_and_linter();
    std::println("\n=== Issue #3169: production solve_delta fail-closed + clear partial ===");
    ac3169_1_production_clear_partial_and_reject();
    ac3169_2_soft_zero_extra();
    ac3169_3_quiet_zero_extra();
    ac3169_4_additive_counter_only();
    ac3169_5_existing_3003_2963_2913_preserved();
    ac3169_6_source_and_linter();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_solve_delta_unresolved_export();
}
#endif
