// serve/scheduler.h — Multi-threaded fiber scheduler
#ifndef AURA_SERVE_SCHEDULER_H
#define AURA_SERVE_SCHEDULER_H

#include "fiber.h"
#include "worker.h"
#include <ucontext.h>
#include <deque>
#include <unordered_map>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>

namespace aura::serve {

// ── Scheduler — M:N fiber scheduler ────────────────────
//
// Manages N WorkerThreads and an IO event loop.
// Fibers are distributed across workers for parallel execution.
//
// Architecture:
//   - IO thread (main thread): epoll_wait for eventfd/stdin events,
//     dispatches woken fibers to workers
//   - Worker threads: run fibers from local queues
//   - Fiber lifecycle:         spawn → [Ready → Running → yield → Ready|Waiting → Done]
//   - Event-driven wakeup:     Waiting fiber's eventfd fires → IO thread enqueues to worker
class Scheduler {
public:
    explicit Scheduler(int num_workers = 0);
    ~Scheduler();

    // Non-copyable
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    // Create a new fiber. The fiber is assigned to a worker
    // (round-robin across available workers).
    Fiber* spawn(Fiber::Func func, size_t stack_size = 2 * 1024 * 1024);

    // Run the event loop until all fibers are done.
    // This is called on the main thread (IO thread).
    void run();

    // Stop all workers and the event loop
    void stop();

    // ── Event management ────────────────────────────
    // Register a fiber's eventfd with the epoll instance.
    // Called during fiber spawn or session setup.
    // Thread-safe.
    void register_event_fiber(int eventfd, Fiber* fiber);

    // Remove a fiber from the event map.
    // Called by worker when fiber is Done.
    // Thread-safe.
    void unregister_fiber(int eventfd);

    // ── Callbacks for workers ───────────────────────
    // Notify scheduler that a fiber is done.
    // Thread-safe.
    void on_fiber_done(Fiber* fiber);

    // Check if there are any fibers in Waiting state (on epoll).
    bool has_waiting_fibers() const;

    // ── Worker management ───────────────────────────
    int num_workers() const { return num_workers_; }
    WorkerThread* worker(int idx);
    int next_worker_id();  // round-robin assignment

    // ── Stdin fiber ─────────────────────────────────
    void set_stdin_fiber(Fiber* f) { stdin_fiber_ = f; }
    Fiber* stdin_fiber() const { return stdin_fiber_; }

    // ── Accessors ───────────────────────────────────
    int epoll_fd() const { return epoll_fd_; }

private:
    int num_workers_;
    std::vector<std::unique_ptr<WorkerThread>> workers_;
    std::atomic<int> next_worker_{0};  // round-robin counter

    // IO thread resources
    int epoll_fd_ = -1;
    int stdin_fd_ = -1;

    // eventfd → Fiber mapping (protected by mutex)
    std::unordered_map<int, Fiber*> wait_map_;
    mutable std::mutex wait_map_mutex_;

    // Stdin fiber (handles stdin line protocol in serve mode)
    Fiber* stdin_fiber_ = nullptr;

    // Runtime flag
    std::atomic<bool> running_{true};
};

} // namespace aura::serve

#endif // AURA_SERVE_SCHEDULER_H
