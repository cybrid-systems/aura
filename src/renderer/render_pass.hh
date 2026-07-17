// render_pass.hh — Issues #1179/#1186/#1559/#1562: DirtyAware render short-circuit + delta.
// Keep in sync with render_pass.ixx for module consumers.

#ifndef AURA_RENDERER_RENDER_PASS_HH
#define AURA_RENDERER_RENDER_PASS_HH

#include "core/arena_auto_policy_stats.h"

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace aura::renderer {

inline constexpr int kRenderPassPhase = 2; // #1562 dirty-region delta
inline constexpr int kRenderPassIssue = 1562;

// Framebuffer AABB dirty region (cell coordinates, inclusive).
struct DirtyRegion {
    bool clean = true;
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    // True after clear() until first mark_dirty / mark_all_dirty (disambiguates AABB seed).
    bool empty_aabb = true;

    [[nodiscard]] bool is_clean() const noexcept { return clean; }
    [[nodiscard]] bool is_dirty() const noexcept { return !clean; }

    void mark_dirty(std::uint32_t x, std::uint32_t y) noexcept {
        clean = false;
        if (empty_aabb) {
            x0 = x1 = x;
            y0 = y1 = y;
            empty_aabb = false;
            return;
        }
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }

    void mark_all_dirty(std::uint32_t width, std::uint32_t height) noexcept {
        clean = false;
        empty_aabb = false;
        if (width == 0 || height == 0) {
            x0 = y0 = x1 = y1 = 0;
            return;
        }
        x0 = 0;
        y0 = 0;
        x1 = width - 1;
        y1 = height - 1;
    }

    void mark_row_dirty(std::uint32_t y, std::uint32_t width) noexcept {
        if (width == 0)
            return;
        mark_dirty(0, y);
        mark_dirty(width - 1, y);
    }

    void clear() noexcept {
        clean = true;
        empty_aabb = true;
        x0 = y0 = x1 = y1 = 0;
    }

    // Clamp AABB into framebuffer [0,w) x [0,h). Returns false if empty after clamp.
    [[nodiscard]] bool clamp_to(std::uint32_t w, std::uint32_t h) noexcept {
        if (clean || empty_aabb || w == 0 || h == 0)
            return false;
        if (x0 >= w || y0 >= h)
            return false;
        x1 = std::min(x1, w - 1);
        y1 = std::min(y1, h - 1);
        if (x0 > x1 || y0 > y1)
            return false;
        return true;
    }

    // Inclusive cell count in AABB (0 if clean).
    [[nodiscard]] std::uint64_t cell_count() const noexcept {
        if (clean || empty_aabb)
            return 0;
        if (x1 < x0 || y1 < y0)
            return 0;
        return static_cast<std::uint64_t>(x1 - x0 + 1) * static_cast<std::uint64_t>(y1 - y0 + 1);
    }

    [[nodiscard]] std::uint32_t width() const noexcept {
        return clean || empty_aabb || x1 < x0 ? 0 : (x1 - x0 + 1);
    }
    [[nodiscard]] std::uint32_t height() const noexcept {
        return clean || empty_aabb || y1 < y0 ? 0 : (y1 - y0 + 1);
    }

    // True when AABB covers the full framebuffer.
    [[nodiscard]] bool is_full_frame(std::uint32_t w, std::uint32_t h) const noexcept {
        if (clean || empty_aabb || w == 0 || h == 0)
            return false;
        return x0 == 0 && y0 == 0 && x1 + 1 >= w && y1 + 1 >= h;
    }
};

// #1562 dirty-delta present metrics (process-wide).
struct DirtyDeltaMetrics {
    std::atomic<std::uint64_t> dirty_region_skips_total{0}; // clean short-circuit
    std::atomic<std::uint64_t> dirty_cells_emitted_total{0};
    std::atomic<std::uint64_t> dirty_cells_skipped_total{0}; // full - dirty per frame
    std::atomic<std::uint64_t> dirty_present_frames{0};
    std::atomic<std::uint64_t> dirty_full_frame_presents{0};
    std::atomic<std::uint64_t> dirty_partial_presents{0};
    std::atomic<std::uint64_t> dirty_cells_max{0}; // high-water (p99 proxy)
    static constexpr std::size_t kSampleCap = 64;
    std::uint64_t samples[kSampleCap]{};
    std::atomic<std::uint64_t> sample_ix{0};
    std::atomic<std::uint64_t> sample_count{0};
};

inline DirtyDeltaMetrics& g_dirty_delta_metrics() noexcept {
    static DirtyDeltaMetrics m;
    return m;
}

inline void record_dirty_emit_sample(std::uint64_t dirty_cells, std::uint64_t full_cells) noexcept {
    auto& m = g_dirty_delta_metrics();
    m.dirty_present_frames.fetch_add(1, std::memory_order_relaxed);
    m.dirty_cells_emitted_total.fetch_add(dirty_cells, std::memory_order_relaxed);
    if (full_cells > dirty_cells)
        m.dirty_cells_skipped_total.fetch_add(full_cells - dirty_cells, std::memory_order_relaxed);
    auto cur_max = m.dirty_cells_max.load(std::memory_order_relaxed);
    while (dirty_cells > cur_max && !m.dirty_cells_max.compare_exchange_weak(
                                        cur_max, dirty_cells, std::memory_order_relaxed))
        ;
    const auto i =
        m.sample_ix.fetch_add(1, std::memory_order_relaxed) % DirtyDeltaMetrics::kSampleCap;
    m.samples[i] = dirty_cells;
    m.sample_count.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline double dirty_cells_avg() noexcept {
    auto& m = g_dirty_delta_metrics();
    const auto frames = m.dirty_present_frames.load(std::memory_order_relaxed);
    if (frames == 0)
        return 0.0;
    return static_cast<double>(m.dirty_cells_emitted_total.load(std::memory_order_relaxed)) /
           static_cast<double>(frames);
}

[[nodiscard]] inline std::uint64_t dirty_cells_p99() noexcept {
    auto& m = g_dirty_delta_metrics();
    const auto n = std::min<std::uint64_t>(m.sample_count.load(std::memory_order_relaxed),
                                           DirtyDeltaMetrics::kSampleCap);
    if (n == 0)
        return 0;
    std::uint64_t tmp[DirtyDeltaMetrics::kSampleCap];
    for (std::uint64_t i = 0; i < n; ++i)
        tmp[i] = m.samples[i];
    std::sort(tmp, tmp + n);
    const auto idx = static_cast<std::size_t>((n * 99) / 100);
    return tmp[std::min(idx, static_cast<std::size_t>(n - 1))];
}

// Skip rate: fraction of cells not emitted over all presents (0..1).
[[nodiscard]] inline double dirty_cell_skip_rate() noexcept {
    auto& m = g_dirty_delta_metrics();
    const auto emitted = m.dirty_cells_emitted_total.load(std::memory_order_relaxed);
    const auto skipped = m.dirty_cells_skipped_total.load(std::memory_order_relaxed);
    const auto total = emitted + skipped;
    if (total == 0)
        return 0.0;
    return static_cast<double>(skipped) / static_cast<double>(total);
}

inline void reset_dirty_delta_metrics_for_test() noexcept {
    auto& m = g_dirty_delta_metrics();
    m.dirty_region_skips_total.store(0, std::memory_order_relaxed);
    m.dirty_cells_emitted_total.store(0, std::memory_order_relaxed);
    m.dirty_cells_skipped_total.store(0, std::memory_order_relaxed);
    m.dirty_present_frames.store(0, std::memory_order_relaxed);
    m.dirty_full_frame_presents.store(0, std::memory_order_relaxed);
    m.dirty_partial_presents.store(0, std::memory_order_relaxed);
    m.dirty_cells_max.store(0, std::memory_order_relaxed);
    m.sample_ix.store(0, std::memory_order_relaxed);
    m.sample_count.store(0, std::memory_order_relaxed);
    for (auto& s : m.samples)
        s = 0;
}

struct RenderHotPathStats {
    std::uint64_t dirty_short_circuit_total = 0;
    std::uint64_t shape_stable_hit_total = 0;
    std::uint64_t present_batch_total = 0;
    std::uint64_t draw_batch_total = 0;
    std::uint64_t present_bytes_total = 0;
    std::uint64_t zero_copy_acquire_total = 0;
    std::uint64_t dirty_cells_emitted = 0;
    std::uint64_t dirty_partial_presents = 0;
};

inline RenderHotPathStats g_render_hot_path_stats{};
inline DirtyRegion g_framebuffer_dirty{};

// Phase 1 present gate: short-circuit when dirty region is clean.
inline bool present_batch_if_dirty() {
    ++g_render_hot_path_stats.present_batch_total;
    if (g_framebuffer_dirty.is_clean()) {
        ++g_render_hot_path_stats.dirty_short_circuit_total;
        g_dirty_delta_metrics().dirty_region_skips_total.fetch_add(1, std::memory_order_relaxed);
        return false; // skipped
    }
    return true; // would present
}

// #1562 AC6: lightweight render mutation checkpoint RAII (sibling to #1355).
// Scopes dirty marks under render hotpath for sparse TUI mutations.
struct RenderDirtyMutationGuard {
    DirtyRegion* dirty = nullptr;
    bool entered_hotpath = false;
    std::uint32_t marks = 0;

    explicit RenderDirtyMutationGuard(DirtyRegion& d, bool enter_hotpath = true) noexcept
        : dirty(&d) {
        if (enter_hotpath && !aura::core::arena_policy::in_render_hotpath()) {
            aura::core::arena_policy::enter_render_hotpath();
            entered_hotpath = true;
        }
    }
    ~RenderDirtyMutationGuard() noexcept {
        if (entered_hotpath)
            aura::core::arena_policy::exit_render_hotpath();
    }
    RenderDirtyMutationGuard(const RenderDirtyMutationGuard&) = delete;
    RenderDirtyMutationGuard& operator=(const RenderDirtyMutationGuard&) = delete;

    void mark(std::uint32_t x, std::uint32_t y) noexcept {
        if (dirty) {
            dirty->mark_dirty(x, y);
            ++marks;
        }
    }
};

} // namespace aura::renderer

#endif // AURA_RENDERER_RENDER_PASS_HH
