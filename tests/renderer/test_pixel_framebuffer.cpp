// test_pixel_framebuffer.cpp — Issue #1980 / Epic #1979
// Half-block pixel framebuffer layer (headless unit tests).
//
// ACs:
//   AC1: PixelFramebuffer from FramebufferOwned / SoA
//   AC2: pixel_set maps (x,y) → cell + upper/lower half correctly
//   AC3: round-trip present → ANSI contains truecolor + U+2580 ▀
//   AC4: dirty region is cell-granular (two pixels → one cell mark)
//   AC5: counters: pixels written / half-block cells touched
//   AC6: pixel_clear + fill_rect + blit helpers

#include "test_harness.hpp"

#include "renderer/pixel_framebuffer.hh"

#include <cstdint>
#include <print>
#include <string>

import std;

namespace {

using aura::renderer::Color32;
using aura::renderer::FramebufferOwned;
using aura::renderer::kHalfBlockGlyph;
using aura::renderer::pixel_blit_u32;
using aura::renderer::pixel_clear;
using aura::renderer::pixel_fill_rect;
using aura::renderer::pixel_framebuffer_counters;
using aura::renderer::pixel_framebuffer_from_owned;
using aura::renderer::pixel_get;
using aura::renderer::pixel_present_to_string;
using aura::renderer::pixel_set;
using aura::renderer::PixelFramebuffer;
using aura::renderer::reset_pixel_framebuffer_counters_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

void ac1_create_from_owned() {
    std::println("\n--- AC1: create from FramebufferOwned ---");
    FramebufferOwned fb;
    fb.resize(8, 4); // 8 cols × 4 rows → 8×8 pixels
    auto pf = pixel_framebuffer_from_owned(fb);
    CHECK(pf.valid(), "pixel framebuffer valid");
    CHECK(pf.pixel_width == 8, "pixel_width == 8");
    CHECK(pf.pixel_height == 8, "pixel_height == 8 (rows*2)");
    CHECK(pf.cell_cols() == 8, "cell_cols");
    CHECK(pf.cell_rows() == 4, "cell_rows");
}

void ac2_pixel_set_mapping() {
    std::println("\n--- AC2: pixel_set upper/lower mapping ---");
    FramebufferOwned fb;
    fb.resize(4, 2);
    auto pf = pixel_framebuffer_from_owned(fb);
    reset_pixel_framebuffer_counters_for_test();

    const auto red = Color32::from_rgb(255, 0, 0);
    const auto blue = Color32::from_rgb(0, 0, 255);
    CHECK(pixel_set(pf, 1, 0, red), "set upper pixel (1,0)");
    CHECK(pixel_set(pf, 1, 1, blue), "set lower pixel (1,1)");

    // Cell (1,0) should be half-block with fg=red, bg=blue
    const auto& cell = fb.storage[static_cast<std::size_t>(0 * 4 + 1)];
    CHECK(cell.ch == kHalfBlockGlyph, "glyph is U+2580");
    CHECK(cell.mode == 1, "RGB mode");
    CHECK(cell.fg_r == 255 && cell.fg_g == 0 && cell.fg_b == 0, "upper → fg red");
    CHECK(cell.bg_r == 0 && cell.bg_g == 0 && cell.bg_b == 255, "lower → bg blue");

    CHECK(pixel_get(pf, 1, 0) == red, "pixel_get upper");
    CHECK(pixel_get(pf, 1, 1) == blue, "pixel_get lower");
    CHECK(!pixel_set(pf, 99, 0, red), "OOB x rejected");
    CHECK(!pixel_set(pf, 0, 99, red), "OOB y rejected");
}

void ac3_present_ansi_truecolor_halfblock() {
    std::println("\n--- AC3: present ANSI truecolor + half-block ---");
    FramebufferOwned fb;
    fb.resize(2, 1);
    auto pf = pixel_framebuffer_from_owned(fb);
    pixel_clear(pf, Color32::from_rgb(0, 0, 0));
    CHECK(pixel_set(pf, 0, 0, Color32::from_rgb(10, 20, 30)), "upper");
    CHECK(pixel_set(pf, 0, 1, Color32::from_rgb(40, 50, 60)), "lower");

    std::string ansi;
    const auto n = pixel_present_to_string(pf, ansi);
    CHECK(n > 0, "present produced bytes");
    CHECK(!ansi.empty(), "ansi non-empty");
    // Truecolor SGR: 38;2;R;G;B (fg) and 48;2;R;G;B (bg)
    CHECK(ansi.find("38;2;10;20;30") != std::string::npos, "fg truecolor in ANSI");
    CHECK(ansi.find("48;2;40;50;60") != std::string::npos, "bg truecolor in ANSI");
    // U+2580 UTF-8 is E2 96 80
    CHECK(ansi.find("\xE2\x96\x80") != std::string::npos || ansi.find("▀") != std::string::npos,
          "half-block glyph in ANSI");
}

void ac4_dirty_cell_granularity() {
    std::println("\n--- AC4: dirty region cell granularity ---");
    FramebufferOwned fb;
    fb.resize(10, 5);
    fb.dirty.clear();
    auto pf = pixel_framebuffer_from_owned(fb);
    CHECK(fb.dirty.is_clean(), "starts clean after clear");

    // Two pixels in same cell → one dirty cell
    CHECK(pixel_set(pf, 3, 4, Color32::from_rgb(1, 2, 3)), "upper of cell (3,2)");
    CHECK(pixel_set(pf, 3, 5, Color32::from_rgb(4, 5, 6)), "lower of cell (3,2)");
    CHECK(fb.dirty.is_dirty(), "dirty after pixel writes");
    CHECK(fb.dirty.x0 == 3 && fb.dirty.x1 == 3, "dirty x is cell col 3");
    CHECK(fb.dirty.y0 == 2 && fb.dirty.y1 == 2, "dirty y is cell row 2 (pixel 4/5)");
    CHECK(fb.dirty.cell_count() == 1, "exactly one dirty cell");

    // Second cell expands AABB
    CHECK(pixel_set(pf, 7, 0, Color32::from_rgb(9, 9, 9)), "another cell");
    CHECK(fb.dirty.x0 == 3 && fb.dirty.x1 == 7, "AABB expanded in x");
    CHECK(fb.dirty.y0 == 0 && fb.dirty.y1 == 2, "AABB expanded in y");
}

void ac5_counters() {
    std::println("\n--- AC5: pixel / half-block counters ---");
    reset_pixel_framebuffer_counters_for_test();
    FramebufferOwned fb;
    fb.resize(4, 2);
    auto pf = pixel_framebuffer_from_owned(fb);
    CHECK(pixel_set(pf, 0, 0, Color32::from_rgb(1, 0, 0)), "p0");
    CHECK(pixel_set(pf, 0, 1, Color32::from_rgb(0, 1, 0)), "p1");
    CHECK(pixel_set(pf, 1, 0, Color32::from_rgb(0, 0, 1)), "p2");
    CHECK(pf.pixels_written == 3, "local pixels_written == 3");
    auto& c = pixel_framebuffer_counters();
    CHECK(c.pixels_written_total == 3, "global pixels_written_total == 3");
    CHECK(c.half_block_cells_touched_total >= 2, "cells touched >= 2");
    std::string out;
    (void)pixel_present_to_string(pf, out);
    CHECK(c.present_calls >= 1, "present_calls advanced");
}

void ac6_clear_fill_blit() {
    std::println("\n--- AC6: clear / fill_rect / blit_u32 ---");
    FramebufferOwned fb;
    fb.resize(4, 2);
    auto pf = pixel_framebuffer_from_owned(fb);
    pixel_clear(pf, Color32::from_rgb(11, 22, 33));
    CHECK(pixel_get(pf, 0, 0) == Color32::from_rgb(11, 22, 33), "clear upper");
    CHECK(pixel_get(pf, 0, 1) == Color32::from_rgb(11, 22, 33), "clear lower");

    pixel_fill_rect(pf, 1, 1, 2, 2, Color32::from_rgb(200, 100, 50));
    CHECK(pixel_get(pf, 1, 1) == Color32::from_rgb(200, 100, 50), "fill lower-left");
    CHECK(pixel_get(pf, 2, 2) == Color32::from_rgb(200, 100, 50), "fill upper-right of rect");

    std::uint32_t frame[8] = {
        0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, // row 0
        0x010203, 0x040506, 0x070809, 0x0A0B0C, // row 1
    };
    pixel_blit_u32(pf, frame, 4, 2);
    CHECK(pixel_get(pf, 0, 0) == Color32::from_u32(0xFF0000), "blit (0,0)");
    CHECK(pixel_get(pf, 3, 1) == Color32::from_u32(0x0A0B0C), "blit (3,1)");
}

} // namespace

int main() {
    std::println("=== test_pixel_framebuffer (#1980 / epic #1979) ===");
    ac1_create_from_owned();
    ac2_pixel_set_mapping();
    ac3_present_ansi_truecolor_halfblock();
    ac4_dirty_cell_granularity();
    ac5_counters();
    ac6_clear_fill_blit();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
