// render_telemetry.hh — Issue #1357: per-prim latency + frame-time histogram.
// Header-only for global module fragment inclusion.

#ifndef AURA_COMPILER_RENDER_TELEMETRY_HH
#define AURA_COMPILER_RENDER_TELEMETRY_HH

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string_view>

namespace aura::compiler::render_telemetry {

inline constexpr int kPrimLatencyBuckets = 16;
inline constexpr int kFrameTimeBuckets = 12;

// Latency buckets (ns): 0-100, 100-200, 200-500, 500-1k, 1-2k, 2-5k,
// 5-10k, 10-20k, 20-50k, 50-100k, 100-200k, 200-500k, 500k-1M, 1-2M, 2-10M, 10M+
[[nodiscard]] inline int latency_bucket(std::uint64_t ns) noexcept {
    if (ns < 100)
        return 0;
    if (ns < 200)
        return 1;
    if (ns < 500)
        return 2;
    if (ns < 1'000)
        return 3;
    if (ns < 2'000)
        return 4;
    if (ns < 5'000)
        return 5;
    if (ns < 10'000)
        return 6;
    if (ns < 20'000)
        return 7;
    if (ns < 50'000)
        return 8;
    if (ns < 100'000)
        return 9;
    if (ns < 200'000)
        return 10;
    if (ns < 500'000)
        return 11;
    if (ns < 1'000'000)
        return 12;
    if (ns < 2'000'000)
        return 13;
    if (ns < 10'000'000)
        return 14;
    return 15;
}

// Frame time buckets (ns): 0-1ms … 15-16ms, 16-33ms, >33ms
[[nodiscard]] inline int frame_time_bucket(std::uint64_t ns) noexcept {
    constexpr std::uint64_t ms = 1'000'000ull;
    if (ns < 1 * ms)
        return 0;
    if (ns < 2 * ms)
        return 1;
    if (ns < 4 * ms)
        return 2;
    if (ns < 6 * ms)
        return 3;
    if (ns < 8 * ms)
        return 4;
    if (ns < 10 * ms)
        return 5;
    if (ns < 12 * ms)
        return 6;
    if (ns < 14 * ms)
        return 7;
    if (ns < 16 * ms)
        return 8;
    if (ns < 20 * ms)
        return 9;
    if (ns < 33 * ms)
        return 10;
    return 11;
}

struct PrimLatencyStats {
    std::atomic<std::uint64_t> call_count{0};
    std::atomic<std::uint64_t> total_ns{0};
    std::atomic<std::uint64_t> min_ns{std::numeric_limits<std::uint64_t>::max()};
    std::atomic<std::uint64_t> max_ns{0};
    std::array<std::atomic<std::uint64_t>, kPrimLatencyBuckets> bucket_counts{};

    void record(std::uint64_t ns) noexcept {
        call_count.fetch_add(1, std::memory_order_relaxed);
        total_ns.fetch_add(ns, std::memory_order_relaxed);
        auto prev_min = min_ns.load(std::memory_order_relaxed);
        while (ns < prev_min &&
               !min_ns.compare_exchange_weak(prev_min, ns, std::memory_order_relaxed)) {
        }
        auto prev_max = max_ns.load(std::memory_order_relaxed);
        while (ns > prev_max &&
               !max_ns.compare_exchange_weak(prev_max, ns, std::memory_order_relaxed)) {
        }
        const int b = latency_bucket(ns);
        bucket_counts[static_cast<std::size_t>(b)].fetch_add(1, std::memory_order_relaxed);
    }

    // Estimate p50 from buckets (midpoint of cumulative 50%).
    [[nodiscard]] std::uint64_t estimate_p50_ns() const noexcept {
        const auto total = call_count.load(std::memory_order_relaxed);
        if (total == 0)
            return 0;
        const auto target = (total + 1) / 2;
        std::uint64_t cum = 0;
        static constexpr std::uint64_t kMids[kPrimLatencyBuckets] = {
            50,     150,    350,     750,     1'500,   3'500,     7'500,     15'000,
            35'000, 75'000, 150'000, 350'000, 750'000, 1'500'000, 6'000'000, 15'000'000};
        for (int i = 0; i < kPrimLatencyBuckets; ++i) {
            cum += bucket_counts[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
            if (cum >= target)
                return kMids[i];
        }
        return kMids[kPrimLatencyBuckets - 1];
    }
};

struct FrameTimeStats {
    std::atomic<std::uint64_t> total_frames{0};
    std::atomic<std::uint64_t> total_ns{0};
    std::atomic<std::uint64_t> min_ns{std::numeric_limits<std::uint64_t>::max()};
    std::atomic<std::uint64_t> max_ns{0};
    std::array<std::atomic<std::uint64_t>, kFrameTimeBuckets> bucket_counts{};

    void record(std::uint64_t ns) noexcept {
        total_frames.fetch_add(1, std::memory_order_relaxed);
        total_ns.fetch_add(ns, std::memory_order_relaxed);
        auto prev_min = min_ns.load(std::memory_order_relaxed);
        while (ns < prev_min &&
               !min_ns.compare_exchange_weak(prev_min, ns, std::memory_order_relaxed)) {
        }
        auto prev_max = max_ns.load(std::memory_order_relaxed);
        while (ns > prev_max &&
               !max_ns.compare_exchange_weak(prev_max, ns, std::memory_order_relaxed)) {
        }
        const int b = frame_time_bucket(ns);
        bucket_counts[static_cast<std::size_t>(b)].fetch_add(1, std::memory_order_relaxed);
    }
};

// Process-wide frame stats (shared by all Evaluators for Agent queries).
inline FrameTimeStats& global_frame_time_stats() {
    static FrameTimeStats s;
    return s;
}

// Process-wide aggregate for named hot prims (slot tables are per-Evaluator).
// Used when query runs without full slot table export.
struct NamedPrimAgg {
    std::atomic<std::uint64_t> call_count{0};
    std::atomic<std::uint64_t> total_ns{0};
    std::atomic<std::uint64_t> min_ns{std::numeric_limits<std::uint64_t>::max()};
    std::atomic<std::uint64_t> max_ns{0};
};

// Fixed set of render-critical names for compact query (Phase 1).
inline constexpr const char* kTrackedRenderPrims[] = {
    "terminal-set-cell",
    "terminal-set-cell-rgb",
    "terminal-present-batch",
    "terminal-present",
    "make-terminal-buffer",
    "terminal-diff-update",
    "tui:cell",
    "tui:present",
    "tui:read-event",
    "+",
    "-",
    "*",
};

inline constexpr std::size_t kTrackedRenderPrimCount =
    sizeof(kTrackedRenderPrims) / sizeof(kTrackedRenderPrims[0]);

inline NamedPrimAgg& tracked_prim_stats(std::size_t i) {
    static NamedPrimAgg table[kTrackedRenderPrimCount];
    return table[i % kTrackedRenderPrimCount];
}

inline int tracked_prim_index(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kTrackedRenderPrimCount; ++i) {
        if (name == kTrackedRenderPrims[i])
            return static_cast<int>(i);
    }
    return -1;
}

inline std::atomic<std::uint64_t> g_render_prim_latency_samples{0};
inline std::atomic<std::uint64_t> g_render_prim_latency_total_ns{0};

inline void record_tracked_prim(std::string_view name, std::uint64_t ns) noexcept {
    g_render_prim_latency_samples.fetch_add(1, std::memory_order_relaxed);
    g_render_prim_latency_total_ns.fetch_add(ns, std::memory_order_relaxed);
    const int idx = tracked_prim_index(name);
    if (idx < 0)
        return;
    auto& s = tracked_prim_stats(static_cast<std::size_t>(idx));
    s.call_count.fetch_add(1, std::memory_order_relaxed);
    s.total_ns.fetch_add(ns, std::memory_order_relaxed);
    auto prev_min = s.min_ns.load(std::memory_order_relaxed);
    while (ns < prev_min &&
           !s.min_ns.compare_exchange_weak(prev_min, ns, std::memory_order_relaxed)) {
    }
    auto prev_max = s.max_ns.load(std::memory_order_relaxed);
    while (ns > prev_max &&
           !s.max_ns.compare_exchange_weak(prev_max, ns, std::memory_order_relaxed)) {
    }
}

} // namespace aura::compiler::render_telemetry

#endif // AURA_COMPILER_RENDER_TELEMETRY_HH
