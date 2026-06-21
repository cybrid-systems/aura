// @category: integration
// @reason: uses CompilerService to verify IR-native define env binding

// test_issue_272.cpp — Issue #272: IR-native env binding for defines.
// Cycle 1: cache_define / eval. Cycle 2: compile_module + invalidate.
// Cycle 3: value defines + TopCellLoad references.

#include <iostream>
#include <memory>
#include <print>
#include <string>

#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.arena;
import aura.parser.parser;
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

bool test_compile_module_ir_bind() {
    std::println("\n--- AC5: compile_module binds via IR ---");
    aura::compiler::CompilerService cs;
    const auto binds_before = cs.define_ir_env_bind_count();
    auto r = cs.compile_module("mod272", "(define (mod-fn x) (* x 10))");
    CHECK(static_cast<bool>(r), "compile_module succeeds");
    CHECK(cs.define_ir_env_bind_count() == binds_before + 1,
          "compile_module bumps define_ir_env_bind_count");
    CHECK(run_int(cs, "(mod-fn 3)") == 30, "(mod-fn 3) => 30 after compile_module");
    return true;
}

bool test_invalidate_rebinds_dependent() {
    std::println("\n--- AC6: redefine invalidates and re-binds dependents via IR ---");
    aura::compiler::CompilerService cs;
    if (!run_ok(cs, "(define (base x) (+ x 1))")) {
        ++g_failed;
        return false;
    }
    if (!run_ok(cs, "(define (wrap x) (base x))")) {
        ++g_failed;
        return false;
    }
    CHECK(run_int(cs, "(wrap 5)") == 6, "wrap uses (+ x 1) base initially");

    if (!run_ok(cs, "(define (base x) (* x 2))")) {
        ++g_failed;
        return false;
    }
    CHECK(run_int(cs, "(wrap 5)") == 10, "wrap uses re-bound (* x 2) base after redefine");
    return true;
}

bool test_value_define_ir_bind() {
    std::println("\n--- AC8: value define bound via IR ---");
    aura::compiler::CompilerService cs;
    CHECK(cs.value_define_ir_env_bind_count() == 0, "value metric starts at 0");
    auto r = cs.eval("(define y (+ 1 2))");
    CHECK(static_cast<bool>(r), "value define succeeds");
    CHECK(cs.value_define_ir_env_bind_count() == 1, "value define bumps IR bind metric");
    CHECK(run_int(cs, "(+ y 10)") == 13, "(+ y 10) => 13 via TopCellLoad");
    return true;
}

bool test_value_define_no_tree_walker_fallback() {
    std::println("\n--- AC9: value define does not need tree-walker fallback ---");
    aura::compiler::CompilerService cs;
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    auto pr = aura::parser::parse_to_flat("(define n 42)", *flat, *pool);
    CHECK(pr.success, "value define parses");
    flat->root = pr.root;
    CHECK(!cs.public_needs_tree_walker_fallback(*flat, *pool, pr.root),
          "needs_tree_walker_fallback false for value define");
    return true;
}

bool test_fn_define_no_tree_walker_fallback() {
    std::println("\n--- AC7: pure function define does not need tree-walker fallback ---");
    aura::compiler::CompilerService cs;
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    auto pr = aura::parser::parse_to_flat("(define (f x) (+ x 1))", *flat, *pool);
    CHECK(pr.success, "function define parses");
    flat->root = pr.root;
    CHECK(!cs.public_needs_tree_walker_fallback(*flat, *pool, pr.root),
          "needs_tree_walker_fallback false for function define");
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
    test_compile_module_ir_bind();
    test_define_ir_env_bind_metric();
    test_define_then_call();
    test_redefine_updates_binding();
    test_invalidate_rebinds_dependent();
    test_value_define_ir_bind();
    test_value_define_no_tree_walker_fallback();
    test_fn_define_no_tree_walker_fallback();
    test_nested_define_calls();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace aura_issue_272_detail

int aura_issue_272_run() { return aura_issue_272_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_272_run(); }
#endif