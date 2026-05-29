// serve/thread_pool.h — Thread pool for offloading blocking operations
#ifndef AURA_SERVE_THREAD_POOL_H
#define AURA_SERVE_THREAD_POOL_H

#include <functional>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <utility>
#include <stop_token>

namespace aura::serve {

// ── ThreadPool ─────────────────────────────────────────────
// Fixed-size thread pool for blocking operations.
// Uses std::jthread for automatic RAII join on destruction.
//
// Each enqueued task runs on a background thread and signals
// completion by writing to the provided eventfd (which should
// be registered with the scheduler's epoll).
class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads = 4);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Enqueue a blocking task. fn runs on a background thread.
    // When fn completes, 1 is written to wake_evfd.
    void enqueue(std::function<void()> fn, int wake_evfd);

    // Pending (enqueued + running) task count
    size_t pending() const;

    // Request all workers stop (called by destructor automatically via jthread)
    void shutdown();

private:
    void worker_loop(std::stop_token st);

    struct Task {
        std::function<void()> fn;
        int wake_evfd;
    };

    std::vector<std::jthread> workers_;
    std::deque<Task> queue_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    size_t pending_count_ = 0;
};

} // namespace aura::serve

#endif // AURA_SERVE_THREAD_POOL_H
