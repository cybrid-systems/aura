// lifetime_consistency_proof.hh — Issue #2888: unified LifetimeConsistencyProof.
//
// Aggregates the four proof surfaces + residual flags into ONE read-only
// Agent-visible snapshot for long-running self-evo loops:
//
//   | Component              | Source                                    | Issue |
//   |------------------------|-------------------------------------------|-------|
//   | EnvFrameLifetimeProof  | envframe_lifetime.ixx snapshot            | #2711 |
//   | TypeLinearCommitProof  | typed_mutation_audit.h outcome + counts   | #2854 |
//   | LifetimePinStats       | lifetime_pin.hh contract fail / remap miss| #2265 |
//   | LayoutStamp components | layout_stamp.hh arena_gen/flat_gen/env_gen| #2170 |
//   | residual flags         | gc_hooks.h residual_defer_after_exit      | #2846 |
//
// Design (mirrors lifetime_contract.h — pure aggregation, zero module deps):
//   - `LifetimeConsistencyProof` is a plain POD read-only snapshot. Callers
//     (stamp sites / query surface) pass live counter values; this header
//     only classifies — never bumps counters, never mutates env or atomics.
//   - `stamp_lifetime_consistency_proof()` publishes a compact process-wide
//     last-proof atomic set for high-frequency Agent poll (no full-struct
//     rebuild per poll). The full struct is built on-demand by the query
//     surface from live state (mirrors #2711/#2697 first-ship approach).
//   - AC3: Soft / empty / no densify → zero extra atomics on the quiet
//     path. The stamp sites only run on the outermost densify success path
//     and steal-complete; a quiet mutate never stamps, so the last-proof
//     atomics stay at healthy-empty defaults (would_allow_commit=true).
//
// Unified formula (would_allow_commit):
//   ok = (densify_scan_fail == 0)                      // envframe axis
//     && (hold_gen_mismatch_total == 0)                // envframe hold pin
//     && (type_linear_outcome != Reject)               // type×linear axis
//     && (pin_contract_fail_total == 0)                // pin axis
//     && (residual_defer_after_exit_total == 0)        // residual axis
//
// force_reason_code (bitmask — multiple axes can fail together):
//   0  = healthy
//   1  = envframe densify ownership scan fail
//   2  = envframe hold-gen mismatch under live guard
//   4  = type-linear rebind/scan reject (production)
//   8  = pin contract fail (Moving densify breach)
//   16 = residual defer-after-exit (sticky #2846)
//   (remap_miss_total is sticky historical — informational, does NOT fail
//    commit; mirrors lifetime_contract.h defer-orphan treatment.)
//
// Additive only: EnvFrameLifetimeProof / TypeLinearCommitProof / pin stats /
// LayoutStamp surfaces stay intact (AC4). No docs/design/ per #1655 (AC5).

#ifndef AURA_CORE_LIFETIME_CONSISTENCY_PROOF_HH
#define AURA_CORE_LIFETIME_CONSISTENCY_PROOF_HH

#include <atomic>
#include <cstdint>

namespace aura::core::lifetime_consistency_proof {

inline constexpr int kLifetimeConsistencyProofIssue = 2888;

// TypeLinearCommitProof outcome sentinels (mirror typed_mutation_audit.h):
// 0=Quiet (no rebind attempted), 1=Stamped (rebind+scan ok),
// 2=Reject (rebind fail OR scan mismatch under production).
inline constexpr std::uint8_t kTypeLinearOutcomeQuiet = 0;
inline constexpr std::uint8_t kTypeLinearOutcomeStamped = 1;
inline constexpr std::uint8_t kTypeLinearOutcomeReject = 2;

// force_reason_code bits (see header comment).
inline constexpr std::uint32_t kProofReasonEnvframeScanFail = 1u << 0;
inline constexpr std::uint32_t kProofReasonEnvframeHoldGenMismatch = 1u << 1;
inline constexpr std::uint32_t kProofReasonTypeLinearReject = 1u << 2;
inline constexpr std::uint32_t kProofReasonPinContractFail = 1u << 3;
inline constexpr std::uint32_t kProofReasonResidualDefer = 1u << 4;

// Read-only aggregate snapshot. POD — trivially copyable; safe to pass by
// value across the mutation boundary / query surface without breaking the
// FFI flat signature. All fields are component reads at snapshot time; the
// unified would_allow_commit / force_reason_code are computed locally.
struct LifetimeConsistencyProof {
    // ── EnvFrameLifetimeProof (#2711) ──
    std::uint64_t envframe_hold_gen = 0;
    std::uint64_t envframe_compact_gen = 0;
    std::uint64_t envframe_scans_run = 0;
    std::uint64_t envframe_densify_scan_total = 0;
    std::uint64_t envframe_densify_scan_fail = 0;
    std::uint64_t envframe_hold_gen_mismatch_total = 0;
    // ── TypeLinearCommitProof (#2854) ──
    std::uint8_t type_linear_outcome = kTypeLinearOutcomeQuiet; // 0/1/2
    std::uint64_t type_linear_linear_root_count = 0;            // post-rebind collect
    std::uint64_t type_linear_stamped_after_rebind_total = 0;
    std::uint64_t type_linear_reject_after_rebind_fail_total = 0;
    // ── LifetimePinStats (#2265 / #2266) ──
    std::uint64_t pin_contract_fail_total = 0;
    std::uint64_t pin_remap_miss_total = 0; // informational (does not fail commit)
    // ── LayoutStamp components (#2170 / #2255 / #2432) ──
    std::uint64_t layout_arena_gen = 0;
    std::uint64_t layout_flat_gen = 0;
    std::uint64_t layout_env_gen = 0;
    // ── residual flags (#2846) ──
    std::uint64_t residual_defer_after_exit_total = 0;
    // ── unified ──
    std::uint64_t mutation_epoch = 0; // shared envframe/layout epoch at snapshot
    bool would_allow_commit = true;
    std::uint32_t force_reason_code = 0;
    // Schema / issue sentinels (additive — no regression on #2711 / #2854).
    static constexpr int kSchema = 2888;
};

// Pure aggregation. Inputs are process-level counters / live depths; this
// function only classifies — never mutates atomics or env. Mirrors
// make_lifetime_contract_snapshot() (#2300).
[[nodiscard]] inline LifetimeConsistencyProof make_lifetime_consistency_proof(
    std::uint64_t envframe_hold_gen = 0, std::uint64_t envframe_compact_gen = 0,
    std::uint64_t envframe_scans_run = 0, std::uint64_t envframe_densify_scan_total = 0,
    std::uint64_t envframe_densify_scan_fail = 0,
    std::uint64_t envframe_hold_gen_mismatch_total = 0, std::uint8_t type_linear_outcome = 0,
    std::uint64_t type_linear_linear_root_count = 0,
    std::uint64_t type_linear_stamped_after_rebind_total = 0,
    std::uint64_t type_linear_reject_after_rebind_fail_total = 0,
    std::uint64_t pin_contract_fail_total = 0, std::uint64_t pin_remap_miss_total = 0,
    std::uint64_t layout_arena_gen = 0, std::uint64_t layout_flat_gen = 0,
    std::uint64_t layout_env_gen = 0, std::uint64_t residual_defer_after_exit_total = 0,
    std::uint64_t mutation_epoch = 0) noexcept {
    LifetimeConsistencyProof p;
    p.envframe_hold_gen = envframe_hold_gen;
    p.envframe_compact_gen = envframe_compact_gen;
    p.envframe_scans_run = envframe_scans_run;
    p.envframe_densify_scan_total = envframe_densify_scan_total;
    p.envframe_densify_scan_fail = envframe_densify_scan_fail;
    p.envframe_hold_gen_mismatch_total = envframe_hold_gen_mismatch_total;
    p.type_linear_outcome = type_linear_outcome;
    p.type_linear_linear_root_count = type_linear_linear_root_count;
    p.type_linear_stamped_after_rebind_total = type_linear_stamped_after_rebind_total;
    p.type_linear_reject_after_rebind_fail_total = type_linear_reject_after_rebind_fail_total;
    p.pin_contract_fail_total = pin_contract_fail_total;
    p.pin_remap_miss_total = pin_remap_miss_total;
    p.layout_arena_gen = layout_arena_gen;
    p.layout_flat_gen = layout_flat_gen;
    p.layout_env_gen = layout_env_gen;
    p.residual_defer_after_exit_total = residual_defer_after_exit_total;
    p.mutation_epoch = mutation_epoch;

    // Hard-fail signals only (informational fields do not fail commit).
    const bool fail_envframe_scan = envframe_densify_scan_fail > 0;
    const bool fail_envframe_hold = envframe_hold_gen_mismatch_total > 0;
    const bool fail_type_linear = (type_linear_outcome == kTypeLinearOutcomeReject);
    const bool fail_pin = pin_contract_fail_total > 0;
    const bool fail_residual = residual_defer_after_exit_total > 0;

    p.would_allow_commit = !fail_envframe_scan && !fail_envframe_hold && !fail_type_linear &&
                           !fail_pin && !fail_residual;

    p.force_reason_code = 0;
    if (fail_envframe_scan)
        p.force_reason_code |= kProofReasonEnvframeScanFail;
    if (fail_envframe_hold)
        p.force_reason_code |= kProofReasonEnvframeHoldGenMismatch;
    if (fail_type_linear)
        p.force_reason_code |= kProofReasonTypeLinearReject;
    if (fail_pin)
        p.force_reason_code |= kProofReasonPinContractFail;
    if (fail_residual)
        p.force_reason_code |= kProofReasonResidualDefer;
    return p;
}

// ── Process-wide last-proof atomics (high-frequency Agent poll) ──────────
// Written by stamp_lifetime_consistency_proof() at the outermost densify
// success path + steal-complete. Quiet path (no densify / no steal) never
// stamps → these stay at healthy-empty defaults (AC3 zero extra atomics).
inline std::atomic<std::uint8_t>& g_lcp_last_would_allow_commit() noexcept {
    static std::atomic<std::uint8_t> v{1}; // healthy-empty default
    return v;
}
inline std::atomic<std::uint32_t>& g_lcp_last_force_reason_code() noexcept {
    static std::atomic<std::uint32_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_lcp_last_mutation_epoch() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_lcp_stamped_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint32_t>& g_lcp_wired() noexcept {
    static std::atomic<std::uint32_t> v{1};
    return v;
}

// Publish the compact last-proof atomic set (called from stamp sites).
// The full struct is still built on-demand by the query surface; this only
// stores the three poll-hot fields + bumps the stamped_total counter.
inline void stamp_lifetime_consistency_proof(const LifetimeConsistencyProof& p) noexcept {
    g_lcp_last_would_allow_commit().store(p.would_allow_commit ? 1 : 0, std::memory_order_relaxed);
    g_lcp_last_force_reason_code().store(p.force_reason_code, std::memory_order_relaxed);
    g_lcp_last_mutation_epoch().store(p.mutation_epoch, std::memory_order_relaxed);
    g_lcp_stamped_total().fetch_add(1, std::memory_order_relaxed);
}

// Cheap high-frequency poll: reads the three atomics only (no full-struct
// rebuild, no counter bumps).
struct LastProofPoll {
    bool would_allow_commit = true;
    std::uint32_t force_reason_code = 0;
    std::uint64_t mutation_epoch = 0;
    std::uint64_t stamped_total = 0;
};

[[nodiscard]] inline LastProofPoll last_lifetime_consistency_proof() noexcept {
    LastProofPoll poll;
    poll.would_allow_commit = g_lcp_last_would_allow_commit().load(std::memory_order_relaxed) != 0;
    poll.force_reason_code = g_lcp_last_force_reason_code().load(std::memory_order_relaxed);
    poll.mutation_epoch = g_lcp_last_mutation_epoch().load(std::memory_order_relaxed);
    poll.stamped_total = g_lcp_stamped_total().load(std::memory_order_relaxed);
    return poll;
}

// Issue #2957: cheap residual hard-AND probes (single relaxed load each).
// Proof "present" = stamped_total > 0 (quiet path never stamps → unset).
// Steal residual arm (f) gates only when present + !would_allow + recent densify
// under production/Hard — Soft skips the arm entirely.
[[nodiscard]] inline bool last_lifetime_consistency_proof_present() noexcept {
    return g_lcp_stamped_total().load(std::memory_order_relaxed) > 0;
}
[[nodiscard]] inline bool last_lifetime_consistency_would_allow() noexcept {
    return g_lcp_last_would_allow_commit().load(std::memory_order_relaxed) != 0;
}
[[nodiscard]] inline std::uint32_t last_lifetime_consistency_force_reason() noexcept {
    return g_lcp_last_force_reason_code().load(std::memory_order_relaxed);
}

// Test hook: reset the last-proof atomics to healthy-empty (stamped_total
// included so tests can assert exact stamp counts).
inline void reset_lifetime_consistency_proof_for_test() noexcept {
    g_lcp_last_would_allow_commit().store(1, std::memory_order_relaxed);
    g_lcp_last_force_reason_code().store(0, std::memory_order_relaxed);
    g_lcp_last_mutation_epoch().store(0, std::memory_order_relaxed);
    g_lcp_stamped_total().store(0, std::memory_order_relaxed);
}

// Issue #3185: densify-entry LCP consult helper. Cheap single-load surface
// for the Phase-5 / optional one-shot Moving densify entry path (matches the
// already-wired steal-complete arm in evaluator_fiber_mutation.cpp which
// publishes the stamp on exit; the entry side consults the same atomic set).
// Soft / Off path: stamped_total stays 0 (quiet path) → present=false →
// caller does NOT consult (zero-cost branch per AC4). Soft live_compact
// itself does NOT relocate per AC2, so the consultation is only on the
// Moving decision point.
struct DensifyEntryLCPPoll {
    bool present = false;
    bool would_allow_commit = true;
    std::uint32_t force_reason_code = 0;
};

[[nodiscard]] inline DensifyEntryLCPPoll consult_last_lcp_for_densify_entry() noexcept {
    DensifyEntryLCPPoll poll;
    poll.present = last_lifetime_consistency_proof_present();
    poll.would_allow_commit = last_lifetime_consistency_would_allow();
    poll.force_reason_code = last_lifetime_consistency_force_reason();
    return poll;
}

// Issue #3185 AC1: Agent-visible counter for densify-entry LCP blocks.
// Bumped when consult_last_lcp_for_densify_entry returns present=true
// AND would_allow_commit=false at a Moving densify entry point (Phase-5
// or optional one-shot Moving). Add-only (no existing counter under this
// name; pair with the existing densify-soak dashboards).
inline std::atomic<std::uint64_t>& g_densify_entry_lcp_blocked_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}

// Test hook: reset the densify-entry LCP blocked counter.
inline void reset_densify_entry_lcp_blocked_for_test() noexcept {
    g_densify_entry_lcp_blocked_total().store(0, std::memory_order_relaxed);
}

} // namespace aura::core::lifetime_consistency_proof

#endif // AURA_CORE_LIFETIME_CONSISTENCY_PROOF_HH
