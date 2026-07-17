// evaluator_primitives_obs_eval_07.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 56 (orig lines 6892-6952)
void ObservabilityPrims::register_eval_p56(PrimRegistrar add, Evaluator& ev) {

    // Issue #746 / #1615: query:jit-typed-mutation-stats — narrow_evidence /
    // TypeId / linear_ownership_state + post-coercion linear revalidation.
    // Schema **1615** (lineage 746). AC keys:
    //   linear_coercion_reval_count, narrow_evidence_guardshape_hits
    ObservabilityPrims::register_stats_impl(
        "query:jit-typed-mutation-stats", [&ev](const auto&) -> EvalValue {
            const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
            const std::int64_t narrow_hits = static_cast<std::int64_t>(
                jit_typed_mutation::narrow_evidence_hits_total.load(std::memory_order_relaxed));
            const std::int64_t cast_elided = static_cast<std::int64_t>(
                jit_typed_mutation::cast_elided_in_l2_total.load(std::memory_order_relaxed));
            const std::int64_t linear_opt = static_cast<std::int64_t>(
                jit_typed_mutation::linear_state_optimized_total.load(std::memory_order_relaxed));
            const std::int64_t stamped = static_cast<std::int64_t>(
                jit_typed_mutation::type_propagation_stamped_total.load(std::memory_order_relaxed));
            const std::int64_t denom = narrow_hits + cast_elided + linear_opt;
            const std::int64_t coverage_bp =
                denom > 0 ? (10000 * stamped) / denom : (stamped > 0 ? 10000 : 0);
            const std::int64_t guardshape_hits =
                m ? static_cast<std::int64_t>(
                        m->coercion_narrow_evidence_hits_total.load(std::memory_order_relaxed))
                  : narrow_hits;
            const std::int64_t lin_co_reval =
                m ? static_cast<std::int64_t>(
                        m->linear_coercion_reval_count.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t lin_co_ok =
                m ? static_cast<std::int64_t>(
                        m->linear_coercion_reval_ok_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t lin_co_viol =
                m ? static_cast<std::int64_t>(
                        m->linear_coercion_violations_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t ne_prop =
                m ? static_cast<std::int64_t>(
                        m->narrow_evidence_propagated_total.load(std::memory_order_relaxed))
                  : 0;
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
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            // #746 lineage
            insert_kv("narrow-evidence-hits", narrow_hits);
            insert_kv("cast-elided-in-l2", cast_elided);
            insert_kv("linear-state-optimized", linear_opt);
            insert_kv("type-propagation-coverage", coverage_bp);
            // #1615 AC keys
            insert_kv("linear_coercion_reval_count", lin_co_reval);
            insert_kv("linear-coercion-reval-count", lin_co_reval);
            insert_kv("linear-coercion-reval-ok", lin_co_ok);
            insert_kv("linear-coercion-violations", lin_co_viol);
            insert_kv("narrow_evidence_guardshape_hits", guardshape_hits);
            insert_kv("narrow-evidence-guardshape-hits", guardshape_hits);
            insert_kv("narrow-evidence-propagated", ne_prop);
            insert_kv("post-coercion-reval-wired", 1);
            insert_kv("guardshape-narrow-wired", 1);
            insert_kv("issue", 1615);
            insert_kv("schema", 1615); // lineage 746
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 57 (orig lines 6953-7012)
void ObservabilityPrims::register_eval_p57(PrimRegistrar add, Evaluator& ev) {

    // Issue #747: query:linear-occurrence-mutate-stats — OwnershipEnv +
    // Occurrence Typing predicate-branch linear safety under typed mutation
    // (non-duplicative with #688 linear-ownership-typed-mutate, #689
    // occurrence-typing-mutate, #746 jit-typed-mutation).
    //
    // Fields (3 + sentinel):
    //   - revalidate-hits                 post-mutate linear∩occurrence revalidates
    //   - escape-violations-prevented     escape/ownership violations caught early
    //   - predicate-branch-linear-safe    ownership pass on narrowed predicate branches
    //   - schema == 747
    ObservabilityPrims::register_stats_impl(
        "query:linear-occurrence-mutate-stats", [&ev](const auto&) -> EvalValue {
            const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
            const std::int64_t revalidates =
                m ? static_cast<std::int64_t>(
                        m->linear_occurrence_revalidate_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t escape_prev =
                m ? static_cast<std::int64_t>(
                        m->linear_occurrence_escape_prevented_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t branch_safe =
                m ? static_cast<std::int64_t>(
                        m->linear_occurrence_predicate_safe_total.load(std::memory_order_relaxed))
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
            insert_kv("revalidate-hits", revalidates);
            insert_kv("escape-violations-prevented", escape_prev);
            insert_kv("predicate-branch-linear-safe", branch_safe);
            insert_kv("schema", 747);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 58 (orig lines 7013-7078)
void ObservabilityPrims::register_eval_p58(PrimRegistrar add, Evaluator& ev) {

    // Issue #748: query:sv-verification-structure-stats — P0 SV verification
    // EDSL structured representation + emit fidelity + dirty re-emit closed-loop
    // (consolidates #724/#725/#726; non-duplicative with #694 sv-sva-structure,
    // #640 sv-verification-closedloop, #693 hardware-backend-sv-closedloop).
    //
    // Fields (4 + sentinel):
    //   - structure-mutate-hits   sv_verification_structure_mutate_hits_total
    //   - dirty-reemit-triggers   sv_verification_dirty_reemit_total
    //   - emit-fidelity-pass      sv_emit_parse_success_total
    //   - emit-fidelity-fail      sv_emit_parse_fail_total
    //   - schema == 748
    ObservabilityPrims::register_stats_impl(
        "query:sv-verification-structure-stats", [&ev](const auto&) -> EvalValue {
            const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
            const std::int64_t structure_mutate =
                m ? static_cast<std::int64_t>(m->sv_verification_structure_mutate_hits_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t dirty_reemit =
                m ? static_cast<std::int64_t>(
                        m->sv_verification_dirty_reemit_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t emit_pass =
                m ? static_cast<std::int64_t>(
                        m->sv_emit_parse_success_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t emit_fail =
                m ? static_cast<std::int64_t>(
                        m->sv_emit_parse_fail_total.load(std::memory_order_relaxed))
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
            insert_kv("structure-mutate-hits", structure_mutate);
            insert_kv("dirty-reemit-triggers", dirty_reemit);
            insert_kv("emit-fidelity-pass", emit_pass);
            insert_kv("emit-fidelity-fail", emit_fail);
            insert_kv("schema", 748);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 59 (orig lines 7079-7144)
void ObservabilityPrims::register_eval_p59(PrimRegistrar add, Evaluator& ev) {

    // Issue #801: query:sv-commercial-emit-fidelity-stats — commercial SV emit
    // roundtrip + dirty re-emit fidelity dashboard (refines #772/#748/#725;
    // non-duplicative with query:sv-verification-structure-stats #748).
    //
    // Fields (4 + sentinel):
    //   - emit-parse-success-hits          sv_commercial_emit_parse_success_total
    //   - roundtrip-mismatch-prevented     sv_commercial_emit_roundtrip_mismatch_prevented_total
    //   - dirty-reemit-hits                sv_commercial_emit_dirty_reemit_total
    //   - commercial-tool-compatible-hits    sv_commercial_emit_tool_compatible_total
    //   - schema == 801
    ObservabilityPrims::register_stats_impl(
        "query:sv-commercial-emit-fidelity-stats", [&ev](const auto&) -> EvalValue {
            const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
            const std::int64_t parse_success =
                m ? static_cast<std::int64_t>(
                        m->sv_commercial_emit_parse_success_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t mismatch_prevented =
                m ? static_cast<std::int64_t>(
                        m->sv_commercial_emit_roundtrip_mismatch_prevented_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t dirty_reemit =
                m ? static_cast<std::int64_t>(
                        m->sv_commercial_emit_dirty_reemit_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t tool_compatible =
                m ? static_cast<std::int64_t>(
                        m->sv_commercial_emit_tool_compatible_total.load(std::memory_order_relaxed))
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
            insert_kv("emit-parse-success-hits", parse_success);
            insert_kv("roundtrip-mismatch-prevented", mismatch_prevented);
            insert_kv("dirty-reemit-hits", dirty_reemit);
            insert_kv("commercial-tool-compatible-hits", tool_compatible);
            insert_kv("schema", 801);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 60 (orig lines 7145-7209)
void ObservabilityPrims::register_eval_p60(PrimRegistrar add, Evaluator& ev) {

    // Issue #802: query:sv-verification-self-evolution-stats — feedback-driven
    // structured self-evolution closed-loop dashboard (refines #774/#726/#748;
    // non-duplicative with query:closed-loop-reliability-stats #726).
    //
    // Fields (4 + sentinel):
    //   - feedback-parse-hits       sv_self_evo_feedback_parse_total
    //   - structured-mutate-hits    sv_self_evo_structured_mutate_total
    //   - closed-loop-rounds        sv_self_evo_closed_loop_rounds_total
    //   - convergence-hits          sv_self_evo_convergence_hits_total
    //   - schema == 802
    ObservabilityPrims::register_stats_impl(
        "query:sv-verification-self-evolution-stats", [&ev](const auto&) -> EvalValue {
            const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
            const std::int64_t feedback_parse =
                m ? static_cast<std::int64_t>(
                        m->sv_self_evo_feedback_parse_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t structured_mutate =
                m ? static_cast<std::int64_t>(
                        m->sv_self_evo_structured_mutate_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t closed_loop_rounds =
                m ? static_cast<std::int64_t>(
                        m->sv_self_evo_closed_loop_rounds_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t convergence =
                m ? static_cast<std::int64_t>(
                        m->sv_self_evo_convergence_hits_total.load(std::memory_order_relaxed))
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
            insert_kv("feedback-parse-hits", feedback_parse);
            insert_kv("structured-mutate-hits", structured_mutate);
            insert_kv("closed-loop-rounds", closed_loop_rounds);
            insert_kv("convergence-hits", convergence);
            insert_kv("schema", 802);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 61 (orig lines 7210-7415)
void ObservabilityPrims::register_eval_p61(PrimRegistrar add, Evaluator& ev) {

    // Issue #803: query:seva-longrunning-concurrent-slo — SEVA
    // Long-Running Concurrent Verification Evolution SLO
    // observability composite (P0 EDA-SV-verification-production
    // long-running concurrent multi-agent harness foundation;
    // consolidates/non-duplicates #794 + #755 + #773 + #774 + #802
    // + #748). #803 is the FIRST observability surface that tracks
    // the *production-scale long-running concurrent SEVA SLO
    // composite* the Agent reads to decide whether the long-
    // running concurrent verification self-evolution harness is
    // production-ready for commercial multi-agent EDA agent
    // deployment at SoC scale.
    //
    // Fields (8 + sentinel):
    //   - convergence-rate       derived (convergence_hits /
    //                             closed_loop_rounds) × 10000
    //                             (0-10000 fixed-point percent
    //                             × 100; 10000 = 100.00% baseline
    //                             when closed_loop_rounds == 0 =
    //                             vacuous-true default; SLO target
    //                             >98% = 9800 per body "convergence
    //                             rate >98% without manual
    //                             intervention")
    //   - ref-drift-prevented    seva_concurrent_ref_drift_prevented_
    //                             total (NEW atomic; # of ref-drift
    //                             attempts caught + prevented during
    //                             long-running concurrent SEVA round;
    //                             bumped by
    //                             bump_seva_concurrent_ref_drift_
    //                             prevented() when
    //                             StableNodeRef.refresh_if_stale +
    //                             auto re-resolve succeeds;
    //                             distinct from #762 stale_ref
    //                             which is workspace-loop level)
    //   - hygiene-safe-rollback-pct  derived (code_as_data_rollback_
    //                             hygiene_safe_total /
    //                             (atomic_batch_sv_rollback_total +
    //                             code_as_data_rollback_hygiene_safe
    //                             _total + 1)) × 10000 (0-10000
    //                             fixed-point percent × 100; SLO
    //                             target 100% = 10000 per body
    //                             "ref_fidelity 100%" + "hygiene_
    //                             safe_rollback 100%")
    //   - steal-during-verification-mutate  seva_concurrent_steal_
    //                             during_verification_mutate_total
    //                             (NEW atomic; # of fiber steal
    //                             events occurring during a
    //                             verification mutate inside the
    //                             long-running harness — a
    //                             high-fidelity load metric for
    //                             SEVA test surface; bumped by
    //                             bump_seva_concurrent_steal_during_
    //                             verification_mutate() when a
    //                             fiber steal fires during a
    //                             mutation_stack_ + outermost
    //                             MutationBoundaryGuard active
    //                             during a SEVA round)
    //   - dirty-consistency-hits seva_concurrent_dirty_
    //                             propagation_hits_total (NEW
    //                             atomic; # of dirty propagation
    //                             consistency checks that passed
    //                             during harness round — a no-fail
    //                             signal; bumped by
    //                             bump_seva_concurrent_dirty_
    //                             propagation_hits() at the
    //                             mark_dirty_upward + verify_dirty_
    //                             pass-mark)
    //   - avg-rounds-to-target   derived (closed_loop_rounds /
    //                             (convergence_hits + 1)) so 0
    //                             when no convergence observed
    //                             yet (a typical harness
    //                             converges in ~3-7 rounds; a
    //                             low ratio = SLO breach)
    //   - longrunning-harness-active  hardcoded 0 (Phase 2+; the
    //                             actual `tests/test_seva_longrunning
    //                             _concurrent_verification_evolution
    //                             .cpp` + CI gate step + SLO
    //                             dashboard + self-heal hooks +
    //                             SEVA tutorial extension all
    //                             remain follow-up work per body
    //                             Actionable 1+3+4+6)
    //   - recommendation         derived 0/1/2/3 (0 = production-
    //                             ready when SLO met + harness
    //                             active; 1 = near-production when
    //                             SLO met but harness not yet
    //                             active; 2 = partial Phase 1
    //                             when convergence seen but not
    //                             yet SLO; 3 = early-stage)
    //   - schema == 803          drift sentinel
    ObservabilityPrims::register_stats_impl(
        "query:seva-longrunning-concurrent-slo", [&ev](const auto&) -> EvalValue {
            const auto* m = ev.compiler_metrics()
                                ? static_cast<const CompilerMetrics*>(ev.compiler_metrics())
                                : nullptr;
            // Reused #802 atomics for convergence derivation.
            const std::int64_t closed_loop_rounds =
                m ? static_cast<std::int64_t>(
                        m->sv_self_evo_closed_loop_rounds_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t convergence_hits =
                m ? static_cast<std::int64_t>(
                        m->sv_self_evo_convergence_hits_total.load(std::memory_order_relaxed))
                  : 0;
            // Reused #759 + #632 for hygiene-safe rollback derivation.
            const std::int64_t hygiene_safe_total =
                m ? static_cast<std::int64_t>(
                        m->code_as_data_rollback_hygiene_safe_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t sv_rollback_total =
                m ? static_cast<std::int64_t>(
                        m->atomic_batch_sv_rollback_total.load(std::memory_order_relaxed))
                  : 0;
            // NEW #803 atomics.
            const std::int64_t ref_drift_prevented =
                m ? static_cast<std::int64_t>(m->seva_concurrent_ref_drift_prevented_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t steal_during_v_mutate =
                m ? static_cast<std::int64_t>(
                        m->seva_concurrent_steal_during_verification_mutate_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t dirty_consistency_hits =
                m ? static_cast<std::int64_t>(m->seva_concurrent_dirty_propagation_hits_total.load(
                        std::memory_order_relaxed))
                  : 0;
            // Derived convergence_rate: vacuous-true 10000 baseline when
            // closed_loop_rounds == 0; otherwise integer division.
            std::int64_t convergence_rate = 10000;
            if (closed_loop_rounds > 0) {
                convergence_rate = static_cast<std::int64_t>(
                    (convergence_hits * ::aura::compiler::kBasisPointScale) / closed_loop_rounds);
            }
            // Derived hygiene_safe_rollback_pct: 10000 baseline when no
            // rollbacks have happened; otherwise (hygiene_safe / total) ×
            // 10000. The +1 in denominator ensures vacuous-true semantics
            // when only hygiene_safe was observed (0 rollback round).
            std::int64_t hygiene_safe_rollback_pct = 10000;
            const std::int64_t total_rollbacks = sv_rollback_total;
            if (hygiene_safe_total + total_rollbacks > 0) {
                hygiene_safe_rollback_pct = static_cast<std::int64_t>(
                    (hygiene_safe_total * ::aura::compiler::kBasisPointScale) /
                    (total_rollbacks + 1));
            }
            // Derived avg_rounds_to_target: 0 baseline when no convergence
            // hits; otherwise rounds / (hits + 1).
            std::int64_t avg_rounds_to_target = 0;
            if (convergence_hits > 0) {
                avg_rounds_to_target =
                    static_cast<std::int64_t>(closed_loop_rounds / (convergence_hits + 1));
            }
            // Hardcoded "not yet" — Phase 2+ deferred.
            const std::int64_t longrunning_harness_active = 0;
            // Recommendation derivation:
            //   0 = production-ready (SLO met + harness active)
            //   1 = near-production (SLO met but harness not yet active)
            //   2 = partial Phase 1 (convergence seen but not yet SLO)
            //   3 = early-stage (no activity yet)
            std::int64_t recommendation = 3;
            if (convergence_hits + closed_loop_rounds + ref_drift_prevented +
                    steal_during_v_mutate + dirty_consistency_hits >
                0) {
                if (convergence_rate >= 9800 && hygiene_safe_rollback_pct >= 10000) {
                    recommendation = longrunning_harness_active ? 0 : 1;
                } else {
                    recommendation = 2;
                }
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
            insert_kv("convergence-rate", convergence_rate);
            insert_kv("ref-drift-prevented", ref_drift_prevented);
            insert_kv("hygiene-safe-rollback-pct", hygiene_safe_rollback_pct);
            insert_kv("steal-during-verification-mutate", steal_during_v_mutate);
            insert_kv("dirty-consistency-hits", dirty_consistency_hits);
            insert_kv("avg-rounds-to-target", avg_rounds_to_target);
            insert_kv("longrunning-harness-active", longrunning_harness_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 803);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 62 (orig lines 7416-7492)
void ObservabilityPrims::register_eval_p62(PrimRegistrar add, Evaluator& ev) {

    // Issue #766: query:ir-soa-migration-stats — IR-SoA migration
    // observability + DirtyAware incremental pipeline dashboard
    // (P0 high-perf C++26 DOD/SoA foundation; refines #167/#463/
    // #741; non-duplicative with #729 query:soa-hotpath-stats and
    // #765 query:incremental-quote-lambda-linear-stats).
    //
    // Fields (5 + sentinel):
    //   - soa-instructions-emitted     ir_soa_instructions_emitted_total
    //   - dirty-block-skips            ir_soa_dirty_block_skips_total
    //   - clean-block-hit-rate         ir_soa_clean_block_hit_rate_pct
    //                                   (0-10000 fixed-point percent × 100;
    //                                    10000 = 100.00%)
    //   - pmr-column-utilization       ir_soa_pmr_column_utilization_pct
    //                                   (0-10000 fixed-point percent × 100;
    //                                    5000 = 50.00%)
    //   - jit-soa-codegen-time-ns      ir_soa_jit_codegen_time_ns_total
    //   - schema == 766
    ObservabilityPrims::register_stats_impl(
        "query:ir-soa-migration-stats", [&ev](const auto&) -> EvalValue {
            const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
            const std::int64_t soa_instructions_emitted =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_instructions_emitted_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t dirty_block_skips =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_dirty_block_skips_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t clean_block_hit_rate =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_clean_block_hit_rate_pct.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t pmr_column_utilization =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_pmr_column_utilization_pct.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t jit_soa_codegen_time_ns =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_jit_codegen_time_ns_total.load(std::memory_order_relaxed))
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
            insert_kv("soa-instructions-emitted", soa_instructions_emitted);
            insert_kv("dirty-block-skips", dirty_block_skips);
            insert_kv("clean-block-hit-rate", clean_block_hit_rate);
            insert_kv("pmr-column-utilization", pmr_column_utilization);
            insert_kv("jit-soa-codegen-time-ns", jit_soa_codegen_time_ns);
            insert_kv("schema", 766);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 63 (orig lines 7493-7631)
void ObservabilityPrims::register_eval_p63(PrimRegistrar add, Evaluator& ev) {

    // Issue #767: query:arena-auto-compact-defrag-fiber-stats —
    // Arena Auto-Compact Policy + Live Defrag + Fiber/GC Safepoint
    // Yield observability dashboard (P0 high-perf C++26 Arena
    // foundation; completes #300 P1 + #685 + #731; non-duplicative
    // with #685 query:arena-auto-compact-stats and #642 query:
    // arena-auto-compaction-stats).
    //
    // The 6 fields map to the issue body AC4 exactly:
    //   - auto-compact-triggers        existing arena_/arena_group_
    //                                  stats (auto_alloc_trigger_count /
    //                                  auto_triggers) — proxy for
    //                                  "how often the auto-compact
    //                                  policy fired" (high = memory
    //                                  pressure real; 0 = threshold
    //                                  too lax).
    //   - frag-reduced-bp              existing arena stats (frag_reduced_bp;
    //                                  basis points × 100 — 5000 = 50.00%)
    //                                  — proxy for "how much fragmentation
    //                                  the auto-compact path reduced".
    //   - live-defrag-savings          existing arena stats (defrag_savings_alloc /
    //                                  defrag_savings) — proxy for "how
    //                                  much memory the live defrag recovered".
    //   - fiber-yield-during-compact   arena_auto_compact_fiber_yield_during_
    //                                  compact_total (NEW atomic, foundation
    //                                  for AC2 — actual fiber yields during
    //                                  compact/defrag).
    //   - shape-inval-count            existing arena stats (shape_inval_on_compact;
    //                                  mirror #685 shape-inval-on-compact).
    //   - defrag-blocked-fibers        arena_auto_compact_defrag_blocked_
    //                                  fibers_total (NEW atomic, foundation
    //                                  for AC3 — fibers blocked waiting
    //                                  for defrag to complete; a metric
    //                                  #767 introduces to surface the
    //                                  hidden defrag-fiber interaction
    //                                  cost).
    //   - production-readiness         derived ordinal (0 = production-
    //                                  ready, 1 = partial Phase 1 only,
    //                                  2 = early-stage) added by #797
    //                                  to make the body AC4 "SLO frag
    //                                  <0.3 under load" observable to
    //                                  the Agent without exposing the
    //                                  raw frag_ratio. Computed from
    //                                  the same atomics above — no
    //                                  independent counters; refresh
    //                                  cost is one branch.
    //   - schema == 767
    ObservabilityPrims::register_stats_impl(
        "query:arena-auto-compact-defrag-fiber-stats", [&ev](const auto&) -> EvalValue {
            // Reuse the existing arena_/arena_group_ stats for the 4 fields
            // that already have a source-of-truth — mirrors the pattern
            // used by #685 (query:arena-auto-compact-stats).
            std::uint64_t auto_triggers = 0;
            std::uint64_t frag_reduced_bp = 0;
            std::uint64_t shape_inval_count = 0;
            std::uint64_t live_defrag_savings = 0;
            if (ev.arena_) {
                const auto s = ev.arena_->stats();
                auto_triggers += s.auto_alloc_trigger_count;
                frag_reduced_bp += s.frag_reduced_bp;
                shape_inval_count += s.shape_inval_on_compact;
                live_defrag_savings += s.defrag_savings_alloc;
            }
            if (ev.arena_group_) {
                const auto ag = ev.arena_group_->auto_compact_policy_stats();
                auto_triggers += ag.auto_triggers;
                frag_reduced_bp += ag.frag_reduced;
                shape_inval_count += ag.shape_inval_on_compact;
                live_defrag_savings += ag.defrag_savings;
            }
            const auto* m = ev.compiler_metrics()
                                ? static_cast<const CompilerMetrics*>(ev.compiler_metrics())
                                : nullptr;
            const std::int64_t fiber_yield_during_compact =
                m ? static_cast<std::int64_t>(
                        m->arena_auto_compact_fiber_yield_during_compact_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t defrag_blocked_fibers =
                m ? static_cast<std::int64_t>(
                        m->arena_auto_compact_defrag_blocked_fibers_total.load(
                            std::memory_order_relaxed))
                  : 0;
            // Issue #797 AC4 (body): "SLO frag <0.3 under load" — derive
            // a production-readiness ordinal from the existing
            // frag-reduced-bp + bump activity. Both #767 and #797 share
            // the same source-of-truth atomics so the derived field is
            // deterministic: 0 = production-ready (auto-policy fires +
            // yield observed under sustained load), 1 = partial Phase 1
            // only (some activity but no fiber-yield / no defrag-blocked
            // surface observed yet), 2 = early-stage (no auto-compact
            // activity yet — service has not exercised the tiered pool
            // hot path). The frag_ratio threshold 0.30 lives in
            // evaluator.ixx probe_arena_auto_policy_on_boundary_exit
            // (#643 wire-up), not exposed here — the field tells the
            // Agent whether the production-readiness SLO is observable,
            // not whether the threshold itself was met.
            std::int64_t production_readiness = 2; // default early-stage
            if (auto_triggers > 0 || live_defrag_savings > 0 || shape_inval_count > 0) {
                production_readiness =
                    (fiber_yield_during_compact > 0 || defrag_blocked_fibers > 0) ? 0 : 1;
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
            insert_kv("frag-reduced-bp", static_cast<std::int64_t>(frag_reduced_bp));
            insert_kv("live-defrag-savings", static_cast<std::int64_t>(live_defrag_savings));
            insert_kv("fiber-yield-during-compact", fiber_yield_during_compact);
            insert_kv("shape-inval-count", static_cast<std::int64_t>(shape_inval_count));
            insert_kv("defrag-blocked-fibers", defrag_blocked_fibers);
            insert_kv("production-readiness", production_readiness);
            insert_kv("schema", 767);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

} // namespace aura::compiler::primitives_detail
