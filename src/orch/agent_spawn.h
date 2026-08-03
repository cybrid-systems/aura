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
// Issue #2009: AgentHandle is move-only RAII for arena reservations
// (destructor + explicit release_agent_memory_reservation are idempotent).

#ifndef AURA_ORCH_AGENT_SPAWN_H
#define AURA_ORCH_AGENT_SPAWN_H

#include "core/resource_quota.hh"
#include "serve/fiber.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/parallel_orch.h"
#include "serve/scheduler.h"

#include <atomic>
#include <optional>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Evaluator hooks (strong defs in evaluator_fiber_mutation.cpp; weak no-ops
// in fiber_bridge.cpp for serve-only link units).
extern "C" void aura_evaluator_post_resume_refresh();
extern "C" void aura_evaluator_on_fiber_join(void* joined_fiber);
// Issue #1880 / #2118: MutationBoundary around agent body (0=ok, 1=reject).
// register_soft_boundary: when 1 (default), fiber path soft-registers
// per-fiber mutation depth for steal/GC visibility without full Guard.
extern "C" int aura_orch_agent_body_try_acquire();
extern "C" int aura_orch_agent_body_try_acquire_ex(int register_soft_boundary);
extern "C" void aura_orch_agent_body_release_guard();

namespace aura::orch {

inline constexpr int kOrchModulePhase = 4; // #1881 orch health observability
inline constexpr int kOrchModuleIssue = 1881;
// Issue #2153: configurable secondary drain after non-Ok join cancel.
inline constexpr int kJoinDrainTimeoutIssue = 2153;
// Issue #2397: reclaimed vs body-still-running after residual hard-reclaim.
inline constexpr int kJoinDrainReclaimStillRunningIssue = 2397;
// Issue #2158: per-Evaluator agent apply mutex (replace process-static orch_eval_mu).
inline constexpr int kAgentApplyPerEvalMutexIssue = 2158;
// Issue #2155: quota-reject spawn path — no name-table put, no arena leak.
inline constexpr int kSpawnQuotaNoLeakIssue = 2155;
// Issue #2159: fiber-native keepalive helper (replace detached std::thread).
inline constexpr int kFiberNativeKeepaliveIssue = 2159;
// Default secondary drain window after request_cancel (#2082 preserved).
inline constexpr std::uint64_t kDefaultJoinDrainMs = 2000;
// Issue #2227: hard-reclaim orphan deadline ceiling. The
// orch join path computes the per-call hard_ms as
// min(drain_ms * 8, kJoinDrainResidualHardMsDefault) so the
// cooperative cancel has 8x the drain window before the
// Scheduler reaps the fiber. 30s is the upper bound — long
// enough to absorb a GC pause or short I/O stall without
// reaping prematurely, short enough that production cancel
// storms converge within a minute.
inline constexpr std::uint64_t kJoinDrainResidualHardMsDefault = 30000;
// Issue #2228 / #2535: mailbox-backpressure admit threshold default.
// spawn_agent_with_mailbox soft-rejects new agents (with attach_mailbox)
// when the process-wide mailbox_bp_recent_total is >= this threshold.
// Issue #2535: production default is a mild gate (32) so producer BP storms
// cannot unbounded-spawn consumers without env opt-in. Opt-out:
//   AURA_ORCH_BP_ADMIT_THRESHOLD=0  — disable gate (legacy / diagnostic,
//   zero cost: no decay work, no BP reject). N>0 enables/overrides.
// Invalid env falls back to kMailboxBpAdmitThresholdDefault.
inline constexpr std::uint64_t kMailboxBpAdmitThresholdDefault = 32;
// Issue #2398 / #2228 lineage sentinel for query:orch-module-stats.
inline constexpr int kMailboxBpAdmitIssue = 2228;
inline constexpr int kMailboxBpRecentWindowIssue = 2398;
// Issue #2535: production-default mild BP admit (threshold default non-zero).
inline constexpr int kMailboxBpAdmitDefaultOnIssue = 2535;
// Issue #2399: AgentScope concurrent access detection (metric + optional abort).
inline constexpr int kAgentScopeConcurrentMisuseIssue = 2399;

// Issue #2228 / #2535: env resolution for the BP admit threshold.
// Returns the configured threshold (0 = admit control off). Parses
// AURA_ORCH_BP_ADMIT_THRESHOLD as a uint64; missing/empty/invalid
// falls back to kMailboxBpAdmitThresholdDefault (32, #2535).
// Explicit env=0 remains the production opt-out.
inline std::uint64_t resolve_mailbox_bp_admit_threshold() noexcept {
    const char* env = std::getenv("AURA_ORCH_BP_ADMIT_THRESHOLD");
    if (!env || !*env)
        return kMailboxBpAdmitThresholdDefault;
    try {
        const auto v = static_cast<std::uint64_t>(std::stoull(env));
        return v;
    } catch (...) {
        return kMailboxBpAdmitThresholdDefault;
    }
}

// Issue #2465 / #2398: quiet-period window for mailbox_bp_recent_total.
// Without decay, the "recent" counter is process-wide cumulative forever —
// once AURA_ORCH_BP_ADMIT_THRESHOLD > 0, a past storm permanently soft-rejects
// attach_mailbox spawns until process restart. #2398 makes it a real recent
// signal: if no new BP events for window_ms, the next admit preflight zeros
// the gauge (quiet-period reset). send_backpressure_total stays cumulative.
//
// Default = 30s (matches kJoinDrainResidualHardMsDefault / #2465).
// Env (either name; WINDOW preferred per #2398):
//   AURA_ORCH_BP_WINDOW_MS=N  — sliding quiet-period window
//   AURA_ORCH_BP_DECAY_MS=N   — alias kept for #2465 deployments
// N=0 disables decay (diagnostic-only; counter truly monotonic).
inline constexpr std::uint64_t kMailboxBpDecayMsDefault = 30000;
inline constexpr std::uint64_t kMailboxBpWindowMsDefault = kMailboxBpDecayMsDefault;

inline std::uint64_t resolve_mailbox_bp_decay_ms() noexcept {
    // Prefer #2398 WINDOW_MS; fall back to #2465 DECAY_MS; else default.
    const char* window = std::getenv("AURA_ORCH_BP_WINDOW_MS");
    if (window && *window) {
        try {
            return static_cast<std::uint64_t>(std::stoull(window));
        } catch (...) {
            // fall through to DECAY_MS / default
        }
    }
    const char* env = std::getenv("AURA_ORCH_BP_DECAY_MS");
    if (!env || !*env)
        return kMailboxBpWindowMsDefault;
    try {
        return static_cast<std::uint64_t>(std::stoull(env));
    } catch (...) {
        return kMailboxBpWindowMsDefault;
    }
}
// Issue #2398: alias — window_ms is the public name for the quiet period.
inline std::uint64_t resolve_mailbox_bp_window_ms() noexcept {
    return resolve_mailbox_bp_decay_ms();
}

// Last time mailbox_bp_recent_total was quiet-period reset (orch_now_us).
// compare_exchange_strong ensures only one thread zeros per window.
// Helpers note_mailbox_bp_recent_event / maybe_decay_mailbox_bp_recent
// live after orch_now_us + g_orch_module_stats (below).
inline std::atomic<std::uint64_t> g_mailbox_bp_last_decay_us{0};
// Issue #2398: last BP event timestamp (orch_now_us). Quiet-period
// decay triggers when now - last_event > window_ms (no new BP).
inline std::atomic<std::uint64_t> g_mailbox_bp_last_event_us{0};

// Short drain for keepalive helper fiber after body join (#2159).
inline constexpr std::uint64_t kDefaultKeepaliveHelperDrainMs = 500;

// Issue #2153: primary join timeout + secondary cancel-drain policy.
// primary_ms nullopt = wait forever; drain_ms=0 = cancel only (no wait).
struct JoinPolicy {
    std::optional<std::uint64_t> primary_ms{};
    std::uint64_t drain_ms = kDefaultJoinDrainMs;
};

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
    // Issue #2155: quota-reject accounting invariants (no leaked arena / put).
    // no_leak_ok: reject path left reserved_memory_bytes==0 (or defensive release).
    // leak_detect: should stay 0 in production (residual reserved on reject).
    // no_leak: last reject path verified clean (Agent gauge 0|1).
    std::atomic<std::uint64_t> spawn_quota_reject_no_leak_ok_total{0};
    std::atomic<std::uint64_t> spawn_quota_reject_leak_detect_total{0};
    std::atomic<std::uint32_t> spawn_quota_reject_no_leak{0};
    // Issue #1879: StableNodeRef + linear ownership on orch spawn/join/steal.
    std::atomic<std::uint64_t> stable_ref_auto_refresh_total{0};
    std::atomic<std::uint64_t> fiber_steal_provenance_enforced_total{0};
    std::atomic<std::uint64_t> linear_violation_prevented_total{0};
    // Issue #1880: ResourceQuota rejects + try_acquire body rejects.
    std::atomic<std::uint64_t> resource_quota_rejects_total{0};
    std::atomic<std::uint64_t> agent_body_try_acquire_rejects_total{0};
    std::atomic<std::uint64_t> agent_body_try_acquire_ok_total{0};
    // Issue #2118: soft mutation-boundary registration for agent fiber body
    // (steal/GC visibility without full Guard on small fiber stacks).
    std::atomic<std::uint64_t> orch_agent_boundary_entered_total{0};
    std::atomic<std::uint64_t> orch_agent_steal_skipped_boundary_total{0};
    std::atomic<std::uint64_t> orch_agent_boundary_skip_pure_total{0}; // AC2 pure path
    // Issue #1881: observability — hot-path counters (no dead bumps).
    std::atomic<std::uint64_t> agents_active{0}; // spawn - joined (approx live)
    std::atomic<std::uint64_t> send_backpressure_total{0};
    // Issue #2228: process-wide mailbox backpressure event counter
    // (mirrors send_backpressure_total but separated so the spawn
    // admission preflight can read it without conflating with the
    // cumulative send BP count). Bumped in the 2 strong-def push /
    // broadcast_fanout BP sites (line 449 / 966). Reset-for-test
    // helper (reset_orch_module_stats_for_test) clears both
    // mailbox_bp_recent_total and spawn_bp_admit_reject_total together.
    std::atomic<std::uint64_t> mailbox_bp_recent_total{0};
    // Issue #2228: spawn admission-reject counter — bumped every
    // time spawn_agent_with_mailbox soft-rejects a new agent
    // because the process-wide BP condition is at/above the
    // configured admit threshold. Reserved memory stays 0 (no
    // leak per #2155 / #2227 sibling contract).
    std::atomic<std::uint64_t> spawn_bp_admit_reject_total{0};
    // Issue #2591: per-spec override deny counter (separate from the
    // process-default spawn_bp_admit_reject_total above). Multi-tenant
    // hosts can distinguish "process default storm" (gauge too hot
    // against env threshold) from "local override storm" (gauge too
    // hot against per-spec threshold — usually a single noisy scope
    // or producer).
    std::atomic<std::uint64_t> spawn_bp_admit_reject_override_total{0};
    std::atomic<std::uint64_t> send_closed_total{0};
    std::atomic<std::uint64_t> recv_empty_total{0};
    std::atomic<std::uint64_t> join_wait_us_total{0};
    std::atomic<std::uint64_t> join_ok_total{0};
    std::atomic<std::uint64_t> join_fail_total{0};
    // Issue #2153: secondary drain after cancel on non-Ok join.
    // residual = fiber still !is_done() after drain window (cancelled-leaked).
    std::atomic<std::uint64_t> join_drain_residual_total{0};
    // Issue #2227: hard-reclaim counter — bumped every time the
    // orch join path observes a residual fiber and registers it
    // for force-reclaim (Scheduler::note_orphan_fiber). The
    // actual force-reap is driven by Scheduler::reap_orphans_now
    // (driven by tests + scheduler tick). Reset-for-test helper
    // (reset_orch_module_stats_for_test) clears both this and
    // join_drain_residual_total together.
    std::atomic<std::uint64_t> join_drain_residual_reclaim_total{0};
    // Issue #2397: distinguish logical reclaim from body still
    // burning CPU/stack. still_running is a process gauge (+1 on
    // mark_reclaimed while !Done, −1 on body exit or Fiber dtor
    // abandon). body_retired counts reclaimed bodies that finally
    // returned (trampoline after set_state Done). Zero cost on Ok
    // join path (only mark_reclaimed / body-exit / dtor touch these).
    // Mirrored from Fiber statics via aura_orch_note_join_drain_* hooks.
    std::atomic<std::uint64_t> join_drain_residual_still_running{0};
    std::atomic<std::uint64_t> join_drain_residual_body_retired_total{0};
    std::atomic<std::uint64_t> join_drain_us_total{0};
    // Issue #2229: supervision policy metrics (parallel batch
    // FailurePolicy #2007 + RestartN extension). Bumped by
    // AgentScope::watch_all(AgentFailurePolicy) when on_stall ==
    // RestartN, plus the circuit-like consecutive-stall cap and
    // the exhausted-after-max-restarts signal.
    std::atomic<std::uint64_t> agent_restart_total{0};
    std::atomic<std::uint64_t> agent_restart_exhausted_total{0};
    std::atomic<std::uint64_t> agent_consecutive_stall_total{0};
    // Issue #2588: Aura language surface for AgentScope supervision
    // (orch:scope-spawn / orch:scope-watch / orch:scope-join-all /
    // orch:scope-cancel-all). Counters per public prim invocation;
    // scope_active is a process gauge (spawn_count - join_all_ok_count
    // - cancel_all_count, approx live scopes that still own handles).
    // scope_watch_restart_count mirrors the RestartN bump per
    // AgentScope::watch_all(policy) re-spawn path.
    std::atomic<std::uint64_t> scope_spawn_total{0};
    std::atomic<std::uint64_t> scope_watch_total{0};
    std::atomic<std::uint64_t> scope_watch_restart_count{0};
    std::atomic<std::uint64_t> scope_join_all_total{0};
    std::atomic<std::uint64_t> scope_cancel_all_total{0};
    std::atomic<std::uint64_t> scope_dropped_total{0};
    // Issue #2231: agent-ask request/response channel metrics.
    // Bumped by the C++ helper agent_ask(...) and the Aura
    // primitive (orch:agent-ask name payload [:timeout-ms n]).
    // agent_ask_total counts successful Ok returns; agent_ask_timeout_total
    // counts wait-window expirations. Both share the same
    // process-wide atomic surface as the send/recv counters above.
    std::atomic<std::uint64_t> agent_ask_total{0};
    std::atomic<std::uint64_t> agent_ask_timeout_total{0};
    // Issue #2401: agent-reply standard response path metrics.
    // agent_reply_total = successful pushes of reply::<corr>::body;
    // agent_reply_fail_total = unknown-corr / closed / backpressure.
    std::atomic<std::uint64_t> agent_reply_total{0};
    std::atomic<std::uint64_t> agent_reply_fail_total{0};
    // Issue #2538: typed correlation path (MailKind + correlation_id).
    // agent_ask_typed_match_total: Ok match via kind/corr fields (no text parse).
    // agent_reply_typed_total: reply pushes that stamped typed fields.
    std::atomic<std::uint64_t> agent_ask_typed_match_total{0};
    std::atomic<std::uint64_t> agent_reply_typed_total{0};
    // Issue #2008: keepalive / liveness.
    std::atomic<std::uint64_t> keepalive_emitted_total{0};
    std::atomic<std::uint64_t> stalled_agents_total{0};
    std::atomic<std::uint64_t> last_keepalive_us{0}; // process-wide most recent emit
    std::atomic<std::uint64_t> keepalive_cancels_total{0};
    std::atomic<std::uint64_t> keepalive_helpers_spawned{0};
    std::atomic<std::uint64_t> keepalive_helper_spawn_fail{0};
    // Issue #2159: helpers that completed join (Done) after body join/scope.
    std::atomic<std::uint64_t> keepalive_helpers_joined_total{0};
    // Issue #2158: per-Evaluator agent_apply_mu_ acquire accounting.
    // wait_us includes uncontended lock time (usually ~0) + contention wait.
    std::atomic<std::uint64_t> agent_apply_lock_acquisitions_total{0};
    std::atomic<std::uint64_t> agent_apply_lock_wait_us_total{0};
    // Issue #2163: parallel-intend :pure #t path (skip eval_mu for pure thunks).
    std::atomic<std::uint64_t> pure_parallel_batches_total{0};
    std::atomic<std::uint64_t> pure_parallel_tasks_total{0};
    // Task applied under pure but mutation observed → pure-contract-violated.
    std::atomic<std::uint64_t> pure_contract_violated_total{0};
    // pure=#t requested but this task forced the lock (boundary held / unsafe).
    std::atomic<std::uint64_t> pure_fallback_locked_total{0};
    // Issue #2399: AgentScope concurrent misuse detection (metric path).
    // Bumped when a second thread enters spawn/join_all/watch_all/cancel_all
    // while another thread already holds the scope. Default metric-only;
    // AURA_AGENT_SCOPE_CONCURRENT_ABORT=1 hard-aborts. Not a lock —
    // ownership model stays single-owner serialize (no internal mutex).
    std::atomic<std::uint64_t> agent_scope_concurrent_misuse_total{0};
    // Issue #2540: cooperative yield contract (AgentSpec.max_no_yield_ms).
    // Bumped when agent_poll forces Fiber::yield after the no-yield window.
    // Zero cost when max_no_yield_ms==0 (no coop state, poll is no-op).
    std::atomic<std::uint64_t> agent_forced_yield_total{0};
    // Issue #2585: production default coop window applied when
    // AgentSpec.max_no_yield_ms==0 and !dev_off (default 50ms).
    // Opt-out via env AURA_AGENT_MAX_NO_YIELD_MS=0 keeps zero-cost.
    // Bumped once per spawn that injects the default (advisory).
    std::atomic<std::uint64_t> agent_no_yield_default_applied_total{0};
    // Issue #2543: orch self-throttle when aot-hot-update-health_bp < budget.
    // Advisory only (never hard-fails mutate). Bumped on agent body enter /
    // parallel-intend when throttle fires. last_force_reason mirrors
    // force_reason_code (0=ok … 6=deferred-reemit).
    std::atomic<std::uint64_t> orch_hot_update_health_throttle_total{0};
    std::atomic<std::int64_t> orch_hot_update_health_last_force_reason{0};
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

// Issue #2540: per-agent cooperative yield budget (long-running body contract).
// Shared body ↔ poll sites. null / max_no_yield_ms==0 ⇒ zero cost.
struct AgentCoopYield {
    std::uint32_t max_no_yield_ms = 0; // 0 = off
    std::atomic<std::uint64_t> last_coop_us{0};
};

inline constexpr int kAgentMaxNoYieldIssue = 2540;

[[nodiscard]] inline std::uint64_t orch_now_us() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Fiber-id → coop state for agent_poll() without an AgentHandle (body loops).
// Registered for the body lifetime only when max_no_yield_ms > 0.
// agent_poll implementations live after AgentHandle + g_orch_module_stats.
inline std::mutex g_agent_coop_mu;
inline std::unordered_map<std::uint64_t, std::shared_ptr<AgentCoopYield>> g_agent_coop_by_fiber;

inline void register_agent_coop(std::uint64_t fiber_id,
                                std::shared_ptr<AgentCoopYield> coop) noexcept {
    if (!coop || fiber_id == 0 || coop->max_no_yield_ms == 0)
        return;
    std::lock_guard<std::mutex> lock(g_agent_coop_mu);
    g_agent_coop_by_fiber[fiber_id] = std::move(coop);
}

inline void unregister_agent_coop(std::uint64_t fiber_id) noexcept {
    if (fiber_id == 0)
        return;
    std::lock_guard<std::mutex> lock(g_agent_coop_mu);
    g_agent_coop_by_fiber.erase(fiber_id);
}

[[nodiscard]] inline std::shared_ptr<AgentCoopYield>
lookup_agent_coop(std::uint64_t fiber_id) noexcept {
    if (fiber_id == 0)
        return nullptr;
    std::lock_guard<std::mutex> lock(g_agent_coop_mu);
    auto it = g_agent_coop_by_fiber.find(fiber_id);
    if (it == g_agent_coop_by_fiber.end())
        return nullptr;
    return it->second;
}

// Cooperative sleep for keepalive cadence (steal-friendly yield loop).
// Issue #2159: when running on a fiber, yield in coarser wall-clock slices
// (check clock every N yields) to avoid multi-worker yield-spin thrash that
// races with mailbox notify + body join. Host-thread callers still sleep.
inline void fiber_sleep_ms(std::uint32_t ms) noexcept {
    if (!serve::g_current_fiber) {
        if (ms == 0)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return;
    }
    if (ms == 0) {
        serve::Fiber::yield(serve::YieldReason::Explicit);
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    // Coarser cadence: ~1 yield per ~100µs wall budget is enough for cancel
    // responsiveness without saturating the scheduler.
    constexpr int kYieldsPerClockCheck = 8;
    while (std::chrono::steady_clock::now() < deadline) {
        if (serve::g_current_fiber->is_cancel_requested())
            return;
        for (int i = 0; i < kYieldsPerClockCheck; ++i) {
            if (serve::g_current_fiber->is_cancel_requested())
                return;
            serve::Fiber::yield(serve::YieldReason::Explicit);
        }
    }
}

inline OrchModuleStats g_orch_module_stats{};

// Issue #2228 / #2398: record one process-wide mailbox BP event for the
// admit "recent" gauge + last-event clock. Called from push/fanout BP
// sites (via aura_orch_note_mailbox_backpressure strong def) and from
// orch agent_send / emit_keepalive when those paths already counted BP.
inline void note_mailbox_bp_recent_event() noexcept {
    g_orch_module_stats.mailbox_bp_recent_total.fetch_add(1, std::memory_order_relaxed);
    g_mailbox_bp_last_event_us.store(orch_now_us(), std::memory_order_release);
}

// Issue #2398: quiet-period decay of mailbox_bp_recent_total.
// Zero-cost when window_ms==0. Admit preflight calls this only when
// threshold>0 (zero cost when admit control is off). One CAS winner zeros.
inline void maybe_decay_mailbox_bp_recent() noexcept {
    const auto window_ms = resolve_mailbox_bp_window_ms();
    if (window_ms == 0)
        return;
    const auto now_us = orch_now_us();
    const auto last_bp = g_mailbox_bp_last_event_us.load(std::memory_order_acquire);
    if (last_bp == 0)
        return; // never saw a BP event
    if (now_us - last_bp <= window_ms * 1000ULL)
        return; // still inside quiet window after last BP
    auto last_decay = g_mailbox_bp_last_decay_us.load(std::memory_order_acquire);
    if (g_mailbox_bp_last_decay_us.compare_exchange_strong(last_decay, now_us,
                                                           std::memory_order_acq_rel)) {
        g_orch_module_stats.mailbox_bp_recent_total.store(0, std::memory_order_release);
    }
}

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
// Issue #2009: move-only RAII. Destructor releases any outstanding
// arena/mailbox reservation (reserved_memory_bytes). Long-lived agents must
// be stored in a container that keeps the handle alive (or join first).
// join_agent / join_agents / release_agent_memory_reservation are idempotent
// with the destructor (first call zeros reserved_memory_bytes).
struct AgentHandle {
    std::uint64_t id = 0; // Fiber::id()
    std::string name;
    serve::Fiber* fiber = nullptr;
    std::shared_ptr<serve::mf_mailbox::MultiFiberMailbox> mailbox;
    bool ok = false;
    // Issue #1600 / #1880: typed quota failure surface for Agent frameworks.
    bool quota_exceeded = false;
    std::string error; // e.g. "ResourceQuotaExceeded: fibers quota exceeded"
    // Issue #2079: structured quota-reject fields (machine-readable per Agent spec).
    // Empty / 0 when not a quota reject (success or non-quota failure).
    // Field names align with `query:resource-quota-stats` dimension names
    // ("fibers" | "memory" | "mutations") for Agent framework consistency.
    std::string quota_dimension;      // "fibers" | "memory" | "mutations" | "" (none)
    std::uint64_t quota_used = 0;     // current usage at reject time
    std::uint64_t quota_limit = 0;    // configured limit at reject time
    std::uint64_t retry_after_ms = 0; // suggested backoff (0 if unknown)
    // Issue #1880: memory reserved at spawn (released on join / scope exit).
    std::uint64_t reserved_memory_bytes = 0;
    // Issue #2008 / #2159: keepalive / liveness (null / 0 when disabled — zero cost).
    std::uint32_t keepalive_interval_ms = 0;
    std::shared_ptr<AgentLiveness> liveness; // shared body ↔ helper ↔ supervisor
    // True when a Scheduler-owned keepalive helper fiber was started (#2159).
    bool keepalive_active = false;
    // Issue #2159: fiber-native helper (null when disabled / spawn failed).
    // Joined by join_agent after body; cancelled on stop / dtor (no host thread).
    serve::Fiber* keepalive_helper = nullptr;
    // Issue #2540: cooperative yield contract (0 / null = off, zero cost).
    std::uint32_t max_no_yield_ms = 0;
    std::shared_ptr<AgentCoopYield> coop;

    AgentHandle() = default;
    AgentHandle(const AgentHandle&) = delete;
    AgentHandle& operator=(const AgentHandle&) = delete;

    AgentHandle(AgentHandle&& o) noexcept
        : id(o.id)
        , name(std::move(o.name))
        , fiber(o.fiber)
        , mailbox(std::move(o.mailbox))
        , ok(o.ok)
        , quota_exceeded(o.quota_exceeded)
        , error(std::move(o.error))
        , quota_dimension(std::move(o.quota_dimension))
        , quota_used(o.quota_used)
        , quota_limit(o.quota_limit)
        , retry_after_ms(o.retry_after_ms)
        , reserved_memory_bytes(o.reserved_memory_bytes)
        , keepalive_interval_ms(o.keepalive_interval_ms)
        , liveness(std::move(o.liveness))
        , keepalive_active(o.keepalive_active)
        , keepalive_helper(o.keepalive_helper)
        , max_no_yield_ms(o.max_no_yield_ms)
        , coop(std::move(o.coop)) {
        o.id = 0;
        o.fiber = nullptr;
        o.ok = false;
        o.quota_exceeded = false;
        o.quota_dimension.clear();
        o.quota_used = 0;
        o.quota_limit = 0;
        o.retry_after_ms = 0;
        o.reserved_memory_bytes = 0; // prevent double-release
        o.keepalive_interval_ms = 0;
        o.keepalive_active = false;
        o.keepalive_helper = nullptr;
        o.max_no_yield_ms = 0;
    }

    AgentHandle& operator=(AgentHandle&& o) noexcept {
        if (this != &o) {
            // Release our outstanding reservation before adopting o's.
            release_reservation_if_any();
            if (liveness)
                liveness->helper_stop.store(true, std::memory_order_release);
            id = o.id;
            name = std::move(o.name);
            fiber = o.fiber;
            mailbox = std::move(o.mailbox);
            ok = o.ok;
            quota_exceeded = o.quota_exceeded;
            error = std::move(o.error);
            quota_dimension = std::move(o.quota_dimension);
            quota_used = o.quota_used;
            quota_limit = o.quota_limit;
            retry_after_ms = o.retry_after_ms;
            reserved_memory_bytes = o.reserved_memory_bytes;
            keepalive_interval_ms = o.keepalive_interval_ms;
            liveness = std::move(o.liveness);
            keepalive_active = o.keepalive_active;
            keepalive_helper = o.keepalive_helper;
            max_no_yield_ms = o.max_no_yield_ms;
            coop = std::move(o.coop);
            o.id = 0;
            o.fiber = nullptr;
            o.ok = false;
            o.quota_exceeded = false;
            o.quota_dimension.clear();
            o.quota_used = 0;
            o.quota_limit = 0;
            o.retry_after_ms = 0;
            o.reserved_memory_bytes = 0;
            o.keepalive_interval_ms = 0;
            o.keepalive_active = false;
            o.keepalive_helper = nullptr;
            o.max_no_yield_ms = 0;
        }
        return *this;
    }

    ~AgentHandle() {
        // Best-effort cooperative stop of keepalive helper; body_done not set
        // here so a still-running body is not misreported as Done by
        // watch_agent_liveness. No join in dtor (same as body fiber) —
        // join_agent / scope joins helper. Prefer helper_stop over cancel.
        if (liveness)
            liveness->helper_stop.store(true, std::memory_order_release);
        keepalive_active = false;
        keepalive_helper = nullptr;
        release_reservation_if_any();
    }

    // Issue #2009 / #1880: idempotent reservation release (also used by join).
    void release_reservation_if_any() noexcept {
        if (reserved_memory_bytes == 0)
            return;
        aura::core::resource_quota::process_resource_quota().release_agent_arena(
            reserved_memory_bytes);
        reserved_memory_bytes = 0;
    }
};

struct AgentSpec {
    std::string name;
    std::function<void()> body; // required for spawn
    bool attach_mailbox = true;
    std::size_t mailbox_high_water = 256;
    // Issue #2008 / #2159: 0 = disabled (default, zero-cost). When > 0 and
    // mailbox attached, a Scheduler-owned helper fiber emits keepalive
    // pulses at this cadence (no detached host thread).
    std::uint32_t keepalive_interval_ms = 0;
    // Issue #2118: when true (default), agent fiber body soft-registers
    // MutationBoundary depth on the per-fiber stack so steal (#2115) and
    // GC see the mutation window. Set false for pure-reasoning agents
    // that never mutate (AC2 zero-cost path — quota check only).
    bool mutation_boundary = true;
    // Issue #2540: max wall-clock ms without a cooperative yield edge.
    // 0 = off (default, zero cost). When > 0, body should call agent_poll()
    // (or orch:agent-poll / note_agent_progress) so cancel/steal/GC can
    // cooperate before residual hard-reclaim (#2227 / #2533).
    std::uint32_t max_no_yield_ms = 0;
    // Issue #2591: per-spec override for mailbox BP admit threshold
    // (multi-tenant / multi-scope isolation). nullopt = process default
    // (#2535 default=32, env override via AURA_ORCH_BP_ADMIT_THRESHOLD);
    // 0 = admit off for this spawn (always reject under attach_mailbox);
    // N > 0 = local threshold (per-spawn policy isolation; gauge is
    // still process-global). Wire surface: Aura kwarg
    // :bp-admit-threshold n on (orch:spawn-agent).
    std::optional<std::uint64_t> bp_admit_threshold{};
};

// Issue #2585: production default for AgentSpec.max_no_yield_ms.
// Returns the effective coop window in milliseconds:
//   - spec_value > 0: respect it (no change).
//   - spec_value == 0 + dev_off: 0 (zero-cost, unit Soft ergonomics).
//   - spec_value == 0 + AURA_AGENT_MAX_NO_YIELD_MS=0 opt-out: 0 (zero-cost).
//   - else: 50ms default under production defaults.
//
// Caller bumps agent_no_yield_default_applied_total when the returned
// value differs from spec_value (i.e., default was injected).
[[nodiscard]] inline std::uint32_t resolve_agent_default_max_no_yield_ms(std::uint32_t spec_value,
                                                                         bool dev_off) noexcept {
    if (spec_value > 0)
        return spec_value;
    if (dev_off)
        return 0;
    const char* env = std::getenv("AURA_AGENT_MAX_NO_YIELD_MS");
    if (env && env[0] == '0' && env[1] == '\0')
        return 0; // explicit operator opt-out
    return 50;    // production default
}

// Issue #2540: force Fiber::yield when elapsed since last coop edge ≥
// max_no_yield_ms. Returns true when a forced yield ran.
// max_no_yield_ms==0 → always false (AC1 zero cost).
[[nodiscard]] inline bool agent_poll(AgentCoopYield& coop) noexcept {
    if (coop.max_no_yield_ms == 0)
        return false;
    auto* f = serve::g_current_fiber;
    if (!f)
        return false;
    const auto now = orch_now_us();
    const auto last = coop.last_coop_us.load(std::memory_order_relaxed);
    const auto window_us = static_cast<std::uint64_t>(coop.max_no_yield_ms) * 1000ULL;
    if (last != 0 && now >= last && (now - last) < window_us)
        return false;
    serve::Fiber::yield(serve::YieldReason::Explicit);
    coop.last_coop_us.store(orch_now_us(), std::memory_order_relaxed);
    g_orch_module_stats.agent_forced_yield_total.fetch_add(1, std::memory_order_relaxed);
    return true;
}

[[nodiscard]] inline bool agent_poll(AgentHandle& h) noexcept {
    if (!h.coop || h.max_no_yield_ms == 0)
        return false;
    return agent_poll(*h.coop);
}

// Current-fiber path: registry installed by spawn body when max_no_yield_ms > 0.
[[nodiscard]] inline bool agent_poll() noexcept {
    auto* f = serve::g_current_fiber;
    if (!f)
        return false;
    auto coop = lookup_agent_coop(f->id());
    if (!coop)
        return false;
    return agent_poll(*coop);
}

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
        // Cumulative send BP (hook also bumps when push notes BP;
        // keep explicit path so emit_keepalive remains self-contained
        // if the weak hook is a no-op in serve-only links).
        g_orch_module_stats.send_backpressure_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #2228 / #2398: recent gauge + last-event for quiet-period.
        // push() already calls note_backpressure → hook which also notes
        // recent when orch is linked; this path covers serve-only links
        // where the weak hook is a no-op. Double-count under full orch
        // link is acceptable for admit (threshold is rate-ish) and
        // send_backpressure remains monotonic (AC2).
        note_mailbox_bp_recent_event();
    } else {
        g_orch_module_stats.send_closed_total.fetch_add(1, std::memory_order_relaxed);
    }
    return st;
}

// Issue #2155: finalize quota-reject accounting. Contract:
//   !ok ⇒ reserved_memory_bytes == 0 (no permanent arena charge).
// Defensive release if residual reserved (should never fire). Bumps
// spawn_quota_reject_no_leak_ok_total when clean so Agents can trust storms.
inline void finalize_spawn_quota_reject(AgentHandle& h) noexcept {
    if (h.ok)
        return;
    if (h.reserved_memory_bytes != 0) {
        aura::core::resource_quota::process_resource_quota().release_agent_arena(
            h.reserved_memory_bytes);
        h.reserved_memory_bytes = 0;
        g_orch_module_stats.spawn_quota_reject_leak_detect_total.fetch_add(
            1, std::memory_order_relaxed);
        g_orch_module_stats.spawn_quota_reject_no_leak.store(0, std::memory_order_relaxed);
        return;
    }
    g_orch_module_stats.spawn_quota_reject_no_leak_ok_total.fetch_add(1, std::memory_order_relaxed);
    g_orch_module_stats.spawn_quota_reject_no_leak.store(1, std::memory_order_relaxed);
}

[[nodiscard]] inline AgentHandle spawn_agent_with_mailbox(serve::Scheduler& sched, AgentSpec spec) {
    AgentHandle h;
    h.name = std::move(spec.name);
    if (!spec.body) {
        g_orch_module_stats.spawn_failures.fetch_add(1, std::memory_order_relaxed);
        return h;
    }

    auto& pq = aura::core::resource_quota::process_resource_quota();

    // Issue #2008 / #2080 / #2159: mailbox keepalive uses a Scheduler-owned
    // helper fiber when attach_mailbox is set; otherwise (attach_mailbox=#f
    // + interval > 0) ProgressClock mode (body `orch:agent-touch` only —
    // no helper fiber).
    const bool want_keepalive = spec.keepalive_interval_ms > 0 && spec.attach_mailbox;
    const bool want_progress_clock = spec.keepalive_interval_ms > 0 && !spec.attach_mailbox;

    // Issue #1880 / #2159: fiber capacity preflight (check only; Scheduler::spawn
    // also consumes). Mailbox keepalive needs body + helper fiber (#2159).
    const std::uint64_t fiber_preflight = want_keepalive ? 2u : 1u;
    if (auto ferr = pq.check_orchestration_fibers(/*amount=*/fiber_preflight)) {
        g_orch_module_stats.spawn_failures.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.spawn_quota_rejects.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.resource_quota_rejects_total.fetch_add(1, std::memory_order_relaxed);
        pq.orch_resource_quota_rejects_total.fetch_add(1, std::memory_order_relaxed);
        // #1600: align preflight reject with Scheduler::spawn metric surface.
        pq.fiber_spawn_rejected_total.fetch_add(1, std::memory_order_relaxed);
        h.quota_exceeded = true;
        // Issue #2079: structured quota-reject fields (machine-readable per Agent spec).
        h.quota_dimension = "fibers";
        h.quota_used = ferr->used;
        h.quota_limit = ferr->limit;
        h.retry_after_ms = 50;
        h.error = "ResourceQuotaExceeded: " + ferr->message;
        // Issue #2155: reserved never set on fiber preflight; assert no-leak.
        finalize_spawn_quota_reject(h);
        return h;
    }

    // Issue #1880: arena + mailbox high-water memory reservation.
    const auto mem_cost = estimate_agent_memory_bytes(spec.mailbox_high_water, spec.attach_mailbox);
    if (auto merr = pq.try_consume_agent_arena(mem_cost)) {
        g_orch_module_stats.spawn_failures.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.spawn_quota_rejects.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.resource_quota_rejects_total.fetch_add(1, std::memory_order_relaxed);
        h.quota_exceeded = true;
        // Issue #2079: structured quota-reject fields (machine-readable per Agent spec).
        h.quota_dimension = "memory";
        h.quota_used = merr->used;
        h.quota_limit = merr->limit;
        h.retry_after_ms = 100;
        h.error = "ResourceQuotaExceeded: " +
                  aura::core::resource_quota::ResourceQuotaManager::format_reason(*merr);
        // Issue #2155: try_consume failed ⇒ no permanent arena charge / reserved=0.
        finalize_spawn_quota_reject(h);
        return h;
    }
    h.reserved_memory_bytes = mem_cost;

    // Issue #2228: mailbox-backpressure admission preflight. When
    // attach_mailbox is requested AND the process-wide BP event count
    // is at/above the configured admit threshold, soft-reject the
    // spawn with the same no-leak contract as #2155 (reserved=0).
    // Preflight runs AFTER the fiber + arena preflights so quota
    // rejects still get the fiber/memory structured fields
    // (quota_dimension = "fibers" / "memory"); BP is a separate
    // admission dimension so Agent frameworks can branch on it.
    if (spec.attach_mailbox) {
        // Issue #2228 / #2398 / #2535: mailbox-BP admit gate.
        // Production default threshold=32 (#2535). threshold==0 (env
        // opt-out) → admit control off; zero cost beyond the single
        // threshold load (no decay work, no BP reject).
        // Issue #2591: spec.bp_admit_threshold (optional) overrides
        // the process default for this spawn only — multi-tenant /
        // multi-scope isolation. nullopt → process default; 0 →
        // admit off for THIS spawn (always reject when attach_mailbox);
        // N > 0 → local threshold (per-spawn policy isolation, gauge
        // is still process-global).
        const auto override_threshold = spec.bp_admit_threshold;
        const auto process_threshold = resolve_mailbox_bp_admit_threshold();
        const auto threshold = override_threshold ? *override_threshold : process_threshold;
        const bool override_active = override_threshold.has_value();
        if (threshold > 0) {
            // Issue #2398: quiet-period decay — if no BP events for
            // window_ms (AURA_ORCH_BP_WINDOW_MS / AURA_ORCH_BP_DECAY_MS),
            // zero mailbox_bp_recent_total so storms self-heal without
            // process restart. send_backpressure_total stays cumulative.
            maybe_decay_mailbox_bp_recent();
            const auto bp_recent =
                g_orch_module_stats.mailbox_bp_recent_total.load(std::memory_order_relaxed);
            if (bp_recent >= threshold) {
                g_orch_module_stats.spawn_failures.fetch_add(1, std::memory_order_relaxed);
                if (override_active) {
                    // #2591: per-spec override deny (separate counter
                    // so multi-tenant hosts can distinguish "process
                    // default storm" from "local override storm").
                    g_orch_module_stats.spawn_bp_admit_reject_override_total.fetch_add(
                        1, std::memory_order_relaxed);
                } else {
                    g_orch_module_stats.spawn_bp_admit_reject_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
                h.quota_exceeded = true;
                h.quota_dimension = "mailbox-bp";
                h.quota_used = bp_recent;
                h.quota_limit = threshold;
                h.retry_after_ms = 50;
                h.error = "AdmissionRejected: mailbox backpressure (bp_recent=" +
                          std::to_string(bp_recent) + " >= threshold=" + std::to_string(threshold) +
                          " override=" + (override_active ? "true" : "false") + ")";
                // #2155 parity: reserved never set on BP reject
                // (h.reserved_memory_bytes still holds the planned
                // mem_cost, but finalize_spawn_quota_reject is no-leak
                // for the !ok path: it only releases if reserved != 0,
                // and BP reject happens before the arena reservation is
                // actually committed via the Scheduler; we explicitly
                // zero reserved here so no future code path can release
                // a phantom allocation).
                h.reserved_memory_bytes = 0;
                finalize_spawn_quota_reject(h);
                return h;
            }
        }
    }

    auto mb = spec.attach_mailbox
                  ? std::make_shared<serve::mf_mailbox::MultiFiberMailbox>(spec.mailbox_high_water)
                  : nullptr;
    auto body = std::move(spec.body);
    auto attach = spec.attach_mailbox;
    // Issue #2080: ProgressClock mode shares the same liveness struct + clock
    // (last_keepalive_us) as MailboxKeepalive, so watch_agent_liveness can use
    // the same staleness check across both modes.
    const auto ka_interval =
        (want_keepalive || want_progress_clock) ? spec.keepalive_interval_ms : 0u;
    std::shared_ptr<AgentLiveness> live;
    if (want_keepalive || want_progress_clock) {
        live = std::make_shared<AgentLiveness>();
        live->interval_ms = ka_interval;
    }
    // Issue #2585: production default coop window under !dev_off.
    // dev_off follows security_defaults.hh convention (AURA_SANDBOX=off).
    // The effective window is what the spawn path installs; default
    // injection (50ms) bumps agent_no_yield_default_applied_total once.
    const char* sb_e = std::getenv("AURA_SANDBOX");
    const bool dev_off_agent = sb_e && *sb_e && std::string_view(sb_e) == "off";
    const auto spec_max_no_yield_ms = spec.max_no_yield_ms;
    const auto effective_max_no_yield_ms =
        resolve_agent_default_max_no_yield_ms(spec_max_no_yield_ms, dev_off_agent);
    // Issue #2540: coop yield state only when effective window > 0 (zero-cost
    // when off; AC1 of #2540 preserved when opted out via AURA_AGENT_MAX_NO_YIELD_MS=0).
    std::shared_ptr<AgentCoopYield> coop;
    if (effective_max_no_yield_ms > 0) {
        coop = std::make_shared<AgentCoopYield>();
        coop->max_no_yield_ms = effective_max_no_yield_ms;
        coop->last_coop_us.store(orch_now_us(), std::memory_order_relaxed);
        // Issue #2585: count default injection (spec==0 → effective>0).
        if (effective_max_no_yield_ms != spec_max_no_yield_ms) {
            g_orch_module_stats.agent_no_yield_default_applied_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    const bool register_soft = spec.mutation_boundary;
    serve::Fiber* f = sched.spawn([body = std::move(body), mb, attach, live, coop,
                                   progress_clock = want_progress_clock, register_soft]() mutable {
        // Issue #2540: register fiber-local coop state for agent_poll().
        struct CoopReg {
            std::uint64_t fid = 0;
            explicit CoopReg(const std::shared_ptr<AgentCoopYield>& c) {
                if (!c || !serve::g_current_fiber)
                    return;
                fid = serve::g_current_fiber->id();
                register_agent_coop(fid, c);
                c->last_coop_us.store(orch_now_us(), std::memory_order_relaxed);
            }
            ~CoopReg() { unregister_agent_coop(fid); }
        } coop_reg(coop);

        if (attach && mb && serve::g_current_fiber)
            mb->attach(serve::g_current_fiber);
        // Issue #2080: ProgressClock mode — seed last_keepalive_us at body
        // entry so watch_agent_liveness has a baseline even if the body
        // never calls `orch:agent-touch`. MailboxKeepalive mode seeds the
        // same clock from emit_keepalive (#2008 / #2159 helper fiber).
        if (live && progress_clock) {
            const auto t0 = orch_now_us();
            live->last_keepalive_us.store(t0, std::memory_order_release);
            g_orch_module_stats.last_keepalive_us.store(t0, std::memory_order_relaxed);
        }
        // Issue #1880 / #2118: try_acquire mutation boundary when Evaluator
        // is bound. On fiber: soft-registers per-fiber depth when
        // mutation_boundary is true (default) so steal/GC see the window;
        // mutation_boundary=false keeps pure-reasoning zero-cost (AC2).
        // On reject: skip body (typed quota path already recorded); no panic.
        // Issue #2006: provenance closed-loop only after a successful body
        // that actually entered the acquire path — reject must not call
        // aura_evaluator_post_resume_refresh or bump provenance counters.
        const int acq = aura_orch_agent_body_try_acquire_ex(register_soft ? 1 : 0);
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
        // Issue #1600 / #2155: Scheduler::spawn returns nullptr on fiber
        // ResourceQuota after arena was already reserved — must release
        // before return so agent_arena_usage_bytes does not leak under storms.
        pq.release_agent_arena(mem_cost);
        h.reserved_memory_bytes = 0;
        g_orch_module_stats.spawn_failures.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.spawn_quota_rejects.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.resource_quota_rejects_total.fetch_add(1, std::memory_order_relaxed);
        h.quota_exceeded = true;
        // Issue #2079: structured quota-reject fields (Scheduler::spawn nullptr
        // mirrors the fiber preflight reject; we snapshot current quota state).
        h.quota_dimension = "fibers";
        h.quota_used = pq.used(aura::core::resource_quota::Dimension::Fibers);
        h.quota_limit = pq.limit(aura::core::resource_quota::Dimension::Fibers);
        h.retry_after_ms = 50;
        h.error = "ResourceQuotaExceeded: fibers quota exceeded";
        finalize_spawn_quota_reject(h);
        return h;
    }

    h.fiber = f;
    h.id = f->id();
    h.mailbox = mb;
    h.ok = true;
    h.keepalive_interval_ms = ka_interval;
    h.liveness = live;
    h.max_no_yield_ms = effective_max_no_yield_ms;
    h.coop = std::move(coop);
    g_orch_module_stats.agents_spawned.fetch_add(1, std::memory_order_relaxed);
    g_orch_module_stats.agents_active.fetch_add(1, std::memory_order_relaxed);

    // Issue #2008 / #2159: optional Scheduler-owned keepalive helper fiber
    // (mailbox-native pulses). Fiber-native so cancel/GC/steal share the agent
    // lifecycle; shared_ptr keeps MultiFiberMailbox + AgentLiveness alive across
    // steals. Helper must not assume g_current_fiber is the agent body — emit
    // uses mailbox only (no attach, no Evaluator apply). Default path zero-cost.
    if (want_keepalive && mb && live) {
        const auto agent_id = h.id;
        const auto interval = ka_interval;
        auto mb_keep = mb; // shared ownership with handle + helper
        auto live_keep = live;
        serve::Fiber* helper = sched.spawn([mb_keep, live_keep, agent_id, interval]() {
            if (!mb_keep || !live_keep)
                return;
            // Immediate first pulse (same as host-thread path #2008).
            (void)emit_keepalive(*mb_keep, agent_id, live_keep.get());
            // Stay alive until body_done (or cancel). helper_stop only
            // suppresses further emits so supervisors can age the clock for
            // stall detection without the helper fiber completing while the
            // body is still running (Done trampoline under multi-worker steal
            // races mailbox attachers on the body).
            auto next_emit = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(std::max<std::uint32_t>(1, interval));
            while (!live_keep->body_done.load(std::memory_order_acquire)) {
                if (serve::g_current_fiber && serve::g_current_fiber->is_cancel_requested())
                    break;
                fiber_sleep_ms(1);
                if (live_keep->body_done.load(std::memory_order_acquire))
                    break;
                if (serve::g_current_fiber && serve::g_current_fiber->is_cancel_requested())
                    break;
                // helper_stop: park without emit (stall-sim / join signal).
                if (live_keep->helper_stop.load(std::memory_order_acquire))
                    continue;
                const auto now = std::chrono::steady_clock::now();
                if (now < next_emit)
                    continue;
                (void)emit_keepalive(*mb_keep, agent_id, live_keep.get());
                next_emit = now + std::chrono::milliseconds(std::max<std::uint32_t>(1, interval));
            }
        });
        if (helper) {
            h.keepalive_helper = helper;
            h.keepalive_active = true;
            g_orch_module_stats.keepalive_helpers_spawned.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Body still runs; keepalive disabled for this agent.
            g_orch_module_stats.keepalive_helper_spawn_fail.fetch_add(1, std::memory_order_relaxed);
            h.keepalive_interval_ms = 0;
            h.keepalive_active = false;
            h.keepalive_helper = nullptr;
        }
    }

    return h;
}

// Issue #1880 / #2009: release spawn-time memory reservation (idempotent,
// zero-cost after first call). Safe under concurrent join + scope exit:
// first caller zeros reserved_memory_bytes; second is a no-op.
inline void release_agent_memory_reservation(AgentHandle& h) noexcept {
    h.release_reservation_if_any();
}

// Stop keepalive helper (if any). Sets helper_stop so the fiber-native helper
// exits its 1ms poll loop (#2159). Does not request_cancel by default — cancel
// mid yield-spin races the trampoline under multi-worker steal. join_keepalive
// may cancel as a last resort after a drain window. Does not mark body_done.
// Does not join (see join_keepalive_helper).
inline void stop_keepalive_helper(AgentHandle& h) noexcept {
    if (h.liveness)
        h.liveness->helper_stop.store(true, std::memory_order_release);
    h.keepalive_active = false;
}

// Issue #2159: join fiber-native keepalive helper after body (short drain).
// Idempotent: null helper or already-Done is a no-op. Clears keepalive_helper.
inline void
join_keepalive_helper(AgentHandle& h,
                      std::uint64_t drain_ms = kDefaultKeepaliveHelperDrainMs) noexcept {
    if (!h.keepalive_helper)
        return;
    stop_keepalive_helper(h); // cooperative helper_stop first
    if (h.keepalive_helper && !h.keepalive_helper->is_done()) {
        auto jr = serve::Fiber::join(h.keepalive_helper, std::optional<std::uint64_t>{drain_ms});
        // Last-resort cancel only if cooperative stop did not finish.
        if (jr.status != serve::JoinStatus::Ok && h.keepalive_helper &&
            !h.keepalive_helper->is_done()) {
            h.keepalive_helper->request_cancel();
            (void)serve::Fiber::join(h.keepalive_helper, std::optional<std::uint64_t>{drain_ms});
        }
    }
    if (h.keepalive_helper && h.keepalive_helper->is_done())
        g_orch_module_stats.keepalive_helpers_joined_total.fetch_add(1, std::memory_order_relaxed);
    h.keepalive_helper = nullptr;
    h.keepalive_active = false;
}

// Issue #2153: request_cancel + optional secondary join; bump residual if
// the body is still live after the drain window (cooperative cancel only).
// drain_ms=0 → cancel only (no secondary wait). Never runs on Ok path.
//
// Issue #2227: residual path now also drives the hard-reclaim protocol.
// After the cooperative drain, !is_done() means the body didn't yield
// / didn't poll is_cancel_requested(). We register the fiber with its
// owner Scheduler for force-reclaim after a hard deadline, then bump
// join_drain_residual_reclaim_total. The Scheduler's reap_orphans_now
// (called by the IO thread tick + tests) drops the fiber from its
// tracking maps and marks it reclaimed_ so existing joiners see
// "logically done" via is_done(). Non-yielding bodies still leak
// stack until they return — documented limitation, same as #2153.
inline void cancel_and_drain_fiber(serve::Fiber* f, std::uint64_t drain_ms) noexcept {
    if (!f || f->is_done())
        return;
    f->request_cancel();
    if (drain_ms > 0) {
        const auto t0 = std::chrono::steady_clock::now();
        (void)serve::Fiber::join(f, std::optional<std::uint64_t>{drain_ms});
        const auto us =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - t0)
                                           .count());
        g_orch_module_stats.join_drain_us_total.fetch_add(us, std::memory_order_relaxed);
    }
    if (!f->is_done()) {
        g_orch_module_stats.join_drain_residual_total.fetch_add(1, std::memory_order_relaxed);
        if (auto* sched = f->owner_sched()) {
            // Issue #2227: hard deadline = drain_ms * 8 (4x headroom
            // over drain_ms so the cooperative cancel has a real
            // chance, capped at kJoinDrainResidualHardMsDefault).
            const std::uint64_t hard_ms = std::min<std::uint64_t>(
                drain_ms > 0 ? drain_ms * 8 : kJoinDrainResidualHardMsDefault,
                kJoinDrainResidualHardMsDefault);
            sched->note_orphan_fiber(f, hard_ms);
        }
        g_orch_module_stats.join_drain_residual_reclaim_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
    }
}

// Batch cancel+drain for not-yet-Done fibers (join_agents / AgentScope).
inline void cancel_and_drain_fibers(std::span<serve::Fiber* const> fibers,
                                    std::uint64_t drain_ms) noexcept {
    if (fibers.empty())
        return;
    for (auto* f : fibers) {
        if (f && !f->is_done())
            f->request_cancel();
    }
    std::vector<serve::Fiber*> not_done;
    not_done.reserve(fibers.size());
    for (auto* f : fibers) {
        if (f && !f->is_done())
            not_done.push_back(f);
    }
    if (not_done.empty())
        return;
    if (drain_ms > 0) {
        const auto t0 = std::chrono::steady_clock::now();
        (void)serve::Fiber::join(std::span<serve::Fiber* const>(not_done),
                                 std::optional<std::uint64_t>{drain_ms});
        const auto us =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - t0)
                                           .count());
        g_orch_module_stats.join_drain_us_total.fetch_add(us, std::memory_order_relaxed);
    }
    std::uint64_t residual = 0;
    for (auto* f : not_done) {
        if (f && !f->is_done())
            ++residual;
    }
    if (residual > 0) {
        g_orch_module_stats.join_drain_residual_total.fetch_add(residual,
                                                                std::memory_order_relaxed);
        // Issue #2227: batch hard-reclaim — register each residual
        // fiber with its owner Scheduler. Bumps the reclaim counter
        // once per fiber (matches the residual count for AgentScope
        // / join_agents paths). Best-effort: fibers without an
        // owner_sched (test / host-thread) are skipped.
        const std::uint64_t hard_ms =
            std::min<std::uint64_t>(drain_ms > 0 ? drain_ms * 8 : kJoinDrainResidualHardMsDefault,
                                    kJoinDrainResidualHardMsDefault);
        for (auto* f : not_done) {
            if (!f || f->is_done())
                continue;
            if (auto* sched = f->owner_sched()) {
                sched->note_orphan_fiber(f, hard_ms);
            }
            g_orch_module_stats.join_drain_residual_reclaim_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
}

// Join a single agent (Fiber::join) + Issue #1879 post-join provenance.
// Issue #2008 / #2159: stop helper → join body (primary) → join helper (short drain).
// Issue #2153: JoinPolicy controls primary timeout + secondary drain_ms.
[[nodiscard]] inline serve::JoinResult join_agent(AgentHandle& h, JoinPolicy policy) {
    if (!h.ok || !h.fiber) {
        serve::JoinResult r;
        r.status = serve::JoinStatus::Invalid;
        return r;
    }
    // Issue #2008 / #2159: stop keepalive helper first (signal + cancel fiber).
    stop_keepalive_helper(h);
    if (h.liveness)
        h.liveness->body_done.store(true, std::memory_order_release);

    auto jr = serve::Fiber::join(h.fiber, policy.primary_ms);
    g_orch_module_stats.agents_joined.fetch_add(1, std::memory_order_relaxed);
    g_orch_module_stats.join_wait_us_total.fetch_add(jr.wait_us, std::memory_order_relaxed);
    if (jr.status == serve::JoinStatus::Ok)
        g_orch_module_stats.join_ok_total.fetch_add(1, std::memory_order_relaxed);
    else
        g_orch_module_stats.join_fail_total.fetch_add(1, std::memory_order_relaxed);
    // Issue #2082 / #2153: On non-Ok join (Timeout / Cancelled / Invalid), the
    // body fiber may still be running and allocating. Releasing the arena
    // reservation now would under-account live work. request_cancel + policy
    // drain window (default 2s); residual metric if body still live (tight
    // non-yielding loop). Reservation release remains idempotent (#2009).
    if (jr.status != serve::JoinStatus::Ok)
        cancel_and_drain_fiber(h.fiber, policy.drain_ms);
    // Issue #2159: join helper after body (short drain; no host-thread leak).
    {
        const auto helper_drain = policy.drain_ms == 0
                                      ? kDefaultKeepaliveHelperDrainMs
                                      : std::min(policy.drain_ms, kDefaultKeepaliveHelperDrainMs);
        join_keepalive_helper(h, helper_drain);
    }
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
    // Provenance only on Ok (AC4) — never after cancel/drain path.
    if (jr.status == serve::JoinStatus::Ok)
        orch_post_join_provenance(h.fiber);
    // Issue #1880 / #2082: free arena/mailbox reservation only after
    // Ok join or after cancel+drain best-effort above. Idempotent with
    // ~AgentHandle (#2009 invariant preserved).
    release_agent_memory_reservation(h);
    return jr;
}

// Backward-compatible: primary timeout only, default drain_ms=2000 (#2082).
[[nodiscard]] inline serve::JoinResult join_agent(AgentHandle& h,
                                                  std::optional<std::uint64_t> timeout_ms = {}) {
    return join_agent(h, JoinPolicy{.primary_ms = timeout_ms, .drain_ms = kDefaultJoinDrainMs});
}

// Join many agents + Issue #1879 post-join provenance per fiber.
// Issue #2008 / #2159: stop helpers → join bodies → join helpers (short drain).
// Issue #2153: JoinPolicy for primary + secondary drain.
[[nodiscard]] inline serve::JoinResult join_agents(std::span<AgentHandle> agents,
                                                   JoinPolicy policy) {
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
    auto jr = serve::Fiber::join(std::span<serve::Fiber* const>(fibers), policy.primary_ms);
    g_orch_module_stats.agents_joined.fetch_add(fibers.size(), std::memory_order_relaxed);
    g_orch_module_stats.join_wait_us_total.fetch_add(jr.wait_us, std::memory_order_relaxed);
    if (jr.status == serve::JoinStatus::Ok)
        g_orch_module_stats.join_ok_total.fetch_add(fibers.size(), std::memory_order_relaxed);
    else
        g_orch_module_stats.join_fail_total.fetch_add(1, std::memory_order_relaxed);
    // Issue #2082 / #2153: On non-Ok batch join, cancel + drain before release.
    if (jr.status != serve::JoinStatus::Ok)
        cancel_and_drain_fibers(std::span<serve::Fiber* const>(fibers), policy.drain_ms);
    // Issue #2159: join keepalive helpers after bodies.
    {
        const auto helper_drain = policy.drain_ms == 0
                                      ? kDefaultKeepaliveHelperDrainMs
                                      : std::min(policy.drain_ms, kDefaultKeepaliveHelperDrainMs);
        for (auto& a : agents)
            join_keepalive_helper(a, helper_drain);
    }
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

[[nodiscard]] inline serve::JoinResult join_agents(std::span<AgentHandle> agents,
                                                   std::optional<std::uint64_t> timeout_ms = {}) {
    return join_agents(agents,
                       JoinPolicy{.primary_ms = timeout_ms, .drain_ms = kDefaultJoinDrainMs});
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
    else if (st == serve::mf_mailbox::PushStatus::Backpressure) {
        g_orch_module_stats.send_backpressure_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #2228 / #2398: recent gauge + last-event (see emit_keepalive).
        note_mailbox_bp_recent_event();
    } else
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

// Issue #2231 / #2401 / #2538: agent-ask request/response helpers.
// No process-global AgentRegistry (#1966): a short-lived pending-ask
// table maps corr_id → per-ask reply mailbox so agent_reply can find
// the destination (not an agent map).
//
// Protocol layers (both active; dual-write for compatibility):
//   Typed (#2538): MailMessage.kind = Ask|Reply + correlation_id
//     → ask↔reply match without parsing payload text.
//   Legacy (#2231/#2401): payload prefixes
//     request: "ask:<corr-id>:" + body
//     reply:   "reply:<corr-id>:" + body
//
// Interleave safety (AC5 #2231): the reply mailbox is unique per ask,
// so concurrent asks to the same target don't drop unrelated messages.
inline constexpr int kAgentAskIssue = 2231;
inline constexpr int kAgentReplyIssue = 2401;
inline constexpr int kAgentAskTypedCorrIssue = 2538;

struct AskResult {
    bool ok = false;
    std::string status;  // "ok" | "timeout" | "no-mailbox" | "malformed"
    std::string payload; // reply body (empty on timeout / malformed)
    std::uint64_t correlation_id = 0;
};

// Issue #2401: structured result for agent_reply (no hang).
struct ReplyResult {
    bool ok = false;
    // "ok" | "unknown-corr" | "closed" | "backpressure" | "no-mailbox"
    std::string status;
};

// Issue #2538: parsed ask envelope (typed fields preferred, text fallback).
struct AskEnvelope {
    std::uint64_t correlation_id = 0;
    std::string_view body; // view into MailMessage::payload
    bool typed = false;    // true when match used MailKind/correlation_id
};

// Pending-ask table: corr_id → per-ask reply mailbox.
// Lifetime = one agent_ask wait window only (RAII unregister).
// NOT AgentRegistry / global agent map (#1966).
inline std::mutex g_pending_ask_mu;
inline std::unordered_map<std::uint64_t, std::shared_ptr<serve::mf_mailbox::MultiFiberMailbox>>
    g_pending_asks;

[[nodiscard]] inline std::string format_ask_prefix(std::uint64_t corr_id) {
    std::string p;
    p.reserve(24);
    p.append("ask:");
    p.append(std::to_string(corr_id));
    p.append(":");
    return p;
}

[[nodiscard]] inline std::string format_reply_prefix(std::uint64_t corr_id) {
    std::string p;
    p.reserve(24);
    p.append("reply:");
    p.append(std::to_string(corr_id));
    p.append(":");
    return p;
}

[[nodiscard]] inline std::string format_ask_payload(std::uint64_t corr_id, std::string_view body) {
    std::string p = format_ask_prefix(corr_id);
    p.append(body);
    return p;
}

[[nodiscard]] inline std::string format_reply_payload(std::uint64_t corr_id,
                                                      std::string_view body) {
    std::string p = format_reply_prefix(corr_id);
    p.append(body);
    return p;
}

// Issue #2538: extract ask corr+body. Prefers typed MailKind::Ask +
// correlation_id (no payload text parse). Falls back to "ask:<id>:" prefix.
[[nodiscard]] inline std::optional<AskEnvelope>
try_parse_ask(const serve::mf_mailbox::MailMessage& m) noexcept {
    using serve::mf_mailbox::MailKind;
    if (m.kind == MailKind::Ask && m.correlation_id != 0) {
        AskEnvelope e;
        e.correlation_id = m.correlation_id;
        e.typed = true;
        const auto prefix = format_ask_prefix(m.correlation_id);
        if (m.payload.size() >= prefix.size() && m.payload.compare(0, prefix.size(), prefix) == 0) {
            e.body = std::string_view(m.payload).substr(prefix.size());
        } else {
            // Pure typed body (no dual-write prefix).
            e.body = std::string_view(m.payload);
        }
        return e;
    }
    // Legacy text-prefix path (#2231).
    constexpr std::string_view kAsk = "ask:";
    if (m.payload.size() < kAsk.size() || m.payload.compare(0, kAsk.size(), kAsk) != 0)
        return std::nullopt;
    const auto colon = m.payload.find(':', kAsk.size());
    if (colon == std::string::npos)
        return std::nullopt;
    const auto corr_s = m.payload.substr(kAsk.size(), colon - kAsk.size());
    std::uint64_t corr = 0;
    try {
        corr = static_cast<std::uint64_t>(std::stoull(corr_s));
    } catch (...) {
        return std::nullopt;
    }
    if (corr == 0)
        return std::nullopt;
    AskEnvelope e;
    e.correlation_id = corr;
    e.typed = false;
    e.body = std::string_view(m.payload).substr(colon + 1);
    return e;
}

// Issue #2538: extract reply body for expected corr. Typed path first.
// Returns nullopt when message is not a matching reply.
[[nodiscard]] inline std::optional<std::string>
try_match_reply(const serve::mf_mailbox::MailMessage& m, std::uint64_t expected_corr,
                bool* matched_typed = nullptr) noexcept {
    using serve::mf_mailbox::MailKind;
    if (matched_typed)
        *matched_typed = false;
    if (m.kind == MailKind::Reply && m.correlation_id == expected_corr && expected_corr != 0) {
        if (matched_typed)
            *matched_typed = true;
        const auto prefix = format_reply_prefix(expected_corr);
        if (m.payload.size() >= prefix.size() && m.payload.compare(0, prefix.size(), prefix) == 0) {
            return m.payload.substr(prefix.size());
        }
        // Pure typed body (no dual-write prefix) — AC1: no text parse needed
        // for match; body is the full payload.
        return m.payload;
    }
    // Legacy text-prefix path (#2231 / #2401).
    const auto prefix = format_reply_prefix(expected_corr);
    if (m.payload.size() >= prefix.size() && m.payload.compare(0, prefix.size(), prefix) == 0) {
        if (matched_typed)
            *matched_typed = false;
        return m.payload.substr(prefix.size());
    }
    return std::nullopt;
}

// Issue #2401 / #2538: standard worker-side reply path.
// Stamps MailKind::Reply + correlation_id (typed) and dual-writes
// "reply:<corr>:" + body for legacy consumers. Pushes to:
//   1) explicit reply_dest if non-null, else
//   2) pending-ask table entry for corr_id (registered by agent_ask).
// Priority Normal (documented). Unknown corr / closed → structured fail
// (no hang). Optional `from` stamps from_fiber for diagnostics only.
[[nodiscard]] inline ReplyResult
agent_reply(std::uint64_t corr_id, std::string_view body,
            serve::mf_mailbox::MultiFiberMailbox* reply_dest = nullptr,
            AgentHandle* from = nullptr) noexcept {
    ReplyResult out;
    std::shared_ptr<serve::mf_mailbox::MultiFiberMailbox> held;
    serve::mf_mailbox::MultiFiberMailbox* dest = reply_dest;
    if (!dest) {
        std::lock_guard<std::mutex> lock(g_pending_ask_mu);
        auto it = g_pending_asks.find(corr_id);
        if (it == g_pending_asks.end() || !it->second) {
            out.status = "unknown-corr";
            g_orch_module_stats.agent_reply_fail_total.fetch_add(1, std::memory_order_relaxed);
            return out;
        }
        held = it->second;
        dest = held.get();
    }
    if (!dest) {
        out.status = "no-mailbox";
        g_orch_module_stats.agent_reply_fail_total.fetch_add(1, std::memory_order_relaxed);
        return out;
    }
    if (dest->closed()) {
        out.status = "closed";
        g_orch_module_stats.agent_reply_fail_total.fetch_add(1, std::memory_order_relaxed);
        return out;
    }
    serve::mf_mailbox::MailMessage msg;
    msg.payload = format_reply_payload(corr_id, body); // dual-write legacy prefix
    msg.priority = serve::mf_mailbox::MailPriority::Normal;
    msg.to_fiber = 0; // any / broadcast on per-ask temp mailbox
    // Issue #2538: typed correlation fields (primary match path).
    msg.correlation_id = corr_id;
    msg.kind = serve::mf_mailbox::MailKind::Reply;
    if (from && from->ok)
        msg.from_fiber = from->id;
    const auto st = dest->push(std::move(msg));
    if (st == serve::mf_mailbox::PushStatus::Ok) {
        out.ok = true;
        out.status = "ok";
        g_orch_module_stats.agent_reply_total.fetch_add(1, std::memory_order_relaxed);
        g_orch_module_stats.agent_reply_typed_total.fetch_add(1, std::memory_order_relaxed);
        return out;
    }
    if (st == serve::mf_mailbox::PushStatus::Backpressure) {
        out.status = "backpressure";
    } else if (st == serve::mf_mailbox::PushStatus::Closed) {
        out.status = "closed";
    } else {
        out.status = "no-mailbox";
    }
    g_orch_module_stats.agent_reply_fail_total.fetch_add(1, std::memory_order_relaxed);
    return out;
}

// Overload: self handle first (issue pseudo-code shape).
[[nodiscard]] inline ReplyResult
agent_reply(AgentHandle& self, std::uint64_t corr_id, std::string_view body,
            serve::mf_mailbox::MultiFiberMailbox* reply_dest = nullptr) noexcept {
    return agent_reply(corr_id, body, reply_dest, &self);
}

[[nodiscard]] inline AskResult agent_ask(AgentHandle& target, std::string_view body,
                                         std::uint64_t timeout_ms) noexcept {
    AskResult out;
    if (!target.ok || !target.mailbox) {
        out.status = "no-mailbox";
        return out;
    }
    // Process atomic correlation id (no global agent map).
    static std::atomic<std::uint64_t> g_ask_corr_id{0};
    const auto corr_id = g_ask_corr_id.fetch_add(1, std::memory_order_relaxed) + 1;
    out.correlation_id = corr_id;
    // Fresh reply mailbox so unrelated traffic doesn't interleave.
    auto reply_mb = std::make_shared<serve::mf_mailbox::MultiFiberMailbox>(/*high_water=*/16);
    // Issue #2401: register so agent_reply(corr, body) can find dest.
    {
        std::lock_guard<std::mutex> lock(g_pending_ask_mu);
        g_pending_asks[corr_id] = reply_mb;
    }
    // RAII unregister on every exit path (ok / timeout / malformed / early).
    struct PendingAskGuard {
        std::uint64_t id;
        ~PendingAskGuard() {
            std::lock_guard<std::mutex> lock(g_pending_ask_mu);
            g_pending_asks.erase(id);
        }
    } pending_guard{corr_id};

    serve::mf_mailbox::MailMessage msg;
    msg.payload = format_ask_payload(corr_id, body); // dual-write legacy prefix
    msg.priority = serve::mf_mailbox::MailPriority::Normal;
    msg.to_fiber = target.id;
    // Issue #2538: typed correlation fields (primary match path).
    msg.correlation_id = corr_id;
    msg.kind = serve::mf_mailbox::MailKind::Ask;
    auto st = target.mailbox->push(std::move(msg));
    if (st == serve::mf_mailbox::PushStatus::Closed) {
        out.status = "no-mailbox";
        return out;
    }
    // Backpressure → surface as timeout (no automatic retry; #2007 / #2228).
    if (st != serve::mf_mailbox::PushStatus::Ok) {
        out.status = "timeout";
        g_orch_module_stats.agent_ask_timeout_total.fetch_add(1, std::memory_order_relaxed);
        return out;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            out.status = "timeout";
            g_orch_module_stats.agent_ask_timeout_total.fetch_add(1, std::memory_order_relaxed);
            return out;
        }
        const auto remaining_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        auto m = reply_mb->recv(/*wait=*/true, remaining_ms, /*fiber_id=*/0);
        if (!m)
            continue;
        // Issue #2538: typed match first, then legacy text prefix (AC1/AC2).
        bool typed = false;
        if (auto body_opt = try_match_reply(*m, corr_id, &typed)) {
            out.ok = true;
            out.status = "ok";
            out.payload = std::move(*body_opt);
            g_orch_module_stats.agent_ask_total.fetch_add(1, std::memory_order_relaxed);
            if (typed) {
                g_orch_module_stats.agent_ask_typed_match_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
            return out;
        }
        // Non-matching on per-ask mailbox → malformed (AC5: no silent drop).
        out.status = "malformed";
        out.payload = m->payload;
        return out;
    }
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

// Issue #2229: supervision policy surface for long-lived agent
// stall responses. Extends the existing StallPolicy (#2161,
// ReportOnly + Cancel) with RestartN — re-spawn the body under
// the same AgentSpec / name rules, capped at max_restarts with a
// consecutive-stall circuit. Used by AgentScope::watch_all to
// turn "kill on stall" into recoverable multi-agent coordination
// (the issue's gap). Phase A: ReportOnly + Cancel (formalize).
// Phase B: RestartN. Phase C: optional CircuitBreaker mirror of
// #2007 deferred (follow-up).
enum class AgentFailureAction : std::uint8_t {
    ReportOnly = 0, // aggregate counts only; no cancel, no restart
    Cancel = 1,     // request_cancel the stalled body + helper
    RestartN = 2,   // re-spawn body under the same AgentSpec / name rules
};

struct AgentFailurePolicy {
    // Response to a single stall observed in watch_all. Default
    // Cancel matches the existing StallPolicy::Cancel behaviour
    // (#2161) so callers adopting AgentFailurePolicy get the
    // same out-of-the-box semantics.
    AgentFailureAction on_stall = AgentFailureAction::Cancel;
    // Response to a non-Ok join (Timeout / Cancelled) on the
    // scope level. AC4: documented scope (currently ReportOnly
    // only — the residual-reclaim path from #2227 handles the
    // fiber lifecycle; a separate restart hook on join_fail is
    // out of scope for #2229).
    AgentFailureAction on_join_fail = AgentFailureAction::ReportOnly;
    // RestartN cap. 0 = restart disabled (Cancel-only behaviour).
    std::uint32_t max_restarts = 0;
    // Circuit-like threshold: after this many consecutive
    // observed stalls, the scope force-downgrades to Cancel
    // even if on_stall == RestartN. Bumps
    // agent_consecutive_stall_total once per stall observed.
    std::uint32_t consecutive_stall_limit = 3;
    // Optional backoff between cancel + join drain + respawn
    // in RestartN path. 0 = no backoff (immediate respawn).
    std::uint32_t restart_backoff_ms = 0;
};

// StallPolicy (#2161) is the binary ReportOnly / Cancel subset
// preserved for callers that don't need the new RestartN surface.
// Kept in agent_scope.h for backward compat.
namespace agent_scope_compat {
    // Map StallPolicy::ReportOnly / Cancel to the new AgentFailureAction
    // enum (RestartN has no StallPolicy equivalent; callers wanting
    // RestartN must use AgentFailurePolicy directly).
    inline AgentFailureAction stall_to_failure_action(bool cancel_on_stall) noexcept {
        return cancel_on_stall ? AgentFailureAction::Cancel : AgentFailureAction::ReportOnly;
    }
} // namespace agent_scope_compat

// Issue #2539: unidirectional FailurePolicy (#2007 batch) →
// AgentFailurePolicy (#2229 long-lived) mapping bridge.
//
// Semantic boundary (documented; do not invert without a new API):
//   - FailurePolicy: body-error admit/retry under parallel_intend.
//   - AgentFailurePolicy: stall response under AgentScope::watch_all.
//   - RestartN is only meaningful for long-lived agents with keepalive;
//     max_restarts=0 leaves RestartN capped at zero (no re-spawn).
//   - Calling this bridge does not change default AgentFailurePolicy
//     or ParallelPolicy behaviour for callers that never use it (AC3).
//
// Mapping table (AC2):
//   FailFast        → on_stall=Cancel
//   CollectAll      → on_stall=ReportOnly
//   RetryN          → on_stall=RestartN, max_restarts from arg
//   CircuitBreaker  → on_stall=Cancel, consecutive_stall_limit aligned
//
// Optional language sugar (orch:supervise-batch / apply after
// parallel_intend) is deferred — this issue ships the mapping API only.
inline constexpr int kFailurePolicyBridgeIssue = 2539;

[[nodiscard]] inline AgentFailurePolicy
to_agent_policy(serve::parallel_orch::FailurePolicy p, std::uint32_t max_restarts = 0,
                std::uint32_t consecutive_stall_limit = 3,
                std::uint32_t restart_backoff_ms = 0) noexcept {
    AgentFailurePolicy out; // defaults match #2229 (Cancel / limit=3 / restarts=0)
    using FP = serve::parallel_orch::FailurePolicy;
    switch (p) {
        case FP::FailFast:
            out.on_stall = AgentFailureAction::Cancel;
            break;
        case FP::CollectAll:
            out.on_stall = AgentFailureAction::ReportOnly;
            break;
        case FP::RetryN:
            out.on_stall = AgentFailureAction::RestartN;
            out.max_restarts = max_restarts;
            out.restart_backoff_ms = restart_backoff_ms;
            break;
        case FP::CircuitBreaker:
            out.on_stall = AgentFailureAction::Cancel;
            out.consecutive_stall_limit = consecutive_stall_limit;
            break;
    }
    return out;
}

// Overload: pull RetryN max_retries / CircuitBreaker consecutive_fail_limit
// / retry_backoff_ms from ParallelPolicy (via resolved_failure_policy).
[[nodiscard]] inline AgentFailurePolicy
to_agent_policy(const serve::parallel_orch::ParallelPolicy& pp) noexcept {
    const auto fp = serve::parallel_orch::resolved_failure_policy(pp);
    return to_agent_policy(fp, pp.max_retries, pp.consecutive_fail_limit, pp.retry_backoff_ms);
}

// Wait up to stall_timeout_ms (default 2× keepalive_interval_ms) for a
// keepalive. Prefers the shared last_keepalive clock (set by the helper
// fiber) and only does non-blocking mailbox peeks — safe from host threads
// concurrent with the keepalive helper. On stall, optionally request_cancel
// the agent body + helper and bump stalled_agents_total / keepalive_cancels_total.
[[nodiscard]] inline KeepaliveWatchResult watch_agent_liveness(AgentHandle& h,
                                                               std::uint32_t stall_timeout_ms = 0,
                                                               bool cancel_on_stall = true) {
    KeepaliveWatchResult out;
    // Issue #2080: ProgressClock mode (attach_mailbox=#f + interval > 0) is
    // accepted: the body entry seeded last_keepalive_us and `orch:agent-touch`
    // keeps it fresh. The mailbox peek path is skipped below when no mailbox.
    if (!h.ok || h.keepalive_interval_ms == 0) {
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
        // Skip for ProgressClock mode (#2080): no mailbox to peek.
        if (h.mailbox) {
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

// Note (Issue #2080): ProgressClock progress touch (no mailbox).
// Agents in ProgressClock mode (attach_mailbox=#f + interval > 0) call this
// from the body to update the shared last_keepalive_us clock so
// watch_agent_liveness can distinguish Alive vs Stalled. MailboxKeepalive
// mode is unchanged — the fiber-native helper owns the clock (#2159).
// Issue #2540: also a recommended cooperative poll edge when max_no_yield_ms > 0.
inline void note_agent_progress(AgentHandle& h) noexcept {
    if (h.liveness && h.mailbox == nullptr && h.keepalive_interval_ms > 0) {
        const auto t = orch_now_us();
        h.liveness->last_keepalive_us.store(t, std::memory_order_release);
        g_orch_module_stats.last_keepalive_us.store(t, std::memory_order_relaxed);
    }
    // Issue #2540: recommended yield point for long-running bodies.
    (void)agent_poll(h);
}

// Note (Issue #1966): no multi-agent public API here.
//   - Batch parallel work: serve::parallel_orch::parallel_intend (optionally
//     bump g_orch_module_stats.parallel_batches at the call site).
//   - Name→handle bookkeeping for Aura orch:spawn-agent / orch:agent-join
//     lives in evaluator_primitives_agent.cpp (file-local table).

} // namespace aura::orch

#endif // AURA_ORCH_AGENT_SPAWN_H
