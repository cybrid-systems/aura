// @category: integration
// @reason: EDSL Aura-layer wrappers + observability primitives
//          (Issue #278: lib/std query/mutate wrappers +
//          mutation-log:summary + ast:stable-refs-valid?)


#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.mutation;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_278_detail {

// Run a snippet and return the integer result. Returns -1 on failure / non-int.
static int64_t run_int(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

// Run a snippet and return the boolean result.
// Returns std::nullopt on failure / non-bool.
static std::optional<bool> run_bool(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    if (!r)
        return std::nullopt;
    auto& v = *r;
    if (aura::compiler::types::is_bool(v))
        return aura::compiler::types::as_bool(v);
    return std::nullopt;
}

// ── AC1: (mutation-log:summary) primitive exists and returns a hash ──
bool test_mutation_log_summary_primitive() {
    std::println("\n--- AC1: (mutation-log:summary) primitive ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (f x) (+ x 1))\")") ||
        !cs.eval("(mutate:rebind \"f\" \"(define (f x) (+ x 2))\" \"bump\")")) {
        ++g_failed;
        return false;
    }
    // Total should be at least 1 (the rebind).
    auto total = run_int(cs, "(hash-ref (mutation-log:summary) \"total\")");
    CHECK(total >= 1, "(mutation-log:summary) total >= 1 after rebind");

    // committed should be >= 1.
    auto committed = run_int(cs, "(hash-ref (mutation-log:summary) \"committed\")");
    CHECK(committed >= 1, "(mutation-log:summary) committed >= 1");

    // rolled-back is initially 0.
    auto rolled = run_int(cs, "(hash-ref (mutation-log:summary) \"rolled-back\")");
    CHECK(rolled == 0, "(mutation-log:summary) rolled-back == 0 (no rollback yet)");

    // by-operator is a hash — should be readable.
    auto rebind_count = run_int(cs,
        "(hash-ref (hash-ref (mutation-log:summary) \"by-operator\") \"rebind\")");
    CHECK(rebind_count >= 1, "by-operator[\"rebind\"] >= 1");

    // last-mutation-id should be > 0 after a rebind.
    auto last_id = run_int(cs, "(hash-ref (mutation-log:summary) \"last-mutation-id\")");
    CHECK(last_id > 0, "last-mutation-id > 0");

    // last-target-node should be >= 0 (any valid node id).
    auto last_tgt = run_int(cs, "(hash-ref (mutation-log:summary) \"last-target-node\")");
    CHECK(last_tgt >= 0, "last-target-node >= 0 (any valid id)");

    // last-operator is a string.
    auto last_op = cs.eval("(hash-ref (mutation-log:summary) \"last-operator\")");
    CHECK(last_op.has_value() && aura::compiler::types::is_string(*last_op),
          "last-operator is a string");

    return true;
}

// ── AC2: (ast:stable-refs-valid?) bulk validity check ──
bool test_ast_stable_refs_valid_primitive() {
    std::println("\n--- AC2: (ast:stable-refs-valid?) bulk validity check ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (f x) (+ x 1)) (define (g y) (* y 2))\")")) {
        ++g_failed;
        return false;
    }

    // Capture two stable-refs.
    if (!cs.eval("(define refs (list (ast:stable-ref 0) (ast:stable-ref 1)))")) {
        ++g_failed;
        return false;
    }

    // Both should be valid initially. ast:ref-valid? takes (id . gen) flat pair.
    auto all_valid = run_bool(cs,
        "(and (ast:ref-valid? (car (car refs)) (cdr (car refs)))"
        "      (ast:ref-valid? (car (cadr refs)) (cdr (cadr refs))))");
    CHECK(all_valid.value_or(false) == true, "both refs valid initially");

    // Bulk check should return a list of (#t #t).
    // ast:stable-refs-valid? takes a list of (id . gen) pairs.
    auto bulk_valid = run_bool(cs,
        "(and (car (ast:stable-refs-valid? refs))"
        "      (cadr (ast:stable-refs-valid? refs)))");
    CHECK(bulk_valid.value_or(false) == true, "(ast:stable-refs-valid?) returns (#t #t) for two valid refs");

    // After a mutation, the captured refs should be stale (gen bumped).
    if (!cs.eval("(mutate:rebind \"f\" \"(define (f x) (+ x 99))\" \"bump-f\")")) {
        ++g_failed;
        return false;
    }
    auto bulk_after = run_bool(cs,
        "(and (car (ast:stable-refs-valid? refs))"
        "      (cadr (ast:stable-refs-valid? refs)))");
    CHECK(bulk_after.value_or(true) == false, "after rebind, all captured refs stale (both #f)");

    // Empty list returns empty list.
    auto empty_list = run_int(cs, "(length (ast:stable-refs-valid? (quote ())))");
    CHECK(empty_list == 0, "empty input → empty output (length 0)");

    // Malformed entries yield #f. (ast:stable-refs-valid? '("a" "b"))
    // Each element is a string (not a pair), so the inner is_int check
    // fails → result is a list of #f.
    auto malformed = run_bool(cs,
        "(if (and (not (car (ast:stable-refs-valid? (quote (\"a\" \"b\")))))"
        "         (not (cadr (ast:stable-refs-valid? (quote (\"a\" \"b\")))))) #t #f)");
    CHECK(malformed.value_or(false) == true, "malformed (non-pair) entries → list of #f");

    return true;
}

// ── AC3: Aura-layer wrappers in lib/std/query.aura work ──
bool test_query_aura_wrappers() {
    std::println("\n--- AC3: lib/std/query.aura Issue #278 wrappers ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/query\" all:)")) {
        ++g_failed;
        return false;
    }

    // (query:defines) returns a list.
    if (!cs.eval("(set-code \"(define (a x) x) (define (b y) y)\")")) {
        ++g_failed;
        return false;
    }
    auto defines_count = run_int(cs, "(length (query:defines))");
    CHECK(defines_count >= 2, "(query:defines) returns >= 2 for 2 defines");

    auto calls_count = run_int(cs, "(length (query:calls))");
    CHECK(calls_count >= 0, "(query:calls) returns a list (>= 0)");

    // (query:defines-by-marker "User") — user-written code, should match defines.
    auto user_defines = run_int(cs, "(length (query:defines-by-marker \"User\"))");
    CHECK(user_defines >= 2, "(query:defines-by-marker \"User\") >= 2");

    // (query:calls-by-marker "User") — no user calls in pure define body.
    auto user_calls = run_int(cs, "(length (query:calls-by-marker \"User\"))");
    CHECK(user_calls >= 0, "(query:calls-by-marker \"User\") returns a list (no crash)");

    // (query:node-marker) returns a string for a known node.
    auto marker = cs.eval("(query:node-marker 0)");
    CHECK(marker.has_value() && aura::compiler::types::is_string(*marker),
          "(query:node-marker) returns a string");

    // (query:ref-counts) returns an int (0 or more).
    auto ref_count = run_int(cs, "(query:ref-counts 0)");
    CHECK(ref_count >= 0, "(query:ref-counts 0) returns a number");

    return true;
}

// ── AC4: Aura-layer wrappers in lib/std/mutate.aura work ──
bool test_mutate_aura_wrappers() {
    std::println("\n--- AC4: lib/std/mutate.aura Issue #278 wrappers ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/mutate\" all:)")) {
        ++g_failed;
        return false;
    }

    // First apply a rebind to get at least 1 mutation.
    if (!cs.eval("(set-code \"(define (f x) (+ x 1))\")") ||
        !cs.eval("(mutate:rebind \"f\" \"(define (f x) (+ x 2))\" \"bump\")")) {
        ++g_failed;
        return false;
    }

    // (mutate:summary) returns a list (pair).
    auto summary_is_list = run_bool(cs, "(pair? (mutate:summary))");
    CHECK(summary_is_list.value_or(false) == true, "(mutate:summary) returns a list");

    // (mutate:operator-count "rebind") >= 1.
    auto rebind_count = run_int(cs, "(mutate:operator-count \"rebind\")");
    CHECK(rebind_count >= 1, "(mutate:operator-count \"rebind\") >= 1");

    // (mutate:operator-count "nonexistent") == 0.
    auto missing_count = run_int(cs, "(mutate:operator-count \"nonexistent-op\")");
    CHECK(missing_count == 0, "(mutate:operator-count \"nonexistent-op\") == 0");

    // (mutate:last-info) returns a list.
    auto last_info_list = run_bool(cs, "(pair? (mutate:last-info))");
    CHECK(last_info_list.value_or(false) == true, "(mutate:last-info) returns a list");

    // (mutate:rolled-back?) returns #f (no rollback happened).
    auto rolled = run_bool(cs, "(mutate:rolled-back?)");
    CHECK(rolled.value_or(true) == false, "(mutate:rolled-back?) == #f when no rollback");

    // (mutate:rollback-rate) returns 0..100.
    auto rate = run_int(cs, "(mutate:rollback-rate)");
    CHECK(rate >= 0 && rate <= 100, "(mutate:rollback-rate) is in 0..100");

    // (mutate:by-operator) returns a hash (use hash-ref on it).
    auto op_count = run_int(cs, "(hash-ref (mutate:by-operator) \"rebind\")");
    CHECK(op_count >= 1, "(mutate:by-operator) hash has rebind >= 1");

    return true;
}

// ── AC5: Backward compat — existing primitives still work ──
bool test_backward_compat() {
    std::println("\n--- AC5: backward compat (existing primitives unchanged) ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (f x) (+ x 1))\")") ||
        !cs.eval("(mutate:rebind \"f\" \"(define (f x) (+ x 2))\" \"bump\")")) {
        ++g_failed;
        return false;
    }

    // (mutation-count) still works.
    auto mc = run_int(cs, "(mutation-count)");
    CHECK(mc >= 1, "(mutation-count) >= 1 (regression check)");

    // (mutation-history node) still works.
    auto hist = cs.eval("(mutation-history 0)");
    CHECK(hist.has_value(), "(mutation-history 0) returns a list");

    // (ast:ref-valid?) still works. Capture BEFORE rebind so the
    // ref is actually stale when checked.
    if (!cs.eval("(define r (ast:stable-ref 0))")) {
        ++g_failed;
        return false;
    }
    auto refv_before = run_bool(cs, "(ast:ref-valid? (car r) (cdr r))");
    CHECK(refv_before.value_or(false) == true, "(ast:ref-valid?) returns #t for fresh ref");

    // Now apply another rebind to make the ref stale.
    if (!cs.eval("(mutate:rebind \"f\" \"(define (f x) (+ x 3))\" \"bump-2\")")) {
        ++g_failed;
        return false;
    }
    auto refv = run_bool(cs, "(ast:ref-valid? (car r) (cdr r))");
    CHECK(refv.value_or(true) == false, "(ast:ref-valid?) returns #f for stale ref (existing behavior)");

    // (ast:summary) still works.
    auto sum = cs.eval("(ast:summary)");
    CHECK(sum.has_value(), "(ast:summary) returns a value (existing primitive)");

    // (dirty:counts) still works.
    auto dc = cs.eval("(dirty:counts)");
    CHECK(dc.has_value(), "(dirty:counts) returns a value (existing primitive)");

    return true;
}

int run_tests() {
    std::println("Issue #278 (EDSL Aura-layer wrappers + observability primitives)\n");
    test_mutation_log_summary_primitive();
    test_ast_stable_refs_valid_primitive();
    test_query_aura_wrappers();
    test_mutate_aura_wrappers();
    test_backward_compat();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_278_detail

int aura_issue_278_run() { return aura_issue_278_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_278_run(); }
#endif
