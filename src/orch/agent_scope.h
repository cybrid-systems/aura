// agent_scope.h — Issue #2083 / #2161: opt-in scoped multi-agent coordination.
//
// STATUS: Advanced / Experimental (Issue #2083, feature-flagged).
// Lives behind AURA_ENABLE_AGENT_SCOPE so the MVP linter
// (scripts/check_orch_mvp_scope.py --strict) stays green by default.
// Commercial multi-agent builds define this flag to opt in.
// Issue #2161: scope-level watch_all (batch liveness + optional stall cancel).
//
// Distinct from evaluator-local OrchAgentNameTable (#2078) and
// serve::parallel_orch::parallel_intend (#1587):
//   - OrchAgentNameTable: per-Evaluator name bookkeeping for Aura primitives
//     (orch:spawn-agent / orch:agent-join).
//   - parallel_intend:    short-lived batch thunks (no long-lived names).
//   - AgentScope:         long-lived named agents, parent-cancel + join_all
//                         semantics, bound to an explicit owner (Scheduler
//                         reference). NOT a global registry.
//
// Rules (per Issue #2083 AC4):
//   - No process-global registry (the orch MVP scope linter still forbids
//     the multi-agent process-static identifiers removed in #1966).
//   - Scope destructor is the supervision root (cancel + best-effort drain
//     + reservation release, mirroring join_agents #2082 contract).
//   - Default builds keep the MVP linter green; the class body lives inside
//     #ifdef AURA_ENABLE_AGENT_SCOPE so opt-in is explicit per TU.

#ifndef AURA_ORCH_AGENT_SCOPE_H
#define AURA_ORCH_AGENT_SCOPE_H

#include "orch/agent_spawn.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#ifdef AURA_ENABLE_AGENT_SCOPE

namespace aura::orch {

// Issue #2161: stall response for scope-level watch_all.
// Restart is out of scope (needs re-spawn + name-table rules).
enum class StallPolicy : std::uint8_t {
    ReportOnly = 0, // aggregate counts only; no cancel
    Cancel = 1,     // cancel_on_stall for Stalled agents only
};

// Issue #2161: aggregated liveness snapshot for one watch_all pass.
// Counts map 1:1 to KeepaliveWatchStatus (+ cancelled when stall cancel fired).
struct ScopeWatchResult {
    std::size_t alive = 0;
    std::size_t stalled = 0;
    std::size_t done = 0;
    std::size_t closed = 0;
    std::size_t cancelled = 0; // subset of stalled that got request_cancel
};

// Scoped multi-agent supervision root. Owns its handles via std::vector
// (no global registry). Destructor cancels + best-effort drains + releases
// reservations (#2082 cancel-before-release contract).
//
// Thread-safety: spawn / join_all / cancel_all / watch_all / handles are NOT
// safe to call concurrently from multiple threads. The owner must serialize
// access (matches the underlying Scheduler single-owner model).
class AgentScope {
public:
    explicit AgentScope(serve::Scheduler& sched) noexcept
        : sched_(sched) {}

    AgentScope(const AgentScope&) = delete;
    AgentScope& operator=(const AgentScope&) = delete;
    AgentScope(AgentScope&&) = delete;
    AgentScope& operator=(AgentScope&&) = delete;

    // Spawn a new agent under this scope. Pushes the handle to the back;
    // reference remains valid until the scope is destroyed.
    AgentHandle& spawn(AgentSpec spec) {
        handles_.emplace_back(spawn_agent_with_mailbox(sched_, std::move(spec)));
        return handles_.back();
    }

    // Join all live handles. Mirrors join_agents (#2082/#2153): on non-Ok,
    // cancel + secondary drain (default 2s, JoinPolicy.drain_ms) before
    // per-handle reservation release. Release is idempotent (#2009).
    [[nodiscard]] serve::JoinResult join_all(std::optional<std::uint64_t> timeout_ms = {}) {
        if (handles_.empty()) {
            serve::JoinResult r;
            r.status = serve::JoinStatus::Invalid;
            return r;
        }
        return join_agents(std::span<AgentHandle>(handles_), timeout_ms);
    }

    // Issue #2153: full JoinPolicy (primary + drain_ms).
    [[nodiscard]] serve::JoinResult join_all(JoinPolicy policy) {
        if (handles_.empty()) {
            serve::JoinResult r;
            r.status = serve::JoinStatus::Invalid;
            return r;
        }
        return join_agents(std::span<AgentHandle>(handles_), policy);
    }

    // Best-effort cancel request on all live fibers. Bounded cost; does
    // NOT wait. Use join_all afterwards to drain. Safe to call multiple
    // times (request_cancel is idempotent).
    void cancel_all() noexcept {
        for (auto& h : handles_) {
            if (h.fiber && !h.fiber->is_done())
                h.fiber->request_cancel();
        }
    }

    // Issue #2161: batch liveness watch over scope handles.
    // For each handle: watch_agent_liveness (same Closed rules when
    // keepalive_interval_ms==0). No process-global registry.
    // StallPolicy::Cancel cancels only Stalled fibers (Done/Alive untouched).
    [[nodiscard]] ScopeWatchResult watch_all(std::uint32_t stall_timeout_ms = 0,
                                             StallPolicy policy = StallPolicy::Cancel) {
        ScopeWatchResult r;
        const bool cancel_on_stall = (policy == StallPolicy::Cancel);
        for (auto& h : handles_) {
            auto wr = watch_agent_liveness(h, stall_timeout_ms, cancel_on_stall);
            switch (wr.status) {
                case KeepaliveWatchStatus::Alive:
                    ++r.alive;
                    break;
                case KeepaliveWatchStatus::Stalled:
                    ++r.stalled;
                    if (wr.cancelled)
                        ++r.cancelled;
                    break;
                case KeepaliveWatchStatus::Done:
                    ++r.done;
                    break;
                case KeepaliveWatchStatus::Closed:
                    ++r.closed;
                    break;
            }
        }
        return r;
    }

    // Convenience overload: bool cancel_on_stall maps to StallPolicy.
    [[nodiscard]] ScopeWatchResult watch_all(std::uint32_t stall_timeout_ms, bool cancel_on_stall) {
        return watch_all(stall_timeout_ms,
                         cancel_on_stall ? StallPolicy::Cancel : StallPolicy::ReportOnly);
    }

    [[nodiscard]] std::size_t size() const noexcept { return handles_.size(); }
    [[nodiscard]] bool empty() const noexcept { return handles_.empty(); }

    // Read-only access (for advanced supervisor logic + tests).
    [[nodiscard]] std::span<const AgentHandle> handles() const noexcept {
        return std::span<const AgentHandle>(handles_);
    }

    // Mutable access for tests / advanced supervisors (watch_agent_liveness).
    [[nodiscard]] std::span<AgentHandle> handles_mut() noexcept {
        return std::span<AgentHandle>(handles_);
    }

    // Supervision root: cancel + best-effort drain + release before
    // destruction. join_agents handles cancel+drain on non-Ok internally
    // (#2082), and per-handle release_agent_memory_reservation is
    // idempotent so concurrent ~AgentHandle + scope destruction is safe.
    ~AgentScope() {
        if (handles_.empty())
            return;
        cancel_all();
        // Issue #2153: destructor uses default drain_ms (kDefaultJoinDrainMs).
        (void)join_agents(
            std::span<AgentHandle>(handles_),
            JoinPolicy{.primary_ms = kDefaultJoinDrainMs, .drain_ms = kDefaultJoinDrainMs});
    }

private:
    serve::Scheduler& sched_;
    std::vector<AgentHandle> handles_;
};

} // namespace aura::orch

#endif // AURA_ENABLE_AGENT_SCOPE

#endif // AURA_ORCH_AGENT_SCOPE_H