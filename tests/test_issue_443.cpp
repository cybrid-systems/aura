// @category: integration
// @reason: Issue #443 — External simulator tool-calling
//          and structured result parsing primitives (P0).
//          Validates:
//            - query:verify-tool-stats returns an integer
//            - verify:run-external-sim returns a string
//            - cache hit bumps verify_tool_cache_hits_total
//            - verify:parse-coverage marks nodes dirty
//            - verify:parse-failures marks nodes dirty
//            - (regression) prior #439/#447/#448 primitives
//              still work


#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_443_detail {

// ── AC1: query:verify-tool-stats returns an integer ──
bool test_query_verify_tool_stats() {
    std::println("\n--- AC1: query:verify-tool-stats returns a value ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(query:verify-tool-stats)");
    if (!r) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_int(*r), "query:verify-tool-stats returns an integer");
    return true;
}

// ── AC2: verify:run-external-sim returns a string ──
bool test_run_external_sim() {
    std::println("\n--- AC2: verify:run-external-sim returns a string ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define v 1)\")")) {
        ++g_failed;
        return false;
    }
    auto r = cs.eval("(verify:run-external-sim \"iverilog -o /tmp/out test.v\")");
    if (!r) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_string(*r), "verify:run-external-sim returns a string");
    return true;
}

// ── AC3: cache hit on second call ──
bool test_cache_hit() {
    std::println("\n--- AC3: cache hit on second call ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define w 1)\")")) {
        ++g_failed;
        return false;
    }
    // First call: cache miss.
    auto r0 = cs.eval(R"aur((verify:run-external-sim "test_cmd"))aur");
    if (!r0) {
        ++g_failed;
        return false;
    }
    // Capture cache-hits baseline.
    auto r_stats_before = cs.eval("(query:verify-tool-stats)");
    if (!r_stats_before) {
        ++g_failed;
        return false;
    }
    const auto before = static_cast<std::int64_t>(aura::compiler::types::as_int(*r_stats_before));
    // Second call: should be a cache hit.
    auto r1 = cs.eval(R"aur((verify:run-external-sim "test_cmd"))aur");
    if (!r1) {
        ++g_failed;
        return false;
    }
    auto r_stats_after = cs.eval("(query:verify-tool-stats)");
    if (!r_stats_after) {
        ++g_failed;
        return false;
    }
    const auto after = static_cast<std::int64_t>(aura::compiler::types::as_int(*r_stats_after));
    CHECK(after > before, "cache hit bumps query:verify-tool-stats counter");
    return true;
}

// ── AC4: verify:parse-coverage marks nodes dirty ──
bool test_parse_coverage() {
    std::println("\n--- AC4: verify:parse-coverage marks nodes dirty ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define a 1) (define b 2) (define c 3)\")")) {
        ++g_failed;
        return false;
    }
    auto r = cs.eval("(verify:parse-coverage \"0 hole_a\n2 hole_c\n\")");
    if (!r || !aura::compiler::types::is_int(*r)) {
        ++g_failed;
        return false;
    }
    const auto count = static_cast<std::int64_t>(aura::compiler::types::as_int(*r));
    CHECK(count == 2, "verify:parse-coverage marks 2 nodes dirty (line-based format)");
    return true;
}

// ── AC5: verify:parse-failures marks nodes dirty ──
bool test_parse_failures() {
    std::println("\n--- AC5: verify:parse-failures marks nodes dirty ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define p 1) (define q 2)\")")) {
        ++g_failed;
        return false;
    }
    auto r = cs.eval("(verify:parse-failures \"1 fail_msg\")");
    if (!r || !aura::compiler::types::is_int(*r)) {
        ++g_failed;
        return false;
    }
    const auto count = static_cast<std::int64_t>(aura::compiler::types::as_int(*r));
    CHECK(count == 1, "verify:parse-failures marks 1 node dirty");
    return true;
}

// ── AC6: parse error bumps parse_errors counter ──
bool test_parse_error() {
    std::println("\n--- AC6: parse error bumps parse_errors counter ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define x 1)\")")) {
        ++g_failed;
        return false;
    }
    auto r0 = cs.eval("(query:verify-tool-stats)");
    if (!r0) {
        ++g_failed;
        return false;
    }
    const auto before = static_cast<std::int64_t>(aura::compiler::types::as_int(*r0));
    // Empty line that doesn't start with an integer
    // → parse error.
    auto r = cs.eval("(verify:parse-coverage \"not_a_number\")");
    if (!r) {
        ++g_failed;
        return false;
    }
    auto r1 = cs.eval("(query:verify-tool-stats)");
    if (!r1) {
        ++g_failed;
        return false;
    }
    const auto after = static_cast<std::int64_t>(aura::compiler::types::as_int(*r1));
    CHECK(after > before, "parse error bumps query:verify-tool-stats counter");
    return true;
}

// ── AC7: regression — prior #439/#447/#448 primitives
//         still work ──
bool test_regression_prior_primitives() {
    std::println("\n--- AC7: regression — prior primitives still work ---");
    aura::compiler::CompilerService cs;
    auto r1 = cs.eval("(query:gc-safepoint-stats)");
    CHECK(r1.has_value() && aura::compiler::types::is_int(*r1),
          "query:gc-safepoint-stats (regression for #439)");
    auto r2 = cs.eval("(query:query-stats)");
    CHECK(r2.has_value() && aura::compiler::types::is_int(*r2),
          "query:query-stats (regression for #447)");
    auto r3 = cs.eval("(query:mutation-coordination-stats)");
    CHECK(r3.has_value() && aura::compiler::types::is_int(*r3),
          "query:mutation-coordination-stats (regression for #448)");
    return true;
}

// ── AC8: define + eval smoke (regression) ──
bool test_define_eval_regression() {
    std::println("\n--- AC8: define + eval smoke ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(define smoke-443-a 21)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define smoke-443-b 21)")) {
        ++g_failed;
        return false;
    }
    auto r = cs.eval("(+ smoke-443-a smoke-443-b)");
    if (!r || !aura::compiler::types::is_int(*r)) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::as_int(*r) == 42, "smoke: (+ 21 21) == 42 (regression)");
    return true;
}

int run_tests() {
    std::println("Issue #443 (External simulator tool-calling + structured result parsing)\n");
    test_query_verify_tool_stats();
    test_run_external_sim();
    test_cache_hit();
    test_parse_coverage();
    test_parse_failures();
    test_parse_error();
    test_regression_prior_primitives();
    test_define_eval_regression();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_443_detail

int aura_issue_443_run() {
    return aura_issue_443_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_443_run();
}
#endif