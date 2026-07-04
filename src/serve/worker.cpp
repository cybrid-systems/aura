// serve/worker.cpp — Worker thread with work-stealing
#include "worker.h"
#include "scheduler.h"
#include "aura_platform.h"

#include <unistd.h>

import std;
#if AURA_HAVE_EVENTFD
#include <sys/eventfd.h>
#endif

namespace aura::serve {

extern "C" void aura_evaluator_probe_linear_on_steal();
extern "C" void aura_evaluator_bump_steal_deferred_violation();

// ── Constructor ───────────────────────────────────────

WorkerThread::WorkerThread(int id, Scheduler* scheduler)
    : id_(id)
    , scheduler_(scheduler) {

    // Create wake eventfd
#if AURA_HAVE_EVENTFD
    wake_evfd_ = ::eventfd(0, EFD_NONBLOCK);
    if (wake_evfd_ == -1)
        throw std::system_error(errno, std::generic_category(),
                                "worker[" + std::to_string(id) + "] eventfd");
#else
    wake_evfd_ = -1; // macOS: no eventfd
#endif
}

// ── Destructor ────────────────────────────────────────

WorkerThread::~WorkerThread() {
    stop();
    if (wake_evfd_ >= 0) {
        ::close(wake_evfd_);
        wake_evfd_ = -1;
    }
}

// ── start — launch the worker thread ──────────────────

void WorkerThread::start() {
    running_.store(true, std::memory_order_release);
    thread_ = std::jthread([this](std::stop_token) { run(); });
}

// ── stop — request graceful stop ──────────────────────

void WorkerThread::stop() {
    running_.store(false, std::memory_order_release);
    // Wake the worker so it can exit
    if (wake_evfd_ >= 0) {
        uint64_t val = 1;
        ::write(wake_evfd_, &val, sizeof(val));
    }
    wake_cv_.notify_all();
}

// ── join — wait for thread to finish ──────────────────

void WorkerThread::join() {
    if (thread_.joinable())
        thread_.join();
}

// ── enqueue — add fiber to local queue ────────────────
// Thread-safe: push into the work-stealing deque.
// The push() operation is "owner-only" in Chase-Lev, but the
// scheduler (IO thread) is the one calling enqueue from outside.
// This is safe because:
//   1. push() only writes to buffer_ and increments bottom_
//   2. The stealers only read the top
//   3. The only conflict is between push and steal, which is handled
//      by the Chase-Lev memory ordering (release fence in push,
//      seq_cst fence in steal)

void WorkerThread::enqueue(Fiber* fiber) {
    if (!fiber || fiber->is_done())
        return;

    local_queue_.push(fiber);
    pending_.fetch_add(1, std::memory_order_release);

    if (worker_metrics_) {
        worker_metrics_->local_pushes.fetch_add(1, std::memory_order_relaxed);
    }

    // Wake the worker if it was sleeping
    if (wake_evfd_ >= 0) {
        uint64_t val = 1;
        ::write(wake_evfd_, &val, sizeof(val));
    }
    wake_cv_.notify_one();
}

// ── notify_fiber_done — report completed fiber ────────

void WorkerThread::notify_fiber_done(Fiber* fiber) {
    if (scheduler_) {
        scheduler_->on_fiber_done(fiber);
    }
}

// Issue #119: per-worker fiber registry. The scheduler's
// enqueue() registers the fiber here, and the worker's
// on_fiber_done path (via notify_fiber_done) unregisters it.
// The registry is the source of truth for fiber_by_id.

void WorkerThread::register_fiber(Fiber* fiber) {
    if (!fiber)
        return;
    std::lock_guard<std::mutex> lock(fiber_registry_mutex_);
    fiber_registry_[fiber->id()] = fiber;
}

void WorkerThread::unregister_fiber(Fiber* fiber) {
    if (!fiber)
        return;
    std::lock_guard<std::mutex> lock(fiber_registry_mutex_);
    fiber_registry_.erase(fiber->id());
}

Fiber* WorkerThread::fiber_by_id(std::uint64_t fiber_id) const {
    std::lock_guard<std::mutex> lock(fiber_registry_mutex_);
    auto it = fiber_registry_.find(fiber_id);
    if (it == fiber_registry_.end())
        return nullptr;
    return it->second;
}

// ── try_steal_from — attempt to steal a fiber ─────────

bool WorkerThread::try_steal_from(WorkerThread* victim) {
    if (!victim || victim == this)
        return false;

    // Try to steal a fiber from the victim's deque.
    // The deque only contains fibers that yielded (Explicit/MutationBoundary),
    // but we also check is_stealable() as a safety measure against stale fibers
    // that may have been mutated after being enqueued.
    for (int attempt = 0; attempt < 3; ++attempt) {
        Fiber* stolen = victim->try_steal();
        if (!stolen)
            break;

        // Skip fibers pinned to another worker (affinity)
        if (stolen->affinity() >= 0 && stolen->affinity() != id()) {
            // Pinned to a different worker — put it back
            victim->enqueue(stolen);
            continue;
        }

        // Issue #588: defer steal when victim yielded at
        // MutationBoundary with active inner/outer guard
        // (per-fiber stack depth > 0).
        if (stolen->is_stealable() && stolen->is_at_mutation_boundary_safe()) {
            const int pri = fiber_steal_priority(stolen);
            if (pri >= 2) {
                metrics::adaptive_steal_stats().outermost_preferred.fetch_add(
                    1, std::memory_order_relaxed);
                if (pri >= 3) {
                    metrics::adaptive_steal_stats().llm_tail_reductions.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            stolen->bump_steal_success();
            aura_evaluator_probe_linear_on_steal();
            local_queue_.push(stolen);
            return true;
        }

        if (stolen->is_stealable() &&
            stolen->last_yield_reason() == YieldReason::MutationBoundary) {
            stolen->bump_steal_deferred_mutation_boundary();
            aura_evaluator_bump_steal_deferred_violation();
            metrics::adaptive_steal_stats().global_deferred_mutation_total.fetch_add(
                1, std::memory_order_relaxed);
            metrics::adaptive_steal_stats().mutation_bias_hits.fetch_add(1,
                                                                         std::memory_order_relaxed);
        }

        // Not stealable — put it back on the victim's queue.
        // This could happen if the fiber state changed after it was enqueued.
        // Give up after a few attempts to avoid infinite loop.
        victim->enqueue(stolen);
    }
    return false;
}

// ── run — the worker's main dispatch loop ─────────────
//
// Algorithm:
//   1. Drain local queue (pop LIFO)
//   2. When empty, try to steal from a random worker
//   3. If steal succeeds, go to step 1
//   4. If steal fails repeatedly, sleep on condition variable
//   5. Wake when new fibers arrive (enqueue/eventfd)

void WorkerThread::run() {
    // Set up thread-local worker context for fiber yield/resume
    g_worker_ctx = &ctx_;
    ctx_.gc_state = &gc_state_; // link GC state for safepoint check

    // Grab metrics pointer (set by scheduler before start)
    auto* my_metrics = worker_metrics_;

    const size_t MAX_ITER_PER_ROUND = 1000;

    while (running_.load(std::memory_order_acquire)) {
        auto cycle_start = std::chrono::steady_clock::now();
        bool was_busy = false;

        // ── Phase 1: drain local queue (LIFO) ───────
        size_t iter = 0;
        while (iter < MAX_ITER_PER_ROUND) {
            Fiber* fiber = local_queue_.pop();
            if (!fiber)
                break;
            ++iter;
            was_busy = true;

            if (my_metrics) {
                my_metrics->local_pops.fetch_add(1, std::memory_order_relaxed);
            }

            if (fiber->is_done()) {
                pending_.fetch_sub(1, std::memory_order_release);
                continue;
            }

            // Resume the fiber — runs until yield() or completion.
            //
            // Issue #115: track the "running fiber" count so the
            // GC coordinator can wait for currently-running
            // fibers to arrive at the safepoint. Increment
            // BEFORE resume (in case the GC requests a
            // safepoint while we're blocked in resume()) and
            // decrement AFTER (so the count goes back to 0
            // when the worker is back in its dispatch loop).
            gc_state_.running_fiber_count.fetch_add(1, std::memory_order_acq_rel);
            if (my_metrics) {
                my_metrics->fibers_executed.fetch_add(1, std::memory_order_relaxed);
            }
            fiber->resume();
            gc_state_.running_fiber_count.fetch_sub(1, std::memory_order_acq_rel);

            // After resume: fiber either yielded or finished
            if (fiber->is_done()) {
                pending_.fetch_sub(1, std::memory_order_release);
                notify_fiber_done(fiber);
                continue;
            }

            auto fb_state = fiber->state();
            if (fb_state == FiberState::Waiting) {
                // Yielded for event — leave off queue, epoll will wake
                pending_.fetch_sub(1, std::memory_order_release);
                if (my_metrics) {
                    my_metrics->fibers_waiting.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                // Non-Waiting yield: keep scheduling
                local_queue_.push(fiber);
                // pending_ unchanged
                if (my_metrics) {
                    my_metrics->fibers_yielded.fetch_add(1, std::memory_order_relaxed);
                    my_metrics->local_pushes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // ── Phase 1.5: record queue depth ───────────
        if (my_metrics) {
            size_t qd = local_queue_.size_approx();
            my_metrics->record_qdepth(qd);
        }

        // ── Phase 2: check pending status ───────────
        bool any_pending = (pending_.load(std::memory_order_acquire) > 0);
        bool local_nonempty = !local_queue_.empty_approx();
        if (!any_pending && !local_nonempty) {
            if (scheduler_ && scheduler_->has_waiting_fibers()) {
                any_pending = true;
            }
        }

        // ── Phase 3: try to steal ───────────────────
        steal_budget_.apply_deferred_pressure(
            metrics::adaptive_steal_stats().global_deferred_mutation_total.load(
                std::memory_order_relaxed));
        if (!local_nonempty && any_pending && steal_budget_.should_steal()) {
            bool stole = false;

            // Try to steal from random workers
            if (scheduler_) {
                int n_workers = scheduler_->num_workers();
                if (n_workers > 1) {
                    const int steal_tries =
                        metrics::adaptive_steal_stats().global_deferred_mutation_total.load(
                            std::memory_order_relaxed) > 10
                            ? 5
                            : 3;
                    for (int attempt = 0; attempt < steal_tries; ++attempt) {
                        int victim_id = std::rand() % n_workers;
                        if (victim_id == id_)
                            continue;
                        auto* victim = scheduler_->worker(victim_id);
                        if (my_metrics) {
                            my_metrics->steal_attempts.fetch_add(1, std::memory_order_relaxed);
                        }
                        if (try_steal_from(victim)) {
                            stole = true;
                            if (my_metrics) {
                                my_metrics->steal_successes.fetch_add(1, std::memory_order_relaxed);
                            }
                            break;
                        }
                    }
                }
            }

            if (stole) {
                steal_budget_.record_success();
                was_busy = true;
                continue; // go back to Phase 1
            } else {
                steal_budget_.record_failure();
            }
        }

        // ── Phase 4: wait for work ──────────────────
        if (!local_nonempty && !iter) {
            // Reset steal budget on wake
            steal_budget_.consecutive_failures = 0;

            // Record idle time
            if (my_metrics) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now - cycle_start).count();
                my_metrics->record_idle(elapsed);
            }

            // Drain the wake eventfd before sleeping
            {
                uint64_t val = 0;
                if (wake_evfd_ >= 0) {
                    ::read(wake_evfd_, &val, sizeof(val));
                }
                if (val > 0 && my_metrics) {
                    my_metrics->wake_events.fetch_add(val, std::memory_order_relaxed);
                }
            }

            // Wait on condition variable
            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
                return !local_queue_.empty_approx() || !running_.load(std::memory_order_acquire);
            });
        } else {
            // Record busy time
            if (my_metrics && was_busy) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now - cycle_start).count();
                my_metrics->record_busy(elapsed);
            }
        }
    }

    g_worker_ctx = nullptr;
    ctx_.gc_state = nullptr;
}

} // namespace aura::serve

extern "C" {
std::uint64_t aura_adaptive_steal_mutation_bias_hits() {
    return aura::serve::metrics::adaptive_steal_stats().mutation_bias_hits.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_adaptive_steal_outermost_preferred() {
    return aura::serve::metrics::adaptive_steal_stats().outermost_preferred.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_adaptive_steal_llm_tail_reductions() {
    return aura::serve::metrics::adaptive_steal_stats().llm_tail_reductions.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_adaptive_steal_deferred_pressure_boosts() {
    return aura::serve::metrics::adaptive_steal_stats().deferred_pressure_boosts.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_adaptive_steal_global_deferred_total() {
    return aura::serve::metrics::adaptive_steal_stats().global_deferred_mutation_total.load(
        std::memory_order_relaxed);
}
}
