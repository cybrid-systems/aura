// @category: unit
// @reason: Issue #1559 — present_batch/draw_batch engine: dirty short-circuit,
// render hotpath enter/exit, zero-copy acquire, ANSI colored present, FramebufferSoA.

#include "test_harness.hpp"

#include "core/arena_auto_policy_stats.h"
#include "core/zero_copy_output.hh"
#include "renderer/render_primitives.hh"

#include <cstdint>
#include <fcntl.h>
#include <print>
#include <string>
#include <unistd.h>

using aura::renderer::DirtyRegion;
using aura::renderer::draw_batch;
using aura::renderer::DrawOp;
using aura::renderer::FramebufferOwned;
using aura::renderer::FramebufferSoA;
using aura::renderer::present_batch;
using aura::renderer::present_batch_to_string;
using aura::renderer::render_engine_counters;
using aura::renderer::reset_render_engine_counters_for_test;
using aura::renderer::TermCell;
using aura::test::g_failed;
using aura::test::g_passed;

namespace {

TermCell make_palette_cell(char ch, std::uint8_t fg = 7, std::uint8_t bg = 0) {
    TermCell c;
    c.ch = static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
    c.fg_r = fg;
    c.bg_r = bg;
    c.mode = 0;
    return c;
}

TermCell make_rgb_cell(char ch, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    TermCell c;
    c.ch = static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
    c.fg_r = r;
    c.fg_g = g;
    c.fg_b = b;
    c.bg_r = 0;
    c.bg_g = 0;
    c.bg_b = 0;
    c.mode = 1;
    return c;
}

// Pipe helper: present to write-end, read from read-end.
std::string present_to_pipe(FramebufferSoA& fb, DirtyRegion& dirty) {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0)
        return {};
    // Non-blocking read side so empty short-circuit doesn't hang.
    const int flags = ::fcntl(fds[0], F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

    const auto n = present_batch(fb, dirty, fds[1]);
    ::close(fds[1]);
    std::string out;
    if (n > 0) {
        out.resize(static_cast<std::size_t>(n));
        const auto rn = ::read(fds[0], out.data(), out.size());
        if (rn > 0)
            out.resize(static_cast<std::size_t>(rn));
        else
            out.clear();
    }
    ::close(fds[0]);
    return out;
}

} // namespace

int main() {
    reset_render_engine_counters_for_test();
    aura::core::arena_policy::render_hotpath_skip_total.store(0, std::memory_order_relaxed);
    aura::core::arena_policy::render_present_total.store(0, std::memory_order_relaxed);
    aura::core::arena_policy::render_draw_batch_total.store(0, std::memory_order_relaxed);
    aura::core::arena_policy::render_hotpath_enter_total.store(0, std::memory_order_relaxed);

    // ── AC1: DirtyRegion is_dirty / is_clean / mark / clear ──
    {
        DirtyRegion d;
        CHECK(d.is_clean(), "fresh DirtyRegion is_clean");
        CHECK(!d.is_dirty(), "fresh DirtyRegion !is_dirty");
        d.mark_dirty(2, 3);
        CHECK(d.is_dirty(), "after mark_dirty is_dirty");
        CHECK(d.x0 == 2 && d.y0 == 3 && d.x1 == 2 && d.y1 == 3, "AABB single cell");
        d.mark_dirty(5, 1);
        CHECK(d.x0 == 2 && d.y0 == 1 && d.x1 == 5 && d.y1 == 3, "AABB expands");
        d.clear();
        CHECK(d.is_clean() && !d.is_dirty(), "clear → clean");
        d.mark_all_dirty(10, 5);
        CHECK(d.is_dirty() && d.x0 == 0 && d.y0 == 0 && d.x1 == 9 && d.y1 == 4,
              "mark_all_dirty full AABB");
    }

    // ── AC2: FramebufferSoA valid / cell_count ──
    {
        FramebufferOwned owned;
        owned.resize(4, 2);
        auto fb = owned.view();
        CHECK(fb.valid(), "FramebufferSoA valid after resize");
        CHECK(fb.cell_count() == 8, "cell_count == w*h");
        FramebufferSoA empty{};
        CHECK(!empty.valid(), "default FramebufferSoA invalid");
    }

    // ── AC3: present_batch short-circuit when clean ──
    {
        reset_render_engine_counters_for_test();
        FramebufferOwned owned;
        owned.resize(3, 2);
        owned.dirty.clear(); // force clean
        auto fb = owned.view();
        const auto enter0 =
            aura::core::arena_policy::render_hotpath_enter_total.load(std::memory_order_relaxed);
        const auto n = present_batch(fb, owned.dirty, /*fd=*/-1);
        CHECK(n == 0, "clean present_batch returns 0");
        CHECK(render_engine_counters().present_skips == 1, "present_skips == 1");
        CHECK(aura::core::arena_policy::render_hotpath_skip_total.load(std::memory_order_relaxed) >=
                  1,
              "render_hotpath_skip_total bumped");
        const auto enter1 =
            aura::core::arena_policy::render_hotpath_enter_total.load(std::memory_order_relaxed);
        CHECK(enter1 == enter0, "short-circuit does not enter hotpath");
    }

    // ── AC4: draw_batch marks dirty + writes cell ──
    {
        reset_render_engine_counters_for_test();
        FramebufferOwned owned;
        owned.resize(5, 3);
        owned.dirty.clear();
        auto fb = owned.view();
        const auto enter0 =
            aura::core::arena_policy::render_hotpath_enter_total.load(std::memory_order_relaxed);
        DrawOp op{1, 1, make_palette_cell('A', 2, 0)};
        const auto n = draw_batch(fb, owned.dirty, std::span<const DrawOp>(&op, 1));
        CHECK(n == 1, "draw_batch wrote 1 cell");
        CHECK(owned.dirty.is_dirty(), "draw_batch marks dirty");
        CHECK(owned.storage[static_cast<std::size_t>(1 * 5 + 1)].ch ==
                  static_cast<std::uint32_t>('A'),
              "cell content written");
        const auto enter1 =
            aura::core::arena_policy::render_hotpath_enter_total.load(std::memory_order_relaxed);
        CHECK(enter1 == enter0 + 1, "draw_batch enters hotpath once");
        CHECK(aura::core::arena_policy::render_draw_batch_total.load(std::memory_order_relaxed) >=
                  1,
              "render_draw_batch_total bumped");
        CHECK(!aura::core::arena_policy::in_render_hotpath(), "hotpath exited after draw_batch");
    }

    // ── AC5: present_batch after draw → ANSI with color + zero-copy ──
    {
        reset_render_engine_counters_for_test();
        aura::core::zero_copy::g_zero_copy_fb.acquire_count = 0;
        aura::core::zero_copy::g_zero_copy_fb.release_count = 0;

        FramebufferOwned owned;
        owned.resize(4, 2);
        owned.dirty.clear();
        auto fb = owned.view();
        DrawOp ops[] = {
            {0, 0, make_palette_cell('H', 1, 0)},
            {1, 0, make_palette_cell('i', 1, 0)},
            {0, 1, make_rgb_cell('!', 255, 128, 0)},
        };
        CHECK(draw_batch(fb, owned.dirty, std::span<const DrawOp>(ops, 3)) == 3, "draw 3 cells");

        const auto enter0 =
            aura::core::arena_policy::render_hotpath_enter_total.load(std::memory_order_relaxed);
        const auto zc0 = aura::core::zero_copy::g_zero_copy_fb.acquire_count;
        std::string ansi;
        const auto n = present_batch_to_string(fb, owned.dirty, ansi);
        CHECK(n > 0, "present_batch_to_string wrote bytes");
        CHECK(static_cast<std::int64_t>(ansi.size()) == n, "ansi size matches return");
        CHECK(ansi.find("\033[?2026h") != std::string::npos, "ANSI has CSI sync begin");
        CHECK(ansi.find("\033[38;5;") != std::string::npos ||
                  ansi.find("\033[38;2;") != std::string::npos,
              "ANSI has SGR color");
        CHECK(ansi.find('H') != std::string::npos, "ANSI contains drawn 'H'");
        CHECK(aura::core::zero_copy::g_zero_copy_fb.acquire_count > zc0,
              "zero-copy acquire_view used");
        CHECK(render_engine_counters().zero_copy_acquires >= 1, "engine zero_copy_acquires");
        const auto enter1 =
            aura::core::arena_policy::render_hotpath_enter_total.load(std::memory_order_relaxed);
        CHECK(enter1 == enter0 + 1, "present enters hotpath once");
        CHECK(!owned.dirty.is_dirty(), "present clears dirty");
        CHECK(!aura::core::arena_policy::in_render_hotpath(), "hotpath exited after present");
    }

    // ── AC6: second present short-circuits (no dirty) ──
    {
        FramebufferOwned owned;
        owned.resize(2, 2);
        auto fb = owned.view();
        // resize left dirty; present once
        std::string ansi;
        CHECK(present_batch_to_string(fb, owned.dirty, ansi) > 0, "first present emits");
        const auto skips0 = render_engine_counters().present_skips;
        const auto n2 = present_batch_to_string(fb, owned.dirty, ansi);
        CHECK(n2 == 0, "second present short-circuits");
        CHECK(ansi.empty(), "skip clears out string");
        CHECK(render_engine_counters().present_skips == skips0 + 1, "skip counter advances");
    }

    // ── AC7: present to pipe (real write path) ──
    {
        FramebufferOwned owned;
        owned.resize(3, 1);
        auto fb = owned.view();
        DrawOp op{0, 0, make_palette_cell('Z', 3, 0)};
        (void)draw_batch(fb, owned.dirty, std::span<const DrawOp>(&op, 1));
        // mark already dirty from draw
        auto out = present_to_pipe(fb, owned.dirty);
        CHECK(!out.empty(), "pipe present produced output");
        CHECK(out.find('Z') != std::string::npos, "pipe output has cell char");
    }

    // ── AC8: out-of-bounds draw is ignored ──
    {
        FramebufferOwned owned;
        owned.resize(2, 2);
        owned.dirty.clear();
        auto fb = owned.view();
        DrawOp bad{9, 9, make_palette_cell('X')};
        CHECK(draw_batch(fb, owned.dirty, std::span<const DrawOp>(&bad, 1)) == 0,
              "OOB draw writes 0");
        CHECK(!owned.dirty.is_dirty(), "OOB draw does not dirty");
    }

    // ── AC9: invalid fb present returns -1 ──
    {
        DirtyRegion d;
        d.mark_dirty(0, 0);
        FramebufferSoA inv{};
        CHECK(present_batch(inv, d, -1) == -1, "invalid fb → -1");
    }

    // ── AC10: phase constants ──
    {
        CHECK(aura::renderer::kRenderPrimitivesPhase == 1, "kRenderPrimitivesPhase == 1");
        CHECK(aura::renderer::kRenderPrimitivesIssue == 1559, "kRenderPrimitivesIssue == 1559");
        CHECK(aura::core::zero_copy::kZeroCopyOutputPhase == 1, "zero_copy phase");
    }

    if (g_failed)
        return 1;
    std::println("render_primitives #1559: OK ({} passed)", g_passed);
    return 0;
}
