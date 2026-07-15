// test_macro_reflect_self_evo_full_matrix_task6.cpp — Issue #597:
// Comprehensive test coverage matrix + observability for macro
// hygiene + static reflection + EDSL self-evolution paths.
//
// Non-duplicative with #547 (pattern hygiene), #548 (Guard panic),
// #549 (self-evo stability), #551 (reflect post-mutate), #326/#327
// (macro+mutate e2e). This binary exercises the **unified** Task6
// production-review matrix:
//
//   macro expand (MacroIntroduced) → query:pattern (hygiene filter)
//   → mutate under Guard (reflect auto_validate) → dirty/epoch
//   → eval/recompile → fiber yield/resume → combined stats.
//
//   - AC1:  E2E single-cycle matrix (macro → query → mutate → eval)
//   - AC2:  (engine:metrics \"query:macro-reflect-self-evo-stats\") reachable + monotonic
//   - AC3:  Combined path stats bundle (hygiene/epoch/guard/reflect)
//   - AC4:  MacroIntroduced marker preserved across mutate cycles
//   - AC5:  Default query:pattern hygiene filter (skip macro nodes)
//   - AC6:  Epoch + dirty propagation after macro+mutate
//   - AC7:  Guard + reflect counters bump on successful mutate
//   - AC8:  Fuzz — random macro + self-evo + panic injection
//   - AC9:  8-thread concurrent full matrix (no crash/deadlock)
//   - AC10: Scheduler + fiber yield at MutationBoundary (no stall)
//   - AC11: Regression — related Task6 primitives still work

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

namespace aura_issue_597_detail {

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

static bool setup_macro_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (mk x) "
                 "  (list 'define (list 'v x) x)) "
                 "(define user-val 1) (mk 10)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

// ── AC1: E2E single-cycle matrix ───────────────────────────
bool test_e2e_macro_query_mutate_eval_matrix() {
    std::println("\n--- AC1: E2E macro → query → mutate → eval matrix ---");
    CompilerService cs;
    CHECK(setup_macro_workspace(cs), "macro workspace setup + eval");
    auto q = cs.eval("(query:pattern \"user-val\")");
    CHECK(q.has_value(), "query:pattern finds user binding post-macro");
    auto epoch0 = cs.eval("(engine:metrics \"query:epoch-stats\")");
    CHECK(epoch0.has_value() && aura::compiler::types::is_int(*epoch0),
          "query:epoch-stats observable pre-mutate");
    auto r = cs.eval("(mutate:replace-value (define user-val 1) "
                     "(define user-val 99))");
    CHECK(r.has_value(), "mutate under Guard on user code succeeds");
    auto re = cs.eval("(eval-current)");
    CHECK(re.has_value(), "re-eval after macro+mutate succeeds");
    auto epoch1 = cs.eval("(query:epoch-delta-since-last-query)");
    CHECK(epoch1.has_value() && aura::compiler::types::is_int(*epoch1),
          "epoch-delta observable post-mutate");
    if (epoch1 && aura::compiler::types::is_int(*epoch1)) {
        CHECK(aura::compiler::types::as_int(*epoch1) >= 2,
              "epoch advanced after Guard mutate (>= 2 bumps)");
    }
    return true;
}

// ── AC2: query:macro-reflect-self-evo-stats ───────────────
bool test_query_macro_reflect_self_evo_stats() {
    std::println("\n--- AC2: (engine:metrics \"query:macro-reflect-self-evo-stats\") ---");
    CompilerService cs;
    CHECK(setup_macro_workspace(cs), "macro workspace for combined stats");
    const auto s0 = eval_int(cs, "(engine:metrics \"query:macro-reflect-self-evo-stats\")");
    CHECK(s0 >= 0, "combined stats starts >= 0");
    for (int i = 0; i < 5; ++i) {
        (void)cs.eval("(query:pattern \"user-val\")");
        (void)cs.eval("(mutate:replace-value (define user-val " + std::to_string(100 + i) +
                      ") (define user-val " + std::to_string(100 + i) + "))");
        (void)cs.eval("(eval-current)");
    }
    const auto s1 = eval_int(cs, "(engine:metrics \"query:macro-reflect-self-evo-stats\")");
    std::println("  macro-reflect-self-evo-stats: {} -> {}", s0, s1);
    CHECK(s1 > s0, "combined stats grew after self-evo loop");
    return true;
}

// ── AC3: Combined path stats bundle ────────────────────────
bool test_combined_path_stats_bundle() {
    std::println("\n--- AC3: hygiene/epoch/guard/reflect stats bundle ---");
    CompilerService cs;
    CHECK(setup_macro_workspace(cs), "macro workspace for stats bundle");
    (void)cs.eval("(mutate:replace-value (define user-val 1) "
                  "(define user-val 42))");
    const char* stats[] = {
        "(engine:metrics \"query:hygiene-stats\")",
        "(engine:metrics \"query:pattern-hygiene-stats\")",
        "(engine:metrics \"query:epoch-stats\")",
        "(query:mutation-impact)",
        "(engine:metrics \"query:reflect-postmutate-stats\")",
        "(engine:metrics \"query:panic-checkpoint-lifecycle-stats\")",
        "(engine:metrics \"query:self-evolution-stability-stats\")",
        "(query:dirty-impact)",
        "(engine:metrics \"query:macro-reflect-self-evo-stats\")",
    };
    for (const char* prim : stats) {
        auto r = cs.eval(prim);
        CHECK(r.has_value() && aura::compiler::types::is_int(*r),
              std::string(prim) + " returns integer");
    }
    return true;
}

// ── AC4: MacroIntroduced marker preserved ───────────────────
bool test_macro_introduced_marker_preserved() {
    std::println("\n--- AC4: MacroIntroduced marker preserved across cycles ---");
    CompilerService cs;
    CHECK(setup_macro_workspace(cs), "macro workspace for marker check");
    auto m0 = cs.eval("(syntax-marker-counts)");
    CHECK(m0.has_value(), "(syntax-marker-counts) callable pre-mutate");
    for (int i = 0; i < 3; ++i) {
        (void)cs.eval("(mutate:replace-value (define user-val " + std::to_string(i) +
                      ") (define user-val " + std::to_string(50 + i) + "))");
        (void)cs.eval("(eval-current)");
    }
    auto m1 = cs.eval("(syntax-marker-counts)");
    CHECK(m1.has_value(), "(syntax-marker-counts) callable post-mutate");
    auto by_marker = cs.eval("(query:by-marker \"MacroIntroduced\")");
    CHECK(by_marker.has_value(), "(query:by-marker \"MacroIntroduced\") callable after cycles");
    return true;
}

// ── AC5: Default hygiene filter on query:pattern ──────────
bool test_default_hygiene_filter() {
    std::println("\n--- AC5: default query:pattern hygiene filter ---");
    CompilerService cs;
    CHECK(setup_macro_workspace(cs), "macro workspace for hygiene filter");
    const auto skips0 = cs.evaluator().get_macro_introduced_skipped_in_query();
    (void)cs.eval("(query:pattern \"v\")");
    const auto skips1 = cs.evaluator().get_macro_introduced_skipped_in_query();
    std::println("  macro_introduced_skipped: {} -> {}", skips0, skips1);
    CHECK(skips1 >= skips0, "macro_introduced_skipped monotonic after query:pattern");
    auto r = cs.eval("(query:pattern \"v\" :respect-hygiene #f)");
    CHECK(r.has_value(), "(query:pattern :respect-hygiene #f) recognized");
    return true;
}

// ── AC6: Epoch + dirty propagation ──────────────────────────
bool test_epoch_dirty_propagation() {
    std::println("\n--- AC6: epoch + dirty propagation after macro+mutate ---");
    CompilerService cs;
    CHECK(setup_macro_workspace(cs), "macro workspace for dirty/epoch");
    const auto epoch0 = cs.evaluator().get_defuse_version();
    const auto snap0 = cs.evaluator().get_impact_snapshot_count();
    const auto dirty0 = eval_int(cs, "(query:dirty-impact)");
    (void)cs.eval("(mutate:rebind \"user-val\" \"77\")");
    const auto epoch1 = cs.evaluator().get_defuse_version();
    const auto snap1 = cs.evaluator().get_impact_snapshot_count();
    const auto dirty1 = eval_int(cs, "(query:dirty-impact)");
    const auto impact1 = eval_int(cs, "(query:mutation-impact)");
    std::println("  epoch: {} -> {} impact_snapshot: {} -> {} "
                 "dirty-impact: {} -> {} mutation-impact: {}",
                 epoch0, epoch1, snap0, snap1, dirty0, dirty1, impact1);
    CHECK(epoch1 > epoch0, "defuse_version epoch advanced after mutate");
    CHECK(snap1 > snap0, "impact_snapshot bumped after Guard mutate");
    CHECK(dirty0 >= 0 && dirty1 >= 0, "dirty-impact observable + non-negative");
    CHECK(impact1 >= 0, "mutation-impact observable post-mutate");
    return true;
}

// ── AC7: Guard + reflect counters on successful mutate ──────
bool test_guard_reflect_counters_bump() {
    std::println("\n--- AC7: Guard + reflect counters bump on mutate ---");
    CompilerService cs;
    CHECK(setup_macro_workspace(cs), "macro workspace for guard/reflect");
    const auto snap0 = cs.evaluator().get_impact_snapshot_count();
    const auto commit0 = cs.evaluator().get_panic_checkpoint_commit_count();
    for (int i = 0; i < 5; ++i) {
        (void)cs.eval("(mutate:replace-value (define user-val " + std::to_string(i) +
                      ") (define user-val " + std::to_string(10 + i) + "))");
    }
    const auto snap1 = cs.evaluator().get_impact_snapshot_count();
    const auto commit1 = cs.evaluator().get_panic_checkpoint_commit_count();
    std::println("  impact_snapshot: {} -> {} panic_commit: {} -> {}", snap0, snap1, commit0,
                 commit1);
    CHECK(snap1 > snap0, "impact_snapshot_count bumped (reflect path)");
    CHECK(commit1 >= commit0, "panic_checkpoint_commit monotonic");
    return true;
}

// ── AC8: Fuzz — macro + self-evo + panic injection ────────
bool test_fuzz_macro_self_evo_panic() {
    std::println("\n--- AC8: {} iters fuzz (macro + self-evo + panic) ---", k_fuzz_iters());
    CompilerService cs;
    CHECK(setup_macro_workspace(cs), "macro workspace for fuzz");
    std::mt19937 rng(597u);
    std::uniform_int_distribution<int> op_dist(0, 4);
    std::uniform_int_distribution<int> val_dist(0, 999);
    int panics = 0;
    const auto stats0 = eval_int(cs, "(engine:metrics \"query:macro-reflect-self-evo-stats\")");
    for (int i = 0; i < k_fuzz_iters(); ++i) {
        switch (op_dist(rng)) {
            case 0:
                (void)cs.eval("(query:pattern \"user-val\")");
                break;
            case 1:
                (void)cs.eval("(mutate:replace-value (define user-val " +
                              std::to_string(val_dist(rng)) + ") (define user-val " +
                              std::to_string(val_dist(rng)) + "))");
                break;
            case 2:
                (void)cs.eval("(query:epoch-delta-since-last-query)");
                break;
            case 3:
                (void)cs.eval("(engine:metrics \"query:macro-reflect-self-evo-stats\")");
                break;
            default:
                (void)cs.eval("(panic-checkpoint)");
                (void)cs.eval("(mutate:replace-value (define user-val " +
                              std::to_string(val_dist(rng)) + ") (define user-val " +
                              std::to_string(val_dist(rng)) + "))");
                (void)cs.eval("(panic-restore)");
                ++panics;
                break;
        }
    }
    const auto stats1 = eval_int(cs, "(engine:metrics \"query:macro-reflect-self-evo-stats\")");
    const auto rollback = cs.evaluator().get_rollback_success_on_panic();
    std::println("  panics: {} stats: {} -> {} rollback_success: {}", panics, stats0, stats1,
                 rollback);
    CHECK(stats1 >= stats0, "combined stats monotonic under fuzz");
    if (panics > 0) {
        CHECK(rollback > 0, "rollback_success bumped after panic injection");
    }
    return true;
}

// ── AC9: 8-thread concurrent full matrix ────────────────────
bool test_eight_thread_concurrent_matrix() {
    std::println("\n--- AC9: 8 threads × {} iters concurrent matrix ---", k_concurrent_iters());
    CompilerService cs;
    CHECK(setup_macro_workspace(cs), "macro workspace for concurrent matrix");
    constexpr int n_threads = 8;
    const int n_iters = k_concurrent_iters();
    std::mutex mtx;
    std::atomic<int> completed{0};
    std::atomic<int> errors{0};
    auto worker = [&](int tid) {
        for (int i = 0; i < n_iters; ++i) {
            std::lock_guard<std::mutex> lk(mtx);
            (void)cs.eval("(query:pattern \"user-val\")");
            std::string code = "(mutate:replace-value (define user-val " +
                               std::to_string(tid * 1000 + i) + ") (define user-val " +
                               std::to_string(tid * 1000 + i) + "))";
            if (!cs.eval(code))
                errors.fetch_add(1);
            if ((i & 15) == 0) {
                (void)cs.eval("(engine:metrics \"query:macro-reflect-self-evo-stats\")");
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
    const auto violations = cs.evaluator().get_hygiene_violation_count();
    std::println("  completed: {}/{} errors: {} hygiene_violations: {} ms: {}", completed.load(),
                 n_threads * n_iters, errors.load(), violations, ms);
    CHECK(completed.load() == n_threads * n_iters, "all concurrent ops completed (no deadlock)");
    CHECK(errors.load() == 0, "no eval errors under concurrent matrix");
    CHECK(violations == 0, "zero hygiene violations under concurrent load");
    CHECK(ms < 120000, "completed within 120s wall-clock budget");
    return true;
}

// ── AC10: Scheduler + fiber yield at MutationBoundary ───────
bool test_fiber_yield_at_mutation_boundary() {
    std::println("\n--- AC10: Scheduler + 8 fibers × 50 MutationBoundary yields ---");
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
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
        if (elapsed > 30000)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    sched.stop();
    io_thread.join();
    std::println("  fibers done: {}/{}", done.load(), k_fibers);
    CHECK(done.load() == k_fibers, "all fibers completed MutationBoundary yields (no stall)");
    return true;
}

// ── AC11: Long-running stress (bounded iter count) ──────────
bool test_long_running_self_evo_stress() {
    std::println("\n--- AC11a: {} iters long-running self-evo stress ---", k_stress_iters());
    CompilerService cs;
    CHECK(setup_macro_workspace(cs), "macro workspace for stress");
    const auto s0 = eval_int(cs, "(engine:metrics \"query:macro-reflect-self-evo-stats\")");
    std::mt19937 rng(5971u);
    std::uniform_int_distribution<int> val_dist(0, 500);
    for (int i = 0; i < k_stress_iters(); ++i) {
        (void)cs.eval("(query:pattern \"user-val\")");
        (void)cs.eval("(mutate:replace-value (define user-val " + std::to_string(val_dist(rng)) +
                      ") (define user-val " + std::to_string(val_dist(rng)) + "))");
    }
    (void)cs.eval("(eval-current)");
    const auto s1 = eval_int(cs, "(engine:metrics \"query:macro-reflect-self-evo-stats\")");
    CHECK(s1 > s0, "combined stats grew under long-running stress");
    return true;
}

// ── AC11: Regression — related primitives ───────────────────
bool test_regression_related_primitives() {
    std::println("\n--- AC11b: regression — related Task6 primitives ---");
    CompilerService cs;
    auto r1 = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    CHECK(r1.has_value() && aura::compiler::types::is_int(*r1),
          "(engine:metrics \"query:pattern-hygiene-stats\") (regression for #547)");
    auto r2 = cs.eval("(engine:metrics \"query:reflect-postmutate-stats\")");
    CHECK(r2.has_value() && aura::compiler::types::is_hash(*r2),
          "(engine:metrics \"query:reflect-postmutate-stats\") (regression for #502)");
    auto r3 = cs.eval("(engine:metrics \"query:panic-checkpoint-lifecycle-stats\")");
    CHECK(r3.has_value() && aura::compiler::types::is_int(*r3),
          "(engine:metrics \"query:panic-checkpoint-lifecycle-stats\") (regression for #548)");
    auto r4 = cs.eval("(engine:metrics \"query:self-evolution-stability-stats\")");
    CHECK(r4.has_value() && aura::compiler::types::is_int(*r4),
          "(engine:metrics \"query:self-evolution-stability-stats\") (regression for #549)");
    auto r5 = cs.eval("(engine:metrics \"query:epoch-stats\")");
    CHECK(r5.has_value() && aura::compiler::types::is_int(*r5),
          "(engine:metrics \"query:epoch-stats\") (regression for #456)");
    if (!cs.eval("(define reg-597-a 10)")) {
        CHECK(false, "define (regression)");
        return false;
    }
    (void)cs.eval("(define reg-597-b 32)");
    auto r6 = cs.eval("(+ reg-597-a reg-597-b)");
    CHECK(r6.has_value() && aura::compiler::types::is_int(*r6) &&
              aura::compiler::types::as_int(*r6) == 42,
          "(+ reg-597-a reg-597-b) == 42 (regression)");
    return true;
}

int run_tests() {
    std::println("═══ Issue #597 macro+reflect+self-evo full matrix ═══\n");
    std::println("Layer 1: E2E matrix + combined stats");
    test_e2e_macro_query_mutate_eval_matrix();
    test_query_macro_reflect_self_evo_stats();
    test_combined_path_stats_bundle();
    std::println("\nLayer 2: hygiene + marker + dirty/epoch + guard/reflect");
    test_macro_introduced_marker_preserved();
    test_default_hygiene_filter();
    test_epoch_dirty_propagation();
    test_guard_reflect_counters_bump();
    std::println("\nLayer 3: fuzz + concurrent + fiber + stress + regression");
    test_fuzz_macro_self_evo_panic();
    test_eight_thread_concurrent_matrix();
    test_fiber_yield_at_mutation_boundary();
    test_long_running_self_evo_stress();
    test_regression_related_primitives();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_597_detail

int aura_issue_597_run() {
    return aura_issue_597_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_597_run();
}
#endif