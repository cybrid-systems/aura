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

#include <algorithm>
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
[[nodiscard]] inline bool
has_mailbox_starvation(const MutationConcurrencyHealthSnapshot& s) noexcept {
    // #2511 hold-exit starvation + #2551 hard residual feed the same signal.
    return s.mailbox_defer_starvation_total > 0 || s.mailbox_deferred_depth > 0 ||
           s.mailbox_hold_exit_starvation_total > 0 || s.mailbox_hold_starvation_hard_total > 0 ||
           s.agent_throttle_for_mailbox_starvation != 0;
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

} // namespace aura::compiler

#endif // AURA_COMPILER_MUTATION_CONCURRENCY_HEALTH_HH
