// @category: unit
// @reason: Issue #2158 — per-Evaluator agent apply mutex (replace process-static
// orch_eval_mu on orch:spawn-agent apply_closure path).
//
//   AC1: No process-static mutex on orch spawn apply path (grep clean).
//   AC2: Two Evaluators concurrent hold ≈ max(T1,T2) not T1+T2.
//   AC3: Single Evaluator multi-agent still serialized for apply.
//   AC4: Aura spawn/join + query schema-2158 surface green.
//   AC5: try_acquire reject skips body (apply lock not required for reject).

#include "test_harness.hpp"

#include "compiler/typed_mutation_audit.h"
#include "orch/agent_scope.h"
#include "orch/agent_spawn.h"
#include "serve/fiber.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
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
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::orch::AgentHandle;
using aura::orch::AgentSpec;
using aura::orch::g_orch_module_stats;
using aura::orch::join_agent;
using aura::orch::kAgentApplyPerEvalMutexIssue;
using aura::orch::spawn_agent_with_mailbox;
using aura::serve::Fiber;
using aura::serve::JoinStatus;
using aura::serve::Scheduler;
using aura::serve::YieldReason;
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
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// `body` runs on this thread after `mu` is already held by a side thread.
// Returns true when `body` finished before that hold ended. A parallel-intend
// that waits on `mu` returns false; one that skips the mutex returns true.
template <class F> bool finished_while_mutex_held(std::mutex& mu, int hold_ms, F&& body) {
    std::atomic<int> holding{0};
    std::atomic<int> done{0};
    std::atomic<int> finished_while_held{-1};
    std::thread holder([&] {
        std::lock_guard<std::mutex> lock(mu);
        holding.store(1, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
        finished_while_held.store(done.load(std::memory_order_acquire), std::memory_order_release);
    });
    const auto arm = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (holding.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < arm) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    body();
    done.store(1, std::memory_order_release);
    holder.join();
    return finished_while_held.load(std::memory_order_acquire) == 1;
}

// `body` runs on this thread after `key` is claimed in spawn_region_inflight_
// (region-fast, no agent_apply_mu_). Returns true when `body` finished
// before that claim ended. A supervise closure on the same key waits
// inside gate_spawn_apply_region and returns false.
template <class Ev, class F>
bool finished_while_region_held(Ev& ev, std::uint64_t key, int hold_ms, F&& body) {
    std::atomic<int> holding{0};
    std::atomic<int> done{0};
    std::atomic<int> finished_while_held{-1};
    std::thread holder([&] {
        ev.gate_spawn_apply_region(
            key, /*region_fast=*/true,
            [&] {
                holding.store(1, std::memory_order_release);
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(hold_ms);
                while (std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                finished_while_held.store(done.load(std::memory_order_acquire),
                                          std::memory_order_release);
            },
            [](std::uint64_t) {});
    });
    const auto arm = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (holding.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < arm) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    body();
    done.store(1, std::memory_order_release);
    holder.join();
    return finished_while_held.load(std::memory_order_acquire) == 1;
}

// Hold agent_apply_mu_ for `hold_ms` (simulates apply_closure under the gate).
// inflight/peak, when set, record how many holders overlapped. Distinct
// per-Evaluator mutexes reach 2; one shared mutex stays at 1. Wall-clock
// slack is not that oracle: CI stretches a 120ms sleep past kHoldMs*2-40
// while the two sections still overlap.
void hold_apply_mu(std::mutex& mu, int hold_ms, std::atomic<int>* inflight = nullptr,
                   std::atomic<int>* peak = nullptr) {
    std::lock_guard lock(mu);
    if (inflight != nullptr && peak != nullptr) {
        const int now = inflight->fetch_add(1, std::memory_order_acq_rel) + 1;
        int seen = peak->load(std::memory_order_relaxed);
        while (now > seen && !peak->compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    if (inflight != nullptr)
        inflight->fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace

// Issue #3586: Scheduler(N>1).run() aborts unless production is latched or
// AURA_SANDBOX=off. This member's dual-Scheduler ACs are unarmed unit
// protocol paths — run the member sandbox=off and restore the inherited
// face on exit (batch-forked and standalone share this entry).
struct MemberSandboxOff {
    std::string prev;
    bool had = false;
    MemberSandboxOff() {
        if (const char* e = std::getenv("AURA_SANDBOX")) {
            had = true;
            prev = e;
        }
        ::setenv("AURA_SANDBOX", "off", 1);
    }
    ~MemberSandboxOff() {
        if (had)
            ::setenv("AURA_SANDBOX", prev.c_str(), 1);
        else
            ::unsetenv("AURA_SANDBOX");
    }
};

int run_test_agent_apply_mutex() {
    MemberSandboxOff member_sandbox_off; // #3586: unarmed Scheduler(2) needs the escape
    std::println("=== Issue #2158: per-Evaluator agent apply mutex ===");
    CHECK(kAgentApplyPerEvalMutexIssue == 2158, "issue stamp");

    // ── AC1: source contract — no process-static orch_eval_mu ──
    {
        std::println("\n--- AC1: grep clean + agent_apply_mu_ ---");
        const auto agent_src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(!agent_src.empty(), "agent primitives readable");
        // AC1: no process-static apply mutex declaration (comments may mention
        // the old name when documenting the migration).
        CHECK(agent_src.find("static std::mutex orch_eval_mu") == std::string::npos,
              "AC1: no process-static orch_eval_mu declaration");
        CHECK(agent_src.find("lock_guard lock(orch_eval_mu)") == std::string::npos &&
                  agent_src.find("lock(orch_eval_mu)") == std::string::npos,
              "AC1: no lock on process-static orch_eval_mu");
        CHECK(agent_src.find("ev.agent_apply_mu_") != std::string::npos,
              "AC1: spawn body locks ev.agent_apply_mu_");
        CHECK(agent_src.find("2158") != std::string::npos, "AC1: agent TU cites 2158");

        const auto ev_src = read_file("src/compiler/evaluator.ixx");
        CHECK(ev_src.find("agent_apply_mu_") != std::string::npos,
              "AC1: Evaluator has agent_apply_mu_");
        CHECK(ev_src.find("2158") != std::string::npos, "AC1: evaluator.ixx cites 2158");

        const auto spawn_src = read_file("src/orch/agent_spawn.h");
        CHECK(spawn_src.find("kAgentApplyPerEvalMutexIssue") != std::string::npos,
              "AC1: issue constant in agent_spawn.h");
        CHECK(spawn_src.find("agent_apply_lock_acquisitions_total") != std::string::npos,
              "AC1: acquire metric declared");
    }

    // ── AC2: two Evaluators concurrent hold ≈ max not sum ──
    {
        std::println("\n--- AC2: dual-Evaluator concurrent apply lock ---");
        CompilerService cs1;
        CompilerService cs2;
        CHECK(cs1.eval("(+ 1 1)").has_value(), "warm cs1");
        CHECK(cs2.eval("(+ 1 1)").has_value(), "warm cs2");
        auto& mu1 = cs1.evaluator().agent_apply_mu_;
        auto& mu2 = cs2.evaluator().agent_apply_mu_;
        CHECK(&mu1 != &mu2, "AC2: distinct mutex objects");

        // Dual schedulers + long agent bodies that each hold their own
        // Evaluator's apply mu for ~120ms (mirrors spawn apply_closure gate).
        constexpr int kHoldMs = 120;
        Scheduler s1(2);
        Scheduler s2(2);
        SchedRunner r1(s1);
        SchedRunner r2(s2);
        std::atomic<int> inflight{0};
        std::atomic<int> peak{0};

        AgentSpec sp1;
        sp1.name = "ac2-a";
        sp1.body = [&mu1, &inflight, &peak] { hold_apply_mu(mu1, kHoldMs, &inflight, &peak); };
        AgentSpec sp2;
        sp2.name = "ac2-b";
        sp2.body = [&mu2, &inflight, &peak] { hold_apply_mu(mu2, kHoldMs, &inflight, &peak); };

        const auto t0 = std::chrono::steady_clock::now();
        auto h1 = spawn_agent_with_mailbox(s1, std::move(sp1));
        auto h2 = spawn_agent_with_mailbox(s2, std::move(sp2));
        CHECK(h1.ok, "AC2: spawn h1 ok");
        CHECK(h2.ok, "AC2: spawn h2 ok");
        auto j1 = join_agent(h1, /*timeout_ms=*/2000);
        auto j2 = join_agent(h2, /*timeout_ms=*/2000);
        const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        CHECK(j1.status == JoinStatus::Ok || (h1.fiber && h1.fiber->is_done()), "AC2: h1 done");
        CHECK(j2.status == JoinStatus::Ok || (h2.fiber && h2.fiber->is_done()), "AC2: h2 done");
        // Overlap is the contract (~max, not the sum). A shared mutex cannot
        // reach 2. Wall clock only checks that the hold actually ran; CI
        // stretches sleep_for past kHoldMs*2-40 without serializing.
        CHECK(peak.load(std::memory_order_relaxed) >= 2,
              std::format("AC2: overlapped holders {} wall {}ms ~max not sum (hold={}ms)",
                          peak.load(std::memory_order_relaxed), wall_ms, kHoldMs)
                  .c_str());
        CHECK(wall_ms >= (kHoldMs - 40),
              std::format("AC2: wall {}ms at least one hold", wall_ms).c_str());
        std::println("  AC2 wall={}ms peak={} (hold={}ms each, concurrent)", wall_ms,
                     peak.load(std::memory_order_relaxed), kHoldMs);
    }

    // ── AC3: single Evaluator multi-agent still serialized ──
    {
        std::println("\n--- AC3: single-Evaluator apply serialized ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        auto& mu = cs.evaluator().agent_apply_mu_;
        constexpr int kHoldMs = 80;
        Scheduler sched(2);
        SchedRunner runner(sched);

        AgentSpec sp1;
        sp1.name = "ac3-a";
        sp1.body = [&mu] { hold_apply_mu(mu, kHoldMs); };
        AgentSpec sp2;
        sp2.name = "ac3-b";
        sp2.body = [&mu] { hold_apply_mu(mu, kHoldMs); };

        const auto t0 = std::chrono::steady_clock::now();
        auto h1 = spawn_agent_with_mailbox(sched, std::move(sp1));
        auto h2 = spawn_agent_with_mailbox(sched, std::move(sp2));
        CHECK(h1.ok && h2.ok, "AC3: both spawn ok");
        (void)join_agent(h1, /*timeout_ms=*/2000);
        (void)join_agent(h2, /*timeout_ms=*/2000);
        const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        // Serialized: ~160ms. Allow slack; must clearly exceed one hold.
        CHECK(wall_ms >= (kHoldMs * 2 - 30),
              std::format("AC3: wall {}ms ~sum (serialized, hold={}ms)", wall_ms, kHoldMs).c_str());
        std::println("  AC3 wall={}ms (hold={}ms each, serialized)", wall_ms, kHoldMs);
    }

    // ── AC4: Aura spawn/join + schema-2158 ──
    {
        std::println("\n--- AC4: Aura orch spawn/join + query ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        const auto acq0 =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);

        auto spawn = cs.eval(R"((orch:spawn-agent "ac4-2158" (lambda () 42)))");
        CHECK(spawn.has_value(), "AC4: spawn returns");
        auto ok = cs.eval(R"((hash-ref (orch:agent-join "ac4-2158" :timeout-ms 2000) "ok"))");
        CHECK(ok && is_bool(*ok) && as_bool(*ok), "AC4: join ok");

        const auto acq1 =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        CHECK(acq1 > acq0, "AC4: apply lock acquired on spawn body");

        CHECK(href(cs, "query:orch-module-stats", "schema-2158") == 2158, "schema-2158");
        CHECK(href(cs, "query:orch-module-stats", "agent-apply-per-eval-mutex-wired") == 1,
              "wired");
        CHECK(href(cs, "query:orch-module-stats", "agent-apply-lock-acquisitions-total") >= 0,
              "acquisitions key");
        CHECK(href(cs, "query:orch-module-stats", "agent-apply-lock-wait-us-total") >= 0,
              "wait-us key");
    }

    // ── AC5: try_acquire reject skips body (no apply lock) ──
    {
        std::println("\n--- AC5: try_acquire reject does not take apply lock ---");
        const auto spawn_src = read_file("src/orch/agent_spawn.h");
        // Body only runs when acq == 0; reject path never calls body().
        CHECK(spawn_src.find("aura_orch_agent_body_try_acquire_ex") != std::string::npos,
              "AC5: try_acquire in spawn wrapper");
        CHECK(spawn_src.find("body()") != std::string::npos, "AC5: body call present");
        // Reject branch increments rejects_total without body().
        CHECK(spawn_src.find("agent_body_try_acquire_rejects_total") != std::string::npos,
              "AC5: reject counter");
        const auto agent_src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(agent_src.find("never hold agent_apply_mu_ on quota-reject") != std::string::npos ||
                  agent_src.find("try_acquire reject path never reaches this body") !=
                      std::string::npos,
              "AC5: body documents reject-before-lock");
    }

    // ── Issue #3728: region-concurrent spawn skips agent_apply_mu_ ──
    auto set_prod_3728 = [](bool on) {
        aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
            .store(on ? 1u : 0u, std::memory_order_relaxed);
    };

    {
        std::println("\n--- #3728 AC1: distinct region keys skip apply mu ---");
        set_prod_3728(true);
        CompilerService cs;
        cs.evaluator().set_workspace_region_concurrency_enabled(true);
        CHECK(cs.eval("(+ 1 1)").has_value(), "3728 AC1: warm");
        const auto acq0 =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        const auto t0 = std::chrono::steady_clock::now();
        auto r = cs.eval(R"(
            (begin
              (orch:spawn-agent "a-3728"
                (lambda () (let loop ((i 0)) (if (< i 4000000) (loop (+ i 1)) i)))
                :region-key 11)
              (orch:spawn-agent "b-3728"
                (lambda () (let loop ((i 0)) (if (< i 4000000) (loop (+ i 1)) i)))
                :region-key 22)
              (let ((ja (orch:agent-join "a-3728" :timeout-ms 2000))
                    (jb (orch:agent-join "b-3728" :timeout-ms 2000)))
                (if (and (hash-ref ja "ok") (hash-ref jb "ok")) 1 0)))
        )");
        const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        CHECK(r && is_int(*r) && as_int(*r) == 1, "3728 AC1: both region-key agents joined");
        const auto acq1 =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        CHECK(acq1 == acq0, "3728 AC1: distinct non-zero region keys did not take agent_apply_mu_");
        CHECK(wall_ms < 1800, "3728 AC1: wall is not a serialized join timeout");
        std::println("  3728 AC1 wall={}ms acq {}→{}", wall_ms, acq0, acq1);
        set_prod_3728(false);
    }

    {
        std::println("\n--- #3728 AC2: missing region_key stays serialized ---");
        set_prod_3728(true);
        CompilerService cs;
        cs.evaluator().set_workspace_region_concurrency_enabled(true);
        CHECK(cs.eval("(+ 1 1)").has_value(), "3728 AC2: warm");
        const auto acq0 =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        auto r = cs.eval(R"(
            (begin
              (orch:spawn-agent "c-3728" (lambda () 1))
              (let ((d (orch:spawn-agent "d-3728" (lambda () 1)))
                    (jc (orch:agent-join "c-3728" :timeout-ms 2000)))
                (if (and (hash-ref jc "ok")
                         (not (hash-ref d "ok"))
                         (string=? (hash-ref d "deny-detail" "")
                                   "missing-or-overlap-keys"))
                    1 0)))
        )");
        // Issue #4280: a second keyless name-table mutate agent is the
        // Scope deny face, not a second serialized admit. The one that
        // landed still takes agent_apply_mu_ (omitted key stays Serialized).
        CHECK(r && is_int(*r) && as_int(*r) == 1,
              "3728 AC2: omitted key serializes; second keyless spawn denied (#4280)");
        const auto acq1 =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        CHECK(acq1 >= acq0 + 1, "3728 AC2: missing region_key still takes agent_apply_mu_");
        set_prod_3728(false);
    }

    {
        std::println("\n--- #3728 AC3: try_acquire deny still skips apply mu ---");
        const auto spawn_src = read_file("src/orch/agent_spawn.h");
        const auto agent_src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(spawn_src.find("aura_orch_agent_body_try_acquire_ex") != std::string::npos,
              "3728 AC3: wrapper still try_acquire_ex before body");
        CHECK(agent_src.find("never hold agent_apply_mu_ on quota-reject") != std::string::npos,
              "3728 AC3: quota-reject still documented as no mu");
        CHECK(agent_src.find("apply_spawn_closure_maybe_locked") != std::string::npos,
              "3728 AC3: apply helper is the body path (after acquire)");
    }

    {
        std::println("\n--- #3728 AC4: Soft keeps mutex; no invent ---");
        set_prod_3728(false);
        CompilerService cs;
        cs.evaluator().set_workspace_region_concurrency_enabled(true);
        CHECK(cs.eval("(+ 1 1)").has_value(), "3728 AC4: warm");
        const auto acq0 =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        auto r = cs.eval(R"(
            (begin
              (orch:spawn-agent "e-3728" (lambda () 1) :region-key 11)
              (orch:spawn-agent "f-3728" (lambda () 1) :region-key 22)
              (let ((je (orch:agent-join "e-3728" :timeout-ms 2000))
                    (jf (orch:agent-join "f-3728" :timeout-ms 2000)))
                (if (and (hash-ref je "ok") (hash-ref jf "ok")) 1 0)))
        )");
        CHECK(r && is_int(*r) && as_int(*r) == 1, "3728 AC4: Soft region-key agents joined");
        const auto acq1 =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        CHECK(acq1 >= acq0 + 2, "3728 AC4: Soft/Off still takes agent_apply_mu_");
        const auto agent_src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(agent_src.find("Issue #3728") != std::string::npos, "3728 AC4: prim cites #3728");
        CHECK(agent_src.find("apply_spawn_closure_maybe_locked") != std::string::npos,
              "3728 AC4: helper present");
        CHECK(agent_src.find("query:3728") == std::string::npos, "3728 AC4: no new query key");
        CHECK(!std::filesystem::exists("tests/orch/test_issue_3728.cpp"),
              "3728 AC4: no test_issue_3728.cpp");
        CHECK(!std::filesystem::exists("docs/design/3728-region-apply-mu.md"),
              "3728 AC4: no docs/design/3728-*");
    }

    // Issue #4394: supervise-batch must enter gate_spawn_apply_region.
    // Placed here, while orch_sched is still in Scheduler::run(). #4383
    // holds agent_apply_mu_ for several seconds with no orch spawn, and
    // the holder idle-stops after ~3s. ensure() does not restart it, so
    // a later supervise-batch queues fibers that never run.
    // A live region-fast claim on the same key blocks the supervise
    // closure. Distinct keys still skip agent_apply_mu_. :watch-scope #f
    // uses that gate, not a private mutex. Missing keys and Soft take
    // the shared mutex.
    {
        std::println("\n--- #4394: supervise-batch shares the spawn region gate ---");
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        using aura::compiler::typed_audit::reset_for_test;
        using aura::orch::reset_all_agent_scopes_for_test;
        constexpr int kHoldMs = 1500;
        auto supervise_ok = [](CompilerService& cs, bool watch, std::string_view keys) -> int {
            const char* w = watch ? "#t" : "#f";
            std::string expr;
            if (keys.empty()) {
                expr = std::format("(let ((pol (orch:compose-workflow 'collect-all))"
                                   "      (tasks (list (lambda () 1) (lambda () 2))))"
                                   "  (let ((h (orch:supervise-batch tasks pol :watch-scope {})))"
                                   "    (if (hash-ref h \"ok\") 1 0)))",
                                   w);
            } else {
                expr = std::format("(let ((pol (orch:compose-workflow 'collect-all))"
                                   "      (tasks (list (lambda () 1) (lambda () 2)))"
                                   "      (keys (vector {})))"
                                   "  (let ((h (orch:supervise-batch tasks pol :watch-scope {} "
                                   ":region-keys keys)))"
                                   "    (if (hash-ref h \"ok\") 1 0)))",
                                   keys, w);
            }
            auto r = cs.eval(expr);
            if (!r || !is_int(*r))
                return -1;
            return static_cast<int>(as_int(*r));
        };

        reset_for_test();
        CompilerService cs;
        cs.evaluator().set_effect_sandbox_mode(0);
        apply_production_audit_defaults();
        cs.evaluator().set_workspace_region_concurrency_enabled(true);
        CHECK(cs.eval("(+ 1 1)").has_value(), "4394: warm");
        reset_all_agent_scopes_for_test();
        auto ping = cs.eval(R"((begin
            (orch:spawn-agent "ping-4394" (lambda () 1))
            (hash-ref (orch:agent-join "ping-4394" :timeout-ms 800) "ok")))");
        CHECK(ping && is_bool(*ping) && as_bool(*ping), "4394: orch scheduler still live");
        reset_all_agent_scopes_for_test();
        CHECK(supervise_ok(cs, true, "3 4") == 1, "4394: warmup watch batch ok");
        reset_all_agent_scopes_for_test();
        auto& ev = cs.evaluator();
        auto& mu = ev.agent_apply_mu_;

        const auto collide_before =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        std::atomic<int> watch_ok{-1};
        const bool watch_overlapped = finished_while_region_held(ev, 1, kHoldMs, [&] {
            watch_ok.store(supervise_ok(cs, true, "1 2"), std::memory_order_relaxed);
        });
        const auto collide_after =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        CHECK(!watch_overlapped, "4394: watch same key waits for the inflight claim");
        CHECK(watch_ok.load() == 1, "4394: watch same-key batch ok after the claim drops");
        CHECK(collide_after > collide_before, "4394: same-key waiter took agent_apply_mu_");
        CHECK(ev.spawn_region_inflight_size_for_test() == 0, "4394: watch inflight drained");
        reset_all_agent_scopes_for_test();

        const auto nowatch_before =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        std::atomic<int> nowatch_ok{-1};
        const bool nowatch_overlapped = finished_while_region_held(ev, 1, kHoldMs, [&] {
            nowatch_ok.store(supervise_ok(cs, false, "1 2"), std::memory_order_relaxed);
        });
        const auto nowatch_after =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        CHECK(!nowatch_overlapped, "4394: watch-scope #f same key waits for the inflight claim");
        CHECK(nowatch_ok.load() == 1, "4394: watch-scope #f same-key batch ok");
        CHECK(nowatch_after > nowatch_before, "4394: #f same-key waiter took agent_apply_mu_");
        CHECK(ev.spawn_region_inflight_size_for_test() == 0, "4394: #f inflight drained");
        reset_all_agent_scopes_for_test();

        const auto distinct_before =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        std::atomic<int> distinct_ok{-1};
        const bool distinct_finished = finished_while_mutex_held(mu, kHoldMs, [&] {
            distinct_ok.store(supervise_ok(cs, true, "11 22"), std::memory_order_relaxed);
        });
        const auto distinct_after =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        CHECK(distinct_finished, "4394: distinct keys skip agent_apply_mu_");
        CHECK(distinct_ok.load() == 1, "4394: distinct-key watch batch ok");
        CHECK(distinct_after == distinct_before, "4394: distinct keys did not take the mutex");
        reset_all_agent_scopes_for_test();

        std::atomic<int> distinct_f_ok{-1};
        const bool distinct_f_finished = finished_while_mutex_held(mu, kHoldMs, [&] {
            distinct_f_ok.store(supervise_ok(cs, false, "11 22"), std::memory_order_relaxed);
        });
        CHECK(distinct_f_finished, "4394: watch-scope #f distinct keys skip agent_apply_mu_");
        CHECK(distinct_f_ok.load() == 1, "4394: distinct-key #f batch ok");
        reset_all_agent_scopes_for_test();

        const auto missing_before =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        std::atomic<int> missing_ok{-1};
        const bool missing_finished = finished_while_mutex_held(mu, kHoldMs, [&] {
            missing_ok.store(supervise_ok(cs, false, ""), std::memory_order_relaxed);
        });
        const auto missing_after =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        CHECK(!missing_finished, "4394: missing keys take the shared mutex");
        CHECK(missing_ok.load() == 1, "4394: missing-key #f batch ok");
        CHECK(missing_after > missing_before, "4394: missing keys acquired agent_apply_mu_");
        reset_all_agent_scopes_for_test();

        reset_for_test();
        cs.evaluator().set_workspace_region_concurrency_enabled(true);
        std::atomic<int> soft_ok{-1};
        const bool soft_finished = finished_while_mutex_held(mu, kHoldMs, [&] {
            soft_ok.store(supervise_ok(cs, false, "1 2"), std::memory_order_relaxed);
        });
        CHECK(!soft_finished, "4394: Soft #f takes agent_apply_mu_");
        CHECK(soft_ok.load() == 1, "4394: Soft #f batch ok");
        CHECK(ev.spawn_region_inflight_size_for_test() == 0, "4394: Soft did not touch inflight");
        reset_all_agent_scopes_for_test();

        const auto agent_src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        const auto sup = agent_src.find("add(\"orch:supervise-batch\"");
        CHECK(sup != std::string::npos, "4394: supervise-batch prim");
        const auto slice = agent_src.substr(sup, 28000);
        CHECK(slice.find("run_supervised_apply") != std::string::npos,
              "4394: both arms call the spawn gate helper");
        CHECK(slice.find("Issue #4394") != std::string::npos, "4394: prim cites the issue");
        CHECK(agent_src.find("gate_spawn_apply_region") != std::string::npos,
              "4394: helper enters the spawn gate");
        CHECK(agent_src.find("make_shared<std::mutex>") == std::string::npos,
              "4394: no private supervise mutex");
        CHECK(agent_src.find("query:4394") == std::string::npos, "4394: no new query key");
        CHECK(read_file("tests/orch/test_issue_4394.cpp").empty(), "4394: no test_issue_4394.cpp");
        CHECK(read_file("docs/design/4394-supervise-region-gate.md").empty(),
              "4394: no docs/design");
        reset_for_test();
    }

    // Issue #4062: same region key must not overlap apply_closure.
    {
        std::println("\n--- #4062: same region key takes agent_apply_mu_ ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        std::atomic<int> in_apply{0};
        std::atomic<int> max_in{0};
        std::atomic<int> locks{0};
        auto body = [&] {
            const int now = in_apply.fetch_add(1, std::memory_order_acq_rel) + 1;
            int prev = max_in.load(std::memory_order_relaxed);
            while (now > prev &&
                   !max_in.compare_exchange_weak(prev, now, std::memory_order_relaxed)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            in_apply.fetch_sub(1, std::memory_order_acq_rel);
        };
        auto on_lock = [&](std::uint64_t) { locks.fetch_add(1, std::memory_order_relaxed); };
        std::thread t1([&] { ev.gate_spawn_apply_region(7, true, body, on_lock); });
        std::thread t2([&] { ev.gate_spawn_apply_region(7, true, body, on_lock); });
        t1.join();
        t2.join();
        CHECK(max_in.load() == 1, "4062: same key bodies did not overlap");
        CHECK(locks.load() == 1, "4062: the colliding body took agent_apply_mu_");
        CHECK(ev.spawn_region_inflight_size_for_test() == 0, "4062: inflight set drained");

        std::atomic<int> in2{0};
        std::atomic<int> max2{0};
        std::atomic<int> locks2{0};
        auto body2 = [&] {
            const int now = in2.fetch_add(1, std::memory_order_acq_rel) + 1;
            int prev = max2.load(std::memory_order_relaxed);
            while (now > prev &&
                   !max2.compare_exchange_weak(prev, now, std::memory_order_relaxed)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            in2.fetch_sub(1, std::memory_order_acq_rel);
        };
        auto on_lock2 = [&](std::uint64_t) { locks2.fetch_add(1, std::memory_order_relaxed); };
        std::thread d1([&] { ev.gate_spawn_apply_region(11, true, body2, on_lock2); });
        std::thread d2([&] { ev.gate_spawn_apply_region(22, true, body2, on_lock2); });
        d1.join();
        d2.join();
        CHECK(max2.load() == 2, "4062: distinct keys overlapped");
        CHECK(locks2.load() == 0, "4062: distinct keys skipped agent_apply_mu_");

        std::atomic<int> locks0{0};
        auto on_lock0 = [&](std::uint64_t) { locks0.fetch_add(1, std::memory_order_relaxed); };
        ev.gate_spawn_apply_region(0, false, [] {}, on_lock0);
        ev.gate_spawn_apply_region(7, false, [] {}, on_lock0);
        CHECK(locks0.load() == 2, "4062: key 0 and Soft always take the lock");
        CHECK(ev.spawn_region_inflight_size_for_test() == 0,
              "4062: Soft/Off did not touch the inflight set");

        const auto agent_src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        const auto par = agent_src.find("region_concurrent_skip_eval_mu =");
        CHECK(par != std::string::npos, "4062: parallel-intend skip site");
        CHECK(agent_src.find("IsolationLevel::RegionConcurrent", par) != std::string::npos ||
                  agent_src.rfind("IsolationLevel::RegionConcurrent", par) != std::string::npos,
              "4062: parallel-intend still gates skip on RegionConcurrent");
        CHECK(agent_src.find("Issue #4062") != std::string::npos, "4062: spawn cite");
        CHECK(agent_src.find("class AgentRegistry") == std::string::npos, "4062: no AgentRegistry");
        CHECK(read_file("docs/design/4062-same-region-apply-mu.md").empty(),
              "4062: no docs/design");
    }

    // Issue #4383: production non-pure parallel-intend apply waits on the
    // same per-Evaluator mutex as spawn. Region-fast distinct keys and
    // Soft do not take it. One eval at a time — setup outside apply_closure
    // is not the gate.
    {
        std::println("\n--- #4383: parallel-intend shares agent_apply_mu_ ---");
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        using aura::compiler::typed_audit::reset_for_test;
        const auto agent_src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(agent_src.find("Issue #4383") != std::string::npos, "4383: prim cites the issue");
        CHECK(agent_src.find("use_eval_gate") != std::string::npos, "4383: production gate flag");
        CHECK(agent_src.find("gate_spawn_apply_region") != std::string::npos,
              "4383: intend apply calls the spawn gate");
        CHECK(agent_src.find("static std::mutex orch_eval_mu") == std::string::npos,
              "4383: no process-static orch_eval_mu");

        auto run_intend = [](CompilerService& cs, const char* expr) -> int {
            auto r = cs.eval(expr);
            if (!r || !is_int(*r))
                return -1;
            return static_cast<int>(as_int(*r));
        };
        constexpr const char* kOne = R"(
            (let ((h (parallel-intend (vector (lambda () 1))
                                    :max-concurrency 1
                                    :collect-errors #t
                                    :timeout-ms 10000)))
              (hash-ref h "ok-count")))";
        constexpr const char* kRegion = R"(
            (let ((h (parallel-intend (vector (lambda () 1) (lambda () 2))
                                    :max-concurrency 2
                                    :region-keys (vector 11 22)
                                    :collect-errors #t
                                    :timeout-ms 10000)))
              (hash-ref h "ok-count")))";

        reset_for_test();
        apply_production_audit_defaults();
        CompilerService cs;
        cs.evaluator().set_effect_sandbox_mode(0);
        cs.evaluator().set_workspace_region_concurrency_enabled(false);
        CHECK(cs.eval("(+ 1 1)").has_value(), "4383: warm");
        auto& mu = cs.evaluator().agent_apply_mu_;
        const auto before =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        std::atomic<int> okc{-1};
        const bool finished = finished_while_mutex_held(
            mu, 1500, [&] { okc.store(run_intend(cs, kOne), std::memory_order_relaxed); });
        const auto after =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        CHECK(!finished, "4383: production intend waits on agent_apply_mu_");
        CHECK(okc.load() == 1, "4383: production one-task batch completed after unlock");
        CHECK(after > before, "4383: agent_apply_lock_acquisitions_total moved");

        cs.evaluator().set_workspace_region_concurrency_enabled(true);
        const auto region_before =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        std::atomic<int> region_ok{-1};
        const bool region_finished = finished_while_mutex_held(
            mu, 1500, [&] { region_ok.store(run_intend(cs, kRegion), std::memory_order_relaxed); });
        const auto region_after =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        CHECK(region_finished, "4383: distinct region keys skip agent_apply_mu_");
        CHECK(region_ok.load() == 2, "4383: region-concurrent batch ok-count");
        CHECK(region_after == region_before, "4383: region-fast did not take the mutex");

        reset_for_test();
        CompilerService soft;
        soft.evaluator().set_effect_sandbox_mode(0);
        CHECK(soft.eval("(+ 1 1)").has_value(), "4383: soft warm");
        const auto soft_before =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        std::atomic<int> soft_ok{-1};
        const bool soft_finished =
            finished_while_mutex_held(soft.evaluator().agent_apply_mu_, 1500, [&] {
                soft_ok.store(run_intend(soft, kOne), std::memory_order_relaxed);
            });
        const auto soft_after =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        CHECK(soft_finished, "4383: Soft intend does not wait on agent_apply_mu_");
        CHECK(soft_ok.load() == 1, "4383: Soft one-task batch completed");
        CHECK(soft_after == soft_before, "4383: Soft did not acquire agent_apply_mu_");
        reset_for_test();
    }

    std::println("\n=== #2158/#3728 agent apply per-eval mutex: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_agent_apply_mutex();
}
#endif
