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
// Cycle 4 follow-up plan:
// - Migrate existing `add("mutate:*", ...)` primitives
//   (mutate:set-body, engine:redefine, mutate:from-verification-
//   feedback, typed mutation, structural mutation) to thin
//   wrappers around mutate_dispatch().
// - Add per-kind observability counters (applied_total /
//   rejected_total / deferred_total per MutateKind) to
//   MutateDispatchMetrics.
// - Add a linter to track per-primitive migration progress.
// - Update scripts/check_primitive_surface.py to count the new
//   mutate:* surface area.
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

// Canonical single dispatch entry point. Cycle 4 ship only
// defines the signature; cycle 4-followup wires the actual
// routing logic. Until then, callers should use the existing
// `add("mutate:*", ...)` primitives directly.
[[nodiscard]] inline MutateDispatchResult mutate_dispatch(MutateKind kind, std::string_view target,
                                                          std::string_view body) noexcept {
    auto& m = g_mutate_dispatch_metrics();
    // Cycle 4 ship: simulate applied path. Real routing lands in
    // cycle 4-followup.
    (void)target;
    (void)body;
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
    return MutateDispatchResult::Applied;
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