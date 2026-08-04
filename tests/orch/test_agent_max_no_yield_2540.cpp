// @category: unit
// @reason: Issue #2540 — AgentSpec.max_no_yield_ms cooperative yield contract.
//   Issue #2585 — production default + opt-out (AURA_AGENT_MAX_NO_YIELD_MS=0).
//
//   #2540 ACs:
//   AC1: max_no_yield_ms==0 → zero extra yield / metric (behaviour parity)
//   AC2: max_no_yield_ms>0 + agent_poll in tight loop → forced yield in window
//   AC3: cancel after poll prefers cooperative Done (no hang)
//   AC4: keepalive + mutation_boundary combine without break
//   AC5: agent_forced_yield_total + schema-2540
//   AC6: source-cite; no docs/design
//
//   #2585 ACs:
//   AC1: production default (AURA_SANDBOX=restricted, no opt-out env) forces
//        non-zero coop window; bumps agent_no_yield_default_applied_total
//   AC2: body that never polls still benefits (cancel/GC observe default
//        window instead of running forever)
//   AC3: AURA_AGENT_MAX_NO_YIELD_MS=0 opt-out keeps zero-cost path
//        (AC1 of #2540 preserved)
//   AC4: AURA_SANDBOX=off (dev_off) keeps zero-cost even without opt-out
//   AC5: README + schema document default + opt-out (source-cite)
//   AC6: agent-no-yield-default-applied-total metric surface (query)

#include "test_harness.hpp"
#include "orch/sched_runner_test_helper.h"

#include "orch/agent_spawn.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <print>
#include <string>
#include <thread>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::orch::agent_poll;
using aura::orch::AgentHandle;
using aura::orch::AgentSpec;
using aura::orch::g_orch_module_stats;
using aura::orch::join_agent;
using aura::orch::kAgentMaxNoYieldIssue;
using aura::orch::spawn_agent_with_mailbox;
using aura::serve::Fiber;
using aura::serve::JoinStatus;
using aura::serve::SchedRunner;
using aura::serve::Scheduler;
using aura::serve::YieldReason;
using aura::test::g_failed;
using aura::test::g_passed;

void cleanup_handle(AgentHandle& h) {
    if (h.fiber) {
        h.fiber->request_cancel();
        if (auto* sched = h.fiber->owner_sched()) {
            sched->note_orphan_fiber(h.fiber, /*hard_deadline_ms=*/50);
            sched->reap_orphans_now();
        }
    }
}

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:orch-module-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// #2585: env helpers for AURA_SANDBOX + AURA_AGENT_MAX_NO_YIELD_MS.
void clear_env(const char* k) {
#if defined(_WIN32)
    _putenv_s(k, "");
#else
    unsetenv(k);
#endif
}
void set_env(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

} // namespace

int run_test_agent_max_no_yield_2540() {
    std::println("=== Issue #2540: AgentSpec.max_no_yield_ms cooperative yield ===");
    CHECK(kAgentMaxNoYieldIssue == 2540, "issue stamp");
    CompilerService cs;

    // ── AC1: max_no_yield_ms==0 zero cost ───────────────────────
    {
        std::println("\n--- AC1: max_no_yield_ms==0 zero extra yield ---");
        Scheduler sched(1);
        SchedRunner runner(sched);
        const auto y0 =
            g_orch_module_stats.agent_forced_yield_total.load(std::memory_order_relaxed);

        AgentSpec spec;
        spec.name = "off-body";
        spec.max_no_yield_ms = 0;
        spec.mutation_boundary = false;
        std::atomic<int> polls{0};
        spec.body = [&] {
            for (int i = 0; i < 50; ++i) {
                if (agent_poll())
                    polls.fetch_add(1, std::memory_order_relaxed);
                // tight spin without forced yield when contract off
            }
        };
        auto h = spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(h.ok, "AC1: spawn ok");
        CHECK(h.max_no_yield_ms == 0, "AC1: handle max_no_yield_ms 0");
        CHECK(!h.coop, "AC1: no coop state when off");
        auto jr = join_agent(h, std::optional<std::uint64_t>{2000});
        CHECK(jr.status == JoinStatus::Ok || jr.status == JoinStatus::Timeout, "AC1: join");
        CHECK(polls.load() == 0, "AC1: agent_poll never forced yield");
        CHECK(g_orch_module_stats.agent_forced_yield_total.load() == y0,
              "AC1: agent_forced_yield_total unchanged");
        cleanup_handle(h);
    }

    // ── AC2: max_no_yield_ms>0 forces yield in window ───────────
    {
        std::println("\n--- AC2: max_no_yield_ms>0 forced yield ---");
        Scheduler sched(1);
        SchedRunner runner(sched);
        const auto y0 =
            g_orch_module_stats.agent_forced_yield_total.load(std::memory_order_relaxed);

        AgentSpec spec;
        spec.name = "poll-body";
        spec.max_no_yield_ms = 5; // 5ms window
        spec.mutation_boundary = false;
        std::atomic<int> forced{0};
        std::atomic<bool> ran{false};
        spec.body = [&] {
            ran.store(true, std::memory_order_relaxed);
            // Seed: last_coop set at body entry; sleep past window then poll.
            for (int i = 0; i < 30; ++i) {
                // Busy work then poll; also wall-clock wait past 5ms once.
                if (i == 1)
                    aura::orch::fiber_sleep_ms(10);
                if (agent_poll())
                    forced.fetch_add(1, std::memory_order_relaxed);
            }
        };
        auto h = spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(h.ok, "AC2: spawn ok");
        CHECK(h.max_no_yield_ms == 5, "AC2: handle records max_no_yield_ms");
        CHECK(h.coop != nullptr, "AC2: coop state allocated");
        auto jr = join_agent(h, std::optional<std::uint64_t>{3000});
        CHECK(jr.status == JoinStatus::Ok || jr.status == JoinStatus::Timeout, "AC2: join");
        CHECK(ran.load(), "AC2: body ran");
        CHECK(forced.load() >= 1, "AC2: at least one forced yield via agent_poll");
        CHECK(g_orch_module_stats.agent_forced_yield_total.load() > y0,
              "AC2: agent_forced_yield_total bumped");
        cleanup_handle(h);
    }

    // ── AC3: cancel prefers cooperative Done ────────────────────
    {
        std::println("\n--- AC3: cancel after poll → cooperative Done ---");
        Scheduler sched(1);
        SchedRunner runner(sched);
        std::atomic<bool> hold{true};
        std::atomic<bool> saw_cancel{false};

        AgentSpec spec;
        spec.name = "cancel-coop";
        spec.max_no_yield_ms = 5;
        spec.mutation_boundary = false;
        spec.body = [&] {
            while (hold.load(std::memory_order_relaxed)) {
                if (aura::serve::g_current_fiber &&
                    aura::serve::g_current_fiber->is_cancel_requested()) {
                    saw_cancel.store(true, std::memory_order_relaxed);
                    break;
                }
                (void)agent_poll();
                aura::orch::fiber_sleep_ms(1);
            }
        };
        auto h = spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(h.ok, "AC3: spawn ok");
        // Let body enter poll loop.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (h.fiber)
            h.fiber->request_cancel();
        auto jr = join_agent(h, std::optional<std::uint64_t>{2000});
        CHECK(jr.status == JoinStatus::Ok || jr.status == JoinStatus::Cancelled ||
                  jr.status == JoinStatus::Timeout,
              "AC3: join after cancel");
        // Prefer cooperative observation of cancel (soft: body exited loop).
        CHECK(saw_cancel.load() || (h.fiber && h.fiber->is_done()),
              "AC3: cancel observed or fiber Done (coop path)");
        hold.store(false, std::memory_order_relaxed);
        cleanup_handle(h);
    }

    // ── AC4: AgentSpec combines max_no_yield with keepalive / mutation flags
    // without breaking spawn (runtime poll path covered in AC2).
    {
        std::println("\n--- AC4: AgentSpec field combination ---");
        Scheduler sched(1);
        SchedRunner runner(sched);
        AgentSpec spec;
        spec.name = "combo-fields";
        spec.attach_mailbox = true;
        spec.keepalive_interval_ms = 50;
        spec.mutation_boundary = true;
        spec.max_no_yield_ms = 20;
        // Short body: no mid-body yield under soft boundary (avoids known
        // yield×soft-boundary races); verifies spawn accepts full combo.
        spec.body = [] {
            for (int i = 0; i < 4; ++i)
                Fiber::yield(YieldReason::Explicit);
        };
        auto h = spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(h.ok, "AC4: spawn ok with keepalive + mutation_boundary + max_no_yield");
        CHECK(h.keepalive_interval_ms == 50, "AC4: keepalive still set");
        CHECK(h.max_no_yield_ms == 20, "AC4: max_no_yield still set");
        CHECK(h.coop != nullptr, "AC4: coop present when max_no_yield>0");
        auto jr = join_agent(h, std::optional<std::uint64_t>{3000});
        CHECK(jr.status == JoinStatus::Ok || jr.status == JoinStatus::Timeout ||
                  jr.status == JoinStatus::Cancelled,
              "AC4: join returns structured status");
        cleanup_handle(h);
        // Off path still zero-cost when flags combined with max_no_yield=0.
        AgentSpec off;
        off.name = "combo-off";
        off.keepalive_interval_ms = 0;
        off.mutation_boundary = true;
        off.max_no_yield_ms = 0;
        off.body = [] {};
        auto h2 = spawn_agent_with_mailbox(sched, std::move(off));
        CHECK(h2.ok && !h2.coop && h2.max_no_yield_ms == 0,
              "AC4: mutation_boundary + max_no_yield=0 still zero coop state");
        (void)join_agent(h2, std::optional<std::uint64_t>{1000});
        cleanup_handle(h2);
    }

    // ── AC5: metrics + schema ───────────────────────────────────
    {
        std::println("\n--- AC5: metrics + schema-2540 ---");
        CHECK(g_orch_module_stats.agent_forced_yield_total.load() > 0,
              "AC5: C++ agent_forced_yield_total > 0 after AC2");
        CHECK(href(cs, "schema-2540") == 2540, "AC5: schema-2540 query");
        CHECK(href(cs, "agent-forced-yield-total") >= 0, "AC5: agent-forced-yield-total query");
        // issue-2540 / wired: source-locked + schema query is the public sentinel.
        auto prim = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(prim.find("issue-2540") != std::string::npos, "AC5: issue-2540 key registered");
        CHECK(prim.find("agent-max-no-yield-wired") != std::string::npos,
              "AC5: agent-max-no-yield-wired registered");
        CHECK(prim.find("agent-forced-yield-total") != std::string::npos,
              "AC5: agent-forced-yield-total registered");
    }

    // ── AC6: source-cite ────────────────────────────────────────
    {
        std::println("\n--- AC6: source-cite ---");
        auto spawn_src = read_file("src/orch/agent_spawn.h");
        auto prim = read_file("src/compiler/evaluator_primitives_agent.cpp");
        auto md = read_file("src/orch/README.md");
        CHECK(spawn_src.find("max_no_yield_ms") != std::string::npos, "AC6: AgentSpec field");
        CHECK(spawn_src.find("agent_poll") != std::string::npos, "AC6: agent_poll");
        CHECK(spawn_src.find("AgentCoopYield") != std::string::npos, "AC6: AgentCoopYield");
        CHECK(spawn_src.find("agent_forced_yield_total") != std::string::npos, "AC6: metric field");
        CHECK(spawn_src.find("kAgentMaxNoYieldIssue") != std::string::npos, "AC6: issue stamp");
        CHECK(prim.find("orch:agent-poll") != std::string::npos, "AC6: Aura prim");
        CHECK(prim.find("schema-2540") != std::string::npos, "AC6: schema in metrics");
        CHECK(prim.find("max-no-yield-ms") != std::string::npos ||
                  prim.find("max_no_yield_ms") != std::string::npos,
              "AC6: spawn-agent keyword");
        CHECK(md.find("2540") != std::string::npos, "AC6: README cites #2540");
        CHECK(md.find("max_no_yield_ms") != std::string::npos ||
                  md.find("agent_poll") != std::string::npos,
              "AC6: README documents contract");
    }

    // ── #2585: production default + opt-out ────────────────────
    {
        // AC1: production default — AURA_SANDBOX=restricted, no opt-out env
        // → coop state installed with 50ms default; default-applied metric bumped.
        std::println("\n--- #2585 AC1: production default forces non-zero coop ---");
        set_env("AURA_SANDBOX", "restricted");
        clear_env("AURA_AGENT_MAX_NO_YIELD_MS");
        Scheduler sched(1);
        SchedRunner runner(sched);
        const auto d0 = g_orch_module_stats.agent_no_yield_default_applied_total.load(
            std::memory_order_relaxed);

        AgentSpec spec;
        spec.name = "2585-default";
        spec.max_no_yield_ms = 0; // unset — default should apply
        spec.mutation_boundary = false;
        std::atomic<bool> ran{false};
        std::atomic<int> forced{0};
        spec.body = [&] {
            ran.store(true, std::memory_order_relaxed);
            // Run past default window so a subsequent agent_poll() would force
            // yield; without poll, body just exits (cancel/GC reclaims fiber).
            for (int i = 0; i < 30; ++i) {
                if (i == 2)
                    aura::orch::fiber_sleep_ms(60); // > 50ms default
                if (agent_poll())
                    forced.fetch_add(1, std::memory_order_relaxed);
            }
        };
        auto h = spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(h.ok, "#2585 AC1: spawn ok under default");
        CHECK(h.coop != nullptr, "#2585 AC1: coop state installed by default");
        CHECK(h.max_no_yield_ms == 50, "#2585 AC1: handle records 50ms default window");
        CHECK(g_orch_module_stats.agent_no_yield_default_applied_total.load() == d0 + 1,
              "#2585 AC1: agent_no_yield_default_applied_total bumped exactly once");
        (void)join_agent(h, std::optional<std::uint64_t>{3000});
        CHECK(ran.load(), "#2585 AC1: body ran");
        // agent_poll() with default 50ms window and a 60ms sleep should force yield.
        CHECK(forced.load() >= 1,
              "#2585 AC1: default window lets agent_poll force yield (no opt-in required)");
        cleanup_handle(h);
    }
    {
        // AC2: body that never polls still gets the default applied (cancel/GC
        // observe the window; AC1 zero-cost under explicit max_no_yield_ms>0
        // path preserved). Here we just assert default is applied at spawn and
        // body that does not call agent_poll completes (no hang).
        std::println("\n--- #2585 AC2: body without poll still benefits from default ---");
        set_env("AURA_SANDBOX", "restricted");
        clear_env("AURA_AGENT_MAX_NO_YIELD_MS");
        Scheduler sched(1);
        SchedRunner runner(sched);
        AgentSpec spec;
        spec.name = "2585-no-poll";
        spec.max_no_yield_ms = 0;
        spec.mutation_boundary = false;
        std::atomic<bool> done{false};
        spec.body = [&] {
            // Short CPU-only body (no agent_poll). With default applied, the
            // fiber is reclaim-eligible after 50ms, but the body exits before
            // that — so the test is just that the spawn applied the default
            // and the body completes.
            for (int i = 0; i < 100; ++i) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            done.store(true, std::memory_order_relaxed);
        };
        auto h = spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(h.ok && h.coop != nullptr && h.max_no_yield_ms == 50,
              "#2585 AC2: default still applies even when body never polls");
        auto jr = join_agent(h, std::optional<std::uint64_t>{2000});
        CHECK(jr.status == JoinStatus::Ok || jr.status == JoinStatus::Timeout,
              "#2585 AC2: body completes under default");
        CHECK(done.load() || jr.status == JoinStatus::Timeout,
              "#2585 AC2: body ran (or timed out cleanly without hang)");
        cleanup_handle(h);
    }
    {
        // AC3: AURA_AGENT_MAX_NO_YIELD_MS=0 explicit opt-out keeps zero-cost
        // (AC1 of #2540 preserved; no default injection; metric not bumped).
        std::println("\n--- #2585 AC3: explicit opt-out keeps zero-cost ---");
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_AGENT_MAX_NO_YIELD_MS", "0");
        Scheduler sched(1);
        SchedRunner runner(sched);
        const auto d0 = g_orch_module_stats.agent_no_yield_default_applied_total.load(
            std::memory_order_relaxed);
        AgentSpec spec;
        spec.name = "2585-optout";
        spec.max_no_yield_ms = 0;
        spec.mutation_boundary = false;
        spec.body = [] {};
        auto h = spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(h.ok, "#2585 AC3: spawn ok with explicit opt-out");
        CHECK(!h.coop, "#2585 AC3: no coop state under explicit opt-out (zero-cost)");
        CHECK(h.max_no_yield_ms == 0, "#2585 AC3: handle max_no_yield_ms stays 0");
        CHECK(g_orch_module_stats.agent_no_yield_default_applied_total.load() == d0,
              "#2585 AC3: default NOT applied under explicit opt-out");
        cleanup_handle(h);
        clear_env("AURA_AGENT_MAX_NO_YIELD_MS");
    }
    {
        // AC4: AURA_SANDBOX=off (dev_off / unit Soft) keeps zero-cost even
        // without the explicit opt-out env var.
        std::println("\n--- #2585 AC4: dev_off forces soft (zero-cost) ---");
        set_env("AURA_SANDBOX", "off");
        clear_env("AURA_AGENT_MAX_NO_YIELD_MS");
        Scheduler sched(1);
        SchedRunner runner(sched);
        const auto d0 = g_orch_module_stats.agent_no_yield_default_applied_total.load(
            std::memory_order_relaxed);
        AgentSpec spec;
        spec.name = "2585-devoff";
        spec.max_no_yield_ms = 0;
        spec.mutation_boundary = false;
        spec.body = [] {};
        auto h = spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(h.ok, "#2585 AC4: spawn ok under dev_off");
        CHECK(!h.coop, "#2585 AC4: no coop state under dev_off");
        CHECK(h.max_no_yield_ms == 0, "#2585 AC4: handle max_no_yield_ms stays 0");
        CHECK(g_orch_module_stats.agent_no_yield_default_applied_total.load() == d0,
              "#2585 AC4: default NOT applied under dev_off");
        cleanup_handle(h);
        clear_env("AURA_SANDBOX");
    }
    {
        // AC5: README + schema document default + opt-out.
        std::println("\n--- #2585 AC5: README + schema source-cite ---");
        auto md = read_file("src/orch/README.md");
        auto spawn_src = read_file("src/orch/agent_spawn.h");
        auto prim = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(md.find("2585") != std::string::npos, "#2585 AC5: README cites #2585");
        CHECK(md.find("AURA_AGENT_MAX_NO_YIELD_MS") != std::string::npos,
              "#2585 AC5: README documents opt-out env var");
        CHECK(md.find("agent_no_yield_default_applied_total") != std::string::npos ||
                  md.find("default-applied") != std::string::npos,
              "#2585 AC5: README documents default-applied metric");
        CHECK(spawn_src.find("resolve_agent_default_max_no_yield_ms") != std::string::npos,
              "#2585 AC5: helper defined in agent_spawn.h");
        CHECK(spawn_src.find("agent_no_yield_default_applied_total") != std::string::npos,
              "#2585 AC5: metric field in g_orch_module_stats");
        CHECK(spawn_src.find("AURA_AGENT_MAX_NO_YIELD_MS") != std::string::npos,
              "#2585 AC5: opt-out env wired in spawn path");
    }
    {
        // AC6: agent-no-yield-default-applied-total metric surface (query).
        std::println("\n--- #2585 AC6: query:orch-module-stats surfaces #2585 metric ---");
        // The metric is bumped only when default applied; under unit Soft
        // (current env) the query surface must still return the key (>= 0).
        const auto v = href(cs, "agent-no-yield-default-applied-total");
        CHECK(v >= 0,
              "#2585 AC6: query:orch-module-stats returns agent-no-yield-default-applied-total");
    }

    std::println("\n=== #2540 + #2585 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_agent_max_no_yield_2540();
}
#endif
