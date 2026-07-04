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

} // extern "C"