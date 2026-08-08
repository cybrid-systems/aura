// steal_safety.cpp — Issue #2699 unified steal safety single transaction
// implementation. See steal_safety.h for the contract.
//
// The transaction is a thin wrapper that performs all AC1 safety steps
// in order and returns Ok / RejectHard. The actual primitives
// (snapshot check, residual GcDefer clear, PanicCheckpoint clear,
// LayoutStamp dual-check, linear provenance, ticket stamp) live in
// their existing TUs (worker.cpp / fiber.cpp / evaluator_fiber_mutation.cpp)
// and keep their per-primitive semantics + counters (AC4 contract).
//
// AC1 step-by-step:
//   1. Sample MutationSafetySnapshot (one seqlock + depth) — fiber.cpp
//   2. Inconsistency → production force-deopt + RejectHard (no soft
//      continue under production lock) — worker.cpp + aura_evaluator_on_steal_complete
//   3. Residual GcDefer hard-AND == 0 (after clear); non-zero → Cancel+Done
//      — aura::gc_hooks::force_clear_residual_defer_for_evaluator
//   4. Live PanicCheckpoint clear under production (#2667 semantics
//      preserved on hard_failed; #2710 extends to Ok path under
//      production / AURA_PANIC_CONTRACT=hard so a stolen fiber with a
//      live PanicCheckpoint cannot enqueue Ready without clearing the
//      previous Eval's GC arm) — aura_evaluator_on_steal_complete
//   5. LayoutStamp dual-check / forced restamp — aura_evaluator_on_steal_complete
//   6. Linear / StableNodeRef provenance probe — aura_evaluator_on_steal_complete
//   7. Stamp resume_safety_ticket only on Ok path — fiber.cpp
//
// First ship: thread the call through all 7 steps in order via the
// existing C-linkage + Fiber methods. Each step delegates to the
// existing primitive (no behavior change). The Ok / RejectHard return
// gates the enqueue path in worker.cpp (AC2).

#include "serve/steal_safety.h"

#include "serve/fiber.h" // Fiber, MutationSafetySnapshot, set_resume_safety_ticket,
                         // mutation_safety_snapshot, mutation_safety_snapshot_inconsistent
#include "core/densify_consistency_report.h" // #2745 last densify envframe/dual_epoch residual
#include "core/gc_hooks.h" // aura::gc_hooks::force_clear_residual_defer_for_evaluator

// Forward declaration: aura_evaluator_on_steal_complete is declared
// weak in worker.cpp (worker.cpp:41). We forward-declare strong here
// so the link surface picks up either the strong (production) or weak
// (light test) implementation. The actual strong impl lives in
// src/compiler/evaluator_fiber_mutation.cpp.
extern "C" void aura_evaluator_on_steal_complete(void* fiber_ptr) noexcept;

// Issue #2721: LayoutStamp dual-check via existing C-linkage shim
// (strong def in evaluator_fiber_mutation.cpp). Returns non-zero on
// mismatch; called inside the transaction hard-AND BEFORE the ticket
// stamp so a mismatched LayoutStamp cannot enqueue Ready.
extern "C" int aura_evaluator_check_resume_layout_stamp(void* fiber_ptr) noexcept;

// Issue #2721: per-victim evaluator_id getter for the GC defer
// hard-AND (predicate (d) in the issue body). Returns the victim's
// evaluator_id (NOT the stealer's current thread-local) so the
// gc_deferred_for_evaluator() check is against the right owner.
// Strong def in fiber.cpp; weak no-op stub in fiber_bridge.cpp.
extern "C" void* aura_fiber_evaluator_id_for_steal_safety(void* fiber_ptr) noexcept;

namespace aura::serve {

StealSafetyDecision steal_safety_transaction(Fiber* stolen) noexcept {
    if (!stolen) [[unlikely]] {
        // Defensive: caller passes null fiber → RejectHard.
        g_steal_safety_transaction_calls_total.fetch_add(1, std::memory_order_relaxed);
        g_steal_safety_transaction_reject_hard_total.fetch_add(1, std::memory_order_relaxed);
        return StealSafetyDecision::RejectHard;
    }
    g_steal_safety_transaction_calls_total.fetch_add(1, std::memory_order_relaxed);

    // AC1 step 1 — sample MutationSafetySnapshot (one seqlock + depth).
    // fiber.cpp owns the snapshot seqlock; we just read it here.
    const auto snap = stolen->mutation_safety_snapshot();

    // AC1 step 2 — inconsistency → production force-deopt + RejectHard.
    // We re-check the seqlock via fiber's helper rather than duplicating
    // the abort logic.
    const bool inconsistent = stolen->mutation_safety_snapshot_inconsistent(snap);
    if (inconsistent) {
        // Delegate to the existing strong ABI for the force-deopt path.
        // Production strict: must succeed or the existing caller aborts.
        // (Strong C-linkage — not a weak function pointer; do not null-check
        // the address under -Werror=address.)
        aura_evaluator_on_steal_complete(stolen);
        // After force-deopt, the fiber is no longer enqueue-safe —
        // RejectHard regardless of soft-mode (soft path can't override
        // production force-deopt under the new single-transaction contract).
        g_steal_safety_transaction_reject_hard_total.fetch_add(1, std::memory_order_relaxed);
        return StealSafetyDecision::RejectHard;
    }

    // AC1 steps 3-6 — Residual GcDefer force-clear (#2314 interlock) +
    // PanicCheckpoint clear + LayoutStamp dual-check + linear /
    // StableNodeRef provenance. The strong ABI
    // aura_evaluator_on_steal_complete (evaluator_fiber_mutation.cpp)
    // owns the Evaluator* resolution via evaluator_for_scheduler_hooks()
    // and runs residual clear with the correct evaluator id. serve/
    // cannot name Evaluator without a module import, so the residual
    // clear is not re-issued here (would take void* evaluator, not
    // fiber id — the earlier fiber_id() form was a type error).
    // Single-transaction contract: one call covers AC1 steps 3-6
    // (AC5 coverage linter asserts the call graph).
    aura_evaluator_on_steal_complete(stolen);

    // AC1 #2721 — hard-AND residual safety checks INSIDE the transaction
    // BEFORE the ticket stamp. #2699 stamped the ticket on the Ok path
    // but residual predicates (per-fiber mutation boundary safety,
    // LayoutStamp match, resume-ticket consistency, GC-defer arm state)
    // were still consulted AFTER the transaction returned Ok in some
    // resume / yield paths — opening a window for stale-ticket resume
    // or concurrent MutationHold steal. #2721 hard-ANDs all 4 inside
    // the transaction: if any fails → bump the matching counter +
    // RejectHard WITHOUT stamping the ticket (no post-transaction escape
    // hatch). Production fail-closed (soft / sandbox stays metric-only
    // per #2699 contract).
    bool residual_ok = true;
    // (a) Per-fiber mutation boundary safety — re-check after the
    // evaluator_on_steal_complete clear (AC1 step 3-6) to catch
    // concurrent re-arm races between clear and stamp.
    if (!stolen->is_at_mutation_boundary_safe()) {
        g_steal_safety_residual_boundary_unsafe_total.fetch_add(1, std::memory_order_relaxed);
        residual_ok = false;
    }
    // (b) LayoutStamp match — fresh-check via existing C-linkage shim
    // (returns non-zero on mismatch). Mismatch here means the fiber's
    // stored LayoutStamp drifted from the worker's current after the
    // on_steal_complete dual-check — concurrent epoch bump + steal race.
    if (aura_evaluator_check_resume_layout_stamp(stolen) != 0) {
        g_steal_safety_residual_layout_stamp_mismatch_total.fetch_add(1, std::memory_order_relaxed);
        residual_ok = false;
    }
    // (c) Resume-ticket consistency — if the victim already has a ticket
    // stored from a prior steal that didn't complete (e.g., a different
    // stealer's transaction that aborted but left a ticket), and the
    // stored ticket differs from snap.ticket, reject. Closes the
    // "ticket was set by steal-A, steal-B's transaction sees a stale
    // ticket" window.
    if (stolen->has_resume_safety_ticket() && stolen->resume_safety_ticket() != snap.ticket) {
        g_steal_safety_residual_ticket_mismatch_total.fetch_add(1, std::memory_order_relaxed);
        residual_ok = false;
    }
    // (d) GC-defer arm state for the VICTIM's evaluator (not the
    // stealer's current thread-local). Uses a C-linkage getter to
    // resolve the victim's evaluator_id (strong def in fiber.cpp; weak
    // no-op stub in fiber_bridge.cpp returns nullptr for non-evaluator
    // link units — GC defer check skipped in that case).
    {
        void* victim_eval_id = aura_fiber_evaluator_id_for_steal_safety(stolen);
        if (victim_eval_id != nullptr &&
            aura::gc_hooks::gc_deferred_for_evaluator(victim_eval_id)) {
            g_steal_safety_residual_gc_defer_armed_total.fetch_add(1, std::memory_order_relaxed);
            residual_ok = false;
        }
    }
    // (e) Issue #2745: EnvFrame residual after densify — last densify left
    // envframe_ok=false or dual_epoch lag. Quiet path (no densify yet,
    // call_seq==0) skips. Counters always bump; RejectHard path matches
    // arms (a–d) (production fail-closed / Soft metric via existing matrix).
    if (aura::core::densify_consistency::last_densify_call_seq() > 0) {
        if (!aura::core::densify_consistency::last_densify_envframe_ok() ||
            !aura::core::densify_consistency::last_densify_dual_epoch_ok()) {
            g_steal_safety_residual_envframe_lag_total.fetch_add(1, std::memory_order_relaxed);
            residual_ok = false;
        }
    }
    if (!residual_ok) {
        // Reject WITHOUT stamping the ticket — no post-transaction
        // escape hatch. Production fail-closed (soft / sandbox stays
        // metric-only per #2699 contract; the counters above bump
        // regardless so dashboards can attribute the miss).
        g_steal_safety_transaction_reject_hard_total.fetch_add(1, std::memory_order_relaxed);
        return StealSafetyDecision::RejectHard;
    }

    // AC1 step 7 — stamp resume_safety_ticket only on Ok path
    // (after the hard-AND passed).
    stolen->set_resume_safety_ticket(snap.ticket);

    g_steal_safety_transaction_ok_total.fetch_add(1, std::memory_order_relaxed);
    return StealSafetyDecision::Ok;
}

} // namespace aura::serve