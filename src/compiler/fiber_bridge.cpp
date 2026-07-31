// fiber_bridge.cpp — C-linkage shims for the
// per-fiber migration / GC safepoint / mutation
// boundary hooks used by Fiber::check_gc_safepoint
// (from src/serve/fiber.cpp) + Fiber::resume().
//
// This file is intentionally NOT a module partition —
// it's a standalone .cpp so non-module binaries
// (test_concurrent, test_issue_*) can include it
// directly via the CMake source list.
//
// Module binaries (aura, test_ir, aura_test_objects)
// also link this file; the weak no-op stubs below are
// overridden by the strong implementations in
// evaluator_fiber_mutation.cpp.
//
// Issue #451's aura_fiber_static_gc_pause_attributed_to_mutation
// lives in fiber.cpp (next to the static counter).


#include "core/gc_hooks.h" // Issue #2377: steal_complete_entry_missing

#include <cstdio>
#include <cstdlib>

import std;

// Issue #2372 / #2377: weak force-deopt / steal-complete no-ops must not
// silently continue under production (multi-worker builds must resolve
// the strong ABI in evaluator_fiber_mutation.cpp / aura_jit_bridge.cpp).
// Forward declare the production Soft lock probe (defined in serve/fiber.cpp).
namespace aura::serve {
[[nodiscard]] bool steal_snapshot_soft_production_locked() noexcept;
}

extern "C" {

// Issue #438: per-thread mutation boundary depth.
__attribute__((weak)) std::size_t aura_evaluator_mutation_boundary_depth() {
    return 0;
}

// Issue #2114: outermost MutationBoundaryGuard held flag (weak stub).
__attribute__((weak)) int aura_evaluator_mutation_boundary_held() {
    return 0;
}

// Issue #2347: weak no-op when Evaluator not linked (mailbox Strict force path).
extern "C" __attribute__((weak)) void aura_evaluator_mark_outermost_mutation_failed() noexcept {}

// Issue #588: per-fiber stack depth probe (weak stub).
__attribute__((weak)) std::size_t
aura_evaluator_mutation_stack_depth_from_ptr(void* /*mutation_stack_storage*/) {
    return 0;
}

// Issue #439: GC safepoint request.
__attribute__((weak)) int aura_evaluator_request_gc_safepoint() {
    return 0;
}

// Issue #439: GC safepoint wait.
__attribute__((weak)) void aura_evaluator_wait_for_safepoint(std::uint64_t /*timeout_ms*/) {}

// Issue #683: linear ownership probe on fiber steal.
__attribute__((weak)) void aura_evaluator_probe_linear_on_steal() {}

// Issue #2203 / #2377: steal-complete single entry (strong def in
// evaluator_fiber_mutation.cpp runs Panic clear → residual → LayoutStamp
// → linear/outermost as one transaction). Weak stub keeps light test
// binaries resolving without the evaluator TU.
//
// Issue #2377 AC1/AC3: under production Soft lock the weak no-op MUST
// NOT silently return (that skips residual #2314 + stamp #2351). Abort
// so mis-linked multi-worker production builds fail closed. Light /
// AURA_SANDBOX=off binaries bump steal_complete_entry_missing_total.
__attribute__((weak, used)) void aura_evaluator_on_steal_complete(void* /*fiber_ptr*/) {
    if (aura::serve::steal_snapshot_soft_production_locked()) {
        std::fprintf(stderr, "FATAL: weak aura_evaluator_on_steal_complete resolved under "
                             "production (#2377); multi-worker builds must link the strong "
                             "steal-complete ABI (Panic clear + residual + LayoutStamp)\n");
        std::abort();
    }
    aura::gc_hooks::bump_steal_complete_entry_missing_total();
}

// Issue #2310 / #2372: fail-closed force-deopt on steal snapshot
// inconsistency. Strong def in evaluator_fiber_mutation.cpp (with
// Evaluator module access — bumps per-CompilerMetrics counter + runs
// refresh). aura_jit_bridge.cpp provides a file-level atomic fallback
// when the module TU is not linked. This weak stub keeps non-evaluator
// link units (test_concurrent / test_issue_*) resolving without dragging
// the full module into their link unit.
//
// Issue #2372 AC2: under production Soft lock the weak no-op MUST NOT
// silently return (that would resume generation-behind code after a
// mismatch bump). Abort so mis-linked multi-worker production builds
// fail closed. Light/test binaries without production lock still get
// the empty no-op for link ergonomics.
__attribute__((weak, used)) void
aura_force_deopt_on_steal_snapshot_mismatch(void* /*fiber_ptr*/) noexcept {
    if (aura::serve::steal_snapshot_soft_production_locked()) {
        std::fprintf(stderr, "FATAL: weak aura_force_deopt_on_steal_snapshot_mismatch resolved "
                             "under production (#2372); multi-worker builds must link the "
                             "strong force-deopt ABI\n");
        std::abort();
    }
}

// Issue #485: deferred steal violation + resume migration.
__attribute__((weak)) void aura_evaluator_bump_steal_deferred_violation() {}
__attribute__((weak)) void aura_evaluator_bump_mutation_steal_attempt() {}
__attribute__((weak)) void aura_evaluator_resume_fiber_migration() {}
// Issue #1490: post-yield EnvFrame/bridge_epoch refresh (strong def in
// evaluator_fiber_mutation.cpp).
__attribute__((weak, used)) void aura_evaluator_post_resume_refresh() {}

// Issue #812: steal + arena/GC safepoint coordination (worker.cpp).
// Strong defs live in evaluator_fiber_mutation.cpp; weak no-ops keep
// test_concurrent / other non-evaluator link units happy.
// `used` prevents --gc-sections from dropping empty weak stubs before
// resolution (observed as undefined symbol under ASAN+lld rebuilds).
__attribute__((weak, used)) void aura_evaluator_bump_steal_arena_yield() {}
__attribute__((weak, used)) void aura_evaluator_bump_steal_outermost_enforced() {}

// aura_evaluator_on_fiber_join (referenced from src/serve/fiber.cpp:606
// in Fiber::join lambda). Strong definition lives in
// evaluator_fiber_mutation.cpp (module). Weak no-op here so non-module
// binaries (test_concurrent, test_issue_*) link without dragging the
// full module into their link unit.
__attribute__((weak, used)) void aura_evaluator_on_fiber_join(void* /*joined_fiber*/) {}

// Issue #1880 / #2118: orch agent body try_acquire (strong defs in evaluator_fiber_mutation.cpp).
__attribute__((weak, used)) int aura_orch_agent_body_try_acquire() {
    return 0; // no evaluator → allow body
}
__attribute__((weak, used)) int aura_orch_agent_body_try_acquire_ex(int /*register_soft*/) {
    return 0;
}
__attribute__((weak, used)) void aura_orch_agent_body_release_guard() {}
__attribute__((weak, used)) void aura_orch_note_agent_steal_skipped_boundary() {}

// Issue #2010: mailbox backpressure → orch dashboard (strong def in
// evaluator_fiber_mutation.cpp when evaluator/orch is linked).
__attribute__((weak, used)) void aura_orch_note_mailbox_backpressure() {}

} // extern "C"