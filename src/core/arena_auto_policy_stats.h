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

// ── Issue #1621 / #1919: smart auto-compact policy ──
//
// Closed-loop: dirty cascade / ShapeProfiler churn / high fragmentation /
// defrag request / JIT deopt + AI mutation pressure →
// evaluate_auto_compact_policy → compact or live_defrag at fiber safepoint
// (not during render hot path). #1919 adds Conservative/Balanced/Aggressive
// modes with dynamic frag thresholds (30–60%) and false-positive tracking.

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

// Issue #1919: intelligent policy mode + AI/JIT pressure + FP gate.
enum class AutoCompactMode : std::uint8_t {
    Conservative = 0, // higher frag threshold, fewer triggers
    Balanced = 1,     // default production (30% base)
    Aggressive = 2,   // lower threshold, act earlier under mutation load
};
inline std::atomic<std::uint8_t> g_auto_compact_mode{
    static_cast<std::uint8_t>(AutoCompactMode::Balanced)};
// Pending AI multi-round mutation pressure (mutate:rebind / atomic-batch).
inline std::atomic<bool> mutation_pressure_pending{false};
inline std::atomic<std::uint64_t> mutation_pressure_signal_total{0};
// JIT deopt pressure (recent compact-linked or general deopt storms).
inline std::atomic<bool> jit_deopt_pressure_pending{false};
inline std::atomic<std::uint64_t> jit_deopt_pressure_signal_total{0};
// False-positive: trigger that reclaimed 0 bytes (for <5% AC gate).
inline std::atomic<std::uint64_t> auto_compact_false_positive_total{0};
inline std::atomic<std::uint64_t> auto_compact_true_positive_total{0};
inline std::atomic<std::uint64_t> intelligent_policy_wired{1}; // #1919
inline std::atomic<std::uint64_t> dynamic_threshold_bp{3000};  // last computed frag thr (bp)

// Reason bits for AutoCompactDecision::reason.
inline constexpr std::uint8_t kPolicyReasonFrag = 0x01;
inline constexpr std::uint8_t kPolicyReasonSmallPool = 0x02;
inline constexpr std::uint8_t kPolicyReasonDirty = 0x04;
inline constexpr std::uint8_t kPolicyReasonShapeChurn = 0x08;
inline constexpr std::uint8_t kPolicyReasonDefragReq = 0x10;
inline constexpr std::uint8_t kPolicyReasonFiberSafe = 0x20;
inline constexpr std::uint8_t kPolicyReasonMutation = 0x40; // #1919
inline constexpr std::uint8_t kPolicyReasonJitDeopt = 0x80; // #1919

struct AutoCompactDecision {
    bool should_compact = false;
    bool prefer_live_defrag = false;
    std::uint8_t reason = 0;
    double frag_threshold_used = 0.30; // #1919 dynamic
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

inline void set_auto_compact_mode(AutoCompactMode mode) noexcept {
    g_auto_compact_mode.store(static_cast<std::uint8_t>(mode), std::memory_order_release);
}

[[nodiscard]] inline AutoCompactMode auto_compact_mode() noexcept {
    return static_cast<AutoCompactMode>(g_auto_compact_mode.load(std::memory_order_acquire));
}

inline void signal_mutation_pressure() noexcept {
    mutation_pressure_pending.store(true, std::memory_order_release);
    mutation_pressure_signal_total.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline bool peek_mutation_pressure() noexcept {
    return mutation_pressure_pending.load(std::memory_order_acquire);
}

[[nodiscard]] inline bool consume_mutation_pressure() noexcept {
    return mutation_pressure_pending.exchange(false, std::memory_order_acq_rel);
}

inline void signal_jit_deopt_pressure() noexcept {
    jit_deopt_pressure_pending.store(true, std::memory_order_release);
    jit_deopt_pressure_signal_total.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline bool peek_jit_deopt_pressure() noexcept {
    return jit_deopt_pressure_pending.load(std::memory_order_acquire);
}

[[nodiscard]] inline bool consume_jit_deopt_pressure() noexcept {
    return jit_deopt_pressure_pending.exchange(false, std::memory_order_acq_rel);
}

// Record post-trigger outcome for false-positive rate (AC: <5% FP).
inline void record_auto_compact_outcome(bool reclaimed_bytes) noexcept {
    if (reclaimed_bytes)
        auto_compact_true_positive_total.fetch_add(1, std::memory_order_relaxed);
    else
        auto_compact_false_positive_total.fetch_add(1, std::memory_order_relaxed);
}

// FP rate in basis points: FP / (FP+TP) * 10000. 0 when no samples.
[[nodiscard]] inline std::uint64_t auto_compact_false_positive_bp() noexcept {
    const auto fp = auto_compact_false_positive_total.load(std::memory_order_relaxed);
    const auto tp = auto_compact_true_positive_total.load(std::memory_order_relaxed);
    const auto den = fp + tp;
    if (den == 0)
        return 0;
    return (fp * 10000ull) / den;
}

// Production base thresholds (documented for Agents). #1919 dynamic range 30–60%.
inline constexpr double kSmartFragThreshold = 0.30;
inline constexpr double kSmartFragSoftThreshold = 0.15; // with defrag_req / churn
inline constexpr double kSmartSmallPoolThreshold = 0.85;
inline constexpr double kFragThresholdMin = 0.30;            // #1919
inline constexpr double kFragThresholdMax = 0.60;            // #1919
inline constexpr std::uint64_t kFalsePositiveTargetBp = 500; // 5%

// Dynamic frag threshold for current mode + AI/JIT pressure (0.30..0.60).
[[nodiscard]] inline double compute_dynamic_frag_threshold(AutoCompactMode mode, bool mutation_p,
                                                           bool deopt_p, bool shape_churn,
                                                           bool dirty) noexcept {
    double thr = kSmartFragThreshold; // 0.30 Balanced base
    switch (mode) {
        case AutoCompactMode::Conservative:
            thr = 0.50; // act later
            break;
        case AutoCompactMode::Aggressive:
            thr = 0.30;
            break;
        case AutoCompactMode::Balanced:
        default:
            thr = 0.35;
            break;
    }
    // Under AI mutation / shape churn / dirty: lower threshold (act sooner).
    if (mutation_p || shape_churn || dirty)
        thr -= 0.05;
    // Under JIT deopt pressure: raise threshold (avoid compact→deopt storms).
    if (deopt_p)
        thr += 0.10;
    if (thr < kFragThresholdMin)
        thr = kFragThresholdMin;
    if (thr > kFragThresholdMax)
        thr = kFragThresholdMax;
    dynamic_threshold_bp.store(static_cast<std::uint64_t>(thr * 10000.0),
                               std::memory_order_relaxed);
    return thr;
}

// Evaluate whether auto-compact / live-defrag should fire.
// render_hotpath → always soft-gate (caller should skip).
// fiber_active → mark fiber-safe reason (caller runs safepoint).
// #1919: peeks mutation/JIT deopt pressure for dynamic threshold.
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
    const bool mutation_p = peek_mutation_pressure();
    const bool deopt_p = peek_jit_deopt_pressure();
    const auto mode = auto_compact_mode();
    const double frag_thr =
        compute_dynamic_frag_threshold(mode, mutation_p, deopt_p, shape_churn, dirty_cascade);
    d.frag_threshold_used = frag_thr;
    // Soft band: half of dynamic thr (floor soft base).
    const double soft_thr =
        frag_thr * 0.5 < kSmartFragSoftThreshold ? kSmartFragSoftThreshold : frag_thr * 0.5;

    const bool frag_high = frag_ratio >= frag_thr;
    const bool frag_soft = frag_ratio >= soft_thr;
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
    if (mutation_p)
        d.reason |= kPolicyReasonMutation;
    if (deopt_p)
        d.reason |= kPolicyReasonJitDeopt;

    // Trigger when frag/pool pressure is high, or dirty/shape cascade, or
    // pending defrag/mutation coincides with soft frag (avoid clearing
    // defrag_req / sticky mutation on no-op low-frag paths).
    // Conservative + deopt: require frag_high/small_high (avoid storms).
    // Mutation pressure alone never forces compact — it only lowers the
    // dynamic threshold and assists soft-frag / defrag paths (#1919).
    const bool conservative_gate =
        mode == AutoCompactMode::Conservative && deopt_p && !frag_high && !small_high;
    d.should_compact =
        frag_high || small_high ||
        (!conservative_gate && (dirty_cascade || shape_churn ||
                                (defrag_requested && (frag_soft || dirty_cascade || shape_churn ||
                                                      frag_high || mutation_p)) ||
                                (mutation_p && frag_soft)));

    // Prefer live_defrag when user/Agent requested defrag or freelist
    // pressure is implied by high frag + dirty/churn/mutation.
    d.prefer_live_defrag =
        defrag_requested || (frag_high && (dirty_cascade || shape_churn || mutation_p));

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