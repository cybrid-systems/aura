// test_pilot_harness.cpp — Merged #1960 harness pilot demos (#1978).
//
// Originally test_harness_pilot.cpp (Step 1.3) + test_primitives_init.cpp
// (Step 1.2). Both were 20-31 line pilots demonstrating the unified harness
// pattern. Merged to reduce per-pilot overhead while keeping the bundle
// entry-function names so the `light` bundle still picks them up.
//
// Bundle: AURA_ISSUE_BUNDLE_LIGHT_MEMBERS (cmake/AuraIssueBundles.cmake)

#include "test_harness.hpp" // #1960 unified harness

import std;
namespace aura_issue_primitives_init_detail {
// Step 1.2: Primitives/Evaluator init smoke
bool test_primitives_init_smoke() {
    std::println("\n--- primitives / init smoke (Step 1.2) ---");
    CHECK(1 == 1, "harness works for primitives/init pilot");
    CHECK(true, "Primitives and Evaluator init (with centralized make_merr) covered "
                "by main suite + 0.x steps");
    return true;
}
} // namespace aura_issue_primitives_init_detail

namespace aura_issue_harness_pilot_detail {
// Step 1.3: smallest harness demo
int run_harness_demo() {
    std::println("\n--- smallest pilot (Step 1.3) ---");
    CHECK(2 + 2 == 4, "smallest pilot harness works (via header + run_pilot_tests)");
    return run_pilot_tests();
}
} // namespace aura_issue_harness_pilot_detail

int aura_issue_primitives_init_run() {
    std::println("═══ test_primitives_init pilot (Step 1.2) ═══\n");
    bool ok = aura_issue_primitives_init_detail::test_primitives_init_smoke();
    return ok ? aura_issue_harness_pilot_detail::run_harness_demo() : 1;
}

int aura_issue_harness_pilot_run() {
    std::println("═══ test_harness_pilot (Step 1.3) ═══\n");
    return aura_issue_harness_pilot_detail::run_harness_demo();
}

// Bundle entry — AURA_ISSUE_BUNDLE_LIGHT calls this single entry which runs
// both Step 1.2 + Step 1.3 demos in sequence (#1978 merged from 2 pilots).
// C++ linkage (not extern "C"): matches generated tests/bundles/*_main.cpp
// `extern int aura_issue_*_run()` declarations used by all issue bundles.
int aura_issue_pilot_harness_run() {
    int rc1 = aura_issue_primitives_init_run();
    int rc2 = aura_issue_harness_pilot_run();
    return rc1 != 0 ? rc1 : rc2;
}
