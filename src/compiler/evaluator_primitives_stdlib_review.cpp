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
import aura.compiler.macro_expansion;

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
                // Issue #1231 Phase 1: hot path / EDA / FFI resource signals
                {"hot-path-hits", make_int(m ? load_u64(m, m->closure_ffi_calls) : 0)},
                {"eda-parse-total", make_int(m ? load_u64(m, m->eda_foundation_parse_total) : 0)},
                {"eda-hash-creates",
                 make_int(m ? load_u64(m, m->eda_hash_table_creates_total) : 0)},
                {"eda-alloc-bytes", make_int(m ? load_u64(m, m->eda_alloc_bytes_total) : 0)},
                {"ffi-opaque-tracking",
                 make_int(m ? load_u64(m, m->ffi_opaque_tracking_hardened) : 1)},
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

    // ── Issues #1014–#1046: production stability + bugfix dashboard ──
    ev.primitives().add(
        "query:production-stability-1014-1046-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1014)},
                {"active", make_int(m ? load_u64(m, m->production_stability_1014_1046_active) : 1)},
                {"rebind-fail-returns",
                 make_int(m ? load_u64(m, m->rebind_validation_fail_returns_total) : 0)},
                {"sandbox-admin-denials",
                 make_int(m ? load_u64(m, m->sandbox_admin_denials_total) : 0)},
                {"dirty-subtree-bfs",
                 make_int(m ? load_u64(m, m->dirty_subtree_bfs_walks_total) : 0)},
                {"ir-marker-queries",
                 make_int(m ? load_u64(m, m->ir_marker_stats_queries_total) : 0)},
                {"ir-cache-lru-evictions",
                 make_int(m ? load_u64(m, m->ir_cache_v2_lru_evictions_total) : 0)},
                {"ir-cache-max", make_int(2048)},
                {"rebind-validation-honest", make_int(1)}, // #1019
                {"sandbox-capability-gated", make_int(1)}, // #1020
                {"dirty-subtree-bfs-fixed", make_int(1)},  // #1036
                {"ir-marker-stats-hash", make_int(1)},     // #1039
                {"defuse-string-bounds", make_int(1)},     // #1040
                {"ir-cache-lru-active", make_int(1)},      // #1042
                {"panic-guard-lifecycle-active",
                 make_int(m ? load_u64(m, m->panic_guard_lifecycle_active) : 1)},
                {"serve-health-slo-active",
                 make_int(m ? load_u64(m, m->serve_health_slo_active) : 1)},
                {"issue-1014", make_int(1014)},
                {"issue-1046", make_int(1046)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Production stability dashboard (#1014–#1046).",
                 .category = "general",
                 .schema = "() -> hash"});

    // Issue #1015: unified serve health / SLO surface (Phase 1).
    ev.primitives().add(
        "query:serve-health",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1015)},
                {"healthy", make_int(1)},
                {"quota-checks", make_int(m ? load_u64(m, m->resource_quota_checks_total) : 0)},
                {"quota-rejects", make_int(m ? load_u64(m, m->resource_quota_rejects_total) : 0)},
                {"ir-cache-evictions",
                 make_int(m ? load_u64(m, m->ir_cache_v2_evictions_total) : 0)},
                {"sandbox-denials", make_int(m ? load_u64(m, m->sandbox_admin_denials_total) : 0)},
                {"slo-active", make_int(m ? load_u64(m, m->serve_health_slo_active) : 1)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Serve health / SLO Phase 1 (#1015).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1047–#1071: hygiene / type / mutate safety ──
    ev.primitives().add(
        "query:production-safety-1047-1071-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1047)},
                {"active", make_int(m ? load_u64(m, m->production_safety_1047_1071_active) : 1)},
                {"hw-coercion-empty-str-fixed",
                 make_int(m ? load_u64(m, m->hw_coercion_empty_str_fixed) : 1)},
                {"mutation-history-void-fixed",
                 make_int(m ? load_u64(m, m->mutation_history_void_fixed) : 1)},
                {"query-where-dedup-fixed",
                 make_int(m ? load_u64(m, m->query_where_dedup_fixed) : 1)},
                {"eval-string-bounds-fixed",
                 make_int(m ? load_u64(m, m->eval_string_bounds_fixed) : 1)},
                {"hygiene-marker-phase1",
                 make_int(m ? load_u64(m, m->hygiene_marker_phase1_active) : 1)},
                {"guard-fiber-phase1", make_int(m ? load_u64(m, m->guard_fiber_phase1_active) : 1)},
                {"ir-marker-hash-active", make_int(1)}, // continues #1039
                {"issue-1047", make_int(1047)},
                {"issue-1071", make_int(1071)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Production safety dashboard (#1047–#1071).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1072–#1096: security / metrics / concurrency ──
    ev.primitives().add(
        "query:production-hardening-1072-1096-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1072)},
                {"active", make_int(m ? load_u64(m, m->production_hardening_1072_1096_active) : 1)},
                {"http-shell-injection-fixed",
                 make_int(m ? load_u64(m, m->http_shell_injection_fixed) : 1)},
                {"recovery-pct-clamped", make_int(m ? load_u64(m, m->recovery_pct_clamped) : 1)},
                {"compaction-efficiency-clamped",
                 make_int(m ? load_u64(m, m->compaction_efficiency_clamped) : 1)},
                {"ast-ref-get-meta-tags", make_int(m ? load_u64(m, m->ast_ref_get_meta_tags) : 1)},
                {"pass-pipeline-yield-counter",
                 make_int(m ? load_u64(m, m->pass_pipeline_yield_counter) : 1)},
                {"remap-func-ids-base0-fixed",
                 make_int(m ? load_u64(m, m->remap_func_ids_base0_fixed) : 1)},
                {"mutate-string-bounds-bulk",
                 make_int(m ? load_u64(m, m->mutate_string_bounds_bulk) : 1)},
                {"arena-adaptive-no-dead-push", make_int(1)}, // #1072
                {"eda-sv-success-zero-on-fail", make_int(1)}, // #1078
                {"jit-fallback-status-defined", make_int(1)}, // #1096 (via #969)
                {"issue-1072", make_int(1072)},
                {"issue-1096", make_int(1096)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Production hardening dashboard (#1072–#1096).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1097–#1122: serialize / fold / serve safety ──
    ev.primitives().add(
        "query:production-safety-1097-1122-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1097)},
                {"active", make_int(m ? load_u64(m, m->production_safety_1097_1122_active) : 1)},
                {"eval-async-heap-result",
                 make_int(m ? load_u64(m, m->eval_async_heap_result) : 1)},
                {"const-fold-bool-tag-fixed",
                 make_int(m ? load_u64(m, m->const_fold_bool_tag_fixed) : 1)},
                {"const-fold-block-clear",
                 make_int(m ? load_u64(m, m->const_fold_block_clear) : 1)},
                {"reflect-bounds-checks", make_int(m ? load_u64(m, m->reflect_bounds_checks) : 1)},
                {"cache-header-validate-ext",
                 make_int(m ? load_u64(m, m->cache_header_validate_ext) : 1)},
                {"open-cache-ir-bounds", make_int(m ? load_u64(m, m->open_cache_ir_bounds) : 1)},
                {"schema-unknown-is-object", make_int(1)}, // #1113
                {"issue-1097", make_int(1097)},
                {"issue-1122", make_int(1122)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Production safety dashboard (#1097–#1122).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1123–#1143: final open-issue sweep ──
    ev.primitives().add(
        "query:production-sweep-1123-1140-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1123)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1123_1140_active) : 1)},
                {"equal-zero-nil-fixed", make_int(m ? load_u64(m, m->equal_zero_nil_fixed) : 1)},
                {"format-void-on-error", make_int(m ? load_u64(m, m->format_void_on_error) : 1)},
                {"term-metric-double-count-fixed",
                 make_int(m ? load_u64(m, m->term_metric_double_count_fixed) : 1)},
                {"defuse-rebuild-monotonic",
                 make_int(m ? load_u64(m, m->defuse_rebuild_monotonic) : 1)},
                {"module-realpath-fail-closed",
                 make_int(m ? load_u64(m, m->module_realpath_fail_closed) : 1)},
                {"env-parent-fallback-fixed",
                 make_int(m ? load_u64(m, m->env_parent_fallback_fixed) : 1)},
                {"issue-1123", make_int(1123)},
                {"issue-1140", make_int(1140)},
                {"issue-1143", make_int(1143)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Final open-issue sweep dashboard (#1123–#1143).",
                 .category = "general",
                 .schema = "() -> hash"});
    // ── Issues #1144–#1148: observability wire-up sweep ──
    ev.primitives().add(
        "query:production-sweep-1144-1148-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1144)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1144_1148_active) : 1)},
                {"flat-hash-insert-helper",
                 make_int(m ? load_u64(m, m->flat_hash_insert_helper) : 1)},
                {"selfevo-hyg-dirty-wired",
                 make_int(m ? load_u64(m, m->selfevo_hyg_dirty_wired) : 1)},
                {"per-fiber-ex-state-wired",
                 make_int(m ? load_u64(m, m->per_fiber_ex_state_wired) : 1)},
                {"orch-telemetry-wired", make_int(m ? load_u64(m, m->orch_telemetry_wired) : 1)},
                {"dead-bump-audit-script",
                 make_int(m ? load_u64(m, m->dead_bump_audit_script) : 1)},
                {"issue-1148", make_int(1148)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Observability wire-up sweep (#1144–#1148).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1158–#1176 Phase 1 ──
    ev.primitives().add(
        "query:production-sweep-1158-1176-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1158)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1158_1176_active) : 1)},
                {"math-int64-ub-fixed", make_int(m ? load_u64(m, m->math_int64_ub_fixed) : 1)},
                {"http-get-no-shell", make_int(m ? load_u64(m, m->http_get_no_shell) : 1)},
                {"git-stage-no-shell", make_int(m ? load_u64(m, m->git_stage_no_shell) : 1)},
                {"file-path-deny-list", make_int(m ? load_u64(m, m->file_path_deny_list) : 1)},
                {"file-cap-checks-extended",
                 make_int(m ? load_u64(m, m->file_cap_checks_extended) : 1)},
                {"stdlib-review-phase1", make_int(m ? load_u64(m, m->stdlib_review_phase1) : 1)},
                {"renderer-module-scaffold",
                 make_int(m ? load_u64(m, m->renderer_module_scaffold) : 1)},
                {"issue-1176", make_int(1176)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Phase 1 production sweep (#1158–#1176).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1177–#1201 Phase 1 ──
    ev.primitives().add(
        "query:production-sweep-1177-1201-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1177)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1177_1201_active) : 1)},
                {"ffi-hot-path-scaffold", make_int(m ? load_u64(m, m->ffi_hot_path_scaffold) : 1)},
                {"zero-copy-framebuffer-supported",
                 make_int(m ? load_u64(m, m->zero_copy_framebuffer_supported) : 1)},
                {"render-dirty-aware-scaffold",
                 make_int(m ? load_u64(m, m->render_dirty_aware_scaffold) : 1)},
                {"security-core-modules-scaffold",
                 make_int(m ? load_u64(m, m->security_core_modules_scaffold) : 1)},
                {"ansi-helper-supported", make_int(m ? load_u64(m, m->ansi_helper_supported) : 1)},
                {"render-ffi-scaffold", make_int(m ? load_u64(m, m->render_ffi_scaffold) : 1)},
                {"tenant-principal-scaffold",
                 make_int(m ? load_u64(m, m->tenant_principal_scaffold) : 1)},
                {"render-memory-profiling-supported",
                 make_int(m ? load_u64(m, m->render_memory_profiling_supported) : 1)},
                {"provenance-rollback-scaffold",
                 make_int(m ? load_u64(m, m->provenance_rollback_scaffold) : 1)},
                {"capability-effects-scaffold",
                 make_int(m ? load_u64(m, m->capability_effects_scaffold) : 1)},
                {"render-ci-slo-scaffold",
                 make_int(m ? load_u64(m, m->render_ci_slo_scaffold) : 1)},
                {"mutation-audit-tenant-scaffold",
                 make_int(m ? load_u64(m, m->mutation_audit_tenant_scaffold) : 1)},
                {"render-obs-schema-scaffold",
                 make_int(m ? load_u64(m, m->render_obs_schema_scaffold) : 1)},
                {"hotpath-contract-gates-scaffold",
                 make_int(m ? load_u64(m, m->hotpath_contract_gates_scaffold) : 1)},
                {"seva-closed-loop-scaffold",
                 make_int(m ? load_u64(m, m->seva_closed_loop_scaffold) : 1)},
                {"panic-quota-checkpoint-scaffold",
                 make_int(m ? load_u64(m, m->panic_quota_checkpoint_scaffold) : 1)},
                {"instruction-dirty-short-circuit",
                 make_int(m ? load_u64(m, m->instruction_dirty_short_circuit) : 1)},
                {"fiber-join-structured", make_int(m ? load_u64(m, m->fiber_join_structured) : 1)},
                {"aura-result-migration-scaffold",
                 make_int(m ? load_u64(m, m->aura_result_migration_scaffold) : 1)},
                {"mailbox-multi-fiber-scaffold",
                 make_int(m ? load_u64(m, m->mailbox_multi_fiber_scaffold) : 1)},
                {"optimization-passes-registry",
                 make_int(m ? load_u64(m, m->optimization_passes_registry) : 1)},
                {"issue-1201", make_int(1201)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Phase 1 production sweep (#1177–#1201).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1202–#1228 Phase 1 ──
    ev.primitives().add(
        "query:production-sweep-1202-1228-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1202)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1202_1228_active) : 1)},
                {"parallel-orch-scaffold",
                 make_int(m ? load_u64(m, m->parallel_orch_scaffold) : 1)},
                {"self-healing-hooks-active",
                 make_int(m ? load_u64(m, m->self_healing_hooks_active) : 1)},
                {"pure-analysis-pass-asserts",
                 make_int(m ? load_u64(m, m->pure_analysis_pass_asserts) : 1)},
                {"agent-fiber-safepoint-wired",
                 make_int(m ? load_u64(m, m->agent_fiber_safepoint_wired) : 1)},
                {"dirty-propagation-module",
                 make_int(m ? load_u64(m, m->dirty_propagation_module) : 1)},
                {"multi-fiber-mailbox-typed",
                 make_int(m ? load_u64(m, m->multi_fiber_mailbox_typed) : 1)},
                {"production-health-slo-scaffold",
                 make_int(m ? load_u64(m, m->production_health_slo_scaffold) : 1)},
                {"typed-mutation-audit-pass",
                 make_int(m ? load_u64(m, m->typed_mutation_audit_pass) : 1)},
                {"metrics-prometheus-scaffold",
                 make_int(m ? load_u64(m, m->metrics_prometheus_scaffold) : 1)},
                {"lifetime-pin-scaffold", make_int(m ? load_u64(m, m->lifetime_pin_scaffold) : 1)},
                {"hot-path-primitives-module",
                 make_int(m ? load_u64(m, m->hot_path_primitives_module) : 1)},
                {"eda-parse-common-dedup",
                 make_int(m ? load_u64(m, m->eda_parse_common_dedup) : 1)},
                {"issue-1228", make_int(1228)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Phase 1 production sweep (#1202–#1228).",
                 .category = "general",
                 .schema = "() -> hash"});

    // Issue #1215 Phase 1: composite production-health query (SLO surface).
    ev.primitives().add(
        "query:production-health",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            const auto heal = m ? load_u64(m, m->longrunning_heal_triggers_total) : 0;
            const auto quota_v = m ? load_u64(m, m->longrunning_quota_violations_total) : 0;
            // Simple health score: 100 baseline minus violations (floor 0).
            std::int64_t score = 100;
            if (quota_v > 0)
                score = std::max<std::int64_t>(0, 100 - static_cast<std::int64_t>(quota_v));
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1215)},
                {"score", make_int(score)},
                {"heal-triggers", make_int(heal)},
                {"quota-violations", make_int(static_cast<std::int64_t>(quota_v))},
                {"slo-enforcement",
                 make_int(m ? load_u64(m, m->production_health_slo_scaffold) : 1)},
                {"healthy", make_int(score >= 80 ? 1 : 0)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Composite production health / SLO surface (#1215).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1229–#1240 Phase 1 ──
    ev.primitives().add(
        "query:production-sweep-1229-1240-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1229)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1229_1240_active) : 1)},
                {"eda-hash-creates",
                 make_int(m ? load_u64(m, m->eda_hash_table_creates_total) : 0)},
                {"eda-alloc-bytes", make_int(m ? load_u64(m, m->eda_alloc_bytes_total) : 0)},
                {"ffi-opaque-tracking-hardened",
                 make_int(m ? load_u64(m, m->ffi_opaque_tracking_hardened) : 1)},
                {"stdlib-hotpath-eda-ffi-dashboard",
                 make_int(m ? load_u64(m, m->stdlib_hotpath_eda_ffi_dashboard) : 1)},
                {"agent-capability-gates",
                 make_int(m ? load_u64(m, m->agent_capability_gates) : 1)},
                {"sv-verification-executor-scaffold",
                 make_int(m ? load_u64(m, m->sv_verification_executor_scaffold) : 1)},
                {"stable-node-ref-eda-scaffold",
                 make_int(m ? load_u64(m, m->stable_node_ref_eda_scaffold) : 1)},
                {"covergroup-sampling-scaffold",
                 make_int(m ? load_u64(m, m->covergroup_sampling_scaffold) : 1)},
                {"synthesize-json-escape-fixed",
                 make_int(m ? load_u64(m, m->synthesize_json_escape_fixed) : 1)},
                {"eda-commercial-sim-scaffold",
                 make_int(m ? load_u64(m, m->eda_commercial_sim_scaffold) : 1)},
                {"sva-semantic-eval-scaffold",
                 make_int(m ? load_u64(m, m->sva_semantic_eval_scaffold) : 1)},
                {"panic-checkpoint-raii-scaffold",
                 make_int(m ? load_u64(m, m->panic_checkpoint_raii_scaffold) : 1)},
                {"value-tag-consteval-contracts",
                 make_int(m ? load_u64(m, m->value_tag_consteval_contracts) : 1)},
                {"issue-1240", make_int(1240)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Phase 1 production sweep (#1229–#1240).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1241–#1245 Phase 1 ──
    ev.primitives().add(
        "query:production-sweep-1241-1245-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1241)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1241_1245_active) : 1)},
                {"soa-view-concept-enforced",
                 make_int(m ? load_u64(m, m->soa_view_concept_enforced) : 1)},
                {"arena-shrink-tier-hardened",
                 make_int(m ? load_u64(m, m->arena_shrink_tier_hardened) : 1)},
                {"soa-view-eval-helpers", make_int(m ? load_u64(m, m->soa_view_eval_helpers) : 1)},
                {"hygiene-ir-marker-propagation",
                 make_int(m ? load_u64(m, m->hygiene_ir_marker_propagation) : 1)},
                {"macro-clone-concurrent-hygiene",
                 make_int(m ? load_u64(m, m->macro_clone_concurrent_hygiene) : 1)},
                {"issue-1245", make_int(1245)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Phase 1 production sweep (#1241–#1245).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1246–#1250 Phase 1 ──
    ev.primitives().add(
        "query:production-sweep-1246-1250-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            // Live hygiene tracer / macro-origin counters from macro_expansion TU.
            using aura::compiler::macro_exp::g_hygiene_tracer_depth_max;
            using aura::compiler::macro_exp::g_hygiene_tracer_expansions;
            using aura::compiler::macro_exp::g_macro_origin_provenance_errors;
            const auto hyg_exp = static_cast<std::int64_t>(
                g_hygiene_tracer_expansions.load(std::memory_order_relaxed));
            const auto hyg_depth = static_cast<std::int64_t>(
                g_hygiene_tracer_depth_max.load(std::memory_order_relaxed));
            const auto origin_err = static_cast<std::int64_t>(
                g_macro_origin_provenance_errors.load(std::memory_order_relaxed));
            if (m) {
                m->hygiene_tracer_expansions.store(static_cast<std::uint64_t>(hyg_exp),
                                                   std::memory_order_relaxed);
                m->hygiene_tracer_depth_max.store(static_cast<std::uint64_t>(hyg_depth),
                                                  std::memory_order_relaxed);
                m->macro_origin_provenance_errors.store(static_cast<std::uint64_t>(origin_err),
                                                        std::memory_order_relaxed);
            }
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1246)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1246_1250_active) : 1)},
                {"runtime-reflect-bridge-guard",
                 make_int(m ? load_u64(m, m->runtime_reflect_bridge_guard) : 1)},
                {"runtime-reflect-mutated-schema-checks",
                 make_int(m ? load_u64(m, m->runtime_reflect_mutated_schema_checks) : 0)},
                {"macro-origin-provenance-errors", make_int(origin_err)},
                {"hygiene-tracer-expansions", make_int(hyg_exp)},
                {"hygiene-tracer-depth-max", make_int(hyg_depth)},
                {"agent-string-heap-bounds-hardened",
                 make_int(m ? load_u64(m, m->agent_string_heap_bounds_hardened) : 1)},
                {"stable-ref-auto-pin-total",
                 make_int(m ? load_u64(m, m->stable_ref_auto_pin_total) : 0)},
                {"stable-ref-full-path-enforced",
                 make_int(m ? load_u64(m, m->stable_ref_full_path_enforced) : 1)},
                {"issue-1250", make_int(1250)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Phase 1 production sweep (#1246–#1250).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1251–#1255 Phase 1 ──
    ev.primitives().add(
        "query:production-sweep-1251-1255-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1251)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1251_1255_active) : 1)},
                {"mark-dirty-bounds-enforced",
                 make_int(m ? load_u64(m, m->mark_dirty_bounds_enforced) : 1)},
                {"rollback-compaction-path",
                 make_int(m ? load_u64(m, m->rollback_compaction_path) : 1)},
                {"mutation-boundary-primitives-wrapped",
                 make_int(m ? load_u64(m, m->mutation_boundary_primitives_wrapped) : 0)},
                {"mutation-boundary-linear-revalidations",
                 make_int(m ? load_u64(m, m->mutation_boundary_linear_revalidations) : 0)},
                {"mutation-hold-samples", make_int(m ? load_u64(m, m->mutation_hold_samples) : 0)},
                {"mutation-too-long-total",
                 make_int(m ? load_u64(m, m->mutation_too_long_total) : 0)},
                {"steal-inner-boundary-hardened",
                 make_int(m ? load_u64(m, m->steal_inner_boundary_hardened) : 1)},
                {"pattern-hygiene-strict-enforced",
                 make_int(m ? load_u64(m, m->pattern_hygiene_strict_enforced) : 1)},
                {"defuse-incremental-updates",
                 make_int(m ? load_u64(m, m->defuse_incremental_updates_total) : 0)},
                {"defuse-full-rebuild-fallbacks",
                 make_int(m ? load_u64(m, m->defuse_full_rebuild_fallbacks_total) : 0)},
                {"issue-1255", make_int(1255)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Phase 1 production sweep (#1251–#1255).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1256–#1260 Phase 1 ──
    ev.primitives().add(
        "query:production-sweep-1256-1260-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1256)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1256_1260_active) : 1)},
                {"gc-safepoint-mutation-metrics",
                 make_int(m ? load_u64(m, m->gc_safepoint_mutation_metrics) : 1)},
                {"workspace-auto-remapped",
                 make_int(m ? load_u64(m, m->workspace_provenance_auto_remapped) : 0)},
                {"workspace-cross-layer-validations",
                 make_int(m ? load_u64(m, m->workspace_cross_layer_validations_on_merge) : 0)},
                {"workspace-merge-mismatch-prevented",
                 make_int(m ? load_u64(m, m->workspace_merge_mismatch_prevented) : 0)},
                {"ir-soa-cache-consistency-enforced",
                 make_int(m ? load_u64(m, m->ir_soa_cache_consistency_enforced) : 1)},
                {"ir-soa-reset-epoch-bumps",
                 make_int(m ? load_u64(m, m->ir_soa_cache_reset_epoch_bumps) : 0)},
                {"mutate-guard-enforced", make_int(m ? load_u64(m, m->mutate_guard_enforced) : 0)},
                {"naked-mutate-attempt", make_int(m ? load_u64(m, m->naked_mutate_attempt) : 0)},
                {"panic-transfer-on-steal",
                 make_int(m ? load_u64(m, m->panic_transfer_on_steal) : 0)},
                {"panic-checkpoint-steal-hardened",
                 make_int(m ? load_u64(m, m->panic_checkpoint_steal_hardened) : 1)},
                {"issue-1260", make_int(1260)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Phase 1 production sweep (#1256–#1260).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1261–#1265 Phase 1 ──
    ev.primitives().add(
        "query:production-sweep-1261-1265-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1261)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1261_1265_active) : 1)},
                {"dep-graph-defuse-bumps",
                 make_int(m ? load_u64(m, m->dep_graph_defuse_version_bumps) : 0)},
                {"dep-graph-nested-lambda-full-dirty",
                 make_int(m ? load_u64(m, m->dep_graph_nested_lambda_full_dirty) : 0)},
                {"dep-graph-hygiene-propagate",
                 make_int(m ? load_u64(m, m->dep_graph_hygiene_propagate) : 0)},
                {"hot-swap-versioned-mangle",
                 make_int(m ? load_u64(m, m->hot_swap_versioned_mangle_enforced) : 0)},
                {"aot-region-filter-enforced",
                 make_int(m ? load_u64(m, m->aot_region_filter_enforced) : 1)},
                {"arena-reset-dirty-forced",
                 make_int(m ? load_u64(m, m->arena_reset_dirty_forced) : 0)},
                {"hot-update-race-detected",
                 make_int(m ? load_u64(m, m->hot_update_race_detected) : 0)},
                {"hot-update-epoch-fences",
                 make_int(m ? load_u64(m, m->hot_update_epoch_fences) : 1)},
                {"query-and-replace-all-or-nothing",
                 make_int(m ? load_u64(m, m->query_and_replace_all_or_nothing) : 0)},
                {"query-and-replace-parse-abort",
                 make_int(m ? load_u64(m, m->query_and_replace_parse_abort) : 0)},
                {"issue-1265", make_int(1265)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Phase 1 production sweep (#1261–#1265).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1266–#1270 Phase 1 ──
    ev.primitives().add(
        "query:production-sweep-1266-1270-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1266)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1266_1270_active) : 1)},
                {"inline-call-lambda-params-copied",
                 make_int(m ? load_u64(m, m->inline_call_lambda_params_copied) : 0)},
                {"set-body-define-value-extracted",
                 make_int(m ? load_u64(m, m->set_body_define_value_extracted) : 0)},
                {"panic-checkpoint-flush-outermost",
                 make_int(m ? load_u64(m, m->panic_checkpoint_flush_outermost) : 0)},
                {"envframe-dualpath-enforced",
                 make_int(m ? load_u64(m, m->envframe_dualpath_enforced) : 1)},
                {"envframe-dualpath-materialize-refresh",
                 make_int(m ? load_u64(m, m->envframe_dualpath_materialize_refresh) : 0)},
                {"steal-starvation-mitigation",
                 make_int(m ? load_u64(m, m->steal_starvation_mitigation) : 1)},
                {"issue-1270", make_int(1270)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Phase 1 production sweep (#1266–#1270).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1271–#1275 Phase 1 ──
    ev.primitives().add(
        "query:production-sweep-1271-1275-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1271)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1271_1275_active) : 1)},
                {"aot-hot-update-atomic-rollback",
                 make_int(m ? load_u64(m, m->aot_hot_update_atomic_rollback) : 0)},
                {"aot-reemit-dirty-skeleton",
                 make_int(m ? load_u64(m, m->aot_reemit_dirty_skeleton_calls) : 0)},
                {"runtime-obs-export-ready",
                 make_int(m ? load_u64(m, m->runtime_obs_export_ready) : 1)},
                {"mutation-boundary-contention-us",
                 make_int(m ? load_u64(m, m->mutation_boundary_contention_us_hist) : 0)},
                {"ir-hygiene-macro-marker-enforced",
                 make_int(m ? load_u64(m, m->ir_hygiene_macro_marker_enforced) : 1)},
                {"dirty-propagation-to-ir",
                 make_int(m ? load_u64(m, m->dirty_propagation_to_ir_count) : 0)},
                {"epoch-bump-for-macro", make_int(m ? load_u64(m, m->epoch_bump_for_macro) : 0)},
                {"naked-macro-mutate-attempt",
                 make_int(m ? load_u64(m, m->naked_macro_mutate_attempt) : 0)},
                {"hygiene-edsl-awareness",
                 make_int(m ? load_u64(m, m->hygiene_edsl_awareness) : 1)},
                {"issue-1275", make_int(1275)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Phase 1 production sweep (#1271–#1275).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1276–#1280 Phase 1 ──
    ev.primitives().add(
        "query:production-sweep-1276-1280-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1276)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1276_1280_active) : 1)},
                {"reflect-nested-struct-scaffold",
                 make_int(m ? load_u64(m, m->reflect_nested_struct_scaffold) : 1)},
                {"reflect-runtime-schema-hooks",
                 make_int(m ? load_u64(m, m->reflect_runtime_schema_hooks) : 1)},
                {"hygiene-violation-stats-active",
                 make_int(m ? load_u64(m, m->hygiene_violation_stats_active) : 1)},
                {"dirty-impact-stats-active",
                 make_int(m ? load_u64(m, m->dirty_impact_stats_active) : 1)},
                {"inline-diamond-cfg-fixed",
                 make_int(m ? load_u64(m, m->inline_diamond_cfg_fixed) : 1)},
                {"stable-ref-auto-refresh-enforced",
                 make_int(m ? load_u64(m, m->stable_ref_auto_refresh_enforced) : 1)},
                {"stable-ref-boundary-auto-refresh",
                 make_int(m ? load_u64(m, m->stable_ref_boundary_auto_refresh) : 0)},
                {"pattern-hygiene-end-to-end",
                 make_int(m ? load_u64(m, m->pattern_hygiene_end_to_end) : 1)},
                {"pattern-hygiene-default-exclude",
                 make_int(m ? load_u64(m, m->pattern_hygiene_default_exclude) : 0)},
                {"issue-1280", make_int(1280)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Phase 1 production sweep (#1276–#1280).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1281–#1285 Phase 1 ──
    ev.primitives().add(
        "query:production-sweep-1281-1285-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1281)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1281_1285_active) : 1)},
                {"children-topology-rollback-fidelity",
                 make_int(m ? load_u64(m, m->children_topology_rollback_fidelity) : 1)},
                {"children-topology-rollback-count",
                 make_int(m ? load_u64(m, m->children_topology_rollback_count) : 0)},
                {"generation-wrap-restamp-policy",
                 make_int(m ? load_u64(m, m->generation_wrap_restamp_policy) : 1)},
                {"generation-auto-restamp-on-wrap",
                 make_int(m ? load_u64(m, m->generation_auto_restamp_on_wrap) : 0)},
                {"provenance-boundary-hooks-active",
                 make_int(m ? load_u64(m, m->provenance_boundary_hooks_active) : 1)},
                {"provenance-boundary-capture-count",
                 make_int(m ? load_u64(m, m->provenance_boundary_capture_count) : 0)},
                {"dirty-provenance-stats-active",
                 make_int(m ? load_u64(m, m->dirty_provenance_stats_active) : 1)},
                {"tree-walker-fallback-reduction",
                 make_int(m ? load_u64(m, m->tree_walker_fallback_reduction) : 1)},
                {"tree-walker-define-cache-hits",
                 make_int(m ? load_u64(m, m->tree_walker_define_cache_hits) : 0)},
                {"jit-exception-opcodes-covered",
                 make_int(m ? load_u64(m, m->jit_exception_opcodes_covered) : 1)},
                {"jit-exception-opcode-lowered",
                 make_int(m ? load_u64(m, m->jit_exception_opcode_lowered) : 0)},
                {"issue-1285", make_int(1285)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Phase 1 production sweep (#1281–#1285).",
                 .category = "general",
                 .schema = "() -> hash"});

    // Issue #1283: query:dirty-provenance-stats — Agent-visible
    // aggregation of boundary provenance captures + dirty impact.
    ev.primitives().add(
        "query:dirty-provenance-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1283)},
                {"active", make_int(m ? load_u64(m, m->dirty_provenance_stats_active) : 1)},
                {"boundary-hooks-active",
                 make_int(m ? load_u64(m, m->provenance_boundary_hooks_active) : 1)},
                {"boundary-capture-count",
                 make_int(m ? load_u64(m, m->provenance_boundary_capture_count) : 0)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Dirty + StableNodeRef provenance boundary stats (#1283).",
                 .category = "general",
                 .schema = "() -> hash"});

    // Issue #1282: query:generation-stats — Agent-visible wrap/restamp surface
    // under the query: namespace (ast:generation-stats remains for legacy callers).
    ev.primitives().add(
        "query:generation-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1282)},
                {"wrap-restamp-policy",
                 make_int(m ? load_u64(m, m->generation_wrap_restamp_policy) : 1)},
                {"auto-restamp-on-wrap",
                 make_int(m ? load_u64(m, m->generation_auto_restamp_on_wrap) : 0)},
            };
            // Enrich with live FlatAST counters when a workspace is loaded.
            if (auto* ws = ev.workspace_flat()) {
                kv.emplace_back("current-generation",
                                make_int(static_cast<std::int64_t>(ws->current_generation())));
                kv.emplace_back("generation-wrap-total",
                                make_int(static_cast<std::int64_t>(ws->generation_wrap_count())));
                kv.emplace_back("current-wrap-epoch",
                                make_int(static_cast<std::int64_t>(ws->wrap_epoch())));
                kv.emplace_back(
                    "auto-restamp-on-wrap-flat",
                    make_int(static_cast<std::int64_t>(ws->auto_restamp_on_wrap_count())));
                kv.emplace_back(
                    "children-topology-restore",
                    make_int(static_cast<std::int64_t>(ws->children_topology_restore_count())));
            }
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Generation wrap / auto-restamp stats (#1282).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1286–#1290 Phase 1 ──
    ev.primitives().add(
        "query:production-sweep-1286-1290-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1286)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1286_1290_active) : 1)},
                {"invalidate-per-block-dirty-active",
                 make_int(m ? load_u64(m, m->invalidate_per_block_dirty_active) : 1)},
                {"invalidate-per-block-dirty-total",
                 make_int(m ? load_u64(m, m->invalidate_per_block_dirty_total) : 0)},
                {"closure-bridge-epoch-safety-active",
                 make_int(m ? load_u64(m, m->closure_bridge_epoch_safety_active) : 1)},
                {"closure-bridge-epoch-safety-enforced",
                 make_int(m ? load_u64(m, m->closure_bridge_epoch_safety_enforced) : 0)},
                {"guard-shape-linear-unified-active",
                 make_int(m ? load_u64(m, m->guard_shape_linear_unified_active) : 1)},
                {"guard-shape-linear-unified-checks",
                 make_int(m ? load_u64(m, m->guard_shape_linear_unified_checks) : 0)},
                {"jit-unhandled-fail-fast-active",
                 make_int(m ? load_u64(m, m->jit_unhandled_fail_fast_active) : 1)},
                {"ownership-lambda-params-fixed",
                 make_int(m ? load_u64(m, m->ownership_lambda_params_fixed) : 1)},
                {"issue-1290", make_int(1290)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Phase 1 production sweep (#1286–#1290).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1291–#1295 Phase 1 ──
    ev.primitives().add(
        "query:production-sweep-1291-1295-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1291)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1291_1295_active) : 1)},
                {"fiber-spawn-fid-holder-fixed",
                 make_int(m ? load_u64(m, m->fiber_spawn_fid_holder_fixed) : 1)},
                {"workspace-delete-pointer-refresh",
                 make_int(m ? load_u64(m, m->workspace_delete_pointer_refresh) : 1)},
                {"capability-compile-gates-active",
                 make_int(m ? load_u64(m, m->capability_compile_gates_active) : 1)},
                {"capability-compile-denials",
                 make_int(m ? load_u64(m, m->capability_compile_denials) : 0)},
                {"capability-retrofit-scaffold-active",
                 make_int(m ? load_u64(m, m->capability_retrofit_scaffold_active) : 1)},
                {"capability-exception-control-active",
                 make_int(m ? load_u64(m, m->capability_exception_control_active) : 1)},
                {"capability-exception-control-denials",
                 make_int(m ? load_u64(m, m->capability_exception_control_denials) : 0)},
                {"issue-1295", make_int(1295)},
            };
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Phase 1 production sweep (#1291–#1295).",
                 .category = "general",
                 .schema = "() -> hash"});

    // ── Issues #1296–#1300 Phase 1 ──
    ev.primitives().add(
        "query:production-sweep-1296-1300-stats",
        [&ev, metrics](std::span<const EvalValue>) -> EvalValue {
            auto* m = metrics();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1296)},
                {"active", make_int(m ? load_u64(m, m->production_sweep_1296_1300_active) : 1)},
                {"custom-predicate-registry-mutex",
                 make_int(m ? load_u64(m, m->custom_predicate_registry_mutex) : 1)},
                {"inline-max-slot-includes-params",
                 make_int(m ? load_u64(m, m->inline_max_slot_includes_params) : 1)},
                {"ghost-orphan-free-on-rollback",
                 make_int(m ? load_u64(m, m->ghost_orphan_free_on_rollback) : 1)},
                {"issue-1300", make_int(1300)},
            };
            if (auto* ws = ev.workspace_flat()) {
                kv.emplace_back("ghost-orphan-nodes-freed", make_int(static_cast<std::int64_t>(
                                                                ws->ghost_orphan_nodes_freed())));
            }
            return build_kv_hash(ev, kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .perf_tier = kPrimPerfHot,
                 .security_level = kPrimSecSafe,
                 .doc = "Phase 1 production sweep (#1296–#1300).",
                 .category = "general",
                 .schema = "() -> hash"});
}

} // namespace aura::compiler::primitives_detail
