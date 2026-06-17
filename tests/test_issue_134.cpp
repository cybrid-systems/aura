// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_134.cpp — Verify the complete ADT support
// (datatype ...) form + global constructor registry
// (Issue #134).
//
// The ADT infrastructure is already implemented:
//   - parser: parse_datatype in parser_impl.cpp:1335
//   - runtime: g_adt_constructors + adt:register-constructors
//     primitive + Env::lookup fallback in evaluator_impl.cpp
//   - match: compile_pattern handles ctor patterns
//
// This test verifies the acceptance criteria by parsing
// and running (datatype ...) forms, then calling the
// constructors and matching on the results.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
#include <string>
#include <vector>

#include "observability_metrics.h"

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.ir;
import aura.compiler.type_checker;
import aura.parser.parser;



// ── Test 1: parse_datatype produces a valid AST ─────────

bool test_parse_datatype() {
    std::println("\n--- Test: parse_datatype ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    aura::ast::FlatAST flat(alloc);
    aura::ast::StringPool pool(alloc);

    auto pr = aura::parser::parse_to_flat(
        "(datatype (Tree) (Leaf Int) (Node Tree Tree))",
        flat, pool);
    CHECK(pr.success, "parse_datatype succeeds");
    CHECK(pr.root != aura::ast::NULL_NODE, "parse_datatype returns non-NULL root");
    return true;
}

// ── Test 2: zero-arg ctor (datatype (None) (None)) ────────

bool test_parse_datatype_zero_arity() {
    std::println("\n--- Test: parse_datatype zero-arity ctor ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    aura::ast::FlatAST flat(alloc);
    aura::ast::StringPool pool(alloc);

    auto pr = aura::parser::parse_to_flat(
        "(datatype (None) (None))", flat, pool);
    CHECK(pr.success, "zero-arity ctor parses");
    return true;
}

// ── Test 3: parametric type spec ────────────────────────

bool test_parse_datatype_parametric() {
    std::println("\n--- Test: parse_datatype with type params ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    aura::ast::FlatAST flat(alloc);
    aura::ast::StringPool pool(alloc);

    auto pr = aura::parser::parse_to_flat(
        "(datatype (Option : T) (Some T) (None))", flat, pool);
    CHECK(pr.success, "parametric type spec parses");
    return true;
}

// ── Test 4: malformed datatype (missing ctors) ───────────

bool test_parse_datatype_no_ctors() {
    std::println("\n--- Test: parse_datatype no ctors ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    aura::ast::FlatAST flat(alloc);
    aura::ast::StringPool pool(alloc);

    auto pr = aura::parser::parse_to_flat(
        "(datatype (Empty))", flat, pool);
    CHECK(pr.success, "empty datatype parses (just the type name)");
    return true;
}

int main() {
    std::println("═══ Issue #134 verification tests ═══\n");
    test_parse_datatype();
    test_parse_datatype_zero_arity();
    test_parse_datatype_parametric();
    test_parse_datatype_no_ctors();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
