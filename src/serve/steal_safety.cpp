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
#include "core/lifetime_consistency_proof.hh" // #2957 last LifetimeConsistencyProof residual arm

#include <thread> // std::this_thread::yield for rare same-fiber decision spin

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

namespace {

    // Issue #2954 / #2901: per-Fiber exclusive decision window for residual
    // hard-AND + ticket stamp. Replaces process-wide g_steal_safety_decision_mu
    // so concurrent steals of *different* victims do not serialize.
    // try_begin CAS-spins on rare same-fiber contention (bumps contention_total).
    // RAII end_steal_decision on all exit paths (Ok + RejectHard).
    struct StealDecisionGuard {
        Fiber* fiber = nullptr;
        explicit StealDecisionGuard(Fiber* f) noexcept
            : fiber(f) {
            if (!fiber)
                return;
            // Uncontended path: single CAS. Contended (same victim): yield +
            // contention counter; spin until free (same-fiber exclusive).
            while (!fiber->try_begin_steal_decision()) {
                g_steal_decision_contention_total.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        }
        ~StealDecisionGuard() noexcept {
            if (fiber)
                fiber->end_steal_decision();
        }
        StealDecisionGuard(const StealDecisionGuard&) = delete;
        StealDecisionGuard& operator=(const StealDecisionGuard&) = delete;
    };

    // Issue #2929: bump dedicated StealInvariant failure counter.
    // Residual arms re-use residual_* totals so #2721/#2745 surfaces stay valid.
    void note_steal_invariant_fail(StealInvariant inv) noexcept {
        switch (inv) {
            case StealInvariant::SnapshotConsistent:
                g_steal_safety_invariant_snapshot_fail_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                break;
            case StealInvariant::BoundarySafe:
                // Keep fetch_add(1 on same line for #2721 AC greps.
                g_steal_safety_residual_boundary_unsafe_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                break;
            case StealInvariant::LayoutStampMatch:
                // Keep fetch_add(1 on same line for #2721 AC greps.
                g_steal_safety_residual_layout_stamp_mismatch_total.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case StealInvariant::TicketFresh:
                g_steal_safety_residual_ticket_mismatch_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                break;
            case StealInvariant::GcDeferClear:
                g_steal_safety_residual_gc_defer_armed_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                break;
            case StealInvariant::EnvFrameOk:
                g_steal_safety_residual_envframe_lag_total.fetch_add(1, std::memory_order_relaxed);
                break;
            case StealInvariant::LifetimeProofOk:
                // Keep fetch_add(1 on same line for #2957 AC greps.
                g_steal_safety_residual_lifetime_proof_reject_total.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case StealInvariant::Count:
            default:
                break;
        }
    }

    // Boolean wrapper (preserves #2721/#2901 residual_ok naming for greps).
    [[nodiscard]] bool evaluate_residual_hard_and(Fiber* stolen, const MutationSafetySnapshot& snap,
                                                  bool bump_counters) noexcept {
        return evaluate_residual_hard_and_bits(stolen, snap, bump_counters) == 0;
    }

} // namespace

// Issue #2929 / #2721 / #2987: evaluate residual hard-AND arms as named
// StealInvariant checks. Returns fail bit-set (0 = all pass).
// When bump_counters is true, increments the matching invariant
// counter on each failing arm. When false (quiet re-sample / mailbox),
// only returns bits — zero atomics on the clean path (AC2 / #2987 AC3).
// skip_mask omits arms (mailbox skips LifetimeProofOk; EnvFrameOk only
// when the payload carries a held-ref). Same table as steal:
//   BoundarySafe      — is_at_mutation_boundary_safe
//   LayoutStampMatch  — aura_evaluator_check_resume_layout_stamp
//   TicketFresh       — resume ticket == snap.ticket
//   GcDeferClear      — victim evaluator GC defer clear
//   EnvFrameOk        — densify EnvFrame residual (#2745)
//   LifetimeProofOk   — last LifetimeConsistencyProof (#2957, production)
[[nodiscard]] std::uint64_t evaluate_residual_hard_and_bits(Fiber* stolen,
                                                            const MutationSafetySnapshot& snap,
                                                            bool bump_counters,
                                                            std::uint64_t skip_mask) noexcept {
    std::uint64_t fail_bits = 0;
    if (!stolen)
        return steal_invariant_mask(StealInvariant::SnapshotConsistent);
    const auto skip = [skip_mask](StealInvariant inv) noexcept {
        return (skip_mask & steal_invariant_mask(inv)) != 0;
    };
    // StealInvariant::BoundarySafe
    if (!skip(StealInvariant::BoundarySafe) && !stolen->is_at_mutation_boundary_safe()) {
        fail_bits |= steal_invariant_mask(StealInvariant::BoundarySafe);
        if (bump_counters)
            note_steal_invariant_fail(StealInvariant::BoundarySafe);
    }
    // StealInvariant::LayoutStampMatch
    if (!skip(StealInvariant::LayoutStampMatch) &&
        aura_evaluator_check_resume_layout_stamp(stolen) != 0) {
        fail_bits |= steal_invariant_mask(StealInvariant::LayoutStampMatch);
        if (bump_counters)
            note_steal_invariant_fail(StealInvariant::LayoutStampMatch);
    }
    // StealInvariant::TicketFresh
    if (!skip(StealInvariant::TicketFresh) && stolen->has_resume_safety_ticket() &&
        stolen->resume_safety_ticket() != snap.ticket) {
        fail_bits |= steal_invariant_mask(StealInvariant::TicketFresh);
        if (bump_counters)
            note_steal_invariant_fail(StealInvariant::TicketFresh);
    }
    // StealInvariant::GcDeferClear
    // (#2721 hard-AND + #2727 per-Fiber durable evaluator_id identity)
    if (!skip(StealInvariant::GcDeferClear)) {
        void* victim_eval_id = aura_fiber_evaluator_id_for_steal_safety(stolen);
        if (victim_eval_id != nullptr &&
            aura::gc_hooks::gc_deferred_for_evaluator(victim_eval_id)) {
            fail_bits |= steal_invariant_mask(StealInvariant::GcDeferClear);
            if (bump_counters)
                note_steal_invariant_fail(StealInvariant::GcDeferClear);
        }
    }
    // StealInvariant::EnvFrameOk — Issue #2745: EnvFrame residual after densify.
    // Issue #3001: chaos soak fail-closed if this arm grows without RejectHard.
    if (!skip(StealInvariant::EnvFrameOk) &&
        aura::core::densify_consistency::last_densify_call_seq() > 0) {
        if (!aura::core::densify_consistency::last_densify_envframe_ok() ||
            !aura::core::densify_consistency::last_densify_dual_epoch_ok()) {
            fail_bits |= steal_invariant_mask(StealInvariant::EnvFrameOk);
            if (bump_counters)
                note_steal_invariant_fail(StealInvariant::EnvFrameOk);
        }
    }
    // StealInvariant::LifetimeProofOk — Issue #2957 residual arm (f).
    // Soft: skip entirely (no loads). Production/Hard only when last
    // proof is stamped AND !would_allow AND recent densify.
    // Issue #3001: soak fail-closed if this arm grows without matching
    // RejectHard / no-ticket (last_reject_invariant_bits covers the arm).
    if (!skip(StealInvariant::LifetimeProofOk) && is_steal_snapshot_hard_mode()) {
        namespace lcp = aura::core::lifetime_consistency_proof;
        if (lcp::last_lifetime_consistency_proof_present() &&
            aura::core::densify_consistency::last_densify_call_seq() > 0 &&
            !lcp::last_lifetime_consistency_would_allow()) {
            fail_bits |= steal_invariant_mask(StealInvariant::LifetimeProofOk);
            if (bump_counters)
                note_steal_invariant_fail(StealInvariant::LifetimeProofOk);
        }
    }
    return fail_bits;
}

StealSafetyDecision steal_safety_transaction(Fiber* stolen) noexcept {
    if (!stolen) [[unlikely]] {
        // Defensive: caller passes null fiber → RejectHard.
        g_steal_safety_transaction_calls_total.fetch_add(1, std::memory_order_relaxed);
        g_steal_safety_transaction_reject_hard_total.fetch_add(1, std::memory_order_relaxed);
        // No concrete invariant — leave last bits unchanged.
        return StealSafetyDecision::RejectHard;
    }
    g_steal_safety_transaction_calls_total.fetch_add(1, std::memory_order_relaxed);

    // AC1 step 1 — sample MutationSafetySnapshot (one seqlock + depth).
    // fiber.cpp owns the snapshot seqlock; we just read it here.
    const auto snap = stolen->mutation_safety_snapshot();

    // AC1 step 2 / StealInvariant::SnapshotConsistent → production
    // force-deopt + RejectHard. We re-check the seqlock via fiber's helper
    // rather than duplicating the abort logic.
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
        // StealInvariant::SnapshotConsistent fail (dedicated counter + bits).
        g_steal_safety_invariant_snapshot_fail_total.fetch_add(1, std::memory_order_relaxed);
        g_steal_safety_last_reject_invariant_bits.store(
            steal_invariant_mask(StealInvariant::SnapshotConsistent), std::memory_order_relaxed);
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
    // Issue #2901 / #2954: primary residual clear remains a single
    // on_steal_complete on the happy path (AC2). Hard-AND + quiet
    // re-sample + ticket stamp run under the *per-Fiber* decision window
    // so concurrent densify / Guard re-enter cannot re-arm residual between
    // observation and stamp without being observed — without process-wide
    // mutex serialization across unrelated victims (#2954).
    aura_evaluator_on_steal_complete(stolen);

    // AC1 #2721 + #2901 + #2954 — residual hard-AND + stamp under exclusive
    // *per-Fiber* decision window. #2721 hard-ANDs residual predicates BEFORE
    // ticket stamp. #2901 closes the re-arm window between clear and stamp:
    //   - hard-AND under decision window (with optional test inject hook)
    //   - quiet re-sample under same window (zero counter atomics when clean)
    //   - on any residual fail: force second residual clear + rearm_race
    //     counter + RejectHard WITHOUT stamping the ticket
    // #2954: decision window is Fiber::try_begin_steal_decision (CAS), not
    // a process-wide mutex.
    {
        StealDecisionGuard decision_guard(stolen);

        // Test seam: inject residual re-arm between clear and hard-AND.
        if (g_steal_safety_between_clear_and_hard_and_hook != nullptr) {
            g_steal_safety_between_clear_and_hard_and_hook();
        }

        std::uint64_t fail_bits =
            evaluate_residual_hard_and_bits(stolen, snap, /*bump_counters=*/true);
        bool residual_ok = (fail_bits == 0);

        // Quiet re-sample under the same decision window — catches re-arm that
        // landed after the first sample but before stamp. Clean path:
        // no counter bumps (AC2 zero extra atomics).
        if (residual_ok) {
            if (!evaluate_residual_hard_and(stolen, snap, /*bump_counters=*/false)) {
                // Re-arm race: attribute residual arms + race counter.
                fail_bits = evaluate_residual_hard_and_bits(stolen, snap, /*bump_counters=*/true);
                g_steal_safety_residual_rearm_race_total.fetch_add(1, std::memory_order_relaxed);
                residual_ok = false;
            }
        } else {
            // Residual observed after primary clear — treat as re-arm /
            // incomplete clear window; force second clear below.
            g_steal_safety_residual_rearm_race_total.fetch_add(1, std::memory_order_relaxed);
        }

        if (!residual_ok) {
            // Issue #2901: force second residual clear so a residual-armed
            // fiber cannot leave residue for a later enqueue path after
            // RejectHard. No ticket stamp (no post-transaction escape).
            // Issue #2929: publish failing invariant bit-set for Agents.
            g_steal_safety_last_reject_invariant_bits.store(fail_bits, std::memory_order_relaxed);
            aura_evaluator_on_steal_complete(stolen);
            g_steal_safety_transaction_reject_hard_total.fetch_add(1, std::memory_order_relaxed);
            return StealSafetyDecision::RejectHard;
        }

        // AC1 step 7 — stamp resume_safety_ticket only on Ok path
        // (after hard-AND + quiet re-sample passed under the same window).
        // Issue #2844 sole-enqueue: ticket stamp ONLY here (all invariants Ok).
        stolen->set_resume_safety_ticket(snap.ticket);
    }

    g_steal_safety_transaction_ok_total.fetch_add(1, std::memory_order_relaxed);
    return StealSafetyDecision::Ok;
}

MailboxDeliverySafety mailbox_delivery_safety_transaction(Fiber* target,
                                                          const MutationSafetySnapshot* snap,
                                                          bool check_envframe) noexcept {
    MailboxDeliverySafety out{};
    // Issue #2987: test inject short-circuits (same StealInvariant bits)
    // without steal mutex / ticket stamp / on_steal_complete.
    switch (g_mailbox_delivery_inject) {
        case MailboxDeliveryInject::LayoutStamp:
            out.fail_bits = steal_invariant_mask(StealInvariant::LayoutStampMatch);
            out.decision = StealSafetyDecision::RejectHard;
            return out;
        case MailboxDeliveryInject::TicketStale:
            out.fail_bits = steal_invariant_mask(StealInvariant::TicketFresh);
            out.decision = StealSafetyDecision::RejectHard;
            return out;
        case MailboxDeliveryInject::ResidualGcDefer:
            out.fail_bits = steal_invariant_mask(StealInvariant::GcDeferClear);
            out.decision = StealSafetyDecision::RejectHard;
            return out;
        case MailboxDeliveryInject::EnvFrame:
            out.fail_bits = steal_invariant_mask(StealInvariant::EnvFrameOk);
            out.decision = StealSafetyDecision::RejectHard;
            return out;
        case MailboxDeliveryInject::None:
        default:
            break;
    }
    if (!target) {
        out.decision = StealSafetyDecision::Ok;
        return out;
    }
    MutationSafetySnapshot local = snap ? *snap : target->mutation_safety_snapshot();
    std::uint64_t skip = steal_invariant_mask(StealInvariant::LifetimeProofOk);
    if (!check_envframe)
        skip |= steal_invariant_mask(StealInvariant::EnvFrameOk);
    // Mailbox never bumps steal counters (own mailbox_* totals).
    out.fail_bits = evaluate_residual_hard_and_bits(target, local, /*bump_counters=*/false, skip);
    out.decision = (out.fail_bits == 0) ? StealSafetyDecision::Ok : StealSafetyDecision::RejectHard;
    return out;
}

} // namespace aura::serve