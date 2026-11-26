// @category: unit
// @reason: IR compile-time unbound globals stayed ConstI64 0. A program that
//          binds the name with a runtime require inside a function (the live
//          game loads lib/coins in load-coins!) then died with "cannot call:
//          coin-blank". The IR now lowers an unresolved global to top-ref and
//          lowers a require inside a function body to the import primitive.
#include "test_harness.hpp"

#include <cstdlib>
#include <print>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_float;
using aura::compiler::types::is_float;
using aura::test::g_failed;
using aura::test::g_passed;

static void ac1_runtime_require_binds_late_global() {
    // The function body requires std/math at run time; sqrt is unbound when
    // the form is compiled.
    CompilerService cs;
    auto r = cs.eval_ir(R"(
        (define (f) (begin (require "std/math") (sqrt 9.0)))
        (f))");
    CHECK(r && is_float(*r) && as_float(*r) == 3.0,
          "AC1: a runtime require binds a compile-time unbound global");
}

static void ac2_plain_global_still_resolves() {
    CompilerService cs;
    auto r = cs.eval_ir(R"((define x 41) (+ x 1))");
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 42,
          "AC2: a bound global still lowers normally");
}

} // namespace

int run_test_ir_runtime_require() {
    // The require path needs the dev capability set in the test process.
    ::setenv("AURA_SANDBOX", "off", 1);
    std::println("=== IR runtime require + late-bound global ===");
    ac1_runtime_require_binds_late_global();
    ac2_plain_global_still_resolves();
    std::println("\n=== IR runtime require: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_ir_runtime_require();
}
#endif
