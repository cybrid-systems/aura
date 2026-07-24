// agent_spawn.h — Issue #1588 / #1879 / #1880: unified agent spawn.
//
// STATUS: Advanced / Experimental (Issue #1945, 2026-07 through 2026-10).
// See docs/agent-orchestration-status.md for MVP scope + status.
// Single-agent MVP (spawn + join + send/recv + AgentHandle/AgentSpec +
// OrchModuleStats) is production-safe.
//
// Issue #1966: multi-agent public surface removed from orch/:
//   - AgentRegistry / global_agent_registry → evaluator-local name table
//     (orch:spawn-agent / orch:agent-join bookkeeping only)
//   - conduct_parallel → use serve::parallel_orch::parallel_intend
// Linter: scripts/check_orch_mvp_scope.py --strict (reintroduction guard).
// Header API under aura::orch; pairs with serve/parallel_orch and multi_fiber_mailbox.
// Issue #1879: spawn body exit + join force StableNodeRef provenance refresh.
// Issue #1880: ResourceQuota preflight (arena/mailbox/fibers) + try_acquire
// body wrapper (typed ResourceQuotaExceeded, no panic).
// Issue #2008: opt-in agent keepalive / heartbeat (mailbox-native; default off).

#ifndef AURA_ORCH_AGENT_SPAWN_H
#define AURA_ORCH_AGENT_SPAWN_H

#include "core/resource_quota.hh"
#include "serve/fiber.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/parallel_orch.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Evaluator hooks (strong defs in evaluator_fiber_mutation.cpp; weak no-ops
// in fiber_bridge.cpp for serve-only link units).
extern "C" void aura_evaluator_post_resume_refresh();
extern "C" void aura_evaluator_on_fiber_join(void* joined_fiber);
// Issue #1880: MutationBoundaryGuard::try_acquire around agent body (0=ok, 1=reject).
extern "C" int aura_orch_agent_body_try_acquire();
extern "C" void aura_orch_agent_body_release_guard();

namespace aura::orch {

inline constexpr int kOrchModulePhase = 4; // #1881 orch health observability
inline constexpr int kOrchModuleIssue = 1881;

// Estimated per-agent arena footprint + mailbox high-water bytes (#1880).
inline constexpr std::uint64_t kOrchAgentArenaBytes = 4096;
inline constexpr std::uint64_t kOrchMailboxSlotBytes = 64;

[[nodiscard]] inline std::uint64_t estimate_agent_memory_bytes(std::size_t mailbox_high_water,
                                                               bool attach_mailbox) noexcept {
    std::uint64_t n = kOrchAgentArenaBytes;
    if (attach_mailbox)
        n += static_cast<std::uint64_t>(mailbox_high_water) * kOrchMailboxSlotBytes;
    return n;
}

// ── Process-wide orch module stats ─────────────────────
struct OrchModuleStats {
    std::atomic<std::uint64_t> agents_spawned{0};
    std::atomic<std::uint64_t> agents_joined{0};
    std::atomic<std::uint64_t> agents_send{0};
    std::atomic<std::uint64_t> agents_recv{0};
    std::atomic<std::uint64_t> spawn_failures{0};
    std::atomic<std::uint64_t> parallel_batches{0};
    // Issue #1600
    std::atomic<std::uint64_t> spawn_quota_rejects{0};
    // Issue #1879: StableNodeRef + linear ownership on orch spawn/join/steal.
    std::atomic<std::uint64_t> stable_ref_auto_refresh_total{0};
    std::atomic<std::uint64_t> fiber_steal_provenance_enforced_total{0};
    std::atomic<std::uint64_t> linear_violation_prevented_total{0};
    // Issue #1880: ResourceQuota rejects + try_acquire body rejects.
    std::atomic<std::uint64_t> resource_quota_rejects_total{0};
    std::atomic<std::uint64_t> agent_body_try_acquire_rejects_total{0};
    std::atomic<std::uint64_t> agent_body_try_acquire_ok_total{0};
    // Issue #1881: observability — hot-path counters (no dead bumps).
    std::atomic<std::uint64_t> agents_active{0}; // spawn - joined (approx live)
    std::atomic<std::uint64_t> send_backpressure_total{0};
    std::atomic<std::uint64_t> send_closed_total{0};
    std::atomic<std::uint64_t> recv_empty_total{0};
    std::atomic<std::uint64_t> join_wait_us_total{0};
    std::atomic<std::uint64_t> join_ok_total{0};
    std::atomic<std::uint64_t> join_fail_total{0};
    // Issue #2008: keepalive / liveness.
    std::atomic<std::uint64_t> keepalive_emitted_total{0};
    std::atomic<std::uint64_t> stalled_agents_total{0};
    std::atomic<std::uint64_t> last_keepalive_us{0}; // process-wide most recent emit
    std::atomic<std::uint64_t> keepalive_cancels_total{0};
    std::atomic<std::uint64_t> keepalive_helpers_spawned{0};
    std::atomic<std::uint64_t> keepalive_helper_spawn_fail{0};
};

// Issue #2008: conventional mailbox keepalive payload prefix.
// Payload form: "keepalive:<steady_us>" with MailPriority::High.
inline constexpr std::string_view kKeepalivePrefix = "keepalive:";

[[nodiscard]] inline bool is_keepalive_payload(std::string_view payload) noexcept {
    return payload.size() >= kKeepalivePrefix.size() &&
           payload.compare(0, kKeepalivePrefix.size(), kKeepalivePrefix) == 0;
}

[[nodiscard]] inline bool is_keepalive_message(const serve::mf_mailbox::MailMessage& msg) noexcept {
    return is_keepalive_payload(msg.payload);
}

// Shared liveness state between body fiber, keepalive helper, and supervisor.
struct AgentLiveness {
    std::atomic<bool> body_done{false};
    std::atomic<bool> helper_stop{false}; // stop keepalive without marking body done
    std::atomic<std::uint64_t> last_keepalive_us{0};
    std::atomic<std::uint64_t> emitted{0};
    std::uint32_t interval_ms = 0;
};

[[nodiscard]] inline std::uint64_t orch_now_us() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Cooperative sleep for keepalive cadence (steal-friendly yield loop).
inline void fiber_sleep_ms(std::uint32_t ms) noexcept {
    if (ms == 0) {
        serve::Fiber::yield(serve::YieldReason::Explicit);
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (serve::g_current_fiber && serve::g_current_fiber->is_cancel_requested())
            return;
        serve::Fiber::yield(serve::YieldReason::Explicit);
    }
}

inline OrchModuleStats g_orch_module_stats{};

inline void snapshot_orch_stats(std::uint64_t& spawned, std::uint64_t& joined, std::uint64_t& sends,
                                std::uint64_t& recvs, std::uint64_t& failures,
                                std::uint64_t& parallel_batches) noexcept {
    spawned = g_orch_module_stats.agents_spawned.load(std::memory_order_relaxed);
    joined = g_orch_module_stats.agents_joined.load(std::memory_order_relaxed);
    sends = g_orch_module_stats.agents_send.load(std::memory_order_relaxed);
    recvs = g_orch_module_stats.agents_recv.load(std::memory_order_relaxed);
    failures = g_orch_module_stats.spawn_failures.load(std::memory_order_relaxed);
    parallel_batches = g_orch_module_stats.parallel_batches.load(std::memory_order_relaxed);
}

// Issue #1879: orch-specific provenance counters for Agent dashboards.
inline void snapshot_orch_provenance_stats(std::uint64_t& stable_ref_refresh,
                                           std::uint64_t& steal_provenance,
                                           std::uint64_t& linear_prevented) noexcept {
    stable_ref_refresh =
        g_orch_module_stats.stable_ref_auto_refresh_total.load(std::memory_order_relaxed);
    steal_provenance =
        g_orch_module_stats.fiber_steal_provenance_enforced_total.load(std::memory_order_relaxed);
    linear_prevented =
        g_orch_module_stats.linear_violation_prevented_total.load(std::memory_order_relaxed);
}

// Force full post-resume/steal closed loop after agent body (EnvFrame refresh +
// StableNodeRef restamp + linear probe). No-op when no Evaluator is wired.
inline void orch_agent_body_exit_provenance() noexcept {
    aura_evaluator_post_resume_refresh();
    g_orch_module_stats.fiber_steal_provenance_enforced_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
    g_orch_module_stats.stable_ref_auto_refresh_total.fetch_add(1, std::memory_order_relaxed);
    g_orch_module_stats.linear_violation_prevented_total.fetch_add(1, std::memory_order_relaxed);
}

// Force post-join linear + StableNodeRef enforcement (also covers the case
// Fiber::join skips host work when called from a fiber stack).
inline void orch_post_join_provenance(serve::Fiber* fiber) noexcept {
    if (fiber)
        aura_evaluator_on_fiber_join(static_cast<void*>(fiber));
    g_orch_module_stats.stable_ref_auto_refresh_total.fetch_add(1, std::memory_order_relaxed);
    g_orch_module_stats.linear_violation_prevented_total.fetch_add(1, std::memory_order_relaxed);
}

// ── Agent handle ───────────────────────────────────────
struct AgentHandle {
    std::uint64_t id = 0; // Fiber::id()
    std::string name;
    serve::Fiber* fiber = nullptr;
    std::shared_ptr<serve::mf_mailbox::MultiFiberMailbox> mailbox;
    bool ok = false;
    // Issue #1600 / #1880: typed quota failure surface for Agent frameworks.
    bool quota_exceeded = false;
    std::string error; // e.g. "ResourceQuotaExceeded: fibers quota exceeded"
    // Issue #1880: memory reserved at spawn (released on join / spawn fail).
    std::uint64_t reserved_memory_bytes = 0;
    // Issue #2008: keepalive / liveness (null / 0 when disabled — zero cost).
    std::uint32_t keepalive_interval_ms = 0;
    std::shared_ptr<AgentLiveness> liveness; // shared body ↔ helper ↔ supervisor
    // True when a detached host keepalive thread was started for this agent.
    bool keepalive_active = false;
};

struct AgentSpec {
    std::string name;
    std::function<void()> body; // required for spawn
    bool attach_mailbox = true;
    std::size_t mailbox_high_water = 256;
    // Issue #2008: 0 = disabled (default, zero-cost). When > 0 and mailbox
    // attached, a helper fiber emits "keepalive:<us>" at this cadence.
    std::uint32_t keepalive_interval_ms = 0;
};

// Spawn a fiber agent on `sched`, optionally with a private MultiFiberMailbox
// attached to the running fiber (attach happens inside the fiber so g_current_fiber
// is valid).
// Issue #1880: preflight ResourceQuota (fibers + estimated arena/mailbox memory)
// with typed ResourceQuotaExceeded (never panic). Agent body uses
// MutationBoundaryGuard::try_acquire when an Evaluator is wired.
// Emit one keepalive into `mb` and bump process + shared liveness clocks.
// Safe under backpressure (push may fail; last_keepalive only advances on Ok).
// Payload is a fixed short string to keep fiber-stack work minimal; epoch is
// carried only on AgentLiveness / process stats (not in the payload body).
inline serve::mf_mailbox::PushStatus emit_keepalive(serve::mf_mailbox::MultiFiberMailbox& mb,
                                                    std::uint64_t agent_fiber_id,
                                                    AgentLiveness* live) {
    const auto now = orch_now_us();
    serve::mf_mailbox::MailMessage msg;
    msg.from_fiber = agent_fiber_id;
    msg.to_fiber = 0;
    msg.priority = serve::mf_mailbox::MailPriority::High;
    // Fixed payload: "keepalive:" — supervisors use is_keepalive_payload;
    // precise epoch lives in live->last_keepalive_us / process stats.
    msg.payload.assign(kKeepalivePrefix.data(), kKeepalivePrefix.size());
    auto st = mb.push(std::move(msg));
    if (st == serve::mf_mailbox::PushStatus::Ok) {
        if (live) {
            live->last_keepalive_us.store(now, std::memory_order_release);
            live->emitted.fetch_add(1, std::memory_order_relaxed);
        }
        g_orch_module_stats.last_keepalive_us.store(now, std::memory_order_relaxed);
        g_orch_module_stats.keepalive_emitted_total.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.agents_send.fetch_add(1, std::memory_order_relaxed);
    } else if (st == serve::mf_mailbox::PushStatus::Backpressure) {
        g_orch_module_stats.send_backpressure_total.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_orch_module_stats.send_closed_total.fetch_add(1, std::memory_order_relaxed);
    }
    return st;
}

[[nodiscard]] inline AgentHandle spawn_agent_with_mailbox(serve::Scheduler& sched, AgentSpec spec) {
    AgentHandle h;
    h.name = std::move(spec.name);
    if (!spec.body) {
        g_orch_module_stats.spawn_failures.fetch_add(1, std::memory_order_relaxed);
        return h;
    }

    auto& pq = aura::core::resource_quota::process_resource_quota();

    // Issue #2008: keepalive uses a host thread (not an extra fiber).
    const bool want_keepalive = spec.keepalive_interval_ms > 0 && spec.attach_mailbox;

    // Issue #1880: fiber capacity preflight (check only; Scheduler::spawn also consumes).
    if (auto ferr = pq.check_orchestration_fibers(/*amount=*/1)) {
        g_orch_module_stats.spawn_failures.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.spawn_quota_rejects.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.resource_quota_rejects_total.fetch_add(1, std::memory_order_relaxed);
        pq.orch_resource_quota_rejects_total.fetch_add(1, std::memory_order_relaxed);
        // #1600: align preflight reject with Scheduler::spawn metric surface.
        pq.fiber_spawn_rejected_total.fetch_add(1, std::memory_order_relaxed);
        h.quota_exceeded = true;
        h.error = "ResourceQuotaExceeded: " + ferr->message;
        return h;
    }

    // Issue #1880: arena + mailbox high-water memory reservation.
    const auto mem_cost = estimate_agent_memory_bytes(spec.mailbox_high_water, spec.attach_mailbox);
    if (auto merr = pq.try_consume_agent_arena(mem_cost)) {
        g_orch_module_stats.spawn_failures.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.spawn_quota_rejects.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.resource_quota_rejects_total.fetch_add(1, std::memory_order_relaxed);
        h.quota_exceeded = true;
        h.error = "ResourceQuotaExceeded: " +
                  aura::core::resource_quota::ResourceQuotaManager::format_reason(*merr);
        return h;
    }
    h.reserved_memory_bytes = mem_cost;

    auto mb = spec.attach_mailbox
                  ? std::make_shared<serve::mf_mailbox::MultiFiberMailbox>(spec.mailbox_high_water)
                  : nullptr;
    auto body = std::move(spec.body);
    auto attach = spec.attach_mailbox;
    const auto ka_interval = want_keepalive ? spec.keepalive_interval_ms : 0u;
    std::shared_ptr<AgentLiveness> live;
    if (want_keepalive) {
        live = std::make_shared<AgentLiveness>();
        live->interval_ms = ka_interval;
    }

    serve::Fiber* f = sched.spawn([body = std::move(body), mb, attach, live]() mutable {
        if (attach && mb && serve::g_current_fiber)
            mb->attach(serve::g_current_fiber);
        // Issue #1880: try_acquire mutation boundary when Evaluator is bound.
        // On reject: skip body (typed quota path already recorded); no panic.
        // Issue #2006: provenance closed-loop only after a successful body
        // that actually entered the acquire path — reject must not call
        // aura_evaluator_post_resume_refresh or bump provenance counters.
        const int acq = aura_orch_agent_body_try_acquire();
        if (acq == 0) {
            g_orch_module_stats.agent_body_try_acquire_ok_total.fetch_add(
                1, std::memory_order_relaxed);
            body();
            aura_orch_agent_body_release_guard();
            // Issue #1879: after successful agent body, force StableNodeRef
            // provenance validation + auto pin/refresh + linear ownership
            // probe so COW / steal / GC cannot leave dangling refs for join.
            orch_agent_body_exit_provenance();
        } else {
            g_orch_module_stats.agent_body_try_acquire_rejects_total.fetch_add(
                1, std::memory_order_relaxed);
            g_orch_module_stats.resource_quota_rejects_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
        }
        // Issue #2008: signal keepalive helper to stop.
        if (live)
            live->body_done.store(true, std::memory_order_release);
        if (attach && mb && serve::g_current_fiber)
            mb->detach(serve::g_current_fiber);
    });

    if (!f) {
        // Issue #1600: Scheduler::spawn returns nullptr on fiber ResourceQuota.
        pq.release_agent_arena(mem_cost);
        h.reserved_memory_bytes = 0;
        g_orch_module_stats.spawn_failures.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.spawn_quota_rejects.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.resource_quota_rejects_total.fetch_add(1, std::memory_order_relaxed);
        h.quota_exceeded = true;
        h.error = "ResourceQuotaExceeded: fibers quota exceeded";
        return h;
    }

    h.fiber = f;
    h.id = f->id();
    h.mailbox = mb;
    h.ok = true;
    h.keepalive_interval_ms = ka_interval;
    h.liveness = live;
    g_orch_module_stats.agents_spawned.fetch_add(1, std::memory_order_relaxed);
    g_orch_module_stats.agents_active.fetch_add(1, std::memory_order_relaxed);

    // Issue #2008: optional host-side keepalive thread (mailbox-native pulses).
    // Uses a std::thread rather than a second fiber so multi-worker steal cannot
    // UAF the helper against MultiFiberMailbox; default path still zero-cost.
    if (want_keepalive && mb && live) {
        const auto agent_id = h.id;
        const auto interval = ka_interval;
        auto mb_keep = mb; // shared ownership with handle
        auto live_keep = live;
        try {
            // Detached host thread: holds shared_ptr copies of mb + live so it
            // remains valid until the thread observes stop and exits.
            std::thread([mb_keep, live_keep, agent_id, interval]() {
                if (!mb_keep || !live_keep)
                    return;
                auto should_stop = [&]() noexcept {
                    return live_keep->body_done.load(std::memory_order_acquire) ||
                           live_keep->helper_stop.load(std::memory_order_acquire);
                };
                // Immediate first pulse.
                (void)emit_keepalive(*mb_keep, agent_id, live_keep.get());
                while (!should_stop()) {
                    const auto slice = std::max<std::uint32_t>(1, interval);
                    for (std::uint32_t slept = 0; slept < slice && !should_stop();) {
                        const auto step = std::min<std::uint32_t>(5, slice - slept);
                        std::this_thread::sleep_for(std::chrono::milliseconds(step));
                        slept += step;
                    }
                    if (should_stop())
                        break;
                    (void)emit_keepalive(*mb_keep, agent_id, live_keep.get());
                }
            }).detach();
            h.keepalive_active = true;
            g_orch_module_stats.keepalive_helpers_spawned.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            g_orch_module_stats.keepalive_helper_spawn_fail.fetch_add(1, std::memory_order_relaxed);
            h.keepalive_interval_ms = 0;
            h.keepalive_active = false;
        }
    }

    return h;
}

// Issue #1880: release spawn-time memory reservation (idempotent).
inline void release_agent_memory_reservation(AgentHandle& h) noexcept {
    if (h.reserved_memory_bytes == 0)
        return;
    aura::core::resource_quota::process_resource_quota().release_agent_arena(
        h.reserved_memory_bytes);
    h.reserved_memory_bytes = 0;
}

// Stop keepalive helper (if any). Detached host thread observes this and exits;
// shared_ptr captures keep mailbox/liveness alive until the thread ends.
// Does not mark body_done — reserved for body exit so supervisors can
// distinguish Done vs Stalled.
inline void stop_keepalive_helper(AgentHandle& h) noexcept {
    if (h.liveness)
        h.liveness->helper_stop.store(true, std::memory_order_release);
    h.keepalive_active = false;
}

// Join a single agent (Fiber::join) + Issue #1879 post-join provenance.
// Issue #2008: signals the detached keepalive host thread to exit.
[[nodiscard]] inline serve::JoinResult join_agent(AgentHandle& h,
                                                  std::optional<std::uint64_t> timeout_ms = {}) {
    if (!h.ok || !h.fiber) {
        serve::JoinResult r;
        r.status = serve::JoinStatus::Invalid;
        return r;
    }
    // Issue #2008: stop host keepalive first (non-blocking signal).
    stop_keepalive_helper(h);
    if (h.liveness)
        h.liveness->body_done.store(true, std::memory_order_release);

    auto jr = serve::Fiber::join(h.fiber, timeout_ms);
    g_orch_module_stats.agents_joined.fetch_add(1, std::memory_order_relaxed);
    g_orch_module_stats.join_wait_us_total.fetch_add(jr.wait_us, std::memory_order_relaxed);
    if (jr.status == serve::JoinStatus::Ok)
        g_orch_module_stats.join_ok_total.fetch_add(1, std::memory_order_relaxed);
    else
        g_orch_module_stats.join_fail_total.fetch_add(1, std::memory_order_relaxed);
    // agents_active: best-effort (never go below 0).
    {
        auto cur = g_orch_module_stats.agents_active.load(std::memory_order_relaxed);
        for (;;) {
            const auto next = cur > 0 ? cur - 1 : 0;
            if (g_orch_module_stats.agents_active.compare_exchange_weak(
                    cur, next, std::memory_order_acq_rel, std::memory_order_relaxed))
                break;
        }
    }
    // Issue #1879: mandate join-path StableNodeRef / linear enforcement
    // even when Fiber::join skipped host refresh (nested fiber join).
    if (jr.status == serve::JoinStatus::Ok)
        orch_post_join_provenance(h.fiber);
    // Issue #1880: free arena/mailbox reservation after join.
    release_agent_memory_reservation(h);
    return jr;
}

// Join many agents + Issue #1879 post-join provenance per fiber.
// Issue #2008: stop keepalive helpers, then join bodies.
[[nodiscard]] inline serve::JoinResult join_agents(std::span<AgentHandle> agents,
                                                   std::optional<std::uint64_t> timeout_ms = {}) {
    for (auto& a : agents) {
        stop_keepalive_helper(a);
        if (a.liveness)
            a.liveness->body_done.store(true, std::memory_order_release);
    }

    std::vector<serve::Fiber*> fibers;
    fibers.reserve(agents.size());
    for (auto& a : agents) {
        if (a.ok && a.fiber)
            fibers.push_back(a.fiber);
    }
    if (fibers.empty()) {
        serve::JoinResult r;
        r.status = serve::JoinStatus::Invalid;
        return r;
    }
    auto jr = serve::Fiber::join(std::span<serve::Fiber* const>(fibers), timeout_ms);
    g_orch_module_stats.agents_joined.fetch_add(fibers.size(), std::memory_order_relaxed);
    g_orch_module_stats.join_wait_us_total.fetch_add(jr.wait_us, std::memory_order_relaxed);
    if (jr.status == serve::JoinStatus::Ok)
        g_orch_module_stats.join_ok_total.fetch_add(fibers.size(), std::memory_order_relaxed);
    else
        g_orch_module_stats.join_fail_total.fetch_add(1, std::memory_order_relaxed);
    {
        auto cur = g_orch_module_stats.agents_active.load(std::memory_order_relaxed);
        const auto n = static_cast<std::uint64_t>(fibers.size());
        for (;;) {
            const auto next = cur > n ? cur - n : 0;
            if (g_orch_module_stats.agents_active.compare_exchange_weak(
                    cur, next, std::memory_order_acq_rel, std::memory_order_relaxed))
                break;
        }
    }
    if (jr.status == serve::JoinStatus::Ok) {
        for (auto& a : agents) {
            if (a.fiber)
                orch_post_join_provenance(a.fiber);
        }
    }
    // Issue #1880: release per-handle memory reservations.
    for (auto& a : agents)
        release_agent_memory_reservation(a);
    return jr;
}

// Send a message to an agent's mailbox (if any).
// Issue #1881: bump all outcomes (ok / backpressure / closed) — no dead path.
[[nodiscard]] inline serve::mf_mailbox::PushStatus agent_send(AgentHandle& h,
                                                              serve::mf_mailbox::MailMessage msg) {
    if (!h.ok || !h.mailbox) {
        g_orch_module_stats.send_closed_total.fetch_add(1, std::memory_order_relaxed);
        return serve::mf_mailbox::PushStatus::Closed;
    }
    msg.to_fiber = h.id;
    auto st = h.mailbox->push(std::move(msg));
    if (st == serve::mf_mailbox::PushStatus::Ok)
        g_orch_module_stats.agents_send.fetch_add(1, std::memory_order_relaxed);
    else if (st == serve::mf_mailbox::PushStatus::Backpressure)
        g_orch_module_stats.send_backpressure_total.fetch_add(1, std::memory_order_relaxed);
    else
        g_orch_module_stats.send_closed_total.fetch_add(1, std::memory_order_relaxed);
    return st;
}

// Blocking/non-blocking recv on agent mailbox.
// Issue #1881: bump empty/timeout path (recv_empty) as well as success.
[[nodiscard]] inline std::optional<serve::mf_mailbox::MailMessage>
agent_recv(AgentHandle& h, bool wait = true, int timeout_ms = -1) {
    if (!h.ok || !h.mailbox) {
        g_orch_module_stats.recv_empty_total.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    auto m = h.mailbox->recv(wait, timeout_ms, h.id);
    if (m)
        g_orch_module_stats.agents_recv.fetch_add(1, std::memory_order_relaxed);
    else
        g_orch_module_stats.recv_empty_total.fetch_add(1, std::memory_order_relaxed);
    return m;
}

// ── Issue #2008: supervisor liveness watch ─────────────
enum class KeepaliveWatchStatus : std::uint8_t {
    Alive = 0,   // keepalive (or activity) observed within stall window
    Stalled = 1, // no keepalive within stall window
    Done = 2,    // agent body finished
    Closed = 3,  // invalid handle / keepalive disabled / no mailbox
};

struct KeepaliveWatchResult {
    KeepaliveWatchStatus status = KeepaliveWatchStatus::Closed;
    std::uint64_t last_keepalive_us = 0;
    bool cancelled = false; // true when cancel_on_stall fired request_cancel
    std::optional<serve::mf_mailbox::MailMessage> message;
};

// Wait up to stall_timeout_ms (default 2× keepalive_interval_ms) for a
// keepalive. Prefers the shared last_keepalive clock (set by the helper
// fiber) and only does non-blocking mailbox peeks — safe from host threads
// concurrent with the keepalive helper. On stall, optionally request_cancel
// the agent body + helper and bump stalled_agents_total / keepalive_cancels_total.
[[nodiscard]] inline KeepaliveWatchResult watch_agent_liveness(AgentHandle& h,
                                                               std::uint32_t stall_timeout_ms = 0,
                                                               bool cancel_on_stall = true) {
    KeepaliveWatchResult out;
    if (!h.ok || !h.mailbox || h.keepalive_interval_ms == 0) {
        out.status = KeepaliveWatchStatus::Closed;
        return out;
    }
    if (h.liveness && h.liveness->body_done.load(std::memory_order_acquire)) {
        out.status = KeepaliveWatchStatus::Done;
        out.last_keepalive_us = h.liveness->last_keepalive_us.load(std::memory_order_relaxed);
        return out;
    }

    const std::uint32_t stall_ms = stall_timeout_ms > 0
                                       ? stall_timeout_ms
                                       : std::max<std::uint32_t>(1, h.keepalive_interval_ms * 2);
    const auto stall_us = static_cast<std::uint64_t>(stall_ms) * 1000ull;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(stall_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        if (h.liveness && h.liveness->body_done.load(std::memory_order_acquire)) {
            out.status = KeepaliveWatchStatus::Done;
            out.last_keepalive_us = h.liveness->last_keepalive_us.load(std::memory_order_relaxed);
            return out;
        }

        // Non-blocking peek: any message (esp. keepalive) counts as alive.
        auto msg = agent_recv(h, /*wait=*/false, /*timeout_ms=*/0);
        if (msg) {
            out.message = std::move(msg);
            if (h.liveness)
                out.last_keepalive_us =
                    h.liveness->last_keepalive_us.load(std::memory_order_relaxed);
            else if (is_keepalive_message(*out.message))
                out.last_keepalive_us = orch_now_us();
            out.status = KeepaliveWatchStatus::Alive;
            return out;
        }

        // Shared clock from helper emit path (works even if messages already
        // drained by another supervisor).
        if (h.liveness) {
            const auto last = h.liveness->last_keepalive_us.load(std::memory_order_acquire);
            out.last_keepalive_us = last;
            if (last > 0) {
                const auto now = orch_now_us();
                if (now >= last && (now - last) < stall_us) {
                    out.status = KeepaliveWatchStatus::Alive;
                    return out;
                }
            }
        }

        // Brief host sleep; helper continues to emit on its own fiber.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Final age check after window closes.
    if (h.liveness) {
        const auto last = h.liveness->last_keepalive_us.load(std::memory_order_acquire);
        out.last_keepalive_us = last;
        if (last > 0) {
            const auto now = orch_now_us();
            if (now >= last && (now - last) < stall_us) {
                out.status = KeepaliveWatchStatus::Alive;
                return out;
            }
        }
        if (h.liveness->body_done.load(std::memory_order_acquire)) {
            out.status = KeepaliveWatchStatus::Done;
            return out;
        }
    }

    // Stall: no fresh keepalive within the window.
    out.status = KeepaliveWatchStatus::Stalled;
    g_orch_module_stats.stalled_agents_total.fetch_add(1, std::memory_order_relaxed);
    if (cancel_on_stall) {
        if (h.fiber)
            h.fiber->request_cancel();
        stop_keepalive_helper(h);
        out.cancelled = true;
        g_orch_module_stats.keepalive_cancels_total.fetch_add(1, std::memory_order_relaxed);
    }
    return out;
}

// Note (Issue #1966): no multi-agent public API here.
//   - Batch parallel work: serve::parallel_orch::parallel_intend (optionally
//     bump g_orch_module_stats.parallel_batches at the call site).
//   - Name→handle bookkeeping for Aura orch:spawn-agent / orch:agent-join
//     lives in evaluator_primitives_agent.cpp (file-local table).

} // namespace aura::orch

#endif // AURA_ORCH_AGENT_SPAWN_H
