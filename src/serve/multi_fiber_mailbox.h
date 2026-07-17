// multi_fiber_mailbox.h — Issue #1585 / #1211: MultiFiberMailbox with
// multi-attach, broadcast, blocking recv, priority, and backpressure.
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
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace aura::serve::mf_mailbox {

inline constexpr int kMultiFiberMailboxPhase = 2; // #1585 production
inline constexpr int kMultiFiberMailboxIssue = 1585;

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
};

// Process-wide aggregate (tests / observability).
inline MultiFiberMailboxStats g_mf_mailbox_stats{};

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
    [[nodiscard]] PushStatus push(MailMessage msg) {
        std::lock_guard lock(mu_);
        if (closed_.load(std::memory_order_relaxed))
            return PushStatus::Closed;
        if (queue_.size() >= high_water_) {
            g_mf_mailbox_stats.backpressure_rejects.fetch_add(1, std::memory_order_relaxed);
            return PushStatus::Backpressure;
        }
        g_mf_mailbox_stats.pushes.fetch_add(1, std::memory_order_relaxed);
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
    [[nodiscard]] PushStatus broadcast(MailMessage msg) {
        msg.to_fiber = 0;
        return push(std::move(msg));
    }

    // Fan-out: enqueue one message copy per attached fiber (to_fiber = fiber id).
    // Returns Backpressure if any push would overflow (none applied).
    [[nodiscard]] PushStatus broadcast_fanout(const MailMessage& proto) {
        std::lock_guard lock(mu_);
        if (closed_.load(std::memory_order_relaxed))
            return PushStatus::Closed;
        const auto need = attachers_.empty() ? std::size_t{1} : attachers_.size();
        if (queue_.size() + need > high_water_) {
            g_mf_mailbox_stats.backpressure_rejects.fetch_add(1, std::memory_order_relaxed);
            return PushStatus::Backpressure;
        }
        g_mf_mailbox_stats.broadcasts.fetch_add(1, std::memory_order_relaxed);
        g_mf_mailbox_stats.pushes.fetch_add(need, std::memory_order_relaxed);
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
