// test_production_sweep_1331_1343.cpp — Issues #1331–#1343 Phase 1 TUI stack

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::compiler::types::is_void;

namespace {

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref ({}) \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

bool file_exists(const char* path) {
    std::ifstream f(path);
    return f.good();
}

} // namespace

int main() {
    CompilerService cs;
    constexpr auto Q = "query:production-sweep-1331-1343-stats";

    {
        auto r = cs.eval(aura::test::aura_call_expr(Q));
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, Q, "schema") == 1331, "schema");
        CHECK(href(cs, Q, "active") == 1, "active");
        CHECK(href(cs, Q, "tui-architecture-plan") == 1, "META plan (#1331)");
        CHECK(href(cs, Q, "tui-layers-total") == 5, "5 layers");
        CHECK(href(cs, Q, "tui-runtime-active") == 1, "runtime (#1332)");
        CHECK(href(cs, Q, "tui-primitives-active") == 1, "primitives (#1333)");
        CHECK(href(cs, Q, "tui-stdlib-active") == 1, "stdlib (#1334-5)");
        CHECK(href(cs, Q, "tui-sync-output-active") == 1, "sync output (#1342)");
        CHECK(href(cs, Q, "tui-mouse-scaffold-active") == 1, "mouse (#1343)");
        // examples/ removed per Anqi 2026-07-19 directive (aura philosophy,
        // no demos). The TUI primitives (tui:init/cell/present/etc.) and
        // mouse/sync/output scaffolding are still verified below; only the
        // demo-specific meta checks (cyber cat + games demos) are dropped.
        CHECK(href(cs, Q, "issue-1343") == 1343, "issue-1343");
    }

    // #1332/#1333: init → cell → present → frame-ansi
    {
        auto ok = cs.eval("(tui:init \"test\" 16 8)");
        CHECK(ok && is_bool(*ok), "tui:init returns bool");

        auto sz = cs.eval("(tui:size)");
        CHECK(sz && is_pair(*sz), "tui:size is pair");

        auto cell = cs.eval("(tui:cell 2 3 \"A\" 16711680 0 0)");
        CHECK(cell && is_bool(*cell), "tui:cell");

        auto g = cs.eval("(tui:get-cell 2 3)");
        CHECK(g && is_pair(*g), "tui:get-cell");

        auto p = cs.eval("(tui:present)");
        CHECK(p, "tui:present");

        auto ansi = cs.eval("(tui:frame-ansi)");
        CHECK(ansi && is_string(*ansi), "tui:frame-ansi string");

        auto inits = href(cs, Q, "tui-init-total");
        CHECK(inits >= 1, "init counted");
        auto presents = href(cs, Q, "tui-present-total");
        CHECK(presents >= 1, "present counted");
        auto writes = href(cs, Q, "tui-cell-writes");
        CHECK(writes >= 1, "cell writes counted");
        auto sync = href(cs, Q, "tui-sync-output-frames");
        CHECK(sync >= 1, "CSI 2026 sync frames (#1342)");

        auto sh = cs.eval("(tui:shutdown)");
        CHECK(sh, "tui:shutdown");
    }

    // #1342 half-block pixel
    {
        (void)cs.eval("(tui:init \"px\" 8 4)");
        auto px = cs.eval("(tui:pixel 1 1 16711680 255)");
        CHECK(px && is_bool(*px), "tui:pixel");
        auto hb = href(cs, Q, "tui-half-block-pixels");
        CHECK(hb >= 1, "half-block counted");
        (void)cs.eval("(tui:shutdown)");
    }

    // #1343 mouse + inject-key event
    {
        (void)cs.eval("(tui:init \"ev\" 8 4)");
        auto m = cs.eval("(tui:mouse 1)");
        CHECK(m && is_bool(*m), "tui:mouse");
        auto me = href(cs, Q, "tui-mouse-enable-total");
        CHECK(me >= 1, "mouse enable counted");
        (void)cs.eval("(tui:inject-key \"q\")");
        auto e = cs.eval("(tui:read-event 0)");
        CHECK(e && is_pair(*e), "read-event after inject");
        (void)cs.eval("(tui:shutdown)");
    }

    // Demo / stdlib files exist
    {
        CHECK(file_exists("lib/std/tui/canvas.aura"), "canvas.aura");
        CHECK(file_exists("lib/std/tui/sprite.aura"), "sprite.aura");
        CHECK(file_exists("lib/std/tui/input.aura"), "input.aura");
        CHECK(file_exists("lib/std/tui/run.aura"), "run.aura");
        CHECK(file_exists("lib/std/tui/scene.aura"), "scene.aura");
        CHECK(file_exists("lib/std/tui/anim.aura"), "anim.aura");
        CHECK(file_exists("examples/cyber_cat.aura"), "cyber_cat.aura");
        CHECK(file_exists("examples/snake.aura"), "snake.aura");
        CHECK(file_exists("examples/tetris.aura"), "tetris.aura");
        CHECK(file_exists("src/tui/tui_runtime.hh"), "tui_runtime.hh");
    }

    {
        CompilerService cs2;
        auto a = cs2.eval("(* 6 7)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(* 6 7)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1331–#1343: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
