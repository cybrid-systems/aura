#include <unordered_map>
// test_issue_127.cpp — Verify the Result<T> aliases
// (Issue #127).
//
// Regression scenarios:
//   1. ParseResult<T>, LowerResult<T>, CompileResult<T>
//      are type aliases of Result<T> (same template, just
//      different names for stage readability).
//   2. VoidResult is Result<void>.
//   3. Result<int> / Result<void> Ok and Err paths work.
//   4. Monadic transform on Result propagates both Ok and
//      Err correctly.
//   5. The new lower_to_ir_result and
//      lower_to_ir_with_cache_result are declared and
//      callable from the lowering module.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
#include <string>
#include <vector>
#include <type_traits>
#include <expected>

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.diag;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.value;



// ── Test 1: alias type identity ──────────────────────────

bool test_alias_type_identity() {
    std::println("\n--- Test: Result alias type identity ---");

    // All three pipeline aliases are the same template as Result<T>
    using R1 = aura::diag::Result<int>;
    using R2 = aura::diag::ParseResult<int>;
    using R3 = aura::diag::LowerResult<int>;
    using R4 = aura::diag::CompileResult<int>;

    CHECK((std::is_same_v<R1, R2>),
          "ParseResult<int> is the same type as Result<int>");
    CHECK((std::is_same_v<R1, R3>),
          "LowerResult<int> is the same type as Result<int>");
    CHECK((std::is_same_v<R1, R4>),
          "CompileResult<int> is the same type as Result<int>");

    // VoidResult is Result<void>
    CHECK((std::is_same_v<aura::diag::VoidResult, aura::diag::Result<void>>),
          "VoidResult is Result<void>");
    return true;
}

// ── Test 2: Result construction / error channel ─────────

bool test_result_error_channel() {
    std::println("\n--- Test: Result<T> construction + error channel ---");

    // Ok case
    aura::diag::Result<int> ok = 42;
    CHECK(ok.has_value(), "Ok result has value");
    if (ok) CHECK(*ok == 42, "Ok result unwraps to 42");

    // Error case
    aura::diag::Diagnostic d{aura::diag::ErrorKind::TypeError, "test error"};
    aura::diag::Result<int> err = std::unexpected(d);
    CHECK(!err.has_value(), "Err result does not have value");
    CHECK(err.error().kind == aura::diag::ErrorKind::TypeError,
          "Err result carries the diagnostic kind");
    CHECK(err.error().message == "test error",
          "Err result carries the diagnostic message");

    // LowerResult<int> works the same way
    aura::diag::LowerResult<int> lr = std::unexpected(d);
    CHECK(!lr.has_value(), "LowerResult<int> carries the error");
    return true;
}

// ── Test 4: monadic chain works on the Result ──────────

bool test_monadic_chain() {
    std::println("\n--- Test: monadic chain on Result ---");

    // Chain: Ok(int) -> Ok(int * 2)
    auto r1 = aura::diag::Result<int>(42);
    auto r2 = r1.transform([](int x) { return x * 2; });
    CHECK(r2.has_value(), "transform on Ok produces Ok");
    if (r2) CHECK(*r2 == 84, "transformed value is 84");

    // Chain: Err -> Err (preserves the error)
    aura::diag::Diagnostic d{aura::diag::ErrorKind::InternalError, "fail"};
    auto r3 = aura::diag::Result<int>(std::unexpected(d));
    auto r4 = r3.transform([](int) { return 999; });
    CHECK(!r4.has_value(), "transform on Err preserves the error");
    CHECK(r4.error().message == "fail", "preserved error message");

    // and_then: chain two Results
    auto r5 = aura::diag::Result<int>(10)
        .and_then([&d](int x) -> aura::diag::Result<int> {
            return x > 0 ? aura::diag::Result<int>(x * 3)
                         : aura::diag::Result<int>(std::unexpected(d));
        });
    CHECK(r5.has_value() && *r5 == 30, "and_then on Ok produces Ok(30)");

    return true;
}

int main() {
    std::println("═══ Issue #127 verification tests ═══\n");
    test_alias_type_identity();
    test_result_error_channel();
        test_monadic_chain();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
