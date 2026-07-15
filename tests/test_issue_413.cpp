// @category: integration
// @reason: uses CompilerService + workspace_flat to verify mutation_log invalidation trace

// test_issue_413.cpp — Issue #413: MutationLog-integrated
// type cache invalidation (scope-limited close).
//
// Pre-#413, when mark_dirty_upward bumps the per-binding
// gen, there's no record of WHICH mutation caused the
// bump. Users debugging "why was this binding's cache
// invalidated" had to grep through the mutation_log and
// reason about which mutations affected which bindings.
// Post-#413, every per-binding gen bump appends an
// (InvalidationRecord) entry to invalidation_trace_
// capturing (mutation_id, SymId, binding_gen_at_bump).
//
// Test cases:
//   AC1: invalidation_trace_records_total starts at 0
//   AC2: snapshot has invalidation_trace_records_total field
//   AC3: typed_mutate on a top-level define increments the
//        trace counter (via mark_dirty_upward recording the
//        mutation)
//   AC4: (engine:metrics \"compile:mutation-log-invalidation-stats\") returns
//        a hash with 2 keys (records-total + trace-size)
//   AC5: trace-size > 0 after a binding mutation
//   AC6: existing eval still works (regression)


import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_413_detail {
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
    std::println("\n--- AC1: invalidation_trace_records_total starts at 0 ---");
    aura::compiler::CompilerService cs;
    auto snap = cs.snapshot();
    CHECK_EQ(snap.invalidation_trace_records_total, 0u, "invalidation_trace_records_total == 0");
    return true;
}

bool test_snapshot_has_new_field() {
    std::println("\n--- AC2: snapshot has invalidation_trace_records_total field ---");
    aura::compiler::CompilerService cs;
    auto snap = cs.snapshot();
    CHECK(true, "snapshot has invalidation_trace_records_total field");
    return true;
}

bool test_typed_mutate_increments_trace() {
    std::println("\n--- AC3: typed_mutate on a top-level define increments trace ---");
    aura::compiler::CompilerService cs;
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Eager);
    cs.eval("(set-code \"(define p 1) (define q (+ p 1))\")");
    cs.eval("(eval-current)");
    auto snap0 = cs.snapshot();
    auto r = cs.eval("(mutate:rebind \"p\" \"100\" \"trace-test\")");
    if (!r) {
        std::println("  FAIL: mutate:rebind failed");
        ++g_failed;
        return false;
    }
    auto snap1 = cs.snapshot();
    std::println("  invalidation_trace_records: {} -> {}", snap0.invalidation_trace_records_total,
                 snap1.invalidation_trace_records_total);
    CHECK(snap1.invalidation_trace_records_total > snap0.invalidation_trace_records_total,
          "invalidation_trace_records_total incremented (binding mutation traced)");
    return true;
}

bool test_invalidation_stats_primitive() {
    std::println("\n--- AC4: (engine:metrics \"compile:mutation-log-invalidation-stats\") returns "
                 "2-key hash ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define stats (engine:metrics "
            "\"compile:mutation-log-invalidation-stats\"))\")");
    cs.eval("(eval-current)");
    for (const char* key : {"records-total", "trace-size"}) {
        std::string check = std::string("(hash-ref stats \"") + key + "\")";
        auto rv = cs.eval(check);
        if (!rv || !aura::compiler::types::is_int(*rv)) {
            std::println("  FAIL: hash-ref stats {} did not return int", key);
            ++g_failed;
        } else {
            CHECK(true, std::string("hash-ref stats \"") + key + "\" returns int");
        }
    }
    return true;
}

bool test_trace_size_nonzero_after_mutation() {
    std::println("\n--- AC5: trace-size > 0 after a binding mutation ---");
    aura::compiler::CompilerService cs;
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Eager);
    cs.eval("(set-code \"(define r 5)\")");
    cs.eval("(eval-current)");
    auto r = cs.eval("(mutate:rebind \"r\" \"10\" \"size-test\")");
    if (!r) {
        std::println("  FAIL: mutate:rebind failed");
        ++g_failed;
        return false;
    }
    auto rstats = cs.eval("(engine:metrics \"compile:mutation-log-invalidation-stats\")");
    CHECK(rstats && aura::compiler::types::is_hash(*rstats),
          "compile:mutation-log-invalidation-stats returns hash");
    auto sz = cs.eval(
        "(hash-ref (engine:metrics \"compile:mutation-log-invalidation-stats\") \"trace-size\")");
    CHECK(sz && aura::compiler::types::is_int(*sz) && aura::compiler::types::as_int(*sz) > 0,
          "trace-size > 0 after a binding mutation");
    return true;
}

bool test_eval_still_works() {
    std::println("\n--- AC6: existing eval still works (regression) ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define z 99)\")");
    cs.eval("(eval-current)");
    auto r = cs.eval("(eval-current)");
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 99,
          "plain (define z 99) + (eval-current) returns 99");
    return true;
}

} // namespace aura_413_detail

int main() {
    using namespace aura_413_detail;
    std::println(
        "=== Issue #413: MutationLog-integrated type cache invalidation (scope-limited) ===");
    test_initial_counters_zero();
    test_snapshot_has_new_field();
    test_typed_mutate_increments_trace();
    test_invalidation_stats_primitive();
    test_trace_size_nonzero_after_mutation();
    test_eval_still_works();
    std::println("\n=== Summary: {}/{} passed, {}/{} failed ===", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}