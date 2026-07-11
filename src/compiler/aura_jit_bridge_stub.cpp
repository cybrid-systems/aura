// aura_jit_bridge_stub.cpp — minimal C-linkage stubs for tests that
// link aura_jit.cpp + aura_jit_runtime.cpp but don't include the
// full aura_jit_bridge.cpp (which is a module .cpp and can't be
// compiled standalone). The stub provides the symbols aura_jit.cpp
// references (e.g. aura_notify_jit_unhandled_opcode) so the link
// succeeds. The full aura_jit_bridge.cpp provides the production
// implementation; the stub is for test binaries that don't need
// the production code path.
//
// Issue #226 follow-up: Without this stub, test_spec_jit /
// test_jit_metrics / test_jit_concurrent_compile / test_jit_consistency
// fail to link with "undefined reference to
// aura_notify_jit_unhandled_opcode". The production
// aura_jit_bridge.cpp can't be linked because it uses `import std;`
// and is part of the aura compiler module.

#include "aura_jit_bridge.h"

#include <atomic>

extern "C" void aura_notify_jit_unhandled_opcode(const char* fn_name) {
    (void)fn_name;
    // Stub: no-op in test binaries. The full implementation
    // in aura_jit_bridge.cpp increments
    // CompilerMetrics::unhandled_opcode_count and emits a
    // diagnostic via CompilerService::repl_diagnostic.
}

// Additional stubs for symbols that aura_jit.cpp may reference.
// Add as needed when new tests fail to link.
static void* g_aot_metrics_stub = nullptr;
static std::uint64_t g_aot_metrics_lazy_stub = 0;
static std::uint64_t g_aot_metrics_explicit_stub = 0;

extern "C" void aura_set_aot_metrics(void* metrics) {
    g_aot_metrics_stub = metrics;
    if (metrics)
        ++g_aot_metrics_explicit_stub;
}

extern "C" void aura_ensure_aot_metrics(void* metrics) {
    if (metrics && !g_aot_metrics_stub) {
        g_aot_metrics_stub = metrics;
        ++g_aot_metrics_lazy_stub;
    }
}

extern "C" void* aura_get_aot_metrics(void) {
    return g_aot_metrics_stub;
}

extern "C" std::uint64_t aura_aot_metrics_lazy_init_total(void) {
    return g_aot_metrics_lazy_stub;
}

extern "C" std::uint64_t aura_aot_metrics_explicit_sets_total(void) {
    return g_aot_metrics_explicit_stub;
}

extern "C" void aura_jit_epoch_acquire_fence(void) {
    std::atomic_thread_fence(std::memory_order_acquire);
}

extern "C" void aura_jit_linear_post_invalidate_safety(std::uint8_t linear_state,
                                                       std::uint32_t opcode) {
    (void)linear_state;
    (void)opcode;
}

// Fiber/eval paths in aura_test_objects reference these AOT hooks
// (defined in aura_jit_bridge.cpp). Light bundles link runtime heaps
// but not the full bridge — provide no-op stubs so link succeeds.
extern "C" bool aura_aot_probe_checkpoint_version(std::uint64_t defuse_version,
                                                  std::uint64_t bridge_epoch) {
    (void)defuse_version;
    (void)bridge_epoch;
    return false; // no drift
}

extern "C" void aura_aot_record_deopt_on_steal(void) {
    // Stub: production increments AOT deopt metrics.
}
