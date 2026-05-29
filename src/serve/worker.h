// serve/worker.h — Worker thread with local fiber queue
#ifndef AURA_SERVE_WORKER_H
#define AURA_SERVE_WORKER_H

#include "fiber.h"
#include <ucontext.h>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <atomic>

namespace aura::serve {

// ── Forward declarations ──────────────────────────────
class Scheduler;
struct EpollEvent;

// ── WorkerThread — per-OS-thread fiber runner ──────────
//
// Each worker has:
//   - A local queue of Ready fibers
//   - A ucontext_t for its dispatch loop (fibers yield back here)
//   - A wake eventfd (scheduler signals this to wake worker)
//
// Workers pull fibers from the local queue, resume them,
// and handle the yield/done lifecycle. When the queue is empty
// and no fibers are in epoll wait, the worker blocks on a
// condition variable.
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

    // Enqueue a fiber to this worker's local queue
    void enqueue(Fiber* fiber);

    // Accessors
    int id() const { return id_; }
    const WorkerContext& context() const { return ctx_; }
    WorkerContext& context() { return ctx_; }
    int wake_evfd() const { return wake_evfd_; }
    bool is_running() const { return running_.load(std::memory_order_acquire); }
    size_t queue_size() const;

    // Join (waits for thread to finish)
    void join();

private:
    // The main loop run on the worker thread:
    //   - Drain local queue: resume fibers
    //   - Handle yield: re-enqueue or leave Waiting
    //   - When queue empty: wait on condition
    void run();

    int id_;
    Scheduler* scheduler_;

    // Worker's dispatch loop context — fibers yield back to this
    WorkerContext ctx_;

    // Local ready queue (protected by mutex)
    std::deque<Fiber*> local_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable wake_cv_;

    // Wake eventfd — scheduler writes to this to wake the worker
    int wake_evfd_ = -1;

    // The OS thread
    std::jthread thread_;

    // Runtime flag
    std::atomic<bool> running_{false};

    // Pending count (for load balancing)
    std::atomic<size_t> pending_{0};
};

} // namespace aura::serve

#endif // AURA_SERVE_WORKER_H
