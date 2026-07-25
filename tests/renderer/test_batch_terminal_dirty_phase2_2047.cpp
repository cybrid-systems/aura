// @category: unit
// @reason: Issue #2047 — Phase 2 full dirty-region differential update +
// present_batch short-circuit in batch_terminal.
//
//   AC1: kBatchTerminalPhase == 2; source cites #2047; DirtyRegion + dirty build
//   AC2: clean present short-circuits (0 bytes; skips counter++)
//   AC3: sparse mutation (~5% cells) emits <20% of full-frame ANSI bytes
//   AC4: packed build_terminal_frame_ansi_dirty partial vs full
//   AC5: query:render-dirty-delta-stats + query:render-stats schema-2047
//   AC6: terminal-set-cell + present-batch EDSL path dirty peel
//   AC7: full-frame dirty present still works (no regression)

#include "test_harness.hpp"

#include "renderer/batch_terminal.hh"
#include "renderer/render_pass.hh"
#include "renderer/render_primitives.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::renderer::build_terminal_frame_ansi;
using aura::renderer::build_terminal_frame_ansi_dirty;
using aura::renderer::DirtyRegion;
using aura::renderer::draw_cell;
using aura::renderer::estimate_ansi_frame_bytes;
using aura::renderer::FramebufferOwned;
using aura::renderer::g_batch_terminal_stats;
using aura::renderer::g_dirty_delta_metrics;
using aura::renderer::kBatchTerminalPhase;
using aura::renderer::present_batch_to_string;
using aura::renderer::reset_batch_terminal_stats_for_test;
using aura::renderer::reset_dirty_delta_metrics_for_test;
using aura::renderer::reset_render_engine_counters_for_test;
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

std::int64_t href_dd(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (stats:get \"query:render-dirty-delta-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::int64_t href_rs(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (stats:get \"query:render-stats\") \"{}\")", key));
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

TermCell cell_ch(char ch, std::uint8_t fg = 7) {
    TermCell c;
    c.ch = static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
    c.fg_r = fg;
    c.mode = 0;
    return c;
}

void ac1_source_phase() {
    std::println("\n--- AC1: Phase 2 + source #2047 ---");
    CHECK(kBatchTerminalPhase == 2, "kBatchTerminalPhase == 2");
    auto ixx = read_file("src/renderer/batch_terminal.ixx");
    auto hh = read_file("src/renderer/batch_terminal.hh");
    auto prim = read_file("src/renderer/render_primitives.cpp");
    auto io = read_file("src/compiler/evaluator_primitives_io.cpp");
    CHECK(!ixx.empty() && ixx.find("#2047") != std::string::npos, "batch_terminal.ixx #2047");
    CHECK(ixx.find("kBatchTerminalPhase = 2") != std::string::npos, "ixx phase 2");
    CHECK(ixx.find("build_terminal_frame_ansi_dirty") != std::string::npos, "ixx dirty build");
    CHECK(ixx.find("batch_terminal_should_short_circuit") != std::string::npos ||
              ixx.find("note_batch_terminal_short_circuit") != std::string::npos,
          "short-circuit helper");
    CHECK(!hh.empty() && hh.find("#2047") != std::string::npos, "batch_terminal.hh #2047");
    CHECK(hh.find("kBatchTerminalPhase = 2") != std::string::npos, "hh phase 2");
    CHECK(!prim.empty() && prim.find("#2047") != std::string::npos, "present path #2047");
    CHECK(prim.find("note_batch_terminal_short_circuit") != std::string::npos, "present short");
    CHECK(!io.empty() && io.find("schema-2047") != std::string::npos, "schema-2047");
    CHECK(io.find("batch-terminal-phase2-wired") != std::string::npos, "phase2 wired key");
    // SlimSurface: no new query:*-stats name — fold into dirty-delta / render-stats.
    CHECK(io.find("query:batch-terminal-stats") == std::string::npos,
          "no new batch-terminal-stats query");
}

void ac2_clean_short_circuit() {
    std::println("\n--- AC2: clean present short-circuit ---");
    reset_batch_terminal_stats_for_test();
    reset_dirty_delta_metrics_for_test();
    reset_render_engine_counters_for_test();

    FramebufferOwned owned;
    owned.resize(40, 20);
    owned.dirty.clear(); // clean
    std::string out;
    const auto n = present_batch_to_string(owned.view(), owned.dirty, out);
    CHECK(n == 0, "clean present returns 0");
    CHECK(out.empty(), "no ANSI on clean");
    CHECK(g_batch_terminal_stats().dirty_short_circuit >= 1, "batch short-circuit++");
    CHECK(g_dirty_delta_metrics().dirty_region_skips_total.load() >= 1, "delta skips++");
}

void ac3_sparse_bytes() {
    std::println("\n--- AC3: sparse mutation <20% full-frame ANSI bytes ---");
    reset_batch_terminal_stats_for_test();
    reset_dirty_delta_metrics_for_test();

    constexpr int W = 80;
    constexpr int H = 24;
    FramebufferOwned owned;
    owned.resize(W, H);
    auto fb = owned.view();
    // Fill then clear dirty so baseline is clean.
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            (void)draw_cell(fb, owned.dirty, static_cast<std::uint32_t>(x),
                            static_cast<std::uint32_t>(y), cell_ch('.'));
    owned.dirty.clear();

    // Full-frame dirty present → byte baseline.
    owned.dirty.mark_all_dirty(static_cast<std::uint32_t>(W), static_cast<std::uint32_t>(H));
    std::string full_out;
    const auto full_n = present_batch_to_string(fb, owned.dirty, full_out);
    CHECK(full_n > 0, "full present >0");
    CHECK(full_out.size() == static_cast<std::size_t>(full_n), "full size match");

    // Sparse: ~5% cells (96 of 1920)
    owned.dirty.clear();
    const int sparse_n = (W * H * 5) / 100;
    int painted = 0;
    for (int y = 0; y < H && painted < sparse_n; ++y) {
        for (int x = 0; x < W && painted < sparse_n; ++x) {
            if ((x + y) % 3 != 0)
                continue;
            (void)draw_cell(fb, owned.dirty, static_cast<std::uint32_t>(x),
                            static_cast<std::uint32_t>(y), cell_ch('X', 1));
            ++painted;
        }
    }
    CHECK(painted > 0, "painted sparse cells");
    CHECK(owned.dirty.is_dirty(), "dirty after sparse");
    // AABB may be larger than painted set — still should be << full for clustered sparse.
    // Force a tight AABB by marking a small rect only.
    owned.dirty.clear();
    for (int i = 0; i < sparse_n; ++i) {
        const int x = i % 16;
        const int y = i / 16;
        if (y >= H)
            break;
        (void)draw_cell(fb, owned.dirty, static_cast<std::uint32_t>(x),
                        static_cast<std::uint32_t>(y), cell_ch('S', 2));
    }
    std::string sparse_out;
    const auto sparse_bytes = present_batch_to_string(fb, owned.dirty, sparse_out);
    CHECK(sparse_bytes > 0, "sparse present >0");
    const double ratio =
        static_cast<double>(sparse_bytes) / static_cast<double>(full_n > 0 ? full_n : 1);
    std::println("  full={} sparse={} ratio={:.3f} painted~{}", full_n, sparse_bytes, ratio,
                 sparse_n);
    CHECK(ratio < 0.20, "sparse <20% of full-frame ANSI bytes");
    CHECK(g_batch_terminal_stats().ansi_bytes_saved > 0 ||
              g_dirty_delta_metrics().ansi_bytes_saved_total.load() > 0,
          "ansi_bytes_saved recorded");
}

void ac4_packed_dirty_build() {
    std::println("\n--- AC4: packed dirty build partial ---");
    // Module-style packed API is in batch_terminal.hh legacy overload only for full;
    // TermCell dirty path is the production path — re-validate partial AABB.
    constexpr int W = 10;
    constexpr int H = 8;
    std::vector<TermCell> cells(static_cast<std::size_t>(W * H), TermCell::space_palette());
    for (auto& c : cells)
        c.ch = '.';
    cells[static_cast<std::size_t>(2 * W + 3)] = cell_ch('A', 1);
    cells[static_cast<std::size_t>(2 * W + 4)] = cell_ch('B', 2);

    std::string full;
    (void)build_terminal_frame_ansi(full, W, H, cells.data());
    std::string dirty;
    DirtyRegion d;
    d.mark_dirty(3, 2);
    d.mark_dirty(4, 2);
    auto er = build_terminal_frame_ansi_dirty(dirty, W, H, cells.data(), d);
    CHECK(er.partial, "partial emit");
    CHECK(er.cells_emitted == 2, "2 cells emitted");
    CHECK(dirty.size() < full.size(), "dirty bytes < full");
    CHECK(dirty.find('A') != std::string::npos && dirty.find('B') != std::string::npos,
          "dirty contains A/B");
}

void ac5_query_schema() {
    std::println("\n--- AC5: query schema-2047 (folded into existing stats) ---");
    CompilerService cs;
    auto h = cs.eval("(stats:get \"query:render-dirty-delta-stats\")");
    CHECK(h && is_hash(*h), "dirty-delta-stats hash");
    CHECK(href_dd(cs, "schema-2047") == 2047, "schema-2047");
    CHECK(href_dd(cs, "batch-terminal-phase") == 2, "phase 2");
    CHECK(href_dd(cs, "batch-terminal-phase2-wired") == 1, "wired");
    CHECK(href_dd(cs, "ansi-bytes-saved") >= 0, "ansi-bytes-saved key");
    CHECK(href_dd(cs, "batch-dirty-short-circuit") >= 0, "short-circuit key");

    auto hr = cs.eval("(stats:get \"query:render-stats\")");
    CHECK(hr && is_hash(*hr), "render-stats hash");
    CHECK(href_rs(cs, "schema-2047") == 2047, "render-stats schema-2047");
    CHECK(href_rs(cs, "batch-terminal-phase") == 2, "render-stats phase");
}

void ac6_edsl_set_cell_present() {
    std::println("\n--- AC6: terminal-set-cell + present-batch ---");
    CompilerService cs;
    const auto id = as_i(cs, "(make-terminal-buffer 32 16)");
    CHECK(id >= 0, "buf id");
    // Initial create may leave dirty; present once to clear.
    (void)as_i(cs, std::format("(terminal-present-batch {} -1)", id));
    // Sparse writes
    for (int i = 0; i < 8; ++i) {
        CHECK(as_i(cs, std::format("(if (terminal-set-cell {} {} 0 {} 7 0) 1 0)", id, i, 65 + i)) ==
                  1,
              "set-cell");
    }
    const auto n = as_i(cs, std::format("(terminal-present-batch {} -1)", id));
    CHECK(n > 0, "present dirty >0");
    // Clean present
    const auto n2 = as_i(cs, std::format("(terminal-present-batch {} -1)", id));
    CHECK(n2 == 0, "second present short-circuits");
}

void ac7_full_frame_ok() {
    std::println("\n--- AC7: full-frame present no regression ---");
    FramebufferOwned owned;
    owned.resize(16, 8);
    auto fb = owned.view();
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 16; ++x)
            (void)draw_cell(fb, owned.dirty, static_cast<std::uint32_t>(x),
                            static_cast<std::uint32_t>(y), cell_ch('#'));
    std::string out;
    const auto n = present_batch_to_string(fb, owned.dirty, out);
    CHECK(n > 0, "full present bytes");
    CHECK(out.find("\033[?2026h") != std::string::npos, "sync begin");
    CHECK(out.find('#') != std::string::npos, "has content");
    CHECK(estimate_ansi_frame_bytes(16, 8) > 0, "estimate >0");
}

} // namespace

int main() {
    std::println("=== test_batch_terminal_dirty_phase2_2047 ===");
    ac1_source_phase();
    ac2_clean_short_circuit();
    ac3_sparse_bytes();
    ac4_packed_dirty_build();
    ac5_query_schema();
    ac6_edsl_set_cell_present();
    ac7_full_frame_ok();
    std::println("\n=== results: {} passed, {} failed ===\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
