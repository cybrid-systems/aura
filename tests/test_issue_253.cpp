// @category: integration
// @reason: uses CompilerService to verify linear-move elision observability + pass wiring

// test_issue_253.cpp — Issue #253 scope-limited close:
// Linear-move elision in TypeSpecializationWrap (Phase 3 slice of
// #149 roadmap).
//
// Issue #253's full scope is a multi-week Pass overhaul
// (shape-aware specialization, escape+linear integration,
// cross-function const folding, pass ordering). This
// scope-limited close ships ONE concrete slice:
// TypeSpecializationWrap now elides MoveOp instructions whose
// source has linear_ownership_state == Owned (linear-typed
// single-use values). The elision is safe because MoveOp is
// already a runtime no-op marker for the type checker —
// removing it doesn't change semantics, only the dispatch
// cost.
//
// Test cases:
//   AC1: linear_elide_count starts at 0 on a fresh Evaluator
//   AC2: linear_elide_count is queryable via the
//        (compile:linear-elide-count) Aura primitive
//   AC3: TypeSpecializationWrap::linear_elide_count() accessor
//        exists (compile-time check via compilation)
//   AC4: after normal Aura eval (no linear-typed code emitted
//        by current lowering), linear_elide_count stays at 0
//        (the elision is conservative — only fires when
//        lowering explicitly tags Owned)
//   AC5: existing passes + regression still pass (no breakage
//        from the new elision logic in TypeSpecializationWrap)

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <print>

import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.pass_manager;
import aura.compiler.service;

namespace aura_issue_253_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; std::println("  PASS: {}", msg); } \
    else      { ++g_failed; std::println("  FAIL: {}", msg); } \
} while (0)

#define CHECK_EQ(a, b, msg) do { \
    auto _a = (a); auto _b = (b); \
    if (_a == _b) { ++g_passed; std::println("  PASS: {}  ({} = {})", msg, _a, _b); } \
    else          { ++g_failed; std::println("  FAIL: {}  ({} != {})", msg, _a, _b); } \
} while (0)

bool test_initial_counter_zero() {
    std::println("\n--- AC1: linear_elide_count starts at 0 on a fresh Evaluator ---");
    aura::compiler::CompilerService cs;
    auto snap = cs.snapshot();
    CHECK_EQ(snap.linear_elide_count, 0u, "linear_elide_count == 0");
    return true;
}

bool test_aura_primitive_returns_int() {
    std::println("\n--- AC2: (compile:linear-elide-count) primitive returns an int ---");
    aura::compiler::CompilerService cs;
    auto r1 = cs.eval("(set-code \"(define n (compile:linear-elide-count))\")");
    if (!r1) { std::println("  FAIL: define n failed"); ++g_failed; return false; }
    auto r2 = cs.eval("(eval-current)");
    if (!r2) { std::println("  FAIL: eval-current failed"); ++g_failed; return false; }
    // The primitive returns make_int(...) which is a fixnum.
    // Use pair? as a type probe — make_int is not a pair.
    auto r3 = cs.eval("(pair? n)");
    if (!r3 || !aura::compiler::types::is_bool(*r3)) {
        std::println("  FAIL: (pair? n) returned non-bool (val={})", r3 ? r3->val : -1);
        ++g_failed; return false;
    }
    if (aura::compiler::types::as_bool(*r3)) {
        std::println("  FAIL: (pair? n) returned #t (should be #f for fixnum)");
        ++g_failed; return false;
    }
    CHECK(true, "(compile:linear-elide-count) returns a fixnum (pair? is #f)");
    // Also verify the value is 0 on a fresh evaluator
    auto r4 = cs.eval("(compile:linear-elide-count)");
    if (!r4 || !aura::compiler::types::is_int(*r4)) {
        std::println("  FAIL: primitive returned non-int (val={})", r4 ? r4->val : -1);
        ++g_failed; return false;
    }
    CHECK_EQ(aura::compiler::types::as_int(*r4), std::int64_t{0},
             "primitive returns 0 on a fresh CompilerService");
    return true;
}

bool test_pass_accessor_exists() {
    std::println("\n--- AC3: TypeSpecializationWrap::linear_elide_count() accessor exists ---");
    using namespace aura::compiler;
    TypeSpecializationWrap ts;
    auto n = ts.linear_elide_count();
    CHECK_EQ(n, std::size_t{0},
             "fresh TypeSpecializationWrap has linear_elide_count() == 0 (smoke test)");
    return true;
}

bool test_no_false_elision_on_normal_code() {
    std::println("\n--- AC4: normal Aura eval doesn't bump linear_elide_count ---");
    aura::compiler::CompilerService cs;
    // Run a few normal evals — none should trigger elision
    // because today's lowering doesn't emit linear_ownership_state
    // != 0 (Phase 2 attachment deferred). The elision is
    // conservative: only fires when source has Owned state.
    for (int i = 0; i < 3; ++i) {
        auto r = cs.eval(
            std::string("(set-code \"(define f (lambda (x) (* x x))) (f ")
            + std::to_string(i + 1) + ")\")");
        if (!r) { std::println("  FAIL: set-code failed"); ++g_failed; return false; }
        r = cs.eval("(eval-current)");
        if (!r) { std::println("  FAIL: eval-current failed"); ++g_failed; return false; }
    }
    auto snap = cs.snapshot();
    CHECK_EQ(snap.linear_elide_count, 0u,
             "linear_elide_count == 0 after 3 normal evals (no Owned MoveOp sources)");
    // Primitive should still return 0
    auto r = cs.eval("(compile:linear-elide-count)");
    if (!r || !aura::compiler::types::is_int(*r)) {
        std::println("  FAIL: primitive returned non-int"); ++g_failed; return false;
    }
    CHECK_EQ(aura::compiler::types::as_int(*r), std::int64_t{0},
             "primitive returns 0 after normal evals");
    return true;
}

bool test_no_regression_on_pass_integration() {
    std::println("\n--- AC5: pass integration intact (eval works after elision logic added) ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(set-code \"(define x 42) x\")");
    if (!r) { std::println("  FAIL: set-code failed"); ++g_failed; return false; }
    r = cs.eval("(eval-current)");
    if (!r) { std::println("  FAIL: eval-current failed"); ++g_failed; return false; }
    if (aura::compiler::types::is_int(*r) &&
        aura::compiler::types::as_int(*r) == 42) {
        CHECK(true, "basic eval (x = 42) returns 42 (pass pipeline intact)");
    } else {
        std::println("  FAIL: result is not 42 (val={})", r->val);
        ++g_failed;
    }
    // Lambda + arithmetic — exercises TypeSpecializationWrap's
    // CastOp insertion path (must coexist with new elision logic)
    r = cs.eval("(set-code \"(define (f x) (+ x 1)) (f 5)\")");
    if (!r) { std::println("  FAIL: set-code (f) failed"); ++g_failed; return false; }
    r = cs.eval("(eval-current)");
    if (!r || !aura::compiler::types::is_int(*r) ||
        aura::compiler::types::as_int(*r) != 6) {
        std::println("  FAIL: (f 5) didn't return 6");
        ++g_failed;
    } else {
        CHECK(true, "(f 5) == 6 (CastOp insertion path intact)");
    }
    return true;
}

int run_tests() {
    std::println("═══ Issue #253 — linear-move elision observability (scope-limited) ═══\n");
    test_initial_counter_zero();
    test_aura_primitive_returns_int();
    test_pass_accessor_exists();
    test_no_false_elision_on_normal_code();
    test_no_regression_on_pass_integration();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
}  // namespace aura_issue_253_detail

int aura_issue_253_run() { return aura_issue_253_detail::run_tests(); }

