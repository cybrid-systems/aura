// runtime_bridge_stub.cpp — weak bridge-hook stubs compiled into
// libaura_test_objects.so itself.
//
// Same class of bug as g_current_fiber_id_fn (fixed in runtime_ssot.cpp,
// 1ca6c8bc): libaura_test_objects.so's TUs (service.ixx, evaluator_ctor.cpp,
// scheduler.cpp, macro_expansion.cpp, ...) reference JIT-lib symbols
// (AuraJIT ctor, aura_cleanup_aot_state, aura_set_long_mutation_scheduler_hook,
// aura_macro_provenance_repin_on_steal). The strong definitions live in the
// JIT SOs (aura_jit.cpp / aura_jit_bridge.cpp) which libaura_test_objects.so
// does NOT NEEDED-link. On x86_64 CI, mold --as-needed strips the JIT lib
// from test_issue_* binaries that don't directly reference it, so the loader
// fails at load time:
//   symbol lookup error: libaura_test_objects.so: undefined symbol: _ZN4aura3jit7AuraJITC1Ev
// (rc=127; local arm64 lld keeps DT_NEEDED so it passes locally).
//
// These weak stubs live in the same .so that references the symbols, so the
// loader always resolves them. When a real JIT lib IS linked (full / light
// JIT tests), its strong definitions interpose the weak stubs and real
// behavior is preserved. Mirrors the existing weak stubs in
// aura_jit_bridge_stub.cpp (which is compiled into the JIT libs, not here).

#include "aura_jit_bridge.h"
#include "aura_jit.h"

#include <cstdint>

extern "C" __attribute__((weak)) void aura_cleanup_aot_state(void* /*eval*/) {}

extern "C" __attribute__((weak)) void
aura_set_long_mutation_scheduler_hook(aura_long_mutation_scheduler_hook_fn /*fn*/) {}

extern "C" __attribute__((weak)) int
aura_macro_provenance_repin_on_steal(void* /*ev_ptr*/, std::uint64_t /*cloned_marker*/) {
    return 0;
}

// Weak AuraJIT stub ctor (+ companion symbols) so service.ixx's jit_ value
// member links when no JIT lib is present. Strong definitions in
// aura_jit.cpp interpose when a JIT lib is linked.
namespace aura::jit {

struct AuraJIT::Impl {};

__attribute__((weak)) AuraJIT::AuraJIT()
    : impl_(nullptr) {}
__attribute__((weak)) AuraJIT::~AuraJIT() = default;
__attribute__((weak)) bool AuraJIT::available() const {
    return false;
}

} // namespace aura::jit
