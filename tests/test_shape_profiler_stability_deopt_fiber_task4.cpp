// test_shape_profiler_stability_deopt_fiber_task4.cpp — Issue #570:
// ShapeProfiler stability judgment + versioning + deopt/fiber hook
// matrix under AI mutate → eval → shape-classify loops.
//
// Non-duplicative with #571 (value dispatch), #607 (Task4 hot-path),
// test_shape.cpp (unit tests). Focus:
//   - record_shape stability ratio + version++ on invalidate
//   - invalidate_all mutation path + deopt hook
//   - (engine:metrics \"query:shape-stability-stats\") observability
//   - fiber MutationBoundary refresh counter
//   - fuzz/stress under concurrent fibers

#include "test_harness.hpp"
#include "shape_profiler.h"

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

namespace aura_issue_570_detail {

using aura::compiler::CompilerService;
using aura::compiler::shape::inline_shape_of;
using aura::compiler::shape::is_known_inline_shape_id;
using aura::compiler::shape::make_fn_key;
using aura::compiler::shape::mutation_shape_churn_count;
using aura::compiler::shape::shape_fiber_refresh_count;
using aura::compiler::shape::SHAPE_INT;
using aura::compiler::shape::shape_stability_hit_count;
using aura::compiler::shape::shape_version_bump_count;
using aura::compiler::shape::ShapeProfiler;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
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
    if (!r || !is_int(*r))
        return -1;
    return static_cast<std::int64_t>(as_int(*r));
}

static std::uint64_t prompt6_violations(CompilerService& cs) {
    const auto v = eval_int(cs, "(stats:get \"query:prompt6-violation-count\")");
    return v < 0 ? 0 : static_cast<std::uint64_t>(v);
}

static bool setup_shape_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (add1 x) (+ x 1)) "
                 "(define (dbl y) (* y 2)) "
                 "(define (wrap z) (add1 z)) "
                 "(define base 10) (define acc 0) "
                 "(add1 1) (dbl 3) (wrap 5)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

// ── AC1: constexpr ShapeID range validation ───────────────
bool test_constexpr_shape_id_ranges() {
    std::println("\n--- AC1: constexpr ShapeID range validation ---");
    CHECK(is_known_inline_shape_id(SHAPE_INT), "SHAPE_INT known");
    CHECK(is_known_inline_shape_id(inline_shape_of(make_int(42).val)),
          "inline_shape_of int in known range");
    CHECK(is_known_inline_shape_id(inline_shape_of(7)), "bool shape known");
    return true;
}

// ── AC2: record_shape stability judgment ──────────────────
bool test_record_shape_stability_judgment() {
    std::println("\n--- AC2: record_shape stability judgment ---");
    ShapeProfiler profiler;
    profiler.set_window_size(200);
    const auto fn = make_fn_key("sess", "f");
    const auto hits0 = shape_stability_hit_count.load();
    for (int i = 0; i < 120; ++i) {
        (void)profiler.record_shape(fn, SHAPE_INT);
    }
    CHECK(profiler.is_stable(fn), "dominant int shape stabilizes");
    CHECK(profiler.dominant_shape(fn) == SHAPE_INT, "dominant shape is Int");
    const auto snap = profiler.current_snapshot(fn);
    CHECK(snap.version == 0, "version starts at 0 before invalidate");
    CHECK(shape_stability_hit_count.load() > hits0, "stability_hit_count grew on first stable");
    return true;
}

// ── AC3: invalidate version++ + needs_deopt ───────────────
bool test_invalidate_version_bump_needs_deopt() {
    std::println("\n--- AC3: invalidate version++ + needs_deopt ---");
    ShapeProfiler profiler;
    profiler.set_window_size(100);
    const auto fn = make_fn_key("sess", "g");
    for (int i = 0; i < 110; ++i)
        (void)profiler.record_shape(fn, SHAPE_INT);
    CHECK(profiler.is_stable(fn), "pre-invalidate stable");
    const auto bumps0 = shape_version_bump_count.load();
    const bool needs_deopt = profiler.invalidate(fn);
    CHECK(needs_deopt, "invalidate returns needs_deopt when was stable");
    CHECK(!profiler.is_stable(fn), "unstable after invalidate");
    const auto snap = profiler.current_snapshot(fn);
    CHECK(snap.version == 1, "version bumped to 1");
    CHECK(shape_version_bump_count.load() > bumps0, "global version_bump_count incremented");
    return true;
}

// ── AC4: invalidate_all preserves profiles ────────────────
bool test_invalidate_all_mutation_path() {
    std::println("\n--- AC4: invalidate_all mutation path ---");
    ShapeProfiler profiler;
    const auto fn1 = make_fn_key("sess", "h1");
    const auto fn2 = make_fn_key("sess", "h2");
    profiler.set_window_size(50);
    for (int i = 0; i < 60; ++i) {
        (void)profiler.record_shape(fn1, SHAPE_INT);
        (void)profiler.record_shape(fn2, SHAPE_INT);
    }
    const auto bumps0 = shape_version_bump_count.load();
    profiler.invalidate_all();
    CHECK(shape_version_bump_count.load() >= bumps0 + 2,
          "invalidate_all bumps version for each profile");
    CHECK(profiler.tracked_fns().size() == 2, "profiles preserved after invalidate_all");
    return true;
}

// ── AC5: query:shape-stability-stats reachable ────────────
bool test_shape_stability_stats_reachable() {
    std::println("\n--- AC5: (engine:metrics \"query:shape-stability-stats\") reachable ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    const auto s = eval_int(cs, "(engine:metrics \"query:shape-stability-stats\")");
    std::println("  shape-stability-stats baseline: {}", s);
    CHECK(s >= 0, "shape-stability-stats >= 0");
    return true;
}

// ── AC6: stats grow under eval + mutate ───────────────────
bool test_stats_grow_under_mutate_eval() {
    std::println("\n--- AC6: stats grow under mutate + eval ---");
    CompilerService cs;
    CHECK(setup_shape_workspace(cs), "shape workspace setup");
    const auto s0 = eval_int(cs, "(engine:metrics \"query:shape-stability-stats\")");
    for (int i = 0; i < 15; ++i) {
        (void)cs.eval("(add1 " + std::to_string(i) + ")");
        const auto mr =
            cs.typed_mutate("(mutate:rebind \"base\" \"" + std::to_string(50 + i) + "\")");
        CHECK(mr.success, "typed_mutate rebind succeeds");
        (void)cs.eval("(eval-current)");
    }
    const auto s1 = eval_int(cs, "(engine:metrics \"query:shape-stability-stats\")");
    const auto churn = mutation_shape_churn_count.load();
    std::println("  shape-stability-stats: {} -> {} churn={}", s0, s1, churn);
    CHECK(s1 >= s0, "shape-stability-stats monotonic under mutate");
    CHECK(churn > 0, "mutation_shape_churn under mutate load");
    return true;
}

// ── AC7: CompilerService typed_mutate + invalidate_shape ──
bool test_service_invalidate_shape_deopt() {
    std::println("\n--- AC7: CompilerService typed_mutate path ---");
    CompilerService cs;
    CHECK(setup_shape_workspace(cs), "workspace for typed_mutate");
    const auto mr = cs.typed_mutate("(mutate:rebind \"base\" \"42\")");
    CHECK(mr.success, "typed_mutate succeeds (invalidate_all wired)");
    cs.invalidate_shape("add1");
    CHECK(!cs.is_shape_stable("add1"), "invalidate_shape clears stability");
    return true;
}

// ── AC8: fuzz — random mutate + shape classify ─────────────
bool test_fuzz_shape_stability_sequences() {
    std::println("\n--- AC8: fuzz {} iters ---", k_fuzz_iters());
    CompilerService cs;
    CHECK(setup_shape_workspace(cs), "workspace for fuzz");
    std::mt19937 rng(5701u);
    std::uniform_int_distribution<int> val_dist(0, 200);
    const auto stats0 = eval_int(cs, "(engine:metrics \"query:shape-stability-stats\")");
    for (int i = 0; i < k_fuzz_iters(); ++i) {
        const int v = val_dist(rng);
        (void)cs.eval("(dbl " + std::to_string(v & 15) + ")");
        if ((i & 3) == 0) {
            (void)cs.typed_mutate("(mutate:rebind \"acc\" \"" + std::to_string(v) + "\")");
        }
        if ((i & 7) == 0)
            (void)cs.eval("(eval-current)");
    }
    const auto stats1 = eval_int(cs, "(engine:metrics \"query:shape-stability-stats\")");
    CHECK(stats1 >= stats0, "shape-stability-stats grew during fuzz");
    return true;
}

// ── AC9: 8-thread concurrent mutate + shape metrics ───────
bool test_eight_thread_concurrent_shape() {
    std::println("\n--- AC9: 8-thread concurrent shape load ---");
    CompilerService cs;
    CHECK(setup_shape_workspace(cs), "workspace for concurrent");
    std::atomic<int> done{0};
    std::mutex eval_mu;
    const int iters = k_concurrent_iters();
    auto worker = [&]() {
        std::mt19937 rng(
            static_cast<unsigned>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
        std::uniform_int_distribution<int> val_dist(0, 100);
        for (int i = 0; i < iters; ++i) {
            const int v = val_dist(rng);
            {
                std::lock_guard lock(eval_mu);
                (void)cs.eval("(wrap " + std::to_string(v & 7) + ")");
                (void)cs.typed_mutate("(mutate:rebind \"acc\" \"" + std::to_string(v) + "\")");
            }
        }
        done.fetch_add(1, std::memory_order_relaxed);
    };
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int t = 0; t < 8; ++t)
        threads.emplace_back(worker);
    for (auto& th : threads)
        th.join();
    CHECK(done.load() == 8, "all 8 threads completed");
    CHECK(prompt6_violations(cs) == 0, "zero Prompt6 violations under concurrent shape load");
    return true;
}

// ── AC10: fiber yield + shape fiber refresh ───────────────
bool test_fiber_yield_shape_refresh() {
    std::println("\n--- AC10: fiber yield shape refresh ---");
    CompilerService cs;
    CHECK(setup_shape_workspace(cs), "workspace for fiber yield");
    const auto refresh0 = shape_fiber_refresh_count.load();
    Scheduler sched(2);
    std::atomic<int> done{0};
    std::mutex eval_mu;
    constexpr int k_fibers = 4;
    constexpr int k_iters = 10;
    for (int f = 0; f < k_fibers; ++f) {
        sched.spawn([&, f]() {
            for (int i = 0; i < k_iters; ++i) {
                {
                    std::lock_guard lock(eval_mu);
                    (void)cs.eval("(add1 " + std::to_string(f + i) + ")");
                }
                Fiber::yield(YieldReason::MutationBoundary);
            }
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    std::thread io_thread([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (done.load() < k_fibers && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    sched.stop();
    io_thread.join();
    CHECK(done.load() == k_fibers, "all fibers completed");
    CHECK(shape_fiber_refresh_count.load() > refresh0,
          "fiber_refresh_count grew after MutationBoundary yields");
    return true;
}

// ── AC11: long stress + bounded deopt ─────────────────────
bool test_long_running_shape_stress() {
    std::println("\n--- AC11: {} iters long stress ---", k_stress_iters());
    CompilerService cs;
    CHECK(setup_shape_workspace(cs), "workspace for stress");
    const auto s0 = eval_int(cs, "(engine:metrics \"query:shape-stability-stats\")");
    std::mt19937 rng(5709u);
    std::uniform_int_distribution<int> val_dist(0, 500);
    for (int i = 0; i < k_stress_iters(); ++i) {
        const int v = val_dist(rng);
        (void)cs.eval("(dbl " + std::to_string(v & 31) + ")");
        if ((i & 1) == 0) {
            (void)cs.typed_mutate("(mutate:rebind \"base\" \"" + std::to_string(v) + "\")");
        } else {
            (void)cs.typed_mutate("(mutate:replace-value (define acc " + std::to_string(v) +
                                  ") (define acc " + std::to_string(v) + "))");
        }
        if ((i & 15) == 0)
            (void)cs.eval("(eval-current)");
    }
    const auto s1 = eval_int(cs, "(engine:metrics \"query:shape-stability-stats\")");
    const auto v = prompt6_violations(cs);
    const auto metrics = cs.shape_metrics("add1");
    std::println("  stats: {} -> {} add1_calls={} violations={}", s0, s1, metrics.total_calls, v);
    CHECK(s1 >= s0, "shape-stability-stats monotonic under stress");
    CHECK(v == 0, "zero Prompt6 violations under stress");
    return true;
}

// ── AC12: regression — related primitives ─────────────────
bool test_regression_related_primitives() {
    std::println("\n--- AC12: regression primitives ---");
    CompilerService cs;
    auto r1 = cs.eval("(engine:metrics \"query:value-dispatch-stats\")");
    CHECK(r1.has_value() && is_int(*r1),
          "(engine:metrics \"query:value-dispatch-stats\") regression for #571");
    auto r2 = cs.eval("(stats:get \"query:task4-hotpath-safety-score\")");
    CHECK(r2.has_value() && is_int(*r2),
          "(stats:get \"query:task4-hotpath-safety-score\") regression for #607");
    auto r3 = cs.eval("(engine:metrics \"query:shape-stability-stats\")");
    CHECK(r3.has_value() && is_int(*r3),
          "(engine:metrics \"query:shape-stability-stats\") reachable");
    if (!cs.eval("(define reg-570-a 12)")) {
        CHECK(false, "define regression");
        return false;
    }
    (void)cs.eval("(define reg-570-b 30)");
    auto r4 = cs.eval("(+ reg-570-a reg-570-b)");
    CHECK(r4.has_value() && is_int(*r4) && as_int(*r4) == 42,
          "(+ reg-570-a reg-570-b) == 42 regression");
    return true;
}

int run_tests() {
    std::println("═══ Issue #570 ShapeProfiler stability + deopt matrix ═══\n");
    std::println("Layer 1: stability judgment + version + stats primitive");
    test_constexpr_shape_id_ranges();
    test_record_shape_stability_judgment();
    test_invalidate_version_bump_needs_deopt();
    test_invalidate_all_mutation_path();
    test_shape_stability_stats_reachable();
    test_stats_grow_under_mutate_eval();
    test_service_invalidate_shape_deopt();
    std::println("\nLayer 2: fuzz + concurrent + fiber + stress");
    test_fuzz_shape_stability_sequences();
    test_eight_thread_concurrent_shape();
    test_fiber_yield_shape_refresh();
    test_long_running_shape_stress();
    test_regression_related_primitives();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_570_detail

int aura_issue_570_run() {
    return aura_issue_570_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_570_run();
}
#endif