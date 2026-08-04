// @category: unit
// @reason: Issue #2538 — typed correlation for agent-ask / agent-reply.
//
//   AC1: corr_id match without payload text parse (MailKind + correlation_id)
//   AC2: legacy ask:/reply: text prefix still works (#2231/#2401)
//   AC3: concurrent asks interleave-safe (distinct corr, no cross-talk)
//   AC4: unknown-corr / closed / backpressure structured fail (no hang)
//   AC5: metrics + schema-2538; Aura orch:agent-ask / orch:agent-reply
//   AC6: source-cite; no docs/design

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
using aura::orch::agent_ask;
using aura::orch::agent_reply;
using aura::orch::AgentHandle;
using aura::orch::AgentSpec;
using aura::orch::AskResult;
using aura::orch::format_reply_payload;
using aura::orch::g_orch_module_stats;
using aura::orch::kAgentAskTypedCorrIssue;
using aura::orch::ReplyResult;
using aura::orch::spawn_agent_with_mailbox;
using aura::orch::try_match_reply;
using aura::orch::try_parse_ask;
using aura::serve::SchedRunner;
using aura::serve::Scheduler;
using aura::serve::mf_mailbox::MailKind;
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

// Worker that uses try_parse_ask (typed-first) + agent_reply.
void typed_worker_loop(AgentHandle& h, std::atomic<bool>& running, std::atomic<int>& handled) {
    while (running.load(std::memory_order_relaxed)) {
        auto m = h.mailbox->recv(/*wait=*/true, /*timeout_ms=*/50, h.id);
        if (!m)
            continue;
        auto ask = try_parse_ask(*m);
        if (!ask)
            continue;
        auto rr = agent_reply(h, ask->correlation_id, std::string(ask->body));
        if (rr.ok)
            handled.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

int run_test_agent_ask_typed_corr() {
    std::println("=== Issue #2538: typed agent-ask correlation ===");
    CHECK(kAgentAskTypedCorrIssue == 2538, "issue stamp");
    CompilerService cs;

    // ── AC1: typed match without payload text parse ─────────────
    {
        std::println("\n--- AC1: typed corr match (no text parse) ---");
        // Direct unit: try_match_reply with pure body (no reply: prefix).
        MailMessage pure_reply;
        pure_reply.kind = MailKind::Reply;
        pure_reply.correlation_id = 42;
        pure_reply.payload = "pure-body-no-prefix";
        bool typed = false;
        auto body = try_match_reply(pure_reply, 42, &typed);
        CHECK(body.has_value(), "AC1: typed pure body matches");
        CHECK(typed, "AC1: matched_typed=true");
        CHECK(*body == "pure-body-no-prefix", "AC1: body is full payload (no strip)");

        // try_parse_ask typed path: kind=Ask + corr, pure body.
        MailMessage pure_ask;
        pure_ask.kind = MailKind::Ask;
        pure_ask.correlation_id = 7;
        pure_ask.payload = "hello";
        auto env = try_parse_ask(pure_ask);
        CHECK(env.has_value(), "AC1: try_parse_ask typed");
        CHECK(env->typed, "AC1: parse typed=true");
        CHECK(env->correlation_id == 7, "AC1: corr from field not text");
        CHECK(env->body == "hello", "AC1: body without ask: prefix");

        // End-to-end: agent_ask + agent_reply stamps typed fields.
        Scheduler sched(1);
        SchedRunner runner(sched);
        AgentHandle b{};
        AgentSpec spec;
        spec.name = "typed-B";
        spec.attach_mailbox = true;
        spec.mailbox_high_water = 16;
        spec.keepalive_interval_ms = 0;
        spec.body = [] {};
        b = spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(b.ok, "AC1: B spawned");

        std::atomic<bool> running{true};
        std::atomic<int> handled{0};
        std::thread worker([&] { typed_worker_loop(b, running, handled); });

        const auto typed_before =
            g_orch_module_stats.agent_ask_typed_match_total.load(std::memory_order_relaxed);
        const auto reply_typed_before =
            g_orch_module_stats.agent_reply_typed_total.load(std::memory_order_relaxed);

        AskResult r = agent_ask(b, "typed-ping", /*timeout_ms=*/2000);
        CHECK(r.ok, "AC1: e2e agent_ask ok via typed worker");
        CHECK(r.payload == "typed-ping", "AC1: e2e payload match");
        CHECK(g_orch_module_stats.agent_ask_typed_match_total.load() > typed_before,
              "AC1: agent_ask_typed_match_total bumped");
        CHECK(g_orch_module_stats.agent_reply_typed_total.load() > reply_typed_before,
              "AC1: agent_reply_typed_total bumped");

        // Inject a pure typed reply into a pending ask (simulates peer that
        // omits dual-write prefix). Uses explicit dest + raw push.
        {
            auto reply_mb = std::make_shared<MultiFiberMailbox>(/*high_water=*/8);
            std::uint64_t corr = 9001;
            {
                std::lock_guard<std::mutex> lock(aura::orch::g_pending_ask_mu);
                aura::orch::g_pending_asks[corr] = reply_mb;
            }
            MailMessage m;
            m.kind = MailKind::Reply;
            m.correlation_id = corr;
            m.payload = "no-prefix-body";
            m.priority = MailPriority::Normal;
            CHECK(reply_mb->push(std::move(m)) == PushStatus::Ok, "AC1: push pure typed reply");
            bool t2 = false;
            auto popped = reply_mb->recv(true, 100, 0);
            CHECK(popped.has_value(), "AC1: recv pure typed");
            auto matched = try_match_reply(*popped, corr, &t2);
            CHECK(matched && t2 && *matched == "no-prefix-body",
                  "AC1: pure typed reply body without text parse");
            {
                std::lock_guard<std::mutex> lock(aura::orch::g_pending_ask_mu);
                aura::orch::g_pending_asks.erase(corr);
            }
        }

        running.store(false, std::memory_order_relaxed);
        worker.join();
        cleanup_handle(b);
    }

    // ── AC2: legacy text prefix still works ─────────────────────
    {
        std::println("\n--- AC2: legacy ask:/reply: text prefix ---");
        // try_parse_ask on Normal + text prefix.
        MailMessage legacy_ask;
        legacy_ask.kind = MailKind::Normal;
        legacy_ask.correlation_id = 0;
        legacy_ask.payload = "ask:99:legacy-body";
        auto env = try_parse_ask(legacy_ask);
        CHECK(env.has_value(), "AC2: legacy try_parse_ask");
        CHECK(!env->typed, "AC2: typed=false for text path");
        CHECK(env->correlation_id == 99, "AC2: corr from text");
        CHECK(env->body == "legacy-body", "AC2: body from text");

        // try_match_reply legacy (kind=Normal, only text).
        MailMessage legacy_reply;
        legacy_reply.kind = MailKind::Normal;
        legacy_reply.correlation_id = 0;
        legacy_reply.payload = format_reply_payload(99, "legacy-ok");
        bool typed = true;
        auto body = try_match_reply(legacy_reply, 99, &typed);
        CHECK(body.has_value() && !typed && *body == "legacy-ok", "AC2: legacy text reply matches");

        // e2e: worker only uses text parse (ignores kind) + hand-pushes
        // reply with format_reply_payload only (no typed fields).
        Scheduler sched(1);
        SchedRunner runner(sched);
        AgentHandle b{};
        AgentSpec spec;
        spec.name = "legacy-B";
        spec.attach_mailbox = true;
        spec.mailbox_high_water = 16;
        spec.keepalive_interval_ms = 0;
        spec.body = [] {};
        b = spawn_agent_with_mailbox(sched, std::move(spec));

        std::atomic<bool> running{true};
        std::thread worker([&] {
            while (running.load(std::memory_order_relaxed)) {
                auto m = b.mailbox->recv(true, 50, b.id);
                if (!m)
                    continue;
                // Strict text-only parse (ignore typed fields).
                constexpr std::string_view kAsk = "ask:";
                if (m->payload.size() < kAsk.size() ||
                    m->payload.compare(0, kAsk.size(), kAsk) != 0)
                    continue;
                const auto colon = m->payload.find(':', kAsk.size());
                if (colon == std::string::npos)
                    continue;
                const auto corr_s = m->payload.substr(kAsk.size(), colon - kAsk.size());
                const auto body_s = m->payload.substr(colon + 1);
                std::uint64_t corr = 0;
                try {
                    corr = static_cast<std::uint64_t>(std::stoull(corr_s));
                } catch (...) {
                    continue;
                }
                // Hand-build legacy reply into pending dest (text only).
                std::shared_ptr<MultiFiberMailbox> dest;
                {
                    std::lock_guard<std::mutex> lock(aura::orch::g_pending_ask_mu);
                    auto it = aura::orch::g_pending_asks.find(corr);
                    if (it != aura::orch::g_pending_asks.end())
                        dest = it->second;
                }
                if (!dest)
                    continue;
                MailMessage rep;
                rep.kind = MailKind::Normal;
                rep.correlation_id = 0;
                rep.payload = format_reply_payload(corr, body_s);
                rep.priority = MailPriority::Normal;
                (void)dest->push(std::move(rep));
            }
        });

        AskResult r = agent_ask(b, "legacy-ping", /*timeout_ms=*/2000);
        CHECK(r.ok, "AC2: agent_ask ok via pure text-prefix worker");
        CHECK(r.payload == "legacy-ping", "AC2: legacy payload match");
        // Match should count as non-typed (text path).
        // (typed_match may not bump for this ask)

        running.store(false, std::memory_order_relaxed);
        worker.join();
        cleanup_handle(b);
    }

    // ── AC3: concurrent asks, no cross-talk + Normal noise ──────
    {
        std::println("\n--- AC3: concurrent asks + Normal noise ---");
        Scheduler sched(2);
        SchedRunner runner(sched);
        AgentHandle b{};
        AgentSpec spec;
        spec.name = "conc-B";
        spec.attach_mailbox = true;
        spec.mailbox_high_water = 64;
        spec.keepalive_interval_ms = 0;
        spec.body = [] {};
        b = spawn_agent_with_mailbox(sched, std::move(spec));

        std::atomic<bool> running{true};
        std::atomic<int> handled{0};
        std::thread worker([&] { typed_worker_loop(b, running, handled); });

        // Inject unrelated Normal messages that must not break ask.
        std::thread noise([&] {
            for (int i = 0; i < 20; ++i) {
                MailMessage n;
                n.kind = MailKind::Normal;
                n.payload = std::format("noise-{}", i);
                n.to_fiber = b.id;
                (void)b.mailbox->push(std::move(n));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

        std::vector<AskResult> results(3);
        std::vector<std::thread> threads;
        for (int i = 0; i < 3; ++i) {
            threads.emplace_back([&, i] {
                results[i] = agent_ask(b, std::format("ping-{}", i + 1), /*timeout_ms=*/4000);
            });
        }
        for (auto& t : threads)
            t.join();
        noise.join();

        int ok_n = 0;
        for (int i = 0; i < 3; ++i) {
            std::println("  ask#{} status={} payload={} corr={}", i + 1, results[i].status,
                         results[i].payload, results[i].correlation_id);
            if (results[i].ok && results[i].payload == std::format("ping-{}", i + 1))
                ++ok_n;
        }
        CHECK(ok_n == 3, "AC3: 3 concurrent asks all Ok (no cross-talk)");
        CHECK(results[0].correlation_id != results[1].correlation_id &&
                  results[1].correlation_id != results[2].correlation_id,
              "AC3: distinct correlation ids");

        running.store(false, std::memory_order_relaxed);
        worker.join();
        cleanup_handle(b);
    }

    // ── AC4: structured fail paths ──────────────────────────────
    {
        std::println("\n--- AC4: unknown-corr / closed structured fail ---");
        const auto fail_before =
            g_orch_module_stats.agent_reply_fail_total.load(std::memory_order_relaxed);
        ReplyResult r = agent_reply(/*corr_id=*/888888888ULL, "x");
        CHECK(!r.ok && r.status == "unknown-corr", "AC4: unknown-corr");
        CHECK(g_orch_module_stats.agent_reply_fail_total.load() > fail_before,
              "AC4: fail counter bumped");

        auto mb = std::make_shared<MultiFiberMailbox>(/*high_water=*/2);
        mb->close();
        ReplyResult r2 = agent_reply(/*corr_id=*/1, "y", mb.get());
        CHECK(!r2.ok && r2.status == "closed", "AC4: closed");

        // Backpressure: tiny high_water, fill then reply.
        auto mb2 = std::make_shared<MultiFiberMailbox>(/*high_water=*/1);
        MailMessage filler;
        filler.payload = "fill";
        CHECK(mb2->push(std::move(filler)) == PushStatus::Ok, "AC4: fill queue");
        ReplyResult r3 = agent_reply(/*corr_id=*/2, "bp", mb2.get());
        CHECK(!r3.ok && r3.status == "backpressure", "AC4: backpressure");
        CHECK(true, "AC4: no hang on structured fails");
    }

    // ── AC5: metrics + Aura prims ───────────────────────────────
    {
        std::println("\n--- AC5: metrics + schema-2538 + Aura prims ---");
        CHECK(href(cs, "schema-2538") == 2538, "AC5: schema-2538");
        CHECK(href(cs, "issue-2538") == 2538, "AC5: issue-2538");
        CHECK(href(cs, "agent-ask-typed-corr-wired") == 1, "AC5: wired sentinel");
        CHECK(href(cs, "agent-ask-typed-match-total") >= 0, "AC5: typed-match query");
        CHECK(href(cs, "agent-reply-typed-total") >= 0, "AC5: reply-typed query");
        CHECK(href(cs, "schema-2231") == 2231, "AC5: schema-2231 retained");
        CHECK(href(cs, "schema-2401") == 2401, "AC5: schema-2401 retained");

        auto rep = cs.eval(R"((orch:agent-reply 42 "hi"))");
        CHECK(rep.has_value() && is_hash(*rep), "AC5: orch:agent-reply hash");
        auto s2538 = cs.eval(R"(
            (let ((h (orch:agent-reply 1 "x")))
              (hash-ref h "schema-2538"))
        )");
        CHECK(s2538 && is_int(*s2538) && as_int(*s2538) == 2538,
              "AC5: orch:agent-reply schema-2538");
    }

    // ── AC6: source-cite ────────────────────────────────────────
    {
        std::println("\n--- AC6: source-cite ---");
        auto spawn_src = read_file("src/orch/agent_spawn.h");
        auto mb_src = read_file("src/serve/multi_fiber_mailbox.h");
        auto prim = read_file("src/compiler/evaluator_primitives_agent.cpp");
        auto md = read_file("src/orch/README.md");
        CHECK(spawn_src.find("kAgentAskTypedCorrIssue") != std::string::npos, "AC6: issue stamp");
        CHECK(spawn_src.find("try_parse_ask") != std::string::npos, "AC6: try_parse_ask");
        CHECK(spawn_src.find("try_match_reply") != std::string::npos, "AC6: try_match_reply");
        CHECK(spawn_src.find("agent_ask_typed_match_total") != std::string::npos,
              "AC6: typed match metric");
        CHECK(mb_src.find("MailKind") != std::string::npos, "AC6: MailKind in mailbox");
        CHECK(mb_src.find("correlation_id") != std::string::npos, "AC6: correlation_id field");
        CHECK(mb_src.find("2538") != std::string::npos, "AC6: #2538 cited in mailbox");
        CHECK(prim.find("schema-2538") != std::string::npos, "AC6: schema-2538 in metrics");
        CHECK(md.find("2538") != std::string::npos || md.find("typed") != std::string::npos,
              "AC6: README documents typed corr");
        CHECK(spawn_src.find("class AgentRegistry") == std::string::npos, "AC6: no AgentRegistry");
    }

    std::println("\n=== #2538 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_agent_ask_typed_corr();
}
#endif
