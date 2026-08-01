// type_system_health.hh — Issue #2350: single Agent type-system health score.
//                       Issue #2462: next-action + repair_nodes closed-loop.
//
// Pure, read-only aggregation of four existing observability surfaces so Agents
// can gate mutate aggressiveness without joining 20+ schema keys.
//
// ── Score definition (AC1) ──
//
//   health_bp =
//     0.25 * coercion_provenance_completeness_bp
//   + 0.25 * (10000 - timeout_reject_rate_bp)
//   + 0.25 * (10000 - linear_pin_miss_rate_bp)
//   + 0.25 * layered_dce_efficiency_bp
//
// Rates are 0 when denominators are 0 (vacuous healthy). Clamp to [0, 10000].
// Equal quarter weights; env override deferred (weights fixed at 25% each).
//
// ── force_reason priority when health_bp < health_budget_bp (default 8000) ──
//
//   timeout-reject > pin-miss > provenance-miss > castop-density > ok
//
// When health_bp >= budget → always "ok".

#ifndef AURA_COMPILER_TYPE_SYSTEM_HEALTH_HH
#define AURA_COMPILER_TYPE_SYSTEM_HEALTH_HH

#include "compiler/observability_metrics.h"
#include "basis_points.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string_view>

// Process-wide counters live in module / header units imported by query TU.
// Forward-load via the same symbols layered/castop/timeout queries use.

namespace aura::compiler {

// Optional module-visible atomics (defined in coercion_map / lifetime_pin /
// optimization_passes). Declared extern-style via includes at call site;
// this header only documents the pure scoring pure function taking snapshots.

struct TypeSystemHealthSnapshot {
    std::uint64_t provenance_completeness_bp = 10000;
    std::uint64_t timeout_reject_rate_bp = 0;
    std::uint64_t linear_pin_miss_rate_bp = 0;
    std::uint64_t layered_dce_efficiency_bp = 10000;
    std::uint64_t castop_density_bp = 0;
    std::uint64_t castop_density_budget_bp = 1500;
    std::uint64_t castop_over_budget_total = 0;
    // Raw component counters (for component-* query mirrors).
    std::uint64_t timeout_reject_total = 0;
    std::uint64_t timeout_full_solve_total = 0;
    std::uint64_t pin_miss_total = 0;
    std::uint64_t pin_total = 0;
    std::uint64_t layered_elided_total = 0;
    std::uint64_t dce_pipeline_runs = 0;
};

struct TypeSystemHealthResult {
    std::uint64_t health_bp = 10000;
    std::uint64_t health_budget_bp = 8000;
    std::string_view force_reason = "ok";
    TypeSystemHealthSnapshot components{};
};

// ── Issue #2462: next-action decision (pure; no solve side effects) ──
//
// Priority (highest first) — Agent closed-loop without multi-query stitch:
//   rollback        — hard-gate / production reject signal
//   full-solve      — truncated_reverify || TIMEOUT escalated / incomplete blame
//   expand-dirty    — non-empty repair_nodes / suggested_roots / unresolved
//   annotate-dynamic— castop over budget while type status otherwise ok
//   ok              — healthy empty / clean snap
//
// Codes (stable int alias for hash tables):
//   0=ok 1=annotate-dynamic 2=expand-dirty 3=full-solve 4=rollback
enum class TypeSystemNextAction : std::uint8_t {
    Ok = 0,
    AnnotateDynamic = 1,
    ExpandDirty = 2,
    FullSolve = 3,
    Rollback = 4,
};

struct TypeSystemNextActionInput {
    // From compute_type_system_health.
    bool health_ok = true; // health_bp >= budget
    std::string_view force_reason = "ok";
    // SolverSnapshot-shaped pure inputs (0=SOLVED, 1=CONFLICT, 2=TIMEOUT).
    std::uint8_t solve_status = 0;
    bool truncated_reverify = false;
    bool blame_complete = true;
    bool production_escalated = false;
    std::size_t repair_nodes_count = 0;
    std::size_t suggested_roots_count = 0;
    std::size_t unresolved_count = 0;
    // CastOp density residual (#2287 / #2459).
    bool castop_over_budget = false;
    // Hard-gate / production reject (e.g. type_repair hard-reject status 99).
    bool hard_gate_reject = false;
    bool production_defaults = false;
};

struct TypeSystemNextActionResult {
    TypeSystemNextAction action = TypeSystemNextAction::Ok;
    std::string_view action_str = "ok";
    std::uint8_t action_code = 0;
};

[[nodiscard]] inline std::string_view type_system_next_action_str(TypeSystemNextAction a) noexcept {
    switch (a) {
        case TypeSystemNextAction::Rollback:
            return "rollback";
        case TypeSystemNextAction::FullSolve:
            return "full-solve";
        case TypeSystemNextAction::ExpandDirty:
            return "expand-dirty";
        case TypeSystemNextAction::AnnotateDynamic:
            return "annotate-dynamic";
        case TypeSystemNextAction::Ok:
        default:
            return "ok";
    }
}

// Pure decision table (AC5: identical inputs → identical output; no atomics).
[[nodiscard]] inline TypeSystemNextActionResult
decide_type_system_next_action(const TypeSystemNextActionInput& in) noexcept {
    TypeSystemNextActionResult r;
    auto set = [&](TypeSystemNextAction a) {
        r.action = a;
        r.action_code = static_cast<std::uint8_t>(a);
        r.action_str = type_system_next_action_str(a);
    };

    // 1) rollback — hard-gate / production reject.
    if (in.hard_gate_reject)
        return (set(TypeSystemNextAction::Rollback), r);

    // 2) full-solve — truncated reverify or TIMEOUT escalated incomplete
    //    under production (AC2).
    const bool timeout = in.solve_status == 2;
    const bool conflict = in.solve_status == 1;
    if (in.truncated_reverify)
        return (set(TypeSystemNextAction::FullSolve), r);
    if (timeout && (in.production_escalated || in.production_defaults))
        return (set(TypeSystemNextAction::FullSolve), r);
    if (in.production_defaults && !in.blame_complete &&
        (timeout || conflict || in.unresolved_count > 0 || in.repair_nodes_count > 0))
        return (set(TypeSystemNextAction::FullSolve), r);
    if (in.force_reason == "timeout-reject")
        return (set(TypeSystemNextAction::FullSolve), r);

    // 3) expand-dirty — concrete repair set / TIMEOUT graph available (AC3).
    if (in.repair_nodes_count > 0 || in.suggested_roots_count > 0 ||
        (timeout && in.unresolved_count > 0) || (conflict && in.unresolved_count > 0))
        return (set(TypeSystemNextAction::ExpandDirty), r);

    // 4) annotate-dynamic — density over budget, type otherwise clean (AC4).
    if (in.castop_over_budget && in.solve_status == 0 && in.blame_complete &&
        !in.truncated_reverify)
        return (set(TypeSystemNextAction::AnnotateDynamic), r);
    if (in.force_reason == "castop-density" && in.solve_status == 0)
        return (set(TypeSystemNextAction::AnnotateDynamic), r);

    // 5) ok — healthy empty / clean (AC1).
    if (in.health_ok && in.force_reason == "ok" && in.solve_status == 0 && !in.truncated_reverify &&
        in.unresolved_count == 0)
        return (set(TypeSystemNextAction::Ok), r);

    // Below-budget residual without typed repair set: still expand/ok map.
    if (!in.health_ok) {
        if (in.force_reason == "pin-miss" || in.force_reason == "provenance-miss")
            return (set(TypeSystemNextAction::ExpandDirty), r);
    }
    return (set(TypeSystemNextAction::Ok), r);
}

// Default budget 8000 bp (80%). Override: AURA_TYPE_SYSTEM_HEALTH_BUDGET_BP.
[[nodiscard]] inline std::uint64_t type_system_health_budget_bp() noexcept {
    const char* e = std::getenv("AURA_TYPE_SYSTEM_HEALTH_BUDGET_BP");
    if (e == nullptr || e[0] == '\0')
        return 8000;
    std::uint64_t v = 0;
    for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
        v = v * 10 + static_cast<std::uint64_t>(*p - '0');
    if (v > 10000)
        v = 10000;
    return v;
}

// Pure score from a snapshot (no atomics — AC3 read-only).
[[nodiscard]] inline TypeSystemHealthResult
compute_type_system_health(const TypeSystemHealthSnapshot& s) noexcept {
    TypeSystemHealthResult r;
    r.components = s;
    r.health_budget_bp = type_system_health_budget_bp();

    const auto prov = std::min<std::uint64_t>(s.provenance_completeness_bp, 10000);
    const auto to_good = 10000 - std::min<std::uint64_t>(s.timeout_reject_rate_bp, 10000);
    const auto pin_good = 10000 - std::min<std::uint64_t>(s.linear_pin_miss_rate_bp, 10000);
    const auto dce = std::min<std::uint64_t>(s.layered_dce_efficiency_bp, 10000);

    // Equal 25% weights via integer quarter sum (no float).
    r.health_bp = (prov + to_good + pin_good + dce) / 4;

    if (r.health_bp >= r.health_budget_bp) {
        r.force_reason = "ok";
        return r;
    }
    // Priority when below budget (AC2).
    if (s.timeout_reject_rate_bp > 0)
        r.force_reason = "timeout-reject";
    else if (s.linear_pin_miss_rate_bp > 0)
        r.force_reason = "pin-miss";
    else if (s.provenance_completeness_bp < 10000)
        r.force_reason = "provenance-miss";
    else if (s.castop_density_bp > s.castop_density_budget_bp || s.castop_over_budget_total > 0)
        r.force_reason = "castop-density";
    else
        r.force_reason = "ok";
    return r;
}

// Rate helper: 0 when denom == 0 (vacuous healthy / no reject signal).
[[nodiscard]] inline std::uint64_t rate_bp(std::uint64_t num, std::uint64_t den) noexcept {
    if (den == 0)
        return 0;
    return (num * 10000u) / den;
}

// Layered DCE efficiency: vacuous 10000 when no runs and no elisions;
// else min(10000, elided*10000/max(1,runs)) — at least one elision per run
// saturates to healthy.
[[nodiscard]] inline std::uint64_t layered_dce_efficiency_bp(std::uint64_t elided,
                                                             std::uint64_t runs) noexcept {
    if (runs == 0 && elided == 0)
        return 10000;
    if (runs == 0)
        return 10000; // elisions without runs counter still count healthy
    if (elided == 0)
        return 0;
    const auto v = (elided * 10000u) / runs;
    return v > 10000 ? 10000 : v;
}

} // namespace aura::compiler

#endif // AURA_COMPILER_TYPE_SYSTEM_HEALTH_HH
