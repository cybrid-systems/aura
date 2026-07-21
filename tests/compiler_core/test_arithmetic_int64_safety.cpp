// test_arithmetic_int64_safety.cpp — Issues #1150–#1156 Phase 1

#include "test_harness.hpp"

#include <cstdint>
#include <limits>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_float;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_float;
using aura::compiler::types::is_int;
using aura::compiler::types::is_void;

int main() {
    CompilerService cs;

    {
        auto r = cs.eval("(+ 1 2 3)");
        CHECK(r && is_int(*r) && as_int(*r) == 6, "small +");
        auto r2 = cs.eval("(* 2 3 4)");
        CHECK(r2 && is_int(*r2) && as_int(*r2) == 24, "small *");
        auto r3 = cs.eval("(- 10 3)");
        CHECK(r3 && is_int(*r3) && as_int(*r3) == 7, "small -");
        auto r4 = cs.eval("(/ 100 5)");
        CHECK(r4 && is_int(*r4) && as_int(*r4) == 20, "small /");
    }

    // #1151: (/ 0) must not UB — eval fails or returns error/void safely
    {
        auto r = cs.eval("(/ 0)");
        // DivisionByZero surfaces as unexpected Diagnostic → empty optional
        CHECK(!r || is_error(*r) || is_void(*r), "(/ 0) is safe (error/void/nullopt)");
    }

    // #1150/#1152: large mul should not crash — may float-promote
    {
        auto r = cs.eval("(* 1000000 1000000 1000000 1000000)");
        CHECK(r, "large * returns");
        CHECK(is_int(*r) || is_float(*r), "large * is int or float");
    }

    // #1150: large add
    {
        auto r = cs.eval("(+ 9000000000000000000 9000000000000000000)");
        CHECK(r, "large + returns");
        CHECK(is_int(*r) || is_float(*r), "large + is int or float");
    }

    {
        auto a = cs.eval("(+ 40 2)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 40 2)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("arithmetic int64 safety: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
