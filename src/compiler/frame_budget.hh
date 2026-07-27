// frame_budget.hh — Issue #2137 cascade isolation + #2218 present guardian.
//
// #2137: While present / render hotpath is active, non-render dirty cascade
// work (mark_define_dirty fan-out, hybrid NodeId cascade, HotUpdate reemit)
// is deferred or coalesced so frame p99 stays inside the budget.
// Render-related defines may still cascade. Deferred names drain when the
// hotpath exits or on the next soft-dirty outside the hotpath.
//
// #2218: Execution-layer guardian on tui:present / tui:present-dirty:
// if recent frame-time p99 exceeds budget, or cached agent-health is low,
// or last agent-action is hold/stop — prefer dirty short-circuit / skip
// full rebuild (keep last good frame). Authoritative for the current frame;
// forces agent-action hold/stop for the next mutate window. Zero extra cost
// under budget beyond a few atomic loads.
//
// Env: AURA_FRAME_BUDGET_US (default 16000 µs ≈ 60 fps; 33000 for 30 fps).

#ifndef AURA_COMPILER_FRAME_BUDGET_HH
#define AURA_COMPILER_FRAME_BUDGET_HH

#include "render_prim_template.hh" // aura_is_render_evolution_name

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace aura::compiler::frame_budget {

inline constexpr int kFrameBudgetIssue = 2137;
inline constexpr int kPresentGuardianIssue = 2218;
// ~60 fps default (microseconds). AC #2218: 16000; was 16667 for #2137.
inline constexpr std::uint64_t kDefaultBudgetUs = 16000;
// Soft warn when p99 ≥ this fraction of budget (basis points; 8000 = 80%).
inline constexpr std::uint64_t kSoftMarginBp = 8000;
// Agent health below this triggers degrade (matches schema-2051 safe-to-mutate).
inline constexpr std::int64_t kHealthDegradeThreshold = 60;
// Dirty cells below this fraction of the buffer are "tiny" → skip full rebuild.
inline constexpr std::uint64_t kTinyDirtyBp = 500; // 5%

// agent-action codes (schema-2051): 0=hold 1=optimize-ok 2=reduce
// 3=prefer-dirty-delta 4=stop-mutate
inline constexpr std::int64_t kActionHold = 0;
inline constexpr std::int64_t kActionOptimizeOk = 1;
inline constexpr std::int64_t kActionStop = 4;

// ── Metrics (process-wide; mirrored into CompilerMetrics / query surfaces) ──
struct FrameBudgetMetrics {
    std::atomic<std::uint64_t> deferred_cascade_total{0};
    std::atomic<std::uint64_t> deferred_coalesce_hits{0}; // re-defer same name
    std::atomic<std::uint64_t> flush_total{0};
    std::atomic<std::uint64_t> flush_names_total{0};
    std::atomic<std::uint64_t> render_allowed_cascade_total{0};
    std::atomic<std::uint64_t> hold_ns_total{0};
    std::atomic<std::uint64_t> hold_samples{0};
    std::atomic<std::uint64_t> present_us_samples{0};
    std::atomic<std::uint64_t> present_us_sum{0};
    std::atomic<std::uint64_t> present_us_max{0};
    // Reservoir for rough p99 (power-of-two ring).
    static constexpr std::size_t kHistCap = 256;
    std::atomic<std::uint64_t> present_hist[kHistCap]{};
    std::atomic<std::uint64_t> present_hist_i{0};
    std::atomic<std::uint64_t> budget_us{kDefaultBudgetUs};
    std::atomic<std::uint64_t> wired{1};
    // Issue #2218 present-path guardian
    std::atomic<std::uint64_t> frame_budget_check_total{0};
    std::atomic<std::uint64_t> frame_budget_degrade_total{0};
    std::atomic<std::uint64_t> frame_budget_soft_warn_total{0};
    std::atomic<std::uint64_t> frame_budget_last_p99_us{0};
    std::atomic<std::uint64_t> frame_budget_skip_full_total{0};
    std::atomic<std::int64_t> cached_agent_health{100};
    std::atomic<std::int64_t> cached_agent_action{kActionOptimizeOk};
    // -1 = no force; else override agent-action for next mutate window
    std::atomic<std::int64_t> forced_agent_action{-1};
    std::atomic<std::uint64_t> env_budget_applied{0};
};

[[nodiscard]] inline FrameBudgetMetrics& g_metrics() noexcept {
    static FrameBudgetMetrics m;
    return m;
}

// Thread-local hotpath budget nesting (pairs with arena_policy depth).
inline thread_local int t_budget_depth = 0;
inline thread_local std::uint64_t t_enter_ns = 0;
inline thread_local std::uint64_t t_deadline_ns = 0;

struct DeferredState {
    std::mutex mtx;
    std::unordered_set<std::string> names;
};

[[nodiscard]] inline DeferredState& g_deferred() noexcept {
    static DeferredState s;
    return s;
}

[[nodiscard]] inline bool active() noexcept {
    return t_budget_depth > 0;
}

// Name looks like render-critical cascade (allowed under budget).
[[nodiscard]] inline bool is_render_related_name(std::string_view name) noexcept {
    return aura_is_render_evolution_name(name);
}

[[nodiscard]] inline std::uint64_t now_ns() noexcept {
    using clock = std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch())
            .count());
}

inline void set_budget_us(std::uint64_t us) noexcept {
    if (us == 0)
        us = kDefaultBudgetUs;
    g_metrics().budget_us.store(us, std::memory_order_relaxed);
}

// Issue #2218: apply AURA_FRAME_BUDGET_US once (0 / empty → keep default).
inline void ensure_budget_from_env() noexcept {
    auto& m = g_metrics();
    if (m.env_budget_applied.load(std::memory_order_relaxed) != 0)
        return;
    std::uint64_t expected = 0;
    if (!m.env_budget_applied.compare_exchange_strong(expected, 1, std::memory_order_relaxed))
        return;
    if (const char* e = std::getenv("AURA_FRAME_BUDGET_US")) {
        char* end = nullptr;
        const auto v = std::strtoull(e, &end, 10);
        if (end != e && v > 0)
            set_budget_us(static_cast<std::uint64_t>(v));
    }
}

[[nodiscard]] inline std::uint64_t budget_us() noexcept {
    ensure_budget_from_env();
    return g_metrics().budget_us.load(std::memory_order_relaxed);
}

// Issue #2218: stash last closed-loop health/action from query:render-stats.
inline void note_agent_closed_loop(std::int64_t health, std::int64_t action) noexcept {
    auto& m = g_metrics();
    m.cached_agent_health.store(health, std::memory_order_relaxed);
    m.cached_agent_action.store(action, std::memory_order_relaxed);
}

// Effective agent-action for Agents (forced hold/stop after degrade wins).
[[nodiscard]] inline std::int64_t effective_agent_action(std::int64_t computed) noexcept {
    const auto forced = g_metrics().forced_agent_action.load(std::memory_order_relaxed);
    if (forced >= 0)
        return forced;
    return computed;
}

// Clear forced action after a healthy under-budget window (optional recover).
inline void clear_forced_agent_action() noexcept {
    g_metrics().forced_agent_action.store(-1, std::memory_order_relaxed);
}

// ── Issue #2218 present guardian ──────────────────────────────────────────
// Thread-local: current present took degrade path (Agents / body branches).
inline thread_local bool t_present_degrade = false;

struct PresentGuardianDecision {
    bool degrade = false;
    bool soft_warn = false;
    bool skip_full_rebuild = false; // dirty clean or tiny → keep last good frame
    std::uint64_t p99_us = 0;
    std::uint64_t budget_us = 0;
    std::int64_t health = 100;
    std::int64_t action = kActionOptimizeOk;
    std::int64_t forced_action = -1; // set when degrade engages
};

// Check at entry of tui:present / tui:present-dirty (after AURA_RENDER_HOT_ENTRY).
// p99_us: pass render_frame_time_p99_us() (or 0 when unknown).
// dirty_cells / total_cells: optional; when total>0 and dirty is clean/tiny under
// degrade, skip_full_rebuild is set (keep last good frame on screen).
// Under budget: a few atomic loads only — no locks, no sort.
[[nodiscard]] inline PresentGuardianDecision
check_present_guardian(std::uint64_t p99_us, std::uint64_t dirty_cells = 0,
                       std::uint64_t total_cells = 0) noexcept {
    PresentGuardianDecision d;
    auto& m = g_metrics();
    d.budget_us = budget_us();
    d.p99_us = p99_us;
    d.health = m.cached_agent_health.load(std::memory_order_relaxed);
    d.action = m.cached_agent_action.load(std::memory_order_relaxed);
    m.frame_budget_check_total.fetch_add(1, std::memory_order_relaxed);
    m.frame_budget_last_p99_us.store(p99_us, std::memory_order_relaxed);

    const bool over_budget = p99_us > 0 && p99_us > d.budget_us;
    const bool soft =
        p99_us > 0 && p99_us * 10000ull >= d.budget_us * kSoftMarginBp && !over_budget;
    const bool low_health = d.health < kHealthDegradeThreshold;
    const bool action_hold_stop = d.action == kActionHold || d.action == kActionStop;
    // Forced action from a prior degrade also re-triggers until cleared.
    const auto forced_prev = m.forced_agent_action.load(std::memory_order_relaxed);
    const bool forced_active = forced_prev == kActionHold || forced_prev == kActionStop;

    d.soft_warn = soft;
    if (soft)
        m.frame_budget_soft_warn_total.fetch_add(1, std::memory_order_relaxed);

    d.degrade = over_budget || low_health || action_hold_stop || forced_active;
    t_present_degrade = d.degrade;

    if (!d.degrade) {
        // Recover: under budget + healthy → drop forced action.
        if (p99_us > 0 && p99_us * 2 < d.budget_us && d.health >= kHealthDegradeThreshold)
            clear_forced_agent_action();
        return d;
    }

    m.frame_budget_degrade_total.fetch_add(1, std::memory_order_relaxed);
    // Force next mutate window: stop if critically unhealthy, else hold.
    d.forced_action =
        (d.health < 40 || d.action == kActionStop || over_budget) ? kActionStop : kActionHold;
    m.forced_agent_action.store(d.forced_action, std::memory_order_relaxed);

    // Prefer skip full rebuild when dirty is clean or tiny (keep last good frame).
    if (total_cells == 0) {
        // Unknown dirty → still prefer skip on full present (degrade path).
        d.skip_full_rebuild = true;
    } else if (dirty_cells == 0) {
        d.skip_full_rebuild = true;
    } else {
        const auto dirty_bp = (dirty_cells * 10000ull) / total_cells;
        d.skip_full_rebuild = dirty_bp < kTinyDirtyBp;
    }
    if (d.skip_full_rebuild)
        m.frame_budget_skip_full_total.fetch_add(1, std::memory_order_relaxed);
    return d;
}

[[nodiscard]] inline bool present_degrade_active() noexcept {
    return t_present_degrade;
}

// Enter frame budget (nested). Default deadline = now + budget_us.
inline void enter(std::uint64_t budget_us_override = 0) noexcept {
    ++t_budget_depth;
    if (t_budget_depth == 1) {
        const auto bud = budget_us_override != 0 ? budget_us_override : budget_us();
        t_enter_ns = now_ns();
        t_deadline_ns = t_enter_ns + bud * 1000ull;
    }
}

// Exit one nesting level; outermost records hold duration.
// Does not flush deferred names (caller / mark_define_dirty drains).
inline void exit() noexcept {
    if (t_budget_depth <= 0)
        return;
    --t_budget_depth;
    if (t_budget_depth == 0) {
        const auto end = now_ns();
        if (end >= t_enter_ns) {
            const auto hold = end - t_enter_ns;
            auto& m = g_metrics();
            m.hold_ns_total.fetch_add(hold, std::memory_order_relaxed);
            m.hold_samples.fetch_add(1, std::memory_order_relaxed);
            const auto us = hold / 1000ull;
            m.present_us_sum.fetch_add(us, std::memory_order_relaxed);
            m.present_us_samples.fetch_add(1, std::memory_order_relaxed);
            // max
            auto cur_max = m.present_us_max.load(std::memory_order_relaxed);
            while (us > cur_max && !m.present_us_max.compare_exchange_weak(
                                       cur_max, us, std::memory_order_relaxed)) {
            }
            const auto i = m.present_hist_i.fetch_add(1, std::memory_order_relaxed) %
                           FrameBudgetMetrics::kHistCap;
            m.present_hist[i].store(us, std::memory_order_relaxed);
        }
        t_enter_ns = 0;
        t_deadline_ns = 0;
    }
}

// Past deadline? (soft signal for prefer-partial / prefer-defer).
[[nodiscard]] inline bool past_deadline() noexcept {
    if (t_budget_depth <= 0 || t_deadline_ns == 0)
        return false;
    return now_ns() >= t_deadline_ns;
}

// True → caller should skip cascade body and call defer_cascade instead.
[[nodiscard]] inline bool should_defer_cascade(std::string_view name) noexcept {
    if (t_budget_depth <= 0)
        return false;
    if (name.empty())
        return true; // unknown non-render
    if (is_render_related_name(name))
        return false;
    return true;
}

inline void defer_cascade(std::string_view name) noexcept {
    if (name.empty())
        return;
    auto& st = g_deferred();
    std::lock_guard<std::mutex> lock(st.mtx);
    const auto [it, inserted] = st.names.emplace(name);
    (void)it;
    auto& m = g_metrics();
    if (inserted)
        m.deferred_cascade_total.fetch_add(1, std::memory_order_relaxed);
    else
        m.deferred_coalesce_hits.fetch_add(1, std::memory_order_relaxed);
}

inline void note_render_allowed_cascade() noexcept {
    g_metrics().render_allowed_cascade_total.fetch_add(1, std::memory_order_relaxed);
}

// Take coalesced deferred names (empties the set).
[[nodiscard]] inline std::vector<std::string> drain_deferred() {
    auto& st = g_deferred();
    std::vector<std::string> out;
    {
        std::lock_guard<std::mutex> lock(st.mtx);
        out.reserve(st.names.size());
        for (auto& n : st.names)
            out.push_back(std::move(n));
        st.names.clear();
    }
    if (!out.empty()) {
        auto& m = g_metrics();
        m.flush_total.fetch_add(1, std::memory_order_relaxed);
        m.flush_names_total.fetch_add(out.size(), std::memory_order_relaxed);
    }
    return out;
}

[[nodiscard]] inline std::size_t deferred_pending() noexcept {
    auto& st = g_deferred();
    std::lock_guard<std::mutex> lock(st.mtx);
    return st.names.size();
}

// Approximate p99 from ring samples (sorted copy).
[[nodiscard]] inline std::uint64_t present_p99_us() noexcept {
    auto& m = g_metrics();
    const auto n = m.present_us_samples.load(std::memory_order_relaxed);
    if (n == 0)
        return 0;
    std::uint64_t buf[FrameBudgetMetrics::kHistCap];
    std::size_t count = 0;
    const std::size_t take = n < FrameBudgetMetrics::kHistCap ? static_cast<std::size_t>(n)
                                                              : FrameBudgetMetrics::kHistCap;
    for (std::size_t i = 0; i < FrameBudgetMetrics::kHistCap && count < take; ++i) {
        const auto v = m.present_hist[i].load(std::memory_order_relaxed);
        if (v != 0 || n >= FrameBudgetMetrics::kHistCap)
            buf[count++] = v;
    }
    if (count == 0)
        return 0;
    // Insertion sort (small N).
    for (std::size_t i = 1; i < count; ++i) {
        auto key = buf[i];
        std::size_t j = i;
        while (j > 0 && buf[j - 1] > key) {
            buf[j] = buf[j - 1];
            --j;
        }
        buf[j] = key;
    }
    const auto idx = (count * 99) / 100;
    return buf[idx < count ? idx : count - 1];
}

[[nodiscard]] inline std::uint64_t present_avg_us() noexcept {
    auto& m = g_metrics();
    const auto n = m.present_us_samples.load(std::memory_order_relaxed);
    if (n == 0)
        return 0;
    return m.present_us_sum.load(std::memory_order_relaxed) / n;
}

struct Snapshot {
    std::uint64_t deferred_cascade_total = 0;
    std::uint64_t deferred_coalesce_hits = 0;
    std::uint64_t flush_total = 0;
    std::uint64_t flush_names_total = 0;
    std::uint64_t render_allowed_cascade_total = 0;
    std::uint64_t hold_ns_total = 0;
    std::uint64_t hold_samples = 0;
    std::uint64_t present_p99_us = 0;
    std::uint64_t present_avg_us = 0;
    std::uint64_t present_max_us = 0;
    std::uint64_t budget_us = kDefaultBudgetUs;
    std::uint64_t deferred_pending = 0;
    std::uint64_t wired = 1;
    int schema = kFrameBudgetIssue;
    // #2218 guardian
    std::uint64_t frame_budget_check_total = 0;
    std::uint64_t frame_budget_degrade_total = 0;
    std::uint64_t frame_budget_soft_warn_total = 0;
    std::uint64_t frame_budget_last_p99_us = 0;
    std::uint64_t frame_budget_skip_full_total = 0;
    std::int64_t cached_agent_health = 100;
    std::int64_t cached_agent_action = kActionOptimizeOk;
    std::int64_t forced_agent_action = -1;
    int schema_2218 = kPresentGuardianIssue;
};

[[nodiscard]] inline Snapshot snapshot() noexcept {
    auto& m = g_metrics();
    Snapshot s;
    s.deferred_cascade_total = m.deferred_cascade_total.load(std::memory_order_relaxed);
    s.deferred_coalesce_hits = m.deferred_coalesce_hits.load(std::memory_order_relaxed);
    s.flush_total = m.flush_total.load(std::memory_order_relaxed);
    s.flush_names_total = m.flush_names_total.load(std::memory_order_relaxed);
    s.render_allowed_cascade_total = m.render_allowed_cascade_total.load(std::memory_order_relaxed);
    s.hold_ns_total = m.hold_ns_total.load(std::memory_order_relaxed);
    s.hold_samples = m.hold_samples.load(std::memory_order_relaxed);
    s.present_p99_us = present_p99_us();
    s.present_avg_us = present_avg_us();
    s.present_max_us = m.present_us_max.load(std::memory_order_relaxed);
    s.budget_us = m.budget_us.load(std::memory_order_relaxed);
    s.deferred_pending = static_cast<std::uint64_t>(deferred_pending());
    s.wired = m.wired.load(std::memory_order_relaxed);
    s.schema = kFrameBudgetIssue;
    s.frame_budget_check_total = m.frame_budget_check_total.load(std::memory_order_relaxed);
    s.frame_budget_degrade_total = m.frame_budget_degrade_total.load(std::memory_order_relaxed);
    s.frame_budget_soft_warn_total = m.frame_budget_soft_warn_total.load(std::memory_order_relaxed);
    s.frame_budget_last_p99_us = m.frame_budget_last_p99_us.load(std::memory_order_relaxed);
    s.frame_budget_skip_full_total = m.frame_budget_skip_full_total.load(std::memory_order_relaxed);
    s.cached_agent_health = m.cached_agent_health.load(std::memory_order_relaxed);
    s.cached_agent_action = m.cached_agent_action.load(std::memory_order_relaxed);
    s.forced_agent_action = m.forced_agent_action.load(std::memory_order_relaxed);
    s.schema_2218 = kPresentGuardianIssue;
    return s;
}

inline void reset_for_test() noexcept {
    auto& m = g_metrics();
    m.deferred_cascade_total.store(0, std::memory_order_relaxed);
    m.deferred_coalesce_hits.store(0, std::memory_order_relaxed);
    m.flush_total.store(0, std::memory_order_relaxed);
    m.flush_names_total.store(0, std::memory_order_relaxed);
    m.render_allowed_cascade_total.store(0, std::memory_order_relaxed);
    m.hold_ns_total.store(0, std::memory_order_relaxed);
    m.hold_samples.store(0, std::memory_order_relaxed);
    m.present_us_samples.store(0, std::memory_order_relaxed);
    m.present_us_sum.store(0, std::memory_order_relaxed);
    m.present_us_max.store(0, std::memory_order_relaxed);
    m.present_hist_i.store(0, std::memory_order_relaxed);
    for (auto& h : m.present_hist)
        h.store(0, std::memory_order_relaxed);
    m.budget_us.store(kDefaultBudgetUs, std::memory_order_relaxed);
    m.frame_budget_check_total.store(0, std::memory_order_relaxed);
    m.frame_budget_degrade_total.store(0, std::memory_order_relaxed);
    m.frame_budget_soft_warn_total.store(0, std::memory_order_relaxed);
    m.frame_budget_last_p99_us.store(0, std::memory_order_relaxed);
    m.frame_budget_skip_full_total.store(0, std::memory_order_relaxed);
    m.cached_agent_health.store(100, std::memory_order_relaxed);
    m.cached_agent_action.store(kActionOptimizeOk, std::memory_order_relaxed);
    m.forced_agent_action.store(-1, std::memory_order_relaxed);
    // Keep env_budget_applied so tests don't re-read env mid-suite unless cleared.
    {
        auto& st = g_deferred();
        std::lock_guard<std::mutex> lock(st.mtx);
        st.names.clear();
    }
    t_budget_depth = 0;
    t_enter_ns = 0;
    t_deadline_ns = 0;
    t_present_degrade = false;
}

// RAII: enter on construct, exit on destroy.
struct FrameBudgetGuard {
    explicit FrameBudgetGuard(std::uint64_t budget_us_override = 0) noexcept {
        enter(budget_us_override);
    }
    ~FrameBudgetGuard() noexcept { exit(); }
    FrameBudgetGuard(const FrameBudgetGuard&) = delete;
    FrameBudgetGuard& operator=(const FrameBudgetGuard&) = delete;
};

} // namespace aura::compiler::frame_budget

#endif // AURA_COMPILER_FRAME_BUDGET_HH
