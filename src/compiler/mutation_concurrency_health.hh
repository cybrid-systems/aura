// mutation_concurrency_health.hh — Issue #2379: single Agent mutation-concurrency
// health score (hold + steal + residual + mailbox + densify).
//
// Pure, read-only aggregation of existing process counters so Agents can
// stop / degrade mutate without joining five subsystem schemas:
//   query:mutation-boundary-hold-stats
//   query:orchestration-steal-outermost-stats
//   query:gc-defer-reason-stats
//   query:mf-mailbox-stats
//   query:lifetime-contract-snapshot
//
// ── Score definition (AC1) ──
//
//   Start health_bp = 10000.
//
//   Hard penalties (each applied at most once when signal non-zero):
//     steal-mismatch (force_deopt | hard_fail)     −4000
//     residual-defer (hard residual | residual-on-steal) −2500
//     densify-fail (consistency_fail | last axis 0) −3000
//
//   Soft penalties (capped; reduce score without zeroing alone):
//     hold-slo / over-budget:  min(2000, (slo_fail + over_budget) * 100)
//     mailbox-starvation:      min(1500, starvation * 200 + depth * 50)
//
//   Clamp result to [0, 10000]. Vacuous process (all zeros / last densify ok)
//   → health_bp = 10000.
//
// ── force_reason priority (AC2) ──
//
//   steal-mismatch > residual-defer > densify-fail > hold-slo >
//   mailbox-starvation > none
//
// Hard signals set force_reason even if soft score still ≥ budget.
// When no hard/soft signal → "none". Alias "ok" not used (Agent contract
// prefers "none" for clean process per issue AC3).

#ifndef AURA_COMPILER_MUTATION_CONCURRENCY_HEALTH_HH
#define AURA_COMPILER_MUTATION_CONCURRENCY_HEALTH_HH

#include "typed_mutation_audit.h" // production_defaults_active (#2985 admit)

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace aura::compiler {

struct MutationConcurrencyHealthSnapshot {
    // Steal / MutationSafetySnapshot (#2310 / #2346).
    std::uint64_t steal_force_deopt_total = 0;
    std::uint64_t steal_hard_fail_total = 0;
    // Residual GC defer (#2314 / #2269).
    std::uint64_t residual_defer_cleared_on_steal_total = 0;
    std::uint64_t residual_hard_fail_total = 0;
    // Densify consistency (#2341 / #2376 last-call axes).
    std::uint64_t densify_consistency_fail_total = 0;
    std::uint8_t last_densify_envframe_ok = 1;
    std::uint8_t last_densify_closure_remount_ok = 1;
    // Hold SLO (#2349) / early over-budget (#2313).
    std::uint64_t hold_slo_violation_total = 0;
    std::uint64_t hold_over_budget_total = 0;
    // Mailbox defer SLA (#2312 / #2378 / #2511 / #2551 hold-exit drain).
    std::uint64_t mailbox_defer_starvation_total = 0;
    std::uint64_t mailbox_deferred_depth = 0;
    std::uint64_t mailbox_deferred_mutation_hold_total = 0;
    std::uint64_t mailbox_hold_exit_starvation_total = 0;    // #2511
    std::uint64_t mailbox_hold_starvation_hard_total = 0;    // #2551
    std::uint64_t agent_throttle_for_mailbox_starvation = 0; // #2551 0/1
    // Issue #2903: under-boundary wait max (µs). Soft signal when > SLO.
    std::uint64_t mailbox_under_boundary_wait_us_max = 0; // #2903
};

struct MutationConcurrencyHealthResult {
    std::uint64_t health_bp = 10000;
    std::uint64_t health_budget_bp = 8000;
    std::string_view force_reason = "none";
    // force-reason-code: 0=none 1=steal-mismatch 2=residual-defer
    // 3=densify-fail 4=hold-slo 5=mailbox-starvation
    std::int64_t force_reason_code = 0;
    MutationConcurrencyHealthSnapshot components{};
};

// Default budget 8000 bp (80%). Override: AURA_MUTATION_CONCURRENCY_HEALTH_BUDGET_BP.
[[nodiscard]] inline std::uint64_t mutation_concurrency_health_budget_bp() noexcept {
    const char* e = std::getenv("AURA_MUTATION_CONCURRENCY_HEALTH_BUDGET_BP");
    if (e == nullptr || e[0] == '\0')
        return 8000;
    std::uint64_t v = 0;
    for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
        v = v * 10 + static_cast<std::uint64_t>(*p - '0');
    if (v > 10000)
        v = 10000;
    return v;
}

[[nodiscard]] inline bool has_steal_mismatch(const MutationConcurrencyHealthSnapshot& s) noexcept {
    return s.steal_force_deopt_total > 0 || s.steal_hard_fail_total > 0;
}
[[nodiscard]] inline bool has_residual_defer(const MutationConcurrencyHealthSnapshot& s) noexcept {
    return s.residual_hard_fail_total > 0 || s.residual_defer_cleared_on_steal_total > 0;
}
[[nodiscard]] inline bool has_densify_fail(const MutationConcurrencyHealthSnapshot& s) noexcept {
    return s.densify_consistency_fail_total > 0 || s.last_densify_envframe_ok == 0 ||
           s.last_densify_closure_remount_ok == 0;
}
[[nodiscard]] inline bool has_hold_slo(const MutationConcurrencyHealthSnapshot& s) noexcept {
    return s.hold_slo_violation_total > 0 || s.hold_over_budget_total > 0;
}
// Issue #2903: default under-boundary wait SLO (µs). Override via
// AURA_MAILBOX_UNDER_BOUNDARY_WAIT_SLO_US (0 disables latency soft signal).
[[nodiscard]] inline std::uint64_t mailbox_under_boundary_wait_slo_us() noexcept {
    const char* e = std::getenv("AURA_MAILBOX_UNDER_BOUNDARY_WAIT_SLO_US");
    if (e == nullptr || e[0] == '\0')
        return 100'000; // 100 ms default — long-hold starvation visible
    std::uint64_t v = 0;
    for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
        v = v * 10 + static_cast<std::uint64_t>(*p - '0');
    return v;
}

[[nodiscard]] inline bool
has_mailbox_starvation(const MutationConcurrencyHealthSnapshot& s) noexcept {
    // #2511 hold-exit starvation + #2551 hard residual feed the same signal.
    // #2903: long under-boundary wait (max > SLO) also soft-signals so Agents
    // see silent starvation without stitching latency queries.
    const auto slo = mailbox_under_boundary_wait_slo_us();
    const bool wait_slo_breach = slo != 0 && s.mailbox_under_boundary_wait_us_max > 0 &&
                                 s.mailbox_under_boundary_wait_us_max >= slo;
    return s.mailbox_defer_starvation_total > 0 || s.mailbox_deferred_depth > 0 ||
           s.mailbox_hold_exit_starvation_total > 0 || s.mailbox_hold_starvation_hard_total > 0 ||
           s.agent_throttle_for_mailbox_starvation != 0 || wait_slo_breach;
}

// Pure score from a snapshot (no atomics — AC3 / AC4 read-only).
[[nodiscard]] inline MutationConcurrencyHealthResult
compute_mutation_concurrency_health(const MutationConcurrencyHealthSnapshot& s) noexcept {
    MutationConcurrencyHealthResult r;
    r.components = s;
    r.health_budget_bp = mutation_concurrency_health_budget_bp();

    std::int64_t bp = 10000;

    // Hard penalties.
    if (has_steal_mismatch(s))
        bp -= 4000;
    if (has_residual_defer(s))
        bp -= 2500;
    if (has_densify_fail(s))
        bp -= 3000;

    // Soft penalties (capped).
    const auto hold_hits = s.hold_slo_violation_total + s.hold_over_budget_total;
    if (hold_hits > 0) {
        const auto soft = std::min<std::uint64_t>(2000, hold_hits * 100);
        bp -= static_cast<std::int64_t>(soft);
    }
    if (has_mailbox_starvation(s)) {
        const auto soft = std::min<std::uint64_t>(
            1500, s.mailbox_defer_starvation_total * 200 + s.mailbox_deferred_depth * 50 +
                      s.mailbox_hold_exit_starvation_total * 200 +
                      s.mailbox_hold_starvation_hard_total * 300 +
                      (s.agent_throttle_for_mailbox_starvation != 0 ? 200ull : 0ull));
        bp -= static_cast<std::int64_t>(soft);
    }

    if (bp < 0)
        bp = 0;
    if (bp > 10000)
        bp = 10000;
    r.health_bp = static_cast<std::uint64_t>(bp);

    // force_reason priority (hard first, then soft). Independent of budget
    // so Agents always see the dominant signal (AC2 inject).
    if (has_steal_mismatch(s)) {
        r.force_reason = "steal-mismatch";
        r.force_reason_code = 1;
    } else if (has_residual_defer(s)) {
        r.force_reason = "residual-defer";
        r.force_reason_code = 2;
    } else if (has_densify_fail(s)) {
        r.force_reason = "densify-fail";
        r.force_reason_code = 3;
    } else if (has_hold_slo(s)) {
        r.force_reason = "hold-slo";
        r.force_reason_code = 4;
    } else if (has_mailbox_starvation(s)) {
        r.force_reason = "mailbox-starvation";
        r.force_reason_code = 5;
    } else {
        r.force_reason = "none";
        r.force_reason_code = 0;
    }
    return r;
}

// Issue #2985: production admit close-loop on concurrency health.
// Soft / sandbox=off / test override → observe only. Production +
// (hard force_reason 1–3 OR health_bp < budget) → reject GlobalExclusive.
// Happy path (health_bp==10000 && force_reason_code==0): no extra stores.
inline constexpr int kMutationConcurrencyHealthAdmitIssue = 2985;
inline std::atomic<std::uint64_t> g_mutation_concurrency_health_reject_total{0};
inline std::atomic<std::uint64_t> g_mutation_concurrency_health_soft_observe_total{0};
inline std::atomic<std::uint32_t> g_mutation_concurrency_health_admit_wired{1};
// -1 = use env/production; 0 = force hard; 1 = force Soft (test).
inline std::atomic<std::int32_t> g_mutation_concurrency_health_soft_for_test{-1};

inline void set_mutation_concurrency_health_soft_for_test(bool soft) noexcept {
    g_mutation_concurrency_health_soft_for_test.store(soft ? 1 : 0, std::memory_order_relaxed);
}
inline void reset_mutation_concurrency_health_soft_for_test() noexcept {
    g_mutation_concurrency_health_soft_for_test.store(-1, std::memory_order_relaxed);
}
[[nodiscard]] inline bool is_mutation_concurrency_health_soft_for_test() noexcept {
    return g_mutation_concurrency_health_soft_for_test.load(std::memory_order_relaxed) == 1;
}

[[nodiscard]] inline bool mutation_concurrency_health_soft_mode() noexcept {
    if (is_mutation_concurrency_health_soft_for_test())
        return true;
    // Explicit production face (apply_production_audit_defaults) wins over
    // process AURA_SANDBOX=off — the issues runner always sets the latter.
    if (typed_audit::production_defaults_active())
        return false;
    const char* sandbox = std::getenv("AURA_SANDBOX");
    if (sandbox && sandbox[0] != '\0' && std::string_view(sandbox) == "off")
        return true;
    return true; // Soft default when production flag is off
}

// Thread-local admit snapshot override (AC5 inject / clear). Unset → live.
inline thread_local MutationConcurrencyHealthSnapshot g_health_admit_override_storage{};
inline thread_local bool g_health_admit_override_armed = false;

inline void set_mutation_concurrency_health_admit_snapshot_for_test(
    const MutationConcurrencyHealthSnapshot& s) noexcept {
    g_health_admit_override_storage = s;
    g_health_admit_override_armed = true;
}
inline void clear_mutation_concurrency_health_admit_snapshot_for_test() noexcept {
    g_health_admit_override_armed = false;
    g_health_admit_override_storage = MutationConcurrencyHealthSnapshot{};
}

[[nodiscard]] inline bool mutation_concurrency_health_admit_override_armed() noexcept {
    return g_health_admit_override_armed;
}
[[nodiscard]] inline const MutationConcurrencyHealthSnapshot&
mutation_concurrency_health_admit_override_snapshot() noexcept {
    return g_health_admit_override_storage;
}

enum class MutationConcurrencyHealthAdmitAction : std::uint8_t {
    Allow = 0,
    SoftObserve = 1,
    Reject = 2,
};

// Hard force_reason: steal-mismatch / residual-defer / densify-fail.
// region_concurrent: only hard reasons reject (budget-only still admits).
[[nodiscard]] inline MutationConcurrencyHealthAdmitAction
mutation_concurrency_health_admit_action(const MutationConcurrencyHealthResult& h,
                                         bool region_concurrent = false) noexcept {
    const bool hard_reason = h.force_reason_code >= 1 && h.force_reason_code <= 3;
    const bool under_budget = h.health_bp < h.health_budget_bp;
    const bool degraded = hard_reason || (!region_concurrent && under_budget);
    if (!degraded)
        return MutationConcurrencyHealthAdmitAction::Allow;
    if (mutation_concurrency_health_soft_mode())
        return MutationConcurrencyHealthAdmitAction::SoftObserve;
    return MutationConcurrencyHealthAdmitAction::Reject;
}

// Happy path: Allow → no extra atomics (AC3).
inline void
note_mutation_concurrency_health_admit(MutationConcurrencyHealthAdmitAction a) noexcept {
    if (a == MutationConcurrencyHealthAdmitAction::Allow)
        return;
    if (a == MutationConcurrencyHealthAdmitAction::Reject)
        g_mutation_concurrency_health_reject_total.fetch_add(1, std::memory_order_relaxed);
    else
        g_mutation_concurrency_health_soft_observe_total.fetch_add(1, std::memory_order_relaxed);
}

inline void reset_mutation_concurrency_health_admit_for_test() noexcept {
    g_mutation_concurrency_health_reject_total.store(0, std::memory_order_relaxed);
    g_mutation_concurrency_health_soft_observe_total.store(0, std::memory_order_relaxed);
    reset_mutation_concurrency_health_soft_for_test();
    clear_mutation_concurrency_health_admit_snapshot_for_test();
}

} // namespace aura::compiler

#endif // AURA_COMPILER_MUTATION_CONCURRENCY_HEALTH_HH
