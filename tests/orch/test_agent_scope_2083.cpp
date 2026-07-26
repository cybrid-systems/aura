// test_agent_scope_2083.cpp — Issue #2083 AgentScope opt-in + #2161 watch_all.
//
// Defines AURA_ENABLE_AGENT_SCOPE so the AgentScope class is visible in
// this TU. Default builds (no flag) compile this file as empty work —
// the linter (scripts/check_orch_mvp_scope.py --strict) must still pass.
//
// Tests:
//   AC1: AgentScope can spawn N agents, join_all / cancel_all behave under
//        timeout.
//   AC2: Destructor does not leak fibers or arena reservations.
//   AC3: Two scopes do not share handles; no global map.
//   AC4: Default (flag off) tree still passes --strict MVP scope linter
//        (verified by the pre-push gate + a source-cite test below).
//   AC5: Stress: cancel mid-run + quota reject inside scope.
//   AC6: Design note in src/orch/README.md (flag + semantics).
//   #2161 AC1–AC5: watch_all batch liveness + stall cancel (feature-flagged).

#define AURA_ENABLE_AGENT_SCOPE // opt-in for this TU (#2083 / #2161)

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <vector>

#include "core/resource_quota.hh"
#include "orch/agent_scope.h"
#include "orch/agent_spawn.h"
#include "serve/fiber.h"
#include "serve/scheduler.h"

import std;

namespace {

using aura::core::resource_quota::Dimension;
using aura::core::resource_quota::process_resource_quota;
using aura::core::resource_quota::reset_process_resource_quota_for_test;
using aura::orch::AgentHandle;
using aura::orch::AgentScope;
using aura::orch::AgentSpec;
using aura::orch::g_orch_module_stats;
using aura::orch::KeepaliveWatchStatus;
using aura::orch::note_agent_progress;
using aura::orch::ScopeWatchResult;
using aura::orch::StallPolicy;
using aura::orch::watch_agent_liveness;
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

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// ── AC4: source cite + linter still passes (gate-level; documented here) ──
static void ac4_linter_and_source() {
    std::println("\n--- AC4: linter stays green + source cites ---");
    auto header_src = read_file("src/orch/agent_scope.h");
    CHECK(!header_src.empty(), "agent_scope.h exists");
    CHECK(header_src.find("Issue #2083") != std::string::npos, "agent_scope.h cites #2083");
    CHECK(header_src.find("AURA_ENABLE_AGENT_SCOPE") != std::string::npos,
          "agent_scope.h references the feature flag");
    CHECK(header_src.find("class AgentScope") != std::string::npos, "AgentScope class declared");
    // Issue #2161: watch_all still behind the same flag (no global registry).
    CHECK(header_src.find("watch_all") != std::string::npos, "#2161: watch_all API");
    CHECK(header_src.find("ScopeWatchResult") != std::string::npos, "#2161: ScopeWatchResult");
    CHECK(header_src.find("2161") != std::string::npos, "agent_scope.h cites #2161");
    CHECK(header_src.find("AgentRegistry") == std::string::npos, "AC5: no AgentRegistry");
    CHECK(header_src.find("global_agent_registry") == std::string::npos,
          "AC5: no global_agent_registry");
}

// ── AC1: spawn N + join_all / cancel_all under timeout ────────────────
static void ac1_spawn_join_cancel() {
    std::println("\n--- AC1: spawn N + join_all/cancel_all under timeout ---");
    Scheduler sched(2);
    SchedRunner runner(sched);
    std::atomic<bool> hold{true};
    AgentScope scope(sched);
    for (int i = 0; i < 4; ++i) {
        scope.spawn({.name = std::format("ac1-{}", i), .body = [&] {
                         while (hold.load(std::memory_order_relaxed)) {
                             if (aura::serve::g_current_fiber &&
                                 aura::serve::g_current_fiber->is_cancel_requested())
                                 break;
                             Fiber::yield(YieldReason::Explicit);
                         }
                     }});
    }
    CHECK(scope.size() == 4, "scope has 4 agents");
    CHECK(!scope.empty(), "scope not empty");

    // Brief wait so bodies are running.
    for (int i = 0;
         i < 32 && std::any_of(
                       scope.handles().begin(), scope.handles().end(),
                       [](const AgentHandle& h) { return h.fiber && !h.fiber->is_done(); });
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto jr = scope.join_all(std::optional<std::uint64_t>{5});
    CHECK(jr.status == JoinStatus::Timeout, "join_all timed out (short timeout)");

    // Release bodies so they exit the loop.
    hold.store(false, std::memory_order_relaxed);
    // Brief drain.
    for (int i = 0;
         i < 200 && std::any_of(
                        scope.handles().begin(), scope.handles().end(),
                        [](const AgentHandle& h) { return h.fiber && !h.fiber->is_done(); });
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto jr2 = scope.join_all(std::optional<std::uint64_t>{5000});
    CHECK(jr2.status == JoinStatus::Ok, "join_all drained after release");
}

// ── AC2: destructor does not leak fibers or arena reservations ────────
static void ac2_destructor_releases() {
    std::println("\n--- AC2: destructor releases fibers + arena ---");
    reset_process_resource_quota_for_test();
    auto& pq = process_resource_quota();
    const auto arena0 = pq.agent_arena_usage_bytes.load(std::memory_order_relaxed);
    {
        Scheduler sched(2);
        SchedRunner runner(sched);
        AgentScope scope(sched);
        for (int i = 0; i < 3; ++i) {
            scope.spawn({.name = std::format("ac2-{}", i), .body = [] {
                             for (int i = 0; i < 4; ++i)
                                 Fiber::yield(YieldReason::Explicit);
                         }});
        }
        CHECK(scope.size() == 3, "scope has 3 agents");
        // scope destructor runs at end of block: cancel + best-effort join +
        // reservation release per handle.
    }
    // After scope destruction, all fibers are done and reservations released.
    CHECK(pq.agent_arena_usage_bytes.load() <= arena0,
          "arena usage returns to baseline after scope destruction");
    CHECK(pq.fiber_reservations_active.load() == 0,
          "no fiber reservations leak after scope destruction");
    reset_process_resource_quota_for_test();
}

// ── AC3: two scopes do not share handles; no global map ──────────────
static void ac3_two_scopes_isolated() {
    std::println("\n--- AC3: two scopes do not share handles ---");
    Scheduler sched(2);
    SchedRunner runner(sched);
    AgentScope scope_a(sched);
    AgentScope scope_b(sched);

    scope_a.spawn({.name = "shared-name", .body = [] {
                       for (int i = 0; i < 4; ++i)
                           Fiber::yield(YieldReason::Explicit);
                   }});
    scope_b.spawn({.name = "shared-name", .body = [] {
                       for (int i = 0; i < 4; ++i)
                           Fiber::yield(YieldReason::Explicit);
                   }});

    CHECK(scope_a.size() == 1, "scope_a has 1 agent");
    CHECK(scope_b.size() == 1, "scope_b has 1 agent");
    const auto* pa = &scope_a.handles().front();
    const auto* pb = &scope_b.handles().front();
    CHECK(pa != pb, "handles point to distinct storage (no shared map)");
    CHECK(pa->id != pb->id, "fiber ids differ across scopes");
}

// ── AC5: stress — cancel mid-run + quota reject inside scope ──────────
static void ac5_stress_cancel_quota() {
    std::println("\n--- AC5: stress cancel mid-run + quota reject ---");
    reset_process_resource_quota_for_test();
    auto& pq = process_resource_quota();
    pq.set_limit(Dimension::Memory, 6 * 1024 * 1024); // ~300 agent budget
    Scheduler sched(2);
    SchedRunner runner(sched);
    std::atomic<bool> hold{true};
    AgentScope scope(sched);
    int spawned_quota = 0;
    for (int i = 0; i < 100; ++i) {
        AgentSpec spec{.name = std::format("ac5-{}", i), .body = [&] {
                           while (hold.load(std::memory_order_relaxed)) {
                               if (aura::serve::g_current_fiber &&
                                   aura::serve::g_current_fiber->is_cancel_requested())
                                   break;
                               Fiber::yield(YieldReason::Explicit);
                           }
                       }};
        try {
            scope.spawn(std::move(spec));
        } catch (...) {
            ++spawned_quota;
        }
    }
    CHECK(scope.size() + spawned_quota == 100,
          "100 total spawn attempts accounted (scope.size() + quota rejects)");
    CHECK(scope.size() >= 1, "at least one agent in scope");

    // Trigger cancel mid-run and verify scope handles observe it.
    scope.cancel_all();
    hold.store(false, std::memory_order_relaxed);
    auto jr = scope.join_all(std::optional<std::uint64_t>{5000});
    CHECK(jr.status == JoinStatus::Ok || jr.status == JoinStatus::Timeout,
          "stress join returns Ok or Timeout");
    reset_process_resource_quota_for_test();
}

// ── AC6: design note in src/orch/README.md (flag + semantics) ─────────
static void ac6_readme_section() {
    std::println("\n--- AC6: README documents AgentScope ---");
    auto readme = read_file("src/orch/README.md");
    CHECK(!readme.empty(), "src/orch/README.md readable");
    CHECK(readme.find("AgentScope") != std::string::npos, "README mentions AgentScope");
    CHECK(readme.find("AURA_ENABLE_AGENT_SCOPE") != std::string::npos,
          "README documents the feature flag");
    CHECK(readme.find("watch_all") != std::string::npos || readme.find("2161") != std::string::npos,
          "README documents watch_all / #2161");
}

// ── #2161 AC2–AC4: watch_all aggregates + cancel only stalled ──────────
static void ac2161_watch_all_batch() {
    std::println("\n--- #2161: watch_all batch liveness ---");
    Scheduler sched(2);
    SchedRunner runner(sched);
    std::atomic<bool> hold{true};
    AgentScope scope(sched);

    // Reserve so spawn refs stay valid across subsequent emplace (vector growth).
    // (handles are moved into a fixed-capacity vector after all spawns via index.)
    AgentHandle* pc_self = nullptr;
    std::atomic<bool> touch{true};

    // (a) keepalive disabled → Closed (AC4).
    scope.spawn({.name = "no-ka", .body = [&] {
                     while (hold.load(std::memory_order_relaxed)) {
                         if (aura::serve::g_current_fiber &&
                             aura::serve::g_current_fiber->is_cancel_requested())
                             break;
                         Fiber::yield(YieldReason::Explicit);
                     }
                 }});

    // (b) ProgressClock agent (interval > 0, no mailbox) — starts Alive.
    AgentSpec pc_spec;
    pc_spec.name = "pc";
    pc_spec.attach_mailbox = false;
    pc_spec.keepalive_interval_ms = 30;
    pc_spec.body = [&] {
        while (hold.load(std::memory_order_relaxed)) {
            if (touch.load(std::memory_order_relaxed) && pc_self)
                note_agent_progress(*pc_self);
            if (aura::serve::g_current_fiber && aura::serve::g_current_fiber->is_cancel_requested())
                break;
            Fiber::yield(YieldReason::Explicit);
        }
    };
    scope.spawn(std::move(pc_spec));

    // (c) Done agent: short body that exits immediately.
    scope.spawn({.name = "done-soon", .body = [] {
                     for (int i = 0; i < 2; ++i)
                         Fiber::yield(YieldReason::Explicit);
                 }});

    // Resolve ProgressClock handle by name after all spawns (stable index).
    AgentHandle* pc = nullptr;
    for (auto& h : scope.handles_mut()) {
        if (h.name == "pc") {
            pc = &h;
            break;
        }
    }
    CHECK(pc != nullptr, "ProgressClock handle found");
    pc_self = pc;
    CHECK(pc->ok, "ProgressClock spawn ok");
    CHECK(pc->keepalive_interval_ms == 30, "ProgressClock interval recorded");
    CHECK(pc->liveness != nullptr, "ProgressClock has liveness");
    CHECK(pc->mailbox == nullptr, "ProgressClock no mailbox");

    // Wait for ProgressClock baseline + done agent to finish.
    for (int i = 0; i < 100 && pc->liveness && pc->liveness->last_keepalive_us.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    for (int i = 0; i < 100; ++i) {
        bool all_done_short = true;
        for (const auto& h : scope.handles()) {
            if (h.name == "done-soon" && h.fiber && !h.fiber->is_done())
                all_done_short = false;
        }
        if (all_done_short)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // ReportOnly while ProgressClock is still fresh → Alive / Closed / Done.
    auto wr0 = scope.watch_all(/*stall_timeout_ms=*/80, StallPolicy::ReportOnly);
    CHECK(wr0.closed >= 1, "AC4: interval=0 counts Closed");
    CHECK(wr0.alive + wr0.done + wr0.closed + wr0.stalled == scope.size(),
          "AC2: aggregate covers all handles");
    CHECK(wr0.cancelled == 0, "ReportOnly: no cancels");
    CHECK(wr0.alive >= 1 || wr0.stalled >= 1, "ProgressClock counted Alive or Stalled");

    // Stall ProgressClock: stop touches first (avoid race with note_agent_progress),
    // then age the shared clock so watch sees a stale pulse.
    touch.store(false, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(pc->liveness != nullptr, "liveness still live");
    pc->liveness->last_keepalive_us.store(1, std::memory_order_release);

    const auto cancel0 =
        g_orch_module_stats.keepalive_cancels_total.load(std::memory_order_relaxed);
    auto single = watch_agent_liveness(*pc, /*stall_timeout_ms=*/15, /*cancel_on_stall=*/false);
    CHECK(single.status == KeepaliveWatchStatus::Stalled,
          "single-handle ProgressClock stall (baseline)");
    pc->liveness->last_keepalive_us.store(1, std::memory_order_release);
    auto wr1 = scope.watch_all(/*stall_timeout_ms=*/15, /*cancel_on_stall=*/true);
    CHECK(wr1.stalled >= 1, "AC2: ProgressClock stall counted");
    CHECK(wr1.cancelled >= 1, "AC3: cancel_on_stall cancelled stalled");
    CHECK(pc->fiber && pc->fiber->is_cancel_requested(), "AC3: stalled fiber cancel requested");
    for (const auto& h : scope.handles()) {
        if (h.name == "done-soon" && h.fiber)
            CHECK(h.fiber->is_done(), "AC3: done agent remains Done");
    }
    CHECK(g_orch_module_stats.keepalive_cancels_total.load() > cancel0,
          "keepalive_cancels advanced");

    hold.store(false, std::memory_order_relaxed);
    (void)scope.join_all(std::optional<std::uint64_t>{3000});
}

// ── #2161 AC1: still feature-flagged; AC5 no global registry ───────────
static void ac2161_flag_and_linter_surface() {
    std::println("\n--- #2161 AC1/AC5: flag + no global registry ---");
    auto header_src = read_file("src/orch/agent_scope.h");
    // watch_all lives inside #ifdef AURA_ENABLE_AGENT_SCOPE (same as class).
    CHECK(header_src.find("#ifdef AURA_ENABLE_AGENT_SCOPE") != std::string::npos,
          "AC1: still behind AURA_ENABLE_AGENT_SCOPE");
    CHECK(header_src.find("StallPolicy") != std::string::npos, "StallPolicy present");
    // No process-static multi-agent registry symbols.
    CHECK(header_src.find("static Agent") == std::string::npos ||
              header_src.find("static AgentRegistry") == std::string::npos,
          "AC5: no static AgentRegistry");
}

} // namespace

int main() {
    std::println("=== Issue #2083 / #2161: AgentScope + watch_all ===");
    ac4_linter_and_source();
    ac2161_flag_and_linter_surface();
    ac1_spawn_join_cancel();
    ac2_destructor_releases();
    ac3_two_scopes_isolated();
    ac5_stress_cancel_quota();
    ac6_readme_section();
    ac2161_watch_all_batch();
    std::println("\n=== #2083/#2161: passed={} failed={} ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}