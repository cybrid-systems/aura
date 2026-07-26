// render_strategy.hh — Issue #2138: evolvable present/draw strategy vs fixed
// native kernel.
//
// Agents hot-replace *policy* (mode, dirty-ratio threshold, skip-on-budget)
// without touching TermCell / DirtyRegion / ANSI / zero-copy / effect gate.
// Kernel resolves a cheap RenderStrategyView once per present (epoch cache).

#ifndef AURA_RENDERER_RENDER_STRATEGY_HH
#define AURA_RENDERER_RENDER_STRATEGY_HH

#include "compiler/frame_budget.hh"
#include "renderer/render_pass.hh"
#include "renderer/render_primitives.hh"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace aura::renderer::strategy {

inline constexpr int kRenderStrategyIssue = 2138;
inline constexpr int kRenderStrategyPhase = 1;

// Kernel-stable mode enum (layout-stable for metrics / Agent scripts).
enum class Mode : std::uint8_t {
    DirtyAABB = 0, // default: differential dirty present
    Full = 1,      // force mark_all_dirty then present
    Skip = 2,      // force skip this frame (preserve dirty)
    Auto = 3,      // policy: ratio / budget → Full | Skip | DirtyAABB
};

// Resolved view consulted by the present path (no Agent logic).
struct RenderStrategyView {
    Mode mode = Mode::DirtyAABB;
    std::uint64_t epoch = 0;
    std::uint32_t full_dirty_ratio_bp = 4000; // 0.4 default for Auto
    bool skip_when_budget_tight = true;       // Auto only
    char name[48]{"dirty-aabb"};
};

struct StrategyMetrics {
    std::atomic<std::uint64_t> resolve_total{0};
    std::atomic<std::uint64_t> mode_dirty_aabb_total{0};
    std::atomic<std::uint64_t> mode_full_total{0};
    std::atomic<std::uint64_t> mode_skip_total{0};
    std::atomic<std::uint64_t> mode_auto_total{0};
    std::atomic<std::uint64_t> auto_to_full_total{0};
    std::atomic<std::uint64_t> auto_to_skip_total{0};
    std::atomic<std::uint64_t> set_strategy_total{0};
    std::atomic<std::uint64_t> epoch{1};
    std::atomic<std::uint64_t> wired{1};
};

[[nodiscard]] inline StrategyMetrics& g_metrics() noexcept {
    static StrategyMetrics m;
    return m;
}

struct StrategyState {
    std::mutex mtx;
    Mode mode = Mode::DirtyAABB;
    std::uint32_t full_dirty_ratio_bp = 4000;
    bool skip_when_budget_tight = true;
    char name[48]{"dirty-aabb"};
    std::uint64_t epoch = 1;
    // Cached resolved view (invalidated on set).
    RenderStrategyView cached{};
    bool cache_valid = false;
};

[[nodiscard]] inline StrategyState& g_state() noexcept {
    static StrategyState s;
    return s;
}

[[nodiscard]] inline const char* mode_name(Mode m) noexcept {
    switch (m) {
        case Mode::DirtyAABB:
            return "dirty-aabb";
        case Mode::Full:
            return "full";
        case Mode::Skip:
            return "skip";
        case Mode::Auto:
            return "auto";
        default:
            return "unknown";
    }
}

[[nodiscard]] inline Mode mode_from_int(std::int64_t v) noexcept {
    if (v < 0 || v > 3)
        return Mode::DirtyAABB;
    return static_cast<Mode>(static_cast<std::uint8_t>(v));
}

[[nodiscard]] inline Mode mode_from_name(std::string_view s) noexcept {
    if (s == "full" || s == "Full" || s == "FULL")
        return Mode::Full;
    if (s == "skip" || s == "Skip" || s == "SKIP")
        return Mode::Skip;
    if (s == "auto" || s == "Auto" || s == "AUTO")
        return Mode::Auto;
    if (s == "dirty" || s == "dirty-aabb" || s == "DirtyAABB" || s == "aabb")
        return Mode::DirtyAABB;
    return Mode::DirtyAABB;
}

inline void copy_name(char (&dst)[48], std::string_view src) noexcept {
    const auto n = src.size() < 47 ? src.size() : 47;
    if (n)
        std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

// Agent / primitive: set strategy mode (+ optional name label). Bumps epoch.
inline void set_strategy(Mode mode, std::string_view label = {}) noexcept {
    auto& st = g_state();
    std::lock_guard<std::mutex> lock(st.mtx);
    st.mode = mode;
    const char* nm = label.empty() ? mode_name(mode) : nullptr;
    if (nm)
        copy_name(st.name, nm);
    else if (!label.empty())
        copy_name(st.name, label);
    ++st.epoch;
    if (st.epoch == 0)
        st.epoch = 1;
    st.cache_valid = false;
    g_metrics().epoch.store(st.epoch, std::memory_order_relaxed);
    g_metrics().set_strategy_total.fetch_add(1, std::memory_order_relaxed);
}

inline void set_auto_full_ratio_bp(std::uint32_t bp) noexcept {
    if (bp > 10000)
        bp = 10000;
    auto& st = g_state();
    std::lock_guard<std::mutex> lock(st.mtx);
    st.full_dirty_ratio_bp = bp;
    ++st.epoch;
    if (st.epoch == 0)
        st.epoch = 1;
    st.cache_valid = false;
    g_metrics().epoch.store(st.epoch, std::memory_order_relaxed);
    g_metrics().set_strategy_total.fetch_add(1, std::memory_order_relaxed);
}

inline void set_skip_when_budget_tight(bool on) noexcept {
    auto& st = g_state();
    std::lock_guard<std::mutex> lock(st.mtx);
    st.skip_when_budget_tight = on;
    ++st.epoch;
    if (st.epoch == 0)
        st.epoch = 1;
    st.cache_valid = false;
    g_metrics().epoch.store(st.epoch, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t strategy_epoch() noexcept {
    return g_metrics().epoch.load(std::memory_order_relaxed);
}

// Snapshot config (not yet resolved against this-frame dirty ratio).
[[nodiscard]] inline RenderStrategyView current_config() noexcept {
    auto& st = g_state();
    std::lock_guard<std::mutex> lock(st.mtx);
    RenderStrategyView v;
    v.mode = st.mode;
    v.epoch = st.epoch;
    v.full_dirty_ratio_bp = st.full_dirty_ratio_bp;
    v.skip_when_budget_tight = st.skip_when_budget_tight;
    std::memcpy(v.name, st.name, sizeof(v.name));
    return v;
}

// Resolve mode for this present (Auto → concrete mode using frame inputs).
[[nodiscard]] inline Mode resolve_mode(const RenderStrategyView& cfg, std::uint32_t dirty_cells,
                                       std::uint32_t total_cells) noexcept {
    auto& met = g_metrics();
    met.resolve_total.fetch_add(1, std::memory_order_relaxed);
    if (cfg.mode != Mode::Auto) {
        switch (cfg.mode) {
            case Mode::Full:
                met.mode_full_total.fetch_add(1, std::memory_order_relaxed);
                break;
            case Mode::Skip:
                met.mode_skip_total.fetch_add(1, std::memory_order_relaxed);
                break;
            case Mode::DirtyAABB:
            default:
                met.mode_dirty_aabb_total.fetch_add(1, std::memory_order_relaxed);
                break;
        }
        return cfg.mode;
    }
    met.mode_auto_total.fetch_add(1, std::memory_order_relaxed);
    // Budget tight → Skip (preserve dirty for next frame).
    if (cfg.skip_when_budget_tight && aura::compiler::frame_budget::past_deadline()) {
        met.auto_to_skip_total.fetch_add(1, std::memory_order_relaxed);
        met.mode_skip_total.fetch_add(1, std::memory_order_relaxed);
        return Mode::Skip;
    }
    // High dirty ratio → Full (one full frame, then clean slate).
    if (total_cells > 0) {
        const auto ratio_bp =
            static_cast<std::uint32_t>((static_cast<std::uint64_t>(dirty_cells) * 10000ull) /
                                       static_cast<std::uint64_t>(total_cells));
        if (ratio_bp >= cfg.full_dirty_ratio_bp) {
            met.auto_to_full_total.fetch_add(1, std::memory_order_relaxed);
            met.mode_full_total.fetch_add(1, std::memory_order_relaxed);
            return Mode::Full;
        }
    }
    met.mode_dirty_aabb_total.fetch_add(1, std::memory_order_relaxed);
    return Mode::DirtyAABB;
}

// Kernel present entry that consults strategy. Cell buffer / ANSI / zero-copy
// remain inside present_batch; this only chooses Full / Skip / DirtyAABB.
[[nodiscard]] inline std::int64_t present_batch_with_strategy(FramebufferSoA fb, DirtyRegion& dirty,
                                                              int fd) {
    const auto cfg = current_config();
    const auto total = static_cast<std::uint32_t>(fb.cell_count());
    const auto dcells = static_cast<std::uint32_t>(dirty.cell_count());
    const Mode m = resolve_mode(cfg, dcells, total);

    if (m == Mode::Skip) {
        // Force skip without clearing dirty (next present can still emit).
        return 0;
    }
    if (m == Mode::Full && fb.valid()) {
        dirty.mark_all_dirty(static_cast<std::uint32_t>(fb.width),
                             static_cast<std::uint32_t>(fb.height));
    }
    // DirtyAABB or Full after mark_all → existing kernel path.
    return present_batch(fb, dirty, fd);
}

struct Snapshot {
    std::uint64_t resolve_total = 0;
    std::uint64_t mode_dirty_aabb_total = 0;
    std::uint64_t mode_full_total = 0;
    std::uint64_t mode_skip_total = 0;
    std::uint64_t mode_auto_total = 0;
    std::uint64_t auto_to_full_total = 0;
    std::uint64_t auto_to_skip_total = 0;
    std::uint64_t set_strategy_total = 0;
    std::uint64_t epoch = 1;
    std::uint64_t mode = 0;
    std::uint64_t full_dirty_ratio_bp = 4000;
    std::uint64_t skip_when_budget_tight = 1;
    std::uint64_t wired = 1;
    int schema = kRenderStrategyIssue;
    char name[48]{"dirty-aabb"};
};

[[nodiscard]] inline Snapshot snapshot() noexcept {
    auto& m = g_metrics();
    Snapshot s;
    s.resolve_total = m.resolve_total.load(std::memory_order_relaxed);
    s.mode_dirty_aabb_total = m.mode_dirty_aabb_total.load(std::memory_order_relaxed);
    s.mode_full_total = m.mode_full_total.load(std::memory_order_relaxed);
    s.mode_skip_total = m.mode_skip_total.load(std::memory_order_relaxed);
    s.mode_auto_total = m.mode_auto_total.load(std::memory_order_relaxed);
    s.auto_to_full_total = m.auto_to_full_total.load(std::memory_order_relaxed);
    s.auto_to_skip_total = m.auto_to_skip_total.load(std::memory_order_relaxed);
    s.set_strategy_total = m.set_strategy_total.load(std::memory_order_relaxed);
    s.epoch = m.epoch.load(std::memory_order_relaxed);
    s.wired = m.wired.load(std::memory_order_relaxed);
    s.schema = kRenderStrategyIssue;
    {
        auto& st = g_state();
        std::lock_guard<std::mutex> lock(st.mtx);
        s.mode = static_cast<std::uint64_t>(st.mode);
        s.full_dirty_ratio_bp = st.full_dirty_ratio_bp;
        s.skip_when_budget_tight = st.skip_when_budget_tight ? 1 : 0;
        std::memcpy(s.name, st.name, sizeof(s.name));
    }
    return s;
}

inline void reset_for_test() noexcept {
    auto& m = g_metrics();
    m.resolve_total.store(0, std::memory_order_relaxed);
    m.mode_dirty_aabb_total.store(0, std::memory_order_relaxed);
    m.mode_full_total.store(0, std::memory_order_relaxed);
    m.mode_skip_total.store(0, std::memory_order_relaxed);
    m.mode_auto_total.store(0, std::memory_order_relaxed);
    m.auto_to_full_total.store(0, std::memory_order_relaxed);
    m.auto_to_skip_total.store(0, std::memory_order_relaxed);
    m.set_strategy_total.store(0, std::memory_order_relaxed);
    m.epoch.store(1, std::memory_order_relaxed);
    {
        auto& st = g_state();
        std::lock_guard<std::mutex> lock(st.mtx);
        st.mode = Mode::DirtyAABB;
        st.full_dirty_ratio_bp = 4000;
        st.skip_when_budget_tight = true;
        copy_name(st.name, "dirty-aabb");
        st.epoch = 1;
        st.cache_valid = false;
    }
}

} // namespace aura::renderer::strategy

#endif // AURA_RENDERER_RENDER_STRATEGY_HH
