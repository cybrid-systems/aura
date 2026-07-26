// serve/gc_coordinator.h — GC coordinator (P2)
// Runs on the IO thread. Coordinates safepoint, mark, and sweep
// across all worker threads.
#ifndef AURA_SERVE_GC_COORDINATOR_H
#define AURA_SERVE_GC_COORDINATOR_H

#include "fiber.h" // GCPhase, WorkerGCState
#include <atomic>
#include <bit> // std::popcount
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace aura::serve {

class Scheduler;

// Issue #2084: process-wide mirrors of mark-size injection metrics so
// Agents can observe coverage without a live GCCollector pointer
// (query:gc-mark-size-stats / engine:metrics). Updated whenever
// mark_from_roots receives non-zero sizes or collect() injects via size_fn_.
inline std::atomic<std::int64_t> g_mark_size_injected_total{0};
inline std::atomic<std::int64_t> g_mark_size_injected_heaps_total{0};
inline std::atomic<std::int64_t> g_last_injected_string_size{0};
inline std::atomic<std::int64_t> g_last_injected_pairs_size{0};
inline std::atomic<std::int64_t> g_last_injected_closures_size{0};
inline std::atomic<std::int64_t> g_mark_size_provider_wired{1}; // serve_async registers size_fn

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

// ── MarkBitVector — concurrent mark bits for vector heaps (Phase 3 / #2117) ─
// One bit per index in a heap vector. Set by parallel marking workers.
// Issue #2117: storage is std::atomic<uint64_t> words (not vector<bool>).
// Concurrent set of the same / adjacent bits cannot lost-update.
// Memory order: relaxed is enough — mark does not require happens-before
// beyond the safepoint / STW fence that brackets the mark phase. Under
// pure STW single-thread mark the atomic ops have no extra observable
// latency vs non-atomic (AC3); multi-fiber concurrent root set is correct.
// Move-only (atomic array is not copyable).
class MarkBitVector {
public:
    explicit MarkBitVector(size_t size = 0) { resize(size); }

    MarkBitVector(const MarkBitVector&) = delete;
    MarkBitVector& operator=(const MarkBitVector&) = delete;

    MarkBitVector(MarkBitVector&& o) noexcept
        : words_(std::move(o.words_))
        , bit_count_(o.bit_count_)
        , word_count_(o.word_count_) {
        o.bit_count_ = 0;
        o.word_count_ = 0;
    }

    MarkBitVector& operator=(MarkBitVector&& o) noexcept {
        if (this != &o) {
            words_ = std::move(o.words_);
            bit_count_ = o.bit_count_;
            word_count_ = o.word_count_;
            o.bit_count_ = 0;
            o.word_count_ = 0;
        }
        return *this;
    }

    void resize(size_t n) {
        bit_count_ = n;
        word_count_ = (n + 63) / 64;
        if (word_count_ == 0) {
            words_.reset();
            return;
        }
        words_ = std::make_unique<std::atomic<std::uint64_t>[]>(word_count_);
        for (size_t i = 0; i < word_count_; ++i)
            words_[i].store(0, std::memory_order_relaxed);
    }

    void clear_all() {
        for (size_t i = 0; i < word_count_; ++i)
            words_[i].store(0, std::memory_order_relaxed);
    }

    // Drop storage (used after sweep). Equivalent to default-constructed state.
    void reset() noexcept {
        words_.reset();
        bit_count_ = 0;
        word_count_ = 0;
    }

    [[nodiscard]] size_t size() const noexcept { return bit_count_; }

    // Mark an index as live. Thread-safe concurrent set via fetch_or.
    void set(size_t idx) {
        if (idx >= bit_count_ || !words_)
            return;
        const size_t w = idx >> 6;
        const std::uint64_t mask = 1ull << (idx & 63);
        words_[w].fetch_or(mask, std::memory_order_relaxed);
    }

    // Check if an index is live (relaxed load).
    [[nodiscard]] bool test(size_t idx) const {
        if (idx >= bit_count_ || !words_)
            return false;
        const size_t w = idx >> 6;
        const auto word = words_[w].load(std::memory_order_relaxed);
        return ((word >> (idx & 63)) & 1ull) != 0;
    }

    // Return count of unmarked (dead) entries among [0, bit_count_).
    [[nodiscard]] size_t count_dead() const {
        if (bit_count_ == 0 || !words_)
            return 0;
        size_t live = 0;
        for (size_t i = 0; i < word_count_; ++i)
            live += static_cast<size_t>(std::popcount(words_[i].load(std::memory_order_relaxed)));
        // Unused high bits in the last word stay 0 (never set) so popcount
        // over full words equals live bits in [0, bit_count_).
        return bit_count_ - live;
    }

    // Issue #2117: true when storage uses atomic words (observability).
    [[nodiscard]] static constexpr bool is_atomic_storage() noexcept { return true; }

private:
    std::unique_ptr<std::atomic<std::uint64_t>[]> words_;
    size_t bit_count_ = 0;
    size_t word_count_ = 0;
};

// Issue #2117: process-wide "atomic mark wired" sentinel for query surface.
inline std::atomic<std::int64_t> g_mark_bitvector_atomic_wired{1};


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

    // Issue #2084: size-provider callback (caller-side). The
    // evaluator publishes its current (string_heap_size,
    // pairs_size, closures_size) so mark_from_roots can size
    // the MarkBitVectors correctly. Without real sizes,
    // mark_from_roots falls back to max-root-index + 1 and
    // high-water dead slots above any live root index never
    // enter the MarkBitVector — silently under-covering the
    // heap under long-running mutate+GC loops.
    //
    // The callback is invoked between collect_roots and
    // mark_from_roots. Returning (0, 0, 0) preserves the
    // pre-#2084 root-derived sizing fallback.
    using GCSizeFn = std::function<std::tuple<std::size_t, std::size_t, std::size_t>()>;
    void register_size_fn(GCSizeFn fn) { size_fn_ = std::move(fn); }

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

    // Mark accessors (for testing + #2084 size-coverage ACs)
    bool string_mark(size_t idx) const { return string_marks_.test(idx); }
    bool pair_mark(size_t idx) const { return pair_marks_.test(idx); }
    bool closure_mark(size_t idx) const { return closure_marks_.test(idx); }
    // Issue #2084: MarkBitVector extents after mark_from_roots — must equal
    // injected heap sizes when size provider returns non-zero (not just
    // max root index + 1).
    [[nodiscard]] size_t string_marks_size() const noexcept { return string_marks_.size(); }
    [[nodiscard]] size_t pair_marks_size() const noexcept { return pair_marks_.size(); }
    [[nodiscard]] size_t closure_marks_size() const noexcept { return closure_marks_.size(); }
    [[nodiscard]] size_t string_marks_dead_count() const noexcept {
        return string_marks_.count_dead();
    }
    [[nodiscard]] size_t pair_marks_dead_count() const noexcept { return pair_marks_.count_dead(); }
    [[nodiscard]] size_t closure_marks_dead_count() const noexcept {
        return closure_marks_.count_dead();
    }

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
        // Issue #2084: cycle count where mark_from_roots was called with
        // non-zero heap sizes (i.e. size provider was registered and
        // returned > 0). 0 = call still uses root-derived fallback.
        std::atomic<int64_t> mark_size_injected_total{0};
        // Issue #2084: cumulative heap-size slots covered by injected
        // sizes (sum of string + pair + closures across cycles that
        // successfully injected). Dashboard visibility.
        std::atomic<int64_t> mark_size_injected_heaps_total{0};
        // Issue #2084: last injected (string, pairs, closures) sizes for
        // Agent inspection of the most recent cycle.
        std::atomic<int64_t> last_injected_string_size{0};
        std::atomic<int64_t> last_injected_pairs_size{0};
        std::atomic<int64_t> last_injected_closures_size{0};
        // Issue #1256: fiber-side safepoint wait latency while MutationBoundary held.
        std::atomic<int64_t> eventfd_wakeup_latency_us{0};
        std::atomic<int64_t> safepoint_wait_while_mutation_held{0};
        std::atomic<int64_t> safepoint_blocked_by_long_mutation{0};
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

    // Issue #2084: size-provider callback (caller-side).
    // Returns (string_heap_size, pairs_size, closures_size)
    // so mark_from_roots can size the MarkBitVectors to the
    // actual current heap extent (not just max root index).
    GCSizeFn size_fn_;

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
