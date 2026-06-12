// test_harness_pilot.cpp — Third (and smallest) CMake pilot (Step 1.3 / 3.2 dedup).
//
// Pure harness demonstration. Shows how cheap it is to add a new
// verification binary once the pattern is established (refactor 1.x).
// Now uses common header.

#include "issue_test_harness.hpp"  // 3.2 dedup pilot (common CHECK + globals)

int main() {
    std::println("═══ test_harness_pilot (Step 1.3 / 3.2 dedup) ═══\n");
    CHECK(2 + 2 == 4, "smallest pilot harness works (via header + run_pilot_tests)");
    return run_pilot_tests();
}