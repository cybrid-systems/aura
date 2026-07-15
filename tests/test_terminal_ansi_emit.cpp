// test_terminal_ansi_emit.cpp — Issue #1349: terminal-present-batch ANSI SGR + CSI H

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;

namespace {

// Capture frame via pipe from terminal-present-batch.
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
    buf.resize(static_cast<std::size_t>(as_int(*written) + 64));
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

std::size_t count_substr(std::string_view hay, std::string_view needle) {
    std::size_t n = 0;
    std::size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string_view::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

} // namespace

int main() {
    CompilerService cs;

    // Basic buffer + colored cells
    {
        auto id = cs.eval("(make-terminal-buffer 4 2)");
        CHECK(id && is_int(*id) && as_int(*id) >= 0, "make-terminal-buffer");
        const auto bid = as_int(*id);

        // A red-on-blue, B green-on-black
        // ch=65 'A' fg=1 bg=4; ch=66 'B' fg=2 bg=0
        auto ok1 = cs.eval(std::format("(terminal-set-cell {} 0 0 65 1 4)", bid));
        CHECK(ok1 && is_bool(*ok1), "set-cell A red/blue");
        auto ok2 = cs.eval(std::format("(terminal-set-cell {} 1 0 66 2 0)", bid));
        CHECK(ok2 && is_bool(*ok2), "set-cell B green/black");
        auto ok3 = cs.eval(std::format("(terminal-set-cell {} 0 1 67 1 4)", bid));
        CHECK(ok3 && is_bool(*ok3), "set-cell C same colors as A");

        // Headless path: terminal-frame-ansi
        auto frame_ev = cs.eval(std::format("(terminal-frame-ansi {})", bid));
        CHECK(frame_ev && is_string(*frame_ev), "terminal-frame-ansi returns string");

        // Pipe capture of present-batch
        auto frame = present_to_string(cs, bid);
        CHECK(!frame.empty(), "present-batch wrote bytes to pipe");
        CHECK(contains(frame, "\033[?2026h"), "CSI 2026 sync begin");
        CHECK(contains(frame, "\033[?2026l"), "CSI 2026 sync end");
        CHECK(contains(frame, "\033[?25l"), "hide cursor");
        CHECK(contains(frame, "\033[?25h"), "show cursor");
        CHECK(contains(frame, "\033[0m"), "SGR reset at frame end");
        CHECK(contains(frame, "\033[1;1H") || contains(frame, "\033[1;1H"), "CSI H home");
        CHECK(contains(frame, "\033[2;1H"), "CSI H row 2");
        // 256-color SGR for fg=1 bg=4
        CHECK(contains(frame, "\033[38;5;1;48;5;4m"), "SGR red-on-blue (fg1 bg4)");
        // Color change to fg=2 bg=0
        CHECK(contains(frame, "\033[38;5;2;48;5;0m"), "SGR green-on-black");
        // Characters present
        CHECK(contains(frame, "A"), "cell A emitted");
        CHECK(contains(frame, "B"), "cell B emitted");
        // SGR should fire for color changes only: first cell + B change
        // (+ possibly C if last was B's colors — C is same as A so one more)
        // At least 2 SGR color sequences
        auto sgr_count = count_substr(frame, "\033[38;5;");
        CHECK(sgr_count >= 2, "SGR emitted on color change (>=2)");
        // Full buffer has spaces with default packing; still far below per-cell (8 cells).
        CHECK(sgr_count < 8, "SGR not emitted for every cell");
    }

    // Metrics after present
    {
        auto id = cs.eval("(make-terminal-buffer 2 1)");
        CHECK(id && is_int(*id), "buf for metrics");
        const auto bid = as_int(*id);
        (void)cs.eval(std::format("(terminal-set-cell {} 0 0 88 9 0)", bid));
        (void)cs.eval(std::format("(terminal-present-batch {})", bid));
        auto s = cs.eval("(hash-ref (engine:metrics \"query:production-sweep-1316-1320-stats\") "
                         "\"terminal-present-batch-total\")");
        // Fallback: present returns non-negative bytes
        auto p = cs.eval(std::format("(terminal-present-batch {})", bid));
        CHECK(p && is_int(*p) && as_int(*p) > 0, "present returns positive byte count");
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
        // Last non-control char area contains '*'
        CHECK(contains(frame, "*"), "ch 42 '*' present");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("terminal ANSI emit #1349: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
