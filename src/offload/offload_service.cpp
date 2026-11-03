// offload/offload_service.cpp — Scheduler completion bridge (#4883)
#include "offload_service.h"

#include "serve/fiber.h"
#include "serve/scheduler.h"

#include <exception>
#include <utility>

namespace aura::offload {

class OffloadService::Sink final : public CompletionSink {
public:
    explicit Sink(aura::serve::Scheduler* sched)
        : sched_(sched) {}

    void on_complete(std::shared_ptr<Ticket> ticket) noexcept override {
        if (sched_ && ticket)
            sched_->post_offload_completion(ticket->fiber_id, ticket->wait_gen);
    }

private:
    aura::serve::Scheduler* sched_ = nullptr;
};

namespace {

    bool terminal(Status status) noexcept {
        return status == Status::Done || status == Status::Failed || status == Status::Cancelled ||
               status == Status::Rejected;
    }

} // namespace

OffloadService& OffloadService::instance() {
    static OffloadService service;
    return service;
}

void OffloadService::start(aura::serve::Scheduler* sched) {
    start(sched, OffloadPool::Config{});
}

void OffloadService::start(aura::serve::Scheduler* sched, OffloadPool::Config cfg) {
    if (pool_)
        return;
    sched_ = sched;
    sink_ = std::make_unique<Sink>(sched);
    pool_ = std::make_unique<OffloadPool>(cfg);
}

void OffloadService::stop() {
    pool_.reset();
    sink_.reset();
    sched_ = nullptr;
}

OffloadService::Submission
OffloadService::submit(Job job, std::shared_ptr<const SnapshotInput> input, OffloadClass klass) {
    Submission out;
    out.ticket = std::make_shared<Ticket>();
    if (!job || !input) {
        out.status = SubmitStatus::ShuttingDown;
        out.ticket->status.store(Status::Rejected, std::memory_order_release);
        return out;
    }

    auto* fiber = aura::serve::g_current_fiber;
    if (!pool_ || !fiber) {
        try {
            out.ticket->out = job(*input, std::stop_token{});
            out.ticket->status.store(Status::Done, std::memory_order_release);
        } catch (const std::exception& e) {
            out.ticket->out = Error{e.what()};
            out.ticket->status.store(Status::Failed, std::memory_order_release);
        } catch (...) {
            out.ticket->out = Error{"offload job failed"};
            out.ticket->status.store(Status::Failed, std::memory_order_release);
        }
        out.status = SubmitStatus::Accepted;
        return out;
    }

    out.ticket->fiber_id = fiber->id();
    out.ticket->wait_gen = fiber->bump_wait_generation();
    out.status =
        pool_->try_submit(std::move(job), std::move(input), out.ticket, sink_.get(), klass);
    return out;
}

void OffloadService::await(const std::shared_ptr<Ticket>& ticket) {
    if (!ticket)
        return;
    auto* fiber = aura::serve::g_current_fiber;
    if (!fiber)
        return;
    while (!terminal(ticket->status.load(std::memory_order_acquire))) {
        fiber->set_state(aura::serve::FiberState::Waiting);
        aura::serve::Fiber::yield();
    }
}

bool OffloadService::await_for(const std::shared_ptr<Ticket>& ticket,
                               std::chrono::milliseconds timeout) {
    if (!ticket)
        return false;
    auto* fiber = aura::serve::g_current_fiber;
    if (!fiber || timeout.count() <= 0)
        return terminal(ticket->status.load(std::memory_order_acquire));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!terminal(ticket->status.load(std::memory_order_acquire))) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        fiber->set_state(aura::serve::FiberState::Waiting);
        aura::serve::Fiber::yield();
    }
    return true;
}

Stats OffloadService::stats() const {
    return pool_ ? pool_->stats() : Stats{};
}

std::uint64_t OffloadService::stale_completions() const {
    return sched_ ? sched_->offload_stale_completions() : 0;
}

namespace {

    const char* class_label(OffloadClass klass) noexcept {
        switch (klass) {
            case OffloadClass::Interactive:
                return "interactive";
            case OffloadClass::Compile:
                return "compile";
            case OffloadClass::Batch:
                return "batch";
        }
        return "compile";
    }

    std::string histogram_json(const LatencyHistogram& hist) {
        std::string out = "{\"count\":" + std::to_string(hist.count) +
                          ",\"sum_us\":" + std::to_string(hist.sum_us) +
                          ",\"max_us\":" + std::to_string(hist.max_us) + ",\"buckets\":[";
        for (std::size_t i = 0; i < kLatencyBucketCount; ++i) {
            if (i != 0)
                out += ',';
            out += std::to_string(hist.buckets[i]);
        }
        out += "]}";
        return out;
    }

    void append_histogram_prometheus(std::string& out, const char* name,
                                     const LatencyHistogram& hist) {
        const std::string prefix = name;
        out += "# TYPE " + prefix + " histogram\n";
        for (std::size_t i = 0; i + 1 < kLatencyBucketCount; ++i) {
            out += prefix + "_bucket{le=\"" + std::to_string(kLatencyBucketBoundsUs[i]) + "\"} " +
                   std::to_string(hist.buckets[i]) + "\n";
        }
        out += prefix + "_bucket{le=\"+Inf\"} " +
               std::to_string(hist.buckets[kLatencyBucketCount - 1]) + "\n";
        out += prefix + "_sum " + std::to_string(hist.sum_us) + "\n";
        out += prefix + "_count " + std::to_string(hist.count) + "\n";
    }

} // namespace

std::string offload_metrics_json() {
    const Stats s = OffloadService::instance().stats();
    std::string out =
        "{\"submitted_total\":" + std::to_string(s.submitted) +
        ",\"rejected_total\":" + std::to_string(s.rejected) +
        ",\"rejected_queue_full_total\":" + std::to_string(s.rejected_queue_full) +
        ",\"rejected_quota_total\":" + std::to_string(s.rejected_quota) +
        ",\"rejected_shutdown_total\":" + std::to_string(s.rejected_shutdown) +
        ",\"completed_done_total\":" + std::to_string(s.completed_done) +
        ",\"completed_failed_total\":" + std::to_string(s.completed_failed) +
        ",\"completed_cancelled_total\":" + std::to_string(s.completed_cancelled) +
        ",\"stale_completion_dropped_total\":" + std::to_string(s.stale_completion_dropped) +
        ",\"sync_fallback_total\":" + std::to_string(s.sync_fallback) +
        ",\"queue_depth\":" + std::to_string(s.queue_depth) +
        ",\"running\":" + std::to_string(s.running) + ",\"threads\":" + std::to_string(s.threads) +
        ",\"queue_depth_peak\":" + std::to_string(s.queue_depth_peak) +
        ",\"queue_wait_us\":" + histogram_json(s.queue_wait_us) +
        ",\"exec_us\":" + histogram_json(s.exec_us) +
        ",\"wake_latency_us\":" + histogram_json(s.wake_latency_us) + ",\"by_class\":[";
    for (std::size_t i = 0; i < kOffloadClassCount; ++i) {
        const ClassStats& c = s.by_class[i];
        if (i != 0)
            out += ',';
        out += "{\"class\":\"" + std::string(class_label(static_cast<OffloadClass>(i))) +
               "\",\"submitted_total\":" + std::to_string(c.submitted) +
               ",\"rejected_total\":" + std::to_string(c.rejected) +
               ",\"completed_done_total\":" + std::to_string(c.completed_done) +
               ",\"completed_failed_total\":" + std::to_string(c.completed_failed) +
               ",\"completed_cancelled_total\":" + std::to_string(c.completed_cancelled) +
               ",\"queue_depth\":" + std::to_string(c.queue_depth) +
               ",\"queue_depth_peak\":" + std::to_string(c.queue_depth_peak) + "}";
    }
    out += "],\"by_tenant\":[";
    for (std::size_t i = 0; i < s.by_tenant_count && i < kMaxTenantStats; ++i) {
        const TenantStats& t = s.by_tenant[i];
        if (i != 0)
            out += ',';
        out += "{\"tenant\":" + std::to_string(t.tenant) +
               ",\"submitted_total\":" + std::to_string(t.submitted) +
               ",\"rejected_total\":" + std::to_string(t.rejected) +
               ",\"completed_done_total\":" + std::to_string(t.completed_done) +
               ",\"completed_failed_total\":" + std::to_string(t.completed_failed) +
               ",\"completed_cancelled_total\":" + std::to_string(t.completed_cancelled) +
               ",\"inflight_peak\":" + std::to_string(t.inflight_peak) + "}";
    }
    out += "]}";
    return out;
}

std::string offload_metrics_prometheus() {
    const Stats s = OffloadService::instance().stats();
    std::string out;
    // TYPE is emitted once per metric name: Prometheus's text parser rejects a
    // second TYPE line for the same family, and the labelled series repeat it.
    const auto type = [&out](const std::string& name, const std::string& kind) {
        out += "# TYPE " + name + " " + kind + "\n";
    };
    const auto sample = [&out](const std::string& name, const std::string& labels,
                               std::uint64_t value) {
        out += name + labels + " " + std::to_string(value) + "\n";
    };
    type("aura_offload_submitted_total", "counter");
    type("aura_offload_rejected_total", "counter");
    type("aura_offload_completed_total", "counter");
    type("aura_offload_stale_completion_dropped_total", "counter");
    type("aura_offload_sync_fallback_total", "counter");
    type("aura_offload_queue_depth", "gauge");
    type("aura_offload_running", "gauge");
    type("aura_offload_threads", "gauge");
    type("aura_offload_queue_depth_peak", "gauge");
    type("aura_offload_class_submitted_total", "counter");
    type("aura_offload_class_rejected_total", "counter");
    type("aura_offload_class_completed_total", "counter");
    type("aura_offload_class_queue_depth", "gauge");
    type("aura_offload_tenant_submitted_total", "counter");
    type("aura_offload_tenant_rejected_total", "counter");
    type("aura_offload_tenant_completed_total", "counter");
    sample("aura_offload_submitted_total", "", s.submitted);
    sample("aura_offload_rejected_total", "{reason=\"queue_full\"}", s.rejected_queue_full);
    sample("aura_offload_rejected_total", "{reason=\"quota\"}", s.rejected_quota);
    sample("aura_offload_rejected_total", "{reason=\"shutdown\"}", s.rejected_shutdown);
    sample("aura_offload_completed_total", "{status=\"done\"}", s.completed_done);
    sample("aura_offload_completed_total", "{status=\"failed\"}", s.completed_failed);
    sample("aura_offload_completed_total", "{status=\"cancelled\"}", s.completed_cancelled);
    sample("aura_offload_stale_completion_dropped_total", "", s.stale_completion_dropped);
    sample("aura_offload_sync_fallback_total", "", s.sync_fallback);
    sample("aura_offload_queue_depth", "", s.queue_depth);
    sample("aura_offload_running", "", s.running);
    sample("aura_offload_threads", "", s.threads);
    sample("aura_offload_queue_depth_peak", "", s.queue_depth_peak);
    append_histogram_prometheus(out, "aura_offload_queue_wait_us", s.queue_wait_us);
    append_histogram_prometheus(out, "aura_offload_exec_us", s.exec_us);
    append_histogram_prometheus(out, "aura_offload_wake_latency_us", s.wake_latency_us);
    for (std::size_t i = 0; i < kOffloadClassCount; ++i) {
        const ClassStats& c = s.by_class[i];
        const std::string labels =
            std::string("{class=\"") + class_label(static_cast<OffloadClass>(i)) + "\"}";
        sample("aura_offload_class_submitted_total", labels, c.submitted);
        sample("aura_offload_class_rejected_total", labels, c.rejected);
        sample("aura_offload_class_completed_total", labels, c.completed_done);
        sample("aura_offload_class_queue_depth", labels, c.queue_depth);
    }
    for (std::size_t i = 0; i < s.by_tenant_count && i < kMaxTenantStats; ++i) {
        const TenantStats& t = s.by_tenant[i];
        const std::string labels = "{tenant=\"" + std::to_string(t.tenant) + "\"}";
        sample("aura_offload_tenant_submitted_total", labels, t.submitted);
        sample("aura_offload_tenant_rejected_total", labels, t.rejected);
        sample("aura_offload_tenant_completed_total", labels, t.completed_done);
    }
    return out;
}

} // namespace aura::offload
