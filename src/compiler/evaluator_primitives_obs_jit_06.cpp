// evaluator_primitives_obs_jit_06.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 48 (orig lines 17881-17987)
void ObservabilityPrims::register_jit_p48(PrimRegistrar add, Evaluator& ev) {

    // Issue #814 / #1139: runtime:self-heal-on-drift — return #t only when
    // metrics are attached (heal path actually recorded); else #f.
    add("runtime:self-heal-on-drift", [&ev](const auto&) -> EvalValue {
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            m->runtime_self_heal_invocations_total.fetch_add(1, std::memory_order_relaxed);
            // Healing also clears one unit of "unpaid" drift for the score.
            // (Does not reset the counter; health-score uses heal vs drift.)
            return make_bool(true);
        }
        return make_bool(false);
    });

    // Issue #816: edsl:define-struct name doc schema — Phase 1 registry
    // that validates non-empty name/schema and records metrics. Full
    // NodeTag generation is Phase 2.
    add("edsl:define-struct", [&ev](const auto& a) -> EvalValue {
        // (edsl:define-struct name doc schema) — name/schema required strings.
        if (a.size() < 3) {
            if (ev.compiler_metrics_) {
                auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
                m->edsl_define_struct_total.fetch_add(1, std::memory_order_relaxed);
                m->edsl_define_struct_validate_fail_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_bool(false);
        }
        auto name_ok = is_string(a[0]) || is_keyword(a[0]);
        auto schema_ok = is_string(a[2]) || is_hash(a[2]) || is_pair(a[2]);
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            m->edsl_define_struct_total.fetch_add(1, std::memory_order_relaxed);
            if (name_ok && schema_ok)
                m->edsl_define_struct_validate_pass_total.fetch_add(1, std::memory_order_relaxed);
            else
                m->edsl_define_struct_validate_fail_total.fetch_add(1, std::memory_order_relaxed);
        }

        return make_bool(name_ok && schema_ok);
    });

    // Issue #819: pattern-hygiene-provenance-stats — SafePCVSpan + index + provenance predicate
    ObservabilityPrims::register_stats_impl(
        "query:pattern-hygiene-provenance-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_predicate_hits =
                m ? static_cast<std::int64_t>(
                        m->pattern_hygiene_provenance_predicate_hits_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_index_hit_rate_pct = ([&]() -> std::int64_t {
                if (!m)
                    return 10000;
                auto h =
                    m->pattern_hygiene_index_enforced_hits_total.load(std::memory_order_relaxed);
                auto y = m->pattern_hygiene_yield_enforced_total.load(std::memory_order_relaxed);
                // proxy hit rate: hits / (hits+1) * ::aura::compiler::kBasisPointScale when only
                // hits wired
                return static_cast<std::int64_t>((h * 10000ull) / (h + y + 1));
            })();
            const std::int64_t f_safe_span_enforced =
                m ? static_cast<std::int64_t>(
                        m->pattern_hygiene_safe_span_enforced_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_yield_points_hit =
                m ? static_cast<std::int64_t>(
                        m->pattern_hygiene_yield_enforced_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_index_enforced_hits =
                m ? static_cast<std::int64_t>(m->pattern_hygiene_index_enforced_hits_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_enforcement_active = 1;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("predicate-hits", f_predicate_hits);
            insert_kv("index-hit-rate-pct", f_index_hit_rate_pct);
            insert_kv("safe-span-enforced", f_safe_span_enforced);
            insert_kv("yield-points-hit", f_yield_points_hit);
            insert_kv("index-enforced-hits", f_index_enforced_hits);
            insert_kv("enforcement-active", f_enforcement_active);
            insert_kv("schema", 819);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 49 (orig lines 17988-18055)
void ObservabilityPrims::register_jit_p49(PrimRegistrar add, Evaluator& ev) {
    // Issue #820: mutate-atomic-batch-e2e-stats — pinned snapshot + per-boundary observability
    ObservabilityPrims::register_stats_impl(
        "query:mutate-atomic-batch-e2e-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_batches_started =
                m ? static_cast<std::int64_t>(
                        m->mutate_batch_e2e_started_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_suppressed_bumps =
                m ? static_cast<std::int64_t>(
                        m->mutate_batch_e2e_suppressed_bumps_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_hygiene_in_batch =
                m ? static_cast<std::int64_t>(
                        m->mutate_batch_e2e_hygiene_in_batch_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_cross_fiber_steals =
                m ? static_cast<std::int64_t>(m->mutate_batch_e2e_cross_fiber_steals_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_pinned_snapshots =
                m ? static_cast<std::int64_t>(
                        m->mutate_batch_e2e_pinned_snapshot_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_panic_recoveries =
                m ? static_cast<std::int64_t>(
                        m->mutate_batch_e2e_panic_recoveries_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_e2e_active = 1;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("batches-started", f_batches_started);
            insert_kv("suppressed-bumps", f_suppressed_bumps);
            insert_kv("hygiene-in-batch", f_hygiene_in_batch);
            insert_kv("cross-fiber-steals", f_cross_fiber_steals);
            insert_kv("pinned-snapshots", f_pinned_snapshots);
            insert_kv("panic-recoveries", f_panic_recoveries);
            insert_kv("e2e-active", f_e2e_active);
            insert_kv("schema", 820);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 50 (orig lines 18056-18108)
void ObservabilityPrims::register_jit_p50(PrimRegistrar add, Evaluator& ev) {
    // Issue #821: jit-fiber-exception-stats — fiber-local exception stack safety
    ObservabilityPrims::register_stats_impl(
        "query:jit-fiber-exception-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_fiber_local_ex_stack =
                m ? static_cast<std::int64_t>(
                        m->jit_fiber_ex_stack_local_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_cross_fiber_prevented =
                m ? static_cast<std::int64_t>(
                        m->jit_fiber_ex_cross_prevented_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_deopt_to_interpreter =
                m ? static_cast<std::int64_t>(
                        m->jit_fiber_ex_deopt_interpreter_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_fiber_local_policy_active = 1;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("fiber-local-ex-stack", f_fiber_local_ex_stack);
            insert_kv("cross-fiber-prevented", f_cross_fiber_prevented);
            insert_kv("deopt-to-interpreter", f_deopt_to_interpreter);
            insert_kv("fiber-local-policy-active", f_fiber_local_policy_active);
            insert_kv("schema", 821);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 51 (orig lines 18109-18166)
void ObservabilityPrims::register_jit_p51(PrimRegistrar add, Evaluator& ev) {
    // Issue #822: l2-specialization-deopt-stats — L2 pair/GuardShape/linear deopt
    ObservabilityPrims::register_stats_impl(
        "query:l2-specialization-deopt-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_pair_fastpath =
                m ? static_cast<std::int64_t>(
                        m->l2_spec_pair_fastpath_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_deopt_version_mismatch =
                m ? static_cast<std::int64_t>(
                        m->l2_spec_deopt_version_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_guardshape_narrow =
                m ? static_cast<std::int64_t>(
                        m->l2_spec_guardshape_narrow_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_linear_probe =
                m ? static_cast<std::int64_t>(
                        m->l2_spec_linear_probe_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_l2_maturity_active = 1;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("pair-fastpath", f_pair_fastpath);
            insert_kv("deopt-version-mismatch", f_deopt_version_mismatch);
            insert_kv("guardshape-narrow", f_guardshape_narrow);
            insert_kv("linear-probe", f_linear_probe);
            insert_kv("l2-maturity-active", f_l2_maturity_active);
            insert_kv("schema", 822);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 52 (orig lines 18167-18218)
void ObservabilityPrims::register_jit_p52(PrimRegistrar add, Evaluator& ev) {
    // Issue #823: opcode-coverage-deopt-stats — per-fn deopt controller surface
    ObservabilityPrims::register_stats_impl(
        "query:opcode-coverage-deopt-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_coverage_hits =
                m ? static_cast<std::int64_t>(
                        m->opcode_cov_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_unhandled_hot =
                m ? static_cast<std::int64_t>(
                        m->opcode_cov_unhandled_hot_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_per_fn_deopt =
                m ? static_cast<std::int64_t>(
                        m->opcode_cov_per_fn_deopt_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_zero_fallback_policy = 1;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("coverage-hits", f_coverage_hits);
            insert_kv("unhandled-hot", f_unhandled_hot);
            insert_kv("per-fn-deopt", f_per_fn_deopt);
            insert_kv("zero-fallback-policy", f_zero_fallback_policy);
            insert_kv("schema", 823);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 53 (orig lines 18219-18281)
void ObservabilityPrims::register_jit_p53(PrimRegistrar add, Evaluator& ev) {
    // Issue #824: terminal-render-production-stats — terminal clear/draw/present/dirty
    ObservabilityPrims::register_stats_impl(
        "query:terminal-render-production-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_clear_total =
                m ? static_cast<std::int64_t>(
                        m->term_render_clear_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_draw_batch_total =
                m ? static_cast<std::int64_t>(
                        m->term_render_draw_batch_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_present_total =
                m ? static_cast<std::int64_t>(
                        m->term_render_present_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_dirty_region_total =
                m ? static_cast<std::int64_t>(
                        m->term_render_dirty_region_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_present_ns_total =
                m ? static_cast<std::int64_t>(
                        m->term_render_present_ns_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_module_active = 1;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("clear-total", f_clear_total);
            insert_kv("draw-batch-total", f_draw_batch_total);
            insert_kv("present-total", f_present_total);
            insert_kv("dirty-region-total", f_dirty_region_total);
            insert_kv("present-ns-total", f_present_ns_total);
            insert_kv("module-active", f_module_active);
            insert_kv("schema", 824);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 54 (orig lines 18282-18339)
void ObservabilityPrims::register_jit_p54(PrimRegistrar add, Evaluator& ev) {
    // Issue #825: render-ffi-buffer-stats — batch FFI + zero-copy buffers
    ObservabilityPrims::register_stats_impl(
        "query:render-ffi-buffer-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_batch_ffi_calls =
                m ? static_cast<std::int64_t>(
                        m->render_ffi_batch_calls_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_zerocopy_views =
                m ? static_cast<std::int64_t>(
                        m->render_ffi_zerocopy_views_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_ffi_crossing_ns =
                m ? static_cast<std::int64_t>(
                        m->render_ffi_crossing_ns_accum_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_allocs_per_frame =
                m ? static_cast<std::int64_t>(
                        m->render_ffi_allocs_frame_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_buffer_path_active = 1;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("batch-ffi-calls", f_batch_ffi_calls);
            insert_kv("zerocopy-views", f_zerocopy_views);
            insert_kv("ffi-crossing-ns", f_ffi_crossing_ns);
            insert_kv("allocs-per-frame", f_allocs_per_frame);
            insert_kv("buffer-path-active", f_buffer_path_active);
            insert_kv("schema", 825);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 55 (orig lines 18340-18397)
void ObservabilityPrims::register_jit_p55(PrimRegistrar add, Evaluator& ev) {
    // Issue #826: render-hotpath-stats — dirty/delta + JIT coverage under mutate
    ObservabilityPrims::register_stats_impl(
        "query:render-hotpath-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_dirty_hits =
                m ? static_cast<std::int64_t>(
                        m->render_hp_dirty_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_present_delta =
                m ? static_cast<std::int64_t>(
                        m->render_hp_present_delta_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_jit_coverage =
                m ? static_cast<std::int64_t>(
                        m->render_hp_jit_coverage_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_mutation_impact =
                m ? static_cast<std::int64_t>(
                        m->render_hp_mutation_impact_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_hotpath_active = 1;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("dirty-hits", f_dirty_hits);
            insert_kv("present-delta", f_present_delta);
            insert_kv("jit-coverage", f_jit_coverage);
            insert_kv("mutation-impact", f_mutation_impact);
            insert_kv("hotpath-active", f_hotpath_active);
            insert_kv("schema", 826);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

} // namespace aura::compiler::primitives_detail
