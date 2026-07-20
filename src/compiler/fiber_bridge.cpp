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


import std;
extern "C" {

// Issue #438: per-thread mutation boundary depth.
__attribute__((weak)) std::size_t aura_evaluator_mutation_boundary_depth() {
    return 0;
}

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

// Issue #1880: orch agent body try_acquire (strong defs in evaluator_fiber_mutation.cpp).
__attribute__((weak, used)) int aura_orch_agent_body_try_acquire() {
    return 0; // no evaluator → allow body
}
__attribute__((weak, used)) void aura_orch_agent_body_release_guard() {}

} // extern "C"