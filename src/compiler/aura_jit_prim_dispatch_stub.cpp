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

// Issue #2370 / CI link: hot_update_registry.cpp references
// aura_get_storm_eval_context (defined strongly in
// spec_jit_controller.cpp). test_concurrent links
// hot_update_registry + aura_jit_bridge but not SpecJIT
// controller — without a weak fallback, link fails with
// undefined reference. Strong definitions (controller /
// bridge_stub) still win when present.
extern "C" __attribute__((weak)) void aura_set_storm_eval_context(void* /*eval_ptr*/) noexcept {}
extern "C" __attribute__((weak)) void* aura_get_storm_eval_context(void) noexcept {
    return nullptr;
}
