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

OffloadService::Submission OffloadService::submit(Job job,
                                                  std::shared_ptr<const SnapshotInput> input) {
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
    out.status = pool_->try_submit(std::move(job), std::move(input), out.ticket, sink_.get());
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

Stats OffloadService::stats() const {
    return pool_ ? pool_->stats() : Stats{};
}

std::uint64_t OffloadService::stale_completions() const {
    return sched_ ? sched_->offload_stale_completions() : 0;
}

} // namespace aura::offload
