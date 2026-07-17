// arena_auto_policy_stats.h — Issue #743: runtime observability for
// Arena auto-compact + live defrag + fiber safepoint + dirty/Shape
// closed loop (zero release cost, plain header for cross-module bumps).
#ifndef AURA_CORE_ARENA_AUTO_POLICY_STATS_H
#define AURA_CORE_ARENA_AUTO_POLICY_STATS_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>

namespace aura::core::arena_policy {

inline std::atomic<std::uint64_t> auto_compact_triggers_total{0};
inline std::atomic<std::uint64_t> defrag_fiber_safe_hits_total{0};
// Last observed post-mutate fragmentation ratio in basis points (0..10000).
inline std::atomic<std::uint64_t> fragmentation_post_mutate_bp{0};
inline std::atomic<std::uint64_t> shape_inval_on_compact_total{0};
inline std::atomic<std::uint64_t> env_reval_success_total{0};
inline std::atomic<bool> dirty_cascade_pending{false};

inline void signal_dirty_cascade() noexcept {
    dirty_cascade_pending.store(true, std::memory_order_release);
}

inline bool consume_dirty_cascade() noexcept {
    return dirty_cascade_pending.exchange(false, std::memory_order_acq_rel);
}

inline void record_auto_compact_trigger() noexcept {
    auto_compact_triggers_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_defrag_fiber_safe_hit() noexcept {
    defrag_fiber_safe_hits_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_fragmentation_post_mutate(double frag_ratio) noexcept {
    const auto bp = static_cast<std::uint64_t>(frag_ratio * 10000.0);
    fragmentation_post_mutate_bp.store(bp, std::memory_order_relaxed);
}

inline void record_shape_inval_on_compact() noexcept {
    shape_inval_on_compact_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_env_reval_success() noexcept {
    env_reval_success_total.fetch_add(1, std::memory_order_relaxed);
}

// ── Issue #1316/#1320: render hot-path TLS for deopt throttle + compact soft-gate ──
// Depth counter: nested present/draw frames. Arena + evaluator share this TLS so
// auto-compact can soft-gate during render without depending on CompilerMetrics.
inline thread_local int g_render_hotpath_depth = 0;
inline std::atomic<std::uint64_t> compact_soft_gated_render_total{0};
inline std::atomic<std::uint64_t> render_hotpath_enter_total{0};
// Issue #1559: dirty short-circuit skips + engine present/draw totals (process-wide).
inline std::atomic<std::uint64_t> render_hotpath_skip_total{0};
inline std::atomic<std::uint64_t> render_present_total{0};
inline std::atomic<std::uint64_t> render_draw_batch_total{0};
// Deopt throttle: last applied render deopt steady-clock ns (monotonic).
inline std::atomic<std::uint64_t> last_render_deopt_ns{0};
inline std::atomic<std::uint64_t> render_jit_deopt_applied_total{0};
inline std::atomic<std::uint64_t> render_jit_deopt_throttled_total{0};
// Live defrag attempts mirrored for cross-module query without ASTArena lock.
inline std::atomic<std::uint64_t> defrag_attempted_total{0};
inline std::atomic<std::uint64_t> defrag_saved_bytes_total{0};
// Issue #1518: live relocate + compact deopt coordination (process-wide).
inline std::atomic<std::uint64_t> live_relocate_total{0};
inline std::atomic<std::uint64_t> compact_deopt_triggered_total{0};
inline std::atomic<std::uint64_t> compact_deopt_throttled_total{0};
inline std::atomic<std::uint64_t> frag_post_compact_bp{0};
inline std::atomic<std::uint64_t> compact_soft_gated_boundary_total{0};

inline void enter_render_hotpath() noexcept {
    ++g_render_hotpath_depth;
    render_hotpath_enter_total.fetch_add(1, std::memory_order_relaxed);
}
inline void exit_render_hotpath() noexcept {
    if (g_render_hotpath_depth > 0)
        --g_render_hotpath_depth;
}
[[nodiscard]] inline bool in_render_hotpath() noexcept {
    return g_render_hotpath_depth > 0;
}

inline void record_compact_soft_gated_render() noexcept {
    compact_soft_gated_render_total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #1316: return true if deopt should actually fire; false if throttled.
// Window is k_window_ms (default 500ms per AC1).
[[nodiscard]] inline bool try_render_deopt_throttle(std::uint64_t window_ms = 500) noexcept {
    using clock = std::chrono::steady_clock;
    const auto now_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch())
            .count());
    const auto window_ns = window_ms * 1'000'000ull;
    auto prev = last_render_deopt_ns.load(std::memory_order_relaxed);
    if (prev != 0 && now_ns > prev && (now_ns - prev) < window_ns) {
        render_jit_deopt_throttled_total.fetch_add(1, std::memory_order_relaxed);
        return false; // throttled
    }
    last_render_deopt_ns.store(now_ns, std::memory_order_relaxed);
    render_jit_deopt_applied_total.fetch_add(1, std::memory_order_relaxed);
    return true; // apply deopt
}

inline void record_defrag_attempt(std::size_t saved_bytes = 0) noexcept {
    defrag_attempted_total.fetch_add(1, std::memory_order_relaxed);
    if (saved_bytes > 0)
        defrag_saved_bytes_total.fetch_add(saved_bytes, std::memory_order_relaxed);
}

// Issue #1518 helpers.
inline void record_live_relocate(std::uint64_t n = 1) noexcept {
    live_relocate_total.fetch_add(n, std::memory_order_relaxed);
}
inline void record_compact_deopt_triggered() noexcept {
    compact_deopt_triggered_total.fetch_add(1, std::memory_order_relaxed);
}
inline void record_compact_deopt_throttled() noexcept {
    compact_deopt_throttled_total.fetch_add(1, std::memory_order_relaxed);
}
inline void record_frag_post_compact(double frag_ratio) noexcept {
    frag_post_compact_bp.store(static_cast<std::uint64_t>(frag_ratio * 10000.0),
                               std::memory_order_relaxed);
}
inline void record_compact_soft_gated_boundary() noexcept {
    compact_soft_gated_boundary_total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #1521: ShapeProfiler soft invalidate on compact (process-wide mirror).
inline std::atomic<std::uint64_t> shape_inval_on_compact_triggered_total{0};
inline std::atomic<std::uint64_t> deopt_from_arena_compact_total{0};
inline std::atomic<std::uint64_t> shape_stability_post_compact_preserved_total{0};

inline void record_shape_inval_on_compact_triggered() noexcept {
    shape_inval_on_compact_triggered_total.fetch_add(1, std::memory_order_relaxed);
}
inline void record_deopt_from_arena_compact(std::uint64_t n = 1) noexcept {
    deopt_from_arena_compact_total.fetch_add(n, std::memory_order_relaxed);
}
inline void record_shape_stability_post_compact_preserved(std::uint64_t n = 1) noexcept {
    shape_stability_post_compact_preserved_total.fetch_add(n, std::memory_order_relaxed);
}

// ── Issue #1621: smart auto-compact policy (frag + dirty + Shape churn + fiber) ──
//
// Closed-loop: dirty cascade / ShapeProfiler churn / high fragmentation /
// defrag request → evaluate_auto_compact_policy → compact or live_defrag
// at fiber safepoint (not during render hot path).

// Shape churn pending (ShapeProfiler invalidate / stability loss).
inline std::atomic<bool> shape_churn_pending{false};
inline std::atomic<std::uint64_t> smart_policy_evaluations_total{0};
inline std::atomic<std::uint64_t> smart_policy_triggers_total{0};
inline std::atomic<std::uint64_t> shape_churn_triggers_total{0};
inline std::atomic<std::uint64_t> boundary_exit_compact_total{0};
inline std::atomic<std::uint64_t> fiber_transition_compact_total{0};
inline std::atomic<std::uint64_t> live_defrag_policy_hits_total{0};
inline std::atomic<std::uint64_t> smart_policy_soft_gated_total{0};
inline std::atomic<std::uint64_t> smart_policy_wired{1};

// Reason bits for AutoCompactDecision::reason.
inline constexpr std::uint8_t kPolicyReasonFrag = 0x01;
inline constexpr std::uint8_t kPolicyReasonSmallPool = 0x02;
inline constexpr std::uint8_t kPolicyReasonDirty = 0x04;
inline constexpr std::uint8_t kPolicyReasonShapeChurn = 0x08;
inline constexpr std::uint8_t kPolicyReasonDefragReq = 0x10;
inline constexpr std::uint8_t kPolicyReasonFiberSafe = 0x20;

struct AutoCompactDecision {
    bool should_compact = false;
    bool prefer_live_defrag = false;
    std::uint8_t reason = 0;
};

inline void signal_shape_churn() noexcept {
    shape_churn_pending.store(true, std::memory_order_release);
}

[[nodiscard]] inline bool consume_shape_churn() noexcept {
    return shape_churn_pending.exchange(false, std::memory_order_acq_rel);
}

[[nodiscard]] inline bool peek_shape_churn() noexcept {
    return shape_churn_pending.load(std::memory_order_acquire);
}

// Production policy thresholds (documented for Agents).
inline constexpr double kSmartFragThreshold = 0.30;
inline constexpr double kSmartFragSoftThreshold = 0.15; // with defrag_req / churn
inline constexpr double kSmartSmallPoolThreshold = 0.85;

// Evaluate whether auto-compact / live-defrag should fire.
// render_hotpath → always soft-gate (caller should skip).
// fiber_active → mark fiber-safe reason (caller runs safepoint).
[[nodiscard]] inline AutoCompactDecision
evaluate_auto_compact_policy(double frag_ratio, bool defrag_requested, bool dirty_cascade,
                             bool shape_churn, bool fiber_active, bool render_hotpath,
                             double small_pool_util = 0.0) noexcept {
    smart_policy_evaluations_total.fetch_add(1, std::memory_order_relaxed);
    AutoCompactDecision d{};
    if (render_hotpath) {
        smart_policy_soft_gated_total.fetch_add(1, std::memory_order_relaxed);
        return d;
    }
    const bool frag_high = frag_ratio >= kSmartFragThreshold;
    const bool frag_soft = frag_ratio >= kSmartFragSoftThreshold;
    const bool small_high = small_pool_util >= kSmartSmallPoolThreshold;
    if (frag_high)
        d.reason |= kPolicyReasonFrag;
    if (small_high)
        d.reason |= kPolicyReasonSmallPool;
    if (dirty_cascade)
        d.reason |= kPolicyReasonDirty;
    if (shape_churn)
        d.reason |= kPolicyReasonShapeChurn;
    if (defrag_requested)
        d.reason |= kPolicyReasonDefragReq;
    if (fiber_active)
        d.reason |= kPolicyReasonFiberSafe;

    // Trigger when any pressure signal is high, or when a pending
    // defrag request coincides with soft frag / dirty / shape churn
    // (so we do not clear defrag_req on a no-op low-frag path).
    d.should_compact =
        frag_high || small_high || dirty_cascade || shape_churn ||
        (defrag_requested && (frag_soft || dirty_cascade || shape_churn || frag_high));

    // Prefer live_defrag when user/Agent requested defrag or freelist
    // pressure is implied by high frag + defrag_req.
    d.prefer_live_defrag = defrag_requested || (frag_high && (dirty_cascade || shape_churn));

    if (d.should_compact) {
        smart_policy_triggers_total.fetch_add(1, std::memory_order_relaxed);
        if (shape_churn)
            shape_churn_triggers_total.fetch_add(1, std::memory_order_relaxed);
        if (d.prefer_live_defrag)
            live_defrag_policy_hits_total.fetch_add(1, std::memory_order_relaxed);
    }
    return d;
}

inline void record_boundary_exit_compact() noexcept {
    boundary_exit_compact_total.fetch_add(1, std::memory_order_relaxed);
}
inline void record_fiber_transition_compact() noexcept {
    fiber_transition_compact_total.fetch_add(1, std::memory_order_relaxed);
}

} // namespace aura::core::arena_policy

#endif // AURA_CORE_ARENA_AUTO_POLICY_STATS_H