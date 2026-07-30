// densify_consistency_report.h — Issue #2341: unified post-densify consistency probe.
//
// Aggregates pin / linear / RootRemap / closure-remount / EnvFrame axes into a
// single decision-oriented report (mirrors #2300 lifetime-contract-snapshot
// pure-aggregate pattern). Soft / empty remap / no Moving → trivially ok.
//
// Force-reason priority (most severe first):
//   pin > linear > root_remap > closure > envframe > none
// Codes (also returned as string for observability): pin / linear /
// root_remap / closure / envframe / none.
//
// Used by Phase 5 mutation boundary driver (evaluator_mutation_boundary.cpp)
// to gate outermost success publishes — mirrors pin_contract_held gating
// (#2266). When overall_ok is false, the driver suppresses
// outermost_exit_phase5_unlock + outermost_exit_order_complete bumps and
// increments densify_consistency_fail_total instead. Optional
// AURA_DENSIFY_CONTRACT=hard env aborts on fail (aligns RootRemap hard
// contract pattern at root_remap_pass.ixx).

#ifndef AURA_CORE_DENSIFY_CONSISTENCY_REPORT_H
#define AURA_CORE_DENSIFY_CONSISTENCY_REPORT_H

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace aura::core::densify_consistency {

// DensifyConsistencyReport — single per-call-site snapshot of every
// densify-time consistency axis. Each axis is independently queryable
// (so Agents can drill into the failing one) and `overall_ok` ANDs them
// for the hard gate.
//
// Soft / empty / no Moving trivially ok (all fields default true).
struct DensifyConsistencyReport {
    bool pin_ok = true;
    bool linear_ok = true;
    bool root_remap_ok = true;
    bool closure_remount_ok = true;
    bool envframe_ok = true;

    [[nodiscard]] bool overall_ok() const noexcept {
        return pin_ok && linear_ok && root_remap_ok && closure_remount_ok && envframe_ok;
    }

    // force_reason priority: pin > linear > root_remap > closure > envframe > none.
    // Returns the *most-severe* failing axis (or "none" when all ok).
    [[nodiscard]] const char* force_reason() const noexcept {
        if (!pin_ok)
            return "pin";
        if (!linear_ok)
            return "linear";
        if (!root_remap_ok)
            return "root_remap";
        if (!closure_remount_ok)
            return "closure";
        if (!envframe_ok)
            return "envframe";
        return "none";
    }
};

// File-level atomic counter — incremented when a Phase 5 driver
// computes a DensifyConsistencyReport with !overall_ok(). Exposed via
// the query surface (query:lifetime-contract-snapshot additive keys).
inline std::atomic<std::uint64_t> g_densify_consistency_fail_total{0};

[[nodiscard]] inline std::uint64_t densify_consistency_fail_total() noexcept {
    return g_densify_consistency_fail_total.load(std::memory_order_relaxed);
}

inline void bump_densify_consistency_fail_total() noexcept {
    g_densify_consistency_fail_total.fetch_add(1, std::memory_order_relaxed);
}

// env-empty branch mirrors #2266 AURA_MOVING_PIN_CONTRACT=hard pattern.
// Returns true when production security defaults demand hard abort on
// !overall_ok (aligns RootRemap hard_contract_enabled at #2294).
[[nodiscard]] inline bool densify_consistency_hard_contract_enabled() noexcept {
    const char* env = std::getenv("AURA_DENSIFY_CONTRACT");
    return env != nullptr && *env != '\0' && std::string_view(env) == "hard";
}

// Code-name helper — shared by Phase 5 driver + query surface so the
// force_reason label is consistent across audit logs + dashboards.
[[nodiscard]] inline std::string_view force_reason_to_string(const char* r) noexcept {
    if (!r)
        return "none";
    std::string_view v(r);
    if (v == "pin")
        return "pin";
    if (v == "linear")
        return "linear";
    if (v == "root_remap")
        return "root_remap";
    if (v == "closure")
        return "closure";
    if (v == "envframe")
        return "envframe";
    return "none";
}

} // namespace aura::core::densify_consistency

#endif // AURA_CORE_DENSIFY_CONSISTENCY_REPORT_H