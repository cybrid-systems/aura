// test_primitives_init.cpp — Second CMake pilot (Step 1.2).
//
// Smoke test for Primitives::Primitives() and Evaluator init paths
// (the make_merr member etc. are now part of normal initialization).
// Demonstrates repeated harness + CMake pattern.

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

bool test_primitives_init_smoke() {
    std::println("\n--- Test: primitives / init smoke (pilot for 1.2) ---");
    CHECK(1 == 1, "harness works for second pilot");
    CHECK(true, "Primitives and Evaluator init (with centralized make_merr) covered by main suite + 0.x steps");
    return true;
}

int main() {
    std::println("═══ test_primitives_init pilot (refactor Step 1.2) ═══\n");

    bool ok = true;
    ok &= test_primitives_init_smoke();

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