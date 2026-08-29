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
// Issue #3037: over-budget outermost restamp marks generation torn for
// export. Lazy-align must not hide a pre-mutate gen (eager bit, not
// node_gen_==generation_). Production reject_stable_ref_export; Soft
// observe only. Happy under-budget path is identical restamp_all.
inline constexpr int kRestampBudgetIssue = 2934;
inline constexpr int kQueryStableRefRestampLagIssue = 3000;
inline constexpr int kRestampOverBudgetExportIssue = 3037;
// Issue #3041: production budget exceed forces QueryEpoch stale +
// pollable restamp-budget-query-epoch-stale-total. Soft metric-only.
inline constexpr int kRestampBudgetQueryEpochStaleIssue = 3041;
// Issue #3019: unified restamp after boundary / abort / steal / densify.
// Additive torn-visible counter — does not replace restamp-lag (#3000).
inline constexpr int kUnifiedRestampIssue = 3019;
// Issue #3058: over-budget torn must be visible on every query:*-stable
// surface (as-stable-ref / ensure-ref included). Additive schema only.
inline constexpr int kUnifiedRestampQueryVisibleIssue = 3058;
// Issue #3121: production query:*-stable / as-stable-ref / ensure-ref
// must surface restamp-budget lag as structured Agent-visible error
// (error="restamp-lag", reason="budget-exceeded"). Soft observe-only.
// Never a green StableNodeRef carrying a pre-mutate gen.
inline constexpr int kQueryStableRestampLagStructuredIssue = 3121;
inline constexpr const char* kRestampLagErrorKind = "restamp-lag";
inline constexpr const char* kRestampLagReasonBudgetExceeded = "budget-exceeded";
// Issue #3198: every Agent export path (query:*-stable / ensure-ref /
// :as-query-result / export_ref) must fail-closed on the same torn face.
// No new public query key — reuse restamp-lag / torn counters.
inline constexpr int kQueryStableRestampExportUniformIssue = 3198;
// Issue #3230: production query:*-stable export must consult torn/budget
// *before* make_ref_layout / make_safe_ref_layout / stamp-green.
// Equivalent to last_budget_exceeded || generation_torn (#3037).
// Soft/Off observe-only. No new public query key.
inline constexpr int kQueryStableRestampLagHardRejectIssue = 3230;
// Issue #3259: production over-budget outermost restamp eager-restamps
// a hot cone (dirty roots + parent chain) up to a fraction of the
// restamp budget so Agent-held StableNodeRef / QueryResult on those
// nodes stay exportable. Remainder stays lazy-align + torn (never
// green a pre-mutate gen). Soft / budget==0 / no production_defaults:
// zero extra (this helper is not consulted). Env:
// AURA_RESTAMP_HOT_CONE_FRAC percent in [1,100], default 50 (budget/2).
inline constexpr int kRestampHotConeBudgetIssue = 3259;
inline constexpr std::uint32_t kRestampHotConeFracPercentDefault = 50;
// Issue #3327: over-budget hot-cone also includes the Agent-held
// QueryResult / last-export StableNodeRef set (union with dirty+parent,
// still capped by restamp_hot_cone_budget). Soft never consults this
// buffer (restamp_hot_cone_after_budget is production-only).
inline constexpr int kRestampHotConeAgentHeldIssue = 3327;
inline constexpr std::uint16_t kRestampHotConeHeldCap = 64;
// Issue #3426: held-cap overflow must not eager a 64-id prefix (half-green
// Agent memory). Flag only — no unbounded buffer, no new query key.
inline constexpr int kRestampHotConeHeldOverflowIssue = 3426;
[[nodiscard]] inline bool restamp_over_budget_torn(bool last_budget_exceeded,
                                                   bool generation_torn) noexcept {
    return last_budget_exceeded || generation_torn;
}
inline std::atomic<std::uint64_t> g_unified_restamp_torn_visible_total{0};
inline std::atomic<std::uint64_t> g_unified_restamp_calls_total{0};
[[nodiscard]] inline std::uint64_t unified_restamp_torn_visible_total_v_read() noexcept {
    return g_unified_restamp_torn_visible_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t unified_restamp_calls_total_v_read() noexcept {
    return g_unified_restamp_calls_total.load(std::memory_order_relaxed);
}
inline void reset_unified_restamp_3019_for_test() noexcept {
    g_unified_restamp_torn_visible_total.store(0, std::memory_order_relaxed);
    g_unified_restamp_calls_total.store(0, std::memory_order_relaxed);
}

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

// Issue #3259: fraction of restamp budget reserved for eager hot-cone
// restamp on production over-budget outermost exit. Cached env, same
// shape as resolve_restamp_budget_nodes. Default 50%.
[[nodiscard]] inline std::uint32_t resolve_restamp_hot_cone_frac_percent() noexcept {
    static std::atomic<std::uint32_t> cached{kRestampHotConeFracPercentDefault};
    static std::atomic<bool> initialized{false};
    if (!initialized.load(std::memory_order_acquire)) {
        const char* e = std::getenv("AURA_RESTAMP_HOT_CONE_FRAC");
        if (e && *e) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(e, &end, 10);
            if (end != e && v >= 1ul && v <= 100ul)
                cached.store(static_cast<std::uint32_t>(v), std::memory_order_release);
        }
        initialized.store(true, std::memory_order_release);
    }
    return cached.load(std::memory_order_acquire);
}

[[nodiscard]] inline std::uint32_t restamp_hot_cone_budget(std::uint32_t budget) noexcept {
    if (budget == 0)
        return 0;
    const auto pct = resolve_restamp_hot_cone_frac_percent();
    const auto n = static_cast<std::uint32_t>((static_cast<std::uint64_t>(budget) * pct) / 100ull);
    return n == 0 ? 1u : n;
}

// Process-visible last Agent-held node ids (QueryResult matches + last
// successful query:*-stable / stamp export + pinned StableNodeRef dump).
// Deduped, capped; excess is fail-closed by the hot-cone budget (AC2).
inline std::uint32_t g_restamp_hot_cone_held_ids[kRestampHotConeHeldCap]{};
inline std::atomic<std::uint16_t> g_restamp_hot_cone_held_count{0};
inline std::atomic<std::uint8_t> g_restamp_hot_cone_held_overflow{0};

inline void note_restamp_hot_cone_held_node(std::uint32_t node_id) noexcept {
    if (node_id == 0)
        return;
    const auto n = g_restamp_hot_cone_held_count.load(std::memory_order_relaxed);
    const auto lim = n > kRestampHotConeHeldCap ? kRestampHotConeHeldCap : n;
    for (std::uint16_t i = 0; i < lim; ++i) {
        if (g_restamp_hot_cone_held_ids[i] == node_id)
            return;
    }
    if (n >= kRestampHotConeHeldCap) {
        g_restamp_hot_cone_held_overflow.store(1, std::memory_order_relaxed);
        return;
    }
    g_restamp_hot_cone_held_ids[n] = node_id;
    g_restamp_hot_cone_held_count.store(static_cast<std::uint16_t>(n + 1),
                                        std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint16_t restamp_hot_cone_held_count() noexcept {
    auto n = g_restamp_hot_cone_held_count.load(std::memory_order_relaxed);
    return n > kRestampHotConeHeldCap ? kRestampHotConeHeldCap : n;
}

[[nodiscard]] inline std::uint32_t restamp_hot_cone_held_id_at(std::uint16_t i) noexcept {
    return i < kRestampHotConeHeldCap ? g_restamp_hot_cone_held_ids[i] : 0u;
}

[[nodiscard]] inline bool restamp_hot_cone_held_overflow() noexcept {
    return g_restamp_hot_cone_held_overflow.load(std::memory_order_relaxed) != 0;
}

inline void clear_restamp_hot_cone_held_for_test() noexcept {
    g_restamp_hot_cone_held_count.store(0, std::memory_order_relaxed);
    g_restamp_hot_cone_held_overflow.store(0, std::memory_order_relaxed);
}

inline void clear_restamp_budget_nodes_override_for_test() noexcept {
    g_restamp_budget_nodes_override_set().store(false, std::memory_order_release);
    g_restamp_budget_nodes_override().store(0, std::memory_order_release);
}

} // namespace aura::ast

#endif // AURA_CORE_FLATAST_RESTAMP_HH
