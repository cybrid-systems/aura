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
extern "C" void aura_set_aot_metrics(void* metrics) {
    (void)metrics;
    // Stub.
}

extern "C" void aura_jit_epoch_acquire_fence(void) {
    std::atomic_thread_fence(std::memory_order_acquire);
}
