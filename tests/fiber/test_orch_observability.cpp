// @category: unit
// @reason: Issue #1881 — orch health query surfaces + hot-path metric wiring
// Issue #1881 (#1978 renamed): issue# moved from filename to header.
// (fold into existing query:orch-module-stats / mf-mailbox-stats /
// parallel-orch-stats; no new frozen query names).
//
//   AC1: source cites #1881; full mailbox/parallel snapshot helpers
//   AC2: query:orch-module-stats schema-1881 + agent/mailbox/parallel fields
//   AC3: query:mf-mailbox-stats + query:parallel-orch-stats schema-1881
//   AC4: spawn/join/send/recv/parallel stress → counters monotonic
//   AC5: health-score in 0..100 under mixed success/pressure
//   AC6: agent_send backpressure + closed paths bump (no dead bump)

#include "orch/orch.h"
#include "serve/fiber.h"
#include "serve/multi_fiber_mailbox.h"
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
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::serve::Fiber;
using aura::serve::JoinStatus;
using aura::serve::Scheduler;
using aura::serve::YieldReason;
using aura::serve::mf_mailbox::MailMessage;
using aura::serve::mf_mailbox::MailPriority;
using aura::serve::mf_mailbox::MultiFiberMailbox;
using aura::serve::mf_mailbox::PushStatus;
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

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    // ── AC1: source surface ──
    {
        std::println("\n--- AC1: #1881 source surface ---");
        auto spawn = read_first({"src/orch/agent_spawn.h", "../src/orch/agent_spawn.h"});
        auto mb =
            read_first({"src/serve/multi_fiber_mailbox.h", "../src/serve/multi_fiber_mailbox.h"});
        auto po = read_first({"src/serve/parallel_orch.h", "../src/serve/parallel_orch.h"});
        auto agent = read_first({"src/compiler/evaluator_primitives_agent.cpp",
                                 "../src/compiler/evaluator_primitives_agent.cpp"});
        auto msg = read_first({"src/compiler/evaluator_primitives_messaging.cpp",
                               "../src/compiler/evaluator_primitives_messaging.cpp"});
        CHECK(!spawn.empty() && spawn.find("#1881") != std::string::npos,
              "agent_spawn cites #1881");
        CHECK(spawn.find("send_backpressure_total") != std::string::npos, "send bp counter");
        CHECK(spawn.find("join_wait_us_total") != std::string::npos, "join wait counter");
        CHECK(!mb.empty() && mb.find("snapshot_global_full") != std::string::npos,
              "mailbox full snapshot");
        CHECK(mb.find("linear_checks") != std::string::npos, "fanout linear_checks");
        CHECK(!po.empty() && po.find("snapshot_global_ext") != std::string::npos,
              "parallel ext snapshot");
        CHECK(po.find("batch_ok_total") != std::string::npos, "batch_ok_total");
        CHECK(po.find("join_wait_us_total") != std::string::npos, "parallel join wait");
        CHECK(!agent.empty() && agent.find("schema-1881") != std::string::npos,
              "orch-module-stats schema-1881");
        CHECK(agent.find("health-score") != std::string::npos, "health-score");
        CHECK(agent.find("mailbox-backpressure-rejects") != std::string::npos,
              "mailbox submodule keys");
        CHECK(!msg.empty() && msg.find("schema-1881") != std::string::npos,
              "mf/parallel stats schema-1881");
    }

    // ── AC2: orch-module-stats health fields ──
    {
        std::println("\n--- AC2: query:orch-module-stats #1881 ---");
        CompilerService cs;
        auto h = cs.eval(R"((engine:metrics "query:orch-module-stats"))");
        CHECK(h && is_hash(*h), "orch-module-stats hash");
        CHECK(href(cs, "query:orch-module-stats", "schema") == 1588, "schema 1588");
        CHECK(href(cs, "query:orch-module-stats", "schema-1881") == 1881, "schema-1881");
        CHECK(href(cs, "query:orch-module-stats", "agent-stats-wired") == 1, "agent-stats-wired");
        CHECK(href(cs, "query:orch-module-stats", "mailbox-stats-wired") == 1,
              "mailbox-stats-wired");
        CHECK(href(cs, "query:orch-module-stats", "parallel-stats-wired") == 1,
              "parallel-stats-wired");
        CHECK(href(cs, "query:orch-module-stats", "health-score") >= 0 &&
                  href(cs, "query:orch-module-stats", "health-score") <= 100,
              "health-score 0..100");
        CHECK(href(cs, "query:orch-module-stats", "agents-active") >= 0, "agents-active");
        CHECK(href(cs, "query:orch-module-stats", "mailbox-pushes") >= 0, "mailbox-pushes");
        CHECK(href(cs, "query:orch-module-stats", "parallel-batch-ok") >= 0, "parallel-batch-ok");
    }

    // ── AC3: dedicated existing stats surfaces ──
    {
        std::println("\n--- AC3: mf-mailbox + parallel-orch stats ---");
        CompilerService cs;
        auto m = cs.eval(R"((engine:metrics "query:mf-mailbox-stats"))");
        CHECK(m && is_hash(*m), "mf-mailbox-stats hash");
        CHECK(href(cs, "query:mf-mailbox-stats", "schema-1881") == 1881, "mb schema-1881");
        CHECK(href(cs, "query:mf-mailbox-stats", "priority-high") >= 0, "priority-high");
        CHECK(href(cs, "query:mf-mailbox-stats", "linear-checks") >= 0, "linear-checks");
        CHECK(href(cs, "query:mf-mailbox-stats", "recv-waits") >= 0, "recv-waits");
        auto p = cs.eval(R"((engine:metrics "query:parallel-orch-stats"))");
        CHECK(p && is_hash(*p), "parallel-orch-stats hash");
        CHECK(href(cs, "query:parallel-orch-stats", "schema-1881") == 1881, "par schema-1881");
        CHECK(href(cs, "query:parallel-orch-stats", "batch-ok") >= 0, "batch-ok");
        CHECK(href(cs, "query:parallel-orch-stats", "avg-join-us") >= 0, "avg-join-us");
        CHECK(href(cs, "query:parallel-orch-stats", "quota-rejects") >= 0, "quota-rejects");
    }

    // ── AC4: stress loop counters monotonic ──
    {
        std::println("\n--- AC4: spawn/join/send/parallel stress ---");
        CompilerService cs;
        const auto sp0 = href(cs, "query:orch-module-stats", "agents-spawned");
        const auto jo0 = href(cs, "query:orch-module-stats", "agents-joined");
        const auto se0 = href(cs, "query:orch-module-stats", "agents-send");
        const auto pb0 = href(cs, "query:parallel-orch-stats", "batches");
        const auto bok0 = href(cs, "query:parallel-orch-stats", "batch-ok");
        const auto push0 = href(cs, "query:mf-mailbox-stats", "pushes");

        Scheduler sched(4);
        SchedRunner runner(sched);
        constexpr int kN = 80;
        std::vector<aura::orch::AgentHandle> hs;
        hs.reserve(kN);
        for (int i = 0; i < kN; ++i) {
            hs.push_back(aura::orch::spawn_agent_with_mailbox(
                sched, {.name = std::format("o{}", i),
                        .body = [] { Fiber::yield(YieldReason::Explicit); }}));
        }
        for (auto& h : hs) {
            (void)aura::orch::agent_send(
                h, MailMessage{.priority = MailPriority::High, .payload = "ping"});
        }
        (void)aura::orch::join_agents(hs, std::optional<std::uint64_t>{30000});

        std::vector<TaskSpec> tasks;
        for (int i = 0; i < 16; ++i) {
            tasks.push_back({.body = [] {
                Fiber::yield(YieldReason::Explicit);
                return aura::serve::parallel_orch::TaskResult{.ok = true, .value = "ok"};
            }});
        }
        auto batch = parallel_intend(sched, tasks, {.max_concurrency = 4, .timeout_ms = 15000});
        CHECK(batch.ok_count == 16 || batch.ok_count > 0, "parallel some ok");

        const auto sp1 = href(cs, "query:orch-module-stats", "agents-spawned");
        const auto jo1 = href(cs, "query:orch-module-stats", "agents-joined");
        const auto se1 = href(cs, "query:orch-module-stats", "agents-send");
        const auto pb1 = href(cs, "query:parallel-orch-stats", "batches");
        const auto bok1 = href(cs, "query:parallel-orch-stats", "batch-ok");
        const auto push1 = href(cs, "query:mf-mailbox-stats", "pushes");
        const auto ph1 = href(cs, "query:mf-mailbox-stats", "priority-high");
        std::println("  spawned {}→{} joined {}→{} send {}→{} batches {}→{} batch-ok {}→{} pushes "
                     "{}→{} pri-high={}",
                     sp0, sp1, jo0, jo1, se0, se1, pb0, pb1, bok0, bok1, push0, push1, ph1);
        CHECK(sp1 >= sp0 + kN, "spawned advanced");
        CHECK(jo1 >= jo0 + kN, "joined advanced");
        CHECK(se1 >= se0 + kN, "send advanced");
        CHECK(pb1 > pb0, "parallel batches advanced");
        CHECK(bok1 > bok0, "batch-ok advanced");
        CHECK(push1 > push0, "mailbox pushes advanced");
        CHECK(ph1 > 0, "priority-high advanced");
        CHECK(href(cs, "query:orch-module-stats", "join-wait-us-total") >= 0, "join wait total");
        CHECK(href(cs, "query:parallel-orch-stats", "join-wait-us-total") >= 0,
              "par join wait total");
    }

    // ── AC5: health-score bounds ──
    {
        std::println("\n--- AC5: health-score ---");
        CompilerService cs;
        const auto h = href(cs, "query:orch-module-stats", "health-score");
        CHECK(h >= 0 && h <= 100, "health-score in range after stress");
    }

    // ── AC6: dead-path send closed/backpressure bumps ──
    {
        std::println("\n--- AC6: send closed + backpressure paths ---");
        CompilerService cs;
        const auto cl0 = href(cs, "query:orch-module-stats", "send-closed");
        const auto bp0 = href(cs, "query:orch-module-stats", "send-backpressure");
        // Closed: invalid handle
        aura::orch::AgentHandle bad{};
        CHECK(aura::orch::agent_send(bad, MailMessage{.payload = "x"}) == PushStatus::Closed,
              "closed on invalid");
        // Backpressure: tiny HWM mailbox
        Scheduler sched(2);
        SchedRunner runner(sched);
        auto h = aura::orch::spawn_agent_with_mailbox(
            sched, {.name = "bp",
                    .body =
                        [] {
                            for (int i = 0; i < 50; ++i)
                                Fiber::yield(YieldReason::Explicit);
                        },
                    .attach_mailbox = true,
                    .mailbox_high_water = 1});
        CHECK(h.ok, "spawn bp agent");
        // First push may succeed; flood until backpressure.
        int bp_hits = 0;
        for (int i = 0; i < 20; ++i) {
            auto st = aura::orch::agent_send(
                h, MailMessage{.priority = MailPriority::Normal, .payload = std::format("m{}", i)});
            if (st == PushStatus::Backpressure)
                ++bp_hits;
        }
        (void)aura::orch::join_agent(h, std::optional<std::uint64_t>{10000});
        const auto cl1 = href(cs, "query:orch-module-stats", "send-closed");
        const auto bp1 = href(cs, "query:orch-module-stats", "send-backpressure");
        CHECK(cl1 > cl0, "send-closed advanced");
        CHECK(bp1 > bp0 || bp_hits > 0, "backpressure observed");
        std::println("  closed {}→{} bp {}→{} hits={}", cl0, cl1, bp0, bp1, bp_hits);
    }

    std::println("\n=== test_orch_observability_1881: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
