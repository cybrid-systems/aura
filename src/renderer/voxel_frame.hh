// voxel_frame.hh — Issue #1985 / Epic #1979: full-frame voxel render loop.
//
// Wires camera → raycast → shade → PixelFramebuffer → present_batch.
// Engine owns this helper; Aura (later, #1986) owns game state / camera motion.
//
// Phase 1: full-frame dirty (every pixel written) is acceptable.
// Soft target: 80×48 cells (≈80×96 px) ≥15–20 FPS on a modern laptop.

#ifndef AURA_RENDERER_VOXEL_FRAME_HH
#define AURA_RENDERER_VOXEL_FRAME_HH

#include "renderer/camera.hh"
#include "renderer/pixel_framebuffer.hh"
#include "renderer/voxel_raycast.hh"
#include "renderer/voxel_shade.hh"
#include "renderer/voxel_volume.hh"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace aura::renderer {

inline constexpr int kVoxelFrameIssue = 1985;
inline constexpr int kVoxelFrameEpic = 1979;

struct FrameStats {
    std::uint64_t pixels = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t rays = 0;
    std::int64_t present_bytes = 0;
    double elapsed_ms = 0.0;

    // Soft FPS estimate from elapsed_ms (0 if elapsed unknown).
    [[nodiscard]] double fps() const noexcept {
        if (elapsed_ms <= 0.0)
            return 0.0;
        return 1000.0 / elapsed_ms;
    }
};

struct RenderFrameOptions {
    bool present = true;                // call pixel_present / to_string
    int present_fd = 1;                 // used when present_out == nullptr
    std::string* present_out = nullptr; // headless: ANSI into this string
    float max_t = 1e6f;                 // raycast far clip
    bool time_it = true;                // fill elapsed_ms
};

// Render one complete frame into pf (must be valid PixelFramebuffer).
// Full-frame write; marks all cells dirty via per-pixel pixel_set.
// Returns stats. No heap beyond what present_batch already uses.
[[nodiscard]] inline FrameStats render_frame(const Camera& cam, const VoxelVolume& vol,
                                             const Material* materials, std::size_t material_count,
                                             const ShadeParams& shade, PixelFramebuffer& pf,
                                             const RenderFrameOptions& opt = {}) noexcept {
    FrameStats st{};
    if (!pf.valid() || !vol.valid() || !materials || material_count == 0)
        return st;

    const auto t0 =
        opt.time_it ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    const auto rays0 = raycast_counters().rays_cast;
    const auto hits0 = raycast_counters().hits;
    const auto miss0 = raycast_counters().misses;

    const int w = pf.pixel_width;
    const int h = pf.pixel_height;

    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            const Ray ray = camera_ray(cam, px, py, w, h);
            const Hit hit = raycast_voxel(vol, ray, opt.max_t);
            const Color32 c = shade_ray(hit, ray, materials, material_count, shade);
            (void)pixel_set(pf, px, py, c);
            ++st.pixels;
        }
    }

    st.rays = raycast_counters().rays_cast - rays0;
    st.hits = raycast_counters().hits - hits0;
    st.misses = raycast_counters().misses - miss0;

    if (opt.present) {
        if (opt.present_out) {
            st.present_bytes = pixel_present_to_string(pf, *opt.present_out);
        } else {
            st.present_bytes = pixel_present(pf, opt.present_fd);
        }
    }

    if (opt.time_it) {
        const auto t1 = std::chrono::steady_clock::now();
        st.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
    return st;
}

// Build a small demo scene: flat platform + a few pillars / blocks.
// Volume should already be resized; existing contents are cleared to air first.
inline void build_demo_scene(VoxelVolume& vol) {
    if (!vol.valid())
        return;
    vol.clear();
    const int sx = vol.size_x;
    const int sy = vol.size_y;
    const int sz = vol.size_z;
    // Floor (grass) at y=0
    if (sy >= 1)
        voxel_fill_box(vol, 0, 0, 0, sx, 1, sz, /*grass*/ 2);

    // Stone pillars
    auto pillar = [&](int cx, int cz, int h, BlockId id) {
        if (cx < 0 || cz < 0 || cx >= sx || cz >= sz)
            return;
        const int top = h < sy ? h : sy;
        for (int y = 1; y < top; ++y)
            vol.set(cx, y, cz, id);
    };
    pillar(sx / 4, sz / 4, sy / 2 + 1, 1); // stone
    pillar(sx * 3 / 4, sz / 4, sy / 2, 1);
    pillar(sx / 2, sz * 3 / 4, sy * 2 / 3, 6); // red accent
    // Small sand mound
    if (sx > 4 && sz > 4 && sy > 2) {
        vol.set(sx / 2, 1, sz / 2, 4);
        vol.set(sx / 2 + 1, 1, sz / 2, 4);
        vol.set(sx / 2, 1, sz / 2 + 1, 4);
    }
}

// Process-wide frame counters (frames rendered via render_frame).
struct FrameLoopCounters {
    std::uint64_t frames = 0;
    std::uint64_t total_pixels = 0;
    std::uint64_t total_hits = 0;
    double total_ms = 0.0;
};

[[nodiscard]] inline FrameLoopCounters& frame_loop_counters() noexcept {
    static FrameLoopCounters c{};
    return c;
}

inline void reset_frame_loop_counters_for_test() noexcept {
    frame_loop_counters() = {};
}

// Same as render_frame but also bumps frame_loop_counters.
[[nodiscard]] inline FrameStats render_frame_tracked(const Camera& cam, const VoxelVolume& vol,
                                                     const Material* materials,
                                                     std::size_t material_count,
                                                     const ShadeParams& shade, PixelFramebuffer& pf,
                                                     const RenderFrameOptions& opt = {}) noexcept {
    FrameStats st = render_frame(cam, vol, materials, material_count, shade, pf, opt);
    auto& c = frame_loop_counters();
    ++c.frames;
    c.total_pixels += st.pixels;
    c.total_hits += st.hits;
    c.total_ms += st.elapsed_ms;
    return st;
}

} // namespace aura::renderer

#endif // AURA_RENDERER_VOXEL_FRAME_HH
