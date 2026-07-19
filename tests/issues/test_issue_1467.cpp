// @category: unit
// @reason: pure C++ arena defrag foundation; no CompilerService / LLVM JIT
//
// test_issue_1467.cpp — Issue #1467 Phase 1: live-object-moving defrag
// foundation (mark-only skeleton, no copy/relocate).
//
// Background: #300 shipped defrag() foundation + counters that stay 0
// until the full live-object-moving path lands. #1467 Phase 1 ships the
// minimum to make `live_defrag()` callable + its counters updateable:
//   1. live_defrag() method (mark-only, no copy)
//   2. live_defrag_attempted_count + live_objects_marked_total counters
//   3. accessor methods
//   4. format() / merge() / JSON output updates
// The actual copy + pointer remapping is tracked as #1467 Phase 2/3
// follow-ups (research-grade work, 4-6 days total per the issue body).
//
// ACs:
//   AC1: live_defrag() callable, returns marked object count >= 0
//   AC2: stats_.live_defrag_attempted_count bumps by 1 per call
//   AC3: live_objects_marked_total bumps by >= tier count after alloc
//   AC4: conservative defrag() still works (no regression)
//   AC5: format() / merge() include new fields
//   AC6: accessor methods return values consistent with stats
//   AC7: shape_inval_on_compact bumps via invoke_compact_hook_

#include "test_harness.hpp"

import aura.core.arena;

import std;
using aura::test::g_failed;
using aura::test::g_passed;

namespace test_issue_1467_detail {

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

void ac1_live_defrag_callable() {
    std::println("\n--- AC1: live_defrag() callable + returns marked count ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    const auto before = arena->live_defrag_attempted_count_relaxed();
    const auto marked = arena->live_defrag();
    const auto after = arena->live_defrag_attempted_count_relaxed();
    CHECK(marked >= 0, "live_defrag() returns non-negative marked count");
    CHECK(after == before + 1, "live_defrag_attempted_count bumps by 1 per call (atomic mirror)");
}

void ac2_stats_counter_bumps() {
    std::println("\n--- AC2: ArenaStats live_defrag_attempted_count bumps ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    const auto s_before = arena->stats().live_defrag_attempted_count;
    arena->live_defrag();
    const auto s_after = arena->stats().live_defrag_attempted_count;
    CHECK(s_after == s_before + 1, "stats.live_defrag_attempted_count bumps by 1 (stats struct)");
}

void ac3_marked_count_after_alloc() {
    std::println("\n--- AC3: live_objects_marked_total reflects allocations ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    // Allocate 3 small objects in tier 0 (16 bytes).
    void* p1 = arena->try_allocate(16);
    void* p2 = arena->try_allocate(16);
    void* p3 = arena->try_allocate(16);
    CHECK(p1 != nullptr && p2 != nullptr && p3 != nullptr,
          "try_allocate(16) x3 returns non-null (tier 0)");
    const auto marked_before = arena->stats().live_objects_marked_total;
    arena->live_defrag();
    const auto marked_after = arena->stats().live_objects_marked_total;
    CHECK(marked_after >= marked_before + 3,
          "live_objects_marked_total bumps by at least 3 (3 allocs in tier 0)");
}

void ac4_conservative_defrag_no_regression() {
    std::println("\n--- AC4: conservative defrag() still works ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    // Allocate something so defrag has something to do.
    void* p = arena->try_allocate(32);
    CHECK(p != nullptr, "try_allocate(32) returns non-null");
    const auto d_before = arena->stats().defrag_attempted_count;
    const auto reclaimed = arena->defrag();
    const auto d_after = arena->stats().defrag_attempted_count;
    CHECK(d_after == d_before + 1, "defrag_attempted_count bumps by 1");
    CHECK(reclaimed >= 0, "defrag() returns non-negative reclaimed bytes");
}

void ac5_format_and_merge() {
    std::println("\n--- AC5: format() + merge() include new fields ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    arena->live_defrag();
    const auto formatted = arena->stats().format();
    CHECK(formatted.find("live-defrags") != std::string::npos,
          "format() output contains 'live-defrags'");
    CHECK(formatted.find("marked") != std::string::npos, "format() output contains 'marked'");
    // Merge discipline: live counters sum across arenas.
    auto arena2 = std::make_unique<aura::ast::ASTArena>();
    arena2->live_defrag();
    arena2->live_defrag();
    auto merged = arena->stats();
    merged.merge(arena2->stats());
    const std::size_t expected_live =
        arena->stats().live_defrag_attempted_count + arena2->stats().live_defrag_attempted_count;
    CHECK(merged.live_defrag_attempted_count == expected_live,
          "merge() sums live_defrag_attempted_count correctly");
}

void ac6_accessors_consistent() {
    std::println("\n--- AC6: accessors return values consistent with stats ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    arena->live_defrag();
    arena->live_defrag();
    arena->live_defrag();
    const auto accessor_count = arena->live_defrag_attempted_count_relaxed();
    const auto stats_count = arena->stats().live_defrag_attempted_count;
    CHECK(accessor_count == stats_count,
          "live_defrag_attempted_count_relaxed() == stats.live_defrag_attempted_count");
    const auto accessor_marked = arena->live_objects_marked_total_relaxed();
    const auto stats_marked = arena->stats().live_objects_marked_total;
    CHECK(accessor_marked == stats_marked,
          "live_objects_marked_total_relaxed() == stats.live_objects_marked_total");
}

void ac7_shape_inval_on_compact_via_hook() {
    std::println("\n--- AC7: live_defrag triggers shape invalidation hook ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    int hook_calls = 0;
    arena->set_on_compact_hook([&hook_calls]() { ++hook_calls; });
    const auto before = arena->stats().shape_inval_on_compact;
    arena->live_defrag();
    const auto after = arena->stats().shape_inval_on_compact;
    CHECK(hook_calls == 1, "on_compact_hook called exactly once per live_defrag()");
    CHECK(after == before + 1, "shape_inval_on_compact bumps by 1 (via invoke_compact_hook_)");
}

} // namespace test_issue_1467_detail

int main() {
    using namespace test_issue_1467_detail;
    std::println("=== Issue #1467 — live-object defrag foundation (Phase 1, mark-only) ===");
    ac1_live_defrag_callable();
    ac2_stats_counter_bumps();
    ac3_marked_count_after_alloc();
    ac4_conservative_defrag_no_regression();
    ac5_format_and_merge();
    ac6_accessors_consistent();
    ac7_shape_inval_on_compact_via_hook();

    std::println("\n─── #1467 summary: {}/{} passed, {}/{} failed ───", g_passed,
                 g_passed + g_failed, g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}