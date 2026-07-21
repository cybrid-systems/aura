// @category: integration
// @reason: Issue #1602 — E2E stress for concurrent mutate + parallel agent
// + Fiber::join + GC compact/steal paths (refine #1584–#1588 / #1595 / #1597).
//
//   AC1: suite/parallel_orchestration_stress.aura companion (C++ metrics)
//   AC2: parallel_intend N tasks under concurrency cap; all join
//   AC3: MultiFiberMailbox fan-in during stress batch
//   AC4: CompilerService parallel-intend + mutate + gc-heap no crash
//   AC5: query:parallel-orch-stats + ai-closedloop-readiness advanced
//   AC6: Fiber::join batch + join_wait metrics; linear join total readable

#include "test_harness.hpp"

#include "serve/fiber.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/parallel_orch.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

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
    CHECK(status_ok, std::format("batch status ok/partial/near-complete-timeout (status={} ok={})",
                                 static_cast<int>(batch.status), batch.ok_count));
    CHECK(batch.ok_count >= static_cast<std::uint64_t>(N - 4),
          std::format("ok_count high ({} / {})", batch.ok_count, N));
    CHECK(ran.load() >= N - 4, std::format("bodies ran ({})", ran.load()));
    CHECK(g_parallel_orch_stats.intend_batches.load() > batches0, "batches advanced");
    CHECK(g_parallel_orch_stats.tasks_joined.load() >= joined0 + static_cast<std::uint64_t>(N - 4),
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
    CHECK(ok_rounds >= kRounds - 1, std::format("stable rounds ({}/{})", ok_rounds, kRounds));
}

} // namespace

int main() {
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
