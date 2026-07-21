// camera.hh — Issue #1981 / Epic #1979: first-person camera + primary rays.
//
// Header-only math for software voxel raycasting. No TUI / TermCell dependency.
//
// Coordinate system (right-handed, Y-up):
//   - yaw = 0, pitch = 0 → forward = (0, 0, -1)  (looking down −Z)
//   - yaw increases → turn left (toward +X)
//   - pitch increases → look up (toward +Y)
//   - world_up = (0, 1, 0)
//
// Pixel convention matches PixelFramebuffer (#1980): origin top-left,
// y increases downward. NDC maps top → +v, bottom → −v on the image plane.

#ifndef AURA_RENDERER_CAMERA_HH
#define AURA_RENDERER_CAMERA_HH

#include <cmath>
#include <cstddef>
#include <span>

namespace aura::renderer {

inline constexpr int kCameraIssue = 1981;
inline constexpr int kCameraEpic = 1979;

// Max |pitch| to avoid gimbal flip (≈ 89.0°).
inline constexpr float kCameraPitchLimit = 1.553343f; // (π/2) - 0.017453f

struct Vec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;

    [[nodiscard]] constexpr Vec3 operator+(Vec3 o) const noexcept {
        return {x + o.x, y + o.y, z + o.z};
    }
    [[nodiscard]] constexpr Vec3 operator-(Vec3 o) const noexcept {
        return {x - o.x, y - o.y, z - o.z};
    }
    [[nodiscard]] constexpr Vec3 operator*(float s) const noexcept { return {x * s, y * s, z * s}; }
    [[nodiscard]] constexpr Vec3 operator-() const noexcept { return {-x, -y, -z}; }
};

[[nodiscard]] constexpr float vec3_dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] constexpr Vec3 vec3_cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] inline float vec3_length(Vec3 v) noexcept {
    return std::sqrt(vec3_dot(v, v));
}

[[nodiscard]] inline Vec3 vec3_normalize(Vec3 v) noexcept {
    const float len = vec3_length(v);
    if (len <= 0.f)
        return {0.f, 0.f, -1.f};
    const float inv = 1.f / len;
    return {v.x * inv, v.y * inv, v.z * inv};
}

struct Ray {
    Vec3 origin{};
    Vec3 direction{}; // normalized
};

struct Camera {
    Vec3 position{};
    float yaw = 0.f;           // radians
    float pitch = 0.f;         // radians (clamped on use)
    float fov_y = 1.04719755f; // ~60° default
    float aspect = 1.f;        // pixel_w / pixel_h (used if w/h not passed)
};

// Clamp pitch into (−kCameraPitchLimit, +kCameraPitchLimit).
[[nodiscard]] inline float camera_clamp_pitch(float pitch) noexcept {
    if (pitch > kCameraPitchLimit)
        return kCameraPitchLimit;
    if (pitch < -kCameraPitchLimit)
        return -kCameraPitchLimit;
    return pitch;
}

// Orthonormal basis for the camera (forward / right / up), pitch clamped.
struct CameraBasis {
    Vec3 forward{};
    Vec3 right{};
    Vec3 up{};
};

[[nodiscard]] inline CameraBasis camera_basis(const Camera& cam) noexcept {
    const float pitch = camera_clamp_pitch(cam.pitch);
    const float cy = std::cos(cam.yaw);
    const float sy = std::sin(cam.yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    // yaw=0,pitch=0 → (0, 0, -1)
    const Vec3 forward = vec3_normalize(Vec3{sy * cp, sp, -cy * cp});
    const Vec3 world_up{0.f, 1.f, 0.f};
    // right-handed: right = normalize(cross(forward, world_up))?
    // cross((0,0,-1),(0,1,0)) = (1,0,0) — good.
    Vec3 right = vec3_cross(forward, world_up);
    // Near ±90° pitch, forward ‖ world_up → right degenerates; use yaw-only fallback.
    if (vec3_dot(right, right) < 1e-12f) {
        right = Vec3{cy, 0.f, sy}; // perpendicular on XZ
    } else {
        right = vec3_normalize(right);
    }
    const Vec3 up = vec3_normalize(vec3_cross(right, forward));
    return CameraBasis{forward, right, up};
}

// Generate primary ray for pixel (px, py). Origin = camera.position.
// No heap allocation. Pixel center sampling (px+0.5, py+0.5).
[[nodiscard]] inline Ray camera_ray(const Camera& cam, int px, int py, int pixel_w,
                                    int pixel_h) noexcept {
    Ray ray;
    ray.origin = cam.position;
    if (pixel_w <= 0 || pixel_h <= 0) {
        ray.direction = camera_basis(cam).forward;
        return ray;
    }
    const CameraBasis b = camera_basis(cam);
    const float aspect =
        cam.aspect > 0.f ? cam.aspect : static_cast<float>(pixel_w) / static_cast<float>(pixel_h);
    const float tan_half = std::tan(cam.fov_y * 0.5f);
    // NDC: x ∈ [-1,1] left→right; y ∈ [-1,1] bottom→top (flip terminal y-down).
    const float ndc_x = (2.f * (static_cast<float>(px) + 0.5f) / static_cast<float>(pixel_w)) - 1.f;
    const float ndc_y = 1.f - (2.f * (static_cast<float>(py) + 0.5f) / static_cast<float>(pixel_h));
    const float plane_x = ndc_x * aspect * tan_half;
    const float plane_y = ndc_y * tan_half;
    // Image plane at unit distance along forward.
    ray.direction = vec3_normalize(b.forward + b.right * plane_x + b.up * plane_y);
    return ray;
}

// Batch fill: out.size() must be >= w*h. Writes row-major (y major outer).
// Returns number of rays written (w*h), or 0 if args invalid / span too small.
// Does not allocate; caller owns storage.
inline std::size_t generate_primary_rays(const Camera& cam, int w, int h,
                                         std::span<Ray> out) noexcept {
    if (w <= 0 || h <= 0)
        return 0;
    const std::size_t need = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (out.size() < need)
        return 0;
    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            out[static_cast<std::size_t>(py) * static_cast<std::size_t>(w) +
                static_cast<std::size_t>(px)] = camera_ray(cam, px, py, w, h);
        }
    }
    return need;
}

} // namespace aura::renderer

#endif // AURA_RENDERER_CAMERA_HH
