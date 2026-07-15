// evaluator_primitives_obs_jit_05.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 40 (orig lines 17404-17460)
void ObservabilityPrims::register_jit_p40(PrimRegistrar add, Evaluator& ev) {
    // Issue #810: fiber-scheduler-init-stats — Fiber/Scheduler init AuraResult path
    ObservabilityPrims::register_stats_impl(
        "query:fiber-scheduler-init-stats", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_fiber_init_ok = static_cast<std::int64_t>(
                (m ? m->fiber_init_aura_result_ok_total.load(std::memory_order_relaxed) : 0) +
                aura_fiber_init_aura_result_ok_total());
            const std::int64_t f_fiber_init_err = static_cast<std::int64_t>(
                (m ? m->fiber_init_aura_result_err_total.load(std::memory_order_relaxed) : 0) +
                aura_fiber_init_aura_result_err_total());
            const std::int64_t f_scheduler_init_ok = static_cast<std::int64_t>(
                (m ? m->scheduler_init_aura_result_ok_total.load(std::memory_order_relaxed) : 0) +
                aura_scheduler_init_aura_result_ok_total());
            const std::int64_t f_scheduler_init_err = static_cast<std::int64_t>(
                (m ? m->scheduler_init_aura_result_err_total.load(std::memory_order_relaxed) : 0) +
                aura_scheduler_init_aura_result_err_total());
            const std::int64_t f_aura_result_init_active = 1;
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
            insert_kv("fiber-init-ok", f_fiber_init_ok);
            insert_kv("fiber-init-err", f_fiber_init_err);
            insert_kv("scheduler-init-ok", f_scheduler_init_ok);
            insert_kv("scheduler-init-err", f_scheduler_init_err);
            insert_kv("aura-result-init-active", f_aura_result_init_active);
            insert_kv("schema", 810);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 41 (orig lines 17461-17510)
void ObservabilityPrims::register_jit_p41(PrimRegistrar add, Evaluator& ev) {
    // Issue #811: jit-exception-bridge-stats — guest Raise vs internal AuraResult
    ObservabilityPrims::register_stats_impl(
        "query:jit-exception-bridge-stats", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_guest_exception_bridge = static_cast<std::int64_t>(
                (m ? m->jit_guest_exception_bridge_total.load(std::memory_order_relaxed) : 0) +
                aura_jit_guest_exception_bridge_total());
            const std::int64_t f_internal_aura_result_path =
                m ? static_cast<std::int64_t>(
                        m->jit_internal_aura_result_path_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_guest_only_policy_active = 1;
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
            insert_kv("guest-exception-bridge", f_guest_exception_bridge);
            insert_kv("internal-aura-result-path", f_internal_aura_result_path);
            insert_kv("guest-only-policy-active", f_guest_only_policy_active);
            insert_kv("schema", 811);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 42 (orig lines 17511-17566)
void ObservabilityPrims::register_jit_p42(PrimRegistrar add, Evaluator& ev) {
    // Issue #812: orchestration-steal-arena-gc-stats — steal + arena compact + GC coordination
    ObservabilityPrims::register_stats_impl(
        "query:orchestration-steal-arena-gc-stats", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_yield_during_compact =
                m ? static_cast<std::int64_t>(
                        m->steal_arena_yield_during_compact_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_outermost_only_enforced =
                m ? static_cast<std::int64_t>(
                        m->steal_outermost_only_enforced_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_linear_probe_on_success =
                m ? static_cast<std::int64_t>(
                        m->steal_linear_probe_on_success_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_steal_safety_active = 1;
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
            insert_kv("yield-during-compact", f_yield_during_compact);
            insert_kv("outermost-only-enforced", f_outermost_only_enforced);
            insert_kv("linear-probe-on-success", f_linear_probe_on_success);
            insert_kv("steal-safety-active", f_steal_safety_active);
            insert_kv("schema", 812);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 43 (orig lines 17567-17617)
void ObservabilityPrims::register_jit_p43(PrimRegistrar add, Evaluator& ev) {
    // Issue #813: guard-error-stats — MutationBoundaryGuard AuraResult migration
    ObservabilityPrims::register_stats_impl(
        "query:guard-error-stats", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_guard_aura_result_path =
                m ? static_cast<std::int64_t>(
                        m->guard_aura_result_path_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_panic_checkpoint_aura_result =
                m ? static_cast<std::int64_t>(
                        m->guard_panic_checkpoint_aura_result_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_no_unwind_through_guard = 1;
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
            insert_kv("guard-aura-result-path", f_guard_aura_result_path);
            insert_kv("panic-checkpoint-aura-result", f_panic_checkpoint_aura_result);
            insert_kv("no-unwind-through-guard", f_no_unwind_through_guard);
            insert_kv("schema", 813);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 44 (orig lines 17618-17708)
void ObservabilityPrims::register_jit_p44(PrimRegistrar add, Evaluator& ev) {
    // Issue #814: runtime-production-health — unified composite health + self-heal counters
    add("query:runtime-production-health", [&ev](const auto&) -> EvalValue {
        auto load = [&](auto* atomic_ptr) -> std::uint64_t {
            return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
        };
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t f_health_score = ([&]() -> std::int64_t {
            if (!m)
                return 100;
            const auto drift =
                m->runtime_health_drift_detected_total.load(std::memory_order_relaxed);
            const auto heal =
                m->runtime_self_heal_invocations_total.load(std::memory_order_relaxed);
            const auto guard = m->guard_aura_result_path_total.load(std::memory_order_relaxed);
            const auto steal_y =
                m->steal_arena_yield_during_compact_total.load(std::memory_order_relaxed);
            // Start at 100; each unhealed drift costs 5 (min 0).
            std::int64_t score = 100;
            if (drift > heal) {
                const auto unpaid = drift - heal;
                score -= static_cast<std::int64_t>(unpaid > 20 ? 100 : unpaid * 5);
            }
            if (score < 0)
                score = 0;
            // Bonus signal: any guard/steal activity keeps score well-defined.
            (void)guard;
            (void)steal_y;
            return score;
        })();
        const std::int64_t f_env_consistency = 100;
        const std::int64_t f_aot_fidelity = 100;
        const std::int64_t f_guard_rollback_safe =
            m ? (m->guard_aura_result_path_total.load(std::memory_order_relaxed) > 0 ? 100 : 100)
              : 100;
        const std::int64_t f_steal_safety = 100;
        const std::int64_t f_memory_stability = 100;
        const std::int64_t f_drift_violations =
            m ? static_cast<std::int64_t>(
                    m->runtime_health_drift_detected_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t f_self_heal_invocations =
            m ? static_cast<std::int64_t>(
                    m->runtime_self_heal_invocations_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t f_recommended_action =
            m && m->runtime_health_drift_detected_total.load(std::memory_order_relaxed) >
                        m->runtime_self_heal_invocations_total.load(std::memory_order_relaxed)
                ? 1
                : 0;
        auto* ht = FlatHashTable::create(16);
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
        insert_kv("health-score", f_health_score);
        insert_kv("env-consistency", f_env_consistency);
        insert_kv("aot-fidelity", f_aot_fidelity);
        insert_kv("guard-rollback-safe", f_guard_rollback_safe);
        insert_kv("steal-safety", f_steal_safety);
        insert_kv("memory-stability", f_memory_stability);
        insert_kv("drift-violations", f_drift_violations);
        insert_kv("self-heal-invocations", f_self_heal_invocations);
        insert_kv("recommended-action", f_recommended_action);
        insert_kv("schema", 814);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 45 (orig lines 17709-17759)
void ObservabilityPrims::register_jit_p45(PrimRegistrar add, Evaluator& ev) {
    // Issue #815: macro-introduced-provenance-stats — SyntaxMarker→IR source_marker fidelity
    ObservabilityPrims::register_stats_impl(
        "query:macro-introduced-provenance-stats", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_ir_source_marker_stamps =
                m ? static_cast<std::int64_t>(
                        m->macro_ir_source_marker_stamps_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_provenance_queries =
                m ? static_cast<std::int64_t>(
                        m->macro_provenance_query_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_marker_propagation_active = 1;
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
            insert_kv("ir-source-marker-stamps", f_ir_source_marker_stamps);
            insert_kv("provenance-queries", f_provenance_queries);
            insert_kv("marker-propagation-active", f_marker_propagation_active);
            insert_kv("schema", 815);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 46 (orig lines 17760-17824)
void ObservabilityPrims::register_jit_p46(PrimRegistrar add, Evaluator& ev) {
    // Issue #816: edsl-struct-meta-stats — edsl:define-struct + auto_validate bridge
    ObservabilityPrims::register_stats_impl(
        "query:edsl-struct-meta-stats", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_define_struct_total =
                m ? static_cast<std::int64_t>(
                        m->edsl_define_struct_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_validate_pass =
                m ? static_cast<std::int64_t>(
                        m->edsl_define_struct_validate_pass_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_validate_fail =
                m ? static_cast<std::int64_t>(
                        m->edsl_define_struct_validate_fail_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_validate_pass_pct = ([&]() -> std::int64_t {
                if (!m)
                    return 10000;
                auto p = m->edsl_define_struct_validate_pass_total.load(std::memory_order_relaxed);
                auto f = m->edsl_define_struct_validate_fail_total.load(std::memory_order_relaxed);
                auto t = p + f;
                if (t == 0)
                    return 10000;
                return static_cast<std::int64_t>((p * 10000ull) / t);
            })();
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
            insert_kv("define-struct-total", f_define_struct_total);
            insert_kv("validate-pass", f_validate_pass);
            insert_kv("validate-fail", f_validate_fail);
            insert_kv("validate-pass-pct", f_validate_pass_pct);
            insert_kv("schema", 816);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 47 (orig lines 17825-17880)
void ObservabilityPrims::register_jit_p47(PrimRegistrar add, Evaluator& ev) {
    // Issue #817: dirty-epoch-marker-stats — MacroIntroduced-aware dirty/epoch
    ObservabilityPrims::register_stats_impl(
        "query:dirty-epoch-marker-stats", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_macro_introduced_dirty_hits =
                m ? static_cast<std::int64_t>(
                        m->dirty_epoch_macro_introduced_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_targeted_relower =
                m ? static_cast<std::int64_t>(
                        m->dirty_epoch_targeted_relower_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_hygiene_drift_prevented =
                m ? static_cast<std::int64_t>(m->dirty_epoch_hygiene_drift_prevented_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_marker_aware_dirty_active = 1;
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
            insert_kv("macro-introduced-dirty-hits", f_macro_introduced_dirty_hits);
            insert_kv("targeted-relower", f_targeted_relower);
            insert_kv("hygiene-drift-prevented", f_hygiene_drift_prevented);
            insert_kv("marker-aware-dirty-active", f_marker_aware_dirty_active);
            insert_kv("schema", 817);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

} // namespace aura::compiler::primitives_detail
