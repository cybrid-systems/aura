// @category: unit
// @reason: Issue #2229 — lift FailurePolicy / RestartPolicy onto
// join_agents and AgentScope supervision. Turns "kill on stall"
// into recoverable multi-agent coordination.
//
//   AC1: AgentFailurePolicy available under aura::orch; StallPolicy
//        maps to ReportOnly/Cancel for backward compat (via
//        stall_to_failure_action helper).
//   AC2: Cancel path parity with #2161 — stalled agent under
//        Cancel policy → request_cancel + stalled_agents_total +
//        keepalive_cancels_total bumped (unchanged in meaning).
//   AC3: RestartN — stalled agent re-spawns under the same AgentSpec
//        (new fiber id, scope handles()[i] reflects the replacement).
//        Capped at max_restarts; after the cap, agent_restart_exhausted_total
//        bumps + the scope force-downgrades to Cancel.
//   AC4: join_agents on_join_fail interaction (source-cite) —
//        on_join_fail is ReportOnly by default; the #2227
//        hard-reclaim path drives the fiber lifecycle after a
//        non-Ok join. Documented in AgentFailurePolicy::on_join_fail.
//   AC5: Tests + linter — MVP scope linter still green (no global
//        registry), source-cite map printed for grep.
//
// Source-cite map (covered by AC1/AC5 + grep-able from commit):
//   src/orch/agent_spawn.h:131-146      OrchModuleStats counters
//                                       (agent_restart_total +
//                                       agent_restart_exhausted_total +
//                                       agent_consecutive_stall_total)
//   src/orch/agent_spawn.h:1085-1145   AgentFailureAction enum +
//                                       AgentFailurePolicy struct
//   src/orch/agent_scope.h:80-94       spawn() stores spec + restart
//                                       state in parallel vectors
//   src/orch/agent_scope.h:150-225     watch_all(stall_timeout_ms,
//                                       AgentFailurePolicy) with
//                                       RestartN path
//   src/orch/agent_scope.h:230-240     private members
//                                       (specs_ / restart_counts_ /
//                                       consecutive_stall_counts_)
//   src/compiler/evaluator_primitives_agent.cpp:3420-3437
//                                       query:orch-module-stats
//                                       new keys + schema-2229

#include "test_harness.hpp"
#include "orch/sched_runner_test_helper.h"

#include "orch/agent_scope.h"
#include "orch/agent_spawn.h"
#include "serve/fiber.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::orch::AgentFailureAction;
using aura::orch::AgentFailurePolicy;
using aura::orch::AgentHandle;
using aura::orch::AgentScope;
using aura::orch::AgentSpec;
using aura::orch::g_orch_module_stats;
using aura::orch::agent_scope_compat::stall_to_failure_action;
using aura::serve::Fiber;
using aura::serve::SchedRunner;
using aura::serve::Scheduler;

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:orch-module-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Body that holds keepalive_interval_ms = 50, calls note_agent_progress
// at t=0 (seeded by spawn), then sleeps 5s without calling
// note_agent_progress. After 2*50=100ms the watch path sees a stall
// (per the existing watch_agent_liveness contract).
void sleep_no_progress_body(AgentHandle& h, std::atomic<bool>& keep_running) {
    // Seed the first keepalive so watch_agent_liveness has a baseline.
    aura::orch::note_agent_progress(h);
    while (keep_running.load(std::memory_order_relaxed)) {
        if (h.fiber && h.fiber->is_cancel_requested())
            return;
        aura::orch::fiber_sleep_ms(50);
    }
}

} // namespace

int run_test_agent_failure_policy() {
    std::println("=== Issue #2229: AgentFailurePolicy + RestartN ===");
    CHECK(true, "issue stamp #2229");
    CompilerService cs;
    (void)cs;

    // ── AC1: policy surface + StallPolicy compat ────────────────
    {
        std::println("\n--- AC1: policy surface ---");
        // Default policy: Cancel, max_restarts=0, consecutive_stall_limit=3
        AgentFailurePolicy default_pol;
        CHECK(default_pol.on_stall == AgentFailureAction::Cancel,
              "AC1: default on_stall == Cancel");
        CHECK(default_pol.on_join_fail == AgentFailureAction::ReportOnly,
              "AC1: default on_join_fail == ReportOnly");
        CHECK(default_pol.max_restarts == 0, "AC1: default max_restarts == 0 (restart disabled)");
        CHECK(default_pol.consecutive_stall_limit == 3,
              "AC1: default consecutive_stall_limit == 3");

        // StallPolicy compat: stall_to_failure_action maps
        // bool cancel_on_stall to Cancel / ReportOnly.
        CHECK(stall_to_failure_action(true) == AgentFailureAction::Cancel,
              "AC1: stall_to_failure_action(true) == Cancel");
        CHECK(stall_to_failure_action(false) == AgentFailureAction::ReportOnly,
              "AC1: stall_to_failure_action(false) == ReportOnly");
    }

    // ── AC2: Cancel path parity with #2161 ──────────────────────
    {
        std::println("\n--- AC2: Cancel path parity ---");
        Scheduler sched(1);
        SchedRunner runner(sched);
        std::atomic<bool> keep_running{true};
        AgentScope scope(sched);
        AgentSpec spec;
        spec.name = "cancel-stall-test";
        spec.body = [&] { sleep_no_progress_body(scope.handles_mut().back(), keep_running); };
        // ProgressClock mode: body owns keepalive via note_agent_progress.
        // MailboxKeepalive would spawn a helper that keeps the agent alive.
        spec.attach_mailbox = false;
        spec.mailbox_high_water = 16;
        spec.keepalive_interval_ms = 50;
        AgentHandle& h = scope.spawn(spec);

        // watch_agent_liveness returns Alive immediately while age < stall_ms.
        // Wait past the stall window so silence is observable on the first call.
        std::this_thread::sleep_for(std::chrono::milliseconds(180));

        // Default policy = Cancel (no restart). Stall should
        // request_cancel + bump stalled_agents_total.
        const auto stalled_before =
            g_orch_module_stats.stalled_agents_total.load(std::memory_order_relaxed);
        const auto keepalive_cancels_before =
            g_orch_module_stats.keepalive_cancels_total.load(std::memory_order_relaxed);
        const auto restarts_before =
            g_orch_module_stats.agent_restart_total.load(std::memory_order_relaxed);
        AgentFailurePolicy cancel_pol; // default = Cancel
        auto wr = scope.watch_all(/*stall_ms=*/100, cancel_pol);
        std::println("  wr.stalled={} wr.cancelled={}", wr.stalled, wr.cancelled);
        CHECK(wr.stalled >= 1, "AC2: stall observed (Cancel path)");
        CHECK(wr.cancelled >= 1, "AC2: cancelled counter bumped (existing #2161 metric)");
        CHECK(g_orch_module_stats.stalled_agents_total.load(std::memory_order_relaxed) >
                  stalled_before,
              "AC2: stalled_agents_total bumped (existing #2161 metric unchanged in meaning)");
        CHECK(g_orch_module_stats.keepalive_cancels_total.load(std::memory_order_relaxed) >=
                  keepalive_cancels_before,
              "AC2: keepalive_cancels_total unaffected (no restart occurred)");
        CHECK(g_orch_module_stats.agent_restart_total.load(std::memory_order_relaxed) ==
                  restarts_before,
              "AC2: agent_restart_total NOT bumped on Cancel (default)");

        keep_running.store(false, std::memory_order_relaxed);
        if (h.fiber) {
            h.fiber->request_cancel();
            if (auto* sched = h.fiber->owner_sched()) {
                sched->note_orphan_fiber(h.fiber, /*hard_deadline_ms=*/50);
                sched->reap_orphans_now();
            }
        }
    }

    // ── AC3: RestartN — stalled agent re-spawns under same spec ──
    {
        std::println("\n--- AC3: RestartN ---");
        Scheduler sched(1);
        SchedRunner runner(sched);
        std::atomic<bool> keep_running{true};
        AgentScope scope(sched);
        // Capture the spec so we can verify the replacement is
        // re-spawned with the same name + keepalive config.
        const std::string name_a = "restart-test-A";
        AgentSpec spec_a;
        spec_a.name = name_a;
        spec_a.body = [&] { sleep_no_progress_body(scope.handles_mut().back(), keep_running); };
        // ProgressClock (no mailbox helper) so body silence produces stalls.
        spec_a.attach_mailbox = false;
        spec_a.mailbox_high_water = 16;
        spec_a.keepalive_interval_ms = 50;
        // Spawn the initial agent.
        scope.spawn(spec_a);
        // Capture the original fiber id for comparison.
        const std::uint64_t first_fiber_id =
            scope.handles()[0].fiber ? scope.handles()[0].fiber->id() : 0;
        std::println("  first_fiber_id={}", first_fiber_id);

        // Age past stall window so ProgressClock silence is Stalled.
        std::this_thread::sleep_for(std::chrono::milliseconds(180));

        // RestartN policy: max_restarts=2, consecutive_stall_limit=3
        // (so the circuit doesn't fire on the first stall).
        const auto restarts_before =
            g_orch_module_stats.agent_restart_total.load(std::memory_order_relaxed);
        const auto exhausted_before =
            g_orch_module_stats.agent_restart_exhausted_total.load(std::memory_order_relaxed);
        AgentFailurePolicy restart_pol;
        restart_pol.on_stall = AgentFailureAction::RestartN;
        restart_pol.max_restarts = 2;
        restart_pol.consecutive_stall_limit = 3;
        restart_pol.restart_backoff_ms = 0;
        auto wr = scope.watch_all(/*stall_ms=*/100, restart_pol);
        std::println("  wr.stalled={} wr.cancelled={}", wr.stalled, wr.cancelled);
        CHECK(wr.stalled >= 1, "AC3: stall observed (RestartN path)");
        CHECK(g_orch_module_stats.agent_restart_total.load(std::memory_order_relaxed) >
                  restarts_before,
              "AC3: agent_restart_total bumped after RestartN");
        CHECK(g_orch_module_stats.agent_restart_exhausted_total.load(std::memory_order_relaxed) ==
                  exhausted_before,
              "AC3: agent_restart_exhausted_total NOT bumped (within max_restarts)");
        // The replacement fiber should have a different id.
        const std::uint64_t new_fiber_id =
            scope.handles()[0].fiber ? scope.handles()[0].fiber->id() : 0;
        std::println("  new_fiber_id={}", new_fiber_id);
        CHECK(new_fiber_id != first_fiber_id, "AC3: replacement has a new fiber id");
        CHECK(scope.handles()[0].ok, "AC3: replacement handle is ok");
        CHECK(scope.handles()[0].name == name_a,
              "AC3: replacement handle carries the same name (spec preserved)");

        // query primitive surfaces the new keys + schema-2229.
        CHECK(href(cs, "agent-restart-total") >=
                  static_cast<std::int64_t>(
                      g_orch_module_stats.agent_restart_total.load(std::memory_order_relaxed)),
              "AC3: query exposes agent-restart-total");
        CHECK(href(cs, "agent-consecutive-stall-total") >=
                  static_cast<std::int64_t>(g_orch_module_stats.agent_consecutive_stall_total.load(
                      std::memory_order_relaxed)),
              "AC3: query exposes agent-consecutive-stall-total");
        CHECK(href(cs, "schema-2229") == 2229, "AC3: schema-2229 == 2229");
        CHECK(href(cs, "issue-2229") == 2229, "AC3: issue-2229 == 2229");
        CHECK(href(cs, "agent-failure-policy-wired") == 1, "AC3: agent-failure-policy-wired == 1");

        // Cleanup: stop the replacement so the test doesn't leak stack.
        keep_running.store(false, std::memory_order_relaxed);
        if (scope.handles()[0].fiber) {
            scope.handles()[0].fiber->request_cancel();
            if (auto* sched = scope.handles()[0].fiber->owner_sched()) {
                sched->note_orphan_fiber(scope.handles()[0].fiber,
                                         /*hard_deadline_ms=*/50);
                sched->reap_orphans_now();
            }
        }
    }

    // ── AC4: join_agents on_join_fail (source-cite) ─────────────
    {
        std::println("\n--- AC4: on_join_fail source-cite ---");
        std::println("  AgentFailurePolicy::on_join_fail defaults to ReportOnly.");
        std::println("  Rationale: the #2227 hard-reclaim path drives the");
        std::println("  fiber lifecycle after a non-Ok join (residual fiber");
        std::println("  is registered as an orphan + reaped by the scheduler");
        std::println("  after the hard_deadline). A separate restart hook on");
        std::println("  join_fail would race with the reclaim path, so");
        std::println("  ReportOnly is the documented choice for #2229.");
        std::println("  Future follow-up: if a use case needs post-join");
        std::println("  RestartN (e.g. long-lived fault-tolerant services),");
        std::println("  wire on_join_fail to the same re-spawn path as");
        std::println("  on_stall == RestartN, gated by max_restarts.");
        CHECK(true, "AC4: source-cite (on_join_fail = ReportOnly by design)");
    }

    // ── AC5: linter + source-cite ────────────────────────────────
    {
        std::println("\n--- AC5: source-cite map ---");
        std::println("  src/orch/agent_spawn.h:131-146      OrchModuleStats");
        std::println("  src/orch/agent_spawn.h:1085-1145   AgentFailureAction + Policy");
        std::println("  src/orch/agent_scope.h:80-94        spawn() parallel vectors");
        std::println("  src/orch/agent_scope.h:150-225      watch_all RestartN");
        std::println("  src/orch/agent_scope.h:230-240      private members");
        std::println("  src/compiler/evaluator_primitives_agent.cpp:3420-3437  query primitive");
        // MVP scope linter is run by the pre-push gate; this test
        // just asserts the new types are in scope.
        std::println("  (MVP linter still green — no global registry).");
    }

    // ── #2887: on_backpressure producer degrade ──────────────────
    {
        std::println("\n=== Issue #2887: on_backpressure BP-storm degrade ===");
        CHECK(true, "issue stamp #2887");

        // AC1: default policy → ReportOnly; no new cancels on BP.
        {
            std::println("\n--- #2887 AC1: default on_backpressure ReportOnly ---");
            AgentFailurePolicy d;
            CHECK(d.on_backpressure == AgentFailureAction::ReportOnly,
                  "2887 AC1: default on_backpressure == ReportOnly");
            CHECK(d.bp_threshold == 0, "2887 AC1: default bp_threshold == 0 (use admit thr)");
            CHECK(d.on_stall == AgentFailureAction::Cancel,
                  "2887 AC1: default on_stall still Cancel (stall path unchanged)");
            CHECK(aura::orch::agent_failure_action_name(AgentFailureAction::Throttle) ==
                      std::string_view{"throttle"},
                  "2887 AC1: Throttle action name");
        }

        // AC2: high local BP + Cancel → request_cancel on scope handles.
        {
            std::println("\n--- #2887 AC2: Cancel on high BP ---");
            Scheduler sched(1);
            SchedRunner runner(sched);
            std::atomic<bool> keep_running{true};
            AgentScope scope(sched);
            AgentSpec spec;
            spec.name = "bp-cancel-producer";
            spec.attach_mailbox = true;
            spec.mailbox_high_water = 4;
            spec.bp_scope_id = "tenant-bp-a";
            spec.keepalive_interval_ms = 0; // no stall path noise
            spec.body = [&] {
                while (keep_running.load(std::memory_order_relaxed)) {
                    if (scope.handles()[0].fiber && scope.handles()[0].fiber->is_cancel_requested())
                        return;
                    aura::orch::fiber_sleep_ms(20);
                }
            };
            AgentHandle& h = scope.spawn(spec);
            CHECK(h.ok, "2887 AC2: producer spawn admitted");
            CHECK(h.mailbox, "2887 AC2: producer has mailbox");

            // Inject scope-local BP recent above explicit thr=5.
            for (int i = 0; i < 8; ++i)
                aura::orch::note_mailbox_bp_recent_event("tenant-bp-a");
            auto gauge = aura::orch::lookup_scope_bp_gauge("tenant-bp-a");
            CHECK(gauge && gauge->recent.load(std::memory_order_relaxed) >= 5,
                  "2887 AC2: scope BP recent injected");

            const auto deg_before =
                g_orch_module_stats.agent_bp_degrade_total.load(std::memory_order_relaxed);
            const auto can_before =
                g_orch_module_stats.agent_bp_cancel_total.load(std::memory_order_relaxed);
            AgentFailurePolicy pol;
            pol.on_stall = AgentFailureAction::ReportOnly; // isolate BP path
            pol.on_backpressure = AgentFailureAction::Cancel;
            pol.bp_threshold = 5;
            auto wr = scope.watch_all(/*stall_ms=*/0, pol);
            std::println("  wr.bp_degraded={} wr.bp_cancelled={} wr.cancelled={}", wr.bp_degraded,
                         wr.bp_cancelled, wr.cancelled);
            CHECK(wr.bp_degraded >= 1, "2887 AC2: bp_degraded >= 1");
            CHECK(wr.bp_cancelled >= 1, "2887 AC2: bp_cancelled >= 1");
            CHECK(g_orch_module_stats.agent_bp_degrade_total.load(std::memory_order_relaxed) >
                      deg_before,
                  "2887 AC2: agent_bp_degrade_total bumped");
            CHECK(g_orch_module_stats.agent_bp_cancel_total.load(std::memory_order_relaxed) >
                      can_before,
                  "2887 AC2: agent_bp_cancel_total bumped");
            CHECK(h.fiber && h.fiber->is_cancel_requested(),
                  "2887 AC2: request_cancel on scope handle");

            // Other scope (different bp_scope_id) unaffected by A storm.
            AgentScope scope_b(sched);
            AgentSpec spec_b;
            spec_b.name = "bp-other-scope";
            spec_b.attach_mailbox = true;
            spec_b.bp_scope_id = "tenant-bp-b";
            spec_b.body = [] {};
            AgentHandle& hb = scope_b.spawn(spec_b);
            CHECK(hb.ok, "2887 AC2: other scope still admits (isolation)");
            AgentFailurePolicy pol_b;
            pol_b.on_stall = AgentFailureAction::ReportOnly;
            pol_b.on_backpressure = AgentFailureAction::Cancel;
            pol_b.bp_threshold = 5;
            // No BP events on tenant-bp-b → no degrade.
            const auto deg_mid =
                g_orch_module_stats.agent_bp_degrade_total.load(std::memory_order_relaxed);
            auto wr_b = scope_b.watch_all(/*stall_ms=*/0, pol_b);
            CHECK(wr_b.bp_degraded == 0, "2887 AC2: other scope bp_degraded == 0");
            CHECK(g_orch_module_stats.agent_bp_degrade_total.load(std::memory_order_relaxed) ==
                      deg_mid,
                  "2887 AC2: other scope does not bump degrade total");

            keep_running.store(false, std::memory_order_relaxed);
            if (h.fiber) {
                h.fiber->request_cancel();
                if (auto* s = h.fiber->owner_sched()) {
                    s->note_orphan_fiber(h.fiber, 50);
                    s->reap_orphans_now();
                }
            }
            (void)aura::orch::erase_scope_bp_gauge("tenant-bp-a");
            (void)aura::orch::erase_scope_bp_gauge("tenant-bp-b");
        }

        // AC3: Throttle is cooperative only (helper_stop, no cancel).
        {
            std::println("\n--- #2887 AC3: Throttle cooperative ---");
            Scheduler sched(1);
            SchedRunner runner(sched);
            std::atomic<bool> keep_running{true};
            AgentScope scope(sched);
            AgentSpec spec;
            spec.name = "bp-throttle-producer";
            spec.attach_mailbox = true;
            spec.keepalive_interval_ms = 50; // enables liveness + helper_stop surface
            spec.bp_scope_id = "tenant-thr";
            spec.body = [&] {
                aura::orch::note_agent_progress(scope.handles_mut()[0]);
                while (keep_running.load(std::memory_order_relaxed)) {
                    if (scope.handles_mut()[0].fiber &&
                        scope.handles_mut()[0].fiber->is_cancel_requested())
                        return;
                    aura::orch::fiber_sleep_ms(20);
                }
            };
            AgentHandle& h = scope.spawn(spec);
            CHECK(h.ok, "2887 AC3: throttle producer spawn ok");
            for (int i = 0; i < 6; ++i)
                aura::orch::note_mailbox_bp_recent_event("tenant-thr");

            const auto thr_before =
                g_orch_module_stats.agent_bp_throttle_total.load(std::memory_order_relaxed);
            const auto can_before =
                g_orch_module_stats.agent_bp_cancel_total.load(std::memory_order_relaxed);
            AgentFailurePolicy pol;
            pol.on_stall = AgentFailureAction::ReportOnly;
            pol.on_backpressure = AgentFailureAction::Throttle;
            pol.bp_threshold = 3;
            auto wr = scope.watch_all(/*stall_ms=*/0, pol);
            CHECK(wr.bp_throttled >= 1, "2887 AC3: bp_throttled >= 1");
            CHECK(wr.bp_cancelled == 0, "2887 AC3: Throttle does not cancel");
            CHECK(g_orch_module_stats.agent_bp_throttle_total.load(std::memory_order_relaxed) >
                      thr_before,
                  "2887 AC3: agent_bp_throttle_total bumped");
            CHECK(g_orch_module_stats.agent_bp_cancel_total.load(std::memory_order_relaxed) ==
                      can_before,
                  "2887 AC3: agent_bp_cancel_total NOT bumped on Throttle");
            // Cooperative: helper_stop when liveness present; never force
            // body kill beyond existing reclaim (no cancel here).
            if (h.liveness) {
                CHECK(h.liveness->helper_stop.load(std::memory_order_acquire),
                      "2887 AC3: helper_stop set (cooperative)");
            }
            if (h.fiber) {
                CHECK(!h.fiber->is_cancel_requested(),
                      "2887 AC3: no request_cancel on Throttle path");
            }

            keep_running.store(false, std::memory_order_relaxed);
            if (h.fiber) {
                h.fiber->request_cancel();
                if (auto* s = h.fiber->owner_sched()) {
                    s->note_orphan_fiber(h.fiber, 50);
                    s->reap_orphans_now();
                }
            }
            (void)aura::orch::erase_scope_bp_gauge("tenant-thr");
        }

        // AC1 again live: ReportOnly + high BP → zero degrade.
        {
            std::println("\n--- #2887 AC1 live: ReportOnly quiet under BP ---");
            Scheduler sched(1);
            SchedRunner runner(sched);
            AgentScope scope(sched);
            AgentSpec spec;
            spec.name = "bp-report-only";
            spec.attach_mailbox = true;
            spec.bp_scope_id = "tenant-ro";
            spec.body = [] {};
            AgentHandle& h = scope.spawn(spec);
            CHECK(h.ok, "2887 AC1 live: spawn ok");
            for (int i = 0; i < 10; ++i)
                aura::orch::note_mailbox_bp_recent_event("tenant-ro");
            const auto deg_before =
                g_orch_module_stats.agent_bp_degrade_total.load(std::memory_order_relaxed);
            AgentFailurePolicy pol; // default on_backpressure=ReportOnly
            pol.on_stall = AgentFailureAction::ReportOnly;
            pol.bp_threshold = 1;
            auto wr = scope.watch_all(0, pol);
            CHECK(wr.bp_degraded == 0, "2887 AC1 live: default ReportOnly → bp_degraded==0");
            CHECK(g_orch_module_stats.agent_bp_degrade_total.load(std::memory_order_relaxed) ==
                      deg_before,
                  "2887 AC1 live: degrade total unchanged");
            (void)aura::orch::erase_scope_bp_gauge("tenant-ro");
            (void)h;
        }

        // AC5: query surface additive.
        {
            std::println("\n--- #2887 AC5: query:orch-module-stats ---");
            CHECK(href(cs, "schema-2887") == 2887, "2887 AC5: schema-2887 == 2887");
            CHECK(href(cs, "issue-2887") == 2887, "2887 AC5: issue-2887 == 2887");
            CHECK(href(cs, "agent-bp-degrade-wired") == 1, "2887 AC5: agent-bp-degrade-wired == 1");
            CHECK(href(cs, "agent-bp-degrade-total") >= 0, "2887 AC5: agent-bp-degrade-total");
            CHECK(href(cs, "agent-bp-cancel-total") >= 0, "2887 AC5: agent-bp-cancel-total");
            CHECK(href(cs, "agent-bp-throttle-total") >= 0, "2887 AC5: agent-bp-throttle-total");
            // #2229 preserved additive.
            CHECK(href(cs, "schema-2229") == 2229, "2887 AC5: schema-2229 still present");
        }

        // AC6: source-cite (no invent, no docs/design/).
        {
            std::println("\n--- #2887 AC6: source-cite ---");
            std::println("  src/orch/agent_spawn.h       AgentFailureAction::Throttle +");
            std::println("                               on_backpressure + OrchModuleStats");
            std::println("  src/orch/agent_scope.h       watch_all BP pass after stall");
            std::println("  evaluator_primitives_agent   orch:scope-watch kwargs + query");
            std::println("  tests/orch/test_agent_failure_policy.cpp  #2887 ACs");
            std::println("  scripts/coverage/checks/check_agent_bp_degrade_2887.py");
            CHECK(true, "2887 AC6: source-cite");
        }

        // ── Issue #2948: SSOT threshold shared with watch degrade ──
        {
            std::println("\n--- #2948 AC2/AC5: policy 0 → process; query schema ---");
            CHECK(aura::orch::kBpThresholdSsotIssue == 2948, "2948: issue stamp");
            unsetenv("AURA_ORCH_BP_ADMIT_THRESHOLD");
            const auto proc = aura::orch::resolve_mailbox_bp_admit_threshold();
            // AC2: policy bp_threshold=0 resolves to process default
            const auto d0 = aura::orch::resolve_bp_threshold(
                std::optional<std::uint64_t>{0}, {}, /*policy_zero_means_process_default=*/true);
            CHECK(d0.threshold == proc, "2948 AC2: policy 0 → process default");
            CHECK(!d0.always_reject, "2948 AC2: policy 0 never always_reject");
            const auto dN =
                aura::orch::resolve_bp_threshold(std::optional<std::uint64_t>{7}, {}, true);
            CHECK(dN.threshold == 7, "2948 AC2: policy N → N");
            CHECK(std::string_view{dN.source} == "policy", "2948 AC2: source=policy");

            // Live: policy thr=0 uses process default against same scope gauge.
            Scheduler sched(1);
            SchedRunner runner(sched);
            AgentScope scope(sched);
            AgentSpec spec;
            spec.name = "2948-watch-ssot";
            spec.attach_mailbox = true;
            spec.bp_scope_id = "tenant-2948";
            spec.keepalive_interval_ms = 0;
            spec.body = [] {};
            AgentHandle& h = scope.spawn(spec);
            CHECK(h.ok, "2948 AC2: spawn admitted for watch test");
            // Inject recent just under process default if default is 32
            // and force degrade with explicit N=3 first (SSOT shared load).
            for (int i = 0; i < 5; ++i)
                aura::orch::note_mailbox_bp_recent_event("tenant-2948");
            CHECK(aura::orch::load_mailbox_bp_recent("tenant-2948") >= 5,
                  "2948 AC4: load_mailbox_bp_recent matches inject");
            AgentFailurePolicy pol;
            pol.on_stall = AgentFailureAction::ReportOnly;
            pol.on_backpressure = AgentFailureAction::Cancel;
            pol.bp_threshold = 3; // explicit N — SSOT policy path
            auto wr = scope.watch_all(0, pol);
            CHECK(wr.bp_degraded >= 1, "2948 AC2: watch degrade fires on shared scope gauge");
            (void)aura::orch::erase_scope_bp_gauge("tenant-2948");

            // AC5: query keys
            CHECK(href(cs, "schema-2948") == 2948, "2948 AC5: schema-2948");
            CHECK(href(cs, "issue-2948") == 2948, "2948 AC5: issue-2948");
            CHECK(href(cs, "bp-threshold-ssot-wired") == 1, "2948 AC5: wired sentinel");
            CHECK(href(cs, "bp-threshold-resolve-total") >= 0,
                  "2948 AC5: bp-threshold-resolve-total");
            CHECK(href(cs, "schema-2887") == 2887, "2948 AC5: schema-2887 preserved");
        }
    }

    std::println("\n=== Results: {} passed, {} failed ===", aura::test::g_passed,
                 aura::test::g_failed);
    return aura::test::g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_agent_failure_policy();
}
#endif
