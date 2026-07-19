// test_issue_1397.cpp - Issue #1397: ASTArena::request_defrag
// atomic CAS newly_set semantics (extended Aura-primitive coverage).
//
// Complementary hard coverage to test_issue_300.cpp AC5. The base
// test exercises 5 sub-steps in one cycle; this file pushes the
// cycle many rounds and verifies that two CompilerService instances
// keep independent flags. Both tests go through the public Aura
// primitive layer (same path as user code) - no C++ internal access
// - so the coverage exercises the Aura->C++ contract directly.
//
// ACs:
//   AC1: 50-cycle request->dup->defrag->request - every cycle
//        produces (true, false), no drift
//   AC2: After (arena:defrag), (arena:request-defrag) returns
//        #t (newly_set); duplicate returns #f; second defrag
//        resets the cycle again
//   AC3: Two independent CompilerService instances have
//        independent flag state (no cross-talk across services)
//   AC4: (arena:defrag-requested?) accurately tracks each
//        transition (post-defrag->#f, post-request->#t, post-
//        defrag again->#f)

#include "test_harness.hpp"
import std;

import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

namespace test_issue_1397_detail {

// Counters live in test_harness.hpp's `namespace aura::test` as
// `inline int g_passed`/`g_failed`; the harness's CHECK macro
// fully-qualifies them (++::aura::test::g_passed/_failed), so we
// just read those at the end and never duplicate the counters
// locally (would shadow + never be incremented).

static void run_ac1_fifty_cycle(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: 50-cycle set->dup->defrag->request consistency ---");
    cs.eval("(set-code \"(define x 1)\")");
    int newly_true = 0;
    int dup_false = 0;
    for (int i = 0; i < 50; ++i) {
        cs.eval("(arena:defrag)");
        auto r1 = cs.eval("(arena:request-defrag)");
        if (r1 && aura::compiler::types::is_bool(*r1) && aura::compiler::types::as_bool(*r1)) {
            ++newly_true;
        }
        auto r2 = cs.eval("(arena:request-defrag)");
        if (r2 && aura::compiler::types::is_bool(*r2) && !aura::compiler::types::as_bool(*r2)) {
            ++dup_false;
        }
    }
    CHECK(newly_true == 50,
          std::format("each of 50 cycles first call returned #t (got {})", newly_true));
    CHECK(dup_false == 50,
          std::format("each of 50 cycles duplicate returned #f (got {})", dup_false));
    cs.eval("(arena:defrag)"); // cleanup
}

static void run_ac2_defrag_clears_flag(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: (arena:defrag) clears the flag - cycle reset ---");
    cs.eval("(set-code \"(define x 1)\")");
    cs.eval("(arena:defrag)");
    auto r1 = cs.eval("(arena:request-defrag)");
    CHECK(r1 && aura::compiler::types::is_bool(*r1) && aura::compiler::types::as_bool(*r1),
          "first request after defrag: #t (newly_set)");
    auto r2 = cs.eval("(arena:request-defrag)");
    CHECK(r2 && aura::compiler::types::is_bool(*r2) && !aura::compiler::types::as_bool(*r2),
          "duplicate: #f (CAS already-set)");
    cs.eval("(arena:defrag)");
    auto r3 = cs.eval("(arena:request-defrag)");
    CHECK(r3 && aura::compiler::types::is_bool(*r3) && aura::compiler::types::as_bool(*r3),
          "after second defrag: #t (defrag clears -> cycle resets)");
    auto r4 = cs.eval("(arena:request-defrag)");
    CHECK(r4 && aura::compiler::types::is_bool(*r4) && !aura::compiler::types::as_bool(*r4),
          "second-cycle duplicate: #f");
}

static void run_ac3_two_services_independent() {
    std::println("\n--- AC3: two CompilerService instances have independent flags ---");
    aura::compiler::CompilerService cs1;
    aura::compiler::CompilerService cs2;
    cs1.eval("(set-code \"(define x 1)\")");
    cs2.eval("(set-code \"(define x 1)\")");

    auto a1 = cs1.eval("(arena:request-defrag)");
    CHECK(a1 && aura::compiler::types::is_bool(*a1) && aura::compiler::types::as_bool(*a1),
          "cs1 first request: #t");
    auto a2 = cs2.eval("(arena:request-defrag)");
    CHECK(a2 && aura::compiler::types::is_bool(*a2) && aura::compiler::types::as_bool(*a2),
          "cs2 first request: #t (independent of cs1)");

    auto a3 = cs1.eval("(arena:request-defrag)");
    CHECK(a3 && aura::compiler::types::is_bool(*a3) && !aura::compiler::types::as_bool(*a3),
          "cs1 duplicate: #f");
    auto a4 = cs2.eval("(arena:request-defrag)");
    CHECK(a4 && aura::compiler::types::is_bool(*a4) && !aura::compiler::types::as_bool(*a4),
          "cs2 duplicate: #f (cs1 dup did not affect cs2)");

    cs1.eval("(arena:defrag)");
    auto a5 = cs1.eval("(arena:request-defrag)");
    CHECK(a5 && aura::compiler::types::is_bool(*a5) && aura::compiler::types::as_bool(*a5),
          "cs1 after own defrag: #t");
    auto a6 = cs2.eval("(arena:request-defrag)");
    CHECK(a6 && aura::compiler::types::is_bool(*a6) && !aura::compiler::types::as_bool(*a6),
          "cs2 still operates independently (no cross-talk)");
}

static void run_ac4_requested_question_tracks(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: (arena:defrag-requested?) accurately tracks each transition ---");
    cs.eval("(set-code \"(define x 1)\")");
    cs.eval("(arena:defrag)");
    auto q0 = cs.eval("(arena:defrag-requested?)");
    CHECK(q0 && aura::compiler::types::is_bool(*q0) && !aura::compiler::types::as_bool(*q0),
          "after defrag: requested? = #f");
    cs.eval("(arena:request-defrag)");
    auto q1 = cs.eval("(arena:defrag-requested?)");
    CHECK(q1 && aura::compiler::types::is_bool(*q1) && aura::compiler::types::as_bool(*q1),
          "after request: requested? = #t");
    cs.eval("(arena:defrag)");
    auto q2 = cs.eval("(arena:defrag-requested?)");
    CHECK(q2 && aura::compiler::types::is_bool(*q2) && !aura::compiler::types::as_bool(*q2),
          "after another defrag: requested? = #f (cycle resets the flag)");
    cs.eval("(arena:defrag)"); // cleanup
}

} // namespace test_issue_1397_detail

int aura_issue_1397_run() {
    using namespace test_issue_1397_detail;
    std::println("=== Issue #1397: ASTArena::request_defrag atomic CAS newly_set semantics "
                 "(extended Aura coverage) ===");
    {
        aura::compiler::CompilerService cs;
        run_ac1_fifty_cycle(cs);
        run_ac2_defrag_clears_flag(cs);
        run_ac4_requested_question_tracks(cs);
    }
    run_ac3_two_services_independent();
    std::println("\n\u2550\u2550\u255d Results: {}/{} passed, {}/{} failed \u2550\u2550\u255d",
                 ::aura::test::g_passed, ::aura::test::g_passed + ::aura::test::g_failed,
                 ::aura::test::g_failed, ::aura::test::g_passed + ::aura::test::g_failed);
    return ::aura::test::g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_1397_run();
}
#endif
