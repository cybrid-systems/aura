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
// Issue #2158: per-Evaluator agent apply mutex (replace process-static orch_eval_mu).
inline constexpr int kAgentApplyPerEvalMutexIssue = 2158;
// Issue #2155: quota-reject spawn path — no name-table put, no arena leak.
inline constexpr int kSpawnQuotaNoLeakIssue = 2155;
// Default secondary drain window after request_cancel (#2082 preserved).
inline constexpr std::uint64_t kDefaultJoinDrainMs = 2000;

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
    std::atomic<std::uint64_t> send_closed_total{0};
    std::atomic<std::uint64_t> recv_empty_total{0};
    std::atomic<std::uint64_t> join_wait_us_total{0};
    std::atomic<std::uint64_t> join_ok_total{0};
    std::atomic<std::uint64_t> join_fail_total{0};
    // Issue #2153: secondary drain after cancel on non-Ok join.
    // residual = fiber still !is_done() after drain window (cancelled-leaked).
    std::atomic<std::uint64_t> join_drain_residual_total{0};
    std::atomic<std::uint64_t> join_drain_us_total{0};
    // Issue #2008: keepalive / liveness.
    std::atomic<std::uint64_t> keepalive_emitted_total{0};
    std::atomic<std::uint64_t> stalled_agents_total{0};
    std::atomic<std::uint64_t> last_keepalive_us{0}; // process-wide most recent emit
    std::atomic<std::uint64_t> keepalive_cancels_total{0};
    std::atomic<std::uint64_t> keepalive_helpers_spawned{0};
    std::atomic<std::uint64_t> keepalive_helper_spawn_fail{0};
    // Issue #2158: per-Evaluator agent_apply_mu_ acquire accounting.
    // wait_us includes uncontended lock time (usually ~0) + contention wait.
    std::atomic<std::uint64_t> agent_apply_lock_acquisitions_total{0};
    std::atomic<std::uint64_t> agent_apply_lock_wait_us_total{0};
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
    // Issue #2008: keepalive / liveness (null / 0 when disabled — zero cost).
    std::uint32_t keepalive_interval_ms = 0;
    std::shared_ptr<AgentLiveness> liveness; // shared body ↔ helper ↔ supervisor
    // True when a detached host keepalive thread was started for this agent.
    bool keepalive_active = false;

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
        , keepalive_active(o.keepalive_active) {
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
        }
        return *this;
    }

    ~AgentHandle() {
        // Best-effort stop of keepalive helper; body_done not set here so a
        // still-running body is not misreported as Done by watch_agent_liveness.
        if (liveness)
            liveness->helper_stop.store(true, std::memory_order_release);
        keepalive_active = false;
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
    // Issue #2008: 0 = disabled (default, zero-cost). When > 0 and mailbox
    // attached, a helper fiber emits "keepalive:<us>" at this cadence.
    std::uint32_t keepalive_interval_ms = 0;
    // Issue #2118: when true (default), agent fiber body soft-registers
    // MutationBoundary depth on the per-fiber stack so steal (#2115) and
    // GC see the mutation window. Set false for pure-reasoning agents
    // that never mutate (AC2 zero-cost path — quota check only).
    bool mutation_boundary = true;
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

    // Issue #2008 / #2080: keepalive uses a host thread (not an extra fiber)
    // when attach_mailbox is set; otherwise (attach_mailbox=#f + interval > 0)
    // we run a zero-host-thread ProgressClock mode that the body entry
    // initializes and `orch:agent-touch` keeps fresh.
    const bool want_keepalive = spec.keepalive_interval_ms > 0 && spec.attach_mailbox;
    const bool want_progress_clock = spec.keepalive_interval_ms > 0 && !spec.attach_mailbox;

    // Issue #1880: fiber capacity preflight (check only; Scheduler::spawn also consumes).
    if (auto ferr = pq.check_orchestration_fibers(/*amount=*/1)) {
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

    const bool register_soft = spec.mutation_boundary;
    serve::Fiber* f = sched.spawn([body = std::move(body), mb, attach, live,
                                   progress_clock = want_progress_clock, register_soft]() mutable {
        if (attach && mb && serve::g_current_fiber)
            mb->attach(serve::g_current_fiber);
        // Issue #2080: ProgressClock mode — seed last_keepalive_us at body
        // entry so watch_agent_liveness has a baseline even if the body
        // never calls `orch:agent-touch`. MailboxKeepalive mode seeds the
        // same clock from emit_keepalive (#2008 host helper).
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

// Issue #1880 / #2009: release spawn-time memory reservation (idempotent,
// zero-cost after first call). Safe under concurrent join + scope exit:
// first caller zeros reserved_memory_bytes; second is a no-op.
inline void release_agent_memory_reservation(AgentHandle& h) noexcept {
    h.release_reservation_if_any();
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

// Issue #2153: request_cancel + optional secondary join; bump residual if
// the body is still live after the drain window (cooperative cancel only).
// drain_ms=0 → cancel only (no secondary wait). Never runs on Ok path.
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
    if (!f->is_done())
        g_orch_module_stats.join_drain_residual_total.fetch_add(1, std::memory_order_relaxed);
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
    if (residual > 0)
        g_orch_module_stats.join_drain_residual_total.fetch_add(residual,
                                                                std::memory_order_relaxed);
}

// Join a single agent (Fiber::join) + Issue #1879 post-join provenance.
// Issue #2008: signals the detached keepalive host thread to exit.
// Issue #2153: JoinPolicy controls primary timeout + secondary drain_ms.
[[nodiscard]] inline serve::JoinResult join_agent(AgentHandle& h, JoinPolicy policy) {
    if (!h.ok || !h.fiber) {
        serve::JoinResult r;
        r.status = serve::JoinStatus::Invalid;
        return r;
    }
    // Issue #2008: stop host keepalive first (non-blocking signal).
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
// Issue #2008: stop keepalive helpers, then join bodies.
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
// mode is unchanged — the host helper thread owns the clock.
inline void note_agent_progress(AgentHandle& h) noexcept {
    if (h.liveness && h.mailbox == nullptr && h.keepalive_interval_ms > 0) {
        const auto t = orch_now_us();
        h.liveness->last_keepalive_us.store(t, std::memory_order_release);
        g_orch_module_stats.last_keepalive_us.store(t, std::memory_order_relaxed);
    }
}

// Note (Issue #1966): no multi-agent public API here.
//   - Batch parallel work: serve::parallel_orch::parallel_intend (optionally
//     bump g_orch_module_stats.parallel_batches at the call site).
//   - Name→handle bookkeeping for Aura orch:spawn-agent / orch:agent-join
//     lives in evaluator_primitives_agent.cpp (file-local table).

} // namespace aura::orch

#endif // AURA_ORCH_AGENT_SPAWN_H
