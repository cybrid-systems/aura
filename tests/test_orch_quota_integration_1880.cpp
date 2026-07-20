// @category: unit
// @reason: Issue #1880 — ResourceQuota + try_acquire on agent_spawn /
// parallel_orch (typed ResourceQuotaExceeded, no panic/OOM).
//
//   AC1: source cites #1880; memory preflight + try_acquire body wire
//   AC2: spawn rejects when memory quota exhausted (typed error)
//   AC3: parallel_orch rejects when memory preflight fails
//   AC4: query:resource-quota-stats schema-1880 + agent_arena fields
//   AC5: spawn until memory exhaust → graceful reject, join releases
//   AC6: try_acquire reject path under mutation budget (no panic)

#include "compiler/observability_metrics.h"
#include "core/resource_quota.hh"
#include "orch/orch.h"
#include "serve/fiber.h"
#include "serve/parallel_orch.h"
#include "serve/scheduler.h"
#include "test_harness.hpp"

#include <atomic>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

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

int main() {
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
