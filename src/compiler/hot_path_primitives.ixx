// hot_path_primitives.ixx — Issue #1227 Phase 1 + #1676 render-tier fast path scaffold.

module;

export module aura.compiler.hot_path_primitives;

import std;

export namespace aura::compiler::hot_path_prims {

// Phase 2 (#1676): render-tier names + process-wide dispatch counters.
inline constexpr int kHotPathPrimitivesPhase = 2;
inline constexpr int kHotPathPrimitivesIssue = 1676;

enum class HotPrimKind : std::uint8_t {
    BatchFfi = 0,
    ParallelIntend = 1,
    RenderPresent = 2,
    QuotaCheck = 3,
    RenderDraw = 4,
    TerminalPresent = 5,
    TuiPresent = 6,
    Count
};

struct HotPrimDescriptor {
    HotPrimKind kind;
    std::string_view name;
    bool batch_capable = false;
    bool render_tier = false; // #1676: participates in render fast dispatch + fence
};

inline constexpr HotPrimDescriptor kHotPrimTable[] = {
    {HotPrimKind::BatchFfi, "batch-ffi-call", true, false},
    {HotPrimKind::ParallelIntend, "parallel-intend", true, false},
    {HotPrimKind::RenderPresent, "render-present-batch", true, true},
    {HotPrimKind::QuotaCheck, "resource:quota-check", false, false},
    {HotPrimKind::RenderDraw, "render-draw-batch", true, true},
    {HotPrimKind::TerminalPresent, "terminal-present-batch", true, true},
    {HotPrimKind::TuiPresent, "tui:present", false, true},
};

struct HotPathPrimStats {
    std::uint64_t dispatches = 0;
    std::uint64_t batch_hits = 0;
    // Issue #1676 process-wide (complements CompilerMetrics atomics).
    std::uint64_t render_tier_dispatches = 0;
    std::uint64_t render_fence_calls = 0;
};

inline HotPathPrimStats g_hot_path_prim_stats{};

[[nodiscard]] inline std::size_t hot_prim_count() noexcept {
    return sizeof(kHotPrimTable) / sizeof(kHotPrimTable[0]);
}

[[nodiscard]] inline bool is_render_tier_name(std::string_view name) noexcept {
    for (const auto& d : kHotPrimTable) {
        if (d.render_tier && d.name == name)
            return true;
    }
    return false;
}

inline void note_render_tier_dispatch() noexcept {
    ++g_hot_path_prim_stats.dispatches;
    ++g_hot_path_prim_stats.render_tier_dispatches;
}

inline void note_render_fence() noexcept {
    ++g_hot_path_prim_stats.render_fence_calls;
}

} // namespace aura::compiler::hot_path_prims
