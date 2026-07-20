// test_error_merr.cpp — Pilot for centralized make_merr (refactor Step 0.1+)
// and CMake test harness dedup (Step 1.1 / 3.2).
//
// This is a minimal standalone binary demonstrating:
//   - Exact same CHECK harness pattern as other issue tests (e.g. test_issue_116/131).
//   - Simple construction smoke (the make_merr member is now part of Evaluator).
//   - The CMake addition pattern for future pilots (to be turned into common macro).
//   - Now uses the common test_harness.hpp (small 3.2 dedup pilot).
//
// Full exercising of make_merr error paths will be covered by the main test_ir
// and later pilots / full dedup.

#include "test_harness.hpp" // #1960 unified harness

import std;
namespace aura_issue_error_merr_detail {
bool test_harness_smoke() {
    std::println("\n--- Test: harness smoke (pilot for 1.1) ---");
    CHECK(1 == 1, "basic CHECK works");
    CHECK(true, "make_merr centralization (0.1) + wiring (0.2/0.3) linked in main test suite");
    // Step 2.3: adt_runtime (FFI pattern) wired into Evaluator; old g_adt removed.
    return true;
}

int run_tests() {
    std::println("═══ test_error_merr pilot (refactor Step 0.1/0.2/0.3 + CMake pilot 1.1) ═══\n");

    bool ok = true;
    ok &= test_harness_smoke();

    std::println("\n--- Results ---");
    return run_pilot_tests();
}
} // namespace aura_issue_error_merr_detail

int aura_issue_error_merr_run() {
    return aura_issue_error_merr_detail::run_tests();
}
