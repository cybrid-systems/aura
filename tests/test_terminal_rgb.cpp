// test_terminal_rgb.cpp — Issue #1350: 24-bit RGB + Unicode terminal cells

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;

namespace {

std::string present_to_string(CompilerService& cs, std::int64_t bid) {
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

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

} // namespace

int main() {
    CompilerService cs;

    // #1350 AC: terminal-set-cell-rgb emits truecolor SGR
    {
        auto id = cs.eval("(make-terminal-buffer 2 1)");
        CHECK(id && is_int(*id) && as_int(*id) >= 0, "make-terminal-buffer");
        const auto bid = as_int(*id);
        // ch='A'(65), fg RGB 255,0,128, bg RGB 0,64,255
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

    // #1350 AC: half-block Unicode U+2580 ▀ as UTF-8 E2 96 80
    {
        auto id = cs.eval("(make-terminal-buffer 1 1)");
        const auto bid = as_int(*id);
        // U+2580 = 9600
        auto ok = cs.eval(std::format("(terminal-set-cell-rgb {} 0 0 9600 255 0 0 0 0 255)", bid));
        CHECK(ok && is_bool(*ok), "set half-block via codepoint");
        auto frame = present_to_string(cs, bid);
        CHECK(contains(frame, "\xE2\x96\x80"), "UTF-8 for U+2580 half-block");
        CHECK(contains(frame, "\033[38;2;255;0;0"), "RGB red fg for half-block");
    }

    // #1350 AC: braille U+283F ⠿ as UTF-8 E2 A0 BF
    {
        auto id = cs.eval("(make-terminal-buffer 1 1)");
        const auto bid = as_int(*id);
        auto ok = cs.eval(std::format("(terminal-set-cell {} 0 0 10303 7 0)", bid)); // 0x283F
        CHECK(ok && is_bool(*ok), "set braille via terminal-set-cell codepoint");
        auto frame = present_to_string(cs, bid);
        CHECK(contains(frame, "\xE2\xA0\xBF"), "UTF-8 for U+283F braille");
        // palette path still emits 256-color SGR
        CHECK(contains(frame, "\033[38;5;7;48;5;0m"), "palette SGR for legacy set-cell");
    }

    // #1350 AC: terminal-set-cell-unicode from string
    {
        auto id = cs.eval("(make-terminal-buffer 1 1)");
        const auto bid = as_int(*id);
        // half-block string + RGB
        auto ok = cs.eval(
            std::format("(terminal-set-cell-unicode {} 0 0 \"\\u2580\" 0 255 0 0 0 0)", bid));
        // Aura string may not support \u escapes — use raw UTF-8 in source if needed.
        if (!ok || !is_bool(*ok) || !as_bool(*ok)) {
            // Fallback: pass codepoint path already covered; try literal bytes via int form
            ok = cs.eval(std::format("(terminal-set-cell-rgb {} 0 0 9600 0 255 0 0 0 0)", bid));
        }
        CHECK(ok && is_bool(*ok), "unicode/rgb set path");
        auto frame = present_to_string(cs, bid);
        CHECK(contains(frame, "\xE2\x96\x80") || contains(frame, "\033[38;2;0;255;0"),
              "unicode cell rendered");
    }

    // Explicit string with UTF-8 half-block via heap: use eval with utf8 in format
    {
        auto id = cs.eval("(make-terminal-buffer 1 1)");
        const auto bid = as_int(*id);
        // Embed UTF-8 for ▀ directly
        std::string expr =
            std::format("(terminal-set-cell-unicode {} 0 0 \"\xE2\x96\x80\" 10 20 30)", bid);
        auto ok = cs.eval(expr);
        CHECK(ok && is_bool(*ok), "set-cell-unicode with UTF-8 string");
        auto frame = present_to_string(cs, bid);
        CHECK(contains(frame, "\xE2\x96\x80"), "UTF-8 half-block from string");
        CHECK(contains(frame, "\033[38;2;10;20;30"), "RGB from unicode setter");
    }

    // Backward compat: 256-color set-cell still works (#1349)
    {
        auto id = cs.eval("(make-terminal-buffer 2 1)");
        const auto bid = as_int(*id);
        (void)cs.eval(std::format("(terminal-set-cell {} 0 0 65 1 4)", bid));
        (void)cs.eval(std::format("(terminal-set-cell {} 1 0 66 2 0)", bid));
        auto frame = present_to_string(cs, bid);
        CHECK(contains(frame, "\033[38;5;1;48;5;4m"), "legacy palette SGR");
        CHECK(contains(frame, "A") && contains(frame, "B"), "legacy cells");
    }

    // Diff still works with TermCell equality
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

    if (::aura::test::g_failed)
        return 1;
    std::println("terminal RGB/Unicode #1350: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
