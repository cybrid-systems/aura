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
#include <cstddef>
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
extern "C" __attribute__((weak)) void aura_escape_move_gate_clear() noexcept {}

// typed_mutation_audit.h inlines call these. Strong defs live in
// ownership_rebind.cpp / typed_mutation_audit_hooks.cpp (full-module
// binaries). test_concurrent does not compile those TUs — without
// stubs asan-build / build-test / ubsan-smoke fail at link:
//   linear_or_dirty_roots_count_for_rebind
//   maybe_persist_typed_summary
namespace aura::compiler {
__attribute__((weak)) std::size_t linear_or_dirty_roots_count_for_rebind() noexcept {
    return 0; // stub: no live rebind roots in the concurrent-fiber binary
}
} // namespace aura::compiler

// Issue #3346: stamp last-look CS consult. Strong def lives in
// evaluator_mutation_boundary.cpp (TypeChecker-using). Light-link
// test_concurrent does not compile that TU — last-look reports match
// so builders still instantiate (Soft/Off never calls this anyway).
extern "C" __attribute__((weak)) int
aura_stamp_last_look_cs_matches(void* /*tc_handle*/, std::uint64_t /*expected_goals*/,
                                std::uint64_t /*expected_fp*/) noexcept {
    return 1; // stub: match (no TypeChecker in this binary)
}

// Last-look reject inlines clear_occurrence_persist_buffer → this C ABI.
// Strong def: evaluator_mutation_boundary.cpp. Light-link: nothing to drop.
extern "C" __attribute__((weak)) std::uint64_t
aura_clear_occurrence_persist_snapshot_tc(void* /*tc_handle*/) noexcept {
    return 0;
}

// Issue #3482: Evaluator persist clear. Strong def:
// evaluator_mutation_boundary.cpp. Light-link: no persist log to drop.
extern "C" __attribute__((weak)) void
aura_clear_occurrence_persist_buffer(void* /*ev_ptr*/) noexcept {}

// Issue #3347: live_policy / grant remirror residual CastOp persist.
// Strong def: dirty_propagation.ixx. Light-link: no persist, no pending.
extern "C" __attribute__((weak)) std::size_t
aura_force_residual_castop_undermark_into_cone() noexcept {
    return 0;
}
extern "C" __attribute__((weak)) int aura_residual_castop_undermark_pending() noexcept {
    return 0;
}
extern "C" __attribute__((weak)) void aura_reset_residual_castop_persist_for_test() noexcept {}
// Issue #3294 CI: light-link has no lifetime pin state to disarm.
extern "C" __attribute__((weak)) void aura_reset_general_object_pin_required_for_test() noexcept {}

// Stamp builders also instantiate commit_readiness_live_policy /
// commit_readiness (header-inline). Strong defs live in
// evaluator_mutation_boundary.cpp. Fail-closed: no live TC, no recover.
namespace aura::compiler::typed_audit {
struct CommitReadinessInput;
}
extern "C" __attribute__((weak)) void aura_typed_audit_fill_from_live_tc(
    void* /*ev*/, aura::compiler::typed_audit::CommitReadinessInput* /*out*/) noexcept {}
extern "C" __attribute__((weak)) bool
aura_typed_audit_try_occurrence_hard_face_full_solve_recover() noexcept {
    return false;
}
extern "C" __attribute__((weak)) void* aura_typed_audit_current_commit_type_checker() noexcept {
    return nullptr;
}

// Issue #3547: light-link has no TLS Evaluator / workspace FlatAST.
extern "C" __attribute__((weak)) std::uint32_t
aura_tls_workspace_type_id(std::uint32_t /*node*/) noexcept {
    return 0;
}

// Issue #3544: aura_macro_provenance_repin_on_steal (in aura_jit_bridge.cpp)
// unwinds per-fiber hygiene slot on steal/repin. Strong def lives in
// aura_jit_bridge_stub.cpp (weak, no-op) for full/light JIT DSOs, and
// macro_expansion.cpp for module-aware builds. test_concurrent does NOT
// pull in either TU — without this stub asan-build fails to link:
//   undefined reference to `aura_unwind_fiber_hygiene_on_steal'
// (test_concurrent_clone_hygiene_depth.cpp exercises it directly).
extern "C" __attribute__((weak)) void
aura_unwind_fiber_hygiene_on_steal(std::uint32_t /*fiber_id*/) noexcept {}

namespace aura::compiler::typed_audit {
struct TypedMutationAuditEvent;
__attribute__((weak)) void maybe_persist_typed_summary(const TypedMutationAuditEvent&) noexcept {
    // stub: no mutation WAL persist (Soft / zero extra)
}
} // namespace aura::compiler::typed_audit
