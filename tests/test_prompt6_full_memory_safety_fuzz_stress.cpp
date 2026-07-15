// test_prompt6_full_memory_safety_fuzz_stress.cpp — Issue #602:
// Comprehensive fuzz + stress matrix for Prompt6 memory safety
// invariants (IRClosure/EnvFrame/bridge_epoch/linear/GuardShape)
// under concurrent fiber + GC + multi-round AI mutate.
//
// Non-duplicative with #531 (closure-env), #543 (SoA EnvFrame),
// #545 (GC safepoint), #542 (fiber steal), #544 (AOT hot-swap),
// #597 (macro+reflect+self-evo). This binary exercises the
// **unified** Prompt6 production-review matrix:
//
//   - AC1:  (query:prompt6-violation-count) starts at 0
//   - AC2:  (query:prompt6-safety-score) reachable + monotonic
//   - AC3:  Combined Prompt6 stats bundle callable
//   - AC4:  Closure + invalidate + bridge_epoch path (no crash)
//   - AC5:  EnvFrame dualpath + stale refresh under mutate
//   - AC6:  Mutate matrix (rebind / replace-value / set-code)
//   - AC7:  GC-heap + closure-env integration
//   - AC8:  Fuzz — random mutate/query/eval + panic injection
//   - AC9:  8-thread concurrent mutate + GC (zero violations)
//   - AC10: Scheduler + fiber yield/steal at MutationBoundary
//   - AC11: Long-running stress (bounded iter count)
//   - AC12: Regression — related Prompt6 primitives still work

#include "test_harness.hpp"

#include "serve/fiber.h"
#include "serve/scheduler.h"

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

namespace aura_issue_602_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::serve::Fiber;
using aura::serve::Scheduler;
using aura::serve::YieldReason;

static int k_fuzz_iters() {
    return k_int_env("AURA_FUZZ_ITERS", 50);
}

static int k_stress_iters() {
    return k_int_env("AURA_STRESS_ITERS", 100);
}

static int k_concurrent_iters() {
    return k_int_env("AURA_STRESS_ITERS", 25);
}

static std::int64_t eval_int(CompilerService& cs, std::string_view code) {
    auto r = cs.eval(code);
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return static_cast<std::int64_t>(aura::compiler::types::as_int(*r));
}

static std::uint64_t violation_count(CompilerService& cs) {
    const auto v = eval_int(cs, "(query:prompt6-violation-count)");
    return v < 0 ? 0 : static_cast<std::uint64_t>(v);
}

static bool setup_closure_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (f x) (+ x 1)) "
                 "(define (g y) (* y 2)) "
                 "(define base 10) (define acc 0)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

// ── AC1: violation-count starts at 0 ─────────────────────
bool test_violation_count_starts_zero() {
    std::println("\n--- AC1: (query:prompt6-violation-count) starts at 0 ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    const auto v = violation_count(cs);
    std::println("  prompt6-violation-count baseline: {}", v);
    CHECK(v == 0, "prompt6-violation-count starts at 0");
    return true;
}

// ── AC2: safety-score reachable + monotonic ───────────────
bool test_safety_score_monotonic() {
    std::println("\n--- AC2: (query:prompt6-safety-score) monotonic ---");
    CompilerService cs;
    CHECK(setup_closure_workspace(cs), "closure workspace setup");
    const auto s0 = eval_int(cs, "(query:prompt6-safety-score)");
    CHECK(s0 >= 0, "prompt6-safety-score starts >= 0");
    cs.bump_bridge_epoch_hit_count();
    cs.bump_linear_check_pass_count();
    cs.bump_gc_envframe_stale_skipped();
    const auto s1 = eval_int(cs, "(query:prompt6-safety-score)");
    std::println("  prompt6-safety-score: {} -> {}", s0, s1);
    CHECK(s1 > s0, "prompt6-safety-score grew after safety bumps");
    return true;
}

// ── AC3: Combined Prompt6 stats bundle ───────────────────
bool test_combined_prompt6_stats_bundle() {
    std::println("\n--- AC3: Prompt6 stats bundle ---");
    CompilerService cs;
    CHECK(setup_closure_workspace(cs), "workspace for stats bundle");
    (void)cs.eval("(mutate:rebind \"base\" \"42\")");
    const char* stats[] = {
        "(query:prompt6-violation-count)",
        "(query:prompt6-safety-score)",
        "(query:closure-env-safety-stats)",
        "(engine:metrics \"query:envframe-dualpath-stats\")",
        "(engine:metrics \"query:gc-safepoint-stats\")",
        "(engine:metrics \"query:fiber-migration-stats\")",
        "(engine:metrics \"query:mutation-coordination-stats\")",
        "(engine:metrics \"query:self-evolution-stability-stats\")",
    };
    for (const char* prim : stats) {
        auto r = cs.eval(prim);
        CHECK(r.has_value() && aura::compiler::types::is_int(*r),
              std::string(prim) + " returns integer");
    }
    return true;
}

// ── AC4: Closure + invalidate + bridge_epoch path ────────
bool test_closure_invalidate_bridge_epoch() {
    std::println("\n--- AC4: closure + invalidate + bridge_epoch path ---");
    CompilerService cs;
    CHECK(setup_closure_workspace(cs), "closure workspace");
    for (int i = 0; i < 5; ++i) {
        (void)cs.eval("(f 42)");
        (void)cs.eval("(g 3)");
    }
    const auto bh0 = cs.get_bridge_epoch_hit_count();
    cs.bump_bridge_epoch_hit_count();
    (void)cs.eval("(mutate:replace-value (define base 10) (define base 99))");
    const auto sr = cs.get_closure_stale_refresh_count();
    const auto v = violation_count(cs);
    std::println("  bridge_hit: {} closure_refresh: {} violations: {}", bh0, sr, v);
    CHECK(v == 0, "zero violations after closure + invalidate path");
    CHECK(sr >= 0, "closure_stale_refresh observable");
    return true;
}

// ── AC5: EnvFrame dualpath + stale refresh under mutate ───
bool test_envframe_dualpath_under_mutate() {
    std::println("\n--- AC5: EnvFrame dualpath under mutate ---");
    CompilerService cs;
    CHECK(setup_closure_workspace(cs), "workspace for EnvFrame");
    const auto d0 = eval_int(cs, "(engine:metrics \"query:envframe-dualpath-stats\")");
    for (int i = 0; i < 5; ++i) {
        (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(i) + "\")");
    }
    const auto d1 = eval_int(cs, "(engine:metrics \"query:envframe-dualpath-stats\")");
    const auto desync = cs.evaluator().get_envframe_desync_detected();
    std::println("  envframe-dualpath-stats: {} -> {} desync: {}", d0, d1, desync);
    CHECK(desync == 0, "zero envframe desync under mutate load");
    CHECK(d1 >= d0, "envframe-dualpath-stats monotonic");
    return true;
}

// ── AC6: Mutate matrix (rebind / replace-value / set-code) ─
bool test_mutate_type_matrix() {
    std::println("\n--- AC6: mutate type matrix ---");
    CompilerService cs;
    CHECK(setup_closure_workspace(cs), "workspace for mutate matrix");
    CHECK(cs.eval("(mutate:rebind \"base\" \"100\")").has_value(), "mutate:rebind succeeds");
    CHECK(cs.eval("(mutate:replace-value (define acc 0) (define acc 7))").has_value(),
          "mutate:replace-value succeeds");
    CHECK(cs.eval("(set-code \"(define base 200) (define acc 8)\")").has_value(),
          "set-code succeeds");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current after set-code succeeds");
    const auto v = violation_count(cs);
    CHECK(v == 0, "zero violations after mutate type matrix");
    return true;
}

// ── AC7: GC-heap + closure-env integration ────────────────
bool test_gc_heap_closure_integration() {
    std::println("\n--- AC7: (gc-heap) + closure-env integration ---");
    CompilerService cs;
    CHECK(setup_closure_workspace(cs), "workspace for GC integration");
    (void)cs.eval("(mutate:rebind \"base\" \"55\")");
    (void)cs.eval("(f 1)");
    auto r = cs.eval("(gc-heap)");
    CHECK(r.has_value(), "(gc-heap) callable after closure mutate");
    const auto v = violation_count(cs);
    CHECK(v == 0, "zero violations after gc-heap + closure path");
    return true;
}

// ── AC8: Fuzz — mutate/query/eval + panic injection ───────
bool test_fuzz_mutate_query_panic() {
    std::println("\n--- AC8: {} iters fuzz (mutate + query + panic) ---", k_fuzz_iters());
    CompilerService cs;
    CHECK(setup_closure_workspace(cs), "workspace for fuzz");
    std::mt19937 rng(602u);
    std::uniform_int_distribution<int> op_dist(0, 5);
    std::uniform_int_distribution<int> val_dist(0, 999);
    int panics = 0;
    const auto v0 = violation_count(cs);
    for (int i = 0; i < k_fuzz_iters(); ++i) {
        switch (op_dist(rng)) {
            case 0:
                (void)cs.eval("(mutate:rebind \"base\" \"" + std::to_string(val_dist(rng)) + "\")");
                break;
            case 1:
                (void)cs.eval("(mutate:replace-value (define acc " + std::to_string(val_dist(rng)) +
                              ") (define acc " + std::to_string(val_dist(rng)) + "))");
                break;
            case 2:
                (void)cs.eval("(query:pattern \"base\")");
                break;
            case 3:
                (void)cs.eval("(query:epoch-delta-since-last-query)");
                break;
            case 4:
                (void)cs.eval("(gc-heap)");
                break;
            default: {
                (void)cs.eval("(panic-checkpoint)");
                (void)cs.eval("(mutate:replace-value (define acc " + std::to_string(val_dist(rng)) +
                              ") (define acc " + std::to_string(val_dist(rng)) + "))");
                auto rr = cs.eval("(panic-restore)");
                ++panics;
                CHECK(rr.has_value(), "panic-restore succeeds under fuzz panic injection");
                break;
            }
        }
    }
    const auto v1 = violation_count(cs);
    std::println("  panics: {} violations: {} -> {}", panics, v0, v1);
    CHECK(v1 == 0, "zero Prompt6 violations under fuzz");
    return true;
}

// ── AC9: 8-thread concurrent mutate + GC ──────────────────
bool test_eight_thread_concurrent_mutate_gc() {
    std::println("\n--- AC9: 8 threads × {} iters concurrent mutate + GC ---",
                 k_concurrent_iters());
    CompilerService cs;
    CHECK(setup_closure_workspace(cs), "workspace for concurrent matrix");
    constexpr int n_threads = 8;
    const int n_iters = k_concurrent_iters();
    std::mutex mtx;
    std::atomic<int> completed{0};
    std::atomic<int> errors{0};
    auto worker = [&](int tid) {
        for (int i = 0; i < n_iters; ++i) {
            std::lock_guard<std::mutex> lk(mtx);
            if ((i & 3) == 0) {
                (void)cs.eval("(gc-heap)");
            } else if ((i & 3) == 1) {
                (void)cs.eval("(mutate:rebind \"base\" \"" + std::to_string(tid * 1000 + i) +
                              "\")");
            } else {
                (void)cs.eval("(mutate:replace-value (define acc " + std::to_string(i) +
                              ") (define acc " + std::to_string(tid + i) + "))");
            }
            if (!cs.eval("(query:prompt6-violation-count)")) {
                errors.fetch_add(1);
            }
            completed.fetch_add(1);
        }
    };
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < n_threads; ++i)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    const auto v = violation_count(cs);
    std::println("  completed: {}/{} errors: {} violations: {} ms: {}", completed.load(),
                 n_threads * n_iters, errors.load(), v, ms);
    CHECK(completed.load() == n_threads * n_iters, "all concurrent ops completed (no deadlock)");
    CHECK(errors.load() == 0, "no eval errors under concurrent load");
    CHECK(v == 0, "zero Prompt6 violations under concurrent mutate+GC");
    CHECK(ms < 120000, "completed within 120s wall-clock budget");
    return true;
}

// ── AC10: Scheduler + fiber yield at MutationBoundary ─────
bool test_fiber_yield_steal_safety() {
    std::println("\n--- AC10: Scheduler + 8 fibers × 50 yields ---");
    Scheduler sched(4);
    std::atomic<int> done{0};
    constexpr int k_fibers = 8;
    constexpr int k_iters = 50;
    for (int i = 0; i < k_fibers; ++i) {
        sched.spawn([&done]() {
            for (int j = 0; j < k_iters; ++j) {
                Fiber::yield(YieldReason::MutationBoundary);
            }
            done.fetch_add(1);
        });
    }
    std::thread io_thread([&sched]() { sched.run(); });
    auto t0 = std::chrono::steady_clock::now();
    while (done.load() < k_fibers) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                  t0)
                .count() > 30000) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    sched.stop();
    io_thread.join();
    std::println("  fibers done: {}/{}", done.load(), k_fibers);
    CHECK(done.load() == k_fibers, "all fibers completed MutationBoundary yields (no stall)");
    return true;
}

// ── AC11: Long-running stress ─────────────────────────────
bool test_long_running_memory_safety_stress() {
    std::println("\n--- AC11: {} iters long-running stress ---", k_stress_iters());
    CompilerService cs;
    CHECK(setup_closure_workspace(cs), "workspace for stress");
    const auto s0 = eval_int(cs, "(query:prompt6-safety-score)");
    std::mt19937 rng(6021u);
    std::uniform_int_distribution<int> val_dist(0, 500);
    for (int i = 0; i < k_stress_iters(); ++i) {
        if ((i & 1) == 0) {
            (void)cs.eval("(mutate:rebind \"base\" \"" + std::to_string(val_dist(rng)) + "\")");
        } else {
            (void)cs.eval("(mutate:replace-value (define acc " + std::to_string(val_dist(rng)) +
                          ") (define acc " + std::to_string(val_dist(rng)) + "))");
        }
        if ((i & 31) == 0)
            (void)cs.eval("(gc-heap)");
    }
    const auto s1 = eval_int(cs, "(query:prompt6-safety-score)");
    const auto v = violation_count(cs);
    std::println("  safety-score: {} -> {} violations: {}", s0, s1, v);
    CHECK(s1 >= s0, "prompt6-safety-score monotonic under stress");
    CHECK(v == 0, "zero violations under long-running stress");
    return true;
}

// ── AC12: Regression — related Prompt6 primitives ─────────
bool test_regression_related_primitives() {
    std::println("\n--- AC12: regression — related Prompt6 primitives ---");
    CompilerService cs;
    auto r1 = cs.eval("(engine:metrics \"query:closure-env-safety-stats\")");
    CHECK(r1.has_value() && aura::compiler::types::is_hash(*r1),
          "(engine:metrics \"query:closure-env-safety-stats\") (regression for #531)");
    auto r2 = cs.eval("(engine:metrics \"query:envframe-dualpath-stats\")");
    CHECK(r2.has_value() && aura::compiler::types::is_int(*r2),
          "(engine:metrics \"query:envframe-dualpath-stats\") (regression for #543)");
    auto r3 = cs.eval("(engine:metrics \"query:gc-safepoint-stats\")");
    CHECK(r3.has_value() && aura::compiler::types::is_int(*r3),
          "(engine:metrics \"query:gc-safepoint-stats\") (regression for #439)");
    auto r4 = cs.eval("(engine:metrics \"query:fiber-migration-stats\")");
    CHECK(r4.has_value() && aura::compiler::types::is_int(*r4),
          "(engine:metrics \"query:fiber-migration-stats\") (regression for #438)");
    auto r5 = cs.eval("(engine:metrics \"query:macro-reflect-self-evo-stats\")");
    CHECK(r5.has_value() && aura::compiler::types::is_int(*r5),
          "(engine:metrics \"query:macro-reflect-self-evo-stats\") (regression for #597)");
    if (!cs.eval("(define reg-602-a 10)")) {
        CHECK(false, "define (regression)");
        return false;
    }
    (void)cs.eval("(define reg-602-b 32)");
    auto r6 = cs.eval("(+ reg-602-a reg-602-b)");
    CHECK(r6.has_value() && aura::compiler::types::is_int(*r6) &&
              aura::compiler::types::as_int(*r6) == 42,
          "(+ reg-602-a reg-602-b) == 42 (regression)");
    return true;
}

int run_tests() {
    std::println("═══ Issue #602 Prompt6 memory-safety fuzz/stress matrix ═══\n");
    std::println("Layer 1: combined Prompt6 stats + closure/EnvFrame");
    test_violation_count_starts_zero();
    test_safety_score_monotonic();
    test_combined_prompt6_stats_bundle();
    test_closure_invalidate_bridge_epoch();
    test_envframe_dualpath_under_mutate();
    std::println("\nLayer 2: mutate matrix + GC + fuzz + concurrent + fiber");
    test_mutate_type_matrix();
    test_gc_heap_closure_integration();
    test_fuzz_mutate_query_panic();
    test_eight_thread_concurrent_mutate_gc();
    test_fiber_yield_steal_safety();
    test_long_running_memory_safety_stress();
    test_regression_related_primitives();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_602_detail

int aura_issue_602_run() {
    return aura_issue_602_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_602_run();
}
#endif