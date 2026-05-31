// runtime_shared.h — Shared types for Aura runtime (JIT + evaluator + native)
// Used by aura_jit_runtime.cpp, evaluator_impl.cpp, runtime.c
#pragma once

#include <cstdint>
#include <cstddef>

// ── PairSlot: unified pair storage format ──
// Replaces separate g_pair_cars/g_pair_cdrs or evaluator's Pair struct.
// Phase 1: stores in vector<PairSlot>.
// Future: stores PairSlot* for arena/heap selection.
struct PairSlot {
    int64_t car;
    int64_t cdr;
};

// ── TL Arena (thread-local bump allocator) ──
struct TLarena {
    uint8_t* base = nullptr;
    size_t offset = 0;
    size_t capacity = 64 * 1024 * 1024;  // 64MB default
};

// Per-thread global arena instance
extern __thread TLarena g_tl_arena;

// ── Flags ──
extern bool g_use_arena;

// ── TL Arena API ──
void tl_arena_init(TLarena* arena);
void tl_arena_destroy(TLarena* arena);
void tl_arena_reset(TLarena* arena);
void* tl_arena_alloc(TLarena* arena, size_t size, size_t align);
void tl_arena_push(TLarena* arena);
void tl_arena_pop(TLarena* arena);
