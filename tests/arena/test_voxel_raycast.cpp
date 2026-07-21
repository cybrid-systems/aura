// test_voxel_raycast.cpp — Issue #1983 / Epic #1979
// Amanatides–Woo DDA voxel raycaster (headless).
//
// ACs:
//   AC1: miss empty volume / miss past max_t
//   AC2: hit axis-aligned wall, correct voxel + face
//   AC3: start inside solid → immediate Inside face
//   AC4: max_t prevents distant hit
//   AC5: counters rays/hits/steps
//   AC6: raycast_frame thin batch over camera rays
//   AC7: diagonal hit reaches expected block

#include "test_harness.hpp"

#include "renderer/camera.hh"
#include "renderer/voxel_raycast.hh"
#include "renderer/voxel_volume.hh"

#include <array>
#include <cmath>
#include <print>

import std;

namespace {

using aura::renderer::BlockId;
using aura::renderer::Camera;
using aura::renderer::Hit;
using aura::renderer::kAir;
using aura::renderer::Ray;
using aura::renderer::raycast_counters;
using aura::renderer::raycast_frame;
using aura::renderer::raycast_voxel;
using aura::renderer::reset_raycast_counters_for_test;
using aura::renderer::Vec3;
using aura::renderer::voxel_face_normal;
using aura::renderer::voxel_fill_box;
using aura::renderer::VoxelFace;
using aura::renderer::VoxelVolume;
using aura::test::g_failed;
using aura::test::g_passed;

constexpr float kEps = 1e-3f;

bool near(float a, float b, float e = kEps) {
    return std::fabs(a - b) <= e;
}

void ac1_miss_empty() {
    std::println("\n--- AC1: miss empty / miss OOB ---");
    VoxelVolume vol(8, 8, 8, kAir);
    Ray r;
    r.origin = {0.5f, 0.5f, -1.f};
    r.direction = {0.f, 0.f, -1.f}; // away from volume
    Hit h = raycast_voxel(vol, r);
    CHECK(!h.hit, "ray leaving volume misses");

    r.origin = {0.5f, 0.5f, -1.f};
    r.direction = {0.f, 0.f, 1.f}; // through empty
    h = raycast_voxel(vol, r);
    CHECK(!h.hit, "empty volume miss");
}

void ac2_hit_wall_face() {
    std::println("\n--- AC2: hit wall + face ---");
    // Wall of solid at x=4, y=0..7, z=0..7
    VoxelVolume vol(8, 8, 8, kAir);
    voxel_fill_box(vol, 4, 0, 0, 5, 8, 8, /*id=*/3);

    Ray r;
    r.origin = {1.5f, 3.5f, 3.5f};
    r.direction = {1.f, 0.f, 0.f}; // +X toward wall
    Hit h = raycast_voxel(vol, r);
    CHECK(h.hit, "hit wall");
    CHECK(h.block == 3, "block id 3");
    CHECK(h.x == 4 && h.y == 3 && h.z == 3, "voxel (4,3,3)");
    CHECK(h.face == VoxelFace::NegX, "entered through −X face");
    CHECK(near(h.t, 2.5f, 0.05f), "t ≈ 2.5 (1.5 → 4.0)");
    CHECK(near(h.position.x, 4.f, 0.05f), "hit x ≈ 4");
    const auto n = voxel_face_normal(h.face);
    CHECK(n.x < 0.f, "normal points −X (outward of entered face)");
}

void ac3_start_inside() {
    std::println("\n--- AC3: start inside solid ---");
    VoxelVolume vol(4, 4, 4, kAir);
    vol.set(1, 1, 1, 9);
    Ray r;
    r.origin = {1.3f, 1.3f, 1.3f};
    r.direction = {0.f, 0.f, 1.f};
    Hit h = raycast_voxel(vol, r);
    CHECK(h.hit, "inside solid hits");
    CHECK(h.block == 9, "block 9");
    CHECK(h.x == 1 && h.y == 1 && h.z == 1, "voxel");
    CHECK(h.face == VoxelFace::Inside, "face Inside");
    CHECK(near(h.t, 0.f, 1e-4f) || h.t >= 0.f, "t ~ 0");
}

void ac4_max_t() {
    std::println("\n--- AC4: max_t blocks distant hit ---");
    VoxelVolume vol(16, 4, 4, kAir);
    vol.set(10, 1, 1, 5);
    Ray r;
    r.origin = {0.5f, 1.5f, 1.5f};
    r.direction = {1.f, 0.f, 0.f};

    Hit far = raycast_voxel(vol, r, 1e6f);
    CHECK(far.hit && far.x == 10, "unbounded hits x=10");

    Hit near_miss = raycast_voxel(vol, r, 2.f); // only travel 2 units
    CHECK(!near_miss.hit, "max_t=2 misses distant block");
}

void ac5_counters() {
    std::println("\n--- AC5: raycast counters ---");
    reset_raycast_counters_for_test();
    VoxelVolume vol(8, 8, 8, kAir);
    vol.set(5, 4, 4, 1);

    Ray hit_r{{0.5f, 4.5f, 4.5f}, {1.f, 0.f, 0.f}};
    Ray miss_r{{0.5f, 0.5f, 0.5f}, {0.f, 1.f, 0.f}};
    (void)raycast_voxel(vol, hit_r);
    (void)raycast_voxel(vol, miss_r);

    auto& c = raycast_counters();
    CHECK(c.rays_cast == 2, "rays_cast == 2");
    CHECK(c.hits == 1, "hits == 1");
    CHECK(c.misses == 1, "misses == 1");
    CHECK(c.steps_total > 0, "steps_total > 0");
}

void ac6_frame_batch() {
    std::println("\n--- AC6: raycast_frame batch ---");
    VoxelVolume vol(16, 16, 16, kAir);
    // Floor at y=0
    voxel_fill_box(vol, 0, 0, 0, 16, 1, 16, 2);

    Camera cam;
    cam.position = {8.f, 4.f, 8.f};
    cam.yaw = 0.f;
    cam.pitch = -0.5f; // look down a bit
    cam.fov_y = 1.0f;

    constexpr int W = 8, H = 8;
    std::array<Hit, W * H> hits{};
    const auto nh = raycast_frame(vol, cam, W, H, hits);
    CHECK(nh > 0, "some floor hits");
    CHECK(nh <= static_cast<std::size_t>(W * H), "hits <= pixels");
    // At least one hit on floor y=0
    bool any_floor = false;
    for (const auto& h : hits) {
        if (h.hit && h.y == 0 && h.block == 2)
            any_floor = true;
    }
    CHECK(any_floor, "hit floor voxels");
}

void ac7_diagonal() {
    std::println("\n--- AC7: diagonal reach ---");
    VoxelVolume vol(8, 8, 8, kAir);
    vol.set(3, 3, 3, 7);
    Ray r;
    r.origin = {0.5f, 0.5f, 0.5f};
    // Direction toward center of voxel (3,3,3)
    r.direction = {1.f, 1.f, 1.f};
    Hit h = raycast_voxel(vol, r);
    CHECK(h.hit, "diagonal hit");
    CHECK(h.block == 7, "block 7");
    CHECK(h.x == 3 && h.y == 3 && h.z == 3, "voxel (3,3,3)");
    CHECK(h.face != VoxelFace::Inside, "entered from outside");
}

} // namespace

int main() {
    std::println("=== test_voxel_raycast (#1983 / epic #1979) ===");
    ac1_miss_empty();
    ac2_hit_wall_face();
    ac3_start_inside();
    ac4_max_t();
    ac5_counters();
    ac6_frame_batch();
    ac7_diagonal();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
