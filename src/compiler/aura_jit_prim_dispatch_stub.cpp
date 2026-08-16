// Stub for aura_jit_prim_dispatch.
//
// test_concurrent (and other per-test binaries that pull in
// aura_jit.cpp) don't have access to service.ixx's
// aura_jit_prim_dispatch definition (which uses the global
// primitive context + lookup table, only available via
// CompilerService).
//
// The stub returns 0 (= "no primitive found"). For tests
// that exercise the JIT primitive path, they'd need a real
// CompilerService context.
//
// This file is added to test_concurrent's per-target
// sources only — aura's main binary gets the real one from
// service.ixx.

#include <cstdint>

// Issue #1527: weak so aura_test_objects / service.ixx strong definition
// wins when both are linked; provides a fallback for minimal JIT-only
// test binaries that do not compile service.ixx.
extern "C" __attribute__((weak)) std::int64_t
aura_jit_prim_dispatch(std::int64_t prim_id, std::int64_t* args, std::int32_t argc) {
    (void)prim_id;
    (void)args;
    (void)argc;
    return 0;
}

// Do not stub aura_set/get_storm_eval_context here. This TU is in
// aura_jit_test_objects (DT_NEEDED first for full-JIT tests); a weak
// no-op in the first DSO wins ELF search over the strong TLS in
// spec_jit_controller.cpp (aura_test_objects) and breaks #2370 PerEval
// isolation. Light-link binaries keep the weak fallback in
// aura_jit_bridge_stub.cpp.
