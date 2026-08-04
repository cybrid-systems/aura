// @category: unit
// @reason: Issue #2576 — JIT PrimCall must forward N args (not only 2).
//
//   AC1: string-append 3 strings → ABC under default JIT
//   AC2: substring 3-arg → "ell"
//   AC3: string-append number->string parts → 1-2
//   AC4: 4-way string-append → ABCD
//   AC5: FORCE_IR / top-level still OK
//   AC6: source + cmake + gate

#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <print>
#include <string>
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
    auto pos = s.rfind('\n');
    if (pos != std::string::npos && s.substr(pos + 1) == "#t")
        s = s.substr(0, pos);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

static void ac1_append3() {
    std::println("\n--- #2576 AC1: string-append 3 ---");
    CompilerService cs;
    auto out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (display (string-append "A" "B" "C"))
    (newline)
    #t)
  (f)))");
    });
    CHECK(trim_nl(out) == "ABC", "AC1: ABC not AB0");
    if (trim_nl(out) != "ABC")
        std::println(stderr, "  got '{}'", out);
}

static void ac2_substring() {
    std::println("\n--- #2576 AC2: substring 3-arg ---");
    CompilerService cs;
    auto out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (write (substring "hello" 1 4))
    (newline)
    #t)
  (f)))");
    });
    CHECK(trim_nl(out) == "\"ell\"", "AC2: \"ell\"");
    if (trim_nl(out) != "\"ell\"")
        std::println(stderr, "  got '{}'", out);
}

static void ac3_nested_num() {
    std::println("\n--- #2576 AC3: append number->string parts ---");
    CompilerService cs;
    auto out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (display (string-append (number->string 1) "-" (number->string 2)))
    (newline)
    #t)
  (f)))");
    });
    CHECK(trim_nl(out) == "1-2", "AC3: 1-2 not 1-0");
    if (trim_nl(out) != "1-2")
        std::println(stderr, "  got '{}'", out);
}

static void ac4_append4() {
    std::println("\n--- #2576 AC4: string-append 4 ---");
    CompilerService cs;
    auto out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (display (string-append "A" "B" "C" "D"))
    (newline)
    #t)
  (f)))");
    });
    CHECK(trim_nl(out) == "ABCD", "AC4: ABCD");
    if (trim_nl(out) != "ABCD")
        std::println(stderr, "  got '{}'", out);
}

static void ac5_controls() {
    std::println("\n--- #2576 AC5: FORCE_IR + top-level ---");
    setenv("AURA_FORCE_IR", "1", 1);
    CompilerService cs_ir;
    auto ir = with_stdout_capture([&] {
        (void)cs_ir.eval(R"((begin
  (define (f) (display (string-append "A" "B" "C")) (newline) #t)
  (f)))");
    });
    unsetenv("AURA_FORCE_IR");
    CHECK(trim_nl(ir).find("ABC") != std::string::npos, "AC5: FORCE_IR ABC");

    CompilerService cs_top;
    auto top = with_stdout_capture(
        [&] { (void)cs_top.eval(R"((begin (display (string-append "A" "B" "C")) (newline)))"); });
    CHECK(trim_nl(top).find("ABC") != std::string::npos, "AC5: top-level ABC");
}

static void ac6_source_gate() {
    std::println("\n--- #2576 AC6: source + gate ---");
    const auto jit = read_file("src/compiler/aura_jit.cpp");
    CHECK(jit.find("#2576") != std::string::npos, "AC6: jit cites #2576");
    CHECK(jit.find("kMax") != std::string::npos || jit.find("pack") != std::string::npos,
          "AC6: pack N args");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(rt.find("#2576") != std::string::npos, "AC6: runtime cites #2576");
    CHECK(rt.find("int64_t* args") != std::string::npos ||
              rt.find("int64_t *args") != std::string::npos,
          "AC6: pointer ABI");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_primcall_narg_2576") != std::string::npos, "AC6: cmake");
    const auto build = read_file("build.py");
    CHECK(build.find("check_primcall_narg_2576") != std::string::npos, "AC6: check");
    CHECK(build.find("cmd_primcall_narg_coverage") != std::string::npos, "AC6: gate cmd");
}

} // namespace

int run_test_primcall_narg_2576() {
    std::println("=== Issue #2576: PrimCall N-arg ===");
    ac1_append3();
    ac2_substring();
    ac3_nested_num();
    ac4_append4();
    ac5_controls();
    ac6_source_gate();
    std::println("\n=== #2576: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_primcall_narg_2576();
}
#endif
