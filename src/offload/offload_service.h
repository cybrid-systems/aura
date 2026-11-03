// offload/offload_service.h — runtime bridge between OffloadPool and Scheduler
#ifndef AURA_OFFLOAD_OFFLOAD_SERVICE_H
#define AURA_OFFLOAD_OFFLOAD_SERVICE_H

#include "offload_pool.h"

#include <memory>

namespace aura::serve {
class Scheduler;
}

namespace aura::offload {

// Issue #4883: owns the pool and translates Ticket completion into the
// scheduler's single completion eventfd. A fiber submits a Job, bumps its
// wait generation, and parks; the IO thread resumes only the matching park.
class OffloadService {
public:
    static OffloadService& instance();

    void start(aura::serve::Scheduler* sched);
    void start(aura::serve::Scheduler* sched, OffloadPool::Config cfg);
    void stop();

    [[nodiscard]] bool active() const noexcept { return pool_ != nullptr; }

    struct Submission {
        SubmitStatus status = SubmitStatus::ShuttingDown;
        std::shared_ptr<Ticket> ticket;
    };

    // Non-fiber context: runs the job inline and returns a terminal ticket.
    // Fiber context: submits to the pool and records fiber_id/wait_gen.
    [[nodiscard]] Submission submit(Job job, std::shared_ptr<const SnapshotInput> input);

    // Park the current fiber until the ticket is terminal. No-op outside a
    // fiber (the synchronous submit is already terminal).
    void await(const std::shared_ptr<Ticket>& ticket);

    [[nodiscard]] Stats stats() const;
    [[nodiscard]] std::uint64_t stale_completions() const;

private:
    class Sink;

    std::unique_ptr<Sink> sink_;
    std::unique_ptr<OffloadPool> pool_;
    aura::serve::Scheduler* sched_ = nullptr;
};

} // namespace aura::offload

#endif // AURA_OFFLOAD_OFFLOAD_SERVICE_H
