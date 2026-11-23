// ──────────────────────────────────────────────────────────────
//  supervisor.hh — the bus supervisor's scaling decision (#5400)
//
//  The supervisor decides how many workers the machine can hold from
//  its *actual* CPU and memory slack, not from a fixed cap. The
//  decision is a pure function over a measured Capacity so a test can
//  inject a fixture; the host measures /proc and records the plan in
//  the audit log.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_SUPERVISOR_HH
#define AURA_BUS_SUPERVISOR_HH

#include <cstdint>
#include <string>

namespace aura::bus {

// The machine's slack at decision time. cpu_free_pct is the idle share
// across all online CPUs (0-100); cpus is the online CPU count.
struct SupervisorCapacity {
    std::uint64_t mem_free_mb = 0;
    std::uint64_t cpu_free_pct = 0;
    std::uint64_t cpus = 0;
};

struct SupervisorPolicy {
    // Memory the host keeps for itself before any worker is admitted.
    std::uint64_t reserve_mb = 256;
    // A worker's working set and CPU share, in MB and percent of one CPU.
    std::uint64_t per_worker_mb = 128;
    std::uint64_t per_worker_cpu_pct = 25;
};

struct SupervisorPlan {
    std::uint64_t workers = 0;
    // 0 = both resources have room, 1 = CPU-bound, 2 = memory-bound,
    // 3 = not enough slack for even one worker.
    std::uint64_t reason = 3;
    std::uint64_t cpu_slots = 0;
    std::uint64_t mem_slots = 0;
};

// Pure decision over an explicit capacity. No cap on the returned
// worker count: the machine's slack is the only bound.
SupervisorPlan plan_workers(const SupervisorPolicy& policy, const SupervisorCapacity& capacity);

// Read the live capacity. AURA_SUPERVISOR_MEM_FREE_MB,
// AURA_SUPERVISOR_CPU_FREE_PCT and AURA_SUPERVISOR_CPUS are test
// fixtures: when set they replace the corresponding /proc reading, so
// CI can pin the decision. Without them Linux /proc answers; on other
// platforms the plan is empty and *why says so.
bool measure_capacity(SupervisorCapacity* out, std::string* why);

} // namespace aura::bus

#endif
