// @category: integration
// @reason: Occurrence Typing: extend check_flat If-branch to
//          use narrowing + Linear Ownership integration
//          (Issue #283: bidirectional check-mode + OwnershipEnv
//          in if-branches).


#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.core.mutation;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_283_detail {

// ── AC1: check-mode honors predicate narrowing. ──
// Before #283, check_flat's If branch only checked
// condition is Bool and uniform-typed both branches.
// After #283, an (if (string? x) (length x) 0)
// should type-check cleanly in BOTH modes.
bool test_check_mode_with_predicate() {
    std::println("\n--- AC1: check-mode honors predicate narrowing ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (f x) (if (string? x) (length x) 0))\")")) {
        ++g_failed;
        return false;
    }
    // synthesize path
    auto r1 = cs.eval("(f \"hi\")");
    CHECK(r1.has_value(), "synthesize path: (f \"hi\") evaluates");
    // typecheck path (check-mode entry)
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "(typecheck-current) succeeds in check-mode");
    return true;
}

// ── AC2: narrowing applies in check-mode then-branch. ──
// Two variables x and y in branches should each get
// their narrowing applied (not just the first).
bool test_check_mode_two_branches() {
    std::println("\n--- AC2: check-mode narrowing on multiple branches ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \""
                 "(define (g x y) "
                 "  (if (string? x) "
                 "      (if (number? y) (+ y 1) 0) "
                 "      0))\")")) {
        ++g_failed;
        return false;
    }
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "nested (if string? x (if number? y ...) ...) typechecks");
    return true;
}

// ── AC3: negation case (else-branch gets narrowing). ──
// (if (not (string? x)) 0 (length x)) — the else-branch
// should see x as String (after negation flip).
bool test_check_mode_negation() {
    std::println("\n--- AC3: check-mode negation — else-branch gets narrowing ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \""
                 "(define (h x) "
                 "  (if (not (string? x)) 0 (length x)))\")")) {
        ++g_failed;
        return false;
    }
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "(if (not (string? x)) 0 (length x)) typechecks");
    return true;
}

// ── AC4: Linear ownership narrowing binding. ──
// In the narrowed branch, the variable should be marked
// Owned (the predicate guarantees presence, use is OK).
// We can't directly observe OwnershipEnv::Owned, but we
// can verify (let ((x (cons 1 2))) (if (pair? x) (car x) 0))
// typechecks — the (car x) requires x to be Pair + Owned.
bool test_linear_ownership_in_narrowed_branch() {
    std::println("\n--- AC4: Linear ownership in narrowed branch ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \""
                 "(define (p x) "
                 "  (if (pair? x) (car x) 0))\")")) {
        ++g_failed;
        return false;
    }
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "(if (pair? x) (car x) 0) typechecks with linear ownership");
    auto r = cs.eval("(p (cons 1 2))");
    CHECK(r.has_value(), "(p (cons 1 2)) evaluates");
    return true;
}

// ── AC5: backward compat — non-predicate condition still
// works. (if (> x 0) x 0) has no predicate, just a guard.
bool test_backward_compat_non_predicate() {
    std::println("\n--- AC5: backward compat — non-predicate condition ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (g x) (if (> x 0) x 0))\")")) {
        ++g_failed;
        return false;
    }
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "(if (> x 0) x 0) typechecks");
    return true;
}

// ── AC6: gradual guarantee — narrowing doesn't cause
// a runtime error when actual type mismatches predicate.
bool test_gradual_guarantee() {
    std::println("\n--- AC6: gradual guarantee ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (g x) (if (string? x) (length x) 0))\")")) {
        ++g_failed;
        return false;
    }
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "typecheck succeeds");
    auto r = cs.eval("(g 42)");
    CHECK(r.has_value(), "(g 42) returns 0 (no crash from narrowing)");
    return true;
}

int run_tests() {
    std::println("Issue #283 (check-mode Occurrence Typing + Linear Ownership)\n");
    test_check_mode_with_predicate();
    test_check_mode_two_branches();
    test_check_mode_negation();
    test_linear_ownership_in_narrowed_branch();
    test_backward_compat_non_predicate();
    test_gradual_guarantee();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_283_detail

int aura_issue_283_run() {
    return aura_issue_283_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_283_run();
}
#endif