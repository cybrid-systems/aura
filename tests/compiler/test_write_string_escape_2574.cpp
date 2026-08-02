// @category: unit
// @reason: Issue #2574 — Scheme write must escape string contents
//          (quotes, backslash, common controls); display stays raw.
//
//   AC1: (write "a\"b") → "a\"b" under default JIT path
//   AC2: (write "a\\b") → "a\\b"
//   AC3: (display "a\"b") → a"b (raw, no escapes)
//   AC4: top-level TW write matches JIT define body
//   AC5: source-cite + cmake + gate

#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::is_error;
using aura::compiler::types::is_string;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

template <typename F> static std::string with_stdout_capture(F&& fn) {
    int pipefd[2];
    if (::pipe(pipefd) != 0)
        return {};
    int saved = ::dup(STDOUT_FILENO);
    if (saved < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return {};
    }
    ::fflush(stdout);
    if (::dup2(pipefd[1], STDOUT_FILENO) < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        ::close(saved);
        return {};
    }
    ::close(pipefd[1]);

    fn();

    ::fflush(stdout);
    ::dup2(saved, STDOUT_FILENO);
    ::close(saved);

    std::string out;
    char buf[512];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    ::close(pipefd[0]);
    return out;
}

// Strip trailing newline for comparisons.
static std::string trim_nl(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

static void ac1_write_embedded_quote() {
    std::println("\n--- #2574 AC1: write embedded quote (JIT define) ---");
    CompilerService cs;
    std::string out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (write "a\"b")
    (newline)
    #t)
  (f)))");
    });
    // Expected external form: "a\"b"
    const bool ok = trim_nl(out) == "\"a\\\"b\"";
    CHECK(ok, "AC1: write \"a\\\"b\" → \"a\\\"b\"");
    if (!ok)
        std::println(stderr, "  captured: '{}'", out);
}

static void ac2_write_backslash() {
    std::println("\n--- #2574 AC2: write backslash ---");
    CompilerService cs;
    std::string out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (write "a\\b")
    (newline)
    #t)
  (f)))");
    });
    // Content a\b → external "a\\b"
    const bool ok = trim_nl(out) == "\"a\\\\b\"";
    CHECK(ok, "AC2: write \"a\\\\b\" → \"a\\\\b\"");
    if (!ok)
        std::println(stderr, "  captured: '{}'", out);
}

static void ac3_display_raw() {
    std::println("\n--- #2574 AC3: display stays raw ---");
    CompilerService cs;
    std::string out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (display "a\"b")
    (newline)
    #t)
  (f)))");
    });
    CHECK(trim_nl(out) == "a\"b", "AC3: display prints raw a\"b");
    if (trim_nl(out) != "a\"b")
        std::println(stderr, "  captured: '{}'", out);
}

static void ac4_tw_matches_jit() {
    std::println("\n--- #2574 AC4: top-level TW write matches define/JIT ---");
    CompilerService cs_tw;
    std::string tw =
        with_stdout_capture([&] { (void)cs_tw.eval(R"((begin (write "a\"b") (newline)))"); });
    CompilerService cs_jit;
    std::string jit = with_stdout_capture([&] {
        (void)cs_jit.eval(R"((begin
  (define (f) (write "a\"b") (newline) #t)
  (f)))");
    });
    CHECK(trim_nl(tw) == trim_nl(jit), "AC4: TW and JIT write agree");
    CHECK(trim_nl(tw) == "\"a\\\"b\"", "AC4: shared form is escaped");
    if (trim_nl(tw) != trim_nl(jit))
        std::println(stderr, "  tw='{}' jit='{}'", tw, jit);
}

static void ac5_source_gate() {
    std::println("\n--- #2574 AC5: source-cite + gate ---");
    const auto jit = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(jit.find("#2574") != std::string::npos, "AC5: jit runtime cites #2574");
    CHECK(jit.find("fputs_scheme_write_string") != std::string::npos, "AC5: escape helper");
    const auto tw = read_file("src/compiler/evaluator_primitives_runtime.cpp");
    CHECK(tw.find("#2574") != std::string::npos, "AC5: TW io_print_val cites #2574");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_write_string_escape_2574") != std::string::npos, "AC5: cmake");
    const auto build = read_file("build.py");
    CHECK(build.find("check_write_string_escape_2574") != std::string::npos, "AC5: check script");
    CHECK(build.find("cmd_write_string_escape_coverage") != std::string::npos, "AC5: gate cmd");
}

} // namespace

int main() {
    std::println("=== Issue #2574: Scheme write string escape ===");
    ac1_write_embedded_quote();
    ac2_write_backslash();
    ac3_display_raw();
    ac4_tw_matches_jit();
    ac5_source_gate();
    std::println("\n=== #2574: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
