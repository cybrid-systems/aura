// demo_voxel_3d.cpp — Issue #1985 / Epic #1979
// Minimal interactive / headless voxel first-person demo.
//
// Usage:
//   ./demo_voxel_3d --headless --frames 3
//   ./demo_voxel_3d --frames 60 --cols 80 --rows 24
//   ./demo_voxel_3d --headless --frames 1 --bench   # print soft FPS sample
//
// Live mode writes ANSI to stdout (fd=1). Headless captures strings only.

#include "renderer/voxel_frame.hh"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>
#include <string_view>
#include <thread>

import std;

namespace {

using aura::renderer::build_demo_scene;
using aura::renderer::Camera;
using aura::renderer::default_materials;
using aura::renderer::default_shade_params;
using aura::renderer::FramebufferOwned;
using aura::renderer::kDefaultMaterialCount;
using aura::renderer::pixel_framebuffer_from_owned;
using aura::renderer::render_frame;
using aura::renderer::RenderFrameOptions;
using aura::renderer::VoxelVolume;

struct Args {
    bool headless = false;
    bool bench = false;
    int frames = 30;
    int cols = 40; // terminal cells
    int rows = 20;
    int vol = 32;
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string_view s(argv[i]);
        auto need = [&](int& out) {
            if (i + 1 < argc)
                out = std::atoi(argv[++i]);
        };
        if (s == "--headless")
            a.headless = true;
        else if (s == "--bench")
            a.bench = true;
        else if (s == "--frames")
            need(a.frames);
        else if (s == "--cols")
            need(a.cols);
        else if (s == "--rows")
            need(a.rows);
        else if (s == "--vol")
            need(a.vol);
        else if (s == "-h" || s == "--help") {
            std::println("demo_voxel_3d [--headless] [--bench] [--frames N] [--cols C] "
                         "[--rows R] [--vol S]");
            std::exit(0);
        }
    }
    if (a.frames < 1)
        a.frames = 1;
    if (a.cols < 4)
        a.cols = 4;
    if (a.rows < 2)
        a.rows = 2;
    if (a.vol < 8)
        a.vol = 8;
    return a;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parse(argc, argv);

    VoxelVolume vol(args.vol, args.vol / 2, args.vol, 0);
    build_demo_scene(vol);

    FramebufferOwned fb;
    fb.resize(args.cols, args.rows);
    auto pf = pixel_framebuffer_from_owned(fb);

    Camera cam;
    // yaw=0 looks down −Z: stand at high Z, look toward origin over the platform.
    cam.position = {static_cast<float>(args.vol) * 0.5f, static_cast<float>(args.vol) * 0.28f,
                    static_cast<float>(args.vol) * 0.88f};
    cam.yaw = 0.f;
    cam.pitch = -0.35f;
    cam.fov_y = 1.05f;

    auto shade = default_shade_params();
    auto* mats = default_materials();

    double sum_ms = 0.0;
    std::uint64_t sum_hits = 0;
    std::string ansi;

    // Hide cursor + clear once for live mode
    if (!args.headless) {
        std::print("\033[?25l\033[2J\033[H");
        std::fflush(stdout);
    }

    for (int f = 0; f < args.frames; ++f) {
        cam.yaw = 0.08f * static_cast<float>(f); // spin

        RenderFrameOptions opt;
        opt.time_it = true;
        if (args.headless) {
            ansi.clear();
            opt.present_out = &ansi;
        } else {
            opt.present_fd = 1;
            // Home cursor each frame for overwrite
            std::print("\033[H");
        }

        const auto st = render_frame(cam, vol, mats, kDefaultMaterialCount, shade, pf, opt);
        sum_ms += st.elapsed_ms;
        sum_hits += st.hits;

        if (!args.headless) {
            // Status line after frame ANSI
            std::println("\n#1985 demo  frame {}/{}  px={} hits={}  {:.1f} ms  ≈{:.1f} FPS", f + 1,
                         args.frames, st.pixels, st.hits, st.elapsed_ms, st.fps());
            std::fflush(stdout);
            // Pace roughly 20 FPS if rendering is faster
            if (st.elapsed_ms < 50.0)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(static_cast<int>(50.0 - st.elapsed_ms)));
        } else if (args.bench || f == args.frames - 1) {
            std::println("frame {}  px={} hits={} misses={}  {:.3f} ms  ≈{:.1f} FPS  ansi={}B",
                         f + 1, st.pixels, st.hits, st.misses, st.elapsed_ms, st.fps(),
                         st.present_bytes);
        }
    }

    if (!args.headless) {
        std::print("\033[?25h"); // show cursor
        std::fflush(stdout);
    }

    const double avg = sum_ms / static_cast<double>(args.frames);
    std::println("\n#1985 summary: {} frames  avg {:.2f} ms  ≈{:.1f} FPS  total_hits={}  "
                 "cells={}×{}  vol={}³",
                 args.frames, avg, avg > 0 ? 1000.0 / avg : 0.0, sum_hits, args.cols, args.rows,
                 args.vol);
    std::println("soft target: 80×48 cells ≥15–20 FPS on modern laptop (Phase 1)");
    return 0;
}
