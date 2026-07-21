// @category: unit
// @reason: Issue #1562 — dirty-region differential present: partial ANSI emit,
// skip rate >60% under sparse mutations, metrics avg/p99, mutation guard.

#include "test_harness.hpp"

#include "renderer/batch_terminal.hh"
#include "renderer/render_pass.hh"
#include "renderer/render_primitives.hh"

#include <cstdint>
#include <print>
#include <random>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::renderer::build_terminal_frame_ansi;
using aura::renderer::build_terminal_frame_ansi_dirty;
using aura::renderer::dirty_cell_skip_rate;
using aura::renderer::dirty_cells_avg;
using aura::renderer::dirty_cells_p99;
using aura::renderer::DirtyRegion;
using aura::renderer::draw_batch;
using aura::renderer::DrawOp;
using aura::renderer::FramebufferOwned;
using aura::renderer::g_dirty_delta_metrics;
using aura::renderer::present_batch;
using aura::renderer::present_batch_to_string;
using aura::renderer::RenderDirtyMutationGuard;
using aura::renderer::reset_dirty_delta_metrics_for_test;
using aura::renderer::reset_render_engine_counters_for_test;
using aura::renderer::TermCell;
using aura::test::g_failed;
using aura::test::g_passed;

namespace {

TermCell cell_at(char ch, std::uint8_t fg = 7) {
    TermCell c;
    c.ch = static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
    c.fg_r = fg;
    c.mode = 0;
    return c;
}

std::int64_t href_m(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:render-dirty-delta-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    reset_dirty_delta_metrics_for_test();
    reset_render_engine_counters_for_test();

    // ── AC1: DirtyRegion cell_count / clamp / full_frame ──
    {
        DirtyRegion d;
        CHECK(d.is_clean() && d.cell_count() == 0, "clean cell_count 0");
        d.mark_dirty(2, 3);
        d.mark_dirty(5, 4);
        CHECK(d.cell_count() == 4 * 2, "AABB 4x2 cell_count"); // x:2..5 y:3..4
        CHECK(d.clamp_to(10, 10), "clamp ok");
        d.mark_all_dirty(8, 6);
        CHECK(d.is_full_frame(8, 6), "full frame");
        CHECK(!d.is_full_frame(16, 16), "not full of larger");
    }

    // ── AC2: dirty builder emits fewer cells than full ──
    {
        FramebufferOwned owned;
        owned.resize(20, 10);
        auto fb = owned.view();
        // paint whole buffer to known content
        for (std::int32_t y = 0; y < 10; ++y)
            for (std::int32_t x = 0; x < 20; ++x) {
                DrawOp op{static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                          cell_at('.')};
                (void)draw_batch(fb, owned.dirty, std::span<const DrawOp>(&op, 1));
            }
        owned.dirty.clear();
        // sparse: 3 cells
        DrawOp ops[] = {
            {1, 1, cell_at('A', 1)},
            {2, 1, cell_at('B', 2)},
            {1, 2, cell_at('C', 3)},
        };
        CHECK(draw_batch(fb, owned.dirty, std::span<const DrawOp>(ops, 3)) == 3, "draw 3");
        CHECK(owned.dirty.cell_count() == 2 * 2, "dirty AABB 2x2"); // (1,1)-(2,2)

        std::string full, partial;
        const auto full_sgr = build_terminal_frame_ansi(full, fb.width, fb.height, fb.cells_c());
        const auto part = build_terminal_frame_ansi_dirty(partial, fb.width, fb.height,
                                                          fb.cells_c(), owned.dirty);
        CHECK(part.cells_emitted == 4, "partial emits 4 AABB cells");
        CHECK(part.partial, "partial flag");
        CHECK(partial.size() < full.size(), "partial ANSI smaller than full");
        CHECK(partial.find('A') != std::string::npos, "partial has A");
        CHECK(full_sgr >= 1, "full has SGR");
        (void)full_sgr;
    }

    // ── AC2b: present_batch_to_string uses dirty path ──
    {
        reset_dirty_delta_metrics_for_test();
        FramebufferOwned owned;
        owned.resize(30, 15);
        auto fb = owned.view();
        owned.dirty.clear();
        DrawOp op{5, 7, cell_at('Z', 4)};
        CHECK(draw_batch(fb, owned.dirty, std::span<const DrawOp>(&op, 1)) == 1, "one cell");
        std::string out;
        const auto n = present_batch_to_string(fb, owned.dirty, out);
        CHECK(n > 0 && !out.empty(), "present partial bytes");
        CHECK(out.find('Z') != std::string::npos, "emitted Z");
        CHECK(g_dirty_delta_metrics().dirty_partial_presents.load() >= 1, "partial present metric");
        CHECK(g_dirty_delta_metrics().dirty_cells_emitted_total.load() == 1, "1 cell emitted");
        CHECK(owned.dirty.is_clean(), "dirty cleared after present");
    }

    // ── AC5: sparse mutation loop — skip rate > 60% ──
    {
        reset_dirty_delta_metrics_for_test();
        reset_render_engine_counters_for_test();
        constexpr int W = 40;
        constexpr int H = 20;
        constexpr int kFrames = 200;
        FramebufferOwned owned;
        owned.resize(W, H);
        auto fb = owned.view();
        // Initial full present (marks all dirty on resize)
        std::string sink;
        (void)present_batch_to_string(fb, owned.dirty, sink);

        std::mt19937 rng(42);
        std::uniform_int_distribution<int> dx(0, W - 1);
        std::uniform_int_distribution<int> dy(0, H - 1);
        // Sparse: 1–2 clustered cells (same 2x2 neighborhood) so AABB stays tiny.
        for (int f = 0; f < kFrames; ++f) {
            const int bx = dx(rng);
            const int by = dy(rng);
            DrawOp op0{
                static_cast<std::uint32_t>(bx), static_cast<std::uint32_t>(by),
                cell_at(static_cast<char>('a' + (f % 26)), static_cast<std::uint8_t>(f % 8))};
            (void)draw_batch(fb, owned.dirty, std::span<const DrawOp>(&op0, 1));
            if (f % 2 == 0) {
                const int x2 = std::min(bx + 1, W - 1);
                const int y2 = std::min(by + 1, H - 1);
                DrawOp op1{static_cast<std::uint32_t>(x2), static_cast<std::uint32_t>(y2),
                           cell_at('X', 1)};
                (void)draw_batch(fb, owned.dirty, std::span<const DrawOp>(&op1, 1));
            }
            (void)present_batch(fb, owned.dirty, /*fd=*/-1);
        }

        const double skip = dirty_cell_skip_rate();
        CHECK(skip > 0.60, std::format("skip rate {:.1f}% > 60%", skip * 100.0));
        CHECK(dirty_cells_avg() > 0.0, "dirty_cells_avg > 0");
        CHECK(dirty_cells_p99() >= 1, "dirty_cells_p99 >= 1");
        CHECK(g_dirty_delta_metrics().dirty_partial_presents.load() >=
                  static_cast<std::uint64_t>(kFrames / 2),
              "many partial presents");
    }

    // ── AC3: clean short-circuit increments dirty_region_skips ──
    {
        reset_dirty_delta_metrics_for_test();
        FramebufferOwned owned;
        owned.resize(4, 4);
        auto fb = owned.view();
        owned.dirty.clear();
        CHECK(present_batch(fb, owned.dirty, -1) == 0, "clean present 0");
        CHECK(g_dirty_delta_metrics().dirty_region_skips_total.load() >= 1, "region skips");
    }

    // ── AC6: RenderDirtyMutationGuard ──
    {
        DirtyRegion d;
        {
            RenderDirtyMutationGuard g(d);
            CHECK(aura::core::arena_policy::in_render_hotpath(), "guard enters hotpath");
            g.mark(1, 1);
            g.mark(2, 2);
            CHECK(g.marks == 2, "guard mark count");
            CHECK(d.is_dirty() && d.cell_count() == 4, "guard marked AABB");
        }
        CHECK(!aura::core::arena_policy::in_render_hotpath(), "guard exits hotpath");
    }

    // ── EDSL: terminal-mark-dirty + query stats ──
    {
        CompilerService cs;
        auto id = cs.eval("(make-terminal-buffer 10 5)");
        CHECK(id && is_int(*id) && as_int(*id) >= 0, "make buffer");
        const auto bid = as_int(*id);
        // First present consumes create-time full dirty
        (void)cs.eval(std::format("(terminal-present-batch {} -1)", bid));
        auto md = cs.eval(std::format("(terminal-mark-dirty {} 3 2)", bid));
        CHECK(md && is_bool(*md) && as_bool(*md), "terminal-mark-dirty");
        auto p = cs.eval(std::format("(terminal-present-batch {} -1)", bid));
        CHECK(p && is_int(*p) && as_int(*p) > 0, "present after mark-dirty");
        auto mad = cs.eval(std::format("(terminal-mark-all-dirty {})", bid));
        CHECK(mad && is_bool(*mad) && as_bool(*mad), "terminal-mark-all-dirty");

        auto h = cs.eval(R"((engine:metrics "query:render-dirty-delta-stats"))");
        CHECK(h && is_hash(*h), "dirty-delta-stats hash");
        CHECK(href_m(cs, "schema") == 1562, "schema 1562");
        CHECK(href_m(cs, "active") == 1, "active");
        CHECK(href_m(cs, "dirty-region-skips") >= 0, "skips field");
        CHECK(href_m(cs, "dirty-cells-emitted") >= 0, "emitted field");
        CHECK(href_m(cs, "dirty-cell-skip-rate-bp") >= 0, "skip-rate-bp field");
    }

    // Phase
    {
        CHECK(aura::renderer::kRenderPassPhase == 2, "kRenderPassPhase == 2");
        CHECK(aura::renderer::kRenderPassIssue == 1562, "issue 1562");
    }

    if (g_failed)
        return 1;
    std::println("dirty_delta_present #1562: OK ({} passed)", g_passed);
    return 0;
}
