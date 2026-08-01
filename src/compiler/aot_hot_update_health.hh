// aot_hot_update_health.hh — Issue #2506: single Agent JIT/AOT recovery gate score.
//
// Pure, read-only aggregation of ReloadRecovery / StormLevel / remount /
// epoch-invariant signals so Agents stop OR-ing many hot-update queries:
//   query:reload-recovery-state (#2367)
//   query:hot-update-registry-stats
//   query:aot-incremental-reemit-stats / remount counters
//   epoch invariant walks (#2366 / #2501)
//
// Pattern: query:mutation-concurrency-health (#2379), query:security-health (#2389).
// Gate signal only — does NOT change recovery policy actions.
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

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace aura::compiler {

inline constexpr int kAotHotUpdateHealthIssue = 2506;

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

} // namespace aura::compiler

#endif // AURA_COMPILER_AOT_HOT_UPDATE_HEALTH_HH
