// serve/worker.h — Worker thread with work-stealing queue
#ifndef AURA_SERVE_WORKER_H
#define AURA_SERVE_WORKER_H

#include "fiber.h"
#include "ws_deque.h"
#include "metrics.h"
#include <cstring>
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
    int max_before_sleep = 3; // after N failures, go to sleep

    // Adaptive tuning state
    static constexpr int WINDOW_SIZE = 10;
    int history[WINDOW_SIZE] = {0};
    int history_idx = 0;
    int total_attempts = 0;
    int total_successes = 0;
    bool adaptive_enabled = true;

    StealBudget() = default;

    explicit StealBudget(bool adaptive)
        : adaptive_enabled(adaptive) {}

    bool should_steal() const { return consecutive_failures < max_before_sleep; }

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
        if (history_idx < WINDOW_SIZE)
            return; // not enough data yet

        int successes = 0;
        for (int i = 0; i < WINDOW_SIZE; ++i)
            successes += history[i];
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

    // Issue #706: when inner MutationBoundary steals are deferred,
    // temporarily raise alertness so outermost/Explicit fibers are found.
    void apply_deferred_pressure(std::uint64_t deferred_total) {
        if (!adaptive_enabled || deferred_total == 0)
            return;
        const int before = max_before_sleep;
        if (deferred_total > 50) {
            max_before_sleep = std::max(max_before_sleep, 7);
        } else if (deferred_total > 10) {
            max_before_sleep = std::max(max_before_sleep, 5);
        } else if (deferred_total > 0) {
            max_before_sleep = std::max(max_before_sleep, 4);
        }
        if (max_before_sleep > before)
            metrics::adaptive_steal_stats().deferred_pressure_boosts.fetch_add(
                1, std::memory_order_relaxed);
    }
};

// Issue #706: steal priority for LLM-bottleneck adaptive bias.
// Higher = prefer stealing this yielded fiber.
// Issue #1492: starvation-mitigation boost raises priority for fibers
// that were repeatedly deferred at an inner MutationBoundary so that,
// once they become outermost-safe, they win the next steal round.
inline int fiber_steal_priority(Fiber* fiber) {
    if (!fiber || !fiber->is_stealable())
        return -1;
    int base = 1;
    if (fiber->is_at_mutation_boundary_safe()) {
        const char* cls = fiber->yield_classification();
        if (std::strcmp(cls, "Explicit") == 0 || std::strcmp(cls, "OperationBoundary") == 0)
            base = 3;
        else if (std::strcmp(cls, "MutationBoundary/outermost") == 0)
            base = 2;
        else
            base = 1;
    } else if (fiber->last_yield_reason() == YieldReason::MutationBoundary) {
        base = 0; // not steal-safe (inner) — lowest
    }
    if (fiber->has_steal_priority_boost() && base >= 1)
        base = std::max(base, 3); // boost to LLM-tail tier once safe
    return base;
}

// Issue #1492: apply starvation mitigation after deferring a steal of a
// fiber at an inner MutationBoundary (depth > 0). Boosts fiber steal
// priority, raises deferred-pressure budget signal, and bumps the
// dedicated steal_inner_deferred_starvation_mitigated_count metric.
inline void apply_starvation_mitigation(Fiber* fiber) noexcept {
    if (!fiber)
        return;
    fiber->apply_steal_priority_boost();
    auto& s = metrics::adaptive_steal_stats();
    s.deferred_pressure_boosts.fetch_add(1, std::memory_order_relaxed);
    s.starvation_priority_boosts.fetch_add(1, std::memory_order_relaxed);
    s.steal_priority_boost_triggered.fetch_add(1, std::memory_order_relaxed);
    s.steal_inner_deferred_starvation_mitigated_count.fetch_add(1, std::memory_order_relaxed);
}

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

    // Issue #119: per-worker fiber registry. Indexed by fiber ID
    // for O(1) lookup from fiber_by_id. Maintained by enqueue +
    // notify_fiber_done. Protected by a small mutex because it's
    // touched from the scheduler's IO thread (lookup) and the
    // worker's own thread (enqueue + done).
    void register_fiber(Fiber* fiber);
    void unregister_fiber(Fiber* fiber);
    Fiber* fiber_by_id(std::uint64_t fiber_id) const;

    // Notify the scheduler about a done fiber (via callback)
    void notify_fiber_done(Fiber* fiber);

    // ── GC state (P2) ────────────────────────────────
    // Each worker has its own GC state. The scheduler's
    // GC coordinator broadcasts phase transitions.
    WorkerGCState gc_state_;
    WorkerGCState& gc_state() { return gc_state_; }

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

    // Issue #119: per-worker fiber registry for fiber:join.
    // Holds pointers to all fibers this worker currently knows
    // about (enqueued, running, or recently completed but not
    // yet removed). Used by fiber_by_id to look up the
    // joiner-map target. Protected by `fiber_registry_mutex_`.
    std::unordered_map<std::uint64_t, Fiber*> fiber_registry_;
    mutable std::mutex fiber_registry_mutex_;
};

} // namespace aura::serve

#endif // AURA_SERVE_WORKER_H
