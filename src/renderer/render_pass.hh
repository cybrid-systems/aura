// render_pass.hh — Issues #1179/#1186/#1559: DirtyAware render short-circuit (header form).
// Keep in sync with render_pass.ixx for module consumers.

#ifndef AURA_RENDERER_RENDER_PASS_HH
#define AURA_RENDERER_RENDER_PASS_HH

#include <algorithm>
#include <cstdint>

namespace aura::renderer {

inline constexpr int kRenderPassPhase = 1;

struct DirtyRegion {
    bool clean = true;
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    // True after clear() until first mark_dirty / mark_all_dirty (disambiguates AABB seed).
    bool empty_aabb = true;

    [[nodiscard]] bool is_clean() const noexcept { return clean; }
    // Issue #1559: AC alias — short-circuit when !is_dirty().
    [[nodiscard]] bool is_dirty() const noexcept { return !clean; }

    void mark_dirty(std::uint32_t x, std::uint32_t y) noexcept {
        clean = false;
        if (empty_aabb) {
            x0 = x1 = x;
            y0 = y1 = y;
            empty_aabb = false;
            return;
        }
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }

    void mark_all_dirty(std::uint32_t width, std::uint32_t height) noexcept {
        clean = false;
        empty_aabb = false;
        if (width == 0 || height == 0) {
            x0 = y0 = x1 = y1 = 0;
            return;
        }
        x0 = 0;
        y0 = 0;
        x1 = width - 1;
        y1 = height - 1;
    }

    void clear() noexcept {
        clean = true;
        empty_aabb = true;
        x0 = y0 = x1 = y1 = 0;
    }
};

struct RenderHotPathStats {
    std::uint64_t dirty_short_circuit_total = 0;
    std::uint64_t shape_stable_hit_total = 0;
    std::uint64_t present_batch_total = 0;
    std::uint64_t draw_batch_total = 0;
    std::uint64_t present_bytes_total = 0;
    std::uint64_t zero_copy_acquire_total = 0;
};

inline RenderHotPathStats g_render_hot_path_stats{};
inline DirtyRegion g_framebuffer_dirty{};

// Phase 1 present gate: short-circuit when dirty region is clean.
inline bool present_batch_if_dirty() {
    ++g_render_hot_path_stats.present_batch_total;
    if (g_framebuffer_dirty.is_clean()) {
        ++g_render_hot_path_stats.dirty_short_circuit_total;
        return false; // skipped
    }
    return true; // would present
}

} // namespace aura::renderer

#endif // AURA_RENDERER_RENDER_PASS_HH
