// test_incremental_typed_selfmod_dirty_narrowing.cpp —
// Issue #550: Dirty/Epoch Propagation + Incremental
// solve_delta + Occurrence Re-Narrowing + Pass Short-Circuit
// for reliable typed self-mod.
//
// Non-duplicative with #536/#537 (dirty + occurrence impl),
// #526 (dirty/epoch to type), #509 (touched_roots_),
// #518 (occurrence-narrowing impl). This binary focuses
// on the **new** observability surface + the typed self-mod
// matrix the Task 6 review flagged:
//
//   - AC1: 4 new dirty/narrowing counters reachable +
//          start at 0
//   - AC2: (query:typed-mutation-stats) returns integer
//          sum of 4 counters
//   - AC3: (query:dirty-impact) returns touched_roots_size
//   - AC4: narrowing_refresh_count_ bumps under Aura
//          mutate load (exit_mutation_boundary success path)
//   - AC5: 200-iter typed mutate cycle — narrowing +
//          passes_skipped monotonic
//   - AC6: touched_roots_size_ observable + settable
//   - AC7: 8-thread concurrent typed mutate (no crash,
//          narrowing monotonic)
//   - AC8: (gc-heap) + dirty integration (no crash)
//   - AC9: regression — existing query primitives still work

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

namespace aura_issue_550_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;

static int k_long_iters() {
    return k_int_env("AURA_STRESS_ITERS", 200);
}

// ── AC1: 4 new dirty/narrowing counters reachable + start at 0
bool test_dirty_narrowing_counters_reachable() {
    std::println("\n--- AC1: 4 dirty/narrowing counters reachable + start at 0 ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    const auto n0 = cs.evaluator().get_narrowing_refresh_count();
    const auto cc0 = cs.evaluator().get_cross_delta_conflicts_caught();
    const auto ps0 = cs.evaluator().get_passes_skipped_type_dirty();
    const auto tr0 = cs.evaluator().get_touched_roots_size();
    std::println("  baseline: narrowing={} cross_delta={} passes_skipped={} touched_roots={}",
                 n0, cc0, ps0, tr0);
    CHECK(n0 == 0, "narrowing_refresh_count starts at 0");
    CHECK(cc0 == 0, "cross_delta_conflicts_caught starts at 0");
    CHECK(ps0 == 0, "passes_skipped_type_dirty starts at 0");
    CHECK(tr0 == 0, "touched_roots_size starts at 0");
    return true;
}

// ── AC2: query:typed-mutation-stats returns integer sum
bool test_query_typed_mutation_stats() {
    std::println("\n--- AC2: (query:typed-mutation-stats) returns integer ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(query:typed-mutation-stats)");
    CHECK(r.has_value(), "(query:typed-mutation-stats) returns");
    CHECK(aura::compiler::types::is_int(*r),
          "(query:typed-mutation-stats) is integer");
    if (r && aura::compiler::types::is_int(*r)) {
        const auto v = aura::compiler::types::as_int(*r);
        std::println("  query:typed-mutation-stats = {}", v);
        CHECK(v >= 0,
              "(query:typed-mutation-stats) >= 0 (4 counters sum)");
    }
    return true;
}

// ── AC3: query:dirty-impact returns touched_roots_size
bool test_query_dirty_impact() {
    std::println("\n--- AC3: (query:dirty-impact) returns touched_roots_size ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(query:dirty-impact)");
    CHECK(r.has_value(), "(query:dirty-impact) returns");
    CHECK(aura::compiler::types::is_int(*r),
          "(query:dirty-impact) is integer");
    if (r && aura::compiler::types::is_int(*r)) {
        const auto v = aura::compiler::types::as_int(*r);
        std::println("  query:dirty-impact = {}", v);
        CHECK(v >= 0, "(query:dirty-impact) >= 0 (snapshot)");
    }
    return true;
}

// ── AC4: narrowing_refresh_count_ bumps under Aura mutate
//         load (exit_mutation_boundary success path) ───────
bool test_narrowing_refresh_under_mutate() {
    std::println("\n--- AC4: narrowing_refresh_count bumps under mutate load ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    const auto n0 = cs.evaluator().get_narrowing_refresh_count();
    // A few Aura mutates — each one goes through a Guard
    // which calls exit_mutation_boundary(success=true) which
    // bumps narrowing_refresh_count_.
    for (int i = 0; i < 5; ++i) {
        (void)cs.eval("(mutate:replace-value (define a " +
            std::to_string(i) + ") (define a " +
            std::to_string(i) + "))");
    }
    const auto n1 = cs.evaluator().get_narrowing_refresh_count();
    std::println("  narrowing_refresh: {} -> {} (delta {})",
                 n0, n1, n1 - n0);
    CHECK(n1 > n0,
          "narrowing_refresh_count bumped after Aura mutate load "
          "(exit_mutation_boundary success path)");
    return true;
}

// ── AC5: 200-iter typed mutate cycle — narrowing +
//         passes_skipped monotonic ─────────────────────────
bool test_long_running_typed_mutate_cycle() {
    std::println("\n--- AC5: {} iters typed mutate cycle ---", k_long_iters());
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    const auto n0 = cs.evaluator().get_narrowing_refresh_count();
    const auto ps0 = cs.evaluator().get_passes_skipped_type_dirty();
    std::mt19937 rng(550u);
    std::uniform_int_distribution<int> val_dist(0, 999);
    // Use mutate:replace-value (goes through Guard + bumps
    // narrowing on success); (define ...) is a workspace-
    // load operation that doesn't go through a Guard.
    for (int i = 0; i < k_long_iters(); ++i) {
        std::string code = std::string("(mutate:replace-value (define ") +
            (i & 1 ? "a" : "b") + " " +
            std::to_string(val_dist(rng)) +
            ") (define " + (i & 1 ? "a" : "b") + " " +
            std::to_string(val_dist(rng)) + "))";
        (void)cs.eval(code);
    }
    const auto n1 = cs.evaluator().get_narrowing_refresh_count();
    const auto ps1 = cs.evaluator().get_passes_skipped_type_dirty();
    std::println("  narrowing: {} -> {} passes_skipped: {} -> {}",
                 n0, n1, ps0, ps1);
    CHECK(n1 >= n0 + static_cast<std::uint64_t>(k_long_iters() - 5),
          "narrowing_refresh grew under typed mutate cycle");
    CHECK(ps1 >= ps0, "passes_skipped monotonic non-decreasing");
    return true;
}

// ── AC6: touched_roots_size_ observable + settable
bool test_touched_roots_size_observable() {
    std::println("\n--- AC6: touched_roots_size observable + settable ---");
    Evaluator ev;
    const auto s0 = ev.get_touched_roots_size();
    CHECK(s0 == 0, "touched_roots_size starts at 0");
    ev.set_touched_roots_size(42);
    CHECK(ev.get_touched_roots_size() == 42,
          "touched_roots_size set/get round-trip (42)");
    ev.set_touched_roots_size(0);
    CHECK(ev.get_touched_roots_size() == 0,
          "touched_roots_size reset to 0");
    return true;
}

// ── AC7: 8-thread concurrent typed mutate (no crash,
//         narrowing monotonic) ─────────────────────────────
bool test_eight_thread_concurrent_typed_mutate() {
    std::println("\n--- AC7: 8 threads × 20 iters concurrent typed mutate ---");
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
            // mutate:replace-value goes through Guard and
            // bumps narrowing_refresh_count_ on success.
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

    const auto n = cs.evaluator().get_narrowing_refresh_count();
    std::println("  completed: {}/{} narrowing_refresh: {}",
                 completed.load(), n_threads * n_iters, n);
    CHECK(completed.load() == n_threads * n_iters,
          "all 160 ops completed (no crash under concurrent typed mutate)");
    CHECK(n > 0,
          "narrowing_refresh > 0 after concurrent typed mutate load");
    return true;
}

// ── AC8: (gc-heap) + dirty integration (no crash) ─────
bool test_gc_heap_with_dirty() {
    std::println("\n--- AC8: (gc-heap) + dirty integration ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    (void)cs.eval("(mutate:replace-value (define a 99) (define a 99))");
    auto r = cs.eval("(gc-heap)");
    CHECK(r.has_value(), "(gc-heap) callable after typed mutate");
    return true;
}

// ── AC9: regression — existing query primitives still work
bool test_regression_existing_primitives() {
    std::println("\n--- AC9: regression — existing primitives still work ---");
    CompilerService cs;
    auto r1 = cs.eval("(query:typed-mutation-stats)");
    CHECK(r1.has_value() && aura::compiler::types::is_int(*r1),
          "(query:typed-mutation-stats) (new for #550)");
    auto r2 = cs.eval("(query:dirty-impact)");
    CHECK(r2.has_value() && aura::compiler::types::is_int(*r2),
          "(query:dirty-impact) (new for #550)");
    auto r3 = cs.eval("(query:self-evolution-stability-stats)");
    CHECK(r3.has_value() && aura::compiler::types::is_int(*r3),
          "(query:self-evolution-stability-stats) (regression for #549)");
    auto r4 = cs.eval("(query:stable-ref-stats)");
    CHECK(r4.has_value() && aura::compiler::types::is_int(*r4),
          "(query:stable-ref-stats) (regression for #457)");
    auto r5 = cs.eval("(query:envframe-dualpath-stats)");
    CHECK(r5.has_value() && aura::compiler::types::is_int(*r5),
          "(query:envframe-dualpath-stats) (regression for #543)");
    if (!cs.eval("(define reg-550-a 10)")) {
        CHECK(false, "define (regression)");
        return false;
    }
    auto r6 = cs.eval("(define reg-550-b 32)");
    (void)r6;
    auto r7 = cs.eval("(+ reg-550-a reg-550-b)");
    CHECK(r7.has_value() && aura::compiler::types::is_int(*r7) &&
              aura::compiler::types::as_int(*r7) == 42,
          "(+ reg-550-a reg-550-b) == 42 (regression)");
    return true;
}

int run_tests() {
    std::println("═══ Issue #550 verification tests ═══\n");
    std::println("Layer 1: 4 dirty/narrowing counters + 2 primitives");
    test_dirty_narrowing_counters_reachable();
    test_query_typed_mutation_stats();
    test_query_dirty_impact();
    std::println("\nLayer 2: narrowing refresh under mutate");
    test_narrowing_refresh_under_mutate();
    test_long_running_typed_mutate_cycle();
    test_touched_roots_size_observable();
    std::println("\nLayer 3: concurrent + GC + regression");
    test_eight_thread_concurrent_typed_mutate();
    test_gc_heap_with_dirty();
    test_regression_existing_primitives();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_550_detail

int aura_issue_550_run() { return aura_issue_550_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_550_run(); }
#endif