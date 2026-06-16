// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_162.cpp — Issue #162 Phase 1: Type Concepts for
// the type system.
//
// Verifies:
//   1. concept TypeConstraint is defined and the two
//      concrete types (EqualConstraint, ConsistentConstraint)
//      satisfy it (compile-time check via static_assert)
//   2. solve_constraints<C> works for both concrete types
//   3. The concept is "tight" — a non-conforming type does
//      not satisfy it (also compile-time, via SFINAE test)
//   4. The new concept-based path is opt-in and does not
//      break the existing ConstraintSystem API
//
// Phase 1 scope: concepts + 2 concrete types + 1 free-
// function template. The full templated ConstraintSystem
// is deferred to Phase 2.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <span>
#include <type_traits>
#include <vector>

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;
import aura.core;
import aura.core.ast;
import aura.core.type;
import aura.diag;
import aura.compiler.type_checker;
import aura.compiler.type_concepts;



#define PRINTLN(msg) do { std::print("{}\n", std::string(msg)); } while(0)

// ── Test 1: concept TypeConstraint exists, both concrete types satisfy it
//
// The static_asserts in type_concepts.ixx already prove the
// two concrete types satisfy the concept at compile time.
// This test is a runtime witness that the module is built
// and the types are usable.
bool test_concept_satisfaction() {
    PRINTLN("\n--- Test 1: concept TypeConstraint satisfaction ---");
    aura::core::TypeRegistry reg;
    aura::diag::DiagnosticCollector diag;
    aura::compiler::ConstraintSystem cs(reg);
    (void)cs;  // silence unused warning if CHECK doesn't reference

    aura::compiler::EqualConstraint eq{};
    aura::compiler::ConsistentConstraint cn{};
    CHECK(eq.kind() == aura::compiler::CK_EQUAL, "EqualConstraint kind is EQUAL");
    CHECK(cn.kind() == aura::compiler::CK_CONSISTENT, "ConsistentConstraint kind is CONSISTENT");
    CHECK(static_cast<bool>(aura::compiler::TypeConstraint<aura::compiler::EqualConstraint>),
          "EqualConstraint satisfies TypeConstraint (via trait)");
    CHECK(static_cast<bool>(aura::compiler::TypeConstraint<aura::compiler::ConsistentConstraint>),
          "ConsistentConstraint satisfies TypeConstraint (via trait)");
    return true;
}

// ── Test 2: solve_constraints<C> works for EqualConstraint
//
// A trivial case: two equal type vars should unify cleanly.
// We use a real TypeRegistry and ConstraintSystem so the
// underlying Union-Find machinery actually runs.
bool test_solve_equal() {
    PRINTLN("\n--- Test 2: solve_constraints<EqualConstraint> ---");
    aura::core::TypeRegistry reg;
    aura::compiler::ConstraintSystem cs(reg);
    auto tv1 = cs.fresh_var();
    auto tv2 = cs.fresh_var();

    std::vector<aura::compiler::EqualConstraint> constraints;
    constraints.emplace_back(tv1, tv2);

    auto r = aura::compiler::solve_constraints(cs, std::span<const aura::compiler::EqualConstraint>(constraints));
    CHECK(r == aura::compiler::SolveResult::SOLVED,
          "EqualConstraint tv1=tv2 solves cleanly");
    // After unification, both vars should normalize to the same rep
    CHECK(cs.find(tv1) == cs.find(tv2),
          "EqualConstraint makes tv1 and tv2 equivalent in Union-Find");
    return true;
}

// ── Test 3: solve_constraints<C> works for ConsistentConstraint
//
// CONSISTENT is the gradual-typing constraint (any two types
// are consistent). It should always succeed (never return
// CONFLICT) as long as the types are valid.
bool test_solve_consistent() {
    PRINTLN("\n--- Test 3: solve_constraints<ConsistentConstraint> ---");
    aura::core::TypeRegistry reg;
    aura::compiler::ConstraintSystem cs(reg);
    auto int_t = reg.int_type();
    auto tv1 = cs.fresh_var();
    auto tv2 = cs.fresh_var();

    std::vector<aura::compiler::ConsistentConstraint> constraints;
    constraints.emplace_back(int_t, tv1);  // Int ~ tv1 (consistent)
    constraints.emplace_back(tv1, tv2);    // tv1 ~ tv2 (consistent)

    auto r = aura::compiler::solve_constraints(cs, std::span<const aura::compiler::ConsistentConstraint>(constraints));
    CHECK(r == aura::compiler::SolveResult::SOLVED,
          "ConsistentConstraint Int~tv1, tv1~tv2 solves cleanly");
    return true;
}

// ── Test 4: empty span is a no-op (returns SOLVED)
//
// The solver should handle empty input gracefully — no
// constraints means nothing to do, return SOLVED. This is
// important for incremental / lazy callers.
bool test_solve_empty() {
    PRINTLN("\n--- Test 4: solve_constraints on empty span ---");
    aura::core::TypeRegistry reg;
    aura::compiler::ConstraintSystem cs(reg);
    std::vector<aura::compiler::EqualConstraint> constraints;  // empty

    auto r = aura::compiler::solve_constraints(cs, std::span<const aura::compiler::EqualConstraint>(constraints));
    CHECK(r == aura::compiler::SolveResult::SOLVED,
          "solve_constraints on empty span returns SOLVED");
    return true;
}

// ── Test 5: type_traits check that a non-conforming type does NOT satisfy TypeConstraint
//
// A type that lacks the required members should NOT
// satisfy TypeConstraint. We use std::is_same_v to check
// the trait — false means the type fails the concept.
struct NotAConstraint {
    int x;  // no kind(), no unify(), no lhs()/rhs()
};

bool test_concept_rejection() {
    PRINTLN("\n--- Test 5: non-conforming type rejected ---");
    constexpr bool satisfies = aura::compiler::TypeConstraint<NotAConstraint>;
    CHECK(!satisfies, "NotAConstraint (missing required methods) does NOT satisfy TypeConstraint");
    return true;
}

// ── Test 6: backward compat — existing Constraint struct still works
//
// Phase 1 must not break the existing API. The original
// `Constraint` struct + `ConstraintSystem::add()` +
// `ConstraintSystem::solve()` should still be functional.
bool test_backward_compat() {
    PRINTLN("\n--- Test 6: backward compat with existing API ---");
    aura::core::TypeRegistry reg;
    aura::compiler::ConstraintSystem cs(reg);
    auto tv1 = cs.fresh_var();
    auto tv2 = cs.fresh_var();

    // Use the original API
    cs.add(aura::compiler::Constraint{aura::compiler::Constraint::EQUAL, tv1, tv2});
    auto r = cs.solve();
    CHECK(r == aura::compiler::SolveResult::SOLVED,
          "original Constraint + ConstraintSystem::add + solve still works");
    return true;
}

// ── Phase 2 tests ─────────────────────────────────────────────

// Test 7: InferenceRule concept — 3 concrete types satisfy it
//
// The static_asserts in type_concepts.ixx already prove the
// three concrete rules satisfy InferenceRule at compile time.
// This test is the runtime witness.
bool test_inference_rule_satisfaction() {
    PRINTLN("\n--- Test 7: InferenceRule concept satisfaction ---");
    CHECK(static_cast<bool>(aura::compiler::InferenceRule<aura::compiler::LiteralRule>),
          "LiteralRule satisfies InferenceRule (via trait)");
    CHECK(static_cast<bool>(aura::compiler::InferenceRule<aura::compiler::BoolRule>),
          "BoolRule satisfies InferenceRule (via trait)");
    CHECK(static_cast<bool>(aura::compiler::InferenceRule<aura::compiler::VarRule>),
          "VarRule satisfies InferenceRule (via trait)");
    return true;
}

// Test 8: LiteralRule — matches LiteralInt, returns int_type
//
// The simplest possible InferenceRule. The synthesize method
// doesn't touch the constraint system — it just returns a
// constant. Verifies the rule's matches() predicate and that
// infer_with_rules routes correctly.
bool test_literal_rule() {
    PRINTLN("\n--- Test 8: LiteralRule ---");
    aura::core::TypeRegistry reg;
    aura::compiler::ConstraintSystem cs(reg);
    aura::diag::SourceLocation loc{};
    aura::compiler::InferenceContext ctx{reg, cs, loc};

    aura::ast::NodeView v_int{};
    v_int.tag = aura::ast::NodeTag::LiteralInt;
    v_int.int_value = 42;
    v_int.marker = aura::ast::SyntaxMarker::User;

    aura::compiler::LiteralRule rule;
    CHECK(rule.matches(v_int), "LiteralRule matches LiteralInt");
    auto ty = rule.synthesize(ctx, v_int);
    CHECK(ty.index == reg.int_type().index, "LiteralRule returns int_type for LiteralInt");
    auto routed = aura::compiler::infer_with_rules(rule, ctx, v_int);
    CHECK(routed.index == reg.int_type().index, "infer_with_rules routes LiteralRule to int_type");
    return true;
}

// Test 9: VarRule — matches Variable, adds CONSISTENT constraint
//
// The first rule that touches the constraint system. After
// synthesize, ctx.cs should have at least one constraint
// (the CONSISTENT between the fresh var and Dynamic).
bool test_var_rule() {
    PRINTLN("\n--- Test 9: VarRule ---");
    aura::core::TypeRegistry reg;
    aura::compiler::ConstraintSystem cs(reg);
    aura::diag::SourceLocation loc{};
    aura::compiler::InferenceContext ctx{reg, cs, loc};

    aura::ast::NodeView v_var{};
    v_var.tag = aura::ast::NodeTag::Variable;
    v_var.sym_id = 0;
    v_var.marker = aura::ast::SyntaxMarker::User;

    aura::compiler::VarRule rule;
    CHECK(rule.matches(v_var), "VarRule matches Variable");
    auto ty = rule.synthesize(ctx, v_var);
    // Fresh var is non-zero type id (not DYNAMIC)
    CHECK(ty.index != 0, "VarRule returns a non-zero fresh type var");
    // VarRule uses add() (not add_delta) — it's committing to the
    // full-solve path. The constraint should be in the system, and
    // a subsequent solve() should resolve it cleanly.
    auto solve_status = cs.solve();
    CHECK(solve_status == aura::compiler::SolveResult::SOLVED,
          "VarRule's constraint (fresh var ~ Dynamic) solves cleanly");
    return true;
}

// Test 10: rule dispatch — non-matching rule returns Dynamic
//
// When the rule's matches() returns false, infer_with_rules
// should fall back to the dynamic type (the safe "I don't
// know" answer). This is the property that makes
// infer_with_rules composable across multiple rules.
bool test_rule_dispatch() {
    PRINTLN("\n--- Test 10: rule dispatch on non-matching view ---");
    aura::core::TypeRegistry reg;
    aura::compiler::ConstraintSystem cs(reg);
    aura::diag::SourceLocation loc{};
    aura::compiler::InferenceContext ctx{reg, cs, loc};

    aura::ast::NodeView v_call{};
    v_call.tag = aura::ast::NodeTag::Call;
    v_call.marker = aura::ast::SyntaxMarker::User;

    aura::compiler::LiteralRule rule;
    CHECK(!rule.matches(v_call), "LiteralRule does NOT match Call");
    auto routed = aura::compiler::infer_with_rules(rule, ctx, v_call);
    CHECK(routed.index == reg.dynamic_type().index,
          "infer_with_rules falls back to dynamic_type for non-matching rule");
    return true;
}

int run_issue_162() {
    std::fprintf(stdout, "═══ Issue #162 — Phases 1+2: Type Concepts ═══\n");

    std::fprintf(stdout, "\n--- Phase 1: TypeConstraint + 2 concrete constraints ---\n");
    test_concept_satisfaction();
    test_solve_equal();
    test_solve_consistent();
    test_solve_empty();
    test_concept_rejection();
    test_backward_compat();

    std::fprintf(stdout, "\n--- Phase 2: InferenceRule + 3 concrete rules ---\n");
    test_inference_rule_satisfaction();
    test_literal_rule();
    test_var_rule();
    test_rule_dispatch();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
