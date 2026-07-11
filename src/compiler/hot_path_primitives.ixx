// hot_path_primitives.ixx — Issue #1227 Phase 1: dedicated hot-path primitive registry scaffold.

module;

export module aura.compiler.hot_path_primitives;

import std;

export namespace aura::compiler::hot_path_prims {

inline constexpr int kHotPathPrimitivesPhase = 1;

enum class HotPrimKind : std::uint8_t {
    BatchFfi = 0,
    ParallelIntend = 1,
    RenderPresent = 2,
    QuotaCheck = 3,
    Count
};

struct HotPrimDescriptor {
    HotPrimKind kind;
    std::string_view name;
    bool batch_capable = false;
};

inline constexpr HotPrimDescriptor kHotPrimTable[] = {
    {HotPrimKind::BatchFfi, "batch-ffi-call", true},
    {HotPrimKind::ParallelIntend, "parallel-intend", true},
    {HotPrimKind::RenderPresent, "render-present-batch", true},
    {HotPrimKind::QuotaCheck, "resource:quota-check", false},
};

struct HotPathPrimStats {
    std::uint64_t dispatches = 0;
    std::uint64_t batch_hits = 0;
};

inline HotPathPrimStats g_hot_path_prim_stats{};

[[nodiscard]] inline std::size_t hot_prim_count() noexcept {
    return sizeof(kHotPrimTable) / sizeof(kHotPrimTable[0]);
}

} // namespace aura::compiler::hot_path_prims
