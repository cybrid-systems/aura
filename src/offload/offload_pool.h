// offload/offload_pool.h — ownership-safe offload pool (issues #4881/#4882)
//
// A job sees only an immutable SnapshotInput and returns a plain Result. It
// never receives an Evaluator, EvalValue, or session CompilerService: the
// caller is responsible for running the job in a thread-private compiler
// context. Completion is published on Ticket::status (release) after the
// result slot is written; waiters read status with acquire.
#ifndef AURA_OFFLOAD_OFFLOAD_POOL_H
#define AURA_OFFLOAD_OFFLOAD_POOL_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace aura::offload {

enum class Status : std::uint8_t { Queued, Running, Done, Failed, Cancelled, Rejected };

struct Diagnostic {
    std::string message;
};

struct TestSummary {
    std::uint64_t passed = 0;
    std::uint64_t failed = 0;
    std::vector<std::string> failures;
};

struct Timing {
    std::uint64_t queue_us = 0;
    std::uint64_t exec_us = 0;
};

struct Result {
    std::vector<Diagnostic> diagnostics;
    TestSummary tests;
    Timing timing;
    std::string artifact;
};

struct SnapshotInput {
    std::string source;
    std::uint64_t snapshot_id = 0;
};

struct Error {
    std::string message;
};

struct Ticket {
    std::atomic<Status> status{Status::Queued};
    std::stop_source stop;
    std::variant<std::monostate, Result, Error> out;
    std::uint64_t fiber_id = 0;
    std::uint64_t wait_gen = 0;
    std::chrono::steady_clock::time_point queued_at{};
    std::chrono::steady_clock::time_point started_at{};
};

using Job = std::move_only_function<Result(const SnapshotInput&, std::stop_token)>;

struct Stats {
    std::uint64_t submitted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t completed_done = 0;
    std::uint64_t completed_failed = 0;
    std::uint64_t completed_cancelled = 0;
    std::uint64_t queue_depth = 0;
    std::uint64_t running = 0;
    std::uint64_t queue_depth_peak = 0;
};

enum class SubmitStatus : std::uint8_t { Accepted, QueueFull, ShuttingDown };

class CompletionSink {
public:
    CompletionSink() = default;
    virtual ~CompletionSink() = default;
    CompletionSink(const CompletionSink&) = delete;
    CompletionSink& operator=(const CompletionSink&) = delete;
    virtual void on_complete(std::shared_ptr<Ticket> ticket) noexcept = 0;
};

class OffloadPool {
public:
    struct Config {
        // 0 => clamp(hardware_concurrency, 2, 8). Overridden by
        // AURA_OFFLOAD_THREADS.
        std::size_t threads = 0;
        // Bounded queue; try_submit never blocks. Overridden by
        // AURA_OFFLOAD_QUEUE_CAP.
        std::size_t queue_cap = 256;
        std::chrono::milliseconds shutdown_grace{2000};
    };

    OffloadPool();
    explicit OffloadPool(Config cfg);
    ~OffloadPool();

    OffloadPool(const OffloadPool&) = delete;
    OffloadPool& operator=(const OffloadPool&) = delete;
    OffloadPool(OffloadPool&&) = delete;
    OffloadPool& operator=(OffloadPool&&) = delete;

    [[nodiscard]] SubmitStatus try_submit(Job job, std::shared_ptr<const SnapshotInput> input,
                                          std::shared_ptr<Ticket> ticket, CompletionSink* sink);

    void shutdown();

    [[nodiscard]] Stats stats() const;

private:
    struct Task {
        Job job;
        std::shared_ptr<const SnapshotInput> input;
        std::shared_ptr<Ticket> ticket;
        CompletionSink* sink = nullptr;
    };

    void worker_loop(std::stop_token st);
    void finish(const Task& task, Status status, std::string error);
    static std::size_t default_threads();
    static std::size_t read_env_size(const char* name, std::size_t fallback);

    Config cfg_;
    std::vector<std::jthread> workers_;
    std::deque<Task> queue_;
    std::vector<std::shared_ptr<Ticket>> running_tickets_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    bool accepting_ = true;
    bool stopping_ = false;
    std::size_t running_ = 0;

    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> completed_done_{0};
    std::atomic<std::uint64_t> completed_failed_{0};
    std::atomic<std::uint64_t> completed_cancelled_{0};
    std::atomic<std::uint64_t> queue_depth_{0};
    std::atomic<std::uint64_t> running_stat_{0};
    std::atomic<std::uint64_t> queue_depth_peak_{0};
};

} // namespace aura::offload

#endif // AURA_OFFLOAD_OFFLOAD_POOL_H
