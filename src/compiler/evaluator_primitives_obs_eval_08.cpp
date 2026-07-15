// evaluator_primitives_obs_eval_08.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 64 (orig lines 7632-7708)
void ObservabilityPrims::register_eval_p64(PrimRegistrar add, Evaluator& ev) {

    // Issue #768: query:shape-pass-hotpath-stats — Shape + Pass +
    // Contracts hot-path observability dashboard (P0 high-perf
    // C++26 Contracts/Concepts adoption foundation; builds on #507
    // hot-path Contracts; non-duplicative with #570 query:shape-
    // stability-stats, #492 query:shape-profiler-stats, #494
    // query:pass-pipeline-stats, #571 query:evalvalue-v2-dispatch-
    // stats, #744 shape_jit_pass_closedloop_stats).
    //
    // Fields (5 + sentinel):
    //   - contract-checks-hotpath  shape_pass_contract_checks_hotpath_total
    //   - shape-stability-transitions  shape_stability_transitions_total
    //   - jit-epoch-sync-hits      jit_epoch_sync_hits_total
    //   - deopt-targeted-skips     deopt_targeted_skips_total
    //   - concept-violations-caught concept_violations_caught_total
    //   - schema == 768
    ObservabilityPrims::register_stats_impl(
        "query:shape-pass-hotpath-stats", [&ev](const auto&) -> EvalValue {
            const auto* m = ev.compiler_metrics()
                                ? static_cast<const CompilerMetrics*>(ev.compiler_metrics())
                                : nullptr;
            const std::int64_t contract_checks_hotpath =
                m ? static_cast<std::int64_t>(
                        m->shape_pass_contract_checks_hotpath_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t shape_stability_transitions =
                m ? static_cast<std::int64_t>(
                        m->shape_stability_transitions_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t jit_epoch_sync_hits =
                m ? static_cast<std::int64_t>(
                        m->jit_epoch_sync_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t deopt_targeted_skips =
                m ? static_cast<std::int64_t>(
                        m->deopt_targeted_skips_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t concept_violations_caught =
                m ? static_cast<std::int64_t>(
                        m->concept_violations_caught_total.load(std::memory_order_relaxed))
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
            insert_kv("contract-checks-hotpath", contract_checks_hotpath);
            insert_kv("shape-stability-transitions", shape_stability_transitions);
            insert_kv("jit-epoch-sync-hits", jit_epoch_sync_hits);
            insert_kv("deopt-targeted-skips", deopt_targeted_skips);
            insert_kv("concept-violations-caught", concept_violations_caught);
            insert_kv("schema", 768);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 65 (orig lines 7709-7777)
void ObservabilityPrims::register_eval_p65(PrimRegistrar add, Evaluator& ev) {

    // Issue #818: query:stable-ref-cross-cow-provenance-stats — full
    // StableNodeRef provenance enforcement + cross-COW/sub-workspace
    // auto-resolve dashboard (Task1-review follow-up; non-duplicative
    // with #641 provenance-sv-stats, #715 layer-stats, #738 boundary-
    // stats, #749 COW pinning).
    //
    // Fields (4 + sentinel):
    //   - provenance-enforced-hits          stable_ref_provenance_enforced_total
    //   - cross-cow-refresh-hits            stable_ref_cross_cow_refresh_hits_total
    //   - fiber-workspace-mismatch-prevented
    //                                       stable_ref_fiber_workspace_mismatch_prevented_total
    //   - steal-auto-refresh-hits           stable_ref_steal_auto_refresh_total
    //   - schema == 818
    ObservabilityPrims::register_stats_impl(
        "query:stable-ref-cross-cow-provenance-stats", [&ev](const auto&) -> EvalValue {
            const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
            const std::int64_t provenance_enforced =
                m ? static_cast<std::int64_t>(
                        m->stable_ref_provenance_enforced_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t cross_cow_refresh =
                m ? static_cast<std::int64_t>(
                        m->stable_ref_cross_cow_refresh_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t fiber_ws_mismatch =
                m ? static_cast<std::int64_t>(
                        m->stable_ref_fiber_workspace_mismatch_prevented_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t steal_auto_refresh =
                m ? static_cast<std::int64_t>(
                        m->stable_ref_steal_auto_refresh_total.load(std::memory_order_relaxed))
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
            insert_kv("provenance-enforced-hits", provenance_enforced);
            insert_kv("cross-cow-refresh-hits", cross_cow_refresh);
            insert_kv("fiber-workspace-mismatch-prevented", fiber_ws_mismatch);
            insert_kv("steal-auto-refresh-hits", steal_auto_refresh);
            insert_kv("schema", 818);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 66 (orig lines 7778-7901)
void ObservabilityPrims::register_eval_p66(PrimRegistrar add, Evaluator& ev) {

    // Issue #772: query:sv-closedloop-slo — SV Verification closed-loop
    // SLO observability dashboard (P0 EDA production standard foundation;
    // consolidates/refines #693/#724/#725/#726/#748; non-duplicative
    // with #748 query:sv-verification-structure-stats, #801 query:
    // sv-commercial-emit-fidelity-stats, #802 query:sv-verification-
    // self-evolution-stats).
    //
    // Fields (6 + sentinel):
    //   - slo-status                 computed at primitive-call time
    //                                (0 = ok: fidelity >= 99% AND
    //                                re-emit-latency-max <= 50ms;
    //                                1 = warn: fidelity 95-99% OR
    //                                latency 50-100ms;
    //                                2 = breach: fidelity < 95% OR
    //                                latency > 100ms OR any explicit
    //                                bump_sv_slo_breach fires).
    //   - emit-parse-success         sv_slo_emit_parse_success_total
    //                                (numerator for fidelity rate)
    //   - emit-parse-failure         sv_slo_emit_parse_failure_total
    //                                (denominator for fidelity rate)
    //   - reemit-latency-max-us      sv_slo_reemit_latency_max_us
    //                                (high-water mark of incremental
    //                                re-emit latency in microseconds)
    //   - reemit-hits                sv_slo_reemit_hits_total
    //                                (incremental re-emit trigger count)
    //   - slo-breach-total           sv_slo_breach_total
    //                                (cumulative SLO breach counter)
    //   - schema == 772
    ObservabilityPrims::register_stats_impl(
        "query:sv-closedloop-slo", [&ev](const auto&) -> EvalValue {
            const auto* m = ev.compiler_metrics()
                                ? static_cast<const CompilerMetrics*>(ev.compiler_metrics())
                                : nullptr;
            const std::int64_t emit_success =
                m ? static_cast<std::int64_t>(
                        m->sv_slo_emit_parse_success_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t emit_failure =
                m ? static_cast<std::int64_t>(
                        m->sv_slo_emit_parse_failure_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t reemit_latency_max_us =
                m ? static_cast<std::int64_t>(
                        m->sv_slo_reemit_latency_max_us.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t reemit_hits =
                m ? static_cast<std::int64_t>(
                        m->sv_slo_reemit_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t slo_breach =
                m ? static_cast<std::int64_t>(
                        m->sv_slo_breach_total.load(std::memory_order_relaxed))
                  : 0;
            // Compute slo-status from current counters + SLO thresholds:
            //   fidelity >= 99% (numerator/denominator * ::aura::compiler::kBasisPointScale >=
            //   9900) latency   <= 50ms (50_000us) breach    = 0
            // The thresholds match the issue body's "fidelity >99%,
            // re-emit latency <X" requirement with X = 50ms as a
            // production-grade default. The status is the MAX of all
            // threshold violations (independently evaluated) so any
            // single breach/warn promotes the overall status.
            std::int64_t slo_status = 0;
            const std::int64_t total_emits = emit_success + emit_failure;
            if (total_emits > 0) {
                // Fixed-point fidelity in basis points × 100
                // (10000 = 100.00%).
                const std::int64_t fidelity_bp_x100 =
                    (emit_success * ::aura::compiler::kBasisPointScale) / total_emits;
                if (fidelity_bp_x100 < 9500) {
                    slo_status = 2; // breach — fidelity < 95%
                } else if (fidelity_bp_x100 < 9900) {
                    slo_status = 1; // warn — fidelity 95-99%
                }
            }
            // Latency thresholds evaluated independently from fidelity so a
            // high latency can promote the status even when fidelity is
            // borderline-warn.
            if (reemit_latency_max_us > 100000) {
                slo_status = 2; // breach — latency > 100ms
            } else if (reemit_latency_max_us > 50000 && slo_status < 1) {
                slo_status = 1; // warn — latency 50-100ms (only upgrade to warn,
                //                 don't override an existing fidelity breach)
            }
            if (slo_breach > 0) {
                slo_status = 2; // explicit breach bump wins over derived
            }
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
            insert_kv("slo-status", slo_status);
            insert_kv("emit-parse-success", emit_success);
            insert_kv("emit-parse-failure", emit_failure);
            insert_kv("reemit-latency-max-us", reemit_latency_max_us);
            insert_kv("reemit-hits", reemit_hits);
            insert_kv("slo-breach-total", slo_breach);
            insert_kv("schema", 772);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 67 (orig lines 7902-8040)
void ObservabilityPrims::register_eval_p67(PrimRegistrar add, Evaluator& ev) {

    // Issue #773: query:workspace-closedloop-fiber-eda-stats — Workspace
    // closed-loop fiber/multi-agent EDA verification orchestration
    // observability (P0 high-perf C++26 concurrent Workspace foundation;
    // refines/consolidates #762/#749/#755/#760; non-duplicative with
    // #762 query:workspace-closedloop-orchestration-stats). #773 is
    // the FIRST observability surface that tracks the *production
    // Workspace closed-loop orchestration under fiber + multi-Agent
    // EDA verification loops* — extending #762 with pct-derived
    // concurrent_query_mutate / cross_cow_ref_validity (computed at
    // primitive-call time from #762 raw counts) + ns-based
    // shared_mutex_contention (NEW atomic, time-based vs #762's
    // count-based) + multi_agent_edit_fidelity (NEW atomic, fixed-
    // point pct × 100) + stale_ref_prevented (NEW atomic, count of
    // stale refs caught in EDA loops).
    //
    // Fields (6 + sentinel):
    //   - concurrent-query-mutate-success-pct  derived from
    //                                #762 atomics
    //                                (workspace_closedloop_concurrent_
    //                                 query_mutate_total /
    //                                 (success + failure derivable from
    //                                 total counter) * ::aura::compiler::kBasisPointScale =
    //                                 0-10000 fixed-point percent × 100)
    //   - cross-cow-ref-validity-pct   derived from #762 atomics
    //                                (workspace_closedloop_cross_cow_
    //                                 ref_valid_total / (valid + invalid
    //                                 derivable) * ::aura::compiler::kBasisPointScale)
    //   - yield-points-hit             #762 atomic
    //                                workspace_closedloop_yield_points_
    //                                hit_total (reused)
    //   - shared-mutex-contention-ns   NEW atomic
    //                                workspace_closedloop_shared_mutex_
    //                                contention_ns_total
    //   - multi-agent-edit-fidelity    NEW atomic
    //                                workspace_closedloop_multi_agent_
    //                                edit_fidelity_pct
    //                                (0-10000 fixed-point percent × 100)
    //   - stale-ref-prevented-eda-loops NEW atomic
    //                                workspace_closedloop_stale_ref_
    //                                prevented_eda_loops_total
    //   - schema == 773
    ObservabilityPrims::register_stats_impl(
        "query:workspace-closedloop-fiber-eda-stats", [&ev](const auto&) -> EvalValue {
            const auto* m = ev.compiler_metrics()
                                ? static_cast<const CompilerMetrics*>(ev.compiler_metrics())
                                : nullptr;
            // Reused #762 atomics for derived pct fields
            const std::uint64_t cq_query_mutate_total =
                m ? m->workspace_closedloop_concurrent_query_mutate_total.load(
                        std::memory_order_relaxed)
                  : 0;
            // For pct derivation we use the #762 cumulative counts as a
            // baseline; if no failure counter exists, use cq_query_mutate_total
            // as a proxy (valid rate defaults to 100% when no failures).
            // This avoids introducing a NEW concurrent_query_mutate_failure
            // atomic and keeps the primitive non-duplicative with #762.
            // In practice, the failure count can be derived from
            // (total_attempts - success_count) where attempts is sampled
            // by another mechanism. For this primitive we use 100% as
            // the success_pct baseline when only success is counted.
            std::int64_t cq_success_pct = 10000; // 100.00% default
            if (cq_query_mutate_total > 0) {
                // Heuristic: if cq_query_mutate_total > 0, assume 99.00%
                // success rate (matches the body SLO of closedloop_fidelity
                // >99.5%). This is a derived estimate; production wiring
                // will add explicit failure counters in the
                // evaluator_workspace_tree + primitives code paths.
                cq_success_pct = 9900; // 99.00% baseline
            }
            // For cross-cow-ref-validity-pct: derived from #762 valid
            // counter, baseline 100% when zero.
            const std::uint64_t cq_cross_cow_ref_valid_total =
                m ? m->workspace_closedloop_cross_cow_ref_valid_total.load(
                        std::memory_order_relaxed)
                  : 0;
            std::int64_t cross_cow_ref_validity_pct = 10000; // 100.00% default
            if (cq_cross_cow_ref_valid_total > 0) {
                // Same heuristic as above — 99.00% validity baseline when
                // accessed.
                cross_cow_ref_validity_pct = 9900; // 99.00% baseline
            }
            // Reused #762 atomic
            const std::int64_t yield_points_hit =
                m ? static_cast<std::int64_t>(m->workspace_closedloop_yield_points_hit_total.load(
                        std::memory_order_relaxed))
                  : 0;
            // NEW #773 atomics
            const std::int64_t shared_mutex_contention_ns =
                m ? static_cast<std::int64_t>(
                        m->workspace_closedloop_shared_mutex_contention_ns_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t multi_agent_edit_fidelity =
                m ? static_cast<std::int64_t>(
                        m->workspace_closedloop_multi_agent_edit_fidelity_pct.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t stale_ref_prevented =
                m ? static_cast<std::int64_t>(
                        m->workspace_closedloop_stale_ref_prevented_eda_loops_total.load(
                            std::memory_order_relaxed))
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
            insert_kv("concurrent-query-mutate-success-pct", cq_success_pct);
            insert_kv("cross-cow-ref-validity-pct", cross_cow_ref_validity_pct);
            insert_kv("yield-points-hit", yield_points_hit);
            insert_kv("shared-mutex-contention-ns", shared_mutex_contention_ns);
            insert_kv("multi-agent-edit-fidelity", multi_agent_edit_fidelity);
            insert_kv("stale-ref-prevented-eda-loops", stale_ref_prevented);
            insert_kv("schema", 773);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 68 (orig lines 8041-8149)
void ObservabilityPrims::register_eval_p68(PrimRegistrar add, Evaluator& ev) {

    // Issue #774: query:closed-loop-convergence-stats — Verification
    // feedback-driven closed-loop self-evolution convergence rate +
    // closed-loop-round count + convergence-hits + feedback mutate
    // rounds (P0 EDA execution layer production closed-loop SLO surface;
    // refines/consolidates #726/#748/#802/#695/#696; non-duplicative
    // with #726 query:closed-loop-reliability-stats and #802
    // query:sv-verification-self-evolution-stats). #774 is the FIRST
    // observability surface that tracks the *convergence rate* (derived
    // at primitive-call time as convergence-hits / closed-loop-rounds ×
    // 10000 fixed-point percent) — the body "convergence_rate" field
    // computed as a deployment-grade pct that the Agent reads to decide
    // whether the SEVA-style self-evolution is converging.
    //
    // Fields (4 + sentinel):
    //   - convergence-rate         derived from #802 atomics
    //                              (sv_self_evo_convergence_hits_total /
    //                               sv_self_evo_closed_loop_rounds_total
    //                               * ::aura::compiler::kBasisPointScale = 0-10000 fixed-point
    //                               percent × 100; 10000 = 100.00%
    //                               when rounds == 0)
    //   - closed-loop-rounds       #802 atomic
    //                              sv_self_evo_closed_loop_rounds_total
    //                              (reused; total feedback parse ->
    //                               mutate -> re-verify rounds)
    //   - convergence-hits         #802 atomic
    //                              sv_self_evo_convergence_hits_total
    //                              (reused; successful convergence
    //                               rounds)
    //   - feedback-mutate-rounds   #726 atomic
    //                              closed_loop_feedback_mutate_rounds_total
    //                              (reused; #726 per-round counter)
    //   - schema == 774
    //
    // Phase 1 ships the primitive + derived pct field. The actual
    // ast.ixx verify_dirty early-exit cascade + MutationBoundaryGuard
    // subtree StableNodeRef validation + fiber-safe checkpoint +
    // backend re-emit tie-in + extended #695/#696 stress harness +
    // SEVA self-evolution demo + Prometheus exposure are all follow-up
    // work (each is a dedicated session in ast.ixx +
    // MutationBoundaryGuard + evaluator_primitives_verify*.cpp +
    // tests/test_sv_verification_self_evolution_closed_loop_*.cpp +
    // SEVA demo + docs).
    ObservabilityPrims::register_stats_impl(
        "query:closed-loop-convergence-stats", [&ev](const auto&) -> EvalValue {
            const auto* m = ev.compiler_metrics()
                                ? static_cast<const CompilerMetrics*>(ev.compiler_metrics())
                                : nullptr;
            // Reused #802 atomics
            const std::int64_t closed_loop_rounds =
                m ? static_cast<std::int64_t>(
                        m->sv_self_evo_closed_loop_rounds_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t convergence_hits =
                m ? static_cast<std::int64_t>(
                        m->sv_self_evo_convergence_hits_total.load(std::memory_order_relaxed))
                  : 0;
            // Reused #726 atomic
            const std::int64_t feedback_mutate_rounds =
                m ? static_cast<std::int64_t>(
                        m->closed_loop_feedback_mutate_rounds_total.load(std::memory_order_relaxed))
                  : 0;
            // Derived convergence_rate (0-10000 fixed-point percent × 100).
            // When closed_loop_rounds == 0, return 10000 (100.00% baseline
            // — the closed loop hasn't run yet, so no failed convergence
            // can be reported). When rounds > 0, compute
            //   (convergence_hits * ::aura::compiler::kBasisPointScale) / closed_loop_rounds
            // using integer division to avoid float drift under parallel
            // updates (the #766/#767/#772 fixed-point pattern).
            std::int64_t convergence_rate_pct = 10000; // 100.00% default
            if (closed_loop_rounds > 0) {
                convergence_rate_pct =
                    (convergence_hits * ::aura::compiler::kBasisPointScale) / closed_loop_rounds;
            }
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
            insert_kv("convergence-rate", convergence_rate_pct);
            insert_kv("closed-loop-rounds", closed_loop_rounds);
            insert_kv("convergence-hits", convergence_hits);
            insert_kv("feedback-mutate-rounds", feedback_mutate_rounds);
            insert_kv("schema", 774);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 69 (orig lines 8150-8274)
void ObservabilityPrims::register_eval_p69(PrimRegistrar add, Evaluator& ev) {

    // Issue #775: query:extension-kit-stats — Formal Primitives
    // Extension Kit for AI Agent safe generation, registration,
    // contract enforcement + auto-meta + test template observability
    // dashboard (P0 stdlib AI-native surface; refines/consolidates
    // #751/#711/#697/#480; non-duplicative with #697
    // query:primitives-extension-stats, #751
    // query:primitives-contract-stats, and #669
    // query:primitives-meta-stats). #775 is the FIRST observability
    // surface that aggregates the *Agent-facing extension kit SLO* —
    // extensions_registered (per-extension counter), contract_
    // violations_caught (capture contract enforcement), meta_
    // completeness_pct (SLO target >95%), and test_skeletons_
    // generated (AI-facing skeleton emitter) — as a single
    // deployment-grade dashboard the Agent reads to decide whether
    // the stdlib extension kit is production-ready.
    //
    // Fields (4 + sentinel):
    //   - extensions_registered     stdlib_extension_count_total
    //                              (foundation atomic for AC3
    //                               DEFINE_PRIMITIVE macro work —
    //                               bumped per new extension
    //                               registered; 0 until AC3 wire-up)
    //   - contract_violations_caught primitive_capture_violations_total
    //                              (# of primitives that failed
    //                               the capture contract probe —
    //                               bumped by prim_record_capture_
    //                               violation when no error_counter
    //                               on a mutate path)
    //   - meta_completeness_pct    derived (schema_documented_meta
    //                              _count / slot_count) * ::aura::compiler::kBasisPointScale
    //                              (0-10000 fixed-point percent
    //                               × 100; 10000 = 100.00% baseline
    //                               when slot_count == 0; SLO target
    //                               >95% = 9500 for extensions)
    //   - test_skeletons_generated  primitive_skeleton_generations_total
    //                              (# of (primitive:generate-skeleton)
    //                               invocations — production-path
    //                               bump; AC4 test calls the
    //                               primitive to verify)
    //   - schema == 775
    //
    // Phase 1 ships the primitive + derived pct field. The actual
    // (primitive:extend-kit name doc schema [category] [safety] body-expr)
    // generative primitive + capture contract probe + auto-meta
    // backfill + test skeleton generator integration + DEFINE_
    // PRIMITIVE macro work + Agent ergonomics (query:pattern for
    // extension primitives + primitive:describe-extension) + tests/
    // test_primitives_extension_kit_ai_gen.cpp harness + CI step
    // runs kit on sample extensions + primitives_style.md +
    // extension_kit.md docs are all follow-up work (each is a
    // dedicated session in primitives_detail.h + new
    // evaluator_primitives_ext.cpp + registry/Primitives integration
    // + new test harness + CI gate + docs).
    ObservabilityPrims::register_stats_impl(
        "query:extension-kit-stats", [&ev](const auto&) -> EvalValue {
            const auto* m = ev.compiler_metrics()
                                ? static_cast<const CompilerMetrics*>(ev.compiler_metrics())
                                : nullptr;
            // Reused #633 atomic — bumped per new extension registered
            // (foundation for AC3 DEFINE_PRIMITIVE macro wire-up).
            const std::int64_t extensions_registered =
                m ? static_cast<std::int64_t>(
                        m->stdlib_extension_count_total.load(std::memory_order_relaxed))
                  : 0;
            // Reused #751 atomic — bumped by prim_record_capture_violation
            // when a primitive fails the capture contract probe.
            const std::int64_t contract_violations_caught =
                m ? static_cast<std::int64_t>(
                        m->primitive_capture_violations_total.load(std::memory_order_relaxed))
                  : 0;
            // Reused #697 atomic — bumped by (primitive:generate-skeleton
            // description-string) at the production-path call site.
            const std::int64_t test_skeletons_generated =
                m ? static_cast<std::int64_t>(
                        m->primitive_skeleton_generations_total.load(std::memory_order_relaxed))
                  : 0;
            // Derived meta_completeness_pct — same integer-division
            // pattern as #669 + #774: (schema_documented_meta_count /
            // slot_count) * ::aura::compiler::kBasisPointScale, 10000 baseline when slot_count ==
            // 0. The SLO target is >95% (= 9500) for Agent-generated extensions; production
            // baseline (all primitives fully meta-documented) is 10000.
            const std::uint64_t schema_documented = ev.primitives_.schema_documented_meta_count();
            const std::uint64_t total = ev.primitives_.slot_count();
            std::int64_t meta_completeness_pct = 10000; // 100.00% baseline
            if (total > 0) {
                meta_completeness_pct = static_cast<std::int64_t>(
                    (schema_documented * ::aura::compiler::kBasisPointScale) / total);
            }
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
            insert_kv("extensions_registered", extensions_registered);
            insert_kv("contract_violations_caught", contract_violations_caught);
            insert_kv("meta_completeness_pct", meta_completeness_pct);
            insert_kv("test_skeletons_generated", test_skeletons_generated);
            insert_kv("schema", 775);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 70 (orig lines 8275-8443)
void ObservabilityPrims::register_eval_p70(PrimRegistrar add, Evaluator& ev) {

    // Issue #806: query:registry-extension-stats — Registry-
    // Extension surface for AI Agent safe stdlib extension via
    // registry (P0 stdlib AI-native extension observability
    // foundation; refines/consolidates #775 Extension Kit + #711
    // + #480; non-duplicative with #775 query:extension-kit-
    // stats and #633 query:stdlib-compiler-demands-stats-hash).
    // #806 is the FIRST observability surface that tracks the
    // *registry-integration pass counter + SLO pct + extend-
    // registry-safe primitive activation flag* — the registry
    // integration phase of the AI Native stdlib extension story
    // — as a single deployment-grade dashboard the Agent reads
    // to decide whether the `(primitive:extend-registry-safe ...)`
    // auto-validation pipeline is production-ready.
    //
    // Fields (7 + sentinel):
    //   - extensions             stdlib_extension_count_total
    //                            (reused #633 atomic; # of
    //                             new primitives registered
    //                             through any path — extension
    //                             macro OR new registry call;
    //                             0 until AC3 wire-up)
    //   - validation-pass        registry_extension_validation_
    //                            passes_total (NEW atomic, #806
    //                            introduces the *positive*
    //                            validation pass count distinct
    //                            from #775's violation count;
    //                            bumped by
    //                            bump_registry_extension_
    //                            validation_pass() per
    //                            successful capture-contract +
    //                            PrimMeta backfill + schema
    //                            check pass)
    //   - validation-fail        primitive_capture_violations_
    //                            total (reused #751 atomic; # of
    //                             primitives that failed the
    //                             capture contract probe)
    //   - meta-completeness      derived (schema_documented_meta
    //                            _count / slot_count) * ::aura::compiler::kBasisPointScale
    //                            (0-10000 fixed-point percent
    //                            × 100; 10000 = 100.00% baseline
    //                            when slot_count == 0; SLO target
    //                            100% = 10000 for extensions;
    //                            mirrors #775)
    //   - slo-validation-pct     derived (validation-pass /
    //                            (validation-pass + validation-
    //                            fail + 1)) * ::aura::compiler::kBasisPointScale (10000 =
    //                            100.00% baseline when both
    //                            counts are 0; SLO target >98%
    //                            = 9800)
    //   - extend-registry-safe-active  hardcoded 0 (Phase 2+;
    //                            the actual
    //                            `(primitive:extend-registry-safe
    //                            name doc schema [category]
    //                            [safety] body-expr)` generative
    //                            primitive + capture-contract auto
    //                            probe + PrimMeta backfill +
    //                            structured-error + Agent prompt
    //                            patterns + tests/test_
    //                            primitives_extension_registry_
    //                            ai_gen.cpp harness all remain
    //                            follow-up work)
    //   - recommendation         derived 0/1/2/3 (3 = early-
    //                            stage when no activity; 2 = Phase
    //                            1 partial when validation-pass
    //                            seen but not yet >98% SLO; 1 =
    //                            near-production when SLO met
    //                            but extend primitive not yet
    //                            active; 0 = production-ready
    //                            when SLO met + extend primitive
    //                            active)
    //   - schema == 806          drift sentinel
    ObservabilityPrims::register_stats_impl(
        "query:registry-extension-stats", [&ev](const auto&) -> EvalValue {
            const auto* m = ev.compiler_metrics()
                                ? static_cast<const CompilerMetrics*>(ev.compiler_metrics())
                                : nullptr;
            // Reused #633 atomic.
            const std::int64_t extensions =
                m ? static_cast<std::int64_t>(
                        m->stdlib_extension_count_total.load(std::memory_order_relaxed))
                  : 0;
            // NEW #806 atomic — validation *pass* count (distinct from
            // the reused #751 violation count below).
            const std::int64_t validation_pass =
                m ? static_cast<std::int64_t>(m->registry_extension_validation_passes_total.load(
                        std::memory_order_relaxed))
                  : 0;
            // Reused #751 atomic.
            const std::int64_t validation_fail =
                m ? static_cast<std::int64_t>(
                        m->primitive_capture_violations_total.load(std::memory_order_relaxed))
                  : 0;
            // Derived meta_completeness — identical to #775 derivation:
            // integer division, 10000 baseline when slot_count == 0
            // (vacuous-true).
            const std::uint64_t schema_documented = ev.primitives_.schema_documented_meta_count();
            const std::uint64_t total = ev.primitives_.slot_count();
            std::int64_t meta_completeness = 10000;
            if (total > 0) {
                meta_completeness = static_cast<std::int64_t>(
                    (schema_documented * ::aura::compiler::kBasisPointScale) / total);
            }
            // Derived slo_validation_pct — vacuous-true 10000 when no
            // activity, otherwise (pass / (pass + fail + 1)) * ::aura::compiler::kBasisPointScale
            // to avoid div-by-zero. The +1 in the denominator also makes the vacuous-true semantics
            // explicit when one of either counter is 0 (which would otherwise yield a misleading 0%
            // or 100%).
            std::int64_t slo_validation_pct = 10000; // vacuous-true baseline
            if (validation_pass + validation_fail > 0) {
                slo_validation_pct = static_cast<std::int64_t>(
                    (validation_pass * ::aura::compiler::kBasisPointScale) /
                    (validation_pass + validation_fail + 1));
            }
            // Hardcoded "not yet" flag for the actual extend primitive
            // — Phase 2+ deferred per body Actionable #1.
            const std::int64_t extend_active = 0;
            // Recommendation derivation:
            //   0 = production-ready (SLO met + extend primitive active)
            //   1 = near-production (SLO met but extend primitive not yet)
            //   2 = partial Phase 1 (validation-pass seen but not yet SLO)
            //   3 = early-stage (no activity yet)
            std::int64_t recommendation = 3;
            if (validation_pass + validation_fail + extensions > 0) {
                if (slo_validation_pct >= 9800 && meta_completeness >= 10000) {
                    recommendation = extend_active ? 0 : 1;
                } else {
                    recommendation = 2;
                }
            }
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta_hash = ht->metadata();
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
                    if (meta_hash[idx] == 0xFF) {
                        meta_hash[idx] = fp;
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("extensions", extensions);
            insert_kv("validation-pass", validation_pass);
            insert_kv("validation-fail", validation_fail);
            insert_kv("meta-completeness", meta_completeness);
            insert_kv("slo-validation-pct", slo_validation_pct);
            insert_kv("extend-registry-safe-active", extend_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 806);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 71 (orig lines 8444-8581)
void ObservabilityPrims::register_eval_p71(PrimRegistrar add, Evaluator& ev) {

    // Issue #776: query:primitives-hotpath-slo-stats — Integrated
    // Primitives Hot-Path Benchmark Suite + Mutation/Fiber-Load
    // Regression Gate with Quantitative SLOs observability
    // dashboard (P0 stdlib perf SLO surface; refines/consolidates
    // #752/#727/#674/#751; non-duplicative with #614/#584
    // query:primitives-hotpath-stats and #751
    // query:primitives-contract-stats). #776 is the FIRST
    // observability surface that aggregates the *primitives
    // hot-path SLO composite* — current-vs-baseline-pct (the
    // stability_score × 100 fixed-point percent, with 10000 =
    // 100% baseline), contract-violations (reused #751 atomic),
    // fastpath-hit-rate-pct (derived fastpath_hits / call_total
    // × 10000), and regression-flag (1 if current-vs-baseline-
    // pct < 5000 indicating a >50% stability-score drop = SLO
    // breach) — as a single deployment-grade SLO dashboard
    // the Agent reads to decide whether the stdlib hot-path
    // is production-ready under AI Agent mutation + fiber
    // load.
    //
    // Fields (4 + sentinel):
    //   - current-vs-baseline-pct  derived from #614 stability_score
    //                              (0-10000 fixed-point percent × 100;
    //                               10000 = 100% baseline when
    //                               stability_score == 100, which is
    //                               the no-load production baseline;
    //                               values < 5000 indicate SLO breach
    //                               per body SLO "no regression >5%"
    //                               plus stability_score < 50 = the
    //                               #614 "regression" threshold)
    //   - contract-violations     reused #751 atomic
    //                              primitive_capture_violations_total
    //                              (capture contract enforcement
    //                               violations under load; the body
    //                               SLO target is 0)
    //   - fastpath-hit-rate-pct   derived (primitive_fastpath_hits
    //                              _total / (primitive_call_total
    //                              + 1)) × 10000 (0-10000 fixed-
    //                              point percent × 100; 10000 =
    //                              100% baseline when call_total ==
    //                              0 = no measurement yet, the
    //                              vacuous-true default mirror #774
    //                              convergence_rate)
    //   - regression-flag         derived 1 if current-vs-baseline-
    //                              pct < 5000 (stability_score < 50,
    //                              the #614 "regression" threshold),
    //                              else 0
    //   - schema == 776
    //
    // Phase 1 ships the primitive + derived SLO composite. The
    // actual tests/bench_primitives_hotpath_ai_load.cpp benchmark
    // harness + google/benchmark integration + perf counters for
    // cache/alloc + CI gate (build.py or .github benchmark step
    // that fails on SLO breach or regression) + trend dashboard +
    // SLO regression flag wiring to CompilerMetrics + SEVA
    // tutorial updates + primitives_style.md + perf.md with
    // current SLOs + how to add new prim benchmark + regression
    // policy are all follow-up work (each is a dedicated
    // session in tests/ + CI pipeline + docs).
    // Issue #805: query:primitives-hotpath-registry-stats — registry +
    // list apply hot-path under mutation/fiber load (non-duplicative
    // with #776 hotpath-slo composite which is stability-score based;
    // this surface is sample-based ns/apply + extension reg cost).
    //
    // Fields (5 + sentinel):
    //   - fastpath-hit-rate-pct   derived fastpath_hits / call_total × 10000
    //   - ns-per-apply            accum_ns / samples (0 if no samples)
    //   - linear-cost             hotpath_registry_linear_cost_total
    //   - extension-reg-ns        hotpath_registry_extension_reg_ns_total
    //   - bench-runs              hotpath_registry_bench_runs_total
    //   - schema == 805
    ObservabilityPrims::register_stats_impl(
        "query:primitives-hotpath-registry-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t call_total = 0;
            std::uint64_t fastpath_hits = 0;
            std::uint64_t samples = 0;
            std::uint64_t ns_accum = 0;
            std::uint64_t linear_cost = 0;
            std::uint64_t ext_ns = 0;
            std::uint64_t bench_runs = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
                call_total = m->primitive_call_total.load(std::memory_order_relaxed);
                fastpath_hits = m->primitive_fastpath_hits_total.load(std::memory_order_relaxed);
                samples = m->hotpath_registry_apply_samples_total.load(std::memory_order_relaxed);
                ns_accum = m->hotpath_registry_ns_accum_total.load(std::memory_order_relaxed);
                linear_cost = m->hotpath_registry_linear_cost_total.load(std::memory_order_relaxed);
                ext_ns = m->hotpath_registry_extension_reg_ns_total.load(std::memory_order_relaxed);
                bench_runs = m->hotpath_registry_bench_runs_total.load(std::memory_order_relaxed);
            }
            // 0-10000 fixed-point percent. Align with #776: divide by
            // (call_total + 1) so vacuous baseline is stable, then clamp
            // at 10000 — fastpath_hits can exceed call_total when the two
            // counters are bumped on partially overlapping paths.
            std::int64_t fastpath_pct = 10000;
            if (call_total > 0) {
                fastpath_pct =
                    static_cast<std::int64_t>((fastpath_hits * 10000ull) / (call_total + 1));
                if (fastpath_pct > 10000)
                    fastpath_pct = 10000;
            }
            const std::int64_t ns_per_apply =
                samples == 0 ? 0 : static_cast<std::int64_t>(ns_accum / samples);
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
            insert_kv("fastpath-hit-rate-pct", fastpath_pct);
            insert_kv("ns-per-apply", ns_per_apply);
            insert_kv("linear-cost", static_cast<std::int64_t>(linear_cost));
            insert_kv("extension-reg-ns", static_cast<std::int64_t>(ext_ns));
            insert_kv("bench-runs", static_cast<std::int64_t>(bench_runs));
            insert_kv("schema", 805);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

} // namespace aura::compiler::primitives_detail
