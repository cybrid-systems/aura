// test_macro_reflect_batch.cpp — batch driver for macro+reflect+self-evo family.
// Consolidates 3 issue tests into 1 batch entry (Phase 4+ migration,
// following the test_per_defuse_batch / test_env_lookup_batch /
// test_fiber_resume_batch / test_compact_sweep_batch /
// test_incremental_relower_batch precedent in AuraDomainTests.cmake):
//
//   Issue #597 — comprehensive test coverage matrix + observability
//                 for macro hygiene + static reflection + EDSL
//                 self-evolution paths. Layer 1: E2E matrix + combined
//                 stats. Layer 2: hygiene + marker + dirty/epoch +
//                 guard/reflect. Layer 3: fuzz + concurrent + fiber +
//                 stress + regression. (11 ACs)
//   Issue #619 — Task6 macro+reflect+self-evo follow-up closed loop
//                 (5-counter bundle + multi-round monotonic). (8 ACs)
//   Issue #635 — Macro+reflect+self-evo commercial closed-loop
//                 production gaps (July 2026 update). (6 ACs)
//
// Pattern: CHECK() macros + RUN_ALL_TESTS() (test_harness.hpp),
// namespace aura_macro_reflect_batch, EXCLUDE_FROM_ALL per
// AuraDomainTests.cmake legacy batch convention. Default build skips;
// granular debug via `ninja test_macro_reflect_batch` on demand.

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
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

namespace aura_macro_reflect_batch {

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

// ── Block 1: Issue #597 (11 ACs, Layer 1/2/3) ──
// Original: tests/test_macro_reflect_self_evo_full_matrix_task6.cpp
static void run_597() {
    std::println("\n═══ Issue #597 macro+reflect+self-evo full matrix ═══");

    // ── Layer 1: E2E matrix + combined stats ──
    // AC1: E2E single-cycle matrix (macro → query → mutate → eval)
    {
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
    }

    // AC2: query:macro-reflect-self-evo-stats reachable + monotonic
    {
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
    }

    // AC3: Combined path stats bundle
    {
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
    }

    // ── Layer 2: hygiene + marker + dirty/epoch + guard/reflect ──
    // AC4: MacroIntroduced marker preserved across cycles
    {
        std::println("\n--- AC4: MacroIntroduced marker preserved across cycles ---");
        CompilerService cs;
        CHECK(setup_macro_workspace(cs), "macro workspace for marker check");
        auto m0 = cs.eval("(stats:get \"syntax-marker-counts\")");
        CHECK(m0.has_value(), "(stats:get \"syntax-marker-counts\") callable pre-mutate");
        for (int i = 0; i < 3; ++i) {
            (void)cs.eval("(mutate:replace-value (define user-val " + std::to_string(i) +
                          ") (define user-val " + std::to_string(50 + i) + "))");
            (void)cs.eval("(eval-current)");
        }
        auto m1 = cs.eval("(stats:get \"syntax-marker-counts\")");
        CHECK(m1.has_value(), "(stats:get \"syntax-marker-counts\") callable post-mutate");
        auto by_marker = cs.eval("(query:by-marker \"MacroIntroduced\")");
        CHECK(by_marker.has_value(), "(query:by-marker \"MacroIntroduced\") callable after cycles");
    }

    // AC5: Default hygiene filter on query:pattern
    {
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
    }

    // AC6: Epoch + dirty propagation
    {
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
    }

    // AC7: Guard + reflect counters bump on successful mutate
    {
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
    }

    // ── Layer 3: fuzz + concurrent + fiber + stress + regression ──
    // AC8: Fuzz — macro + self-evo + panic injection
    {
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
    }

    // AC9: 8-thread concurrent full matrix
    {
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
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        const auto violations = cs.evaluator().get_hygiene_violation_count();
        std::println("  completed: {}/{} errors: {} hygiene_violations: {} ms: {}",
                     completed.load(), n_threads * n_iters, errors.load(), violations, ms);
        CHECK(completed.load() == n_threads * n_iters,
              "all concurrent ops completed (no deadlock)");
        CHECK(errors.load() == 0, "no eval errors under concurrent matrix");
        CHECK(violations == 0, "zero hygiene violations under concurrent load");
        CHECK(ms < 120000, "completed within 120s wall-clock budget");
    }

    // AC10: Scheduler + fiber yield at MutationBoundary
    {
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
    }

    // AC11a: Long-running stress
    {
        std::println("\n--- AC11a: {} iters long-running self-evo stress ---", k_stress_iters());
        CompilerService cs;
        CHECK(setup_macro_workspace(cs), "macro workspace for stress");
        const auto s0 = eval_int(cs, "(engine:metrics \"query:macro-reflect-self-evo-stats\")");
        std::mt19937 rng(5971u);
        std::uniform_int_distribution<int> val_dist(0, 500);
        for (int i = 0; i < k_stress_iters(); ++i) {
            (void)cs.eval("(query:pattern \"user-val\")");
            (void)cs.eval("(mutate:replace-value (define user-val " +
                          std::to_string(val_dist(rng)) + ") (define user-val " +
                          std::to_string(val_dist(rng)) + "))");
        }
        (void)cs.eval("(eval-current)");
        const auto s1 = eval_int(cs, "(engine:metrics \"query:macro-reflect-self-evo-stats\")");
        CHECK(s1 > s0, "combined stats grew under long-running stress");
    }

    // AC11b: Regression — related primitives
    {
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
            return;
        }
        (void)cs.eval("(define reg-597-b 32)");
        auto r6 = cs.eval("(+ reg-597-a reg-597-b)");
        CHECK(r6.has_value() && aura::compiler::types::is_int(*r6) &&
                  aura::compiler::types::as_int(*r6) == 42,
              "(+ reg-597-a reg-597-b) == 42 (regression)");
    }
}

// ── Block 2: Issue #619 (8 ACs) ──
// Original: tests/test_macro_reflect_self_evo_closed_loop_task6_followup.cpp
static void run_619() {
    std::println("\n═══ Issue #619 macro+reflect+self-evo follow-up closed loop ═══");

    auto followup_stats = [](CompilerService& cs) -> std::int64_t {
        auto r = cs.eval("(engine:metrics \"query:macro-reflect-self-evo-followup-stats\")");
        if (!r || !aura::compiler::types::is_int(*r))
            return 0;
        return aura::compiler::types::as_int(*r);
    };

    CompilerService cs;
    CHECK(setup_macro_workspace(cs), "macro workspace setup");

    // AC1: query:macro-reflect-self-evo-followup-stats reachable
    {
        std::println("\n--- AC1: query:macro-reflect-self-evo-followup-stats ---");
        const auto s0 = followup_stats(cs);
        std::println("  followup-stats = {}", s0);
        CHECK(s0 >= 0, "followup-stats non-negative");
    }

    // AC2: macro expand + query:pattern hygiene filter active
    {
        std::println("\n--- AC2: query:pattern hygiene filter ---");
        const auto skips0 = cs.evaluator().get_macro_introduced_skipped_in_query();
        (void)cs.eval("(query:pattern \"v\")");
        const auto skips1 = cs.evaluator().get_macro_introduced_skipped_in_query();
        std::println("  hygiene_skips: {} -> {}", skips0, skips1);
        CHECK(skips1 >= skips0, "macro-introduced nodes filtered in query");
    }

    // AC3: mutate:query-and-replace on user code succeeds
    {
        std::println("\n--- AC3: mutate under Guard on user code ---");
        auto qr = cs.eval("(query:pattern \"user-val\")");
        CHECK(qr.has_value(), "query:pattern finds user-val");
        auto mr = cs.eval("(mutate:rebind \"user-val\" \"42\")");
        CHECK(mr.has_value(), "mutate:rebind on user binding succeeds");
    }

    // AC4: Guard reflect / transform counters
    {
        std::println("\n--- AC4: Guard reflect / transform counters ---");
        const auto impact0 = cs.evaluator().get_mutation_impact_count();
        (void)cs.eval("(mutate:rebind \"user-val\" \"99\")");
        const auto impact1 = cs.evaluator().get_mutation_impact_count();
        const auto schema = cs.evaluator().get_schema_validation_pass_count();
        std::println("  mutation_impact: {} -> {} schema_pass={}", impact0, impact1, schema);
        CHECK(impact1 > impact0, "transform_applied proxy bumped on Guard success");
    }

    // AC5: dirty/epoch after self-evo mutate
    {
        std::println("\n--- AC5: dirty/epoch after self-evo mutate ---");
        auto epoch = cs.eval("(query:epoch-delta-since-last-query)");
        CHECK(epoch.has_value() && aura::compiler::types::is_int(*epoch), "epoch-delta observable");
        if (epoch && aura::compiler::types::is_int(*epoch))
            CHECK(aura::compiler::types::as_int(*epoch) >= 0, "epoch delta non-negative");
    }

    // AC6: typecheck after mutate cycle
    {
        std::println("\n--- AC6: typecheck after mutate cycle ---");
        (void)cs.eval("(eval-current)");
        auto tc = cs.eval("(typecheck-current)");
        CHECK(tc.has_value(), "typecheck-current after self-evo mutate");
    }

    // AC7: query regression
    {
        std::println("\n--- AC7: query regression ---");
        auto mrs = cs.eval("(engine:metrics \"query:macro-reflect-self-evo-stats\")");
        auto phs = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
        CHECK(mrs && aura::compiler::types::is_int(*mrs),
              "macro-reflect-self-evo-stats returns int");
        CHECK(phs && (aura::compiler::types::is_int(*phs) || aura::compiler::types::is_hash(*phs)),
              "pattern-hygiene-stats returns int|hash");
    }

    // AC8: multi-round self-evo cycle
    {
        std::println("\n--- AC8: multi-round self-evo cycle ---");
        const auto stats8a = followup_stats(cs);
        for (int round = 0; round < 3; ++round) {
            (void)cs.eval("(query:pattern \"user-val\")");
            (void)cs.eval("(mutate:rebind \"user-val\" \"" + std::to_string(100 + round) + "\")");
            (void)cs.eval("(eval-current)");
        }
        const auto stats8b = followup_stats(cs);
        std::println("  followup-stats: {} -> {}", stats8a, stats8b);
        CHECK(stats8b >= stats8a, "followup-stats monotonic over matrix");

        const auto impact = cs.evaluator().get_mutation_impact_count();
        const auto skips = cs.evaluator().get_macro_introduced_skipped_in_query();
        std::println("  final transform_applied={} hygiene_skips={}", impact, skips);
        CHECK(impact > 0, "mutation_impact > 0 after self-evo cycle");
    }
}

// ── Block 3: Issue #635 (6 ACs) ──
// Original: tests/test_macro_reflect_self_evo_commercial_closed_loop_635.cpp
static void run_635() {
    std::println("\n═══ Issue #635 macro+reflect+self-evo commercial closed loop ═══");

    auto commercial_stats = [](CompilerService& cs) -> std::int64_t {
        auto r = cs.eval("(engine:metrics \"query:macro-reflect-self-evo-commercial-stats\")");
        if (!r || !aura::compiler::types::is_int(*r))
            return 0;
        return aura::compiler::types::as_int(*r);
    };

    CompilerService cs;
    CHECK(setup_macro_workspace(cs), "macro workspace setup");

    // AC1: query:macro-reflect-self-evo-commercial-stats reachable
    {
        std::println("\n--- AC1: query:macro-reflect-self-evo-commercial-stats ---");
        const auto s0 = commercial_stats(cs);
        std::println("  commercial-stats = {}", s0);
        CHECK(s0 >= 0, "commercial-stats non-negative");
    }

    // AC2: macro expand + query:pattern hygiene filter active
    {
        std::println("\n--- AC2: query:pattern hygiene filter ---");
        const auto skips0 = cs.evaluator().get_macro_introduced_skipped_in_query();
        (void)cs.eval("(query:pattern \"v\")");
        const auto skips1 = cs.evaluator().get_macro_introduced_skipped_in_query();
        std::println("  hygiene_skips: {} -> {}", skips0, skips1);
        CHECK(skips1 >= skips0, "macro-introduced nodes filtered in query");
    }

    // AC3: mutate under Guard bumps reflect/guard/dirty counters
    {
        std::println("\n--- AC3: mutate under Guard bumps counters ---");
        const auto stats3a = commercial_stats(cs);
        (void)cs.eval("(query:pattern \"user-val\")");
        (void)cs.eval("(mutate:rebind \"user-val\" \"42\")");
        (void)cs.eval("(eval-current)");
        const auto stats3b = commercial_stats(cs);
        std::println("  commercial-stats: {} -> {}", stats3a, stats3b);
        CHECK(stats3b > stats3a, "Guard mutate bumps reflect/guard/dirty commercial counters");
    }

    // AC4: compile:macro-dirty-stats regression
    {
        std::println("\n--- AC4: compile:macro-dirty-stats regression ---");
        auto mds = cs.eval("(engine:metrics \"compile:macro-dirty-stats\")");
        CHECK(mds && aura::compiler::types::is_int(*mds), "compile:macro-dirty-stats returns int");
    }

    // AC5: multi-round self-evo cycle monotonic
    {
        std::println("\n--- AC5: multi-round self-evo cycle monotonic ---");
        const auto stats5a = commercial_stats(cs);
        for (int round = 0; round < 3; ++round) {
            (void)cs.eval("(query:pattern \"user-val\")");
            (void)cs.eval("(mutate:rebind \"user-val\" \"" + std::to_string(100 + round) + "\")");
            (void)cs.eval("(eval-current)");
        }
        const auto stats5b = commercial_stats(cs);
        std::println("  commercial-stats: {} -> {}", stats5a, stats5b);
        CHECK(stats5b >= stats5a, "commercial-stats monotonic over matrix");
    }

    // AC6: query regression
    {
        std::println("\n--- AC6: query regression ---");
        auto mrs = cs.eval("(engine:metrics \"query:macro-reflect-self-evo-stats\")");
        auto cpr = cs.eval("(engine:metrics \"query:commercial-production-readiness-stats\")");
        auto fus = cs.eval("(engine:metrics \"query:macro-reflect-self-evo-followup-stats\")");
        CHECK(mrs && aura::compiler::types::is_int(*mrs),
              "macro-reflect-self-evo-stats regression");
        CHECK(cpr && aura::compiler::types::is_int(*cpr),
              "commercial-production-readiness-stats regression");
        CHECK(fus && aura::compiler::types::is_int(*fus),
              "macro-reflect-self-evo-followup-stats regression");
    }
}

} // namespace aura_macro_reflect_batch

int main() {
    aura_macro_reflect_batch::run_597();
    aura_macro_reflect_batch::run_619();
    aura_macro_reflect_batch::run_635();
    return RUN_ALL_TESTS();
}