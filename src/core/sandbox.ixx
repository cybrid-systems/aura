// sandbox.ixx — Issue #1180 Phase 1: dedicated sandbox module scaffold.
// Enforcement remains wired via security_capabilities + workspace primitives;
// this module is the architectural home for fiber isolation / try-mutation guards.

module;

export module aura.core.sandbox;

import std;

export namespace aura::core::sandbox {

inline constexpr int kSandboxPhase = 1;

enum class SandboxMode : std::uint8_t {
    Off = 0,
    Restricted = 1,
    Strict = 2,
};

struct SandboxState {
    SandboxMode mode = SandboxMode::Restricted;
    std::uint64_t trial_mutation_guards = 0;
    std::uint64_t isolation_checks = 0;
};

inline SandboxState g_sandbox_state{};

[[nodiscard]] inline bool is_sandbox_active() noexcept {
    return g_sandbox_state.mode != SandboxMode::Off;
}

} // namespace aura::core::sandbox
