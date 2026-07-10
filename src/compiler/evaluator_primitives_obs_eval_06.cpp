// evaluator_primitives_obs_eval_06.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 48 (orig lines 6285-6412)
void ObservabilityPrims::register_eval_p48(PrimRegistrar add, Evaluator& ev) {

    // Issue #726: (query:closed-loop-reliability-stats) —
    // verification feedback-driven closed-loop self-evolution
    // reliability counters (non-duplicative with the existing
    // #748 SV verification structure stats primitive which
    // covers structural mutate + emit + dirty re-emit; #726
    // covers the closed-loop reliability side: ref drift
    // prevention + rollback success + feedback mutate rounds).
    //
    // Fields (3 + sentinel):
    //   - ref-drift-prevented        closed_loop_ref_drift_prevented_total
    //                                (# of times a StableNodeRef
    //                                 drift across verification
    //                                 feedback mutate was
    //                                 prevented by the runtime
    //                                 guard — proxy for "how
    //                                 many silent ref
    //                                 invalidations the guard
    //                                 caught")
    //   - rollback-success           closed_loop_rollback_success_total
    //                                (# of successful rollbacks
    //                                 on verification feedback
    //                                 mutate — MutationBoundary
    //                                 Guard dtor + panic
    //                                 restore + epoch bump
    //                                 fired cleanly)
    //   - feedback-mutate-rounds     closed_loop_feedback_mutate_rounds_total
    //                                (# of feedback parse ->
    //                                 mutate -> re-verify
    //                                 rounds completed in the
    //                                 closed loop — proxy for
    //                                 "how many autonomous
    //                                 SEVA iterations the
    //                                 agent ran successfully")
    //   - schema == 726
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual verify:parse-coverage-feedback / parse-assert-
    // failure / parse-formal-cex / mutate:from-verification-
    // feedback primitives + closed-loop controller (seva:run-
    // closed-loop) + enhanced subtree StableNodeRef validation
    // in MutationBoundaryGuard + backend re-emit tie-in (#725)
    // are follow-up work (each is a dedicated session in
    // evaluator_primitives_verify*.cpp or new verify_primitives
    // module + MutationBoundaryGuard + ast dirty + new test
    // harness + SEVA demo extension + docs).
    //
    // Issue #726: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=726 + category=general
    // + arity=0 + pure=true (same pattern as #712-#723).
    ev.primitives_.add(
        "query:closed-loop-reliability-stats",
        [&ev](const auto&) -> EvalValue {
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
            const std::int64_t ref_drift_prevented =
                m ? static_cast<std::int64_t>(
                        m->closed_loop_ref_drift_prevented_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t rollback_success =
                m ? static_cast<std::int64_t>(
                        m->closed_loop_rollback_success_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t feedback_mutate_rounds =
                m ? static_cast<std::int64_t>(
                        m->closed_loop_feedback_mutate_rounds_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"ref-drift-prevented", make_int(ref_drift_prevented)},
                {"rollback-success", make_int(rollback_success)},
                {"feedback-mutate-rounds", make_int(feedback_mutate_rounds)},
                {"schema", make_int(726)},
            };
            return build_hash(kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .doc = "Verification feedback-driven closed-loop self-evolution "
                        "reliability counters: ref drift prevented by the runtime "
                        "guard, successful rollbacks on verification feedback mutate, "
                        "and feedback parse -> mutate -> re-verify rounds completed. "
                        "Pairs with the existing #748 SV verification structure "
                        "stats (structural mutate + emit + dirty re-emit); #726 covers "
                        "the closed-loop reliability side as separate counters the "
                        "Agent can consume to monitor SEVA self-evolution stability.",
                 .category = "general",
                 .schema = "() -> hash"});
}

// Issue #909 part 49 (orig lines 6413-6485)
void ObservabilityPrims::register_eval_p49(PrimRegistrar add, Evaluator& ev) {

    // Issue #655: query:edsl-core-stability-stats — 5 EDSL core gaps for
    // Workspace/Query/Mutate + StableNodeRef/COW/atomic under AI multi-round
    // editing (non-duplicative with #527 stable-ref-cow, #552 edsl-stability,
    // #622 atomic-batch, #654 macro-hygiene-fiber-panic).
    //
    // Fields (5 + sentinel):
    //   - cow-stable-ref-remaps       edsl_cow_stable_ref_remap_total
    //   - tag-arity-delta-patches     edsl_tag_arity_delta_patch_total
    //   - nested-atomic-rollbacks     edsl_nested_atomic_rollback_total
    //   - children-safe-views         FlatAST children_safe_view_count_
    //   - mutate-invalidate-precision edsl_mutate_invalidate_precision_total
    //   - schema == 655
    add("query:edsl-core-stability-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t cow_remap =
            m ? static_cast<std::int64_t>(
                    m->edsl_cow_stable_ref_remap_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t delta_patch =
            m ? static_cast<std::int64_t>(
                    m->edsl_tag_arity_delta_patch_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t nested_rollback =
            m ? static_cast<std::int64_t>(
                    m->edsl_nested_atomic_rollback_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t children_safe =
            ev.workspace_flat()
                ? static_cast<std::int64_t>(ev.workspace_flat()->children_safe_view_count())
                : 0;
        const std::int64_t invalidate_precision =
            m ? static_cast<std::int64_t>(
                    m->edsl_mutate_invalidate_precision_total.load(std::memory_order_relaxed))
              : 0;
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
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("cow-stable-ref-remaps", cow_remap);
        insert_kv("tag-arity-delta-patches", delta_patch);
        insert_kv("nested-atomic-rollbacks", nested_rollback);
        insert_kv("children-safe-views", children_safe);
        insert_kv("mutate-invalidate-precision", invalidate_precision);
        insert_kv("schema", 655);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 50 (orig lines 6486-6567)
void ObservabilityPrims::register_eval_p50(PrimRegistrar add, Evaluator& ev) {

    // Issue #657: query:compiler-core-incremental-stats — 5 compiler pipeline
    // gaps for AI multi-round self-mod + incremental (cache bridge epoch,
    // impact-scope partial re-lower, JIT unhandled deopt, linear metadata
    // flow, quote fallback refresh). Non-duplicative with #600
    // incremental-closure-stats, #680 impact_scope, #530 production-reloader.
    //
    // Fields (7 + sentinel):
    //   - bridge-epoch-cache-syncs   compiler_core_bridge_epoch_sync_total
    //   - impact-blocks              Evaluator total_affected_blocks_
    //   - partial-relower-hits       Evaluator partial_relower_count_
    //   - full-fallbacks             relower_full_called_count
    //   - jit-unhandled-deopts       compiler_core_jit_unhandled_invalidate_total
    //   - linear-metadata-flows      compiler_core_linear_metadata_flow_total
    //   - quote-fallback-refreshes   compiler_core_quote_fallback_refresh_total
    //   - schema == 657
    add("query:compiler-core-incremental-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t bridge_sync =
            m ? static_cast<std::int64_t>(
                    m->compiler_core_bridge_epoch_sync_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t impact_blocks =
            static_cast<std::int64_t>(ev.get_total_affected_blocks());
        const std::int64_t partial_relower =
            static_cast<std::int64_t>(ev.get_partial_relower_count());
        const std::int64_t full_fallback =
            m ? static_cast<std::int64_t>(
                    m->relower_full_called_count.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t jit_deopt =
            m ? static_cast<std::int64_t>(
                    m->compiler_core_jit_unhandled_invalidate_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t linear_flow =
            m ? static_cast<std::int64_t>(
                    m->compiler_core_linear_metadata_flow_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t quote_refresh =
            m ? static_cast<std::int64_t>(
                    m->compiler_core_quote_fallback_refresh_total.load(std::memory_order_relaxed))
              : 0;
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
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("bridge-epoch-cache-syncs", bridge_sync);
        insert_kv("impact-blocks", impact_blocks);
        insert_kv("partial-relower-hits", partial_relower);
        insert_kv("full-fallbacks", full_fallback);
        insert_kv("jit-unhandled-deopts", jit_deopt);
        insert_kv("linear-metadata-flows", linear_flow);
        insert_kv("quote-fallback-refreshes", quote_refresh);
        insert_kv("schema", 657);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 51 (orig lines 6568-6633)
void ObservabilityPrims::register_eval_p51(PrimRegistrar add, Evaluator& ev) {

    // Issue #658: query:highperf-cpp26-stats — 5 high-perf integration gaps
    // (Arena tier fallback + IRSoA dirty cascade + Value v2 classify +
    // ShapeProfiler history jitter + Pass DirtyAware short-circuit).
    // Non-duplicative with #657 compiler-core-incremental, #642 arena
    // auto-compact, #571 value-dispatch, #570 shape-stability, #494 pass-pipeline.
    //
    // Fields (5 + sentinel):
    //   - arena-tier-fallbacks      arena_small_tier_fallback_total
    //   - soa-dirty-cascades        irsoa_dirty_cascade_savings
    //   - value-classify-calls      value_classify_call_count
    //   - shape-history-jitter-wins history_jitter_reduction_count
    //   - pass-dirty-skips          passes_skipped_dirty_pipeline
    //   - schema == 658
    add("query:highperf-cpp26-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t arena_fallback = static_cast<std::int64_t>(
            aura::ast::arena_small_tier_fallback_total.load(std::memory_order_relaxed));
        const std::int64_t soa_cascade =
            m ? static_cast<std::int64_t>(
                    m->irsoa_dirty_cascade_savings.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t classify_calls = static_cast<std::int64_t>(
            types::value_classify_call_count.load(std::memory_order_relaxed));
        const std::int64_t jitter_wins = static_cast<std::int64_t>(
            shape::history_jitter_reduction_count.load(std::memory_order_relaxed));
        const std::int64_t dirty_skips = static_cast<std::int64_t>(
            passes_skipped_dirty_pipeline.load(std::memory_order_relaxed));
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
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("arena-tier-fallbacks", arena_fallback);
        insert_kv("soa-dirty-cascades", soa_cascade);
        insert_kv("value-classify-calls", classify_calls);
        insert_kv("shape-history-jitter-wins", jitter_wins);
        insert_kv("pass-dirty-skips", dirty_skips);
        insert_kv("schema", 658);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 52 (orig lines 6634-6686)
void ObservabilityPrims::register_eval_p52(PrimRegistrar add, Evaluator& ev) {

    // Issue #742: query:cpp26-contracts-stats — C++26 Contracts +
    // consteval hot-path invariant observability for Arena/SoA/Value/
    // Shape/Pass pipeline (non-duplicative with #658 highperf-cpp26,
    // #431 cxx26-invariants, #465 cxx26-hotpath-invariants).
    //
    // Fields (3 + sentinel):
    //   - contract-violations-caught  cpp26::contract_violations_caught_total
    //   - consteval-checks            kConstevalChecksTotal (compile-time)
    //   - hotpath-invariant-hits      cpp26::hotpath_invariant_hits_total
    //   - schema == 742
    add("query:cpp26-contracts-stats", [&ev](const auto&) -> EvalValue {
        (void)ev;
        const std::int64_t violations = static_cast<std::int64_t>(
            aura::core::cpp26::contract_violations_caught_total.load(std::memory_order_relaxed));
        const std::int64_t consteval_checks = aura::core::cpp26::kConstevalChecksTotal;
        const std::int64_t hotpath_hits = static_cast<std::int64_t>(
            aura::core::cpp26::hotpath_invariant_hits_total.load(std::memory_order_relaxed));
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
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("contract-violations-caught", violations);
        insert_kv("consteval-checks", consteval_checks);
        insert_kv("hotpath-invariant-hits", hotpath_hits);
        insert_kv("schema", 742);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 53 (orig lines 6687-6763)
void ObservabilityPrims::register_eval_p53(PrimRegistrar add, Evaluator& ev) {

    // Issue #743: query:arena-auto-policy-stats — Arena auto-compact + live
    // defrag + fiber safepoint + dirty/Shape closed loop (non-duplicative with
    // #642 arena-auto-compaction-stats, #685 arena-auto-compact-stats,
    // #569 arena-auto-compact-defrag-stats).
    //
    // Fields (5 + sentinel):
    //   - auto-compact-triggers     alloc-path + group adaptive triggers
    //   - defrag-fiber-safe-hits    fiber-context compact/defrag safepoints
    //   - fragmentation-post-mutate post-mutate frag ratio (basis points)
    //   - shape-inval-on-compact    ShapeProfiler + on_compact_hook fires
    //   - env-reval-success         env resync after compact invalidation
    //   - schema == 743
    add("query:arena-auto-policy-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t auto_triggers =
            aura::core::arena_policy::auto_compact_triggers_total.load(std::memory_order_relaxed);
        std::uint64_t defrag_fiber_safe =
            aura::core::arena_policy::defrag_fiber_safe_hits_total.load(std::memory_order_relaxed);
        const std::uint64_t frag_post =
            aura::core::arena_policy::fragmentation_post_mutate_bp.load(std::memory_order_relaxed);
        std::uint64_t shape_inval =
            aura::core::arena_policy::shape_inval_on_compact_total.load(std::memory_order_relaxed);
        std::uint64_t env_reval =
            aura::core::arena_policy::env_reval_success_total.load(std::memory_order_relaxed);
        if (ev.arena_) {
            const auto s = ev.arena_->stats();
            auto_triggers += s.auto_alloc_trigger_count;
            shape_inval += s.shape_inval_on_compact;
        }
        if (ev.arena_group_) {
            auto_triggers += ev.arena_group_->auto_compact_trigger_count();
            const auto ag = ev.arena_group_->auto_compact_policy_stats();
            auto_triggers += ag.auto_triggers;
            shape_inval += ag.shape_inval_on_compact;
        }
        if (ev.compiler_metrics()) {
            auto* m = static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            env_reval +=
                m->incremental_closure_env_version_resync_total.load(std::memory_order_relaxed);
        }
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
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("auto-compact-triggers", static_cast<std::int64_t>(auto_triggers));
        insert_kv("defrag-fiber-safe-hits", static_cast<std::int64_t>(defrag_fiber_safe));
        insert_kv("fragmentation-post-mutate", static_cast<std::int64_t>(frag_post));
        insert_kv("shape-inval-on-compact", static_cast<std::int64_t>(shape_inval));
        insert_kv("env-reval-success", static_cast<std::int64_t>(env_reval));
        insert_kv("schema", 743);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 54 (orig lines 6764-6824)
void ObservabilityPrims::register_eval_p54(PrimRegistrar add, Evaluator& ev) {

    // Issue #744: query:shape-jit-pass-closedloop-stats — Shape stability churn
    // → IRSoA dirty → DirtyAware Pass short-circuit → JIT deopt/recompile
    // (non-duplicative with #686 shape-value-pass-stats, #605 shapeprofiler,
    // #723 DirtyAware, #720 JIT metadata).
    //
    // Fields (4 + sentinel):
    //   - stability-churn-deopts   stable→unstable / invalidate deopt fires
    //   - dirty-from-shape         dirty_hook / IRSoA cascade from shape loss
    //   - incremental-recompile-hits JIT invalidate + recompile requests
    //   - speculative-win-lost     stable speculative opt invalidated
    //   - schema == 744
    add("query:shape-jit-pass-closedloop-stats", [&ev](const auto&) -> EvalValue {
        (void)ev;
        const std::int64_t churn = static_cast<std::int64_t>(
            shape_jit_pass::stability_churn_deopts_total.load(std::memory_order_relaxed));
        const std::int64_t dirty_shape = static_cast<std::int64_t>(
            shape_jit_pass::dirty_from_shape_total.load(std::memory_order_relaxed));
        const std::int64_t recompile = static_cast<std::int64_t>(
            shape_jit_pass::incremental_recompile_hits_total.load(std::memory_order_relaxed));
        const std::int64_t win_lost = static_cast<std::int64_t>(
            shape_jit_pass::speculative_win_lost_total.load(std::memory_order_relaxed));
        const std::int64_t stable_skips = static_cast<std::int64_t>(
            passes_skipped_shape_stable_blocks.load(std::memory_order_relaxed));
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
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("stability-churn-deopts", churn);
        insert_kv("dirty-from-shape", dirty_shape);
        insert_kv("incremental-recompile-hits", recompile);
        insert_kv("speculative-win-lost", win_lost);
        insert_kv("shape-stable-block-skips", stable_skips);
        insert_kv("schema", 744);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 55 (orig lines 6825-6891)
void ObservabilityPrims::register_eval_p55(PrimRegistrar add, Evaluator& ev) {

    // Issue #745: query:constraint-reverify-occurrence-stats — dynamic
    // effective_reverify_limit + Occurrence-narrowed priority scan in
    // reverify_clean_constraints_for_touched (non-duplicative with #466,
    // #690 constraint-typed-mutate-stats, #659 typesystem-typed-mutate).
    //
    // Fields (4 + sentinel):
    //   - reverify-hits-on-narrow      priority scans on occurrence-narrow roots
    //   - cross-delta-blame-complete   blame chain with active_mutation_id
    //   - timeout-prevented            dynamic limit avoided fixed-256 truncation
    //   - stale-blame-invalidation     cross-delta hit without mutation epoch
    //   - schema == 745
    add("query:constraint-reverify-occurrence-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t narrow_hits =
            m ? static_cast<std::int64_t>(
                    m->constraint_reverify_narrow_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t blame_complete =
            m ? static_cast<std::int64_t>(
                    m->constraint_blame_chain_complete_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t timeout_prevented =
            m ? static_cast<std::int64_t>(
                    m->constraint_reverify_timeout_prevented_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t stale_blame =
            m ? static_cast<std::int64_t>(
                    m->constraint_stale_blame_invalidation_total.load(std::memory_order_relaxed))
              : 0;
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
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("reverify-hits-on-narrow", narrow_hits);
        insert_kv("cross-delta-blame-complete", blame_complete);
        insert_kv("timeout-prevented", timeout_prevented);
        insert_kv("stale-blame-invalidation", stale_blame);
        insert_kv("schema", 745);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

} // namespace aura::compiler::primitives_detail
