// flatast_restamp.hh — FlatAST generation-wrap restamp policy (extracted from
// ast.ixx). SSOT for RestampPolicy + env resolvers used by FlatAST wrap
// recovery and Agent observability.
//
// Issue #2402 / #2122 / #2528. Consumers: aura.core.ast (module re-export),
// tests, evaluator stdlib-review. Keep header-only so non-module TUs can
// resolve policy without importing the full FlatAST module.
//
// Part of FlatAST decomposition (architecture review step 1 of 4).

#ifndef AURA_CORE_FLATAST_RESTAMP_HH
#define AURA_CORE_FLATAST_RESTAMP_HH

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace aura::ast {

// Issue #2402 / #2122: generation-wrap restamp policy (production default = Auto).
//   Full:        always restamp all live nodes on wrap recovery
//   Incremental: dirty/touched cone only; empty cone → lazy-align only
//                (no O(N) full walk); density never forces full
//   Auto:        #2122 density-gated: incremental when touched density
//                ≤ threshold; full fallback when empty cone or dense
// Env: AURA_RESTAMP_POLICY=full|incremental|auto (default auto).
// Zero cost when no wrap / no pending restamp (policy is only read
// inside restamp_all_node_generations on wrap recovery).
enum class RestampPolicy : std::uint8_t {
    Full = 0,
    Incremental = 1,
    Auto = 2,
};

inline constexpr int kRestampIncrementalDefaultIssue = 2402;

// Resolve process-wide restamp policy from AURA_RESTAMP_POLICY.
// Default Auto = production-friendly density-gated incremental (#2122/#2402).
[[nodiscard]] inline RestampPolicy resolve_restamp_policy() noexcept {
    const char* e = std::getenv("AURA_RESTAMP_POLICY");
    if (!e || !*e)
        return RestampPolicy::Auto;
    // Case-insensitive-ish: accept common spellings.
    if ((e[0] == 'f' || e[0] == 'F') && (e[1] == 'u' || e[1] == 'U'))
        return RestampPolicy::Full;
    if ((e[0] == 'i' || e[0] == 'I') && (e[1] == 'n' || e[1] == 'N'))
        return RestampPolicy::Incremental;
    if ((e[0] == 'a' || e[0] == 'A') && (e[1] == 'u' || e[1] == 'U'))
        return RestampPolicy::Auto;
    return RestampPolicy::Auto;
}

// Issue #2528: resolve AURA_REStamp_SLO_US env once on first call.
// Cached static atomic — subsequent calls return the cached budget
// (idempotent thereafter). Default 500 µs. Caller should read via this
// helper rather than the member directly so the env override applies
// without per-call env reads.
[[nodiscard]] inline std::uint32_t resolve_restamp_slo_us() noexcept {
    static std::atomic<std::uint32_t> cached{500};
    static std::atomic<bool> initialized{false};
    if (!initialized.load(std::memory_order_acquire)) {
        const char* e = std::getenv("AURA_REStamp_SLO_US");
        if (e && *e) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(e, &end, 10);
            if (end != e && v >= 1ul && v <= 60000000ul)
                cached.store(static_cast<std::uint32_t>(v), std::memory_order_release);
        }
        initialized.store(true, std::memory_order_release);
    }
    return cached.load(std::memory_order_acquire);
}

[[nodiscard]] inline const char* restamp_policy_name(RestampPolicy p) noexcept {
    switch (p) {
        case RestampPolicy::Full:
            return "full";
        case RestampPolicy::Incremental:
            return "incremental";
        case RestampPolicy::Auto:
        default:
            return "auto";
    }
}

// Issue #2934: restamp budget (max nodes eager-restamped per
// restamp_all_node_generations call). 0 = unlimited (default Soft /
// regression-green current behavior). When planned restamp count
// exceeds budget, soft-degrade to incremental (if dirty cone fits)
// or lazy-align only (skip O(N) full walk); never silent torn gen
// (lazy-align keeps is_valid/make_ref consistent). Env:
// AURA_RESTAMP_BUDGET_NODES (process-wide, cached).
// Issue #3000: export-face residual — query:*-stable must not hand
// the Agent a pre-mutate generation when last restamp exceeded and
// the node was not eagerly restamped. Lazy-align still keeps
// is_valid/make_ref consistent (never silent torn generation, #2934 AC2);
// production export rejects (typed restamp-lag) instead of stamping-green
// a lagging gen.
inline constexpr int kRestampBudgetIssue = 2934;
inline constexpr int kQueryStableRefRestampLagIssue = 3000;

[[nodiscard]] inline std::atomic<std::uint32_t>& g_restamp_budget_nodes_override() noexcept {
    static std::atomic<std::uint32_t> v{0};
    return v;
}
[[nodiscard]] inline std::atomic<bool>& g_restamp_budget_nodes_override_set() noexcept {
    static std::atomic<bool> v{false};
    return v;
}

[[nodiscard]] inline std::uint32_t resolve_restamp_budget_nodes() noexcept {
    static std::atomic<std::uint32_t> cached{0};
    static std::atomic<bool> initialized{false};
    if (!initialized.load(std::memory_order_acquire)) {
        const char* e = std::getenv("AURA_RESTAMP_BUDGET_NODES");
        if (e && *e) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(e, &end, 10);
            // 0 = unlimited; cap at 100M to avoid accidental overflow.
            if (end != e && v <= 100000000ul)
                cached.store(static_cast<std::uint32_t>(v), std::memory_order_release);
        }
        initialized.store(true, std::memory_order_release);
    }
    return cached.load(std::memory_order_acquire);
}

// Test / production-defaults override of the process cache.
inline void set_restamp_budget_nodes_for_process(std::uint32_t n) noexcept {
    g_restamp_budget_nodes_override().store(n, std::memory_order_release);
    g_restamp_budget_nodes_override_set().store(true, std::memory_order_release);
}

[[nodiscard]] inline std::uint32_t restamp_budget_nodes_effective() noexcept {
    if (g_restamp_budget_nodes_override_set().load(std::memory_order_acquire))
        return g_restamp_budget_nodes_override().load(std::memory_order_acquire);
    return resolve_restamp_budget_nodes();
}

inline void clear_restamp_budget_nodes_override_for_test() noexcept {
    g_restamp_budget_nodes_override_set().store(false, std::memory_order_release);
    g_restamp_budget_nodes_override().store(0, std::memory_order_release);
}

} // namespace aura::ast

#endif // AURA_CORE_FLATAST_RESTAMP_HH
