// render_primitives.cpp — Issues #1559/#1561/#1562/#2048: present/draw +
// dirty-delta + zero-copy LifetimePin handoff under render soft-gate.

#include "renderer/render_primitives.hh"

#include "core/arena_auto_policy_stats.h"
#include "core/gc_hooks.h"
#include "core/lifetime_pin.hh"
#include "core/zero_copy_output.hh"
#include "renderer/batch_terminal.hh"
#include "renderer/render_frame_arena.hh"

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

    // Issue #2048: pin zero-copy ANSI buffer for C handoff; arm GC defer so
    // compact cannot reclaim arena frame while write/backend is in flight.
    struct FfiPresentPinGuard {
        aura::core::lifetime::LifetimePin pin;
        bool armed = false;
        explicit FfiPresentPinGuard(void* p, std::size_t nbytes) noexcept {
            if (!p || nbytes == 0)
                return;
            // gen=1 is the present-frame stamp; arena_id=0 (frame bump arena).
            pin.pin(p, /*gen=*/1, /*arena_id=*/0);
            pin.mark_ffi_handoff();
            aura::gc_hooks::arm_ffi_pin_defer();
            armed = true;
            auto& zm = aura::core::zero_copy::g_zero_copy_metrics();
            zm.zero_copy_handoff_hits.fetch_add(1, std::memory_order_relaxed);
            zm.present_pin_handoffs.fetch_add(1, std::memory_order_relaxed);
            if (nbytes >= 4096)
                zm.zero_copy_large_handoff_hits.fetch_add(1, std::memory_order_relaxed);
        }
        ~FfiPresentPinGuard() noexcept {
            if (!armed)
                return;
            pin.unpin_on_compact();
            aura::gc_hooks::release_ffi_pin_defer();
            armed = false;
        }
        FfiPresentPinGuard(const FfiPresentPinGuard&) = delete;
        FfiPresentPinGuard& operator=(const FfiPresentPinGuard&) = delete;
        [[nodiscard]] bool valid() const noexcept { return pin.pinned() && pin.validate(1, 0); }
    };

    // Issue #2135: build dirty-aware ANSI **directly** into Arena-backed view
    // (preferred). Residual TLS scratch + single measured memcpy only on
    // capacity overflow / retry failure. Caps scratch growth after warm-up.
    // Out: sgr_emits, cells_emitted. Returns frame byte count.
    template <typename ArenaLike>
    std::size_t build_frame_zero_copy_arena(const FramebufferSoA& fb, DirtyRegion& dirty,
                                            ArenaLike& arena, std::uint64_t& sgr_emits,
                                            std::uint64_t& cells_emitted) {
        // Clamp dirty AABB into framebuffer.
        DirtyRegion region = dirty;
        if (!region.clamp_to(static_cast<std::uint32_t>(fb.width),
                             static_cast<std::uint32_t>(fb.height))) {
            sgr_emits = 0;
            cells_emitted = 0;
            return 0;
        }

        auto note_emit = [&](const DirtyFrameEmitResult& emit) {
            sgr_emits = emit.sgr_emits;
            cells_emitted = emit.cells_emitted;
            const std::uint64_t full_cells =
                static_cast<std::uint64_t>(fb.width) * static_cast<std::uint64_t>(fb.height);
            record_dirty_emit_sample(cells_emitted, full_cells);
            if (emit.partial) {
                ++g_render_hot_path_stats.dirty_partial_presents;
                g_dirty_delta_metrics().dirty_partial_presents.fetch_add(1,
                                                                         std::memory_order_relaxed);
            } else {
                g_dirty_delta_metrics().dirty_full_frame_presents.fetch_add(
                    1, std::memory_order_relaxed);
            }
            g_render_hot_path_stats.dirty_cells_emitted += cells_emitted;
        };

        auto& zc = aura::core::zero_copy::g_zero_copy_fb;
        auto& zm = aura::core::zero_copy::g_zero_copy_metrics();
        if constexpr (requires { arena.reset(); }) {
            arena.reset();
        }
        // Pre-warm arena capacity so allocate stays on arena path (no vector fallback).
        const std::size_t est = estimate_dirty_ansi_bytes(fb.width, fb.height, region);
        if constexpr (requires { arena.reserve(est); }) {
            arena.reserve(est * 2 + 64);
        }

        // ── Preferred: direct emit into arena view (zero residual memcpy) ──
        for (int attempt = 0; attempt < 2; ++attempt) {
            const std::size_t want = est * (attempt == 0 ? 2u : 4u) + 64u;
            auto view = zc.acquire_view(want, arena);
            ++g_render_hot_path_stats.zero_copy_acquire_total;
            ++g_engine_counters.zero_copy_acquires;
            if (!view.data() || view.size() < 64) {
                // OOM / reject — fall through to scratch path.
                zc.release_view(view, arena);
                break;
            }
            AnsiFixedBuf buf;
            buf.data = reinterpret_cast<char*>(view.data());
            buf.cap = view.size();
            buf.len = 0;
            buf.overflow = false;
            const auto emit =
                build_terminal_frame_ansi_dirty_buf(buf, fb.width, fb.height, fb.cells_c(), region);
            if (!buf.overflow) {
                // Trim last_view to exact length (no second copy).
                zc.last_size = buf.len;
                zc.last_ptr = view.data();
                zc.last_was_arena = true;
                note_emit(emit);
                zm.direct_arena_build_total.fetch_add(1, std::memory_order_relaxed);
                zc.release_view(view, arena);
                return buf.len;
            }
            zc.release_view(view, arena);
            // Retry with larger want; if second attempt overflows, scratch fallback.
            if constexpr (requires { arena.reset(); }) {
                arena.reset();
            }
            if constexpr (requires { arena.reserve(want * 2); }) {
                arena.reserve(want * 2);
            }
        }

        // ── Fallback: TLS scratch + single measured memcpy ──
        thread_local std::string ansi_scratch;
        ansi_scratch.clear();
        // Cap growth: reserve estimate once; never shrink mid-loop (warm-up).
        constexpr std::size_t kScratchCapMax = 4u * 1024u * 1024u; // 4 MiB hard cap
        const std::size_t want_scratch = std::min(est * 2 + 64u, kScratchCapMax);
        if (ansi_scratch.capacity() < want_scratch)
            ansi_scratch.reserve(want_scratch);
        zm.scratch_capacity_bytes.store(
            std::max(zm.scratch_capacity_bytes.load(std::memory_order_relaxed),
                     static_cast<std::uint64_t>(ansi_scratch.capacity())),
            std::memory_order_relaxed);

        const auto emit = build_terminal_frame_ansi_dirty(ansi_scratch, fb.width, fb.height,
                                                          fb.cells_c(), region);
        note_emit(emit);

        const std::size_t n = ansi_scratch.size();
        const std::size_t want = n > 0 ? n : 1;
        auto view = zc.acquire_view(want, arena);
        ++g_render_hot_path_stats.zero_copy_acquire_total;
        ++g_engine_counters.zero_copy_acquires;
        if (n > 0 && view.size() >= n) {
            std::memcpy(view.data(), ansi_scratch.data(), n);
            zm.residual_memcpy_count.fetch_add(1, std::memory_order_relaxed);
            zm.residual_memcpy_bytes.fetch_add(n, std::memory_order_relaxed);
        }
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
            // Issue #2047: clean present is free — short-circuit before hotpath.
            ++g_render_hot_path_stats.dirty_short_circuit_total;
            ++g_engine_counters.present_skips;
            g_dirty_delta_metrics().dirty_region_skips_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
            note_batch_terminal_short_circuit();
            aura::core::arena_policy::render_hotpath_skip_total.fetch_add(
                1, std::memory_order_relaxed);
            if (out_opt)
                out_opt->clear();
            return 0;
        }

        // Issue #2049: sample wall-time for frame-time histogram / p99 proxy.
        RenderFrameTimer frame_timer;
        HotpathGuard hotpath;

        // Prefer dedicated double-buffered RenderFrameArena when no external
        // arena is supplied (#2049); external arena_opt keeps legacy path.
        aura::core::zero_copy::FrameBumpArena* arena_ptr = arena_opt;
        RenderFrameArena* dedicated = nullptr;
        if (!arena_ptr) {
            dedicated = &g_render_frame_arena_v2();
            arena_ptr = &dedicated->current();
        }
        auto& arena = *arena_ptr;

        std::uint64_t sgr_emits = 0;
        std::uint64_t cells_emitted = 0;
        const std::size_t n =
            build_frame_zero_copy_arena(fb, dirty, arena, sgr_emits, cells_emitted);
        g_engine_counters.sgr_emits += sgr_emits;
        g_engine_counters.dirty_cells_emitted += cells_emitted;
        // Issue #2049: account frame scratch alloc (FrameBumpArena path).
        if (const auto used = arena.used_bytes(); used > 0)
            g_render_frame_metrics().render_alloc_bytes.fetch_add(used, std::memory_order_relaxed);
        // Issue #2047: record ANSI byte savings vs full-frame estimate.
        {
            const auto full_est = estimate_ansi_frame_bytes(fb.width, fb.height);
            const bool partial = cells_emitted < (static_cast<std::uint64_t>(fb.width) *
                                                  static_cast<std::uint64_t>(fb.height));
            note_batch_terminal_dirty_present(cells_emitted, n, full_est, partial);
            auto& dm = g_dirty_delta_metrics();
            dm.ansi_bytes_emitted_total.fetch_add(n, std::memory_order_relaxed);
            if (full_est > n)
                dm.ansi_bytes_saved_total.fetch_add(full_est - n, std::memory_order_relaxed);
        }

        auto& zc = aura::core::zero_copy::g_zero_copy_fb;
        const auto last = zc.last_view();
        const char* data = last.data() ? reinterpret_cast<const char*>(last.data()) : nullptr;

        std::int64_t written = 0;
        // Issue #2048 / #2049: pin + GC-defer only for the handoff window;
        // end_frame (double-buffer swap) runs after pin is released.
        {
            FfiPresentPinGuard handoff_pin(data ? const_cast<char*>(data) : nullptr, n);
            (void)handoff_pin.valid();

            if (out_opt) {
                if (data && n > 0)
                    out_opt->assign(data, n);
                else
                    out_opt->clear();
            }

            if (fd >= 0 && n > 0 && data) {
                const auto wn = ::write(fd, data, n);
                written = wn > 0 ? static_cast<std::int64_t>(wn) : 0;
            } else if (fd < 0) {
                written = static_cast<std::int64_t>(n);
            }
        } // pin released — buffer no longer needed for C handoff

        // Issue #2049: O(1) double-buffer swap for next frame.
        if (dedicated)
            dedicated->end_frame();

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
    reset_dirty_delta_metrics_for_test();
    reset_render_frame_metrics_for_test();
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
