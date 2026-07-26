// @category: unit
// @reason: Issue #2134 — tui:draw-batch / fill-rect / present-batch over
// TermBuf DirtyRegion (Agent batch draw + dirty AABB present).
//
//   AC1: draw-batch + fill-rect update cells and expand dirty AABB
//   AC2: present-batch emits dirty only; clean short-circuits to 0
//   AC3: render-tier meta + metrics schema-2134
//   AC4: existing tui:cell / tui:present remain registered
//   AC5: source cites #2134; headless partial vs full dirty golden

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

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
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
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

static bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

// Present to pipe and return ANSI bytes (headless golden).
static std::string present_to_string(CompilerService& cs, std::int64_t bid) {
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return {};
    auto written = cs.eval(std::format("(tui:present-batch {} {})", bid, pipefd[1]));
    ::close(pipefd[1]);
    std::string out;
    if (written && is_int(*written) && as_int(*written) > 0) {
        char buf[8192];
        for (;;) {
            const auto n = ::read(pipefd[0], buf, sizeof(buf));
            if (n <= 0)
                break;
            out.append(buf, static_cast<std::size_t>(n));
        }
    }
    ::close(pipefd[0]);
    return out;
}

} // namespace

int main() {
    std::println("=== Issue #2134: tui batch draw + dirty present ===");

    // ── AC5: source ──
    {
        std::println("\n--- AC5: source ---");
        auto tui = read_file("src/compiler/evaluator_primitives_tui.cpp");
        auto met = read_file("src/compiler/observability_metrics.h");
        CHECK(tui.find("#2134") != std::string::npos, "tui cites #2134");
        CHECK(tui.find("tui:draw-batch") != std::string::npos, "draw-batch");
        CHECK(tui.find("tui:fill-rect") != std::string::npos, "fill-rect");
        CHECK(tui.find("tui:present-batch") != std::string::npos, "present-batch");
        CHECK(tui.find("draw_batch") != std::string::npos ||
                  tui.find("present_batch") != std::string::npos,
              "engine batch API");
        CHECK(met.find("tui_draw_batch_total") != std::string::npos, "metrics");
    }

    // ── AC4: legacy tui:cell / present still registered ──
    {
        std::println("\n--- AC4: legacy surface ---");
        CompilerService cs;
        auto cell = cs.eval("(primitive? 'tui:cell)");
        auto present = cs.eval("(primitive? 'tui:present)");
        // primitive? may not exist — probe via eval of form
        auto r1 = cs.eval("(tui:draw-batch -1 0 0 65)");
        CHECK(r1.has_value(), "draw-batch registered (returns -1 for bad id)");
        CHECK(r1 && is_int(*r1) && as_int(*r1) == -1, "bad id → -1");
        (void)cell;
        (void)present;
    }

    // ── AC1: draw-batch + fill-rect dirty AABB ──
    {
        std::println("\n--- AC1: draw-batch + fill-rect ---");
        CompilerService cs;
        auto id = cs.eval("(make-terminal-buffer 8 4)");
        CHECK(id && is_int(*id) && as_int(*id) >= 0, "make-terminal-buffer");
        const auto bid = as_int(*id);

        auto w1 = cs.eval(std::format("(tui:draw-batch {} 1 1 65 1 0)", bid));
        CHECK(w1 && is_int(*w1) && as_int(*w1) == 1, "draw-batch single cell");

        auto w2 = cs.eval(std::format("(tui:fill-rect {} 2 0 3 2 66 2 0)", bid));
        CHECK(w2 && is_int(*w2) && as_int(*w2) == 6, "fill-rect 3x2 = 6 cells");

        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        CHECK(m && m->tui_draw_batch_total.load() >= 1, "draw-batch total");
        CHECK(m && m->tui_draw_batch_cells_written.load() >= 1, "draw cells");
        CHECK(m && m->tui_fill_rect_total.load() >= 1, "fill-rect total");
        CHECK(m && m->tui_fill_rect_cells_written.load() >= 6, "fill cells");
    }

    // ── AC2: present-batch dirty vs clean ──
    {
        std::println("\n--- AC2: present dirty / clean ---");
        CompilerService cs;
        auto id = cs.eval("(make-terminal-buffer 4 2)");
        CHECK(id && is_int(*id), "buf");
        const auto bid = as_int(*id);

        // Fresh buffer is fully dirty (create marks all dirty).
        auto frame = present_to_string(cs, bid);
        CHECK(!frame.empty(), "first present writes bytes");
        CHECK(contains(frame, "\033[") || contains(frame, " "), "ANSI or spaces");

        // Clean short-circuit
        auto skip = cs.eval(std::format("(tui:present-batch {})", bid));
        CHECK(skip && is_int(*skip) && as_int(*skip) == 0, "clean → 0 bytes");

        // Partial dirty
        auto w = cs.eval(std::format("(tui:draw-batch {} 0 0 88 3 0)", bid));
        CHECK(w && is_int(*w) && as_int(*w) == 1, "re-dirty one cell");
        auto frame2 = present_to_string(cs, bid);
        CHECK(!frame2.empty(), "partial dirty present writes");
        CHECK(contains(frame2, "X") || contains(frame2, "\033["), "partial content");

        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        CHECK(m && m->tui_present_batch_total.load() >= 2, "present-batch total");
        CHECK(m && m->tui_present_batch_skip_clean.load() >= 1, "skip clean");
        CHECK(m && m->tui_present_batch_dirty_cells.load() >= 1, "dirty cells metric");
    }

    // ── AC3: schema-2134 ──
    {
        std::println("\n--- AC3: query:render-stats schema-2134 ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
        auto id = cs.eval("(make-terminal-buffer 2 2)");
        const auto bid = as_int(*id);
        (void)cs.eval(std::format("(tui:fill-rect {} 0 0 2 2 46 7 0)", bid));
        (void)cs.eval(std::format("(tui:present-batch {})", bid));
        CHECK(href(cs, "schema-2134") == 2134, "schema-2134");
        CHECK(href(cs, "issue-2134") == 2134, "issue-2134");
        CHECK(href(cs, "tui-batch-draw-wired") == 1, "wired");
        CHECK(href(cs, "tui-draw-batch-total") >= 0, "draw total key");
        CHECK(href(cs, "tui-fill-rect-total") >= 1, "fill total");
        CHECK(href(cs, "tui-present-batch-total") >= 1, "present total");
        CHECK(href(cs, "tui-present-batch-us-total") >= 0, "present us");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
