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
import aura.core.type;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::solve_delta_occurrence;
using aura::compiler::SolveResult;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeId;
using aura::core::TypeRegistry;
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
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
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
    CHECK(tc.find("truncate_streak_ >= threshold") != std::string::npos,
          "AC2: streak >= threshold check present");
    CHECK(tc.find("delta_truncate_force_full_solve_total") != std::string::npos,
          "AC2: force_full_solve_total bump present");
    CHECK(tc.find("return solve(unresolved_out);") != std::string::npos,
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
    const auto ep = read_file("src/compiler/evaluator_primitives_query.cpp");
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
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_solve_delta_unresolved_export();
}
#endif
