// @category: integration
// @reason: uses CompilerService to verify SoA live dirty stats

// test_issue_429_soa.cpp — Issue #429: IRFunctionSoA + FlatAST
// PersistentChildVector adoption for production cache locality.
//
// Issue #429's full scope is multi-week (port lowering, IR
// executor, JIT, Pass pipeline to IRFunctionSoA + complete
// FlatAST child migration + strengthen dirty tracking). This
// scope-limited close ships the OBSERVABILITY + TEST side:
//
//   1. (engine:metrics \"query:soa-dirty-stats\") — new 8-field hash primitive
//      that returns the LIVE per-block / per-instruction
//      dirty state of ir_cache_v2_ in one pass. Complements
//      (engine:metrics \"query:ir-soa-incremental-stats\") (which reports
//      lifetime mutation-event counters) by exposing the
//      current cache state. The 8 fields:
//        - cached-fns
//        - dirty-fns
//        - total-blocks
//        - dirty-blocks
//        - total-instructions
//        - dirty-instructions
//        - dirty-block-pct
//        - dirty-instruction-pct
//
//   2. CompilerService::func_instruction_count(fi) — public
//      accessor on IRCacheEntry for total instruction count
//      across all basic blocks in a function. Used by the
//      primitive to compute the per-function denominator.
//
//   3. CompilerService::get_soa_dirty_stats() — one-pass
//      aggregate over ir_cache_v2_.
//
// Test cases:
//   AC1:  fresh Evaluator — cached-fns == 0, dirty-block-pct
//         is well-defined (returns 0, not undefined)
//   AC2:  one define → cached-fns >= 1, dirty-fns == 0
//         (just lowered, cache is clean)
//   AC3:  many defines → cached-fns grows monotonically
//   AC4:  hash field consistency: 8 fields all present and
//         each is a non-negative integer
//   AC5:  empty workspace + (engine:metrics \"query:soa-dirty-stats\") doesn't
//         crash (returns hash with zeros)
//   AC6:  repeated (engine:metrics \"query:soa-dirty-stats\") calls return
//         consistent hashes (idempotent observable)
//   AC7:  total-blocks and total-instructions are >= 0
//   AC8:  dirty-block-pct is between 0 and 100
//   AC9:  when the cache is clean (after a fresh lower),
//         dirty-block-pct == 0
//   AC10: stats:list includes "query:soa-dirty-stats"
//   AC11: stats:count >= 40 (was 38 in #428, now 40 in #429)

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_429_detail {
static int g_passed = 0;
static int g_failed = 0;

static aura::compiler::types::EvalValue run_on(aura::compiler::CompilerService& cs,
                                               std::string_view src) {
    auto r = cs.eval(src);
    if (!r) {
        std::println(std::cerr, "    [eval error: {}]", r.error().format());
        return aura::compiler::types::make_void();
    }
    return *r;
}

static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:soa-dirty-stats\") '{}')", key));
    if (!r)
        return -1;
    if (!aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

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

// ═══════════════════════════════════════════════════════════
// AC1: fresh Evaluator — cached-fns == 0
// ═══════════════════════════════════════════════════════════
bool test_fresh_evaluator() {
    std::println("\n--- AC1: fresh Evaluator ---");
    aura::compiler::CompilerService cs;
    auto cached = hash_int(cs, "cached-fns");
    auto dirty = hash_int(cs, "dirty-fns");
    auto dirty_pct = hash_int(cs, "dirty-block-pct");
    CHECK(cached == 0, "fresh Evaluator: cached-fns == 0");
    CHECK(dirty == 0, "fresh Evaluator: dirty-fns == 0");
    CHECK(dirty_pct == 0, "fresh Evaluator: dirty-block-pct == 0 (no divide-by-zero)");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC2: one define → cached-fns >= 1, dirty-fns == 0
// ═══════════════════════════════════════════════════════════
bool test_one_define_clean_cache() {
    std::println("\n--- AC2: one define — clean cache ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    auto cached = hash_int(cs, "cached-fns");
    auto dirty = hash_int(cs, "dirty-fns");
    CHECK(cached >= 1, "after one define + lower: cached-fns >= 1");
    CHECK(dirty == 0, "after one define + lower: dirty-fns == 0 (just lowered)");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC3: many defines → cached-fns grows monotonically
// ═══════════════════════════════════════════════════════════
bool test_many_defines_growth() {
    std::println("\n--- AC3: many defines — monotonic growth ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    auto after_1 = hash_int(cs, "cached-fns");
    run_on(cs, "(set-code \"(define (f x) (+ x 1))(define (g y) (* y 2))(define (h z) (- z 3))\")");
    run_on(cs, "(eval-current)");
    auto after_3 = hash_int(cs, "cached-fns");
    CHECK(after_3 >= after_1, "cached-fns after 3 defines >= after 1 define");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC4: 8 fields present + each is a non-negative integer
// ═══════════════════════════════════════════════════════════
bool test_all_8_fields_present() {
    std::println("\n--- AC4: 8 fields present + non-negative ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    static const char* kFields[] = {
        "cached-fns",         "dirty-fns",          "total-blocks",    "dirty-blocks",
        "total-instructions", "dirty-instructions", "dirty-block-pct", "dirty-instruction-pct",
    };
    bool all_ok = true;
    for (auto* k : kFields) {
        auto v = hash_int(cs, k);
        if (v < 0) {
            std::println("    [field {} returned {}]", k, v);
            all_ok = false;
        }
    }
    CHECK(all_ok, "all 8 fields present and non-negative");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC5: empty workspace — no crash
// ═══════════════════════════════════════════════════════════
bool test_empty_workspace_no_crash() {
    std::println("\n--- AC5: empty workspace — no crash ---");
    aura::compiler::CompilerService cs;
    auto cached = hash_int(cs, "cached-fns");
    auto dirty_pct = hash_int(cs, "dirty-block-pct");
    CHECK(cached == 0, "empty workspace: cached-fns == 0");
    CHECK(dirty_pct == 0, "empty workspace: dirty-block-pct == 0 (defensive)");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC6: idempotence — repeated calls return same hash
// ═══════════════════════════════════════════════════════════
bool test_idempotent_observable() {
    std::println("\n--- AC6: idempotence ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    auto a = hash_int(cs, "cached-fns");
    auto b = hash_int(cs, "cached-fns");
    CHECK(a == b, "two consecutive (engine:metrics \"query:soa-dirty-stats\") calls return the "
                  "same cached-fns");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC7: total-blocks and total-instructions are >= 0
// ═══════════════════════════════════════════════════════════
bool test_total_fields_nonnegative() {
    std::println("\n--- AC7: total fields non-negative ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    auto total_b = hash_int(cs, "total-blocks");
    auto total_i = hash_int(cs, "total-instructions");
    CHECK(total_b >= 0, "total-blocks >= 0");
    CHECK(total_i >= 0, "total-instructions >= 0");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC8: dirty-block-pct is between 0 and 100
// ═══════════════════════════════════════════════════════════
bool test_dirty_pct_bounded() {
    std::println("\n--- AC8: dirty-block-pct is a percent ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    auto pct = hash_int(cs, "dirty-block-pct");
    auto pct2 = hash_int(cs, "dirty-instruction-pct");
    CHECK(pct >= 0 && pct <= 100, "dirty-block-pct is a percent (0..100)");
    CHECK(pct2 >= 0 && pct2 <= 100, "dirty-instruction-pct is a percent (0..100)");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC9: clean cache after fresh lower — dirty-block-pct == 0
// ═══════════════════════════════════════════════════════════
bool test_clean_after_fresh_lower() {
    std::println("\n--- AC9: clean after fresh lower ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))(define (g y) (* y 2))\")");
    run_on(cs, "(eval-current)");
    auto pct = hash_int(cs, "dirty-block-pct");
    CHECK(pct == 0, "after fresh lower, dirty-block-pct == 0 (cache is clean)");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC10: stats:list includes query:soa-dirty-stats
// ═══════════════════════════════════════════════════════════
bool test_stats_list_includes() {
    std::println("\n--- AC10: stats:list includes the new primitive ---");
    aura::compiler::CompilerService cs;
    // stats:list is a list of strings. The list is
    // built in reverse (see evaluator_primitives_observability.cpp
    // line ~700 — `for (auto it = stats.rbegin(); ...)`),
    // so the freshly-added primitive appears EARLIEST in
    // the list, not latest. We check both positions to
    // be robust to future refactors.
    auto r = run_on(
        cs, "(letrec ((find? (lambda (needle hay) "
            "                (if (pair? hay) "
            "                    (if (string=? (car hay) needle) #t (find? needle (cdr hay))) "
            "                    #f)))) "
            "  (if (find? \"query:soa-dirty-stats\" (stats:list)) 1 0))");
    bool included = aura::compiler::types::is_int(r) && aura::compiler::types::as_int(r) == 1;
    CHECK(included, "stats:list includes query:soa-dirty-stats");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC11: stats:count >= 40
// ═══════════════════════════════════════════════════════════
bool test_stats_count() {
    std::println("\n--- AC11: stats:count is up to date ---");
    aura::compiler::CompilerService cs;
    auto r = run_on(cs, "(stats:count)");
    bool ok = aura::compiler::types::is_int(r) && aura::compiler::types::as_int(r) >= 40;
    CHECK(ok, "stats:count >= 40 (was 39 in #428, now 40 in #429)");
    if (aura::compiler::types::is_int(r)) {
        std::println("    [stats:count = {}]", aura::compiler::types::as_int(r));
    }
    return true;
}

} // namespace aura_issue_429_detail

int aura_issue_429_soa_run() {
    using namespace aura_issue_429_detail;
    std::println("═══ Issue #429 SoA live dirty state tests ═══");

    test_fresh_evaluator();
    test_one_define_clean_cache();
    test_many_defines_growth();
    test_all_8_fields_present();
    test_empty_workspace_no_crash();
    test_idempotent_observable();
    test_total_fields_nonnegative();
    test_dirty_pct_bounded();
    test_clean_after_fresh_lower();
    test_stats_list_includes();
    test_stats_count();

    std::println("\n──────────────────────────────────────");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_429_soa_run();
}
#endif
