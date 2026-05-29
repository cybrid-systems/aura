// serve/worker.cpp — Worker thread implementation
#include "worker.h"
#include "scheduler.h"

#include <sys/eventfd.h>
#include <unistd.h>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <system_error>

namespace aura::serve {

// ── Constructor ───────────────────────────────────────

WorkerThread::WorkerThread(int id, Scheduler* scheduler)
    : id_(id), scheduler_(scheduler) {

    // Create wake eventfd
    wake_evfd_ = ::eventfd(0, EFD_NONBLOCK);
    if (wake_evfd_ == -1)
        throw std::system_error(errno, std::generic_category(),
                                "worker[" + std::to_string(id) + "] eventfd");
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
    thread_ = std::jthread([this](std::stop_token) {
        run();
    });
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
// Thread-safe: called from scheduler (IO thread) or other workers.

void WorkerThread::enqueue(Fiber* fiber) {
    if (!fiber || fiber->is_done()) return;

    bool was_empty;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        was_empty = local_queue_.empty();
        local_queue_.push_back(fiber);
        pending_.fetch_add(1, std::memory_order_release);
    }

    if (was_empty && wake_evfd_ >= 0) {
        // Wake the worker if it was sleeping
        uint64_t val = 1;
        ::write(wake_evfd_, &val, sizeof(val));
    }
    wake_cv_.notify_one();
}

// ── queue_size — return approximate local queue length ─

size_t WorkerThread::queue_size() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return local_queue_.size();
}

// ── run — the worker's main dispatch loop ─────────────

void WorkerThread::run() {
    // Set up thread-local worker context
    g_worker_ctx = &ctx_;

    // Per-worker iteration budget to prevent starvation
    const size_t MAX_ITER_PER_ROUND = 1000;

    while (running_.load(std::memory_order_acquire)) {
        // ── Phase 1: drain local queue ──────────────
        size_t iter = 0;
        while (iter < MAX_ITER_PER_ROUND) {
            Fiber* fiber = nullptr;
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                if (local_queue_.empty()) break;
                fiber = local_queue_.front();
                local_queue_.pop_front();
            }
            ++iter;

            if (!fiber || fiber->is_done()) {
                pending_.fetch_sub(1, std::memory_order_release);
                continue;
            }

            // Resume the fiber — runs until yield() or completion
            fiber->resume();

            // After resume: fiber either yielded or finished
            if (fiber->is_done()) {
                pending_.fetch_sub(1, std::memory_order_release);
                // Remove from scheduler's event map
                if (scheduler_) {
                    scheduler_->on_fiber_done(fiber);
                }
                continue;
            }

            auto fb_state = fiber->state();
            if (fb_state == FiberState::Ready) {
                // Non-blocking yield — re-enqueue immediately
                std::lock_guard<std::mutex> lock(queue_mutex_);
                local_queue_.push_back(fiber);
                // pending_ unchanged
            } else if (fb_state == FiberState::Waiting) {
                // Yielded for event — leave off queue, epoll will wake
                pending_.fetch_sub(1, std::memory_order_release);
            }
            // Done handled above; Running is transient
        }

        // ── Phase 2: check if any fibers remain pending ──
        // Pending includes fibers in-Waiting (on epoll) + any queued
        bool any_pending = (pending_.load(std::memory_order_acquire) > 0);
        if (!any_pending) {
            // Check with scheduler if there are fibers in epoll wait
            if (scheduler_ && scheduler_->has_waiting_fibers()) {
                any_pending = true;
            }
        }

        // ── Phase 3: wait for work ──────────────────────
        if (!any_pending && !iter) {
            // No work — wait on condition variable
            std::mutex dummy;
            std::unique_lock<std::mutex> lock(dummy);
            // Also check our eventfd (scheduler might write to it)
            wake_cv_.wait_for(lock, std::chrono::milliseconds(100),
                              [this]() {
                                  return !local_queue_.empty() ||
                                         !running_.load(std::memory_order_acquire);
                              });
        }
    }

    g_worker_ctx = nullptr;
}

} // namespace aura::serve
