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
// Issue #1517: pipeline SoAView concept enforcement + EDSL migration progress.
inline std::atomic<std::uint64_t> g_concept_enforcement_hits{0};
inline std::atomic<std::uint64_t> g_soa_view_pass_skipped{0};
inline std::atomic<std::uint64_t> g_edsl_soa_migration_progress{0};
// Issue #1377: dual-emit is opt-in (default off) in production lower;
// full SoA primary path remains deferred Phase 2+ (see ir_soa.ixx).
inline constexpr int kSoaMigrationPhase2 = 1;
// Issue #1517: pipeline enforcement phase (static_assert + soft metrics).
inline constexpr int kSoaViewEnforcementPhase = 1;

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
inline void record_concept_enforcement_hit() noexcept {
    g_concept_enforcement_hits.fetch_add(1, std::memory_order_relaxed);
}
inline void record_soa_view_pass_skipped() noexcept {
    g_soa_view_pass_skipped.fetch_add(1, std::memory_order_relaxed);
}
inline void record_edsl_soa_migration_progress(std::uint64_t n = 1) noexcept {
    g_edsl_soa_migration_progress.fetch_add(n, std::memory_order_relaxed);
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

// Issue #1241 / #1517: SoAView concept — columnar view with shape +
// linear ownership consult. Const view only (zero-overhead DOD).
// (Inside export namespace — no per-declaration export keyword.)
template <typename V>
concept SoAView = requires(const V& v, std::uint32_t idx) {
    { v.size() } -> std::convertible_to<std::size_t>;
    { v.shape_id(idx) } -> std::convertible_to<std::uint32_t>;
    { v.linear_ownership(idx) } -> std::convertible_to<std::uint8_t>;
};

// Issue #1517: full columnar SoAView — also exposes opcode / shape columns
// as SafePCVSpan for eval/JIT hot dispatch (no pointer-chasing).
template <typename V>
concept SoAViewFull = SoAView<V> && requires(const V& v) {
    { v.opcodes() };
    { v.shape_ids() };
    { v.linear_ownerships() };
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
        record_soa_view_hit();
        return func->linear_ownership_states_[idx];
    }
    [[nodiscard]] aura::ir::IROpcode opcode(std::uint32_t idx) const noexcept {
        contract_assert(func != nullptr);
        contract_assert(static_cast<std::size_t>(idx) < func->opcodes_.size());
        record_soa_view_hit();
        return func->opcodes_[idx];
    }
    [[nodiscard]] SafePCVSpan<aura::ir::IROpcode> opcodes() const noexcept {
        if (!func)
            return {};
        record_soa_view_hit();
        return {func->opcodes_.data(), func->opcodes_.size()};
    }
    [[nodiscard]] SafePCVSpan<std::uint32_t> shape_ids() const noexcept {
        if (!func)
            return {};
        record_soa_view_hit();
        return {func->shape_ids_.data(), func->shape_ids_.size()};
    }
    [[nodiscard]] SafePCVSpan<std::uint8_t> linear_ownerships() const noexcept {
        if (!func)
            return {};
        record_soa_view_hit();
        return {func->linear_ownership_states_.data(), func->linear_ownership_states_.size()};
    }
};

static_assert(SoAView<IRFunctionSoAView>, "IRFunctionSoAView must satisfy SoAView");
static_assert(SoAViewFull<IRFunctionSoAView>, "IRFunctionSoAView must satisfy SoAViewFull");

// Issue #1517: bind a view and record EDSL migration progress when non-null.
[[nodiscard]] inline IRFunctionSoAView make_function_soa_view(const IRFunctionSoA* func) noexcept {
    if (!func) {
        record_soa_view_miss();
        return {};
    }
    record_edsl_soa_migration_progress(1);
    return IRFunctionSoAView{func};
}

// tag_arity_index: compact (tag, arity) → column index helper for matcher hot path.
[[nodiscard]] constexpr std::uint32_t tag_arity_index(std::uint8_t tag,
                                                      std::uint8_t arity) noexcept {
    return (static_cast<std::uint32_t>(tag) << 8) | arity;
}

// Issue #1517: pattern-matcher / children hot-path consult of shape column.
// Returns shape_id or 0 when view empty / OOB (records miss).
[[nodiscard]] inline std::uint32_t consult_shape(const IRFunctionSoAView& view,
                                                 std::uint32_t idx) noexcept {
    if (view.func == nullptr || static_cast<std::size_t>(idx) >= view.size()) {
        record_soa_view_miss();
        return 0;
    }
    return view.shape_id(idx);
}

// Issue #1517: linear_ownership consult for Guard/mutate hot path.
[[nodiscard]] inline std::uint8_t consult_linear(const IRFunctionSoAView& view,
                                                 std::uint32_t idx) noexcept {
    if (view.func == nullptr || static_cast<std::size_t>(idx) >= view.size()) {
        record_soa_view_miss();
        return 0;
    }
    return view.linear_ownership(idx);
}

} // namespace aura::compiler::soa_view
