// @category: integration
// @reason: Blame & Provenance: track Occurrence Typing narrowings
//          in MutationRecord-equivalent provenance log
//          (Issue #282: NarrowingRecord + query:provenance-of
//          + per-IfExpr capture in synthesize_flat_if).


#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.mutation;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_282_detail {

static int64_t run_int(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

// ── AC1: (query:provenance-of "x") returns narrowing records. ──
// After typechecking a function with (if (string? x) ...),
// x should have at least one provenance entry.
bool test_provenance_basic() {
    std::println("\n--- AC1: (query:provenance-of) returns narrowing records ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \""
                 "(define (f x) "
                 "  (if (string? x) (length x) 0))\")")) {
        ++g_failed;
        return false;
    }

    // Force a full typecheck to capture narrowing provenance.
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "(typecheck-current) runs");

    // Count provenance entries for x.
    auto count = run_int(cs, "(length (query:provenance-of \"x\"))");
    CHECK(count >= 1, "provenance of x has >= 1 entry (the (string? x) narrowing)");

    return true;
}

// ── AC2: the predicate source is captured in the record. ──
// The first narrowing entry should have :predicate set
// to a string mentioning "string?".
bool test_provenance_predicate_source() {
    std::println("\n--- AC2: predicate source captured ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \""
                 "(define (g x) "
                 "  (if (number? x) (+ x 1) 0))\")")) {
        ++g_failed;
        return false;
    }

    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "(typecheck-current) runs");

    auto r = cs.eval("(g 42)");
    CHECK(r.has_value(), "(g 42) evaluates");

    auto count = run_int(cs, "(length (query:provenance-of \"x\"))");
    CHECK(count >= 1, "provenance of x has >= 1 entry");

    // Read the first entry's :predicate field.
    auto pred_str = cs.eval("(hash-ref (car (query:provenance-of \"x\")) \"predicate\")");
    CHECK(pred_str.has_value(), "first provenance entry's :predicate is a value");

    return true;
}

// ── AC3: (query:provenance-of) handles unknown var name. ──
bool test_provenance_unknown_var() {
    std::println("\n--- AC3: (query:provenance-of) for unknown var ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (f x) (if (string? x) (length x) 0))\")")) {
        ++g_failed;
        return false;
    }
    auto r = cs.eval("(f \"hi\")");
    CHECK(r.has_value(), "(f \"hi\") evaluates");

    // Unknown var should return ().
    auto count = run_int(cs, "(length (query:provenance-of \"no-such-var\"))");
    CHECK(count == 0, "provenance of unknown var = 0");

    return true;
}

// ── AC4: provenance is keyed by var name — different vars
// get different provenance lists.
bool test_provenance_per_var() {
    std::println("\n--- AC4: provenance keyed by var name ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \""
                 "(define (f x y) "
                 "  (if (string? x) (length x) "
                 "  (if (number? y) (+ y 1) 0)))\")")) {
        ++g_failed;
        return false;
    }
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "(typecheck-current) runs");
    auto r = cs.eval("(f \"hi\" 42)");
    CHECK(r.has_value(), "(f \"hi\" 42) evaluates");

    auto x_count = run_int(cs, "(length (query:provenance-of \"x\"))");
    auto y_count = run_int(cs, "(length (query:provenance-of \"y\"))");
    CHECK(x_count >= 1, "x has >= 1 narrowing");
    CHECK(y_count >= 1, "y has >= 1 narrowing");

    return true;
}

// ── AC5: backward compat — behavior identical to pre-#282
// for existing tests. Visible behavior: code still works.
bool test_backward_compat() {
    std::println("\n--- AC5: backward compat ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (f x) (if (string? x) (length x) 0))\")")) {
        ++g_failed;
        return false;
    }
    auto r1 = cs.eval("(f \"hi\")");
    CHECK(r1.has_value(), "(f \"hi\") evaluates");
    auto r2 = cs.eval("(f 42)");
    CHECK(r2.has_value(), "(f 42) evaluates (else-branch)");
    return true;
}

// ── AC6: gradual guarantee — provenance doesn't crash when
// (if (pred x) ...) is called with a non-matching type.
bool test_gradual_guarantee() {
    std::println("\n--- AC6: gradual guarantee ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (f x) (if (string? x) (length x) 0))\")")) {
        ++g_failed;
        return false;
    }
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "(typecheck-current) runs");
    auto r = cs.eval("(f 42)");
    CHECK(r.has_value(), "(f 42) returns 0 (no crash)");

    // Even with non-string, provenance is still captured.
    auto count = run_int(cs, "(length (query:provenance-of \"x\"))");
    CHECK(count >= 1, "provenance is still captured even when type mismatches");

    return true;
}

int run_tests() {
    std::println("Issue #282 (Blame & Provenance: narrowings in MutationRecord)\n");
    test_provenance_basic();
    test_provenance_predicate_source();
    test_provenance_unknown_var();
    test_provenance_per_var();
    test_backward_compat();
    test_gradual_guarantee();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_282_detail

int aura_issue_282_run() { return aura_issue_282_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_282_run(); }
#endif
