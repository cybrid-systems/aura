// tl_arena_tls.cpp — single SSOT definition of g_tl_arena (TLS bump allocator).
//
// Lives in its own shared object so libaura_test_objects.so (evaluator /
// ir_executor refs) and the light/full JIT SOs share one TLS instance.
// Previously the definition lived only in aura_jit_runtime.cpp (JIT SOs);
// aura_test_objects had an undefined TLS symbol that x86_64 glibc refuses
// to resolve from a later-loaded DSO at load time:
//   symbol lookup error: libaura_test_objects.so: undefined symbol: g_tl_arena
// (aarch64 often still resolved via the executable NEEDED list).
//
// Declaration: runtime_shared.h  extern __thread TLarena g_tl_arena;

#include "runtime_shared.h"

__thread TLarena g_tl_arena;
