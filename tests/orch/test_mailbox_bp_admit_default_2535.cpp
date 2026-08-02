// @category: unit
// @reason: Issue #2535 — production default mild mailbox BP admit (threshold=32).
//
//   AC1: no env → resolve_mailbox_bp_admit_threshold() == 32
//   AC2: AURA_ORCH_BP_ADMIT_THRESHOLD=0 disables gate
//   AC3: BP >= threshold + attach_mailbox → soft-reject mailbox-bp
//   AC4: quiet-period decay can re-admit (#2398 regression)
//   AC5: query keys schema-2535 + threshold-default
//   AC6: source-cite + linter

#include "test_harness.hpp"
#include "orch/sched_runner_test_helper.h"
#include "orch/agent_spawn.h"
#include "serve/fiber.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/scheduler.h"

#include <cstdlib>
#include <fstream>
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
using aura::orch::AgentHandle;
using aura::orch::AgentSpec;
using aura::orch::g_orch_module_stats;
using aura::orch::kMailboxBpAdmitDefaultOnIssue;
using aura::orch::kMailboxBpAdmitThresholdDefault;
using aura::orch::resolve_mailbox_bp_admit_threshold;
using aura::orch::spawn_agent_with_mailbox;
using aura::serve::SchedRunner;
using aura::serve::Scheduler;
using aura::serve::mf_mailbox::MailMessage;
using aura::serve::mf_mailbox::MailPriority;
using aura::serve::mf_mailbox::MultiFiberMailbox;
using aura::serve::mf_mailbox::PushStatus;
using aura::test::g_failed;
using aura::test::g_passed;

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:orch-module-stats\") \"{}\")", key));
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

int main() {
    std::println("=== Issue #2535: mailbox BP admit default-on (threshold=32) ===");
    CHECK(kMailboxBpAdmitDefaultOnIssue == 2535, "issue stamp");
    CHECK(kMailboxBpAdmitThresholdDefault == 32, "AC1: default constant 32");

    {
        std::println("\n--- AC1: no env → default 32 ---");
        unsetenv("AURA_ORCH_BP_ADMIT_THRESHOLD");
        CHECK(resolve_mailbox_bp_admit_threshold() == 32, "AC1: resolve == 32");
    }
    {
        std::println("\n--- AC2: env=0 disables ---");
        setenv("AURA_ORCH_BP_ADMIT_THRESHOLD", "0", 1);
        CHECK(resolve_mailbox_bp_admit_threshold() == 0, "AC2: env=0");
        unsetenv("AURA_ORCH_BP_ADMIT_THRESHOLD");
    }
    {
        std::println("\n--- AC3: BP >= default threshold → soft-reject ---");
        // Use low threshold via env for fast test (default 32 would need 32 BP events).
        // Contract of soft-reject path is same as #2228; default-on is AC1.
        setenv("AURA_ORCH_BP_ADMIT_THRESHOLD", "1", 1);
        setenv("AURA_ORCH_BP_WINDOW_MS", "60000", 1); // no quiet decay mid-test
        Scheduler sched(1);
        SchedRunner runner(sched);
        MultiFiberMailbox mb(8);
        // Force BP by overfilling high_water.
        for (int i = 0; i < 32; ++i) {
            MailMessage m;
            m.priority = MailPriority::Normal;
            m.payload = "x";
            (void)mb.push(std::move(m));
        }
        // Manually note BP if push didn't (depends on high_water wiring).
        for (int i = 0; i < 2; ++i)
            aura::orch::note_mailbox_bp_recent_event();
        const auto reject_before =
            g_orch_module_stats.spawn_bp_admit_reject_total.load(std::memory_order_relaxed);
        AgentSpec s;
        s.name = "2535-bp-reject";
        s.body = [] {};
        s.attach_mailbox = true;
        s.mailbox_high_water = 8;
        s.keepalive_interval_ms = 0;
        AgentHandle h = spawn_agent_with_mailbox(sched, s);
        CHECK(!h.ok, "AC3: ok=false");
        CHECK(h.quota_exceeded, "AC3: quota_exceeded");
        CHECK(h.quota_dimension == "mailbox-bp", "AC3: dimension mailbox-bp");
        CHECK(h.reserved_memory_bytes == 0, "AC3: reserved==0");
        CHECK(g_orch_module_stats.spawn_bp_admit_reject_total.load(std::memory_order_relaxed) >
                  reject_before,
              "AC3: reject counter++");
        setenv("AURA_ORCH_BP_ADMIT_THRESHOLD", "0", 1);
        setenv("AURA_ORCH_BP_WINDOW_MS", "", 1);
    }
    {
        std::println("\n--- AC4: quiet-period re-admit (#2398) ---");
        setenv("AURA_ORCH_BP_ADMIT_THRESHOLD", "1", 1);
        setenv("AURA_ORCH_BP_WINDOW_MS", "5", 1);
        aura::orch::note_mailbox_bp_recent_event();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        Scheduler sched(1);
        SchedRunner runner(sched);
        AgentSpec s;
        s.name = "2535-recover";
        s.body = [] {};
        s.attach_mailbox = true;
        s.mailbox_high_water = 16;
        s.keepalive_interval_ms = 0;
        AgentHandle h = spawn_agent_with_mailbox(sched, s);
        CHECK(h.ok, "AC4: after quiet period admit succeeds");
        if (h.ok && h.fiber) {
            h.fiber->request_cancel();
            if (h.fiber->owner_sched()) {
                h.fiber->owner_sched()->note_orphan_fiber(h.fiber, 50);
                h.fiber->owner_sched()->reap_orphans_now();
            }
        }
        unsetenv("AURA_ORCH_BP_ADMIT_THRESHOLD");
        unsetenv("AURA_ORCH_BP_WINDOW_MS");
    }
    {
        std::println("\n--- AC5: query keys ---");
        unsetenv("AURA_ORCH_BP_ADMIT_THRESHOLD");
        CompilerService cs;
        CHECK(href(cs, "schema-2535") == 2535, "AC5: schema-2535");
        CHECK(href(cs, "issue-2535") == 2535, "AC5: issue-2535");
        CHECK(href(cs, "mailbox-bp-admit-threshold-default") == 32, "AC5: default key 32");
        CHECK(href(cs, "mailbox-bp-admit-default-on-wired") == 1, "AC5: wired");
        CHECK(href(cs, "mailbox-bp-admit-threshold") == 32, "AC5: live threshold default");
        CHECK(href(cs, "schema-2228") == 2228, "AC5: #2228 preserved");
    }
    {
        std::println("\n--- AC6: source-cite ---");
        auto h = read_file("src/orch/agent_spawn.h");
        auto readme = read_file("src/orch/README.md");
        auto agent = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(h.find("kMailboxBpAdmitThresholdDefault = 32") != std::string::npos, "default 32");
        CHECK(h.find("2535") != std::string::npos, "cite #2535");
        CHECK(readme.find("2535") != std::string::npos ||
                  readme.find("AURA_ORCH_BP_ADMIT_THRESHOLD=0") != std::string::npos,
              "README opt-out");
        CHECK(agent.find("schema-2535") != std::string::npos, "query schema");
    }
    std::println("\n=== #2535: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
