// offload/offload_primitives.cpp — bridge for the offload:* primitives (#4888)
//
// The evaluator module cannot construct a CompilerService (module cycle), so
// this non-module TU owns the ticket registry and the private-compiler job
// factory. It registers the bridge at static init when linked into the aura
// binary; evaluator-only test targets keep the null bridge and the primitives
// report offload-unavailable.
#include "compiler/messaging_bridge.h"
#include "offload_job.h"
#include "offload_service.h"

#include <cstdint>

namespace aura::messaging {
namespace {

    using aura::offload::OffloadClass;
    using aura::offload::OffloadService;
    using aura::offload::Result;
    using aura::offload::SnapshotInput;
    using aura::offload::Status;
    using aura::offload::Ticket;

    struct TicketRegistry {
        std::mutex mutex;
        std::unordered_map<int, std::shared_ptr<Ticket>> tickets;
        std::uint64_t next_snapshot_id = 1;
        int next_ticket_id = 1;
    };

    TicketRegistry& registry() {
        static TicketRegistry reg;
        return reg;
    }

    OffloadClass parse_class(const std::string& name) {
        if (name == "interactive" || name == ":interactive")
            return OffloadClass::Interactive;
        if (name == "batch" || name == ":batch")
            return OffloadClass::Batch;
        return OffloadClass::Compile;
    }

    const char* status_name(Status status) noexcept {
        switch (status) {
            case Status::Queued:
                return "queued";
            case Status::Running:
                return "running";
            case Status::Done:
                return "done";
            case Status::Failed:
                return "failed";
            case Status::Cancelled:
                return "cancelled";
            case Status::Rejected:
                return "rejected";
        }
        return "unknown";
    }

    void fill_result(const std::shared_ptr<Ticket>& ticket, OffloadTicketResult& out) {
        out = OffloadTicketResult{};
        out.status = status_name(ticket->status.load(std::memory_order_acquire));
        out.mode = ticket->fiber_id == 0 ? "sync" : "async";
        if (auto* result = std::get_if<Result>(&ticket->out)) {
            for (const auto& diag : result->diagnostics)
                out.diagnostics.push_back(diag.message);
            out.tests_passed = result->tests.passed;
            out.tests_failed = result->tests.failed;
            out.test_failures = result->tests.failures;
            out.queue_us = result->timing.queue_us;
            out.exec_us = result->timing.exec_us;
        } else if (auto* error = std::get_if<aura::offload::Error>(&ticket->out)) {
            out.error = error->message;
        }
    }

    int submit_impl(std::string source, std::vector<std::string> stages, std::string klass) {
        auto& reg = registry();
        std::uint64_t snapshot_id = 0;
        {
            std::scoped_lock lock(reg.mutex);
            snapshot_id = reg.next_snapshot_id++;
        }
        auto input =
            std::make_shared<const SnapshotInput>(SnapshotInput{std::move(source), snapshot_id});
        auto job = [stages = std::move(stages)](const SnapshotInput& in, std::stop_token st) {
            return aura::offload::run_stages(in, stages, st);
        };
        auto submission =
            OffloadService::instance().submit(std::move(job), std::move(input), parse_class(klass));
        switch (submission.status) {
            case aura::offload::SubmitStatus::Accepted:
                break;
            case aura::offload::SubmitStatus::QueueFull:
                return -1;
            case aura::offload::SubmitStatus::QuotaExceeded:
                return -2;
            case aura::offload::SubmitStatus::ShuttingDown:
                return -3;
        }
        std::scoped_lock lock(reg.mutex);
        const int id = reg.next_ticket_id++;
        reg.tickets.emplace(id, std::move(submission.ticket));
        return id;
    }

    std::shared_ptr<Ticket> find_ticket(int id) {
        auto& reg = registry();
        std::scoped_lock lock(reg.mutex);
        auto it = reg.tickets.find(id);
        return it == reg.tickets.end() ? nullptr : it->second;
    }

    bool await_impl(int ticket_id, std::uint64_t timeout_ms, OffloadTicketResult& out) {
        auto ticket = find_ticket(ticket_id);
        if (!ticket)
            return false;
        bool terminal = false;
        if (timeout_ms == 0) {
            OffloadService::instance().await(ticket);
            terminal = true;
        } else {
            terminal =
                OffloadService::instance().await_for(ticket, std::chrono::milliseconds(timeout_ms));
        }
        if (!terminal)
            return false;
        fill_result(ticket, out);
        return true;
    }

    std::string poll_impl(int ticket_id) {
        auto ticket = find_ticket(ticket_id);
        if (!ticket)
            return "unknown";
        return status_name(ticket->status.load(std::memory_order_acquire));
    }

    bool cancel_impl(int ticket_id) {
        auto ticket = find_ticket(ticket_id);
        if (!ticket)
            return false;
        ticket->stop.request_stop();
        return true;
    }

    struct Registrar {
        Registrar() noexcept { // #4936: static init must not throw
            g_offload_submit = &submit_impl;
            g_offload_await = &await_impl;
            g_offload_poll = &poll_impl;
            g_offload_cancel = &cancel_impl;
        }
    };

    const Registrar g_registrar;

} // namespace
} // namespace aura::messaging
