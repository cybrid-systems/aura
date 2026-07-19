// @category: integration
// @reason: uses CompilerService to verify per-DefUseIndex O(uses) wall-clock signal

// test_issue_411_followup_4.cpp — Issue #411 fu1
// follow-up #4 (scope-limited close): Caller struct
// NodeId 化 + TypeChecker iterates tracker entries
// directly (the actual O(uses) wall-clock win, not just
// the metric). Pre-fu4, the per-DefUseIndex path
// correctly recorded `per_defuse_index_used_total`
// (the metric) but still did the O(n)
// `affected_subtree_for_symbol` walk for the actual
// NodeId set (no wall-clock win). Post-fu4, the path
// iterates the tracker's stored NodeIds directly — O(K)
// where K is the number of use-sites for that binding,
// not O(N) where N is the total number of nodes.
//
// The actual wall-clock savings will be visible in
// benchmarks (tests/bench/), not in the unit tests.
// This scope-limited slice ships the FOUNDATION +
// observability: the metric
// `per_defuse_index_visited_total` now correctly counts
// the O(uses) work, separate from the O(n) walk cost
// (which goes into `per_symbol_visited_total` when the
// tracker is present but the sym isn't in the tracker).
//
// Test cases:
//   AC1: fresh CompilerService → all counters = 0
//   AC2: Caller struct stores NodeId (not string)
//   AC3: (compile:per-defuse-index-add) with int NodeId
//        arg works
//   AC4: (compile:per-defuse-index-callers) returns
//        hash with stringified NodeIds
//   AC5: typed_mutate with populated tracker bumps
//        per_defuse_index_visited_total (the O(uses)
//        signal, not just the metric)
//   AC6: typed_mutate with populated tracker for a
//        non-tracked sym bumps per_defuse_index_walk_fallback_total
//        (sym not in tracker → O(n) walk fallback)
//   AC7: regression — existing eval still works


import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_411fu4_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println("  PASS: {}", msg);                                                       \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println("  FAIL: {}", msg);                                                       \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b, msg)                                                                        \
    do {                                                                                           \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (_a == _b) {                                                                            \
            ++g_passed;                                                                            \
            std::println("  PASS: {}  ({} = {})", msg, _a, _b);                                    \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println("  FAIL: {}  ({} != {})", msg, _a, _b);                                   \
        }                                                                                          \
    } while (0)

bool test_initial_counters_zero() {
    std::println("\n--- AC1: per-DefUseIndex counters start at 0 ---");
    aura::compiler::CompilerService cs;
    auto snap = cs.snapshot();
    CHECK_EQ(snap.per_defuse_index_used_total, 0u, "per_defuse_index_used_total == 0");
    CHECK_EQ(snap.per_defuse_index_visited_total, 0u,
             "per_defuse_index_visited_total == 0 (O(uses) signal)");
    CHECK_EQ(snap.per_defuse_index_walk_fallback_total, 0u,
             "per_defuse_index_walk_fallback_total == 0");
    return true;
}

bool test_caller_stores_node_id() {
    std::println("\n--- AC2: Caller struct stores NodeId ---");
    // Verify via the Aura surface: (compile:per-defuse-index-add
    // <idx> <NodeId-int>) should accept an int and the
    // resulting (compile:per-defuse-index-callers <idx>)
    // should return the same NodeId (stringified for hash
    // keys, returned as int for values). This proves the
    // underlying Caller struct stores NodeIds, not strings.
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(begin (compile:per-defuse-index-add \\\"foo\\\" 12345))\")");
    cs.eval("(eval-current)");
    auto r = cs.eval("(hash-ref (compile:per-defuse-index-callers \"foo\") \"12345\")");
    if (r && aura::compiler::types::is_int(*r)) {
        std::int64_t v = aura::compiler::types::as_int(*r);
        CHECK_EQ(v, 12345, "Caller stores NodeId 12345 (int round-trip)");
    } else {
        std::println("  FAIL: hash-ref 12345 did not return int (val={})", r ? r->val : -1);
        ++g_failed;
    }
    return true;
}

bool test_aura_add_with_node_id() {
    std::println("\n--- AC3: (compile:per-defuse-index-add) with NodeId int arg ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(begin (compile:per-defuse-index-add \\\"foo\\\" 999))\")");
    cs.eval("(eval-current)");
    auto rst =
        cs.eval("(hash-ref (engine:metrics \"compile:per-defuse-index-stats\") \"total-size\")");
    if (rst && aura::compiler::types::is_int(*rst)) {
        std::int64_t v = aura::compiler::types::as_int(*rst);
        CHECK_EQ(v, 1, "per-defuse-index-add with NodeId int -> total-size == 1");
    } else {
        std::println("  FAIL: stats hash-ref returned non-int");
        ++g_failed;
    }
    return true;
}

bool test_aura_callers_returns_nodeid_hash() {
    std::println("\n--- AC4: (compile:per-defuse-index-callers) returns hash ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(begin (compile:per-defuse-index-add \\\"foo\\\" 11) "
            "(compile:per-defuse-index-add \\\"foo\\\" 22))\")");
    cs.eval("(eval-current)");
    // hash-ref with string key (the keys are stringified NodeIds)
    auto r11 = cs.eval("(hash-ref (compile:per-defuse-index-callers \"foo\") \"11\")");
    auto r22 = cs.eval("(hash-ref (compile:per-defuse-index-callers \"foo\") \"22\")");
    if (r11 && aura::compiler::types::is_int(*r11)) {
        CHECK_EQ(aura::compiler::types::as_int(*r11), 11, "hash-ref \"11\" returns 11 (NodeId)");
    } else {
        std::println("  FAIL: hash-ref \"11\" failed (val={})", r11 ? r11->val : -1);
        ++g_failed;
    }
    if (r22 && aura::compiler::types::is_int(*r22)) {
        CHECK_EQ(aura::compiler::types::as_int(*r22), 22, "hash-ref \"22\" returns 22 (NodeId)");
    } else {
        std::println("  FAIL: hash-ref \"22\" failed (val={})", r22 ? r22->val : -1);
        ++g_failed;
    }
    return true;
}

bool test_typed_mutate_populated_tracker_bumps_visited() {
    std::println(
        "\n--- AC5: typed_mutate with populated tracker bumps per_defuse_index_visited_total ---");
    aura::compiler::CompilerService cs;
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Eager);
    cs.eval("(set-code \"(define f 1) (define g (+ f 1))\")");
    cs.eval("(eval-current)");
    // Populate the tracker with a NodeId for the "f"
    // sym. The O(uses) lookup will use the stored
    // NodeIds directly.
    cs.eval("(set-code \"(define setup (begin (compile:per-defuse-index-add \\\"f\\\" 555)))\")");
    cs.eval("(eval-current)");
    cs.eval("(setup)");
    auto snap0 = cs.snapshot();
    // Mutate:rebind f. The per-DefUseIndex path should
    // fire and bump per_defuse_index_visited_total.
    auto r = cs.eval("(mutate:rebind \"f\" \"100\" \"bump\")");
    if (!r) {
        std::println("  FAIL: mutate:rebind failed");
        ++g_failed;
        return false;
    }
    auto snap1 = cs.snapshot();
    std::println("  per_defuse_index_used: {} -> {}", snap0.per_defuse_index_used_total,
                 snap1.per_defuse_index_used_total);
    std::println("  per_defuse_index_visited: {} -> {}", snap0.per_defuse_index_visited_total,
                 snap1.per_defuse_index_visited_total);
    CHECK(snap1.per_defuse_index_used_total > snap0.per_defuse_index_used_total,
          "per_defuse_index_used_total incremented (O(uses) path fired)");
    CHECK(snap1.per_defuse_index_visited_total > snap0.per_defuse_index_visited_total,
          "per_defuse_index_visited_total incremented (the O(uses) signal)");
    return true;
}

bool test_typed_mutate_non_tracked_sym_bumps_walk_fallback() {
    std::println("\n--- AC6: typed_mutate with non-tracked sym bumps walk_fallback ---");
    aura::compiler::CompilerService cs;
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Eager);
    cs.eval("(set-code \"(define f 1) (define g (+ f 1))\")");
    cs.eval("(eval-current)");
    // Populate the tracker for an UNRELATED sym "x".
    // When we mutate:rebind "f", the tracker has no entry
    // for "f", so the per-DefUseIndex path should fall
    // back to the O(n) walk and bump walk_fallback_total.
    cs.eval("(set-code \"(define setup (begin (compile:per-defuse-index-add \\\"x\\\" 999)))\")");
    cs.eval("(eval-current)");
    cs.eval("(setup)");
    auto snap0 = cs.snapshot();
    auto r = cs.eval("(mutate:rebind \"f\" \"200\" \"bump2\")");
    if (!r) {
        std::println("  FAIL: mutate:rebind failed");
        ++g_failed;
        return false;
    }
    auto snap1 = cs.snapshot();
    std::println("  per_defuse_index_used: {} -> {}", snap0.per_defuse_index_used_total,
                 snap1.per_defuse_index_used_total);
    std::println("  per_defuse_index_walk_fallback: {} -> {}",
                 snap0.per_defuse_index_walk_fallback_total,
                 snap1.per_defuse_index_walk_fallback_total);
    CHECK(snap1.per_defuse_index_used_total > snap0.per_defuse_index_used_total,
          "per_defuse_index_used_total incremented (path fired)");
    CHECK(snap1.per_defuse_index_walk_fallback_total > snap0.per_defuse_index_walk_fallback_total,
          "per_defuse_index_walk_fallback_total incremented (sym not in tracker → O(n) walk)");
    return true;
}

bool test_eval_still_works() {
    std::println("\n--- AC7: existing eval still works (regression) ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define x 42)\")");
    cs.eval("(eval-current)");
    auto r = cs.eval("(eval-current)");
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 42,
          "plain (define x 42) + (eval-current) returns 42");
    return true;
}

} // namespace aura_411fu4_detail

int main() {
    using namespace aura_411fu4_detail;
    std::println("=== Issue #411 fu1 follow-up #4: O(uses) wall-clock (scope-limited) ===");
    test_initial_counters_zero();
    test_caller_stores_node_id();
    test_aura_add_with_node_id();
    test_aura_callers_returns_nodeid_hash();
    test_typed_mutate_populated_tracker_bumps_visited();
    test_typed_mutate_non_tracked_sym_bumps_walk_fallback();
    test_eval_still_works();
    std::println("\n=== Summary: {}/{} passed, {}/{} failed ===", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}
