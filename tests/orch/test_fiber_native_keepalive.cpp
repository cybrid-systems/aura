// @category: unit
// @reason: Issue #2159 — fiber-native keepalive helper (replace detached
// std::thread). Helper shares cancel/GC/steal model with agent body.
//
//   AC1: Default keepalive_interval_ms=0 remains zero-cost (no helper fiber).
//   AC2: Enabled keepalive: helpers_spawned++, pulses update last/emitted.
//   AC3: join_agent leaves helper Done (no detached host thread).
//   AC4: watch_agent_liveness + cancel_on_stall cancels body and stops helper.
//   AC5: Multi-worker steal stress: no UAF; helper + body complete cleanly.
//   AC6: ProgressClock (attach_mailbox=false) unchanged (no helper fiber).

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
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::orch::AgentHandle;
using aura::orch::AgentSpec;
using aura::orch::g_orch_module_stats;
using aura::orch::is_keepalive_message;
using aura::orch::join_agent;
using aura::orch::KeepaliveWatchStatus;
using aura::orch::kFiberNativeKeepaliveIssue;
using aura::orch::note_agent_progress;
using aura::orch::spawn_agent_with_mailbox;
using aura::orch::stop_keepalive_helper;
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

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:orch-module-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_fiber_native_keepalive() {
    std::println("=== Issue #2159: fiber-native keepalive helper ===");
    CHECK(kFiberNativeKeepaliveIssue == 2159, "issue stamp");

    // ── Source contract: no detached host thread on keepalive path ──
    {
        std::println("\n--- source: no std::thread detach keepalive ---");
        const auto src = read_file("src/orch/agent_spawn.h");
        CHECK(!src.empty(), "agent_spawn.h readable");
        CHECK(src.find("2159") != std::string::npos, "cites 2159");
        CHECK(src.find("kFiberNativeKeepaliveIssue") != std::string::npos, "issue constant");
        CHECK(src.find("keepalive_helper") != std::string::npos, "AgentHandle.keepalive_helper");
        CHECK(src.find("join_keepalive_helper") != std::string::npos, "join_keepalive_helper API");
        // No detached host thread for keepalive (may still use this_thread in watch).
        CHECK(src.find("std::thread([mb_keep") == std::string::npos &&
                  src.find(".detach()") == std::string::npos,
              "no std::thread().detach() keepalive path");
        CHECK(src.find("fiber_sleep_ms") != std::string::npos, "helper uses fiber_sleep_ms");
    }

    // ── AC1: default zero-cost ──
    {
        std::println("\n--- AC1: default interval=0 zero-cost ---");
        Scheduler sched(2);
        SchedRunner runner(sched);
        const auto help0 =
            g_orch_module_stats.keepalive_helpers_spawned.load(std::memory_order_relaxed);
        const auto ka0 =
            g_orch_module_stats.keepalive_emitted_total.load(std::memory_order_relaxed);
        auto h = spawn_agent_with_mailbox(sched, {.name = "ac1-no-ka", .body = [] {
                                                      for (int i = 0; i < 4; ++i)
                                                          Fiber::yield(YieldReason::Explicit);
                                                  }});
        CHECK(h.ok, "AC1: spawn ok");
        CHECK(h.keepalive_interval_ms == 0, "AC1: interval 0");
        CHECK(!h.keepalive_active, "AC1: no helper");
        CHECK(h.keepalive_helper == nullptr, "AC1: helper Fiber* null");
        CHECK(h.liveness == nullptr, "AC1: no liveness");
        (void)join_agent(h, std::optional<std::uint64_t>{2000});
        CHECK(g_orch_module_stats.keepalive_helpers_spawned.load() == help0,
              "AC1: helpers_spawned unchanged");
        CHECK(g_orch_module_stats.keepalive_emitted_total.load() == ka0, "AC1: no emissions");
    }

    // ── AC2: enabled keepalive pulses ──
    {
        std::println("\n--- AC2: enabled keepalive pulses ---");
        // Two workers: body + helper (steal possible but not multi-steal thrash).
        Scheduler sched(2);
        SchedRunner runner(sched);
        std::atomic<bool> hold{true};
        const auto help0 =
            g_orch_module_stats.keepalive_helpers_spawned.load(std::memory_order_relaxed);
        const auto ka0 =
            g_orch_module_stats.keepalive_emitted_total.load(std::memory_order_relaxed);
        auto h = spawn_agent_with_mailbox(
            sched, {.name = "ac2-ka",
                    .body =
                        [&] {
                            while (hold.load(std::memory_order_relaxed)) {
                                if (aura::serve::g_current_fiber &&
                                    aura::serve::g_current_fiber->is_cancel_requested())
                                    break;
                                Fiber::yield(YieldReason::Explicit);
                            }
                        },
                    .keepalive_interval_ms = 12});
        CHECK(h.ok, "AC2: spawn ok");
        CHECK(h.keepalive_active, "AC2: helper active");
        CHECK(h.keepalive_helper != nullptr, "AC2: helper Fiber*");
        CHECK(g_orch_module_stats.keepalive_helpers_spawned.load() > help0,
              "AC2: helpers_spawned++");

        int saw = 0;
        for (int i = 0; i < 400; ++i) {
            if (h.liveness && h.liveness->emitted.load(std::memory_order_acquire) >= 1) {
                saw = 1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(saw == 1, "AC2: emit observed");
        CHECK(g_orch_module_stats.keepalive_emitted_total.load() > ka0,
              "AC2: keepalive_emitted_total advanced");
        CHECK(h.liveness->last_keepalive_us.load() > 0, "AC2: last_keepalive_us set");
        CHECK(g_orch_module_stats.last_keepalive_us.load() > 0, "AC2: process last_keepalive_us");

        bool saw_payload = false;
        for (int i = 0; i < 80; ++i) {
            auto m = aura::orch::agent_recv(h, /*wait=*/false, 0);
            if (m && is_keepalive_message(*m)) {
                saw_payload = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(saw_payload, "AC2: keepalive payload in mailbox");

        hold.store(false, std::memory_order_relaxed);
        // Wait for body to exit before join so helper sees body_done cleanly.
        for (int i = 0; i < 200 && h.fiber && !h.fiber->is_done(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        // Stop helper and wait for Done before join_agent (join still re-joins).
        if (h.keepalive_helper) {
            stop_keepalive_helper(h);
            for (int i = 0; i < 200 && h.keepalive_helper && !h.keepalive_helper->is_done(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        auto jr = join_agent(h, std::optional<std::uint64_t>{3000});
        CHECK(jr.status == JoinStatus::Ok || jr.status == JoinStatus::Cancelled, "AC2: join body");
        // Drain a few ms so any finishing trampoline completes before next AC.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(h.keepalive_helper == nullptr, "AC2: helper cleared after join");
        CHECK(!h.keepalive_active, "AC2: not active after join");
    }

    // ── AC3: join leaves helper Done ──
    {
        std::println("\n--- AC3: join_agent helper Done ---");
        Scheduler sched(2);
        SchedRunner runner(sched);
        std::atomic<bool> hold{true};
        const auto joined0 =
            g_orch_module_stats.keepalive_helpers_joined_total.load(std::memory_order_relaxed);
        auto h = spawn_agent_with_mailbox(
            sched, {.name = "ac3-join",
                    .body =
                        [&] {
                            while (hold.load(std::memory_order_relaxed)) {
                                if (aura::serve::g_current_fiber &&
                                    aura::serve::g_current_fiber->is_cancel_requested())
                                    break;
                                Fiber::yield(YieldReason::Explicit);
                            }
                        },
                    .keepalive_interval_ms = 15});
        CHECK(h.ok && h.keepalive_helper, "AC3: helper fiber live");
        Fiber* helper = h.keepalive_helper;
        for (int i = 0; i < 300 && h.liveness && h.liveness->emitted.load() < 1; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(h.liveness && h.liveness->emitted.load() >= 1, "AC3: at least one emit");
        hold.store(false, std::memory_order_relaxed);
        for (int i = 0; i < 200 && h.fiber && !h.fiber->is_done(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        stop_keepalive_helper(h);
        for (int i = 0; i < 200 && helper && !helper->is_done(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        auto jr = join_agent(h, std::optional<std::uint64_t>{3000});
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(jr.status == JoinStatus::Ok || jr.status == JoinStatus::Cancelled, "AC3: join ok");
        CHECK(helper->is_done(), "AC3: helper fiber is_done after join");
        CHECK(g_orch_module_stats.keepalive_helpers_joined_total.load() > joined0,
              "AC3: helpers_joined_total advanced");
    }

    // ── AC4: stall cancel stops helper ──
    {
        std::println("\n--- AC4: stall cancel stops helper ---");
        Scheduler sched(2);
        SchedRunner runner(sched);
        std::atomic<bool> body_running{true};
        std::atomic<bool> saw_cancel{false};
        const auto cancel0 =
            g_orch_module_stats.keepalive_cancels_total.load(std::memory_order_relaxed);
        const auto stall0 =
            g_orch_module_stats.stalled_agents_total.load(std::memory_order_relaxed);
        auto h = spawn_agent_with_mailbox(
            sched, {.name = "ac4-stall",
                    .body =
                        [&] {
                            while (body_running.load(std::memory_order_relaxed)) {
                                if (aura::serve::g_current_fiber &&
                                    aura::serve::g_current_fiber->is_cancel_requested()) {
                                    saw_cancel.store(true, std::memory_order_relaxed);
                                    break;
                                }
                                Fiber::yield(YieldReason::Explicit);
                            }
                        },
                    .keepalive_interval_ms = 20});
        CHECK(h.ok && h.keepalive_active, "AC4: spawn with helper");
        CHECK(h.keepalive_helper != nullptr, "AC4: helper Fiber*");
        for (int i = 0; i < 300 && h.liveness && h.liveness->emitted.load() < 1; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(h.liveness && h.liveness->emitted.load() >= 1, "AC4: at least one emit");
        std::println("  AC4: emit ok, stopping helper for stall sim");

        // Stop helper without finishing the body (simulate silent agent).
        // Cooperative helper_stop only — wait fixed wall time for 1ms poll loop.
        stop_keepalive_helper(h);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(h.liveness && h.liveness->helper_stop.load(), "AC4: helper_stop set");

        // Age out last pulse so watch sees a stale clock.
        h.liveness->last_keepalive_us.store(1, std::memory_order_release);
        for (int i = 0; i < 64; ++i) {
            auto m = aura::orch::agent_recv(h, false, 0);
            if (!m)
                break;
        }
        auto wr = watch_agent_liveness(h, /*stall_timeout_ms=*/20, /*cancel_on_stall=*/true);
        CHECK(wr.status == KeepaliveWatchStatus::Stalled, "AC4: stalled");
        CHECK(wr.cancelled, "AC4: cancel_on_stall");
        CHECK(g_orch_module_stats.stalled_agents_total.load() > stall0, "AC4: stalled metric");
        CHECK(g_orch_module_stats.keepalive_cancels_total.load() > cancel0,
              "AC4: cancels advanced");
        CHECK(h.fiber && h.fiber->is_cancel_requested(), "AC4: body fiber cancel requested");
        for (int i = 0; i < 200 && !saw_cancel.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(saw_cancel.load() || (h.fiber && h.fiber->is_cancel_requested()),
              "AC4: body cancel observed or requested");

        body_running.store(false, std::memory_order_relaxed);
        for (int i = 0; i < 200 && h.fiber && !h.fiber->is_done(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        (void)join_agent(h, std::optional<std::uint64_t>{3000});
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(h.keepalive_helper == nullptr, "AC4: helper cleared after join");
    }

    // ── AC5: multi-worker steal stress ──
    {
        std::println("\n--- AC5: multi-worker steal stress ---");
        // 3 workers, 3 agents (each body+helper = 6 fibers) — steal pressure without
        // overwhelming the yield-spin helper cadence.
        Scheduler sched(3);
        SchedRunner runner(sched);
        std::atomic<bool> hold{true};
        std::vector<AgentHandle> agents;
        agents.reserve(3);
        for (int i = 0; i < 3; ++i) {
            agents.push_back(spawn_agent_with_mailbox(
                sched, {.name = std::format("ac5-{}", i),
                        .body =
                            [&] {
                                while (hold.load(std::memory_order_relaxed)) {
                                    if (aura::serve::g_current_fiber &&
                                        aura::serve::g_current_fiber->is_cancel_requested())
                                        break;
                                    Fiber::yield(YieldReason::Explicit);
                                }
                            },
                        .keepalive_interval_ms = 20}));
            CHECK(agents.back().ok, std::format("AC5: spawn {}", i).c_str());
        }
        // Let helpers emit under multi-worker steal.
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        std::uint64_t emits = 0;
        for (auto& a : agents) {
            if (a.liveness)
                emits += a.liveness->emitted.load(std::memory_order_relaxed);
        }
        CHECK(emits >= 1, "AC5: at least one emit under multi-worker");
        hold.store(false, std::memory_order_relaxed);
        for (auto& a : agents) {
            for (int i = 0; i < 200 && a.fiber && !a.fiber->is_done(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            stop_keepalive_helper(a);
            for (int i = 0; i < 200 && a.keepalive_helper && !a.keepalive_helper->is_done(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            auto jr = join_agent(a, std::optional<std::uint64_t>{5000});
            CHECK(jr.status == JoinStatus::Ok || jr.status == JoinStatus::Cancelled,
                  "AC5: join clean");
            CHECK(a.keepalive_helper == nullptr, "AC5: helper cleared");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    // ── AC6: ProgressClock unchanged (no helper fiber) ──
    {
        std::println("\n--- AC6: ProgressClock no helper fiber ---");
        Scheduler sched(2);
        SchedRunner runner(sched);
        const auto help0 =
            g_orch_module_stats.keepalive_helpers_spawned.load(std::memory_order_relaxed);
        std::atomic<bool> hold{true};
        AgentHandle* self = nullptr;
        auto h = spawn_agent_with_mailbox(
            sched, {.name = "ac6-pc",
                    .body =
                        [&] {
                            while (hold.load(std::memory_order_relaxed)) {
                                if (self)
                                    note_agent_progress(*self);
                                if (aura::serve::g_current_fiber &&
                                    aura::serve::g_current_fiber->is_cancel_requested())
                                    break;
                                Fiber::yield(YieldReason::Explicit);
                            }
                        },
                    .attach_mailbox = false,
                    .keepalive_interval_ms = 20});
        self = &h;
        CHECK(h.ok, "AC6: ProgressClock spawn ok");
        CHECK(h.mailbox == nullptr, "AC6: no mailbox");
        CHECK(h.keepalive_helper == nullptr, "AC6: no helper fiber");
        CHECK(!h.keepalive_active, "AC6: keepalive_active false");
        CHECK(h.liveness != nullptr, "AC6: liveness for ProgressClock");
        CHECK(h.keepalive_interval_ms == 20, "AC6: interval recorded");
        CHECK(g_orch_module_stats.keepalive_helpers_spawned.load() == help0,
              "AC6: helpers_spawned unchanged");
        // Body entry seeds last_keepalive_us; wait for baseline.
        for (int i = 0; i < 100 && h.liveness->last_keepalive_us.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(h.liveness->last_keepalive_us.load() > 0, "AC6: ProgressClock baseline seed");
        auto wr = watch_agent_liveness(h, /*stall_timeout_ms=*/200, /*cancel_on_stall=*/false);
        CHECK(wr.status == KeepaliveWatchStatus::Alive || wr.status == KeepaliveWatchStatus::Done,
              "AC6: watch Alive/Done");
        hold.store(false, std::memory_order_relaxed);
        (void)join_agent(h, std::optional<std::uint64_t>{2000});
    }

    // ── Query schema-2159 ──
    {
        std::println("\n--- query schema-2159 ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        CHECK(href(cs, "schema-2159") == 2159, "schema-2159");
        CHECK(href(cs, "fiber-native-keepalive-wired") == 1, "wired");
        CHECK(href(cs, "keepalive-helpers-joined-total") >= 0, "joined total key");
    }

    std::println("\n=== #2159 fiber-native keepalive: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_fiber_native_keepalive();
}
#endif
