// evaluator_primitives_obs_jit_09.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 72 (orig lines 19285-19335)
void ObservabilityPrims::register_jit_p72(PrimRegistrar add, Evaluator& ev) {
    // Issue #844: query:orchestration-telemetry-pipeline-stats
    add("query:orchestration-telemetry-pipeline-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total =
            m ? static_cast<std::int64_t>(m->orch_telemetry_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hits = m ? static_cast<std::int64_t>(m->orch_telemetry_hits_total.load(
                                          std::memory_order_relaxed))
                                    : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->orch_telemetry_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 844);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 73 (orig lines 19336-19387)
void ObservabilityPrims::register_jit_p73(PrimRegistrar add, Evaluator& ev) {
    // Issue #845: query:per-fiber-exception-state-stats
    add("query:per-fiber-exception-state-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total = m ? static_cast<std::int64_t>(m->per_fiber_ex_state_total.load(
                                           std::memory_order_relaxed))
                                     : 0;
        const std::int64_t hits =
            m ? static_cast<std::int64_t>(
                    m->per_fiber_ex_state_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->per_fiber_ex_state_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 845);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 74 (orig lines 19388-19438)
void ObservabilityPrims::register_jit_p74(PrimRegistrar add, Evaluator& ev) {
    // Issue #846: query:aot-hotswap-pipeline-stats
    add("query:aot-hotswap-pipeline-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total =
            m ? static_cast<std::int64_t>(m->aot_hotswap_pipe_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hits = m ? static_cast<std::int64_t>(m->aot_hotswap_pipe_hits_total.load(
                                          std::memory_order_relaxed))
                                    : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->aot_hotswap_pipe_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 846);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 75 (orig lines 19439-19490)
void ObservabilityPrims::register_jit_p75(PrimRegistrar add, Evaluator& ev) {
    // Issue #847: query:macro-hygiene-query-provenance-v2-stats
    add("query:macro-hygiene-query-provenance-v2-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total = m ? static_cast<std::int64_t>(m->macro_hyg_query_v2_total.load(
                                           std::memory_order_relaxed))
                                     : 0;
        const std::int64_t hits =
            m ? static_cast<std::int64_t>(
                    m->macro_hyg_query_v2_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->macro_hyg_query_v2_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 847);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 76 (orig lines 19491-19541)
void ObservabilityPrims::register_jit_p76(PrimRegistrar add, Evaluator& ev) {
    // Issue #848: query:reflection-edsl-extension-v2-stats
    add("query:reflection-edsl-extension-v2-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total =
            m ? static_cast<std::int64_t>(m->reflect_edsl_v2_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hits = m ? static_cast<std::int64_t>(m->reflect_edsl_v2_hits_total.load(
                                          std::memory_order_relaxed))
                                    : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->reflect_edsl_v2_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 848);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 77 (orig lines 19542-19593)
void ObservabilityPrims::register_jit_p77(PrimRegistrar add, Evaluator& ev) {
    // Issue #849: query:self-evolution-hygiene-dirty-epoch-stats
    add("query:self-evolution-hygiene-dirty-epoch-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total = m ? static_cast<std::int64_t>(m->selfevo_hyg_dirty_total.load(
                                           std::memory_order_relaxed))
                                     : 0;
        const std::int64_t hits =
            m ? static_cast<std::int64_t>(
                    m->selfevo_hyg_dirty_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->selfevo_hyg_dirty_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 849);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 78 (orig lines 19594-19644)
void ObservabilityPrims::register_jit_p78(PrimRegistrar add, Evaluator& ev) {
    // Issue #850: query:sv-verification-feedback-closedloop-stats
    add("query:sv-verification-feedback-closedloop-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total =
            m ? static_cast<std::int64_t>(m->sv_fb_closedloop_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hits = m ? static_cast<std::int64_t>(m->sv_fb_closedloop_hits_total.load(
                                          std::memory_order_relaxed))
                                    : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->sv_fb_closedloop_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 850);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 79 (orig lines 19645-19696)
void ObservabilityPrims::register_jit_p79(PrimRegistrar add, Evaluator& ev) {
    // Issue #851: query:pattern-defuse-hygiene-full-stats
    add("query:pattern-defuse-hygiene-full-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total = m ? static_cast<std::int64_t>(m->pattern_defuse_hyg_total.load(
                                           std::memory_order_relaxed))
                                     : 0;
        const std::int64_t hits =
            m ? static_cast<std::int64_t>(
                    m->pattern_defuse_hyg_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->pattern_defuse_hyg_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 851);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

} // namespace aura::compiler::primitives_detail
