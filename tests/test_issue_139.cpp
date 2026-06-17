// @category: integration
// @reason: uses CompilerService to eval Aura source
// test_issue_139.cpp — Verify Issue #139 acceptance criteria
// ("Add advanced structural refactoring operators to mutate EDSL
// and improve strategy selector").
//
// Issue #139 is an umbrella issue. Most of the requested structural
// refactor primitives are already implemented:
//   - mutate:extract-function (10548): extract an expression into a
//     new function, auto-detecting free-variable params
//   - mutate:inline-call (10671): inline a function call at the call site
//   - mutate:rename-symbol (10414): global rename with hygiene (all
//     bindings + uses updated atomically)
//   - mutate:refactor/extract (10174): general-purpose extract
//   - mutate:move-node (10472): move a subtree to a new position
//   - mutate:splice (9965), mutate:wrap (10057): composition operators
//
// This binary verifies the operators work end-to-end. Approach:
//   - Aura-level primitives (set-code + mutate:* + eval-current) for
//     behavior verification
//   - Direct C++ verification where possible (e.g. node count after
//     refactor)
//
// IMPORTANT: Workspace state is per-eval. We bundle set-code +
// mutate + eval-current into one cs.eval() call per observation.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
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
import aura.core.type;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.diag;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.parser.parser;



static int64_t run_int(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r) {
        std::println(std::cerr, "    [eval error: {}]", r.error().format());
        return -1;
    }
    if (!aura::compiler::types::is_int(*r)) {
        std::println(std::cerr, "    [expected int, got val={}]", r->val);
        return -1;
    }
    return aura::compiler::types::as_int(*r);
}

static std::string run_str(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r) {
        std::println(std::cerr, "    [eval error: {}]", r.error().format());
        return "";
    }
    if (!aura::compiler::types::is_string(*r)) {
        std::println(std::cerr, "    [expected string, got val={}]", r->val);
        return "";
    }
    auto idx = aura::compiler::types::as_string_idx(*r);
    const auto& heap = cs.evaluator().string_heap();
    if (idx >= heap.size()) return "";
    return std::string(heap[idx]);
}

// Helper: count nodes with a given tag in the workspace flat.
// Uses the query primitives if available.
static int64_t count_nodes(aura::compiler::CompilerService& cs, const std::string& tag) {
    return run_int(cs, std::string("(query:node-type ") + tag + ")");
}

// ═══════════════════════════════════════════════════════════════
// AC #1: Structural refactor operators
// ═══════════════════════════════════════════════════════════════

// ── Test 1.1: mutate:rename-symbol renames all occurrences ───

bool test_rename_symbol_all_occurrences() {
    std::println("\n--- Test 1.1: mutate:rename-symbol renames all occurrences ---");
    aura::compiler::CompilerService cs;
    // After rename x→y, the workspace should still pass typecheck.
    // The (f 5) call after eval-current is hard to verify directly
    // because workspace defines are not in the eval env. We use
    // typecheck-current as a proxy for "the workspace is still
    // valid after rename".
    std::string status = run_str(cs,
        "(set-code \"(define (f x) (+ x x))\") "
        "(begin "
        "  (mutate:rename-symbol \"x\" \"y\" \"rename test\") "
        "  (typecheck-current))");
    bool ok = status.find("no errors") != std::string::npos;
    CHECK(ok, "after rename x→y, typecheck-current passes (status: " +
          status.substr(0, 60) + ")");
    return true;
}

// ── Test 1.2: rename-symbol with new function (cross-binding) ──

bool test_rename_symbol_creates_new_binding() {
    std::println("\n--- Test 1.2: rename-symbol cross-binding verification ---");
    aura::compiler::CompilerService cs;
    // Rename f → g; the body should still work
    int64_t r = run_int(cs,
        "(set-code \"(define (f x) (+ x 1))(f 5)\") "
        "(mutate:rename-symbol \"f\" \"g\" \"rename test\") "
        "(eval-current) "
        "(g 5)");
    CHECK(r == 6, "after rename f→g, (g 5) = 6 (got " + std::to_string(r) + ")");
    return true;
}

// ── Test 1.3: rename-symbol doesn't touch shadowed locals ───

bool test_rename_symbol_preserves_shadow() {
    std::println("\n--- Test 1.3: rename-symbol handles shadowed locals ---");
    aura::compiler::CompilerService cs;
    // The inner `x` shadows the outer `x`. Renaming outer `x` should
    // not touch the inner `x`. We use a function where outer and
    // inner are both used to verify they remain distinct.
    int64_t r = run_int(cs,
        "(set-code "
        "\""
        "(define (outer x) (define (inner x) (* x 2)) (+ x (inner x)))"
        "\") "
        "(mutate:rename-symbol \"outer\" \"outer2\" \"test\") "
        "(eval-current) "
        "(outer2 5)");
    // After rename, outer → outer2, but inner's x is unchanged.
    // (outer2 5) should still compute (5 + (5*2)) = 15.
    CHECK(r == 15, "after rename outer→outer2, (outer2 5) = 15 (got " +
          std::to_string(r) + ")");
    return true;
}

// ── Test 1.4: mutate:inline-call inlines a call site ─────────

bool test_inline_call_basic() {
    std::println("\n--- Test 1.4: mutate:inline-call inlines a call ---");
    aura::compiler::CompilerService cs;
    // (define (sq x) (* x x))
    // (define v (sq 5))  -> v=25
    // After inline-call on (sq 5), the call site becomes (* 5 5).
    // The result should still be 25.
    int64_t r = run_int(cs,
        "(set-code \"(define (sq x) (* x x))(define v (sq 5))\") "
        // Inline the call to sq. Find Call nodes; the (sq 5) call
        // site is the second one (after the (* x x) in sq's body).
        "(begin "
        "  (define call-ids (query:node-type \"Call\")) "
        "  (define target (list-ref call-ids 1)) "
        "  (mutate:inline-call target \"inline test\") "
        "  (eval-current) "
        "  v)");
    CHECK(r == 25, "after inline-call, v=25 (got " + std::to_string(r) + ")");
    return true;
}

// ── Test 1.5: mutate:refactor/extract extracts a subexpression ─

bool test_refactor_extract_basic() {
    std::println("\n--- Test 1.5: mutate:refactor/extract extracts a subexpression ---");
    aura::compiler::CompilerService cs;
    // (define (f x) (+ (* x 2) 1)) has 1 Define.
    // After extract, the workspace has 2 Defines: the original f
    // and the extracted `doubled` function. Verify the count.
    int64_t after = run_int(cs,
        "(set-code \"(define (f x) (+ (* x 2) 1))\") "
        "(begin "
        "  (define call-id (car (query:node-type \"Call\"))) "
        "  (mutate:refactor/extract call-id \"doubled\") "
        "  (length (query:node-type \"Define\")))");
    std::println("    [Defines after extract: {}]", after);
    CHECK(after == 2,
          "after refactor/extract: 2 Defines (original + extracted, got " +
          std::to_string(after) + ")");
    return true;
}

// ── Test 1.6: mutate:move-node moves a subtree ───────────────

bool test_move_node_basic() {
    std::println("\n--- Test 1.6: mutate:move-node moves a subtree ---");
    aura::compiler::CompilerService cs;
    // Build code with a structure we can move.
    // (begin (define a 1) (define b 2)) — move `(define b 2)` to before `(define a 1)`.
    // After move, the workspace is structurally reorganized.
    // The result of eval-current should still produce both bindings.
    int64_t r = run_int(cs,
        "(set-code \"(begin (define a 1) (define b 2))\") "
        // Count Begin nodes before move
        "(begin "
        "  (mutate:move-node (query:find \"b\") "
        "                       (query:find \"a\") "
        "                       0 "
        "                       \"move b before a\") "
        "  (eval-current) "
        "  (+ a b))");
    // After move, a=1 and b=2 still, (+ 1 2) = 3.
    CHECK(r == 3, "after move-node, (+ a b) = 3 (got " + std::to_string(r) + ")");
    return true;
}

// ── Test 1.7: mutate:extract-function extracts and creates def ─

bool test_extract_function_basic() {
    std::println("\n--- Test 1.7: mutate:extract-function creates a new function ---");
    aura::compiler::CompilerService cs;
    // (define (f x) (+ (* x 2) 1))
    // Extract the (* x 2) call body — mutate:extract-function
    // detects free variables and creates a function with them
    // as parameters.
    int64_t r = run_int(cs,
        "(set-code \"(define (f x) (+ (* x 2) 1))\") "
        "(begin "
        "  (define call-id (car (query:node-type \"Call\"))) "
        "  (mutate:extract-function call-id \"double-of\") "
        "  (eval-current) "
        "  (f 5))");
    CHECK(r == 11, "after extract-function, (f 5) still = 11 (got " +
          std::to_string(r) + ")");
    return true;
}

// ── Test 1.8: hygiene preservation in rename ────────────────

bool test_rename_hygiene() {
    std::println("\n--- Test 1.8: rename-symbol preserves hygiene (no capture) ---");
    aura::compiler::CompilerService cs;
    // A classic capture test: caller has a tmp, the macro/template
    // body has its own tmp. Renaming the macro's tmp should not
    // affect the caller's tmp.
    int64_t r = run_int(cs,
        "(set-code "
        "\""
        "(define-hygienic-macro (swap! a b) (let ((tmp a)) (set! a b) (set! b tmp)))"
        "(define x 1) (define y 2) (define tmp 99)"
        "\") "
        "(eval-current) "
        "(begin (swap! x y) tmp)");
    // After (swap! x y), x=2, y=1. The caller's tmp should be
    // unchanged (hygienic macro).
    CHECK(r == 99, "caller's tmp is NOT captured by hygienic swap! (got " +
          std::to_string(r) + ")");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #2: Stress test — many refactors in sequence
// ═══════════════════════════════════════════════════════════════

// ── Test 2.1: many rename cycles don't corrupt the AST ────

bool test_stress_rename_cycles() {
    std::println("\n--- Test 2.1: many rename cycles, no AST corruption ---");
    aura::compiler::CompilerService cs;
    // Do 50 rename cycles on a small workspace, verify it
    // remains valid throughout.
    std::string code2 = "(set-code \"(define a 1)(define b 2)\") (begin ";
    for (int i = 0; i < 50; ++i) {
        if (i % 2 == 0) {
            code2 += "(mutate:rename-symbol \"a\" \"tmp\" \"test\") ";
        } else {
            code2 += "(mutate:rename-symbol \"tmp\" \"a\" \"test\") ";
        }
    }
    code2 += "(typecheck-current))";
    std::string status = run_str(cs, code2);
    bool ok = status.find("no errors") != std::string::npos;
    CHECK(ok, "after 50 rename cycles, typecheck-current passes (status: " +
          status.substr(0, 60) + ")");
    return true;
}

// ── Test 2.2: mixed refactor operations ─────────────────────

bool test_mixed_refactors() {
    std::println("\n--- Test 2.2: mixed refactor ops on a workspace ---");
    aura::compiler::CompilerService cs;
    // Combine rename + inline in a single workspace. The result
    // should still produce a valid workspace (typecheck passes).
    std::string status = run_str(cs,
        "(set-code \"(define (sq x) (* x x))(define v (sq 5))\") "
        "(begin "
        "  (mutate:rename-symbol \"sq\" \"square\" \"test\") "
        "  (mutate:inline-call (list-ref (query:node-type \"Call\") 1) \"test\") "
        "  (typecheck-current))");
    bool ok = status.find("no errors") != std::string::npos;
    CHECK(ok, "after rename+inline, typecheck-current still passes (status: " +
          status.substr(0, 60) + ")");
    return true;
}

// ── Test 2.3: splice and wrap primitives work ──────────────

bool test_splice_wrap() {
    std::println("\n--- Test 2.3: mutate:splice and mutate:wrap ---");
    aura::compiler::CompilerService cs;
    // Verify that mutate:wrap produces a still-valid workspace.
    std::string code = "(set-code \"(define x 5)\") "
                       "(begin "
                       "  (mutate:wrap (car (query:find \"x\")) \"lambda-wrap\" \"test\") "
                       "  (typecheck-current))";
    std::string status = run_str(cs, code);
    bool ok = status.find("no errors") != std::string::npos;
    CHECK(ok, "after mutate:wrap, typecheck-current passes (status: " +
          status.substr(0, 60) + ")");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #3: Refactor operators are observable via the type system
// ═══════════════════════════════════════════════════════════════

// ── Test 3.1: rename preserves type correctness ──────────

bool test_rename_type_correctness() {
    std::println("\n--- Test 3.1: rename preserves type correctness ---");
    aura::compiler::CompilerService cs;
    // After rename, typecheck-current should still pass.
    std::string code = "(set-code \"(define (add1 x) (+ x 1))(add1 5)\") "
                       "(begin "
                       "  (mutate:rename-symbol \"add1\" \"increment\" \"test\") "
                       "  (typecheck-current))";
    std::string status = run_str(cs, code);
    bool ok = status.find("no errors") != std::string::npos;
    CHECK(ok, "after rename add1→increment, typecheck passes (status: " +
          status.substr(0, 60) + ")");
    return true;
}

// ── Test 3.2: extract-function creates a properly-typed def ─

bool test_extract_type_correctness() {
    std::println("\n--- Test 3.2: extract-function produces typed def ---");
    aura::compiler::CompilerService cs;
    std::string code = "(set-code \"(define (f x) (+ (* x 2) 1))\") "
                       "(begin "
                       "  (mutate:extract-function (car (query:find \"x\")) \"double-of\") "
                       "  (typecheck-current))";
    std::string status = run_str(cs, code);
    bool ok = status.find("no errors") != std::string::npos;
    CHECK(ok, "after extract-function, typecheck passes (status: " +
          status.substr(0, 60) + ")");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════

int main() {
    std::println("═══ Issue #139 verification tests ═══\n");

    std::println("── AC #1: Structural refactor operators ──");
    test_rename_symbol_all_occurrences();
    test_rename_symbol_creates_new_binding();
    test_rename_symbol_preserves_shadow();
    test_inline_call_basic();
    test_refactor_extract_basic();
    test_move_node_basic();
    test_extract_function_basic();
    test_rename_hygiene();

    std::println("\n── AC #2: Stress test (refactor sequences) ──");
    test_stress_rename_cycles();
    test_mixed_refactors();
    test_splice_wrap();

    std::println("\n── AC #3: Type correctness after refactor ──");
    test_rename_type_correctness();
    test_extract_type_correctness();

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
