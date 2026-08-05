// sandbox.ixx — Issues #1180/#1565/#2657: sandbox modes linked to capability
// effects + single authority for SandboxMode.
//
// Mirrors sandbox.hh for module-aware TUs. Reads from the process-wide
// atomic (g_sandbox_mode_atomic); set_mode is the sole writer across all
// stores (atomic + plain enum + registry + workspace_isolation + provenance
// tracker). AURA_SANDBOX_ASSERT=1 enables post-condition triple equality.

module;

#include "core/sandbox.hh"

export module aura.core.sandbox;

import std;

export namespace aura::core::sandbox {

inline constexpr int kSandboxPhase = 2; // #1565 effect linkage
inline constexpr int kSandboxIssue = 1565;
inline constexpr int kSandboxAuthorityIssue = 2657;

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

inline std::atomic<std::uint8_t>& g_sandbox_mode_atomic() noexcept {
    static std::atomic<std::uint8_t> v{static_cast<std::uint8_t>(SandboxMode::Off)};
    return v;
}

inline SandboxState g_sandbox_state{};

[[nodiscard]] inline bool is_sandbox_active() noexcept {
    return g_sandbox_mode_atomic().load(std::memory_order_acquire) !=
           static_cast<std::uint8_t>(SandboxMode::Off);
}

[[nodiscard]] inline bool is_strict() noexcept {
    return g_sandbox_mode_atomic().load(std::memory_order_acquire) ==
           static_cast<std::uint8_t>(SandboxMode::Strict);
}

inline std::atomic<std::uint64_t>& g_sandbox_mode_authority_set_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}

inline void set_mode(SandboxMode m) noexcept {
    std::uint8_t u = static_cast<std::uint8_t>(m);
    if (u > static_cast<std::uint8_t>(SandboxMode::Strict))
        u = static_cast<std::uint8_t>(SandboxMode::Strict);
    g_sandbox_mode_atomic().store(u, std::memory_order_release);
    g_sandbox_state.mode = static_cast<SandboxMode>(u);
    using namespace ::aura::core::capability;
    g_capability_registry().sandbox_mode = static_cast<EffectSandboxMode>(u);
    using namespace ::aura::core::workspace_isolation;
    g_workspace_isolation().set_strict_sandbox_linked(
        u == static_cast<std::uint8_t>(SandboxMode::Strict));
    {
        auto& prov = ::aura::core::provenance::g_provenance_tracker();
        if (u == static_cast<std::uint8_t>(SandboxMode::Strict)) {
            prov.set_policy(::aura::core::provenance::AutoRefreshPolicy::FailOnStale);
        } else if (prov.get_policy() == ::aura::core::provenance::AutoRefreshPolicy::FailOnStale) {
            prov.set_policy(::aura::core::provenance::AutoRefreshPolicy::AutoRefreshOnBoundary);
        }
    }
    g_sandbox_mode_authority_set_total().fetch_add(1, std::memory_order_relaxed);
    if (const char* e = std::getenv("AURA_SANDBOX_ASSERT"); e != nullptr && e[0] == '1') {
        const auto a = g_sandbox_mode_atomic().load(std::memory_order_acquire);
        const auto b = static_cast<std::uint8_t>(g_capability_registry().sandbox_mode.load());
        const auto c = (g_sandbox_state.mode == SandboxMode::Strict)
                           ? static_cast<std::uint8_t>(SandboxMode::Strict)
                       : (g_sandbox_state.mode == SandboxMode::Restricted)
                           ? static_cast<std::uint8_t>(SandboxMode::Restricted)
                           : static_cast<std::uint8_t>(SandboxMode::Off);
        if (a != b || a != c) {
            std::fprintf(stderr,
                         "[aura#2657] SANDBOX_ASSERT drift: atomic=%u registry=%u plain=%u\n", a, b,
                         c);
            std::abort();
        }
    }
}

struct SandboxAuthorityStats {
    std::uint64_t authority_set_total = 0;
    std::uint8_t current_mode = 0;
    std::uint8_t registry_mode = 0;
    std::uint8_t plain_mode = 0;
};

inline SandboxAuthorityStats snapshot_sandbox_authority_stats() noexcept {
    SandboxAuthorityStats s;
    s.authority_set_total = g_sandbox_mode_authority_set_total().load(std::memory_order_acquire);
    s.current_mode = g_sandbox_mode_atomic().load(std::memory_order_acquire);
    s.registry_mode = static_cast<std::uint8_t>(
        ::aura::core::capability::g_capability_registry().sandbox_mode.load());
    s.plain_mode = static_cast<std::uint8_t>(g_sandbox_state.mode);
    return s;
}

} // namespace aura::core::sandbox
