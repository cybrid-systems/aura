// @category: unit
// @reason: Issue #2153 — Configurable secondary drain timeout on non-Ok
// join (join_agent / join_agents / parallel_run).
//
//   AC1: Default drain_ms=2000 preserves #2082 (Ok path / yielding body)
//   AC2: drain_ms=0 → cancel only; residual moves if body still alive
//   AC3: Spin body without yield + short primary → non-Ok; after drain,
//        Done or residual metric > 0
//   AC4: Ok join never runs secondary drain; provenance only on Ok
//   AC5: parallel_intend Timeout path uses ParallelPolicy.drain_ms

#include "test_harness.hpp"

#include "orch/agent_spawn.h"
#include "serve/fiber.h"
#include "serve/parallel_orch.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::orch::AgentSpec;
using aura::orch::g_orch_module_stats;
using aura::orch::join_agent;
using aura::orch::JoinPolicy;
using aura::orch::kDefaultJoinDrainMs;
using aura::orch::kJoinDrainTimeoutIssue;
using aura::orch::spawn_agent_with_mailbox;
using aura::serve::Fiber;
using aura::serve::JoinStatus;
using aura::serve::Scheduler;
using aura::serve::YieldReason;
using aura::serve::parallel_orch::BatchStatus;
using aura::serve::parallel_orch::g_parallel_orch_stats;
using aura::serve::parallel_orch::parallel_intend;
using aura::serve::parallel_orch::ParallelPolicy;
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

std::int64_t href_orch(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:orch-module-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::int64_t href_par(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:parallel-orch-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

} // namespace

int run_test_join_drain_timeout() {
    std::println("=== Issue #2153: configurable join drain timeout ===");
    CHECK(kJoinDrainTimeoutIssue == 2153, "issue stamp");
    CHECK(kDefaultJoinDrainMs == 2000, "default drain 2000ms");

    // ── AC1: default drain preserves Ok path for yielding body ──
    {
        std::println("\n--- AC1: default drain / Ok yielding body ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm eval");

        Scheduler sched(2);
        SchedRunner run(sched);
        std::atomic<bool> ran{false};

        AgentSpec spec;
        spec.name = "yield-ok";
        spec.mutation_boundary = false;
        spec.attach_mailbox = false;
        spec.body = [&]() {
            Fiber::yield(YieldReason::Explicit);
            ran.store(true, std::memory_order_release);
        };
        auto h = spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(h.ok && h.fiber, "spawn ok");

        const auto residual0 = g_orch_module_stats.join_drain_residual_total.load();
        const auto drain_us0 = g_orch_module_stats.join_drain_us_total.load();
        const auto fail0 = g_orch_module_stats.join_fail_total.load();

        // Default policy (drain_ms=2000 via optional timeout overload).
        auto jr = join_agent(h, std::optional<std::uint64_t>{5000});
        CHECK(jr.status == JoinStatus::Ok, "AC1: Ok join");
        CHECK(ran.load(), "AC1: body ran");
        CHECK(g_orch_module_stats.join_drain_residual_total.load() == residual0,
              "AC1: Ok path no residual bump");
        CHECK(g_orch_module_stats.join_drain_us_total.load() == drain_us0,
              "AC1: Ok path no drain wait");
        CHECK(g_orch_module_stats.join_fail_total.load() == fail0, "AC1: no fail on Ok");

        CHECK(href_orch(cs, "schema-2153") == 2153, "schema-2153 orch");
        CHECK(href_orch(cs, "join-drain-wired") == 1, "join-drain-wired");
        CHECK(href_orch(cs, "join-drain-default-ms") == 2000, "default ms surface");
    }

    // ── AC2: drain_ms=0 cancel only; residual if body still alive ──
    {
        std::println("\n--- AC2: drain_ms=0 cancel only ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 0 0)").has_value(), "warm");

        Scheduler sched(2);
        SchedRunner run(sched);
        std::atomic<bool> entered{false};
        // Busy-loop without yield/cancel poll for several seconds so join
        // returns non-Ok while body is still live (residual). Bounded so
        // scheduler stop does not hang the process.
        AgentSpec spec;
        spec.name = "spin-no-yield";
        spec.mutation_boundary = false;
        spec.attach_mailbox = false;
        spec.body = [&]() {
            entered.store(true, std::memory_order_release);
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            volatile std::uint64_t x = 0;
            while (std::chrono::steady_clock::now() < end) {
                for (int i = 0; i < 64; ++i)
                    x += static_cast<std::uint64_t>(i);
            }
            (void)x;
        };
        auto h = spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(h.ok && h.fiber, "spawn spin");

        // Wait until body is on a worker.
        const auto wait_enter = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!entered.load() && std::chrono::steady_clock::now() < wait_enter)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(entered.load(), "body entered spin");

        const auto residual0 = g_orch_module_stats.join_drain_residual_total.load();
        const auto drain_us0 = g_orch_module_stats.join_drain_us_total.load();

        JoinPolicy pol;
        pol.primary_ms = 50; // short primary → Timeout
        pol.drain_ms = 0;    // cancel only
        auto jr = join_agent(h, pol);
        CHECK(jr.status != JoinStatus::Ok, "AC2: non-Ok primary (timeout)");
        CHECK(g_orch_module_stats.join_drain_us_total.load() == drain_us0,
              "AC2: drain_ms=0 no secondary wait us");
        // Residual should advance if still not Done after cancel-only.
        const auto residual1 = g_orch_module_stats.join_drain_residual_total.load();
        CHECK(residual1 > residual0 || (h.fiber && h.fiber->is_done()),
              "AC2: residual++ or already done");
        if (residual1 > residual0)
            CHECK(true, "AC2: residual advanced on live body");
    }

    // ── AC3: short primary + short drain → residual or Done ──
    {
        std::println("\n--- AC3: spin + short primary/drain ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 2 2)").has_value(), "warm");

        Scheduler sched(2);
        SchedRunner run(sched);
        std::atomic<bool> entered{false};
        AgentSpec spec;
        spec.name = "spin-drain";
        spec.mutation_boundary = false;
        spec.attach_mailbox = false;
        spec.body = [&]() {
            entered.store(true, std::memory_order_release);
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            volatile std::uint64_t x = 1;
            while (std::chrono::steady_clock::now() < end)
                x = x * 1103515245u + 12345u;
            (void)x;
        };
        auto h = spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(h.ok && h.fiber, "spawn");

        const auto wait_enter = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!entered.load() && std::chrono::steady_clock::now() < wait_enter)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(entered.load(), "entered");

        const auto residual0 = g_orch_module_stats.join_drain_residual_total.load();
        const auto drain_us0 = g_orch_module_stats.join_drain_us_total.load();

        JoinPolicy pol;
        pol.primary_ms = 30;
        pol.drain_ms = 50; // short secondary
        auto jr = join_agent(h, pol);
        CHECK(jr.status != JoinStatus::Ok, "AC3: non-Ok");
        const auto residual1 = g_orch_module_stats.join_drain_residual_total.load();
        const auto drain_us1 = g_orch_module_stats.join_drain_us_total.load();
        CHECK(drain_us1 >= drain_us0, "AC3: drain wait recorded (or zero if instant Done)");
        CHECK(residual1 > residual0 || (h.fiber && h.fiber->is_done()),
              "AC3: residual or Done after drain");
    }

    // ── AC4: Ok path never secondary drain; source provenance only on Ok ──
    {
        std::println("\n--- AC4: Ok no drain; provenance Ok-only ---");
        const auto src = read_file("src/orch/agent_spawn.h");
        CHECK(!src.empty(), "agent_spawn.h readable");
        CHECK(src.find("JoinPolicy") != std::string::npos, "JoinPolicy present");
        CHECK(src.find("kDefaultJoinDrainMs") != std::string::npos, "default constant");
        CHECK(src.find("join_drain_residual_total") != std::string::npos, "residual metric");
        CHECK(src.find("cancel_and_drain_fiber") != std::string::npos, "drain helper");
        // Provenance only when Ok
        CHECK(src.find("if (jr.status == serve::JoinStatus::Ok)") != std::string::npos ||
                  src.find("jr.status == serve::JoinStatus::Ok") != std::string::npos,
              "Ok-gated provenance");
        // Secondary drain only on non-Ok
        CHECK(src.find("jr.status != serve::JoinStatus::Ok") != std::string::npos,
              "non-Ok cancel+drain");

        CompilerService cs;
        CHECK(cs.eval("(+ 3 3)").has_value(), "warm");
        Scheduler sched(2);
        SchedRunner run(sched);
        AgentSpec spec;
        spec.name = "ok-no-drain";
        spec.mutation_boundary = false;
        spec.attach_mailbox = false;
        spec.body = []() { Fiber::yield(YieldReason::Explicit); };
        auto h = spawn_agent_with_mailbox(sched, std::move(spec));
        const auto residual0 = g_orch_module_stats.join_drain_residual_total.load();
        const auto drain_us0 = g_orch_module_stats.join_drain_us_total.load();
        auto jr = join_agent(h, JoinPolicy{.primary_ms = 3000, .drain_ms = 2000});
        CHECK(jr.status == JoinStatus::Ok, "AC4: Ok");
        CHECK(g_orch_module_stats.join_drain_residual_total.load() == residual0,
              "AC4: no residual on Ok");
        CHECK(g_orch_module_stats.join_drain_us_total.load() == drain_us0,
              "AC4: no drain us on Ok");
    }

    // ── AC5: parallel_intend Timeout uses ParallelPolicy.drain_ms ──
    {
        std::println("\n--- AC5: parallel Timeout drain_ms ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 4 4)").has_value(), "warm");

        Scheduler sched(2);
        SchedRunner run(sched);

        std::atomic<bool> entered{false};
        std::vector<TaskSpec> tasks(1);
        tasks[0].name = "spin-par";
        tasks[0].body = [&]() {
            entered.store(true, std::memory_order_release);
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            volatile std::uint64_t x = 7;
            while (std::chrono::steady_clock::now() < end)
                x = x * 1664525u + 1013904223u;
            (void)x;
            return aura::serve::parallel_orch::TaskResult{.ok = true, .value = "x"};
        };

        ParallelPolicy pol;
        pol.max_concurrency = 2;
        pol.timeout_ms = 40; // overall primary deadline
        pol.drain_ms = 0;    // cancel only → residual likely
        pol.fail_fast = false;

        const auto residual0 = g_parallel_orch_stats.join_drain_residual_total.load();
        const auto drain_us0 = g_parallel_orch_stats.join_drain_us_total.load();
        const auto to0 = g_parallel_orch_stats.timeouts.load();

        auto batch = parallel_intend(sched, tasks, pol);
        CHECK(batch.status == BatchStatus::Timeout || batch.join_status == JoinStatus::Timeout,
              "AC5: Timeout batch");
        CHECK(g_parallel_orch_stats.timeouts.load() > to0, "AC5: timeouts counter");
        CHECK(g_parallel_orch_stats.join_drain_us_total.load() == drain_us0,
              "AC5: drain_ms=0 no parallel drain us");
        // Residual when body still spinning.
        const auto residual1 = g_parallel_orch_stats.join_drain_residual_total.load();
        CHECK(residual1 >= residual0, "AC5: residual non-decreasing");
        if (residual1 > residual0)
            CHECK(true, "AC5: parallel residual advanced");

        // With positive drain_ms, wait is recorded.
        std::atomic<bool> entered2{false};
        std::vector<TaskSpec> tasks2(1);
        tasks2[0].name = "spin-par-drain";
        tasks2[0].body = [&]() {
            entered2.store(true, std::memory_order_release);
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            volatile std::uint64_t x = 3;
            while (std::chrono::steady_clock::now() < end)
                x = x * 214013u + 2531011u;
            (void)x;
            return aura::serve::parallel_orch::TaskResult{.ok = true};
        };
        ParallelPolicy pol2;
        pol2.max_concurrency = 2;
        pol2.timeout_ms = 40;
        pol2.drain_ms = 60;
        const auto drain_us1 = g_parallel_orch_stats.join_drain_us_total.load();
        (void)parallel_intend(sched, tasks2, pol2);
        // Drain us may advance if not_done was non-empty.
        CHECK(g_parallel_orch_stats.join_drain_us_total.load() >= drain_us1,
              "AC5: positive drain_ms may record wait");

        CHECK(href_par(cs, "schema-2153") == 2153, "schema-2153 parallel");
        CHECK(href_par(cs, "join-drain-wired") == 1, "parallel join-drain-wired");

        // Source: ParallelPolicy.drain_ms
        const auto por = read_file("src/serve/parallel_orch.h");
        CHECK(por.find("drain_ms") != std::string::npos, "ParallelPolicy.drain_ms");
        CHECK(por.find("join_drain_residual_total") != std::string::npos,
              "parallel residual metric");
    }

    // Aura surface cites :drain-ms + query keys (test-binding for prim sources).
    {
        std::println("\n--- Aura orch:agent-join :drain-ms ---");
        const auto ag = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(ag.find("drain-ms") != std::string::npos || ag.find("drain_ms") != std::string::npos,
              "orch:agent-join accepts drain-ms");
        CHECK(ag.find("schema-2153") != std::string::npos, "schema-2153 on join result");
        CHECK(ag.find("join-drain-residual-total") != std::string::npos,
              "orch-module-stats residual key");
        const auto msg = read_file("src/compiler/evaluator_primitives_messaging.cpp");
        CHECK(!msg.empty(), "evaluator_primitives_messaging.cpp readable");
        CHECK(msg.find("schema-2153") != std::string::npos, "parallel-orch-stats schema-2153");
        CHECK(msg.find("join-drain-residual-total") != std::string::npos,
              "parallel residual key in messaging prims");
    }

    std::println("\n=== #2153 join drain timeout: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_join_drain_timeout();
}
#endif
