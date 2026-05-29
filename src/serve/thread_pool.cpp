// serve/thread_pool.cpp — Thread pool implementation
#include "thread_pool.h"
#include <unistd.h>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <system_error>

namespace aura::serve {

ThreadPool::ThreadPool(size_t num_threads) {
    if (num_threads == 0) num_threads = 1;
    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&ThreadPool::worker_loop, this, static_cast<int>(i));
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stop_) return;
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
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

void ThreadPool::worker_loop(int /*id*/) {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty())
                return;
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
                std::fprintf(stderr, "thread_pool: write wake_evfd %d failed: %s\n",
                             task.wake_evfd, std::strerror(errno));
            }
        }

        {
            std::unique_lock<std::mutex> lock(mutex_);
            --pending_count_;
        }
    }
}

} // namespace aura::serve
