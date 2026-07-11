// render_pass.ixx — Issues #1179/#1186 Phase 1: DirtyAware render short-circuit scaffold.

module;

export module aura.renderer.render_pass;

import std;

export namespace aura::renderer {

inline constexpr int kRenderPassPhase = 1;

struct DirtyRegion {
    bool clean = true;
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    [[nodiscard]] bool is_clean() const noexcept { return clean; }
    void mark_dirty(std::uint32_t x, std::uint32_t y) noexcept {
        clean = false;
        if (x0 == 0 && y0 == 0 && x1 == 0 && y1 == 0) {
            x0 = x1 = x;
            y0 = y1 = y;
        } else {
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
    }
    void clear() noexcept {
        clean = true;
        x0 = y0 = x1 = y1 = 0;
    }
};

struct RenderHotPathStats {
    std::uint64_t dirty_short_circuit_total = 0;
    std::uint64_t shape_stable_hit_total = 0;
    std::uint64_t present_batch_total = 0;
};

inline RenderHotPathStats g_render_hot_path_stats{};
inline DirtyRegion g_framebuffer_dirty{};

// Phase 1 present_batch: short-circuit when dirty region is clean.
inline bool present_batch_if_dirty() {
    ++g_render_hot_path_stats.present_batch_total;
    if (g_framebuffer_dirty.is_clean()) {
        ++g_render_hot_path_stats.dirty_short_circuit_total;
        return false; // skipped
    }
    return true; // would present
}

} // namespace aura::renderer
