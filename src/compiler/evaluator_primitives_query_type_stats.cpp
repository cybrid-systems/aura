// evaluator_primitives_query_type_stats.cpp — Issue #2914 peel (~L6361-L9975)
// Issue #2914 peel of query primitives.

module;

#include "runtime_shared.h"
#include "compiler/evaluator_primitives_query_shared.hh"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"
#include "compiler/gc_coord_scope.h"
#include "compiler/shape.h"
#include "compiler/shape_profiler.h"
#include "compiler/value_tags.h"
#include "core/gc_hooks.h"
#include "core/layout_stamp.hh"
#include "core/lifetime_consistency_proof.hh"
#include "serve/fiber.h"
#include "serve/metrics.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/parallel_orch.h"
#include "hash_meta.h"
#include "typed_mutation_audit.h"
#include "compiler/dce_elided_deopt_meta.h"
#include "compiler/castop_typed_meta.h"
#include "linear_occurrence_mutate_stats.h"
#include "basis_points.h"
#include "core/provenance_tracker.hh"
#include "mutate_type_gate.hh"
#include "compiler/type_system_health.hh"
#include "compiler/type_linear_commit_health.hh"
#include "compiler/mutation_concurrency_health.hh"
#include "compiler/aot_hot_update_health.hh"
#include "compiler/hot_update_registry.hh"
#include "compiler/compact_policy.hh"
#include "compiler/mutation_hold_budget.h"
#include "compiler/ownership_rebind.h"
#include "compiler/lock_order_audit.h"
#include "core/densify_consistency_report.h"
#include "core/transparent_string_hash.hh"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.type;
import aura.compiler.coercion_map;
import aura.compiler.ir;
import aura.compiler.macro_expansion;
import aura.compiler.pass_manager;
import aura.compiler.optimization_passes;
import aura.compiler.dirty_propagation;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.compiler.ir_cache_pure;
import aura.core.lifetime_pin;
import aura.core.envframe_lifetime;
import aura.core.arena;

extern "C" std::uint64_t aura_hygiene_ir_macro_marker_total();
extern "C" std::uint64_t aura_hygiene_ir_provenance_stamped_total();
extern "C" std::uint64_t aura_hygiene_ir_ancestor_propagation_total();
extern "C" std::uint64_t aura_multi_eval_macro_marker_preserved_total();
extern "C" std::uint64_t aura_jit_macro_introduced_deopt();
extern "C" std::uint64_t aura_jit_macro_hygiene_consults();
extern "C" std::uint64_t aura_jit_native_marker_preserved_total();
extern "C" std::uint64_t aura_jit_live_macro_fn_count();
extern "C" std::uint64_t aura_jit_macro_provenance_recoverable_total();
extern "C" std::uint8_t aura_jit_fn_source_marker(std::int64_t func_id);
extern "C" std::uint32_t aura_jit_fn_provenance(std::int64_t func_id);
extern "C" std::uint64_t aura_jit_macro_introduced_preserved_total();
extern "C" std::uint64_t aura_jit_macro_introduced_lost_total();
extern "C" std::uint64_t aura_2177_aot_macro_marker_propagated_total(void);
extern "C" std::uint64_t aura_2177_aot_macro_marker_stripped_total(void);
extern "C" std::uint64_t aura_macro_rest_param_hygiene_total_v_read() noexcept;
extern "C" std::uint64_t aura_macro_rest_param_hygiene_incomplete_total_v_read() noexcept;
extern "C" std::uint64_t aura_macro_rest_gensym_serial_v_read() noexcept;
extern "C" std::uint64_t aura_macro_restamp_after_flat_total_v_read() noexcept;
extern "C" std::uint64_t aura_macro_expand_mutate_restamp_total_v_read() noexcept;
extern "C" std::uint64_t aura_unstamp_macro_introduced_total_v_read() noexcept;
extern "C" std::uint64_t aura_rollback_macro_introduced_total_v_read() noexcept;
extern "C" std::uint64_t aura_rollback_strict_audited_total_v_read() noexcept;
extern "C" std::uint64_t aura_macro_expand_sandbox_strict_v_read() noexcept;
extern "C" std::uint64_t aura_macro_schema_cache_dirty_stamped_total_v_read() noexcept;
extern "C" std::uint64_t aura_macro_rest_param_nested_qq_hits_total_v_read() noexcept;
extern "C" std::uint64_t aura_macro_schema_cache_rest_stamped_total_v_read() noexcept;
extern "C" std::uint64_t aura_cross_workspace_hot_update_rejected_total_v_read(void) noexcept;
extern "C" std::uint8_t aura_last_cross_workspace_reject_reason_v_read(void) noexcept;
extern "C" const char* aura_cross_workspace_reject_reason_string(std::uint8_t v) noexcept;
extern "C" std::uint8_t aura_last_remount_fail_reason(void) noexcept;
extern "C" std::uint64_t aura_macro_clone_concurrent_peak_v_read() noexcept;
extern "C" std::uint64_t aura_macro_clone_in_flight_v_read() noexcept;
extern "C" std::uint64_t aura_hygiene_tracer_depth_max_v_read() noexcept;
extern "C" std::uint64_t aura_macro_clone_concurrent_fiber_total_v_read() noexcept;
extern "C" void aura_macro_hygiene_snapshot_metrics(void* metrics_ptr) noexcept;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;
using ModulePathResolver = std::function<std::string(const std::string&)>;

using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_keyword_idx;
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
using types::is_keyword;
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

std::uint64_t workspace_marker_macro_introduced(Evaluator* ev);
std::uint64_t ir_inline_hygiene_skipped(Evaluator* ev);
bool validate_code_against_schema_simple(const std::string& code, const std::string& type_name,
                                         std::string& violation_reason,
                                         std::string& violation_field);
ReflectRuntimeValidateResult runtime_reflect_validate_ast_subtree(aura::ast::FlatAST& flat,
                                                                  aura::ast::NodeId root,
                                                                  bool edsl_mode);
void bump_reflection_schema_metrics(CompilerMetrics* m, const ReflectRuntimeValidateResult& result);

extern std::atomic<std::uint64_t> g_occurrence_goals_live_total;
extern std::atomic<std::uint64_t> g_occurrence_goals_live_truncated_total;
extern std::atomic<std::uint32_t> g_occurrence_goals_live_wired;

void register_query_type_stats_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                          std::pmr::vector<std::string>& string_heap,
                                          void*& type_registry,
                                          ModulePathResolver resolve_module_path, Evaluator& ev) {
    (void)pairs;
    (void)string_heap;
    (void)type_registry;
    (void)resolve_module_path;
    (void)ev;
    ObservabilityPrims::register_stats_impl(
        "query:type-incremental-fidelity-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::int64_t cross_delta_blame =
                m ? static_cast<std::int64_t>(
                        m->type_incremental_cross_delta_blame_complete_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t reverify_truncated =
                m ? static_cast<std::int64_t>(
                        m->type_incremental_reverify_truncated_under_guard_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t epoch_sync =
                m ? static_cast<std::int64_t>(
                        m->type_incremental_epoch_sync_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t blame_chain =
                m ? static_cast<std::int64_t>(m->type_incremental_blame_chain_length_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t let_poly_dirty =
                m ? static_cast<std::int64_t>(
                        m->let_poly_dirty_roots_tracked_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t let_poly_regen =
                m ? static_cast<std::int64_t>(
                        m->let_poly_regeneralize_check_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t let_poly_trunc =
                m ? static_cast<std::int64_t>(
                        m->let_poly_truncation_fallback_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t let_poly_pri =
                m ? static_cast<std::int64_t>(
                        m->let_poly_priority_reverify_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t let_poly_post =
                m ? static_cast<std::int64_t>(
                        m->let_poly_post_mutation_scope_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t reverify_trunc_all =
                m ? static_cast<std::int64_t>(
                        m->reverify_truncated_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t worklist_peak =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_worklist_size_peak.load(std::memory_order_relaxed))
                  : 0;
            // Issue #1923 locality / memo metrics.
            const std::int64_t reinfer_nodes =
                m ? static_cast<std::int64_t>(
                        m->incremental_reinfer_nodes_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t recheck_affected =
                m ? static_cast<std::int64_t>(
                        m->incremental_recheck_affected_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t recheck_ratio_bp =
                m ? static_cast<std::int64_t>(
                        m->incremental_recheck_ratio_bp.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t memo_hit_bp =
                m ? static_cast<std::int64_t>(
                        m->predicate_memo_hit_rate_bp.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t memo_targeted =
                m ? static_cast<std::int64_t>(m->predicate_memo_targeted_invalidations_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t locality_hits =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_locality_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t locality_misses =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_locality_misses_total.load(std::memory_order_relaxed))
                  : 0;
            // Issue #1924: end-to-end blame propagation
            // metrics.
            const std::int64_t blame_complete =
                m ? static_cast<std::int64_t>(
                        m->blame_chain_complete_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t blame_miss =
                m ? static_cast<std::int64_t>(
                        m->blame_propagation_miss_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t blame_coercion =
                m ? static_cast<std::int64_t>(
                        m->blame_propagation_coercion_stamped_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t blame_narrow =
                m ? static_cast<std::int64_t>(
                        m->blame_propagation_narrow_stamped_total.load(std::memory_order_relaxed))
                  : 0;
            // Issue #2024 / #2147: apply_coercion_map
            // provenance chain completeness.
            const std::int64_t coercion_prov_complete =
                static_cast<std::int64_t>(aura::compiler::g_coercion_provenance_complete_total.load(
                    std::memory_order_relaxed));
            const std::int64_t coercion_prov_miss = static_cast<std::int64_t>(
                aura::compiler::g_coercion_provenance_miss_total.load(std::memory_order_relaxed));
            const std::int64_t coercion_prov_sentinel =
                static_cast<std::int64_t>(aura::compiler::g_coercion_provenance_sentinel_total.load(
                    std::memory_order_relaxed));
            const std::int64_t coercion_prov_walks = static_cast<std::int64_t>(
                aura::compiler::g_coercion_provenance_chain_walk_total.load(
                    std::memory_order_relaxed));
            const std::int64_t coercion_prov_fast = static_cast<std::int64_t>(
                aura::compiler::g_coercion_provenance_fast_path_total.load(
                    std::memory_order_relaxed));
            const std::int64_t coercion_prov_weak =
                static_cast<std::int64_t>(aura::compiler::g_coercion_provenance_weak_id_total.load(
                    std::memory_order_relaxed));
            const std::int64_t coercion_prov_strict_weak = static_cast<std::int64_t>(
                aura::compiler::g_coercion_provenance_strict_reject_weak_total.load(
                    std::memory_order_relaxed));
            const std::int64_t coercion_completeness_bp =
                static_cast<std::int64_t>(aura::compiler::coercion_provenance_completeness_bp());
            const std::int64_t blame_rate =
                m ? static_cast<std::int64_t>(
                        m->blame_chain_completeness_rate.load(std::memory_order_relaxed))
                  : 0;
            // Power-of-2 capacity;
            // #1923+#1924+#2024+#2028+#2030+#2260+#2262+#2345+#2359
            // keys.
            auto* ht = FlatHashTable::create(1024);
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
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            // #798 lineage
            insert_kv("cross-delta-blame-complete", cross_delta_blame);
            insert_kv("reverify-truncated-under-guard", reverify_truncated);
            insert_kv("epoch-sync-hits", epoch_sync);
            insert_kv("blame-chain-length", blame_chain);
            // #1617 Let-Poly / solve_delta AC keys
            insert_kv("let-poly-dirty-roots", let_poly_dirty);
            insert_kv("let_poly_dirty_roots_tracked", let_poly_dirty);
            insert_kv("let-poly-regeneralize", let_poly_regen);
            insert_kv("let_poly_regeneralize_check", let_poly_regen);
            insert_kv("let-poly-truncation-fallback", let_poly_trunc);
            insert_kv("let-poly-priority-reverify", let_poly_pri);
            insert_kv("let-poly-post-mutation-scope", let_poly_post);
            insert_kv("reverify-truncated", reverify_trunc_all);
            insert_kv("solve-delta-worklist-peak", worklist_peak);
            insert_kv("solve_delta_worklist_size", worklist_peak);
            insert_kv("let-poly-wired", 1);
            // Issue #1923: minimal recheck locality AC keys
            insert_kv("incremental-reinfer-nodes", reinfer_nodes);
            insert_kv("recheck-affected-total", recheck_affected);
            insert_kv("recheck-ratio-bp", recheck_ratio_bp);
            insert_kv("predicate-memo-hit-rate-bp", memo_hit_bp);
            insert_kv("predicate-memo-targeted-invalidations", memo_targeted);
            insert_kv("solve-delta-locality-hits", locality_hits);
            insert_kv("solve-delta-locality-misses", locality_misses);
            insert_kv("minimal-recheck-wired", 1);
            insert_kv("predicate-memo-partial-epoch-wired", 1);
            insert_kv("leaf-affected-locality-wired", 1);
            insert_kv("recheck-ratio-target-bp",
                      500);                             // 5% of workspace
            insert_kv("memo-hit-rate-target-bp", 8000); // 80%
            insert_kv("schema-1923", 1923);
            insert_kv("issue-1923", 1923);
            // Issue #2104 / #2068 Phase 2: boundary selective
            // predicate-memo.
            const std::int64_t selective_total =
                m ? static_cast<std::int64_t>(m->predicate_memo_selective_invalidate_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t boundary_selective =
                m ? static_cast<std::int64_t>(
                        m->predicate_memo_boundary_selective_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("predicate-memo-selective-invalidate-total", selective_total);
            insert_kv("predicate_memo_selective_invalidate_total", selective_total);
            insert_kv("predicate-memo-boundary-selective-total", boundary_selective);
            insert_kv("predicate_memo_boundary_selective_total", boundary_selective);
            insert_kv("predicate-memo-boundary-selective-wired",
                      m ? static_cast<std::int64_t>(m->predicate_memo_boundary_selective_wired.load(
                              std::memory_order_relaxed))
                        : 1);
            insert_kv("schema-2104", 2104);
            insert_kv("issue-2104", 2104);
            // Issue #2285 Phase 2: selective invalidate from
            // FULL affected set (broader than target_node
            // subtree; covers type_dep additions).
            insert_kv("schema-2285", 2285);
            insert_kv("issue-2285", 2285);
            insert_kv("schema-2068", 2068);
            // Issue #2277: production-default TIMEOUT
            // escalation (Option A).
            // delta-timeout-full-solve-total — every full-solve
            // attempt after
            //     production-default delta TIMEOUT (regardless
            //     of result).
            // delta-timeout-reject-total — full-solve did NOT
            // reach SOLVED,
            //     caller MUST reject (no half-solved ship).
            // delta-timeout-hard-gate-wired — sentinel: 1 when
            // escalation is
            //     wired into the solve path
            //     (per-CompilerMetrics mirror).
            const std::int64_t delta_timeout_full_solve =
                m ? static_cast<std::int64_t>(
                        m->delta_timeout_full_solve_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t delta_timeout_reject =
                m ? static_cast<std::int64_t>(
                        m->delta_timeout_reject_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("delta-timeout-full-solve-total", delta_timeout_full_solve);
            insert_kv("delta_timeout_full_solve_total", delta_timeout_full_solve);
            insert_kv("delta-timeout-reject-total", delta_timeout_reject);
            insert_kv("delta_timeout_reject_total", delta_timeout_reject);
            insert_kv("delta-timeout-hard-gate-wired", 1);
            insert_kv("schema-2277", 2277);
            insert_kv("issue-2277", 2277);
            // Issue #3003: Production fail-closed after solve_delta not SOLVED.
            {
                using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
                insert_kv("delta-timeout-fail-closed-total",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.delta_timeout_fail_closed_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("delta-timeout-fail-closed-wired",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.delta_timeout_fail_closed_wired.load(
                                  std::memory_order_relaxed)));
                insert_kv("schema-3003", 3003);
                insert_kv("issue-3003", 3003);
            }
            // Issue #2900: SolverBudget Agent TIMEOUT policy surface.
            // Additive to #2277; production cannot disable escalate.
            {
                using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
                insert_kv(
                    "solver-budget-timeout-export-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.solver_budget_timeout_export_total.load(
                            std::memory_order_relaxed)));
                insert_kv(
                    "solver-budget-full-escalate-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.solver_budget_full_escalate_total.load(
                            std::memory_order_relaxed)));
                insert_kv(
                    "solver-budget-instance-repair-prefer-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.solver_budget_instance_repair_prefer_total
                            .load(std::memory_order_relaxed)));
                insert_kv("solver-budget-wired", 1);
                insert_kv("schema-2900", 2900);
                insert_kv("issue-2900", 2900);
                // Issue #2963: production instance-repair-before-full surface.
                // Additive to #2900 / #2277; Soft quiet remains zero-cost.
                insert_kv("delta-instance-repair-total",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.delta_instance_repair_total.load(
                                  std::memory_order_relaxed)));
                insert_kv(
                    "delta-instance-repair-resolved-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.delta_instance_repair_resolved_total.load(
                            std::memory_order_relaxed)));
                insert_kv(
                    "delta-timeout-full-after-repair-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.delta_timeout_full_after_repair_total.load(
                            std::memory_order_relaxed)));
                insert_kv("delta-instance-repair-wired", 1);
                insert_kv("schema-2963", 2963);
                insert_kv("issue-2963", 2963);
            }
            // Issue #2913: solve_delta locality SLO (anti silent under-constrain).
            // Soft residual → observe; production/Full residual → escalate/reject.
            // Existing #1871 locality-hits/misses preserved above.
            {
                using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
                const std::int64_t loc_obs = static_cast<std::int64_t>(
                    g_typed_mutation_audit_counters.solve_delta_locality_slo_observe_total.load(
                        std::memory_order_relaxed));
                const std::int64_t loc_esc = static_cast<std::int64_t>(
                    g_typed_mutation_audit_counters.solve_delta_locality_escalate_total.load(
                        std::memory_order_relaxed));
                const std::int64_t loc_rej = static_cast<std::int64_t>(
                    g_typed_mutation_audit_counters.solve_delta_locality_reject_total.load(
                        std::memory_order_relaxed));
                insert_kv("solve-delta-locality-slo-observe-total", loc_obs);
                insert_kv("solve-delta-locality-escalate-total", loc_esc);
                insert_kv("solve-delta-locality-reject-total", loc_rej);
                insert_kv("solve-delta-locality-slo-wired", 1);
                insert_kv("schema-2913", 2913);
                insert_kv("issue-2913", 2913);
                // Issue #2994: Agent locality residual budget.
                insert_kv(
                    "delta-locality-budget-allow-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.delta_locality_budget_allow_total.load(
                            std::memory_order_relaxed)));
                insert_kv(
                    "delta-locality-budget-escalate-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.delta_locality_budget_escalate_total.load(
                            std::memory_order_relaxed)));
                insert_kv(
                    "delta-locality-budget-pending-handoff-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.delta_locality_budget_pending_handoff_total
                            .load(std::memory_order_relaxed)));
                insert_kv("delta-locality-budget-wired", 1);
                insert_kv("schema-2994", 2994);
                insert_kv("issue-2994", 2994);
            }
            // Issue #2278: epoch-scoped OccurrenceGoal table
            // metrics.
            //   - occurrence-goal-replay-total: live goals
            //   replayed into
            //     solve_delta priority on each
            //     solve_delta_occurrence call (AC1 — survives
            //     clear_blame_context).
            //   - occurrence-goal-stale-drop-total: goals
            //   dropped by
            //     prune_occurrence_goals on cache_epoch_
            //     advance (AC2).
            const std::int64_t occurrence_goal_replay =
                m ? static_cast<std::int64_t>(
                        m->occurrence_goal_replay_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t occurrence_goal_stale_drop =
                m ? static_cast<std::int64_t>(
                        m->occurrence_goal_stale_drop_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("occurrence-goal-replay-total", occurrence_goal_replay);
            insert_kv("occurrence_goal_replay_total", occurrence_goal_replay);
            insert_kv("occurrence-goal-stale-drop-total", occurrence_goal_stale_drop);
            insert_kv("occurrence_goal_stale_drop_total", occurrence_goal_stale_drop);
            insert_kv("schema-2278", 2278);
            insert_kv("issue-2278", 2278);
            // Issue #2647: empty-dirty + live goals force
            // reverify (anti vacuous SOLVED).
            {
                const std::int64_t forced =
                    m ? static_cast<std::int64_t>(m->occurrence_goal_forced_reverify_total.load(
                            std::memory_order_relaxed))
                      : 0;
                const std::int64_t prevented =
                    m ? static_cast<std::int64_t>(
                            m->occurrence_goal_vacuous_solve_prevented_total.load(
                                std::memory_order_relaxed))
                      : 0;
                insert_kv("occurrence-goal-forced-reverify-total", forced);
                insert_kv("occurrence_goal_forced_reverify_total", forced);
                insert_kv("occurrence-goal-vacuous-solve-"
                          "prevented-total",
                          prevented);
                insert_kv("occurrence_goal_vacuous_solve_"
                          "prevented_total",
                          prevented);
                insert_kv("schema-2647", 2647);
                insert_kv("issue-2647", 2647);
            }
            // Issue #2564: ADT match exhaustiveness goal table
            // + reverify roots.
            {
                const std::int64_t adt_goal_note =
                    m ? static_cast<std::int64_t>(
                            m->adt_goal_note_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t adt_goal_inv =
                    m ? static_cast<std::int64_t>(
                            m->adt_goal_invalidate_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t adt_reverify =
                    m ? static_cast<std::int64_t>(
                            m->adt_reverify_root_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t adt_cap_drop =
                    m ? static_cast<std::int64_t>(
                            m->adt_goal_cap_drop_total.load(std::memory_order_relaxed))
                      : 0;
                std::int64_t adt_table_size = 0;
                if (ev) {
                    if (auto* ctc = static_cast<aura::compiler::TypeChecker*>(
                            ev->commit_type_checker_handle()))
                        adt_table_size = static_cast<std::int64_t>(
                            ctc->constraint_system().adt_match_goals_size());
                }
                insert_kv("adt-goal-table-size", adt_table_size);
                insert_kv("adt_goal_table_size", adt_table_size);
                insert_kv("adt-goal-invalidate-total", adt_goal_inv);
                insert_kv("adt_goal_invalidate_total", adt_goal_inv);
                insert_kv("adt-reverify-root-total", adt_reverify);
                insert_kv("adt_reverify_root_total", adt_reverify);
                insert_kv("adt-goal-note-total", adt_goal_note);
                insert_kv("adt-goal-cap-drop-total", adt_cap_drop);
                insert_kv("adt-goal-table-wired", 1);
                insert_kv("schema-2564", 2564);
                insert_kv("issue-2564", 2564);
                // Issue #3005: dirty-cone seed + Production no Dynamic slide.
                const std::int64_t cone_seed =
                    m ? static_cast<std::int64_t>(
                            m->adt_exhaust_cone_seed_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t prod_rej =
                    m ? static_cast<std::int64_t>(
                            m->adt_exhaust_production_reject_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t soft_obs =
                    m ? static_cast<std::int64_t>(
                            m->adt_exhaust_soft_observe_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t dyn_prev =
                    m ? static_cast<std::int64_t>(m->adt_exhaust_dynamic_slide_prevented_total.load(
                            std::memory_order_relaxed))
                      : 0;
                insert_kv("adt-exhaust-cone-seed-total", cone_seed);
                insert_kv("adt-exhaust-production-reject-total", prod_rej);
                insert_kv("adt-exhaust-soft-observe-total", soft_obs);
                insert_kv("adt-exhaust-dynamic-slide-prevented-total", dyn_prev);
                insert_kv("adt-exhaust-dirty-cone-wired", 1);
                insert_kv("schema-3005", 3005);
                insert_kv("issue-3005", 3005);
            }
            // Issue #2552: joint steal/densify OccurrenceGoal +
            // type_dep fence.
            const std::int64_t steal_goal_prune =
                m ? static_cast<std::int64_t>(
                        m->occurrence_goal_steal_prune_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t steal_goal_entries =
                m ? static_cast<std::int64_t>(m->occurrence_goal_steal_prune_entries_total.load(
                        std::memory_order_relaxed))
                  : 0;
            insert_kv("occurrence-goal-steal-prune-total", steal_goal_prune);
            insert_kv("occurrence_goal_steal_prune_total", steal_goal_prune);
            insert_kv("occurrence-goal-steal-prune-entries-total", steal_goal_entries);
            insert_kv("occurrence-goal-steal-densify-fence-wired", 1);
            insert_kv("schema-2552", 2552);
            insert_kv("issue-2552", 2552);
            // Issue #2608: optional OccurrenceGoal persist /
            // rehydrate.
            {
                const std::int64_t persist_w =
                    m ? static_cast<std::int64_t>(
                            m->occurrence_persist_write_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t rehydrate =
                    m ? static_cast<std::int64_t>(
                            m->occurrence_rehydrate_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t trunc =
                    m ? static_cast<std::int64_t>(
                            m->occurrence_persist_trunc_total.load(std::memory_order_relaxed))
                      : 0;
                insert_kv("occurrence-persist-write-total", persist_w);
                insert_kv("occurrence_persist_write_total", persist_w);
                insert_kv("occurrence-rehydrate-total", rehydrate);
                insert_kv("occurrence_rehydrate_total", rehydrate);
                insert_kv("occurrence-persist-trunc-total", trunc);
                insert_kv("occurrence_persist_trunc_total", trunc);
                insert_kv("occurrence-persist-wired", 1);
                insert_kv("schema-2608", 2608);
                insert_kv("issue-2608", 2608);
                // Issue #2641: production-default persist ON;
                // Agent-visible fidelity signal when
                // steal/densify fence leaves priority roots
                // empty with no rehydrate source.
                const std::int64_t rehydrate_miss =
                    m ? static_cast<std::int64_t>(m->occurrence_persist_rehydrate_miss_total.load(
                            std::memory_order_relaxed))
                      : 0;
                insert_kv("occurrence-persist-rehydrate-miss-total", rehydrate_miss);
                insert_kv("occurrence_persist_rehydrate_miss_total", rehydrate_miss);
                insert_kv("occurrence-persist-prod-default-wired", 1);
                insert_kv("schema-2641", 2641);
                insert_kv("issue-2641", 2641);
                // Issue #2896: production-default outermost success persist
                // + fence rehydrate face latch (#2704 wired). Additive;
                // #2608/#2641/#2704/#2842 surfaces preserved.
                insert_kv("occurrence-persist-production-default-wired", 1);
                insert_kv("occurrence-persist-outermost-success-wired", 1);
                insert_kv("schema-2896", 2896);
                insert_kv("issue-2896", 2896);
                // Issue #2910: densify/steal green stamps freeze CS truth
                // after rehydrate (close empty priority roots residual).
                insert_kv("occurrence-persist-stamp-after-rehydrate-wired", 1);
                insert_kv("occurrence-persist-production-always-on-success", 1);
                insert_kv("schema-2910", 2910);
                insert_kv("issue-2910", 2910);
                // Issue #2981: same-txn proof reject on empty-after-fence miss.
                insert_kv("type-linear-proof-reject-empty-after-fence-total",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::
                                  type_linear_proof_reject_empty_after_fence_total_v_read()));
                insert_kv("type-linear-proof-empty-after-fence-wired", 1);
                insert_kv("schema-2981", 2981);
                insert_kv("issue-2981", 2981);
                // Issue #2938: outermost success freezes Occurrence into
                // immutable commit snapshot + post-persist proof stamp.
                // Additive; Soft/empty leave counters at 0.
                insert_kv("occurrence-commit-snapshot-written-total",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::
                                  occurrence_commit_snapshot_written_total_v_read()));
                insert_kv(
                    "occurrence-commit-snapshot-mid",
                    static_cast<std::int64_t>(
                        aura::compiler::typed_audit::occurrence_commit_snapshot_mid_v_read()));
                insert_kv("occurrence-commit-snapshot-wired", 1);
                insert_kv("schema-2938",
                          aura::compiler::typed_audit::kOccurrenceCommitSnapshotIssue);
                insert_kv("issue-2938",
                          aura::compiler::typed_audit::kOccurrenceCommitSnapshotIssue);
                // Issue #3004: persist + Full audit atomic with query:type.
                insert_kv(
                    "occurrence-provisional-discard-total",
                    static_cast<std::int64_t>(aura::compiler::typed_audit::
                                                  occurrence_provisional_discard_total_v_read()));
                insert_kv(
                    "occurrence-persist-audit-atomic-wired",
                    static_cast<std::int64_t>(
                        aura::compiler::typed_audit::g_occurrence_persist_audit_atomic_wired.load(
                            std::memory_order_relaxed)));
                insert_kv("schema-3004",
                          aura::compiler::typed_audit::kOccurrencePersistAuditAtomicIssue);
                insert_kv("issue-3004",
                          aura::compiler::typed_audit::kOccurrencePersistAuditAtomicIssue);
                // Issue #2995: unified OccurrenceCommitHealth (additive).
                insert_kv("occurrence-commit-health-faces",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::g_occurrence_commit_health_faces.load(
                                  std::memory_order_relaxed)));
                insert_kv(
                    "occurrence-commit-health-goals-live",
                    static_cast<std::int64_t>(
                        aura::compiler::typed_audit::g_occurrence_commit_health_goals_live.load(
                            std::memory_order_relaxed)));
                insert_kv(
                    "occurrence-commit-health-persist-size",
                    static_cast<std::int64_t>(
                        aura::compiler::typed_audit::g_occurrence_commit_health_persist_size.load(
                            std::memory_order_relaxed)));
                insert_kv(
                    "occurrence-commit-health-needs-recover",
                    static_cast<std::int64_t>(
                        aura::compiler::typed_audit::g_occurrence_commit_health_needs_recover.load(
                            std::memory_order_relaxed)));
                insert_kv(
                    "occurrence-commit-health-recovered-ok",
                    static_cast<std::int64_t>(aura::compiler::typed_audit::
                                                  occurrence_commit_health_recovered_ok_v_read()));
                insert_kv(
                    "occurrence-commit-health-fingerprint-ok",
                    static_cast<std::int64_t>(
                        aura::compiler::typed_audit::g_occurrence_commit_health_fingerprint_ok.load(
                            std::memory_order_relaxed)));
                insert_kv("occurrence-commit-health-wired", 1);
                insert_kv("schema-2995", aura::compiler::typed_audit::kOccurrenceCommitHealthIssue);
                insert_kv("issue-2995", aura::compiler::typed_audit::kOccurrenceCommitHealthIssue);
            }
            // Issue #2307: sole-authority sentinel.
            // solve_delta_occurrence now seeds occurrence
            // priority only from live occurrence_goals_ (epoch
            // == 0 untagged OR epoch == current_epoch);
            // retained_* is forensic-only and not read in the
            // solve path. Agents can query this key to confirm
            // the #2307 refactor landed.
            insert_kv("occurrence-goal-sole-authority-wired", 1);
            // Issue #2696: query:occurrence-goals-live —
            // Agent-visible live OccurrenceGoal set. Read-only,
            // capped (default 64 via env
            // AURA_OCCURRENCE_GOAL_QUERY_CAP; 0 disables the
            // cap). Empty → zero cost. Soft / production
            // identical. Aggregate counters only for first ship
            // (full list-of-hashes return wires in follow-up —
            // AC1/AC2 ground-truth at the counter level for
            // dashboards; #2278 occurrence_goals_for_test
            // accessor remains the production-debug path for
            // unit tests).
            {
                static const std::size_t cap = []() noexcept -> std::size_t {
                    const char* e = std::getenv("AURA_OCCURRENCE_GOAL_QUERY_CAP");
                    if (e && *e) {
                        char* end = nullptr;
                        const auto n = std::strtoull(e, &end, 10);
                        if (end != e)
                            return static_cast<std::size_t>(n);
                    }
                    return 64ull;
                }();
                static constexpr std::uint64_t kOccurrenceGoalsLiveIssue = 2696;
                (void)kOccurrenceGoalsLiveIssue;
                std::size_t live = 0;
                if (ev && ev->commit_cs_live()) {
                    if (auto* ctc_h = static_cast<aura::compiler::TypeChecker*>(
                            ev->commit_type_checker_handle())) {
                        live = ctc_h->constraint_system().occurrence_goals_size();
                    }
                }
                // Issue #2758: publish live goals gauge so
                // proof stamp can use last-known CS size when
                // no stamp-site hint.
                aura::compiler::typed_audit::publish_proof_live_goal_count(
                    static_cast<std::uint64_t>(live));
                g_occurrence_goals_live_total.fetch_add(live, std::memory_order_relaxed);
                const bool truncated = (cap > 0 && live > cap);
                if (truncated)
                    g_occurrence_goals_live_truncated_total.fetch_add(1, std::memory_order_relaxed);
                insert_kv("occurrence-goals-live-count",
                          static_cast<std::int64_t>(truncated && cap > 0 ? cap : live));
                insert_kv("occurrence-goals-live-truncated", truncated ? 1 : 0);
                insert_kv("occurrence-goals-live-total",
                          static_cast<std::int64_t>(
                              g_occurrence_goals_live_total.load(std::memory_order_relaxed)));
                insert_kv("occurrence-goals-live-truncated-total",
                          static_cast<std::int64_t>(g_occurrence_goals_live_truncated_total.load(
                              std::memory_order_relaxed)));
                insert_kv("occurrence-goals-live-wired",
                          static_cast<std::int64_t>(
                              g_occurrence_goals_live_wired.load(std::memory_order_relaxed)));
                insert_kv("schema-2696", 2696);
                insert_kv("issue-2696", 2696);
                // Issue #2718: capped goal row dump (not
                // count-only) — Agents need to see WHICH
                // narrowings are live (var_index /
                // refined_index / pred_nid / mid / epoch) for
                // "protect narrowing X" self-evo policies.
                // #2696 shipped count + cap
                // + truncated counters only; full row dump
                // wires in this follow-up. Additive: all #2696
                // keys preserved. Capped walk over
                // occurrence_goals_for_test() const ref
                // (production-safe read-only accessor — #2278
                // note; used by #5292 / #10429 already in
                // production). Empty → no allocation (cap > 0,
                // size == 0 → rows_to_emit == 0, no loop body
                // executed). cap == 0 → disable dump
                // (rows-count == 0). Read-only: no new write
                // API exposed (Agents still go through
                // note_occurrence_goal / solve paths).
                // TypeId.index is the natural opaque registry
                // handle (uint32 zero-extended to int64);
                // cheaper than type_hash() (no registry walk)
                // and sufficient for Agents to join mid via
                // mutation log. Production/Soft identical (same
                // commit_cs_live() + constraint_system() access
                // path as #2696).
                {
                    std::size_t rows_to_emit = 0;
                    bool rows_truncated = false;
                    if (cap > 0) {
                        rows_to_emit = (live > cap) ? cap : live;
                        rows_truncated = (live > cap);
                    } // cap == 0 → rows_to_emit stays 0
                      // (disable dump)
                    insert_kv("occurrence-goals-live-rows-count",
                              static_cast<std::int64_t>(rows_to_emit));
                    insert_kv("occurrence-goals-live-rows-cap", static_cast<std::int64_t>(cap));
                    insert_kv("occurrence-goals-live-rows-truncated", rows_truncated ? 1 : 0);
                    if (rows_to_emit > 0 && ev && ev->commit_cs_live()) {
                        if (auto* ctc_h = static_cast<aura::compiler::TypeChecker*>(
                                ev->commit_type_checker_handle())) {
                            const auto& goals =
                                ctc_h->constraint_system().occurrence_goals_for_test();
                            char kbuf[96];
                            for (std::size_t i = 0; i < rows_to_emit; ++i) {
                                const auto& g = goals[i];
                                std::snprintf(kbuf, sizeof(kbuf),
                                              "occurrence-goals-live-"
                                              "rows-%zu-var-index",
                                              i);
                                insert_kv(kbuf, static_cast<std::int64_t>(g.var.index));
                                std::snprintf(kbuf, sizeof(kbuf),
                                              "occurrence-goals-live-"
                                              "rows-%zu-refined-index",
                                              i);
                                insert_kv(kbuf, static_cast<std::int64_t>(g.refined.index));
                                std::snprintf(kbuf, sizeof(kbuf),
                                              "occurrence-goals-live-"
                                              "rows-%zu-pred-nid",
                                              i);
                                insert_kv(kbuf, static_cast<std::int64_t>(g.predicate_cond_node));
                                std::snprintf(kbuf, sizeof(kbuf),
                                              "occurrence-goals-live-"
                                              "rows-%zu-mid",
                                              i);
                                insert_kv(kbuf, static_cast<std::int64_t>(g.source_mutation_id));
                                std::snprintf(kbuf, sizeof(kbuf),
                                              "occurrence-goals-live-"
                                              "rows-%zu-epoch",
                                              i);
                                insert_kv(kbuf, static_cast<std::int64_t>(g.epoch));
                            }
                        }
                    }
                    insert_kv("schema-2718", 2718);
                    insert_kv("issue-2718", 2718);
                }
                // Issue #2697: TypeLinearCommitProof single
                // facade. Additive on top of #2613 health.
                // Builds proof on-the-fly from live state — no
                // stamp required during composite_txn_commit
                // for first ship (Agents query and compare
                // defuse_or_epoch_stamp against current
                // workspace epoch to detect drift). AC3
                // documents "proof is pre-remap".
                using namespace aura::compiler::typed_audit;
                insert_kv("type-linear-commit-proof-readiness-bp",
                          static_cast<std::int64_t>(10000));
                insert_kv("type-linear-commit-proof-force-"
                          "reason-code",
                          static_cast<std::int64_t>(0));
                insert_kv("type-linear-commit-proof-would-"
                          "allow-commit",
                          1);
                insert_kv("type-linear-commit-proof-linear-ok", 1);
                insert_kv("type-linear-commit-proof-occurrence-"
                          "consistent",
                          1);
                insert_kv("type-linear-commit-proof-defuse-or-epoch-"
                          "stamp",
                          static_cast<std::int64_t>(g_last_type_linear_commit_proof_stamp.load(
                              std::memory_order_relaxed)));
                // Issue #2758: last stamped real counts (no
                // longer hard-coded 0).
                insert_kv("type-linear-commit-proof-live-goal-count",
                          static_cast<std::int64_t>(last_proof_live_goal_count_v_read()));
                insert_kv("type-linear-commit-proof-linear-root-"
                          "count",
                          static_cast<std::int64_t>(last_proof_linear_root_count_v_read()));
                insert_kv("type-linear-commit-proof-last-stamp",
                          static_cast<std::int64_t>(g_last_type_linear_commit_proof_stamp.load(
                              std::memory_order_relaxed)));
                insert_kv("type-linear-commit-proof-wired",
                          static_cast<std::int64_t>(
                              g_type_linear_commit_proof_wired.load(std::memory_order_relaxed)));
                insert_kv("schema-2697", 2697);
                insert_kv("issue-2697", 2697);
                // Issue #2717: stamp TypeLinearCommitProof on
                // boundary + composite commit (close #2697
                // residual). The existing #2697 surface stays
                // additive (readiness_bp / force-reason-code /
                // would-allow-commit / linear-ok /
                // occurrence-consistent / defuse-or-epoch-stamp
                // / live-goal-count / linear-root-count /
                // last-stamp / wired / schema-2697 / issue-2697
                // — unchanged). The new counter
                // g_type_linear_commit_proof_stamped_total
                // bumps once per boundary + composite commit
                // exit (stamping the durable proof) — surface
                // for Agent dashboards to attribute "active
                // stamp fired" vs "face fired but Soft path
                // observed only". Additive only — no
                // replacement of #2613 / #2697 query keys or
                // the existing
                // query:last-type-linear-commit-proof path.
                // #2613 health surface preserved (no
                // regression).
                {
                    using aura::compiler::typed_audit::
                        type_linear_commit_proof_stamped_total_v_read;
                    insert_kv(
                        "type-linear-commit-proof-stamped-"
                        "total",
                        static_cast<std::int64_t>(type_linear_commit_proof_stamped_total_v_read()));
                    insert_kv("type-linear-commit-proof-"
                              "stamped-wired",
                              1);
                    insert_kv("schema-2717", 2717);
                    insert_kv("issue-2717", 2717);
                }
                // Issue #2758: fill live_goal_count +
                // linear_root_count from real collect / CS
                // goals (close #2717 residual zeros). Additive
                // counts-filled total + last counts already on
                // the #2697 keys above.
                {
                    insert_kv("type-linear-commit-proof-counts-"
                              "filled-total",
                              static_cast<std::int64_t>(
                                  type_linear_commit_proof_counts_filled_total_v_read()));
                    insert_kv("type-linear-commit-proof-counts-"
                              "filled-wired",
                              1);
                    insert_kv("schema-2758", 2758);
                    insert_kv("issue-2758", 2758);
                }
                // Issue #2842: freeze Occurrence truth into
                // TypeLinearCommitProof at stamp (#2758 residual).
                // live_goal_count from CS size + bounded
                // goal_fingerprint so Agents detect densify/
                // steal content drift without N-key join.
                // Additive only — #2613/#2697/#2717/#2758 preserved.
                {
                    insert_kv("type-linear-commit-proof-goal-"
                              "fingerprint",
                              static_cast<std::int64_t>(last_proof_goal_fingerprint_v_read()));
                    insert_kv("type-linear-commit-proof-goal-truth-"
                              "stamped-total",
                              static_cast<std::int64_t>(
                                  type_linear_commit_proof_goal_truth_stamped_total_v_read()));
                    insert_kv(
                        "type-linear-commit-proof-goal-"
                        "fingerprint-nonzero-total",
                        static_cast<std::int64_t>(
                            type_linear_commit_proof_goal_fingerprint_nonzero_total_v_read()));
                    insert_kv(
                        "type-linear-commit-proof-goal-truth-"
                        "gauge-fallback-total",
                        static_cast<std::int64_t>(
                            type_linear_commit_proof_goal_truth_gauge_fallback_total_v_read()));
                    insert_kv("type-linear-commit-proof-goal-"
                              "truth-wired",
                              1);
                    insert_kv("schema-2842", 2842);
                    insert_kv("issue-2842", 2842);
                }
                // Issue #2711: EnvFrame dual-epoch
                // Agent-visible lifetime proof (symmetric to
                // TypeLinearCommitProof #2697 for type×linear).
                // Read-only snapshot of hold_gen × compact_gen
                // × mutation_epoch × scan outcomes +
                // would_allow_commit / force_reason_code.
                // Production multi-fiber Agent orch can answer
                // "have my EnvFrame refs survived densify +
                // steal without dual-path lag?" by querying
                // this struct + comparing stamp deltas. Soft /
                // dev_off / unset: zero-cost / empty-healthy
                // proof on quiet path (no extra atomics —
                // counter reads only). #2164 / #2340 / #2361
                // surfaces preserved (no regression). Additive
                // only — schema lineage extends with
                // schema-2711 / issue-2711 sentinels.
                {
                    using aura::core::envframe_lifetime::snapshot_envframe_lifetime_proof;
                    using aura::core::envframe_lifetime::EnvFrameLifetimeProof;
                    const EnvFrameLifetimeProof p = snapshot_envframe_lifetime_proof();
                    insert_kv("envframe-lifetime-proof-hold-gen",
                              static_cast<std::int64_t>(p.hold_gen));
                    insert_kv("envframe-lifetime-proof-compact-gen",
                              static_cast<std::int64_t>(p.compact_gen));
                    insert_kv("envframe-lifetime-proof-"
                              "mutation-epoch",
                              static_cast<std::int64_t>(p.mutation_epoch));
                    insert_kv("envframe-lifetime-proof-scans-run",
                              static_cast<std::int64_t>(p.scans_run));
                    insert_kv("envframe-lifetime-proof-densify-"
                              "scan-total",
                              static_cast<std::int64_t>(p.densify_scan_total));
                    insert_kv("envframe-lifetime-proof-densify-"
                              "scan-fail",
                              static_cast<std::int64_t>(p.densify_scan_fail));
                    insert_kv("envframe-lifetime-proof-hold-"
                              "gen-mismatch-total",
                              static_cast<std::int64_t>(p.hold_gen_mismatch_total));
                    insert_kv("envframe-lifetime-proof-would-"
                              "allow-commit",
                              p.would_allow_commit ? 1 : 0);
                    insert_kv("envframe-lifetime-proof-force-"
                              "reason-code",
                              static_cast<std::int64_t>(p.force_reason_code));
                    insert_kv("envframe-lifetime-proof-wired", 1);
                    insert_kv("schema-2711", 2711);
                    insert_kv("issue-2711", 2711);
                }
                // Issue #2698: query:occurrence-stability-epoch
                // — independent monotonic epoch (decoupled from
                // cache_epoch). Advances only on outermost
                // success + persist (#2608), densify/steal that
                // pruned goals (#2552/#2608/#2641), or explicit
                // Agent fence (occurrence_stability_fence()).
                // Soft zero-cost on empty goals path;
                // production default records.
                {
                    const auto cur = static_cast<std::int64_t>(
                        g_occurrence_stability_epoch.load(std::memory_order_relaxed));
                    const auto fence_calls = static_cast<std::int64_t>(
                        g_occurrence_stability_fence_calls_total.load(std::memory_order_relaxed));
                    const auto adv_persist = static_cast<std::int64_t>(
                        g_occurrence_stability_advance_on_persist_total.load(
                            std::memory_order_relaxed));
                    const auto adv_prune = static_cast<std::int64_t>(
                        g_occurrence_stability_advance_on_prune_total.load(
                            std::memory_order_relaxed));
                    const auto wired = static_cast<std::int64_t>(
                        g_occurrence_stability_wired.load(std::memory_order_relaxed));
                    insert_kv("occurrence-stability-epoch", cur);
                    insert_kv("occurrence-stability-fence-"
                              "calls-total",
                              fence_calls);
                    insert_kv("occurrence-stability-advance-on-"
                              "persist-total",
                              adv_persist);
                    insert_kv("occurrence-stability-advance-on-"
                              "prune-total",
                              adv_prune);
                    insert_kv("occurrence-stability-wired", wired);
                    insert_kv("schema-2698", 2698);
                    insert_kv("issue-2698", 2698);
                    // Issue #2700:
                    // query:handoff-ref-mailbox-gate — explicit
                    // happens-before contract surface. While
                    // outermost MutationBoundaryGuard is held,
                    // MailMessage payloads carrying a
                    // StableNodeRef MUST have completed
                    // Evaluator::handoff_ref before push /
                    // broadcast_fanout succeeds. Rejects bump
                    // MultiFiberMailboxStats::handoff_reject_total
                    // (file-scope). Additive surface — no
                    // replacement of #2632 / #2312 / #2680 /
                    // #2188 / #2347 counters.
                    {
                        const auto& mfst = aura::serve::mf_mailbox::g_mf_mailbox_stats;
                        insert_kv("handoff-reject-total",
                                  static_cast<std::int64_t>(
                                      mfst.handoff_reject_total.load(std::memory_order_relaxed)));
                        insert_kv("mailbox-deferred-mutation-hold-"
                                  "total",
                                  static_cast<std::int64_t>(
                                      mfst.mailbox_deferred_mutation_hold_total.load(
                                          std::memory_order_relaxed)));
                        insert_kv("mailbox-handoff-ref-gate-wired", 1);
                        insert_kv("schema-2700", 2700);
                        insert_kv("issue-2700", 2700);
                        // Issue #2701:
                        // query:mutation-hold-budget-gate —
                        // Agent-visible reject surface. Soft /
                        // sandbox observes; production
                        // hard-rejects new mutate admit when
                        // live longest hold exceeds budget
                        // (AURA_MUTATION_HOLD_BUDGET_US /
                        // default 100_000 µs). Additive — no
                        // replacement of #2587 / #2630 / #2660
                        // / #2188 surfaces.
                        {
                            const auto rej = static_cast<std::int64_t>(
                                mutation_hold_budget_reject_total_v_read());
                            const auto soft = static_cast<std::int64_t>(
                                mutation_hold_budget_soft_observe_total_v_read());
                            const auto wired =
                                static_cast<std::int64_t>(mutation_hold_budget_wired_v_read());
                            insert_kv("mutation-hold-budget-"
                                      "reject-total",
                                      rej);
                            insert_kv("mutation-hold-budget-"
                                      "soft-observe-total",
                                      soft);
                            insert_kv("mutation-hold-budget-wired", wired);
                            insert_kv("schema-2701", 2701);
                            insert_kv("issue-2701", 2701);
                            // Issue #2720: P0 holder-degrade
                            // path (#2701 residual). Same query
                            // surface
                            // (query:mutation-hold-budget-gate)
                            // so Agents see #2701 reject +
                            // #2720 degrade counters together —
                            // the full story. Additive — all
                            // #2701 keys above preserved.
                            // Same-fiber / cross-fiber split
                            // lets Agents attribute "degrade
                            // hit the current fiber" vs
                            // "cross-fiber attempt (real cancel
                            // = follow-up)".
                            const auto deg_total = static_cast<std::int64_t>(
                                mutation_hold_budget_holder_degrade_total_v_read());
                            const auto deg_same = static_cast<std::int64_t>(
                                mutation_hold_budget_holder_degrade_same_fiber_total_v_read());
                            const auto deg_cross = static_cast<std::int64_t>(
                                mutation_hold_budget_holder_degrade_cross_fiber_total_v_read());
                            const auto deg_wired = static_cast<std::int64_t>(
                                mutation_hold_budget_holder_degrade_wired_v_read());
                            insert_kv("mutation-hold-budget-"
                                      "holder-degrade-total",
                                      deg_total);
                            insert_kv("mutation-hold-budget-holder-"
                                      "degrade-same-fiber-total",
                                      deg_same);
                            insert_kv("mutation-hold-budget-holder-"
                                      "degrade-cross-fiber-total",
                                      deg_cross);
                            insert_kv("mutation-hold-budget-"
                                      "holder-degrade-wired",
                                      deg_wired);
                            insert_kv("schema-2720", 2720);
                            insert_kv("issue-2720", 2720);
                            // Issue #2724:
                            // region/subtree-scoped concurrent
                            // admit — Agent-visible counters
                            // for the disjoint region check.
                            // Additive — all #2701/#2720/
                            // #2587/#2630 surfaces preserved.
                            insert_kv("mutation-region-concurrent-"
                                      "admit-total",
                                      static_cast<std::int64_t>(
                                          mutation_region_concurrent_admit_total_v_read()));
                            insert_kv("mutation-region-overlap-"
                                      "reject-total",
                                      static_cast<std::int64_t>(
                                          mutation_region_overlap_reject_total_v_read()));
                            insert_kv("mutation-region-concurrent-"
                                      "wired",
                                      static_cast<std::int64_t>(
                                          mutation_region_concurrent_wired_v_read()));
                            insert_kv("schema-2724", 2724);
                            insert_kv("issue-2724", 2724);
                            // Issue #2754: cone / ImpactScope
                            // mask-AND residual
                            // (#2724 follow-up). Equal keys +
                            // proven cone disjoint → concurrent
                            // admit; dashboards split
                            // key-disjoint vs cone-disjoint via
                            // the cone counter. Additive — all
                            // #2724 surfaces above preserved.
                            insert_kv("mutation-region-concurrent-"
                                      "cone-admit-total",
                                      static_cast<std::int64_t>(
                                          mutation_region_concurrent_cone_admit_total_v_read()));
                            insert_kv("mutation-region-cone-disjoint-"
                                      "wired",
                                      static_cast<std::int64_t>(
                                          mutation_region_cone_disjoint_wired_v_read()));
                            insert_kv("schema-2754", 2754);
                            insert_kv("issue-2754", 2754);
                            // Issue #2757: mask-AND disjoint
                            // admit (zero keys + equal keys).
                            // Superset of #2754 cone path;
                            // quiet path (no masks) never
                            // bumps. Additive — all #2724/#2754
                            // surfaces above preserved.
                            insert_kv("mutation-region-mask-disjoint-"
                                      "admit-total",
                                      static_cast<std::int64_t>(
                                          mutation_region_mask_disjoint_admit_total_v_read()));
                            insert_kv("mutation-region-mask-disjoint-"
                                      "wired",
                                      static_cast<std::int64_t>(
                                          mutation_region_mask_disjoint_wired_v_read()));
                            insert_kv("schema-2757", 2757);
                            insert_kv("issue-2757", 2757);
                            // Issue #2760: ImpactScope /
                            // dirty-bit mask production
                            // enablement (#2724 residual).
                            // Counts concurrent admits that
                            // used a non-zero proven or derived
                            // cone mask. Additive — all
                            // #2724/#2754/#2757 surfaces above
                            // preserved.
                            insert_kv("mutation-region-impact-mask-"
                                      "admit-total",
                                      static_cast<std::int64_t>(
                                          mutation_region_impact_mask_admit_total_v_read()));
                            insert_kv("mutation-region-impact-mask-"
                                      "wired",
                                      static_cast<std::int64_t>(
                                          mutation_region_impact_mask_wired_v_read()));
                            insert_kv("schema-2760", 2760);
                            insert_kv("issue-2760", 2760);
                            // Issue #2761: mask-AND sole
                            // authority when both masks proven
                            // — unequal keys with overlapping
                            // cones reject. mask-overlap-reject
                            // attributes mask-strength rejects
                            // (subset of overlap-reject).
                            // Additive — all
                            // #2724/#2754/#2757/#2760 above.
                            insert_kv("mutation-region-mask-overlap-"
                                      "reject-total",
                                      static_cast<std::int64_t>(
                                          mutation_region_mask_overlap_reject_total_v_read()));
                            insert_kv("mutation-region-mask-overlap-"
                                      "wired",
                                      static_cast<std::int64_t>(
                                          mutation_region_mask_overlap_wired_v_read()));
                            insert_kv("schema-2761", 2761);
                            insert_kv("issue-2761", 2761);
                            // Issue #2847: region type/occurrence
                            // commit bind — Soft observe / production
                            // reject when OccurrenceGoal pred bits fall
                            // outside admitted cone mask. Additive —
                            // all #2724/#2754/#2757/#2760/#2761 above.
                            insert_kv("region-type-cross-talk-observe-total",
                                      static_cast<std::int64_t>(
                                          aura::compiler::typed_audit::
                                              region_type_cross_talk_observe_total_v_read()));
                            insert_kv("region-type-cross-talk-reject-total",
                                      static_cast<std::int64_t>(
                                          aura::compiler::typed_audit::
                                              region_type_cross_talk_reject_total_v_read()));
                            insert_kv("region-type-cross-talk-wired",
                                      static_cast<std::int64_t>(
                                          aura::compiler::typed_audit::
                                              region_type_cross_talk_wired_v_read()));
                            insert_kv("schema-2847", 2847);
                            insert_kv("issue-2847", 2847);
                            // Issue #2726: P0 cross-fiber
                            // hold-budget force-degrade real
                            // cancel (per-fiber pending-cancel
                            // map polled at safepoints) —
                            // closes #2720 residual. Additive —
                            // all #2701/#2720/#2724/#2587/#2630
                            // surfaces preserved. fired vs
                            // consumed divergence is observable
                            // (Fiber lifetime race = holder
                            // gone before consume; Agent
                            // health).
                            insert_kv(
                                "mutation-hold-budget-holder-"
                                "degrade-cross-fiber-cancel-"
                                "fired-"
                                "total",
                                static_cast<std::int64_t>(
                                    mutation_hold_budget_holder_degrade_cross_fiber_cancel_fired_total_v_read()));
                            insert_kv(
                                "mutation-hold-budget-holder-"
                                "degrade-cross-fiber-cancel-"
                                "consumed-"
                                "total",
                                static_cast<std::int64_t>(
                                    mutation_hold_budget_holder_degrade_cross_fiber_cancel_consumed_total_v_read()));
                            insert_kv("schema-2726", 2726);
                            insert_kv("issue-2726", 2726);
                            // Issue #2932: hold-budget overtime forced
                            // outermost fail-closed (safepoint edge,
                            // not cooperative Phase-5-only). Additive —
                            // all #2701/#2720/#2726 surfaces preserved.
                            insert_kv("mutation-hold-budget-forced-fail-"
                                      "closed-total",
                                      static_cast<std::int64_t>(
                                          mutation_hold_budget_forced_fail_closed_total_v_read()));
                            insert_kv("mutation-hold-budget-forced-fail-"
                                      "closed-wired",
                                      static_cast<std::int64_t>(
                                          mutation_hold_budget_forced_fail_closed_wired_v_read()));
                            insert_kv("schema-2932", 2932);
                            insert_kv("issue-2932", 2932);
                            // Issue #2999: outermost dtor consume (exit half
                            // of #2932). Additive — in-body window still
                            // requires force-safepoint to enter dtor.
                            insert_kv(
                                "mutation-hold-budget-forced-fail-"
                                "closed-dtor-consume-total",
                                static_cast<std::int64_t>(
                                    mutation_hold_budget_forced_fail_closed_dtor_consume_total_v_read()));
                            insert_kv("schema-2999", 2999);
                            insert_kv("issue-2999", 2999);
                            // Issue #2702:
                            // query:resume-hard-fail —
                            // Agent-visible resume hard-fail
                            // surface. Production path: ticket
                            // mismatch or
                            // mutation_safety_snapshot_inconsistent
                            // → request_cancel +
                            // set_state(Done), no swapcontext
                            // body. Soft / test override:
                            // metric-only continue. Additive —
                            // no replacement of #2346 / #2518 /
                            // #2667 / #2184 / #2310 surfaces.
                            {
                                const auto hard = static_cast<std::int64_t>(
                                    aura::serve::resume_hard_fail_total_v_read());
                                const auto soft = static_cast<std::int64_t>(
                                    aura::serve::resume_soft_observe_total_v_read());
                                const auto wired = static_cast<std::int64_t>(
                                    aura::serve::resume_hard_fail_wired_v_read());
                                insert_kv("resume-hard-fail-total", hard);
                                insert_kv("resume-soft-observe-total", soft);
                                insert_kv("resume-hard-fail-wired", wired);
                                insert_kv("schema-2702", 2702);
                                insert_kv("issue-2702", 2702);
                                // Issue #2703:
                                // query:cone-outside-goal-drop
                                // — Agent-visible production
                                // hard-face surface. Soft /
                                // sandbox observes only;
                                // production hard-rejects
                                // commit when partial cone
                                // truncate drops outside-If
                                // OccurrenceGoals ("half-green"
                                // typed mutate). Additive — no
                                // replacement of #2621 / #2560
                                // / #2672 surfaces.
                                {
                                    const auto hard = static_cast<std::int64_t>(
                                        cone_outside_goal_drop_total_v_read());
                                    const auto soft = static_cast<std::int64_t>(
                                        cone_outside_goal_drop_soft_total_v_read());
                                    const auto wired = static_cast<std::int64_t>(
                                        cone_outside_goal_drop_wired_v_read());
                                    insert_kv("cone-outside-"
                                              "goal-drop-total",
                                              hard);
                                    insert_kv("cone-outside-goal-"
                                              "drop-soft-total",
                                              soft);
                                    insert_kv("cone-outside-"
                                              "goal-drop-wired",
                                              wired);
                                    insert_kv("schema-2703", 2703);
                                    insert_kv("issue-2703", 2703);
                                    // Issue #2704:
                                    // query:occurrence-empty-after-fence
                                    // — Agent-visible
                                    // production hard-face
                                    // surface. Soft / sandbox
                                    // observes only; production
                                    // hard-rejects commit when
                                    // steal/densify fence drops
                                    // OccurrenceGoals +
                                    // rehydrate returns 0
                                    // (empty priority roots).
                                    // Additive — no replacement
                                    // of #2608 / #2641 / #2552
                                    // / #2622 / #2672 surfaces.
                                    {
                                        const auto hard = static_cast<std::int64_t>(
                                            occurrence_empty_after_fence_total_v_read());
                                        const auto soft = static_cast<std::int64_t>(
                                            occurrence_empty_after_fence_soft_total_v_read());
                                        const auto wired = static_cast<std::int64_t>(
                                            occurrence_empty_after_fence_wired_v_read());
                                        insert_kv("occurrence-empty-"
                                                  "after-fence-total",
                                                  hard);
                                        insert_kv("occurrence-empty-"
                                                  "after-fence-soft-"
                                                  "total",
                                                  soft);
                                        insert_kv("occurrence-empty-"
                                                  "after-fence-wired",
                                                  wired);
                                        insert_kv("schema-2704", 2704);
                                        insert_kv("issue-2704", 2704);
                                        insert_kv(
                                            "type-linear-proof-reject-empty-after-fence-total",
                                            static_cast<std::int64_t>(
                                                aura::compiler::typed_audit::
                                                    type_linear_proof_reject_empty_after_fence_total_v_read()));
                                        insert_kv("type-linear-proof-empty-after-fence-wired", 1);
                                        insert_kv("schema-2981", 2981);
                                        insert_kv("issue-2981", 2981);
                                    }
                                    // Issue #2716: occurrence
                                    // hard-faces active branch
                                    // (close #2703 / #2704
                                    // residual). When
                                    // production / Full
                                    // + face hit under
                                    // commit_readiness_live_policy,
                                    // the active branch in
                                    // commit_readiness
                                    // hard-rejects with
                                    // force_reason
                                    // "cone_outside_goal_drop"
                                    // (code 10) or
                                    // "occurrence_empty_after_fence"
                                    // (code 11). This counter
                                    // bumps whenever the active
                                    // branch fires under
                                    // prod/Full — surface for
                                    // Agent dashboards to
                                    // attribute "active face
                                    // wired in" vs "face fired
                                    // but Soft path observed
                                    // only". Additive — no
                                    // replacement of #2703 /
                                    // #2704 / #2621 / #2458 /
                                    // #2608 query keys
                                    // (preserved above). #2703
                                    // / #2704 still surface the
                                    // face counters; #2716
                                    // surfaces the
                                    // active-branch recover
                                    // counter (production /
                                    // Full only).
                                    {
                                        const auto recover = static_cast<std::int64_t>(
                                            occurrence_hard_face_full_solve_recover_total_v_read());
                                        insert_kv("occurrence-hard-"
                                                  "face-full-solve-"
                                                  "recover-total",
                                                  recover);
                                        insert_kv("occurrence-hard-"
                                                  "face-full-solve-"
                                                  "recover-wired",
                                                  1);
                                        insert_kv("schema-2716", 2716);
                                        insert_kv("issue-2716", 2716);
                                        // Issue #2750: true
                                        // recover success/fail.
                                        insert_kv(
                                            "occurrence-hard-"
                                            "face-recover-"
                                            "success-total",
                                            static_cast<std::int64_t>(
                                                occurrence_hard_face_recover_success_total_v_read()));
                                        insert_kv(
                                            "occurrence-hard-"
                                            "face-recover-fail-"
                                            "total",
                                            static_cast<std::int64_t>(
                                                occurrence_hard_face_recover_fail_total_v_read()));
                                        insert_kv("schema-2750", 2750);
                                        insert_kv("issue-2750", 2750);
                                        // Issue #2909: cone truncate +
                                        // outside drop force-closure.
                                        insert_kv("cone-truncate-force-closure-total",
                                                  static_cast<std::int64_t>(
                                                      cone_truncate_force_closure_total_v_read()));
                                        insert_kv(
                                            "cone-truncate-force-closure-attempt-total",
                                            static_cast<std::int64_t>(
                                                cone_truncate_force_closure_attempt_total_v_read()));
                                        insert_kv(
                                            "cone-truncate-force-closure-reject-total",
                                            static_cast<std::int64_t>(
                                                cone_truncate_force_closure_reject_total_v_read()));
                                        insert_kv("cone-truncate-force-closure-wired",
                                                  static_cast<std::int64_t>(
                                                      cone_truncate_force_closure_wired_v_read()));
                                        insert_kv("schema-2909", kConeTruncateForceClosureIssue);
                                        insert_kv("issue-2909", kConeTruncateForceClosureIssue);
                                        // Issue #2962: residual SOLVED-only recover /
                                        // hard-reject Agent keys (additive to #2909).
                                        insert_kv(
                                            "cone-outside-goal-drop-recover-ok-total",
                                            static_cast<std::int64_t>(
                                                cone_outside_goal_drop_recover_ok_total_v_read()));
                                        insert_kv(
                                            "cone-outside-goal-drop-reject-total",
                                            static_cast<std::int64_t>(
                                                cone_outside_goal_drop_reject_total_v_read()));
                                        insert_kv(
                                            "cone-outside-goal-drop-recover-reject-wired",
                                            static_cast<std::int64_t>(
                                                cone_outside_goal_drop_recover_reject_wired_v_read()));
                                        insert_kv("schema-2962",
                                                  kConeOutsideGoalDropRecoverRejectIssue);
                                        insert_kv("issue-2962",
                                                  kConeOutsideGoalDropRecoverRejectIssue);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // Issue #2308: Agent-stable SolverSnapshot (status
            // + unresolved + blame + repair_nodes + truncated +
            // production escalation). Built from the live
            // commit CS via snapshot_constraint_system —
            // mirrors C++ API shape so Agents query the same
            // fields they see in SolverSnapshot. Pure read; no
            // solve side effects.
            //   solver-snapshot-status: 0=SOLVED, 1=CONFLICT,
            //   2=TIMEOUT solver-snapshot-unresolved-count:
            //   size of unresolved vec
            //   solver-snapshot-repair-nodes-count: dedup
            //   affected_node
            //     ids from unresolved + blame.frames (cap 16)
            //   solver-snapshot-blame-complete: 0/1 from
            //   blame.is_complete() solver-snapshot-truncated:
            //   0/1 from blame.truncated_reverify
            //     || cs.last_reverify_truncated()
            {
                SolverSnapshot snap{};
                if (ev && ev->commit_cs_live()) {
                    if (auto* ctc_h = static_cast<aura::compiler::TypeChecker*>(
                            ev->commit_type_checker_handle())) {
                        snap = snapshot_constraint_system(ctc_h->constraint_system(), nullptr);
                    }
                }
                insert_kv("solver-snapshot-status", static_cast<std::int64_t>(snap.status));
                insert_kv("solver-snapshot-unresolved-count",
                          static_cast<std::int64_t>(snap.unresolved.size()));
                insert_kv("solver-snapshot-repair-nodes-count",
                          static_cast<std::int64_t>(snap.repair_nodes.size()));
                insert_kv("solver-snapshot-blame-complete", snap.blame.is_complete() ? 1 : 0);
                insert_kv("solver-snapshot-truncated", snap.truncated_reverify ? 1 : 0);
            }
            insert_kv("schema-2308", 2308);
            insert_kv("issue-2308", 2308);
            // Wired sentinel — confirms the #2308 refactor
            // landed (C++ API + query surface both present).
            insert_kv("solver-snapshot-wired", 1);
            // Issue #2281: Agent-visible TypedMutationAudit
            // decision query. Exposes the current strategy /
            // sample_ratio / production_defaults state + a
            // representative decide() result for inputs (mid=1,
            // nodes=1, linear=false, strict=false, match=false)
            // — the typical "skip" path under Sampled. Agent
            // can call decide() directly with custom inputs to
            // predict force-rollback. Schema-2281 additive
            // (aligns with #2222 LinearEnforce).
            {
                using aura::compiler::typed_audit::decide;
                const auto d = decide(/*mid=*/1, /*nodes=*/1,
                                      /*linear=*/false, /*strict=*/false,
                                      /*match=*/false);
                insert_kv("audit-decision-strategy", d.strategy);
                insert_kv("audit-decision-sample-ratio", d.sample_ratio);
                insert_kv("audit-decision-production-defaults", d.production_defaults ? 1 : 0);
                insert_kv("audit-decision-would-audit", d.would_audit ? 1 : 0);
                insert_kv("audit-decision-would-hard-gate", d.would_hard_gate ? 1 : 0);
                // force_reason → int mapping (documented in
                // typed_mutation_audit.h):
                //   0=off 1=full 2=linear 3=match-sites 4=nodes
                //   5=production-nodes 6=sampled-hit
                //   7=sampled-skip 8=strict
                std::int64_t reason_int = -1;
                if (d.force_reason == "off")
                    reason_int = 0;
                else if (d.force_reason == "full")
                    reason_int = 1;
                else if (d.force_reason == "linear")
                    reason_int = 2;
                else if (d.force_reason == "match-sites")
                    reason_int = 3;
                else if (d.force_reason == "nodes")
                    reason_int = 4;
                else if (d.force_reason == "production-nodes")
                    reason_int = 5;
                else if (d.force_reason == "sampled-hit")
                    reason_int = 6;
                else if (d.force_reason == "sampled-skip")
                    reason_int = 7;
                else if (d.force_reason == "strict")
                    reason_int = 8;
                insert_kv("audit-decision-force-reason", reason_int);
                insert_kv("audit-decision-wired", 1);
                insert_kv("schema-2281", 2281);
                insert_kv("issue-2281", 2281);
            }
            // Issue #2553: single Agent commit-readiness score
            // (solve × linear × blame × truncate). Exposes live
            // hard-policy flags + the pure commit_readiness()
            // result for a clean SOLVED face (vacuous healthy
            // when no pending commit — AC5 zero cost). Agents
            // recompute with custom CommitReadinessInput via
            // the C++ helper. Additive schema-2553; no commit
            // side effects.
            {
                using aura::compiler::typed_audit::commit_readiness;
                using aura::compiler::typed_audit::commit_readiness_live_policy;
                using aura::compiler::typed_audit::kRefinedConsistencyGateIssue;
                using aura::compiler::typed_audit::refined_consistency_drift_face_hit;
                using aura::compiler::typed_audit::refined_consistency_observe_total_v_read;
                using aura::compiler::typed_audit::refined_consistency_recover_total_v_read;
                using aura::compiler::typed_audit::refined_consistency_reject_total_v_read;
                using aura::compiler::typed_audit::refined_consistency_wired_v_read;
                auto in = commit_readiness_live_policy();
                // Clean face defaults: SOLVED + linear_ok +
                // blame_ok + !trunc.
                const auto cr = commit_readiness(in);
                insert_kv("commit-readiness-bp", static_cast<std::int64_t>(cr.readiness_bp));
                insert_kv("commit-readiness-would-allow", cr.would_allow_commit ? 1 : 0);
                insert_kv("commit-readiness-force-reason", cr.force_reason_code);
                insert_kv("commit-readiness-empty-cs-hard", in.empty_cs_hard ? 1 : 0);
                insert_kv("commit-readiness-truncate-hard", in.truncate_hard ? 1 : 0);
                insert_kv("commit-readiness-linear-hard", in.linear_hard ? 1 : 0);
                insert_kv("commit-readiness-blame-hard", in.blame_hard ? 1 : 0);
                // Sample hard cells for Agent matrix without
                // mutate: empty_cs hard under live policy.
                {
                    auto e = in;
                    e.expected_partial = true;
                    e.cs_has_work = false;
                    const auto er = commit_readiness(e);
                    insert_kv("commit-readiness-sample-empty-"
                              "cs-allow",
                              er.would_allow_commit ? 1 : 0);
                    insert_kv("commit-readiness-sample-empty-"
                              "cs-reason",
                              er.force_reason_code);
                }
                {
                    auto t = in;
                    t.truncated_reverify = true;
                    const auto tr = commit_readiness(t);
                    insert_kv("commit-readiness-sample-"
                              "truncate-allow",
                              tr.would_allow_commit ? 1 : 0);
                    insert_kv("commit-readiness-sample-"
                              "truncate-reason",
                              tr.force_reason_code);
                }
                // Issue #2621: cone_truncate sample (partial
                // cone soft overflow).
                {
                    auto c = in;
                    c.partial_cone_truncated = true;
                    c.truncated_reverify = false;
                    const auto cr_cone = commit_readiness(c);
                    insert_kv("commit-readiness-sample-cone-"
                              "truncate-allow",
                              cr_cone.would_allow_commit ? 1 : 0);
                    insert_kv("commit-readiness-sample-cone-"
                              "truncate-reason",
                              cr_cone.force_reason_code);
                    insert_kv("commit-readiness-force-reason-"
                              "cone-truncate",
                              9);
                }
                // Issue #2610: auto_partial sample
                // (under-marked cone + empty CS).
                {
                    auto a = in;
                    a.expected_partial = false;
                    a.auto_partial_from_cone = true;
                    a.cs_has_work = false;
                    const auto ar = commit_readiness(a);
                    insert_kv("commit-readiness-sample-auto-"
                              "partial-allow",
                              ar.would_allow_commit ? 1 : 0);
                    insert_kv("commit-readiness-sample-auto-"
                              "partial-reason",
                              ar.force_reason_code);
                    insert_kv("commit-readiness-force-reason-"
                              "auto-partial",
                              6);
                }
                insert_kv("commit-readiness-wired", 1);
                insert_kv("schema-2553", 2553);
                insert_kv("issue-2553", 2553);
                // Issue #2911: unified refined-consistency hard gate.
                insert_kv("refined-consistency-wired",
                          static_cast<std::int64_t>(refined_consistency_wired_v_read()));
                insert_kv("refined-consistency-observe-total",
                          static_cast<std::int64_t>(refined_consistency_observe_total_v_read()));
                insert_kv("refined-consistency-reject-total",
                          static_cast<std::int64_t>(refined_consistency_reject_total_v_read()));
                insert_kv("refined-consistency-recover-total",
                          static_cast<std::int64_t>(refined_consistency_recover_total_v_read()));
                insert_kv("refined-consistency-face", refined_consistency_drift_face_hit() ? 1 : 0);
                insert_kv("commit-readiness-force-reason-refined-drift", 15);
                insert_kv("schema-2911", kRefinedConsistencyGateIssue);
                insert_kv("issue-2911", kRefinedConsistencyGateIssue);
            }
            // Issue #2220: long-lived TypeChecker on Evaluator
            // mutate path.
            {
                const std::int64_t tc_create =
                    m ? static_cast<std::int64_t>(
                            m->typecheck_persistent_create_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t tc_reuse =
                    m ? static_cast<std::int64_t>(
                            m->typecheck_persistent_reuse_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t tc_inv =
                    m ? static_cast<std::int64_t>(m->typecheck_persistent_invalidate_total.load(
                            std::memory_order_relaxed))
                      : 0;
                const std::int64_t tc_hits =
                    m ? static_cast<std::int64_t>(
                            m->typecheck_persistent_cs_cache_hits.load(std::memory_order_relaxed))
                      : 0;
                insert_kv("typecheck-persistent-create-total", tc_create);
                insert_kv("typecheck-persistent-reuse-total", tc_reuse);
                insert_kv("typecheck-persistent-invalidate-total", tc_inv);
                insert_kv("typecheck-persistent-cs-cache-hits", tc_hits);
                insert_kv("typecheck-persistent-wired", 1);
                insert_kv("schema-2220", 2220);
                insert_kv("issue-2220", 2220);
            }
            // Issue #2219: post-mutate Soft/Hard type gate
            // policy surface.
            {
                const auto mtg = mutate_type_gate::snapshot();
                insert_kv("mutate-type-gate-mode", mtg.mode);
                insert_kv("mutate-soft-type-skip-total",
                          static_cast<std::int64_t>(mtg.soft_type_skip_total));
                insert_kv("mutate-type-gate-exhaustiveness-"
                          "reject-total",
                          static_cast<std::int64_t>(mtg.exhaustiveness_reject_total));
                insert_kv("mutate-type-gate-hard-type-error-reject-"
                          "total",
                          static_cast<std::int64_t>(mtg.hard_type_error_reject_total));
                insert_kv("mutate-type-gate-check-total",
                          static_cast<std::int64_t>(mtg.gate_check_total));
                insert_kv("mutate-type-gate-wired", 1);
                insert_kv("schema-2219", 2219);
                insert_kv("issue-2219", 2219);
                // Issue #2279: production lock state +
                // soft-override opt-out
                // + alarm counter. Mirrors
                // mutate_type_gate::Snapshot fields
                // (production_locked, soft_override_allowed,
                // soft_in_production_alarm_total) and the
                // CompilerMetrics::mutate_type_gate_soft_in_production_alarm_total
                // per-instance mirror. Schema-2279 additive.
                insert_kv("mutate-type-gate-production-locked", mtg.production_locked);
                insert_kv("mutate_type_gate_production_locked", mtg.production_locked);
                insert_kv("mutate-type-gate-soft-override-allowed", mtg.soft_override_allowed);
                insert_kv("mutate_type_gate_soft_override_allowed", mtg.soft_override_allowed);
                insert_kv("mutate-type-gate-soft-in-production-alarm-"
                          "total",
                          static_cast<std::int64_t>(mtg.soft_in_production_alarm_total));
                const std::int64_t alarm_metrics_mirror =
                    m ? static_cast<std::int64_t>(
                            m->mutate_type_gate_soft_in_production_alarm_total.load(
                                std::memory_order_relaxed))
                      : 0;
                insert_kv("mutate_type_gate_soft_in_production_"
                          "alarm_total",
                          alarm_metrics_mirror);
                insert_kv("mutate-type-gate-lock-wired", 1);
                insert_kv("schema-2279", 2279);
                insert_kv("issue-2279", 2279);
            }
            // Issue #2191: type affected cone ↔ dirty::DepGraph
            // cascade unify.
            {
                const std::int64_t type_mirrored =
                    m ? static_cast<std::int64_t>(
                            m->type_dirty_cone_mirrored_total.load(std::memory_order_relaxed))
                      : static_cast<std::int64_t>(
                            aura::compiler::dirty::type_dirty_cone_mirrored_total.load(
                                std::memory_order_relaxed));
                const std::int64_t union_avg_x100 = static_cast<std::int64_t>(
                    aura::compiler::dirty::type_ir_cone_union_size_avg() * 100.0);
                insert_kv("type_dirty_cone_mirrored_total", type_mirrored);
                insert_kv("type-dirty-cone-mirrored-total", type_mirrored);
                insert_kv("type_ir_cone_union_size_avg", union_avg_x100);
                insert_kv("type-ir-cone-union-size-avg-x100", union_avg_x100);
                insert_kv("type-dirty-cone-mirror-wired", 1);
                insert_kv("schema-2191", 2191);
                insert_kv("issue-2191", 2191);
            }
            // Issue #2144: outermost Guard-exit selective memo
            // + occurrence reanalyze.
            const std::int64_t guard_refresh =
                m ? static_cast<std::int64_t>(
                        m->guard_exit_occurrence_refresh_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t guard_skip =
                m ? static_cast<std::int64_t>(
                        m->guard_exit_occurrence_early_skip_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t guard_reanalyze =
                m ? static_cast<std::int64_t>(
                        m->guard_exit_occurrence_reanalyze_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t guard_sel =
                m ? static_cast<std::int64_t>(
                        m->guard_exit_selective_invalidate_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t narrow_recovery =
                m ? static_cast<std::int64_t>(
                        m->narrowing_dirty_recovery_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("guard-exit-occurrence-refresh-total", guard_refresh);
            insert_kv("guard-exit-occurrence-early-skip-total", guard_skip);
            insert_kv("guard-exit-occurrence-reanalyze-total", guard_reanalyze);
            insert_kv("guard-exit-selective-invalidate-total", guard_sel);
            insert_kv("narrowing-dirty-recovery", narrow_recovery);
            insert_kv("narrowing_dirty_recovery", narrow_recovery);
            insert_kv("guard-exit-occurrence-refresh-wired",
                      m ? static_cast<std::int64_t>(m->guard_exit_occurrence_refresh_wired.load(
                              std::memory_order_relaxed))
                        : 1);
            insert_kv("schema-2144", 2144);
            insert_kv("issue-2144", 2144);
            // Issue #2146: adaptive reverify limit + truncation
            // Agent surface.
            const std::int64_t reverify_limit_used =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_reverify_limit_used.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t reverify_trunc_2146 =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_reverify_truncated_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t pending_full =
                m ? static_cast<std::int64_t>(m->solve_delta_pending_full_solve_roots_last.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t pending_enq =
                m ? static_cast<std::int64_t>(m->solve_delta_pending_full_solve_enqueued_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t trunc_flag =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_truncated_reverify_last.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t unscanned_last =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_unscanned_last.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t adaptive_adj =
                m ? static_cast<std::int64_t>(
                        m->reverify_adaptive_adjustments_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("solve-delta-reverify-limit-used", reverify_limit_used);
            insert_kv("solve_delta_reverify_limit_used", reverify_limit_used);
            insert_kv("solve-delta-reverify-truncated-total", reverify_trunc_2146);
            insert_kv("solve_delta_reverify_truncated_total", reverify_trunc_2146);
            // Issue #2356: truncated reverify one-shot expand
            // for occurrence/let-poly.
            const std::int64_t reverify_expand =
                m ? static_cast<std::int64_t>(
                        m->delta_reverify_expand_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("delta-reverify-expand-total", reverify_expand);
            insert_kv("delta_reverify_expand_total", reverify_expand);
            insert_kv("delta-reverify-expand-wired", 1);
            insert_kv("schema-2356", 2356);
            // Issue #2939: dep-closure reverify (O(affected) vs clean scan).
            {
                const std::int64_t cl_nodes =
                    m ? static_cast<std::int64_t>(
                            m->delta_reverify_closure_nodes_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t cl_edges =
                    m ? static_cast<std::int64_t>(
                            m->delta_reverify_closure_edges_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t cl_cap =
                    m ? static_cast<std::int64_t>(
                            m->delta_reverify_closure_cap_hit_total.load(std::memory_order_relaxed))
                      : 0;
                insert_kv("delta-reverify-closure-nodes-total", cl_nodes);
                insert_kv("delta_reverify_closure_nodes_total", cl_nodes);
                insert_kv("delta-reverify-closure-edges-total", cl_edges);
                insert_kv("delta_reverify_closure_edges_total", cl_edges);
                insert_kv("delta-reverify-closure-cap-hit-total", cl_cap);
                insert_kv("delta_reverify_closure_cap_hit_total", cl_cap);
                insert_kv("delta-reverify-closure-wired", 1);
                insert_kv("schema-2939", 2939);
                insert_kv("issue-2939", 2939);
            }
            insert_kv("issue-2356", 2356);
            insert_kv("solve-delta-pending-full-solve-roots", pending_full);
            insert_kv("pending-full-solve-roots", pending_full);
            insert_kv("solve-delta-pending-full-solve-enqueued", pending_enq);
            insert_kv("truncated-reverify", trunc_flag);
            insert_kv("truncated", trunc_flag); // AC3 alias
            insert_kv("unscanned-constraint-count", unscanned_last);
            insert_kv("unscanned", unscanned_last);
            insert_kv("reverify-adaptive-adjustments", adaptive_adj);
            insert_kv("reverify-adaptive-wired",
                      m ? static_cast<std::int64_t>(
                              m->reverify_adaptive_wired.load(std::memory_order_relaxed))
                        : 1);
            insert_kv("reverify-base-limit", 256);
            insert_kv("reverify-max-limit", 4096);
            insert_kv("schema-2146", 2146);
            insert_kv("issue-2146", 2146);
            // Issue #1924: DeltaBlameChain / typed_mutate blame
            // propagation
            insert_kv("blame-chain-complete-total", blame_complete);
            insert_kv("blame_chain_complete_total", blame_complete);
            insert_kv("blame-propagation-miss-total", blame_miss);
            insert_kv("blame_propagation_miss_total", blame_miss);
            insert_kv("blame-propagation-coercion-stamped", blame_coercion);
            insert_kv("blame-propagation-narrow-stamped", blame_narrow);
            insert_kv("blame-propagation-wired", 1);
            insert_kv("schema-1924", 1924);
            insert_kv("issue-1924", 1924);
            // Issue #2024: occurrence narrowing provenance
            // chain completeness
            insert_kv("coercion-provenance-complete-total", coercion_prov_complete);
            insert_kv("coercion-provenance-miss-total", coercion_prov_miss);
            insert_kv("coercion-provenance-sentinel-total", coercion_prov_sentinel);
            insert_kv("coercion-provenance-chain-walks", coercion_prov_walks);
            insert_kv("coercion-provenance-completeness-bp", coercion_completeness_bp);
            insert_kv("blame-chain-completeness-rate", blame_rate);
            // completeness-ratio-bp aliases coercion apply
            // completeness for Agents
            insert_kv("completeness-ratio-bp", coercion_completeness_bp);
            insert_kv("occurrence-provenance-chain-wired", 1);
            insert_kv("schema-2024", 2024);
            insert_kv("issue-2024", 2024);
            // Issue #2147: fast path + weak id honesty under
            // Strict/Full
            insert_kv("coercion-provenance-fast-path-total", coercion_prov_fast);
            insert_kv("coercion_provenance_fast_path_total", coercion_prov_fast);
            insert_kv("coercion-provenance-weak-id-total", coercion_prov_weak);
            insert_kv("coercion_provenance_weak_id_total", coercion_prov_weak);
            insert_kv("coercion-provenance-strict-reject-weak-total", coercion_prov_strict_weak);
            // Issue #2317: Sampled insert counter — bumped when
            // Sampled + incomplete provenance + NOT production
            // reject → still insert CoercionNode (with
            // force-audit via fill_coercion_provenance_chain's
            // note_provenance_miss_for_boundary call). Distinct
            // from coercion-provenance-sampled-reject-total
            // which counts SKIPS. P0 production Sampled hosts
            // must not silently lose coercion sites (soundness
            // / debuggability hole).
            const std::int64_t coercion_sampled_insert = static_cast<std::int64_t>(
                aura::compiler::g_coercion_sampled_insert_incomplete_total.load(
                    std::memory_order_relaxed));
            insert_kv("coercion-sampled-insert-incomplete-total", coercion_sampled_insert);
            insert_kv("coercion_sampled_insert_incomplete_total", coercion_sampled_insert);
            insert_kv("coercion-sampled-insert-policy-wired", 1);
            insert_kv("schema-2317", 2317);
            insert_kv("issue-2317", 2317);
            // Issue #2562: dual-field (pred+mid)
            // require-or-drop under production / Full /
            // AURA_COERCION_DUAL_REQUIRE.
            {
                const std::int64_t dual_drop = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_dual_require_drop_total.load(
                        std::memory_order_relaxed));
                insert_kv("coercion-dual-require-drop-total", dual_drop);
                insert_kv("coercion_dual_require_drop_total", dual_drop);
                insert_kv("coercion-dual-require-enabled",
                          aura::compiler::coercion_dual_require_active() ? 1 : 0);
                insert_kv(
                    "coercion-dual-require-wired",
                    static_cast<std::int64_t>(aura::compiler::g_coercion_dual_require_wired.load(
                        std::memory_order_relaxed)));
                insert_kv("schema-2562", 2562);
                insert_kv("issue-2562", 2562);
            }
            // Issue #2620: Soft/Sampled incomplete → skip
            // insert + force-Full arm. Additive keys; #2317
            // canary counter retained for env=1 inserts.
            {
                const std::int64_t soft_skip = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_soft_incomplete_skip_total.load(
                        std::memory_order_relaxed));
                insert_kv("coercion-soft-incomplete-skip-total", soft_skip);
                insert_kv("coercion_soft_incomplete_skip_total", soft_skip);
                insert_kv("coercion-unify-incomplete-skip-wired",
                          static_cast<std::int64_t>(
                              aura::compiler::g_coercion_unify_incomplete_skip_wired.load(
                                  std::memory_order_relaxed)));
                insert_kv("schema-2620", 2620);
                insert_kv("issue-2620", 2620);
            }
            // Issue #2648: Soft evidence-loss bp + force-Full
            // arm/consume (Agent face).
            {
                const std::int64_t loss_bp =
                    static_cast<std::int64_t>(aura::compiler::coercion_evidence_loss_bp());
                const std::int64_t thr = static_cast<std::int64_t>(
                    aura::compiler::coercion_evidence_loss_threshold_bp());
                const std::int64_t armed = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_evidence_loss_force_armed_total.load(
                        std::memory_order_relaxed));
                const std::int64_t consumed = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_evidence_loss_force_consumed_total.load(
                        std::memory_order_relaxed));
                const std::int64_t breach = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_evidence_loss_breach_total.load(
                        std::memory_order_relaxed));
                insert_kv("coercion-evidence-loss-bp", loss_bp);
                insert_kv("coercion_evidence_loss_bp", loss_bp);
                insert_kv("coercion-evidence-loss-threshold-bp", thr);
                insert_kv("coercion-evidence-loss-force-armed", armed);
                insert_kv("coercion-evidence-loss-force-consumed", consumed);
                insert_kv("coercion-evidence-loss-breach-total", breach);
                insert_kv("coercion-evidence-loss-force-armed-total", armed);
                insert_kv("coercion-evidence-loss-force-"
                          "consumed-total",
                          consumed);
                insert_kv(
                    "coercion-evidence-loss-wired",
                    static_cast<std::int64_t>(aura::compiler::g_coercion_evidence_loss_wired.load(
                        std::memory_order_relaxed)));
                insert_kv("schema-2648", 2648);
                insert_kv("issue-2648", 2648);
            }
            // Issue #2318: anti-starvation streak gate. N
            // consecutive truncated delta solves → force one
            // full ConstraintSystem:: solve() (mirror #2277
            // escalation body). Reads from the
            // per-CompilerMetrics fields added in
            // observability_metrics.h
            // (delta_reverify_truncate_streak +
            // delta_truncate_force_full _solve_total +
            // delta_truncate_streak_threshold + delta_
            // truncate_anti_starve_wired). Threshold reads from
            // env AURA_DELTA_TRUNCATE_STREAK_FULL (default 2).
            const std::int64_t delta_reverify_truncate_streak =
                m ? static_cast<std::int64_t>(
                        m->delta_reverify_truncate_streak.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t delta_truncate_force_full_solve_total =
                m ? static_cast<std::int64_t>(
                        m->delta_truncate_force_full_solve_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t delta_truncate_streak_threshold =
                m ? static_cast<std::int64_t>(
                        m->delta_truncate_streak_threshold.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("delta-reverify-truncate-streak", delta_reverify_truncate_streak);
            insert_kv("delta_truncate_reverify_truncate_streak", delta_reverify_truncate_streak);
            insert_kv("delta-truncate-force-full-solve-total",
                      delta_truncate_force_full_solve_total);
            insert_kv("delta_truncate_force_full_solve_total",
                      delta_truncate_force_full_solve_total);
            insert_kv("delta-truncate-streak-threshold", delta_truncate_streak_threshold);
            insert_kv("delta_truncate_streak_threshold", delta_truncate_streak_threshold);
            insert_kv("delta-truncate-anti-starve-wired",
                      (m && m->delta_truncate_anti_starve_wired.load() != 0) ? 1 : 0);
            insert_kv("delta_truncate_anti_starve_wired",
                      (m && m->delta_truncate_anti_starve_wired.load() != 0) ? 1 : 0);
            insert_kv("schema-2318", 2318);
            insert_kv("issue-2318", 2318);
            // Issue #2508: goal-priority reverify before
            // anti-starve full solve. Runs when truncate streak
            // hits AURA_DELTA_TRUNCATE_STREAK_FULL and
            // occurrence_goals_ / priority roots are live.
            // Recovered → no force-full.
            const std::int64_t goal_pri_reverify =
                m ? static_cast<std::int64_t>(m->delta_truncate_goal_priority_reverify_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t goal_pri_recovered =
                m ? static_cast<std::int64_t>(m->delta_truncate_goal_priority_recovered_total.load(
                        std::memory_order_relaxed))
                  : 0;
            insert_kv("delta-truncate-goal-priority-reverify-total", goal_pri_reverify);
            insert_kv("delta_truncate_goal_priority_reverify_total", goal_pri_reverify);
            insert_kv("delta-truncate-goal-priority-recovered-total", goal_pri_recovered);
            insert_kv("delta_truncate_goal_priority_recovered_total", goal_pri_recovered);
            insert_kv("delta-truncate-goal-priority-wired", 1);
            insert_kv("schema-2508", 2508);
            insert_kv("issue-2508", 2508);
            // Issue #2321: OccurrenceGoal refined-drift
            // observability.
            //   - occurrence-goal-refined-drift-total:
            //   cumulative count of
            //     goals dropped from solve_delta_occurrence
            //     replay when the stored `refined` is no longer
            //     consistent with the current Union-Find
            //     binding of `g.var` (drift detection).
            //   - occurrence-goal-drift-wired: sentinel = 1
            //   when the
            //     re-validate + drop logic is integrated.
            const std::int64_t refined_drift_total =
                m ? static_cast<std::int64_t>(
                        m->occurrence_goal_refined_drift_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("occurrence-goal-refined-drift-total", refined_drift_total);
            insert_kv("occurrence_goal_refined_drift_total", refined_drift_total);
            insert_kv("occurrence-goal-drift-wired", 1);
            insert_kv("schema-2321", 2321);
            insert_kv("issue-2321", 2321);
            insert_kv("coercion-parent-walk-cap-sampled", 16);
            insert_kv("coercion-parent-walk-cap-full", 64);
            insert_kv("coercion-provenance-fast-path-wired", 1);
            insert_kv("schema-2147", 2147);
            insert_kv("issue-2147", 2147);
            // Issue #2512: stamp active mid/pred into
            // CoercionEntry at deferred-add. Raises apply-time
            // fast-path hit rate when TLS would otherwise
            // clear.
            insert_kv("coercion-stamp-at-add-total",
                      static_cast<std::int64_t>(aura::compiler::g_coercion_stamp_at_add_total.load(
                          std::memory_order_relaxed)));
            insert_kv("coercion_stamp_at_add_total",
                      static_cast<std::int64_t>(aura::compiler::g_coercion_stamp_at_add_total.load(
                          std::memory_order_relaxed)));
            insert_kv("coercion-stamp-at-add-wired",
                      static_cast<std::int64_t>(aura::compiler::g_coercion_stamp_at_add_wired.load(
                          std::memory_order_relaxed)));
            insert_kv("schema-2512", 2512);
            insert_kv("issue-2512", 2512);
            // Issue #2991: high-frequency mutate blame completeness.
            insert_kv("schema-2991", 2991);
            insert_kv("issue-2991", 2991);
            // Issue #2992: non-strict ground-type Agent feedback.
            insert_kv("schema-2992", 2992);
            insert_kv("issue-2992", 2992);
            // Issue #2993: type-check metrics tier (minimal default).
            insert_kv("schema-2993", 2993);
            insert_kv("issue-2993", 2993);
            insert_kv("typecheck-metrics-wired", 1);
            insert_kv("typecheck-metrics-hot-path-gated", 1);
            {
                const auto tier =
                    static_cast<std::int64_t>(aura::compiler::typecheck_metrics_tier());
                insert_kv("typecheck-metrics-tier", tier);
                insert_kv("typecheck-metrics-full",
                          aura::compiler::typecheck_metrics_full() ? 1 : 0);
            }
            insert_kv("gradual-permissiveness-wired", 1);
            {
                const std::int64_t gw =
                    m ? static_cast<std::int64_t>(m->gradual_ground_incompatible_warning_total.load(
                            std::memory_order_relaxed))
                      : 0;
                const std::int64_t ge =
                    m ? static_cast<std::int64_t>(m->gradual_ground_incompatible_error_total.load(
                            std::memory_order_relaxed))
                      : 0;
                insert_kv("ground-incompatible-warning-total", gw);
                insert_kv("ground-incompatible-error-total", ge);
            }
            insert_kv("coercion-blame-chain-complete-total",
                      static_cast<std::int64_t>(
                          aura::compiler::g_coercion_blame_chain_complete_total.load(
                              std::memory_order_relaxed)));
            insert_kv("coercion-blame-missing-total",
                      static_cast<std::int64_t>(aura::compiler::g_coercion_blame_missing_total.load(
                          std::memory_order_relaxed)));
            insert_kv(
                "coercion-blame-epoch-restamp-total",
                static_cast<std::int64_t>(aura::compiler::g_coercion_blame_epoch_restamp_total.load(
                    std::memory_order_relaxed)));
            insert_kv(
                "coercion-blame-hf-mutate-wired",
                static_cast<std::int64_t>(aura::compiler::g_coercion_blame_hf_mutate_wired.load(
                    std::memory_order_relaxed)));
            // Issue #2148: precision meet/join lattice
            // observability.
            const std::int64_t meet_prec =
                m ? static_cast<std::int64_t>(
                        m->meet_precision_hit_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t meet_uses =
                m ? static_cast<std::int64_t>(
                        m->and_or_meet_uses_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t join_uses =
                m ? static_cast<std::int64_t>(
                        m->and_or_join_uses_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("meet-precision-hit-total", meet_prec);
            insert_kv("meet_precision_hit_total", meet_prec);
            insert_kv("and-or-meet-uses-total", meet_uses);
            insert_kv("and-or-join-uses-total", join_uses);
            insert_kv("meet-precision-lattice-wired", 1);
            insert_kv("schema-2148", 2148);
            insert_kv("issue-2148", 2148);
            // Issue #2102: provenance miss → force
            // Full/contextual audit or reject.
            const std::int64_t miss_force_audit = static_cast<std::int64_t>(
                aura::compiler::g_coercion_provenance_miss_force_audit_total.load(
                    std::memory_order_relaxed));
            const std::int64_t miss_reject = static_cast<std::int64_t>(
                aura::compiler::g_coercion_provenance_miss_reject_total.load(
                    std::memory_order_relaxed));
            insert_kv("coercion-provenance-miss-force-audit-total", miss_force_audit);
            insert_kv("coercion_provenance_miss_force_audit_total", miss_force_audit);
            insert_kv("coercion-provenance-miss-reject-total", miss_reject);
            insert_kv("force-audit-on-provenance-miss",
                      aura::compiler::force_audit_on_provenance_miss() ? 1 : 0);
            insert_kv("reject-apply-on-provenance-miss",
                      aura::compiler::reject_apply_on_provenance_miss() ? 1 : 0);
            insert_kv("provenance-miss-force-audit-wired", 1);
            insert_kv("schema-2102", 2102);
            insert_kv("issue-2102", 2102);
            // Issue #2185: production defaults force
            // reject-on-miss
            insert_kv("production-defaults-reject-on-miss",
                      aura::compiler::reject_apply_on_provenance_miss() ? 1 : 0);
            insert_kv("coercion-provenance-reject-production-wired", 1);
            insert_kv("schema-2185", 2185);
            insert_kv("issue-2185", 2185);
            // Issue #2558: completeness SLO health (backstop
            // force Full).
            {
                const std::int64_t slo_bp =
                    static_cast<std::int64_t>(aura::compiler::coercion_prov_slo_bp());
                const std::int64_t breach =
                    static_cast<std::int64_t>(aura::compiler::g_coercion_prov_slo_breach_total.load(
                        std::memory_order_relaxed));
                const std::int64_t observe = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_prov_slo_observe_only_total.load(
                        std::memory_order_relaxed));
                const std::int64_t armed = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_prov_slo_force_armed_total.load(
                        std::memory_order_relaxed));
                const std::int64_t consumed = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_prov_slo_force_consumed_total.load(
                        std::memory_order_relaxed));
                insert_kv("coercion-prov-slo-bp", slo_bp);
                insert_kv("coercion-prov-slo-breach-total", breach);
                insert_kv("coercion-prov-slo-observe-only-total", observe);
                insert_kv("coercion-prov-slo-force-armed-total", armed);
                insert_kv("coercion-prov-slo-force-consumed-total", consumed);
                insert_kv("coercion-prov-slo-force-full-pending",
                          aura::compiler::coercion_prov_slo_force_full_pending() ? 1 : 0);
                insert_kv("schema-2558", 2558);
                insert_kv("issue-2558", 2558);
            }
            // Issue #2561: Soft/Sampled blame-chain recover +
            // one-shot Full escalate.
            {
                const std::int64_t recover = static_cast<std::int64_t>(
                    aura::compiler::g_blame_soft_recover_total.load(std::memory_order_relaxed));
                const std::int64_t recover_fail =
                    static_cast<std::int64_t>(aura::compiler::g_blame_soft_recover_fail_total.load(
                        std::memory_order_relaxed));
                const std::int64_t escalate = static_cast<std::int64_t>(
                    aura::compiler::g_blame_soft_escalate_total.load(std::memory_order_relaxed));
                insert_kv("blame-soft-recover-total", recover);
                insert_kv("blame_soft_recover_total", recover);
                insert_kv("blame-soft-recover-fail-total", recover_fail);
                insert_kv("blame_soft_recover_fail_total", recover_fail);
                insert_kv("blame-soft-escalate-total", escalate);
                insert_kv("blame_soft_escalate_total", escalate);
                insert_kv("blame-soft-recover-wired", 1);
                insert_kv("schema-2561", 2561);
                insert_kv("issue-2561", 2561);
            }
            // Issue #2261: Sampled ban weak mid / no
            // CoercionNode pretend stamps
            {
                const std::int64_t sampled_rej = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_provenance_sampled_reject_total.load(
                        std::memory_order_relaxed));
                insert_kv("coercion-provenance-sampled-reject-total", sampled_rej);
                insert_kv("coercion_provenance_sampled_reject_total", sampled_rej);
                insert_kv("coercion-provenance-ban-weak-ir-wired",
                          static_cast<std::int64_t>(
                              aura::compiler::g_coercion_provenance_ban_weak_ir_wired.load(
                                  std::memory_order_relaxed)));
                insert_kv("schema-2261", 2261);
                insert_kv("issue-2261", 2261);
            }
            // Issue #2221: blame-complete optional hard gate on
            // composite commit
            {
                const std::int64_t blame_rej = static_cast<std::int64_t>(
                    aura::compiler::g_blame_commit_reject_total.load(std::memory_order_relaxed));
                const std::int64_t blame_obs = static_cast<std::int64_t>(
                    aura::compiler::g_blame_commit_incomplete_observe_total.load(
                        std::memory_order_relaxed));
                const std::int64_t blame_chk = static_cast<std::int64_t>(
                    aura::compiler::g_blame_commit_check_total.load(std::memory_order_relaxed));
                const std::int64_t m_blame_rej =
                    m ? static_cast<std::int64_t>(
                            m->blame_commit_reject_total.load(std::memory_order_relaxed))
                      : blame_rej;
                insert_kv("require-blame-complete-on-commit",
                          aura::compiler::require_blame_complete_on_commit() ? 1 : 0);
                insert_kv("blame-commit-reject-total", blame_rej);
                insert_kv("blame_commit_reject_total", m_blame_rej);
                insert_kv("blame-commit-incomplete-observe-total", blame_obs);
                insert_kv("blame-commit-check-total", blame_chk);
                insert_kv("blame-commit-require-wired", 1);
                insert_kv("schema-2221", 2221);
                insert_kv("issue-2221", 2221);
            }
            // Issue #2028: stable constraint solver surface
            // metrics
            const std::int64_t sdo_total =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_occurrence_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t sdo_stable =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_occurrence_stable_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t lpi_prov =
                m ? static_cast<std::int64_t>(
                        m->let_poly_instantiate_provenance_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t adt_ren =
                m ? static_cast<std::int64_t>(
                        m->adt_guardshape_selective_renarrow_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t cont_hits =
                m ? static_cast<std::int64_t>(
                        m->cross_delta_solve_continuity_hits_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("solve-delta-occurrence-total", sdo_total);
            insert_kv("solve_delta_occurrence_total", sdo_total);
            insert_kv("solve-delta-occurrence-stable", sdo_stable);
            insert_kv("let-poly-instantiate-provenance", lpi_prov);
            insert_kv("let_poly_instantiate_provenance_total", lpi_prov);
            insert_kv("adt-guardshape-selective-renarrow", adt_ren);
            insert_kv("adt_guardshape_selective_renarrow_total", adt_ren);
            insert_kv("cross-delta-solve-continuity-hits", cont_hits);
            insert_kv("solver-surface-wired", 1);
            insert_kv("solve-delta-occurrence-wired", 1);
            insert_kv("let-poly-instantiate-provenance-wired", 1);
            insert_kv("adt-guardshape-renarrow-wired", 1);
            insert_kv("schema-2028", 2028);
            insert_kv("issue-2028", 2028);
            // Issue #2107: structured TIMEOUT / unresolved
            // export for Agents
            {
                const std::int64_t unr_export =
                    m ? static_cast<std::int64_t>(
                            m->solve_delta_unresolved_export_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t unr_cons =
                    m ? static_cast<std::int64_t>(m->solve_delta_unresolved_constraints_total.load(
                            std::memory_order_relaxed))
                      : 0;
                const std::int64_t to_unr =
                    m ? static_cast<std::int64_t>(
                            m->solve_delta_timeout_unresolved_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t last_n =
                    m ? static_cast<std::int64_t>(
                            m->solve_delta_unresolved_last_count.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t last_unscanned =
                    m ? static_cast<std::int64_t>(
                            m->solve_delta_unscanned_last.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t last_trunc =
                    m ? static_cast<std::int64_t>(
                            m->solve_delta_truncated_reverify_last.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t sample_len =
                    m ? static_cast<std::int64_t>(
                            m->solve_delta_unresolved_affected_sample_len.load(
                                std::memory_order_relaxed))
                      : 0;
                insert_kv("solve-delta-unresolved-export-total", unr_export);
                insert_kv("solve-delta-unresolved-constraints-total", unr_cons);
                insert_kv("solve-delta-timeout-unresolved-total", to_unr);
                insert_kv("solve-delta-unresolved-last-count", last_n);
                insert_kv("solve-delta-unscanned-last", last_unscanned);
                insert_kv("solve-delta-truncated-reverify-last", last_trunc);
                insert_kv("solve-delta-unresolved-affected-"
                          "sample-len",
                          sample_len);
                insert_kv("solve-delta-unresolved-affected-0",
                          m ? static_cast<std::int64_t>(m->solve_delta_unresolved_affected_0.load(
                                  std::memory_order_relaxed))
                            : 0);
                insert_kv("solve-delta-unresolved-affected-1",
                          m ? static_cast<std::int64_t>(m->solve_delta_unresolved_affected_1.load(
                                  std::memory_order_relaxed))
                            : 0);
                insert_kv("solve-delta-unresolved-affected-2",
                          m ? static_cast<std::int64_t>(m->solve_delta_unresolved_affected_2.load(
                                  std::memory_order_relaxed))
                            : 0);
                insert_kv("solve-delta-unresolved-affected-3",
                          m ? static_cast<std::int64_t>(m->solve_delta_unresolved_affected_3.load(
                                  std::memory_order_relaxed))
                            : 0);
                insert_kv("solve-delta-unresolved-export-wired", 1);
                insert_kv("schema-2107", 2107);
                insert_kv("issue-2107", 2107);
                // Issue #2195: goal kind on conflict/timeout
                // export (SUBTYPE=2).
                insert_kv("last-conflict-goal-kind",
                          m ? static_cast<std::int64_t>(
                                  m->last_conflict_goal_kind.load(std::memory_order_relaxed))
                            : 0);
                insert_kv("last-unresolved-goal-kind",
                          m ? static_cast<std::int64_t>(
                                  m->last_unresolved_goal_kind.load(std::memory_order_relaxed))
                            : 0);
                insert_kv("subtype-goal-solve-total",
                          m ? static_cast<std::int64_t>(
                                  m->subtype_goal_solve_total.load(std::memory_order_relaxed))
                            : 0);
                insert_kv("subtype-goal-conflict-total",
                          m ? static_cast<std::int64_t>(
                                  m->subtype_goal_conflict_total.load(std::memory_order_relaxed))
                            : 0);
                insert_kv("subtype-goal-wired", 1);
                insert_kv("schema-2195", 2195);
                insert_kv("issue-2195", 2195);
                // Issue #2607: INSTANCE goal (depth-capped ∀
                // peel + unify).
                insert_kv("instance-unify-total",
                          m ? static_cast<std::int64_t>(
                                  m->instance_unify_total.load(std::memory_order_relaxed))
                            : 0);
                insert_kv("instance-depth-cap-total",
                          m ? static_cast<std::int64_t>(
                                  m->instance_depth_cap_total.load(std::memory_order_relaxed))
                            : 0);
                insert_kv("instance-goal-solve-total",
                          m ? static_cast<std::int64_t>(
                                  m->instance_goal_solve_total.load(std::memory_order_relaxed))
                            : 0);
                insert_kv("instance-goal-conflict-total",
                          m ? static_cast<std::int64_t>(
                                  m->instance_goal_conflict_total.load(std::memory_order_relaxed))
                            : 0);
                insert_kv("instance-depth-cap",
                          static_cast<std::int64_t>(aura::compiler::kInstanceDepthCap));
                insert_kv("instance-goal-wired", 1);
                insert_kv("schema-2607", 2607);
                insert_kv("issue-2607", 2607);
            }
            // Issue #2030: agent blame completeness +
            // occurrence post-mutate hit rate
            {
                const std::uint64_t blame_c =
                    m ? m->blame_chain_complete_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t blame_m =
                    m ? m->blame_propagation_miss_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t blame_den = blame_c + blame_m;
                const std::int64_t blame_ratio_bp =
                    blame_den == 0 ? 10000
                                   : static_cast<std::int64_t>((blame_c * 10000ull) / blame_den);
                const std::uint64_t ren_hits =
                    m ? m->occurrence_renarrow_hits_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t ren_tot =
                    m ? m->occurrence_renarrow_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t stale_r =
                    m ? m->occurrence_stale_refreshes_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t occ_blame =
                    m ? m->occurrence_blame_chain_complete_total.load(std::memory_order_relaxed)
                      : 0;
                std::int64_t occ_hit_bp = 10000;
                if (ren_tot > 0)
                    occ_hit_bp = static_cast<std::int64_t>((ren_hits * 10000ull) / ren_tot);
                else if (stale_r > 0)
                    occ_hit_bp = static_cast<std::int64_t>((occ_blame * 10000ull) / stale_r);
                using namespace aura::compiler::linear_occurrence_mutate;
                const std::uint64_t lin_reval =
                    revalidate_hits_total.load(std::memory_order_relaxed) +
                    (m ? m->linear_occurrence_revalidate_hits_total.load(std::memory_order_relaxed)
                       : 0);
                const std::uint64_t lin_esc =
                    escape_violations_prevented_total.load(std::memory_order_relaxed) +
                    (m ? m->linear_occurrence_escape_prevented_total.load(std::memory_order_relaxed)
                       : 0);
                const std::uint64_t lin_den = lin_reval + lin_esc;
                const std::int64_t lin_occ_bp =
                    lin_den == 0 ? 10000
                                 : static_cast<std::int64_t>((lin_reval * 10000ull) / lin_den);
                insert_kv("blame_completeness_ratio", blame_ratio_bp);
                insert_kv("blame-completeness-ratio-bp", blame_ratio_bp);
                insert_kv("occurrence_narrowing_post_mutate_hit_rate", occ_hit_bp);
                insert_kv("occurrence-narrowing-post-mutate-"
                          "hit-rate-bp",
                          occ_hit_bp);
                insert_kv("linear-occurrence-consistency-bp", lin_occ_bp);
                insert_kv("linear-provenance-consistency-bp",
                          static_cast<std::int64_t>(
                              aura::core::provenance::linear_provenance_consistency_bp()));
                insert_kv("blame-occurrence-ratios-wired", 1);
                insert_kv("schema-2030", 2030);
                insert_kv("issue-2030", 2030);
            }
            // Issue #2260: boundary type-proof hard-gate
            // metrics
            {
                using namespace aura::compiler::typed_audit;
                insert_kv("boundary-solve-hard-gate-total",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.boundary_solve_hard_gate_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("boundary-solve-full-resync-total",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.boundary_solve_full_resync_total.load(
                                  std::memory_order_relaxed)));
                insert_kv(
                    "boundary-solve-force-rollback-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.boundary_solve_force_rollback_total.load(
                            std::memory_order_relaxed)));
                insert_kv(
                    "boundary-solve-truncated-seen-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.boundary_solve_truncated_seen_total.load(
                            std::memory_order_relaxed)));
                insert_kv("boundary-solve-hard-gate-wired",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.boundary_solve_hard_gate_wired.load(
                                  std::memory_order_relaxed)));
                insert_kv("schema-2260", 2260);
                insert_kv("issue-2260", 2260);
            }
            // Issue #2262: partial CS single source of truth
            {
                insert_kv("partial-cs-import-total",
                          static_cast<std::int64_t>(
                              aura::compiler::TypeChecker::partial_cs_import_total()));
                insert_kv("partial-cs-import-skip-total",
                          static_cast<std::int64_t>(
                              aura::compiler::TypeChecker::partial_cs_import_skip_total()));
                insert_kv("partial-cs-hard-empty-miss-total",
                          static_cast<std::int64_t>(
                              aura::compiler::g_partial_cs_hard_empty_miss_total.load(
                                  std::memory_order_relaxed)));
                insert_kv(
                    "partial-cs-single-source-wired",
                    static_cast<std::int64_t>(aura::compiler::g_partial_cs_single_source_wired.load(
                        std::memory_order_relaxed)));
                insert_kv("schema-2262", 2262);
                insert_kv("issue-2262", 2262);
            }
            // Issue #2345: expected-partial empty CS anti
            // false-green (hard vs soft).
            {
                using namespace aura::compiler::typed_audit;
                insert_kv(
                    "composite-commit-empty-cs-hard-miss-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_commit_empty_cs_hard_miss_total
                            .load(std::memory_order_relaxed)));
                insert_kv(
                    "composite-commit-empty-cs-observe-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_commit_empty_cs_observe_total
                            .load(std::memory_order_relaxed)));
                // Lineage #2180 empty-cs total retained.
                insert_kv(
                    "composite-commit-solve-empty-cs-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_commit_solve_empty_cs_total.load(
                            std::memory_order_relaxed)));
                insert_kv("composite-empty-cs-hard-wired",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.composite_empty_cs_hard_wired.load(
                                  std::memory_order_relaxed)));
                insert_kv("schema-2345", 2345);
                insert_kv("issue-2345", 2345);
            }
            // Issue #2509: symmetric expected_partial ↔
            // commit_cs_has_work matrix.
            {
                using namespace aura::compiler::typed_audit;
                insert_kv(
                    "composite-commit-unexpected-cs-work-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_commit_unexpected_cs_work_total
                            .load(std::memory_order_relaxed)));
                insert_kv(
                    "composite-commit-expected-has-work-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_commit_expected_has_work_total
                            .load(std::memory_order_relaxed)));
                insert_kv(
                    "composite-commit-sdo-entered-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_commit_sdo_entered_total.load(
                            std::memory_order_relaxed)));
                insert_kv(
                    "composite-cs-signature-matrix-wired",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_cs_signature_matrix_wired.load(
                            std::memory_order_relaxed)));
                insert_kv("schema-2509", 2509);
                insert_kv("issue-2509", 2509);
                // Issue #2610: auto-detect expected_partial
                // from dirty cone.
                insert_kv("composite-commit-auto-partial-from-cone-"
                          "total",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters
                                  .composite_commit_auto_partial_from_cone_total.load(
                                      std::memory_order_relaxed)));
                insert_kv("composite-commit-auto-partial-from-cone-"
                          "observe-total",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters
                                  .composite_commit_auto_partial_from_cone_observe_total.load(
                                      std::memory_order_relaxed)));
                insert_kv("composite-auto-partial-from-cone-wired", 1);
                insert_kv("commit-readiness-force-reason-auto-"
                          "partial",
                          6);
                insert_kv("schema-2610", 2610);
                insert_kv("issue-2610", 2610);
                // Issue #2898: explicit required TypeId invariant set
                // on composite_txn_commit (anti under-mark false-green).
                insert_kv(
                    "composite-required-type-fail-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_required_type_fail_total.load(
                            std::memory_order_relaxed)));
                insert_kv(
                    "composite-required-type-observe-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_required_type_observe_total.load(
                            std::memory_order_relaxed)));
                insert_kv(
                    "composite-required-type-checked-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_required_type_checked_total.load(
                            std::memory_order_relaxed)));
                insert_kv("composite-required-type-wired", 1);
                insert_kv("commit-readiness-force-reason-required-type", 14);
                insert_kv("schema-2898", 2898);
                insert_kv("issue-2898", 2898);
                // Issue #2983: production default required TypeId auto-fill.
                insert_kv(
                    "composite-required-type-auto-fill-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_required_type_auto_fill_total
                            .load(std::memory_order_relaxed)));
                insert_kv("composite-required-type-auto-fill-capped-total",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters
                                  .composite_required_type_auto_fill_capped_total.load(
                                      std::memory_order_relaxed)));
                insert_kv("composite-required-type-reject-over-infer-total",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters
                                  .composite_required_type_reject_over_infer_total.load(
                                      std::memory_order_relaxed)));
                insert_kv("composite-required-type-auto-fill-wired", 1);
                insert_kv("composite-required-type-auto-fill-cap",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::kCompositeRequiredTypeAutoFillCap));
                insert_kv("schema-2983", 2983);
                insert_kv("issue-2983", 2983);
            }
            // Issue #2458: truncate-commit Soft observe / Hard
            // full-solve-or-reject. Additive keys on
            // fidelity-stats (anti half-green under
            // multi-round).
            {
                using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
                using aura::compiler::typed_audit::truncate_commit_hard_enabled;
                insert_kv("truncate-commit-observe-total",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.truncate_commit_observe_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("truncate-commit-reject-total",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.truncate_commit_reject_total.load(
                                  std::memory_order_relaxed)));
                insert_kv(
                    "truncate-commit-full-solve-recover-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.truncate_commit_full_solve_recover_total
                            .load(std::memory_order_relaxed)));
                insert_kv("truncate-commit-hard-wired",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.truncate_commit_hard_wired.load(
                                  std::memory_order_relaxed)));
                insert_kv("truncate-commit-hard-enabled", truncate_commit_hard_enabled() ? 1 : 0);
                insert_kv("schema-2458", 2458);
                insert_kv("issue-2458", 2458);
            }
            // Issue #2359: unify occurrence_goals +
            // predicate_memo epoch health on the fidelity-stats
            // surface (pure read; no solve side effects).
            // Agents use these keys to decide whether narrowing
            // caches and CS goals are same-generation:
            //   - cache-epoch: TypeChecker / Evaluator current
            //   epoch
            //   - occurrence-goals-live / max-epoch /
            //   stale-vs-epoch
            //   - predicate-memo-live / stale-vs-epoch
            //   - memo-goal-epoch-delta: 0 healthy; >0 lag
            //   (memo stale
            //     + goal survivors past prune boundary)
            // Vacuous healthy (0s) when no commit TypeChecker /
            // memo.
            {
                std::int64_t cache_epoch_v = 0;
                std::int64_t goals_live = 0;
                std::int64_t goals_max_epoch = 0;
                std::int64_t goals_stale = 0;
                std::int64_t memo_live = 0;
                std::int64_t memo_stale = 0;
                if (ev) {
                    cache_epoch_v = static_cast<std::int64_t>(ev->current_cache_epoch());
                    if (auto* ctc = static_cast<aura::compiler::TypeChecker*>(
                            ev->commit_type_checker_handle())) {
                        cache_epoch_v = static_cast<std::int64_t>(ctc->cache_epoch());
                        const auto& cs = ctc->constraint_system();
                        goals_live = static_cast<std::int64_t>(cs.occurrence_goals_size());
                        goals_max_epoch =
                            static_cast<std::int64_t>(cs.occurrence_goals_max_epoch());
                        goals_stale = static_cast<std::int64_t>(
                            cs.occurrence_goals_stale_vs_epoch(ctc->cache_epoch()));
                        // Last partial snapshot (engine is
                        // ephemeral).
                        memo_live = static_cast<std::int64_t>(ctc->last_predicate_memo_live());
                        memo_stale =
                            static_cast<std::int64_t>(ctc->last_predicate_memo_stale_vs_epoch());
                    }
                    // Prefer live guard-path InferenceEngine
                    // when present (memo survives multi-round
                    // Guard exit / selective).
                    if (auto* eng = static_cast<aura::compiler::InferenceEngine*>(
                            ev->guard_infer_engine())) {
                        memo_live = static_cast<std::int64_t>(eng->predicate_memo_size());
                        memo_stale =
                            static_cast<std::int64_t>(eng->predicate_memo_stale_vs_epoch());
                    }
                }
                // Lag signal: memo entries behind cache epoch +
                // goals that would be prune-eligible but still
                // live.
                const std::int64_t delta = memo_stale + goals_stale;
                insert_kv("cache-epoch", cache_epoch_v);
                insert_kv("occurrence-goals-live", goals_live);
                insert_kv("occurrence-goals-max-epoch", goals_max_epoch);
                insert_kv("occurrence-goals-stale-vs-epoch", goals_stale);
                insert_kv("predicate-memo-live", memo_live);
                insert_kv("predicate-memo-stale-vs-epoch", memo_stale);
                insert_kv("memo-goal-epoch-delta", delta);
                insert_kv("memo-goal-epoch-health-wired", 1);
                insert_kv("schema-2359", 2359);
                insert_kv("issue-2359", 2359);
                // Issue #2461: per-If structural cache key
                // hit/miss.
                std::int64_t key_hit = 0;
                std::int64_t key_miss = 0;
                if (m) {
                    key_hit = static_cast<std::int64_t>(
                        m->occurrence_cache_key_hit_total.load(std::memory_order_relaxed));
                    key_miss = static_cast<std::int64_t>(
                        m->occurrence_cache_key_miss_total.load(std::memory_order_relaxed));
                }
                if (ev) {
                    if (auto* eng = static_cast<aura::compiler::InferenceEngine*>(
                            ev->guard_infer_engine())) {
                        // Prefer live engine session counters
                        // when present.
                        key_hit = static_cast<std::int64_t>(eng->occurrence_cache_key_hits());
                        key_miss = static_cast<std::int64_t>(eng->occurrence_cache_key_misses());
                    }
                }
                insert_kv("occurrence-cache-key-hit-total", key_hit);
                insert_kv("occurrence_cache_key_hit_total", key_hit);
                insert_kv("occurrence-cache-key-miss-total", key_miss);
                insert_kv("occurrence_cache_key_miss_total", key_miss);
                insert_kv("occurrence-cache-key-wired", 1);
                insert_kv("schema-2461", 2461);
                insert_kv("issue-2461", 2461);
                // Issue #2622: single dirty-key authority (memo
                // + goals).
                std::int64_t diverge = 0;
                std::int64_t sync_tot = 0;
                std::int64_t fence_joint = 0;
                if (m) {
                    diverge = static_cast<std::int64_t>(
                        m->occurrence_memo_goal_diverge_total.load(std::memory_order_relaxed));
                    sync_tot = static_cast<std::int64_t>(
                        m->occurrence_sync_after_dirty_total.load(std::memory_order_relaxed));
                    fence_joint = static_cast<std::int64_t>(
                        m->occurrence_memo_goal_fence_joint_total.load(std::memory_order_relaxed));
                }
                if (ev) {
                    if (auto* eng = static_cast<aura::compiler::InferenceEngine*>(
                            ev->guard_infer_engine())) {
                        diverge =
                            static_cast<std::int64_t>(eng->occurrence_memo_goal_diverge_total());
                        sync_tot =
                            static_cast<std::int64_t>(eng->occurrence_sync_after_dirty_total());
                    }
                }
                insert_kv("occurrence-memo-goal-diverge-total", diverge);
                insert_kv("occurrence_memo_goal_diverge_total", diverge);
                insert_kv("occurrence-sync-after-dirty-total", sync_tot);
                insert_kv("occurrence_sync_after_dirty_total", sync_tot);
                insert_kv("occurrence-memo-goal-fence-joint-total", fence_joint);
                insert_kv(
                    "occurrence-dirty-key-authority-wired",
                    m ? static_cast<std::int64_t>(
                            m->occurrence_dirty_key_authority_wired.load(std::memory_order_relaxed))
                      : 1);
                insert_kv("schema-2622", 2622);
                insert_kv("issue-2622", 2622);
            }
            insert_kv("issue",
                      1617); // primary lineage (#1617 / #798 /
                             // #1924 / #2028 / #2030)
            insert_kv("schema",
                      1617); // keep 1617 for existing ACs;
                             // #2030 via schema-2030
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2888: query:lifetime-consistency-proof — unified Agent-visible
    // snapshot for "is my live state consistent after the last
    // densify/steal/mutate?" without torn views. Aggregates EnvFrame (#2711)
    // + TypeLinear (#2854) + pin (#2265) + LayoutStamp (#2170) + residual
    // (#2846) into one read-only proof with a single would_allow_commit /
    // force_reason_code. Also surfaces the process-wide last-proof atomic set
    // (stamped on outermost densify success + steal-complete) for
    // high-frequency Agent poll. Additive only — #2711 / #2697 / #2854 / pin
    // stats surfaces preserved (AC4). Pure reads — no counter bumps, no
    // mutate side effects (AC3 quiet path: no extra atomics).
    ObservabilityPrims::register_stats_impl(
        "query:lifetime-consistency-proof",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            // Capacity 96: 16 component keys + 5 axis flags + unified + poll + sentinels.
            auto* ht = FlatHashTable::create(96);
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
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            using aura::core::envframe_lifetime::snapshot_envframe_lifetime_proof;
            using aura::core::lifetime_consistency_proof::last_lifetime_consistency_proof;
            using aura::core::lifetime_consistency_proof::make_lifetime_consistency_proof;
            const auto efl = snapshot_envframe_lifetime_proof();
            aura::core::LayoutStamp layout{};
            if (ev)
                layout = ev->current_layout_stamp();
            const auto proof = make_lifetime_consistency_proof(
                efl.hold_gen, efl.compact_gen, efl.scans_run, efl.densify_scan_total,
                efl.densify_scan_fail, efl.hold_gen_mismatch_total,
                typed_audit::last_type_linear_proof_outcome_v_read(),
                typed_audit::last_proof_linear_root_count_v_read(),
                typed_audit::type_linear_proof_stamped_after_rebind_total_v_read(),
                typed_audit::type_linear_proof_reject_after_rebind_fail_total_v_read(),
                aura::core::lifetime::lifetime_pin_contract_fail_total(),
                aura::core::lifetime::lifetime_pin_remap_miss_total(), layout.arena_gen,
                layout.flat_gen, layout.env_gen, aura::gc_hooks::residual_defer_after_exit_total(),
                efl.mutation_epoch);
            // EnvFrame axis (#2711)
            insert_kv("lifetime-consistency-proof-envframe-hold-gen",
                      static_cast<std::int64_t>(proof.envframe_hold_gen));
            insert_kv("lifetime-consistency-proof-envframe-compact-gen",
                      static_cast<std::int64_t>(proof.envframe_compact_gen));
            insert_kv("lifetime-consistency-proof-envframe-scans-run",
                      static_cast<std::int64_t>(proof.envframe_scans_run));
            insert_kv("lifetime-consistency-proof-envframe-densify-scan-total",
                      static_cast<std::int64_t>(proof.envframe_densify_scan_total));
            insert_kv("lifetime-consistency-proof-envframe-densify-scan-fail",
                      static_cast<std::int64_t>(proof.envframe_densify_scan_fail));
            insert_kv("lifetime-consistency-proof-envframe-hold-gen-mismatch-total",
                      static_cast<std::int64_t>(proof.envframe_hold_gen_mismatch_total));
            // TypeLinear axis (#2854)
            insert_kv("lifetime-consistency-proof-type-linear-outcome",
                      static_cast<std::int64_t>(proof.type_linear_outcome));
            insert_kv("lifetime-consistency-proof-linear-root-count",
                      static_cast<std::int64_t>(proof.type_linear_linear_root_count));
            insert_kv("lifetime-consistency-proof-type-linear-stamped-after-rebind-total",
                      static_cast<std::int64_t>(proof.type_linear_stamped_after_rebind_total));
            insert_kv("lifetime-consistency-proof-type-linear-reject-after-rebind-fail-total",
                      static_cast<std::int64_t>(proof.type_linear_reject_after_rebind_fail_total));
            // Pin axis (#2265 / #2266)
            insert_kv("lifetime-consistency-proof-pin-contract-fail-total",
                      static_cast<std::int64_t>(proof.pin_contract_fail_total));
            insert_kv("lifetime-consistency-proof-pin-remap-miss-total",
                      static_cast<std::int64_t>(proof.pin_remap_miss_total));
            // LayoutStamp axis (#2170)
            insert_kv("lifetime-consistency-proof-layout-arena-gen",
                      static_cast<std::int64_t>(proof.layout_arena_gen));
            insert_kv("lifetime-consistency-proof-layout-flat-gen",
                      static_cast<std::int64_t>(proof.layout_flat_gen));
            insert_kv("lifetime-consistency-proof-layout-env-gen",
                      static_cast<std::int64_t>(proof.layout_env_gen));
            // Residual axis (#2846)
            insert_kv("lifetime-consistency-proof-residual-defer-after-exit-total",
                      static_cast<std::int64_t>(proof.residual_defer_after_exit_total));
            // Unified
            insert_kv("lifetime-consistency-proof-mutation-epoch",
                      static_cast<std::int64_t>(proof.mutation_epoch));
            insert_kv("lifetime-consistency-proof-would-allow-commit",
                      proof.would_allow_commit ? 1 : 0);
            insert_kv("lifetime-consistency-proof-force-reason-code",
                      static_cast<std::int64_t>(proof.force_reason_code));
            // Process-wide last-proof atomic poll (stamp sites publish).
            const auto poll = last_lifetime_consistency_proof();
            insert_kv("lifetime-consistency-proof-last-would-allow-commit",
                      poll.would_allow_commit ? 1 : 0);
            insert_kv("lifetime-consistency-proof-last-force-reason-code",
                      static_cast<std::int64_t>(poll.force_reason_code));
            insert_kv("lifetime-consistency-proof-last-mutation-epoch",
                      static_cast<std::int64_t>(poll.mutation_epoch));
            insert_kv("lifetime-consistency-proof-stamped-total",
                      static_cast<std::int64_t>(poll.stamped_total));
            insert_kv("lifetime-consistency-proof-wired", 1);
            insert_kv("schema-2888", 2888);
            insert_kv("issue-2888",
                      aura::core::lifetime_consistency_proof::kLifetimeConsistencyProofIssue);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2283: query:type-dep-partial-merge-stats. Hash view of the
    // systematic type_dep_graph_ merge for delta-touched TypeIds (CS
    // touched_roots + occurrence vars + rebinding type change). Agents
    // can compute the ratio of partial-merge expansion vs the existing
    // type_dep_graph_affected_expand_total to gauge the cost of the new
    // path under incremental typecheck.
    //   - type-dep-partial-merge-total: # of times the merge was performed
    //   - type-dep-partial-nodes-added: # of nodes added by the merge
    //   - type-dep-graph-affected-expand-total: existing #1528 expansion
    //   - type-dep-merge-ratio-bp: nodes_added * 10000 / affected_expand
    //     (ratio in basis points; 0 when no expansion)
    ObservabilityPrims::register_stats_impl(
        "query:type-dep-partial-merge-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            // Capacity 64: #2283 + #2320 + #2355 + #2516 keys.
            auto* ht = FlatHashTable::create(64);
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
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            const std::int64_t partial_merge_total =
                m ? static_cast<std::int64_t>(
                        m->type_dep_partial_merge_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t partial_nodes_added =
                m ? static_cast<std::int64_t>(
                        m->type_dep_partial_nodes_added.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t affected_expand =
                m ? static_cast<std::int64_t>(
                        m->type_dep_graph_affected_expand_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t merge_ratio_bp =
                affected_expand > 0 ? (partial_nodes_added * 10000) / affected_expand : 0;
            insert_kv("type-dep-partial-merge-total", partial_merge_total);
            insert_kv("type-dep-partial-nodes-added", partial_nodes_added);
            insert_kv("type-dep-graph-affected-expand-total", affected_expand);
            insert_kv("type-dep-merge-ratio-bp", merge_ratio_bp);
            // Issue #2320: prune observability (refine #2283 #387).
            //   - type-dep-graph-prune-total: cumulative count of prune calls
            //     when set_cache_epoch advances (or when prune_type_dep_graph
            //     is invoked from infer_flat_partial entry once per epoch).
            //   - type-dep-graph-entries-dropped: total NodeIds dropped
            //     across all buckets per prune call (cumulative).
            //   - type-dep-graph-cap-evict-total: cumulative count of
            //     per-bucket cap-triggered evictions (AC2 optional cap
            //     path; default OFF when env unset).
            const std::int64_t prune_total =
                m ? static_cast<std::int64_t>(
                        m->type_dep_graph_prune_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t entries_dropped =
                m ? static_cast<std::int64_t>(
                        m->type_dep_graph_entries_dropped.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t cap_evict =
                m ? static_cast<std::int64_t>(
                        m->type_dep_graph_cap_evict_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("type-dep-graph-prune-total", prune_total);
            insert_kv("type-dep-graph-entries-dropped", entries_dropped);
            insert_kv("type_dep-graph-entries-dropped", entries_dropped);
            insert_kv("type-dep-graph-cap-evict-total", cap_evict);
            insert_kv("schema-2320", 2320);
            insert_kv("issue-2320", 2320);
            // Issue #2355: epoch-stale drop + dirty invalidate + size surface.
            const std::int64_t stale_drop =
                m ? static_cast<std::int64_t>(
                        m->type_dep_graph_stale_drop_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t inv_drop =
                m ? static_cast<std::int64_t>(
                        m->type_dep_graph_invalidate_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t dep_size = m ? static_cast<std::int64_t>(m->type_dep_graph_size.load(
                                                  std::memory_order_relaxed))
                                            : 0;
            insert_kv("type-dep-stale-drop-total", stale_drop);
            insert_kv("type_dep_stale_drop_total", stale_drop);
            insert_kv("type-dep-invalidate-total", inv_drop);
            insert_kv("type_dep_invalidate_total", inv_drop);
            insert_kv("type-dep-size", dep_size);
            insert_kv("type_dep_size", dep_size);
            insert_kv("type-dep-epoch-wired", 1);
            insert_kv("schema-2355", 2355);
            insert_kv("issue-2355", 2355);
            // Issue #2552: type_dep side of steal/densify joint fence.
            const std::int64_t steal_td_prune =
                m ? static_cast<std::int64_t>(
                        m->type_dep_steal_prune_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t steal_td_entries =
                m ? static_cast<std::int64_t>(
                        m->type_dep_steal_prune_entries_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("type-dep-steal-prune-total", steal_td_prune);
            insert_kv("type_dep_steal_prune_total", steal_td_prune);
            insert_kv("type-dep-steal-prune-entries-total", steal_td_entries);
            insert_kv("type-dep-steal-densify-fence-wired", 1);
            insert_kv("schema-2552", 2552);
            insert_kv("issue-2552", 2552);
            // Issue #2516: dirty txn order (invalidate → re-infer → mirror).
            const std::int64_t txn_wired =
                m ? static_cast<std::int64_t>(
                        m->type_dirty_txn_order_wired.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t txn_total =
                m ? static_cast<std::int64_t>(
                        m->type_dirty_txn_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t txn_p1 =
                m ? static_cast<std::int64_t>(
                        m->type_dirty_txn_phase1_invalidate_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t txn_p2 =
                m ? static_cast<std::int64_t>(
                        m->type_dirty_txn_phase2_reinfer_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t txn_p3 =
                m ? static_cast<std::int64_t>(
                        m->type_dirty_txn_phase3_mirror_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("type-dirty-txn-order-wired", txn_wired);
            insert_kv("type-dirty-txn-total", txn_total);
            insert_kv("type-dirty-txn-phase1-invalidate-total", txn_p1);
            insert_kv("type-dirty-txn-phase2-reinfer-total", txn_p2);
            insert_kv("type-dirty-txn-phase3-mirror-total", txn_p3);
            insert_kv("schema-2516", 2516);
            insert_kv("issue-2516", 2516);
            // Issue #2560: partial re-infer cone soft/hard SLA (type layer).
            {
                const std::int64_t soft_ov =
                    m ? static_cast<std::int64_t>(
                            m->partial_cone_soft_overflow_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t hard_fb =
                    m ? static_cast<std::int64_t>(
                            m->partial_cone_hard_fallback_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t deg_trunc =
                    m ? static_cast<std::int64_t>(m->partial_cone_type_dep_degree_trunc_total.load(
                            std::memory_order_relaxed))
                      : 0;
                const std::int64_t last_sz =
                    m ? static_cast<std::int64_t>(
                            m->partial_cone_last_size.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t wired =
                    m ? static_cast<std::int64_t>(
                            m->partial_cone_cap_wired.load(std::memory_order_relaxed))
                      : 1;
                insert_kv("partial-cone-soft-overflow-total", soft_ov);
                insert_kv("partial-cone-hard-fallback-total", hard_fb);
                insert_kv("partial-cone-type-dep-degree-trunc-total", deg_trunc);
                insert_kv("partial-cone-last-size", last_sz);
                insert_kv("partial-cone-cap-wired", wired);
                insert_kv("schema-2560", 2560);
                insert_kv("issue-2560", 2560);
                // Issue #2621: cone truncate → commit fidelity (pairs #2560).
                insert_kv("last-partial-cone-truncated",
                          aura::compiler::typed_audit::last_partial_cone_truncated() ? 1 : 0);
                insert_kv("last-partial-cone-dropped",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::last_partial_cone_dropped()));
                insert_kv("last-partial-cone-fanout-trunc",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::last_partial_cone_fanout_trunc()));
                insert_kv("partial-cone-commit-observe-total",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::g_partial_cone_commit_observe_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("partial-cone-commit-reject-total",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::g_partial_cone_commit_reject_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("partial-cone-commit-gate-wired",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::g_partial_cone_commit_gate_wired.load(
                                  std::memory_order_relaxed)));
                insert_kv("schema-2621", 2621);
                insert_kv("issue-2621", 2621);
                // Issue #2672: drift-injection soak for #2646 cone-truncate
                // outside-If invalidate (schema sentinel so #2703/#2704
                // "prior surface preserved" AC rows stay green; helper lives
                // on TypeChecker + Evaluator, counters reuse #2621 atomics).
                insert_kv("schema-2672", 2672);
                insert_kv("issue-2672", 2672);
                // Issue #2694: soft truncated cone silent dependency escalate
                // (additive — pairs #2646 outside-If invalidate + #2672 soak).
                insert_kv("soft-truncated-silent-dep-escalate-total",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::
                                  soft_truncated_silent_dep_escalate_total_v_read()));
                insert_kv("last-soft-truncated-silent-dep-count",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::last_soft_truncated_silent_dep_count()));
                insert_kv("soft-truncated-silent-dep-wired",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::g_soft_truncated_silent_dep_wired.load(
                                  std::memory_order_relaxed)));
                insert_kv("schema-2694", 2694);
                insert_kv("issue-2694", 2694);
                // Issue #2695: unified OwnershipEnv rebind API post-densify /
                // steal / explicit-Agent mutate:rebind (additive — pairs
                // #2673 linear-root consistency scan + #2563 force rollback).
                insert_kv(
                    "ownership-rebind-total",
                    static_cast<std::int64_t>(aura::compiler::ownership_rebind_total_v_read()));
                insert_kv("ownership-rebind-fail-total",
                          static_cast<std::int64_t>(
                              aura::compiler::ownership_rebind_fail_total_v_read()));
                insert_kv(
                    "ownership-rebind-wired",
                    static_cast<std::int64_t>(aura::compiler::ownership_rebind_wired_v_read()));
                insert_kv("ownership-rebind-densify-total",
                          static_cast<std::int64_t>(
                              aura::compiler::ownership_rebind_densify_total_v_read()));
                insert_kv("ownership-rebind-steal-total",
                          static_cast<std::int64_t>(
                              aura::compiler::ownership_rebind_steal_total_v_read()));
                insert_kv("ownership-rebind-explicit-agent-total",
                          static_cast<std::int64_t>(
                              aura::compiler::ownership_rebind_explicit_agent_total_v_read()));
                insert_kv("schema-2695", 2695);
                insert_kv("issue-2695", 2695);
                // Issue #2742: dirty-pin / densify-affected fallback path when
                // linear_roots() is empty (additive observability).
                insert_kv("ownership-rebind-dirty-fallback-total",
                          static_cast<std::int64_t>(
                              aura::compiler::ownership_rebind_dirty_fallback_total_v_read()));
                insert_kv("ownership-rebind-nonempty-span-total",
                          static_cast<std::int64_t>(
                              aura::compiler::ownership_rebind_nonempty_span_total_v_read()));
                insert_kv("schema-2742", 2742);
                insert_kv("issue-2742", 2742);
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2284: query:type-timeout-repair-stats. Hash view of the
    // Agent-first-class TIMEOUT repair surface (structured unresolved_
    // affected_nodes). On SolveResult::TIMEOUT or hard-reject after full-
    // solve failure, the publish site (evaluator_typecheck.cpp:post-solve,
    // evaluator_mutation_boundary.cpp:hard-reject) captures the status +
    // unresolved_count + unresolved_affected_nodes (capped at 16) +
    // truncated_reverify + blame_complete on a durable Agent surface.
    //   - type-timeout-repair-last-status: last SolveResult::Status (0=SOLVED,
    //     1=CONFLICT, 2=TIMEOUT, 99=hard-reject boundary)
    //   - type-timeout-repair-last-unresolved-count: size of sdo.unresolved
    //   - type-timeout-repair-last-unresolved-aff-nodes-count: capped size
    //     of unresolved_affected_nodes (capped at 16)
    //   - type-timeout-repair-last-unresolved-aff-node-N: individual NodeId
    //     slot (N=0..15) for the Agent repair set
    //   - type-timeout-repair-last-truncated-reverify: last sdo.truncated_reverify
    //   - type-timeout-repair-last-blame-complete: last blame.is_complete()
    //   - type-timeout-repair-publish-total: counter of publish calls
    //   - type-timeout-repair-wired: sentinel (=1)
    //   - schema == 2284 (lineage 2284)
    // Issue #2343 (schema-additive): var↔constraint unresolved graph
    //   - type-repair-unresolved-edge-count: exported edges (≤64)
    //   - type-repair-suggested-root-count: top-k roots by degree (≤8)
    //   - type-repair-suggested-root-N: UF rep (N=0..7)
    //   - type-repair-edge-N-{var,cix,kind,lhs,rhs}: query sample N=0..15
    //     Layout: var=UF rep, cix=constraint index, kind=Constraint::Kind,
    //     lhs/rhs=TypeId.index of the constraint endpoints.
    //   - type-repair-graph-export-total / type-repair-graph-wired
    //   - schema-2343 / issue-2343 (=2343)
    ObservabilityPrims::register_stats_impl(
        "query:type-timeout-repair-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            // Capacity must cover #2284 fixed keys + 16 aff-node slots +
            // #2343 graph keys (edge sample 16×5 + 8 roots + meta) +
            // #2548 reason/degree tags + reason enum sentinels.
            auto* ht = FlatHashTable::create(512);
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
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            const std::int64_t last_status =
                m ? static_cast<std::int64_t>(
                        m->type_repair_last_timeout_status.load(std::memory_order_relaxed))
                  : 0;
            const std::uint64_t last_unresolved_count =
                m ? m->type_repair_last_unresolved_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t last_unresolved_aff_nodes_count =
                m ? m->type_repair_last_unresolved_aff_nodes_count.load(std::memory_order_relaxed)
                  : 0;
            const std::int64_t last_truncated_reverify =
                m ? static_cast<std::int64_t>(
                        m->type_repair_last_truncated_reverify.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t last_blame_complete =
                m ? static_cast<std::int64_t>(
                        m->type_repair_last_blame_complete.load(std::memory_order_relaxed))
                  : 0;
            const std::uint64_t publish_total =
                m ? m->type_repair_publish_total.load(std::memory_order_relaxed) : 0;
            insert_kv("type-timeout-repair-last-status", last_status);
            insert_kv("type-timeout-repair-last-unresolved-count",
                      static_cast<std::int64_t>(last_unresolved_count));
            insert_kv("type-timeout-repair-last-unresolved-aff-nodes-count",
                      static_cast<std::int64_t>(last_unresolved_aff_nodes_count));
            insert_kv("type-timeout-repair-last-truncated-reverify", last_truncated_reverify);
            insert_kv("type-timeout-repair-last-blame-complete", last_blame_complete);
            insert_kv("type-timeout-repair-publish-total",
                      static_cast<std::int64_t>(publish_total));
            insert_kv("type-timeout-repair-wired", 1);
            char field_buf[72];
            for (std::size_t i = 0; i < 16; ++i) {
                const std::uint64_t node_id =
                    m ? m->type_repair_last_unresolved_aff_nodes[i].load(std::memory_order_relaxed)
                      : 0;
                std::snprintf(field_buf, sizeof(field_buf),
                              "type-timeout-repair-last-unresolved-aff-node-%zu", i);
                insert_kv(field_buf, static_cast<std::int64_t>(node_id));
            }
            insert_kv("schema", 2284);
            insert_kv("issue", 2284);
            // Issue #2343: additive graph surface (schema-2284 keys intact).
            const std::uint64_t edge_count =
                m ? m->type_repair_unresolved_edge_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t root_count =
                m ? m->type_repair_suggested_root_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t graph_export_total =
                m ? m->type_repair_graph_export_total.load(std::memory_order_relaxed) : 0;
            insert_kv("type-repair-unresolved-edge-count", static_cast<std::int64_t>(edge_count));
            insert_kv("type-repair-suggested-root-count", static_cast<std::int64_t>(root_count));
            insert_kv("type-repair-graph-export-total",
                      static_cast<std::int64_t>(graph_export_total));
            insert_kv("type-repair-graph-wired", 1);
            for (std::size_t i = 0; i < 8; ++i) {
                const std::uint64_t root =
                    m ? m->type_repair_suggested_roots[i].load(std::memory_order_relaxed) : 0;
                std::snprintf(field_buf, sizeof(field_buf), "type-repair-suggested-root-%zu", i);
                insert_kv(field_buf, static_cast<std::int64_t>(root));
                // Issue #2548: structured reason + degree per root.
                const std::uint64_t why =
                    m ? m->type_repair_suggested_root_reasons[i].load(std::memory_order_relaxed)
                      : 0;
                const std::uint64_t deg =
                    m ? m->type_repair_suggested_root_degrees[i].load(std::memory_order_relaxed)
                      : 0;
                std::snprintf(field_buf, sizeof(field_buf), "type-repair-suggested-root-%zu-reason",
                              i);
                insert_kv(field_buf, static_cast<std::int64_t>(why));
                std::snprintf(field_buf, sizeof(field_buf), "type-repair-suggested-root-%zu-degree",
                              i);
                insert_kv(field_buf, static_cast<std::int64_t>(deg));
            }
            // Issue #2548: aggregate reason tags + caps / sentinels.
            insert_kv(
                "type-repair-occurrence-replay-miss-count",
                m ? static_cast<std::int64_t>(
                        m->type_repair_occurrence_replay_miss_count.load(std::memory_order_relaxed))
                  : 0);
            insert_kv("type-repair-let-poly-suggested-count",
                      m ? static_cast<std::int64_t>(m->type_repair_let_poly_suggested_count.load(
                              std::memory_order_relaxed))
                        : 0);
            insert_kv("type-repair-occurrence-suggested-count",
                      m ? static_cast<std::int64_t>(m->type_repair_occurrence_suggested_count.load(
                              std::memory_order_relaxed))
                        : 0);
            insert_kv("type-repair-rich-roots-export-total",
                      m ? static_cast<std::int64_t>(m->type_repair_rich_roots_export_total.load(
                              std::memory_order_relaxed))
                        : 0);
            insert_kv("type-repair-rich-roots-wired", 1);
            insert_kv("type-repair-root-reason-touched", 0);
            insert_kv("type-repair-root-reason-unresolved-endpoint", 1);
            insert_kv("type-repair-root-reason-pending-full", 2);
            insert_kv("type-repair-root-reason-let-poly", 3);
            insert_kv("type-repair-root-reason-occurrence", 4);
            insert_kv("type-repair-root-reason-occurrence-replay-miss", 5);
            // Issue #2607: Instance ranks above occurrence when degrees tie.
            insert_kv("type-repair-root-reason-instance", 6);
            insert_kv("instance-unify-total",
                      m ? static_cast<std::int64_t>(
                              m->instance_unify_total.load(std::memory_order_relaxed))
                        : 0);
            insert_kv("instance-depth-cap-total",
                      m ? static_cast<std::int64_t>(
                              m->instance_depth_cap_total.load(std::memory_order_relaxed))
                        : 0);
            insert_kv("instance-goal-solve-total",
                      m ? static_cast<std::int64_t>(
                              m->instance_goal_solve_total.load(std::memory_order_relaxed))
                        : 0);
            insert_kv("instance-goal-conflict-total",
                      m ? static_cast<std::int64_t>(
                              m->instance_goal_conflict_total.load(std::memory_order_relaxed))
                        : 0);
            insert_kv("instance-depth-cap",
                      static_cast<std::int64_t>(aura::compiler::kInstanceDepthCap));
            insert_kv("instance-goal-wired", 1);
            // Issue #2643: bounded INSTANCE depth-cap repair hint sample
            // (TIMEOUT path only — zero-cost on SOLVED / no INSTANCE).
            // Agents see per-goal depth_used / depth_cap / poly / var_rep /
            // site_node and re-instantiate polymorphic call sites before
            // full solve. Schema-additive (schema-2607 keys intact).
            insert_kv("instance-depth-cap-repair-hint-total",
                      m ? static_cast<std::int64_t>(m->instance_depth_cap_repair_hint_total.load(
                              std::memory_order_relaxed))
                        : 0);
            insert_kv("instance-depth-cap-repair-hint-count",
                      m ? static_cast<std::int64_t>(
                              m->type_repair_instance_hint_count.load(std::memory_order_relaxed))
                        : 0);
            insert_kv("instance-depth-cap-repair-hint-cap",
                      static_cast<std::int64_t>(aura::compiler::kInstanceRepairHintCap));
            insert_kv("instance-depth-cap-repair-hint-wired", 1);
            for (std::size_t i = 0; i < 8; ++i) {
                const std::uint32_t depth_used =
                    m ? m->type_repair_instance_hint_depth_used[i].load(std::memory_order_relaxed)
                      : 0u;
                const std::uint32_t depth_cap =
                    m ? m->type_repair_instance_hint_depth_cap[i].load(std::memory_order_relaxed)
                      : 0u;
                const std::uint32_t poly =
                    m ? m->type_repair_instance_hint_poly[i].load(std::memory_order_relaxed) : 0u;
                const std::uint32_t var_rep =
                    m ? m->type_repair_instance_hint_var_rep[i].load(std::memory_order_relaxed)
                      : 0u;
                const std::uint32_t site_node =
                    m ? m->type_repair_instance_hint_site_node[i].load(std::memory_order_relaxed)
                      : 0u;
                std::snprintf(field_buf, sizeof(field_buf),
                              "instance-depth-cap-repair-hint-%zu-depth-used", i);
                insert_kv(field_buf, static_cast<std::int64_t>(depth_used));
                std::snprintf(field_buf, sizeof(field_buf),
                              "instance-depth-cap-repair-hint-%zu-depth-cap", i);
                insert_kv(field_buf, static_cast<std::int64_t>(depth_cap));
                std::snprintf(field_buf, sizeof(field_buf),
                              "instance-depth-cap-repair-hint-%zu-poly", i);
                insert_kv(field_buf, static_cast<std::int64_t>(poly));
                std::snprintf(field_buf, sizeof(field_buf),
                              "instance-depth-cap-repair-hint-%zu-var-rep", i);
                insert_kv(field_buf, static_cast<std::int64_t>(var_rep));
                std::snprintf(field_buf, sizeof(field_buf),
                              "instance-depth-cap-repair-hint-%zu-site-node", i);
                insert_kv(field_buf, static_cast<std::int64_t>(site_node));
            }
            insert_kv("schema-2643", 2643);
            insert_kv("issue-2643", 2643);
            insert_kv("schema-2607", 2607);
            insert_kv("issue-2607", 2607);
            insert_kv("type-repair-edge-cap", 64);
            insert_kv("type-repair-root-cap", 8);
            insert_kv("schema-2548", 2548);
            insert_kv("issue-2548", 2548);
            for (std::size_t i = 0; i < 16; ++i) {
                const std::uint64_t var =
                    m ? m->type_repair_edge_var[i].load(std::memory_order_relaxed) : 0;
                const std::uint64_t cix =
                    m ? m->type_repair_edge_cix[i].load(std::memory_order_relaxed) : 0;
                const std::uint64_t kind =
                    m ? m->type_repair_edge_kind[i].load(std::memory_order_relaxed) : 0;
                const std::uint64_t lhs =
                    m ? m->type_repair_edge_lhs[i].load(std::memory_order_relaxed) : 0;
                const std::uint64_t rhs =
                    m ? m->type_repair_edge_rhs[i].load(std::memory_order_relaxed) : 0;
                std::snprintf(field_buf, sizeof(field_buf), "type-repair-edge-%zu-var", i);
                insert_kv(field_buf, static_cast<std::int64_t>(var));
                std::snprintf(field_buf, sizeof(field_buf), "type-repair-edge-%zu-cix", i);
                insert_kv(field_buf, static_cast<std::int64_t>(cix));
                std::snprintf(field_buf, sizeof(field_buf), "type-repair-edge-%zu-kind", i);
                insert_kv(field_buf, static_cast<std::int64_t>(kind));
                std::snprintf(field_buf, sizeof(field_buf), "type-repair-edge-%zu-lhs", i);
                insert_kv(field_buf, static_cast<std::int64_t>(lhs));
                std::snprintf(field_buf, sizeof(field_buf), "type-repair-edge-%zu-rhs", i);
                insert_kv(field_buf, static_cast<std::int64_t>(rhs));
            }
            insert_kv("schema-2343", 2343);
            insert_kv("issue-2343", 2343);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #305: query:type-propagation-stats. Returns the
    // sum of 4 TypeId/TypeScheme propagation observability
    // counters from the shared CompilerMetrics struct (EDA
    // hardware optimization / synthesis track):
    //   - type_propagation_runs_        (# of TypePropagationPass
    //     invocations)
    //   - type_propagation_total_        (# of instructions whose
    //     type_id was propagated)
    //   - type_propagation_unknown_      (# of instructions whose
    //     type_id == 0 (unknown) the pass could NOT propagate)
    //   - type_propagation_int_width_    (# of integers whose
    //     inferred bit-width (8/16/32/64) was used by a
    //     downstream pass — the EDA backend key metric)
    //
    // P0: returns an integer = sum of the 4 counters.
    // Follow-up: returns a 4-tuple
    // (runs total unknown int-width) so the AI Agent can
    // compute propagation_rate = total / (total + unknown)
    // and react to low bit-width usage as a hint that the
    // EDA backend needs more type info.
    //
    // Non-duplicative with #550 (query:typed-mutation-stats)
    // — the latter is general; this primitive is the EDA-
    // specific TypePropagation + bit-width observability.
    ObservabilityPrims::register_stats_impl(
        "query:type-propagation-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t runs = m->type_propagation_runs_.load(std::memory_order_relaxed);
            const std::uint64_t total = m->type_propagation_total_.load(std::memory_order_relaxed);
            const std::uint64_t unknown =
                m->type_propagation_unknown_.load(std::memory_order_relaxed);
            const std::uint64_t int_width =
                m->type_propagation_int_width_.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(runs + total + unknown + int_width));
        });

    // Issue #508 / #468: query:dead-coercion-zerooverhead-stats. Hash view
    // of DeadCoercionEliminationPass zero-overhead lifetime counters:
    //   - eliminated: dead_coercion_eliminated_total
    //   - elapsed-us: dead_coercion_elapsed_us_total (#508 timing)
    //   - kept-for-debug: dead_coercion_kept_for_debug_total (#508 blame)
    //   - type-prop-hits: coercion_type_prop_hits_total (Rule 1)
    //   - zerooverhead-wins: coercion_zerooverhead_win_total
    //   - dead-coercion-total: sum of the 5 counters
    //   - dead-coercion-recommendation: 0=ok, 1=review elapsed, 2=debug kept
    ObservabilityPrims::register_stats_impl(
        "query:dead-coercion-zerooverhead-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
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
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            const std::uint64_t eliminated =
                m ? m->dead_coercion_eliminated_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t elapsed =
                m ? m->dead_coercion_elapsed_us_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t kept =
                m ? m->dead_coercion_kept_for_debug_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t type_prop =
                m ? m->coercion_type_prop_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t win =
                m ? m->coercion_zerooverhead_win_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t total = eliminated + elapsed + kept + type_prop + win;
            std::int64_t recommendation = 0;
            if (kept > 0)
                recommendation = 2;
            else if (elapsed > 10000)
                recommendation = 1;
            insert_kv("eliminated", static_cast<std::int64_t>(eliminated));
            insert_kv("elapsed-us", static_cast<std::int64_t>(elapsed));
            insert_kv("kept-for-debug", static_cast<std::int64_t>(kept));
            insert_kv("type-prop-hits", static_cast<std::int64_t>(type_prop));
            insert_kv("zerooverhead-wins", static_cast<std::int64_t>(win));
            insert_kv("dead-coercion-total", static_cast<std::int64_t>(total));
            insert_kv("dead-coercion-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2282 / #2556: query:dead-coercion-layered-stats. Hash view of the
    // 3 layered dead-coercion elision sources (AST identity + IR CastOp DCE
    // + dirty-cone early-out), so Agents can compute "this mutate removed
    // N Casts" without joining multiple schemas:
    //   - dead-coercion-layered-total: ast_elided + ir_elided + dirty_cone_skips
    //   - ast-elided: g_dead_coercion_ast_elided_total (#1425 / #2025)
    //   - ir-elided: dead_coercion_ir_elided_total (#2025 / #2066)
    //   - dirty-cone-skips: dead_coercion_dirty_cone_skips (#2106 / #2556
    //     CastOp sites outside type∪IR cone, or soft empty-cone count)
    //   - ir-narrow-evidence-hits: dead_coercion_ir_narrow_evidence_hits
    //   - pipeline-runs-total: dead_coercion_pipeline_runs_total
    // Issue #2556 additive keys (schema-2556):
    //   - dirty-cone-partial-runs / dirty-cone-cast-sites-scanned / full-scan-runs
    // Components stay individually queryable for additive schema lineage (AC4).
    ObservabilityPrims::register_stats_impl(
        "query:dead-coercion-layered-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            // Capacity 256: base + #2556/#2562/#2611/#2624 + #2674 keys (was 128).
            auto* ht = FlatHashTable::create(256);
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
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            const std::uint64_t ast_elided =
                ::aura::compiler::g_dead_coercion_ast_elided_total.load(std::memory_order_relaxed);
            const std::uint64_t ir_elided =
                ::aura::compiler::opt_registry::dead_coercion_ir_elided_total.load(
                    std::memory_order_relaxed);
            const std::uint64_t dirty_cone_skips =
                ::aura::compiler::opt_registry::dead_coercion_dirty_cone_skips.load(
                    std::memory_order_relaxed);
            const std::uint64_t narrow_evidence =
                ::aura::compiler::opt_registry::dead_coercion_ir_narrow_evidence_hits.load(
                    std::memory_order_relaxed);
            const std::uint64_t pipeline_runs =
                ::aura::compiler::opt_registry::dead_coercion_pipeline_runs_total.load(
                    std::memory_order_relaxed);
            const std::uint64_t partial_runs =
                ::aura::compiler::opt_registry::dead_coercion_dirty_cone_partial_runs.load(
                    std::memory_order_relaxed);
            const std::uint64_t cast_sites_scanned =
                ::aura::compiler::opt_registry::dead_coercion_dirty_cone_cast_sites_scanned.load(
                    std::memory_order_relaxed);
            const std::uint64_t full_scan_runs =
                ::aura::compiler::opt_registry::dead_coercion_full_scan_runs.load(
                    std::memory_order_relaxed);
            const std::uint64_t layered_total = ast_elided + ir_elided + dirty_cone_skips;
            insert_kv("dead-coercion-layered-total", static_cast<std::int64_t>(layered_total));
            insert_kv("ast-elided", static_cast<std::int64_t>(ast_elided));
            insert_kv("ir-elided", static_cast<std::int64_t>(ir_elided));
            insert_kv("dirty-cone-skips", static_cast<std::int64_t>(dirty_cone_skips));
            insert_kv("ir-narrow-evidence-hits", static_cast<std::int64_t>(narrow_evidence));
            insert_kv("pipeline-runs-total", static_cast<std::int64_t>(pipeline_runs));
            // Issue #2556 additive Agent keys (schema-2556).
            insert_kv("schema-2556", 2556);
            insert_kv("dirty-cone-partial-runs", static_cast<std::int64_t>(partial_runs));
            insert_kv("dirty-cone-cast-sites-scanned",
                      static_cast<std::int64_t>(cast_sites_scanned));
            insert_kv("full-scan-runs", static_cast<std::int64_t>(full_scan_runs));
            // Issue #2562: dual-require drop observability (layered dead-coercion
            // + dual gate for Agent pre-check / insert integrity).
            insert_kv(
                "coercion-dual-require-drop-total",
                static_cast<std::int64_t>(aura::compiler::g_coercion_dual_require_drop_total.load(
                    std::memory_order_relaxed)));
            insert_kv("coercion-dual-require-enabled",
                      aura::compiler::coercion_dual_require_active() ? 1 : 0);
            insert_kv("coercion-dual-require-wired", 1);
            insert_kv("schema-2562", 2562);
            insert_kv("issue-2562", 2562);
            // Issue #2611: elided CastOp deopt meta (mid + narrow_evidence + type_tag).
            // last-* atomics are stamped at elision; expose_last_deopt_meta (deopt path /
            // tests) bumps deopt-meta-deopt-expose-total for Agent join (AC1).
            {
                using namespace ::aura::compiler::dce_deopt;
                insert_kv("schema-2611", 2611);
                insert_kv("issue-2611", 2611);
                insert_kv("deopt-meta-stamped-total",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_stamped_total.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-map-size",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_map_size.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-skipped-no-evidence",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_skipped_no_evidence.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-lookup-hits",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_lookup_hits.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-deopt-expose-total",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_deopt_expose_total.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-last-mid",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_last_mid.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-last-evidence",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_last_evidence.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-last-type-tag",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_last_type_tag.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-last-site-key",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_last_site_key.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-wired", 1);
            }
            // Issue #2624 Phase A: CastOp typed meta side table (src/dst/evidence).
            // Not persisted in IR cache; missing on legacy IR → missing_total only.
            // Phase B executor Strict / Phase C JIT deopt are out of scope.
            {
                using namespace ::aura::compiler::castop_meta;
                insert_kv("schema-2624", 2624);
                insert_kv("issue-2624", 2624);
                insert_kv("castop-typed-meta-stamped-total",
                          static_cast<std::int64_t>(
                              castop_typed_meta_stamped_total.load(std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-missing-total",
                          static_cast<std::int64_t>(
                              castop_typed_meta_missing_total.load(std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-map-size",
                          static_cast<std::int64_t>(
                              castop_typed_meta_map_size.load(std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-lookup-hits",
                          static_cast<std::int64_t>(
                              castop_typed_meta_lookup_hits.load(std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-identity-elide-total",
                          static_cast<std::int64_t>(castop_typed_meta_identity_elide_total.load(
                              std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-last-src",
                          static_cast<std::int64_t>(
                              castop_typed_meta_last_src.load(std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-last-dst",
                          static_cast<std::int64_t>(
                              castop_typed_meta_last_dst.load(std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-last-evidence",
                          static_cast<std::int64_t>(
                              castop_typed_meta_last_evidence.load(std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-wired",
                          static_cast<std::int64_t>(
                              castop_typed_meta_wired.load(std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-phase-a",
                          static_cast<std::int64_t>(
                              castop_typed_meta_phase_a.load(std::memory_order_relaxed)));
            }
            // Issue #2674: layered evidence-coherence invariant — observe-only
            // divergence counter under Soft/Sampled (no hard-reject of mutate
            // by default per AC5). Agent-visible ast-elided-with-evidence counter
            // pairs with ir-narrow-evidence-hits + deopt-meta-stamped-total so
            // dashboards can detect layered-stats divergence under typed_mutate
            // → lower → JIT paths. Zero cost when no evidence path (pure atomic
            // loads on the read side; coherence check runs on MutationBoundary
            // outermost exit in evaluator_mutation_boundary.cpp).
            {
                insert_kv("schema-2674", 2674);
                insert_kv("issue-2674", 2674);
                insert_kv("ast-elided-with-evidence",
                          static_cast<std::int64_t>(
                              ::aura::compiler::g_dead_coercion_ast_elided_with_evidence_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("layered-evidence-diverge-total",
                          static_cast<std::int64_t>(
                              ::aura::compiler::g_layered_evidence_diverge_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("layered-evidence-coherence-wired", 1);
                // Issue #2719 / #2912: Full/production hard gate on
                // layered evidence diverge (#2674 residual). Default
                // production arm: force-Full on next boundary (fidelity-
                // health note, not a hard-reject of current commit).
                // #2912: boundary *consumes* force-full-pending → Full
                // audit (dual-complete + provenance recover). Opt-in
                // HARD env → hard-reject consume with force_reason.
                // Soft/Sampled: observe-only (#2674). Additive keys.
                insert_kv("layered-evidence-diverge-force-armed-total",
                          static_cast<std::int64_t>(
                              ::aura::compiler::g_layered_evidence_diverge_force_armed_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("layered-evidence-diverge-hard-reject-total",
                          static_cast<std::int64_t>(
                              ::aura::compiler::g_layered_evidence_diverge_hard_reject_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("layered-evidence-diverge-force-full-pending",
                          ::aura::compiler::g_layered_evidence_diverge_force_full_pending.load(
                              std::memory_order_relaxed) != 0
                              ? 1
                              : 0);
                insert_kv("layered-evidence-diverge-hard-reject-pending",
                          ::aura::compiler::g_layered_evidence_diverge_hard_reject_pending.load(
                              std::memory_order_relaxed) != 0
                              ? 1
                              : 0);
                insert_kv("layered-evidence-diverge-hard-wired",
                          ::aura::compiler::layered_diverge_hard_enabled() ? 1 : 0);
                insert_kv("schema-2719", 2719);
                insert_kv("issue-2719", 2719);
                // Issue #2912: consume totals + force_reason sentinel.
                insert_kv(
                    "layered-evidence-diverge-force-consumed-total",
                    static_cast<std::int64_t>(
                        ::aura::compiler::g_layered_evidence_diverge_force_consumed_total.load(
                            std::memory_order_relaxed)));
                insert_kv(
                    "layered-evidence-diverge-hard-reject-consumed-total",
                    static_cast<std::int64_t>(
                        ::aura::compiler::g_layered_evidence_diverge_hard_reject_consumed_total
                            .load(std::memory_order_relaxed)));
                insert_kv("layered-evidence-diverge-force-full-consume-wired", 1);
                insert_kv("schema-2912", 2912);
                insert_kv("issue-2912", 2912);
                // Issue #2979: Phase-5 outermost consume + Full sample.
                insert_kv(
                    "layered-evidence-diverge-force-full-sample-total",
                    static_cast<std::int64_t>(
                        ::aura::compiler::g_layered_evidence_diverge_force_full_sample_total.load(
                            std::memory_order_relaxed)));
                insert_kv("layered-evidence-diverge-phase5-consume-wired", 1);
                insert_kv("schema-2979", 2979);
                insert_kv("issue-2979", 2979);
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2558 / #2561 / #2648: query:coercion-provenance-health — completeness
    // SLO + Soft blame recover/escalate + Soft evidence-loss bp for Agents.
    ObservabilityPrims::register_stats_impl(
        "query:coercion-provenance-health",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            // #2648 evidence-loss keys need headroom beyond the #2558/#2561 set.
            auto* ht = FlatHashTable::create(48);
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
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("schema-2558", 2558);
            insert_kv(
                "completeness-bp",
                static_cast<std::int64_t>(aura::compiler::coercion_provenance_completeness_bp()));
            insert_kv("miss-total", static_cast<std::int64_t>(
                                        aura::compiler::g_coercion_provenance_miss_total.load(
                                            std::memory_order_relaxed)));
            insert_kv(
                "complete-total",
                static_cast<std::int64_t>(aura::compiler::g_coercion_provenance_complete_total.load(
                    std::memory_order_relaxed)));
            insert_kv("slo-bp", static_cast<std::int64_t>(aura::compiler::coercion_prov_slo_bp()));
            insert_kv("slo-breach-total", static_cast<std::int64_t>(
                                              aura::compiler::g_coercion_prov_slo_breach_total.load(
                                                  std::memory_order_relaxed)));
            insert_kv("slo-observe-only-total",
                      static_cast<std::int64_t>(
                          aura::compiler::g_coercion_prov_slo_observe_only_total.load(
                              std::memory_order_relaxed)));
            insert_kv("force-full-pending",
                      aura::compiler::coercion_prov_slo_force_full_pending() ? 1 : 0);
            insert_kv("force-armed-total",
                      static_cast<std::int64_t>(
                          aura::compiler::g_coercion_prov_slo_force_armed_total.load(
                              std::memory_order_relaxed)));
            insert_kv("force-consumed-total",
                      static_cast<std::int64_t>(
                          aura::compiler::g_coercion_prov_slo_force_consumed_total.load(
                              std::memory_order_relaxed)));
            insert_kv("stamp-at-add-total",
                      static_cast<std::int64_t>(aura::compiler::g_coercion_stamp_at_add_total.load(
                          std::memory_order_relaxed)));
            // Issue #2561: Soft/Sampled blame recover + escalate (additive).
            insert_kv("blame-soft-recover-total",
                      static_cast<std::int64_t>(aura::compiler::g_blame_soft_recover_total.load(
                          std::memory_order_relaxed)));
            insert_kv(
                "blame-soft-recover-fail-total",
                static_cast<std::int64_t>(aura::compiler::g_blame_soft_recover_fail_total.load(
                    std::memory_order_relaxed)));
            insert_kv("blame-soft-escalate-total",
                      static_cast<std::int64_t>(aura::compiler::g_blame_soft_escalate_total.load(
                          std::memory_order_relaxed)));
            insert_kv("schema-2561", 2561);
            insert_kv("issue-2561", 2561);
            // Issue #2562: dual-require gate keys (completeness pre-check).
            insert_kv(
                "coercion-dual-require-drop-total",
                static_cast<std::int64_t>(aura::compiler::g_coercion_dual_require_drop_total.load(
                    std::memory_order_relaxed)));
            insert_kv("coercion-dual-require-enabled",
                      aura::compiler::coercion_dual_require_active() ? 1 : 0);
            insert_kv("schema-2562", 2562);
            insert_kv("issue-2562", 2562);
            // Issue #2648: Soft evidence-loss rate + force arm/consume (single Agent face).
            {
                const std::int64_t loss_bp =
                    static_cast<std::int64_t>(aura::compiler::coercion_evidence_loss_bp());
                const std::int64_t thr = static_cast<std::int64_t>(
                    aura::compiler::coercion_evidence_loss_threshold_bp());
                insert_kv("coercion-evidence-loss-bp", loss_bp);
                insert_kv("coercion-evidence-loss-threshold-bp", thr);
                insert_kv("coercion-evidence-loss-force-armed",
                          static_cast<std::int64_t>(
                              aura::compiler::g_coercion_evidence_loss_force_armed_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("coercion-evidence-loss-force-consumed",
                          static_cast<std::int64_t>(
                              aura::compiler::g_coercion_evidence_loss_force_consumed_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("coercion-evidence-loss-breach-total",
                          static_cast<std::int64_t>(
                              aura::compiler::g_coercion_evidence_loss_breach_total.load(
                                  std::memory_order_relaxed)));
                insert_kv(
                    "coercion-evidence-loss-wired",
                    static_cast<std::int64_t>(aura::compiler::g_coercion_evidence_loss_wired.load(
                        std::memory_order_relaxed)));
                insert_kv("schema-2648", 2648);
                insert_kv("issue-2648", 2648);
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2557: query:lock-order-audit-stats — soft audit active flag +
    // inversion counters for production Restricted default. Additive schema
    // (mode: 1=off 2=soft 3=hard; production-soft-active 0/1).
    ObservabilityPrims::register_stats_impl(
        "query:lock-order-audit-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
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
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            using namespace ::aura::compiler::lock_order;
            insert_kv("schema-2557", 2557);
            insert_kv("mode", static_cast<std::int64_t>(lock_order_mode()));
            insert_kv("soft-active",
                      lock_order_audit_enabled() && !lock_order_canary_enabled() ? 1 : 0);
            insert_kv("canary-active", lock_order_canary_enabled() ? 1 : 0);
            insert_kv("production-soft-active", lock_order_production_soft_active() ? 1 : 0);
            insert_kv("inversion-detected-total",
                      static_cast<std::int64_t>(
                          g_lock_inversion_detected_total.load(std::memory_order_relaxed)));
            insert_kv("violation-total",
                      static_cast<std::int64_t>(
                          g_lock_order_violation_total.load(std::memory_order_relaxed)));
            insert_kv("acquire-total", static_cast<std::int64_t>(g_lock_order_acquire_total.load(
                                           std::memory_order_relaxed)));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #629: query:coercion-zerooverhead-stats. Returns the
    // sum of the 4 zero-overhead coercion lifetime counters:
    //   - coercion_castop_emitted_total (TypeSpec CastOp inserts)
    //   - coercion_type_prop_hits_total (DCE Rule 1 elisions)
    //   - coercion_narrow_evidence_hits_total (DCE Rule 6 +
    //     TypeSpec narrow skips + GuardShape fast-path)
    //   - coercion_zerooverhead_win_total (per-run wins)
    ObservabilityPrims::register_stats_impl(
        "query:coercion-zerooverhead-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t castop =
                m->coercion_castop_emitted_total.load(std::memory_order_relaxed);
            const std::uint64_t type_prop =
                m->coercion_type_prop_hits_total.load(std::memory_order_relaxed);
            const std::uint64_t narrow =
                m->coercion_narrow_evidence_hits_total.load(std::memory_order_relaxed);
            const std::uint64_t win =
                m->coercion_zerooverhead_win_total.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(castop + type_prop + narrow + win));
        });

    // Issue #2287: query:castop-density-stats. Hash view of the Dynamic
    // CastOp density budget + Agent annotation hint surface. Non-blocking
    // hint — when last_castop_density_bp > castop_density_budget_bp, the
    // Agent sees castop-annotation-hint=1 and can prefer annotations over
    // blind Dynamic. density = 10000 * castop_emitted / max(1, insts);
    // budget is env AURA_CASTOP_DENSITY_BUDGET_BP (default 1500 = 15%).
    // Additive to #2282 layered stats (uses residual emitted, not elided).
}

} // namespace aura::compiler::primitives_detail
