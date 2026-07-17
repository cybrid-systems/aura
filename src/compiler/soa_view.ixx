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
// Issue #1517 / #1619: pipeline enforcement phase (static_assert + soft metrics).
// Phase 2 (#1619): SoAView requires columnar_accessor + full EDSL hot-path helpers.
inline constexpr int kSoaViewEnforcementPhase = 2;
inline constexpr int kSoaViewEnforcementIssue = 1619;

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

// Issue #1241 / #1517 / #1619: SoAView concept — columnar view with
// shape + linear ownership + columnar_accessor(). Const view only
// (zero-overhead DOD). Hot passes that consume IR SoA must satisfy this.
// (Inside export namespace — no per-declaration export keyword.)
template <typename V>
concept SoAView = requires(const V& v, std::uint32_t idx) {
    { v.size() } -> std::convertible_to<std::size_t>;
    { v.shape_id(idx) } -> std::convertible_to<std::uint32_t>;
    { v.linear_ownership(idx) } -> std::convertible_to<std::uint8_t>;
    // Issue #1619: mandatory columnar accessor (SafePCVSpan-like column root).
    { v.columnar_accessor() };
};

// Issue #1517 / #1619: full columnar SoAView — opcode / shape / ownership
// columns as SafePCVSpan for eval/JIT hot dispatch (no pointer-chasing).
template <typename V>
concept SoAViewFull = SoAView<V> && requires(const V& v) {
    { v.opcodes() };
    { v.shape_ids() };
    { v.linear_ownerships() };
    { v.columnar_accessor() };
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
    // Issue #1619: SoAView::columnar_accessor — primary opcode column root.
    [[nodiscard]] SafePCVSpan<aura::ir::IROpcode> columnar_accessor() const noexcept {
        return opcodes();
    }
};

static_assert(SoAView<IRFunctionSoAView>,
              "IRFunctionSoAView must satisfy SoAView (#1619 columnar)");
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

// Issue #1619: EDSL/eval hot-path consult via tag_arity_index packing.
// Pattern matchers and children iterators call this instead of AoS chase.
[[nodiscard]] inline std::uint32_t consult_tag_arity(std::uint8_t tag,
                                                     std::uint8_t arity) noexcept {
    record_soa_view_hit();
    record_edsl_soa_migration_progress(1);
    return tag_arity_index(tag, arity);
}

// Issue #1619: children / apply_closure hot-path marker — records SoA
// migration progress when callers use SafePCVSpan / columnar children.
inline void record_edsl_children_soa_path() noexcept {
    record_soa_view_hit();
    record_edsl_soa_migration_progress(1);
}

// Issue #1619: apply_closure / Guard mutate consult (shape + linear pair).
// Returns false when view is empty (records miss); true on dual consult hit.
[[nodiscard]] inline bool consult_closure_shape_linear(const IRFunctionSoAView& view,
                                                       std::uint32_t idx, std::uint32_t& shape_out,
                                                       std::uint8_t& linear_out) noexcept {
    if (view.func == nullptr || static_cast<std::size_t>(idx) >= view.size()) {
        record_soa_view_miss();
        shape_out = 0;
        linear_out = 0;
        return false;
    }
    shape_out = view.shape_id(idx);
    linear_out = view.linear_ownership(idx);
    record_edsl_soa_migration_progress(1);
    return true;
}

// Migration ratio in basis points: hits / (hits+misses) * 10000.
// 10000 = 100% SoA hits; 0 when no samples.
[[nodiscard]] inline std::uint64_t migration_ratio_bp() noexcept {
    const auto h = g_soa_view_hits.load(std::memory_order_relaxed);
    const auto m = g_soa_view_misses.load(std::memory_order_relaxed);
    const auto denom = h + m;
    if (denom == 0)
        return 0;
    return (h * 10000ull) / denom;
}

// Compile-time pack check helper for tests / pipeline documentation.
template <typename V> consteval void assert_soa_view_compliant() {
    static_assert(SoAView<V>, "type must satisfy SoAView (#1619 columnar_accessor + shape/linear)");
}

template <typename V> consteval void assert_soa_view_full_compliant() {
    static_assert(SoAViewFull<V>, "type must satisfy SoAViewFull (#1619)");
}

} // namespace aura::compiler::soa_view
