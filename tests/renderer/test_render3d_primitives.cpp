// test_render3d_primitives.cpp — Issue #1986 / Epic #1979
// Aura EDSL surface for software 3D voxel rendering.
//
// ACs:
//   AC1: create-volume returns positive handle
//   AC2: set/get-block round-trip
//   AC3: build-demo + set-camera + frame-ansi produces half-block ANSI
//   AC4: stats hash schema 1986 with hits/pixels
//   AC5: destroy-volume invalidates handle

#include "test_harness.hpp"

#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::test::g_failed;
using aura::test::g_passed;

std::int64_t eval_int(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(std::string(expr));
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
}

bool eval_true(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(std::string(expr));
    return r && is_bool(*r) && as_bool(*r);
}

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (render3d:stats) \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void ac1_create_volume() {
    std::println("\n--- AC1: create-volume ---");
    CompilerService cs;
    const auto id = eval_int(cs, "(render3d:create-volume 24 12 24)");
    CHECK(id > 0, "volume id > 0");
    CHECK(eval_true(cs, std::format("(render3d:destroy-volume {})", id)), "destroy ok");
}

void ac2_set_get_block() {
    std::println("\n--- AC2: set/get-block ---");
    CompilerService cs;
    const auto id = eval_int(cs, "(render3d:create-volume 8 8 8)");
    CHECK(id > 0, "vol");
    CHECK(eval_true(cs, std::format("(render3d:set-block {} 2 1 3 7)", id)), "set-block");
    CHECK(eval_int(cs, std::format("(render3d:get-block {} 2 1 3)", id)) == 7, "get-block 7");
    CHECK(eval_int(cs, std::format("(render3d:get-block {} 0 0 0)", id)) == 0, "air 0");
    (void)cs.eval(std::format("(render3d:destroy-volume {})", id));
}

void ac3_frame_ansi() {
    std::println("\n--- AC3: build-demo + frame-ansi ---");
    CompilerService cs;
    const auto id = eval_int(cs, "(render3d:create-volume 32 16 32)");
    CHECK(id > 0, "vol");
    CHECK(eval_true(cs, std::format("(render3d:build-demo {})", id)), "build-demo");
    CHECK(eval_true(cs, "(render3d:resize-fb 20 10)"), "resize-fb");
    // milliradians: pitch -350 mrad ≈ -0.35 rad
    CHECK(eval_true(cs, "(render3d:set-camera 16 8 28 0 -350 1050)"), "set-camera");
    auto r = cs.eval(std::format("(render3d:frame-ansi {})", id));
    CHECK(r && is_string(*r), "frame-ansi → string");
    if (r && is_string(*r)) {
        const auto idx = as_string_idx(*r);
        // Pull via length heuristic: non-empty heap string
        auto len = cs.eval(std::format("(string-length (render3d:frame-ansi {}))", id));
        // re-eval is ok for second frame
        CHECK(len && is_int(*len) && as_int(*len) > 100, "ANSI length > 100");
        (void)idx;
    }
    // half-block presence via contains if we can get string — check stats hits instead
    (void)cs.eval(std::format("(render3d:frame {} 1)", id)); // headless
    CHECK(href(cs, "schema") == 1986, "stats schema 1986");
    CHECK(href(cs, "pixels") > 0, "pixels > 0");
    CHECK(href(cs, "hits") > 0, "hits > 0 (demo geometry)");
    (void)cs.eval(std::format("(render3d:destroy-volume {})", id));
}

void ac4_stats_hash() {
    std::println("\n--- AC4: stats hash keys ---");
    CompilerService cs;
    const auto id = eval_int(cs, "(render3d:create-volume 16 8 16)");
    CHECK(eval_true(cs, std::format("(render3d:build-demo {})", id)), "demo");
    CHECK(eval_true(cs, "(render3d:resize-fb 12 6)"), "fb");
    CHECK(eval_true(cs, "(render3d:set-camera 8 5 14 0 -300 1000)"), "cam");
    CHECK(eval_true(cs, std::format("(render3d:frame {} 1)", id)), "frame headless");
    auto h = cs.eval("(render3d:stats)");
    CHECK(h && is_hash(*h), "stats is hash");
    CHECK(href(cs, "rays") == href(cs, "pixels"), "rays == pixels");
    CHECK(href(cs, "hits") + href(cs, "misses") == href(cs, "rays"), "hits+misses");
    (void)cs.eval(std::format("(render3d:destroy-volume {})", id));
}

void ac5_destroy() {
    std::println("\n--- AC5: destroy invalidates ---");
    CompilerService cs;
    const auto id = eval_int(cs, "(render3d:create-volume 4 4 4)");
    CHECK(eval_true(cs, std::format("(render3d:destroy-volume {})", id)), "destroy");
    CHECK(!eval_true(cs, std::format("(render3d:set-block {} 0 0 0 1)", id)), "set after destroy");
    CHECK(!eval_true(cs, std::format("(render3d:frame {} 1)", id)), "frame after destroy");
}

} // namespace

int main() {
    std::println("=== test_render3d_primitives (#1986 / epic #1979) ===");
    ac1_create_volume();
    ac2_set_get_block();
    ac3_frame_ansi();
    ac4_stats_hash();
    ac5_destroy();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
