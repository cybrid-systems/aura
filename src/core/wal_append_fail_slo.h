// wal_append_fail_slo.h — Issue #3056: production WAL append_fail
// arms security-posture degraded (fail-open residual).
//
// Shared decision for mutation_audit_wal (#1567) and
// security_event_wal (#2225). After fwrite miss, counters stay
// authoritative; production + WAL enabled + (consecutive >= SLO or
// fail-rate > SLO_BP) → would_arm_degraded + posture key
// `wal-append-fail-breach`. Soft / WAL-off: observe only, never arm.
// Mutation commit stays fail-open (callers keep `(void)append`).
// Issue #3211: security-schedule-gate consumes would_arm_degraded and
// hard-denies the *next* outermost mutate in production.
//
// SLO env:
//   AURA_WAL_APPEND_FAIL_SLO     consecutive fails (default 3)
//   AURA_WAL_APPEND_FAIL_SLO_BP  fail rate bp (default 500 = 5%)
//
// Mirrors #2594 mid-fallback SLO / #2389 health-arm. Header form so
// both WAL TUs + query:security-posture / query:audit-wal-stats share
// one surface (AC5 — no dual-track posture).

#ifndef AURA_CORE_WAL_APPEND_FAIL_SLO_H
#define AURA_CORE_WAL_APPEND_FAIL_SLO_H

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

// C-linkage SSOT used by core/serve (strong in typed_mutation_audit_hooks,
// weak no-op in fiber.cpp). Core must not include typed_audit.h.
extern "C" int aura_production_defaults_active_probe() noexcept;

namespace aura::core::wal_slo {

inline constexpr int kWalAppendFailSloIssue = 3056;

// Consecutive-fail SLO. Default 3 (small conservative window).
[[nodiscard]] inline std::uint64_t wal_append_fail_slo_consecutive() noexcept {
    static const std::uint64_t cached = []() noexcept -> std::uint64_t {
        const char* e = std::getenv("AURA_WAL_APPEND_FAIL_SLO");
        if (e == nullptr || e[0] == '\0')
            return 3;
        std::uint64_t v = 0;
        for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
            v = v * 10 + static_cast<std::uint64_t>(*p - '0');
        if (v == 0)
            v = 1;
        return v;
    }();
    return cached;
}

// Rate SLO in basis points. Default 500 = 5% (matches #2594).
[[nodiscard]] inline std::uint64_t wal_append_fail_slo_bp() noexcept {
    static const std::uint64_t cached = []() noexcept -> std::uint64_t {
        const char* e = std::getenv("AURA_WAL_APPEND_FAIL_SLO_BP");
        if (e == nullptr || e[0] == '\0')
            return 500;
        std::uint64_t v = 0;
        for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
            v = v * 10 + static_cast<std::uint64_t>(*p - '0');
        if (v > 10000)
            v = 10000;
        return v;
    }();
    return cached;
}

[[nodiscard]] inline std::uint64_t
compute_wal_append_fail_rate_bp(std::uint64_t fail_total, std::uint64_t persisted_total) noexcept {
    const auto den = fail_total + persisted_total;
    if (den == 0)
        return 0;
    return (fail_total * 10000u) / den;
}

// Pure input. Same input → same output (no atomics).
struct WalAppendFailSloInput {
    std::uint64_t fail_total = 0;      // mutation + SE append_fail
    std::uint64_t persisted_total = 0; // mutation + SE persisted
    std::uint64_t consecutive = 0;     // current consecutive fail window
    bool wal_enabled = false;          // either WAL is_enabled()
    bool production_defaults = false;  // production_defaults_active()
    // Soft = !production OR AURA_SANDBOX=off (never arm; AC1).
    bool soft_mode = false;
};

struct WalAppendFailSloDecision {
    std::uint64_t rate_bp = 0;
    std::uint64_t slo_bp = 500;
    std::uint64_t slo_consecutive = 3;
    bool breached = false;
    bool would_arm_degraded = false;
    std::string_view force_reason = "ok";
    // 0=ok, 1=soft-breach-observe, 2=wal-append-fail-breach
    int force_reason_code = 0;
};

// Pure decision table (AC5 — identical inputs → identical output).
//   !wal_enabled                         → ok (no residual walk)
//   soft_mode || !production_defaults    → observe only (never arm)
//   production && (consec >= SLO || rate > SLO_BP) → arm
[[nodiscard]] inline WalAppendFailSloDecision
decide_wal_append_fail_slo(const WalAppendFailSloInput& in) noexcept {
    WalAppendFailSloDecision d;
    d.slo_bp = wal_append_fail_slo_bp();
    d.slo_consecutive = wal_append_fail_slo_consecutive();
    d.rate_bp = compute_wal_append_fail_rate_bp(in.fail_total, in.persisted_total);
    if (!in.wal_enabled) {
        d.breached = false;
        d.would_arm_degraded = false;
        d.force_reason = "ok";
        d.force_reason_code = 0;
        return d;
    }
    d.breached =
        (in.consecutive >= d.slo_consecutive) || (in.fail_total > 0 && d.rate_bp > d.slo_bp);
    if (in.soft_mode || !in.production_defaults) {
        d.would_arm_degraded = false;
        d.force_reason = d.breached ? "soft-breach-observe" : "ok";
        d.force_reason_code = d.breached ? 1 : 0;
        return d;
    }
    d.would_arm_degraded = d.breached;
    d.force_reason = d.breached ? "wal-append-fail-breach" : "ok";
    d.force_reason_code = d.breached ? 2 : 0;
    return d;
}

struct WalAppendFailSloCounters {
    std::atomic<std::uint64_t> checks_total{0};
    std::atomic<std::uint64_t> breach_total{0};
    std::atomic<std::uint64_t> soft_breach_observe_total{0};
    std::atomic<std::uint64_t> arm_degraded_total{0};
    std::atomic<std::uint64_t> consecutive{0};
    std::atomic<std::uint64_t> combined_fail_total{0};
    std::atomic<std::uint32_t> last_breached{0};
    std::atomic<std::uint32_t> last_would_arm_degraded{0};
    std::atomic<std::uint32_t> last_force_reason_code{0};
    // Test-only: remaining synthetic fwrite misses (enabled path only).
    std::atomic<int> inject_fail_remaining{0};
};

inline WalAppendFailSloCounters g_wal_append_fail_slo_counters{};

// Assemble live counters from both WAL namespaces (AC5).
[[nodiscard]] inline WalAppendFailSloInput
make_wal_append_fail_slo_input(std::uint64_t mutation_fail, std::uint64_t se_fail,
                               std::uint64_t mutation_persisted, std::uint64_t se_persisted,
                               bool wal_enabled, bool production_defaults,
                               bool soft_mode) noexcept {
    WalAppendFailSloInput in;
    in.fail_total = mutation_fail + se_fail;
    in.persisted_total = mutation_persisted + se_persisted;
    in.consecutive = g_wal_append_fail_slo_counters.consecutive.load(std::memory_order_relaxed);
    in.wal_enabled = wal_enabled;
    in.production_defaults = production_defaults;
    in.soft_mode = soft_mode;
    return in;
}

inline WalAppendFailSloDecision
evaluate_wal_append_fail_slo(const WalAppendFailSloInput& in) noexcept {
    auto& c = g_wal_append_fail_slo_counters;
    c.checks_total.fetch_add(1, std::memory_order_relaxed);
    const auto d = decide_wal_append_fail_slo(in);
    c.last_breached.store(d.breached ? 1 : 0, std::memory_order_relaxed);
    c.last_would_arm_degraded.store(d.would_arm_degraded ? 1 : 0, std::memory_order_relaxed);
    c.last_force_reason_code.store(static_cast<std::uint32_t>(d.force_reason_code),
                                   std::memory_order_relaxed);
    if (d.would_arm_degraded) {
        c.arm_degraded_total.fetch_add(1, std::memory_order_relaxed);
        c.breach_total.fetch_add(1, std::memory_order_relaxed);
    } else if (d.breached) {
        c.soft_breach_observe_total.fetch_add(1, std::memory_order_relaxed);
        c.breach_total.fetch_add(1, std::memory_order_relaxed);
    }
    return d;
}

// Called only after WAL is_enabled() && fp (AC1: disabled path never
// reaches here — still the existing `if (!enabled || !fp) return false`).
inline void note_wal_append_fail() noexcept {
    auto& c = g_wal_append_fail_slo_counters;
    c.consecutive.fetch_add(1, std::memory_order_relaxed);
    c.combined_fail_total.fetch_add(1, std::memory_order_relaxed);
}

inline void note_wal_append_ok() noexcept {
    g_wal_append_fail_slo_counters.consecutive.store(0, std::memory_order_relaxed);
}

[[nodiscard]] inline bool consume_wal_inject_append_fail() noexcept {
    auto& rem = g_wal_append_fail_slo_counters.inject_fail_remaining;
    int v = rem.load(std::memory_order_relaxed);
    while (v > 0) {
        if (rem.compare_exchange_weak(v, v - 1, std::memory_order_relaxed))
            return true;
    }
    return false;
}

inline std::atomic<int>& wal_fail_closed_defaulted_by_force_wal_flag() noexcept {
    static std::atomic<int> v{0};
    return v;
}

inline void set_wal_fail_closed_defaulted_by_force_wal(bool v) noexcept {
    wal_fail_closed_defaulted_by_force_wal_flag().store(v ? 1 : 0, std::memory_order_relaxed);
}

[[nodiscard]] inline int wal_fail_closed_defaulted_by_force_wal() noexcept {
    return wal_fail_closed_defaulted_by_force_wal_flag().load(std::memory_order_relaxed);
}

inline void reset_wal_append_fail_slo_for_test() noexcept {
    auto& c = g_wal_append_fail_slo_counters;
    c.checks_total.store(0, std::memory_order_relaxed);
    c.breach_total.store(0, std::memory_order_relaxed);
    c.soft_breach_observe_total.store(0, std::memory_order_relaxed);
    c.arm_degraded_total.store(0, std::memory_order_relaxed);
    c.consecutive.store(0, std::memory_order_relaxed);
    c.combined_fail_total.store(0, std::memory_order_relaxed);
    c.last_breached.store(0, std::memory_order_relaxed);
    c.last_would_arm_degraded.store(0, std::memory_order_relaxed);
    c.last_force_reason_code.store(0, std::memory_order_relaxed);
    c.inject_fail_remaining.store(0, std::memory_order_relaxed);
    set_wal_fail_closed_defaulted_by_force_wal(false);
}

// Issue #3109: production WAL append fail-closed option (SE + mutation
// audit trail integrity). Issue #3302 residual: when force_wal actually
// enabled WAL (Restricted / Strict / multi_tenant), default fail-closed
// so durable + evidence capture stay paired. Explicit opt-out:
// AURA_WAL_APPEND_FAIL_OPEN=1. Explicit AURA_WAL_APPEND_FAIL_CLOSED=1
// still forces on. Soft / production_defaults_active()==0: always false
// (zero new cost, AC1). Caller pattern:
//   if (wal_append_fail_closed_active()) wal_overflow_ring_push(rec);
inline constexpr int kWalAppendFailClosedForceWalIssue = 3302;

[[nodiscard]] inline bool wal_env_flag_truthy(const char* name) noexcept {
    const char* e = std::getenv(name);
    if (e == nullptr || e[0] == '\0')
        return false;
    return std::strcmp(e, "1") == 0 || std::strcmp(e, "true") == 0 || std::strcmp(e, "on") == 0 ||
           std::strcmp(e, "TRUE") == 0 || std::strcmp(e, "ON") == 0 ||
           std::strcmp(e, "True") == 0 || std::strcmp(e, "On") == 0 || std::strcmp(e, "yes") == 0 ||
           std::strcmp(e, "YES") == 0 || std::strcmp(e, "Yes") == 0;
}

[[nodiscard]] inline bool wal_append_fail_closed_active() noexcept {
    // Soft / no production_defaults: fail-closed never active (AC1).
    if (aura_production_defaults_active_probe() == 0)
        return false;
    if (wal_env_flag_truthy("AURA_WAL_APPEND_FAIL_OPEN"))
        return false; // #3302 explicit opt-out
    if (wal_env_flag_truthy("AURA_WAL_APPEND_FAIL_CLOSED"))
        return true; // #3109 explicit opt-in (AC4)
    // #3302: force_wal arm sets this process flag in security_defaults.
    return wal_fail_closed_defaulted_by_force_wal() != 0;
}

} // namespace aura::core::wal_slo
#endif // AURA_CORE_WAL_APPEND_FAIL_SLO_H
