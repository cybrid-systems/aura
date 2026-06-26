// src/core/mutators.ixx — Issue #501 Phase 2 slice.
//
// Concrete strategy classes that satisfy the Mutator
// concept (aura.core.concepts). Each strategy wraps a
// FlatAST structural mutation primitive (set_child /
// insert_child / remove_child) as a Mutator strategy
// object, and a generic `apply_mutation<>()` template
// dispatches through the concept.
//
// Architectural note: why a separate module?
//   - aura.core.mutation  imports aura.core.error
//   - aura.core.ast       imports aura.core.mutation
//   - aura.core.mutators  imports BOTH (the join point)
//
// Putting the strategies directly in mutation.ixx would
// require mutation.ixx to import ast.ixx, creating a
// cycle (ast already imports mutation). A separate
// partition breaks the cycle cleanly.
//
// What's in here (this slice):
//   - 4 concrete strategy classes (Replace / Insert /
//     Remove / NoOp), all operating on FlatAST directly.
//   - static_asserts locking the Mutator concept shape.
//   - apply_mutation<Mutator M>(FlatAST&, NodeId, M&&)
//     generic dispatch template.
//   - StrategyKind enum for future tag-based dispatch.
//
// What's NOT in here (5 follow-ups tracked):
//   1. Apply ASTContainer → query_impl.cpp + query.ixx
//      (the FlatAST::children() returns std::span<const
//       NodeId>, not std::ranges::view — relaxation is
//       a separate Phase 4 task).
//   2. Wire StrategyKind → mutate:* primitive dispatcher
//      in evaluator_primitives_mutate.cpp.
//   3. Add (compile:mutator-dispatch-stats) Aura
//      primitive for EDSL observability.
//   4. Replace inline lambda bodies in evaluator_*
//      with strategy classes for cleaner diffing.
//   5. (Phase 4) Children iteration → std::ranges views
//      (the ASTContainer relaxation feeds into this).

module;

#include <cstdint>
#include <format>

export module aura.core.mutators;

import std;
import aura.core.ast;
import aura.core.concepts;
import aura.core.error;
import aura.core.mutation;

namespace aura::ast::mutators {

// ── Convenience aliases ─────────────────────────────────────
//
// Shorter names within this module. Keep the fully-qualified
// `aura::core::` path on the public API surface so callers
// don't have to know about this namespace.
using aura::core::AuraError;
using aura::core::AuraErrorKind;
using aura::core::AuraResult;
using aura::core::make_unexpected;
using aura::ast::NodeId;
using aura::ast::NULL_NODE;
using aura::ast::FlatAST;

// ── ReplaceChildMutator ─────────────────────────────────────
//
// Replace the child at a fixed index on `target` with
// `new_child`. Validates the target id and index range
// before mutating. Marks the target dirty after a
// successful mutation so cache invalidation cascades.
//
// Error cases:
//   - target is NULL_NODE or out-of-range → MutationError
//   - index >= child count → MutationError
//   - otherwise succeeds and returns target.
export struct ReplaceChildMutator {
    std::uint32_t index = 0;
    NodeId new_child = NULL_NODE;

    [[nodiscard]] AuraResult<NodeId>
    apply(FlatAST& flat, NodeId target) const {
        if (!flat.is_valid(target)) {
            return std::unexpected(make_unexpected(
                AuraErrorKind::MutationInvalidTarget,
                std::format("ReplaceChildMutator: invalid target NodeId {}",
                            static_cast<std::uint64_t>(target))));
        }
        const auto children = flat.children(target);
        if (index >= children.size()) {
            return std::unexpected(make_unexpected(
                AuraErrorKind::MutationOutOfRange,
                std::format("ReplaceChildMutator: index {} >= child count {}",
                            index, children.size())));
        }
        flat.set_child(target, index, new_child);
        flat.mark_dirty_upward(target);
        return target;
    }
};

// ── InsertChildMutator ──────────────────────────────────────
//
// Insert `new_child` at `index` on `target`. Appends if
// index == child_count, no-op-validates for larger
// indices (FlatAST's insert_child clamps).
//
// Error cases:
//   - target invalid → MutationInvalidTarget
//   - otherwise succeeds and returns target.
export struct InsertChildMutator {
    std::uint32_t index = 0;
    NodeId new_child = NULL_NODE;

    [[nodiscard]] AuraResult<NodeId>
    apply(FlatAST& flat, NodeId target) const {
        if (!flat.is_valid(target)) {
            return std::unexpected(make_unexpected(
                AuraErrorKind::MutationInvalidTarget,
                std::format("InsertChildMutator: invalid target NodeId {}",
                            static_cast<std::uint64_t>(target))));
        }
        flat.insert_child(target, index, new_child);
        flat.mark_dirty_upward(target);
        return target;
    }
};

// ── RemoveChildMutator ──────────────────────────────────────
//
// Remove the child at `index` from `target`.
//
// Error cases:
//   - target invalid → MutationInvalidTarget
//   - index >= child count → MutationOutOfRange
//   - otherwise succeeds and returns target.
export struct RemoveChildMutator {
    std::uint32_t index = 0;

    [[nodiscard]] AuraResult<NodeId>
    apply(FlatAST& flat, NodeId target) const {
        if (!flat.is_valid(target)) {
            return std::unexpected(make_unexpected(
                AuraErrorKind::MutationInvalidTarget,
                std::format("RemoveChildMutator: invalid target NodeId {}",
                            static_cast<std::uint64_t>(target))));
        }
        const auto children = flat.children(target);
        if (index >= children.size()) {
            return std::unexpected(make_unexpected(
                AuraErrorKind::MutationOutOfRange,
                std::format("RemoveChildMutator: index {} >= child count {}",
                            index, children.size())));
        }
        flat.remove_child(target, index);
        flat.mark_dirty_upward(target);
        return target;
    }
};

// ── NoOpMutator ─────────────────────────────────────────────
//
// Identity strategy. Returns `target` unchanged.
// Useful as a default in conditional mutation pipelines
// (e.g., "if condition then Replace else NoOp") and as
// a reference for the Mutator concept shape.
export struct NoOpMutator {
    [[nodiscard]] AuraResult<NodeId>
    apply(FlatAST& /*flat*/, NodeId target) const {
        return target;
    }
};

// ── Compile-time concept verification ───────────────────────
//
// Each strategy must satisfy Mutator<FlatAST>. If a future
// refactor changes the apply() signature, this static_assert
// fires at the module's translation unit — caught at build
// time, not at runtime.
static_assert(aura::core::Mutator<ReplaceChildMutator, FlatAST>,
              "ReplaceChildMutator must satisfy Mutator<FlatAST>");
static_assert(aura::core::Mutator<InsertChildMutator, FlatAST>,
              "InsertChildMutator must satisfy Mutator<FlatAST>");
static_assert(aura::core::Mutator<RemoveChildMutator, FlatAST>,
              "RemoveChildMutator must satisfy Mutator<FlatAST>");
static_assert(aura::core::Mutator<NoOpMutator, FlatAST>,
              "NoOpMutator must satisfy Mutator<FlatAST>");

// ── Verify FlatAST satisfies ASTContainer ─────────────────
//
// After Phase 3 relaxed ASTContainer to std::ranges::range
// (from std::ranges::view), FlatAST qualifies because
// FlatAST::children() returns std::span<const NodeId>,
// which is a borrowed_range. This static_assert locks
// the contract: if a future refactor of FlatAST breaks
// ASTContainer (e.g., by changing children() to return a
// non-range), the build fails immediately.
static_assert(aura::core::ASTContainer<FlatAST>,
              "FlatAST must satisfy ASTContainer<FlatAST>");

// ── Generic dispatch template ──────────────────────────────
//
// apply_mutation<>() is the Mutator-constrained generic
// entry point. It compiles only when M is a Mutator for
// FlatAST — bad shapes fail at compile time with a clear
// concept-violation error instead of an inscrutable SFINAE
// substitution failure.
//
// Usage:
//   ReplaceChildMutator r{/*index*/0, /*new_child*/child};
//   auto result = apply_mutation(flat, target, r);
//   if (!result) return std::unexpected(result.error());
//
//   // Or with a NoOpMutator default:
//   NoOpMutator noop;
//   auto result2 = apply_mutation(flat, target2, noop);
export template <aura::core::Mutator<FlatAST> M>
[[nodiscard]] AuraResult<NodeId>
apply_mutation(FlatAST& flat, NodeId target, M&& mut) {
    return std::forward<M>(mut).apply(flat, target);
}

// ── StrategyKind (tag enum for runtime dispatch) ────────────
//
// Maps a strategy by-name to its concrete type. The enum
// enables a future tag-dispatch helper (apply_by_kind())
// that picks the right strategy at runtime. The actual
// dispatcher is follow-up #2 — wiring this through the
// evaluator's mutate:* primitives.
//
// Tag values are stable: persisted in mutation logs and
// (eventually) in the wire protocol, so do NOT reorder.
export enum class StrategyKind : std::uint8_t {
    NoOp = 0,
    ReplaceChild = 1,
    InsertChild = 2,
    RemoveChild = 3,
};

// ── kind_name() — human-readable label ──────────────────────
//
// Returns a short string for each StrategyKind. Useful in
// REPL output, error messages, and EDSL observability.
export [[nodiscard]] constexpr std::string_view
kind_name(StrategyKind k) noexcept {
    switch (k) {
        case StrategyKind::NoOp:         return "no-op";
        case StrategyKind::ReplaceChild: return "replace-child";
        case StrategyKind::InsertChild:  return "insert-child";
        case StrategyKind::RemoveChild:  return "remove-child";
    }
    return "unknown";
}

// ── StrategyParams — uniform parameter bag ─────────────────
//
// Bundles the parameters shared across the structural
// strategies (ReplaceChild / InsertChild / RemoveChild).
// NoOp ignores the params. Keeps the dispatch helper's
// signature stable as new strategies are added (each new
// strategy can extend StrategyParams with optional fields,
// or take a separate params struct in its own dispatch
// overload).
export struct StrategyParams {
    std::uint32_t index = 0;
    NodeId new_child = NULL_NODE;
};

// ── apply_by_kind — runtime strategy selection ─────────────
//
// Picks the right concrete strategy from a StrategyKind tag
// + StrategyParams, then routes through apply_mutation<>.
// This is the entry point for data-driven mutation pipelines
// (REPL input, mutation logs, mutate:* primitive dispatch).
//
// Each case forwards through the Mutator-constrained
// apply_mutation<> template, so adding a new strategy
// requires:
//   1. Add the kind + concrete strategy class.
//   2. Add a case below.
//   3. Update the static_assert(Mutator<...>) list.
//
// The switch is exhaustive over StrategyKind; adding a new
// kind without a case triggers an enum-value-not-handled
// compiler warning (or error with -Werror=switch).
export [[nodiscard]] AuraResult<NodeId>
apply_by_kind(FlatAST& flat, NodeId target,
              StrategyKind kind, const StrategyParams& params) {
    switch (kind) {
        case StrategyKind::NoOp:
            return apply_mutation(flat, target, NoOpMutator{});

        case StrategyKind::ReplaceChild:
            return apply_mutation(flat, target,
                ReplaceChildMutator{params.index, params.new_child});

        case StrategyKind::InsertChild:
            return apply_mutation(flat, target,
                InsertChildMutator{params.index, params.new_child});

        case StrategyKind::RemoveChild:
            return apply_mutation(flat, target,
                RemoveChildMutator{params.index});
    }
    // Unreachable: switch is exhaustive over StrategyKind.
    // Defensive: return InternalInvariantViolation if a
    // future refactor adds a kind without updating this
    // switch (and -Werror=switch is off).
    return std::unexpected(make_unexpected(
        AuraErrorKind::InternalInvariantViolation,
        std::format("apply_by_kind: unhandled StrategyKind value {}",
                    static_cast<std::uint32_t>(kind))));
}

// ── apply_by_name — string-tagged dispatch ─────────────────
//
// Convenience wrapper for callers that have a strategy name
// (e.g. from REPL input or mutate:* primitive arguments).
// Returns InvalidTarget if the name doesn't match a known
// strategy.
//
// kind_from_name is the inverse of kind_name.
export [[nodiscard]] std::optional<StrategyKind>
kind_from_name(std::string_view name) noexcept {
    if (name == "no-op")         return StrategyKind::NoOp;
    if (name == "replace-child") return StrategyKind::ReplaceChild;
    if (name == "insert-child")  return StrategyKind::InsertChild;
    if (name == "remove-child")  return StrategyKind::RemoveChild;
    return std::nullopt;
}

export [[nodiscard]] AuraResult<NodeId>
apply_by_name(FlatAST& flat, NodeId target,
              std::string_view name, const StrategyParams& params) {
    auto kind = kind_from_name(name);
    if (!kind) {
        return std::unexpected(make_unexpected(
            AuraErrorKind::MutationInvalidField,
            std::format("apply_by_name: unknown strategy name '{}'", name)));
    }
    return apply_by_kind(flat, target, *kind, params);
}

} // namespace aura::ast::mutators