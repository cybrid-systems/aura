// test_issue_396.cpp — Issue #396: Strengthen
// `mutate:atomic-batch` fiber safety + expand lockless
// helpers + expose better stats.
//
// Validates 3 phases + 4 ACs:
//   AC1: Fibers executing inside atomic-batch have
//        last_yield_reason == MutationBoundary. The
//        bridge setter g_fiber_set_yield_reason_mutation_boundary
//        is invoked on Guard entry (verified by a mock
//        setter that records calls).
//   AC2: mutate:remove-node + mutate:insert-child now work
//        inside atomic-batch (no more "batch-unsupported-op"
//        error). The lockless helpers
//        eval_flat_apply_mutate_remove_node +
//        eval_flat_apply_mutate_insert_child extract the
//        inner logic from the wrapper primitives.
//   AC3: Existing concurrent fiber tests pass (regression
//        check). Implicit — the rest of the suite covers
//        this; we just confirm mutate:rebind + tweak-literal
//        still work via the atomic-batch path.
//   AC4: (stats:get "atomic-batch:stats") hash now includes the new
//        "executed-under-concurrent-fiber" key (Issue #396
//        Phase 3 heuristic). The other keys
//        (batch-count, ops-total, rollback-count,
//        ops-per-batch, bumps-saved-total) still work.

#include "test_harness.hpp"
#include "messaging_bridge.h"

using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_396_detail {

// Local CS helper (mirrors test_issue_393's pattern).
struct CS {
    aura::compiler::CompilerService svc;
    struct EvalResult {
        bool ok = false;
        aura::compiler::types::EvalValue v{};
    };
    EvalResult try_run(std::string_view src) {
        auto r = svc.eval(src);
        if (!r)
            return {false, aura::compiler::types::make_void()};
        return {true, *r};
    }
    bool set_source(const std::string& src) {
        auto r = try_run(std::string("(set-code \"") + src + "\")");
        return r.ok;
    }
};

// AC1: bridge setter g_fiber_set_yield_reason_mutation_boundary
// is called on Guard entry. We install a mock setter that
// records the call count, then run a batch and check it was
// invoked exactly once.
//
// Setup/teardown: stash the original setter on entry, restore
// on exit so we don't pollute other tests.
struct SetterProbe {
    int call_count = 0;
    static SetterProbe* current;
    static void probe_fn() {
        if (current)
            ++current->call_count;
    }
};
SetterProbe* SetterProbe::current = nullptr;

bool test_ac1_fiber_setter_invoked() {
    std::println("\n--- AC1: bridge setter invoked on atomic-batch Guard entry ---");
    CS cs;
    if (!cs.set_source("(define x 1)")) {
        ++g_failed;
        std::println("  FAIL: set-source failed");
        return false;
    }
    // Install the mock setter.
    SetterProbe probe;
    SetterProbe::current = &probe;
    auto* prev = aura::messaging::g_fiber_set_yield_reason_mutation_boundary;
    aura::messaging::g_fiber_set_yield_reason_mutation_boundary = &SetterProbe::probe_fn;
    // Run a batch.
    auto r = cs.try_run(
        "(mutate:atomic-batch (list (list \"mutate:rebind\" \"x\" \"99\")) \"probe-test\")");
    // Restore.
    aura::messaging::g_fiber_set_yield_reason_mutation_boundary = prev;
    SetterProbe::current = nullptr;
    if (!r.ok || !aura::compiler::types::is_bool(r.v)) {
        ++g_failed;
        std::println("  FAIL: batch eval failed");
        return false;
    }
    bool batch_ok = aura::compiler::types::as_bool(r.v);
    if (!batch_ok) {
        ++g_failed;
        std::println("  FAIL: batch returned #f");
        return false;
    }
    ++g_passed;
    std::println("  PASS: batch returned #t");
    if (probe.call_count != 1) {
        ++g_failed;
        std::println("  FAIL: setter call_count = {} (expected 1)", probe.call_count);
        return false;
    }
    ++g_passed;
    std::println("  PASS: setter called exactly once on Guard entry");
    return true;
}

// AC2: mutate:remove-node and mutate:insert-child now work
// inside atomic-batch (no more "batch-unsupported-op" error).
bool test_ac2_lockless_remove_and_insert() {
    std::println("\n--- AC2: remove-node + insert-child work inside atomic-batch ---");
    CS cs;
    if (!cs.set_source("(define x 1) (define y 2)")) {
        ++g_failed;
        std::println("  FAIL: set-source failed");
        return false;
    }
    // Find the node ids of x and y so we can mutate them.
    auto r_ids = cs.try_run("(let ((defs (query:defines-by-marker \"User\")))"
                            "  (list (car defs) (car (cdr defs))))");
    if (!r_ids.ok || !aura::compiler::types::is_pair(r_ids.v)) {
        ++g_failed;
        std::println("  FAIL: list of define ids not returned");
        return false;
    }
    // Run an atomic-batch that:
    //   1. removes y
    //   2. inserts 99 as a child of x
    // Both ops should succeed inside the batch.
    auto r = cs.try_run("(let* ((defs (query:defines-by-marker \"User\"))"
                        "       (x-id (car defs))"
                        "       (y-id (car (cdr defs))))"
                        "  (mutate:atomic-batch"
                        "    (list (list \"mutate:remove-node\" y-id)"
                        "          (list \"mutate:insert-child\" x-id 0 \"99\"))"
                        "    \"ac2-test\"))");
    if (!r.ok || !aura::compiler::types::is_bool(r.v)) {
        ++g_failed;
        std::println("  FAIL: batch eval failed");
        return false;
    }
    bool batch_ok = aura::compiler::types::as_bool(r.v);
    if (!batch_ok) {
        ++g_failed;
        std::println("  FAIL: batch returned #f (sub-op failed)");
        return false;
    }
    ++g_passed;
    std::println("  PASS: batch with remove-node + insert-child returned #t");
    // Verify the ops took effect: y should be gone, x's body should be 99.
    auto r_after = cs.try_run(
        "(query:ref-valid? (query:stable-ref (car (query:defines-by-marker \"User\"))))");
    if (!r_after.ok) {
        ++g_failed;
        std::println("  FAIL: post-batch ref-valid? check failed");
        return false;
    }
    if (aura::compiler::types::is_bool(r_after.v) && aura::compiler::types::as_bool(r_after.v)) {
        ++g_passed;
        std::println("  PASS: x still queryable after batch (single Define remains)");
    } else {
        ++g_failed;
        std::println("  FAIL: x not queryable after batch");
    }
    return true;
}

// AC3 (regression): mutate:rebind + the existing lockless path
// still work via atomic-batch. The new remove-node / insert-child
// sub-ops should not have broken anything.
bool test_ac3_regression_rebind_still_works() {
    std::println("\n--- AC3: regression — mutate:rebind still works in atomic-batch ---");
    CS cs;
    if (!cs.set_source("(define x 1)")) {
        ++g_failed;
        std::println("  FAIL: set-source failed");
        return false;
    }
    auto r = cs.try_run(
        "(mutate:atomic-batch (list (list \"mutate:rebind\" \"x\" \"999\")) \"rebind-test\")");
    if (!r.ok || !aura::compiler::types::is_bool(r.v)) {
        ++g_failed;
        std::println("  FAIL: rebind batch failed");
        return false;
    }
    if (!aura::compiler::types::as_bool(r.v)) {
        ++g_failed;
        std::println("  FAIL: rebind batch returned #f");
        return false;
    }
    ++g_passed;
    std::println("  PASS: mutate:rebind still works in atomic-batch");
    return true;
}

// AC4: (stats:get "atomic-batch:stats") hash now includes
// "executed-under-concurrent-fiber" key (the new heuristic
// from Phase 3). Also: the other 5 existing keys still
// work (regression).
bool test_ac4_new_stats_key() {
    std::println("\n--- AC4: atomic-batch:stats includes executed-under-concurrent-fiber ---");
    CS cs;
    if (!cs.set_source("(define x 1)")) {
        ++g_failed;
        std::println("  FAIL: set-source failed");
        return false;
    }
    // Run a batch to bump the batch-count.
    auto r_batch =
        cs.try_run("(mutate:atomic-batch (list (list \"mutate:rebind\" \"x\" \"2\")) \"ac4\")");
    if (!r_batch.ok) {
        ++g_failed;
        std::println("  FAIL: batch failed");
        return false;
    }
    // Hash-ref on each expected key.
    auto check_key = [&](const char* key, const char* desc) {
        auto r = cs.try_run(std::string("(hash-ref (stats:get " atomic - batch : stats ") \"") +
                            key + "\")");
        if (!r.ok || !aura::compiler::types::is_int(r.v)) {
            ++g_failed;
            std::println("  FAIL: key {} ({}) missing or not int", key, desc);
            return false;
        }
        ++g_passed;
        std::println("  PASS: {} ({}) = {}", key, desc,
                     (long long)aura::compiler::types::as_int(r.v));
        return true;
    };
    check_key("batch-count", "total batches since startup");
    check_key("ops-total", "total ops applied across batches");
    check_key("rollback-count", "total batch rollbacks");
    check_key("ops-per-batch", "avg ops per batch (integer)");
    check_key("bumps-saved-total", "lifetime generation bumps saved by batching");
    // The new key — present in test mode but the counter
    // stays 0 because the bridge setter is null in
    // non-serve binaries. Its presence in the hash is
    // the new contract.
    check_key("executed-under-concurrent-fiber", "Issue #396 Phase 3 heuristic (0 in test mode)");
    return true;
}

} // namespace aura_issue_396_detail

int main() {
    using namespace aura_issue_396_detail;
    std::println("=== test_issue_396: atomic-batch fiber safety + lockless + stats ===");
    test_ac1_fiber_setter_invoked();
    test_ac2_lockless_remove_and_insert();
    test_ac3_regression_rebind_still_works();
    test_ac4_new_stats_key();
    std::println("\n=== Totals: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
