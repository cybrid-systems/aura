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
import aura.core;
import aura.core.type;
import aura.diag;
import aura.compiler.type_checker;
import aura.compiler.type_concepts;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::print("  FAIL: {} (line {})\n", std::string(msg), __LINE__); \
        ++g_failed; \
    } else { \
        std::print("  PASS: {}\n", std::string(msg)); \
        ++g_passed; \
    } \
} while(0)

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

int main() {
    std::fprintf(stdout, "═══ Issue #162 — Phase 1: Type Concepts ═══\n");

    test_concept_satisfaction();
    test_solve_equal();
    test_solve_consistent();
    test_solve_empty();
    test_concept_rejection();
    test_backward_compat();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
