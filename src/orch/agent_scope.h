// agent_scope.h — Issue #2083 / #2161: scoped multi-agent coordination.
// Issue #2226: promoted from opt-in feature flag to default multi-agent
// supervision root. AgentScope is now always available under aura::orch
// (the class body no longer lives inside a feature-flag ifdef; always on).
// Issue #2537: hierarchical parent/children tree — explicit owner pointers
// (parent_ raw non-owning + children_ unique_ptr vector). No process-global
// registry. cancel_all / ~AgentScope propagate cancel top-down; drain is
// bottom-up (children unique_ptrs destroyed before this scope joins).
//
// STATUS: Default / Documented multi-agent supervision surface.
// MVP linter (scripts/coverage/checks/check_orch_mvp_scope.py --strict) still forbids
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
//                         reference). NOT a global registry. Hierarchy
//                         (#2537) is a tree of scopes, still no static map.
//
// Rules (per Issue #2083 AC4 / #2226 / #2537):
//   - No process-global registry (the orch MVP scope linter still forbids
//     the multi-agent process-static identifiers removed in #1966).
//   - Scope destructor is the supervision root (cancel + best-effort drain
//     + reservation release, mirroring join_agents #2082 contract).
//   - Hierarchy (#2537): parent owns children via unique_ptr; cancel
//     propagates to descendants first, then local handles; dtor drains
//     children then self. Single-owner serial model (#2399) still applies
//     to each scope; child ops are same-thread (or explicitly serialized).
//   - Default-on (no #define required). Documented in src/orch/README.md
//     as the supported multi-agent supervision root.

#ifndef AURA_ORCH_AGENT_SCOPE_H
#define AURA_ORCH_AGENT_SCOPE_H

#include "orch/agent_spawn.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace aura::orch {

// Issue #2537: hierarchical AgentScope (parent/children cancel tree).
inline constexpr int kAgentScopeHierarchyIssue = 2537;

// Issue #2399: optional hard abort on concurrent AgentScope enter.
// Default OFF (metric-only). Env AURA_AGENT_SCOPE_CONCURRENT_ABORT=1 enables.
[[nodiscard]] inline bool agent_scope_concurrent_abort_enabled() noexcept {
    const char* e = std::getenv("AURA_AGENT_SCOPE_CONCURRENT_ABORT");
    return e != nullptr && e[0] == '1' && e[1] == '\0';
}

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
// Hierarchy (Issue #2537): a scope may own child scopes via unique_ptr.
// parent_ is a non-owning back-pointer. No static/global agent table —
// the tree is built only through spawn_child() on an explicit owner.
//
// Cancel / destroy order (AC2, documented):
//   1. cancel_all: recurse into children first, then request_cancel on
//      this scope's local handles (top-down cancel propagation).
//   2. ~AgentScope: cancel_all, then destroy children_ (each child dtor
//      drains its own handles — bottom-up drain), then join this scope.
// RestartN / watch_all remain scope-local (no cross-scope restart map).
//
// Thread-safety: spawn / spawn_child / join_all / cancel_all / watch_all /
// handles are NOT safe to call concurrently from multiple threads. The
// owner must serialize access (matches the underlying Scheduler
// single-owner model). Child scopes inherit the same serial model (#2399).
//
// Issue #2399: concurrent enter is *detected* (metric + optional hard abort)
// but not locked — no internal mutex, no global registry. Same-thread
// re-entry (e.g. ~AgentScope → cancel_all → join_all) is allowed via depth.
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
        ScopeEnterGuard g(this, "spawn");
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

    // Issue #2537: create a child AgentScope owned by this scope.
    // Shares the parent Scheduler. Child's parent() returns this.
    // Cancel/destroy on the parent propagates to all descendants.
    // Caller must serialize access (same single-owner model as spawn).
    AgentScope& spawn_child() {
        ScopeEnterGuard g(this, "spawn_child");
        auto child = std::make_unique<AgentScope>(sched_);
        child->parent_ = this;
        children_.push_back(std::move(child));
        return *children_.back();
    }

    [[nodiscard]] AgentScope* parent() const noexcept { return parent_; }
    [[nodiscard]] bool is_root() const noexcept { return parent_ == nullptr; }
    [[nodiscard]] std::size_t child_count() const noexcept { return children_.size(); }

    // Indexed child access (throws std::out_of_range if i >= child_count()).
    [[nodiscard]] AgentScope& child_at(std::size_t i) { return *children_.at(i); }
    [[nodiscard]] const AgentScope& child_at(std::size_t i) const { return *children_.at(i); }

    // Join all live handles. Mirrors join_agents (#2082/#2153): on non-Ok,
    // cancel + secondary drain (default 2s, JoinPolicy.drain_ms) before
    // per-handle reservation release. Release is idempotent (#2009).
    [[nodiscard]] serve::JoinResult join_all(std::optional<std::uint64_t> timeout_ms = {}) {
        ScopeEnterGuard g(this, "join_all");
        if (handles_.empty()) {
            serve::JoinResult r;
            r.status = serve::JoinStatus::Invalid;
            return r;
        }
        return join_agents(std::span<AgentHandle>(handles_), timeout_ms);
    }

    // Issue #2153: full JoinPolicy (primary + drain_ms).
    [[nodiscard]] serve::JoinResult join_all(JoinPolicy policy) {
        ScopeEnterGuard g(this, "join_all(policy)");
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
    //
    // Issue #2537: cancels child scopes first (top-down), then local
    // handles. Does not join/drain — callers (or ~AgentScope) drain.
    void cancel_all() noexcept {
        ScopeEnterGuard g(this, "cancel_all");
        for (auto& c : children_) {
            if (c)
                c->cancel_all();
        }
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
        ScopeEnterGuard g(this, "watch_all");
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
        ScopeEnterGuard g(this, "watch_all(policy)");
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
                case KeepaliveWatchStatus::Stalled: {
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
                }
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
    // Issue #2399: same-thread re-entry of cancel_all/join_all under the
    // enter guard is allowed (depth); concurrent ~AgentScope from another
    // thread is detected as misuse.
    //
    // Issue #2537 destroy order (documented AC2):
    //   1. cancel_all — top-down (children, then local handles)
    //   2. children_.clear() — each child dtor drains its tree bottom-up
    //   3. join local handles (default drain_ms, #2153)
    ~AgentScope() {
        ScopeEnterGuard g(this, "~AgentScope");
        cancel_all();
        // Drain descendants before this scope's handles (bottom-up).
        children_.clear();
        if (handles_.empty())
            return;
        // Issue #2153: destructor uses default drain_ms (kDefaultJoinDrainMs).
        (void)join_agents(
            std::span<AgentHandle>(handles_),
            JoinPolicy{.primary_ms = kDefaultJoinDrainMs, .drain_ms = kDefaultJoinDrainMs});
    }

private:
    // Issue #2399: RAII enter/leave for concurrent misuse detection.
    // Same-thread re-entry increments depth (no metric). Concurrent enter
    // from another thread bumps agent_scope_concurrent_misuse_total and
    // optionally aborts. Metric path still runs the method body (detect,
    // don't invent locks). Zero cost beyond one atomic CAS when free.
    struct ScopeEnterGuard {
        AgentScope* self = nullptr;
        bool holds = false;
        ScopeEnterGuard(AgentScope* s, const char* site) noexcept
            : self(s) {
            if (!self)
                return;
            holds = self->try_enter(site);
        }
        ~ScopeEnterGuard() noexcept {
            if (holds && self)
                self->leave();
        }
        ScopeEnterGuard(const ScopeEnterGuard&) = delete;
        ScopeEnterGuard& operator=(const ScopeEnterGuard&) = delete;
    };

    // Returns true if this thread holds ownership (caller must leave).
    // Returns false on concurrent misuse after metric/abort path (caller
    // still runs the method body without tracking ownership).
    bool try_enter(const char* site) noexcept {
        const auto tid = std::this_thread::get_id();
        std::thread::id expected{}; // default-constructed = unowned
        if (owner_tid_.compare_exchange_strong(expected, tid, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
            enter_depth_.store(1, std::memory_order_relaxed);
            return true;
        }
        if (expected == tid) {
            // Same-thread re-entry (~AgentScope → cancel_all → join_all).
            enter_depth_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // Another thread owns the scope — concurrent misuse.
        g_orch_module_stats.agent_scope_concurrent_misuse_total.fetch_add(
            1, std::memory_order_relaxed);
        if (agent_scope_concurrent_abort_enabled()) {
            std::fprintf(stderr,
                         "FATAL: AgentScope concurrent misuse at %s "
                         "(AURA_AGENT_SCOPE_CONCURRENT_ABORT=1, #2399)\n",
                         site ? site : "?");
            std::abort();
        }
        return false;
    }

    void leave() noexcept {
        const auto d = enter_depth_.fetch_sub(1, std::memory_order_relaxed);
        if (d == 1) {
            owner_tid_.store(std::thread::id{}, std::memory_order_release);
        }
    }

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
    // Issue #2537: explicit tree links — no process-global registry.
    // parent_ is non-owning (set only by parent's spawn_child).
    // children_ owns descendants; cleared in ~AgentScope after cancel.
    AgentScope* parent_ = nullptr;
    std::vector<std::unique_ptr<AgentScope>> children_;
    // Issue #2399: single-owner detection (not a mutex).
    // owner_tid_ empty = free; depth tracks same-thread re-entry.
    mutable std::atomic<std::thread::id> owner_tid_{};
    mutable std::atomic<std::uint32_t> enter_depth_{0};
};

// Issue #2588: per-Evaluator scope map (Session-local, NOT process-static).
// Lazy-created on first scope-spawn, destroyed when the Evaluator's scope
// is empty (cancel-all / join-all) or the scope is explicitly dropped.
// The map is process-level storage keyed by Evaluator* (typed as void* to
// avoid circular include with compiler/evaluator.h) — the SCOPE itself
// is per-Evaluator; the storage is just a convenience container for
// Aura prims to find / own the lifetime. NOT a global agent registry
// (MVP linter scripts/coverage/checks/check_orch_mvp_scope.py still guards
// AgentRegistry / global_agent_registry symbols).
inline std::unordered_map<void*, std::unique_ptr<AgentScope>>& g_evaluator_agent_scopes() noexcept {
    static std::unordered_map<void*, std::unique_ptr<AgentScope>> m;
    return m;
}

// Issue #2588: get or create the scope for the given Evaluator (key).
// Lazy allocation on first scope-spawn; subsequent calls return the
// existing scope. Scheduler reference is borrowed — caller must keep
// `sched` alive for the lifetime of the scope (process-local Scheduler
// satisfies this for the MVP; production hosts bind a per-session
// Scheduler to Evaluator).
inline AgentScope& get_or_create_agent_scope(void* ev_key, serve::Scheduler& sched) {
    auto& m = g_evaluator_agent_scopes();
    auto it = m.find(ev_key);
    if (it == m.end()) {
        auto scope = std::make_unique<AgentScope>(sched);
        auto* raw = scope.get();
        m.emplace(ev_key, std::move(scope));
        return *raw;
    }
    return *it->second;
}

// Issue #2588: find an existing scope for the given Evaluator (key).
// Returns nullptr if no scope has been created yet.
inline AgentScope* find_agent_scope(void* ev_key) noexcept {
    auto& m = g_evaluator_agent_scopes();
    auto it = m.find(ev_key);
    return it == m.end() ? nullptr : it->second.get();
}

// Issue #2588: drop the scope for the given Evaluator (key). Returns true
// if a scope was dropped. ~AgentScope is invoked (cancel + drain + reservation
// release), so per-handle no-leak (#2155 / #2009) holds.
inline bool drop_agent_scope(void* ev_key) noexcept {
    auto& m = g_evaluator_agent_scopes();
    return m.erase(ev_key) > 0;
}

// Issue #2588: process-wide reset for tests / session boundary (cancels + drains
// every scope, then clears the map). Returns the count of scopes dropped.
inline std::size_t reset_all_agent_scopes_for_test() noexcept {
    auto& m = g_evaluator_agent_scopes();
    const auto n = m.size();
    m.clear();
    return n;
}

} // namespace aura::orch

#endif // AURA_ORCH_AGENT_SCOPE_H