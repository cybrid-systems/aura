// Issue #909: ObservabilityPrims — friend of Evaluator for peeled registration.
// Issue #1670: tier member decls generated from observability_{eval,jit}_tiers.inc
// (single source of truth for peel tier counts).
struct ObservabilityPrims {
    static EvalValue meta_to_pair(Evaluator& ev, const PrimMeta& m);
    static const std::vector<std::string>& stats_primitives();
    // Issue #1439: internal-only stats impl table (not public primitives).
    // (engine:metrics "name") / :all / :prefix resolve via lookup_stats_impl.
    static void register_stats_impl(std::string name, PrimFn fn);
    [[nodiscard]] static std::optional<PrimFn> lookup_stats_impl(std::string_view name);
    [[nodiscard]] static bool is_legacy_stats_name(std::string_view name);
    // True if name is in the internal stats table (or still public for non-#1439 names).
    [[nodiscard]] static bool stats_impl_registered(std::string_view name);
    // Issue #1672: catalog ↔ register_stats_impl drift diagnostic (hash schema 1672).
    // Exposed via (stats:get "stats:drift-check") / (engine:metrics "stats:drift-check")
    // — not a public add() (SlimSurface ceiling).
    [[nodiscard]] static EvalValue stats_drift_check(Evaluator& ev);
    using PrimRegistrar = std::function<void(std::string, PrimFn)>;
#define OBS_EVAL_TIER(N) static void register_eval_p##N(PrimRegistrar add, Evaluator& ev);
#include "compiler/observability_eval_tiers.inc"
#undef OBS_EVAL_TIER
#define OBS_JIT_TIER(N) static void register_jit_p##N(PrimRegistrar add, Evaluator& ev);
#include "compiler/observability_jit_tiers.inc"
#undef OBS_JIT_TIER
    static void register_eval_all(PrimRegistrar add, Evaluator& ev);
    static void register_jit_all(PrimRegistrar add, Evaluator& ev);
    // P2b: stats:list + stats:count + engine:metrics only (s0 facade).
    // Issue #1450: also stats:get / stats:prefix / engine:surface.
    static void register_metrics_facade(PrimRegistrar add, Evaluator& ev);
    // Issue #1434: mark top-20 query:*-stats PrimMeta.deprecated after reg.
    static void mark_p1b_top_stats_deprecated(Evaluator& ev);
    // Issue #1450: residual public *-stats (non query:/compile:) → deprecated
    // aliases; prefer (stats:get) / (engine:metrics). Count kept <50.
    static void mark_residual_public_stats_deprecated(Evaluator& ev);
};
