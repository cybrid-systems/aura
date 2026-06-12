// Minimal common harness for issue/pilot test binaries (refactor 3.2 dedup pilot).
// Reduces boilerplate in the 3 CMake pilots (test_error_merr, test_primitives_init, test_harness_pilot).
// Usage: #include "issue_test_harness.hpp" then define your test_foo() funcs; the main + CHECK are provided.

#include <cstdio>
#include <cstdlib>
#include <print>
#include <string>

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

static int run_harness_tests() {
    // Caller should have defined test_*() funcs and called them before including? 
    // For simplicity in pilots: the including .cpp defines the tests and calls run.
    // This header provides the globals + CHECK + a helper main wrapper.
    // Pilots currently inline their main; this is a starting point for dedup.
    return g_failed > 0 ? 1 : 0;
}
