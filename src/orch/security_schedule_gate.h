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

#include "core/audit_wal.hh"                 // #2076 wal_off posture under Restricted
#include "core/capability_model.hh"          // #2534 capability deny storm window
#include "core/mutation_audit_wal.hh"        // audit_wal_enabled (#2076)
#include "compiler/audit_mid_fallback_slo.h" // #2594 mid-fallback SLO
#include "compiler/typed_mutation_audit.h"   // #2553 commit_readiness; production_defaults_active

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

// ── Issue #2660: live signal extractors (production admit wiring) ─
//
// `decide_security_schedule()` is pure (#2590 AC1); the gate contract
// requires callers to feed it live signals. The helpers below build
// each input from current process state — used by try_acquire /
// try_acquire_for_region (evaluator_mutation_boundary.cpp) and
// parallel-intend (evaluator_primitives_agent.cpp) so the gate matches
// real posture instead of always reading "all clear" defaults.
//
// Soft / sandbox=off: callers still flow these in but the gate's
// `enforce = production && !soft` short-circuits the deny branch
// (counters still bump). Cost is one relaxed load per signal — no
// alloc on the hot path.

// Issue #2553: commit_readiness live extraction. `hard_reject` returns
// true when the gate flipped to a non-ok reason (force_reason_code != 0
// == "ok" sentinel — see commit_readiness_reason_code).
inline std::pair<bool, bool> commit_readiness_live_signals() noexcept {
    const auto in = aura::compiler::typed_audit::commit_readiness_live_policy();
    const auto r = aura::compiler::typed_audit::commit_readiness(in);
    return {r.would_allow_commit, r.force_reason_code != 0};
}

// Issue #2534: capability deny storm window. Process-wide counter
// compared against a single threshold (counter increments monotonically;
// under unit / soft paths the counter is reset by test reset). The
// threshold is intentionally conservative — callers can lower it via
// AURA_DENY_STORM_THRESHOLD env override if needed.
inline std::atomic<std::uint64_t>& g_capability_deny_storm_threshold() noexcept {
    static std::atomic<std::uint64_t> v{64};
    return v;
}
inline bool capability_deny_storm_live() noexcept {
    const auto& met = aura::core::capability::g_capability_effect_metrics();
    const auto threshold = g_capability_deny_storm_threshold().load(std::memory_order_relaxed);
    return met.capability_effect_denied_total.load(std::memory_order_relaxed) >= threshold;
}

// Issue #2594: mid-fallback SLO breach. evaluate_audit_mid_fallback_slo
// returns a `MidFallbackSloDecision` with `breached` flag set when
// rate > SLO. Reuse the production mode flag from typed_mutation_audit.
inline bool mid_fallback_slo_breach_live() noexcept {
    using namespace aura::compiler::typed_audit;
    MidFallbackSloInput in{};
    in.production_defaults = production_defaults_active();
    const auto d = evaluate_audit_mid_fallback_slo(in);
    return d.breached;
}

// Issue #2076: posture wal_off under Restricted. The audit WAL is
// expected under production / Restricted / Strict. If WAL is disabled
// AND we're under Restricted/Strict, posture is degraded.
inline bool posture_wal_off_restricted_live(std::uint8_t sandbox_mode) noexcept {
    const auto wal_on =
        aura::core::audit_wal::g_mutation_audit_wal().is_enabled() ||
        aura::core::audit_wal_metrics().audit_wal_enabled.load(std::memory_order_relaxed) != 0;
    if (wal_on)
        return false;
    // Restricted = 1, Strict = 2
    return sandbox_mode == 1 || sandbox_mode == 2;
}

// ── Issue #2660: build SecurityScheduleInput from live signals ───
//
// Caller passes the Evaluator's current sandbox_mode (read from
// effect_sandbox_mode()). The returned input is consumed by
// evaluate_security_schedule() which bumps counters and decides
// admit. Production default denies; soft / sandbox=off stays
// observe-only (counter-only, no deny).
inline SecurityScheduleInput make_security_schedule_input_live(std::uint8_t eval_sandbox_mode,
                                                               bool production_defaults,
                                                               bool soft_mode) noexcept {
    SecurityScheduleInput in;
    in.production_mode = production_defaults;
    in.soft_mode = soft_mode;
    const auto cr = commit_readiness_live_signals();
    in.commit_readiness_would_allow = cr.first;
    in.commit_readiness_hard_reject = cr.second;
    in.capability_deny_storm = capability_deny_storm_live();
    in.mid_fallback_slo_breach = mid_fallback_slo_breach_live();
    in.posture_wal_off_restricted = posture_wal_off_restricted_live(eval_sandbox_mode);
    return in;
}

// ── Issue #2660: typed admit helper ─────────────────────────────
//
// Returns `std::nullopt` if the gate allows (or soft / observe-only);
// otherwise returns a structured `AdmissionRejected: security-schedule:<reason>`
// message mirroring mailbox-hold-starvation (#2587) style. Counters
// always bump via evaluate_security_schedule.
inline std::optional<std::string>
admit_security_schedule(const SecurityScheduleInput& in) noexcept {
    const auto d = evaluate_security_schedule(in);
    if (d.would_allow_new_mutate)
        return std::nullopt;
    if (!in.production_mode || in.soft_mode)
        return std::nullopt; // Soft path: fall through (metrics only).
    return std::string("AdmissionRejected: security-schedule:") +
           std::string(security_schedule_force_reason_name(d.force_reason));
}

} // namespace aura::orch