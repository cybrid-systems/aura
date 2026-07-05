// test_harness.hpp — unified C++ test harness for Aura
// test_issue_*.cpp files.
//
// Background: the Aura test suite has 80+ test_issue_*.cpp
// files, each with its own ad-hoc main() that runs a list
// of test functions and reports pass/fail counts. The pattern
// varies: most use `g_passed` / `g_failed` counters with a
// `CHECK(cond, msg)` macro; a few use `test_1_xxx()` style
// functions; test_issue_220 has a slightly different
// standalone variant.
//
// This header provides a SINGLE entry point that works
// for all the patterns:
//
//   - `CHECK(cond, msg)` — backward-compat macro that
//     increments the global counter. The existing tests
//     already use this; they don't need to change.
//
//   - `register_test(name, fn)` — registers a test
//     function (taking no args, returning void). Use this
//     if you want a structured runner with per-test
//     reporting.
//
//   - `RUN_ALL_TESTS()` — replaces the per-file main().
//     Runs all registered tests (or, if none are registered,
//     runs the g_passed/g_failed counter-based tests that
//     the file already exercises). Reports per-test
//     pass/fail, then a summary line. Returns 0 on full
//     pass, 1 on any failure.
//
//   - `g_passed` / `g_failed` — global counters, available
//     for existing tests that use them. Initialized to 0.
//
// Usage:
//
//   // File: tests/test_issue_NNN.cpp
//   #include "test_harness.hpp"
//   // ... test functions, possibly with CHECK(...) ...
//   int main() { return RUN_ALL_TESTS(); }
//
// Or, for tests that already use `g_passed`/`g_failed`
// without registering individual tests, the harness still
// works: `RUN_ALL_TESTS()` reports the final counter
// values.
//
// The header is intentionally lightweight: no Google Test,
// no Catch2, no dependencies. It compiles with the same
// -Wall -Wextra flags as the rest of the suite.

#ifndef AURA_TEST_HARNESS_HPP
#define AURA_TEST_HARNESS_HPP

#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace aura::test {

// Global pass/fail counters. Available for tests that
// use the legacy CHECK() macro pattern. Initialized to 0
// at static-init time.
inline int g_passed = 0;
inline int g_failed = 0;

// One registered test. We use a function pointer (not
// std::function) to avoid pulling in <functional>, which
// conflicts with the std module import in module-unit
// test files. All TEST() bodies are zero-arg void
// functions, so a plain pointer is sufficient.
using TestFn = void (*)();
struct TestCase {
    const char* name;
    TestFn fn;
};

// Registry of all tests added via register_test() in the
// current TU. Each TU that includes this header gets its
// own static registry.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int register_test(const char* name, TestFn fn) {
    registry().push_back(TestCase{name, fn});
    return 0; // dummy return so it can be used in static init
}

} // namespace aura::test

// ── CHECK / EXPECT macros (backward-compat) ───────────────────
//
// Tests in the Aura suite use one of two patterns:
//   - CHECK(cond, msg) — increments g_passed/g_failed
//   - assert(cond) — direct assert, no reporting
//
// The CHECK macro below is the standard form. Tests that
// already use it don't need to change.
//
// Issue #226 follow-up: store _check_msg as an owning
// std::string (by value, not const-reference). The previous
// `const auto&` bound a reference to the const char* returned
// by `(...).c_str()`, but the underlying temporary std::string
// is not bound to anything that extends its lifetime — so by
// the time std::println's format_args dereferences the
// stored pointer (in std::formatter<char const*, char>::format
// via strlen / memcpy), the buffer is freed and ASan flags a
// heap-use-after-free. Copying into a local std::string
// makes the message own its data for the duration of the
// macro body. Accepts both const char* and std::string
// arguments thanks to std::string's converting constructor.
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        const std::string _check_msg = (msg);                                                      \
        if (!(cond)) {                                                                             \
            std::println(std::cerr, "  FAIL: {} (line {})", _check_msg, __LINE__);                 \
            ++::aura::test::g_failed;                                                              \
        } else {                                                                                   \
            std::println("  PASS: {}", _check_msg);                                                \
            ++::aura::test::g_passed;                                                              \
        }                                                                                          \
    } while (0)

// Optional: an EXPECT_* family for new tests that want
// richer reporting (currently mapped to the same counter
// machinery; future cycles can extend these to capture
// file/line for richer failure reports).
#define EXPECT_TRUE(cond) CHECK((cond), #cond)
#define EXPECT_FALSE(cond) CHECK(!(cond), "!(" #cond ")")
#define EXPECT_EQ(a, b) CHECK((a) == (b), #a " == " #b)
#define EXPECT_NE(a, b) CHECK((a) != (b), #a " != " #b)
#define EXPECT_LT(a, b) CHECK((a) < (b), #a " < " #b)
#define EXPECT_LE(a, b) CHECK((a) <= (b), #a " <= " #b)
#define EXPECT_GT(a, b) CHECK((a) > (b), #a " > " #b)
#define EXPECT_GE(a, b) CHECK((a) >= (b), #a " >= " #b)

// ── TEST() registration macro ────────────────────────────────
//
// Use: TEST(test_name) { ...test body... }
//
// Defines a function and registers it in the harness's
// static registry. RUN_ALL_TESTS() will run all
// registered tests and report per-test pass/fail.
//
// The body of a TEST() is the test function itself — it
// can use CHECK/EXPECT_* macros to record results.
#define TEST(test_name)                                                                            \
    static void test_name();                                                                       \
    static int test_name##_reg = ::aura::test::register_test(#test_name, test_name);               \
    static void test_name()

// ── RUN_ALL_TESTS() — unified main() body ────────────────────
//
// Replaces the per-file main(). Returns 0 on full pass, 1
// on any failure.
//
// If the file has registered tests via TEST(), runs them
// in registration order with per-test reporting. If the
// file has only legacy CHECK() calls (no registered
// tests), the harness reports the final g_passed/g_failed
// counter values.
inline int RUN_ALL_TESTS() {
    auto& reg = ::aura::test::registry();
    if (reg.empty()) {
        // No registered tests. The file used legacy
        // CHECK() macros directly. Report the counters.
        std::println("──────────────────────────────────────");
        std::println("Total: {} passed, {} failed", ::aura::test::g_passed, ::aura::test::g_failed);
        return ::aura::test::g_failed == 0 ? 0 : 1;
    }
    // Run registered tests.
    int passed = 0;
    int failed = 0;
    int failed_before = 0;
    for (auto& tc : reg) {
        failed_before = ::aura::test::g_failed;
        tc.fn();
        int new_fails = ::aura::test::g_failed - failed_before;
        if (new_fails == 0) {
            std::println("  PASS: {}", tc.name);
            ++passed;
        } else {
            std::println("  FAIL: {} ({} check(s) failed)", tc.name, new_fails);
            ++failed;
        }
    }
    std::println("════════════════════════════════════════");
    std::println("Total: {} passed, {} failed (across {} test cases)", passed, failed, reg.size());
    return failed == 0 ? 0 : 1;
}

// ── Env-var knob helpers (Issue #371 follow-up) ───────────
//
// Convention (2026-07-01, replaces the per-issue
// `AURA_NNN_*` prefix that the suite accumulated over
// 2025-2026). Single env var name across all tests for a
// given purpose; per-test default preserves the original
// intent (some tests want 8 iters, others 200, others
// 1000).
//
//   AURA_STRESS_ITERS    main stress-loop iteration count
//                        (default varies per test; e.g. 200
//                        for #371, 8 for #542's 8-thread
//                        scenario, 50 for 50-thread, 200
//                        for 605's stress, etc.)
//   AURA_STRESS_PARALLEL parallel-unit count for concurrent
//                        stress (threads / fibers / workers;
//                        default 4)
//   AURA_RACE_ITERS      race-scenario iteration count
//                        (default 200)
//   AURA_FUZZ_ITERS      fuzz-scenario iteration count
//                        (default 200)
//   AURA_WARMUP_CALLS    JIT warmup call count (default 120)
//
// Why no issue number? Env vars are process-global — when
// a test binary runs, it's the only reader. The NNN prefix
// didn't isolate anything; it just made every test have a
// different name. Centralizing shrinks the CI / local
// override surface to 5 names total.
//
// Returns the env value as int, or `default_value` if the
// env is unset / empty / unparseable / non-positive.
[[nodiscard]] inline int k_int_env(const char* name, int default_value) noexcept {
    if (const char* v = std::getenv(name); v != nullptr && *v != '\0') {
        char* end = nullptr;
        const long parsed = std::strtol(v, &end, 10);
        if (end != v && parsed > 0 && parsed <= 1'000'000'000) {
            return static_cast<int>(parsed);
        }
    }
    return default_value;
}

#endif // AURA_TEST_HARNESS_HPP