// offload/offload_pool.cpp — bounded, ownership-safe offload pool (#4881/#4882)
#include "offload_pool.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <utility>

namespace aura::offload {
namespace {

    std::uint64_t us_between(std::chrono::steady_clock::time_point a,
                             std::chrono::steady_clock::time_point b) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
    }

} // namespace

void OffloadPool::HistogramState::record(std::uint64_t us) noexcept {
    std::size_t bucket = 0;
    while (bucket + 1 < kLatencyBucketCount && us > kLatencyBucketBoundsUs[bucket])
        ++bucket;
    buckets[bucket].fetch_add(1, std::memory_order_relaxed);
    count.fetch_add(1, std::memory_order_relaxed);
    sum_us.fetch_add(us, std::memory_order_relaxed);
    auto cur = max_us.load(std::memory_order_relaxed);
    while (us > cur && !max_us.compare_exchange_weak(cur, us, std::memory_order_relaxed)) {
    }
}

LatencyHistogram OffloadPool::HistogramState::snapshot() const noexcept {
    LatencyHistogram out;
    for (std::size_t i = 0; i < kLatencyBucketCount; ++i)
        out.buckets[i] = buckets[i].load(std::memory_order_relaxed);
    out.count = count.load(std::memory_order_relaxed);
    out.sum_us = sum_us.load(std::memory_order_relaxed);
    out.max_us = max_us.load(std::memory_order_relaxed);
    return out;
}

std::size_t OffloadPool::default_threads() {
    auto hw = static_cast<std::size_t>(std::thread::hardware_concurrency());
    if (hw < 2)
        hw = 2;
    if (hw > 8)
        hw = 8;
    return hw;
}

std::size_t OffloadPool::read_env_size(const char* name, std::size_t fallback) {
    const char* raw = std::getenv(name);
    if (!raw || *raw == '\0')
        return fallback;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(raw, &end, 10);
    if (end == raw || value == 0)
        return fallback;
    return static_cast<std::size_t>(value);
}

OffloadPool::OffloadPool()
    : OffloadPool(Config{}) {}

OffloadPool::OffloadPool(Config cfg)
    : cfg_(cfg) {
    if (cfg_.threads == 0)
        cfg_.threads = default_threads();
    cfg_.threads = read_env_size("AURA_OFFLOAD_THREADS", cfg_.threads);
    cfg_.queue_cap = read_env_size("AURA_OFFLOAD_QUEUE_CAP", cfg_.queue_cap);
    if (cfg_.queue_cap == 0)
        cfg_.queue_cap = 1;
    workers_.reserve(cfg_.threads);
    for (std::size_t i = 0; i < cfg_.threads; ++i) {
        workers_.emplace_back([this](std::stop_token st) { worker_loop(std::move(st)); });
    }
}

OffloadPool::~OffloadPool() {
    shutdown();
}

SubmitStatus OffloadPool::try_submit(Job job, std::shared_ptr<const SnapshotInput> input,
                                     std::shared_ptr<Ticket> ticket, CompletionSink* sink) {
    if (!job || !input || !ticket || !sink)
        return SubmitStatus::ShuttingDown;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        rejected_shutdown_.fetch_add(1, std::memory_order_relaxed);
        ticket->status.store(Status::Rejected, std::memory_order_release);
        return SubmitStatus::ShuttingDown;
    }
    if (queue_.size() >= cfg_.queue_cap) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        rejected_queue_full_.fetch_add(1, std::memory_order_relaxed);
        ticket->status.store(Status::Rejected, std::memory_order_release);
        return SubmitStatus::QueueFull;
    }
    ticket->queued_at = std::chrono::steady_clock::now();
    queue_.push_back(Task{std::move(job), std::move(input), std::move(ticket), sink});
    submitted_.fetch_add(1, std::memory_order_relaxed);
    const auto depth = static_cast<std::uint64_t>(queue_.size());
    queue_depth_.store(depth, std::memory_order_relaxed);
    auto peak = queue_depth_peak_.load(std::memory_order_relaxed);
    while (depth > peak &&
           !queue_depth_peak_.compare_exchange_weak(peak, depth, std::memory_order_relaxed)) {
    }
    cv_.notify_one();
    return SubmitStatus::Accepted;
}

void OffloadPool::finish(const Task& task, Status status, std::string error) {
    const bool ran = task.ticket->status.load(std::memory_order_relaxed) == Status::Running;
    if (ran) {
        exec_hist_.record(us_between(task.ticket->started_at, std::chrono::steady_clock::now()));
    }
    std::chrono::steady_clock::time_point completed_at;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status != Status::Done) {
            task.ticket->out = Error{error.empty() ? std::string("cancelled") : std::move(error)};
        }
        completed_at = std::chrono::steady_clock::now();
        task.ticket->status.store(status, std::memory_order_release);
        if (ran) {
            std::erase(running_tickets_, task.ticket);
            if (running_ > 0)
                --running_;
            running_stat_.store(running_, std::memory_order_relaxed);
        }
    }
    switch (status) {
        case Status::Done:
            completed_done_.fetch_add(1, std::memory_order_relaxed);
            break;
        case Status::Failed:
            completed_failed_.fetch_add(1, std::memory_order_relaxed);
            break;
        case Status::Cancelled:
            completed_cancelled_.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
    }
    if (task.sink) {
        // Approximate wake latency: completion publication -> sink dispatch.
        // Parent integrations that resume the fiber later can add their own
        // measured resume latency through note_wake_latency_us().
        wake_hist_.record(us_between(completed_at, std::chrono::steady_clock::now()));
        task.sink->on_complete(task.ticket);
    }
    cv_.notify_all();
}

void OffloadPool::worker_loop(std::stop_token st) {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, st, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stopping_)
                    return;
                continue;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
            queue_depth_.store(static_cast<std::uint64_t>(queue_.size()),
                               std::memory_order_relaxed);
            task.ticket->status.store(Status::Running, std::memory_order_release);
            task.ticket->started_at = std::chrono::steady_clock::now();
            queue_wait_hist_.record(us_between(task.ticket->queued_at, task.ticket->started_at));
            running_tickets_.push_back(task.ticket);
            ++running_;
            running_stat_.store(running_, std::memory_order_relaxed);
        }

        const auto started = std::chrono::steady_clock::now();
        try {
            Result result = task.job(*task.input, task.ticket->stop.get_token());
            result.timing.queue_us = us_between(task.ticket->queued_at, started);
            result.timing.exec_us = us_between(started, std::chrono::steady_clock::now());
            if (task.ticket->stop.stop_requested()) {
                finish(task, Status::Cancelled, "cancelled");
            } else {
                task.ticket->out = std::move(result);
                finish(task, Status::Done, {});
            }
        } catch (const std::exception& e) {
            finish(task, Status::Failed, e.what());
        } catch (...) {
            finish(task, Status::Failed, "unknown exception");
        }
    }
}

void OffloadPool::shutdown() {
    std::deque<Task> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return;
        stopping_ = true;
        accepting_ = false;
        pending.swap(queue_);
        queue_depth_.store(0, std::memory_order_relaxed);
        for (auto& ticket : running_tickets_)
            ticket->stop.request_stop();
        for (auto& worker : workers_)
            worker.request_stop();
    }

    for (auto& task : pending)
        finish(task, Status::Cancelled, "pool shutdown");
    cv_.notify_all();

    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, cfg_.shutdown_grace, [this] { return running_ == 0; });
    }
    for (auto& worker : workers_) {
        if (worker.joinable())
            worker.join();
    }
}

Stats OffloadPool::stats() const {
    Stats out;
    out.submitted = submitted_.load(std::memory_order_relaxed);
    out.rejected = rejected_.load(std::memory_order_relaxed);
    out.rejected_queue_full = rejected_queue_full_.load(std::memory_order_relaxed);
    out.rejected_quota = rejected_quota_.load(std::memory_order_relaxed);
    out.rejected_shutdown = rejected_shutdown_.load(std::memory_order_relaxed);
    out.completed_done = completed_done_.load(std::memory_order_relaxed);
    out.completed_failed = completed_failed_.load(std::memory_order_relaxed);
    out.completed_cancelled = completed_cancelled_.load(std::memory_order_relaxed);
    out.stale_completion_dropped = stale_completion_dropped_.load(std::memory_order_relaxed);
    out.sync_fallback = sync_fallback_.load(std::memory_order_relaxed);
    out.queue_depth = queue_depth_.load(std::memory_order_relaxed);
    out.running = running_stat_.load(std::memory_order_relaxed);
    out.threads = cfg_.threads;
    out.queue_depth_peak = queue_depth_peak_.load(std::memory_order_relaxed);
    out.queue_wait_us = queue_wait_hist_.snapshot();
    out.exec_us = exec_hist_.snapshot();
    out.wake_latency_us = wake_hist_.snapshot();
    return out;
}

void OffloadPool::note_stale_completion_dropped() noexcept {
    stale_completion_dropped_.fetch_add(1, std::memory_order_relaxed);
}

void OffloadPool::note_sync_fallback() noexcept {
    sync_fallback_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace aura::offload
