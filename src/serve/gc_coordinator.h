// serve/gc_coordinator.h — GC coordinator (P2)
// Runs on the IO thread. Coordinates safepoint, mark, and sweep
// across all worker threads.
#ifndef AURA_SERVE_GC_COORDINATOR_H
#define AURA_SERVE_GC_COORDINATOR_H

#include "fiber.h"  // GCPhase, WorkerGCState
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include <chrono>
#include <unordered_map>

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

// ── RootSet — GC root set (Phase 2) ─────────────────
// Stores indices into evaluator heaps that must be kept alive.
// The actual traversal is done by flush_gc_roots callbacks
// registered by each evaluator.
struct GCRootSet {
    // Indices into string_heap_ that are still reachable
    std::vector<int64_t> string_roots;
    // Pair indices that are still reachable
    std::vector<int64_t> pair_roots;
    // Closure IDs that are still alive
    std::vector<int64_t> closure_roots;
    // Fiber result pointers (s_fiber_results)
    std::vector<int64_t> fiber_result_roots;
    // Workspace tree node flat indices
    std::vector<int64_t> workspace_roots;

    bool empty() const {
        return string_roots.empty() && pair_roots.empty()
            && closure_roots.empty() && fiber_result_roots.empty()
            && workspace_roots.empty();
    }
    void clear() {
        string_roots.clear();
        pair_roots.clear();
        closure_roots.clear();
        fiber_result_roots.clear();
        workspace_roots.clear();
    }
};

// Root source callback — registered by each Evaluator (CompilerService).
// Called during the GC root collection phase.
using GCRootFlushFn = std::function<void(GCRootSet& out)>;

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

    // ── Root source registration (Phase 2) ──────────
    // Each Evaluator registers a callback that enumerates its
    // reachable roots (string_heap, pairs, closures, etc.)
    // into a GCRootSet. Called from g_gc_flush_root_set
    // during GC root collection.
    void register_root_source(int worker_id, GCRootFlushFn fn);
    void unregister_root_source(int worker_id);

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
        std::atomic<int64_t> root_count{0};      // Phase 2: number of roots found
        std::atomic<int64_t> root_collect_us{0}; // Phase 2: time to collect roots
    };
    const Metrics& metrics() const { return metrics_; }

private:
    Scheduler* scheduler_;

    // Root sources (Phase 2): per-worker-id callbacks
    std::mutex root_sources_mutex_;
    std::unordered_map<int, GCRootFlushFn> root_sources_;

    // Collect all roots from registered sources
    void collect_roots(GCRootSet& out);

    std::atomic<int64_t> alloc_counter_{0};
    int64_t alloc_threshold_ = 100000;
    std::atomic<bool> gc_in_progress_{false};
    bool gc_in_progress() const { return gc_in_progress_.load(std::memory_order_acquire); }
    Metrics metrics_;
};

} // namespace aura::serve

#endif // AURA_SERVE_GC_COORDINATOR_H
