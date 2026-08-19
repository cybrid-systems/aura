// evaluator_primitives_query_reflect.cpp — Issue #2914 peel (~L9976-L12457)
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

void register_query_reflect_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                       std::pmr::vector<std::string>& string_heap,
                                       void*& type_registry, ModulePathResolver resolve_module_path,
                                       Evaluator& ev) {
    (void)pairs;
    (void)string_heap;
    (void)type_registry;
    (void)resolve_module_path;
    (void)ev;
    ObservabilityPrims::register_stats_impl(
        "query:castop-density-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            // Capacity 64: #2287 + #2319 + #2358 + #2459 keys.
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
            const std::uint64_t dens = m->last_castop_density_bp.load(std::memory_order_relaxed);
            const std::uint64_t budget =
                m->castop_density_budget_bp.load(std::memory_order_relaxed);
            const std::uint64_t over_budget =
                m->castop_density_over_budget_total.load(std::memory_order_relaxed);
            insert_kv("castop-density-bp", static_cast<std::int64_t>(dens));
            insert_kv("castop-density-budget-bp", static_cast<std::int64_t>(budget));
            insert_kv("castop-density-over-budget-total", static_cast<std::int64_t>(over_budget));
            // Agent hint: 1 when last density exceeds budget, else 0. Soft
            // (non-blocking) — #2108 hard-block stays unchanged.
            insert_kv("castop-annotation-hint", dens > budget ? 1 : 0);
            // Issue #2319: opt-in hard gate (refine #2287 soft hint).
            //   - castop-density-hard-reject-total: cumulative count of
            //     hard/soft gate firings when AURA_CASTOP_DENSITY_HARD=1
            //     + density > budget + dirty scope has unannotated
            //     Dynamic binding. Read-only observability.
            //   - castop-density-hard-wired: sentinel = 1 when the hard
            //     gate is integrated (production default OFF; env-gated
            //     Soft default).
            const std::int64_t hard_reject =
                m ? static_cast<std::int64_t>(
                        m->castop_density_hard_reject_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hard_wired =
                m ? static_cast<std::int64_t>(
                        m->castop_density_hard_wired.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("castop-density-hard-reject-total", hard_reject);
            insert_kv("castop_density_hard_reject_total", hard_reject);
            insert_kv("castop-density-hard-wired", hard_wired);
            insert_kv("castop_density_hard_wired", hard_wired);
            insert_kv("schema-2319", 2319);
            insert_kv("issue-2319", 2319);
            // Issue #2358: HARD policy force-JIT action (codegen, not type gate).
            //   - castop-density-hard-enabled: env AURA_CASTOP_DENSITY_HARD (0/1)
            //   - castop-density-hard-action-total: force-JIT fires under HARD=1
            //   - schema-2358 / issue-2358 / wired sentinel
            const std::int64_t hard_enabled =
                m ? static_cast<std::int64_t>(
                        m->castop_density_hard_enabled.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hard_action =
                m ? static_cast<std::int64_t>(
                        m->castop_density_hard_action_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("castop-density-hard-enabled", hard_enabled);
            insert_kv("castop_density_hard_enabled", hard_enabled);
            insert_kv("castop-density-hard-action-total", hard_action);
            insert_kv("castop_density_hard_action_total", hard_action);
            insert_kv("castop-density-hard-action-wired", 1);
            insert_kv("schema-2358", 2358);
            insert_kv("issue-2358", 2358);
            // Issue #2459: production closed-loop streak + gate reject.
            //   - castop-density-streak: consecutive over-budget unannotated
            //   - castop-density-gate-reject-total: MutateTypeGate-aligned
            //   - castop-density-production-default-wired: policy present
            //   - schema-2458 / issue-2458
            const std::int64_t streak =
                static_cast<std::int64_t>(m->castop_density_streak.load(std::memory_order_relaxed));
            const std::int64_t gate_rej = static_cast<std::int64_t>(
                m->castop_density_gate_reject_total.load(std::memory_order_relaxed));
            const std::int64_t soft_warn = static_cast<std::int64_t>(
                m->castop_density_soft_warn_total.load(std::memory_order_relaxed));
            const std::int64_t prod_wired = static_cast<std::int64_t>(
                m->castop_density_production_default_wired.load(std::memory_order_relaxed));
            insert_kv("castop-density-streak", streak);
            insert_kv("castop_density_streak", streak);
            insert_kv("castop-density-gate-reject-total", gate_rej);
            insert_kv("castop_density_gate_reject_total", gate_rej);
            insert_kv("castop-density-soft-warn-total", soft_warn);
            insert_kv("castop-density-production-default-wired", prod_wired);
            insert_kv("castop_density_production_default_wired", prod_wired);
            insert_kv("schema-2459", 2459);
            insert_kv("issue-2459", 2459);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2613: query:type-linear-commit-health — single Agent fold of
    // commit_readiness (#2553) × coercion SLO (#2558) × linear force (#2545/#2563)
    // × occurrence/memo stale (#2359). Pure aggregation (AC4: no commit policy change).
    ObservabilityPrims::register_stats_impl(
        "query:type-linear-commit-health",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();

            TypeLinearCommitHealthSnapshot snap = type_linear_commit_health_live_base();
            // #2558 coercion completeness + SLO force pending.
            snap.coercion_completeness_bp = coercion_provenance_completeness_bp();
            snap.coercion_slo_force_pending = coercion_prov_slo_force_full_pending();
            // #2648 Soft evidence-loss bp (single Agent throttle face).
            snap.coercion_evidence_loss_bp = coercion_evidence_loss_bp();
            snap.coercion_evidence_loss_pressure =
                coercion_evidence_loss_pressure(snap.coercion_evidence_loss_bp);
            // #2359 occurrence goals + predicate memo stale (vacuous 0 without TC).
            if (__qev_) {
                if (auto* ctc = static_cast<aura::compiler::TypeChecker*>(
                        __qev_->commit_type_checker_handle())) {
                    const auto& cs = ctc->constraint_system();
                    snap.occurrence_stale = static_cast<std::uint64_t>(
                        cs.occurrence_goals_stale_vs_epoch(ctc->cache_epoch()));
                    snap.predicate_memo_stale =
                        static_cast<std::uint64_t>(ctc->last_predicate_memo_stale_vs_epoch());
                }
                if (auto* eng = static_cast<aura::compiler::InferenceEngine*>(
                        __qev_->guard_infer_engine())) {
                    snap.predicate_memo_stale =
                        static_cast<std::uint64_t>(eng->predicate_memo_stale_vs_epoch());
                }
            }

            const auto scored = compute_type_linear_commit_health(snap);

            // Issue #3020: ~73 live keys; next_pow2(planned*2) (256).
            constexpr std::size_t kTypeLinearCommitHealthPlannedKeys = 160;
            auto* ht =
                FlatHashTable::create(query_hash_capacity_for(kTypeLinearCommitHealthPlannedKeys));
            if (!ht)
                return make_void();
            bool overflowed = false;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                if (!insert_kv_checked(ht, string_heap, k_str, v))
                    overflowed = true;
            };

            // #2553 readiness face
            insert_kv("readiness-bp", static_cast<std::int64_t>(scored.readiness_bp));
            insert_kv("force-reason", scored.force_reason_code);
            insert_kv("force-reason-code", scored.force_reason_code);
            insert_kv("would-allow-commit", scored.would_allow_commit ? 1 : 0);
            // #2558 coercion
            insert_kv("coercion-completeness-bp",
                      static_cast<std::int64_t>(scored.coercion_completeness_bp));
            insert_kv("coercion-slo-force-pending", scored.coercion_slo_force_pending ? 1 : 0);
            // #2648 Soft evidence-loss (single bp; no multi-key join)
            insert_kv("coercion-evidence-loss-bp",
                      static_cast<std::int64_t>(scored.coercion_evidence_loss_bp));
            insert_kv("coercion-evidence-loss-pressure",
                      scored.coercion_evidence_loss_pressure ? 1 : 0);
            insert_kv("coercion-evidence-loss-force-armed",
                      static_cast<std::int64_t>(g_coercion_evidence_loss_force_armed_total.load(
                          std::memory_order_relaxed)));
            insert_kv("coercion-evidence-loss-force-consumed",
                      static_cast<std::int64_t>(g_coercion_evidence_loss_force_consumed_total.load(
                          std::memory_order_relaxed)));
            // #2545 / #2563 linear
            insert_kv("linear-force-unified", scored.linear_force_unified ? 1 : 0);
            insert_kv("linear-cross-closure-escape-total",
                      static_cast<std::int64_t>(scored.linear_cross_closure_escape_total));
            insert_kv("linear-cross-closure-force-total",
                      static_cast<std::int64_t>(scored.linear_cross_closure_force_total));
            insert_kv("linear-cross-closure-observe-total",
                      static_cast<std::int64_t>(scored.linear_cross_closure_observe_total));
            // #2359 occurrence / memo
            insert_kv("occurrence-stale", static_cast<std::int64_t>(scored.occurrence_stale));
            insert_kv("predicate-memo-stale",
                      static_cast<std::int64_t>(scored.predicate_memo_stale));
            // Advisory throttle (optional orch map)
            insert_kv("throttle-action", scored.throttle_action); // 0 none 1 delay 2 split
            // Issue #3091: additive proof audit_mid (TypeLinearCommitProof.audit_mid,
            // stamped from TLS boundary-noted mid / g_last_stamped_audit_mid).
            // Agent reads same mid as Typed trail / SE so proof ↔ SE ↔ trail
            // join via single key. Soft / no boundary → 0. Abort → 0.
            insert_kv("audit-mid",
                      static_cast<std::int64_t>(
                          typed_audit::g_last_stamped_audit_mid.load(std::memory_order_relaxed)));
            insert_kv("schema-3091", 3091);
            insert_kv("issue-3091", 3091);
            // force-reason code legend
            insert_kv("force-reason-ok", 0);
            insert_kv("force-reason-solve", 1);
            insert_kv("force-reason-blame", 2);
            insert_kv("force-reason-linear", 3);
            insert_kv("force-reason-truncate", 4);
            insert_kv("force-reason-empty-cs", 5);
            insert_kv("force-reason-auto-partial", 6);
            insert_kv("force-reason-coercion-slo", 7);
            insert_kv("force-reason-occurrence-stale", 8);
            insert_kv("force-reason-coercion-evidence-loss", 9);
            insert_kv("force-reason-refined-drift", 15); // #2911
            insert_kv("type-linear-commit-health-wired", 1);
            // Issue #2979: layered-evidence Phase-5 consume face (additive).
            insert_kv("layered-evidence-diverge-force-full-sample-total",
                      static_cast<std::int64_t>(
                          ::aura::compiler::g_layered_evidence_diverge_force_full_sample_total.load(
                              std::memory_order_relaxed)));
            insert_kv("layered-evidence-diverge-phase5-consume-wired", 1);
            insert_kv("schema-2979", 2979);
            insert_kv("issue-2979", 2979);
            // Issue #2981: same-txn proof bind on empty-after-fence miss.
            insert_kv("type-linear-proof-reject-empty-after-fence-total",
                      static_cast<std::int64_t>(
                          aura::compiler::typed_audit::
                              type_linear_proof_reject_empty_after_fence_total_v_read()));
            insert_kv("type-linear-proof-empty-after-fence-wired", 1);
            insert_kv("force-reason-occurrence-empty-after-fence", 11);
            insert_kv("schema-2981", 2981);
            insert_kv("issue-2981", 2981);
            // Issue #2984: compact vs last proof linear_root_count.
            insert_kv("linear-compact-root-check-total",
                      static_cast<std::int64_t>(
                          aura::compiler::typed_audit::linear_compact_root_check_total_v_read()));
            insert_kv(
                "linear-compact-root-mismatch-observe-total",
                static_cast<std::int64_t>(aura::compiler::typed_audit::
                                              linear_compact_root_mismatch_observe_total_v_read()));
            insert_kv(
                "linear-compact-root-mismatch-total",
                static_cast<std::int64_t>(
                    aura::compiler::typed_audit::linear_compact_root_mismatch_total_v_read()));
            insert_kv("linear-compact-root-mismatch-wired", 1);
            insert_kv("schema-2984", 2984);
            insert_kv("issue-2984", 2984);
            insert_kv("schema-2673", 2673);
            insert_kv("schema-2899", 2899);
            insert_kv("schema-2908", 2908);
            insert_kv("commit-readiness-wired", 1); // #2553 lineage face
            insert_kv("schema-2613", kTypeLinearCommitHealthIssue);
            insert_kv("issue-2613", kTypeLinearCommitHealthIssue);
            // Lineage schemas (detailed queries remain authoritative)
            insert_kv("schema-2553", 2553);
            insert_kv("schema-2558", 2558);
            insert_kv("schema-2545", 2545);
            insert_kv("schema-2563", 2563);
            insert_kv("schema-2359", 2359);
            insert_kv("schema-2648", 2648);
            insert_kv("issue-2648", 2648);
            // Issue #2911: unified refined-consistency hard gate face.
            {
                using aura::compiler::typed_audit::kRefinedConsistencyGateIssue;
                using aura::compiler::typed_audit::refined_consistency_drift_face_hit;
                using aura::compiler::typed_audit::refined_consistency_observe_total_v_read;
                using aura::compiler::typed_audit::refined_consistency_recover_total_v_read;
                using aura::compiler::typed_audit::refined_consistency_reject_total_v_read;
                using aura::compiler::typed_audit::refined_consistency_wired_v_read;
                insert_kv("refined-consistency-wired",
                          static_cast<std::int64_t>(refined_consistency_wired_v_read()));
                insert_kv("refined-consistency-observe-total",
                          static_cast<std::int64_t>(refined_consistency_observe_total_v_read()));
                insert_kv("refined-consistency-reject-total",
                          static_cast<std::int64_t>(refined_consistency_reject_total_v_read()));
                insert_kv("refined-consistency-recover-total",
                          static_cast<std::int64_t>(refined_consistency_recover_total_v_read()));
                insert_kv("refined-consistency-face", refined_consistency_drift_face_hit() ? 1 : 0);
                insert_kv("schema-2911", kRefinedConsistencyGateIssue);
                insert_kv("issue-2911", kRefinedConsistencyGateIssue);
            }
            // Issue #2995: unified OccurrenceCommitHealth (single Agent fold).
            {
                using aura::compiler::typed_audit::g_occurrence_commit_health_faces;
                using aura::compiler::typed_audit::g_occurrence_commit_health_fingerprint_ok;
                using aura::compiler::typed_audit::g_occurrence_commit_health_goals_live;
                using aura::compiler::typed_audit::g_occurrence_commit_health_needs_recover;
                using aura::compiler::typed_audit::g_occurrence_commit_health_persist_size;
                using aura::compiler::typed_audit::kOccurrenceCommitHealthIssue;
                using aura::compiler::typed_audit::occurrence_commit_health_recovered_ok_v_read;
                std::int64_t faces = static_cast<std::int64_t>(
                    g_occurrence_commit_health_faces.load(std::memory_order_relaxed));
                std::int64_t goals = static_cast<std::int64_t>(
                    g_occurrence_commit_health_goals_live.load(std::memory_order_relaxed));
                std::int64_t persist = static_cast<std::int64_t>(
                    g_occurrence_commit_health_persist_size.load(std::memory_order_relaxed));
                std::int64_t needs = static_cast<std::int64_t>(
                    g_occurrence_commit_health_needs_recover.load(std::memory_order_relaxed));
                std::int64_t recovered =
                    static_cast<std::int64_t>(occurrence_commit_health_recovered_ok_v_read());
                std::int64_t fp_ok = static_cast<std::int64_t>(
                    g_occurrence_commit_health_fingerprint_ok.load(std::memory_order_relaxed));
                if (__qev_) {
                    if (auto* ctc = static_cast<aura::compiler::TypeChecker*>(
                            __qev_->commit_type_checker_handle())) {
                        const auto h = ctc->evaluate_occurrence_commit_health();
                        faces = static_cast<std::int64_t>(h.faces_bitmask);
                        goals = static_cast<std::int64_t>(h.goals_live);
                        persist = static_cast<std::int64_t>(h.persist_size);
                        needs = h.needs_recover ? 1 : 0;
                        fp_ok = h.fingerprint_ok ? 1 : 0;
                        recovered = h.recovered_ok ? 1 : 0;
                    }
                }
                insert_kv("occurrence-commit-health-faces", faces);
                insert_kv("occurrence-commit-health-goals-live", goals);
                insert_kv("occurrence-commit-health-persist-size", persist);
                insert_kv("occurrence-commit-health-needs-recover", needs);
                insert_kv("occurrence-commit-health-recovered-ok", recovered);
                insert_kv("occurrence-commit-health-fingerprint-ok", fp_ok);
                insert_kv("occurrence-commit-health-wired", 1);
                insert_kv("schema-2995", kOccurrenceCommitHealthIssue);
                insert_kv("issue-2995", kOccurrenceCommitHealthIssue);
            }
            // Issue #3030: abort/restore clears TypeLinearCommitProof face.
            insert_kv(
                "type-linear-proof-cleared-on-abort-total",
                static_cast<std::int64_t>(aura::compiler::typed_audit::
                                              type_linear_proof_cleared_on_abort_total_v_read()));
            insert_kv("type-linear-proof-cleared-on-abort-observe-total",
                      static_cast<std::int64_t>(
                          aura::compiler::typed_audit::
                              type_linear_proof_cleared_on_abort_observe_total_v_read()));
            insert_kv(
                "type-linear-proof-cleared-on-abort-wired",
                static_cast<std::int64_t>(
                    aura::compiler::typed_audit::g_type_linear_proof_cleared_on_abort_wired.load(
                        std::memory_order_relaxed)));
            insert_kv("schema-3030",
                      aura::compiler::typed_audit::kTypeLinearProofClearedOnAbortIssue);
            insert_kv("issue-3030",
                      aura::compiler::typed_audit::kTypeLinearProofClearedOnAbortIssue);
            // Issue #3031: pending_full_solve / locality residual at commit.
            insert_kv("pending-full-solve-residual-last",
                      static_cast<std::int64_t>(
                          aura::compiler::typed_audit::pending_full_solve_residual_last_v_read()));
            insert_kv(
                "pending-full-solve-residual-observe-total",
                static_cast<std::int64_t>(aura::compiler::typed_audit::
                                              pending_full_solve_residual_observe_total_v_read()));
            insert_kv(
                "pending-full-solve-residual-escalate-total",
                static_cast<std::int64_t>(aura::compiler::typed_audit::
                                              pending_full_solve_residual_escalate_total_v_read()));
            insert_kv(
                "pending-full-solve-residual-reject-total",
                static_cast<std::int64_t>(aura::compiler::typed_audit::
                                              pending_full_solve_residual_reject_total_v_read()));
            insert_kv("pending-full-solve-residual-wired",
                      static_cast<std::int64_t>(
                          aura::compiler::typed_audit::g_pending_full_solve_residual_wired.load(
                              std::memory_order_relaxed)));
            insert_kv("force-reason-pending-full-solve-residual", 16);
            insert_kv("schema-3031", aura::compiler::typed_audit::kPendingFullSolveResidualIssue);
            insert_kv("issue-3031", aura::compiler::typed_audit::kPendingFullSolveResidualIssue);
            // Issue #3032: rehydrate-miss → proof-invalidate → deopt.
            insert_kv("rehydrate-miss-invalidate-total",
                      static_cast<std::int64_t>(
                          aura::compiler::typed_audit::rehydrate_miss_invalidate_total_v_read()));
            insert_kv(
                "rehydrate-miss-invalidate-observe-total",
                static_cast<std::int64_t>(
                    aura::compiler::typed_audit::rehydrate_miss_invalidate_observe_total_v_read()));
            insert_kv("rehydrate-miss-force-deopt-total",
                      static_cast<std::int64_t>(
                          aura::compiler::typed_audit::rehydrate_miss_force_deopt_total_v_read()));
            insert_kv("rehydrate-miss-success-bind-total",
                      static_cast<std::int64_t>(
                          aura::compiler::typed_audit::rehydrate_success_bind_total_v_read()));
            insert_kv("rehydrate-miss-invalidate-wired",
                      static_cast<std::int64_t>(
                          aura::compiler::typed_audit::g_rehydrate_miss_invalidate_wired.load(
                              std::memory_order_relaxed)));
            insert_kv("schema-3032", aura::compiler::typed_audit::kRehydrateMissInvalidateIssue);
            insert_kv("issue-3032", aura::compiler::typed_audit::kRehydrateMissInvalidateIssue);
            // Issue #3063: steal/densify success invalidate-before-restamp.
            insert_kv(
                "steal-densify-success-invalidate-total",
                static_cast<std::int64_t>(
                    aura::compiler::typed_audit::steal_densify_success_invalidate_total_v_read()));
            insert_kv(
                "steal-densify-success-invalidate-wired",
                static_cast<std::int64_t>(
                    aura::compiler::typed_audit::g_steal_densify_success_invalidate_wired.load(
                        std::memory_order_relaxed)));
            insert_kv("schema-3063",
                      aura::compiler::typed_audit::kStealDensifySuccessInvalidateIssue);
            insert_kv("issue-3063",
                      aura::compiler::typed_audit::kStealDensifySuccessInvalidateIssue);
            insert_kv(
                "linear-fast-path-rehydrate-gen-elision-wired",
                static_cast<std::int64_t>(
                    aura::compiler::typed_audit::g_linear_fast_path_rehydrate_gen_elision_wired
                        .load(std::memory_order_relaxed)));
            insert_kv("schema-3085",
                      aura::compiler::typed_audit::kLinearFastPathRehydrateGenElisionIssue);
            insert_kv("issue-3085",
                      aura::compiler::typed_audit::kLinearFastPathRehydrateGenElisionIssue);
            insert_kv("linear-fast-path-steal-densify-clear-complete-wired",
                      static_cast<std::int64_t>(
                          aura::compiler::typed_audit::
                              g_linear_fast_path_steal_densify_clear_complete_wired.load(
                                  std::memory_order_relaxed)));
            insert_kv("schema-3171",
                      aura::compiler::typed_audit::kLinearFastPathStealDensifyClearCompleteIssue);
            insert_kv("issue-3171",
                      aura::compiler::typed_audit::kLinearFastPathStealDensifyClearCompleteIssue);

            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #2897: query:type-linear-evolution-snapshot — single atomic/
    // last-proof gauge poll for type×linear×occurrence self-evo loops
    // (Agent join reduction; #2860 pattern for hygiene/defuse/boundary).
    // Orthogonal to #2888 LifetimeConsistencyProof (EnvFrame/pin axis).
    // Pure SSOT fold of existing gauges — no CS walk, no new process state.
    // Soft quiet → zeros, cheap. Additive; preserves #2613/#2697/#2842/#2854.
    ObservabilityPrims::register_stats_impl(
        "query:type-linear-evolution-snapshot",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            const auto snap = capture_type_linear_evolution_snapshot();
            // ~24 keys + lineage schemas — 64 slots keep load factor healthy.
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
            insert_kv("schema", kTypeLinearEvolutionSnapshotIssue);
            insert_kv("issue", kTypeLinearEvolutionSnapshotIssue);
            insert_kv("schema-2897", kTypeLinearEvolutionSnapshotIssue);
            insert_kv("issue-2897", kTypeLinearEvolutionSnapshotIssue);
            // #2553 readiness face
            insert_kv("readiness-bp", snap.readiness_bp);
            insert_kv("force-reason-code", snap.force_reason_code);
            insert_kv("would-allow-commit", snap.would_allow_commit);
            // #2854 last-proof outcome (0 Quiet / 1 Stamped / 2 Reject)
            insert_kv("last-proof-outcome", snap.last_proof_outcome);
            insert_kv("last-proof-outcome-quiet", 0);
            insert_kv("last-proof-outcome-stamped", 1);
            insert_kv("last-proof-outcome-reject", 2);
            // #2697 / #2758 / #2842 last-proof gauges
            insert_kv("live-goal-count", snap.live_goal_count);
            insert_kv("goal-fingerprint", snap.goal_fingerprint);
            insert_kv("linear-root-count", snap.linear_root_count);
            insert_kv("last-proof-stamp", snap.last_proof_stamp);
            insert_kv("last-proof-mid", snap.last_proof_mid);
            insert_kv("schema-3016", 3016);
            insert_kv("issue-3016", 3016);
            // Issue #3091: additive proof audit_mid (TypeLinearCommitProof.audit_mid,
            // stamped from TLS boundary-noted mid / g_last_stamped_audit_mid).
            // Mirrors last-proof-mid but reads the proof's own audit_mid gauge
            // (typed_audit::g_last_stamped_audit_mid is the SSOT for "last
            // stamped" since #3016; proof.audit_mid also falls back to it
            // when no TLS boundary mid is noted, so the two converge).
            insert_kv("audit-mid",
                      static_cast<std::int64_t>(
                          typed_audit::g_last_stamped_audit_mid.load(std::memory_order_relaxed)));
            insert_kv("schema-3091", 3091);
            insert_kv("issue-3091", 3091);
            // #2854 same-tx order totals
            insert_kv("proof-stamped-after-rebind-total", snap.proof_stamped_after_rebind_total);
            insert_kv("proof-reject-after-rebind-fail-total",
                      snap.proof_reject_after_rebind_fail_total);
            // #2621 / #2703 / #2704 face bits + totals
            insert_kv("partial-cone-truncated", snap.partial_cone_truncated);
            insert_kv("occurrence-empty-after-fence", snap.occurrence_empty_after_fence_total);
            insert_kv("occurrence-empty-after-fence-total",
                      snap.occurrence_empty_after_fence_total);
            insert_kv("occurrence-empty-after-fence-soft-total",
                      snap.occurrence_empty_after_fence_soft_total);
            insert_kv("cone-outside-goal-drop", snap.cone_outside_goal_drop_total);
            insert_kv("cone-outside-goal-drop-total", snap.cone_outside_goal_drop_total);
            // Issue #2962: residual recover-ok / hard-reject totals (fidelity).
            insert_kv(
                "cone-outside-goal-drop-recover-ok-total",
                static_cast<std::int64_t>(
                    aura::compiler::typed_audit::cone_outside_goal_drop_recover_ok_total_v_read()));
            insert_kv(
                "cone-outside-goal-drop-reject-total",
                static_cast<std::int64_t>(
                    aura::compiler::typed_audit::cone_outside_goal_drop_reject_total_v_read()));
            insert_kv("type-linear-evolution-snapshot-wired", 1);
            // Issue #3116: abort dual-clear of last_coercions_ + TLS context.
            insert_kv(
                "coercion-abort-dual-clear-total",
                static_cast<std::int64_t>(aura::compiler::g_coercion_abort_dual_clear_total.load(
                    std::memory_order_relaxed)));
            insert_kv("coercion-abort-dual-clear-observe-total",
                      static_cast<std::int64_t>(
                          aura::compiler::g_coercion_abort_dual_clear_observe_total.load(
                              std::memory_order_relaxed)));
            insert_kv("schema-3116", aura::compiler::kCoercionAbortDualClearIssue);
            insert_kv("issue-3116", aura::compiler::kCoercionAbortDualClearIssue);
            // Lineage preserved (detailed queries remain authoritative)
            insert_kv("schema-2613", 2613);
            insert_kv("schema-2697", 2697);
            insert_kv("schema-2842", 2842);
            insert_kv("schema-2854", 2854);
            insert_kv("schema-2860", 2860); // hygiene/defuse/boundary axis sibling
            insert_kv("schema-2553", 2553);
            insert_kv("schema-2621", 2621);
            insert_kv("schema-2704", 2704);
            insert_kv("schema-2703", 2703);
            insert_kv("schema-2981", 2981);
            insert_kv("issue-2981", 2981);
            insert_kv("type-linear-proof-empty-after-fence-wired", 1);
            insert_kv("schema-2962",
                      aura::compiler::typed_audit::kConeOutsideGoalDropRecoverRejectIssue);
            insert_kv("issue-2962",
                      aura::compiler::typed_audit::kConeOutsideGoalDropRecoverRejectIssue);

            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2350: query:type-system-health — single Agent score (basis points)
    // aggregating provenance completeness, timeout reject rate, linear pin miss
    // rate, and layered DCE efficiency. Pure/read-only (AC3); does not rename
    // existing keys. force-reason priority when health < budget:
    // timeout-reject > pin-miss > provenance-miss > castop-density > ok.
    //
    // Score (see type_system_health.hh AC1 comment):
    //   health_bp = (prov + (10000-timeout_rate) + (10000-pin_rate) + dce_eff) / 4
    ObservabilityPrims::register_stats_impl(
        "query:type-system-health", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;

            TypeSystemHealthSnapshot snap;
            // AC1 components — vacuous healthy when no samples.
            snap.provenance_completeness_bp = coercion_provenance_completeness_bp();

            const std::uint64_t to_reject =
                m ? m->delta_timeout_reject_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t to_full =
                m ? m->delta_timeout_full_solve_total.load(std::memory_order_relaxed) : 0;
            snap.timeout_reject_total = to_reject;
            snap.timeout_full_solve_total = to_full;
            snap.timeout_reject_rate_bp = rate_bp(to_reject, to_full);

            const auto pin_stats = aura::core::lifetime::linear_root_snapshot();
            snap.pin_total = pin_stats.pin_total;
            snap.pin_miss_total = pin_stats.pin_miss_total;
            snap.linear_pin_miss_rate_bp = rate_bp(pin_stats.pin_miss_total, pin_stats.pin_total);

            const std::uint64_t ast_elided =
                g_dead_coercion_ast_elided_total.load(std::memory_order_relaxed);
            const std::uint64_t ir_elided =
                opt_registry::dead_coercion_ir_elided_total.load(std::memory_order_relaxed);
            const std::uint64_t dirty_cone =
                opt_registry::dead_coercion_dirty_cone_skips.load(std::memory_order_relaxed);
            const std::uint64_t pipeline_runs =
                opt_registry::dead_coercion_pipeline_runs_total.load(std::memory_order_relaxed);
            snap.layered_elided_total = ast_elided + ir_elided + dirty_cone;
            snap.dce_pipeline_runs = pipeline_runs;
            snap.layered_dce_efficiency_bp =
                layered_dce_efficiency_bp(snap.layered_elided_total, pipeline_runs);

            if (m) {
                snap.castop_density_bp = m->last_castop_density_bp.load(std::memory_order_relaxed);
                snap.castop_density_budget_bp =
                    m->castop_density_budget_bp.load(std::memory_order_relaxed);
                snap.castop_over_budget_total =
                    m->castop_density_over_budget_total.load(std::memory_order_relaxed);
            }

            const auto scored = compute_type_system_health(snap);

            // Issue #2462: SolverSnapshot + density + hard-reject → next-action.
            // Pure read (no solve); reuses snapshot_constraint_system (#2308)
            // and type_repair last surface for repair_nodes / suggested_roots.
            SolverSnapshot solver_snap{};
            std::size_t suggested_roots_n = 0;
            bool hard_gate_reject = false;
            if (__qev_ && __qev_->commit_cs_live()) {
                if (auto* ctc = static_cast<aura::compiler::TypeChecker*>(
                        __qev_->commit_type_checker_handle())) {
                    solver_snap = snapshot_constraint_system(ctc->constraint_system(), nullptr);
                }
            }
            if (m) {
                // Cap-16 repair sample from last type_repair publish when
                // snapshot repair_nodes empty (boundary reject path).
                if (solver_snap.repair_nodes.empty()) {
                    const auto n = m->type_repair_last_unresolved_aff_nodes_count.load(
                        std::memory_order_relaxed);
                    const std::size_t cap = std::min<std::size_t>(n, 16);
                    for (std::size_t i = 0; i < cap; ++i) {
                        const auto id = m->type_repair_last_unresolved_aff_nodes[i].load(
                            std::memory_order_relaxed);
                        if (id != 0)
                            solver_snap.repair_nodes.push_back(static_cast<std::uint32_t>(id));
                    }
                }
                suggested_roots_n =
                    m->type_repair_suggested_root_count.load(std::memory_order_relaxed);
                // Hard-reject status 99 from boundary force path (#2284).
                constexpr std::uint64_t kHardRejectStatus = 99;
                hard_gate_reject = m->type_repair_last_timeout_status.load(
                                       std::memory_order_relaxed) == kHardRejectStatus;
                if (solver_snap.unresolved.empty()) {
                    // Mirror last unresolved count as signal only (no Constraint body).
                    const auto uc =
                        m->type_repair_last_unresolved_count.load(std::memory_order_relaxed);
                    if (uc > 0 && solver_snap.status == SolveResult::SOLVED &&
                        m->type_repair_last_timeout_status.load(std::memory_order_relaxed) != 0)
                        solver_snap.status = static_cast<SolveResult>(std::min<std::uint64_t>(
                            m->type_repair_last_timeout_status.load(std::memory_order_relaxed), 2));
                }
                if (m->type_repair_last_truncated_reverify.load(std::memory_order_relaxed) != 0)
                    solver_snap.truncated_reverify = true;
            }
            TypeSystemNextActionInput nai{};
            nai.health_ok = scored.health_bp >= scored.health_budget_bp;
            nai.force_reason = scored.force_reason;
            nai.solve_status = static_cast<std::uint8_t>(solver_snap.status);
            nai.truncated_reverify = solver_snap.truncated_reverify;
            nai.blame_complete = solver_snap.blame.is_complete() || solver_snap.blame.complete;
            nai.production_escalated = solver_snap.production_escalated;
            nai.repair_nodes_count = solver_snap.repair_nodes.size();
            nai.suggested_roots_count = suggested_roots_n;
            nai.unresolved_count = solver_snap.unresolved.size();
            if (m && nai.unresolved_count == 0) {
                nai.unresolved_count =
                    m->type_repair_last_unresolved_count.load(std::memory_order_relaxed);
            }
            nai.castop_over_budget = snap.castop_density_bp > snap.castop_density_budget_bp ||
                                     snap.castop_over_budget_total > 0;
            nai.hard_gate_reject = hard_gate_reject;
            nai.production_defaults = aura::compiler::typed_audit::production_defaults_active();
            const auto next = decide_type_system_next_action(nai);

            // Capacity 64: #2350 keys + #2462 next-action / repair sample.
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
            auto insert_kv_str = [&](const char* k_str, std::string_view v_str) {
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
                        auto vidx = string_heap.size();
                        string_heap.push_back(std::string(v_str));
                        vals[idx] = make_string(static_cast<std::uint64_t>(vidx)).val;
                        ht->size++;
                        return;
                    }
                }
            };

            insert_kv("health-bp", static_cast<std::int64_t>(scored.health_bp));
            insert_kv("health-budget-bp", static_cast<std::int64_t>(scored.health_budget_bp));
            insert_kv_str("force-reason", scored.force_reason);
            // Component mirrors (optional AC4).
            insert_kv("component-provenance-completeness-bp",
                      static_cast<std::int64_t>(snap.provenance_completeness_bp));
            insert_kv("component-timeout-reject-rate-bp",
                      static_cast<std::int64_t>(snap.timeout_reject_rate_bp));
            insert_kv("component-linear-pin-miss-rate-bp",
                      static_cast<std::int64_t>(snap.linear_pin_miss_rate_bp));
            insert_kv("component-layered-dce-efficiency-bp",
                      static_cast<std::int64_t>(snap.layered_dce_efficiency_bp));
            insert_kv("component-castop-density-bp",
                      static_cast<std::int64_t>(snap.castop_density_bp));
            insert_kv("type-system-health-wired", 1);
            insert_kv("schema-2350", 2350);
            insert_kv("issue-2350", 2350);
            // Lineage sentinels (related schemas still independently queryable).
            insert_kv("schema-2282", 2282);
            insert_kv("schema-2284", 2284);
            insert_kv("schema-2287", 2287);
            // Issue #2462: next-action + repair_nodes closed-loop surface.
            //   next-action: string enum (ok|annotate-dynamic|expand-dirty|
            //     full-solve|rollback)
            //   next-action-code: 0..4 int alias
            //   repair-nodes-count + repair-node-0..15 (cap 16)
            //   suggested-roots-count + suggested-root-0..7 when available
            insert_kv_str("next-action", next.action_str);
            insert_kv("next-action-code", static_cast<std::int64_t>(next.action_code));
            insert_kv("repair-nodes-count",
                      static_cast<std::int64_t>(solver_snap.repair_nodes.size()));
            {
                const std::size_t cap =
                    std::min(solver_snap.repair_nodes.size(), static_cast<std::size_t>(16));
                for (std::size_t i = 0; i < 16; ++i) {
                    char keybuf[32];
                    // Fixed keys repair-node-0 .. repair-node-15
                    std::snprintf(keybuf, sizeof(keybuf), "repair-node-%zu", i);
                    const std::int64_t v =
                        i < cap ? static_cast<std::int64_t>(solver_snap.repair_nodes[i]) : 0;
                    insert_kv(keybuf, v);
                }
            }
            insert_kv("suggested-roots-count", static_cast<std::int64_t>(suggested_roots_n));
            if (m) {
                const std::size_t rcap = std::min(suggested_roots_n, static_cast<std::size_t>(8));
                for (std::size_t i = 0; i < 8; ++i) {
                    char keybuf[32];
                    std::snprintf(keybuf, sizeof(keybuf), "suggested-root-%zu", i);
                    const std::int64_t v =
                        i < rcap ? static_cast<std::int64_t>(m->type_repair_suggested_roots[i].load(
                                       std::memory_order_relaxed))
                                 : 0;
                    insert_kv(keybuf, v);
                }
            }
            insert_kv("type-system-health-next-action-wired", 1);
            insert_kv("schema-2462", 2462);
            insert_kv("issue-2462", 2462);

            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2379: query:mutation-concurrency-health — single Agent score
    // for runtime mutation safety (hold + steal + residual + mailbox + densify).
    // Pure reads of existing atomics (AC4 zero extra hot-path cost). Formula +
    // force_reason priority in mutation_concurrency_health.hh.
    // Priority: steal-mismatch > residual-defer > densify-fail > hold-slo >
    // mailbox-starvation > none.
    ObservabilityPrims::register_stats_impl(
        "query:mutation-concurrency-health",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;

            MutationConcurrencyHealthSnapshot snap;
            // Steal force-deopt / hard-fail (process Fiber statics; always present).
            snap.steal_force_deopt_total =
                aura::serve::Fiber::steal_snapshot_mismatch_force_deopt_total();
            snap.steal_hard_fail_total = aura::serve::Fiber::steal_snapshot_hard_fail_total();
            // Residual GC defer.
            snap.residual_defer_cleared_on_steal_total =
                aura::gc_hooks::residual_defer_cleared_on_steal_total();
            if (m)
                snap.residual_hard_fail_total =
                    m->mutation_boundary_residual_defer_hard_fail_total.load(
                        std::memory_order_relaxed) +
                    m->residual_defer_steal_hard_fail_total.load(std::memory_order_relaxed);
            // Densify consistency last-call + cumulative fail.
            snap.densify_consistency_fail_total =
                aura::core::densify_consistency::densify_consistency_fail_total();
            snap.last_densify_envframe_ok =
                aura::core::densify_consistency::last_densify_envframe_ok() ? 1 : 0;
            snap.last_densify_closure_remount_ok =
                aura::core::densify_consistency::last_densify_closure_remount_ok() ? 1 : 0;
            // Hold SLO / over-budget.
            if (m) {
                snap.hold_slo_violation_total =
                    m->mutation_hold_slo_violation_total.load(std::memory_order_relaxed);
                snap.hold_over_budget_total =
                    m->mutation_hold_over_budget_total.load(std::memory_order_relaxed);
            }
            // Mailbox defer SLA.
            using aura::serve::mf_mailbox::g_mf_mailbox_stats;
            snap.mailbox_defer_starvation_total =
                g_mf_mailbox_stats.mailbox_defer_starvation_total.load(std::memory_order_relaxed);
            snap.mailbox_deferred_depth =
                g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed);
            snap.mailbox_deferred_mutation_hold_total =
                g_mf_mailbox_stats.mailbox_deferred_mutation_hold_total.load(
                    std::memory_order_relaxed);
            snap.mailbox_hold_exit_starvation_total =
                g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.load(
                    std::memory_order_relaxed);
            snap.mailbox_hold_starvation_hard_total =
                g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(
                    std::memory_order_relaxed);
            snap.agent_throttle_for_mailbox_starvation =
                g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
                    std::memory_order_relaxed);
            // Issue #2903: under-boundary wait max for soft latency SLO.
            snap.mailbox_under_boundary_wait_us_max =
                g_mf_mailbox_stats.mailbox_under_boundary_wait_us_max.load(
                    std::memory_order_relaxed);

            const auto scored = compute_mutation_concurrency_health(snap);

            // Capacity 64→128: #2551 hard starvation + throttle keys.
            auto* ht = FlatHashTable::create(128);
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
            auto insert_kv_str = [&](const char* k_str, std::string_view v_str) {
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
                        auto vidx = string_heap.size();
                        string_heap.push_back(std::string(v_str));
                        vals[idx] = make_string(static_cast<std::uint64_t>(vidx)).val;
                        ht->size++;
                        return;
                    }
                }
            };

            insert_kv("health-bp", static_cast<std::int64_t>(scored.health_bp));
            insert_kv("health-budget-bp", static_cast<std::int64_t>(scored.health_budget_bp));
            insert_kv_str("force-reason", scored.force_reason);
            insert_kv("force-reason-code", scored.force_reason_code);
            // Component raw totals (additive; subsystem queries unchanged).
            insert_kv("component-steal-force-deopt-total",
                      static_cast<std::int64_t>(snap.steal_force_deopt_total));
            insert_kv("component-steal-hard-fail-total",
                      static_cast<std::int64_t>(snap.steal_hard_fail_total));
            insert_kv("component-residual-defer-cleared-on-steal-total",
                      static_cast<std::int64_t>(snap.residual_defer_cleared_on_steal_total));
            insert_kv("component-residual-hard-fail-total",
                      static_cast<std::int64_t>(snap.residual_hard_fail_total));
            insert_kv("component-densify-consistency-fail-total",
                      static_cast<std::int64_t>(snap.densify_consistency_fail_total));
            insert_kv("component-last-densify-envframe-ok",
                      static_cast<std::int64_t>(snap.last_densify_envframe_ok));
            insert_kv("component-last-densify-closure-remount-ok",
                      static_cast<std::int64_t>(snap.last_densify_closure_remount_ok));
            insert_kv("component-hold-slo-violation-total",
                      static_cast<std::int64_t>(snap.hold_slo_violation_total));
            insert_kv("component-hold-over-budget-total",
                      static_cast<std::int64_t>(snap.hold_over_budget_total));
            insert_kv("component-mailbox-defer-starvation-total",
                      static_cast<std::int64_t>(snap.mailbox_defer_starvation_total));
            insert_kv("component-mailbox-deferred-depth",
                      static_cast<std::int64_t>(snap.mailbox_deferred_depth));
            insert_kv("component-mailbox-deferred-mutation-hold-total",
                      static_cast<std::int64_t>(snap.mailbox_deferred_mutation_hold_total));
            insert_kv("component-mailbox-hold-exit-starvation-total",
                      static_cast<std::int64_t>(snap.mailbox_hold_exit_starvation_total));
            insert_kv("component-mailbox-hold-starvation-hard-total",
                      static_cast<std::int64_t>(snap.mailbox_hold_starvation_hard_total));
            insert_kv("component-agent-throttle-for-mailbox-starvation",
                      static_cast<std::int64_t>(snap.agent_throttle_for_mailbox_starvation));
            // Issue #2903: under-boundary wait max component for soft latency SLO.
            insert_kv("component-mailbox-under-boundary-wait-us-max",
                      static_cast<std::int64_t>(snap.mailbox_under_boundary_wait_us_max));
            insert_kv("mutation-concurrency-health-wired", 1);
            insert_kv("schema-2379", 2379);
            insert_kv("issue-2379", 2379);
            // Issue #2985: production admit close-loop (additive).
            insert_kv("mutation-concurrency-health-reject-total",
                      static_cast<std::int64_t>(g_mutation_concurrency_health_reject_total.load(
                          std::memory_order_relaxed)));
            insert_kv(
                "mutation-concurrency-health-soft-observe-total",
                static_cast<std::int64_t>(g_mutation_concurrency_health_soft_observe_total.load(
                    std::memory_order_relaxed)));
            insert_kv("mutation-concurrency-health-admit-wired", 1);
            insert_kv("schema-2985", 2985);
            insert_kv("issue-2985", 2985);
            // Issue #3039: production ScopedParallel overlap hard-reject
            // (additive on existing health surface).
            insert_kv("mutation-region-overlap-hard-reject-total",
                      static_cast<std::int64_t>(
                          aura::compiler::mutation_region_overlap_hard_reject_total_v_read()));
            insert_kv("mutation-region-overlap-hard-reject-wired",
                      static_cast<std::int64_t>(
                          aura::compiler::mutation_region_overlap_hard_reject_wired_v_read()));
            insert_kv("schema-3039", 3039);
            insert_kv("issue-3039", 3039);
            // Lineage sentinels (existing queries remain authoritative).
            insert_kv("schema-2310", 2310);
            insert_kv("schema-2341", 2341);
            insert_kv("schema-2349", 2349);
            insert_kv("schema-2312", 2312);
            insert_kv("schema-2378", 2378);
            insert_kv("schema-2511", 2511);
            insert_kv("issue-2511", 2511);
            insert_kv("schema-2551", 2551);
            insert_kv("issue-2551", 2551);
            insert_kv("schema-2903", 2903);
            insert_kv("issue-2903", 2903);

            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2506: query:aot-hot-update-health — single Agent score for
    // JIT/AOT recovery gate (reload recovery + storm + remount + epoch).
    // Pure relaxed loads of existing atomics (AC4 zero idle cost). Formula +
    // force_reason priority in aot_hot_update_health.hh.
    // Alias: query:hot-update-health (same builder).
    auto register_aot_hot_update_health = [&string_heap](const char* name) {
        ObservabilityPrims::register_stats_impl(
            name, [&string_heap](std::span<const EvalValue> a) -> EvalValue {
                (void)a;
                auto* __qev_ = Evaluator::get_query_evaluator();
                const auto* m =
                    __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics())
                           : nullptr;

                AotHotUpdateHealthSnapshot snap;
                // Reload recovery C snapshot (#2367) — pure relaxed loads.
                aura_reload_recovery_snapshot rs{};
                aura_hot_update_reload_recovery_get_snapshot(&rs);
                snap.attempts_left = static_cast<std::uint32_t>(rs.attempts_left);
                snap.force_jit_regions_mask = static_cast<std::uint64_t>(rs.force_jit_regions_mask);
                snap.pending_dirty_count = static_cast<std::uint64_t>(rs.pending_dirty_count);
                snap.deferred_reemit_pending =
                    static_cast<std::uint8_t>(rs.deferred_reemit_pending);
                snap.storm_level = static_cast<std::uint8_t>(rs.storm_level);
                snap.hard_storm_active = rs.hard_storm_active;
                snap.last_reload_fail_reason = static_cast<std::uint8_t>(rs.last_reason);
                snap.recovery_active = rs.recovery_active;
                // Remount counters (#2234).
                if (m) {
                    snap.remount_fail_total =
                        m->closure_capture_remount_fail_total.load(std::memory_order_relaxed);
                    snap.remount_ok_total =
                        m->closure_capture_remount_ok_total.load(std::memory_order_relaxed);
                }
                // Epoch invariant (#2366 / #2501).
                snap.epoch_invariant_violation_total =
                    aura_epoch_invariant_violation_total_v_read();

                const auto scored = compute_aot_hot_update_health(snap);

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
                        h = (h ^ static_cast<std::uint8_t>(*p)) *
                            ::aura::compiler::stats::kFnvPrime;
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
                auto insert_kv_str = [&](const char* k_str, std::string_view v_str) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (const char* p = k_str; *p; ++p)
                        h = (h ^ static_cast<std::uint8_t>(*p)) *
                            ::aura::compiler::stats::kFnvPrime;
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
                            auto vidx = string_heap.size();
                            string_heap.push_back(std::string(v_str));
                            vals[idx] = make_string(static_cast<std::uint64_t>(vidx)).val;
                            ht->size++;
                            return;
                        }
                    }
                };

                insert_kv("health-bp", static_cast<std::int64_t>(scored.health_bp));
                insert_kv("health-budget-bp", static_cast<std::int64_t>(scored.health_budget_bp));
                insert_kv_str("force-reason", scored.force_reason);
                insert_kv("force-reason-code", scored.force_reason_code);
                insert_kv("recovery-active", scored.recovery_active);
                // Components (raw totals; subsystem queries unchanged).
                insert_kv("component-attempts-left", static_cast<std::int64_t>(snap.attempts_left));
                insert_kv("component-force-jit-regions-mask",
                          static_cast<std::int64_t>(snap.force_jit_regions_mask));
                insert_kv("component-pending-dirty-count",
                          static_cast<std::int64_t>(snap.pending_dirty_count));
                insert_kv("component-deferred-reemit-pending",
                          static_cast<std::int64_t>(snap.deferred_reemit_pending));
                insert_kv("component-storm-level", static_cast<std::int64_t>(snap.storm_level));
                insert_kv("component-hard-storm-active", snap.hard_storm_active);
                insert_kv("component-last-reload-fail-reason",
                          static_cast<std::int64_t>(snap.last_reload_fail_reason));
                insert_kv("component-remount-fail-total",
                          static_cast<std::int64_t>(snap.remount_fail_total));
                insert_kv("component-remount-ok-total",
                          static_cast<std::int64_t>(snap.remount_ok_total));
                insert_kv("component-epoch-invariant-violation-total",
                          static_cast<std::int64_t>(snap.epoch_invariant_violation_total));
                // Issue #2640: production Restricted default periodic
                // epoch-invariant soft walk counters (additive).
                insert_kv(
                    "component-epoch-invariant-periodic-walks-total",
                    static_cast<std::int64_t>(aura_epoch_invariant_periodic_walks_total_v_read()));
                insert_kv("component-epoch-invariant-periodic-last-walk-at-ms",
                          static_cast<std::int64_t>(
                              aura_epoch_invariant_periodic_last_walk_at_ms_v_read()));
                insert_kv(
                    "component-epoch-invariant-periodic-period-ms",
                    static_cast<std::int64_t>(aura_epoch_invariant_periodic_period_ms_v_read()));
                insert_kv("component-epoch-invariant-periodic-skipped-off-total",
                          static_cast<std::int64_t>(
                              aura_epoch_invariant_periodic_skipped_off_total_v_read()));
                insert_kv("component-epoch-invariant-periodic-skipped-wrong-mode-total",
                          static_cast<std::int64_t>(
                              aura_epoch_invariant_periodic_skipped_wrong_mode_total_v_read()));
                insert_kv("component-epoch-invariant-periodic-skipped-rate-limited-total",
                          static_cast<std::int64_t>(
                              aura_epoch_invariant_periodic_skipped_rate_limited_total_v_read()));
                insert_kv("component-epoch-invariant-periodic-skipped-disabled-total",
                          static_cast<std::int64_t>(
                              aura_epoch_invariant_periodic_skipped_disabled_total_v_read()));
                insert_kv("epoch-invariant-periodic-wired", 1);
                insert_kv("schema-2640", 2640);
                insert_kv("issue-2640", 2640);
                // force-reason code sentinels (docs)
                insert_kv("force-reason-ok", 0);
                insert_kv("force-reason-storm", 1);
                insert_kv("force-reason-force-jit", 2);
                insert_kv("force-reason-reload-fail", 3);
                insert_kv("force-reason-remount-fail", 4);
                insert_kv("force-reason-epoch-invariant", 5);
                insert_kv("force-reason-deferred-reemit", 6);
                insert_kv("aot-hot-update-health-wired", 1);
                insert_kv("schema-2506", 2506);
                insert_kv("issue-2506", 2506);
                // Issue #2543: orch self-throttle control plane over this score.
                {
                    const auto td = decide_hot_update_throttle(scored);
                    insert_kv("throttle-active", td.throttle ? 1 : 0);
                    insert_kv(
                        "throttle-action-code",
                        static_cast<std::int64_t>(td.action)); // 0 none 1 split 2 delay 3 skip
                    insert_kv("throttle-max-concurrency-cap",
                              static_cast<std::int64_t>(td.max_concurrency_cap));
                    insert_kv(
                        "orch-hot-update-health-throttle-total",
                        static_cast<std::int64_t>(g_orch_hot_update_health_throttle_total.load(
                            std::memory_order_relaxed)));
                    insert_kv("orch-hot-update-health-checks-total",
                              static_cast<std::int64_t>(g_orch_hot_update_health_checks_total.load(
                                  std::memory_order_relaxed)));
                    insert_kv(
                        "orch-hot-update-health-last-force-reason",
                        g_orch_hot_update_health_last_force_reason.load(std::memory_order_relaxed));
                    insert_kv("schema-2543", kAotHotUpdateHealthThrottleIssue);
                    insert_kv("issue-2543", kAotHotUpdateHealthThrottleIssue);
                    insert_kv("orch-hot-update-health-throttle-wired", 1);
                }
                // Lineage (existing queries remain authoritative).
                insert_kv("schema-2367", 2367);
                insert_kv("schema-2302", 2302);
                insert_kv("schema-2094", 2094);
                insert_kv("schema-2234", 2234);
                insert_kv("schema-2366", 2366);

                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            });
    };
    register_aot_hot_update_health("query:aot-hot-update-health");
    register_aot_hot_update_health("query:hot-update-health");

    // Issue #2500: query:compact-policy (+ aliases orch:compact-policy /
    // arena:recommend-compact) — pure recommendation over existing health /
    // frag / defer / hold atomics. Does not change Soft/Force gates (AC4).
    auto register_compact_policy = [&string_heap](const char* name) {
        ObservabilityPrims::register_stats_impl(
            name, [&string_heap](std::span<const EvalValue> a) -> EvalValue {
                (void)a;
                auto* __qev_ = Evaluator::get_query_evaluator();
                const auto* m =
                    __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics())
                           : nullptr;

                // ── Build mutation-concurrency health (same sources as #2379) ──
                MutationConcurrencyHealthSnapshot hsnap;
                hsnap.steal_force_deopt_total =
                    aura::serve::Fiber::steal_snapshot_mismatch_force_deopt_total();
                hsnap.steal_hard_fail_total = aura::serve::Fiber::steal_snapshot_hard_fail_total();
                hsnap.residual_defer_cleared_on_steal_total =
                    aura::gc_hooks::residual_defer_cleared_on_steal_total();
                if (m)
                    hsnap.residual_hard_fail_total =
                        m->mutation_boundary_residual_defer_hard_fail_total.load(
                            std::memory_order_relaxed) +
                        m->residual_defer_steal_hard_fail_total.load(std::memory_order_relaxed);
                hsnap.densify_consistency_fail_total =
                    aura::core::densify_consistency::densify_consistency_fail_total();
                hsnap.last_densify_envframe_ok =
                    aura::core::densify_consistency::last_densify_envframe_ok() ? 1 : 0;
                hsnap.last_densify_closure_remount_ok =
                    aura::core::densify_consistency::last_densify_closure_remount_ok() ? 1 : 0;
                if (m) {
                    hsnap.hold_slo_violation_total =
                        m->mutation_hold_slo_violation_total.load(std::memory_order_relaxed);
                    hsnap.hold_over_budget_total =
                        m->mutation_hold_over_budget_total.load(std::memory_order_relaxed);
                }
                using aura::serve::mf_mailbox::g_mf_mailbox_stats;
                hsnap.mailbox_defer_starvation_total =
                    g_mf_mailbox_stats.mailbox_defer_starvation_total.load(
                        std::memory_order_relaxed);
                hsnap.mailbox_deferred_depth =
                    g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed);
                hsnap.mailbox_deferred_mutation_hold_total =
                    g_mf_mailbox_stats.mailbox_deferred_mutation_hold_total.load(
                        std::memory_order_relaxed);
                const auto health = compute_mutation_concurrency_health(hsnap);

                CompactPolicyInput pin;
                pin.health_bp = health.health_bp;
                pin.health_budget_bp = health.health_budget_bp;
                pin.should_defer_destructive_gc = aura::gc_hooks::should_defer_destructive_gc();
                pin.active_guard_depth = aura::core::envframe_lifetime::active_guard_depth();
                pin.mutation_hold_active = (__qev_ && __qev_->mutation_boundary_depth() > 0) ||
                                           hsnap.mailbox_deferred_mutation_hold_total > 0;
                pin.densify_consistency_fail_total = hsnap.densify_consistency_fail_total;
                if (m) {
                    pin.pin_contract_fail_total =
                        m->moving_compact_pin_contract_fail_total.load(std::memory_order_relaxed);
                }
                pin.force_blocked_by_pin_total =
                    aura::ast::g_force_compact_blocked_by_pin_total.load(std::memory_order_relaxed);
                pin.force_blocked_by_envframe_total =
                    aura::ast::g_force_compact_blocked_by_envframe_guard_total.load(
                        std::memory_order_relaxed);

                // Frag: process metric (post-mutate) — pure read, no private
                // arena_ access. Values may be pct 0..100 or bp; normalize.
                std::uint64_t frag_bp = 0;
                if (m) {
                    frag_bp = m->arena_fragmentation_post_mutate.load(std::memory_order_relaxed);
                    if (frag_bp > 0 && frag_bp <= 100)
                        frag_bp *= 100;
                    if (frag_bp > 10000)
                        frag_bp = 10000;
                }
                pin.frag_bp = frag_bp;

                // Hold estimate → recommend-split (#2405): estimate > 0.7 * budget.
                {
                    const auto budget =
                        static_cast<std::int64_t>(aura::compiler::mutation_hold_budget_us());
                    std::int64_t estimate = 0;
                    if (m) {
                        const auto total =
                            m->mutation_hold_sample_count.load(std::memory_order_relaxed);
                        const auto n = static_cast<std::size_t>(std::min(
                            total,
                            static_cast<std::uint64_t>(CompilerMetrics::kMutationHoldSampleRing)));
                        if (n > 0) {
                            std::array<std::uint64_t, CompilerMetrics::kMutationHoldSampleRing>
                                samples{};
                            if (total < CompilerMetrics::kMutationHoldSampleRing) {
                                for (std::size_t i = 0; i < n; ++i)
                                    samples[i] = m->mutation_hold_sample_ring[i].load(
                                        std::memory_order_relaxed);
                            } else {
                                for (std::size_t i = 0;
                                     i < CompilerMetrics::kMutationHoldSampleRing; ++i)
                                    samples[i] = m->mutation_hold_sample_ring[i].load(
                                        std::memory_order_relaxed);
                            }
                            std::sort(samples.begin(),
                                      samples.begin() + static_cast<std::ptrdiff_t>(n));
                            const auto p99_idx = static_cast<std::size_t>(
                                (static_cast<std::uint64_t>(n) * 99 + 99) / 100);
                            estimate = static_cast<std::int64_t>(samples[std::min(p99_idx, n - 1)]);
                        }
                    }
                    pin.recommend_split = (budget > 0 && estimate * 10 > budget * 7);
                }

                const auto pol = compute_compact_policy(pin);

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
                        h = (h ^ static_cast<std::uint8_t>(*p)) *
                            ::aura::compiler::stats::kFnvPrime;
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
                auto insert_kv_str = [&](const char* k_str, std::string_view v_str) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (const char* p = k_str; *p; ++p)
                        h = (h ^ static_cast<std::uint8_t>(*p)) *
                            ::aura::compiler::stats::kFnvPrime;
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
                            auto vidx = string_heap.size();
                            string_heap.push_back(std::string(v_str));
                            vals[idx] = make_string(static_cast<std::uint64_t>(vidx)).val;
                            ht->size++;
                            return;
                        }
                    }
                };

                insert_kv_str("mode", pol.mode_name);
                insert_kv("mode-code", pol.mode_code);
                insert_kv_str("reason", pol.reason);
                insert_kv("health-bp", static_cast<std::int64_t>(pin.health_bp));
                insert_kv("health-budget-bp", static_cast<std::int64_t>(pin.health_budget_bp));
                insert_kv("frag-bp", static_cast<std::int64_t>(pin.frag_bp));
                insert_kv("should-defer", pin.should_defer_destructive_gc ? 1 : 0);
                insert_kv("active-guard-depth", static_cast<std::int64_t>(pin.active_guard_depth));
                insert_kv("mutation-hold-active", pin.mutation_hold_active ? 1 : 0);
                insert_kv("pin-contract-fail-total",
                          static_cast<std::int64_t>(pin.pin_contract_fail_total));
                insert_kv("densify-consistency-fail-total",
                          static_cast<std::int64_t>(pin.densify_consistency_fail_total));
                insert_kv("force-blocked-by-pin-total",
                          static_cast<std::int64_t>(pin.force_blocked_by_pin_total));
                insert_kv("force-blocked-by-envframe-total",
                          static_cast<std::int64_t>(pin.force_blocked_by_envframe_total));
                insert_kv("recommend-split", pin.recommend_split ? 1 : 0);
                insert_kv("advisory", 1); // Agents: advisory unless enforce env (future)
                insert_kv("compact-policy-wired", 1);
                insert_kv("schema-2500", kCompactPolicyIssue);
                insert_kv("issue-2500", kCompactPolicyIssue);
                insert_kv("schema", kCompactPolicyIssue);
                // Lineage sentinels (subsystem queries remain authoritative).
                insert_kv("schema-2379", 2379);
                insert_kv("schema-2405", 2405);
                insert_kv("schema-2004", 2004);

                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            });
    };
    register_compact_policy("query:compact-policy");
    register_compact_policy("orch:compact-policy");
    register_compact_policy("arena:recommend-compact");

    // Issue #574: query:coercion-elim-stats. Returns the sum of
    // 4 coercion elimination observability counters:
    //   - coercion_castop_emitted_total (total CastOps from lowering)
    //   - dead_coercion_eliminated_total (identity/no-op elisions)
    //   - coercion_narrow_evidence_hits_total (runtime-check elisions
    //     proved away by narrow_evidence Rule 6)
    //   - narrowing_provenance_total (blame/provenance preserved on
    //     occurrence-narrowing paths feeding coercion metadata)
    ObservabilityPrims::register_stats_impl(
        "query:coercion-elim-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t total_castop =
                m->coercion_castop_emitted_total.load(std::memory_order_relaxed);
            const std::uint64_t eliminated =
                m->dead_coercion_eliminated_total.load(std::memory_order_relaxed);
            const std::uint64_t runtime_check_elided =
                m->coercion_narrow_evidence_hits_total.load(std::memory_order_relaxed);
            const std::uint64_t blame =
                m->narrowing_provenance_total.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(total_castop + eliminated +
                                                      runtime_check_elided + blame));
        });

    // Issue #306: query:linear-ownership-stats. Returns the
    // sum of 4 hardware resource linear-ownership observability
    // counters (EDA track — wire/reg/mem/port borrow + double-
    // drive detection):
    //   - hw_resource_wire_borrows_    (# of Wire resource
    //     borrows issued by the lowerer)
    //   - hw_resource_reg_writes_      (# of Reg resource
    //     writes issued by the lowerer)
    //   - hw_resource_mem_access_     (# of Mem resource
    //     accesses issued by the lowerer)
    //   - hw_resource_double_drive_   (# of double-drive
    //     violations caught at compile time — should be 0
    //     in correct hardware code; > 0 = EDA bug)
    //
    // P0: returns an integer = sum of the 4 counters.
    // Follow-up: returns a 4-tuple
    // (wire-borrows reg-writes mem-access double-drive) so
    // the AI Agent can react to double-drive > 0 as a hard
    // alert (hardware simulation safety).
    //
    // Non-duplicative with #556 (query:edsl-concurrency-stats)
    // — the latter is general EDSL concurrency; this primitive
    // is the EDA-specific hardware-resource linear-ownership
    // observability.
    ObservabilityPrims::register_stats_impl(
        "query:linear-ownership-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t wire_borrows =
                m->hw_resource_wire_borrows_.load(std::memory_order_relaxed);
            const std::uint64_t reg_writes =
                m->hw_resource_reg_writes_.load(std::memory_order_relaxed);
            const std::uint64_t mem_access =
                m->hw_resource_mem_access_.load(std::memory_order_relaxed);
            const std::uint64_t double_drive =
                m->hw_resource_double_drive_.load(std::memory_order_relaxed);
            return make_int(
                static_cast<std::int64_t>(wire_borrows + reg_writes + mem_access + double_drive));
        });

    // Issue #575: query:linear-ownership-incremental-stats. Returns
    // the sum of 4 Task2 PerDefUse + ownership_dirty incremental
    // linear ownership counters:
    //   - ownership_revalidate_count: linear_post_mutate_revalidations_total
    //   - dirty_linear_uses: per_defuse_index_visited_total
    //     (O(uses) selective re-validation proxy)
    //   - violation_caught_post_mutate: linear_violations_caught_total
    //     + linear_leak_prevented_total
    //   - escape_analysis_hits: linear_check_pass_count_
    //     (runtime linear ownership_state fast-path checks)
    ObservabilityPrims::register_stats_impl(
        "query:linear-ownership-incremental-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t revalidate =
                m->linear_post_mutate_revalidations_total.load(std::memory_order_relaxed);
            const std::uint64_t dirty_uses =
                m->per_defuse_index_visited_total.load(std::memory_order_relaxed);
            const std::uint64_t violations =
                m->linear_violations_caught_total.load(std::memory_order_acquire) /* #1867 */;
            const std::uint64_t leaks =
                m->linear_leak_prevented_total.load(std::memory_order_relaxed);
            const std::uint64_t escape_hits =
                m->linear_check_pass_count_.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(revalidate + dirty_uses + violations + leaks +
                                                      escape_hits));
        });

    // Issue #2988: query:mutate-invalidate-stats — mutate success →
    // DefUse / IR / JIT close-loop (dirty_nodes, defuse_bumps, jit, binding_gen).
    ObservabilityPrims::register_stats_impl(
        "query:mutate-invalidate-stats",
        [&string_heap, &ev](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
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
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
            auto load = [&](const std::atomic<std::uint64_t>& a) -> std::int64_t {
                return m ? static_cast<std::int64_t>(a.load(std::memory_order_relaxed)) : 0;
            };
            insert_kv("dirty-nodes", m ? load(m->mutate_invalidate_dirty_nodes_total) : 0);
            insert_kv("defuse-bumps", m ? load(m->mutate_invalidate_defuse_bumps_total) : 0);
            insert_kv("jit-invalidate-count", m ? load(m->mutate_invalidate_jit_total) : 0);
            insert_kv("binding-gen-bumps",
                      m ? load(m->mutate_invalidate_binding_gen_bumps_total) : 0);
            insert_kv("coarse-fallback-total",
                      m ? load(m->mutate_invalidate_coarse_fallback_total) : 0);
            insert_kv("precise-wired",
                      m ? static_cast<std::int64_t>(
                              m->mutate_invalidate_precise_wired.load(std::memory_order_relaxed))
                        : 1);
            insert_kv("schema-2988", 2988);
            insert_kv("issue-2988", 2988);
            insert_kv("schema-2038", 2038);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #610: query:linear-ownership-mutation-stats. Returns
    // the sum of 4 post-mutation linear ownership observability
    // counters:
    //   - post_mutate_revalidations: linear_post_mutate_revalidations_total
    //   - violations_caught: linear_violations_caught_total
    //   - deopt_on_linear: linear_deopt_on_invalidate_total
    //   - leak_prevented: linear_leak_prevented_total
    ObservabilityPrims::register_stats_impl(
        "query:linear-ownership-mutation-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t revalidations =
                m->linear_post_mutate_revalidations_total.load(std::memory_order_relaxed);
            const std::uint64_t violations =
                m->linear_violations_caught_total.load(std::memory_order_acquire) /* #1867 */;
            const std::uint64_t deopt =
                m->linear_deopt_on_invalidate_total.load(std::memory_order_relaxed);
            const std::uint64_t leaks =
                m->linear_leak_prevented_total.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(revalidations + violations + deopt + leaks));
        });

    // Issue #638: query:linear-ownership-safety-stats. Returns the
    // sum of 3 runtime linear + GuardShape post-mutation safety
    // counters (non-duplicative with #610 mutation-stats,
    // #575 incremental-stats, #306 hw linear-ownership-stats):
    //   - violations_caught: linear_violations_caught_total
    //   - deopt_on_linear_mismatch: linear_deopt_on_mismatch_total
    //   - post_mutate_enforcements: linear_post_mutate_enforcements_total
    ObservabilityPrims::register_stats_impl(
        "query:linear-ownership-safety-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t violations =
                m->linear_violations_caught_total.load(std::memory_order_acquire) /* #1867 */;
            const std::uint64_t deopt_mismatch =
                m->linear_deopt_on_mismatch_total.load(std::memory_order_relaxed);
            const std::uint64_t enforcements =
                m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(violations + deopt_mismatch + enforcements));
        });

    // Issue #1543: query:linear-gc-root-audit-log — read-only accessor for
    // the GC root registration consistency audit ring + counter snapshot.
    // Schema fields (see docs/design/linear-gc-roots.md):
    //   audit-checks-total, last-path, last-ok, last-registrations,
    //   last-stale-hits, last-violations, last-env-version-resync,
    //   last-live-roots, log-size, schema=1543
    // Optional arg0 int = max recent log lines returned under "log" as a
    // list of strings (default 0 = summary hash only).
    ObservabilityPrims::register_stats_impl(
        "query:linear-gc-root-audit-log",
        [&ev, &string_heap, &pairs](std::span<const EvalValue> a) -> EvalValue {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            const std::int64_t checks =
                m ? static_cast<std::int64_t>(
                        m->linear_gc_root_audit_checks_total.load(std::memory_order_relaxed))
                  : 0;
            const auto total = ev.linear_gc_root_audit_total();
            const auto seq = ev.linear_gc_root_audit_seq();

            std::int64_t last_ok = 1;
            std::int64_t last_path = -1;
            std::int64_t last_reg = 0;
            std::int64_t last_stale = 0;
            std::int64_t last_viol = 0;
            std::int64_t last_resync = 0;
            std::int64_t last_live = 0;
            std::string last_path_name = "none";
            if (seq > 0) {
                const auto& e = ev.linear_gc_root_audit_entry_at(seq - 1);
                last_ok = e.ok;
                last_path = e.path;
                last_reg = static_cast<std::int64_t>(e.registrations);
                last_stale = static_cast<std::int64_t>(e.stale_hits);
                last_viol = static_cast<std::int64_t>(e.violations_prevented);
                last_resync = static_cast<std::int64_t>(e.env_version_resync);
                last_live = static_cast<std::int64_t>(e.live_roots);
                last_path_name = std::string(Evaluator::linear_gc_root_audit_path_name(e.path));
            }

            // Optional recent log as linked list of strings.
            EvalValue log_list = make_void();
            std::size_t limit = 0;
            if (!a.empty() && is_int(a[0]) && as_int(a[0]) > 0)
                limit = static_cast<std::size_t>(as_int(a[0]));
            for (std::size_t i = 0; i < limit && i < Evaluator::kLinearGcRootAuditRingSize; ++i) {
                if (seq <= i)
                    break;
                const auto& entry = ev.linear_gc_root_audit_entry_at(seq - 1 - i);
                if (entry.seq == 0 && entry.path == 0 && entry.registrations == 0)
                    continue;
                auto line = std::format(
                    "seq={} path={} ok={} reg={} stale={} viol={} resync={} live={}", entry.seq,
                    Evaluator::linear_gc_root_audit_path_name(entry.path), entry.ok,
                    entry.registrations, entry.stale_hits, entry.violations_prevented,
                    entry.env_version_resync, entry.live_roots);
                auto sidx = string_heap.size();
                string_heap.push_back(std::move(line));
                auto pid = pairs.size();
                pairs.push_back({make_string(sidx), log_list});
                log_list = make_pair(pid);
            }

            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const std::string& k_str, EvalValue v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k_str)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = string_heap.size();
                string_heap.push_back(k_str);
                EvalValue key_ev = make_string(kidx);
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto slot = ((h >> 1) + at) & (hcap - 1);
                    if (meta[slot] == 0xFF) {
                        meta[slot] = fp;
                        keys[slot] = key_ev.val;
                        vals[slot] = v.val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("audit-checks-total", make_int(checks));
            insert_kv("log-size", make_int(static_cast<std::int64_t>(total)));
            insert_kv("last-path", make_int(last_path));
            {
                auto sidx = string_heap.size();
                string_heap.push_back(last_path_name);
                insert_kv("last-path-name", make_string(sidx));
            }
            insert_kv("last-ok", make_int(last_ok));
            insert_kv("last-registrations", make_int(last_reg));
            insert_kv("last-stale-hits", make_int(last_stale));
            insert_kv("last-violations", make_int(last_viol));
            insert_kv("last-env-version-resync", make_int(last_resync));
            insert_kv("last-live-roots", make_int(last_live));
            insert_kv("log", log_list);
            // Issue #1599: closed-loop refine — issue + lineage schema + wiring flags.
            insert_kv("linear_gc_root_audit_checks_total", make_int(checks));
            insert_kv("walk-active-closures-wired", make_int(1));
            insert_kv("six-touchpoints-documented", make_int(1));
            insert_kv("issue", make_int(1599));
            insert_kv("schema", make_int(1599)); // lineage 1543
            // Issue #2642: Phase 5 linear-root consistency scan counters.
            insert_kv("component-linear-densify-scan-mismatch-total",
                      make_int(static_cast<std::int64_t>(
                          m ? m->linear_densify_scan_mismatch_total.load() : 0)));
            insert_kv("component-linear-densify-scan-mismatch-observe-total",
                      make_int(static_cast<std::int64_t>(
                          m ? m->linear_densify_scan_mismatch_observe_total.load() : 0)));
            insert_kv("linear-densify-scan-mismatch-total",
                      make_int(static_cast<std::int64_t>(
                          m ? m->linear_densify_scan_mismatch_total.load() : 0)));
            insert_kv("linear_densify_scan_mismatch_total",
                      make_int(static_cast<std::int64_t>(
                          m ? m->linear_densify_scan_mismatch_total.load() : 0)));
            insert_kv("linear-densify-scan-mismatch-observe-total",
                      make_int(static_cast<std::int64_t>(
                          m ? m->linear_densify_scan_mismatch_observe_total.load() : 0)));
            insert_kv("linear_densify_scan_mismatch_observe_total",
                      make_int(static_cast<std::int64_t>(
                          m ? m->linear_densify_scan_mismatch_observe_total.load() : 0)));
            insert_kv("linear-densify-wired", make_int(1));
            insert_kv("schema-2642", make_int(2642));
            insert_kv("issue-2642", make_int(2642));
            // Issue #2673: hard-path lock for densify linear-root consistency
            // scan (refine #2642 residual). Production/Full mismatch forces
            // force_linear_rollback(LinearDensifyRootMismatch) under
            // linear_ops_present. Schema sentinel + hard-path wired flag.
            insert_kv("linear-densify-hard-path-wired", make_int(1));
            insert_kv("schema-2673", make_int(2673));
            insert_kv("issue-2673", make_int(2673));
            // Issue #2984: compact linear-root family (align #2673).
            insert_kv("linear-compact-root-check-total",
                      make_int(static_cast<std::int64_t>(
                          aura::compiler::typed_audit::linear_compact_root_check_total_v_read())));
            insert_kv("linear-compact-root-mismatch-observe-total",
                      make_int(static_cast<std::int64_t>(
                          aura::compiler::typed_audit::
                              linear_compact_root_mismatch_observe_total_v_read())));
            insert_kv(
                "linear-compact-root-mismatch-total",
                make_int(static_cast<std::int64_t>(
                    aura::compiler::typed_audit::linear_compact_root_mismatch_total_v_read())));
            insert_kv("linear-compact-root-mismatch-wired", make_int(1));
            insert_kv("schema-2984", make_int(2984));
            insert_kv("issue-2984", make_int(2984));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1568 / #1596 / #1606 / #1659 / #1895 / #1928:
    // query:linear-boundary-consistency-stats — unified mutation/compact/
    // JIT/fiber/GC boundary enforce closed-loop (walk_active_closures +
    // live-closure linear scan + force Drop + GC root).
    // Schema **1895** lineage; **schema-1928** closes #1928 AC surface.
    // Metrics: linear_post_mutate_enforcements, linear_live_closure_scans_total,
    // linear_ownership_violation_prevented, linear_gc_root_audit_checks_total,
    // linear_live_closures_marked_invalid_total (#1606 AC).
    // #1895/#1928: NULL_ENV_ID force Drop + all 5+ boundary wire flags.
    ObservabilityPrims::register_stats_impl(
        "query:linear-boundary-consistency-stats",
        [&ev, &string_heap, &pairs](std::span<const EvalValue> a) -> EvalValue {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            // Issue #1867: acquire so release-stores from record_linear_gc_probe
            // violations are visible to this audit/stats reader.
            const auto L = [&](const std::atomic<std::uint64_t>* field) -> std::int64_t {
                return field ? static_cast<std::int64_t>(field->load(std::memory_order_acquire))
                             : 0;
            };
            // Optional log lines of linear violation provenance.
            EvalValue viol_log = make_void();
            std::size_t limit = 0;
            if (!a.empty() && is_int(a[0]) && as_int(a[0]) > 0)
                limit = static_cast<std::size_t>(as_int(a[0]));
            const auto vseq = ev.linear_violation_audit_seq();
            for (std::size_t i = 0; i < limit && i < Evaluator::kLinearViolationAuditRingSize;
                 ++i) {
                if (vseq <= i)
                    break;
                const auto& e = ev.linear_violation_audit_entry_at(vseq - 1 - i);
                if (e.seq == 0 && e.reason == 0)
                    continue;
                auto line =
                    std::format("seq={} path={} reason={} epoch={} defuse={} env={} closure={}",
                                e.seq, Evaluator::linear_gc_root_audit_path_name(e.path), e.reason,
                                e.epoch, e.defuse_version, e.env_id, e.closure_id);
                auto sidx = string_heap.size();
                string_heap.push_back(std::move(line));
                auto pid = pairs.size();
                pairs.push_back({make_string(sidx), viol_log});
                viol_log = make_pair(pid);
            }
            // Capacity power-of-two; #1659 adds mandate wire keys.
            auto* ht = FlatHashTable::create(64);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const std::string& k_str, EvalValue v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k_str)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = string_heap.size();
                string_heap.push_back(k_str);
                EvalValue key_ev = make_string(kidx);
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto slot = ((h >> 1) + at) & (hcap - 1);
                    if (meta[slot] == 0xFF) {
                        meta[slot] = fp;
                        keys[slot] = key_ev.val;
                        vals[slot] = v.val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("schema", make_int(1895)); // #1895 closed-loop; lineage 1659/1606/1568
            insert_kv("issue", make_int(1895));
            insert_kv("schema-1928", make_int(1928)); // #1928 walk_active_closures mandate
            insert_kv("issue-1928", make_int(1928));
            insert_kv("schema-1929", make_int(1929)); // #1929 Closure Bridge unified surface
            insert_kv("issue-1929", make_int(1929));
            insert_kv("schema-1954", make_int(1954)); // #1954 refine of #1929
            insert_kv("issue-1954", make_int(1954));
            insert_kv("active", make_int(1));
            insert_kv("phase", make_int(5)); // production + all-boundary mandate (#1895/#1928)
            insert_kv("truncate-scan-wired", make_int(1));    // #1928 truncate force Drop
            insert_kv("boundaries-wired-count", make_int(6)); // inv/compact/trunc/jit/fiber/gc
            insert_kv("closure-view-lifetime-paired",
                      make_int(1)); // pairs query:closure-view-lifetime-stats
            insert_kv("linear-post-mutate-enforcements",
                      make_int(L(m ? &m->linear_post_mutate_enforcements : nullptr)));
            // #1596 AC5 alias (underscore form) + hyphen form for agents.
            insert_kv("linear_post_mutate_enforcements",
                      make_int(L(m ? &m->linear_post_mutate_enforcements : nullptr)));
            insert_kv("linear_live_closure_scans_total",
                      make_int(L(m ? &m->linear_live_closure_scans_total : nullptr)));
            insert_kv("linear-live-closure-scans-total",
                      make_int(L(m ? &m->linear_live_closure_scans_total : nullptr)));
            insert_kv("linear_live_closures_marked_invalid_total",
                      make_int(L(m ? &m->linear_live_closures_marked_invalid_total : nullptr)));
            insert_kv("linear-ownership-violation-prevented",
                      make_int(L(m ? &m->linear_ownership_violation_prevented : nullptr)));
            insert_kv("linear_ownership_violation_prevented",
                      make_int(L(m ? &m->linear_ownership_violation_prevented : nullptr)));
            // #1659 AC: issue-body metric names (aliases of existing counters).
            insert_kv("linear-violation-count",
                      make_int(L(m ? &m->linear_violations_caught_total : nullptr)));
            insert_kv("linear_violations_caught_total",
                      make_int(L(m ? &m->linear_violations_caught_total : nullptr)));
            insert_kv("linear-gc-root-audit-checks",
                      make_int(L(m ? &m->linear_gc_root_audit_checks_total : nullptr)));
            insert_kv("linear_gc_root_audit_checks_total",
                      make_int(L(m ? &m->linear_gc_root_audit_checks_total : nullptr)));
            insert_kv("boundary-consistency-total",
                      make_int(L(m ? &m->linear_boundary_consistency_total : nullptr)));
            insert_kv("epoch-fence-enforce-total",
                      make_int(L(m ? &m->linear_epoch_fence_enforce_total : nullptr)));
            insert_kv("force-drop-total", make_int(L(m ? &m->linear_force_drop_total : nullptr)));
            // #1606 AC wire flags (Evaluator + service + JIT ResourceTracker)
            insert_kv("walk-active-closures-wired", make_int(1));
            insert_kv("invalidate-scan-wired", make_int(1));
            insert_kv("compact-scan-wired", make_int(1));
            insert_kv("jit-resource-tracker-scan-wired", make_int(1));
            insert_kv("force-drop-wired", make_int(1));
            // #1659 mandate wire flags (linear_ownership_state + heap + GC/Arena)
            insert_kv("envframe-linear-ownership-snapshot-wired", make_int(1));
            insert_kv("linear-heap-runtime-wired", make_int(1));
            insert_kv("linear-ownership-state-propagated-wired", make_int(1));
            insert_kv("apply-closure-linear-check-wired", make_int(1));
            insert_kv("jit-linear-post-mutate-enforce-wired", make_int(1));
            // #1895: all boundary paths + NULL_ENV_ID linear body safety
            insert_kv("fiber-steal-scan-wired", make_int(1));
            insert_kv("gc-safepoint-scan-wired", make_int(1));
            insert_kv("guard-exit-scan-wired", make_int(1));
            insert_kv("materialize-null-env-wired", make_int(1));
            insert_kv("null-env-force-drop-wired", make_int(1));
            insert_kv("linear_null_env_force_drop_total",
                      make_int(L(m ? &m->linear_null_env_force_drop_total : nullptr)));
            insert_kv("linear_null_env_safe_fallback_total",
                      make_int(L(m ? &m->linear_null_env_safe_fallback_total : nullptr)));
            insert_kv("linear_post_mutate_null_env_id_total",
                      make_int(L(m ? &m->linear_post_mutate_null_env_id_total : nullptr)));
            insert_kv("schema-1659", make_int(1659)); // lineage sentinel for agents
            insert_kv("invalidate-tombstone-wired", make_int(1));
            insert_kv("gc-arena-linear-synergy-wired", make_int(1));
            insert_kv("guardshape-linear-unified-wired", make_int(1));
            insert_kv("linear-ownership-mandate-active", make_int(1));
            insert_kv("violation-audit-total",
                      make_int(static_cast<std::int64_t>(ev.linear_violation_audit_total())));
            insert_kv("violation-log", viol_log);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #800: query:linear-postmutate-fidelity-stats — linear ownership
    // post-mutate / rollback / steal / EnvFrame fidelity dashboard
    // (refines #793/#792/#784/#791; non-duplicative with #763 gc-compiler
    // stats and #638 linear-ownership-safety-stats).
    //
    // Fields (4 + sentinel):
    //   - post-rollback-revalidate-hits  linear_postmutate_post_rollback_revalidate_total
    //   - escape-violations-prevented    linear_postmutate_escape_violations_prevented_total
    //   - guard-boundary-linear-safe     linear_postmutate_guard_boundary_linear_safe_total
    //   - env-version-sync               linear_postmutate_env_version_sync_total
    //   - schema == 800
    ObservabilityPrims::register_stats_impl(
        "query:linear-postmutate-fidelity-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::int64_t post_rollback =
                m ? static_cast<std::int64_t>(
                        m->linear_postmutate_post_rollback_revalidate_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t escape_prevented =
                m ? static_cast<std::int64_t>(
                        m->linear_postmutate_escape_violations_prevented_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t guard_safe =
                m ? static_cast<std::int64_t>(
                        m->linear_postmutate_guard_boundary_linear_safe_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t env_sync =
                m ? static_cast<std::int64_t>(
                        m->linear_postmutate_env_version_sync_total.load(std::memory_order_relaxed))
                  : 0;
            // #2043 window keys + #2131 GcCoordScope metrics need ≥64 slots.
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
            insert_kv("post-rollback-revalidate-hits", post_rollback);
            insert_kv("escape-violations-prevented", escape_prevented);
            insert_kv("guard-boundary-linear-safe", guard_safe);
            insert_kv("env-version-sync", env_sync);
            // Issue #2043: atomic linear+GC window observability (additive)
            insert_kv("linear-gc-window-finalize-total",
                      m ? static_cast<std::int64_t>(
                              m->linear_gc_window_finalize_total.load(std::memory_order_relaxed))
                        : 0);
            insert_kv("linear-ownership-epoch-bumps",
                      m ? static_cast<std::int64_t>(
                              m->linear_ownership_epoch_bumps_total.load(std::memory_order_relaxed))
                        : 0);
            insert_kv("linear-gc-window-under-mutate",
                      m ? static_cast<std::int64_t>(m->linear_gc_window_under_mutate_total.load(
                              std::memory_order_relaxed))
                        : 0);
            insert_kv("linear-ownership-epoch",
                      static_cast<std::int64_t>(ev->linear_ownership_epoch()));
            insert_kv("schema", 800); // lineage retained for #800 tests
            insert_kv("schema-2043", 2043);
            insert_kv("issue-2043", 2043);
            insert_kv("linear-gc-window-wired", 1);
            // Issue #2131: unified GcCoordScope pin → cascade → audit metrics
            insert_kv("gc-coord-scopes-opened",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::scopes_opened_total.load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-pre-pin-total",
                      static_cast<std::int64_t>(
                          aura::compiler::gc_coord::pre_pin_total.load(std::memory_order_relaxed)));
            insert_kv("gc-coord-cascade-enter-total",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::cascade_enter_total.load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-post-audit-total",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::post_audit_total.load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-released-total",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::released_total.load(
                          std::memory_order_relaxed)));
            insert_kv(
                "gc-coord-phase-violations",
                static_cast<std::int64_t>(aura::compiler::gc_coord::phase_violations_total.load(
                    std::memory_order_relaxed)));
            insert_kv(
                "gc-coord-missing-post-audit",
                static_cast<std::int64_t>(aura::compiler::gc_coord::missing_post_audit_total.load(
                    std::memory_order_relaxed)));
            insert_kv("gc-coord-reverse-order",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::reverse_order_total.load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-path-invalidate",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::scopes_by_path[0].load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-path-soft-dirty",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::scopes_by_path[1].load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-path-boundary",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::scopes_by_path[2].load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-path-hot-swap",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::scopes_by_path[3].load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-path-compact",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::scopes_by_path[4].load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-wired", 1);
            insert_kv("schema-2131", 2131);
            insert_kv("issue-2131", 2131);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #598: query:linear-ownership-runtime-stats. Returns the
    // sum of 4 runtime linear enforcement counters spanning
    // Interpreter/JIT hot-path + invalidate_function integration
    // (non-duplicative with #638 safety-stats which omits invalidate
    // deopt; #610 mutation-stats which includes revalidations/leaks):
    //   - violations_caught: linear_violations_caught_total
    //   - deopt_on_linear_mismatch: linear_deopt_on_mismatch_total
    //   - post_mutate_enforcement_hits:
    //     linear_post_mutate_enforcements_total
    //   - deopt_on_invalidate: linear_deopt_on_invalidate_total
    ObservabilityPrims::register_stats_impl(
        "query:linear-ownership-runtime-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t violations =
                m->linear_violations_caught_total.load(std::memory_order_acquire) /* #1867 */;
            const std::uint64_t deopt_mismatch =
                m->linear_deopt_on_mismatch_total.load(std::memory_order_relaxed);
            const std::uint64_t enforcement_hits =
                m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed);
            const std::uint64_t deopt_invalidate =
                m->linear_deopt_on_invalidate_total.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(violations + deopt_mismatch +
                                                      enforcement_hits + deopt_invalidate));
        });

    // Issue #454: query:reflect-edsl-bridge-stats. Returns the
    // sum of 4 reflection-to-EDSL bridge observability counters:
    //   - schema_validation_pass_count_  (auto_validate hook)
    //   - schema_validation_fail_count_  (validation failures)
    //   - impact_snapshot_count_         (post-mutate reflection data)
    //   - macro_introduced_skipped_in_query_  (SyntaxMarker filter
    //     introspection via query:pattern / schema-of-marker bridge)
    ObservabilityPrims::register_stats_impl(
        "query:reflect-edsl-bridge-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t pass = ev->get_schema_validation_pass_count();
            const std::uint64_t fail = ev->get_schema_validation_fail_count();
            const std::uint64_t snapshots = ev->get_impact_snapshot_count();
            const std::uint64_t marker_skips = ev->get_macro_introduced_skipped_in_query();
            return make_int(static_cast<std::int64_t>(pass + fail + snapshots + marker_skips));
        });

    // Issue #502 / #551 / #1611: query:reflect-postmutate-stats — Guard
    // post-mutate reflect validation + MacroIntroduced hygiene gate.
    // Schema **1611** (lineage 502/551). AC keys:
    //   reflect-macro-hygiene-checks / reflect-macro-hygiene-rejects
    //   allow-macro-mutate / hygiene-aware-validate-wired
    ObservabilityPrims::register_stats_impl(
        "query:reflect-postmutate-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(24);
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
            CompilerMetrics* m = ev->compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev->compiler_metrics())
                                     : nullptr;
            const std::uint64_t snapshots = ev->get_impact_snapshot_count();
            const std::uint64_t pass = ev->get_schema_validation_pass_count();
            const std::uint64_t fail = ev->get_schema_validation_fail_count();
            const std::uint64_t dirty = ev->get_dirty_nodes_in_snapshot();
            const std::uint64_t markers = ev->get_macro_markers_in_snapshot();
            const std::uint64_t hygiene_checks =
                m ? m->reflect_macro_hygiene_checks_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t hygiene_rejects =
                m ? m->reflect_macro_hygiene_rejects_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t total = snapshots + pass + fail + dirty;
            std::int64_t recommendation = 0;
            if (fail > 0 || !ev->get_last_schema_validation_ok())
                recommendation = 2;
            else if (dirty > 50)
                recommendation = 1;
            // #502/#551 lineage
            insert_kv("impact-snapshots", static_cast<std::int64_t>(snapshots));
            insert_kv("schema-pass", static_cast<std::int64_t>(pass));
            insert_kv("schema-fail", static_cast<std::int64_t>(fail));
            insert_kv("dirty-nodes", static_cast<std::int64_t>(dirty));
            insert_kv("macro-markers", static_cast<std::int64_t>(markers));
            insert_kv("schema-valid", ev->get_last_schema_validation_ok() ? 1 : 0);
            insert_kv("reflect-postmutate-total", static_cast<std::int64_t>(total));
            insert_kv("reflect-postmutate-recommendation", recommendation);
            // #1611 AC keys (folded — no new public query:*-stats)
            insert_kv("reflect-macro-hygiene-checks", static_cast<std::int64_t>(hygiene_checks));
            insert_kv("reflect-macro-hygiene-rejects", static_cast<std::int64_t>(hygiene_rejects));
            insert_kv("allow-macro-mutate", ev->get_allow_macro_mutate() ? 1 : 0);
            insert_kv("hygiene-aware-validate-wired", 1);
            insert_kv("post-mutation-macro-check-wired", 1);
            insert_kv("deserialize-hygiene-wired", 1);
            insert_kv("issue", 1611);
            insert_kv("schema", 1611); // lineage 502 / 551 / 750
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #594: query:reflection-selfmod-stats. Returns the sum of
    // 5 static-reflection + self-mod validation observability counters
    // spanning the Task6 Guard post-mutate validate hook (#551) and
    // mutate:* self-evolution paths (non-duplicative with #551
    // reflect-postmutate 4-tuple and #454 reflect-edsl-bridge):
    //   - post_mutate_validate_pass: schema_validation_pass_count_
    //   - schema_violations_prevented: schema_validation_fail_count_
    //   - validations_run proxy: impact_snapshot_count_ (Guard hook
    //     invocations — each successful mutate triggers validate)
    //   - mutation_impact_count_  (successful Guard self-mod transforms)
    //   - guard_dirty_epoch_count_  (Guard + schema/type integration)
    //
    // P0: returns an integer = sum of all 5 counter groups.
    // validations_run (pass + fail) is derivable by the Agent;
    // follow-up returns a 5-tuple for independent pass-rate tracking.
    ObservabilityPrims::register_stats_impl(
        "query:reflection-selfmod-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t pass = ev->get_schema_validation_pass_count();
            const std::uint64_t fail = ev->get_schema_validation_fail_count();
            const std::uint64_t snapshots = ev->get_impact_snapshot_count();
            const std::uint64_t impact = ev->get_mutation_impact_count();
            const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
            return make_int(
                static_cast<std::int64_t>(pass + fail + snapshots + impact + guard_epoch));
        });

    // Issue #750 / #1611: (reflect:validate-macro-body node-id [:allow-macro? #t])
    // Runtime hygiene/schema check on MacroIntroduced subtrees before Guard
    // commit. Default rejects unclean MacroIntroduced provenance; pass
    // :allow-macro? #t or (hygiene:set-allow-macro-mutate! #t) to relax.
    add("reflect:validate-macro-body", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        const auto nid = static_cast<aura::ast::NodeId>(as_int(a[0]));
        // Issue #1611: optional :allow-macro? kwarg (or global allow flag).
        bool allow = ev.get_allow_macro_mutate();
        const auto& kt = ev.keyword_table();
        std::size_t allow_kw = std::string::npos;
        for (std::size_t i = 0; i < kt.size(); ++i) {
            if (kt[i] == ":allow-macro?") {
                allow_kw = i;
                break;
            }
        }
        if (allow_kw != std::string::npos) {
            for (std::size_t i = 0; i + 1 < a.size(); ++i) {
                if (is_keyword(a[i]) && as_keyword_idx(a[i]) == allow_kw && is_bool(a[i + 1])) {
                    allow = as_bool(a[i + 1]);
                    break;
                }
            }
        }
        auto result = runtime_reflect_validate_ast_subtree(*ws, nid, false);
        // Issue #1611: without allow, unclean macro subtree is a typed reject.
        if (!allow && result.macro_markers > 0 && !result.hygiene_held)
            result.ok = false;
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            bump_reflection_schema_metrics(m, result);
            m->reflect_macro_hygiene_checks_total.fetch_add(1, std::memory_order_relaxed);
            if (!result.ok && !allow)
                m->reflect_macro_hygiene_rejects_total.fetch_add(1, std::memory_order_relaxed);
        }
        return make_bool(result.ok);
    });

    // Issue #750: (reflect:validate-edsl node-id) — runtime schema check for
    // SV verification EDSL nodes (Constraint/Class/Covergroup/SVA/etc.).
    add("reflect:validate-edsl", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        const auto nid = static_cast<aura::ast::NodeId>(as_int(a[0]));
        const auto result = runtime_reflect_validate_ast_subtree(*ws, nid, true);
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            bump_reflection_schema_metrics(m, result);
        return make_bool(result.ok);
    });

    // Issue #2020: (engine:metrics "reflect:hygiene-stats" [node-id]) — Agent-visible live
    // hygiene snapshot for expand → diagnose → mutate/rollback closed loops.
    // No arg / void: process-wide atomics + workspace MacroIntroduced counts.
    // With node-id: also counts MacroIntroduced / dirty / stale under that root
    // (bounded walk; no allocation beyond the hash table).
    // Cheap: relaxed atomics + optional O(subtree) walk only when root given.
    // Issue #2628: private; use (engine:metrics "reflect:hygiene-stats" [node]).
    ObservabilityPrims::register_stats_impl(
        "reflect:hygiene-stats", [&ev, &string_heap](const auto& a) -> EvalValue {
            using aura::compiler::macro_exp::g_hygiene_tracer_depth_max;
            using aura::compiler::macro_exp::g_hygiene_tracer_expansions;
            using aura::compiler::macro_exp::g_hygiene_violation_in_macro_expand_total;
            using aura::compiler::macro_exp::g_macro_clone_concurrent_fiber_total;
            using aura::compiler::macro_exp::g_macro_clone_hygiene_dirty_total;
            using aura::compiler::macro_exp::g_macro_expansion_total;
            using aura::compiler::macro_exp::g_macro_origin_provenance_errors;
            using aura::compiler::macro_exp::g_macro_rest_param_hygiene_total;
            using aura::compiler::macro_exp::g_macro_restamp_after_flat_total;
            using aura::compiler::macro_exp::MAX_HYGIENE_DEPTH;

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

            // Process-wide expand/hygiene atomics (relaxed; Agent dashboard).
            const std::int64_t violation_count = static_cast<std::int64_t>(
                g_hygiene_violation_in_macro_expand_total.load(std::memory_order_relaxed) +
                ev.get_hygiene_violation_count());
            const std::int64_t provenance_errors = static_cast<std::int64_t>(
                g_macro_origin_provenance_errors.load(std::memory_order_relaxed));
            const std::int64_t max_depth = static_cast<std::int64_t>(
                g_hygiene_tracer_depth_max.load(std::memory_order_relaxed));
            const std::int64_t dirty_nodes = static_cast<std::int64_t>(
                g_macro_clone_hygiene_dirty_total.load(std::memory_order_relaxed));
            const std::int64_t concurrent_fiber = static_cast<std::int64_t>(
                g_macro_clone_concurrent_fiber_total.load(std::memory_order_relaxed));
            const std::int64_t expansions =
                static_cast<std::int64_t>(g_macro_expansion_total.load(std::memory_order_relaxed));
            const std::int64_t tracer_expansions = static_cast<std::int64_t>(
                g_hygiene_tracer_expansions.load(std::memory_order_relaxed));
            const std::int64_t rest_hyg = static_cast<std::int64_t>(
                g_macro_rest_param_hygiene_total.load(std::memory_order_relaxed));
            const std::int64_t restamp = static_cast<std::int64_t>(
                g_macro_restamp_after_flat_total.load(std::memory_order_relaxed));

            std::int64_t macro_markers = 0;
            std::int64_t subtree_dirty = 0;
            std::int64_t subtree_stale = 0;
            std::int64_t subtree_nodes = 0;
            std::int64_t scoped = 0;
            auto* ws = ev.workspace_flat();
            if (ws) {
                constexpr auto kExpansion = static_cast<std::uint8_t>(
                    aura::ast::FlatAST::MacroDirtyReason::kMacroExpansion);
                // Optional root: walk only that subtree; else whole flat.
                aura::ast::NodeId root = aura::ast::NULL_NODE;
                if (!a.empty() && is_int(a[0])) {
                    root = static_cast<aura::ast::NodeId>(as_int(a[0]));
                    scoped = 1;
                }
                if (scoped && (root == aura::ast::NULL_NODE || root >= ws->size() ||
                               !ws->is_live_node(root))) {
                    // Invalid root: report zeros for scoped fields; still return atomics.
                } else if (scoped) {
                    std::vector<aura::ast::NodeId> stack;
                    std::vector<std::uint8_t> seen(ws->size(), 0);
                    stack.push_back(root);
                    seen[static_cast<std::size_t>(root)] = 1;
                    while (!stack.empty()) {
                        const auto id = stack.back();
                        stack.pop_back();
                        ++subtree_nodes;
                        if (ws->is_macro_introduced(id)) {
                            ++macro_markers;
                            if ((ws->macro_dirty(id) & kExpansion) != 0)
                                ++subtree_dirty;
                            if (!ws->is_valid(id))
                                ++subtree_stale;
                        }
                        auto v = ws->get(id);
                        for (auto c : v.children) {
                            if (c == aura::ast::NULL_NODE || c >= ws->size())
                                continue;
                            if (seen[static_cast<std::size_t>(c)])
                                continue;
                            seen[static_cast<std::size_t>(c)] = 1;
                            stack.push_back(c);
                        }
                    }
                } else {
                    for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                        if (!ws->is_live_node(id))
                            continue;
                        if (ws->is_macro_introduced(id)) {
                            ++macro_markers;
                            if ((ws->macro_dirty(id) & kExpansion) != 0)
                                ++subtree_dirty;
                            if (!ws->is_valid(id))
                                ++subtree_stale;
                        }
                    }
                    subtree_nodes = static_cast<std::int64_t>(ws->size());
                }
            }

            // AC names (exact + snake_case aliases for Agent scripts).
            insert_kv("violation_count", violation_count);
            insert_kv("hygiene-violations", violation_count);
            insert_kv("provenance_errors", provenance_errors);
            insert_kv("provenance-errors", provenance_errors);
            insert_kv("max_depth", max_depth);
            insert_kv("max-depth", max_depth);
            insert_kv("hygiene-depth-max", max_depth);
            insert_kv("dirty_nodes", dirty_nodes);
            insert_kv("hygiene-dirty", dirty_nodes);
            insert_kv("concurrent_fiber_count", concurrent_fiber);
            insert_kv("concurrent-fiber-count", concurrent_fiber);
            // Issue #2021: peak concurrent top-level clone + live in-flight.
            {
                using aura::compiler::macro_exp::g_macro_clone_concurrent_peak;
                using aura::compiler::macro_exp::g_macro_clone_in_flight;
                const std::int64_t peak = static_cast<std::int64_t>(
                    g_macro_clone_concurrent_peak.load(std::memory_order_relaxed));
                const std::int64_t inflight = static_cast<std::int64_t>(
                    g_macro_clone_in_flight.load(std::memory_order_relaxed));
                insert_kv("concurrent_peak", peak);
                insert_kv("concurrent-peak", peak);
                insert_kv("macro-clone-concurrent-peak", peak);
                insert_kv("in_flight", inflight);
                insert_kv("in-flight", inflight);
                insert_kv("macro-clone-in-flight", inflight);
                insert_kv("depth-obs-wired", 1);
                insert_kv("concurrent-obs-wired", 1);
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                    aura_macro_hygiene_snapshot_metrics(m);
            }
            insert_kv("expansions", expansions);
            insert_kv("tracer-expansions", tracer_expansions);
            insert_kv("macro_markers", macro_markers);
            insert_kv("macro-markers", macro_markers);
            insert_kv("macro-dirty-nodes", subtree_dirty);
            insert_kv("stale-macro-nodes", subtree_stale);
            insert_kv("subtree-nodes", subtree_nodes);
            insert_kv("scoped", scoped);
            insert_kv("rest-param-hygiene-total", rest_hyg);
            // Issue #2169: rest gensym completeness + process serial.
            {
                using aura::compiler::macro_exp::g_macro_rest_param_hygiene_incomplete_total;
                using aura::compiler::macro_exp::g_macro_rest_gensym_serial;
                const std::int64_t incomplete = static_cast<std::int64_t>(
                    g_macro_rest_param_hygiene_incomplete_total.load(std::memory_order_relaxed));
                const std::int64_t serial = static_cast<std::int64_t>(
                    g_macro_rest_gensym_serial.load(std::memory_order_relaxed));
                insert_kv("rest-param-hygiene-incomplete-total", incomplete);
                insert_kv("rest-param-gensym-serial", serial);
                insert_kv("schema-2169", 2169);
                insert_kv("issue-2169", 2169);
                insert_kv("rest-param-hygiene-complete-wired", 1);
            }
            insert_kv("restamp-after-flat-total", restamp);
            insert_kv("max-hygiene-depth-cap", static_cast<std::int64_t>(MAX_HYGIENE_DEPTH));
            insert_kv("hard-max-depth", static_cast<std::int64_t>(MAX_HYGIENE_DEPTH));
            // Issue #2101: live effective + runtime process-wide caps.
            insert_kv("effective-max-depth",
                      static_cast<std::int64_t>(
                          aura::compiler::macro_exp::effective_hygiene_depth_limit()));
            insert_kv(
                "runtime-depth-cap",
                static_cast<std::int64_t>(aura::compiler::macro_exp::runtime_hygiene_depth_cap()));
            insert_kv(
                "self-evo-pass-cap",
                static_cast<std::int64_t>(aura::compiler::macro_exp::effective_hygiene_pass_cap()));
            insert_kv(
                "runtime-pass-cap",
                static_cast<std::int64_t>(aura::compiler::macro_exp::runtime_hygiene_pass_cap()));
            insert_kv("schema-2101", 2101);
            insert_kv("issue-2101", 2101);
            insert_kv("hygiene-limits-runtime-wired", 1);
            insert_kv("process-wide", 1);
            insert_kv("capability-tightens-only", 1);
            insert_kv("allow-macro-mutate", ev.get_allow_macro_mutate() ? 1 : 0);
            insert_kv("active", 1);
            insert_kv("schema", 2020); // Agent surface #2020; concurrent peak keys #2021 / #2101
            insert_kv("issue", 2020);
            insert_kv("depth-concurrent-obs-issue", 2021);
            // Issue #2167: Agent hygiene-diagnostic + provenance-chain surface.
            insert_kv("schema-2167", 2167);
            insert_kv("issue-2167", 2167);
            insert_kv("hygiene-diagnostic-wired", 1);
            insert_kv("macro-provenance-chain-wired", 1);

            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2020: (reflect:provenance-blame node-id) — MacroIntroduced origin
    // / expansion-site provenance for a single node. Returns void (nil) when
    // the node is not MacroIntroduced (or invalid); hash otherwise.
    add("reflect:provenance-blame", [&ev, &string_heap](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_void();
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_void();
        const auto nid = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (nid == aura::ast::NULL_NODE || nid >= ws->size() || !ws->is_live_node(nid))
            return make_void();
        if (!ws->is_macro_introduced(nid))
            return make_void(); // AC: nil if not macro-introduced

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
                    auto kidx = string_heap.size();
                    string_heap.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };

        constexpr auto kExpansion =
            static_cast<std::uint8_t>(aura::ast::FlatAST::MacroDirtyReason::kMacroExpansion);
        const auto prov = static_cast<std::int64_t>(ws->provenance(nid));
        const auto dirty = static_cast<std::int64_t>(ws->macro_dirty(nid));
        const auto parent = ws->parent_of(nid);
        insert_kv("node", static_cast<std::int64_t>(nid));
        insert_kv("macro-introduced", 1);
        insert_kv("marker", static_cast<std::int64_t>(static_cast<std::uint8_t>(ws->marker(nid))));
        insert_kv("provenance", prov);
        insert_kv("origin", prov); // expansion-site id (weak provenance stamp)
        insert_kv("gen-valid", ws->is_valid(nid) ? 1 : 0);
        insert_kv("macro-dirty", dirty);
        insert_kv("macro-expansion-dirty", (dirty & kExpansion) != 0 ? 1 : 0);
        insert_kv("parent", parent == aura::ast::NULL_NODE ? static_cast<std::int64_t>(-1)
                                                           : static_cast<std::int64_t>(parent));
        insert_kv("flat-generation", static_cast<std::int64_t>(ws->generation()));
        insert_kv("schema", 2020);
        insert_kv("issue", 2020);
        // Issue #2167: diagnostic / chain surface lineage stamp.
        insert_kv("schema-2167", 2167);
        insert_kv("issue-2167", 2167);
        insert_kv("hygiene-diagnostic-wired", 1);

        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #2167: (query:hygiene-diagnostic node-id [:depth n] [:include-chain? #t])
    // Agent-visible structured hygiene + provenance blame for a single node
    // (MacroIntroduced or user). Lazy: zero hot-path cost until queried.
    // Returns void for bad args / no workspace; hash otherwise (schema 2167).
    //
    // Fields: marker, provenance-id, macro-def-id, expansion-id, mutation-id,
    // fiber-id, violation-flags, depth-at-clone, restamp-count, + optional chain.
    sink_query_prim("query:hygiene-diagnostic", [&ev, &string_heap](const auto& a) -> EvalValue {
        using aura::compiler::macro_exp::g_hygiene_tracer_depth_max;
        using aura::compiler::macro_exp::g_hygiene_violation_in_macro_expand_total;
        using aura::compiler::macro_exp::g_macro_origin_provenance_errors;
        using aura::compiler::macro_exp::g_macro_restamp_after_flat_total;

        if (a.empty() || !is_int(a[0]))
            return make_void();
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_void();
        const auto nid = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (nid == aura::ast::NULL_NODE || nid >= ws->size() || !ws->is_live_node(nid))
            return make_void();

        // Optional kwargs: :depth n (chain walk budget), :include-chain? #t|#f
        std::int64_t depth_budget = 8;
        bool include_chain = true;
        const auto& kt = ev.keyword_table();
        for (std::size_t i = 1; i < a.size(); ++i) {
            if (!is_keyword(a[i]))
                continue;
            const auto kidx = as_keyword_idx(a[i]);
            if (kidx >= kt.size())
                continue;
            const auto& kw = kt[kidx];
            if (kw == ":depth" || kw == "depth") {
                if (i + 1 < a.size() && is_int(a[i + 1])) {
                    depth_budget = as_int(a[i + 1]);
                    ++i;
                }
            } else if (kw == ":include-chain?" || kw == ":include-chain" ||
                       kw == "include-chain?" || kw == "include-chain") {
                include_chain = true;
                if (i + 1 < a.size() && (is_bool(a[i + 1]) || is_int(a[i + 1]))) {
                    if (is_bool(a[i + 1]))
                        include_chain = as_bool(a[i + 1]);
                    else
                        include_chain = as_int(a[i + 1]) != 0;
                    ++i;
                }
            }
        }
        if (depth_budget < 0)
            depth_budget = 0;
        if (depth_budget > 64)
            depth_budget = 64;

        constexpr auto kExpansion =
            static_cast<std::uint8_t>(aura::ast::FlatAST::MacroDirtyReason::kMacroExpansion);
        constexpr auto kSelfModify =
            static_cast<std::uint8_t>(aura::ast::FlatAST::MacroDirtyReason::kMacroSelfModify);

        const bool is_mi = ws->is_macro_introduced(nid);
        const auto marker = ws->marker(nid);
        const auto prov_id = static_cast<std::int64_t>(ws->provenance(nid));
        const auto dirty = static_cast<std::uint8_t>(ws->macro_dirty(nid));
        // Weak provenance stamp: origin body id (macro expansion clone path).
        // macro-def-id / expansion-id both surface the origin; Agents correlate.
        const std::int64_t macro_def_id = prov_id;
        const std::int64_t expansion_id = prov_id;

        // Last mutation that targeted this node (scan log reverse — lazy).
        std::int64_t last_mut_id = 0;
        {
            const auto view = ws->mutation_log_view();
            for (auto it = view.rbegin(); it != view.rend(); ++it) {
                if (it->target_node == nid) {
                    last_mut_id = static_cast<std::int64_t>(it->mutation_id);
                    break;
                }
            }
        }
        const std::int64_t fiber_id = static_cast<std::int64_t>(aura_fiber_current_id());

        // violation-flags bitfield (Agent-readable):
        //   bit0 = process expand violation > 0
        //   bit1 = provenance origin error total > 0
        //   bit2 = this node macro-expansion dirty
        //   bit3 = this node self-modify dirty
        //   bit4 = gen invalid / stale
        //   bit5 = MacroIntroduced without provenance stamp
        std::int64_t viol_flags = 0;
        const auto proc_viol =
            g_hygiene_violation_in_macro_expand_total.load(std::memory_order_relaxed) +
            static_cast<std::uint64_t>(ev.get_hygiene_violation_count());
        const auto prov_err = g_macro_origin_provenance_errors.load(std::memory_order_relaxed);
        if (proc_viol > 0)
            viol_flags |= 0x01;
        if (prov_err > 0)
            viol_flags |= 0x02;
        if ((dirty & kExpansion) != 0)
            viol_flags |= 0x04;
        if ((dirty & kSelfModify) != 0)
            viol_flags |= 0x08;
        if (!ws->is_valid(nid))
            viol_flags |= 0x10;
        if (is_mi && prov_id == 0)
            viol_flags |= 0x20;

        // depth-at-clone: walk parent chain until non-MI / root (capped).
        std::int64_t depth_at_clone = 0;
        {
            auto cur = nid;
            for (int d = 0; d < 64; ++d) {
                const auto p = ws->parent_of(cur);
                if (p == aura::ast::NULL_NODE || p >= ws->size())
                    break;
                ++depth_at_clone;
                if (!ws->is_macro_introduced(p))
                    break;
                cur = p;
            }
        }
        // Prefer tracer max as secondary signal when node depth is 0.
        const std::int64_t tracer_depth =
            static_cast<std::int64_t>(g_hygiene_tracer_depth_max.load(std::memory_order_relaxed));
        const std::int64_t restamp = static_cast<std::int64_t>(
            g_macro_restamp_after_flat_total.load(std::memory_order_relaxed));

        // Optional provenance chain: follow provenance_ column (origin stamps).
        std::vector<std::int64_t> chain;
        chain.push_back(static_cast<std::int64_t>(nid));
        if (include_chain && depth_budget > 0) {
            std::uint32_t cur_prov = ws->provenance(nid);
            for (std::int64_t step = 0; step < depth_budget && cur_prov != 0; ++step) {
                const auto origin = static_cast<aura::ast::NodeId>(cur_prov);
                if (origin == nid || origin >= ws->size())
                    break;
                // Avoid cycles.
                bool seen = false;
                for (auto x : chain) {
                    if (x == static_cast<std::int64_t>(origin)) {
                        seen = true;
                        break;
                    }
                }
                if (seen)
                    break;
                chain.push_back(static_cast<std::int64_t>(origin));
                // Next hop: origin's own provenance (side-table walk).
                if (!ws->is_live_node(origin))
                    break;
                const auto next = ws->provenance(origin);
                if (next == 0 || next == cur_prov)
                    break;
                cur_prov = next;
            }
        }

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

        insert_kv("node-id", static_cast<std::int64_t>(nid));
        insert_kv("node", static_cast<std::int64_t>(nid));
        insert_kv("marker", static_cast<std::int64_t>(static_cast<std::uint8_t>(marker)));
        insert_kv("macro-introduced", is_mi ? 1 : 0);
        insert_kv("provenance-id", prov_id);
        insert_kv("provenance", prov_id);
        insert_kv("macro-def-id", macro_def_id);
        insert_kv("expansion-id", expansion_id);
        insert_kv("mutation-id", last_mut_id);
        insert_kv("fiber-id", fiber_id);
        insert_kv("violation-flags", viol_flags);
        insert_kv("depth-at-clone", depth_at_clone);
        insert_kv("tracer-depth-max", tracer_depth);
        insert_kv("restamp-count", restamp);
        insert_kv("macro-dirty", static_cast<std::int64_t>(dirty));
        insert_kv("macro-expansion-dirty", (dirty & kExpansion) != 0 ? 1 : 0);
        insert_kv("gen-valid", ws->is_valid(nid) ? 1 : 0);
        insert_kv("flat-generation", static_cast<std::int64_t>(ws->generation()));
        insert_kv("process-violation-total", static_cast<std::int64_t>(proc_viol));
        insert_kv("process-provenance-errors", static_cast<std::int64_t>(prov_err));
        insert_kv("include-chain", include_chain ? 1 : 0);
        insert_kv("depth-budget", depth_budget);
        insert_kv("chain-length", static_cast<std::int64_t>(chain.size()));
        // First few chain hops as fixed keys (Agent scripts; no list alloc).
        if (!chain.empty())
            insert_kv("chain-0", chain[0]);
        if (chain.size() > 1)
            insert_kv("chain-1", chain[1]);
        if (chain.size() > 2)
            insert_kv("chain-2", chain[2]);
        if (chain.size() > 3)
            insert_kv("chain-3", chain[3]);
        insert_kv("schema", 2167);
        insert_kv("issue", 2167);
        insert_kv("schema-2167", 2167);
        insert_kv("active", 1);
        insert_kv("lazy", 1); // zero overhead when not queried

        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #2167: (query:macro-provenance-chain node-id [:depth n])
    // Lightweight side-table walk of provenance_ origins. Returns hash with
    // chain-length + chain-0..chain-N + terminal flags (schema 2167).
}

} // namespace aura::compiler::primitives_detail
