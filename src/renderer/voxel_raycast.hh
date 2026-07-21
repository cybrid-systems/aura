// voxel_raycast.hh — Issue #1983 / Epic #1979: Amanatides–Woo DDA voxel raycast.
//
// Pure function: (VoxelVolume, Ray) → Hit. No heap on the hot path.
// Depends on camera.hh (Ray/Vec3) and voxel_volume.hh (VoxelVolume/BlockId).
//
// Grid: unit voxels [x,x+1)×[y,y+1)×[z,z+1), Y-up (matches #1981/#1982).
// Solid test: block_id != kAir.
//
// Starting inside a solid voxel:
//   Report immediately with face = VoxelFace::Inside, t = 0, position = origin.
//   (Documented choice — do not step out first.)
//
// Face indices when entering a voxel by DDA step:
//   PosX=0 NegX=1 PosY=2 NegY=3 PosZ=4 NegZ=5
//   Stepping +X enters through NegX of the new cell, etc.

#ifndef AURA_RENDERER_VOXEL_RAYCAST_HH
#define AURA_RENDERER_VOXEL_RAYCAST_HH

#include "renderer/camera.hh"
#include "renderer/voxel_volume.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace aura::renderer {

inline constexpr int kVoxelRaycastIssue = 1983;
inline constexpr int kVoxelRaycastEpic = 1979;

enum class VoxelFace : int {
    Inside = -1, // ray origin already in solid
    None = -2,
    PosX = 0,
    NegX = 1,
    PosY = 2,
    NegY = 3,
    PosZ = 4,
    NegZ = 5,
};

struct Hit {
    bool hit = false;
    BlockId block = kAir;
    int x = 0;
    int y = 0;
    int z = 0;
    VoxelFace face = VoxelFace::None;
    float t = 0.f;   // distance along ray (origin + t * direction)
    Vec3 position{}; // world-space hit point
};

struct RaycastCounters {
    std::uint64_t rays_cast = 0;
    std::uint64_t hits = 0;
    std::uint64_t steps_total = 0; // DDA steps across all rays (for avg)
    std::uint64_t misses = 0;
};

[[nodiscard]] inline RaycastCounters& raycast_counters() noexcept {
    static RaycastCounters c{};
    return c;
}

inline void reset_raycast_counters_for_test() noexcept {
    raycast_counters() = {};
}

// Outward unit normal for a face (Inside/None → zero).
[[nodiscard]] inline Vec3 voxel_face_normal(VoxelFace f) noexcept {
    switch (f) {
        case VoxelFace::PosX:
            return {1.f, 0.f, 0.f};
        case VoxelFace::NegX:
            return {-1.f, 0.f, 0.f};
        case VoxelFace::PosY:
            return {0.f, 1.f, 0.f};
        case VoxelFace::NegY:
            return {0.f, -1.f, 0.f};
        case VoxelFace::PosZ:
            return {0.f, 0.f, 1.f};
        case VoxelFace::NegZ:
            return {0.f, 0.f, -1.f};
        default:
            return {0.f, 0.f, 0.f};
    }
}

namespace detail {

    inline constexpr float kInf = std::numeric_limits<float>::infinity();
    inline constexpr float kEps = 1e-6f;

    // Ray vs AABB [min,max] (inclusive max as continuous bounds of the grid).
    // On hit, t0 = enter, t1 = exit along ray. Returns false if no intersection.
    [[nodiscard]] inline bool ray_aabb(const Vec3& o, const Vec3& d, float minx, float miny,
                                       float minz, float maxx, float maxy, float maxz, float& t0,
                                       float& t1) noexcept {
        t0 = 0.f;
        t1 = kInf;
        const float* od[3] = {&o.x, &o.y, &o.z};
        const float* dd[3] = {&d.x, &d.y, &d.z};
        const float mn[3] = {minx, miny, minz};
        const float mx[3] = {maxx, maxy, maxz};
        for (int i = 0; i < 3; ++i) {
            const float origin = *od[i];
            const float dir = *dd[i];
            if (std::fabs(dir) < kEps) {
                if (origin < mn[i] || origin > mx[i])
                    return false;
                continue;
            }
            float inv = 1.f / dir;
            float ta = (mn[i] - origin) * inv;
            float tb = (mx[i] - origin) * inv;
            if (ta > tb)
                std::swap(ta, tb);
            t0 = ta > t0 ? ta : t0;
            t1 = tb < t1 ? tb : t1;
            if (t0 > t1)
                return false;
        }
        return true;
    }

    [[nodiscard]] inline bool is_solid(BlockId id) noexcept {
        return id != kAir;
    }

} // namespace detail

// Amanatides–Woo DDA. No heap allocation.
[[nodiscard]] inline Hit raycast_voxel(const VoxelVolume& vol, const Ray& ray,
                                       float max_t = 1e6f) noexcept {
    auto& ctr = raycast_counters();
    ++ctr.rays_cast;

    Hit miss{};
    if (!vol.valid() || max_t <= 0.f) {
        ++ctr.misses;
        return miss;
    }

    const Vec3 o = ray.origin;
    Vec3 d = ray.direction;
    // Degenerate direction → miss
    const float dlen2 = vec3_dot(d, d);
    if (dlen2 < 1e-20f) {
        ++ctr.misses;
        return miss;
    }
    // Accept non-unit; work in given scale (t is along this vector).
    // Prefer unit dirs from camera_ray; re-normalize for stability.
    d = vec3_normalize(d);

    const float wx = static_cast<float>(vol.size_x);
    const float wy = static_cast<float>(vol.size_y);
    const float wz = static_cast<float>(vol.size_z);

    float t_enter = 0.f;
    float t_exit = max_t;
    if (!detail::ray_aabb(o, d, 0.f, 0.f, 0.f, wx, wy, wz, t_enter, t_exit)) {
        ++ctr.misses;
        return miss;
    }
    if (t_enter < 0.f)
        t_enter = 0.f;
    if (t_exit > max_t)
        t_exit = max_t;
    if (t_enter > t_exit) {
        ++ctr.misses;
        return miss;
    }

    // Start just inside the volume along the ray.
    float t = t_enter;
    Vec3 p{o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
    // Nudge inward if sitting on a max face.
    constexpr float nudge = 1e-5f;
    if (p.x >= wx)
        p.x = wx - nudge;
    if (p.y >= wy)
        p.y = wy - nudge;
    if (p.z >= wz)
        p.z = wz - nudge;
    if (p.x < 0.f)
        p.x = 0.f;
    if (p.y < 0.f)
        p.y = 0.f;
    if (p.z < 0.f)
        p.z = 0.f;

    int x = static_cast<int>(std::floor(p.x));
    int y = static_cast<int>(std::floor(p.y));
    int z = static_cast<int>(std::floor(p.z));
    if (x >= vol.size_x)
        x = vol.size_x - 1;
    if (y >= vol.size_y)
        y = vol.size_y - 1;
    if (z >= vol.size_z)
        z = vol.size_z - 1;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (z < 0)
        z = 0;

    // Starting inside solid → immediate hit (documented choice).
    {
        const BlockId id0 = vol.get(x, y, z);
        if (detail::is_solid(id0)) {
            Hit h;
            h.hit = true;
            h.block = id0;
            h.x = x;
            h.y = y;
            h.z = z;
            h.face = VoxelFace::Inside;
            h.t = t;
            h.position = {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
            ++ctr.hits;
            return h;
        }
    }

    const int step_x = d.x > detail::kEps ? 1 : (d.x < -detail::kEps ? -1 : 0);
    const int step_y = d.y > detail::kEps ? 1 : (d.y < -detail::kEps ? -1 : 0);
    const int step_z = d.z > detail::kEps ? 1 : (d.z < -detail::kEps ? -1 : 0);

    const float t_delta_x = step_x != 0 ? std::fabs(1.f / d.x) : detail::kInf;
    const float t_delta_y = step_y != 0 ? std::fabs(1.f / d.y) : detail::kInf;
    const float t_delta_z = step_z != 0 ? std::fabs(1.f / d.z) : detail::kInf;

    auto next_boundary_t = [](float origin, float dir, int cell, int step) -> float {
        if (step == 0)
            return detail::kInf;
        if (step > 0)
            return (static_cast<float>(cell + 1) - origin) / dir;
        return (static_cast<float>(cell) - origin) / dir;
    };

    // tMax is absolute distance from ray origin to next plane.
    float t_max_x = next_boundary_t(o.x, d.x, x, step_x);
    float t_max_y = next_boundary_t(o.y, d.y, y, step_y);
    float t_max_z = next_boundary_t(o.z, d.z, z, step_z);
    // If we entered from outside, ensure t_max >= t_enter.
    if (t_max_x < t)
        t_max_x = t + (step_x != 0 ? t_delta_x : 0.f);
    if (t_max_y < t)
        t_max_y = t + (step_y != 0 ? t_delta_y : 0.f);
    if (t_max_z < t)
        t_max_z = t + (step_z != 0 ? t_delta_z : 0.f);

    VoxelFace last_face = VoxelFace::None;
    std::uint64_t steps = 0;
    constexpr int kMaxSteps = 1 << 20; // hard safety

    while (t <= t_exit && steps < static_cast<std::uint64_t>(kMaxSteps)) {
        // Advance to next voxel boundary.
        if (t_max_x <= t_max_y && t_max_x <= t_max_z) {
            t = t_max_x;
            t_max_x += t_delta_x;
            x += step_x;
            last_face = step_x > 0 ? VoxelFace::NegX : VoxelFace::PosX;
        } else if (t_max_y <= t_max_z) {
            t = t_max_y;
            t_max_y += t_delta_y;
            y += step_y;
            last_face = step_y > 0 ? VoxelFace::NegY : VoxelFace::PosY;
        } else {
            t = t_max_z;
            t_max_z += t_delta_z;
            z += step_z;
            last_face = step_z > 0 ? VoxelFace::NegZ : VoxelFace::PosZ;
        }
        ++steps;

        if (t > max_t || t > t_exit)
            break;
        if (!vol.in_bounds(x, y, z))
            break;

        const BlockId id = vol.get(x, y, z);
        if (detail::is_solid(id)) {
            Hit h;
            h.hit = true;
            h.block = id;
            h.x = x;
            h.y = y;
            h.z = z;
            h.face = last_face;
            h.t = t;
            h.position = {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
            ctr.steps_total += steps;
            ++ctr.hits;
            return h;
        }
    }

    ctr.steps_total += steps;
    ++ctr.misses;
    return miss;
}

// Thin batch: cast one ray per output pixel (row-major). out_hits size >= w*h.
// Returns number of hits. No extra heap.
inline std::size_t raycast_frame(const VoxelVolume& vol, const Camera& cam, int w, int h,
                                 std::span<Hit> out_hits, float max_t = 1e6f) noexcept {
    if (w <= 0 || h <= 0)
        return 0;
    const std::size_t need = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (out_hits.size() < need)
        return 0;
    std::size_t nhit = 0;
    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            const Ray r = camera_ray(cam, px, py, w, h);
            Hit h = raycast_voxel(vol, r, max_t);
            out_hits[static_cast<std::size_t>(py) * static_cast<std::size_t>(w) +
                     static_cast<std::size_t>(px)] = h;
            if (h.hit)
                ++nhit;
        }
    }
    return nhit;
}

} // namespace aura::renderer

#endif // AURA_RENDERER_VOXEL_RAYCAST_HH
