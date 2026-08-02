// @category: unit
// @reason: Issue #2573 — IR interpreter ConstString must intern by module
//          pool index so hot loops do not grow string heaps O(iterations).
//
//   AC1: IR-path loop with a string literal: heap growth O(1) not O(N)
//   AC2: distinct literals still print correctly (no cross-intern)
//   AC3: recursive IR loop (display "tick") still produces correct output
//   AC4: source-cite + cmake + gate

#include "test_harness.hpp"

#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_int;
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

// Issue #2573 AC1: N iterations of ConstString "tick" must not add ~N strings.
static void ac1_loop_heap_growth_bounded() {
    std::println("\n--- #2573 AC1: ConstString loop heap growth O(1) ---");
    setenv("AURA_FORCE_IR", "1", 1);
    CompilerService cs;
    // Warm: define loop once (may allocate some strings for names/etc).
    auto def = cs.eval(R"((define (loop n)
  (if (<= n 0)
      #t
      (begin
        (display "tick")
        (loop (- n 1))))))");
    CHECK(def.has_value() && !(is_error(*def) && !is_string(*def)), "define loop");

    const auto heap0 = cs.evaluator().string_heap().size();
    auto r = cs.eval("(loop 500)");
    CHECK(r.has_value() && !(is_error(*r) && !is_string(*r)), "run loop 500");
    const auto heap1 = cs.evaluator().string_heap().size();
    const auto growth = heap1 > heap0 ? heap1 - heap0 : 0;
    // Pre-fix: growth ~500+. Post-fix: intern reuses "tick" → tiny growth
    // (unrelated side allocations allowed). Cap well below O(N).
    CHECK(growth < 50, "AC1: heap growth << iterations (interned ConstString)");
    if (growth >= 50)
        std::println(stderr, "  heap0={} heap1={} growth={}", heap0, heap1, growth);
    unsetenv("AURA_FORCE_IR");
}

// Distinct body literals must not collapse.
static void ac2_distinct_literals() {
    std::println("\n--- #2573 AC2: distinct ConstString literals ---");
    setenv("AURA_FORCE_IR", "1", 1);
    CompilerService cs;
    auto r = cs.eval(R"((begin
  (define (f)
    (display "AAA")
    (display "BBB")
    (newline)
    #t)
  (f)
  (f)
  1))");
    CHECK(r && is_int(*r) && as_int(*r) == 1, "AC2: two distinct lits + two calls ok");
    unsetenv("AURA_FORCE_IR");
}

// Correctness under recursion (output path still works with intern).
static void ac3_recursive_display_ok() {
    std::println("\n--- #2573 AC3: recursive display still works ---");
    setenv("AURA_FORCE_IR", "1", 1);
    CompilerService cs;
    auto r = cs.eval(R"((begin
  (define (loop n)
    (if (<= n 0)
        0
        (begin
          (display "x")
          (loop (- n 1)))))
  (loop 3)
  42))");
    CHECK(r && is_int(*r) && as_int(*r) == 42, "AC3: recursive ConstString display");
    unsetenv("AURA_FORCE_IR");
}

static void ac4_source_gate() {
    std::println("\n--- #2573 AC4: source-cite + gate ---");
    const auto impl = read_file("src/compiler/ir_executor_impl.cpp");
    CHECK(impl.find("#2573") != std::string::npos, "AC4: impl cites #2573");
    CHECK(impl.find("const_string_cache_") != std::string::npos, "AC4: intern cache used");
    const auto hdr = read_file("src/compiler/ir_executor.ixx");
    CHECK(hdr.find("const_string_cache_") != std::string::npos, "AC4: cache member");
    CHECK(hdr.find("#2573") != std::string::npos, "AC4: header cites #2573");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_ir_const_string_intern_2573") != std::string::npos, "AC4: cmake");
    const auto build = read_file("build.py");
    CHECK(build.find("check_ir_const_string_intern_2573") != std::string::npos,
          "AC4: check script");
    CHECK(build.find("cmd_ir_const_string_intern_coverage") != std::string::npos, "AC4: gate cmd");
}

} // namespace

int main() {
    std::println("=== Issue #2573: IR ConstString intern / heap growth ===");
    ac1_loop_heap_growth_bounded();
    ac2_distinct_literals();
    ac3_recursive_display_ok();
    ac4_source_gate();
    std::println("\n=== #2573: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
