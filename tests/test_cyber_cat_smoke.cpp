// test_cyber_cat_smoke.cpp — Issue #1358: end-to-end cyber_cat headless smoke

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

} // namespace

int main() {
    // AC: stdlib + demo files present
    CHECK(file_exists("lib/std/tui/canvas.aura"), "canvas.aura");
    CHECK(file_exists("lib/std/tui/sprite.aura"), "sprite.aura");
    CHECK(file_exists("lib/std/tui/input.aura"), "input.aura");
    CHECK(file_exists("lib/std/tui/run.aura"), "run.aura");
    CHECK(file_exists("lib/std/tui/scene.aura"), "scene.aura");
    CHECK(file_exists("lib/std/tui/anim.aura"), "anim.aura");
    CHECK(file_exists("examples/cyber_cat.aura"), "cyber_cat.aura");

    // File content markers for #1358 demo quality
    {
        auto cat = read_file("examples/cyber_cat.aura");
        CHECK(cat.find("cyber-cat-demo") != std::string::npos, "cyber-cat-demo entry");
        CHECK(cat.find("matrix") != std::string::npos || cat.find("rain") != std::string::npos,
              "matrix rain present");
        CHECK(cat.find("cyber-cat-interactive") != std::string::npos, "interactive entry");
        CHECK(cat.find("tui:raw-mode") != std::string::npos, "raw mode for interactive");
        CHECK(cat.find("tui:read-event") != std::string::npos, "input poll");
    }
    {
        auto canvas = read_file("lib/std/tui/canvas.aura");
        CHECK(canvas.find("make-canvas") != std::string::npos, "make-canvas");
        CHECK(canvas.find("make-terminal-buffer") != std::string::npos, "buffer-backed canvas");
        CHECK(canvas.find("draw-text") != std::string::npos, "draw-text");
    }
    {
        auto sp = read_file("lib/std/tui/sprite.aura");
        CHECK(sp.find("cyber-cat-frames") != std::string::npos, "cyber-cat-frames");
        CHECK(sp.find("draw-sprite") != std::string::npos, "draw-sprite");
    }

    // Headless eval of cyber_cat (auto-runs cyber-cat-demo on load)
    {
        CompilerService cs;
        // Prefer load of demo body without double auto-run: call cyber-cat-run directly
        // after defining helpers by loading file (runs demo once).
        auto r = cs.eval("(load \"examples/cyber_cat.aura\")");
        CHECK(r.has_value(), "load cyber_cat.aura");
        // Re-run a short headless pass
        auto again = cs.eval("(cyber-cat-run 8 #f)");
        CHECK(again.has_value(), "cyber-cat-run 8 frames");
        if (again && is_bool(*again))
            CHECK(as_bool(*again), "cyber-cat-run returns #t");
    }

    // Canvas buffer API smoke
    {
        CompilerService cs;
        (void)cs.eval("(load \"lib/std/tui/canvas.aura\")");
        auto c = cs.eval("(make-canvas 8 4)");
        CHECK(c.has_value(), "make-canvas via load");
        auto set =
            cs.eval("(let ((c (make-canvas 4 2))) (canvas-set! c 0 0 65 1 0) (canvas-present c) "
                    "(canvas-destroy c) #t)");
        CHECK(set.has_value(), "canvas set/present/destroy");
    }

    // Sprite + run module load
    {
        CompilerService cs;
        auto a = cs.eval("(load \"lib/std/tui/sprite.aura\")");
        CHECK(a.has_value(), "load sprite");
        auto b = cs.eval("(load \"lib/std/tui/input.aura\")");
        CHECK(b.has_value(), "load input");
        auto c = cs.eval("(load \"lib/std/tui/run.aura\")");
        CHECK(c.has_value(), "load run");
        auto d = cs.eval("(load \"lib/std/tui/anim.aura\")");
        CHECK(d.has_value(), "load anim");
        auto e = cs.eval("(load \"lib/std/tui/scene.aura\")");
        CHECK(e.has_value(), "load scene");
    }

    // tui:run smoke
    {
        CompilerService cs;
        (void)cs.eval("(load \"lib/std/tui/run.aura\")");
        auto r = cs.eval("(tui:run \"smoke\" (lambda () 42))");
        CHECK(r && is_int(*r) && as_int(*r) == 42, "tui:run returns thunk result");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("cyber_cat smoke #1358: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
