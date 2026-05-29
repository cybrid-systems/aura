// serve/worker.h — Worker thread with work-stealing queue
#ifndef AURA_SERVE_WORKER_H
#define AURA_SERVE_WORKER_H

#include "fiber.h"
#include "ws_deque.h"
#include <ucontext.h>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <atomic>

namespace aura::serve {

// ── Forward declarations ──────────────────────────────
class Scheduler;

// ── StealBudget — adaptive steal control ──────────────
// Tracks how many consecutive steal attempts failed,
// so workers don't burn CPU spinning on empty queues.
struct StealBudget {
    int consecutive_failures = 0;
    int max_before_sleep = 3;  // after 3 failures, go to sleep

    bool should_steal() const {
        return consecutive_failures < max_before_sleep;
    }

    void record_success() { consecutive_failures = 0; }
    void record_failure() { ++consecutive_failures; }
};

// ── WorkerThread — per-OS-thread fiber runner ──────────
//
// Each worker has:
//   - A lock-free work-stealing deque (Chase-Lev) for local fibers
//   - A ucontext_t for its dispatch loop (fibers yield back here)
//   - A wake eventfd (scheduler signals this to wake worker)
//
// Workers pull fibers from the LOCAL deque (LIFO, for cache locality).
// When empty, they try to STEAL from another random worker's deque
// (FIFO, from the other end). When nothing to steal, they sleep.
class WorkerThread {
public:
    WorkerThread(int id, Scheduler* scheduler);
    ~WorkerThread();

    // Non-copyable
    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;
    WorkerThread(WorkerThread&&) = delete;
    WorkerThread& operator=(WorkerThread&&) = delete;

    // Start the worker thread (creates std::jthread)
    void start();

    // Request stop
    void stop();

    // Enqueue a fiber to this worker's local deque.
    // Thread-safe (uses push which is owner-only, but callers are
    // external threads — this is fine for Chase-Lev as the caller
    // is effectively acting as owner for push).
    void enqueue(Fiber* fiber);

    // Accessors
    int id() const { return id_; }
    int wake_evfd() const { return wake_evfd_; }
    bool is_running() const { return running_.load(std::memory_order_acquire); }
    size_t queue_size() const { return local_queue_.size_approx(); }

    // Join (waits for thread to finish)
    void join();

    // Steal a fiber from this worker's deque (called by other workers).
    // Thread-safe (uses steal() which is lock-free for stealers).
    Fiber* try_steal() { return local_queue_.steal(); }

    // Notify the scheduler about a done fiber (via callback)
    void notify_fiber_done(Fiber* fiber);

private:
    // The main loop
    void run();

    // Try to steal from another worker
    // Returns true if a fiber was stolen and placed in local queue
    bool try_steal_from(WorkerThread* victim);

    int id_;
    Scheduler* scheduler_;

    // Worker's dispatch loop context — fibers yield back to this
    WorkerContext ctx_;

    // Lock-free work-stealing deque (Chase-Lev)
    WorkStealingDeque<Fiber*> local_queue_;

    // Wake eventfd — scheduler writes to this to wake the worker
    int wake_evfd_ = -1;

    // Condition variable for sleep/wake
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;

    // The OS thread
    std::jthread thread_;

    // Runtime flag
    std::atomic<bool> running_{false};

    // Steal budget (per-work-cycle, avoids busy-spin)
    StealBudget steal_budget_;

    // Pending count (for IO thread's liveness check)
    std::atomic<size_t> pending_{0};
};

} // namespace aura::serve

#endif // AURA_SERVE_WORKER_H
