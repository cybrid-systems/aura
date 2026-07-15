// serve/scheduler.h — Multi-threaded fiber scheduler
#ifndef AURA_SERVE_SCHEDULER_H
#define AURA_SERVE_SCHEDULER_H

#include "fiber.h"
#include "worker.h"
#include "metrics.h"
#include <ucontext.h>
#include <deque>
#include <unordered_map>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>

namespace aura::serve {

// Forward declarations
class GCCollector;

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
    // (round-robin across available workers, or load-aware when enabled).
    Fiber* spawn(Fiber::Func func, size_t stack_size = 2 * 1024 * 1024);

    // Create a new fiber pinned to a specific worker (affinity).
    // worker_id: 0..num_workers-1. The fiber will always run on
    // that worker, even after yield/wakeup (cache-aware scheduling).
    Fiber* spawn_with_affinity(Fiber::Func func, int worker_id,
                               size_t stack_size = 2 * 1024 * 1024);

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

    // Issue #119: proper-blocking fiber:join API. The
    // joiner fiber is registered against the target fiber's
    // id. When the target completes (on_fiber_done), all
    // registered joiners are woken by writing to their eventfds.
    //
    // The joiner calls this from inside its own fiber (it has
    // a current Fiber*). The target may or may not exist yet
    // (a fiber ID is just an int64_t; the lookup goes through
    // `fiber_by_id`). If the target is already done, the
    // joiner is NOT registered — the caller checks via
    // `fiber_by_id(id)->is_done()` before calling add_joiner.
    //
    // Returns true on success, false if the target fiber
    // cannot be found (caller should treat that as a join
    // failure).
    bool add_joiner(std::uint64_t target_fiber_id, Fiber* joiner);

    // Issue #119: remove a previously-registered joiner. Called
    // by the joiner after it wakes up (regardless of whether
    // the target completed or the join timed out), to clean
    // up the map. Idempotent: removing a joiner that wasn't
    // registered is a no-op.
    void remove_joiner(std::uint64_t target_fiber_id, Fiber* joiner);

    // Issue #119: lookup a Fiber* by its ID. Returns nullptr
    // if no such fiber exists (either the fiber has been
    // destroyed, or the id was never valid). Used by the
    // evaluator's fiber:join to check if the target is
    // already done before registering as a joiner.
    Fiber* fiber_by_id(std::uint64_t fiber_id) const;

    // ── Worker management ───────────────────────────
    int num_workers() const { return num_workers_; }
    WorkerThread* worker(int idx);
    int next_worker_id(); // round-robin assignment

    // ── Phase 4: load-aware worker selection ─────────
    // Picks the worker with the smallest local queue.
    // Falls back to round-robin when queue sizes are unavailable.
    int next_worker_id_load_aware();

    // ── Stdin fiber ─────────────────────────────────
    void set_stdin_fiber(Fiber* f) { stdin_fiber_ = f; }
    Fiber* stdin_fiber() const { return stdin_fiber_; }

    // ── GC support (P2) ─────────────────────────────
    // Access the GC collector (may be null if not initialized).
    GCCollector* gc_collector() const { return gc_collector_.get(); }

    // Request all workers reach safepoint for GC.
    // Returns number of workers that acknowledged.
    int request_gc_safepoint();

    // Wait for all workers to reach safepoint.
    // Returns true if all workers arrived within timeout_ms.
    bool wait_for_safepoint(int timeout_ms = 100);

    // Resume all workers after GC completes.
    void resume_from_gc();

    // Accessors
    int epoll_fd() const { return epoll_fd_; }

    // ── Metrics access ──────────────────────────────
    metrics::GlobalMetrics& metrics() { return metrics_; }
    const metrics::GlobalMetrics& metrics() const { return metrics_; }
    std::string metrics_json() const { return metrics_.to_json(); }
    void enable_metrics(bool on = true) { metrics_on_ = on; }
    bool metrics_enabled() const { return metrics_on_; }

    // Issue #1443 AC3 follow-up + #1445 AC6: scheduler hook for long-mutation.
    // Called from MutationBoundaryGuard dtor when an outermost Guard exits
    // with hold duration > long_mutation_threshold_us. Default impl bumps
    // starvation_mitigated_count (AdaptiveStealStats) so waiters can be
    // priority-boosted. fiber_id is the outermost fiber; duration_us is the
    // hold duration in microseconds.
    void on_long_mutation_held(std::uint64_t fiber_id, std::uint64_t duration_us);


private:
    int num_workers_;
    std::vector<std::unique_ptr<WorkerThread>> workers_;
    std::atomic<int> next_worker_{0}; // round-robin counter

    // IO thread resources
    int epoll_fd_ = -1;
    int stdin_fd_ = -1;

    // eventfd → Fiber mapping (protected by mutex)
    std::unordered_map<int, Fiber*> wait_map_;
    mutable std::mutex wait_map_mutex_;

    // Issue #63723: helper to check if a Fiber* is still
    // owned by owned_fibers_ (source of truth for live
    // fibers). Used by Scheduler::run's event-dispatch to
    // skip stale events whose Fiber* has been corrupted or
    // whose atomic load would SIGSEGV (the underlying
    // corruption that produces these stale entries is a
    // separate root-cause investigation; this is a minimal
    // defensive guard).
    bool owned_fibers_end_contains(const Fiber* fiber) const {
        for (const auto& f : owned_fibers_) {
            if (f.get() == fiber)
                return true;
        }
        return false;
    }

    // Issue #119: joiner map — target fiber ID → set of joiner
    // fibers. When a target fiber completes, all its joiners
    // are woken via their eventfds (one write per joiner; the
    // scheduler writes to each joiner's eventfd, which is also
    // registered in wait_map_ for epoll).
    //
    // Stored as a map from target's uint64_t ID to a vector of
    // joiner Fiber* pointers. The vector is keyed so the same
    // joiner can join multiple targets without double-registration.
    std::unordered_map<std::uint64_t, std::vector<Fiber*>> joiner_map_;
    mutable std::mutex joiner_map_mutex_;

    // Stdin fiber (handles stdin line protocol in serve mode)
    Fiber* stdin_fiber_ = nullptr;

    // Runtime flag
    std::atomic<bool> running_{true};

    // ── Config ───────────────────────────────────────
    // Use load-aware distribution instead of round-robin
    bool use_load_aware_distribution_ = true;

    // GC collector (Phase 1-4, initialized in constructor)
    std::unique_ptr<GCCollector> gc_collector_;

    // Metrics collection
    metrics::GlobalMetrics metrics_;
    std::atomic<bool> metrics_on_{true};

    // Issue #707: per-scheduler fiber ownership so ~Fiber returns
    // per-fiber stack storage to the bounded pool on teardown.
    std::vector<std::unique_ptr<Fiber>> owned_fibers_;
    std::mutex owned_fibers_mutex_;
};

} // namespace aura::serve

#endif // AURA_SERVE_SCHEDULER_H
