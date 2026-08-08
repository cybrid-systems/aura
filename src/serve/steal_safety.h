// steal_safety.h — Issue #2699 unified steal safety single transaction.
//
// Steal safety logic was previously distributed across three call sites:
//   - WorkerThread::try_steal_from (src/serve/worker.cpp) — snapshot sample,
//     is_stealable(snap), inconsistency bump, force-deopt, ticket stamp,
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

inline constexpr int kStealSafetyTransactionIssue = 2699;
inline constexpr int kStealSafetyTransactionHardAndIssue = 2721;
// Issue #2745: EnvFrame hold_gen / dual-epoch residual hard-AND arm.
inline constexpr int kStealSafetyEnvFrameResidualIssue = 2745;

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
inline std::atomic<std::uint32_t> g_steal_safety_residual_hard_and_wired{1};

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
[[nodiscard]] inline std::uint32_t steal_safety_residual_hard_and_wired_v_read() noexcept {
    return g_steal_safety_residual_hard_and_wired.load(std::memory_order_relaxed);
}

// The single authoritative transaction. Returns Ok (fiber ready to
// enqueue) or RejectHard (Cancel+Done — never local_queue_.push). On
// RejectHard, the caller MUST skip the enqueue path. Existing counters
// (snapshot mismatch / residual defer / panic checkpoint / ticket
// mismatch) keep bumping via the underlying primitives — additive, not
// regressing.
StealSafetyDecision steal_safety_transaction(Fiber* stolen) noexcept;

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
}

} // namespace aura::serve