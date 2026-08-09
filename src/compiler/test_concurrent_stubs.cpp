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
