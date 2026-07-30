// type_system_health.hh — Issue #2350: single Agent type-system health score.
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
