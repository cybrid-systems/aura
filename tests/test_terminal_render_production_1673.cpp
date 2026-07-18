// @category: unit
// @reason: Issue #1673 — production terminal render closed loop:
// make-terminal-buffer + set-cell + terminal-diff-update (dirty mark) +
// terminal-present-batch (zero-copy / dirty short-circuit) + query:render-stats.
//
//   AC1: make-terminal-buffer creates live ids
//   AC2: terminal-set-cell writes; present-batch emits >0 bytes when dirty
//   AC3: second present-batch short-circuits to 0 bytes (clean dirty)
//   AC4: terminal-diff-update detects cell changes and returns count
//   AC5: after diff, present-batch can emit again (dirty marked on new)
//   AC6: query:render-stats schema 1673; present-calls advanced

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
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t as_i(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(expr);
    if (!r || !is_int(*r))
        return -999;
    return as_int(*r);
}

static std::int64_t href(CompilerService& cs, std::string_view expr, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \"{}\")", expr, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Present to /dev/null fd to avoid flooding the test TTY.
static int open_null_fd() {
    return ::open("/dev/null", O_WRONLY);
}

static void ac1_create() {
    std::println("\n--- AC1: make-terminal-buffer ---");
    CompilerService cs;
    auto a = as_i(cs, "(make-terminal-buffer 8 4)");
    auto b = as_i(cs, "(make-terminal-buffer 8 4)");
    CHECK(a >= 0 && b >= 0, "buffer ids non-negative");
    CHECK(a != b, "distinct buffer ids");
}

static void ac2_present_dirty() {
    std::println("\n--- AC2: present-batch when dirty ---");
    CompilerService cs;
    const auto id = as_i(cs, "(make-terminal-buffer 4 2)");
    CHECK(id >= 0, "buf ok");
    CHECK(as_i(cs, std::format("(if (terminal-set-cell {} 0 0 65 7 0) 1 0)", id)) == 1,
          "set-cell A");
    const int nfd = open_null_fd();
    CHECK(nfd >= 0, "open /dev/null");
    const auto bytes = as_i(cs, std::format("(terminal-present-batch {} {})", id, nfd));
    ::close(nfd);
    CHECK(bytes > 0, "present emits ANSI when dirty");
}

static void ac3_present_skip() {
    std::println("\n--- AC3: present-batch short-circuit when clean ---");
    CompilerService cs;
    const auto id = as_i(cs, "(make-terminal-buffer 4 2)");
    CHECK(as_i(cs, std::format("(if (terminal-set-cell {} 1 0 66 7 0) 1 0)", id)) == 1, "set B");
    const int nfd = open_null_fd();
    (void)as_i(cs, std::format("(terminal-present-batch {} {})", id, nfd));
    const auto bytes2 = as_i(cs, std::format("(terminal-present-batch {} {})", id, nfd));
    ::close(nfd);
    CHECK(bytes2 == 0, "second present returns 0 (dirty short-circuit)");
}

static void ac4_diff_count() {
    std::println("\n--- AC4: terminal-diff-update count ---");
    CompilerService cs;
    const auto a = as_i(cs, "(make-terminal-buffer 3 2)");
    const auto b = as_i(cs, "(make-terminal-buffer 3 2)");
    CHECK(as_i(cs, std::format("(if (terminal-set-cell {} 0 0 88 7 0) 1 0)", b)) == 1,
          "set X on b");
    const auto d0 = as_i(cs, std::format("(terminal-diff-update {} {})", a, b));
    CHECK(d0 >= 1, "diff detects at least one changed cell");
    const auto d1 = as_i(cs, std::format("(terminal-diff-update {} {})", b, b));
    CHECK(d1 == 0, "diff same buffer → 0");
}

static void ac5_diff_marks_dirty_for_present() {
    std::println("\n--- AC5: diff marks dirty on new for present ---");
    CompilerService cs;
    const auto front = as_i(cs, "(make-terminal-buffer 4 2)");
    const auto back = as_i(cs, "(make-terminal-buffer 4 2)");
    // Present front once to clear its dirty (full-frame from create).
    const int nfd = open_null_fd();
    (void)as_i(cs, std::format("(terminal-present-batch {} {})", front, nfd));
    // Mutate *back* without using set-cell dirty path... actually set-cell marks dirty
    // on back. Present back once to clear, then copy-like mutate via set-cell again?
    // Flow: present back (clear dirty) → set-cell on back marks dirty → present works.
    // To test *diff-driven* dirty: present both clean, then change back via set-cell,
    // present back to clear, then change front only and copy by re-setting back cells
    // without set-cell... hard without raw cell write.
    // Practical: set-cell on back, present clears dirty; make front different by set-cell
    // on front only; then *re-apply* back cells via terminal-set-cell again would re-dirty.
    // Instead: after present(back) clears, call terminal-diff-update(front, back) while
    // front≠back so diff re-marks dirty on back even if we cleared it.
    CHECK(as_i(cs, std::format("(if (terminal-set-cell {} 0 0 90 7 0) 1 0)", back)) == 1,
          "Z on back");
    (void)as_i(cs, std::format("(terminal-present-batch {} {})", back, nfd)); // clear dirty
    const auto skip = as_i(cs, std::format("(terminal-present-batch {} {})", back, nfd));
    CHECK(skip == 0, "back clean before diff re-mark");
    // front still all spaces; back has Z → diff should re-dirty back
    const auto ch = as_i(cs, std::format("(terminal-diff-update {} {})", front, back));
    CHECK(ch >= 1, "diff count after present-clean");
    const auto bytes = as_i(cs, std::format("(terminal-present-batch {} {})", back, nfd));
    ::close(nfd);
    CHECK(bytes > 0, "present after diff re-mark emits again");
}

static void ac6_render_stats() {
    std::println("\n--- AC6: query:render-stats ---");
    CompilerService cs;
    const auto id = as_i(cs, "(make-terminal-buffer 2 2)");
    CHECK(as_i(cs, std::format("(if (terminal-set-cell {} 0 0 65 7 0) 1 0)", id)) == 1, "cell");
    const int nfd = open_null_fd();
    (void)as_i(cs, std::format("(terminal-present-batch {} {})", id, nfd));
    ::close(nfd);
    auto r = cs.eval("(stats:get \"query:render-stats\")");
    CHECK(r && is_hash(*r), "render-stats is hash");
    // Schema advanced to 1674 when #1674 wired full bump surface; still accept 1673.
    const auto schema = href(cs, "(stats:get \"query:render-stats\")", "schema");
    CHECK(schema == 1676 || schema == 1674 || schema == 1673, "schema 1673/1674/1676");
    CHECK(href(cs, "(stats:get \"query:render-stats\")", "present-calls") >= 1, "present-calls");
    CHECK(href(cs, "(stats:get \"query:render-stats\")", "buffer-creates") >= 1, "buffer-creates");
    const auto issue = href(cs, "(stats:get \"query:render-stats\")", "issue");
    CHECK(issue == 1676 || issue == 1674 || issue == 1673, "issue field");
}

} // namespace

int main() {
    std::println("=== Issue #1673: production terminal render closed loop ===");
    ac1_create();
    ac2_present_dirty();
    ac3_present_skip();
    ac4_diff_count();
    ac5_diff_marks_dirty_for_present();
    ac6_render_stats();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
