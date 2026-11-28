// serve/mailbox.h — Fiber-aware mailbox for inter-session messaging
#ifndef AURA_SERVE_MAILBOX_H
#define AURA_SERVE_MAILBOX_H

#include "fiber.h"
#include <string>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <unistd.h>
#include <poll.h>
#include <cstdint>

namespace aura::serve {

// ── Mailbox — per-session message queue ────────────────
// When recv is called on an empty mailbox, the calling fiber
// yields. When send pushes a message, the target fiber is
// woken via eventfd.
class Mailbox {
public:
    // Attach to an owning fiber. Must be called before pop(wait=true).
    void attach(Fiber* owner) { owner_ = owner; }

    // Push a message and wake the waiting fiber (if any).
    // Issue #5507: session-send pushes from another session's worker while
    // the owner pops; the deque needs the lock (the wake stays outside it).
    void push(std::string msg) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.push_back(std::move(msg));
        }
        notify_owner();
    }

    // Try to pop a message.
    // wait=false: non-blocking, returns empty if no message.
    // wait=true: blocking, yields fiber until message arrives.
    std::string pop(bool wait = false) { return pop_impl(wait, -1); }

    // Pop with optional timeout_ms (>0). Returns empty string on timeout.
    // timeout_ms = -1 means wait forever (if wait=true).
    std::string pop(bool wait, int timeout_ms) { return pop_impl(wait, timeout_ms); }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return queue_.size();
    }
    bool empty() const {
        std::lock_guard<std::mutex> lk(mu_);
        return queue_.empty();
    }

private:
    mutable std::mutex mu_;
    std::deque<std::string> queue_;
    Fiber* owner_ = nullptr;

    std::optional<std::string> take_locked_() {
        std::lock_guard<std::mutex> lk(mu_);
        if (queue_.empty())
            return std::nullopt;
        auto msg = std::move(queue_.front());
        queue_.pop_front();
        return msg;
    }

    std::string pop_impl(bool wait, int timeout_ms) {
        if (auto got = take_locked_())
            return std::move(*got);
        if (wait && g_current_fiber && owner_) {
            auto evfd = owner_->eventfd();
            if (evfd >= 0 && timeout_ms != 0) {
                // Use poll with timeout on the eventfd
                struct pollfd pfd;
                pfd.fd = evfd;
                pfd.events = POLLIN;
                int pret = ::poll(&pfd, 1, timeout_ms);
                if (pret > 0) {
                    // Drain the eventfd (read the 8-byte counter)
                    // This race is safe: if the scheduler also reads it,
                    // we get EAGAIN but the queue has the message.
                    uint64_t val = 0;
                    auto rret = ::read(evfd, &val, sizeof(val));
                    (void)rret;
                } else if (pret == 0) {
                    // Timeout
                    return {};
                }
                // Check queue after poll
                if (auto got = take_locked_())
                    return std::move(*got);
            }
            // Fallback: yield and wait (for scheduler-based wake)
            g_current_fiber->set_state(FiberState::Waiting);
            Fiber::yield();
            if (auto got = take_locked_())
                return std::move(*got);
        }
        return {};
    }

    void notify_owner() {
        if (owner_) {
            uint64_t val = 1;
            ::write(owner_->eventfd(), &val, sizeof(val));
        }
    }
};

} // namespace aura::serve

#endif // AURA_SERVE_MAILBOX_H
