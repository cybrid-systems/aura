// @category: unit
// @reason: Issue #1674 — wire render hot-path bump_* counters; query:render-stats
// surfaces non-zero fields; CompilerService::snapshot exposes synthetic
// terminal-present-batch FnMetrics.
//
//   AC1: after create/set/present, query:render-stats schema 1674
//   AC2: term-render-present / present-batch-total > 0
//   AC3: after diff, term-buf-diff > 0
//   AC4: dirty short-circuit bumps hp-dirty-hits
//   AC5: snapshot().functions contains terminal-present-batch with total_calls>0
//   AC6: draw-batch path bumps term-render-draw-batch

#include "test_harness.hpp"

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
    auto r = cs.eval(std::format("(hash-ref (stats:get \"query:render-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static int open_null_fd() {
    return ::open("/dev/null", O_WRONLY);
}

static void ac1_schema() {
    std::println("\n--- AC1: query:render-stats schema 1674 ---");
    CompilerService cs;
    const auto id = as_i(cs, "(make-terminal-buffer 4 2)");
    CHECK(id >= 0, "buf");
    CHECK(as_i(cs, std::format("(if (terminal-set-cell {} 0 0 65 7 0) 1 0)", id)) == 1, "cell");
    const int nfd = open_null_fd();
    (void)as_i(cs, std::format("(terminal-present-batch {} {})", id, nfd));
    ::close(nfd);
    auto r = cs.eval("(stats:get \"query:render-stats\")");
    CHECK(r && is_hash(*r), "render-stats hash");
    // Schema advanced by #1676 (dispatch fences); accept 1674+ lineage.
    CHECK(href(cs, "schema") >= 1674, "schema >= 1674");
    CHECK(href(cs, "issue") >= 1674, "issue >= 1674");
}

static void ac2_present_wired() {
    std::println("\n--- AC2: present bumps non-zero ---");
    CompilerService cs;
    const auto id = as_i(cs, "(make-terminal-buffer 3 2)");
    CHECK(as_i(cs, std::format("(if (terminal-set-cell {} 0 0 66 7 0) 1 0)", id)) == 1, "cell");
    const int nfd = open_null_fd();
    (void)as_i(cs, std::format("(terminal-present-batch {} {})", id, nfd));
    ::close(nfd);
    (void)cs.eval("(stats:get \"query:render-stats\")");
    CHECK(href(cs, "term-render-present") >= 1, "term-render-present >= 1");
    CHECK(href(cs, "present-batch-total") >= 1, "present-batch-total >= 1");
    CHECK(href(cs, "term-render-clear") >= 1 || href(cs, "buffer-creates") >= 1,
          "buffer create path counted");
}

static void ac3_diff_wired() {
    std::println("\n--- AC3: diff bumps ---");
    CompilerService cs;
    const auto a = as_i(cs, "(make-terminal-buffer 3 2)");
    const auto b = as_i(cs, "(make-terminal-buffer 3 2)");
    CHECK(as_i(cs, std::format("(if (terminal-set-cell {} 0 0 88 7 0) 1 0)", b)) == 1, "set");
    CHECK(as_i(cs, std::format("(terminal-diff-update {} {})", a, b)) >= 1, "diff>0");
    (void)cs.eval("(stats:get \"query:render-stats\")");
    CHECK(href(cs, "term-buf-diff") >= 1, "term-buf-diff >= 1");
    CHECK(href(cs, "term-buf-diff-hits") >= 1, "term-buf-diff-hits >= 1");
}

static void ac4_skip_wired() {
    std::println("\n--- AC4: dirty short-circuit hp-dirty-hits ---");
    CompilerService cs;
    const auto id = as_i(cs, "(make-terminal-buffer 2 2)");
    CHECK(as_i(cs, std::format("(if (terminal-set-cell {} 0 0 67 7 0) 1 0)", id)) == 1, "cell");
    const int nfd = open_null_fd();
    (void)as_i(cs, std::format("(terminal-present-batch {} {})", id, nfd));
    CHECK(as_i(cs, std::format("(terminal-present-batch {} {})", id, nfd)) == 0, "skip");
    ::close(nfd);
    (void)cs.eval("(stats:get \"query:render-stats\")");
    CHECK(href(cs, "hp-dirty-hits") >= 1 || href(cs, "present-skips") >= 1,
          "short-circuit observed");
}

static void ac5_snapshot_fn() {
    std::println("\n--- AC5: snapshot FnMetrics for present ---");
    CompilerService cs;
    const auto id = as_i(cs, "(make-terminal-buffer 2 2)");
    CHECK(as_i(cs, std::format("(if (terminal-set-cell {} 0 0 68 7 0) 1 0)", id)) == 1, "cell");
    const int nfd = open_null_fd();
    (void)as_i(cs, std::format("(terminal-present-batch {} {})", id, nfd));
    ::close(nfd);
    auto snap = cs.snapshot();
    bool found = false;
    for (const auto& f : snap.functions) {
        if (f.name == "terminal-present-batch") {
            found = true;
            CHECK(f.total_calls >= 1, "synthetic FnMetrics total_calls >= 1");
            std::println("  total_calls={} hit_rate={:.2f}", f.total_calls, f.hit_rate);
        }
    }
    CHECK(found, "snapshot contains terminal-present-batch");
}

static void ac6_draw_batch() {
    std::println("\n--- AC6: render-draw-batch bump ---");
    CompilerService cs;
    const auto id = as_i(cs, "(make-terminal-buffer 3 2)");
    CHECK(as_i(cs, std::format("(render-draw-batch {} 0 0 69 7 0)", id)) >= 0, "draw");
    (void)cs.eval("(stats:get \"query:render-stats\")");
    CHECK(href(cs, "term-render-draw-batch") >= 1, "term-render-draw-batch >= 1");
}

} // namespace

int main() {
    std::println("=== Issue #1674: render hot-path observability ===");
    ac1_schema();
    ac2_present_wired();
    ac3_diff_wired();
    ac4_skip_wired();
    ac5_snapshot_fn();
    ac6_draw_batch();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
