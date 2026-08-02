// aot_hot_update_health.hh — Issue #2506: single Agent JIT/AOT recovery gate score.
// Issue #2543: orch agent self-throttle control plane over the same score.
//
// Pure, read-only aggregation of ReloadRecovery / StormLevel / remount /
// epoch-invariant signals so Agents stop OR-ing many hot-update queries:
//   query:reload-recovery-state (#2367)
//   query:hot-update-registry-stats
//   query:aot-incremental-reemit-stats / remount counters
//   epoch invariant walks (#2366 / #2501)
//
// Pattern: query:mutation-concurrency-health (#2379), query:security-health (#2389).
// #2506: gate signal only. #2543: advisory throttle (never hard-fails mutate).
//
// ── Score definition (AC1 / AC2) ──
//
//   Start health_bp = 10000.
//
//   Hard penalties (each applied at most once when signal non-zero):
//     storm (StormLevel != None | hard_storm)     −3500
//     force-jit (force_jit_regions_mask != 0)     −3000
//     reload-fail (last AotReloadFail != Ok)      −2000
//     remount-fail (fail_total > ok_total)        −1500
//     epoch-invariant (violation_total > 0)       −2000
//     deferred-reemit (pending flag)              −1500
//
//   Soft penalties (capped):
//     attempts_left:   min(1000, attempts_left * 200)
//     pending_dirty:   min(1500, pending_dirty_count * 50)
//
//   Clamp [0, 10000]. Idle vacuous snapshot → health_bp = 10000.
//
// ── force_reason priority (AC2) ──
//
//   storm > force-jit > reload-fail > remount-fail >
//   epoch-invariant > deferred-reemit > ok
//
// Codes: 0=ok 1=storm 2=force-jit 3=reload-fail 4=remount-fail
//        5=epoch-invariant 6=deferred-reemit
//
// Hard signals set force_reason even if soft score still ≥ budget.
// When no signal → "ok" (AC1 idle healthy).

#ifndef AURA_COMPILER_AOT_HOT_UPDATE_HEALTH_HH
#define AURA_COMPILER_AOT_HOT_UPDATE_HEALTH_HH

#include "compiler/hot_update_registry.hh" // aura_reload_recovery_snapshot + get

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string_view>

extern "C" std::uint64_t aura_epoch_invariant_violation_total_v_read(void);

namespace aura::compiler {

inline constexpr int kAotHotUpdateHealthIssue = 2506;
// Issue #2543: orch self-throttle over health_bp (control plane).
inline constexpr int kAotHotUpdateHealthThrottleIssue = 2543;

struct AotHotUpdateHealthSnapshot {
    // ReloadRecoveryState core (#2302 / #2367).
    std::uint32_t attempts_left = 0;
    std::uint64_t force_jit_regions_mask = 0;
    std::uint64_t pending_dirty_count = 0;
    std::uint8_t deferred_reemit_pending = 0;
    // StormLevel bitmask (#2094): 0=None, 1=Shape, 2=Global, 3=Both.
    std::uint8_t storm_level = 0;
    std::int64_t hard_storm_active = 0;
    // AotReloadFail enum; 0 = Ok (success / never failed).
    std::uint8_t last_reload_fail_reason = 0;
    // Capture remount outcome (#2234).
    std::uint64_t remount_fail_total = 0;
    std::uint64_t remount_ok_total = 0;
    // Epoch invariant (#2366 / #2501).
    std::uint64_t epoch_invariant_violation_total = 0;
    // Collapsed recovery-active (snapshot builder may precompute).
    std::int64_t recovery_active = 0;
};

struct AotHotUpdateHealthResult {
    std::uint64_t health_bp = 10000;
    std::uint64_t health_budget_bp = 8000;
    std::string_view force_reason = "ok";
    // force-reason-code: 0=ok 1=storm 2=force-jit 3=reload-fail
    // 4=remount-fail 5=epoch-invariant 6=deferred-reemit
    std::int64_t force_reason_code = 0;
    std::int64_t recovery_active = 0;
    AotHotUpdateHealthSnapshot components{};
};

// Default budget 8000 bp (80%). Override: AURA_AOT_HOT_UPDATE_HEALTH_BUDGET_BP.
[[nodiscard]] inline std::uint64_t aot_hot_update_health_budget_bp() noexcept {
    const char* e = std::getenv("AURA_AOT_HOT_UPDATE_HEALTH_BUDGET_BP");
    if (e == nullptr || e[0] == '\0')
        return 8000;
    std::uint64_t v = 0;
    for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
        v = v * 10 + static_cast<std::uint64_t>(*p - '0');
    if (v > 10000)
        v = 10000;
    return v;
}

[[nodiscard]] inline bool has_storm(const AotHotUpdateHealthSnapshot& s) noexcept {
    return s.storm_level != 0 || s.hard_storm_active != 0;
}
[[nodiscard]] inline bool has_force_jit(const AotHotUpdateHealthSnapshot& s) noexcept {
    return s.force_jit_regions_mask != 0;
}
[[nodiscard]] inline bool has_reload_fail(const AotHotUpdateHealthSnapshot& s) noexcept {
    return s.last_reload_fail_reason != 0; // AotReloadFail::Ok == 0
}
[[nodiscard]] inline bool has_remount_fail(const AotHotUpdateHealthSnapshot& s) noexcept {
    return s.remount_fail_total > s.remount_ok_total;
}
[[nodiscard]] inline bool has_epoch_invariant(const AotHotUpdateHealthSnapshot& s) noexcept {
    return s.epoch_invariant_violation_total > 0;
}
[[nodiscard]] inline bool has_deferred_reemit(const AotHotUpdateHealthSnapshot& s) noexcept {
    return s.deferred_reemit_pending != 0;
}

// Pure score from a snapshot (no atomics — AC3 pure / unit-testable).
[[nodiscard]] inline AotHotUpdateHealthResult
compute_aot_hot_update_health(const AotHotUpdateHealthSnapshot& s) noexcept {
    AotHotUpdateHealthResult r;
    r.components = s;
    r.health_budget_bp = aot_hot_update_health_budget_bp();

    std::int64_t bp = 10000;

    // Hard penalties.
    if (has_storm(s))
        bp -= 3500;
    if (has_force_jit(s))
        bp -= 3000;
    if (has_reload_fail(s))
        bp -= 2000;
    if (has_remount_fail(s))
        bp -= 1500;
    if (has_epoch_invariant(s))
        bp -= 2000;
    if (has_deferred_reemit(s))
        bp -= 1500;

    // Soft penalties (capped).
    if (s.attempts_left > 0) {
        const auto soft =
            std::min<std::uint64_t>(1000, static_cast<std::uint64_t>(s.attempts_left) * 200);
        bp -= static_cast<std::int64_t>(soft);
    }
    if (s.pending_dirty_count > 0) {
        const auto soft = std::min<std::uint64_t>(1500, s.pending_dirty_count * 50);
        bp -= static_cast<std::int64_t>(soft);
    }

    if (bp < 0)
        bp = 0;
    if (bp > 10000)
        bp = 10000;
    r.health_bp = static_cast<std::uint64_t>(bp);

    // recovery_active: any non-idle recovery signal (matches #2367 collapse).
    r.recovery_active = (s.attempts_left != 0 || s.force_jit_regions_mask != 0 ||
                         s.pending_dirty_count != 0 || s.deferred_reemit_pending != 0 ||
                         s.storm_level != 0 || s.hard_storm_active != 0 || s.recovery_active != 0)
                            ? 1
                            : 0;

    // force_reason priority (hard first). Independent of budget.
    if (has_storm(s)) {
        r.force_reason = "storm";
        r.force_reason_code = 1;
    } else if (has_force_jit(s)) {
        r.force_reason = "force-jit";
        r.force_reason_code = 2;
    } else if (has_reload_fail(s)) {
        r.force_reason = "reload-fail";
        r.force_reason_code = 3;
    } else if (has_remount_fail(s)) {
        r.force_reason = "remount-fail";
        r.force_reason_code = 4;
    } else if (has_epoch_invariant(s)) {
        r.force_reason = "epoch-invariant";
        r.force_reason_code = 5;
    } else if (has_deferred_reemit(s)) {
        r.force_reason = "deferred-reemit";
        r.force_reason_code = 6;
    } else {
        r.force_reason = "ok";
        r.force_reason_code = 0;
    }
    return r;
}

// ── Issue #2543: orch agent self-throttle control plane ──
//
// Advisory only (never hard-fails mutate / spawn). When
// health_bp < health_budget_bp, map force_reason_code → action:
//
//   storm (1) / force-jit (2)     → split-batch   (cap concurrency=1)
//   reload-fail (3) / remount (4) /
//     epoch-invariant (5)         → delay-mutate  (cap concurrency=2)
//   deferred-reemit (6)           → skip-reemit   (cap concurrency=4)
//   healthy (health_bp ≥ budget)  → none          (no metric bump)
//
// Soft empty / idle → health_bp=10000 → zero throttle cost (AC2).
enum class HotUpdateThrottleAction : std::uint8_t {
    None = 0,
    SplitBatch = 1,
    DelayMutate = 2,
    SkipReemit = 3,
};

struct HotUpdateThrottleDecision {
    bool throttle = false;
    HotUpdateThrottleAction action = HotUpdateThrottleAction::None;
    std::string_view action_name = "none";
    // Suggested max_concurrency cap for parallel_intend / agent batches.
    std::uint32_t max_concurrency_cap = 1024;
    AotHotUpdateHealthResult health{};
};

// Pure: map scored health → throttle decision (no atomics / side effects).
[[nodiscard]] inline HotUpdateThrottleDecision
decide_hot_update_throttle(const AotHotUpdateHealthResult& h) noexcept {
    HotUpdateThrottleDecision d;
    d.health = h;
    if (h.health_bp >= h.health_budget_bp) {
        d.throttle = false;
        d.action = HotUpdateThrottleAction::None;
        d.action_name = "none";
        d.max_concurrency_cap = 1024;
        return d;
    }
    d.throttle = true;
    switch (h.force_reason_code) {
        case 1: // storm
        case 2: // force-jit
            d.action = HotUpdateThrottleAction::SplitBatch;
            d.action_name = "split-batch";
            d.max_concurrency_cap = 1;
            break;
        case 3: // reload-fail
        case 4: // remount-fail
        case 5: // epoch-invariant
            d.action = HotUpdateThrottleAction::DelayMutate;
            d.action_name = "delay-mutate";
            d.max_concurrency_cap = 2;
            break;
        case 6: // deferred-reemit
            d.action = HotUpdateThrottleAction::SkipReemit;
            d.action_name = "skip-reemit";
            d.max_concurrency_cap = 4;
            break;
        default:
            d.action = HotUpdateThrottleAction::SplitBatch;
            d.action_name = "split-batch";
            d.max_concurrency_cap = 2;
            break;
    }
    return d;
}

// Live sample of recovery + epoch signals (pure relaxed loads).
// Remount counters optional (0 when metrics unavailable) — storm/force-jit
// still drive the primary throttle path.
[[nodiscard]] inline AotHotUpdateHealthSnapshot sample_aot_hot_update_health_snapshot() noexcept {
    AotHotUpdateHealthSnapshot snap;
    aura_reload_recovery_snapshot rs{};
    aura_hot_update_reload_recovery_get_snapshot(&rs);
    snap.attempts_left = static_cast<std::uint32_t>(rs.attempts_left);
    snap.force_jit_regions_mask = static_cast<std::uint64_t>(rs.force_jit_regions_mask);
    snap.pending_dirty_count = static_cast<std::uint64_t>(rs.pending_dirty_count);
    snap.deferred_reemit_pending = static_cast<std::uint8_t>(rs.deferred_reemit_pending);
    snap.storm_level = static_cast<std::uint8_t>(rs.storm_level);
    snap.hard_storm_active = rs.hard_storm_active;
    snap.last_reload_fail_reason = static_cast<std::uint8_t>(rs.last_reason);
    snap.recovery_active = rs.recovery_active;
    snap.epoch_invariant_violation_total = aura_epoch_invariant_violation_total_v_read();
    return snap;
}

// Process-level orch throttle observability (#2543).
// Zero bump when healthy (AC2).
inline std::atomic<std::uint64_t> g_orch_hot_update_health_throttle_total{0};
inline std::atomic<std::uint64_t> g_orch_hot_update_health_checks_total{0};
inline std::atomic<std::int64_t> g_orch_hot_update_health_last_force_reason{0};
inline std::atomic<std::int64_t> g_orch_hot_update_health_last_action{0};

// Tick: sample → score → decide. Bumps throttle metric only when
// throttle==true. Safe to call from agent body / parallel_intend / boundary.
[[nodiscard]] inline HotUpdateThrottleDecision orch_hot_update_health_throttle_tick() noexcept {
    g_orch_hot_update_health_checks_total.fetch_add(1, std::memory_order_relaxed);
    const auto snap = sample_aot_hot_update_health_snapshot();
    const auto scored = compute_aot_hot_update_health(snap);
    auto d = decide_hot_update_throttle(scored);
    g_orch_hot_update_health_last_force_reason.store(scored.force_reason_code,
                                                     std::memory_order_relaxed);
    g_orch_hot_update_health_last_action.store(static_cast<std::int64_t>(d.action),
                                               std::memory_order_relaxed);
    if (d.throttle) {
        g_orch_hot_update_health_throttle_total.fetch_add(1, std::memory_order_relaxed);
    }
    return d;
}

// Cap requested concurrency for parallel_intend / agent batches.
// Healthy path returns requested unchanged (no throttle metric when
// already healthy — tick still records checks_total).
[[nodiscard]] inline std::uint32_t
apply_hot_update_health_concurrency_cap(std::uint32_t requested) noexcept {
    if (requested == 0)
        requested = 1;
    const auto d = orch_hot_update_health_throttle_tick();
    if (!d.throttle)
        return requested;
    return std::min(requested, d.max_concurrency_cap);
}

// Should non-critical reemit be skipped (deferred-reemit / unhealthy)?
[[nodiscard]] inline bool orch_hot_update_health_should_skip_reemit() noexcept {
    const auto d = orch_hot_update_health_throttle_tick();
    return d.throttle && (d.action == HotUpdateThrottleAction::SkipReemit ||
                          d.action == HotUpdateThrottleAction::SplitBatch ||
                          d.action == HotUpdateThrottleAction::DelayMutate);
}

} // namespace aura::compiler

#endif // AURA_COMPILER_AOT_HOT_UPDATE_HEALTH_HH
