// Issue #2858: stub for test_concurrent to avoid C++20 module
// compilation cascade. The real definition lives in
// src/compiler/evaluator_fiber_mutation.cpp (a C++20 module
// implementation unit that requires aura_target_cxx_modules() +
// evaluator.ixx BMI to compile). test_concurrent does NOT compile
// the full evaluator module, so we provide a weak no-op stub here
// (only compiled into the test_concurrent target, not the main
// aura binary which gets the real definition).
//
// This file is listed ONLY in target_sources(test_concurrent ...)
// — never in target_sources(aura ...) — so no duplicate symbol.

#include "compiler/aura_jit_bridge.h" // for the C-linkage declaration
#include <cstdint>

extern "C" int aura_evaluator_bump_macro_provenance_repin_on_steal(void* /*ev_ptr*/) noexcept {
    return 0; // stub: no-op for test_concurrent
}

// Issue #2370: PerEval storm TLS lives in spec_jit_controller.cpp.
// test_concurrent does not compile that TU (or aura_jit_bridge_stub.cpp);
// the first-DSO weak stubs were removed from
// aura_jit_prim_dispatch_stub.cpp so full-JIT DSOs keep the strong TLS.
// Light-link only: nullptr means Global / process-window storm path.
extern "C" __attribute__((weak)) void aura_set_storm_eval_context(void* /*eval_ptr*/) noexcept {}
extern "C" __attribute__((weak)) void* aura_get_storm_eval_context(void) noexcept {
    return nullptr;
}

// Strong definition lives in ir_cache_pure.ixx (full-module binaries).
// test_concurrent does not compile that module.
extern "C" __attribute__((weak)) void aura_clear_partial_relower_threshold_force(void) {}

// Issue #2263 / #2964: linear_fast_path_ok() in typed_mutation_audit.h
// calls this. Strong def is typed_mutation_audit_hooks.cpp (full-module
// binaries). test_concurrent does not compile that TU — without a stub
// asan-build fails: undefined symbol aura_escape_move_gate_active.
extern "C" __attribute__((weak)) int aura_escape_move_gate_active() noexcept {
    return 0; // stub: no escape gate in the concurrent-fiber binary
}
