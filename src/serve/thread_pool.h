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

namespace aura::serve {

// ── ThreadPool ─────────────────────────────────────────────
// Fixed-size thread pool for blocking operations (compilation,
// type-checking, file I/O). Each enqueued task runs on a background
// thread and signals completion by writing to the provided eventfd.
//
// The eventfd should be registered with the scheduler's epoll so
// that the waiting fiber can be woken.
//
// Usage:
//   ThreadPool pool(4);
//   int evfd = make_eventfd();
//   epoll_ctl(scheduler_epoll, EPOLL_CTL_ADD, evfd, &target_fiber);
//   pool.enqueue(fn, evfd);
//   // fiber: set Waiting → yield
//   // thread: fn() → write(evfd, 1)
//   // epoll: eventfd readable → scheduler enqueues fiber
//
class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads = 4);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Enqueue a blocking task. fn runs on a background thread.
    // When fn completes, 1 is written to wake_evfd.
    // The caller owns the wake_evfd lifecycle (create before enqueue,
    // close after event is consumed).
    void enqueue(std::function<void()> fn, int wake_evfd);

    // Number of pending (enqueued + running) tasks
    size_t pending() const;

    // Stop all worker threads (called by destructor)
    void shutdown();

private:
    void worker_loop(int id);

    struct Task {
        std::function<void()> fn;
        int wake_evfd;
    };

    std::vector<std::thread> workers_;
    std::deque<Task> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    size_t pending_count_ = 0;
};

} // namespace aura::serve

#endif // AURA_SERVE_THREAD_POOL_H
