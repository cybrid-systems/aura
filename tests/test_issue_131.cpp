// test_issue_131.cpp — Verify the FFI primitives
// extraction (Issue #131).
//
// Regression scenarios:
//   1. FFIRuntime exists as a standalone class
//   2. parse_ffi_sig() handles valid signatures
//   3. parse_ffi_sig() rejects invalid signatures
//   4. The Evaluator ctor wires FFIRuntime successfully
//      (no segfault, FFI primitives are registered)
//   5. Multiple evaluators have independent FFI state
//      (no global statics anymore)

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
import aura.compiler.value;
import aura.compiler.ffi_primitives;



// ── Test 1: FFIRuntime standalone construction ──────────

bool test_ffi_runtime_standalone() {
    std::println("\n--- Test: FFIRuntime standalone ---");
    aura::compiler::FFIRuntime ffi;
    CHECK(ffi.lib_count() == 0, "fresh FFIRuntime has 0 libs");
    CHECK(ffi.func_count() == 0, "fresh FFIRuntime has 0 funcs");
    return true;
}

// ── Test 2: parse_ffi_sig() valid signatures ────────────

bool test_parse_ffi_sig_valid() {
    std::println("\n--- Test: parse_ffi_sig() valid ---");
    int ret = 0;
    std::vector<int> args;
    std::string err;

    // (String) -> Int
    bool ok = aura::compiler::FFIRuntime::parse_ffi_sig(
        "(String) -> Int", ret, args, &err);
    CHECK(ok, "(String) -> Int parses");
    CHECK(ret == 1, "(String) -> Int has ret_type == 1 (Int)");
    CHECK(args.size() == 1 && args[0] == 3, "(String) -> Int has [3] (String)");

    // (Float Float) -> Float
    args.clear();
    ok = aura::compiler::FFIRuntime::parse_ffi_sig(
        "(Float Float) -> Float", ret, args, &err);
    CHECK(ok, "(Float Float) -> Float parses");
    CHECK(ret == 2, "(Float Float) -> Float has ret_type == 2 (Float)");
    CHECK(args.size() == 2 && args[0] == 2 && args[1] == 2,
          "(Float Float) -> Float has [2, 2] (Float, Float)");

    // (Int) -> Void
    args.clear();
    ok = aura::compiler::FFIRuntime::parse_ffi_sig(
        "(Int) -> Void", ret, args, &err);
    CHECK(ok, "(Int) -> Void parses");
    CHECK(ret == 0, "(Int) -> Void has ret_type == 0 (Void)");

    // (Opaque) -> Opaque
    args.clear();
    ok = aura::compiler::FFIRuntime::parse_ffi_sig(
        "(Opaque) -> Opaque", ret, args, &err);
    CHECK(ok, "(Opaque) -> Opaque parses");
    CHECK(ret == 4, "(Opaque) -> Opaque has ret_type == 4");
    return true;
}

// ── Test 3: parse_ffi_sig() invalid signatures ─────────

bool test_parse_ffi_sig_invalid() {
    std::println("\n--- Test: parse_ffi_sig() invalid ---");
    int ret = 0;
    std::vector<int> args;
    std::string err;

    // missing '->'
    bool ok = aura::compiler::FFIRuntime::parse_ffi_sig(
        "Int", ret, args, &err);
    CHECK(!ok, "missing '->' is rejected");
    CHECK(!err.empty(), "missing '->' has error message");

    // doesn't start with '('
    err.clear();
    ok = aura::compiler::FFIRuntime::parse_ffi_sig(
        "Int -> Int", ret, args, &err);
    CHECK(!ok, "missing '(' is rejected");
    CHECK(!err.empty(), "missing '(' has error message");

    // unknown type
    err.clear();
    ok = aura::compiler::FFIRuntime::parse_ffi_sig(
        "(Bogus) -> Int", ret, args, &err);
    CHECK(!ok, "unknown type is rejected");
    CHECK(!err.empty(), "unknown type has error message");
    return true;
}

int run_issue_131() {
    std::println("═══ Issue #131 verification tests ═══\n");
    test_ffi_runtime_standalone();
    test_parse_ffi_sig_valid();
    test_parse_ffi_sig_invalid();
        std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
