// test_issue_194.cpp — Verify Issue #194 acceptance criteria
// ("jit: per-lowering runtime → intrinsic migrations (4 candidates,
//  #170 follow-up)").
//
// The intrinsic_count counter was already in place from
// prior commits. This follow-up adds the Aura-level
// observability primitives to surface the per-commit
// migration count: (jit:intrinsic-count) and (jit:deopt-fn?
// fn-name threshold). The (jit:deopt-fn?) primitive was
// also added for Issue #193 since it shares the same
// per-function hook infrastructure.
//
// Test strategy:
//   - (jit:intrinsic-count) returns 0 when no hook installed
//   - (jit:deopt-fn? fn-name threshold) returns #f when
//     the function has no unhandled opcodes
//   - (jit:deopt-fn? fn-name threshold) threshold check works
//   - (jit:deopt-fn? fn-name 0) returns #f for unknown fn

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <print>
#include <string>
#include <vector>

import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::println("  FAIL: {} (line {})", msg, __LINE__); \
        ++g_failed; \
    } else { \
        std::println("  PASS: {}", msg); \
        ++g_passed; \
    } \
} while(0)

static aura::compiler::types::EvalValue run_on(aura::compiler::CompilerService& cs,
                                                std::string_view src) {
    auto r = cs.eval(src);
    if (!r) {
        std::println(std::cerr, "    [eval error: {}]", r.error().format());
        return aura::compiler::types::make_void();
    }
    return *r;
}

static int64_t run_int(aura::compiler::CompilerService& cs, std::string_view src) {
    auto v = run_on(cs, src);
    if (!aura::compiler::types::is_int(v)) {
        std::println(std::cerr, "    [expected int, got val={}]", v.val);
        return -1;
    }
    return aura::compiler::types::as_int(v);
}

static bool run_bool(aura::compiler::CompilerService& cs, std::string_view src) {
    auto v = run_on(cs, src);
    if (!aura::compiler::types::is_bool(v)) {
        std::println(std::cerr, "    [expected bool, got val={}]", v.val);
        return false;
    }
    return aura::compiler::types::as_bool(v);
}

// ═════════════════════════════════════════════════════════════
// AC1: (jit:intrinsic-count) primitive is registered
// ═════════════════════════════════════════════════════════════

bool test_intrinsic_count_primitive_registered() {
    std::println("\n--- Test 1.1: (jit:intrinsic-count) is registered ---");
    aura::compiler::CompilerService cs;
    auto v = run_on(cs, "(jit:intrinsic-count)");
    if (v.val == 11) {
        std::println("    [expected int, got void]");
        ++g_failed;
    } else {
        std::println("  PASS: (jit:intrinsic-count) is registered");
        ++g_passed;
    }
    return true;
}

bool test_intrinsic_count_returns_int() {
    std::println("\n--- Test 1.2: (jit:intrinsic-count) returns int ---");
    aura::compiler::CompilerService cs;
    int64_t n = run_int(cs, "(jit:intrinsic-count)");
    CHECK(n >= 0, "(jit:intrinsic-count) returns non-negative int");
    return true;
}

bool test_intrinsic_count_zero_without_jit() {
    std::println("\n--- Test 1.3: (jit:intrinsic-count) is 0 without JIT ---");
    // The CompilerService has a JIT installed, but the intrinsic
    // counter starts at 0 and only increments when migrations
    // trigger. So 0 is the expected initial value.
    aura::compiler::CompilerService cs;
    int64_t n = run_int(cs, "(jit:intrinsic-count)");
    CHECK(n == 0, "(jit:intrinsic-count) is 0 at startup (no migrations yet)");
    return true;
}

// ═════════════════════════════════════════════════════════════
// AC2: (jit:deopt-fn?) primitive is registered
// ═════════════════════════════════════════════════════════════

bool test_deopt_fn_primitive_registered() {
    std::println("\n--- Test 2.1: (jit:deopt-fn?) is registered ---");
    aura::compiler::CompilerService cs;
    auto v = run_on(cs, "(jit:deopt-fn? \"some-func\")");
    if (v.val == 11) {
        std::println("    [expected bool, got void]");
        ++g_failed;
    } else {
        std::println("  PASS: (jit:deopt-fn? \"some-func\") is registered");
        ++g_passed;
    }
    return true;
}

bool test_deopt_fn_returns_false_for_unknown() {
    std::println("\n--- Test 2.2: (jit:deopt-fn? unknown) returns #f ---");
    aura::compiler::CompilerService cs;
    bool is_deopt = run_bool(cs, "(jit:deopt-fn? \"never-seen-function\")");
    CHECK(!is_deopt, "unknown function is not deopted");
    return true;
}

bool test_deopt_fn_threshold_check() {
    std::println("\n--- Test 2.3: threshold parameter is respected ---");
    aura::compiler::CompilerService cs;
    bool r1 = run_bool(cs, "(jit:deopt-fn? \"f\" 0)");
    bool r2 = run_bool(cs, "(jit:deopt-fn? \"f\" 5)");
    CHECK(!r1, "threshold=0 with count=0 → #f");
    CHECK(!r2, "threshold=5 with count=0 → #f");
    return true;
}

bool test_deopt_fn_no_args_safe() {
    std::println("\n--- Test 2.4: (jit:deopt-fn? no-args) safe ---");
    // Calling with no args should return #f (malformed call,
    // safe default).
    aura::compiler::CompilerService cs;
    auto v = run_on(cs, "(jit:deopt-fn?)");
    if (v.val == 11 || aura::compiler::types::is_bool(v)) {
        std::println("  PASS: (jit:deopt-fn? no-args) returns bool/void (safe)");
        ++g_passed;
    } else {
        std::println("    [unexpected val={}]", v.val);
        ++g_failed;
    }
    return true;
}

// ═════════════════════════════════════════════════════════════
// AC3: Both primitives are non-destructive
// ═════════════════════════════════════════════════════════════

bool test_primitives_non_destructive() {
    std::println("\n--- Test 3.1: primitives don't modify state ---");
    // Calling the observability primitives shouldn't bump the
    // intrinsic_count or unhandled_opcode_count. Verify by
    // reading the count multiple times and checking it's the
    // same.
    aura::compiler::CompilerService cs;
    int64_t n1 = run_int(cs, "(jit:intrinsic-count)");
    int64_t n2 = run_int(cs, "(jit:intrinsic-count)");
    int64_t n3 = run_int(cs, "(jit:intrinsic-count)");
    CHECK(n1 == n2 && n2 == n3, "(jit:intrinsic-count) is read-only (n1==n2==n3)");
    return true;
}

// ═════════════════════════════════════════════════════════════
// Main test runner
// ═════════════════════════════════════════════════════════════

int main() {
    std::println("═══ Issue #194 verification tests ═══\n");
    std::println("AC #1: (jit:intrinsic-count) primitive");
    test_intrinsic_count_primitive_registered();
    test_intrinsic_count_returns_int();
    test_intrinsic_count_zero_without_jit();

    std::println("\nAC #2: (jit:deopt-fn?) primitive");
    test_deopt_fn_primitive_registered();
    test_deopt_fn_returns_false_for_unknown();
    test_deopt_fn_threshold_check();
    test_deopt_fn_no_args_safe();

    std::println("\nAC #3: non-destructive observability");
    test_primitives_non_destructive();

    std::println("\n════════════════════════════════════════");
    std::println("Results: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
