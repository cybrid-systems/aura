// steal_safety.h — Issue #2699 unified steal safety single transaction.
//
// Steal safety logic was previously distributed across three call sites:
//   - WorkerThread::try_steal_from (src/serve/worker.cpp) — candidate filter
//     then sole enqueue via steal_safety_transaction Ok (#2752/#2844);
//     call_steal_complete.
//   - aura_evaluator_on_steal_complete (src/compiler/evaluator_fiber_mutation.cpp)
//     — residual GcDefer clear (#2314/#2546), PanicCheckpoint clear (#2667),
//     LayoutStamp dual-check, linear provenance.
//   - Fiber::check_and_enforce_resume_snapshot_invariant (src/serve/fiber.cpp)
//     — ticket mismatch + snapshot hard-fail on resume.
//
// This header introduces a single authoritative transaction
// `steal_safety_transaction(Fiber*)` that performs all safety steps in
// order. Production must have one Ok path or one RejectHard path — no
// silent continue, no Soft path under production lock. Soft / sandbox
// / test-override path remains metric-only (no production lock
// violation). Production Soft env ignored when
// `steal_snapshot_soft_production_locked()`.
//
// AC1 contract — steps in order:
//   1. Sample MutationSafetySnapshot (one seqlock + depth)
//   2. Inconsistency → production force-deopt + RejectHard (no soft
//      continue under production lock)
//   3. Residual GcDefer hard-AND == 0 (after clear); non-zero → Cancel+Done
//   4. Live PanicCheckpoint clear under production (#2667 semantics
//      preserved)
//   5. LayoutStamp dual-check / forced restamp
//   6. Linear / StableNodeRef provenance probe
//   7. Stamp resume_safety_ticket only on Ok path
//
// AC4 — existing counters (steal_snapshot_mismatch_force_deopt_total,
// residual_defer_steal_hard_fail_total, panic_checkpoint_cleared_on_steal_total,
// steal_safety_ticket_mismatch_total) remain additive / non-regressing.

#pragma once

#include <atomic>
#include <cstdint>

namespace aura::serve {

class Fiber;

enum class StealSafetyDecision : std::uint8_t {
    Ok = 0,
    RejectHard = 1,
};

// Issue #2929: named invariants for the sole-enqueue gate hard-AND.
// Every residual arm in steal_safety_transaction maps to one value;
// RejectHard records the failing bit-set (bit N = StealInvariant N).
// Stable ABI for Agents / dashboards / linters — do not reorder.
enum class StealInvariant : std::uint8_t {
    SnapshotConsistent = 0, // MutationSafetySnapshot seqlock consistent
    BoundarySafe = 1,       // is_at_mutation_boundary_safe
    LayoutStampMatch = 2,   // aura_evaluator_check_resume_layout_stamp
    TicketFresh = 3,        // resume ticket matches snap.ticket
    GcDeferClear = 4,       // victim evaluator GC defer clear
    EnvFrameOk = 5,         // densify EnvFrame residual (#2745)
    LifetimeProofOk = 6,    // last LifetimeConsistencyProof would_allow (#2957)
    Count = 7,
};

[[nodiscard]] inline constexpr std::uint64_t steal_invariant_mask(StealInvariant inv) noexcept {
    return static_cast<std::uint64_t>(1) << static_cast<unsigned>(inv);
}

inline constexpr int kStealSafetyTransactionIssue = 2699;
inline constexpr int kStealSafetyTransactionHardAndIssue = 2721;
// Issue #2745: EnvFrame hold_gen / dual-epoch residual hard-AND arm.
inline constexpr int kStealSafetyEnvFrameResidualIssue = 2745;
// Issue #2929: explicit StealInvariant table + last RejectHard bits.
inline constexpr int kStealSafetyInvariantTableIssue = 2929;

// File-scope atomics (mirror #2693/#2694/#2695/#2696/#2697/#2698 pattern).
// The transaction itself is the caller; these counters surface the
// unified-call-graph observability that AC5's coverage linter asserts.
inline std::atomic<std::uint64_t> g_steal_safety_transaction_calls_total{0};
inline std::atomic<std::uint64_t> g_steal_safety_transaction_reject_hard_total{0};
inline std::atomic<std::uint64_t> g_steal_safety_transaction_ok_total{0};
inline std::atomic<std::uint32_t> g_steal_safety_transaction_wired{1};

// Issue #2721: hard-AND residual safety checks inside the atomic
// decision. #2699 stamped the ticket only on the Ok path, but residual
// predicates (per-fiber mutation boundary safety, LayoutStamp match,
// resume-ticket consistency, GC-defer arm state) were still consulted
// AFTER the transaction returned Ok in some resume / yield paths —
// opening a window for stale-ticket resume or concurrent MutationHold
// steal. #2721 hard-ANDs all 4 inside the transaction BEFORE the ticket
// stamp. If any fails → bump the matching counter + RejectHard WITHOUT
// stamping the ticket (no post-transaction escape hatch). Production
// fail-closed (soft / sandbox stays metric-only per #2699 contract).
inline std::atomic<std::uint64_t> g_steal_safety_residual_boundary_unsafe_total{0};
inline std::atomic<std::uint64_t> g_steal_safety_residual_layout_stamp_mismatch_total{0};
inline std::atomic<std::uint64_t> g_steal_safety_residual_ticket_mismatch_total{0};
inline std::atomic<std::uint64_t> g_steal_safety_residual_gc_defer_armed_total{0};
// Issue #2745: EnvFrame residual (last densify envframe_ok=false or dual_epoch lag).
inline std::atomic<std::uint64_t> g_steal_safety_residual_envframe_lag_total{0};
// Issue #2957: last LifetimeConsistencyProof negative after recent densify
// (production residual arm (f)). Soft / no densify / no proof: no bump.
inline std::atomic<std::uint64_t> g_steal_safety_residual_lifetime_proof_reject_total{0};
inline std::atomic<std::uint32_t> g_steal_safety_residual_hard_and_wired{1};
inline constexpr int kStealSafetyLifetimeProofResidualIssue = 2957;
// Issue #2901: residual re-arm race between on_steal_complete clear and
// hard-AND / ticket stamp. Bumped only on the fail path (RejectHard after
// clear when residual is still/re-observed). Quiet happy path: zero bump.
inline std::atomic<std::uint64_t> g_steal_safety_residual_rearm_race_total{0};
inline std::atomic<std::uint32_t> g_steal_safety_residual_rearm_race_wired{1};
inline constexpr int kStealSafetyResidualRearmRaceIssue = 2901;
// Issue #2954: per-Fiber decision protocol (replaces process-wide mutex).
// contention_total bumps when try_begin_steal_decision CAS fails (same
// victim concurrent decision). per_fiber_wired=1 when Ok path uses Fiber
// CAS instead of g_steal_safety_decision_mu.
inline std::atomic<std::uint64_t> g_steal_decision_contention_total{0};
inline std::atomic<std::uint32_t> g_steal_decision_per_fiber_wired{1};
inline constexpr int kStealDecisionPerFiberIssue = 2954;

// Issue #2929: SnapshotConsistent fail counter (inconsistency path before
// residual hard-AND). Residual arms re-use residual_* counters above as
// the dedicated per-StealInvariant failure totals (BoundarySafe →
// residual_boundary_unsafe, … EnvFrameOk → residual_envframe_lag).
inline std::atomic<std::uint64_t> g_steal_safety_invariant_snapshot_fail_total{0};
// Bit-set of last RejectHard failing invariants (bit N = StealInvariant N).
// Soft/quiet Ok path does not write (Agents read last RejectHard only).
inline std::atomic<std::uint64_t> g_steal_safety_last_reject_invariant_bits{0};
inline std::atomic<std::uint32_t> g_steal_safety_invariant_table_wired{1};

[[nodiscard]] inline std::uint64_t steal_safety_transaction_calls_v_read() noexcept {
    return g_steal_safety_transaction_calls_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t steal_safety_transaction_reject_hard_v_read() noexcept {
    return g_steal_safety_transaction_reject_hard_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t steal_safety_transaction_ok_v_read() noexcept {
    return g_steal_safety_transaction_ok_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t steal_safety_transaction_wired_v_read() noexcept {
    return g_steal_safety_transaction_wired.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t steal_safety_residual_boundary_unsafe_total_v_read() noexcept {
    return g_steal_safety_residual_boundary_unsafe_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t
steal_safety_residual_layout_stamp_mismatch_total_v_read() noexcept {
    return g_steal_safety_residual_layout_stamp_mismatch_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t steal_safety_residual_ticket_mismatch_total_v_read() noexcept {
    return g_steal_safety_residual_ticket_mismatch_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t steal_safety_residual_gc_defer_armed_total_v_read() noexcept {
    return g_steal_safety_residual_gc_defer_armed_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t steal_safety_residual_envframe_lag_total_v_read() noexcept {
    return g_steal_safety_residual_envframe_lag_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t
steal_safety_residual_lifetime_proof_reject_total_v_read() noexcept {
    return g_steal_safety_residual_lifetime_proof_reject_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t steal_safety_residual_hard_and_wired_v_read() noexcept {
    return g_steal_safety_residual_hard_and_wired.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t steal_safety_residual_rearm_race_total_v_read() noexcept {
    return g_steal_safety_residual_rearm_race_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t steal_safety_residual_rearm_race_wired_v_read() noexcept {
    return g_steal_safety_residual_rearm_race_wired.load(std::memory_order_relaxed);
}
// Issue #2954: per-Fiber decision observability.
[[nodiscard]] inline std::uint64_t steal_decision_contention_total_v_read() noexcept {
    return g_steal_decision_contention_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t steal_decision_per_fiber_wired_v_read() noexcept {
    return g_steal_decision_per_fiber_wired.load(std::memory_order_relaxed);
}
// Issue #2929: StealInvariant table accessors.
[[nodiscard]] inline std::uint64_t steal_safety_invariant_snapshot_fail_total_v_read() noexcept {
    return g_steal_safety_invariant_snapshot_fail_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t steal_safety_last_reject_invariant_bits_v_read() noexcept {
    return g_steal_safety_last_reject_invariant_bits.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t steal_safety_invariant_table_wired_v_read() noexcept {
    return g_steal_safety_invariant_table_wired.load(std::memory_order_relaxed);
}
// Per-invariant fail total (SSOT for Agents). Residual counters alias
// the existing residual_* totals so #2721/#2745 dashboards stay valid.
[[nodiscard]] inline std::uint64_t steal_safety_invariant_fail_total(StealInvariant inv) noexcept {
    switch (inv) {
        case StealInvariant::SnapshotConsistent:
            return steal_safety_invariant_snapshot_fail_total_v_read();
        case StealInvariant::BoundarySafe:
            return steal_safety_residual_boundary_unsafe_total_v_read();
        case StealInvariant::LayoutStampMatch:
            return steal_safety_residual_layout_stamp_mismatch_total_v_read();
        case StealInvariant::TicketFresh:
            return steal_safety_residual_ticket_mismatch_total_v_read();
        case StealInvariant::GcDeferClear:
            return steal_safety_residual_gc_defer_armed_total_v_read();
        case StealInvariant::EnvFrameOk:
            return steal_safety_residual_envframe_lag_total_v_read();
        case StealInvariant::LifetimeProofOk:
            return steal_safety_residual_lifetime_proof_reject_total_v_read();
        case StealInvariant::Count:
        default:
            return 0;
    }
}

// The single authoritative transaction. Returns Ok (fiber ready to
// enqueue) or RejectHard (Cancel+Done — never local_queue_.push). On
// RejectHard, the caller MUST skip the enqueue path. Existing counters
// (snapshot mismatch / residual defer / panic checkpoint / ticket
// mismatch) keep bumping via the underlying primitives — additive, not
// regressing.
StealSafetyDecision steal_safety_transaction(Fiber* stolen) noexcept;

// Issue #2901: test seam — optional hook invoked after on_steal_complete
// clear and before residual hard-AND / stamp (under the per-Fiber decision
// window). Used to inject concurrent residual re-arm between clear and stamp.
// Nullptr default (production); never called on the quiet path when unset.
// Set only from unit tests; cleared after use.
inline thread_local void (*g_steal_safety_between_clear_and_hard_and_hook)() noexcept = nullptr;

// Test reset (used by the #81967 extension in
// test_steal_complete_restamp_txn.cpp or successor).
inline void clear_steal_safety_transaction_for_test() noexcept {
    g_steal_safety_transaction_calls_total.store(0, std::memory_order_relaxed);
    g_steal_safety_transaction_reject_hard_total.store(0, std::memory_order_relaxed);
    g_steal_safety_transaction_ok_total.store(0, std::memory_order_relaxed);
    g_steal_safety_residual_boundary_unsafe_total.store(0, std::memory_order_relaxed);
    g_steal_safety_residual_layout_stamp_mismatch_total.store(0, std::memory_order_relaxed);
    g_steal_safety_residual_ticket_mismatch_total.store(0, std::memory_order_relaxed);
    g_steal_safety_residual_gc_defer_armed_total.store(0, std::memory_order_relaxed);
    g_steal_safety_residual_envframe_lag_total.store(0, std::memory_order_relaxed);
    g_steal_safety_residual_lifetime_proof_reject_total.store(0, std::memory_order_relaxed);
    g_steal_safety_residual_rearm_race_total.store(0, std::memory_order_relaxed);
    g_steal_safety_invariant_snapshot_fail_total.store(0, std::memory_order_relaxed);
    g_steal_safety_last_reject_invariant_bits.store(0, std::memory_order_relaxed);
    g_steal_decision_contention_total.store(0, std::memory_order_relaxed);
    g_steal_safety_between_clear_and_hard_and_hook = nullptr;
}

} // namespace aura::serve