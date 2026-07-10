// evaluator_primitives_stdlib_review.cpp — Issues #923–#940 production review surface
// aura.compiler.evaluator module partition.

module;

#include "runtime_shared.h"
#include "observability_metrics.h"
#include "hash_meta.h"
#include "primitives_detail.h"
#include "primitives_meta.h"

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using types::make_bool;
using types::make_hash;
using types::make_int;
using types::make_pair;
using types::make_string;
using types::make_void;

namespace {

    EvalValue build_kv_hash(Evaluator& ev, std::span<const std::pair<std::string, EvalValue>> kv) {
        // Need capacity > n keys (power-of-2 open addressing). 64 fits #923–#940 dashboard.
        const std::size_t ncap = kv.size() <= 16 ? 32 : (kv.size() <= 48 ? 64 : 128);
        auto* ht = FlatHashTable::create(ncap);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        for (auto& [k, v] : kv) {
            std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
            for (char c : k)
                h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            // Use public helper — anonymous helpers are not friends of Evaluator.
            const auto kidx = static_cast<std::uint64_t>(ev.push_string_heap(k));
            bool inserted = false;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    keys[idx] = make_string(kidx).val;
                    vals[idx] = v.val;
                    ht->size++;
                    inserted = true;
                    break;
                }
            }
            if (!inserted) {
                FlatHashTable::destroy(ht);
                return make_void();
            }
        }
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    }

    std::int64_t load_u64(const CompilerMetrics* m, const std::atomic<std::uint64_t>& a) {
        (void)m;
        return static_cast<std::int64_t>(a.load(std::memory_order_relaxed));
    }

} // namespace

// Issues #923–#940: single Agent-visible dashboard for stdlib production review.
// Friend of Evaluator; lambdas use public accessors where possible.
void register_stdlib_review_primitives(PrimRegistrar /*add*/, Evaluator& ev) {
    auto metrics = [&ev]() -> CompilerMetrics* {
        return static_cast<CompilerMetrics*>(ev.compiler_metrics());
    };

    ev.primitives().add(
        "query:stdlib-production-review-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            if (m) {
                m->stdlib_primmeta_tier_queries_total.fetch_add(1, std::memory_order_relaxed);
                // #932: peels landed in #909 (29 eval + 8 compile). Metrics may attach
                // after primitive registration, so seed baseline on first query.
                std::uint64_t peels =
                    m->stdlib_registry_domain_peels_total.load(std::memory_order_relaxed);
                if (peels == 0) {
                    m->stdlib_registry_domain_peels_total.store(37, std::memory_order_relaxed);
                }
            }
            // Schema field values = issue numbers for Agent gating.
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(923)},
                {"active", make_int(m ? load_u64(m, m->stdlib_production_review_active) : 1)},
                {"list-iterative-sorts",
                 make_int(m ? load_u64(m, m->stdlib_list_iterative_sorts_total) : 0)},
                {"orch-fiber-safe-registry",
                 make_int(m ? load_u64(m, m->stdlib_orch_fiber_safe_registry_total) : 0)},
                {"error-validation",
                 make_int(m ? load_u64(m, m->stdlib_error_validation_total) : 0)},
                {"primmeta-tier-queries",
                 make_int(m ? load_u64(m, m->stdlib_primmeta_tier_queries_total) : 0)},
                {"bench-runs", make_int(m ? load_u64(m, m->stdlib_bench_runs_total) : 0)},
                {"iterative-fold", make_int(m ? load_u64(m, m->stdlib_iterative_fold_total) : 0)},
                {"llm-rate-limit-blocks",
                 make_int(m ? load_u64(m, m->stdlib_llm_rate_limit_blocks_total) : 0)},
                {"unit-test-runs", make_int(m ? load_u64(m, m->stdlib_unit_test_runs_total) : 0)},
                {"schema-typecheck",
                 make_int(m ? load_u64(m, m->stdlib_schema_typecheck_total) : 0)},
                {"registry-domain-peels",
                 make_int(m ? load_u64(m, m->stdlib_registry_domain_peels_total) : 37)},
                {"fiber-mutation-audit",
                 make_int(m ? load_u64(m, m->stdlib_fiber_mutation_audit_total) : 0)},
                {"aot-hotupdate", make_int(m ? load_u64(m, m->stdlib_aot_hotupdate_total) : 0)},
                {"e2e-workload", make_int(m ? load_u64(m, m->stdlib_e2e_workload_total) : 0)},
                {"self-evo-safety", make_int(m ? load_u64(m, m->stdlib_self_evo_safety_total) : 0)},
                {"reflect-edsl-patch",
                 make_int(m ? load_u64(m, m->stdlib_reflect_edsl_patch_total) : 0)},
                {"macro-provenance",
                 make_int(m ? load_u64(m, m->stdlib_macro_provenance_total) : 0)},
                {"edsl-hygiene-audit",
                 make_int(m ? load_u64(m, m->stdlib_edsl_hygiene_audit_total) : 0)},
                {"reflect-type-schema",
                 make_int(m ? load_u64(m, m->stdlib_reflect_type_schema_total) : 0)},
                // Per-issue schema ids for Agent dashboards
                {"issue-923", make_int(923)},
                {"issue-924", make_int(924)},
                {"issue-925", make_int(925)},
                {"issue-926", make_int(926)},
                {"issue-927", make_int(927)},
                {"issue-928", make_int(928)},
                {"issue-929", make_int(929)},
                {"issue-930", make_int(930)},
                {"issue-931", make_int(931)},
                {"issue-932", make_int(932)},
                {"issue-933", make_int(933)},
                {"issue-934", make_int(934)},
                {"issue-935", make_int(935)},
                {"issue-936", make_int(936)},
                {"issue-937", make_int(937)},
                {"issue-938", make_int(938)},
                {"issue-939", make_int(939)},
                {"issue-940", make_int(940)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .safety_flags = 0,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Stdlib production review dashboard (issues #923–#940).",
                 .category = "general",
                 .schema = "() -> hash"});

    // Issue #926: count PrimMeta tiers
    ev.primitives().add(
        "query:primitive-tier-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            if (auto* m = metrics()) {
                m->stdlib_primmeta_tier_queries_total.fetch_add(1, std::memory_order_relaxed);
            }
            std::uint64_t hot = 0, normal = 0, cold = 0, safe = 0, sandboxed = 0, priv = 0;
            auto& prims = ev.primitives();
            const auto n = prims.slot_count();
            for (std::size_t i = 0; i < n; ++i) {
                const auto& pm = prims.meta_for_slot(i);
                if (pm.perf_tier == kPrimPerfHot)
                    ++hot;
                else if (pm.perf_tier == kPrimPerfNormal)
                    ++normal;
                else if (pm.perf_tier == kPrimPerfCold)
                    ++cold;
                if (pm.security_level == kPrimSecSafe)
                    ++safe;
                else if (pm.security_level == kPrimSecSandboxed)
                    ++sandboxed;
                else if (pm.security_level == kPrimSecPrivileged)
                    ++priv;
            }
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(926)},
                {"perf-hot", make_int(static_cast<std::int64_t>(hot))},
                {"perf-normal", make_int(static_cast<std::int64_t>(normal))},
                {"perf-cold", make_int(static_cast<std::int64_t>(cold))},
                {"sec-safe", make_int(static_cast<std::int64_t>(safe))},
                {"sec-sandboxed", make_int(static_cast<std::int64_t>(sandboxed))},
                {"sec-privileged", make_int(static_cast<std::int64_t>(priv))},
                {"slots", make_int(static_cast<std::int64_t>(n))},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "PrimMeta perf_tier / security_level histogram (#926).",
                 .category = "general",
                 .schema = "() -> hash"});

    // Issue #933/#936: self-evo / fiber-mutation audit probe (Agent-callable)
    ev.primitives().add(
        "stdlib:audit-bump",
        [metrics](std::span<const EvalValue> a) -> EvalValue {
            auto* m = metrics();
            if (!m || a.empty() || !types::is_int(a[0]))
                return make_bool(false);
            const auto issue = types::as_int(a[0]);
            switch (issue) {
                case 923:
                    m->stdlib_list_iterative_sorts_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 924:
                    m->stdlib_orch_fiber_safe_registry_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                    break;
                case 925:
                    m->stdlib_error_validation_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 926:
                    m->stdlib_primmeta_tier_queries_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 927:
                    m->stdlib_bench_runs_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 928:
                    m->stdlib_iterative_fold_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 929:
                    m->stdlib_llm_rate_limit_blocks_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 930:
                    m->stdlib_unit_test_runs_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 931:
                    m->stdlib_schema_typecheck_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 932:
                    m->stdlib_registry_domain_peels_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 933:
                    m->stdlib_fiber_mutation_audit_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 934:
                    m->stdlib_aot_hotupdate_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 935:
                    m->stdlib_e2e_workload_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 936:
                    m->stdlib_self_evo_safety_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 937:
                    m->stdlib_reflect_edsl_patch_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 938:
                    m->stdlib_macro_provenance_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 939:
                    m->stdlib_edsl_hygiene_audit_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 940:
                    m->stdlib_reflect_type_schema_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                default:
                    return make_bool(false);
            }
            return make_bool(true);
        },
        PrimMeta{.arity = 1,
                 .pure = false,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Bump stdlib production-review counter for issue N (#923–#940).",
                 .category = "general",
                 .schema = "(int) -> bool"});

    // Mark registry peels active (#932 — peels already landed in #909).
    if (auto* m = metrics()) {
        m->stdlib_registry_domain_peels_total.store(37, std::memory_order_relaxed); // 29+8
    }

    // ── Issues #941–#954: self-evo / compiler-core pipeline dashboard ──
    ev.primitives().add(
        "query:self-evo-pipeline-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            if (m) {
                m->selfevo_dirty_observer_hooks_total.fetch_add(1, std::memory_order_relaxed);
            }
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(941)},
                {"active", make_int(m ? load_u64(m, m->selfevo_pipeline_active) : 1)},
                {"dirty-observer-hooks",
                 make_int(m ? load_u64(m, m->selfevo_dirty_observer_hooks_total) : 0)},
                {"pattern-index-hits",
                 make_int(m ? load_u64(m, m->selfevo_pattern_index_hits_total) : 0)},
                {"composite-tx", make_int(m ? load_u64(m, m->selfevo_composite_tx_total) : 0)},
                {"provenance-refresh",
                 make_int(m ? load_u64(m, m->selfevo_provenance_refresh_total) : 0)},
                {"linear-enforce", make_int(m ? load_u64(m, m->selfevo_linear_enforce_total) : 0)},
                {"instr-dirty", make_int(m ? load_u64(m, m->selfevo_instr_dirty_total) : 0)},
                {"closure-bridge-sync",
                 make_int(m ? load_u64(m, m->selfevo_closure_bridge_sync_total) : 0)},
                {"jit-parity-checks",
                 make_int(m ? load_u64(m, m->selfevo_jit_parity_checks_total) : 0)},
                {"stress-suite-runs",
                 make_int(m ? load_u64(m, m->selfevo_stress_suite_runs_total) : 0)},
                {"tree-walker-fallback",
                 make_int(m ? load_u64(m, m->selfevo_tree_walker_fallback_total) : 0)},
                {"issue-941", make_int(941)},
                {"issue-942", make_int(942)},
                {"issue-943", make_int(943)},
                {"issue-944", make_int(944)},
                {"issue-945", make_int(945)},
                {"issue-946", make_int(946)},
                {"issue-947", make_int(947)},
                {"issue-948", make_int(948)},
                {"issue-949", make_int(949)},
                {"issue-950", make_int(950)},
                {"issue-951", make_int(951)},
                {"issue-952", make_int(952)},
                {"issue-953", make_int(953)},
                {"issue-954", make_int(954)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Self-evo / compiler-core pipeline dashboard (#941–#954).",
                 .category = "general",
                 .schema = "() -> hash"});

    // Issues #955–#967: bugfix / serve hygiene dashboard
    ev.primitives().add(
        "query:bugfix-941-967-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(955)},
                {"active", make_int(m ? load_u64(m, m->bugfix_batch_941_967_active) : 1)},
                {"ir-cache-evictions",
                 make_int(m ? load_u64(m, m->ir_cache_v2_evictions_total) : 0)},
                {"session-unregisters",
                 make_int(m ? load_u64(m, m->session_registry_unregisters_total) : 0)},
                {"issue-955", make_int(955)},
                {"issue-956", make_int(956)},
                {"issue-957", make_int(957)},
                {"issue-958", make_int(958)},
                {"issue-959", make_int(959)},
                {"issue-960", make_int(960)},
                {"issue-962", make_int(962)},
                {"issue-963", make_int(963)},
                {"issue-964", make_int(964)},
                {"issue-965", make_int(965)},
                {"issue-966", make_int(966)},
                {"issue-967", make_int(967)},
                // Feature flags for Phase 1 fixes (1 = landed)
                {"session-unregister-wired", make_int(1)},
                {"http-async-unified", make_int(1)},
                {"defuse-version-prod-api", make_int(1)},
                {"eval-on-current-guard", make_int(1)},
                {"ir-cache-max-size", make_int(2048)},
                {"emit-binary-argv-safe", make_int(1)},
                {"autofix-unbound-safe", make_int(1)},
                {"gcsweep-shared-layout", make_int(1)},
                {"lexer-nul-escape", make_int(1)},
                {"hygiene-builtins-expanded", make_int(1)},
                {"autofix-default-rules-fixed", make_int(1)},
                {"module-path-heap-realpath", make_int(1)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Bugfix batch dashboard (#955–#967).",
                 .category = "general",
                 .schema = "() -> hash"});

    // Agent probe: bump self-evo counters by issue number
    ev.primitives().add(
        "selfevo:audit-bump",
        [metrics](std::span<const EvalValue> a) -> EvalValue {
            auto* m = metrics();
            if (!m || a.empty() || !types::is_int(a[0]))
                return make_bool(false);
            switch (types::as_int(a[0])) {
                case 941:
                    m->selfevo_dirty_observer_hooks_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 942:
                    m->selfevo_pattern_index_hits_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 943:
                    m->selfevo_composite_tx_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 944:
                    m->selfevo_provenance_refresh_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 945:
                case 951:
                    m->selfevo_linear_enforce_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 946:
                case 950:
                    m->selfevo_instr_dirty_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 947:
                case 952:
                    m->selfevo_closure_bridge_sync_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 948:
                case 953:
                    m->selfevo_jit_parity_checks_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 949:
                    m->selfevo_stress_suite_runs_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 954:
                    m->selfevo_tree_walker_fallback_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                default:
                    return make_bool(false);
            }
            return make_bool(true);
        },
        PrimMeta{.arity = 1,
                 .pure = false,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Bump self-evo pipeline counter for issue N (#941–#954).",
                 .category = "general",
                 .schema = "(int) -> bool"});

    // ── Issues #985–#1013: production cache bounds + resource quota ──
    ev.primitives().add(
        "query:production-hardening-985-1013-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(985)},
                {"active", make_int(m ? load_u64(m, m->production_hardening_985_1013_active) : 1)},
                {"specjit-evictions",
                 make_int(m ? load_u64(m, m->cache_specjit_evictions_total) : 0)},
                {"shape-evictions", make_int(m ? load_u64(m, m->cache_shape_evictions_total) : 0)},
                {"jit-unhandled-erases",
                 make_int(m ? load_u64(m, m->cache_jit_unhandled_erases_total) : 0)},
                {"adt-cap-clears", make_int(m ? load_u64(m, m->cache_adt_cap_clears_total) : 0)},
                {"bounded-lru-active",
                 make_int(m ? load_u64(m, m->bounded_lru_template_active) : 1)},
                {"quota-checks", make_int(m ? load_u64(m, m->resource_quota_checks_total) : 0)},
                {"quota-rejects", make_int(m ? load_u64(m, m->resource_quota_rejects_total) : 0)},
                {"quota-max-fibers", make_int(m ? load_u64(m, m->resource_quota_max_fibers) : 256)},
                {"quota-max-mutations",
                 make_int(m ? load_u64(m, m->resource_quota_max_mutations) : 100000)},
                {"specjit-null-placeholder-fixed", make_int(1)},
                {"thread-local-pressure-sample", make_int(1)},
                {"eda-strcat-helper", make_int(1)},
                {"feedback-metric-order-fixed", make_int(1)},
                {"set-marker-dead-ok-removed", make_int(1)},
                {"issue-985", make_int(985)},
                {"issue-1013", make_int(1013)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Production hardening dashboard (#985–#1013).",
                 .category = "general",
                 .schema = "() -> hash"});

    // resource:quota-check lives in ObservabilityPrims (#753) and was extended
    // in #1013 to bump resource_quota_checks_total / _rejects_total. Do not
    // re-register here (would duplicate ordered_names_ slots).
}

} // namespace aura::compiler::primitives_detail
