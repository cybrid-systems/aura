// runtime_shared.h — Shared types for Aura runtime (JIT + evaluator + native)
// Used by aura_jit_runtime.cpp, evaluator partition TUs, runtime.c
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

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
    size_t capacity = 64 * 1024 * 1024; // 64MB default
};

// Per-thread global arena instance
extern __thread TLarena g_tl_arena;

// ── Shared pair storage (used by JIT runtime + IR interpreter) ──
// Stores pointers to pairs allocated on arena (non-escaping) or heap (escaping).
// Evaluator's car/cdr primitives check this as fallback.
extern std::vector<PairSlot*> g_pair_slots;
// Heap-owned pair slots (subset of g_pair_slots allocated via malloc).
// Process-exit cleanup frees them; see PairSlotCleanup in
// aura_jit_runtime.cpp. Without this, escape-analyzed pairs that fall
// back to heap allocation leak silently.
extern std::vector<PairSlot*> g_owned_pair_slots_;

// ── Flags ──
extern bool g_use_arena;

// ── FlatHashTable: contiguous hash table storage (Phase 4c) ──
// Single malloc block, layout (all offsets in bytes):
//   [0..8)       capacity   (uint64_t)
//   [8..16)      size       (uint64_t)
//   [16..16+cap) metadata   (uint8_t[capacity], 0xFF=empty)
//   [keys_offset...)       keys     (int64_t[capacity])
//   [values_offset...)     values   (int64_t[capacity])
struct FlatHashTable {
    uint64_t capacity;
    uint64_t size;
    // Data follows immediately after this struct header
    // metadata[0..capacity-1]
    // keys[0..capacity-1]
    // values[0..capacity-1]

    // Two uint64_t fields (capacity + size) = 16 bytes
    static constexpr uint64_t HEADER_SIZE = 16;

    static uint64_t total_bytes(uint64_t cap) {
        return HEADER_SIZE + cap * (1 + 8 + 8); // metadata + keys + values
    }

    uint8_t* metadata() { return reinterpret_cast<uint8_t*>(this) + HEADER_SIZE; }
    int64_t* keys() { return reinterpret_cast<int64_t*>(metadata() + capacity); }
    int64_t* values() { return reinterpret_cast<int64_t*>(keys() + capacity); }

    const uint8_t* metadata() const { return const_cast<FlatHashTable*>(this)->metadata(); }
    const int64_t* keys() const { return const_cast<FlatHashTable*>(this)->keys(); }
    const int64_t* values() const { return const_cast<FlatHashTable*>(this)->values(); }

    static FlatHashTable* create(uint64_t cap);
    static void destroy(FlatHashTable* ht);
    void rebuild(uint64_t new_cap); // rehash (grow/shrink)
};

extern std::vector<FlatHashTable*> g_hash_tables;

// ── TL Arena API ──
void tl_arena_init(TLarena* arena);
void tl_arena_destroy(TLarena* arena);
void tl_arena_reset(TLarena* arena);
void* tl_arena_alloc(TLarena* arena, size_t size, size_t align);
void tl_arena_push(TLarena* arena);
void tl_arena_pop(TLarena* arena);

// ── JIT / runtime C ABI (defined in aura_jit_runtime.cpp, aura_jit_bridge.cpp) ──
extern "C" std::int64_t aura_jit_test();
extern "C" const char* aura_jit_string_content(std::int64_t val);
extern "C" void aura_set_prim_dispatcher(std::int64_t (*fn)(std::int64_t, std::int64_t*,
                                                            std::int32_t));
extern "C" void aura_set_lock_hooks(void (*lock_read)(void*), void (*unlock_read)(void*),
                                    void (*lock_write)(void*), void (*unlock_write)(void*),
                                    std::uint64_t (*get_version)(void*),
                                    void (*yield_boundary)(void*), void* user_data);
extern "C" std::size_t aura_jit_pool_size();
extern "C" const char* aura_jit_pool_string(std::size_t idx);

// Issue #195: per-fiber exception state API (aura_jit_runtime.cpp).
extern "C" std::uint64_t aura_exception_depth();
extern "C" std::uint64_t aura_exception_fiber_count();
extern "C" void aura_exception_clear_all();
