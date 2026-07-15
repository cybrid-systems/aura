// evaluator_primitives_obs_eval_02.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 16 (orig lines 2790-2849)
void ObservabilityPrims::register_eval_p16(PrimRegistrar add, Evaluator& ev) {

    // Issue #752: query:list-soa-hotpath-stats — P0 list/vector
    // map/filter SoA + intrinsic fast-dispatch observability
    // (refines #727; non-duplicative with #667 apply-loop
    // counters and #506 IR SoA adoption).
    //
    // Fields (4 + sentinel):
    //   - chain-traversals      list_chain_traversals_total
    //   - soa-hits              list_soa_hits_total
    //   - intrinsic-dispatches  list_intrinsic_dispatches_total
    //   - estimated-cache-misses list_estimated_cache_misses_total
    //   - hotpath-events-total  (sum of 4, per-call derivation)
    //   - schema == 752
    ObservabilityPrims::register_stats_impl(
        "query:list-soa-hotpath-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t chain_traversals =
                static_cast<std::int64_t>(ev.get_list_chain_traversals());
            const std::int64_t soa_hits = static_cast<std::int64_t>(ev.get_list_soa_hits());
            const std::int64_t intrinsic_dispatches =
                static_cast<std::int64_t>(ev.get_list_intrinsic_dispatches());
            const std::int64_t estimated_cache_misses =
                static_cast<std::int64_t>(ev.get_list_estimated_cache_misses());
            const std::int64_t events_total =
                chain_traversals + soa_hits + intrinsic_dispatches + estimated_cache_misses;
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
            insert_kv("chain-traversals", chain_traversals);
            insert_kv("soa-hits", soa_hits);
            insert_kv("intrinsic-dispatches", intrinsic_dispatches);
            insert_kv("estimated-cache-misses", estimated_cache_misses);
            insert_kv("hotpath-events-total", events_total);
            insert_kv("schema", 752);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 17 (orig lines 2850-2914)
void ObservabilityPrims::register_eval_p17(PrimRegistrar add, Evaluator& ev) {

    // Issue #753: query:longrunning-infra-stats — P0 long-running
    // deployment infra observability (refines #729; non-duplicative
    // with #548 panic-checkpoint-lifecycle, #677 deployment-stats,
    // #674 chaos-stats).
    //
    // Fields (5 + sentinel):
    //   - quota-violations       longrunning_quota_violations_total
    //   - checkpoint-success     longrunning_checkpoint_success_total
    //   - heal-triggers          longrunning_heal_triggers_total
    //   - resource-trend         longrunning_resource_trend_total
    //   - deployment-slo-hits    longrunning_deployment_slo_hits_total
    //   - infra-events-total     (sum of 5, per-call derivation)
    //   - schema == 753
    ObservabilityPrims::register_stats_impl(
        "query:longrunning-infra-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t quota_violations =
                static_cast<std::int64_t>(ev.get_longrunning_quota_violations());
            const std::int64_t checkpoint_success =
                static_cast<std::int64_t>(ev.get_longrunning_checkpoint_success());
            const std::int64_t heal_triggers =
                static_cast<std::int64_t>(ev.get_longrunning_heal_triggers());
            const std::int64_t resource_trend =
                static_cast<std::int64_t>(ev.get_longrunning_resource_trend());
            const std::int64_t deployment_slo_hits =
                static_cast<std::int64_t>(ev.get_longrunning_deployment_slo_hits());
            const std::int64_t events_total = quota_violations + checkpoint_success +
                                              heal_triggers + resource_trend + deployment_slo_hits;
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
            insert_kv("quota-violations", quota_violations);
            insert_kv("checkpoint-success", checkpoint_success);
            insert_kv("heal-triggers", heal_triggers);
            insert_kv("resource-trend", resource_trend);
            insert_kv("deployment-slo-hits", deployment_slo_hits);
            insert_kv("infra-events-total", events_total);
            insert_kv("schema", 753);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 18 (orig lines 2915-2976)
void ObservabilityPrims::register_eval_p18(PrimRegistrar add, Evaluator& ev) {

    // Issue #754: query:orchestration-llm-bottleneck-stats — P0 LLM-
    // bottleneck adaptive scheduling + yield-classification-driven
    // work-stealing bias + GC safepoint self-tuning (refines #730;
    // non-duplicative with #706 scheduler-stealbudget-adaptive-stats,
    // #650 yield-class-stats, #646 gc-safepoint-deferral-stats).
    //
    // Fields (4 + sentinel):
    //   - outermost-preferred   AdaptiveStealStats::outermost_preferred
    //   - backoff-triggers      AdaptiveStealStats::deferred_pressure_boosts
    //   - llm-tail-reduction    AdaptiveStealStats::llm_tail_reductions
    //   - gc-safepoint-adapted  orchestration_llm_gc_safepoint_adapted_total
    //   - orchestration-events-total (sum of 4, per-call derivation)
    //   - schema == 754
    ObservabilityPrims::register_stats_impl(
        "query:orchestration-llm-bottleneck-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t outermost_preferred =
                static_cast<std::int64_t>(aura_adaptive_steal_outermost_preferred());
            const std::int64_t backoff_triggers =
                static_cast<std::int64_t>(aura_adaptive_steal_deferred_pressure_boosts());
            const std::int64_t llm_tail_reduction =
                static_cast<std::int64_t>(aura_adaptive_steal_llm_tail_reductions());
            const std::int64_t gc_safepoint_adapted =
                static_cast<std::int64_t>(ev.get_orchestration_llm_gc_safepoint_adapted());
            const std::int64_t events_total =
                outermost_preferred + backoff_triggers + llm_tail_reduction + gc_safepoint_adapted;
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
            insert_kv("outermost-preferred", outermost_preferred);
            insert_kv("backoff-triggers", backoff_triggers);
            insert_kv("llm-tail-reduction", llm_tail_reduction);
            insert_kv("gc-safepoint-adapted", gc_safepoint_adapted);
            insert_kv("orchestration-events-total", events_total);
            insert_kv("schema", 754);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 19 (orig lines 2977-3037)
void ObservabilityPrims::register_eval_p19(PrimRegistrar add, Evaluator& ev) {

    // Issue #755: query:concurrent-safety-full-cycle-stats — P0 end-to-end
    // concurrent safety integration (MutationBoundary + steal + AOT + GC +
    // recovery; refines #732/#731/#730/#674/#739; non-duplicative with
    // #674 chaos-stats, #754 orchestration-llm-bottleneck-stats).
    //
    // Fields (4 + sentinel):
    //   - steal-boundary-success   concurrent_safety_steal_boundary_success_total
    //   - aot-reload-at-guard      concurrent_safety_aot_reload_at_guard_total
    //   - gc-safepoint-during-steal concurrent_safety_gc_safepoint_during_steal_total
    //   - recovery-success         concurrent_safety_recovery_success_total
    //   - safety-events-total      (sum of 4, per-call derivation)
    //   - schema == 755
    ObservabilityPrims::register_stats_impl(
        "query:concurrent-safety-full-cycle-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t steal_boundary_success =
                static_cast<std::int64_t>(ev.get_concurrent_safety_steal_boundary_success());
            const std::int64_t aot_reload_at_guard =
                static_cast<std::int64_t>(ev.get_concurrent_safety_aot_reload_at_guard());
            const std::int64_t gc_safepoint_during_steal =
                static_cast<std::int64_t>(ev.get_concurrent_safety_gc_safepoint_during_steal());
            const std::int64_t recovery_success =
                static_cast<std::int64_t>(ev.get_concurrent_safety_recovery_success());
            const std::int64_t events_total = steal_boundary_success + aot_reload_at_guard +
                                              gc_safepoint_during_steal + recovery_success;
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
            insert_kv("steal-boundary-success", steal_boundary_success);
            insert_kv("aot-reload-at-guard", aot_reload_at_guard);
            insert_kv("gc-safepoint-during-steal", gc_safepoint_during_steal);
            insert_kv("recovery-success", recovery_success);
            insert_kv("safety-events-total", events_total);
            insert_kv("schema", 755);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 20 (orig lines 3038-3164)
void ObservabilityPrims::register_eval_p20(PrimRegistrar add, Evaluator& ev) {

    // Issue #644: query:aot-reload-func-table-stats —
    // Agent-discoverable structured dashboard for the AOT
    // Hot-Reload func_table Refcount + Per-Region Isolation
    // (P0 Runtime-Gap + AOT production-readiness surface —
    // non-duplicative to #624 #601 #358).
    //
    // Note the naming distinction from #708:
    //   - (query:aot-reload-stats) (#708) — 5-field primitive
    //     focused on the high-level reload attempt / success /
    //     stale / refcount_swaps / region_violations summary
    //   - (query:aot-reload-func-table-stats) (#644, this
    //     primitive) — *enforcement-track* companion that
    //     focuses on the func_table refcount bump/decrement
    //     protocol + region filter re-apply wire-up
    //     (the AC1+AC2+AC4 enforcement counters that #708
    //     did not surface as a separate primitive).
    //
    // Fields (3 + sentinel):
    //   - ref-bump            new aot_func_table_ref_bump_total
    //                          atomic (foundation for AC1
    //                          atomic refcount bumps on new
    //                          func_table entry install).
    //                          Value is 0 until AC1 wire-up.
    //   - ref-decrement       new aot_func_table_ref_decrement_
    //                          total atomic (foundation for AC1
    //                          atomic refcount decrements on
    //                          old entry retirement after grace
    //                          period / epoch check). Value is
    //                          0 until AC1 wire-up.
    //   - region-reapply      new aot_region_filter_reapply_
    //                          total atomic (foundation for AC2
    //                          region filtering re-applied on
    //                          reload per agent/workspace).
    //                          Value is 0 until AC2 wire-up.
    //   - schema == 644         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633+#637+#640+#641+
    //                          #642+#643 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level AOT
    // reload observability surface:
    //   - (query:aot-reload-stats) (#708) — 5-field reload
    //     summary (attempts / success / stale / swaps /
    //     region_violations)
    //   - (query:aot-hot-reload-stats) (#358/#452) — earlier
    //     AOT hot-reload summary
    //   - (query:aot-checkpoint-version-stats) (#708) —
    //     checkpoint version tracking
    //   - aot_reload_attempts_ + aot_hot_update_success_ +
    //     aot_stale_reject_count_ + aot_refcount_swaps_ +
    //     aot_region_mismatch_ (#708) — high-level counters
    // What the issue body specifies by **exact enforcement
    // layer** — granular func_table refcount bump/decrement
    // + per-region filter re-apply counters for AC1+AC2+AC4
    // — was *not* shipped under that exact enforcement layer.
    // So #644 ships ONE new Aura primitive + 3 new foundation
    // atomics.
    //
    // The remaining #644 AC1 (func_table refcount swap
    // protocol) + AC2 (region filtering re-apply) + AC4
    // (MutationBoundaryGuard + fiber yield wire-up) work is
    // invasive C++ on aura_jit_bridge.cpp + hot-swap hooks +
    // service.ixx invalidate + needs the 1000+ reload cycles
    // + concurrent apply_closure + TSan coverage from the
    // issue body — separate follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:aot-reload-func-table-stats", [&ev](const auto&) -> EvalValue {
            // ref-bump: new foundation atomic
            // (0 until AC1 atomic refcount bumps wire-up).
            const std::uint64_t ref_bump =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->aot_func_table_ref_bump_total.load(std::memory_order_relaxed)
                    : 0;
            // ref-decrement: new foundation atomic
            // (0 until AC1 atomic refcount decrements wire-up).
            const std::uint64_t ref_decrement =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->aot_func_table_ref_decrement_total.load(std::memory_order_relaxed)
                    : 0;
            // region-reapply: new foundation atomic
            // (0 until AC2 region filtering re-apply wire-up).
            const std::uint64_t region_reapply =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->aot_region_filter_reapply_total.load(std::memory_order_relaxed)
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
            insert_kv("ref-bump", static_cast<std::int64_t>(ref_bump));
            insert_kv("ref-decrement", static_cast<std::int64_t>(ref_decrement));
            insert_kv("region-reapply", static_cast<std::int64_t>(region_reapply));
            insert_kv("schema", 644);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 21 (orig lines 3165-3290)
void ObservabilityPrims::register_eval_p21(PrimRegistrar add, Evaluator& ev) {

    // Issue #645: query:scheduler-steal-bias-stats —
    // Agent-discoverable structured dashboard for the
    // Work-Stealing LIFO/FIFO Adaptive Bias + YieldReason /
    // outermost Mutation Depth (P0 Runtime-Gap + Scheduler
    // production-readiness surface — non-duplicative to
    // #618 #588 #451).
    //
    // Note the naming distinction from #706:
    //   - (query:scheduler-stealbudget-adaptive-stats) (#706)
    //     is the steal-budget adaptive bias primitive
    //     (LLM-bottleneck adjustments — higher level
    //     orchestration tune)
    //   - (query:scheduler-steal-bias-stats) (#645, this
    //     primitive) is the *enforcement-track* companion
    //     that focuses on the per-steal LIFO/FIFO +
    //     mutation-deferred counters for AC1+AC2+AC4
    //     enforcement (lower level — what each steal
    //     decision consults).
    //
    // Fields (3 + sentinel):
    //   - lifo-hits            new scheduler_lifo_hits_total
    //                          atomic (foundation for AC1
    //                          LIFO local hits on worker
    //                          deque). Value is 0 until
    //                          AC1 wire-up.
    //   - fifo-steals          new scheduler_fifo_steals_total
    //                          atomic (foundation for AC1
    //                          FIFO steals from victim).
    //                          Value is 0 until AC1 wire-up.
    //   - mutation-deferred    new scheduler_mutation_deferred_
    //                          bias_total atomic (foundation
    //                          for AC1+AC2 deferred-steal
    //                          from inner-MutationBoundary
    //                          fibers + the simple adaptive
    //                          LIFO/FIFO tuning). Value is
    //                          0 until AC1+AC2 wire-up.
    //   - schema == 645         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633+#637+#640+#641+
    //                          #642+#643+#644 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level
    // scheduler adaptive bias surface:
    //   - (query:scheduler-stealbudget-adaptive-stats) (#706)
    //     — LLM-bottleneck adjustments (orchestration tune)
    //   - #618 per-fiber yield_reason classification +
    //     is_at_mutation_boundary_safe + outermost depth probe
    //   - #588 per-fiber stack + adaptive hints
    //   - #451 work-stealing deque LIFO local + FIFO steal +
    //     request_gc_safepoint
    // What the issue body AC3 specifies by **exact name +
    // fields** — `query:scheduler-steal-bias-stats` with
    // LIFO/FIFO + mutation-deferred counters — was *not*
    // shipped under that exact name. So #645 ships ONE new
    // Aura primitive + 3 new foundation atomics.
    //
    // The remaining #645 AC1 (steal loop consults
    // victim->last_yield_reason() + outermost depth) +
    // AC2 (simple adaptive LIFO/FIFO tuning) + AC4 (wire
    // to #618 orchestration tune) work is invasive C++
    // on worker steal loop + scheduler next_worker +
    // needs the 20+ fibers + LLM-sim latency matrix +
    // TSan coverage from the issue body — separate
    // follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:scheduler-steal-bias-stats", [&ev](const auto&) -> EvalValue {
            // lifo-hits: new foundation atomic
            // (0 until AC1 LIFO local hits wire-up).
            const std::uint64_t lifo_hits =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->scheduler_lifo_hits_total.load(std::memory_order_relaxed)
                    : 0;
            // fifo-steals: new foundation atomic
            // (0 until AC1 FIFO steals wire-up).
            const std::uint64_t fifo_steals =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->scheduler_fifo_steals_total.load(std::memory_order_relaxed)
                    : 0;
            // mutation-deferred: new foundation atomic
            // (0 until AC1+AC2 deferred-steal wire-up).
            const std::uint64_t mutation_deferred =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->scheduler_mutation_deferred_bias_total.load(std::memory_order_relaxed)
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
            insert_kv("lifo-hits", static_cast<std::int64_t>(lifo_hits));
            insert_kv("fifo-steals", static_cast<std::int64_t>(fifo_steals));
            insert_kv("mutation-deferred", static_cast<std::int64_t>(mutation_deferred));
            insert_kv("schema", 645);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 22 (orig lines 3291-3411)
void ObservabilityPrims::register_eval_p22(PrimRegistrar add, Evaluator& ev) {

    // Issue #646: query:gc-safepoint-deferral-stats —
    // Agent-discoverable structured dashboard for the GC
    // Safepoint Deferral + Backoff Only for Outermost
    // MutationBoundary + Contention Metrics (P0 Runtime-Gap
    // + GC production-readiness surface — non-duplicative to
    // #642 #623 #591).
    //
    // Note the naming distinction from existing primitives:
    //   - (query:gc-safepoint-stats) — base GC safepoint
    //     primitive (no deferral-specific breakdown)
    //   - (query:gc-safepoint-deferral-stats) (#646, this
    //     primitive) — *deferral-track* companion that
    //     focuses on the outermost-vs-inner deferral +
    //     backoff contention counters for AC1+AC2+AC4
    //     enforcement.
    //
    // Fields (3 + sentinel):
    //   - outermost-deferral  new gc_outermost_deferral_total
    //                          atomic (foundation for AC1
    //                          outermost MutationBoundary
    //                          depth==1 full deferral).
    //                          Value is 0 until AC1 wire-up.
    //   - inner-proceeded      new gc_inner_proceeded_total
    //                          atomic (foundation for AC1
    //                          inner MutationBoundary
    //                          depth>1 short-yield/proceed).
    //                          Value is 0 until AC1 wire-up.
    //   - backoff-trigger      new gc_backoff_trigger_total
    //                          atomic (foundation for AC2
    //                          backoff fires under repeated
    //                          deferral contention). Value
    //                          is 0 until AC2 wire-up.
    //   - schema == 646         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633+#637+#640+#641+
    //                          #642+#643+#644+#645 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the base GC
    // safepoint observability surface:
    //   - (query:gc-safepoint-stats) — base GC safepoint
    //     primitive (no deferral-specific breakdown)
    //   - #591 gc pause attributed to mutation count
    //   - #588 per-fiber stack + GC coordination
    //   - #623 arena + GC safepoint coordination
    //   - #642 arena auto-compaction + fiber/GC safepoint
    // What the issue body AC3 specifies by **exact name +
    // fields** — `query:gc-safepoint-deferral-stats` with
    // outermost-vs-inner + backoff counters — was *not*
    // shipped under that exact name. So #646 ships ONE new
    // Aura primitive + 3 new foundation atomics.
    //
    // The remaining #646 AC1 (outermost vs inner check) +
    // AC2 (backoff retry) + AC4 (wire to scheduler GC phase
    // + fiber yield_classification) work is invasive C++ on
    // aura_evaluator_request_gc_safepoint + fiber
    // check_gc_safepoint + scheduler request_gc_safepoint /
    // wait_for_safepoint + needs the high-contention matrix
    // + arena pressure + TSan coverage from the issue body
    // — separate follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:gc-safepoint-deferral-stats", [&ev](const auto&) -> EvalValue {
            // outermost-deferral: new foundation atomic
            // (0 until AC1 outermost depth==1 wire-up).
            const std::uint64_t outermost_deferral =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->gc_outermost_deferral_total.load(std::memory_order_relaxed)
                    : 0;
            // inner-proceeded: new foundation atomic
            // (0 until AC1 inner depth>1 wire-up).
            const std::uint64_t inner_proceeded =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->gc_inner_proceeded_total.load(std::memory_order_relaxed)
                    : 0;
            // backoff-trigger: new foundation atomic
            // (0 until AC2 backoff wire-up).
            const std::uint64_t backoff_trigger =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->gc_backoff_trigger_total.load(std::memory_order_relaxed)
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
            insert_kv("outermost-deferral", static_cast<std::int64_t>(outermost_deferral));
            insert_kv("inner-proceeded", static_cast<std::int64_t>(inner_proceeded));
            insert_kv("backoff-trigger", static_cast<std::int64_t>(backoff_trigger));
            insert_kv("schema", 646);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 23 (orig lines 3412-3552)
void ObservabilityPrims::register_eval_p23(PrimRegistrar add, Evaluator& ev) {

    // Issue #647: query:envframe-dualpath-stale-stats-hash —
    // Agent-discoverable structured dashboard for the
    // Dual-Path EnvFrame/Env (parent_id_ vs parent_,
    // bindings_symid_ vs bindings_) Cross-Fiber Stale
    // Detection + materialize_call_env After Steal
    // (P0 Runtime-Gap + SoA production-readiness surface —
    // non-duplicative to #637 #589 #355).
    //
    // Note the naming distinction from existing primitives:
    //   - (query:envframe-dualpath-stale-stats) (#418) —
    //     existing flat-int primitive (returns a single
    //     sum of 7 counters — no field breakdown)
    //   - (query:envframe-dualpath-stats) — existing base
    //     dualpath primitive
    //   - (query:envframe-dualpath-stale-stats-hash) (#647,
    //     this primitive) — *enforcement-track* companion
    //     with `-hash` suffix (matches the #630 / #641
    //     hash-vs-int naming convention) that focuses on
    //     the AC1+AC2+AC4 counters for cross-fiber stale +
    //     post-steal version mismatch + dual-path repair
    //     wire-up.
    //
    // Fields (3 + sentinel):
    //   - cross-fiber-stale   new envframe_cross_fiber_stale_
    //                          total atomic (foundation for
    //                          AC1 cross-fiber stale detection
    //                          post-steal — parent_id_
    //                          mismatch vs env_frames_
    //                          owner). Value is 0 until AC1
    //                          wire-up.
    //   - version-mismatch    new envframe_version_mismatch_
    //                          post_steal_total atomic
    //                          (foundation for AC1 version_
    //                          stamp mismatch detection
    //                          post-steal). Value is 0 until
    //                          AC1 wire-up.
    //   - dualpath-repair     new envframe_dualpath_repair_
    //                          total atomic (foundation for
    //                          AC2 dual-path consistency
    //                          check + repair hits). Value
    //                          is 0 until AC2 wire-up.
    //   - schema == 647         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633+#637+#640+#641+
    //                          #642+#643+#644+#645+#646
    //                          sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level
    // EnvFrame dual-path observability surface:
    //   - (query:envframe-dualpath-stale-stats) (#418) —
    //     flat-int sum of 7 counters (no field breakdown)
    //   - (query:envframe-dualpath-stats) — base dualpath
    //     primitive
    //   - (query:envframe-stale-stats) — stale refresh stats
    //   - (query:envframe-bump-stats) — bump stats
    //   - env_frames_ EnvFrame arena (walk + lookup_by_symid_
    //     chain) with version_ + INVALID_VERSION sentinel #356
    //   - #637 IRClosure + EnvFrame versioning + bridge
    //     invalidate protocol
    //   - #589 / #355 SoA migration (parent_id_ vs parent_,
    //     bindings_symid_ vs bindings_ dual-path)
    // What the issue body AC3 specifies by **exact name +
    // fields** — `query:envframe-dualpath-stale-stats` with
    // AC1+AC2+AC4-specific counters as a structured hash —
    // was *not* shipped under that exact hash form. The
    // existing flat-int primitive ships under the same name
    // without `-hash` suffix; #647 ships the hash form with
    // `-hash` suffix (matches #630 / #641 convention for
    // hash-vs-int naming).
    //
    // The remaining #647 AC1 (parent_id_ vs current owner
    // validation) + AC2 (fiber resume dual-path consistency
    // check / repair) + AC4 (GCEnvWalkFn skip/repair) work
    // is invasive C++ on materialize_call_env + lookup paths
    // + fiber resume + g_fiber_sync_mutation_stack_ +
    // GCEnvWalkFn + needs the heavy mutate + fiber steal/
    // yield/resume matrix + INVALID_VERSION post-rollback
    // + TSan coverage from the issue body — separate
    // follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:envframe-dualpath-stale-stats-hash", [&ev](const auto&) -> EvalValue {
            // cross-fiber-stale: new foundation atomic
            // (0 until AC1 cross-fiber post-steal wire-up).
            const std::uint64_t cross_fiber_stale =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->envframe_cross_fiber_stale_total.load(std::memory_order_relaxed)
                    : 0;
            // version-mismatch: new foundation atomic
            // (0 until AC1 version_ mismatch post-steal wire-up).
            const std::uint64_t version_mismatch =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->envframe_version_mismatch_post_steal_total.load(
                              std::memory_order_relaxed)
                    : 0;
            // dualpath-repair: new foundation atomic
            // (0 until AC2 dual-path repair wire-up).
            const std::uint64_t dualpath_repair =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->envframe_dualpath_repair_total.load(std::memory_order_relaxed)
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
            insert_kv("cross-fiber-stale", static_cast<std::int64_t>(cross_fiber_stale));
            insert_kv("version-mismatch", static_cast<std::int64_t>(version_mismatch));
            insert_kv("dualpath-repair", static_cast<std::int64_t>(dualpath_repair));
            insert_kv("schema", 647);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

} // namespace aura::compiler::primitives_detail
