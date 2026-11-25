// supervisor.cpp — Issue #5400: worker capacity from real slack.
#include "bus/supervisor.hh"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <mutex>
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

    // Issue #5457: measure_cpu used to read /proc/stat, sleep 20 ms and read
    // it again. That ran on the coordinator's only poll thread, so every
    // bus:supervise froze every peer for 20 ms. Keep one process-wide sample
    // and derive the idle share from the delta since that sample; calls
    // within 100 ms reuse the percentage instead of sampling again, so a
    // tight supervisor loop never blocks the coordinator.
    struct CpuSample {
        std::uint64_t idle = 0;
        std::uint64_t total = 0;
        std::uint64_t free_pct = 100;
        std::uint64_t cpus = 1;
        bool have = false;
        std::chrono::steady_clock::time_point at{};
    };

    CpuSample g_cpu_sample{};
    std::mutex g_cpu_sample_mutex;

    bool measure_cpu(std::uint64_t* free_pct, std::uint64_t* cpus) {
        std::scoped_lock lock(g_cpu_sample_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (g_cpu_sample.have && now - g_cpu_sample.at < std::chrono::milliseconds(100)) {
            *free_pct = g_cpu_sample.free_pct;
            *cpus = g_cpu_sample.cpus;
            return true;
        }
        std::uint64_t idle = 0, total = 0;
        if (!read_cpu_times(&idle, &total)) {
            if (!g_cpu_sample.have)
                return false;
            *free_pct = g_cpu_sample.free_pct;
            *cpus = g_cpu_sample.cpus;
            return true;
        }
        const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
        std::uint64_t pct = 100;
        if (g_cpu_sample.have && total > g_cpu_sample.total && idle >= g_cpu_sample.idle) {
            const std::uint64_t dtotal = total - g_cpu_sample.total;
            const std::uint64_t didle = idle - g_cpu_sample.idle;
            pct = std::min<std::uint64_t>(100, (didle * 100) / dtotal);
        } else if (n > 0) {
            // First sample: the 1-minute load average is current enough and
            // costs no sleep; the next sample after 100 ms uses the delta.
            double load[1] = {0.0};
            if (::getloadavg(load, 1) == 1) {
                const double busy_pct = load[0] * 100.0 / static_cast<double>(n);
                pct = busy_pct >= 100.0 ? 0 : 100 - static_cast<std::uint64_t>(busy_pct);
            } else if (total > 0) {
                pct = std::min<std::uint64_t>(100, (idle * 100) / total);
            }
        } else if (total > 0) {
            pct = std::min<std::uint64_t>(100, (idle * 100) / total);
        }
        g_cpu_sample.idle = idle;
        g_cpu_sample.total = total;
        g_cpu_sample.free_pct = pct;
        g_cpu_sample.cpus = n > 0 ? static_cast<std::uint64_t>(n) : 1;
        g_cpu_sample.at = now;
        g_cpu_sample.have = true;
        *free_pct = pct;
        *cpus = g_cpu_sample.cpus;
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
