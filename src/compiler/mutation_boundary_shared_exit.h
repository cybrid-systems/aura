// mutation_boundary_shared_exit.h — Issue #2600: shared exit helper for
// soft fiber boundary + full Guard outermost success paths.
//
// Avoids dual-rail drift between soft fiber boundary exit
// (src/compiler/evaluator_fiber_mutation.cpp orch_soft_boundary_exit)
// and full Guard outermost success exit
// (src/compiler/evaluator_mutation_boundary.cpp ResidualPolicy::Clear path).
// The shared helper centralizes:
//
//   1. residual GcDeferReason force-clear for the evaluator (idempotent
//      — force_clear_residual_defer_for_evaluator is atomic + CAS-based,
//      calling twice does not double-bump counters).
//   2. MutationHold release when this path owned outermost hold
//      (mutation_hold_defer_active + release_mutation_hold_defer +
//      reconcile_gc_defer_bits_after_clear for hold-bit ≠ Panic reconcile).
//
// Stack-light contract (per issue AC3):
//   - Does NOT construct a full MutationBoundaryGuard (soft path keeps
//     #1881 stack-light contract).
//   - Does NOT perform linear ownership probe (caller's responsibility;
//     avoids double-count with #2545 force_linear_rollback classify).
//   - Does NOT publish LayoutStamp / mutation-safety mirror (caller's
//     responsibility — soft path uses g_orch_soft_boundary_ev tracking
//     from #2515 symmetric-mirror contract; full Guard path uses the
//     Guard's evaluator pointer directly).
//
// Caller-side responsibilities:
//   - Soft fiber boundary: pop soft depth + publish held=false mirror
//     (using g_orch_soft_boundary_ev defuse version) BEFORE calling this
//     helper so steal / GC / is_at_mutation_boundary_safe see the soft
//     window as fully released (matches #2515 symmetric release order).
//   - Full Guard outermost: call this helper after ResidualPolicy::Clear
//     decides Clear (soft) and BEFORE densify Phase 5 specific work
//     so the unified gate sees a clean residual baseline.
//
// Zero cost when residual already zero and no hold owned (idempotent
// CAS short-circuits and the hold-release branch is a single relaxed load).
//
// Idempotent under chaos / multi-fiber (atomic + CAS-based — concurrent
// exits from sibling fibers just both observe the cleared state, no
// double-bump, no race).
//
// Ref: #2555 TransactionGuard real host + soft fiber path,
//      #2546 residual GcDefer hard-AND steal-complete,
//      #2515 soft boundary symmetric mirror,
//      #2314 residual clear helper (used directly here),
//      #2269 outermost residual clear policy,
//      #2554 chaos PR hard-fail gate (regression check).

#ifndef AURA_COMPILER_MUTATION_BOUNDARY_SHARED_EXIT_H
#define AURA_COMPILER_MUTATION_BOUNDARY_SHARED_EXIT_H

#include "core/gc_hooks.h"

namespace aura::compiler {

// Issue #2600: shared exit helper. Stack-light, idempotent.
// Performs:
//   1. residual GcDeferReason force-clear for the evaluator
//   2. MutationHold release (if owned) + reconcile
// Does NOT perform:
//   - linear ownership probe (caller; avoids #2545 double-count)
//   - full Guard dtor (soft path keeps #1881 stack-light)
//   - LayoutStamp / mutation-safety mirror publish (caller; soft path
//     uses g_orch_soft_boundary_ev from #2515; full Guard path uses
//     Guard's evaluator pointer directly)
inline void mutation_boundary_shared_exit(void* evaluator_id) noexcept {
    // 1. Force-clear residual GcDeferReason for this evaluator
    // (idempotent atomic + CAS-based — #2314 helper).
    if (evaluator_id) {
        (void)aura::gc_hooks::force_clear_residual_defer_for_evaluator(evaluator_id);
    }
    // 2. Release MutationHold if this path owned outermost hold
    // (#2338 overflow + #2269 outermost policy).
    if (aura::gc_hooks::mutation_hold_defer_active()) {
        aura::gc_hooks::release_mutation_hold_defer();
        // Final reconcile after hold release (hold bit ≠ Panic).
        (void)aura::gc_hooks::reconcile_gc_defer_bits_after_clear();
    }
}

} // namespace aura::compiler

#endif // AURA_COMPILER_MUTATION_BOUNDARY_SHARED_EXIT_H