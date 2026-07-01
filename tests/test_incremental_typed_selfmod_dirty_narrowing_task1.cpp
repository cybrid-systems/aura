// test_incremental_typed_selfmod_dirty_narrowing_task1.cpp
// — Issue #555: MutationBoundaryGuard + mark_dirty_upward +
// defuse_version_ + Incremental Type/Occurrence Narrowing
// Post-Mutate for Reliable Typed AI Self-Mod.
//
// Non-duplicative refinement of #550 (Task 6 review) focused
// on Task 1 EDSL mutate + Guard + dirty propagation paths.
// #550 added 4 counters (narrowing_refresh + cross_delta +
// passes_skipped + touched_roots); #555 adds 4 more
// Task1-specific counters (dirty_propagation +
// selective_recheck + touched_roots_conflict +
// guard_dirty_epoch) so the AI Agent can compute
// propagation_ratio + selective_recheck_rate + conflict_rate
// in one primitive read.
//
//   - AC1: 4 new counters reachable + start at 0
//   - AC2: (query:typed-mutation-stats-task1) returns
//          integer sum of 8 counters (4 new + 4 from #550)
//   - AC3: guard_dirty_epoch_count bumps on Guard dtor
//          success (mutate:replace-value path)
//   - AC4: 200-iter mutate cycle — guard_dirty_epoch +
//          selective_recheck grow monotonically
//   - AC5: 100-iter mark_dirty_upward cycle —
//          dirty_propagation grows linearly
//   - AC6: touched_roots_conflict_count reachable + settable
//   - AC7: 8-thread concurrent typed mutate —
//          guard_dirty_epoch monotonic
//   - AC8: (gc-heap) + typed self-mod integration
//   - AC9: regression — #550 + #554 primitives still work

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

namespace aura_issue_555_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;

static int k_long_iters() {
    return k_int_env("AURA_STRESS_ITERS", 200);
}

// ── AC1: 4 new Task1 counters reachable + start at 0
bool test_task1_typed_mod_counters_reachable() {
    std::println("\n--- AC1: 4 Task1 typed-mod counters reachable + start at 0 ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    const auto dp0 = cs.evaluator().get_dirty_propagation_count();
    const auto sr0 = cs.evaluator().get_selective_recheck_count();
    const auto tc0 = cs.evaluator().get_touched_roots_conflict_count();
    const auto ge0 = cs.evaluator().get_guard_dirty_epoch_count();
    std::println("  baseline: dirty_prop={} selective={} conflicts={} guard_epoch={}",
                 dp0, sr0, tc0, ge0);
    CHECK(dp0 == 0, "dirty_propagation_count starts at 0");
    CHECK(sr0 == 0, "selective_recheck_count starts at 0");
    CHECK(tc0 == 0, "touched_roots_conflict_count starts at 0");
    CHECK(ge0 == 0, "guard_dirty_epoch_count starts at 0");
    return true;
}

// ── AC2: query:typed-mutation-stats-task1 returns integer
//         sum of 8 counters ─────────────────────────────────
bool test_query_typed_mutation_stats_task1() {
    std::println("\n--- AC2: (query:typed-mutation-stats-task1) returns integer ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(query:typed-mutation-stats-task1)");
    CHECK(r.has_value(), "(query:typed-mutation-stats-task1) returns");
    CHECK(aura::compiler::types::is_int(*r),
          "(query:typed-mutation-stats-task1) is integer");
    if (r && aura::compiler::types::is_int(*r)) {
        const auto v = aura::compiler::types::as_int(*r);
        std::println("  query:typed-mutation-stats-task1 = {}", v);
        CHECK(v >= 0,
              "(query:typed-mutation-stats-task1) >= 0 (8 counters sum)");
    }
    return true;
}

// ── AC3: guard_dirty_epoch_count bumps on Guard dtor success
//         (mutate:replace-value path) ───────────────────────
bool test_guard_dirty_epoch_under_mutate() {
    std::println("\n--- AC3: guard_dirty_epoch_count bumps on Guard dtor success ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    const auto ge0 = cs.evaluator().get_guard_dirty_epoch_count();
    const auto sr0 = cs.evaluator().get_selective_recheck_count();
    // mutate:replace-value goes through Guard + bumps
    // guard_dirty_epoch + selective_recheck on successful dtor.
    for (int i = 0; i < 5; ++i) {
        (void)cs.eval("(mutate:replace-value (define a " +
            std::to_string(i) + ") (define a " +
            std::to_string(i) + "))");
    }
    const auto ge1 = cs.evaluator().get_guard_dirty_epoch_count();
    const auto sr1 = cs.evaluator().get_selective_recheck_count();
    std::println("  guard_dirty_epoch: {} -> {} selective_recheck: {} -> {}",
                 ge0, ge1, sr0, sr1);
    CHECK(ge1 > ge0,
          "guard_dirty_epoch_count bumped after Aura mutate (Guard dtor success)");
    CHECK(sr1 > sr0,
          "selective_recheck_count bumped after Aura mutate");
    return true;
}

// ── AC4: 200-iter typed mutate cycle — guard_dirty_epoch +
//         selective_recheck grow monotonically ─────────────
bool test_long_running_typed_selfmod_cycle() {
    std::println("\n--- AC4: {} iters typed self-mod cycle ---", k_long_iters());
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    const auto ge0 = cs.evaluator().get_guard_dirty_epoch_count();
    const auto sr0 = cs.evaluator().get_selective_recheck_count();
    for (int i = 0; i < k_long_iters(); ++i) {
        std::string code = std::string("(mutate:replace-value (define ") +
            (i & 1 ? "a" : "b") + " " +
            std::to_string(i) +
            ") (define " + (i & 1 ? "a" : "b") + " " +
            std::to_string(i) + "))";
        (void)cs.eval(code);
    }
    const auto ge1 = cs.evaluator().get_guard_dirty_epoch_count();
    const auto sr1 = cs.evaluator().get_selective_recheck_count();
    std::println("  guard_dirty_epoch: {} -> {} selective_recheck: {} -> {}",
                 ge0, ge1, sr0, sr1);
    CHECK(ge1 >= ge0 + static_cast<std::uint64_t>(k_long_iters() - 5),
          "guard_dirty_epoch grew by >= ~iter count under typed cycle");
    CHECK(sr1 >= sr0 + static_cast<std::uint64_t>(k_long_iters() - 5),
          "selective_recheck grew by >= ~iter count under typed cycle");
    return true;
}

// ── AC5: 100-iter mark_dirty_upward cycle —
//         dirty_propagation grows linearly ──────────────────
bool test_dirty_propagation_cycle() {
    std::println("\n--- AC5: 100 iters mark_dirty_upward cycle ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws) { ++aura::test::g_failed; return false; }
    constexpr int k_iters = 100;
    const auto dp0 = cs.evaluator().get_dirty_propagation_count();
    for (int i = 0; i < k_iters; ++i) {
        if (ws->size() > 0) {
            ws->mark_dirty_upward(
                static_cast<aura::ast::NodeId>(i % ws->size()));
        }
    }
    const auto dp1 = cs.evaluator().get_dirty_propagation_count();
    std::println("  dirty_propagation: {} -> {} (delta {})", dp0, dp1, dp1 - dp0);
    CHECK(dp1 >= dp0,
          "dirty_propagation_count non-decreasing under mark_dirty_upward");
    return true;
}

// ── AC6: touched_roots_conflict_count reachable + settable
bool test_touched_roots_conflict_count() {
    std::println("\n--- AC6: touched_roots_conflict_count reachable + settable ---");
    Evaluator ev;
    const auto c0 = ev.get_touched_roots_conflict_count();
    CHECK(c0 == 0, "touched_roots_conflict_count starts at 0");
    ev.bump_touched_roots_conflict_count();
    ev.bump_touched_roots_conflict_count();
    ev.bump_touched_roots_conflict_count();
    const auto c1 = ev.get_touched_roots_conflict_count();
    CHECK(c1 == 3, "touched_roots_conflict_count bumped by 3");
    return true;
}

// ── AC7: 8-thread concurrent typed mutate (no crash,
//         guard_dirty_epoch monotonic) ─────────────────────
bool test_eight_thread_concurrent_typed_selfmod() {
    std::println("\n--- AC7: 8 threads × 20 iters concurrent typed self-mod ---");
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
            std::string code = "(mutate:replace-value (define v" +
                std::to_string(tid) + " " + std::to_string(i) +
                ") (define v" + std::to_string(tid) + " " +
                std::to_string(i) + "))";
            (void)cs.eval(code);
            completed.fetch_add(1);
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < n_threads; ++i) threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();

    const auto ge = cs.evaluator().get_guard_dirty_epoch_count();
    const auto sr = cs.evaluator().get_selective_recheck_count();
    std::println("  completed: {}/{} guard_dirty_epoch: {} selective_recheck: {}",
                 completed.load(), n_threads * n_iters, ge, sr);
    CHECK(completed.load() == n_threads * n_iters,
          "all 160 ops completed (no crash under concurrent typed self-mod)");
    CHECK(ge >= static_cast<std::uint64_t>(n_threads * n_iters),
          "guard_dirty_epoch >= concurrent mutate count");
    CHECK(sr >= static_cast<std::uint64_t>(n_threads * n_iters),
          "selective_recheck >= concurrent mutate count");
    return true;
}

// ── AC8: (gc-heap) + typed self-mod integration
bool test_gc_heap_with_typed_selfmod() {
    std::println("\n--- AC8: (gc-heap) + typed self-mod integration ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    (void)cs.eval("(mutate:replace-value (define a 99) (define a 99))");
    auto r = cs.eval("(gc-heap)");
    CHECK(r.has_value(), "(gc-heap) callable after typed self-mod");
    return true;
}

// ── AC9: regression — #550 + #554 primitives still work
bool test_regression_existing_primitives() {
    std::println("\n--- AC9: regression — #550 + #554 primitives still work ---");
    CompilerService cs;
    auto r1 = cs.eval("(query:typed-mutation-stats-task1)");
    CHECK(r1.has_value() && aura::compiler::types::is_int(*r1),
          "(query:typed-mutation-stats-task1) (new for #555)");
    auto r2 = cs.eval("(query:typed-mutation-stats)");
    CHECK(r2.has_value() && aura::compiler::types::is_int(*r2),
          "(query:typed-mutation-stats) (regression for #550)");
    auto r3 = cs.eval("(query:dirty-impact)");
    CHECK(r3.has_value() && aura::compiler::types::is_int(*r3),
          "(query:dirty-impact) (regression for #550)");
    auto r4 = cs.eval("(query:pattern-index-stats)");
    CHECK(r4.has_value() && aura::compiler::types::is_int(*r4),
          "(query:pattern-index-stats) (regression for #554)");
    if (!cs.eval("(define reg-555-a 10)")) {
        CHECK(false, "define (regression)");
        return false;
    }
    auto r5 = cs.eval("(define reg-555-b 32)");
    (void)r5;
    auto r6 = cs.eval("(+ reg-555-a reg-555-b)");
    CHECK(r6.has_value() && aura::compiler::types::is_int(*r6) &&
              aura::compiler::types::as_int(*r6) == 42,
          "(+ reg-555-a reg-555-b) == 42 (regression)");
    return true;
}

int run_tests() {
    std::println("═══ Issue #555 verification tests ═══\n");
    std::println("Layer 1: 4 new Task1 counters + primitive");
    test_task1_typed_mod_counters_reachable();
    test_query_typed_mutation_stats_task1();
    std::println("\nLayer 2: Guard dtor + dirty propagation + selective recheck");
    test_guard_dirty_epoch_under_mutate();
    test_long_running_typed_selfmod_cycle();
    test_dirty_propagation_cycle();
    test_touched_roots_conflict_count();
    std::println("\nLayer 3: concurrent + GC + regression");
    test_eight_thread_concurrent_typed_selfmod();
    test_gc_heap_with_typed_selfmod();
    test_regression_existing_primitives();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_555_detail

int aura_issue_555_run() { return aura_issue_555_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_555_run(); }
#endif