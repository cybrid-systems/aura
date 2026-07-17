// sandbox.hh — Issues #1180/#1565: header form for evaluator TUs.

#ifndef AURA_CORE_SANDBOX_HH
#define AURA_CORE_SANDBOX_HH

#include <cstdint>

namespace aura::core::sandbox {

inline constexpr int kSandboxPhase = 2;
inline constexpr int kSandboxIssue = 1565;

enum class SandboxMode : std::uint8_t {
    Off = 0,
    Restricted = 1,
    Strict = 2,
};

struct SandboxState {
    SandboxMode mode = SandboxMode::Off;
    std::uint64_t trial_mutation_guards = 0;
    std::uint64_t isolation_checks = 0;
    std::uint64_t effect_checks = 0;
    std::uint64_t effect_denials = 0;
};

inline SandboxState& g_sandbox_state() noexcept {
    static SandboxState s;
    return s;
}

[[nodiscard]] inline bool is_sandbox_active() noexcept {
    return g_sandbox_state().mode != SandboxMode::Off;
}
[[nodiscard]] inline bool is_strict() noexcept {
    return g_sandbox_state().mode == SandboxMode::Strict;
}
inline void set_mode(SandboxMode m) noexcept {
    g_sandbox_state().mode = m;
}

} // namespace aura::core::sandbox

#endif // AURA_CORE_SANDBOX_HH
