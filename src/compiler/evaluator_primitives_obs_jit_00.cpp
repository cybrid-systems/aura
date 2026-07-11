// evaluator_primitives_obs_jit_00.cpp — Issue #909: peeled domain registration from observability
// monolith aura.compiler.evaluator module partition.

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

// Issue #909 part 0 (orig lines 11704-11758)
void ObservabilityPrims::register_jit_p0(PrimRegistrar add, Evaluator& ev) {

    // (jit:intrinsic-count) — Issue #194: return the
    // runtime→intrinsic migration counter from the AuraJIT.
    // This is the per-commit observability signal for the 4
    // candidates the issue body tracks. Returns 0 if no hook
    // is installed (e.g. unit-test Evaluator without a JIT).
    add("jit:intrinsic-count", [&ev](const auto&) -> EvalValue {
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
    add("jit:exception-depth", [&ev](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(aura_exception_depth()));
    });

    // (jit:exception-fibers) — Issue #195: number of distinct
    // fiber ids that have exception state. Used for
    // observability of the per-fiber ExStack map size.
    add("jit:exception-fibers", [&ev](const auto&) -> EvalValue {
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
        if (ev.sandbox_mode() &&
            !ev.has_capability(aura::compiler::security::kCapExceptionControl) &&
            !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->capability_exception_control_denials.fetch_add(1, std::memory_order_relaxed);
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
    add("query:jit-stats", [&ev](const auto&) -> EvalValue {
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
    add("query:jit-stats-hash", [&ev](const auto&) -> EvalValue {
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
            hotswap_invalidate = m->jit_hotswap_invalidate_total.load(std::memory_order_relaxed);
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
    add("query:jit-hotswap-closure-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t refreshed = 0;
        std::uint64_t forced_deopt = 0;
        std::uint64_t mismatch_prevented = 0;
        std::uint64_t hotswap_invalidate = 0;
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            refreshed = m->jit_hotswap_live_closure_refreshed_total.load(std::memory_order_relaxed);
            forced_deopt = m->jit_hotswap_forced_deopt_total.load(std::memory_order_relaxed);
            mismatch_prevented =
                m->jit_hotswap_epoch_mismatch_prevented_total.load(std::memory_order_relaxed);
            hotswap_invalidate = m->jit_hotswap_invalidate_total.load(std::memory_order_relaxed);
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
        insert_kv("epoch-mismatch-prevented-total", static_cast<std::int64_t>(mismatch_prevented));
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
    add("query:hotpath-bottleneck-stats", [&ev](const auto&) -> EvalValue {
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
        const std::uint64_t soa_wired = m ? m->irsoa_wired_hits.load(std::memory_order_relaxed) : 0;
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
    add("query:pass-pipeline-stats", [&ev](const auto&) -> EvalValue {
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
        const std::uint64_t pipeline_total = pipeline_yield + passes_skip_dirty + passes_skip_type +
                                             relower_skip + relower_per_fn + mod_skip;
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
    add("query:pass-pipeline-dirtyaware-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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
        const std::uint64_t total = pipeline_runs + pipeline_yield + passes_skipped_due_to_dirty +
                                    relower_skip + relower_per_fn + mod_skip + block_dirty_hits +
                                    wrap_delegation;
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
            {"passes-skipped-type-dirty", make_int(static_cast<std::int64_t>(passes_skip_type))},
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
    add("query:soa-dirty-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
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
            {"dirty-instruction-pct", make_int(static_cast<std::int64_t>(s.dirty_instruction_pct))},
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
    add("query:arena-compaction-stats-hash", [&ev](const auto&) -> EvalValue {
        // Reuse the same build_hash pattern as the
        // closure:stats / soa-dirty-stats primitives.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
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

} // namespace aura::compiler::primitives_detail
