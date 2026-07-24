// test_terminal_domain_batch.cpp — terminal domain batch driver.
// Consolidates 3 compiler_core terminal tests into one CI binary
// (same harness / CompilerService / pipe-capture pattern):
//
//   Issue #1349 — terminal-present-batch ANSI SGR + CSI H
//   Issue #1350 — 24-bit RGB + Unicode terminal cells
//   Issue #1353 — keyboard raw mode + non-blocking poll + event parse
//
// Pattern: CHECK() + run_* AC blocks (test_env_lookup_batch precedent).
// Source: cmake/AuraDomainTests.cmake · all_test_issue_targets.

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_terminal_domain_batch {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::test::g_failed;
using aura::test::g_passed;

// Capture frame via pipe from terminal-present-batch.
static std::string present_to_string(CompilerService& cs, std::int64_t bid) {
    int pipefd[2];
    if (::pipe(pipefd) != 0)
        return {};
    auto written = cs.eval(std::format("(terminal-present-batch {} {})", bid, pipefd[1]));
    ::close(pipefd[1]);
    if (!written || !is_int(*written) || as_int(*written) <= 0) {
        ::close(pipefd[0]);
        return {};
    }
    std::string buf;
    buf.resize(static_cast<std::size_t>(as_int(*written) + 128));
    const auto n = ::read(pipefd[0], buf.data(), buf.size());
    ::close(pipefd[0]);
    if (n <= 0)
        return {};
    buf.resize(static_cast<std::size_t>(n));
    return buf;
}

static bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

static std::size_t count_substr(std::string_view hay, std::string_view needle) {
    std::size_t n = 0;
    std::size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string_view::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// Embed raw bytes into an Aura string literal for tui:inject-bytes.
static bool inject_raw(CompilerService& cs, std::string_view bytes) {
    std::string expr = "(tui:inject-bytes \"";
    for (unsigned char c : bytes) {
        if (c == '"' || c == '\\') {
            expr.push_back('\\');
            expr.push_back(static_cast<char>(c));
        } else {
            expr.push_back(static_cast<char>(c));
        }
    }
    expr += "\")";
    auto r = cs.eval(expr);
    return r && is_bool(*r) && as_bool(*r);
}

static bool eval_true(CompilerService& cs, const char* expr) {
    auto r = cs.eval(expr);
    return r && is_bool(*r) && as_bool(*r);
}

// ── Issue #1349 — ANSI SGR + CSI H present-batch ──
static void run_1349_ansi_emit() {
    std::println("\n=== Issue #1349: terminal ANSI emit ===");
    CompilerService cs;

    // Basic buffer + colored cells
    {
        auto id = cs.eval("(make-terminal-buffer 4 2)");
        CHECK(id && is_int(*id) && as_int(*id) >= 0, "make-terminal-buffer");
        const auto bid = as_int(*id);

        auto ok1 = cs.eval(std::format("(terminal-set-cell {} 0 0 65 1 4)", bid));
        CHECK(ok1 && is_bool(*ok1), "set-cell A red/blue");
        auto ok2 = cs.eval(std::format("(terminal-set-cell {} 1 0 66 2 0)", bid));
        CHECK(ok2 && is_bool(*ok2), "set-cell B green/black");
        auto ok3 = cs.eval(std::format("(terminal-set-cell {} 0 1 67 1 4)", bid));
        CHECK(ok3 && is_bool(*ok3), "set-cell C same colors as A");

        auto frame_ev = cs.eval(std::format("(terminal-frame-ansi {})", bid));
        CHECK(frame_ev && is_string(*frame_ev), "terminal-frame-ansi returns string");

        auto frame = present_to_string(cs, bid);
        CHECK(!frame.empty(), "present-batch wrote bytes to pipe");
        CHECK(contains(frame, "\033[?2026h"), "CSI 2026 sync begin");
        CHECK(contains(frame, "\033[?2026l"), "CSI 2026 sync end");
        CHECK(contains(frame, "\033[?25l"), "hide cursor");
        CHECK(contains(frame, "\033[?25h"), "show cursor");
        CHECK(contains(frame, "\033[0m"), "SGR reset at frame end");
        CHECK(contains(frame, "\033[1;1H") || contains(frame, "\033[1;1H"), "CSI H home");
        CHECK(contains(frame, "\033[2;1H"), "CSI H row 2");
        CHECK(contains(frame, "\033[38;5;1;48;5;4m"), "SGR red-on-blue (fg1 bg4)");
        CHECK(contains(frame, "\033[38;5;2;48;5;0m"), "SGR green-on-black");
        CHECK(contains(frame, "A"), "cell A emitted");
        CHECK(contains(frame, "B"), "cell B emitted");
        auto sgr_count = count_substr(frame, "\033[38;5;");
        CHECK(sgr_count >= 2, "SGR emitted on color change (>=2)");
        CHECK(sgr_count < 8, "SGR not emitted for every cell");
    }

    // Metrics after present
    {
        auto id = cs.eval("(make-terminal-buffer 2 1)");
        CHECK(id && is_int(*id), "buf for metrics");
        const auto bid = as_int(*id);
        (void)cs.eval(std::format("(terminal-set-cell {} 0 0 88 9 0)", bid));
        auto p1 = cs.eval(std::format("(terminal-present-batch {})", bid));
        CHECK(p1 && is_int(*p1) && as_int(*p1) > 0, "dirty present returns positive byte count");
        auto s = cs.eval("(hash-ref (engine:metrics \"query:production-sweep-1316-1320-stats\") "
                         "\"terminal-present-batch-total\")");
        (void)s;
        // Issue #1559: clean present short-circuits (0 bytes); re-dirty for another full present.
        auto p_skip = cs.eval(std::format("(terminal-present-batch {})", bid));
        CHECK(p_skip && is_int(*p_skip) && as_int(*p_skip) == 0,
              "clean present short-circuits → 0");
        (void)cs.eval(std::format("(terminal-set-cell {} 1 0 89 9 0)", bid));
        auto p = cs.eval(std::format("(terminal-present-batch {})", bid));
        CHECK(p && is_int(*p) && as_int(*p) > 0, "re-dirty present returns positive byte count");
    }

    // Existing alias still works
    {
        auto id = cs.eval("(make-terminal-buffer 1 1)");
        CHECK(id && is_int(*id), "buf for alias");
        const auto bid = as_int(*id);
        (void)cs.eval(std::format("(terminal-set-cell {} 0 0 90 7 0)", bid));
        auto p = cs.eval(std::format("(terminal-present {})", bid));
        CHECK(p && is_int(*p) && as_int(*p) > 0, "terminal-present alias works");
    }

    // Pure helper smoke (frame must include sync bookends)
    {
        auto id = cs.eval("(make-terminal-buffer 1 1)");
        const auto bid = as_int(*id);
        (void)cs.eval(std::format("(terminal-set-cell {} 0 0 42 3 5)", bid));
        auto frame = present_to_string(cs, bid);
        CHECK(frame.find("\033[?2026h") < frame.find("\033[?2026l"), "sync begin before end");
        CHECK(frame.find("\033[0m") < frame.find("\033[?25h") ||
                  frame.find("\033[0m") != std::string::npos,
              "reset present");
        CHECK(contains(frame, "*"), "ch 42 '*' present");
    }
}

// ── Issue #1350 — RGB truecolor + Unicode cells ──
static void run_1350_rgb_unicode() {
    std::println("\n=== Issue #1350: terminal RGB / Unicode ===");
    CompilerService cs;

    {
        auto id = cs.eval("(make-terminal-buffer 2 1)");
        CHECK(id && is_int(*id) && as_int(*id) >= 0, "make-terminal-buffer");
        const auto bid = as_int(*id);
        auto ok = cs.eval(std::format("(terminal-set-cell-rgb {} 0 0 65 255 0 128 0 64 255)", bid));
        CHECK(ok && is_bool(*ok), "terminal-set-cell-rgb");
        auto frame = present_to_string(cs, bid);
        CHECK(!frame.empty(), "present wrote frame");
        CHECK(contains(frame, "\033[38;2;255;0;128;48;2;0;64;255m") ||
                  (contains(frame, "\033[38;2;255;0;128") && contains(frame, "48;2;0;64;255")),
              "truecolor SGR for RGB fg/bg");
        CHECK(contains(frame, "A"), "ASCII still present");
        CHECK(contains(frame, "\033[?2026h"), "sync begin preserved");
        CHECK(contains(frame, "\033[0m"), "reset preserved");
    }

    {
        auto id = cs.eval("(make-terminal-buffer 1 1)");
        const auto bid = as_int(*id);
        auto ok = cs.eval(std::format("(terminal-set-cell-rgb {} 0 0 9600 255 0 0 0 0 255)", bid));
        CHECK(ok && is_bool(*ok), "set half-block via codepoint");
        auto frame = present_to_string(cs, bid);
        CHECK(contains(frame, "\xE2\x96\x80"), "UTF-8 for U+2580 half-block");
        CHECK(contains(frame, "\033[38;2;255;0;0"), "RGB red fg for half-block");
    }

    {
        auto id = cs.eval("(make-terminal-buffer 1 1)");
        const auto bid = as_int(*id);
        auto ok = cs.eval(std::format("(terminal-set-cell {} 0 0 10303 7 0)", bid)); // 0x283F
        CHECK(ok && is_bool(*ok), "set braille via terminal-set-cell codepoint");
        auto frame = present_to_string(cs, bid);
        CHECK(contains(frame, "\xE2\xA0\xBF"), "UTF-8 for U+283F braille");
        CHECK(contains(frame, "\033[38;5;7;48;5;0m"), "palette SGR for legacy set-cell");
    }

    {
        auto id = cs.eval("(make-terminal-buffer 1 1)");
        const auto bid = as_int(*id);
        auto ok = cs.eval(
            std::format("(terminal-set-cell-unicode {} 0 0 \"\\u2580\" 0 255 0 0 0 0)", bid));
        if (!ok || !is_bool(*ok) || !as_bool(*ok)) {
            ok = cs.eval(std::format("(terminal-set-cell-rgb {} 0 0 9600 0 255 0 0 0 0)", bid));
        }
        CHECK(ok && is_bool(*ok), "unicode/rgb set path");
        auto frame = present_to_string(cs, bid);
        CHECK(contains(frame, "\xE2\x96\x80") || contains(frame, "\033[38;2;0;255;0"),
              "unicode cell rendered");
    }

    {
        auto id = cs.eval("(make-terminal-buffer 1 1)");
        const auto bid = as_int(*id);
        std::string expr =
            std::format("(terminal-set-cell-unicode {} 0 0 \"\xE2\x96\x80\" 10 20 30)", bid);
        auto ok = cs.eval(expr);
        CHECK(ok && is_bool(*ok), "set-cell-unicode with UTF-8 string");
        auto frame = present_to_string(cs, bid);
        CHECK(contains(frame, "\xE2\x96\x80"), "UTF-8 half-block from string");
        CHECK(contains(frame, "\033[38;2;10;20;30"), "RGB from unicode setter");
    }

    {
        auto id = cs.eval("(make-terminal-buffer 2 1)");
        const auto bid = as_int(*id);
        (void)cs.eval(std::format("(terminal-set-cell {} 0 0 65 1 4)", bid));
        (void)cs.eval(std::format("(terminal-set-cell {} 1 0 66 2 0)", bid));
        auto frame = present_to_string(cs, bid);
        CHECK(contains(frame, "\033[38;5;1;48;5;4m"), "legacy palette SGR");
        CHECK(contains(frame, "A") && contains(frame, "B"), "legacy cells");
    }

    {
        auto a = cs.eval("(make-terminal-buffer 2 2)");
        auto b = cs.eval("(make-terminal-buffer 2 2)");
        const auto aid = as_int(*a);
        const auto bid = as_int(*b);
        (void)cs.eval(std::format("(terminal-set-cell-rgb {} 0 0 88 1 2 3 4 5 6)", aid));
        auto d0 = cs.eval(std::format("(terminal-diff-update {} {})", aid, bid));
        CHECK(d0 && is_int(*d0) && as_int(*d0) >= 1, "diff detects RGB cell change");
        (void)cs.eval(std::format("(terminal-set-cell-rgb {} 0 0 88 1 2 3 4 5 6)", bid));
        auto d1 = cs.eval(std::format("(terminal-diff-update {} {})", aid, bid));
        CHECK(d1 && is_int(*d1) && as_int(*d1) == 0, "diff zero when RGB cells match");
    }
}

// ── Issue #1353 — raw mode + poll + event parse ──
static void run_1353_input() {
    std::println("\n=== Issue #1353: terminal input ===");
    CompilerService cs;

    {
        auto e = cs.eval("(tui:read-event 0)");
        CHECK(e && is_bool(*e) && !as_bool(*e), "read-event timeout=0 empty → #f");
    }

    {
        auto a = cs.eval("(tui:raw-mode-on)");
        CHECK(a && is_bool(*a) && as_bool(*a), "raw-mode-on");
        auto b = cs.eval("(tui:raw-mode-on)");
        CHECK(b && is_bool(*b) && as_bool(*b), "raw-mode-on idempotent");
        auto ir = cs.eval("(tui:is-raw-mode)");
        CHECK(ir && is_bool(*ir) && as_bool(*ir), "is-raw-mode after on");
        auto off = cs.eval("(tui:raw-mode-off)");
        CHECK(off && is_bool(*off) && as_bool(*off), "raw-mode-off");
        auto ir2 = cs.eval("(tui:is-raw-mode)");
        CHECK(ir2 && is_bool(*ir2) && !as_bool(*ir2), "is-raw-mode after off");
    }

    {
        (void)cs.eval("(tui:raw-mode-on)");
        CHECK(inject_raw(cs, "\x1b[A"), "inject up arrow CSI");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key")
                                   (equal? (cdr e) "↑"))))"),
              "↑ arrow key event");

        CHECK(inject_raw(cs, "\x1b[B"), "inject down");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key")
                                   (equal? (cdr e) "↓"))))"),
              "↓ arrow key event");

        CHECK(inject_raw(cs, "\x1b[C"), "inject right");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key")
                                   (equal? (cdr e) "→"))))"),
              "→ arrow key event");

        CHECK(inject_raw(cs, "\x1b[D"), "inject left");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key")
                                   (equal? (cdr e) "←"))))"),
              "← arrow key event");
        (void)cs.eval("(tui:raw-mode-off)");
    }

    {
        CHECK(inject_raw(cs, "\r"), "inject enter");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key"))))"),
              "Enter → key event");

        CHECK(inject_raw(cs, " "), "inject space");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key")
                                   (equal? (cdr e) " "))))"),
              "Space → key");

        CHECK(inject_raw(cs, "\x1b"), "inject esc");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key"))))"),
              "Escape → key");

        CHECK(inject_raw(cs, "q"), "inject q");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key")
                                   (equal? (cdr e) "q"))))"),
              "q → key");
    }

    {
        CHECK(inject_raw(cs, std::string_view("\x03", 1)), "inject ctrl-c");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "quit"))))"),
              "Ctrl-C → quit event");
    }

    {
        CHECK(inject_raw(cs, "\x1b[<0;5;10M"), "inject mouse press");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "mouse")
                                   (pair? (cdr e))
                                   (= (car (cdr e)) 0)
                                   (= (car (cdr (cdr e))) 5)
                                   (= (cdr (cdr (cdr e))) 10))))"),
              "mouse SGR event btn/col/row");
    }

    {
        CHECK(eval_true(cs,
                        R"((let ((s (tui:terminal-size)))
                              (and (pair? s) (> (car s) 0) (> (cdr s) 0))))"),
              "terminal-size positive rows/cols");
    }

    {
        auto m = cs.eval("(tui:enable-mouse)");
        CHECK(m && is_bool(*m) && as_bool(*m), "enable-mouse");
    }

    {
        (void)cs.eval("(tui:init \"in\" 8 4)");
        (void)cs.eval("(tui:inject-key \"x\")");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key")
                                   (equal? (cdr e) "x"))))"),
              "inject-key still works");
        (void)cs.eval("(tui:shutdown)");
    }

    {
        auto e = cs.eval("(tui:read-event 0)");
        CHECK(e && is_bool(*e) && !as_bool(*e), "empty after drain → #f");
    }
}

} // namespace aura_terminal_domain_batch

int main() {
    aura_terminal_domain_batch::run_1349_ansi_emit();
    aura_terminal_domain_batch::run_1350_rgb_unicode();
    aura_terminal_domain_batch::run_1353_input();
    if (::aura::test::g_failed)
        return 1;
    std::println("terminal domain batch (#1349/#1350/#1353): OK ({} passed)",
                 ::aura::test::g_passed);
    return 0;
}
