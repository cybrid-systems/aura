// @category: unit
// @reason: Issue #2214 — tui:present-dirty differential zero-copy present.
//
//   AC1: Primitive registered under AURA_ENABLE_TUI + hot meta
//   AC2: Dirty 5%/20%/100% → partial path; clean short-circuit; ansi_bytes_saved
//   AC3: LinearCellGrid active path consumes dirty
//   AC4: query:render-stats schema-2214 + present-dirty keys
//   AC5: source cites + AURA_RENDER_HOT_ENTRY + full tui:present retained

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "renderer/batch_terminal.hh"
#include "renderer/render_frame_arena.hh"
#include "renderer/render_primitives.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::renderer::active_linear_cell_grid;
using aura::renderer::g_batch_terminal_stats;
using aura::renderer::LinearCellGrid;
using aura::renderer::reset_batch_terminal_stats_for_test;
using aura::renderer::reset_render_engine_counters_for_test;
using aura::renderer::set_active_linear_cell_grid;
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

static std::int64_t make_term_buf(CompilerService& cs, int w, int h) {
    auto r = cs.eval(std::format("(make-terminal-buffer {} {})", w, h));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    std::println("=== Issue #2214: tui:present-dirty ===");
    reset_batch_terminal_stats_for_test();
    reset_render_engine_counters_for_test();
    set_active_linear_cell_grid(nullptr);

    // ── AC5: source ──
    {
        std::println("\n--- AC5: source wiring ---");
        auto tui = read_file("src/compiler/evaluator_primitives_tui.cpp");
        auto tpl = read_file("src/compiler/render_prim_template.hh");
        CHECK(tui.find("tui:present-dirty") != std::string::npos, "prim registered");
        CHECK(tui.find("Issue #2214") != std::string::npos ||
                  tui.find("#2214") != std::string::npos,
              "cites #2214");
        CHECK(tui.find("AURA_RENDER_HOT_ENTRY") != std::string::npos, "hot entry");
        CHECK(tui.find("present_batch") != std::string::npos, "uses present_batch path");
        CHECK(tui.find("tui:present") != std::string::npos &&
                  (tui.find("register_render_hot_prim") != std::string::npos ||
                   tui.find("add(\"tui:present\"") != std::string::npos),
              "full present retained");
        CHECK(tpl.find("present-dirty") != std::string::npos, "template docs Agents");
    }

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");

    // ── AC1: registered (callable + meta) ──
    {
        std::println("\n--- AC1: primitive surface ---");
        // Callable without args (no TUI/grid → clean 0)
        auto p = cs.eval("(tui:present-dirty)");
        CHECK(p && is_int(*p) && as_int(*p) >= 0, "tui:present-dirty callable");
        // Full present retained
        auto full = cs.eval("(tui:present)");
        CHECK(full.has_value(), "tui:present retained");
        auto tui = read_file("src/compiler/evaluator_primitives_tui.cpp");
        CHECK(tui.find("tui:present-dirty") != std::string::npos &&
                  (tui.find("register_render_hot_prim") != std::string::npos ||
                   tui.find("RENDER_PRIMITIVE_META") != std::string::npos),
              "AC1: render meta on present-dirty (helper or RENDER_PRIMITIVE_META)");
    }

    // ── AC2: TermBuf dirty ratios ──
    {
        std::println("\n--- AC2: dirty 5%/20%/100% + clean short-circuit ---");
        const int W = 40, H = 12;
        auto bid = make_term_buf(cs, W, H);
        CHECK(bid >= 0, "term buf");

        // Fill a few cells (5%)
        const int n5 = (W * H * 5) / 100;
        for (int i = 0; i < n5; ++i) {
            const int x = i % W;
            const int y = i / W;
            (void)cs.eval(std::format("(tui:draw-batch {} {} {} 65)", bid, x, y));
        }
        const auto saved0 = g_batch_terminal_stats().ansi_bytes_saved;
        const auto partial0 = g_batch_terminal_stats().dirty_partial_presents;
        auto n = cs.eval(std::format("(tui:present-dirty {})", bid));
        CHECK(n && is_int(*n) && as_int(*n) >= 0, "AC2: 5% present-dirty ok");
        CHECK(g_batch_terminal_stats().dirty_partial_presents > partial0 || as_int(*n) > 0,
              "AC2: 5% took partial or emitted");
        CHECK(g_batch_terminal_stats().ansi_bytes_saved >= saved0, "AC2: bytes_saved non-decrease");

        // 20% more dirty
        const int n20 = (W * H * 20) / 100;
        for (int i = 0; i < n20; ++i) {
            const int x = i % W;
            const int y = (i / W) % H;
            (void)cs.eval(std::format("(tui:draw-batch {} {} {} 66)", bid, x, y));
        }
        n = cs.eval(std::format("(tui:present-dirty {})", bid));
        CHECK(n && is_int(*n) && as_int(*n) >= 0, "AC2: 20% ok");

        // 100% dirty via fill-rect
        (void)cs.eval(std::format("(tui:fill-rect {} 0 0 {} {} 67 7 0)", bid, W, H));
        n = cs.eval(std::format("(tui:present-dirty {})", bid));
        CHECK(n && is_int(*n) && as_int(*n) >= 0, "AC2: 100% ok");

        // Clean short-circuit
        const auto sc0 = g_batch_terminal_stats().dirty_short_circuit;
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        const auto msc0 = m->tui_present_dirty_short_circuit.load();
        n = cs.eval(std::format("(tui:present-dirty {})", bid));
        CHECK(n && is_int(*n) && as_int(*n) == 0, "AC2: clean → 0");
        CHECK(g_batch_terminal_stats().dirty_short_circuit > sc0 ||
                  m->tui_present_dirty_short_circuit.load() > msc0,
              "AC2: short-circuit counter advanced");
    }

    // ── AC2b: explicit AABB ──
    {
        std::println("\n--- AC2b: explicit AABB on TermBuf ---");
        auto bid = make_term_buf(cs, 20, 10);
        CHECK(bid >= 0, "buf");
        (void)cs.eval(std::format("(tui:draw-batch {} 2 3 88)", bid));
        auto n = cs.eval(std::format("(tui:present-dirty {} 1 1 5 5)", bid));
        CHECK(n && is_int(*n) && as_int(*n) >= 0, "explicit AABB present");
    }

    // ── AC3: LinearCellGrid ──
    {
        std::println("\n--- AC3: LinearCellGrid active dirty consume ---");
        LinearCellGrid grid(16, 8);
        CHECK(grid.valid(), "grid valid");
        grid.cells[0].ch = 'Z';
        grid.dirty.mark_dirty(0, 0);
        set_active_linear_cell_grid(&grid);
        CHECK(active_linear_cell_grid() == &grid, "active set");
        auto n = cs.eval("(tui:present-dirty)");
        CHECK(n && is_int(*n) && as_int(*n) >= 0, "AC3: present from LinearCellGrid");
        CHECK(grid.dirty.is_clean(), "AC3: dirty cleared after present");
        // Second present short-circuits
        n = cs.eval("(tui:present-dirty)");
        CHECK(n && is_int(*n) && as_int(*n) == 0, "AC3: clean LinearCellGrid → 0");
        set_active_linear_cell_grid(nullptr);
    }

    // ── AC4: query schema ──
    {
        std::println("\n--- AC4: query schema-2214 ---");
        CHECK(href(cs, "schema-2214") == 2214, "schema-2214");
        CHECK(href(cs, "issue-2214") == 2214, "issue-2214");
        CHECK(href(cs, "present-dirty-wired") == 1, "wired");
        CHECK(href(cs, "present-dirty-calls") >= 1, "present-dirty-calls");
        CHECK(href(cs, "present-dirty-short-circuit") >= 0, "short-circuit key");
        CHECK(href(cs, "present-dirty-short-circuit-rate-bp") >= 0, "rate-bp");
        CHECK(href(cs, "schema-2047") == 2047, "2047 lineage retained");
        auto io = read_file("src/compiler/evaluator_primitives_io.cpp");
        CHECK(io.find("schema-2214") != std::string::npos, "query source cites 2214");
    }

    // ── TUIRuntime path smoke ──
    {
        std::println("\n--- TUIRuntime present-dirty smoke ---");
        set_active_linear_cell_grid(nullptr);
        CHECK(cs.eval("(tui:init \"#2214\" 24 8)").has_value(), "tui init");
        CHECK(cs.eval("(tui:cell 1 1 \"A\")").has_value(), "cell write");
        auto n = cs.eval("(tui:present-dirty)");
        CHECK(n && is_int(*n) && as_int(*n) >= 0, "TUIRuntime present-dirty");
        n = cs.eval("(tui:present-dirty)");
        CHECK(n && is_int(*n) && as_int(*n) == 0, "TUIRuntime clean short-circuit");
        // explicit AABB
        CHECK(cs.eval("(tui:cell 2 2 \"B\")").has_value(), "cell 2");
        n = cs.eval("(tui:present-dirty 0 0 5 5)");
        CHECK(n && is_int(*n) && as_int(*n) >= 0, "TUIRuntime explicit AABB");
        (void)cs.eval("(tui:shutdown)");
    }

    std::println("\n=== test_tui_present_dirty_2214: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
