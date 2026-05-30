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
        alloc_threshold_ = 100000;  // ~100k allocs → GC
    }
}

// ── request — trigger GC if threshold crossed ──────────

bool GCCollector::request() {
    if (gc_in_progress_.load(std::memory_order_acquire))
        return false;  // already running, don't re-request

    int64_t count = alloc_counter_.load(std::memory_order_relaxed);
    if (count < alloc_threshold_)
        return false;

    // Try to start GC
    bool expected = false;
    if (!gc_in_progress_.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel))
        return false;  // someone else got there first

    return true;  // GC should run
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
        std::fprintf(stderr, "gc: safepoint timeout after 100ms — forcing resume\n");
        // Force resume even if not all workers arrived
        scheduler_->resume_from_gc();
        gc_in_progress_.store(false, std::memory_order_release);
        return false;
    }

    auto safepoint_us = std::chrono::duration_cast<std::chrono::microseconds>(
        safepoint_end - safepoint_start).count();

    // ── Phase 2: Collect roots (skeleton) ─────────────
    // TODO: Phase 2 — iterate evaluator heaps, fiber stacks, etc.
    // Not yet implemented — just log the fact that we stopped.

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

    // Reset alloc counter for next GC cycle
    reset_alloc_counter();
    gc_in_progress_.store(false, std::memory_order_release);

    // Log GC event
    std::fprintf(stderr, "gc: #%ld complete — safepoint %ldμs total %ldμs\n",
                 static_cast<long>(gc_idx), static_cast<long>(safepoint_us),
                 static_cast<long>(total_us));

    return true;
}

} // namespace aura::serve
