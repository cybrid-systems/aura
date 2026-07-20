// agent_spawn.h — Issue #1588 / #1879: unified agent spawn (fiber + mailbox + join).
// Header API under aura::orch; pairs with serve/parallel_orch and multi_fiber_mailbox.
// Issue #1879: spawn body exit + join force StableNodeRef provenance refresh /
// auto re-pin / linear ownership probe via evaluator C trampolines.

#ifndef AURA_ORCH_AGENT_SPAWN_H
#define AURA_ORCH_AGENT_SPAWN_H

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

namespace aura::orch {

inline constexpr int kOrchModulePhase = 2; // #1879 provenance mandate
inline constexpr int kOrchModuleIssue = 1879;

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
    // Issue #1600: typed quota failure surface for Agent frameworks.
    bool quota_exceeded = false;
    std::string error; // e.g. "ResourceQuotaExceeded: fibers quota exceeded"
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
[[nodiscard]] inline AgentHandle spawn_agent_with_mailbox(serve::Scheduler& sched, AgentSpec spec) {
    AgentHandle h;
    h.name = std::move(spec.name);
    if (!spec.body) {
        g_orch_module_stats.spawn_failures.fetch_add(1, std::memory_order_relaxed);
        return h;
    }

    auto mb = spec.attach_mailbox
                  ? std::make_shared<serve::mf_mailbox::MultiFiberMailbox>(spec.mailbox_high_water)
                  : nullptr;
    auto body = std::move(spec.body);
    auto attach = spec.attach_mailbox;

    serve::Fiber* f = sched.spawn([body = std::move(body), mb, attach]() mutable {
        if (attach && mb && serve::g_current_fiber)
            mb->attach(serve::g_current_fiber);
        body();
        // Issue #1879: after agent body, force StableNodeRef provenance
        // validation + auto pin/refresh + linear ownership probe so COW /
        // steal / GC cannot leave dangling refs for the join path.
        orch_agent_body_exit_provenance();
        if (attach && mb && serve::g_current_fiber)
            mb->detach(serve::g_current_fiber);
    });

    if (!f) {
        // Issue #1600: Scheduler::spawn returns nullptr on fiber ResourceQuota.
        g_orch_module_stats.spawn_failures.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.spawn_quota_rejects.fetch_add(1, std::memory_order_relaxed);
        h.quota_exceeded = true;
        h.error = "ResourceQuotaExceeded: fibers quota exceeded";
        return h;
    }

    h.fiber = f;
    h.id = f->id();
    h.mailbox = std::move(mb);
    h.ok = true;
    g_orch_module_stats.agents_spawned.fetch_add(1, std::memory_order_relaxed);
    return h;
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
    // Issue #1879: mandate join-path StableNodeRef / linear enforcement
    // even when Fiber::join skipped host refresh (nested fiber join).
    if (jr.status == serve::JoinStatus::Ok)
        orch_post_join_provenance(h.fiber);
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
    if (jr.status == serve::JoinStatus::Ok) {
        for (auto* f : fibers)
            orch_post_join_provenance(f);
    }
    return jr;
}

// Send a message to an agent's mailbox (if any).
[[nodiscard]] inline serve::mf_mailbox::PushStatus agent_send(AgentHandle& h,
                                                              serve::mf_mailbox::MailMessage msg) {
    if (!h.ok || !h.mailbox)
        return serve::mf_mailbox::PushStatus::Closed;
    msg.to_fiber = h.id;
    auto st = h.mailbox->push(std::move(msg));
    if (st == serve::mf_mailbox::PushStatus::Ok)
        g_orch_module_stats.agents_send.fetch_add(1, std::memory_order_relaxed);
    return st;
}

// Blocking/non-blocking recv on agent mailbox.
[[nodiscard]] inline std::optional<serve::mf_mailbox::MailMessage>
agent_recv(AgentHandle& h, bool wait = true, int timeout_ms = -1) {
    if (!h.ok || !h.mailbox)
        return std::nullopt;
    auto m = h.mailbox->recv(wait, timeout_ms, h.id);
    if (m)
        g_orch_module_stats.agents_recv.fetch_add(1, std::memory_order_relaxed);
    return m;
}

// Thin alias: parallel batch via serve::parallel_orch (counts as orch parallel).
[[nodiscard]] inline serve::parallel_orch::BatchResult
conduct_parallel(serve::Scheduler& sched, std::span<const serve::parallel_orch::TaskSpec> tasks,
                 serve::parallel_orch::ParallelPolicy policy = {},
                 serve::mf_mailbox::MultiFiberMailbox* mb = nullptr) {
    g_orch_module_stats.parallel_batches.fetch_add(1, std::memory_order_relaxed);
    return serve::parallel_orch::parallel_intend(sched, tasks, policy, mb);
}

// Named agent registry (process-local, for multi-agent coordination tests).
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
