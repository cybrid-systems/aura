// orch.ixx — Issue #1588: unified orchestration module inventory.
// Production APIs live in orch/orch.h + orch/agent_spawn.h (header form).

module;

export module aura.orch;

import std;

export namespace aura::orch {

inline constexpr int kOrchModulePhase = 1;
inline constexpr int kOrchModuleIssue = 1588;

// Component map (serve/ building blocks composed by this module).
inline constexpr const char* kOrchComponents[] = {
    "agent_spawn",
    "multi_fiber_mailbox",
    "parallel_orch",
    "fiber_join",
};

[[nodiscard]] inline std::size_t orch_component_count() noexcept {
    return sizeof(kOrchComponents) / sizeof(kOrchComponents[0]);
}

} // namespace aura::orch
