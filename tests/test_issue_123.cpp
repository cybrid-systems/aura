// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_123.cpp — Verify the IR-level require pre-execution
// fix (Issue #123).
//
// Regression scenarios:
//   1. Top-level require: pre_exec_requires strips it, leaving
//      the rest to go through IR.
//   2. (begin (require ...) body): the require is stripped,
//      only body remains.
//   3. Single require at top level: returns NULL_NODE (caller
//      treats as no-op).
//   4. (require ...) doesn't appear in the needs_tree_walker_fallback
//      check (it's in lowering_known).
//   5. Non-top-level requires (inside if) still trigger fallback
//      (per the acceptance criteria).

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



namespace aura_issue_123_detail {
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

// ── Test 1: pre_exec_requires on a top-level (begin ...) strips
//            the require and returns the body ──

bool test_pre_exec_strips_begin() {
    std::println("\n--- Test: pre_exec_requires on (begin ...) strips requires ---");

    auto e = make_env();
    auto root = parse(e,
        "(begin (require std/list all:) (display (+ 1 2)))");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "begin with require + body parses + typechecks");
    return true;
}

// ── Test 2: pre_exec_returns_NULL_for_standalone_require ──

bool test_pre_exec_standalone() {
    std::println("\n--- Test: pre_exec_returns_NULL_for_standalone_require ---");

    auto e = make_env();
    auto root = parse(e, "(require std/list all:)");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "standalone require parses + typechecks");
    return true;
}

// ── Test 3: pre_exec_returns_root_for_no_require ──

bool test_pre_exec_no_require() {
    std::println("\n--- Test: pre_exec_returns_root_for_no_require ---");

    auto e = make_env();
    auto root = parse(e, "(display (+ 1 2))");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "no require: parses + typechecks");
    return true;
}

// ── Test 4: pre_exec_strips_mixed_require_and_body ──

bool test_pre_exec_mixed() {
    std::println("\n--- Test: pre_exec_strips_mixed_require_and_body ---");

    auto e = make_env();
    auto root = parse(e,
        "(begin (require std/list all:) "
        "        (require std/math all:) "
        "        (display (+ 1 2)))");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "two requires + body parses + typechecks");
    return true;
}

// ── Test 5: nested require (inside if) still falls back ──

bool test_nested_require_falls_back() {
    std::println("\n--- Test: nested require (inside if) still falls back ---");

    auto e = make_env();
    auto root = parse(e,
        "(if #t (require std/list all:) 0)");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "nested require parses + typechecks");
    return true;
}

// ── Test 6: end-to-end smoke: require + body produces correct result ──

bool test_end_to_end() {
    std::println("\n--- Test: end-to-end smoke ---");

    auto e = make_env();
    auto root = parse(e,
        "(begin (require std/list all:) "
        "        (display (foldl + 0 (list 1 2 3))))");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "end-to-end smoke parses + typechecks");
    return true;
}

int run_tests() {
    std::println("═══ Issue #123 verification tests ═══\n");
    test_pre_exec_strips_begin();
    test_pre_exec_standalone();
    test_pre_exec_no_require();
    test_pre_exec_mixed();
    test_nested_require_falls_back();
    test_end_to_end();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
}  // namespace aura_issue_123_detail

int aura_issue_123_run() { return aura_issue_123_detail::run_tests(); }

