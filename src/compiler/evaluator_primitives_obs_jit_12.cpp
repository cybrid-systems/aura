// evaluator_primitives_obs_jit_12.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 96 (orig lines 20519-20569)
void ObservabilityPrims::register_jit_p96(PrimRegistrar add, Evaluator& ev) {
    // Issue #868: query:sv-eda-primitives-cluster-stats
    ObservabilityPrims::register_stats_impl(
        "query:sv-eda-primitives-cluster-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(m->sv_eda_prims_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits = m ? static_cast<std::int64_t>(m->sv_eda_prims_hits_total.load(
                                              std::memory_order_relaxed))
                                        : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->sv_eda_prims_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16) /* #1141 */;
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
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 868);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 97 (orig lines 20570-20620)
void ObservabilityPrims::register_jit_p97(PrimRegistrar add, Evaluator& ev) {
    // Issue #869: query:primitives-resource-quota-fiber-stats
    ObservabilityPrims::register_stats_impl(
        "query:primitives-resource-quota-fiber-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->prim_quota_fiber_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->prim_quota_fiber_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->prim_quota_fiber_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16) /* #1141 */;
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
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 869);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1481: query:resource-quota-stats (sister to #869 above).
    // Returns a 4-field hash: {checks_total, rejects_total, max_fibers,
    // max_mutations} from the typed-error enforcement helpers shipped at
    // ebc88e0d (ResourceQuotaExceeded enum variant) + 0bfeec38 (5 inline
    // impl bodies in evaluator.ixx + counter wiring). Aggregate tracking
    // migration (per-call mutation / fiber quota compare) is deferred to
    // follow-up issues — for #1481 (scope-limited close), only the
    // typed-error entry point + counter wiring exists, so the
    // _checks_total / _rejects_total counters are observable while
    // _max_fibers / _max_mutations reflect the currently-configured
    // limits (defaults 256 / 100000 from observability_metrics.h:1568
    // area).
    ObservabilityPrims::register_stats_impl(
        "query:resource-quota-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t checks_total =
                m ? static_cast<std::int64_t>(
                        m->resource_quota_checks_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t rejects_total =
                m ? static_cast<std::int64_t>(
                        m->resource_quota_rejects_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t max_fibers =
                m ? static_cast<std::int64_t>(
                        m->resource_quota_max_fibers.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t max_mutations =
                m ? static_cast<std::int64_t>(
                        m->resource_quota_max_mutations.load(std::memory_order_relaxed))
                  : 0;
            auto* ht = FlatHashTable::create(16) /* #1141 */;
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
            insert_kv("checks_total", checks_total);
            insert_kv("rejects_total", rejects_total);
            insert_kv("max_fibers", max_fibers);
            insert_kv("max_mutations", max_mutations);
            insert_kv("schema", 1481);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1483 C3: query:per-fiber-mutation-stack-stats — exposes
    // the per_fiber_mutation_stack_depth_max + _current_max atomics
    // added at C2 (observability_metrics.h:1550-1551 area + wire sites
    // at evaluator_fiber_mutation.cpp:316 + :454). Returns a 3-field
    // hash: {lifetime-max, current-max, schema=1483}.
    //
    // Closes EDSL-visibility gap — the C2 metrics exist on
    // CompilerMetrics but no Aura primitive surfaces them, so
    // orchestration queries + LLM-bottleneck monitors can't observe
    // per-fiber mutation_stack_depth pressure without importing
    // observability_metrics directly. The new primitive reads the
    // metrics via the canonical Evaluator accessors
    // (get_per_fiber_mutation_stack_depth_max + _current_max) so
    // callers don't need to know about the CompilerMetrics layout.
    //
    // The lifetime-max + current-max pair distinguishes "all-time
    // peak across this Evaluator lifetime" from "current live peak
    // across active fibers" — useful for orchestrators tuning the
    // adaptive safepoint threshold (C4 follow-up).
    ObservabilityPrims::register_stats_impl(
        "query:per-fiber-mutation-stack-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t lifetime_max =
                static_cast<std::int64_t>(ev.get_per_fiber_mutation_stack_depth_max());
            const std::int64_t current_max =
                static_cast<std::int64_t>(ev.get_per_fiber_mutation_stack_depth_current_max());
            auto* ht = FlatHashTable::create(8) /* #1141 */;
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
            insert_kv("lifetime-max", lifetime_max);
            insert_kv("current-max", current_max);
            insert_kv("schema", 1483);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1483 C4: query:gc-safepoint-adaptive-stats — exposes
    // the adaptive safepoint threshold + adaptive-defer counter
    // added at C4 (observability_metrics.h safepoint_adaptive_*
    // atomics + wire in request_gc_safepoint at evaluator.ixx:4191
    // area + helper functions at evaluator.ixx:7198-7259 area).
    // Returns a 4-field hash: {threshold, defer_count, schema=1483}.
    //
    // Closes EDSL-visibility gap — the C4 adaptive threshold logic
    // lives on CompilerMetrics + Evaluator but no Aura primitive
    // surfaces it, so orchestration queries + LLM-bottleneck
    // monitors can't observe whether the adaptive heuristic is
    // backing off (threshold > 0) or how many adaptive deferrals
    // have happened (defer_count) without importing
    // observability_metrics directly.
    //
    // The threshold-doubled-per-defer pattern matches the
    // exponential-backoff heuristic (a) from the #1483 plan. The
    // pair of threshold + defer_count lets orchestrators verify
    // both the current backoff state AND the cumulative
    // adaptive-defer pressure (vs. the natural mutation_boundary_
    // depth > 0 defer path tracked by bump_gc_safepoint_deferred).
    ObservabilityPrims::register_stats_impl(
        "query:gc-safepoint-adaptive-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t threshold =
                static_cast<std::int64_t>(ev.get_safepoint_adaptive_threshold());
            const std::int64_t defer_count =
                static_cast<std::int64_t>(ev.get_safepoint_adaptive_defer_count());
            auto* ht = FlatHashTable::create(8) /* #1141 */;
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
            insert_kv("threshold", threshold);
            insert_kv("defer-count", defer_count);
            insert_kv("schema", 1483);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 98 (orig lines 20621-20671)
void ObservabilityPrims::register_jit_p98(PrimRegistrar add, Evaluator& ev) {
    // Issue #870: query:declarative-primitive-registry-stats
    ObservabilityPrims::register_stats_impl(
        "query:declarative-primitive-registry-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->decl_prim_reg_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->decl_prim_reg_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->decl_prim_reg_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16) /* #1141 */;
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
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 870);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 99 (orig lines 20672-20722)
void ObservabilityPrims::register_jit_p99(PrimRegistrar add, Evaluator& ev) {
    // Issue #872: query:primitives-namespace-alias-stats
    ObservabilityPrims::register_stats_impl(
        "query:primitives-namespace-alias-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->prim_ns_alias_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->prim_ns_alias_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->prim_ns_alias_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16) /* #1141 */;
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
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 872);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 100 (orig lines 20723-20774)
void ObservabilityPrims::register_jit_p100(PrimRegistrar add, Evaluator& ev) {
    // Issue #875: query:guard-steal-gc-safety-v2-stats
    ObservabilityPrims::register_stats_impl(
        "query:guard-steal-gc-safety-v2-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->guard_steal_gc_v2_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->guard_steal_gc_v2_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->guard_steal_gc_v2_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16) /* #1141 */;
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
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 875);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 101 (orig lines 20775-20826)
void ObservabilityPrims::register_jit_p101(PrimRegistrar add, Evaluator& ev) {
    // Issue #876: query:dirtyaware-ir-cache-consistency-stats
    ObservabilityPrims::register_stats_impl(
        "query:dirtyaware-ir-cache-consistency-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->dirty_ircache_cons_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->dirty_ircache_cons_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->dirty_ircache_cons_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16) /* #1141 */;
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
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 876);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 102 (orig lines 20827-20878)
void ObservabilityPrims::register_jit_p102(PrimRegistrar add, Evaluator& ev) {
    // Issue #877: query:stats-builder-refactor-stats
    ObservabilityPrims::register_stats_impl(
        "query:stats-builder-refactor-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->stats_builder_ref_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->stats_builder_ref_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->stats_builder_ref_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16) /* #1141 */;
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
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 877);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 103 (orig lines 20879-20930)
void ObservabilityPrims::register_jit_p103(PrimRegistrar add, Evaluator& ev) {
    // Issue #878: query:load-or-zero-helper-stats
    ObservabilityPrims::register_stats_impl(
        "query:load-or-zero-helper-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->load_or_zero_help_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->load_or_zero_help_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->load_or_zero_help_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16) /* #1141 */;
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
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 878);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

} // namespace aura::compiler::primitives_detail
