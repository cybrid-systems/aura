// Issue #5400: the supervisor's scaling decision. The plan is a pure
// function over measured slack, so a fixture pins it: too little
// memory/CPU admits no worker, recovery admits more, and there is no
// fixed worker cap — 100 workers fit when the machine says so.

#include "bus/supervisor.hh"

#include <cstdio>
#include <cstdlib>

namespace {

int g_failed = 0;
void check(bool c, const char* msg) {
    if (!c) {
        ++g_failed;
        std::printf("FAIL: %s\n", msg);
    }
}

} // namespace

int main() {
    using namespace aura::bus;
    SupervisorPolicy policy{}; // 256 MB reserve, 128 MB and 25% per worker

    auto plan = [&](std::uint64_t mem, std::uint64_t cpu, std::uint64_t cpus) {
        SupervisorCapacity capacity{};
        capacity.mem_free_mb = mem;
        capacity.cpu_free_pct = cpu;
        capacity.cpus = cpus;
        return plan_workers(policy, capacity);
    };

    // Fixture low: below the reserve and with almost no idle CPU.
    {
        const SupervisorPlan p = plan(100, 5, 4);
        check(p.workers == 0, "low slack admits no worker");
        check(p.reason == 3, "low slack reason is insufficient");
    }
    // Recovery: 1000 MB free (5 slots), 50% of 8 CPUs (16 slots).
    {
        const SupervisorPlan p = plan(1000, 50, 8);
        check(p.workers == 5, "recovered slack admits memory slots");
        check(p.reason == 2, "memory is the bound");
    }
    // CPU-bound: 4 CPUs at 20% idle => 3 slots; memory has 37.
    {
        const SupervisorPlan p = plan(5000, 20, 4);
        check(p.workers == 3, "cpu slots bound the plan");
        check(p.reason == 1, "cpu is the bound");
    }
    // No fixed cap: 100 workers is a legal answer when the slack holds.
    {
        const SupervisorPlan p = plan(256 + 128 * 100, 100, 25);
        check(p.workers == 100, "no fixed worker cap");
        check(p.reason == 0, "both resources have room");
    }

    // The fixture path: environment overrides make /proc deterministic.
    {
        check(::setenv("AURA_SUPERVISOR_MEM_FREE_MB", "900", 1) == 0, "set mem fixture");
        check(::setenv("AURA_SUPERVISOR_CPU_FREE_PCT", "25", 1) == 0, "set cpu fixture");
        check(::setenv("AURA_SUPERVISOR_CPUS", "4", 1) == 0, "set cpus fixture");
        SupervisorCapacity capacity{};
        std::string why;
        check(measure_capacity(&capacity, &why), "measure with fixtures");
        check(capacity.mem_free_mb == 900, "mem fixture read");
        check(capacity.cpu_free_pct == 25, "cpu fixture read");
        check(capacity.cpus == 4, "cpus fixture read");
        const SupervisorPlan p = plan_workers(policy, capacity);
        // 900 - 256 = 644 => 5 memory slots; 25*4/25 = 4 CPU slots.
        check(p.workers == 4, "fixture plan is deterministic");
        check(p.reason == 1, "fixture is CPU-bound");
    }

    if (g_failed == 0)
        std::printf("test_bus_supervisor: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
