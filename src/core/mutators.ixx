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
using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NULL_NODE;
using aura::core::AuraError;
using aura::core::AuraErrorKind;
using aura::core::AuraResult;
using aura::core::make_unexpected;

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

    [[nodiscard]] AuraResult<NodeId> apply(FlatAST& flat, NodeId target) const {
        // Issue #1688: is_live_node — multi-step mutates bump generation_
        // between ops; is_valid would reject still-live parents.
        if (!flat.is_live_node(target)) {
            return std::unexpected(
                make_unexpected(AuraErrorKind::MutationInvalidTarget,
                                std::format("ReplaceChildMutator: invalid target NodeId {}",
                                            static_cast<std::uint64_t>(target))));
        }
        const auto children = flat.children(target);
        if (index >= children.size()) {
            return std::unexpected(
                make_unexpected(AuraErrorKind::MutationOutOfRange,
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

    [[nodiscard]] AuraResult<NodeId> apply(FlatAST& flat, NodeId target) const {
        // Issue #1688: use is_live_node (not is_valid). is_valid requires
        // node_gen == current generation_; each structural op bumps gen, so
        // multi-edge / multi-step mutates would spuriously reject live parents.
        if (!flat.is_live_node(target)) {
            return std::unexpected(
                make_unexpected(AuraErrorKind::MutationInvalidTarget,
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

    [[nodiscard]] AuraResult<NodeId> apply(FlatAST& flat, NodeId target) const {
        // Issue #1688: is_live_node — see InsertChildMutator note.
        if (!flat.is_live_node(target)) {
            return std::unexpected(
                make_unexpected(AuraErrorKind::MutationInvalidTarget,
                                std::format("RemoveChildMutator: invalid target NodeId {}",
                                            static_cast<std::uint64_t>(target))));
        }
        const auto children = flat.children(target);
        if (index >= children.size()) {
            return std::unexpected(
                make_unexpected(AuraErrorKind::MutationOutOfRange,
                                std::format("RemoveChildMutator: index {} >= child count {}", index,
                                            children.size())));
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
    [[nodiscard]] AuraResult<NodeId> apply(FlatAST& /*flat*/, NodeId target) const {
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
static_assert(aura::core::ASTContainer<FlatAST>, "FlatAST must satisfy ASTContainer<FlatAST>");

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

// ── Dispatch stats (EDSL observability, follow-up #4) ──────────────────
//
// Process-global counters for the strategy dispatch layer.
// Read by the (compile:mutator-dispatch-stats) Aura primitive
// so the AI agent can see which strategies get the most
// traffic (and which ones always roll back — a signal that
// the strategy isn't a good fit for the workload).
//
// Counters are std::atomic with relaxed updates; multi-field
// snapshots take shared_mtx_ so (compile:mutator-dispatch-stats)
// does not observe a torn multi-counter view while writers
// hold the unique side of the same mutex (#1849).
//
// Counters:
//   - apply_mutation_total: direct apply_mutation<> calls.
//   - apply_by_kind_total: apply_by_kind() dispatch calls.
//   - apply_by_name_total: apply_by_name() dispatch calls.
//   - failure_total: dispatched calls that returned AuraError.
//   - per-kind success counts (one per StrategyKind).
export struct MutatorDispatchStats {
    // Issue #1849: serializes multi-field snapshot vs bumps.
    // Writers take unique_lock for short fetch_add windows;
    // readers take shared_lock for a coherent capture().
    mutable std::shared_mutex mtx_;

    std::atomic<std::uint64_t> apply_mutation_total{0};
    std::atomic<std::uint64_t> apply_by_kind_total{0};
    std::atomic<std::uint64_t> apply_by_name_total{0};
    std::atomic<std::uint64_t> failure_total{0};

    // Per-kind success counters. Index = static_cast<uint8_t>(StrategyKind).
    std::atomic<std::uint64_t> kind_success[4] = {
        std::atomic<std::uint64_t>{0}, std::atomic<std::uint64_t>{0}, std::atomic<std::uint64_t>{0},
        std::atomic<std::uint64_t>{0}};

    // Per-kind failure counters. Same indexing.
    std::atomic<std::uint64_t> kind_failure[4] = {
        std::atomic<std::uint64_t>{0}, std::atomic<std::uint64_t>{0}, std::atomic<std::uint64_t>{0},
        std::atomic<std::uint64_t>{0}};

    // Coherent multi-field view for EDSL stats (#1849).
    struct Snapshot {
        std::uint64_t apply_mutation_total = 0;
        std::uint64_t apply_by_kind_total = 0;
        std::uint64_t apply_by_name_total = 0;
        std::uint64_t failure_total = 0;
        std::uint64_t kind_success[4] = {0, 0, 0, 0};
        std::uint64_t kind_failure[4] = {0, 0, 0, 0};

        [[nodiscard]] std::uint64_t total() const noexcept {
            return apply_mutation_total + apply_by_kind_total + apply_by_name_total;
        }
    };

    [[nodiscard]] Snapshot capture() const noexcept {
        std::shared_lock<std::shared_mutex> rlock(mtx_);
        Snapshot out;
        out.apply_mutation_total = apply_mutation_total.load(std::memory_order_relaxed);
        out.apply_by_kind_total = apply_by_kind_total.load(std::memory_order_relaxed);
        out.apply_by_name_total = apply_by_name_total.load(std::memory_order_relaxed);
        out.failure_total = failure_total.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < 4; ++i) {
            out.kind_success[i] = kind_success[i].load(std::memory_order_relaxed);
            out.kind_failure[i] = kind_failure[i].load(std::memory_order_relaxed);
        }
        return out;
    }

    void bump_apply_mutation() noexcept {
        std::unique_lock<std::shared_mutex> wlock(mtx_);
        apply_mutation_total.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_apply_by_kind() noexcept {
        std::unique_lock<std::shared_mutex> wlock(mtx_);
        apply_by_kind_total.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_apply_by_name() noexcept {
        std::unique_lock<std::shared_mutex> wlock(mtx_);
        apply_by_name_total.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_failure() noexcept {
        std::unique_lock<std::shared_mutex> wlock(mtx_);
        failure_total.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_kind_success(std::size_t idx) noexcept {
        if (idx >= 4)
            return;
        std::unique_lock<std::shared_mutex> wlock(mtx_);
        kind_success[idx].fetch_add(1, std::memory_order_relaxed);
    }
    void bump_kind_failure(std::size_t idx) noexcept {
        if (idx >= 4)
            return;
        std::unique_lock<std::shared_mutex> wlock(mtx_);
        kind_failure[idx].fetch_add(1, std::memory_order_relaxed);
    }

    void reset() noexcept {
        std::unique_lock<std::shared_mutex> wlock(mtx_);
        apply_mutation_total.store(0, std::memory_order_relaxed);
        apply_by_kind_total.store(0, std::memory_order_relaxed);
        apply_by_name_total.store(0, std::memory_order_relaxed);
        failure_total.store(0, std::memory_order_relaxed);
        for (auto& c : kind_success)
            c.store(0, std::memory_order_relaxed);
        for (auto& c : kind_failure)
            c.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t total() const noexcept {
        // Prefer capture().total() for multi-field consistency;
        // kept for existing C++ call sites.
        return capture().total();
    }
};

// Process-global stats accessor. Singleton initialized on
// first call (thread-safe per C++11 magic statics).
export [[nodiscard]] MutatorDispatchStats& dispatch_stats() noexcept {
    static MutatorDispatchStats s;
    return s;
}

export template <aura::core::Mutator<FlatAST> M>
[[nodiscard]] AuraResult<NodeId> apply_mutation(FlatAST& flat, NodeId target, M&& mut) {
    // Issue #1849: short unique bumps (never hold lock across apply).
    dispatch_stats().bump_apply_mutation();
    auto r = std::forward<M>(mut).apply(flat, target);
    if (!r) {
        dispatch_stats().bump_failure();
    }
    return r;
}

// Issue #1688: FlatAST is a DAG — a node may be a child of multiple
// parents (shared subexpressions, macro-shared bodies). Collect every
// (parent, child_index) edge pointing at `target`, ordered for safe
// multi-remove: same parent → higher child index first so RemoveChild
// does not shift still-pending indices.
export struct IncomingChildEdge {
    NodeId parent = NULL_NODE;
    std::uint32_t child_index = 0;
};

export [[nodiscard]] std::vector<IncomingChildEdge>
collect_incoming_child_edges(const FlatAST& flat, NodeId target) {
    // Issue #1689: O(deg) via FlatAST inverted parent-edge index
    // (rebuild O(N+E) only when dirty after bulk topology ops).
    std::vector<IncomingChildEdge> edges;
    if (target == NULL_NODE || target >= flat.size())
        return edges;
    auto raw = flat.collect_incoming_parent_edges(target);
    edges.reserve(raw.size());
    for (const auto& e : raw)
        edges.push_back(IncomingChildEdge{e.first, e.second});
    return edges;
}

// Issue #1688: remove `target` from ALL parents via RemoveChildMutator.
// Returns the number of edges removed, or an error on the first failed apply.
// Optional `on_edge` is invoked after each successful removal (for mutation log).
export template <typename OnEdge = std::nullptr_t>
[[nodiscard]] AuraResult<std::size_t> remove_node_from_all_parents(FlatAST& flat, NodeId target,
                                                                   OnEdge&& on_edge = {}) {
    auto edges = collect_incoming_child_edges(flat, target);
    if (edges.empty()) {
        return std::unexpected(
            make_unexpected(AuraErrorKind::MutationInvalidTarget,
                            std::format("remove_node_from_all_parents: node {} has no parent",
                                        static_cast<std::uint64_t>(target))));
    }
    std::size_t removed = 0;
    for (const auto& e : edges) {
        auto result = apply_mutation(flat, e.parent, RemoveChildMutator{e.child_index});
        if (!result)
            return std::unexpected(result.error());
        if constexpr (!std::is_same_v<std::remove_cvref_t<OnEdge>, std::nullptr_t>) {
            on_edge(e.parent, e.child_index);
        }
        ++removed;
    }
    return removed;
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
export [[nodiscard]] constexpr std::string_view kind_name(StrategyKind k) noexcept {
    switch (k) {
        case StrategyKind::NoOp:
            return "no-op";
        case StrategyKind::ReplaceChild:
            return "replace-child";
        case StrategyKind::InsertChild:
            return "insert-child";
        case StrategyKind::RemoveChild:
            return "remove-child";
    }
    return "unknown";
}

// Helper: index for a StrategyKind into the per-kind counter
// arrays. Defined here (after StrategyKind) so the dispatch
// templates can use it.
export [[nodiscard]] inline std::size_t kind_index(StrategyKind k) noexcept {
    return static_cast<std::size_t>(k);
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
apply_by_kind(FlatAST& flat, NodeId target, StrategyKind kind, const StrategyParams& params) {
    // Issue #1849: unique bump; apply_mutation releases before re-enter.
    dispatch_stats().bump_apply_by_kind();
    AuraResult<NodeId> result = std::unexpected(
        make_unexpected(AuraErrorKind::InternalInvariantViolation,
                        std::format("apply_by_kind: unhandled StrategyKind value {}",
                                    static_cast<std::uint32_t>(kind))));
    bool ok = false;
    switch (kind) {
        case StrategyKind::NoOp:
            result = apply_mutation(flat, target, NoOpMutator{});
            ok = true;
            break;
        case StrategyKind::ReplaceChild:
            result =
                apply_mutation(flat, target, ReplaceChildMutator{params.index, params.new_child});
            ok = true;
            break;
        case StrategyKind::InsertChild:
            result =
                apply_mutation(flat, target, InsertChildMutator{params.index, params.new_child});
            ok = true;
            break;
        case StrategyKind::RemoveChild:
            result = apply_mutation(flat, target, RemoveChildMutator{params.index});
            ok = true;
            break;
    }
    if (ok) {
        const auto idx = kind_index(kind);
        if (result) {
            dispatch_stats().bump_kind_success(idx);
        } else {
            dispatch_stats().bump_kind_failure(idx);
        }
    }
    return result;
}

// ── apply_by_name — string-tagged dispatch ─────────────────
//
// Convenience wrapper for callers that have a strategy name
// (e.g. from REPL input or mutate:* primitive arguments).
// Returns InvalidTarget if the name doesn't match a known
// strategy.
//
// kind_from_name is the inverse of kind_name.
export [[nodiscard]] std::optional<StrategyKind> kind_from_name(std::string_view name) noexcept {
    if (name == "no-op")
        return StrategyKind::NoOp;
    if (name == "replace-child")
        return StrategyKind::ReplaceChild;
    if (name == "insert-child")
        return StrategyKind::InsertChild;
    if (name == "remove-child")
        return StrategyKind::RemoveChild;
    return std::nullopt;
}

export [[nodiscard]] AuraResult<NodeId>
apply_by_name(FlatAST& flat, NodeId target, std::string_view name, const StrategyParams& params) {
    // Issue #1849: unique bump; apply_by_kind does not nest the same lock.
    dispatch_stats().bump_apply_by_name();
    auto kind = kind_from_name(name);
    if (!kind) {
        dispatch_stats().bump_failure();
        return std::unexpected(
            make_unexpected(AuraErrorKind::MutationInvalidField,
                            std::format("apply_by_name: unknown strategy name '{}'", name)));
    }
    return apply_by_kind(flat, target, *kind, params);
}

// (MutatorDispatchStats + dispatch_stats() + kind_index() are
// defined earlier in the module so apply_mutation / apply_by_kind
// can use them.)

} // namespace aura::ast::mutators