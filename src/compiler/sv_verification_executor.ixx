// sv_verification_executor.ixx — Issue #1233 Phase 1: verification plan executor scaffold.

module;

export module aura.compiler.sv_verification_executor;

import std;

export namespace aura::compiler::sv_verify {

inline constexpr int kSvVerificationExecutorPhase = 1;

enum class VerifyStage : std::uint8_t {
    Compile = 0,
    Elaborate = 1,
    Simulate = 2,
    CollectCoverage = 3,
    Feedback = 4,
};

struct VerificationPlan {
    std::string_view name;
    std::uint32_t timeout_ms = 0;
    bool enable_coverage = true;
};

struct SVVerificationExecutorStats {
    std::uint64_t plans_run = 0;
    std::uint64_t stages_ok = 0;
    std::uint64_t stages_fail = 0;
    std::uint64_t feedback_loops = 0;
};

inline SVVerificationExecutorStats g_sv_verify_stats{};

// Phase 1: stage orchestration counters; commercial tool peel in #1237.
struct SVVerificationExecutor {
    [[nodiscard]] bool run_plan(const VerificationPlan& plan) {
        ++g_sv_verify_stats.plans_run;
        (void)plan;
        // Stub pipeline: mark each stage as success for scaffolding.
        for (int i = 0; i < 5; ++i)
            ++g_sv_verify_stats.stages_ok;
        ++g_sv_verify_stats.feedback_loops;
        return true;
    }
};

inline SVVerificationExecutor g_sv_verification_executor{};

} // namespace aura::compiler::sv_verify
