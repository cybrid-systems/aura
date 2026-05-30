// serve/gc_coordinator.cpp — GC coordinator implementation
#include "gc_coordinator.h"
#include "scheduler.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>

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
    if (!gc_in_progress_.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel))
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

// ── collect_roots — enumerate all registered root sources ──

void GCCollector::collect_roots(GCRootSet& out) {
    out.clear();
    std::lock_guard<std::mutex> lock(root_sources_mutex_);
    for (auto& [wid, fn] : root_sources_) {
        if (fn) fn(out);
    }
}

// ── collect — full GC cycle ────────────────────────────

bool GCCollector::collect() {
    if (!gc_in_progress_.load(std::memory_order_acquire))
        return false;

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

    auto safepoint_us = std::chrono::duration_cast<std::chrono::microseconds>(
        safepoint_end - safepoint_start).count();

    // ── Phase 2: Collect roots from all registered sources ──
    auto roots_start = std::chrono::steady_clock::now();
    GCRootSet roots;
    collect_roots(roots);
    auto roots_end = std::chrono::steady_clock::now();
    auto roots_us = std::chrono::duration_cast<std::chrono::microseconds>(
        roots_end - roots_start).count();

    metrics_.root_count.store(
        static_cast<int64_t>(roots.string_roots.size() + roots.pair_roots.size()
                             + roots.closure_roots.size()
                             + roots.fiber_result_roots.size()
                             + roots.workspace_roots.size()),
        std::memory_order_relaxed);
    metrics_.root_collect_us.fetch_add(roots_us, std::memory_order_relaxed);

    // ── Phase 3: Parallel mark (skeleton) ─────────────
    // TODO: Phase 3 — tri-color marking

    // ── Phase 4: Sweep (skeleton) ─────────────────────
    // TODO: Phase 4 — reclaim temp_arena, compact heaps

    // ── Phase 1e: Resume workers ──────────────────────
    scheduler_->resume_from_gc();

    auto end = std::chrono::steady_clock::now();
    auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(
        end - start).count();

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

} // namespace aura::serve
