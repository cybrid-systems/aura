// densify_consistency_report.h — Issue #2341: unified post-densify consistency probe.
//
// Aggregates pin / linear / RootRemap / closure-remount / EnvFrame axes into a
// single decision-oriented report (mirrors #2300 lifetime-contract-snapshot
// pure-aggregate pattern). Soft / empty remap / no Moving → trivially ok.
//
// Force-reason priority (most severe first):
//   pin > linear > type > root_remap > closure > envframe > none
// Codes (also returned as string for observability): pin / linear / type /
// root_remap / closure / envframe / none. (#2353 adds type axis.)
//
// Used by Phase 5 mutation boundary driver (evaluator_mutation_boundary.cpp)
// to gate outermost success publishes — mirrors pin_contract_held gating
// (#2266). When overall_ok is false, the driver suppresses
// outermost_exit_phase5_unlock + outermost_exit_order_complete bumps and
// increments densify_consistency_fail_total instead. Optional
// AURA_DENSIFY_CONTRACT=hard env aborts on fail (aligns RootRemap hard
// contract pattern at root_remap_pass.ixx).
//
// ── Issue #2365 / #2368: densify-success closed-loop order (do not reorder) ──
// After Moving densify (live_compact / compact_all_moving_pinned):
//   1. RootRemapPass          (inside densify via RootRemapCallback)
//   2. pin verify             (pin_contract_held / verify_pins_under_moving)
//   3. EnvFrame live-ref xfer (#2360/#2362 scan_live_env_frame_refs_after_densify)
//   4. closure remount scan   (scan_live_closures_for_linear_captures only_if_moved)
//   5. dual-epoch restamp     (ensure_dual_path_consistent over live EnvFrames)
//   6. report axes            (root_remap_ok / closure_remount_ok / envframe_ok
//                              from last-call probes — Soft vacuous true)
// Soft / empty densify skips 3–5 (zero extra cost).
//
// Issue #2368: remap-context pairing is **never optional** on the production
// Moving success path. Call sites MUST use Evaluator::force_densify_remap_pairing()
// (or the Phase 5 driver that invokes it) so steps 1+3–5 cannot be reordered
// into a partial-remap window. Dual-epoch restamp is always last before report.

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
    // Issue #2353: type-axis after densify/steal (ownership + optional partial).
    // Default true (no densify / Soft / no linear → vacuous ok).
    bool type_ok = true;
    bool root_remap_ok = true;
    bool closure_remount_ok = true;
    bool envframe_ok = true;

    [[nodiscard]] bool overall_ok() const noexcept {
        return pin_ok && linear_ok && type_ok && root_remap_ok && closure_remount_ok && envframe_ok;
    }

    // force_reason priority: pin > linear > type > root_remap > closure > envframe > none.
    // Returns the *most-severe* failing axis (or "none" when all ok).
    [[nodiscard]] const char* force_reason() const noexcept {
        if (!pin_ok)
            return "pin";
        if (!linear_ok)
            return "linear";
        if (!type_ok)
            return "type";
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

// Issue #2361: last Phase 5 densify envframe_ok (1=ok, 0=fail). Query
// surface reads this instead of forcing true; Soft / no densify leaves 1.
inline std::atomic<std::uint8_t> g_last_densify_envframe_ok{1};
// Issue #2365: last-call root_remap_ok / closure_remount_ok (Soft → 1).
// Query surface uses these instead of cumulative process fails that
// would poison Soft densify after a prior Moving fail.
inline std::atomic<std::uint8_t> g_last_densify_root_remap_ok{1};
inline std::atomic<std::uint8_t> g_last_densify_closure_remount_ok{1};
// Issue #2368: dual-epoch restamp last-call + pairing-forced flag.
// Soft densify leaves dual_epoch_ok=1 and pairing_forced=0 (never ran).
// Moving success path always sets pairing_forced=1 after force_densify_remap_pairing.
inline std::atomic<std::uint8_t> g_last_densify_dual_epoch_ok{1};
inline std::atomic<std::uint8_t> g_last_densify_remap_pairing_forced{0};

// Result of Evaluator::force_densify_remap_pairing() — permanent step order
// encoded in the helper body (Issue #2368 pairing guarantee).
struct DensifyRemapPairingResult {
    bool root_remap_ok = true;
    bool envframe_ok = true;
    bool closure_remount_ok = true;
    bool dual_epoch_ok = true;
    bool forced = false; // true iff helper ran (Moving success path)
};

[[nodiscard]] inline std::uint64_t densify_consistency_fail_total() noexcept {
    return g_densify_consistency_fail_total.load(std::memory_order_relaxed);
}

inline void bump_densify_consistency_fail_total() noexcept {
    g_densify_consistency_fail_total.fetch_add(1, std::memory_order_relaxed);
}

inline void note_last_densify_envframe_ok(bool ok) noexcept {
    g_last_densify_envframe_ok.store(ok ? 1 : 0, std::memory_order_relaxed);
}

[[nodiscard]] inline bool last_densify_envframe_ok() noexcept {
    return g_last_densify_envframe_ok.load(std::memory_order_relaxed) != 0;
}

inline void note_last_densify_root_remap_ok(bool ok) noexcept {
    g_last_densify_root_remap_ok.store(ok ? 1 : 0, std::memory_order_relaxed);
}
[[nodiscard]] inline bool last_densify_root_remap_ok() noexcept {
    return g_last_densify_root_remap_ok.load(std::memory_order_relaxed) != 0;
}
inline void note_last_densify_closure_remount_ok(bool ok) noexcept {
    g_last_densify_closure_remount_ok.store(ok ? 1 : 0, std::memory_order_relaxed);
}
[[nodiscard]] inline bool last_densify_closure_remount_ok() noexcept {
    return g_last_densify_closure_remount_ok.load(std::memory_order_relaxed) != 0;
}
inline void note_last_densify_dual_epoch_ok(bool ok) noexcept {
    g_last_densify_dual_epoch_ok.store(ok ? 1 : 0, std::memory_order_relaxed);
}
[[nodiscard]] inline bool last_densify_dual_epoch_ok() noexcept {
    return g_last_densify_dual_epoch_ok.load(std::memory_order_relaxed) != 0;
}
inline void note_last_densify_remap_pairing_forced(bool forced) noexcept {
    g_last_densify_remap_pairing_forced.store(forced ? 1 : 0, std::memory_order_relaxed);
}
[[nodiscard]] inline bool last_densify_remap_pairing_forced() noexcept {
    return g_last_densify_remap_pairing_forced.load(std::memory_order_relaxed) != 0;
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
    if (v == "type")
        return "type";
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