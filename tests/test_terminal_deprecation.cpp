// test_terminal_deprecation.cpp — Issue #1351: deprecate 7 no-op terminal:* primitives

#include "test_harness.hpp"

#include <cstdio>
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

// Capture stderr while running fn; restore after.
std::string capture_stderr(const std::function<void()>& fn) {
    int pipefd[2];
    if (::pipe(pipefd) != 0)
        return {};
    const int old_stderr = ::dup(STDERR_FILENO);
    if (old_stderr < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return {};
    }
    if (::dup2(pipefd[1], STDERR_FILENO) < 0) {
        ::close(old_stderr);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return {};
    }
    ::close(pipefd[1]);
    fn();
    ::fflush(stderr);
    ::dup2(old_stderr, STDERR_FILENO);
    ::close(old_stderr);
    std::string buf;
    buf.resize(8192);
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

bool returns_false(CompilerService& cs, const char* expr) {
    auto v = cs.eval(expr);
    return v && is_bool(*v) && !as_bool(*v);
}

} // namespace

int main() {
    CompilerService cs;

    static constexpr const char* kDeprecated[] = {
        "(terminal:clear)",         "(terminal:draw-batch)",
        "(terminal:present)",       "(terminal:mark-dirty-region)",
        "(terminal:present-delta)", "(terminal:create-buffer)",
        "(terminal:diff)",
    };

    // #1351 AC1 + AC2: first call → #f and one-shot stderr WARN (capture before any call)
    std::string first_stderr;
    {
        first_stderr = capture_stderr([&] {
            for (const char* expr : kDeprecated) {
                auto v = cs.eval(expr);
                CHECK(v && is_bool(*v) && !as_bool(*v), std::format("{} first call → #f", expr));
            }
        });
        CHECK(!first_stderr.empty(), "first calls emit stderr warnings");
        CHECK(contains(first_stderr, "deprecated") || contains(first_stderr, "WARN"),
              "stderr contains deprecation warning");
        CHECK(contains(first_stderr, "terminal:clear"), "warn mentions terminal:clear");
        CHECK(contains(first_stderr, "terminal:create-buffer") ||
                  contains(first_stderr, "make-terminal-buffer"),
              "warn mentions create-buffer / replacement");
        CHECK(contains(first_stderr, "#1351"), "warn cites #1351");
        // Expect one warn line per primitive (7)
        std::size_t warn_lines = 0;
        for (std::size_t i = 0; i < first_stderr.size();) {
            auto pos = first_stderr.find("[aura] WARN:", i);
            if (pos == std::string::npos)
                break;
            ++warn_lines;
            i = pos + 1;
        }
        CHECK(warn_lines >= 7, "one-shot warn for each of 7 primitives");
    }

    // Second call: still #f, no additional warns
    {
        auto second_stderr = capture_stderr([&] {
            for (const char* expr : kDeprecated) {
                CHECK(returns_false(cs, expr), std::format("{} second call still #f", expr));
            }
        });
        CHECK(!contains(second_stderr, "[aura] WARN:"), "no repeated warn on second call");
    }

    // #1351 AC3: metrics counters still increment
    {
        (void)cs.eval("(terminal:clear)");
        (void)cs.eval("(terminal:present)");
        auto ct = cs.eval("(hash-ref (query:terminal-render-production-stats) 'clear-total)");
        auto pt = cs.eval("(hash-ref (query:terminal-render-production-stats) 'present-total)");
        CHECK(ct && is_int(*ct) && as_int(*ct) >= 1, "clear-total still increments");
        CHECK(pt && is_int(*pt) && as_int(*pt) >= 1, "present-total still increments");
    }

    // Real replacements remain available
    {
        auto id = cs.eval("(make-terminal-buffer 2 1)");
        CHECK(id && is_int(*id) && as_int(*id) >= 0, "make-terminal-buffer works");
        auto d = cs.eval(std::format("(terminal-diff-update {} {})", as_int(*id), as_int(*id)));
        CHECK(d && is_int(*d) && as_int(*d) == 0, "terminal-diff-update works");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("terminal deprecation #1351: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
