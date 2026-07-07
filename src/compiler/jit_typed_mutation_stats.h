// jit_typed_mutation_stats.h — Issue #746: narrow_evidence / TypeId /
// linear_ownership_state propagation → JIT L2 zero-overhead closed loop.
#ifndef AURA_COMPILER_JIT_TYPED_MUTATION_STATS_H
#define AURA_COMPILER_JIT_TYPED_MUTATION_STATS_H

#include <atomic>
#include <cstdint>

namespace aura::compiler::jit_typed_mutation {

inline std::atomic<std::uint64_t> narrow_evidence_hits_total{0};
inline std::atomic<std::uint64_t> cast_elided_in_l2_total{0};
inline std::atomic<std::uint64_t> linear_state_optimized_total{0};
inline std::atomic<std::uint64_t> type_propagation_stamped_total{0};

inline void record_narrow_evidence_hit() noexcept {
    narrow_evidence_hits_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_cast_elided_in_l2() noexcept {
    cast_elided_in_l2_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_linear_state_optimized() noexcept {
    linear_state_optimized_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_type_propagation_stamp() noexcept {
    type_propagation_stamped_total.fetch_add(1, std::memory_order_relaxed);
}

} // namespace aura::compiler::jit_typed_mutation

#endif // AURA_COMPILER_JIT_TYPED_MUTATION_STATS_H