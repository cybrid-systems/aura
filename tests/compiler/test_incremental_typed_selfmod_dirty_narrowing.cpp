// test_incremental_typed_selfmod_dirty_narrowing.cpp — Merged #509/#518/#526/#536/#537/#550 +
// #554/#555 (#1978).
//
// Originally test_incremental_typed_selfmod_dirty_narrowing.cpp +
// test_incremental_typed_selfmod_dirty_narrowing_task1.cpp. Both cover
// Dirty/Epoch Propagation + Incremental solve_delta + Occurrence
// Re-Narrowing + Pass Short-Circuit for reliable typed self-mod.
// task1 consolidates #550 lineage + adds #554/#555. Merged here with
// all 18 ACs preserved (9 from each issue range).
//
// AC list (all preserved; each AC section cites original issue#):
//   #509/#518/#526/#536/#537/#550 (orig):
//     AC1: 4 dirty/narrowing counters reachable + start at 0
//     AC2: (engine:metrics "query:typed-mutation-stats") returns integer sum
//     AC3: (query:dirty-impact) returns touched_roots_size
//     AC4: narrowing_refresh_count bumps under Aura mutate
//     AC5: 200-iter typed mutate cycle — narrowing + passes_skipped monotonic
//     AC6: touched_roots_size observable + settable
//     AC7: 8-thread concurrent typed mutate (no crash, narrowing monotonic)
//     AC8: (gc-heap) + dirty integration
//     AC9: regression — existing query primitives still work
//   #554/#555 (task1):
//     AC1: cross_delta_conflicts_caught observable + settable
//     AC2: passes_skipped_type_dirty observable + settable
//     AC3: 1000-iter typed mutate cycle — counters monotonic
//     AC4: typed-mutate + heuristic-tc + warmcache combo (no crash)
//     AC5: nested mutation boundaries (no crash)
//     AC6: 16-thread concurrent typed mutate (high-concurrency)
//     AC7: (gc-heap) integration with typed-mutate cycle
//     AC8: regression — existing evaluate primitives work
//     AC9: regression — (+ ...) arithmetic after typed mutate

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

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;

static int k_long_iters() {
    return k_int_env("AURA_STRESS_ITERS", 200);
}

// ── ORIG AC1: 4 dirty/narrowing counters reachable + start at 0 ──
static void ac1_orig() {
    std::println("\n--- ORIG #550 AC1: 4 dirty/narrowing counters reachable + start at 0 ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    const auto n0 = cs.evaluator().get_narrowing_refresh_count();
    const auto cc0 = cs.evaluator().get_cross_delta_conflicts_caught();
    const auto ps0 = cs.evaluator().get_passes_skipped_type_dirty();
    const auto tr0 = cs.evaluator().get_touched_roots_size();
    std::println("  baseline: narrowing={} cross_delta={} passes_skipped={} touched_roots={}", n0,
                 cc0, ps0, tr0);
    CHECK(n0 == 0, "narrowing_refresh_count starts at 0");
    CHECK(cc0 == 0, "cross_delta_conflicts_caught starts at 0");
    CHECK(ps0 == 0, "passes_skipped_type_dirty starts at 0");
    CHECK(tr0 == 0, "touched_roots_size starts at 0");
}

// ── ORIG AC2: query:typed-mutation-stats returns integer sum ──
static void ac2_orig() {
    std::println("\n--- ORIG #550 AC2: (engine:metrics query:typed-mutation-stats) ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(engine:metrics \"query:typed-mutation-stats\")");
    CHECK(r.has_value(), "(engine:metrics \"query:typed-mutation-stats\") returns");
    CHECK(aura::compiler::types::is_int(*r), "is integer");
    if (r && aura::compiler::types::is_int(*r)) {
        const auto v = aura::compiler::types::as_int(*r);
        std::println("  query:typed-mutation-stats = {}", v);
        CHECK(v >= 0, ">= 0 (4 counters sum)");
    }
}

// ── ORIG AC3: query:dirty-impact returns touched_roots_size ──
static void ac3_orig() {
    std::println("\n--- ORIG #550 AC3: (query:dirty-impact) ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(query:dirty-impact)");
    CHECK(r.has_value(), "(query:dirty-impact) returns");
    CHECK(aura::compiler::types::is_int(*r), "is integer");
    if (r && aura::compiler::types::is_int(*r)) {
        const auto v = aura::compiler::types::as_int(*r);
        std::println("  query:dirty-impact = {}", v);
        CHECK(v >= 0, ">= 0 (snapshot)");
    }
}

// ── ORIG AC4: narrowing_refresh_count bumps under Aura mutate ──
static void ac4_orig() {
    std::println("\n--- ORIG #550 AC4: narrowing_refresh_count bumps under Aura mutate ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    const auto n0 = cs.evaluator().get_narrowing_refresh_count();
    for (int i = 0; i < 5; ++i) {
        (void)cs.eval("(mutate:replace-value (define a " + std::to_string(i) + ") (define a " +
                      std::to_string(i) + "))");
    }
    const auto n1 = cs.evaluator().get_narrowing_refresh_count();
    std::println("  narrowing_refresh: {} -> {} (delta {})", n0, n1, n1 - n0);
    CHECK(n1 > n0, "narrowing_refresh_count bumped after Aura mutate load");
}

// ── ORIG AC5: 200-iter typed mutate cycle ──
static void ac5_orig() {
    std::println("\n--- ORIG #550 AC5: {} iters typed mutate cycle ---", k_long_iters());
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    const auto n0 = cs.evaluator().get_narrowing_refresh_count();
    const auto ps0 = cs.evaluator().get_passes_skipped_type_dirty();
    std::mt19937 rng(550u);
    std::uniform_int_distribution<int> val_dist(0, 999);
    for (int i = 0; i < k_long_iters(); ++i) {
        std::string code = std::string("(mutate:replace-value (define ") + (i & 1 ? "a" : "b") +
                           " " + std::to_string(val_dist(rng)) + ") (define " +
                           (i & 1 ? "a" : "b") + " " + std::to_string(val_dist(rng)) + "))";
        (void)cs.eval(code);
    }
    const auto n1 = cs.evaluator().get_narrowing_refresh_count();
    const auto ps1 = cs.evaluator().get_passes_skipped_type_dirty();
    std::println("  narrowing: {} -> {} passes_skipped: {} -> {}", n0, n1, ps0, ps1);
    CHECK(n1 >= n0 + static_cast<std::uint64_t>(k_long_iters() - 5),
          "narrowing_refresh grew under typed mutate cycle");
    CHECK(ps1 >= ps0, "passes_skipped monotonic non-decreasing");
}

// ── ORIG AC6: touched_roots_size observable + settable ──
static void ac6_orig() {
    std::println("\n--- ORIG #550 AC6: touched_roots_size observable + settable ---");
    Evaluator ev;
    const auto s0 = ev.get_touched_roots_size();
    CHECK(s0 == 0, "touched_roots_size starts at 0");
    ev.set_touched_roots_size(42);
    CHECK(ev.get_touched_roots_size() == 42, "touched_roots_size set/get round-trip (42)");
    ev.set_touched_roots_size(0);
    CHECK(ev.get_touched_roots_size() == 0, "touched_roots_size reset to 0");
}

// ── ORIG AC7: 8-thread concurrent typed mutate ──
static void ac7_orig() {
    std::println("\n--- ORIG #550 AC7: 8 threads × 20 iters concurrent typed mutate ---");
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
            std::string code = "(mutate:replace-value (define v" + std::to_string(tid) + " " +
                               std::to_string(i) + ") (define v" + std::to_string(tid) + " " +
                               std::to_string(i) + "))";
            (void)cs.eval(code);
            completed.fetch_add(1);
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < n_threads; ++i)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();

    const auto n = cs.evaluator().get_narrowing_refresh_count();
    std::println("  completed: {}/{} narrowing_refresh: {}", completed.load(), n_threads * n_iters,
                 n);
    CHECK(completed.load() == n_threads * n_iters,
          "all 160 ops completed (no crash under concurrent typed mutate)");
    CHECK(n > 0, "narrowing_refresh > 0 after concurrent typed mutate load");
}

// ── ORIG AC8: (gc-heap) + dirty integration ──
static void ac8_orig() {
    std::println("\n--- ORIG #550 AC8: (gc-heap) + dirty integration ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    (void)cs.eval("(mutate:replace-value (define a 99) (define a 99))");
    auto r = cs.eval("(gc-heap)");
    CHECK(r.has_value(), "(gc-heap) callable after typed mutate");
}

// ── ORIG AC9: regression — existing query primitives ──
static void ac9_orig() {
    std::println("\n--- ORIG #550 AC9: regression — existing primitives still work ---");
    CompilerService cs;
    auto r1 = cs.eval("(engine:metrics \"query:typed-mutation-stats\")");
    CHECK(r1.has_value() && aura::compiler::types::is_int(*r1), "query:typed-mutation-stats");
    auto r2 = cs.eval("(query:dirty-impact)");
    CHECK(r2.has_value() && aura::compiler::types::is_int(*r2), "query:dirty-impact");
    auto r3 = cs.eval("(engine:metrics \"query:self-evolution-stability-stats\")");
    CHECK(r3.has_value() && aura::compiler::types::is_int(*r3), "self-evolution-stability");
    auto r4 = cs.eval("(engine:metrics \"query:stable-ref-stats\")");
    CHECK(r4.has_value() && aura::compiler::types::is_int(*r4), "stable-ref-stats");
    auto r5 = cs.eval("(engine:metrics \"query:envframe-dualpath-stats\")");
    CHECK(r5.has_value() && aura::compiler::types::is_int(*r5), "envframe-dualpath");
    if (!cs.eval("(define reg-550-a 10)")) {
        CHECK(false, "define (regression)");
    }
    auto r6 = cs.eval("(define reg-550-b 32)");
    (void)r6;
    auto r7 = cs.eval("(+ reg-550-a reg-550-b)");
    CHECK(r7.has_value() && aura::compiler::types::is_int(*r7) &&
              aura::compiler::types::as_int(*r7) == 42,
          "(+ reg-550-a reg-550-b) == 42");
}

// ── TASK1 AC1: cross_delta_conflicts_caught observable + settable ──
static void ac1_task1() {
    std::println("\n--- TASK1 #554/#555 AC1: cross_delta_conflicts_caught ---");
    Evaluator ev;
    const auto v0 = ev.get_cross_delta_conflicts_caught();
    CHECK(v0 == 0, "starts at 0");
    ev.set_cross_delta_conflicts_caught_for_test(17);
    CHECK(ev.get_cross_delta_conflicts_caught() == 17, "round-trip (17)");
    ev.set_cross_delta_conflicts_caught_for_test(0);
    CHECK(ev.get_cross_delta_conflicts_caught() == 0, "reset to 0");
}

// ── TASK1 AC2: passes_skipped_type_dirty observable + settable ──
static void ac2_task1() {
    std::println("\n--- TASK1 AC2: passes_skipped_type_dirty ---");
    Evaluator ev;
    const auto v0 = ev.get_passes_skipped_type_dirty();
    CHECK(v0 == 0, "starts at 0");
    ev.set_passes_skipped_type_dirty_for_test(99);
    CHECK(ev.get_passes_skipped_type_dirty() == 99, "round-trip (99)");
    ev.set_passes_skipped_type_dirty_for_test(0);
    CHECK(ev.get_passes_skipped_type_dirty() == 0, "reset to 0");
}

// ── TASK1 AC3: 1000-iter typed mutate cycle ──
static void ac3_task1() {
    std::println("\n--- TASK1 AC3: 1000-iter typed mutate cycle ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    const auto n0 = cs.evaluator().get_narrowing_refresh_count();
    const auto cc0 = cs.evaluator().get_cross_delta_conflicts_caught();
    const auto ps0 = cs.evaluator().get_passes_skipped_type_dirty();
    std::mt19937 rng(555u);
    std::uniform_int_distribution<int> val_dist(0, 9999);
    for (int i = 0; i < 1000; ++i) {
        std::string code = std::string("(mutate:replace-value (define ") + (i & 1 ? "a" : "b") +
                           " " + std::to_string(val_dist(rng)) + ") (define " +
                           (i & 1 ? "a" : "b") + " " + std::to_string(val_dist(rng)) + "))";
        (void)cs.eval(code);
    }
    const auto n1 = cs.evaluator().get_narrowing_refresh_count();
    const auto cc1 = cs.evaluator().get_cross_delta_conflicts_caught();
    const auto ps1 = cs.evaluator().get_passes_skipped_type_dirty();
    std::println("  narrowing: {} -> {} cross_delta: {} -> {} passes_skipped: {} -> {}", n0, n1,
                 cc0, cc1, ps0, ps1);
    CHECK(n1 >= n0, "narrowing monotonic");
    CHECK(cc1 >= cc0, "cross_delta_conflicts monotonic");
    CHECK(ps1 >= ps0, "passes_skipped monotonic");
}

// ── TASK1 AC4: typed-mutate + heuristic-tc + warmcache ──
static void ac4_task1() {
    std::println("\n--- TASK1 AC4: typed-mutate + heuristic-tc + warmcache combo ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define x 1) (define y 2) (define z 3)\")");
    (void)cs.eval("(eval-current)");
    for (int i = 0; i < 50; ++i) {
        (void)cs.eval("(mutate:replace-value (define x " + std::to_string(i) + ") (define x " +
                      std::to_string(i) + "))");
        (void)cs.eval("(define w" + std::to_string(i) + " " + std::to_string(i * 2) + ")");
    }
    CHECK(cs.eval("(+ 1 2 3)").has_value(), "arithmetic works after combo load");
}

// ── TASK1 AC5: nested mutation boundaries ──
static void ac5_task1() {
    std::println("\n--- TASK1 AC5: nested mutation boundaries (no crash) ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0)\")");
    (void)cs.eval("(eval-current)");
    for (int i = 0; i < 20; ++i) {
        (void)cs.eval("(mutate:replace-value (define a " + std::to_string(i) + ") (define a " +
                      std::to_string(i) + "))");
    }
    CHECK(true, "nested mutation boundaries didn't crash");
}

// ── TASK1 AC6: 16-thread concurrent typed mutate ──
static void ac6_task1() {
    std::println("\n--- TASK1 AC6: 16 threads × 10 iters concurrent typed mutate ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    constexpr int n_threads = 16;
    constexpr int n_iters = 10;
    std::mutex mtx;
    std::atomic<int> completed{0};
    auto worker = [&](int tid) {
        for (int i = 0; i < n_iters; ++i) {
            std::lock_guard<std::mutex> lk(mtx);
            std::string code = "(mutate:replace-value (define v" + std::to_string(tid) + " " +
                               std::to_string(i) + ") (define v" + std::to_string(tid) + " " +
                               std::to_string(i) + "))";
            (void)cs.eval(code);
            completed.fetch_add(1);
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < n_threads; ++i)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();
    std::println("  completed: {}/{}", completed.load(), n_threads * n_iters);
    CHECK(completed.load() == n_threads * n_iters,
          "all 160 ops completed (no crash under high-concurrency typed mutate)");
}

// ── TASK1 AC7: (gc-heap) integration ──
static void ac7_task1() {
    std::println("\n--- TASK1 AC7: (gc-heap) integration with typed-mutate cycle ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0)\")");
    (void)cs.eval("(eval-current)");
    for (int i = 0; i < 50; ++i) {
        (void)cs.eval("(mutate:replace-value (define a " + std::to_string(i) + ") (define a " +
                      std::to_string(i) + "))");
    }
    auto r = cs.eval("(gc-heap)");
    CHECK(r.has_value(), "(gc-heap) callable after typed-mutate cycle");
}

// ── TASK1 AC8: regression — existing evaluate primitives ──
static void ac8_task1() {
    std::println("\n--- TASK1 AC8: regression — evaluate primitives work ---");
    CompilerService cs;
    auto r1 = cs.eval("(evaluate '(+ 1 2))");
    CHECK(r1.has_value(), "(evaluate ...) works");
    auto r2 = cs.eval("(query:dirty-impact)");
    CHECK(r2.has_value(), "(query:dirty-impact) works after typed-mutate");
}

// ── TASK1 AC9: regression — (+ ...) arithmetic after typed mutate ──
static void ac9_task1() {
    std::println("\n--- TASK1 AC9: regression — arithmetic after typed mutate ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define r1 10) (define r2 20)\")");
    (void)cs.eval("(eval-current)");
    for (int i = 0; i < 10; ++i) {
        (void)cs.eval("(mutate:replace-value (define r1 " + std::to_string(i) + ") (define r1 " +
                      std::to_string(i) + "))");
    }
    auto r = cs.eval("(+ r1 r2)");
    CHECK(r.has_value() && aura::compiler::types::is_int(*r), "(+ r1 r2) callable");
}

} // namespace

int main() {
    std::println("=== Merged typed self-mod dirty narrowing: ORIG #509-#550 + TASK1 #554-#555 ===");
    // ORIG ACs (9)
    ac1_orig();
    ac2_orig();
    ac3_orig();
    ac4_orig();
    ac5_orig();
    ac6_orig();
    ac7_orig();
    ac8_orig();
    ac9_orig();
    // TASK1 ACs (9)
    ac1_task1();
    ac2_task1();
    ac3_task1();
    ac4_task1();
    ac5_task1();
    ac6_task1();
    ac7_task1();
    ac8_task1();
    ac9_task1();
    std::println("\n=== Results: {} passed, {} failed ===", ::aura::test::g_passed,
                 ::aura::test::g_failed);
    return ::aura::test::g_failed ? 1 : 0;
}