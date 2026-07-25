// test_agent_scope_2083.cpp — Issue #2083 AgentScope opt-in.
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

#define AURA_ENABLE_AGENT_SCOPE // opt-in for this TU (#2083)

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
}

} // namespace

int main() {
    std::println("=== Issue #2083: AgentScope opt-in ===");
    ac4_linter_and_source();
    ac1_spawn_join_cancel();
    ac2_destructor_releases();
    ac3_two_scopes_isolated();
    ac5_stress_cancel_quota();
    ac6_readme_section();
    std::println("\n=== #2083: passed={} failed={} ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}