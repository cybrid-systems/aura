// soa_view.ixx — Issues #1241/#1243 Phase 1: SoAView helpers for eval/IR hot paths.

module;

#include <atomic>
#include <contracts>
#include <cstdint>

export module aura.compiler.soa_view;

import std;
import aura.compiler.ir;
import aura.compiler.ir_soa;

export namespace aura::compiler::soa_view {

inline constexpr int kSoaViewPhase = 1;

// Metrics: hits when hot paths consult SoAView helpers.
inline std::atomic<std::uint64_t> g_soa_view_hits{0};
inline std::atomic<std::uint64_t> g_soa_view_misses{0};
// Issue #1318 Phase 1: progressive full-migration counters (beyond dual-emit scaffold).
inline std::atomic<std::uint64_t> g_soa_migration_hotpath_hits{0};
inline std::atomic<std::uint64_t> g_soa_dual_emit_bridge_count{0};
inline std::atomic<std::uint64_t> g_soa_dirty_short_circuit{0};
inline constexpr int kSoaMigrationPhase2 = 1; // progressive; full retire of dual-emit is follow-up

inline void record_soa_view_hit() noexcept {
    g_soa_view_hits.fetch_add(1, std::memory_order_relaxed);
    g_soa_migration_hotpath_hits.fetch_add(1, std::memory_order_relaxed);
}
inline void record_soa_view_miss() noexcept {
    g_soa_view_misses.fetch_add(1, std::memory_order_relaxed);
}
inline void record_soa_dual_emit_bridge() noexcept {
    g_soa_dual_emit_bridge_count.fetch_add(1, std::memory_order_relaxed);
}
inline void record_soa_dirty_short_circuit() noexcept {
    g_soa_dirty_short_circuit.fetch_add(1, std::memory_order_relaxed);
}

// Zero-overhead non-owning span over a single SoA column (safe PCV pattern).
// Issue #1321: bounds-checked operator[] (contract in debug; no-op release).
template <typename T> struct SafePCVSpan {
    const T* data = nullptr;
    std::size_t len = 0;

    [[nodiscard]] constexpr std::size_t size() const noexcept { return len; }
    [[nodiscard]] constexpr bool empty() const noexcept { return len == 0; }
    [[nodiscard]] const T& operator[](std::size_t i) const {
        // AI mutation context: prevent SoA column OOB when dirty short-circuit
        // or dual-emit leaves sparse lengths.
        contract_assert(i < len);
        return data[i];
    }
    [[nodiscard]] constexpr const T* begin() const noexcept { return data; }
    [[nodiscard]] constexpr const T* end() const noexcept { return data + len; }
};

// Issue #1241: SoAView concept — columnar view with shape + linear consult.
export template <typename V>
concept SoAView = requires(const V& v, std::uint32_t idx) {
    { v.size() } -> std::convertible_to<std::size_t>;
    { v.shape_id(idx) } -> std::convertible_to<std::uint32_t>;
    { v.linear_ownership(idx) } -> std::convertible_to<std::uint8_t>;
};

// Adapter over IRFunctionSoA instruction columns.
struct IRFunctionSoAView {
    const IRFunctionSoA* func = nullptr;

    [[nodiscard]] std::size_t size() const noexcept { return func ? func->size() : 0; }
    [[nodiscard]] std::uint32_t shape_id(std::uint32_t idx) const noexcept {
        // Issue #1321: pre(idx valid) for SoAView hot dispatch.
        contract_assert(func != nullptr);
        contract_assert(static_cast<std::size_t>(idx) < func->shape_ids_.size());
        record_soa_view_hit();
        return func->shape_ids_[idx];
    }
    [[nodiscard]] std::uint8_t linear_ownership(std::uint32_t idx) const noexcept {
        contract_assert(func != nullptr);
        contract_assert(static_cast<std::size_t>(idx) < func->linear_ownership_states_.size());
        return func->linear_ownership_states_[idx];
    }
    [[nodiscard]] SafePCVSpan<aura::ir::IROpcode> opcodes() const noexcept {
        if (!func)
            return {};
        return {func->opcodes_.data(), func->opcodes_.size()};
    }
    [[nodiscard]] SafePCVSpan<std::uint32_t> shape_ids() const noexcept {
        if (!func)
            return {};
        return {func->shape_ids_.data(), func->shape_ids_.size()};
    }
};

static_assert(SoAView<IRFunctionSoAView>, "IRFunctionSoAView must satisfy SoAView");

// tag_arity_index: compact (tag, arity) → column index helper for matcher hot path.
[[nodiscard]] constexpr std::uint32_t tag_arity_index(std::uint8_t tag,
                                                      std::uint8_t arity) noexcept {
    return (static_cast<std::uint32_t>(tag) << 8) | arity;
}

} // namespace aura::compiler::soa_view
