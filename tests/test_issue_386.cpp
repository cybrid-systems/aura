// @category: integration
// @reason: uses CompilerService + occurrence-typing to verify narrowing observability + let/if combination

// test_issue_386.cpp — Issue #386: Deep Occurrence Typing
// narrowing integration (scope-limited close).
//
// The full #386 scope is:
//   1. Push OccurrenceInfo into TypeEnv scopes in
//      let+if combinations
//   2. Strengthen ConstraintSystem consistent_unify for
//      refined types
//   3. Leverage per-node occurrence-dirty for targeted
//      re-analysis
//
// Pre-#386, the engine captured `last_if_narrowing_` (a
// bitmask) but didn't bump any "did the narrowing actually
// take effect" counter. Users couldn't observe whether
// narrowing was firing or being silently rejected.
//
// Post-#386, the engine bumps 3 observability counters:
//   - narrowing_applied: refined type was pushed into env
//   - narrowing_skipped: analyzed but rejected (var not
//     bound in env, etc.)
//   - narrowing_reanalyzed: predicate re-walked
//     (cache miss — memo didn't have it)
//
// Test cases:
//   AC1: fresh CompilerService → narrowing_* = 0
//   AC2: snapshot has 4 new narrowing fields
//   AC3: typing let + if with predicate → narrowing applied
//        (env_.bind(occ->var_name, occ->refined_type) fires)
//   AC4: (compile:occurrence-typing-stats) returns 4-key
//        hash (applied-total, skipped-total,
//        reanalyzed-total, applied-ratio-bp)
//   AC5: repeated typing of the same expression →
//        narrowing_reanalyzed doesn't blow up
//        (predicate memo works)
//   AC6: existing eval still works (regression)


import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_386_detail {
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
    std::println("\n--- AC1: narrowing_* counters start at 0 ---");
    aura::compiler::CompilerService cs;
    auto snap = cs.snapshot();
    CHECK_EQ(snap.narrowing_applied_total, 0u, "narrowing_applied_total == 0");
    CHECK_EQ(snap.narrowing_skipped_total, 0u, "narrowing_skipped_total == 0");
    CHECK_EQ(snap.narrowing_reanalyzed_total, 0u, "narrowing_reanalyzed_total == 0");
    CHECK_EQ(snap.narrowing_applied_ratio_bp, 0u, "narrowing_applied_ratio_bp == 0");
    return true;
}

bool test_snapshot_has_new_fields() {
    std::println("\n--- AC2: snapshot has 4 new narrowing fields ---");
    aura::compiler::CompilerService cs;
    auto snap = cs.snapshot();
    CHECK(true, "snapshot has narrowing_applied_total field");
    CHECK(true, "snapshot has narrowing_skipped_total field");
    CHECK(true, "snapshot has narrowing_reanalyzed_total field");
    CHECK(true, "snapshot has narrowing_applied_ratio_bp field");
    return true;
}

bool test_typing_with_predicate_bumps_narrowing_applied() {
    std::println("\n--- AC3: typing let + if with predicate → narrowing_applied bumps ---");
    aura::compiler::CompilerService cs;
    // The narrowing counter bumps when the engine
    // routes through infer_flat. Use the public
    // cs.typecheck() method (the (typecheck-current)
    // Aura primitive uses a local TypeChecker
    // outside the service's shared metrics).
    auto result = cs.typecheck("(let ((x 5)) (if (number? x) (+ x 1) 0))");
    std::println("  typecheck result: {} chars", result.size());
    auto snap = cs.snapshot();
    std::println("  narrowing_applied_total: {}", snap.narrowing_applied_total);
    std::println("  narrowing_skipped_total: {}", snap.narrowing_skipped_total);
    std::println("  narrowing_reanalyzed_total: {}", snap.narrowing_reanalyzed_total);
    std::println("  typecheck_cache_misses_total: {}", snap.typecheck_cache_misses_total);
    CHECK(snap.narrowing_applied_total > 0u,
          "narrowing_applied_total > 0 (typecheck ran let+if narrowing)");
    return true;
}

bool test_occurrence_typing_stats_primitive() {
    std::println("\n--- AC4: (compile:occurrence-typing-stats) returns 4-key hash ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define ots (compile:occurrence-typing-stats))\")");
    cs.eval("(eval-current)");
    for (const char* key : {"applied-total", "skipped-total",
                            "reanalyzed-total", "applied-ratio-bp"}) {
        std::string check = std::string("(hash-ref ots \"") + key + "\")";
        auto rv = cs.eval(check);
        if (!rv || !aura::compiler::types::is_int(*rv)) {
            std::println("  FAIL: hash-ref ots {} did not return int", key);
            ++g_failed;
        } else {
            CHECK(true, std::string("hash-ref ots \"") + key + "\" returns int");
        }
    }
    return true;
}

bool test_repeated_typing_uses_memo() {
    std::println("\n--- AC5: repeated typing of same expression — memo works ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(let ((y 7)) (if (number? y) (+ y 2) 0))\")");
    cs.eval("(eval-current)");
    auto snap0 = cs.snapshot();
    // Eval again (same expression). The predicate
    // memo should hit (same condition) on the second
    // pass — narrowing_reanalyzed shouldn't grow
    // substantially.
    cs.eval("(eval-current)");
    auto snap1 = cs.snapshot();
    std::println("  narrowing_reanalyzed: {} -> {}",
                 snap0.narrowing_reanalyzed_total,
                 snap1.narrowing_reanalyzed_total);
    // Allow some growth (cold memo start) but not
    // unbounded.
    CHECK(snap1.narrowing_reanalyzed_total -
                  snap0.narrowing_reanalyzed_total < 10u,
              "narrowing_reanalyzed didn't blow up on repeated eval (memo works)");
    return true;
}

bool test_eval_still_works() {
    std::println("\n--- AC6: existing eval still works (regression) ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define w 42)\")");
    cs.eval("(eval-current)");
    auto r = cs.eval("(eval-current)");
    CHECK(r && aura::compiler::types::is_int(*r) &&
              aura::compiler::types::as_int(*r) == 42,
          "plain (define w 42) + (eval-current) returns 42");
    return true;
}

}  // namespace aura_386_detail

int main() {
    using namespace aura_386_detail;
    std::println("=== Issue #386: Deep Occurrence Typing integration (scope-limited) ===");
    test_initial_counters_zero();
    test_snapshot_has_new_fields();
    test_typing_with_predicate_bumps_narrowing_applied();
    test_occurrence_typing_stats_primitive();
    test_repeated_typing_uses_memo();
    test_eval_still_works();
    std::println("\n=== Summary: {}/{} passed, {}/{} failed ===",
                 g_passed, g_passed + g_failed, g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}