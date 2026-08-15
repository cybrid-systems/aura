// mutate_dispatch.hh — Issue #1964 cycle 4
// Unified mutate:* entry point dispatch.
//
// Issue #1964 AC #2 cycle 4: consolidate mutate:* entry points into
// a single dispatch (Issue #1439 / #1950 / #1953 prerequisite).
//
// Cycle 4 ship scope (design + minimal API surface):
// - Defines MutateKind enum (the canonical list of mutate entry
//   points: SetBody, Redefine, FromVerificationFeedback, etc.).
// - Defines MutateDispatchResult enum (Applied / Rejected /
//   Deferred) for the 3 outcomes a mutate call can have.
// - Defines mutate_dispatch() function signature — the canonical
//   single dispatch entry point. Existing `add("mutate:*", ...)`
//   primitives become thin wrappers around this function.
//
// Issue #3074 (cycle 4 follow-up): structural mutate:* bodies acquire
// the Guard only via mutate_dispatch_try_acquire. Metrics are live.
// GUARD_EXEMPT metadata prims stay exempt. See check_mutate_dispatch_sole_guard_3074.py.
//
// See docs/agent-safety-mechanisms-simplification.md §"Mutation
// paths" for the design rationale.

#ifndef AURA_COMPILER_MUTATE_DISPATCH_HH
#define AURA_COMPILER_MUTATE_DISPATCH_HH

#include <atomic>
#include <cstdint>
#include <string_view>

namespace aura::compiler {

// Canonical list of mutate entry points. Order is stable for
// serialization / observability snapshots. Append new kinds at
// the END to preserve wire-format compatibility with prior
// snapshots.
enum class MutateKind : std::uint8_t {
    SetBody = 0,                  // Formerly (mutate:set-body …)
    Redefine = 1,                 // Formerly (engine:redefine …)
    FromVerificationFeedback = 2, // Formerly (mutate:from-verification-feedback …)
    Typed = 3,                    // Typed mutation path
    Structural = 4,               // Structural mutation path
};

enum class MutateDispatchResult : std::uint8_t {
    Applied = 0,
    Rejected = 1,
    Deferred = 2,
};

// Process-wide observability counters for the unified mutate
// dispatch. Cycle 4 ship only defines the struct; cycle 4-followup
// wires the per-kind bumps in mutate_dispatch().
struct MutateDispatchMetrics {
    std::atomic<std::uint64_t> applied_total{0};
    std::atomic<std::uint64_t> rejected_total{0};
    std::atomic<std::uint64_t> deferred_total{0};
    // Per-kind counters (cycle 4-followup adds the bumps):
    std::atomic<std::uint64_t> set_body_applied_total{0};
    std::atomic<std::uint64_t> redefine_applied_total{0};
    std::atomic<std::uint64_t> from_verification_feedback_applied_total{0};
    std::atomic<std::uint64_t> typed_applied_total{0};
    std::atomic<std::uint64_t> structural_applied_total{0};
};

inline MutateDispatchMetrics& g_mutate_dispatch_metrics() noexcept {
    static MutateDispatchMetrics m;
    return m;
}

// Issue #3074: live (not simulate) applied/rejected bumps. Soft path
// is whatever try_acquire already does; no extra hot-path work beyond
// one relaxed add on the already-rare mutate entry.
inline constexpr int kMutateDispatchSoleGuardIssue = 3074;
inline std::atomic<std::uint32_t> g_mutate_dispatch_sole_guard_wired{1};

inline void mutate_dispatch_note(MutateKind kind, MutateDispatchResult result) noexcept {
    auto& m = g_mutate_dispatch_metrics();
    if (result == MutateDispatchResult::Deferred) {
        m.deferred_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (result != MutateDispatchResult::Applied) {
        m.rejected_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    m.applied_total.fetch_add(1, std::memory_order_relaxed);
    switch (kind) {
        case MutateKind::SetBody:
            m.set_body_applied_total.fetch_add(1, std::memory_order_relaxed);
            break;
        case MutateKind::Redefine:
            m.redefine_applied_total.fetch_add(1, std::memory_order_relaxed);
            break;
        case MutateKind::FromVerificationFeedback:
            m.from_verification_feedback_applied_total.fetch_add(1, std::memory_order_relaxed);
            break;
        case MutateKind::Typed:
            m.typed_applied_total.fetch_add(1, std::memory_order_relaxed);
            break;
        case MutateKind::Structural:
            m.structural_applied_total.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

// Issue #3074: sole Guard acquire for structural mutate:* bodies.
// Same AuraResult as MutationBoundaryGuard::try_acquire so call sites
// stay `if (!guard_r) / error() / std::move(*guard_r)`. Metrics are
// live: acquire fail → rejected; acquire ok → applied (dispatch entered
// the body). Include this header after Evaluator is in scope.
[[nodiscard]] inline aura::core::AuraResult<std::unique_ptr<Evaluator::MutationBoundaryGuard>>
mutate_dispatch_try_acquire(Evaluator& ev, std::uint64_t pending_count, bool* success_flag,
                            bool fine_rollback = false,
                            MutateKind kind = MutateKind::Structural) noexcept {
    auto gr = Evaluator::MutationBoundaryGuard::try_acquire(ev, pending_count, success_flag,
                                                            fine_rollback);
    if (!gr)
        mutate_dispatch_note(kind, MutateDispatchResult::Rejected);
    else
        mutate_dispatch_note(kind, MutateDispatchResult::Applied);
    return gr;
}

// Cycle-4 signature kept for source-cite / 1964 tests. Does not simulate
// applied — real Guard path is mutate_dispatch_try_acquire.
[[nodiscard]] inline MutateDispatchResult mutate_dispatch(MutateKind kind, std::string_view target,
                                                          std::string_view body) noexcept {
    (void)kind;
    (void)target;
    (void)body;
    return MutateDispatchResult::Deferred;
}

// String ↔ MutateKind mapping for the add("mutate:*", …) wrappers.
// Cycle 4-followup uses this to dispatch.
[[nodiscard]] inline MutateKind mutate_kind_from_string(std::string_view name) noexcept {
    if (name == "set-body")
        return MutateKind::SetBody;
    if (name == "redefine")
        return MutateKind::Redefine;
    if (name == "from-verification-feedback")
        return MutateKind::FromVerificationFeedback;
    if (name == "typed")
        return MutateKind::Typed;
    if (name == "structural")
        return MutateKind::Structural;
    return MutateKind::SetBody; // default fallback
}

[[nodiscard]] inline std::string_view mutate_kind_to_string(MutateKind kind) noexcept {
    switch (kind) {
        case MutateKind::SetBody:
            return "set-body";
        case MutateKind::Redefine:
            return "redefine";
        case MutateKind::FromVerificationFeedback:
            return "from-verification-feedback";
        case MutateKind::Typed:
            return "typed";
        case MutateKind::Structural:
            return "structural";
    }
    return "set-body"; // unreachable; suppresses -Wreturn-type
}

} // namespace aura::compiler

#endif // AURA_COMPILER_MUTATE_DISPATCH_HH