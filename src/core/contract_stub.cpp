// Global stub for __tu_has_violation.
//
// Background:
// The aura.compiler.constant_folding module (and other aura
// modules) use C++26 contracts (`pre(...)` / `post(...)`).
// When the test_issue_* binaries link against
// libaura_test_objects.a, the contract_handler.cpp in the
// lib generates __tu_has_violation as a LOCAL symbol
// (compiler-generated, "t" in nm). Static libraries do not
// export local symbols, so per-test binaries fail to link
// with "undefined reference to __tu_has_violation".
//
// The aura binary itself doesn't have this problem because
// it has -fcontracts globally and the .o files within the
// binary resolve their own references (each .o has its own
// private local copy).
//
// Fix: provide a default-visibility definition in a
// per-translation-unit source file that's part of the
// libaura_test_objects.a archive. The test linking then
// resolves the undefined reference via the static lib's
// normal lazy-pull mechanism.
//
// The signature must use GCC's internal type
// `__builtin_contract_violation_type` (a 33-character name
// in the mangled symbol), NOT std::contracts::contract_violation
// (which has a different mangled length). We forward-declare
// the builtin type since user code can't include the GCC
// internal header.
//
// The stub aborts on violation. Real violation handling
// (with diagnostic context) happens via handle_contract_violation
// in contract_handler.cpp, called from the aura binary.
// Per-test binaries don't exercise the violation path; this
// stub is a safety net so a test that DOES hit a contract
// violation fails loudly instead of UB-linking.
#include <cstdlib>

struct __builtin_contract_violation_type;

int __tu_has_violation(
    const __builtin_contract_violation_type& v, unsigned short s) {
    (void)v; (void)s;
    std::abort();
}

// Stub for aura_jit_prim_dispatch. test_concurrent (and other
// per-test binaries that pull in aura_jit.cpp) don't have
// access to service.ixx's aura_jit_prim_dispatch definition
// (which uses the global primitive context + lookup table).
// The stub returns 0 (= "no primitive found"). For tests
// that exercise the JIT primitive path, they'd need a real
// CompilerService context.
#include <cstdint>
extern "C" std::int64_t aura_jit_prim_dispatch(
    std::int64_t prim_id, std::int64_t* args, std::int32_t argc) {
    (void)prim_id; (void)args; (void)argc;
    return 0;
}
