// Minimal common harness for issue/pilot test binaries (refactor 3.2 dedup pilot).
// Reduces boilerplate in the 3 CMake pilots (test_error_merr, test_primitives_init, test_harness_pilot).
// Usage: #include "issue_test_harness.hpp" then define your test_foo() funcs; the main + CHECK are provided.
#ifndef AURA_ISSUE_TEST_HARNESS_HPP
#define AURA_ISSUE_TEST_HARNESS_HPP

#include <print>

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

// Run the pilot's test_* functions (caller defines them) and report.
// Usage in pilot .cpp:
//   bool test_foo() { ... CHECK(...); return true; }  // etc.
//   int main() { ... test_foo(); ... return run_pilot_tests(); }
static int run_pilot_tests() {
    // Pilots call their test_ funcs before this; we just report.
    std::println("\n--- Results ---");
    std::println("  PASSED: {}", g_passed);
    std::println("  FAILED: {}", g_failed);
    if (g_failed > 0) {
        std::println("  OVERALL: FAIL");
        return 1;
    }
    std::println("  OVERALL: PASS");
    return 0;
}

#endif  // AURA_ISSUE_TEST_HARNESS_HPP
