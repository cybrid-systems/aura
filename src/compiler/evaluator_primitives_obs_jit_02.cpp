// evaluator_primitives_obs_jit_02.cpp — Issue #909: peeled domain registration from observability
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
    ev.primitives_.add(
        "query:envframe-dualpath-policy-stats",
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
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .doc = "EnvFrame dual-path consistency enforcement + desync panic "
                        "policy + GCEnvWalkFn stale handling observability: "
                        "desync-panic-count (strict-panic firings), "
                        "gc-stale-desync-hits (GCEnvWalkFn detected stale under "
                        "concurrent steal/mutate), dualpath-repair + "
                        "version-mismatch (cross-reference from #647 atomics). "
                        "Pairs with the existing #647 query:envframe-dualpath-"
                        "stale-stats-hash 3-field hash but tracks the *desync "
                        "panic policy + GCEnvWalkFn stale handling* specifically "
                        "as separate per-decision-point counters. #756 exposes "
                        "the strict-panic vs log-and-sync mode adoption rate the "
                        "Agent consumes to decide whether to enable strict-panic "
                        "policy or trigger re-ensure on concurrent steal.",
                 .category = "general",
                 .schema = "() -> hash"});
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
    ev.primitives_.add(
        "query:macro-hygiene-provenance-stats",
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
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .doc = "Fine-grained MacroIntroduced provenance tracking + dynamic "
                        "inliner policy + AI-queryable hygiene violation correlation "
                        "observability: provenance-captured (per-node provenance at "
                        "clone_macro_body), inliner-policy-violations (dynamic inliner "
                        "policy + static respect_macro_hygiene_ disagreements), "
                        "provenance-violations + hygiene-dirty-impact (cross-reference "
                        "from #654 atomics). Pairs with the existing #654 "
                        "query:macro-hygiene-fiber-panic-stats 5-field hash + #458 "
                        "query:pattern-hygiene-stats basic count + #373 mutate hygiene "
                        "guard but tracks the *fine-grained provenance + dynamic "
                        "inliner policy + per-macro correlation* specifically as "
                        "separate per-decision-point counters. #757 exposes the "
                        "provenance + inliner-policy + per-macro correlation adoption "
                        "rate the Agent consumes to decide whether to enable "
                        "fine-grained provenance tracking or trigger inliner-policy "
                        "tuning under Guard.",
                 .category = "general",
                 .schema = "() -> hash"});
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
    ev.primitives_.add(
        "query:edsl-reflection-stats",
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
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .doc = "Runtime auto_validate bridge for user-defined EDSL structs "
                        "(DEFINE_STRUCT / custom nodes) under MutationBoundaryGuard "
                        "with macro hygiene invariant correlation observability: "
                        "validated-edsl (per-type EDSL struct pass), "
                        "hygiene-invariants-held (MacroIntroduced descendants "
                        "verified for valid provenance), schema-fail-by-type (per-type "
                        "EDSL struct fail), macro-correlated-violations (hygiene "
                        "violations correlated to macro_def_id). Pairs with the "
                        "existing #750 query:reflection-schema-stats 4-field hash "
                        "but tracks the *user-defined EDSL struct + macro hygiene "
                        "invariant correlation* specifically as separate "
                        "per-decision-point counters. #758 exposes the EDSL "
                        "extension adoption rate the Agent consumes to decide "
                        "whether to define new DEFINE_STRUCT types or trigger "
                        "hygiene invariant checks under Guard commit.",
                 .category = "general",
                 .schema = "() -> hash"});
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
    ev.primitives_.add(
        "query:code-as-data-maturity-stats",
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
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .doc = "Unified 'code-as-data' closed-loop maturity observability "
                        "composite (production readiness dashboard for Task6): "
                        "fidelity-samples (total marker propagation fidelity check "
                        "samples — denominator for fidelity_pct derivation), "
                        "fidelity-drift (samples where marker propagation drift "
                        "detected — drift / samples = 1 - fidelity_rate), "
                        "guard-rollback-hygiene-safe (Guard rollback events that "
                        "preserved hygiene invariants + StableRef validity — safe / "
                        "attempts = guard_rollback_hygiene_safe_pct), "
                        "reflect-schema-macro-edsl (reflect schema validation calls "
                        "on macro-generated or EDSL-mutated subtrees — covered / "
                        "total = reflection schema coverage on macro/EDSL ratio). "
                        "Pairs with the existing #757 query:macro-hygiene-"
                        "provenance-stats 4-field hash + #758 query:edsl-reflection-"
                        "stats 4-field hash but tracks the *code-as-data closed-loop "
                        "maturity composite* specifically as separate per-decision-"
                        "point counters. #759 exposes the integrated macro + reflect "
                        "+ EDSL self-evo loop production health the Agent consumes "
                        "to decide whether to trigger self-heal, alert on SLO "
                        "breach, or roll out additional task6 themes.",
                 .category = "general",
                 .schema = "() -> hash"});
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
    ev.primitives_.add(
        "query:pattern-performance-stats",
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
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .doc = "query:pattern performance + hygiene fidelity observability "
                        "for large macro-heavy concurrent AI pattern-mutate loops: "
                        "linear-scans (linear O(N) pattern scans — high value = perf "
                        "cliff on tag_arity_index_ fast-path miss), index-hits "
                        "(tag_arity_index_ fast-path O(1) candidate set retrievals via "
                        "(tag, child_count, marker) hash — high value = perf win), "
                        "wildcard-cost (... wildcard match firings — early-exit / "
                        "DFA cost on rest-param handling), hygiene-filtered (macro "
                        "nodes filtered / skipped by deep hygiene predicate — :marker "
                        "MacroIntroduced :from-macro sym in QueryExpr). Pairs with "
                        "the existing #757 query:macro-hygiene-provenance-stats + "
                        "#758 query:edsl-reflection-stats + #759 query:code-as-data-"
                        "maturity-stats 4-field hashes but tracks the *query:pattern "
                        "performance + hygiene fidelity* specifically as separate "
                        "per-decision-point counters. #760 exposes the pattern-match "
                        "perf cliff + hygiene predicate activity the Agent consumes "
                        "to decide whether to rebuild the index, tune the matcher, "
                        "or trigger deep hygiene filter under Guard commit.",
                 .category = "general",
                 .schema = "() -> hash"});
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
    ev.primitives_.add(
        "query:mutate-batch-stats",
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
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .doc = "End-to-end atomic batch mutate + suppressed generation bump + "
                        "cross-fiber safety observability composite for reliable "
                        "multi-step AI iterative edits: batches-started (# of "
                        "(mutate:batch [body]) or begin/commit batch lifecycles "
                        "entered — proxy for atomic compound AI edit volume), "
                        "suppressed-bumps (# of generation bumps suppressed by the "
                        "batch guard — the whole point of suppressed bumps; high "
                        "value = churn saved), cross-fiber-steals-during-batch (# of "
                        "fiber steals occurring while a batch is active — triggers "
                        "version re-stamp + StableRef refresh), hygiene-violations-"
                        "in-batch (# of hygiene guard violations caught within a "
                        "batch boundary — batch rollback prevented partial apply). "
                        "Pairs with the existing #757 + #758 + #759 + #760 4-field "
                        "observability hashes but tracks the *end-to-end atomic batch "
                        "mutate + suppressed generation bump + cross-fiber safety "
                        "composite* specifically as separate per-decision-point "
                        "counters. #761 exposes the atomic compound EDSL edit health "
                        "the Agent consumes to decide whether to batch, suppress "
                        "bumps, or trigger cross-fiber re-stamp under Guard commit.",
                 .category = "general",
                 .schema = "() -> hash"});
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
    ev.primitives_.add(
        "query:workspace-closedloop-orchestration-stats",
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
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .doc = "Workspace '锁定-导航-修改-执行' closed-loop orchestration "
                        "observability under concurrent fiber + multi-Agent parallel "
                        "edits: concurrent-query-mutate (# of concurrent query+mutate "
                        "success events on shared / COW workspaces under fiber steal — "
                        "proxy for concurrent orchestration health), cross-cow-ref-valid "
                        "(# of cross-COW StableRef accesses that remained valid after "
                        "auto-propagation — valid / total = cross_cow_ref_validity_pct "
                        "derivation), yield-points-hit (# of explicit yield point hits "
                        "in matcher / children iteration / mark_dirty paths — low value "
                        "= starvation risk), shared-mutex-contention (# of shared_mutex "
                        "contention events on workspace primitives under heavy AI load — "
                        "high value = bottleneck signal). Pairs with the existing #757 "
                        "+ #758 + #759 + #760 + #761 4-field observability hashes but "
                        "tracks the *Workspace closed-loop orchestration* specifically "
                        "as separate per-decision-point counters. #762 exposes the "
                        "Workspace closed-loop production health the Agent consumes to "
                        "decide whether to spawn more agents, add yield points, or "
                        "trigger multi-Agent SLO breach under Guard commit.",
                 .category = "general",
                 .schema = "() -> hash"});
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
    ev.primitives_.add(
        "query:linear-ownership-gc-compiler-stats",
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
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .doc = "Runtime linear_ownership_state enforcement + GC root "
                        "registration observability for IRClosure/EnvFrame in "
                        "invalidate_function and live-closure paths: "
                        "root-registrations (# of compiler IRClosure / EnvFrame "
                        "root registrations called from invalidate + fiber "
                        "mutation boundary — proxy for invalidate + boundary "
                        "GC-safety coverage), root-stale-hits (# of stale GC root "
                        "hits during GC walk from previously invalidated functions "
                        "— high value = UAF risk signal), violations-prevented "
                        "(# of runtime linear violations caught by the runtime "
                        "check in Apply / MakeClosure paths — high value = safety "
                        "net firings), env-version-resync (# of Env version "
                        "re-syncs on invalidate — proxy for invalidate path "
                        "correctly bumping version_ on bridged EnvFrames to keep "
                        "GC walk safe). Pairs with the existing #757 + #758 + #759 "
                        "+ #760 + #761 + #762 4-field observability hashes and "
                        "the existing query:linear-ownership-gc-stats GC layer "
                        "observability, but tracks the *compiler IRClosure + "
                        "EnvFrame + invalidate runtime linear enforcement "
                        "composite* specifically as separate per-decision-point "
                        "counters. #763 exposes the linear ownership + GC + "
                        "compiler maturation production health the Agent consumes "
                        "to decide whether to force re-emit, register GC roots, "
                        "or trigger linear runtime check under invalidate path.",
                 .category = "general",
                 .schema = "() -> hash"});
}

} // namespace aura::compiler::primitives_detail
