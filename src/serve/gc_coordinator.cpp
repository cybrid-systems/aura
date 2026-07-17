// serve/gc_coordinator.cpp — GC coordinator implementation
#include "gc_coordinator.h"
#include "scheduler.h"
#include "core/gc_hooks.h"

import std;
namespace aura::serve {

// ── Constructor ─────────────────────────────────────────

GCCollector::GCCollector(Scheduler* sched)
    : scheduler_(sched) {
    if (sched) {
        alloc_threshold_ = 100000;
    }
}

// ── request — trigger GC if threshold crossed ──────────

bool GCCollector::request() {
    if (gc_in_progress_.load(std::memory_order_acquire))
        return false;

    int64_t count = alloc_counter_.load(std::memory_order_relaxed);
    if (count < alloc_threshold_)
        return false;

    bool expected = false;
    if (!gc_in_progress_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return false;

    return true;
}

// ── register_root_source / unregister_root_source ──────

void GCCollector::register_root_source(int worker_id, GCRootFlushFn fn) {
    std::lock_guard<std::mutex> lock(root_sources_mutex_);
    root_sources_[worker_id] = std::move(fn);
}

void GCCollector::unregister_root_source(int worker_id) {
    std::lock_guard<std::mutex> lock(root_sources_mutex_);
    root_sources_.erase(worker_id);
}

// ── register_sweep_fn — register evaluator heap compaction (Phase 4) ─

void GCCollector::register_sweep_fn(GCSweepFn fn) {
    sweep_fn_ = std::move(fn);
}

// ── collect_roots — enumerate all registered root sources ──

void GCCollector::collect_roots(GCRootSet& out) {
    out.clear();
    std::lock_guard<std::mutex> lock(root_sources_mutex_);
    for (auto& [wid, fn] : root_sources_) {
        if (fn)
            fn(out);
    }
}

// ── collect — full GC cycle ────────────────────────────

bool GCCollector::collect() {
    if (!gc_in_progress_.load(std::memory_order_acquire))
        return false;

    // Issue #1489: defer full GC while a PanicCheckpoint recovery
    // window is open (arm on save / block_gc trampoline; release on
    // commit/restore). Avoid compact_sweep of pinned state.
    if (aura::gc_hooks::gc_deferred_for_pending_panic()) {
        aura::gc_hooks::note_gc_sweep_skipped_pending_panic();
        gc_in_progress_.store(false, std::memory_order_release);
        return false;
    }

    auto start = std::chrono::steady_clock::now();

    // ── Phase 1a: Broadcast safepoint ─────────────────
    scheduler_->request_gc_safepoint();

    // ── Phase 1b: Wait for all workers to arrive ──────
    auto safepoint_start = std::chrono::steady_clock::now();
    bool all_stopped = scheduler_->wait_for_safepoint(100);
    auto safepoint_end = std::chrono::steady_clock::now();

    if (!all_stopped) {
        scheduler_->resume_from_gc();
        gc_in_progress_.store(false, std::memory_order_release);
        return false;
    }

    auto safepoint_us =
        std::chrono::duration_cast<std::chrono::microseconds>(safepoint_end - safepoint_start)
            .count();

    // Issue #1364: advertise STW window to mutators (in_gc_safepoint).
    // Scope covers mark+sweep until resume_from_gc below.
    {
        aura::gc_hooks::ScopedSafepoint safepoint_active_guard;

        // ── Phase 2: Collect roots from all registered sources ──
        auto roots_start = std::chrono::steady_clock::now();
        GCRootSet roots;
        collect_roots(roots);
        auto roots_end = std::chrono::steady_clock::now();
        auto roots_us =
            std::chrono::duration_cast<std::chrono::microseconds>(roots_end - roots_start).count();

        metrics_.root_count.store(
            static_cast<int64_t>(
                roots.string_roots.size() + roots.pair_roots.size() + roots.closure_roots.size() +
                roots.fiber_result_roots.size() + roots.workspace_roots.size() +
                roots.compiler_closure_roots.size() + roots.compiler_env_roots.size()),
            std::memory_order_relaxed);
        metrics_.root_collect_us.fetch_add(roots_us, std::memory_order_relaxed);

        // ── Phase 3: Mark from roots ──────────────────────
        auto mark_start = std::chrono::steady_clock::now();
        // Size hints: default to root count estimate.
        // In full integration, evaluator provides actual heap sizes.
        mark_from_roots(roots, 0, 0, 0);
        auto mark_end = std::chrono::steady_clock::now();
        auto mark_us =
            std::chrono::duration_cast<std::chrono::microseconds>(mark_end - mark_start).count();
        metrics_.mark_us.fetch_add(mark_us, std::memory_order_relaxed);

        // Issue #205: env-walk. The evaluator walks env_frames_
        // (O(frames) SoA linear pass, replacing the old pointer
        // chase). Produces pair/closure index lists. We then
        // call mark_env_frame_roots to set the bits. The walk
        // runs AFTER mark_from_roots so the mark vectors are
        // already sized (mark_env_frame_roots is a no-op if the
        // vectors aren't sized yet). The walk is also additive:
        // it doesn't replace the explicit root sources, it
        // ADDS to them (env chains are not always reachable from
        // the root sources, e.g., a frame that's only reachable
        // through another frame's parent_id).
        if (env_walk_fn_) {
            EnvFrameRoots env_roots;
            env_walk_fn_(env_roots);
            mark_env_frame_roots(env_roots.pair_roots, env_roots.closure_roots);
        }

        // ── Phase 4: Sweep (skeleton) ─────────────────────
        auto sweep_start = std::chrono::steady_clock::now();
        auto sweep_result = sweep();
        auto sweep_end = std::chrono::steady_clock::now();
        auto sweep_us =
            std::chrono::duration_cast<std::chrono::microseconds>(sweep_end - sweep_start).count();
        metrics_.sweep_us.fetch_add(sweep_us, std::memory_order_relaxed);
        metrics_.strings_freed.fetch_add(static_cast<int64_t>(sweep_result.strings_freed),
                                         std::memory_order_relaxed);
        metrics_.pairs_freed.fetch_add(static_cast<int64_t>(sweep_result.pairs_freed),
                                       std::memory_order_relaxed);
        metrics_.closures_freed.fetch_add(static_cast<int64_t>(sweep_result.closures_freed),
                                          std::memory_order_relaxed);

    } // end ScopedSafepoint — clear active flag before resume

    // ── Phase 1e: Resume workers ──────────────────────
    scheduler_->resume_from_gc();

    auto end = std::chrono::steady_clock::now();
    auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    // Update metrics
    auto gc_idx = metrics_.gc_count.fetch_add(1, std::memory_order_relaxed);
    metrics_.total_pause_us.fetch_add(total_us, std::memory_order_relaxed);
    metrics_.max_pause_us.store(
        std::max(metrics_.max_pause_us.load(std::memory_order_relaxed), total_us),
        std::memory_order_relaxed);
    metrics_.safepoint_wait_us.fetch_add(safepoint_us, std::memory_order_relaxed);

    reset_alloc_counter();
    gc_in_progress_.store(false, std::memory_order_release);

    return true;
}

// ── mark_from_roots — set mark bits from root set (Phase 3) ─

void GCCollector::mark_from_roots(const GCRootSet& roots, size_t string_heap_size,
                                  size_t pairs_size, size_t closures_size) {
    // Size the mark vectors
    // If the caller passed 0 for sizes, we use the max root index + 1
    size_t s_size = string_heap_size > 0 ? string_heap_size : 0;
    size_t p_size = pairs_size > 0 ? pairs_size : 0;
    size_t c_size = closures_size > 0 ? closures_size : 0;

    // If no sizes given, compute from roots
    if (s_size == 0) {
        for (auto idx : roots.string_roots)
            if (static_cast<size_t>(idx) >= s_size)
                s_size = static_cast<size_t>(idx) + 1;
    }
    if (p_size == 0) {
        for (auto idx : roots.pair_roots)
            if (static_cast<size_t>(idx) >= p_size)
                p_size = static_cast<size_t>(idx) + 1;
    }
    if (c_size == 0) {
        for (auto idx : roots.closure_roots)
            if (static_cast<size_t>(idx) >= c_size)
                c_size = static_cast<size_t>(idx) + 1;
    }

    // Initialize mark vectors
    if (s_size > 0) {
        string_marks_.resize(s_size);
        string_marks_.clear_all();
        for (auto idx : roots.string_roots)
            string_marks_.set(static_cast<size_t>(idx));
    }
    if (p_size > 0) {
        pair_marks_.resize(p_size);
        pair_marks_.clear_all();
        for (auto idx : roots.pair_roots)
            pair_marks_.set(static_cast<size_t>(idx));
    }
    if (c_size > 0) {
        closure_marks_.resize(c_size);
        closure_marks_.clear_all();
        for (auto idx : roots.closure_roots)
            closure_marks_.set(static_cast<size_t>(idx));
    }
}

// Issue #172 / #204: env_frame_roots walk integration.
// The caller (evaluator) walks the env_frames_ SoA arena
// (O(frames)) and produces 3 index lists (cells, pairs,
// closures reachable through env parent chains). The GC
// marks each list's indices in the corresponding MarkBitVector.
//
// This replaces the old pointer-chasing Env* walk with a
// single linear pass over the env_frames_ arena, which is
// the issue's "3-5x faster mark phase" benefit for large
// workspaces. The decoupling (caller does the walk) keeps
// the GC's surface area narrow — it doesn't need to know
// EnvFrame's layout.
void GCCollector::mark_env_frame_roots(const std::vector<int64_t>& pair_roots,
                                       const std::vector<int64_t>& closure_roots) {
    // The mark vectors are sized lazily by mark_from_roots.
    // If the caller invokes mark_env_frame_roots BEFORE
    // mark_from_roots (unlikely but possible), the set()
    // calls below silently no-op because the bits_ vector
    // hasn't been resized yet. The caller should typically
    // call mark_from_roots first (to size the mark vectors
    // based on heap sizes) and then mark_env_frame_roots
    // (to add the env-walk roots). Either order is safe.
    for (auto idx : pair_roots) {
        if (idx >= 0)
            pair_marks_.set(static_cast<size_t>(idx));
    }
    for (auto idx : closure_roots) {
        if (idx >= 0)
            closure_marks_.set(static_cast<size_t>(idx));
    }
}

GCSweepResult GCCollector::sweep() {
    GCSweepResult result;

    // If a sweep callback is registered, let the evaluator compact its heaps
    if (sweep_fn_) {
        GCSweepBuffers bufs;
        bufs.string_marks = &string_marks_;
        bufs.pair_marks = &pair_marks_;
        bufs.closure_marks = &closure_marks_;
        result = sweep_fn_(bufs);
    } else {
        // No callback: just count dead entries
        if (string_marks_.size() > 0)
            result.strings_freed = string_marks_.count_dead();
        if (pair_marks_.size() > 0)
            result.pairs_freed = pair_marks_.count_dead();
        if (closure_marks_.size() > 0)
            result.closures_freed = closure_marks_.count_dead();
    }

    // Clear mark state for next cycle
    string_marks_ = MarkBitVector();
    pair_marks_ = MarkBitVector();
    closure_marks_ = MarkBitVector();

    return result;
}

} // namespace aura::serve
