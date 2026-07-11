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

// Issue #1318 Phase 1: dual-emit bridge counter (plain header for lowering_impl).
// Issue #1377: dual-emit is opt-in (default off) — production lower pays
// AoS-only cost unless a CompilerService / test enables the flag.
namespace aura::compiler::ir_soa_migration {
inline std::atomic<std::uint64_t> dual_emit_bridge_count{0};
inline std::atomic<std::uint64_t> hotpath_hits{0};
// Issue #1377: process-wide opt-in for SoA dual-emit in lower_to_ir_impl.
// Default false — zero production overhead until Phase 2 consumers land.
inline std::atomic<bool> g_enable_soa_dual_emit{false};
inline std::atomic<std::uint64_t> dual_emit_skipped_total{0};
[[nodiscard]] inline bool soa_dual_emit_enabled() noexcept {
    return g_enable_soa_dual_emit.load(std::memory_order_relaxed);
}
inline void set_soa_dual_emit_enabled(bool on) noexcept {
    g_enable_soa_dual_emit.store(on, std::memory_order_relaxed);
}
inline void record_dual_emit_bridge() noexcept {
    dual_emit_bridge_count.fetch_add(1, std::memory_order_relaxed);
}
inline void record_dual_emit_skipped() noexcept {
    dual_emit_skipped_total.fetch_add(1, std::memory_order_relaxed);
}
inline void record_hotpath_hit() noexcept {
    hotpath_hits.fetch_add(1, std::memory_order_relaxed);
}
} // namespace aura::compiler::ir_soa_migration

#endif // AURA_COMPILER_JIT_TYPED_MUTATION_STATS_H