// @category: integration
// @reason: Issue #660 — cache_define bundle ordering when Variable hits cache
//  (multi-define pre-existing bug). The bundle[0] issue (cache-lookup
//  additions interleaved with the define's own lambda) was fixed across
//  commits fe30663b (Option 1 step 1: name registry infrastructure) +
//  476ce10d (cache lookup uses name instead of bundle[0] for lambda_fid)
//  + 9d632679 (cleanup) + c00a9c1a (remap_func_ids underflow fix) +
//  fef8898d (preserve ir_mod in bundle for env binding). This test
//  ships as the regression net for the bundle[0] fix.
//
// AC1: 2-define, second depends on first via direct call → returns 6
// AC2: 2-define, second uses (+ x 1) arg to first → returns 42
// AC3: 2-define, second uses inline lambda calling first → returns 6
//      (the bug initially manifested as `invalid closure` here)
// AC4: 1-define baseline (regression) → returns 6
// AC5: 3-define chain (fn-b, fn-c both call fn-a) → fn-b=6, fn-c=10
// AC6: module require chain (test_module_chain_5 still passes)
// AC7: redefine after define (cache eviction path)

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_660_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

// Eval a snippet through CompilerService and check the int result.
// The define + body combo drives cache_define for each define,
// exercising the bundle[0] path that was the original bug.

static void run_ac1_two_define_direct(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: 2-define direct call (fn-b 20) = 6 ---");
    // Fresh service so cache_define is exercised from a clean slate.
    auto r = cs.eval("(define (fn-a x) (* x 2))"
                     "(define (fn-b x) (fn-a 3))"
                     "(fn-b 20)");
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 6,
          "fn-b(20) returns 6 (was 40 = fn-a's body, BEFORE #660 fix)");
}

static void run_ac2_two_define_arg(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: 2-define with (+ x 1) arg (fn-b 20) = 42 ---");
    // Force a fresh service to avoid AC1's cache pollution confusing this.
    auto r = cs.eval("(begin"
                     "  (define (fn-a x) (* x 2))"
                     "  (define (fn-b x) (fn-a (+ x 1)))"
                     "  (fn-b 20))");
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 42,
          "fn-b(20) returns 42 (was 40 BEFORE #660 fix)");
}

static void run_ac3_inline_lambda(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: 2-define inline lambda (fn-b 20) = 6 ---");
    auto r = cs.eval("(begin"
                     "  (define (fn-a x) (* x 2))"
                     "  (define (fn-b x) ((lambda (y) (fn-a y)) 3))"
                     "  (fn-b 20))");
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 6,
          "fn-b(20) returns 6 (was 'invalid closure' BEFORE #660 fix)");
}

static void run_ac4_single_define_baseline(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: 1-define baseline (fn-a 3) = 6 ---");
    auto r = cs.eval("(begin"
                     "  (define (fn-a x) (* x 2))"
                     "  (fn-a 3))");
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 6,
          "single define still works (regression)");
}

static void run_ac5_three_define_chain(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: 3-define chain (fn-b + fn-c both depend on fn-a) ---");
    auto r = cs.eval("(begin"
                     "  (define (fn-a x) (* x 2))"
                     "  (define (fn-b x) (fn-a 3))"
                     "  (define (fn-c x) (fn-a x))"
                     "  (fn-b 20))");
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 6,
          "fn-b(20) = 6 (not 40)");
    auto r2 = cs.eval("(fn-c 5)");
    CHECK(r2 && aura::compiler::types::is_int(*r2) && aura::compiler::types::as_int(*r2) == 10,
          "fn-c(5) = 10 (not 6)");
}

static void run_ac6_redefine(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC6: redefine after define ---");
    // Define fn-a, use it, then redefine and re-use. The redefine path
    // invalidates the JIT cache (#660 follow-up) and reruns the bundle
    // build with the new lambda. Make sure the new lambda wins.
    auto r = cs.eval("(begin"
                     "  (define (fn-a x) (* x 2))"
                     "  (define (fn-b x) (fn-a 5))"
                     "  (let ((v1 (fn-b 10)))"
                     "    (define (fn-a x) (+ x 100))"
                     "    (let ((v2 (fn-b 10)))"
                     "      (+ v1 v2))))");
    // v1 = fn-b(10) = fn-a(5) = 10, after redefine fn-a(5) = 105,
    // so fn-b(10) = 105. v1+v2 = 10+105 = 115.
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 115,
          "redefine path: v1=10, v2=105, sum=115");
}

} // namespace aura_issue_660_detail

int main() {
    using namespace aura_issue_660_detail;

    // Each AC uses a fresh CompilerService to keep cache_define cleanly
    // exercised from its baseline (0 entries) for each scenario. This
    // isolates each AC from the others and makes failures debuggable.
    {
        aura::compiler::CompilerService cs;
        run_ac1_two_define_direct(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac2_two_define_arg(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac3_inline_lambda(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac4_single_define_baseline(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac5_three_define_chain(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac6_redefine(cs);
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
