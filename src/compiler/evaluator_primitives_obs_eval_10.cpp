// evaluator_primitives_obs_eval_10.cpp — Issue #909: peeled domain registration from observability
// monolith aura.compiler.evaluator module partition.

module;

#include "runtime_shared.h"
#include "compiler/aura_jit_bridge.h"
#include "observability_metrics.h"
#include "compiler/shape.h"
#include "compiler/value_tags.h"
#include "core/cpp26_contract_stats.h"
#include "core/arena_auto_policy_stats.h"
#include "jit_typed_mutation_stats.h"
#include "shape_jit_pass_closedloop_stats.h"
#include "ci_build_info.h"
#include "primitives_meta.h"
#include "primitives_detail.h"
#include "serve/metrics.h"
#include "hash_meta.h"
#include "basis_points.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.value;
import aura.compiler.pass_manager;

extern "C" std::uint64_t aura_fiber_static_steal_outermost_mutation_boundary_total();
extern "C" std::uint64_t aura_fiber_static_steal_inner_mutation_boundary_deferred_total();
extern "C" std::uint64_t aura_fiber_static_cross_fiber_mutation_safe_steal_total();
extern "C" std::uint64_t aura_fiber_init_aura_result_ok_total();
extern "C" std::uint64_t aura_fiber_init_aura_result_err_total();
extern "C" std::uint64_t aura_scheduler_init_aura_result_ok_total();
extern "C" std::uint64_t aura_scheduler_init_aura_result_err_total();
extern "C" std::uint64_t aura_jit_guest_exception_bridge_total();

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_keyword;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

// Issue #909 part 80 (orig lines 9552-9617)
void ObservabilityPrims::register_eval_p80(PrimRegistrar add, Evaluator& ev) {

    // Issue #750: query:reflection-schema-stats — Runtime reflection schema
    // validation bridge for macro bodies + EDSL structs under Guard mutate
    // (refines #734; non-duplicative with #454 reflect-edsl-bridge, #502
    // reflect-postmutate, #654 macro-hygiene-fiber-panic).
    //
    // Fields (4 + sentinel):
    //   - validated                  reflection_schema_validated_total
    //   - hygiene-invariants-held    reflection_macro_provenance_held_total
    //   - schema-violations          reflection_schema_violations_total
    //   - stale-validation-prevented reflection_stale_validation_prevented_total
    //   - schema == 750
    add("query:reflection-schema-stats", [&ev](const auto&) -> EvalValue {
        const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
        const std::int64_t validated =
            m ? static_cast<std::int64_t>(
                    m->reflection_schema_validated_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hygiene_held =
            m ? static_cast<std::int64_t>(
                    m->reflection_macro_provenance_held_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t violations =
            m ? static_cast<std::int64_t>(
                    m->reflection_schema_violations_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t stale_prev =
            m ? static_cast<std::int64_t>(
                    m->reflection_stale_validation_prevented_total.load(std::memory_order_relaxed))
              : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("validated", validated);
        insert_kv("hygiene-invariants-held", hygiene_held);
        insert_kv("schema-violations", violations);
        insert_kv("stale-validation-prevented", stale_prev);
        insert_kv("schema", 750);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 81 (orig lines 9618-9691)
void ObservabilityPrims::register_eval_p81(PrimRegistrar add, Evaluator& ev) {

    // Issue #659: query:typesystem-typed-mutate-stats — 5 type system gaps for
    // AI multi-round typed mutation (solve_delta reverify, dead coercion elim,
    // linear ownership post-mutate, occurrence provenance refresh, coercion map
    // incremental). Non-duplicative with #656 Lambda param recheck, #657
    // compiler-core-incremental, #690 constraint-typed-mutate-stats.
    //
    // Fields (5 + sentinel):
    //   - delta-reverify-scans           delta_conflict_reverify_total
    //   - dead-coercion-eliminated       dead_coercion_eliminated_total
    //   - linear-post-mutate-revalidations linear_post_mutate_revalidations_total
    //   - narrowing-provenance-refresh   narrowing_provenance_total
    //   - coercion-incremental-wins      coercion_zerooverhead_win_total
    //   - schema == 659
    add("query:typesystem-typed-mutate-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t reverify =
            m ? static_cast<std::int64_t>(
                    m->delta_conflict_reverify_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t dce =
            m ? static_cast<std::int64_t>(
                    m->dead_coercion_eliminated_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t linear =
            m ? static_cast<std::int64_t>(
                    m->linear_post_mutate_revalidations_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t provenance =
            m ? static_cast<std::int64_t>(
                    m->narrowing_provenance_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t coercion =
            m ? static_cast<std::int64_t>(
                    m->coercion_zerooverhead_win_total.load(std::memory_order_relaxed))
              : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("delta-reverify-scans", reverify);
        insert_kv("dead-coercion-eliminated", dce);
        insert_kv("linear-post-mutate-revalidations", linear);
        insert_kv("narrowing-provenance-refresh", provenance);
        insert_kv("coercion-incremental-wins", coercion);
        insert_kv("schema", 659);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 82 (orig lines 9692-9770)
void ObservabilityPrims::register_eval_p82(PrimRegistrar add, Evaluator& ev) {

    // Issue #673: query:runtime-observability-correlated-stats — Unified
    // Runtime Observability Layer (P1) cross-module correlation primitive.
    //
    // The other observability primitives (#527, #529, #506, #480, #598,
    // #548, #599, #592, #593, #596, #591, #438, #448 et al.) each cover
    // a single module's stats. #673 ships the FIRST dedicated
    // correlation counters that resolve cross-module events to a single
    // signal: "mutation during steal" / "ownership violation rate during
    // steal" / "GC deferred by boundary" (3 of the 4 concrete gaps
    // identified in the issue body).
    //
    // Fields (4 + sentinel):
    //   - steal-attempts-correlated
    //       runtime_observability_steal_attempt_correlated_total
    //       (any steal attempt; baseline denominator)
    //   - steal-deferred-correlated
    //       runtime_observability_steal_deferred_correlated_total
    //       (steal deferred at an active MutationBoundary — the
    //       "mutation during steal" correlation)
    //   - steal-ownership-violation-correlated
    //       runtime_observability_steal_ownership_violation_correlated_total
    //       (linear ownership violation caught during steal probe —
    //       the "ownership violation rate during steal" correlation)
    //   - correlated-events-total
    //       Sum of the 3 correlated counters above (per-call derivation,
    //       not a separate atomic). Lets dashboards show overall
    //       correlated-event volume at a glance.
    //   - schema == 673
    //
    // Non-duplicative with #591 scheduler-mutation-coord-stats,
    // #438 fiber-migration-stats, #448 mutation-coordination-stats,
    // #599 compiler-root-stats, #592 panic-checkpoint-fiber-stats,
    // #596 guard-panic-reflect-stats — each of those exposes its own
    // module-local view; this primitive is the FIRST unified view.
    add("query:runtime-observability-correlated-stats", [&ev](const auto&) -> EvalValue {
        const std::int64_t steal_attempts =
            static_cast<std::int64_t>(ev.get_runtime_observability_steal_attempt_correlated());
        const std::int64_t steal_deferred =
            static_cast<std::int64_t>(ev.get_runtime_observability_steal_deferred_correlated());
        const std::int64_t ownership_violation = static_cast<std::int64_t>(
            ev.get_runtime_observability_steal_ownership_violation_correlated());
        const std::int64_t correlated_total = steal_attempts + steal_deferred + ownership_violation;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("steal-attempts-correlated", steal_attempts);
        insert_kv("steal-deferred-correlated", steal_deferred);
        insert_kv("steal-ownership-violation-correlated", ownership_violation);
        insert_kv("correlated-events-total", correlated_total);
        insert_kv("schema", 673);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 83 (orig lines 9771-9848)
void ObservabilityPrims::register_eval_p83(PrimRegistrar add, Evaluator& ev) {

    // Issue #674: query:self-evolution-chaos-stats — Closed-loop
    // self-evolution stability stress testing (P0) outcome
    // classifier. Companion primitive for the chaos stress
    // harness that drives 1000+ mutation cycles under fiber
    // steal + GC + AOT hot-reload conditions. The 3 fields
    // are the "outcome classifier" of each chaos cycle:
    //
    //   - chaos-cycles      self_evolution_chaos_cycles_total
    //       Bumped by the chaos harness once per full chaos
    //       mutation cycle (one attempted self-evolution,
    //       regardless of outcome). The "1000+ mutations" sum
    //       the issue body calls out as the long-running
    //       stress baseline.
    //   - chaos-failures    self_evolution_chaos_failures_total
    //       Bumped by the chaos harness when a chaos mutation
    //       cycle fails (post-mutation validation, rollback,
    //       or panic). The "evolution success rate" denominator.
    //   - chaos-corruptions self_evolution_chaos_corruptions_total
    //       Bumped by the chaos harness when a version/ownership
    //       mismatch is detected during a chaos cycle. The
    //       "corruption detected per epoch" metric from the
    //       issue body.
    //   - chaos-events-total
    //       Sum of the 3 (per-call derivation, not a separate
    //       atomic). Lets dashboards show overall chaos-event
    //       volume at a glance.
    //   - schema == 674
    //
    // Non-duplicative with #548 panic-checkpoint-lifecycle,
    // #529 atomic-batch-rollback, #527 stable-ref-cow-fiber,
    // #400 mutation-rollback-coverage, #679 nested-Guard
    // atomic-batch-rollback. Those cover the production
    // counter set; #674 covers the chaos/stress-test
    // outcome classifier.
    add("query:self-evolution-chaos-stats", [&ev](const auto&) -> EvalValue {
        const std::int64_t cycles = static_cast<std::int64_t>(ev.get_self_evolution_chaos_cycles());
        const std::int64_t failures =
            static_cast<std::int64_t>(ev.get_self_evolution_chaos_failures());
        const std::int64_t corruptions =
            static_cast<std::int64_t>(ev.get_self_evolution_chaos_corruptions());
        const std::int64_t events_total = cycles + failures + corruptions;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("chaos-cycles", cycles);
        insert_kv("chaos-failures", failures);
        insert_kv("chaos-corruptions", corruptions);
        insert_kv("chaos-events-total", events_total);
        insert_kv("schema", 674);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 84 (orig lines 9849-9934)
void ObservabilityPrims::register_eval_p84(PrimRegistrar add, Evaluator& ev) {

    // Issue #498: query:primitive-metadata — structured AI-native primitive
    // registry introspection for Agent development workflows.
    add("query:primitive-metadata", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(16);
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
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
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
        };
        const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t slots = ev.primitives_.slot_count();
        const std::uint64_t documented = ev.primitives_.documented_meta_count();
        const std::uint64_t schema_doc = ev.primitives_.schema_documented_meta_count();
        const std::uint64_t describes = ev.get_primitive_describe_count();
        const std::uint64_t list_meta = ev.get_primitive_list_meta_count();
        const std::uint64_t skeletons =
            m ? m->primitive_skeleton_generations_total.load(std::memory_order_relaxed) : 0;
        std::uint64_t pure_count = 0;
        std::uint64_t mutate_count = 0;
        for (std::size_t si = 0; si < slots; ++si) {
            const auto& pm = ev.primitives_.meta_for_slot(si);
            if (pm.pure)
                ++pure_count;
            if ((pm.safety_flags & kPrimSafetyMutates) != 0)
                ++mutate_count;
        }
        const std::uint64_t coverage_bp =
            slots > 0 ? (10000 * documented / slots) : (documented > 0 ? 10000 : 0);
        std::int64_t recommendation = 0;
        if (coverage_bp < 5000)
            recommendation = 1;
        else if (schema_doc < documented / 2)
            recommendation = 2;
        const std::uint64_t metadata_total = slots + documented + schema_doc + describes +
                                             list_meta + skeletons + pure_count + mutate_count;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"registry-slots", make_int(static_cast<std::int64_t>(slots))},
            {"documented-meta", make_int(static_cast<std::int64_t>(documented))},
            {"schema-documented", make_int(static_cast<std::int64_t>(schema_doc))},
            {"describe-calls", make_int(static_cast<std::int64_t>(describes))},
            {"list-meta-calls", make_int(static_cast<std::int64_t>(list_meta))},
            {"skeleton-generations", make_int(static_cast<std::int64_t>(skeletons))},
            {"pure-primitives", make_int(static_cast<std::int64_t>(pure_count))},
            {"mutate-primitives", make_int(static_cast<std::int64_t>(mutate_count))},
            {"meta-coverage-bp", make_int(static_cast<std::int64_t>(coverage_bp))},
            {"extension-kit-version",
             make_int(static_cast<std::int64_t>(kPrimitivesExtensionKitVersion))},
            {"metadata-recommendation", make_int(recommendation)},
            {"metadata-total", make_int(static_cast<std::int64_t>(metadata_total))},
        };
        return build_hash(kv);
    });
}

// Issue #909 part 85 (orig lines 9935-10010)
void ObservabilityPrims::register_eval_p85(PrimRegistrar add, Evaluator& ev) {

    // Issue #499: query:eda-foundation-stats — EDA primitives module
    // parse/query/mutate/waveform/feedback observability for Agent loops.
    add("query:eda-foundation-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(16);
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
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
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
        };
        const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t parse =
            m ? m->eda_foundation_parse_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t query =
            m ? m->eda_foundation_query_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t mutate =
            m ? m->eda_foundation_mutate_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t waveform =
            m ? m->eda_foundation_waveform_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t feedback =
            m ? m->eda_foundation_feedback_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t hooks =
            m ? m->hardware_backend_hook_calls_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t foundation_total = parse + query + mutate + waveform + feedback;
        std::int64_t recommendation = 0;
        if (parse == 0)
            recommendation = 1;
        else if (mutate == 0)
            recommendation = 2;
        else if (feedback == 0)
            recommendation = 3;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"parse-total", make_int(static_cast<std::int64_t>(parse))},
            {"query-total", make_int(static_cast<std::int64_t>(query))},
            {"mutate-total", make_int(static_cast<std::int64_t>(mutate))},
            {"waveform-total", make_int(static_cast<std::int64_t>(waveform))},
            {"feedback-total", make_int(static_cast<std::int64_t>(feedback))},
            {"hardware-hook-calls", make_int(static_cast<std::int64_t>(hooks))},
            {"foundation-total", make_int(static_cast<std::int64_t>(foundation_total))},
            {"foundation-recommendation", make_int(recommendation)},
        };
        return build_hash(kv);
    });
}

// Issue #909 part 86 (orig lines 10011-10096)
void ObservabilityPrims::register_eval_p86(PrimRegistrar add, Evaluator& ev) {

    // Issue #616: query:eda-hw-stats — EDA hardware-co-design
    // primitives observability. Companion to query:eda-foundation-stats
    // (#499) but covering the file-boundary surface (load-sv,
    // parse-verification-result). Separate primitive so the #499
    // foundation stats shape stays unchanged for back-compat, and
    // the file-I/O layer has its own dedicated dashboard.
    //
    // Returned hash:
    //   - load-sv-total               successful (eda:load-sv) calls
    //   - load-sv-failure-total       failed (eda:load-sv) calls
    //   - parse-verification-result-total successful calls
    //   - parse-verification-failure-total failed calls
    //   - load-sv-success-rate        0..100 (0 when both are 0)
    //   - parse-verification-success-rate 0..100 (0 when both are 0)
    //
    // The success-rate fields are computed inline so the Agent
    // doesn't have to do the division itself; the per-call counters
    // are also exposed so a custom Agent can compute its own
    // moving-window rate.
    add("query:eda-hw-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(16);
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
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
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
        };
        const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t load_sv_ok =
            m ? m->eda_load_sv_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t load_sv_fail =
            m ? m->eda_load_sv_failure_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t parse_vr_ok =
            m ? m->eda_parse_verification_result_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t parse_vr_fail =
            m ? m->eda_parse_verification_failure_total.load(std::memory_order_relaxed) : 0;
        const auto load_total = load_sv_ok + load_sv_fail;
        const auto parse_total = parse_vr_ok + parse_vr_fail;
        const std::int64_t load_rate =
            load_total == 0 ? 0 : static_cast<std::int64_t>((load_sv_ok * 100) / load_total);
        const std::int64_t parse_rate =
            parse_total == 0 ? 0 : static_cast<std::int64_t>((parse_vr_ok * 100) / parse_total);
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"load-sv-total", make_int(static_cast<std::int64_t>(load_sv_ok))},
            {"load-sv-failure-total", make_int(static_cast<std::int64_t>(load_sv_fail))},
            {"parse-verification-result-total", make_int(static_cast<std::int64_t>(parse_vr_ok))},
            {"parse-verification-failure-total",
             make_int(static_cast<std::int64_t>(parse_vr_fail))},
            {"load-sv-success-rate", make_int(load_rate)},
            {"parse-verification-success-rate", make_int(parse_rate)},
        };
        return build_hash(kv);
    });
}

// Issue #909 part 87 (orig lines 10097-10173)
void ObservabilityPrims::register_eval_p87(PrimRegistrar add, Evaluator& ev) {

    // Issue #841: query:eda-infra-stats — EDA production infrastructure
    // observability dashboard (refines #499/#616; non-duplicative with
    // query:eda-foundation-stats and query:eda-hw-stats).
    //
    // Fields (4 + sentinel):
    //   - parse-success-hits       eda_infra_parse_success_total
    //   - structured-mutate-hits   eda_infra_structured_mutate_total
    //   - feedback-ingest-hits     eda_infra_feedback_ingest_total
    //   - cosim-invoke-hits        eda_infra_cosim_invoke_total
    //   - schema == 841
    add("query:eda-infra-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
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
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
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
        };
        const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
        const std::int64_t parse_success =
            m ? static_cast<std::int64_t>(
                    m->eda_infra_parse_success_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t structured_mutate =
            m ? static_cast<std::int64_t>(
                    m->eda_infra_structured_mutate_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t feedback_ingest =
            m ? static_cast<std::int64_t>(
                    m->eda_infra_feedback_ingest_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t cosim_invoke =
            m ? static_cast<std::int64_t>(
                    m->eda_infra_cosim_invoke_total.load(std::memory_order_relaxed))
              : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"parse-success-hits", make_int(parse_success)},
            {"structured-mutate-hits", make_int(structured_mutate)},
            {"feedback-ingest-hits", make_int(feedback_ingest)},
            {"cosim-invoke-hits", make_int(cosim_invoke)},
            {"schema", make_int(841)},
        };
        return build_hash(kv);
    });
}

} // namespace aura::compiler::primitives_detail
