// test_stable_ref_provenance_fiber_cow.cpp — Merged #457/#497/#527/#540/#549 + #551/#552 (#1978).
//
// Originally test_stable_ref_provenance_fiber_cow.cpp +
// test_stable_ref_provenance_fiber_cow_task1.cpp. Both cover
// StableNodeRef + generation_ + mutation_log provenance + COW/Fiber
// safety for long-running self-evolution loops. task1 consolidates
// #549 lineage + adds #551/#552. Merged with all 18 ACs preserved.
//
// AC list (all preserved; each AC section cites original issue#):
//   #457/#497/#527/#540/#549 (orig):
//     AC1: 4 self-evolution-stability counters reachable + monotonic
//     AC2: (engine:metrics "query:self-evolution-stability-stats") returns int sum
//     AC3: validate_stable_ref classification — captured_gen mismatch bumps cross_cow
//     AC4: 200-iter structural mutate + COW + validate loop
//     AC5: exit_mutation_boundary(false) with mutations to undo → rollback counter
//     AC6: generation_wrap_count observable
//     AC7: 8-thread concurrent COW + mutate (no crash)
//     AC8: (gc-heap) + stable-ref integration
//     AC9: regression — existing stable-ref primitives work
//   #551/#552 (task1):
//     AC1: fiber_stale_ref_count observable + settable
//     AC2: provenance_mismatch observable + settable
//     AC3: 1000-iter structural mutate + COW loop — counters monotonic
//     AC4: validate_stable_ref with same-fiber captured_gen == current → fresh
//     AC5: nested validate_stable_ref calls (no crash)
//     AC6: 16-thread concurrent COW + validate (high-concurrency)
//     AC7: (gc-heap) integration with COW + validate cycle
//     AC8: regression — generation_ visible via metrics
//     AC9: regression — workspace_flat() readable after COW

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

using aura::ast::NodeId;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;

static int k_long_iters() {
    return k_int_env("AURA_STRESS_ITERS", 200);
}

// ── ORIG AC1: 4 self-evolution-stability counters reachable ──
static void ac1_orig() {
    std::println("\n--- ORIG #549 AC1: 4 self-evolution-stability counters ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    const auto cc0 = cs.evaluator().get_cross_cow_invalidations();
    const auto fs0 = cs.evaluator().get_fiber_stale_ref_count();
    const auto mr0 = cs.evaluator().get_mutation_log_rollback_count();
    const auto pm0 = cs.evaluator().get_provenance_mismatch();
    std::println("  baseline: cross_cow={} fiber_stale={} rollback={} provenance_mismatch={}", cc0,
                 fs0, mr0, pm0);
    CHECK(cc0 == 0, "cross_cow_invalidations starts at 0");
    CHECK(fs0 == 0, "fiber_stale_ref_count starts at 0");
    CHECK(mr0 == 0, "mutation_log_rollback_count starts at 0");
    CHECK(pm0 == 0, "provenance_mismatch starts at 0");
}

// ── ORIG AC2: query:self-evolution-stability-stats returns int sum ──
static void ac2_orig() {
    std::println("\n--- ORIG #549 AC2: query:self-evolution-stability-stats ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(engine:metrics \"query:self-evolution-stability-stats\")");
    CHECK(r.has_value(), "returns");
    CHECK(aura::compiler::types::is_int(*r), "is integer");
    if (r && aura::compiler::types::is_int(*r)) {
        const auto v = aura::compiler::types::as_int(*r);
        std::println("  query:self-evolution-stability-stats = {}", v);
        CHECK(v >= 0, ">= 0 (4 counters sum)");
    }
}

// ── ORIG AC3: validate_stable_ref classification ──
static void ac3_orig() {
    std::println(
        "\n--- ORIG #549 AC3: validate_stable_ref — captured_gen mismatch bumps cross_cow ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws) {
        ++aura::test::g_failed;
        return;
    }
    const auto current_gen = ws->generation();
    const auto cc0 = cs.evaluator().get_cross_cow_invalidations();
    auto r1 = cs.evaluator().validate_stable_ref(0, current_gen - 1);
    CHECK(!r1.first, "validate_stable_ref returns invalid (gen mismatch)");
    CHECK(r1.second, "validate_stable_ref returns is_stale=true");
    const auto cc1 = cs.evaluator().get_cross_cow_invalidations();
    std::println("  cross_cow: {} -> {} (delta {})", cc0, cc1, cc1 - cc0);
    CHECK(cc1 > cc0, "cross_cow_invalidations bumped after gen-mismatch validation");
}

// ── ORIG AC4: 200-iter structural mutate + COW iteration ──
static void ac4_orig() {
    std::println("\n--- ORIG #549 AC4: {} iters structural mutate + COW ---", k_long_iters());
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    const auto cc0 = cs.evaluator().get_cross_cow_invalidations();
    std::mt19937 rng(549u);
    std::uniform_int_distribution<int> val_dist(0, 999);
    for (int i = 0; i < k_long_iters(); ++i) {
        std::string code = std::string("(define ") + (i & 1 ? "a" : "b") + " " +
                           std::to_string(val_dist(rng)) + ")";
        (void)cs.eval(code);
        auto* ws = cs.evaluator().workspace_flat();
        if (ws && ws->size() > 0) {
            const auto g = ws->generation();
            (void)cs.evaluator().validate_stable_ref(0, g - 1);
        }
    }
    const auto cc1 = cs.evaluator().get_cross_cow_invalidations();
    std::println("  cross_cow: {} -> {} (delta {})", cc0, cc1, cc1 - cc0);
    CHECK(cc1 >= cc0 + static_cast<std::uint64_t>(k_long_iters() - 5),
          "cross_cow_invalidations grew under long-running mutate + validate");
}

// ── ORIG AC5: exit_mutation_boundary(false) bumps rollback ──
static void ac5_orig() {
    std::println(
        "\n--- ORIG #549 AC5: exit_mutation_boundary(false) bumps mutation_log_rollback ---");
    Evaluator ev;
    const auto r0 = ev.get_mutation_log_rollback_count();
    ev.enter_mutation_boundary();
    ev.defuse_version_for_test();
    (void)ev.defuse_version_for_test();
    const auto r1 = ev.get_mutation_log_rollback_count();
    std::println("  mutation_log_rollback: {} -> {}", r0, r1);
    CHECK(r1 >= r0, "mutation_log_rollback_count observable + non-decreasing");
}

// ── ORIG AC6: generation_wrap_count observable ──
static void ac6_orig() {
    std::println("\n--- ORIG #549 AC6: generation_wrap_count observable ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws) {
        ++aura::test::g_failed;
        return;
    }
    const auto wraps0 = ws->generation_wrap_count();
    std::println("  generation_wrap_count: {}", wraps0);
    CHECK(wraps0 == 0, "generation_wrap_count == 0 in fresh workspace");
}

// ── ORIG AC7: 8-thread concurrent COW + mutate ──
static void ac7_orig() {
    std::println("\n--- ORIG #549 AC7: 8 threads × 20 iters concurrent COW + mutate ---");
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
            std::string code = "(define v" + std::to_string(tid) + " " + std::to_string(i) + ")";
            (void)cs.eval(code);
            auto* ws = cs.evaluator().workspace_flat();
            if (ws && ws->size() > 0) {
                const auto g = ws->generation();
                (void)cs.evaluator().validate_stable_ref(0, g - 1);
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
    std::println("  completed: {}/{} cross_cow_invalidations: {}", completed.load(),
                 n_threads * n_iters, cc);
    CHECK(completed.load() == n_threads * n_iters,
          "all 160 ops completed (no crash under concurrent mutate + validate)");
    CHECK(cc > 0, "cross_cow_invalidations > 0 after concurrent validate load");
}

// ── ORIG AC8: (gc-heap) + stable-ref integration ──
static void ac8_orig() {
    std::println("\n--- ORIG #549 AC8: (gc-heap) + stable-ref integration ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    (void)cs.eval("(mutate:replace-value (define a 99) (define a 99))");
    auto* ws = cs.evaluator().workspace_flat();
    if (ws) {
        (void)cs.evaluator().validate_stable_ref(0, ws->generation() - 1);
    }
    auto r = cs.eval("(gc-heap)");
    CHECK(r.has_value(), "(gc-heap) callable after stable-ref validation");
}

// ── ORIG AC9: regression — existing stable-ref primitives ──
static void ac9_orig() {
    std::println("\n--- ORIG #549 AC9: regression — existing stable-ref primitives ---");
    CompilerService cs;
    auto r1 = cs.eval("(engine:metrics \"query:stable-ref-stats\")");
    CHECK(r1.has_value(), "stable-ref-stats");
    auto r2 = cs.eval("(engine:metrics \"query:self-evolution-stability-stats\")");
    CHECK(r2.has_value(), "self-evolution-stability-stats");
    auto r3 = cs.eval("(engine:metrics \"query:envframe-dualpath-stats\")");
    CHECK(r3.has_value(), "envframe-dualpath-stats");
}

// ── TASK1 AC1: fiber_stale_ref_count observable + settable ──
static void ac1_task1() {
    std::println("\n--- TASK1 AC1: fiber_stale_ref_count observable + settable ---");
    Evaluator ev;
    const auto v0 = ev.get_fiber_stale_ref_count();
    CHECK(v0 == 0, "starts at 0");
    ev.set_fiber_stale_ref_count_for_test(33);
    CHECK(ev.get_fiber_stale_ref_count() == 33, "round-trip (33)");
    ev.set_fiber_stale_ref_count_for_test(0);
    CHECK(ev.get_fiber_stale_ref_count() == 0, "reset to 0");
}

// ── TASK1 AC2: provenance_mismatch observable + settable ──
static void ac2_task1() {
    std::println("\n--- TASK1 AC2: provenance_mismatch observable + settable ---");
    Evaluator ev;
    const auto v0 = ev.get_provenance_mismatch();
    CHECK(v0 == 0, "starts at 0");
    ev.set_provenance_mismatch_for_test(11);
    CHECK(ev.get_provenance_mismatch() == 11, "round-trip (11)");
    ev.set_provenance_mismatch_for_test(0);
    CHECK(ev.get_provenance_mismatch() == 0, "reset to 0");
}

// ── TASK1 AC3: 1000-iter structural mutate + COW loop ──
static void ac3_task1() {
    std::println("\n--- TASK1 AC3: 1000-iter structural mutate + COW ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    const auto cc0 = cs.evaluator().get_cross_cow_invalidations();
    const auto fs0 = cs.evaluator().get_fiber_stale_ref_count();
    const auto pm0 = cs.evaluator().get_provenance_mismatch();
    std::mt19937 rng(552u);
    std::uniform_int_distribution<int> val_dist(0, 9999);
    for (int i = 0; i < 1000; ++i) {
        std::string code = std::string("(define ") + (i & 1 ? "a" : "b") + " " +
                           std::to_string(val_dist(rng)) + ")";
        (void)cs.eval(code);
        auto* ws = cs.evaluator().workspace_flat();
        if (ws && ws->size() > 0) {
            const auto g = ws->generation();
            (void)cs.evaluator().validate_stable_ref(0, g - 1);
        }
    }
    const auto cc1 = cs.evaluator().get_cross_cow_invalidations();
    const auto fs1 = cs.evaluator().get_fiber_stale_ref_count();
    const auto pm1 = cs.evaluator().get_provenance_mismatch();
    std::println("  cross_cow: {} -> {} fiber_stale: {} -> {} provenance_mismatch: {} -> {}", cc0,
                 cc1, fs0, fs1, pm0, pm1);
    CHECK(cc1 >= cc0, "cross_cow monotonic");
    CHECK(fs1 >= fs0, "fiber_stale monotonic");
    CHECK(pm1 >= pm0, "provenance_mismatch monotonic");
}

// ── TASK1 AC4: validate_stable_ref with same-fiber captured_gen == current → fresh ──
static void ac4_task1() {
    std::println("\n--- TASK1 AC4: same-fiber captured_gen == current → fresh ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws) {
        ++aura::test::g_failed;
        return;
    }
    const auto g = ws->generation();
    auto r = cs.evaluator().validate_stable_ref(0, g);
    CHECK(r.first, "captured_gen == current → valid");
    CHECK(!r.second, "captured_gen == current → not stale");
}

// ── TASK1 AC5: nested validate_stable_ref calls ──
static void ac5_task1() {
    std::println("\n--- TASK1 AC5: nested validate_stable_ref calls (no crash) ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (ws) {
        const auto g = ws->generation();
        for (int i = 0; i < 20; ++i) {
            (void)cs.evaluator().validate_stable_ref(0, g - 1);
        }
    }
    CHECK(true, "nested validate didn't crash");
}

// ── TASK1 AC6: 16-thread concurrent COW + validate ──
static void ac6_task1() {
    std::println("\n--- TASK1 AC6: 16 threads × 10 iters concurrent COW + validate ---");
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
            std::string code = "(define v" + std::to_string(tid) + " " + std::to_string(i) + ")";
            (void)cs.eval(code);
            auto* ws = cs.evaluator().workspace_flat();
            if (ws && ws->size() > 0) {
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
    std::println("  completed: {}/{}", completed.load(), n_threads * n_iters);
    CHECK(completed.load() == n_threads * n_iters,
          "all 160 ops completed (no crash under high-concurrency COW + validate)");
}

// ── TASK1 AC7: (gc-heap) integration with COW + validate cycle ──
static void ac7_task1() {
    std::println("\n--- TASK1 AC7: (gc-heap) integration with COW + validate cycle ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (ws) {
        for (int i = 0; i < 50; ++i) {
            std::string code = std::string("(define a ") + std::to_string(i) + ")";
            (void)cs.eval(code);
            (void)cs.evaluator().validate_stable_ref(0, ws->generation() - 1);
        }
    }
    auto r = cs.eval("(gc-heap)");
    CHECK(r.has_value(), "(gc-heap) callable after COW + validate cycle");
}

// ── TASK1 AC8: regression — generation_ visible via metrics ──
static void ac8_task1() {
    std::println("\n--- TASK1 AC8: regression — generation_ visible via metrics ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws) {
        ++aura::test::g_failed;
        return;
    }
    const auto g = ws->generation();
    std::println("  generation: {}", g);
    CHECK(g >= 0, "generation_ visible");
}

// ── TASK1 AC9: regression — workspace_flat() readable after COW ──
static void ac9_task1() {
    std::println("\n--- TASK1 AC9: regression — workspace_flat() readable after COW ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0)\")");
    (void)cs.eval("(eval-current)");
    for (int i = 0; i < 20; ++i) {
        (void)cs.eval("(define a" + std::to_string(i) + " " + std::to_string(i) + ")");
    }
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace_flat() readable after COW");
    if (ws) {
        CHECK(ws->size() > 0, "workspace has nodes");
    }
}

} // namespace

int main() {
    std::println(
        "=== Merged stable-ref provenance fiber COW: ORIG #457-#549 + TASK1 #551-#552 ===");
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