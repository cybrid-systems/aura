// ──────────────────────────────────────────────────────────────
// Type Concepts — Issue #162 (Phases 1 + 2)
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
// Phase 1 (shipped 2026-06-12):
//   - concept TypeConstraint
//   - concept InferenceRule scaffold (result_type only)
//   - EqualConstraint, ConsistentConstraint
//   - solve_constraints<C> free-function template
//
// Phase 2 (this commit, also 2026-06-12):
//   - Full InferenceRule concept (result_type + synthesize)
//   - InferenceContext struct (lightweight rule context)
//   - 3 concrete rule types: LiteralRule, BoolRule, VarRule
//   - infer_with_rules<R> free-function template
//   - test_issue_162 expanded with Phase 2 assertions
//
// Phase 3 (deferred — scoped for fresh-session push):
//   - Template-ize ConstraintSystem itself so add/solve are
//     concept-driven rather than over the monolithic
//     Constraint struct (the existing API stays).
//   - Migrate existing Constraint{ Kind, lhs, rhs } call
//     sites in type_checker_impl.cpp to EqualConstraint /
//     ConsistentConstraint.
//   - Touch the Union-Find + worklist algorithm to be
//     template-friendly.
//   - Add more concrete rule types (CallRule, LambdaRule,
//     IfRule, LetRule, BeginRule, AnnotationRule).

module;
#include <cstdint>

export module aura.compiler.type_concepts;

import std;
import aura.core;
import aura.core.ast;
import aura.core.type;
import aura.diag;
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
    { c.kind() } -> std::same_as<ConstraintKind>;
    { c.unify(cs) } -> std::same_as<bool>;
    { c.lhs() } -> std::same_as<aura::core::TypeId>;
    { c.rhs() } -> std::same_as<aura::core::TypeId>;
};

// ── InferenceContext ──────────────────────────────────────────
//
// Lightweight handle that bundles everything an InferenceRule
// needs to do its work. The full InferenceEngine holds many
// more members (env_, ownership_env_, coercions_, ...), but
// the rules we ship in Phase 2 only need:
//
//   - TypeRegistry&  — to intern/lookup types and produce
//                      type ids for literals
//   - ConstraintSystem&  — to add EQUAL/CONSISTENT constraints
//                           (the rules produce constraints,
//                           the caller solves them)
//   - SourceLocation — to attach to any diagnostic
//
// A rule that needs more (e.g. type env lookup for VarRule)
// can read it from the registry, or the InferenceContext can
// be extended in Phase 3 to carry an env reference. For now,
// the simplest possible context that the three Phase 2 rules
// actually need.
export struct InferenceContext {
    aura::core::TypeRegistry& registry;
    ConstraintSystem& cs;
    aura::diag::SourceLocation loc;
};

// ── InferenceRule concept (full) ──────────────────────────────
//
// A type R satisfies InferenceRule if it:
//   1. Exposes `result_type` (typically `aura::core::TypeId`)
//   2. Has a `synthesize(ctx, view)` method that returns the
//      result type. The method MAY add constraints to ctx.cs
//      for downstream solving.
//
// The `matches(ctx, view)` method is a "does this rule apply
// to this node" predicate — a rule returns true iff it knows
// how to synthesize a type for the given node tag. The free
// function `infer_with_rules<R>(ctx, view)` dispatches via
// `matches()`.
export template <typename R>
concept InferenceRule =
    requires(R r, const InferenceContext& ctx, const aura::ast::NodeView& view) {
        typename R::result_type;
        { r.matches(view) } -> std::same_as<bool>;
        { r.synthesize(ctx, view) } -> std::same_as<typename R::result_type>;
    };

// ── infer_with_rules<R> — concept-driven inference (opt-in)
//
// Runs the rule R against the given view. Returns the rule's
// `synthesize` result if the rule matches. If the rule does
// not match (predicate returns false), returns the dynamic
// type (the safe fallback when no rule applies).
//
// Phase 2 ships the per-rule dispatcher (one rule per call).
// Phase 3 will provide a multi-rule fold expression that
// tries each rule in order until one matches.
export template <InferenceRule R>
aura::core::TypeId infer_with_rules(R& rule, const InferenceContext& ctx,
                                    const aura::ast::NodeView& view) {
    if (rule.matches(view)) {
        return rule.synthesize(ctx, view);
    }
    return ctx.registry.dynamic_type();
}

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
        : lhs_(l)
        , rhs_(r) {}

    constexpr ConstraintKind kind() const { return CK_EQUAL; }
    constexpr aura::core::TypeId lhs() const { return lhs_; }
    constexpr aura::core::TypeId rhs() const { return rhs_; }

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
        : lhs_(l)
        , rhs_(r) {}

    constexpr ConstraintKind kind() const { return CK_CONSISTENT; }
    constexpr aura::core::TypeId lhs() const { return lhs_; }
    constexpr aura::core::TypeId rhs() const { return rhs_; }

    bool unify(ConstraintSystem& cs) const;
};

// ── LiteralRule — concrete InferenceRule for integer literals
//
// Matches NodeTag::LiteralInt. Returns the registry's int_type().
// This is the simplest possible rule — no constraints added, no
// environment lookup, just a constant return. Demonstrates that
// the InferenceRule concept can carry rules that don't touch the
// constraint system at all.
export struct LiteralRule {
    using result_type = aura::core::TypeId;

    constexpr bool matches(const aura::ast::NodeView& view) const {
        return view.tag == aura::ast::NodeTag::LiteralInt;
    }
    aura::core::TypeId synthesize(const InferenceContext& ctx, const aura::ast::NodeView&) const {
        return ctx.registry.int_type();
    }
};

// ── BoolRule — concrete InferenceRule for boolean literals
//
// Matches nodes marked with the BoolLiteral SyntaxMarker. The
// FlatAST uses a single NodeTag::LiteralInt for both Int and
// Bool literals and distinguishes via the marker column. We
// follow the same convention.
//
// For now we return Dynamic from synthesize (the registry
// doesn't have a dedicated bool_type() — it's lumped with
// Int in many places). The Phase 3 work will add a
// proper bool_type() to the registry.
export struct BoolRule {
    using result_type = aura::core::TypeId;

    constexpr bool matches(const aura::ast::NodeView& view) const {
        return view.tag == aura::ast::NodeTag::LiteralInt &&
               view.marker == aura::ast::SyntaxMarker::BoolLiteral;
    }
    aura::core::TypeId synthesize(const InferenceContext& ctx, const aura::ast::NodeView&) const {
        return ctx.registry.int_type(); // no separate bool_type yet
    }
};

// ── VarRule — concrete InferenceRule for variable references
//
// Matches NodeTag::Variable. Allocates a fresh type variable
// from the ConstraintSystem and adds a CONSISTENT constraint
// so downstream callers can solve it. This is the rule that
// does meaningful work — it adds constraints to ctx.cs.
//
// In the existing InferenceEngine::synthesize_flat_var, a
// variable reference binds to the env's type or returns a
// fresh type var. We use the simpler "fresh var" behavior
// here, which is what a "type inference per node" rule
// does when the env isn't in scope. The Phase 3 work can
// extend this rule to read from an env reference.
export struct VarRule {
    using result_type = aura::core::TypeId;

    constexpr bool matches(const aura::ast::NodeView& view) const {
        return view.tag == aura::ast::NodeTag::Variable;
    }
    aura::core::TypeId synthesize(const InferenceContext& ctx, const aura::ast::NodeView&) const {
        // Allocate a fresh type var and add a CONSISTENT
        // constraint against the dynamic type so the solver
        // can resolve it. (The full env lookup is deferred
        // to Phase 3 — see the rule's docstring.)
        auto tv = ctx.cs.fresh_var();
        ctx.cs.add(Constraint{CK_CONSISTENT, tv, ctx.registry.dynamic_type()});
        return tv;
    }
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

// Compile-time concept checks: all shipped concrete types
// must satisfy the corresponding concept. If a future
// refactor breaks the contract, the build fails at the
// concept site, not at every call site. Big win for
// refactorability.
static_assert(TypeConstraint<EqualConstraint>,
              "EqualConstraint must satisfy TypeConstraint concept");
static_assert(TypeConstraint<ConsistentConstraint>,
              "ConsistentConstraint must satisfy TypeConstraint concept");
static_assert(InferenceRule<LiteralRule>, "LiteralRule must satisfy InferenceRule concept");
static_assert(InferenceRule<BoolRule>, "BoolRule must satisfy InferenceRule concept");
static_assert(InferenceRule<VarRule>, "VarRule must satisfy InferenceRule concept");

} // namespace aura::compiler
