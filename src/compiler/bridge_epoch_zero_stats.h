// bridge_epoch_zero_stats.h — Issue #2930 residual unstamped bridge_epoch==0
// process-wide counters. Shared by Evaluator::is_bridge_stale (module)
// and aura_is_jit_closure_fresh (aura_jit_bridge.cpp) so both paths attribute
// zero-epoch observations without linking Evaluator into the bridge TU.
//
// Quiet path: counters only advance when is_bridge_stale / dual-check sees
// bridge_epoch==0 while tracking is active (current != 0).

#pragma once

#include <atomic>
#include <cstdint>

namespace aura::compiler::bridge_epoch_zero {

inline std::atomic<std::uint64_t> observed_total{0};
inline std::atomic<std::uint64_t> treated_stale_total{0};

inline void note_observed() noexcept {
    observed_total.fetch_add(1, std::memory_order_relaxed);
}

inline void note_treated_stale() noexcept {
    treated_stale_total.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t observed_v_read() noexcept {
    return observed_total.load(std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t treated_stale_v_read() noexcept {
    return treated_stale_total.load(std::memory_order_relaxed);
}

} // namespace aura::compiler::bridge_epoch_zero
