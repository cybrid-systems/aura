// @category: unit
// @reason: Issue #2572 — exported module multi-(display …) must not garble
//          string literals when IR-cached and called from a fresh module.
//
//   AC1: issue repro — (require) + multi-display export prints prefix + arg
//   AC2: body-only multi ConstString (no arg) prints both literals
//   AC3: control — same body defined in main prints correctly
//   AC4: apply path and let-bound arg still correct
//   AC5: source-cite — cache_module persists ir_cache_strings_ + gate

#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

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

static bool write_mod(const std::string& dir, const std::string& name, const std::string& body) {
    std::filesystem::create_directories(dir);
    std::ofstream out(dir + "/" + name);
    if (!out)
        return false;
    out << body;
    return true;
}

// Capture stdout produced by `fn` (display goes to stdout).
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

// Issue repro: exported multi-display helper after require.
static void ac1_exported_multi_display() {
    std::println("\n--- #2572 AC1: exported multi-display after require ---");
    const std::string dir = "/tmp/aura_test_mod2572_ac1";
    CHECK(write_mod(dir, "badlog.aura",
                    R"((export badlog)
(define (badlog msg)
  (display "[flux] ")
  (display msg)
  (newline)
  #t)
)"),
          "write badlog.aura");
    setenv("AURA_PATH", dir.c_str(), 1);
    CompilerService cs;
    std::string out = with_stdout_capture([&] {
        auto req = cs.eval("(require \"badlog\" all:)");
        if (!req || (is_error(*req) && !is_string(*req)))
            return;
        (void)cs.eval("(badlog \"hello-once\")");
    });
    // Expected: "[flux] hello-once\n" — not "hello-oncehello-once\n"
    const bool ok = out.find("[flux] hello-once") != std::string::npos &&
                    out.find("hello-oncehello-once") == std::string::npos;
    CHECK(ok, "AC1: [flux] hello-once (not garbled msgmsg)");
    if (!ok)
        std::println(stderr, "  captured stdout: '{}'", out);
    unsetenv("AURA_PATH");
}

// Body-only multi ConstString (no parameter) — was blank when pool missing.
static void ac2_body_only_literals() {
    std::println("\n--- #2572 AC2: body-only multi ConstString ---");
    const std::string dir = "/tmp/aura_test_mod2572_ac2";
    CHECK(write_mod(dir, "lit.aura",
                    R"((export litlog)
(define (litlog)
  (display "AAA")
  (display "BBB")
  (newline)
  #t)
)"),
          "write lit.aura");
    setenv("AURA_PATH", dir.c_str(), 1);
    CompilerService cs;
    std::string out = with_stdout_capture([&] {
        auto req = cs.eval("(require \"lit\" all:)");
        if (!req || (is_error(*req) && !is_string(*req)))
            return;
        (void)cs.eval("(litlog)");
    });
    CHECK(out.find("AAABBB") != std::string::npos, "AC2: AAABBB from body ConstStrings");
    if (out.find("AAABBB") == std::string::npos)
        std::println(stderr, "  captured stdout: '{}'", out);
    unsetenv("AURA_PATH");
}

// Control: same body in main (not via require) — always worked.
static void ac3_local_control() {
    std::println("\n--- #2572 AC3: local multi-display control ---");
    CompilerService cs;
    std::string out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (mylog msg)
    (display "[flux] ")
    (display msg)
    (newline)
    #t)
  (mylog "hello-once")))");
    });
    CHECK(out.find("[flux] hello-once") != std::string::npos, "AC3: local mylog correct");
}

// apply + let-bound arg paths (TW-friendly) still correct after fix.
static void ac4_apply_and_let() {
    std::println("\n--- #2572 AC4: apply + let-arg ---");
    const std::string dir = "/tmp/aura_test_mod2572_ac4";
    CHECK(write_mod(dir, "badlog.aura",
                    R"((export badlog)
(define (badlog msg)
  (display "[flux] ")
  (display msg)
  (newline)
  #t)
)"),
          "write badlog.aura");
    setenv("AURA_PATH", dir.c_str(), 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    CompilerService cs;
    std::string out = with_stdout_capture([&] {
        auto req = cs.eval("(require \"badlog\" all:)");
        if (!req || (is_error(*req) && !is_string(*req)))
            return;
        (void)cs.eval("(apply badlog (list \"via-apply\"))");
        (void)cs.eval("(let ((x \"via-let\")) (badlog x))");
    });
    CHECK(out.find("[flux] via-apply") != std::string::npos, "AC4: apply path");
    CHECK(out.find("[flux] via-let") != std::string::npos, "AC4: let-arg path");
    unsetenv("AURA_PIPELINE_STRICT");
    unsetenv("AURA_PATH");
}

static void ac5_source_gate() {
    std::println("\n--- #2572 AC5: source-cite + gate ---");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("#2572") != std::string::npos, "AC5: service cites #2572");
    CHECK(svc.find("ir_cache_strings_[name] = ir_mod.string_pool") != std::string::npos ||
              svc.find("ir_cache_strings_[name] = ir_mod.string_pool;") != std::string::npos,
          "AC5: cache_module persists string pool");
    const auto jit_rt = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(jit_rt.find("aura_display_value") != std::string::npos,
          "AC5: JIT tagged display runtime");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_module_export_display_2572") != std::string::npos, "AC5: cmake");
    const auto build = read_file("build.py");
    CHECK(build.find("check_module_export_display_2572") != std::string::npos, "AC5: check script");
    CHECK(build.find("cmd_module_export_display_coverage") != std::string::npos, "AC5: gate cmd");
}

} // namespace

int main() {
    std::println("=== Issue #2572: module export multi-display / ConstString pool ===");
    ac1_exported_multi_display();
    ac2_body_only_literals();
    ac3_local_control();
    ac4_apply_and_let();
    ac5_source_gate();
    std::println("\n=== #2572: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
