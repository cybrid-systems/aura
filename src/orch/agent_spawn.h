// agent_spawn.h — Issue #1588: unified agent spawn (fiber + mailbox + join).
// Header API under aura::orch; pairs with serve/parallel_orch and multi_fiber_mailbox.

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

namespace aura::orch {

inline constexpr int kOrchModulePhase = 1; // #1588 foundation
inline constexpr int kOrchModuleIssue = 1588;

// ── Process-wide orch module stats ─────────────────────
struct OrchModuleStats {
    std::atomic<std::uint64_t> agents_spawned{0};
    std::atomic<std::uint64_t> agents_joined{0};
    std::atomic<std::uint64_t> agents_send{0};
    std::atomic<std::uint64_t> agents_recv{0};
    std::atomic<std::uint64_t> spawn_failures{0};
    std::atomic<std::uint64_t> parallel_batches{0};
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

// ── Agent handle ───────────────────────────────────────
struct AgentHandle {
    std::uint64_t id = 0; // Fiber::id()
    std::string name;
    serve::Fiber* fiber = nullptr;
    std::shared_ptr<serve::mf_mailbox::MultiFiberMailbox> mailbox;
    bool ok = false;
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
        if (attach && mb && serve::g_current_fiber)
            mb->detach(serve::g_current_fiber);
    });

    if (!f) {
        g_orch_module_stats.spawn_failures.fetch_add(1, std::memory_order_relaxed);
        return h;
    }

    h.fiber = f;
    h.id = f->id();
    h.mailbox = std::move(mb);
    h.ok = true;
    g_orch_module_stats.agents_spawned.fetch_add(1, std::memory_order_relaxed);
    return h;
}

// Join a single agent (Fiber::join).
[[nodiscard]] inline serve::JoinResult join_agent(AgentHandle& h,
                                                  std::optional<std::uint64_t> timeout_ms = {}) {
    if (!h.ok || !h.fiber) {
        serve::JoinResult r;
        r.status = serve::JoinStatus::Invalid;
        return r;
    }
    auto jr = serve::Fiber::join(h.fiber, timeout_ms);
    g_orch_module_stats.agents_joined.fetch_add(1, std::memory_order_relaxed);
    return jr;
}

// Join many agents.
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
