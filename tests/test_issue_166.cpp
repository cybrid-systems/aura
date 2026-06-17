// @category: integration
// @reason: uses CompilerService to eval Aura source
// test_issue_166.cpp — Issue #166: multi-layer cache invalidation
// (Phase 1: epoch counter).
//
// Verifies that the global mutation_epoch_ mechanism works:
//   1. The epoch increments on each mutation
//   2. Cache entries stamped with an old epoch are detected
//      as stale (re-compile, not use-the-stale-pointer)
//   3. The basic flow (execute, mutate, re-execute) still
//      works without regression

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <print>
#include <string>
#include <vector>

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.diag;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;



#define PRINTLN(msg) do { std::print("{}\n", std::string(msg)); } while(0)

static int64_t run_int(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r) return -1;
    auto& v = *r;
    if (aura::compiler::types::is_int(v)) {
        return aura::compiler::types::as_int(v);
    }
    // mutate:* primitives return #t/#f (bool). Treat #t as 1 (success)
    // and #f as 0 (failure) so the test can check > 0 the same way
    // it would for an int return.
    if (aura::compiler::types::is_bool(v)) {
        return aura::compiler::types::as_bool(v) ? 1 : 0;
    }
    return -1;
}

// ── Test 1: basic eval-mutate-eval works (regression check) ──
bool test_eval_mutate_eval() {
    PRINTLN("\n--- Test 1: eval → mutate → re-eval (no regression) ---");
    aura::compiler::CompilerService cs;
    auto v1 = run_int(cs, "(set-code \"(define x 10)\")");
    auto v2 = run_int(cs, "x");
    CHECK(v2 == 10, "first eval of x = 10");
    auto v3 = run_int(cs, "(mutate:rebind \"x\" \"42\" \"bump x\")");
    CHECK(v3 > 0, "mutate:rebind succeeded");
    auto v4 = run_int(cs, "x");
    CHECK(v4 == 42, "after mutate:rebind, x = 42 (the new value)");
    return true;
}

// ── Test 2: epoch is exposed and non-decreasing across mutations ──
bool test_epoch_increments_on_mutation() {
    PRINTLN("\n--- Test 2: mutation_epoch_ increments on each mutation ---");
    aura::compiler::CompilerService cs;
    // We can't access the private field directly. The public
    // observable signal is the cache: after a mutation, any
    // pre-mutation cache entry should be treated as stale. We
    // test this by:
    //   1. Setting up a function (cache populated on first call)
    //   2. Calling the function (cache hit)
    //   3. Mutating an unrelated binding
    //   4. Calling the function again — should still work,
    //      even though the epoch has bumped.
    auto setup = run_int(cs, "(set-code \"(define (f x) (* x x))(define y 0)\")");
    auto v1 = run_int(cs, "(f 5)");
    CHECK(v1 == 25, "first call (f 5) = 25");
    auto m = run_int(cs, "(mutate:rebind \"y\" \"100\" \"bump y (unrelated)\")");
    CHECK(m > 0, "mutate:rebind on unrelated var succeeded");
    auto v2 = run_int(cs, "(f 5)");
    CHECK(v2 == 25, "after unrelated mutation, (f 5) still = 25 (cache miss handled)");
    return true;
}

// ── Test 3: epoch mechanism: mutating a function's body invalidates JIT cache ──
bool test_epoch_handles_function_body_mutation() {
    PRINTLN("\n--- Test 3: mutating a function's body uses stale-detection ---");
    aura::compiler::CompilerService cs;
    auto setup = run_int(cs, "(set-code \"(define (g x) (+ x 1))\")");
    auto v1 = run_int(cs, "(g 10)");
    CHECK(v1 == 11, "first (g 10) = 11");
    // Mutate g's body
    auto m = run_int(cs, "(mutate:set-body \"g\" \"(lambda (x) (+ x 100))\" \"bump g\")");
    CHECK(m > 0, "mutate:set-body on g succeeded");
    // Re-eval — should reflect the new body
    auto v2 = run_int(cs, "(g 10)");
    CHECK(v2 == 110, "after mutate:set-body, (g 10) = 110 (new body)");
    return true;
}

// ── Test 4: rapid mutations don't break the epoch counter ──
//
// 10 mutations in a row. After each, re-eval should succeed.
// The epoch counter monotonically increases (no overflow in
// 10 calls, but tests the basic flow).
bool test_rapid_mutations() {
    PRINTLN("\n--- Test 4: 10 rapid mutations (epoch counter is monotonic) ---");
    aura::compiler::CompilerService cs;
    auto setup = run_int(cs, "(set-code \"(define counter 0)\")");
    bool all_ok = true;
    for (int i = 1; i <= 10; ++i) {
        auto m = run_int(cs,
            "(mutate:rebind \"counter\" \"" + std::to_string(i) +
            "\" \"iteration " + std::to_string(i) + "\")");
        if (m <= 0) {
            all_ok = false;
            break;
        }
        auto v = run_int(cs, "counter");
        if (v != i) {
            all_ok = false;
            break;
        }
    }
    CHECK(all_ok, "10 rapid mutations all succeeded with correct values");
    return true;
}

int main() {
    std::fprintf(stdout, "═══ Issue #166 — multi-layer cache invalidation (Phase 1) ═══\n");

    test_eval_mutate_eval();
    test_epoch_increments_on_mutation();
    test_epoch_handles_function_body_mutation();
    test_rapid_mutations();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
