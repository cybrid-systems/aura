// audit_mid_fallback_slo.h — Issue #2594: audit mid-fallback 率 SLO
// → security-health 降级标志.
//
// Pure gate: rate_bp = 10000 * fallback_gen / max(1, contextual_total).
// When production_defaults_active && rate > SLO → arm
// security_health_degraded / posture key `mid-fallback-slo-breach`.
// Soft / sandbox=off: observe only (AC4 — never arm degraded).
//
// SLO env override: AURA_MID_FALLBACK_SLO_BP (default 500 = 5%).
//
// Counters always bump (checks_total++); soft-breach and arm-degraded
// arms are independent per #2389 health-arm pattern (mirrors
// decide_security_schedule / evaluate_security_schedule from #2590).
//
// Pairs with src/compiler/typed_mutation_audit.h:
//   - audit_mid_fallback_gen_total (AC1 counter source)
//   - contextual_total             (denominator; AC1 rate computation)
//   - production_defaults_active() (AC2 / AC4 mode gate)
//
// Header form so evaluator_primitives_security.cpp + tests can include
// without module churn (matches #2389 / #2590 / #2553 pattern).

#ifndef AURA_COMPILER_AUDIT_MID_FALLBACK_SLO_H
#define AURA_COMPILER_AUDIT_MID_FALLBACK_SLO_H

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace aura::compiler {

inline constexpr int kAuditMidFallbackSloIssue = 2594;

// Issue #2594: AURA_MID_FALLBACK_SLO_BP env override (default 500 = 5%).
// Lazy-init; digit parse matches AURA_SECURITY_HEALTH_BUDGET_BP (#2389) and
// AURA_LINEAR_CROSS_CLOSURE_HARD (#2563) pattern.
[[nodiscard]] inline std::uint64_t audit_mid_fallback_slo_bp() noexcept {
    static const std::uint64_t cached = []() noexcept -> std::uint64_t {
        const char* e = std::getenv("AURA_MID_FALLBACK_SLO_BP");
        if (e == nullptr || e[0] == '\0')
            return 500; // default 5%
        std::uint64_t v = 0;
        for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
            v = v * 10 + static_cast<std::uint64_t>(*p - '0');
        if (v > 10000)
            v = 10000;
        return v;
    }();
    return cached;
}

// Issue #2594: rate helper — basis points (0..10000). Vacuous when
// contextual_total == 0 (no audits yet → no rate to compute).
[[nodiscard]] inline std::uint64_t
compute_mid_fallback_rate_bp(std::uint64_t fallback_gen, std::uint64_t contextual_total) noexcept {
    if (contextual_total == 0)
        return 0;
    return (fallback_gen * 10000u) / contextual_total;
}

// Issue #2594: pure input. Same input → same output (no atomics).
struct MidFallbackSloInput {
    std::uint64_t fallback_gen = 0;     // audit_mid_fallback_gen_total
    std::uint64_t contextual_total = 0; // typed_mutation_audit_contextual_total
    bool production_defaults = false;   // production_defaults_active()
    // Soft mode = !production_defaults OR AURA_SANDBOX=off (never arm
    // degraded; observe only — AC4).
    bool soft_mode = false;
};

struct MidFallbackSloDecision {
    std::uint64_t rate_bp = 0;
    std::uint64_t slo_bp = 500;
    bool breached = false;
    bool would_arm_degraded = false;
    std::string_view force_reason = "ok";
    // 0=ok, 1=soft-breach-observe, 2=mid-fallback-slo-breach
    int force_reason_code = 0;
};

// Pure decision table (AC5 — identical inputs → identical output).
//   soft_mode || !production_defaults → observe only (never arm)
//   production_defaults && rate > SLO → arm degraded
//   production_defaults && rate <= SLO → ok
//   any case where rate <= SLO → breached=false
[[nodiscard]] inline MidFallbackSloDecision
decide_audit_mid_fallback_slo(const MidFallbackSloInput& in) noexcept {
    MidFallbackSloDecision d;
    d.slo_bp = audit_mid_fallback_slo_bp();
    d.rate_bp = compute_mid_fallback_rate_bp(in.fallback_gen, in.contextual_total);
    d.breached = d.rate_bp > d.slo_bp;

    if (in.soft_mode || !in.production_defaults) {
        // AC4: Soft / sandbox=off → observe only.
        d.would_arm_degraded = false;
        d.force_reason = d.breached ? "soft-breach-observe" : "ok";
        d.force_reason_code = d.breached ? 1 : 0;
        return d;
    }
    // AC2: Production defaults active.
    d.would_arm_degraded = d.breached;
    d.force_reason = d.breached ? "mid-fallback-slo-breach" : "ok";
    d.force_reason_code = d.breached ? 2 : 0;
    return d;
}

struct AuditMidFallbackSloCounters {
    std::atomic<std::uint64_t> checks_total{0};
    std::atomic<std::uint64_t> breach_total{0};
    std::atomic<std::uint64_t> soft_breach_observe_total{0};
    std::atomic<std::uint64_t> arm_degraded_total{0};
    std::atomic<std::uint64_t> last_rate_bp{0};
    std::atomic<std::uint64_t> last_slo_bp{500};
    std::atomic<std::uint32_t> last_breached{0};
    std::atomic<std::uint32_t> last_would_arm_degraded{0};
};

inline AuditMidFallbackSloCounters g_audit_mid_fallback_slo_counters{};

// Pure w.r.t. inputs once copied; bumps process-wide counters atomically.
// Same pattern as evaluate_security_schedule (#2590) and commit_readiness
// (#2553): callers that want hermetic tests pass MidFallbackSloInput
// directly to decide_audit_mid_fallback_slo without this helper.
inline MidFallbackSloDecision
evaluate_audit_mid_fallback_slo(const MidFallbackSloInput& in) noexcept {
    auto& c = g_audit_mid_fallback_slo_counters;
    c.checks_total.fetch_add(1, std::memory_order_relaxed);
    const auto d = decide_audit_mid_fallback_slo(in);
    c.last_rate_bp.store(d.rate_bp, std::memory_order_relaxed);
    c.last_slo_bp.store(d.slo_bp, std::memory_order_relaxed);
    c.last_breached.store(d.breached ? 1 : 0, std::memory_order_relaxed);
    c.last_would_arm_degraded.store(d.would_arm_degraded ? 1 : 0, std::memory_order_relaxed);
    if (d.would_arm_degraded) {
        c.arm_degraded_total.fetch_add(1, std::memory_order_relaxed);
        c.breach_total.fetch_add(1, std::memory_order_relaxed);
    } else if (d.breached) {
        c.soft_breach_observe_total.fetch_add(1, std::memory_order_relaxed);
        c.breach_total.fetch_add(1, std::memory_order_relaxed);
    }
    return d;
}

inline void reset_audit_mid_fallback_slo_for_test() noexcept {
    auto& c = g_audit_mid_fallback_slo_counters;
    c.checks_total.store(0, std::memory_order_relaxed);
    c.breach_total.store(0, std::memory_order_relaxed);
    c.soft_breach_observe_total.store(0, std::memory_order_relaxed);
    c.arm_degraded_total.store(0, std::memory_order_relaxed);
    c.last_rate_bp.store(0, std::memory_order_relaxed);
    c.last_slo_bp.store(audit_mid_fallback_slo_bp(), std::memory_order_relaxed);
    c.last_breached.store(0, std::memory_order_relaxed);
    c.last_would_arm_degraded.store(0, std::memory_order_relaxed);
}

} // namespace aura::compiler

#endif // AURA_COMPILER_AUDIT_MID_FALLBACK_SLO_H