// ──────────────────────────────────────────────────────────────
// Type Concepts impl — Issue #162 Phase 1
// ──────────────────────────────────────────────────────────────
//
// Out-of-line definitions for EqualConstraint::unify and
// ConsistentConstraint::unify. The bodies need the complete
// ConstraintSystem type, so they're defined here (which
// imports the type_checker module) rather than inline in
// the .ixx.
//
// Both methods preserve the existing ConstraintSystem
// semantics:
//   - EqualConstraint::unify      → cs.unify(lhs, rhs)
//   - ConsistentConstraint::unify → cs.consistent_unify(lhs, rhs)
//
// If those underlying methods ever change, these need to
// change too. A future Phase 2 could route them through
// the same Union-Find machinery, but for now we delegate.

module aura.compiler.type_concepts;

namespace aura::compiler {

bool EqualConstraint::unify(ConstraintSystem& cs) const {
    return cs.unify(lhs_, rhs_);
}

bool ConsistentConstraint::unify(ConstraintSystem& cs) const {
    return cs.consistent_unify(lhs_, rhs_);
}

} // namespace aura::compiler
