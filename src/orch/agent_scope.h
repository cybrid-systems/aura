// agent_scope.h — Issue #2083 / #2161: scoped multi-agent coordination.
// Issue #2226: promoted from opt-in feature flag to default multi-agent
// supervision root. AgentScope is now always available under aura::orch
// (the class body no longer lives inside `#ifdef AURA_ENABLE_AGENT_SCOPE`).
//
// STATUS: Default / Documented multi-agent supervision surface.
// MVP linter (scripts/check_orch_mvp_scope.py --strict) still forbids
// the process-global registry identifiers removed in #1966
// (AgentRegistry / global_agent_registry / conduct_parallel), so the
// "no global registry" contract from #2083 is preserved by the linter,
// not by a build-time gate.
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
// Rules (per Issue #2083 AC4 / #2226):
//   - No process-global registry (the orch MVP scope linter still forbids
//     the multi-agent process-static identifiers removed in #1966).
//   - Scope destructor is the supervision root (cancel + best-effort drain
//     + reservation release, mirroring join_agents #2082 contract).
//   - Default-on (no #define required). Documented in src/orch/README.md
//     as the supported multi-agent supervision root.

#ifndef AURA_ORCH_AGENT_SCOPE_H
#define AURA_ORCH_AGENT_SCOPE_H

#include "orch/agent_spawn.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

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
    //
    // Issue #2229: also stores a copy of the spec in specs_ so
    // watch_all(stall_timeout_ms, AgentFailurePolicy) with
    // on_stall == RestartN can re-spawn the body under the same
    // AgentSpec / name rules. restart_counts_ / consecutive_stall_counts_
    // are parallel vectors for per-handle supervision state.
    AgentHandle& spawn(AgentSpec spec) {
        handles_.emplace_back(spawn_agent_with_mailbox(sched_, spec));
        // Copy the spec for re-spawn. AgentSpec's body is a
        // std::function (cheap to copy; the body closure is shared
        // by handle, so the new spawn reuses the same closure
        // instance). specs_ / restart_counts_ / consecutive_stall_counts_
        // stay aligned with handles_ via push_back in the same order.
        specs_.push_back(spec);
        restart_counts_.push_back(0);
        consecutive_stall_counts_.push_back(0);
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

    // Issue #2229: full supervision policy surface. Replaces the
    // binary StallPolicy with AgentFailureAction (ReportOnly /
    // Cancel / RestartN) and adds a circuit-like consecutive-stall
    // limit + optional restart backoff. RestartN path: stop helper
    // → cancel body → join drain (via #2227 hard-reclaim) →
    // optional backoff → spawn replacement under the same AgentSpec
    // → replace handle in the scope vector → bump restart count.
    // Once restart_counts_[i] >= max_restarts OR
    // consecutive_stall_counts_[i] >= consecutive_stall_limit, the
    // scope force-downgrades to Cancel (request_cancel already
    // invoked by watch_agent_liveness with cancel_on_stall=true)
    // and bumps agent_restart_exhausted_total. Default policy
    // (Cancel / max_restarts=0 / consecutive_stall_limit=3) matches
    // the pre-#2229 StallPolicy::Cancel behaviour for callers
    // adopting AgentFailurePolicy incrementally.
    [[nodiscard]] ScopeWatchResult watch_all(std::uint32_t stall_timeout_ms,
                                             const AgentFailurePolicy& policy) {
        ScopeWatchResult r;
        // Map AgentFailureAction::ReportOnly / Cancel to the binary
        // watch_agent_liveness cancel_on_stall flag. RestartN uses
        // Cancel internally too (the re-spawn path runs after the
        // existing cancel completes).
        const bool cancel_on_stall = (policy.on_stall != AgentFailureAction::ReportOnly);
        for (std::size_t i = 0; i < handles_.size(); ++i) {
            auto& h = handles_[i];
            auto wr = watch_agent_liveness(h, stall_timeout_ms, cancel_on_stall);
            switch (wr.status) {
                case KeepaliveWatchStatus::Alive:
                    ++r.alive;
                    // Reset consecutive_stall on healthy (fresh heartbeat).
                    consecutive_stall_counts_[i] = 0;
                    break;
                case KeepaliveWatchStatus::Stalled:
                    ++r.stalled;
                    if (wr.cancelled)
                        ++r.cancelled;
                    ++consecutive_stall_counts_[i];
                    g_orch_module_stats.agent_consecutive_stall_total.fetch_add(
                        1, std::memory_order_relaxed);
                    // Decide between RestartN and the fallback Cancel /
                    // ReportOnly per the policy + circuit-like cap.
                    const bool circuit_open =
                        consecutive_stall_counts_[i] >= policy.consecutive_stall_limit;
                    const bool within_max_restarts = restart_counts_[i] < policy.max_restarts;
                    const bool should_restart = (policy.on_stall == AgentFailureAction::RestartN) &&
                                                !circuit_open && within_max_restarts;
                    if (should_restart) {
                        // Re-spawn path: stop helper, cancel body,
                        // join drain (via #2227 hard-reclaim), optional
                        // backoff, then spawn replacement under the
                        // same AgentSpec and replace the handle.
                        stop_keepalive_helper(h);
                        if (h.fiber && !h.fiber->is_done()) {
                            h.fiber->request_cancel();
                            if (auto* sched = h.fiber->owner_sched()) {
                                sched->note_orphan_fiber(h.fiber, /*hard_deadline_ms=*/50);
                            }
                        }
                        if (policy.restart_backoff_ms > 0)
                            fiber_sleep_ms(policy.restart_backoff_ms);
                        // Spawn replacement under the same spec.
                        handles_[i] = spawn_agent_with_mailbox(sched_, specs_[i]);
                        // Reset per-handle supervision state.
                        consecutive_stall_counts_[i] = 0;
                        ++restart_counts_[i];
                        g_orch_module_stats.agent_restart_total.fetch_add(
                            1, std::memory_order_relaxed);
                    } else if ((policy.on_stall == AgentFailureAction::RestartN) &&
                               (!within_max_restarts || circuit_open)) {
                        // Cancel path: request_cancel already invoked
                        // by watch_agent_liveness (cancel_on_stall=true).
                        // Bump exhausted on the max-restarts OR
                        // circuit-open signal so Agent frameworks can
                        // branch on the supervision outcome.
                        g_orch_module_stats.agent_restart_exhausted_total.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    // ReportOnly path: nothing extra — aggregate count
                    // already bumped via r.stalled.
                    break;
                case KeepaliveWatchStatus::Done:
                    ++r.done;
                    consecutive_stall_counts_[i] = 0;
                    break;
                case KeepaliveWatchStatus::Closed:
                    ++r.closed;
                    break;
            }
        }
        return r;
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
    // Issue #2229: parallel vectors for the RestartN supervision
    // policy. All three stay aligned with handles_ via push_back in
    // spawn (same order). specs_ preserves the original AgentSpec
    // for re-spawn under the same name / body / mailbox config;
    // restart_counts_ / consecutive_stall_counts_ track per-handle
    // supervision state for the circuit-like consecutive_stall_limit
    // + max_restarts cap.
    std::vector<AgentSpec> specs_;
    std::vector<std::uint32_t> restart_counts_;
    std::vector<std::uint32_t> consecutive_stall_counts_;
};

} // namespace aura::orch

#endif // AURA_ORCH_AGENT_SCOPE_H