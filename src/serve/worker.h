// serve/worker.h — Worker thread with work-stealing queue
#ifndef AURA_SERVE_WORKER_H
#define AURA_SERVE_WORKER_H

#include "fiber.h"
#include "ws_deque.h"
#include "metrics.h"
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
//
// Phase 4: adaptive mode. When adaptive is enabled,
// max_before_sleep adjusts based on overall steal success rate.
// If the system is under-loaded (many steal attempts fail),
// workers learn to sleep sooner. Under high steal success,
// they stay alert longer.
struct StealBudget {
    int consecutive_failures = 0;
    int max_before_sleep = 3;  // after N failures, go to sleep

    // Adaptive tuning state
    static constexpr int WINDOW_SIZE = 10;
    int history[WINDOW_SIZE] = {0};
    int history_idx = 0;
    int total_attempts = 0;
    int total_successes = 0;
    bool adaptive_enabled = true;

    StealBudget() = default;

    explicit StealBudget(bool adaptive) : adaptive_enabled(adaptive) {}

    bool should_steal() const {
        return consecutive_failures < max_before_sleep;
    }

    void record_success() {
        consecutive_failures = 0;
        if (adaptive_enabled) {
            history[history_idx % WINDOW_SIZE] = 1;
            ++history_idx;
            ++total_attempts;
            ++total_successes;
            adapt();
        }
    }

    void record_failure() {
        ++consecutive_failures;
        if (adaptive_enabled) {
            history[history_idx % WINDOW_SIZE] = 0;
            ++history_idx;
            ++total_attempts;
            adapt();
        }
    }

    // Dynamically adjust max_before_sleep based on recent steal success rate
    //   - High success (>50%): stay alert longer (max_before_sleep = 5-8)
    //   - Medium success (20-50%): default (3-4)
    //   - Low success (<20%): sleep sooner (1-2)
    void adapt() {
        if (history_idx < WINDOW_SIZE) return;  // not enough data yet

        int successes = 0;
        for (int i = 0; i < WINDOW_SIZE; ++i) successes += history[i];
        double rate = static_cast<double>(successes) / WINDOW_SIZE;

        if (rate > 0.50) {
            max_before_sleep = 6;
        } else if (rate > 0.30) {
            max_before_sleep = 4;
        } else if (rate > 0.10) {
            max_before_sleep = 2;
        } else {
            max_before_sleep = 1;
        }
    }

    void reset() {
        consecutive_failures = 0;
        max_before_sleep = 3;
        history_idx = 0;
        total_attempts = 0;
        total_successes = 0;
    }
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

    // Pending fiber count (for liveness check)
    size_t pending_count() const { return pending_.load(std::memory_order_acquire); }

    // Join (waits for thread to finish)
    void join();

    // Steal a fiber from this worker's deque (called by other workers).
    // Thread-safe (uses steal() which is lock-free for stealers).
    Fiber* try_steal() { return local_queue_.steal(); }

    // Notify the scheduler about a done fiber (via callback)
    void notify_fiber_done(Fiber* fiber);

    // ── Metrics access ──────────────────────────────
    void set_metrics(metrics::WorkerMetrics* m) { worker_metrics_ = m; }
    metrics::WorkerMetrics* worker_metrics() const { return worker_metrics_; }

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

    // Metrics pointer (set by scheduler, never null during run)
    metrics::WorkerMetrics* worker_metrics_ = nullptr;
};

} // namespace aura::serve

#endif // AURA_SERVE_WORKER_H
