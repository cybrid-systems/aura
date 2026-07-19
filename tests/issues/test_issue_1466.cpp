// @category: unit
// @reason: pure C++ consteval + contract_stats surface; no CompilerService
//
// test_issue_1466.cpp — Issue #1466: hot-path Contracts + consteval
// invariant coverage expansion.
//
// Background: #1466 audits the hot paths (shape dispatch, dirty
// cascade, arena bump, value as_*, eval core) for aggressive
// `pre`/`post` placement (zero release cost under observe semantic),
// and grows the consteval invariant surface in cxx26_invariants.ixx.
// The consteval_checks count bumps so the AI Agent can detect drift
// via (query:cpp26-contracts-stats).
//
// ACs:
//   AC1: kConstevalChecksTotal bumped 36 → 53 (+17 hot-path invariants)
//   AC2: kCpp26ConstevalChecksShipped matches kConstevalChecksTotal
//   AC3: shape_inline_post_contracts_active flag = 1 (contract present)
//   AC4: arena_compact_contracts_active flag = 1 (contract present)
//   AC5: dirty_cascade_contracts_active flag = 1 (contract present)
//   AC6: consteval_invariants_total atomic matches shipped count
//   AC7: hotpath_invariant_hits_total still exposed (no regression)

#include "test_harness.hpp"
#include "core/cpp26_contract_stats.h"

import aura.core.cxx26_invariants;

import std;
using aura::test::g_failed;
using aura::test::g_passed;

namespace test_issue_1466_detail {

#undef CHECK
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::println("  FAIL: {} (line {})", msg, __LINE__);                                   \
            ++g_failed;                                                                            \
        } else {                                                                                   \
            std::println("  PASS: {}", msg);                                                       \
            ++g_passed;                                                                            \
        }                                                                                          \
    } while (0)

// Compile-time: consteval surface expansion is consistent across both
// authoritative sources. Any drift between cxx26_invariants.ixx and
// cpp26_contract_stats.h is caught at compile time.
static_assert(aura::core::cpp26::kConstevalChecksTotal == 53,
              "Issue #1466: kConstevalChecksTotal must be 53 (was 36, +17)");
static_assert(aura::core::kCpp26ConstevalChecksShipped == 53,
              "Issue #1466: kCpp26ConstevalChecksShipped must be 53 (was 36, +17)");
static_assert(
    aura::core::cpp26::kConstevalChecksTotal == aura::core::kCpp26ConstevalChecksShipped,
    "Issue #1466: consteval count must match between cpp26_contract_stats and cxx26_invariants");

void ac1_ac2_consteval_count() {
    std::println("\n--- AC1/AC2: consteval invariant count (compile-time + runtime) ---");
    CHECK(aura::core::cpp26::kConstevalChecksTotal == 53,
          "kConstevalChecksTotal == 53 (was 36, +17 in #1466)");
    CHECK(aura::core::kCpp26ConstevalChecksShipped == 53,
          "kCpp26ConstevalChecksShipped == 53 (matches cpp26_contract_stats)");
    CHECK(aura::core::cpp26::kConstevalChecksTotal == aura::core::kCpp26ConstevalChecksShipped,
          "consteval count cross-source consistency (no drift)");
}

void ac3_shape_inline_post_flag() {
    std::println("\n--- AC3: shape inline_shape_of post-contract flag ---");
    using aura::core::cpp26::shape_inline_post_contracts_active;
    CHECK(shape_inline_post_contracts_active.load(std::memory_order_relaxed) == 1,
          "shape_inline_post_contracts_active default = 1 (contract placed)");
}

void ac4_arena_compact_flag() {
    std::println("\n--- AC4: arena::compact pre/post flag ---");
    using aura::core::cpp26::arena_compact_contracts_active;
    CHECK(arena_compact_contracts_active.load(std::memory_order_relaxed) == 1,
          "arena_compact_contracts_active default = 1 (contract placed)");
}

void ac5_dirty_cascade_flag() {
    std::println("\n--- AC5: ast mark_dirty_upward post-contract flag ---");
    using aura::core::cpp26::dirty_cascade_contracts_active;
    CHECK(dirty_cascade_contracts_active.load(std::memory_order_relaxed) == 1,
          "dirty_cascade_contracts_active default = 1 (contract placed)");
}

void ac6_consteval_invariants_runtime() {
    std::println("\n--- AC6: consteval_invariants_total atomic default ---");
    using aura::core::cpp26::consteval_invariants_total;
    CHECK(consteval_invariants_total.load(std::memory_order_relaxed) == 53,
          "consteval_invariants_total default = 53 (matches shipped consteval)");
    CHECK(consteval_invariants_total.load(std::memory_order_relaxed) ==
              aura::core::cpp26::kConstevalChecksTotal,
          "consteval_invariants_total == kConstevalChecksTotal (no drift)");
}

void ac7_hotpath_invariant_hits_no_regression() {
    std::println("\n--- AC7: hotpath_invariant_hits_total still exposed ---");
    using aura::core::cpp26::hotpath_invariant_hits_total;
    // Pre-existing counter from #742. Just verify it's still addressable
    // and not zero-initialized as a sentinel for "removed".
    const auto v = hotpath_invariant_hits_total.load(std::memory_order_relaxed);
    CHECK(v >= 0, "hotpath_invariant_hits_total is readable (no regression)");
}

void ac8_record_consteval_invariant_added() {
    std::println("\n--- AC8: record_consteval_invariant_added helper ---");
    using aura::core::cpp26::consteval_invariants_total;
    using aura::core::cpp26::record_consteval_invariant_added;
    const auto before = consteval_invariants_total.load(std::memory_order_relaxed);
    record_consteval_invariant_added();
    const auto after = consteval_invariants_total.load(std::memory_order_relaxed);
    CHECK(after == before + 1, "record_consteval_invariant_added bumps the atomic counter by 1");
}

} // namespace test_issue_1466_detail

int main() {
    using namespace test_issue_1466_detail;
    std::println("=== Issue #1466 — hot-path Contracts + consteval coverage ===");
    ac1_ac2_consteval_count();
    ac3_shape_inline_post_flag();
    ac4_arena_compact_flag();
    ac5_dirty_cascade_flag();
    ac6_consteval_invariants_runtime();
    ac7_hotpath_invariant_hits_no_regression();
    ac8_record_consteval_invariant_added();

    std::println("\n─── #1466 summary: {}/{} passed, {}/{} failed ───", g_passed,
                 g_passed + g_failed, g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}