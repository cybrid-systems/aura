// serve/scheduler.h — Fiber scheduler with epoll event loop
#ifndef AURA_SERVE_SCHEDULER_H
#define AURA_SERVE_SCHEDULER_H

#include "fiber.h"
#include <ucontext.h>
#include <deque>
#include <unordered_map>
#include <memory>
#include <vector>

namespace aura::serve {

// ── Scheduler — cooperative fiber scheduler ────────────
// Drives fibers with round-robin + epoll-based wakeup.
// When all fibers are Waiting, blocks on epoll_wait.
class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    // Create a new fiber
    Fiber* spawn(Fiber::Func func, size_t stack_size = 2 * 1024 * 1024);

    // Run the event loop until all fibers are done
    void run();

    // Stop the scheduler (from signal handler or another thread)
    void stop() { running_ = false; }

    // Register a fiber's eventfd with epoll (called after fiber creation)
    void register_fiber_event(Fiber* fiber);

    // Accessors
    ucontext_t& main_ctx() { return main_ctx_; }

private:
    // Ready queue: fibers that can be resumed
    std::deque<Fiber*> ready_queue_;

    // All fibers (keeps them alive)
    std::vector<std::unique_ptr<Fiber>> fibers_;

    // eventfd → Fiber mapping (for epoll dispatch)
    std::unordered_map<int, Fiber*> wait_map_;

    // Epoll instance
    int epoll_fd_ = -1;

    // Stdin fd
    int stdin_fd_ = -1;

    // Scheduler's own context (fibers yield back to this)
    ucontext_t main_ctx_;

    // Runtime flag
    bool running_ = true;

    // Add a fiber to ready queue
    void enqueue(Fiber* f);
};

} // namespace aura::serve

#endif // AURA_SERVE_SCHEDULER_H
