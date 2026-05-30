// serve/gc_coordinator.h — GC coordinator (P2)
// Runs on the IO thread. Coordinates safepoint, mark, and sweep
// across all worker threads.
#ifndef AURA_SERVE_GC_COORDINATOR_H
#define AURA_SERVE_GC_COORDINATOR_H

#include "fiber.h"  // GCPhase, WorkerGCState
#include <atomic>
#include <functional>
#include <vector>
#include <chrono>

namespace aura::serve {

class Scheduler;

// ── GCCollector — GC coordinator (Phase 1: safepoint) ────
//
// Manages the GC lifecycle:
//   1. request() — trigger GC if enough allocs have been done
//   2. collect() — full GC cycle (called from IO thread)
//      a. Broadcast safepoint → wait for all workers to arrive
//      b. Collect roots from each evaluator
//      c. Parallel mark
//      d. Sweep
//      e. Resume workers
//
// Phase 1 implements steps (a) and (e). Steps (b)-(d)
// are skeletons that will be filled in Phases 2-4.
class GCCollector {
public:
    explicit GCCollector(Scheduler* sched);

    // ── GC request ──────────────────────────────────
    // Called by any fiber when alloc count crosses threshold.
    // Returns true if GC was triggered (or was already in progress).
    bool request();

    // ── GC cycle ────────────────────────────────────
    // Called from IO thread's epoll loop.
    // Returns true if GC actually ran.
    bool collect();

    // ── Configuration ────────────────────────────────
    // Sets the alloc threshold that triggers GC.
    void set_alloc_threshold(int64_t threshold) { alloc_threshold_ = threshold; }
    int64_t alloc_threshold() const { return alloc_threshold_; }

    // Reset alloc counter (called after GC)
    void reset_alloc_counter() { alloc_counter_.store(0, std::memory_order_release); }

    // Increment alloc counter (called from arena::alloc)
    void record_alloc() {
        alloc_counter_.fetch_add(1, std::memory_order_relaxed);
    }

    // ── Metrics ─────────────────────────────────────
    struct Metrics {
        std::atomic<int64_t> gc_count{0};
        std::atomic<int64_t> total_pause_us{0};
        std::atomic<int64_t> max_pause_us{0};
        std::atomic<int64_t> safepoint_wait_us{0};
    };
    const Metrics& metrics() const { return metrics_; }

private:
    Scheduler* scheduler_;
    std::atomic<int64_t> alloc_counter_{0};
    int64_t alloc_threshold_ = 100000;  // 100k allocs → trigger GC
    std::atomic<bool> gc_in_progress_{false};
    Metrics metrics_;
};

} // namespace aura::serve

#endif // AURA_SERVE_GC_COORDINATOR_H
