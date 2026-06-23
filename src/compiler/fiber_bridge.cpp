// fiber_bridge.cpp — C-linkage shims for the
// per-fiber migration / GC safepoint / mutation
// boundary hooks used by Fiber::check_gc_safepoint
// (from src/serve/fiber.cpp) + Fiber::resume() + the
// (query:orchestration-metrics) primitive (Issue #451).
//
// This file is intentionally NOT a module partition —
// it's a standalone .cpp so non-module binaries
// (test_concurrent, test_issue_*) can include it
// directly via the CMake source list. The 3
// GC-safepoint + mutation-boundary-depth shims
// (aura_evaluator_mutation_boundary_depth,
// aura_evaluator_request_gc_safepoint,
// aura_evaluator_wait_for_safepoint) are defined in
// the module partition evaluator_fiber_mutation.cpp
// (compiled into the module's binary); the
// standalone .cpp version is only needed because
// non-module binaries (test_concurrent,
// test_issue_*) can't link to a module partition
// directly.
//
// The standalone-binary shims for the 3 above live
// in a non-module wrapper; see the previous
// version of this file (now removed) for the
// no-op implementations.
//
// For test_concurrent + test_issue_* the #438/#439
// shims are resolved from evaluator_fiber_mutation.cpp
// via the test_issue_*'s source list (test_concurrent
// has its own minimal shim path that delegates to
// the module's static state via the C-linkage).
//
// Issue #451: this file now provides the
// aura_fiber_static_gc_pause_attributed_to_mutation
// shim used by the (query:orchestration-metrics)
// primitive. The standalone-binary version returns
// 0 (no module state); the module binary resolves
// the real symbol at link time.

#include <cstddef>
#include <cstdint>

extern "C" {

// Issue #451: C-linkage shim for the static
// gc_pause_attributed_to_mutation_count_ on Fiber.
// The (query:orchestration-metrics) primitive
// (registered in evaluator_primitives_query.cpp)
// reads this via the shim. Returns 0 in standalone
// binary context (no module state).
std::uint64_t aura_fiber_static_gc_pause_attributed_to_mutation() {
    return 0;
}

} // extern "C"
