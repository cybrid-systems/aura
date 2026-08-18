// Non-allocating arena hook thunks for tests (Issue #3124).
// Production hooks are function pointers + ctx — tests must not pass
// capturing lambdas.
#pragma once

#include <atomic>
#include <cstdint>
#include <unordered_map>

inline void arena_hook_compact_nop(void*) noexcept {}

inline void arena_hook_compact_bump_i32(void* ctx) noexcept {
    if (ctx)
        static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
}

inline void arena_hook_compact_bump_u64(void* ctx) noexcept {
    if (ctx)
        static_cast<std::atomic<std::uint64_t>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
}

inline void arena_hook_layout_nop(void*, std::uint64_t, std::uint64_t) noexcept {}

struct ArenaHookLayoutRecord {
    int fire_count = 0;
    std::uint64_t last_arena_id = 0;
    std::uint64_t last_new_gen = 0;
};

inline void arena_hook_layout_record(void* ctx, std::uint64_t arena_id,
                                     std::uint64_t new_gen) noexcept {
    if (!ctx)
        return;
    auto* r = static_cast<ArenaHookLayoutRecord*>(ctx);
    ++r->fire_count;
    r->last_arena_id = arena_id;
    r->last_new_gen = new_gen;
}

inline void arena_hook_root_remap_nop(void*, std::uint64_t, std::uint64_t,
                                      std::unordered_map<void*, void*> const&, std::size_t&,
                                      std::size_t&, std::size_t&, std::size_t&) noexcept {}

inline void arena_hook_root_remap_bump_i32(void* ctx, std::uint64_t, std::uint64_t,
                                           std::unordered_map<void*, void*> const&, std::size_t&,
                                           std::size_t&, std::size_t&, std::size_t&) noexcept {
    if (ctx)
        static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
}
