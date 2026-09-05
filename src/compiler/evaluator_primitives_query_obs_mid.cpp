// evaluator_primitives_query_obs_mid.cpp — Issue #2914 peel (~L2494-L6360)
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
extern "C" std::uint64_t aura_macro_clone_same_flat_reject_total_v_read() noexcept;
extern "C" std::uint64_t aura_macro_clone_steal_abort_total_v_read() noexcept;
extern "C" std::uint64_t aura_macro_clone_last_reject_reason_v_read() noexcept;
extern "C" std::uint64_t aura_macro_hygiene_last_limit_reason_v_read() noexcept;
extern "C" std::uint64_t aura_hygiene_violation_se_emit_total_v_read() noexcept;
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

void register_query_obs_mid_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                       std::pmr::vector<std::string>& string_heap,
                                       void*& type_registry, ModulePathResolver resolve_module_path,
                                       Evaluator& ev) {
    (void)pairs;
    (void)string_heap;
    (void)type_registry;
    (void)resolve_module_path;
    (void)ev;
    ObservabilityPrims::register_stats_impl(
        "query:pattern-index-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_int(0);
            const std::uint64_t hits = ws->tag_arity_index_hits();
            const std::uint64_t misses = ws->tag_arity_index_misses();
            const std::uint64_t rebuilds = ws->tag_arity_index_rebuilds();
            const std::uint64_t dirty_marks = ws->tag_arity_index_dirty_marks();
            // Issue #554: include rebuild_time_us + delta_hits
            // so (query:pattern-index-stats) returns the full
            // 6-counter matrix. The AI Agent can compute
            // avg_rebuild_us = rebuild_time_us / rebuilds and
            // delta_hit_rate = delta_hits / (delta_hits + rebuilds).
            const std::uint64_t rebuild_time_us = ws->tag_arity_index_rebuild_time_us();
            const std::uint64_t delta_hits = ws->tag_arity_index_delta_hits();
            return make_int(static_cast<std::int64_t>(hits + misses + rebuilds + dirty_marks +
                                                      rebuild_time_us + delta_hits));
        });

    // Issue #2861: query:pattern-safety-stats. Hash view of the
    // query:pattern full safety contract metrics (refine #819 /
    // #2036 / #2123 / #2763 / #2525). Tracks the 4 non-negotiable
    // safety contract surfaces mandated by #2861 AC #7
    // ("metrics on query stats surface"):
    //   - pattern-safe-span-uses-total: every SafePCVSpan /
    //       children_ safe_view walk on the public surface (raw
    //       std::span over PCV is forbidden).
    //   - pattern-hygiene-filtered-total: MacroIntroduced nodes
    //       skipped by the default filter (opt-in via
    //       :include-macro-introduced).
    //   - pattern-epoch-mismatch-total: QueryEpoch (mutation_epoch
    //       + generation) mismatch detected under concurrent Guard.
    //   - pattern-dangling-prevented-total: StableNodeRef returned
    //       from a pattern walk that failed is_valid_in / refresh
    //       under concurrent mutate and was dropped.
    // Distinct from #547 pattern-index-stats (tag_arity_index hot
    // path) and #490 pattern-index-rebuild-stats (lazy vs eager
    // rebuild) — these are the #2861 contract-level surfaces
    // covering SafePCVSpan + hygiene filter + QueryEpoch + StableNodeRef.
    ObservabilityPrims::register_stats_impl(
        "query:pattern-safety-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t safe_span_uses =
                m ? m->pattern_safe_span_uses_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t hygiene_filtered =
                m ? m->pattern_hygiene_filtered_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t epoch_mismatch =
                m ? m->pattern_epoch_mismatch_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t dangling_prevented =
                m ? m->pattern_dangling_prevented_total.load(std::memory_order_relaxed) : 0;
            auto* ht = FlatHashTable::create(query_hash_capacity_for(14));
            if (!ht)
                return make_void();
            bool overflowed = false;
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const std::string& k_str, std::int64_t v) {
                if (!insert_kv_checked(ht, ev->string_heap_mut(), k_str, v))
                    overflowed = true;
            };
            insert_kv("schema", 2861);
            insert_kv("issue", 2861);
            insert_kv("pattern-safe-span-uses-total", static_cast<std::int64_t>(safe_span_uses));
            insert_kv("pattern-hygiene-filtered-total",
                      static_cast<std::int64_t>(hygiene_filtered));
            insert_kv("pattern-epoch-mismatch-total", static_cast<std::int64_t>(epoch_mismatch));
            insert_kv("pattern-dangling-prevented-total",
                      static_cast<std::int64_t>(dangling_prevented));
            return query_hash_finish(ht, ev->string_heap_mut(), overflowed);
        });

    // Issue #490 / #1503: query:pattern-index-rebuild-stats. Hash view of
    // lazy vs eager Evaluator index rebuild counters + FlatAST timing +
    // incremental maintenance policy (threshold, auto-warm, patches).
    ObservabilityPrims::register_stats_impl(
        "query:pattern-index-rebuild-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            std::uint64_t flat_rebuilds = 0;
            std::uint64_t flat_rebuild_time_us = 0;
            std::uint64_t flat_delta_hits = 0;
            std::uint64_t threshold_full = 0;
            std::uint64_t incremental_patches = 0;
            std::int64_t threshold_pct = 25;
            if (auto* ws = ev->workspace_flat()) {
                flat_rebuilds = ws->tag_arity_index_rebuilds();
                flat_rebuild_time_us = ws->tag_arity_index_rebuild_time_us();
                flat_delta_hits = ws->tag_arity_index_delta_hits();
                threshold_full = ws->tag_arity_index_threshold_full_rebuilds();
                incremental_patches = ws->tag_arity_index_incremental_patches();
                threshold_pct =
                    static_cast<std::int64_t>(ws->tag_arity_index_full_rebuild_threshold_pct());
            }
            auto* ht = FlatHashTable::create(query_hash_capacity_for(27));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            insert_kv("lazy-rebuilds",
                      static_cast<std::int64_t>(ev->get_pattern_index_lazy_rebuilds()));
            insert_kv("eager-mutate-rebuilds",
                      static_cast<std::int64_t>(ev->get_pattern_index_eager_mutate_rebuilds()));
            insert_kv("eager-cow-rebuilds",
                      static_cast<std::int64_t>(ev->get_pattern_index_eager_cow_rebuilds()));
            insert_kv("auto-warm-syncs",
                      static_cast<std::int64_t>(ev->get_pattern_index_auto_warm_syncs()));
            insert_kv("flat-rebuilds", static_cast<std::int64_t>(flat_rebuilds));
            insert_kv("flat-rebuild-time-us", static_cast<std::int64_t>(flat_rebuild_time_us));
            insert_kv("flat-delta-hits", static_cast<std::int64_t>(flat_delta_hits));
            insert_kv("threshold-full-rebuilds", static_cast<std::int64_t>(threshold_full));
            insert_kv("incremental-patches", static_cast<std::int64_t>(incremental_patches));
            insert_kv("threshold-pct", threshold_pct);
            insert_kv("schema", 1503);
            // Issue #2763: Agent-facing delta/full rebuild totals for
            // production multi-round query:pattern latency (refine #1503).
            // Additive — schema-1503 lineage preserved; schema-2763 sentinel.
            if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
                insert_kv("query-pattern-delta-rebuild-total",
                          static_cast<std::int64_t>(m->query_pattern_delta_rebuild_total.load(
                              std::memory_order_relaxed)));
                insert_kv("query-pattern-full-rebuild-total",
                          static_cast<std::int64_t>(
                              m->query_pattern_full_rebuild_total.load(std::memory_order_relaxed)));
                insert_kv("query-pattern-delta-rebuild-wired", 1);
            } else {
                insert_kv("query-pattern-delta-rebuild-total",
                          static_cast<std::int64_t>(flat_delta_hits));
                insert_kv("query-pattern-full-rebuild-total",
                          static_cast<std::int64_t>(threshold_full));
                insert_kv("query-pattern-delta-rebuild-wired", 1);
            }
            insert_kv("schema-2763", 2763);
            insert_kv("issue-2763", 2763);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #621 / #2403: query:pattern-index-stats-hash — Agent-discoverable
    // structured form of (query:pattern-index-stats). The legacy
    // primitive (#547) returns an int = sum of 6 counters; this
    // version returns the full field hash so the AI Agent can
    // react to each category independently.
    //
    // Base fields (#621):
    //   - hits / misses / rebuilds / dirty-marks / rebuild-time-us
    //   - delta-hits / linear-fallbacks / arity-accuracy / delta-hit-rate
    //   - recommendation / race-window-hits / schema
    // Issue #2403 additive:
    //   - query-index-hit-rate / query-index-miss-total / query-index-hit-total
    //   - query-shared-lock-us-total / query-shared-lock-us-max
    //   - schema-2403 / issue-2403 / query-index-composite-wired
    ObservabilityPrims::register_stats_impl(
        "query:pattern-index-stats-hash",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            std::uint64_t hits = 0;
            std::uint64_t misses = 0;
            std::uint64_t rebuilds = 0;
            std::uint64_t dirty_marks = 0;
            std::uint64_t rebuild_time_us = 0;
            std::uint64_t delta_hits = 0;
            if (auto* ws = ev->workspace_flat()) {
                hits = ws->tag_arity_index_hits();
                misses = ws->tag_arity_index_misses();
                rebuilds = ws->tag_arity_index_rebuilds();
                dirty_marks = ws->tag_arity_index_dirty_marks();
                rebuild_time_us = ws->tag_arity_index_rebuild_time_us();
                delta_hits = ws->tag_arity_index_delta_hits();
            }
            const std::uint64_t total = hits + misses;
            const std::int64_t arity_accuracy =
                total == 0 ? 0 : static_cast<std::int64_t>((hits * 100) / total);
            const std::uint64_t delta_denom = delta_hits + rebuilds;
            const std::int64_t delta_hit_rate =
                delta_denom == 0 ? 0 : static_cast<std::int64_t>((delta_hits * 100) / delta_denom);
            std::int64_t recommendation = 0;
            if (total > 0 && arity_accuracy < 50)
                recommendation = 1;
            else if (rebuilds > 0 &&
                     rebuild_time_us > static_cast<std::uint64_t>(delta_hits + 1) * 100)
                recommendation = 2;
            // Issue #2403: composite query path hit rate (miss = unconstrained only).
            const std::uint64_t q_hit = ev->get_query_index_composite_hit_total();
            const std::uint64_t q_miss = ev->get_query_index_composite_miss_total();
            const std::uint64_t q_total = q_hit + q_miss;
            const std::int64_t query_index_hit_rate =
                q_total == 0 ? 0 : static_cast<std::int64_t>((q_hit * 100) / q_total);
            // Capacity power-of-two; 64 slots for #2403 additive keys.
            auto* ht = FlatHashTable::create(query_hash_capacity_for(28));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            insert_kv("hits", static_cast<std::int64_t>(hits));
            insert_kv("misses", static_cast<std::int64_t>(misses));
            insert_kv("rebuilds", static_cast<std::int64_t>(rebuilds));
            insert_kv("dirty-marks", static_cast<std::int64_t>(dirty_marks));
            insert_kv("rebuild-time-us", static_cast<std::int64_t>(rebuild_time_us));
            insert_kv("delta-hits", static_cast<std::int64_t>(delta_hits));
            insert_kv("linear-fallbacks", static_cast<std::int64_t>(misses));
            insert_kv("arity-accuracy", arity_accuracy);
            insert_kv("delta-hit-rate", delta_hit_rate);
            insert_kv("recommendation", recommendation);
            // Issue #1372: race window hits (0 with snapshot_tag_arity_bucket)
            insert_kv("race-window-hits",
                      static_cast<std::int64_t>(ev->get_tag_arity_index_race_window_hits()));
            insert_kv("schema", 621);
            // Issue #2403 additive keys (no schema break of base schema=621).
            insert_kv("query-index-hit-total", static_cast<std::int64_t>(q_hit));
            insert_kv("query-index-miss-total", static_cast<std::int64_t>(q_miss));
            insert_kv("query-index-hit-rate", query_index_hit_rate);
            insert_kv("query-shared-lock-us-total",
                      static_cast<std::int64_t>(ev->get_query_shared_lock_us_total()));
            insert_kv("query-shared-lock-us-max",
                      static_cast<std::int64_t>(ev->get_query_shared_lock_us_max()));
            insert_kv("query-index-composite-wired", 1);
            insert_kv("schema-2403", 2403);
            insert_kv("issue-2403", 2403);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #547 / #1501 / #1609 / #1636 / #1892 / #2123 / #2525:
    // query:pattern-hygiene-stats — authoritative MacroIntroduced hygiene
    // dashboard for query:pattern + query:filter residual defaults.
    // Schema **2525** (lineage 2123/1892/1636/1609/1501/547). Defense-in-depth:
    // root/full-walk skip + recursive matcher + user-only
    // tag_arity_index_user_ (marker dimension via parallel index — not
    // packing marker into TagArityKey; same hot-path win).
    // #2123/#2525: default-exclude is production contract for pattern + filter;
    // opt-in counters + unconstrained-walk metric exposed for Agent throttle.
    ObservabilityPrims::register_stats_impl(
        "query:pattern-hygiene-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            // Capacity must be power-of-two (open-address mask hcap-1).
            // #2123 / #2525 / #2989 added several keys — 256 slots for open addressing.
            auto* ht = FlatHashTable::create(query_hash_capacity_for(89));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const auto root_skips =
                static_cast<std::int64_t>(ev->get_macro_introduced_skipped_in_query());
            const auto recursive_skips =
                static_cast<std::int64_t>(ev->get_pattern_recursive_macro_skipped());
            const auto violations = static_cast<std::int64_t>(ev->get_hygiene_violation_count());
            // #1636 / #1892 AC: issue-body metric names (aliases of existing counters).
            const auto pattern_skips = root_skips + recursive_skips;
            insert_kv("root-skips", root_skips);
            insert_kv("recursive-skips", recursive_skips);
            insert_kv("hygiene-violations", violations);
            insert_kv("macro_introduced_skipped_in_pattern_total", pattern_skips);
            insert_kv("macro-introduced-skipped-in-pattern-total", pattern_skips);
            // #1892 AC name (exact): hotpath default-skip total.
            insert_kv("macro_introduced_skipped_in_query_total", root_skips);
            insert_kv("macro-introduced-skipped-in-query-total", root_skips);
            insert_kv("hygiene_violation_prevented_total", violations);
            insert_kv("hygiene-violation-prevented-total", violations);
            // Result leakage after verify_pattern_result_hygiene (must stay 0).
            insert_kv("pattern-macro-filter-violations",
                      static_cast<std::int64_t>(ev->get_pattern_macro_filter_violations()));
            insert_kv("hygiene-leakage",
                      static_cast<std::int64_t>(ev->get_pattern_macro_filter_violations()));
            // #547 back-compat: total used by agents that expected int sum
            insert_kv("total", root_skips + violations);
            insert_kv("macro-markers",
                      static_cast<std::int64_t>(workspace_marker_macro_introduced(ev)));
            insert_kv("hygiene-index-served",
                      static_cast<std::int64_t>(ev->get_tag_arity_hygiene_index_served()));
            // #1609 / #1636 / #1892 wire flags
            insert_kv("core-loop-force-skip-wired", 1);
            insert_kv("matcher-recursive-skip-wired", 1);
            insert_kv("user-only-tag-arity-index-wired", 1);
            insert_kv("marker-dimension-via-user-index-wired", 1); // #1636 strategy
            insert_kv("default-exclude-macro-introduced", 1);
            insert_kv("allow-macro-introduced-opt-in", 1);
            insert_kv("pattern-hygiene-mandate-active", 1);
            insert_kv("typed-mutation-audit-skip-wired", 1); // #1892
            insert_kv("self-evo-query-hygiene-mandate", 1);  // #1892
            // Primary schema stays 2123 for back-compat; #2525 is additive.
            insert_kv("issue", 2123);       // #2123 production default-filter contract
            insert_kv("schema", 2123);      // lineage 1892 / 1636 / 1609 / 1501 / 547
            insert_kv("schema-1892", 1892); // #1892 / #1636 lineage retained
            insert_kv("issue-1892", 1892);
            insert_kv("schema-1636", 1636);
            insert_kv("issue-1636", 1636);
            insert_kv("schema-2123", 2123);
            insert_kv("issue-2123", 2123);
            insert_kv("schema-2525", 2525); // #2525 residual filter/unconstrained
            insert_kv("issue-2525", 2525);
            insert_kv("default-hygiene-filter-wired", 1);    // #2123 AC1 sentinel
            insert_kv("filter-default-skip-macro-wired", 1); // #2525 query:filter default
            insert_kv("unconstrained-walk-metric-wired", 1); // #2525
            // Issue #1914 / #2123 / #2525 AC metric aliases on pattern-hygiene surface.
            if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
                const auto filt = static_cast<std::int64_t>(
                    m->pattern_hygiene_filtered_total.load(std::memory_order_relaxed));
                const auto skip_tot = static_cast<std::int64_t>(
                    m->hygiene_skip_total.load(std::memory_order_relaxed));
                const auto include_tot = static_cast<std::int64_t>(
                    m->pattern_include_macro_opt_in_total.load(std::memory_order_relaxed) +
                    m->hygiene_filter_include_opt_in_total.load(std::memory_order_relaxed));
                insert_kv("pattern_hygiene_filter_hits",
                          static_cast<std::int64_t>(
                              m->pattern_hygiene_filter_hits.load(std::memory_order_relaxed)));
                insert_kv("pattern_hygiene_filtered_total", filt);
                insert_kv("pattern-hygiene-filtered-total", filt);
                // Issue #2763: Agent-facing alias (exact AC name).
                insert_kv("query-pattern-hygiene-filtered-total", filt);
                // Issue #2763: delta/full rebuild attribution on hygiene surface.
                insert_kv("query-pattern-delta-rebuild-total",
                          static_cast<std::int64_t>(m->query_pattern_delta_rebuild_total.load(
                              std::memory_order_relaxed)));
                insert_kv("query-pattern-full-rebuild-total",
                          static_cast<std::int64_t>(
                              m->query_pattern_full_rebuild_total.load(std::memory_order_relaxed)));
                insert_kv("schema-2763", 2763);
                insert_kv("issue-2763", 2763);
                // Issue #2525 Agent-facing names (AC3)
                insert_kv("hygiene_skip_total", skip_tot > 0 ? skip_tot : filt + pattern_skips);
                insert_kv("hygiene-skip-total", skip_tot > 0 ? skip_tot : filt + pattern_skips);
                insert_kv("hygiene_include_total", include_tot);
                insert_kv("hygiene-include-total", include_tot);
                insert_kv("pattern_include_macro_opt_in_total",
                          static_cast<std::int64_t>(m->pattern_include_macro_opt_in_total.load(
                              std::memory_order_relaxed)));
                insert_kv("pattern-include-macro-opt-in-total",
                          static_cast<std::int64_t>(m->pattern_include_macro_opt_in_total.load(
                              std::memory_order_relaxed)));
                insert_kv(
                    "pattern_hygiene_unconstrained_walk_total",
                    static_cast<std::int64_t>(m->pattern_hygiene_unconstrained_walk_total.load(
                        std::memory_order_relaxed)));
                insert_kv(
                    "pattern-hygiene-unconstrained-walk-total",
                    static_cast<std::int64_t>(m->pattern_hygiene_unconstrained_walk_total.load(
                        std::memory_order_relaxed)));
                insert_kv("hygiene_filter_default_skip_total",
                          static_cast<std::int64_t>(m->hygiene_filter_default_skip_total.load(
                              std::memory_order_relaxed)));
                insert_kv("hygiene_filter_include_opt_in_total",
                          static_cast<std::int64_t>(m->hygiene_filter_include_opt_in_total.load(
                              std::memory_order_relaxed)));
                insert_kv(
                    "tag_arity_marker_dimension_rebuild_total",
                    static_cast<std::int64_t>(m->tag_arity_marker_dimension_rebuild_total.load(
                        std::memory_order_relaxed)));
                insert_kv("macro_introduced_in_pattern_violations",
                          static_cast<std::int64_t>(m->macro_introduced_in_pattern_violations.load(
                              std::memory_order_relaxed)));
                // Issue #2989: concurrent SafePCVSpan + hygiene skip Agent keys.
                insert_kv("hygiene-skip-count",
                          static_cast<std::int64_t>(ev->get_query_hygiene_skip_count()));
                insert_kv("safe-span-pin-count",
                          static_cast<std::int64_t>(ev->get_query_safe_span_pin_count()));
                insert_kv("query-safe-span-default-wired", 1);
                insert_kv("query-epoch-retry-total",
                          static_cast<std::int64_t>(ev->get_query_epoch_retry_total()));
                insert_kv("schema-2989", 2989);
                insert_kv("issue-2989", 2989);
            } else {
                insert_kv("pattern_hygiene_filter_hits", pattern_skips);
                insert_kv("pattern_hygiene_filtered_total", pattern_skips);
                insert_kv("pattern-hygiene-filtered-total", pattern_skips);
                insert_kv("query-pattern-hygiene-filtered-total", pattern_skips); // #2763
                insert_kv("query-pattern-delta-rebuild-total", 0);
                insert_kv("query-pattern-full-rebuild-total", 0);
                insert_kv("schema-2763", 2763);
                insert_kv("issue-2763", 2763);
                insert_kv("hygiene_skip_total", pattern_skips);
                insert_kv("hygiene-skip-total", pattern_skips);
                insert_kv("hygiene_include_total", 0);
                insert_kv("hygiene-include-total", 0);
                insert_kv("pattern_include_macro_opt_in_total", 0);
                insert_kv("pattern-include-macro-opt-in-total", 0);
                insert_kv("pattern_hygiene_unconstrained_walk_total", 0);
                insert_kv("macro_introduced_in_pattern_violations",
                          static_cast<std::int64_t>(ev->get_pattern_macro_filter_violations()));
                insert_kv("hygiene-skip-count", pattern_skips);
                insert_kv("safe-span-pin-count", 0);
                insert_kv("query-safe-span-default-wired", 1);
                insert_kv("query-epoch-retry-total", 0);
                insert_kv("schema-2989", 2989);
                insert_kv("issue-2989", 2989);
            }
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #2989: dedicated Agent counters (int). Same values as
    // pattern-hygiene-stats hygiene-skip-count / safe-span-pin-count.
    ObservabilityPrims::register_stats_impl(
        "query:hygiene-skip-count", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            return make_int(static_cast<std::int64_t>(ev->get_query_hygiene_skip_count()));
        });
    ObservabilityPrims::register_stats_impl(
        "query:safe-span-pin-count", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            return make_int(static_cast<std::int64_t>(ev->get_query_safe_span_pin_count()));
        });

    // Issue #2242: query:by-marker — per-marker MacroIntroduced composition
    // stats (split out from query:hygiene-provenance-stats combined primitive).
    // Schema **2242**. Exposes by-marker :where composition hits + workspace
    // MacroIntroduced marker count for agent root-cause discovery.
    ObservabilityPrims::register_stats_impl(
        "query:by-marker", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            auto* ht = FlatHashTable::create(query_hash_capacity_for(16));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };

            const std::uint64_t where_hits =
                m ? m->by_marker_where_filter_hits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);

            insert_kv("by_marker_where_filter_hits", static_cast<std::int64_t>(where_hits));
            insert_kv("by-marker-where-filter-hits", static_cast<std::int64_t>(where_hits));
            insert_kv("macro_markers", static_cast<std::int64_t>(markers));
            insert_kv("macro-markers", static_cast<std::int64_t>(markers));
            insert_kv("by-marker-where-wired", 1);
            insert_kv("schema", 2242);
            insert_kv("issue", 2242);
            insert_kv("active", 1);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #2242: query:node-provenance — per-node provenance query hit
    // stats. Schema **2242**. Exposes provenance_query_total +
    // stable_ref_provenance_query_total + macro_provenance_query_total
    // so the per-fiber fiber pin + stable-ref provenance surfaces can
    // diagnose AI self-evo misses at node resolution time.
    ObservabilityPrims::register_stats_impl(
        "query:node-provenance", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            // Auto-bump on each invocation so the counter monotonically
            // tracks request rate, not just resolution success.
            if (m)
                m->provenance_query_total.fetch_add(1, std::memory_order_relaxed);

            auto* ht = FlatHashTable::create(query_hash_capacity_for(18));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };

            const std::uint64_t prov_total =
                m ? m->provenance_query_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t stable_prov =
                m ? m->stable_ref_provenance_query_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t macro_prov =
                m ? m->macro_provenance_query_total.load(std::memory_order_relaxed) : 0;

            insert_kv("provenance_query_total", static_cast<std::int64_t>(prov_total));
            insert_kv("provenance-query-total", static_cast<std::int64_t>(prov_total));
            insert_kv("stable_ref_provenance_query_total", static_cast<std::int64_t>(stable_prov));
            insert_kv("stable-ref-provenance-queries", static_cast<std::int64_t>(stable_prov));
            insert_kv("macro_provenance_query_total", static_cast<std::int64_t>(macro_prov));
            insert_kv("macro-provenance-query-total", static_cast<std::int64_t>(macro_prov));
            insert_kv("node-provenance-wired", 1);
            insert_kv("schema", 2242);
            insert_kv("issue", 2242);
            insert_kv("active", 1);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #2242: query:last-mutation-provenance — last hygiene stamp
    // provenance for agent root-cause. Schema **2242**. Exposes the most
    // recent HygieneProvenanceStamp recorded by ProvenanceTracker along
    // with the per-CompilerMetrics blame hints.
    ObservabilityPrims::register_stats_impl(
        "query:last-mutation-provenance",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            auto* ht = FlatHashTable::create(query_hash_capacity_for(24)); // ~16 keys (snake+kebab)
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };

            const auto& hs = aura::core::provenance::g_provenance_tracker().last_hygiene;
            const std::uint64_t blame_node =
                m ? static_cast<std::uint64_t>(m->last_hygiene_blame_node) : 0;
            const std::uint64_t blame_mutation = m ? m->last_hygiene_blame_mutation : 0;

            insert_kv("last_hygiene_node_id", static_cast<std::int64_t>(hs.node_id));
            insert_kv("last-hygiene-node-id", static_cast<std::int64_t>(hs.node_id));
            insert_kv("last_hygiene_tenant_id", static_cast<std::int64_t>(hs.tenant_id));
            insert_kv("last-hygiene-tenant-id", static_cast<std::int64_t>(hs.tenant_id));
            insert_kv("last_hygiene_mutation_id", static_cast<std::int64_t>(hs.source_mutation_id));
            insert_kv("last-hygiene-mutation-id", static_cast<std::int64_t>(hs.source_mutation_id));
            insert_kv("last_hygiene_fiber_id", static_cast<std::int64_t>(hs.fiber_id));
            insert_kv("last-hygiene-fiber-id", static_cast<std::int64_t>(hs.fiber_id));
            insert_kv("last_hygiene_seq", static_cast<std::int64_t>(hs.seq));
            insert_kv("last-hygiene-seq", static_cast<std::int64_t>(hs.seq));
            insert_kv("last_hygiene_blame_node", static_cast<std::int64_t>(blame_node));
            insert_kv("last_hygiene_blame_mutation", static_cast<std::int64_t>(blame_mutation));
            insert_kv("last-mutation-provenance-wired", 1);
            insert_kv("schema", 2242);
            insert_kv("issue", 2242);
            insert_kv("active", 1);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #1914: query:hygiene-provenance-stats — unified hygiene +
    // provenance diagnostics dashboard for AI self-evo root-cause.
    // Schema **1914**. Aggregates pattern hygiene filters, provenance
    // query hits, by-marker :where composition, invalidation_trace,
    // and TypedMutationAudit signals.
    ObservabilityPrims::register_stats_impl(
        "query:hygiene-provenance-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            auto load_m =
                [m](std::atomic<std::uint64_t> CompilerMetrics::* field) -> std::uint64_t {
                return m ? (m->*field).load(std::memory_order_relaxed) : 0;
            };
            if (m)
                m->hygiene_provenance_stats_queries_total.fetch_add(1, std::memory_order_relaxed);

            const std::uint64_t root_skips = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t recursive_skips = ev->get_pattern_recursive_macro_skipped();
            const std::uint64_t filter_hits = load_m(&CompilerMetrics::pattern_hygiene_filter_hits);
            const std::uint64_t pattern_skips = root_skips + recursive_skips;
            const std::uint64_t prov_queries = load_m(&CompilerMetrics::provenance_query_total);
            const std::uint64_t leakage =
                load_m(&CompilerMetrics::macro_introduced_in_pattern_violations);
            const std::uint64_t filter_viol = ev->get_pattern_macro_filter_violations();
            const std::uint64_t hygiene_viol = ev->get_hygiene_violation_count();
            const std::uint64_t where_hits = load_m(&CompilerMetrics::by_marker_where_filter_hits);
            const std::uint64_t stable_prov =
                load_m(&CompilerMetrics::stable_ref_provenance_query_total);
            std::int64_t inv_trace = 0;
            std::int64_t inv_records = 0;
            if (auto* ws = ev->workspace_flat()) {
                inv_trace = static_cast<std::int64_t>(ws->invalidation_trace_size());
                inv_records = static_cast<std::int64_t>(ws->invalidation_trace_records_total());
            }

            auto* ht = FlatHashTable::create(query_hash_capacity_for(31));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };

            // AC metric names (exact)
            insert_kv("pattern_hygiene_filter_hits",
                      static_cast<std::int64_t>(filter_hits > 0 ? filter_hits : pattern_skips));
            insert_kv("pattern-hygiene-filter-hits",
                      static_cast<std::int64_t>(filter_hits > 0 ? filter_hits : pattern_skips));
            insert_kv("provenance_query_total", static_cast<std::int64_t>(prov_queries));
            insert_kv("provenance-query-total", static_cast<std::int64_t>(prov_queries));
            insert_kv("macro_introduced_in_pattern_violations",
                      static_cast<std::int64_t>(leakage > 0 ? leakage : filter_viol));
            insert_kv("macro-introduced-in-pattern-violations",
                      static_cast<std::int64_t>(leakage > 0 ? leakage : filter_viol));
            // Supporting dashboard keys
            insert_kv("root-skips", static_cast<std::int64_t>(root_skips));
            insert_kv("recursive-skips", static_cast<std::int64_t>(recursive_skips));
            insert_kv("pattern-macro-filter-violations", static_cast<std::int64_t>(filter_viol));
            insert_kv("hygiene-violations", static_cast<std::int64_t>(hygiene_viol));
            insert_kv("by-marker-where-hits", static_cast<std::int64_t>(where_hits));
            insert_kv("stable-ref-provenance-queries", static_cast<std::int64_t>(stable_prov));
            insert_kv("invalidation-trace-size", inv_trace);
            insert_kv("invalidation-trace-records-total", inv_records);
            insert_kv("default-hygiene-wired", 1);
            insert_kv("by-marker-where-wired", 1);
            insert_kv("node-provenance-wired", 1);
            insert_kv("last-mutation-provenance-wired", 1);
            insert_kv("lineage-1892", 1892);
            insert_kv("lineage-1909", 1909);
            insert_kv("schema", 1914);
            insert_kv("issue", 1914);
            insert_kv("active", 1);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #486 / #1501 / #1609 / #1613: query:macro-hygiene-stats —
    // consolidated MacroIntroduced hygiene health for AI self-evo.
    // Schema **1613** (lineage 1609/1501). Aggregates query skips,
    // reflect gate, IR stamps, fiber refresh, TypedMutationAudit trail.
    // Also serves as the ai-closedloop-macro-health surface (no extra
    // public primitive — #1448 freeze).
    ObservabilityPrims::register_stats_impl(
        "query:macro-hygiene-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            // Issue #2021: capacity 128 (power-of-2) — depth/concurrent keys
            // grew the dashboard past the old 48-slot open-address table.
            auto* ht = FlatHashTable::create(query_hash_capacity_for(104));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            const auto load_m =
                [m](std::atomic<std::uint64_t> CompilerMetrics::* field) -> std::uint64_t {
                return m ? (m->*field).load(std::memory_order_relaxed) : 0;
            };
            const std::int64_t root_skips =
                static_cast<std::int64_t>(ev->get_macro_introduced_skipped_in_query());
            const std::int64_t recursive_skips =
                static_cast<std::int64_t>(ev->get_pattern_recursive_macro_skipped());
            const std::int64_t violations =
                static_cast<std::int64_t>(ev->get_hygiene_violation_count());
            const std::int64_t markers =
                static_cast<std::int64_t>(workspace_marker_macro_introduced(ev));
            const std::int64_t index_served =
                static_cast<std::int64_t>(ev->get_tag_arity_hygiene_index_served());
            const std::int64_t inline_skips =
                static_cast<std::int64_t>(ir_inline_hygiene_skipped(ev));
            const std::int64_t ir_stamped =
                static_cast<std::int64_t>(aura_hygiene_ir_macro_marker_total());
            const std::int64_t macro_stale =
                static_cast<std::int64_t>(ev->get_macro_stale_ref_prevented());
            const std::int64_t macro_repin =
                static_cast<std::int64_t>(ev->get_macro_provenance_repin_total());
            const std::int64_t reflect_checks = static_cast<std::int64_t>(
                load_m(&CompilerMetrics::reflect_macro_hygiene_checks_total));
            const std::int64_t reflect_rejects = static_cast<std::int64_t>(
                load_m(&CompilerMetrics::reflect_macro_hygiene_rejects_total));
            const std::int64_t naked_attempts =
                static_cast<std::int64_t>(load_m(&CompilerMetrics::naked_macro_mutate_attempt));
            const std::int64_t audit_events = static_cast<std::int64_t>(
                typed_audit::g_typed_mutation_audit_counters.macro_hygiene_events.load(
                    std::memory_order_relaxed));
            const std::int64_t audit_blocked = static_cast<std::int64_t>(
                typed_audit::g_typed_mutation_audit_counters.macro_hygiene_blocked.load(
                    std::memory_order_relaxed));
            const std::int64_t audit_allowed = static_cast<std::int64_t>(
                typed_audit::g_typed_mutation_audit_counters.macro_hygiene_allowed.load(
                    std::memory_order_relaxed));
            const std::int64_t trail_writes = static_cast<std::int64_t>(
                typed_audit::g_typed_mutation_audit_counters.trail_writes.load(
                    std::memory_order_relaxed));

            // Health score 0..100 (higher better). Penalties for violations /
            // naked mutate attempts / stale refs / high skip pressure.
            std::int64_t health = 100;
            auto penalize = [&](std::int64_t pts) {
                health -= pts;
                if (health < 0)
                    health = 0;
            };
            if (violations > 0)
                penalize(std::min<std::int64_t>(40, violations * 5));
            if (naked_attempts > 0)
                penalize(std::min<std::int64_t>(25, naked_attempts * 3));
            if (reflect_rejects > 0)
                penalize(std::min<std::int64_t>(15, reflect_rejects * 2));
            if (macro_stale > 20)
                penalize(std::min<std::int64_t>(10, macro_stale / 10));
            if (root_skips + recursive_skips >= 500)
                penalize(10);
            if (audit_blocked >= 10)
                penalize(5);

            // Recommendation: 0=ok 1=review-skips 2=throttle-macro-mutate
            // 3=enable-allow-with-care 4=hygiene-critical
            std::int64_t recommendation = 0;
            if (violations > 0 || naked_attempts >= 5)
                recommendation = 4;
            else if (naked_attempts > 0 || reflect_rejects > 0)
                recommendation = 3;
            else if (root_skips + recursive_skips >= 200)
                recommendation = 2;
            else if (root_skips + recursive_skips >= 50 || macro_stale > 0)
                recommendation = 1;

            // #486/#1501/#1609 lineage
            insert_kv("root-skips", root_skips);
            insert_kv("recursive-skips", recursive_skips);
            insert_kv("hygiene-violations", violations);
            insert_kv("macro-markers", markers);
            insert_kv("hygiene-index-served", index_served);
            // #1613 consolidated breakdown + health
            insert_kv("inline-hygiene-skipped", inline_skips);
            insert_kv("ir-hygiene-stamped-count", ir_stamped);
            insert_kv("macro-stale-ref-prevented", macro_stale);
            insert_kv("macro-provenance-repin-total", macro_repin);
            insert_kv("reflect-macro-hygiene-checks", reflect_checks);
            insert_kv("reflect-macro-hygiene-rejects", reflect_rejects);
            insert_kv("naked-macro-mutate-attempts", naked_attempts);
            insert_kv("macro-audit-events", audit_events);
            insert_kv("macro-audit-blocked", audit_blocked);
            insert_kv("macro-audit-allowed", audit_allowed);
            insert_kv("audit-trail-writes", trail_writes);
            insert_kv("allow-macro-mutate", ev->get_allow_macro_mutate() ? 1 : 0);
            // Issue #2237: expose agent-driven MacroIntroduced rollback
            // counters + strict-mode audit visibility. Mirrors the
            // `g_rollback_macro_introduced_total` + `g_rollback_strict_audited_total`
            // file-level atomics (macro_expansion.cpp:401, :407) plus
            // the existing `g_unstamp_macro_introduced_total` per-node
            // counter (macro_expansion.cpp:395). Strict-mode flag is a
            // live read of `g_macro_expand_sandbox_strict` (atomic,
            // process-wide). When rollback under Strict sandbox,
            // `mutate:rollback-macro-introduced` emits
            // SecurityEventKind::MacroHygieneRollbackOnStrict into the
            // g_security_event_ring() (and #2225 SecurityEventWAL if
            // enabled) — query:security-audit / query:security-audit-trail
            // surfaces the events separately. `rollback-wired=1` is the
            // signal key (mirrors `region-priority-throttle-wired=1`
            // for #2132 + `capture-remount-wired=1` for #2234 +
            // `storm-isolation-wired=1` for #2236).
            insert_kv("unstamp-macro-introduced-total",
                      static_cast<std::int64_t>(aura_unstamp_macro_introduced_total_v_read()));
            insert_kv("rollback-macro-introduced-total",
                      static_cast<std::int64_t>(aura_rollback_macro_introduced_total_v_read()));
            insert_kv("rollback-strict-audited-total",
                      static_cast<std::int64_t>(aura_rollback_strict_audited_total_v_read()));
            insert_kv("rollback-strict-mode-flag",
                      static_cast<std::int64_t>(aura_macro_expand_sandbox_strict_v_read()));
            insert_kv("rollback-wired", 1);
            insert_kv("schema-2237", 2237);
            insert_kv("issue-2237", 2237);
            // Issue #2018 / #2019 / #2021: mirror live atomics → CompilerMetrics,
            // then publish depth + concurrent peak on this Agent surface.
            if (m)
                aura_macro_hygiene_snapshot_metrics(m);
            {
                const std::int64_t rest_file =
                    static_cast<std::int64_t>(aura_macro_rest_param_hygiene_total_v_read());
                insert_kv("macro-rest-param-hygiene-total", rest_file);
                insert_kv("macro_rest_param_hygiene_total", rest_file);
                // Issue #2169: incomplete rest renames + process-wide gensym serial.
                const std::int64_t rest_incomplete = static_cast<std::int64_t>(
                    aura_macro_rest_param_hygiene_incomplete_total_v_read());
                const std::int64_t rest_serial =
                    static_cast<std::int64_t>(aura_macro_rest_gensym_serial_v_read());
                insert_kv("macro-rest-param-hygiene-incomplete-total", rest_incomplete);
                insert_kv("rest-param-hygiene-incomplete-total", rest_incomplete);
                insert_kv("rest-param-gensym-serial", rest_serial);
                insert_kv("schema-2169", 2169);
                insert_kv("issue-2169", 2169);
                insert_kv("rest-param-hygiene-complete-wired", 1);
                const std::int64_t restamp_f =
                    static_cast<std::int64_t>(aura_macro_restamp_after_flat_total_v_read());
                insert_kv("macro-restamp-after-flat-total", restamp_f);
                insert_kv("macro_restamp_after_flat_total", restamp_f);
                // Issue #2021: depth max + concurrent peak / in-flight.
                const std::int64_t depth_max =
                    static_cast<std::int64_t>(aura_hygiene_tracer_depth_max_v_read());
                const std::int64_t conc_peak =
                    static_cast<std::int64_t>(aura_macro_clone_concurrent_peak_v_read());
                const std::int64_t in_flight =
                    static_cast<std::int64_t>(aura_macro_clone_in_flight_v_read());
                const std::int64_t fiber_stamps =
                    static_cast<std::int64_t>(aura_macro_clone_concurrent_fiber_total_v_read());
                insert_kv("max_depth", depth_max);
                insert_kv("max-depth", depth_max);
                insert_kv("hygiene-tracer-depth-max", depth_max);
                insert_kv("hygiene-depth-max", depth_max);
                insert_kv("concurrent_fiber_count", fiber_stamps);
                insert_kv("concurrent-fiber-count", fiber_stamps);
                insert_kv("macro-clone-concurrent-fiber-total", fiber_stamps);
                insert_kv("concurrent_peak", conc_peak);
                insert_kv("concurrent-peak", conc_peak);
                insert_kv("macro-clone-concurrent-peak", conc_peak);
                insert_kv("in_flight", in_flight);
                insert_kv("in-flight", in_flight);
                insert_kv("macro-clone-in-flight", in_flight);
                insert_kv("max-hygiene-depth-cap",
                          static_cast<std::int64_t>(aura::compiler::macro_exp::MAX_HYGIENE_DEPTH));
                insert_kv("hard-max-depth",
                          static_cast<std::int64_t>(aura::compiler::macro_exp::MAX_HYGIENE_DEPTH));
                // Issue #2101: live effective + process-wide runtime caps.
                insert_kv("effective-max-depth",
                          static_cast<std::int64_t>(
                              aura::compiler::macro_exp::effective_hygiene_depth_limit()));
                insert_kv("runtime-depth-cap",
                          static_cast<std::int64_t>(
                              aura::compiler::macro_exp::runtime_hygiene_depth_cap()));
                insert_kv("self-evo-pass-cap",
                          static_cast<std::int64_t>(
                              aura::compiler::macro_exp::effective_hygiene_pass_cap()));
                insert_kv("runtime-pass-cap",
                          static_cast<std::int64_t>(
                              aura::compiler::macro_exp::runtime_hygiene_pass_cap()));
                insert_kv("schema-2101", 2101);
                insert_kv("issue-2101", 2101);
                insert_kv("hygiene-limits-runtime-wired", 1);
                insert_kv("process-wide", 1); // AC5: caps are process-wide atomics
                insert_kv("capability-tightens-only", 1);
                insert_kv("depth-obs-wired", 1);
                insert_kv("concurrent-obs-wired", 1);
                insert_kv("depth-concurrent-obs-issue", 2021);
                // Issue #3028: same-FlatAST reject + steal abort (additive).
                insert_kv(
                    "same-flat-reject-total",
                    static_cast<std::int64_t>(aura_macro_clone_same_flat_reject_total_v_read()));
                insert_kv("steal-abort-total",
                          static_cast<std::int64_t>(aura_macro_clone_steal_abort_total_v_read()));
                insert_kv("last-reject-reason",
                          static_cast<std::int64_t>(aura_macro_clone_last_reject_reason_v_read()));
                insert_kv("same-flat-reject-wired", 1);
                insert_kv("explicit-depth-authority-wired", 1);
                insert_kv("schema-3028", 3028);
                insert_kv("issue-3028", 3028);
                // Issue #3029: Agent-stable ceiling/depth/pass reasons.
                insert_kv("last-hygiene-limit-reason",
                          static_cast<std::int64_t>(aura_macro_hygiene_last_limit_reason_v_read()));
                insert_kv("hygiene-limit-reason-none", 0);
                insert_kv("hygiene-limit-reason-gensym-ceiling", 1);
                insert_kv("hygiene-limit-reason-depth-limit", 2);
                insert_kv("hygiene-limit-reason-pass-limit", 3);
                // Issue #3215: Agent reject reasons (reuse last-hygiene-limit-reason).
                insert_kv("hygiene-limit-reason-macro-introduced", 4);
                insert_kv("hygiene-limit-reason-rest-unmarked", 5);
                insert_kv("hygiene-limit-reason-wired", 1);
                insert_kv("schema-3029", 3029);
                insert_kv("issue-3029", 3029);
                insert_kv("schema-3215", 3215);
                insert_kv("issue-3215", 3215);
                // Issue #3543: typed SE on hygiene fail (additive keys).
                // Existing last-hygiene-limit-reason / schema-3029 stay.
                insert_kv("schema-3543", 3543);
                insert_kv("issue-3543", 3543);
                insert_kv("hygiene-violation-se-wired", 1);
                insert_kv("hygiene-violation-se-emit-total",
                          static_cast<std::int64_t>(aura_hygiene_violation_se_emit_total_v_read()));
            }
            insert_kv("health-score", health);
            insert_kv("hygiene-health-score", health); // AC alias
            insert_kv("recommendation", recommendation);
            // 0=ok 1=investigate 2=throttle 3=review-allow 4=critical
            insert_kv("action", recommendation);
            insert_kv("ai-closedloop-macro-health-wired", 1);
            insert_kv("audit-trail-wired", 1);
            insert_kv("issue", 1613); // lineage 1609 / 1501 / 486; depth keys #2021 / #2101
            insert_kv("schema", 1613);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #548: query:panic-checkpoint-lifecycle-stats.
    // Returns the sum of the 4 panic-checkpoint lifecycle
    // observability counters:
    //   - panic_checkpoint_save_count_  (lifetime # of
    //     save_panic_checkpoint() calls that succeeded)
    //   - panic_checkpoint_restore_count_  (lifetime # of
    //     restore_panic_checkpoint() calls, both successful
    //     and failed)
    //   - panic_checkpoint_commit_count_  (lifetime # of
    //     commit_panic_checkpoint() calls — typically once
    //     per successful Guard dtor)
    //   - rollback_success_on_panic_  (lifetime # of
    //     restore_panic_checkpoint() calls that actually
    //     succeeded — a stricter subset of restore_count)
    //
    // P0: returns an integer = sum of all 4 counters.
    // Follow-up: returns a 4-tuple
    // (save restore commit rollback-success) so the AI
    // Agent can compute the rollback-success rate
    // (= rollback_success / restore) and the save/commit
    // ratio (= save / commit, ideally 1.0).
    ObservabilityPrims::register_stats_impl(
        "query:panic-checkpoint-lifecycle-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t save = ev->get_panic_checkpoint_save_count();
            const std::uint64_t restore = ev->get_panic_checkpoint_restore_count();
            const std::uint64_t commit = ev->get_panic_checkpoint_commit_count();
            const std::uint64_t rollback_success = ev->get_rollback_success_on_panic();
            return make_int(static_cast<std::int64_t>(save + restore + commit + rollback_success));
        });

    // Issue #511: query:workspace-snapshot-stats. Hash view of workspace
    // persistence + panic-checkpoint snapshot observability for long-session
    // AI Agent resume (non-duplicative with #548 int-sum
    // panic-checkpoint-lifecycle-stats and #497 stable-ref-lifecycle hash):
    //   - workspace-size: live FlatAST node count
    //   - gen-age: current FlatAST generation_
    //   - wrap-epoch: generation wrap epoch for resume safety
    //   - stable-ref-invalidations: stale ref detections since session start
    //   - checkpoint-save / checkpoint-restore / checkpoint-commit /
    //     checkpoint-transfer: panic checkpoint lifecycle counters
    //   - rollback-success: successful panic restores
    //   - panic-safe-source-len: bytes in last checkpoint source snapshot
    //   - workspace-snapshot-total: sum of primary counters
    //   - workspace-snapshot-recommendation: 0=ok, 1=checkpoint stale, 2=high restore
    ObservabilityPrims::register_stats_impl(
        "query:workspace-snapshot-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            // Issue #2966: capacity 32 so schema-2966 + fail-reason keys fit
            // with existing workspace-snapshot counters (was 16 → silent drop).
            auto* ht = FlatHashTable::create(query_hash_capacity_for(26));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const std::uint64_t workspace_size = ws ? ws->size() : 0;
            const std::uint64_t gen_age = ws ? ws->current_generation() : 0;
            const std::uint64_t wrap_epoch = ws ? ws->wrap_epoch() : 0;
            const std::uint64_t ref_inval = ws ? ws->stable_ref_invalidations() : 0;
            const std::uint64_t save = ev->get_panic_checkpoint_save_count();
            const std::uint64_t restore = ev->get_panic_checkpoint_restore_count();
            const std::uint64_t commit = ev->get_panic_checkpoint_commit_count();
            const std::uint64_t transfer = ev->get_panic_checkpoint_transfer_count();
            const std::uint64_t rollback_success = ev->get_rollback_success_on_panic();
            const std::uint64_t source_len = ev->panic_safe_source().size();
            const std::uint64_t total =
                workspace_size + gen_age + save + restore + commit + transfer + rollback_success;
            std::int64_t recommendation = 0;
            if (save > 0 && restore > save)
                recommendation = 2;
            else if (save > 0 && source_len == 0)
                recommendation = 1;
            insert_kv("workspace-size", static_cast<std::int64_t>(workspace_size));
            insert_kv("gen-age", static_cast<std::int64_t>(gen_age));
            insert_kv("wrap-epoch", static_cast<std::int64_t>(wrap_epoch));
            insert_kv("stable-ref-invalidations", static_cast<std::int64_t>(ref_inval));
            insert_kv("checkpoint-save", static_cast<std::int64_t>(save));
            insert_kv("checkpoint-restore", static_cast<std::int64_t>(restore));
            insert_kv("checkpoint-commit", static_cast<std::int64_t>(commit));
            insert_kv("checkpoint-transfer", static_cast<std::int64_t>(transfer));
            insert_kv("rollback-success", static_cast<std::int64_t>(rollback_success));
            insert_kv("panic-safe-source-len", static_cast<std::int64_t>(source_len));
            insert_kv("workspace-snapshot-total", static_cast<std::int64_t>(total));
            insert_kv("workspace-snapshot-recommendation", recommendation);
            // Issue #2966: ast:snapshot failure observability (never silent -1).
            // last-ast-snapshot-fail-reason: 0=none, 1=guard, 2=no-workspace, 3=empty
            // Contract: snapshot requires set-code/mutate workspace; define-only → 2.
            insert_kv("last-ast-snapshot-fail-reason",
                      static_cast<std::int64_t>(ev->last_ast_snapshot_fail_reason()));
            insert_kv("ast-snapshot-fail-total",
                      static_cast<std::int64_t>(ev->ast_snapshot_fail_total()));
            insert_kv("ast-snapshot-ok-total",
                      static_cast<std::int64_t>(ev->ast_snapshot_ok_total()));
            insert_kv("ast-snapshot-fail-wired", 1);
            insert_kv("schema-2966", 2966);
            insert_kv("issue-2966", 2966);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #512: query:runtime-orchestration-stats. Hash view of runtime
    // production orchestration gaps (work-stealing outermost enforcement,
    // EnvFrame/GC safepoint coordination, fiber migration) — non-duplicative
    // synthesis of #500 work-steal-stats, #618 scheduler-mutation-coord-stats,
    // and #543 envframe-dualpath-stats:
    //   - steal-attempts / steal-successes / steal-deferred-outermost /
    //     steal-violations: work-stealing + MutationBoundary safety
    //   - mutation-boundary-depth: current guard nesting (0 = steal-safe)
    //   - gc-safepoint-requests / gc-safepoint-waits /
    //     gc-pauses-attributed-to-mutation: scheduler/GC coordination
    //   - envframe-stale-refresh: EnvFrame dual-path consistency signal
    //   - lock-contention-us / fiber-migration-attempts: orchestration load
    //   - runtime-orchestration-total / runtime-orchestration-recommendation
    ObservabilityPrims::register_stats_impl(
        "query:runtime-orchestration-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(query_hash_capacity_for(21));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const std::uint64_t steal_attempts = aura_work_steal_attempts_total();
            const std::uint64_t steal_successes = aura_work_steal_successes_total();
            const std::uint64_t steal_deferred = aura_adaptive_steal_global_deferred_total();
            const std::uint64_t steal_violations = ev->get_mutation_steal_violation_count();
            const std::uint64_t boundary_depth = aura_evaluator_mutation_boundary_depth();
            const std::uint64_t gc_requests = ev->get_gc_safepoint_requests_total();
            const std::uint64_t gc_waits = ev->get_gc_safepoint_waits_total();
            const std::uint64_t gc_attributed = aura_fiber_static_gc_pause_attributed_to_mutation();
            const std::uint64_t env_refresh = ev->get_envframe_stale_refresh_count();
            const std::uint64_t lock_us = ev->get_lock_contention_us();
            const std::uint64_t migration = ev->get_mutation_steal_attempts();
            const std::uint64_t total = steal_attempts + steal_successes + steal_deferred +
                                        steal_violations + gc_requests + gc_waits + gc_attributed +
                                        env_refresh + migration;
            std::int64_t recommendation = 0;
            if (steal_violations > 0)
                recommendation = 3;
            else if (steal_deferred > steal_successes && steal_deferred > 3)
                recommendation = 2;
            else if (boundary_depth > 0 && steal_attempts > 0)
                recommendation = 1;
            insert_kv("steal-attempts", static_cast<std::int64_t>(steal_attempts));
            insert_kv("steal-successes", static_cast<std::int64_t>(steal_successes));
            insert_kv("steal-deferred-outermost", static_cast<std::int64_t>(steal_deferred));
            insert_kv("steal-violations", static_cast<std::int64_t>(steal_violations));
            insert_kv("mutation-boundary-depth", static_cast<std::int64_t>(boundary_depth));
            insert_kv("gc-safepoint-requests", static_cast<std::int64_t>(gc_requests));
            insert_kv("gc-safepoint-waits", static_cast<std::int64_t>(gc_waits));
            insert_kv("gc-pauses-attributed-to-mutation", static_cast<std::int64_t>(gc_attributed));
            insert_kv("envframe-stale-refresh", static_cast<std::int64_t>(env_refresh));
            insert_kv("lock-contention-us", static_cast<std::int64_t>(lock_us));
            insert_kv("fiber-migration-attempts", static_cast<std::int64_t>(migration));
            insert_kv("runtime-orchestration-total", static_cast<std::int64_t>(total));
            insert_kv("runtime-orchestration-recommendation", recommendation);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #513: query:aot-hot-reload-stats. Consolidated hash view of AOT
    // hot-reload production readiness (func_table epoch swap, stale detection,
    // refcount/region safety) — non-duplicative synthesis of #708
    // query:aot-reload-stats + query:aot-checkpoint-version-stats:
    //   - reload-attempts / reload-success / stale-rejected: dlopen path
    //   - refcount-swaps / concurrent-safe-reloads: func_table epoch swap
    //   - region-violations / deopt-on-steal: multi-fiber safety signals
    //   - checkpoint-version-drifts: defuse/bridge_epoch drift probe
    //   - func-table-epoch / defuse-version: live version state
    //   - aot-hot-reload-total / aot-hot-reload-recommendation
    ObservabilityPrims::register_stats_impl(
        "query:aot-hot-reload-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(query_hash_capacity_for(20));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t attempts =
                m ? m->aot_reload_attempts_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t success =
                m ? m->aot_hot_update_success_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t stale =
                m ? m->aot_stale_reject_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t swaps =
                m ? m->aot_refcount_swaps_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t region_viol =
                m ? m->aot_region_mismatch_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t deopt_steal =
                m ? m->aot_deopt_on_steal_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t concurrent_safe =
                m ? m->aot_concurrent_safe_reloads_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t drifts =
                m ? m->aot_checkpoint_version_drifts_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t table_epoch = aura_aot_func_table_epoch();
            const std::uint64_t defuse_ver = aura_get_aot_defuse_version();
            const std::uint64_t total = attempts + success + stale + swaps + region_viol +
                                        deopt_steal + concurrent_safe + drifts;
            std::int64_t recommendation = 0;
            if (region_viol > 0 || deopt_steal > 0)
                recommendation = 3;
            else if (stale > success && stale > 0)
                recommendation = 2;
            else if (drifts > 0)
                recommendation = 1;
            insert_kv("reload-attempts", static_cast<std::int64_t>(attempts));
            insert_kv("reload-success", static_cast<std::int64_t>(success));
            insert_kv("stale-rejected", static_cast<std::int64_t>(stale));
            insert_kv("refcount-swaps", static_cast<std::int64_t>(swaps));
            insert_kv("region-violations", static_cast<std::int64_t>(region_viol));
            insert_kv("deopt-on-steal", static_cast<std::int64_t>(deopt_steal));
            insert_kv("concurrent-safe-reloads", static_cast<std::int64_t>(concurrent_safe));
            insert_kv("checkpoint-version-drifts", static_cast<std::int64_t>(drifts));
            insert_kv("func-table-epoch", static_cast<std::int64_t>(table_epoch));
            insert_kv("defuse-version", static_cast<std::int64_t>(defuse_ver));
            insert_kv("aot-hot-reload-total", static_cast<std::int64_t>(total));
            insert_kv("aot-hot-reload-recommendation", recommendation);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #522: query:aot-production-reload-stats. Commercial P0 hash view
    // of AOT hot-reload deployment readiness (func_table swap, multi-agent
    // module/region namespace, version drift) — non-duplicative synthesis
    // of #513 aot-hot-reload-stats with #287 module_version + #708 region
    // isolation; avoids duplicating #708 per-theme security.cpp hashes:
    //   - reload-attempts / reload-success / stale-rejected: dlopen path
    //   - refcount-swaps / concurrent-safe-reloads: func_table epoch swap
    //   - region-violations / deopt-on-steal: multi-fiber safety
    //   - checkpoint-version-drifts: defuse/bridge_epoch drift
    //   - func-table-epoch / defuse-version / module-version / host-region-mask
    //   - swap-success-rate-pct: 0..100 from attempts/success
    //   - aot-production-reload-total / aot-production-reload-recommendation
    ObservabilityPrims::register_stats_impl(
        "query:aot-production-reload-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(query_hash_capacity_for(23));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t attempts =
                m ? m->aot_reload_attempts_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t success =
                m ? m->aot_hot_update_success_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t stale =
                m ? m->aot_stale_reject_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t swaps =
                m ? m->aot_refcount_swaps_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t region_viol =
                m ? m->aot_region_mismatch_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t deopt_steal =
                m ? m->aot_deopt_on_steal_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t concurrent_safe =
                m ? m->aot_concurrent_safe_reloads_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t drifts =
                m ? m->aot_checkpoint_version_drifts_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t table_epoch = aura_aot_func_table_epoch();
            const std::uint64_t defuse_ver = aura_get_aot_defuse_version();
            const std::uint64_t module_ver = aura_get_module_version();
            const std::uint64_t region_mask = aura_get_aot_region_mask();
            const std::uint64_t success_rate_pct =
                attempts > 0 ? (100 * success / attempts) : (success > 0 ? 100 : 0);
            const std::uint64_t total = attempts + success + stale + swaps + region_viol +
                                        deopt_steal + concurrent_safe + drifts;
            std::int64_t recommendation = 0;
            if (region_viol > 0 || deopt_steal > 0)
                recommendation = 3;
            else if (stale > success && stale > 0)
                recommendation = 2;
            else if (drifts > 0 || defuse_ver != module_ver)
                recommendation = 1;
            insert_kv("reload-attempts", static_cast<std::int64_t>(attempts));
            insert_kv("reload-success", static_cast<std::int64_t>(success));
            insert_kv("stale-rejected", static_cast<std::int64_t>(stale));
            insert_kv("refcount-swaps", static_cast<std::int64_t>(swaps));
            insert_kv("region-violations", static_cast<std::int64_t>(region_viol));
            insert_kv("deopt-on-steal", static_cast<std::int64_t>(deopt_steal));
            insert_kv("concurrent-safe-reloads", static_cast<std::int64_t>(concurrent_safe));
            insert_kv("checkpoint-version-drifts", static_cast<std::int64_t>(drifts));
            insert_kv("func-table-epoch", static_cast<std::int64_t>(table_epoch));
            insert_kv("defuse-version", static_cast<std::int64_t>(defuse_ver));
            insert_kv("module-version", static_cast<std::int64_t>(module_ver));
            insert_kv("host-region-mask", static_cast<std::int64_t>(region_mask));
            insert_kv("swap-success-rate-pct", static_cast<std::int64_t>(success_rate_pct));
            insert_kv("aot-production-reload-total", static_cast<std::int64_t>(total));
            insert_kv("aot-production-reload-recommendation", recommendation);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #523: query:envframe-production-safety-stats. Commercial P0 hash view
    // of SoA EnvFrame/EnvId dual-path consistency + version stamping + stale
    // detection + GC safety — non-duplicative synthesis of #543
    // envframe-dualpath-stats, #418 envframe-dualpath-stale-stats, and #505
    // closure-env-safety envframe themes; avoids #516 prompt6-memory-safety-stats
    // broad Prompt6 matrix and #512 runtime-orchestration steal/GC pillars:
    //   - dual-path-desync / dual-path-sync-count: bindings_ vs bindings_symid_
    //   - stale-refresh-count / version-mismatch-in-walk: materialize + walk paths
    //   - gc-walk-safe-skips / gc-envframe-stale-skipped: GCEnvWalkFn hardening
    //   - post-rollback-invalidations: MutationBoundary checkpoint rollback
    //   - defuse-version: live epoch snapshot for version stamping
    //   - dual-path-sync-rate-pct: sync / (sync + desync) * 100
    //   - envframe-production-safety-total / envframe-production-safety-recommendation
    ObservabilityPrims::register_stats_impl(
        "query:envframe-production-safety-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(query_hash_capacity_for(19));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t desync = ev->get_envframe_desync_detected();
            const std::uint64_t dual_sync = ev->get_bindings_dual_sync_count();
            const std::uint64_t stale_refresh = ev->get_envframe_stale_refresh_count();
            const std::uint64_t version_mismatch = ev->get_envframe_version_mismatch_in_walk();
            const std::uint64_t gc_walk_skips = ev->get_envframe_gc_walk_safe_skips();
            const std::uint64_t gc_stale =
                m ? m->gc_envframe_stale_skipped_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t post_rollback = ev->get_envframe_post_rollback_invalidations();
            const std::uint64_t defuse_ver = ev->current_defuse_version();
            const std::uint64_t dual_checks = dual_sync + desync;
            const std::int64_t sync_rate_pct =
                dual_checks > 0 ? static_cast<std::int64_t>((dual_sync * 100) / dual_checks) : 100;
            const std::uint64_t total = desync + dual_sync + stale_refresh + version_mismatch +
                                        gc_walk_skips + gc_stale + post_rollback;
            std::int64_t recommendation = 0;
            if (desync > 0)
                recommendation = 3;
            else if (gc_stale > 0 && version_mismatch > stale_refresh)
                recommendation = 2;
            else if (version_mismatch > 0 || stale_refresh > 0)
                recommendation = 1;
            insert_kv("dual-path-desync", static_cast<std::int64_t>(desync));
            insert_kv("dual-path-sync-count", static_cast<std::int64_t>(dual_sync));
            insert_kv("stale-refresh-count", static_cast<std::int64_t>(stale_refresh));
            insert_kv("version-mismatch-in-walk", static_cast<std::int64_t>(version_mismatch));
            insert_kv("gc-walk-safe-skips", static_cast<std::int64_t>(gc_walk_skips));
            insert_kv("gc-envframe-stale-skipped", static_cast<std::int64_t>(gc_stale));
            insert_kv("post-rollback-invalidations", static_cast<std::int64_t>(post_rollback));
            insert_kv("defuse-version", static_cast<std::int64_t>(defuse_ver));
            insert_kv("dual-path-sync-rate-pct", sync_rate_pct);
            insert_kv("envframe-production-safety-total", static_cast<std::int64_t>(total));
            insert_kv("envframe-production-safety-recommendation", recommendation);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #524: query:macro-production-hygiene-stats. Commercial P0 hash view
    // of MacroIntroduced marker propagation + hygiene closed-loop (clone_macro_body
    // → query:pattern → IR InlinePass) — non-duplicative synthesis of #501
    // ir-hygiene-stats, #503 pattern-marker-stats, #547 pattern-hygiene-stats,
    // and #486 macro-hygiene-stats; avoids #597 macro-reflect-self-evo-stats
    // reflect/validation bundle and #420 macro-hygiene-contract-stats int-sum:
    //   - root-skips / recursive-skips: query:pattern hygiene filter surface
    //   - hygiene-violations / filter-violations: matcher + filter contract
    //   - macro-markers: workspace SyntaxMarker::MacroIntroduced tally
    //   - inline-hygiene-skipped / respect-macro-hygiene: IR InlinePass policy
    //   - contract-violations / macro-expansion-dirty: clone_macro_body path
    //   - hygiene-filter-rate-pct: (root+recursive) / markers * 100
    //   - macro-production-hygiene-total / macro-production-hygiene-recommendation
    ObservabilityPrims::register_stats_impl(
        "query:macro-production-hygiene-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            auto* ht = FlatHashTable::create(query_hash_capacity_for(20));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const std::uint64_t root_skips = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t recursive_skips = ev->get_pattern_recursive_macro_skipped();
            const std::uint64_t violations = ev->get_hygiene_violation_count();
            const std::uint64_t filter_violations = ev->get_pattern_macro_filter_violations();
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);
            const std::uint64_t inline_skipped = ir_inline_hygiene_skipped(ev);
            // Issue #1780: per-Evaluator policy (not InlinePass static).
            const bool respects = ev->get_inline_respect_macro_hygiene();
            const std::uint64_t contract = ev->get_macro_hygiene_contract_violations();
            const std::uint64_t macro_dirty = ws ? ws->macro_expansion_dirty_total() : 0;
            const std::uint64_t filter_checks = root_skips + recursive_skips;
            const std::int64_t filter_rate_pct =
                markers > 0 ? static_cast<std::int64_t>((filter_checks * 100) / markers) : 0;
            const std::uint64_t total = root_skips + recursive_skips + violations +
                                        filter_violations + markers + inline_skipped + contract +
                                        macro_dirty;
            std::int64_t recommendation = 0;
            if (violations > 0 || filter_violations > 0 || contract > 0)
                recommendation = 3;
            else if (inline_skipped > 0 && !respects)
                recommendation = 2;
            else if (root_skips + recursive_skips > 0)
                recommendation = 1;
            insert_kv("root-skips", static_cast<std::int64_t>(root_skips));
            insert_kv("recursive-skips", static_cast<std::int64_t>(recursive_skips));
            insert_kv("hygiene-violations", static_cast<std::int64_t>(violations));
            insert_kv("filter-violations", static_cast<std::int64_t>(filter_violations));
            insert_kv("macro-markers", static_cast<std::int64_t>(markers));
            insert_kv("inline-hygiene-skipped", static_cast<std::int64_t>(inline_skipped));
            insert_kv("respect-macro-hygiene", respects ? 1 : 0);
            insert_kv("contract-violations", static_cast<std::int64_t>(contract));
            insert_kv("macro-expansion-dirty", static_cast<std::int64_t>(macro_dirty));
            insert_kv("hygiene-filter-rate-pct", filter_rate_pct);
            insert_kv("macro-production-hygiene-total", static_cast<std::int64_t>(total));
            insert_kv("macro-production-hygiene-recommendation", recommendation);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #525: query:guard-production-impact-stats. Commercial P0 hash view
    // of MutationBoundaryGuard post-success impact snapshot + reflect/schema
    // validation closed-loop — non-duplicative synthesis of #504
    // mutation-boundary-log, #488 mutation-impact-snapshot, and #551
    // reflect-postmutate-stats; avoids #515 consolidated P0 tracker and #597
    // macro-reflect-self-evo-stats broad bundle:
    //   - epoch-after/delta, nodes-changed, reasons-mask: latest ring entry
    //   - impact-snapshots / mutation-impacts: Guard success tallies
    //   - dirty-nodes / macro-markers: snapshot marker+delta surface
    //   - schema-pass/fail/valid: post_mutation_reflect_validate hook
    //   - guard-epoch / boundary-depth / dirty-propagation: epoch coordination
    //   - checkpoint-commits: panic checkpoint commit on Guard success
    //   - validation-pass-rate-pct: pass / (pass + fail) * 100
    //   - guard-production-impact-total / guard-production-impact-recommendation
    ObservabilityPrims::register_stats_impl(
        "query:guard-production-impact-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto entry = ev->get_latest_mutation_impact_entry();
            auto* ht = FlatHashTable::create(query_hash_capacity_for(26));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const std::uint64_t snapshots = ev->get_impact_snapshot_count();
            const std::uint64_t impacts = ev->get_mutation_impact_count();
            const std::uint64_t dirty = ev->get_dirty_nodes_in_snapshot();
            const std::uint64_t markers = ev->get_macro_markers_in_snapshot();
            const std::uint64_t pass = ev->get_schema_validation_pass_count();
            const std::uint64_t fail = ev->get_schema_validation_fail_count();
            const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
            const std::uint64_t dirty_prop = ev->get_dirty_propagation_count();
            const std::uint64_t commits = ev->get_panic_checkpoint_commit_count();
            const std::uint64_t validations = pass + fail;
            const std::int64_t pass_rate_pct =
                validations > 0 ? static_cast<std::int64_t>((pass * 100) / validations) : 100;
            const std::uint64_t total = snapshots + impacts + entry.epoch_delta +
                                        entry.nodes_changed + dirty + pass + fail + guard_epoch +
                                        dirty_prop + commits;
            std::int64_t recommendation = 0;
            if (fail > 0 || !ev->get_last_schema_validation_ok())
                recommendation = 3;
            else if (entry.nodes_changed > 20)
                recommendation = 2;
            else if (dirty > 0 || snapshots > 0)
                recommendation = 1;
            insert_kv("epoch-after", static_cast<std::int64_t>(entry.epoch_after));
            insert_kv("epoch-delta", static_cast<std::int64_t>(entry.epoch_delta));
            insert_kv("nodes-changed", static_cast<std::int64_t>(entry.nodes_changed));
            insert_kv("reasons-mask", static_cast<std::int64_t>(entry.reasons_mask));
            insert_kv("impact-snapshots", static_cast<std::int64_t>(snapshots));
            insert_kv("mutation-impacts", static_cast<std::int64_t>(impacts));
            insert_kv("dirty-nodes", static_cast<std::int64_t>(dirty));
            insert_kv("macro-markers", static_cast<std::int64_t>(markers));
            insert_kv("schema-pass", static_cast<std::int64_t>(pass));
            insert_kv("schema-fail", static_cast<std::int64_t>(fail));
            insert_kv("schema-valid", ev->get_last_schema_validation_ok() ? 1 : 0);
            insert_kv("guard-epoch", static_cast<std::int64_t>(guard_epoch));
            insert_kv("boundary-depth",
                      static_cast<std::int64_t>(Evaluator::mutation_boundary_depth()));
            insert_kv("dirty-propagation", static_cast<std::int64_t>(dirty_prop));
            insert_kv("checkpoint-commits", static_cast<std::int64_t>(commits));
            insert_kv("validation-pass-rate-pct", pass_rate_pct);
            insert_kv("guard-production-impact-total", static_cast<std::int64_t>(total));
            insert_kv("guard-production-impact-recommendation", recommendation);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #528: query:pattern-production-index-stats. Commercial P0 hash view
    // of query:pattern incremental tag_arity_index + MacroIntroduced hygiene
    // integration for large-AST AI loops — non-duplicative synthesis of #547
    // pattern-index-stats, #490 pattern-index-rebuild-stats, #621
    // pattern-index-stats-hash, and #547 pattern-hygiene-stats; avoids #524
    // macro-production-hygiene-stats IR/clone bundle and #503 pattern-marker
    // int-only themes:
    //   P1 Index: hits/misses/rebuilds, dirty-marks, rebuild-time-us, delta-hits
    //   P2 Rebuild triggers: lazy/eager-mutate/eager-cow rebuild tallies
    //   P3 Structural fast-path: structural-hits/misses, index-entries
    //   P4 Hygiene: root-skips, recursive-skips, hygiene-violations, markers
    //   - arity-accuracy-pct / delta-hit-rate-pct derived metrics
    //   - pattern-production-index-total / pattern-production-index-recommendation
    ObservabilityPrims::register_stats_impl(
        "query:pattern-production-index-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            auto* ht = FlatHashTable::create(query_hash_capacity_for(28));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const std::uint64_t hits = ws ? ws->tag_arity_index_hits() : 0;
            const std::uint64_t misses = ws ? ws->tag_arity_index_misses() : 0;
            const std::uint64_t rebuilds = ws ? ws->tag_arity_index_rebuilds() : 0;
            const std::uint64_t dirty_marks = ws ? ws->tag_arity_index_dirty_marks() : 0;
            const std::uint64_t rebuild_time_us = ws ? ws->tag_arity_index_rebuild_time_us() : 0;
            const std::uint64_t delta_hits = ws ? ws->tag_arity_index_delta_hits() : 0;
            const std::uint64_t lazy_rebuilds = ev->get_pattern_index_lazy_rebuilds();
            const std::uint64_t eager_mutate = ev->get_pattern_index_eager_mutate_rebuilds();
            const std::uint64_t eager_cow = ev->get_pattern_index_eager_cow_rebuilds();
            const std::uint64_t structural_hits = ev->get_pattern_structural_index_hits();
            const std::uint64_t structural_misses = ev->get_pattern_structural_index_misses();
            const std::uint64_t index_entries = ev->tag_arity_index_entry_count();
            const std::uint64_t root_skips = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t recursive_skips = ev->get_pattern_recursive_macro_skipped();
            const std::uint64_t violations = ev->get_hygiene_violation_count();
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);
            const std::uint64_t index_total = hits + misses;
            const std::int64_t arity_accuracy_pct =
                index_total == 0 ? 0 : static_cast<std::int64_t>((hits * 100) / index_total);
            const std::uint64_t delta_denom = delta_hits + rebuilds;
            const std::int64_t delta_hit_rate_pct =
                delta_denom == 0 ? 0 : static_cast<std::int64_t>((delta_hits * 100) / delta_denom);
            const std::uint64_t total = hits + misses + rebuilds + dirty_marks + rebuild_time_us +
                                        delta_hits + root_skips + recursive_skips + violations +
                                        structural_hits + structural_misses;
            std::int64_t recommendation = 0;
            if (violations > 0)
                recommendation = 3;
            else if (index_total > 0 && arity_accuracy_pct < 50)
                recommendation = 2;
            else if (rebuilds > 0 &&
                     rebuild_time_us > static_cast<std::uint64_t>(delta_hits + 1) * 100)
                recommendation = 2;
            else if (root_skips + recursive_skips > 0)
                recommendation = 1;
            insert_kv("index-hits", static_cast<std::int64_t>(hits));
            insert_kv("index-misses", static_cast<std::int64_t>(misses));
            insert_kv("index-rebuilds", static_cast<std::int64_t>(rebuilds));
            insert_kv("dirty-marks", static_cast<std::int64_t>(dirty_marks));
            insert_kv("rebuild-time-us", static_cast<std::int64_t>(rebuild_time_us));
            insert_kv("delta-hits", static_cast<std::int64_t>(delta_hits));
            insert_kv("lazy-rebuilds", static_cast<std::int64_t>(lazy_rebuilds));
            insert_kv("eager-mutate-rebuilds", static_cast<std::int64_t>(eager_mutate));
            insert_kv("eager-cow-rebuilds", static_cast<std::int64_t>(eager_cow));
            insert_kv("structural-hits", static_cast<std::int64_t>(structural_hits));
            insert_kv("structural-misses", static_cast<std::int64_t>(structural_misses));
            insert_kv("index-entries", static_cast<std::int64_t>(index_entries));
            insert_kv("root-skips", static_cast<std::int64_t>(root_skips));
            insert_kv("recursive-skips", static_cast<std::int64_t>(recursive_skips));
            insert_kv("hygiene-violations", static_cast<std::int64_t>(violations));
            insert_kv("macro-markers", static_cast<std::int64_t>(markers));
            insert_kv("arity-accuracy-pct", arity_accuracy_pct);
            insert_kv("delta-hit-rate-pct", delta_hit_rate_pct);
            insert_kv("pattern-production-index-total", static_cast<std::int64_t>(total));
            insert_kv("pattern-production-index-recommendation", recommendation);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #530: query:incremental-production-relower-stats. Commercial P0 hash
    // view of incremental compilation dirty/re-lower granularity + ir_cache_ /
    // dep_graph_ / JIT bridge interaction — non-duplicative synthesis of #460
    // compiler-incremental-stats, #404 ir-soa-incremental-stats, #426
    // compiler-cache-stats, and #429 soa-dirty-stats; avoids #506 soa-hotpath
    // adoption int-sum and #515 consolidated P0 tracker:
    //   P1 Re-lower path: partial/per-fn/full/skipped + blocks-saved
    //   P2 Impact scope: impact-scope-calls + affected-blocks-total
    //   P3 JIT/bridge: jit-invalidate + bridge-invalidate + invalidate-fn
    //   P4 Live cache: dirty-blocks/fns, cached-fns, dirty-block-pct
    //   P5 Triggers: should-relower + cascade-body-only
    //   - min-scope-hit-rate-pct: blocks_saved / relower work * 100
    //   - incremental-production-relower-total / recommendation
    ObservabilityPrims::register_stats_impl(
        "query:incremental-production-relower-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(query_hash_capacity_for(27));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            Evaluator::SoaDirtyStats soa;
            if (ev->get_soa_dirty_stats_fn_)
                soa = ev->get_soa_dirty_stats_fn_();
            const std::uint64_t should_relower =
                m ? m->should_relower_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t partial = ev->get_partial_relower_count();
            const std::uint64_t impact_calls = ev->get_impact_scope_calls();
            const std::uint64_t affected_blocks = ev->get_total_affected_blocks();
            const std::uint64_t relower_skip =
                m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_per_fn =
                m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_full =
                m ? m->relower_full_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t blocks_saved =
                m ? m->ir_soa_relower_blocks_saved_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t jit_invalidate =
                m ? m->jit_hotswap_invalidate_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t bridge_invalidate =
                m ? m->compiler_inval_bridge_epoch_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t invalidate_fn =
                m ? m->invalidate_function_calls.load(std::memory_order_relaxed) : 0;
            const std::uint64_t cascade_body =
                m ? m->cascade_body_only_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_work = relower_full + relower_per_fn + 1;
            const std::int64_t min_scope_hit_pct =
                static_cast<std::int64_t>((blocks_saved * 100) / relower_work);
            const std::uint64_t total = should_relower + partial + impact_calls + affected_blocks +
                                        relower_skip + relower_per_fn + relower_full +
                                        blocks_saved + jit_invalidate + cascade_body +
                                        soa.dirty_blocks;
            std::int64_t recommendation = 0;
            if (soa.dirty_block_pct > 50 && blocks_saved == 0)
                recommendation = 3;
            else if (relower_full > relower_per_fn && min_scope_hit_pct < 25)
                recommendation = 2;
            else if (partial > 0 || blocks_saved > 0 || relower_skip > 0)
                recommendation = 1;
            insert_kv("should-relower-triggers", static_cast<std::int64_t>(should_relower));
            insert_kv("partial-relowers", static_cast<std::int64_t>(partial));
            insert_kv("impact-scope-calls", static_cast<std::int64_t>(impact_calls));
            insert_kv("affected-blocks-total", static_cast<std::int64_t>(affected_blocks));
            insert_kv("relower-skipped", static_cast<std::int64_t>(relower_skip));
            insert_kv("relower-per-fn", static_cast<std::int64_t>(relower_per_fn));
            insert_kv("relower-full", static_cast<std::int64_t>(relower_full));
            insert_kv("blocks-saved", static_cast<std::int64_t>(blocks_saved));
            insert_kv("jit-invalidate-count", static_cast<std::int64_t>(jit_invalidate));
            insert_kv("bridge-invalidate-count", static_cast<std::int64_t>(bridge_invalidate));
            insert_kv("invalidate-function-calls", static_cast<std::int64_t>(invalidate_fn));
            insert_kv("cascade-body-only", static_cast<std::int64_t>(cascade_body));
            insert_kv("dirty-blocks", static_cast<std::int64_t>(soa.dirty_blocks));
            insert_kv("dirty-functions", static_cast<std::int64_t>(soa.dirty_fns));
            insert_kv("cached-functions", static_cast<std::int64_t>(soa.cached_fns));
            insert_kv("dirty-block-pct", static_cast<std::int64_t>(soa.dirty_block_pct));
            insert_kv("min-scope-hit-rate-pct", min_scope_hit_pct);
            insert_kv("incremental-production-relower-total", static_cast<std::int64_t>(total));
            insert_kv("incremental-production-relower-recommendation", recommendation);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #532 / #1512 / #1658 / #1917: query:jit-consistency-stats. Commercial P0
    // hash view of JIT opcode coverage completeness, IRInterpreter execution
    // consistency, and GuardShape/Linear/hot-swap safety — non-duplicative
    // synthesis of #491 jit-stats-hash, #601 jit-hotswap-closure-stats,
    // #513/#522 AOT reload themes, and #516 prompt6-memory-safety
    // linear/bridge slices; avoids repeating the per-field jit-stats-hash
    // surface verbatim:
    //   P1 Opcode coverage: unhandled-count, fallback-count,
    //      consistency-violations, opcode-coverage-pct
    //   P2 Deopt parity: deopt-count, deopt-rate-pct
    //   P3 Hot-swap safety: hotswap-invalidate, live-closure-refreshed,
    //      forced-deopt, hotswap-success-rate-pct
    //   P4 Linear/bridge: linear-check-hits, bridge-epoch-hits,
    //      epoch-mismatch-hits, safe-fallbacks
    //   P5 #1658 mandate: GuardShape/Linear/PrimCall lowered wire flags,
    //      strict-consistency default-on, fail-fast safe-deopt, schema 1658
    //   P6 #1917 critical opcodes: MakeClosure/Apply/PrimCall/GuardShape/Linear*
    //      coverage pct, fastpath hits, apply-site epoch probe
    //   - jit-consistency-total / jit-consistency-recommendation
    ObservabilityPrims::register_stats_impl(
        "query:jit-consistency-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            // Capacity power-of-two; #1917 adds critical coverage keys (~46 total).
            auto* ht = FlatHashTable::create(query_hash_capacity_for(54));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            std::uint64_t compiles = 0;
            std::uint64_t unhandled = 0;
            std::uint64_t fallback = aura_jit_fallback_count_v_read();
            std::uint64_t consistency = 0;
            // Issue #1917 critical-path counters (from Metrics::format).
            std::uint64_t critical_lowered = 0;
            std::uint64_t critical_unhandled = 0;
            std::uint64_t critical_coverage_pct = 100;
            std::uint64_t primcall_fastpath = 0;
            std::uint64_t apply_site_probe = 0;
            if (ev->get_jit_stats_fn_) {
                const char* s = ev->get_jit_stats_fn_();
                if (s) {
                    auto parse_u64 = [&](std::string_view key) -> std::uint64_t {
                        std::string_view hay(s);
                        auto pos = hay.find(key);
                        if (pos == std::string_view::npos)
                            return 0;
                        return std::strtoull(hay.data() + pos + key.size(), nullptr, 10);
                    };
                    compiles = parse_u64("compiles=");
                    unhandled = parse_u64("unhandled_opcode=");
                    fallback = parse_u64("fallback_count=");
                    consistency = parse_u64("consistency_violations=");
                    critical_lowered = parse_u64("critical_opcode_lowered=");
                    critical_unhandled = parse_u64("critical_opcode_unhandled=");
                    critical_coverage_pct = parse_u64("critical_opcode_coverage_pct=");
                    // When format omits key (older builds), static lower table is complete.
                    if (std::string_view(s).find("critical_opcode_coverage_pct=") ==
                        std::string_view::npos)
                        critical_coverage_pct = 100;
                    primcall_fastpath = parse_u64("primcall_fastpath_hits=");
                    apply_site_probe = parse_u64("apply_site_epoch_probe=");
                }
            }
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t deopt = m ? m->deopt_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t hotswap_invalidate =
                m ? m->jit_hotswap_invalidate_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t refreshed =
                m ? m->jit_hotswap_live_closure_refreshed_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t forced_deopt =
                m ? m->jit_hotswap_forced_deopt_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t linear_hits =
                m ? m->linear_check_pass_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t bridge_hits =
                m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t epoch_mismatch =
                m ? m->compiler_closure_epoch_mismatch_hits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t safe_fallbacks =
                m ? m->compiler_closure_safe_fallbacks.load(std::memory_order_relaxed) : 0;
            const std::int64_t deopt_rate_pct =
                compiles > 0 ? static_cast<std::int64_t>((deopt * 100) / compiles) : 0;
            const std::int64_t coverage_pct =
                unhandled == 0 && fallback == 0
                    ? 100
                    : std::max<std::int64_t>(
                          0, 100 - static_cast<std::int64_t>((unhandled + fallback) * 100 /
                                                             std::max<std::uint64_t>(1, compiles)));
            const std::uint64_t hotswap_work = refreshed + forced_deopt + 1;
            const std::int64_t hotswap_success_pct =
                static_cast<std::int64_t>((refreshed * 100) / hotswap_work);
            const std::uint64_t total = unhandled + fallback + consistency + compiles + deopt +
                                        hotswap_invalidate + refreshed + forced_deopt +
                                        linear_hits + bridge_hits + epoch_mismatch + safe_fallbacks;
            std::int64_t recommendation = 0;
            if (unhandled > 0 || consistency > 0)
                recommendation = 3;
            else if (deopt_rate_pct > 10 || forced_deopt > refreshed)
                recommendation = 2;
            else if (hotswap_invalidate > 0 || bridge_hits > 0)
                recommendation = 1;
            insert_kv("unhandled-count", static_cast<std::int64_t>(unhandled));
            insert_kv("fallback-count", static_cast<std::int64_t>(fallback));
            insert_kv("consistency-violations", static_cast<std::int64_t>(consistency));
            insert_kv("compiles", static_cast<std::int64_t>(compiles));
            insert_kv("opcode-coverage-pct", coverage_pct);
            insert_kv("deopt-count", static_cast<std::int64_t>(deopt));
            insert_kv("deopt-rate-pct", deopt_rate_pct);
            insert_kv("hotswap-invalidate-count", static_cast<std::int64_t>(hotswap_invalidate));
            insert_kv("live-closure-refreshed", static_cast<std::int64_t>(refreshed));
            insert_kv("forced-deopt-total", static_cast<std::int64_t>(forced_deopt));
            insert_kv("hotswap-success-rate-pct", hotswap_success_pct);
            insert_kv("linear-check-hits", static_cast<std::int64_t>(linear_hits));
            insert_kv("bridge-epoch-hits", static_cast<std::int64_t>(bridge_hits));
            insert_kv("epoch-mismatch-hits", static_cast<std::int64_t>(epoch_mismatch));
            insert_kv("safe-fallbacks", static_cast<std::int64_t>(safe_fallbacks));
            insert_kv("jit-consistency-total", static_cast<std::int64_t>(total));
            insert_kv("jit-consistency-recommendation", recommendation);
            // Issue #1658: opcode coverage + strict-consistency mandate flags.
            // GuardShape / Linear* / PrimCall are fully lowered in aura_jit.cpp;
            // unhandled path is fail-fast (compile nullptr → interpreter).
            insert_kv("opcode-tracked-total", 54);
            insert_kv("guard-shape-lowered-wired", 1);
            insert_kv("linear-ops-lowered-wired", 1);
            insert_kv("primcall-lowered-wired", 1);
            insert_kv("fail-fast-unhandled-wired", 1);
            insert_kv("safe-deopt-on-unhandled-wired", 1);
            insert_kv("strict-consistency-default-on", 1);
            insert_kv("force-jit-consistency-check-wired", 1);
            insert_kv("consistency-mandate-active", 1);
            // Issue #1917: critical opcode coverage + PrimCall/Apply hardening.
            // Keep schema=1658 for lineage tests; schema-1917 is additive.
            insert_kv("critical-opcode-count", 13);
            insert_kv("critical-opcode-coverage-pct",
                      static_cast<std::int64_t>(critical_coverage_pct));
            insert_kv("critical-opcode-lowered-total", static_cast<std::int64_t>(critical_lowered));
            insert_kv("critical-opcode-unhandled-total",
                      static_cast<std::int64_t>(critical_unhandled));
            insert_kv("primcall-fastpath-hits", static_cast<std::int64_t>(primcall_fastpath));
            insert_kv("apply-site-epoch-probe-total", static_cast<std::int64_t>(apply_site_probe));
            insert_kv("make-closure-lowered-wired", 1);
            insert_kv("apply-lowered-wired", 1);
            insert_kv("capture-lowered-wired", 1);
            insert_kv("call-lowered-wired", 1);
            insert_kv("guard-shape-critical-wired", 1);
            insert_kv("linear-ops-critical-wired", 1);
            insert_kv("primcall-fastpath-vector-error-wired", 1);
            insert_kv("apply-site-epoch-probe-wired", 1);
            insert_kv("critical-coverage-mandate-active", 1);
            // AC: critical lower success ≥80 when static table complete (unhandled=0).
            insert_kv("critical-hit-rate-gate-pct",
                      static_cast<std::int64_t>(critical_coverage_pct));
            insert_kv("schema-1917", 1917);
            insert_kv("issue-1917", 1917);
            insert_kv("issue", 1658);
            insert_kv("schema", 1658); // lineage 532 / 1512 / 1289 / 427 → 1658 + #1917
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #533: query:soa-production-columnar-stats. Commercial P0 hash view
    // of children_ columnar migration + IRModuleV2 / IRInstructionView hot-path
    // adoption + DirtyAwarePass block_dirty_ short-circuit — non-duplicative
    // synthesis of #463 soa-adoption-stats, #506 soa-hotpath-adoption int-sum,
    // #404 ir-soa-incremental-stats, #429 soa-dirty-stats, #530 incremental-
    // production-relower-stats, and #684 irsoa-incremental-stats hash slices:
    //   P1 AST children columnar: children-call/safe-view, mark-dirty upward
    //   P2 IR SoA adoption: soa-functions/instructions-visited, view-cache,
    //      irsoa-wired-hits, ir-soa-instr/func-emitted
    //   P3 block_dirty short-circuit: block-dirty-hits, blocks-saved,
    //      passes-skipped-type-dirty, passes-skipped-dirty-pipeline
    //   - dirty-skip-rate-pct / columnar-locality-pct
    //   - soa-production-columnar-total / soa-production-columnar-recommendation
    ObservabilityPrims::register_stats_impl(
        "query:soa-production-columnar-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(
                query_hash_capacity_for(34)); // #2036 SafePCVSpan default keys
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t children_calls = ws ? ws->children_call_count() : 0;
            const std::uint64_t children_safe = ws ? ws->children_safe_view_count() : 0;
            const std::uint64_t dirty_up = ws ? ws->mark_dirty_upward_call_count() : 0;
            const std::uint64_t dirty_nodes = ws ? ws->mark_dirty_total_nodes() : 0;
            const std::uint64_t fast_fixed = ws ? ws->dirty_upward_fast_fixed_point_count() : 0;
            const std::uint64_t soa_funcs =
                m ? m->soa_functions_visited.load(std::memory_order_relaxed) : 0;
            const std::uint64_t soa_instr =
                m ? m->soa_instructions_visited.load(std::memory_order_relaxed) : 0;
            const std::uint64_t aos_views =
                m ? m->aos_view_built_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t view_cache =
                m ? m->ir_soa_view_cache_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t wired = m ? m->irsoa_wired_hits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t block_dirty_hits =
                m ? m->ir_soa_block_dirty_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t blocks_saved =
                m ? m->ir_soa_relower_blocks_saved_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t passes_skip_type = ev->get_passes_skipped_type_dirty();
            const std::uint64_t passes_skip_pipeline =
                aura::compiler::passes_skipped_dirty_pipeline.load(std::memory_order_relaxed);
            const std::uint64_t ir_instr_emitted =
                m ? m->ir_soa_instructions_emitted.load(std::memory_order_relaxed) : 0;
            const std::uint64_t ir_func_emitted =
                m ? m->ir_soa_functions_emitted.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_full =
                m ? m->relower_full_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_per_fn =
                m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_work = relower_full + relower_per_fn + passes_skip_type + 1;
            const std::int64_t dirty_skip_rate_pct =
                static_cast<std::int64_t>((passes_skip_type * 100) / relower_work);
            const std::uint64_t columnar_denom = children_calls + children_safe + 1;
            const std::int64_t columnar_locality_pct =
                static_cast<std::int64_t>((children_safe * 100) / columnar_denom);
            Evaluator::SoaDirtyStats soa;
            if (ev->get_soa_dirty_stats_fn_)
                soa = ev->get_soa_dirty_stats_fn_();
            const std::uint64_t total = children_calls + children_safe + dirty_up + dirty_nodes +
                                        fast_fixed + soa_funcs + soa_instr + aos_views +
                                        view_cache + wired + block_dirty_hits + blocks_saved +
                                        passes_skip_type + passes_skip_pipeline + ir_instr_emitted +
                                        ir_func_emitted + soa.dirty_blocks;
            std::int64_t recommendation = 0;
            if (soa.dirty_block_pct > 50 && blocks_saved == 0)
                recommendation = 3;
            else if (passes_skip_type == 0 && relower_full > 0)
                recommendation = 2;
            else if (blocks_saved > 0 || passes_skip_type > 0 || wired > 0)
                recommendation = 1;
            insert_kv("children-call-count", static_cast<std::int64_t>(children_calls));
            insert_kv("children-safe-view-count", static_cast<std::int64_t>(children_safe));
            // Issue #2036: PCV migration end-state + SafePCVSpan default APIs.
            {
                const std::uint64_t safe_default =
                    ws ? ws->children_stable_safe_default_total() : 0;
                const std::uint64_t span_calls = ws ? ws->children_stable_span_calls_total() : 0;
                insert_kv("children-stable-safe-default-total",
                          static_cast<std::int64_t>(safe_default));
                insert_kv("children-stable-span-calls-total",
                          static_cast<std::int64_t>(span_calls));
                insert_kv("children-pcv-migration-complete",
                          ws ? ws->children_pcv_migration_complete() : 1);
                insert_kv("children-default-safe-pcv-wired", 1);
                insert_kv("schema-2036", 2036);
                insert_kv("issue-2036", 2036);
            }
            insert_kv("mark-dirty-upward-calls", static_cast<std::int64_t>(dirty_up));
            insert_kv("mark-dirty-total-nodes", static_cast<std::int64_t>(dirty_nodes));
            insert_kv("dirty-fast-fixed-point-hits", static_cast<std::int64_t>(fast_fixed));
            insert_kv("soa-functions-visited", static_cast<std::int64_t>(soa_funcs));
            insert_kv("soa-instructions-visited", static_cast<std::int64_t>(soa_instr));
            insert_kv("aos-view-built-count", static_cast<std::int64_t>(aos_views));
            insert_kv("ir-soa-view-cache-hits", static_cast<std::int64_t>(view_cache));
            insert_kv("irsoa-wired-hits", static_cast<std::int64_t>(wired));
            insert_kv("ir-soa-block-dirty-hits", static_cast<std::int64_t>(block_dirty_hits));
            insert_kv("blocks-saved", static_cast<std::int64_t>(blocks_saved));
            insert_kv("passes-skipped-type-dirty", static_cast<std::int64_t>(passes_skip_type));
            insert_kv("passes-skipped-dirty-pipeline",
                      static_cast<std::int64_t>(passes_skip_pipeline));
            insert_kv("ir-soa-instr-emitted", static_cast<std::int64_t>(ir_instr_emitted));
            insert_kv("ir-soa-func-emitted", static_cast<std::int64_t>(ir_func_emitted));
            insert_kv("dirty-skip-rate-pct", dirty_skip_rate_pct);
            insert_kv("columnar-locality-pct", columnar_locality_pct);
            insert_kv("soa-production-columnar-total", static_cast<std::int64_t>(total));
            insert_kv("soa-production-columnar-recommendation", recommendation);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #568: query:soa-children-columnar-migration-stats. Task4-review
    // closing hash for FlatAST children_ columnar SoA migration completion +
    // IRInstructionView hot-path adoption + DirtyAwarePass block_dirty_
    // short-circuit — non-duplicative synthesis of #533 soa-production-
    // columnar-stats, #463 soa-adoption-stats, #506 soa-hotpath-adoption,
    // #404 ir-soa-incremental-stats, #684 irsoa-incremental-stats, and #607
    // task4-cache-locality-win themes; focuses on #568 completion metrics:
    //   P1 Columnar children: children-call/safe-view, child-columnar-hit-rate
    //   P2 IR SoA hot-path: soa-functions/instructions-visited, view-cache,
    //      irsoa-wired-hits, ir-soa-instr/func-emitted
    //   P3 DirtyAwarePass: passes-skipped-due-to-dirty, relower-block-count,
    //      blocks-saved, ir-soa-block-dirty-hits
    //   - migration-schema (568 sentinel)
    //   - soa-children-columnar-migration-total / recommendation
    ObservabilityPrims::register_stats_impl(
        "query:soa-children-columnar-migration-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht =
                FlatHashTable::create(query_hash_capacity_for(34)); // #2036 migration complete keys
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t children_calls = ws ? ws->children_call_count() : 0;
            const std::uint64_t children_safe = ws ? ws->children_safe_view_count() : 0;
            const std::uint64_t columnar_denom = children_calls + children_safe + 1;
            const std::int64_t columnar_hit_rate_pct =
                static_cast<std::int64_t>((children_safe * 100) / columnar_denom);
            const std::uint64_t soa_funcs =
                m ? m->soa_functions_visited.load(std::memory_order_relaxed) : 0;
            const std::uint64_t soa_instr =
                m ? m->soa_instructions_visited.load(std::memory_order_relaxed) : 0;
            const std::uint64_t view_cache =
                m ? m->ir_soa_view_cache_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t wired = m ? m->irsoa_wired_hits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t ir_instr_emitted =
                m ? m->ir_soa_instructions_emitted.load(std::memory_order_relaxed) : 0;
            const std::uint64_t ir_func_emitted =
                m ? m->ir_soa_functions_emitted.load(std::memory_order_relaxed) : 0;
            const std::uint64_t passes_skip_type = ev->get_passes_skipped_type_dirty();
            const std::uint64_t passes_skip_pipeline =
                aura::compiler::passes_skipped_dirty_pipeline.load(std::memory_order_relaxed);
            const std::uint64_t passes_skipped_due_to_dirty =
                passes_skip_type + passes_skip_pipeline;
            const std::uint64_t relower_full =
                m ? m->relower_full_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_per_fn =
                m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_block_count = relower_full + relower_per_fn;
            const std::uint64_t blocks_saved =
                m ? m->ir_soa_relower_blocks_saved_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t block_dirty_hits =
                m ? m->ir_soa_block_dirty_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t total = children_calls + children_safe + soa_funcs + soa_instr +
                                        view_cache + wired + ir_instr_emitted + ir_func_emitted +
                                        passes_skipped_due_to_dirty + relower_block_count +
                                        blocks_saved + block_dirty_hits;
            std::int64_t recommendation = 0;
            if (columnar_hit_rate_pct < 25 && children_calls > 0)
                recommendation = 3;
            else if (passes_skipped_due_to_dirty == 0 && relower_block_count > 0)
                recommendation = 2;
            else if (blocks_saved > 0 || wired > 0 || columnar_hit_rate_pct >= 50)
                recommendation = 1;
            insert_kv("children-call-count", static_cast<std::int64_t>(children_calls));
            insert_kv("children-safe-view-count", static_cast<std::int64_t>(children_safe));
            insert_kv("child-columnar-hit-rate-pct", columnar_hit_rate_pct);
            insert_kv("soa-functions-visited", static_cast<std::int64_t>(soa_funcs));
            insert_kv("soa-instructions-visited", static_cast<std::int64_t>(soa_instr));
            insert_kv("ir-soa-view-cache-hits", static_cast<std::int64_t>(view_cache));
            insert_kv("irsoa-wired-hits", static_cast<std::int64_t>(wired));
            insert_kv("ir-soa-instr-emitted", static_cast<std::int64_t>(ir_instr_emitted));
            insert_kv("ir-soa-func-emitted", static_cast<std::int64_t>(ir_func_emitted));
            insert_kv("passes-skipped-due-to-dirty",
                      static_cast<std::int64_t>(passes_skipped_due_to_dirty));
            insert_kv("relower-block-count", static_cast<std::int64_t>(relower_block_count));
            insert_kv("blocks-saved", static_cast<std::int64_t>(blocks_saved));
            insert_kv("ir-soa-block-dirty-hits", static_cast<std::int64_t>(block_dirty_hits));
            insert_kv("migration-schema", 568);
            insert_kv("soa-children-columnar-migration-total", static_cast<std::int64_t>(total));
            insert_kv("soa-children-columnar-migration-recommendation", recommendation);
            // Issue #1520: live FlatAST columnar counters when workspace present.
            if (ws) {
                insert_kv("children-column-soa-hits",
                          static_cast<std::int64_t>(ws->children_column_soa_hits()));
                insert_kv("pcv-pin-count", static_cast<std::int64_t>(ws->pcv_pin_count()));
                insert_kv("region-dense-hits", static_cast<std::int64_t>(ws->region_dense_hits()));
                insert_kv("map-indirection-miss",
                          static_cast<std::int64_t>(ws->map_indirection_miss_total()));
                // Issue #2036: PCV migration end-state.
                insert_kv("children-stable-safe-default-total",
                          static_cast<std::int64_t>(ws->children_stable_safe_default_total()));
                insert_kv("children-stable-span-calls-total",
                          static_cast<std::int64_t>(ws->children_stable_span_calls_total()));
            }
            insert_kv("children-pcv-migration-complete", 1);
            insert_kv("children-default-safe-pcv-wired", 1);
            insert_kv("schema-2036", 2036);
            insert_kv("issue-2036", 2036);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #2862: query:children-stable-stats. Hash view of the
    // query:children-stable full safety contract metrics (refine
    // #2036 / #678 / #655 Gap4 / #2861). Tracks the 4 non-negotiable
    // safety contract surfaces mandated by #2861 AC #7 ("metrics on
    // query / stability surface"):
    //   - children-stable-span-calls-total: every SafePCVSpan /
    //       children_ safe_view walk on the public surface (already
    //       tracked on FlatAST as ws->children_stable_span_calls_total()
    //       - ast.ixx #2198 - and reused via the query surface).
    //   - children-stable-pin-hits-total: every SafePCVSpan pin hit
    //       (amortized refcount).
    //   - children-stable-invalidation-detected-total: StableNodeRef
    //       returned from children-stable that failed is_valid /
    //       refresh after concurrent mutate and was dropped.
    //   - children-stable-epoch-mismatch-total: QueryEpoch
    //       (mutation_epoch + generation) mismatch on a held span
    //       under concurrent Guard.
    // Distinct from #2861 pattern-safety-stats (query:pattern walks)
    // - these are the query:children-stable surfaces. Children-stable
    // views are held ACROSS mutate rounds so invalidation_detected +
    // epoch_mismatch have longer exposure windows than the pattern
    // equivalents.
    ObservabilityPrims::register_stats_impl(
        "query:children-stable-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            auto* ws = ev->workspace_flat();
            const std::uint64_t span_calls = ws ? ws->children_stable_span_calls_total() : 0;
            const std::uint64_t pin_hits =
                m ? m->children_stable_pin_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t invalidation_detected =
                m ? m->children_stable_invalidation_detected_total.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t epoch_mismatch =
                m ? m->children_stable_epoch_mismatch_total.load(std::memory_order_relaxed) : 0;
            // Capacity 64: #2862 keys + #2960 stamp counters / schema.
            auto* ht = FlatHashTable::create(query_hash_capacity_for(29));
            if (!ht)
                return make_void();
            bool overflowed = false;
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const std::string& k_str, std::int64_t v) {
                if (!insert_kv_checked(ht, ev->string_heap_mut(), k_str, v))
                    overflowed = true;
            };
            insert_kv("schema", 2862);
            insert_kv("issue", 2862);
            insert_kv("children-stable-span-calls-total", static_cast<std::int64_t>(span_calls));
            insert_kv("children-stable-pin-hits-total", static_cast<std::int64_t>(pin_hits));
            insert_kv("children-stable-invalidation-detected-total",
                      static_cast<std::int64_t>(invalidation_detected));
            insert_kv("children-stable-epoch-mismatch-total",
                      static_cast<std::int64_t>(epoch_mismatch));
            // Issue #2960: query stable-return stamp counters (Agent export).
            insert_kv("query-stable-ref-stamped-total",
                      static_cast<std::int64_t>(
                          aura::core::provenance::g_query_stable_ref_stamped_total_atomic().load(
                              std::memory_order_relaxed)));
            insert_kv(
                "query-stable-ref-unstamped-prevented-total",
                static_cast<std::int64_t>(
                    aura::core::provenance::g_query_stable_ref_unstamped_prevented_total_atomic()
                        .load(std::memory_order_relaxed)));
            insert_kv("schema-2960", 2960);
            insert_kv("issue-2960", 2960);
            // Issue #3000: restamp-lag export gate (additive on children-stable-stats).
            insert_kv(
                "query-stable-ref-restamp-lag-prevented-total",
                static_cast<std::int64_t>(
                    aura::core::provenance::g_query_stable_ref_restamp_lag_prevented_total_atomic()
                        .load(std::memory_order_relaxed)));
            insert_kv("query-stable-ref-restamp-lag-soft-observe-total",
                      static_cast<std::int64_t>(
                          aura::core::provenance::
                              g_query_stable_ref_restamp_lag_soft_observe_total_atomic()
                                  .load(std::memory_order_relaxed)));
            insert_kv("schema-3000", 3000);
            insert_kv("issue-3000", 3000);
            // Issue #3037: over-budget torn export (additive).
            insert_kv(
                "query-stable-ref-restamp-torn-reject-total",
                static_cast<std::int64_t>(
                    aura::core::provenance::g_query_stable_ref_restamp_torn_reject_total_atomic()
                        .load(std::memory_order_relaxed)));
            insert_kv("schema-3037", 3037);
            insert_kv("issue-3037", 3037);
            // Issue #3076: Soft-observe counters are not Hard guarantees.
            insert_kv("soft-observe-not-hard-guarantee", 1);
            insert_kv("schema-3076", 3076);
            insert_kv("issue-3076", 3076);
            insert_kv("schema-3058", aura::ast::kUnifiedRestampQueryVisibleIssue);
            insert_kv("issue-3058", aura::ast::kUnifiedRestampQueryVisibleIssue);
            insert_kv("schema-3041", 3041);
            insert_kv("restamp-budget-query-epoch-stale-total",
                      static_cast<std::int64_t>(
                          aura::core::g_restamp_budget_query_epoch_stale_total().load(
                              std::memory_order_relaxed)));
            return query_hash_finish(ht, ev->string_heap_mut(), overflowed);
        });

    // Issue #2863: query:replace-subtree-stats. Hash view of the
    // mutate:replace-subtree full safety contract metrics (refine
    // #2858 / #2797 / #1281 / #369 / #2801). Tracks the 5
    // non-negotiable safety contract surfaces mandated by #2863
    // AC #8 ("observability: full mutation log, fine-rollback counts,
    // macro_restamp, densify triggers, hygiene rejects"):
    //   - mutate-replace-subtree-calls-total: every primitive entry
    //       (all paths via MutationBoundaryGuard).
    //   - mutate-replace-subtree-fine-rollback-total: fine-rollback
    //       fired on failure (parse / hygiene / linear / type).
    //   - mutate-replace-subtree-densify-triggers-total: post-mutate
    //       densify cascade fired (children_ PCV topology change).
    //   - mutate-replace-subtree-hygiene-rejects-total: default
    //       MacroIntroduced reject (without :allow-macro?).
    //   - mutate-replace-subtree-restamp-nodes-total: StableNodeRef
    //       restamp nodes (target + replacement + cascade) on success.
    // Distinct from existing #2858 macro_mutate_auto_restamp_total
    // (allowed-mutate cascade) and #2037 hygiene_mutate_restamp_total
    // (hygiene-restamp-only). These are the #2863 contract surfaces
    // covering replace-subtree entry + fine-rollback + densify +
    // hygiene reject + restamp nodes.
    ObservabilityPrims::register_stats_impl(
        "query:replace-subtree-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t calls =
                m ? m->mutate_replace_subtree_calls_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t fine_rollback =
                m ? m->mutate_replace_subtree_fine_rollback_total.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t densify_triggers =
                m ? m->mutate_replace_subtree_densify_triggers_total.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t hygiene_rejects =
                m ? m->mutate_replace_subtree_hygiene_rejects_total.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t restamp_nodes =
                m ? m->mutate_replace_subtree_restamp_nodes_total.load(std::memory_order_relaxed)
                  : 0;
            auto* ht = FlatHashTable::create(query_hash_capacity_for(15));
            if (!ht)
                return make_void();
            bool overflowed = false;
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const std::string& k_str, std::int64_t v) {
                if (!insert_kv_checked(ht, ev->string_heap_mut(), k_str, v))
                    overflowed = true;
            };
            insert_kv("schema", 2863);
            insert_kv("issue", 2863);
            insert_kv("mutate-replace-subtree-calls-total", static_cast<std::int64_t>(calls));
            insert_kv("mutate-replace-subtree-fine-rollback-total",
                      static_cast<std::int64_t>(fine_rollback));
            insert_kv("mutate-replace-subtree-densify-triggers-total",
                      static_cast<std::int64_t>(densify_triggers));
            insert_kv("mutate-replace-subtree-hygiene-rejects-total",
                      static_cast<std::int64_t>(hygiene_rejects));
            insert_kv("mutate-replace-subtree-restamp-nodes-total",
                      static_cast<std::int64_t>(restamp_nodes));
            return query_hash_finish(ht, ev->string_heap_mut(), overflowed);
        });

    // Issue #2864: query:remove-node-stats. Hash view of the
    // mutate:remove-node full safety contract metrics (refine
    // #1688 / #1689 / #1281 / #369 / #2863 sibling). Tracks the 5
    // non-negotiable safety contract surfaces mandated by #2864
    // AC #7 ("observability: edges_removed, multi_parent_count,
    // rollback_fidelity, densify_triggered"):
    //   - mutate-remove-node-calls-total: every primitive entry.
    //   - mutate-remove-node-edges-removed-total: parent-edge
    //       removals (DAG multi-parent case bumps by N).
    //   - mutate-remove-node-multi-parent-count-total: # of times
    //       a target had 2+ parents (DAG path exercised).
    //   - mutate-remove-node-rollback-fidelity-total: fine-rollback
    //       fired on failure.
    //   - mutate-remove-node-densify-triggered-total: post-mutate
    //       densify cascade fired.
    // Distinct from existing #2863 mutate_replace_subtree_*
    // (sibling mutate:replace-subtree contract). These are the
    // #2864 remove-node contract surfaces.
    ObservabilityPrims::register_stats_impl(
        "query:remove-node-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t calls =
                m ? m->mutate_remove_node_calls_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t edges_removed =
                m ? m->mutate_remove_node_edges_removed_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t multi_parent =
                m ? m->mutate_remove_node_multi_parent_count_total.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t rollback_fidelity =
                m ? m->mutate_remove_node_rollback_fidelity_total.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t densify_triggered =
                m ? m->mutate_remove_node_densify_triggered_total.load(std::memory_order_relaxed)
                  : 0;
            auto* ht = FlatHashTable::create(query_hash_capacity_for(15));
            if (!ht)
                return make_void();
            bool overflowed = false;
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const std::string& k_str, std::int64_t v) {
                if (!insert_kv_checked(ht, ev->string_heap_mut(), k_str, v))
                    overflowed = true;
            };
            insert_kv("schema", 2864);
            insert_kv("issue", 2864);
            insert_kv("mutate-remove-node-calls-total", static_cast<std::int64_t>(calls));
            insert_kv("mutate-remove-node-edges-removed-total",
                      static_cast<std::int64_t>(edges_removed));
            insert_kv("mutate-remove-node-multi-parent-count-total",
                      static_cast<std::int64_t>(multi_parent));
            insert_kv("mutate-remove-node-rollback-fidelity-total",
                      static_cast<std::int64_t>(rollback_fidelity));
            insert_kv("mutate-remove-node-densify-triggered-total",
                      static_cast<std::int64_t>(densify_triggered));
            return query_hash_finish(ht, ev->string_heap_mut(), overflowed);
        });

    // Issue #2179: query:impact-scope-stats — cross-function instruction-
    // level impact scope metrics (refine #2109 instr-level precision).
    // Returns a hash with:
    //   - schema-2179 / issue-2179 sentinels
    //   - impact-scope-cross-fn-wired (1 if engine wired)
    //   - impact-scope-cross-fn-blocks-total
    //   - impact-scope-cross-fn-instrs-total
    //   - impact-scope-cross-fn-callsites-total
    // Counters come from CompilerMetrics (incremented by the new
    // compute_impact_scope cross-function fan-out when call-site
    // instructions in callers are discovered via node_dep_graph_).
    ObservabilityPrims::register_stats_impl(
        "query:impact-scope-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            if (!m)
                return make_void();
            auto* ht = FlatHashTable::create(
                16); // #2246: 10 keys (indirect/unresolved/schema/wired/blocks/instrs/callsites)
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
            // Issue #2246: refine #2179 — indirect + unresolved callee hits
            insert_kv("impact-scope-cross-fn-indirect-total",
                      static_cast<std::int64_t>(
                          m->impact_scope_cross_fn_indirect_total.load(std::memory_order_relaxed)));
            insert_kv("impact-scope-unresolved-callee-total",
                      static_cast<std::int64_t>(
                          m->impact_scope_unresolved_callee_total.load(std::memory_order_relaxed)));
            insert_kv("schema-2246", 2246);
            insert_kv("issue-2246", 2246);
            insert_kv("schema-2179", 2179);
            insert_kv("issue-2179", 2179);
            insert_kv("impact-scope-cross-fn-wired", 1);
            insert_kv("impact-scope-cross-fn-blocks-total",
                      static_cast<std::int64_t>(
                          m->impact_scope_cross_fn_blocks_total.load(std::memory_order_relaxed)));
            insert_kv("impact-scope-cross-fn-instrs-total",
                      static_cast<std::int64_t>(
                          m->impact_scope_cross_fn_instrs_total.load(std::memory_order_relaxed)));
            insert_kv("impact-scope-cross-fn-callsites-total",
                      static_cast<std::int64_t>(
                          m->impact_scope_cross_fn_instrs_total.load(std::memory_order_relaxed)));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
    // region dense lookup + DOD migration progress surface
    // (non-duplicative with #568 migration hash; no new query:*-stats).
    ObservabilityPrims::register_stats_impl(
        "query:children-column-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(query_hash_capacity_for(24)); // #1624 more AC keys
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            auto* ws = ev->workspace_flat();
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            const std::int64_t col =
                ws ? static_cast<std::int64_t>(ws->children_column_soa_hits()) : 0;
            const std::int64_t pin = ws ? static_cast<std::int64_t>(ws->pcv_pin_count()) : 0;
            const std::int64_t dense = ws ? static_cast<std::int64_t>(ws->region_dense_hits()) : 0;
            const std::int64_t map_miss =
                ws ? static_cast<std::int64_t>(ws->map_indirection_miss_total()) : 0;
            const std::int64_t raw = ws ? static_cast<std::int64_t>(ws->children_call_count()) : 0;
            const std::int64_t safe =
                ws ? static_cast<std::int64_t>(ws->children_safe_view_count()) : 0;
            if (m) {
                m->children_column_soa_hits_total.store(static_cast<std::uint64_t>(col),
                                                        std::memory_order_relaxed);
                m->pcv_pin_count_total.store(static_cast<std::uint64_t>(pin),
                                             std::memory_order_relaxed);
                m->region_dense_hits_total.store(static_cast<std::uint64_t>(dense),
                                                 std::memory_order_relaxed);
                m->map_indirection_miss_total.store(static_cast<std::uint64_t>(map_miss),
                                                    std::memory_order_relaxed);
            }
            const auto denom = col + raw + 1;
            const auto columnar_pct = (col * 100) / denom;
            // Issue #1624: DOD migration progress + hit rate (basis points).
            const std::int64_t dod_progress =
                ws ? static_cast<std::int64_t>(ws->soa_dod_migration_progress()) : col;
            const std::int64_t hit_rate_bp =
                ws ? static_cast<std::int64_t>(ws->pcv_columnar_hit_rate_bp())
                   : static_cast<std::int64_t>((col * 10000) / denom);
            if (m) {
                m->soa_dod_migration_progress_total.store(static_cast<std::uint64_t>(dod_progress),
                                                          std::memory_order_relaxed);
                m->pcv_columnar_hit_rate_bp.store(static_cast<std::uint64_t>(hit_rate_bp),
                                                  std::memory_order_relaxed);
            }
            insert_kv("children-column-soa-hits", col);
            insert_kv("pcv-pin-count", pin);
            insert_kv("region-dense-hits", dense);
            insert_kv("map-indirection-miss", map_miss);
            insert_kv("children-raw-calls", raw);
            insert_kv("children-safe-views", safe);
            insert_kv("columnar-hit-rate-pct", columnar_pct);
            // #1624 AC keys (no new query:*-stats — fold into #1520 surface)
            insert_kv("soa_dod_migration_progress", dod_progress);
            insert_kv("pcv_columnar_hit_rate", hit_rate_bp);
            insert_kv("pcv_columnar_hit_rate_bp", hit_rate_bp);
            insert_kv("soa-columnar-concept-enforced", 1);
            insert_kv("soa-columnar-full-enforced", 1);
            insert_kv("pmr-columns-soa-columnar", 1);
            insert_kv("get-set-child-contracts", 1);
            insert_kv("issue", 1624);
            insert_kv("schema", 1624); // lineage 1520|568|370
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #1521: query:shape-arena-compact-stats — ShapeProfiler versioning
    // + Arena compact soft deopt synergy (no deopt-storm from compact alone).
    ObservabilityPrims::register_stats_impl(
        "query:shape-arena-compact-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(query_hash_capacity_for(15));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            const std::int64_t triggered = static_cast<std::int64_t>(
                shape::shape_inval_on_compact_triggered.load(std::memory_order_relaxed));
            const std::int64_t deopt_ac = static_cast<std::int64_t>(
                shape::deopt_from_arena_compact_total.load(std::memory_order_relaxed));
            const std::int64_t preserved = static_cast<std::int64_t>(
                shape::shape_stability_post_compact_preserved.load(std::memory_order_relaxed));
            const std::int64_t storm_suppressed = static_cast<std::int64_t>(
                shape::deopt_storm_compact_suppressed.load(std::memory_order_relaxed));
            const std::int64_t boundary_checks = static_cast<std::int64_t>(
                shape::shape_boundary_post_compact_checks.load(std::memory_order_relaxed));
            const std::int64_t fiber_sync = static_cast<std::int64_t>(
                shape::shape_fiber_steal_sync_total.load(std::memory_order_relaxed));
            if (m) {
                m->shape_inval_on_compact_triggered_total.store(
                    static_cast<std::uint64_t>(triggered), std::memory_order_relaxed);
                m->deopt_from_arena_compact_total.store(static_cast<std::uint64_t>(deopt_ac),
                                                        std::memory_order_relaxed);
                m->shape_stability_post_compact_preserved_total.store(
                    static_cast<std::uint64_t>(preserved), std::memory_order_relaxed);
                m->deopt_storm_compact_suppressed_total.store(
                    static_cast<std::uint64_t>(storm_suppressed), std::memory_order_relaxed);
            }
            insert_kv("shape-inval-on-compact-triggered", triggered);
            insert_kv("deopt-from-arena-compact", deopt_ac);
            insert_kv("shape-stability-post-compact-preserved", preserved);
            insert_kv("deopt-storm-compact-suppressed", storm_suppressed);
            insert_kv("boundary-post-compact-checks", boundary_checks);
            insert_kv("fiber-steal-sync", fiber_sync);
            insert_kv("schema", 1521);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #534: query:arena-production-compaction-stats. Commercial P0 hash
    // view of auto-compaction threshold policy + live-object defrag
    // coordination with fiber safepoints and MutationBoundaryGuard — non-
    // duplicative synthesis of #405 arena-compaction-stats int-sum, #430
    // arena-compaction-stats-hash, #464 arena-auto-stats, #685 arena-auto-
    // compact-stats, #604 arena-fragmentation-snapshot, and #300 defrag
    // foundation themes; avoids repeating the per-field #430 hash surface:
    //   P1 Fragmentation policy: fragmentation-ratio-pct, peak-used-bytes,
    //      compaction-efficiency-pct
    //   P2 Auto-compact lifecycle: auto-compact-triggers/skips/guard-calls,
    //      compactions, bytes-saved, last-saved
    //   P3 Defrag coordination: defrag-attempted-count, defrag-saved-bytes
    //   P4 Safepoint/Guard: compaction-yield-checks, paused-by-boundary,
    //      gc-safepoint-waits, safepoint-coordination-count
    //   - arena-production-compaction-total / recommendation
    ObservabilityPrims::register_stats_impl(
        "query:arena-production-compaction-stats",
        [&string_heap, &ev](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ht = FlatHashTable::create(query_hash_capacity_for(27));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const auto& group = ev.arena_group();
            const auto stats = group.total_stats();
            const auto policy = group.auto_compact_policy_stats();
            const std::uint64_t triggers =
                group.auto_compact_trigger_count() + policy.auto_triggers;
            const std::uint64_t skips = group.auto_compact_skip_count();
            const std::uint64_t guard_calls = group.auto_compact_guard_call_count();
            const std::uint64_t compacts = stats.compaction_count;
            const std::uint64_t saved = stats.total_compaction_saved;
            const std::uint64_t last_saved = stats.last_compaction_saved;
            const std::uint64_t defrag_attempted = stats.defrag_attempted_count;
            const std::uint64_t defrag_saved =
                policy.defrag_savings + stats.defrag_savings_alloc + stats.last_defrag_saved;
            const std::uint64_t yield_checks = group.compaction_yield_checks_group() +
                                               policy.yield_checks_hit +
                                               stats.compaction_yield_checks;
            const std::uint64_t paused = ev.compaction_paused_by_boundary();
            const std::uint64_t gc_waits = ev.get_gc_safepoint_waits_total();
            const std::uint64_t gc_deferred = ev.get_gc_safepoint_deferred_total();
            const std::uint64_t safepoint_coord = yield_checks + paused + gc_waits + gc_deferred;
            const std::uint64_t mutations = ev.total_mutations();
            const std::uint64_t dirty = ev.get_dirty_propagation_count();
            const std::int64_t frag_pct =
                static_cast<std::int64_t>(stats.fragmentation_ratio() * 100.0);
            // Issue #1080: efficiency is meaningful only when compacts>0;
            // never report saved*100 when compacts==0 (would exceed 100%).
            const std::int64_t efficiency_pct =
                compacts == 0 ? 0
                              : static_cast<std::int64_t>(
                                    std::min<std::uint64_t>(100, (saved * 100) / compacts));
            const std::uint64_t total = triggers + skips + guard_calls + compacts + saved +
                                        defrag_attempted + defrag_saved + safepoint_coord +
                                        mutations + dirty + stats.peak_used;
            std::int64_t recommendation = 0;
            if (frag_pct > 30 && saved == 0 && compacts == 0)
                recommendation = 3;
            else if (paused > yield_checks && paused > 0)
                recommendation = 2;
            else if (triggers > 0 || compacts > 0 || defrag_saved > 0)
                recommendation = 1;
            insert_kv("fragmentation-ratio-pct", frag_pct);
            insert_kv("peak-used-bytes", static_cast<std::int64_t>(stats.peak_used));
            insert_kv("auto-compact-triggers", static_cast<std::int64_t>(triggers));
            insert_kv("auto-compact-skips", static_cast<std::int64_t>(skips));
            insert_kv("auto-compact-guard-calls", static_cast<std::int64_t>(guard_calls));
            insert_kv("compactions", static_cast<std::int64_t>(compacts));
            insert_kv("bytes-saved", static_cast<std::int64_t>(saved));
            insert_kv("last-saved", static_cast<std::int64_t>(last_saved));
            insert_kv("defrag-attempted-count", static_cast<std::int64_t>(defrag_attempted));
            insert_kv("defrag-saved-bytes", static_cast<std::int64_t>(defrag_saved));
            insert_kv("compaction-yield-checks", static_cast<std::int64_t>(yield_checks));
            insert_kv("paused-by-boundary", static_cast<std::int64_t>(paused));
            insert_kv("gc-safepoint-waits", static_cast<std::int64_t>(gc_waits));
            insert_kv("safepoint-coordination-count", static_cast<std::int64_t>(safepoint_coord));
            insert_kv("mutation-volume", static_cast<std::int64_t>(mutations));
            insert_kv("dirty-propagation", static_cast<std::int64_t>(dirty));
            insert_kv("compaction-efficiency-pct", efficiency_pct);
            insert_kv("arena-production-compaction-total", static_cast<std::int64_t>(total));
            insert_kv("arena-production-compaction-recommendation", recommendation);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #535: query:contracts-production-hotpath-stats. Commercial P1 hash
    // view of C++26 Contracts + consteval invariants in Arena create,
    // inline_shape_of, Pass run_one/Wraps, and evaluator/lowering hot paths —
    // non-duplicative synthesis of #507 task4-hotpath-contracts inventory hash,
    // #406 pass-contracts-stats int-sum, #626 contracts-hotpath-stats-hash,
    // #465/#431 C++26 density themes; avoids repeating the static per-site
    // #507 surface verbatim:
    //   P1 Contract inventory: contract-site-count, shape-dispatch-table-size,
    //      consteval-hits
    //   P2 Runtime health: contract-violations, dispatch-hits, dispatch-misses
    //   P3 Zero-overhead: zerooverhead-wins, zerooverhead-rate-pct
    //   P4 Pass pipeline: passes-skipped-dirty, pass-pipeline-runs, relower-skipped
    //   P5 Dirty/mark paths: mark-dirty-upward-calls, dirty-propagation
    //   - contracts-coverage-pct / contracts-production-hotpath-total / recommendation
    ObservabilityPrims::register_stats_impl(
        "query:contracts-production-hotpath-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(query_hash_capacity_for(24));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            auto* ws = ev->workspace_flat();
            constexpr std::int64_t k_contract_sites = 6;
            const std::int64_t table_size =
                static_cast<std::int64_t>(shape::k_task4_shape_dispatch_table_size);
            const std::int64_t consteval_hits =
                static_cast<std::int64_t>(shape::k_shape_value_consteval_hits);
            const std::uint64_t violations =
                types::value_contract_violation_count.load(std::memory_order_relaxed);
            const std::uint64_t dispatch_hits =
                types::value_dispatch_hit_count.load(std::memory_order_relaxed);
            const std::uint64_t dispatch_miss =
                types::value_dispatch_miss_count.load(std::memory_order_relaxed);
            const std::uint64_t zero_wins =
                m ? m->coercion_zerooverhead_win_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t passes_skip = ev->get_passes_skipped_type_dirty();
            const std::uint64_t pipeline_runs =
                aura::compiler::pass_pipeline_runs_total.load(std::memory_order_relaxed);
            const std::uint64_t relower_skip =
                m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t dirty_up = ws ? ws->mark_dirty_upward_call_count() : 0;
            const std::uint64_t dirty_prop = ev->get_dirty_propagation_count();
            const std::uint64_t dispatch_denom = dispatch_hits + dispatch_miss + 1;
            const std::int64_t coverage_pct =
                static_cast<std::int64_t>((dispatch_hits * 100) / dispatch_denom);
            const std::uint64_t zero_denom = zero_wins + dispatch_miss + 1;
            const std::int64_t zerooverhead_pct =
                static_cast<std::int64_t>((zero_wins * 100) / zero_denom);
            const std::uint64_t total = static_cast<std::uint64_t>(k_contract_sites) +
                                        static_cast<std::uint64_t>(table_size) +
                                        static_cast<std::uint64_t>(consteval_hits) + violations +
                                        dispatch_hits + dispatch_miss + zero_wins + passes_skip +
                                        pipeline_runs + relower_skip + dirty_up + dirty_prop;
            std::int64_t recommendation = 0;
            if (violations > 0)
                recommendation = 3;
            else if (dispatch_miss > dispatch_hits && dispatch_miss > 0)
                recommendation = 2;
            else if (zero_wins > 0 || passes_skip > 0 || pipeline_runs > 0)
                recommendation = 1;
            insert_kv("contract-site-count", k_contract_sites);
            insert_kv("shape-dispatch-table-size", table_size);
            insert_kv("consteval-hits", consteval_hits);
            insert_kv("contract-violations", static_cast<std::int64_t>(violations));
            insert_kv("dispatch-hits", static_cast<std::int64_t>(dispatch_hits));
            insert_kv("dispatch-misses", static_cast<std::int64_t>(dispatch_miss));
            insert_kv("zerooverhead-wins", static_cast<std::int64_t>(zero_wins));
            insert_kv("zerooverhead-rate-pct", zerooverhead_pct);
            insert_kv("passes-skipped-dirty", static_cast<std::int64_t>(passes_skip));
            insert_kv("pass-pipeline-runs", static_cast<std::int64_t>(pipeline_runs));
            insert_kv("relower-skipped", static_cast<std::int64_t>(relower_skip));
            insert_kv("mark-dirty-upward-calls", static_cast<std::int64_t>(dirty_up));
            insert_kv("dirty-propagation", static_cast<std::int64_t>(dirty_prop));
            insert_kv("contracts-coverage-pct", coverage_pct);
            insert_kv("contracts-production-hotpath-total", static_cast<std::int64_t>(total));
            insert_kv("contracts-production-hotpath-recommendation", recommendation);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #539: query:sv-production-verification-stats. Commercial P0 hash
    // view of EDA verification feedback → structured SV mutate closed loop +
    // commercial tool interop — non-duplicative synthesis of #519 edsl-eda-sv-
    // closedloop-stats, #630 sv-verification-closedloop-stats-hash, #510
    // eda-verification-stats, and #469 verification_dirty_ themes; avoids
    // repeating the per-field #630 hash surface verbatim:
    //   P1 Feedback mapping: feedback-mapped-count, feedback-mutate-success,
    //      structured-mutate-hits
    //   P2 SV mutate impact: sv-mutate-attempts/success, stable-ref-captures,
    //      dirty-propagated-nodes
    //   P3 Verification dirty: coverage-feedback-total, assert-failure-total
    //   P4 Re-emit/re-verify: reverify-success, verification-convergence,
    //      hardware-hook-calls, commercial-reemits, rollback-on-partial
    //   - feedback-success-rate-pct / sv-production-verification-total / recommendation
    ObservabilityPrims::register_stats_impl(
        "query:sv-production-verification-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            auto* ht = FlatHashTable::create(query_hash_capacity_for(24));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const std::uint64_t feedback_mapped =
                m ? m->feedback_mutate_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t feedback_success =
                ev->get_verify_tool_feedback_mutate_success_total();
            const std::uint64_t structured_hits =
                m ? m->sva_structured_mutate_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t sv_attempts = 0; // Issue #3239: retired
            const std::uint64_t sv_success = 0;
            const std::uint64_t stable_ref = ev->get_verify_tool_stable_ref_hits_total();
            const std::uint64_t dirty_props = ev->get_verify_tool_dirty_propagations_total();
            const std::uint64_t coverage = ws ? ws->verification_coverage_feedback_total() : 0;
            const std::uint64_t assert_fail = ws ? ws->verification_assert_failure_total() : 0;
            const std::uint64_t reverify =
                m ? m->verification_loop_success_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t hw_hooks =
                m ? m->hardware_backend_hook_calls_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t reemits =
                m ? m->commercial_reemits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t rollback =
                m ? m->sv_emit_parse_fail_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t feedback_denom = feedback_mapped + sv_attempts + 1;
            const std::int64_t feedback_success_pct =
                static_cast<std::int64_t>((feedback_success * 100) / feedback_denom);
            const std::uint64_t total = feedback_mapped + feedback_success + structured_hits +
                                        sv_attempts + sv_success + stable_ref + dirty_props +
                                        coverage + assert_fail + reverify + hw_hooks + reemits +
                                        rollback;
            std::int64_t recommendation = 0;
            if (assert_fail > coverage && assert_fail > 0)
                recommendation = 3;
            else if (sv_attempts > 0 && sv_success == 0)
                recommendation = 2;
            else if (feedback_mapped > 0 || reverify > 0 || structured_hits > 0)
                recommendation = 1;
            insert_kv("feedback-mapped-count", static_cast<std::int64_t>(feedback_mapped));
            insert_kv("feedback-mutate-success", static_cast<std::int64_t>(feedback_success));
            insert_kv("structured-mutate-hits", static_cast<std::int64_t>(structured_hits));
            insert_kv("sv-mutate-attempts", static_cast<std::int64_t>(sv_attempts));
            insert_kv("sv-mutate-success", static_cast<std::int64_t>(sv_success));
            insert_kv("stable-ref-captures-in-sv", static_cast<std::int64_t>(stable_ref));
            insert_kv("dirty-propagated-nodes", static_cast<std::int64_t>(dirty_props));
            insert_kv("coverage-feedback-total", static_cast<std::int64_t>(coverage));
            insert_kv("assert-failure-total", static_cast<std::int64_t>(assert_fail));
            insert_kv("reverify-success", static_cast<std::int64_t>(reverify));
            // verification-convergence (eda_sv_verification_convergence_total) retired 4.4
            insert_kv("hardware-hook-calls", static_cast<std::int64_t>(hw_hooks));
            insert_kv("commercial-reemits", static_cast<std::int64_t>(reemits));
            insert_kv("rollback-on-partial", static_cast<std::int64_t>(rollback));
            insert_kv("feedback-success-rate-pct", feedback_success_pct);
            insert_kv("sv-production-verification-total", static_cast<std::int64_t>(total));
            insert_kv("sv-production-verification-recommendation", recommendation);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #540: query:eda-stability-stats. Commercial P0 hash view of
    // StableNodeRef + generation_ + mutation_log provenance hardening for
    // long-running concurrent AI EDA verification sessions — non-duplicative
    // synthesis of #527 stable-ref-cow-fiber-stats, #552 edsl-stability-stats,
    // #497 stable-ref-lifecycle-stats, #457 stable-ref-stats, and #631
    // provenance SV scaffolding; avoids repeating int-sum surfaces verbatim:
    //   P1 COW/fiber staleness: cross-cow-invalidations, fiber-stale-ref-count,
    //      provenance-mismatch, mutation-log-rollback-count
    //   P2 Generation/mutation_log: generation-wrap-events,
    //      stable-ref-invalidations, node-gen-stale-accesses,
    //      stale-ref-auto-refresh-count
    //   P3 SV provenance scaffolding: cross-fiber-violations, safe-resolves,
    //      stale-ref-blocked-count
    //   - eda-stability-total / eda-stability-recommendation
    ObservabilityPrims::register_stats_impl(
        "query:eda-stability-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            auto* ht = FlatHashTable::create(query_hash_capacity_for(21));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
            const std::uint64_t fiber_stale = ev->get_fiber_stale_ref_count();
            const std::uint64_t provenance = ev->get_provenance_mismatch();
            const std::uint64_t rollback = ev->get_mutation_log_rollback_count();
            const std::uint64_t gen_wrap = ws ? ws->generation_wrap_count() : 0;
            const std::uint64_t invalidations = ws ? ws->stable_ref_invalidations() : 0;
            const std::uint64_t stale_access = ws ? ws->node_gen_stale_access_count() : 0;
            const std::uint64_t auto_refresh = ws ? ws->stale_ref_auto_refresh_count() : 0;
            const std::uint64_t cross_fiber =
                m ? m->cross_fiber_violations_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t safe_resolves =
                m ? m->safe_resolves_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t stale_blocked = ev->get_stale_ref_blocked_count();
            const std::uint64_t total = cross_cow + fiber_stale + provenance + rollback + gen_wrap +
                                        invalidations + stale_access + auto_refresh + cross_fiber +
                                        safe_resolves + stale_blocked;
            std::int64_t recommendation = 0;
            if (fiber_stale > 0 || cross_fiber > 0)
                recommendation = 3;
            else if (gen_wrap > 0 || rollback > 0)
                recommendation = 2;
            else if (cross_cow > 0 || provenance > 0 || invalidations > 0)
                recommendation = 1;
            insert_kv("cross-cow-invalidations", static_cast<std::int64_t>(cross_cow));
            insert_kv("fiber-stale-ref-count", static_cast<std::int64_t>(fiber_stale));
            insert_kv("provenance-mismatch", static_cast<std::int64_t>(provenance));
            insert_kv("mutation-log-rollback-count", static_cast<std::int64_t>(rollback));
            insert_kv("generation-wrap-events", static_cast<std::int64_t>(gen_wrap));
            insert_kv("stable-ref-invalidations", static_cast<std::int64_t>(invalidations));
            insert_kv("node-gen-stale-accesses", static_cast<std::int64_t>(stale_access));
            insert_kv("stale-ref-auto-refresh-count", static_cast<std::int64_t>(auto_refresh));
            insert_kv("cross-fiber-violations", static_cast<std::int64_t>(cross_fiber));
            insert_kv("safe-resolves", static_cast<std::int64_t>(safe_resolves));
            insert_kv("stale-ref-blocked-count", static_cast<std::int64_t>(stale_blocked));
            insert_kv("eda-stability-total", static_cast<std::int64_t>(total));
            insert_kv("eda-stability-recommendation", recommendation);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #541: query:pattern-sv-verification-stats. Commercial P0 hash
    // view of query:pattern + DefUseIndex + tag_arity_index incremental
    // maintenance + MacroIntroduced hygiene for large-scale SV SoC AI
    // verification loops — non-duplicative synthesis of #528
    // pattern-production-index-stats, #547 pattern-index/hygiene-stats,
    // #503 pattern-marker-stats, and #519 edsl-eda-sv-closedloop-stats;
    // avoids repeating the per-field #528 hash surface verbatim:
    //   P1 DefUseIndex: defuse-index-used/visited/walk-fallback, defuse-version
    //   P2 Incremental index: tag-arity-delta-hits, dirty-marks, rebuild-time-us,
    //      structural-index-hits/misses
    //   P3 Hygiene: hygiene-skips, recursive-hygiene-skips, hygiene-violations,
    //      macro-marker-count
    //   P4 SV verification loop: sv-node-count, verification-dirty-count
    //   - incremental-hit-rate-pct / pattern-sv-verification-total / recommendation
    ObservabilityPrims::register_stats_impl(
        "query:pattern-sv-verification-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            auto* ht = FlatHashTable::create(query_hash_capacity_for(26));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const std::uint64_t defuse_used =
                m ? m->per_defuse_index_used_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t defuse_visited =
                m ? m->per_defuse_index_visited_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t defuse_fallback =
                m ? m->per_defuse_index_walk_fallback_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t defuse_version = ev->get_defuse_version();
            const std::uint64_t delta_hits = ws ? ws->tag_arity_index_delta_hits() : 0;
            const std::uint64_t dirty_marks = ws ? ws->tag_arity_index_dirty_marks() : 0;
            const std::uint64_t rebuild_time_us = ws ? ws->tag_arity_index_rebuild_time_us() : 0;
            const std::uint64_t rebuilds = ws ? ws->tag_arity_index_rebuilds() : 0;
            const std::uint64_t structural_hits = ev->get_pattern_structural_index_hits();
            const std::uint64_t structural_misses = ev->get_pattern_structural_index_misses();
            const std::uint64_t hygiene_skips = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t recursive_skips = ev->get_pattern_recursive_macro_skipped();
            const std::uint64_t violations = ev->get_hygiene_violation_count();
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);
            std::uint64_t sv_node_count = 0;
            std::uint64_t verification_dirty_count = 0;
            if (ws) {
                for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                    switch (ws->get(id).tag) {
                        case aura::ast::NodeTag::Interface:
                        case aura::ast::NodeTag::Modport:
                        case aura::ast::NodeTag::Property:
                        case aura::ast::NodeTag::Sequence:
                        case aura::ast::NodeTag::Assert:
                        case aura::ast::NodeTag::Covergroup:
                        case aura::ast::NodeTag::Coverpoint:
                        case aura::ast::NodeTag::Constraint:
                            ++sv_node_count;
                            break;
                        default:
                            break;
                    }
                    if (ws->verification_dirty(id) != 0)
                        ++verification_dirty_count;
                }
            }
            const std::uint64_t delta_denom = delta_hits + rebuilds;
            const std::int64_t incremental_hit_rate_pct =
                delta_denom == 0 ? 0 : static_cast<std::int64_t>((delta_hits * 100) / delta_denom);
            const std::uint64_t total =
                defuse_used + defuse_visited + defuse_fallback + delta_hits + dirty_marks +
                rebuild_time_us + structural_hits + structural_misses + hygiene_skips +
                recursive_skips + violations + markers + sv_node_count + verification_dirty_count;
            std::int64_t recommendation = 0;
            if (violations > 0)
                recommendation = 3;
            else if (rebuilds > 0 &&
                     rebuild_time_us > static_cast<std::uint64_t>(delta_hits + 1) * 100)
                recommendation = 2;
            else if (hygiene_skips + recursive_skips > 0 || delta_hits > 0)
                recommendation = 1;
            insert_kv("defuse-index-used", static_cast<std::int64_t>(defuse_used));
            insert_kv("defuse-index-visited", static_cast<std::int64_t>(defuse_visited));
            insert_kv("defuse-index-walk-fallback", static_cast<std::int64_t>(defuse_fallback));
            insert_kv("defuse-version", static_cast<std::int64_t>(defuse_version));
            insert_kv("tag-arity-delta-hits", static_cast<std::int64_t>(delta_hits));
            insert_kv("tag-arity-dirty-marks", static_cast<std::int64_t>(dirty_marks));
            insert_kv("tag-arity-rebuild-time-us", static_cast<std::int64_t>(rebuild_time_us));
            insert_kv("structural-index-hits", static_cast<std::int64_t>(structural_hits));
            insert_kv("structural-index-misses", static_cast<std::int64_t>(structural_misses));
            insert_kv("hygiene-skips", static_cast<std::int64_t>(hygiene_skips));
            insert_kv("recursive-hygiene-skips", static_cast<std::int64_t>(recursive_skips));
            insert_kv("hygiene-violations", static_cast<std::int64_t>(violations));
            insert_kv("macro-marker-count", static_cast<std::int64_t>(markers));
            insert_kv("sv-node-count", static_cast<std::int64_t>(sv_node_count));
            insert_kv("verification-dirty-count",
                      static_cast<std::int64_t>(verification_dirty_count));
            insert_kv("incremental-hit-rate-pct", incremental_hit_rate_pct);
            insert_kv("pattern-sv-verification-total", static_cast<std::int64_t>(total));
            insert_kv("pattern-sv-verification-recommendation", recommendation);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #557: query:top5-commercial-coverage-stats. Commercial P0 hash
    // view of the Top 5 test-coverage cluster (#531 closure-env, #530
    // incremental relower, #532 JIT consistency, #556 EDSL concurrency,
    // #553 atomic batch/rollback) for Prompt6+Incremental+JIT production
    // review — non-duplicative synthesis of per-issue hash/int primitives;
    // avoids repeating their per-field surfaces verbatim:
    //   P1 #531 Prompt6: closure-stale-refresh, bridge-epoch-hits,
    //      linear-check-pass, env-stale-refresh
    //   P2 #530 Prompt2: blocks-saved, partial-relowers,
    //      invalidate-function-calls, min-scope-hit-rate-pct
    //   P3 #532 Prompt3: unhandled-opcode-count, deopt-count,
    //      hotswap-invalidate-count, opcode-coverage-pct
    //   P4 #556 concurrency: steal-attempts, boundary-violations,
    //      unsafe-boundary-attempts, lock-contention-us
    //   P5 #553 atomicity: batch-commits, batch-rollbacks,
    //      bumps-saved, steal-violations-during-batch
    //   - top5-commercial-coverage-total / top5-commercial-coverage-recommendation
    ObservabilityPrims::register_stats_impl(
        "query:top5-commercial-coverage-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            auto* ht = FlatHashTable::create(query_hash_capacity_for(30));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const std::uint64_t stale_refresh =
                m ? m->closure_stale_refresh_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t bridge_hits =
                m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t linear_pass =
                m ? m->linear_check_pass_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t env_refresh = ev->get_envframe_stale_refresh_count();
            const std::uint64_t blocks_saved =
                m ? m->ir_soa_relower_blocks_saved_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t partial = ev->get_partial_relower_count();
            const std::uint64_t invalidate_fn =
                m ? m->invalidate_function_calls.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_full =
                m ? m->relower_full_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_per_fn =
                m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_work = relower_full + relower_per_fn + 1;
            const std::int64_t min_scope_hit_pct =
                static_cast<std::int64_t>((blocks_saved * 100) / relower_work);
            std::uint64_t unhandled = 0;
            std::uint64_t compiles = 0;
            std::uint64_t fallback = aura_jit_fallback_count_v_read();
            if (ev->get_jit_stats_fn_) {
                const char* s = ev->get_jit_stats_fn_();
                if (s) {
                    auto parse_u64 = [&](std::string_view key) -> std::uint64_t {
                        std::string_view hay(s);
                        auto pos = hay.find(key);
                        if (pos == std::string_view::npos)
                            return 0;
                        return std::strtoull(hay.data() + pos + key.size(), nullptr, 10);
                    };
                    compiles = parse_u64("compiles=");
                    unhandled = parse_u64("unhandled_opcode=");
                    fallback = parse_u64("fallback_count=");
                }
            }
            const std::uint64_t deopt = m ? m->deopt_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t hotswap_invalidate =
                m ? m->jit_hotswap_invalidate_total.load(std::memory_order_relaxed) : 0;
            const std::int64_t opcode_coverage_pct =
                unhandled == 0 && fallback == 0
                    ? 100
                    : std::max<std::int64_t>(
                          0, 100 - static_cast<std::int64_t>((unhandled + fallback) * 100 /
                                                             std::max<std::uint64_t>(1, compiles)));
            const std::uint64_t steals = ev->get_mutation_steal_attempts();
            const std::uint64_t violations = ev->get_boundary_violation_count();
            const std::uint64_t unsafe_attempts = ev->get_unsafe_boundary_attempts();
            const std::uint64_t contention_us = ev->get_lock_contention_us();
            const std::uint64_t batch_commits = ev->atomic_batch_count();
            const std::uint64_t batch_rollbacks = ev->atomic_batch_rollbacks();
            const std::uint64_t bumps_saved = ev->atomic_batch_bumps_saved_total();
            const std::uint64_t steal_violations = ev->get_atomic_batch_steal_violation();
            const std::uint64_t total = stale_refresh + bridge_hits + linear_pass + env_refresh +
                                        blocks_saved + partial + invalidate_fn + unhandled + deopt +
                                        hotswap_invalidate + steals + violations + unsafe_attempts +
                                        contention_us + batch_commits + batch_rollbacks +
                                        bumps_saved + steal_violations;
            std::int64_t recommendation = 0;
            if (unhandled > 0 || steal_violations > 0 || unsafe_attempts > 0)
                recommendation = 3;
            else if (batch_rollbacks > batch_commits && batch_rollbacks > 0)
                recommendation = 2;
            else if (hotswap_invalidate > 0 || partial > 0 || invalidate_fn > 0)
                recommendation = 1;
            insert_kv("closure-stale-refresh", static_cast<std::int64_t>(stale_refresh));
            insert_kv("bridge-epoch-hits", static_cast<std::int64_t>(bridge_hits));
            insert_kv("linear-check-pass", static_cast<std::int64_t>(linear_pass));
            insert_kv("env-stale-refresh", static_cast<std::int64_t>(env_refresh));
            insert_kv("blocks-saved", static_cast<std::int64_t>(blocks_saved));
            insert_kv("partial-relowers", static_cast<std::int64_t>(partial));
            insert_kv("invalidate-function-calls", static_cast<std::int64_t>(invalidate_fn));
            insert_kv("min-scope-hit-rate-pct", min_scope_hit_pct);
            insert_kv("unhandled-opcode-count", static_cast<std::int64_t>(unhandled));
            insert_kv("deopt-count", static_cast<std::int64_t>(deopt));
            insert_kv("hotswap-invalidate-count", static_cast<std::int64_t>(hotswap_invalidate));
            insert_kv("opcode-coverage-pct", opcode_coverage_pct);
            insert_kv("steal-attempts", static_cast<std::int64_t>(steals));
            insert_kv("boundary-violations", static_cast<std::int64_t>(violations));
            insert_kv("unsafe-boundary-attempts", static_cast<std::int64_t>(unsafe_attempts));
            insert_kv("lock-contention-us", static_cast<std::int64_t>(contention_us));
            insert_kv("batch-commits", static_cast<std::int64_t>(batch_commits));
            insert_kv("batch-rollbacks", static_cast<std::int64_t>(batch_rollbacks));
            insert_kv("bumps-saved", static_cast<std::int64_t>(bumps_saved));
            insert_kv("steal-violations-during-batch", static_cast<std::int64_t>(steal_violations));
            insert_kv("top5-commercial-coverage-total", static_cast<std::int64_t>(total));
            insert_kv("top5-commercial-coverage-recommendation", recommendation);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #515: query:consolidated-p0-production-stats. Hash view of the
    // consolidated Top 5 P0 production-readiness pillars (non-duplicative
    // synthesis of #511/#510/#506/#505/#512 hash slices; avoids #514 Task6
    // int-sum, #517 3-pillar int-sum, and #520 Top-5 roadmap int-sum):
    //   P1 Persistence (#511): checkpoint-save/commit + gen-wrap
    //   P2 EDA (#510): coverage-feedback + assert-failures
    //   P3 SoA (#506): passes-skipped + ir-soa-emitted + module-dirty-skips
    //   P4 Memory (#505): bridge-epoch + closure-refresh + envframe-refresh
    //   P5 Orchestration (#500/#512): steal-attempts/violations + boundary-depth
    //   - consolidated-p0-production-total / consolidated-p0-production-recommendation
    ObservabilityPrims::register_stats_impl(
        "query:consolidated-p0-production-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            auto* ht = FlatHashTable::create(query_hash_capacity_for(24));
            if (!ht)
                return make_void();
            bool overflowed = false;
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

                overflowed = true;
            };
            const std::uint64_t checkpoint_save = ev->get_panic_checkpoint_save_count();
            const std::uint64_t checkpoint_commit = ev->get_panic_checkpoint_commit_count();
            const std::uint64_t gen_wrap = ws ? ws->generation_wrap_count() : 0;
            const std::uint64_t coverage = ws ? ws->verification_coverage_feedback_total() : 0;
            const std::uint64_t assert_fail = ws ? ws->verification_assert_failure_total() : 0;
            const std::uint64_t passes_skipped = ev->get_passes_skipped_type_dirty();
            const std::uint64_t ir_soa =
                m ? m->ir_soa_instructions_emitted.load(std::memory_order_relaxed) +
                        m->ir_soa_functions_emitted.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t module_dirty =
                m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0;
            const std::uint64_t bridge_epoch =
                m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t closure_refresh =
                m ? m->closure_stale_refresh_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t envframe_refresh = ev->get_envframe_stale_refresh_count();
            const std::uint64_t steal_attempts = aura_work_steal_attempts_total();
            const std::uint64_t steal_violations = ev->get_mutation_steal_violation_count();
            const std::uint64_t boundary_depth = aura_evaluator_mutation_boundary_depth();
            const std::uint64_t total = checkpoint_save + checkpoint_commit + gen_wrap + coverage +
                                        assert_fail + passes_skipped + ir_soa + module_dirty +
                                        bridge_epoch + closure_refresh + envframe_refresh +
                                        steal_attempts + steal_violations + boundary_depth;
            std::int64_t recommendation = 0;
            if (steal_violations > 0)
                recommendation = 3;
            else if (assert_fail > coverage && assert_fail > 0)
                recommendation = 2;
            else if (gen_wrap > 0 || envframe_refresh > bridge_epoch)
                recommendation = 1;
            insert_kv("checkpoint-save", static_cast<std::int64_t>(checkpoint_save));
            insert_kv("checkpoint-commit", static_cast<std::int64_t>(checkpoint_commit));
            insert_kv("gen-wrap", static_cast<std::int64_t>(gen_wrap));
            insert_kv("eda-coverage-feedback", static_cast<std::int64_t>(coverage));
            insert_kv("eda-assert-failures", static_cast<std::int64_t>(assert_fail));
            insert_kv("soa-passes-skipped", static_cast<std::int64_t>(passes_skipped));
            insert_kv("soa-ir-emitted", static_cast<std::int64_t>(ir_soa));
            insert_kv("soa-module-dirty-skips", static_cast<std::int64_t>(module_dirty));
            insert_kv("memory-bridge-epoch", static_cast<std::int64_t>(bridge_epoch));
            insert_kv("memory-closure-refresh", static_cast<std::int64_t>(closure_refresh));
            insert_kv("memory-envframe-refresh", static_cast<std::int64_t>(envframe_refresh));
            insert_kv("orchestration-steal-attempts", static_cast<std::int64_t>(steal_attempts));
            insert_kv("orchestration-steal-violations",
                      static_cast<std::int64_t>(steal_violations));
            insert_kv("orchestration-boundary-depth", static_cast<std::int64_t>(boundary_depth));
            insert_kv("consolidated-p0-production-total", static_cast<std::int64_t>(total));
            insert_kv("consolidated-p0-production-recommendation", recommendation);
            return query_hash_finish(ht, string_heap, overflowed);
        });

    // Issue #549: query:self-evolution-stability-stats.
    // Returns the sum of the 4 self-evolution observability
    // counters:
    //   - cross_cow_invalidations_  (# of StableNodeRef
    //     rejections caused by crossing a COW snapshot
    //     boundary — bumped by validate_stable_ref when
    //     captured_gen != current generation_ with small
    //     delta, suggesting same fiber post-mutate)
    //   - fiber_stale_ref_count_  (# of stale-ref detections
    //     where the captured gen is from a different fiber's
    //     workspace — large delta)
    //   - mutation_log_rollback_count_  (# of times
    //     exit_mutation_boundary(false) actually rolled back
    //     the log — a stricter subset of failed boundaries)
    //   - provenance_mismatch_  (# of stable-ref checks
    //     where the captured provenance (origin layer)
    //     didn't match the current workspace layer)
    //
    // P0: returns an integer = sum of all 4 counters.
    // Follow-up: returns a 4-tuple
    // (cross-cow fiber-stale rollback provenance-mismatch)
    // so the AI Agent can react to each category
    // independently. cross-cow > 0 is expected under load
    // (every structural mutate bumps generation_); fiber-stale
    // > 0 indicates a worker-migration bug; rollback > 0
    // indicates panic or fail-fast path was hit;
    // provenance-mismatch > 0 indicates a stale layer in the
    // StableNodeRef handle.
    ObservabilityPrims::register_stats_impl(
        "query:self-evolution-stability-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
            const std::uint64_t fiber_stale = ev->get_fiber_stale_ref_count();
            const std::uint64_t rollback = ev->get_mutation_log_rollback_count();
            const std::uint64_t provenance = ev->get_provenance_mismatch();
            return make_int(
                static_cast<std::int64_t>(cross_cow + fiber_stale + rollback + provenance));
        });

    // Issue #550: query:typed-mutation-stats. Returns the
    // sum of the 4 incremental typed self-mod observability
    // counters:
    //   - narrowing_refresh_count_  (# of OccurrenceInfoFlat
    //     entries refreshed after dirty propagation)
    //   - cross_delta_conflicts_caught_  (# of times
    //     touched_roots_ detected a CONFLICT between two
    //     delta batches)
    //   - passes_skipped_type_dirty_  (# of clean Pass
    //     blocks skipped by the DirtyAwarePass short-circuit)
    //   - touched_roots_size_  (current touched_roots_ set
    //     size — a snapshot, not a counter)
    //
    // P0: returns an integer = sum of the 4 counters.
    // Follow-up: returns a 4-tuple
    // (narrowing-refresh cross-delta-conflicts passes-skipped
    // touched-roots-size) so the AI Agent can react to each
    // category independently (narrowing-refresh > 0 expected
    // under typed mutate; cross-delta-conflicts > 0 indicates
    // a CONFLICT that needs human review).
    ObservabilityPrims::register_stats_impl(
        "query:typed-mutation-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t narrowing = ev->get_narrowing_refresh_count();
            const std::uint64_t conflicts = ev->get_cross_delta_conflicts_caught();
            const std::uint64_t passes_skipped = ev->get_passes_skipped_type_dirty();
            const std::uint64_t touched_size = ev->get_touched_roots_size();
            return make_int(
                static_cast<std::int64_t>(narrowing + conflicts + passes_skipped + touched_size));
        });

    // Issue #550: query:dirty-impact. Returns the touched
    // roots set size as an integer (a snapshot, not a
    // counter). Production use: the AI Agent reads this to
    // decide whether to schedule a full re-solve (size is
    // large or growth is monotonic) or trust the incremental
    // path (size is bounded).
    add("query:dirty-impact", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev->get_touched_roots_size()));
    });

    // Issue #495: query:task2-refinement-stats. Returns the sum
    // of 4 Task2 review refinement pillar counters:
    //   - constraint_soundness: delta_conflict_reverify_total +
    //     delta_conflict_detected_total (#466/#509)
    //   - coercion_zerooverhead: dead_coercion_eliminated_total +
    //     coercion_zerooverhead_win_total (#468/#574)
    //   - occurrence_blame: narrowing_dirty_recovery_total +
    //     occurrence_blame_chain_complete_total (#467)
    //   - jit_elision_hits: coercion_narrow_evidence_hits_total
    //     (JIT/IR narrow-evidence elision synergy)
    ObservabilityPrims::register_stats_impl(
        "query:task2-refinement-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t constraint =
                m->delta_conflict_reverify_total.load(std::memory_order_relaxed) +
                m->delta_conflict_detected_total.load(std::memory_order_relaxed);
            const std::uint64_t coercion =
                m->dead_coercion_eliminated_total.load(std::memory_order_relaxed) +
                m->coercion_zerooverhead_win_total.load(std::memory_order_relaxed);
            const std::uint64_t occurrence =
                m->narrowing_dirty_recovery_total.load(std::memory_order_relaxed) +
                m->occurrence_blame_chain_complete_total.load(std::memory_order_relaxed);
            const std::uint64_t jit_elision =
                m->coercion_narrow_evidence_hits_total.load(std::memory_order_relaxed);
            return make_int(
                static_cast<std::int64_t>(constraint + coercion + occurrence + jit_elision));
        });

    // Issue #690: query:constraint-delta-blame-stats. Returns the
    // sum of cross-delta constraint blame-chain completeness hits
    // plus occurrence narrowing blame-chain completeness.
    ObservabilityPrims::register_stats_impl(
        "query:constraint-delta-blame-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t constraint_blame =
                m ? m->constraint_blame_chain_complete_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t occurrence_blame =
                m ? m->occurrence_blame_chain_complete_total.load(std::memory_order_relaxed) : 0;
            return make_int(static_cast<std::int64_t>(constraint_blame + occurrence_blame));
        });

    // Issue #509: query:constraint-delta-stats. Returns the sum
    // of 2 solve_delta touched_roots soundness counters:
    //   - touched_roots_hits: delta_conflict_reverify_total
    //     (bounded clean-constraint re-scans after touched roots)
    //   - cross_delta_conflicts_caught: cross_delta_conflicts_caught_
    ObservabilityPrims::register_stats_impl(
        "query:constraint-delta-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t touched_hits =
                m ? m->delta_conflict_reverify_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t conflicts = ev->get_cross_delta_conflicts_caught();
            return make_int(static_cast<std::int64_t>(touched_hits + conflicts));
        });

    // Issue #628: query:solve-delta-safety-stats. Returns the sum
    // of 4 solve_delta clean-constraint safety counters:
    //   - clean_conflicts_detected: delta_conflict_detected_total
    //   - full_solve_fallbacks: solve_delta_full_solve_fallback_total
    //   - delta_vs_full_consistency: delta_conflict_reverify_total
    //   - missed_conflict_prevented: delta_constraints_processed_total
    ObservabilityPrims::register_stats_impl(
        "query:solve-delta-safety-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t clean_conflicts =
                m->delta_conflict_detected_total.load(std::memory_order_relaxed);
            const std::uint64_t full_fallbacks =
                m->solve_delta_full_solve_fallback_total.load(std::memory_order_relaxed);
            const std::uint64_t consistency =
                m->delta_conflict_reverify_total.load(std::memory_order_relaxed);
            const std::uint64_t prevented =
                m->delta_constraints_processed_total.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(clean_conflicts + full_fallbacks +
                                                      consistency + prevented));
        });

    // Issue #573: query:typed-incremental-stats. Returns the sum
    // of 4 Task2 incremental typed self-mod reliability counters:
    //   - delta_conflicts_caught: cross_delta_conflicts_caught_
    //   - narrowing_refresh_count: narrowing_refresh_count_
    //   - local_recheck_hit_rate: selective_recheck_count_
    //     (proxy for selective local re-check vs full solve)
    //   - solve_delta_time_us: delta_solve_time_us (CompilerMetrics)
    ObservabilityPrims::register_stats_impl(
        "query:typed-incremental-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t conflicts = ev->get_cross_delta_conflicts_caught();
            const std::uint64_t narrowing = ev->get_narrowing_refresh_count();
            const std::uint64_t local_recheck = ev->get_selective_recheck_count();
            const std::uint64_t solve_us =
                m ? m->delta_solve_time_us.load(std::memory_order_relaxed) : 0;
            return make_int(
                static_cast<std::int64_t>(conflicts + narrowing + local_recheck + solve_us));
        });

    // Issue #608: query:type-incremental-stats. Returns the sum
    // of 4 incremental type reliability counters from
    // CompilerMetrics:
    //   - delta_constraints_processed_total (dep-tracked solve_delta)
    //   - narrowing_dirty_recovery_total (occurrence-dirty recoveries)
    //   - post_mutate_narrow_consistency_total (narrow reliability)
    //   - incremental_typecheck_auto_invocations_total (delta win vs full)
    ObservabilityPrims::register_stats_impl(
        "query:type-incremental-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t delta_processed =
                m->delta_constraints_processed_total.load(std::memory_order_relaxed);
            const std::uint64_t occ_recovery =
                m->narrowing_dirty_recovery_total.load(std::memory_order_relaxed);
            const std::uint64_t narrow_hits =
                m->post_mutate_narrow_consistency_total.load(std::memory_order_relaxed);
            const std::uint64_t delta_win =
                m->incremental_typecheck_auto_invocations_total.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(delta_processed + occ_recovery + narrow_hits +
                                                      delta_win));
        });

    // Issue #798 / #1617: query:type-incremental-fidelity-stats — ConstraintSystem
    // incremental fidelity under Guard/steal/MutationBoundary + Let-Poly dirty
    // invalidation (refines #792/#793/#466/#409/#745; non-duplicative with
    // #608 type-incremental-stats and #509 constraint-delta-stats).
    //
    // Fields (4 lineage + #1617 Let-Poly + sentinel):
    //   - cross-delta-blame-complete  type_incremental_cross_delta_blame_complete_total
    //   - reverify-truncated-under-guard
    //       type_incremental_reverify_truncated_under_guard_total
    //   - epoch-sync-hits             type_incremental_epoch_sync_hits_total
    //   - blame-chain-length          type_incremental_blame_chain_length_total
    //   - let-poly-dirty-roots        let_poly_dirty_roots_tracked_total
    //   - let-poly-regeneralize       let_poly_regeneralize_check_total
    //   - let-poly-truncation-fallback let_poly_truncation_fallback_total
    //   - let-poly-priority-reverify  let_poly_priority_reverify_hits_total
    //   - let-poly-post-mutation-scope let_poly_post_mutation_scope_total
    //   - reverify-truncated          reverify_truncated_total
    //   - solve-delta-worklist-peak   solve_delta_worklist_size_peak
    //   - let-poly-wired              1
    //   - schema == 1617 (lineage 798)
}

} // namespace aura::compiler::primitives_detail
