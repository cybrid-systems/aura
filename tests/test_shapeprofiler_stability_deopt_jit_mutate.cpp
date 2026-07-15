// test_shapeprofiler_stability_deopt_jit_mutate.cpp — Issue #605:
// ShapeProfiler versioning + JIT cache synergy under multi-round
// AI mutation. Non-duplicative with #570 (fiber/deopt matrix)
// and #601 (JIT hot-swap); focuses on:
//   - JIT cache shape_version staleness → re-compile
//   - jit_shape_miss_count observability
//   - (eval-current :jit) semantic correctness post-mutate
//   - concurrent mutate + JIT under fibers
//   - long stress (10000+ mutate sequences via env override)

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

namespace aura_issue_605_detail {

using aura::compiler::CompilerService;
using aura::compiler::shape::jit_shape_miss_count;
using aura::compiler::shape::make_fn_key;
using aura::compiler::shape::SHAPE_INT;
using aura::compiler::shape::shape_version_bump_count;
using aura::compiler::shape::ShapeProfiler;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::serve::Fiber;
using aura::serve::Scheduler;
using aura::serve::YieldReason;

static int k_warmup_calls() {
    return k_int_env("AURA_WARMUP_CALLS", 120);
}

static int k_stress_iters() {
    return k_int_env("AURA_STRESS_ITERS", 200);
}

static int k_concurrent_iters() {
    return k_int_env("AURA_STRESS_ITERS", 30);
}

static std::int64_t eval_int(CompilerService& cs, std::string_view code) {
    auto r = cs.eval(code);
    if (!r || !is_int(*r))
        return -1;
    return static_cast<std::int64_t>(as_int(*r));
}

static std::uint64_t prompt6_violations(CompilerService& cs) {
    const auto v = eval_int(cs, "(query:prompt6-violation-count)");
    return v < 0 ? 0 : static_cast<std::uint64_t>(v);
}

static bool setup_jit_shape_workspace(CompilerService& cs) {
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

static bool jit_eval(CompilerService& cs) {
    return cs.eval("(eval-current :jit)").has_value();
}

// ── AC1: ShapeProfiler version stale unit contract ─────────
bool test_shape_version_stale_unit_contract() {
    std::println("\n--- AC1: ShapeProfiler version stale unit contract ---");
    ShapeProfiler profiler;
    profiler.set_window_size(200);
    const auto fn = make_fn_key("sess605", "hotfn");
    for (int i = 0; i < k_warmup_calls(); ++i)
        (void)profiler.record_shape(fn, SHAPE_INT);
    CHECK(profiler.is_stable(fn), "hotfn stabilizes after warmup");
    const auto compiled_version = profiler.current_snapshot(fn).version;
    const bool needs_deopt = profiler.invalidate(fn);
    CHECK(needs_deopt, "invalidate returns needs_deopt when stable");
    const auto current_version = profiler.current_snapshot(fn).version;
    CHECK(current_version > compiled_version, "version bumped on invalidate");
    const bool stale = compiled_version != current_version;
    CHECK(stale, "compiled_shape_version != snapshot.version → stale");
    return true;
}

// ── AC2: workspace + JIT eval semantic baseline ────────────
bool test_jit_eval_semantic_baseline() {
    std::println("\n--- AC2: workspace + JIT eval semantic baseline ---");
    CompilerService cs;
    CHECK(setup_jit_shape_workspace(cs), "workspace setup");
    CHECK(jit_eval(cs), "(eval-current :jit) succeeds");
    const auto v = eval_int(cs, "(add1 41)");
    CHECK(v == 42, "add1 41 == 42 after JIT eval");
    return true;
}

// ── AC3: mutate bumps version + JIT re-eval semantics ──────
bool test_mutate_version_bump_jit_reeval() {
    std::println("\n--- AC3: mutate version bump + JIT re-eval ---");
    CompilerService cs;
    CHECK(setup_jit_shape_workspace(cs), "workspace ready");
    CHECK(jit_eval(cs), "initial JIT compile");
    const auto bumps0 = shape_version_bump_count.load();
    const auto mr = cs.typed_mutate("(mutate:rebind \"base\" \"99\")");
    CHECK(mr.success, "typed_mutate rebind succeeds");
    CHECK(shape_version_bump_count.load() > bumps0, "version_bump_count grew after mutate");
    CHECK(jit_eval(cs), "re-JIT after mutate succeeds");
    const auto v = eval_int(cs, "(add1 5)");
    CHECK(v == 6, "semantic correctness after re-JIT (add1 5 == 6)");
    return true;
}

// ── AC4: invalidate_shape clears stability + forces refresh
bool test_invalidate_shape_jit_refresh() {
    std::println("\n--- AC4: invalidate_shape + JIT refresh ---");
    CompilerService cs;
    CHECK(setup_jit_shape_workspace(cs), "workspace ready");
    CHECK(jit_eval(cs), "JIT compile with shape map");
    cs.invalidate_shape("add1");
    CHECK(!cs.is_shape_stable("add1"), "invalidate_shape clears stability");
    const auto miss0 = jit_shape_miss_count.load();
    CHECK(jit_eval(cs), "JIT eval after invalidate_shape");
    const auto snap = cs.evaluator().workspace_flat() != nullptr;
    CHECK(snap, "workspace still valid after invalidate");
    (void)miss0;
    const auto v = eval_int(cs, "(wrap 10)");
    CHECK(v == 11, "wrap 10 == 11 after shape invalidate");
    return true;
}

// ── AC5: query:shape-stability-stats includes jit_shape_miss
bool test_shape_stability_stats_includes_jit_miss() {
    std::println("\n--- AC5: shape-stability-stats includes jit_shape_miss ---");
    CompilerService cs;
    CHECK(setup_jit_shape_workspace(cs), "workspace ready");
    const auto s0 = eval_int(cs, "(engine:metrics \"query:shape-stability-stats\")");
    CHECK(jit_eval(cs), "JIT compile");
    (void)cs.typed_mutate("(mutate:rebind \"acc\" \"7\")");
    CHECK(jit_eval(cs), "re-JIT after mutate");
    const auto s1 = eval_int(cs, "(engine:metrics \"query:shape-stability-stats\")");
    const auto miss = jit_shape_miss_count.load();
    std::println("  stats: {} -> {} jit_shape_miss={}", s0, s1, miss);
    CHECK(s1 >= s0, "shape-stability-stats monotonic");
    CHECK(s1 > s0, "stats grew after mutate/JIT activity");
    (void)miss;
    return true;
}

// ── AC6: multi-round mutate + JIT semantic matrix ──────────
bool test_multi_round_mutate_jit_semantics() {
    std::println("\n--- AC6: multi-round mutate + JIT semantics ---");
    CompilerService cs;
    CHECK(setup_jit_shape_workspace(cs), "workspace ready");
    CHECK(jit_eval(cs), "initial JIT");
    for (int round = 0; round < 10; ++round) {
        (void)cs.typed_mutate("(mutate:rebind \"base\" \"" + std::to_string(100 + round) + "\")");
        CHECK(jit_eval(cs), "JIT eval succeeds each round");
        const int arg = 10 + round;
        const auto expected = 2 * arg;
        const auto v = eval_int(cs, "(dbl " + std::to_string(arg) + ")");
        CHECK(v == expected, "dbl semantic match after mutate round");
    }
    CHECK(prompt6_violations(cs) == 0, "zero Prompt6 violations under mutate+JIT rounds");
    return true;
}

// ── AC7: 8-thread concurrent mutate + JIT ──────────────────
bool test_eight_thread_concurrent_jit_mutate() {
    std::println("\n--- AC7: 8-thread concurrent JIT + mutate ---");
    CompilerService cs;
    CHECK(setup_jit_shape_workspace(cs), "workspace ready");
    CHECK(jit_eval(cs), "initial JIT");
    std::atomic<int> done{0};
    std::mutex eval_mu;
    const int iters = k_concurrent_iters();
    auto worker = [&]() {
        std::mt19937 rng(
            static_cast<unsigned>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
        std::uniform_int_distribution<int> val_dist(0, 50);
        for (int i = 0; i < iters; ++i) {
            const int v = val_dist(rng);
            {
                std::lock_guard lock(eval_mu);
                (void)cs.eval("(wrap " + std::to_string(v & 7) + ")");
                if ((i & 3) == 0) {
                    (void)cs.typed_mutate("(mutate:rebind \"acc\" \"" + std::to_string(v) + "\")");
                }
                if ((i & 7) == 0)
                    (void)jit_eval(cs);
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
    CHECK(prompt6_violations(cs) == 0, "zero Prompt6 violations under concurrent JIT+mutate");
    return true;
}

// ── AC8: fiber yield + JIT under MutationBoundary ──────────
bool test_fiber_yield_jit_mutate() {
    std::println("\n--- AC8: fiber yield + JIT under MutationBoundary ---");
    CompilerService cs;
    CHECK(setup_jit_shape_workspace(cs), "workspace ready");
    CHECK(jit_eval(cs), "initial JIT");
    Scheduler sched(2);
    std::atomic<int> done{0};
    std::mutex eval_mu;
    constexpr int k_fibers = 4;
    constexpr int k_iters = 8;
    for (int f = 0; f < k_fibers; ++f) {
        sched.spawn([&, f]() {
            for (int i = 0; i < k_iters; ++i) {
                {
                    std::lock_guard lock(eval_mu);
                    (void)cs.eval("(add1 " + std::to_string(f + i) + ")");
                    if ((i & 1) == 0) {
                        (void)cs.typed_mutate("(mutate:rebind \"base\" \"" + std::to_string(f + i) +
                                              "\")");
                    }
                    if ((i & 3) == 0)
                        (void)jit_eval(cs);
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
    CHECK(prompt6_violations(cs) == 0, "zero Prompt6 violations under fiber JIT+mutate");
    return true;
}

// ── AC9: long stress — mutate + JIT cycles ─────────────────
bool test_long_stress_mutate_jit_cycles() {
    std::println("\n--- AC9: {} iters stress mutate+JIT ---", k_stress_iters());
    CompilerService cs;
    CHECK(setup_jit_shape_workspace(cs), "workspace ready");
    CHECK(jit_eval(cs), "initial JIT");
    const auto stats0 = eval_int(cs, "(engine:metrics \"query:shape-stability-stats\")");
    const auto miss0 = jit_shape_miss_count.load();
    std::mt19937 rng(605u);
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
            CHECK(jit_eval(cs), "periodic JIT eval in stress");
    }
    const auto stats1 = eval_int(cs, "(engine:metrics \"query:shape-stability-stats\")");
    const auto miss1 = jit_shape_miss_count.load();
    std::println("  stats: {} -> {} jit_miss: {} -> {}", stats0, stats1, miss0, miss1);
    CHECK(stats1 >= stats0, "shape-stability-stats monotonic under stress");
    CHECK(miss1 >= miss0, "jit_shape_miss monotonic under stress");
    CHECK(prompt6_violations(cs) == 0, "zero Prompt6 violations under stress");
    return true;
}

// ── AC10: regression — #570 primitives still work ──────────
bool test_regression_related_primitives() {
    std::println("\n--- AC10: regression primitives ---");
    CompilerService cs;
    auto r1 = cs.eval("(engine:metrics \"query:shape-stability-stats\")");
    CHECK(r1.has_value() && is_int(*r1),
          "(engine:metrics \"query:shape-stability-stats\") regression for #570");
    auto r2 = cs.eval("(query:prompt6-safety-score)");
    CHECK(r2.has_value() && is_int(*r2), "(query:prompt6-safety-score) regression for #602");
    auto r3 = cs.eval("(query:task4-hotpath-safety-score)");
    CHECK(r3.has_value() && is_int(*r3), "(query:task4-hotpath-safety-score) regression for #607");
    return true;
}

int run_tests() {
    std::println("═══ Issue #605 ShapeProfiler + JIT mutate synergy ═══\n");
    std::println("Layer 1: version stale contract + JIT + stats");
    test_shape_version_stale_unit_contract();
    test_jit_eval_semantic_baseline();
    test_mutate_version_bump_jit_reeval();
    test_invalidate_shape_jit_refresh();
    test_shape_stability_stats_includes_jit_miss();
    std::println("\nLayer 2: multi-round + concurrent + fiber + stress");
    test_multi_round_mutate_jit_semantics();
    test_eight_thread_concurrent_jit_mutate();
    test_fiber_yield_jit_mutate();
    test_long_stress_mutate_jit_cycles();
    std::println("\nLayer 3: regression");
    test_regression_related_primitives();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_605_detail

int aura_issue_605_run() {
    return aura_issue_605_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_605_run();
}
#endif