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
    // Attach to an owning fiber. Must be called before pop(wait=true).
    void attach(Fiber* owner) { owner_ = owner; }

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
            g_current_fiber->set_state(FiberState::Waiting);
            Fiber::yield();
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
    Fiber* owner_ = nullptr;

    void notify_owner() {
        if (owner_) {
            uint64_t val = 1;
            ::write(owner_->eventfd(), &val, sizeof(val));
        }
    }
};

} // namespace aura::serve

#endif // AURA_SERVE_MAILBOX_H
