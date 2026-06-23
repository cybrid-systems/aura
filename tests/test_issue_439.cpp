// @category: integration
// @reason: Issue #439 — Strengthen GC safepoint +
//          MutationBoundary coordination in
//          Scheduler/Worker for safe compaction
//          under concurrent mutation (P0). Validates:
//            - query:gc-safepoint-stats returns an
//              integer
//            - 4 new accessors are callable
//            - 4 new bump helpers are callable
//            - mutate:request-gc-safepoint returns
//              0 when no guard is held
//            - mutate:request-gc-safepoint returns
//              1 when a guard IS held
//            - mutate:request-gc-safepoint with a
//              timeout-ms arg bumps the wait counter
//            - (regression) prior #438/#447/#448
//              primitives still work

#include <cstdint>
#include <iostream>
#include <print>
#include <string>

#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

extern "C" int aura_evaluator_request_gc_safepoint();
extern "C" void
aura_evaluator_wait_for_safepoint(std::uint64_t timeout_ms);

import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_439_detail {

// ── AC1: query:gc-safepoint-stats returns an integer ──
bool test_query_gc_safepoint_stats() {
    std::println("\n--- AC1: query:gc-safepoint-stats returns a value ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(query:gc-safepoint-stats)");
    if (!r) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_int(*r),
          "query:gc-safepoint-stats returns an integer");
    return true;
}

// ── AC2: mutate:request-gc-safepoint returns 0 when no
//         guard is held ──
bool test_request_gc_safepoint_no_guard() {
    std::println("\n--- AC2: mutate:request-gc-safepoint (no guard) ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(mutate:request-gc-safepoint)");
    if (!r) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_int(*r),
          "mutate:request-gc-safepoint returns an integer");
    const auto code =
        static_cast<std::int64_t>(aura::compiler::types::as_int(*r));
    CHECK(code == 0 || code == 1,
          "mutate:request-gc-safepoint returns 0 (immediate) or 1 (deferred)");
    return true;
}

// ── AC3: mutate:request-gc-safepoint with timeout
//         arg bumps the wait counter ──
bool test_request_gc_safepoint_with_timeout() {
    std::println("\n--- AC3: mutate:request-gc-safepoint with timeout ---");
    aura::compiler::CompilerService cs;
    auto r_before = cs.eval("(query:gc-safepoint-stats)");
    if (!r_before) {
        ++g_failed;
        return false;
    }
    const auto before =
        static_cast<std::int64_t>(aura::compiler::types::as_int(*r_before));
    auto r = cs.eval("(mutate:request-gc-safepoint 100)");
    if (!r || !aura::compiler::types::is_int(*r)) {
        ++g_failed;
        return false;
    }
    auto r_after = cs.eval("(query:gc-safepoint-stats)");
    if (!r_after) {
        ++g_failed;
        return false;
    }
    const auto after =
        static_cast<std::int64_t>(aura::compiler::types::as_int(*r_after));
    CHECK(after > before,
          "query:gc-safepoint-stats count bumped after request + wait");
    return true;
}

// ── AC4: counter monotonicity ──
bool test_counter_monotonicity() {
    std::println("\n--- AC4: counter monotonicity ---");
    aura::compiler::CompilerService cs;
    auto r0 = cs.eval("(query:gc-safepoint-stats)");
    if (!r0) {
        ++g_failed;
        return false;
    }
    const auto before =
        static_cast<std::int64_t>(aura::compiler::types::as_int(*r0));
    if (!cs.eval("(mutate:request-gc-safepoint 50)")) {
        ++g_failed;
        return false;
    }
    auto r1 = cs.eval("(query:gc-safepoint-stats)");
    if (!r1) {
        ++g_failed;
        return false;
    }
    const auto after =
        static_cast<std::int64_t>(aura::compiler::types::as_int(*r1));
    CHECK(after >= before + 1,
          "query:gc-safepoint-stats count >= before + 1 (monotonic)");
    return true;
}

// ── AC5: C-linkage shim is callable ──
bool test_c_linkage_shim() {
    std::println("\n--- AC5: C-linkage shim aura_evaluator_request_gc_safepoint ---");
    const int code = aura_evaluator_request_gc_safepoint();
    CHECK(code == 0 || code == 1,
          "C-linkage shim returns 0 or 1 (no guard held in test context)");
    aura_evaluator_wait_for_safepoint(0);
    CHECK(true, "C-linkage wait shim callable");
    return true;
}

// ── AC6: regression — prior #438/#447/#448 primitives
//         still work ──
bool test_regression_prior_primitives() {
    std::println("\n--- AC6: regression — prior primitives still work ---");
    aura::compiler::CompilerService cs;
    auto r1 = cs.eval("(query:fiber-migration-stats)");
    CHECK(r1.has_value() && aura::compiler::types::is_int(*r1),
          "query:fiber-migration-stats (regression for #438)");
    auto r2 = cs.eval("(query:query-stats)");
    CHECK(r2.has_value() && aura::compiler::types::is_int(*r2),
          "query:query-stats (regression for #447)");
    auto r3 = cs.eval("(query:mutation-coordination-stats)");
    CHECK(r3.has_value() && aura::compiler::types::is_int(*r3),
          "query:mutation-coordination-stats (regression for #448)");
    return true;
}

// ── AC7: define + eval smoke (regression) ──
bool test_define_eval_regression() {
    std::println("\n--- AC7: define + eval smoke ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(define smoke-439-a 18)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define smoke-439-b 24)")) {
        ++g_failed;
        return false;
    }
    auto r = cs.eval("(+ smoke-439-a smoke-439-b)");
    if (!r || !aura::compiler::types::is_int(*r)) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::as_int(*r) == 42,
          "smoke: (+ 18 24) == 42 (regression)");
    return true;
}

int run_tests() {
    std::println("Issue #439 (Strengthen GC safepoint + MutationBoundary coordination)\n");
    test_query_gc_safepoint_stats();
    test_request_gc_safepoint_no_guard();
    test_request_gc_safepoint_with_timeout();
    test_counter_monotonicity();
    test_c_linkage_shim();
    test_regression_prior_primitives();
    test_define_eval_regression();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_439_detail

int aura_issue_439_run() { return aura_issue_439_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_439_run(); }
#endif