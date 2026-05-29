// serve/scheduler.cpp — Multi-threaded fiber scheduler
#include "scheduler.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <system_error>

namespace aura::serve {

// ── Constructor ───────────────────────────────────────

Scheduler::Scheduler(int num_workers) {
    // Default: hardware concurrency, capped at reasonable range
    if (num_workers <= 0) {
        num_workers = static_cast<int>(std::thread::hardware_concurrency());
        if (num_workers < 2) num_workers = 2;
        if (num_workers > 16) num_workers = 16;
    }
    num_workers_ = num_workers;

    // Create epoll instance
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ == -1)
        throw std::system_error(errno, std::generic_category(), "scheduler epoll_create");

    // Register stdin (fd 0) with edge-triggered mode
    stdin_fd_ = STDIN_FILENO;
    struct epoll_event ee;
    ee.events = EPOLLIN | EPOLLET;
    ee.data.ptr = nullptr;  // nullptr = stdin event
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, stdin_fd_, &ee) == -1)
        throw std::system_error(errno, std::generic_category(), "scheduler epoll_ctl stdin");

    // Also register the scheduler's own wakeup eventfd for fast shutdown
    // (self-wake from stop())

    // Create workers
    workers_.reserve(num_workers_);
    for (int i = 0; i < num_workers_; ++i) {
        auto w = std::make_unique<WorkerThread>(i, this);
        workers_.push_back(std::move(w));
    }
}

// ── Destructor ───────────────────────────────────────

Scheduler::~Scheduler() {
    stop();
    for (auto& w : workers_) {
        w->join();
    }
    workers_.clear();
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
}

// ── spawn — create a new fiber ────────────────────────
// Creates the fiber and assigns it to a worker (round-robin).

Fiber* Scheduler::spawn(Fiber::Func func, size_t stack_size) {
    auto fb = std::make_unique<Fiber>(std::move(func), stack_size);
    auto* ptr = fb.get();

    // Register eventfd with epoll
    struct epoll_event ee;
    ee.events = EPOLLIN;
    ee.data.ptr = ptr;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, ptr->eventfd(), &ee);

    {
        std::lock_guard<std::mutex> lock(wait_map_mutex_);
        wait_map_[ptr->eventfd()] = ptr;
    }

    // Store fiber for lifetime management
    // We need to keep it alive — use a global list or let workers own it
    // For now, fibers are owned by the scheduler (simplest)
    static std::mutex s_fibers_mutex;
    static std::vector<std::unique_ptr<Fiber>> s_fibers;
    {
        std::lock_guard<std::mutex> lock(s_fibers_mutex);
        s_fibers.push_back(std::move(fb));
    }

    // Assign to a worker
    int wid = next_worker_id();
    workers_[wid]->enqueue(ptr);

    return ptr;
}

// ── stop ─────────────────────────────────────────────

void Scheduler::stop() {
    running_.store(false, std::memory_order_release);
    for (auto& w : workers_) {
        w->stop();
    }
}

// ── register_event_fiber ─────────────────────────────

void Scheduler::register_event_fiber(int eventfd, Fiber* fiber) {
    struct epoll_event ee;
    ee.events = EPOLLIN;
    ee.data.ptr = fiber;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, eventfd, &ee);

    std::lock_guard<std::mutex> lock(wait_map_mutex_);
    wait_map_[eventfd] = fiber;
}

// ── unregister_fiber ─────────────────────────────────

void Scheduler::unregister_fiber(int eventfd) {
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, eventfd, nullptr);
    std::lock_guard<std::mutex> lock(wait_map_mutex_);
    wait_map_.erase(eventfd);
}

// ── on_fiber_done — called by worker when fiber completes ──
// Removes the fiber's eventfd from epoll and cleans up wait map.

void Scheduler::on_fiber_done(Fiber* fiber) {
    if (!fiber) return;
    int evfd = fiber->eventfd();
    if (evfd >= 0) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, evfd, nullptr);
        std::lock_guard<std::mutex> lock(wait_map_mutex_);
        wait_map_.erase(evfd);
    }
}

// ── has_waiting_fibers — check epoll wait map ─────────

bool Scheduler::has_waiting_fibers() const {
    std::lock_guard<std::mutex> lock(wait_map_mutex_);
    for (auto& [evfd, fiber] : wait_map_) {
        if (fiber && fiber->state() == FiberState::Waiting)
            return true;
    }
    return false;
}

// ── worker — access worker by index ──────────────────

WorkerThread* Scheduler::worker(int idx) {
    if (idx < 0 || idx >= num_workers_) return nullptr;
    return workers_[idx].get();
}

// ── next_worker_id — round-robin worker assignment ────

int Scheduler::next_worker_id() {
    int id = next_worker_.fetch_add(1, std::memory_order_acq_rel);
    return id % num_workers_;
}

// ── run — main IO event loop ─────────────────────────
//
// The IO thread (main thread) runs the epoll event loop.
// It monitors:
//   - stdin (fd 0): new commands for session fibers
//   - Worker wake eventfds: not used directly here since workers
//     self-wake via their own eventfd
//   - Fiber eventfds: when a fiber in Waiting state gets woken
//     (e.g., by send/recv or thread pool completion), the IO thread
//     enqueues it to a worker.

void Scheduler::run() {
    g_scheduler = this;

    // Start all workers
    for (auto& w : workers_) {
        w->start();
    }

    struct epoll_event events[64];

    while (running_.load(std::memory_order_acquire)) {
        // Block on epoll_wait for events
        // Timeout: check running_ periodically (in case all fibers are busy)
        int n = ::epoll_wait(epoll_fd_, events, 64, 1000);

        if (!running_.load(std::memory_order_acquire))
            break;

        if (n < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "scheduler: epoll_wait failed: %s\n",
                         std::strerror(errno));
            break;
        }

        // Process events
        for (int i = 0; i < n; ++i) {
            if (events[i].data.ptr == nullptr) {
                // stdin event — wake the stdin fiber AND all waiting fibers
                if (stdin_fiber_) {
                    int wid = next_worker_id();
                    workers_[wid]->enqueue(stdin_fiber_);
                }
                // Always wake all waiting fibers on stdin activity
                // (they may be waiting for stdin data from the reader)
                {
                    std::lock_guard<std::mutex> lock(wait_map_mutex_);
                    for (auto& [evfd, fiber] : wait_map_) {
                        if (fiber && fiber->state() == FiberState::Waiting) {
                            int wid = next_worker_id();
                            workers_[wid]->enqueue(fiber);
                        }
                    }
                }
            } else {
                // Fiber eventfd event
                auto* fiber = static_cast<Fiber*>(events[i].data.ptr);
                if (!fiber || fiber->is_done()) continue;

                // Drain the eventfd (read the 8-byte counter)
                uint64_t val;
                ::read(fiber->eventfd(), &val, sizeof(val));

                // Enqueue to a worker for resumption
                int wid = next_worker_id();
                workers_[wid]->enqueue(fiber);
            }
        }

        // Check if all fibers are done
        {
            std::lock_guard<std::mutex> lock(wait_map_mutex_);
            if (wait_map_.empty()) {
                // No fibers in epoll — check if any still running
                bool all_done = true;
                for (auto& w : workers_) {
                    if (w->queue_size() > 0 || w->is_running()) {
                        all_done = false;
                        break;
                    }
                }
                // Also check if there are waiting fibers
                if (!all_done) continue;
                // Double check with the running flag
                if (!running_.load(std::memory_order_acquire))
                    break;
                // Give workers a moment to finish
                continue;
            }
        }
    }

    // Stop all workers
    for (auto& w : workers_) {
        w->stop();
    }
    for (auto& w : workers_) {
        w->join();
    }

    g_scheduler = nullptr;
}

} // namespace aura::serve
