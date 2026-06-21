// @category: integration
// @reason: uses CompilerService to verify IR-native define env binding

// test_issue_272.cpp — Issue #272 Cycle 1: top-level (define (f x) ...)
// binds env via IRInterpreter instead of eval_flat tree-walker fallback.

#include <iostream>
#include <print>
#include <string>

#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_272_detail {

static bool run_ok(aura::compiler::CompilerService& cs, std::string_view src) {
    return static_cast<bool>(cs.eval(src));
}

static int64_t run_int(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

bool test_define_ir_env_bind_metric() {
    std::println("\n--- AC1: define_ir_env_bind_count bumps on function define ---");
    aura::compiler::CompilerService cs;
    CHECK(cs.define_ir_env_bind_count() == 0, "metric starts at 0");
    CHECK(run_ok(cs, "(define (dbl x) (* x 2))"), "define succeeds");
    CHECK(cs.define_ir_env_bind_count() == 1, "one IR env bind after define");
    return true;
}

bool test_define_then_call() {
    std::println("\n--- AC2: defined function callable via IR closure bridge ---");
    aura::compiler::CompilerService cs;
    if (!run_ok(cs, "(define (mul2 x) (* x 2))")) {
        ++g_failed;
        return false;
    }
    CHECK(run_int(cs, "(mul2 7)") == 14, "(mul2 7) => 14");
    CHECK(run_int(cs, "(mul2 0)") == 0, "(mul2 0) => 0");
    return true;
}

bool test_redefine_updates_binding() {
    std::println("\n--- AC3: redefine re-binds via IR and new body is used ---");
    aura::compiler::CompilerService cs;
    if (!run_ok(cs, "(define (f x) (+ x 1))")) {
        ++g_failed;
        return false;
    }
    CHECK(run_int(cs, "(f 5)") == 6, "first body: (f 5) => 6");
    const auto binds_after_first = cs.define_ir_env_bind_count();

    if (!run_ok(cs, "(define (f x) (* x 3))")) {
        ++g_failed;
        return false;
    }
    CHECK(cs.define_ir_env_bind_count() == binds_after_first + 1,
          "redefine bumps define_ir_env_bind_count again");
    CHECK(run_int(cs, "(f 5)") == 15, "redefined body: (f 5) => 15");
    return true;
}

bool test_nested_define_calls() {
    std::println("\n--- AC4: helper + caller both IR-bound ---");
    aura::compiler::CompilerService cs;
    if (!run_ok(cs, "(define (inc x) (+ x 1))")) {
        ++g_failed;
        return false;
    }
    if (!run_ok(cs, "(define (twice x) (* x 2))")) {
        ++g_failed;
        return false;
    }
    CHECK(run_int(cs, "(twice (inc 3))") == 8, "(twice (inc 3)) => 8");
    CHECK(cs.define_ir_env_bind_count() == 2, "two separate defines => two IR env binds");
    return true;
}

int run_tests() {
    std::println("Issue #272 (IR-native env binding for function defines)\n");
    test_define_ir_env_bind_metric();
    test_define_then_call();
    test_redefine_updates_binding();
    test_nested_define_calls();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace aura_issue_272_detail

int aura_issue_272_run() { return aura_issue_272_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_272_run(); }
#endif