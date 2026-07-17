// render_primitives.cpp — Issues #1559/#1561: present_batch / draw_batch engine.
// Dirty short-circuit, hotpath, Arena-backed zero-copy ANSI present.

#include "renderer/render_primitives.hh"

#include "core/arena_auto_policy_stats.h"
#include "core/zero_copy_output.hh"

#include <cstring>
#include <string>
#include <unistd.h>

namespace aura::renderer {
namespace {

    RenderEngineCounters g_engine_counters{};

    struct HotpathGuard {
        HotpathGuard() noexcept { aura::core::arena_policy::enter_render_hotpath(); }
        ~HotpathGuard() noexcept { aura::core::arena_policy::exit_render_hotpath(); }
        HotpathGuard(const HotpathGuard&) = delete;
        HotpathGuard& operator=(const HotpathGuard&) = delete;
    };

    // Build ANSI into reusable scratch, place final bytes in Arena-backed zero-copy view.
    // Returns frame byte count. Out: sgr_emits.
    // #1561: uses FrameBumpArena (or caller-supplied ArenaLike via present_batch_with_arena).
    template <typename ArenaLike>
    std::size_t build_frame_zero_copy_arena(const FramebufferSoA& fb, ArenaLike& arena,
                                            std::uint64_t& sgr_emits) {
        thread_local std::string ansi_scratch;
        ansi_scratch.clear();
        sgr_emits = build_terminal_frame_ansi(ansi_scratch, fb.width, fb.height, fb.cells_c());

        auto& zc = aura::core::zero_copy::g_zero_copy_fb;
        // Frame-scope reset so each present starts at bump base (capacity retained).
        if constexpr (requires { arena.reset(); }) {
            arena.reset();
        }
        const std::size_t n = ansi_scratch.size();
        const std::size_t want = n > 0 ? n : 1;
        auto view = zc.acquire_view(want, arena);
        ++g_render_hot_path_stats.zero_copy_acquire_total;
        ++g_engine_counters.zero_copy_acquires;
        if (n > 0 && view.size() >= n)
            std::memcpy(view.data(), ansi_scratch.data(), n);
        zc.release_view(view, arena);
        return n;
    }

    std::int64_t present_batch_impl(const FramebufferSoA& fb, DirtyRegion& dirty, int fd,
                                    std::string* out_opt,
                                    aura::core::zero_copy::FrameBumpArena* arena_opt) {
        ++g_render_hot_path_stats.present_batch_total;
        ++g_engine_counters.present_calls;

        if (!fb.valid())
            return -1;

        if (!dirty.is_dirty()) {
            ++g_render_hot_path_stats.dirty_short_circuit_total;
            ++g_engine_counters.present_skips;
            aura::core::arena_policy::render_hotpath_skip_total.fetch_add(
                1, std::memory_order_relaxed);
            if (out_opt)
                out_opt->clear();
            return 0;
        }

        HotpathGuard hotpath;

        auto& arena = arena_opt ? *arena_opt : aura::core::zero_copy::g_render_frame_arena();
        std::uint64_t sgr_emits = 0;
        const std::size_t n = build_frame_zero_copy_arena(fb, arena, sgr_emits);
        g_engine_counters.sgr_emits += sgr_emits;

        auto& zc = aura::core::zero_copy::g_zero_copy_fb;
        const auto last = zc.last_view();
        const char* data = last.data() ? reinterpret_cast<const char*>(last.data()) : nullptr;

        if (out_opt) {
            if (data && n > 0)
                out_opt->assign(data, n);
            else
                out_opt->clear();
        }

        std::int64_t written = 0;
        if (fd >= 0 && n > 0 && data) {
            const auto wn = ::write(fd, data, n);
            written = wn > 0 ? static_cast<std::int64_t>(wn) : 0;
        } else if (fd < 0) {
            written = static_cast<std::int64_t>(n);
        }

        dirty.clear();
        g_render_hot_path_stats.present_bytes_total +=
            static_cast<std::uint64_t>(written > 0 ? written : 0);
        g_engine_counters.present_bytes += static_cast<std::uint64_t>(written > 0 ? written : 0);
        aura::core::arena_policy::render_present_total.fetch_add(1, std::memory_order_relaxed);
        return written;
    }

} // namespace

RenderEngineCounters& render_engine_counters() noexcept {
    return g_engine_counters;
}

void reset_render_engine_counters_for_test() noexcept {
    g_engine_counters = {};
    g_render_hot_path_stats = {};
    aura::core::zero_copy::g_zero_copy_fb.acquire_count = 0;
    aura::core::zero_copy::g_zero_copy_fb.release_count = 0;
    aura::core::zero_copy::reset_zero_copy_metrics_for_test();
}

std::int64_t present_batch(const FramebufferSoA& fb, DirtyRegion& dirty, int fd) {
    return present_batch_impl(fb, dirty, fd, nullptr, nullptr);
}

std::int64_t present_batch_to_string(const FramebufferSoA& fb, DirtyRegion& dirty,
                                     std::string& out) {
    return present_batch_impl(fb, dirty, /*fd=*/-1, &out, nullptr);
}

std::int64_t present_batch_with_arena(const FramebufferSoA& fb, DirtyRegion& dirty,
                                      aura::core::zero_copy::FrameBumpArena& arena, int fd) {
    return present_batch_impl(fb, dirty, fd, nullptr, &arena);
}

std::int64_t draw_batch(FramebufferSoA& fb, DirtyRegion& dirty, std::span<const DrawOp> ops) {
    ++g_render_hot_path_stats.draw_batch_total;
    ++g_engine_counters.draw_calls;
    if (!fb.valid() || ops.empty())
        return 0;

    HotpathGuard hotpath;
    std::int64_t written = 0;
    const auto w = static_cast<std::uint32_t>(fb.width);
    const auto h = static_cast<std::uint32_t>(fb.height);
    for (const auto& op : ops) {
        if (op.x >= w || op.y >= h)
            continue;
        const auto idx = static_cast<std::size_t>(op.y) * static_cast<std::size_t>(w) + op.x;
        fb.cells[idx] = op.cell;
        dirty.mark_dirty(op.x, op.y);
        ++written;
    }
    g_engine_counters.draw_cells += static_cast<std::uint64_t>(written);
    aura::core::arena_policy::render_draw_batch_total.fetch_add(1, std::memory_order_relaxed);
    return written;
}

} // namespace aura::renderer
