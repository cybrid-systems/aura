// zero_copy_output.hh — Issues #1178/#1559/#1561: Arena-backed true zero-copy views.
// Header form for renderer TUs + unit tests. Keep in sync with zero_copy_output.ixx.

#ifndef AURA_CORE_ZERO_COPY_OUTPUT_HH
#define AURA_CORE_ZERO_COPY_OUTPUT_HH

#include "core/arena_auto_policy_stats.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aura::core::zero_copy {

inline constexpr int kZeroCopyOutputPhase = 2; // #1561: Arena path
inline constexpr int kZeroCopyOutputIssue = 1561;

// Process-wide metrics (#1561 AC4).
struct ZeroCopyMetrics {
    std::atomic<std::uint64_t> acquire_count{0};
    std::atomic<std::uint64_t> release_count{0};
    std::atomic<std::uint64_t> arena_acquire_count{0};
    std::atomic<std::uint64_t> arena_alloc_bytes{0}; // zero_copy_arena_alloc_bytes
    std::atomic<std::uint64_t> hit_in_render{0};     // zero_copy_hit_in_render
    std::atomic<std::uint64_t> vector_fallback_count{0};
    std::atomic<std::uint64_t> arena_path_active{1};
};

inline ZeroCopyMetrics& g_zero_copy_metrics() noexcept {
    static ZeroCopyMetrics m;
    return m;
}

// Frame-scope monotonic bump allocator (Arena-like allocate_raw API).
// Default present_batch temp arena when no external ASTArena is passed.
// release is a no-op; reset() reclaims the whole frame for the next present.
struct FrameBumpArena {
    std::vector<std::byte> block;
    std::size_t used = 0;
    std::uint64_t alloc_calls = 0;
    std::uint64_t total_bytes = 0;

    void reserve(std::size_t capacity) {
        if (block.size() < capacity)
            block.resize(capacity);
    }

    // Same contract as ASTArena::allocate_raw (size>0, power-of-two align).
    [[nodiscard]] void* allocate_raw(std::size_t size, std::size_t alignment) {
        if (size == 0)
            return nullptr;
        if (alignment == 0 || (alignment & (alignment - 1)) != 0)
            alignment = alignof(std::max_align_t);
        const std::size_t base = used;
        const std::size_t aligned = (base + alignment - 1) & ~(alignment - 1);
        const std::size_t end = aligned + size;
        if (end > block.size()) {
            // Grow capacity (cold path / first frames only).
            const std::size_t grow =
                block.empty() ? std::max(end, size * 2) : std::max(end, block.size() * 2);
            block.resize(grow);
        }
        used = end;
        ++alloc_calls;
        total_bytes += size;
        return block.data() + aligned;
    }

    void reset() noexcept { used = 0; }

    [[nodiscard]] std::size_t used_bytes() const noexcept { return used; }
    [[nodiscard]] std::size_t capacity_bytes() const noexcept { return block.size(); }
};

// Lightweight view holder with vector pool + Arena-backed acquire (#1561).
struct ZeroCopyFramebuffer {
    std::vector<std::byte> storage; // legacy / fallback path
    std::uint64_t acquire_count = 0;
    std::uint64_t release_count = 0;
    // Last view (valid until arena reset / next acquire); for present write path.
    std::byte* last_ptr = nullptr;
    std::size_t last_size = 0;
    bool last_was_arena = false;

    // Legacy path: grow process-local vector (stable after warm-up).
    std::span<std::byte> acquire_view(std::size_t size) {
        if (size == 0)
            size = 1;
        if (storage.size() < size)
            storage.resize(size);
        ++acquire_count;
        auto& m = g_zero_copy_metrics();
        m.acquire_count.fetch_add(1, std::memory_order_relaxed);
        m.vector_fallback_count.fetch_add(1, std::memory_order_relaxed);
        if (aura::core::arena_policy::in_render_hotpath())
            m.hit_in_render.fetch_add(1, std::memory_order_relaxed);
        last_was_arena = false;
        last_ptr = storage.data();
        last_size = size;
        return {storage.data(), size};
    }

    // AC1: Arena-backed acquire — template works with FrameBumpArena
    // (allocate_raw) and ASTArena (public try_allocate / allocate_checked;
    // allocate_raw is private).
    template <typename ArenaLike>
    std::span<std::byte> acquire_view(std::size_t size, ArenaLike& arena) {
        if (size == 0)
            size = 1;
        void* p = nullptr;
        if constexpr (requires { arena.try_allocate(size); }) {
            // ASTArena public seam (wraps allocate_raw).
            p = arena.try_allocate(size);
        } else if constexpr (requires { arena.allocate_raw(size, alignof(std::byte)); }) {
            // FrameBumpArena / free-standing Arena-like.
            p = arena.allocate_raw(size, alignof(std::byte));
        } else {
            return acquire_view(size);
        }
        if (!p) {
            // Quota reject / OOM → fall back to vector pool.
            return acquire_view(size);
        }
        ++acquire_count;
        auto& m = g_zero_copy_metrics();
        m.acquire_count.fetch_add(1, std::memory_order_relaxed);
        m.arena_acquire_count.fetch_add(1, std::memory_order_relaxed);
        m.arena_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
        m.arena_path_active.store(1, std::memory_order_relaxed);
        if (aura::core::arena_policy::in_render_hotpath())
            m.hit_in_render.fetch_add(1, std::memory_order_relaxed);
        last_was_arena = true;
        last_ptr = static_cast<std::byte*>(p);
        last_size = size;
        return {static_cast<std::byte*>(p), size};
    }

    // AC2: arena-aware release — no-op for monotonic frame arenas.
    void release_view(std::span<std::byte> /*v*/) noexcept {
        ++release_count;
        g_zero_copy_metrics().release_count.fetch_add(1, std::memory_order_relaxed);
    }

    template <typename ArenaLike>
    void release_view(std::span<std::byte> /*v*/, ArenaLike& /*arena*/) noexcept {
        ++release_count;
        g_zero_copy_metrics().release_count.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::span<const std::byte> last_view() const noexcept {
        if (!last_ptr || last_size == 0)
            return {};
        return {last_ptr, last_size};
    }
};

inline ZeroCopyFramebuffer g_zero_copy_fb{};

// Process-local default frame arena for present_batch (thread_local for fiber safety).
inline FrameBumpArena& g_render_frame_arena() noexcept {
    static thread_local FrameBumpArena arena;
    return arena;
}

inline void reset_zero_copy_metrics_for_test() noexcept {
    auto& m = g_zero_copy_metrics();
    m.acquire_count.store(0, std::memory_order_relaxed);
    m.release_count.store(0, std::memory_order_relaxed);
    m.arena_acquire_count.store(0, std::memory_order_relaxed);
    m.arena_alloc_bytes.store(0, std::memory_order_relaxed);
    m.hit_in_render.store(0, std::memory_order_relaxed);
    m.vector_fallback_count.store(0, std::memory_order_relaxed);
    m.arena_path_active.store(1, std::memory_order_relaxed);
    g_zero_copy_fb.acquire_count = 0;
    g_zero_copy_fb.release_count = 0;
    g_zero_copy_fb.last_ptr = nullptr;
    g_zero_copy_fb.last_size = 0;
    g_render_frame_arena().reset();
    g_render_frame_arena().block.clear();
    g_render_frame_arena().alloc_calls = 0;
    g_render_frame_arena().total_bytes = 0;
}

struct ZeroCopyStatsSnapshot {
    std::uint64_t acquire_count = 0;
    std::uint64_t release_count = 0;
    std::uint64_t arena_acquire_count = 0;
    std::uint64_t arena_alloc_bytes = 0;
    std::uint64_t hit_in_render = 0;
    std::uint64_t vector_fallback_count = 0;
    std::uint64_t arena_path_active = 1;
    std::uint64_t zero_copy_supported = 1;
    int phase = kZeroCopyOutputPhase;
};

[[nodiscard]] inline ZeroCopyStatsSnapshot snapshot_zero_copy_stats() noexcept {
    auto& m = g_zero_copy_metrics();
    const auto active = m.arena_path_active.load(std::memory_order_relaxed);
    return ZeroCopyStatsSnapshot{
        m.acquire_count.load(std::memory_order_relaxed),
        m.release_count.load(std::memory_order_relaxed),
        m.arena_acquire_count.load(std::memory_order_relaxed),
        m.arena_alloc_bytes.load(std::memory_order_relaxed),
        m.hit_in_render.load(std::memory_order_relaxed),
        m.vector_fallback_count.load(std::memory_order_relaxed),
        active,
        active ? 1ull : 0ull,
        kZeroCopyOutputPhase,
    };
}

} // namespace aura::core::zero_copy

#endif // AURA_CORE_ZERO_COPY_OUTPUT_HH
