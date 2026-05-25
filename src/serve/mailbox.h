// serve/mailbox.h — Fiber-aware mailbox for inter-session messaging
#ifndef AURA_SERVE_MAILBOX_H
#define AURA_SERVE_MAILBOX_H

#include "fiber.h"
#include <string>
#include <deque>
#include <utility>
#include <unistd.h>

namespace aura::serve {

// ── Mailbox — per-session message queue ────────────────
// When recv is called on an empty mailbox, the calling fiber
// yields. When send pushes a message, the target fiber is
// woken via eventfd.
class Mailbox {
public:
    Mailbox() = default;

    // Push a message and wake the waiting fiber (if any)
    void push(const std::string& msg) {
        queue_.push_back(msg);
        notify_owner();
    }

    // Try to pop a message. If wait=true and empty, yields the fiber.
    // Returns empty string on timeout / no message.
    std::string pop(bool wait = false) {
        if (!queue_.empty()) {
            auto msg = std::move(queue_.front());
            queue_.pop_front();
            return msg;
        }
        if (wait && g_current_fiber) {
            // Register as waiting and yield
            g_current_fiber->set_state(FiberState::Waiting);
            // The fiber's eventfd is already registered with epoll.
            // When push() writes to eventfd, the scheduler will wake us.
            Fiber::yield();
            // Resumed: try again
            if (!queue_.empty()) {
                auto msg = std::move(queue_.front());
                queue_.pop_front();
                return msg;
            }
        }
        return {};
    }

    size_t size() const { return queue_.size(); }
    bool empty() const { return queue_.empty(); }

private:
    std::deque<std::string> queue_;

    // Wake the fiber waiting on this mailbox (if any)
    void notify_owner() {
        // The caller (session fiber) should have set itself as current
        // The eventfd mechanism handles wakeup through the scheduler
        if (g_current_fiber) {
            // Write to eventfd to wake through epoll
            uint64_t val = 1;
            ::write(g_current_fiber->eventfd(), &val, sizeof(val));
        }
    }
};

} // namespace aura::serve

#endif // AURA_SERVE_MAILBOX_H
