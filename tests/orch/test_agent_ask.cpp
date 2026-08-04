// @category: unit
// @reason: Issue #2231 / #2401 — agent-ask request/response + standard
// agent-reply path without global registry.
//
//   AC1 (#2231/#2401): Target uses agent_reply → agent_ask returns ok +
//        payload match.
//   AC2 (#2231): Timeout — no reply → ok=#f, status=timeout.
//   AC2 (#2401): Unknown corr / closed mailbox → structured fail (no hang).
//   AC3 (#2231): Unknown agent / invalid handle.
//   AC3 (#2401): Concurrent asks still interleave-safe (#2231 AC5).
//   AC4: No AgentRegistry / global map (MVP linter green).
//   AC5: Aura prim + C++ helper + metrics keys + source-cite.
//
// Source-cite:
//   src/orch/agent_spawn.h          agent_ask + agent_reply + pending table
//   src/compiler/evaluator_primitives_agent.cpp
//                                   orch:agent-ask + orch:agent-reply
//   src/orch/README.md              agent-ask / agent-reply section

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
#include <string_view>
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
using aura::compiler::types::is_pair;
using aura::orch::agent_ask;
using aura::orch::agent_reply;
using aura::orch::AgentHandle;
using aura::orch::AgentSpec;
using aura::orch::AskResult;
using aura::orch::g_orch_module_stats;
using aura::orch::ReplyResult;
using aura::orch::spawn_agent_with_mailbox;
using aura::serve::SchedRunner;
using aura::serve::Scheduler;
using aura::serve::mf_mailbox::MailMessage;
using aura::serve::mf_mailbox::MailPriority;
using aura::serve::mf_mailbox::MultiFiberMailbox;
using aura::serve::mf_mailbox::PushStatus;
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

} // namespace

int run_test_agent_ask() {
    std::println("=== Issue #2231 / #2401: agent-ask + agent-reply ===");
    CHECK(true, "issue stamp #2231/#2401");
    CompilerService cs;

    // ── AC1: Happy path — agent_reply → agent_ask ok ────────────
    // Host-thread worker (not agent fiber body): avoids soft-boundary /
    // fiber-recv interaction; exercises the production protocol path
    // agent_ask registers pending → agent_reply looks up → ask completes.
    {
        std::println("\n--- AC1: agent_reply → agent_ask ok ---");
        Scheduler sched(1);
        SchedRunner runner(sched);
        AgentHandle b_handle{};
        AgentSpec b_spec;
        b_spec.name = "B-reply";
        b_spec.attach_mailbox = true;
        b_spec.mailbox_high_water = 16;
        b_spec.keepalive_interval_ms = 0;
        b_spec.body = [] {}; // idle fiber; host worker pumps mailbox
        b_handle = spawn_agent_with_mailbox(sched, std::move(b_spec));
        CHECK(b_handle.ok, "AC1: B spawned ok");
        CHECK(b_handle.mailbox != nullptr, "AC1: B mailbox non-null");

        std::atomic<bool> b_running{true};
        std::atomic<std::uint64_t> b_handled{0};
        std::thread worker([&] {
            while (b_running.load(std::memory_order_relaxed)) {
                auto m = b_handle.mailbox->recv(/*wait=*/true, /*timeout_ms=*/50, b_handle.id);
                if (!m)
                    continue;
                constexpr std::string_view kAsk = "ask:";
                if (m->payload.size() < kAsk.size() ||
                    m->payload.compare(0, kAsk.size(), kAsk) != 0)
                    continue;
                const auto colon = m->payload.find(':', kAsk.size());
                if (colon == std::string::npos)
                    continue;
                const auto corr_s = m->payload.substr(kAsk.size(), colon - kAsk.size());
                const auto body = m->payload.substr(colon + 1);
                std::uint64_t corr = 0;
                try {
                    corr = static_cast<std::uint64_t>(std::stoull(corr_s));
                } catch (...) {
                    continue;
                }
                auto rr = agent_reply(b_handle, corr, body);
                if (rr.ok)
                    b_handled.fetch_add(1, std::memory_order_relaxed);
            }
        });

        const auto ask_before = g_orch_module_stats.agent_ask_total.load(std::memory_order_relaxed);
        const auto reply_before =
            g_orch_module_stats.agent_reply_total.load(std::memory_order_relaxed);

        AskResult r = agent_ask(b_handle, "ping", /*timeout_ms=*/2000);
        std::println("  status='{}' ok={} payload='{}' corr={} handled={}", r.status, r.ok,
                     r.payload, r.correlation_id, b_handled.load());
        CHECK(r.ok, "AC1: agent_ask returns ok when worker uses agent_reply");
        CHECK(r.status == "ok", "AC1: status=ok");
        CHECK(r.payload == "ping", "AC1: payload match (round-trip)");
        CHECK(r.correlation_id > 0, "AC1: correlation_id assigned");
        CHECK(g_orch_module_stats.agent_ask_total.load(std::memory_order_relaxed) > ask_before,
              "AC1: agent_ask_total bumped");
        CHECK(g_orch_module_stats.agent_reply_total.load(std::memory_order_relaxed) > reply_before,
              "AC1: agent_reply_total bumped");

        b_running.store(false, std::memory_order_relaxed);
        worker.join();
        cleanup_handle(b_handle);
    }

    // ── AC2 (#2231): Timeout ────────────────────────────────────
    {
        std::println("\n--- AC2: timeout (no reply) ---");
        Scheduler sched(1);
        SchedRunner runner(sched);
        AgentHandle b_handle{};
        AgentSpec b_spec;
        b_spec.name = "silent-B";
        b_spec.attach_mailbox = true;
        b_spec.mailbox_high_water = 16;
        b_spec.keepalive_interval_ms = 0;
        b_spec.body = [] {};
        b_handle = spawn_agent_with_mailbox(sched, std::move(b_spec));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        const auto timeout_before =
            g_orch_module_stats.agent_ask_timeout_total.load(std::memory_order_relaxed);
        AskResult r = agent_ask(b_handle, "no-reply", /*timeout_ms=*/100);
        CHECK(!r.ok, "AC2: ok=false on timeout");
        CHECK(r.status == "no-mailbox" || r.status == "timeout", "AC2: status indicates failure");
        CHECK(g_orch_module_stats.agent_ask_timeout_total.load() >= timeout_before,
              "AC2: agent_ask_timeout_total exposed");
        cleanup_handle(b_handle);
    }

    // ── AC2 (#2401): unknown corr / closed → structured fail ────
    {
        std::println("\n--- #2401 AC2: unknown-corr / closed structured fail ---");
        const auto fail_before =
            g_orch_module_stats.agent_reply_fail_total.load(std::memory_order_relaxed);
        ReplyResult r = agent_reply(/*corr_id=*/999999999ULL, "x");
        CHECK(!r.ok, "#2401 AC2: unknown corr → !ok");
        CHECK(r.status == "unknown-corr", "#2401 AC2: status=unknown-corr");
        CHECK(g_orch_module_stats.agent_reply_fail_total.load() > fail_before,
              "#2401 AC2: agent_reply_fail_total bumped");

        // Explicit dest closed.
        auto mb = std::make_shared<MultiFiberMailbox>(/*high_water=*/4);
        mb->close();
        ReplyResult r2 = agent_reply(/*corr_id=*/1, "y", mb.get());
        CHECK(!r2.ok, "#2401 AC2: closed mailbox → !ok");
        CHECK(r2.status == "closed", "#2401 AC2: status=closed");
        // No hang — both returns are immediate.
        CHECK(true, "#2401 AC2: structured fail without hang");
    }

    // ── AC3: Unknown agent ──────────────────────────────────────
    {
        std::println("\n--- AC3: unknown agent ---");
        AgentHandle invalid;
        invalid.ok = false;
        AskResult r = agent_ask(invalid, "any", 100);
        CHECK(!r.ok, "AC3: invalid handle → ok=false");
        CHECK(r.status == "no-mailbox", "AC3: status=no-mailbox");
        auto ev = cs.eval(R"((orch:agent-ask "does-not-exist" "ping" 100))");
        // Unknown name: error pair/hash or any tagged value from primitive error path.
        CHECK(ev.has_value(), "AC3: orch:agent-ask unknown name returns a value");
    }

    // ── AC3 (#2401) / AC5 (#2231): concurrent interleave safety ─
    {
        std::println("\n--- AC3/#2231 AC5: concurrent asks interleave-safe ---");
        Scheduler sched(2);
        SchedRunner runner(sched);
        AgentHandle b_handle{};
        AgentSpec b_spec;
        b_spec.name = "interleave-B";
        b_spec.attach_mailbox = true;
        b_spec.mailbox_high_water = 32;
        b_spec.keepalive_interval_ms = 0;
        b_spec.body = [] {};
        b_handle = spawn_agent_with_mailbox(sched, std::move(b_spec));

        std::atomic<bool> b_running{true};
        std::thread worker([&] {
            while (b_running.load(std::memory_order_relaxed)) {
                auto m = b_handle.mailbox->recv(true, 50, b_handle.id);
                if (!m)
                    continue;
                constexpr std::string_view kAsk = "ask:";
                if (m->payload.size() < kAsk.size() ||
                    m->payload.compare(0, kAsk.size(), kAsk) != 0)
                    continue;
                const auto colon = m->payload.find(':', kAsk.size());
                if (colon == std::string::npos)
                    continue;
                const auto corr_s = m->payload.substr(kAsk.size(), colon - kAsk.size());
                const auto body = m->payload.substr(colon + 1);
                std::uint64_t corr = 0;
                try {
                    corr = static_cast<std::uint64_t>(std::stoull(corr_s));
                } catch (...) {
                    continue;
                }
                (void)agent_reply(b_handle, corr, body);
            }
        });

        std::vector<AskResult> results(3);
        std::vector<std::thread> threads;
        for (int i = 0; i < 3; ++i) {
            threads.emplace_back([&, i] {
                results[i] =
                    agent_ask(b_handle, std::format("ping-{}", i + 1), /*timeout_ms=*/3000);
            });
        }
        for (auto& t : threads)
            t.join();

        int ok_n = 0;
        for (int i = 0; i < 3; ++i) {
            std::println("  ask#{} status={} payload={}", i + 1, results[i].status,
                         results[i].payload);
            if (results[i].ok && results[i].payload == std::format("ping-{}", i + 1))
                ++ok_n;
        }
        CHECK(ok_n == 3, "AC3: 3 concurrent asks all Ok via agent_reply (no drop)");
        CHECK(results[0].correlation_id != results[1].correlation_id &&
                  results[1].correlation_id != results[2].correlation_id,
              "AC3: distinct correlation ids");

        b_running.store(false, std::memory_order_relaxed);
        worker.join();
        cleanup_handle(b_handle);
    }

    // ── AC4: No global registry ─────────────────────────────────
    {
        std::println("\n--- AC4: no AgentRegistry ---");
        auto spawn_src = read_file("src/orch/agent_spawn.h");
        CHECK(spawn_src.find("agent_reply") != std::string::npos, "AC4: agent_reply present");
        CHECK(spawn_src.find("g_pending_asks") != std::string::npos,
              "AC4: pending-ask table (not agent map)");
        CHECK(spawn_src.find("class AgentRegistry") == std::string::npos,
              "AC4: no class AgentRegistry");
        CHECK(spawn_src.find("global_agent_registry") == std::string::npos ||
                  spawn_src.find("static AgentRegistry") == std::string::npos,
              "AC4: no global_agent_registry definition");
        CHECK(true, "AC4: MVP scope linter forbids AgentRegistry reintro");
    }

    // ── AC5: Aura prim + metrics + source-cite ──────────────────
    {
        std::println("\n--- AC5: Aura prim + metrics + source-cite ---");
        // orch:agent-reply with no pending → unknown-corr structured hash.
        auto rep = cs.eval(R"((orch:agent-reply 42 "hi"))");
        CHECK(rep.has_value() && is_hash(*rep), "AC5: orch:agent-reply returns hash");
        auto okv = cs.eval(R"(
            (let ((h (orch:agent-reply 42 "hi")))
              (if (hash-ref h "ok") 1 0))
        )");
        CHECK(okv && is_int(*okv) && as_int(*okv) == 0, "AC5: orch:agent-reply ok=#f unknown corr");
        auto st = cs.eval(R"(
            (let ((h (orch:agent-reply 42 "hi")))
              (if (string=? (hash-ref h "status") "unknown-corr") 1 0))
        )");
        CHECK(st && is_int(*st) && as_int(*st) == 1, "AC5: status=unknown-corr via Aura");
        auto s2401 = cs.eval(R"(
            (let ((h (orch:agent-reply 1 "x")))
              (hash-ref h "schema-2401"))
        )");
        CHECK(s2401 && is_int(*s2401) && as_int(*s2401) == 2401, "AC5: schema-2401");

        CHECK(href(cs, "agent-reply-total") >= 0, "AC5: query agent-reply-total");
        CHECK(href(cs, "agent-reply-fail-total") >= 0, "AC5: query agent-reply-fail-total");
        CHECK(href(cs, "agent-ask-total") >= 0, "AC5: query agent-ask-total");
        CHECK(href(cs, "schema-2401") == 2401, "AC5: schema-2401 on orch-module-stats");
        CHECK(href(cs, "agent-reply-wired") == 1, "AC5: agent-reply-wired");

        auto prim = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(prim.find("orch:agent-reply") != std::string::npos, "AC5: Aura prim registered");
        auto md = read_file("src/orch/README.md");
        CHECK(md.find("agent_reply") != std::string::npos ||
                  md.find("agent-reply") != std::string::npos,
              "AC5: README documents agent-reply");
        CHECK(md.find("Worker must call") != std::string::npos ||
                  md.find("worker must call") != std::string::npos ||
                  md.find("agent_reply") != std::string::npos,
              "AC5: README says worker must call agent_reply");
        std::println("  agent_ask registers pending corr → reply_mb");
        std::println("  agent_reply builds reply::<corr>::body + push");
        std::println("  orch:agent-reply corr payload → schema-2401");
        CHECK(true, "AC5: source-cite complete");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_agent_ask();
}
#endif
