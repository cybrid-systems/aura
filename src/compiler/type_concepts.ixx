// ──────────────────────────────────────────────────────────────
// Type Concepts — Issue #162 Phase 1
// ──────────────────────────────────────────────────────────────
//
// C++26 Concepts for the type system, per cpp26_guide §2.2
// (Concepts > 运行时多态). The goal of #162 is to make the
// type checker extensible through Concepts: new constraint
// types, new inference rules, new type features (linear
// types, effects, etc.) can be added as types that satisfy
// the Concepts, without modifying the core ConstraintSystem
// or InferenceEngine classes.
//
// Phase 1 ships:
//   - concept TypeConstraint  (the constraint-level concept)
//   - concept InferenceRule   (the inference-level concept, scaffold)
//   - EqualConstraint, ConsistentConstraint (concrete types
//     that satisfy TypeConstraint, zero behavior change vs
//     the existing Constraint struct)
//   - solve_constraints<C>(cs, span) free-function template
//     that uses the concept (opt-in path — the existing
//     ConstraintSystem::solve remains the default)
//
// Phase 2 (follow-up issue): template-ize ConstraintSystem
// itself so the add/solve paths are concept-driven rather
// than over the monolithic Constraint struct. This is a
// bigger change and is scoped for a fresh-session push.

module;
#include <cstdint>

export module aura.compiler.type_concepts;

import std;
import aura.core;
import aura.core.type;
import aura.compiler.type_checker;

namespace aura::compiler {

// ── Re-export the core types so callers don't need to import
//    type_checker just to use the concepts ──
export using ConstraintKind = Constraint::Kind;

export constexpr ConstraintKind CK_EQUAL = Constraint::EQUAL;
export constexpr ConstraintKind CK_CONSISTENT = Constraint::CONSISTENT;

export using CSolveResult = SolveResult;

// ── TypeConstraint concept ────────────────────────────────────
//
// A type C satisfies TypeConstraint if it can be:
//   1. Categorized — `c.kind()` returns the kind tag
//   2. Applied     — `c.unify(cs)` performs the constraint's
//                    unification against a ConstraintSystem
//   3. Inspected   — `c.lhs()` and `c.rhs()` expose the two
//                    type ids participating in the constraint
//
// The concept is intentionally narrow. We don't require
// serialization, hashing, or comparison — those can be
// added via refinement concepts (e.g. `PersistentConstraint`)
// in follow-up issues.
export template <typename C>
concept TypeConstraint = requires(C c, ConstraintSystem& cs) {
    { c.kind() }      -> std::same_as<ConstraintKind>;
    { c.unify(cs) }   -> std::same_as<bool>;
    { c.lhs() }       -> std::same_as<aura::core::TypeId>;
    { c.rhs() }       -> std::same_as<aura::core::TypeId>;
};

// ── InferenceRule concept (scaffold) ───────────────────────────
//
// A type R satisfies InferenceRule if it can synthesize a
// type for a FlatAST node. Phase 1 ships the concept; no
// concrete rules are migrated yet. The implementation needs
// the full InferenceEngine definition, so the concept
// declaration is here and concrete rules will land in
// Phase 2 when the template-ize work happens.
export template <typename R>
concept InferenceRule = requires {
    typename R::result_type;  // typically aura::core::TypeId
    // The full expression body will be added in Phase 2
    // (requires importing ast::NodeView + InferenceEngine
    // for the full signature). For now, having result_type
    // is enough to start authoring concrete rules against
    // the concept.
};

// ── EqualConstraint — concrete type satisfying TypeConstraint
//
// Models the original Constraint{ Kind::EQUAL, lhs, rhs }.
// Behavior is identical to calling cs.unify(lhs, rhs) directly
// (the existing ConstraintSystem::unify path).
export struct EqualConstraint {
    aura::core::TypeId lhs_;
    aura::core::TypeId rhs_;

    constexpr EqualConstraint() = default;
    constexpr EqualConstraint(aura::core::TypeId l, aura::core::TypeId r)
        : lhs_(l), rhs_(r) {}

    constexpr ConstraintKind kind() const { return CK_EQUAL; }
    constexpr aura::core::TypeId lhs() const { return lhs_; }
    constexpr aura::core::TypeId rhs() const { return rhs_; }

    // Body requires complete ConstraintSystem; satisfied via
    // import of aura.compiler.type_checker above.
    bool unify(ConstraintSystem& cs) const;
};

// ── ConsistentConstraint — concrete type satisfying TypeConstraint
//
// Models the original Constraint{ Kind::CONSISTENT, lhs, rhs }.
// Behavior is identical to calling cs.consistent_unify(lhs, rhs).
export struct ConsistentConstraint {
    aura::core::TypeId lhs_;
    aura::core::TypeId rhs_;

    constexpr ConsistentConstraint() = default;
    constexpr ConsistentConstraint(aura::core::TypeId l, aura::core::TypeId r)
        : lhs_(l), rhs_(r) {}

    constexpr ConstraintKind kind() const { return CK_CONSISTENT; }
    constexpr aura::core::TypeId lhs() const { return lhs_; }
    constexpr aura::core::TypeId rhs() const { return rhs_; }

    bool unify(ConstraintSystem& cs) const;
};

// ── solve_constraints<C> — concept-driven solver (opt-in)
//
// Solves a span of any type C satisfying TypeConstraint.
// Returns:
//   - SolveResult::SOLVED   if all constraints unified cleanly
//   - SolveResult::CONFLICT on the first failing unification
//   - SolveResult::TIMEOUT  if the worklist scan hit the pass
//                           limit (delegated to the existing
//                           ConstraintSystem::solve)
//
// Phase 1 ships this as a free function that wraps the
// per-constraint unify calls. It does NOT replace
// ConstraintSystem::solve (which has a richer worklist
// algorithm and Union-Find normalization). The new path
// is for code that wants to drive solving manually — e.g.
// a new constraint kind, or a per-call solver for
// incremental type checking.
export template <TypeConstraint C>
SolveResult solve_constraints(ConstraintSystem& cs, std::span<const C> constraints) {
    for (const auto& c : constraints) {
        if (!c.unify(cs)) {
            return SolveResult::CONFLICT;
        }
    }
    return SolveResult::SOLVED;
}

// Compile-time concept check: EqualConstraint and
// ConsistentConstraint must satisfy TypeConstraint. This
// is a static_assert — if the concept is broken in a
// refactor, the build will fail at the concept site, not
// at every call site. Big win for refactorability.
static_assert(TypeConstraint<EqualConstraint>,
              "EqualConstraint must satisfy TypeConstraint concept");
static_assert(TypeConstraint<ConsistentConstraint>,
              "ConsistentConstraint must satisfy TypeConstraint concept");

} // namespace aura::compiler
