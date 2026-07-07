// arena_auto_policy_stats.h — Issue #743: runtime observability for
// Arena auto-compact + live defrag + fiber safepoint + dirty/Shape
// closed loop (zero release cost, plain header for cross-module bumps).
#ifndef AURA_CORE_ARENA_AUTO_POLICY_STATS_H
#define AURA_CORE_ARENA_AUTO_POLICY_STATS_H

#include <atomic>
#include <cstdint>

namespace aura::core::arena_policy {

inline std::atomic<std::uint64_t> auto_compact_triggers_total{0};
inline std::atomic<std::uint64_t> defrag_fiber_safe_hits_total{0};
// Last observed post-mutate fragmentation ratio in basis points (0..10000).
inline std::atomic<std::uint64_t> fragmentation_post_mutate_bp{0};
inline std::atomic<std::uint64_t> shape_inval_on_compact_total{0};
inline std::atomic<std::uint64_t> env_reval_success_total{0};
inline std::atomic<bool> dirty_cascade_pending{false};

inline void signal_dirty_cascade() noexcept {
    dirty_cascade_pending.store(true, std::memory_order_release);
}

inline bool consume_dirty_cascade() noexcept {
    return dirty_cascade_pending.exchange(false, std::memory_order_acq_rel);
}

inline void record_auto_compact_trigger() noexcept {
    auto_compact_triggers_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_defrag_fiber_safe_hit() noexcept {
    defrag_fiber_safe_hits_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_fragmentation_post_mutate(double frag_ratio) noexcept {
    const auto bp = static_cast<std::uint64_t>(frag_ratio * 10000.0);
    fragmentation_post_mutate_bp.store(bp, std::memory_order_relaxed);
}

inline void record_shape_inval_on_compact() noexcept {
    shape_inval_on_compact_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_env_reval_success() noexcept {
    env_reval_success_total.fetch_add(1, std::memory_order_relaxed);
}

} // namespace aura::core::arena_policy

#endif // AURA_CORE_ARENA_AUTO_POLICY_STATS_H