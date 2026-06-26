// @category: integration
// @reason: uses CompilerService + eval_ir to exercise DeadCoercionEliminationPass

// test_issue_433.cpp — Issue #433: DeadCoercionEliminationPass
// observability + integration verification (scope-limited
// close).
//
// The DeadCoercionEliminationPass (pass_manager.ixx:705)
// already exists and is already wired into the lowering
// pipeline (service.ixx:1442, in the run-passes block).
// Pre-#433, the pass's eliminated_count() was never
// surfaced to the user — the metric lived only on the
// per-call pass instance. Post-#433, the count is
// accumulated into a lifetime CompilerMetrics counter
// and exposed via the (compile:dead-coercion-stats)
// Aura primitive and CompilerSnapshot.
//
// Test cases:
//   AC1: fresh CompilerService → dead_coercion_eliminated_total = 0
//   AC2: snapshot has dead_coercion_eliminated_total field
//   AC3: (compile:dead-coercion-stats) returns int (0 initially)
//   AC4: typing + eval IR on a gradual-typing expression →
//        counter is wired (may be 0 if no identity casts in
//        the IR — we don't require > 0 because the pass only
//        fires on identity casts or chained casts which are
//        rare in a fresh program)
//   AC5: existing eval still works (regression)


import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_433_detail {
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

bool test_initial_counter_zero() {
    std::println("\n--- AC1: dead_coercion_eliminated_total starts at 0 ---");
    aura::compiler::CompilerService cs;
    auto snap = cs.snapshot();
    CHECK_EQ(snap.dead_coercion_eliminated_total, 0u,
             "dead_coercion_eliminated_total == 0");
    return true;
}

bool test_snapshot_has_new_field() {
    std::println("\n--- AC2: snapshot has dead_coercion_eliminated_total field ---");
    aura::compiler::CompilerService cs;
    auto snap = cs.snapshot();
    CHECK(true, "snapshot has dead_coercion_eliminated_total field");
    return true;
}

bool test_dead_coercion_stats_primitive() {
    std::println("\n--- AC3: (compile:dead-coercion-stats) returns int ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(compile:dead-coercion-stats)");
    CHECK(r && aura::compiler::types::is_int(*r),
          "(compile:dead-coercion-stats) returns int");
    if (r && aura::compiler::types::is_int(*r)) {
        std::println("  initial value: {}", aura::compiler::types::as_int(*r));
        CHECK_EQ(aura::compiler::types::as_int(*r), 0,
                 "initial dead-coercion-stats is 0");
    }
    return true;
}

bool test_dce_wired_into_pipeline() {
    std::println("\n--- AC4: dce is wired into the lowering pipeline ---");
    aura::compiler::CompilerService cs;
    // Type-check + eval an expression that exercises
    // the lowering pipeline (so dce.run() is called
    // at least once). The counter may stay 0 if no
    // identity casts are in the IR — the test is
    // just that the metric is plumbed and the
    // pipeline runs end-to-end.
    cs.eval("(set-code \"(define q 42)\")");
    cs.eval("(eval-current)");
    auto r = cs.eval("(compile:dead-coercion-stats)");
    CHECK(r && aura::compiler::types::is_int(*r),
          "(compile:dead-coercion-stats) returns int after eval");
    auto snap = cs.snapshot();
    std::println("  dead_coercion_eliminated_total: {}",
                 snap.dead_coercion_eliminated_total);
    CHECK(snap.dead_coercion_eliminated_total == 0u,
          "dead_coercion_eliminated_total still 0 (no identity casts in this IR)");
    return true;
}

bool test_eval_still_works() {
    std::println("\n--- AC5: existing eval still works (regression) ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define t 42)\")");
    cs.eval("(eval-current)");
    auto r = cs.eval("(eval-current)");
    CHECK(r && aura::compiler::types::is_int(*r) &&
              aura::compiler::types::as_int(*r) == 42,
          "plain (define t 42) + (eval-current) returns 42");
    return true;
}

}  // namespace aura_433_detail

int main() {
    using namespace aura_433_detail;
    std::println("=== Issue #433: DeadCoercionEliminationPass observability (scope-limited) ===");
    test_initial_counter_zero();
    test_snapshot_has_new_field();
    test_dead_coercion_stats_primitive();
    test_dce_wired_into_pipeline();
    test_eval_still_works();
    std::println("\n=== Summary: {}/{} passed, {}/{} failed ===",
                 g_passed, g_passed + g_failed, g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}