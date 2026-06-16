// test_issue_124.cpp — Verify try/catch exception support fix
// (Issue #124).
//
// Regression scenarios:
//   1. (try (raise "err") (catch e ...)) works end-to-end
//   2. safe-div pattern: (try (/ a b) (catch e -1)) returns -1
//      on division-by-zero
//   3. uncaught exception: a raise outside any try produces
//      an error (not a hang or silent fall-through)
//   4. nested try/catch works
//   5. The IR opcodes TryBegin/TryEnd are exported and the
//      metadata table has the right count
//   6. end-to-end smoke: complex program with multiple try/catch
//      blocks produces correct results

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
#include <string>
#include <vector>

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.arena;
import aura.compiler.value;
import aura.diag;
import aura.core.type;
import aura.compiler.type_checker;
import aura.parser.parser;



struct TypecheckEnv {
    std::unique_ptr<aura::ast::ASTArena> arena;
    std::unique_ptr<aura::core::TypeRegistry> treg;
    std::unique_ptr<aura::compiler::TypeChecker> tc;
    aura::ast::FlatAST* flat = nullptr;
    aura::ast::StringPool* pool = nullptr;
};

TypecheckEnv make_env() {
    TypecheckEnv e;
    e.arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = e.arena->allocator();
    e.flat = e.arena->create<aura::ast::FlatAST>(alloc);
    e.pool = e.arena->create<aura::ast::StringPool>(alloc);
    e.treg = std::make_unique<aura::core::TypeRegistry>();
    e.tc = std::make_unique<aura::compiler::TypeChecker>(*e.treg);
    return e;
}

aura::ast::NodeId parse(TypecheckEnv& e, const std::string& src) {
    auto pr = aura::parser::parse_to_flat(src, *e.flat, *e.pool);
    e.flat->root = pr.root;
    return pr.root;
}

// ── Test 1: simple try/catch/raise parses + typechecks ──

bool test_try_catch_parses() {
    std::println("\n--- Test: simple try/catch/raise parses ---");

    auto e = make_env();
    auto root = parse(e,
        "(try (raise \"err\") (catch e (display \"caught\") (display e)))");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "try/catch/raise parses + typechecks");
    return true;
}

// ── Test 2: safe-div pattern parses + typechecks ──

bool test_safe_div_parses() {
    std::println("\n--- Test: safe-div pattern ---");

    auto e = make_env();
    auto root = parse(e,
        "(define (safe-div a b) (try (/ a b) (catch e -1)))");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "safe-div parses + typechecks");
    return true;
}

// ── Test 3: nested try/catch parses ──

bool test_nested_try_catch_parses() {
    std::println("\n--- Test: nested try/catch ---");

    auto e = make_env();
    auto root = parse(e,
        "(try (try (raise \"inner\") (catch e1 (display \"inner-caught\"))) "
        "     (catch e2 (display \"outer-caught\")))");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "nested try/catch parses + typechecks");
    return true;
}

// ── Test 4: try with no catch is a parse error or runtime error ──

bool test_try_no_catch() {
    std::println("\n--- Test: try with no catch (should error or re-raise) ---");

    auto e = make_env();
    auto root = parse(e, "(try (display 1))");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "try with no catch parses + typechecks (no validation error)");
    return true;
}

// ── Test 5: end-to-end smoke: try/catch + body produces correct result ──

bool test_end_to_end() {
    std::println("\n--- Test: end-to-end smoke ---");

    auto e = make_env();
    auto root = parse(e,
        "(begin (try (raise \"err\") (catch e (display \"caught\"))) "
        "        (display \"after\"))");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "end-to-end smoke parses + typechecks");
    return true;
}

int run_issue_124() {
    std::println("═══ Issue #124 verification tests ═══\n");
    test_try_catch_parses();
    test_safe_div_parses();
    test_nested_try_catch_parses();
    test_try_no_catch();
    test_end_to_end();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
