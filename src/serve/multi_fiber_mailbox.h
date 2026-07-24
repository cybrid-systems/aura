// multi_fiber_mailbox.h — Issue #1585 / #1211 / #1595: MultiFiberMailbox with
// multi-attach, broadcast, blocking recv, priority, and backpressure.
// #1595: linear-claim payload prefix filter (linear-viol:) + process counters.
// #1881: fanout linear_checks + local push stats.
// #2010: shared linear filter on all entry points; fanout backpressure
//        observability (+ orch hook for dashboards).
// Header form (like mailbox.h) so serve + tests can include without module churn.

#ifndef AURA_SERVE_MULTI_FIBER_MAILBOX_H
#define AURA_SERVE_MULTI_FIBER_MAILBOX_H

#include "fiber.h"

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
};

// Process-wide aggregate (tests / observability).
inline MultiFiberMailboxStats g_mf_mailbox_stats{};

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
        std::lock_guard lock(mu_);
        attachers_.erase(std::remove(attachers_.begin(), attachers_.end(), f), attachers_.end());
    }

    [[nodiscard]] std::size_t attacher_count() const {
        std::lock_guard lock(mu_);
        return attachers_.size();
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mu_);
        return queue_.size();
    }
    [[nodiscard]] bool empty() const {
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
        std::lock_guard lock(mu_);
        if (closed_.load(std::memory_order_relaxed))
            return PushStatus::Closed;
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
    static void snapshot_global_full(std::uint64_t& pushes, std::uint64_t& pops,
                                     std::uint64_t& broadcasts, std::uint64_t& bp,
                                     std::uint64_t& attaches, std::uint64_t& priority_high,
                                     std::uint64_t& recv_waits, std::uint64_t& recv_timeouts,
                                     std::uint64_t& linear_checks, std::uint64_t& linear_violations,
                                     std::uint64_t* fanout_bp = nullptr) noexcept {
        snapshot_global(pushes, pops, broadcasts, bp, attaches);
        priority_high = g_mf_mailbox_stats.priority_high.load(std::memory_order_relaxed);
        recv_waits = g_mf_mailbox_stats.recv_waits.load(std::memory_order_relaxed);
        recv_timeouts = g_mf_mailbox_stats.recv_timeouts.load(std::memory_order_relaxed);
        linear_checks = g_mf_mailbox_stats.linear_checks.load(std::memory_order_relaxed);
        linear_violations = g_mf_mailbox_stats.linear_violations.load(std::memory_order_relaxed);
        if (fanout_bp)
            *fanout_bp =
                g_mf_mailbox_stats.fanout_backpressure_rejects.load(std::memory_order_relaxed);
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
