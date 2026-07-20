// test_harness.hpp — unified C++ test harness for Aura (#1960)
//
// Single recommended header for domain/, batch, pilot, and issue tests.
// Legacy alias: issue_test_harness.hpp (thin shim; prefer this file).
//
// Policy: tests/README.md · tests/domain/README.md · tests/STRATEGY.md (#1887)
//
// Features:
//   CHECK / EXPECT_*     — pass/fail counters (ASan-safe owned message string)
//   TEST / RUN_ALL_TESTS — optional registered cases
//   AURA_ISSUE_TEST     — bundle entry-point helper
//   AURA_ISSUE_BOOTSTRAP — CompilerService main scaffolding
//   run_pilot_tests()    — report counters (pilot-style mains)
//   aura_call_expr()     — engine:metrics / stats:get routing for demoted names
//   k_int_env()          — shared stress/fuzz env knobs
//   capture_stable_refs / validate_stable_refs — FlatAST white-box helpers
//   note_strategy_*      — hot-path / AI self-mod strategy stamps (#1887)
//
// Intentionally lightweight: no Google Test / Catch2.

#ifndef AURA_TEST_HARNESS_HPP
#define AURA_TEST_HARNESS_HPP

#include "test/test_strategy.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace aura::test {

// Global pass/fail counters (legacy CHECK pattern). Initialized to 0.
inline int g_passed = 0;
inline int g_failed = 0;

// Issue #1887: strategy-driven hot-path / self-mod stamps (see tests/STRATEGY.md).
inline void note_strategy_scenario(strategy::HotPathScenario s, bool pass = true) noexcept {
    strategy::note_hotpath_scenario(s, pass);
}
inline void note_strategy_self_mod_loop(bool ok = true) noexcept {
    strategy::note_self_mod_loop(ok);
}
inline void note_strategy_profile(strategy::StrategyProfile p) noexcept {
    strategy::select_profile(p);
}

// Issue #1439 / #1449: build an Aura expression that evaluates a primitive or
// facade-only stats name. Mirrors ObservabilityPrims::is_legacy_stats_name —
// demoted dashboards route through (engine:metrics "…") / (stats:get "…").
// Multi-arg structural query:* stay bare.
inline std::string aura_call_expr(std::string_view name) {
    if (name.ends_with("-stats") || name.ends_with("-stats-hash") ||
        name.find("-stats-") != std::string_view::npos)
        return std::format("(engine:metrics \"{}\")", name);
    if (name.starts_with("query:")) {
        const auto rest = name.substr(6);
        // Keep in sync with is_legacy_stats_name demotion suffixes / names.
        if (rest.ends_with("-health") || rest.ends_with("-readiness") || rest.ends_with("-slo") ||
            rest.ends_with("-score") || rest.ends_with("-fidelity") ||
            rest.ends_with("-invariants") || rest.ends_with("-win") ||
            rest.ends_with("-contracts") || rest.ends_with("-stability") ||
            rest.ends_with("-snapshot") || rest.ends_with("-histogram") ||
            rest.ends_with("-effectiveness") || rest.ends_with("-available") ||
            rest.ends_with("-audit-log") || rest == "primitives-meta-catalog" ||
            rest == "primitive-metadata" || rest == "primitive-list-with-meta" ||
            rest == "primitives-meta" || rest == "primitives-by-category" ||
            rest == "orchestration-metrics" || rest == "edsl-readiness" ||
            rest == "production-health" || rest == "serve-health" ||
            rest == "code-as-data-production-health" || rest == "runtime-production-health" ||
            rest == "eda-production-readiness")
            return std::format("(engine:metrics \"{}\")", name);
    }
    if (name.starts_with("dirty:") ||
        (name.starts_with("render-") &&
         (name.ends_with("-samples") || name == "render-hotpath-depth" ||
          name.ends_with("-histogram"))))
        return std::format("(stats:get \"{}\")", name);
    return std::format("({})", name);
}

// One registered test. Function pointer (not std::function) so module-unit
// TUs avoid <functional> conflicts with `import std`.
using TestFn = void (*)();
struct TestCase {
    const char* name;
    TestFn fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int register_test(const char* name, TestFn fn) {
    registry().push_back(TestCase{name, fn});
    return 0;
}

// ── FlatAST StableNodeRef helpers (from issue_test_harness / #329) ──
// Header-only white-box helpers; no aura module imports required.

template <typename FlatT> auto capture_stable_refs(FlatT& flat, int count) {
    using RefT = decltype(flat.make_ref(typename FlatT::NodeId{0}));
    std::vector<RefT> refs;
    refs.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        refs.push_back(flat.make_ref(typename FlatT::NodeId{static_cast<std::uint32_t>(i)}));
    }
    return refs;
}

struct RefStats {
    std::size_t still_valid = 0;
    std::size_t dangling = 0;
    [[nodiscard]] std::size_t total() const noexcept { return still_valid + dangling; }
    [[nodiscard]] double dangling_pct() const noexcept {
        return total() == 0 ? 0.0
                            : 100.0 * static_cast<double>(dangling) / static_cast<double>(total());
    }
};

template <typename FlatT, typename RefsT> RefStats validate_stable_refs(RefsT& refs, FlatT& flat) {
    RefStats stats;
    for (auto& ref : refs) {
        if (ref.is_valid_in(flat)) {
            stats.still_valid++;
        } else {
            stats.dangling++;
        }
    }
    return stats;
}

// Env-var knobs (shared names; no per-issue AURA_NNN_* prefixes).
//   AURA_STRESS_ITERS / AURA_STRESS_PARALLEL / AURA_RACE_ITERS /
//   AURA_FUZZ_ITERS / AURA_WARMUP_CALLS
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

} // namespace aura::test

// ── CHECK / EXPECT macros ────────────────────────────────────
//
// Issue #226: store _check_msg as owning std::string so format_args
// never hold a dangling c_str() from a temporary (ASan UAF).
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

#define EXPECT_TRUE(cond) CHECK((cond), #cond)
#define EXPECT_FALSE(cond) CHECK(!(cond), "!(" #cond ")")
#define EXPECT_EQ(a, b) CHECK((a) == (b), #a " == " #b)
#define EXPECT_NE(a, b) CHECK((a) != (b), #a " != " #b)
#define EXPECT_LT(a, b) CHECK((a) < (b), #a " < " #b)
#define EXPECT_LE(a, b) CHECK((a) <= (b), #a " <= " #b)
#define EXPECT_GT(a, b) CHECK((a) > (b), #a " > " #b)
#define EXPECT_GE(a, b) CHECK((a) >= (b), #a " >= " #b)

// ── AURA_ISSUE_TEST — bundle entry-point helper ──
// Prefer domain suites (tests/README.md). Macro at FILE SCOPE only.
#define AURA_ISSUE_TEST(ISSUE_NUM, DESC, BODY)                                                    \
    extern "C" int aura_issue_##ISSUE_NUM##_run() {                                                \
        BODY                                                                                       \
        return ::aura::test::g_failed > 0 ? 1 : 0;                                                 \
    }

// ── TEST() registration ──────────────────────────────────────
#define TEST(test_name)                                                                            \
    static void test_name();                                                                       \
    static int test_name##_reg = ::aura::test::register_test(#test_name, test_name);               \
    static void test_name()

// ── RUN_ALL_TESTS / run_pilot_tests ──────────────────────────
inline int RUN_ALL_TESTS() {
    auto& reg = ::aura::test::registry();
    if (reg.empty()) {
        std::println("──────────────────────────────────────");
        std::println("Total: {} passed, {} failed", ::aura::test::g_passed, ::aura::test::g_failed);
        return ::aura::test::g_failed == 0 ? 0 : 1;
    }
    int passed = 0;
    int failed = 0;
    for (auto& tc : reg) {
        const int failed_before = ::aura::test::g_failed;
        tc.fn();
        const int new_fails = ::aura::test::g_failed - failed_before;
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

// Pilot-style summary (same counters as CHECK). Prefer RUN_ALL_TESTS for
// registered TEST() cases; this remains for legacy pilot mains.
inline int run_pilot_tests() {
    std::println("\n--- Results ---");
    std::println("  PASSED: {}", ::aura::test::g_passed);
    std::println("  FAILED: {}", ::aura::test::g_failed);
    if (::aura::test::g_failed > 0) {
        std::println("  OVERALL: FAIL");
        return 1;
    }
    std::println("  OVERALL: PASS");
    return 0;
}

// Free-function aliases used by older TUs that call k_int_env unqualified.
using ::aura::test::k_int_env;

// Legacy namespace for StableNodeRef helpers (issue_test_harness callers).
namespace aura_test_harness {
using ::aura::test::RefStats;
using ::aura::test::capture_stable_refs;
using ::aura::test::validate_stable_refs;
} // namespace aura_test_harness

// Issue #881: AURA_ISSUE_BOOTSTRAP — CompilerService main scaffolding.
// Requires CompilerService visible in the TU (import / include before use).
#ifndef AURA_ISSUE_BOOTSTRAP
#define AURA_ISSUE_BOOTSTRAP(run_fn)                                                               \
    int main() {                                                                                   \
        aura::compiler::CompilerService cs;                                                        \
        run_fn(cs);                                                                                \
        return RUN_ALL_TESTS();                                                                    \
    }
#endif

#endif // AURA_TEST_HARNESS_HPP
