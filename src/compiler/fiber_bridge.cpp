// fiber_bridge.cpp — C-linkage shims for the
// per-fiber migration / GC safepoint / mutation
// boundary hooks used by Fiber::check_gc_safepoint
// (from src/serve/fiber.cpp) + Fiber::resume().
//
// This file is intentionally NOT a module partition —
// it's a standalone .cpp so non-module binaries
// (test_concurrent, test_issue_*) can include it
// directly via the CMake source list. The actual
// shim implementations delegate to the in-module
// helpers in evaluator_fiber_mutation.cpp via
// the aura_set_gc_hooks / aura_set_mutation_hooks
// C-linkage shim pattern (see #285 + #453 + #438 +
// #439 for the underlying module-side state).
//
// The shims are no-ops in standalone binary contexts
// (no module-state, no yield-hook evaluator) — the
// real implementation lives in the
// aura.compiler.evaluator module.

#include <cstddef>
#include <cstdint>

extern "C" {

// Issue #438: per-thread mutation boundary depth.
// Returns 0 when no module is loaded (standalone
// binary context).
std::size_t aura_evaluator_mutation_boundary_depth() {
    return 0;
}

// Issue #439: GC safepoint request.
// Returns 0 (immediate) when no module is loaded.
int aura_evaluator_request_gc_safepoint() {
    return 0;
}

// Issue #439: GC safepoint wait.
void aura_evaluator_wait_for_safepoint(std::uint64_t /*timeout_ms*/) {
}

} // extern "C"
