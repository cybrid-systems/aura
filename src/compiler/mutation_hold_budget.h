// mutation_hold_budget.h — Issue #2313: hold-budget threshold accessor.
// Shared header so MutationBoundaryGuard dtor + query:mutation-boundary-hold-stats
// share one env-cached source of truth (AURA_MUTATION_HOLD_BUDGET_US).

#ifndef AURA_COMPILER_MUTATION_HOLD_BUDGET_H
#define AURA_COMPILER_MUTATION_HOLD_BUDGET_H

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace aura::compiler {

// Default 100_000 µs (100 ms). Lazy-init from AURA_MUTATION_HOLD_BUDGET_US.
// C-style digit parse (no exceptions). Cached once per process.
[[nodiscard]] inline std::uint64_t mutation_hold_budget_us() noexcept {
    static const std::uint64_t cached = []() noexcept -> std::uint64_t {
        const char* e = std::getenv("AURA_MUTATION_HOLD_BUDGET_US");
        if (e == nullptr || e[0] == '\0')
            return 100000ULL;
        std::uint64_t v = 0;
        for (const char* p = e; *p >= '0' && *p <= '9'; ++p) {
            v = v * 10 + static_cast<std::uint64_t>(*p - '0');
        }
        return v > 0 ? v : 100000ULL;
    }();
    return cached;
}

// Issue #2349: production hold SLO circuit-breaker (default force-fail path).
// Distinct from #2313 signal-only budget and #2199 opt-in STRICT hard-timeout.
//
// ── Decision table (Soft / Production / Disabled) ──
// | Mode       | Env select                                   | hold_us > SLO action              |
// | Soft       | AURA_SANDBOX=off OR AURA_MUTATION_HOLD_SLO_SOFT=1 | metric only (no force-fail) |
// | Production | default (sandbox not off)                    | success_flag=false + counter      |
// | Disabled   | AURA_MUTATION_HOLD_SLO_US=0                  | no check (AC4)                    |
// Happy path (hold ≤ SLO or disabled): one compare / getenv parse, zero force
// work beyond existing long-hold metrics (AC3).
//
// Default SLO 100_000 µs (100 ms). Live getenv (not process-static) so tests
// can set AURA_MUTATION_HOLD_SLO_US without process restart (same pattern as
// #2346 Soft/Hard).
[[nodiscard]] inline std::uint64_t mutation_hold_slo_us() noexcept {
    const char* e = std::getenv("AURA_MUTATION_HOLD_SLO_US");
    if (e == nullptr || e[0] == '\0')
        return 100000ULL; // production default 100ms
    // Explicit 0 disables the circuit (AC4).
    if (e[0] == '0' && e[1] == '\0')
        return 0;
    std::uint64_t v = 0;
    for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
        v = v * 10 + static_cast<std::uint64_t>(*p - '0');
    return v; // 0 if non-numeric → disable
}

// Soft / sandbox: metric-only SLO violation (AC2). Production (default): force.
[[nodiscard]] inline bool mutation_hold_slo_soft_mode() noexcept {
    const char* soft = std::getenv("AURA_MUTATION_HOLD_SLO_SOFT");
    if (soft && soft[0] == '1')
        return true;
    const char* sandbox = std::getenv("AURA_SANDBOX");
    return sandbox && sandbox[0] != '\0' && std::string_view(sandbox) == "off";
}

} // namespace aura::compiler

#endif // AURA_COMPILER_MUTATION_HOLD_BUDGET_H
