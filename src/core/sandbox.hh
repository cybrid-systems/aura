// sandbox.hh — Issues #1180/#1565/#2657: SSOT for SandboxMode.
//
// Module consumers: `import aura.core.sandbox;` re-exports this header
// (no second set_mode body). Non-module TUs: #include this file.
//
// Issue #2657: single authority for SandboxMode. The legacy `g_sandbox_
// state().mode` plain enum is now mirror-only — the canonical source of
// truth is `g_sandbox_mode_atomic()` (process-wide atomic uint8). All
// readers (is_strict / is_sandbox_active) acquire-load the atomic; all
// writers MUST go through `set_mode`, which atomically broadcasts to:
//   1. g_sandbox_mode_atomic() (release)
//   2. g_sandbox_state().mode (plain, for backward-compat reads)
//   3. g_capability_registry().sandbox_mode (atomic, release)
//   4. g_workspace_isolation().set_strict_sandbox_linked(m == 2)
//   5. g_provenance_tracker().set_policy(FailOnStale / AutoRefreshOnBoundary)
//   6. g_sandbox_mode_authority_set_total counter (process-wide)
//   7. AURA_SANDBOX_ASSERT=1 → post-condition triple equality check
// CapabilityRegistry::sandbox_mode is private (friend == set_mode), so
// direct writes from TUs / tests fail to compile. The coverage linter
// `scripts/check_sandbox_mode_authority.py` is the second gate.

#ifndef AURA_CORE_SANDBOX_HH
#define AURA_CORE_SANDBOX_HH

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "core/capability_model.hh"
#include "core/provenance_tracker.hh"
#include "core/workspace_isolation.hh"

namespace aura::core::sandbox {

inline constexpr int kSandboxPhase = 2;
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

// Process-wide atomic source of truth for sandbox mode (issue #2657).
// Acquires readers see the most recent release-store, never a torn value.
inline std::atomic<std::uint8_t>& g_sandbox_mode_atomic() noexcept {
    static std::atomic<std::uint8_t> v{static_cast<std::uint8_t>(SandboxMode::Off)};
    return v;
}

// Legacy plain-enum mirror (backward-compat for reads that still consult
// the field directly). Writers MUST go through set_mode (the authority).
inline SandboxState& g_sandbox_state() noexcept {
    static SandboxState s;
    return s;
}

[[nodiscard]] inline bool is_sandbox_active() noexcept {
    return g_sandbox_mode_atomic().load(std::memory_order_acquire) !=
           static_cast<std::uint8_t>(SandboxMode::Off);
}
[[nodiscard]] inline bool is_strict() noexcept {
    return g_sandbox_mode_atomic().load(std::memory_order_acquire) ==
           static_cast<std::uint8_t>(SandboxMode::Strict);
}

// Process-wide authority — the SOLE writer of sandbox mode across all
// stores. Updates atomic + plain enum + registry + workspace_isolation
// strict link + provenance_tracker policy. Increments the
// authority-set total counter; under AURA_SANDBOX_ASSERT=1 also runs a
// post-condition triple-equality check (atomic == registry == plain).
[[nodiscard]] inline std::atomic<std::uint64_t>& g_sandbox_mode_authority_set_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}

inline void set_mode(SandboxMode m) noexcept {
    std::uint8_t u = static_cast<std::uint8_t>(m);
    if (u > static_cast<std::uint8_t>(SandboxMode::Strict))
        u = static_cast<std::uint8_t>(SandboxMode::Strict);
    // 1) Atomic source of truth (release pairs with acquire reads).
    g_sandbox_mode_atomic().store(u, std::memory_order_release);
    // 2) Plain enum mirror (backward-compat with code that reads
    //    g_sandbox_state().mode directly).
    g_sandbox_state().mode = static_cast<SandboxMode>(u);
    // 3) CapabilityRegistry atomic (single source of truth for
    //    check_and_record_effect / check_macro_self_evo enforcement).
    using namespace ::aura::core::capability;
    g_capability_registry().sandbox_mode = static_cast<EffectSandboxMode>(u);
    // 4) Workspace isolation strict link (Strict linking drives
    //    boundary check, privilege-sticky bypass).
    using namespace ::aura::core::workspace_isolation;
    g_workspace_isolation().set_strict_sandbox_linked(
        u == static_cast<std::uint8_t>(SandboxMode::Strict));
    // 5) Provenance policy — Strict = FailOnStale (no silent restamp);
    //    leaving Strict = restore AutoRefreshOnBoundary if the previous
    //    policy was FailOnStale (preserves manual policy overrides).
    {
        auto& prov = ::aura::core::provenance::g_provenance_tracker();
        if (u == static_cast<std::uint8_t>(SandboxMode::Strict)) {
            prov.set_policy(::aura::core::provenance::AutoRefreshPolicy::FailOnStale);
        } else if (prov.get_policy() == ::aura::core::provenance::AutoRefreshPolicy::FailOnStale) {
            prov.set_policy(::aura::core::provenance::AutoRefreshPolicy::AutoRefreshOnBoundary);
        }
    }
    // 6) Authority-set counter (one bump per authoritative write).
    g_sandbox_mode_authority_set_total().fetch_add(1, std::memory_order_relaxed);
    // 7) Optional post-condition triple equality (AURA_SANDBOX_ASSERT=1).
    if (const char* e = std::getenv("AURA_SANDBOX_ASSERT"); e != nullptr && e[0] == '1') {
        const auto a = g_sandbox_mode_atomic().load(std::memory_order_acquire);
        const auto b = static_cast<std::uint8_t>(g_capability_registry().sandbox_mode.load());
        const auto c = (g_sandbox_state().mode == SandboxMode::Strict)
                           ? static_cast<std::uint8_t>(SandboxMode::Strict)
                       : (g_sandbox_state().mode == SandboxMode::Restricted)
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

// Snapshot the authority state for diagnostics / Agent query surface.
struct SandboxAuthorityStats {
    std::uint64_t authority_set_total = 0;
    std::uint8_t current_mode = 0;  // acquire load of atomic
    std::uint8_t registry_mode = 0; // acquire load of registry atomic
    std::uint8_t plain_mode = 0;    // plain enum mirror
};

inline SandboxAuthorityStats snapshot_sandbox_authority_stats() noexcept {
    SandboxAuthorityStats s;
    s.authority_set_total = g_sandbox_mode_authority_set_total().load(std::memory_order_acquire);
    s.current_mode = g_sandbox_mode_atomic().load(std::memory_order_acquire);
    s.registry_mode = static_cast<std::uint8_t>(
        ::aura::core::capability::g_capability_registry().sandbox_mode.load());
    s.plain_mode = static_cast<std::uint8_t>(g_sandbox_state().mode);
    return s;
}

} // namespace aura::core::sandbox

#endif // AURA_CORE_SANDBOX_HH
