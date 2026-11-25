// Issue #5400: the supervisor's scaling decision. The plan is a pure
// function over measured slack, so a fixture pins it: too little
// memory/CPU admits no worker, recovery admits more, and there is no
// fixed worker cap — 100 workers fit when the machine says so.

#include "bus/supervisor.hh"

#include <chrono>
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

    // #5423: production policy knobs; zero/invalid values keep the
    // default bound instead of disabling it.
    {
        check(::setenv("AURA_SUPERVISOR_RESERVE_MB", "512", 1) == 0, "set reserve knob");
        check(::setenv("AURA_SUPERVISOR_PER_WORKER_MB", "64", 1) == 0, "set worker mb knob");
        check(::setenv("AURA_SUPERVISOR_PER_WORKER_CPU_PCT", "50", 1) == 0, "set worker cpu knob");
        const SupervisorPolicy tuned = supervisor_policy_from_env();
        check(tuned.reserve_mb == 512, "reserve knob read");
        check(tuned.per_worker_mb == 64, "worker mb knob read");
        check(tuned.per_worker_cpu_pct == 50, "worker cpu knob read");

        check(::setenv("AURA_SUPERVISOR_PER_WORKER_MB", "0", 1) == 0, "set zero worker mb");
        check(::setenv("AURA_SUPERVISOR_PER_WORKER_CPU_PCT", "0", 1) == 0, "set zero worker cpu");
        const SupervisorPolicy fallback = supervisor_policy_from_env();
        check(fallback.per_worker_mb == 128, "zero worker mb keeps the default");
        check(fallback.per_worker_cpu_pct == 25, "zero worker cpu keeps the default");
        unsetenv("AURA_SUPERVISOR_RESERVE_MB");
        unsetenv("AURA_SUPERVISOR_PER_WORKER_MB");
        unsetenv("AURA_SUPERVISOR_PER_WORKER_CPU_PCT");
    }

    // #5457: a real /proc/stat sample must not sleep on the coordinator
    // thread. 50 calls with only the memory fixture pinned used to take
    // 50 x 20 ms; the cached/delta sampler is microseconds.
    {
        check(::setenv("AURA_SUPERVISOR_MEM_FREE_MB", "900", 1) == 0, "set mem fixture for timing");
        unsetenv("AURA_SUPERVISOR_CPU_FREE_PCT");
        unsetenv("AURA_SUPERVISOR_CPUS");
        SupervisorCapacity capacity{};
        std::string why;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 50; ++i)
            check(measure_capacity(&capacity, &why), "measure_capacity without CPU fixture");
        const auto ns = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        check(ns < 500, "50 CPU samples do not sleep 20 ms each");
        check(capacity.cpu_free_pct <= 100, "sampled CPU percentage is a share");
        check(capacity.cpus > 0, "sampled CPU count is positive");
        unsetenv("AURA_SUPERVISOR_MEM_FREE_MB");
    }

    if (g_failed == 0)
        std::printf("test_bus_supervisor: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
