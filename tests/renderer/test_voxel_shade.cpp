// test_voxel_shade.cpp — Issue #1984 / Epic #1979
// Materials, face shading, fog, sky (pure, headless).
//
// ACs:
//   AC1: face factors differ (+Y > sides > −Y)
//   AC2: shade_hit multiplies albedo by face factor
//   AC3: fog tints/darkens with distance
//   AC4: sky gradient zenith vs horizon
//   AC5: shade_ray miss → sky, hit → material
//   AC6: ambient lifts dark faces
//   AC7: default material palette + OOB magenta

#include "test_harness.hpp"

#include "renderer/voxel_shade.hh"

#include <cmath>
#include <print>

import std;

namespace {

using aura::renderer::Color32;
using aura::renderer::color_lerp;
using aura::renderer::color_scale;
using aura::renderer::default_materials;
using aura::renderer::default_shade_params;
using aura::renderer::face_shade_factor;
using aura::renderer::fog_factor;
using aura::renderer::Hit;
using aura::renderer::kDefaultMaterialCount;
using aura::renderer::Material;
using aura::renderer::material_albedo;
using aura::renderer::Ray;
using aura::renderer::shade_hit;
using aura::renderer::shade_ray;
using aura::renderer::shade_sky;
using aura::renderer::ShadeParams;
using aura::renderer::VoxelFace;
using aura::test::g_failed;
using aura::test::g_passed;

int luma(Color32 c) {
    return static_cast<int>(c.r) + static_cast<int>(c.g) + static_cast<int>(c.b);
}

void ac1_face_factors() {
    std::println("\n--- AC1: face shade factors ordered ---");
    const float top = face_shade_factor(VoxelFace::PosY);
    const float side = face_shade_factor(VoxelFace::PosX);
    const float bot = face_shade_factor(VoxelFace::NegY);
    const float inside = face_shade_factor(VoxelFace::Inside);
    CHECK(top > side && side > bot, "+Y > side > −Y");
    CHECK(bot > inside, "−Y > Inside");
    CHECK(std::fabs(top - 1.f) < 1e-5f, "+Y == 1.0");
    CHECK(std::fabs(side - 0.8f) < 1e-5f, "side == 0.8");
    CHECK(std::fabs(bot - 0.6f) < 1e-5f, "−Y == 0.6");
}

void ac2_face_multiplies_albedo() {
    std::println("\n--- AC2: face multiplies albedo ---");
    Material mats[4] = {
        Material{Color32::from_rgb(0, 0, 0)},
        Material{Color32::from_rgb(0, 0, 0)},
        Material{Color32::from_rgb(100, 100, 100)},
        Material{Color32::from_rgb(0, 0, 0)},
    };
    ShadeParams p = default_shade_params();
    p.fog_density = 0.f; // isolate face
    p.ambient = 0.f;

    Hit top{.hit = true, .block = 2, .face = VoxelFace::PosY, .t = 1.f};
    Hit side{.hit = true, .block = 2, .face = VoxelFace::PosX, .t = 1.f};
    Hit bot{.hit = true, .block = 2, .face = VoxelFace::NegY, .t = 1.f};
    Ray dummy{};

    const Color32 ct = shade_hit(top, dummy, mats, 4, p);
    const Color32 cs = shade_hit(side, dummy, mats, 4, p);
    const Color32 cb = shade_hit(bot, dummy, mats, 4, p);
    CHECK(ct.r == 100, "top unscaled 100");
    CHECK(cs.r == 80, "side 100*0.8");
    CHECK(cb.r == 60, "bot 100*0.6");
    CHECK(luma(ct) > luma(cs) && luma(cs) > luma(cb), "luma top > side > bot");
}

void ac3_distance_fog() {
    std::println("\n--- AC3: distance fog ---");
    CHECK(fog_factor(0.f, 0.1f) == 0.f, "t=0 no fog");
    CHECK(fog_factor(100.f, 0.f) == 0.f, "density=0 no fog");
    CHECK(fog_factor(100.f, 0.1f) > fog_factor(1.f, 0.1f), "far > near fog");

    Material mats[2] = {
        Material{},
        Material{Color32::from_rgb(0, 255, 0)},
    };
    ShadeParams p;
    p.fog_color = Color32::from_rgb(255, 0, 0);
    p.fog_density = 0.2f;
    p.ambient = 0.f;

    Hit near_h{.hit = true, .block = 1, .face = VoxelFace::PosY, .t = 0.5f};
    Hit far_h{.hit = true, .block = 1, .face = VoxelFace::PosY, .t = 40.f};
    Ray dummy{};
    const Color32 cn = shade_hit(near_h, dummy, mats, 2, p);
    const Color32 cf = shade_hit(far_h, dummy, mats, 2, p);
    // Far should be more red (fog), less green
    CHECK(cf.r > cn.r, "far more fog red");
    CHECK(cf.g < cn.g, "far less green albedo");
}

void ac4_sky_gradient() {
    std::println("\n--- AC4: sky zenith vs horizon ---");
    ShadeParams p;
    p.sky_zenith = Color32::from_rgb(0, 0, 255);
    p.sky_horizon = Color32::from_rgb(255, 255, 255);

    Ray up{{}, {0.f, 1.f, 0.f}};
    Ray horiz{{}, {0.f, 0.f, -1.f}};
    Ray down{{}, {0.f, -1.f, 0.f}};

    const Color32 cz = shade_sky(up, p);
    const Color32 ch = shade_sky(horiz, p);
    const Color32 cd = shade_sky(down, p);
    CHECK(cz.b > cz.r, "zenith bluish");
    CHECK(ch.r > cz.r || ch.g > cz.g, "horizon brighter/whiter than pure zenith blue");
    // Zenith should be closer to blue than horizon
    CHECK(cz.b >= ch.b || (255 - cz.r) > (255 - ch.r), "zenith more blue-biased");
    CHECK(luma(cd) <= luma(ch), "looking down not brighter than horizon");
}

void ac5_shade_ray_dispatch() {
    std::println("\n--- AC5: shade_ray hit vs miss ---");
    auto* mats = default_materials();
    ShadeParams p = default_shade_params();
    p.fog_density = 0.f;

    Hit miss{};
    Ray sky_r{{}, {0.f, 0.8f, -0.2f}};
    const Color32 sky = shade_ray(miss, sky_r, mats, kDefaultMaterialCount, p);
    const Color32 sky2 = shade_sky(sky_r, p);
    CHECK(sky == sky2, "miss == shade_sky");

    Hit hit{.hit = true, .block = 2, .face = VoxelFace::PosY, .t = 2.f};
    const Color32 solid = shade_ray(hit, sky_r, mats, kDefaultMaterialCount, p);
    CHECK(solid.g > solid.r, "grass block greenish");
    CHECK(!(solid == sky), "hit color ≠ sky");
}

void ac6_ambient() {
    std::println("\n--- AC6: ambient lifts dark faces ---");
    Material mats[2] = {
        Material{},
        Material{Color32::from_rgb(50, 50, 50)},
    };
    ShadeParams p0;
    p0.fog_density = 0.f;
    p0.ambient = 0.f;
    ShadeParams p1 = p0;
    p1.ambient = 0.2f;

    Hit bot{.hit = true, .block = 1, .face = VoxelFace::NegY, .t = 1.f};
    Ray dummy{};
    const Color32 c0 = shade_hit(bot, dummy, mats, 2, p0);
    const Color32 c1 = shade_hit(bot, dummy, mats, 2, p1);
    CHECK(luma(c1) > luma(c0), "ambient increases luma");
}

void ac7_palette_oob() {
    std::println("\n--- AC7: default palette + OOB magenta ---");
    auto* mats = default_materials();
    CHECK(material_albedo(mats, kDefaultMaterialCount, 1).r == 140, "stone gray");
    CHECK(material_albedo(mats, kDefaultMaterialCount, 2).g > 100, "grass green");
    const Color32 oob = material_albedo(mats, kDefaultMaterialCount, 99);
    CHECK(oob.r == 255 && oob.g == 0 && oob.b == 255, "OOB magenta");
    CHECK(material_albedo(nullptr, 0, 1).r == 255, "null table magenta");

    // color_lerp / scale helpers
    const Color32 mid = color_lerp(Color32::from_rgb(0, 0, 0), Color32::from_rgb(100, 0, 0), 0.5f);
    CHECK(mid.r == 50, "lerp midpoint");
    CHECK(color_scale(Color32::from_rgb(200, 0, 0), 0.5f).r == 100, "scale half");
}

} // namespace

int main() {
    std::println("=== test_voxel_shade (#1984 / epic #1979) ===");
    ac1_face_factors();
    ac2_face_multiplies_albedo();
    ac3_distance_fog();
    ac4_sky_gradient();
    ac5_shade_ray_dispatch();
    ac6_ambient();
    ac7_palette_oob();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
