// linear_occurrence_mutate_stats.h — Issue #747: OwnershipEnv +
// Occurrence Typing predicate-branch linear safety under typed mutation.
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