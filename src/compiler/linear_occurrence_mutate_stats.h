// linear_occurrence_mutate_stats.h — Issue #747: OwnershipEnv +
// Occurrence Typing predicate-branch linear safety under typed mutation.
//
// Issue #2222 + #2675 decision table (align IR LinearEnforceMode with composite):
//   production_defaults || MutationBoundary fiber hold → effective Strict
//   else Soft (unless AURA_LINEAR_ENFORCE=strict)
//   #2108 composite cross-batch escape hard-block always on (independent)
//
// Issue #2675: single effective API shared by AST audit, IR execute, and
// MutationBoundary force classification. See
// `aura::core::provenance::effective_linear_enforce(production_defaults,
// fiber_boundary_hold, env_force_strict)` in core/provenance_tracker.hh —
// pure function, no globals, header-only (no module cycle). Replaces split
// checks per call site. #2108 cross-batch escape hard-block remains an
// independent authority (returns its own deny_kind, not through this table).
//
// See core/provenance_tracker.hh LinearEnforceMode +
// mutation_boundary_push/pop_linear_enforce_strict.
#ifndef AURA_COMPILER_LINEAR_OCCURRENCE_MUTATE_STATS_H
#define AURA_COMPILER_LINEAR_OCCURRENCE_MUTATE_STATS_H

#include <atomic>
#include <cstdint>

namespace aura::compiler::linear_occurrence_mutate {

inline std::atomic<std::uint64_t> revalidate_hits_total{0};
inline std::atomic<std::uint64_t> escape_violations_prevented_total{0};
inline std::atomic<std::uint64_t> predicate_branch_linear_safe_total{0};
inline std::atomic<std::uint64_t> linear_occurrence_dirty_total{0};

inline void record_revalidate_hit() noexcept {
    revalidate_hits_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_escape_violation_prevented() noexcept {
    escape_violations_prevented_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_predicate_branch_linear_safe() noexcept {
    predicate_branch_linear_safe_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_linear_occurrence_dirty() noexcept {
    linear_occurrence_dirty_total.fetch_add(1, std::memory_order_relaxed);
}

} // namespace aura::compiler::linear_occurrence_mutate

#endif // AURA_COMPILER_LINEAR_OCCURRENCE_MUTATE_STATS_H