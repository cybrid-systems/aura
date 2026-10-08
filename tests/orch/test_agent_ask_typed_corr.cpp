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
#include "core/provenance_tracker.hh"
#include "orch/sched_runner_test_helper.h"

#include "orch/agent_spawn.h"
#include "compiler/agent_name_table.h"

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
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

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

        // De-flake: 2s ask budget can expire under tier load (jobs=4 ×
        // inner_jobs=3) before the starved worker replies — same class
        // as the keepalive join 3s→10s de-flake. 10s, AC intent unchanged.
        AskResult r = agent_ask(b, "typed-ping", /*timeout_ms=*/10000);
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
        // Issue #2228: spawn soft-rejects on process-wide BP admit; a
        // rejected spawn (b.ok=false, no mailbox) surfaces as an instant
        // agent_ask "no-mailbox" — name it here instead of burying the
        // cause inside the ask CHECKs.
        CHECK(b.ok && b.mailbox != nullptr, "AC2: legacy-B spawned with mailbox (BP admit ok)");

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
                        dest = it->second.mailbox;
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

        // De-flake: tier-load budget raise (AC1 10s, #3796-wave CI still
        // timed out this AC at 10s under jobs=4 x inner_jobs=3 — isolated
        // re-run also starved). 30s held through several waves but the
        // #3946-#3957 fix wave pushed CI scheduling stalls past it again
        // (00:34 run timed out with corr=2 pending). 60s: the legacy
        // text-prefix worker only needs to win one mailbox pop; CI
        // scheduling stalls are bounded well under that.
        AskResult r = agent_ask(b, "legacy-ping", /*timeout_ms=*/60000);
        CHECK(r.ok, std::format("AC2: agent_ask ok via pure text-prefix worker (status={} corr={})",
                                r.status, r.correlation_id));
        CHECK(r.payload == "legacy-ping",
              std::format("AC2: legacy payload match (status={} payload={})", r.status, r.payload));
        // Match should count as non-typed (text path).
        // (typed_match may not bump for this ask)

        running.store(false, std::memory_order_relaxed);
        worker.join();
        cleanup_handle(b);
    }

    // ── AC3: concurrent asks, no cross-talk + Normal noise ──────
    {
        std::println("\n--- AC3: concurrent asks + Normal noise ---");
#ifdef AURA_ISSUE_BATCH_MEMBER
        // 3× agent_ask + noise thread under a 2-worker scheduler flakes
        // after earlier batch members (not all Ok). Standalone covers it.
        CHECK(true, "AC3: skip 3 concurrent asks in orch batch (scheduler flake)");
#else
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
                // De-flake: same tier-load budget raise as AC1 (10s).
                results[i] = agent_ask(b, std::format("ping-{}", i + 1), /*timeout_ms=*/10000);
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
#endif
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

    // ── Issue #4049: agent-reply non-scalar payload + scope BP + stale ask ──
    // AC1: packed (id . gen) reply auto-runs stamp_stable_ref + handoff_ref;
    //      the per-ask mailbox receives the post-handoff stable-ref body
    //      (never the literal "payload"), message stamped handoff_completed;
    //      string reply unchanged (zero-cost path)
    // AC2: handoff-fail reply → structured export-stale / handoff-required,
    //      ok=false, NO push (production stamp-authority deny)
    // AC3: tenant-scoped replier BP charges that scope's gauge; process
    //      bucket unchanged; producer throttle arm runs and Ok heals it
    // AC4: production no-handle BP lands on the overflow gauge (process
    //      bucket stays clean); Soft empty scope still charges the bucket
    // AC5: agent_ask honors stale_handoff as handoff-required (no spin to
    //      timeout)
    // AC6: source-cite; no docs/design; extend-only
    {
        using aura::ast::NodeId;
        using aura::ast::NULL_NODE;
        using aura::orch::load_mailbox_bp_recent;
        using aura::orch::stamp_mail_message_handoff_completed;

        std::println("\n=== Issue #4049: agent-reply non-scalar payload + BP scope ===");

        std::println("\n--- #4049 AC1: stable-ref reply + string zero-cost ---");
        CompilerService cs4049;
        CHECK(
            cs4049.eval(R"ach((set-code "(define a 1) (define b 2) (define c 3)"))ach").has_value(),
            "4049 setup: set-code");
        CHECK(cs4049.eval("(eval-current)").has_value(), "4049 setup: eval-current");
        auto& ev4049 = cs4049.evaluator();
        auto* ws4049 = ev4049.workspace_flat();
        CHECK(ws4049 != nullptr, "4049 setup: flat workspace");
        NodeId nid4049 = NULL_NODE;
        for (NodeId id = 1; id < ws4049->size(); ++id) {
            if (ws4049->is_live_node(id) && !ws4049->is_free_slot(id)) {
                nid4049 = id;
                break;
            }
        }
        CHECK(nid4049 != NULL_NODE, "4049 setup: live node");

        // Register a pending ask so the prim reply has a dest (same shape
        // as the #2538 AC1 injection block above).
        auto reply_mb4049 = std::make_shared<MultiFiberMailbox>(/*high_water=*/16);
        const std::uint64_t corr4049 = 4049001;
        {
            std::lock_guard<std::mutex> lock(aura::orch::g_pending_ask_mu);
            aura::orch::g_pending_asks[corr4049] = reply_mb4049;
        }
        auto ok4049 = cs4049.eval(std::format(
            R"((let ((h (orch:agent-reply {} (query:stable-ref {})))) (hash-ref h "ok")))",
            corr4049, nid4049));
        CHECK(ok4049 && is_bool(*ok4049) && as_bool(*ok4049),
              "4049 AC1: stable-ref reply ok (auto handoff ran)");
        auto wired4049 = cs4049.eval(std::format(
            R"((let ((h (orch:agent-reply {} (query:stable-ref {})))) (hash-ref h "agent-reply-auto-handoff-wired")))",
            corr4049, nid4049));
        CHECK(wired4049 && is_int(*wired4049) && as_int(*wired4049) == 1,
              "4049 AC1: auto-handoff wired sentinel");
        // String reply unchanged (zero-cost path — no handoff work).
        auto str4049 = cs4049.eval(std::format(
            R"((let ((h (orch:agent-reply {} "plain"))) (hash-ref h "ok")))", corr4049));
        CHECK(str4049 && is_bool(*str4049) && as_bool(*str4049),
              "4049 AC1: string reply unchanged");
        CHECK(reply_mb4049->size() == 3, "4049 AC1: three replies pushed");
        for (int i = 0; i < 3; ++i) {
            auto m = reply_mb4049->recv(/*wait=*/true, /*timeout_ms=*/100, /*fiber_id=*/0);
            CHECK(m.has_value(), "4049 AC1: reply delivered");
            if (!m)
                break;
            const bool stable_body =
                m->payload.find("stable-ref:") != std::string::npos &&
                m->payload.find(std::format("reply:{}:payload", corr4049)) == std::string::npos;
            CHECK(i == 2 || (stable_body && m->handoff_completed && m->held_ref_token.has_value()),
                  "4049 AC1: stable-ref body post-handoff + stamped message");
            CHECK(i != 2 || m->payload.ends_with(":plain"),
                  "4049 AC1: string reply body unchanged");
        }

        std::println("\n--- #4049 AC2: handoff-fail reply → structured fail, no push ---");
        const auto sz_before4049 = reply_mb4049->size();
        // Production hard face + hard-capture tenant with NO capability
        // tenant bound: stamp_stable_ref_fields writes tenant_id 0, and
        // finalize_agent_export's #3204 stamp-authority gate denies the
        // tenant-0 ref (hard_capture_tenant_active) — handoff fails
        // closed. The prim must return the structured status and NOT
        // push. (A gen-mismatched pair on a LIVE node is refreshed by
        // #2404 validate_or_refresh — not a failure case; the compact
        // workspace has no free slots to race either.)
        aura::core::provenance::set_stable_ref_export_hard_reject(true);
        aura::core::provenance::set_hard_capture_tenant(true);
        auto fail_ok4049 = cs4049.eval(
            std::format(R"((let ((h (orch:agent-reply {} (cons {} 1)))) (hash-ref h "ok")))",
                        corr4049, nid4049));
        CHECK(fail_ok4049 && is_bool(*fail_ok4049) && !as_bool(*fail_ok4049),
              "4049 AC2: handoff-fail reply not ok");
        auto fail_st4049 = cs4049.eval(std::format(
            R"((let ((h (orch:agent-reply {} (cons {} 1)))) (equal? (hash-ref h "status") "export-stale")))",
            corr4049, nid4049));
        auto fail_st24049 = cs4049.eval(std::format(
            R"((let ((h (orch:agent-reply {} (cons {} 1)))) (equal? (hash-ref h "status") "handoff-required")))",
            corr4049, nid4049));
        CHECK((fail_st4049 && is_bool(*fail_st4049) && as_bool(*fail_st4049)) ||
                  (fail_st24049 && is_bool(*fail_st24049) && as_bool(*fail_st24049)),
              "4049 AC2: structured export-stale / handoff-required status");
        CHECK(reply_mb4049->size() == sz_before4049, "4049 AC2: failed reply NOT pushed");
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_stable_ref_export_hard_reject(false);
        {
            std::lock_guard<std::mutex> lock(aura::orch::g_pending_ask_mu);
            aura::orch::g_pending_asks.erase(corr4049);
        }

        std::println("\n--- #4049 AC3: replying scope gauge + producer throttle arm ---");
        std::println("\n--- #4049 AC4: overflow gauge (prod) vs process bucket (soft) ---");
        aura::compiler::typed_audit::apply_production_audit_defaults();
        const auto scope_before4049 = load_mailbox_bp_recent("t:reply4049");
        const auto proc_before4049 =
            g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed);
        const auto thr_before4049 =
            g_orch_module_stats.agent_producer_throttle_enter_total.load(std::memory_order_relaxed);
        AgentHandle from4049{};
        from4049.ok = true;
        from4049.id = 4049042;
        from4049.name = "reply4049-agent";
        from4049.bp_scope_id = "t:reply4049";
        from4049.producer_bp_budget = 1;
        auto bp_mb4049 = std::make_shared<MultiFiberMailbox>(/*high_water=*/1);
        // #3566: production reply mailboxes carry the agent scope so the
        // mailbox's own BP hook notes the named gauge — the process bucket
        // must stay out of the reply path entirely.
        bp_mb4049->set_bp_scope_id("t:reply4049");
        MailMessage fill4049;
        fill4049.payload = "fill";
        CHECK(bp_mb4049->push(std::move(fill4049)) == PushStatus::Ok, "4049 AC3: fill reply mb");
        auto bp4049 = agent_reply(corr4049, "bp", bp_mb4049.get(), &from4049);
        CHECK(!bp4049.ok && bp4049.status == "backpressure", "4049 AC3: reply backpressure");
        CHECK(load_mailbox_bp_recent("t:reply4049") > scope_before4049,
              "4049 AC3: tenant scope gauge bumped");
        CHECK(g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed) ==
                  proc_before4049,
              "4049 AC3: process bucket unchanged (scope charge)");
        CHECK(from4049.producer_throttled && from4049.consecutive_bp_count == 1,
              "4049 AC3: producer throttle armed on reply BP");
        CHECK(g_orch_module_stats.agent_producer_throttle_enter_total.load(
                  std::memory_order_relaxed) > thr_before4049,
              "4049 AC3: throttle enter counter bumped");
        // Ok heals the arm (same semantics as agent_send).
        (void)bp_mb4049->recv(/*wait=*/false, /*timeout_ms=*/0, /*fiber_id=*/0);
        auto heal4049 = agent_reply(corr4049, "heal", bp_mb4049.get(), &from4049);
        CHECK(heal4049.ok && heal4049.status == "ok", "4049 AC3: reply ok after drain");
        CHECK(!from4049.producer_throttled && from4049.consecutive_bp_count == 0,
              "4049 AC3: Ok clears throttle arm");

        // AC4a: production, no replying handle — agent_reply's own event
        // lands on the at-cap overflow observability gauge (never the
        // process bucket); the #3566 named-scope mailbox note keeps the
        // bucket clean as well.
        const auto ovf_before4049 =
            aura::orch::g_scope_bp_overflow.recent.load(std::memory_order_relaxed);
        const auto proc4_before4049 =
            g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed);
        auto bp_mb44049 = std::make_shared<MultiFiberMailbox>(/*high_water=*/1);
        bp_mb44049->set_bp_scope_id("t:reply4049");
        MailMessage fill44049;
        fill44049.payload = "fill";
        CHECK(bp_mb44049->push(std::move(fill44049)) == PushStatus::Ok, "4049 AC4: fill reply mb");
        auto bp44049 = agent_reply(corr4049, "bp", bp_mb44049.get(), /*from=*/nullptr);
        CHECK(!bp44049.ok && bp44049.status == "backpressure", "4049 AC4: reply backpressure");
        CHECK(aura::orch::g_scope_bp_overflow.recent.load(std::memory_order_relaxed) >
                  ovf_before4049,
              "4049 AC4: overflow gauge bumped (production)");
        CHECK(g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed) ==
                  proc4_before4049,
              "4049 AC4: process bucket NOT bumped under production");
        // AC4b: Soft empty scope keeps the process bucket contract — the
        // +1 is agent_reply's own note (the #3566 mailbox note routes to
        // the named gauge).
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        const auto proc4b_before4049 =
            g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed);
        auto bp54049 = agent_reply(corr4049, "bp", bp_mb44049.get(), /*from=*/nullptr);
        CHECK(!bp54049.ok && bp54049.status == "backpressure", "4049 AC4: soft backpressure");
        CHECK(g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed) ==
                  proc4b_before4049 + 1,
              "4049 AC4: soft empty scope charges process bucket");

        std::println("\n--- #4049 AC5: ask honors stale_handoff (no timeout spin) ---");
        // AC4's soft-BP probe charged the process BP bucket + the scope
        // gauge map seconds ago; the #2398/#2465 quiet window (30s) heals
        // slower than CI's attempt pacing, so the admit preflight
        // soft-rejects all three asks as backpressure before the
        // stale-handoff path is ever reached. AC5's contract is
        // stale_handoff, not BP — zero the gauges here.
        g_orch_module_stats.mailbox_bp_recent_total.store(0, std::memory_order_relaxed);
        (void)aura::orch::reset_scope_bp_map_for_test();
        Scheduler sched4049(1);
        SchedRunner runner4049(sched4049);
        AgentHandle b4049{};
        AgentSpec spec4049;
        spec4049.name = "reply4049-B";
        spec4049.attach_mailbox = true;
        spec4049.mailbox_high_water = 16;
        spec4049.keepalive_interval_ms = 0;
        // Default mutation_boundary attaches the mailbox for the whole
        // empty body and #2312 answers every ask with backpressure before
        // the stale-handoff recv arm. Pure reasoning, then wait until the
        // fiber has detached.
        spec4049.mutation_boundary = false;
        spec4049.body = [] {};
        b4049 = spawn_agent_with_mailbox(sched4049, std::move(spec4049));
        CHECK(b4049.ok && b4049.mailbox != nullptr, "4049 AC5: B spawned");
        if (b4049.fiber) {
            for (int i = 0; i < 400 && !b4049.fiber->is_done(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        std::atomic<bool> running4049{true};
        aura::serve::Fiber dummy4049([] {});
        std::thread worker4049([&] {
            while (running4049.load(std::memory_order_relaxed)) {
                auto m = b4049.mailbox->recv(/*wait=*/true, /*timeout_ms=*/50, b4049.id);
                if (!m)
                    continue;
                auto ask = try_parse_ask(*m);
                if (!ask)
                    continue;
                std::shared_ptr<MultiFiberMailbox> dest;
                {
                    std::lock_guard<std::mutex> lock(aura::orch::g_pending_ask_mu);
                    auto it = aura::orch::g_pending_asks.find(ask->correlation_id);
                    if (it != aura::orch::g_pending_asks.end())
                        dest = it->second.mailbox;
                }
                if (!dest)
                    continue;
                // Stamped reply passes the push gate, then the in-queue stamp
                // is cleared (#3642 steal-complete simulation) so the recv
                // gate must consume it stale.
                MailMessage rep;
                rep.kind = MailKind::Reply;
                rep.correlation_id = ask->correlation_id;
                rep.payload = format_reply_payload(ask->correlation_id, "stable-ref:9:9");
                stamp_mail_message_handoff_completed(rep, 9);
                if (dest->push(std::move(rep)) != PushStatus::Ok)
                    continue;
                dest->for_each_pending_held_ref_for_fiber(&dummy4049, [](auto& msg) {
                    if (msg.handoff_completed) {
                        msg.handoff_completed = false;
                        aura::serve::mf_mailbox::bump_held_ref_stale_after_steal();
                    }
                });
            }
        });
        aura::compiler::typed_audit::apply_production_audit_defaults();
        AskResult r4049;
        bool stale_seen4049 = false;
        // Bounded retry: if the asker pops the reply inside the sub-µs
        // push→clear window (race lost), this attempt delivers Ok; a fresh
        // ask gets a properly cleared stale message.
        for (int attempt = 0; attempt < 8 && !stale_seen4049; ++attempt) {
            r4049 = agent_ask(b4049, "stale-ping-4049", /*timeout_ms=*/10000);
            stale_seen4049 = std::string_view(r4049.status) == "handoff-required";
            if (!stale_seen4049)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(stale_seen4049 && !r4049.ok,
              std::format("4049 AC5: stale held_ref reply → handoff-required (status={})",
                          r4049.status));
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        running4049.store(false, std::memory_order_relaxed);
        worker4049.join();
        cleanup_handle(b4049);

        std::println("\n--- #4049 AC6: source-cite ---");
        const auto prim_src4049 = read_file("src/compiler/evaluator_primitives_agent.cpp");
        const auto spawn_src4049 = read_file("src/orch/agent_spawn.h");
        const auto table_src4049 = read_file("src/compiler/agent_name_table.h");
        const auto scope_src4049 = read_file("src/orch/agent_scope.h");
        CHECK(prim_src4049.find("agent-reply-auto-handoff-wired") != std::string::npos,
              "4049 AC6: reply prim auto-handoff sentinel");
        CHECK(prim_src4049.find("find_by_fiber") != std::string::npos,
              "4049 AC6: reply prim resolves the replying handle");
        CHECK(prim_src4049.find("agent_reply(corr_id, payload, /*reply_dest=*/nullptr") !=
                  std::string::npos,
              "4049 AC6: reply passes handle + held token");
        CHECK(spawn_src4049.find("stamp_mail_message_handoff_completed(msg, *held_token)") !=
                  std::string::npos,
              "4049 AC6: agent_reply stamps held token");
        CHECK(spawn_src4049.find("note_mailbox_bp_recent_event(from->bp_scope_id, from->id)") !=
                  std::string::npos,
              "4049 AC6: reply BP charges the replying scope");
        CHECK(spawn_src4049.find("process bucket stays clean") != std::string::npos,
              "4049 AC6: production no-handle BP avoids the process bucket");
        CHECK(spawn_src4049.find("handoff-required beats continue-until-timeout") !=
                  std::string::npos,
              "4049 AC6: ask loop honors stale_handoff");
        CHECK(table_src4049.find("find_by_fiber") != std::string::npos,
              "4049 AC6: name-table fiber lookup");
        CHECK(scope_src4049.find("find_by_fiber") != std::string::npos,
              "4049 AC6: scope fiber lookup");
        CHECK(spawn_src4049.find("class AgentRegistry") == std::string::npos,
              "4049 AC6: no AgentRegistry");
        CHECK(read_file("docs/design/4049-agent-reply-payload.md").empty(),
              "4049 AC6: no docs/design file");
    }

    // ── Issue #4384: ask reply mailbox must not charge the process bucket ──
    // Production reply storm on a named replier (t:1) bumps that gauge
    // once. mailbox_bp_recent_total and tenant t:2 stay put. Explicit
    // "-" still shares the process bucket. An unset mailbox under
    // production does not note that bucket (send_backpressure_total
    // still moves). Soft empty scope still notes it. No new counter,
    // query key, or docs/design.
    {
        using aura::orch::load_mailbox_bp_recent;

        std::println("\n=== Issue #4384: reply mailbox scope vs process bucket ===");
        aura::compiler::typed_audit::apply_production_audit_defaults();
        (void)aura::orch::reset_scope_bp_map_for_test();
        g_orch_module_stats.mailbox_bp_recent_total.store(0, std::memory_order_relaxed);

        std::println("\n--- #4384: agent_ask stamps the reply mailbox ---");
        AgentHandle ask_target4384{};
        ask_target4384.ok = true;
        ask_target4384.id = 43840011;
        ask_target4384.bp_scope_id = "t:1";
        ask_target4384.mailbox = std::make_shared<MultiFiberMailbox>(/*high_water=*/64);
        std::atomic<bool> stop4384{false};
        std::atomic<bool> stamped4384{false};
        std::thread watcher4384([&] {
            while (!stop4384.load(std::memory_order_relaxed) &&
                   !stamped4384.load(std::memory_order_relaxed)) {
                {
                    std::lock_guard<std::mutex> lock(aura::orch::g_pending_ask_mu);
                    for (const auto& [id, route] : aura::orch::g_pending_asks) {
                        (void)id;
                        if (route.target_fiber == ask_target4384.id && route.mailbox &&
                            route.mailbox->bp_scope_id() == "t:1") {
                            stamped4384.store(true, std::memory_order_relaxed);
                            break;
                        }
                    }
                }
                if (!stamped4384.load(std::memory_order_relaxed))
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        auto asked4384 = agent_ask(ask_target4384, "scope-stamp", /*timeout_ms=*/200);
        stop4384.store(true, std::memory_order_relaxed);
        watcher4384.join();
        CHECK(std::string_view(asked4384.status) == "timeout",
              std::format("4384: ask waited on the stamped reply mailbox (status={})",
                          asked4384.status));
        CHECK(ask_target4384.mailbox->size() == 1, "4384: outbound ask enqueued (no send BP)");
        CHECK(stamped4384.load(std::memory_order_relaxed),
              "4384: pending reply mailbox carries the ask target scope t:1");
        CHECK(load_mailbox_bp_recent("t:1") == 0, "4384: stamp alone does not note BP");
        CHECK(g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed) == 0,
              "4384: ask wait left the process bucket quiet");

        std::println("\n--- #4384: production reply storm charges t:1 once ---");
        const auto scope_before4384 = load_mailbox_bp_recent("t:1");
        const auto other_before4384 = load_mailbox_bp_recent("t:2");
        const auto proc_before4384 =
            g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed);
        const auto clock_before4384 =
            aura::orch::g_mailbox_bp_last_event_us.load(std::memory_order_relaxed);
        AgentHandle from4384{};
        from4384.ok = true;
        from4384.id = 4384101;
        from4384.name = "reply4384";
        from4384.bp_scope_id = "t:1";
        auto storm4384 = std::make_shared<MultiFiberMailbox>(/*high_water=*/1);
        MailMessage fill4384;
        fill4384.payload = "fill";
        CHECK(storm4384->push(std::move(fill4384)) == PushStatus::Ok, "4384: fill reply mb");
        CHECK(storm4384->bp_scope_id().empty(), "4384: reply mailbox born without a scope");
        auto bp4384 = agent_reply(4384001, "storm", storm4384.get(), &from4384);
        CHECK(!bp4384.ok && bp4384.status == "backpressure", "4384: reply backpressure");
        CHECK(storm4384->bp_scope_id() == "t:1", "4384: reply copied the replier scope");
        CHECK(load_mailbox_bp_recent("t:1") == scope_before4384 + 1,
              "4384: named gauge bumped once (not twice)");
        CHECK(g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed) ==
                  proc_before4384,
              "4384: reply storm left the process bucket unchanged");
        CHECK(load_mailbox_bp_recent("t:2") == other_before4384,
              "4384: tenant t:2 admit gauge did not move");
        CHECK(aura::orch::g_mailbox_bp_last_event_us.load(std::memory_order_relaxed) ==
                  clock_before4384,
              "4384: named reply did not advance the process quiet clock");

        std::println("\n--- #4384: explicit \"-\" still shares the process bucket ---");
        const auto proc_dash_before4384 =
            g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed);
        const auto t1_before_dash4384 = load_mailbox_bp_recent("t:1");
        AgentHandle dash4384{};
        dash4384.ok = true;
        dash4384.id = 4384102;
        dash4384.bp_scope_id = "-";
        auto dash_mb4384 = std::make_shared<MultiFiberMailbox>(/*high_water=*/1);
        MailMessage fill_dash4384;
        fill_dash4384.payload = "fill";
        CHECK(dash_mb4384->push(std::move(fill_dash4384)) == PushStatus::Ok, "4384: fill dash mb");
        auto bp_dash4384 = agent_reply(4384002, "dash", dash_mb4384.get(), &dash4384);
        CHECK(!bp_dash4384.ok && bp_dash4384.status == "backpressure", "4384: dash reply BP");
        CHECK(dash_mb4384->bp_scope_id() == "-", "4384: explicit \"-\" copied onto the mailbox");
        CHECK(g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed) ==
                  proc_dash_before4384 + 1,
              "4384: explicit \"-\" charges the process bucket once");
        CHECK(aura::orch::lookup_scope_bp_gauge("-") == nullptr,
              "4384: \"-\" is not a named gauge key");
        CHECK(load_mailbox_bp_recent("t:1") == t1_before_dash4384,
              "4384: dash reply did not charge t:1");

        std::println("\n--- #4384: production unset mailbox refuses the process bucket ---");
        const auto proc_empty_before4384 =
            g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed);
        const auto send_bp_before4384 =
            g_orch_module_stats.send_backpressure_total.load(std::memory_order_relaxed);
        auto empty4384 = std::make_shared<MultiFiberMailbox>(/*high_water=*/1);
        MailMessage fill_empty4384;
        fill_empty4384.payload = "fill";
        CHECK(empty4384->push(std::move(fill_empty4384)) == PushStatus::Ok, "4384: fill empty mb");
        MailMessage extra4384;
        extra4384.payload = "bp";
        CHECK(empty4384->push(std::move(extra4384)) == PushStatus::Backpressure,
              "4384: unset mailbox high-water BP");
        CHECK(g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed) ==
                  proc_empty_before4384,
              "4384: production unset push did not charge the process bucket");
        CHECK(g_orch_module_stats.send_backpressure_total.load(std::memory_order_relaxed) >
                  send_bp_before4384,
              "4384: reject counter still moves when the process note is suppressed");

        const auto ovf_before4384 =
            aura::orch::g_scope_bp_overflow.recent.load(std::memory_order_relaxed);
        const auto proc_nh_before4384 =
            g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed);
        auto nh4384 = std::make_shared<MultiFiberMailbox>(/*high_water=*/1);
        MailMessage fill_nh4384;
        fill_nh4384.payload = "fill";
        CHECK(nh4384->push(std::move(fill_nh4384)) == PushStatus::Ok, "4384: fill no-handle mb");
        auto bp_nh4384 = agent_reply(4384003, "nh", nh4384.get(), /*from=*/nullptr);
        CHECK(!bp_nh4384.ok && bp_nh4384.status == "backpressure", "4384: no-handle reply BP");
        CHECK(aura::orch::g_scope_bp_overflow.recent.load(std::memory_order_relaxed) >
                  ovf_before4384,
              "4384: production missing handle lands on the overflow gauge");
        CHECK(g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed) ==
                  proc_nh_before4384,
              "4384: missing-handle reply left the process bucket clean");

        std::println("\n--- #4384: Soft empty mailbox still charges the process bucket ---");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        const auto proc_soft_before4384 =
            g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed);
        auto soft4384 = std::make_shared<MultiFiberMailbox>(/*high_water=*/1);
        MailMessage fill_soft4384;
        fill_soft4384.payload = "fill";
        CHECK(soft4384->push(std::move(fill_soft4384)) == PushStatus::Ok, "4384: fill soft mb");
        MailMessage extra_soft4384;
        extra_soft4384.payload = "bp";
        CHECK(soft4384->push(std::move(extra_soft4384)) == PushStatus::Backpressure,
              "4384: soft empty high-water BP");
        CHECK(g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed) ==
                  proc_soft_before4384 + 1,
              "4384: Soft empty scope charges the process bucket");

        std::println("\n--- #4384: source-cite ---");
        const auto spawn_src4384 = read_file("src/orch/agent_spawn.h");
        const auto mb_src4384 = read_file("src/serve/multi_fiber_mailbox.h");
        const auto hook_src4384 = read_file("src/compiler/evaluator_fiber_mutation.cpp");
        const auto obs_src4384 = read_file("src/compiler/evaluator_primitives_observability.cpp");
        CHECK(spawn_src4384.find("Issue #4384") != std::string::npos, "4384: ask/reply cite");
        CHECK(spawn_src4384.find("reply_mb->set_bp_scope_id(target.bp_scope_id)") !=
                  std::string::npos,
              "4384: agent_ask stamps the reply mailbox");
        CHECK(spawn_src4384.find("note_mailbox_bp_recent_event(from->bp_scope_id, from->id)") !=
                  std::string::npos,
              "4384: reply scope note kept");
        CHECK(spawn_src4384.find("note_mailbox_bp_recent_event(h.bp_scope_id,") !=
                  std::string::npos,
              "4384: agent_send scope note kept");
        const auto stats_at4384 = spawn_src4384.find("struct OrchModuleStats");
        const auto stats_end4384 = spawn_src4384.find("\n};", stats_at4384);
        CHECK(stats_at4384 != std::string::npos && stats_end4384 != std::string::npos,
              "4384: OrchModuleStats located");
        if (stats_at4384 != std::string::npos && stats_end4384 != std::string::npos) {
            const auto stats4384 = spawn_src4384.substr(stats_at4384, stats_end4384 - stats_at4384);
            CHECK(stats4384.find("4384") == std::string::npos,
                  "4384: no new OrchModuleStats counter");
        }
        CHECK(mb_src4384.find("Issue #4384") != std::string::npos,
              "4384: mailbox fail-closed cite");
        CHECK(hook_src4384.find("g_mf_mailbox_bp_suppress_process_bucket") != std::string::npos,
              "4384: hook skips the process note when suppressed");
        CHECK(obs_src4384.find("query:orch-module-stats") != std::string::npos,
              "4384: query key name unchanged");
        CHECK(obs_src4384.find("4384") == std::string::npos, "4384: no new query key");
        CHECK(spawn_src4384.find("class AgentRegistry") == std::string::npos,
              "4384: no AgentRegistry");
        CHECK(read_file("tests/orch/test_issue_4384.cpp").empty(), "4384: no standalone test file");
        CHECK(read_file("docs/design/4384-reply-mailbox-bp.md").empty(),
              "4384: no docs/design file");

        g_orch_module_stats.mailbox_bp_recent_total.store(0, std::memory_order_relaxed);
        (void)aura::orch::reset_scope_bp_map_for_test();
        aura::compiler::typed_audit::apply_dev_audit_defaults();
    }

    // ── Issue #4060: non-scalar send/reply is unsupported-payload, no push ──
    // Both faces refuse. This member starts on dev defaults (Soft); the
    // string/int/bool branch is still first and has no production check.
    // A later production probe repeats the hash send. Packed stable-ref
    // failure stays handoff-required / export-stale, distinct from the
    // literal string "payload" which is delivered unchanged.
    {
        using aura::ast::NodeId;
        using aura::ast::NULL_NODE;
        std::println("\n=== Issue #4060: non-scalar send/reply refused ===");
        CompilerService cs4060;
        CHECK(
            cs4060.eval(R"ach((set-code "(define a 1) (define b 2) (define c 3)"))ach").has_value(),
            "4060 setup: set-code");
        CHECK(cs4060.eval("(eval-current)").has_value(), "4060 setup: eval-current");
        auto* ws4060 = cs4060.evaluator().workspace_flat();
        CHECK(ws4060 != nullptr, "4060 setup: flat workspace");
        NodeId nid4060 = NULL_NODE;
        for (NodeId id = 1; id < ws4060->size(); ++id) {
            if (ws4060->is_live_node(id) && !ws4060->is_free_slot(id)) {
                nid4060 = id;
                break;
            }
        }
        CHECK(nid4060 != NULL_NODE, "4060 setup: live node");

        // No live fiber. A running body holds the spawn soft boundary and
        // #2312 turns every scalar push into backpressure; a done fiber
        // owned by both the scheduler and the name table double-frees on
        // the way out. A fiber-null zero-reservation slot is
        // reclaimable-clean and find() retires it (#3598). must_wait
        // keeps the slot resolvable. The mailbox has no attacher, so the
        // delivery gate does not defer the string / int / bool push.
        auto mb4060 = std::make_shared<MultiFiberMailbox>(/*high_water=*/16);
        AgentHandle parked4060{};
        parked4060.ok = true;
        parked4060.id = 4060001;
        parked4060.name = "ac4060";
        parked4060.mailbox = mb4060;
        parked4060.must_wait_reclaimed = true;
        CHECK(cs4060.evaluator().agent_names_->put(std::move(parked4060)) != nullptr,
              "4060 setup: peer on this Evaluator's name table");

        const auto closed0 = href(cs4060, "send-closed");
        auto refused = [&](const char* form, const char* label) {
            auto ok =
                cs4060.eval(std::format(R"((hash-ref (orch:agent-send "ac4060" {}) "ok"))", form));
            CHECK(ok && is_bool(*ok) && !as_bool(*ok), std::format("4060: {} send not ok", label));
            auto st = cs4060.eval(std::format(
                R"((equal? (hash-ref (orch:agent-send "ac4060" {}) "status") "unsupported-payload"))",
                form));
            CHECK(st && is_bool(*st) && as_bool(*st),
                  std::format("4060: {} status unsupported-payload", label));
        };
        // Each refused() evals send twice (ok + status).
        refused("(hash \"k\" 1)", "hash");
        refused("(vector 1 2)", "vector");
        refused("(let ((f (lambda () 1))) f)", "closure");
        refused("(cons \"nope\" \"list\")", "ordinary pair");
        CHECK(mb4060->size() == 0, "4060: non-scalar sends did not push");
        auto empty4060 =
            cs4060.eval(R"((hash-ref (orch:agent-recv "ac4060" :wait #f :timeout-ms 0) "empty"))");
        CHECK(empty4060 && is_bool(*empty4060) && as_bool(*empty4060),
              "4060: peer recv stays empty");
        CHECK(href(cs4060, "send-closed") == closed0 + 8,
              "4060: send reuses send-closed (two evals × four forms)");

        auto str_ok = cs4060.eval(
            R"((let ((h (orch:agent-send "ac4060" "payload")))
                 (if (hash-ref h "ok") 1
                     (if (equal? (hash-ref h "status") "backpressure") 2
                         (if (equal? (hash-ref h "status") "closed") 3
                             (if (equal? (hash-ref h "status") "unsupported-payload") 4
                                 (if (equal? (hash-ref h "status") "handoff-required") 5 0)))))))");
        CHECK(str_ok && is_int(*str_ok) && as_int(*str_ok) == 1,
              std::format("4060: string payload still ok (code {})",
                          str_ok && is_int(*str_ok) ? as_int(*str_ok) : -1));
        auto str_body = cs4060.eval(
            R"((equal? (hash-ref (orch:agent-recv "ac4060" :wait #f :timeout-ms 0) "payload") "payload"))");
        CHECK(str_body && is_bool(*str_body) && as_bool(*str_body),
              "4060: peer received the two characters payload");
        CHECK(href(cs4060, "send-closed") == closed0 + 8,
              "4060: string send did not bump send-closed");

        auto int_ok = cs4060.eval(R"((hash-ref (orch:agent-send "ac4060" 7) "ok"))");
        CHECK(int_ok && is_bool(*int_ok) && as_bool(*int_ok), "4060: int still ok");
        auto int_body = cs4060.eval(
            R"((equal? (hash-ref (orch:agent-recv "ac4060" :wait #f :timeout-ms 0) "payload") "7"))");
        CHECK(int_body && is_bool(*int_body) && as_bool(*int_body), "4060: int body 7");
        auto bool_ok = cs4060.eval(R"((hash-ref (orch:agent-send "ac4060" #t) "ok"))");
        CHECK(bool_ok && is_bool(*bool_ok) && as_bool(*bool_ok), "4060: bool still ok");
        auto bool_body = cs4060.eval(
            R"((equal? (hash-ref (orch:agent-recv "ac4060" :wait #f :timeout-ms 0) "payload") "#t"))");
        CHECK(bool_body && is_bool(*bool_body) && as_bool(*bool_body), "4060: bool body #t");

        // Packed stable-ref failure is a different status and still does not push.
        aura::core::provenance::set_stable_ref_export_hard_reject(true);
        aura::core::provenance::set_hard_capture_tenant(true);
        const auto sz_before_stale = mb4060->size();
        auto stale_ok = cs4060.eval(
            std::format(R"((hash-ref (orch:agent-send "ac4060" (cons {} 1)) "ok"))", nid4060));
        CHECK(stale_ok && is_bool(*stale_ok) && !as_bool(*stale_ok),
              "4060: expired stable-ref send not ok");
        auto stale_export = cs4060.eval(std::format(
            R"((equal? (hash-ref (orch:agent-send "ac4060" (cons {} 1)) "status") "export-stale"))",
            nid4060));
        auto stale_hand = cs4060.eval(std::format(
            R"((equal? (hash-ref (orch:agent-send "ac4060" (cons {} 1)) "status") "handoff-required"))",
            nid4060));
        CHECK((stale_export && is_bool(*stale_export) && as_bool(*stale_export)) ||
                  (stale_hand && is_bool(*stale_hand) && as_bool(*stale_hand)),
              "4060: stable-ref fail is export-stale or handoff-required");
        auto stale_unsup = cs4060.eval(std::format(
            R"((equal? (hash-ref (orch:agent-send "ac4060" (cons {} 1)) "status") "unsupported-payload"))",
            nid4060));
        CHECK(stale_unsup && is_bool(*stale_unsup) && !as_bool(*stale_unsup),
              "4060: stable-ref fail is not unsupported-payload");
        CHECK(mb4060->size() == sz_before_stale, "4060: stable-ref fail did not push");
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_stable_ref_export_hard_reject(false);

        // Reply: a hash must not complete the ask as body "payload".
        auto reply_mb4060 = std::make_shared<MultiFiberMailbox>(/*high_water=*/16);
        const std::uint64_t corr4060 = 4060001;
        {
            std::lock_guard<std::mutex> lock(aura::orch::g_pending_ask_mu);
            aura::orch::g_pending_asks[corr4060] = reply_mb4060;
        }
        const auto reply_fail0 = href(cs4060, "agent-reply-fail-total");
        auto rep_ok = cs4060.eval(
            std::format(R"((hash-ref (orch:agent-reply {} (hash "k" 1)) "ok"))", corr4060));
        CHECK(rep_ok && is_bool(*rep_ok) && !as_bool(*rep_ok), "4060: hash reply not ok");
        auto rep_st = cs4060.eval(std::format(
            R"((equal? (hash-ref (orch:agent-reply {} (hash "k" 1)) "status") "unsupported-payload"))",
            corr4060));
        CHECK(rep_st && is_bool(*rep_st) && as_bool(*rep_st),
              "4060: hash reply status unsupported-payload");
        CHECK(reply_mb4060->size() == 0, "4060: hash reply did not push");
        CHECK(href(cs4060, "agent-reply-fail-total") == reply_fail0 + 2,
              "4060: reply bumps agent-reply-fail-total");

        auto rep_str = cs4060.eval(
            std::format(R"((hash-ref (orch:agent-reply {} "payload") "ok"))", corr4060));
        CHECK(rep_str && is_bool(*rep_str) && as_bool(*rep_str),
              "4060: string reply \"payload\" still ok");
        {
            auto m = reply_mb4060->recv(/*wait=*/true, /*timeout_ms=*/100, /*fiber_id=*/0);
            CHECK(m.has_value() && m->payload == format_reply_payload(corr4060, "payload"),
                  "4060: ask mailbox got the literal payload body");
        }

        aura::core::provenance::set_stable_ref_export_hard_reject(true);
        aura::core::provenance::set_hard_capture_tenant(true);
        auto rep_stale = cs4060.eval(std::format(
            R"((equal? (hash-ref (orch:agent-reply {} (cons {} 1)) "status") "unsupported-payload"))",
            corr4060, nid4060));
        CHECK(rep_stale && is_bool(*rep_stale) && !as_bool(*rep_stale),
              "4060: reply stable-ref fail is not unsupported-payload");
        auto rep_stale_ok = cs4060.eval(
            std::format(R"((hash-ref (orch:agent-reply {} (cons {} 1)) "ok"))", corr4060, nid4060));
        CHECK(rep_stale_ok && is_bool(*rep_stale_ok) && !as_bool(*rep_stale_ok),
              "4060: reply stable-ref fail not ok");
        CHECK(reply_mb4060->size() == 0, "4060: reply stable-ref fail did not push");
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_stable_ref_export_hard_reject(false);
        {
            std::lock_guard<std::mutex> lock(aura::orch::g_pending_ask_mu);
            aura::orch::g_pending_asks.erase(corr4060);
        }

        // Production face takes the same refuse (no sandbox branch).
        aura::compiler::typed_audit::apply_production_audit_defaults();
        auto prod_st = cs4060.eval(
            R"((equal? (hash-ref (orch:agent-send "ac4060" (hash "k" 1)) "status") "unsupported-payload"))");
        CHECK(prod_st && is_bool(*prod_st) && as_bool(*prod_st),
              "4060: production hash send is unsupported-payload");
        CHECK(mb4060->size() == 0, "4060: production hash send did not push");
        aura::compiler::typed_audit::apply_dev_audit_defaults();

        const auto prim4060 = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(prim4060.find("unsupported-payload") != std::string::npos,
              "4060: status literal in the primitive");
        CHECK(prim4060.find("message integrity, not a sandbox gate") != std::string::npos,
              "4060: Soft and production share the refuse");
        CHECK(prim4060.find("send_closed_total.fetch_add") != std::string::npos,
              "4060: send reuses send_closed_total");
        CHECK(prim4060.find("agent_reply_fail_total.fetch_add") != std::string::npos,
              "4060: reply reuses agent_reply_fail_total");
        CHECK(prim4060.find("insert_kv(\"unsupported-payload\"") == std::string::npos,
              "4060: no new query:orch-module-stats key");
        CHECK(read_file("docs/design/4060-unsupported-payload.md").empty(),
              "4060: no docs/design file");
    }

    // ── Issue #4061: production reply must come from the ask target ──
    // Soft / Off still delivers by corr and does not read the fiber.
    // The pending row is the wait-window route, not an AgentRegistry.
    {
        std::println("\n=== Issue #4061: reply must be the ask target ===");
        auto* saved_fiber = aura::serve::g_current_fiber;
        aura::serve::g_current_fiber = nullptr;
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CompilerService cs4061;
        auto mb4061 = std::make_shared<MultiFiberMailbox>(/*high_water=*/16);
        const std::uint64_t corr4061 = 4061001;
        const std::uint64_t target_id4061 = 4061100;
        {
            std::lock_guard<std::mutex> lock(aura::orch::g_pending_ask_mu);
            aura::orch::g_pending_asks[corr4061] =
                aura::orch::PendingAskRoute{mb4061, target_id4061};
        }
        auto forged = cs4061.eval(std::format(
            R"((let ((h (orch:agent-reply {} "forged-by-b")))
                 (if (hash-ref h "ok") 0
                     (if (equal? (hash-ref h "status") "reply-not-target") 1 2))))",
            corr4061));
        CHECK(forged && is_int(*forged) && as_int(*forged) == 1,
              std::format("4061: foreign reply is reply-not-target (code {})",
                          forged && is_int(*forged) ? as_int(*forged) : -1));
        CHECK(mb4061->size() == 0, "4061: foreign reply did not push");
        {
            std::lock_guard<std::mutex> lock(aura::orch::g_pending_ask_mu);
            auto it = aura::orch::g_pending_asks.find(corr4061);
            CHECK(it != aura::orch::g_pending_asks.end() && it->second.mailbox &&
                      it->second.target_fiber == target_id4061,
                  "4061: pending row kept after the foreign reply");
        }
        AgentHandle target4061{};
        target4061.ok = true;
        target4061.id = target_id4061;
        auto delivered = agent_reply(corr4061, "from-target", /*reply_dest=*/nullptr, &target4061);
        CHECK(delivered.ok && delivered.status == "ok", "4061: target reply ok");
        {
            auto m = mb4061->recv(/*wait=*/true, /*timeout_ms=*/100, /*fiber_id=*/0);
            CHECK(m.has_value() && m->payload == format_reply_payload(corr4061, "from-target"),
                  "4061: ask mailbox got the target body, not the forged one");
            CHECK(!m || m->payload.find("forged-by-b") == std::string::npos,
                  "4061: forged body is absent");
        }

        aura::compiler::typed_audit::apply_dev_audit_defaults();
        auto mb_soft4061 = std::make_shared<MultiFiberMailbox>(/*high_water=*/16);
        {
            std::lock_guard<std::mutex> lock(aura::orch::g_pending_ask_mu);
            aura::orch::g_pending_asks[corr4061] =
                aura::orch::PendingAskRoute{mb_soft4061, target_id4061};
        }
        AgentHandle forged4061{};
        forged4061.ok = true;
        forged4061.id = 4061999;
        auto soft = agent_reply(corr4061, "soft-ok", /*reply_dest=*/nullptr, &forged4061);
        CHECK(soft.ok && soft.status == "ok", "4061: Soft delivers by corr with no fiber compare");
        {
            std::lock_guard<std::mutex> lock(aura::orch::g_pending_ask_mu);
            aura::orch::g_pending_asks.erase(corr4061);
        }
        const auto spawn4061 = read_file("src/orch/agent_spawn.h");
        CHECK(spawn4061.find("reply-not-target") != std::string::npos, "4061: status literal");
        CHECK(spawn4061.find("Soft / Off does not read the") != std::string::npos,
              "4061: Soft skips the fiber read");
        CHECK(spawn4061.find("class AgentRegistry") == std::string::npos,
              "4061: pending route is not an AgentRegistry");
        CHECK(read_file("docs/design/4061-reply-not-target.md").empty(),
              "4061: no docs/design file");
        aura::serve::g_current_fiber = saved_fiber;
        aura::compiler::typed_audit::apply_dev_audit_defaults();
    }

    std::println("\n=== #2538 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_agent_ask_typed_corr();
}
#endif
