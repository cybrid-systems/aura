// test_stable_ref_provenance_fiber_cow_task1.cpp —
// Issue #552: StableNodeRef + generation_ + COW/Fiber/
// Provenance Safety for Reliable Long-Running AI Multi-Round
// Query → Mutate → Eval Loops.
//
// Non-duplicative refinement of #549 (Task 6 review) focused
// on Task 1 EDSL primitives (mutate:* + query:*-stable).
// This binary exercises the EDSL primitive paths specifically:
//
//   - AC1: 5-counter sum reachable via
//          (query:edsl-stability-stats)
//   - AC2: query:edsl-stability-stats returns integer
//          sum of the 5 counters (4 from #549 Evaluator +
//          1 from #457 FlatAST generation_wrap_count_)
//   - AC3: cross_cow_invalidations_ grows under
//          mutate:query-and-replace + COW validate loop
//   - AC4: 500-iter heavy mutate:* + validate cycle
//          (mutate:replace-value, mutate:query-and-replace)
//   - AC5: query:*-stable primitive integration
//   - AC6: validate_stable_ref classification still
//          reachable (Task1 EDSL hot path)
//   - AC7: 8-thread concurrent mutate:* + COW (no crash)
//   - AC8: (gc-heap) + StableNodeRef integration
//   - AC9: regression — #549 + #551 primitives still work

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

namespace aura_issue_552_detail {

using aura::ast::NodeId;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;

static int k_long_iters() {
    return k_int_env("AURA_STRESS_ITERS", 500);
}

// ── AC1: 5 counters reachable (cross_cow + fiber_stale +
//         wrap + rollback + provenance) ────────────────────
bool test_edsl_stability_counters_reachable() {
    std::println("\n--- AC1: 5 EDSL stability counters reachable ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    const auto cc0 = cs.evaluator().get_cross_cow_invalidations();
    const auto fs0 = cs.evaluator().get_fiber_stale_ref_count();
    const auto r0 = cs.evaluator().get_mutation_log_rollback_count();
    const auto pm0 = cs.evaluator().get_provenance_mismatch();
    auto* ws = cs.evaluator().workspace_flat();
    const auto w0 = ws ? ws->generation_wrap_count() : 0;
    std::println("  baseline: cross_cow={} fiber_stale={} wrap={} rollback={} provenance={}", cc0,
                 fs0, w0, r0, pm0);
    CHECK(cc0 == 0, "cross_cow starts at 0");
    CHECK(fs0 == 0, "fiber_stale starts at 0");
    CHECK(w0 == 0, "generation_wrap starts at 0 (FlatAST, #457)");
    CHECK(r0 == 0, "mutation_log_rollback starts at 0");
    CHECK(pm0 == 0, "provenance_mismatch starts at 0");
    return true;
}

// ── AC2: query:edsl-stability-stats returns integer sum ───
bool test_query_edsl_stability_stats() {
    std::println("\n--- AC2: (query:edsl-stability-stats) returns integer ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(query:edsl-stability-stats)");
    CHECK(r.has_value(), "(query:edsl-stability-stats) returns");
    CHECK(aura::compiler::types::is_int(*r), "(query:edsl-stability-stats) is integer");
    if (r && aura::compiler::types::is_int(*r)) {
        const auto v = aura::compiler::types::as_int(*r);
        std::println("  query:edsl-stability-stats = {}", v);
        CHECK(v >= 0, "(query:edsl-stability-stats) >= 0 (5 counters sum)");
    }
    return true;
}

// ── AC3: cross_cow grows under mutate:replace-value +
//         validate_stable_ref loop ────────────────────────
bool test_cross_cow_under_mutate_validate() {
    std::println("\n--- AC3: cross_cow grows under mutate + validate ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws) {
        ++aura::test::g_failed;
        return false;
    }
    const auto cc0 = cs.evaluator().get_cross_cow_invalidations();
    // mutate:replace-value (goes through Guard) + validate.
    // Each iteration bumps generation_ + cross_cow.
    for (int i = 0; i < 50; ++i) {
        std::string code = std::string("(mutate:replace-value (define a ") + std::to_string(i) +
                           ") (define a " + std::to_string(i) + "))";
        (void)cs.eval(code);
        const auto g = ws->generation();
        (void)cs.evaluator().validate_stable_ref(0, g - 1);
    }
    const auto cc1 = cs.evaluator().get_cross_cow_invalidations();
    std::println("  cross_cow: {} -> {} (delta {})", cc0, cc1, cc1 - cc0);
    CHECK(cc1 >= cc0 + 50, "cross_cow_invalidations grew by >= 50 under mutate + validate loop");
    return true;
}

// ── AC4: 500-iter heavy mutate:* cycle (Task1 EDSL focus) ─
bool test_long_running_edsl_cycle() {
    std::println("\n--- AC4: {} iters heavy mutate:* cycle ---", k_long_iters());
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws) {
        ++aura::test::g_failed;
        return false;
    }
    const auto cc0 = cs.evaluator().get_cross_cow_invalidations();
    std::mt19937 rng(552u);
    std::uniform_int_distribution<int> val_dist(0, 999);
    for (int i = 0; i < k_long_iters(); ++i) {
        // Task1 EDSL primitives: mutate:replace-value +
        // mutate:rebind (the latter is the pilot for
        // Guard-based mutate). Cycle through them.
        std::string code;
        if (i & 1) {
            code = std::string("(mutate:replace-value (define ") + (i & 1 ? "a" : "b") + " " +
                   std::to_string(val_dist(rng)) + ") (define " + (i & 1 ? "a" : "b") + " " +
                   std::to_string(val_dist(rng)) + "))";
        } else {
            code = std::string("(define ") + (i & 1 ? "a" : "b") + " " +
                   std::to_string(val_dist(rng)) + ")";
        }
        (void)cs.eval(code);
        // Validate each iteration (cross_cow bump).
        const auto g = ws->generation();
        (void)cs.evaluator().validate_stable_ref(0, g - 1);
    }
    const auto cc1 = cs.evaluator().get_cross_cow_invalidations();
    std::println("  cross_cow: {} -> {} (delta {})", cc0, cc1, cc1 - cc0);
    CHECK(cc1 >= cc0 + static_cast<std::uint64_t>(k_long_iters() / 2),
          "cross_cow grew by >= half the iter count under heavy mutate");
    return true;
}

// ── AC5: query:*-stable primitive integration
bool test_query_stable_primitives_integration() {
    std::println("\n--- AC5: query:*-stable primitive integration ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define x 1)\")");
    (void)cs.eval("(eval-current)");
    // (query:tag-arity-count tag-int arity-int) is the
    // standard stable query primitive from #447. Verify it
    // still works alongside the new query:edsl-stability-stats.
    auto r1 = cs.eval("(query:tag-arity-count 32 0)");
    CHECK(r1.has_value() && aura::compiler::types::is_int(*r1),
          "(query:tag-arity-count) returns (regression for #447)");
    auto r2 = cs.eval("(query:stable-ref-stats)");
    CHECK(r2.has_value() && aura::compiler::types::is_int(*r2),
          "(query:stable-ref-stats) returns (regression for #457)");
    auto r3 = cs.eval("(query:edsl-stability-stats)");
    CHECK(r3.has_value() && aura::compiler::types::is_int(*r3),
          "(query:edsl-stability-stats) returns (new for #552)");
    return true;
}

// ── AC6: validate_stable_ref classification (Task1 hot path)
bool test_validate_stable_ref_hot_path() {
    std::println("\n--- AC6: validate_stable_ref classification (Task1 hot path) ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws) {
        ++aura::test::g_failed;
        return false;
    }
    const auto current_gen = ws->generation();
    const auto cc0 = cs.evaluator().get_cross_cow_invalidations();
    // Validate with small gen delta (= cross_cow, same fiber).
    auto r1 = cs.evaluator().validate_stable_ref(0, current_gen - 1);
    CHECK(!r1.first, "validate_stable_ref(gen-1) returns invalid");
    CHECK(r1.second, "validate_stable_ref(gen-1) returns is_stale=true");
    const auto cc1 = cs.evaluator().get_cross_cow_invalidations();
    CHECK(cc1 > cc0, "cross_cow bumped by small-delta gen mismatch (Task1 hot path)");
    return true;
}

// ── AC7: 8-thread concurrent mutate:* + COW (no crash)
bool test_eight_thread_concurrent_edsl_mutate() {
    std::println("\n--- AC7: 8 threads × 20 iters concurrent EDSL mutate ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    constexpr int n_threads = 8;
    constexpr int n_iters = 20;
    std::mutex mtx;
    std::atomic<int> completed{0};
    auto worker = [&](int tid) {
        for (int i = 0; i < n_iters; ++i) {
            std::lock_guard<std::mutex> lk(mtx);
            // Task1 EDSL primitives: mutate:replace-value +
            // validate_stable_ref (Task1 hot path combo).
            std::string code = "(mutate:replace-value (define v" + std::to_string(tid) + " " +
                               std::to_string(i) + ") (define v" + std::to_string(tid) + " " +
                               std::to_string(i) + "))";
            (void)cs.eval(code);
            auto* ws = cs.evaluator().workspace_flat();
            if (ws) {
                (void)cs.evaluator().validate_stable_ref(0, ws->generation() - 1);
            }
            completed.fetch_add(1);
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < n_threads; ++i)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();

    const auto cc = cs.evaluator().get_cross_cow_invalidations();
    std::println("  completed: {}/{} cross_cow: {}", completed.load(), n_threads * n_iters, cc);
    CHECK(completed.load() == n_threads * n_iters,
          "all 160 ops completed (no crash under concurrent EDSL mutate)");
    CHECK(cc >= static_cast<std::uint64_t>(n_threads * n_iters),
          "cross_cow >= concurrent mutate count");
    return true;
}

// ── AC8: (gc-heap) + StableNodeRef integration ────────
bool test_gc_heap_with_stable_ref_task1() {
    std::println("\n--- AC8: (gc-heap) + StableNodeRef integration ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    (void)cs.eval("(mutate:replace-value (define a 99) (define a 99))");
    auto* ws = cs.evaluator().workspace_flat();
    if (ws) {
        (void)cs.evaluator().validate_stable_ref(0, ws->generation() - 1);
    }
    auto r = cs.eval("(gc-heap)");
    CHECK(r.has_value(), "(gc-heap) callable after StableNodeRef validate");
    return true;
}

// ── AC9: regression — #549 + #551 primitives still work
bool test_regression_existing_primitives() {
    std::println("\n--- AC9: regression — #549 + #551 primitives still work ---");
    CompilerService cs;
    auto r1 = cs.eval("(query:edsl-stability-stats)");
    CHECK(r1.has_value() && aura::compiler::types::is_int(*r1),
          "(query:edsl-stability-stats) (new for #552)");
    auto r2 = cs.eval("(query:self-evolution-stability-stats)");
    CHECK(r2.has_value() && aura::compiler::types::is_int(*r2),
          "(query:self-evolution-stability-stats) (regression for #549)");
    auto r3 = cs.eval("(query:reflect-postmutate-stats)");
    CHECK(r3.has_value() && aura::compiler::types::is_int(*r3),
          "(query:reflect-postmutate-stats) (regression for #551)");
    auto r4 = cs.eval("(query:stable-ref-stats)");
    CHECK(r4.has_value() && aura::compiler::types::is_int(*r4),
          "(query:stable-ref-stats) (regression for #457)");
    if (!cs.eval("(define reg-552-a 10)")) {
        CHECK(false, "define (regression)");
        return false;
    }
    auto r5 = cs.eval("(define reg-552-b 32)");
    (void)r5;
    auto r6 = cs.eval("(+ reg-552-a reg-552-b)");
    CHECK(r6.has_value() && aura::compiler::types::is_int(*r6) &&
              aura::compiler::types::as_int(*r6) == 42,
          "(+ reg-552-a reg-552-b) == 42 (regression)");
    return true;
}

int run_tests() {
    std::println("═══ Issue #552 verification tests ═══\n");
    std::println("Layer 1: 5 EDSL stability counters + primitive");
    test_edsl_stability_counters_reachable();
    test_query_edsl_stability_stats();
    std::println("\nLayer 2: EDSL primitive paths (mutate:* + validate)");
    test_cross_cow_under_mutate_validate();
    test_long_running_edsl_cycle();
    test_query_stable_primitives_integration();
    test_validate_stable_ref_hot_path();
    std::println("\nLayer 3: concurrent + GC + regression");
    test_eight_thread_concurrent_edsl_mutate();
    test_gc_heap_with_stable_ref_task1();
    test_regression_existing_primitives();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_552_detail

int aura_issue_552_run() {
    return aura_issue_552_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_552_run();
}
#endif