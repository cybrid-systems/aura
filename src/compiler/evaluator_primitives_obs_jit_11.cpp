// evaluator_primitives_obs_jit_11.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 88 (orig lines 20108-20158)
void ObservabilityPrims::register_jit_p88(PrimRegistrar add, Evaluator& ev) {
    // Issue #860: query:ir-soa-dirty-hybrid-full-v2-stats
    add("query:ir-soa-dirty-hybrid-full-v2-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total =
            m ? static_cast<std::int64_t>(m->irsoa_dirty_v2_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hits = m ? static_cast<std::int64_t>(m->irsoa_dirty_v2_hits_total.load(
                                          std::memory_order_relaxed))
                                    : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->irsoa_dirty_v2_savings_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t active = 1;
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
        insert_kv("total", total);
        insert_kv("hits", hits);
        insert_kv("savings", savings);
        insert_kv("active", active);
        insert_kv("schema", 860);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 89 (orig lines 20159-20210)
void ObservabilityPrims::register_jit_p89(PrimRegistrar add, Evaluator& ev) {
    // Issue #861: query:value-shape-consteval-full-v2-stats
    add("query:value-shape-consteval-full-v2-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total = m ? static_cast<std::int64_t>(m->val_shape_ceval_v2_total.load(
                                           std::memory_order_relaxed))
                                     : 0;
        const std::int64_t hits =
            m ? static_cast<std::int64_t>(
                    m->val_shape_ceval_v2_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->val_shape_ceval_v2_savings_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t active = 1;
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
        insert_kv("total", total);
        insert_kv("hits", hits);
        insert_kv("savings", savings);
        insert_kv("active", active);
        insert_kv("schema", 861);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 90 (orig lines 20211-20262)
void ObservabilityPrims::register_jit_p90(PrimRegistrar add, Evaluator& ev) {
    // Issue #862: query:defuse-infer-partial-stats
    add("query:defuse-infer-partial-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total = m ? static_cast<std::int64_t>(m->defuse_infer_part_total.load(
                                           std::memory_order_relaxed))
                                     : 0;
        const std::int64_t hits =
            m ? static_cast<std::int64_t>(
                    m->defuse_infer_part_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->defuse_infer_part_savings_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t active = 1;
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
        insert_kv("total", total);
        insert_kv("hits", hits);
        insert_kv("savings", savings);
        insert_kv("active", active);
        insert_kv("schema", 862);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 91 (orig lines 20263-20313)
void ObservabilityPrims::register_jit_p91(PrimRegistrar add, Evaluator& ev) {
    // Issue #863: query:ownership-escape-postmutate-stats
    add("query:ownership-escape-postmutate-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total =
            m ? static_cast<std::int64_t>(m->own_escape_post_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hits = m ? static_cast<std::int64_t>(m->own_escape_post_hits_total.load(
                                          std::memory_order_relaxed))
                                    : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->own_escape_post_savings_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t active = 1;
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
        insert_kv("total", total);
        insert_kv("hits", hits);
        insert_kv("savings", savings);
        insert_kv("active", active);
        insert_kv("schema", 863);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 92 (orig lines 20314-20364)
void ObservabilityPrims::register_jit_p92(PrimRegistrar add, Evaluator& ev) {
    // Issue #864: query:typed-mutation-audit-pass-stats
    add("query:typed-mutation-audit-pass-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total =
            m ? static_cast<std::int64_t>(m->typed_audit_pass_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hits = m ? static_cast<std::int64_t>(m->typed_audit_pass_hits_total.load(
                                          std::memory_order_relaxed))
                                    : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->typed_audit_pass_savings_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t active = 1;
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
        insert_kv("total", total);
        insert_kv("hits", hits);
        insert_kv("savings", savings);
        insert_kv("active", active);
        insert_kv("schema", 864);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 93 (orig lines 20365-20415)
void ObservabilityPrims::register_jit_p93(PrimRegistrar add, Evaluator& ev) {
    // Issue #865: query:sv-backend-emit-bidirectional-stats
    add("query:sv-backend-emit-bidirectional-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total =
            m ? static_cast<std::int64_t>(m->sv_backend_bi_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hits = m ? static_cast<std::int64_t>(m->sv_backend_bi_hits_total.load(
                                          std::memory_order_relaxed))
                                    : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->sv_backend_bi_savings_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t active = 1;
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
        insert_kv("total", total);
        insert_kv("hits", hits);
        insert_kv("savings", savings);
        insert_kv("active", active);
        insert_kv("schema", 865);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 94 (orig lines 20416-20466)
void ObservabilityPrims::register_jit_p94(PrimRegistrar add, Evaluator& ev) {
    // Issue #866: query:large-sv-pattern-defuse-stats
    add("query:large-sv-pattern-defuse-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total =
            m ? static_cast<std::int64_t>(m->large_sv_pattern_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hits = m ? static_cast<std::int64_t>(m->large_sv_pattern_hits_total.load(
                                          std::memory_order_relaxed))
                                    : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->large_sv_pattern_savings_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t active = 1;
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
        insert_kv("total", total);
        insert_kv("hits", hits);
        insert_kv("savings", savings);
        insert_kv("active", active);
        insert_kv("schema", 866);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 95 (orig lines 20467-20518)
void ObservabilityPrims::register_jit_p95(PrimRegistrar add, Evaluator& ev) {
    // Issue #867: query:longrunning-stable-ref-dirty-stats
    add("query:longrunning-stable-ref-dirty-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total = m ? static_cast<std::int64_t>(m->longrun_sref_dirty_total.load(
                                           std::memory_order_relaxed))
                                     : 0;
        const std::int64_t hits =
            m ? static_cast<std::int64_t>(
                    m->longrun_sref_dirty_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->longrun_sref_dirty_savings_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t active = 1;
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
        insert_kv("total", total);
        insert_kv("hits", hits);
        insert_kv("savings", savings);
        insert_kv("active", active);
        insert_kv("schema", 867);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

} // namespace aura::compiler::primitives_detail
