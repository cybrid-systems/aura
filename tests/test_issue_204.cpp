// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_204.cpp — Issue #172 (Phase 4) / #204: GC
// integration with the EnvFrame SoA arena.
//
// Verifies GCCollector::mark_env_frame_roots(), the new
// method that the GC uses to discover roots reachable
// through env_frames_. The caller (evaluator) walks the
// env_frames_ arena and produces pair/closure index lists;
// the GC marks each list's indices in the corresponding
// MarkBitVector.
//
// Test scenarios:
//   1. mark_env_frame_roots with non-empty pair/closure lists
//      sets the corresponding bits
//   2. mark_env_frame_roots with empty lists is a no-op
//   3. Negative indices are ignored (defensive)
//   4. mark_env_frame_roots is independent of mark_from_roots
//      (the env walk is additive on top of the root set)
//   5. mark_env_frame_roots before mark_from_roots (no prior
//      resize) is a safe no-op (mark vector not sized yet)


#include "serve/gc_coordinator.h"

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;



namespace aura_issue_204_detail {
#define PRINTLN(msg) do { std::print( "%s\n", (msg)); } while(0)

// ── Test 1: mark_env_frame_roots sets the bits ──
bool test_env_frame_roots_marks_bits() {
    PRINTLN("\n--- Test 1: mark_env_frame_roots sets bits ---");
    // GCCollector needs a Scheduler. We can pass nullptr since
    // mark_env_frame_roots doesn't touch the scheduler.
    aura::serve::GCCollector gc(nullptr);
    // Size the mark vectors (mark_from_roots will resize)
    std::vector<int64_t> dummy_pairs = {0};  // just to size pair_marks_
    std::vector<int64_t> dummy_closures = {0};  // size closure_marks_
    // We need heap sizes to size the mark vectors properly
    gc.mark_from_roots(
        /*roots=*/{}, /*string_heap_size=*/10, /*pairs_size=*/20,
        /*closures_size=*/30);
    // After this: pair_marks_ has 20 bits, closure_marks_ has 30 bits

    // Provide env-walk roots for pairs and closures
    std::vector<int64_t> env_pairs = {3, 7, 15};
    std::vector<int64_t> env_closures = {5, 10, 25};
    gc.mark_env_frame_roots(env_pairs, env_closures);

    // Verify the bits are set
    CHECK(gc.pair_mark(3) == true, "pair index 3 marked via env walk");
    CHECK(gc.pair_mark(7) == true, "pair index 7 marked via env walk");
    CHECK(gc.pair_mark(15) == true, "pair index 15 marked via env walk");
    CHECK(gc.closure_mark(5) == true, "closure index 5 marked via env walk");
    CHECK(gc.closure_mark(10) == true, "closure index 10 marked via env walk");
    CHECK(gc.closure_mark(25) == true, "closure index 25 marked via env walk");
    // Non-walked indices should be false
    CHECK(gc.pair_mark(0) == false, "pair index 0 NOT marked (not in env walk)");
    CHECK(gc.pair_mark(4) == false, "pair index 4 NOT marked");
    CHECK(gc.closure_mark(0) == false, "closure index 0 NOT marked");
    return true;
}

// ── Test 2: empty env walk is a no-op ──
bool test_env_frame_roots_empty() {
    PRINTLN("\n--- Test 2: empty env walk is a no-op ---");
    aura::serve::GCCollector gc(nullptr);
    gc.mark_from_roots({}, 5, 5, 5);
    // Pass empty lists — should be a no-op (no crash, no false marks)
    gc.mark_env_frame_roots({}, {});
    CHECK(gc.pair_mark(0) == false, "empty env walk: no pair marks");
    CHECK(gc.pair_mark(4) == false, "empty env walk: no pair marks (out of range)");
    CHECK(gc.closure_mark(0) == false, "empty env walk: no closure marks");
    return true;
}

// ── Test 3: negative indices are ignored (defensive) ──
bool test_env_frame_roots_negative_ignored() {
    PRINTLN("\n--- Test 3: negative indices are ignored ---");
    aura::serve::GCCollector gc(nullptr);
    gc.mark_from_roots({}, 10, 10, 10);
    std::vector<int64_t> bad = {-1, -2, 5, -100};
    gc.mark_env_frame_roots(bad, bad);
    CHECK(gc.pair_mark(5) == true, "valid index 5 still marked");
    // The negative indices should not crash
    CHECK(gc.pair_mark(0) == false, "no spurious mark from negatives");
    return true;
}

// ── Test 4: env walk is additive to mark_from_roots ──
bool test_env_walk_additive_to_roots() {
    PRINTLN("\n--- Test 4: env walk is additive to mark_from_roots ---");
    aura::serve::GCCollector gc(nullptr);
    aura::serve::GCRootSet roots;
    roots.pair_roots.push_back(0);
    roots.pair_roots.push_back(1);
    roots.closure_roots.push_back(2);
    gc.mark_from_roots(roots, 0, 10, 10);

    // After mark_from_roots, pairs 0+1 are marked, closure 2 is marked
    CHECK(gc.pair_mark(0) == true, "pair 0 marked from root set");
    CHECK(gc.pair_mark(1) == true, "pair 1 marked from root set");
    CHECK(gc.closure_mark(2) == true, "closure 2 marked from root set");

    // Now add env-walk roots for OTHER indices
    std::vector<int64_t> env_pairs = {3, 4};
    std::vector<int64_t> env_closures = {5, 6};
    gc.mark_env_frame_roots(env_pairs, env_closures);

    // The original root-set marks are still there
    CHECK(gc.pair_mark(0) == true, "pair 0 still marked (additive)");
    CHECK(gc.pair_mark(1) == true, "pair 1 still marked (additive)");
    // The env-walk marks are added
    CHECK(gc.pair_mark(3) == true, "pair 3 added by env walk");
    CHECK(gc.pair_mark(4) == true, "pair 4 added by env walk");
    CHECK(gc.closure_mark(5) == true, "closure 5 added by env walk");
    CHECK(gc.closure_mark(6) == true, "closure 6 added by env walk");
    return true;
}

// ── Test 5: env walk before mark_from_roots is a safe no-op ──
bool test_env_walk_before_root_set() {
    PRINTLN("\n--- Test 5: env walk before mark_from_roots is safe ---");
    aura::serve::GCCollector gc(nullptr);
    // Note: mark_env_frame_roots called BEFORE mark_from_roots.
    // The mark vectors aren't sized yet, so set() is a no-op.
    std::vector<int64_t> env_pairs = {0, 1, 2};
    std::vector<int64_t> env_closures = {0, 1};
    gc.mark_env_frame_roots(env_pairs, env_closures);

    // Now size the vectors via mark_from_roots
    gc.mark_from_roots({}, 5, 5, 5);

    // The env-walk calls happened before resize, so they were
    // silent no-ops. The bits should be 0.
    CHECK(gc.pair_mark(0) == false, "pair 0 NOT marked (env walk was pre-resize)");
    CHECK(gc.pair_mark(1) == false, "pair 1 NOT marked");
    CHECK(gc.closure_mark(0) == false, "closure 0 NOT marked");
    return true;
}

int run_tests() {
    std::println("═══ Issue #172 (Phase 4) / #204 — GC env_frames_ walk ═══");
    std::println("  Verifies GCCollector::mark_env_frame_roots().\n");

    test_env_frame_roots_marks_bits();
    test_env_frame_roots_empty();
    test_env_frame_roots_negative_ignored();
    test_env_walk_additive_to_roots();
    test_env_walk_before_root_set();

    std::println("\n──────────────────────────────────────");
    std::println("Total: %d passed, %d failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
}  // namespace aura_issue_204_detail

int aura_issue_204_run() { return aura_issue_204_detail::run_tests(); }

