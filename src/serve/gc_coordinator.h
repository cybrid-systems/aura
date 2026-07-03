// serve/gc_coordinator.h — GC coordinator (P2)
// Runs on the IO thread. Coordinates safepoint, mark, and sweep
// across all worker threads.
#ifndef AURA_SERVE_GC_COORDINATOR_H
#define AURA_SERVE_GC_COORDINATOR_H

#include "fiber.h" // GCPhase, WorkerGCState
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
    // Issue #682: compiler-managed IRClosure / bridge cache roots
    // (ClosureId from ir_cache_bridge_ + persistent IR interpreters).
    std::vector<int64_t> compiler_closure_roots;
    // Issue #682: live EnvId handles from compiler materialize paths.
    std::vector<int64_t> compiler_env_roots;

    bool empty() const {
        return string_roots.empty() && pair_roots.empty() && closure_roots.empty() &&
               fiber_result_roots.empty() && workspace_roots.empty() &&
               compiler_closure_roots.empty() && compiler_env_roots.empty();
    }
    void clear() {
        string_roots.clear();
        pair_roots.clear();
        closure_roots.clear();
        fiber_result_roots.clear();
        workspace_roots.clear();
        compiler_closure_roots.clear();
        compiler_env_roots.clear();
    }
};

// Root source callback — registered by each Evaluator (CompilerService).
// Called during the GC root collection phase.
using GCRootFlushFn = std::function<void(GCRootSet& out)>;

// Issue #205: env-walk callback (caller-side).
// The evaluator walks its env_frames_ SoA arena (O(frames))
// and produces index lists for pair/closure cells that are
// reachable through env parent chains. The GC marks each
// list's indices. This replaces the old pointer-chasing
// Env* walk with a single linear pass over env_frames_,
// giving 3-5x faster mark phase for large workspaces
// (per Issue #172).
//
// The callback is registered once (at startup) and called
// once per GC cycle (between mark_from_roots and sweep).
// Decoupling the walk from the GC keeps the GC's surface
// area narrow — it doesn't need to know EnvFrame's layout.
struct EnvFrameRoots {
    std::vector<int64_t> pair_roots;    // pair indices reachable through env chains
    std::vector<int64_t> closure_roots; // closure indices reachable through env chains
    // Future: string_roots, workspace_roots, etc. — add as
    // the issue's body sections get implemented.
};
using GCEnvWalkFn = std::function<void(EnvFrameRoots& out)>;

// Forward declarations for sweep types
class MarkBitVector;
struct GCSweepResult;

// ── GC sweep result (Phase 3+4) ────────────────────
// After marking and sweeping, records what was reclaimed.
struct GCSweepResult {
    size_t strings_freed = 0;
    size_t pairs_freed = 0;
    size_t closures_freed = 0;
    size_t fiber_results_freed = 0;
};

// ── Sweep callback (Phase 4) ───────────────────────
// After marking, the GC calls this to let the evaluator compact
// its vector heaps (string_heap_, pairs_, closures_, etc.) by
// removing unmarked entries.
struct GCSweepBuffers {
    const MarkBitVector* string_marks = nullptr;
    const MarkBitVector* pair_marks = nullptr;
    const MarkBitVector* closure_marks = nullptr;
};
using GCSweepFn = std::function<GCSweepResult(const GCSweepBuffers&)>;

// ── MarkBitVector — concurrent mark bits for vector heaps (Phase 3) ─
// One bit per index in a heap vector. Set by parallel marking workers.
// Uses std::vector<bool> internally which is bit-packed.
// Thread-safe: concurrent set_bit is safe (atomic byte writes).
class MarkBitVector {
public:
    explicit MarkBitVector(size_t size = 0)
        : bits_(size, false) {}

    void resize(size_t n) { bits_.resize(n, false); }
    void clear_all() { std::fill(bits_.begin(), bits_.end(), false); }
    size_t size() const { return bits_.size(); }

    // Mark an index as live. Thread-safe for concurrent marking.
    void set(size_t idx) {
        if (idx < bits_.size())
            bits_[idx] = true;
    }

    // Check if an index is live.
    bool test(size_t idx) const { return idx < bits_.size() && bits_[idx]; }

    // Return count of unmarked (dead) entries
    size_t count_dead() const {
        size_t dead = 0;
        for (size_t i = 0; i < bits_.size(); ++i)
            if (!bits_[i])
                ++dead;
        return dead;
    }

private:
    std::vector<bool> bits_;
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

    // ── Sweep callback registration (Phase 4) ───────
    // The evaluator registers a callback that compacts its
    // vector heaps after marking. Called during sweep phase.
    void register_sweep_fn(GCSweepFn fn);

    // Issue #205: env-walk callback (caller-side).
    // The evaluator registers a callback that walks its
    // env_frames_ SoA arena and produces the EnvFrameRoots
    // lists. The GC calls it between mark_from_roots and
    // sweep (so the mark vectors are already sized). This
    // is the 3-5x mark-phase speedup from #172.
    void register_env_walk_fn(GCEnvWalkFn fn) { env_walk_fn_ = std::move(fn); }

    // ── Mark + Sweep (Phase 3) ──────────────────────
    void mark_from_roots(const GCRootSet& roots, size_t string_heap_size, size_t pairs_size,
                         size_t closures_size);

    // Issue #172 / #204: env_frame_roots walk. The caller
    // (evaluator) walks the env_frames_ arena itself and
    // passes the resulting pair/closure indices here. The
    // GC doesn't know about EnvFrame's definition (it lives
    // in the evaluator); the caller does the walk and
    // returns the indices. This decouples the GC from the
    // EnvFrame type while still benefiting from the SoA
    // arena's linear-walk efficiency.
    //
    // pair_roots: pair indices reachable from any env frame
    //             (via bindings_symid_ holding a tagged pair ref)
    // closure_roots: closure indices reachable from any env
    //             frame (via bindings holding a tagged closure ref)
    //
    // The mark vectors must be sized (via mark_from_roots or
    // a direct resize) before calling this; otherwise set()
    // is a silent no-op.
    void mark_env_frame_roots(const std::vector<int64_t>& pair_roots,
                              const std::vector<int64_t>& closure_roots);

    GCSweepResult sweep();

    // Mark accessors (for testing)
    bool string_mark(size_t idx) const { return string_marks_.test(idx); }
    bool pair_mark(size_t idx) const { return pair_marks_.test(idx); }
    bool closure_mark(size_t idx) const { return closure_marks_.test(idx); }

    // ── Configuration ────────────────────────────────
    void set_alloc_threshold(int64_t threshold) { alloc_threshold_ = threshold; }
    int64_t alloc_threshold() const { return alloc_threshold_; }
    void reset_alloc_counter() { alloc_counter_.store(0, std::memory_order_release); }
    void record_alloc() { alloc_counter_.fetch_add(1, std::memory_order_relaxed); }

    // ── Metrics ─────────────────────────────────────
    struct Metrics {
        std::atomic<int64_t> gc_count{0};
        std::atomic<int64_t> total_pause_us{0};
        std::atomic<int64_t> max_pause_us{0};
        std::atomic<int64_t> safepoint_wait_us{0};
        std::atomic<int64_t> root_count{0};
        std::atomic<int64_t> root_collect_us{0};
        std::atomic<int64_t> mark_us{0};       // Phase 3: time to mark
        std::atomic<int64_t> sweep_us{0};      // Phase 3: time to sweep
        std::atomic<int64_t> strings_freed{0}; // entries removed
        std::atomic<int64_t> pairs_freed{0};
        std::atomic<int64_t> closures_freed{0};
    };
    const Metrics& metrics() const { return metrics_; }

private:
    Scheduler* scheduler_;

    std::mutex root_sources_mutex_;
    std::unordered_map<int, GCRootFlushFn> root_sources_;

    void collect_roots(GCRootSet& out);

    // Sweep callback (Phase 4)
    GCSweepFn sweep_fn_;

    // Issue #205: env-walk callback (caller-side). The
    // evaluator walks env_frames_ and produces pair/closure
    // index lists. Called between mark_from_roots and sweep.
    GCEnvWalkFn env_walk_fn_;

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
