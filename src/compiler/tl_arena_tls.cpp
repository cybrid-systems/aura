// tl_arena_tls.cpp — SSOT definition of g_tl_arena (TLS bump allocator).
//
// Lives in libaura_tl_arena.so together with runtime_ssot.cpp (pair/hash
// globals) so libaura_test_objects.so and the light/full JIT SOs share one
// definition of every runtime data symbol. Previously g_tl_arena (and later
// g_pair_slots) lived only in aura_jit_runtime.cpp (JIT SOs); aura_test_objects
// had undefined data symbols that x86_64 glibc refuses to resolve from a
// later-loaded DSO at load time:
//   symbol lookup error: libaura_test_objects.so: undefined symbol: g_tl_arena
//   symbol lookup error: libaura_test_objects.so: undefined symbol: g_pair_slots
// (aarch64 often still resolved via the executable NEEDED list).
//
// Declaration: runtime_shared.h  extern __thread TLarena g_tl_arena;

#include "runtime_shared.h"

__thread TLarena g_tl_arena;
