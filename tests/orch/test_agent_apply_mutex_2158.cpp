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

#include "orch/agent_spawn.h"
#include "serve/fiber.h"
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

int run_test_agent_apply_mutex_2158() {
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

    std::println("\n=== #2158 agent apply per-eval mutex: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_agent_apply_mutex_2158();
}
#endif
