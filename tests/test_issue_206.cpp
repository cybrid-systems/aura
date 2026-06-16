// test_issue_206.cpp — Issue #206 GC sweep compact + remap
// + resolve_X helpers (subset of #145 Phase 2 / workstream 2).
//
// Verifies that the Evaluator's compact_pairs() and
// resolve_pair() correctly handle arena reallocation:
//   - Live pairs are moved to the front
//   - Dead pairs get remap entry -1
//   - Stale PairIds from before the compact still
//     resolve correctly (either to the new index or to -1)
//
// Test scenarios:
//   1. resolve_pair() before any compact is identity
//   2. compact_pairs() with empty live_mask = all-live
//   3. compact_pairs() with full live_mask = all-live
//   4. compact_pairs() with selective live_mask (some dead)
//      — dead pairs get -1, live pairs move to the front
//   5. Multi-step compact (compact twice) — first compact
//      builds remap A, second compact builds remap B;
//      resolve_pair uses the LATEST compact's remap
//   6. clear_pair_remap() resets to identity
//
// The arena is exercised directly via Evaluator internals
// (not via Aura primitives). This is a unit test of the
// compact/remap infrastructure.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.compiler.evaluator;
import aura.compiler.service;



#define PRINTLN(msg) std::fprintf(stdout, "%s\n", (msg))

// Helper: allocate N pairs into the evaluator's pairs_ arena.
// We use the CompilerService.eval interface (the Aura-level
// `cons` primitive) to allocate pairs, then access the
// Evaluator's compact/remap methods via service.evaluator().
static void alloc_pairs(aura::compiler::CompilerService& cs, int n) {
    for (int i = 0; i < n; ++i) {
        // Each pair: (i, i+1) — distinct values for verification
        std::string src = "(cons " + std::to_string(i) + " " +
                          std::to_string(i + 1) + ")";
        auto r = cs.eval(src);
        (void)r;
    }
}

// ── Test 1: resolve_pair before any compact is identity ──
// The remap table is empty (no compact has happened), so
// resolve_pair returns the input as-is.
bool test_resolve_pair_identity_before_compact() {
    PRINTLN("\n--- Test 1: resolve_pair is identity before any compact ---");
    aura::compiler::CompilerService cs;
    alloc_pairs(cs, 5);
    auto& ev = cs.evaluator();
    // No compact has happened; remap is empty
    CHECK(ev.pair_remap_size() == 0,
          "remap is empty (no compact yet)");
    // All 5 pairs resolve to themselves
    for (std::uint64_t i = 0; i < 5; ++i) {
        CHECK(ev.resolve_pair(i) == static_cast<std::int64_t>(i),
              "resolve_pair returns identity for id " + std::to_string(i));
    }
    return true;
}

// ── Test 2: compact_pairs with empty live_mask = all-live ──
// The empty live_mask is the "all pairs are live" signal.
// All pairs should be kept, and the remap should be
// identity (each old id maps to its same index).
bool test_compact_pairs_empty_mask_all_live() {
    PRINTLN("\n--- Test 2: compact_pairs with empty live_mask = all-live ---");
    aura::compiler::CompilerService cs;
    alloc_pairs(cs, 5);
    auto& ev = cs.evaluator();
    // Empty live_mask = all live
    std::vector<bool> empty_mask;
    std::int64_t n_after = ev.compact_pairs(empty_mask);
    CHECK(n_after == 5, "5 pairs remain after compact (all live)");
    // Remap: 0→0, 1→1, 2→2, 3→3, 4→4
    for (std::uint64_t i = 0; i < 5; ++i) {
        CHECK(ev.resolve_pair(i) == static_cast<std::int64_t>(i),
              "resolve_pair(" + std::to_string(i) +
                  ") is identity after all-live compact");
    }
    return true;
}

// ── Test 3: compact_pairs with selective live_mask ──
// Some pairs are marked dead. After compact, the live
// pairs are moved to the front, and the dead pairs'
// remap entries are -1.
bool test_compact_pairs_selective_mask() {
    PRINTLN("\n--- Test 3: compact_pairs with selective live_mask ---");
    aura::compiler::CompilerService cs;
    alloc_pairs(cs, 5);
    auto& ev = cs.evaluator();
    // Mark pairs 1 and 3 as dead. Live: 0, 2, 4.
    std::vector<bool> mask = {true, false, true, false, true};
    std::int64_t n_after = ev.compact_pairs(mask);
    CHECK(n_after == 3, "3 pairs remain after compact (5 - 2 dead)");
    // Expected remap:
    //   old 0 (live, 1st) → new 0
    //   old 1 (dead)      → -1
    //   old 2 (live, 2nd) → new 1
    //   old 3 (dead)      → -1
    //   old 4 (live, 3rd) → new 2
    CHECK(ev.resolve_pair(0) == 0, "old 0 (live) → new 0");
    CHECK(ev.resolve_pair(1) == -1, "old 1 (dead) → -1");
    CHECK(ev.resolve_pair(2) == 1, "old 2 (live) → new 1");
    CHECK(ev.resolve_pair(3) == -1, "old 3 (dead) → -1");
    CHECK(ev.resolve_pair(4) == 2, "old 4 (live) → new 2");
    return true;
}

// ── Test 4: multi-step compact (remap is rebuilt) ──
// A second compact replaces the first remap. Stale ids
// from the first compact no longer apply (resolve_pair
// uses the LATEST compact's remap).
bool test_compact_pairs_multi_step() {
    PRINTLN("\n--- Test 4: multi-step compact rebuilds the remap ---");
    aura::compiler::CompilerService cs;
    alloc_pairs(cs, 6);
    auto& ev = cs.evaluator();
    // First compact: dead = 1, 4. Live: 0, 2, 3, 5.
    std::vector<bool> mask1 = {true, false, true, true, false, true};
    ev.compact_pairs(mask1);
    // After first compact: 4 pairs live.
    // Remap: 0→0, 1→-1, 2→1, 3→2, 4→-1, 5→3
    CHECK(ev.resolve_pair(0) == 0, "after compact1: old 0 → new 0");
    CHECK(ev.resolve_pair(1) == -1, "after compact1: old 1 → -1");
    CHECK(ev.resolve_pair(2) == 1, "after compact1: old 2 → new 1");
    // Add 2 more pairs (now we have 4 live + 2 new = 6)
    alloc_pairs(cs, 2);
    // Second compact: dead = new pair at index 4 (the 5th
    // element of the new arena). Live: indices 0, 1, 2, 3, 5.
    // The 4 new pairs are at old indices 0..3 in the arena
    // AFTER the first compact. We've added 2 more, so the
    // arena is now 4 + 2 = 6. Let's mark index 4 (5th) as
    // dead (one of the newly added).
    std::vector<bool> mask2 = {true, true, true, true, false, true};
    ev.compact_pairs(mask2);
    // After second compact: 5 pairs live.
    // Remap: 0→0, 1→1, 2→2, 3→3, 4→-1, 5→4
    CHECK(ev.resolve_pair(0) == 0, "after compact2: old 0 → new 0");
    CHECK(ev.resolve_pair(1) == 1, "after compact2: old 1 → new 1");
    CHECK(ev.resolve_pair(4) == -1, "after compact2: old 4 → -1");
    CHECK(ev.resolve_pair(5) == 4, "after compact2: old 5 → new 4");
    return true;
}

// ── Test 5: clear_pair_remap resets to identity ──
// After clear, the remap is empty, and resolve_pair
// returns the input identity (no compact is in effect).
bool test_clear_pair_remap() {
    PRINTLN("\n--- Test 5: clear_pair_remap resets to identity ---");
    aura::compiler::CompilerService cs;
    alloc_pairs(cs, 3);
    auto& ev = cs.evaluator();
    std::vector<bool> mask = {true, false, true};
    ev.compact_pairs(mask);
    // Remap: 0→0, 1→-1, 2→1
    CHECK(ev.resolve_pair(1) == -1, "before clear: old 1 → -1");
    // Clear
    ev.clear_pair_remap();
    CHECK(ev.pair_remap_size() == 0, "remap is empty after clear");
    CHECK(ev.resolve_pair(0) == 0, "after clear: resolve_pair(0) is identity");
    CHECK(ev.resolve_pair(1) == 1, "after clear: resolve_pair(1) is identity");
    CHECK(ev.resolve_pair(2) == 2, "after clear: resolve_pair(2) is identity");
    return true;
}

// ── Test 6: resolve_pair handles out-of-range ids ──
// resolve_pair(id) where id >= remap size returns -1
// (defensive). resolve_pair(id) where id is huge returns
// the input identity (since remap is empty until compact).
bool test_resolve_pair_out_of_range() {
    PRINTLN("\n--- Test 6: resolve_pair handles out-of-range ids ---");
    aura::compiler::CompilerService cs;
    alloc_pairs(cs, 3);
    auto& ev = cs.evaluator();
    // No compact yet: identity
    CHECK(ev.resolve_pair(999) == 999,
          "resolve_pair(999) is identity when no compact has happened");
    // After compact with all-live (empty mask):
    std::vector<bool> empty_mask;
    ev.compact_pairs(empty_mask);
    // remap is size 3; resolve_pair(999) is out of range
    CHECK(ev.resolve_pair(999) == -1,
          "resolve_pair(999) returns -1 (out of remap range)");
    // resolve_pair(2) is in range, identity
    CHECK(ev.resolve_pair(2) == 2, "resolve_pair(2) is in range, identity");
    return true;
}

// ── Test 7: compact_pairs with all-dead mask ──
// All pairs are dead. After compact, the arena is empty,
// and the remap is all -1.
bool test_compact_pairs_all_dead() {
    PRINTLN("\n--- Test 7: compact_pairs with all-dead mask ---");
    aura::compiler::CompilerService cs;
    alloc_pairs(cs, 4);
    auto& ev = cs.evaluator();
    std::vector<bool> mask = {false, false, false, false};
    std::int64_t n_after = ev.compact_pairs(mask);
    CHECK(n_after == 0, "0 pairs remain after all-dead compact");
    for (std::uint64_t i = 0; i < 4; ++i) {
        CHECK(ev.resolve_pair(i) == -1,
              "resolve_pair(" + std::to_string(i) +
                  ") is -1 (all dead)");
    }
    return true;
}

int run_issue_206() {
    std::fprintf(stdout, "═══ Issue #206 — GC sweep compact + remap + resolve_X ═══\n");
    std::fprintf(stdout, "  Verifies the compact_pairs() / resolve_pair() API.\n\n");

    test_resolve_pair_identity_before_compact();
    test_compact_pairs_empty_mask_all_live();
    test_compact_pairs_selective_mask();
    test_compact_pairs_multi_step();
    test_clear_pair_remap();
    test_resolve_pair_out_of_range();
    test_compact_pairs_all_dead();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
