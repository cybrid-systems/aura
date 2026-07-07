// shape_jit_pass_closedloop_stats.h — Issue #744: Shape → dirty → Pass →
// JIT deopt/recompile closed-loop observability (plain header, cross-TU bumps).
#ifndef AURA_COMPILER_SHAPE_JIT_PASS_CLOSEDLOOP_STATS_H
#define AURA_COMPILER_SHAPE_JIT_PASS_CLOSEDLOOP_STATS_H

#include <atomic>
#include <cstdint>

namespace aura::compiler::shape_jit_pass {

inline std::atomic<std::uint64_t> stability_churn_deopts_total{0};
inline std::atomic<std::uint64_t> dirty_from_shape_total{0};
inline std::atomic<std::uint64_t> incremental_recompile_hits_total{0};
inline std::atomic<std::uint64_t> speculative_win_lost_total{0};

using MutationEpochFn = std::uint64_t (*)() noexcept;
inline std::atomic<MutationEpochFn> g_mutation_epoch_fn{nullptr};

inline std::uint64_t current_mutation_epoch() noexcept {
    auto fn = g_mutation_epoch_fn.load(std::memory_order_acquire);
    return fn ? fn() : 0;
}

inline void record_stability_churn_deopt() noexcept {
    stability_churn_deopts_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_dirty_from_shape() noexcept {
    dirty_from_shape_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_incremental_recompile_hit() noexcept {
    incremental_recompile_hits_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_speculative_win_lost() noexcept {
    speculative_win_lost_total.fetch_add(1, std::memory_order_relaxed);
}

} // namespace aura::compiler::shape_jit_pass

#endif // AURA_COMPILER_SHAPE_JIT_PASS_CLOSEDLOOP_STATS_H