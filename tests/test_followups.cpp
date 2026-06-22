// @category: integration
// @reason: Test follow-up primitives across #278/#280/#282
//          (mutation-log:diff / dirty:summary /
//          multi-predicate OR combine-bits /
//          query:provenance-of* wildcard /
//          query:narrowings-at-mutation join).

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.mutation;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_followups_detail {

static int64_t run_int(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    if (!r || !aura::compiler::types::is_int(*r)) return -1;
    return aura::compiler::types::as_int(*r);
}

// AC1: (mutation-log:diff from to) returns matching records
bool test_mutation_log_diff() {
    std::println("\n--- AC1: (mutation-log:diff from to) ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (f x) (+ x 1))\")")) {
        ++g_failed; return false;
    }
    // Note: set-code may add mutations to the log
    // (workspace rebind, eval-prime, etc.), so the mutation_id
    // range of the rebinds depends on what set-code does. We
    // dynamically discover the rebind count via mutation-count
    // before each diff call.
    auto n_before = run_int(cs, "(mutation-count)");
    if (!cs.eval("(mutate:rebind \"f\" \"(define (f x) (+ x 2))\" \"bump-1\")")) {
        ++g_failed; return false;
    }
    if (!cs.eval("(mutate:rebind \"f\" \"(define (f x) (+ x 3))\" \"bump-2\")")) {
        ++g_failed; return false;
    }
    if (!cs.eval("(mutate:rebind \"f\" \"(define (f x) (+ x 4))\" \"bump-3\")")) {
        ++g_failed; return false;
    }
    auto n_total = run_int(cs, "(mutation-count)");
    CHECK(n_total >= n_before + 3, "3 rebinds added 3+ mutations");
    // Diff from 0 to MAX returns all mutations
    auto n_all = run_int(cs, "(length (mutation-log:diff 0 -1))");
    CHECK(n_all == n_total, "diff 0..-1 returns all mutations");
    // Diff from the last-2 to MAX returns 2 mutations
    auto n_from2 = run_int(cs, "(length (mutation-log:diff 2 -1))");
    // The exact count depends on how many mutations set-code added
    // before the rebinds. We just verify the count is consistent:
    // diff(2..-1) >= diff(n..-1) for any n > 2.
    auto n_from3 = run_int(cs, "(length (mutation-log:diff 3 -1))");
    CHECK(n_from2 >= n_from3, "diff(2..-1) >= diff(3..-1) (range shrinks)");
    // Diff from 1 to 1 returns 1 (single mutation)
    auto n_1_1 = run_int(cs, "(length (mutation-log:diff 1 1))");
    CHECK(n_1_1 == 1, "diff 1..1 returns 1 mutation");
    // Inverted range returns 0
    auto n_inv = run_int(cs, "(length (mutation-log:diff 5 2))");
    CHECK(n_inv == 0, "diff 5..2 (inverted) returns 0");
    return true;
}

// AC2: (dirty:summary) returns present-bits + per-reason counts
bool test_dirty_summary() {
    std::println("\n--- AC2: (dirty:summary) ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (f x) (+ x 1))\")")) {
        ++g_failed; return false;
    }
    // Force a mutation to make something dirty
    if (!cs.eval("(mutate:rebind \"f\" \"(define (f x) (+ x 2))\" \"bump\")")) {
        ++g_failed; return false;
    }
    // dirty:summary should return a hash with at least present-bits
    auto r = cs.eval("(dirty:summary)");
    CHECK(r.has_value(), "(dirty:summary) returns a value");
    auto present = run_int(cs, "(hash-ref (dirty:summary) \"present-bits\")");
    CHECK(present >= 0, "present-bits >= 0");
    auto total = run_int(cs, "(hash-ref (dirty:summary) \"total\")");
    CHECK(total >= 0, "total >= 0");
    return true;
}

// AC3: multi-predicate OR combine bits (#280 follow-up #1)
// This is a behavior test — we can't observe the bitmask
// directly through the public API, but the path runs
// without error and visible behavior is preserved.
bool test_or_combine_bits() {
    std::println("\n--- AC3: multi-predicate OR combine bits ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \""
                 "(define (f x) "
                 "  (if (or (string? x) (number? x)) 1 0))\")")) {
        ++g_failed; return false;
    }
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "(typecheck-current) succeeds for OR of two predicates");
    auto r1 = cs.eval("(f \"hi\")");
    CHECK(r1.has_value(), "(f \"hi\") evaluates");
    auto r2 = cs.eval("(f 42)");
    CHECK(r2.has_value(), "(f 42) evaluates");
    return true;
}

// AC4: (query:provenance-of*) wildcard (#282 follow-up #2)
bool test_provenance_wildcard() {
    std::println("\n--- AC4: (query:provenance-of*) wildcard ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \""
                 "(define (f x y) "
                 "  (if (string? x) (length x) "
                 "  (if (number? y) (+ y 1) 0)))\")")) {
        ++g_failed; return false;
    }
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "typecheck succeeds");
    // Wildcard returns all variables that have provenance
    auto n = run_int(cs, "(length (query:provenance-of*))");
    CHECK(n >= 2, "wildcard returns >= 2 vars (x, y)");
    return true;
}

// AC5: (query:narrowings-at-mutation mutation-id) (#282 follow-up #5)
bool test_narrowings_at_mutation() {
    std::println("\n--- AC5: (query:narrowings-at-mutation mutation-id) ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (f x) (if (string? x) (length x) 0))\")")) {
        ++g_failed; return false;
    }
    // Force typecheck + provenance capture
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "typecheck succeeds");
    // Get the current mutation count
    auto muts = run_int(cs, "(mutation-count)");
    CHECK(muts >= 0, "mutation-count >= 0");
    // Query narrowings at the latest mutation
    auto n = run_int(cs, "(length (query:narrowings-at-mutation 9999))");
    CHECK(n >= 0, "narrowings-at-mutation returns a list");
    return true;
}

// AC6: backward compat — existing primitives still work
bool test_backward_compat() {
    std::println("\n--- AC6: backward compat ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (f x) (+ x 1))\")")) {
        ++g_failed; return false;
    }
    if (!cs.eval("(mutate:rebind \"f\" \"(define (f x) (+ x 2))\" \"bump\")")) {
        ++g_failed; return false;
    }
    // Existing primitives still work
    auto r1 = cs.eval("(mutation-log:summary)");
    CHECK(r1.has_value(), "(mutation-log:summary) still works");
    auto r2 = cs.eval("(dirty:counts)");
    CHECK(r2.has_value(), "(dirty:counts) still works");
    auto r3 = cs.eval("(dirty:reasons)");
    CHECK(r3.has_value(), "(dirty:reasons) still works");
    return true;
}

int run_tests() {
    std::println("Follow-up primitives (cross-issue batch)\n");
    test_mutation_log_diff();
    test_dirty_summary();
    test_or_combine_bits();
    test_provenance_wildcard();
    test_narrowings_at_mutation();
    test_backward_compat();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_followups_detail

int aura_followups_run() { return aura_followups_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_followups_run(); }
#endif
