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

// ═══════════════════════════════════════════════════════════════
// Issue #329: StableNodeRef / generation_ white-box helpers
// ═══════════════════════════════════════════════════════════════
//
// capture_stable_refs(flat, count) → returns a vector of
// StableNodeRef (deduced via decltype, since the type isn't
// exported). Callers can iterate and check is_valid_in() after
// a mutation cycle to measure the dangling rate.
//
// validate_stable_refs(refs, flat) → returns {still_valid,
// dangling}. Use after a mutate to gate against regressions
// (e.g. REQUIRE dangling_pct > X to prove mutation invalidates).
//
// These helpers are header-only so any test binary can include
// the harness without linking a separate .cpp.

#include <cstddef>
#include <vector>

namespace aura_test_harness {

// Capture count StableNodeRefs from the flat starting at
// NodeId{0} .. NodeId{count-1}. Returns a vector of the
// deduced StableNodeRef type (auto in the caller).
template <typename FlatT>
auto capture_stable_refs(FlatT& flat, int count) {
    using RefT = decltype(flat.make_ref(typename FlatT::NodeId{0}));
    std::vector<RefT> refs;
    refs.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        refs.push_back(flat.make_ref(typename FlatT::NodeId{
            static_cast<std::uint32_t>(i)}));
    }
    return refs;
}

struct RefStats {
    std::size_t still_valid = 0;
    std::size_t dangling = 0;
    [[nodiscard]] std::size_t total() const noexcept {
        return still_valid + dangling;
    }
    [[nodiscard]] double dangling_pct() const noexcept {
        return total() == 0 ? 0.0
                             : 100.0 * static_cast<double>(dangling) / static_cast<double>(total());
    }
};

template <typename FlatT, typename RefsT>
RefStats validate_stable_refs(RefsT& refs, FlatT& flat) {
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

}  // namespace aura_test_harness

#endif  // AURA_ISSUE_TEST_HARNESS_HPP
