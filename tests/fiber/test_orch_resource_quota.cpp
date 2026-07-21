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

#include "test_harness.hpp"
#include "core/resource_quota.hh"
#include "orch/orch.h"
#include "serve/fiber.h"
#include "serve/scheduler.h"
#include "serve/parallel_orch.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

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
            sched,
            {.name = "a" + std::to_string(i), .body = [] { Fiber::yield(YieldReason::Explicit); }});
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
            CHECK(r.error.find("ResourceQuotaExceeded") != std::string::npos, "typed task error");
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
    CHECK(href(cs, "schema") == 1618 || href(cs, "schema") == 1600 || href(cs, "schema") == 1590,
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
    CHECK(aura::serve::parallel_orch::kParallelOrchIssue == 1586, "parallel_orch issue");
    CHECK(aura::core::resource_quota::kResourceQuotaIssue == 1579, "quota module issue");
}

} // namespace

int main() {
    std::println("=== Issue #1600: orch ResourceQuota typed reject ===");
    ac1_spawn_agent_quota();
    ac2_parallel_intend_quota();
    ac3_metrics_and_exhaust();
    ac4_query_schema();
    ac6_siblings();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
