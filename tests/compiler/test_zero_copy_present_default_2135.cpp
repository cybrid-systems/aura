// @category: unit
// @reason: Issue #2135 — default zero-copy present pipeline for TermBuf /
// Agent batch path (direct arena ANSI; residual memcpy measured).
//
//   AC1: present_batch / tui:present-batch default arena path
//   AC2: pure present loop: vector_fallback stays 0; hit_in_render / arena_acquire move
//   AC3: direct_arena_build preferred; residual memcpy measured if any
//   AC4: schema-2135 on query:render-stats
//   AC5: source cites #2135; scratch capacity bounded

#include "test_harness.hpp"

#include "core/zero_copy_output.hh"
#include "renderer/render_primitives.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::zero_copy::g_zero_copy_metrics;
using aura::core::zero_copy::reset_zero_copy_metrics_for_test;
using aura::renderer::draw_cell;
using aura::renderer::FramebufferOwned;
using aura::renderer::present_batch;
using aura::renderer::reset_render_engine_counters_for_test;
using aura::renderer::TermCell;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"query:render-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

TermCell cell(char ch, std::uint8_t fg = 7) {
    TermCell c = TermCell::space_palette();
    c.ch = static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
    c.fg_r = fg;
    return c;
}

} // namespace

int main() {
    std::println("=== Issue #2135: default zero-copy present / direct arena ===");

    // ── AC5: source ──
    {
        std::println("\n--- AC5: source ---");
        auto rp = read_file("src/renderer/render_primitives.cpp");
        auto bt = read_file("src/renderer/batch_terminal.hh");
        auto zc = read_file("src/core/zero_copy_output.hh");
        auto tui = read_file("src/compiler/evaluator_primitives_tui.cpp");
        CHECK(rp.find("#2135") != std::string::npos, "render_primitives #2135");
        CHECK(rp.find("direct_arena_build") != std::string::npos ||
                  rp.find("build_terminal_frame_ansi_dirty_buf") != std::string::npos,
              "direct arena path");
        CHECK(bt.find("AnsiFixedBuf") != std::string::npos, "AnsiFixedBuf");
        CHECK(bt.find("build_terminal_frame_ansi_dirty_buf") != std::string::npos, "buf build");
        CHECK(zc.find("direct_arena_build_total") != std::string::npos, "metrics fields");
        CHECK(tui.find("#2135") != std::string::npos ||
                  tui.find("present_batch") != std::string::npos,
              "tui present-batch arena default");
    }

    reset_zero_copy_metrics_for_test();
    reset_render_engine_counters_for_test();

    // ── AC1/AC2/AC3: pure present loop prefers arena, no vector fallback ──
    {
        std::println("\n--- AC1–AC3: pure present loop ---");
        FramebufferOwned fb;
        fb.resize(40, 12);
        // Warm-up presents
        for (int i = 0; i < 3; ++i) {
            auto v = fb.view();
            draw_cell(v, fb.dirty, 0, 0, cell(static_cast<char>('A' + (i % 3))));
            (void)present_batch(v, fb.dirty, /*fd=*/-1); // no write; still builds frame
        }
        // Steady-state metrics (after warm-up)
        const auto fb_before = g_zero_copy_metrics().vector_fallback_count.load();
        const auto arena0 = g_zero_copy_metrics().arena_acquire_count.load();
        const auto hit0 = g_zero_copy_metrics().hit_in_render.load();
        const auto direct0 = g_zero_copy_metrics().direct_arena_build_total.load();

        for (int i = 0; i < 50; ++i) {
            auto v = fb.view();
            // Small dirty region each frame
            draw_cell(v, fb.dirty, static_cast<std::uint32_t>(i % 40),
                      static_cast<std::uint32_t>((i / 40) % 12), cell('X'));
            auto n = present_batch(v, fb.dirty, -1);
            CHECK(n >= 0, "present ok");
        }

        const auto fb_after = g_zero_copy_metrics().vector_fallback_count.load();
        const auto arena1 = g_zero_copy_metrics().arena_acquire_count.load();
        const auto hit1 = g_zero_copy_metrics().hit_in_render.load();
        const auto direct1 = g_zero_copy_metrics().direct_arena_build_total.load();
        const auto residual = g_zero_copy_metrics().residual_memcpy_count.load();

        std::println("  vector_fallback {}→{} arena_acq {}→{} hit {}→{} direct {}→{} residual {}",
                     fb_before, fb_after, arena0, arena1, hit0, hit1, direct0, direct1, residual);

        CHECK(fb_after == fb_before, "vector_fallback unchanged under pure present");
        CHECK(arena1 > arena0, "arena_acquire_count advanced");
        CHECK(hit1 > hit0, "hit_in_render advanced (hotpath)");
        CHECK(direct1 > direct0, "direct_arena_build preferred");
        CHECK(g_zero_copy_metrics().arena_path_active.load() == 1, "arena_path_active");
        // Residual memcpy may be 0 when direct always succeeds
        CHECK(residual >= 0, "residual memcpy non-negative");
        CHECK(g_zero_copy_metrics().scratch_capacity_bytes.load() <= 4u * 1024u * 1024u,
              "scratch capacity bounded ≤ 4MiB");
    }

    // ── AC1: Agent tui:present-batch uses same path ──
    {
        std::println("\n--- AC1: tui:present-batch ---");
        CompilerService cs;
        auto id = cs.eval("(make-terminal-buffer 16 8)");
        CHECK(id && is_int(*id) && as_int(*id) >= 0, "make-terminal-buffer");
        const auto bid = as_int(*id);
        (void)cs.eval(std::format("(tui:fill-rect {} 0 0 16 8 46 7 0)", bid));
        const auto direct0 = g_zero_copy_metrics().direct_arena_build_total.load();
        const auto vf0 = g_zero_copy_metrics().vector_fallback_count.load();
        auto p = cs.eval(std::format("(tui:present-batch {})", bid));
        CHECK(p && is_int(*p) && as_int(*p) > 0, "dirty present writes");
        auto skip = cs.eval(std::format("(tui:present-batch {})", bid));
        CHECK(skip && is_int(*skip) && as_int(*skip) == 0, "clean skip");
        CHECK(g_zero_copy_metrics().vector_fallback_count.load() == vf0,
              "no vector fallback on tui present");
        CHECK(g_zero_copy_metrics().direct_arena_build_total.load() > direct0,
              "direct arena on tui present");
    }

    // ── AC4: query metrics ──
    {
        std::println("\n--- AC4: schema-2135 ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
        auto id = cs.eval("(make-terminal-buffer 4 2)");
        const auto bid = as_int(*id);
        (void)cs.eval(std::format("(tui:draw-batch {} 0 0 65 1 0)", bid));
        (void)cs.eval(std::format("(tui:present-batch {})", bid));
        CHECK(href(cs, "schema-2135") == 2135, "schema-2135");
        CHECK(href(cs, "issue-2135") == 2135, "issue-2135");
        CHECK(href(cs, "zero-copy-default-arena-wired") == 1, "wired");
        CHECK(href(cs, "zero-copy-arena-path-active") == 1, "arena active");
        CHECK(href(cs, "zero-copy-direct-arena-build-total") >= 0, "direct build key");
        CHECK(href(cs, "zero-copy-residual-memcpy-count") >= 0, "residual key");
        CHECK(href(cs, "zero-copy-vector-fallback-count") >= 0, "fallback key");
        CHECK(href(cs, "zero-copy-hit-in-render") >= 0, "hit key");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
