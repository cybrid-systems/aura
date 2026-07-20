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
// Issue #1920 Phase 2: full consumer adoption counters (lowering / executor /
// critical passes / JIT) + dirty/shape driven incremental metrics.
namespace aura::compiler::ir_soa_migration {
inline std::atomic<std::uint64_t> dual_emit_bridge_count{0};
inline std::atomic<std::uint64_t> hotpath_hits{0};
// Issue #1377: process-wide opt-in for SoA dual-emit in lower_to_ir_impl.
// Default false — zero production overhead until Phase 2 consumers land.
inline std::atomic<bool> g_enable_soa_dual_emit{false};
inline std::atomic<std::uint64_t> dual_emit_skipped_total{0};
// Issue #1920: migration phase (2 = full consumer adoption active).
inline constexpr int kIrSoaMigrationPhase = 2;
inline constexpr int kIrSoaMigrationIssue = 1920;
// Consumer adoption tallies (Agents / gate: ≥4 consumers wired).
inline std::atomic<std::uint64_t> consumer_lowering_hits{0};
inline std::atomic<std::uint64_t> consumer_executor_hits{0};
inline std::atomic<std::uint64_t> consumer_pass_hits{0};
inline std::atomic<std::uint64_t> consumer_jit_hits{0};
// Dirty / shape / linear integration.
inline std::atomic<std::uint64_t> dirty_block_driven_skips{0};
inline std::atomic<std::uint64_t> dirty_block_driven_runs{0};
inline std::atomic<std::uint64_t> shape_column_consults{0};
inline std::atomic<std::uint64_t> linear_column_consults{0};
// Closure capture dirty tracking (#1046 / #1920).
inline std::atomic<std::uint64_t> capture_dirty_marks_total{0};
inline std::atomic<std::uint64_t> phase2_consumer_wired{1};

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
inline void record_consumer_lowering() noexcept {
    consumer_lowering_hits.fetch_add(1, std::memory_order_relaxed);
    record_hotpath_hit();
}
inline void record_consumer_executor() noexcept {
    consumer_executor_hits.fetch_add(1, std::memory_order_relaxed);
    record_hotpath_hit();
}
inline void record_consumer_pass() noexcept {
    consumer_pass_hits.fetch_add(1, std::memory_order_relaxed);
    record_hotpath_hit();
}
inline void record_consumer_jit() noexcept {
    consumer_jit_hits.fetch_add(1, std::memory_order_relaxed);
    record_hotpath_hit();
}
inline void record_dirty_block_skip(std::uint64_t n = 1) noexcept {
    dirty_block_driven_skips.fetch_add(n, std::memory_order_relaxed);
}
inline void record_dirty_block_run(std::uint64_t n = 1) noexcept {
    dirty_block_driven_runs.fetch_add(n, std::memory_order_relaxed);
}
inline void record_shape_column_consult() noexcept {
    shape_column_consults.fetch_add(1, std::memory_order_relaxed);
}
inline void record_linear_column_consult() noexcept {
    linear_column_consults.fetch_add(1, std::memory_order_relaxed);
}
inline void record_capture_dirty_mark(std::uint64_t n = 1) noexcept {
    capture_dirty_marks_total.fetch_add(n, std::memory_order_relaxed);
}
// Clean-block hit rate in basis points: skips / (skips+runs) * 10000.
[[nodiscard]] inline std::uint64_t dirty_driven_clean_hit_rate_bp() noexcept {
    const auto s = dirty_block_driven_skips.load(std::memory_order_relaxed);
    const auto r = dirty_block_driven_runs.load(std::memory_order_relaxed);
    const auto den = s + r;
    if (den == 0)
        return 0;
    return (s * 10000ull) / den;
}
// Distinct consumer families with ≥1 hit (0..4).
[[nodiscard]] inline std::uint64_t consumer_families_active() noexcept {
    std::uint64_t n = 0;
    if (consumer_lowering_hits.load(std::memory_order_relaxed) > 0)
        ++n;
    if (consumer_executor_hits.load(std::memory_order_relaxed) > 0)
        ++n;
    if (consumer_pass_hits.load(std::memory_order_relaxed) > 0)
        ++n;
    if (consumer_jit_hits.load(std::memory_order_relaxed) > 0)
        ++n;
    return n;
}
} // namespace aura::compiler::ir_soa_migration

#endif // AURA_COMPILER_JIT_TYPED_MUTATION_STATS_H