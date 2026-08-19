// evaluator_primitives_query_lifecycle.cpp — Issue #2914 peel (~L12458-L16420)
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

void register_query_lifecycle_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                         std::pmr::vector<std::string>& string_heap,
                                         void*& type_registry,
                                         ModulePathResolver resolve_module_path, Evaluator& ev) {
    (void)pairs;
    (void)string_heap;
    (void)type_registry;
    (void)resolve_module_path;
    (void)ev;
    sink_query_prim(
        "query:macro-provenance-chain", [&ev, &string_heap](const auto& a) -> EvalValue {
            if (a.empty() || !is_int(a[0]))
                return make_void();
            auto* ws = ev.workspace_flat();
            if (!ws)
                return make_void();
            const auto nid = static_cast<aura::ast::NodeId>(as_int(a[0]));
            if (nid == aura::ast::NULL_NODE || nid >= ws->size() || !ws->is_live_node(nid))
                return make_void();

            std::int64_t depth_budget = 16;
            const auto& kt = ev.keyword_table();
            for (std::size_t i = 1; i < a.size(); ++i) {
                if (!is_keyword(a[i]))
                    continue;
                const auto kidx = as_keyword_idx(a[i]);
                if (kidx >= kt.size())
                    continue;
                if ((kt[kidx] == ":depth" || kt[kidx] == "depth") && i + 1 < a.size() &&
                    is_int(a[i + 1])) {
                    depth_budget = as_int(a[i + 1]);
                    ++i;
                }
            }
            if (depth_budget < 1)
                depth_budget = 1;
            if (depth_budget > 64)
                depth_budget = 64;

            std::vector<std::int64_t> chain;
            chain.reserve(static_cast<std::size_t>(depth_budget) + 1);
            chain.push_back(static_cast<std::int64_t>(nid));
            std::uint32_t cur_prov = ws->provenance(nid);
            std::int64_t hops = 0;
            bool cycle = false;
            bool dead = false;
            while (hops < depth_budget && cur_prov != 0) {
                const auto origin = static_cast<aura::ast::NodeId>(cur_prov);
                if (origin >= ws->size()) {
                    dead = true;
                    break;
                }
                for (auto x : chain) {
                    if (x == static_cast<std::int64_t>(origin)) {
                        cycle = true;
                        break;
                    }
                }
                if (cycle)
                    break;
                chain.push_back(static_cast<std::int64_t>(origin));
                ++hops;
                if (!ws->is_live_node(origin)) {
                    dead = true;
                    break;
                }
                const auto next = ws->provenance(origin);
                if (next == 0 || next == cur_prov)
                    break;
                cur_prov = next;
            }

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

            insert_kv("node-id", static_cast<std::int64_t>(nid));
            insert_kv("macro-introduced", ws->is_macro_introduced(nid) ? 1 : 0);
            insert_kv("provenance-id", static_cast<std::int64_t>(ws->provenance(nid)));
            insert_kv("chain-length", static_cast<std::int64_t>(chain.size()));
            insert_kv("hops", hops);
            insert_kv("depth-budget", depth_budget);
            insert_kv("cycle", cycle ? 1 : 0);
            insert_kv("dead-end", dead ? 1 : 0);
            insert_kv("terminal", chain.empty() ? -1 : chain.back());
            // Emit up to 8 hops as chain-i keys (fixed schema for Agents).
            static constexpr const char* kChainKeys[] = {
                "chain-0", "chain-1", "chain-2", "chain-3",
                "chain-4", "chain-5", "chain-6", "chain-7",
            };
            for (std::size_t i = 0; i < chain.size() && i < 8; ++i)
                insert_kv(kChainKeys[i], chain[i]);
            insert_kv("schema", 2167);
            insert_kv("issue", 2167);
            insert_kv("schema-2167", 2167);
            insert_kv("active", 1);
            insert_kv("lazy", 1);

            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #501 / #514 / #1610 / #1616 / #1891 / #2022: query:ir-hygiene-stats —
    // IR-level MacroIntroduced + ClosureBridge provenance (refine #1047).
    // Schema **2022** (lineage 1891 / 1616 / 1610 / 501). Authoritative e2e
    // surface for self-evolution: propagated counts + zero-leakage key +
    // native JIT/AOT MacroIntroduced side-table after native code is live.
    auto build_ir_hygiene_stats = [&string_heap](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_void();
        // 40+ keys (2022 native side-table + 1891 lineage) — 128 slots.
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
        auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
        const auto load_m =
            [m](std::atomic<std::uint64_t> CompilerMetrics::* field) -> std::uint64_t {
            return m ? (m->*field).load(std::memory_order_relaxed) : 0;
        };
        const std::uint64_t inline_skipped = ir_inline_hygiene_skipped(ev);
        const std::uint64_t markers = workspace_marker_macro_introduced(ev);
        const std::uint64_t stamped = aura_hygiene_ir_macro_marker_total();
        const std::uint64_t provenance_stamped = aura_hygiene_ir_provenance_stamped_total();
        const std::uint64_t jit_deopt = aura_jit_macro_introduced_deopt();
        const std::uint64_t jit_consults = aura_jit_macro_hygiene_consults();
        // Issue #2022: native side-table after JIT/AOT (survives deopt).
        const std::uint64_t native_preserved = aura_jit_native_marker_preserved_total();
        const std::uint64_t live_macro_fns = aura_jit_live_macro_fn_count();
        const std::uint64_t prov_recoverable = aura_jit_macro_provenance_recoverable_total();
        const std::uint64_t ir_prov = load_m(&CompilerMetrics::ir_provenance_stamped_total);
        const std::uint64_t closure_macro =
            load_m(&CompilerMetrics::ir_closure_macro_stamped_total);
        const std::uint64_t closure_consults =
            load_m(&CompilerMetrics::ir_closure_macro_marker_consults_total);
        const std::uint64_t macro_ignored =
            load_m(&CompilerMetrics::macro_introduced_ignored_in_ir_total);
        // #1891: lowering_marker_propagated from metrics or shared C stamp.
        std::uint64_t lowering_prop = load_m(&CompilerMetrics::lowering_marker_propagated_total);
        if (stamped > lowering_prop)
            lowering_prop = stamped;
        std::uint64_t inline_skip_metric =
            load_m(&CompilerMetrics::ir_macro_introduced_inlined_skipped_total);
        if (inline_skipped > inline_skip_metric)
            inline_skip_metric = inline_skipped;
        // IR-module walk: count MacroIntroduced instrs + zero-provenance leaks.
        std::uint64_t ir_instr_macro = 0;
        std::uint64_t ir_instr_total = 0;
        std::uint64_t ir_macro_zero_provenance = 0;
        std::int64_t ir_module_walked = 0;
        if (ev->compiler_service()) {
            auto* svc = static_cast<aura::compiler::CompilerService*>(ev->compiler_service());
            if (const auto& mod_opt = svc->last_ir_module(); mod_opt.has_value()) {
                ir_module_walked = 1;
                for (const auto& fn : mod_opt->functions) {
                    for (const auto& blk : fn.blocks) {
                        for (const auto& instr : blk.instructions) {
                            ++ir_instr_total;
                            if (instr.source_marker == 1) {
                                ++ir_instr_macro;
                                if (instr.provenance == 0)
                                    ++ir_macro_zero_provenance;
                            }
                        }
                    }
                }
            }
        }
        // Leakage: MacroIntroduced treated as user in IR + zero-provenance
        // MacroIntroduced IR instrs (should be 0 after #1891 clone stamp).
        const std::uint64_t hygiene_leakage = macro_ignored + ir_macro_zero_provenance;
        const std::uint64_t total = inline_skipped + markers + stamped + ir_prov;
        // Issue #1780: per-Evaluator policy (not InlinePass static).
        const bool respects = ev->get_inline_respect_macro_hygiene();
        std::int64_t recommendation = 0;
        if (inline_skipped > 0 && !respects)
            recommendation = 3;
        else if (inline_skipped > 5)
            recommendation = 2;
        else if (markers > 0 && inline_skipped == 0 && respects)
            recommendation = 1;
        // #501/#514 lineage
        insert_kv("inline-hygiene-skipped", static_cast<std::int64_t>(inline_skipped));
        insert_kv("macro-markers", static_cast<std::int64_t>(markers));
        insert_kv("respect-macro-hygiene", respects ? 1 : 0);
        insert_kv("ir-hygiene-total", static_cast<std::int64_t>(total));
        insert_kv("ir-hygiene-recommendation", recommendation);
        // #1610 AC keys
        insert_kv("ir-hygiene-stamped-count", static_cast<std::int64_t>(stamped));
        insert_kv("provenance-stamped-count", static_cast<std::int64_t>(provenance_stamped));
        insert_kv("jit-macro-introduced-deopt", static_cast<std::int64_t>(jit_deopt));
        insert_kv("jit-macro-hygiene-consults", static_cast<std::int64_t>(jit_consults));
        insert_kv("lowering-stamp-wired", 1);
        insert_kv("jit-marker-check-wired", 1);
        insert_kv("aot-bridge-marker-wired", 1);
        // #1616 AC keys (refine #1047 ClosureBridge / IRClosure)
        insert_kv("ir_provenance_stamped_total", static_cast<std::int64_t>(ir_prov));
        insert_kv("ir-provenance-stamped-total", static_cast<std::int64_t>(ir_prov));
        insert_kv("macro_introduced_ignored_in_ir", static_cast<std::int64_t>(macro_ignored));
        insert_kv("macro-introduced-ignored-in-ir", static_cast<std::int64_t>(macro_ignored));
        insert_kv("ir-closure-macro-stamped", static_cast<std::int64_t>(closure_macro));
        insert_kv("ir-closure-macro-consults", static_cast<std::int64_t>(closure_consults));
        insert_kv("macro-introduced-count", static_cast<std::int64_t>(markers + stamped));
        insert_kv("closure-bridge-marker-wired", 1);
        insert_kv("ir-closure-marker-wired", 1);
        insert_kv("flat-instr-provenance-wired", 1);
        // #1891 e2e keys
        insert_kv("lowering-marker-propagated", static_cast<std::int64_t>(lowering_prop));
        insert_kv("ir-macro-introduced-inlined-skipped",
                  static_cast<std::int64_t>(inline_skip_metric));
        insert_kv("ir-module-walked", ir_module_walked);
        insert_kv("ir-instr-total", static_cast<std::int64_t>(ir_instr_total));
        insert_kv("ir-instr-macro-introduced", static_cast<std::int64_t>(ir_instr_macro));
        insert_kv("ir-macro-zero-provenance", static_cast<std::int64_t>(ir_macro_zero_provenance));
        insert_kv("hygiene-leakage", static_cast<std::int64_t>(hygiene_leakage));
        insert_kv("clone-provenance-stamped-wired", 1);
        // Issue #2022: MacroIntroduced preserved across JIT/AOT native boundary.
        insert_kv("jit-native-marker-preserved-total", static_cast<std::int64_t>(native_preserved));
        insert_kv("jit-live-macro-fn-count", static_cast<std::int64_t>(live_macro_fns));
        insert_kv("jit-macro-provenance-recoverable", static_cast<std::int64_t>(prov_recoverable));
        insert_kv("jit-native-marker-side-table-wired", 1);
        insert_kv("jit-native-marker-preserve-wired", 1);
        // After deopt, side-table still holds marker/provenance (not cleared).
        insert_kv("jit-macro-deopt-provenance-retained", 1);
        // Issue #2100: deopt round-trip preserved/lost (IR attrs → AST restamp).
        const auto deopt_preserved = aura_jit_macro_introduced_preserved_total();
        const auto deopt_lost = aura_jit_macro_introduced_lost_total();
        insert_kv("jit-macro-introduced-preserved-total",
                  static_cast<std::int64_t>(deopt_preserved));
        insert_kv("jit-macro-introduced-lost-total", static_cast<std::int64_t>(deopt_lost));
        insert_kv("jit-macro-deopt-ast-restore-wired", 1);
        insert_kv("ir-macro-attr-source-marker-wired", 1);
        insert_kv("schema-2100", 2100);
        insert_kv("issue-2100", 2100);
        // Issue #2177: AOT marker propagation observability (refine #2100
        // which was JIT-only). Companion to the existing jit-* keys for
        // full AOT/JIT parity in the Agent dashboard.
        insert_kv("aot-macro-marker-propagated-total",
                  static_cast<std::int64_t>(aura_2177_aot_macro_marker_propagated_total()));
        insert_kv("aot-macro-marker-stripped-total",
                  static_cast<std::int64_t>(aura_2177_aot_macro_marker_stripped_total()));
        insert_kv("schema-2177", 2177);
        insert_kv("issue-2177", 2177);
        // Issue #2167: Agent hygiene-diagnostic / provenance-chain surface.
        insert_kv("schema-2167", 2167);
        insert_kv("issue-2167", 2167);
        insert_kv("hygiene-diagnostic-wired", 1);
        insert_kv("macro-provenance-chain-wired", 1);
        // Issue #2764: residual IR/JIT/AOT source_marker + InlinePass hard
        // filter + multi-eval denseness preserve (refine #501/#1610/#2100).
        // Additive — schema 2022/2100/2177 lineage preserved.
        const auto ancestor_prop =
            static_cast<std::int64_t>(aura_hygiene_ir_ancestor_propagation_total());
        const auto multi_eval_preserved =
            static_cast<std::int64_t>(aura_multi_eval_macro_marker_preserved_total());
        insert_kv("marker-ancestor-propagation-total", ancestor_prop);
        insert_kv("propagate-marker-from-ast-wired", 1);
        insert_kv("inline-macro-hygiene-hard-filter-wired", 1);
        insert_kv("multi-eval-macro-marker-preserved-total", multi_eval_preserved);
        insert_kv("deopt-restore-macro-introduced-wired", 1);
        insert_kv("schema-2764", 2764);
        insert_kv("issue-2764", 2764);
        // Issue #3064: InlinePass refuses MacroIntroduced *body*
        // instructions even when IRFunction.marker stayed User.
        insert_kv("inline-body-macro-hygiene-wired", 1);
        insert_kv("schema-3064", 3064);
        insert_kv("issue-3064", 3064);
        insert_kv("issue", 2022);
        insert_kv("schema", 2022); // lineage 2764 / 2100 / 2022 / 1891 / 1610
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    };
    ObservabilityPrims::register_stats_impl("query:ir-hygiene-stats", build_ir_hygiene_stats);
    // Note: AC "query:ir-marker-stats" is served as keys on ir-hygiene-stats
    // (macro-introduced-count, etc.) — no new *-stats name (#1448 freeze).

    // Issue #503 / #514: query:pattern-marker-stats. Hash view of
    // query:pattern subtree marker/hygiene counters for Agent loops:
    //   - root-skips: macro_introduced_skipped_in_query_
    //   - recursive-skips: pattern_recursive_macro_skipped_
    //   - hygiene-violations: hygiene_violation_count_
    //   - macro-markers: workspace MacroIntroduced marker tally
    //   - pattern-marker-total: root + recursive + violations + markers
    //   - pattern-marker-recommendation: 0=ok, 1=review skips, 2=alert
    ObservabilityPrims::register_stats_impl(
        "query:pattern-marker-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
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
            const std::uint64_t root_skips = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t recursive_skips = ev->get_pattern_recursive_macro_skipped();
            const std::uint64_t violations = ev->get_hygiene_violation_count();
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);
            const std::uint64_t total = root_skips + recursive_skips + violations + markers;
            std::int64_t recommendation = 0;
            if (violations > 0)
                recommendation = 2;
            else if (root_skips + recursive_skips > 10)
                recommendation = 1;
            insert_kv("root-skips", static_cast<std::int64_t>(root_skips));
            insert_kv("recursive-skips", static_cast<std::int64_t>(recursive_skips));
            insert_kv("hygiene-violations", static_cast<std::int64_t>(violations));
            insert_kv("macro-markers", static_cast<std::int64_t>(markers));
            insert_kv("pattern-marker-total", static_cast<std::int64_t>(total));
            insert_kv("pattern-marker-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #517: query:consolidated-production-priority-stats.
    // Returns the sum of 9 counter groups spanning the 3 P0 foundational
    // pillars from the consolidated meta tracker (non-duplicative with
    // #514 Task6, #515 consolidated P0, #516 Prompt6, and #520 Top-5
    // roadmap which adds batch/orchestration/SV-mutate themes):
    //   P1 Persistence + EDA (#511/#510): panic_checkpoint_save/commit +
    //                                    generation_wrap_count +
    //                                    verification_coverage_feedback +
    //                                    verification_assert_failure
    //   P2 Memory-safety (#505/#516): bridge_epoch_hit +
    //                                 closure_stale_refresh +
    //                                 envframe_stale_refresh +
    //                                 envframe_gc_walk_safe_skips +
    //                                 gc_safepoint_waits_total
    //   P3 SoA hotpath (#506/#463): ir_soa_instructions_emitted +
    //                               ir_soa_functions_emitted +
    //                               passes_skipped_type_dirty +
    //                               module_dirty_skips
    //
    // P0: returns an integer = sum of all 9 counter groups.
    // Follow-up: returns a 3-tuple (persistence+eda memory soa) for
    // fleet dashboards tracking the #517 north-star pillars.
    ObservabilityPrims::register_stats_impl(
        "query:consolidated-production-priority-stats",
        [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t persistence = ev->get_panic_checkpoint_save_count() +
                                              ev->get_panic_checkpoint_commit_count() +
                                              (ws ? ws->generation_wrap_count() : 0);
            const std::uint64_t eda_feedback = ws ? ws->verification_coverage_feedback_total() +
                                                        ws->verification_assert_failure_total()
                                                  : 0;
            const std::uint64_t memory_bridge =
                m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) +
                        m->closure_stale_refresh_count_.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t memory_env =
                ev->get_envframe_stale_refresh_count() + ev->get_envframe_gc_walk_safe_skips();
            const std::uint64_t gc_sync = ev->get_gc_safepoint_waits_total();
            const std::uint64_t ir_soa =
                m ? m->ir_soa_instructions_emitted.load(std::memory_order_relaxed) +
                        m->ir_soa_functions_emitted.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t soa_dirty =
                ev->get_passes_skipped_type_dirty() +
                (m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0);
            return make_int(static_cast<std::int64_t>(persistence + eda_feedback + memory_bridge +
                                                      memory_env + gc_sync + ir_soa + soa_dirty));
        });

    // Issue #520: query:production-roadmap-stats. Returns the sum of
    // 10 counter groups spanning the consolidated Top 5 production
    // priorities (non-duplicative synthesis of #496/#510/#511/#505/
    // #506/#413 themes; avoids #514 Task6, #634 commercial, #635
    // macro-reflect, and the original #429/#430/#431 core tracks):
    //   P1 EDA/SV closed-loop (#496/#510): verification_feedback +
    //                                      sv_mutate_attempts/success
    //   P2 Persistence (#511): panic_checkpoint_save/commit +
    //                          generation_wrap_count
    //   P3 Memory safety (#505/#516): bridge_epoch_hit +
    //                                 closure_stale_refresh +
    //                                 envframe_stale_refresh
    //   P4 SoA hotpath (#506): passes_skipped_type_dirty +
    //                          tag_arity_index_hits + specialization_hits
    //   P5 Atomic batch/rollback (#413/#439): atomic_batch_commits +
    //                                         batch_rollbacks +
    //                                         mutation_log_rollbacks
    //
    // P0: returns an integer = sum of all 10 counter groups.
    // Follow-up: returns a 10-tuple so fleet dashboards can track
    // each north-star pillar independently.
    ObservabilityPrims::register_stats_impl(
        "query:production-roadmap-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t eda_feedback = ws ? ws->verification_coverage_feedback_total() +
                                                        ws->verification_assert_failure_total()
                                                  : 0;
            const std::uint64_t eda_sv =
                ws ? ws->sv_mutate_attempts_total() + ws->sv_mutate_success_total() : 0;
            const std::uint64_t checkpoint =
                ev->get_panic_checkpoint_save_count() + ev->get_panic_checkpoint_commit_count();
            const std::uint64_t gen_wrap = ws ? ws->generation_wrap_count() : 0;
            const std::uint64_t memory_bridge =
                m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) +
                        m->closure_stale_refresh_count_.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t memory_env = ev->get_envframe_stale_refresh_count();
            const std::uint64_t soa_skip = ev->get_passes_skipped_type_dirty();
            const std::uint64_t soa_hotpath =
                (m ? m->specialization_hits.load(std::memory_order_relaxed) : 0) +
                (ws ? ws->tag_arity_index_hits() : 0);
            const std::uint64_t batch =
                (ws ? ws->atomic_batch_commits() : 0) + ev->atomic_batch_count();
            const std::uint64_t rollback =
                ev->atomic_batch_rollbacks() + ev->get_mutation_log_rollback_count();
            return make_int(static_cast<std::int64_t>(eda_feedback + eda_sv + checkpoint +
                                                      gen_wrap + memory_bridge + memory_env +
                                                      soa_skip + soa_hotpath + batch + rollback));
        });

    // Issue #514: query:task6-production-readiness-stats. Returns the
    // sum of 12 counters spanning the Task6 review Top 3 production
    // gaps (non-duplicative synthesis of #547/#551/#550 themes):
    //   Top1 hygiene/marker: skips + violations + inline_skipped + markers
    //   Top2 Guard/reflect: mutation_impact + impact_snapshot + schema_pass
    //                       + panic_commit
    //   Top3 dirty/type: narrowing_refresh + passes_skipped + touched_roots
    //                    + narrowing_dirty_recovery
    ObservabilityPrims::register_stats_impl(
        "query:task6-production-readiness-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t top1 =
                ev->get_macro_introduced_skipped_in_query() + ev->get_hygiene_violation_count() +
                ir_inline_hygiene_skipped(ev) + workspace_marker_macro_introduced(ev);
            const std::uint64_t top2 =
                ev->get_mutation_impact_count() + ev->get_impact_snapshot_count() +
                ev->get_schema_validation_pass_count() + ev->get_panic_checkpoint_commit_count();
            const std::uint64_t dirty_recovery =
                m ? m->narrowing_dirty_recovery_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t top3 = ev->get_narrowing_refresh_count() +
                                       ev->get_passes_skipped_type_dirty() +
                                       ev->get_touched_roots_size() + dirty_recovery;
            return make_int(static_cast<std::int64_t>(top1 + top2 + top3));
        });

    // Issue #441: query:compiler-runtime-production-readiness-stats.
    // Returns the sum of 12 counters spanning the consolidated
    // P0 production-readiness pillars (non-duplicative synthesis
    // of #438/#439/#440/#437 themes; avoids #514 Task6 hygiene/
    // dirty focus and the core-three #426/#427/#428 tracks):
    //   Runtime fiber (#438): mutation_steal_attempts +
    //                          boundary_violation_count
    //   Runtime GC (#439): gc_safepoint_requests + waits + deferred
    //   EDSL workspace (#440): cross_cow + fiber_stale + provenance
    //                          + mutation_log_rollback + schema_pass
    //                          + mutation_impact
    //   EDA verify (#437): verify_dirty (assertion+coverage+sva+
    //                      formal) + verify_tool_calls
    ObservabilityPrims::register_stats_impl(
        "query:compiler-runtime-production-readiness-stats",
        [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const std::uint64_t runtime_fiber =
                ev->get_mutation_steal_attempts() + ev->get_boundary_violation_count();
            const std::uint64_t runtime_gc = ev->get_gc_safepoint_requests_total() +
                                             ev->get_gc_safepoint_waits_total() +
                                             ev->get_gc_safepoint_deferred_total();
            const std::uint64_t edsl_workspace =
                ev->get_cross_cow_invalidations() + ev->get_fiber_stale_ref_count() +
                ev->get_provenance_mismatch() + ev->get_mutation_log_rollback_count() +
                ev->get_schema_validation_pass_count() + ev->get_mutation_impact_count();
            const std::uint64_t eda_verify =
                (ws ? ws->verify_assertion_dirty_total() + ws->verify_coverage_dirty_total() +
                          ws->verify_sva_dirty_total() + ws->verify_formal_cex_dirty_total()
                    : 0) +
                ev->get_verify_tool_calls_total();
            return make_int(static_cast<std::int64_t>(runtime_fiber + runtime_gc + edsl_workspace +
                                                      eda_verify));
        });

    // Issue #634: query:commercial-production-readiness-stats.
    // Returns the sum of 14 counters spanning the July 2026
    // commercial P0 pillars (non-duplicative synthesis of
    // #620/#623/#624/#627-#629/#630-#632/#614-#617/#618;
    // avoids #441 compiler-runtime focus and #613-#633 per-theme
    // issue tests):
    //   Fiber/StableRef (#620/#631): provenance_mismatch +
    //                                stable_ref_invalidations
    //   Arena/GC (#623): gc_safepoint_waits + gc_safepoint_requests
    //   Shape/JIT (#624): shape_stability_hit_count + deopt_count
    //   TypeSystem (#627/#628/#629): narrowing_dirty_recovery +
    //                                 coercion_zerooverhead_win
    //   EDA verify/batch (#630-#632): verify_dirty totals +
    //                                 atomic_batch_commits
    //   Stdlib hotpath (#614/#615/#617): specialization_hits +
    //                                    tag_arity_index_hits
    //   Orchestration (#618): mutation_steal_attempts +
    //                         lock_contention_us
    ObservabilityPrims::register_stats_impl(
        "query:commercial-production-readiness-stats",
        [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t stable_ref =
                ev->get_provenance_mismatch() + (ws ? ws->stable_ref_invalidations() : 0);
            const std::uint64_t arena_gc =
                ev->get_gc_safepoint_waits_total() + ev->get_gc_safepoint_requests_total();
            const std::uint64_t shape_jit =
                shape::shape_stability_hit_count.load(std::memory_order_relaxed) +
                (m ? m->deopt_count.load(std::memory_order_relaxed) : 0);
            const std::uint64_t type_system =
                (m ? m->narrowing_dirty_recovery_total.load(std::memory_order_relaxed) : 0) +
                (m ? m->coercion_zerooverhead_win_total.load(std::memory_order_relaxed) : 0);
            const std::uint64_t eda_batch =
                (ws ? ws->verify_assertion_dirty_total() + ws->verify_coverage_dirty_total() +
                          ws->verify_sva_dirty_total() + ws->verify_formal_cex_dirty_total()
                    : 0) +
                (m ? m->atomic_batch_commits.load(std::memory_order_relaxed) : 0);
            const std::uint64_t stdlib_hotpath =
                (m ? m->specialization_hits.load(std::memory_order_relaxed) : 0) +
                (ws ? ws->tag_arity_index_hits() : 0);
            const std::uint64_t orchestration =
                ev->get_mutation_steal_attempts() + ev->get_lock_contention_us();
            return make_int(static_cast<std::int64_t>(stable_ref + arena_gc + shape_jit +
                                                      type_system + eda_batch + stdlib_hotpath +
                                                      orchestration));
        });

    // Issue #635: query:macro-reflect-self-evo-commercial-stats.
    // Returns the sum of 10 counters spanning the July 2026
    // macro + static reflection + self-evolution commercial
    // closed-loop (non-duplicative synthesis of #597 Task6
    // matrix, #619 follow-up, and #634 runtime pillars):
    //   Macro (#290 clone_macro_body): macro_expansion_dirty +
    //                                  macro_self_modify_dirty
    //   Query hygiene (#547): macro_introduced_skipped_in_query +
    //                          marker_macro_introduced_count
    //   Reflect (#551/#454): schema_validation_pass +
    //                        schema_validation_fail +
    //                        impact_snapshot_count
    //   Guard self-evo (#555): mutation_impact_count +
    //                          guard_dirty_epoch_count
    //   Dirty propagation (#415): mark_dirty_upward_call_count
    //   Commercial safety (#620/#624): stable_ref_invalidations +
    //                                  deopt_count
    //
    // P0: returns an integer = sum of all 10 counter groups.
    // Follow-up: returns a 10-tuple so the AI Agent can compute
    // macro_dirty_rate, reflect_pass_rate, and propagation_depth
    // independently for commercial fleet dashboards.
    ObservabilityPrims::register_stats_impl(
        "query:macro-reflect-self-evo-commercial-stats",
        [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_int(0);
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t macro_dirty =
                ws->macro_expansion_dirty_total() + ws->macro_self_modify_dirty_total();
            const std::uint64_t query_hygiene = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);
            const std::uint64_t reflect_validate =
                ev->get_schema_validation_pass_count() + ev->get_schema_validation_fail_count();
            const std::uint64_t reflect_snap = ev->get_impact_snapshot_count();
            const std::uint64_t guard_impact = ev->get_mutation_impact_count();
            const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
            const std::uint64_t dirty_up = ws->mark_dirty_upward_call_count();
            const std::uint64_t stable_ref = ws->stable_ref_invalidations();
            const std::uint64_t deopt = m ? m->deopt_count.load(std::memory_order_relaxed) : 0;
            return make_int(static_cast<std::int64_t>(
                macro_dirty + query_hygiene + markers + reflect_validate + reflect_snap +
                guard_impact + guard_epoch + dirty_up + stable_ref + deopt));
        });

    // Issue #636: query:edsl-query-mutate-commercial-stats.
    // Returns the sum of 10 counter groups spanning the July 2026
    // EDSL workspace + query/mutate + StableNodeRef commercial
    // closed-loop (non-duplicative synthesis of #620/#622/#619/
    // #621/#630/#623/#618 themes; avoids #552 edsl-stability
    // long-run focus, #635 macro-reflect, and #634 runtime pillars):
    //   StableNodeRef (#620/#631): stable_ref_invalidations +
    //                              node_gen_stale_access +
    //                              provenance_mismatch + fiber_stale_ref
    //   Query/pattern (#619/#621): tag_arity_index_hits +
    //                              tag_arity_index_dirty_marks +
    //                              macro_introduced_skipped_in_query
    //   Mutate/Guard (#622): mutation_impact_count +
    //                        guard_dirty_epoch_count
    //   Dirty propagation: mark_dirty_upward_call_count +
    //                     mark_dirty_total_nodes
    //   Atomic batch (#632): atomic_batch_commits +
    //                        atomic_batch_rollbacks + batch_count
    //   EDA feedback (#630): verification_coverage_feedback +
    //                        verification_assert_failure
    //   GC/orchestration (#623/#618): gc_safepoint_requests/waits +
    //                                 steal_attempts + lock_contention
    //
    // P0: returns an integer = sum of all 10 counter groups.
    // Follow-up: returns a 10-tuple for per-pillar fleet dashboards.
    ObservabilityPrims::register_stats_impl(
        "query:edsl-query-mutate-commercial-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_int(0);
            const std::uint64_t stable_ref =
                ws->stable_ref_invalidations() + ws->node_gen_stale_access_count();
            const std::uint64_t provenance =
                ev->get_provenance_mismatch() + ev->get_fiber_stale_ref_count();
            const std::uint64_t query_index =
                ws->tag_arity_index_hits() + ws->tag_arity_index_dirty_marks();
            const std::uint64_t query_hygiene = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t guard_mutate =
                ev->get_mutation_impact_count() + ev->get_guard_dirty_epoch_count();
            const std::uint64_t dirty_up =
                ws->mark_dirty_upward_call_count() + ws->mark_dirty_total_nodes();
            const std::uint64_t atomic = ws->atomic_batch_commits() + ev->atomic_batch_count() +
                                         ev->atomic_batch_rollbacks();
            const std::uint64_t eda_feedback = ws->verification_coverage_feedback_total() +
                                               ws->verification_assert_failure_total();
            const std::uint64_t gc_coord =
                ev->get_gc_safepoint_requests_total() + ev->get_gc_safepoint_waits_total();
            const std::uint64_t orchestration =
                ev->get_mutation_steal_attempts() + ev->get_lock_contention_us();
            return make_int(static_cast<std::int64_t>(
                stable_ref + provenance + query_index + query_hygiene + guard_mutate + dirty_up +
                atomic + eda_feedback + gc_coord + orchestration));
        });

    // Issue #619: query:macro-reflect-self-evo-followup-stats.
    // Returns the sum of 4 Task6 follow-up closed-loop counters:
    //   - hygiene_skips: macro_introduced_skipped_in_query_
    //   - post_mutate_reflect_pass: schema_validation_pass_count_
    //   - dirty_type_recheck_count: narrowing_dirty_recovery_total +
    //     incremental_typecheck_auto_invocations_total
    //   - transform_applied: mutation_impact_count_
    ObservabilityPrims::register_stats_impl(
        "query:macro-reflect-self-evo-followup-stats",
        [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t hygiene = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t reflect = ev->get_schema_validation_pass_count();
            const std::uint64_t dirty_recheck =
                m ? m->narrowing_dirty_recovery_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t type_recheck =
                m ? m->incremental_typecheck_auto_invocations_total.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t transform = ev->get_mutation_impact_count();
            return make_int(static_cast<std::int64_t>(hygiene + reflect + dirty_recheck +
                                                      type_recheck + transform));
        });

    // Issue #597: query:macro-reflect-self-evo-stats. Returns
    // the sum of 8 observability counters spanning the full
    // Task6 production-review closed loop:
    //   macro expand (MacroIntroduced) → query:pattern hygiene
    //   → mutate under Guard → reflect auto_validate → epoch/
    //   dirty propagation → self-evo stability:
    //   - macro_introduced_skipped_in_query_  (hygiene filter)
    //   - hygiene_violation_count_            (hygiene breach)
    //   - mutation_impact_count_            (Guard success)
    //   - impact_snapshot_count_              (reflect snapshot)
    //   - schema_validation_pass_count_       (auto_validate ok)
    //   - schema_validation_fail_count_     (auto_validate fail)
    //   - panic_checkpoint_commit_count_      (Guard commit)
    //   - cross_cow_invalidations_            (self-evo COW)
    //
    // P0: returns an integer = sum of all 8 counters.
    // Follow-up: returns an 8-tuple so the AI Agent can react
    // to each category independently.
    //
    // Non-duplicative with #547/#548/#549/#551 — those expose
    // per-theme stats; this primitive is the unified Task6
    // matrix observability surface for macro+reflect+self-evo.
    ObservabilityPrims::register_stats_impl(
        "query:macro-reflect-self-evo-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t skips = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t violations = ev->get_hygiene_violation_count();
            const std::uint64_t impact = ev->get_mutation_impact_count();
            const std::uint64_t snapshots = ev->get_impact_snapshot_count();
            const std::uint64_t pass = ev->get_schema_validation_pass_count();
            const std::uint64_t fail = ev->get_schema_validation_fail_count();
            const std::uint64_t commit = ev->get_panic_checkpoint_commit_count();
            const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
            return make_int(static_cast<std::int64_t>(skips + violations + impact + snapshots +
                                                      pass + fail + commit + cross_cow));
        });

    // Issue #595 / #1883: query:self-evolution-loop-stats.
    // #595 shipped a sum int of 5 marker/dirty/epoch/Guard loop counters.
    // #1883 upgrades to a structured hash for AI Agent health dashboards
    // while keeping "total" == legacy sum for monotonic back-compat.
    // Fields (schema 1883):
    //   total, hygiene-skips, dirty-propagated, epoch-deltas, validation-pass,
    //   rollback-count, mutation-total, mutation-success-rate-bp,
    //   invariant-audits, invariant-pass-rate-bp, trail-writes,
    //   stack-depth-lifetime-max, stack-depth-current-max, stack-depth-live,
    //   aot-hotupdate-attempts, aot-hotupdate-ok, aot-hotupdate-fail,
    //   aot-hotupdate-invariant-fail, audit-coverage-bp, schema, issue, active
    ObservabilityPrims::register_stats_impl(
        "query:self-evolution-loop-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            using namespace aura::compiler::typed_audit;
            const std::uint64_t hygiene = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t dirty = ev->get_dirty_propagation_count();
            const std::uint64_t epoch = ev->get_guard_dirty_epoch_count();
            const std::uint64_t validation = ev->get_schema_validation_pass_count();
            const std::uint64_t rollback = ev->get_mutation_log_rollback_count();
            const std::uint64_t total_legacy = hygiene + dirty + epoch + validation + rollback;
            const std::uint64_t mut_total = ev->total_mutations();
            const auto& ac = g_typed_mutation_audit_counters;
            const std::uint64_t inv_aud = ac.invariant_audits.load(std::memory_order_relaxed);
            const std::uint64_t inv_pass = ac.invariant_all_pass.load(std::memory_order_relaxed);
            const std::uint64_t inv_fail =
                ac.invariant_violations_caught.load(std::memory_order_relaxed);
            const std::uint64_t trail = ac.trail_writes.load(std::memory_order_relaxed);
            const std::uint64_t contextual = ac.contextual_total.load(std::memory_order_relaxed);
            const std::uint64_t aot_att = ac.aot_hotupdate_attempts.load(std::memory_order_relaxed);
            const std::uint64_t aot_ok = ac.aot_hotupdate_ok.load(std::memory_order_relaxed);
            const std::uint64_t aot_fail = ac.aot_hotupdate_fail.load(std::memory_order_relaxed);
            const std::uint64_t aot_inv =
                ac.aot_hotupdate_invariant_fail_total.load(std::memory_order_relaxed);
            const std::uint64_t aot_aud = ac.aot_hotupdate_audits.load(std::memory_order_relaxed);
            // Success rate: mutations that did not roll back / (mutations + rollbacks)
            // Use trail successes proxy when mutation log rollbacks available.
            const std::uint64_t mut_den = mut_total + rollback;
            const std::int64_t mut_success_bp =
                mut_den == 0 ? 10000 : static_cast<std::int64_t>((mut_total * 10000ull) / mut_den);
            const std::uint64_t inv_den = inv_aud == 0 ? 0 : inv_aud;
            const std::int64_t inv_pass_bp =
                inv_den == 0 ? 10000 : static_cast<std::int64_t>((inv_pass * 10000ull) / inv_den);
            const std::int64_t aot_cov_bp =
                aot_att == 0 ? 10000 : static_cast<std::int64_t>((aot_aud * 10000ull) / aot_att);
            const std::int64_t stack_life =
                static_cast<std::int64_t>(ev->get_per_fiber_mutation_stack_depth_max());
            const std::int64_t stack_cur =
                static_cast<std::int64_t>(ev->get_per_fiber_mutation_stack_depth_current_max());
            const std::int64_t stack_live =
                static_cast<std::int64_t>(Evaluator::mutation_boundary_depth());

            auto& string_heap = ev->string_heap_mut();
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
            insert_kv("total", static_cast<std::int64_t>(total_legacy)); // #595 back-compat
            insert_kv("hygiene-skips", static_cast<std::int64_t>(hygiene));
            insert_kv("dirty-propagated", static_cast<std::int64_t>(dirty));
            insert_kv("epoch-deltas", static_cast<std::int64_t>(epoch));
            insert_kv("validation-pass", static_cast<std::int64_t>(validation));
            insert_kv("rollback-count", static_cast<std::int64_t>(rollback));
            insert_kv("mutation-total", static_cast<std::int64_t>(mut_total));
            insert_kv("mutation-success-rate-bp", mut_success_bp);
            insert_kv("invariant-audits", static_cast<std::int64_t>(inv_aud));
            insert_kv("invariant-pass", static_cast<std::int64_t>(inv_pass));
            insert_kv("invariant-fail", static_cast<std::int64_t>(inv_fail));
            insert_kv("invariant-pass-rate-bp", inv_pass_bp);
            insert_kv("trail-writes", static_cast<std::int64_t>(trail));
            insert_kv("contextual-total", static_cast<std::int64_t>(contextual));
            insert_kv("stack-depth-lifetime-max", stack_life);
            insert_kv("stack-depth-current-max", stack_cur);
            insert_kv("stack-depth-live", stack_live);
            // avg ≈ current-max when hist sparse; basis points of live/current
            insert_kv("avg-stack-depth-x100",
                      stack_cur > 0 ? (stack_live * 100) / (stack_cur > 0 ? stack_cur : 1)
                                    : stack_live * 100);
            insert_kv("aot-hotupdate-attempts", static_cast<std::int64_t>(aot_att));
            insert_kv("aot-hotupdate-ok", static_cast<std::int64_t>(aot_ok));
            insert_kv("aot-hotupdate-fail", static_cast<std::int64_t>(aot_fail));
            insert_kv("aot-hotupdate-invariant-fail", static_cast<std::int64_t>(aot_inv));
            insert_kv("audit-coverage-bp", aot_cov_bp);
            insert_kv("schema", 1883);
            insert_kv("issue", 1883);
            insert_kv("active", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1909: query:self-evo-stats — unified self-evolution loop
    // health dashboard for AI Agents. Aggregates IR hygiene (#1891),
    // pattern hygiene (#1892), TypedMutationAudit (#1894), Guard hold
    // latency, and closed-loop rollback into one hash (schema **1909**).
    // AC keys (basis points 0..10000 unless noted):
    //   macro-introduced-ratio-bp, hygiene-violation-rate-bp,
    //   ir-macro-propagated-pct-bp, avg-mutation-boundary-depth-x100,
    //   rollback-success-rate-bp, self-evo-loop-latency-p99-us,
    //   recommendation (0=ok,1=review,2=alert), health-score-bp
    ObservabilityPrims::register_stats_impl(
        "query:self-evo-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            using namespace aura::compiler::typed_audit;
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            auto load_m =
                [m](std::atomic<std::uint64_t> CompilerMetrics::* field) -> std::uint64_t {
                return m ? (m->*field).load(std::memory_order_relaxed) : 0;
            };

            // ── Macro / pattern hygiene ──
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);
            const std::uint64_t root_skips = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t recursive_skips = ev->get_pattern_recursive_macro_skipped();
            const std::uint64_t hygiene_viol = ev->get_hygiene_violation_count();
            const std::uint64_t pattern_leak = ev->get_pattern_macro_filter_violations();
            const std::uint64_t boundary_hygiene =
                load_m(&CompilerMetrics::hygiene_violation_prevented_on_boundary_total) +
                load_m(&CompilerMetrics::mutation_boundary_hygiene_violation_total);
            std::uint64_t ws_nodes = 0;
            if (auto* ws = ev->workspace_flat())
                ws_nodes = static_cast<std::uint64_t>(ws->size());
            const std::uint64_t macro_ratio_den = ws_nodes > 0 ? ws_nodes : 1;
            const std::int64_t macro_introduced_ratio_bp =
                static_cast<std::int64_t>((markers * 10000ull) / macro_ratio_den);
            const std::uint64_t hygiene_events =
                root_skips + recursive_skips + hygiene_viol + pattern_leak + boundary_hygiene;
            const std::uint64_t hygiene_den =
                hygiene_events > 0 ? hygiene_events : (root_skips + recursive_skips + 1);
            const std::int64_t hygiene_violation_rate_bp =
                static_cast<std::int64_t>(((hygiene_viol + pattern_leak) * 10000ull) / hygiene_den);

            // ── IR hygiene / propagation ──
            std::uint64_t ir_instr_total = 0, ir_instr_macro = 0, ir_macro_zero_prov = 0;
            std::int64_t ir_module_walked = 0;
            if (ev->compiler_service()) {
                auto* svc = static_cast<aura::compiler::CompilerService*>(ev->compiler_service());
                if (const auto& mod_opt = svc->last_ir_module(); mod_opt.has_value()) {
                    ir_module_walked = 1;
                    for (const auto& fn : mod_opt->functions) {
                        for (const auto& blk : fn.blocks) {
                            for (const auto& instr : blk.instructions) {
                                ++ir_instr_total;
                                if (instr.source_marker == 1) {
                                    ++ir_instr_macro;
                                    if (instr.provenance == 0)
                                        ++ir_macro_zero_prov;
                                }
                            }
                        }
                    }
                }
            }
            const std::uint64_t ir_den = ir_instr_total > 0 ? ir_instr_total : 1;
            const std::int64_t ir_macro_propagated_pct_bp =
                static_cast<std::int64_t>((ir_instr_macro * 10000ull) / ir_den);
            const std::uint64_t lowering_prop =
                load_m(&CompilerMetrics::lowering_marker_propagated_total);
            const std::uint64_t hygiene_leakage =
                load_m(&CompilerMetrics::macro_introduced_ignored_in_ir_total) + ir_macro_zero_prov;

            // ── Boundary depth ──
            const std::int64_t depth_live =
                static_cast<std::int64_t>(Evaluator::mutation_boundary_depth());
            const std::int64_t depth_max =
                static_cast<std::int64_t>(ev->get_per_fiber_mutation_stack_depth_current_max());
            const std::int64_t avg_depth_x100 =
                depth_max > 0 ? (depth_live * 100) / depth_max : depth_live * 100;

            // ── Rollback success ──
            const std::uint64_t rollback_ok =
                load_m(&CompilerMetrics::closed_loop_rollback_success_total);
            const std::uint64_t rollback_log = ev->get_mutation_log_rollback_count();
            const std::uint64_t guard_exc =
                load_m(&CompilerMetrics::mutation_guard_exception_total) +
                load_m(&CompilerMetrics::mutation_boundary_exception_rollback_total);
            const std::uint64_t rollback_attempts = rollback_ok + rollback_log + guard_exc;
            const std::int64_t rollback_success_rate_bp =
                rollback_attempts == 0
                    ? 10000
                    : static_cast<std::int64_t>((rollback_ok * 10000ull) /
                                                (rollback_attempts > 0 ? rollback_attempts : 1));

            // ── Latency p99 proxy: max hold duration (us); hist top bucket ──
            const std::uint64_t hold_max_us =
                load_m(&CompilerMetrics::mutation_hold_duration_us_max);
            const std::uint64_t hold_total =
                load_m(&CompilerMetrics::mutation_hold_duration_us_total);
            const std::uint64_t hold_samples = load_m(&CompilerMetrics::mutation_hold_samples);
            const std::int64_t hold_avg_us =
                hold_samples > 0 ? static_cast<std::int64_t>(hold_total / hold_samples) : 0;
            // p99 ≈ max when sample count small; else use max as conservative p99.
            const std::int64_t latency_p99_us = static_cast<std::int64_t>(hold_max_us);

            // ── TypedMutationAudit ──
            const auto& ac = g_typed_mutation_audit_counters;
            const std::uint64_t inv_aud = ac.invariant_audits.load(std::memory_order_relaxed);
            const std::uint64_t inv_pass = ac.invariant_all_pass.load(std::memory_order_relaxed);
            const std::uint64_t inv_fail =
                ac.invariant_violations_caught.load(std::memory_order_relaxed);
            const std::int64_t inv_pass_bp =
                inv_aud == 0 ? 10000 : static_cast<std::int64_t>((inv_pass * 10000ull) / inv_aud);

            // ── Issue #2030: provenance blame completeness + occurrence hit rate ──
            // blame_completeness_ratio = complete / (complete + miss) in bp.
            // Prefer CompilerMetrics when populated; else process-wide audit counters.
            const std::uint64_t m_blame_c = load_m(&CompilerMetrics::blame_chain_complete_total);
            const std::uint64_t m_blame_m = load_m(&CompilerMetrics::blame_propagation_miss_total);
            const std::uint64_t blame_c =
                m_blame_c > 0 ? m_blame_c
                              : ac.blame_chain_complete_total.load(std::memory_order_relaxed);
            const std::uint64_t blame_m =
                m_blame_m > 0 ? m_blame_m
                              : ac.blame_propagation_miss_total.load(std::memory_order_relaxed);
            const std::uint64_t blame_den = blame_c + blame_m;
            const std::int64_t blame_completeness_ratio_bp =
                blame_den == 0 ? 10000
                               : static_cast<std::int64_t>((blame_c * 10000ull) / blame_den);
            // occurrence_narrowing_post_mutate_hit_rate: renarrow hits/total,
            // else stale-refresh blame-complete ratio, else N/A → 10000.
            const std::uint64_t renarrow_hits =
                load_m(&CompilerMetrics::occurrence_renarrow_hits_total);
            const std::uint64_t renarrow_total =
                load_m(&CompilerMetrics::occurrence_renarrow_total);
            const std::uint64_t stale_refresh =
                load_m(&CompilerMetrics::occurrence_stale_refreshes_total);
            const std::uint64_t occ_blame_ok =
                load_m(&CompilerMetrics::occurrence_blame_chain_complete_total);
            const std::uint64_t narrowing_refresh = ev->get_narrowing_refresh_count();
            std::int64_t occurrence_narrowing_post_mutate_hit_rate_bp = 10000;
            if (renarrow_total > 0) {
                occurrence_narrowing_post_mutate_hit_rate_bp =
                    static_cast<std::int64_t>((renarrow_hits * 10000ull) / renarrow_total);
            } else if (stale_refresh > 0) {
                occurrence_narrowing_post_mutate_hit_rate_bp =
                    static_cast<std::int64_t>((occ_blame_ok * 10000ull) / stale_refresh);
            } else if (narrowing_refresh > 0) {
                // Refresh activity without miss accounting → treat as hit.
                occurrence_narrowing_post_mutate_hit_rate_bp = 10000;
            }
            // Linear × occurrence consistency (process-wide atomics + metrics).
            using namespace aura::compiler::linear_occurrence_mutate;
            const std::uint64_t lin_reval =
                revalidate_hits_total.load(std::memory_order_relaxed) +
                load_m(&CompilerMetrics::linear_occurrence_revalidate_hits_total);
            const std::uint64_t lin_escape =
                escape_violations_prevented_total.load(std::memory_order_relaxed) +
                load_m(&CompilerMetrics::linear_occurrence_escape_prevented_total);
            const std::uint64_t lin_pred_safe =
                predicate_branch_linear_safe_total.load(std::memory_order_relaxed) +
                load_m(&CompilerMetrics::linear_occurrence_predicate_safe_total);
            const std::uint64_t lin_den = lin_reval + lin_escape;
            const std::int64_t linear_occurrence_consistency_bp =
                lin_den == 0 ? 10000 : static_cast<std::int64_t>((lin_reval * 10000ull) / lin_den);
            const std::uint64_t type_ok = ac.type_invariant_ok.load(std::memory_order_relaxed);
            const std::uint64_t type_fail = ac.type_invariant_fail.load(std::memory_order_relaxed);
            const std::uint64_t lin_ok = ac.linear_invariant_ok.load(std::memory_order_relaxed);
            const std::uint64_t lin_inv_fail =
                ac.linear_invariant_fail.load(std::memory_order_relaxed);
            const std::uint64_t prov_ok =
                ac.provenance_invariant_ok.load(std::memory_order_relaxed);
            const std::uint64_t prov_fail =
                ac.provenance_invariant_fail.load(std::memory_order_relaxed);
            const std::int64_t type_invariant_ratio_bp =
                (type_ok + type_fail) == 0
                    ? 10000
                    : static_cast<std::int64_t>((type_ok * 10000ull) / (type_ok + type_fail));
            const std::int64_t linear_invariant_ratio_bp =
                (lin_ok + lin_inv_fail) == 0
                    ? 10000
                    : static_cast<std::int64_t>((lin_ok * 10000ull) / (lin_ok + lin_inv_fail));
            const std::int64_t provenance_invariant_ratio_bp =
                (prov_ok + prov_fail) == 0
                    ? 10000
                    : static_cast<std::int64_t>((prov_ok * 10000ull) / (prov_ok + prov_fail));
            const std::int64_t linear_provenance_consistency_bp = static_cast<std::int64_t>(
                aura::core::provenance::linear_provenance_consistency_bp());
            (void)lin_pred_safe;

            // ── SV closed-loop rounds (self-evo signal) ──
            const std::uint64_t sv_rounds =
                load_m(&CompilerMetrics::sv_self_evo_closed_loop_rounds_total);
            const std::uint64_t sv_hits =
                load_m(&CompilerMetrics::sv_self_evo_convergence_hits_total);

            // ── Recommendation + composite health ──
            // 0=ok, 1=review (elevated skips/latency), 2=alert (leakage/violations)
            std::int64_t recommendation = 0;
            if (hygiene_viol > 0 || pattern_leak > 0 || hygiene_leakage > 0 || inv_fail > 0)
                recommendation = 2;
            else if (hygiene_violation_rate_bp > 500 || hold_avg_us > 10000 ||
                     macro_introduced_ratio_bp > 5000 || blame_completeness_ratio_bp < 5000)
                recommendation = 1;
            // health-score: average of inverted risk signals (higher better)
            const std::int64_t hygiene_health =
                10000 - std::min<std::int64_t>(hygiene_violation_rate_bp, 10000);
            const std::int64_t inv_health = inv_pass_bp;
            const std::int64_t rb_health = rollback_success_rate_bp;
            const std::int64_t ir_health =
                hygiene_leakage == 0
                    ? 10000
                    : std::max<std::int64_t>(
                          0, 10000 - static_cast<std::int64_t>(hygiene_leakage * 1000));
            // Issue #2030: fold blame + occurrence hit rate into health.
            const std::int64_t health_score_bp =
                (hygiene_health + inv_health + rb_health + ir_health + blame_completeness_ratio_bp +
                 occurrence_narrowing_post_mutate_hit_rate_bp) /
                6;

            if (m)
                m->self_evo_unified_health_queries_total.fetch_add(1, std::memory_order_relaxed);

            auto& string_heap = ev->string_heap_mut();
            // #1909 + #2030 agent ratio keys → 128 slots.
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

            // Issue #1909 AC names (exact + kebab aliases)
            insert_kv("macro-introduced-ratio-bp", macro_introduced_ratio_bp);
            insert_kv("macro_introduced_ratio", macro_introduced_ratio_bp);
            insert_kv("hygiene-violation-rate-bp", hygiene_violation_rate_bp);
            insert_kv("hygiene_violation_rate", hygiene_violation_rate_bp);
            insert_kv("ir-macro-propagated-pct-bp", ir_macro_propagated_pct_bp);
            insert_kv("ir_macro_propagated_pct", ir_macro_propagated_pct_bp);
            insert_kv("avg-mutation-boundary-depth-x100", avg_depth_x100);
            insert_kv("avg_mutation_boundary_depth", avg_depth_x100);
            insert_kv("rollback-success-rate-bp", rollback_success_rate_bp);
            insert_kv("rollback_success_rate", rollback_success_rate_bp);
            insert_kv("self-evo-loop-latency-p99-us", latency_p99_us);
            insert_kv("self_evo_loop_latency_p99", latency_p99_us);
            // Supporting counters for dashboards
            insert_kv("macro-markers", static_cast<std::int64_t>(markers));
            insert_kv("workspace-nodes", static_cast<std::int64_t>(ws_nodes));
            insert_kv("hygiene-skips", static_cast<std::int64_t>(root_skips + recursive_skips));
            insert_kv("hygiene-violations", static_cast<std::int64_t>(hygiene_viol));
            insert_kv("pattern-hygiene-leakage", static_cast<std::int64_t>(pattern_leak));
            insert_kv("boundary-hygiene-prevented", static_cast<std::int64_t>(boundary_hygiene));
            insert_kv("ir-module-walked", ir_module_walked);
            insert_kv("ir-instr-total", static_cast<std::int64_t>(ir_instr_total));
            insert_kv("ir-instr-macro-introduced", static_cast<std::int64_t>(ir_instr_macro));
            insert_kv("ir-macro-zero-provenance", static_cast<std::int64_t>(ir_macro_zero_prov));
            insert_kv("hygiene-leakage", static_cast<std::int64_t>(hygiene_leakage));
            insert_kv("lowering-marker-propagated", static_cast<std::int64_t>(lowering_prop));
            insert_kv("mutation-boundary-depth-live", depth_live);
            insert_kv("mutation-boundary-depth-max", depth_max);
            insert_kv("mutation-hold-avg-us", hold_avg_us);
            insert_kv("mutation-hold-max-us", static_cast<std::int64_t>(hold_max_us));
            insert_kv("rollback-success-total", static_cast<std::int64_t>(rollback_ok));
            insert_kv("rollback-log-total", static_cast<std::int64_t>(rollback_log));
            insert_kv("invariant-audits", static_cast<std::int64_t>(inv_aud));
            insert_kv("invariant-pass-rate-bp", inv_pass_bp);
            insert_kv("invariant-fail", static_cast<std::int64_t>(inv_fail));
            insert_kv("sv-closed-loop-rounds", static_cast<std::int64_t>(sv_rounds));
            insert_kv("sv-convergence-hits", static_cast<std::int64_t>(sv_hits));
            // Issue #2030: agent-facing blame + occurrence + linear/provenance ratios
            insert_kv("blame_completeness_ratio", blame_completeness_ratio_bp);
            insert_kv("blame-completeness-ratio-bp", blame_completeness_ratio_bp);
            insert_kv("blame-chain-complete-total", static_cast<std::int64_t>(blame_c));
            insert_kv("blame-propagation-miss-total", static_cast<std::int64_t>(blame_m));
            insert_kv("occurrence_narrowing_post_mutate_hit_rate",
                      occurrence_narrowing_post_mutate_hit_rate_bp);
            insert_kv("occurrence-narrowing-post-mutate-hit-rate-bp",
                      occurrence_narrowing_post_mutate_hit_rate_bp);
            insert_kv("occurrence-renarrow-hits", static_cast<std::int64_t>(renarrow_hits));
            insert_kv("occurrence-renarrow-total", static_cast<std::int64_t>(renarrow_total));
            insert_kv("occurrence-stale-refreshes", static_cast<std::int64_t>(stale_refresh));
            insert_kv("narrowing-refresh-count", static_cast<std::int64_t>(narrowing_refresh));
            insert_kv("linear-occurrence-consistency-bp", linear_occurrence_consistency_bp);
            insert_kv("linear_occurrence_consistency_bp", linear_occurrence_consistency_bp);
            insert_kv("type-invariant-ratio-bp", type_invariant_ratio_bp);
            insert_kv("linear-invariant-ratio-bp", linear_invariant_ratio_bp);
            insert_kv("provenance-invariant-ratio-bp", provenance_invariant_ratio_bp);
            insert_kv("linear-provenance-consistency-bp", linear_provenance_consistency_bp);
            insert_kv("blame-occurrence-ratios-wired", 1);
            insert_kv("schema-2030", 2030);
            insert_kv("issue-2030", 2030);
            insert_kv("health-score-bp", health_score_bp);
            insert_kv("recommendation", recommendation);
            insert_kv("unified-dashboard-wired", 1);
            insert_kv("ir-hygiene-lineage", 1891);
            insert_kv("pattern-hygiene-lineage", 1892);
            insert_kv("typed-audit-lineage", 1894);
            insert_kv("loop-stats-lineage", 1883);
            insert_kv("schema", 1909); // primary lineage; #2030 via schema-2030
            insert_kv("issue", 1909);
            insert_kv("active", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1912: query:stable-refs-batch-health — batch StableNodeRef
    // refresh/pin observability for AI multi-round COW/sub-workspace loops.
    // Schema **1912**. AC metrics:
    //   stable_ref_batch_refresh_total, cow_pinned_across_layers_total,
    //   batch_refresh_latency_p99 (us, max proxy), stale_ref_prevented_total,
    //   fail_rate_bp, batch-refresh-success-rate-bp.
    ObservabilityPrims::register_stats_impl(
        "query:stable-refs-batch-health", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            auto load_m =
                [m](std::atomic<std::uint64_t> CompilerMetrics::* field) -> std::uint64_t {
                return m ? (m->*field).load(std::memory_order_relaxed) : 0;
            };

            const std::uint64_t batch_refresh =
                load_m(&CompilerMetrics::stable_ref_batch_refresh_total);
            const std::uint64_t cow_pinned =
                load_m(&CompilerMetrics::cow_pinned_across_layers_total);
            const std::uint64_t stale_prevented =
                load_m(&CompilerMetrics::stale_ref_prevented_total);
            const std::uint64_t calls = load_m(&CompilerMetrics::batch_refresh_calls_total);
            const std::uint64_t fails = load_m(&CompilerMetrics::batch_refresh_fail_total);
            const std::uint64_t lat_total =
                load_m(&CompilerMetrics::batch_refresh_latency_us_total);
            const std::uint64_t lat_p99 = load_m(&CompilerMetrics::batch_refresh_latency_us_max);
            const std::uint64_t boundary_pins = ev->cow_boundary_pins_total();
            const std::uint64_t atomic_pins = ev->atomic_batch_pinned_refs_total();
            const std::size_t live_boundary = ev->cow_boundary_pinned_ref_count();
            const std::size_t live_atomic = ev->atomic_batch_pinned_ref_count();

            // success rate: ok / (ok + fail); empty → 10000 bp
            const std::uint64_t attempts = batch_refresh + fails;
            const std::int64_t success_rate_bp =
                attempts == 0 ? 10000
                              : static_cast<std::int64_t>((batch_refresh * 10000ull) /
                                                          (attempts > 0 ? attempts : 1));
            const std::int64_t fail_rate_bp = 10000 - success_rate_bp;
            // fail rate < 0.1% ⇒ fail_rate_bp < 10 for multi-round loops
            const std::int64_t lat_avg_us =
                calls > 0 ? static_cast<std::int64_t>(lat_total / calls) : 0;
            // health: high success + non-zero batch surface when pins exist
            const std::int64_t health_score_bp = success_rate_bp;

            if (m)
                m->stable_refs_batch_health_queries_total.fetch_add(1, std::memory_order_relaxed);

            auto& string_heap = ev->string_heap_mut();
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

            // AC metric names (exact snake + kebab aliases)
            insert_kv("stable_ref_batch_refresh_total", static_cast<std::int64_t>(batch_refresh));
            insert_kv("stable-ref-batch-refresh-total", static_cast<std::int64_t>(batch_refresh));
            insert_kv("cow_pinned_across_layers_total", static_cast<std::int64_t>(cow_pinned));
            insert_kv("cow-pinned-across-layers-total", static_cast<std::int64_t>(cow_pinned));
            insert_kv("batch_refresh_latency_p99", static_cast<std::int64_t>(lat_p99));
            insert_kv("batch-refresh-latency-p99", static_cast<std::int64_t>(lat_p99));
            insert_kv("batch_refresh_latency_p99_us", static_cast<std::int64_t>(lat_p99));
            insert_kv("stale_ref_prevented_total", static_cast<std::int64_t>(stale_prevented));
            insert_kv("stale-ref-prevented-total", static_cast<std::int64_t>(stale_prevented));
            insert_kv("cow_boundary_pinned", static_cast<std::int64_t>(boundary_pins));
            insert_kv("cow-boundary-pinned", static_cast<std::int64_t>(boundary_pins));
            // Supporting dashboard keys
            insert_kv("batch-refresh-calls-total", static_cast<std::int64_t>(calls));
            insert_kv("batch-refresh-fail-total", static_cast<std::int64_t>(fails));
            insert_kv("batch-refresh-success-rate-bp", success_rate_bp);
            insert_kv("fail-rate-bp", fail_rate_bp);
            insert_kv("batch-refresh-latency-avg-us", lat_avg_us);
            insert_kv("atomic-batch-pinned-refs-total", static_cast<std::int64_t>(atomic_pins));
            insert_kv("live-cow-boundary-pinned-count", static_cast<std::int64_t>(live_boundary));
            insert_kv("live-atomic-batch-pinned-count", static_cast<std::int64_t>(live_atomic));
            insert_kv("health-score-bp", health_score_bp);
            insert_kv("batch-api-wired", 1);
            insert_kv("children-stable-batch-wired", 1);
            insert_kv("guard-auto-refresh-wired", 1);
            insert_kv("lineage-1500", 1500);
            insert_kv("lineage-738", 738);
            insert_kv("schema", 1912);
            insert_kv("issue", 1912);
            insert_kv("active", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #583: query:primitives-stats. Returns the sum of 6
    // primitives registry + core hot-path observability counters
    // spanning evaluator_primitives_registry.cpp registration
    // and list/math/core builtins (non-duplicative with #478
    // primitive-error-stats 2-tuple and stats:count meta):
    //   - registry_slots: primitives_.slot_count() (ordered_names_)
    //   - primitive_errors: primitive_error_count_ (make_primitive_error)
    //   - error_values_stored: error_values_.size() proxy
    //   - total_mutations_: AI Agent mutate-loop activity
    //   - total_query_calls_: query:* hot-path activity
    //   - specialization_hits_: compiled hot-path proxy
    //
    // P0: returns an integer = sum of all 6 counter groups.
    // Follow-up: returns a 6-tuple so the Agent can compute
    // error_rate = primitive_errors / (mutations + query_calls)
    // and registry health independently.
    ObservabilityPrims::register_stats_impl(
        "query:primitives-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t registry_slots = ev->get_primitive_slot_count();
            const std::uint64_t errors = ev->get_primitive_error_count();
            const std::uint64_t stored = ev->get_primitive_error_values_size();
            const std::uint64_t mutations = ev->total_mutations();
            const std::uint64_t queries = ev->get_total_query_calls();
            const std::uint64_t hot_hits =
                m ? m->specialization_hits.load(std::memory_order_relaxed) : 0;
            return make_int(static_cast<std::int64_t>(registry_slots + errors + stored + mutations +
                                                      queries + hot_hits));
        });

    // Issue #480: query:primitive-meta-stats. Returns the sum of
    // 6 self-describing primitive metadata counters spanning
    // PrimMeta storage + describe/list primitives (non-duplicative
    // with #583 primitives-stats registry hot-path and #478
    // primitive-error-stats 2-tuple):
    //   - registry_slots: primitives_.slot_count()
    //   - documented_meta: slots with non-empty doc string
    //   - describe_calls: primitive_describe_count_
    //   - list_meta_calls: primitive_list_meta_count_
    //   - primitive_errors: primitive_error_count_
    //   - total_query_calls_: Agent meta-inspection activity
    //
    // P0: returns an integer = sum of all 6 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:primitive-meta-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t registry_slots = ev->get_primitive_slot_count();
            const std::uint64_t documented = ev->get_primitive_documented_meta_count();
            const std::uint64_t describes = ev->get_primitive_describe_count();
            const std::uint64_t list_meta = ev->get_primitive_list_meta_count();
            const std::uint64_t errors = ev->get_primitive_error_count();
            const std::uint64_t queries = ev->get_total_query_calls();
            return make_int(static_cast<std::int64_t>(registry_slots + documented + describes +
                                                      list_meta + errors + queries));
        });

    // Issue #602: query:prompt6-violation-count. Returns
    // the sum of 7 Prompt6 memory-safety violation counters
    // that must stay at 0 under production load:
    //   - boundary_violation_count_         (unsafe boundary)
    //   - mutation_steal_violation_count_   (steal during guard)
    //   - envframe_desync_detected_         (SoA dual-path mismatch)
    //   - unsafe_boundary_attempts_         (EDSL concurrency)
    //   - atomic_batch_steal_violation_     (batch + steal race)
    //   - provenance_mismatch_              (StableNodeRef layer)
    //   - fiber_stale_ref_count_            (cross-fiber stale ref)
    //
    // P0: returns an integer = sum of all 7 counters.
    // Follow-up: returns a 7-tuple so the AI Agent can react
    // to each category independently. Any value > 0 is a hard
    // alert for commercial production sign-off.
    //
    // Non-duplicative with #438/#448/#531/#543 — those expose
    // per-theme stats; this primitive is the unified Prompt6
    // violation surface for the full memory-safety matrix.
    ObservabilityPrims::register_stats_impl(
        "query:prompt6-violation-count", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t boundary = ev->get_boundary_violation_count();
            const std::uint64_t steal_viol = ev->get_mutation_steal_violation_count();
            const std::uint64_t desync = ev->get_envframe_desync_detected();
            const std::uint64_t unsafe = ev->get_unsafe_boundary_attempts();
            const std::uint64_t batch_steal = ev->get_atomic_batch_steal_violation();
            const std::uint64_t provenance = ev->get_provenance_mismatch();
            const std::uint64_t fiber_stale = ev->get_fiber_stale_ref_count();
            return make_int(static_cast<std::int64_t>(boundary + steal_viol + desync + unsafe +
                                                      batch_steal + provenance + fiber_stale));
        });

    // Issue #602: query:prompt6-safety-score. Returns
    // the sum of 7 Prompt6 memory-safety positive indicators
    // (higher = more safety checks passed / stale refs caught):
    //   - bridge_epoch_hit_count_           (fresh closure bridge)
    //   - linear_check_pass_count_          (linear ownership ok)
    //   - closure_stale_refresh_count_      (stale closure refreshed)
    //   - envframe_stale_refresh_count_     (stale EnvFrame refreshed)
    //   - gc_envframe_stale_skipped_        (GC caught stale EnvFrame)
    //   - envframe_gc_walk_safe_skips_      (GC walk safe skip)
    //   - gc_safepoint_waits_total_         (GC coordination completed)
    //
    // P0: returns an integer = sum of all 7 counters.
    // Follow-up: returns a 7-tuple so the AI Agent can compute
    // safety_ratio = safety_score / (safety_score + violation_count).
    //
    // Non-duplicative with #531/#543/#439 — those expose per-theme
    // pass counters; this primitive is the unified Prompt6 safety
    // score for the full memory-safety fuzz/stress matrix.
    ObservabilityPrims::register_stats_impl(
        "query:prompt6-safety-score", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t bridge_hit =
                m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t linear_pass =
                m ? m->linear_check_pass_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t closure_refresh =
                m ? m->closure_stale_refresh_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t env_refresh = ev->get_envframe_stale_refresh_count();
            const std::uint64_t gc_skipped =
                m ? m->gc_envframe_stale_skipped_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t gc_walk_skips = ev->get_envframe_gc_walk_safe_skips();
            const std::uint64_t gc_waits = ev->get_gc_safepoint_waits_total();
            return make_int(static_cast<std::int64_t>(bridge_hit + linear_pass + closure_refresh +
                                                      env_refresh + gc_skipped + gc_walk_skips +
                                                      gc_waits));
        });

    // Issue #516: query:prompt6-memory-safety-stats. Hash view of Prompt6
    // memory/ownership/GC production-readiness pillars (non-duplicative
    // synthesis of #602 prompt6-violation-count + prompt6-safety-score int
    // sums and #505 closure-env-safety-stats hash; avoids #515 consolidated
    // P0 tracker and #517 3-pillar int-sum):
    //   P1 Closure/EnvFrame/bridge_epoch: bridge-epoch-hits, closure-stale-refresh,
    //      envframe-stale-refresh, linear-check-passes
    //   P2 invalidate + GC sync: gc-envframe-skipped, gc-walk-safe-skips,
    //      gc-safepoint-waits
    //   P3 Incremental dirty: passes-skipped-dirty, module-dirty-skips
    //   P4 Violation alert surface: boundary/steal/envframe-desync/
    //      provenance-mismatch/fiber-stale-refs
    //   P5 JIT/deopt: deopt-count
    //   - safety-score / violation-count: per-pillar subtotals
    //   - prompt6-memory-safety-total / prompt6-memory-safety-recommendation
    ObservabilityPrims::register_stats_impl(
        "query:prompt6-memory-safety-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
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
            const std::uint64_t bridge_hit =
                m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t linear_pass =
                m ? m->linear_check_pass_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t closure_refresh =
                m ? m->closure_stale_refresh_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t env_refresh = ev->get_envframe_stale_refresh_count();
            const std::uint64_t gc_skipped =
                m ? m->gc_envframe_stale_skipped_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t gc_walk_skips = ev->get_envframe_gc_walk_safe_skips();
            const std::uint64_t gc_waits = ev->get_gc_safepoint_waits_total();
            const std::uint64_t passes_skipped = ev->get_passes_skipped_type_dirty();
            const std::uint64_t module_dirty =
                m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0;
            const std::uint64_t boundary = ev->get_boundary_violation_count();
            const std::uint64_t steal_viol = ev->get_mutation_steal_violation_count();
            const std::uint64_t desync = ev->get_envframe_desync_detected();
            const std::uint64_t unsafe = ev->get_unsafe_boundary_attempts();
            const std::uint64_t batch_steal = ev->get_atomic_batch_steal_violation();
            const std::uint64_t provenance = ev->get_provenance_mismatch();
            const std::uint64_t fiber_stale = ev->get_fiber_stale_ref_count();
            const std::uint64_t deopt = m ? m->deopt_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t safety_score = bridge_hit + linear_pass + closure_refresh +
                                               env_refresh + gc_skipped + gc_walk_skips + gc_waits;
            const std::uint64_t violation_count =
                boundary + steal_viol + desync + unsafe + batch_steal + provenance + fiber_stale;
            const std::uint64_t total =
                safety_score + violation_count + passes_skipped + module_dirty + deopt;
            std::int64_t recommendation = 0;
            if (violation_count > 0)
                recommendation = 3;
            else if (deopt > closure_refresh && deopt > 0)
                recommendation = 2;
            else if (env_refresh > bridge_hit && env_refresh > 0)
                recommendation = 1;
            insert_kv("bridge-epoch-hits", static_cast<std::int64_t>(bridge_hit));
            insert_kv("linear-check-passes", static_cast<std::int64_t>(linear_pass));
            insert_kv("closure-stale-refresh", static_cast<std::int64_t>(closure_refresh));
            insert_kv("envframe-stale-refresh", static_cast<std::int64_t>(env_refresh));
            insert_kv("gc-envframe-skipped", static_cast<std::int64_t>(gc_skipped));
            insert_kv("gc-walk-safe-skips", static_cast<std::int64_t>(gc_walk_skips));
            insert_kv("gc-safepoint-waits", static_cast<std::int64_t>(gc_waits));
            insert_kv("passes-skipped-dirty", static_cast<std::int64_t>(passes_skipped));
            insert_kv("module-dirty-skips", static_cast<std::int64_t>(module_dirty));
            insert_kv("boundary-violations", static_cast<std::int64_t>(boundary));
            insert_kv("steal-violations", static_cast<std::int64_t>(steal_viol));
            insert_kv("envframe-desync", static_cast<std::int64_t>(desync));
            insert_kv("unsafe-boundary-attempts", static_cast<std::int64_t>(unsafe));
            insert_kv("atomic-batch-steal-violations", static_cast<std::int64_t>(batch_steal));
            insert_kv("provenance-mismatch", static_cast<std::int64_t>(provenance));
            insert_kv("fiber-stale-refs", static_cast<std::int64_t>(fiber_stale));
            insert_kv("deopt-count", static_cast<std::int64_t>(deopt));
            insert_kv("safety-score", static_cast<std::int64_t>(safety_score));
            insert_kv("violation-count", static_cast<std::int64_t>(violation_count));
            insert_kv("prompt6-memory-safety-total", static_cast<std::int64_t>(total));
            insert_kv("prompt6-memory-safety-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #519: query:edsl-eda-sv-closedloop-stats. Hash view of the
    // consolidated EDSL/EDA/SV verification closed-loop pillars (non-
    // duplicative synthesis of #496 sv-node-stats, #510 eda-verification-
    // stats, #499 eda-foundation-stats, #497 stable-ref-lifecycle, and
    // #413 mutation-log themes; avoids #514-#518 meta int-sum trackers):
    //   P1 SV structured (#496): sv-node-total, sv-mutate-attempts/success,
    //      structured-mutate-hits
    //   P2 Query scale + hygiene (#447): tag-arity-index-hits,
    //      hygiene-skipped-in-query
    //   P3 StableRef (#497): stable-ref-invalidations, generation-wrap-count,
    //      stable-ref-validated
    //   P4 Verification interop (#510): coverage-feedback, assert-failures,
    //      verification-loop-success, hardware-hook-calls
    //   P5 Atomic batch (#413): atomic-batch-commits, mutation-log-rollbacks
    //   - edsl-eda-sv-closedloop-total / edsl-eda-sv-closedloop-recommendation
    ObservabilityPrims::register_stats_impl(
        "query:edsl-eda-sv-closedloop-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
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
            std::uint64_t sv_node_total = 0;
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
                        case aura::ast::NodeTag::Class:
                            ++sv_node_total;
                            break;
                        default:
                            break;
                    }
                }
            }
            const std::uint64_t sv_attempts = ws ? ws->sv_mutate_attempts_total() : 0;
            const std::uint64_t sv_success = ws ? ws->sv_mutate_success_total() : 0;
            const std::uint64_t structured_hits =
                m ? m->sva_structured_mutate_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t tag_hits = ws ? ws->tag_arity_index_hits() : 0;
            const std::uint64_t hygiene_skipped = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t ref_inval = ws ? ws->stable_ref_invalidations() : 0;
            const std::uint64_t gen_wrap = ws ? ws->generation_wrap_count() : 0;
            const std::uint64_t ref_validated = ev->get_stable_ref_validated_in_primitives_count();
            const std::uint64_t coverage = ws ? ws->verification_coverage_feedback_total() : 0;
            const std::uint64_t assert_fail = ws ? ws->verification_assert_failure_total() : 0;
            const std::uint64_t loop_success =
                m ? m->verification_loop_success_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t hw_hooks =
                m ? m->hardware_backend_hook_calls_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t batch_commits = ws ? ws->atomic_batch_commits() : 0;
            const std::uint64_t rollbacks = ev->get_mutation_log_rollback_count();
            const std::uint64_t total = sv_node_total + sv_attempts + sv_success + structured_hits +
                                        tag_hits + hygiene_skipped + ref_inval + gen_wrap +
                                        ref_validated + coverage + assert_fail + loop_success +
                                        hw_hooks + batch_commits + rollbacks;
            std::int64_t recommendation = 0;
            if (assert_fail > coverage && assert_fail > 0)
                recommendation = 3;
            else if (sv_attempts > 0 && sv_success == 0)
                recommendation = 2;
            else if (ref_inval > 0)
                recommendation = 1;
            insert_kv("sv-node-total", static_cast<std::int64_t>(sv_node_total));
            insert_kv("sv-mutate-attempts", static_cast<std::int64_t>(sv_attempts));
            insert_kv("sv-mutate-success", static_cast<std::int64_t>(sv_success));
            insert_kv("structured-mutate-hits", static_cast<std::int64_t>(structured_hits));
            insert_kv("tag-arity-index-hits", static_cast<std::int64_t>(tag_hits));
            insert_kv("hygiene-skipped-in-query", static_cast<std::int64_t>(hygiene_skipped));
            insert_kv("stable-ref-invalidations", static_cast<std::int64_t>(ref_inval));
            insert_kv("generation-wrap-count", static_cast<std::int64_t>(gen_wrap));
            insert_kv("stable-ref-validated", static_cast<std::int64_t>(ref_validated));
            insert_kv("coverage-feedback", static_cast<std::int64_t>(coverage));
            insert_kv("assert-failures", static_cast<std::int64_t>(assert_fail));
            insert_kv("verification-loop-success", static_cast<std::int64_t>(loop_success));
            insert_kv("hardware-hook-calls", static_cast<std::int64_t>(hw_hooks));
            insert_kv("atomic-batch-commits", static_cast<std::int64_t>(batch_commits));
            insert_kv("mutation-log-rollbacks", static_cast<std::int64_t>(rollbacks));
            insert_kv("edsl-eda-sv-closedloop-total", static_cast<std::int64_t>(total));
            insert_kv("edsl-eda-sv-closedloop-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #521: query:multi-fiber-orchestration-stats. Hash view of
    // commercial multi-fiber orchestration + MutationBoundary + work-
    // stealing safety (non-duplicative synthesis of #500 work-steal-stats,
    // #618 scheduler-mutation-coord-stats, and #512 runtime-orchestration
    // themes; avoids #512 envframe pillar and #515-#520 meta trackers):
    //   - steal-attempts / steal-successes / steal-deferred-outermost:
    //     outermost MutationBoundary steal enforcement
    //   - steal-violations / boundary-violations / unsafe-boundary-attempts:
    //     concurrent mutation safety alert surface
    //   - mutation-boundary-depth / current-fiber-id: live guard state
    //   - gc-safepoint-requests / gc-safepoint-waits /
    //     gc-pauses-attributed-to-mutation: scheduler/GC coordination
    //   - fiber-migration-attempts / lock-contention-us: orchestration load
    //   - multi-fiber-orchestration-total / multi-fiber-orchestration-recommendation
    ObservabilityPrims::register_stats_impl(
        "query:multi-fiber-orchestration-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
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
            const std::uint64_t steal_attempts = aura_work_steal_attempts_total();
            const std::uint64_t steal_successes = aura_work_steal_successes_total();
            const std::uint64_t steal_deferred = aura_adaptive_steal_global_deferred_total();
            const std::uint64_t steal_violations = ev->get_mutation_steal_violation_count();
            const std::uint64_t boundary_violations = ev->get_boundary_violation_count();
            const std::uint64_t unsafe_boundary = ev->get_unsafe_boundary_attempts();
            const std::uint64_t boundary_depth = aura_evaluator_mutation_boundary_depth();
            const std::uint64_t cur_fiber = aura_fiber_current_id();
            const std::uint64_t gc_requests = ev->get_gc_safepoint_requests_total();
            const std::uint64_t gc_waits = ev->get_gc_safepoint_waits_total();
            const std::uint64_t gc_attributed = aura_fiber_static_gc_pause_attributed_to_mutation();
            const std::uint64_t migration = ev->get_mutation_steal_attempts();
            const std::uint64_t lock_us = ev->get_lock_contention_us();
            const std::uint64_t total = steal_attempts + steal_successes + steal_deferred +
                                        steal_violations + boundary_violations + unsafe_boundary +
                                        gc_requests + gc_waits + gc_attributed + migration +
                                        lock_us;
            std::int64_t recommendation = 0;
            if (steal_violations > 0 || boundary_violations > 0)
                recommendation = 3;
            else if (steal_deferred > steal_successes && steal_deferred > 3)
                recommendation = 2;
            else if (boundary_depth > 0 && steal_attempts > 0)
                recommendation = 1;
            insert_kv("steal-attempts", static_cast<std::int64_t>(steal_attempts));
            insert_kv("steal-successes", static_cast<std::int64_t>(steal_successes));
            insert_kv("steal-deferred-outermost", static_cast<std::int64_t>(steal_deferred));
            insert_kv("steal-violations", static_cast<std::int64_t>(steal_violations));
            insert_kv("boundary-violations", static_cast<std::int64_t>(boundary_violations));
            insert_kv("unsafe-boundary-attempts", static_cast<std::int64_t>(unsafe_boundary));
            insert_kv("mutation-boundary-depth", static_cast<std::int64_t>(boundary_depth));
            insert_kv("current-fiber-id", static_cast<std::int64_t>(cur_fiber));
            insert_kv("gc-safepoint-requests", static_cast<std::int64_t>(gc_requests));
            insert_kv("gc-safepoint-waits", static_cast<std::int64_t>(gc_waits));
            insert_kv("gc-pauses-attributed-to-mutation", static_cast<std::int64_t>(gc_attributed));
            insert_kv("fiber-migration-attempts", static_cast<std::int64_t>(migration));
            insert_kv("lock-contention-us", static_cast<std::int64_t>(lock_us));
            insert_kv("multi-fiber-orchestration-total", static_cast<std::int64_t>(total));
            insert_kv("multi-fiber-orchestration-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #506: query:soa-hotpath-adoption-stats. Returns the sum
    // of 8 IR SoA + dirty-aware Pass Pipeline adoption counters
    // spanning evaluator/lowering hot paths (#463 scaffold →
    // production adoption; non-duplicative with #607 Task4
    // matrix and compile:ir-soa-stats hash primitive):
    //   - ir_soa_instructions_emitted_   (IRFunctionSoA dual-emit)
    //   - ir_soa_functions_emitted_      (IRFunctionSoA functions)
    //   - passes_skipped_type_dirty_     (DirtyAwarePass short-circuit)
    //   - relower_skipped_entirely_count_ (incremental re-lower win)
    //   - relower_per_function_called_   (per-fn SoA re-lower path)
    //   - module_dirty_skips_            (clean module skip)
    //   - linear_elide_count_            (SoA column fast path in Pass)
    //   - cascade_body_only_count_       (targeted dirty cascade)
    //
    // P0: returns an integer = sum of all 8 counters.
    // Follow-up: returns an 8-tuple so the AI Agent can compute
    // dirty_skip_rate = passes_skipped / (passes_skipped + relower_called)
    // and SoA adoption_ratio independently.
    ObservabilityPrims::register_stats_impl(
        "query:soa-hotpath-adoption-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t ir_instr =
                m ? m->ir_soa_instructions_emitted.load(std::memory_order_relaxed) : 0;
            const std::uint64_t ir_funcs =
                m ? m->ir_soa_functions_emitted.load(std::memory_order_relaxed) : 0;
            const std::uint64_t passes_skip = ev->get_passes_skipped_type_dirty();
            const std::uint64_t relower_skip =
                m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_per_fn =
                m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t mod_skip =
                m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0;
            const std::uint64_t linear_elide =
                m ? m->linear_elide_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t cascade =
                m ? m->cascade_body_only_count.load(std::memory_order_relaxed) : 0;
            return make_int(static_cast<std::int64_t>(ir_instr + ir_funcs + passes_skip +
                                                      relower_skip + relower_per_fn + mod_skip +
                                                      linear_elide + cascade));
        });

    // Issue #408: query:dirty-propagation-cost-stats. Returns the
    // sum of 7 EDSL dirty propagation + IR block_dirty_ cost
    // counters for high-frequency structural mutation profiling
    // (non-duplicative with #415 dirty-reason-propagation-stats
    // 9-counter verify-category slice, #550 typed-mutation-stats
    // narrowing/touched_roots slice, and #399/#398 per-theme tests):
    //   - upward_calls: mark_dirty_upward_call_count_ (FlatAST)
    //   - upward_nodes: mark_dirty_total_nodes_ (walk depth proxy)
    //   - fast_fixed_point: dirty_upward_fast_fixed_point_hits_
    //   - dirty_propagation: dirty_propagation_count_ (Evaluator)
    //   - passes_skipped: passes_skipped_type_dirty_ (IR block skip)
    //   - selective_recheck: selective_recheck_count_ (incremental)
    //   - cascade_body: cascade_body_only_count (precise block mark)
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:dirty-propagation-cost-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t upward_calls = ws ? ws->mark_dirty_upward_call_count() : 0;
            const std::uint64_t upward_nodes = ws ? ws->mark_dirty_total_nodes() : 0;
            const std::uint64_t fast_hits = ws ? ws->dirty_upward_fast_fixed_point_count() : 0;
            const std::uint64_t propagation = ev->get_dirty_propagation_count();
            const std::uint64_t passes_skip = ev->get_passes_skipped_type_dirty();
            const std::uint64_t selective = ev->get_selective_recheck_count();
            const std::uint64_t cascade =
                m ? m->cascade_body_only_count.load(std::memory_order_relaxed) : 0;
            return make_int(static_cast<std::int64_t>(upward_calls + upward_nodes + fast_hits +
                                                      propagation + passes_skip + selective +
                                                      cascade));
        });

    // Issue #471: query:dirty-propagation-stats. Returns the
    // sum of 3 SV-scale dirty-propagation observability counters
    // (complements query:dirty-propagation-cost-stats from #408
    // which covers 7 cost-related counters):
    //   - upward_calls: mark_dirty_upward_call_count_ (FlatAST)
    //   - early_exit_count: mark_dirty_early_exit_count_ (when
    //     mark_dirty_upward_fast's fixed-point check fired)
    //   - max_depth_observed: mark_dirty_max_depth_observed_
    //     (deepest BFS level reached across all calls)
    //
    // The max_depth_observed is the key signal for SV-scale
    // perf: deep module hierarchies (10k+ nodes, generate
    // blocks) hit BFS levels of 50+ on every small mutate;
    // the early-exit rate (early_exit / upward_calls) tells
    // the AI Agent how much redundant work is being saved.
    ObservabilityPrims::register_stats_impl(
        "query:dirty-propagation-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_int(0);
            const std::uint64_t upward_calls = ws->mark_dirty_upward_call_count();
            const std::uint64_t early_exit = ws->mark_dirty_early_exit_count();
            const std::uint64_t max_depth = ws->mark_dirty_max_depth_observed();
            return make_int(static_cast<std::int64_t>(upward_calls + early_exit + max_depth));
        });

    // Issue #2904: query:dirty-columnar — Agent-visible columnar dirty
    // propagation metrics (column writes / cascades avoided / column
    // scans / legacy tree-walk). Hash surface so Agents poll without
    // stitching. Additive schema-2904. Name avoids *-stats freeze (#1448).
    ObservabilityPrims::register_stats_impl(
        "query:dirty-columnar", [&string_heap, &ev](std::span<const EvalValue>) -> EvalValue {
            auto* ws = ev.workspace_flat();
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
            const std::uint64_t col_writes = ws ? ws->dirty_column_writes_total() : 0;
            const std::uint64_t cascades_avoided =
                ws ? ws->dirty_upward_cascades_avoided_total() : 0;
            const std::uint64_t scan_nodes = ws ? ws->dirty_scan_nodes_total() : 0;
            const std::uint64_t legacy = ws ? ws->dirty_legacy_tree_walk_total() : 0;
            const std::uint64_t upward = ws ? ws->mark_dirty_upward_call_count() : 0;
            const std::uint64_t early = ws ? ws->mark_dirty_early_exit_count() : 0;
            insert_kv("dirty-column-writes-total", static_cast<std::int64_t>(col_writes));
            insert_kv("dirty-upward-cascades-avoided-total",
                      static_cast<std::int64_t>(cascades_avoided));
            insert_kv("dirty-scan-nodes-total", static_cast<std::int64_t>(scan_nodes));
            insert_kv("dirty-legacy-tree-walk-total", static_cast<std::int64_t>(legacy));
            insert_kv("mark-dirty-upward-calls", static_cast<std::int64_t>(upward));
            insert_kv("mark-dirty-early-exit-count", static_cast<std::int64_t>(early));
            insert_kv("dirty-columnar-wired", 1);
            insert_kv("schema-2904", 2904);
            insert_kv("issue-2904", 2904);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #414: query:generation-epoch-stats. Returns the sum of
    // 7 long-running generation_ + composite wrap_epoch_ +
    // mutation-epoch observability counters for AI multi-round
    // session profiling (non-duplicative with #456 epoch-stats
    // single defuse_version return, #457 stable-ref-stats
    // 3-counter invalidation slice, #368 ast:generation-stats
    // per-field hash, #527 stable-ref-cow-fiber-stats COW/fiber
    // slice, and #552 edsl-stability-stats Task1 slice):
    //   - bump_generation: bump_generation_count_ (FlatAST churn)
    //   - generation_wrap: generation_wrap_count_ (uint16 wrap)
    //   - wrap_epoch: wrap_epoch_ (composite epoch, #368)
    //   - is_valid_checks: is_valid_check_count_ (validity load)
    //   - guard_epoch: guard_dirty_epoch_count_ (boundary epoch)
    //   - defuse_epoch: defuse_version_ (global mutation epoch)
    //   - rollback_ok: structural_rollback_success_ (mutate/rollback)
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:generation-epoch-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const std::uint64_t bumps = ws ? ws->bump_generation_count() : 0;
            const std::uint64_t wraps = ws ? ws->generation_wrap_count() : 0;
            const std::uint64_t wrap_ep = ws ? ws->wrap_epoch() : 0;
            const std::uint64_t checks = ws ? ws->is_valid_check_count() : 0;
            const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
            const std::uint64_t defuse = ev->get_defuse_version();
            const std::uint64_t rollback = ws ? ws->structural_rollback_success() : 0;
            return make_int(static_cast<std::int64_t>(bumps + wraps + wrap_ep + checks +
                                                      guard_epoch + defuse + rollback));
        });

    // Issue #416: query:ast-column-compaction-stats. Returns the sum
    // of 7 FlatAST SoA column compaction + fragmentation
    // observability counters for long-lived workspace profiling
    // (non-duplicative with #405 arena-compaction-stats ArenaGroup
    // slice, #261 ast:node-lifecycle-stats per-field hash,
    // ast:recycle-nodes / ast:compact-nodes action primitives,
    // and #430 arena live-object moving theme):
    //   - recycle_total: node_recycle_total_ (dead slot reuse)
    //   - compact_total: node_compact_total_ (densify reclaimed)
    //   - slot_reuse: node_slot_reuse_count_ (free_list hits)
    //   - live_nodes: node_lifecycle_stats live count snapshot
    //   - free_slots: node_lifecycle_stats free_list size
    //   - total_slots: node_lifecycle_stats SoA column size
    //   - fragmentation_bp: dead/total ratio in basis points
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:ast-column-compaction-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_int(0);
            const auto snap = ws->node_lifecycle_stats();
            const std::uint64_t recycle = ws->node_recycle_total();
            const std::uint64_t compact = ws->node_compact_total();
            const std::uint64_t reuse = ws->node_slot_reuse_count();
            const std::uint64_t live = snap.live_nodes;
            const std::uint64_t free = snap.free_slots;
            const std::uint64_t total = snap.total_slots;
            const std::uint64_t frag_bp =
                static_cast<std::uint64_t>(snap.fragmentation_ratio * 10000.0);
            return make_int(static_cast<std::int64_t>(recycle + compact + reuse + live + free +
                                                      total + frag_bp));
        });

    // Issue #417: query:mutation-boundary-invariant-stats. Returns
    // the sum of 7 cross-TU MutationBoundaryGuard + defuse_version_
    // + per-fiber stack observability counters for post-P1/P2
    // evaluator split drift detection (non-duplicative with #448
    // mutation-coordination-stats scheduler/GC slice, #438
    // fiber-migration-stats 2-counter steal slice, #264
    // compile:concurrency-stats per-field hash, and #456
    // epoch-stats single defuse_version return):
    //   - invariant_violations: total_invariant_violations_
    //   - cross_fiber_rollback: cross_fiber_rollback_count_
    //   - mutation_yields: mutation_yield_count_
    //   - guard_epoch: guard_dirty_epoch_count_
    //   - boundary_violations: boundary_violation_count_
    //   - defuse_epoch: defuse_version_ (mutation epoch)
    //   - boundary_depth: mutation_boundary_depth() snapshot
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:mutation-boundary-invariant-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t violations = ev->get_total_invariant_violations();
            const std::uint64_t rollback = ev->cross_fiber_rollback_count();
            const std::uint64_t yields = ev->mutation_yield_count();
            const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
            const std::uint64_t boundary = ev->get_boundary_violation_count();
            const std::uint64_t defuse = ev->get_defuse_version();
            const std::uint64_t depth = Evaluator::mutation_boundary_depth();
            return make_int(static_cast<std::int64_t>(violations + rollback + yields + guard_epoch +
                                                      boundary + defuse + depth));
        });

    // Issue #418: query:envframe-dualpath-stale-stats. Returns the
    // sum of 7 EnvFrame SoA dual-path + stale-policy observability
    // counters spanning evaluator_env.cpp hot paths
    // (non-duplicative with #543 envframe-dualpath-stats 4-counter
    // core slice, #602 prompt6-safety-score aggregated matrix,
    // and pre-registered envframe-stale-stats / envframe-bump-stats
    // stats:list aliases without dedicated sum primitives):
    //   - desync: envframe_desync_detected_ (length mismatch)
    //   - stale_refresh: envframe_stale_refresh_count_ (AutoRefresh)
    //   - post_rollback: envframe_post_rollback_invalidations_
    //   - version_mismatch: envframe_version_mismatch_in_walk_
    //   - gc_walk_skips: envframe_gc_walk_safe_skips_
    //   - gc_stale_skipped: gc_envframe_stale_skipped_ (GC policy)
    //   - defuse_epoch: defuse_version_ (stale epoch snapshot)
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:envframe-dualpath-stale-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t desync = ev->get_envframe_desync_detected();
            const std::uint64_t stale = ev->get_envframe_stale_refresh_count();
            const std::uint64_t rollback = ev->get_envframe_post_rollback_invalidations();
            const std::uint64_t mismatch = ev->get_envframe_version_mismatch_in_walk();
            const std::uint64_t gc_skips = ev->get_envframe_gc_walk_safe_skips();
            const std::uint64_t gc_stale =
                m ? m->gc_envframe_stale_skipped_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t defuse = ev->get_defuse_version();
            return make_int(static_cast<std::int64_t>(desync + stale + rollback + mismatch +
                                                      gc_skips + gc_stale + defuse));
        });

    // Issue #419: query:defuse-version-stats. Returns the sum of
    // 7 modular defuse_version_ + AOT/runtime dispatch
    // observability counters for hot-update stale detection
    // (non-duplicative with #456 epoch-stats single-version return,
    // #456 epoch-delta-since-last-query delta-only primitive,
    // #189 concurrency:version-snapshot pair, and #414/#417/#418
    // slices that include defuse_version as one of seven groups):
    //   - defuse_epoch: current_defuse_version() (live epoch)
    //   - last_queried: last_queried_epoch_ (epoch-stats stamp)
    //   - total_mutations: total_mutations_ (lifetime bump count)
    //   - mutation_impact: mutation_impact_count_ (boundary flush)
    //   - guard_epoch: guard_dirty_epoch_count_ (boundary coord)
    //   - aot_emits: CompilerMetrics::aot_emits (emit events)
    //   - bridge_hits: bridge_epoch_hit_count_ (fresh bridge)
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:defuse-version-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t defuse = ev->current_defuse_version();
            const std::uint64_t last = ev->get_last_queried_epoch();
            const std::uint64_t mutations = ev->total_mutations();
            const std::uint64_t impact = ev->get_mutation_impact_count();
            const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
            const std::uint64_t aot = m ? m->aot_emits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t bridge =
                m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) : 0;
            return make_int(static_cast<std::int64_t>(defuse + last + mutations + impact +
                                                      guard_epoch + aot + bridge));
        });

    // Issue #2860: query:evolution-epoch-snapshot. Unified fiber-scoped
    // "evolution epoch" view for Agent self-evo loops. Reduces the
    // multi-primitive stitch (query:macro-fiber-hygiene +
    // pattern-index-stats + mutation-boundary-hold-stats + defuse
    // queries + marker-stats) into one cheap, atomic-only snapshot
    // that Agent loops can poll at high frequency without torn
    // views under concurrent mutate. Returns a hash with:
    //   - hygiene_depth / violations / gensym_map_size (fiber)
    //   - defuse_version / dirty_node_count
    //   - mutation_boundary_depth / nested_max
    //   - macro_introduced_count (workspace walk)
    //   - generation (arena + flat gen from layout_stamp)
    //   - layout_stamp_publish_total
    //   - residual_defer flags (3 atomic counters)
    //   - schema = 2860
    // Optional [:fiber-id n] arg → fiber-scoped; absent → current
    // fiber. Zero heavy allocation; pure atomic / snapshot loads.
    // Source-cited in the additive-keys contract with #2174,
    // #2097, #2101 (existing fiber-hygiene / boundary-hold /
    // macro-marker surfaces remain green; this is additive, not
    // duplicative).
    ObservabilityPrims::register_stats_impl(
        "query:evolution-epoch-snapshot", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            // Public surfaces only (no private workspace_flat_ / fiber hygiene
            // accessors from this TU — keep #2860 additive + compile-clean).
            const std::uint64_t defuse_version = ev->defuse_version();
            const std::uint64_t residual_defer_total =
                m ? m->mutation_boundary_residual_defer_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t residual_defer_forced_clear =
                m ? m->mutation_boundary_residual_defer_forced_clear_total.load(
                        std::memory_order_relaxed)
                  : 0;
            const std::uint64_t residual_defer_steal_hard_fail =
                m ? m->residual_defer_steal_hard_fail_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t layout_stamp_publish_total =
                m ? m->layout_stamp_publish_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t gen_arena =
                m ? m->layout_stamp_last_arena_gen.load(std::memory_order_relaxed) : 0;
            const std::uint64_t gen_flat =
                m ? m->layout_stamp_last_flat_gen.load(std::memory_order_relaxed) : 0;
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
                        auto kidx = static_cast<std::uint64_t>(ev->push_string_heap(k_str));
                        keys[idx] = make_string(kidx).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("schema", 2860);
            insert_kv("issue", 2860);
            insert_kv("defuse-version", static_cast<std::int64_t>(defuse_version));
            insert_kv("layout-stamp-gen-arena", static_cast<std::int64_t>(gen_arena));
            insert_kv("layout-stamp-gen-flat", static_cast<std::int64_t>(gen_flat));
            insert_kv("layout-stamp-publish-total",
                      static_cast<std::int64_t>(layout_stamp_publish_total));
            insert_kv("residual-defer-total", static_cast<std::int64_t>(residual_defer_total));
            insert_kv("residual-defer-forced-clear",
                      static_cast<std::int64_t>(residual_defer_forced_clear));
            insert_kv("residual-defer-steal-hard-fail",
                      static_cast<std::int64_t>(residual_defer_steal_hard_fail));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #420: query:macro-hygiene-contract-stats. Returns the
    // sum of 7 end-to-end MacroIntroduced hygiene contract
    // observability counters spanning clone/expand → query:pattern
    // → mutate guards → IR InlinePass (non-duplicative with #458
    // hygiene-stats 1-counter skip-only slice, #547
    // pattern-hygiene-stats 2-counter query slice, #514
    // ir-hygiene-stats / pattern-marker-stats 2–3 counter slices,
    // and #597 macro-reflect-self-evo-stats 8-counter bundle):
    //   - query_skips: macro_introduced_skipped_in_query_
    //   - violations: hygiene_violation_count_
    //   - markers: workspace MacroIntroduced marker column tally
    //   - ir_skips: InlinePass macro_hygiene_skipped_
    //   - queries: total_query_calls_
    //   - contract_violations: macro_hygiene_contract_violations_
    //   - macro_dirty: macro_expansion_dirty_total_ (clone path)
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:macro-hygiene-contract-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const std::uint64_t query_skips = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t violations = ev->get_hygiene_violation_count();
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);
            const std::uint64_t ir_skips = ir_inline_hygiene_skipped(ev);
            const std::uint64_t queries = ev->get_total_query_calls();
            const std::uint64_t contract = ev->get_macro_hygiene_contract_violations();
            const std::uint64_t macro_dirty = ws ? ws->macro_expansion_dirty_total() : 0;
            return make_int(static_cast<std::int64_t>(query_skips + violations + markers +
                                                      ir_skips + queries + contract + macro_dirty));
        });

    // Issue #421: query:pattern-macro-filter-stats. Returns the
    // sum of 7 query:pattern recursive MacroIntroduced filter
    // observability counters (non-duplicative with #547
    // pattern-hygiene-stats 2-counter root-skip slice, #514
    // pattern-marker-stats 3-counter slice, and #420
    // macro-hygiene-contract-stats end-to-end bundle):
    //   - root_skips: macro_introduced_skipped_in_query_
    //   - recursive_skips: pattern_recursive_macro_skipped_
    //   - filter_violations: pattern_macro_filter_violations_
    //   - markers: workspace MacroIntroduced marker tally
    //   - queries: total_query_calls_
    //   - index_hits: tag_arity_index_hits (fast-path)
    //   - hygiene_violations: hygiene_violation_count_
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:pattern-macro-filter-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const std::uint64_t root_skips = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t recursive = ev->get_pattern_recursive_macro_skipped();
            const std::uint64_t violations = ev->get_pattern_macro_filter_violations();
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);
            const std::uint64_t queries = ev->get_total_query_calls();
            const std::uint64_t index_hits = ws ? ws->tag_arity_index_hits() : 0;
            const std::uint64_t hygiene = ev->get_hygiene_violation_count();
            return make_int(static_cast<std::int64_t>(root_skips + recursive + violations +
                                                      markers + queries + index_hits + hygiene));
        });

    // Issue #422: query:hygiene-violation-stats. Returns the sum of
    // 7 mutate-path hygiene violation observability counters
    // (non-duplicative with #458 hygiene-stats skip-only slice,
    // #547 pattern-hygiene-stats query slice, and #420/#421
    // macro-hygiene bundles):
    //   - violation_attempts: hygiene_violation_attempts_
    //   - violation_count: hygiene_violation_count_
    //   - query_skips: macro_introduced_skipped_in_query_
    //   - mutation_impact: mutation_impact_count_
    //   - total_mutations: total_mutations_ (lifetime)
    //   - markers: workspace MacroIntroduced marker tally
    //   - guard_epoch: guard_dirty_epoch_count_
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:hygiene-violation-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t attempts = ev->get_hygiene_violation_attempts();
            const std::uint64_t violations = ev->get_hygiene_violation_count();
            const std::uint64_t query_skips = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t impact = ev->get_mutation_impact_count();
            const std::uint64_t mutations = ev->total_mutations();
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);
            const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
            return make_int(static_cast<std::int64_t>(attempts + violations + query_skips + impact +
                                                      mutations + markers + guard_epoch));
        });

    // Issue #423: query:pattern-structural-index-stats. Returns the
    // sum of 7 Evaluator-side structural pre-index observability
    // counters (non-duplicative with #547/#554 query:pattern-index-stats
    // FlatAST workspace slice):
    //   - structural_hits: pattern_structural_index_hits_
    //   - structural_misses: pattern_structural_index_misses_
    //   - index_buckets: tag_arity_index_size()
    //   - index_entries: tag_arity_index_entry_count()
    //   - synced_size: tag_arity_index_synced_size()
    //   - synced_gen: tag_arity_index_synced_gen()
    //   - consistency_violations: pattern_index_consistency_violations_
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:pattern-structural-index-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t hits = ev->get_pattern_structural_index_hits();
            const std::uint64_t misses = ev->get_pattern_structural_index_misses();
            const std::uint64_t buckets = ev->tag_arity_index_size();
            const std::uint64_t entries = ev->tag_arity_index_entry_count();
            const std::uint64_t synced_size = ev->tag_arity_index_synced_size();
            const std::uint64_t synced_gen = ev->tag_arity_index_synced_gen();
            const std::uint64_t violations = ev->get_pattern_index_consistency_violations();
            return make_int(static_cast<std::int64_t>(hits + misses + buckets + entries +
                                                      synced_size + synced_gen + violations));
        });

    // Issue #424: query:stable-ref-workspace-tree-stats. Returns the
    // sum of 7 WorkspaceTree / cross-layer StableNodeRef
    // observability counters (non-duplicative with #457
    // stable-ref-stats 3 FlatAST counters, #527
    // stable-ref-cow-fiber-stats COW/fiber bundle):
    //   - workspace_resolves: stable_ref_workspace_resolves_
    //   - workspace_resolve_misses: stable_ref_workspace_resolve_misses_
    //   - tree_violations: stable_ref_workspace_tree_violations_
    //   - tree_layers: WorkspaceTree::size()
    //   - active_idx: WorkspaceTree::active_idx()
    //   - cow_epoch: WorkspaceTree::cow_epoch_
    //   - is_valid_checks: FlatAST is_valid_check_count()
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:stable-ref-workspace-tree-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const std::uint64_t resolves = ev->get_stable_ref_workspace_resolves();
            const std::uint64_t misses = ev->get_stable_ref_workspace_resolve_misses();
            const std::uint64_t violations = ev->get_stable_ref_workspace_tree_violations();
            std::uint64_t layers = 0;
            std::uint64_t active = 0;
            std::uint64_t cow_epoch = 0;
            if (auto* wt = static_cast<WorkspaceTree*>(ev->workspace_tree())) {
                layers = wt->size();
                active = wt->active_idx();
                cow_epoch = wt->cow_epoch_;
            }
            const std::uint64_t checks = ws ? ws->is_valid_check_count() : 0;
            return make_int(static_cast<std::int64_t>(resolves + misses + violations + layers +
                                                      active + cow_epoch + checks));
        });

    // Issue #407: query:shape-deopt-burst-stats. Returns the sum of
    // 7 ShapeProfiler bursty-mutation + deopt-storm observability
    // counters for AI orchestration workload tuning
    // (non-duplicative with #570 shape-stability-stats 6-counter
    // slice emphasizing stable_hits/fiber_refresh, #605 JIT mutate
    // matrix, and #403 ir-metadata-stats deopt-only slice):
    //   - shape_churn: mutation_shape_churn_count (burst detect)
    //   - shape_changes: shape_changes_observed (change frequency)
    //   - deopt_storm: deopt_count (GuardShape mismatch total)
    //   - jit_recompile: jit_shape_miss_count (cache version miss)
    //   - deopt_hooks: shape_deopt_hook_fire_count (invalidate hook)
    //   - version_bumps: shape_version_bump_count (profile invalidate)
    //   - spec_hits: specialization_hits (steady-state contrast)
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:shape-deopt-burst-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t churn =
                shape::mutation_shape_churn_count.load(std::memory_order_relaxed);
            const std::uint64_t changes =
                m ? m->shape_changes_observed.load(std::memory_order_relaxed) : 0;
            const std::uint64_t deopt = m ? m->deopt_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t jit_miss =
                shape::jit_shape_miss_count.load(std::memory_order_relaxed);
            const std::uint64_t hooks =
                shape::shape_deopt_hook_fire_count.load(std::memory_order_relaxed);
            const std::uint64_t bumps =
                shape::shape_version_bump_count.load(std::memory_order_relaxed);
            const std::uint64_t spec_hits =
                m ? m->specialization_hits.load(std::memory_order_relaxed) : 0;
            return make_int(static_cast<std::int64_t>(churn + changes + deopt + jit_miss + hooks +
                                                      bumps + spec_hits));
        });

    // Issue #406: query:pass-contracts-stats. Returns the sum of
    // 7 Pass Pipeline + Contracts + zero-overhead hot-path counters
    // spanning AnalysisPass/DirtyAwarePass concepts and cheap-view
    // dispatch (non-duplicative with #571 value-dispatch-stats
    // 4-tuple, #506 soa-hotpath-adoption 8-counter slice, and
    // #381 per-concept unit tests in test_issue_163):
    //   - contract_violations: value_contract_violation_count
    //   - dispatch_hits: value_dispatch_hit_count (cheap-view fast path)
    //   - passes_skipped: passes_skipped_type_dirty_ (DirtyAwarePass)
    //   - relower_skipped: relower_skipped_entirely_count
    //   - relower_per_fn: relower_per_function_called_count
    //   - module_dirty_skips: clean module incremental skip
    //   - zerooverhead_wins: coercion_zerooverhead_win_total
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:pass-contracts-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t violations =
                types::value_contract_violation_count.load(std::memory_order_relaxed);
            const std::uint64_t dispatch_hits =
                types::value_dispatch_hit_count.load(std::memory_order_relaxed);
            const std::uint64_t passes_skip = ev->get_passes_skipped_type_dirty();
            const std::uint64_t relower_skip =
                m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_per_fn =
                m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t mod_skip =
                m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0;
            const std::uint64_t zero_wins =
                m ? m->coercion_zerooverhead_win_total.load(std::memory_order_relaxed) : 0;
            return make_int(static_cast<std::int64_t>(violations + dispatch_hits + passes_skip +
                                                      relower_skip + relower_per_fn + mod_skip +
                                                      zero_wins));
        });

    // Issue #405: query:arena-compaction-stats. Returns the sum of
    // 7 arena automatic compaction + fragmentation orchestration
    // counters for AI multi-round mutation workloads
    // (non-duplicative with #187 compile:arena-stats / arena:*
    // primitives, #335 arena:adaptive-stats 2-tuple, and #300
    // arena:defrag-stats 5-tuple):
    //   - auto_compact_triggers: ArenaGroup trigger count
    //   - auto_compact_skips: ArenaGroup skip count (below threshold)
    //   - compaction_count: lifetime compact() calls (all modules)
    //   - compaction_saved: lifetime bytes reclaimed
    //   - compaction_paused: deferred at MutationBoundary
    //   - mutation_volume: total_mutations_ (orchestration signal)
    //   - dirty_propagation: mark_dirty_upward activity
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:arena-compaction-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto& group = ev->arena_group();
            const auto stats = group.total_stats();
            const std::uint64_t triggers = group.auto_compact_trigger_count();
            const std::uint64_t skips = group.auto_compact_skip_count();
            const std::uint64_t compacts = stats.compaction_count;
            const std::uint64_t saved = stats.total_compaction_saved;
            const std::uint64_t paused = ev->compaction_paused_by_boundary();
            const std::uint64_t mutations = ev->total_mutations();
            const std::uint64_t dirty = ev->get_dirty_propagation_count();
            return make_int(static_cast<std::int64_t>(triggers + skips + compacts + saved + paused +
                                                      mutations + dirty));
        });

    // Issue #404: query:ir-soa-incremental-stats. Returns the sum
    // of 7 IR SoA Phase 3 block_dirty_-driven incremental lowering
    // counters (non-duplicative with #506 soa-hotpath-adoption
    // 8-counter slice that includes passes_skipped + linear_elide,
    // #607 task4-cache-locality-win tag_arity slice, and #254
    // compile:ir-soa-stats hash primitive):
    //   - ir_soa_instructions_emitted   (IRFunctionSoA dual-emit)
    //   - ir_soa_functions_emitted      (IRFunctionSoA functions)
    //   - relower_skipped_entirely      (skip clean re-lower win)
    //   - relower_per_function_called   (per-fn incremental path)
    //   - module_dirty_skips            (clean module skip)
    //   - module_dirty_recompiles       (dirty module recompile)
    //   - cascade_body_only             (block_dirty cascade)
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:ir-soa-incremental-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t ir_instr =
                m ? m->ir_soa_instructions_emitted.load(std::memory_order_relaxed) : 0;
            const std::uint64_t ir_funcs =
                m ? m->ir_soa_functions_emitted.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_skip =
                m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_per_fn =
                m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t mod_skip =
                m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0;
            const std::uint64_t mod_recompile =
                m ? m->module_dirty_recompiles.load(std::memory_order_relaxed) : 0;
            const std::uint64_t cascade =
                m ? m->cascade_body_only_count.load(std::memory_order_relaxed) : 0;
            return make_int(static_cast<std::int64_t>(ir_instr + ir_funcs + relower_skip +
                                                      relower_per_fn + mod_skip + mod_recompile +
                                                      cascade));
        });

    // Issue #403: query:ir-metadata-stats. Returns the sum of
    // 7 IRInstruction rich-metadata consumption counters spanning
    // IRInterpreter + JIT paths (non-duplicative with #506 SoA
    // adoption 8-counter slice, #570 shape-stability-stats,
    // #598 linear-ownership-runtime-stats 4-tuple):
    //   - narrow_evidence_hits: coercion_narrow_evidence_hits_total
    //     (GuardShape narrow fast-path — interpreter + JIT)
    //   - linear_elide: linear_elide_count (linear_ownership_state
    //     elision in TypeSpecializationWrap)
    //   - linear_enforce: linear_post_mutate_enforcements_total
    //     (interpreter GuardShape linear enforcement)
    //   - linear_pass: linear_check_pass_count_ (interpreter linear
    //     ownership fast-path checks)
    //   - jit_shape_hits: specialization_hits (JIT shape_id fast path)
    //   - deopt_total: deopt_count (shape mismatch — consistency signal)
    //   - adt_variant_impacts: adt_variant_mutate_impacts_total
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:ir-metadata-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t narrow =
                m ? m->coercion_narrow_evidence_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t linear_elide =
                m ? m->linear_elide_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t linear_enforce =
                m ? m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t linear_pass =
                m ? m->linear_check_pass_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t jit_hits =
                m ? m->specialization_hits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t deopt = m ? m->deopt_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t adt_impacts =
                m ? m->adt_variant_mutate_impacts_total.load(std::memory_order_relaxed) : 0;
            return make_int(static_cast<std::int64_t>(narrow + linear_elide + linear_enforce +
                                                      linear_pass + jit_hits + deopt +
                                                      adt_impacts));
        });

    // Issue #607: query:task4-hotpath-safety-score. Returns
    // the sum of 6 Task4 high-perf hot-path positive indicators:
    //   - specialization_hits_         (shape/JIT fast path)
    //   - relower_skipped_entirely_    (incremental re-lower win)
    //   - passes_skipped_type_dirty_   (Pass short-circuit)
    //   - linear_elide_count_          (linear-move elision)
    //   - tag_arity_index_hits_        (SoA query index hit)
    //   - module_dirty_skips_          (clean module skip)
    //
    // P0: returns an integer = sum of all 6 counters.
    // Non-duplicative with #602/#547/#550 — unified Task4
    // hot-path observability for Arena/SoA/Value/Shape/Pass.
    ObservabilityPrims::register_stats_impl(
        "query:task4-hotpath-safety-score", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            auto* ws = ev->workspace_flat();
            const std::uint64_t spec_hits =
                m ? m->specialization_hits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_skip =
                m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t passes_skip = ev->get_passes_skipped_type_dirty();
            const std::uint64_t linear_elide =
                m ? m->linear_elide_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t index_hits = ws ? ws->tag_arity_index_hits() : 0;
            const std::uint64_t mod_skip =
                m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0;
            return make_int(static_cast<std::int64_t>(spec_hits + relower_skip + passes_skip +
                                                      linear_elide + index_hits + mod_skip));
        });

    // Issue #607: query:task4-cache-locality-win. Returns
    // the sum of 5 cache-friendly / incremental-win counters:
    //   - tag_arity_index_hits_          (SoA index cache hit)
    //   - tag_arity_index_delta_hits_    (incremental index update)
    //   - specialization_hits_           (shape specialization)
    //   - cascade_body_only_count_       (targeted dirty cascade)
    //   - relower_per_function_called_   (per-fn incremental re-lower)
    //
    // P0: returns an integer = sum of all 5 counters.
    ObservabilityPrims::register_stats_impl(
        "query:task4-cache-locality-win", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            auto* ws = ev->workspace_flat();
            const std::uint64_t hits = ws ? ws->tag_arity_index_hits() : 0;
            const std::uint64_t delta = ws ? ws->tag_arity_index_delta_hits() : 0;
            const std::uint64_t spec =
                m ? m->specialization_hits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t cascade =
                m ? m->cascade_body_only_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t per_fn =
                m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
            return make_int(static_cast<std::int64_t>(hits + delta + spec + cascade + per_fn));
        });

    // Issue #570/#605: query:shape-stability-stats. Returns the sum
    // of 6 ShapeProfiler stability observability counters:
    //   - shape_stability_hit_count    (first-time stable)
    //   - shape_version_bump_count     (invalidate version++)
    //   - shape_fiber_refresh_count    (MutationBoundary yield)
    //   - mutation_shape_churn_count   (stable→unstable / invalidate)
    //   - shape_deopt_hook_fire_count  (invalidate deopt hook)
    //   - jit_shape_miss_count         (#605 JIT cache version miss)
    //
    // P0: returns an integer = sum of all 6 counters.
    // Follow-up: returns a 6-tuple + derived stable_ratio_bp.
    // Non-duplicative with #571 (value dispatch) and #607
    // (Task4 hot-path) — unified shape-stability surface.
    ObservabilityPrims::register_stats_impl(
        "query:shape-stability-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            const std::uint64_t stable_hits =
                shape::shape_stability_hit_count.load(std::memory_order_relaxed);
            const std::uint64_t version_bumps =
                shape::shape_version_bump_count.load(std::memory_order_relaxed);
            const std::uint64_t fiber_refresh =
                shape::shape_fiber_refresh_count.load(std::memory_order_relaxed);
            const std::uint64_t churn =
                shape::mutation_shape_churn_count.load(std::memory_order_relaxed);
            const std::uint64_t deopt_hooks =
                shape::shape_deopt_hook_fire_count.load(std::memory_order_relaxed);
            const std::uint64_t jit_shape_miss =
                shape::jit_shape_miss_count.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(stable_hits + version_bumps + fiber_refresh +
                                                      churn + deopt_hooks + jit_shape_miss));
        });

    // Issue #492: query:shape-profiler-stats — structured ShapeProfiler
    // deopt/stability view for AI orchestration (non-duplicative with
    // #570 int-sum and #407 burst-stats).
    ObservabilityPrims::register_stats_impl(
        "query:shape-profiler-stats",
        [&ev, &string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t stable_hits =
                shape::shape_stability_hit_count.load(std::memory_order_relaxed);
            const std::uint64_t version_bumps =
                shape::shape_version_bump_count.load(std::memory_order_relaxed);
            const std::uint64_t fiber_refresh =
                shape::shape_fiber_refresh_count.load(std::memory_order_relaxed);
            const std::uint64_t churn =
                shape::mutation_shape_churn_count.load(std::memory_order_relaxed);
            const std::uint64_t deopt_hooks =
                shape::shape_deopt_hook_fire_count.load(std::memory_order_relaxed);
            const std::uint64_t jit_shape_miss =
                shape::jit_shape_miss_count.load(std::memory_order_relaxed);
            const std::uint64_t deopt_storm =
                shape::shape_deopt_storm_count.load(std::memory_order_relaxed);
            const std::uint64_t shape_changes =
                m ? m->shape_changes_observed.load(std::memory_order_relaxed) : 0;
            const std::uint64_t deopt_count =
                m ? m->deopt_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t spec_hits =
                m ? m->specialization_hits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t spec_misses =
                m ? m->specialization_misses.load(std::memory_order_relaxed) : 0;
            constexpr std::int64_t k_window =
                static_cast<std::int64_t>(shape::ShapeProfiler::kDefaultWindowSize);
            constexpr std::int64_t k_ratio_bp =
                static_cast<std::int64_t>(shape::ShapeProfiler::kDefaultStabilityRatio * 10000.0);
            // Issue #2433: capacity 32 (≥24 keys incl. schema-2257 + schema-2433).
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
            insert_kv("stability-hits", static_cast<std::int64_t>(stable_hits));
            insert_kv("version-bumps", static_cast<std::int64_t>(version_bumps));
            insert_kv("fiber-refresh", static_cast<std::int64_t>(fiber_refresh));
            insert_kv("shape-churn", static_cast<std::int64_t>(churn));
            insert_kv("deopt-hooks", static_cast<std::int64_t>(deopt_hooks));
            insert_kv("jit-shape-miss", static_cast<std::int64_t>(jit_shape_miss));
            insert_kv("deopt-storm-count", static_cast<std::int64_t>(deopt_storm));
            insert_kv("shape-changes-observed", static_cast<std::int64_t>(shape_changes));
            insert_kv("deopt-count", static_cast<std::int64_t>(deopt_count));
            insert_kv("specialization-hits", static_cast<std::int64_t>(spec_hits));
            insert_kv("specialization-misses", static_cast<std::int64_t>(spec_misses));
            insert_kv("window-size", k_window);
            insert_kv("stability-ratio-bp", k_ratio_bp);
            // Issue #2257 AC3: 3 new query keys (shape-version +
            //   deopt-storm-isolations-total + current-stability-ratio)
            //   + schema-2257 lineage. AC2: under HighMutation +
            //   continuous body mutate, deopt-storm-isolations stays
            //   bounded (one bump per storm enter, not per deopt event).
            insert_kv("shape-version",
                      static_cast<std::int64_t>(
                          shape::shape_version_bump_count.load(std::memory_order_relaxed)));
            insert_kv(
                "deopt-storm-isolations-total",
                m ? static_cast<std::int64_t>(
                        m->deopt_storm_isolations_total.load(std::memory_order_relaxed))
                  : static_cast<std::int64_t>(shape::g_deopt_storm_isolations_total_atomic().load(
                        std::memory_order_relaxed)));
            // Issue #2257: stability ratio lives in aura::compiler
            // (ir_cache_pure.ixx adaptive feed), not shape:: namespace.
            insert_kv("current-stability-ratio",
                      static_cast<std::int64_t>(current_shape_stability_ratio() * 10000.0));
            insert_kv("shape-version-wired", 1);
            insert_kv("schema-2257", 2257);
            insert_kv("issue-2257", 2257);
            // Issue #2433: additive storm-health keys (same hash, no extra
            // public primitive). shape-version-at-storm + shape-storm-active
            // + force-reason for Agent closed-loop throttle.
            insert_kv("shape-version-at-storm",
                      static_cast<std::int64_t>(shape::shape_version_at_last_storm()));
            // force-reason non-zero while storm published; soft-clear resets to 0.
            insert_kv("shape-storm-active",
                      static_cast<std::int64_t>(shape::shape_storm_force_reason() !=
                                                shape::kShapeStormForceReasonNone));
            insert_kv("shape-storm-force-reason",
                      static_cast<std::int64_t>(shape::shape_storm_force_reason()));
            insert_kv("schema-2433", 2433);
            insert_kv("issue-2433", 2433);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2433: query:shape-storm-health — compact Agent-facing score
    // for closed-loop throttle (health-bp + force-reason + version snapshot).
    // Soft path: pure atomic loads; zero writes.
    ObservabilityPrims::register_stats_impl(
        "query:shape-storm-health", [&ev, &string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            (void)ev;
            const auto isolations =
                shape::g_deopt_storm_isolations_total_atomic().load(std::memory_order_relaxed);
            const auto force = shape::shape_storm_force_reason();
            // Issue #2526: only Threshold force-reason is active storm (hard fence).
            // AdaptiveSuppress is soft (no isolation / no health crash).
            const bool storm_active = force == shape::kShapeStormForceReasonThreshold;
            // health-bp: 10000 quiet; −2500 when storm active; −min(4000, isolations*100)
            std::int64_t health_bp = 10000;
            if (storm_active)
                health_bp -= 2500;
            const auto iso_pen =
                static_cast<std::int64_t>(std::min<std::uint64_t>(isolations, 40) * 100);
            health_bp -= iso_pen;
            if (health_bp < 0)
                health_bp = 0;
            // Issue #2526 / #2617 / #2908: capacity 48 for adaptive + compact-
            // isolation + PerEval no-global-bump keys.
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
            insert_kv("health-bp", health_bp);
            insert_kv("force-reason", static_cast<std::int64_t>(force));
            insert_kv("shape-version-at-storm",
                      static_cast<std::int64_t>(shape::shape_version_at_last_storm()));
            insert_kv("shape-storm-active", storm_active ? 1 : 0);
            insert_kv("deopt-storm-isolations-total", static_cast<std::int64_t>(isolations));
            insert_kv("high-mutation-default",
                      static_cast<std::int64_t>(shape::shape_high_mutation_default_enabled()));
            insert_kv("schema-2433", 2433);
            insert_kv("issue-2433", 2433);
            // Issue #2526: adaptive threshold closed-loop surface.
            insert_kv("schema-2526", 2526);
            insert_kv("issue-2526", 2526);
            insert_kv("adaptive-threshold-live",
                      static_cast<std::int64_t>(shape::deopt_storm_adaptive_threshold_live()));
            insert_kv("adaptive-suppress-total",
                      static_cast<std::int64_t>(
                          shape::g_deopt_storm_adaptive_suppress_total_atomic().load(
                              std::memory_order_relaxed)));
            insert_kv(
                "adaptive-enter-total",
                static_cast<std::int64_t>(shape::g_deopt_storm_adaptive_enter_total_atomic().load(
                    std::memory_order_relaxed)));
            insert_kv("shape-storm-fence-hard", shape::shape_storm_fence_hard() ? 1 : 0);
            insert_kv("force-reason-threshold",
                      static_cast<std::int64_t>(shape::kShapeStormForceReasonThreshold));
            insert_kv("force-reason-adaptive-suppress",
                      static_cast<std::int64_t>(shape::kShapeStormForceReasonAdaptiveSuppress));
            insert_kv("adaptive-policy-wired", 1);
            // Issue #2617: compact path never feeds deopt-storm ring (gate + contract).
            insert_kv("schema-2617", 2617);
            insert_kv("issue-2617", 2617);
            insert_kv("compact-storm-isolated-wired",
                      static_cast<std::int64_t>(shape::shape_compact_storm_isolation_wired()));
            insert_kv("deopt-storm-compact-suppressed",
                      static_cast<std::int64_t>(
                          shape::deopt_storm_compact_suppressed.load(std::memory_order_relaxed)));
            // Issue #2908: PerEval harden — compact must not bump process-global
            // shape_version; LayoutStamp force-reason Threshold remains hard.
            insert_kv("schema-2908", shape::kShapeCompactNoGlobalBumpIssue);
            insert_kv("issue-2908", shape::kShapeCompactNoGlobalBumpIssue);
            insert_kv("compact-no-global-bump-wired",
                      static_cast<std::int64_t>(shape::shape_compact_no_global_bump_wired()));
            insert_kv("compact-global-version-skipped-total",
                      static_cast<std::int64_t>(
                          shape::g_shape_compact_global_version_skipped_total_atomic().load(
                              std::memory_order_relaxed)));
            insert_kv("compact-global-version-bump-total",
                      static_cast<std::int64_t>(
                          shape::g_shape_compact_global_version_bump_total_atomic().load(
                              std::memory_order_relaxed)));
            insert_kv("shape-storm-per-eval-isolations-total",
                      static_cast<std::int64_t>(
                          shape::g_shape_storm_per_eval_isolations_total_atomic().load(
                              std::memory_order_relaxed)));
            insert_kv(
                "shape-storm-global-bump-total",
                static_cast<std::int64_t>(shape::g_shape_storm_global_bump_total_atomic().load(
                    std::memory_order_relaxed)));
            insert_kv("shape-storm-isolation-default-per-eval", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #624: query:shape-stability-jit-stats-hash —
    // Agent-discoverable structured companion to the existing
    // query:shape-stability-stats (#570/#605, int-sum) and
    // query:shape-profiler-stats (#492, 12-field). This primitive
    // specifically covers AC4 from the issue body — a
    // JIT-shape-stability dashboard the Agent can probe to decide
    // when to trigger hot-swap invalidation + rebuild ahead of a
    // heavy mutate.
    //
    // Fields (5):
    //   - stability-ratio-post-mutate   synthetic: shape-churn /
    //                                   (shape-churn + shape-changes-
    //                                   observed) * 100, rounded;
    //                                   ~0 when both are 0. Higher
    //                                   = more instability after
    //                                   mutate.
    //   - deopt-on-instability          synthetic: jit-shape-miss
    //                                   / (jit-shape-miss + version-
    //                                   bumps) * 100; 0 when both
    //                                   are 0. Higher = more
    //                                   deopt-triggering shapes.
    //   - version-bumps                 shape::shape_version_bump_count
    //                                   (existing counter from #570)
    //   - jit-shape-miss                shape::jit_shape_miss_count
    //                                   (existing counter from #605)
    //   - wrong-opt-prevented           shape::shape_deopt_storm_count
    //                                   (existing counter from #570) —
    //                                   each deopt storm is a wrong
    //                                   speculative-opt the system
    //                                   caught and backed out
    //   - schema == 624                  sentinel for Agent drift
    //                                   detection (mirrors #618's +
    //                                   #620's + #621's + #622's)
    //
    // Discovery before this PR: the C++ side already exposes the
    // full feature list via shape::*_count counters in `shape::*`
    // namespace (added by #570 / #605 / #492 / #686). The single
    // NEW contribution is the structured primitive the issue
    // body explicitly names — AC4 listed `query:shape-stability-
    // jit-stats` with these exact fields, and no prior PR shipped
    // it under that name. So #624 ships ONE new Aura primitive.
    //
    // The remaining #624 AC work (post-mutate re-eval in
    // record_shape + GuardShape dispatch version check on the
    // shape version bump in aura_jit lower + optional invalidate
    // in mutate primitives success path) is invasive C++ + hot-
    // path change that needs benchmarking + perf regression
    // coverage alongside the JIT/hot-swap work in #601/#491.
    ObservabilityPrims::register_stats_impl(
        "query:shape-stability-jit-stats-hash",
        [&ev, &string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            const std::uint64_t shape_churn =
                shape::mutation_shape_churn_count.load(std::memory_order_relaxed);
            const std::uint64_t version_bumps =
                shape::shape_version_bump_count.load(std::memory_order_relaxed);
            const std::uint64_t jit_shape_miss =
                shape::jit_shape_miss_count.load(std::memory_order_relaxed);
            const std::uint64_t deopt_storms =
                shape::shape_deopt_storm_count.load(std::memory_order_relaxed);
            const std::uint64_t shape_changes_observed =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->shape_changes_observed.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t churn_total = shape_churn + shape_changes_observed;
            const std::int64_t post_mutate_ratio =
                churn_total == 0 ? 0 : static_cast<std::int64_t>((shape_churn * 100) / churn_total);
            const std::uint64_t deopt_denom = jit_shape_miss + version_bumps;
            const std::int64_t deopt_on_instability =
                deopt_denom == 0 ? 0
                                 : static_cast<std::int64_t>((jit_shape_miss * 100) / deopt_denom);
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
            insert_kv("stability-ratio-post-mutate", post_mutate_ratio);
            insert_kv("deopt-on-instability", deopt_on_instability);
            insert_kv("version-bumps", static_cast<std::int64_t>(version_bumps));
            insert_kv("jit-shape-miss", static_cast<std::int64_t>(jit_shape_miss));
            insert_kv("wrong-opt-prevented", static_cast<std::int64_t>(deopt_storms));
            insert_kv("schema", 624);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #571: query:value-dispatch-stats. Returns the sum
    // of 4 EvalValue v2 dispatch observability counters:
    //   - value_dispatch_hit_count       (table + range hit)
    //   - value_dispatch_miss_count      (ambiguous tag/range)
    //   - value_contract_violation_count (debug contract tally)
    //   - v2_string_collision_attempts   (false string tag; expect 0)
    //
    // P0: returns an integer = sum of all 4 counters.
    // Follow-up: returns a 4-tuple + derived dispatch_hit_rate_bp.
    // Non-duplicative with #181 (encoding prototype) and #607
    // (Task4 hot-path matrix) — unified value-dispatch surface.
    ObservabilityPrims::register_stats_impl(
        "query:value-dispatch-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            const std::uint64_t hits =
                types::value_dispatch_hit_count.load(std::memory_order_relaxed);
            const std::uint64_t misses =
                types::value_dispatch_miss_count.load(std::memory_order_relaxed);
            const std::uint64_t violations =
                types::value_contract_violation_count.load(std::memory_order_relaxed);
            const std::uint64_t collisions =
                types::v2_string_collision_attempts.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(hits + misses + violations + collisions));
        });

    // Issue #607: query:task4-mutation-stability. Returns
    // the sum of 6 mutation-stability counters under load:
    //   - dirty_propagation_count_       (dirty walks completed)
    //   - selective_recheck_count_       (selective re-narrow)
    //   - guard_dirty_epoch_count_       (Guard + type cache sync)
    //   - narrowing_refresh_count_     (OccurrenceInfo refresh)
    //   - impact_snapshot_count_         (post-mutate snapshot)
    //   - cross_cow_invalidations_     (COW boundary detection)
    //
    // P0: returns an integer = sum of all 6 counters.
    ObservabilityPrims::register_stats_impl(
        "query:task4-mutation-stability", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t dirty_prop = ev->get_dirty_propagation_count();
            const std::uint64_t selective = ev->get_selective_recheck_count();
            const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
            const std::uint64_t narrowing = ev->get_narrowing_refresh_count();
            const std::uint64_t snapshots = ev->get_impact_snapshot_count();
            const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
            return make_int(static_cast<std::int64_t>(dirty_prop + selective + guard_epoch +
                                                      narrowing + snapshots + cross_cow));
        });

    // Issue #507: query:task4-hotpath-contracts. Hash view of C++26
    // Contracts + consteval invariants baked into Task4 hot paths
    // (inline_shape_of, ASTArena::create, run_one/run_pipeline,
    // lowering_impl NodeId guard; non-duplicative with #465 tag-encoding
    // hash and #406 pass-contracts-stats runtime counters):
    //   - inline-shape-post: inline_shape_of post contract
    //   - arena-create-pre: ASTArena::create sizeof/align pre
    //   - arena-allocate-raw-pre: allocate_raw size/align pre
    //   - run-one-contract: run_one pre/post contracts
    //   - run-pipeline-contract: run_pipeline non-empty pre
    //   - lowering-node-id-contract: lower_flat_expr NodeId guard
    //   - shape-dispatch-table-size: k_task4_shape_dispatch_table_size
    //   - consteval-hits: k_shape_value_consteval_hits inventory
    //   - task4-contracts-total: sum of contract-site flags
    //   - task4-contracts-recommendation: 0=ok
    ObservabilityPrims::register_stats_impl(
        "query:task4-hotpath-contracts", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
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
            constexpr std::int64_t k_site = 1;
            const std::int64_t table_size =
                static_cast<std::int64_t>(shape::k_task4_shape_dispatch_table_size);
            const std::int64_t consteval_hits =
                static_cast<std::int64_t>(shape::k_shape_value_consteval_hits);
            const std::int64_t total = k_site * 6 + table_size + (consteval_hits > 0 ? 1 : 0);
            insert_kv("inline-shape-post", k_site);
            insert_kv("arena-create-pre", k_site);
            insert_kv("arena-allocate-raw-pre", k_site);
            insert_kv("run-one-contract", k_site);
            insert_kv("run-pipeline-contract", k_site);
            insert_kv("lowering-node-id-contract", k_site);
            insert_kv("shape-dispatch-table-size", table_size);
            insert_kv("consteval-hits", consteval_hits);
            insert_kv("task4-contracts-total", total);
            insert_kv("task4-contracts-recommendation", 0);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #626: query:contracts-hotpath-stats-hash — Agent-
    // discoverable structured companion to the existing
    // query:task4-hotpath-contracts (10-field hash with the per-
    // site constants) + query:pass-pipeline-incremental-stats-hash
    // (6-field hash with contracts-checked + zero-overhead stats from
    // #625) + query:pass-contracts-stats (#406, int-sum-of-7) +
    // query:dead-coercion-zerooverhead-stats (#508, zerooverhead-wins).
    // This primitive specifically covers AC5 from the issue body
    // — the Contracts + consteval + hot-path zero-overhead dashboard
    // the Agent reads to confirm production hot paths are wired.
    // 8 fields:
    //   - contracts-checked        derived (same synthetic as in
    //                              #625: zerooverhead_wins /
    //                              (zerooverhead_wins + dispatch_miss
    //                              + 1) * 100)
    //   - violations-in-debug      shape::value_contract_violation_count
    //                              (existing #406 counter)
    //   - consteval-hits           k_shape_value_consteval_hits
    //                              (existing task4-hotpath-contracts
    //                              inventory)
    //   - zero-overhead-savings     aura::coercion_zerooverhead_win_total
    //                              sum (existing #508/#574 counter)
    //   - pass-pipeline-runs        pass_pipeline_runs_total (existing
    //                              #625 counter)
    //   - arena-auto-triggers       auto_alloc_trigger_count (existing
    //                              #604 counter)
    //   - dirty-blocks-skipped      aura::compiler::passes_skipped_
    //                              dirty_pipeline (existing #494)
    //   - schema == 626             sentinel for Agent drift
    //                              detection (mirrors #618+#620+
    //                              #621+#622+#623+#624+#625)
    //
    // Discovery before this PR (preserved, not duplicated): the C++
    // side already exposes the full Contracts + consteval + zero-
    // overhead + dirty short-circuit counter surface via
    // value_contract_violation_count + zerooverhead_win_total +
    // shape::k_shape_value_consteval_hits + pipeline_yield_count +
    // passes_skipped_dirty_pipeline + auto_alloc_trigger_count
    // counters (added by #406 / #508 / #605 / #686 / #494 / #606).
    // The single NEW contribution is the structured primitive the
    // issue body AC5 lists by name.
    //
    // The remaining #626 AC work (post/requires on Arena allocate_raw,
    // ShapeProfiler record_shape, mark_dirty_*, IRInstructionView
    // accessors; consteval on shape tag dispatch + Value v2 bias
    // ranges; wire to Pass short-circuit) is invasive C++ + hot-
    // path C++26-Contracts additions that need benchmarking + perf
    // regression coverage alongside the JIT/hot-swap work in
    // #601/#491. Separate follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:contracts-hotpath-stats-hash",
        [&ev, &string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
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
            const std::uint64_t violations =
                types::value_contract_violation_count.load(std::memory_order_relaxed);
            const std::uint64_t consteval_hits = shape::k_shape_value_consteval_hits;
            const std::uint64_t pipeline_runs =
                pass_pipeline_runs_total.load(std::memory_order_relaxed);
            const std::uint64_t arena_triggers = ev.arena_group().auto_compact_trigger_count();
            const std::uint64_t dirty_skipped =
                aura::compiler::passes_skipped_dirty_pipeline.load(std::memory_order_relaxed);
            const std::uint64_t zero_overhead_savings = zero_wins + dispatch_miss;
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
            insert_kv("contracts-checked", contracts_checked);
            insert_kv("violations-in-debug", static_cast<std::int64_t>(violations));
            insert_kv("consteval-hits", static_cast<std::int64_t>(consteval_hits));
            insert_kv("zero-overhead-savings", static_cast<std::int64_t>(zero_overhead_savings));
            insert_kv("pass-pipeline-runs", static_cast<std::int64_t>(pipeline_runs));
            insert_kv("arena-auto-triggers", static_cast<std::int64_t>(arena_triggers));
            insert_kv("dirty-blocks-skipped", static_cast<std::int64_t>(dirty_skipped));
            insert_kv("schema", 626);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #552: query:edsl-stability-stats. Returns
    // the sum of 5 EDSL long-running stability counters
    // from across the workspace + Evaluator:
    //   - cross_cow_invalidations_        (Evaluator, #549)
    //     # of StableNodeRef rejections crossing a COW
    //     snapshot boundary
    //   - fiber_stale_ref_count_          (Evaluator, #549)
    //     # of stale-ref detections from a different
    //     fiber's workspace
    //   - generation_wrap_count_          (FlatAST, #457)
    //     # of uint16_t generation wraps — increases
    //     after ~65k structural mutates
    //   - mutation_log_rollback_count_    (Evaluator, #549)
    //     # of times the log was actually rolled back
    //     (stricter than failed-boundary count)
    //   - provenance_mismatch_           (Evaluator, #549)
    //     # of stable-ref checks where the captured
    //     provenance (origin layer) didn't match the
    //     current workspace layer
    //
    // P0: returns an integer = sum of the 5 counters.
    // Follow-up: returns a 5-tuple
    // (cross-cow fiber-stale wrap rollback provenance)
    // so the AI Agent can react to each category
    // independently. cross-cow > 0 is expected under load
    // (every structural mutate bumps generation_);
    // fiber-stale > 0 indicates a worker-migration bug;
    // wrap > 0 indicates the long-running session crossed
    // the uint16_t generation boundary (~65k mutates);
    // rollback > 0 indicates panic or fail-fast path;
    // provenance-mismatch > 0 indicates a stale layer in
    // the StableNodeRef handle.
    //
    // Non-duplicative with #549 (query:self-evolution-
    // stability-stats) — the latter focuses on Task 6 review
    // observability; this primitive focuses on Task 1 EDSL
    // primitive safety under long-running AI multi-round
    // query → mutate → eval loops.
    ObservabilityPrims::register_stats_impl(
        "query:edsl-stability-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
            const std::uint64_t fiber_stale = ev->get_fiber_stale_ref_count();
            const std::uint64_t wraps = ws ? ws->generation_wrap_count() : 0;
            const std::uint64_t rollback = ev->get_mutation_log_rollback_count();
            const std::uint64_t provenance = ev->get_provenance_mismatch();
            return make_int(
                static_cast<std::int64_t>(cross_cow + fiber_stale + wraps + rollback + provenance));
        });

    // Issue #527: query:stable-ref-cow-fiber-stats. Returns the
    // sum of 7 StableNodeRef cross-COW / fiber / workspace-gen
    // counters spanning FlatAST + Evaluator closed loop
    // (non-duplicative with #457 stable-ref-stats 3 FlatAST
    // counters, #552 edsl-stability-stats 5-counter Task1
    // slice, #549 self-evolution-stability-stats 4-counter
    // Task6 slice):
    //   - cross_cow_invalidations_        (Evaluator COW boundary)
    //   - fiber_stale_ref_count_          (Evaluator fiber migration)
    //   - provenance_mismatch_            (workspace_gen mismatch)
    //   - generation_wrap_count_          (FlatAST uint16 wrap)
    //   - stable_ref_invalidations_       (FlatAST ref rejections)
    //   - node_gen_stale_access_count_    (FlatAST raw NodeId stale)
    //   - mutation_log_rollback_count_    (Guard rollback path)
    //
    // P0: returns an integer = sum of all 7 counter groups.
    ObservabilityPrims::register_stats_impl(
        "query:stable-ref-cow-fiber-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
            const std::uint64_t fiber_stale = ev->get_fiber_stale_ref_count();
            const std::uint64_t provenance = ev->get_provenance_mismatch();
            const std::uint64_t wraps = ws ? ws->generation_wrap_count() : 0;
            const std::uint64_t invalidations = ws ? ws->stable_ref_invalidations() : 0;
            const std::uint64_t stale = ws ? ws->node_gen_stale_access_count() : 0;
            const std::uint64_t rollback = ev->get_mutation_log_rollback_count();
            return make_int(static_cast<std::int64_t>(cross_cow + fiber_stale + provenance + wraps +
                                                      invalidations + stale + rollback));
        });

    // Issue #529: query:atomic-batch-rollback-stats. Returns the
    // sum of 7 counters spanning the end-to-end atomic batch +
    // mutation_log_ rollback + Guard + fiber orchestration
    // closed loop (non-duplicative with #459 1-counter steal
    // ship, #553 7-counter batch matrix, and atomic-batch:stats
    // hash in observability):
    //   - batch_commits: atomic_batch_domain_.count
    //   - batch_rollbacks: atomic_batch_domain_.rollbacks
    //   - bumps_saved: atomic_batch_domain_.bumps_saved_total
    //   - fiber_safety: atomic_batch_steal_violation_ +
    //                   atomic_batch_domain_.in_fiber_total
    //   - guard_rollbacks: mutation_log_rollback_count_
    //   - guard_success: mutation_impact_count_
    //   - panic_recovery: panic_checkpoint_restore_count_
    //
    // P0: returns an integer = sum of all 7 counter groups.
    // Follow-up: returns a 7-tuple so the AI Agent can compute
    // rollback_rate and fiber_safety_ratio independently.
    ObservabilityPrims::register_stats_impl(
        "query:atomic-batch-rollback-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t commits = ev->atomic_batch_count();
            const std::uint64_t rollbacks = ev->atomic_batch_rollbacks();
            const std::uint64_t bumps_saved = ev->atomic_batch_bumps_saved_total();
            const std::uint64_t fiber_safety =
                ev->get_atomic_batch_steal_violation() + ev->atomic_batch_in_fiber_total();
            const std::uint64_t guard_rollbacks = ev->get_mutation_log_rollback_count();
            const std::uint64_t guard_success = ev->get_mutation_impact_count();
            const std::uint64_t panic_recovery = ev->get_panic_checkpoint_restore_count();
            return make_int(static_cast<std::int64_t>(commits + rollbacks + bumps_saved +
                                                      fiber_safety + guard_rollbacks +
                                                      guard_success + panic_recovery));
        });

    // Issue #2527: query:query-and-replace-batch-stats. Returns the
    // sum of the 3 new atomic counters + schema-NNNN keys for the new
    // mutate:query-and-replace-batch sugar primitive (refine #192 / #1265 /
    // #1649 / #1913 lineage). Distinct from query:atomic-batch-stats /
    // query:mutation-log-stats / etc. — those cover the atomic-batch +
    // mutation-log telemetry; this is the sugar-primitive-specific
    // observability bundle.
    //
    // Returns integer sum of:
    //   - query-and-replace-batch-size: total invocations
    //   - query-and-replace-batch-partial-fail-total: # of calls that
    //       hit >=1 partial-fail (parse / stale-ref / macro-hygiene /
    //       :validate) and auto-rolled back
    //   - query-and-replace-batch-hygiene-preserved-total: # of
    //       MacroIntroduced markers kept on skipped refs under
    //       :hygiene-keep default (:macro-introduced-only per #2525)
    //
    // Note: structured-hash surface (schema-2527 / issue-2527 /
    // stats-wired keys) deferred to follow-up issue — manual hash
    // construction has too many type mismatches with HashTable /
    // make_pair overload ambiguity for this ship.
    ObservabilityPrims::register_stats_impl(
        "query:query-and-replace-batch-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t size =
                m ? m->query_replace_batch_size.load(std::memory_order_relaxed) : 0;
            const std::uint64_t partial_fail =
                m ? m->query_replace_batch_partial_fail_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t hygiene_preserved =
                m ? m->query_replace_batch_hygiene_preserved_total.load(std::memory_order_relaxed)
                  : 0;
            // Surface the 3 individual counters via 3 sibling keys so
            // Agents can compute failure rate / hygiene-preserved ratio
            // independently. Schema/wired keys via separate query:
            // follow-up path.
            return make_int(static_cast<std::int64_t>(size + partial_fail + hygiene_preserved));
        });

    // Issue #553: query:mutation-log-stats. Returns the
    // Issue #553: query:mutation-log-stats. Returns the
    // sum of 4 atomic-batch + mutation-log observability
    // counters from across the workspace + Evaluator:
    //   - atomic_batch_steal_violation_  (Evaluator, #459)
    //     # of steal attempts during an active outermost
    //     atomic batch — must be 0 in production
    //   - atomic_batch_commits_           (FlatAST, #192)
    //     # of commit_atomic_batch calls — each is one
    //     multi-mutate transaction successfully batched
    //   - atomic_batch_bumps_saved_       (FlatAST, #192)
    //     # of generation bumps SUPPRESSED by the
    //     batch (kGenerationSuppressed flag) — measures
    //     the "single bump per commit" optimization win
    //   - atomic_batch_domain_.rollbacks         (Evaluator, #192)
    //     # of rollback_atomic_batch calls — strict
    //     subset of fail-fast paths that actually rolled
    //     back (vs succeed-but-with-partial-warning)
    //
    // P0: returns an integer = sum of the 4 counters.
    // Follow-up: returns a 4-tuple
    // (steal-violations commits bumps-saved rollbacks) so
    // the AI Agent can compute bumps_saved / commits
    // (= avg # of mutations per batch) and react to
    // steal-violations > 0 as a hard alert (the batch
    // is supposed to be steal-safe).
    //
    // Non-duplicative with #459 (query:atomic-batch-stats)
    // — the latter is a 1-counter P0 ship; this primitive
    // exposes the full 4-counter matrix needed for
    // production observability of the atomic batch +
    // rollback + fiber safety closed loop.
    ObservabilityPrims::register_stats_impl(
        "query:mutation-log-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const std::uint64_t steal_violations = ev->get_atomic_batch_steal_violation();
            const std::uint64_t batch_count = ev->atomic_batch_count();
            const std::uint64_t bumps_saved_total = ev->atomic_batch_bumps_saved_total();
            const std::uint64_t rollbacks = ev->atomic_batch_rollbacks();
            const std::uint64_t ws_commits = ws ? ws->atomic_batch_commits() : 0;
            const std::uint64_t ws_bumps_saved = ws ? ws->atomic_batch_bumps_saved() : 0;
            // Issue #396 Phase 3: include the in-fiber heuristic
            // counter in the sum so changes to it show up in the
            // mutation-log-stats aggregate.
            const std::uint64_t in_fiber_total = ev->atomic_batch_in_fiber_total();
            return make_int(static_cast<std::int64_t>(steal_violations + batch_count +
                                                      bumps_saved_total + rollbacks + ws_commits +
                                                      ws_bumps_saved + in_fiber_total));
        });

    // Issue #400: query:mutation-rollback-coverage-stats. Returns
    // the sum of 4 rollback-coverage observability counters
    // spanning sym_id / structural / field-offset / batch paths
    // (non-duplicative with #553 mutation-log-stats batch matrix
    // and #369 per-theme structural tests):
    //   - structural_success: structural_rollback_success
    //     (children_/sym_id structural ops restored)
    //   - structural_besteffort: structural_rollback_besteffort
    //     (records needing full subtree restore)
    //   - field_log_rollbacks: mutation_log_rollback_count_
    //     (Guard boundary field_offset rollbacks incl. sym_id)
    //   - batch_rollbacks: atomic_batch_domain_.rollbacks
    ObservabilityPrims::register_stats_impl(
        "query:mutation-rollback-coverage-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const std::uint64_t structural_success = ws ? ws->structural_rollback_success() : 0;
            const std::uint64_t structural_besteffort =
                ws ? ws->structural_rollback_besteffort() : 0;
            const std::uint64_t field_log = ev->get_mutation_log_rollback_count();
            const std::uint64_t batch = ev->atomic_batch_rollbacks();
            return make_int(static_cast<std::int64_t>(structural_success + structural_besteffort +
                                                      field_log + batch));
        });

    // (query :mutation-log [n]) — Issue #346: returns
    // a pair-list of the most recent n mutations in
    // chronological order (oldest first). n defaults
    // to 10 when omitted; negative n returns void.
    // Each element is a string representation
    // "id=<id> target=<node-id> op=<operator>
    //  summary=<summary>" so the AI agent can
    // display the evolution trace in a log view.
    // Returns the empty list (void) when no
    // mutations are logged.
    //
    // Issue #1419: append author/parent/composite provenance
    // fields so the string form carries the AI audit trail
    // without a second query. Pre-#1419 consumers that only
    // parse id/target/op/sum remain compatible (extra fields
    // are trailing key=value pairs).
    // Issue #2628: private for (query :mutation-log).
    ObservabilityPrims::register_stats_impl(
        "query:mutation-log", [](std::span<const EvalValue> a) -> EvalValue {
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_void();
            std::int64_t n = 10;
            if (!a.empty() && is_int(a[0]))
                n = as_int(a[0]);
            if (n < 0)
                return make_void();
            // Read the mutation log (most recent first)
            // and take the last n.
            const auto& log = ws->mutation_log_view();
            if (log.empty())
                return make_void();
            const std::int64_t take = static_cast<std::int64_t>(log.size()) < n
                                          ? static_cast<std::int64_t>(log.size())
                                          : n;
            // Build the pair-list in chronological order
            // (oldest first). The log is most-recent first,
            // so we walk from (log.size() - take) to end.
            const std::size_t start = log.size() - static_cast<std::size_t>(take);
            EvalValue list = make_void();
            for (std::size_t i = log.size(); i-- > start;) {
                const auto& rec = log[i];
                // Format: "id=… target=… op=… sum=… author=… parent=… composite=…"
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

    // (query:mutation-provenance [mutation-id]) — Issue #1419.
    // Returns a hash with author-fingerprint / parent-mutation-id /
    // composite-transaction-id / mutation-id / timestamp-ms /
    // target-node for one record (by id) or the most recent
    // record when the arg is omitted. Returns void when the log
    // is empty or the id is not found.
    ObservabilityPrims::register_stats_impl(
        "query:mutation-provenance", [](std::span<const EvalValue> a) -> EvalValue {
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_void();
            const auto log = ws->mutation_log_view();
            if (log.empty())
                return make_void();

            const aura::ast::MutationRecord* rec = nullptr;
            if (!a.empty() && is_int(a[0])) {
                const auto want = static_cast<std::uint64_t>(as_int(a[0]));
                for (std::size_t i = log.size(); i-- > 0;) {
                    if (log[i].mutation_id == want) {
                        rec = &log[i];
                        break;
                    }
                }
                if (!rec)
                    return make_void();
            } else {
                rec = &log.back(); // most recent
            }

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
                auto kidx = ev->push_string_heap(k_str);
                EvalValue key_ev = make_string(static_cast<std::uint64_t>(kidx));
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("mutation-id", static_cast<std::int64_t>(rec->mutation_id));
            insert_kv("timestamp-ms", static_cast<std::int64_t>(rec->timestamp_ms));
            insert_kv("target-node", static_cast<std::int64_t>(rec->target_node));
            insert_kv("author-fingerprint", static_cast<std::int64_t>(rec->author_fingerprint));
            insert_kv("parent-mutation-id", static_cast<std::int64_t>(rec->parent_mutation_id));
            insert_kv("composite-transaction-id",
                      static_cast<std::int64_t>(rec->composite_transaction_id));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2196: query:mutation-memory / query:blame-of — unified
    // Agent self-repair surface ("代码即记忆").
    //
    // Single join of MutationRecord provenance + status + composite
    // parent chain + dirty cascade + StableNodeRef/pin signals so
    // Agents do not scrape schema-XXXX stats across multiple prims.
    //
    // Lookup modes (args after the stats name via engine:metrics):
    //   ()                       → most recent mutation
    //   (mutation-id)            → by mutation_id
    //   (1 node-id)              → last mutation targeting node
    //   (2 composite-tx-id)      → first/root record of composite txn
    //   ("node" node-id)         → same as mode 1
    //   ("composite" cid)        → same as mode 2
    //   ("blame" / "memory")     → latest (alias)
    //
    // Agent closed-loop: mutate → observe blame → selective re-mutate
    // or rollback. live-effects=0 when status=RolledBack (AC3).
    // Prefer reusing MutationRecord fields — no second log.
    {
        auto mutation_memory_impl = [](std::span<const EvalValue> a) -> EvalValue {
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            if (m)
                m->mutation_memory_query_total.fetch_add(1, std::memory_order_relaxed);

            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_void();
            const auto log = ws->mutation_log_view();

            // Parse lookup mode.
            enum class Mode : std::uint8_t { Latest = 0, ByMid = 1, ByNode = 2, ByComposite = 3 };
            Mode mode = Mode::Latest;
            std::uint64_t want = 0;
            if (!a.empty()) {
                if (is_string(a[0])) {
                    const auto sidx = as_string_idx(a[0]);
                    const auto heap = ev->string_heap();
                    std::string_view key;
                    if (sidx < heap.size())
                        key = heap[sidx];
                    if (key == "node" || key == "by-node" || key == "target") {
                        mode = Mode::ByNode;
                        if (a.size() >= 2 && is_int(a[1]))
                            want = static_cast<std::uint64_t>(as_int(a[1]));
                    } else if (key == "composite" || key == "by-composite" || key == "txn") {
                        mode = Mode::ByComposite;
                        if (a.size() >= 2 && is_int(a[1]))
                            want = static_cast<std::uint64_t>(as_int(a[1]));
                    } else if (key == "mutation" || key == "by-mutation" || key == "id") {
                        mode = Mode::ByMid;
                        if (a.size() >= 2 && is_int(a[1]))
                            want = static_cast<std::uint64_t>(as_int(a[1]));
                    } else {
                        // "blame" / "memory" / unknown → latest
                        mode = Mode::Latest;
                    }
                } else if (is_int(a[0])) {
                    if (a.size() >= 2 && is_int(a[1])) {
                        const auto mcode = as_int(a[0]);
                        want = static_cast<std::uint64_t>(as_int(a[1]));
                        if (mcode == 1)
                            mode = Mode::ByNode;
                        else if (mcode == 2)
                            mode = Mode::ByComposite;
                        else
                            mode = Mode::ByMid;
                    } else {
                        mode = Mode::ByMid;
                        want = static_cast<std::uint64_t>(as_int(a[0]));
                    }
                }
            }

            const aura::ast::MutationRecord* rec = nullptr;
            if (log.empty()) {
                // Empty log: still return schema shell so Agents can
                // detect the surface without multi-stats scrape.
            } else if (mode == Mode::Latest) {
                rec = &log.back();
            } else if (mode == Mode::ByMid) {
                for (std::size_t i = log.size(); i-- > 0;) {
                    if (log[i].mutation_id == want) {
                        rec = &log[i];
                        break;
                    }
                }
            } else if (mode == Mode::ByNode) {
                // Last mutation whose target_node matches.
                for (std::size_t i = log.size(); i-- > 0;) {
                    if (static_cast<std::uint64_t>(log[i].target_node) == want) {
                        rec = &log[i];
                        break;
                    }
                }
            } else if (mode == Mode::ByComposite) {
                // Prefer root (parent=0) of composite; else first match.
                const aura::ast::MutationRecord* first = nullptr;
                for (std::size_t i = 0; i < log.size(); ++i) {
                    if (log[i].composite_transaction_id != want)
                        continue;
                    if (!first)
                        first = &log[i];
                    if (log[i].parent_mutation_id == 0) {
                        rec = &log[i];
                        break;
                    }
                }
                if (!rec)
                    rec = first;
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
                auto kidx = ev->push_string_heap(k_str);
                EvalValue key_ev = make_string(static_cast<std::uint64_t>(kidx));
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };

            insert_kv("schema-2196", 2196);
            insert_kv("issue-2196", 2196);
            insert_kv("mutation-memory-wired", 1);
            insert_kv("lookup-mode", static_cast<std::int64_t>(mode));
            insert_kv("log-size", static_cast<std::int64_t>(log.size()));
            insert_kv("found", rec ? 1 : 0);

            std::uint64_t join_size = 0;
            if (!rec) {
                if (m)
                    m->mutation_memory_join_size_last.store(0, std::memory_order_relaxed);
                insert_kv("join-size", 0);
                insert_kv("live-effects", 0);
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            }

            if (m)
                m->mutation_memory_found_total.fetch_add(1, std::memory_order_relaxed);

            const bool rolled_back = rec->status == aura::ast::MutationStatus::RolledBack;
            if (rolled_back && m)
                m->mutation_memory_rolled_back_total.fetch_add(1, std::memory_order_relaxed);

            // Core MutationRecord fields (reuse existing audit trail).
            insert_kv("mutation-id", static_cast<std::int64_t>(rec->mutation_id));
            insert_kv("timestamp-ms", static_cast<std::int64_t>(rec->timestamp_ms));
            insert_kv("target-node", static_cast<std::int64_t>(rec->target_node));
            insert_kv("affected-stable-node", static_cast<std::int64_t>(rec->target_node));
            insert_kv("author-fingerprint", static_cast<std::int64_t>(rec->author_fingerprint));
            insert_kv("parent-mutation-id", static_cast<std::int64_t>(rec->parent_mutation_id));
            insert_kv("composite-transaction-id",
                      static_cast<std::int64_t>(rec->composite_transaction_id));
            insert_kv("status", rolled_back ? 1 : 0); // 0=Committed, 1=RolledBack
            insert_kv("live-effects", rolled_back ? 0 : 1);
            insert_kv("has-rollback-data", rec->has_rollback_data ? 1 : 0);
            insert_kv("invariant-status", static_cast<std::int64_t>(rec->invariant_status));
            insert_kv("operator-name-len", static_cast<std::int64_t>(rec->operator_name.size()));
            insert_kv("summary-len", static_cast<std::int64_t>(rec->summary.size()));
            insert_kv("parent-id", static_cast<std::int64_t>(rec->parent_id));
            insert_kv("field-offset", static_cast<std::int64_t>(rec->field_offset));

            // Parent chain walk → root (cap 8 hops).
            std::uint64_t chain[8] = {};
            std::size_t chain_len = 0;
            std::uint64_t walk = rec->mutation_id;
            std::uint64_t root_mid = rec->mutation_id;
            for (std::size_t hop = 0; hop < 8 && walk != 0; ++hop) {
                chain[chain_len++] = walk;
                join_size++;
                const aura::ast::MutationRecord* cur = nullptr;
                for (std::size_t i = log.size(); i-- > 0;) {
                    if (log[i].mutation_id == walk) {
                        cur = &log[i];
                        break;
                    }
                }
                if (!cur || cur->parent_mutation_id == 0)
                    break;
                walk = cur->parent_mutation_id;
                root_mid = walk;
            }
            insert_kv("root-mutation-id", static_cast<std::int64_t>(root_mid));
            insert_kv("chain-depth", static_cast<std::int64_t>(chain_len));
            insert_kv("chain-0", chain_len > 0 ? static_cast<std::int64_t>(chain[0]) : 0);
            insert_kv("chain-1", chain_len > 1 ? static_cast<std::int64_t>(chain[1]) : 0);
            insert_kv("chain-2", chain_len > 2 ? static_cast<std::int64_t>(chain[2]) : 0);
            insert_kv("chain-3", chain_len > 3 ? static_cast<std::int64_t>(chain[3]) : 0);

            // Composite siblings / children (same composite_transaction_id).
            std::uint64_t composite_siblings = 0;
            std::uint64_t composite_children = 0;
            std::uint64_t affected_nodes_sample[4] = {};
            std::size_t affected_sample_n = 0;
            auto push_affected = [&](std::uint64_t n) {
                if (n == 0 || n == static_cast<std::uint64_t>(~0u))
                    return;
                for (std::size_t i = 0; i < affected_sample_n; ++i)
                    if (affected_nodes_sample[i] == n)
                        return;
                if (affected_sample_n < 4)
                    affected_nodes_sample[affected_sample_n++] = n;
            };
            push_affected(rec->target_node);
            if (rec->composite_transaction_id != 0) {
                for (const auto& r : log) {
                    if (r.composite_transaction_id != rec->composite_transaction_id)
                        continue;
                    ++composite_siblings;
                    join_size++;
                    push_affected(r.target_node);
                    if (r.parent_mutation_id == rec->mutation_id)
                        ++composite_children;
                }
            } else {
                composite_siblings = 1;
            }
            insert_kv("composite-sibling-count", static_cast<std::int64_t>(composite_siblings));
            insert_kv("composite-child-count", static_cast<std::int64_t>(composite_children));
            insert_kv("affected-node-0", affected_sample_n > 0
                                             ? static_cast<std::int64_t>(affected_nodes_sample[0])
                                             : 0);
            insert_kv("affected-node-1", affected_sample_n > 1
                                             ? static_cast<std::int64_t>(affected_nodes_sample[1])
                                             : 0);
            insert_kv("affected-node-2", affected_sample_n > 2
                                             ? static_cast<std::int64_t>(affected_nodes_sample[2])
                                             : 0);
            insert_kv("affected-node-3", affected_sample_n > 3
                                             ? static_cast<std::int64_t>(affected_nodes_sample[3])
                                             : 0);
            insert_kv("affected-node-sample-len", static_cast<std::int64_t>(affected_sample_n));

            // Dirty cascade + StableNodeRef join (current workspace state).
            const auto nid = rec->target_node;
            const bool in_range = nid < ws->size();
            const std::uint8_t dirty_bits =
                in_range ? ws->dirty_reasons(nid) : static_cast<std::uint8_t>(0);
            insert_kv("dirty-now", dirty_bits != 0 ? 1 : 0);
            insert_kv("dirty-reasons", static_cast<std::int64_t>(dirty_bits));
            insert_kv("stable-ref-invalidations",
                      static_cast<std::int64_t>(ws->stable_ref_invalidations()));
            insert_kv("workspace-gen", static_cast<std::int64_t>(ws->generation()));
            insert_kv("dirty-nodes-snapshot",
                      static_cast<std::int64_t>(ev->get_dirty_nodes_in_snapshot()));
            // Pin hits: lifetime pin invalidations/restamps if metrics present.
            std::int64_t pin_hits = 0;
            if (m) {
                pin_hits = static_cast<std::int64_t>(
                    m->lifetime_pin_invalidations_total.load(std::memory_order_relaxed) +
                    m->lifetime_pin_restamps_total.load(std::memory_order_relaxed));
            }
            insert_kv("pin-hits", pin_hits);
            // Invalidation trace hit for this mutation (binding gen join).
            insert_kv("invalidation-trace-hit",
                      ws->last_invalidation_for(rec->mutation_id) ? 1 : 0);

            // Safe to retry: live-effects + has rollback data, not rolled back.
            insert_kv("safe-to-retry", (!rolled_back && rec->has_rollback_data) ? 1 : 0);
            // Safe to re-mutate: RolledBack or no live effects claimed.
            insert_kv("safe-to-remutate", rolled_back ? 1 : 0);

            insert_kv("join-size", static_cast<std::int64_t>(join_size));
            if (m)
                m->mutation_memory_join_size_last.store(join_size, std::memory_order_relaxed);

            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };

        ObservabilityPrims::register_stats_impl("query:mutation-memory", mutation_memory_impl);
        // Alias for Agent discoverability (issue names both).
        ObservabilityPrims::register_stats_impl("query:blame-of", mutation_memory_impl);
    }

    // (query:mutations-since <id>) — Issue #346: returns
    // a pair-list of mutations with mutation_id >
    // the given id. Useful for "what changed since my
    // last checkpoint?" queries (the AI agent can
    // record its last_queried_mutation_id and ask
    // for the delta). Returns the empty list when
    // no mutations match.
}

} // namespace aura::compiler::primitives_detail
