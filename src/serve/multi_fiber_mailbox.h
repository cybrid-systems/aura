// multi_fiber_mailbox.h — Issue #1585 / #1211 / #1595 / #2312 / #2316:
// MultiFiberMailbox with
// multi-attach, broadcast, blocking recv, priority, and backpressure.
// #1595: linear-claim payload prefix filter (linear-viol:) + process counters.
// #1881: fanout linear_checks + local push stats.
// #2010: shared linear filter on all entry points; fanout backpressure
//        observability (+ orch hook for dashboards).
// #2188: forbid blocking recv / Fiber::yield while MutationBoundary is live
//        (depth>0 or held) — Policy A: non-blocking empty + metric, no park.
// #2347: Guard-live blocking recv → hard audit under Strict/production.
//        Agent contract: **Guard 内禁止 blocking recv**; use try_recv /
//        recv(wait=false) or exit MutationBoundary first. Policy A stays
//        non-blocking; Strict bumps hard counter and may force-rollback
//        after N rejects in one outermost Guard window.
// #2316: wire mu_ acquire to lock_order::on_acquire(Level::Mailbox) for
//        rank-table audit + AURA_LOCK_ORDER_CANARY inversion detection.
// Header form (like mailbox.h) so serve + tests can include without module churn.

#ifndef AURA_SERVE_MULTI_FIBER_MAILBOX_H
#define AURA_SERVE_MULTI_FIBER_MAILBOX_H

#include "fiber.h"
#include "compiler/lock_order_audit.h" // Issue #2316: lock-order audit

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

// Issue #2010: optional orch mirror for mailbox backpressure (weak no-op when
// orch is not linked; strong def bumps OrchModuleStats::send_backpressure_total).
extern "C" void aura_orch_note_mailbox_backpressure();
// Issue #2347: force outermost mutation success_flag=false (strong def in
// evaluator_fiber_mutation.cpp; weak no-op when Evaluator not linked).
extern "C" void aura_evaluator_mark_outermost_mutation_failed() noexcept;
// Issue #2346 / #2347: production canary probe (strong in audit hooks).
extern "C" int aura_production_defaults_active_probe() noexcept;

namespace aura::serve::mf_mailbox {

inline constexpr int kMultiFiberMailboxPhase = 3; // #1881 observability
inline constexpr int kMultiFiberMailboxIssue = 1881;

// Issue #1595 / #2010: provenance-safety prefix (fiber-stack safe, pure string).
inline constexpr std::string_view kLinearViolPrefix = "linear-viol:";

enum class MailPriority : std::uint8_t { Low = 0, Normal = 1, High = 2, Critical = 3 };

enum class PushStatus : std::uint8_t {
    Ok = 0,
    Backpressure = 1, // queue at high-water mark
    Closed = 2,
};

struct MailMessage {
    std::uint64_t from_fiber = 0;
    std::uint64_t to_fiber = 0; // 0 = broadcast / any
    MailPriority priority = MailPriority::Normal;
    std::string payload;
};

struct MultiFiberMailboxStats {
    std::atomic<std::uint64_t> pushes{0};
    std::atomic<std::uint64_t> pops{0};
    std::atomic<std::uint64_t> broadcasts{0};
    std::atomic<std::uint64_t> priority_high{0};
    std::atomic<std::uint64_t> backpressure_rejects{0};
    std::atomic<std::uint64_t> attaches{0};
    std::atomic<std::uint64_t> recv_waits{0};
    std::atomic<std::uint64_t> recv_timeouts{0};
    // Issue #1595: linear claim checks / violations (prefix filter on push).
    std::atomic<std::uint64_t> linear_checks{0};
    std::atomic<std::uint64_t> linear_violations{0};
    // Issue #2010: fanout-specific backpressure (also counted in backpressure_rejects).
    std::atomic<std::uint64_t> fanout_backpressure_rejects{0};
    // Issue #2188: blocking recv refused while MutationBoundary is live
    // (depth>0 or held) — Policy A non-blocking empty return.
    std::atomic<std::uint64_t> recv_rejected_in_mutation_boundary{0};
    // Issue #2347: Strict / production hard path for the same Policy A
    // reject (Agents must poll this; Soft path leaves it at 0).
    std::atomic<std::uint64_t> recv_rejected_in_mutation_boundary_hard_total{0};
    // Issue #2347: times window threshold forced mutation mark-failed.
    std::atomic<std::uint64_t> recv_boundary_force_rollback_total{0};
    // Issue #2312: delivery gate — bump when push / broadcast_fanout
    // observes the target fiber(s) holding a live MutationBoundary
    // (depth>0 or held) and defers (Backpressure) rather than risking
    // AST writes interleaving with foreign delivery under the same
    // workspace_mtx_ exclusive + GcDeferReason::MutationHold arm.
    // Reuse the #2184 MutationSafetySnapshot + is_at_mutation_boundary_safe
    // truth table. Distinct from recv_rejected_in_mutation_boundary which
    // is the recv-side gate (#2188) — this is the push-side gate.
    std::atomic<std::uint64_t> mailbox_deferred_mutation_hold_total{0}; // #2312
};

// Process-wide aggregate (tests / observability).
inline MultiFiberMailboxStats g_mf_mailbox_stats{};

// Issue #2347: rejects in the current outermost Guard window (TLS).
// Cleared on outermost Guard exit so multi-round mutates do not carry
// a stale window. Soft path still increments this for optional dashboards.
inline thread_local std::uint64_t g_recv_boundary_reject_window{0};

inline void clear_recv_boundary_reject_window() noexcept {
    g_recv_boundary_reject_window = 0;
}

// Issue #2347: Strict / production opt-in for hard audit + threshold.
// Soft (default): Policy A soft counter only.
// Strict: AURA_MUTATE_MAILBOX_STRICT=1 OR production_defaults canary.
[[nodiscard]] inline bool is_mutate_mailbox_strict() noexcept {
    const char* e = std::getenv("AURA_MUTATE_MAILBOX_STRICT");
    if (e && e[0] == '1')
        return true;
    return aura_production_defaults_active_probe() != 0;
}

// Default N=8 rejects in one Guard window → force mark-failed under Strict.
// Override: AURA_MUTATE_MAILBOX_REJECT_THRESHOLD=<N> (0 disables threshold).
[[nodiscard]] inline std::uint64_t mutate_mailbox_reject_threshold() noexcept {
    const char* e = std::getenv("AURA_MUTATE_MAILBOX_REJECT_THRESHOLD");
    if (e == nullptr || e[0] == '\0')
        return 8;
    std::uint64_t v = 0;
    for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
        v = v * 10 + static_cast<std::uint64_t>(*p - '0');
    return v; // 0 = disable force-rollback threshold
}

// ── Hot-path helpers (no Evaluator / GC / provenance) ──
[[nodiscard]] inline bool is_linear_viol_payload(std::string_view payload) noexcept {
    return payload.size() >= kLinearViolPrefix.size() &&
           payload.compare(0, kLinearViolPrefix.size(), kLinearViolPrefix) == 0;
}

// Bump linear_checks; if payload is linear-viol:, bump violations and return true
// (caller should return Closed without locking).
[[nodiscard]] inline bool reject_if_linear_viol(std::string_view payload) noexcept {
    g_mf_mailbox_stats.linear_checks.fetch_add(1, std::memory_order_relaxed);
    if (!is_linear_viol_payload(payload))
        return false;
    g_mf_mailbox_stats.linear_violations.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Backpressure accounting: process + local + optional orch dashboard mirror.
inline void note_backpressure(MultiFiberMailboxStats* local = nullptr,
                              bool from_fanout = false) noexcept {
    g_mf_mailbox_stats.backpressure_rejects.fetch_add(1, std::memory_order_relaxed);
    if (local)
        local->backpressure_rejects.fetch_add(1, std::memory_order_relaxed);
    if (from_fanout) {
        g_mf_mailbox_stats.fanout_backpressure_rejects.fetch_add(1, std::memory_order_relaxed);
        if (local)
            local->fanout_backpressure_rejects.fetch_add(1, std::memory_order_relaxed);
    }
    aura_orch_note_mailbox_backpressure();
}

// Multi-fiber mailbox: many attachers, priority queue, broadcast wake,
// high-water backpressure.
class MultiFiberMailbox {
public:
    explicit MultiFiberMailbox(std::size_t high_water = 1024) noexcept
        : high_water_(high_water == 0 ? 1 : high_water) {}

    void set_high_water(std::size_t n) noexcept { high_water_ = n == 0 ? 1 : n; }
    [[nodiscard]] std::size_t high_water() const noexcept { return high_water_; }
    [[nodiscard]] bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }
    void close() noexcept {
        closed_.store(true, std::memory_order_release);
        notify_all_locked();
    }

    // Multi-attach: multiple fibers may wait on this mailbox.
    // priority is reserved for future fair scheduling (stored for stats).
    void attach(Fiber* f, int /*priority*/ = 0) {
        if (!f)
            return;
        // Issue #2316: wire mu_ acquire to lock_order audit.
        (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::Mailbox,
                                                       __builtin_FILE(), __builtin_LINE());
        std::lock_guard lock(mu_);
        for (auto* a : attachers_) {
            if (a == f)
                return;
        }
        attachers_.push_back(f);
        g_mf_mailbox_stats.attaches.fetch_add(1, std::memory_order_relaxed);
    }

    void detach(Fiber* f) {
        if (!f)
            return;
        (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::Mailbox,
                                                       __builtin_FILE(), __builtin_LINE());
        std::lock_guard lock(mu_);
        attachers_.erase(std::remove(attachers_.begin(), attachers_.end(), f), attachers_.end());
    }

    [[nodiscard]] std::size_t attacher_count() const {
        (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::Mailbox,
                                                       __builtin_FILE(), __builtin_LINE());
        std::lock_guard lock(mu_);
        return attachers_.size();
    }

    [[nodiscard]] std::size_t size() const {
        (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::Mailbox,
                                                       __builtin_FILE(), __builtin_LINE());
        std::lock_guard lock(mu_);
        return queue_.size();
    }
    [[nodiscard]] bool empty() const {
        (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::Mailbox,
                                                       __builtin_FILE(), __builtin_LINE());
        std::lock_guard lock(mu_);
        return queue_.empty();
    }

    // Push with backpressure: when size >= high_water, reject.
    // Issue #1595 / #2010: linear-viol: filter runs first (before lock), pure
    // string prefix — fiber-stack safe, no Evaluator/GC/provenance on hot path.
    // Deeper StableNodeRef/linear probe is via host/post-join paths only.
    [[nodiscard]] PushStatus push(MailMessage msg) {
        if (reject_if_linear_viol(msg.payload))
            return PushStatus::Closed;
        (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::Mailbox,
                                                       __builtin_FILE(), __builtin_LINE());
        std::lock_guard lock(mu_);
        if (closed_.load(std::memory_order_relaxed))
            return PushStatus::Closed;
        // Issue #2312: delivery gate — when msg.to_fiber resolves to an
        // attached fiber, gate on its MutationSafetySnapshot. If the
        // target is NOT at a mutation-boundary-safe point, defer (BP)
        // rather than delivering mutate-triggering work to a fiber
        // holding Guard (lock-order inversion vs workspace_mtx_,
        // silent partial updates under multi-agent fanout). Reuses
        // #2184 snapshot + is_at_mutation_boundary_safe truth table.
        // Per AC3: zero cost when target is safe (one snapshot load).
        // Per AC2: deferred = not dropped; the sender retries / queues
        // and the target becomes deliverable after outermost Guard exit.
        if (msg.to_fiber != 0) {
            for (auto* a : attachers_) {
                if (a && a->id() == msg.to_fiber) {
                    const auto snap = a->mutation_safety_snapshot();
                    if (!a->is_at_mutation_boundary_safe(snap)) {
                        g_mf_mailbox_stats.mailbox_deferred_mutation_hold_total.fetch_add(
                            1, std::memory_order_relaxed);
                        local_stats_.mailbox_deferred_mutation_hold_total.fetch_add(
                            1, std::memory_order_relaxed);
                        return PushStatus::Backpressure;
                    }
                    break;
                }
            }
        }
        if (queue_.size() >= high_water_) {
            note_backpressure(&local_stats_, /*from_fanout=*/false);
            return PushStatus::Backpressure;
        }
        g_mf_mailbox_stats.pushes.fetch_add(1, std::memory_order_relaxed);
        local_stats_.pushes.fetch_add(1, std::memory_order_relaxed); // #1881: was dead
        if (msg.priority >= MailPriority::High)
            g_mf_mailbox_stats.priority_high.fetch_add(1, std::memory_order_relaxed);
        if (msg.to_fiber == 0)
            g_mf_mailbox_stats.broadcasts.fetch_add(1, std::memory_order_relaxed);
        // Critical/High go front; others back (stable within band).
        if (msg.priority >= MailPriority::High)
            queue_.push_front(std::move(msg));
        else
            queue_.push_back(std::move(msg));
        notify_all_unlocked();
        return PushStatus::Ok;
    }

    // Broadcast: enqueue a copy for routing tag to_fiber=0 and wake all attachers.
    // Still a single queue message; all waiters compete via priority pop.
    // Semantics: one message, all waiters woken (first recv wins unless fan-out).
    // For true per-fiber fan-out, use broadcast_fanout.
    // Linear filter runs via push (shared entry-point guarantee, #2010).
    [[nodiscard]] PushStatus broadcast(MailMessage msg) {
        msg.to_fiber = 0;
        return push(std::move(msg));
    }

    // Fan-out: enqueue one message copy per attached fiber (to_fiber = fiber id).
    // Returns Backpressure if any push would overflow (none applied).
    // Issue #1881 / #2010: linear filter first; fanout BP mirrored to orch.
    [[nodiscard]] PushStatus broadcast_fanout(const MailMessage& proto) {
        if (reject_if_linear_viol(proto.payload))
            return PushStatus::Closed;
        std::lock_guard lock(mu_);
        if (closed_.load(std::memory_order_relaxed))
            return PushStatus::Closed;
        // Issue #2312: per-attached-fiber delivery gate. If ANY attached
        // fiber is unsafe (MutationBoundary held / depth>0), defer the
        // entire fan-out — don't partially deliver mutate-triggering
        // work to a subset while another target holds Guard. Mirrors
        // the per-to_fiber gate in push(). Reuses #2184 snapshot.
        for (auto* a : attachers_) {
            if (!a)
                continue;
            const auto snap = a->mutation_safety_snapshot();
            if (!a->is_at_mutation_boundary_safe(snap)) {
                g_mf_mailbox_stats.mailbox_deferred_mutation_hold_total.fetch_add(
                    1, std::memory_order_relaxed);
                local_stats_.mailbox_deferred_mutation_hold_total.fetch_add(
                    1, std::memory_order_relaxed);
                note_backpressure(&local_stats_, /*from_fanout=*/true);
                return PushStatus::Backpressure;
            }
        }
        const auto need = attachers_.empty() ? std::size_t{1} : attachers_.size();
        if (queue_.size() + need > high_water_) {
            note_backpressure(&local_stats_, /*from_fanout=*/true);
            return PushStatus::Backpressure;
        }
        g_mf_mailbox_stats.broadcasts.fetch_add(1, std::memory_order_relaxed);
        g_mf_mailbox_stats.pushes.fetch_add(need, std::memory_order_relaxed);
        local_stats_.pushes.fetch_add(need, std::memory_order_relaxed);
        if (proto.priority >= MailPriority::High)
            g_mf_mailbox_stats.priority_high.fetch_add(need, std::memory_order_relaxed);

        if (attachers_.empty()) {
            MailMessage m = proto;
            m.to_fiber = 0;
            if (m.priority >= MailPriority::High)
                queue_.push_front(std::move(m));
            else
                queue_.push_back(std::move(m));
        } else {
            for (auto* f : attachers_) {
                MailMessage m = proto;
                m.to_fiber = f ? f->id() : 0;
                if (m.priority >= MailPriority::High)
                    queue_.push_front(std::move(m));
                else
                    queue_.push_back(std::move(m));
            }
        }
        notify_all_unlocked();
        return PushStatus::Ok;
    }

    [[nodiscard]] bool try_pop(MailMessage& out) {
        std::lock_guard lock(mu_);
        return try_pop_unlocked(out, /*for_fiber=*/0);
    }

    // Blocking recv for the current fiber (or host poll if no fiber).
    // timeout_ms < 0: wait forever; 0: try once; >0: deadline.
    // for_fiber: if non-zero, prefer messages with matching to_fiber or broadcast (0).
    //
    // Issue #2188 / #2347 (Policy A + hard audit):
    // While MutationBoundary is live (depth>0 or held), never park /
    // Fiber::yield — return empty immediately and bump soft reject counter.
    // Agent contract: **do not blocking-recv under Guard** — use try_recv /
    // recv(wait=false) or exit the boundary first (prevents livelock spin).
    // Strict / production (AURA_MUTATE_MAILBOX_STRICT=1 or production
    // canary): also bump hard counter; after N rejects in one Guard window
    // force outermost mutation mark-failed (threshold, default 8).
    // Soft / default: Policy A soft counter only (AC1). Depth==0 unchanged.
    // Gate sits next to the #2010 linear-viol pure-string filter contract
    // (both are hot-path safety fences before any blocking wait).
    // Happy path (no boundary): zero extra cost beyond existing depth/held probe.
    [[nodiscard]] std::optional<MailMessage> recv(bool wait = true, int timeout_ms = -1,
                                                  std::uint64_t for_fiber = 0) {
        const auto deadline = timeout_ms > 0 ? std::chrono::steady_clock::now() +
                                                   std::chrono::milliseconds(timeout_ms)
                                             : std::chrono::steady_clock::time_point::max();

        for (;;) {
            {
                std::lock_guard lock(mu_);
                MailMessage out;
                if (try_pop_unlocked(out, for_fiber))
                    return out;
                if (closed_.load(std::memory_order_relaxed))
                    return std::nullopt;
            }
            if (!wait || timeout_ms == 0) {
                if (timeout_ms == 0)
                    g_mf_mailbox_stats.recv_timeouts.fetch_add(1, std::memory_order_relaxed);
                return std::nullopt;
            }
            if (timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline) {
                g_mf_mailbox_stats.recv_timeouts.fetch_add(1, std::memory_order_relaxed);
                return std::nullopt;
            }

            // Issue #2188: hard gate — no yield-while-Guard (depth or held).
            // Mirrors #362 skip of mutation-boundary yield, but covers the
            // generic Explicit/BlockingIO park used by Agent message loops.
            const bool boundary_live = aura_evaluator_mutation_boundary_depth() > 0 ||
                                       aura_evaluator_mutation_boundary_held() != 0;
            if (boundary_live) {
                g_mf_mailbox_stats.recv_rejected_in_mutation_boundary.fetch_add(
                    1, std::memory_order_relaxed);
                local_stats_.recv_rejected_in_mutation_boundary.fetch_add(
                    1, std::memory_order_relaxed);
                // Issue #2347: window accumulate (all modes; Soft for dashboard).
                ++g_recv_boundary_reject_window;
                // Strict / production hard audit (AC2) + optional threshold (AC3).
                if (is_mutate_mailbox_strict()) {
                    g_mf_mailbox_stats.recv_rejected_in_mutation_boundary_hard_total.fetch_add(
                        1, std::memory_order_relaxed);
                    local_stats_.recv_rejected_in_mutation_boundary_hard_total.fetch_add(
                        1, std::memory_order_relaxed);
                    const auto thr = mutate_mailbox_reject_threshold();
                    if (thr > 0 && g_recv_boundary_reject_window >= thr) {
                        g_mf_mailbox_stats.recv_boundary_force_rollback_total.fetch_add(
                            1, std::memory_order_relaxed);
                        local_stats_.recv_boundary_force_rollback_total.fetch_add(
                            1, std::memory_order_relaxed);
                        // Prefer mark-failed over re-parking (Policy A stays).
                        aura_evaluator_mark_outermost_mutation_failed();
                    }
                }
                // Policy A: non-blocking empty (no park, no Fiber::yield).
                return std::nullopt;
            }

            g_mf_mailbox_stats.recv_waits.fetch_add(1, std::memory_order_relaxed);
            if (g_current_fiber != nullptr) {
                // Park so GC safepoint / steal can proceed.
                g_current_fiber->set_state(FiberState::Waiting);
                if (timeout_ms > 0)
                    Fiber::yield(YieldReason::Explicit);
                else
                    Fiber::yield(YieldReason::BlockingIO);
                // Drain eventfd if present.
                int evfd = g_current_fiber->eventfd();
                if (evfd >= 0) {
                    std::uint64_t val = 0;
                    while (::read(evfd, &val, sizeof(val)) > 0) {
                    }
                }
                g_current_fiber->set_state(FiberState::Running);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    // Issue #2188: Agent-facing alias for non-blocking recv (safe under Guard).
    [[nodiscard]] std::optional<MailMessage> try_recv(std::uint64_t for_fiber = 0) {
        return recv(/*wait=*/false, /*timeout_ms=*/0, for_fiber);
    }

    [[nodiscard]] const MultiFiberMailboxStats& stats() const noexcept { return local_stats_; }

    // Snapshot process-wide counters into ints for tests.
    static void snapshot_global(std::uint64_t& pushes, std::uint64_t& pops,
                                std::uint64_t& broadcasts, std::uint64_t& bp,
                                std::uint64_t& attaches) noexcept {
        pushes = g_mf_mailbox_stats.pushes.load(std::memory_order_relaxed);
        pops = g_mf_mailbox_stats.pops.load(std::memory_order_relaxed);
        broadcasts = g_mf_mailbox_stats.broadcasts.load(std::memory_order_relaxed);
        bp = g_mf_mailbox_stats.backpressure_rejects.load(std::memory_order_relaxed);
        attaches = g_mf_mailbox_stats.attaches.load(std::memory_order_relaxed);
    }

    // Issue #1881: full health snapshot (priority / waits / linear).
    // Issue #2010: optional fanout_bp out-param (pass nullptr to skip).
    // Issue #2188: optional recv_rejected_boundary out-param.
    static void snapshot_global_full(std::uint64_t& pushes, std::uint64_t& pops,
                                     std::uint64_t& broadcasts, std::uint64_t& bp,
                                     std::uint64_t& attaches, std::uint64_t& priority_high,
                                     std::uint64_t& recv_waits, std::uint64_t& recv_timeouts,
                                     std::uint64_t& linear_checks, std::uint64_t& linear_violations,
                                     std::uint64_t* fanout_bp = nullptr,
                                     std::uint64_t* recv_rejected_boundary = nullptr,
                                     std::uint64_t* deferred_mutation_hold = nullptr) noexcept {
        snapshot_global(pushes, pops, broadcasts, bp, attaches);
        priority_high = g_mf_mailbox_stats.priority_high.load(std::memory_order_relaxed);
        recv_waits = g_mf_mailbox_stats.recv_waits.load(std::memory_order_relaxed);
        recv_timeouts = g_mf_mailbox_stats.recv_timeouts.load(std::memory_order_relaxed);
        linear_checks = g_mf_mailbox_stats.linear_checks.load(std::memory_order_relaxed);
        linear_violations = g_mf_mailbox_stats.linear_violations.load(std::memory_order_relaxed);
        if (fanout_bp)
            *fanout_bp =
                g_mf_mailbox_stats.fanout_backpressure_rejects.load(std::memory_order_relaxed);
        // Issue #2347: hard / force totals available via g_mf_mailbox_stats
        // or query:mf-mailbox-stats (schema-2347); soft reject remains here.
        if (recv_rejected_boundary)
            *recv_rejected_boundary = g_mf_mailbox_stats.recv_rejected_in_mutation_boundary.load(
                std::memory_order_relaxed);
        // Issue #2312: delivery-gate counter (push-side, distinct from
        // recv_rejected_boundary which is recv-side #2188).
        if (deferred_mutation_hold)
            *deferred_mutation_hold = g_mf_mailbox_stats.mailbox_deferred_mutation_hold_total.load(
                std::memory_order_relaxed);
    }

private:
    bool try_pop_unlocked(MailMessage& out, std::uint64_t for_fiber) {
        if (queue_.empty())
            return false;
        if (for_fiber == 0) {
            out = std::move(queue_.front());
            queue_.pop_front();
            g_mf_mailbox_stats.pops.fetch_add(1, std::memory_order_relaxed);
            local_stats_.pops.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // Prefer exact match, then broadcast (to_fiber==0).
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (it->to_fiber == for_fiber || it->to_fiber == 0) {
                out = std::move(*it);
                queue_.erase(it);
                g_mf_mailbox_stats.pops.fetch_add(1, std::memory_order_relaxed);
                local_stats_.pops.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    void notify_all_unlocked() {
        for (auto* f : attachers_) {
            if (!f)
                continue;
            int evfd = f->eventfd();
            if (evfd >= 0) {
                std::uint64_t one = 1;
                (void)::write(evfd, &one, sizeof(one));
            }
            // If Waiting, mark Ready so scheduler may pick it up.
            if (f->state() == FiberState::Waiting)
                f->set_state(FiberState::Ready);
        }
    }

    void notify_all_locked() {
        std::lock_guard lock(mu_);
        notify_all_unlocked();
    }

    mutable std::mutex mu_;
    std::deque<MailMessage> queue_;
    std::vector<Fiber*> attachers_;
    std::size_t high_water_ = 1024;
    std::atomic<bool> closed_{false};
    MultiFiberMailboxStats local_stats_{};
};

} // namespace aura::serve::mf_mailbox

#endif // AURA_SERVE_MULTI_FIBER_MAILBOX_H
