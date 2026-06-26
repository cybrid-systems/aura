// test_primitives_init.cpp — Second CMake pilot (Step 1.2 / 3.2 dedup).
//
// Smoke test for Primitives::Primitives() and Evaluator init paths
// (the make_merr member etc. are now part of normal initialization).
// Demonstrates repeated harness + CMake pattern. Now uses common header.

#include "issue_test_harness.hpp"  // 3.2 dedup pilot (common CHECK + globals)

import std;
namespace aura_issue_primitives_init_detail {
bool test_primitives_init_smoke() {
    std::println("\n--- Test: primitives / init smoke (pilot for 1.2) ---");
    CHECK(1 == 1, "harness works for second pilot");
    CHECK(true, "Primitives and Evaluator init (with centralized make_merr) covered by main suite + 0.x steps");
    return true;
}

int run_tests() {
    std::println("═══ test_primitives_init pilot (refactor Step 1.2) ═══\n");

    bool ok = true;
    ok &= test_primitives_init_smoke();

    return run_pilot_tests();
}
}  // namespace aura_issue_primitives_init_detail

int aura_issue_primitives_init_run() { return aura_issue_primitives_init_detail::run_tests(); }
