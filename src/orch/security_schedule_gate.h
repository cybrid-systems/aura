// src/orch/security_schedule_gate.h — Issue #2590
//
// Pure security schedule gate that decides whether the orch / agent-body
// should ADMIT a NEW mutate. Synthesizes commit_readiness (#2553),
// capability deny rate (#2534 trail), mid-fallback SLO breach, and
// posture wal_off under Restricted (#2076). Soft / sandbox=off is
// observe-only (counted but never denies) — production default denies
// new mutate when gate flips false (mailbox already enqueued is not
// killed — additive over existing admission).
//
// Design follows the #2543 AOT throttle precedent
// (aot_hot_update_health.hh::decide_hot_update_throttle):
//   - Pure: same input → same output, no atomics (#2590 AC1).
//   - evaluate_security_schedule() wraps pure + bumps process-wide
//     counters atomically.
//   - query:security-schedule-gate surfaces the live snapshot.
//
// Admission wiring (orch:agent-body / orch:parallel-intend / mutate
// dispatch) is call-site responsibility — call evaluate_security_schedule()
// BEFORE admitting new mutate; if decision.would_allow_new_mutate == false
// in production mode, deny the new work and return a typed error. Counters
// always bump (soft + production both observable).

#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace aura::orch {

// ── Decision types ───────────────────────────────────────────────
enum class SecurityScheduleForceReason : std::uint8_t {
    ok = 0,
    commit_not_ready = 1,
    deny_storm = 2,
    mid_fallback_slo = 3,
    posture_degraded = 4,
};

[[nodiscard]] inline std::string_view
security_schedule_force_reason_name(SecurityScheduleForceReason r) noexcept {
    switch (r) {
        case SecurityScheduleForceReason::ok:
            return "ok";
        case SecurityScheduleForceReason::commit_not_ready:
            return "commit-not-ready";
        case SecurityScheduleForceReason::deny_storm:
            return "deny-storm";
        case SecurityScheduleForceReason::mid_fallback_slo:
            return "mid-fallback-slo";
        case SecurityScheduleForceReason::posture_degraded:
            return "posture-degraded";
    }
    return "unknown";
}

struct SecurityScheduleDecision {
    bool would_allow_new_mutate = true;
    SecurityScheduleForceReason force_reason = SecurityScheduleForceReason::ok;
};

struct SecurityScheduleInput {
    // From typed_mutation_audit.h::commit_readiness (Issue #2553).
    bool commit_readiness_would_allow = true;
    bool commit_readiness_hard_reject = false;
    // Short-window capability deny rate exceeded threshold (#2534 trail).
    bool capability_deny_storm = false;
    // mid_fallback_rate_bp > SLO.
    bool mid_fallback_slo_breach = false;
    // posture wal_off under Restricted force (#2076).
    bool posture_wal_off_restricted = false;
    // Production default: AURA_SECURITY_HARD or production profile.
    bool production_mode = false;
    // Soft mode: AURA_SECURITY_SOFT or sandbox=off → observe only.
    bool soft_mode = false;
};

// ── Pure decision (#2590 AC1: same input → same output, no atomics) ──
//
// Priority order (first match wins):
//   1. commit-not-ready   (commit_readiness_hard_reject && production && !soft)
//   2. deny-storm         (capability_deny_storm && production && !soft)
//   3. mid-fallback-slo   (mid_fallback_slo_breach && production && !soft)
//   4. posture-degraded   (posture_wal_off_restricted && production && !soft)
//   else: ok / allow
[[nodiscard]] inline SecurityScheduleDecision
decide_security_schedule(const SecurityScheduleInput& in) noexcept {
    SecurityScheduleDecision d;
    const bool enforce = in.production_mode && !in.soft_mode;
    if (enforce && !in.commit_readiness_would_allow && in.commit_readiness_hard_reject) {
        d.would_allow_new_mutate = false;
        d.force_reason = SecurityScheduleForceReason::commit_not_ready;
        return d;
    }
    if (enforce && in.capability_deny_storm) {
        d.would_allow_new_mutate = false;
        d.force_reason = SecurityScheduleForceReason::deny_storm;
        return d;
    }
    if (enforce && in.mid_fallback_slo_breach) {
        d.would_allow_new_mutate = false;
        d.force_reason = SecurityScheduleForceReason::mid_fallback_slo;
        return d;
    }
    if (enforce && in.posture_wal_off_restricted) {
        d.would_allow_new_mutate = false;
        d.force_reason = SecurityScheduleForceReason::posture_degraded;
        return d;
    }
    return d;
}

// ── Process-wide counters (#2590 AC2 + AC5) ───────────────────────
struct OrchSecurityScheduleCounters {
    std::atomic<std::uint64_t> checks_total{0};
    std::atomic<std::uint64_t> deny_total{0};
    std::atomic<std::uint64_t> allow_total{0};
    std::atomic<std::uint64_t> deny_commit_not_ready_total{0};
    std::atomic<std::uint64_t> deny_deny_storm_total{0};
    std::atomic<std::uint64_t> deny_mid_fallback_slo_total{0};
    std::atomic<std::uint64_t> deny_posture_degraded_total{0};
    std::atomic<std::int64_t> last_force_reason_code{0};
    std::atomic<std::int64_t> last_would_allow{1}; // 1=allow, 0=deny
};

inline OrchSecurityScheduleCounters g_orch_security_schedule_counters{};

// ── evaluate_security_schedule: pure + counter bumps (#2590 AC2 + AC3) ──
//
// Callers SHOULD invoke this once per new-mutate admission decision
// (orch:agent-body / orch:parallel-intend / mutate dispatch). Soft mode
// bumps counters but never denies; production mode denies on force_reason.
[[nodiscard]] inline SecurityScheduleDecision
evaluate_security_schedule(const SecurityScheduleInput& in) noexcept {
    const auto d = decide_security_schedule(in);
    auto& c = g_orch_security_schedule_counters;
    c.checks_total.fetch_add(1, std::memory_order_relaxed);
    if (d.would_allow_new_mutate) {
        c.allow_total.fetch_add(1, std::memory_order_relaxed);
    } else {
        c.deny_total.fetch_add(1, std::memory_order_relaxed);
        switch (d.force_reason) {
            case SecurityScheduleForceReason::commit_not_ready:
                c.deny_commit_not_ready_total.fetch_add(1, std::memory_order_relaxed);
                break;
            case SecurityScheduleForceReason::deny_storm:
                c.deny_deny_storm_total.fetch_add(1, std::memory_order_relaxed);
                break;
            case SecurityScheduleForceReason::mid_fallback_slo:
                c.deny_mid_fallback_slo_total.fetch_add(1, std::memory_order_relaxed);
                break;
            case SecurityScheduleForceReason::posture_degraded:
                c.deny_posture_degraded_total.fetch_add(1, std::memory_order_relaxed);
                break;
            default:
                break;
        }
    }
    c.last_force_reason_code.store(static_cast<std::int64_t>(d.force_reason),
                                   std::memory_order_relaxed);
    c.last_would_allow.store(d.would_allow_new_mutate ? 1 : 0, std::memory_order_relaxed);
    return d;
}

// ── Test reset ───────────────────────────────────────────────────
inline void reset_orch_security_schedule_counters_for_test() noexcept {
    auto& c = g_orch_security_schedule_counters;
    c.checks_total.store(0, std::memory_order_relaxed);
    c.deny_total.store(0, std::memory_order_relaxed);
    c.allow_total.store(0, std::memory_order_relaxed);
    c.deny_commit_not_ready_total.store(0, std::memory_order_relaxed);
    c.deny_deny_storm_total.store(0, std::memory_order_relaxed);
    c.deny_mid_fallback_slo_total.store(0, std::memory_order_relaxed);
    c.deny_posture_degraded_total.store(0, std::memory_order_relaxed);
    c.last_force_reason_code.store(0, std::memory_order_relaxed);
    c.last_would_allow.store(1, std::memory_order_relaxed);
}

} // namespace aura::orch