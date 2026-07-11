// parallel_orch.ixx — Issue #1202 Phase 1: parallel agent orchestration scaffold.

module;

export module aura.serve.parallel_orch;

import std;

export namespace aura::serve::parallel_orch {

inline constexpr int kParallelOrchPhase = 1;

struct ParallelPolicy {
    std::uint32_t max_concurrency = 8;
    std::uint32_t timeout_ms = 0; // 0 = no timeout
    bool fail_fast = false;
};

struct ParallelOrchStats {
    std::uint64_t intend_batches = 0;
    std::uint64_t tasks_spawned = 0;
    std::uint64_t tasks_joined = 0;
    std::uint64_t fail_fast_aborts = 0;
};

inline ParallelOrchStats g_parallel_orch_stats{};

// Phase 1: policy validation + counters. Full fiber spawn peel uses #1198/#1200.
[[nodiscard]] inline bool validate_policy(const ParallelPolicy& p) noexcept {
    return p.max_concurrency > 0 && p.max_concurrency <= 1024;
}

inline void record_batch(std::uint32_t n_tasks) noexcept {
    ++g_parallel_orch_stats.intend_batches;
    g_parallel_orch_stats.tasks_spawned += n_tasks;
}

} // namespace aura::serve::parallel_orch
