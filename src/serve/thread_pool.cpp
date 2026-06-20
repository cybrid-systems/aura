// serve/thread_pool.cpp — Thread pool implementation (jthread-based)
#include "thread_pool.h"
#include <unistd.h>
#include <cstdio>
#include <cerrno>
#include <cstring>

namespace aura::serve {

ThreadPool::ThreadPool(size_t num_threads) {
    if (num_threads == 0)
        num_threads = 1;
    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this](std::stop_token st) { worker_loop(std::move(st)); });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
    // jthread destructors automatically join
}

void ThreadPool::shutdown() {
    for (auto& t : workers_)
        t.request_stop();
    cv_.notify_all();
}

void ThreadPool::enqueue(std::function<void()> fn, int wake_evfd) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        queue_.push_back(Task{std::move(fn), wake_evfd});
        ++pending_count_;
    }
    cv_.notify_one();
}

size_t ThreadPool::pending() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return pending_count_;
}

void ThreadPool::worker_loop(std::stop_token st) {
    while (!st.stop_requested()) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Wait until there's work or we're asked to stop
            cv_.wait(lock, st, [this]() { return !queue_.empty(); });
            if (st.stop_requested() && queue_.empty())
                return;
            if (queue_.empty())
                continue; // spurious wake, loop back
            task = std::move(queue_.front());
            queue_.pop_front();
        }

        // Execute the blocking operation
        try {
            task.fn();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "thread_pool: task threw: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "thread_pool: task threw unknown exception\n");
        }

        // Signal completion via eventfd
        if (task.wake_evfd >= 0) {
            uint64_t val = 1;
            auto n = ::write(task.wake_evfd, &val, sizeof(val));
            if (n == -1) {
                std::fprintf(stderr, "thread_pool: write wake_evfd %d failed: %s\n", task.wake_evfd,
                             std::strerror(errno));
            }
        }

        {
            std::unique_lock<std::mutex> lock(mutex_);
            --pending_count_;
        }
    }
}

} // namespace aura::serve
