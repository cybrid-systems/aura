// evaluator_primitives_obs_jit_13.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 104 (orig lines 20931-20981)
void ObservabilityPrims::register_jit_p104(PrimRegistrar add, Evaluator& ev) {
    // Issue #879: query:cpp26-modernization-sweep-stats
    add("query:cpp26-modernization-sweep-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total =
            m ? static_cast<std::int64_t>(m->cpp26_mod_sweep_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hits = m ? static_cast<std::int64_t>(m->cpp26_mod_sweep_hits_total.load(
                                          std::memory_order_relaxed))
                                    : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->cpp26_mod_sweep_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 879);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 105 (orig lines 20982-21033)
void ObservabilityPrims::register_jit_p105(PrimRegistrar add, Evaluator& ev) {
    // Issue #880: query:metrics-meta-reflection-stats
    add("query:metrics-meta-reflection-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total = m ? static_cast<std::int64_t>(m->metrics_meta_refl_total.load(
                                           std::memory_order_relaxed))
                                     : 0;
        const std::int64_t hits =
            m ? static_cast<std::int64_t>(
                    m->metrics_meta_refl_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->metrics_meta_refl_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 880);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 106 (orig lines 21034-21085)
void ObservabilityPrims::register_jit_p106(PrimRegistrar add, Evaluator& ev) {
    // Issue #881: query:test-harness-bootstrap-stats
    add("query:test-harness-bootstrap-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total = m ? static_cast<std::int64_t>(m->test_harness_boot_total.load(
                                           std::memory_order_relaxed))
                                     : 0;
        const std::int64_t hits =
            m ? static_cast<std::int64_t>(
                    m->test_harness_boot_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->test_harness_boot_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 881);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 107 (orig lines 21086-21137)
void ObservabilityPrims::register_jit_p107(PrimRegistrar add, Evaluator& ev) {
    // Issue #882: query:bundle-codegen-decouple-stats
    add("query:bundle-codegen-decouple-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total = m ? static_cast<std::int64_t>(m->bundle_codegen_dec_total.load(
                                           std::memory_order_relaxed))
                                     : 0;
        const std::int64_t hits =
            m ? static_cast<std::int64_t>(
                    m->bundle_codegen_dec_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->bundle_codegen_dec_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 882);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 108 (orig lines 21138-21188)
void ObservabilityPrims::register_jit_p108(PrimRegistrar add, Evaluator& ev) {
    // Issue #883: query:test-bundle-migration-stats
    add("query:test-bundle-migration-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total =
            m ? static_cast<std::int64_t>(m->test_bundle_mig_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hits = m ? static_cast<std::int64_t>(m->test_bundle_mig_hits_total.load(
                                          std::memory_order_relaxed))
                                    : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->test_bundle_mig_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 883);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 109 (orig lines 21189-21240)
void ObservabilityPrims::register_jit_p109(PrimRegistrar add, Evaluator& ev) {
    // Issue #884: query:test-profile-flag-stats
    add("query:test-profile-flag-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total = m ? static_cast<std::int64_t>(m->test_profile_flag_total.load(
                                           std::memory_order_relaxed))
                                     : 0;
        const std::int64_t hits =
            m ? static_cast<std::int64_t>(
                    m->test_profile_flag_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->test_profile_flag_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 884);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 110 (orig lines 21241-21291)
void ObservabilityPrims::register_jit_p110(PrimRegistrar add, Evaluator& ev) {
    // Issue #885: query:test-harness-module-stats
    add("query:test-harness-module-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total =
            m ? static_cast<std::int64_t>(m->test_harness_mod_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hits = m ? static_cast<std::int64_t>(m->test_harness_mod_hits_total.load(
                                          std::memory_order_relaxed))
                                    : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->test_harness_mod_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 885);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 111 (orig lines 21292-21342)
void ObservabilityPrims::register_jit_p111(PrimRegistrar add, Evaluator& ev) {
    // Issue #886: query:test-json-report-stats
    add("query:test-json-report-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total =
            m ? static_cast<std::int64_t>(m->test_json_report_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hits = m ? static_cast<std::int64_t>(m->test_json_report_hits_total.load(
                                          std::memory_order_relaxed))
                                    : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->test_json_report_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 886);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

} // namespace aura::compiler::primitives_detail
