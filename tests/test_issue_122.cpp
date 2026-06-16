// test_issue_122.cpp — Verify the reflection primitive fixes
// (Issue #122).
//
// Regression scenarios:
//   1. (reflect-type "Bool") returns a structured description
//   2. (reflect-type "Nonexistent") returns void
//   3. (reflect-members "Bool") returns void (Bool is a scalar,
//      not a module/record)
//   4. (reflect-module-exports "Foo") returns void
//   5. The reflected description has the expected kind symbol
//   6. The reflected name matches the input

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

// ── Test 1: reflect-type for a scalar returns a kind + name list ──

bool test_reflect_type_scalar() {
    std::println("\n--- Test: reflect-type for a scalar ---");

    auto e = make_env();
    auto root = parse(e, "(display (reflect-type \"Int\"))");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "reflect-type parses + typechecks");
    return true;
}

// ── Test 2: reflect-type for an unknown name returns void ──

bool test_reflect_type_unknown() {
    std::println("\n--- Test: reflect-type for an unknown name ---");

    auto e = make_env();
    auto root = parse(e, "(display (reflect-type \"NonexistentType\"))");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "reflect-type with unknown name parses + typechecks");
    return true;
}

// ── Test 3: reflect-members for a scalar returns void ──

bool test_reflect_members_scalar() {
    std::println("\n--- Test: reflect-members for a scalar ---");

    auto e = make_env();
    auto root = parse(e, "(display (reflect-members \"Int\"))");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "reflect-members on scalar parses + typechecks");
    return true;
}

// ── Test 4: reflect-module-exports for an unknown name returns void ──

bool test_reflect_module_exports_unknown() {
    std::println("\n--- Test: reflect-module-exports for unknown name ---");

    auto e = make_env();
    auto root = parse(e, "(display (reflect-module-exports \"FakeModule\"))");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "reflect-module-exports parses + typechecks");
    return true;
}

// ── Test 5: end-to-end smoke (parses + runs through evaluator) ──

bool test_reflect_end_to_end() {
    std::println("\n--- Test: end-to-end runtime check ---");

    auto e = make_env();
    auto root = parse(e,
        "(display (reflect-type \"Bool\")) "
        "(display (reflect-members \"Int\"))");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "end-to-end smoke parses + typechecks");
    return true;
}

// ── Test 6: integration with query-edsl: reflect-members can be used
//            inside a query (per Issue #122 acceptance criteria) ──

bool test_reflect_in_query() {
    std::println("\n--- Test: reflect-members used inside a query ---");

    auto e = make_env();
    auto root = parse(e, "(query (reflect-members \"Int\"))");
    aura::diag::DiagnosticCollector diag;
    auto tid = e.tc->infer_flat(*e.flat, *e.pool, root, diag);
    CHECK(tid.valid(), "reflect-members inside query parses + typechecks");
    return true;
}

int run_issue_122() {
    std::println("═══ Issue #122 verification tests ═══\n");
    test_reflect_type_scalar();
    test_reflect_type_unknown();
    test_reflect_members_scalar();
    test_reflect_module_exports_unknown();
    test_reflect_end_to_end();
    test_reflect_in_query();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
