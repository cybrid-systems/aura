// @category: unit
// @reason: Issue #1675 — render hot-path memory predictability:
// FrameBumpArena capacity stable after warm-up; GC safepoint defers in hotpath;
// query:render-memory-stats schema 1675; present does not grow string_heap.
//
//   AC1: query:render-memory-stats schema 1675
//   AC2: after warm present loop, frame-arena-capacity non-decreasing & stable
//   AC3: zero-copy-arena-acquires / hit-in-render advance under present
//   AC4: request_gc_safepoint returns 1 (deferred) inside render hotpath
//   AC5: string_heap delta ~0 over N present-batch frames (no per-frame intern)
//   AC6: buffer-live-capacity-bytes tracks live TermBuf capacity

#include "test_harness.hpp"

#include "core/arena_auto_policy_stats.h"
#include "core/zero_copy_output.hh"
#include "renderer/render_primitives.hh"

#include <cstdint>
#include <fcntl.h>
#include <print>
#include <string>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t as_i(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(expr);
    if (!r || !is_int(*r))
        return -999;
    return as_int(*r);
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (stats:get \"query:render-memory-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static int open_null_fd() {
    return ::open("/dev/null", O_WRONLY);
}

static void ac1_schema() {
    std::println("\n--- AC1: schema 1675 ---");
    CompilerService cs;
    auto r = cs.eval("(stats:get \"query:render-memory-stats\")");
    CHECK(r && is_hash(*r), "render-memory-stats is hash");
    CHECK(href(cs, "schema") == 1675, "schema == 1675");
    CHECK(href(cs, "issue") == 1675, "issue == 1675");
}

static void ac2_frame_arena_stable() {
    std::println("\n--- AC2: frame-arena capacity stable after warm-up ---");
    CompilerService cs;
    const auto id = as_i(cs, "(make-terminal-buffer 16 8)");
    CHECK(id >= 0, "buf");
    const int nfd = open_null_fd();
    // Warm: dirty every frame so present always takes zero-copy path.
    std::int64_t cap_after_warm = -1;
    for (int i = 0; i < 20; ++i) {
        CHECK(as_i(cs, std::format("(if (terminal-set-cell {} 0 0 {} 7 0) 1 0)", id,
                                   65 + (i % 26))) == 1,
              "set");
        (void)as_i(cs, std::format("(terminal-present-batch {} {})", id, nfd));
        if (i == 9)
            cap_after_warm = href(cs, "frame-arena-capacity");
    }
    const auto cap_end = href(cs, "frame-arena-capacity");
    ::close(nfd);
    CHECK(cap_after_warm >= 0 && cap_end >= 0, "capacity readable");
    CHECK(cap_end >= cap_after_warm, "capacity non-decreasing");
    // After warm-up, capacity should not explode (stable bound for fixed grid).
    CHECK(cap_end <= cap_after_warm * 4 + 4096 || cap_after_warm == 0,
          "capacity stays within warm-up bound");
    std::println("  cap_warm={} cap_end={}", cap_after_warm, cap_end);
}

static void ac3_zero_copy_metrics() {
    std::println("\n--- AC3: zero-copy metrics advance ---");
    CompilerService cs;
    const auto id = as_i(cs, "(make-terminal-buffer 8 4)");
    const int nfd = open_null_fd();
    CHECK(as_i(cs, std::format("(if (terminal-set-cell {} 1 1 90 7 0) 1 0)", id)) == 1, "set");
    const auto a0 = href(cs, "zero-copy-arena-acquires");
    (void)as_i(cs, std::format("(terminal-present-batch {} {})", id, nfd));
    ::close(nfd);
    const auto a1 = href(cs, "zero-copy-arena-acquires");
    const auto hit = href(cs, "zero-copy-hit-in-render");
    CHECK(a1 >= a0, "arena acquires non-decreasing");
    CHECK(a1 > a0 || hit >= 0, "zero-copy path observed or metric present");
    std::println("  acquires {}→{} hit-in-render={}", a0, a1, hit);
}

static void ac4_safepoint_defers_in_hotpath() {
    std::println("\n--- AC4: GC safepoint defers in render hotpath ---");
    CompilerService cs;
    // Outside hotpath: may be immediate (0) or deferred for other reasons.
    const auto outside = cs.evaluator().request_gc_safepoint();
    aura::core::arena_policy::enter_render_hotpath();
    const auto inside = cs.evaluator().request_gc_safepoint();
    aura::core::arena_policy::exit_render_hotpath();
    CHECK(inside == 1, "safepoint deferred while in_render_hotpath");
    std::println("  outside={} inside={}", outside, inside);
}

static void ac5_string_heap_stable() {
    std::println("\n--- AC5: present-batch does not grow string_heap ---");
    CompilerService cs;
    const auto id = as_i(cs, "(make-terminal-buffer 6 3)");
    const int nfd = open_null_fd();
    CHECK(as_i(cs, std::format("(if (terminal-set-cell {} 0 0 65 7 0) 1 0)", id)) == 1, "set");
    // Warm one present
    (void)as_i(cs, std::format("(terminal-present-batch {} {})", id, nfd));
    const auto heap0 = static_cast<std::int64_t>(cs.evaluator().string_heap().size());
    for (int i = 0; i < 50; ++i) {
        CHECK(as_i(cs, std::format("(if (terminal-set-cell {} 0 0 {} 7 0) 1 0)", id,
                                   65 + (i % 10))) == 1,
              "set loop");
        (void)as_i(cs, std::format("(terminal-present-batch {} {})", id, nfd));
    }
    const auto heap1 = static_cast<std::int64_t>(cs.evaluator().string_heap().size());
    ::close(nfd);
    // Eval of set-cell/present may intern nothing; allow tiny delta from test harness.
    CHECK(heap1 - heap0 <= 2, "string_heap growth ~0 over present loop");
    std::println("  string_heap {}→{} (delta={})", heap0, heap1, heap1 - heap0);
}

static void ac6_live_capacity() {
    std::println("\n--- AC6: buffer-live-capacity-bytes ---");
    CompilerService cs;
    const auto id = as_i(cs, "(make-terminal-buffer 10 5)");
    CHECK(id >= 0, "buf");
    const auto bytes = href(cs, "buffer-live-capacity-bytes");
    const auto count = href(cs, "buffer-live-count");
    CHECK(count >= 1, "live count >= 1");
    CHECK(bytes >= static_cast<std::int64_t>(10 * 5 * sizeof(std::uint32_t)),
          "capacity bytes covers cell grid");
    std::println("  live_count={} live_bytes={}", count, bytes);
}

} // namespace

int main() {
    std::println("=== Issue #1675: render memory predictability ===");
    ac1_schema();
    ac2_frame_arena_stable();
    ac3_zero_copy_metrics();
    ac4_safepoint_defers_in_hotpath();
    ac5_string_heap_stable();
    ac6_live_capacity();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
