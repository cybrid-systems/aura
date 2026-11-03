// tests/serve/test_offload_pool.cpp — #4882 pool core acceptance
#include "offload/offload_pool.h"

#include "core/resource_quota.hh"

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

// Issue #4884: counters must match submitted / rejected / failed exactly,
// and gauges must drain back to zero.
void test_metrics_counts_exact() {
    RecordingSink sink;
    OffloadPool::Config cfg;
    cfg.threads = 2;
    cfg.queue_cap = 64;
    OffloadPool pool(cfg);

    constexpr int kOk = 7;
    constexpr int kFailed = 3;
    std::vector<std::shared_ptr<Ticket>> tickets;
    for (int i = 0; i < kOk + kFailed; ++i) {
        auto ticket = std::make_shared<Ticket>();
        tickets.push_back(ticket);
        const bool fail = i >= kOk;
        CHECK(pool.try_submit(
                  [fail](const SnapshotInput&, std::stop_token) -> Result {
                      if (fail)
                          throw std::runtime_error("boom");
                      return Result{};
                  },
                  input_for("metric"), ticket, &sink) == SubmitStatus::Accepted);
    }
    CHECK(sink.wait_for(kOk + kFailed));

    const Stats s = pool.stats();
    CHECK(s.submitted == kOk + kFailed);
    CHECK(s.rejected == 0);
    CHECK(s.rejected_queue_full == 0);
    CHECK(s.rejected_quota == 0);
    CHECK(s.rejected_shutdown == 0);
    CHECK(s.completed_done == kOk);
    CHECK(s.completed_failed == kFailed);
    CHECK(s.completed_cancelled == 0);
    CHECK(s.queue_depth == 0);
    CHECK(s.running == 0);
    CHECK(s.threads == 2);
    CHECK(s.queue_depth_peak >= 1);
    CHECK(s.queue_wait_us.count == kOk + kFailed);
    CHECK(s.exec_us.count == kOk + kFailed);
    CHECK(s.wake_latency_us.count == kOk + kFailed);
    for (const LatencyHistogram* h : {&s.queue_wait_us, &s.exec_us, &s.wake_latency_us}) {
        std::uint64_t bucketed = 0;
        for (auto n : h->buckets)
            bucketed += n;
        CHECK(bucketed == h->count);
        CHECK(h->max_us <= h->sum_us);
    }
}

void test_metrics_rejected_and_integration_hooks() {
    RecordingSink sink;
    OffloadPool::Config cfg;
    cfg.threads = 1;
    cfg.queue_cap = 1;
    OffloadPool pool(cfg);

    auto gate = std::make_shared<std::atomic<bool>>(false);
    auto started = std::make_shared<std::atomic<bool>>(false);
    auto blocker = [gate, started](const SnapshotInput&, std::stop_token) -> Result {
        started->store(true, std::memory_order_release);
        while (!gate->load(std::memory_order_acquire))
            std::this_thread::sleep_for(1ms);
        return {};
    };
    auto t1 = std::make_shared<Ticket>();
    auto t2 = std::make_shared<Ticket>();
    CHECK(pool.try_submit(blocker, input_for("a"), t1, &sink) == SubmitStatus::Accepted);
    while (!started->load(std::memory_order_acquire))
        std::this_thread::sleep_for(1ms);
    CHECK(pool.try_submit([](const SnapshotInput&, std::stop_token) { return Result{}; },
                          input_for("b"), t2, &sink) == SubmitStatus::Accepted);
    auto t3 = std::make_shared<Ticket>();
    CHECK(pool.try_submit([](const SnapshotInput&, std::stop_token) { return Result{}; },
                          input_for("c"), t3, &sink) == SubmitStatus::QueueFull);
    CHECK(t3->status.load(std::memory_order_acquire) == Status::Rejected);

    const Stats s = pool.stats();
    CHECK(s.submitted == 2);
    CHECK(s.rejected == 1);
    CHECK(s.rejected_queue_full == 1);
    CHECK(s.rejected_quota == 0);
    CHECK(s.rejected_shutdown == 0);
    // Rejected submissions do not notify the sink and never run.
    CHECK(s.completed_done == 0);
    CHECK(s.queue_wait_us.count == 1);
    CHECK(s.queue_depth == 1);

    gate->store(true, std::memory_order_release);
    CHECK(sink.wait_for(2));
    CHECK(pool.stats().queue_depth == 0);
    CHECK(pool.stats().running == 0);

    pool.note_stale_completion_dropped();
    pool.note_stale_completion_dropped();
    pool.note_sync_fallback();
    const Stats after = pool.stats();
    CHECK(after.stale_completion_dropped == 2);
    CHECK(after.sync_fallback == 1);
}

// Issue #4886: class queues are bounded and default to Compile.
void test_class_default_and_bounds() {
    RecordingSink sink;
    OffloadPool::Config cfg;
    cfg.threads = 1;
    cfg.queue_cap = 4;
    cfg.class_queue_cap = 2;
    OffloadPool pool(cfg);

    auto gate = std::make_shared<std::atomic<bool>>(false);
    auto started = std::make_shared<std::atomic<bool>>(false);
    auto blocker = [gate, started](const SnapshotInput&, std::stop_token) -> Result {
        started->store(true, std::memory_order_release);
        while (!gate->load(std::memory_order_acquire))
            std::this_thread::sleep_for(1ms);
        return {};
    };
    auto t1 = std::make_shared<Ticket>();
    CHECK(pool.try_submit(blocker, input_for("block"), t1, &sink) == SubmitStatus::Accepted);
    while (!started->load(std::memory_order_acquire))
        std::this_thread::sleep_for(1ms);

    auto t2 = std::make_shared<Ticket>();
    CHECK(pool.try_submit([](const SnapshotInput&, std::stop_token) { return Result{}; },
                          input_for("default"), t2, &sink) == SubmitStatus::Accepted);
    CHECK(t2->klass == OffloadClass::Compile);

    auto t3 = std::make_shared<Ticket>();
    CHECK(pool.try_submit([](const SnapshotInput&, std::stop_token) { return Result{}; },
                          input_for("compile"), t3, &sink,
                          OffloadClass::Compile) == SubmitStatus::Accepted);
    auto t4 = std::make_shared<Ticket>();
    CHECK(pool.try_submit([](const SnapshotInput&, std::stop_token) { return Result{}; },
                          input_for("compile2"), t4, &sink,
                          OffloadClass::Compile) == SubmitStatus::QueueFull);
    CHECK(t4->status.load(std::memory_order_acquire) == Status::Rejected);
    // A rejected class does not consume another class's budget.
    auto t5 = std::make_shared<Ticket>();
    CHECK(pool.try_submit([](const SnapshotInput&, std::stop_token) { return Result{}; },
                          input_for("batch"), t5, &sink,
                          OffloadClass::Batch) == SubmitStatus::Accepted);

    const Stats queued = pool.stats();
    CHECK(queued.by_class[0].queue_depth == 0);
    CHECK(queued.by_class[1].queue_depth == 2);
    CHECK(queued.by_class[2].queue_depth == 1);
    CHECK(queued.by_class[1].rejected == 1);
    CHECK(queued.queue_depth == 3);

    gate->store(true, std::memory_order_release);
    CHECK(sink.wait_for(4));
    CHECK(pool.stats().queue_depth == 0);
    // The blocker itself is a default-class (Compile) job.
    CHECK(pool.stats().by_class[1].completed_done == 3);
    CHECK(pool.stats().by_class[2].completed_done == 1);
}

// Issue #4886: with a single worker and a pre-filled queue, the 4:2:1
// schedule drains Interactive first, then Compile, then Batch.
void test_weighted_dispatch_order() {
    RecordingSink sink;
    OffloadPool::Config cfg;
    cfg.threads = 1;
    cfg.queue_cap = 32;
    OffloadPool pool(cfg);

    auto gate = std::make_shared<std::atomic<bool>>(false);
    auto started = std::make_shared<std::atomic<bool>>(false);
    auto blocker = [gate, started](const SnapshotInput&, std::stop_token) -> Result {
        started->store(true, std::memory_order_release);
        while (!gate->load(std::memory_order_acquire))
            std::this_thread::sleep_for(1ms);
        return {};
    };
    auto blocker_ticket = std::make_shared<Ticket>();
    CHECK(pool.try_submit(blocker, input_for("block"), blocker_ticket, &sink) ==
          SubmitStatus::Accepted);
    while (!started->load(std::memory_order_acquire))
        std::this_thread::sleep_for(1ms);

    auto submit = [&](OffloadClass klass) {
        auto ticket = std::make_shared<Ticket>();
        CHECK(pool.try_submit([](const SnapshotInput&, std::stop_token) { return Result{}; },
                              input_for("class"), ticket, &sink, klass) == SubmitStatus::Accepted);
        return ticket;
    };
    // Two warm-up jobs advance the 7-slot schedule back to phase 0; they are
    // not part of the ordering assertion.
    submit(OffloadClass::Compile);
    submit(OffloadClass::Batch);
    // Enqueue low-priority work first to prove the class, not FIFO, decides.
    submit(OffloadClass::Batch);
    submit(OffloadClass::Compile);
    submit(OffloadClass::Compile);
    std::vector<std::shared_ptr<Ticket>> interactive;
    for (int i = 0; i < 4; ++i)
        interactive.push_back(submit(OffloadClass::Interactive));

    gate->store(true, std::memory_order_release);
    CHECK(sink.wait_for(10));
    CHECK(sink.count() == 10);

    std::vector<OffloadClass> order;
    {
        std::lock_guard<std::mutex> lock(sink.m);
        for (const auto& ticket : sink.done)
            order.push_back(ticket->klass);
    }
    CHECK(order.size() == 10);
    if (order.size() == 10) {
        // 0: default-class blocker, 1-2: warm-ups, then the 4:2:1 cycle.
        CHECK(order[0] == OffloadClass::Compile);
        CHECK(order[1] == OffloadClass::Compile);
        CHECK(order[2] == OffloadClass::Batch);
        for (std::size_t i = 3; i <= 6; ++i)
            CHECK(order[i] == OffloadClass::Interactive);
        CHECK(order[7] == OffloadClass::Compile);
        CHECK(order[8] == OffloadClass::Compile);
        CHECK(order[9] == OffloadClass::Batch);
    }

    const Stats s = pool.stats();
    CHECK(s.by_class[0].submitted == 4);
    CHECK(s.by_class[1].submitted == 4); // 2 Compile + 2 defaults (blocker, warm-up)
    CHECK(s.by_class[2].submitted == 2); // 1 Batch + warm-up
    CHECK(s.by_class[0].completed_done == 4);
    CHECK(s.by_class[1].completed_done == 4);
    CHECK(s.by_class[2].completed_done == 2);
}

// Issue #4886: sustained Batch pressure must not starve Interactive work.
void test_no_class_starvation_under_batch_flood() {
    RecordingSink sink;
    OffloadPool::Config cfg;
    cfg.threads = 2;
    cfg.queue_cap = 256;
    OffloadPool pool(cfg);

    constexpr int kBatch = 24;
    for (int i = 0; i < kBatch; ++i) {
        auto ticket = std::make_shared<Ticket>();
        CHECK(pool.try_submit(
                  [](const SnapshotInput&, std::stop_token) {
                      std::this_thread::sleep_for(2ms);
                      return Result{};
                  },
                  input_for("batch"), ticket, &sink,
                  OffloadClass::Batch) == SubmitStatus::Accepted);
    }
    std::vector<std::shared_ptr<Ticket>> interactive;
    for (int i = 0; i < 4; ++i) {
        auto ticket = std::make_shared<Ticket>();
        interactive.push_back(ticket);
        CHECK(pool.try_submit([](const SnapshotInput&, std::stop_token) { return Result{}; },
                              input_for("interactive"), ticket, &sink,
                              OffloadClass::Interactive) == SubmitStatus::Accepted);
    }
    CHECK(sink.wait_for(kBatch + 4, 5000ms));
    for (const auto& ticket : interactive)
        CHECK(ticket->status.load(std::memory_order_acquire) == Status::Done);
    const Stats s = pool.stats();
    CHECK(s.by_class[0].completed_done == 4);
    CHECK(s.by_class[2].completed_done == kBatch);
    CHECK(s.queue_depth == 0);
}

// Issue #4885: per-tenant OffloadInflight quota, release on every terminal.
void test_offload_quota_per_tenant() {
    using namespace aura::core::resource_quota;
    reset_process_resource_quota_for_test();
    set_quota_per_tenant_enabled_for_test(true);
    auto& quota = process_resource_quota();
    constexpr TenantId kTenantA = 101;
    constexpr TenantId kTenantB = 202;
    quota.set_tenant_limit(kTenantA, Dimension::OffloadInflight, 2);

    {
        RecordingSink sink;
        OffloadPool::Config cfg;
        cfg.threads = 2;
        cfg.queue_cap = 16;
        cfg.quota = &quota;
        OffloadPool pool(cfg);

        auto gate = std::make_shared<std::atomic<bool>>(false);
        auto started = std::make_shared<std::atomic<int>>(0);
        auto blocker = [gate, started](const SnapshotInput&, std::stop_token) -> Result {
            started->fetch_add(1, std::memory_order_release);
            while (!gate->load(std::memory_order_acquire))
                std::this_thread::sleep_for(1ms);
            std::this_thread::sleep_for(2ms);
            return {};
        };

        set_current_quota_tenant(kTenantA);
        auto t1 = std::make_shared<Ticket>();
        auto t2 = std::make_shared<Ticket>();
        CHECK(pool.try_submit(blocker, input_for("a1"), t1, &sink) == SubmitStatus::Accepted);
        CHECK(pool.try_submit(blocker, input_for("a2"), t2, &sink) == SubmitStatus::Accepted);
        while (started->load(std::memory_order_acquire) < 2)
            std::this_thread::sleep_for(1ms);
        CHECK(t1->tenant == kTenantA);
        CHECK(t2->tenant == kTenantA);
        CHECK(quota.tenant_used(kTenantA, Dimension::OffloadInflight) == 2);

        // Tenant A is at its limit: the third concurrent submit is typed
        // Rejected without touching the queue.
        auto t3 = std::make_shared<Ticket>();
        CHECK(pool.try_submit([](const SnapshotInput&, std::stop_token) { return Result{}; },
                              input_for("a3"), t3, &sink) == SubmitStatus::QuotaExceeded);
        CHECK(t3->status.load(std::memory_order_acquire) == Status::Rejected);
        CHECK(t3->tenant == kTenantA);
        auto* qerr = std::get_if<Error>(&t3->out);
        CHECK(qerr != nullptr);
        CHECK(qerr != nullptr && !qerr->message.empty());

        // Tenant B has no per-tenant limit and is unaffected by A's cap.
        set_current_quota_tenant(kTenantB);
        auto tb = std::make_shared<Ticket>();
        CHECK(pool.try_submit([](const SnapshotInput&, std::stop_token) { return Result{}; },
                              input_for("b1"), tb, &sink) == SubmitStatus::Accepted);
        CHECK(tb->tenant == kTenantB);

        set_current_quota_tenant(kTenantA);
        auto t4 = std::make_shared<Ticket>();
        CHECK(pool.try_submit([](const SnapshotInput&, std::stop_token) { return Result{}; },
                              input_for("a4"), t4, &sink) == SubmitStatus::QuotaExceeded);

        const Stats rejected_stats = pool.stats();
        CHECK(rejected_stats.rejected_quota == 2);
        CHECK(rejected_stats.rejected_queue_full == 0);
        CHECK(rejected_stats.rejected == 2);
        CHECK(rejected_stats.by_tenant_count == 2);

        gate->store(true, std::memory_order_release);
        CHECK(sink.wait_for(3));

        // Releasing A's slots lets A submit again; B was never blocked.
        auto t5 = std::make_shared<Ticket>();
        CHECK(pool.try_submit([](const SnapshotInput&, std::stop_token) { return Result{}; },
                              input_for("a5"), t5, &sink) == SubmitStatus::Accepted);
        CHECK(sink.wait_for(4));
        CHECK(t5->status.load(std::memory_order_acquire) == Status::Done);
        CHECK(quota.tenant_used(kTenantA, Dimension::OffloadInflight) == 0);
        CHECK(quota.tenant_used(kTenantB, Dimension::OffloadInflight) == 0);
        CHECK(quota.used(Dimension::OffloadInflight) == 0);
        CHECK(quota.tenant_used(kTenantA, Dimension::TimeUs) > 0);

        const Stats s = pool.stats();
        CHECK(s.by_tenant_count == 2);
        bool saw_a = false;
        bool saw_b = false;
        for (std::size_t i = 0; i < s.by_tenant_count; ++i) {
            const auto& ts = s.by_tenant[i];
            if (ts.tenant == kTenantA) {
                saw_a = true;
                CHECK(ts.submitted == 3);
                CHECK(ts.rejected == 2);
                CHECK(ts.completed_done == 3);
                CHECK(ts.inflight_peak == 2);
            } else if (ts.tenant == kTenantB) {
                saw_b = true;
                CHECK(ts.submitted == 1);
                CHECK(ts.rejected == 0);
                CHECK(ts.completed_done == 1);
                CHECK(ts.inflight_peak == 1);
            }
        }
        CHECK(saw_a);
        CHECK(saw_b);

        // Cancelled terminal path also releases the slot exactly once.
        auto t6 = std::make_shared<Ticket>();
        auto spin = [](const SnapshotInput&, std::stop_token st) -> Result {
            while (!st.stop_requested())
                std::this_thread::sleep_for(1ms);
            return {};
        };
        CHECK(pool.try_submit(spin, input_for("a6"), t6, &sink) == SubmitStatus::Accepted);
        CHECK(quota.tenant_used(kTenantA, Dimension::OffloadInflight) == 1);
        pool.shutdown();
        CHECK(t6->status.load(std::memory_order_acquire) == Status::Cancelled);
        CHECK(sink.count() == 5);
        CHECK(quota.tenant_used(kTenantA, Dimension::OffloadInflight) == 0);
        CHECK(quota.used(Dimension::OffloadInflight) == 0);
    }
    reset_process_resource_quota_for_test();
}

} // namespace

int main() {
    test_exception_becomes_failed();
    test_queue_full_is_rejected_without_notification();
    test_shutdown_cancels_queued_and_running();
    test_one_terminal_notification_per_task();
    test_metrics_counts_exact();
    test_metrics_rejected_and_integration_hooks();
    test_class_default_and_bounds();
    test_weighted_dispatch_order();
    test_no_class_starvation_under_batch_flood();
    test_offload_quota_per_tenant();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ok offload_pool: all checks passed\n");
    return 0;
}
