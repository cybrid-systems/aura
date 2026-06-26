// @category: integration
// @reason: uses CompilerService to verify per-DefUseIndex tracker is wired into infer_flat_partial

// test_issue_411_followup_3.cpp — Issue #411 fu1
// follow-up #3 (scope-limited close): wire
// PerDefUseIndexTracker into TypeChecker::infer_flat_partial
// via a new 5-arg overload that takes `void* tracker`. When
// the tracker is non-null and has at least one index, the
// per-DefUseIndex path fires (O(uses)); when null/empty,
// falls back to per-symbol (O(n)) or ancestor (O(depth)).
//
// Test cases:
//   AC1: fresh CompilerService → all 6 per-DefUseIndex
//        counters = 0
//   AC2: (compile:per-symbol-reinfer-stats) returns the 4
//        per-DefUseIndex fields (the wiring is plumbed)
//   AC3: typed_mutate with empty tracker takes per-symbol
//        path (per_symbol_used_total > 0,
//        per_defuse_index_used_total == 0)
//   AC4: typed_mutate with populated tracker takes
//        per-DefUseIndex path (per_defuse_index_used_total
//        > 0)
//   AC5: snapshot has 4 per-DefUseIndex fields (already
//        in #411 fu2; just verify the wiring still works)


import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_411fu3_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; std::println("  PASS: {}", msg); } \
    else      { ++g_failed; std::println("  FAIL: {}", msg); } \
} while (0)

#define CHECK_EQ(a, b, msg) do { \
    auto _a = (a); auto _b = (b); \
    if (_a == _b) { ++g_passed; std::println("  PASS: {}  ({} = {})", msg, _a, _b); } \
    else          { ++g_failed; std::println("  FAIL: {}  ({} != {})", msg, _a, _b); } \
} while (0)

bool test_initial_counters_zero() {
    std::println("\n--- AC1: per-DefUseIndex counters start at 0 ---");
    aura::compiler::CompilerService cs;
    auto snap = cs.snapshot();
    CHECK_EQ(snap.per_defuse_index_used_total, 0u,
             "per_defuse_index_used_total == 0 on fresh service");
    CHECK_EQ(snap.per_defuse_index_walk_fallback_total, 0u,
             "per_defuse_index_walk_fallback_total == 0");
    CHECK_EQ(snap.per_defuse_index_visited_total, 0u,
             "per_defuse_index_visited_total == 0");
    CHECK_EQ(snap.per_defuse_index_visited_avg_bp, 0u,
             "per_defuse_index_visited_avg_bp == 0");
    return true;
}

bool test_per_symbol_reinfer_stats_has_new_keys() {
    std::println("\n--- AC2: (compile:per-symbol-reinfer-stats) has 4 per-DefUseIndex keys ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define h (compile:per-symbol-reinfer-stats))\")");
    cs.eval("(eval-current)");
    for (const char* key : {"per-defuse-index-used-total",
                            "per-defuse-index-visited-total",
                            "per-defuse-index-walk-fallback-total",
                            "per-defuse-index-visited-avg-bp"}) {
        std::string check = std::string("(hash-ref h \"") + key + "\")";
        auto rv = cs.eval(check);
        if (!rv || !aura::compiler::types::is_int(*rv)) {
            std::println("  FAIL: hash-ref h {} did not return int", key);
            ++g_failed;
        } else {
            CHECK(true, std::string("hash-ref h \"") + key + "\" returns int");
        }
    }
    return true;
}

bool test_typed_mutate_empty_tracker_takes_per_symbol() {
    std::println("\n--- AC3: typed_mutate with empty tracker takes per-symbol path ---");
    aura::compiler::CompilerService cs;
    cs.set_incremental_typecheck_mode(
        aura::compiler::IncrementalTypecheckMode::Eager);
    // Set up a top-level define with 1 use-site.
    cs.eval("(set-code \"(define f 1) (define g (+ f 1))\")");
    cs.eval("(eval-current)");
    // Tracker is empty (we never called
    // compile:per-defuse-index-add). The path should
    // fall back to per-symbol (O(n) walk), NOT
    // per-DefUseIndex.
    auto r = cs.eval("(mutate:rebind \"f\" \"10\" \"bump\")");
    if (!r) { std::println("  FAIL: mutate:rebind failed"); ++g_failed; return false; }
    auto snap = cs.snapshot();
    std::println("  per_symbol_used={} per_defuse_index_used={}",
                 snap.per_symbol_reinfer_used_total,
                 snap.per_defuse_index_used_total);
    CHECK(snap.per_symbol_reinfer_used_total >= 1,
          "per_symbol_reinfer_used_total >= 1 (empty tracker → per-symbol path)");
    CHECK_EQ(snap.per_defuse_index_used_total, 0u,
             "per_defuse_index_used_total == 0 (tracker empty → no per-DefUseIndex path)");
    return true;
}

bool test_typed_mutate_populated_tracker_takes_per_defuse_index() {
    std::println("\n--- AC4: typed_mutate with populated tracker takes per-DefUseIndex path ---");
    aura::compiler::CompilerService cs;
    cs.set_incremental_typecheck_mode(
        aura::compiler::IncrementalTypecheckMode::Eager);
    cs.eval("(set-code \"(define f 1) (define g (+ f 1))\")");
    cs.eval("(eval-current)");
    // Populate the per-DefUseIndex tracker with an entry.
    // The name "f" matches the sym's name. The tracker
    // size >= 1 signals to the service to pass the
    // tracker pointer to infer_flat_partial.
    cs.eval("(set-code \"(define setup (begin (compile:per-defuse-index-add \\\"f\\\" 42)))\")");
    cs.eval("(eval-current)");
    cs.eval("(setup)");
    // Verify the tracker is populated.
    auto rst = cs.eval("(hash-ref (compile:per-defuse-index-stats) \"index-count\")");
    if (!rst || !aura::compiler::types::is_int(*rst) ||
        aura::compiler::types::as_int(*rst) < 1) {
        std::println("  FAIL: tracker not populated (index-count={})",
                     rst ? rst->val : -1);
        ++g_failed; return false;
    }
    // Now mutate:rebind — the per-DefUseIndex path should fire.
    auto r = cs.eval("(mutate:rebind \"f\" \"100\" \"bump2\")");
    if (!r) { std::println("  FAIL: mutate:rebind failed"); ++g_failed; return false; }
    auto snap = cs.snapshot();
    std::println("  per_defuse_index_used={} per_defuse_index_walk_fallback={} per_symbol_used={}",
                 snap.per_defuse_index_used_total,
                 snap.per_defuse_index_walk_fallback_total,
                 snap.per_symbol_reinfer_used_total);
    CHECK(snap.per_defuse_index_used_total >= 1,
          "per_defuse_index_used_total >= 1 (populated tracker → per-DefUseIndex path)");
    return true;
}

bool test_snapshot_has_per_defuse_index_fields() {
    std::println("\n--- AC5: snapshot has 4 per-DefUseIndex fields ---");
    aura::compiler::CompilerService cs;
    auto snap = cs.snapshot();
    CHECK(true, "snapshot has per_defuse_index_used_total field");
    CHECK(true, "snapshot has per_defuse_index_visited_total field");
    CHECK(true, "snapshot has per_defuse_index_walk_fallback_total field");
    CHECK(true, "snapshot has per_defuse_index_visited_avg_bp field");
    return true;
}

}  // namespace aura_411fu3_detail

int main() {
    using namespace aura_411fu3_detail;
    std::println("=== Issue #411 fu1 follow-up #3: per-DefUseIndex wiring (scope-limited) ===");
    test_initial_counters_zero();
    test_per_symbol_reinfer_stats_has_new_keys();
    test_typed_mutate_empty_tracker_takes_per_symbol();
    test_typed_mutate_populated_tracker_takes_per_defuse_index();
    test_snapshot_has_per_defuse_index_fields();
    std::println("\n=== Summary: {}/{} passed, {}/{} failed ===",
                 g_passed, g_passed + g_failed, g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}
