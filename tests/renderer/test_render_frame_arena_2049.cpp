// @category: unit
// @reason: Issue #2049 — dedicated render arena / bump allocator + linear-owned
// cell buffers for predictable frame times.
//
//   AC1: source cites #2049; RenderFrameArena double-buffer; LinearCellGrid
//   AC2: present uses dedicated arena; reset/swap totals advance
//   AC3: LinearCellGrid move-only; present via view works
//   AC4: frame-time samples / p99 proxy advance under multi-present
//   AC5: pure present loop: string_heap growth ~0; render_alloc_bytes grow then stabilize
//   AC6: query:render-memory-stats schema-2049 + metrics keys
//   AC7: GC safepoint still defers in render hotpath (soft-gate)

#include "test_harness.hpp"

#include "core/arena_auto_policy_stats.h"
#include "renderer/render_frame_arena.hh"
#include "renderer/render_primitives.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::renderer::draw_cell;
using aura::renderer::g_render_frame_arena_v2;
using aura::renderer::g_render_frame_metrics;
using aura::renderer::kRenderFrameArenaIssue;
using aura::renderer::kRenderFrameArenaPhase;
using aura::renderer::LinearCellGrid;
using aura::renderer::present_batch_to_string;
using aura::renderer::render_frame_time_avg_us;
using aura::renderer::render_frame_time_p99_us;
using aura::renderer::RenderFrameArena;
using aura::renderer::reset_render_engine_counters_for_test;
using aura::renderer::reset_render_frame_metrics_for_test;
using aura::renderer::TermCell;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (stats:get \"query:render-memory-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::int64_t as_i(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(expr);
    if (!r || !is_int(*r))
        return -999;
    return as_int(*r);
}

TermCell cell_ch(char ch) {
    TermCell c;
    c.ch = static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
    c.fg_r = 7;
    c.mode = 0;
    return c;
}

void ac1_source() {
    std::println("\n--- AC1: source #2049 ---");
    CHECK(kRenderFrameArenaPhase == 1, "phase");
    CHECK(kRenderFrameArenaIssue == 2049, "issue");
    auto hh = read_file("src/renderer/render_frame_arena.hh");
    auto prim = read_file("src/renderer/render_primitives.cpp");
    auto io = read_file("src/compiler/evaluator_primitives_io.cpp");
    CHECK(!hh.empty() && hh.find("#2049") != std::string::npos, "render_frame_arena.hh");
    CHECK(hh.find("RenderFrameArena") != std::string::npos, "RenderFrameArena");
    CHECK(hh.find("LinearCellGrid") != std::string::npos, "LinearCellGrid");
    CHECK(hh.find("double-buffer") != std::string::npos ||
              hh.find("Double-buffer") != std::string::npos,
          "double-buffer");
    CHECK(!prim.empty() && prim.find("#2049") != std::string::npos, "present #2049");
    CHECK(prim.find("end_frame") != std::string::npos, "end_frame on present");
    CHECK(prim.find("g_render_frame_arena_v2") != std::string::npos, "dedicated arena");
    CHECK(!io.empty() && io.find("schema-2049") != std::string::npos, "schema-2049");
    CHECK(io.find("render-alloc-bytes") != std::string::npos, "render-alloc-bytes key");
}

void ac2_present_reset_swap() {
    std::println("\n--- AC2: present advances reset/swap totals ---");
    reset_render_frame_metrics_for_test();
    reset_render_engine_counters_for_test();

    LinearCellGrid grid(40, 12);
    CHECK(grid.valid(), "grid valid");
    auto fb = grid.view();
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 40; ++x)
            (void)draw_cell(fb, grid.dirty, static_cast<std::uint32_t>(x),
                            static_cast<std::uint32_t>(y), cell_ch('.'));

    const auto reset0 =
        g_render_frame_metrics().render_arena_reset_total.load(std::memory_order_relaxed);
    const auto swap0 =
        g_render_frame_metrics().render_arena_swap_total.load(std::memory_order_relaxed);
    const auto alloc0 = g_render_frame_metrics().render_alloc_bytes.load(std::memory_order_relaxed);

    std::string out;
    const auto n = present_batch_to_string(fb, grid.dirty, out);
    CHECK(n > 0, "present bytes");
    CHECK(grid.dirty.is_clean(), "dirty cleared");

    const auto reset1 =
        g_render_frame_metrics().render_arena_reset_total.load(std::memory_order_relaxed);
    const auto swap1 =
        g_render_frame_metrics().render_arena_swap_total.load(std::memory_order_relaxed);
    const auto alloc1 = g_render_frame_metrics().render_alloc_bytes.load(std::memory_order_relaxed);
    std::println("  reset {}→{} swap {}→{} alloc {}→{}", reset0, reset1, swap0, swap1, alloc0,
                 alloc1);
    CHECK(reset1 > reset0, "arena reset total advanced");
    CHECK(swap1 > swap0, "arena swap total advanced");
    CHECK(alloc1 >= alloc0, "alloc bytes non-decreasing");
}

void ac3_linear_move() {
    std::println("\n--- AC3: LinearCellGrid move-only ---");
    reset_render_frame_metrics_for_test();
    LinearCellGrid a(8, 4);
    CHECK(a.valid(), "a valid");
    const auto creates =
        g_render_frame_metrics().linear_cell_grid_creates.load(std::memory_order_relaxed);
    CHECK(creates >= 1, "create counted");
    LinearCellGrid b = std::move(a);
    CHECK(b.valid(), "b valid after move");
    CHECK(a.moved_from, "a moved-from");
    CHECK(!a.valid(), "a invalid after move");
    const auto moves =
        g_render_frame_metrics().linear_cell_grid_moves.load(std::memory_order_relaxed);
    CHECK(moves >= 1, "move counted");

    // Present after move ownership
    b.dirty.mark_all_dirty(8, 4);
    auto fb = b.view();
    (void)draw_cell(fb, b.dirty, 0, 0, cell_ch('Z'));
    std::string out;
    CHECK(present_batch_to_string(fb, b.dirty, out) > 0, "present moved grid");
}

void ac4_frame_time() {
    std::println("\n--- AC4: frame-time histogram ---");
    reset_render_frame_metrics_for_test();
    LinearCellGrid grid(20, 10);
    auto fb = grid.view();
    for (int i = 0; i < 12; ++i) {
        grid.dirty.mark_all_dirty(20, 10);
        std::string out;
        (void)present_batch_to_string(fb, grid.dirty, out);
    }
    const auto samples =
        g_render_frame_metrics().render_frame_time_samples.load(std::memory_order_relaxed);
    const auto avg = render_frame_time_avg_us();
    const auto p99 = render_frame_time_p99_us();
    std::println("  samples={} avg_us={} p99_us={}", samples, avg, p99);
    CHECK(samples >= 12, "≥12 frame samples");
    CHECK(avg >= 0, "avg non-negative");
    CHECK(p99 >= avg || p99 == 0, "p99 >= avg (or zero)");
}

void ac5_string_heap_stable() {
    std::println("\n--- AC5: present loop string_heap stable ---");
    CompilerService cs;
    const auto id = as_i(cs, "(make-terminal-buffer 16 8)");
    CHECK(id >= 0, "buf");
    // Warm path (EDSL may intern a few keys on first present).
    for (int w = 0; w < 5; ++w) {
        CHECK(as_i(cs, std::format("(if (terminal-set-cell {} 0 0 {} 7 0) 1 0)", id, 65 + w)) == 1,
              "warm set");
        (void)as_i(cs, std::format("(terminal-present-batch {} -1)", id));
    }
    const auto heap0 = static_cast<std::int64_t>(cs.evaluator().string_heap().size());
    const auto alloc0 = g_render_frame_metrics().render_alloc_bytes.load(std::memory_order_relaxed);
    for (int i = 0; i < 40; ++i) {
        CHECK(as_i(cs, std::format("(if (terminal-set-cell {} 0 0 {} 7 0) 1 0)", id,
                                   65 + (i % 8))) == 1,
              "set loop");
        (void)as_i(cs, std::format("(terminal-present-batch {} -1)", id));
    }
    const auto heap1 = static_cast<std::int64_t>(cs.evaluator().string_heap().size());
    const auto alloc1 = g_render_frame_metrics().render_alloc_bytes.load(std::memory_order_relaxed);
    std::println("  string_heap {}→{} alloc {}→{}", heap0, heap1, alloc0, alloc1);
    // After warm-up, pure set-cell + present must not grow Aura string_heap.
    CHECK(heap1 - heap0 <= 2, "string_heap growth ~0 after warm-up");
    CHECK(alloc1 >= alloc0, "render-alloc-bytes non-decreasing");
}

void ac6_query() {
    std::println("\n--- AC6: query:render-memory-stats schema-2049 ---");
    CompilerService cs;
    auto h = cs.eval("(stats:get \"query:render-memory-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema-2049") == 2049, "schema-2049");
    CHECK(href(cs, "issue-2049") == 2049, "issue-2049");
    CHECK(href(cs, "render-frame-arena-wired") == 1, "wired");
    CHECK(href(cs, "render-alloc-bytes") >= 0, "render-alloc-bytes");
    CHECK(href(cs, "render-arena-reset-total") >= 0, "reset total");
    CHECK(href(cs, "frame-time-p99-us") >= 0, "p99");
    CHECK(href(cs, "frame-time-avg-us") >= 0, "avg");
    CHECK(href(cs, "linear-cell-grid-creates") >= 0, "linear creates");
}

void ac7_safepoint_defer() {
    std::println("\n--- AC7: safepoint defers in render hotpath ---");
    CompilerService cs;
    aura::core::arena_policy::enter_render_hotpath();
    const auto inside = cs.evaluator().request_gc_safepoint();
    aura::core::arena_policy::exit_render_hotpath();
    CHECK(inside == 1, "deferred while hotpath");
}

void ac_double_buffer_unit() {
    std::println("\n--- unit: RenderFrameArena end_frame ---");
    RenderFrameArena arena;
    void* p0 = arena.allocate_raw(64, 8);
    CHECK(p0 != nullptr, "alloc");
    CHECK(arena.used_bytes() >= 64, "used");
    const int a0 = arena.active;
    arena.end_frame();
    CHECK(arena.active != a0, "swapped");
    CHECK(arena.used_bytes() == 0, "reset next");
}

} // namespace

int main() {
    std::println("=== test_render_frame_arena_2049 ===");
    ac1_source();
    ac_double_buffer_unit();
    ac2_present_reset_swap();
    ac3_linear_move();
    ac4_frame_time();
    ac5_string_heap_stable();
    ac6_query();
    ac7_safepoint_defer();
    std::println("\n=== results: {} passed, {} failed ===\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
