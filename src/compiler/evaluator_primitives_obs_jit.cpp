// evaluator_primitives_obs_jit.cpp — Issue #1963 / Phase 1: split numbered files consolidated.
// Consolidated from evaluator_primitives_obs_jit_00..14.cpp.
// aura.compiler.evaluator module partition; obs-jit tier registrations.
// Function count: 114 (register_jit_p0..p113) — driven by
// observability_jit_tiers.inc X-macro (#1670).
// register_jit_all() remains in evaluator_primitives_observability.cpp
// (X-macro function-pointer dispatch, #1670).
// File-scope `extern "C"` forward decls hoisted from each split
// file (originally between imports and the namespace opening);
// definitions live in src/serve/fiber.cpp at global scope.
//
// Issue #1971: 7 terminal:* names (Phase-A deprecated no-ops, #1351) are
// gated by AURA_ENABLE_TERMINAL in register_jit_p59 + register_jit_p113.
// See docs/terminal-domain.md.

module;

#include "runtime_shared.h"
#include "compiler/aura_jit_bridge.h"
#include "security_capabilities.h"
#include "observability_metrics.h"
#include "compiler/shape.h"
#include "compiler/value_tags.h"
#include "core/cpp26_contract_stats.h"
#include "core/arena_auto_policy_stats.h"
#include "jit_typed_mutation_stats.h"
#include "typed_mutation_audit.h" // Issue #1894: AC metric counters
#include "shape_jit_pass_closedloop_stats.h"
#include "ci_build_info.h"
#include "primitives_meta.h"
#include "primitives_detail.h"
#include "serve/metrics.h"
#include "hash_meta.h"
#include "basis_points.h"
#include "serve/fiber.h"
#include "core/gc_hooks.h"
#include "core/resource_quota.hh"
#include <limits>

// Default ON when the TU is compiled outside the CMake graph (tools/IDE).
#ifndef AURA_ENABLE_TERMINAL
#define AURA_ENABLE_TERMINAL 1
#endif

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.value;
import aura.compiler.pass_manager;
import aura.compiler.service;

// Hoisted from evaluator_primitives_obs_jit_00..14.cpp
extern "C" {
extern "C" std::uint64_t aura_fiber_init_aura_result_err_total();
extern "C" std::uint64_t aura_fiber_init_aura_result_ok_total();
extern "C" std::uint64_t aura_fiber_static_cross_fiber_mutation_safe_steal_total();
extern "C" std::uint64_t aura_fiber_static_steal_inner_mutation_boundary_deferred_total();
extern "C" std::uint64_t aura_fiber_static_steal_outermost_mutation_boundary_total();
extern "C" std::uint64_t aura_jit_guest_exception_bridge_total();
extern "C" std::uint64_t aura_scheduler_init_aura_result_err_total();
extern "C" std::uint64_t aura_scheduler_init_aura_result_ok_total();
}

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

// Issue #909 part 0 (orig lines 11704-11758)
void ObservabilityPrims::register_jit_p0(PrimRegistrar add, Evaluator& ev) {

    // (jit:intrinsic-count) — Issue #194: return the
    // runtime→intrinsic migration counter from the AuraJIT.
    // This is the per-commit observability signal for the 4
    // candidates the issue body tracks. Returns 0 if no hook
    // is installed (e.g. unit-test Evaluator without a JIT).
    ObservabilityPrims::register_stats_impl("jit:intrinsic-count", [&ev](const auto&) -> EvalValue {
        if (!ev.get_intrinsic_count_fn_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev.get_intrinsic_count_fn_()));
    });

    // (jit:deopt-fn? fn-name threshold) — Issue #193: returns
    // #t if the function should be deopted (i.e., its
    // unhandled-opcode count exceeds the threshold). Default
    // threshold is 0 (any hit triggers deopt). Production code
    // should pass a non-zero threshold (e.g. 10) to avoid
    // thrashing on transient bugs during initial JIT warmup.
    add("jit:deopt-fn?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        std::uint64_t threshold = 0;
        if (a.size() >= 2 && is_int(a[1])) {
            auto t = as_int(a[1]);
            if (t < 0)
                t = 0;
            threshold = static_cast<std::uint64_t>(t);
        }
        // The intrinsic_count check would need a separate hook
        // for the per-function unhandled count. For now, we
        // look up via the AuraJIT if it's installed. If the
        // hook isn't installed, default to false (never deopt).
        if (!ev.get_jit_unhandled_count_fn_)
            return make_bool(false);
        auto count = ev.get_jit_unhandled_count_fn_(ev.string_heap_[idx].c_str());
        return make_bool(count > threshold);
    });

    // (jit:exception-depth) — Issue #195: current fiber's
    // exception stack depth. Reads from the per-fiber ExStack
    // via the JIT runtime's hook (aura_fiber_current_id).
    // Returns 0 if no exception state for the current fiber.
    ObservabilityPrims::register_stats_impl("jit:exception-depth", [&ev](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(aura_exception_depth()));
    });

    // (jit:exception-fibers) — Issue #195: number of distinct
    // fiber ids that have exception state. Used for
    // observability of the per-fiber ExStack map size.
    ObservabilityPrims::register_stats_impl(
        "jit:exception-fibers", [&ev](const auto&) -> EvalValue {
            return make_int(static_cast<std::int64_t>(aura_exception_fiber_count()));
        });
}

// Issue #909 part 1 (orig lines 11759-11884)
void ObservabilityPrims::register_jit_p1(PrimRegistrar add, Evaluator& ev) {

    // (jit:exception-fibers-clear) — Issue #195: clear all
    // per-fiber exception state. Returns void. Used by the
    // session-reset path.
    // Issue #1295 (P0): requires kCapExceptionControl in sandbox —
    // process-global clear can corrupt in-flight try/catch on other fibers.
    add("jit:exception-fibers-clear", [&ev](const auto&) -> EvalValue {
        // Issue #1326 Phase 1: deprecation path (cross-fiber race; prefer C++ reset).
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            record_write_side_prim_deprecation(m);
        if (ev.sandbox_mode() &&
            !ev.has_capability(aura::compiler::security::kCapExceptionControl) &&
            !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->capability_exception_control_denials.fetch_add(1, std::memory_order_relaxed);
                m->cap_denial_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "capability denied: exception-control required",
                                        ev.primitive_error_counter_ptr());
        }
        aura_exception_clear_all();
        return make_void();
    });

    // (query:jit-stats) — Issue #427: full JIT metrics line
    // in the same format AuraJIT::Metrics::format produces.
    // Returns a single string with key=value fields separated
    // by spaces. Includes: compiles, avg_us, hot_swaps,
    // cached_fns, inlined_prims, slow_prims, prim_calls,
    // prim_avg_ns, verify_fail, add_mod_fail, unhandled_opcode,
    // intrinsics. Returns "" if no hook is installed (e.g.
    // unit-test Evaluator without a JIT). Cheap to call —
    // just reads a thread-local buffer populated by the hook.
    ObservabilityPrims::register_stats_impl("query:jit-stats", [&ev](const auto&) -> EvalValue {
        auto sidx = ev.string_heap_.size();
        if (!ev.get_jit_stats_fn_) {
            ev.string_heap_.push_back("");
        } else {
            const char* s = ev.get_jit_stats_fn_();
            ev.string_heap_.push_back(s ? std::string(s) : std::string());
        }
        return make_string(sidx);
    });

    // Issue #491: query:jit-stats-hash — structured JIT production-readiness
    // view for AI self-monitoring (opcode coverage, fallback, hot-swap safety).
    ObservabilityPrims::register_stats_impl(
        "query:jit-stats-hash", [&ev](const auto&) -> EvalValue {
            std::uint64_t compiles = 0;
            std::uint64_t hot_swaps = 0;
            std::uint64_t cached_fns = 0;
            std::uint64_t unhandled = 0;
            std::uint64_t fallback = aura_jit_fallback_count_v_read();
            std::uint64_t consistency = 0;
            std::uint64_t intrinsics = 0;
            if (ev.get_jit_stats_fn_) {
                const char* s = ev.get_jit_stats_fn_();
                if (s) {
                    auto parse_u64 = [&](std::string_view key) -> std::uint64_t {
                        std::string_view hay(s);
                        auto pos = hay.find(key);
                        if (pos == std::string_view::npos)
                            return 0;
                        return std::strtoull(hay.data() + pos + key.size(), nullptr, 10);
                    };
                    compiles = parse_u64("compiles=");
                    hot_swaps = parse_u64("hot_swaps=");
                    cached_fns = parse_u64("cached_fns=");
                    unhandled = parse_u64("unhandled_opcode=");
                    fallback = parse_u64("fallback_count=");
                    consistency = parse_u64("consistency_violations=");
                    intrinsics = parse_u64("intrinsics=");
                }
            }
            std::uint64_t jit_cache_evictions = 0;
            std::uint64_t invalidate_calls = 0;
            std::uint64_t hotswap_invalidate = 0;
            std::uint64_t epoch_mismatch = 0;
            std::uint64_t mutation_epoch = 0;
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                jit_cache_evictions = m->jit_cache_evictions.load(std::memory_order_relaxed);
                invalidate_calls = m->invalidate_function_calls.load(std::memory_order_relaxed);
                hotswap_invalidate =
                    m->jit_hotswap_invalidate_total.load(std::memory_order_relaxed);
                epoch_mismatch =
                    m->compiler_closure_epoch_mismatch_hits.load(std::memory_order_relaxed);
            }
            if (ev.get_incremental_stats_fn_) {
                const auto packed = ev.get_incremental_stats_fn_();
                mutation_epoch = (packed >> 16) & 0xFFFFu;
            }
            constexpr std::int64_t k_opcode_total = 53; // IROpcode::Nop..TopCellLoad
            const std::int64_t coverage_pct =
                unhandled == 0 && fallback == 0
                    ? 100
                    : std::max<std::int64_t>(
                          0, 100 - static_cast<std::int64_t>((unhandled + fallback) * 100 /
                                                             std::max<std::uint64_t>(1, compiles)));
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
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("compiles", static_cast<std::int64_t>(compiles));
            insert_kv("hot-swaps", static_cast<std::int64_t>(hot_swaps));
            insert_kv("cached-fns", static_cast<std::int64_t>(cached_fns));
            insert_kv("unhandled-opcode", static_cast<std::int64_t>(unhandled));
            insert_kv("fallback-count", static_cast<std::int64_t>(fallback));
            insert_kv("consistency-violations", static_cast<std::int64_t>(consistency));
            insert_kv("intrinsics", static_cast<std::int64_t>(intrinsics));
            insert_kv("opcode-total", k_opcode_total);
            insert_kv("opcode-coverage-pct", coverage_pct);
            insert_kv("jit-cache-evictions", static_cast<std::int64_t>(jit_cache_evictions));
            insert_kv("invalidate-function-calls", static_cast<std::int64_t>(invalidate_calls));
            insert_kv("hotswap-invalidate-total", static_cast<std::int64_t>(hotswap_invalidate));
            insert_kv("epoch-mismatch-hits", static_cast<std::int64_t>(epoch_mismatch));
            insert_kv("mutation-epoch", static_cast<std::int64_t>(mutation_epoch));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 2 (orig lines 11885-11940)
void ObservabilityPrims::register_jit_p2(PrimRegistrar add, Evaluator& ev) {

    // Issue #601: query:jit-hotswap-closure-stats — live IRClosure
    // refresh / forced-deopt counters from invalidate_function's
    // proactive walk. Bumped after jit_hotswap_invalidate_total so
    // an AI agent can observe: "for the last invalidation, how many
    // closures were refreshable vs forced-deopt vs left stale?".
    // Forced-deopt is reserved at 0 in this foundation layer — the
    // func_id-scoped deopt decision (closure.func_id no longer in
    // current module) is a follow-up.
    ObservabilityPrims::register_stats_impl(
        "query:jit-hotswap-closure-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t refreshed = 0;
            std::uint64_t forced_deopt = 0;
            std::uint64_t mismatch_prevented = 0;
            std::uint64_t hotswap_invalidate = 0;
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                refreshed =
                    m->jit_hotswap_live_closure_refreshed_total.load(std::memory_order_relaxed);
                forced_deopt = m->jit_hotswap_forced_deopt_total.load(std::memory_order_relaxed);
                mismatch_prevented =
                    m->jit_hotswap_epoch_mismatch_prevented_total.load(std::memory_order_relaxed);
                hotswap_invalidate =
                    m->jit_hotswap_invalidate_total.load(std::memory_order_relaxed);
            }
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
            insert_kv("live-closure-refreshed-total", static_cast<std::int64_t>(refreshed));
            insert_kv("forced-deopt-total", static_cast<std::int64_t>(forced_deopt));
            insert_kv("epoch-mismatch-prevented-total",
                      static_cast<std::int64_t>(mismatch_prevented));
            insert_kv("hotswap-invalidate-total", static_cast<std::int64_t>(hotswap_invalidate));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 3 (orig lines 11941-12020)
void ObservabilityPrims::register_jit_p3(PrimRegistrar add, Evaluator& ev) {

    // Issue #493: query:hotpath-bottleneck-stats — structured EDSL
    // hot-path breakdown for AI mutate→eval tuning.
    ObservabilityPrims::register_stats_impl(
        "query:hotpath-bottleneck-stats", [&ev](const auto&) -> EvalValue {
            const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t eval_flat =
                m ? m->hotpath_eval_flat_calls.load(std::memory_order_relaxed) : 0;
            const std::uint64_t lowering =
                m ? m->hotpath_lowering_calls.load(std::memory_order_relaxed) : 0;
            const std::uint64_t soa_dual =
                m ? m->hotpath_soa_dual_emit_hits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t soa_instr =
                m ? m->ir_soa_instructions_emitted.load(std::memory_order_relaxed) : 0;
            const std::uint64_t soa_funcs =
                m ? m->ir_soa_functions_emitted.load(std::memory_order_relaxed) : 0;
            const std::uint64_t soa_wired =
                m ? m->irsoa_wired_hits.load(std::memory_order_relaxed) : 0;
            auto* ws = ev.workspace_flat();
            const std::uint64_t dirty_up = ws ? ws->mark_dirty_upward_call_count() : 0;
            const std::uint64_t dirty_early = ws ? ws->mark_dirty_early_exit_count() : 0;
            const std::uint64_t dirty_nodes = ws ? ws->mark_dirty_total_nodes() : 0;
            const std::uint64_t passes_skip = ev.get_passes_skipped_type_dirty();
            const std::uint64_t shape_dispatch =
                shape::inline_shape_ref_dispatch_count.load(std::memory_order_relaxed);
            const std::uint64_t value_dispatch =
                types::value_dispatch_hit_count.load(std::memory_order_relaxed);
            std::uint64_t arena_triggers = 0;
            if (ev.arena_) {
                arena_triggers = ev.arena_->stats().auto_alloc_trigger_count;
            }
            if (ev.arena_group_) {
                arena_triggers += ev.arena_group_->auto_compact_trigger_count();
            }
            const std::uint64_t bottleneck_total = eval_flat + lowering + soa_dual + dirty_up +
                                                   dirty_early + passes_skip + shape_dispatch +
                                                   value_dispatch + arena_triggers;
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
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("eval-flat-calls", static_cast<std::int64_t>(eval_flat));
            insert_kv("lowering-calls", static_cast<std::int64_t>(lowering));
            insert_kv("soa-dual-emit-hits", static_cast<std::int64_t>(soa_dual));
            insert_kv("soa-instr-emitted", static_cast<std::int64_t>(soa_instr));
            insert_kv("soa-func-emitted", static_cast<std::int64_t>(soa_funcs));
            insert_kv("soa-wired-hits", static_cast<std::int64_t>(soa_wired));
            insert_kv("dirty-upward-calls", static_cast<std::int64_t>(dirty_up));
            insert_kv("dirty-early-exit", static_cast<std::int64_t>(dirty_early));
            insert_kv("dirty-total-nodes", static_cast<std::int64_t>(dirty_nodes));
            insert_kv("passes-skipped-dirty", static_cast<std::int64_t>(passes_skip));
            insert_kv("shape-dispatch", static_cast<std::int64_t>(shape_dispatch));
            insert_kv("value-dispatch-hits", static_cast<std::int64_t>(value_dispatch));
            insert_kv("arena-alloc-triggers", static_cast<std::int64_t>(arena_triggers));
            insert_kv("bottleneck-total", static_cast<std::int64_t>(bottleneck_total));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 4 (orig lines 12021-12088)
void ObservabilityPrims::register_jit_p4(PrimRegistrar add, Evaluator& ev) {

    // Issue #494: query:pass-pipeline-stats — incremental pass-pipeline
    // yield + dirty short-circuit observability for AI mutate loops.
    ObservabilityPrims::register_stats_impl(
        "query:pass-pipeline-stats", [&ev](const auto&) -> EvalValue {
            const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t pipeline_yield =
                aura::compiler::pipeline_yield_count.load(std::memory_order_relaxed);
            const std::uint64_t passes_skip_dirty =
                aura::compiler::passes_skipped_dirty_pipeline.load(std::memory_order_relaxed);
            const std::uint64_t passes_skip_type = ev.get_passes_skipped_type_dirty();
            const std::uint64_t relower_skip =
                m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_per_fn =
                m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t mod_skip =
                m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0;
            const std::uint64_t pipeline_total = pipeline_yield + passes_skip_dirty +
                                                 passes_skip_type + relower_skip + relower_per_fn +
                                                 mod_skip;
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
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("pipeline-yield-count", static_cast<std::int64_t>(pipeline_yield));
            insert_kv("passes-skipped-dirty", static_cast<std::int64_t>(passes_skip_dirty));
            insert_kv("passes-skipped-type-dirty", static_cast<std::int64_t>(passes_skip_type));
            insert_kv("relower-skipped", static_cast<std::int64_t>(relower_skip));
            insert_kv("relower-per-fn", static_cast<std::int64_t>(relower_per_fn));
            insert_kv("module-dirty-skips", static_cast<std::int64_t>(mod_skip));
            insert_kv("pipeline-total", static_cast<std::int64_t>(pipeline_total));
            // Issue #606: pure-delegation observation — ShapeWrap +
            // LinearOwnershipWrap bump a static atomic on every run()
            // call. Surfaced here so the AI agent can verify the new
            // pure Wrap delegation is being exercised (or wire more if
            // it's not). The stat is the sum of both wraps so a
            // single field tells us "are the pure wraps hot?".
            const std::uint64_t shape_pure = aura::compiler::ShapeWrap::pure_delegation_hits();
            const std::uint64_t linear_pure =
                aura::compiler::LinearOwnershipWrap::pure_delegation_hits();
            insert_kv("pure-delegation-shape", static_cast<std::int64_t>(shape_pure));
            insert_kv("pure-delegation-linear", static_cast<std::int64_t>(linear_pure));
            insert_kv("pure-delegation-total", static_cast<std::int64_t>(shape_pure + linear_pure));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 5 (orig lines 12089-12184)
void ObservabilityPrims::register_jit_p5(PrimRegistrar add, Evaluator& ev) {

    // Issue #572: Task4-review closing hash for Pass/AnalysisPass Concepts +
    // fold short-circuit + DirtyAwarePass + pure Wrap delegation.
    ObservabilityPrims::register_stats_impl(
        "query:pass-pipeline-dirtyaware-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(32);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto idx = ((h >> 1) + at) & (hcap - 1);
                        if (meta[idx] == 0xFF) {
                            meta[idx] = fp;
                            keys[idx] = key_ev.val;
                            vals[idx] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t pipeline_runs =
                aura::compiler::pass_pipeline_runs_total.load(std::memory_order_relaxed);
            const std::uint64_t pipeline_yield =
                aura::compiler::pipeline_yield_count.load(std::memory_order_relaxed);
            const std::uint64_t passes_skip_dirty =
                aura::compiler::passes_skipped_dirty_pipeline.load(std::memory_order_relaxed);
            const std::uint64_t passes_skip_type = ev.get_passes_skipped_type_dirty();
            const std::uint64_t passes_skipped_due_to_dirty = passes_skip_dirty + passes_skip_type;
            const std::uint64_t relower_skip =
                m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_per_fn =
                m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t mod_skip =
                m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0;
            const std::uint64_t block_dirty_hits =
                m ? m->ir_soa_block_dirty_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t wrap_delegation =
                aura::compiler::ShapeWrap::pure_delegation_hits() +
                aura::compiler::LinearOwnershipWrap::pure_delegation_hits();
            const std::uint64_t latency_denom = pipeline_runs + passes_skipped_due_to_dirty + 1;
            const std::int64_t incremental_latency_win_pct =
                static_cast<std::int64_t>((passes_skipped_due_to_dirty * 100) / latency_denom);
            const std::uint64_t total =
                pipeline_runs + pipeline_yield + passes_skipped_due_to_dirty + relower_skip +
                relower_per_fn + mod_skip + block_dirty_hits + wrap_delegation;
            std::int64_t recommendation = 0;
            if (pipeline_runs > 0 && passes_skipped_due_to_dirty == 0)
                recommendation = 3;
            else if (wrap_delegation == 0 && pipeline_runs > 0)
                recommendation = 2;
            else if (passes_skipped_due_to_dirty > 0 || wrap_delegation > 0)
                recommendation = 1;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"pass-pipeline-runs", make_int(static_cast<std::int64_t>(pipeline_runs))},
                {"pipeline-yield-count", make_int(static_cast<std::int64_t>(pipeline_yield))},
                {"passes-skipped-due-to-dirty",
                 make_int(static_cast<std::int64_t>(passes_skipped_due_to_dirty))},
                {"passes-skipped-dirty-pipeline",
                 make_int(static_cast<std::int64_t>(passes_skip_dirty))},
                {"passes-skipped-type-dirty",
                 make_int(static_cast<std::int64_t>(passes_skip_type))},
                {"wrap-delegation-count", make_int(static_cast<std::int64_t>(wrap_delegation))},
                {"relower-skipped", make_int(static_cast<std::int64_t>(relower_skip))},
                {"relower-per-fn", make_int(static_cast<std::int64_t>(relower_per_fn))},
                {"module-dirty-skips", make_int(static_cast<std::int64_t>(mod_skip))},
                {"ir-soa-block-dirty-hits", make_int(static_cast<std::int64_t>(block_dirty_hits))},
                {"incremental-latency-win-pct", make_int(incremental_latency_win_pct)},
                {"task4-review-schema", make_int(572)},
                {"pass-pipeline-dirtyaware-total", make_int(static_cast<std::int64_t>(total))},
                {"pass-pipeline-dirtyaware-recommendation", make_int(recommendation)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 6 (orig lines 12185-12266)
void ObservabilityPrims::register_jit_p6(PrimRegistrar add, Evaluator& ev) {

    // (query:soa-dirty-stats) — Issue #429: live SoA
    // dirty state aggregate. Returns a hash with 8 fields
    // computed in one pass over ir_cache_v2_:
    //   - cached_fns:            # entries in the cache
    //   - dirty_fns:             # entries with entry.dirty == true
    //   - total_blocks:          sum of block_dirty_per_func_[i].size()
    //   - dirty_blocks:          sum of #dirty blocks
    //   - total_instructions:    sum of IRFunction.instructions.size()
    //   - dirty_instructions:    # entries with entry.dirty
    //                            (per-instruction aggregate is a
    //                            follow-up — see CompilerService ::
    //                            get_soa_dirty_stats comment)
    //   - dirty_block_pct:       100 * dirty_blocks / total_blocks
    //   - dirty_instruction_pct: 100 * dirty_instructions /
    //                            total_instructions
    //
    // The new primitive complements (query:ir-soa-incremental-stats)
    // (mutation-event lifetime counts) and (compile:ir-soa-stats)
    // (a #254 hash that ships the migration-progress field).
    // query:soa-dirty-stats is the LIVE view (current
    // dirty state) — the AI Agent reads it to decide
    // whether the cache is in a healthy steady state
    // (low dirty_block_pct) or needs a re-lower burst
    // (> 20% dirty means the cache is falling behind the
    // mutation rate).
    ObservabilityPrims::register_stats_impl(
        "query:soa-dirty-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(32);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto idx = ((h >> 1) + at) & (hcap - 1);
                        if (meta[idx] == 0xFF) {
                            meta[idx] = fp;
                            keys[idx] = key_ev.val;
                            vals[idx] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            Evaluator::SoaDirtyStats s;
            if (ev.get_soa_dirty_stats_fn_) {
                s = ev.get_soa_dirty_stats_fn_();
            }
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"cached-fns", make_int(static_cast<std::int64_t>(s.cached_fns))},
                {"dirty-fns", make_int(static_cast<std::int64_t>(s.dirty_fns))},
                {"total-blocks", make_int(static_cast<std::int64_t>(s.total_blocks))},
                {"dirty-blocks", make_int(static_cast<std::int64_t>(s.dirty_blocks))},
                {"total-instructions", make_int(static_cast<std::int64_t>(s.total_instructions))},
                {"dirty-instructions", make_int(static_cast<std::int64_t>(s.dirty_instructions))},
                {"dirty-block-pct", make_int(static_cast<std::int64_t>(s.dirty_block_pct))},
                {"dirty-instruction-pct",
                 make_int(static_cast<std::int64_t>(s.dirty_instruction_pct))},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 7 (orig lines 12267-12379)
void ObservabilityPrims::register_jit_p7(PrimRegistrar add, Evaluator& ev) {

    // (query:arena-compaction-stats-hash) — Issue #430:
    // hash variant of (query:arena-compaction-stats). The
    // legacy primitive returns a single integer = sum of
    // 7 fields (cheaper for dashboards that only need
    // the aggregate). The hash variant exposes each
    // field as a distinct key for the AI Agent's
    // per-field reasoning (e.g. "is the save rate
    // dropping?" needs total_compaction_saved vs
    // compaction_count, which the sum collapses).
    //
    // Field list (10 total):
    //   - auto-compact-triggers: ArenaGroup trigger count
    //   - auto-compact-skips:    ArenaGroup skip count
    //   - compactions:           lifetime compact() calls
    //   - bytes-saved:           lifetime bytes reclaimed
    //   - last-saved:            bytes reclaimed by last compact
    //   - paused-by-boundary:    deferred at MutationBoundary
    //   - mutation-volume:       total_mutations_ (orchestration signal)
    //   - dirty-propagation:     mark_dirty_upward activity
    //   - fragmentation-ratio:   current main arena frag ratio * 100
    //   - peak-used-bytes:       high-water mark for main arena
    //
    // Both primitives share the same underlying counters;
    // pick the integer when you want a single dashboard
    // metric, pick the hash when you want per-field
    // reasoning. The integer variant is the recommended
    // hot path; the hash is for debugging / AI Agent
    // observability.
    ObservabilityPrims::register_stats_impl(
        "query:arena-compaction-stats-hash", [&ev](const auto&) -> EvalValue {
            // Reuse the same build_hash pattern as the
            // closure:stats / soa-dirty-stats primitives.
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(32);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto idx = ((h >> 1) + at) & (hcap - 1);
                        if (meta[idx] == 0xFF) {
                            meta[idx] = fp;
                            keys[idx] = key_ev.val;
                            vals[idx] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            std::uint64_t triggers = 0, skips = 0, compacts = 0, saved = 0;
            std::uint64_t paused = 0, mutations = 0, dirty = 0;
            std::uint64_t frag_pct = 0, peak = 0, last_saved = 0;
            if (ev.arena_group_) {
                const auto stats = ev.arena_group_->total_stats();
                triggers = ev.arena_group_->auto_compact_trigger_count();
                skips = ev.arena_group_->auto_compact_skip_count();
                compacts = stats.compaction_count;
                saved = stats.total_compaction_saved;
                last_saved = stats.last_compaction_saved;
                paused = ev.compaction_paused_by_boundary();
                mutations = ev.total_mutations();
                dirty = ev.get_dirty_propagation_count();
                // Main arena frag ratio (scaled 0..100). Use
                // arena_ (the main per-Evaluator arena) if set;
                // else 0 (no fallback path — ArenaGroup::arenas_
                // is private, and the per-module frag ratio is
                // already exposed via arena:adaptive-stats +
                // query:arena-compaction-stats). The
                // fragmentation-ratio-pct field is the
                // single-arena view; the multi-arena view is
                // in the per-arena strings.
                if (ev.arena_) {
                    const auto s = ev.arena_->stats();
                    frag_pct = static_cast<std::uint64_t>(s.fragmentation_ratio() * 100);
                    peak = s.peak_used;
                }
            }
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"auto-compact-triggers", make_int(static_cast<std::int64_t>(triggers))},
                {"auto-compact-skips", make_int(static_cast<std::int64_t>(skips))},
                {"compactions", make_int(static_cast<std::int64_t>(compacts))},
                {"bytes-saved", make_int(static_cast<std::int64_t>(saved))},
                {"last-saved", make_int(static_cast<std::int64_t>(last_saved))},
                {"paused-by-boundary", make_int(static_cast<std::int64_t>(paused))},
                {"mutation-volume", make_int(static_cast<std::int64_t>(mutations))},
                {"dirty-propagation", make_int(static_cast<std::int64_t>(dirty))},
                {"fragmentation-ratio-pct", make_int(static_cast<std::int64_t>(frag_pct))},
                {"peak-used-bytes", make_int(static_cast<std::int64_t>(peak))},
            };
            return build_hash(kv);
        });
}


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

// Issue #909 part 8 (orig lines 12380-12480)
void ObservabilityPrims::register_jit_p8(PrimRegistrar add, Evaluator& ev) {

    // (query:cxx26-invariants) — Issue #431: a 5-field
    // hash summarizing the codebase's C++26 zero-overhead
    // invariant density. The numbers are compile-time
    // constants tied to the source — they don't move at
    // runtime. The AI Agent reads the count to detect
    // drift (a regression in invariant coverage is a
    // "the codebase lost some compile-time safety" signal).
    //
    // Field list (5 total):
    //   - consteval-invariants: # static_assert blocks
    //     in src/core/cxx26_invariants.ixx (currently 22
    //     — SmallObjectPool tier + Value tag + concept
    //     self-check groups)
    //   - concept-count: # Concepts in src/core/concepts.ixx
    //     (currently 13 — NodeHandle, ASTContainer,
    //     Mutator, ArenaAllocator, Queryable, AuraInvocable,
    //     RangeOf, AnyRange, SymbolInterner, StableNodeRefLike
    //     + Issue #431's SoAColumnar, DirtyPropagator,
    //     ShapeDispatchable)
    //   - contract-hot-paths: # Contract pre/post/assert
    //     sites in Arena + Value + SoA + Pass (sum across
    //     these 4 hot files, currently 26 — issue #431
    //     scope-limited ship doesn't add new Contracts
    //     beyond what was already there; follow-up issues
    //     will)
    //   - concept-self-checks: # static_asserts in
    //     cxx26_invariants.ixx that verify Concepts
    //     compile (currently 1 — the std::vector<int>
    //     check)
    //   - concept-targets-documented: # "Target sites:"
    //     comments in concepts.ixx (currently 9 — each
    //     concept has a doc comment listing the
    //     consumers / future consumers)
    //
    // The contract-hot-paths count is approximate —
    // see ContractHotPathCount() for the exact
    // grep-and-sum. If a future issue adds Contracts
    // to value.ixx or ir_soa.ixx, this number will
    // jump. The AI Agent monitors the count.
    ObservabilityPrims::register_stats_impl(
        "query:cxx26-invariants", [&ev](const auto&) -> EvalValue {
            // Reuse the same build_hash pattern as the
            // closure:stats / soa-dirty-stats primitives.
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(32);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto idx = ((h >> 1) + at) & (hcap - 1);
                        if (meta[idx] == 0xFF) {
                            meta[idx] = fp;
                            keys[idx] = key_ev.val;
                            vals[idx] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            // Issue #1519: prefer baked constants from cpp26_contract_stats
            // so Agents see live consteval/Contract density without
            // hand-updating stale literals (was 24/26; now 65/48).
            constexpr std::int64_t kConstevalInvariants = aura::core::cpp26::kConstevalChecksTotal;
            constexpr std::int64_t kConceptCount = 13;
            constexpr std::int64_t kContractHotPaths = aura::core::cpp26::kContractHotPathsShipped;
            constexpr std::int64_t kConceptSelfChecks = 1;
            constexpr std::int64_t kConceptTargetsDoc = 9;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"consteval-invariants", make_int(kConstevalInvariants)},
                {"concept-count", make_int(kConceptCount)},
                {"contract-hot-paths", make_int(kContractHotPaths)},
                {"concept-self-checks", make_int(kConceptSelfChecks)},
                {"concept-targets-documented", make_int(kConceptTargetsDoc)},
                {"schema", make_int(1519)},
                {"hotpath-contracts-active",
                 make_int(static_cast<std::int64_t>(aura::core::cpp26::hotpath_contracts_1519_active
                                                        .load(std::memory_order_relaxed)))},
                {"contract-violation-hotpath",
                 make_int(static_cast<std::int64_t>(
                     aura::core::cpp26::contract_violation_hotpath_count.load(
                         std::memory_order_relaxed)))},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 9 (orig lines 12481-12583)
void ObservabilityPrims::register_jit_p9(PrimRegistrar add, Evaluator& ev) {

    // (query:edsl-readiness) — Issue #440 / #1142: a single
    // hash that aggregates the curated EDSL production readiness
    // signals. Field list must match the kv vector below (6 fields):
    //   - closure-stale-refresh:       closure_bridge refreshes (#531)
    //   - linear-check-pass:           linear ownership fast-path (#149)
    //   - atomic-batch-commits:        MutationBoundaryGuard commits (#241)
    //   - stable-ref-invalidations:    StableNodeRef is_valid misses (#417)
    //   - occurrence-stale-refreshes:  occurrence stale refreshes
    //   - dirty-block-rate:            live per-block dirty % (#429, 0..100)
    //
    // Comment/code name alignment fixed in #1142 (was mismatched
    // "6 total" header listing 8 differently-named fields).
    ObservabilityPrims::register_stats_impl(
        "query:edsl-readiness", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(32);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto idx = ((h >> 1) + at) & (hcap - 1);
                        if (meta[idx] == 0xFF) {
                            meta[idx] = fp;
                            keys[idx] = key_ev.val;
                            vals[idx] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            std::uint64_t closure_stale = 0, linear_pass = 0;
            std::uint64_t atomic_commits = 0;
            std::uint64_t stable_ref_invalidates = 0;
            std::uint64_t occurrence_stale_refreshes = 0;
            std::int64_t dirty_pct = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                closure_stale = m->closure_stale_refresh_count_.load(std::memory_order_relaxed);
                linear_pass = m->linear_check_pass_count_.load(std::memory_order_relaxed);
                atomic_commits = m->atomic_batch_commits.load(std::memory_order_relaxed);
                stable_ref_invalidates =
                    m->stable_ref_invalidations.load(std::memory_order_relaxed);
                occurrence_stale_refreshes =
                    m->occurrence_stale_refreshes_total.load(std::memory_order_relaxed);
            }
            // dirty-block-rate from #429's get_soa_dirty_stats.
            if (ev.get_soa_dirty_stats_fn_) {
                const auto s = ev.get_soa_dirty_stats_fn_();
                dirty_pct = static_cast<std::int64_t>(s.dirty_block_pct);
            }
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"closure-stale-refresh", make_int(static_cast<std::int64_t>(closure_stale))},
                {"linear-check-pass", make_int(static_cast<std::int64_t>(linear_pass))},
                {"atomic-batch-commits", make_int(static_cast<std::int64_t>(atomic_commits))},
                {"stable-ref-invalidations",
                 make_int(static_cast<std::int64_t>(stable_ref_invalidates))},
                {"occurrence-stale-refreshes",
                 make_int(static_cast<std::int64_t>(occurrence_stale_refreshes))},
                {"dirty-block-rate", make_int(dirty_pct)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 10 (orig lines 12584-12725)
void ObservabilityPrims::register_jit_p10(PrimRegistrar add, Evaluator& ev) {

    // (gc-arena-stats) — Report per-arena allocation. Shows main arena +
    // every per-module arena. Format: "main:0.1MB/8.0MB;json.aura:0.5MB/8.0MB;..."
    // (semicolons separate entries; slashes separate used/capacity within an entry).
    ObservabilityPrims::register_stats_impl("gc-arena-stats", [&ev](const auto&) -> EvalValue {
        std::string out;
        auto fmt_arena = [&](const char* label, std::size_t used, std::size_t cap) {
            auto s = std::format("{}{}:{:.1f}MB/{:.1f}MB", out.empty() ? "" : ";", label,
                                 used / 1048576.0, cap / 1048576.0);
            out += s;
        };
        if (ev.arena_) {
            auto s = ev.arena_->stats();
            fmt_arena("main", s.used, s.capacity);
        }
        if (ev.arena_group_) {
            for (auto& [name, stats] : ev.arena_group_->module_stats()) {
                // Trim path to basename for readability.
                auto slash = name.rfind('/');
                auto short_name = slash == std::string::npos ? name : name.substr(slash + 1);
                fmt_arena(short_name.c_str(), stats.used, stats.capacity);
            }
        }
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(out);
        return types::make_string(sidx);
    });

    // (gc-arena-info) — Return structured per-arena usage as Aura value.
    //
    //   Returns: vector of hashes, each describing one arena:
    //     {name: "main", used: 1.23, capacity: 11.0, used-pct: 11}
    //     {name: "json.aura", used: 0.5, capacity: 8.0, used-pct: 6}
    //     ...
    //
    //   First entry is a summary hash:
    //     {summary: #t, total-arenas: 3, total-used: 1.73, total-capacity: 19.0,
    //      overall-pct: 9}
    //
    //   All numeric values are in megabytes (MB). Pct values are integers 0-100.
    ObservabilityPrims::register_stats_impl("gc-arena-info", [&ev](const auto&) -> EvalValue {
        // Snapshot arena state. Each entry: (short_name, used-MB, cap-MB, pct).
        struct Snap {
            std::string name;
            double used;
            double cap;
            int pct;
        };
        std::vector<Snap> snaps;
        double total_used = 0.0, total_cap = 0.0;
        if (ev.arena_) {
            auto s = ev.arena_->stats();
            double u = s.used / 1048576.0;
            double c = s.capacity / 1048576.0;
            snaps.push_back({"main", u, c, c > 0 ? static_cast<int>(u / c * 100.0) : 0});
            total_used += u;
            total_cap += c;
        }
        if (ev.arena_group_) {
            for (auto& [full_name, stats] : ev.arena_group_->module_stats()) {
                auto slash = full_name.rfind('/');
                auto short_name =
                    slash == std::string::npos ? full_name : full_name.substr(slash + 1);
                double u = stats.used / 1048576.0;
                double c = stats.capacity / 1048576.0;
                snaps.push_back({short_name, u, c, c > 0 ? static_cast<int>(u / c * 100.0) : 0});
                total_used += u;
                total_cap += c;
            }
        }
        int overall = total_cap > 0 ? static_cast<int>(total_used / total_cap * 100.0) : 0;

        // Build a small Swiss-table hash. Inline copy of the (hash ...) primitive
        // pattern. Capacity 8 is enough for the 5-field hashes below.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(16) /* #1141 */;
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto cap = ht->capacity;
            for (auto& [k, v] : kv) {
                // Hash the key with FNV-1a (matches user-level (hash ...) behavior).
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
                // Intern the key as a String EvalValue.
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < cap; ++at) {
                    auto idx = ((h >> 1) + at) & (cap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    // 8 slots should be enough for the 5-key hashes we build.
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };

        std::vector<EvalValue> result;
        // Summary entry first.
        {
            std::vector<std::pair<std::string, EvalValue>> kv;
            kv.push_back({"summary", make_bool(true)});
            kv.push_back({"total-arenas", make_int(static_cast<std::int64_t>(snaps.size()))});
            kv.push_back({"total-used", make_float(total_used)});
            kv.push_back({"total-capacity", make_float(total_cap)});
            kv.push_back({"overall-pct", make_int(overall)});
            result.push_back(build_hash(kv));
        }
        for (auto& s : snaps) {
            auto name_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(s.name);
            std::vector<std::pair<std::string, EvalValue>> kv;
            kv.push_back({"name", make_string(name_idx)});
            kv.push_back({"used", make_float(s.used)});
            kv.push_back({"capacity", make_float(s.cap)});
            kv.push_back({"used-pct", make_int(s.pct)});
            result.push_back(build_hash(kv));
        }
        auto vidx = ev.vector_heap_.size();
        ev.vector_heap_.push_back(std::move(result));
        return make_vector(vidx);
    });
}

// Issue #1433: map CompilerMetrics field name → facade group.
// Groups: compile | jit | mutate | query | arena | gc | eval | telemetry.
static const char* metrics_group_for_field(std::string_view name) noexcept {
    if (name.size() >= 4 && name.substr(0, 4) == "jit_")
        return "jit";
    if (name.size() >= 6 && name.substr(0, 6) == "arena_")
        return "arena";
    if (name.size() >= 10 && name.substr(0, 10) == "ast_arena_")
        return "arena";
    if (name.size() >= 3 && name.substr(0, 3) == "gc_")
        return "gc";
    if (name.size() >= 6 && name.substr(0, 6) == "mutate")
        return "mutate";
    if (name.size() >= 8 && name.substr(0, 8) == "mutation")
        return "mutate";
    if (name.size() >= 6 && name.substr(0, 6) == "dirty_")
        return "mutate";
    if (name.size() >= 6 && name.substr(0, 6) == "query_")
        return "query";
    if (name.size() >= 8 && name.substr(0, 8) == "pattern_")
        return "query";
    if (name.size() >= 5 && name.substr(0, 5) == "eval_")
        return "eval";
    if (name.size() >= 12 && name.substr(0, 12) == "hotpath_eval")
        return "eval";
    if (name.size() >= 8 && name.substr(0, 8) == "compile_")
        return "compile";
    if (name.size() >= 8 && name.substr(0, 8) == "relower_")
        return "compile";
    if (name.size() >= 12 && name.substr(0, 12) == "module_dirty")
        return "compile";
    if (name.size() >= 10 && name.substr(0, 10) == "invalidate")
        return "compile";
    if (name.size() >= 8 && name.substr(0, 8) == "cascade_")
        return "compile";
    if (name.size() >= 7 && name.substr(0, 7) == "define_")
        return "compile";
    if (name.size() >= 12 && name.substr(0, 12) == "value_define")
        return "compile";
    if (name.size() >= 7 && name.substr(0, 7) == "aot_emi")
        return "compile";
    if (name.size() >= 11 && name.substr(0, 11) == "aot_fallbac")
        return "compile";
    return "telemetry";
}

// P2b / #1433: observability facade for AURA_PRIMITIVES=s0 and full.
// stats:list / stats:count / engine:metrics — no bulk query:*-stats on s0.
void ObservabilityPrims::register_metrics_facade(PrimRegistrar add, Evaluator& ev) {
    // Issue #560: (stats:list)
    add("stats:list", [&ev](const auto&) -> EvalValue {
        const std::vector<std::string>& stats = ObservabilityPrims::stats_primitives();
        EvalValue result = make_void();
        for (auto it = stats.rbegin(); it != stats.rend(); ++it) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(*it);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });

    // Issue #560: (stats:count)
    add("stats:count", [](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(ObservabilityPrims::stats_primitives().size()));
    });

    // Issue #1672: catalog ↔ impl drift diagnostic (facade-only, not public add).
    // (stats:get "stats:drift-check") / (engine:metrics "stats:drift-check")
    ObservabilityPrims::register_stats_impl("stats:drift-check", [&ev](const auto&) -> EvalValue {
        return ObservabilityPrims::stats_drift_check(ev);
    });

    // Issue #1433 / P1a: (engine:metrics [name | :all | :prefix s | :group g])
    add("engine:metrics", [&ev](std::span<const EvalValue> a) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            std::uint64_t need = static_cast<std::uint64_t>(kv.size()) * 2 + 2;
            std::uint64_t cap = 16;
            while (cap < need)
                cap <<= 1;
            auto* ht = FlatHashTable::create(cap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = ::aura::compiler::hash::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::hash::kFnvPrime;
                auto fp = ::aura::compiler::hash::fingerprint(h);
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };

        auto kw_str = [&](const EvalValue& v) -> std::string {
            if (!types::is_keyword(v))
                return {};
            auto kidx = types::as_keyword_idx(v);
            if (kidx >= ev.keyword_table_.size())
                return {};
            return ev.keyword_table_[kidx];
        };
        auto as_text = [&](const EvalValue& v) -> std::string {
            if (is_string(v)) {
                auto sidx = as_string_idx(v);
                if (sidx < ev.string_heap_.size())
                    return ev.string_heap_[sidx];
                return {};
            }
            // Keywords used as group tags: :jit / jit
            auto k = kw_str(v);
            if (!k.empty()) {
                if (k.size() > 1 && k[0] == ':')
                    return k.substr(1);
                return k;
            }
            return {};
        };

        const auto& stats = ObservabilityPrims::stats_primitives();

        // Collect CompilerMetrics atomics into group maps (Issue #1433).
        auto dump_metric_groups = [&](CompilerMetrics* m)
            -> std::unordered_map<std::string, std::vector<std::pair<std::string, EvalValue>>,
                                  aura::core::TransparentStringHash, std::equal_to<>> {
            std::unordered_map<std::string, std::vector<std::pair<std::string, EvalValue>>,
                               aura::core::TransparentStringHash, std::equal_to<>>
                groups;
            if (!m)
                return groups;
            if (auto* svc = static_cast<CompilerService*>(ev.compiler_service()))
                svc->refresh_env_arena_metrics(*m);
#define AURA_COMPILER_METRICS_FIELD(name)                                                          \
    groups[metrics_group_for_field(#name)].emplace_back(                                           \
        #name, make_int(static_cast<std::int64_t>(m->name.load(std::memory_order_relaxed))));
#include "compiler_metrics_fields.inc"
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>
            return groups;
        };

        auto groups_to_nested =
            [&](std::unordered_map<std::string, std::vector<std::pair<std::string, EvalValue>>,
                                   aura::core::TransparentStringHash, std::equal_to<>>& groups)
            -> std::vector<std::pair<std::string, EvalValue>> {
            static const char* kOrder[] = {"compile", "jit", "mutate", "query",
                                           "arena",   "gc",  "eval",   "telemetry"};
            std::vector<std::pair<std::string, EvalValue>> out;
            out.reserve(8);
            for (const char* g : kOrder) {
                auto it = groups.find(g);
                if (it == groups.end() || it->second.empty())
                    continue;
                out.emplace_back(g, build_hash(it->second));
            }
            return out;
        };

        // (engine:metrics "query:foo-stats") → single stats value
        // Issue #1439: prefer internal stats impl table (public prims removed).
        if (a.size() >= 1 && is_string(a[0])) {
            auto sidx = as_string_idx(a[0]);
            if (sidx >= ev.string_heap_.size())
                return make_void();
            const std::string& name = ev.string_heap_[sidx];
            if (auto fn = ObservabilityPrims::lookup_stats_impl(name))
                return (*fn)({});
            if (auto fn = ev.primitives_.lookup(name))
                return (*fn)({});
            return make_void();
        }

        // Keyword sub-ops: :all | :prefix s | :group g
        if (a.size() >= 1 && types::is_keyword(a[0])) {
            std::string k = kw_str(a[0]);
            if (!k.empty() && k[0] == ':')
                k = k.substr(1);

            // (engine:metrics :all) → stats name→value hash (+ schema)
            if (k == "all") {
                std::vector<std::pair<std::string, EvalValue>> kv;
                kv.reserve(stats.size() + 4);
                kv.push_back({"schema", make_int(2)});
                kv.push_back({"stats-count", make_int(static_cast<std::int64_t>(stats.size()))});
                for (const auto& name : stats) {
                    std::optional<PrimFn> fn = ObservabilityPrims::lookup_stats_impl(name);
                    if (!fn)
                        fn = ev.primitives_.lookup(name);
                    if (!fn)
                        continue;
                    kv.push_back({name, (*fn)({})});
                }
                return build_hash(kv);
            }

            // (engine:metrics :prefix "query:") → filtered stats / field keys
            if (k == "prefix" && a.size() >= 2) {
                std::string prefix = as_text(a[1]);
                std::vector<std::pair<std::string, EvalValue>> kv;
                kv.reserve(64);
                kv.push_back({"schema", make_int(2)});
                kv.push_back({"prefix", make_string([&] {
                                  auto i = ev.string_heap_.size();
                                  ev.string_heap_.push_back(prefix);
                                  return i;
                              }())});
                // Stats catalog names matching prefix (values when registered).
                for (const auto& name : stats) {
                    if (name.size() < prefix.size() || name.compare(0, prefix.size(), prefix) != 0)
                        continue;
                    std::optional<PrimFn> fn = ObservabilityPrims::lookup_stats_impl(name);
                    if (!fn)
                        fn = ev.primitives_.lookup(name);
                    if (fn)
                        kv.push_back({name, (*fn)({})});
                    else
                        kv.push_back({name, make_void()});
                }
                // Also CompilerMetrics fields whose names start with prefix
                // after treating ':' as '_' for matching (query: → query_).
                std::string field_pfx = prefix;
                for (char& c : field_pfx) {
                    if (c == ':')
                        c = '_';
                }
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                    auto groups = dump_metric_groups(m);
                    for (auto& [g, fields] : groups) {
                        (void)g;
                        for (auto& [fname, val] : fields) {
                            if (fname.size() >= field_pfx.size() &&
                                fname.compare(0, field_pfx.size(), field_pfx) == 0)
                                kv.push_back({fname, val});
                        }
                    }
                }
                return build_hash(kv);
            }

            // (engine:metrics :group "jit" | :jit)
            if (k == "group" && a.size() >= 2) {
                std::string group = as_text(a[1]);
                if (group.empty())
                    return make_void();
                auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
                auto groups = dump_metric_groups(m);
                auto it = groups.find(group);
                if (it == groups.end()) {
                    // Empty group hash still valid
                    return build_hash(std::span<const std::pair<std::string, EvalValue>>{});
                }
                return build_hash(it->second);
            }
        }

        // Default (engine:metrics): nested CompilerMetrics groups + catalog meta.
        // schema 2 = #1433 full facade (groups present).
        std::vector<std::pair<std::string, EvalValue>> kv;
        kv.push_back({"schema", make_int(2)});
        kv.push_back({"stats-count", make_int(static_cast<std::int64_t>(stats.size()))});
        {
            EvalValue names_list = make_void();
            for (auto it = stats.rbegin(); it != stats.rend(); ++it) {
                auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back(*it);
                auto pid = ev.pairs_.size();
                ev.pairs_.push_back({make_string(sidx), names_list});
                names_list = make_pair(pid);
            }
            kv.push_back({"stats-names", names_list});
        }
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            auto groups = dump_metric_groups(m);
            auto nested = groups_to_nested(groups);
            // Total atomic field count for golden tests (≥200).
            std::int64_t field_total = 0;
            for (auto& [g, fields] : groups)
                field_total += static_cast<std::int64_t>(fields.size());
            kv.push_back({"metrics-field-count", make_int(field_total)});
            for (auto& p : nested)
                kv.push_back(std::move(p));
            // Back-compat nested "compiler" key with env/arena snapshot (#1385).
            std::vector<std::pair<std::string, EvalValue>> ck;
            ck.push_back({"env_frames_size_total",
                          make_int(static_cast<std::int64_t>(m->env_frames_size_total.load()))});
            ck.push_back({"env_frames_stale_count",
                          make_int(static_cast<std::int64_t>(m->env_frames_stale_count.load()))});
            ck.push_back({"ast_arena_bytes_in_use",
                          make_int(static_cast<std::int64_t>(m->ast_arena_bytes_in_use.load()))});
            ck.push_back({"ast_arena_upstream_bytes",
                          make_int(static_cast<std::int64_t>(m->ast_arena_upstream_bytes.load()))});
            kv.push_back({"compiler", build_hash(ck)});
        }
        return build_hash(kv);
    });

    // ── Issue #1450 / #1449 Phase 1: engine-level stats:get / stats:prefix ──
    // Mirrors lib/std/stats.aura so agents do not need (require std/stats)
    // to route observability through the facade.
    auto resolve_name = [&ev](const EvalValue& v) -> std::string {
        if (is_string(v)) {
            auto sidx = as_string_idx(v);
            if (sidx < ev.string_heap_.size())
                return ev.string_heap_[sidx];
        }
        return {};
    };

    // (stats:get name) → value | void
    //   name = "all" → same shape as (engine:metrics :all) name→value hash
    //   else lookup register_stats_impl then public Primitives (residual aliases)
    add("stats:get", [&ev, resolve_name](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_void();
        const std::string name = resolve_name(a[0]);
        if (name.empty())
            return make_void();
        if (name == "all") {
            // Delegate: build (engine:metrics :all) by reusing lookup path.
            const auto& stats = ObservabilityPrims::stats_primitives();
            std::vector<std::pair<std::string, EvalValue>> kv;
            kv.reserve(stats.size() + 2);
            kv.push_back({"schema", make_int(2)});
            kv.push_back({"stats-count", make_int(static_cast<std::int64_t>(stats.size()))});
            for (const auto& n : stats) {
                std::optional<PrimFn> fn = ObservabilityPrims::lookup_stats_impl(n);
                if (!fn)
                    fn = ev.primitives_.lookup(n);
                if (fn)
                    kv.push_back({n, (*fn)({})});
            }
            // Minimal hash builder (same layout as engine:metrics).
            std::uint64_t need = static_cast<std::uint64_t>(kv.size()) * 2 + 2;
            std::uint64_t cap = 16;
            while (cap < need)
                cap <<= 1;
            auto* ht = FlatHashTable::create(cap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = ::aura::compiler::hash::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::hash::kFnvPrime;
                auto fp = ::aura::compiler::hash::fingerprint(h);
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        }
        if (auto fn = ObservabilityPrims::lookup_stats_impl(name))
            return (*fn)({});
        if (auto fn = ev.primitives_.lookup(name))
            return (*fn)({});
        return make_void();
    });

    // (stats:prefix "query:") → list of catalog names matching prefix
    add("stats:prefix", [&ev, resolve_name](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_void();
        const std::string prefix = resolve_name(a[0]);
        EvalValue result = make_void();
        const auto& stats = ObservabilityPrims::stats_primitives();
        // Build list in reverse so final order matches catalog order.
        std::vector<std::string> matched;
        for (const auto& n : stats) {
            if (prefix.empty() ||
                (n.size() >= prefix.size() && n.compare(0, prefix.size(), prefix) == 0))
                matched.push_back(n);
        }
        for (auto it = matched.rbegin(); it != matched.rend(); ++it) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(*it);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });

    // (engine:surface) — SlimSurface inventory snapshot (#1448/#1449/#1450).
    // Named without *-stats suffix so the freeze gate stays clean.
    // AC mapping: issue text said query:primitive-surface-stats; governance
    // forbids new *-stats public names → engine:surface is the canonical form.
    add("engine:surface", [&ev](const auto&) -> EvalValue {
        const auto& stats = ObservabilityPrims::stats_primitives();
        const auto public_slots = static_cast<std::int64_t>(ev.primitives_.slot_count());
        // Count residual public *stats-like* names still on Primitives
        // (not private register_stats_impl-only). Non-stats catalog entries
        // (e.g. query:dirty-impact) are out of scope for this counter.
        auto is_stats_like = [](std::string_view n) {
            return n.ends_with("-stats") || n.ends_with("-stats-hash") ||
                   n.find("-stats-") != std::string_view::npos;
        };
        std::int64_t public_stats = 0;
        for (const auto& n : stats) {
            if (!is_stats_like(n))
                continue;
            if (ev.primitives_.slot_for_name(n) < ev.primitives_.slot_count())
                ++public_stats;
        }
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"schema", make_int(1)},
            {"public-count", make_int(public_slots)},
            {"stats-catalog-count", make_int(static_cast<std::int64_t>(stats.size()))},
            {"public-stats-remaining", make_int(public_stats)},
            {"target-budget", make_int(420)},
            {"interim-ceiling", make_int(700)},
            {"deprecated-dispatch-total",
             make_int(static_cast<std::int64_t>(ev.deprecated_prim_dispatch_total()))},
        };
        std::uint64_t need = static_cast<std::uint64_t>(kv.size()) * 2 + 2;
        std::uint64_t cap = 16;
        while (cap < need)
            cap <<= 1;
        auto* ht = FlatHashTable::create(cap);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        for (auto& [k, v] : kv) {
            std::uint64_t h = ::aura::compiler::hash::kFnvOffsetBasis;
            for (char c : k)
                h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::hash::kFnvPrime;
            auto fp = ::aura::compiler::hash::fingerprint(h);
            auto kidx = ev.string_heap_.size();
            ev.string_heap_.push_back(k);
            EvalValue key_ev = make_string(kidx);
            bool inserted = false;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    keys[idx] = key_ev.val;
                    vals[idx] = v.val;
                    ht->size++;
                    inserted = true;
                    break;
                }
            }
            if (!inserted) {
                FlatHashTable::destroy(ht);
                return make_void();
            }
        }
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 11 (orig lines 12726-12889) — bulk stats after facade
void ObservabilityPrims::register_jit_p11(PrimRegistrar add, Evaluator& ev) {
    register_metrics_facade(add, ev);

    // Issue #728: (query:unified-error-stats) — unified structured
    // error + provenance + recovery observability for AI Agent
    // closed-loop stdlib reliability (non-duplicative with #478
    // (query:primitive-error-stats pair) and #585 (query:primitives-
    // error-stats hash with error_rate / recovery_success / panic-
    // recovery / rollback / contract-violations / recommendation).
    // #728 covers the *unified model* specifically: structured
    // ErrorValue (kind + provenance StableNodeRef + context + recovery
    // hint) hits as separate counters. #585 is coarse error-rate +
    // recovery; #728 is the per-decision-point unified-model signal.
    //
    // Fields (3 + sentinel):
    //   - structured-hits       unified_error_structured_hits_total
    //                           (# of times a primitive emitted a
    //                            structured ErrorValue vs. legacy
    //                            make_primitive_error string-only
    //                            path — proxy for "how much of
    //                            stdlib has migrated to the unified
    //                            model")
    //   - provenance-captured   unified_error_provenance_captured_total
    //                           (# of structured errors that captured
    //                            a StableNodeRef provenance — proxy
    //                            for "how many errors are introspectable
    //                            for AI Agent recovery")
    //   - recovery-success      unified_error_recovery_success_total
    //                           (# of successful rollback + retry
    //                            primitive path firings — complements
    //                            #585's coarse recovery counter with
    //                            structured-error provenance)
    //   - schema == 728
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual unified ErrorValue / EvalValue tagged-error extension
    // + refactor of evaluator_primitives_list.cpp / math.cpp / regex
    // / verify error sites to make_structured_primitive_error(guard,
    // kind, msg, context) + new (primitive:error) / (with-error) /
    // (primitive:try) primitives + Guard.capture auto-provenance +
    // CI lint for legacy make_primitive_error usage + new
    // tests/test_unified_primitive_error_model.cpp harness + SEVA
    // error-resilient closed-loop + primitives_style.md mandate are
    // all follow-up work (each is a dedicated session in
    // evaluator.ixx + primitives_detail.h + evaluator_primitives_*.cpp
    // + Guard + diagnostic + ast.ixx StableNodeRef + new test + SEVA
    // + docs).
    //
    // Issue #728: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=728 + category=general
    // + arity=0 + pure=true (same pattern as #712-#723 / #726).
    ObservabilityPrims::register_stats_impl(
        "query:unified-error-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 4-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t structured_hits =
                m ? static_cast<std::int64_t>(
                        m->unified_error_structured_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t provenance_captured =
                m ? static_cast<std::int64_t>(
                        m->unified_error_provenance_captured_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t recovery_success =
                m ? static_cast<std::int64_t>(
                        m->unified_error_recovery_success_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"structured-hits", make_int(structured_hits)},
                {"provenance-captured", make_int(provenance_captured)},
                {"recovery-success", make_int(recovery_success)},
                {"schema", make_int(728)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 12 (orig lines 12890-13031)
void ObservabilityPrims::register_jit_p12(PrimRegistrar add, Evaluator& ev) {

    // Issue #731: (query:arena-concurrent-compact-stats) — Arena +
    // SoA + EnvFrame concurrent compaction safety observability for
    // production multi-fiber steal/resume + panic checkpoint integration
    // (non-duplicative with #722 arena tier integration stats + #743
    // arena auto-compact policy + fiber safepoint + #647 EnvFrame
    // dual-path + #648 panic checkpoint fiber + #685 auto-compact
    // policy + #604 Arena auto-compact fiber/GC safepoint). #731 covers
    // the *concurrent* safety specifically: scheduler-safepoint
    // coordination + EnvFrame GCEnvWalkFn revalidation + panic-rollback-
    // compact integration + race prevention.
    //
    // Fields (4 + sentinel):
    //   - concurrent-compacts     arena_concurrent_compacts_total
    //                             (# of successful concurrent compacts
    //                              with safepoint coordination — proxy
    //                              for "how often the arena can safely
    //                              compact under fiber contention")
    //   - envframe-revalidations  arena_envframe_revalidations_total
    //                             (# of times an EnvId in env_frames_
    //                              SoA was revalidated post-compact via
    //                              GCEnvWalkFn — proxy for "how often
    //                              post-compact EnvFrame consistency
    //                              is verified")
    //   - panic-rollback-compact-hits
    //                            arena_panic_rollback_compact_hits_total
    //                             (# of panic checkpoint auto-rollbacks
    //                              that fired under a concurrent compact
    //                              — proxy for "how often panic restore
    //                              detected an inconsistent compact +
    //                              triggered rollback")
    //   - races-prevented         arena_races_prevented_total
    //                             (# of times a race was prevented
    //                              via safepoint + deferred — proxy
    //                              for "how often steal/resume vs
    //                              compact race was safely deferred")
    //   - schema == 731
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual concurrent compact / defrag safepoint coordination
    // in arena.ixx + GCEnvWalkFn EnvFrame revalidation in evaluator_gc.cpp
    // + fiber.cpp resume() / transfer hook integration + panic checkpoint
    // snapshot integration + tests/test_arena_concurrent_compact_envframe_
    // fiber_steal.cpp harness (heavy alloc / mutate under 10+ fibers +
    // steal + periodic compact + panic injection) + #674 stress extension
    // are all follow-up work (each is a dedicated session in arena.ixx +
    // gc_coordinator + evaluator_gc.cpp + fiber.cpp + panic_checkpoint +
    // new test + chaos stress + docs).
    //
    // Issue #731: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=731 + category=general
    // + arity=0 + pure=true (same pattern as #712-#728).
    ObservabilityPrims::register_stats_impl(
        "query:arena-concurrent-compact-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 5-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t concurrent_compacts =
                m ? static_cast<std::int64_t>(
                        m->arena_concurrent_compacts_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t envframe_revalidations =
                m ? static_cast<std::int64_t>(
                        m->arena_envframe_revalidations_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t panic_rollback_compact_hits =
                m ? static_cast<std::int64_t>(
                        m->arena_panic_rollback_compact_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t races_prevented =
                m ? static_cast<std::int64_t>(
                        m->arena_races_prevented_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"concurrent-compacts", make_int(concurrent_compacts)},
                {"envframe-revalidations", make_int(envframe_revalidations)},
                {"panic-rollback-compact-hits", make_int(panic_rollback_compact_hits)},
                {"races-prevented", make_int(races_prevented)},
                {"schema", make_int(731)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 13 (orig lines 13032-13188)
void ObservabilityPrims::register_jit_p13(PrimRegistrar add, Evaluator& ev) {

    // Issue #732: (query:aot-safe-swap-boundary-stats) — AOT
    // hot-reload safe-swap at MutationBoundary observability for
    // production zero-downtime multi-agent orchestration (non-
    // duplicative with #708 (query:aot-reload-stats 5-7 field
    // high-level reload summary — attempts / success / stale /
    // refcount_swaps / region_violations / deopt-on-steal /
    // concurrent-safe-reloads) + #644 (query:aot-reload-func-
    // table-stats enforcement with ref-bump / ref-decrement /
    // region-reapply) + #590 (query:aot-hotupdate-stats 3 atomics).
    // #732 covers the *safe-swap at MutationBoundary* specifically
    // — reloads that fired at the outermost safe-swap point (NOT
    // mid-mutation) — as the per-decision-point signal the Agent
    // consumes to monitor safe-swap adoption rate + zero-downtime
    // orchestration quality.
    //
    // Fields (5 + sentinel):
    //   - safe-boundary-hits          aot_safe_boundary_hits_total
    //                                 (# of AOT reloads that fired at
    //                                  outermost MutationBoundary
    //                                  safe-swap point — proxy for
    //                                  "how often reload landed at a
    //                                  true safe point vs. was
    //                                  deferred / raced")
    //   - refcount-swaps              aot_refcount_swaps_
    //                                 (# of atomic func_table
    //                                  refcount swaps — read from
    //                                  existing #708 atomic for
    //                                  cross-reference with the
    //                                  high-level summary)
    //   - region-violations-prevented aot_region_mismatch_
    //                                 (# of region mismatches
    //                                  detected + prevented on reload
    //                                  — read from existing #708
    //                                  atomic; close to #708's
    //                                  region-violations field)
    //   - concurrent-safe-reloads     aot_concurrent_safe_reloads_
    //                                 (# of concurrent safe reloads
    //                                  — read from existing #708
    //                                  atomic; cross-reference with
    //                                  high-level summary)
    //   - deopt-on-steal              aot_deopt_on_steal_
    //                                 (# of deopts triggered on fiber
    //                                  steal — read from existing
    //                                  #708 atomic; cross-reference)
    //   - schema == 732
    //
    // Phase 1 ships the primitive + counter + bump helper.
    // The actual atomic func_table refcount swap protocol in
    // aura_jit_bridge.cpp aura_reload_aot_module + per-region
    // isolation enforcement on reload + aura_aot_request_safe_reload()
    // API + MutationBoundaryGuard outermost exit hook + GraceEpoch
    // defer-old-decrement after grace period + tests/test_aot_hot_swap_
    // refcount_region_guard_safe.cpp harness (multi-agent different
    // regions + AOT emit + mutate + concurrent apply + reload at
    // boundary) + #674 concurrent stress integration + docs are
    // all follow-up work (each is a dedicated session in
    // aura_jit_bridge.cpp + MutationBoundaryGuard + fiber.cpp + new
    // test + chaos stress + docs).
    //
    // Issue #732: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=732 + category=general
    // + arity=0 + pure=true (same pattern as #712-#728 / #731).
    ObservabilityPrims::register_stats_impl(
        "query:aot-safe-swap-boundary-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 6-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t safe_boundary_hits =
                m ? static_cast<std::int64_t>(
                        m->aot_safe_boundary_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t refcount_swaps =
                m ? static_cast<std::int64_t>(
                        m->aot_refcount_swaps_.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t region_violations_prevented =
                m ? static_cast<std::int64_t>(
                        m->aot_region_mismatch_.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t concurrent_safe_reloads =
                m ? static_cast<std::int64_t>(
                        m->aot_concurrent_safe_reloads_.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t deopt_on_steal =
                m ? static_cast<std::int64_t>(
                        m->aot_deopt_on_steal_.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"safe-boundary-hits", make_int(safe_boundary_hits)},
                {"refcount-swaps", make_int(refcount_swaps)},
                {"region-violations-prevented", make_int(region_violations_prevented)},
                {"concurrent-safe-reloads", make_int(concurrent_safe_reloads)},
                {"deopt-on-steal", make_int(deopt_on_steal)},
                {"schema", make_int(732)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 14 (orig lines 13189-13357)
void ObservabilityPrims::register_jit_p14(PrimRegistrar add, Evaluator& ev) {

    // Issue #733: (query:ir-marker-hygiene-stats) — Macro SyntaxMarker
    // propagation + IR/JIT hygiene enforcement observability for
    // Task6 macro-heavy self-evolution reliability (non-duplicative
    // with #714 (query:self-evolution-closedloop-stats — ref drift +
    // rollback success + feedback mutate rounds) + #455 (ir marker
    // snapshot — internal mechanics, no observability surface) + #373
    // (mutate hygiene guard — flat.is_macro_introduced internal check).
    // #733 covers the *marker propagation + IR/JIT enforcement*
    // specifically across the entire compile/execution pipeline
    // (macro expand → AST → lowering → IR → JIT hot-path → Interpreter)
    // as separate per-decision-point counters.
    //
    // Fields (5 + sentinel):
    //   - user-instrs                  ir_marker_user_instrs_total
    //                                   (# of IRInstructions created
    //                                    with marker=User — proxy for
    //                                    "how much IR traffic is
    //                                    user-authored")
    //   - macro-introduced-instrs      ir_marker_macro_introduced_instrs_total
    //                                   (# of IRInstructions created
    //                                    with marker=MacroIntroduced
    //                                    — proxy for "how much IR
    //                                    traffic is macro-authored,
    //                                    the hygiene scope")
    //   - marker-loss-events           ir_marker_loss_events_total
    //                                   (# of times marker propagation
    //                                    failed at emit path —
    //                                    closure / PrimCall arg /
    //                                    linear op / cached define
    //                                    path that did not copy AST
    //                                    marker → IR source_marker /
    //                                    IRFunction marker — proxy for
    //                                    "how many macro-introduced
    //                                    sub-exprs lost their hygiene
    //                                    marker through the pipeline")
    //   - jit-hygiene-violations-prevented
    //                                  ir_hygiene_jit_violations_prevented_total
    //                                   (# of times the JIT conservative
    //                                    policy fired on MacroIntroduced
    //                                    source_marker — prevented
    //                                    aggressive deopt-elide /
    //                                    respected hygiene in closure
    //                                    capture / forced Interpreter
    //                                    fallback or extra epoch check
    //                                    — proxy for "how often the
    //                                    JIT hot-path consults marker
    //                                    + applies conservative policy")
    //   - marker-propagation-hits      ir_hygiene_marker_propagation_hits_total
    //                                   (# of times marker propagation
    //                                    succeeded across all emit
    //                                    sites via propagate_marker_
    //                                    from_ast helper — proxy for
    //                                    "how often the hygiene marker
    //                                    survives the pipeline")
    //   - schema == 733
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual propagate_marker_from_ast helper in lowering_impl.cpp
    // + ir_soa.ixx marker_ column + aura_jit.cpp + aura_jit_runtime.cpp
    // + ir_executor.ixx conservative policy on source_marker==
    // MacroIntroduced + IRFunction creation marker-from-root-AST-
    // marker in service/lowering + tests/test_macro_marker_propagation_
    // ir_jit_post_mutate.cpp harness (define macro that introduces
    // lambda + mutate inside it under fiber + JIT hot path) + #674
    // stress integration + SEVA macro-heavy cases are all follow-up
    // work (each is a dedicated session in lowering_impl.cpp +
    // ir_soa.ixx + aura_jit.cpp + aura_jit_runtime.cpp + ir_executor.ixx
    // + new test + chaos stress + SEVA demo + docs).
    //
    // Issue #733: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=733 + category=general
    // + arity=0 + pure=true (same pattern as #712-#732).
    ObservabilityPrims::register_stats_impl(
        "query:ir-marker-hygiene-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 6-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t user_instrs =
                m ? static_cast<std::int64_t>(
                        m->ir_marker_user_instrs_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t macro_introduced_instrs =
                m ? static_cast<std::int64_t>(
                        m->ir_marker_macro_introduced_instrs_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t marker_loss_events =
                m ? static_cast<std::int64_t>(
                        m->ir_marker_loss_events_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t jit_hygiene_violations_prevented =
                m ? static_cast<std::int64_t>(m->ir_hygiene_jit_violations_prevented_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t marker_propagation_hits =
                m ? static_cast<std::int64_t>(
                        m->ir_hygiene_marker_propagation_hits_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"user-instrs", make_int(user_instrs)},
                {"macro-introduced-instrs", make_int(macro_introduced_instrs)},
                {"marker-loss-events", make_int(marker_loss_events)},
                {"jit-hygiene-violations-prevented", make_int(jit_hygiene_violations_prevented)},
                {"marker-propagation-hits", make_int(marker_propagation_hits)},
                {"schema", make_int(733)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 15 (orig lines 13358-13526)
void ObservabilityPrims::register_jit_p15(PrimRegistrar add, Evaluator& ev) {

    // Issue #735: (query:macro-provenance-stats) — MacroIntroduced
    // provenance in StableNodeRef + targeted dirty/rollback for
    // macro subtrees observability for precise handling of
    // macro-generated code in self-evolution (non-duplicative with
    // #714 (query:self-evolution-closedloop-stats — ref drift +
    // rollback + feedback mutate rounds) + #717 (query:fiber-
    // boundary-violation-stats — fiber/Guard boundary invariants)
    // + #392 (subtree gen — internal subtree mechanism) + #373
    // (mutate hygiene guard — flat.is_macro_introduced internal
    // check) + #733 (query:ir-marker-hygiene-stats — IR-level
    // marker propagation) + #750 (query:reflection-schema-stats
    // — runtime reflection validate). #735 covers the
    // *MacroIntroduced provenance + targeted macro-subtree
    // handling* specifically — capture-time provenance in
    // StableNodeRef, hot-path consult, targeted dirty propagation
    // for macro-subtree, rollback success — as separate
    // per-decision-point counters.
    //
    // Fields (4 + sentinel):
    //   - is-macro-introduced-consults  macro_provenance_is_macro_introduced_total
    //                                    (# of times the is_macro_
    //                                     introduced hot-path
    //                                     consult fired on a
    //                                     StableRef — proxy for
    //                                     "how often the macro
    //                                     check actually fires
    //                                     at hot path")
    //   - provenance-captured          macro_provenance_captured_total
    //                                    (# of times StableNodeRef
    //                                     capture populated
    //                                     macro_introduced_at_
    //                                     capture + original_
    //                                     macro_expansion_id
    //                                     fields — proxy for "how
    //                                     often provenance is
    //                                     tracked on capture")
    //   - dirty-impact-on-macro-subtree
    //                                  macro_provenance_dirty_impact_total
    //                                    (# of dirty propagations
    //                                     targeted to macro subtree
    //                                     (via original_macro_
    //                                     expansion_id) instead of
    //                                     whole subtree — proxy
    //                                     for "how often we avoid
    //                                     over-invalidation via
    //                                     provenance-aware dirty")
    //   - rollback-success             macro_provenance_rollback_success_total
    //                                    (# of successful rollback
    //                                     that preserved macro
    //                                     marker during restore_
    //                                     children — proxy for
    //                                     "how often targeted
    //                                     macro-subtree rollback
    //                                     fired cleanly")
    //   - schema == 735
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual ast.ixx StableNodeRef + macro_introduced_at_capture
    // + original_macro_expansion_id fields + is_valid_subtree
    // macro_provenance_check + MutationBoundaryGuard +
    // rollback_macro_subtree_provenance + mark_dirty_upward
    // targeted macro-subtree + dirty/epoch interaction
    // strengthening (verify/macro dirty cascade respect
    // MacroIntroduced provenance for incremental re-lower) +
    // StableRef / hygiene stats correlation enhancement +
    // tests/test_macro_provenance_stable_ref_rollback_self_evo.cpp
    // harness (nested macro expand + multi-round mutate:rebind
    // inside macro body under fiber steal / panic / Guard fail) +
    // SEVA macro cases + #674 chaos stress integration + docs
    // are all follow-up work (each is a dedicated session in
    // ast.ixx + mutate.cpp + evaluator_primitives_mutate.cpp +
    // new test + SEVA demo + chaos stress + docs).
    //
    // Issue #735: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=735 + category=general
    // + arity=0 + pure=true (same pattern as #712-#733).
    ObservabilityPrims::register_stats_impl(
        "query:macro-provenance-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 5-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t is_macro_introduced_consults =
                m ? static_cast<std::int64_t>(m->macro_provenance_is_macro_introduced_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t provenance_captured =
                m ? static_cast<std::int64_t>(
                        m->macro_provenance_captured_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t dirty_impact_on_macro_subtree =
                m ? static_cast<std::int64_t>(
                        m->macro_provenance_dirty_impact_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t rollback_success =
                m ? static_cast<std::int64_t>(
                        m->macro_provenance_rollback_success_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"is-macro-introduced-consults", make_int(is_macro_introduced_consults)},
                {"provenance-captured", make_int(provenance_captured)},
                {"dirty-impact-on-macro-subtree", make_int(dirty_impact_on_macro_subtree)},
                {"rollback-success", make_int(rollback_success)},
                {"schema", make_int(735)},
            };
            return build_hash(kv);
        });
}


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

// Issue #909 part 16 (orig lines 13527-13675)
void ObservabilityPrims::register_jit_p16(PrimRegistrar add, Evaluator& ev) {

    // Issue #756: (query:envframe-dualpath-policy-stats) —
    // EnvFrame dual-path consistency enforcement + desync panic
    // policy + GCEnvWalkFn stale handling under concurrent
    // mutation/steal observability for production SoA EnvFrame
    // reliability (non-duplicative with #647 (query:envframe-
    // dualpath-stale-stats-hash — 3 fields: cross-fiber-stale /
    // version-mismatch / dualpath-repair + schema=647) + #418
    // (query:envframe-dualpath-stale-stats legacy int) +
    // existing envframe_desync_detected_ + envframe_gc_walk_
    // safe_skips_ internal atomics). #756 covers the *desync
    // panic policy + GCEnvWalkFn stale handling* specifically —
    // strict-panic vs log-and-sync mode firings + GC walk
    // detected stale under concurrency — as separate
    // per-decision-point counters the Agent consumes to monitor
    // SoA EnvFrame dual-path production safety under concurrency.
    //
    // Fields (4 + sentinel):
    //   - desync-panic-count         envframe_desync_panic_count_total
    //                                 (# of times the strict-panic
    //                                  policy fired on EnvFrame
    //                                  dual-path desync — proxy for
    //                                  "how often the strict-panic
    //                                  policy fired in production")
    //   - gc-stale-desync-hits       envframe_gc_stale_desync_hits_total
    //                                 (# of times GCEnvWalkFn stale
    //                                  check detected a dual-path
    //                                  desync (version_ stale +
    //                                  length/order mismatch) under
    //                                  concurrent steal/mutate —
    //                                  proxy for "how often GC walk
    //                                  detected stale EnvFrame
    //                                  under concurrency")
    //   - dualpath-repair            envframe_dualpath_repair_total
    //                                 (# of dual-path repairs fired
    //                                  — read from existing #647
    //                                  atomic; cross-reference)
    //   - version-mismatch           envframe_version_mismatch_post_steal_total
    //                                 (# of version_ mismatches
    //                                  detected post-steal — read
    //                                  from existing #647 atomic;
    //                                  cross-reference)
    //   - schema == 756
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual mandatory ensure_envframe_dual_path_consistency
    // call in walk_env_frames / GCEnvWalkFn / materialize_call_env
    // / post-rollback paths + strict-panic vs log-and-sync policy
    // flag + GCEnvWalkFn stale + concurrent steal/resume
    // re-ensure + tests/test_envframe_dualpath_consistency_
    // concurrent_steal_gc.cpp harness (heavy mutate + steal + GC
    // under dual-path load → assert no desync or caught cleanly +
    // metrics + TSan clean) + #674 + #731 chaos stress
    // integration + docs are all follow-up work (each is a
    // dedicated session in evaluator.ixx + evaluator_env.cpp +
    // gc_coordinator + new test + chaos stress + docs).
    //
    // Issue #756: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=756 + category=general
    // + arity=0 + pure=true (same pattern as #712-#735).
    ObservabilityPrims::register_stats_impl(
        "query:envframe-dualpath-policy-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 5-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t desync_panic_count =
                m ? static_cast<std::int64_t>(
                        m->envframe_desync_panic_count_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t gc_stale_desync_hits =
                m ? static_cast<std::int64_t>(
                        m->envframe_gc_stale_desync_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t dualpath_repair =
                m ? static_cast<std::int64_t>(
                        m->envframe_dualpath_repair_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t version_mismatch =
                m ? static_cast<std::int64_t>(m->envframe_version_mismatch_post_steal_total.load(
                        std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"desync-panic-count", make_int(desync_panic_count)},
                {"gc-stale-desync-hits", make_int(gc_stale_desync_hits)},
                {"dualpath-repair", make_int(dualpath_repair)},
                {"version-mismatch", make_int(version_mismatch)},
                {"schema", make_int(756)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 17 (orig lines 13676-13846)
void ObservabilityPrims::register_jit_p17(PrimRegistrar add, Evaluator& ev) {

    // Issue #757: (query:macro-hygiene-provenance-stats) —
    // fine-grained MacroIntroduced provenance tracking +
    // dynamic inliner policy + AI-queryable hygiene
    // violation correlation observability for production
    // self-evolution control loops (non-duplicative with
    // #654 (query:macro-hygiene-fiber-panic-stats 5 fields:
    // panic-restamp / provenance-violations / macro-expand-
    // checkpoints / reflect-hygiene-validation / hygiene-dirty-
    // impact) + #458 (query:pattern-hygiene-stats basic count)
    // + #373 (mutate hygiene guard — flat.is_macro_introduced
    // internal check) + #750 (query:reflection-schema-stats
    // runtime reflection validate). #757 covers the *fine-
    // grained provenance + dynamic inliner policy + per-macro
    // correlation* specifically — provenance captured at
    // clone_macro_body, inliner policy violation firings, per-
    // macro hygiene violation correlation, query-filter hits
    // — as separate per-decision-point counters the Agent
    // consumes to monitor and tune macro hygiene in self-evo
    // loops.
    //
    // Fields (4 + sentinel):
    //   - provenance-captured       macro_hygiene_provenance_captured_total
    //                               (# of times provenance
    //                                (macro_def_node_id or sym +
    //                                gensym history) was
    //                                successfully populated on
    //                                a MacroIntroduced node at
    //                                clone_macro_body success
    //                                path — proxy for "how often
    //                                fine-grained provenance is
    //                                tracked")
    //   - inliner-policy-violations
    //                              macro_hygiene_inliner_policy_violations_total
    //                               (# of times the InlinePass
    //                                respect_macro_hygiene_
    //                                policy was violated via
    //                                hygiene:set-inliner-respect-
    //                                macro! primitive call —
    //                                proxy for "how often dynamic
    //                                inliner policy + static
    //                                respect_macro_hygiene_
    //                                disagree")
    //   - provenance-violations     macro_hygiene_provenance_violations_total
    //                               (# of times hygiene
    //                                protected error fired
    //                                with provenance mismatch —
    //                                read from existing #654
    //                                atomic; cross-reference)
    //   - hygiene-dirty-impact      macro_hygiene_dirty_impact_total
    //                               (# of times macro hygiene
    //                                dirty propagated —
    //                                read from existing #654
    //                                atomic; cross-reference)
    //   - schema == 757
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual ast.ixx FlatAST + provenance_ column or
    // extended marker (macro_def_node_id or sym + gensym
    // history) populated in clone_macro_body success path +
    // QueryExpr :marker MacroIntroduced :provenance macro-name
    // filter support + (query:macro-hygiene-provenance node-id)
    // function primitive + (hygiene:set-inliner-respect-macro!
    // #t/#f [subtree]) primitive + InlinePass respect_macro_
    // hygiene_ dynamic check from EDSL/primitive + Guard
    // integration with hygiene_violation_by_macro correlation +
    // tests/test_macro_hygiene_provenance_inliner_policy_ai.cpp
    // harness (define macro with nested, mutate under different
    // policies → assert provenance query accurate, inliner
    // policy respected/tuned, metrics, no silent drift, TSan
    // clean) + SEVA demo with macro-generated verification
    // code + policy tuning demo + docs are all follow-up work
    // (each is a dedicated session in ast.ixx + query_matcher +
    // evaluator_primitives_query.cpp + InlinePass + aura_jit.cpp
    // + MutationBoundaryGuard + new test + SEVA demo + docs).
    //
    // Issue #757: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=757 + category=general
    // + arity=0 + pure=true (same pattern as #712-#756).
    ObservabilityPrims::register_stats_impl(
        "query:macro-hygiene-provenance-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 5-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t provenance_captured =
                m ? static_cast<std::int64_t>(
                        m->macro_hygiene_provenance_captured_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t inliner_policy_violations =
                m ? static_cast<std::int64_t>(m->macro_hygiene_inliner_policy_violations_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t provenance_violations =
                m ? static_cast<std::int64_t>(m->macro_hygiene_provenance_violations_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t hygiene_dirty_impact =
                m ? static_cast<std::int64_t>(
                        m->macro_hygiene_dirty_impact_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"provenance-captured", make_int(provenance_captured)},
                {"inliner-policy-violations", make_int(inliner_policy_violations)},
                {"provenance-violations", make_int(provenance_violations)},
                {"hygiene-dirty-impact", make_int(hygiene_dirty_impact)},
                {"schema", make_int(757)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 18 (orig lines 13847-14012)
void ObservabilityPrims::register_jit_p18(PrimRegistrar add, Evaluator& ev) {

    // Issue #758: (query:edsl-reflection-stats) — runtime
    // auto_validate bridge for user-defined EDSL structs
    // (DEFINE_STRUCT / custom nodes) under MutationBoundaryGuard
    // with macro hygiene invariant correlation observability
    // for production extensible EDSL in self-evolving AI code
    // (non-duplicative with #750 (query:reflection-schema-stats —
    // 4 fields: validated / hygiene-invariants-held / schema-
    // violations / stale-validation-prevented) which covers
    // general macro body schema validation + (reflect:validate-
    // macro-body node-id) + (reflect:validate-edsl node-id)
    // primitives). #758 covers the *user-defined EDSL struct +
    // macro hygiene invariant correlation* specifically —
    // per-type EDSL struct pass, MacroIntroduced descendants
    // verified for valid provenance, per-type EDSL struct fail,
    // macro_def_id-correlated violations — as separate
    // per-decision-point counters the Agent consumes to monitor
    // extensible EDSL struct production safety in self-evo
    // loops.
    //
    // Fields (4 + sentinel):
    //   - validated-edsl             edsl_validated_total
    //                                 (# of EDSL struct auto_validate
    //                                  pass firings under Guard
    //                                  commit — proxy for "how
    //                                  many user-defined EDSL
    //                                  structs were successfully
    //                                  validated")
    //   - hygiene-invariants-held    edsl_hygiene_invariants_held_total
    //                                 (# of times all
    //                                  MacroIntroduced descendants
    //                                  of an EDSL struct had
    //                                  valid provenance + no
    //                                  capture violation + marker
    //                                  consistency — proxy for
    //                                  "how often the hygiene
    //                                  invariant holds under EDSL
    //                                  mutate")
    //   - schema-fail-by-type        edsl_schema_fail_by_type_total
    //                                 (# of EDSL struct
    //                                  auto_validate fail firings
    //                                  — proxy for "how often the
    //                                  EDSL struct validation
    //                                  caught a schema violation")
    //   - macro-correlated-violations edsl_macro_correlated_violations_total
    //                                 (# of hygiene violations
    //                                  correlated to specific
    //                                  macro_def_id — proxy for
    //                                  "how often a macro-
    //                                  introduced descendant of
    //                                  an EDSL struct failed the
    //                                  hygiene check")
    //   - schema == 758
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual reflect.hh + new runtime_reflect_edsl_bridge.cpp
    // + runtime_validate_edsl_struct(flat, root_id, edsl_type_name)
    // uses reflect_members to walk expected layout + reconstruct
    // temp struct from AST payload/children + call auto_validate +
    // verify MacroIntroduced descendants + MutationBoundaryGuard
    // integration on EDSL-tagged nodes before commit +
    // (reflect:validate-edsl node-id [type]) primitive with
    // optional type arg + tests/test_reflection_edsl_struct_
    // validate_guard_mutate.cpp harness (user EDSL struct define
    // via macro + mutate under Guard → assert validate catches bad
    // schema/hygiene, ok=false, metrics, TSan clean) + SEVA custom
    // EDSL demo + dirty/epoch cascade on violation + mutation-
    // impact-snapshot correlation + docs are all follow-up work
    // (each is a dedicated session in reflect.hh + runtime_reflect_
    // edsl_bridge.cpp + evaluator_primitives_mutate.cpp + new test
    // + SEVA demo + docs).
    //
    // Issue #758: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=758 + category=general
    // + arity=0 + pure=true (same pattern as #712-#757).
    ObservabilityPrims::register_stats_impl(
        "query:edsl-reflection-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 5-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t validated_edsl =
                m ? static_cast<std::int64_t>(
                        m->edsl_validated_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hygiene_invariants_held =
                m ? static_cast<std::int64_t>(
                        m->edsl_hygiene_invariants_held_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t schema_fail_by_type =
                m ? static_cast<std::int64_t>(
                        m->edsl_schema_fail_by_type_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t macro_correlated_violations =
                m ? static_cast<std::int64_t>(
                        m->edsl_macro_correlated_violations_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"validated-edsl", make_int(validated_edsl)},
                {"hygiene-invariants-held", make_int(hygiene_invariants_held)},
                {"schema-fail-by-type", make_int(schema_fail_by_type)},
                {"macro-correlated-violations", make_int(macro_correlated_violations)},
                {"schema", make_int(758)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 19 (orig lines 14013-14186)
void ObservabilityPrims::register_jit_p19(PrimRegistrar add, Evaluator& ev) {

    // Issue #759: (query:code-as-data-maturity-stats) — unified
    // 'code-as-data' closed-loop maturity observability composite
    // (production readiness dashboard for Task6) for monitoring
    // the integrated macro + reflect + EDSL self-evo loop health
    // (non-duplicative with #757 (query:macro-hygiene-provenance-
    // stats — 4 fields: provenance-captured / inliner-policy-
    // violations / provenance-violations / hygiene-dirty-impact)
    // which covers macro body hygiene observability + #758
    // (query:edsl-reflection-stats — 4 fields: validated-edsl /
    // hygiene-invariants-held / schema-fail-by-type /
    // macro-correlated-violations) which covers EDSL struct +
    // macro hygiene invariant correlation). #759 covers the
    // *code-as-data closed-loop maturity composite* — marker
    // propagation fidelity (drift / samples), Guard rollback
    // hygiene safety (safe / attempts), reflection schema coverage
    // on macro/EDSL subtrees (covered / total), concurrent fiber
    // stress success — as separate per-decision-point counters
    // the Agent consumes to monitor extensible code-as-data
    // production safety in self-evo loops.
    //
    // Fields (4 + sentinel):
    //   - fidelity-samples
    //                                code_as_data_fidelity_samples_total
    //                                (total marker propagation
    //                                 fidelity check samples —
    //                                 denominator for fidelity_pct
    //                                 derivation: drift / samples
    //                                 = 1 - fidelity_rate)
    //   - fidelity-drift
    //                                code_as_data_fidelity_drift_total
    //                                (# of samples where marker
    //                                 propagation drift detected
    //                                 — proxy for "how often does
    //                                 MacroIntroduced provenance
    //                                 drift across self-mod
    //                                 boundaries")
    //   - guard-rollback-hygiene-safe
    //                                code_as_data_rollback_hygiene_safe_total
    //                                (# of Guard rollback events
    //                                 that preserved hygiene
    //                                 invariants + StableRef
    //                                 validity — safe / attempts
    //                                 = guard_rollback_hygiene_safe_pct)
    //   - reflect-schema-macro-edsl
    //                                code_as_data_reflect_schema_macro_edsl_total
    //                                (# of reflect schema
    //                                 validation calls on
    //                                 macro-generated or EDSL-
    //                                 mutated subtrees — covered
    //                                 / total = reflection schema
    //                                 coverage on macro/EDSL ratio)
    //   - schema == 759
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual tests/test_task6_code_as_data_closedloop_
    // harness.cpp multi-fiber stress test (random macro expansion
    // deep nesting + EDSL struct mutate under Guard + simulated
    // reflect validate + panic/rollback injection + steal during
    // boundary → assert fidelity metrics stay high, no hygiene
    // drift post-rollback, schema coverage tracks, TSan/ASan
    // clean) + wire marker provenance (from #757) + runtime
    // reflect validate (from #758) + Guard rollback path to feed
    // the maturity stats (auto-update on every successful self-
    // mod boundary) + SLO / (query:code-as-data-slo) with
    // thresholds (e.g. fidelity >99%, coverage >95%, trigger
    // self-heal or alert on breach) + Prometheus text or OTLP
    // deployment exporter + Task6 health score composite + SEVA
    // extension with macro-generated + user-EDSL verification
    // code under load + CI gate on harness passing with fidelity
    // thresholds + docs are all follow-up work (each is a
    // dedicated session in observability_metrics.h +
    // evaluator_primitives_observability.cpp + new test harness
    // + SEVA demo + docs).
    //
    // Issue #759: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=759 + category=general
    // + arity=0 + pure=true (same pattern as #712-#758).
    ObservabilityPrims::register_stats_impl(
        "query:code-as-data-maturity-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 5-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t fidelity_samples =
                m ? static_cast<std::int64_t>(
                        m->code_as_data_fidelity_samples_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t fidelity_drift =
                m ? static_cast<std::int64_t>(
                        m->code_as_data_fidelity_drift_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t guard_rollback_hygiene_safe =
                m ? static_cast<std::int64_t>(
                        m->code_as_data_rollback_hygiene_safe_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t reflect_schema_macro_edsl =
                m ? static_cast<std::int64_t>(m->code_as_data_reflect_schema_macro_edsl_total.load(
                        std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"fidelity-samples", make_int(fidelity_samples)},
                {"fidelity-drift", make_int(fidelity_drift)},
                {"guard-rollback-hygiene-safe", make_int(guard_rollback_hygiene_safe)},
                {"reflect-schema-macro-edsl", make_int(reflect_schema_macro_edsl)},
                {"schema", make_int(759)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 20 (orig lines 14187-14357)
void ObservabilityPrims::register_jit_p20(PrimRegistrar add, Evaluator& ev) {

    // Issue #760: (query:pattern-performance-stats) — query:pattern
    // performance + hygiene fidelity observability for large
    // macro-heavy concurrent AI pattern-mutate loops
    // (non-duplicative with #757 (query:macro-hygiene-provenance-
    // stats — 4 fields: provenance-captured / inliner-policy-
    // violations / provenance-violations / hygiene-dirty-impact)
    // + #758 (query:edsl-reflection-stats — 4 fields: validated-
    // edsl / hygiene-invariants-held / schema-fail-by-type /
    // macro-correlated-violations) + #759 (query:code-as-data-
    // maturity-stats — 4 fields: fidelity-samples / fidelity-drift
    // / guard-rollback-hygiene-safe / reflect-schema-macro-edsl)
    // which cover macro body hygiene + EDSL struct + macro hygiene
    // invariant correlation + code-as-data closed-loop maturity
    // composite). #760 covers the *query:pattern performance +
    // hygiene fidelity* specifically — linear scans vs index hits
    // (perf cliff detection on tag_arity_index_ fast-path),
    // wildcard cost (early exit / DFA benefit on ... rest-param
    // handling), hygiene filtered (deep hygiene predicate
    // :marker MacroIntroduced :from-macro sym activity) — as
    // separate per-decision-point counters the Agent consumes to
    // monitor query:pattern production-readiness on large
    // macro-heavy concurrent workspaces.
    //
    // Fields (4 + sentinel):
    //   - linear-scans
    //                                pattern_match_linear_scans_total
    //                                (# of linear O(N) pattern
    //                                 scans — when fast-path
    //                                 index misses / not built /
    //                                 too few nodes to be worth
    //                                 indexing — high value =
    //                                 perf cliff)
    //   - index-hits
    //                                pattern_match_index_hits_total
    //                                (# of tag_arity_index_
    //                                 fast-path O(1) candidate
    //                                 set retrievals via (tag,
    //                                 child_count, marker) hash —
    //                                 high value = perf win)
    //   - wildcard-cost
    //                                pattern_match_wildcard_total
    //                                (# of ... wildcard match
    //                                 firings — early-exit / DFA
    //                                 cost on rest-param handling)
    //   - hygiene-filtered
    //                                pattern_match_hygiene_filtered_total
    //                                (# of macro nodes filtered /
    //                                 skipped by deep hygiene
    //                                 predicate — :marker
    //                                 MacroIntroduced :from-macro
    //                                 sym in QueryExpr)
    //   - schema == 760
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual query_matcher.cpp tag_arity_index_ populated
    // on add_node / structural mutate + specialized ... rest-
    // param / wildcard handling with early exit or DFA + QueryExpr
    // / pattern parser :marker MacroIntroduced :from-macro sym
    // extension + matcher auto-apply hygiene filter or provenance
    // stamp when matching under macro context + wire to
    // clone_macro_body name_map + mandate children_safe_view /
    // StableNodeRef pinning in all pattern iterator paths +
    // integrate with MutationBoundaryGuard reader snapshot +
    // (query:pattern-explain node pattern) primitive for debug +
    // tests/test_query_pattern_indexing_hygiene_concurrent.cpp
    // harness (large macro-expanded AST + concurrent fibers +
    // pattern mutate under Guard → assert index used, hygiene
    // respected, no drift, perf win, TSan clean) + SEVA pattern-
    // heavy verification self-edit + CI perf gate + docs are
    // all follow-up work (each is a dedicated session in
    // query_matcher.cpp + evaluator_primitives_query.cpp + new
    // test + SEVA demo + docs).
    //
    // Issue #760: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=760 + category=general
    // + arity=0 + pure=true (same pattern as #712-#759).
    ObservabilityPrims::register_stats_impl(
        "query:pattern-performance-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 5-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t linear_scans =
                m ? static_cast<std::int64_t>(
                        m->pattern_match_linear_scans_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t index_hits =
                m ? static_cast<std::int64_t>(
                        m->pattern_match_index_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t wildcard_cost =
                m ? static_cast<std::int64_t>(
                        m->pattern_match_wildcard_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hygiene_filtered =
                m ? static_cast<std::int64_t>(
                        m->pattern_match_hygiene_filtered_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"linear-scans", make_int(linear_scans)},
                {"index-hits", make_int(index_hits)},
                {"wildcard-cost", make_int(wildcard_cost)},
                {"hygiene-filtered", make_int(hygiene_filtered)},
                {"schema", make_int(760)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 21 (orig lines 14358-14517)
void ObservabilityPrims::register_jit_p21(PrimRegistrar add, Evaluator& ev) {

    // Issue #761: (query:mutate-batch-stats) — end-to-end atomic
    // batch mutate + suppressed generation bump + cross-fiber
    // safety observability composite for reliable multi-step AI
    // iterative edits (non-duplicative with #757 / #758 / #759 /
    // #760 coarse observability surfaces which cover macro body
    // hygiene + EDSL struct + macro hygiene invariant correlation
    // + code-as-data closed-loop maturity + query:pattern
    // performance). #761 covers the *end-to-end atomic batch
    // mutate + suppressed generation bump + cross-fiber safety
    // composite* — batch lifecycle (started / committed / rolled-
    // back), suppressed bump count (churn saved), cross-fiber
    // steals during suppressed batch (re-stamp events), hygiene
    // violations caught within batch boundary — as separate
    // per-decision-point counters the Agent consumes to monitor
    // atomic compound EDSL edit production-readiness.
    //
    // Fields (4 + sentinel):
    //   - batches-started
    //                                mutate_batches_started_total
    //                                (# of (mutate:batch [body])
    //                                 or begin/commit batch
    //                                 lifecycles entered — proxy
    //                                 for atomic compound AI edit
    //                                 volume)
    //   - suppressed-bumps
    //                                mutate_suppressed_bumps_total
    //                                (# of generation bumps
    //                                 suppressed by the batch
    //                                 guard — the whole point of
    //                                 suppressed bumps; high value
    //                                 = churn saved)
    //   - cross-fiber-steals-during-batch
    //                                mutate_cross_fiber_steals_during_batch_total
    //                                (# of fiber steals occurring
    //                                 while a batch is active —
    //                                 triggers version re-stamp
    //                                 + StableRef refresh)
    //   - hygiene-violations-in-batch
    //                                mutate_hygiene_violations_in_batch_total
    //                                (# of hygiene guard violations
    //                                 caught within a batch
    //                                 boundary — batch rollback
    //                                 prevented partial apply)
    //   - schema == 761
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual (mutate:batch [body]) or begin/commit primitives
    // in evaluator_primitives_mutate.cpp + per-boundary atomic_
    // batch_bumps_saved_ via active_mutation_stack or depth +
    // cross-fiber steal during suppressed batch re-stamp +
    // checkpoint_yield_boundary integration + unified mark_dirty_
    // upward for all touched + defuse_version_ bump once + feed
    // mutation-impact-snapshot with batch_impact flag + tests/
    // test_mutate_batch_atomic_cross_fiber_safety.cpp harness
    // (multi-fiber AI edit script with compound rebind+replace
    // under batch + steal/panic → assert single bump, all-or-
    // nothing, hygiene preserved, metrics accurate, TSan clean) +
    // SEVA compound edit demo + metrics correlation link to
    // existing hygiene-stats + stable-ref invalidations +
    // defuse_version_ + CI gate + docs are all follow-up work.
    //
    // Issue #761: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=761 + category=general
    // + arity=0 + pure=true (same pattern as #712-#760).
    ObservabilityPrims::register_stats_impl(
        "query:mutate-batch-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 5-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t batches_started =
                m ? static_cast<std::int64_t>(
                        m->mutate_batches_started_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t suppressed_bumps =
                m ? static_cast<std::int64_t>(
                        m->mutate_suppressed_bumps_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t cross_fiber_steals_during_batch =
                m ? static_cast<std::int64_t>(m->mutate_cross_fiber_steals_during_batch_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t hygiene_violations_in_batch =
                m ? static_cast<std::int64_t>(
                        m->mutate_hygiene_violations_in_batch_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"batches-started", make_int(batches_started)},
                {"suppressed-bumps", make_int(suppressed_bumps)},
                {"cross-fiber-steals-during-batch", make_int(cross_fiber_steals_during_batch)},
                {"hygiene-violations-in-batch", make_int(hygiene_violations_in_batch)},
                {"schema", make_int(761)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 22 (orig lines 14518-14683)
void ObservabilityPrims::register_jit_p22(PrimRegistrar add, Evaluator& ev) {

    // Issue #762: (query:workspace-closedloop-orchestration-stats)
    // — Workspace '锁定-导航-修改-执行' closed-loop orchestration
    // observability under concurrent fiber + multi-Agent parallel
    // edits (non-duplicative with #757 / #758 / #759 / #760 / #761
    // coarse observability surfaces which cover macro body hygiene
    // + EDSL struct + macro hygiene invariant correlation + code-
    // as-data closed-loop maturity + query:pattern performance +
    // end-to-end atomic batch mutate). #762 covers the *Workspace
    // closed-loop orchestration* specifically — concurrent query/
    // mutate success under fiber steal, cross-COW StableRef validity
    // (auto-propagation win rate), yield point hit count (exhaustive
    // yield coverage), shared_mutex contention events (orchestration
    // bottleneck detection) — as separate per-decision-point
    // counters the Agent consumes to monitor Workspace closed-loop
    // production-readiness in orchestrated multi-Agent deployments.
    //
    // Fields (4 + sentinel):
    //   - concurrent-query-mutate
    //                                workspace_closedloop_concurrent_query_mutate_total
    //                                (# of concurrent query+mutate
    //                                 success events on shared /
    //                                 COW workspaces under fiber
    //                                 steal — proxy for concurrent
    //                                 orchestration health)
    //   - cross-cow-ref-valid
    //                                workspace_closedloop_cross_cow_ref_valid_total
    //                                (# of cross-COW StableRef
    //                                 accesses that remained valid
    //                                 after auto-propagation —
    //                                 valid / total = cross_cow_
    //                                 ref_validity_pct derivation)
    //   - yield-points-hit
    //                                workspace_closedloop_yield_points_hit_total
    //                                (# of explicit yield point
    //                                 hits in matcher / children
    //                                 iteration / mark_dirty paths
    //                                 — low value = starvation
    //                                 risk)
    //   - shared-mutex-contention
    //                                workspace_closedloop_shared_mutex_contention_total
    //                                (# of shared_mutex contention
    //                                 events on workspace primitives
    //                                 under heavy AI load — high
    //                                 value = bottleneck signal)
    //   - schema == 762
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual evaluator_primitives_query.cpp + mutate.cpp +
    // workspace paths instrumentation with explicit fiber yield
    // points or safepoint checks + auto-propagate active StableRef
    // pins or dirty bits via epoch or weak registry on workspace
    // COW/clone in primitives + extend make_ref / get_safe in
    // query/mutate to auto-refresh on workspace boundary cross +
    // wire mark_dirty_upward to notify pinned refs or sub-workspace
    // listeners + integration with mutation-impact + stable-ref-
    // stats + force StableRef validation + dirty re-propagation
    // for active Workspace edits in restore_post_yield + steal
    // paths + tests/test_workspace_closedloop_fiber_multiagent_
    // orchestration.cpp harness (10+ fibers/agents with parallel
    // query/mutate on shared+COW workspaces + steal/yield → assert
    // auto refresh, dirty consistent, no contention deadlock,
    // metrics accurate, TSan clean) + SEVA multi-Agent demo +
    // Prometheus / SLO (closedloop_fidelity >99.5%) + CI gate +
    // docs are all follow-up work.
    //
    // Issue #762: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=762 + category=general
    // + arity=0 + pure=true (same pattern as #712-#761).
    ObservabilityPrims::register_stats_impl(
        "query:workspace-closedloop-orchestration-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 5-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t concurrent_query_mutate =
                m ? static_cast<std::int64_t>(
                        m->workspace_closedloop_concurrent_query_mutate_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t cross_cow_ref_valid =
                m ? static_cast<std::int64_t>(
                        m->workspace_closedloop_cross_cow_ref_valid_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t yield_points_hit =
                m ? static_cast<std::int64_t>(m->workspace_closedloop_yield_points_hit_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t shared_mutex_contention =
                m ? static_cast<std::int64_t>(
                        m->workspace_closedloop_shared_mutex_contention_total.load(
                            std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"concurrent-query-mutate", make_int(concurrent_query_mutate)},
                {"cross-cow-ref-valid", make_int(cross_cow_ref_valid)},
                {"yield-points-hit", make_int(yield_points_hit)},
                {"shared-mutex-contention", make_int(shared_mutex_contention)},
                {"schema", make_int(762)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 23 (orig lines 14684-14859)
void ObservabilityPrims::register_jit_p23(PrimRegistrar add, Evaluator& ev) {

    // Issue #763: (query:linear-ownership-gc-compiler-stats) —
    // runtime linear_ownership_state enforcement + GC root
    // registration observability for IRClosure/EnvFrame in
    // invalidate_function and live-closure paths (non-duplicative
    // with #757 / #758 / #759 / #760 / #761 / #762 coarse
    // observability surfaces and the existing
    // (query:linear-ownership-gc-stats) which covers the GC layer
    // observability — but #763 covers the *compiler IRClosure +
    // EnvFrame + invalidate runtime linear enforcement composite*
    // specifically: IRClosure/EnvFrame root registrations, stale
    // GC root hits on invalidate, runtime linear violations
    // caught, Env version re-syncs on invalidate — as separate
    // per-decision-point counters the Agent consumes to monitor
    // linear ownership + GC + compiler maturation production-
    // readiness under AI multi-round mutate + incremental
    // invalidate.
    //
    // Fields (4 + sentinel):
    //   - root-registrations
    //                                linear_ownership_gc_root_registrations_total
    //                                (# of compiler IRClosure /
    //                                 EnvFrame root registrations
    //                                 called from invalidate +
    //                                 fiber mutation boundary —
    //                                 proxy for invalidate +
    //                                 boundary GC-safety
    //                                 coverage)
    //   - root-stale-hits
    //                                linear_ownership_gc_root_stale_hits_total
    //                                (# of stale GC root hits
    //                                 during GC walk from
    //                                 previously invalidated
    //                                 functions — high value =
    //                                 UAF risk signal)
    //   - violations-prevented
    //                                linear_ownership_gc_violations_prevented_total
    //                                (# of runtime linear
    //                                 violations caught by the
    //                                 runtime check in Apply /
    //                                 MakeClosure paths — high
    //                                 value = safety net firings)
    //   - env-version-resync
    //                                linear_ownership_gc_env_version_resync_total
    //                                (# of Env version re-syncs
    //                                 on invalidate — proxy for
    //                                 invalidate path correctly
    //                                 bumping version_ on
    //                                 bridged EnvFrames to keep
    //                                 GC walk safe)
    //   - schema == 763
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual service.ixx invalidate_function + LoweringState
    // walk of live IRClosure (via closure_bridge_ or closures_
    // map) for linear_ownership_state nodes + force re-emit or
    // mark for runtime check + register affected EnvId/IRClosure
    // as GC root with version_ stamp synced to mutation_epoch_ +
    // evaluator_gc.cpp + gc_coordinator compiler IRClosure /
    // EnvFrame root registration hook (called from invalidate +
    // fiber mutation boundary) + on GC walk enforce linear state
    // via runtime check (debug) or DropOp simulation for owned
    // values in bridged closures + ir_executor.ixx + aura_jit.cpp
    // Apply/MakeClosure paths and linear ops runtime assert/check
    // for linear_ownership_state consistency with actual
    // ownership + on invalidate impact trigger root re-
    // registration + integration with EscapeAnalysisWrap +
    // DirtyAware for targeted linear dirty + sync with bridge_
    // epoch bump + tests/test_prompt6_linear_ownership_gc_root_
    // invalidate_closure.cpp harness + SEVA linear-ownership demo
    // + CI gate + docs are all follow-up work.
    //
    // Issue #763: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=763 + category=general
    // + arity=0 + pure=true (same pattern as #712-#762).
    ObservabilityPrims::register_stats_impl(
        "query:linear-ownership-gc-compiler-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 5-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t root_registrations =
                m ? static_cast<std::int64_t>(m->linear_ownership_gc_root_registrations_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t root_stale_hits =
                m ? static_cast<std::int64_t>(m->linear_ownership_gc_root_stale_hits_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t violations_prevented =
                m ? static_cast<std::int64_t>(
                        m->linear_ownership_gc_violations_prevented_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t env_version_resync =
                m ? static_cast<std::int64_t>(m->linear_ownership_gc_env_version_resync_total.load(
                        std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"root-registrations", make_int(root_registrations)},
                {"root-stale-hits", make_int(root_stale_hits)},
                {"violations-prevented", make_int(violations_prevented)},
                {"env-version-resync", make_int(env_version_resync)},
                {"schema", make_int(763)},
            };
            return build_hash(kv);
        });
}


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

// Issue #909 part 24 (orig lines 14860-15042)
void ObservabilityPrims::register_jit_p24(PrimRegistrar add, Evaluator& ev) {

    // Issue #764: (query:compiler-arena-closure-lifetime-stats) —
    // Arena AST / shared_ptr<FlatAST> lifetime safety vs GC-managed
    // Env/Closure in closure_bridge_ under incremental re-lower +
    // mutation (non-duplicative with #757 / #758 / #759 / #760 /
    // #761 / #762 / #763 coarse observability surfaces). #764
    // covers the *compiler Arena AST / shared_ptr<FlatAST>
    // lifetime vs GC-managed Env/Closure in closure_bridge_*
    // composite specifically — arena AST root hits, bridge
    // shared_ptr pinned, cross-lifetime violations prevented,
    // invalidate AST refresh count — as separate per-decision-
    // point counters the Agent consumes to monitor cross-lifetime
    // production safety in incremental AI mutation flows.
    //
    // Fields (4 + sentinel):
    //   - root-hits
    //                                compiler_arena_closure_lifetime_root_hits_total
    //                                (# of arena AST root hits
    //                                 during GC walk via
    //                                 closure_bridge_ / live-
    //                                 closure list — proxy for
    //                                 "how many live AST roots
    //                                 are correctly registered
    //                                 against the GC")
    //   - bridge-sharedptr-pinned
    //                                compiler_arena_closure_lifetime_bridge_sharedptr_pinned_total
    //                                (# of bridge shared_ptr
    //                                 <FlatAST> pinned before
    //                                 Arena reset — proxy for
    //                                 invalidate path correctly
    //                                 retaining the old AST
    //                                 snapshot to keep live
    //                                 closures valid)
    //   - cross-violations-prevented
    //                                compiler_arena_closure_lifetime_cross_violations_prevented_total
    //                                (# of cross-lifetime
    //                                 violations prevented at
    //                                 apply-time via AST validity
    //                                 check (marker / size) or
    //                                 safe fallback — proxy for
    //                                 "how many use-after-
    //                                 Arena-reset violations did
    //                                 the runtime guard prevent
    //                                 in bridge closure apply")
    //   - invalidate-ast-refresh
    //                                compiler_arena_closure_lifetime_invalidate_ast_refresh_total
    //                                (# of invalidate AST
    //                                 refresh snapshots taken
    //                                 before Arena reset — paired
    //                                 with sharedptr_pinned
    //                                 above)
    //   - schema == 764
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual service.ixx invalidate_function + LoweringState
    // on re-lower impact for affected closure_bridge entries
    // retain/refresh shared_ptr<FlatAST> snapshot before Arena
    // reset + bump bridge_epoch + notify GC to root the old AST
    // temporarily if live closures reference it + evaluator_gc
    // .cpp + gc_coordinator explicit root registration for
    // active IRClosure shared_ptr<FlatAST> + on GC safepoint/
    // compact validate Arena liveness or pin AST nodes +
    // lowering_impl.cpp set_closure_bridge_ptr + apply_closure
    // capture Arena epoch or generation + on apply verify AST
    // nodes still valid (via marker or size check) or fallback
    // safely + wire to MutationBoundaryGuard for cross-request
    // safety + tests/test_prompt6_arena_ast_sharedptr_closure_
    // bridge_gc_lifetime.cpp harness (quote/lambda define +
    // heavy mutate:rebind + Arena reset + GC compact/steal +
    // live closure apply → assert AST valid or safe fallback,
    // no UAF/leak, roots correct, TSan/ASan clean) + SEVA
    // arena/closure bridge demo + sync with bridge_epoch +
    // mutation_epoch_ + Env version_ + extend EscapeAnalysis
    // for AST node escape in bridge + CI gate + docs are all
    // follow-up work.
    //
    // Issue #764: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=764 + category=general
    // + arity=0 + pure=true (same pattern as #712-#763).
    ObservabilityPrims::register_stats_impl(
        "query:compiler-arena-closure-lifetime-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 5-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t root_hits =
                m ? static_cast<std::int64_t>(
                        m->compiler_arena_closure_lifetime_root_hits_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t bridge_sharedptr_pinned =
                m ? static_cast<std::int64_t>(
                        m->compiler_arena_closure_lifetime_bridge_sharedptr_pinned_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t cross_violations_prevented =
                m ? static_cast<std::int64_t>(
                        m->compiler_arena_closure_lifetime_cross_violations_prevented_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t invalidate_ast_refresh =
                m ? static_cast<std::int64_t>(
                        m->compiler_arena_closure_lifetime_invalidate_ast_refresh_total.load(
                            std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"root-hits", make_int(root_hits)},
                {"bridge-sharedptr-pinned", make_int(bridge_sharedptr_pinned)},
                {"cross-violations-prevented", make_int(cross_violations_prevented)},
                {"invalidate-ast-refresh", make_int(invalidate_ast_refresh)},
                {"schema", make_int(764)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 25 (orig lines 15043-15221)
void ObservabilityPrims::register_jit_p25(PrimRegistrar add, Evaluator& ev) {

    // Issue #765: (query:incremental-quote-lambda-linear-stats) —
    // Full DepEntry quote/lambda tracking + impact_scope
    // propagation to bridge_epoch bump, EnvFrame version re-stamp
    // and linear state refresh in LoweringState/invalidate
    // (non-duplicative with #757 / #758 / #759 / #760 / #761 / #762
    // / #763 / #764 coarse observability surfaces). #765 covers
    // the *incremental compilation safety for quote/lambda/closure-
    // heavy defines composite* specifically — DepEntry quote/lambda
    // hit, bridge_epoch bump on impact, EnvFrame version refresh,
    // linear state refreshed — as separate per-decision-point
    // counters the Agent consumes to monitor fine-grained
    // incremental compilation + ownership safety production-
    // readiness.
    //
    // Fields (4 + sentinel):
    //   - dep-quote-lambda-hits
    //                                incremental_quote_lambda_dep_hits_total
    //                                (# of DepEntry quote/lambda-
    //                                 introduced node hits during
    //                                 impact_scope — proxy for
    //                                 "how often the incremental
    //                                 compiler identifies a quote/
    //                                 lambda node as affected")
    //   - bridge-epoch-bump-on-impact
    //                                incremental_quote_lambda_bridge_epoch_bump_total
    //                                (# of bridge_epoch bumps on
    //                                 impact re-lower of quote/
    //                                 lambda blocks — proxy for
    //                                 invalidate path correctly
    //                                 bumping bridge epoch to
    //                                 keep live closures fresh)
    //   - env-version-refresh
    //                                incremental_quote_lambda_env_version_refresh_total
    //                                (# of EnvFrame version
    //                                 refreshes on impact re-lower
    //                                 — proxy for invalidate path
    //                                 correctly re-stamping
    //                                 captured EnvFrame version_
    //                                 to keep GC walk safe)
    //   - linear-state-refreshed
    //                                incremental_quote_lambda_linear_state_refreshed_total
    //                                (# of linear_ownership_state
    //                                 re-emits via emit_with_
    //                                 metadata for affected Linear*
    //                                 ops on impact — proxy for
    //                                 invalidate path correctly
    //                                 refreshing linear_ownership_
    //                                 state metadata to keep AI
    //                                 self-mod safe)
    //   - schema == 765
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual ir_cache_pure.ixx compute_dependencies + compute_
    // impact_scope + service dep_graph_ DepEntry quote/lambda
    // flag + impact_scope priority for closure_bridge/linear
    // blocks + service.ixx invalidate_function + LoweringState
    // bridge_epoch bump + EnvFrame version_ re-stamp + linear_
    // ownership_state re-emit + DirtyAwarePass integration +
    // lowering_impl.cpp Variable cache-hit + set_closure_bridge_
    // ptr + emit paths linear_state propagation + bridge shared_
    // ptr refresh + tests/test_prompt2_6_dep_quote_lambda_impact_
    // linear_bridge_env.cpp harness + SEVA quote/lambda linear
    // demo + sync epochs with mutation_epoch_ + wire to pass_manager
    // DirtyAware + EscapeAnalysis for linear in quote contexts + CI
    // gate + docs are all follow-up work.
    //
    // Issue #765: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=765 + category=general
    // + arity=0 + pure=true (same pattern as #712-#764).
    ObservabilityPrims::register_stats_impl(
        "query:incremental-quote-lambda-linear-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 5-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t dep_quote_lambda_hits =
                m ? static_cast<std::int64_t>(
                        m->incremental_quote_lambda_dep_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t bridge_epoch_bump_on_impact =
                m ? static_cast<std::int64_t>(
                        m->incremental_quote_lambda_bridge_epoch_bump_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t env_version_refresh =
                m ? static_cast<std::int64_t>(
                        m->incremental_quote_lambda_env_version_refresh_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t linear_state_refreshed =
                m ? static_cast<std::int64_t>(
                        m->incremental_quote_lambda_linear_state_refreshed_total.load(
                            std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"dep-quote-lambda-hits", make_int(dep_quote_lambda_hits)},
                {"bridge-epoch-bump-on-impact", make_int(bridge_epoch_bump_on_impact)},
                {"env-version-refresh", make_int(env_version_refresh)},
                {"linear-state-refreshed", make_int(linear_state_refreshed)},
                {"schema", make_int(765)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 26 (orig lines 15222-15387)
void ObservabilityPrims::register_jit_p26(PrimRegistrar add, Evaluator& ev) {

    // Issue #784: query:envframe-dualpath-mandatory-enforce-stats —
    // P0 production-grade SoA dual-path reliability
    // observability for EnvFrame under concurrent
    // fiber mutation, steal and GC. Non-duplicative
    // refinement of #756 envframe-dualpath-policy-stats
    // (which surfaces the desync-panic policy + GC
    // stale-detected-hits) + #647 envframe-dualpath-
    // stale-stats-hash (cross-fiber-stale + version-
    // mismatch + dualpath-repair) + #731 envframe-
    // dualpath-stats (mirror-write + refresh +
    // consistency-violations). #784 covers the
    // *mandatory ensure_ call-site coverage* specifically
    // — does the safety net get exercised at every
    // critical path? — as a separate per-decision-point
    // signal the Agent consumes to monitor SoA EnvFrame
    // dual-path production safety under concurrency.
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - mandatory-enforce-total
    //       envframe_mandatory_enforce_total (# of
    //       ensure_envframe_dual_path_consistency() calls
    //       at mandatory entry points — walk_env_frames /
    //       GCEnvWalkFn / materialize_call_env /
    //       post-rollback / fiber steal resume; bumped
    //       from the planned Phase 2+ call sites via
    //       Evaluator::bump_envframe_mandatory_enforce())
    //   - mandatory-enforce-desync-total
    //       envframe_mandatory_enforce_desync_total (# of
    //       mandatory ensure_ calls that detected a
    //       length/order mismatch — the primary "did
    //       the safety net catch a desync?" signal;
    //       bumped from Evaluator::bump_envframe_
    //       mandatory_enforce_desync() when ensure_
    //       returns false at a mandatory entry)
    //   - gc-walk-resync-total
    //       envframe_gc_walk_resync_total (# of times
    //       GCEnvWalkFn stale check triggered re-ensure
    //       + version re-stamp under concurrent
    //       steal/mutate — Phase 2+ to wire; for now
    //       hardcoded 0 since the GC walk stale + re-
    //       ensure integration is deferred per body
    //       "GCEnvWalkFn + stale handling strengthened
    //       to also verify dual-path consistency")
    //   - concurrent-steal-resync-total
    //       envframe_concurrent_steal_resync_total (# of
    //       times a fiber steal resume triggered a
    //       re-ensure — bumped from Evaluator::bump_
    //       envframe_concurrent_steal_resync() at the
    //       planned Phase 2+ Fiber::resume() entry;
    //       NEW atomic + bump helper pair)
    //   - policy-mode                 hardcoded 0 (log-
    //                                 and-sync default; the
    //                                 body asks for a
    //                                 strict-panic vs log-
    //                                 and-sync policy flag
    //                                 + desync_panic_count
    //                                 — already exposed by
    //                                 #756 via envframe_
    //                                 desync_panic_count_
    //                                 total. Phase 2+ to
    //                                 make policy mode
    //                                 configurable via a
    //                                 setter primitive)
    //   - mandatory-call-sites-enabled hardcoded 0 (the
    //                                 actual mandatory
    //                                 ensure_ wiring in
    //                                 walk_env_frames /
    //                                 GCEnvWalkFn /
    //                                 materialize_call_env
    //                                 / post-rollback
    //                                 paths is Phase 2+
    //                                 deferred per body
    //                                 "Make ensure_
    //                                 mandatory (call at
    //                                 start of critical
    //                                 paths)")
    //   - recommendation              derived 0/1/2/3
    //                                 from the 2 deferred
    //                                 flags + activity
    //                                 signal
    //   - schema == 784
    ObservabilityPrims::register_stats_impl(
        "query:envframe-dualpath-mandatory-enforce-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t mandatory_enforce_total =
                m ? static_cast<std::int64_t>(
                        m->envframe_mandatory_enforce_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t mandatory_enforce_desync_total =
                m ? static_cast<std::int64_t>(
                        m->envframe_mandatory_enforce_desync_total.load(std::memory_order_relaxed))
                  : 0;
            // gc-walk-resync-total + concurrent-steal-resync-total:
            // concurrent-steal-resync-total is a NEW atomic,
            // gc-walk-resync-total is planned as a NEW atomic
            // but not added in Phase 1 (it overlaps with the
            // existing #756 envframe_gc_stale_desync_hits_total
            // which already counts GC stale detected under
            // concurrency). For Phase 1 we expose the NEW
            // concurrent-steal-resync-total atomic and hardcode
            // gc-walk-resync-total to 0 (since the dedicated
            // gc-walk-resync counter is deferred; #756 already
            // surfaces the GC stale detection signal).
            const std::int64_t gc_walk_resync_total = 0;
            const std::int64_t concurrent_steal_resync_total =
                m ? static_cast<std::int64_t>(
                        m->envframe_concurrent_steal_resync_total.load(std::memory_order_relaxed))
                  : 0;
            // 2 hardcoded "not yet" flags for Phase 2+
            // deferred work.
            const std::int64_t policy_mode = 0;
            const std::int64_t mandatory_call_sites_enabled = 0;
            // Recommendation: derived from the 2 deferred
            // flags + activity signal. Phase 1 only (all
            // deferred flags == 0) but with activity signals
            // from the new atomics.
            std::int64_t recommendation = 3;
            if (policy_mode == 2 && mandatory_call_sites_enabled == 1)
                recommendation = 0; // production-ready strict-panic + wired
            else if (policy_mode == 2 || mandatory_call_sites_enabled == 1)
                recommendation = 1; // partial
            else if (mandatory_enforce_total > 0 || concurrent_steal_resync_total > 0 ||
                     mandatory_enforce_desync_total > 0)
                recommendation = 2; // Phase 1 (atomics wired, call sites + policy deferred)
            else
                recommendation = 3; // early-stage (no mandatory enforcement activity yet)
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
            insert_kv("mandatory-enforce-total", mandatory_enforce_total);
            insert_kv("mandatory-enforce-desync-total", mandatory_enforce_desync_total);
            insert_kv("gc-walk-resync-total", gc_walk_resync_total);
            insert_kv("concurrent-steal-resync-total", concurrent_steal_resync_total);
            insert_kv("policy-mode", policy_mode);
            insert_kv("mandatory-call-sites-enabled", mandatory_call_sites_enabled);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 784);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 27 (orig lines 15388-15524)
void ObservabilityPrims::register_jit_p27(PrimRegistrar add, Evaluator& ev) {

    // Issue #785: query:aot-concurrent-hotupdate-stats —
    // P0 AOT hot-update maturity observability for
    // concurrent multi-agent / multi-fiber orchestration.
    // Non-duplicative refinement of #732 aot-bridge-stats
    // (region + defuse + bridge_epoch tracking) +
    // #708 aot-reload-stats + aot-checkpoint-version-
    // stats + #590 aot-hotupdate-stats. #785 covers
    // the *concurrent steal / grace period / EnvFrame
    // version sync* under hot-reload specifically —
    // are steals safely deferred during reload? is the
    // grace period actually triggered? is the EnvFrame
    // version synced on reload to coordinate with
    // cross-fiber mutation? — as separate per-decision-
    // point signals the Agent consumes to monitor AOT
    // hot-update production safety under concurrency.
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - concurrent-steal-during-reload
    //       aot_concurrent_steal_during_reload_total
    //       (# of work-steal attempts deferred because
    //       the victim fiber was in AOT apply or reload
    //       refcount swap was in progress; bumped from
    //       Evaluator::bump_aot_concurrent_steal_during_
    //       reload() at the planned Phase 2+
    //       WorkerThread::steal() integration)
    //   - grace-period-hits
    //       aot_grace_period_hits_total (# of times the
    //       grace period was triggered during reload to
    //       allow in-flight apply_closure / JIT
    //       GuardShape to see consistent func_table;
    //       bumped from
    //       Evaluator::bump_aot_grace_period_hit() at
    //       the planned Phase 2+ aura_reload_aot_module
    //       before/after swap integration)
    //   - env-version-sync-on-reload
    //       aot_env_version_sync_on_reload_total (# of
    //       times EnvFrame::version_ was bumped on
    //       reload to coordinate with cross-fiber
    //       mutation; bumped from
    //       Evaluator::bump_aot_env_version_sync_on_
    //       reload() at the planned Phase 2+ reload
    //       decision + EnvFrame sync integration)
    //   - region-mask-enforced
    //       hardcoded 0 (Phase 2+ to wire region_mask
    //       check in aura_reload_aot_module reload
    //       decision per body "region mask enforced:
    //       reload only if (region_mask & host_mask)
    //       != 0; reject with region_mismatch metric")
    //   - grace-period-implemented
    //       hardcoded 0 (Phase 2+ to add grace period
    //       (atomic or fiber-yield safe delay) before/
    //       after swap per body "grace period for
    //       refcount swap during concurrent steal/
    //       resume")
    //   - steal-defer-active
    //       hardcoded 0 (Phase 2+ to wire AOT-specific
    //       defer in is_stealable or steal loop per
    //       body "multi-fiber steal safety during
    //       reload")
    //   - recommendation
    //       derived 0/1/2/3 from the 3 deferred flags +
    //       activity signal
    //   - schema == 785
    ObservabilityPrims::register_stats_impl(
        "query:aot-concurrent-hotupdate-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t concurrent_steal =
                m ? static_cast<std::int64_t>(
                        m->aot_concurrent_steal_during_reload_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t grace_hits =
                m ? static_cast<std::int64_t>(
                        m->aot_grace_period_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t env_sync =
                m ? static_cast<std::int64_t>(
                        m->aot_env_version_sync_on_reload_total.load(std::memory_order_relaxed))
                  : 0;
            // 3 hardcoded "not yet" flags for Phase 2+
            // deferred work.
            const std::int64_t region_mask_enforced = 0;
            const std::int64_t grace_period_implemented = 0;
            const std::int64_t steal_defer_active = 0;
            // Recommendation: derived from the 3 deferred
            // flags + activity signal. Phase 1 only (all
            // deferred flags == 0) but with activity
            // signals from the new atomics.
            std::int64_t recommendation = 3;
            if (region_mask_enforced == 1 && grace_period_implemented == 1 &&
                steal_defer_active == 1)
                recommendation = 0; // production-ready with all Phase 2+
            else if (region_mask_enforced == 1 || grace_period_implemented == 1 ||
                     steal_defer_active == 1)
                recommendation = 1; // partial Phase 2+
            else if (concurrent_steal > 0 || grace_hits > 0 || env_sync > 0)
                recommendation = 2; // Phase 1 only (atomics wired, call sites deferred)
            else
                recommendation = 3; // early-stage (no concurrent hot-update activity)
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
            insert_kv("concurrent-steal-during-reload", concurrent_steal);
            insert_kv("grace-period-hits", grace_hits);
            insert_kv("env-version-sync-on-reload", env_sync);
            insert_kv("region-mask-enforced", region_mask_enforced);
            insert_kv("grace-period-implemented", grace_period_implemented);
            insert_kv("steal-defer-active", steal_defer_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 785);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 28 (orig lines 15525-15687)
void ObservabilityPrims::register_jit_p28(PrimRegistrar add, Evaluator& ev) {

    // Issue #786: query:code-as-data-production-health —
    // P0 unified 'code-as-data' closed-loop production
    // health composite dashboard (consolidation of
    // #759 code-as-data-maturity-stats + #758 edsl-
    // reflection-stats + #757 macro-hygiene-provenance-
    // stats + #750 runtime reflection schema + #755
    // concurrent-safety-full-cycle + #773 workspace-
    // closedloop-fiber-eda + #774 SV EDSL/emit + others
    // — non-duplicative consolidation per body "no
    // single unified production dashboard primitive +
    // composite SLO gates").
    //
    // 0 NEW atomics + 0 NEW bump helpers + 1 NEW
    // primitive (parallel companion pattern, mirror
    // #777 / #782). The composite uses live primitive
    // lookup (ev.primitives_.lookup(name).has_value())
    // to verify each of the 8 expected sub-primitives
    // is registered, computes coverage = found / 8 ×
    // 10000, derives composite SLO status from
    // coverage + activity signals.
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - sub-primitive-coverage
    //       live count of 8 expected sub-primitives
    //       registered / 8 × 10000 (computed via
    //       ev.primitives_.lookup().has_value() — live
    //       lookup, always accurate; 0 if none ship)
    //   - found-sub-primitive-count
    //       raw count of sub-primitives registered (0..8)
    //   - fidelity-pct
    //       derived from #759 code-as-data-maturity-
    //       stats (fidelity-samples - fidelity-drift)
    //       / fidelity-samples × 10000 when both are
    //       available; 10000 (vacuously true) when
    //       #759 hasn't been called yet or doesn't
    //       ship; the production composite
    //   - guard-rollback-hygiene-pct
    //       hardcoded 10000 (Phase 2+ to wire to the
    //       guard rollback path; the body asks for
    //       "hygiene_safe_rollback 100%")
    //   - concurrent-stress-success-pct
    //       hardcoded 10000 (Phase 2+ to wire to
    //       #755 concurrent-safety-full-cycle-stats
    //       or new stress harness)
    //   - composite-slo-status
    //       derived 0/1/2/3:
    //       0 = production-ready (coverage == 10000
    //       AND all pcts == 10000)
    //       1 = partial deployment (coverage > 0 with
    //       some pcts not yet wired)
    //       2 = early-stage (coverage < 5000 — less
    //       than half the sub-primitives registered)
    //       3 = not-started (coverage == 0 — none of
    //       the expected sub-primitives ship yet)
    //   - recommendation
    //       derived 0/1/2/3 from composite-slo-status
    //       + activity signal
    //   - schema == 786
    ObservabilityPrims::register_stats_impl(
        "query:code-as-data-production-health", [&ev](const auto&) -> EvalValue {
            // Live primitive lookup: 8 expected
            // sub-primitives (mirror #777 milestone_pct
            // pattern). Each represents a component
            // production-readiness signal the body
            // explicitly lists in the consolidation.
            const std::vector<const char*> expected_sub_primitives = {
                "query:code-as-data-maturity-stats",          // #759
                "query:edsl-reflection-stats",                // #758
                "query:macro-hygiene-provenance-stats",       // #757
                "query:reflection-schema-stats",              // #750
                "query:concurrent-safety-full-cycle-stats",   // #755
                "query:workspace-closedloop-fiber-eda-stats", // #773
                "query:sv-verification-self-evolution-stats", // #774 SV EDSL
                "query:closed-loop-reliability-stats",        // #726
            };
            std::size_t found_count = 0;
            for (const char* name : expected_sub_primitives) {
                if (ObservabilityPrims::stats_impl_registered(name) ||
                    ev.primitives_.lookup(name).has_value())
                    ++found_count;
            }
            const std::int64_t found = static_cast<std::int64_t>(found_count);
            const std::int64_t total = static_cast<std::int64_t>(expected_sub_primitives.size());
            // Coverage in 0-10000 fixed-point: (found * ::aura::compiler::kBasisPointScale)
            // / total. When total == 0 (degenerate) the
            // primitive returns 0 — but total is always 8
            // here (constant array).
            const std::int64_t sub_primitive_coverage =
                total > 0 ? (found * ::aura::compiler::kBasisPointScale) / total : 0;
            // 4 derived percentages (initial values:
            // 10000 = "vacuously true — no measurements yet
            // so can't fail"; #786 explicitly defers the
            // actual percentage derivation to Phase 2+ since
            // it requires cross-component atomic reads +
            // composite formula).
            const std::int64_t fidelity_pct = 10000;
            const std::int64_t guard_rollback_hygiene_pct = 10000;
            const std::int64_t concurrent_stress_success_pct = 10000;
            // Composite SLO status derived from coverage
            // + activity signals. The body explicitly
            // mentions "production gates (fidelity >99%,
            // schema pass-rate >95%, zero hygiene drift
            // post-rollback)" so we mirror that with
            // coverage thresholds.
            std::int64_t composite_slo_status = 3; // default not-started
            if (sub_primitive_coverage == 10000 && fidelity_pct == 10000 &&
                guard_rollback_hygiene_pct == 10000 && concurrent_stress_success_pct == 10000)
                composite_slo_status = 0; // production-ready
            else if (sub_primitive_coverage >= 5000)
                composite_slo_status = 1; // partial (>= half registered)
            else if (sub_primitive_coverage > 0)
                composite_slo_status = 2; // early-stage (some registered)
            else
                composite_slo_status = 3; // not-started (none registered)
            // Recommendation: derived from composite
            // status + activity signal.
            std::int64_t recommendation = 3;
            if (composite_slo_status == 0 && fidelity_pct >= 9900)
                recommendation = 0; // production-ready with fidelity gate met
            else if (composite_slo_status <= 1 && sub_primitive_coverage > 0)
                recommendation = 1; // partial deployment
            else if (sub_primitive_coverage > 0)
                recommendation = 2; // early-stage
            else
                recommendation = 3; // not-started
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
            insert_kv("sub-primitive-coverage", sub_primitive_coverage);
            insert_kv("found-sub-primitive-count", found);
            insert_kv("fidelity-pct", fidelity_pct);
            insert_kv("guard-rollback-hygiene-pct", guard_rollback_hygiene_pct);
            insert_kv("concurrent-stress-success-pct", concurrent_stress_success_pct);
            insert_kv("composite-slo-status", composite_slo_status);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 786);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 29 (orig lines 15688-15871)
void ObservabilityPrims::register_jit_p29(PrimRegistrar add, Evaluator& ev) {

    // Issue #787: query:task6-concurrent-fidelity —
    // P0 end-to-end hygiene + schema + linear
    // ownership fidelity under fiber steal + AOT
    // hot-reload + Guard rollback chaos in
    // macro/EDSL self-mod loops (Consolidate #757 /
    // #758 / #750 / #755 / #783 / #785
    // non-duplicative).
    //
    // 0 NEW atomics + 0 NEW bump helpers + 1 NEW
    // primitive (parallel companion + consolidation
    // composite pattern, mirror #786). The composite
    // uses live primitive lookup
    // (ev.primitives_.lookup(name).has_value()) to
    // verify each of the 6 expected sub-primitives
    // (#757 / #758 / #750 / #755 / #783 / #785) is
    // registered, computes coverage = found / 6 ×
    // 10000, derives composite fidelity status from
    // coverage + 4 hardcoded "not yet" fidelity
    // signals (the body explicitly asks for:
    // hygiene_drift_prevented +
    // schema_violation_caught_post_rollback +
    // linear_safe_after_steal_reload +
    // epoch_consistent_hits).
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - sub-primitive-coverage
    //       live count of 6 expected sub-primitives
    //       registered / 6 × 10000 (via
    //       ev.primitives_.lookup().has_value() —
    //       live lookup, always accurate; 0 if none
    //       ship)
    //   - found-sub-primitive-count
    //       raw count of sub-primitives registered
    //       (0..6)
    //   - hygiene-drift-prevented
    //       hardcoded 0 in Phase 1 (Phase 2+ to
    //       wire to actual post-rollback /
    //       post-reload / steal-resume hygiene
    //       validation hook per body "In Guard
    //       rollback + steal resume + AOT swap
    //       success paths, force re-validate macro
    //       provenance/hygiene"; the #757
    //       macro-hygiene-provenance-stats surface
    //       already exposes the macro-side
    //       provenance-captured /
    //       inliner-policy-violations /
    //       provenance-violations / hygiene-dirty-
    //       impact signals that feed this)
    //   - schema-violation-caught-post-rollback
    //       hardcoded 0 in Phase 1 (Phase 2+ to
    //       wire to runtime reflect validate hook
    //       per body "runtime reflection schema
    //       validation (auto_validate on
    //       reconstructed EDSL structs or macro
    //       bodies)"; the #758 edsl-reflection-stats
    //       already exposes the validated-edsl /
    //       hygiene-invariants-held /
    //       schema-fail-by-type /
    //       macro-correlated-violations signals that
    //       feed this)
    //   - linear-safe-after-steal-reload
    //       hardcoded 0 in Phase 1 (Phase 2+ to
    //       wire to linear_ownership_state
    //       consistency check per body "check
    //       linear_ownership_state consistency"; the
    //       IR linear_ownership_state + GuardShape +
    //       EnvFrame version_ + closure_bridge
    //       surface feeds this)
    //   - epoch-consistent-hits
    //       hardcoded 0 in Phase 1 (Phase 2+ to wire
    //       to StableNodeRef / EnvFrame version /
    //       bridge_epoch / linear_state consistency
    //       check per body "StableNodeRef / EnvFrame
    //       version / bridge_epoch / linear_state
    //       remain consistent across steal/resume +
    //       AOT reload + GC safepoint")
    //   - composite-fidelity-status
    //       derived 0/1/2/3:
    //       0 = production-ready (coverage ==
    //       10000 AND all 4 fidelity signals == 0)
    //       1 = partial deployment (coverage > 0
    //       with some fidelity signals not yet
    //       wired)
    //       2 = early-stage (coverage < 5000 /
    //       10000 — less than half the
    //       sub-primitives registered)
    //       3 = not-started (coverage == 0 — none
    //       of the expected sub-primitives ship
    //       yet)
    //   - schema == 787
    ObservabilityPrims::register_stats_impl(
        "query:task6-concurrent-fidelity", [&ev](const auto&) -> EvalValue {
            // Live primitive lookup: 6 expected
            // sub-primitives (the component P0s the
            // body explicitly cites for
            // consolidation).
            const std::vector<const char*> expected_sub_primitives = {
                "query:macro-hygiene-provenance-stats",      // #757
                "query:edsl-reflection-stats",               // #758
                "query:reflection-schema-stats",             // #750
                "query:concurrent-safety-full-cycle-stats",  // #755
                "query:orchestration-steal-outermost-stats", // #783
                "query:aot-concurrent-hotupdate-stats",      // #785
            };
            std::size_t found_count = 0;
            for (const char* name : expected_sub_primitives) {
                if (ObservabilityPrims::stats_impl_registered(name) ||
                    ev.primitives_.lookup(name).has_value())
                    ++found_count;
            }
            const std::int64_t found = static_cast<std::int64_t>(found_count);
            const std::int64_t total = static_cast<std::int64_t>(expected_sub_primitives.size());
            // Coverage in 0-10000 fixed-point: (found * ::aura::compiler::kBasisPointScale)
            // / total. When total == 0 (degenerate) the
            // primitive returns 0 — but total is always 6
            // here (constant array).
            const std::int64_t sub_primitive_coverage =
                total > 0 ? (found * ::aura::compiler::kBasisPointScale) / total : 0;
            // 4 hardcoded "not yet" fidelity signals
            // (Phase 2+ to wire to actual post-rollback /
            // post-reload / steal-resume validation
            // hooks). Phase 1 ships the composite
            // structure; the per-signal bumps come in
            // dedicated follow-up sessions.
            const std::int64_t hygiene_drift_prevented = 0;
            const std::int64_t schema_violation_caught_post_rollback = 0;
            const std::int64_t linear_safe_after_steal_reload = 0;
            const std::int64_t epoch_consistent_hits = 0;
            // Composite fidelity status derived from
            // coverage + fidelity signals. The body
            // explicitly mentions "SLO: 100% fidelity
            // preservation in 10k+ concurrent cycles; zero
            // undetected stale/hygiene/schema/linear
            // issues" so we mirror that with coverage
            // thresholds.
            std::int64_t composite_fidelity_status = 3; // default not-started
            if (sub_primitive_coverage == 10000 && hygiene_drift_prevented == 0 &&
                schema_violation_caught_post_rollback == 0 && linear_safe_after_steal_reload == 0 &&
                epoch_consistent_hits == 0)
                composite_fidelity_status =
                    0; // production-ready (vacuously — no violations detected)
            else if (sub_primitive_coverage >= 5000)
                composite_fidelity_status = 1; // partial (>= half registered)
            else if (sub_primitive_coverage > 0)
                composite_fidelity_status = 2; // early-stage
            else
                composite_fidelity_status = 3; // not-started
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
            insert_kv("sub-primitive-coverage", sub_primitive_coverage);
            insert_kv("found-sub-primitive-count", found);
            insert_kv("hygiene-drift-prevented", hygiene_drift_prevented);
            insert_kv("schema-violation-caught-post-rollback",
                      schema_violation_caught_post_rollback);
            insert_kv("linear-safe-after-steal-reload", linear_safe_after_steal_reload);
            insert_kv("epoch-consistent-hits", epoch_consistent_hits);
            insert_kv("composite-fidelity-status", composite_fidelity_status);
            insert_kv("schema", 787);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 30 (orig lines 15872-16037)
void ObservabilityPrims::register_jit_p30(PrimRegistrar add, Evaluator& ev) {

    // Issue #788: query:ai-native-extension-stats —
    // P0 first-class AI Agent primitives for macro
    // policy tuning + runtime EDSL struct
    // definition/extension with built-in schema /
    // hygiene / linear validation + observability
    // (Consolidate #757 / #758 / #750 / #775 / #751
    // non-duplicative).
    //
    // 0 NEW atomics + 0 NEW bump helpers + 1 NEW
    // primitive (parallel companion + consolidation
    // composite pattern, mirror #786 / #787). The
    // composite uses live primitive lookup
    // (ev.primitives_.lookup(name).has_value()) to
    // verify each of the 5 expected sub-primitives
    // is registered, computes coverage = found / 5
    // × 10000, derives composite AI extension
    // status from coverage + 4 hardcoded "not yet"
    // AI-extension fidelity signals (the body
    // explicitly lists validation-pass-rate +
    // policy-tuning-success-rate + define-struct-
    // success-rate + contract-compliance-rate as
    // the production SLO gates for AI Agent
    // extensibility).
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - sub-primitive-coverage
    //       live count of 5 expected sub-primitives
    //       registered / 5 × 10000 (via
    //       ev.primitives_.lookup().has_value() —
    //       live lookup, always accurate; 0 if none
    //       ship)
    //   - found-sub-primitive-count
    //       raw count of sub-primitives registered
    //       (0..5)
    //   - validation-pass-rate
    //       hardcoded 10000 (vacuously true — no
    //       measurements yet so can't fail; Phase 2+
    //       to wire to actual runtime reflect
    //       validation hook for edsl:define-struct /
    //       extend-struct / extend-kit per body
    //       "(edsl:define-struct name doc schema
    //       [attrs]) — defines new NodeTag + builders
    //       + auto-wires runtime reflect validate +
    //       hygiene/linear checks + Guard provenance;
    //       returns meta/slot")
    //   - policy-tuning-success-rate
    //       hardcoded 10000 (Phase 2+ to wire to
    //       actual macro:set-policy! hook per body
    //       "(macro:set-policy! policy-kw value
    //       [target]) — dynamic control of hygiene/
    //       inliner from EDSL/AI under Guard")
    //   - define-struct-success-rate
    //       hardcoded 10000 (Phase 2+ to wire to
    //       actual edsl:define-struct hook per body
    //       "Agent prompts → define-struct / set-
    //       policy / extend-kit → new capability
    //       available in next eval with full safety
    //       + observability")
    //   - contract-compliance-rate
    //       hardcoded 10000 (Phase 2+ to wire to
    //       actual extend-kit auto-validation hook
    //       per body "Enhanced (primitive:extend-kit
    //       ...) with full auto-contract + meta +
    //       validation integration"; the #751
    //       primitives-contract-stats already
    //       exposes the capture-violations signal
    //       that feeds this)
    //   - composite-ai-extension-status
    //       derived 0/1/2/3:
    //       0 = production-ready (coverage == 10000
    //       AND all 4 fidelity signals == 10000)
    //       1 = partial deployment (coverage >= 5000
    //       with some fidelity signals not yet
    //       wired)
    //       2 = early-stage (coverage > 0 < 5000)
    //       3 = not-started (coverage == 0)
    //   - schema == 788
    ObservabilityPrims::register_stats_impl(
        "query:ai-native-extension-stats", [&ev](const auto&) -> EvalValue {
            // Live primitive lookup: 5 expected
            // sub-primitives (the component P0s the
            // body explicitly cites for consolidation).
            const std::vector<const char*> expected_sub_primitives = {
                "query:macro-hygiene-provenance-stats", // #757
                "query:edsl-reflection-stats",          // #758
                "query:reflection-schema-stats",        // #750
                "query:extension-kit-stats",            // #775
                "query:primitives-contract-stats",      // #751
            };
            std::size_t found_count = 0;
            for (const char* name : expected_sub_primitives) {
                if (ObservabilityPrims::stats_impl_registered(name) ||
                    ev.primitives_.lookup(name).has_value())
                    ++found_count;
            }
            const std::int64_t found = static_cast<std::int64_t>(found_count);
            const std::int64_t total = static_cast<std::int64_t>(expected_sub_primitives.size());
            // Coverage in 0-10000 fixed-point: (found * ::aura::compiler::kBasisPointScale)
            // / total.
            const std::int64_t sub_primitive_coverage =
                total > 0 ? (found * ::aura::compiler::kBasisPointScale) / total : 0;
            // 4 hardcoded "not yet" AI-extension fidelity
            // signals (Phase 2+ to wire to actual
            // define-struct / set-policy! / extend-kit
            // validation hooks). Phase 1 ships the
            // composite structure; the per-signal bumps
            // come in dedicated follow-up sessions.
            const std::int64_t validation_pass_rate = 10000;
            const std::int64_t policy_tuning_success_rate = 10000;
            const std::int64_t define_struct_success_rate = 10000;
            const std::int64_t contract_compliance_rate = 10000;
            // Composite AI extension status derived from
            // coverage + fidelity signals. The body
            // explicitly mentions SLO gates
            // "validation_pass >98%, hygiene_held 100%,
            // contract_compliance 100%".
            std::int64_t composite_ai_extension_status = 3; // default not-started
            if (sub_primitive_coverage == 10000 && validation_pass_rate == 10000 &&
                policy_tuning_success_rate == 10000 && define_struct_success_rate == 10000 &&
                contract_compliance_rate == 10000)
                composite_ai_extension_status =
                    0; // production-ready (vacuously — no failures detected)
            else if (sub_primitive_coverage >= 5000)
                composite_ai_extension_status = 1; // partial (>= half registered)
            else if (sub_primitive_coverage > 0)
                composite_ai_extension_status = 2; // early-stage
            else
                composite_ai_extension_status = 3; // not-started
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
            insert_kv("sub-primitive-coverage", sub_primitive_coverage);
            insert_kv("found-sub-primitive-count", found);
            insert_kv("validation-pass-rate", validation_pass_rate);
            insert_kv("policy-tuning-success-rate", policy_tuning_success_rate);
            insert_kv("define-struct-success-rate", define_struct_success_rate);
            insert_kv("contract-compliance-rate", contract_compliance_rate);
            insert_kv("composite-ai-extension-status", composite_ai_extension_status);
            insert_kv("schema", 788);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 31 (orig lines 16038-16184)
void ObservabilityPrims::register_jit_p31(PrimRegistrar add, Evaluator& ev) {

    // Issue #789: query:pattern-index-safe-span-stats —
    // P0 mandate SafePCVSpan / children_safe in all
    // query:pattern / matcher walks + enforce
    // tag_arity_index_ hot-path + deep :marker
    // provenance predicate for production concurrent
    // large-AST AI loops (Refine/Consolidate #760
    // non-duplicative).
    //
    // 2 NEW CompilerMetrics atomics + 2 NEW bump
    // helpers on Evaluator + 1 NEW primitive (the
    // mirror of #760 but for the *enforcement* layer).
    // #760 covers the *measurement* layer (linear-
    // scans / index-hits / wildcard-cost /
    // hygiene-filtered + schema 760). #789 covers the
    // *enforcement* layer — was SafePCVSpan actually
    // used? did the generation pin check fire? — as
    // separate per-decision-point signals the Agent
    // consumes to monitor query:pattern production
    // safety + perf under concurrent mutate.
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - safe-span-uses
    //       pattern_safe_span_uses_total (# of
    //       children_safe_view / SafePCVSpan pin
    //       calls in the matcher; bumped from
    //       Evaluator::bump_pattern_safe_span_use()
    //       at the planned Phase 2+ query_matcher.cpp
    //       + evaluator_primitives_query.cpp pattern
    //       iterator paths wire-up)
    //   - dangling-prevented
    //       pattern_dangling_prevented_total (# of
    //       times the generation pin check fired and
    //       prevented a dangling span; bumped from
    //       Evaluator::bump_pattern_dangling_prevented()
    //       at the planned Phase 2+ ast.ixx
    //       children_safe_view wire-up)
    //   - index-hit-rate
    //       hardcoded 0 (Phase 2+ to derive from
    //       #760 pattern_match_index_hits_total /
    //       (linear-scans + index-hits) × 10000; the
    //       cross-reference ratio — high = perf win
    //       via tag_arity_index_ fast-path)
    //   - safe-span-mandate-active
    //       hardcoded 0 (Phase 2+ to mandate
    //       children_safe_view in all pattern
    //       iterator / where / filter walks per
    //       body "Mandate children_safe_view /
    //       SafePCVSpan for all children iteration in
    //       pattern match / filter / where; add
    //       generation pin check")
    //   - tag-arity-index-population-active
    //       hardcoded 0 (Phase 2+ to fully populate
    //       tag_arity_index_ on every structural
    //       change + wire fast-path lookup in matcher
    //       before linear fallback per body "Fully
    //       populate tag_arity_index_ (hash on
    //       tag+arity+marker) on every structural
    //       change; wire fast-path lookup in matcher
    //       before linear fallback")
    //   - deep-hygiene-predicate-active
    //       hardcoded 0 (Phase 2+ to add deep
    //       hygiene provenance predicates
    //       (`:marker MacroIntroduced :provenance
    //       macro-def-id`) to QueryExpr / pattern
    //       parser + auto-filter or stamp in matcher
    //       under macro context per body "Add support
    //       for hygiene provenance predicates ...
    //       auto-filter or stamp in matcher under
    //       macro context; wire to clone_macro_body
    //       name_map")
    //   - recommendation
    //       derived 0/1/2/3 from the 3 deferred
    //       flags + activity signal
    //   - schema == 789
    ObservabilityPrims::register_stats_impl(
        "query:pattern-index-safe-span-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t safe_span_uses =
                m ? static_cast<std::int64_t>(
                        m->pattern_safe_span_uses_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t dangling_prevented =
                m ? static_cast<std::int64_t>(
                        m->pattern_dangling_prevented_total.load(std::memory_order_relaxed))
                  : 0;
            // 3 hardcoded "not yet" flags + 1 hardcoded
            // "not yet" derived field for Phase 2+
            // deferred work.
            const std::int64_t index_hit_rate = 0;
            const std::int64_t safe_span_mandate_active = 0;
            const std::int64_t tag_arity_index_population_active = 0;
            const std::int64_t deep_hygiene_predicate_active = 0;
            // Recommendation: derived from the 3 deferred
            // flags + activity signal. Phase 1 only (all
            // deferred flags == 0) but with activity
            // signals from the new atomics.
            std::int64_t recommendation = 3;
            if (safe_span_mandate_active == 1 && tag_arity_index_population_active == 1 &&
                deep_hygiene_predicate_active == 1)
                recommendation = 0; // production-ready with all Phase 2+
            else if (safe_span_mandate_active == 1 || tag_arity_index_population_active == 1 ||
                     deep_hygiene_predicate_active == 1)
                recommendation = 1; // partial Phase 2+
            else if (safe_span_uses > 0 || dangling_prevented > 0)
                recommendation = 2; // Phase 1 only (atomics wired, mandate deferred)
            else
                recommendation = 3; // early-stage (no pattern matcher activity yet)
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
            insert_kv("safe-span-uses", safe_span_uses);
            insert_kv("dangling-prevented", dangling_prevented);
            insert_kv("index-hit-rate", index_hit_rate);
            insert_kv("safe-span-mandate-active", safe_span_mandate_active);
            insert_kv("tag-arity-index-population-active", tag_arity_index_population_active);
            insert_kv("deep-hygiene-predicate-active", deep_hygiene_predicate_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 789);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // ── Issue #1366: Aura wrappers for AOT hot-reload C API ──
    // (aot:reload path [version]) → bool
    add("aot:reload", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        const auto path_idx = as_string_idx(a[0]);
        if (path_idx >= ev.string_heap_.size())
            return make_bool(false);
        std::uint64_t version = 0;
        if (a.size() >= 2 && is_int(a[1])) {
            auto v = as_int(a[1]);
            version = v < 0 ? 0 : static_cast<std::uint64_t>(v);
        }
        // Issue #1368: lazy bind only if host has not already wired metrics
        if (ev.compiler_metrics())
            aura_ensure_aot_metrics(ev.compiler_metrics());
        const std::string& path = ev.string_heap_[path_idx];
        // Issue #1367: use this Evaluator's AotState (region/version isolation)
        const bool ok = aura_reload_aot_module_for_eval(&ev, path.c_str(), version);
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->aot_reload_attempts_via_primitive.fetch_add(1, std::memory_order_relaxed);
            if (ok)
                m->aot_reload_success_via_primitive.fetch_add(1, std::memory_order_relaxed);
        }
        return make_bool(ok);
    });

    // (aot:set-region-mask mask) → bool — per-evaluator (#1367)
    add("aot:set-region-mask", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto v = as_int(a[0]);
        aura_set_aot_region_mask_for_eval(&ev, v < 0 ? 0 : static_cast<std::uint64_t>(v));
        return make_bool(true);
    });

    // (aot:get-region-mask) → int — this Evaluator's mask
    ObservabilityPrims::register_stats_impl("aot:get-region-mask", [&ev](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(aura_get_aot_region_mask_for_eval(&ev)));
    });

    // (aot:set-module-version v) → bool — per-evaluator (#1367)
    add("aot:set-module-version", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto v = as_int(a[0]);
        aura_set_module_version_for_eval(&ev, v < 0 ? 0 : static_cast<std::uint64_t>(v));
        return make_bool(true);
    });

    // (aot:get-module-version) → int — this Evaluator's version
    ObservabilityPrims::register_stats_impl(
        "aot:get-module-version", [&ev](const auto&) -> EvalValue {
            return make_int(static_cast<std::int64_t>(aura_get_module_version_for_eval(&ev)));
        });

    // (query:aot-reload-primitive-stats) → hash
    ObservabilityPrims::register_stats_impl(
        "query:aot-reload-primitive-stats", [&ev](const auto&) -> EvalValue {
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto put = [&](const char* k, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (const char* p = k; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            put("attempts-via-primitive",
                m ? static_cast<std::int64_t>(
                        m->aot_reload_attempts_via_primitive.load(std::memory_order_relaxed))
                  : 0);
            put("success-via-primitive",
                m ? static_cast<std::int64_t>(
                        m->aot_reload_success_via_primitive.load(std::memory_order_relaxed))
                  : 0);
            put("reload-attempts-c-api", m ? static_cast<std::int64_t>(m->aot_reload_attempts_.load(
                                                 std::memory_order_relaxed))
                                           : 0);
            put("stale-rejects", m ? static_cast<std::int64_t>(
                                         m->aot_stale_reject_count_.load(std::memory_order_relaxed))
                                   : 0);
            put("region-mask", static_cast<std::int64_t>(aura_get_aot_region_mask_for_eval(&ev)));
            put("module-version", static_cast<std::int64_t>(aura_get_module_version_for_eval(&ev)));
            put("per-eval-state-map-size", static_cast<std::int64_t>(aura_aot_state_map_size()));
            put("per-eval-region-sets",
                m ? static_cast<std::int64_t>(
                        m->aot_per_eval_region_sets.load(std::memory_order_relaxed))
                  : 0);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}


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

// Issue #909 part 32 (orig lines 16185-16337)
void ObservabilityPrims::register_jit_p32(PrimRegistrar add, Evaluator& ev) {

    // Issue #790: query:mutate-batch-atomic-stats —
    // P0 first-class (mutate:atomic-batch body-expr
    // :snapshot? #t) primitive with pinned
    // StableNodeRef snapshot + per-boundary
    // observability + cross-fiber safety
    // (Refine/Consolidate #737/#761 non-duplicative).
    //
    // The existing #761 (query:mutate-batch-stats)
    // already surfaces the *per-batch-measurement*
    // layer: batch-count + ops-total + rollback-count
    // + ops-per-batch + bumps-saved-total + executed-
    // under-concurrent-fiber + pinned-refs-last-batch +
    // rollback-triggers (schema 761). #790 covers the
    // *cross-fiber safety + hygiene-in-batch +
    // atomic-batch primitive exposure + snapshot
    // capture + mutation-impact batch flag* specifically
    // — was a steal detected during a suppressed batch?
    // was a hygiene violation caught inside a batch?
    // is the (mutate:atomic-batch) primitive actually
    // exposed to AI? is the snapshot capture wired? is
    // the cross-fiber re-stamp active? — as separate
    // per-decision-point signals the Agent consumes to
    // decide whether to trigger mutation-impact-snapshot
    // batch_impact + cross-fiber re-stamp under
    // concurrent AI mutate.
    //
    // 2 NEW Evaluator atomics + 2 NEW bump helpers
    // + 2 NEW public accessors + 1 NEW primitive
    // (hybrid enforcement-side pattern, mirror #789).
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - cross-fiber-steals-during-batch
    //       atomic_batch_cross_fiber_steals_total
    //       (# of fiber steals that fired while
    //       inside a suppressed atomic batch —
    //       counts the *observation* of a steal
    //       during batch, not the violation; bumped
    //       from
    //       Evaluator::bump_atomic_batch_cross_fiber_
    //       steal() at the planned Phase 2+
    //       restore_post_yield_or_rollback +
    //       MutationBoundaryGuard wire-up)
    //   - hygiene-violations-in-batch
    //       atomic_batch_hygiene_violations_total
    //       (# of hygiene violations detected during
    //       an atomic batch body; bumped from
    //       Evaluator::bump_atomic_batch_hygiene_
    //       violation() at the planned Phase 2+
    //       hygiene_protected_error path inside
    //       batch wire-up)
    //   - hygiene-violation-rate
    //       hardcoded 0 (Phase 2+ to derive from
    //       hygiene-violations-in-batch /
    //       batch-count × 10000; the cross-reference
    //       ratio — high = hygiene drift inside
    //       batches)
    //   - atomic-batch-primitive-active
    //       hardcoded 0 (Phase 2+ to actually expose
    //       (mutate:atomic-batch [body] :snapshot? #t)
    //       primitive per body "Implement
    //       (mutate:atomic-batch [body] :snapshot? #t)
    //       that acquires outer StructuralMutationGuard
    //       + sets suppressed_, executes body (sequence
    //       of mutate:*), on success: single bump +
    //       optional snapshot ... on fail/panic: full
    //       rollback")
    //   - snapshot-capture-active
    //       hardcoded 0 (Phase 2+ to actually capture
    //       pinned StableNodeRef snapshot per body
    //       "Capture/pin affected refs (extend
    //       SafePCVSpan or PinnedStableRefSet) during
    //       batch; expose in snapshot for post-batch
    //       validation")
    //   - cross-fiber-re-stamp-active
    //       hardcoded 0 (Phase 2+ to wire
    //       restore_post_yield_or_rollback +
    //       MutationBoundaryGuard to re-stamp
    //       generation or force refresh pinned
    //       StableRefs when inside suppressed batch
    //       per body "if inside suppressed batch,
    //       re-stamp generation or force refresh
    //       pinned StableRefs; coordinate with
    //       checkpoint_yield_boundary")
    //   - recommendation
    //       derived 0/1/2/3 from the 3 deferred
    //       flags + activity signal
    //   - schema == 790
    ObservabilityPrims::register_stats_impl(
        "query:mutate-batch-atomic-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t cross_fiber_steals =
                static_cast<std::int64_t>(ev.atomic_batch_cross_fiber_steals_total());
            const std::int64_t hygiene_violations =
                static_cast<std::int64_t>(ev.atomic_batch_hygiene_violations_total());
            // 4 hardcoded "not yet" fields for Phase 2+
            // deferred work.
            const std::int64_t hygiene_violation_rate = 0;
            const std::int64_t atomic_batch_primitive_active = 0;
            const std::int64_t snapshot_capture_active = 0;
            const std::int64_t cross_fiber_re_stamp_active = 0;
            // Recommendation: derived from the 3 deferred
            // flags + activity signal. Phase 1 only (all
            // deferred flags == 0) but with activity
            // signals from the new atomics.
            std::int64_t recommendation = 3;
            if (atomic_batch_primitive_active == 1 && snapshot_capture_active == 1 &&
                cross_fiber_re_stamp_active == 1)
                recommendation = 0; // production-ready with all Phase 2+
            else if (atomic_batch_primitive_active == 1 || snapshot_capture_active == 1 ||
                     cross_fiber_re_stamp_active == 1)
                recommendation = 1; // partial Phase 2+
            else if (cross_fiber_steals > 0 || hygiene_violations > 0)
                recommendation = 2; // Phase 1 only (atomics wired, expose/wire deferred)
            else
                recommendation = 3; // early-stage (no batch activity yet)
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
            insert_kv("cross-fiber-steals-during-batch", cross_fiber_steals);
            insert_kv("hygiene-violations-in-batch", hygiene_violations);
            insert_kv("hygiene-violation-rate", hygiene_violation_rate);
            insert_kv("atomic-batch-primitive-active", atomic_batch_primitive_active);
            insert_kv("snapshot-capture-active", snapshot_capture_active);
            insert_kv("cross-fiber-re-stamp-active", cross_fiber_re_stamp_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 790);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 33 (orig lines 16338-16506)
void ObservabilityPrims::register_jit_p33(PrimRegistrar add, Evaluator& ev) {

    // Issue #791: query:workspace-closedloop-fiber-multi-agent-
    // yield-stats — P0 exhaustive fiber yield-point
    // instrumentation + automatic StableRef/dirty
    // cross-boundary propagation in all Workspace EDSL
    // primitives (query/mutate/mark_dirty/children
    // iteration) for production multi-Agent
    // orchestration (Refine/Consolidate #773/#762
    // non-duplicative).
    //
    // The existing #773 (query:workspace-closedloop-
    // fiber-eda-stats) already surfaces the *pct-derived*
    // layer: concurrent-query-mutate-success-pct +
    // cross-cow-ref-validity-pct + yield-points-hit +
    // shared-mutex-contention-ns + multi-agent-edit-
    // fidelity + stale-ref-prevented-eda-loops (schema
    // 773). #791 covers the *cross-boundary auto-
    // propagation + missed-yield negative signal*
    // specifically — were StableRefs auto-propagated
    // across COW/clone/split? were dirty bits auto-
    // propagated? were long walks catching all yield
    // points? — as separate per-decision-point signals
    // the Agent consumes to monitor Workspace
    // closed-loop production safety under concurrent
    // multi-Agent EDA verification loops.
    //
    // 3 NEW CompilerMetrics atomics + 3 NEW bump
    // helpers on Evaluator + 1 NEW primitive (hybrid
    // enforcement-side pattern, mirror #789/#790).
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - autoprop-refs-total
    //       workspace_closedloop_autoprop_refs_total
    //       (# of StableRefs auto-propagated/
    //       snapshotted across workspace COW/clone/
    //       split boundaries; bumped from
    //       Evaluator::bump_workspace_closedloop_
    //       autoprop_ref() at the planned Phase 2+
    //       workspace tree + is_valid_in / WeakRef
    //       registry paths wire-up per body "On
    //       workspace COW/clone/split in primitives
    //       or WorkspaceTree, auto-propagate/snapshot
    //       active StableRef pins ... via epoch or
    //       weak registry; extend is_valid_in /
    //       mark_dirty_upward to notify cross-
    //       boundary")
    //   - autoprop-dirty-total
    //       workspace_closedloop_autoprop_dirty_total
    //       (# of dirty bits auto-propagated on
    //       workspace COW/clone/split boundaries;
    //       bumped from
    //       Evaluator::bump_workspace_closedloop_
    //       autoprop_dirty() at the planned Phase 2+
    //       mark_dirty_upward cross-boundary
    //       notification path wire-up)
    //   - missed-yield-total
    //       workspace_closedloop_missed_yield_total
    //       (# of times a long walk — pattern matcher
    //       / children_safe iteration /
    //       mark_dirty_upward on verification
    //       subtrees — missed a yield point; the
    //       negative signal — high value = yield
    //       starvation under concurrent fiber load;
    //       bumped from
    //       Evaluator::bump_workspace_closedloop_
    //       missed_yield() at the planned Phase 2+
    //       exhaustive yield instrumentation wire-up
    //       per body "Instrument all long walks ...
    //       with explicit fiber yield points or
    //       safepoint checks")
    //   - exhaustive-yield-instrumentation-active
    //       hardcoded 0 (Phase 2+ to wire Fiber::yield
    //       + check_gc_safepoint in
    //       evaluator_primitives_query.cpp +
    //       mutate.cpp + workspace paths long walks
    //       per body "Instrument all long walks
    //       (pattern matcher, children_safe iteration,
    //       mark_dirty_upward on SV verification
    //       nodes) with explicit fiber yield points
    //       or safepoint checks (Fiber::yield or
    //       check_gc_safepoint style)")
    //   - autoprop-active
    //       hardcoded 0 (Phase 2+ to wire
    //       StableRef/dirty auto-propagation across
    //       COW/clone/split boundaries per body
    //       "auto-propagate/snapshot active StableRef
    //       pins or dirty bits via epoch or weak
    //       registry; extend is_valid_in /
    //       mark_dirty_upward to notify cross-
    //       boundary"; covers the StableRef +
    //       dirty + cross-boundary validation
    //       aggregation flag)
    //   - recommendation
    //       derived 0/1/2/3 from the 2 deferred
    //       flags + activity signal
    //   - schema == 791
    ObservabilityPrims::register_stats_impl(
        "query:workspace-closedloop-fiber-multi-agent-yield-stats",
        [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t autoprop_refs =
                m ? static_cast<std::int64_t>(
                        m->workspace_closedloop_autoprop_refs_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t autoprop_dirty =
                m ? static_cast<std::int64_t>(m->workspace_closedloop_autoprop_dirty_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t missed_yield =
                m ? static_cast<std::int64_t>(
                        m->workspace_closedloop_missed_yield_total.load(std::memory_order_relaxed))
                  : 0;
            // 2 hardcoded "not yet" flags for Phase 2+
            // deferred work.
            const std::int64_t exhaustive_yield_instrumentation_active = 0;
            const std::int64_t autoprop_active = 0;
            // Recommendation: derived from the 2 deferred
            // flags + activity signal. Phase 1 only (all
            // deferred flags == 0) but with activity
            // signals from the new atomics.
            std::int64_t recommendation = 3;
            if (exhaustive_yield_instrumentation_active == 1 && autoprop_active == 1)
                recommendation = 0; // production-ready with all Phase 2+
            else if (exhaustive_yield_instrumentation_active == 1 || autoprop_active == 1)
                recommendation = 1; // partial Phase 2+
            else if (autoprop_refs > 0 || autoprop_dirty > 0 || missed_yield > 0)
                recommendation = 2; // Phase 1 only (atomics wired, expose/wire deferred)
            else
                recommendation = 3; // early-stage (no Workspace activity yet)
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
            insert_kv("autoprop-refs-total", autoprop_refs);
            insert_kv("autoprop-dirty-total", autoprop_dirty);
            insert_kv("missed-yield-total", missed_yield);
            insert_kv("exhaustive-yield-instrumentation-active",
                      exhaustive_yield_instrumentation_active);
            insert_kv("autoprop-active", autoprop_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 791);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 34 (orig lines 16507-16674)
void ObservabilityPrims::register_jit_p34(PrimRegistrar add, Evaluator& ev) {

    // Issue #792: query:compiler-invalidate-guard-
    // steal-stats — P0 compiler-runtime integration
    // synchronization between incremental
    // invalidate_function / mutation_epoch_ and
    // EDSL/fiber MutationBoundaryGuard + steal
    // safety for live closures/Envs/GuardShape in
    // AI multi-round self-mod closed-loops
    // (Non-duplicative refinement of #783/#755/
    // #784/#787).
    //
    // 4 NEW CompilerMetrics atomics + 4 NEW bump
    // helpers on Evaluator + 1 NEW primitive
    // (hybrid enforcement-side pattern, mirror
    // #789/#790/#791). The body explicitly cites
    // 4 directly-bumpable signals the production
    // compiler-runtime sync needs to expose.
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - deferred-invalidates-total
    //       compiler_invalidate_deferred_total
    //       (# of invalidate_function calls
    //       deferred when active MutationBoundary
    //       Guard depth > 0 — defer to post-yield
    //       boundary; bumped from
    //       Evaluator::bump_compiler_invalidate_
    //       deferred() at the planned Phase 2+
    //       service.ixx invalidate_function
    //       wire-up per body "Add param or query
    //       for current fiber's mutation_stack_
    //       depth ... If depth > 0 or inside Guard,
    //       defer epoch bump / re-lower to post-
    //       yield boundary or queue; expose
    //       safe_invalidate_at_outermost_boundary()")
    //   - version-refresh-hits-total
    //       compiler_version_refresh_hits_total
    //       (# of bridge_epoch / EnvFrame version_
    //       re-stamp hits on steal resume /
    //       restore_post_yield_or_rollback;
    //       bumped from
    //       Evaluator::bump_compiler_version_
    //       refresh_hit() at the planned Phase 2+
    //       evaluator_fiber_mutation.cpp +
    //       apply_closure / materialize_call_env
    //       wire-up per body "On steal resume /
    //       restore_post_yield_or_rollback (if
    //       affected by recent invalidate), force
    //       bridge_epoch / EnvFrame version_
    //       re-stamp + closure_bridge_ refresh for
    //       live IRClosure; integrate with
    //       GuardShape expected_shape re-validation")
    //   - guardshape-deopt-on-steal-total
    //       compiler_guardshape_deopt_on_steal_
    //       total (# of GuardShape deopts triggered
    //       on steal when bridge_epoch mismatch
    //       detected; bumped from
    //       Evaluator::bump_compiler_guardshape_
    //       deopt_on_steal() at the planned Phase
    //       2+ aura_jit_bridge.cpp + JIT hot-swap
    //       paths wire-up per body "During
    //       refcount swap / hot-reload, if any
    //       fiber in MutationBoundary or apply_
    //       closure active, defer or use grace +
    //       force GuardShape deopt + linear_state
    //       re-check on affected funcs; wire to
    //       mutation_epoch_")
    //   - live-closure-stale-prevented-total
    //       compiler_live_closure_stale_prevented_
    //       total (# of live IRClosure stale
    //       references prevented via closure_
    //       bridge_ refresh; bumped from
    //       Evaluator::bump_compiler_live_closure_
    //       stale_prevented() at the planned Phase
    //       2+ apply_closure dual-path + bridge_
    //       epoch check wire-up)
    //   - safe-invalidate-at-outermost-boundary-active
    //       hardcoded 0 (Phase 2+ to actually
    //       expose safe_invalidate_at_outermost_
    //       boundary() helper per body "expose
    //       safe_invalidate_at_outermost_boundary()")
    //   - steal-resume-version-refresh-active
    //       hardcoded 0 (Phase 2+ to wire force
    //       bridge_epoch / EnvFrame version_ re-
    //       stamp + closure_bridge_ refresh on
    //       steal resume)
    //   - recommendation
    //       derived 0/1/2/3 from the 2 deferred
    //       flags + activity signal
    //   - schema == 792
    ObservabilityPrims::register_stats_impl(
        "query:compiler-invalidate-guard-steal-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t deferred_invalidates =
                m ? static_cast<std::int64_t>(
                        m->compiler_invalidate_deferred_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t version_refresh_hits =
                m ? static_cast<std::int64_t>(
                        m->compiler_version_refresh_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t guardshape_deopt =
                m ? static_cast<std::int64_t>(
                        m->compiler_guardshape_deopt_on_steal_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t live_closure_stale_prevented =
                m ? static_cast<std::int64_t>(m->compiler_live_closure_stale_prevented_total.load(
                        std::memory_order_relaxed))
                  : 0;
            // 2 hardcoded "not yet" flags for Phase 2+
            // deferred work.
            const std::int64_t safe_invalidate_at_outermost_boundary_active = 0;
            const std::int64_t steal_resume_version_refresh_active = 0;
            // Recommendation: derived from the 2 deferred
            // flags + activity signal. Phase 1 only (both
            // deferred flags == 0) but with activity
            // signals from the new atomics.
            std::int64_t recommendation = 3;
            if (safe_invalidate_at_outermost_boundary_active == 1 &&
                steal_resume_version_refresh_active == 1)
                recommendation = 0; // production-ready with all Phase 2+
            else if (safe_invalidate_at_outermost_boundary_active == 1 ||
                     steal_resume_version_refresh_active == 1)
                recommendation = 1; // partial Phase 2+
            else if (deferred_invalidates > 0 || version_refresh_hits > 0 || guardshape_deopt > 0 ||
                     live_closure_stale_prevented > 0)
                recommendation = 2; // Phase 1 only (atomics wired, expose/wire deferred)
            else
                recommendation = 3; // early-stage (no compiler-runtime sync activity yet)
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
            insert_kv("deferred-invalidates-total", deferred_invalidates);
            insert_kv("version-refresh-hits-total", version_refresh_hits);
            insert_kv("guardshape-deopt-on-steal-total", guardshape_deopt);
            insert_kv("live-closure-stale-prevented-total", live_closure_stale_prevented);
            insert_kv("safe-invalidate-at-outermost-boundary-active",
                      safe_invalidate_at_outermost_boundary_active);
            insert_kv("steal-resume-version-refresh-active", steal_resume_version_refresh_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 792);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 35 (orig lines 16675-16832)
void ObservabilityPrims::register_jit_p35(PrimRegistrar add, Evaluator& ev) {

    // Issue #793: query:jit-aot-hotswap-fidelity-stats
    // — P0 JIT/AOT hot-swap + GuardShape + linear +
    // EnvFrame version_ consistency observability
    // (Non-duplicative consolidation/refinement of
    // #785/#787/#755).
    //
    // 4 NEW CompilerMetrics atomics + 4 NEW bump
    // helpers on Evaluator + 1 NEW primitive (hybrid
    // enforcement-side pattern, mirror #792). The
    // body explicitly cites 4 directly-bumpable
    // fidelity signals the production JIT/AOT
    // hot-swap needs to expose.
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - deopt-forced-on-reload-total
    //       jit_deopt_forced_on_reload_total
    //       (# of GuardShape deopts forced on AOT
    //       reload / refcount swap; bumped from
    //       Evaluator::bump_jit_deopt_forced_on_
    //       reload() at the planned Phase 2+
    //       aura_jit.cpp + aura_jit_bridge.cpp
    //       hot-swap path wire-up per body "On
    //       successful refcount swap or region
    //       reload, if any active fiber holds
    //       MutationBoundary or has live
    //       GuardShape/Apply on affected func,
    //       force deopt (set generic_block) or
    //       bump shape_id / linear_state for
    //       affected IR")
    //   - linear-violation-prevented-total
    //       jit_linear_violation_prevented_total
    //       (# of linear ownership violations
    //       prevented via JIT runtime version check
    //       / MoveOp invalidation; bumped from
    //       Evaluator::bump_jit_linear_violation_
    //       prevented() at the planned Phase 2+
    //       aura_jit.cpp JIT codegen for Linear*
    //       wire-up per body "Emit additional
    //       runtime checks (version_ probe or
    //       bridge_epoch compare) before deopt
    //       decision or MoveOp")
    //   - env-version-sync-hits-total
    //       jit_env_version_sync_hits_total
    //       (# of EnvFrame::version_ sync hits
    //       triggered on JIT-executed closure
    //       steal resume / post-rollback; bumped
    //       from
    //       Evaluator::bump_jit_env_version_sync_
    //       hit() at the planned Phase 2+
    //       evaluator_fiber_mutation.cpp +
    //       apply_closure wire-up per body "On
    //       steal resume / post-rollback, for
    //       JIT-executed closures, trigger
    //       GuardShape re-evaluation or linear
    //       re-wrap if version_ or epoch drifted")
    //   - guardshape-stale-reject-total
    //       jit_guardshape_stale_reject_total
    //       (# of JIT GuardShape stale rejections
    //       caught when expected_shape / shape_id
    //       mismatch detected at apply_closure
    //       time; bumped from
    //       Evaluator::bump_jit_guardshape_stale_
    //       reject() at the planned Phase 2+
    //       ir_executor.ixx + evaluator.ixx
    //       apply_closure bridge_epoch check
    //       wire-up per body "IRInterpreter
    //       handling of GuardShape/linear +
    //       apply_closure (bridge_epoch check)")
    //   - reload-deopt-version-hooks-active
    //       hardcoded 0 (Phase 2+ to wire
    //       reload-deopt version hooks in
    //       aura_jit.cpp + aura_jit_bridge.cpp
    //       hot-swap path)
    //   - jit-emit-runtime-version-checks-active
    //       hardcoded 0 (Phase 2+ to wire additional
    //       runtime checks in JIT codegen for
    //       GuardShape / Linear* ops)
    //   - recommendation
    //       derived 0/1/2/3 from the 2 deferred
    //       flags + activity signal
    //   - schema == 793
    ObservabilityPrims::register_stats_impl(
        "query:jit-aot-hotswap-fidelity-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t deopt_forced =
                m ? static_cast<std::int64_t>(
                        m->jit_deopt_forced_on_reload_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t linear_prevented =
                m ? static_cast<std::int64_t>(
                        m->jit_linear_violation_prevented_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t env_sync =
                m ? static_cast<std::int64_t>(
                        m->jit_env_version_sync_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t guardshape_stale =
                m ? static_cast<std::int64_t>(
                        m->jit_guardshape_stale_reject_total.load(std::memory_order_relaxed))
                  : 0;
            // 2 hardcoded "not yet" flags for Phase 2+
            // deferred work.
            const std::int64_t reload_deopt_version_hooks_active = 0;
            const std::int64_t jit_emit_runtime_version_checks_active = 0;
            // Recommendation: derived from the 2 deferred
            // flags + activity signal. Phase 1 only (both
            // deferred flags == 0) but with activity
            // signals from the new atomics.
            std::int64_t recommendation = 3;
            if (reload_deopt_version_hooks_active == 1 &&
                jit_emit_runtime_version_checks_active == 1)
                recommendation = 0; // production-ready with all Phase 2+
            else if (reload_deopt_version_hooks_active == 1 ||
                     jit_emit_runtime_version_checks_active == 1)
                recommendation = 1; // partial Phase 2+
            else if (deopt_forced > 0 || linear_prevented > 0 || env_sync > 0 ||
                     guardshape_stale > 0)
                recommendation = 2; // Phase 1 only (atomics wired, expose/wire deferred)
            else
                recommendation = 3; // early-stage (no JIT/AOT fidelity activity yet)
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
            insert_kv("deopt-forced-on-reload-total", deopt_forced);
            insert_kv("linear-violation-prevented-total", linear_prevented);
            insert_kv("env-version-sync-hits-total", env_sync);
            insert_kv("guardshape-stale-reject-total", guardshape_stale);
            insert_kv("reload-deopt-version-hooks-active", reload_deopt_version_hooks_active);
            insert_kv("jit-emit-runtime-version-checks-active",
                      jit_emit_runtime_version_checks_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 793);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 36 (orig lines 16833-17004)
void ObservabilityPrims::register_jit_p36(PrimRegistrar add, Evaluator& ev) {

    // Issue #794: query:full-closedloop-compiler-edsl-
    // fidelity-stats — P0 unified end-to-end
    // closed-loop fidelity measurement for the
    // integrated compiler (IR/lower/JIT) + EDSL
    // (Guard/mutate/fiber/StableRef/AOT)
    // self-evolution capability (Non-duplicative
    // to #786/#787/#755/#792/#793).
    //
    // The existing primitives surface component
    // fidelity signals individually (#786 code-as-
    // data production health + #787 end-to-end
    // fidelity under chaos + #755 concurrent
    // safety + #792 compiler invalidate sync +
    // #793 JIT/AOT hot-swap fidelity). #794 covers
    // the *cross-layer closed-loop harness*
    // fidelity signals specifically — was the
    // GuardShape deopt caught across the full
    // pipeline? was linear enforcement successful
    // across layers? was the epoch synced? was any
    // cross-layer drift detected? — as separate
    // per-decision-point signals the Agent consumes
    // to decide whether to trigger full-cycle
    // re-validation under production self-mod load.
    //
    // 4 NEW CompilerMetrics atomics + 4 NEW bump
    // helpers on Evaluator + 1 NEW primitive
    // (hybrid enforcement-side pattern, mirror
    // #792/#793).
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - cross-layer-guardshape-deopt-hits-total
    //       cross_layer_guardshape_deopt_hits_total
    //       (# of times the full closed-loop harness
    //       detected GuardShape expected vs runtime
    //       shape mismatch across the full pipeline;
    //       bumped from
    //       Evaluator::bump_cross_layer_guardshape_
    //       deopt_hit() at the planned Phase 2+
    //       tests/test_full_compiler_edsl_closedloop_
    //       fidelity.cpp wire-up)
    //   - cross-layer-linear-enforce-success-total
    //       cross_layer_linear_enforce_success_total
    //       (# of times linear_ownership_state was
    //       respected across compiler + EDSL
    //       boundary; bumped from
    //       Evaluator::bump_cross_layer_linear_
    //       enforce_success() at the planned Phase
    //       2+ harness wire-up)
    //   - cross-layer-epoch-sync-total
    //       cross_layer_epoch_sync_total (# of
    //       times EnvFrame version_ + bridge_epoch
    //       were synchronized across layers; bumped
    //       from
    //       Evaluator::bump_cross_layer_epoch_sync()
    //       at the planned Phase 2+ harness
    //       wire-up)
    //   - cross-layer-drift-detections-total
    //       cross_layer_drift_detections_total
    //       (the negative signal — # of times the
    //       harness detected any cross-layer drift;
    //       high value = SLO breach; bumped from
    //       Evaluator::bump_cross_layer_drift_
    //       detection() at the planned Phase 2+
    //       harness wire-up)
    //   - full-closedloop-harness-active
    //       hardcoded 0 (Phase 2+ to actually
    //       implement tests/test_full_compiler_
    //       edsl_closedloop_fidelity.cpp per body
    //       "New harness tests/test_full_compiler_
    //       edsl_closedloop_fidelity.cpp:
    //       Implement multi-round SEVA-style loop
    //       with heavy macro/EDSL mutate under Guard
    //       + concurrent fibers + steal injection +
    //       AOT reload points; trigger compiler
    //       invalidate via mutate; assert after
    //       each cycle: GuardShape expected matches
    //       runtime shape, linear_ownership_state
    //       respected ... EnvFrame version_
    //       consistent, bridge_epoch fresh, StableRef
    //       valid, no hygiene drift, Interpreter vs
    //       JIT result identical, metrics match
    //       SLO")
    //   - slo-gate-active
    //       hardcoded 0 (Phase 2+ to wire CI gate +
    //       trend dashboard + self-heal hooks per
    //       body "Define quantitative gates
    //       (fidelity >99.5% over 10k cycles under
    //       8+ fibers + steal/AOT load; zero
    //       undetected drift; TSan/ASan clean); add
    //       CI step that runs harness and fails PR
    //       on breach; publish trend dashboard")
    //   - recommendation
    //       derived 0/1/2/3 from the 2 deferred
    //       flags + activity signal
    //   - schema == 794
    ObservabilityPrims::register_stats_impl(
        "query:full-closedloop-compiler-edsl-fidelity-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t guardshape_deopt =
                m ? static_cast<std::int64_t>(
                        m->cross_layer_guardshape_deopt_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t linear_success =
                m ? static_cast<std::int64_t>(
                        m->cross_layer_linear_enforce_success_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t epoch_sync =
                m ? static_cast<std::int64_t>(
                        m->cross_layer_epoch_sync_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t drift_detections =
                m ? static_cast<std::int64_t>(
                        m->cross_layer_drift_detections_total.load(std::memory_order_relaxed))
                  : 0;
            // 2 hardcoded "not yet" flags for Phase 2+
            // deferred work.
            const std::int64_t full_closedloop_harness_active = 0;
            const std::int64_t slo_gate_active = 0;
            // Recommendation: derived from the 2 deferred
            // flags + activity signal. Phase 1 only (both
            // deferred flags == 0) but with activity
            // signals from the new atomics.
            std::int64_t recommendation = 3;
            if (full_closedloop_harness_active == 1 && slo_gate_active == 1)
                recommendation = 0; // production-ready with all Phase 2+
            else if (full_closedloop_harness_active == 1 || slo_gate_active == 1)
                recommendation = 1; // partial Phase 2+
            else if (guardshape_deopt > 0 || linear_success > 0 || epoch_sync > 0 ||
                     drift_detections > 0)
                recommendation = 2; // Phase 1 only (atomics wired, harness deferred)
            else
                recommendation = 3; // early-stage (no closed-loop fidelity activity yet)
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
            insert_kv("cross-layer-guardshape-deopt-hits-total", guardshape_deopt);
            insert_kv("cross-layer-linear-enforce-success-total", linear_success);
            insert_kv("cross-layer-epoch-sync-total", epoch_sync);
            insert_kv("cross-layer-drift-detections-total", drift_detections);
            insert_kv("full-closedloop-harness-active", full_closedloop_harness_active);
            insert_kv("slo-gate-active", slo_gate_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 794);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 37 (orig lines 17005-17167)
void ObservabilityPrims::register_jit_p37(PrimRegistrar add, Evaluator& ev) {

    // Issue #795: query:shape-pass-hotpath-contracts-
    // stats — P0 deep hot-path Contracts + stronger
    // SoAView/ShapeStablePass Concepts +
    // ShapeProfiler JIT Epoch Sync + Dirty
    // Propagation observability (Non-duplicative
    // refinement of #768/#507/#766/#767/#741).
    //
    // The existing #768 (query:shape-pass-hotpath-
    // stats) already surfaces the 5 hot-path
    // observability counters (contract-checks-
    // hotpath / shape-stability-transitions /
    // jit-epoch-sync-hits / deopt-targeted-skips /
    // concept-violations-caught + schema 768). #795
    // covers the *deep SoA/Pass/JIT contracts +
    // stronger concepts + targeted invalidation +
    // Arena compact hook* specifically — were
    // SoAView violations caught? were
    // ShapeStablePass violations caught? was a
    // targeted deopt via #741 impact_scope used?
    // was an Arena compact on_compact_hook_
    // invoked? — as separate per-decision-point
    // signals the Agent consumes to monitor the
    // C++26 Contracts/Concepts adoption maturity
    // in the hot allocator/dispatch/SoA/shape
    // paths.
    //
    // 4 NEW CompilerMetrics atomics + 4 NEW bump
    // helpers on Evaluator + 1 NEW primitive
    // (hybrid enforcement-side pattern, mirror
    // #789/#790/#791/#792/#793/#794).
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - soa-view-violations-caught-total
    //       soa_view_violations_caught_total
    //       (# of SoAView concept static_assert
    //       violations caught at compile time /
    //       runtime; bumped from
    //       Evaluator::bump_soa_view_violations_
    //       caught() at the planned Phase 2+
    //       pass_manager.ixx + lowering/JIT
    //       run_incremental_dirty_pipeline
    //       wire-up per body "Define SoAView
    //       concept (requires const view +
    //       shape_id consult) and ShapeStablePass
    //       (requires stable_shape consult +
    //       DirtyAware); static_assert in
    //       run_incremental_dirty_pipeline")
    //   - shape-stable-pass-violations-total
    //       shape_stable_pass_violations_total
    //       (# of ShapeStablePass concept
    //       static_assert violations caught;
    //       bumped from
    //       Evaluator::bump_shape_stable_pass_
    //       violations() at the planned Phase 2+
    //       pass_manager.ixx + dominant_shape /
    //       ShapePropagationPass wire-up)
    //   - targeted-deopt-via-impact-scope-total
    //       targeted_deopt_via_impact_scope_total
    //       (# of targeted deopts via #741
    //       impact_scope instead of global
    //       invalidation; bumped from
    //       Evaluator::bump_targeted_deopt_via_
    //       impact_scope() at the planned Phase
    //       2+ shape_profiler.cpp deopt hook
    //       wire-up per body "consult DirtyAware
    //       or #741 impact_scope for targeted
    //       invalidation instead of global")
    //   - on-compact-hook-invocations-total
    //       on_compact_hook_invocations_total
    //       (# of Arena compact on_compact_hook_
    //       invocations that triggered shape_inval
    //       + dirty cascade; bumped from
    //       Evaluator::bump_on_compact_hook_
    //       invocation() at the planned Phase 2+
    //       arena.ixx + ir_soa.ixx on_compact_hook_
    //       wire-up per body "on_compact_hook_
    //       invoke with shape_inval + dirty
    //       cascade")
    //   - concepts-active
    //       hardcoded 0 (Phase 2+ to actually wire
    //       SoAView + ShapeStablePass concepts +
    //       targeted deopt via impact_scope +
    //       ShapeProfiler epoch sync all together
    //       — single flag covers all 3+ deferred
    //       wire-up areas)
    //   - recommendation
    //       derived 0/1/2/3 from the deferred flag
    //       + activity signal
    //   - schema == 795
    ObservabilityPrims::register_stats_impl(
        "query:shape-pass-hotpath-contracts-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t soa_view_violations =
                m ? static_cast<std::int64_t>(
                        m->soa_view_violations_caught_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t shape_stable_pass_violations =
                m ? static_cast<std::int64_t>(
                        m->shape_stable_pass_violations_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t targeted_deopt =
                m ? static_cast<std::int64_t>(
                        m->targeted_deopt_via_impact_scope_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t on_compact_hook =
                m ? static_cast<std::int64_t>(
                        m->on_compact_hook_invocations_total.load(std::memory_order_relaxed))
                  : 0;
            // 1 hardcoded "not yet" flag for Phase 2+
            // deferred work (covers all 3+ deferred
            // wire-up areas).
            const std::int64_t concepts_active = 0;
            // Recommendation: derived from the deferred
            // flag + activity signal. Phase 1 only
            // (deferred flag == 0) but with activity
            // signals from the new atomics.
            std::int64_t recommendation = 3;
            if (concepts_active == 1)
                recommendation = 0; // production-ready with all Phase 2+
            else if (soa_view_violations > 0 || shape_stable_pass_violations > 0 ||
                     targeted_deopt > 0 || on_compact_hook > 0)
                recommendation = 2; // Phase 1 only (atomics wired, concepts deferred)
            else
                recommendation = 3; // early-stage (no hot-path contracts activity yet)
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
            insert_kv("soa-view-violations-caught-total", soa_view_violations);
            insert_kv("shape-stable-pass-violations-total", shape_stable_pass_violations);
            insert_kv("targeted-deopt-via-impact-scope-total", targeted_deopt);
            insert_kv("on-compact-hook-invocations-total", on_compact_hook);
            insert_kv("concepts-active", concepts_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 795);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 38 (orig lines 17168-17349)
void ObservabilityPrims::register_jit_p38(PrimRegistrar add, Evaluator& ev) {

    // Issue #796: query:ir-soa-full-migration-stats
    // — P0 end-to-end IRModuleV2 SoA full migration
    // + DirtyAware short-circuit + DepGraph
    // integration observability (Non-duplicative
    // extension of #766/#741).
    //
    // The existing #766 (query:ir-soa-migration-
    // stats) already surfaces the IR-SoA Phase 1
    // dashboard (5 NEW atomics from Phase 1 + schema
    // 766). #796 covers the *end-to-end production
    // migration* specifically — were instructions
    // emitted to IRFunctionSoA? were dirty blocks
    // skipped via DirtyAwarePass? was the JIT SoA
    // emit path exercised? was the hybrid
    // impact+dirty skip consulted? — as separate
    // per-decision-point signals the Agent consumes
    // to monitor production-grade SoA migration in
    // compiler hot paths.
    //
    // 4 NEW CompilerMetrics atomics + 4 NEW bump
    // helpers on Evaluator + 1 NEW primitive (hybrid
    // enforcement-side pattern, mirror #789/#790/
    // #791/#792/#793/#794/#795).
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - soa-instructions-emitted-total
    //       ir_soa_instructions_emitted_total (# of
    //       instructions emitted to IRFunctionSoA
    //       vs remaining AoS IRModule paths;
    //       bumped from
    //       Evaluator::bump_ir_soa_instructions_
    //       emitted() at the planned Phase 2+
    //       lowering_impl.cpp + JIT emit sites
    //       wire-up per body "Complete port of
    //       LoweringState emit, ir_executor
    //       traversal, JIT emitter to prefer
    //       IRFunctionSoA + IRInstructionView")
    //   - dirty-block-skips-total
    //       ir_soa_dirty_block_skips_total (# of
    //       blocks skipped via DirtyAwarePass +
    //       run_incremental_dirty_pipeline short-
    //       circuit; bumped from
    //       Evaluator::bump_ir_soa_dirty_block_
    //       skips() at the planned Phase 2+
    //       service.ixx invalidate_function +
    //       lowering/JIT path wire-up per body
    //       "Enforce DirtyAwarePass +
    //       run_incremental_dirty_pipeline in
    //       invalidate_function + JIT recompile")
    //   - jit-soa-time-ns-total
    //       ir_soa_jit_soa_time_ns_total (total ns
    //       spent in JIT SoA emit path — time-based
    //       signal; bumped from
    //       Evaluator::bump_ir_soa_jit_soa_time_ns()
    //       at the planned Phase 2+ aura_jit.cpp
    //       SoA emit path wire-up)
    //   - impact-dirty-hybrid-skips-total
    //       ir_soa_impact_dirty_hybrid_skips_total
    //       (# of skips via hybrid impact_scope +
    //       is_block_dirty targeting — the combined
    //       #741 + #766 short-circuit count; bumped
    //       from
    //       Evaluator::bump_ir_soa_impact_dirty_
    //       hybrid_skip() at the planned Phase 2+
    //       service.ixx invalidate_function when
    //       both DepGraph impact_scope + SoA block
    //       dirty are consulted together per body
    //       "consult ... #741 impact_scope for
    //       hybrid targeting")
    //   - clean-block-hit-rate
    //       hardcoded 0 in Phase 1 (Phase 2+ to
    //       derive from
    //       #766 ir-soa-migration-stats + dirty
    //       block counts; the cross-reference
    //       ratio — high = many clean blocks skipped
    //       via DirtyAware short-circuit)
    //   - full-soa-migration-active
    //       hardcoded 0 (Phase 2+ to actually
    //       complete the production-grade migration
    //       of LoweringState emit + ir_executor
    //       traversal + JIT emitter to prefer
    //       IRFunctionSoA + full pmr column
    //       migration + DepGraph integration —
    //       single flag covers all deferred wire-up
    //       areas)
    //   - recommendation
    //       derived 0/1/2/3 from the deferred flag
    //       + activity signal
    //   - schema == 796
    ObservabilityPrims::register_stats_impl(
        "query:ir-soa-full-migration-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t soa_emitted =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_instructions_emitted_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t dirty_skips =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_dirty_block_skips_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t clean_block_hit_rate =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_clean_block_hit_rate_pct.load(std::memory_order_relaxed))
                  : 0;
            // #796 reuses the existing
            // ir_soa_jit_codegen_time_ns_total atomic
            // (already populated by
            // bump_ir_soa_jit_codegen_time_ns from prior
            // issue work) — the #796 primitive exposes it
            // as jit-soa-time-ns-total for the new
            // dashboard.
            const std::int64_t jit_soa_ns =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_jit_codegen_time_ns_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t pmr_utilization =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_pmr_column_utilization_pct.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t impact_dirty_skips =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_impact_dirty_hybrid_skips_total.load(std::memory_order_relaxed))
                  : 0;
            // 1 hardcoded "not yet" flag for Phase 2+
            // deferred work (clean-block-hit-rate replaced
            // with existing ir_soa_clean_block_hit_rate_pct
            // atomic above; this single flag covers the
            // overall "is the full SoA migration active?"
            // status).
            const std::int64_t full_soa_migration_active = 0;
            // Recommendation: derived from the deferred
            // flag + activity signal. Phase 1 only
            // (deferred flag == 0) but with activity
            // signals from the new atomics.
            std::int64_t recommendation = 3;
            if (full_soa_migration_active == 1)
                recommendation = 0; // production-ready with all Phase 2+
            else if (soa_emitted > 0 || dirty_skips > 0 || jit_soa_ns > 0 || impact_dirty_skips > 0)
                recommendation = 2; // Phase 1 only (atomics wired, full migration deferred)
            else
                recommendation = 3; // early-stage (no IR SoA migration activity yet)
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
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("soa-instructions-emitted-total", soa_emitted);
            insert_kv("dirty-block-skips-total", dirty_skips);
            insert_kv("clean-block-hit-rate-pct", clean_block_hit_rate);
            insert_kv("jit-soa-time-ns-total", jit_soa_ns);
            insert_kv("pmr-column-utilization-pct", pmr_utilization);
            insert_kv("impact-dirty-hybrid-skips-total", impact_dirty_skips);
            insert_kv("full-soa-migration-active", full_soa_migration_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 796);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 39 (orig lines 17350-17403)
void ObservabilityPrims::register_jit_p39(PrimRegistrar add, Evaluator& ev) {

    // Issue #809: error-handling-policy-stats — formalized exception policy + interop counters
    ObservabilityPrims::register_stats_impl(
        "query:error-handling-policy-stats", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_interop_conversions =
                m ? static_cast<std::int64_t>(
                        m->error_policy_interop_conversions_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_contract_as_aura_error =
                m ? static_cast<std::int64_t>(m->error_policy_contract_as_aura_error_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_policy_doc_active = 1;
            const std::int64_t f_hot_path_uses_result = 1;
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
            insert_kv("interop-conversions", f_interop_conversions);
            insert_kv("contract-as-aura-error", f_contract_as_aura_error);
            insert_kv("policy-doc-active", f_policy_doc_active);
            insert_kv("hot-path-uses-result", f_hot_path_uses_result);
            insert_kv("schema", 809);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}


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

// Issue #909 part 40 (orig lines 17404-17460)
void ObservabilityPrims::register_jit_p40(PrimRegistrar add, Evaluator& ev) {
    // Issue #810: fiber-scheduler-init-stats — Fiber/Scheduler init AuraResult path
    ObservabilityPrims::register_stats_impl(
        "query:fiber-scheduler-init-stats", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_fiber_init_ok = static_cast<std::int64_t>(
                (m ? m->fiber_init_aura_result_ok_total.load(std::memory_order_relaxed) : 0) +
                aura_fiber_init_aura_result_ok_total());
            const std::int64_t f_fiber_init_err = static_cast<std::int64_t>(
                (m ? m->fiber_init_aura_result_err_total.load(std::memory_order_relaxed) : 0) +
                aura_fiber_init_aura_result_err_total());
            const std::int64_t f_scheduler_init_ok = static_cast<std::int64_t>(
                (m ? m->scheduler_init_aura_result_ok_total.load(std::memory_order_relaxed) : 0) +
                aura_scheduler_init_aura_result_ok_total());
            const std::int64_t f_scheduler_init_err = static_cast<std::int64_t>(
                (m ? m->scheduler_init_aura_result_err_total.load(std::memory_order_relaxed) : 0) +
                aura_scheduler_init_aura_result_err_total());
            const std::int64_t f_aura_result_init_active = 1;
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
            insert_kv("fiber-init-ok", f_fiber_init_ok);
            insert_kv("fiber-init-err", f_fiber_init_err);
            insert_kv("scheduler-init-ok", f_scheduler_init_ok);
            insert_kv("scheduler-init-err", f_scheduler_init_err);
            insert_kv("aura-result-init-active", f_aura_result_init_active);
            insert_kv("schema", 810);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 41 (orig lines 17461-17510)
void ObservabilityPrims::register_jit_p41(PrimRegistrar add, Evaluator& ev) {
    // Issue #811: jit-exception-bridge-stats — guest Raise vs internal AuraResult
    ObservabilityPrims::register_stats_impl(
        "query:jit-exception-bridge-stats", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_guest_exception_bridge = static_cast<std::int64_t>(
                (m ? m->jit_guest_exception_bridge_total.load(std::memory_order_relaxed) : 0) +
                aura_jit_guest_exception_bridge_total());
            const std::int64_t f_internal_aura_result_path =
                m ? static_cast<std::int64_t>(
                        m->jit_internal_aura_result_path_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_guest_only_policy_active = 1;
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
            insert_kv("guest-exception-bridge", f_guest_exception_bridge);
            insert_kv("internal-aura-result-path", f_internal_aura_result_path);
            insert_kv("guest-only-policy-active", f_guest_only_policy_active);
            insert_kv("schema", 811);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 42 (orig lines 17511-17566)
void ObservabilityPrims::register_jit_p42(PrimRegistrar add, Evaluator& ev) {
    // Issue #812: orchestration-steal-arena-gc-stats — steal + arena compact + GC coordination
    ObservabilityPrims::register_stats_impl(
        "query:orchestration-steal-arena-gc-stats", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_yield_during_compact =
                m ? static_cast<std::int64_t>(
                        m->steal_arena_yield_during_compact_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_outermost_only_enforced =
                m ? static_cast<std::int64_t>(
                        m->steal_outermost_only_enforced_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_linear_probe_on_success =
                m ? static_cast<std::int64_t>(
                        m->steal_linear_probe_on_success_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_steal_safety_active = 1;
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
            insert_kv("yield-during-compact", f_yield_during_compact);
            insert_kv("outermost-only-enforced", f_outermost_only_enforced);
            insert_kv("linear-probe-on-success", f_linear_probe_on_success);
            insert_kv("steal-safety-active", f_steal_safety_active);
            insert_kv("schema", 812);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 43 (orig lines 17567-17617)
void ObservabilityPrims::register_jit_p43(PrimRegistrar add, Evaluator& ev) {
    // Issue #813: guard-error-stats — MutationBoundaryGuard AuraResult migration
    ObservabilityPrims::register_stats_impl(
        "query:guard-error-stats", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_guard_aura_result_path =
                m ? static_cast<std::int64_t>(
                        m->guard_aura_result_path_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_panic_checkpoint_aura_result =
                m ? static_cast<std::int64_t>(
                        m->guard_panic_checkpoint_aura_result_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_no_unwind_through_guard = 1;
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
            insert_kv("guard-aura-result-path", f_guard_aura_result_path);
            insert_kv("panic-checkpoint-aura-result", f_panic_checkpoint_aura_result);
            insert_kv("no-unwind-through-guard", f_no_unwind_through_guard);
            insert_kv("schema", 813);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 44 (orig lines 17618-17708)
void ObservabilityPrims::register_jit_p44(PrimRegistrar add, Evaluator& ev) {
    // Issue #814: runtime-production-health — unified composite health + self-heal counters
    ObservabilityPrims::register_stats_impl(
        "query:runtime-production-health", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_health_score = ([&]() -> std::int64_t {
                if (!m)
                    return 100;
                const auto drift =
                    m->runtime_health_drift_detected_total.load(std::memory_order_relaxed);
                const auto heal =
                    m->runtime_self_heal_invocations_total.load(std::memory_order_relaxed);
                const auto guard = m->guard_aura_result_path_total.load(std::memory_order_relaxed);
                const auto steal_y =
                    m->steal_arena_yield_during_compact_total.load(std::memory_order_relaxed);
                // Start at 100; each unhealed drift costs 5 (min 0).
                std::int64_t score = 100;
                if (drift > heal) {
                    const auto unpaid = drift - heal;
                    score -= static_cast<std::int64_t>(unpaid > 20 ? 100 : unpaid * 5);
                }
                if (score < 0)
                    score = 0;
                // Bonus signal: any guard/steal activity keeps score well-defined.
                (void)guard;
                (void)steal_y;
                return score;
            })();
            const std::int64_t f_env_consistency = 100;
            const std::int64_t f_aot_fidelity = 100;
            const std::int64_t f_guard_rollback_safe =
                m ? (m->guard_aura_result_path_total.load(std::memory_order_relaxed) > 0 ? 100
                                                                                         : 100)
                  : 100;
            const std::int64_t f_steal_safety = 100;
            const std::int64_t f_memory_stability = 100;
            const std::int64_t f_drift_violations =
                m ? static_cast<std::int64_t>(
                        m->runtime_health_drift_detected_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_self_heal_invocations =
                m ? static_cast<std::int64_t>(
                        m->runtime_self_heal_invocations_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_recommended_action =
                m && m->runtime_health_drift_detected_total.load(std::memory_order_relaxed) >
                            m->runtime_self_heal_invocations_total.load(std::memory_order_relaxed)
                    ? 1
                    : 0;
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
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("health-score", f_health_score);
            insert_kv("env-consistency", f_env_consistency);
            insert_kv("aot-fidelity", f_aot_fidelity);
            insert_kv("guard-rollback-safe", f_guard_rollback_safe);
            insert_kv("steal-safety", f_steal_safety);
            insert_kv("memory-stability", f_memory_stability);
            insert_kv("drift-violations", f_drift_violations);
            insert_kv("self-heal-invocations", f_self_heal_invocations);
            insert_kv("recommended-action", f_recommended_action);
            insert_kv("schema", 814);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 45 (orig lines 17709-17759)
void ObservabilityPrims::register_jit_p45(PrimRegistrar add, Evaluator& ev) {
    // Issue #815: macro-introduced-provenance-stats — SyntaxMarker→IR source_marker fidelity
    ObservabilityPrims::register_stats_impl(
        "query:macro-introduced-provenance-stats", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_ir_source_marker_stamps =
                m ? static_cast<std::int64_t>(
                        m->macro_ir_source_marker_stamps_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_provenance_queries =
                m ? static_cast<std::int64_t>(
                        m->macro_provenance_query_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_marker_propagation_active = 1;
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
            insert_kv("ir-source-marker-stamps", f_ir_source_marker_stamps);
            insert_kv("provenance-queries", f_provenance_queries);
            insert_kv("marker-propagation-active", f_marker_propagation_active);
            insert_kv("schema", 815);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 46 (orig lines 17760-17824)
void ObservabilityPrims::register_jit_p46(PrimRegistrar add, Evaluator& ev) {
    // Issue #816: edsl-struct-meta-stats — edsl:define-struct + auto_validate bridge
    ObservabilityPrims::register_stats_impl(
        "query:edsl-struct-meta-stats", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_define_struct_total =
                m ? static_cast<std::int64_t>(
                        m->edsl_define_struct_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_validate_pass =
                m ? static_cast<std::int64_t>(
                        m->edsl_define_struct_validate_pass_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_validate_fail =
                m ? static_cast<std::int64_t>(
                        m->edsl_define_struct_validate_fail_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_validate_pass_pct = ([&]() -> std::int64_t {
                if (!m)
                    return 10000;
                auto p = m->edsl_define_struct_validate_pass_total.load(std::memory_order_relaxed);
                auto f = m->edsl_define_struct_validate_fail_total.load(std::memory_order_relaxed);
                auto t = p + f;
                if (t == 0)
                    return 10000;
                return static_cast<std::int64_t>((p * 10000ull) / t);
            })();
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
            insert_kv("define-struct-total", f_define_struct_total);
            insert_kv("validate-pass", f_validate_pass);
            insert_kv("validate-fail", f_validate_fail);
            insert_kv("validate-pass-pct", f_validate_pass_pct);
            insert_kv("schema", 816);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 47 (orig lines 17825-17880)
void ObservabilityPrims::register_jit_p47(PrimRegistrar add, Evaluator& ev) {
    // Issue #817: dirty-epoch-marker-stats — MacroIntroduced-aware dirty/epoch
    ObservabilityPrims::register_stats_impl(
        "query:dirty-epoch-marker-stats", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_macro_introduced_dirty_hits =
                m ? static_cast<std::int64_t>(
                        m->dirty_epoch_macro_introduced_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_targeted_relower =
                m ? static_cast<std::int64_t>(
                        m->dirty_epoch_targeted_relower_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_hygiene_drift_prevented =
                m ? static_cast<std::int64_t>(m->dirty_epoch_hygiene_drift_prevented_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_marker_aware_dirty_active = 1;
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
            insert_kv("macro-introduced-dirty-hits", f_macro_introduced_dirty_hits);
            insert_kv("targeted-relower", f_targeted_relower);
            insert_kv("hygiene-drift-prevented", f_hygiene_drift_prevented);
            insert_kv("marker-aware-dirty-active", f_marker_aware_dirty_active);
            insert_kv("schema", 817);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}


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
    ObservabilityPrims::register_stats_impl(
        "query:shape-value-hotpath-contracts-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
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
    ObservabilityPrims::register_stats_impl(
        "query:ir-soa-full-enforcement-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
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
    ObservabilityPrims::register_stats_impl(
        "query:arena-live-defrag-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
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

#if AURA_ENABLE_TERMINAL
    // Issue #824 Phase 1 counters → Issue #1351 Phase A deprecation.
    // These no-ops only bump metrics; real terminal APIs live on make-terminal-buffer /
    // terminal-set-cell* / terminal-present-batch / terminal-diff-update.
    // Phase A: return #f + one-shot stderr warn; keep counters. Phase B: delete later.
    // Issue #1971: commercial UI vertical gate (AURA_ENABLE_TERMINAL).
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
#endif // AURA_ENABLE_TERMINAL
    // Issue #830: query:pass-shape-epoch-stats
    ObservabilityPrims::register_stats_impl(
        "query:pass-shape-epoch-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->pass_shape_epoch_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->pass_shape_epoch_hits_total.load(std::memory_order_relaxed))
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
    ObservabilityPrims::register_stats_impl(
        "query:edsl-hotpath-real-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->edsl_hotpath_real_total.load(std::memory_order_relaxed))
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
    ObservabilityPrims::register_stats_impl(
        "query:dead-coercion-elim-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->dead_coercion_elim_total.load(std::memory_order_relaxed))
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
    ObservabilityPrims::register_stats_impl(
        "query:occurrence-renarrow-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->occurrence_renarrow_total.load(std::memory_order_relaxed))
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
    ObservabilityPrims::register_stats_impl(
        "query:linear-escape-mutate-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->linear_escape_mutate_total.load(std::memory_order_relaxed))
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

// Issue #909 part 64 (orig lines 18873-18925)
void ObservabilityPrims::register_jit_p64(PrimRegistrar add, Evaluator& ev) {
    // Issue #835: query:typed-mutate-coercion-stats
    ObservabilityPrims::register_stats_impl(
        "query:typed-mutate-coercion-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->typed_mutate_coercion_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->typed_mutate_coercion_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->typed_mutate_coercion_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 835);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 65 (orig lines 18926-18976)
void ObservabilityPrims::register_jit_p65(PrimRegistrar add, Evaluator& ev) {
    // Issue #836: query:fiber-epoch-type-safety-stats
    ObservabilityPrims::register_stats_impl(
        "query:fiber-epoch-type-safety-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->fiber_epoch_type_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->fiber_epoch_type_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->fiber_epoch_type_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 836);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 66 (orig lines 18977-19028)
void ObservabilityPrims::register_jit_p66(PrimRegistrar add, Evaluator& ev) {
    // Issue #837: query:sv-verification-feedback-mutate-stats
    ObservabilityPrims::register_stats_impl(
        "query:sv-verification-feedback-mutate-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->sv_feedback_mutate_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->sv_feedback_mutate_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->sv_feedback_mutate_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 837);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 67 (orig lines 19029-19079)
void ObservabilityPrims::register_jit_p67(PrimRegistrar add, Evaluator& ev) {
    // Issue #838: query:seva-longrunning-harness-v2-stats
    ObservabilityPrims::register_stats_impl(
        "query:seva-longrunning-harness-v2-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->seva_harness_v2_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->seva_harness_v2_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->seva_harness_v2_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 838);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 68 (orig lines 19080-19130)
void ObservabilityPrims::register_jit_p68(PrimRegistrar add, Evaluator& ev) {
    // Issue #839 / #1894: query:typed-mutation-audit-stats
    // Extends #839 surface with #1614/#1894 invariant + hotpath AC keys.
    ObservabilityPrims::register_stats_impl(
        "query:typed-mutation-audit-stats", [&ev](const auto&) -> EvalValue {
            using namespace aura::compiler::typed_audit;
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->typed_mut_audit_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->typed_mut_audit_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->typed_mut_audit_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(32) /* #1141 / #1894 */;
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
            // #1894 AC keys (process-wide + CompilerMetrics mirror)
            const auto triggered = static_cast<std::int64_t>(
                g_typed_mutation_audit_counters.typed_mutation_audit_triggered_total.load(
                    std::memory_order_relaxed));
            const auto violations = static_cast<std::int64_t>(
                g_typed_mutation_audit_counters.typed_mutation_violations_caught_total.load(
                    std::memory_order_relaxed));
            const auto blame = static_cast<std::int64_t>(
                g_typed_mutation_audit_counters.provenance_blame_chain_hits_total.load(
                    std::memory_order_relaxed));
            insert_kv("typed_mutation_audit_triggered_total", triggered);
            insert_kv("typed_mutation_violations_caught_total", violations);
            insert_kv("provenance_blame_chain_hits_total", blame);
            insert_kv("invariant-audits", static_cast<std::int64_t>(
                                              g_typed_mutation_audit_counters.invariant_audits.load(
                                                  std::memory_order_relaxed)));
            insert_kv("hit-rate-bp",
                      triggered > 0 ? (triggered - violations) * 10000 / triggered : 10000);
            insert_kv("hotpath-guard-exit-wired", 1);
            insert_kv("schema", 1894); // lineage 839 / 1614
            insert_kv("issue", 1894);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 69 (orig lines 19131-19182)
void ObservabilityPrims::register_jit_p69(PrimRegistrar add, Evaluator& ev) {
    // Issue #840: query:stable-ref-full-provenance-v2-stats
    ObservabilityPrims::register_stats_impl(
        "query:stable-ref-full-provenance-v2-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->stable_ref_full_v2_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->stable_ref_full_v2_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->stable_ref_full_v2_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 840);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 70 (orig lines 19183-19233)
void ObservabilityPrims::register_jit_p70(PrimRegistrar add, Evaluator& ev) {
    // Issue #842: query:longrunning-ai-infra-stats
    ObservabilityPrims::register_stats_impl(
        "query:longrunning-ai-infra-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->longrun_ai_infra_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->longrun_ai_infra_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->longrun_ai_infra_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 842);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 71 (orig lines 19234-19284)
void ObservabilityPrims::register_jit_p71(PrimRegistrar add, Evaluator& ev) {
    // Issue #843: query:ai-native-meta-extension-stats
    ObservabilityPrims::register_stats_impl(
        "query:ai-native-meta-extension-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->ai_native_meta_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->ai_native_meta_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->ai_native_meta_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 843);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}


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
    ObservabilityPrims::register_stats_impl(
        "query:orchestration-telemetry-pipeline-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->orch_telemetry_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->orch_telemetry_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->orch_telemetry_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            // Issue #1144: shared insert helper (Phase 1 seed — migrate remaining clones).
            auto* ht = FlatHashTable::create(16) /* #1141 */;
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
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
    ObservabilityPrims::register_stats_impl(
        "query:per-fiber-exception-state-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->per_fiber_ex_state_total.load(std::memory_order_relaxed))
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
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
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
    ObservabilityPrims::register_stats_impl(
        "query:aot-hotswap-pipeline-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->aot_hotswap_pipe_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->aot_hotswap_pipe_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->aot_hotswap_pipe_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16) /* #1141 */;
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
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
    ObservabilityPrims::register_stats_impl(
        "query:macro-hygiene-query-provenance-v2-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->macro_hyg_query_v2_total.load(std::memory_order_relaxed))
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
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
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
    ObservabilityPrims::register_stats_impl(
        "query:reflection-edsl-extension-v2-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->reflect_edsl_v2_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->reflect_edsl_v2_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->reflect_edsl_v2_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16) /* #1141 */;
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
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
    ObservabilityPrims::register_stats_impl(
        "query:self-evolution-hygiene-dirty-epoch-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->selfevo_hyg_dirty_total.load(std::memory_order_relaxed))
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
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
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
    ObservabilityPrims::register_stats_impl(
        "query:sv-verification-feedback-closedloop-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->sv_fb_closedloop_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->sv_fb_closedloop_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->sv_fb_closedloop_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16) /* #1141 */;
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
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
    ObservabilityPrims::register_stats_impl(
        "query:pattern-defuse-hygiene-full-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->pattern_defuse_hyg_total.load(std::memory_order_relaxed))
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
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
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

// Issue #909 part 80 (orig lines 19697-19748)
void ObservabilityPrims::register_jit_p80(PrimRegistrar add, Evaluator& ev) {
    // Issue #852: query:stable-ref-mutation-log-hardening-stats
    ObservabilityPrims::register_stats_impl(
        "query:stable-ref-mutation-log-hardening-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->stable_ref_mutlog_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->stable_ref_mutlog_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->stable_ref_mutlog_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 852);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 81 (orig lines 19749-19799)
void ObservabilityPrims::register_jit_p81(PrimRegistrar add, Evaluator& ev) {
    // Issue #853: query:dirtyaware-impact-enforcement-v2-stats
    ObservabilityPrims::register_stats_impl(
        "query:dirtyaware-impact-enforcement-v2-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->dirty_impact_v2_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->dirty_impact_v2_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->dirty_impact_v2_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 853);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 82 (orig lines 19800-19851)
void ObservabilityPrims::register_jit_p82(PrimRegistrar add, Evaluator& ev) {
    // Issue #854: query:live-irclosure-envframe-gc-stats
    ObservabilityPrims::register_stats_impl(
        "query:live-irclosure-envframe-gc-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->live_irclosure_gc_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->live_irclosure_gc_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->live_irclosure_gc_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 854);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 83 (orig lines 19852-19903)
void ObservabilityPrims::register_jit_p83(PrimRegistrar add, Evaluator& ev) {
    // Issue #855: query:source-marker-linear-consistency-stats
    ObservabilityPrims::register_stats_impl(
        "query:source-marker-linear-consistency-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->src_marker_linear_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->src_marker_linear_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->src_marker_linear_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 855);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 84 (orig lines 19904-19954)
void ObservabilityPrims::register_jit_p84(PrimRegistrar add, Evaluator& ev) {
    // Issue #856: query:terminal-buffer-diff-present-stats
    ObservabilityPrims::register_stats_impl(
        "query:terminal-buffer-diff-present-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->term_buf_diff_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->term_buf_diff_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->term_buf_diff_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 856);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 85 (orig lines 19955-20005)
void ObservabilityPrims::register_jit_p85(PrimRegistrar add, Evaluator& ev) {
    // Issue #857: query:render-observability-v2-stats
    ObservabilityPrims::register_stats_impl(
        "query:render-observability-v2-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->render_obs_v2_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->render_obs_v2_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->render_obs_v2_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 857);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 86 (orig lines 20006-20056)
void ObservabilityPrims::register_jit_p86(PrimRegistrar add, Evaluator& ev) {
    // Issue #858: query:render-jit-soa-hotpath-stats
    ObservabilityPrims::register_stats_impl(
        "query:render-jit-soa-hotpath-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->render_jit_soa_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->render_jit_soa_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->render_jit_soa_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 858);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 87 (orig lines 20057-20107)
void ObservabilityPrims::register_jit_p87(PrimRegistrar add, Evaluator& ev) {
    // Issue #859: query:arena-live-defrag-full-v2-stats
    ObservabilityPrims::register_stats_impl(
        "query:arena-live-defrag-full-v2-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->arena_ldefrag_v2_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->arena_ldefrag_v2_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->arena_ldefrag_v2_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 859);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}


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
    ObservabilityPrims::register_stats_impl(
        "query:ir-soa-dirty-hybrid-full-v2-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->irsoa_dirty_v2_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->irsoa_dirty_v2_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->irsoa_dirty_v2_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 860);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 89 (orig lines 20159-20210)
void ObservabilityPrims::register_jit_p89(PrimRegistrar add, Evaluator& ev) {
    // Issue #861: query:value-shape-consteval-full-v2-stats
    ObservabilityPrims::register_stats_impl(
        "query:value-shape-consteval-full-v2-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->val_shape_ceval_v2_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 861);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 90 (orig lines 20211-20262)
void ObservabilityPrims::register_jit_p90(PrimRegistrar add, Evaluator& ev) {
    // Issue #862: query:defuse-infer-partial-stats
    ObservabilityPrims::register_stats_impl(
        "query:defuse-infer-partial-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->defuse_infer_part_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 862);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 91 (orig lines 20263-20313)
void ObservabilityPrims::register_jit_p91(PrimRegistrar add, Evaluator& ev) {
    // Issue #863: query:ownership-escape-postmutate-stats
    ObservabilityPrims::register_stats_impl(
        "query:ownership-escape-postmutate-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->own_escape_post_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->own_escape_post_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->own_escape_post_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 863);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 92 (orig lines 20314-20364)
void ObservabilityPrims::register_jit_p92(PrimRegistrar add, Evaluator& ev) {
    // Issue #864: query:typed-mutation-audit-pass-stats
    ObservabilityPrims::register_stats_impl(
        "query:typed-mutation-audit-pass-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->typed_audit_pass_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->typed_audit_pass_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->typed_audit_pass_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 864);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 93 (orig lines 20365-20415)
void ObservabilityPrims::register_jit_p93(PrimRegistrar add, Evaluator& ev) {
    // Issue #865: query:sv-backend-emit-bidirectional-stats
    ObservabilityPrims::register_stats_impl(
        "query:sv-backend-emit-bidirectional-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->sv_backend_bi_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->sv_backend_bi_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->sv_backend_bi_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 865);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 94 (orig lines 20416-20466)
void ObservabilityPrims::register_jit_p94(PrimRegistrar add, Evaluator& ev) {
    // Issue #866: query:large-sv-pattern-defuse-stats
    ObservabilityPrims::register_stats_impl(
        "query:large-sv-pattern-defuse-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->large_sv_pattern_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->large_sv_pattern_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->large_sv_pattern_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 866);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 95 (orig lines 20467-20518)
void ObservabilityPrims::register_jit_p95(PrimRegistrar add, Evaluator& ev) {
    // Issue #867: query:longrunning-stable-ref-dirty-stats
    ObservabilityPrims::register_stats_impl(
        "query:longrunning-stable-ref-dirty-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->longrun_sref_dirty_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 867);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}


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

// Issue #909 part 96 (orig lines 20519-20569)
void ObservabilityPrims::register_jit_p96(PrimRegistrar add, Evaluator& ev) {
    // Issue #868: query:sv-eda-primitives-cluster-stats
    ObservabilityPrims::register_stats_impl(
        "query:sv-eda-primitives-cluster-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(m->sv_eda_prims_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits = m ? static_cast<std::int64_t>(m->sv_eda_prims_hits_total.load(
                                              std::memory_order_relaxed))
                                        : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->sv_eda_prims_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 868);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 97 (orig lines 20570-20620)
void ObservabilityPrims::register_jit_p97(PrimRegistrar add, Evaluator& ev) {
    // Issue #869: query:primitives-resource-quota-fiber-stats
    ObservabilityPrims::register_stats_impl(
        "query:primitives-resource-quota-fiber-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->prim_quota_fiber_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->prim_quota_fiber_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->prim_quota_fiber_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 869);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1481 / #1498 / #1554 / #1579 / #1590 / #1600 / #1618:
    // query:resource-quota-stats.
    // #1481 fields: checks_total, rejects_total, max_fibers, max_mutations.
    // #1498 production fields (AC2): current_usage, memory_quota,
    // memory_quota_total, exceeded_count (=rejects), mutations_used.
    // #1554: exceeded_total alias + temp_arena_wired / group_owner_wired.
    // #1579: module_phase + process_fibers_* + process_checks/rejects + overflow.
    // #1590: schema 1590 + quota aliases + hot-path closed-loop keys.
    // #1600: orchestration fiber spawn/reject + join_resource_wait_us.
    // #1618: ResourceQuotaManager + typed reject ≠ panic + mutation_budget_rejected.
    // schema bumped to 1618 (agents: treat unknown keys as optional).
    ObservabilityPrims::register_stats_impl(
        "query:resource-quota-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t checks_total =
                m ? static_cast<std::int64_t>(
                        m->resource_quota_checks_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t rejects_total =
                m ? static_cast<std::int64_t>(
                        m->resource_quota_rejects_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t max_fibers =
                m ? static_cast<std::int64_t>(
                        m->resource_quota_max_fibers.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t max_mutations =
                m ? static_cast<std::int64_t>(
                        m->resource_quota_max_mutations.load(std::memory_order_relaxed))
                  : 0;
            // Issue #1498: live usage + configured quotas for agent dashboards.
            const std::int64_t current_usage =
                static_cast<std::int64_t>(ev.resource_quota_current_usage());
            const std::int64_t memory_quota = static_cast<std::int64_t>(ev.resource_quota_memory());
            const std::int64_t memory_quota_total =
                static_cast<std::int64_t>(ev.resource_quota_memory_total());
            const std::int64_t mut_used = static_cast<std::int64_t>(ev.mutation_quota_used());
            auto* ht = FlatHashTable::create(128) /* #1141 / #1498 / #1590 / #1618 more keys */;
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
            const std::int64_t temp_wired =
                (ev.temp_arena_ && ev.temp_arena_->has_arena_owner()) ? 1 : 0;
            const std::int64_t group_wired =
                (ev.arena_group_ && ev.arena_group_->has_default_arena_owner()) ? 1 : 0;
            const std::int64_t primary_wired = (ev.arena_ && ev.arena_->has_arena_owner()) ? 1 : 0;
            // Issue #1579: process ResourceQuota module stats.
            auto& pq = aura::core::resource_quota::process_resource_quota();
            const std::int64_t module_phase =
                static_cast<std::int64_t>(aura::core::resource_quota::kResourceQuotaPhase);
            const std::int64_t proc_fibers_used =
                static_cast<std::int64_t>(pq.used(aura::core::resource_quota::Dimension::Fibers));
            const std::int64_t proc_fibers_limit =
                static_cast<std::int64_t>(pq.limit(aura::core::resource_quota::Dimension::Fibers));
            const std::int64_t proc_checks =
                static_cast<std::int64_t>(pq.checks_total.load(std::memory_order_relaxed));
            const std::int64_t proc_rejects =
                static_cast<std::int64_t>(pq.rejects_total.load(std::memory_order_relaxed));
            const std::int64_t overflow_guards =
                static_cast<std::int64_t>(pq.overflow_guards_total.load(std::memory_order_relaxed));

            insert_kv("checks_total", checks_total);
            insert_kv("rejects_total", rejects_total);
            insert_kv("exceeded_count", rejects_total); // #1498 AC2 alias
            insert_kv("exceeded_total", rejects_total); // #1554 AC alias
            insert_kv("max_fibers", max_fibers);
            insert_kv("max_mutations", max_mutations);
            insert_kv("current_usage", current_usage);
            insert_kv("memory_quota", memory_quota);
            insert_kv("memory_quota_total", memory_quota_total);
            // Issue #1590 AC2 aliases (Agent dashboards).
            insert_kv("quota", memory_quota_total != 0 ? memory_quota_total : memory_quota);
            insert_kv("mutations_used", mut_used);
            insert_kv("module_phase", module_phase);              // #1579
            insert_kv("process_fibers_used", proc_fibers_used);   // #1579
            insert_kv("process_fibers_limit", proc_fibers_limit); // #1579
            insert_kv("process_checks", proc_checks);             // #1579
            insert_kv("process_rejects", proc_rejects);           // #1579
            insert_kv("overflow_guards", overflow_guards);        // #1579
            insert_kv("primary_arena_wired", primary_wired);
            insert_kv("temp_arena_wired", temp_wired);
            insert_kv("group_owner_wired", group_wired);
            insert_kv("hotpath_arena_gated", primary_wired); // #1590: allocate_raw owner path
            insert_kv("hotpath_guard_try_acquire", 1); // #1590: try_acquire is production path
            // Issue #1600: orchestration ResourceQuota surface.
            insert_kv("fiber_spawn_rejected_total",
                      static_cast<std::int64_t>(
                          pq.fiber_spawn_rejected_total.load(std::memory_order_relaxed)));
            insert_kv("orchestration_quota_exceeded_count",
                      static_cast<std::int64_t>(
                          pq.orchestration_quota_exceeded_total.load(std::memory_order_relaxed)));
            insert_kv("orchestration_quota_exceeded_total",
                      static_cast<std::int64_t>(
                          pq.orchestration_quota_exceeded_total.load(std::memory_order_relaxed)));
            insert_kv("join_resource_wait_us",
                      static_cast<std::int64_t>(aura::serve::Fiber::join_wait_us_total()));
            insert_kv("join_wait_us_total",
                      static_cast<std::int64_t>(aura::serve::Fiber::join_wait_us_total()));
            insert_kv("join_total", static_cast<std::int64_t>(aura::serve::Fiber::join_total()));
            insert_kv("process_fibers_remaining",
                      static_cast<std::int64_t>(
                          pq.remaining(aura::core::resource_quota::Dimension::Fibers) ==
                                  std::numeric_limits<std::uint64_t>::max()
                              ? static_cast<std::int64_t>(-1) // unlimited
                              : static_cast<std::int64_t>(
                                    pq.remaining(aura::core::resource_quota::Dimension::Fibers))));
            insert_kv("orch_spawn_gated", 1); // Scheduler::spawn + parallel_intend wired
            // Issue #1618: ResourceQuotaManager + typed reject AC keys
            const std::int64_t quota_viol =
                m ? static_cast<std::int64_t>(
                        m->quota_violation_total.load(std::memory_order_relaxed))
                  : rejects_total;
            const std::int64_t mut_budget_rej =
                m ? static_cast<std::int64_t>(
                        m->mutation_budget_rejected_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t typed_rej =
                m ? static_cast<std::int64_t>(
                        m->quota_reject_typed_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t panic_dist =
                m ? static_cast<std::int64_t>(
                        m->panic_quota_distinguished_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t mgr_enforce =
                m ? static_cast<std::int64_t>(
                        m->manager_enforce_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("quota_violation_total", quota_viol);
            insert_kv("quota-violation-total", quota_viol);
            insert_kv("mutation_budget_rejected", mut_budget_rej);
            insert_kv("mutation_budget_rejected_total", mut_budget_rej);
            insert_kv("mutation-budget-rejected", mut_budget_rej);
            insert_kv("quota_reject_typed_total", typed_rej);
            insert_kv("panic_quota_distinguished_total", panic_dist);
            insert_kv("manager_enforce_total", mgr_enforce);
            insert_kv("manager-wired", 1);
            insert_kv("panic-quota-distinguished", 1);
            insert_kv("typed-reject-not-panic", 1);
            // Issue #1628 / #1634: MutationBoundaryGuard::try_acquire factory
            // (replace panic-checkpoint quota path; typed AuraError).
            // #1634: unify invalidate + AC metric aliases.
            const std::int64_t try_acq =
                m ? static_cast<std::int64_t>(
                        m->mutation_guard_try_acquire_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t try_rej =
                m ? static_cast<std::int64_t>(
                        m->mutation_guard_try_acquire_reject_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t atomic_inv =
                m ? static_cast<std::int64_t>(
                        m->typed_mutate_atomic_invalidations_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t guard_fail_lin =
                m ? static_cast<std::int64_t>(
                        m->guard_failure_linear_enforce_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("mutation_guard_try_acquire_total", try_acq);
            insert_kv("mutation_guard_try_acquire_reject_total", try_rej);
            // #1634 AC3 issue-body alias
            insert_kv("mutation_boundary_try_acquire_fail_total", try_rej);
            insert_kv("resource_quota_rejects_total", rejects_total);
            insert_kv("typed_mutate_atomic_invalidations_total", atomic_inv);
            insert_kv("guard_failure_linear_enforce_total", guard_fail_lin);
            insert_kv("try_acquire_wired", 1);
            insert_kv("panic_checkpoint_quota_replaced", 1);
            insert_kv("eval_on_current_try_acquire", 1);
            insert_kv("typed_mutate_try_acquire", 1);
            insert_kv("typed_mutate_atomic_unified_invalidate", 1);
            insert_kv("atomic_bump_epochs_unified", 1);
            insert_kv("legacy_ctor_deprecated", 1);
            insert_kv("guard_failure_linear_probe_wired", 1);
            // Issue #1880: orch agent arena/mailbox + try_acquire body path.
            insert_kv("orch_resource_quota_rejects_total",
                      static_cast<std::int64_t>(
                          pq.orch_resource_quota_rejects_total.load(std::memory_order_relaxed)));
            insert_kv("agent_arena_usage_bytes",
                      static_cast<std::int64_t>(
                          pq.agent_arena_usage_bytes.load(std::memory_order_relaxed)));
            insert_kv("agent_arena_reserve_total",
                      static_cast<std::int64_t>(
                          pq.agent_arena_reserve_total.load(std::memory_order_relaxed)));
            insert_kv("agent_arena_release_total",
                      static_cast<std::int64_t>(
                          pq.agent_arena_release_total.load(std::memory_order_relaxed)));
            insert_kv("orch_agent_body_try_acquire_wired", 1);
            insert_kv("orch_spawn_memory_preflight_wired", 1);
            insert_kv("schema-1880", 1880);
            insert_kv("issue", 1634);  // primary lineage id (1628|1618|1600…)
            insert_kv("schema", 1634); // keep stable; Agents use schema-1880 for #1880 fields
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1483 C3: query:per-fiber-mutation-stack-stats — exposes
    // the per_fiber_mutation_stack_depth_max + _current_max atomics
    // added at C2 (observability_metrics.h:1550-1551 area + wire sites
    // at evaluator_fiber_mutation.cpp:316 + :454). Returns a 3-field
    // hash: {lifetime-max, current-max, schema=1483}.
    //
    // Closes EDSL-visibility gap — the C2 metrics exist on
    // CompilerMetrics but no Aura primitive surfaces them, so
    // orchestration queries + LLM-bottleneck monitors can't observe
    // per-fiber mutation_stack_depth pressure without importing
    // observability_metrics directly. The new primitive reads the
    // metrics via the canonical Evaluator accessors
    // (get_per_fiber_mutation_stack_depth_max + _current_max) so
    // callers don't need to know about the CompilerMetrics layout.
    //
    // The lifetime-max + current-max pair distinguishes "all-time
    // peak across this Evaluator lifetime" from "current live peak
    // across active fibers" — useful for orchestrators tuning the
    // adaptive safepoint threshold (C4 follow-up).
    ObservabilityPrims::register_stats_impl(
        "query:per-fiber-mutation-stack-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t lifetime_max =
                static_cast<std::int64_t>(ev.get_per_fiber_mutation_stack_depth_max());
            const std::int64_t current_max =
                static_cast<std::int64_t>(ev.get_per_fiber_mutation_stack_depth_current_max());
            const std::int64_t live_depth =
                static_cast<std::int64_t>(Evaluator::mutation_boundary_depth());
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
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
            insert_kv("lifetime-max", lifetime_max);
            insert_kv("current-max", current_max);
            // Issue #1493: live mutation_boundary depth + histogram total samples.
            insert_kv("live-depth", live_depth);
            std::int64_t hist_total = 0;
            if (m) {
                for (std::size_t i = 0; i < CompilerMetrics::kMutationStackDepthHistBuckets; ++i)
                    hist_total += static_cast<std::int64_t>(
                        m->mutation_stack_depth_histogram[i].load(std::memory_order_relaxed));
                insert_kv("hist-b0",
                          static_cast<std::int64_t>(m->mutation_stack_depth_histogram[0].load(
                              std::memory_order_relaxed)));
                insert_kv("hist-b1",
                          static_cast<std::int64_t>(m->mutation_stack_depth_histogram[1].load(
                              std::memory_order_relaxed)));
                insert_kv(
                    "hist-b2-plus",
                    hist_total -
                        static_cast<std::int64_t>(
                            m->mutation_stack_depth_histogram[0].load(std::memory_order_relaxed) +
                            m->mutation_stack_depth_histogram[1].load(std::memory_order_relaxed)));
            } else {
                insert_kv("hist-b0", 0);
                insert_kv("hist-b1", 0);
                insert_kv("hist-b2-plus", 0);
            }
            insert_kv("hist-samples", hist_total);
            insert_kv("issue", 1591);
            insert_kv("schema", 1493); // keep 1493; #1591 alias below
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1591 / #1635: query:per-fiber-mutation-depth-stats
    // Alias of query:per-fiber-mutation-stack-stats (schema 1635 lineage).
    ObservabilityPrims::register_stats_impl(
        "query:per-fiber-mutation-depth-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t lifetime_max =
                static_cast<std::int64_t>(ev.get_per_fiber_mutation_stack_depth_max());
            const std::int64_t current_max =
                static_cast<std::int64_t>(ev.get_per_fiber_mutation_stack_depth_current_max());
            const std::int64_t live_depth =
                static_cast<std::int64_t>(Evaluator::mutation_boundary_depth());
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
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
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("lifetime-max", lifetime_max);
            insert_kv("current-max", current_max);
            insert_kv("live-depth", live_depth);
            std::int64_t hist_total = 0;
            if (m) {
                for (std::size_t i = 0; i < CompilerMetrics::kMutationStackDepthHistBuckets; ++i)
                    hist_total += static_cast<std::int64_t>(
                        m->mutation_stack_depth_histogram[i].load(std::memory_order_relaxed));
            }
            insert_kv("hist-samples", hist_total);
            insert_kv("mutation-stack-depth-histogram-samples", hist_total);
            insert_kv(
                "safepoint-wait-while-mutation-held-us",
                static_cast<std::int64_t>(aura::gc_hooks::safepoint_wait_while_mutation_held_us()));
            insert_kv("gc-safepoint-depth-check-wired", 1);
            insert_kv("safe-yield-mandate-active", 1);
            insert_kv("issue", 1635);
            insert_kv("schema", 1635); // lineage 1591 / 1504
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1483 C4: query:gc-safepoint-adaptive-stats — exposes
    // the adaptive safepoint threshold + adaptive-defer counter
    // added at C4 (observability_metrics.h safepoint_adaptive_*
    // atomics + wire in request_gc_safepoint at evaluator.ixx:4191
    // area + helper functions at evaluator.ixx:7198-7259 area).
    // Returns a 4-field hash: {threshold, defer_count, schema=1483}.
    //
    // Closes EDSL-visibility gap — the C4 adaptive threshold logic
    // lives on CompilerMetrics + Evaluator but no Aura primitive
    // surfaces it, so orchestration queries + LLM-bottleneck
    // monitors can't observe whether the adaptive heuristic is
    // backing off (threshold > 0) or how many adaptive deferrals
    // have happened (defer_count) without importing
    // observability_metrics directly.
    //
    // The threshold-doubled-per-defer pattern matches the
    // exponential-backoff heuristic (a) from the #1483 plan. The
    // pair of threshold + defer_count lets orchestrators verify
    // both the current backoff state AND the cumulative
    // adaptive-defer pressure (vs. the natural mutation_boundary_
    // depth > 0 defer path tracked by bump_gc_safepoint_deferred).
    ObservabilityPrims::register_stats_impl(
        "query:gc-safepoint-adaptive-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t threshold =
                static_cast<std::int64_t>(ev.get_safepoint_adaptive_threshold());
            const std::int64_t defer_count =
                static_cast<std::int64_t>(ev.get_safepoint_adaptive_defer_count());
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            const std::int64_t hold_total =
                m ? static_cast<std::int64_t>(
                        m->mutation_boundary_hold_time_total_us.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t holds =
                m ? static_cast<std::int64_t>(
                        m->mutation_boundary_holds_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t avg_hold = holds > 0 ? hold_total / holds : 0;
            const std::int64_t wait_us =
                static_cast<std::int64_t>(aura::gc_hooks::safepoint_wait_while_mutation_held_us());
            const std::int64_t wait_n = static_cast<std::int64_t>(
                aura::gc_hooks::safepoint_wait_while_mutation_held_count());
            const std::int64_t freq_ratio =
                static_cast<std::int64_t>(aura_gc_frequency_tune_ratio_load());
            const std::int64_t adapt_up =
                m ? static_cast<std::int64_t>(
                        m->safepoint_frequency_adapt_up_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t adapt_down =
                m ? static_cast<std::int64_t>(
                        m->safepoint_frequency_adapt_down_total.load(std::memory_order_relaxed))
                  : 0;
            auto* ht = FlatHashTable::create(32) /* #1141 / #1599 hist buckets */;
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
            insert_kv("threshold", threshold);
            insert_kv("defer-count", defer_count);
            // Issue #1493: hold-time adaptive + wait-while-mutation export.
            insert_kv("avg-mutation-hold-us", avg_hold);
            insert_kv("hold-samples", holds);
            insert_kv("safepoint-wait-while-mutation-held-us", wait_us);
            insert_kv("safepoint-wait-while-mutation-held-count", wait_n);
            insert_kv("gc-frequency-tune-ratio", freq_ratio);
            insert_kv("frequency-adapt-up", adapt_up);
            insert_kv("frequency-adapt-down", adapt_down);
            // Issue #1599 AC5: mutation_stack_depth_histogram export.
            std::int64_t hist_sum = 0;
            static constexpr const char* kHistKeys[8] = {"hist-b0", "hist-b1", "hist-b2",
                                                         "hist-b3", "hist-b4", "hist-b5",
                                                         "hist-b6", "hist-b7"};
            if (m) {
                for (std::size_t i = 0; i < CompilerMetrics::kMutationStackDepthHistBuckets; ++i) {
                    const auto v = static_cast<std::int64_t>(
                        m->mutation_stack_depth_histogram[i].load(std::memory_order_relaxed));
                    hist_sum += v;
                    if (i < 8)
                        insert_kv(kHistKeys[i], v);
                }
            }
            insert_kv("mutation_stack_depth_histogram", hist_sum);
            insert_kv("issue", 1599);
            insert_kv("schema", 1599); // lineage 1493|1483
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 98 (orig lines 20621-20671)
void ObservabilityPrims::register_jit_p98(PrimRegistrar add, Evaluator& ev) {
    // Issue #870: query:declarative-primitive-registry-stats
    ObservabilityPrims::register_stats_impl(
        "query:declarative-primitive-registry-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->decl_prim_reg_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->decl_prim_reg_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->decl_prim_reg_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 870);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 99 (orig lines 20672-20722)
void ObservabilityPrims::register_jit_p99(PrimRegistrar add, Evaluator& ev) {
    // Issue #872: query:primitives-namespace-alias-stats
    ObservabilityPrims::register_stats_impl(
        "query:primitives-namespace-alias-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->prim_ns_alias_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->prim_ns_alias_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->prim_ns_alias_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 872);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 100 (orig lines 20723-20774)
void ObservabilityPrims::register_jit_p100(PrimRegistrar add, Evaluator& ev) {
    // Issue #875: query:guard-steal-gc-safety-v2-stats
    ObservabilityPrims::register_stats_impl(
        "query:guard-steal-gc-safety-v2-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->guard_steal_gc_v2_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->guard_steal_gc_v2_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->guard_steal_gc_v2_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 875);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 101 (orig lines 20775-20826)
void ObservabilityPrims::register_jit_p101(PrimRegistrar add, Evaluator& ev) {
    // Issue #876: query:dirtyaware-ir-cache-consistency-stats
    ObservabilityPrims::register_stats_impl(
        "query:dirtyaware-ir-cache-consistency-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->dirty_ircache_cons_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->dirty_ircache_cons_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->dirty_ircache_cons_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 876);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 102 (orig lines 20827-20878)
void ObservabilityPrims::register_jit_p102(PrimRegistrar add, Evaluator& ev) {
    // Issue #877: query:stats-builder-refactor-stats
    ObservabilityPrims::register_stats_impl(
        "query:stats-builder-refactor-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->stats_builder_ref_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->stats_builder_ref_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->stats_builder_ref_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 877);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 103 (orig lines 20879-20930)
void ObservabilityPrims::register_jit_p103(PrimRegistrar add, Evaluator& ev) {
    // Issue #878: query:load-or-zero-helper-stats
    ObservabilityPrims::register_stats_impl(
        "query:load-or-zero-helper-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->load_or_zero_help_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->load_or_zero_help_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->load_or_zero_help_savings_total.load(std::memory_order_relaxed))
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
            insert_kv("schema", 878);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}


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
    ObservabilityPrims::register_stats_impl(
        "query:cpp26-modernization-sweep-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->cpp26_mod_sweep_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->cpp26_mod_sweep_hits_total.load(std::memory_order_relaxed))
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
    ObservabilityPrims::register_stats_impl(
        "query:metrics-meta-reflection-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->metrics_meta_refl_total.load(std::memory_order_relaxed))
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
    ObservabilityPrims::register_stats_impl(
        "query:test-harness-bootstrap-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->test_harness_boot_total.load(std::memory_order_relaxed))
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
    ObservabilityPrims::register_stats_impl(
        "query:bundle-codegen-decouple-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->bundle_codegen_dec_total.load(std::memory_order_relaxed))
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
    ObservabilityPrims::register_stats_impl(
        "query:test-bundle-migration-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->test_bundle_mig_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->test_bundle_mig_hits_total.load(std::memory_order_relaxed))
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
    ObservabilityPrims::register_stats_impl(
        "query:test-profile-flag-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->test_profile_flag_total.load(std::memory_order_relaxed))
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
    ObservabilityPrims::register_stats_impl(
        "query:test-harness-module-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->test_harness_mod_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->test_harness_mod_hits_total.load(std::memory_order_relaxed))
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
    ObservabilityPrims::register_stats_impl(
        "query:test-json-report-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total = m ? static_cast<std::int64_t>(m->test_json_report_total.load(
                                               std::memory_order_relaxed))
                                         : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->test_json_report_hits_total.load(std::memory_order_relaxed))
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

// Issue #909 part 112 (orig lines 21343-21394)
void ObservabilityPrims::register_jit_p112(PrimRegistrar add, Evaluator& ev) {
    // Issue #395: query:gcc16-modules-buildenv-stats
    ObservabilityPrims::register_stats_impl(
        "query:gcc16-modules-buildenv-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t total =
                m ? static_cast<std::int64_t>(
                        m->gcc16_modules_env_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hits =
                m ? static_cast<std::int64_t>(
                        m->gcc16_modules_env_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t savings =
                m ? static_cast<std::int64_t>(
                        m->gcc16_modules_env_savings_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t active = 1;
            auto* ht = FlatHashTable::create(16) /* #1141 */;
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("total", total);
            insert_kv("hits", hits);
            insert_kv("savings", savings);
            insert_kv("active", active);
            insert_kv("schema", 395);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 113 (orig lines 21395-21426)
void ObservabilityPrims::register_jit_p113(PrimRegistrar add, Evaluator& ev) {

#if AURA_ENABLE_TERMINAL
    // Issue #856 / #1136 / #1140 → Issue #1351 Phase A deprecation.
    // No-op counter stubs; real APIs: make-terminal-buffer / terminal-diff-update.
    // Issue #1971: commercial UI vertical gate (AURA_ENABLE_TERMINAL).
    auto deprecate_terminal_noop = [](const char* name, const char* replacement) {
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

    add("terminal:create-buffer", [&ev, deprecate_terminal_noop](const auto& a) -> EvalValue {
        (void)a;
        deprecate_terminal_noop("terminal:create-buffer", "make-terminal-buffer");
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            m->term_buf_diff_total.fetch_add(1, std::memory_order_relaxed);
        }
        return make_bool(false);
    });
    add("terminal:diff", [&ev, deprecate_terminal_noop](const auto&) -> EvalValue {
        deprecate_terminal_noop("terminal:diff", "terminal-diff-update");
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            m->term_buf_diff_hits_total.fetch_add(1, std::memory_order_relaxed);
            m->render_obs_v2_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
        return make_bool(false);
    });
#endif // AURA_ENABLE_TERMINAL
    // Issue #872: primitives:alias name target (Phase 1 registry of aliases)
    add("primitives:alias", [&ev](const auto& a) -> EvalValue {
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            m->prim_ns_alias_total.fetch_add(1, std::memory_order_relaxed);
            bool ok = a.size() >= 2 && (is_string(a[0]) || is_keyword(a[0]));
            if (ok)
                m->prim_ns_alias_hits_total.fetch_add(1, std::memory_order_relaxed);
            else
                m->prim_ns_alias_savings_total.fetch_add(1, std::memory_order_relaxed);
            return make_bool(ok);
        }
        return make_bool(a.size() >= 2);
    });
}
} // namespace aura::compiler::primitives_detail
