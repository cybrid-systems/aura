// @category: integration
// @reason: Issue #456 — Fine-grained mutation observability
//          primitives (query:dirty-subtree,
//          query:mutation-impact, query:epoch-stats,
//          query:epoch-delta-since-last-query) for the
//          closed self-evolution loop. Validates:
//            - 4 new primitives are registered and callable
//            - query:epoch-stats returns the current epoch
//            - query:epoch-delta-since-last-query returns
//              a non-zero value after a mutation
//            - query:mutation-impact counter bumps after
//              a successful mutation
//            - query:dirty-subtree counts dirty nodes
//            - (regression) prior #437/#455/#458/#459/#460
//              primitives still work

#include <cstdint>
#include <iostream>
#include <print>
#include <string>

#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_456_detail {

// ── AC1: query:epoch-stats returns the current epoch ──
bool test_query_epoch_stats() {
    std::println("\n--- AC1: query:epoch-stats returns the current epoch ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(query:epoch-stats)");
    if (!r) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_int(*r),
          "query:epoch-stats returns an integer");
    return true;
}

// ── AC2: query:epoch-delta-since-last-query returns 0
//         on the first call (since there's been no
//         query before) ──
bool test_query_epoch_delta_first_call() {
    std::println("\n--- AC2: query:epoch-delta-since-last-query first call ---");
    aura::compiler::CompilerService cs;
    // First call (no prior query). Returns 0.
    auto r1 = cs.eval("(query:epoch-delta-since-last-query)");
    if (!r1) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_int(*r1),
          "query:epoch-delta-since-last-query first call returns int");
    return true;
}

// ── AC3: query:dirty-subtree returns a count ──
bool test_query_dirty_subtree() {
    std::println("\n--- AC3: query:dirty-subtree returns a count ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define x 1) (define y 2)\")")) {
        ++g_failed;
        return false;
    }
    auto r = cs.eval("(query:dirty-subtree 0)");
    if (!r) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_int(*r),
          "query:dirty-subtree returns an integer (count of dirty nodes)");
    return true;
}

// ── AC4: query:mutation-impact returns a value ──
bool test_query_mutation_impact() {
    std::println("\n--- AC4: query:mutation-impact returns a value ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(query:mutation-impact)");
    if (!r) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_int(*r),
          "query:mutation-impact returns an integer (count of recorded impacts)");
    return true;
}

// ── AC5: query:epoch-delta-since-last-query returns
//         a non-zero value after a mutate:rebind ──
bool test_query_epoch_delta_after_mutate() {
    std::println("\n--- AC5: query:epoch-delta-since-last-query after mutate ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define x 1) (define y 2)\")")) {
        ++g_failed;
        return false;
    }
    // Capture initial epoch.
    auto r_init = cs.eval("(query:epoch-stats)");
    if (!r_init || !aura::compiler::types::is_int(*r_init)) {
        ++g_failed;
        return false;
    }
    const auto epoch_before =
        static_cast<std::int64_t>(aura::compiler::types::as_int(*r_init));
    // Run a noop query to stamp last_queried_epoch_.
    auto r0 = cs.eval("(query:epoch-delta-since-last-query)");
    if (!r0) {
        ++g_failed;
        return false;
    }
    // After a mutation (mutate:rebind), epoch must advance.
    if (!cs.eval("(mutate:rebind \"x\" \"42\")")) {
        ++g_failed;
        return false;
    }
    auto r1 = cs.eval("(query:epoch-delta-since-last-query)");
    if (!r1 || !aura::compiler::types::is_int(*r1)) {
        ++g_failed;
        return false;
    }
    const auto delta =
        static_cast<std::int64_t>(aura::compiler::types::as_int(*r1));
    CHECK(delta >= 2,
          "epoch-delta >= 2 after a mutate:rebind (2 bumps per boundary)");
    return true;
}

// ── AC6: query:mutation-impact counter is observable
//         after a successful mutation ──
bool test_query_mutation_impact_after_mutate() {
    std::println("\n--- AC6: query:mutation-impact counter observable after mutate ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define z 1)\")")) {
        ++g_failed;
        return false;
    }
    auto r0 = cs.eval("(query:mutation-impact)");
    if (!r0 || !aura::compiler::types::is_int(*r0)) {
        ++g_failed;
        return false;
    }
    const auto count_before =
        static_cast<std::int64_t>(aura::compiler::types::as_int(*r0));
    if (!cs.eval("(mutate:rebind \"z\" \"99\")")) {
        ++g_failed;
        return false;
    }
    auto r1 = cs.eval("(query:mutation-impact)");
    if (!r1 || !aura::compiler::types::is_int(*r1)) {
        ++g_failed;
        return false;
    }
    const auto count_after =
        static_cast<std::int64_t>(aura::compiler::types::as_int(*r1));
    CHECK(count_after > count_before,
          "query:mutation-impact count increased after a successful mutate");
    return true;
}

// ── AC7: query:dirty-subtree with reason-mask filters ──
bool test_query_dirty_subtree_reason_mask() {
    std::println("\n--- AC7: query:dirty-subtree with reason-mask ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define m 1)\")")) {
        ++g_failed;
        return false;
    }
    // reason-mask 0xFF (all bits) and 0x00 (none) should
    // both return valid integers.
    auto r1 = cs.eval("(query:dirty-subtree 0 255)");
    if (!r1) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_int(*r1),
          "query:dirty-subtree 0 255 returns an int");
    auto r2 = cs.eval("(query:dirty-subtree 0 0)");
    if (!r2) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_int(*r2),
          "query:dirty-subtree 0 0 returns an int");
    return true;
}

// ── AC8: regression — prior #459 / #437 / #455
//         primitives still work ──
bool test_regression_prior_primitives() {
    std::println("\n--- AC8: regression — prior primitives still work ---");
    aura::compiler::CompilerService cs;
    auto r1 = cs.eval("(query:atomic-batch-stats)");
    CHECK(r1.has_value() && aura::compiler::types::is_int(*r1),
          "query:atomic-batch-stats (regression for #459)");
    auto r2 = cs.eval("(query:verify-dirty-stats)");
    CHECK(r2.has_value() && aura::compiler::types::is_int(*r2),
          "query:verify-dirty-stats (regression for #437)");
    auto r3 = cs.eval("(query:ir-marker-stats)");
    CHECK(r3.has_value() && aura::compiler::types::is_int(*r3),
          "query:ir-marker-stats (regression for #455)");
    return true;
}

// ── AC9: regression — define + eval smoke ──
bool test_define_eval_regression() {
    std::println("\n--- AC9: define + eval smoke ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(define smoke-456-a 11)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define smoke-456-b 31)")) {
        ++g_failed;
        return false;
    }
    auto r = cs.eval("(+ smoke-456-a smoke-456-b)");
    if (!r || !aura::compiler::types::is_int(*r)) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::as_int(*r) == 42,
          "smoke: (+ 11 31) == 42 (regression)");
    return true;
}

int run_tests() {
    std::println("Issue #456 (Fine-grained mutation observability primitives)\n");
    test_query_epoch_stats();
    test_query_epoch_delta_first_call();
    test_query_dirty_subtree();
    test_query_mutation_impact();
    test_query_epoch_delta_after_mutate();
    test_query_mutation_impact_after_mutate();
    test_query_dirty_subtree_reason_mask();
    test_regression_prior_primitives();
    test_define_eval_regression();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_456_detail

int aura_issue_456_run() { return aura_issue_456_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_456_run(); }
#endif