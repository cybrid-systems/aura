// test_camera_rays.cpp — Issue #1981 / Epic #1979
// Camera + primary ray generation (headless, no TUI).
//
// ACs:
//   AC1: yaw=0,pitch=0 → center ray along −Z (forward)
//   AC2: pitch clamp prevents flip past ±limit
//   AC3: corner rays have expected half-FOV angles (approx)
//   AC4: generate_primary_rays fills w*h without needing heap in API
//   AC5: right-handed basis: right × forward ≈ up
//   AC6: pixel y-down → top pixels look slightly up (positive dir.y)

#include "test_harness.hpp"

#include "renderer/camera.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <print>

import std;

namespace {

using aura::renderer::Camera;
using aura::renderer::camera_basis;
using aura::renderer::camera_clamp_pitch;
using aura::renderer::camera_ray;
using aura::renderer::generate_primary_rays;
using aura::renderer::kCameraPitchLimit;
using aura::renderer::Ray;
using aura::renderer::Vec3;
using aura::renderer::vec3_cross;
using aura::renderer::vec3_dot;
using aura::renderer::vec3_length;
using aura::renderer::vec3_normalize;
using aura::test::g_failed;
using aura::test::g_passed;

constexpr float kEps = 1e-4f;

bool near(float a, float b, float eps = kEps) {
    return std::fabs(a - b) <= eps;
}

bool near_vec(Vec3 a, Vec3 b, float eps = 1e-3f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

void ac1_center_ray_forward() {
    std::println("\n--- AC1: center ray along camera forward (−Z) ---");
    Camera cam;
    cam.position = {1.f, 2.f, 3.f};
    cam.yaw = 0.f;
    cam.pitch = 0.f;
    cam.fov_y = 1.04719755f; // 60°
    cam.aspect = 1.f;

    // Odd resolution so pixel (w/2,h/2) is the true center sample.
    const int w = 65, h = 65;
    const Ray r = camera_ray(cam, w / 2, h / 2, w, h);
    CHECK(near_vec(r.origin, cam.position), "origin == camera.position");
    CHECK(near(vec3_length(r.direction), 1.f, 1e-5f), "direction unit length");
    // Center should match basis.forward ≈ (0,0,-1)
    const auto b = camera_basis(cam);
    CHECK(near_vec(b.forward, Vec3{0.f, 0.f, -1.f}, 1e-4f), "forward is −Z");
    CHECK(near_vec(r.direction, b.forward, 2e-3f), "center ray ≈ forward");
    // Dot with forward should be ~1
    CHECK(vec3_dot(r.direction, b.forward) > 0.999f, "center aligns with forward");
}

void ac2_pitch_clamp() {
    std::println("\n--- AC2: pitch clamp ---");
    CHECK(near(camera_clamp_pitch(10.f), kCameraPitchLimit), "large +pitch clamped");
    CHECK(near(camera_clamp_pitch(-10.f), -kCameraPitchLimit), "large −pitch clamped");
    CHECK(near(camera_clamp_pitch(0.5f), 0.5f), "in-range pitch unchanged");

    Camera cam;
    cam.pitch = 100.f; // absurd
    cam.yaw = 0.f;
    const auto b = camera_basis(cam);
    // Forward.y should be sin(limit) < 1, not flipped past poles
    CHECK(b.forward.y > 0.f && b.forward.y < 1.f, "clamped look-up has 0<fy<1");
    CHECK(std::fabs(b.forward.y - std::sin(kCameraPitchLimit)) < 1e-4f, "fy == sin(limit)");
    // Basis still orthonormal-ish
    CHECK(std::fabs(vec3_dot(b.forward, b.right)) < 1e-3f, "forward ⊥ right");
    CHECK(std::fabs(vec3_dot(b.forward, b.up)) < 1e-3f, "forward ⊥ up");
    CHECK(std::fabs(vec3_dot(b.right, b.up)) < 1e-3f, "right ⊥ up");
}

void ac3_corner_fov_angles() {
    std::println("\n--- AC3: corner rays vs half FOV ---");
    Camera cam;
    cam.yaw = 0.f;
    cam.pitch = 0.f;
    cam.fov_y = 1.04719755f; // 60° → half = 30°
    cam.aspect = 1.f;
    const int w = 100, h = 100;
    const auto b = camera_basis(cam);

    // Top-center pixel: should tilt up by ≈ half FOV
    const Ray top = camera_ray(cam, w / 2, 0, w, h);
    const float angle_up = std::acos(std::clamp(vec3_dot(top.direction, b.forward), -1.f, 1.f));
    const float half_fov = cam.fov_y * 0.5f;
    CHECK(std::fabs(angle_up - half_fov) < 0.05f, "top-center ≈ +half FOV from forward");
    CHECK(top.direction.y > 0.f, "top ray has +dir.y");

    // Bottom-center
    const Ray bot = camera_ray(cam, w / 2, h - 1, w, h);
    const float angle_dn = std::acos(std::clamp(vec3_dot(bot.direction, b.forward), -1.f, 1.f));
    CHECK(std::fabs(angle_dn - half_fov) < 0.05f, "bottom-center ≈ half FOV");
    CHECK(bot.direction.y < 0.f, "bottom ray has −dir.y");

    // Right-center: +X component
    const Ray right = camera_ray(cam, w - 1, h / 2, w, h);
    CHECK(right.direction.x > 0.f, "right ray has +dir.x");
    const Ray left = camera_ray(cam, 0, h / 2, w, h);
    CHECK(left.direction.x < 0.f, "left ray has −dir.x");
}

void ac4_batch_generate() {
    std::println("\n--- AC4: generate_primary_rays batch ---");
    Camera cam;
    cam.yaw = 0.f;
    cam.pitch = 0.f;
    constexpr int w = 8, h = 4;
    std::array<Ray, w * h> rays{};
    const auto n = generate_primary_rays(cam, w, h, rays);
    CHECK(n == static_cast<std::size_t>(w * h), "wrote w*h rays");
    // Center-ish matches single camera_ray
    const Ray single = camera_ray(cam, 3, 2, w, h);
    const Ray& batch = rays[static_cast<std::size_t>(2) * w + 3];
    CHECK(near_vec(single.direction, batch.direction, 1e-5f), "batch matches single");
    // Too-small span → 0
    std::array<Ray, 2> tiny{};
    CHECK(generate_primary_rays(cam, w, h, tiny) == 0, "short span rejected");
    CHECK(generate_primary_rays(cam, 0, h, rays) == 0, "zero width rejected");
}

void ac5_right_handed_basis() {
    std::println("\n--- AC5: right-handed orthonormal basis ---");
    Camera cam;
    cam.yaw = 0.7f;
    cam.pitch = 0.3f;
    const auto b = camera_basis(cam);
    const Vec3 recon = vec3_cross(b.right, b.forward);
    // up should ≈ cross(right, forward) for right-handed looking along forward
    // We defined up = cross(right, forward)
    CHECK(near_vec(b.up, recon, 1e-4f), "up == cross(right, forward)");
    CHECK(near(vec3_length(b.forward), 1.f), "forward unit");
    CHECK(near(vec3_length(b.right), 1.f), "right unit");
    CHECK(near(vec3_length(b.up), 1.f), "up unit");
}

void ac6_yaw_rotation() {
    std::println("\n--- AC6: yaw turns left toward +X ---");
    Camera cam;
    cam.yaw = 0.f;
    cam.pitch = 0.f;
    const auto b0 = camera_basis(cam);
    cam.yaw = 1.5707963f; // +90°
    const auto b1 = camera_basis(cam);
    // After +90° yaw, forward should ≈ (−1, 0, 0) wait:
    // f = (sin(yaw)*cos(pitch), sin(pitch), -cos(yaw)*cos(pitch))
    // yaw=π/2 → (1, 0, 0)
    CHECK(near_vec(b0.forward, Vec3{0.f, 0.f, -1.f}, 1e-4f), "yaw0 forward −Z");
    CHECK(near_vec(b1.forward, Vec3{1.f, 0.f, 0.f}, 1e-3f), "yaw+90 forward +X");
}

} // namespace

int main() {
    std::println("=== test_camera_rays (#1981 / epic #1979) ===");
    ac1_center_ray_forward();
    ac2_pitch_clamp();
    ac3_corner_fov_angles();
    ac4_batch_generate();
    ac5_right_handed_basis();
    ac6_yaw_rotation();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
