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
//      preserved) — aura_evaluator_on_steal_complete
//   5. LayoutStamp dual-check / forced restamp — aura_evaluator_on_steal_complete
//   6. Linear / StableNodeRef provenance probe — aura_evaluator_on_steal_complete
//   7. Stamp resume_safety_ticket only on Ok path — fiber.cpp
//
// First ship: thread the call through all 7 steps in order via the
// existing C-linkage + Fiber methods. Each step delegates to the
// existing primitive (no behavior change). The Ok / RejectHard return
// gates the enqueue path in worker.cpp (AC2).

#include "serve/steal_safety.h"

#include "serve/fiber.h"   // Fiber, MutationSafetySnapshot, set_resume_safety_ticket,
                           // mutation_safety_snapshot, mutation_safety_snapshot_inconsistent
#include "core/gc_hooks.h" // aura::gc_hooks::force_clear_residual_defer_for_evaluator

// Forward declaration: aura_evaluator_on_steal_complete is declared
// weak in worker.cpp (worker.cpp:41). We forward-declare strong here
// so the link surface picks up either the strong (production) or weak
// (light test) implementation. The actual strong impl lives in
// src/compiler/evaluator_fiber_mutation.cpp.
extern "C" void aura_evaluator_on_steal_complete(void* fiber_ptr) noexcept;

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
        if (aura_evaluator_on_steal_complete) {
            aura_evaluator_on_steal_complete(stolen);
        }
        // After force-deopt, the fiber is no longer enqueue-safe —
        // RejectHard regardless of soft-mode (soft path can't override
        // production force-deopt under the new single-transaction contract).
        g_steal_safety_transaction_reject_hard_total.fetch_add(1, std::memory_order_relaxed);
        return StealSafetyDecision::RejectHard;
    }

    // AC1 step 3 — Residual GcDefer hard-AND == 0 after clear; non-zero
    // → Cancel+Done. The existing helper force-clears + returns 0 on
    // success / non-zero on hard-fail.
    const std::int32_t defer_after = aura::gc_hooks::force_clear_residual_defer_for_evaluator(
        static_cast<std::uint64_t>(stolen->fiber_id()));
    if (defer_after != 0) {
        g_steal_safety_transaction_reject_hard_total.fetch_add(1, std::memory_order_relaxed);
        return StealSafetyDecision::RejectHard;
    }

    // AC1 steps 4-6 — PanicCheckpoint clear + LayoutStamp dual-check +
    // linear / StableNodeRef provenance probe. The existing
    // aura_evaluator_on_steal_complete runs these in order. We call it
    // once here (single-transaction contract — AC5 coverage linter
    // asserts the call graph).
    if (aura_evaluator_on_steal_complete) {
        aura_evaluator_on_steal_complete(stolen);
    }

    // AC1 step 7 — stamp resume_safety_ticket only on Ok path.
    stolen->set_resume_safety_ticket(snap.ticket);

    g_steal_safety_transaction_ok_total.fetch_add(1, std::memory_order_relaxed);
    return StealSafetyDecision::Ok;
}

} // namespace aura::serve