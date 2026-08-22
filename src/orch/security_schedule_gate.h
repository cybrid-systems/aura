// src/orch/security_schedule_gate.h — Issue #2590 / #2947
//
// Pure security schedule gate that decides whether the orch / agent-body
// should ADMIT a NEW mutate. Synthesizes commit_readiness (#2553),
// capability deny rate (#2534 trail), mid-fallback SLO breach,
// posture wal_off under Restricted (#2076), and mailbox under-boundary
// wait p99 / starvation throttle (#2903 / #2551 / #2947). Soft /
// sandbox=off is observe-only (counted but never denies) — production
// default denies new mutate when gate flips false (mailbox already
// enqueued is not killed — additive over existing admission).
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
//
// Issue #2947: mailbox_hold_slo is lowest priority among deny reasons so
// it never masks commit_not_ready / deny_storm / mid_fallback_slo /
// posture_degraded / wal_append_fail_breach (#3211). #2587 mutate reject
// sites remain independent defense-in-depth (not weakened).
//
// Issue #3211: production WAL append-fail SLO would_arm_degraded
// (`wal-append-fail-breach`) hard-denies the next mutate. #3056 only
// armed posture; this residual closes admit. Soft / WAL-off never
// hard-deny. Overflow ring (#3109) stays process-local remedy.
//
// Issue #3002: fill_mailbox_hold_slo_live_ is the SSOT live sample for
// p99 + throttle (#2958 cancel reuses the same loads via
// sample_mailbox_hold_slo_live). Production + mailbox_hold_slo_signal +
// live outermost holder → one-shot request_hold_budget_cancel (no
// double-arm). Quiet path remains two relaxed loads.

#pragma once

#include "core/audit_wal_metrics.h"                // #2076 wal_off posture gauge
#include "core/capability_model.hh"                // #2534 capability deny storm window
#include "core/mutation_audit_wal.hh"              // audit_wal_enabled (#2076)
#include "core/security_event_wal.hh"              // #3211 SE WAL fail/persisted loads
#include "core/wal_append_fail_slo.h"              // #3211 would_arm_degraded
#include "compiler/audit_mid_fallback_slo.h"       // #2594 mid-fallback SLO
#include "compiler/mutation_concurrency_health.hh" // #2903/#2947 wait SLO
#include "compiler/typed_mutation_audit.h" // #2553 commit_readiness; production_defaults_active
#include "serve/multi_fiber_mailbox.h"     // #2947 p99 + throttle live loads

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Issue #3244: existing #3018/#3020 overflow counters (C ABI, no extra bus).
extern "C" std::uint64_t aura_engine_metrics_hash_overflow_total(void);
extern "C" std::uint64_t aura_query_hash_overflow_total(void);

namespace aura::orch {

// Issue #2947: mailbox under-boundary wait / starvation throttle face.
inline constexpr int kSecurityScheduleMailboxHoldSloIssue = 2947;
// Issue #3211: production WAL append-fail SLO → schedule deny.
inline constexpr int kSecurityScheduleWalAppendFailIssue = 3211;
// Issue #3244: production engine/query hash overflow → posture + schedule
// observe (no hard admit deny yet; tighten later).
inline constexpr int kSecurityScheduleMetricsHashOverflowIssue = 3244;

// ── Decision types ───────────────────────────────────────────────
enum class SecurityScheduleForceReason : std::uint8_t {
    ok = 0,
    commit_not_ready = 1,
    deny_storm = 2,
    mid_fallback_slo = 3,
    posture_degraded = 4,
    mailbox_hold_slo = 5,             // #2947 under-boundary wait p99 / throttle
    wal_append_fail_breach = 6,       // #3211 production WAL append-fail SLO
    metrics_hash_overflow_breach = 7, // #3244 observe-only (no admit deny)
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
        case SecurityScheduleForceReason::mailbox_hold_slo:
            return "mailbox-hold-slo";
        case SecurityScheduleForceReason::wal_append_fail_breach:
            return "wal-append-fail-breach";
        case SecurityScheduleForceReason::metrics_hash_overflow_breach:
            return "metrics-hash-overflow-breach";
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
    // Issue #2947: under-boundary wait p99 (µs) from #2903 hist face.
    // Zero when no samples — quiet path does not walk hist.
    std::uint64_t mailbox_wait_p99_us = 0;
    // Issue #2947: agent_throttle_for_mailbox_starvation (#2551/#2587).
    bool mailbox_starvation_throttled = false;
    // Issue #2947: SLO threshold (µs). 0 disables p99 latency arm.
    // Live path fills from mailbox_under_boundary_wait_slo_us()
    // (default 100 ms; AURA_MAILBOX_UNDER_BOUNDARY_WAIT_SLO_US).
    std::uint64_t mailbox_wait_slo_us = 0;
    // Production default: AURA_SECURITY_HARD or production profile.
    bool production_mode = false;
    // Soft mode: AURA_SECURITY_SOFT or sandbox=off → observe only.
    bool soft_mode = false;
    // Issue #3211: decide_wal_append_fail_slo.would_arm_degraded
    // (production + WAL enabled + consecutive/rate SLO). Soft never
    // arms. Live fill uses existing g_* SLO counters (no extra bus).
    bool wal_append_fail_would_arm = false;
    // Issue #3244: engine:metrics / query:* hash overflow in production.
    // Live fill uses existing overflow counters (no extra bus). Soft never
    // arms. Observe+posture first — decide does not hard-deny admit.
    bool metrics_hash_overflow_would_arm = false;
};

// Issue #2947 / #3002: pure predicate — p99 ≥ SLO (SLO>0) or throttle flag.
// Same boolean as mf_mailbox::mailbox_hold_slo_live_signal (SSOT).
[[nodiscard]] inline bool mailbox_hold_slo_signal(const SecurityScheduleInput& in) noexcept {
    return aura::serve::mf_mailbox::mailbox_hold_slo_live_signal(
        in.mailbox_wait_p99_us, in.mailbox_wait_slo_us, in.mailbox_starvation_throttled);
}

// ── Pure decision (#2590 AC1: same input → same output, no atomics) ──
//
// Priority order (first match wins):
//   1. commit-not-ready   (commit_readiness_hard_reject && production && !soft)
//   2. deny-storm         (capability_deny_storm && production && !soft)
//   3. mid-fallback-slo   (mid_fallback_slo_breach && production && !soft)
//   4. posture-degraded   (posture_wal_off_restricted && production && !soft)
//   5. wal-append-fail-breach (#3211 would_arm_degraded && production && !soft)
//   6. mailbox-hold-slo   (p99≥SLO || throttle; #2947 — never masks 1–5)
//   7. metrics-hash-overflow-breach (#3244 observe; would_allow stays true)
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
    // Issue #3211: production WAL append-fail SLO would_arm_degraded.
    // Soft / WAL-off never set the input true (live helper). Does not
    // mask 1–4. Mailbox wait stays below this forensic stop.
    if (enforce && in.wal_append_fail_would_arm) {
        d.would_allow_new_mutate = false;
        d.force_reason = SecurityScheduleForceReason::wal_append_fail_breach;
        return d;
    }
    // Issue #2947: lowest priority so commit_not_ready etc. always win.
    if (enforce && mailbox_hold_slo_signal(in)) {
        d.would_allow_new_mutate = false;
        d.force_reason = SecurityScheduleForceReason::mailbox_hold_slo;
        return d;
    }
    // Issue #3244: lowest, after real denies. Surface force_reason so
    // Agents see residual catalog overflow; do not deny admit yet
    // (observe+posture first; tighten later).
    if (enforce && in.metrics_hash_overflow_would_arm) {
        d.force_reason = SecurityScheduleForceReason::metrics_hash_overflow_breach;
    }
    return d;
}

// ── Process-wide counters (#2590 AC2 + AC5 / #2947) ───────────────
struct OrchSecurityScheduleCounters {
    std::atomic<std::uint64_t> checks_total{0};
    std::atomic<std::uint64_t> deny_total{0};
    std::atomic<std::uint64_t> allow_total{0};
    std::atomic<std::uint64_t> deny_commit_not_ready_total{0};
    std::atomic<std::uint64_t> deny_deny_storm_total{0};
    std::atomic<std::uint64_t> deny_mid_fallback_slo_total{0};
    std::atomic<std::uint64_t> deny_posture_degraded_total{0};
    // Issue #2947: production deny face for under-boundary wait / throttle.
    std::atomic<std::uint64_t> deny_mailbox_hold_slo_total{0};
    // Issue #3211: production deny face for WAL append-fail SLO breach.
    std::atomic<std::uint64_t> deny_wal_append_fail_breach_total{0};
    // Issue #3244: observe-only overflow arm (would_allow stays true).
    std::atomic<std::uint64_t> observe_metrics_hash_overflow_total{0};
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
        if (d.force_reason == SecurityScheduleForceReason::metrics_hash_overflow_breach)
            c.observe_metrics_hash_overflow_total.fetch_add(1, std::memory_order_relaxed);
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
            case SecurityScheduleForceReason::mailbox_hold_slo:
                c.deny_mailbox_hold_slo_total.fetch_add(1, std::memory_order_relaxed);
                break;
            case SecurityScheduleForceReason::wal_append_fail_breach:
                c.deny_wal_append_fail_breach_total.fetch_add(1, std::memory_order_relaxed);
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
    c.deny_mailbox_hold_slo_total.store(0, std::memory_order_relaxed);
    c.deny_wal_append_fail_breach_total.store(0, std::memory_order_relaxed);
    c.observe_metrics_hash_overflow_total.store(0, std::memory_order_relaxed);
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
    aura::compiler::MidFallbackSloInput in{};
    in.production_defaults = production_defaults_active();
    const auto d = aura::compiler::evaluate_audit_mid_fallback_slo(in);
    return d.breached;
}

// Issue #2076: posture wal_off under Restricted. The audit WAL is
// expected under production / Restricted / Strict. If WAL is disabled
// AND we're under Restricted/Strict, posture is degraded.
inline bool posture_wal_off_restricted_live(std::uint8_t sandbox_mode) noexcept {
    const auto wal_on = aura::core::audit_wal::g_mutation_audit_wal().is_enabled() ||
                        aura::core::audit_wal::g_audit_wal_metrics().audit_wal_enabled.load(
                            std::memory_order_relaxed) != 0;
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
// Issue #2947: two relaxed loads (p99 + throttle flag) — no hist walk.
// Quiet path (no samples, throttle=0) is the same cost as a pair of
// loads; matches #2903 AC zero-extra-work when depth==0.
inline void fill_mailbox_hold_slo_live_(SecurityScheduleInput& in) noexcept {
    // Issue #3002: SSOT sample (same two relaxed loads as #2958).
    aura::serve::mf_mailbox::sample_mailbox_hold_slo_live(
        in.mailbox_wait_p99_us, in.mailbox_starvation_throttled, in.mailbox_wait_slo_us);
    // Production + signal + live holder → one-shot cancel (reuse #2958
    // CAS; do not double-arm). Quiet: signal false → no extra work.
    if (mailbox_hold_slo_signal(in))
        aura::serve::mf_mailbox::maybe_mailbox_defer_slo_hold_cancel();
}

// Issue #3211: live WAL append-fail SLO would_arm_degraded.
// Quiet / WAL-off / no-fail: two relaxed loads on SLO counters then
// return false (AC5). After a real append miss, load existing WAL
// fail/persisted totals + pure decide_wal_append_fail_slo (no extra
// bus; does not bump SLO checks_total — query:security-posture owns
// that evaluate). Soft_mode → would_arm_degraded stays false.
inline bool wal_append_fail_would_arm_live(bool production_defaults, bool soft_mode) noexcept {
    auto& c = aura::core::wal_slo::g_wal_append_fail_slo_counters;
    if (c.consecutive.load(std::memory_order_relaxed) == 0 &&
        c.combined_fail_total.load(std::memory_order_relaxed) == 0)
        return false;
    const auto& am = aura::core::audit_wal::g_audit_wal_metrics();
    const auto& sm = aura::core::security_event_wal::g_security_event_wal_metrics();
    const auto d = aura::core::wal_slo::decide_wal_append_fail_slo(
        aura::core::wal_slo::make_wal_append_fail_slo_input(
            am.audit_wal_append_fail_total.load(std::memory_order_relaxed),
            sm.security_event_wal_append_fail_total.load(std::memory_order_relaxed),
            am.audit_record_persisted_total.load(std::memory_order_relaxed),
            sm.security_event_persisted_total.load(std::memory_order_relaxed),
            /*wal_enabled=*/true, production_defaults, soft_mode));
    return d.would_arm_degraded;
}

// Issue #3244: live engine:metrics / query:* hash overflow arm.
// Soft / !production: false with no counter loads (zero extra).
// Production: two relaxed loads of existing overflow totals (no extra bus).
[[nodiscard]] inline bool metrics_hash_overflow_would_arm_live(bool production_defaults,
                                                               bool soft_mode) noexcept {
    if (!production_defaults || soft_mode)
        return false;
    return ::aura_engine_metrics_hash_overflow_total() > 0 ||
           ::aura_query_hash_overflow_total() > 0;
}

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
    // Issue #2947: mailbox under-boundary wait / throttle into same gate.
    fill_mailbox_hold_slo_live_(in);
    // Issue #3211: WAL append-fail SLO would_arm → schedule deny.
    in.wal_append_fail_would_arm = wal_append_fail_would_arm_live(production_defaults, soft_mode);
    // Issue #3244: metrics hash overflow observe (does not deny admit).
    in.metrics_hash_overflow_would_arm =
        metrics_hash_overflow_would_arm_live(production_defaults, soft_mode);
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