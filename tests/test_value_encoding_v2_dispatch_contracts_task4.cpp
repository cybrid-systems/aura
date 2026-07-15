// test_value_encoding_v2_dispatch_contracts_task4.cpp — Issue #571:
// EvalValue v2 encoding dispatch table + hot-path Contracts matrix
// under AI mutate → eval → shape-classify loops.
//
// Non-duplicative with #181 (exhaustive v2 collision prototype),
// #607 (Task4 hot-path), #602 (Prompt6 memory safety). Focus:
//   - consteval low-2-bit dispatch table correctness
//   - classify_eval_value_tag + is_/as_ contract wiring
//   - shape_profiler inline_shape_of v2 string tag path
//   - (engine:metrics \"query:value-dispatch-stats\") observability
//   - fuzz/stress under fibers with zero collision attempts

#include "test_harness.hpp"
#include "value_tags.h"
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

namespace aura_issue_571_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::shape::inline_shape_of;
using aura::compiler::shape::SHAPE_BOOL;
using aura::compiler::shape::SHAPE_FLOAT;
using aura::compiler::shape::SHAPE_INT;
using aura::compiler::shape::SHAPE_STRING;
using aura::compiler::shape::SHAPE_VOID;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::classify_eval_value_tag;
using aura::compiler::types::eval_value_tag_low2_table;
using aura::compiler::types::EvalValue;
using aura::compiler::types::EvalValueTag;
using aura::compiler::types::is_int;
using aura::compiler::types::is_ref;
using aura::compiler::types::is_string;
using aura::compiler::types::make_int;
using aura::compiler::types::make_string;
using aura::compiler::types::make_string_raw_v2;
using aura::compiler::types::v2_string_collision_attempts;
using aura::compiler::types::value_dispatch_hit_count;
using aura::compiler::types::value_dispatch_miss_count;
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

static bool setup_value_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (add1 x) (+ x 1)) "
                 "(define (mk n) (+ n 0)) "
                 "(define base 10) (define acc 0) "
                 "(add1 1) (mk 3)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

// ── AC1: consteval dispatch table ─────────────────────────
bool test_consteval_low2_dispatch_table() {
    std::println("\n--- AC1: consteval low-2-bit dispatch table ---");
    CHECK(eval_value_tag_low2_table(0) == EvalValueTag::Fixnum, "low2=0 → Fixnum");
    CHECK(eval_value_tag_low2_table(1) == EvalValueTag::Ref, "low2=1 → Ref");
    CHECK(eval_value_tag_low2_table(2) == EvalValueTag::StringV2, "low2=2 → StringV2");
    CHECK(eval_value_tag_low2_table(3) == EvalValueTag::Special, "low2=3 → Special");
    return true;
}

// ── AC2: classify roundtrip for core tags ─────────────────
bool test_classify_core_tags() {
    std::println("\n--- AC2: classify_eval_value_tag core tags ---");
    const auto iv = make_int(42);
    CHECK(classify_eval_value_tag(iv.val) == EvalValueTag::Fixnum, "make_int → Fixnum");
    CHECK(is_int(iv), "is_int agrees with classify");

    const auto sv = make_string(7);
    CHECK(classify_eval_value_tag(sv.val) == EvalValueTag::StringV2, "make_string → StringV2");
    CHECK(is_string(sv), "is_string agrees with classify");
    CHECK(as_string_idx(sv) == 7, "as_string_idx roundtrip");

    CHECK(classify_eval_value_tag(3) == EvalValueTag::Special, "#f special");
    CHECK(classify_eval_value_tag(7) == EvalValueTag::Special, "#t special");
    CHECK(classify_eval_value_tag(11) == EvalValueTag::Special, "void special");
    return true;
}

// ── AC3: collision indices 19/31 classify as StringV2 ───────
bool test_collision_indices_dispatch_string() {
    std::println("\n--- AC3: idx 19/31 → StringV2 (not Ref) ---");
    for (const std::uint64_t idx : {19u, 31u, 64u + 19u, 64u + 31u}) {
        const auto raw = make_string_raw_v2(idx);
        CHECK(classify_eval_value_tag(raw) == EvalValueTag::StringV2,
              "v2 string idx classified as StringV2");
        CHECK(!is_ref(raw), "v2 string never classified as Ref");
        CHECK(inline_shape_of(raw) == SHAPE_STRING, "shape_profiler maps v2 string → SHAPE_STRING");
    }
    CHECK(v2_string_collision_attempts.load() == 0, "zero collision attempts for valid v2 strings");
    return true;
}

// ── AC4: query:value-dispatch-stats reachable ─────────────
bool test_value_dispatch_stats_reachable() {
    std::println("\n--- AC4: (engine:metrics \"query:value-dispatch-stats\") reachable ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    const auto s = eval_int(cs, "(engine:metrics \"query:value-dispatch-stats\")");
    std::println("  value-dispatch-stats baseline: {}", s);
    CHECK(s >= 0, "value-dispatch-stats >= 0");
    return true;
}

// ── AC5: dispatch stats grow under eval churn ─────────────
bool test_dispatch_stats_grow_under_eval() {
    std::println("\n--- AC5: dispatch stats grow under eval ---");
    CompilerService cs;
    CHECK(setup_value_workspace(cs), "value workspace setup");
    const auto s0 = eval_int(cs, "(engine:metrics \"query:value-dispatch-stats\")");
    const auto h0 = value_dispatch_hit_count.load();
    for (int i = 0; i < 20; ++i) {
        (void)cs.eval("(+ base acc)");
        (void)cs.eval("(add1 acc)");
    }
    const auto s1 = eval_int(cs, "(engine:metrics \"query:value-dispatch-stats\")");
    const auto h1 = value_dispatch_hit_count.load();
    std::println("  dispatch stats: {} -> {} hits: {} -> {}", s0, s1, h0, h1);
    CHECK(s1 >= s0, "value-dispatch-stats monotonic under eval");
    CHECK(h1 > h0, "dispatch hit count grew under eval");
    return true;
}

// ── AC6: shape classify int/string/bool under mutate ──────
bool test_shape_classify_under_mutate() {
    std::println("\n--- AC6: shape classify under mutate ---");
    CompilerService cs;
    CHECK(setup_value_workspace(cs), "workspace for shape classify");
    CHECK(inline_shape_of(make_int(1).val) == SHAPE_INT, "int shape");
    CHECK(inline_shape_of(make_string(3).val) == SHAPE_STRING, "string shape");
    CHECK(inline_shape_of(7) == SHAPE_BOOL, "bool shape");
    CHECK(inline_shape_of(11) == SHAPE_VOID, "void shape");
    for (int i = 0; i < 10; ++i) {
        (void)cs.eval("(mutate:rebind \"base\" \"" + std::to_string(50 + i) + "\")");
        (void)cs.eval("(eval-current)");
    }
    CHECK(v2_string_collision_attempts.load() == 0, "zero collisions after mutate batch");
    return true;
}

// ── AC7: fuzz — random eval + shape probes ────────────────
bool test_fuzz_value_dispatch_sequences() {
    std::println("\n--- AC7: fuzz {} iters ---", k_fuzz_iters());
    CompilerService cs;
    CHECK(setup_value_workspace(cs), "workspace for fuzz");
    std::mt19937 rng(5711u);
    std::uniform_int_distribution<int> val_dist(0, 200);
    const auto hits0 = value_dispatch_hit_count.load();
    for (int i = 0; i < k_fuzz_iters(); ++i) {
        const int v = val_dist(rng);
        (void)inline_shape_of(make_int(v).val);
        (void)inline_shape_of(make_string(static_cast<std::uint64_t>(v & 127)).val);
        if ((i & 3) == 0) {
            (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(v) + "\")");
        }
        if ((i & 7) == 0)
            (void)cs.eval("(eval-current)");
    }
    const auto hits1 = value_dispatch_hit_count.load();
    CHECK(hits1 > hits0, "dispatch hits grew during fuzz");
    CHECK(v2_string_collision_attempts.load() == 0, "zero v2 collision attempts during fuzz");
    return true;
}

// ── AC8: 8-thread concurrent dispatch + mutate ─────────────
bool test_eight_thread_concurrent_dispatch() {
    std::println("\n--- AC8: 8-thread concurrent dispatch ---");
    CompilerService cs;
    CHECK(setup_value_workspace(cs), "workspace for concurrent");
    std::atomic<int> done{0};
    std::mutex eval_mu;
    const int iters = k_concurrent_iters();
    auto worker = [&]() {
        std::mt19937 rng(
            static_cast<unsigned>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
        std::uniform_int_distribution<int> val_dist(0, 100);
        for (int i = 0; i < iters; ++i) {
            const int v = val_dist(rng);
            (void)inline_shape_of(make_string(static_cast<std::uint64_t>(v)).val);
            {
                std::lock_guard lock(eval_mu);
                (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(v) + "\")");
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
    CHECK(v2_string_collision_attempts.load() == 0, "zero collisions under 8-thread load");
    CHECK(prompt6_violations(cs) == 0, "zero Prompt6 violations under concurrent dispatch");
    return true;
}

// ── AC9: fiber yield during mutate + classify ─────────────
bool test_fiber_yield_value_dispatch() {
    std::println("\n--- AC9: fiber yield + value dispatch ---");
    CompilerService cs;
    CHECK(setup_value_workspace(cs), "workspace for fiber yield");
    Scheduler sched(2);
    std::atomic<int> done{0};
    std::mutex eval_mu;
    constexpr int k_fibers = 4;
    constexpr int k_iters = 10;
    for (int f = 0; f < k_fibers; ++f) {
        sched.spawn([&, f]() {
            for (int i = 0; i < k_iters; ++i) {
                (void)inline_shape_of(make_int(f + i).val);
                {
                    std::lock_guard lock(eval_mu);
                    (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(f * 10 + i) + "\")");
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
    return true;
}

// ── AC10: long stress + zero collisions ───────────────────
bool test_long_running_dispatch_stress() {
    std::println("\n--- AC10: {} iters long stress ---", k_stress_iters());
    CompilerService cs;
    CHECK(setup_value_workspace(cs), "workspace for stress");
    const auto s0 = eval_int(cs, "(engine:metrics \"query:value-dispatch-stats\")");
    std::mt19937 rng(5719u);
    std::uniform_int_distribution<int> val_dist(0, 500);
    for (int i = 0; i < k_stress_iters(); ++i) {
        const int v = val_dist(rng);
        (void)inline_shape_of(make_string(static_cast<std::uint64_t>(v & 255)).val);
        if ((i & 1) == 0) {
            (void)cs.eval("(mutate:rebind \"base\" \"" + std::to_string(v) + "\")");
        } else {
            (void)cs.eval("(mutate:replace-value (define acc " + std::to_string(v) +
                          ") (define acc " + std::to_string(v) + "))");
        }
        if ((i & 15) == 0)
            (void)cs.eval("(eval-current)");
    }
    const auto s1 = eval_int(cs, "(engine:metrics \"query:value-dispatch-stats\")");
    const auto collisions = v2_string_collision_attempts.load();
    const auto misses = value_dispatch_miss_count.load();
    const auto v = prompt6_violations(cs);
    std::println("  dispatch: {} -> {} collisions={} misses={} violations={}", s0, s1, collisions,
                 misses, v);
    CHECK(s1 >= s0, "dispatch stats monotonic under stress");
    CHECK(collisions == 0, "v2_string_collision_attempts stays 0");
    CHECK(v == 0, "zero Prompt6 violations under stress");
    return true;
}

// ── AC11: dispatch hit rate healthy after load ─────────────
bool test_dispatch_hit_rate_healthy() {
    std::println("\n--- AC11: dispatch hit rate after load ---");
    const auto hits = value_dispatch_hit_count.load();
    const auto misses = value_dispatch_miss_count.load();
    const auto total = hits + misses;
    std::println("  hits={} misses={}", hits, misses);
    CHECK(total > 0, "dispatch exercised");
    if (total > 0) {
        const double rate = static_cast<double>(hits) / static_cast<double>(total);
        CHECK(rate >= 0.9, "dispatch_hit_rate >= 90% after matrix");
    }
    return true;
}

// ── AC12: regression — related primitives ─────────────────
bool test_regression_related_primitives() {
    std::println("\n--- AC12: regression primitives ---");
    CompilerService cs;
    auto r1 = cs.eval("(stats:get \"query:prompt6-violation-count\")");
    CHECK(r1.has_value() && is_int(*r1),
          "(stats:get \"query:prompt6-violation-count\") regression");
    auto r2 = cs.eval("(stats:get \"query:task4-hotpath-safety-score\")");
    CHECK(r2.has_value() && is_int(*r2),
          "(stats:get \"query:task4-hotpath-safety-score\") regression");
    auto r3 = cs.eval("(engine:metrics \"query:macro-reflect-self-evo-stats\")");
    CHECK(r3.has_value() && is_int(*r3),
          "(engine:metrics \"query:macro-reflect-self-evo-stats\") regression");
    if (!cs.eval("(define reg-571-a 11)")) {
        CHECK(false, "define regression");
        return false;
    }
    (void)cs.eval("(define reg-571-b 31)");
    auto r4 = cs.eval("(+ reg-571-a reg-571-b)");
    CHECK(r4.has_value() && is_int(*r4) && as_int(*r4) == 42,
          "(+ reg-571-a reg-571-b) == 42 regression");
    return true;
}

int run_tests() {
    std::println("═══ Issue #571 Value v2 dispatch + contracts matrix ═══\n");
    std::println("Layer 1: dispatch table + classify + shape + stats");
    test_consteval_low2_dispatch_table();
    test_classify_core_tags();
    test_collision_indices_dispatch_string();
    test_value_dispatch_stats_reachable();
    test_dispatch_stats_grow_under_eval();
    test_shape_classify_under_mutate();
    std::println("\nLayer 2: fuzz + concurrent + fiber + stress");
    test_fuzz_value_dispatch_sequences();
    test_eight_thread_concurrent_dispatch();
    test_fiber_yield_value_dispatch();
    test_long_running_dispatch_stress();
    test_dispatch_hit_rate_healthy();
    test_regression_related_primitives();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_571_detail

int aura_issue_571_run() {
    return aura_issue_571_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_571_run();
}
#endif