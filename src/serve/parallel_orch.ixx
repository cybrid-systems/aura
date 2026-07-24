// parallel_orch.ixx — Issue #1586 / #1202: parallel agent orchestration.
// Phase tag + module inventory. Production API lives in parallel_orch.h.

module;

export module aura.serve.parallel_orch;

import std;

export namespace aura::serve::parallel_orch {

// Production phase (header API: parallel_orch.h).
inline constexpr int kParallelOrchPhase = 2;
inline constexpr int kParallelOrchIssue = 1586;

// Scaffold counters retained for Phase-1 sweep queries (#1202).
// Live counters + FailurePolicy are in parallel_orch.h (g_parallel_orch_stats).
// #2007: production API lives in the header; module keeps policy shape notes.
struct ParallelPolicy {
    std::uint32_t max_concurrency = 8;
    std::uint32_t timeout_ms = 0;
    bool fail_fast = false;
    bool collect_errors = true;
};

struct ParallelOrchStats {
    std::uint64_t intend_batches = 0;
    std::uint64_t tasks_spawned = 0;
    std::uint64_t tasks_joined = 0;
    std::uint64_t fail_fast_aborts = 0;
};

inline ParallelOrchStats g_parallel_orch_stats_module{};

[[nodiscard]] inline bool validate_policy(const ParallelPolicy& p) noexcept {
    return p.max_concurrency > 0 && p.max_concurrency <= 1024;
}

inline void record_batch(std::uint32_t n_tasks) noexcept {
    ++g_parallel_orch_stats_module.intend_batches;
    g_parallel_orch_stats_module.tasks_spawned += n_tasks;
}

} // namespace aura::serve::parallel_orch
