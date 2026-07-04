// @category: integration
// @reason: uses CompilerService to verify post-mutation auto-incremental typecheck wiring

// test_issue_411.cpp — Issue #411 scope-limited close:
// Make infer_flat_partial the primary path after typed_mutate
// (wiring foundation + observability).
//
// Issue #411's full scope is: after a successful (typed_mutate …)
// call, (query:type <name>) and (get-inferred-type <node-id>)
// should return up-to-date types immediately (no manual
// (typecheck-incremental) call). Plus a configuration flag for
// eager vs lazy. That full path requires: (1) wiring
// infer_flat_partial into the typed_mutate success path so the
// affected nodes have updated type_id_ cached, and (2) a
// (compile:incremental-typecheck-stats) observability primitive
// so users can measure how often the auto-path runs and how
// many nodes it re-infers. The per-symbol optimization (which
// will reduce the avg re-inferred count) is a separate follow-up
// tracked under #410 Phase 2/2.
//
// This scope-limited close ships the WIRING FOUNDATION:
//
// 1. CompilerMetrics gains 2 lifetime-total counters:
//    - incremental_typecheck_auto_invocations_total
//    - incremental_typecheck_re_inferred_total
// 2. CompilerSnapshot mirrors the 2 + derives
//    incremental_typecheck_avg_re_inferred_bp
//    (re_inferred * 10000 / auto_invocations, in basis points).
// 3. CompilerService::IncrementalTypecheckMode enum + field
//    + accessors (Eager / Lazy / Disabled; default Eager).
// 4. typed_mutate's success path now auto-invokes
//    TypeChecker::infer_flat_partial on the workspace flat
//    (post-COW) using the most recent mutation record when the
//    mode is Eager. The same path that the manual
//    (typecheck-incremental) Aura primitive uses.
// 5. (compile:incremental-typecheck-stats) Aura primitive returns
//    a hash with 3 fields: auto-invocations-total,
//    re-inferred-total, avg-re-inferred-bp.
//
// Test cases:
//   AC1: fresh CompilerService → snapshot fields start at 0
//   AC2: (compile:incremental-typecheck-stats) returns a hash
//        with 3 keys
//   AC3: typed_mutate in Eager mode auto-invokes infer_flat_partial
//        (auto_invocations_total increments + re_inferred_total > 0)
//   AC4: typed_mutate in Lazy mode does NOT auto-invoke
//   AC5: typed_mutate in Disabled mode does NOT auto-invoke
//   AC6: avg-re-inferred-bp computes correctly (re_inferred /
//        auto_invocations * 10000)
//   AC7: query-type-of returns up-to-date result after a
//        typed_mutate in Eager mode (the actual AC from #411)
//   AC8: full regression — existing eval still works


import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_411_detail {
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
    std::println("\n--- AC1: post-mutation auto-incremental typecheck counters start at 0 ---");
    aura::compiler::CompilerService cs;
    auto snap = cs.snapshot();
    CHECK_EQ(snap.incremental_typecheck_auto_invocations_total, 0u,
             "incremental_typecheck_auto_invocations_total == 0");
    CHECK_EQ(snap.incremental_typecheck_re_inferred_total, 0u,
             "incremental_typecheck_re_inferred_total == 0");
    CHECK_EQ(snap.incremental_typecheck_avg_re_inferred_bp, 0u,
             "incremental_typecheck_avg_re_inferred_bp == 0");
    // Also confirm the default mode is Eager.
    auto mode = cs.incremental_typecheck_mode();
    CHECK(mode == aura::compiler::IncrementalTypecheckMode::Eager,
          "default mode is Eager (the auto-invocation-on path)");
    return true;
}

bool test_aura_primitive_returns_hash() {
    std::println("\n--- AC2: (compile:incremental-typecheck-stats) returns hash with 3 keys ---");
    aura::compiler::CompilerService cs;
    auto r1 = cs.eval("(set-code \"(define h (compile:incremental-typecheck-stats))\")");
    if (!r1) {
        std::println("  FAIL: define h failed");
        ++g_failed;
        return false;
    }
    auto r2 = cs.eval("(eval-current)");
    if (!r2) {
        std::println("  FAIL: eval-current failed");
        ++g_failed;
        return false;
    }
    auto rh = cs.eval("(hash? h)");
    if (!rh || !aura::compiler::types::is_bool(*rh) || !aura::compiler::types::as_bool(*rh)) {
        std::println("  FAIL: (hash? h) did not return #t (val={})", rh ? rh->val : -1);
        ++g_failed;
        return false;
    }
    CHECK(true, "(compile:incremental-typecheck-stats) returns a hash");
    // Verify the 3 keys exist with int values.
    for (const char* key : {"auto-invocations-total", "re-inferred-total", "avg-re-inferred-bp"}) {
        std::string check = std::string("(hash-ref h \"") + key + "\")";
        auto rv = cs.eval(check);
        if (!rv || !aura::compiler::types::is_int(*rv)) {
            std::println("  FAIL: hash-ref h {} did not return int (val={})", key,
                         rv ? rv->val : -1);
            ++g_failed;
        } else {
            CHECK(true, std::string("hash-ref h \"") + key + "\" returns int");
        }
    }
    return true;
}

bool test_eager_mode_auto_invokes() {
    std::println("\n--- AC3: typed_mutate in Eager mode auto-invokes infer_flat_partial ---");
    aura::compiler::CompilerService cs;
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Eager);
    cs.eval("(set-code \"(define x 1) (define y 2) (define z 3)\")");
    auto r_eval = cs.eval("(eval-current)");
    if (!r_eval) {
        std::println("  FAIL: initial eval-current failed");
        ++g_failed;
        return false;
    }
    // Now run a typed_mutate. In Eager mode, the success path
    // should auto-invoke infer_flat_partial on the affected
    // subtree, bumping auto_invocations_total and
    // re_inferred_total.
    auto snap0 = cs.snapshot();
    auto r_mut = cs.eval("(mutate:rebind \"x\" \"42\" \"bump\")");
    if (!r_mut ||
        !(aura::compiler::types::is_int(*r_mut) || aura::compiler::types::is_bool(*r_mut))) {
        std::println("  FAIL: mutate:rebind failed (val={})", r_mut ? r_mut->val : -1);
        ++g_failed;
        return false;
    }
    auto snap1 = cs.snapshot();
    std::println("  auto_invocations: {} -> {}", snap0.incremental_typecheck_auto_invocations_total,
                 snap1.incremental_typecheck_auto_invocations_total);
    std::println("  re_inferred:      {} -> {}", snap0.incremental_typecheck_re_inferred_total,
                 snap1.incremental_typecheck_re_inferred_total);
    CHECK(snap1.incremental_typecheck_auto_invocations_total >
              snap0.incremental_typecheck_auto_invocations_total,
          "auto_invocations_total incremented after typed_mutate (Eager mode)");
    CHECK(snap1.incremental_typecheck_re_inferred_total >
              snap0.incremental_typecheck_re_inferred_total,
          "re_inferred_total > 0 after typed_mutate (re-inferred the affected subtree)");
    return true;
}

bool test_lazy_mode_does_not_invoke() {
    std::println("\n--- AC4: typed_mutate in Lazy mode does NOT auto-invoke ---");
    aura::compiler::CompilerService cs;
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Lazy);
    cs.eval("(set-code \"(define x 1) (define y 2)\")");
    cs.eval("(eval-current)");
    auto snap0 = cs.snapshot();
    auto r_mut = cs.eval("(mutate:rebind \"x\" \"42\" \"bump\")");
    if (!r_mut ||
        !(aura::compiler::types::is_int(*r_mut) || aura::compiler::types::is_bool(*r_mut))) {
        std::println("  FAIL: mutate:rebind failed");
        ++g_failed;
        return false;
    }
    auto snap1 = cs.snapshot();
    CHECK_EQ(snap1.incremental_typecheck_auto_invocations_total,
             snap0.incremental_typecheck_auto_invocations_total,
             "Lazy mode: auto_invocations_total unchanged");
    CHECK_EQ(snap1.incremental_typecheck_re_inferred_total,
             snap0.incremental_typecheck_re_inferred_total,
             "Lazy mode: re_inferred_total unchanged (manual typecheck-incremental needed)");
    return true;
}

bool test_disabled_mode_does_not_invoke() {
    std::println("\n--- AC5: typed_mutate in Disabled mode does NOT auto-invoke ---");
    aura::compiler::CompilerService cs;
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Disabled);
    cs.eval("(set-code \"(define x 1) (define y 2)\")");
    cs.eval("(eval-current)");
    auto snap0 = cs.snapshot();
    auto r_mut = cs.eval("(mutate:rebind \"x\" \"42\" \"bump\")");
    if (!r_mut ||
        !(aura::compiler::types::is_int(*r_mut) || aura::compiler::types::is_bool(*r_mut))) {
        std::println("  FAIL: mutate:rebind failed");
        ++g_failed;
        return false;
    }
    auto snap1 = cs.snapshot();
    CHECK_EQ(snap1.incremental_typecheck_auto_invocations_total,
             snap0.incremental_typecheck_auto_invocations_total,
             "Disabled mode: auto_invocations_total unchanged (pre-#411 behavior)");
    CHECK_EQ(snap1.incremental_typecheck_re_inferred_total,
             snap0.incremental_typecheck_re_inferred_total,
             "Disabled mode: re_inferred_total unchanged");
    return true;
}

bool test_avg_re_inferred_bp_computes_correctly() {
    std::println("\n--- AC6: avg-re-inferred-bp matches manual calculation ---");
    aura::compiler::CompilerService cs;
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Eager);
    // 3 mutations across 3 different symbols. Each mutation
    // bumps auto_invocations_total by 1. We don't know the
    // exact re_inferred count (depends on the AST), but the
    // avg_bp = re_inferred * 10000 / auto_invocations should
    // be derivable from the 2 raw counters.
    cs.eval("(set-code \"(define a 1) (define b 2) (define c 3)\")");
    cs.eval("(eval-current)");
    cs.eval("(mutate:rebind \"a\" \"10\" \"v1\")");
    cs.eval("(mutate:rebind \"b\" \"20\" \"v2\")");
    cs.eval("(mutate:rebind \"c\" \"30\" \"v3\")");
    auto snap = cs.snapshot();
    std::println("  auto_invocations={}, re_inferred={}, avg_bp={}",
                 snap.incremental_typecheck_auto_invocations_total,
                 snap.incremental_typecheck_re_inferred_total,
                 snap.incremental_typecheck_avg_re_inferred_bp);
    CHECK(snap.incremental_typecheck_auto_invocations_total >= 3,
          "auto_invocations >= 3 (3 mutations each bumped it)");
    if (snap.incremental_typecheck_auto_invocations_total > 0) {
        const std::uint64_t expected_bp = (snap.incremental_typecheck_re_inferred_total * 10000u) /
                                          snap.incremental_typecheck_auto_invocations_total;
        CHECK_EQ(snap.incremental_typecheck_avg_re_inferred_bp, expected_bp,
                 "avg-re-inferred-bp matches manual calc");
    } else {
        CHECK(true, "skip ratio check when auto_invocations == 0 (no mutations applied)");
    }
    return true;
}

bool test_query_type_of_uptodate_in_eager_mode() {
    std::println(
        "\n--- AC7: query-type-of returns up-to-date result after typed_mutate (Eager) ---");
    aura::compiler::CompilerService cs;
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Eager);
    // Set up: a top-level define f that's a number. After a
    // mutation that changes the value, the query-type-of
    // should still return a string (the lookup works) AND
    // the auto_invocations counter should increment (the
    // auto-invoke happened). The exact type string is
    // renderer-specific so we don't pin it.
    cs.eval("(set-code \"(define f 42)\")");
    cs.eval("(eval-current)");
    // (query-type-of "f") before mutation.
    auto r_before = cs.eval("(query-type-of \"f\")");
    if (!r_before || !aura::compiler::types::is_string(*r_before)) {
        std::println("  FAIL: query-type-of pre-mutation failed (val={})",
                     r_before ? r_before->val : -1);
        ++g_failed;
        return false;
    }
    CHECK(true, "query-type-of returns a string before mutation");
    // Now mutate f to a different value.
    auto r_mut = cs.eval("(mutate:rebind \"f\" \"100\" \"bump\")");
    if (!r_mut) {
        std::println("  FAIL: mutate:rebind failed (val={})", r_mut ? r_mut->val : -1);
        ++g_failed;
        return false;
    }
    // Immediately query-type-of — no manual typecheck-incremental.
    auto r_after = cs.eval("(query-type-of \"f\")");
    if (!r_after || !aura::compiler::types::is_string(*r_after)) {
        std::println("  FAIL: query-type-of post-mutation failed (val={})",
                     r_after ? r_after->val : -1);
        ++g_failed;
        return false;
    }
    CHECK(true, "query-type-of returns a string after mutation (no manual typecheck-incremental)");
    // The authoritative AC check: the auto_invocations counter
    // must have incremented, proving the typed_mutate's
    // success path auto-invoked infer_flat_partial. Without
    // the auto-invoke, the cached type_id for f's value
    // subtree would be stale and (query-type-of "f") could
    // return "unknown" or hit the no-typecheck-yet fallback.
    auto snap = cs.snapshot();
    std::println("  auto_invocations={} (must be >= 1 to prove auto-invoke)",
                 snap.incremental_typecheck_auto_invocations_total);
    CHECK(snap.incremental_typecheck_auto_invocations_total >= 1,
          "auto-invoke happened (auto_invocations_total >= 1) — "
          "the wired post-mutation infer_flat_partial ran");
    return true;
}

bool test_no_regression() {
    std::println("\n--- AC8: existing eval still works ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define x 42)\")");
    auto r = cs.eval("(eval-current)");
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 42,
          "plain (define x 42) + (eval-current) returns 42");
    return true;
}

} // namespace aura_issue_411_detail

int main() {
    using namespace aura_issue_411_detail;
    std::println("=== Issue #411: post-mutation auto-incremental typecheck (scope-limited) ===");
    test_initial_counters_zero();
    test_aura_primitive_returns_hash();
    test_eager_mode_auto_invokes();
    test_lazy_mode_does_not_invoke();
    test_disabled_mode_does_not_invoke();
    test_avg_re_inferred_bp_computes_correctly();
    test_query_type_of_uptodate_in_eager_mode();
    test_no_regression();
    std::println("\n=== Summary: {}/{} passed, {}/{} failed ===", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}
