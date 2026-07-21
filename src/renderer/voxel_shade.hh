// voxel_shade.hh — Issue #1984 / Epic #1979: materials, face shading, fog, sky.
//
// Pure functions: Hit/Ray → Color32. No TUI / terminal / heap on hot path.
// Depends on pixel_framebuffer.hh (Color32) and voxel_raycast.hh (Hit/Ray).
//
// Phase 1 pipeline for a pixel:
//   1. raycast_voxel → Hit
//   2. if hit: shade_hit (albedo × face factor × ambient, then fog)
//      else:   shade_sky (zenith → horizon by ray.direction.y)
//
// Face factors (directional look without lights):
//   +Y (top)  1.00
//   sides     0.80
//   −Y (bot)  0.60
//   Inside    0.50  (origin buried in solid)

#ifndef AURA_RENDERER_VOXEL_SHADE_HH
#define AURA_RENDERER_VOXEL_SHADE_HH

#include "renderer/pixel_framebuffer.hh"
#include "renderer/voxel_raycast.hh"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace aura::renderer {

inline constexpr int kVoxelShadeIssue = 1984;
inline constexpr int kVoxelShadeEpic = 1979;

struct Material {
    Color32 albedo{};
};

struct ShadeParams {
    Color32 fog_color = Color32::from_rgb(180, 200, 220);
    float fog_density = 0.04f; // higher → thicker fog (exp falloff)
    Color32 sky_zenith = Color32::from_rgb(40, 80, 180);
    Color32 sky_horizon = Color32::from_rgb(180, 200, 230);
    float ambient = 0.15f; // 0..1 added after face factor, before fog
};

// Default face luminance multipliers.
[[nodiscard]] inline float face_shade_factor(VoxelFace f) noexcept {
    switch (f) {
        case VoxelFace::PosY:
            return 1.00f;
        case VoxelFace::NegY:
            return 0.60f;
        case VoxelFace::PosX:
        case VoxelFace::NegX:
        case VoxelFace::PosZ:
        case VoxelFace::NegZ:
            return 0.80f;
        case VoxelFace::Inside:
            return 0.50f;
        default:
            return 0.80f;
    }
}

// Linear blend a→b by t in [0,1].
[[nodiscard]] inline Color32 color_lerp(Color32 a, Color32 b, float t) noexcept {
    if (t <= 0.f)
        return a;
    if (t >= 1.f)
        return b;
    const float u = 1.f - t;
    return Color32::from_rgb(
        static_cast<std::uint8_t>(u * static_cast<float>(a.r) + t * static_cast<float>(b.r) + 0.5f),
        static_cast<std::uint8_t>(u * static_cast<float>(a.g) + t * static_cast<float>(b.g) + 0.5f),
        static_cast<std::uint8_t>(u * static_cast<float>(a.b) + t * static_cast<float>(b.b) +
                                  0.5f));
}

// Scale RGB by factor, clamp to 0..255.
[[nodiscard]] inline Color32 color_scale(Color32 c, float s) noexcept {
    auto ch = [s](std::uint8_t v) -> std::uint8_t {
        const float x = static_cast<float>(v) * s;
        if (x <= 0.f)
            return 0;
        if (x >= 255.f)
            return 255;
        return static_cast<std::uint8_t>(x + 0.5f);
    };
    return Color32::from_rgb(ch(c.r), ch(c.g), ch(c.b));
}

// Additive ambient then clamp (cheap ambient term).
[[nodiscard]] inline Color32 color_add_ambient(Color32 c, float ambient) noexcept {
    if (ambient <= 0.f)
        return c;
    auto ch = [ambient](std::uint8_t v) -> std::uint8_t {
        const float x = static_cast<float>(v) + ambient * 255.f;
        if (x >= 255.f)
            return 255;
        if (x <= 0.f)
            return 0;
        return static_cast<std::uint8_t>(x + 0.5f);
    };
    return Color32::from_rgb(ch(c.r), ch(c.g), ch(c.b));
}

// Exponential fog factor: 1 - exp(-density * t). Result in [0,1].
[[nodiscard]] inline float fog_factor(float t, float density) noexcept {
    if (density <= 0.f || t <= 0.f)
        return 0.f;
    const float f = 1.f - std::exp(-density * t);
    if (f < 0.f)
        return 0.f;
    if (f > 1.f)
        return 1.f;
    return f;
}

// Lookup albedo; OOB / null / air → magenta debug or black.
[[nodiscard]] inline Color32 material_albedo(const Material* materials, std::size_t count,
                                             BlockId id) noexcept {
    if (!materials || count == 0)
        return Color32::from_rgb(255, 0, 255);
    if (static_cast<std::size_t>(id) >= count)
        return Color32::from_rgb(255, 0, 255);
    return materials[id].albedo;
}

// Sky: lerp horizon → zenith by upward component of direction.
// dir.y = +1 → zenith, dir.y = 0 → horizon, dir.y < 0 → slightly darker horizon.
[[nodiscard]] inline Color32 shade_sky(const Ray& ray, const ShadeParams& p) noexcept {
    // Normalize y into [0,1] for blend; below horizon still uses horizon tint.
    float up = ray.direction.y;
    // Map [-1,1] → [0,1] with bias so horizon sits at 0.
    float t = up; // 0 at horizon, 1 at zenith
    if (t < 0.f)
        t = 0.f;
    if (t > 1.f)
        t = 1.f;
    // Smoothstep for a softer band.
    t = t * t * (3.f - 2.f * t);
    Color32 c = color_lerp(p.sky_horizon, p.sky_zenith, t);
    // Optional slight darkening below horizon (looking down into void).
    if (ray.direction.y < 0.f) {
        const float down = std::min(1.f, -ray.direction.y);
        c = color_scale(c, 1.f - 0.35f * down);
    }
    return c;
}

// Full hit shade: face factor + ambient + distance fog.
// materials[id] must cover used BlockIds; missing → magenta.
[[nodiscard]] inline Color32 shade_hit(const Hit& hit, const Ray& /*ray*/,
                                       const Material* materials, std::size_t material_count,
                                       const ShadeParams& p) noexcept {
    if (!hit.hit)
        return Color32{}; // caller should use shade_sky

    Color32 albedo = material_albedo(materials, material_count, hit.block);
    const float face = face_shade_factor(hit.face);
    Color32 lit = color_scale(albedo, face);
    lit = color_add_ambient(lit, p.ambient);

    const float fog = fog_factor(hit.t, p.fog_density);
    return color_lerp(lit, p.fog_color, fog);
}

// Convenience: shade hit or sky in one call.
[[nodiscard]] inline Color32 shade_ray(const Hit& hit, const Ray& ray, const Material* materials,
                                       std::size_t material_count, const ShadeParams& p) noexcept {
    if (hit.hit)
        return shade_hit(hit, ray, materials, material_count, p);
    return shade_sky(ray, p);
}

// Built-in small palette for demos/tests (index = BlockId).
// 0 = air (unused for solids), 1 = stone, 2 = grass, 3 = dirt, 4 = sand, 5 = water-ish
inline constexpr std::size_t kDefaultMaterialCount = 8;

[[nodiscard]] inline const Material* default_materials() noexcept {
    static const Material kMats[kDefaultMaterialCount] = {
        Material{Color32::from_rgb(0, 0, 0)},       // 0 air
        Material{Color32::from_rgb(140, 140, 140)}, // 1 stone
        Material{Color32::from_rgb(60, 160, 60)},   // 2 grass
        Material{Color32::from_rgb(120, 80, 40)},   // 3 dirt
        Material{Color32::from_rgb(210, 190, 120)}, // 4 sand
        Material{Color32::from_rgb(40, 90, 180)},   // 5 water
        Material{Color32::from_rgb(200, 40, 40)},   // 6 red
        Material{Color32::from_rgb(240, 240, 240)}, // 7 white
    };
    return kMats;
}

// Default outdoor-ish params.
[[nodiscard]] inline ShadeParams default_shade_params() noexcept {
    return ShadeParams{};
}

} // namespace aura::renderer

#endif // AURA_RENDERER_VOXEL_SHADE_HH
