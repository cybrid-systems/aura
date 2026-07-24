// test_voxel_frame.cpp — Issue #1985 / Epic #1979
// Full-frame voxel render loop (headless CI).
//
// ACs:
//   AC1: render_frame writes all pixels and produces ANSI with half-blocks
//   AC2: demo scene has solid hits (not pure sky)
//   AC3: FrameStats rays/hits/pixels/elapsed populated
//   AC4: N headless frames advance frame_loop_counters
//   AC5: dirty region non-clean after frame (full-frame dirty OK)

#include "test_harness.hpp"

#include "renderer/voxel_frame.hh"

#include <print>
#include <string>

import std;

namespace {

using aura::renderer::build_demo_scene;
using aura::renderer::Camera;
using aura::renderer::default_materials;
using aura::renderer::default_shade_params;
using aura::renderer::frame_loop_counters;
using aura::renderer::FramebufferOwned;
using aura::renderer::kDefaultMaterialCount;
using aura::renderer::pixel_framebuffer_from_owned;
using aura::renderer::render_frame;
using aura::renderer::render_frame_tracked;
using aura::renderer::RenderFrameOptions;
using aura::renderer::reset_frame_loop_counters_for_test;
using aura::renderer::ShadeParams;
using aura::renderer::VoxelVolume;
using aura::test::g_failed;
using aura::test::g_passed;

void ac1_complete_frame_ansi() {
    std::println("\n--- AC1: complete frame → ANSI half-block ---");
    VoxelVolume vol(24, 12, 24, 0);
    build_demo_scene(vol);

    FramebufferOwned fb;
    fb.resize(20, 10); // 20×20 pixels
    auto pf = pixel_framebuffer_from_owned(fb);

    Camera cam;
    cam.position = {12.f, 8.f, 12.f};
    cam.yaw = 0.7f;
    cam.pitch = -0.4f;
    cam.fov_y = 1.0f;

    std::string ansi;
    RenderFrameOptions opt;
    opt.present = true;
    opt.present_out = &ansi;

    const auto st = render_frame(cam, vol, default_materials(), kDefaultMaterialCount,
                                 default_shade_params(), pf, opt);
    CHECK(st.pixels == 20u * 20u, "all pixels written");
    CHECK(st.present_bytes > 0, "present produced bytes");
    CHECK(!ansi.empty(), "ANSI non-empty");
    CHECK(ansi.find("\xE2\x96\x80") != std::string::npos || ansi.find("▀") != std::string::npos,
          "half-block in ANSI");
    // Truecolor SGR present
    CHECK(ansi.find("38;2;") != std::string::npos || ansi.find("48;2;") != std::string::npos,
          "truecolor SGR in ANSI");
}

void ac2_demo_hits() {
    std::println("\n--- AC2: demo scene produces hits ---");
    VoxelVolume vol(32, 16, 32, 0);
    build_demo_scene(vol);
    CHECK(vol.get(0, 0, 0) != 0, "floor non-air");

    FramebufferOwned fb;
    fb.resize(16, 8);
    auto pf = pixel_framebuffer_from_owned(fb);

    Camera cam;
    cam.position = {16.f, 10.f, 16.f};
    cam.yaw = 0.f;
    cam.pitch = -0.6f; // look down at floor
    cam.fov_y = 1.1f;

    std::string ansi;
    RenderFrameOptions opt;
    opt.present_out = &ansi;
    const auto st = render_frame(cam, vol, default_materials(), kDefaultMaterialCount,
                                 default_shade_params(), pf, opt);
    CHECK(st.hits > 0, "some ray hits geometry");
    CHECK(st.rays == st.pixels, "one ray per pixel");
    CHECK(st.hits + st.misses == st.rays, "hits+misses == rays");
}

void ac3_stats_timing() {
    std::println("\n--- AC3: FrameStats timing ---");
    VoxelVolume vol(16, 8, 16, 0);
    build_demo_scene(vol);
    FramebufferOwned fb;
    fb.resize(12, 6);
    auto pf = pixel_framebuffer_from_owned(fb);
    Camera cam;
    cam.position = {8.f, 5.f, 8.f};
    cam.pitch = -0.3f;

    std::string ansi;
    RenderFrameOptions opt;
    opt.present_out = &ansi;
    opt.time_it = true;
    const auto st = render_frame(cam, vol, default_materials(), kDefaultMaterialCount,
                                 default_shade_params(), pf, opt);
    CHECK(st.elapsed_ms >= 0.0, "elapsed_ms >= 0");
    CHECK(st.fps() >= 0.0, "fps estimate non-negative");
    std::println("    pixels={} hits={} misses={} ms={:.3f} fps≈{:.1f}", st.pixels, st.hits,
                 st.misses, st.elapsed_ms, st.fps());
}

void ac4_multi_frame_counters() {
    std::println("\n--- AC4: multi-frame counters ---");
    reset_frame_loop_counters_for_test();
    VoxelVolume vol(16, 8, 16, 0);
    build_demo_scene(vol);
    FramebufferOwned fb;
    fb.resize(10, 5);
    auto pf = pixel_framebuffer_from_owned(fb);
    Camera cam;
    cam.position = {8.f, 6.f, 4.f};
    cam.pitch = -0.2f;

    RenderFrameOptions opt;
    opt.present = false; // skip present for speed
    constexpr int N = 5;
    for (int i = 0; i < N; ++i) {
        cam.yaw = 0.15f * static_cast<float>(i);
        (void)render_frame_tracked(cam, vol, default_materials(), kDefaultMaterialCount,
                                   default_shade_params(), pf, opt);
    }
    auto& c = frame_loop_counters();
    CHECK(c.frames == static_cast<std::uint64_t>(N), "frames == N");
    CHECK(c.total_pixels == static_cast<std::uint64_t>(N) * 10u * 10u, "total pixels");
    CHECK(c.total_hits > 0, "accumulated hits");
}

void ac5_dirty_after_frame() {
    std::println("\n--- AC5: dirty region after frame ---");
    VoxelVolume vol(12, 6, 12, 0);
    build_demo_scene(vol);
    FramebufferOwned fb;
    fb.resize(8, 4);
    fb.dirty.clear();
    CHECK(fb.dirty.is_clean(), "starts clean");
    auto pf = pixel_framebuffer_from_owned(fb);
    Camera cam;
    cam.position = {6.f, 4.f, 6.f};
    cam.pitch = -0.4f;
    RenderFrameOptions opt;
    opt.present = false;
    (void)render_frame(cam, vol, default_materials(), kDefaultMaterialCount, default_shade_params(),
                       pf, opt);
    CHECK(fb.dirty.is_dirty(), "dirty after full frame");
    CHECK(fb.dirty.cell_count() > 0, "dirty cells > 0");
}

} // namespace

int main() {
    std::println("=== test_voxel_frame (#1985 / epic #1979) ===");
    ac1_complete_frame_ansi();
    ac2_demo_hits();
    ac3_stats_timing();
    ac4_multi_frame_counters();
    ac5_dirty_after_frame();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
