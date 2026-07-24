// Issue #1622/#571/#723 (#1978 renamed): issue# moved from filename to header.
// test_value_encoding_v2_dispatch_contracts.cpp — Merged #1622 + #571 (task4) (#1978).
//
// Originally test_value_encoding_v2_dispatch_contracts_1622.cpp (6 ACs)
// + test_value_encoding_v2_dispatch_contracts_task4.cpp (12 ACs). Both
// exercise EvalValue v2 consteval dispatch + hot-path Contracts matrix.
// 1622 focuses on schema flags + mutate path; task4 adds shape_profiler +
// collision indices + fuzz/concurrent/fiber/stress. Merged with all
// 18 ACs preserved verbatim.
//
// AC list (all preserved; each section cites original issue#):
//   Issue #1622 (test_value_encoding_v2_dispatch_contracts_1622.cpp):
//     AC1: consteval classify + low2 table static checks
//     AC2: runtime classify matches consteval for common patterns
//     AC3: query:value-dispatch-stats schema 1622 AC keys
//     AC4: mutate/eval advances dispatch-hits; collisions stay 0
//     AC5: multi-round value churn stress
//     AC6: #723 lineage keys + wire flags
//   Issue #571 (test_value_encoding_v2_dispatch_contracts_task4.cpp):
//     AC1: consteval low-2-bit dispatch table
//     AC2: classify roundtrip for core tags
//     AC3: collision indices 19/31 classify as StringV2
//     AC4: query:value-dispatch-stats reachable
//     AC5: dispatch stats grow under eval churn
//     AC6: shape classify int/string/bool under mutate
//     AC7: fuzz — random eval + shape probes
//     AC8: 8-thread concurrent dispatch + mutate
//     AC9: fiber yield during mutate + classify
//     AC10: long stress + zero collisions
//     AC11: dispatch hit rate healthy after load
//     AC12: regression — related primitives

#include "test_harness.hpp"
#include "compiler/value_tags.h"
#include "compiler/shape_profiler.h"

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

namespace {

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
using aura::compiler::types::EvalValueTag;
using aura::compiler::types::is_int;
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
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:value-dispatch-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static bool seed(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (add1 x) (+ x 1)) "
                 "(define a 1) (define b 2) (add1 3)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

static bool seed_value(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (add1 x) (+ x 1)) "
                 "(define (mk n) (+ n 0)) "
                 "(define base 10) (define acc 0) "
                 "(add1 1) (mk 3)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

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

// ── #1622 AC1: consteval dispatch table ─────────────────────
static void ac1_1622_consteval() {
    std::println("\n--- #1622 AC1: consteval dispatch table ---");
    static_assert(eval_value_tag_low2_table(0) == EvalValueTag::Fixnum);
    static_assert(eval_value_tag_low2_table(1) == EvalValueTag::Ref);
    static_assert(eval_value_tag_low2_table(2) == EvalValueTag::StringV2);
    static_assert(eval_value_tag_low2_table(3) == EvalValueTag::Special);
    CHECK(true, "consteval static_asserts compiled");
}

// ── #1622 AC2: runtime classify matches consteval ─────────
static void ac2_1622_runtime() {
    std::println("\n--- #1622 AC2: runtime classify matches consteval ---");
    auto i = make_int(42);
    CHECK(classify_eval_value_tag(i.val) == EvalValueTag::Fixnum, "make_int Fixnum");
    CHECK(as_int(i) == 42, "as_int 42");
    auto s = make_string(0);
    CHECK(classify_eval_value_tag(s.val) == EvalValueTag::StringV2, "make_string StringV2");
}

// ── #1622 AC3: query schema 1622 ──────────────────────────
static void ac3_1622_schema() {
    std::println("\n--- #1622 AC3: query schema 1622 ---");
    CompilerService cs;
    CHECK(seed(cs), "seed");
    (void)cs.eval("(+ 1 2)");
    (void)cs.eval("(add1 5)");
    auto h = cs.eval("(engine:metrics \"query:value-dispatch-stats\")");
    CHECK(h && is_int(*h), "int stats");
    CHECK(href(cs, "schema") == 1622 || href(cs, "schema") == 723, "schema 1622|723");
    CHECK(href(cs, "dispatch-hits") >= 0, "dispatch-hits");
    CHECK(href(cs, "dispatch-misses") >= 0, "dispatch-misses");
    CHECK(href(cs, "consteval-table-wired") == 1, "consteval-table-wired");
    CHECK(href(cs, "hotpath-contracts-wired") == 1, "hotpath-contracts-wired");
}

// ── #1622 AC4: mutate/eval advances hits ─────────────────
static void ac4_1622_mutate() {
    std::println("\n--- #1622 AC4: mutate/eval advances hits ---");
    CompilerService cs;
    CHECK(seed(cs), "seed");
    const auto hits0 = load_u64(value_dispatch_hit_count);
    for (int i = 0; i < 30; ++i) {
        (void)cs.eval(std::format("(+ {} {})", i, i + 1));
        (void)cs.eval("(add1 1)");
    }
    (void)cs.eval("(mutate:rebind \"a\" \"10\")");
    (void)cs.eval("(eval-current)");
    CHECK(load_u64(value_dispatch_hit_count) > hits0, "hits advanced");
    CHECK(load_u64(v2_string_collision_attempts) == 0, "collisions unchanged (0 growth)");
}

// ── #1622 AC5: multi-round value churn stress ──────────────
static void ac5_1622_stress() {
    std::println("\n--- #1622 AC5: multi-round value churn ---");
    CompilerService cs;
    CHECK(seed(cs), "seed");
    const auto c0 = load_u64(v2_string_collision_attempts);
    for (int i = 0; i < 100; ++i) {
        (void)make_int(i);
        (void)classify_eval_value_tag(make_int(i * 3).val);
        (void)cs.eval(std::format("(+ {} 1)", i % 50));
    }
    CHECK(load_u64(v2_string_collision_attempts) == c0, "no collisions under churn");
}

// ── #1622 AC6: #723 lineage ─────────────────────────────────
static void ac6_1622_lineage() {
    std::println("\n--- #1622 AC6: #723 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "dispatch-calls") >= 0, "dispatch-calls");
    CHECK(href(cs, "unknown-tags") >= 0, "unknown-tags");
    CHECK(href(cs, "shape-history-shifts") >= 0, "shape-history-shifts");
    CHECK(href(cs, "consteval-table-wired") == 1, "wired");
}

// ── #571 task4 AC1: consteval low-2-bit dispatch table ──────
static void ac1_task4_consteval() {
    std::println("\n--- #571 AC1: consteval low-2-bit dispatch table ---");
    CHECK(eval_value_tag_low2_table(0) == EvalValueTag::Fixnum, "low2=0 → Fixnum");
    CHECK(eval_value_tag_low2_table(1) == EvalValueTag::Ref, "low2=1 → Ref");
    CHECK(eval_value_tag_low2_table(2) == EvalValueTag::StringV2, "low2=2 → StringV2");
    CHECK(eval_value_tag_low2_table(3) == EvalValueTag::Special, "low2=3 → Special");
}

// ── #571 task4 AC2: classify roundtrip for core tags ───────
static void ac2_task4_classify() {
    std::println("\n--- #571 AC2: classify_eval_value_tag core tags ---");
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
}

// ── #571 task4 AC3: collision indices 19/31 ────────────────
static void ac3_task4_collision() {
    std::println("\n--- #571 AC3: idx 19/31 → StringV2 (not Ref) ---");
    for (const std::uint64_t idx : {19u, 31u, 64u + 19u, 64u + 31u}) {
        const auto raw = make_string_raw_v2(idx);
        CHECK(classify_eval_value_tag(raw) == EvalValueTag::StringV2,
              "v2 string idx classified as StringV2");
        CHECK(inline_shape_of(raw) == SHAPE_STRING, "shape_profiler maps v2 string → SHAPE_STRING");
    }
    CHECK(v2_string_collision_attempts.load() == 0, "zero collision attempts for valid v2 strings");
}

// ── #571 task4 AC4: query:value-dispatch-stats reachable ───
static void ac4_task4_stats() {
    std::println("\n--- #571 AC4: (engine:metrics query:value-dispatch-stats) reachable ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    const auto s = eval_int(cs, "(engine:metrics \"query:value-dispatch-stats\")");
    CHECK(s >= 0, "value-dispatch-stats >= 0");
}

// ── #571 task4 AC5: dispatch stats grow under eval churn ──
static void ac5_task4_grow() {
    std::println("\n--- #571 AC5: dispatch stats grow under eval ---");
    CompilerService cs;
    CHECK(seed_value(cs), "value workspace setup");
    const auto s0 = eval_int(cs, "(engine:metrics \"query:value-dispatch-stats\")");
    const auto h0 = value_dispatch_hit_count.load();
    for (int i = 0; i < 20; ++i) {
        (void)cs.eval("(+ base acc)");
        (void)cs.eval("(add1 acc)");
    }
    const auto s1 = eval_int(cs, "(engine:metrics \"query:value-dispatch-stats\")");
    CHECK(s1 >= s0, "value-dispatch-stats monotonic");
    CHECK(value_dispatch_hit_count.load() > h0, "dispatch hit count grew");
}

// ── #571 task4 AC6: shape classify int/string/bool ─────────
static void ac6_task4_shape() {
    std::println("\n--- #571 AC6: shape classify under mutate ---");
    CHECK(inline_shape_of(make_int(1).val) == SHAPE_INT, "int shape");
    CHECK(inline_shape_of(make_string(3).val) == SHAPE_STRING, "string shape");
    CHECK(inline_shape_of(7) == SHAPE_BOOL, "bool shape");
    CHECK(inline_shape_of(11) == SHAPE_VOID, "void shape");
    CompilerService cs;
    CHECK(seed_value(cs), "workspace for shape classify");
    for (int i = 0; i < 10; ++i) {
        (void)cs.eval("(mutate:rebind \"base\" \"" + std::to_string(50 + i) + "\")");
        (void)cs.eval("(eval-current)");
    }
    CHECK(v2_string_collision_attempts.load() == 0, "zero collisions after mutate batch");
}

// ── #571 task4 AC7: fuzz ──────────────────────────────────
static void ac7_task4_fuzz() {
    std::println("\n--- #571 AC7: fuzz {} iters ---", k_fuzz_iters());
    CompilerService cs;
    CHECK(seed_value(cs), "workspace for fuzz");
    std::mt19937 rng(5711u);
    std::uniform_int_distribution<int> val_dist(0, 200);
    const auto hits0 = value_dispatch_hit_count.load();
    for (int i = 0; i < k_fuzz_iters(); ++i) {
        const int v = val_dist(rng);
        (void)inline_shape_of(make_int(v).val);
        (void)inline_shape_of(make_string(static_cast<std::uint64_t>(v & 127)).val);
        if ((i & 3) == 0)
            (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(v) + "\")");
        if ((i & 7) == 0)
            (void)cs.eval("(eval-current)");
    }
    CHECK(value_dispatch_hit_count.load() > hits0, "dispatch hits grew during fuzz");
    CHECK(v2_string_collision_attempts.load() == 0, "zero collisions during fuzz");
}

// ── #571 task4 AC8: 8-thread concurrent ──────────────────
static void ac8_task4_concurrent() {
    std::println("\n--- #571 AC8: 8-thread concurrent dispatch ---");
    CompilerService cs;
    CHECK(seed_value(cs), "workspace for concurrent");
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
    for (int t = 0; t < 8; ++t)
        threads.emplace_back(worker);
    for (auto& th : threads)
        th.join();
    CHECK(done.load() == 8, "all 8 threads completed");
    CHECK(v2_string_collision_attempts.load() == 0, "zero collisions under 8-thread load");
    CHECK(prompt6_violations(cs) == 0, "zero Prompt6 violations");
}

// ── #571 task4 AC9: fiber yield ────────────────────────────
static void ac9_task4_fiber() {
    std::println("\n--- #571 AC9: fiber yield + value dispatch ---");
    CompilerService cs;
    CHECK(seed_value(cs), "workspace for fiber yield");
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
    while (done.load() < k_fibers && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    sched.stop();
    io_thread.join();
    CHECK(done.load() == k_fibers, "all fibers completed");
}

// ── #571 task4 AC10: long stress + zero collisions ────────
static void ac10_task4_stress() {
    std::println("\n--- #571 AC10: {} iters long stress ---", k_stress_iters());
    CompilerService cs;
    CHECK(seed_value(cs), "workspace for stress");
    const auto s0 = eval_int(cs, "(engine:metrics \"query:value-dispatch-stats\")");
    std::mt19937 rng(5719u);
    std::uniform_int_distribution<int> val_dist(0, 500);
    for (int i = 0; i < k_stress_iters(); ++i) {
        const int v = val_dist(rng);
        (void)inline_shape_of(make_string(static_cast<std::uint64_t>(v & 255)).val);
        if ((i & 1) == 0)
            (void)cs.eval("(mutate:rebind \"base\" \"" + std::to_string(v) + "\")");
        else
            (void)cs.eval("(mutate:replace-value (define acc " + std::to_string(v) +
                          ") (define acc " + std::to_string(v) + "))");
        if ((i & 15) == 0)
            (void)cs.eval("(eval-current)");
    }
    const auto s1 = eval_int(cs, "(engine:metrics \"query:value-dispatch-stats\")");
    CHECK(s1 >= s0, "dispatch stats monotonic under stress");
    CHECK(v2_string_collision_attempts.load() == 0, "v2 collisions stay 0");
    CHECK(prompt6_violations(cs) == 0, "zero Prompt6 violations under stress");
}

// ── #571 task4 AC11: dispatch hit rate healthy ────────────
static void ac11_task4_hit_rate() {
    std::println("\n--- #571 AC11: dispatch hit rate after load ---");
    const auto hits = value_dispatch_hit_count.load();
    const auto misses = value_dispatch_miss_count.load();
    const auto total = hits + misses;
    CHECK(total > 0, "dispatch exercised");
    if (total > 0) {
        const double rate = static_cast<double>(hits) / static_cast<double>(total);
        CHECK(rate >= 0.9, "dispatch_hit_rate >= 90% after matrix");
    }
}

// ── #571 task4 AC12: regression — related primitives ──────
static void ac12_task4_regression() {
    std::println("\n--- #571 AC12: regression primitives ---");
    CompilerService cs;
    auto r1 = cs.eval("(stats:get \"query:prompt6-violation-count\")");
    CHECK(r1.has_value() && is_int(*r1), "prompt6-violation-count regression");
    auto r2 = cs.eval("(stats:get \"query:task4-hotpath-safety-score\")");
    CHECK(r2.has_value() && is_int(*r2), "task4-hotpath-safety-score regression");
    if (!cs.eval("(define reg-571-a 11)")) {
        CHECK(false, "define regression");
        return;
    }
    (void)cs.eval("(define reg-571-b 31)");
    auto r4 = cs.eval("(+ reg-571-a reg-571-b)");
    CHECK(r4.has_value() && is_int(*r4) && as_int(*r4) == 42,
          "(+ reg-571-a reg-571-b) == 42 regression");
}

} // namespace

int main() {
    std::println("=== Merged value v2 dispatch + contracts: #1622 + #571 (task4) ===");
    // #1622 (6 ACs)
    ac1_1622_consteval();
    ac2_1622_runtime();
    ac3_1622_schema();
    ac4_1622_mutate();
    ac5_1622_stress();
    ac6_1622_lineage();
    // #571 task4 (12 ACs)
    ac1_task4_consteval();
    ac2_task4_classify();
    ac3_task4_collision();
    ac4_task4_stats();
    ac5_task4_grow();
    ac6_task4_shape();
    ac7_task4_fuzz();
    ac8_task4_concurrent();
    ac9_task4_fiber();
    ac10_task4_stress();
    ac11_task4_hit_rate();
    ac12_task4_regression();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}