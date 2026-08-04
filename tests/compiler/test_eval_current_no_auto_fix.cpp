// @category: unit
// @reason: Issue #2484 — eval-current must not auto-invoke workspace
//          Defines when the result is a closure (side-effect / DoS).
//
//   AC1: last form lambda → closure returned unchanged
//   AC2: arity-0 define → still a closure (not auto-called body result)
//   AC3: source has #2484 + no auto-fix machinery
//   AC4: gate wiring

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_closure;
using aura::compiler::types::is_int;
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

// AC1: set-code with a lambda last form; eval-current returns the closure.
static void ac1_closure_unchanged() {
    std::println("\n--- #2484 AC1: eval-current returns closure ---");
    CompilerService cs;
    auto r = cs.eval("(begin (set-code \"(define (double x) (* x 2))\\n(lambda (y) (+ y 1))\") "
                     "(eval-current))");
    CHECK(r.has_value(), "AC1: eval ok");
    CHECK(r && is_closure(*r), "AC1: result is closure (not auto-called int)");

    auto r3 =
        cs.eval("(begin (set-code \"(define (double x) (* x 2))\") (eval-current) (double 21))");
    CHECK(r3.has_value() && is_int(*r3) && as_int(*r3) == 42, "AC1: explicit (double 21) → 42");
}

// AC2: (define (f) 99) as workspace — old auto-fix arity-0 would call (f)
// and return 99. New path returns the define/closure result without invoking.
static void ac2_no_auto_call_arity0() {
    std::println("\n--- #2484 AC2: arity-0 define not auto-called ---");
    CompilerService cs;
    // Workspace whose last form is a define of nullary fn with body 99.
    // eval-current must NOT return 99 (that would mean auto-call ran).
    auto r = cs.eval("(begin (set-code \"(define (probe-auto-fix-2484) 99)\") (eval-current))");
    CHECK(r.has_value(), "AC2: eval ok");
    // Accept either void (define returns void) or closure — never 99.
    if (r && is_int(*r)) {
        CHECK(as_int(*r) != 99, "AC2: must not return auto-call body 99");
    } else {
        CHECK(r.has_value(), "AC2: got non-int (closure/void) without auto-call");
        if (r)
            CHECK(!is_int(*r) || as_int(*r) != 99, "AC2: not body result");
    }
    // Explicit call still yields 99
    auto call = cs.eval("(begin (set-code \"(define (probe-auto-fix-2484) 99)\") "
                        "(eval-current) (probe-auto-fix-2484))");
    CHECK(call.has_value() && is_int(*call) && as_int(*call) == 99,
          "AC2: explicit (probe-auto-fix-2484) → 99");
}

static void ac3_source() {
    std::println("\n--- #2484 AC3: source contracts ---");
    auto src = read_file("src/compiler/evaluator_primitives_eval.cpp");
    CHECK(!src.empty(), "AC3: read eval primitives");
    CHECK(src.find("Issue #2484") != std::string::npos, "AC3: cites #2484");
    CHECK(src.find("auto-fix-on-closure REMOVED") != std::string::npos ||
              src.find("auto-fix") != std::string::npos,
          "AC3: documents auto-fix removal");
    CHECK(src.find("winning_call") == std::string::npos, "AC3: no winning_call");
    CHECK(src.find("auto_fixed") == std::string::npos, "AC3: no auto_fixed");
    CHECK(src.find("arg_pats") == std::string::npos, "AC3: no arg_pats");
    CHECK(src.find("list 3 1 4 1 5") == std::string::npos, "AC3: no hardcoded list probe");
}

static void ac4_gate() {
    std::println("\n--- #2484 AC4: gate wiring ---");
    auto build = read_file("build.py");
    auto cmake = read_file("CMakeLists.txt");
    CHECK(build.find("check_eval_current_no_auto_fix_2484") != std::string::npos,
          "AC4: check script in build.py");
    CHECK(build.find("cmd_eval_current_no_auto_fix_coverage") != std::string::npos,
          "AC4: coverage cmd");
    CHECK(cmake.find("test_eval_current_no_auto_fix") != std::string::npos, "AC4: cmake test");
    CHECK(!read_file("scripts/coverage/checks/check_eval_current_no_auto_fix_2484.py").empty(),
          "AC4: check script exists");
}

} // namespace

int run_test_eval_current_no_auto_fix() {
    std::println("=== Issue #2484: eval-current no auto-fix ===");
    ac1_closure_unchanged();
    ac2_no_auto_call_arity0();
    ac3_source();
    ac4_gate();
    std::println("\n=== #2484 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_eval_current_no_auto_fix();
}
#endif
