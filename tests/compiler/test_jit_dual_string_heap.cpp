// @category: unit
// @reason: Issue #2575 — PrimCall string results re-interned into
//          g_string_pool so JIT PrimDisplay sees correct content
//          (string-append / number->string no longer garble).
//
//   AC1: (display (string-append "A" "B")) → AB under default JIT
//   AC2: (display (number->string 42)) → 42 under default JIT
//   AC3: (write (string-append "A" "B")) → "AB" under default JIT
//   AC4: FORCE_IR and top-level controls still correct
//   AC5: (display (string-append x "!")) with arg still works
//   AC6: source-cite + cmake + gate

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

static std::string trim_nl(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    // drop trailing #t lines from define-body returns in some paths
    auto pos = s.rfind('\n');
    if (pos != std::string::npos && s.substr(pos + 1) == "#t")
        s = s.substr(0, pos);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

static void ac1_string_append_display() {
    std::println("\n--- #2575 AC1: string-append + display (JIT) ---");
    CompilerService cs;
    std::string out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (display (string-append "A" "B"))
    (newline)
    #t)
  (f)))");
    });
    const auto t = trim_nl(out);
    CHECK(t == "AB" || t.find("AB") != std::string::npos, "AC1: prints AB not A");
    CHECK(t.find("ABAB") == std::string::npos, "AC1: not duplicated");
    if (t != "AB" && t.find("AB") == std::string::npos)
        std::println(stderr, "  captured: '{}'", out);
}

static void ac2_number_to_string() {
    std::println("\n--- #2575 AC2: number->string + display (JIT) ---");
    CompilerService cs;
    std::string out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (display (number->string 42))
    (newline)
    #t)
  (f)))");
    });
    const auto t = trim_nl(out);
    CHECK(t == "42" || t.find("42") != std::string::npos, "AC2: prints 42");
    if (t != "42" && t.find("42") == std::string::npos)
        std::println(stderr, "  captured: '{}'", out);
}

static void ac3_write_string_append() {
    std::println("\n--- #2575 AC3: write (string-append …) ---");
    CompilerService cs;
    std::string out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (write (string-append "A" "B"))
    (newline)
    #t)
  (f)))");
    });
    const auto t = trim_nl(out);
    CHECK(t == "\"AB\"", "AC3: write prints \"AB\"");
    if (t != "\"AB\"")
        std::println(stderr, "  captured: '{}'", out);
}

static void ac4_controls() {
    std::println("\n--- #2575 AC4: FORCE_IR + top-level controls ---");
    setenv("AURA_FORCE_IR", "1", 1);
    CompilerService cs_ir;
    std::string ir = with_stdout_capture([&] {
        (void)cs_ir.eval(R"((begin
  (define (f)
    (display (string-append "A" "B"))
    (newline)
    #t)
  (f)))");
    });
    unsetenv("AURA_FORCE_IR");
    CHECK(trim_nl(ir).find("AB") != std::string::npos, "AC4: FORCE_IR still AB");

    CompilerService cs_top;
    std::string top = with_stdout_capture(
        [&] { (void)cs_top.eval(R"((begin (display (string-append "A" "B")) (newline)))"); });
    CHECK(trim_nl(top).find("AB") != std::string::npos, "AC4: top-level still AB");
}

static void ac5_arg_append() {
    std::println("\n--- #2575 AC5: string-append with arg ---");
    CompilerService cs;
    std::string out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f x)
    (display (string-append x "!"))
    (newline)
    #t)
  (f "hi")))");
    });
    CHECK(trim_nl(out).find("hi!") != std::string::npos, "AC5: hi!");
    if (trim_nl(out).find("hi!") == std::string::npos)
        std::println(stderr, "  captured: '{}'", out);
}

static void ac6_source_gate() {
    std::println("\n--- #2575 AC6: source-cite + gate ---");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("#2575") != std::string::npos, "AC6: service cites #2575");
    CHECK(svc.find("convert_str_for_jit") != std::string::npos, "AC6: eval→JIT re-intern");
    CHECK(svc.find("convert_str_for_eval") != std::string::npos, "AC6: JIT→eval args");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_jit_dual_string_heap") != std::string::npos, "AC6: cmake");
    const auto build = read_file("build.py");
    CHECK(build.find("check_jit_dual_string_heap_2575") != std::string::npos, "AC6: check");
    CHECK(build.find("cmd_jit_dual_string_heap_coverage") != std::string::npos, "AC6: gate cmd");
}

} // namespace

int run_test_jit_dual_string_heap() {
    std::println("=== Issue #2575: dual string heap PrimCall re-intern ===");
    ac1_string_append_display();
    ac2_number_to_string();
    ac3_write_string_append();
    ac4_controls();
    ac5_arg_append();
    ac6_source_gate();
    std::println("\n=== #2575: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_jit_dual_string_heap();
}
#endif
