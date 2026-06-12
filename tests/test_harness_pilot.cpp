// test_harness_pilot.cpp — Third (and smallest) CMake pilot (Step 1.3).
//
// Pure harness demonstration. Shows how cheap it is to add a new
// verification binary once the pattern is established (refactor 1.x).

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

int main() {
    std::println("═══ test_harness_pilot (Step 1.3) ═══\n");
    CHECK(2 + 2 == 4, "smallest pilot harness works");
    std::println("\n  PASSED: {}", g_passed);
    std::println("  FAILED: {}", g_failed);
    return g_failed > 0 ? 1 : 0;
}