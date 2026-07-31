// security_health.hh — Issue #2389: single Agent security-health score.
//
// Pure, read-only aggregation of capability effect stats, tenant isolation
// stats, SecurityEvent ring wrap, and WAL posture so Agents can gate
// self-mod without joining four+ query surfaces.
//
// ── Score definition (AC1 / issue body) ──
//
//   health_bp =
//     0.30 * (10000 - effect_deny_rate_bp)
//   + 0.25 * (10000 - isolation_deny_rate_bp)
//   + 0.20 * fence_health_bp
//   + 0.15 * wal_posture_bp
//   + 0.10 * (10000 - wrap_pressure_bp)
//
// Integer form (no float):
//   health_bp = (30*effect_good + 25*iso_good + 20*fence + 15*wal + 10*wrap_good)
//               / 100
//
// Rates: 0 when denominators are 0 (vacuous healthy). Clamp [0, 10000].
//
// fence_health_bp:
//   0 fence hits → 10000. Else 10000 - rate(fence_hits, checks); unexplained
//   hits with checks==0 → 0 (hurts).
//
// wal_posture_bp:
//   Off / non-elevated sandbox → 10000 (WAL optional).
//   Restricted/Strict/multi-tenant elevated → 10000 if SecurityEvent WAL on
//   (10000 if mutation WAL also paired; 7000 if only SE WAL; 0 if SE WAL off).
//
// wrap_pressure_bp:
//   rate(ring_wrap_total, ring_total) vacuous 0 when total==0.
//
// ── force_reason priority when health_bp < health_budget_bp (default 8000) ──
//
//   effect-deny > isolation-deny > epoch-fence > wal-off > ring-wrap > ok
//
// When health_bp >= budget → always "ok".

#ifndef AURA_COMPILER_SECURITY_HEALTH_HH
#define AURA_COMPILER_SECURITY_HEALTH_HH

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace aura::compiler {

inline constexpr int kSecurityHealthIssue = 2389;

struct SecurityHealthSnapshot {
    // Capability effect surface (#1565 / #2072).
    std::uint64_t effect_checks = 0;
    std::uint64_t effect_denied = 0;
    std::uint64_t epoch_fence_hits = 0;
    std::uint64_t effect_grants = 0;
    int sandbox_mode = 0; // 0=Off 1=Restricted 2=Strict

    // Tenant isolation surface (#1566 / #2385).
    std::uint64_t isolation_checks = 0;
    std::uint64_t isolation_violations = 0;
    int isolation_enabled = 0;
    int strict_linked = 0;

    // SecurityEvent ring + WAL (#2075 / #2225 / #2150).
    std::uint64_t ring_total = 0;
    std::uint64_t ring_wrap_total = 0;
    int security_event_wal_enabled = 0;
    int mutation_wal_enabled = 0;
};

struct SecurityHealthResult {
    std::uint64_t health_bp = 10000;
    std::uint64_t health_budget_bp = 8000;
    std::string_view force_reason = "ok";
    // Component mirrors (for query surface).
    std::uint64_t effect_deny_rate_bp = 0;
    std::uint64_t isolation_deny_rate_bp = 0;
    std::uint64_t fence_health_bp = 10000;
    std::uint64_t wal_posture_bp = 10000;
    std::uint64_t wrap_pressure_bp = 0;
    int elevated_posture = 0; // 1 when Restricted/Strict/multi-tenant
    SecurityHealthSnapshot components{};
};

// Default budget 8000 bp (80%). Override: AURA_SECURITY_HEALTH_BUDGET_BP.
[[nodiscard]] inline std::uint64_t security_health_budget_bp() noexcept {
    const char* e = std::getenv("AURA_SECURITY_HEALTH_BUDGET_BP");
    if (e == nullptr || e[0] == '\0')
        return 8000;
    std::uint64_t v = 0;
    for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
        v = v * 10 + static_cast<std::uint64_t>(*p - '0');
    if (v > 10000)
        v = 10000;
    return v;
}

// Rate helper: 0 when denom == 0 (vacuous healthy).
[[nodiscard]] inline std::uint64_t security_rate_bp(std::uint64_t num, std::uint64_t den) noexcept {
    if (den == 0)
        return 0;
    return (num * 10000u) / den;
}

[[nodiscard]] inline bool security_elevated_posture(const SecurityHealthSnapshot& s) noexcept {
    // Restricted(1) / Strict(2), isolation principal set, or Strict-linked.
    return s.sandbox_mode >= 1 || s.isolation_enabled != 0 || s.strict_linked != 0;
}

[[nodiscard]] inline std::uint64_t
compute_fence_health_bp(const SecurityHealthSnapshot& s) noexcept {
    if (s.epoch_fence_hits == 0)
        return 10000;
    // Unexplained fences (no checks yet) hurt fully.
    if (s.effect_checks == 0)
        return 0;
    const auto rate = security_rate_bp(s.epoch_fence_hits, s.effect_checks);
    return 10000 - std::min<std::uint64_t>(rate, 10000);
}

[[nodiscard]] inline std::uint64_t
compute_wal_posture_bp(const SecurityHealthSnapshot& s) noexcept {
    if (!security_elevated_posture(s))
        return 10000; // Soft/Off: WAL optional
    if (s.security_event_wal_enabled == 0)
        return 0; // elevated + SE WAL off
    // SE WAL on: full if mutation WAL paired; partial otherwise (#2150 ideal).
    if (s.mutation_wal_enabled != 0)
        return 10000;
    return 7000;
}

[[nodiscard]] inline std::uint64_t
compute_wrap_pressure_bp(const SecurityHealthSnapshot& s) noexcept {
    if (s.ring_total == 0)
        return 0;
    return std::min<std::uint64_t>(10000, security_rate_bp(s.ring_wrap_total, s.ring_total));
}

// Pure score from a snapshot (no atomics — AC3 read-only).
[[nodiscard]] inline SecurityHealthResult
compute_security_health(const SecurityHealthSnapshot& s) noexcept {
    SecurityHealthResult r;
    r.components = s;
    r.health_budget_bp = security_health_budget_bp();
    r.elevated_posture = security_elevated_posture(s) ? 1 : 0;

    r.effect_deny_rate_bp = security_rate_bp(s.effect_denied, s.effect_checks);
    r.isolation_deny_rate_bp = security_rate_bp(s.isolation_violations, s.isolation_checks);
    r.fence_health_bp = compute_fence_health_bp(s);
    r.wal_posture_bp = compute_wal_posture_bp(s);
    r.wrap_pressure_bp = compute_wrap_pressure_bp(s);

    const auto effect_good = 10000 - std::min<std::uint64_t>(r.effect_deny_rate_bp, 10000);
    const auto iso_good = 10000 - std::min<std::uint64_t>(r.isolation_deny_rate_bp, 10000);
    const auto wrap_good = 10000 - std::min<std::uint64_t>(r.wrap_pressure_bp, 10000);
    const auto fence = std::min<std::uint64_t>(r.fence_health_bp, 10000);
    const auto wal = std::min<std::uint64_t>(r.wal_posture_bp, 10000);

    // Weighted sum in basis points (weights sum to 100).
    r.health_bp =
        (30u * effect_good + 25u * iso_good + 20u * fence + 15u * wal + 10u * wrap_good) / 100u;
    if (r.health_bp > 10000)
        r.health_bp = 10000;

    if (r.health_bp >= r.health_budget_bp) {
        r.force_reason = "ok";
        return r;
    }
    // Priority when below budget (issue AC2).
    if (r.effect_deny_rate_bp > 0)
        r.force_reason = "effect-deny";
    else if (r.isolation_deny_rate_bp > 0)
        r.force_reason = "isolation-deny";
    else if (s.epoch_fence_hits > 0)
        r.force_reason = "epoch-fence";
    else if (r.elevated_posture != 0 && r.wal_posture_bp < 10000)
        r.force_reason = "wal-off";
    else if (r.wrap_pressure_bp > 0)
        r.force_reason = "ring-wrap";
    else
        r.force_reason = "ok";
    return r;
}

} // namespace aura::compiler

#endif // AURA_COMPILER_SECURITY_HEALTH_HH
