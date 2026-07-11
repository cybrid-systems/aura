// @category: issue_specific
// @reason: default for test_issue_*.cpp
// test_issue_136.cpp — Verify Issue #136 acceptance criteria
//
// Sub-tasks covered:
//   1. Memory hardening (aura_reset_runtime clears string/float pools)
//   2. Hash performance (open addressing with probing)
//   3. AOT polish (mangle_aot_name robustness)
//   4. Benchmarks (verified by tests/benchmark.py — see BenchCase additions)
//
// The runtime.c tests for memory reset live in
// tests/runtime_test_harness.c. The Aura-level benchmarks live
// in tests/benchmark.py. This C++ test binary covers the parts
// of #136 that need direct C++ verification:
//
//   - mangle_aot_name: thorough name mangling (aura_jit_bridge.cpp)
//   - open addressing correctness: hash find, insert, remove,
//     update, collision, grow via rebuild
//   - runtime reset: g_string_pool / g_float_pool cleared
//
// All tests use C++ APIs directly; no CompilerService dependency.


// mangle_aot_name is defined in aot_mangle.h (extracted from
// aura_jit_bridge.cpp in Issue #136 so tests can call it
// without linking the full AOT bridge).
#include "aot_mangle.h"

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;
namespace aura_issue_136_detail {
using aura::compiler::aot_link_name;
using aura::compiler::mangle_aot_name;


// ═══════════════════════════════════════════════════════════════
// Sub-task 3: AOT name mangling (mangle_aot_name)
// ═══════════════════════════════════════════════════════════════

bool test_mangle_basic() {
    std::println("\n--- Test: mangle_aot_name basic cases ---");
    // Issue #1369: always `_vN` (including `_v0`)
    CHECK(mangle_aot_name("foo", 0) == "foo_0_v0", "foo → foo_0_v0");
    CHECK(mangle_aot_name("bar", 1) == "bar_1_v0", "bar → bar_1_v0");
    // __top__: no disambiguator, always version (#1369)
    CHECK(mangle_aot_name("__top__", 0) == "__top___v0",
          "__top__ → __top___v0 (no disambiguator, always version)");
    CHECK(aot_link_name("__top__", 0) == "__top__",
          "link name for __top__ stays unversioned (runtime.c)");
    return true;
}

bool test_mangle_special_chars() {
    std::println("\n--- Test: mangle_aot_name handles special chars ---");
    // @ . - space (previously handled) — still work
    CHECK(mangle_aot_name("foo@bar", 0) == "foo_bar_0_v0", "foo@bar → foo_bar_0_v0");
    CHECK(mangle_aot_name("a.b.c", 0) == "a_b_c_0_v0", "a.b.c → a_b_c_0_v0");
    // Newly-handled chars
    CHECK(mangle_aot_name("foo?bar", 0) == "foo_bar_0_v0", "foo?bar → foo_bar_0_v0");
    CHECK(mangle_aot_name("a+b", 0) == "a_b_0_v0", "a+b → a_b_0_v0");
    CHECK(mangle_aot_name("x*y/z", 0) == "x_y_z_0_v0", "x*y/z → x_y_z_0_v0");
    CHECK(mangle_aot_name("a&b|c", 0) == "a_b_c_0_v0", "a&b|c → a_b_c_0_v0");
    CHECK(mangle_aot_name("a!b", 0) == "a_b_0_v0", "a!b → a_b_0_v0");
    CHECK(mangle_aot_name("a(b)c", 0) == "a_b_c_0_v0", "a(b)c → a_b_c_0_v0");
    return true;
}

bool test_mangle_collapse_runs() {
    std::println("\n--- Test: mangle_aot_name collapses run of underscores ---");
    // Multiple consecutive special chars → single underscore
    CHECK(mangle_aot_name("foo@@bar", 0) == "foo_bar_0_v0",
          "foo@@bar → foo_bar_0_v0 (run collapsed)");
    CHECK(mangle_aot_name("a...b", 0) == "a_b_0_v0", "a...b → a_b_0_v0 (run collapsed)");
    CHECK(mangle_aot_name("x?!@y", 0) == "x_y_0_v0", "x?!@y → x_y_0_v0 (run collapsed)");
    return true;
}

bool test_mangle_preserves_leading_trailing() {
    std::println("\n--- Test: mangle_aot_name preserves leading/trailing _ ---");
    // Leading / trailing underscores (reserved-name prefix style)
    // must be preserved, not collapsed.
    //
    // The disambiguator is appended at the END (after any
    // trailing underscores), per the aot_mangle.h comment:
    // "Appending at the end produces the same string both
    //  sides, so the .reg.c reference and the LLVM object
    //  symbol line up." So `__init__` becomes `__init___0_v0`
    // (the trailing _ is preserved, then `_0_v0` is appended).
    CHECK(mangle_aot_name("__top__", 0) == "__top___v0",
          "__top__ no disambiguator, always _v0 (#1369)");
    CHECK(mangle_aot_name("__init__", 0) == "__init___0_v0",
          "__init__ → __init___0_v0 (trailing _ preserved)");
    CHECK(mangle_aot_name("_priv", 0) == "_priv_0_v0", "_priv → _priv_0_v0 (leading preserved)");
    CHECK(mangle_aot_name("trail_", 0) == "trail__0_v0",
          "trail_ → trail__0_v0 (trailing _ preserved)");
    return true;
}

bool test_mangle_collision() {
    std::println("\n--- Test: mangle_aot_name disambiguator prevents collision ---");
    // Two different originals with same suffix should NOT collide
    CHECK(mangle_aot_name("foo", 0) != mangle_aot_name("foo", 1), "foo_0_v0 != foo_1_v0");
    CHECK(mangle_aot_name("bar", 0) != mangle_aot_name("bar", 1), "bar_0_v0 != bar_1_v0");
    // Same original with different disambiguator also won't collide
    // (verified by the != above)
    return true;
}

bool test_mangle_empty_and_alphanumeric() {
    std::println("\n--- Test: mangle_aot_name empty + pure alphanumeric ---");
    // Empty string (defensive)
    auto e0 = mangle_aot_name("", 0);
    CHECK(e0 == "_0_v0", "empty → _0_v0 (disambiguator + version)");
    // Pure alphanumeric
    CHECK(mangle_aot_name("foo123", 0) == "foo123_0_v0", "foo123 → foo123_0_v0");
    CHECK(mangle_aot_name("ABC", 0) == "ABC_0_v0", "ABC → ABC_0_v0");
    return true;
}

int run_tests() {
    std::println("═══ Issue #136 verification tests ═══\n");

    std::println("── Sub-task 3: AOT name mangling ──");
    test_mangle_basic();
    test_mangle_special_chars();
    test_mangle_collapse_runs();
    test_mangle_preserves_leading_trailing();
    test_mangle_collision();
    test_mangle_empty_and_alphanumeric();

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
} // namespace aura_issue_136_detail

int aura_issue_136_run() {
    return aura_issue_136_detail::run_tests();
}
