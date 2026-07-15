// evaluator_primitives_obs_jit_08.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 64 (orig lines 18873-18925)
void ObservabilityPrims::register_jit_p64(PrimRegistrar add, Evaluator& ev) {
    // Issue #835: query:typed-mutate-coercion-stats
    ObservabilityPrims::register_stats_impl(
        "query:typed-mutate-coercion-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->typed_mutate_coercion_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->typed_mutate_coercion_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->typed_mutate_coercion_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 835);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 65 (orig lines 18926-18976)
void ObservabilityPrims::register_jit_p65(PrimRegistrar add, Evaluator& ev) {
    // Issue #836: query:fiber-epoch-type-safety-stats
    ObservabilityPrims::register_stats_impl(
        "query:fiber-epoch-type-safety-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->fiber_epoch_type_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->fiber_epoch_type_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->fiber_epoch_type_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 836);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 66 (orig lines 18977-19028)
void ObservabilityPrims::register_jit_p66(PrimRegistrar add, Evaluator& ev) {
    // Issue #837: query:sv-verification-feedback-mutate-stats
    ObservabilityPrims::register_stats_impl(
        "query:sv-verification-feedback-mutate-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->sv_feedback_mutate_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->sv_feedback_mutate_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->sv_feedback_mutate_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 837);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 67 (orig lines 19029-19079)
void ObservabilityPrims::register_jit_p67(PrimRegistrar add, Evaluator& ev) {
    // Issue #838: query:seva-longrunning-harness-v2-stats
    ObservabilityPrims::register_stats_impl(
        "query:seva-longrunning-harness-v2-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->seva_harness_v2_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->seva_harness_v2_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->seva_harness_v2_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 838);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 68 (orig lines 19080-19130)
void ObservabilityPrims::register_jit_p68(PrimRegistrar add, Evaluator& ev) {
    // Issue #839: query:typed-mutation-audit-stats
    ObservabilityPrims::register_stats_impl(
        "query:typed-mutation-audit-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->typed_mut_audit_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->typed_mut_audit_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->typed_mut_audit_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 839);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 69 (orig lines 19131-19182)
void ObservabilityPrims::register_jit_p69(PrimRegistrar add, Evaluator& ev) {
    // Issue #840: query:stable-ref-full-provenance-v2-stats
    ObservabilityPrims::register_stats_impl(
        "query:stable-ref-full-provenance-v2-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->stable_ref_full_v2_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->stable_ref_full_v2_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->stable_ref_full_v2_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 840);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 70 (orig lines 19183-19233)
void ObservabilityPrims::register_jit_p70(PrimRegistrar add, Evaluator& ev) {
    // Issue #842: query:longrunning-ai-infra-stats
    ObservabilityPrims::register_stats_impl(
        "query:longrunning-ai-infra-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->longrun_ai_infra_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->longrun_ai_infra_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->longrun_ai_infra_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 842);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 71 (orig lines 19234-19284)
void ObservabilityPrims::register_jit_p71(PrimRegistrar add, Evaluator& ev) {
    // Issue #843: query:ai-native-meta-extension-stats
    ObservabilityPrims::register_stats_impl(
        "query:ai-native-meta-extension-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->ai_native_meta_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->ai_native_meta_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->ai_native_meta_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 843);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

} // namespace aura::compiler::primitives_detail
