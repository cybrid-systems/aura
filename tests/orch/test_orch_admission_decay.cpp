// @category: unit
// @reason: Issue #2465 — mailbox_bp_recent_total must decay over time so
// spawn_agent_with_mailbox admission gate (env AURA_ORCH_BP_ADMIT_THRESHOLD)
// does not permanently deny all spawns after the first BP event.
//
//   AC1: decay window — BP event → counter > 0 → spawn denied
//   AC2: after decay interval — counter resets to 0 → spawn admitted
//   AC3: env var AURA_ORCH_BP_DECAY_MS parses correctly + falls back to
//        default 30000 on invalid/missing input
//   AC4: decay_ms = 0 (AURA_ORCH_BP_DECAY_MS=0) disables decay — counter
//        stays monotonic (diagnostic-only mode)
//   AC5: default decay constant = 30000ms (matches the existing
//        kJoinDrainResidualHardMsDefault env-override pattern from #2155)
//
// Sibling of test_mailbox_bp_admit.cpp (the BP-admit gate itself;
// #2465 is the decay window that prevents the gate from permanently
// denying after the first BP event). Lives in tests/orch/ per #81934/#81967.
//
// BP trigger strategy: we directly bump mailbox_bp_recent_total via
// fetch_add (the same atomic op the production code uses in
// emit_keepalive at agent_spawn.h:538 + agent_send at agent_spawn.h:1062).
// This isolates the decay-logic test from the BP-trigger path which
// test_mailbox_bp_admit.cpp already covers end-to-end. Reset
// helper: no global reset exists for the BP counters; we baseline-
// capture at AC start + restore at AC end (same pattern as
// test_mailbox_bp_admit.cpp).

#include "test_harness.hpp"
#include "orch/sched_runner_test_helper.h"

#include "orch/agent_spawn.h"
#include "serve/fiber.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <print>
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
using aura::orch::g_mailbox_bp_last_decay_us;
using aura::orch::g_mailbox_bp_last_event_us;
using aura::orch::g_orch_module_stats;
using aura::orch::kMailboxBpDecayMsDefault;
using aura::orch::orch_now_us;
using aura::orch::resolve_mailbox_bp_admit_threshold;
using aura::orch::resolve_mailbox_bp_decay_ms;
using aura::orch::spawn_agent_with_mailbox;
using aura::serve::Fiber;
using aura::serve::SchedRunner;
using aura::serve::Scheduler;

// RAII helper: restore process-wide counter state on scope exit so each AC
// starts from a clean baseline and doesn't leak state to other tests in the
// same binary. The decay-state atomic g_mailbox_bp_last_decay_us is also
// restored so the next AC's first spawn can decay cleanly.
struct ScopedOrchRestore {
    std::uint64_t saved_counter;
    std::uint64_t saved_decay_us;
    std::uint64_t saved_event_us;
    ScopedOrchRestore()
        : saved_counter(g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed))
        , saved_decay_us(g_mailbox_bp_last_decay_us.load(std::memory_order_relaxed))
        , saved_event_us(g_mailbox_bp_last_event_us.load(std::memory_order_relaxed)) {}
    ~ScopedOrchRestore() {
        g_orch_module_stats.mailbox_bp_recent_total.store(saved_counter, std::memory_order_release);
        g_mailbox_bp_last_decay_us.store(saved_decay_us, std::memory_order_release);
        g_mailbox_bp_last_event_us.store(saved_event_us, std::memory_order_release);
    }
};

// Bump the counter as if a BP event fired (same helper production push /
// agent_send / emit_keepalive paths use). Updates last-event clock so
// quiet-period decay (#2398) can observe the event timestamp.
std::uint64_t simulate_bp_event(std::uint64_t n = 1) {
    for (std::uint64_t i = 0; i < n; ++i)
        aura::orch::note_mailbox_bp_recent_event();
    return g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed);
}

// Run a spawn under the current threshold/env settings; return the handle.
AgentHandle try_spawn(Scheduler& sched, const char* tag) {
    AgentSpec spec;
    spec.name = tag;
    spec.attach_mailbox = true;
    spec.mailbox_high_water = 16;
    spec.keepalive_interval_ms = 0;
    spec.body = [] {};
    return spawn_agent_with_mailbox(sched, spec);
}

} // namespace

int main() {
    std::println("=== Issue #2465: mailbox BP counter decay window ===");
    CHECK(true, "issue stamp #2465");

    // ── AC5: default constant + env resolver ──────────────────────
    {
        std::println("\n--- AC5: default decay constant + env resolver ---");
        CHECK(kMailboxBpDecayMsDefault == 30000,
              "AC5: kMailboxBpDecayMsDefault == 30000 (matches kJoinDrainResidualHardMsDefault)");

        setenv("AURA_ORCH_BP_DECAY_MS", "", 1);
        CHECK(resolve_mailbox_bp_decay_ms() == 30000, "AC5: unset env falls back to 30000");

        setenv("AURA_ORCH_BP_DECAY_MS", "100", 1);
        CHECK(resolve_mailbox_bp_decay_ms() == 100, "AC5: env=100 parses to 100");

        setenv("AURA_ORCH_BP_DECAY_MS", "0", 1);
        CHECK(resolve_mailbox_bp_decay_ms() == 0, "AC5: env=0 disables decay");

        setenv("AURA_ORCH_BP_DECAY_MS", "garbage", 1);
        CHECK(resolve_mailbox_bp_decay_ms() == 30000, "AC5: invalid env falls back to 30000");

        // Restore default so later ACs see the production default unless
        // explicitly overridden.
        setenv("AURA_ORCH_BP_DECAY_MS", "", 1);
    }

    // ── AC1: BP event → counter > 0 → spawn denied ───────────────
    {
        std::println("\n--- AC1: BP event → counter > 0 → spawn denied ---");
        ScopedOrchRestore restore;

        setenv("AURA_ORCH_BP_ADMIT_THRESHOLD", "1", 1);
        setenv("AURA_ORCH_BP_DECAY_MS", "100", 1);

        Scheduler sched(1);
        SchedRunner runner(sched);

        // First spawn with no BP history → admits (decay state = 0 means
        // "never decayed"; counter is also 0 so threshold check passes).
        AgentHandle h0 = try_spawn(sched, "ac1-baseline");
        CHECK(h0.ok, "AC1: baseline spawn admitted (no BP history)");
        if (h0.ok && h0.fiber) {
            h0.fiber->request_cancel();
            if (h0.fiber->owner_sched())
                h0.fiber->owner_sched()->note_orphan_fiber(h0.fiber, /*hard_deadline_ms=*/50);
        }

        // Simulate a BP event (counter += 1).
        const auto baseline =
            g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed);
        simulate_bp_event(3);
        const auto after_bp =
            g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed);
        CHECK(after_bp >= baseline + 3, "AC1: counter incremented after BP events");

        // Spawn with threshold=1 + counter>=1 → denied.
        AgentHandle h_denied = try_spawn(sched, "ac1-denied");
        CHECK(!h_denied.ok, "AC1: spawn denied when threshold met");
        CHECK(h_denied.quota_exceeded, "AC1: quota_exceeded=true on BP reject");
        CHECK(h_denied.quota_dimension == "mailbox-bp", "AC1: quota_dimension=\"mailbox-bp\"");
        CHECK(h_denied.reserved_memory_bytes == 0,
              "AC1: no arena leak on BP reject (no-leak parity with #2155)");

        // Restore env.
        setenv("AURA_ORCH_BP_ADMIT_THRESHOLD", "0", 1);
    }

    // ── AC2: after quiet-period window — counter resets → spawn admitted
    // Issue #2398: quiet period is measured from last BP event, not from
    // last decay stamp. BP → immediate deny; wait window with no new BP →
    // admit.
    {
        std::println("\n--- AC2: after quiet-period window — counter resets, spawn admitted ---");
        ScopedOrchRestore restore;

        setenv("AURA_ORCH_BP_ADMIT_THRESHOLD", "1", 1);
        setenv("AURA_ORCH_BP_DECAY_MS", "100", 1);

        Scheduler sched(1);
        SchedRunner runner(sched);

        // Prime counter via BP events (stamps last_event_us).
        simulate_bp_event(5);
        const auto after_bp =
            g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed);
        CHECK(after_bp >= 5, "AC2: counter non-zero after BP events");

        // Without waiting, spawn should be denied (quiet window not elapsed).
        AgentHandle h_denied = try_spawn(sched, "ac2-denied-pre-wait");
        CHECK(!h_denied.ok,
              "AC2: spawn denied before quiet window elapses (counter still >= threshold)");
        CHECK(h_denied.quota_dimension == "mailbox-bp",
              "AC2: quota_dimension=\"mailbox-bp\" on pre-wait deny");

        // Wait past the 100ms quiet window (no new BP).
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        // Next spawn should decay + admit.
        AgentHandle h_admitted = try_spawn(sched, "ac2-admitted-post-wait");
        CHECK(h_admitted.ok, "AC2: spawn admitted after quiet window elapsed (counter reset to 0)");

        const auto last_decay_after = g_mailbox_bp_last_decay_us.load(std::memory_order_relaxed);
        CHECK(last_decay_after > 0, "AC2: g_mailbox_bp_last_decay_us advanced");

        if (h_admitted.ok && h_admitted.fiber) {
            h_admitted.fiber->request_cancel();
            if (h_admitted.fiber->owner_sched())
                h_admitted.fiber->owner_sched()->note_orphan_fiber(h_admitted.fiber,
                                                                   /*hard_deadline_ms=*/50);
        }

        // Restore env.
        setenv("AURA_ORCH_BP_ADMIT_THRESHOLD", "0", 1);
        setenv("AURA_ORCH_BP_DECAY_MS", "", 1);
    }

    // ── AC4: decay_ms=0 disables decay (diagnostic-only mode) ─────
    {
        std::println("\n--- AC4: decay_ms=0 disables decay ---");
        ScopedOrchRestore restore;

        setenv("AURA_ORCH_BP_ADMIT_THRESHOLD", "1", 1);
        setenv("AURA_ORCH_BP_DECAY_MS", "0", 1);

        Scheduler sched(1);
        SchedRunner runner(sched);

        // Prime counter.
        simulate_bp_event(3);

        AgentHandle h_denied = try_spawn(sched, "ac4-denied");
        CHECK(!h_denied.ok, "AC4: spawn denied when threshold=1 + counter>=1");

        // Wait LONGER than any plausible decay window — with decay_ms=0,
        // the preflight skips the decay check entirely.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        AgentHandle h_still_denied = try_spawn(sched, "ac4-still-denied");
        CHECK(!h_still_denied.ok, "AC4: spawn still denied after 200ms when decay disabled");

        // Restore env.
        setenv("AURA_ORCH_BP_ADMIT_THRESHOLD", "0", 1);
        setenv("AURA_ORCH_BP_DECAY_MS", "", 1);
    }

    // ── AC2 multi-cycle: BP / quiet / BP / quiet (regression) ─────
    {
        std::println("\n--- AC2 multi-cycle: BP / quiet / BP / quiet ---");
        ScopedOrchRestore restore;

        setenv("AURA_ORCH_BP_ADMIT_THRESHOLD", "1", 1);
        setenv("AURA_ORCH_BP_DECAY_MS", "100", 1);

        Scheduler sched(1);
        SchedRunner runner(sched);

        for (int cycle = 0; cycle < 3; ++cycle) {
            // Each cycle: BP stamps last_event → deny; wait quiet → admit.
            simulate_bp_event(3);
            AgentHandle h_denied = try_spawn(sched, "ac2-multi-denied");
            CHECK(!h_denied.ok, std::format("AC2-multi cycle {}: denied pre-quiet", cycle));
            if (h_denied.ok && h_denied.fiber) {
                h_denied.fiber->request_cancel();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            AgentHandle h_admitted = try_spawn(sched, "ac2-multi-admitted");
            CHECK(h_admitted.ok, std::format("AC2-multi cycle {}: admitted post-quiet", cycle));
            if (h_admitted.ok && h_admitted.fiber) {
                h_admitted.fiber->request_cancel();
            }
        }

        // Restore env.
        setenv("AURA_ORCH_BP_ADMIT_THRESHOLD", "0", 1);
        setenv("AURA_ORCH_BP_DECAY_MS", "", 1);
    }

    std::println("\n=== Issue #2465: decay window ACs complete ===");
    return 0;
}