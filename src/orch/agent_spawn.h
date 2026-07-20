// agent_spawn.h — Issue #1588 / #1879 / #1880: unified agent spawn.
//
// STATUS: Advanced / Experimental (Issue #1945, 2026-07 through 2026-10).
// See docs/agent-orchestration-status.md for the full MVP vs deferred
// scope + re-enable path. The single-agent MVP path (spawn +
// join + send/recv + AgentHandle/AgentSpec + OrchModuleStats) is
// production-safe. The multi-agent coordination surface
// (AgentRegistry + conduct_parallel) is // DEFERRED per #1965
// cycle 1 (commit bcb68c7c); tracked by
// scripts/check_orch_mvp_scope.py.
// Header API under aura::orch; pairs with serve/parallel_orch and multi_fiber_mailbox.
// Issue #1879: spawn body exit + join force StableNodeRef provenance refresh.
// Issue #1880: ResourceQuota preflight (arena/mailbox/fibers) + try_acquire
// body wrapper (typed ResourceQuotaExceeded, no panic).

#ifndef AURA_ORCH_AGENT_SPAWN_H
#define AURA_ORCH_AGENT_SPAWN_H

#include "core/resource_quota.hh"
#include "serve/fiber.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/parallel_orch.h"
#include "serve/scheduler.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
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
};

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
};

struct AgentSpec {
    std::string name;
    std::function<void()> body; // required for spawn
    bool attach_mailbox = true;
    std::size_t mailbox_high_water = 256;
};

// Spawn a fiber agent on `sched`, optionally with a private MultiFiberMailbox
// attached to the running fiber (attach happens inside the fiber so g_current_fiber
// is valid).
// Issue #1880: preflight ResourceQuota (fibers + estimated arena/mailbox memory)
// with typed ResourceQuotaExceeded (never panic). Agent body uses
// MutationBoundaryGuard::try_acquire when an Evaluator is wired.
[[nodiscard]] inline AgentHandle spawn_agent_with_mailbox(serve::Scheduler& sched, AgentSpec spec) {
    AgentHandle h;
    h.name = std::move(spec.name);
    if (!spec.body) {
        g_orch_module_stats.spawn_failures.fetch_add(1, std::memory_order_relaxed);
        return h;
    }

    auto& pq = aura::core::resource_quota::process_resource_quota();

    // Issue #1880: fiber capacity preflight (check only; Scheduler::spawn also consumes).
    if (auto ferr = pq.check_orchestration_fibers(/*amount=*/1)) {
        g_orch_module_stats.spawn_failures.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.spawn_quota_rejects.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.resource_quota_rejects_total.fetch_add(1, std::memory_order_relaxed);
        pq.orch_resource_quota_rejects_total.fetch_add(1, std::memory_order_relaxed);
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

    serve::Fiber* f = sched.spawn([body = std::move(body), mb, attach]() mutable {
        if (attach && mb && serve::g_current_fiber)
            mb->attach(serve::g_current_fiber);
        // Issue #1880: try_acquire mutation boundary when Evaluator is bound.
        // On reject: skip body (typed quota path already recorded); no panic.
        const int acq = aura_orch_agent_body_try_acquire();
        if (acq == 0) {
            g_orch_module_stats.agent_body_try_acquire_ok_total.fetch_add(
                1, std::memory_order_relaxed);
            body();
            aura_orch_agent_body_release_guard();
        } else {
            g_orch_module_stats.agent_body_try_acquire_rejects_total.fetch_add(
                1, std::memory_order_relaxed);
            g_orch_module_stats.resource_quota_rejects_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
        }
        // Issue #1879: after agent body, force StableNodeRef provenance
        // validation + auto pin/refresh + linear ownership probe so COW /
        // steal / GC cannot leave dangling refs for the join path.
        orch_agent_body_exit_provenance();
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
    h.mailbox = std::move(mb);
    h.ok = true;
    g_orch_module_stats.agents_spawned.fetch_add(1, std::memory_order_relaxed);
    g_orch_module_stats.agents_active.fetch_add(1, std::memory_order_relaxed);
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

// Join a single agent (Fiber::join) + Issue #1879 post-join provenance.
[[nodiscard]] inline serve::JoinResult join_agent(AgentHandle& h,
                                                  std::optional<std::uint64_t> timeout_ms = {}) {
    if (!h.ok || !h.fiber) {
        serve::JoinResult r;
        r.status = serve::JoinStatus::Invalid;
        return r;
    }
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
[[nodiscard]] inline serve::JoinResult join_agents(std::span<AgentHandle> agents,
                                                   std::optional<std::uint64_t> timeout_ms = {}) {
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
        for (auto* f : fibers)
            orch_post_join_provenance(f);
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

// Thin alias: parallel batch via serve::parallel_orch (counts as orch parallel).
// DEFERRED (Issue #1965 cycle 1 — beyond MVP scope). New callers should use
// serve::parallel_orch::parallel_intend directly. This alias exists only
// for the legacy single-consumer in evaluator_primitives_agent.cpp; see
// the orch-mvp-scope linter for the grandfather list.
[[nodiscard]] inline serve::parallel_orch::BatchResult
conduct_parallel(serve::Scheduler& sched, std::span<const serve::parallel_orch::TaskSpec> tasks,
                 serve::parallel_orch::ParallelPolicy policy = {},
                 serve::mf_mailbox::MultiFiberMailbox* mb = nullptr) {
    g_orch_module_stats.parallel_batches.fetch_add(1, std::memory_order_relaxed);
    return serve::parallel_orch::parallel_intend(sched, tasks, policy, mb);
}

// Named agent registry (process-local, for multi-agent coordination tests).
// DEFERRED (Issue #1965 cycle 1 — beyond MVP scope). Single production
// consumer in src/compiler/evaluator_primitives_agent.cpp (lines 2674,
// 2713) is grandfathered. New callers should use serve::Fiber +
// MultiFiberMailbox directly. See orch-mvp-scope linter for the list.
class AgentRegistry {
public:
    AgentHandle& put(AgentHandle h) {
        std::lock_guard lock(mu_);
        auto name = h.name.empty() ? ("agent-" + std::to_string(h.id)) : h.name;
        h.name = name;
        agents_[name] = std::move(h);
        return agents_[name];
    }

    [[nodiscard]] AgentHandle* find(const std::string& name) {
        std::lock_guard lock(mu_);
        auto it = agents_.find(name);
        return it == agents_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mu_);
        return agents_.size();
    }

    void clear() {
        std::lock_guard lock(mu_);
        agents_.clear();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, AgentHandle> agents_;
};

inline AgentRegistry& global_agent_registry() {
    static AgentRegistry reg;
    return reg;
}

} // namespace aura::orch

#endif // AURA_ORCH_AGENT_SPAWN_H
