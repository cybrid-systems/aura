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

// Hold agent_apply_mu_ for `hold_ms` (simulates apply_closure under the gate).
void hold_apply_mu(std::mutex& mu, int hold_ms) {
    std::lock_guard lock(mu);
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
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

        AgentSpec sp1;
        sp1.name = "ac2-a";
        sp1.body = [&mu1] { hold_apply_mu(mu1, kHoldMs); };
        AgentSpec sp2;
        sp2.name = "ac2-b";
        sp2.body = [&mu2] { hold_apply_mu(mu2, kHoldMs); };

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
        // Concurrent: ~120ms, not ~240ms. Allow slack for CI noise.
        CHECK(wall_ms < (kHoldMs * 2 - 40),
              std::format("AC2: wall {}ms ~max not sum (hold={}ms)", wall_ms, kHoldMs).c_str());
        CHECK(wall_ms >= (kHoldMs - 40),
              std::format("AC2: wall {}ms at least one hold", wall_ms).c_str());
        std::println("  AC2 wall={}ms (hold={}ms each, concurrent)", wall_ms, kHoldMs);
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
              (orch:spawn-agent "d-3728" (lambda () 1))
              (let ((jc (orch:agent-join "c-3728" :timeout-ms 2000))
                    (jd (orch:agent-join "d-3728" :timeout-ms 2000)))
                (if (and (hash-ref jc "ok") (hash-ref jd "ok")) 1 0)))
        )");
        CHECK(r && is_int(*r) && as_int(*r) == 1, "3728 AC2: omitted-key agents joined");
        const auto acq1 =
            g_orch_module_stats.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed);
        CHECK(acq1 >= acq0 + 2, "3728 AC2: missing region_key still takes agent_apply_mu_");
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

    std::println("\n=== #2158/#3728 agent apply per-eval mutex: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_agent_apply_mutex();
}
#endif
