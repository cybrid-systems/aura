// tests/serve/test_offload_pool.cpp — #4882 pool core acceptance
#include "offload/offload_pool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace aura::offload;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

struct RecordingSink final : CompletionSink {
    mutable std::mutex m;
    std::condition_variable cv;
    std::vector<std::shared_ptr<Ticket>> done;

    void on_complete(std::shared_ptr<Ticket> ticket) noexcept override {
        {
            std::lock_guard<std::mutex> lock(m);
            done.push_back(std::move(ticket));
        }
        cv.notify_all();
    }

    bool wait_for(std::size_t n, std::chrono::milliseconds timeout = 3000ms) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, timeout, [&] { return done.size() >= n; });
    }

    std::size_t count() const {
        std::lock_guard<std::mutex> lock(m);
        return done.size();
    }
};

std::shared_ptr<SnapshotInput> input_for(const char* source) {
    return std::make_shared<SnapshotInput>(SnapshotInput{source, 1});
}

void test_exception_becomes_failed() {
    RecordingSink sink;
    OffloadPool::Config cfg;
    cfg.threads = 1;
    cfg.queue_cap = 8;
    OffloadPool pool(cfg);

    auto ticket = std::make_shared<Ticket>();
    const auto submitted = pool.try_submit(
        [](const SnapshotInput&, std::stop_token) -> Result { throw std::runtime_error("boom"); },
        input_for("(+ 1 2)"), ticket, &sink);

    CHECK(submitted == SubmitStatus::Accepted);
    CHECK(sink.wait_for(1));
    CHECK(ticket->status.load(std::memory_order_acquire) == Status::Failed);
    auto* err = std::get_if<Error>(&ticket->out);
    CHECK(err != nullptr);
    CHECK(err != nullptr && err->message.find("boom") != std::string::npos);
    CHECK(sink.count() == 1);
}

void test_queue_full_is_rejected_without_notification() {
    RecordingSink sink;
    OffloadPool::Config cfg;
    cfg.threads = 1;
    cfg.queue_cap = 1;
    OffloadPool pool(cfg);

    auto gate = std::make_shared<std::atomic<bool>>(false);
    auto started = std::make_shared<std::atomic<bool>>(false);
    auto blocking = [gate, started](const SnapshotInput&, std::stop_token st) -> Result {
        started->store(true, std::memory_order_release);
        while (!gate->load(std::memory_order_acquire) && !st.stop_requested())
            std::this_thread::sleep_for(1ms);
        return {};
    };

    auto t1 = std::make_shared<Ticket>();
    CHECK(pool.try_submit(blocking, input_for("a"), t1, &sink) == SubmitStatus::Accepted);
    while (!started->load(std::memory_order_acquire))
        std::this_thread::sleep_for(1ms);

    auto t2 = std::make_shared<Ticket>();
    CHECK(pool.try_submit([](const SnapshotInput&, std::stop_token) { return Result{}; },
                          input_for("b"), t2, &sink) == SubmitStatus::Accepted);

    auto t3 = std::make_shared<Ticket>();
    CHECK(pool.try_submit([](const SnapshotInput&, std::stop_token) { return Result{}; },
                          input_for("c"), t3, &sink) == SubmitStatus::QueueFull);
    CHECK(t3->status.load(std::memory_order_acquire) == Status::Rejected);

    gate->store(true, std::memory_order_release);
    CHECK(sink.wait_for(2));
    CHECK(sink.count() == 2);
    CHECK(pool.stats().submitted == 2);
    CHECK(pool.stats().rejected == 1);
}

void test_shutdown_cancels_queued_and_running() {
    RecordingSink sink;
    OffloadPool::Config cfg;
    cfg.threads = 1;
    cfg.queue_cap = 8;
    cfg.shutdown_grace = 2000ms;
    OffloadPool pool(cfg);

    auto started = std::make_shared<std::atomic<bool>>(false);
    auto blocking = [started](const SnapshotInput&, std::stop_token st) -> Result {
        started->store(true, std::memory_order_release);
        while (!st.stop_requested())
            std::this_thread::sleep_for(1ms);
        return {};
    };

    auto t1 = std::make_shared<Ticket>();
    CHECK(pool.try_submit(blocking, input_for("run"), t1, &sink) == SubmitStatus::Accepted);
    while (!started->load(std::memory_order_acquire))
        std::this_thread::sleep_for(1ms);

    auto t2 = std::make_shared<Ticket>();
    CHECK(pool.try_submit([](const SnapshotInput&, std::stop_token) { return Result{}; },
                          input_for("queued"), t2, &sink) == SubmitStatus::Accepted);

    std::thread stopper([&] { pool.shutdown(); });
    stopper.join();

    CHECK(sink.count() == 2);
    CHECK(t1->status.load(std::memory_order_acquire) == Status::Cancelled);
    CHECK(t2->status.load(std::memory_order_acquire) == Status::Cancelled);
    auto* e2 = std::get_if<Error>(&t2->out);
    CHECK(e2 != nullptr);
    CHECK(pool.stats().completed_cancelled == 2);
}

void test_one_terminal_notification_per_task() {
    RecordingSink sink;
    OffloadPool::Config cfg;
    cfg.threads = 2;
    cfg.queue_cap = 64;
    OffloadPool pool(cfg);

    constexpr int kTasks = 16;
    std::vector<std::shared_ptr<Ticket>> tickets;
    for (int i = 0; i < kTasks; ++i) {
        auto ticket = std::make_shared<Ticket>();
        tickets.push_back(ticket);
        CHECK(pool.try_submit(
                  [](const SnapshotInput& in, std::stop_token) {
                      Result r;
                      r.artifact = in.source;
                      return r;
                  },
                  input_for("job"), ticket, &sink) == SubmitStatus::Accepted);
    }
    CHECK(sink.wait_for(kTasks));
    CHECK(sink.count() == kTasks);
    for (const auto& ticket : tickets)
        CHECK(ticket->status.load(std::memory_order_acquire) == Status::Done);
    CHECK(pool.stats().completed_done == kTasks);
}

} // namespace

int main() {
    test_exception_becomes_failed();
    test_queue_full_is_rejected_without_notification();
    test_shutdown_cancels_queued_and_running();
    test_one_terminal_notification_per_task();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ok offload_pool: all checks passed\n");
    return 0;
}
