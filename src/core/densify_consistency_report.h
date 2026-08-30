// densify_consistency_report.h — Issue #2341: unified post-densify consistency probe.
//
// Aggregates pin / linear / RootRemap / closure-remount / EnvFrame axes into a
// single decision-oriented report (mirrors #2300 lifetime-contract-snapshot
// pure-aggregate pattern). Soft / empty remap / no Moving → trivially ok.
//
// Force-reason priority (most severe first):
//   pin > untracked > panic_residual > linear > type > root_remap > closure > envframe > none
// Codes (also returned as string for observability): pin / untracked /
// panic_residual / linear / type / root_remap / closure / envframe / none.
// (#2353 adds type axis; #2595 adds untracked + panic_residual axes.)
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
#include <cstddef>
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
    // Issue #2595: untracked axis — Moving densify must leave no
    // incomplete-remap external roots (live_compact untracked_kept_count
    // path; any_moving_incomplete_remap from AdaptiveCompactResult aggregate
    // OR #2495 process-wide g_moving_untracked_external_roots_total delta
    // during the densify window). Default true (no Moving / Soft).
    bool untracked_ok = true;
    // Issue #2595: panic_residual axis — if any PanicCheckpoint is live
    // AND not deferred (gc_deferred_for_evaluator), Phase 5 must NOT
    // claim success (panic in progress can leak half-green densify).
    // Default true (no panic_cp OR gc_deferred_for_evaluator is true).
    bool panic_residual_ok = true;
    bool linear_ok = true;
    // Issue #2353: type-axis after densify/steal (ownership + optional partial).
    // Default true (no densify / Soft / no linear → vacuous ok).
    bool type_ok = true;
    bool root_remap_ok = true;
    bool closure_remount_ok = true;
    bool envframe_ok = true;

    [[nodiscard]] bool overall_ok() const noexcept {
        return pin_ok && untracked_ok && panic_residual_ok && linear_ok && type_ok &&
               root_remap_ok && closure_remount_ok && envframe_ok;
    }

    // force_reason priority:
    //   pin > untracked > panic_residual > linear > type > root_remap > closure > envframe > none.
    // Returns the *most-severe* failing axis (or "none" when all ok).
    [[nodiscard]] const char* force_reason() const noexcept {
        if (!pin_ok)
            return "pin";
        if (!untracked_ok)
            return "untracked";
        if (!panic_residual_ok)
            return "panic_residual";
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

// Issue #3314: DensifyConsistencyReport is append-only at struct END
// (#2906 / #3292 lineage). Agents + Phase-5 read axes by field; a
// mid-struct insert shifts BMI offsets. Compile-time only, zero runtime.
static_assert(offsetof(DensifyConsistencyReport, pin_ok) == 0,
              "Issue #3314: DensifyConsistencyReport.pin_ok must stay at offset 0 "
              "(#2906/#3292)");
static_assert(offsetof(DensifyConsistencyReport, envframe_ok) == 7,
              "Issue #3314: DensifyConsistencyReport.envframe_ok must stay at offset 7 "
              "(last axis, append-only at struct END, #2906/#3292)");
static_assert(sizeof(DensifyConsistencyReport) == 8,
              "Issue #3314: DensifyConsistencyReport size must stay 8 "
              "(append-only at struct END, #2906/#3292)");

// File-level atomic counter — incremented when a Phase 5 driver
// computes a DensifyConsistencyReport with !overall_ok(). Exposed via
// the query surface (query:lifetime-contract-snapshot additive keys).
inline std::atomic<std::uint64_t> g_densify_consistency_fail_total{0};

// Issue #2595: unified gate fail counter — additive schema key. Bumped
// when Phase 5 / densify success path computes a report with
// !overall_ok() under production_defaults (or via the gate's own
// evaluate path). Mirrors g_densify_consistency_fail_total (which
// covers all paths); this one is the unified-gate fail-closed contract
// for AI self-mod under Moving default ON.
inline std::atomic<std::uint64_t> g_densify_unified_gate_fail_total{0};

// Issue #2361 / #2376: last Phase 5 densify envframe_ok (1=ok, 0=fail).
// Query surface reads this instead of forcing true; Soft / no densify leaves 1.
// #2376 seals last-call semantics (not cumulative / not trivial true) as the
// production densify-consistency contract for envframe + closure axes.
inline std::atomic<std::uint8_t> g_last_densify_envframe_ok{1};
// Issue #2365 / #2376: last-call root_remap_ok / closure_remount_ok (Soft → 1).
// Query surface uses these instead of cumulative process fails that
// would poison Soft densify after a prior Moving fail.
inline std::atomic<std::uint8_t> g_last_densify_root_remap_ok{1};
inline std::atomic<std::uint8_t> g_last_densify_closure_remount_ok{1};
// Issue #2368: dual-epoch restamp last-call + pairing-forced flag.
// Soft densify leaves dual_epoch_ok=1 and pairing_forced=0 (never ran).
// Moving success path always sets pairing_forced=1 after force_densify_remap_pairing.
inline std::atomic<std::uint8_t> g_last_densify_dual_epoch_ok{1};
inline std::atomic<std::uint8_t> g_last_densify_remap_pairing_forced{0};

// Issue #2376: densify last-call sequence + fail-reason codes (Agent debug).
// call_seq bumps once per Phase 5 densify report publish (Soft or Moving)
// so dashboards can detect stale last-axis samples.
// Fail codes (0 = none / ok):
//   envframe: 1=ownership_scan, 2=dual_epoch, 3=linear_type (composed at Phase 5)
//   closure:  1=capture_remap_delta, 2=dual_epoch
inline std::atomic<std::uint64_t> g_last_densify_call_seq{0};
// Issue #2749: auto-registered intermediate pins that remapped successfully
// vs still-untracked incomplete roots (split incomplete-remap surface).
inline std::atomic<std::uint64_t> g_moving_auto_registered_remapped_total{0};
inline std::atomic<std::uint64_t> g_moving_still_untracked_incomplete_total{0};
inline constexpr int kMovingIncompleteRemapResidualIssue = 2749;
[[nodiscard]] inline std::uint64_t moving_auto_registered_remapped_total_v_read() noexcept {
    return g_moving_auto_registered_remapped_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t moving_still_untracked_incomplete_total_v_read() noexcept {
    return g_moving_still_untracked_incomplete_total.load(std::memory_order_relaxed);
}
// Issue #2889: known intermediate + compiler external root slots that the
// densify entry walk auto-registered into the Moving densify window (via
// ArenaGroup::register_external_root_slot_for_densify_all). Additive only —
// does not gate; pairs with #2749 split counters so Agents can verify the
// incomplete-remap surface shrank because known roots entered the window.
inline std::atomic<std::uint64_t> g_moving_known_roots_auto_registered_total{0};
inline constexpr int kMovingKnownRootsAutoRegisterIssue = 2889;
[[nodiscard]] inline std::uint64_t moving_known_roots_auto_registered_total_v_read() noexcept {
    return g_moving_known_roots_auto_registered_total.load(std::memory_order_relaxed);
}
inline void reset_moving_known_roots_auto_registered_for_test() noexcept {
    g_moving_known_roots_auto_registered_total.store(0, std::memory_order_relaxed);
}
// Issue #2935: Agent recovery after sticky densify-off (re-register known
// roots + clear sticky + optional one-shot Moving densify retry). Additive
// only — does not gate fail-closed incomplete-remap / production hard arm.
// Soft never arms sticky (#2905 AC3), so Soft recovery leaves these at 0
// unless an Agent force-arms sticky under test.
inline std::atomic<std::uint64_t> g_moving_sticky_cleared_via_recovery_total{0};
inline std::atomic<std::uint64_t> g_moving_densify_retry_after_recovery_total{0};
inline constexpr int kMovingStickyDensifyRecoveryIssue = 2935;
[[nodiscard]] inline std::uint64_t moving_sticky_cleared_via_recovery_total_v_read() noexcept {
    return g_moving_sticky_cleared_via_recovery_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t moving_densify_retry_after_recovery_total_v_read() noexcept {
    return g_moving_densify_retry_after_recovery_total.load(std::memory_order_relaxed);
}
inline void reset_moving_sticky_densify_recovery_for_test() noexcept {
    g_moving_sticky_cleared_via_recovery_total.store(0, std::memory_order_relaxed);
    g_moving_densify_retry_after_recovery_total.store(0, std::memory_order_relaxed);
}

// Issue #2973: production hard pre-densify external-root completeness.
// reject_total = densify windows blocked BEFORE address movement.
// untracked_total = declared external roots that would move without a
// covering slot or LifetimePin. Soft / hard_pref<=0 never bumps these
// (post-move incomplete-remap counters stay the observe-only path).
inline std::atomic<std::uint64_t> g_moving_pre_densify_reject_total{0};
inline std::atomic<std::uint64_t> g_moving_pre_densify_untracked_total{0};
inline constexpr int kMovingPreDensifyCompletenessIssue = 2973;
[[nodiscard]] inline std::uint64_t moving_pre_densify_reject_total_v_read() noexcept {
    return g_moving_pre_densify_reject_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t moving_pre_densify_untracked_total_v_read() noexcept {
    return g_moving_pre_densify_untracked_total.load(std::memory_order_relaxed);
}
inline void reset_moving_pre_densify_completeness_for_test() noexcept {
    g_moving_pre_densify_reject_total.store(0, std::memory_order_relaxed);
    g_moving_pre_densify_untracked_total.store(0, std::memory_order_relaxed);
}
// Issue #3017: residual of #2495/#2837/#2973 — value-only prep is not
// safe cover. Bumped only on the production-hard pre-move walk (same
// branch as pre-densify-untracked; Soft / hard_pref<=0 is a single
// atomic load and never increments this). Additive observability so
// Agents can distinguish "declared but not slotted/pinned" from other
// untracked axes without a second pin registry.
inline std::atomic<std::uint64_t> g_moving_value_only_not_cover_total{0};
inline constexpr int kMovingIncompleteRemapResidual3017Issue = 3017;
[[nodiscard]] inline std::uint64_t moving_value_only_not_cover_total_v_read() noexcept {
    return g_moving_value_only_not_cover_total.load(std::memory_order_relaxed);
}
inline void reset_moving_incomplete_remap_residual_3017_for_test() noexcept {
    g_moving_value_only_not_cover_total.store(0, std::memory_order_relaxed);
}
// Issue #3055: post-Moving objects_moved consistency. Bumped when a
// known-path live ptr (EnvFrame/Closure/FFI/JIT canary) still holds a
// last_object_remap_ key after slot + pin + RootRemapPass. Soft /
// no-move never increments (scan not invoked).
inline std::atomic<std::uint64_t> g_moving_post_moving_stale_total{0};
inline constexpr int kMovingPostMovingStaleIssue = 3055;
[[nodiscard]] inline std::uint64_t moving_post_moving_stale_total_v_read() noexcept {
    return g_moving_post_moving_stale_total.load(std::memory_order_relaxed);
}
inline void reset_moving_post_moving_stale_for_test() noexcept {
    g_moving_post_moving_stale_total.store(0, std::memory_order_relaxed);
}
inline std::atomic<std::uint8_t> g_last_densify_envframe_fail_code{0};
inline std::atomic<std::uint8_t> g_last_densify_closure_fail_code{0};

// Fail-reason codes for last densify axes (#2376).
inline constexpr std::uint8_t kDensifyFailNone = 0;
inline constexpr std::uint8_t kDensifyEnvframeFailOwnershipScan = 1;
inline constexpr std::uint8_t kDensifyEnvframeFailDualEpoch = 2;
inline constexpr std::uint8_t kDensifyEnvframeFailLinearType = 3;
inline constexpr std::uint8_t kDensifyClosureFailCaptureRemap = 1;
inline constexpr std::uint8_t kDensifyClosureFailDualEpoch = 2;

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

[[nodiscard]] inline std::uint64_t densify_unified_gate_fail_total() noexcept {
    return g_densify_unified_gate_fail_total.load(std::memory_order_relaxed);
}

inline void bump_densify_unified_gate_fail_total() noexcept {
    g_densify_unified_gate_fail_total.fetch_add(1, std::memory_order_relaxed);
}

// Test helper: reset unified gate counter for hermetic tests.
inline void reset_densify_unified_gate_for_test() noexcept {
    g_densify_unified_gate_fail_total.store(0, std::memory_order_relaxed);
}

inline void note_last_densify_envframe_ok(bool ok,
                                          std::uint8_t fail_code = kDensifyFailNone) noexcept {
    g_last_densify_envframe_ok.store(ok ? 1 : 0, std::memory_order_relaxed);
    g_last_densify_envframe_fail_code.store(ok ? kDensifyFailNone : fail_code,
                                            std::memory_order_relaxed);
}

[[nodiscard]] inline bool last_densify_envframe_ok() noexcept {
    return g_last_densify_envframe_ok.load(std::memory_order_relaxed) != 0;
}
[[nodiscard]] inline std::uint8_t last_densify_envframe_fail_code() noexcept {
    return g_last_densify_envframe_fail_code.load(std::memory_order_relaxed);
}

inline void note_last_densify_root_remap_ok(bool ok) noexcept {
    g_last_densify_root_remap_ok.store(ok ? 1 : 0, std::memory_order_relaxed);
}
[[nodiscard]] inline bool last_densify_root_remap_ok() noexcept {
    return g_last_densify_root_remap_ok.load(std::memory_order_relaxed) != 0;
}
inline void
note_last_densify_closure_remount_ok(bool ok, std::uint8_t fail_code = kDensifyFailNone) noexcept {
    g_last_densify_closure_remount_ok.store(ok ? 1 : 0, std::memory_order_relaxed);
    g_last_densify_closure_fail_code.store(ok ? kDensifyFailNone : fail_code,
                                           std::memory_order_relaxed);
}
[[nodiscard]] inline bool last_densify_closure_remount_ok() noexcept {
    return g_last_densify_closure_remount_ok.load(std::memory_order_relaxed) != 0;
}
[[nodiscard]] inline std::uint8_t last_densify_closure_fail_code() noexcept {
    return g_last_densify_closure_fail_code.load(std::memory_order_relaxed);
}

// Issue #2376: bump densify last-call sequence (Phase 5 report publish).
inline void bump_last_densify_call_seq() noexcept {
    g_last_densify_call_seq.fetch_add(1, std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t last_densify_call_seq() noexcept {
    return g_last_densify_call_seq.load(std::memory_order_relaxed);
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
    if (v == "untracked")
        return "untracked";
    if (v == "panic_residual")
        return "panic_residual";
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

// Issue #3210: temporary EnvFrame/Closure/JIT/FFI live-ptr canary notes
// (observe-only inventory drained into post_moving_live_canaries_ before
// Moving densify). Additive; does not gate. Appended at header end so the
// DensifyConsistencyReport struct layout stays stable. Soft / Off /
// !moving_compact_enabled never increment (note helper early-returns).
inline std::atomic<std::uint64_t> g_moving_temporary_canary_noted_total{0};
inline constexpr int kMovingTemporaryCanaryIssue = 3210;
[[nodiscard]] inline std::uint64_t moving_temporary_canary_noted_total_v_read() noexcept {
    return g_moving_temporary_canary_noted_total.load(std::memory_order_relaxed);
}
inline void reset_moving_temporary_canary_noted_for_test() noexcept {
    g_moving_temporary_canary_noted_total.store(0, std::memory_order_relaxed);
}

// Issue #3274: densify-tracked FFI opaque / native alias cover. Bumped when
// a potentially-arena-tracked opaque alias is installed under production
// Moving with a stable void** slot available (slot-rewrite cover on the next
// densify). The canary axis of the same call reuses #3210's
// g_moving_temporary_canary_noted_total. Additive; does not gate. Soft /
// Off / !moving_compact_enabled never increment (helper early-returns to
// note_ffi_opaque_create_exempt). Appended at header end (layout-stable).
inline std::atomic<std::uint64_t> g_ffi_opaque_alias_slot_cover_total{0};
inline constexpr int kFfiOpaqueDensifyAliasCoverIssue = 3274;
[[nodiscard]] inline std::uint64_t ffi_opaque_alias_slot_cover_total_v_read() noexcept {
    return g_ffi_opaque_alias_slot_cover_total.load(std::memory_order_relaxed);
}
inline void reset_ffi_opaque_alias_slot_cover_for_test() noexcept {
    g_ffi_opaque_alias_slot_cover_total.store(0, std::memory_order_relaxed);
}

// Issue #3443: FFI/JIT live ptrs outside opaque_heap_ slots. Stamp only
// (no new query key / no g_3443_*). Cover SSOT stays slot XOR #3210
// canary XOR EXEMPT(would_move==false). Residual of #3022/#3057/#3055/
// #3210/#3368.
inline constexpr int kFfiJitLivePtrInventoryIssue = 3443;

} // namespace aura::core::densify_consistency

#endif // AURA_CORE_DENSIFY_CONSISTENCY_REPORT_H