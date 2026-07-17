// render_primitives.hh — Issue #1559: present_batch / draw_batch engine API.
// Header form for evaluator partition TUs + unit tests.

#ifndef AURA_RENDERER_RENDER_PRIMITIVES_HH
#define AURA_RENDERER_RENDER_PRIMITIVES_HH

#include "core/zero_copy_output.hh"
#include "renderer/batch_terminal.hh"
#include "renderer/render_pass.hh"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aura::renderer {

inline constexpr int kRenderPrimitivesPhase = 1; // #1559 foundational engine
inline constexpr int kRenderPrimitivesIssue = 1559;

// Minimal cell-buffer view consumed by present_batch / draw_batch.
// Non-owning: cells points at TermCell[width * height] (AoS Phase 1;
// SoA column views deferred per issue scope).
struct FramebufferSoA {
    std::int32_t width = 0;
    std::int32_t height = 0;
    TermCell* cells = nullptr; // non-owning; may be null when invalid

    [[nodiscard]] bool valid() const noexcept {
        return cells != nullptr && width > 0 && height > 0;
    }
    [[nodiscard]] std::size_t cell_count() const noexcept {
        if (width <= 0 || height <= 0)
            return 0;
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
    [[nodiscard]] const TermCell* cells_c() const noexcept { return cells; }
};

// Single cell write for draw_batch.
struct DrawOp {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    TermCell cell{};
};

// Owned framebuffer for tests / standalone engine use.
struct FramebufferOwned {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<TermCell> storage;
    DirtyRegion dirty{};

    void resize(std::int32_t w, std::int32_t h) {
        width = w;
        height = h;
        storage.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                       TermCell::space_palette());
        dirty.mark_all_dirty(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    }

    [[nodiscard]] FramebufferSoA view() noexcept {
        return FramebufferSoA{width, height, storage.empty() ? nullptr : storage.data()};
    }
    [[nodiscard]] FramebufferSoA view() const noexcept {
        return FramebufferSoA{width, height,
                              storage.empty() ? nullptr : const_cast<TermCell*>(storage.data())};
    }
};

// Estimate ANSI frame byte budget (upper bound for zero-copy acquire).
[[nodiscard]] inline std::size_t estimate_ansi_frame_bytes(std::int32_t w,
                                                           std::int32_t h) noexcept {
    if (w <= 0 || h <= 0)
        return 128;
    return static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 16u + 128u;
}

// ── Engine entry points (implemented in render_primitives.cpp) ──

// present_batch: dirty short-circuit → hotpath → zero-copy → ANSI → write(fd).
// Returns bytes written, or 0 on short-circuit / empty, or -1 on invalid fb.
// Clears dirty on successful present (including 0-byte write of empty frame).
[[nodiscard]] std::int64_t present_batch(const FramebufferSoA& fb, DirtyRegion& dirty, int fd = 1);

// present_batch_to_string: same short-circuit / hotpath / zero-copy path but
// copies the ANSI frame into `out` (no write). Returns bytes, 0 skip, -1 invalid.
[[nodiscard]] std::int64_t present_batch_to_string(const FramebufferSoA& fb, DirtyRegion& dirty,
                                                   std::string& out);

// Issue #1561: present using an explicit frame arena (tests / external callers).
[[nodiscard]] std::int64_t present_batch_with_arena(const FramebufferSoA& fb, DirtyRegion& dirty,
                                                    aura::core::zero_copy::FrameBumpArena& arena,
                                                    int fd = 1);

// draw_batch: write DrawOps into fb, mark dirty AABB. Returns cells written.
// Enters/exits render hotpath around the loop.
[[nodiscard]] std::int64_t draw_batch(FramebufferSoA& fb, DirtyRegion& dirty,
                                      std::span<const DrawOp> ops);

// Convenience: draw a single cell.
[[nodiscard]] inline std::int64_t draw_cell(FramebufferSoA& fb, DirtyRegion& dirty, std::uint32_t x,
                                            std::uint32_t y, TermCell cell) {
    DrawOp op{x, y, cell};
    return draw_batch(fb, dirty, std::span<const DrawOp>(&op, 1));
}

// Process-wide engine counters (also mirrored into g_render_hot_path_stats).
struct RenderEngineCounters {
    std::uint64_t present_calls = 0;
    std::uint64_t present_skips = 0; // dirty short-circuit
    std::uint64_t present_bytes = 0;
    std::uint64_t draw_calls = 0;
    std::uint64_t draw_cells = 0;
    std::uint64_t zero_copy_acquires = 0;
    std::uint64_t sgr_emits = 0;
    std::uint64_t dirty_cells_emitted = 0; // #1562
    std::uint64_t registered = 0;          // set when EDSL registrar runs
};

[[nodiscard]] RenderEngineCounters& render_engine_counters() noexcept;

// Snapshot for tests / query hooks.
void reset_render_engine_counters_for_test() noexcept;

} // namespace aura::renderer

#endif // AURA_RENDERER_RENDER_PRIMITIVES_HH
