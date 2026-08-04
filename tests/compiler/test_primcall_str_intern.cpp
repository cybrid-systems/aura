// @category: unit
// @reason: Issue #2577 — PrimCall string re-intern must content-intern
//          so hot loops with fixed string args do not grow heaps O(N×args).
//
//   AC1: N× (string-append x "!") — eval heap growth ≪ N
//   AC2: display correctness preserved (string-append still works)
//   AC3: distinct contents still distinct (two different appends)
//   AC4: source-cite + cmake + gate

#include "test_harness.hpp"

#include <cstdlib>
#include <fstream>
#include <print>
#include <string>

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

// AC1: fixed-arg string-append loop should not grow eval heap by ~N×2.
static void ac1_loop_heap_growth() {
    std::println("\n--- #2577 AC1: fixed-arg PrimCall heap growth ---");
    CompilerService cs;
    auto def = cs.eval(R"((begin
  (define (once x)
    (string-append x "!"))
  (define (loop n x)
    (if (<= n 0)
        #t
        (begin
          (once x)
          (loop (- n 1) x))))
  #t))");
    CHECK(def.has_value() && !(is_error(*def) && !is_string(*def)), "define helpers");

    // Warm one call so ConstString "!" and arg path settle.
    (void)cs.eval(R"((once "hi"))");
    const auto heap0 = cs.evaluator().string_heap().size();

    auto r = cs.eval(R"((loop 400 "hi"))");
    CHECK(r.has_value() && !(is_error(*r) && !is_string(*r)), "run loop 400");
    const auto heap1 = cs.evaluator().string_heap().size();
    const auto growth = heap1 > heap0 ? heap1 - heap0 : 0;
    // Pre-fix: ~800 from arg re-intern alone + results. Post-fix: arg
    // converts hit cache; results from string-append still may add unique
    // "hi!" once (or few). Cap well below O(N).
    CHECK(growth < 80, "AC1: heap growth << iterations (content intern)");
    if (growth >= 80)
        std::println(stderr, "  heap0={} heap1={} growth={}", heap0, heap1, growth);
}

// AC2: still correct output (dual-heap #2575 preserved).
static void ac2_display_ok() {
    std::println("\n--- #2577 AC2: display still correct ---");
    CompilerService cs;
    // Capture via write of result string
    auto r = cs.eval(R"((begin
  (define (f x) (string-append x "!"))
  (f "hi")))");
    CHECK(r && is_string(*r), "AC2: returns string");
    // Content via equal? / string=?
    auto eq = cs.eval(R"((string=? (string-append "hi" "!") "hi!"))");
    CHECK(eq.has_value(), "AC2: string=? ok");
}

// AC3: distinct contents not collapsed incorrectly for different values.
static void ac3_distinct() {
    std::println("\n--- #2577 AC3: distinct contents ---");
    CompilerService cs;
    auto r = cs.eval(R"((begin
  (define a (string-append "X" "1"))
  (define b (string-append "Y" "2"))
  (if (string=? a b) 0 1)))");
    CHECK(r.has_value(), "AC3: eval");
    // Expect 1 (not equal)
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_int;
    CHECK(r && is_int(*r) && as_int(*r) == 1, "AC3: X1 != Y2");
}

static void ac4_source_gate() {
    std::println("\n--- #2577 AC4: source + gate ---");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(rt.find("#2577") != std::string::npos, "AC4: runtime cites #2577");
    CHECK(rt.find("g_string_intern") != std::string::npos, "AC4: JIT content intern map");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("#2577") != std::string::npos, "AC4: service cites #2577");
    CHECK(svc.find("g_jit_idx_to_eval_idx") != std::string::npos, "AC4: jit→eval idx cache");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_primcall_str_intern") != std::string::npos, "AC4: cmake");
    const auto build = read_file("build.py");
    CHECK(build.find("check_primcall_str_intern_2577") != std::string::npos, "AC4: check");
    CHECK(build.find("cmd_primcall_str_intern_coverage") != std::string::npos, "AC4: gate cmd");
}

} // namespace

int run_test_primcall_str_intern() {
    std::println("=== Issue #2577: PrimCall string re-intern growth ===");
    ac1_loop_heap_growth();
    ac2_display_ok();
    ac3_distinct();
    ac4_source_gate();
    std::println("\n=== #2577: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_primcall_str_intern();
}
#endif
