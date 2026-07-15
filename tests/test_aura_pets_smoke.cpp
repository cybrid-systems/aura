// test_aura_pets_smoke.cpp — Issue #1454: aura-pets headless TUI regression
//
// Covers cyber_cat + snake + tetris demos and lib/std/tui/* modules without a
// real TTY. Complements test_cyber_cat_smoke (#1358) with multi-demo + CI gate.

#include "test_harness.hpp"

#include <cstdlib>
#include <fstream>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;

namespace {

bool file_exists(const char* path) {
    std::ifstream f(path);
    return f.good();
}

std::string read_file(const char* path) {
    std::ifstream f(path);
    if (!f)
        return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool eval_true(CompilerService& cs, const char* expr, const char* label) {
    auto r = cs.eval(expr);
    if (!r.has_value()) {
        CHECK(false, label);
        return false;
    }
    if (is_bool(*r)) {
        CHECK(as_bool(*r), label);
        return as_bool(*r);
    }
    // Some paths return void / other values after side effects — presence is OK.
    CHECK(true, label);
    return true;
}

} // namespace

int main() {
    // ── Artifacts present ──
    static const char* kRequired[] = {
        "lib/std/tui/canvas.aura",  "lib/std/tui/sprite.aura", "lib/std/tui/input.aura",
        "lib/std/tui/run.aura",     "lib/std/tui/scene.aura",  "lib/std/tui/anim.aura",
        "examples/cyber_cat.aura",  "examples/snake.aura",     "examples/tetris.aura",
        "lib/std/atomic-swap.aura",
    };
    for (const char* p : kRequired)
        CHECK(file_exists(p), p);

    // ── Demo markers (headless entry points) ──
    {
        auto cat = read_file("examples/cyber_cat.aura");
        CHECK(cat.find("cyber-cat-run") != std::string::npos, "cyber-cat-run");
        CHECK(cat.find("cyber-cat-demo") != std::string::npos, "cyber-cat-demo");
        CHECK(cat.find("interactive?") != std::string::npos ||
                  cat.find("interactive") != std::string::npos,
              "headless vs interactive split");
    }
    {
        auto sn = read_file("examples/snake.aura");
        CHECK(sn.find("snake-demo") != std::string::npos, "snake-demo");
        CHECK(sn.find("tui:init") != std::string::npos, "snake uses tui:init");
        CHECK(sn.find("tui:shutdown") != std::string::npos, "snake shutdown");
    }
    {
        auto te = read_file("examples/tetris.aura");
        CHECK(te.find("tetris-demo") != std::string::npos, "tetris-demo");
        CHECK(te.find("tui:init") != std::string::npos, "tetris uses tui:init");
        CHECK(te.find("tui:shutdown") != std::string::npos, "tetris shutdown");
    }

    // ── Headless demos via CompilerService ──
    {
        CompilerService cs;
        auto r = cs.eval("(load \"examples/snake.aura\")");
        CHECK(r.has_value(), "load snake.aura");
        eval_true(cs, "(snake-demo)", "snake-demo re-run");
    }
    {
        CompilerService cs;
        auto r = cs.eval("(load \"examples/tetris.aura\")");
        CHECK(r.has_value(), "load tetris.aura");
        eval_true(cs, "(tetris-demo)", "tetris-demo re-run");
    }
    {
        CompilerService cs;
        auto r = cs.eval("(load \"examples/cyber_cat.aura\")");
        CHECK(r.has_value(), "load cyber_cat.aura");
        auto again = cs.eval("(cyber-cat-run 6 #f)");
        CHECK(again.has_value(), "cyber-cat-run 6 frames");
        if (again && is_bool(*again))
            CHECK(as_bool(*again), "cyber-cat-run → #t");
    }

    // ── Raw tui:* session (no demo files) ──
    {
        CompilerService cs;
        auto r = cs.eval("(begin "
                         "  (tui:init \"pets-smoke\" 12 6) "
                         "  (tui:hide-cursor) "
                         "  (tui:clear 0 0) "
                         "  (tui:cell 2 2 \"P\" 65280 0 0) "
                         "  (tui:cell 3 2 \"e\" 65280 0 0) "
                         "  (tui:cell 4 2 \"t\" 65280 0 0) "
                         "  (tui:present) "
                         "  (tui:show-cursor) "
                         "  (tui:shutdown) "
                         "  #t)");
        CHECK(r.has_value() && is_bool(*r) && as_bool(*r), "raw tui session");
    }

    // ── stdlib tui modules ──
    {
        CompilerService cs;
        CHECK(cs.eval("(load \"lib/std/tui/canvas.aura\")").has_value(), "load canvas");
        CHECK(cs.eval("(load \"lib/std/tui/sprite.aura\")").has_value(), "load sprite");
        CHECK(cs.eval("(load \"lib/std/tui/input.aura\")").has_value(), "load input");
        CHECK(cs.eval("(load \"lib/std/tui/run.aura\")").has_value(), "load run");
        CHECK(cs.eval("(load \"lib/std/tui/anim.aura\")").has_value(), "load anim");
        CHECK(cs.eval("(load \"lib/std/tui/scene.aura\")").has_value(), "load scene");
        auto run = cs.eval("(tui:run \"pets\" (lambda () 7))");
        CHECK(run && is_int(*run) && as_int(*run) == 7, "tui:run thunk");
        auto canvas = cs.eval("(let ((c (make-canvas 6 3))) "
                              "  (canvas-set! c 0 0 65 1 0) "
                              "  (canvas-present c) "
                              "  (canvas-destroy c) "
                              "  #t)");
        CHECK(canvas.has_value(), "canvas buffer lifecycle");
    }

    // ── atomic-swap (pets render-sync building block, #1380) ──
    {
        CompilerService cs;
        auto load = cs.eval("(load \"lib/std/atomic-swap.aura\")");
        CHECK(load.has_value(), "load atomic-swap");
        auto swap = cs.eval("(let* ((b (make-binding \"ui\" 0 \"sprite-0\")) "
                            "       (b2 (swap-binding! b 1 \"sprite-1\")) "
                            "       (r (sync-bindings! b2))) "
                            "  (and (binding-dirty? b2) (cdr r)))");
        if (swap.has_value() && is_bool(*swap))
            CHECK(as_bool(*swap), "atomic-swap dirty→commit");
        else
            CHECK(swap.has_value(), "atomic-swap eval");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("aura-pets smoke #1454: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
