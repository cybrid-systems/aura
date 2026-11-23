// supervisor.cpp — Issue #5400: worker capacity from real slack.
#include "bus/supervisor.hh"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>

#include <unistd.h>

namespace aura::bus {
namespace {

    bool env_u64(const char* name, std::uint64_t* out) {
        const char* raw = std::getenv(name);
        if (raw == nullptr || *raw == '\0')
            return false;
        char* end = nullptr;
        const unsigned long long value = std::strtoull(raw, &end, 10);
        if (end == raw || *end != '\0')
            return false;
        *out = static_cast<std::uint64_t>(value);
        return true;
    }

    // /proc/stat's first line: cpu user nice system idle iowait irq softirq steal ...
    bool read_cpu_times(std::uint64_t* idle, std::uint64_t* total) {
        std::ifstream in("/proc/stat");
        if (!in)
            return false;
        std::string line;
        if (!std::getline(in, line))
            return false;
        std::istringstream fields(line);
        std::string cpu;
        if (!(fields >> cpu) || cpu != "cpu")
            return false;
        std::uint64_t sum = 0;
        std::uint64_t idle_units = 0;
        std::uint64_t value = 0;
        for (int i = 0; i < 10 && fields >> value; ++i) {
            sum += value;
            if (i == 3 || i == 4) // idle + iowait
                idle_units += value;
        }
        if (sum == 0)
            return false;
        *idle = idle_units;
        *total = sum;
        return true;
    }

    // MemAvailable is the kernel's own estimate of what a new worker can
    // take without swapping; MemFree would overstate it.
    bool read_mem_available_mb(std::uint64_t* out) {
        std::ifstream in("/proc/meminfo");
        if (!in)
            return false;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.starts_with("MemAvailable:"))
                continue;
            std::istringstream fields(line.substr(13));
            std::uint64_t kb = 0;
            if (!(fields >> kb))
                return false;
            *out = kb / 1024;
            return true;
        }
        return false;
    }

    bool measure_cpu(std::uint64_t* free_pct, std::uint64_t* cpus) {
        std::uint64_t idle0 = 0, total0 = 0, idle1 = 0, total1 = 0;
        if (!read_cpu_times(&idle0, &total0))
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!read_cpu_times(&idle1, &total1))
            return false;
        const std::uint64_t total = total1 - total0;
        const std::uint64_t idle = idle1 - idle0;
        if (total == 0)
            return false;
        *free_pct = std::min<std::uint64_t>(100, (idle * 100) / total);
        const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
        *cpus = n > 0 ? static_cast<std::uint64_t>(n) : 1;
        return true;
    }

} // namespace

SupervisorPlan plan_workers(const SupervisorPolicy& policy, const SupervisorCapacity& capacity) {
    SupervisorPlan plan{};
    if (policy.per_worker_mb == 0 || policy.per_worker_cpu_pct == 0)
        return plan;
    plan.mem_slots = capacity.mem_free_mb > policy.reserve_mb
                         ? (capacity.mem_free_mb - policy.reserve_mb) / policy.per_worker_mb
                         : 0;
    // cpu_free_pct is the idle share of the whole machine; multiply by
    // the online CPU count to get idle "percent of one CPU" units.
    plan.cpu_slots = (capacity.cpu_free_pct * capacity.cpus) / policy.per_worker_cpu_pct;
    plan.workers = std::min(plan.mem_slots, plan.cpu_slots);
    if (plan.workers == 0) {
        plan.reason = 3;
        return plan;
    }
    if (plan.mem_slots < plan.cpu_slots)
        plan.reason = 2;
    else if (plan.cpu_slots < plan.mem_slots)
        plan.reason = 1;
    else
        plan.reason = 0;
    return plan;
}

SupervisorPolicy supervisor_policy_from_env() {
    SupervisorPolicy policy{};
    std::uint64_t value = 0;
    if (env_u64("AURA_SUPERVISOR_RESERVE_MB", &value))
        policy.reserve_mb = value;
    if (env_u64("AURA_SUPERVISOR_PER_WORKER_MB", &value) && value > 0)
        policy.per_worker_mb = value;
    if (env_u64("AURA_SUPERVISOR_PER_WORKER_CPU_PCT", &value) && value > 0)
        policy.per_worker_cpu_pct = value;
    return policy;
}

bool measure_capacity(SupervisorCapacity* out, std::string* why) {
    SupervisorCapacity capacity{};
    bool ok = true;
    if (!env_u64("AURA_SUPERVISOR_MEM_FREE_MB", &capacity.mem_free_mb)) {
        if (!read_mem_available_mb(&capacity.mem_free_mb)) {
            ok = false;
            if (why)
                *why = "cannot read MemAvailable from /proc/meminfo";
        }
    }
    if (!env_u64("AURA_SUPERVISOR_CPU_FREE_PCT", &capacity.cpu_free_pct)) {
        std::uint64_t cpus = 0;
        if (!measure_cpu(&capacity.cpu_free_pct, &cpus)) {
            ok = false;
            if (why)
                *why = "cannot sample /proc/stat";
        }
        if (!env_u64("AURA_SUPERVISOR_CPUS", &capacity.cpus))
            capacity.cpus = cpus;
    } else if (!env_u64("AURA_SUPERVISOR_CPUS", &capacity.cpus)) {
        const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
        capacity.cpus = n > 0 ? static_cast<std::uint64_t>(n) : 1;
    }
    if (capacity.cpus == 0)
        capacity.cpus = 1;
    if (!ok)
        return false;
    *out = capacity;
    return true;
}

} // namespace aura::bus
