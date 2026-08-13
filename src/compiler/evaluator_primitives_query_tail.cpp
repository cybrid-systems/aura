// evaluator_primitives_query_tail.cpp — Issue #2914 peel (~L16421-L18041)
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

void register_query_tail_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                    std::pmr::vector<std::string>& string_heap,
                                    void*& type_registry, ModulePathResolver resolve_module_path,
                                    Evaluator& ev) {
    (void)pairs;
    (void)string_heap;
    (void)type_registry;
    (void)resolve_module_path;
    (void)ev;
    add("query:mutations-since", [](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_void();
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_void();
        auto* ws = ev->workspace_flat();
        if (!ws)
            return make_void();
        const std::uint64_t since_id = static_cast<std::uint64_t>(as_int(a[0]));
        const auto& log = ws->mutation_log_view();
        EvalValue list = make_void();
        // Walk most-recent first (the natural order
        // for the agent's "newest changes" view).
        for (std::size_t i = log.size(); i-- > 0;) {
            const auto& rec = log[i];
            if (rec.mutation_id <= since_id)
                break;
            // Issue #1419: include provenance in the string form
            // (same trailing fields as query:mutation-log).
            const std::string s = "id=" + std::to_string(rec.mutation_id) +
                                  " target=" + std::to_string(rec.target_node) +
                                  " op=" + rec.operator_name + " sum=" + rec.summary +
                                  " author=" + std::to_string(rec.author_fingerprint) +
                                  " parent=" + std::to_string(rec.parent_mutation_id) +
                                  " composite=" + std::to_string(rec.composite_transaction_id);
            const auto sidx = ev->push_string_heap(std::move(s));
            const auto p_idx = ev->push_pair(make_string(sidx), list);
            list = make_pair(p_idx);
        }
        return list;
    });

    // (query:last-mutation-blame) — Issue #349: returns
    // the blame info for the most recent mutation as
    // a 2-tuple (operator_name . summary). The blame
    // info is what post_mutation_invariant_check
    // (#260) stamps on each emitted note; exposing
    // it as an Aura primitive lets the AI agent
    // display "triggered by mutate:rebind" in the
    // diagnostic output. Returns the empty pair
    // (void) when no mutation has been logged.
    ObservabilityPrims::register_stats_impl(
        "query:last-mutation-blame", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_void();
            const auto view = ws->mutation_log_view();
            if (view.empty())
                return make_void();
            // Most-recent first.
            const auto& rec = view.back();
            // Build the 2-tuple (operator_name . summary).
            // The pair is (op_str . summary_str) — a flat
            // (a . b) pair where a is operator_name and
            // b is summary. The cdr is a string (not a
            // nested pair) because the test contracts
            // expect a 2-tuple of strings.
            const auto oidx = ev->push_string_heap(rec.operator_name);
            const auto sidx = ev->push_string_heap(rec.summary);
            // The push_pair helper copies the EvalValues
            // (so the cdr is a fresh make_string of sidx).
            return make_pair(ev->push_pair(make_string(oidx), make_string(sidx)));
        });

    // Issue #577: query:adt-exhaustiveness-stats. Returns the sum
    // of 4 Task2 ADT/match exhaustiveness + narrowing counters:
    //   - exhaustiveness_checks: adt_exhaust_rechecks_total
    //   - narrowing_hits_on_match: adt_occurrence_narrow_in_match_total
    //   - stale_exhaustiveness_prevented: adt_stale_exhaust_prevented_total
    //   - mutation_impact_on_adt: adt_variant_mutate_impacts_total
    ObservabilityPrims::register_stats_impl(
        "query:adt-exhaustiveness-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t checks =
                m->adt_exhaust_rechecks_total.load(std::memory_order_relaxed);
            const std::uint64_t narrow =
                m->adt_occurrence_narrow_in_match_total.load(std::memory_order_relaxed);
            const std::uint64_t stale =
                m->adt_stale_exhaust_prevented_total.load(std::memory_order_relaxed);
            const std::uint64_t impact =
                m->adt_variant_mutate_impacts_total.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(checks + narrow + stale + impact));
        });

    // Issue #612: query:adt-match-exhaust-stats. Returns the
    // sum of 4 ADT/match post-mutation reliability counters:
    //   - adt_exhaust_rechecks_total
    //   - adt_variant_mutate_impacts_total
    //   - adt_stale_exhaust_prevented_total
    //   - adt_occurrence_narrow_in_match_total
    ObservabilityPrims::register_stats_impl(
        "query:adt-match-exhaust-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t rechecks =
                m->adt_exhaust_rechecks_total.load(std::memory_order_relaxed);
            const std::uint64_t impacts =
                m->adt_variant_mutate_impacts_total.load(std::memory_order_relaxed);
            const std::uint64_t stale =
                m->adt_stale_exhaust_prevented_total.load(std::memory_order_relaxed);
            const std::uint64_t narrow =
                m->adt_occurrence_narrow_in_match_total.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(rechecks + impacts + stale + narrow));
        });

    // (query:match-exhaustiveness-notes) — Issue
    // #350: returns the most-recent match-
    // exhaustiveness notes (the kind = "Missing-
    // ConstructorInNestedMatch" notes emitted by
    // recheck_match_exhaustiveness_in_dirty_scope
    // after a mutation that touches an ADT
    // constructor). The function returns a
    // pair-list of node-ids (smallest first) that
    // are currently in the post-mutation
    // exhaustiveness notes.
    //
    // The C++ side (recheck_match_exhaustiveness_in_dirty_scope
    // in type_checker_impl.cpp #260) already
    // computes these notes; this primitive
    // surfaces the underlying match-info state to
    // Aura so the AI agent can ask "which matches
    // are currently flagged as non-exhaustive?".
    ObservabilityPrims::register_stats_impl(
        "query:match-exhaustiveness-notes", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_void();
            // Walk the flat; collect node-ids that
            // have a match_info entry with
            // exhaustiveness_checked = true + a
            // candidate_constructors / used_constructors
            // gap. We surface the NodeId; the agent can
            // use (query:node-type <id>) to inspect.
            EvalValue list = make_void();
            const auto n = ws->size();
            for (std::size_t id = n; id-- > 0;) {
                if (!ws->has_match_info(static_cast<aura::ast::NodeId>(id)))
                    continue;
                const auto* mi = ws->get_match_info(static_cast<aura::ast::NodeId>(id));
                if (!mi || !mi->exhaustiveness_checked)
                    continue;
                // We surface any checked match. A
                // future enhancement can filter to
                // "non-exhaustive" (used < candidates)
                // but the agent can derive that
                // locally.
                auto sidx = ev->push_string_heap(std::to_string(id));
                auto p_idx = ev->push_pair(make_string(sidx), list);
                list = make_pair(p_idx);
            }
            return list;
        });

    // Issue #555: query:typed-mutation-stats-task1. Returns
    // the sum of 4 Task1 typed self-mod observability
    // counters + the 4 existing #550 counters (so the AI
    // Agent can compute propagation_ratio +
    // selective_recheck_rate + conflict_rate in one read):
    //   - dirty_propagation_count_       (Evaluator, #555)
    //     # of mark_dirty_upward walks — dirty propagation
    //     throughput
    //   - selective_recheck_count_       (Evaluator, #555)
    //     # of selective OccurrenceInfoFlat re-narrows
    //     (vs full re-solve)
    //   - touched_roots_conflict_count_  (Evaluator, #555)
    //     # of CONFLICT detections between delta batches
    //   - guard_dirty_epoch_count_       (Evaluator, #555)
    //     # of Guard dtor success paths that propagated
    //     dirty to the type cache generation
    //   - narrowing_refresh_count_       (Evaluator, #550)
    //   - cross_delta_conflicts_caught_  (Evaluator, #550)
    //   - passes_skipped_type_dirty_     (Evaluator, #550)
    //   - touched_roots_size_            (Evaluator, #550)
    //
    // P0: returns an integer = sum of the 8 counters.
    // Follow-up: returns an 8-tuple so the AI Agent can
    // react to each category independently (e.g.,
    // touched_roots_conflict > 0 = hard alert;
    // propagation_ratio close to 1.0 = expected;
    // selective_recheck_rate high = win).
    //
    // Non-duplicative with #550 (query:typed-mutation-stats)
    // — the latter is Task 6 review; this primitive is
    // Task 1 EDSL mutate + Guard + dirty propagation focus.
    ObservabilityPrims::register_stats_impl(
        "query:typed-mutation-stats-task1", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t dirty_prop = ev->get_dirty_propagation_count();
            const std::uint64_t selective = ev->get_selective_recheck_count();
            const std::uint64_t conflicts = ev->get_touched_roots_conflict_count();
            const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
            const std::uint64_t narrowing = ev->get_narrowing_refresh_count();
            const std::uint64_t cross_delta = ev->get_cross_delta_conflicts_caught();
            const std::uint64_t passes_skipped = ev->get_passes_skipped_type_dirty();
            const std::uint64_t touched_size = ev->get_touched_roots_size();
            return make_int(static_cast<std::int64_t>(dirty_prop + selective + conflicts +
                                                      guard_epoch + narrowing + cross_delta +
                                                      passes_skipped + touched_size));
        });

    // Issue #556: query:edsl-concurrency-stats. Returns
    // the sum of 4 EDSL concurrency safety observability
    // counters from across the Evaluator:
    //   - mutation_steal_attempts_        (Evaluator, #438)
    //     # of steal attempts the scheduler logged
    //   - boundary_violation_count_       (Evaluator, #438)
    //     # of unsafe boundary attempts deferred/skipped
    //   - unsafe_boundary_attempts_       (Evaluator, #556)
    //     # of unsafe boundary attempts (a stricter
    //     subset of boundary_violation — cases where
    //     the boundary actually completed despite the
    //     violation)
    //   - lock_contention_us_              (Evaluator, #556)
    //     # lifetime microseconds spent waiting on
    //     workspace_mtx_ + Guard locks
    //
    // P0: returns an integer = sum of the 4 counters.
    // Follow-up: returns a 4-tuple
    // (steal-attempts boundary-violations unsafe-attempts
    // lock-contention-us) so the AI Agent can compute
    // contention_ratio = lock_contention_us / wall_time
    // and react to unsafe_boundary_attempts > 0 as a hard
    // alert (concurrency bug).
    //
    // Non-duplicative with #438 (query:fiber-migration-stats)
    // — the latter sums 2 counters (steal + violation);
    // this primitive adds the 2 Task1 EDSL concurrency
    // counters (#556) to the matrix.
    ObservabilityPrims::register_stats_impl(
        "query:edsl-concurrency-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t steals = ev->get_mutation_steal_attempts();
            const std::uint64_t violations = ev->get_boundary_violation_count();
            const std::uint64_t unsafe_attempts = ev->get_unsafe_boundary_attempts();
            const std::uint64_t contention_us = ev->get_lock_contention_us();
            return make_int(
                static_cast<std::int64_t>(steals + violations + unsafe_attempts + contention_us));
        });

    // Issue #505 / #531: query:closure-env-safety-stats. Hash view of
    // closure / EnvFrame / bridge_epoch / linear_ownership_state
    // post-invalidate safety counters for AI multi-round mutate loops:
    //   - stale-refresh: closure_stale_refresh_count_
    //   - bridge-hit: bridge_epoch_hit_count_
    //   - linear-pass: linear_check_pass_count_
    //   - gc-skipped: gc_envframe_stale_skipped_
    //   - env-stale-refresh: envframe_stale_refresh_count_
    //   - closure-env-safety-total: sum of the 5 counters
    //   - refresh-rate-pct: stale / (stale + bridge) * 100
    //   - closure-env-safety-recommendation: 0=ok, 1=review, 2=alert
    ObservabilityPrims::register_stats_impl(
        "query:closure-env-safety-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t stale_refresh =
                m ? m->closure_stale_refresh_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t bridge_hit =
                m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t linear_pass =
                m ? m->linear_check_pass_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t gc_skipped =
                m ? m->gc_envframe_stale_skipped_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t env_refresh = ev->get_envframe_stale_refresh_count();
            const std::uint64_t total =
                stale_refresh + bridge_hit + linear_pass + gc_skipped + env_refresh;
            const std::uint64_t epoch_checks = stale_refresh + bridge_hit;
            const std::int64_t refresh_pct =
                epoch_checks > 0 ? static_cast<std::int64_t>((stale_refresh * 100) / epoch_checks)
                                 : 0;
            std::int64_t recommendation = 0;
            if (gc_skipped > 0)
                recommendation = 2;
            else if (refresh_pct > 25)
                recommendation = 1;
            insert_kv("stale-refresh", static_cast<std::int64_t>(stale_refresh));
            insert_kv("bridge-hit", static_cast<std::int64_t>(bridge_hit));
            insert_kv("linear-pass", static_cast<std::int64_t>(linear_pass));
            insert_kv("gc-skipped", static_cast<std::int64_t>(gc_skipped));
            insert_kv("env-stale-refresh", static_cast<std::int64_t>(env_refresh));
            insert_kv("closure-env-safety-total", static_cast<std::int64_t>(total));
            insert_kv("refresh-rate-pct", refresh_pct);
            insert_kv("closure-env-safety-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #447: (query:tag-arity-count tag-int arity-int)
    // — count of nodes matching (tag, arity) using the
    // pre-built index. Bumps the hits or misses counter
    // accordingly. P0: 0 on miss (the follow-up falls
    // back to a linear scan on miss).
    ObservabilityPrims::register_stats_impl(
        "query:tag-arity-count", [](std::span<const EvalValue> a) -> EvalValue {
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1]))
                return make_int(0);
            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_int(0);
            // Issue #1371: ensure index is fresh (delta if append-only,
            // full rebuild if empty/dirty-with-mutate).
            ws->ensure_tag_arity_index();
            const auto tag = static_cast<std::uint32_t>(as_int(a[0]));
            const auto ar = static_cast<std::uint16_t>(as_int(a[1]));
            const auto nodes = ws->find_by_tag_arity(tag, ar, ar);
            return make_int(static_cast<std::int64_t>(nodes.size()));
        });

    // Issue #469: query:verification-loop-stats. Returns
    // observability counters for the closed-loop
    // verification-driven self-evolution pipeline:
    //   - verification_coverage_feedback_total_  (# of
    //     coverage-hole marks applied)
    //   - verification_assert_failure_total_  (# of
    //     assert-failure marks applied)
    //   - sv_mutate_attempts_total_  (total structured
    //     SV mutates called)
    //   - sv_mutate_success_total_  (successful SV
    //     mutates)
    //   - verify_loop_cycles_total_  (manual loop ticks
    //     from the AI Agent)
    //
    // P0: returns an integer = sum of all 5 counters.
    // Follow-up: returns a 5-tuple so the AI Agent can
    // compute mutate_success_rate + coverage_delta
    // independently.
    ObservabilityPrims::register_stats_impl(
        "query:verification-loop-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_int(0);
            const std::uint64_t cov = ws->verification_coverage_feedback_total();
            const std::uint64_t ass = ws->verification_assert_failure_total();
            const std::uint64_t att = ws->sv_mutate_attempts_total();
            const std::uint64_t suc = ws->sv_mutate_success_total();
            const std::uint64_t cyc = ws->verify_loop_cycles_total();
            return make_int(static_cast<std::int64_t>(cov + ass + att + suc + cyc));
        });

    // Issue #510: query:eda-verification-stats. Hash view of commercial
    // EDA verification interop + coverage/assert feedback closed-loop
    // counters (non-duplicative with #469 int-sum verification-loop-stats,
    // #695 eda-sv-closedloop-stress-stats stress harness, and #698
    // hardware-backend-commercial-stats emit/parse focus):
    //   - coverage-delta: verification_coverage_feedback_total
    //   - assert-fail-count: verification_assert_failure_total
    //   - auto-mutate-from-feedback: feedback_mutate_hits +
    //     verify_tool_feedback_mutate_success (eda_sv_feedback retired 4.4)
    //   - commercial-reemits: commercial_reemits_total
    //   - commercial-simulator-runs: commercial_simulator_runs_total
    //   - verification-loop-success: verification_loop_success_total
    //   - sv-mutate-success-rate-pct: 0..100 from attempts/success
    //   - eda-verification-total: sum of primary counters
    //   - eda-verification-recommendation: 0=ok, 1=assert-heavy, 2=low mutate rate
    ObservabilityPrims::register_stats_impl(
        "query:eda-verification-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
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
            const std::uint64_t coverage = ws ? ws->verification_coverage_feedback_total() : 0;
            const std::uint64_t assert_fail = ws ? ws->verification_assert_failure_total() : 0;
            const std::uint64_t feedback_hits =
                m ? m->feedback_mutate_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t verify_tool_feedback =
                ev->get_verify_tool_feedback_mutate_success_total();
            const std::uint64_t auto_mutate = feedback_hits + verify_tool_feedback;
            const std::uint64_t reemits =
                m ? m->commercial_reemits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t sim_runs =
                m ? m->commercial_simulator_runs_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t loop_success =
                m ? m->verification_loop_success_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t sv_attempts = ws ? ws->sv_mutate_attempts_total() : 0;
            const std::uint64_t sv_success = ws ? ws->sv_mutate_success_total() : 0;
            const std::uint64_t success_rate_pct =
                sv_attempts > 0 ? (100 * sv_success / sv_attempts) : (sv_success > 0 ? 100 : 0);
            const std::uint64_t total =
                coverage + assert_fail + auto_mutate + reemits + sim_runs + loop_success;
            std::int64_t recommendation = 0;
            if (assert_fail > coverage && auto_mutate == 0)
                recommendation = 1;
            else if (sv_attempts > 0 && success_rate_pct < 50)
                recommendation = 2;
            insert_kv("coverage-delta", static_cast<std::int64_t>(coverage));
            insert_kv("assert-fail-count", static_cast<std::int64_t>(assert_fail));
            insert_kv("auto-mutate-from-feedback", static_cast<std::int64_t>(auto_mutate));
            insert_kv("commercial-reemits", static_cast<std::int64_t>(reemits));
            insert_kv("commercial-simulator-runs", static_cast<std::int64_t>(sim_runs));
            insert_kv("verification-loop-success", static_cast<std::int64_t>(loop_success));
            insert_kv("sv-mutate-success-rate-pct", static_cast<std::int64_t>(success_rate_pct));
            insert_kv("eda-verification-total", static_cast<std::int64_t>(total));
            insert_kv("eda-verification-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #415: query:dirty-reason-propagation-stats. Returns
    // the sum of 9 dirty-reason + propagation observability
    // counters spanning the verification-category bitmask
    // infrastructure (#344/#437/#469) and mark_dirty_upward
    // propagation metrics (#256/#336):
    //   - mark_dirty_upward_call_count_   (FlatAST, #256)
    //   - mark_dirty_total_nodes_         (FlatAST, #256)
    //   - dirty_upward_fast_fixed_point_hits_ (FlatAST, #336)
    //   - verify_assertion_dirty_total_   (FlatAST, #437)
    //   - verify_coverage_dirty_total_    (FlatAST, #437)
    //   - verify_sva_dirty_total_         (FlatAST, #437)
    //   - verify_formal_cex_dirty_total_  (FlatAST, #437)
    //   - verification_coverage_feedback_total_ (FlatAST, #469)
    //   - verification_assert_failure_total_  (FlatAST, #469)
    //
    // P0: returns an integer = sum of all 9 counters.
    // Follow-up: returns a 9-tuple so the AI Agent can
    // compute propagation_depth = total_nodes / call_count
    // and verify_feedback_rate independently.
    //
    // Non-duplicative with #344 (compile:dirty-reason-counts
    // per-node tallies + query:dirty-nodes subtree query),
    // #437 (query:verify-dirty-stats 4-tuple only), and
    // #469 (query:verification-loop-stats includes SV
    // mutate + loop-cycle counters).
    ObservabilityPrims::register_stats_impl(
        "query:dirty-reason-propagation-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_int(0);
            const std::uint64_t upward_calls = ws->mark_dirty_upward_call_count();
            const std::uint64_t upward_nodes = ws->mark_dirty_total_nodes();
            const std::uint64_t fast_hits = ws->dirty_upward_fast_fixed_point_count();
            const std::uint64_t verify =
                ws->verify_assertion_dirty_total() + ws->verify_coverage_dirty_total() +
                ws->verify_sva_dirty_total() + ws->verify_formal_cex_dirty_total();
            const std::uint64_t feedback = ws->verification_coverage_feedback_total() +
                                           ws->verification_assert_failure_total();
            return make_int(static_cast<std::int64_t>(upward_calls + upward_nodes + fast_hits +
                                                      verify + feedback));
        });

    // Issue #448: query:mutation-coordination-stats.
    // Returns observability counters for the fiber /
    // scheduler / GC coordination layer:
    //   - mutation_steal_violation_count_  (work-steal
    //     attempts deferred because the victim fiber
    //     is in an unsafe MutationBoundary state)
    //   - gc_blocked_by_mutation_boundary_  (GC safepoint
    //     requests deferred because an outermost guard
    //     is held)
    //   - safepoint_mutation_wait_total_ns_  (total ns
    //     spent waiting for fibers to reach a safe
    //     mutation boundary during a safepoint)
    //
    // P0: returns an integer = sum of all 3 counters.
    // Follow-up: returns a 3-tuple
    // (steal-violations gc-blocks wait-ns) so the AI
    // Agent can react to each category independently.
    ObservabilityPrims::register_stats_impl(
        "query:mutation-coordination-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t steals = ev->get_mutation_steal_violation_count();
            const std::uint64_t gc_blocks = ev->get_gc_blocked_by_mutation_boundary();
            const std::uint64_t wait_ns = ev->get_safepoint_mutation_wait_total_ns();
            return make_int(static_cast<std::int64_t>(steals + gc_blocks + wait_ns));
        });

    // Issue #543: query:envframe-dualpath-stats.
    // Returns observability counters for the SoA
    // EnvFrame/EnvId dual-path (bindings_ vs
    // bindings_symid_) + version stamping + stale
    // detection + GCEnvWalkFn integration layer:
    //   - envframe_desync_detected_  (# of frames
    //     where the dual-path length/order check found
    //     a mismatch — should be 0 in production)
    //   - envframe_stale_refresh_count_  (# of frames
    //     whose version_ was bumped by
    //     materialize_call_env because it was older
    //     than the current defuse_version_)
    //   - envframe_version_mismatch_in_walk_  (# of
    //     frames skipped during walk_env_frames /
    //     lookup_by_symid_chain because their version_
    //     was older than the snapshot)
    //   - envframe_gc_walk_safe_skips_  (# of frames
    //     skipped during walk_env_frame_roots for the
    //     same reason — important for tuning the GC's
    //     epoch snapshot strategy)
    //
    //   - bindings_dual_sync_count_  (# of frames where
    //     the dual-path length/order check succeeded —
    //     expected to grow under normal mutation)
    //
    // P0: returns an integer = sum of all 5 counters.
    // Follow-up: returns a 5-tuple
    // (desync dual-sync stale-refresh version-mismatch gc-skips)
    // so the AI Agent can react to each category
    // independently (a desync > 0 should be a hard
    // alert; a version-mismatch > 0 is expected under
    // concurrent mutation).
    ObservabilityPrims::register_stats_impl(
        "query:envframe-dualpath-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t desync = ev->get_envframe_desync_detected();
            const std::uint64_t dual_sync = ev->get_bindings_dual_sync_count();
            const std::uint64_t stale = ev->get_envframe_stale_refresh_count();
            const std::uint64_t mismatch = ev->get_envframe_version_mismatch_in_walk();
            const std::uint64_t gc_skips = ev->get_envframe_gc_walk_safe_skips();
            return make_int(
                static_cast<std::int64_t>(desync + dual_sync + stale + mismatch + gc_skips));
        });

    // Issue #1903: query:envframe-dual-consistency-stats.
    // Returns the #1903 dual-path consistency enforcement counters:
    //   - envframe_dual_consistency_asserted_: every bind/bind_symid +
    //     post-steal + post-materialize ensure_dual_path_consistent() call.
    //     Expected to grow linearly with mutation + steal + materialize
    //     throughput (the "did we run the check?" signal).
    //   - envframe_post_steal_dual_synced_: # of frames where
    //     refresh_stale_frames_after_steal ran ensure_dual_path_consistent
    //     on a refreshed frame. Should roughly track fiber resume count.
    //   - envframe_materialize_consistency_checks_: # of materialize_call_env
    //     invocations that explicitly asserted consistency post-copy. Should
    //     roughly track apply_closure + TCO call count.
    //   - envframe_gc_walk_legacy_fallback_uses_: # of GC walk frames where
    //     bindings_symid_ was empty and walk fell back to bindings_. Should
    //     trend toward 0 as the SymId-keyed primary path saturates.
    //
    // Like the dualpath-stats cousin, P0 ships a single integer sum;
    // a follow-up returns a 4-tuple so the Agent can react to each
    // counter independently (a spike in legacy-fallback is the early
    // signal that pool intern coverage is regressing).
    ObservabilityPrims::register_stats_impl(
        "query:envframe-dual-consistency-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t asserted = ev->get_envframe_dual_consistency_asserted();
            const std::uint64_t post_steal = ev->get_envframe_post_steal_dual_synced();
            const std::uint64_t materialize = ev->get_envframe_materialize_consistency_checks();
            const std::uint64_t legacy_fallback = ev->get_envframe_gc_walk_legacy_fallback_uses();
            return make_int(
                static_cast<std::int64_t>(asserted + post_steal + materialize + legacy_fallback));
        });

    // Issue #1904: query:mutation-guard-coverage.
    // Returns MutationBoundaryGuard coverage as integer basis points
    // (0..10000) - 10000 means 100% coverage (every observed mutate:*
    // call site uses MutationBoundaryGuard RAII, zero manual lock+bump
    // sites remain). The ratio is
    //   wrapped / (wrapped + legacy) * 10000
    // so agents can alert on < 10000 (any drop = a regression).
    //
    // Returns -1 sentinel when coverage < 100% (legacy > 0) to make
    // regressions grep-friendly from --metrics output.
    // Returns 10000 (vacuously full coverage) when no mutate activity
    // has been observed yet - a fresh evaluator with no Guard wraps
    // and no legacy sites is not in violation of the contract.
    ObservabilityPrims::register_stats_impl(
        "query:mutation-guard-coverage", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(10000);
            const std::uint64_t wrapped = ev->get_mutation_boundary_primitives_wrapped();
            const std::uint64_t legacy = ev->get_mutation_legacy_manual_lock_total();
            const std::uint64_t total = wrapped + legacy;
            if (total == 0)
                return make_int(10000); // no activity yet - vacuously covered
            if (legacy > 0)
                return make_int(-1); // regression sentinel
            return make_int(static_cast<std::int64_t>((wrapped * 10000) / total));
        });

    // Issue #1905: query:aot-hot-update-stats.
    // Returns observability for the AOT incremental hot-update /
    // invalidation loop (build on #1046). 6 counters surfaced:
    //   - aot_live_closure_refresh_on_mutation_total: every
    //     aura_refresh_live_closures_for_mutated_define call from
    //     flush_mutation_boundary outermost exit (Step 2 of #1905).
    //   - aot_live_closure_refresh_on_steal_total: every refresh
    //     call from complete_post_resume_steal_refresh (Step 3).
    //   - aot_bridge_epoch_bump_on_mutation_total: bridge_epoch bump
    //     driven by outermost MutationBoundaryGuard exit.
    //   - aot_bridge_epoch_bump_on_steal_total: bridge_epoch bump
    //     driven by fiber resume / steal.
    //   - aot_region_mismatch_on_resume_total: per-eval AotState
    //     region_mask drift on resume (deopt path).
    //   - aot_stale_deopt_on_steal_total: stale AOT closure dispatch
    //     on stolen fiber resume (vs the regular jit_closure_stale_deopt_total
    //     which is the on-AOT path).
    //
    // Default (no args or args[0]==0): Returns -1 sentinel when
    // aot_stale_deopt_on_steal_total > 0 (grep-friendly regression
    // marker). Otherwise returns the sum of all 6 counters. Returns
    // 0 when no AOT hot-update activity observed yet (a fresh
    // evaluator with no Guard exits + no steals is not in violation
    // of the contract — vacuously covered).
    //
    // Issue #2240: hash mode (args[0]!=0) returns a hash of all
    // counters + cross-workspace reject metadata (refine #2178).
    // Additive — default arg=0 path is unchanged so existing
    // dashboards keep working.
    ObservabilityPrims::register_stats_impl(
        "query:aot-hot-update-stats", [&](std::span<const EvalValue> a) -> EvalValue {
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t refresh_mut = ev->get_aot_live_closure_refresh_on_mutation_total();
            const std::uint64_t refresh_steal = ev->get_aot_live_closure_refresh_on_steal_total();
            const std::uint64_t bridge_mut = ev->get_aot_bridge_epoch_bump_on_mutation_total();
            const std::uint64_t bridge_steal = ev->get_aot_bridge_epoch_bump_on_steal_total();
            const std::uint64_t region_mismatch = ev->get_aot_region_mismatch_on_resume_total();
            const std::uint64_t stale_deopt = ev->get_aot_stale_deopt_on_steal_total();

            // Issue #2240: optional hash mode via args[0]. Default
            // (no args / args[0]==0) keeps existing sum sentinel
            // behavior for backwards compat — no schema break.
            const bool hash_mode = (!a.empty() && is_int(a[0]) && as_int(a[0]) != 0);
            if (!hash_mode) {
                if (stale_deopt > 0)
                    return make_int(-1); // regression sentinel
                return make_int(static_cast<std::int64_t>(refresh_mut + refresh_steal + bridge_mut +
                                                          bridge_steal + region_mismatch +
                                                          stale_deopt));
            }

            // Hash mode (#2240 refine #2178): file-scope C readers.
            // Uses register_query_primitives `string_heap` ref (not private
            // Evaluator::string_heap_).
            const std::uint64_t cw_reject_total =
                aura_cross_workspace_hot_update_rejected_total_v_read();
            const std::uint8_t cw_last_reason_u8 = aura_last_cross_workspace_reject_reason_v_read();
            const char* cw_last_reason_symbol =
                aura_cross_workspace_reject_reason_string(cw_last_reason_u8);
            const std::string reason_str =
                cw_last_reason_symbol ? std::string(cw_last_reason_symbol) : std::string("unknown");
            const auto reason_kidx = string_heap.size();
            string_heap.push_back(reason_str);

            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, EvalValue v) {
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
                        vals[idx] = v.val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("aot-live-closure-refresh-on-mutation-total",
                      make_int(static_cast<std::int64_t>(refresh_mut)));
            insert_kv("aot-live-closure-refresh-on-steal-total",
                      make_int(static_cast<std::int64_t>(refresh_steal)));
            insert_kv("aot-bridge-epoch-bump-on-mutation-total",
                      make_int(static_cast<std::int64_t>(bridge_mut)));
            insert_kv("aot-bridge-epoch-bump-on-steal-total",
                      make_int(static_cast<std::int64_t>(bridge_steal)));
            insert_kv("aot-region-mismatch-on-resume-total",
                      make_int(static_cast<std::int64_t>(region_mismatch)));
            insert_kv("aot-stale-deopt-on-steal-total",
                      make_int(static_cast<std::int64_t>(stale_deopt)));
            insert_kv("cross-workspace-hot-update-rejected-total",
                      make_int(static_cast<std::int64_t>(cw_reject_total)));
            insert_kv("cross-workspace-last-reject-reason",
                      make_int(static_cast<std::int64_t>(cw_last_reason_u8)));
            insert_kv("cross-workspace-last-reject-reason-symbol",
                      make_string(static_cast<std::uint64_t>(reason_kidx)));
            insert_kv("cross-workspace-reject-wired", make_int(1));
            insert_kv("schema-2178", make_int(2178));
            insert_kv("issue-2178", make_int(2178));
            insert_kv("schema-2240", make_int(2240));
            insert_kv("issue-2240", make_int(2240));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1952 / #1930: query:aot-incremental-reemit-stats.
    // Hash surface (schema-1930) for the aura_reemit_aot_for_dirty pipeline:
    //   - aot_incremental_reemit_count: region-filtered "would re-emit"
    //   - aot_incremental_reemit_success_total: host emit callback true
    //   - stable_func_id_preserved_total / assigned_total: #1930 map
    //   - aot_closure_dependency_reemit_total: closure-capture cascade
    //   - wire flags: emit-callback / stable-map / return-success-when-emit
    // Optional arg 0 = "sum": returns compact sum (legacy path) without
    // the old stable==success -1 sentinel (#1930 real map breaks 1:1).
    ObservabilityPrims::register_stats_impl(
        "query:aot-incremental-reemit-stats",
        [&ev, &string_heap](std::span<const EvalValue> a) -> EvalValue {
            auto* qev = Evaluator::get_query_evaluator();
            if (!qev)
                qev = &ev;
            const std::uint64_t total = qev->get_aot_incremental_reemit_count();
            const std::uint64_t success = qev->get_aot_incremental_reemit_success_total();
            const std::uint64_t preserved = qev->get_stable_func_id_preserved_total();
            const std::uint64_t assigned = qev->get_stable_func_id_assigned_total();
            const std::uint64_t closure_dep = qev->get_aot_closure_dependency_reemit_total();
            const std::uint64_t live_remap = qev->get_live_closure_remap_total();
            // Issue #2175: legacy sid=0 backfill (independent of name fallback).
            const std::uint64_t live_backfill = qev->get_live_closure_stable_id_backfill_total();
            // Issue #2233: post-reemit live-closure stamp metrics
            // (hit / miss split). The hit counter is the per-closure
            // bump in aura_remap_live_closures_after_reemit when the
            // restamp + clear-MustDeopt path runs; the miss counter
            // is the per-closure bump in the name-candidate-no-remap
            // path (when name_fallback is off but the name resolves in
            // the reemit set).
            const std::uint64_t epoch_restamp = qev->get_live_closure_epoch_restamp_total();
            const std::uint64_t must_deopt_kept = qev->get_live_closure_must_deopt_kept_total();
            // Issue #2234: capture remount ok/fail totals.
            const std::uint64_t capture_remount_ok = qev->get_closure_capture_remount_ok_total();
            const std::uint64_t capture_remount_fail =
                qev->get_closure_capture_remount_fail_total();
            // Legacy compact sum path: (engine:metrics "query:..." "sum")
            if (!a.empty() && is_string(a[0])) {
                auto sidx = as_string_idx(a[0]);
                if (sidx < string_heap.size() && string_heap[sidx] == "sum") {
                    return make_int(static_cast<std::int64_t>(
                        total + success + preserved + assigned + closure_dep + live_remap +
                        live_backfill + epoch_restamp + must_deopt_kept + capture_remount_ok +
                        capture_remount_fail));
                }
            }
            // Capacity must be power-of-two (open-address mask).
            // ~70 keys with #2297 + #2369; use 256 for headroom.
            auto* ht = FlatHashTable::create(256);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const std::string& k_str, std::int64_t v) {
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
                        vals[slot] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("schema", 1930);
            insert_kv("issue", 1930);
            insert_kv("schema-1930", 1930);
            insert_kv("issue-1930", 1930);
            insert_kv("schema-1952", 1952);
            // Issue #2233: post-reemit live-closure stamp metrics
            // (hit / miss split). Schema bump for the 2233 lineage.
            insert_kv("live-closure-epoch-restamp-total", static_cast<std::int64_t>(epoch_restamp));
            insert_kv("live-closure-must-deopt-kept-total",
                      static_cast<std::int64_t>(must_deopt_kept));
            insert_kv("schema-2233", 2233);
            insert_kv("issue-2233", 2233);
            // insert_kv takes int64_t (not EvalValue) — bare 1, not make_int(1).
            insert_kv("post-reemit-stamp-wired", 1);
            // Issue #2234: post-reemit / post-compact env_frame + linear
            // capture remount metrics. Bumped from
            // aura_remount_closure_captures (aura_jit_runtime.cpp)
            // when a closure's captured env_frame version + linear state
            // are rebound to the live generation (ok) or fail and force
            // the caller to set MustDeopt (fail). Schema bump for the
            // 2234 lineage.
            insert_kv("closure-capture-remount-ok-total",
                      static_cast<std::int64_t>(capture_remount_ok));
            insert_kv("closure-capture-remount-fail-total",
                      static_cast<std::int64_t>(capture_remount_fail));
            // Issue #2272: env_generation PRIMARY axis counter
            // (distinct from remount-fail-total). Pairs with
            // remount-fail-total on dashboards to distinguish
            // "env_gen drift" from "defuse drift".
            insert_kv(
                "closure-capture-env-gen-mismatch-total",
                qev ? static_cast<std::int64_t>(qev->get_closure_capture_env_gen_mismatch_total())
                    : 0);
            insert_kv("closure-capture-env-gen-wired", 1);
            insert_kv("schema-2272", 2272);
            insert_kv("issue-2272", 2272);
            // Issue #2297: structural capture-cell remap (densify object_remap).
            // Read metrics via compiler_metrics_ (not qev getters) to avoid
            // partition BMI coupling on new accessors.
            {
                auto* m = static_cast<const CompilerMetrics*>(qev->compiler_metrics());
                const auto cell_ok =
                    m ? m->closure_capture_cell_remap_ok_total.load(std::memory_order_relaxed) : 0;
                const auto cell_fail =
                    m ? m->closure_capture_cell_remap_fail_total.load(std::memory_order_relaxed)
                      : 0;
                insert_kv("closure-capture-cell-remap-ok-total",
                          static_cast<std::int64_t>(cell_ok));
                insert_kv("closure-capture-cell-remap-fail-total",
                          static_cast<std::int64_t>(cell_fail));
            }
            insert_kv("capture-cell-remap-wired", 1);
            insert_kv("schema-2297", 2297);
            insert_kv("issue-2297", 2297);
            // Issue #2503: remount fail → MustDeopt + batch_deopt shared path.
            insert_kv("remount-or-force-deopt-wired", 1);
            insert_kv("schema-2503", 2503);
            insert_kv("issue-2503", 2503);
            // Issue #2894: last remount fail reason + mapped axis totals.
            // Additive only; #2503/#2234/#2272/#2297 surfaces preserved.
            {
                const std::uint8_t last_rr = aura_last_remount_fail_reason();
                insert_kv("last-remount-fail-reason", static_cast<std::int64_t>(last_rr));
                insert_kv("last_remount_fail_reason", static_cast<std::int64_t>(last_rr));
                // Map existing counters so Agents get named totals without
                // correlating env_gen_mismatch / cell_remap_fail themselves.
                insert_kv("remount-fail-env-gen-total",
                          qev ? static_cast<std::int64_t>(
                                    qev->get_closure_capture_env_gen_mismatch_total())
                              : 0);
                {
                    auto* m = static_cast<const CompilerMetrics*>(qev->compiler_metrics());
                    const auto cell_fail =
                        m ? m->closure_capture_cell_remap_fail_total.load(std::memory_order_relaxed)
                          : 0;
                    insert_kv("remount-fail-densify-cell-total",
                              static_cast<std::int64_t>(cell_fail));
                }
                insert_kv("remount-fail-total", static_cast<std::int64_t>(capture_remount_fail));
                insert_kv("remount-fail-reason-wired", 1);
                insert_kv("schema-2894", 2894);
                insert_kv("issue-2894", 2894);
            }
            insert_kv("schema-2234", 2234);
            insert_kv("issue-2234", 2234);
            insert_kv("capture-remount-wired", 1);
            insert_kv("schema-2013", 2013);
            insert_kv("active", 1);
            insert_kv("aot_incremental_reemit_count", static_cast<std::int64_t>(total));
            insert_kv("aot_incremental_reemit_success_total", static_cast<std::int64_t>(success));
            insert_kv("stable_func_id_preserved_total", static_cast<std::int64_t>(preserved));
            insert_kv("stable_func_id_assigned_total", static_cast<std::int64_t>(assigned));
            insert_kv("aot_closure_dependency_reemit_total",
                      static_cast<std::int64_t>(closure_dep));
            // Issue #2013: live closures retargeted after reemit.
            insert_kv("live_closure_remap_total", static_cast<std::int64_t>(live_remap));
            // Issue #2175: legacy sid=0 backfill counter (one-shot lookup
            // per successful backfill during remap walk — fires when
            // stored_sid == 0 but the name resolves in the live stable map).
            insert_kv("live_closure_stable_id_backfill_total",
                      static_cast<std::int64_t>(live_backfill));
            // Issue #2605: residual_backfill aliases (#2175 counter) +
            // assign / preserve axes for Agent dashboards.
            insert_kv("stable-id-residual-backfill-total",
                      static_cast<std::int64_t>(live_backfill));
            insert_kv("stable_id_residual_backfill_total",
                      static_cast<std::int64_t>(live_backfill));
            insert_kv("stable-id-assign-total", static_cast<std::int64_t>(assigned));
            insert_kv("stable-id-preserve-total", static_cast<std::int64_t>(preserved));
            {
                auto* m = static_cast<const CompilerMetrics*>(qev->compiler_metrics());
                const std::uint64_t named_reject =
                    m ? m->live_closure_named_name_fallback_reject_total.load(
                            std::memory_order_relaxed)
                      : 0;
                insert_kv("named-name-fallback-reject-total",
                          static_cast<std::int64_t>(named_reject));
                insert_kv("named_name_fallback_reject_total",
                          static_cast<std::int64_t>(named_reject));
            }
            insert_kv("anonymous-must-deopt-policy-wired", 1);
            insert_kv("residual-sid0-policy-wired", 1);
            insert_kv("schema-2605", 2605);
            insert_kv("issue-2605", 2605);
            // Issue #2606: multi-AotState reemit ownership filter —
            // candidates dropped when stable_func_id maps to a slot
            // owned by a foreign eval. Soft single-eval keeps counter 0.
            {
                auto* m = static_cast<const CompilerMetrics*>(qev->compiler_metrics());
                const std::uint64_t cross =
                    m ? m->reemit_cross_eval_candidate_skipped_total.load(std::memory_order_relaxed)
                      : 0;
                insert_kv("reemit_cross_eval_candidate_skipped_total",
                          static_cast<std::int64_t>(cross));
                insert_kv("reemit-cross-eval-candidate-skipped-total",
                          static_cast<std::int64_t>(cross));
            }
            insert_kv("reemit-cross-eval-filter-wired", 1);
            insert_kv("schema-2606", 2606);
            insert_kv("issue-2606", 2606);
            insert_kv("schema-2175", 2175);
            insert_kv("issue-2175", 2175);
            // Issue #2016 metrics (may be 0 if CompilerMetrics not wired).
            {
                auto* m = static_cast<const CompilerMetrics*>(qev->compiler_metrics());
                const std::uint64_t llvm_n =
                    m ? m->aot_incremental_llvm_emit_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t evo =
                    m ? m->aot_evolution_region_skips_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t clr =
                    m ? m->aot_region_mask_adapt_clears_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t rst =
                    m ? m->aot_region_mask_adapt_restores_total.load(std::memory_order_relaxed) : 0;
                insert_kv("aot_incremental_llvm_emit_total", static_cast<std::int64_t>(llvm_n));
                insert_kv("aot_evolution_region_skips_total", static_cast<std::int64_t>(evo));
                insert_kv("aot_region_mask_adapt_clears_total", static_cast<std::int64_t>(clr));
                insert_kv("aot_region_mask_adapt_restores_total", static_cast<std::int64_t>(rst));
                // Issue #2128: MustDeoptBeforeNextCall after remap miss.
                const std::uint64_t must_n =
                    m ? m->must_deopt_before_next_call_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t force_ok =
                    m ? m->must_deopt_force_deopt_success_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t force_fail =
                    m ? m->must_deopt_force_deopt_fail_total.load(std::memory_order_relaxed) : 0;
                insert_kv("must_deopt_before_next_call_total", static_cast<std::int64_t>(must_n));
                insert_kv("must-deopt-before-next-call-total", static_cast<std::int64_t>(must_n));
                insert_kv("must_deopt_force_deopt_success_total",
                          static_cast<std::int64_t>(force_ok));
                insert_kv("must_deopt_force_deopt_fail_total",
                          static_cast<std::int64_t>(force_fail));
                insert_kv("schema-2128", 2128);
                insert_kv("issue-2128", 2128);
                insert_kv("must-deopt-before-next-call-wired", 1);
            }
            insert_kv("stable-func-id-map-wired", 1);
            insert_kv("emit-callback-path-wired", 1);
            insert_kv("return-success-when-emit-wired", 1);
            insert_kv("live-closure-remap-wired", 1);
            insert_kv("adaptive-region-mask-wired", 1);
            insert_kv("pipeline-phase", 6); // + must-deopt-before-next-call (#2128)
            // Issue #2369: stable_func_id sole primary for live-closure remap;
            // name-fallback rewrite is legacy opt-in only (default off).
            {
                auto* m = static_cast<const CompilerMetrics*>(qev->compiler_metrics());
                const std::uint64_t name_fb =
                    m ? m->live_closure_remap_name_fallback_total.load(std::memory_order_relaxed)
                      : 0;
                insert_kv("live-closure-remap-name-fallback-total",
                          static_cast<std::int64_t>(name_fb));
                insert_kv("live_closure_remap_name_fallback_total",
                          static_cast<std::int64_t>(name_fb));
                insert_kv("remap-name-fallback-enabled",
                          static_cast<std::int64_t>(aura_get_remap_name_fallback_enabled()));
                insert_kv("remap-name-fallback-default-off", 1);
                insert_kv("stable-func-id-sole-primary-wired", 1);
                insert_kv("schema-2369", 2369);
                insert_kv("issue-2369", 2369);
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1907: query:reflect-schema.
    // Returns observability for the reflect/EDSL bridge hook. Counters
    // surfaced:
    //   - reflect_schema_query_total: every (query:reflect-schema) call
    //     (Step 2 of #1907). Backed by the bridge hook
    //     aura_validate_reflected_post_mutation from flush_mutation_boundary
    //     outermost exit.
    //   - reflect_post_mutation_validate_total: every bridge-hook call
    //     from flush_mutation_boundary outermost exit (Step 1 of #1907).
    //   - reflect_post_mutation_validate_fail_total: subset where the
    //     auto_validate pass returns false (validation failure).
    //   - reflect_hygiene_macro_reject_total: subset where the
    //     SyntaxMarker::MacroIntroduced gate rejects (no explicit
    //     allow_macro_evolution + dirty_macro_nodes > 0).
    //   - reflect_validate_reflected_query_total: every
    //     (mutate:validate-reflected) call (Step 2 of #1907).
    //   - reflect_dirty_macro_nodes_total: cumulative sum of
    //     dirty_macro_nodes reported by the bridge hook (trending
    //     metric for self-evolution hygiene regression detection).
    //
    // Returns -1 sentinel when reflect_post_mutation_validate_fail_total > 0
    // (grep-friendly regression marker). Otherwise returns the sum of all 6
    // counters (sum-path, like #1903/#1904/#1905/#1906 P0/P1 shape).
    ObservabilityPrims::register_stats_impl(
        "query:reflect-schema", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t schema = ev->get_reflect_schema_query_total();
            const std::uint64_t post_validate = ev->get_reflect_post_mutation_validate_total();
            const std::uint64_t post_fail = ev->get_reflect_post_mutation_validate_fail_total();
            const std::uint64_t macro_reject = ev->get_reflect_hygiene_macro_reject_total();
            const std::uint64_t validate_reflected =
                ev->get_reflect_validate_reflected_query_total();
            const std::uint64_t dirty_macro = ev->get_reflect_dirty_macro_nodes_total();
            if (post_fail > 0)
                return make_int(-1); // regression sentinel
            return make_int(static_cast<std::int64_t>(schema + post_validate + post_fail +
                                                      macro_reject + validate_reflected +
                                                      dirty_macro));
        });

    // Issue #1907: mutate:validate-reflected.
    // Calls aura_validate_reflected_post_mutation bridge hook from the
    // C bridge layer (aura_jit_bridge.cpp). The hook combines the
    // aura::reflect::auto_validate pass with the
    // aura::reflect::hygiene_allows_evolution macro guard. Returns
    // sum of post-mutation validate counters + bumps the
    // validate_reflected_query_total counter (Step 2 of #1907).
    //
    // Args:
    //   mutate:validate-reflected (no args) -- always succeeds on a
    //   fresh evaluator (the bridge hook counter path is the real
    //   validation). The primitive is the EDSL entry point for
    //   self-evolution audits that want to confirm the bridge hook is
    //   wired and counters are incrementing.
    //
    // Returns the sum of reflect_post_mutation_validate_total +
    // reflect_hygiene_macro_reject_total + reflect_dirty_macro_nodes_total
    // after bumping validate_reflected_query_total by 1.
    // SECURITY_EXEMPT: diagnostic counters only — no AST write (#2057/#2152).
    // GUARD_EXEMPT: diagnostic counters only — no AST write (#2986). PrimMeta.guard_exempt.
    add("mutate:validate-reflected", [&ev](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        // Public accessor: register_query_primitives is not a friend of
        // Evaluator (unlike ObservabilityPrims / register_mutate_*).
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->reflect_validate_reflected_query_total.fetch_add(1, std::memory_order_relaxed);
        }
        const std::uint64_t post_validate = ev.get_reflect_post_mutation_validate_total();
        const std::uint64_t macro_reject = ev.get_reflect_hygiene_macro_reject_total();
        const std::uint64_t dirty_macro = ev.get_reflect_dirty_macro_nodes_total();
        return make_int(static_cast<std::int64_t>(post_validate + macro_reject + dirty_macro));
    });
    {
        ::aura::compiler::PrimMeta ex{};
        ex.security_exempt = true;
        ex.guard_exempt = true;
        ex.pure = true;
        ex.doc = "SECURITY_EXEMPT / GUARD_EXEMPT: diagnostic reflect counters only (#2152/#2986)";
        ev.primitives().set_meta_for_name("mutate:validate-reflected", std::move(ex));
    }

    add("query:schema", [&string_heap, &type_registry](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        // Issue #1908: query:macro-provenance-stats.
        // Returns observability for the MutationBoundaryGuard + macro clone
        // provenance hardening surface (refine #1014 / #1047). Two counters
        // surfaced (per-eval view via Evaluator getters):
        //   - macro_provenance_repin_on_steal_total: every forced repin of
        //     MacroIntroduced marker + provenance that fires on fiber steal /
        //     resume / outermost Guard exit / PanicCheckpoint transfer. Bumped
        //     from clone_macro_body (MacroIntroduced branch) +
        //     complete_post_resume_steal_refresh (after probe_and_repin_macro_provenance)
        //     + transfer_and_revalidate_panic_checkpoint (post panic restamp).
        //   - hygiene_violation_prevented_on_boundary_total: every time the
        //     boundary interaction (outermost flush dirty/epoch bump + post-steal
        //     probe + PanicCheckpoint transfer coupling) prevented a hygiene
        //     violation from manifesting. Bumped from flush_mutation_boundary
        //     outermost exit + complete_post_resume_steal_refresh (post probe) +
        //     transfer_and_revalidate_panic_checkpoint (post panic restamp).
        //
        // Returns -1 sentinel when macro_provenance_repin_on_steal_total > 0
        // AND hygiene_violation_prevented_on_boundary_total == 0 (regression
        // marker: boundary did NOT prevent violation despite MacroIntroduced
        // repin firing — grep-friendly). Otherwise returns the sum of the
        // 2 counters (sum-path, like #1903/#1904/#1905/#1906 P0/P1 shape).
        ObservabilityPrims::register_stats_impl(
            "query:macro-provenance-stats", [](std::span<const EvalValue> a) -> EvalValue {
                (void)a;
                auto* ev = Evaluator::get_query_evaluator();
                if (!ev)
                    return make_int(0);
                const std::uint64_t repin = ev->get_macro_provenance_repin_on_steal_total();
                const std::uint64_t prevented =
                    ev->get_hygiene_violation_prevented_on_boundary_total();
                // Regression sentinel: repin fired but boundary did NOT prevent
                // violation (inconsistent boundary interaction).
                if (repin > 0 && prevented == 0)
                    return make_int(-1);
                return make_int(static_cast<std::int64_t>(repin + prevented));
            });
        if (idx >= string_heap.size())
            return make_bool(false);
        std::string name = string_heap[idx];
        if (!type_registry) {
            type_registry = new aura::core::TypeRegistry();
        }
        auto* treg = static_cast<aura::core::TypeRegistry*>(type_registry);
        if (!treg)
            return make_bool(false);
        auto ty = treg->lookup_type(name);
        if (!ty.valid())
            return make_bool(false);
        std::string schema = "{\"title\": \"" + name + "\"";
        schema += ", \"type\": \"" +
                  std::string(treg->tag_of(ty) == aura::core::TypeTag::MODULE ? "object" : "any") +
                  "\"}";
        auto sidx = string_heap.size();
        string_heap.push_back(schema);
        return make_string(sidx);
    });

    // ── Issue #288: mutate:validate-against-schema ──────────────────
    //
    // Standalone pre-mutation validation. Callers (mutate:rebind,
    // mutate:query-and-replace, or user code) can use this to
    // check a new value against a registered type's schema before
    // committing the change. Returns one of:
    //   - #t                 (valid)
    //   - #f                 (no schema registered for the type)
    //   - (list "schema-violation" <reason> <field>) on failure
    //
    // Usage:
    //   (mutate :validate <new-value> <type-name>)
    //     → bool or tagged-violation pair
    //
    // The new-value form is intentionally loose: an int, a string
    // (parsed as code), or a quoted s-expression all flow through
    // `validate_value_against_schema` which dispatches by type.
    // For the initial P0 ship we validate the *string code form*
    // (since mutate:rebind takes a code-string), checking that the
    // source contains no obvious shape violations (out-of-range
    // integer literals, malformed s-exprs). This is a "cheap,
    // best-effort" check — full type-level validation is a
    // follow-up.
    // SECURITY_EXEMPT: read-only schema check — no AST write (#2057/#2152).
    // GUARD_EXEMPT: read-only schema check — no AST write (#2986). PrimMeta.guard_exempt.
    // Issue #2628: private for (mutate :validate).
    ObservabilityPrims::register_stats_impl(
        "mutate:validate-against-schema",
        [&string_heap, &type_registry](std::span<const EvalValue> a) -> EvalValue {
            if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
                return make_bool(false);
            auto code_idx = as_string_idx(a[0]);
            auto type_idx = as_string_idx(a[1]);
            if (code_idx >= string_heap.size() || type_idx >= string_heap.size())
                return make_bool(false);
            std::string code = string_heap[code_idx];
            std::string type_name = string_heap[type_idx];
            if (!type_registry) {
                type_registry = new aura::core::TypeRegistry();
            }
            auto* treg = static_cast<aura::core::TypeRegistry*>(type_registry);
            if (!treg)
                return make_bool(false);
            auto ty = treg->lookup_type(type_name);
            if (!ty.valid())
                return make_bool(false); // no schema; treat as "no constraint"
            // Best-effort shape check on the code string. The
            // registered schema (if any) is consulted for an
            // `integer_min` / `integer_max` constraint; if the code
            // string contains a literal that violates the constraint,
            // we return a tagged violation pair.
            //
            // We deliberately keep this conservative: only literal
            // integer overflow is detected. Function bodies, variable
            // references, and dynamic values are not statically
            // validated here — those are follow-up work. The point of
            // the P0 ship is to give callers a *hook* for explicit
            // pre-mutation checks (and a tagged error path), not to
            // reimplement the type checker.
            std::string violation_reason;
            std::string violation_field;
            if (!validate_code_against_schema_simple(code, type_name, violation_reason,
                                                     violation_field)) {
                // Build (list "schema-violation" <reason> <field>) pair
                // in string_heap_ + pairs_.
                auto reason_idx = string_heap.size();
                string_heap.push_back(violation_reason);
                auto field_idx = string_heap.size();
                string_heap.push_back(violation_field);
                // ("schema-violation" reason field)
                auto reason_kw_idx = string_heap.size();
                string_heap.push_back(std::string("schema-violation"));
                // ... but keyword encoding goes through make_keyword in
                // a more complex path. We build the pair as a string-
                // tagged list (s-expression) and return it as a string
                // — the caller can (eval) it. This keeps the primitive
                // self-contained without needing a full keyword path.
                std::string repr =
                    "(schema-violation \"" + violation_reason + "\" \"" + violation_field + "\")";
                auto repr_idx = string_heap.size();
                string_heap.push_back(repr);
                return make_string(repr_idx);
            }
            return make_bool(true);
        });
    // #2628: security_exempt was on public meta; impl is stats-only + (mutate :validate).

    // (query:occurrence-stale? if-node-id) — Issue #339:
    // returns #t when the if-node's occurrence-narrowing
    // is stale (must re-analyze before the narrowing is
    // trusted). The staleness bit is set by
    // validate_occurrence_narrowing() when the
    // post-mutation predicate or var type no longer
    // matches the previously-recorded refined type.
    // Returns #f otherwise (fresh + #f on bad args / OOR).
    add("query:occurrence-stale?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        auto node_id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (node_id >= ws->size())
            return make_bool(false);
        return make_bool(ws->is_occurrence_stale(node_id) != 0);
    });

    // Issue #639: query:narrow-blame-stats. Returns the sum of
    // narrow stale-caught, blame-attached, invalidation-post-
    // mutate, provenance-hits, and safe-fallback counters.
    ObservabilityPrims::register_stats_impl(
        "query:narrow-blame-stats", [&ev](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            std::uint64_t stale_caught = 0;
            std::uint64_t blame_attached = 0;
            std::uint64_t invalidation = 0;
            std::uint64_t provenance_hits = 0;
            std::uint64_t safe_fallbacks = 0;
            if (m) {
                stale_caught = m->narrow_stale_caught_total.load(std::memory_order_relaxed);
                blame_attached = m->narrow_blame_attached_total.load(std::memory_order_relaxed);
                invalidation =
                    m->narrow_invalidation_post_mutate_total.load(std::memory_order_relaxed);
                provenance_hits = m->narrowing_provenance_total.load(std::memory_order_relaxed);
                safe_fallbacks = m->narrow_safe_fallback_total.load(std::memory_order_relaxed);
            }
            if (auto* ws = ev.workspace_flat()) {
                invalidation += ws->narrow_invalidation_post_mutate_count();
            }
            return make_int(static_cast<std::int64_t>(stale_caught + blame_attached + invalidation +
                                                      provenance_hits + safe_fallbacks));
        });

    // Issue #627: query:bidirectional-narrow-stats. Returns the sum
    // of check-mode narrow hits, synthesize/check switches,
    // post-mutate narrow consistency, and stale-check prevented.
    ObservabilityPrims::register_stats_impl(
        "query:bidirectional-narrow-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t check_hits =
                m->check_mode_narrow_hits_total.load(std::memory_order_relaxed);
            const std::uint64_t switches =
                m->synthesize_check_switch_count_total.load(std::memory_order_relaxed);
            const std::uint64_t consistency =
                m->post_mutate_narrow_consistency_total.load(std::memory_order_relaxed);
            const std::uint64_t stale_prevented =
                m->stale_check_narrow_prevented_total.load(std::memory_order_relaxed);
            return make_int(
                static_cast<std::int64_t>(check_hits + switches + consistency + stale_prevented));
        });

    // Issue #467: query:occurrence-stats. Returns the sum of 4
    // per-node occurrence-dirty + blame chain propagation counters:
    //   - occurrence_dirty_recoveries: narrowing_dirty_recovery_total
    //   - blame_chain_propagated: narrow_blame_attached_total +
    //     occurrence_blame_chain_complete_total
    //   - stale_narrowing_prevented: occurrence_stale_refreshes_total +
    //     stale_check_narrow_prevented_total
    //   - narrowing_refresh: narrowing_refresh_count_ (Evaluator)
    ObservabilityPrims::register_stats_impl(
        "query:occurrence-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t dirty_recovery =
                m ? m->narrowing_dirty_recovery_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t blame_attached =
                m ? m->narrow_blame_attached_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t blame_complete =
                m ? m->occurrence_blame_chain_complete_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t stale_refresh =
                m ? m->occurrence_stale_refreshes_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t stale_prevented =
                m ? m->stale_check_narrow_prevented_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t narrowing = ev->get_narrowing_refresh_count();
            return make_int(static_cast<std::int64_t>(dirty_recovery + blame_attached +
                                                      blame_complete + stale_refresh +
                                                      stale_prevented + narrowing));
        });

    // Issue #576: query:occurrence-blame-stats. Returns the sum
    // of 4 Task2 occurrence typing + blame/provenance counters:
    //   - stale_narrowing_prevented: stale_check_narrow_prevented_total
    //   - blame_chain_preserved: occurrence_blame_chain_complete_total
    //   - narrowing_refresh_count: narrowing_refresh_count_ (Evaluator)
    //   - provenance_mismatch: provenance_mismatch_ (Evaluator)
    ObservabilityPrims::register_stats_impl(
        "query:occurrence-blame-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t stale_prevented =
                m ? m->stale_check_narrow_prevented_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t blame_preserved =
                m ? m->occurrence_blame_chain_complete_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t narrowing = ev->get_narrowing_refresh_count();
            const std::uint64_t provenance = ev->get_provenance_mismatch();
            return make_int(static_cast<std::int64_t>(stale_prevented + blame_preserved +
                                                      narrowing + provenance));
        });

    // Issue #609: query:occurrence-narrow-stats. Returns the sum
    // of 4 post-mutation occurrence narrow recovery counters:
    //   - narrow_recoveries: occurrence_stale_refreshes_total +
    //     narrowing_reanalyzed_total (predicate re-analysis)
    //   - blame_attached: narrow_blame_attached_total
    //   - post_mutate_correctness: post_mutate_narrow_consistency_total
    //   - stale_narrow_prevented: stale_check_narrow_prevented_total
    ObservabilityPrims::register_stats_impl(
        "query:occurrence-narrow-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t narrow_recoveries =
                m->occurrence_stale_refreshes_total.load(std::memory_order_relaxed) +
                m->narrowing_reanalyzed_total.load(std::memory_order_relaxed);
            const std::uint64_t blame_attached =
                m->narrow_blame_attached_total.load(std::memory_order_relaxed);
            const std::uint64_t post_mutate_correctness =
                m->post_mutate_narrow_consistency_total.load(std::memory_order_relaxed);
            const std::uint64_t stale_narrow_prevented =
                m->stale_check_narrow_prevented_total.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(narrow_recoveries + blame_attached +
                                                      post_mutate_correctness +
                                                      stale_narrow_prevented));
        });

    // Issue #537 / #518 Phase 2: query:occurrence-narrowing-stats.
    // Returns the sum of stale-refresh + blame-chain-complete
    // counters from CompilerMetrics (post-mutation re-narrow
    // provenance observability).
    ObservabilityPrims::register_stats_impl(
        "query:occurrence-narrowing-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t stale_refreshes =
                m->occurrence_stale_refreshes_total.load(std::memory_order_relaxed);
            const std::uint64_t blame_complete =
                m->occurrence_blame_chain_complete_total.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(stale_refreshes + blame_complete));
        });

    // (query:occurrence-stale-count) — Issue #339:
    // returns the current count of stale occurrence
    // nodes in the workspace. Cheap O(n) walk; intended
    // for observability + AI agent monitoring.
    ObservabilityPrims::register_stats_impl(
        "query:occurrence-stale-count", [&ev](const auto&) -> EvalValue {
            auto* ws = ev.workspace_flat();
            if (!ws)
                return make_int(0);
            return make_int(static_cast<std::int64_t>(ws->occurrence_stale_count()));
        });

    // (query:mark-occurrence-stale if-node-id) — Issue
    // #339: explicitly mark an if-node as stale.
    // Used by callers that decide staleness outside
    // the type-checker (e.g. an external validator or
    // a test). Returns #t on success, #f on bad args.
    add("query:mark-occurrence-stale", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        auto node_id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (node_id >= ws->size())
            return make_bool(false);
        ws->mark_occurrence_stale(node_id);
        return make_bool(true);
    });

    // Issue #625: query:pass-pipeline-incremental-stats-hash —
    // Agent-discoverable structured companion to the existing
    // query:pass-pipeline-stats (#494/#606, 10-field) and
    // query:pass-contracts-stats (#406, int-sum-of-7). This
    // primitive specifically covers AC4 from the issue body
    // — the incremental re-lower dashboard the Agent reads to
    // confirm dirty-block short-circuit savings.
    //
    // Fields (6):
    //   - passes-run                 lifetime # of full
    //                                run_pipeline() invocations
    //                                (pass_pipeline_runs_total,
    //                                bumped in pass_manager.ixx)
    //   - contracts-checked          synthetic: zerooverhead_wins /
    //                                (zerooverhead_wins + value_
    //                                dispatch_miss_count + 1) * 100;
    //                                measures how often the
    //                                Contracts / cheap-view dispatch
    //                                was used as a fast path
    //   - pure-delegation-hits       ShapeWrap + LinearOwnershipWrap
    //                                pure_delegation_hits() sum
    //   - shortcircuit-savings       passes_skipped_dirty_pipeline
    //                                + module_dirty_skips (total
    //                                work avoided by the dirty
    //                                short-circuit path)
    //   - dirty-blocks-skipped       passes_skipped_dirty_pipeline
    //                                (more directly: each pass
    //                                skipped by the dirty filter)
    //   - schema == 625              sentinel for Agent drift
    //                                detection (mirrors #618+#620+
    //                                #621+#622+#623+#624 sentinels)
    //
    // Discovery before this PR: the C++ side already exposes the
    // full pipeline + contracts + dirty-skipped counter surface
    // via aura::compiler::pipeline_yield_count + passes_skipped_
    // dirty_pipeline + passes_skipped_type_dirty + ShapeWrap +
    // LinearOwnershipWrap + CompilerMetrics::relower_*
    // (added by #494 / #606 / #406 / #686). The single NEW
    // contribution is the structured primitive the issue body
    // AC4 lists by name + the `pass_pipeline_runs_total` counter
    // (per-full-pipeline-run, not per-pass).
    //
    // The remaining #625 AC work (more `requires` constraints on
    // Pass/AnalysisPass, fold-expressions in run_pipeline, uniform
    // ShapeProfilerWrap / LinearOwnershipWrap / DirtyImpactWrap
    // classes, estimate_relower_blocks integration with
    // invalidate_function) is invasive C++ that needs benchmarking
    // + perf regression coverage before going in.
    ObservabilityPrims::register_stats_impl(
        "query:pass-pipeline-incremental-stats-hash",
        [&ev, &string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            const std::uint64_t passes_run =
                pass_pipeline_runs_total.load(std::memory_order_relaxed);
            const std::uint64_t dirty_blocks_skipped =
                aura::compiler::passes_skipped_dirty_pipeline.load(std::memory_order_relaxed);
            const std::uint64_t shortcircuit_savings =
                dirty_blocks_skipped +
                (ev.compiler_metrics()
                     ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                           ->module_dirty_skips.load(std::memory_order_relaxed)
                     : 0);
            const std::uint64_t zero_wins =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->coercion_zerooverhead_win_total.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t dispatch_miss =
                types::value_dispatch_miss_count.load(std::memory_order_relaxed);
            const std::uint64_t contracts_denom = zero_wins + dispatch_miss + 1;
            const std::int64_t contracts_checked =
                static_cast<std::int64_t>((zero_wins * 100) / contracts_denom);
            const std::uint64_t pure_delegation =
                aura::compiler::ShapeWrap::pure_delegation_hits() +
                aura::compiler::LinearOwnershipWrap::pure_delegation_hits();
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
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("passes-run", static_cast<std::int64_t>(passes_run));
            insert_kv("contracts-checked", contracts_checked);
            insert_kv("pure-delegation-hits", static_cast<std::int64_t>(pure_delegation));
            insert_kv("shortcircuit-savings", static_cast<std::int64_t>(shortcircuit_savings));
            insert_kv("dirty-blocks-skipped", static_cast<std::int64_t>(dirty_blocks_skipped));
            insert_kv("schema", 625);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

} // namespace aura::compiler::primitives_detail
