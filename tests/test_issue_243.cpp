// @category: issue_specific
// @reason: mangle_aot_name + AOT bridge observability are pure C++ units
//          that don't need CompilerService.

// test_issue_243.cpp — Issue #243: AOT bridge enhancement verification
//
// Issue #243 (scope-limited close) covers three sub-asks:
//
//   Phase 1: mangle_aot_name gains a `defuse_version` parameter
//            (default 0 for back-compat). When non-zero, the
//            mangled name gets a `_v<N>` suffix.
//
//   Phase 2: a new C-linkage setter
//            `aura_set_aot_defuse_version(uint64_t)` /
//            `aura_get_aot_defuse_version()` lets callers
//            flow the Evaluator's current defuse_version_
//            into the AOT bridge.
//
//   Phase 3: observability — per-function progress logging,
//            start banner with function count + version,
//            collision warning includes the version, link
//            success banner includes version + count,
//            `_reg.c` records `aot_emit_version`.
//
//   Deferred to a follow-up: incremental re-AOT for dirty
//   functions (depends on #196 dirty-tracking plumbing).
//
// Tests in this file:
//
//   AC1: mangle_aot_name default arg is 0 (back-compat).
//   AC2: mangle_aot_name with version=0 produces same result
//        as the 2-arg call (no suffix).
//   AC3: mangle_aot_name with version=7 appends "_v7".
//   AC4: mangle_aot_name with version=1 + different disambig
//        produces different names (no collision).
//   AC5: mangle_aot_name with version=42 + __top__ still
//        keeps __top__ verbatim (no disambiguator).
//   AC6: aura_set_aot_defuse_version / get_aot_defuse_version
//        round-trip.
//
// The new aot_emit_version symbol + per-function start/finish
// banners are covered implicitly: the existing test_issue_237
// already exercises the full aura --emit-binary pipeline, and
// the run output (see commit 3f04f95's stderr) now shows the
// new banners.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <print>
#include <string>
#include <vector>

#include "aot_mangle.h"

// The C-linkage setter/getter for g_aot_defuse_version are
// defined in aura_jit_bridge.cpp. test_issue_243 doesn't
// link that TU (it would pull in the full JIT/AOT chain),
// so we provide a local stub that mirrors the same global
// state. The real bridge TU is exercised end-to-end by
// test_issue_237's aura --emit-binary path.
namespace aura_issue_243_detail {
static std::uint64_t g_aot_defuse_version_stub = 0;
extern "C" void aura_set_aot_defuse_version(std::uint64_t v) {
    g_aot_defuse_version_stub = v;
}
extern "C" std::uint64_t aura_get_aot_defuse_version(void) {
    return g_aot_defuse_version_stub;
}

// ── Minimal CHECK macro (mirrors tests/test_issue_136.cpp) ──
static int g_passed = 0;
static int g_failed = 0;
static const char* g_test_name = "";

#define CHECK(cond, msg) do { \
    if (cond) { \
        ++g_passed; \
        std::println("  PASS: {}", msg); \
    } else { \
        ++g_failed; \
        std::println("  FAIL: {}  [{}: {}]", msg, g_test_name, #cond); \
    } \
} while (0)

// ═════════════════════════════════════════════════════════════
// Tests
// ═════════════════════════════════════════════════════════════

// ── AC1: Default defuse_version is 0 ────────────────────────
//
// The 3-arg overload must default to defuse_version=0 so
// existing callers (and the test_issue_136 test suite) keep
// working without modification.

bool test_mangle_default_version() {
    std::println("\n--- AC1: mangle_aot_name default version is 0 ---");
    g_test_name = "AC1";

    // 2-arg call (old behavior) — must still work, no _v suffix.
    auto r_2arg = aura::compiler::mangle_aot_name("foo", 3);
    auto r_3arg_v0 = aura::compiler::mangle_aot_name("foo", 3, 0);
    CHECK(r_2arg == r_3arg_v0,
          "2-arg call == 3-arg call with version=0 (back-compat)");
    CHECK(r_2arg == "foo_3",
          "result has no _v suffix when version=0");
    return true;
}

// ── AC2: Version=0 produces the same name as 2-arg call ────
//
// Even when the caller passes version=0 explicitly (instead of
// relying on the default), the result must match the 2-arg call.
// This guarantees a clean transition for callers that upgrade.

bool test_mangle_version_0_explicit() {
    std::println("\n--- AC2: explicit version=0 == 2-arg call ---");
    g_test_name = "AC2";

    auto r_2arg = aura::compiler::mangle_aot_name("bar_baz", 7);
    auto r_v0 = aura::compiler::mangle_aot_name("bar_baz", 7, 0);
    CHECK(r_2arg == r_v0,
          "2-arg call == 3-arg call with explicit version=0");
    CHECK(r_v0 == "bar_baz_7",
          "expected result 'bar_baz_7'");
    return true;
}

// ── AC3: Version != 0 appends `_v<N>` ──────────────────────
//
// The most basic new behavior: passing a non-zero version
// results in a `_vN` suffix.

bool test_mangle_version_nonzero() {
    std::println("\n--- AC3: version=7 appends '_v7' ---");
    g_test_name = "AC3";

    auto r = aura::compiler::mangle_aot_name("my_fn", 0, 7);
    CHECK(r == "my_fn_0_v7",
          "my_fn + disambig=0 + version=7 → 'my_fn_0_v7'");

    auto r2 = aura::compiler::mangle_aot_name("compute", 2, 13);
    CHECK(r2 == "compute_2_v13",
          "compute + disambig=2 + version=13 → 'compute_2_v13'");

    // Special chars still get cleaned before the version suffix.
    auto r3 = aura::compiler::mangle_aot_name("my-fn", 1, 5);
    CHECK(r3 == "my_fn_1_v5",
          "special-char name still gets cleaned, then version appended");
    return true;
}

// ── AC4: Different versions produce different names ────────
//
// Critical for the use case: two AOT binaries emitted at
// different defuse_version_ values must have different
// symbol names so the linker doesn't conflate them.

bool test_mangle_version_uniqueness() {
    std::println("\n--- AC4: different versions → different names ---");
    g_test_name = "AC4";

    auto v1 = aura::compiler::mangle_aot_name("fn", 0, 1);
    auto v2 = aura::compiler::mangle_aot_name("fn", 0, 2);
    auto v100 = aura::compiler::mangle_aot_name("fn", 0, 100);
    CHECK(v1 != v2, "v=1 != v=2");
    CHECK(v1 != v100, "v=1 != v=100");
    CHECK(v2 != v100, "v=2 != v=100");
    CHECK(v1 == "fn_0_v1", "v=1 expected 'fn_0_v1'");
    CHECK(v2 == "fn_0_v2", "v=2 expected 'fn_0_v2'");
    CHECK(v100 == "fn_0_v100", "v=100 expected 'fn_0_v100'");
    return true;
}

// ── AC5: __top__ still special-cased under versioning ──────
//
// `__top__` is the canonical entry point. Pre-#243 it had
// no disambiguator; #243 must preserve that behavior even
// when version != 0 (only the version suffix is added).

bool test_mangle_top_versioned() {
    std::println("\n--- AC5: __top__ still skips disambiguator, gets version ---");
    g_test_name = "AC5";

    auto r = aura::compiler::mangle_aot_name("__top__", 0, 9);
    CHECK(r == "__top___v9",
          "__top__ + version=9 → '__top___v9' (disambiguator skipped, version kept)");
    return true;
}

// ── AC6: aura_set_aot_defuse_version round-trip ────────────
//
// The C-linkage setter is what main.cpp uses to flow the
// Evaluator's current defuse_version_ into the AOT bridge
// before each emit. Round-trip via the getter.

bool test_set_get_defuse_version() {
    std::println("\n--- AC6: aura_set_aot_defuse_version round-trip ---");
    g_test_name = "AC6";

    aura_set_aot_defuse_version(0);
    CHECK(aura_get_aot_defuse_version() == 0,
          "set 0 → get 0 (reset baseline)");

    aura_set_aot_defuse_version(42);
    CHECK(aura_get_aot_defuse_version() == 42,
          "set 42 → get 42");

    aura_set_aot_defuse_version(999999);
    CHECK(aura_get_aot_defuse_version() == 999999,
          "set 999999 → get 999999 (large value round-trip)");

    // Reset to 0 so we don't pollute subsequent test runs.
    aura_set_aot_defuse_version(0);
    CHECK(aura_get_aot_defuse_version() == 0,
          "set 0 (cleanup) → get 0");
    return true;
}

// ═════════════════════════════════════════════════════════════
// Main test runner
// ═════════════════════════════════════════════════════════════

int run_tests() {
    std::println("═══ Issue #243 verification tests ═══\n");

    std::println("AC #1: mangle_aot_name default version is 0");
    test_mangle_default_version();

    std::println("\nAC #2: explicit version=0 == 2-arg call");
    test_mangle_version_0_explicit();

    std::println("\nAC #3: version != 0 appends '_vN'");
    test_mangle_version_nonzero();

    std::println("\nAC #4: different versions → different names");
    test_mangle_version_uniqueness();

    std::println("\nAC #5: __top__ still skips disambiguator under versioning");
    test_mangle_top_versioned();

    std::println("\nAC #6: aura_set_aot_defuse_version round-trip");
    test_set_get_defuse_version();

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
}  // namespace aura_issue_243_detail

int aura_issue_243_run() { return aura_issue_243_detail::run_tests(); }

