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
extern "C" std::int64_t aura_jit_prim_dispatch(
    std::int64_t prim_id, std::int64_t* args, std::int32_t argc) {
    (void)prim_id; (void)args; (void)argc;
    return 0;
}
