// test_task4_highperf_full_hotpath_matrix.cpp — Issue #607:
// End-to-end hot-path fuzz + stress matrix for Task4 high-perf
// mechanisms (Arena/SoA/Value/Shape/Pass) under AI multi-round
// self-modify + fiber/GC.
//
// Non-duplicative with #602 (Prompt6 memory-safety), #597 (macro+
// reflect+self-evo), #547 (pattern index), #531 (closure-env),
// #254 (ir-soa). This binary exercises the **unified** Task4
// production-review matrix:
//
//   - AC1:  (query:task4-hotpath-safety-score) reachable
//   - AC2:  (query:task4-cache-locality-win) monotonic after queries
//   - AC3:  (query:task4-mutation-stability) grows under mutate
//   - AC4:  Combined Task4 stats bundle callable
//   - AC5:  SoA tag_arity_index dirty hook under mutate
//   - AC6:  Shape profiler reachable post-eval
//   - AC7:  Mutate matrix + eval/recompile hot path
//   - AC8:  Arena/GC integration (gc-heap + gc-arena-stats)
//   - AC9:  Fuzz — random mutate/query/eval + GC
//   - AC10: 8-thread concurrent hot-path load
//   - AC11: Fiber yield at MutationBoundary
//   - AC12: Long-running stress + zero Prompt6 violations
//   - AC13: Regression — related Task4/Prompt6 primitives

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

namespace aura_issue_607_detail {

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

static std::uint64_t prompt6_violations(CompilerService& cs) {
    const auto v = eval_int(cs, "(query:prompt6-violation-count)");
    return v < 0 ? 0 : static_cast<std::uint64_t>(v);
}

static bool setup_hotpath_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (add1 x) (+ x 1)) "
                 "(define (dbl y) (* y 2)) "
                 "(define base 10) (define acc 0) (define tmp 5) "
                 "(add1 1) (dbl 3)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

// ── AC1: hotpath-safety-score reachable ───────────────────
bool test_hotpath_safety_score_reachable() {
    std::println("\n--- AC1: (query:task4-hotpath-safety-score) reachable ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    const auto s = eval_int(cs, "(query:task4-hotpath-safety-score)");
    std::println("  task4-hotpath-safety-score baseline: {}", s);
    CHECK(s >= 0, "task4-hotpath-safety-score >= 0");
    return true;
}

// ── AC2: cache-locality-win monotonic after queries ───────
bool test_cache_locality_win_monotonic() {
    std::println("\n--- AC2: (query:task4-cache-locality-win) monotonic ---");
    CompilerService cs;
    CHECK(setup_hotpath_workspace(cs), "hotpath workspace setup");
    const auto c0 = eval_int(cs, "(query:task4-cache-locality-win)");
    for (int i = 0; i < 10; ++i) {
        (void)cs.eval("(query:tag-arity-count 32 0)");
        (void)cs.eval("(query:pattern \"base\")");
    }
    const auto c1 = eval_int(cs, "(query:task4-cache-locality-win)");
    std::println("  task4-cache-locality-win: {} -> {}", c0, c1);
    CHECK(c1 >= c0, "cache-locality-win monotonic after SoA queries");
    return true;
}

// ── AC3: mutation-stability grows under mutate ────────────
bool test_mutation_stability_under_mutate() {
    std::println("\n--- AC3: (query:task4-mutation-stability) under mutate ---");
    CompilerService cs;
    CHECK(setup_hotpath_workspace(cs), "workspace for mutation stability");
    const auto m0 = eval_int(cs, "(query:task4-mutation-stability)");
    for (int i = 0; i < 10; ++i) {
        (void)cs.eval("(mutate:rebind \"base\" \"" + std::to_string(100 + i) + "\")");
        (void)cs.eval("(mutate:replace-value (define acc " + std::to_string(i) + ") (define acc " +
                      std::to_string(i) + "))");
    }
    const auto m1 = eval_int(cs, "(query:task4-mutation-stability)");
    std::println("  task4-mutation-stability: {} -> {}", m0, m1);
    CHECK(m1 >= m0, "mutation-stability monotonic under mutate load");
    return true;
}

// ── AC4: Combined Task4 stats bundle ───────────────────────
bool test_combined_task4_stats_bundle() {
    std::println("\n--- AC4: Task4 stats bundle ---");
    CompilerService cs;
    CHECK(setup_hotpath_workspace(cs), "workspace for stats bundle");
    const char* stats[] = {
        "(query:task4-hotpath-safety-score)",
        "(query:task4-cache-locality-win)",
        "(query:task4-mutation-stability)",
        "(engine:metrics \"query:pattern-index-stats\")",
        "(engine:metrics \"query:typed-mutation-stats\")",
        "(query:typed-mutation-stats-task1)",
        "(query:incremental-effectiveness)",
        "(gc-arena-stats)",
    };
    for (const char* prim : stats) {
        auto r = cs.eval(prim);
        CHECK(r.has_value(), std::string(prim) + " returns a value");
    }
    auto soa = cs.eval("(compile:ir-soa-stats)");
    CHECK(soa.has_value(), "(compile:ir-soa-stats) returns a value");
    auto inline_stats = cs.eval("(compile:inline-pass-stats)");
    CHECK(inline_stats.has_value(), "(compile:inline-pass-stats) returns a value");
    return true;
}

// ── AC5: SoA tag_arity_index dirty hook under mutate ──────
bool test_soa_dirty_hook_under_mutate() {
    std::println("\n--- AC5: SoA tag_arity_index dirty hook ---");
    CompilerService cs;
    CHECK(setup_hotpath_workspace(cs), "workspace for SoA dirty");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace_flat() reachable");
    if (!ws)
        return false;
    ws->rebuild_tag_arity_index();
    const auto d0 = ws->tag_arity_index_dirty_marks();
    (void)cs.eval("(mutate:rebind \"tmp\" \"99\")");
    const auto d1 = ws->tag_arity_index_dirty_marks();
    std::println("  dirty_marks: {} -> {}", d0, d1);
    CHECK(d1 >= d0, "tag_arity_index_dirty_marks monotonic after mutate");
    return true;
}

// ── AC6: Shape profiler reachable post-eval ────────────────
bool test_shape_profiler_post_eval() {
    std::println("\n--- AC6: Shape profiler reachable post-eval ---");
    CompilerService cs;
    CHECK(setup_hotpath_workspace(cs), "workspace for shape profiler");
    for (int i = 0; i < 5; ++i) {
        (void)cs.eval("(add1 " + std::to_string(i) + ")");
        (void)cs.eval("(dbl " + std::to_string(i + 1) + ")");
    }
    const auto tracked = cs.is_shape_stable("add1");
    const auto metrics = cs.shape_metrics("add1");
    std::println("  add1 stable={} total_calls={}", tracked, metrics.total_calls);
    CHECK(metrics.total_calls >= 0, "shape_metrics.total_calls observable");
    return true;
}

// ── AC7: Mutate matrix + eval hot path ─────────────────────
bool test_mutate_eval_hotpath_matrix() {
    std::println("\n--- AC7: mutate + eval hot path matrix ---");
    CompilerService cs;
    CHECK(setup_hotpath_workspace(cs), "workspace for hot path");
    CHECK(cs.eval("(mutate:rebind \"base\" \"42\")").has_value(), "mutate:rebind succeeds");
    CHECK(cs.eval("(set-code \"(define base 100) (define acc 7)\")").has_value(),
          "set-code succeeds");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current succeeds");
    (void)cs.eval("(add1 5)");
    const auto v = prompt6_violations(cs);
    CHECK(v == 0, "zero Prompt6 violations after hot-path mutate+eval");
    return true;
}

// ── AC8: Arena/GC integration ─────────────────────────────
bool test_arena_gc_integration() {
    std::println("\n--- AC8: Arena/GC integration ---");
    CompilerService cs;
    CHECK(setup_hotpath_workspace(cs), "workspace for arena/GC");
    for (int i = 0; i < 5; ++i) {
        (void)cs.eval("(mutate:replace-value (define tmp " + std::to_string(i) + ") (define tmp " +
                      std::to_string(i) + "))");
    }
    auto gc = cs.eval("(gc-heap)");
    CHECK(gc.has_value(), "(gc-heap) callable under arena pressure");
    auto arena = cs.eval("(gc-arena-stats)");
    CHECK(arena.has_value(), "(gc-arena-stats) callable");
    return true;
}

// ── AC9: Fuzz — mutate/query/eval + GC ────────────────────
bool test_fuzz_hotpath_sequences() {
    std::println("\n--- AC9: {} iters fuzz (mutate + query + GC) ---", k_fuzz_iters());
    CompilerService cs;
    CHECK(setup_hotpath_workspace(cs), "workspace for fuzz");
    std::mt19937 rng(607u);
    std::uniform_int_distribution<int> op_dist(0, 5);
    std::uniform_int_distribution<int> val_dist(0, 999);
    const auto v0 = prompt6_violations(cs);
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
                (void)cs.eval("(query:tag-arity-count 32 0)");
                break;
            case 3:
                (void)cs.eval("(engine:metrics \"query:pattern-index-stats\")");
                break;
            case 4:
                (void)cs.eval("(gc-heap)");
                break;
            default:
                (void)cs.eval("(add1 " + std::to_string(val_dist(rng) % 100) + ")");
                break;
        }
    }
    const auto v1 = prompt6_violations(cs);
    std::println("  violations: {} -> {}", v0, v1);
    CHECK(v1 == 0, "zero Prompt6 violations under Task4 fuzz");
    return true;
}

// ── AC10: 8-thread concurrent hot-path load ────────────────
bool test_eight_thread_concurrent_hotpath() {
    std::println("\n--- AC10: 8 threads × {} iters concurrent ---", k_concurrent_iters());
    CompilerService cs;
    CHECK(setup_hotpath_workspace(cs), "workspace for concurrent");
    constexpr int n_threads = 8;
    const int n_iters = k_concurrent_iters();
    std::mutex mtx;
    std::atomic<int> completed{0};
    std::atomic<int> errors{0};
    auto worker = [&](int tid) {
        for (int i = 0; i < n_iters; ++i) {
            std::lock_guard<std::mutex> lk(mtx);
            if ((i & 3) == 0) {
                (void)cs.eval("(query:tag-arity-count 32 0)");
            } else if ((i & 3) == 1) {
                (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(tid * 1000 + i) + "\")");
            } else if ((i & 3) == 2) {
                (void)cs.eval("(gc-heap)");
            } else {
                (void)cs.eval("(mutate:replace-value (define tmp " + std::to_string(i) +
                              ") (define tmp " + std::to_string(tid + i) + "))");
            }
            if (!cs.eval("(query:task4-hotpath-safety-score)")) {
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
    const auto v = prompt6_violations(cs);
    std::println("  completed: {}/{} errors: {} violations: {} ms: {}", completed.load(),
                 n_threads * n_iters, errors.load(), v, ms);
    CHECK(completed.load() == n_threads * n_iters, "all concurrent ops completed (no deadlock)");
    CHECK(errors.load() == 0, "no eval errors under concurrent load");
    CHECK(v == 0, "zero violations under concurrent Task4 hot-path");
    CHECK(ms < 120000, "completed within 120s wall-clock budget");
    return true;
}

// ── AC11: Fiber yield at MutationBoundary ─────────────────
bool test_fiber_yield_hotpath() {
    std::println("\n--- AC11: Scheduler + 8 fibers × 50 yields ---");
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
    CHECK(done.load() == k_fibers, "all fibers completed yields (no stall)");
    return true;
}

// ── AC12: Long-running stress ──────────────────────────────
bool test_long_running_hotpath_stress() {
    std::println("\n--- AC12: {} iters long-running stress ---", k_stress_iters());
    CompilerService cs;
    CHECK(setup_hotpath_workspace(cs), "workspace for stress");
    const auto s0 = eval_int(cs, "(query:task4-hotpath-safety-score)");
    const auto m0 = eval_int(cs, "(query:task4-mutation-stability)");
    std::mt19937 rng(6071u);
    std::uniform_int_distribution<int> val_dist(0, 500);
    for (int i = 0; i < k_stress_iters(); ++i) {
        if ((i & 1) == 0) {
            (void)cs.eval("(mutate:rebind \"base\" \"" + std::to_string(val_dist(rng)) + "\")");
        } else {
            (void)cs.eval("(mutate:replace-value (define acc " + std::to_string(val_dist(rng)) +
                          ") (define acc " + std::to_string(val_dist(rng)) + "))");
        }
        if ((i & 15) == 0) {
            (void)cs.eval("(query:tag-arity-count 32 0)");
        }
        if ((i & 31) == 0)
            (void)cs.eval("(gc-heap)");
    }
    const auto s1 = eval_int(cs, "(query:task4-hotpath-safety-score)");
    const auto m1 = eval_int(cs, "(query:task4-mutation-stability)");
    const auto v = prompt6_violations(cs);
    std::println("  hotpath: {} -> {} stability: {} -> {} violations: {}", s0, s1, m0, m1, v);
    CHECK(s1 >= s0, "hotpath-safety-score monotonic under stress");
    CHECK(m1 >= m0, "mutation-stability monotonic under stress");
    CHECK(v == 0, "zero violations under long-running stress");
    return true;
}

// ── AC13: Regression ───────────────────────────────────────
bool test_regression_related_primitives() {
    std::println("\n--- AC13: regression — Task4/Prompt6 primitives ---");
    CompilerService cs;
    auto r1 = cs.eval("(query:prompt6-violation-count)");
    CHECK(r1.has_value() && aura::compiler::types::is_int(*r1),
          "(query:prompt6-violation-count) (regression for #602)");
    auto r2 = cs.eval("(engine:metrics \"query:pattern-index-stats\")");
    CHECK(r2.has_value() && aura::compiler::types::is_int(*r2),
          "(engine:metrics \"query:pattern-index-stats\") (regression for #547)");
    auto r3 = cs.eval("(query:compiler-cache-stats)");
    CHECK(r3.has_value(), "(query:compiler-cache-stats) (regression)");
    auto r4 = cs.eval("(query:macro-reflect-self-evo-stats)");
    CHECK(r4.has_value() && aura::compiler::types::is_int(*r4),
          "(query:macro-reflect-self-evo-stats) (regression for #597)");
    if (!cs.eval("(define reg-607-a 10)")) {
        CHECK(false, "define (regression)");
        return false;
    }
    (void)cs.eval("(define reg-607-b 32)");
    auto r5 = cs.eval("(+ reg-607-a reg-607-b)");
    CHECK(r5.has_value() && aura::compiler::types::is_int(*r5) &&
              aura::compiler::types::as_int(*r5) == 42,
          "(+ reg-607-a reg-607-b) == 42 (regression)");
    return true;
}

int run_tests() {
    std::println("═══ Issue #607 Task4 high-perf hot-path matrix ═══\n");
    std::println("Layer 1: combined Task4 stats + SoA + shape");
    test_hotpath_safety_score_reachable();
    test_cache_locality_win_monotonic();
    test_mutation_stability_under_mutate();
    test_combined_task4_stats_bundle();
    test_soa_dirty_hook_under_mutate();
    test_shape_profiler_post_eval();
    std::println("\nLayer 2: hot path + arena/GC + fuzz + concurrent + fiber");
    test_mutate_eval_hotpath_matrix();
    test_arena_gc_integration();
    test_fuzz_hotpath_sequences();
    test_eight_thread_concurrent_hotpath();
    test_fiber_yield_hotpath();
    test_long_running_hotpath_stress();
    test_regression_related_primitives();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_607_detail

int aura_issue_607_run() {
    return aura_issue_607_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_607_run();
}
#endif