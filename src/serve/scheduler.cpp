// serve/scheduler.cpp — Fiber scheduler + epoll event loop
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

Scheduler::Scheduler() {
    // Create epoll instance
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ == -1)
        throw std::system_error(errno, std::generic_category(), "scheduler epoll_create");

    // Register stdin (fd 0) with edge-triggered mode.
    // Edge-triggered: only fires ONCE when new data arrives.
    // After consuming all data (read returns EAGAIN), no re-fire until new data.
    stdin_fd_ = STDIN_FILENO;
    struct epoll_event ee;
    ee.events = EPOLLIN | EPOLLET;
    ee.data.ptr = nullptr;  // nullptr = stdin event
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, stdin_fd_, &ee) == -1)
        throw std::system_error(errno, std::generic_category(), "scheduler epoll_ctl stdin");
}

// ── Destructor ───────────────────────────────────────

Scheduler::~Scheduler() {
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
}

// ── spawn — create a new fiber ────────────────────────

Fiber* Scheduler::spawn(Fiber::Func func, size_t stack_size) {
    auto fb = std::make_unique<Fiber>(std::move(func), stack_size);
    auto* ptr = fb.get();

    // Register its eventfd
    struct epoll_event ee;
    ee.events = EPOLLIN;
    ee.data.ptr = ptr;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, ptr->eventfd(), &ee);

    wait_map_[ptr->eventfd()] = ptr;
    fibers_.push_back(std::move(fb));
    enqueue(ptr);
    return ptr;
}

// ── enqueue — add fiber to ready queue ────────────────

void Scheduler::enqueue(Fiber* f) {
    if (f->state() != FiberState::Done) {
        f->set_state(FiberState::Ready);
        ready_queue_.push_back(f);
    }
}

// ── register_fiber_event ──────────────────────────────

void Scheduler::register_fiber_event(Fiber* fiber) {
    struct epoll_event ee;
    ee.events = EPOLLIN;
    ee.data.ptr = fiber;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fiber->eventfd(), &ee);
    wait_map_[fiber->eventfd()] = fiber;
}

// ── run — main event loop ─────────────────────────────

void Scheduler::run() {
    g_scheduler = this;  // Set global for Fiber::yield()

    struct epoll_event events[64];

    while (running_) {
        // Phase 1: Drain ready queue — resume all Ready fibers
        size_t iterations = 0;
        const size_t MAX_ITER = 10000;  // safety: prevent fiber-spin lock

        while (!ready_queue_.empty() && iterations < MAX_ITER) {
            auto* fiber = ready_queue_.front();
            ready_queue_.pop_front();
            ++iterations;

            if (fiber->is_done()) {
                // Remove from epoll
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fiber->eventfd(), nullptr);
                wait_map_.erase(fiber->eventfd());
                continue;
            }

            // Resume the fiber — it may yield back or complete

            fiber->resume();

            if (fiber->is_done()) {
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fiber->eventfd(), nullptr);
                wait_map_.erase(fiber->eventfd());
            } else if (fiber->state() == FiberState::Ready) {
                // Yielded without waiting — re-enqueue
                ready_queue_.push_back(fiber);
            }
            // Waiting: in epoll wait map, scheduler resumes on event
        }

        // Phase 2: Check if any fibers remain alive
        bool any_alive = false;
        for (auto& f : fibers_) {
            if (!f->is_done()) {
                any_alive = true;
                break;
            }
        }
        if (!any_alive) break;

        // Phase 3: All fibers Waiting or Done — block on epoll
        if (ready_queue_.empty()) {
            int n = ::epoll_wait(epoll_fd_, events, 64, -1);

            for (int i = 0; i < n; ++i) {
                if (events[i].data.ptr == nullptr) {
                    // stdin event — wake the stdin fiber if set
        
                    if (stdin_fiber_) {
            
                        enqueue(stdin_fiber_);
                    } else {
                        // Fallback: wake all waiting fibers
                        for (auto& f : fibers_) {
                            if (f->state() == FiberState::Waiting)
                                enqueue(f.get());
                        }
                    }
                } else {
                    // eventfd event — drain and enqueue the target fiber
                    auto* fiber = static_cast<Fiber*>(events[i].data.ptr);
        
                    uint64_t val;
                    ::read(fiber->eventfd(), &val, sizeof(val));
                    if (fiber->state() == FiberState::Waiting || fiber->state() == FiberState::Ready) {
                        enqueue(fiber);
                    }
                }
            }
        }
    }

    g_scheduler = nullptr;
    fibers_.clear();
}

} // namespace aura::serve
