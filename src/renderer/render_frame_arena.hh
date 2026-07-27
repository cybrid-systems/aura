// render_frame_arena.hh — Issue #2049: dedicated render bump arena +
// linear-owned cell grids for predictable frame times.
//
// Complements zero_copy_output.hh FrameBumpArena with:
//   - Double-buffered RenderFrameArena (swap/reset is O(1) after warm-up)
//   - Process metrics: alloc bytes, reset total, frame-time p99 proxy
//   - LinearCellGrid: move-only cell storage for short-lived frame grids
//
// Contract:
//   - Never compacted mid-present (present holds render hotpath + pin-defer).
//   - end_frame() after handoff: switch buffer, reset next slot for reuse.
//   - General string_heap_ / ASTArena are not used for frame scratch.

#ifndef AURA_RENDERER_RENDER_FRAME_ARENA_HH
#define AURA_RENDERER_RENDER_FRAME_ARENA_HH

#include "core/zero_copy_output.hh"
#include "renderer/batch_terminal.hh"
#include "renderer/render_pass.hh"
#include "renderer/render_primitives.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace aura::renderer {

inline constexpr int kRenderFrameArenaPhase = 1;
inline constexpr int kRenderFrameArenaIssue = 2049;

// Process-wide render-arena observability (#2049 ACs).
struct RenderFrameArenaMetrics {
    std::atomic<std::uint64_t> render_alloc_bytes{0};
    std::atomic<std::uint64_t> render_arena_reset_total{0};
    std::atomic<std::uint64_t> render_arena_swap_total{0};
    std::atomic<std::uint64_t> render_frame_presents{0};
    std::atomic<std::uint64_t> render_frame_time_us_total{0};
    std::atomic<std::uint64_t> render_frame_time_samples{0};
    std::atomic<std::uint64_t> render_frame_time_max_us{0};
    std::atomic<std::uint64_t> linear_cell_grid_creates{0};
    std::atomic<std::uint64_t> linear_cell_grid_moves{0};
    static constexpr std::size_t kSampleCap = 64;
    std::uint64_t frame_us_samples[kSampleCap]{};
    std::atomic<std::uint64_t> sample_ix{0};
};

inline RenderFrameArenaMetrics& g_render_frame_metrics() noexcept {
    static RenderFrameArenaMetrics m;
    return m;
}

inline void reset_render_frame_metrics_for_test() noexcept {
    auto& m = g_render_frame_metrics();
    m.render_alloc_bytes.store(0, std::memory_order_relaxed);
    m.render_arena_reset_total.store(0, std::memory_order_relaxed);
    m.render_arena_swap_total.store(0, std::memory_order_relaxed);
    m.render_frame_presents.store(0, std::memory_order_relaxed);
    m.render_frame_time_us_total.store(0, std::memory_order_relaxed);
    m.render_frame_time_samples.store(0, std::memory_order_relaxed);
    m.render_frame_time_max_us.store(0, std::memory_order_relaxed);
    m.linear_cell_grid_creates.store(0, std::memory_order_relaxed);
    m.linear_cell_grid_moves.store(0, std::memory_order_relaxed);
    m.sample_ix.store(0, std::memory_order_relaxed);
    for (auto& s : m.frame_us_samples)
        s = 0;
}

inline void record_render_frame_time_us(std::uint64_t us) noexcept {
    auto& m = g_render_frame_metrics();
    m.render_frame_time_us_total.fetch_add(us, std::memory_order_relaxed);
    m.render_frame_time_samples.fetch_add(1, std::memory_order_relaxed);
    auto cur = m.render_frame_time_max_us.load(std::memory_order_relaxed);
    while (us > cur &&
           !m.render_frame_time_max_us.compare_exchange_weak(cur, us, std::memory_order_relaxed))
        ;
    const auto i =
        m.sample_ix.fetch_add(1, std::memory_order_relaxed) % RenderFrameArenaMetrics::kSampleCap;
    m.frame_us_samples[i] = us;
}

[[nodiscard]] inline std::uint64_t render_frame_time_p99_us() noexcept {
    auto& m = g_render_frame_metrics();
    const auto n =
        std::min<std::uint64_t>(m.render_frame_time_samples.load(std::memory_order_relaxed),
                                RenderFrameArenaMetrics::kSampleCap);
    if (n == 0)
        return 0;
    std::uint64_t tmp[RenderFrameArenaMetrics::kSampleCap];
    for (std::uint64_t i = 0; i < n; ++i)
        tmp[i] = m.frame_us_samples[i];
    std::sort(tmp, tmp + n);
    const auto idx = static_cast<std::size_t>((n * 99) / 100);
    return tmp[std::min(idx, static_cast<std::size_t>(n - 1))];
}

[[nodiscard]] inline std::uint64_t render_frame_time_avg_us() noexcept {
    auto& m = g_render_frame_metrics();
    const auto samples = m.render_frame_time_samples.load(std::memory_order_relaxed);
    if (samples == 0)
        return 0;
    return m.render_frame_time_us_total.load(std::memory_order_relaxed) / samples;
}

// Double-buffered bump arena: present allocates from `current()`; after
// handoff, end_frame() flips to the other buffer and resets it so the
// previous buffer remains intact until the subsequent swap (safe for
// any delayed C-backend readers within one frame).
struct RenderFrameArena {
    aura::core::zero_copy::FrameBumpArena buffers[2];
    int active = 0;

    [[nodiscard]] aura::core::zero_copy::FrameBumpArena& current() noexcept {
        return buffers[active];
    }
    [[nodiscard]] const aura::core::zero_copy::FrameBumpArena& current() const noexcept {
        return buffers[active];
    }

    [[nodiscard]] void* allocate_raw(std::size_t size, std::size_t alignment) {
        void* p = current().allocate_raw(size, alignment);
        if (p && size > 0)
            g_render_frame_metrics().render_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
        return p;
    }

    void reserve(std::size_t capacity) { current().reserve(capacity); }

    // O(1) after warm-up: flip buffer, reset the new active for next frame.
    void end_frame() noexcept {
        active ^= 1;
        buffers[active].reset();
        g_render_frame_metrics().render_arena_reset_total.fetch_add(1, std::memory_order_relaxed);
        g_render_frame_metrics().render_arena_swap_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Soft reset of current without swap (tests / explicit reclaim).
    void reset_current() noexcept {
        current().reset();
        g_render_frame_metrics().render_arena_reset_total.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t used_bytes() const noexcept { return current().used_bytes(); }
    [[nodiscard]] std::size_t capacity_bytes() const noexcept {
        return buffers[0].capacity_bytes() + buffers[1].capacity_bytes();
    }
};

// Process-local default double-buffered render arena (thread_local for fiber safety).
inline RenderFrameArena& g_render_frame_arena_v2() noexcept {
    static thread_local RenderFrameArena arena;
    return arena;
}

// Move-only cell grid for short-lived frame ownership (#2049 linear semantics).
// Long-lived terminal buffers remain in terminal_buffer_registry (shared).
struct LinearCellGrid {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<TermCell> cells;
    DirtyRegion dirty{};
    bool moved_from = false;

    LinearCellGrid() = default;

    explicit LinearCellGrid(std::int32_t w, std::int32_t h)
        : width(w)
        , height(h)
        , cells(static_cast<std::size_t>(std::max(w, 0) * std::max(h, 0)),
                TermCell::space_palette()) {
        dirty.mark_all_dirty(static_cast<std::uint32_t>(std::max(w, 0)),
                             static_cast<std::uint32_t>(std::max(h, 0)));
        g_render_frame_metrics().linear_cell_grid_creates.fetch_add(1, std::memory_order_relaxed);
    }

    LinearCellGrid(const LinearCellGrid&) = delete;
    LinearCellGrid& operator=(const LinearCellGrid&) = delete;

    LinearCellGrid(LinearCellGrid&& o) noexcept
        : width(o.width)
        , height(o.height)
        , cells(std::move(o.cells))
        , dirty(o.dirty)
        , moved_from(false) {
        o.width = 0;
        o.height = 0;
        o.dirty.clear();
        o.moved_from = true;
        g_render_frame_metrics().linear_cell_grid_moves.fetch_add(1, std::memory_order_relaxed);
    }

    LinearCellGrid& operator=(LinearCellGrid&& o) noexcept {
        if (this != &o) {
            width = o.width;
            height = o.height;
            cells = std::move(o.cells);
            dirty = o.dirty;
            moved_from = false;
            o.width = 0;
            o.height = 0;
            o.dirty.clear();
            o.moved_from = true;
            g_render_frame_metrics().linear_cell_grid_moves.fetch_add(1, std::memory_order_relaxed);
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept {
        return !moved_from && width > 0 && height > 0 &&
               cells.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }

    [[nodiscard]] FramebufferSoA view() noexcept {
        if (!valid())
            return {};
        return FramebufferSoA{width, height, cells.data()};
    }

    [[nodiscard]] FramebufferSoA view() const noexcept {
        if (!valid())
            return {};
        return FramebufferSoA{width, height, const_cast<TermCell*>(cells.data())};
    }

    // Present takes ownership of the dirty region (move semantics).
    DirtyRegion& dirty_mut() noexcept { return dirty; }

    // Issue #2214: process/thread active LinearCellGrid for tui:present-dirty.
    // When non-null and valid(), present-dirty prefers this buffer over TUIRuntime.
    // Tests / Agent glue set via set_active_linear_cell_grid; present clears dirty.
};

// Issue #2214: thread-local active LinearCellGrid pointer (non-owning).
inline LinearCellGrid*& g_active_linear_cell_grid_ptr() noexcept {
    static thread_local LinearCellGrid* p = nullptr;
    return p;
}
inline void set_active_linear_cell_grid(LinearCellGrid* g) noexcept {
    g_active_linear_cell_grid_ptr() = g;
}
[[nodiscard]] inline LinearCellGrid* active_linear_cell_grid() noexcept {
    return g_active_linear_cell_grid_ptr();
}

// RAII: sample present wall-time into the frame histogram.
struct RenderFrameTimer {
    using clock = std::chrono::steady_clock;
    clock::time_point t0 = clock::now();
    bool armed = true;

    ~RenderFrameTimer() {
        if (!armed)
            return;
        const auto us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0).count());
        record_render_frame_time_us(us);
        g_render_frame_metrics().render_frame_presents.fetch_add(1, std::memory_order_relaxed);
    }
    void cancel() noexcept { armed = false; }
};

} // namespace aura::renderer

#endif // AURA_RENDERER_RENDER_FRAME_ARENA_HH
