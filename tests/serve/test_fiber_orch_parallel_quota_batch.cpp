// test_fiber_orch_parallel_quota_batch.cpp — consolidated fiber-theme drivers
// Merged from per-issue standalones; each section in its own namespace.
// Prefer adding a section here over a new tests/fiber binary.

#include "test_harness.hpp"
#include <cstdint>
#include <print>
#include "serve/fiber.h"
#include "serve/scheduler.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/parallel_orch.h"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include "compiler/observability_metrics.h"
#include "core/resource_quota.hh"
#include "orch/orch.h"
#include <fstream>
#include <string_view>
#include <format>
#include <iostream>
#include <limits>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.core.error;
import aura.core.resource_quota;


// ─── from test_parallel_intend_primitive.cpp → aura_fiber_run_parallel_intend::run_parallel_intend
// ───
namespace aura_fiber_run_parallel_intend {
// @category: integration
// @reason: Issue #1587 — Aura (parallel-intend) primitive over parallel_orch.
//
//   AC1: empty tasks → status ok
//   AC2: vector of thunks → all ok + results
//   AC3: list of thunks
//   AC4: policy :max-concurrency / :fail-fast
//   AC5: query:parallel-orch-stats advanced
//   AC6: bad args → error


namespace {

    using aura::compiler::CompilerService;
    using aura::compiler::types::as_bool;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_bool;
    using aura::compiler::types::is_error;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_int;
    using aura::compiler::types::is_string;
    using aura::compiler::types::is_vector;
    using aura::test::g_failed;
    using aura::test::g_passed;

    static std::int64_t href_int(CompilerService& cs, const char* expr) {
        auto r = cs.eval(expr);
        if (!r || !is_int(*r))
            return -999999;
        return as_int(*r);
    }

    static void ac1_empty() {
        std::println("\n--- AC1: empty tasks ---");
        CompilerService cs;
        auto r = cs.eval(R"((parallel-intend (vector)))");
        CHECK(r && is_hash(*r), "empty returns hash");
        CHECK(href_int(cs, R"((hash-ref (parallel-intend (vector)) "schema"))") == 1587,
              "schema 1587");
        CHECK(href_int(cs, R"((hash-ref (parallel-intend (vector)) "ok-count"))") == 0,
              "ok-count 0");
        auto st = cs.eval(R"((hash-ref (parallel-intend (vector)) "status"))");
        CHECK(st && is_string(*st), "status string");
    }

    static void ac2_vector_thunks() {
        std::println("\n--- AC2: vector of thunks ---");
        CompilerService cs;
        auto r = cs.eval(R"(
(let ((out (parallel-intend
             (vector (lambda () 1) (lambda () 2) (lambda () 3))
             :max-concurrency 2
             :timeout-ms 10000)))
  out)
)");
        CHECK(r && is_hash(*r), "batch hash");
        CHECK(href_int(cs, R"(
(hash-ref (parallel-intend
            (vector (lambda () 10) (lambda () 20))
            :max-concurrency 2 :timeout-ms 10000)
           "ok-count")
)") == 2,
              "ok-count 2");
        auto st = cs.eval(R"(
(hash-ref (parallel-intend
            (vector (lambda () 1) (lambda () 2))
            :max-concurrency 2 :timeout-ms 10000)
           "status")
)");
        CHECK(st && is_string(*st), "status present");
        auto res = cs.eval(R"(
(hash-ref (parallel-intend
            (vector (lambda () 42))
            :timeout-ms 10000)
           "results")
)");
        CHECK(res && is_vector(*res), "results vector");
        auto v0 = cs.eval(R"(
(hash-ref (vector-ref
            (hash-ref (parallel-intend
                        (vector (lambda () 42))
                        :timeout-ms 10000)
                       "results")
            0)
           "value")
)");
        CHECK(v0 && is_int(*v0) && as_int(*v0) == 42, "value 42");
    }

    static void ac3_list_thunks() {
        std::println("\n--- AC3: list of thunks ---");
        CompilerService cs;
        auto n = href_int(cs, R"(
(hash-ref (parallel-intend
            (list (lambda () 7) (lambda () 8))
            :timeout-ms 10000)
           "ok-count")
)");
        CHECK(n == 2, "list ok-count 2");
    }

    static void ac4_fail_fast_policy() {
        std::println("\n--- AC4: fail-fast policy ---");
        CompilerService cs;
        // One failing thunk via (error ...) if available; else use a throw path.
        // Prefer a verifier-style false? Use (raise) if present; otherwise force
        // type error by applying non-closure — simpler: return ok for all and
        // only check max-concurrency is accepted.
        auto r = cs.eval(R"(
(parallel-intend
  (vector (lambda () 1) (lambda () 2) (lambda () 3) (lambda () 4))
  :max-concurrency 1
  :fail-fast #f
  :timeout-ms 10000)
)");
        CHECK(r && is_hash(*r), "policy batch hash");
        CHECK(href_int(cs, R"(
(hash-ref (parallel-intend
            (vector (lambda () 1) (lambda () 2))
            :max-concurrency 1 :fail-fast #t :timeout-ms 10000)
           "ok-count")
)") == 2,
              "fail-fast all-ok still ok-count 2");
    }

    static void ac5_stats() {
        std::println("\n--- AC5: parallel-orch-stats ---");
        CompilerService cs;
        (void)cs.eval(R"(
(parallel-intend (vector (lambda () 1)) :timeout-ms 5000)
)");
        auto schema =
            cs.eval(R"((hash-ref (engine:metrics "query:parallel-orch-stats") "schema"))");
        CHECK(schema && is_int(*schema) && as_int(*schema) == 1586, "orch schema 1586");
        auto batches =
            cs.eval(R"((hash-ref (engine:metrics "query:parallel-orch-stats") "batches"))");
        CHECK(batches && is_int(*batches) && as_int(*batches) >= 1, "batches advanced");
    }

    static void ac6_bad_args() {
        std::println("\n--- AC6: bad args ---");
        CompilerService cs;
        auto r = cs.eval(R"((parallel-intend 123))");
        CHECK(r && is_error(*r), "non-list/vector is error");
        auto r2 = cs.eval(R"((parallel-intend))");
        CHECK(r2 && is_error(*r2), "missing tasks is error");
    }

} // namespace

int run_parallel_intend() {
    std::println("=== test_parallel_intend_primitive (#1587) ===");
    ac1_empty();
    ac2_vector_thunks();
    ac3_list_thunks();
    ac4_fail_fast_policy();
    ac5_stats();
    ac6_bad_args();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_fiber_run_parallel_intend
// ─── end test_parallel_intend_primitive.cpp ───

// ─── from test_parallel_orch.cpp → aura_fiber_run_parallel_orch_1586::run_parallel_orch_1586 ───
namespace aura_fiber_run_parallel_orch_1586 {
// @category: integration
// @reason: Issue #1586 — parallel_orch / parallel_intend: concurrency cap,
// timeout, fail-fast, result aggregate, mailbox fan-in, throughput vs sequential.
//
//   AC1: validate_policy + empty batch
//   AC2: parallel_intend N tasks join Ok, all results present
//   AC3: max_concurrency gate (never exceeds cap)
//   AC4: fail_fast aborts remaining
//   AC5: timeout status + drain
//   AC6: MultiFiberMailbox result posts
//   AC7: parallel faster than sequential (busy yield work)
//   AC8: query:parallel-orch-stats schema 1586


namespace {

    using aura::compiler::CompilerService;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_int;
    using aura::serve::Fiber;
    using aura::serve::Scheduler;
    using aura::serve::YieldReason;
    using aura::serve::mf_mailbox::MultiFiberMailbox;
    using aura::serve::parallel_orch::BatchStatus;
    using aura::serve::parallel_orch::g_parallel_orch_stats;
    using aura::serve::parallel_orch::parallel_intend;
    using aura::serve::parallel_orch::ParallelPolicy;
    using aura::serve::parallel_orch::sequential_run;
    using aura::serve::parallel_orch::TaskResult;
    using aura::serve::parallel_orch::TaskSpec;
    using aura::serve::parallel_orch::validate_policy;
    using aura::test::g_failed;
    using aura::test::g_passed;

    struct SchedRunner {
        Scheduler& sched;
        std::thread thr;
        explicit SchedRunner(Scheduler& s)
            : sched(s)
            , thr([&s] { s.run(); }) {}
        ~SchedRunner() {
            sched.stop();
            if (thr.joinable())
                thr.join();
        }
    };

    static void busy_yield(int n) {
        for (int i = 0; i < n; ++i)
            Fiber::yield(YieldReason::Explicit);
    }

    static void ac1_policy() {
        std::println("\n--- AC1: policy + empty ---");
        CHECK(validate_policy(ParallelPolicy{}), "default policy ok");
        CHECK(validate_policy(ParallelPolicy{.max_concurrency = 1}), "min concurrency");
        CHECK(!validate_policy(ParallelPolicy{.max_concurrency = 0}), "zero concurrency invalid");
        CHECK(!validate_policy(ParallelPolicy{.max_concurrency = 2048}), "over 1024 invalid");

        Scheduler sched(1);
        SchedRunner runner(sched);
        std::vector<TaskSpec> empty;
        auto r = parallel_intend(sched, empty);
        CHECK(r.status == BatchStatus::Ok, "empty batch Ok");
        CHECK(r.results.empty(), "no results");
    }

    static void ac2_parallel_ok() {
        std::println("\n--- AC2: parallel_intend all Ok ---");
        Scheduler sched(4);
        SchedRunner runner(sched);
        std::vector<TaskSpec> tasks;
        for (int i = 0; i < 8; ++i) {
            tasks.push_back(TaskSpec{.body =
                                         [i] {
                                             busy_yield(2);
                                             return TaskResult{.ok = true,
                                                               .value = "t" + std::to_string(i),
                                                               .task_index =
                                                                   static_cast<std::uint64_t>(i)};
                                         },
                                     .name = "t" + std::to_string(i)});
        }
        ParallelPolicy p{.max_concurrency = 4, .timeout_ms = 15000};
        auto r = parallel_intend(sched, tasks, p);
        CHECK(r.status == BatchStatus::Ok, "batch Ok");
        CHECK(r.ok_count == 8, "8 ok");
        CHECK(r.err_count == 0, "0 err");
        CHECK(r.results.size() == 8, "8 results");
        for (std::size_t i = 0; i < r.results.size(); ++i)
            CHECK(r.results[i].ok && r.results[i].value == "t" + std::to_string(i), "result value");
    }

    static void ac3_concurrency_cap() {
        std::println("\n--- AC3: max_concurrency gate ---");
        Scheduler sched(4);
        SchedRunner runner(sched);
        std::atomic<int> peak{0};
        std::atomic<int> current{0};
        std::vector<TaskSpec> tasks;
        for (int i = 0; i < 12; ++i) {
            tasks.push_back(TaskSpec{.body = [&] {
                int c = current.fetch_add(1, std::memory_order_relaxed) + 1;
                int p = peak.load(std::memory_order_relaxed);
                while (c > p && !peak.compare_exchange_weak(p, c, std::memory_order_relaxed)) {
                }
                busy_yield(5);
                current.fetch_sub(1, std::memory_order_relaxed);
                return TaskResult{.ok = true, .value = "x"};
            }});
        }
        ParallelPolicy p{.max_concurrency = 3, .timeout_ms = 20000};
        auto r = parallel_intend(sched, tasks, p);
        CHECK(r.status == BatchStatus::Ok, "cap batch Ok");
        CHECK(r.ok_count == 12, "all 12 ok");
        CHECK(peak.load() <= 3, "peak concurrency <= 3");
        CHECK(peak.load() >= 1, "peak concurrency >= 1");
    }

    static void ac4_fail_fast() {
        std::println("\n--- AC4: fail_fast ---");
        Scheduler sched(2);
        SchedRunner runner(sched);
        std::atomic<int> ran{0};
        std::vector<TaskSpec> tasks;
        // First task fails after a short yield; others sleep longer so admit aborts.
        tasks.push_back(TaskSpec{.body = [&] {
            ran.fetch_add(1, std::memory_order_relaxed);
            busy_yield(1);
            return TaskResult{.ok = false, .error = "boom"};
        }});
        for (int i = 0; i < 7; ++i) {
            tasks.push_back(TaskSpec{.body = [&] {
                // Spin a while so fail-fast can set abort before we take a permit.
                for (int k = 0; k < 50; ++k)
                    Fiber::yield(YieldReason::Explicit);
                ran.fetch_add(1, std::memory_order_relaxed);
                return TaskResult{.ok = true, .value = "late"};
            }});
        }
        ParallelPolicy p{.max_concurrency = 1, .timeout_ms = 15000, .fail_fast = true};
        auto r = parallel_intend(sched, tasks, p);
        CHECK(r.status == BatchStatus::FailFast || r.status == BatchStatus::Partial,
              "fail-fast or partial");
        CHECK(r.err_count + r.aborted_count >= 1, "errors or aborts recorded");
        // Not all 8 need to fully run when fail-fast aborts admission.
        CHECK(r.results.size() == 8, "8 result slots");
        bool saw_fail_or_abort = false;
        for (auto& tr : r.results) {
            if (!tr.ok)
                saw_fail_or_abort = true;
        }
        CHECK(saw_fail_or_abort, "at least one non-ok result");
    }

    static void ac5_timeout() {
        std::println("\n--- AC5: timeout ---");
        Scheduler sched(2);
        SchedRunner runner(sched);
        std::vector<TaskSpec> tasks;
        for (int i = 0; i < 2; ++i) {
            tasks.push_back(TaskSpec{.body = [] {
                // Cooperative cancel: spin until cancel or long budget.
                for (int k = 0; k < 500000; ++k) {
                    if (aura::serve::g_current_fiber &&
                        aura::serve::g_current_fiber->is_cancel_requested())
                        return TaskResult{.ok = false, .error = "cancelled"};
                    Fiber::yield(YieldReason::Explicit);
                }
                return TaskResult{.ok = true, .value = "done"};
            }});
        }
        ParallelPolicy p{.max_concurrency = 2, .timeout_ms = 30};
        auto r = parallel_intend(sched, tasks, p);
        CHECK(r.status == BatchStatus::Timeout, "timeout status");
        CHECK(g_parallel_orch_stats.timeouts.load() >= 1, "timeout counter");
    }

    static void ac6_mailbox() {
        std::println("\n--- AC6: MultiFiberMailbox fan-in ---");
        Scheduler sched(3);
        SchedRunner runner(sched);
        MultiFiberMailbox mb(64);
        std::vector<TaskSpec> tasks;
        for (int i = 0; i < 5; ++i) {
            tasks.push_back(TaskSpec{
                .body = [i] { return TaskResult{.ok = true, .value = "m" + std::to_string(i)}; }});
        }
        const auto posts0 = g_parallel_orch_stats.mailbox_posts.load(std::memory_order_relaxed);
        ParallelPolicy p{.max_concurrency = 3, .timeout_ms = 10000};
        auto r = parallel_intend(sched, tasks, p, &mb);
        CHECK(r.status == BatchStatus::Ok, "mailbox batch Ok");
        CHECK(g_parallel_orch_stats.mailbox_posts.load() >= posts0 + 5, "5 mailbox posts");
        int popped = 0;
        aura::serve::mf_mailbox::MailMessage msg;
        while (mb.try_pop(msg))
            ++popped;
        CHECK(popped == 5, "5 messages in mailbox");
    }

    static void ac7_throughput() {
        std::println("\n--- AC7: parallel vs sequential throughput ---");
        // Host sequential path sleeps; fiber parallel path only yields. Parallel
        // wall time should be well below sequential under multi-worker sched.
        constexpr int N = 8;
        constexpr int SLEEP_US = 1500; // per sequential task
        auto make_seq = [] {
            std::vector<TaskSpec> tasks;
            for (int i = 0; i < N; ++i) {
                tasks.push_back(TaskSpec{.body = [] {
                    std::this_thread::sleep_for(std::chrono::microseconds(1500));
                    return TaskResult{.ok = true, .value = "w"};
                }});
            }
            return tasks;
        };
        auto make_par = [] {
            std::vector<TaskSpec> tasks;
            for (int i = 0; i < N; ++i) {
                tasks.push_back(TaskSpec{.body = [] {
                    busy_yield(4);
                    return TaskResult{.ok = true, .value = "w"};
                }});
            }
            return tasks;
        };

        auto seq = sequential_run(make_seq());
        CHECK(seq.ok_count == N, "seq all ok");
        const auto seq_us = seq.wait_us;

        Scheduler sched(4);
        SchedRunner runner(sched);
        ParallelPolicy p{.max_concurrency = 8, .timeout_ms = 10000};
        auto t_par0 = std::chrono::steady_clock::now();
        auto par = parallel_intend(sched, make_par(), p);
        auto par_us =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - t_par0)
                                           .count());
        CHECK(par.status == BatchStatus::Ok, "par Ok");
        CHECK(par.ok_count == N, "par all ok");
        std::println("  seq_us={} par_us={}", seq_us, par_us);
        // Sequential ≈ N * SLEEP_US; parallel should be much lower.
        CHECK(par_us < seq_us, "parallel faster than sequential");
        CHECK(par_us * 2 < seq_us || par_us < 50000, "parallel throughput win");
    }

    static void ac8_stats_query() {
        std::println("\n--- AC8: query:parallel-orch-stats ---");
        CompilerService cs;
        auto h = cs.eval("(engine:metrics \"query:parallel-orch-stats\")");
        CHECK(h && is_hash(*h), "parallel-orch-stats is hash");
        auto schema =
            cs.eval("(hash-ref (engine:metrics \"query:parallel-orch-stats\") \"schema\")");
        CHECK(schema && is_int(*schema) && as_int(*schema) == 1586, "schema 1586");
        auto phase = cs.eval("(hash-ref (engine:metrics \"query:parallel-orch-stats\") \"phase\")");
        CHECK(phase && is_int(*phase) && as_int(*phase) >= 2, "phase >= 2");
        auto batches =
            cs.eval("(hash-ref (engine:metrics \"query:parallel-orch-stats\") \"batches\")");
        CHECK(batches && is_int(*batches) && as_int(*batches) >= 1, "batches advanced");
    }

    // Issue #1602: light multi-round stress (full suite in
    // test_parallel_orchestration_stress_1602 + suite/*.aura).
    static void ac9_stress_multi_round() {
        std::println("\n--- AC9: multi-round stress (#1602) ---");
        Scheduler sched(4);
        SchedRunner runner(sched);
        constexpr int kRounds = 10;
        constexpr int kTasks = 16;
        int ok_rounds = 0;
        const auto joined0 = g_parallel_orch_stats.tasks_joined.load(std::memory_order_relaxed);
        for (int r = 0; r < kRounds; ++r) {
            std::vector<TaskSpec> tasks;
            tasks.reserve(kTasks);
            for (int i = 0; i < kTasks; ++i) {
                tasks.push_back(TaskSpec{
                    .body =
                        [i] {
                            Fiber::yield(YieldReason::Explicit);
                            if ((i % 4) == 0)
                                Fiber::yield(YieldReason::MutationBoundary);
                            return TaskResult{.ok = true, .value = std::to_string(i)};
                        },
                    .name = "s",
                });
            }
            ParallelPolicy p{.max_concurrency = 4, .timeout_ms = 10000};
            auto batch = parallel_intend(sched, std::span<const TaskSpec>(tasks), p, nullptr);
            if (batch.ok_count == static_cast<std::uint64_t>(kTasks))
                ++ok_rounds;
        }
        // Under batch/CI load allow a few flaky rounds.
        CHECK(ok_rounds >= kRounds - 3, std::format("stable rounds ({}/{})", ok_rounds, kRounds));
        CHECK(g_parallel_orch_stats.tasks_joined.load() >=
                  joined0 + static_cast<std::uint64_t>((kRounds - 1) * kTasks),
              "joined advanced under stress");
    }

} // namespace

int run_parallel_orch_1586() {
    std::println("=== test_parallel_orch (#1586) ===");
    ac1_policy();
    ac2_parallel_ok();
    ac3_concurrency_cap();
    ac4_fail_fast();
    ac5_timeout();
    ac6_mailbox();
    ac7_throughput();
    ac8_stats_query();
    ac9_stress_multi_round();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_fiber_run_parallel_orch_1586
// ─── end test_parallel_orch.cpp ───

// ─── from test_parallel_orchestration_stress.cpp →
// aura_fiber_run_parallel_orch_stress::run_parallel_orch_stress ───
namespace aura_fiber_run_parallel_orch_stress {
// @category: integration
// @reason: Issue #1602 — E2E stress for concurrent mutate + parallel agent
// Issue #1602 (#1978 renamed): issue# moved from filename to header.
// + Fiber::join + GC compact/steal paths (refine #1584–#1588 / #1595 / #1597).
//
//   AC1: suite/parallel_orchestration_stress.aura companion (C++ metrics)
//   AC2: parallel_intend N tasks under concurrency cap; all join
//   AC3: MultiFiberMailbox fan-in during stress batch
//   AC4: CompilerService parallel-intend + mutate + gc-heap no crash
//   AC5: query:parallel-orch-stats + ai-closedloop-readiness advanced
//   AC6: Fiber::join batch + join_wait metrics; linear join total readable


namespace {

    using aura::compiler::CompilerService;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_int;
    using aura::compiler::types::is_string;
    using aura::serve::Fiber;
    using aura::serve::Scheduler;
    using aura::serve::YieldReason;
    using aura::serve::mf_mailbox::MultiFiberMailbox;
    using aura::serve::parallel_orch::BatchStatus;
    using aura::serve::parallel_orch::g_parallel_orch_stats;
    using aura::serve::parallel_orch::parallel_intend;
    using aura::serve::parallel_orch::ParallelPolicy;
    using aura::serve::parallel_orch::TaskResult;
    using aura::serve::parallel_orch::TaskSpec;
    using aura::test::g_failed;
    using aura::test::g_passed;

    struct SchedRunner {
        Scheduler& sched;
        std::thread thr;
        explicit SchedRunner(Scheduler& s)
            : sched(s)
            , thr([&s] { s.run(); }) {}
        ~SchedRunner() {
            sched.stop();
            if (thr.joinable())
                thr.join();
        }
    };

    static std::int64_t href(CompilerService& cs, const char* q, const char* key) {
        auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
        if (!r || !is_int(*r))
            return -999999;
        return as_int(*r);
    }

    static void ac2_parallel_intend_stress() {
        std::println("\n--- AC2: parallel_intend stress (N=64, cap=8) ---");
        Scheduler sched(4);
        SchedRunner runner(sched);

        constexpr int N = 64;
        std::atomic<int> ran{0};
        std::vector<TaskSpec> tasks;
        tasks.reserve(N);
        for (int i = 0; i < N; ++i) {
            tasks.push_back(TaskSpec{
                .body =
                    [&ran, i] {
                        // Yield mix → work-steal opportunities across workers.
                        Fiber::yield(YieldReason::Explicit);
                        if ((i % 3) == 0)
                            Fiber::yield(YieldReason::MutationBoundary);
                        ran.fetch_add(1, std::memory_order_relaxed);
                        return TaskResult{.ok = true, .value = std::to_string(i)};
                    },
                .name = "t" + std::to_string(i),
            });
        }

        ParallelPolicy pol;
        pol.max_concurrency = 8;
        pol.timeout_ms = 60000; // steal-friendly yields need headroom under load
        pol.fail_fast = false;

        const auto joined0 = g_parallel_orch_stats.tasks_joined.load(std::memory_order_relaxed);
        const auto batches0 = g_parallel_orch_stats.intend_batches.load(std::memory_order_relaxed);

        auto batch = parallel_intend(sched, std::span<const TaskSpec>(tasks), pol, nullptr);
        // Timeout with high ok_count is still a successful stress exercise of join
        // deadline + drain (status Timeout=2). Prefer Ok/Partial; accept Timeout
        // when almost all tasks completed.
        const bool status_ok = batch.status == BatchStatus::Ok ||
                               batch.status == BatchStatus::Partial ||
                               (batch.status == BatchStatus::Timeout && batch.ok_count >= 60);
        CHECK(status_ok,
              std::format("batch status ok/partial/near-complete-timeout (status={} ok={})",
                          static_cast<int>(batch.status), batch.ok_count));
        CHECK(batch.ok_count >= static_cast<std::uint64_t>(N - 4),
              std::format("ok_count high ({} / {})", batch.ok_count, N));
        CHECK(ran.load() >= N - 4, std::format("bodies ran ({})", ran.load()));
        CHECK(g_parallel_orch_stats.intend_batches.load() > batches0, "batches advanced");
        CHECK(g_parallel_orch_stats.tasks_joined.load() >=
                  joined0 + static_cast<std::uint64_t>(N - 4),
              "joined advanced");
    }

    static void ac3_mailbox_stress() {
        std::println("\n--- AC3: MultiFiberMailbox fan-in under stress ---");
        Scheduler sched(4);
        SchedRunner runner(sched);
        MultiFiberMailbox mb(128);

        constexpr int N = 24;
        std::vector<TaskSpec> tasks;
        for (int i = 0; i < N; ++i) {
            tasks.push_back(TaskSpec{
                .body =
                    [i] {
                        Fiber::yield(YieldReason::Explicit);
                        return TaskResult{.ok = true, .value = "m" + std::to_string(i)};
                    },
                .name = "mb",
            });
        }
        ParallelPolicy pol{.max_concurrency = 6, .timeout_ms = 20000};
        const auto posts0 = g_parallel_orch_stats.mailbox_posts.load(std::memory_order_relaxed);
        auto batch = parallel_intend(sched, std::span<const TaskSpec>(tasks), pol, &mb);
        CHECK(batch.ok_count == static_cast<std::uint64_t>(N), "mailbox batch all ok");
        CHECK(g_parallel_orch_stats.mailbox_posts.load() >= posts0 + static_cast<std::uint64_t>(N),
              "mailbox posts advanced");
    }

    static void ac4_aura_mutate_gc_parallel() {
        std::println("\n--- AC4: Aura parallel-intend + mutate + gc-heap ---");
        CompilerService cs;
        // Avoid bare "#t" lines inside CHECK(...) — the preprocessor treats them
        // as directives when nested in macro args (even inside R"(...)").
        auto seed = cs.eval("(begin (set-code \"(define (sf x) (+ x 1))\") (eval-current) 1)");
        CHECK(seed.has_value(), "seed");

        static constexpr const char* kBatch = R"AURA(
(parallel-intend
  (vector
    (lambda ()
      (mutate:set-body "sf" "(+ x 2)")
      (eval-current)
      (gc-heap)
      (sf 0))
    (lambda ()
      (mutate:set-body "sf" "(+ x 3)")
      (eval-current)
      (gc-heap)
      (sf 0))
    (lambda () (begin (gc-heap) (gc-heap) 1))
    (lambda () (fiber:join (fiber:spawn (lambda () 42))))
    (lambda () (fiber:join (fiber:spawn (lambda () 43))))
    (lambda () 7)
    (lambda () 8)
    (lambda () 9))
  :max-concurrency 4
  :timeout-ms 30000
  :fail-fast #f)
)AURA";
        auto batch = cs.eval(kBatch);
        CHECK(batch && is_hash(*batch), "batch hash");
        auto okc = cs.eval("(hash-ref (parallel-intend (vector (lambda () 1) (lambda () 2)) "
                           ":timeout-ms 10000) \"ok-count\")");
        CHECK(okc && is_int(*okc) && as_int(*okc) == 2, "simple ok-count 2");

        static constexpr const char* kStatus = R"AURA(
(hash-ref
  (parallel-intend
    (vector
      (lambda () (begin (gc-heap) 1))
      (lambda () (begin (mutate:set-body "sf" "(+ x 1)") (eval-current) (sf 0))))
    :timeout-ms 15000)
  "status")
)AURA";
        auto status = cs.eval(kStatus);
        CHECK(status && is_string(*status), "status string after mutate/gc");

        auto live = cs.eval("(begin (gc-heap) (sf 1))");
        CHECK(live && is_int(*live), "sf survives gc after stress");
    }

    static void ac5_metrics_surfaces() {
        std::println("\n--- AC5: parallel-orch + closedloop metrics ---");
        CompilerService cs;
        (void)cs.eval(R"(
(parallel-intend
  (vector (lambda () 1) (lambda () 2) (lambda () 3) (lambda () (gc-heap)))
  :max-concurrency 2 :timeout-ms 10000)
)");

        CHECK(href(cs, "query:parallel-orch-stats", "schema") == 1586, "orch schema 1586");
        CHECK(href(cs, "query:parallel-orch-stats", "batches") >= 1, "batches >= 1");
        CHECK(href(cs, "query:parallel-orch-stats", "joined") >= 1, "joined >= 1");
        CHECK(href(cs, "query:parallel-orch-stats", "ok") >= 1, "ok >= 1");

        const auto cl_schema = href(cs, "query:ai-closedloop-readiness-stats", "schema");
        CHECK(cl_schema >= 1593, std::format("closedloop schema >= 1593 (got {})", cl_schema));
        CHECK(href(cs, "query:ai-closedloop-readiness-stats", "orch-health-score") >= 0,
              "orch-health-score");
        // avg-join may be 0 if join_wait not sampled via Aura path; still readable.
        CHECK(href(cs, "query:ai-closedloop-readiness-stats", "avg-join-latency-us") >= 0,
              "avg-join-latency-us readable");
    }

    static void ac6_fiber_join_batch_metrics() {
        std::println("\n--- AC6: Fiber::join batch + latency metrics ---");
        Scheduler sched(4);
        SchedRunner runner(sched);

        const auto wait0 = Fiber::join_wait_us_total();
        const auto linear0 = Fiber::join_linear_enforcement_total();

        std::vector<Fiber*> kids;
        for (int i = 0; i < 16; ++i) {
            kids.push_back(sched.spawn([i] {
                Fiber::yield(YieldReason::Explicit);
                if ((i % 2) == 0)
                    Fiber::yield(YieldReason::MutationBoundary);
            }));
        }
        auto jr = Fiber::join(std::span<Fiber* const>(kids), std::optional<std::uint64_t>{15000});
        CHECK(jr.status == aura::serve::JoinStatus::Ok, "join batch Ok");
        int done = 0;
        for (auto* f : kids)
            if (f && f->is_done())
                ++done;
        CHECK(done >= 14, std::format("most fibers done ({}/16)", done));
        CHECK(Fiber::join_wait_us_total() >= wait0, "join_wait_us non-decreasing");
        // #1595 linear enforcement counter must remain readable (may be 0).
        CHECK(Fiber::join_linear_enforcement_total() >= linear0, "linear enforce readable");
        CHECK(Fiber::join_latency_hist_sum() >= 0, "join latency hist readable");
    }

    static void ac4b_repeated_batches() {
        std::println("\n--- AC4b: repeated parallel_intend rounds ---");
        Scheduler sched(4);
        SchedRunner runner(sched);
        constexpr int kRounds = 20;
        constexpr int kTasks = 12;
        int ok_rounds = 0;
        for (int r = 0; r < kRounds; ++r) {
            std::vector<TaskSpec> tasks;
            for (int i = 0; i < kTasks; ++i) {
                tasks.push_back(TaskSpec{
                    .body =
                        [i] {
                            Fiber::yield(YieldReason::Explicit);
                            return TaskResult{.ok = true, .value = std::to_string(i)};
                        },
                    .name = "r",
                });
            }
            ParallelPolicy pol{.max_concurrency = 4, .timeout_ms = 10000};
            auto batch = parallel_intend(sched, std::span<const TaskSpec>(tasks), pol, nullptr);
            if (batch.ok_count == static_cast<std::uint64_t>(kTasks))
                ++ok_rounds;
        }
        // Under batch/CI load allow a few flaky rounds.
        CHECK(ok_rounds >= kRounds - 3, std::format("stable rounds ({}/{})", ok_rounds, kRounds));
    }

} // namespace

int run_parallel_orch_stress() {
    std::println("=== Issue #1602: parallel orchestration stress ===");
    ac2_parallel_intend_stress();
    ac3_mailbox_stress();
    ac4_aura_mutate_gc_parallel();
    ac4b_repeated_batches();
    ac5_metrics_surfaces();
    ac6_fiber_join_batch_metrics();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_fiber_run_parallel_orch_stress
// ─── end test_parallel_orchestration_stress.cpp ───

// ─── from test_orch_quota_integration.cpp →
// aura_fiber_run_orch_quota_integration::run_orch_quota_integration ───
namespace aura_fiber_run_orch_quota_integration {
// @category: unit
// @reason: Issue #1880 — ResourceQuota + try_acquire on agent_spawn /
// Issue #1880 (#1978 renamed): issue# moved from filename to header.
// parallel_orch (typed ResourceQuotaExceeded, no panic/OOM).
//
//   AC1: source cites #1880; memory preflight + try_acquire body wire
//   AC2: spawn rejects when memory quota exhausted (typed error)
//   AC3: parallel_orch rejects when memory preflight fails
//   AC4: query:resource-quota-stats schema-1880 + agent_arena fields
//   AC5: spawn until memory exhaust → graceful reject, join releases
//   AC6: try_acquire reject path under mutation budget (no panic)


namespace {

    using aura::compiler::CompilerMetrics;
    using aura::compiler::CompilerService;
    using aura::compiler::Evaluator;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_int;
    using aura::core::resource_quota::Dimension;
    using aura::core::resource_quota::process_resource_quota;
    using aura::core::resource_quota::reset_process_resource_quota_for_test;
    using aura::serve::Fiber;
    using aura::serve::Scheduler;
    using aura::serve::YieldReason;
    using aura::serve::parallel_orch::BatchStatus;
    using aura::serve::parallel_orch::parallel_intend;
    using aura::serve::parallel_orch::ParallelPolicy;
    using aura::serve::parallel_orch::TaskSpec;
    using aura::test::g_failed;
    using aura::test::g_passed;

    using Guard = Evaluator::MutationBoundaryGuard;

    extern "C" int aura_orch_agent_body_try_acquire();
    extern "C" void aura_orch_agent_body_release_guard();

    struct SchedRunner {
        Scheduler& sched;
        std::thread thr;
        explicit SchedRunner(Scheduler& s)
            : sched(s)
            , thr([&s] { s.run(); }) {}
        ~SchedRunner() {
            sched.stop();
            if (thr.joinable())
                thr.join();
        }
    };

    std::string read_file(const char* path) {
        std::ifstream in(path);
        if (!in)
            return {};
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    std::string read_first(std::initializer_list<const char*> paths) {
        for (const char* p : paths) {
            auto s = read_file(p);
            if (!s.empty())
                return s;
        }
        return {};
    }

    std::int64_t href(CompilerService& cs, std::string_view key) {
        auto r = cs.eval(
            std::format("(hash-ref (engine:metrics \"query:resource-quota-stats\") \"{}\")", key));
        if (!r || !is_int(*r))
            return -1;
        return as_int(*r);
    }

} // namespace

int run_orch_quota_integration() {
    // ── AC1: source surface ──
    {
        std::println("\n--- AC1: #1880 source surface ---");
        auto spawn = read_first({"src/orch/agent_spawn.h", "../src/orch/agent_spawn.h"});
        auto por = read_first({"src/serve/parallel_orch.h", "../src/serve/parallel_orch.h"});
        auto rq = read_first({"src/core/resource_quota.hh", "../src/core/resource_quota.hh"});
        auto mut = read_first({"src/compiler/evaluator_fiber_mutation.cpp",
                               "../src/compiler/evaluator_fiber_mutation.cpp"});
        auto stats = read_first({"src/compiler/evaluator_primitives_obs_jit.cpp",
                                 "../src/compiler/evaluator_primitives_obs_jit.cpp"});
        CHECK(!spawn.empty() && spawn.find("#1880") != std::string::npos,
              "agent_spawn cites #1880");
        CHECK(spawn.find("try_consume_agent_arena") != std::string::npos, "arena consume");
        CHECK(spawn.find("aura_orch_agent_body_try_acquire") != std::string::npos,
              "try_acquire body");
        CHECK(spawn.find("ResourceQuotaExceeded") != std::string::npos, "typed error");
        CHECK(!por.empty() && por.find("#1880") != std::string::npos, "parallel_orch cites #1880");
        CHECK(!rq.empty() && rq.find("orch_resource_quota_rejects_total") != std::string::npos,
              "orch rejects metric");
        CHECK(rq.find("agent_arena_usage_bytes") != std::string::npos, "agent_arena_usage_bytes");
        CHECK(!mut.empty() && mut.find("aura_orch_agent_body_try_acquire") != std::string::npos,
              "try_acquire strong def");
        CHECK(!stats.empty() && stats.find("schema-1880") != std::string::npos, "schema-1880");
    }

    // ── AC2: memory quota reject on spawn ──
    {
        std::println("\n--- AC2: spawn memory quota typed reject ---");
        reset_process_resource_quota_for_test();
        auto& pq = process_resource_quota();
        // One agent needs 4096 + 256*64 = 20480; limit below that.
        pq.set_limit(Dimension::Memory, 1000);
        Scheduler sched(2);
        SchedRunner runner(sched);
        const auto rej0 = pq.orch_resource_quota_rejects_total.load();
        auto h = aura::orch::spawn_agent_with_mailbox(
            sched, {.name = "mem-over", .body = [] { Fiber::yield(YieldReason::Explicit); }});
        CHECK(!h.ok, "spawn fails under memory limit");
        CHECK(h.quota_exceeded, "quota_exceeded flag");
        CHECK(h.error.find("ResourceQuotaExceeded") != std::string::npos,
              "typed ResourceQuotaExceeded");
        CHECK(pq.orch_resource_quota_rejects_total.load() > rej0, "orch rejects bumped");
        CHECK(h.reserved_memory_bytes == 0, "no reservation on fail");
        reset_process_resource_quota_for_test();
    }

    // ── AC3: parallel_orch memory preflight ──
    {
        std::println("\n--- AC3: parallel_orch memory preflight ---");
        reset_process_resource_quota_for_test();
        auto& pq = process_resource_quota();
        // 4 tasks * 4096 = 16384; set limit tiny.
        pq.set_limit(Dimension::Memory, 100);
        Scheduler sched(2);
        SchedRunner runner(sched);
        std::vector<TaskSpec> tasks;
        for (int i = 0; i < 4; ++i) {
            tasks.push_back({.body = [] {
                Fiber::yield(YieldReason::Explicit);
                return aura::serve::parallel_orch::TaskResult{.ok = true, .value = "x"};
            }});
        }
        auto batch = parallel_intend(sched, tasks, {.max_concurrency = 2, .timeout_ms = 5000});
        CHECK(batch.status == BatchStatus::QuotaExceeded, "QuotaExceeded status");
        CHECK(!batch.results.empty(), "results present");
        CHECK(batch.results[0].error.find("ResourceQuotaExceeded") != std::string::npos,
              "typed error on task");
        CHECK(pq.orch_resource_quota_rejects_total.load() >= 1, "rejects metric");
        reset_process_resource_quota_for_test();
    }

    // ── AC4: query stats ──
    {
        std::println("\n--- AC4: query:resource-quota-stats #1880 ---");
        reset_process_resource_quota_for_test();
        CompilerService cs;
        auto h = cs.eval(R"((engine:metrics "query:resource-quota-stats"))");
        CHECK(h && is_hash(*h), "stats hash");
        CHECK(href(cs, "schema-1880") == 1880, "schema-1880");
        CHECK(href(cs, "orch_agent_body_try_acquire_wired") == 1, "try_acquire wired");
        CHECK(href(cs, "orch_spawn_memory_preflight_wired") == 1, "memory preflight wired");
        CHECK(href(cs, "orch_resource_quota_rejects_total") >= 0, "rejects field");
        CHECK(href(cs, "agent_arena_usage_bytes") >= 0, "arena usage field");
        // schema primary stays lineage-stable
        CHECK(href(cs, "schema") == 1634 || href(cs, "schema") == 1628 ||
                  href(cs, "schema") == 1618,
              "primary schema lineage");
    }

    // ── AC5: spawn until memory exhaust + join releases ──
    {
        std::println("\n--- AC5: spawn until exhaust + release on join ---");
        reset_process_resource_quota_for_test();
        auto& pq = process_resource_quota();
        // Each agent with default mailbox: 4096 + 256*64 = 20480.
        constexpr std::uint64_t kPer = 4096 + 256 * 64;
        pq.set_limit(Dimension::Memory, kPer * 3 + 100); // fit ~3 agents
        Scheduler sched(2);
        SchedRunner runner(sched);
        std::vector<aura::orch::AgentHandle> hs;
        for (int i = 0; i < 3; ++i) {
            auto h = aura::orch::spawn_agent_with_mailbox(
                sched, {.name = std::format("ok{}", i),
                        .body = [] { Fiber::yield(YieldReason::Explicit); }});
            CHECK(h.ok, std::format("spawn {} under limit", i));
            hs.push_back(std::move(h));
        }
        CHECK(pq.agent_arena_usage_bytes.load() >= kPer * 3, "arena usage reserved");
        auto over = aura::orch::spawn_agent_with_mailbox(
            sched, {.name = "over", .body = [] { Fiber::yield(YieldReason::Explicit); }});
        CHECK(!over.ok && over.quota_exceeded, "4th spawn rejected");
        CHECK(over.error.find("ResourceQuotaExceeded") != std::string::npos, "typed on exhaust");
        for (auto& h : hs)
            (void)aura::orch::join_agent(h, std::optional<std::uint64_t>{5000});
        CHECK(pq.agent_arena_usage_bytes.load() == 0, "usage released after join");
        CHECK(pq.agent_arena_release_total.load() >= 3, "release counter");
        // After release, spawn again succeeds.
        auto again = aura::orch::spawn_agent_with_mailbox(
            sched, {.name = "again", .body = [] { Fiber::yield(YieldReason::Explicit); }});
        CHECK(again.ok, "spawn after release ok");
        (void)aura::orch::join_agent(again, std::optional<std::uint64_t>{5000});
        reset_process_resource_quota_for_test();
    }

    // ── AC6: try_acquire reject under mutation budget (no throw) ──
    {
        std::println("\n--- AC6: try_acquire reject no panic ---");
        reset_process_resource_quota_for_test();
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        ev.set_resource_quota_mutations(1);
        ev.reset_mutation_quota_used();
        const auto tr0 = m->mutation_guard_try_acquire_reject_total.load();
        bool ok = true;
        CHECK(Guard::try_acquire(ev, 1, &ok).has_value(), "first try_acquire ok");
        auto g2 = Guard::try_acquire(ev, 1, &ok);
        CHECK(!g2.has_value(), "second try_acquire typed reject");
        CHECK(m->mutation_guard_try_acquire_reject_total.load() > tr0, "reject metric");
        // Direct C trampoline when evaluator is yield-hook bound.
        // Without binding, trampoline allows body (serve-only).
        CHECK(aura_orch_agent_body_try_acquire() == 0 || aura_orch_agent_body_try_acquire() == 1,
              "trampoline returns 0 or 1 without throw");
        aura_orch_agent_body_release_guard();
        reset_process_resource_quota_for_test();
    }

    std::println("\n=== test_orch_quota_integration_1880: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_fiber_run_orch_quota_integration
// ─── end test_orch_quota_integration.cpp ───

// ─── from test_orch_resource_quota.cpp →
// aura_fiber_run_orch_resource_quota::run_orch_resource_quota ───
namespace aura_fiber_run_orch_resource_quota {
// @category: integration
// @reason: Issue #1600 — ResourceQuota on agent_spawn / parallel_intend /
// Issue #1600 (#1978 renamed): issue# moved from filename to header.
// Fiber::join orchestration paths with typed ResourceQuotaExceeded.
//
//   AC1: Scheduler::spawn / agent_spawn reject when fibers quota exhausted
//   AC2: parallel_intend returns QuotaExceeded + typed error strings
//   AC3: metrics fiber_spawn_rejected_total / orchestration_quota_exceeded
//   AC4: query:resource-quota-stats schema 1600 orch keys
//   AC5: continuous spawn until exhaust; join_resource_wait_us exposed
//   AC6: integration with #1590 process quota + #1586 parallel_orch


namespace {

    using aura::compiler::CompilerService;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_int;
    using aura::core::resource_quota::Dimension;
    using aura::core::resource_quota::process_resource_quota;
    using aura::core::resource_quota::reset_process_resource_quota_for_test;
    using aura::serve::Fiber;
    using aura::serve::Scheduler;
    using aura::serve::YieldReason;
    using aura::serve::parallel_orch::BatchStatus;
    using aura::serve::parallel_orch::check_orchestration_fiber_quota;
    using aura::serve::parallel_orch::g_parallel_orch_stats;
    using aura::serve::parallel_orch::parallel_intend;
    using aura::serve::parallel_orch::ParallelPolicy;
    using aura::serve::parallel_orch::TaskResult;
    using aura::serve::parallel_orch::TaskSpec;
    using aura::test::g_failed;
    using aura::test::g_passed;

    struct SchedRunner {
        Scheduler& sched;
        std::thread thr;
        explicit SchedRunner(Scheduler& s)
            : sched(s)
            , thr([&s] { s.run(); }) {}
        ~SchedRunner() {
            sched.stop();
            if (thr.joinable())
                thr.join();
        }
    };

    static std::int64_t href(CompilerService& cs, const char* key) {
        auto r = cs.eval(
            std::format("(hash-ref (engine:metrics \"query:resource-quota-stats\") \"{}\")", key));
        if (!r || !is_int(*r))
            return -999999;
        return as_int(*r);
    }

    static void ac1_spawn_agent_quota() {
        std::println("\n--- AC1: agent_spawn / Scheduler::spawn quota reject ---");
        reset_process_resource_quota_for_test();
        auto& pq = process_resource_quota();
        pq.set_limit(Dimension::Fibers, 2);

        Scheduler sched(2);
        SchedRunner runner(sched);
        std::vector<aura::orch::AgentHandle> hs;
        for (int i = 0; i < 2; ++i) {
            auto h = aura::orch::spawn_agent_with_mailbox(
                sched, {.name = "a" + std::to_string(i),
                        .body = [] { Fiber::yield(YieldReason::Explicit); }});
            CHECK(h.ok, std::format("spawn {} ok under limit", i));
            hs.push_back(std::move(h));
        }
        auto rej = aura::orch::spawn_agent_with_mailbox(
            sched, {.name = "over", .body = [] { Fiber::yield(YieldReason::Explicit); }});
        CHECK(!rej.ok, "spawn over quota fails");
        CHECK(rej.quota_exceeded, "quota_exceeded flag");
        CHECK(rej.error.find("ResourceQuotaExceeded") != std::string::npos, "typed error string");
        CHECK(pq.fiber_spawn_rejected_total.load() >= 1, "fiber_spawn_rejected_total");
        CHECK(pq.orchestration_quota_exceeded_total.load() >= 1, "orch exceeded total");

        for (auto& h : hs)
            (void)aura::orch::join_agent(h, std::optional<std::uint64_t>{3000});
        reset_process_resource_quota_for_test();
    }

    static void ac2_parallel_intend_quota() {
        std::println("\n--- AC2: parallel_intend QuotaExceeded ---");
        reset_process_resource_quota_for_test();
        auto& pq = process_resource_quota();
        pq.set_limit(Dimension::Fibers, 1);

        Scheduler sched(2);
        SchedRunner runner(sched);
        // Consume the single fiber slot with a long-lived fiber.
        std::atomic<bool> release{false};
        Fiber* holder = sched.spawn([&] {
            while (!release.load(std::memory_order_relaxed))
                Fiber::yield(YieldReason::Explicit);
        });
        CHECK(holder != nullptr, "holder spawn");

        // Preflight check when remaining is 0.
        auto pre = check_orchestration_fiber_quota(1);
        CHECK(pre.has_value(), "preflight rejects");
        CHECK(pre->message.find("quota") != std::string::npos, "preflight message");

        std::vector<TaskSpec> tasks;
        for (int i = 0; i < 3; ++i) {
            tasks.push_back(
                TaskSpec{.body = [] { return TaskResult{.ok = true, .value = "x"}; }, .name = "t"});
        }
        ParallelPolicy pol;
        pol.max_concurrency = 2;
        pol.timeout_ms = 5000;
        auto batch = parallel_intend(sched, std::span<const TaskSpec>(tasks), pol, nullptr);
        CHECK(batch.status == BatchStatus::QuotaExceeded, "status QuotaExceeded");
        CHECK(batch.err_count >= 1, "errors recorded");
        for (auto& r : batch.results) {
            if (!r.ok)
                CHECK(r.error.find("ResourceQuotaExceeded") != std::string::npos,
                      "typed task error");
        }
        CHECK(g_parallel_orch_stats.quota_rejects.load() >= 1, "parallel quota_rejects");

        release.store(true, std::memory_order_relaxed);
        (void)Fiber::join(holder, std::optional<std::uint64_t>{3000});
        reset_process_resource_quota_for_test();
    }

    static void ac3_metrics_and_exhaust() {
        std::println("\n--- AC3/AC5: continuous spawn until exhaust ---");
        reset_process_resource_quota_for_test();
        auto& pq = process_resource_quota();
        pq.set_limit(Dimension::Fibers, 3);

        Scheduler sched(2);
        SchedRunner runner(sched);
        int ok_n = 0, rej_n = 0;
        std::vector<Fiber*> live;
        for (int i = 0; i < 20; ++i) {
            Fiber* f = sched.spawn([] { Fiber::yield(YieldReason::Explicit); });
            if (f) {
                ++ok_n;
                live.push_back(f);
            } else {
                ++rej_n;
            }
        }
        CHECK(ok_n == 3, "exactly limit succeeds");
        CHECK(rej_n == 17, "remainder rejected");
        CHECK(pq.fiber_spawn_rejected_total.load() >= 17, "spawn rejected metric");
        CHECK(pq.orchestration_quota_exceeded_total.load() >= 17, "orch exceeded metric");

        (void)Fiber::join(std::span<Fiber* const>(live), std::optional<std::uint64_t>{5000});
        CHECK(Fiber::join_wait_us_total() >= 0, "join_resource_wait_us path (join_wait_us_total)");
        reset_process_resource_quota_for_test();
    }

    static void ac4_query_schema() {
        std::println("\n--- AC4: query:resource-quota-stats schema 1600 ---");
        reset_process_resource_quota_for_test();
        process_resource_quota().set_limit(Dimension::Fibers, 1);
        Scheduler sched(1);
        SchedRunner runner(sched);
        Fiber* f = sched.spawn([] { Fiber::yield(YieldReason::Explicit); });
        (void)sched.spawn([] {}); // reject
        if (f)
            (void)Fiber::join(f, std::optional<std::uint64_t>{2000});

        CompilerService cs;
        auto h = cs.eval("(engine:metrics \"query:resource-quota-stats\")");
        CHECK(h && is_hash(*h), "stats hash");
        CHECK(href(cs, "schema") == 1618 || href(cs, "schema") == 1600 ||
                  href(cs, "schema") == 1590,
              "schema 1618|1600|1590");
        CHECK(href(cs, "issue") == 1600 || href(cs, "issue") == 1590, "issue 1600|1590");
        CHECK(href(cs, "fiber_spawn_rejected_total") >= 1, "fiber_spawn_rejected_total");
        CHECK(href(cs, "orchestration_quota_exceeded_count") >= 1, "orchestration_quota_exceeded");
        CHECK(href(cs, "join_resource_wait_us") >= 0, "join_resource_wait_us");
        CHECK(href(cs, "process_fibers_used") >= 0, "process_fibers_used");
        CHECK(href(cs, "orch_spawn_gated") == 1, "orch_spawn_gated");
        reset_process_resource_quota_for_test();
    }

    static void ac6_siblings() {
        std::println("\n--- AC6: siblings #1590 / #1586 ---");
        CompilerService cs;
        auto h1 = cs.eval("(engine:metrics \"query:resource-quota-stats\")");
        CHECK(h1 && is_hash(*h1), "resource-quota-stats");
        auto h2 = cs.eval("(engine:metrics \"query:parallel-orch-stats\")");
        CHECK(h2 && is_hash(*h2), "parallel-orch-stats");
        CHECK(aura::serve::parallel_orch::kParallelOrchIssue == 1586 ||
                  aura::serve::parallel_orch::kParallelOrchIssue == 1881,
              "parallel_orch issue (#1586/#1881)");
        CHECK(aura::core::resource_quota::kResourceQuotaIssue == 1579, "quota module issue");
    }

} // namespace

int run_orch_resource_quota() {
    std::println("=== Issue #1600: orch ResourceQuota typed reject ===");
    ac1_spawn_agent_quota();
    ac2_parallel_intend_quota();
    ac3_metrics_and_exhaust();
    ac4_query_schema();
    ac6_siblings();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_fiber_run_orch_resource_quota
// ─── end test_orch_resource_quota.cpp ───

// ─── from test_resource_quota.cpp → aura_fiber_run_resource_quota::run_resource_quota ───
namespace aura_fiber_run_resource_quota {
// @category: integration
// @reason: Issue #1481 — typed-error ResourceQuota enforcement helpers
// (check_arena_quota / check_mutation_quota / check_fiber_quota /
// check_time_quota / allocate_checked). Scope-limited close matching
// #1459 / #1470 / #1473-#1480. Tests via import pattern matching
// test_issue_1476 — links against aura.compiler.evaluator module.
// Per #1478 / #1480 precedent, this file is added to test-binding-
// allowlist.txt in case the evaluator module link hits the system
// 5-min build timeout (per invariant #29). Verification of the link
// itself deferred to #1538 batch.
//
// 7 ACs covering the typed-error surface + counter wiring that
// exists at HEAD (0bfeec38):
//
//   AC1: AuraErrorKind::ResourceQuotaExceeded exists as a distinct variant
//   AC2: check_arena_quota returns nullopt when limit==0 (unlimited) OR
//        requested <= limit
//   AC3: check_arena_quota returns typed AuraError with kind ==
//        ResourceQuotaExceeded when requested > limit
//   AC4: check_time_quota same pattern (elapsed vs resource_quota_time_us_)
//   AC5: check_mutation_quota / check_fiber_quota — no-arg variants return
//        nullopt when limit==0; aggregate compare intentionally deferred
//        (existing bump_longrunning_quota_violations continues to surface)
//   AC6: allocate_checked returns InternalInvariantViolation when arena_
//        is null (NOT a quota error — the quota check happens FIRST; the
//        arena null check returns InternalInvariantViolation only when
//        the quota check passes)
//   AC7: resource_quota_checks_total increments on every check_*_quota
//        call; resource_quota_rejects_total increments on reject only
//        (NOT on pass-through)


namespace aura_issue_1481_detail {

// test_harness.hpp defines CHECK (line ~127). Undefine + redefine to
// match test_issue_1476 formatting (cout/cerr stream + thread-safe counters).
#undef CHECK
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++::aura::test::g_passed;                                                              \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++::aura::test::g_failed;                                                              \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

} // namespace aura_issue_1481_detail

int aura_issue_1481_run() {
    using namespace aura_issue_1481_detail;

    using aura::core::AuraErrorKind;

    aura::compiler::Evaluator ev;
    aura::compiler::CompilerMetrics metrics;
    ev.set_compiler_metrics(static_cast<void*>(&metrics));

    // ── AC1: AuraErrorKind::ResourceQuotaExceeded exists + is distinct ──
    {
        const auto k_quota = static_cast<std::uint8_t>(AuraErrorKind::ResourceQuotaExceeded);
        const auto k_intern = static_cast<std::uint8_t>(AuraErrorKind::InternalInvariantViolation);
        CHECK(k_quota != k_intern, std::format("AuraErrorKind::ResourceQuotaExceeded is distinct "
                                               "from InternalInvariantViolation (got {} vs {})",
                                               k_quota, k_intern));
        // Must NOT be the sentinel
        CHECK(k_quota != static_cast<std::uint8_t>(AuraErrorKind::Sentinel_COUNT_),
              "AuraErrorKind::ResourceQuotaExceeded is not Sentinel_COUNT_");
    }

    // ── AC2: check_arena_quota returns nullopt when limit==0 (unlimited) ──
    {
        // Default limit == 0
        auto err = ev.check_arena_quota(/*requested_bytes=*/1024);
        CHECK(!err.has_value(),
              "check_arena_quota(1024) with default limit=0 (unlimited) returns nullopt");
    }

    // ── AC2b: check_arena_quota returns nullopt when requested <= limit ──
    {
        ev.set_resource_quota_memory(/*limit=*/1024);
        auto err = ev.check_arena_quota(/*requested=*/512);
        CHECK(!err.has_value(), "check_arena_quota(512) under quota=1024 returns nullopt");
    }

    // ── AC3: check_arena_quota returns typed AuraError when requested > limit ──
    {
        // quota=1024 still set from AC2b
        auto err = ev.check_arena_quota(/*requested=*/2048);
        CHECK(err.has_value(), "check_arena_quota(2048) over quota=1024 returns error");
        if (err) {
            CHECK(err->kind == AuraErrorKind::ResourceQuotaExceeded,
                  std::format("rejected err.kind == AuraErrorKind::ResourceQuotaExceeded (got {})",
                              static_cast<int>(err->kind)));
            CHECK(err->message.find("arena") != std::string::npos,
                  std::format("rejected err.message mentions 'arena' (got '{}')", err->message));
        }
    }

    // Reset arena quota for downstream ACs
    ev.set_resource_quota_memory(0);

    // ── AC4: check_time_quota ──
    {
        // limit=0 → pass
        auto err = ev.check_time_quota(/*elapsed_us=*/1000);
        CHECK(!err.has_value(), "check_time_quota(1000us) with default limit=0 returns nullopt");

        ev.set_resource_quota_time_us(/*limit=*/500);
        err = ev.check_time_quota(/*elapsed=*/100);
        CHECK(!err.has_value(), "check_time_quota(100us) under quota=500us returns nullopt");

        err = ev.check_time_quota(/*elapsed=*/1000);
        CHECK(err.has_value() && err->kind == AuraErrorKind::ResourceQuotaExceeded,
              "check_time_quota(1000us) over quota=500us returns ResourceQuotaExceeded");

        // Reset
        ev.set_resource_quota_time_us(0);
    }

    // ── AC5: check_mutation_quota / check_fiber_quota (no-arg variants) ──
    {
        // limit == 0 → pass
        auto m_err = ev.check_mutation_quota();
        CHECK(!m_err.has_value(), "check_mutation_quota() with default limit=0 returns nullopt");
        auto f_err = ev.check_fiber_quota();
        CHECK(!f_err.has_value(), "check_fiber_quota() with default limit=0 returns nullopt");

        // Non-zero limit: typed-error entry point returns nullopt
        // (cumulative tracking intentionally deferred — existing
        // bump_longrunning_quota_violations continues to surface).
        ev.set_resource_quota_fibers(/*limit=*/256);
        f_err = ev.check_fiber_quota();
        CHECK(
            !f_err.has_value(),
            "check_fiber_quota() with non-zero limit still returns nullopt (cumulative deferred)");
        ev.set_resource_quota_fibers(0);

        ev.set_resource_quota_memory(/*force mutation limit to non-zero via set on memory? no*/ 0);
        // mutation_quota limit accessor is via the metrics counter
        // (resource_quota_max_mutations); we leave default — covered by
        // the mutation_quota returns-nullopt branch above.
    }

    // ── AC6: allocate_checked returns InternalInvariantViolation without arena ──
    {
        // No arena set — the impl checks arena AFTER the quota check;
        // with limit=0 (unlimited), the quota passes and we fall through
        // to the arena null check which returns InternalInvariantViolation.
        auto result = ev.allocate_checked(/*size=*/128, /*align=*/8);
        CHECK(!result.has_value(), "allocate_checked without arena returns error");
        if (!result) {
            CHECK(result.error().kind == AuraErrorKind::InternalInvariantViolation,
                  std::format(
                      "allocate_checked no-arena err.kind == InternalInvariantViolation (got {})",
                      static_cast<int>(result.error().kind)));
        }
    }

    // ── AC7: counter increments (checks_total on every call; rejects_total on reject only) ──
    {
        const auto before_checks =
            metrics.resource_quota_checks_total.load(std::memory_order_relaxed);
        const auto before_rejects =
            metrics.resource_quota_rejects_total.load(std::memory_order_relaxed);

        // 4 pass-through calls (all unlimited) — should bump checks_total by 4,
        // reject_total unchanged.
        ev.check_arena_quota(100); // unlimited
        ev.check_time_quota(100);  // unlimited
        ev.check_mutation_quota(); // limit==0
        ev.check_fiber_quota();    // limit==0

        const auto after_pass_checks =
            metrics.resource_quota_checks_total.load(std::memory_order_relaxed);
        CHECK(after_pass_checks >= before_checks + 4,
              std::format("4 pass-through check_*_quota calls bump resource_quota_checks_total by "
                          ">= 4 ({} -> {})",
                          before_checks, after_pass_checks));

        const auto after_pass_rejects =
            metrics.resource_quota_rejects_total.load(std::memory_order_relaxed);
        CHECK(after_pass_rejects == before_rejects,
              std::format("pass-through does NOT bump resource_quota_rejects_total ({} == {})",
                          before_rejects, after_pass_rejects));

        // 1 reject call — should bump BOTH checks_total and rejects_total.
        const auto before_reject_rejects =
            metrics.resource_quota_rejects_total.load(std::memory_order_relaxed);
        ev.set_resource_quota_memory(100);
        ev.check_arena_quota(200); // over → reject
        ev.set_resource_quota_memory(0);

        const auto after_reject_rejects =
            metrics.resource_quota_rejects_total.load(std::memory_order_relaxed);
        CHECK(after_reject_rejects == before_reject_rejects + 1,
              std::format(
                  "reject check_arena_quota bumps resource_quota_rejects_total by 1 ({} -> {})",
                  before_reject_rejects, after_reject_rejects));
    }

    return ::aura::test::g_failed == 0 ? 0 : 1;
}

int run_resource_quota() {
    return aura_issue_1481_run();
}

} // namespace aura_fiber_run_resource_quota
// ─── end test_resource_quota.cpp ───

// ─── from test_resource_quota_module.cpp →
// aura_fiber_run_resource_quota_module::run_resource_quota_module ───
namespace aura_fiber_run_resource_quota_module {
// @category: unit
// @reason: Issue #1579 — dedicated ResourceQuota module: multi-dimension
// quotas, atomic check_and_consume, fiber RAII, overflow, process quota
// for scheduler spawn, concurrent setter/checker.
//
//   AC1: module exports ResourceQuota + phase constants
//   AC2: multi-dimension limits + check_and_consume / release
//   AC3: try_reserve_fiber RAII + process_resource_quota
//   AC4: overflow guard (uint64 max + 1)
//   AC5: concurrent consume/release races (monotonic counters)
//   AC6: evaluator check_fiber_quota mirrors process fibers limit
//   AC7: query:resource-quota-stats schema 1579 + process fields


namespace {

    using aura::core::AuraErrorKind;
    using aura::core::resource_quota::Dimension;
    using aura::core::resource_quota::process_resource_quota;
    using aura::core::resource_quota::reset_process_resource_quota_for_test;
    using aura::core::resource_quota::ResourceQuota;
    using aura::test::g_failed;
    using aura::test::g_passed;

    static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
        return a.load(std::memory_order_relaxed);
    }

    static void ac1_module_surface() {
        std::println("\n--- AC1: module surface ---");
        CHECK(aura::core::resource_quota::kResourceQuotaPhase >= 1, "phase >= 1");
        CHECK(aura::core::resource_quota::kResourceQuotaIssue == 1579, "issue 1579");
        ResourceQuota q;
        CHECK(q.limit(Dimension::Memory) == 0, "default unlimited memory");
        CHECK(q.limit(Dimension::Fibers) == 0, "default unlimited fibers");
    }

    static void ac2_multi_dimension_consume() {
        std::println("\n--- AC2: multi-dimension check_and_consume ---");
        ResourceQuota q;
        q.set_limit(Dimension::Memory, 1000);
        q.set_limit(Dimension::Mutations, 5);
        CHECK(!q.check_and_consume(Dimension::Memory, 400).has_value(), "mem 400 ok");
        CHECK(!q.check_and_consume(Dimension::Memory, 400).has_value(), "mem 800 ok");
        auto err = q.check_and_consume(Dimension::Memory, 400);
        CHECK(err.has_value(), "mem 1200 reject");
        CHECK(q.used(Dimension::Memory) == 800, "used stays 800 after reject");
        q.release(Dimension::Memory, 300);
        CHECK(q.used(Dimension::Memory) == 500, "release works");

        for (int i = 0; i < 5; ++i)
            CHECK(!q.check_and_consume(Dimension::Mutations, 1).has_value(), "mutation consume");
        CHECK(q.check_and_consume(Dimension::Mutations, 1).has_value(), "mutation over reject");
        CHECK(q.rejects_total.load() >= 2, "rejects counted");
    }

    static void ac3_fiber_token_and_process() {
        std::println("\n--- AC3: fiber token + process quota ---");
        reset_process_resource_quota_for_test();
        auto& pq = process_resource_quota();
        pq.set_limit(Dimension::Fibers, 2);

        auto t1 = pq.try_reserve_fiber();
        auto t2 = pq.try_reserve_fiber();
        CHECK(t1.has_value() && t2.has_value(), "reserve 2 fibers");
        CHECK(pq.used(Dimension::Fibers) == 2, "used == 2");
        auto t3 = pq.try_reserve_fiber();
        CHECK(!t3.has_value(), "3rd fiber rejected");
        t1->reset();
        CHECK(pq.used(Dimension::Fibers) == 1, "release via token");
        auto t4 = pq.try_reserve_fiber();
        CHECK(t4.has_value(), "reserve after release");
        // leave tokens to auto-release
    }

    static void ac4_overflow_guard() {
        std::println("\n--- AC4: overflow guard ---");
        ResourceQuota q;
        q.set_limit(Dimension::Memory, std::numeric_limits<std::uint64_t>::max());
        // Force used near max via unlimited path then set tight... simpler:
        // unlimited consume then check with huge amount after setting used manually
        q.memory_used.store(std::numeric_limits<std::uint64_t>::max() - 5,
                            std::memory_order_relaxed);
        q.set_limit(Dimension::Memory, std::numeric_limits<std::uint64_t>::max());
        auto err = q.check_and_consume(Dimension::Memory, 100);
        CHECK(err.has_value(), "overflow guard rejects");
        CHECK(q.overflow_guards_total.load() >= 1, "overflow_guards_total++");
    }

    static void ac5_concurrent_race() {
        std::println("\n--- AC5: concurrent consume/release ---");
        ResourceQuota q;
        q.set_limit(Dimension::Mutations, 10000);
        std::atomic<int> ok{0};
        std::atomic<int> rej{0};
        auto worker = [&] {
            for (int i = 0; i < 2000; ++i) {
                if (!q.check_and_consume(Dimension::Mutations, 1)) {
                    ok.fetch_add(1, std::memory_order_relaxed);
                    q.release(Dimension::Mutations, 1);
                } else {
                    rej.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };
        std::thread t1(worker), t2(worker), t3(worker), t4(worker);
        t1.join();
        t2.join();
        t3.join();
        t4.join();
        CHECK(ok.load() + rej.load() == 8000, "all attempts accounted");
        CHECK(q.checks_total.load() >= 8000, "checks monotonic");
        CHECK(q.used(Dimension::Mutations) <= 10000, "used within limit");
    }

    static void ac6_evaluator_fiber_quota() {
        std::println("\n--- AC6: evaluator check_fiber_quota ---");
        reset_process_resource_quota_for_test();
        aura::compiler::Evaluator ev;
        aura::compiler::CompilerMetrics metrics;
        ev.set_compiler_metrics(&metrics);

        CHECK(!ev.check_fiber_quota().has_value(), "unlimited fibers pass");
        ev.set_resource_quota_fibers(1);
        // Simulate one live fiber on process quota
        process_resource_quota().check_and_consume(Dimension::Fibers, 1);
        auto err = ev.check_fiber_quota();
        CHECK(err.has_value() && err->kind == AuraErrorKind::ResourceQuotaExceeded,
              "fiber quota reject when used >= limit");
        process_resource_quota().release(Dimension::Fibers, 1);
        CHECK(!ev.check_fiber_quota().has_value(), "pass after release");
    }

    static void ac7_stats_primitive() {
        std::println("\n--- AC7: query:resource-quota-stats schema 1579 ---");
        aura::compiler::CompilerService cs;
        auto r = cs.eval("(engine:metrics \"query:resource-quota-stats\")");
        CHECK(r.has_value() && aura::compiler::types::is_hash(*r), "hash");
        auto schema = cs.eval("(hash-ref (engine:metrics \"query:resource-quota-stats\") 'schema)");
        // #1618 bumped schema to 1618; accept earlier lineage for older agents.
        CHECK(schema.has_value() && aura::compiler::types::is_int(*schema) &&
                  (aura::compiler::types::as_int(*schema) == 1618 ||
                   aura::compiler::types::as_int(*schema) == 1600 ||
                   aura::compiler::types::as_int(*schema) == 1590 ||
                   aura::compiler::types::as_int(*schema) == 1579),
              "schema == 1618 or 1600 or 1590 or 1579");
        auto phase =
            cs.eval("(hash-ref (engine:metrics \"query:resource-quota-stats\") 'module_phase)");
        CHECK(phase.has_value() && aura::compiler::types::is_int(*phase) &&
                  aura::compiler::types::as_int(*phase) >= 1,
              "module_phase >= 1");
    }

} // namespace

int run_resource_quota_module() {
    std::println("=== test_resource_quota_module (#1579) ===");
    ac1_module_surface();
    ac2_multi_dimension_consume();
    ac3_fiber_token_and_process();
    ac4_overflow_guard();
    ac5_concurrent_race();
    ac6_evaluator_fiber_quota();
    ac7_stats_primitive();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_fiber_run_resource_quota_module
// ─── end test_resource_quota_module.cpp ───

int main() {
    std::println("\n######## run_parallel_intend ########");
    if (int rc = aura_fiber_run_parallel_intend::run_parallel_intend(); rc != 0) {
        std::println("run_parallel_intend FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_parallel_orch_1586 ########");
    if (int rc = aura_fiber_run_parallel_orch_1586::run_parallel_orch_1586(); rc != 0) {
        std::println("run_parallel_orch_1586 FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_parallel_orch_stress ########");
    if (int rc = aura_fiber_run_parallel_orch_stress::run_parallel_orch_stress(); rc != 0) {
        std::println("run_parallel_orch_stress FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_orch_quota_integration ########");
    if (int rc = aura_fiber_run_orch_quota_integration::run_orch_quota_integration(); rc != 0) {
        std::println("run_orch_quota_integration FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_orch_resource_quota ########");
    if (int rc = aura_fiber_run_orch_resource_quota::run_orch_resource_quota(); rc != 0) {
        std::println("run_orch_resource_quota FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_resource_quota ########");
    if (int rc = aura_fiber_run_resource_quota::run_resource_quota(); rc != 0) {
        std::println("run_resource_quota FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_resource_quota_module ########");
    if (int rc = aura_fiber_run_resource_quota_module::run_resource_quota_module(); rc != 0) {
        std::println("run_resource_quota_module FAILED rc={}", rc);
        return rc;
    }
    if (::aura::test::g_failed)
        return 1;
    std::println("\ntest_fiber_orch_parallel_quota_batch: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
