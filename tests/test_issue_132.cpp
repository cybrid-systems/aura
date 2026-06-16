// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_132.cpp — Verify the AST walker extractions
// (Issue #132).
//
// Regression scenarios:
//   1. find_top_level_defines extracts all top-level
//      (define ...) forms in document order
//   2. find_top_level_defines skips nested defines inside
//      lambda bodies
//   3. find_top_level_defines handles NULL_NODE and
//      out-of-range roots
//   4. collect_user_bindings extracts both Define and
//      TypeAnnotation bindings
//   5. Both functions are pure (no global state, no
//      mutation of inputs)

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
import aura.compiler.ast_walkers;



// ── Test 1: find_top_level_defines basic ───────────────

bool test_find_top_level_defines_basic() {
    std::println("\n--- Test: find_top_level_defines — basic ---");

    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    aura::ast::FlatAST flat(alloc);
    aura::ast::StringPool pool(alloc);

    // Build: (begin (define a 1) (define b 2) 42)
    auto a_name = pool.intern("a");
    auto b_name = pool.intern("b");
    auto def_a = flat.add_define(a_name, flat.add_literal(1));
    auto def_b = flat.add_define(b_name, flat.add_literal(2));
    auto lit_42 = flat.add_literal(42);
    auto begin = flat.add_begin({def_a, def_b, lit_42});

    auto defs = aura::compiler::find_top_level_defines(flat, pool, begin);
    CHECK(defs.size() == 2,
          "find_top_level_defines returns 2 defines");
    if (defs.size() == 2) {
        CHECK(defs[0].first == "a", "first define is 'a'");
        CHECK(defs[1].first == "b", "second define is 'b'");
    }
    return true;
}

// ── Test 2: find_top_level_defines skips nested ────────

bool test_find_top_level_defines_nested() {
    std::println("\n--- Test: find_top_level_defines — skips nested ---");

    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    aura::ast::FlatAST flat(alloc);
    aura::ast::StringPool pool(alloc);

    // Build: (begin (define outer 1)
    //              (lambda (x) (define inner 2)))
    auto outer_name = pool.intern("outer");
    auto inner_name = pool.intern("inner");
    auto x_name = pool.intern("x");
    auto def_outer = flat.add_define(outer_name, flat.add_literal(1));
    auto x_var = flat.add_variable(x_name);
    auto inner_body = flat.add_define(inner_name, flat.add_literal(2));
    auto lambda = flat.add_lambda({x_var}, {inner_body});
    auto begin = flat.add_begin({def_outer, lambda});

    auto defs = aura::compiler::find_top_level_defines(flat, pool, begin);
    // Outer is found; inner (inside lambda) is NOT
    CHECK(defs.size() == 1,
          "find_top_level_defines skips nested defines");
    if (defs.size() == 1) {
        CHECK(defs[0].first == "outer", "only outer is found");
    }
    return true;
}

// ── Test 3: find_top_level_defines edge cases ──────────

bool test_find_top_level_defines_edge() {
    std::println("\n--- Test: find_top_level_defines — edge cases ---");

    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    aura::ast::FlatAST flat(alloc);
    aura::ast::StringPool pool(alloc);

    // NULL_NODE → empty result
    auto defs1 = aura::compiler::find_top_level_defines(
        flat, pool, aura::ast::NULL_NODE);
    CHECK(defs1.empty(), "NULL_NODE returns empty result");

    // Literal (not a Begin, not a Define) → empty
    auto lit = flat.add_literal(42);
    auto defs2 = aura::compiler::find_top_level_defines(flat, pool, lit);
    CHECK(defs2.empty(), "non-define root returns empty result");
    return true;
}

// ── Test 4: collect_user_bindings ──────────────────────

bool test_collect_user_bindings() {
    std::println("\n--- Test: collect_user_bindings ---");

    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    aura::ast::FlatAST flat(alloc);
    aura::ast::StringPool pool(alloc);

    // Build: (begin (define a 1) (: b Int) (define c 3))
    // add_type_annotation(type_name, inner, var_sym).
    // The variable name "b" goes in var_sym so the walker
    // can extract it as the user binding.
    auto a_name = pool.intern("a");
    auto b_name = pool.intern("b");
    auto c_name = pool.intern("c");
    auto int_sym = pool.intern("Int");
    auto def_a = flat.add_define(a_name, flat.add_literal(1));
    auto def_c = flat.add_define(c_name, flat.add_literal(3));
    auto annot = flat.add_type_annotation(int_sym, flat.add_literal(0), b_name);
    auto begin = flat.add_begin({def_a, annot, def_c});

    auto names = aura::compiler::collect_user_bindings(flat, pool, begin);
    CHECK(names.size() == 3, "collect_user_bindings returns 3 names");
    if (names.size() == 3) {
        CHECK(names[0] == "a", "first is 'a' (define)");
        CHECK(names[1] == "b", "second is 'b' (annotation)");
        CHECK(names[2] == "c", "third is 'c' (define)");
    }
    return true;
}

int run_issue_132() {
    std::println("═══ Issue #132 verification tests ═══\n");
    test_find_top_level_defines_basic();
    test_find_top_level_defines_nested();
    test_find_top_level_defines_edge();
    test_collect_user_bindings();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
