// serve/metrics.h — Scheduler performance counters and instrumentation
//
// Phase 3: Tuning metrics for the M:N work-stealing fiber scheduler.
//
// Collects:
//   - Fiber lifecycle counters (spawned/completed/waiting)
//   - Steal attempt/success/failure rates
//   - Worker utilization (busy cycles, idle cycles)
//   - Local queue depth samples
//   - Eventfd wake counts
//   - Thread pool job counters
//
// All counters are lock-free atomics. Metrics are indexed per-worker
// and aggregated globally. Dump via metrics::summary() or JSON.
//
#ifndef AURA_SERVE_METRICS_H
#define AURA_SERVE_METRICS_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <memory>
#include <chrono>

namespace aura::serve::metrics {

// ── Per-worker metrics ────────────────────────────────
//
struct WorkerMetrics {
    // Fiber lifecycle
    std::atomic<uint64_t> fibers_executed{0};
    std::atomic<uint64_t> fibers_yielded{0};
    std::atomic<uint64_t> fibers_waiting{0};

    // Steal operations
    std::atomic<uint64_t> steal_attempts{0};
    std::atomic<uint64_t> steal_successes{0};
    std::atomic<uint64_t> steal_failures{0};

    // Local queue
    std::atomic<uint64_t> local_pushes{0};
    std::atomic<uint64_t> local_pops{0};

    // Timing (nanosecond accumulators, approximate)
    std::atomic<int64_t> busy_ns{0};
    std::atomic<int64_t> idle_ns{0};

    // Wake events
    std::atomic<uint64_t> wake_events{0};

    // Queue depth samples (for histogram)
    std::atomic<uint64_t> qdepth_max{0};
    std::atomic<uint64_t> qdepth_samples{0};
    std::atomic<uint64_t> qdepth_sum{0};

    void record_busy(int64_t ns) { busy_ns.fetch_add(ns, std::memory_order_relaxed); }

    void record_idle(int64_t ns) { idle_ns.fetch_add(ns, std::memory_order_relaxed); }

    void record_qdepth(size_t depth) {
        uint64_t d = static_cast<uint64_t>(depth);
        qdepth_sum.fetch_add(d, std::memory_order_relaxed);
        qdepth_samples.fetch_add(1, std::memory_order_relaxed);
        // Update max
        uint64_t cur = qdepth_max.load(std::memory_order_relaxed);
        while (d > cur && !qdepth_max.compare_exchange_weak(cur, d, std::memory_order_relaxed)) {
        }
    }

    double avg_qdepth() const {
        auto samples = qdepth_samples.load(std::memory_order_acquire);
        return samples ? static_cast<double>(qdepth_sum.load(std::memory_order_acquire)) /
                             static_cast<double>(samples)
                       : 0.0;
    }

    double steal_success_rate() const {
        auto attempts = steal_attempts.load(std::memory_order_acquire);
        return attempts ? static_cast<double>(steal_successes.load(std::memory_order_acquire)) *
                              100.0 / static_cast<double>(attempts)
                        : 0.0;
    }

    double utilization() const {
        auto busy = busy_ns.load(std::memory_order_acquire);
        auto idle = idle_ns.load(std::memory_order_acquire);
        auto total = busy + idle;
        return total ? static_cast<double>(busy) * 100.0 / static_cast<double>(total) : 0.0;
    }

    void dump(int id, FILE* out = stdout) const {
        std::fprintf(out,
                     "  [W-%d] fibers=%lu yields=%lu waits=%lu\n"
                     "         steals=%lu/%lu (%.1f%%) pushes=%lu pops=%lu\n"
                     "         util=%.1f%% busy=%ldms idle=%ldms\n"
                     "         wakups=%lu avg_qd=%.1f max_qd=%lu\n",
                     id, (unsigned long)fibers_executed.load(std::memory_order_acquire),
                     (unsigned long)fibers_yielded.load(std::memory_order_acquire),
                     (unsigned long)fibers_waiting.load(std::memory_order_acquire),
                     (unsigned long)steal_successes.load(std::memory_order_acquire),
                     (unsigned long)steal_attempts.load(std::memory_order_acquire),
                     steal_success_rate(),
                     (unsigned long)local_pushes.load(std::memory_order_acquire),
                     (unsigned long)local_pops.load(std::memory_order_acquire), utilization(),
                     (unsigned long)(busy_ns.load(std::memory_order_acquire) / 1000000),
                     (unsigned long)(idle_ns.load(std::memory_order_acquire) / 1000000),
                     (unsigned long)wake_events.load(std::memory_order_acquire), avg_qdepth(),
                     (unsigned long)qdepth_max.load(std::memory_order_acquire));
    }
};

// ── Global scheduler metrics ──────────────────────────
//
struct GlobalMetrics {
    // Aggregate
    std::atomic<uint64_t> fibers_spawned{0};
    std::atomic<uint64_t> fibers_completed{0};
    std::atomic<uint64_t> fibers_waiting_peak{0};

    // IO events
    std::atomic<uint64_t> io_events_processed{0};
    std::atomic<uint64_t> io_stdin_events{0};

    // Errors
    std::atomic<uint64_t> schedule_errors{0};

    // Per-worker metrics (stored as unique_ptr to avoid atomic copy issues)
    std::vector<std::unique_ptr<WorkerMetrics>> workers;

    explicit GlobalMetrics(size_t num_workers = 0) { resize_workers(num_workers); }

    void resize_workers(size_t n) {
        workers.clear();
        for (size_t i = 0; i < n; ++i) {
            workers.push_back(std::make_unique<WorkerMetrics>());
        }
    }

    WorkerMetrics& worker(size_t id) { return *workers[id]; }

    const WorkerMetrics& worker(size_t id) const { return *workers[id]; }

    size_t num_workers() const { return workers.size(); }

    // ── Summary dump ─────────────────────────────────
    void dump(FILE* out = stdout) const {
        auto spawned = fibers_spawned.load(std::memory_order_acquire);
        auto completed = fibers_completed.load(std::memory_order_acquire);
        auto io_events = io_events_processed.load(std::memory_order_acquire);
        auto io_stdin = io_stdin_events.load(std::memory_order_acquire);
        auto errors = schedule_errors.load(std::memory_order_acquire);

        // Aggregate steal stats
        uint64_t total_steal_attempts = 0;
        uint64_t total_steal_successes = 0;
        for (const auto& w : workers) {
            total_steal_attempts += w->steal_attempts.load(std::memory_order_acquire);
            total_steal_successes += w->steal_successes.load(std::memory_order_acquire);
        }

        std::fprintf(out,
                     "\n═══ Scheduler Metrics ═══\n"
                     "  fibers: %lu spawned, %lu completed\n"
                     "  steal:  %lu/%lu (%.1f%% success)\n"
                     "  IO:     %lu events (%lu stdin)\n"
                     "  errors: %lu\n",
                     (unsigned long)spawned, (unsigned long)completed,
                     (unsigned long)total_steal_successes, (unsigned long)total_steal_attempts,
                     total_steal_attempts ? static_cast<double>(total_steal_successes) * 100.0 /
                                                static_cast<double>(total_steal_attempts)
                                          : 0.0,
                     (unsigned long)io_events, (unsigned long)io_stdin, (unsigned long)errors);

        for (size_t i = 0; i < workers.size(); ++i) {
            workers[i]->dump(static_cast<int>(i), out);
        }
    }

    // ── JSON dump ────────────────────────────────────
    std::string to_json() const {
        std::string json = "{\n";
        auto append = [&](const char* key, auto val) {
            json += "  \"" + std::string(key) + "\": " + std::to_string(val) + ",\n";
        };

        append("fibers_spawned", fibers_spawned.load(std::memory_order_acquire));
        append("fibers_completed", fibers_completed.load(std::memory_order_acquire));
        append("io_events", io_events_processed.load(std::memory_order_acquire));
        append("io_stdin", io_stdin_events.load(std::memory_order_acquire));
        append("schedule_errors", schedule_errors.load(std::memory_order_acquire));

        // Aggregate steal
        uint64_t steal_a = 0, steal_s = 0;
        for (const auto& w : workers) {
            steal_a += w->steal_attempts.load(std::memory_order_acquire);
            steal_s += w->steal_successes.load(std::memory_order_acquire);
        }
        append("steal_attempts", steal_a);
        steal_s < steal_a ? (json += "  \"steal_successes\": " + std::to_string(steal_s) + ",\n")
                          : (json += "  \"steal_successes\": " + std::to_string(steal_s) + ",\n");

        json += "  \"workers\": [\n";
        for (size_t i = 0; i < workers.size(); ++i) {
            const auto& w = workers[i];
            json += "    {\n";
            auto wj = [&](const char* k, auto v) {
                json += "      \"" + std::string(k) + "\": " + std::to_string(v) + ",\n";
            };
            wj("id", i);
            wj("fibers_executed", w->fibers_executed.load(std::memory_order_acquire));
            wj("fibers_yielded", w->fibers_yielded.load(std::memory_order_acquire));
            wj("fibers_waiting", w->fibers_waiting.load(std::memory_order_acquire));
            wj("steal_attempts", w->steal_attempts.load(std::memory_order_acquire));
            wj("steal_successes", w->steal_successes.load(std::memory_order_acquire));
            wj("local_pushes", w->local_pushes.load(std::memory_order_acquire));
            wj("local_pops", w->local_pops.load(std::memory_order_acquire));
            wj("busy_ns", w->busy_ns.load(std::memory_order_acquire));
            wj("idle_ns", w->idle_ns.load(std::memory_order_acquire));
            wj("wake_events", w->wake_events.load(std::memory_order_acquire));
            wj("qdepth_max", w->qdepth_max.load(std::memory_order_acquire));
            wj("qdepth_samples", w->qdepth_samples.load(std::memory_order_acquire));
            wj("qdepth_sum", w->qdepth_sum.load(std::memory_order_acquire));
            // Remove trailing comma
            json.pop_back();
            json.pop_back();
            json += "\n    }";
            if (i + 1 < workers.size())
                json += ",";
            json += "\n";
        }
        json += "  ]\n}\n";
        return json;
    }

    // Issue #677: Prometheus text exposition for /metrics scrape.
    std::string to_prometheus() const {
        std::string out;
        auto append_counter = [&](const std::string& name, std::uint64_t val) {
            out += "# TYPE " + name + " counter\n" + name + " " + std::to_string(val) + "\n";
        };
        append_counter(std::string("aura_fibers_spawned"),
                       fibers_spawned.load(std::memory_order_acquire));
        append_counter(std::string("aura_fibers_completed"),
                       fibers_completed.load(std::memory_order_acquire));
        append_counter(std::string("aura_io_events_processed"),
                       io_events_processed.load(std::memory_order_acquire));
        append_counter(std::string("aura_io_stdin_events"),
                       io_stdin_events.load(std::memory_order_acquire));
        append_counter(std::string("aura_schedule_errors"),
                       schedule_errors.load(std::memory_order_acquire));
        std::uint64_t steal_a = 0, steal_s = 0;
        for (const auto& w : workers) {
            steal_a += w->steal_attempts.load(std::memory_order_acquire);
            steal_s += w->steal_successes.load(std::memory_order_acquire);
        }
        append_counter(std::string("aura_steal_attempts"), steal_a);
        append_counter(std::string("aura_steal_successes"), steal_s);
        for (std::size_t i = 0; i < workers.size(); ++i) {
            const auto& w = workers[i];
            const std::string prefix = "aura_worker_" + std::to_string(i) + "_";
            append_counter(prefix + "fibers_executed",
                         w->fibers_executed.load(std::memory_order_acquire));
            append_counter(prefix + "fibers_yielded",
                           w->fibers_yielded.load(std::memory_order_acquire));
        }
        return out;
    }
};

// ── Per-fiber trace events (lightweight, compile-time opt-in) ──
//
// When AURA_FIBER_TRACE is defined, each fiber records its
// lifecycle as a ring buffer of trace events for post-mortem
// debugging. Disabled by default to avoid overhead.
//
#ifdef AURA_FIBER_TRACE

struct TraceEvent {
    enum Type : uint8_t {
        Spawn,
        Resume,
        Yield,
        Wait,
        Done,
    };
    Type type;
    uint64_t fiber_id;
    int64_t timestamp_ns; // steady_clock
};

// Ring buffer per fiber (global pool managed by scheduler)
struct TraceBuffer {
    static constexpr size_t CAPACITY = 256;
    TraceEvent events[CAPACITY];
    std::atomic<uint32_t> head{0}; // write cursor

    void record(TraceEvent::Type type, uint64_t fiber_id) {
        auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
        uint32_t pos = head.fetch_add(1, std::memory_order_relaxed) % CAPACITY;
        events[pos] = {type, fiber_id, now};
    }
};

inline const char* trace_type_name(TraceEvent::Type t) {
    switch (t) {
        case TraceEvent::Spawn:
            return "spawn";
        case TraceEvent::Resume:
            return "resume";
        case TraceEvent::Yield:
            return "yield";
        case TraceEvent::Wait:
            return "wait";
        case TraceEvent::Done:
            return "done";
    }
    return "?";
}

#endif // AURA_FIBER_TRACE

} // namespace aura::serve::metrics

#endif // AURA_SERVE_METRICS_H
