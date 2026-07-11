// evaluator_primitives_obs_jit_07.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 56 (orig lines 18398-18450)
void ObservabilityPrims::register_jit_p56(PrimRegistrar add, Evaluator& ev) {
    // Issue #827: shape-value-hotpath-contracts-stats — consteval dispatch + contracts
    add("query:shape-value-hotpath-contracts-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t f_contract_checks_hotpath =
            m ? static_cast<std::int64_t>(
                    m->sv_contract_hotpath_checks_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t f_consteval_dispatch_hits =
            m ? static_cast<std::int64_t>(
                    m->sv_consteval_dispatch_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t f_stability_transitions =
            m ? static_cast<std::int64_t>(
                    m->sv_stability_transitions_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t f_contracts_active = 1;
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
        insert_kv("contract-checks-hotpath", f_contract_checks_hotpath);
        insert_kv("consteval-dispatch-hits", f_consteval_dispatch_hits);
        insert_kv("stability-transitions", f_stability_transitions);
        insert_kv("contracts-active", f_contracts_active);
        insert_kv("schema", 827);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 57 (orig lines 18451-18508)
void ObservabilityPrims::register_jit_p57(PrimRegistrar add, Evaluator& ev) {
    // Issue #828: ir-soa-full-enforcement-stats — DirtyAware + DepGraph hybrid + pmr
    add("query:ir-soa-full-enforcement-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t f_dirty_skips =
            m ? static_cast<std::int64_t>(
                    m->irsoa_enforce_dirty_skips_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t f_impact_hybrid_skips =
            m ? static_cast<std::int64_t>(
                    m->irsoa_enforce_impact_hybrid_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t f_pmr_util_pct =
            m ? static_cast<std::int64_t>(
                    m->irsoa_enforce_pmr_util_pct.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t f_relower_savings =
            m ? static_cast<std::int64_t>(
                    m->irsoa_enforce_relower_savings_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t f_enforcement_active = 1;
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
        insert_kv("dirty-skips", f_dirty_skips);
        insert_kv("impact-hybrid-skips", f_impact_hybrid_skips);
        insert_kv("pmr-util-pct", f_pmr_util_pct);
        insert_kv("relower-savings", f_relower_savings);
        insert_kv("enforcement-active", f_enforcement_active);
        insert_kv("schema", 828);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 58 (orig lines 18509-18571)
void ObservabilityPrims::register_jit_p58(PrimRegistrar add, Evaluator& ev) {
    // Issue #829: arena-live-defrag-stats — live defrag + fiber yield + fixup
    add("query:arena-live-defrag-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t f_auto_triggers =
            m ? static_cast<std::int64_t>(
                    m->arena_ldefrag_auto_triggers_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t f_live_defrag_savings =
            m ? static_cast<std::int64_t>(
                    m->arena_ldefrag_savings_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t f_fiber_yield_during =
            m ? static_cast<std::int64_t>(
                    m->arena_ldefrag_fiber_yield_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t f_shape_inval =
            m ? static_cast<std::int64_t>(
                    m->arena_ldefrag_shape_inval_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t f_pointer_fixup_hits =
            m ? static_cast<std::int64_t>(
                    m->arena_ldefrag_pointer_fixup_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t f_live_defrag_active = 1;
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
        insert_kv("auto-triggers", f_auto_triggers);
        insert_kv("live-defrag-savings", f_live_defrag_savings);
        insert_kv("fiber-yield-during", f_fiber_yield_during);
        insert_kv("shape-inval", f_shape_inval);
        insert_kv("pointer-fixup-hits", f_pointer_fixup_hits);
        insert_kv("live-defrag-active", f_live_defrag_active);
        insert_kv("schema", 829);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 59 (orig lines 18572-18664)
void ObservabilityPrims::register_jit_p59(PrimRegistrar add, Evaluator& ev) {

    // Issue #824 Phase 1 counters → Issue #1351 Phase A deprecation.
    // These no-ops only bump metrics; real terminal APIs live on make-terminal-buffer /
    // terminal-set-cell* / terminal-present-batch / terminal-diff-update.
    // Phase A: return #f + one-shot stderr warn; keep counters. Phase B: delete later.
    auto deprecate_terminal_noop = [](const char* name, const char* replacement) {
        // Per-name one-shot via address of static storage keyed by name pointer
        // (literals are unique). Thread-safe enough for stderr warn spam control.
        static std::mutex warn_mu;
        static std::unordered_set<const void*> warned;
        std::lock_guard<std::mutex> lock(warn_mu);
        if (warned.insert(static_cast<const void*>(name)).second) {
            std::fprintf(stderr,
                         "[aura] WARN: %s is deprecated (no-op); use %s instead "
                         "(see #1351)\n",
                         name, replacement);
        }
    };

    add("terminal:clear", [&ev, deprecate_terminal_noop](const auto&) -> EvalValue {
        deprecate_terminal_noop("terminal:clear", "make-terminal-buffer + terminal-present-batch");
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            m->term_render_clear_total.fetch_add(1, std::memory_order_relaxed);
        }
        return make_bool(false);
    });
    add("terminal:draw-batch", [&ev, deprecate_terminal_noop](const auto& a) -> EvalValue {
        deprecate_terminal_noop("terminal:draw-batch", "terminal-set-cell / terminal-set-cell-rgb");
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            m->term_render_draw_batch_total.fetch_add(1, std::memory_order_relaxed);
            if (!a.empty() && is_int(a[0]))
                m->term_render_present_ns_total.fetch_add(static_cast<std::uint64_t>(as_int(a[0])),
                                                          std::memory_order_relaxed);
        }
        return make_bool(false);
    });
    add("terminal:present", [&ev, deprecate_terminal_noop](const auto&) -> EvalValue {
        deprecate_terminal_noop("terminal:present", "terminal-present-batch / terminal-present");
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            m->term_render_present_total.fetch_add(1, std::memory_order_relaxed);
        }
        return make_bool(false);
    });
    add("terminal:mark-dirty-region", [&ev, deprecate_terminal_noop](const auto&) -> EvalValue {
        deprecate_terminal_noop("terminal:mark-dirty-region",
                                "terminal-diff-update (real dirty cell count)");
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            m->term_render_dirty_region_total.fetch_add(1, std::memory_order_relaxed);
            m->render_hp_dirty_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
        return make_bool(false);
    });
    // Issue #1135: present-delta only bumps its own delta counter —
    // term_render_present_total is owned solely by terminal:present.
    add("terminal:present-delta", [&ev, deprecate_terminal_noop](const auto&) -> EvalValue {
        deprecate_terminal_noop("terminal:present-delta",
                                "terminal-diff-update + terminal-present-batch");
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            m->render_hp_present_delta_total.fetch_add(1, std::memory_order_relaxed);
        }
        return make_bool(false);
    });
    // Issue #830: query:pass-shape-epoch-stats
    add("query:pass-shape-epoch-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total =
            m ? static_cast<std::int64_t>(m->pass_shape_epoch_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hits = m ? static_cast<std::int64_t>(m->pass_shape_epoch_hits_total.load(
                                          std::memory_order_relaxed))
                                    : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->pass_shape_epoch_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 830);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 60 (orig lines 18665-18716)
void ObservabilityPrims::register_jit_p60(PrimRegistrar add, Evaluator& ev) {
    // Issue #831: query:edsl-hotpath-real-stats
    add("query:edsl-hotpath-real-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total = m ? static_cast<std::int64_t>(m->edsl_hotpath_real_total.load(
                                           std::memory_order_relaxed))
                                     : 0;
        const std::int64_t hits =
            m ? static_cast<std::int64_t>(
                    m->edsl_hotpath_real_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->edsl_hotpath_real_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 831);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 61 (orig lines 18717-18768)
void ObservabilityPrims::register_jit_p61(PrimRegistrar add, Evaluator& ev) {
    // Issue #832: query:dead-coercion-elim-stats
    add("query:dead-coercion-elim-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total = m ? static_cast<std::int64_t>(m->dead_coercion_elim_total.load(
                                           std::memory_order_relaxed))
                                     : 0;
        const std::int64_t hits =
            m ? static_cast<std::int64_t>(
                    m->dead_coercion_elim_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->dead_coercion_elim_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 832);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 62 (orig lines 18769-18820)
void ObservabilityPrims::register_jit_p62(PrimRegistrar add, Evaluator& ev) {
    // Issue #833: query:occurrence-renarrow-stats
    add("query:occurrence-renarrow-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total = m ? static_cast<std::int64_t>(m->occurrence_renarrow_total.load(
                                           std::memory_order_relaxed))
                                     : 0;
        const std::int64_t hits =
            m ? static_cast<std::int64_t>(
                    m->occurrence_renarrow_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->occurrence_renarrow_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 833);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 63 (orig lines 18821-18872)
void ObservabilityPrims::register_jit_p63(PrimRegistrar add, Evaluator& ev) {
    // Issue #834: query:linear-escape-mutate-stats
    add("query:linear-escape-mutate-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics_ ? static_cast<CompilerMetrics*>(ev.compiler_metrics_) : nullptr;
        const std::int64_t total = m ? static_cast<std::int64_t>(m->linear_escape_mutate_total.load(
                                           std::memory_order_relaxed))
                                     : 0;
        const std::int64_t hits =
            m ? static_cast<std::int64_t>(
                    m->linear_escape_mutate_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t savings =
            m ? static_cast<std::int64_t>(
                    m->linear_escape_mutate_savings_total.load(std::memory_order_relaxed))
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
        insert_kv("schema", 834);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

} // namespace aura::compiler::primitives_detail
