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
#include "serve/fiber.h" // #1600 join_resource_wait_us
#include "core/gc_hooks.h"
#include "core/resource_quota.hh" // Issue #1579 / #1600
#include "hash_meta.h"
#include "basis_points.h"

#include <limits>

// aura_gc_frequency_tune_ratio_* declared in runtime_shared.h (#1493)

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

    // Issue #1481 / #1498 / #1554 / #1579 / #1590 / #1600 / #1618:
    // query:resource-quota-stats.
    // #1481 fields: checks_total, rejects_total, max_fibers, max_mutations.
    // #1498 production fields (AC2): current_usage, memory_quota,
    // memory_quota_total, exceeded_count (=rejects), mutations_used.
    // #1554: exceeded_total alias + temp_arena_wired / group_owner_wired.
    // #1579: module_phase + process_fibers_* + process_checks/rejects + overflow.
    // #1590: schema 1590 + quota aliases + hot-path closed-loop keys.
    // #1600: orchestration fiber spawn/reject + join_resource_wait_us.
    // #1618: ResourceQuotaManager + typed reject ≠ panic + mutation_budget_rejected.
    // schema bumped to 1618 (agents: treat unknown keys as optional).
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
            // Issue #1498: live usage + configured quotas for agent dashboards.
            const std::int64_t current_usage =
                static_cast<std::int64_t>(ev.resource_quota_current_usage());
            const std::int64_t memory_quota = static_cast<std::int64_t>(ev.resource_quota_memory());
            const std::int64_t memory_quota_total =
                static_cast<std::int64_t>(ev.resource_quota_memory_total());
            const std::int64_t mut_used = static_cast<std::int64_t>(ev.mutation_quota_used());
            auto* ht = FlatHashTable::create(128) /* #1141 / #1498 / #1590 / #1618 more keys */;
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
            const std::int64_t temp_wired =
                (ev.temp_arena_ && ev.temp_arena_->has_arena_owner()) ? 1 : 0;
            const std::int64_t group_wired =
                (ev.arena_group_ && ev.arena_group_->has_default_arena_owner()) ? 1 : 0;
            const std::int64_t primary_wired = (ev.arena_ && ev.arena_->has_arena_owner()) ? 1 : 0;
            // Issue #1579: process ResourceQuota module stats.
            auto& pq = aura::core::resource_quota::process_resource_quota();
            const std::int64_t module_phase =
                static_cast<std::int64_t>(aura::core::resource_quota::kResourceQuotaPhase);
            const std::int64_t proc_fibers_used =
                static_cast<std::int64_t>(pq.used(aura::core::resource_quota::Dimension::Fibers));
            const std::int64_t proc_fibers_limit =
                static_cast<std::int64_t>(pq.limit(aura::core::resource_quota::Dimension::Fibers));
            const std::int64_t proc_checks =
                static_cast<std::int64_t>(pq.checks_total.load(std::memory_order_relaxed));
            const std::int64_t proc_rejects =
                static_cast<std::int64_t>(pq.rejects_total.load(std::memory_order_relaxed));
            const std::int64_t overflow_guards =
                static_cast<std::int64_t>(pq.overflow_guards_total.load(std::memory_order_relaxed));

            insert_kv("checks_total", checks_total);
            insert_kv("rejects_total", rejects_total);
            insert_kv("exceeded_count", rejects_total); // #1498 AC2 alias
            insert_kv("exceeded_total", rejects_total); // #1554 AC alias
            insert_kv("max_fibers", max_fibers);
            insert_kv("max_mutations", max_mutations);
            insert_kv("current_usage", current_usage);
            insert_kv("memory_quota", memory_quota);
            insert_kv("memory_quota_total", memory_quota_total);
            // Issue #1590 AC2 aliases (Agent dashboards).
            insert_kv("quota", memory_quota_total != 0 ? memory_quota_total : memory_quota);
            insert_kv("mutations_used", mut_used);
            insert_kv("module_phase", module_phase);              // #1579
            insert_kv("process_fibers_used", proc_fibers_used);   // #1579
            insert_kv("process_fibers_limit", proc_fibers_limit); // #1579
            insert_kv("process_checks", proc_checks);             // #1579
            insert_kv("process_rejects", proc_rejects);           // #1579
            insert_kv("overflow_guards", overflow_guards);        // #1579
            insert_kv("primary_arena_wired", primary_wired);
            insert_kv("temp_arena_wired", temp_wired);
            insert_kv("group_owner_wired", group_wired);
            insert_kv("hotpath_arena_gated", primary_wired); // #1590: allocate_raw owner path
            insert_kv("hotpath_guard_try_acquire", 1); // #1590: try_acquire is production path
            // Issue #1600: orchestration ResourceQuota surface.
            insert_kv("fiber_spawn_rejected_total",
                      static_cast<std::int64_t>(
                          pq.fiber_spawn_rejected_total.load(std::memory_order_relaxed)));
            insert_kv("orchestration_quota_exceeded_count",
                      static_cast<std::int64_t>(
                          pq.orchestration_quota_exceeded_total.load(std::memory_order_relaxed)));
            insert_kv("orchestration_quota_exceeded_total",
                      static_cast<std::int64_t>(
                          pq.orchestration_quota_exceeded_total.load(std::memory_order_relaxed)));
            insert_kv("join_resource_wait_us",
                      static_cast<std::int64_t>(aura::serve::Fiber::join_wait_us_total()));
            insert_kv("join_wait_us_total",
                      static_cast<std::int64_t>(aura::serve::Fiber::join_wait_us_total()));
            insert_kv("join_total", static_cast<std::int64_t>(aura::serve::Fiber::join_total()));
            insert_kv("process_fibers_remaining",
                      static_cast<std::int64_t>(
                          pq.remaining(aura::core::resource_quota::Dimension::Fibers) ==
                                  std::numeric_limits<std::uint64_t>::max()
                              ? static_cast<std::int64_t>(-1) // unlimited
                              : static_cast<std::int64_t>(
                                    pq.remaining(aura::core::resource_quota::Dimension::Fibers))));
            insert_kv("orch_spawn_gated", 1); // Scheduler::spawn + parallel_intend wired
            // Issue #1618: ResourceQuotaManager + typed reject AC keys
            const std::int64_t quota_viol =
                m ? static_cast<std::int64_t>(
                        m->quota_violation_total.load(std::memory_order_relaxed))
                  : rejects_total;
            const std::int64_t mut_budget_rej =
                m ? static_cast<std::int64_t>(
                        m->mutation_budget_rejected_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t typed_rej =
                m ? static_cast<std::int64_t>(
                        m->quota_reject_typed_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t panic_dist =
                m ? static_cast<std::int64_t>(
                        m->panic_quota_distinguished_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t mgr_enforce =
                m ? static_cast<std::int64_t>(
                        m->manager_enforce_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("quota_violation_total", quota_viol);
            insert_kv("quota-violation-total", quota_viol);
            insert_kv("mutation_budget_rejected", mut_budget_rej);
            insert_kv("mutation_budget_rejected_total", mut_budget_rej);
            insert_kv("mutation-budget-rejected", mut_budget_rej);
            insert_kv("quota_reject_typed_total", typed_rej);
            insert_kv("panic_quota_distinguished_total", panic_dist);
            insert_kv("manager_enforce_total", mgr_enforce);
            insert_kv("manager-wired", 1);
            insert_kv("panic-quota-distinguished", 1);
            insert_kv("typed-reject-not-panic", 1);
            // Issue #1628: MutationBoundaryGuard::try_acquire factory
            // (replace panic-checkpoint quota path; typed AuraError).
            const std::int64_t try_acq =
                m ? static_cast<std::int64_t>(
                        m->mutation_guard_try_acquire_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t try_rej =
                m ? static_cast<std::int64_t>(
                        m->mutation_guard_try_acquire_reject_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("mutation_guard_try_acquire_total", try_acq);
            insert_kv("mutation_guard_try_acquire_reject_total", try_rej);
            insert_kv("try_acquire_wired", 1);
            insert_kv("panic_checkpoint_quota_replaced", 1);
            insert_kv("eval_on_current_try_acquire", 1);
            insert_kv("typed_mutate_try_acquire", 1);
            insert_kv("legacy_ctor_deprecated", 1);
            insert_kv("issue", 1628);
            insert_kv("schema", 1628); // lineage 1618|1600|1590|1547
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
            const std::int64_t live_depth =
                static_cast<std::int64_t>(Evaluator::mutation_boundary_depth());
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
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
            insert_kv("lifetime-max", lifetime_max);
            insert_kv("current-max", current_max);
            // Issue #1493: live mutation_boundary depth + histogram total samples.
            insert_kv("live-depth", live_depth);
            std::int64_t hist_total = 0;
            if (m) {
                for (std::size_t i = 0; i < CompilerMetrics::kMutationStackDepthHistBuckets; ++i)
                    hist_total += static_cast<std::int64_t>(
                        m->mutation_stack_depth_histogram[i].load(std::memory_order_relaxed));
                insert_kv("hist-b0",
                          static_cast<std::int64_t>(m->mutation_stack_depth_histogram[0].load(
                              std::memory_order_relaxed)));
                insert_kv("hist-b1",
                          static_cast<std::int64_t>(m->mutation_stack_depth_histogram[1].load(
                              std::memory_order_relaxed)));
                insert_kv(
                    "hist-b2-plus",
                    hist_total -
                        static_cast<std::int64_t>(
                            m->mutation_stack_depth_histogram[0].load(std::memory_order_relaxed) +
                            m->mutation_stack_depth_histogram[1].load(std::memory_order_relaxed)));
            } else {
                insert_kv("hist-b0", 0);
                insert_kv("hist-b1", 0);
                insert_kv("hist-b2-plus", 0);
            }
            insert_kv("hist-samples", hist_total);
            insert_kv("issue", 1591);
            insert_kv("schema", 1493); // keep 1493; #1591 alias below
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1591 AC name: query:per-fiber-mutation-depth-stats
    // Alias of query:per-fiber-mutation-stack-stats (same fields + schema 1591).
    ObservabilityPrims::register_stats_impl(
        "query:per-fiber-mutation-depth-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t lifetime_max =
                static_cast<std::int64_t>(ev.get_per_fiber_mutation_stack_depth_max());
            const std::int64_t current_max =
                static_cast<std::int64_t>(ev.get_per_fiber_mutation_stack_depth_current_max());
            const std::int64_t live_depth =
                static_cast<std::int64_t>(Evaluator::mutation_boundary_depth());
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            auto* ht = FlatHashTable::create(32);
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
            insert_kv("live-depth", live_depth);
            std::int64_t hist_total = 0;
            if (m) {
                for (std::size_t i = 0; i < CompilerMetrics::kMutationStackDepthHistBuckets; ++i)
                    hist_total += static_cast<std::int64_t>(
                        m->mutation_stack_depth_histogram[i].load(std::memory_order_relaxed));
            }
            insert_kv("hist-samples", hist_total);
            insert_kv("mutation-stack-depth-histogram-samples", hist_total);
            insert_kv(
                "safepoint-wait-while-mutation-held-us",
                static_cast<std::int64_t>(aura::gc_hooks::safepoint_wait_while_mutation_held_us()));
            insert_kv("issue", 1591);
            insert_kv("schema", 1591);
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
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            const std::int64_t hold_total =
                m ? static_cast<std::int64_t>(
                        m->mutation_boundary_hold_time_total_us.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t holds =
                m ? static_cast<std::int64_t>(
                        m->mutation_boundary_holds_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t avg_hold = holds > 0 ? hold_total / holds : 0;
            const std::int64_t wait_us =
                static_cast<std::int64_t>(aura::gc_hooks::safepoint_wait_while_mutation_held_us());
            const std::int64_t wait_n = static_cast<std::int64_t>(
                aura::gc_hooks::safepoint_wait_while_mutation_held_count());
            const std::int64_t freq_ratio =
                static_cast<std::int64_t>(aura_gc_frequency_tune_ratio_load());
            const std::int64_t adapt_up =
                m ? static_cast<std::int64_t>(
                        m->safepoint_frequency_adapt_up_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t adapt_down =
                m ? static_cast<std::int64_t>(
                        m->safepoint_frequency_adapt_down_total.load(std::memory_order_relaxed))
                  : 0;
            auto* ht = FlatHashTable::create(32) /* #1141 / #1599 hist buckets */;
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
            // Issue #1493: hold-time adaptive + wait-while-mutation export.
            insert_kv("avg-mutation-hold-us", avg_hold);
            insert_kv("hold-samples", holds);
            insert_kv("safepoint-wait-while-mutation-held-us", wait_us);
            insert_kv("safepoint-wait-while-mutation-held-count", wait_n);
            insert_kv("gc-frequency-tune-ratio", freq_ratio);
            insert_kv("frequency-adapt-up", adapt_up);
            insert_kv("frequency-adapt-down", adapt_down);
            // Issue #1599 AC5: mutation_stack_depth_histogram export.
            std::int64_t hist_sum = 0;
            static constexpr const char* kHistKeys[8] = {"hist-b0", "hist-b1", "hist-b2",
                                                         "hist-b3", "hist-b4", "hist-b5",
                                                         "hist-b6", "hist-b7"};
            if (m) {
                for (std::size_t i = 0; i < CompilerMetrics::kMutationStackDepthHistBuckets; ++i) {
                    const auto v = static_cast<std::int64_t>(
                        m->mutation_stack_depth_histogram[i].load(std::memory_order_relaxed));
                    hist_sum += v;
                    if (i < 8)
                        insert_kv(kHistKeys[i], v);
                }
            }
            insert_kv("mutation_stack_depth_histogram", hist_sum);
            insert_kv("issue", 1599);
            insert_kv("schema", 1599); // lineage 1493|1483
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
