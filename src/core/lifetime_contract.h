// lifetime_contract.h — Issue #2300: pure Agent-visible lifetime contract snapshot.
//
// Aggregates pin / linear / EnvFrame / GC-defer / residual signals into one
// decision-oriented surface (mirrors #2281 TypedMutationAudit decide()).
//
// Pure: no counter bumps, no mutate side effects. Callers pass live reads;
// formula for `ok` is documented below so Agents can predict before mutate.
//
// Formula (lifetime-contract-ok):
//   ok = (moving_pin_contract_fail_total == 0)
//     && (linear_pin_miss_total == 0)
//     && (residual_defer_hard_fail_total == 0)
// Live pins, linear roots, envframe guard depth, and armed gc_defer_reasons
// are informational (they describe current hold state) and do NOT force ok=0.
//
// force_reason priority (most severe first):
//   pin-miss > linear-miss > residual > defer-orphan > none
// Codes: 0=none 1=pin-miss 2=linear-miss 3=residual 4=defer-orphan

#ifndef AURA_CORE_LIFETIME_CONTRACT_H
#define AURA_CORE_LIFETIME_CONTRACT_H

#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "core/gc_hooks.h"

namespace aura::core::lifetime_contract {

inline constexpr int kLifetimeContractIssue = 2300;

// residual-defer-policy: 0=soft, 1=clear (production default), 2=hard
// Matches query:mutation-boundary-hold-stats residual-defer-policy (#2269).
[[nodiscard]] inline int residual_defer_policy_from_env() noexcept {
    const char* sandbox_e = std::getenv("AURA_SANDBOX");
    const bool dev_off = sandbox_e && *sandbox_e && std::string_view(sandbox_e) == "off";
    if (dev_off)
        return 0; // Soft
    const char* policy_e = std::getenv("AURA_RESIDUAL_DEFER_POLICY");
    const bool policy_hard_env = policy_e && *policy_e && std::string_view(policy_e) == "hard";
    const char* legacy_e = std::getenv("AURA_HARD_RESIDUAL_DEFER");
    const bool legacy_hard = legacy_e && *legacy_e && legacy_e[0] != '0';
    if (policy_hard_env || legacy_hard)
        return 2; // Hard
    return 1;     // Clear (production default)
}

// moving-compact-enabled preference: 1 when env/pref on, 0 off, -1 unset→default off
// for Agent dashboards (arena.ixx default remains env-driven at densify site).
[[nodiscard]] inline int moving_compact_enabled_from_env() noexcept {
    const char* e = std::getenv("AURA_ARENA_MOVING_COMPACT");
    if (!e || !*e)
        return 0;
    if (e[0] == '0' && e[1] == '\0')
        return 0;
    return 1;
}

struct LifetimeContractSnapshot {
    bool ok = true;
    std::uint32_t force_reason_code = 0; // 0..4
    const char* force_reason = "none";
    std::uint64_t lifetime_pin_live_count = 0;
    std::uint64_t linear_pin_live_count = 0;
    std::uint64_t envframe_active_guard_depth = 0;
    std::uint32_t gc_defer_reasons_mask = 0;
    int residual_defer_policy = 1;
    int moving_compact_enabled = 0;
    std::uint64_t moving_pin_contract_fail_total = 0;
    std::uint64_t linear_pin_miss_total = 0;
    std::uint64_t residual_defer_hard_fail_total = 0;
    std::uint64_t gc_defer_orphan_cleared_on_steal_total = 0;
};

// Pure aggregation. Inputs are process-level counters / live depths;
// this function only classifies — never mutates atomics or env.
[[nodiscard]] inline LifetimeContractSnapshot
make_lifetime_contract_snapshot(std::uint64_t lifetime_pin_live, std::uint64_t linear_pin_live,
                                std::uint64_t envframe_guard_depth, std::uint32_t gc_defer_reasons,
                                std::uint64_t pin_contract_fail, std::uint64_t linear_pin_miss,
                                std::uint64_t residual_hard_fail,
                                std::uint64_t orphan_cleared_on_steal, int residual_policy,
                                int moving_enabled) noexcept {
    LifetimeContractSnapshot s;
    s.lifetime_pin_live_count = lifetime_pin_live;
    s.linear_pin_live_count = linear_pin_live;
    s.envframe_active_guard_depth = envframe_guard_depth;
    s.gc_defer_reasons_mask = gc_defer_reasons;
    s.residual_defer_policy = residual_policy;
    s.moving_compact_enabled = moving_enabled;
    s.moving_pin_contract_fail_total = pin_contract_fail;
    s.linear_pin_miss_total = linear_pin_miss;
    s.residual_defer_hard_fail_total = residual_hard_fail;
    s.gc_defer_orphan_cleared_on_steal_total = orphan_cleared_on_steal;

    // Hard-miss signals only (informational live holds do not fail ok).
    const bool pin_miss = pin_contract_fail > 0;
    const bool linear_miss = linear_pin_miss > 0;
    const bool residual_miss = residual_hard_fail > 0;
    // defer-orphan is sticky historical signal: only forces when residual is
    // already clean but steal-orphan path has fired (Agent attention).
    const bool orphan_signal =
        orphan_cleared_on_steal > 0 && !pin_miss && !linear_miss && !residual_miss;

    s.ok = !pin_miss && !linear_miss && !residual_miss;

    if (pin_miss) {
        s.force_reason = "pin-miss";
        s.force_reason_code = 1;
    } else if (linear_miss) {
        s.force_reason = "linear-miss";
        s.force_reason_code = 2;
    } else if (residual_miss) {
        s.force_reason = "residual";
        s.force_reason_code = 3;
    } else if (orphan_signal) {
        // Informational force-reason only — ok stays 1 (not a hard miss).
        s.force_reason = "defer-orphan";
        s.force_reason_code = 4;
    } else {
        s.force_reason = "none";
        s.force_reason_code = 0;
    }
    return s;
}

// Live process snapshot: reads atomics only (no bumps). Callers that have
// module access to lifetime_pin / envframe should prefer the overloads that
// pass those live counts; this entry point uses gc_hooks + zeros for the
// parts that are header-reachable and zeros module-only live counts.
// Production query path fills live counts from modules.
[[nodiscard]] inline LifetimeContractSnapshot snapshot_lifetime_contract_gc_only() noexcept {
    return make_lifetime_contract_snapshot(
        /*lifetime_pin_live=*/0, /*linear_pin_live=*/0, /*envframe_guard_depth=*/0,
        aura::gc_hooks::defer_reasons_snapshot(),
        /*pin_contract_fail=*/0, /*linear_pin_miss=*/0, /*residual_hard_fail=*/0,
        aura::gc_hooks::gc_defer_orphan_cleared_on_steal_total(), residual_defer_policy_from_env(),
        moving_compact_enabled_from_env());
}

} // namespace aura::core::lifetime_contract

#endif // AURA_CORE_LIFETIME_CONTRACT_H
