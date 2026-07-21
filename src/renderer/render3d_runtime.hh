// render3d_runtime.hh — Issue #1986 / Epic #1979: process-local 3D render state.
//
// Opaque volume IDs for Aura primitives. No raw pointers cross the EDSL boundary.
// Engine owns volumes, camera, framebuffer, shade params; Aura mutates via IDs.

#ifndef AURA_RENDERER_RENDER3D_RUNTIME_HH
#define AURA_RENDERER_RENDER3D_RUNTIME_HH

#include "renderer/camera.hh"
#include "renderer/pixel_framebuffer.hh"
#include "renderer/voxel_frame.hh"
#include "renderer/voxel_shade.hh"
#include "renderer/voxel_volume.hh"

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace aura::renderer {

inline constexpr int kRender3dIssue = 1986;
inline constexpr int kRender3dEpic = 1979;
inline constexpr int kRender3dMaxVolumes = 8;

struct Render3dState {
    std::mutex mu;
    std::array<std::optional<VoxelVolume>, kRender3dMaxVolumes> volumes{};
    int next_slot = 0;

    Camera camera{};
    FramebufferOwned fb{};
    bool fb_ready = false;
    int fb_cols = 40;
    int fb_rows = 20;

    ShadeParams shade = default_shade_params();
    FrameStats last_stats{};
    std::string last_ansi{};

    // Allocate volume; returns 1-based id, or 0 on failure.
    int create_volume(int sx, int sy, int sz) {
        for (int i = 0; i < kRender3dMaxVolumes; ++i) {
            const int slot = (next_slot + i) % kRender3dMaxVolumes;
            if (!volumes[static_cast<std::size_t>(slot)].has_value()) {
                volumes[static_cast<std::size_t>(slot)] = VoxelVolume(sx, sy, sz, kAir);
                next_slot = (slot + 1) % kRender3dMaxVolumes;
                return slot + 1; // 1-based handle
            }
        }
        return 0;
    }

    bool destroy_volume(int id) {
        const int slot = id - 1;
        if (slot < 0 || slot >= kRender3dMaxVolumes)
            return false;
        if (!volumes[static_cast<std::size_t>(slot)].has_value())
            return false;
        volumes[static_cast<std::size_t>(slot)].reset();
        return true;
    }

    VoxelVolume* volume(int id) {
        const int slot = id - 1;
        if (slot < 0 || slot >= kRender3dMaxVolumes)
            return nullptr;
        auto& o = volumes[static_cast<std::size_t>(slot)];
        return o.has_value() ? &*o : nullptr;
    }

    void ensure_fb(int cols, int rows) {
        if (cols < 2)
            cols = 2;
        if (rows < 1)
            rows = 1;
        if (!fb_ready || fb_cols != cols || fb_rows != rows) {
            fb.resize(cols, rows);
            fb_cols = cols;
            fb_rows = rows;
            fb_ready = true;
        }
    }

    PixelFramebuffer pixel_view() {
        ensure_fb(fb_cols, fb_rows);
        return pixel_framebuffer_from_owned(fb);
    }
};

[[nodiscard]] inline Render3dState& global_render3d() {
    static Render3dState s{};
    // Default camera overlooking origin
    static bool once = false;
    if (!once) {
        s.camera.position = {16.f, 10.f, 28.f};
        s.camera.yaw = 0.f;
        s.camera.pitch = -0.35f;
        s.camera.fov_y = 1.05f;
        once = true;
    }
    return s;
}

} // namespace aura::renderer

#endif // AURA_RENDERER_RENDER3D_RUNTIME_HH
