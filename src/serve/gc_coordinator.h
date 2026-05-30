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

// ── MarkBitVector — concurrent mark bits for vector heaps (Phase 3) ─
// One bit per index in a heap vector. Set by parallel marking workers.
// Uses std::vector<bool> internally which is bit-packed.
// Thread-safe: concurrent set_bit is safe (atomic byte writes).
class MarkBitVector {
public:
    explicit MarkBitVector(size_t size = 0) : bits_(size, false) {}

    void resize(size_t n) { bits_.resize(n, false); }
    void clear_all() { std::fill(bits_.begin(), bits_.end(), false); }
    size_t size() const { return bits_.size(); }

    // Mark an index as live. Thread-safe for concurrent marking.
    void set(size_t idx) {
        if (idx < bits_.size())
            bits_[idx] = true;
    }

    // Check if an index is live.
    bool test(size_t idx) const {
        return idx < bits_.size() && bits_[idx];
    }

    // Return count of unmarked (dead) entries
    size_t count_dead() const {
        size_t dead = 0;
        for (size_t i = 0; i < bits_.size(); ++i)
            if (!bits_[i]) ++dead;
        return dead;
    }

private:
    std::vector<bool> bits_;
};

// ── GC sweep result (Phase 3) ────────────────────────
// After marking and sweeping, records what was reclaimed.
struct GCSweepResult {
    // Number of entries removed from each heap
    size_t strings_freed = 0;
    size_t pairs_freed = 0;
    size_t closures_freed = 0;
    size_t fiber_results_freed = 0;
};

class GCCollector {
public:
    explicit GCCollector(Scheduler* sched);

    // ── GC request ──────────────────────────────────
    bool request();

    // ── GC cycle ────────────────────────────────────
    // Called from IO thread's epoll loop.
    // Returns true if GC actually ran.
    bool collect();

    // ── Root source registration (Phase 2) ──────────
    void register_root_source(int worker_id, GCRootFlushFn fn);
    void unregister_root_source(int worker_id);

    // ── Mark + Sweep (Phase 3) ──────────────────────
    void mark_from_roots(const GCRootSet& roots,
                         size_t string_heap_size,
                         size_t pairs_size,
                         size_t closures_size);

    GCSweepResult sweep();

    // Mark accessors (for testing)
    bool string_mark(size_t idx) const { return string_marks_.test(idx); }
    bool pair_mark(size_t idx) const { return pair_marks_.test(idx); }
    bool closure_mark(size_t idx) const { return closure_marks_.test(idx); }

    // ── Configuration ────────────────────────────────
    void set_alloc_threshold(int64_t threshold) { alloc_threshold_ = threshold; }
    int64_t alloc_threshold() const { return alloc_threshold_; }
    void reset_alloc_counter() { alloc_counter_.store(0, std::memory_order_release); }
    void record_alloc() {
        alloc_counter_.fetch_add(1, std::memory_order_relaxed);
    }

    // ── Metrics ─────────────────────────────────────
    struct Metrics {
        std::atomic<int64_t> gc_count{0};
        std::atomic<int64_t> total_pause_us{0};
        std::atomic<int64_t> max_pause_us{0};
        std::atomic<int64_t> safepoint_wait_us{0};
        std::atomic<int64_t> root_count{0};
        std::atomic<int64_t> root_collect_us{0};
        std::atomic<int64_t> mark_us{0};          // Phase 3: time to mark
        std::atomic<int64_t> sweep_us{0};         // Phase 3: time to sweep
        std::atomic<int64_t> strings_freed{0};    // entries removed
        std::atomic<int64_t> pairs_freed{0};
        std::atomic<int64_t> closures_freed{0};
    };
    const Metrics& metrics() const { return metrics_; }

private:
    Scheduler* scheduler_;

    std::mutex root_sources_mutex_;
    std::unordered_map<int, GCRootFlushFn> root_sources_;

    void collect_roots(GCRootSet& out);

    // Mark state (Phase 3)
    MarkBitVector string_marks_;
    MarkBitVector pair_marks_;
    MarkBitVector closure_marks_;

    std::atomic<int64_t> alloc_counter_{0};
    int64_t alloc_threshold_ = 100000;
    std::atomic<bool> gc_in_progress_{false};
    bool gc_in_progress() const { return gc_in_progress_.load(std::memory_order_acquire); }
    Metrics metrics_;
};

} // namespace aura::serve

#endif // AURA_SERVE_GC_COORDINATOR_H
