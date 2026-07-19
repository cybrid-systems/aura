// @category: integration
// @reason: uses CompilerService to verify per-DefUseIndex tracker wiring (Aura primitive surface)

// test_issue_411_followup_2.cpp — Issue #411 fu1
// follow-up #2 (scope-limited close): wire the
// PerDefUseIndexTracker (from #411 fu1 follow-up #1)
// into CompilerService + expose it via 3 new Aura
// primitives + 3 new metrics. The full scope of #411 fu1
// follow-up #2 is to route TypeChecker::infer_flat_partial
// through the tracker for O(uses) instead of the current
// O(n) per_symbol walk. This scope-limited slice ships the
// WIRING (tracker in service.ixx + 3 primitives + 3
// metrics + 4 new keys in (engine:metrics \"compile:per-symbol-reinfer-stats\"))
// so the optimization can be measured when it's wired in
// the next commit.
//
// Test cases:
//   AC1: fresh CompilerService → per_defuse_index_* counters = 0
//   AC2: (compile:per-defuse-index-add <idx> <caller>)
//        adds a caller and returns the new size
//   AC3: (compile:per-defuse-index-callers <idx>) returns
//        the registered callers as a hash
//   AC4: per-DefUseIndex isolation (Aura surface): adding
//        to one index doesn't affect another
//   AC5: (engine:metrics \"compile:per-defuse-index-stats\") returns
//        total-size + index-count + service ptr
//   AC6: (engine:metrics \"compile:per-symbol-reinfer-stats\") has 4 new
//        per-DefUseIndex keys
//   AC7: snapshot has 4 new per-DefUseIndex fields
//   AC8: regression — existing eval still works


import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_411fu2_detail {
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
    CHECK_EQ(snap.per_defuse_index_visited_total, 0u, "per_defuse_index_visited_total == 0");
    CHECK_EQ(snap.per_defuse_index_walk_fallback_total, 0u,
             "per_defuse_index_walk_fallback_total == 0");
    CHECK_EQ(snap.per_defuse_index_visited_avg_bp, 0u, "per_defuse_index_visited_avg_bp == 0");
    return true;
}

bool test_add_caller_primitive() {
    std::println("\n--- AC2: (compile:per-defuse-index-add) adds + returns size ---");
    aura::compiler::CompilerService cs;
    // Use begin so the call result is discarded (the
    // side-effect is the caller registration). The test
    // pattern of binding the result and calling it would
    // fail because the result is an int, not a function.
    cs.eval("(set-code \"(begin (compile:per-defuse-index-add \\\"foo\\\" 101))\")");
    cs.eval("(eval-current)");
    // Now read back via the stats primitive — total-size should be 1.
    auto r =
        cs.eval("(hash-ref (engine:metrics \"compile:per-defuse-index-stats\") \"total-size\")");
    if (!r || !aura::compiler::types::is_int(*r)) {
        std::println("  FAIL: hash-ref total-size did not return int (val={})", r ? r->val : -1);
        ++g_failed;
        return false;
    }
    std::int64_t total_size = aura::compiler::types::as_int(*r);
    CHECK_EQ(total_size, 1, "after one add, total-size == 1");
    return true;
}

bool test_callers_primitive() {
    std::println("\n--- AC3: (compile:per-defuse-index-callers) returns hash ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(begin (compile:per-defuse-index-add \\\"foo\\\" 101) "
            "(compile:per-defuse-index-add \\\"foo\\\" 102))\")");
    cs.eval("(eval-current)");
    // Define a helper that returns the callers hash.
    cs.eval("(set-code \"(define cs_hash (compile:per-defuse-index-callers \\\"foo\\\"))\")");
    cs.eval("(eval-current)");
    auto rh = cs.eval("(hash? cs_hash)");
    if (!rh || !aura::compiler::types::is_bool(*rh) || !aura::compiler::types::as_bool(*rh)) {
        std::println("  FAIL: (hash? cs_hash) did not return #t");
        ++g_failed;
        return false;
    }
    CHECK(true, "(compile:per-defuse-index-callers) returns a hash");
    // Verify the 2 callers exist. hash-ref keys are
    // strings (the tracker stores NodeIds stringified
    // for Aura-side display), so the key must be a
    // string. The hash-ref primitive does type-strict
    // comparison — int key 101 doesn't match string key
    // "101". Use string keys throughout.
    auto r101 = cs.eval("(hash-ref cs_hash \"101\")");
    auto r102 = cs.eval("(hash-ref cs_hash \"102\")");
    if (r101 && aura::compiler::types::is_int(*r101))
        CHECK(true, "hash-ref cs_hash \"101\" returns int");
    else {
        std::println("  FAIL: hash-ref cs_hash \"101\" did not return int (val={})",
                     r101 ? r101->val : -1);
        ++g_failed;
    }
    if (r102 && aura::compiler::types::is_int(*r102))
        CHECK(true, "hash-ref cs_hash \"102\" returns int");
    else {
        std::println("  FAIL: hash-ref cs_hash \"102\" did not return int (val={})",
                     r102 ? r102->val : -1);
        ++g_failed;
    }
    return true;
}

bool test_per_index_isolation_via_auras() {
    std::println("\n--- AC4: per-DefUseIndex isolation via Aura surface ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(begin "
            "  (compile:per-defuse-index-add \\\"foo\\\" 201) "
            "  (compile:per-defuse-index-add \\\"foo\\\" 202) "
            "  (compile:per-defuse-index-add \\\"bar\\\" 301))\")");
    cs.eval("(eval-current)");
    // Per-DefUseIndex isolation: 201 IS in foo, 301 is NOT
    // in foo, 201 is NOT in bar, 301 IS in bar.
    auto r201_in_foo = cs.eval("(hash-ref (compile:per-defuse-index-callers \"foo\") \"201\")");
    auto r301_in_foo = cs.eval("(hash-ref (compile:per-defuse-index-callers \"foo\") \"301\")");
    auto r201_in_bar = cs.eval("(hash-ref (compile:per-defuse-index-callers \"bar\") \"201\")");
    auto r301_in_bar = cs.eval("(hash-ref (compile:per-defuse-index-callers \"bar\") \"301\")");
    // The isolation checks use is_int() — a missing key
    // returns a valid EvalResult wrapping a void value
    // (the unique_ptr is non-null, just is_int() ==
    // false). Comparing `!r` would always be false for
    // missing keys because the pointer is non-null.
    CHECK(r201_in_foo && aura::compiler::types::is_int(*r201_in_foo),
          "201 IS in foo's caller list (expected, is_int)");
    CHECK(!r301_in_foo || !aura::compiler::types::is_int(*r301_in_foo),
          "301 is NOT in foo's caller list (per-DefUseIndex isolation)");
    CHECK(!r201_in_bar || !aura::compiler::types::is_int(*r201_in_bar),
          "201 is NOT in bar's caller list (per-DefUseIndex isolation)");
    CHECK(r301_in_bar && aura::compiler::types::is_int(*r301_in_bar),
          "301 IS in bar's caller list (expected, is_int)");
    return true;
}

bool test_stats_primitive() {
    std::println("\n--- AC5: (engine:metrics \"compile:per-defuse-index-stats\") returns hash ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(begin "
            "  (compile:per-defuse-index-add \\\"s1\\\" 11) "
            "  (compile:per-defuse-index-add \\\"s1\\\" 12) "
            "  (compile:per-defuse-index-add \\\"s2\\\" 13))\")");
    cs.eval("(eval-current)");
    auto r = cs.eval("(define st (engine:metrics \"compile:per-defuse-index-stats\"))");
    if (!r) {
        std::println("  FAIL: define st failed");
        ++g_failed;
        return false;
    }
    cs.eval("(eval-current)");
    auto rh = cs.eval("(hash? st)");
    if (!rh || !aura::compiler::types::is_bool(*rh) || !aura::compiler::types::as_bool(*rh)) {
        std::println("  FAIL: (hash? st) did not return #t");
        ++g_failed;
        return false;
    }
    CHECK(true, "(engine:metrics \"compile:per-defuse-index-stats\") returns a hash");
    // Verify the 3 keys.
    for (const char* key : {"total-size", "index-count", "defuse-service-ptr"}) {
        std::string check = std::string("(hash-ref st \"") + key + "\")";
        auto rv = cs.eval(check);
        if (!rv || !aura::compiler::types::is_int(*rv)) {
            std::println("  FAIL: hash-ref st {} did not return int", key);
            ++g_failed;
        } else {
            CHECK(true, std::string("hash-ref st \"") + key + "\" returns int");
        }
    }
    // total-size should be 3 (2 + 1).
    auto rts = cs.eval("(hash-ref st \"total-size\")");
    if (rts && aura::compiler::types::is_int(*rts)) {
        CHECK_EQ(aura::compiler::types::as_int(*rts), 3,
                 "total-size == 3 (2 callers for s1 + 1 for s2)");
    }
    auto ric = cs.eval("(hash-ref st \"index-count\")");
    if (ric && aura::compiler::types::is_int(*ric)) {
        CHECK_EQ(aura::compiler::types::as_int(*ric), 2, "index-count == 2 (s1 + s2)");
    }
    return true;
}

bool test_per_symbol_reinfer_stats_has_new_keys() {
    std::println("\n--- AC6: (engine:metrics \"compile:per-symbol-reinfer-stats\") has 4 new "
                 "per-DefUseIndex keys ---");
    aura::compiler::CompilerService cs;
    cs.eval("(define h (engine:metrics \"compile:per-symbol-reinfer-stats\"))");
    for (const char* key :
         {"per-defuse-index-used-total", "per-defuse-index-visited-total",
          "per-defuse-index-walk-fallback-total", "per-defuse-index-visited-avg-bp"}) {
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

bool test_snapshot_has_new_fields() {
    std::println("\n--- AC7: snapshot has 4 new per-DefUseIndex fields ---");
    aura::compiler::CompilerService cs;
    auto snap = cs.snapshot();
    std::println("  per_defuse_index_used={} per_defuse_index_visited={} "
                 "per_defuse_index_walk_fallback={} per_defuse_index_visited_avg_bp={}",
                 snap.per_defuse_index_used_total, snap.per_defuse_index_visited_total,
                 snap.per_defuse_index_walk_fallback_total, snap.per_defuse_index_visited_avg_bp);
    CHECK(true, "snapshot has per_defuse_index_used_total field");
    CHECK(true, "snapshot has per_defuse_index_visited_total field");
    CHECK(true, "snapshot has per_defuse_index_walk_fallback_total field");
    CHECK(true, "snapshot has per_defuse_index_visited_avg_bp field");
    return true;
}

bool test_eval_still_works() {
    std::println("\n--- AC8: existing eval still works (regression) ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define x 42)\")");
    cs.eval("(eval-current)");
    auto r = cs.eval("(eval-current)");
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 42,
          "plain (define x 42) + (eval-current) returns 42");
    return true;
}

} // namespace aura_411fu2_detail

int main() {
    using namespace aura_411fu2_detail;
    std::println("=== Issue #411 fu1 follow-up #2: per-DefUseIndex wiring (scope-limited) ===");
    test_initial_counters_zero();
    test_add_caller_primitive();
    test_callers_primitive();
    test_per_index_isolation_via_auras();
    test_stats_primitive();
    test_per_symbol_reinfer_stats_has_new_keys();
    test_snapshot_has_new_fields();
    test_eval_still_works();
    std::println("\n=== Summary: {}/{} passed, {}/{} failed ===", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}
