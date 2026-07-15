// evaluator_primitives_obs_eval_04.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 32 (orig lines 4537-4603)
void ObservabilityPrims::register_eval_p32(PrimRegistrar add, Evaluator& ev) {

    // Issue #599: query:compiler-root-stats — automatic epoch/version root
    // management for live IRClosure/EnvFrame post-invalidate_function
    // (non-duplicative with #531 closure-env-safety-stats,
    // #598 linear-ownership-runtime-stats, #682 GC root coordination).
    //
    // Fields (4 + sentinel):
    //   - stale-closure-detected   compiler_root_stale_closure_detected_total
    //   - env-version-mismatch     compiler_root_env_version_mismatch_total
    //   - root-refresh-count       closure_stale_refresh_count_
    //   - dangling-prevented       compiler_root_dangling_prevented_total
    //   - schema == 599
    ObservabilityPrims::register_stats_impl(
        "query:compiler-root-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t stale_closure =
                m ? static_cast<std::int64_t>(m->compiler_root_stale_closure_detected_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t env_mismatch =
                m ? static_cast<std::int64_t>(
                        m->compiler_root_env_version_mismatch_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t root_refresh =
                m ? static_cast<std::int64_t>(
                        m->closure_stale_refresh_count_.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t dangling_prevented =
                m ? static_cast<std::int64_t>(
                        m->compiler_root_dangling_prevented_total.load(std::memory_order_relaxed))
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
            insert_kv("stale-closure-detected", stale_closure);
            insert_kv("env-version-mismatch", env_mismatch);
            insert_kv("root-refresh-count", root_refresh);
            insert_kv("dangling-prevented", dangling_prevented);
            insert_kv("schema", 599);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 33 (orig lines 4604-4670)
void ObservabilityPrims::register_eval_p33(PrimRegistrar add, Evaluator& ev) {

    // Issue #600: query:incremental-closure-stats — per-block dirty + impact
    // scope + closure bridge synergy for minimal re-lower
    // (non-duplicative with #530 incremental-production-relower-stats,
    // #429 soa-dirty-stats, #531 closure-env-safety-stats).
    //
    // Fields (4 + sentinel):
    //   - blocks-relowered     incremental_closure_blocks_relowered_total
    //   - closure-bridge-hits  bridge_epoch_hit_count_
    //   - min-scope-win        incremental_closure_min_scope_win_total
    //   - jit-sync-count       incremental_closure_jit_sync_total
    //   - schema == 600
    ObservabilityPrims::register_stats_impl(
        "query:incremental-closure-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t blocks_relowered =
                m ? static_cast<std::int64_t>(m->incremental_closure_blocks_relowered_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t bridge_hits =
                m ? static_cast<std::int64_t>(
                        m->bridge_epoch_hit_count_.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t min_scope_win =
                m ? static_cast<std::int64_t>(
                        m->incremental_closure_min_scope_win_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t jit_sync =
                m ? static_cast<std::int64_t>(
                        m->incremental_closure_jit_sync_total.load(std::memory_order_relaxed))
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
            insert_kv("blocks-relowered", blocks_relowered);
            insert_kv("closure-bridge-hits", bridge_hits);
            insert_kv("min-scope-win", min_scope_win);
            insert_kv("jit-sync-count", jit_sync);
            insert_kv("schema", 600);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 34 (orig lines 4671-4733)
void ObservabilityPrims::register_eval_p34(PrimRegistrar add, Evaluator& ev) {

    // Issue #741: query:incremental-closure-bridge-stats — impact_scope
    // propagation to closure_bridge shared_ptr lifetime + EnvFrame version
    // re-stamp for quote/lambda-heavy defines (non-duplicative with #718
    // block_dirty, #719 epoch/bridge general safety).
    //
    // Fields (3 + sentinel):
    //   - impact-blocks-on-bridge  incremental_closure_bridge_impact_blocks_total
    //   - quote-lambda-stale-prevented
    //                              incremental_closure_quote_lambda_stale_prevented_total
    //   - env-version-resync       incremental_closure_env_version_resync_total
    //   - schema == 741
    ObservabilityPrims::register_stats_impl(
        "query:incremental-closure-bridge-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t impact_on_bridge =
                m ? static_cast<std::int64_t>(
                        m->incremental_closure_bridge_impact_blocks_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t quote_lambda_prevented =
                m ? static_cast<std::int64_t>(
                        m->incremental_closure_quote_lambda_stale_prevented_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t env_resync =
                m ? static_cast<std::int64_t>(m->incremental_closure_env_version_resync_total.load(
                        std::memory_order_relaxed))
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
            insert_kv("impact-blocks-on-bridge", impact_on_bridge);
            insert_kv("quote-lambda-stale-prevented", quote_lambda_prevented);
            insert_kv("env-version-resync", env_resync);
            insert_kv("schema", 741);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 35 (orig lines 4734-4806)
void ObservabilityPrims::register_eval_p35(PrimRegistrar add, Evaluator& ev) {

    // Issue #654: query:macro-hygiene-fiber-panic-stats — 5 cross-cutting
    // macro+reflect+self-evo hygiene gaps vs fiber/panic/AOT/SoA runtime
    // (non-duplicative with #593 pattern-ir-hygiene-closed-loop,
    // #596 guard-panic-reflect, #597 macro-reflect-self-evo-stats).
    //
    // Fields (5 + sentinel):
    //   - hygiene-panic-restamp      macro_hygiene_panic_restamp_total
    //   - provenance-violations      macro_hygiene_provenance_violations_total
    //   - macro-expand-checkpoints   macro_expand_checkpoint_saves_total
    //   - reflect-hygiene-validation macro_reflect_hygiene_validation_total
    //   - hygiene-dirty-impact       macro_hygiene_dirty_impact_total
    //   - schema == 654
    ObservabilityPrims::register_stats_impl(
        "query:macro-hygiene-fiber-panic-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t panic_restamp =
                m ? static_cast<std::int64_t>(
                        m->macro_hygiene_panic_restamp_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t provenance_violations =
                m ? static_cast<std::int64_t>(m->macro_hygiene_provenance_violations_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t expand_checkpoints =
                m ? static_cast<std::int64_t>(
                        m->macro_expand_checkpoint_saves_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t reflect_validation =
                m ? static_cast<std::int64_t>(
                        m->macro_reflect_hygiene_validation_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t dirty_impact =
                m ? static_cast<std::int64_t>(
                        m->macro_hygiene_dirty_impact_total.load(std::memory_order_relaxed))
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
            insert_kv("hygiene-panic-restamp", panic_restamp);
            insert_kv("provenance-violations", provenance_violations);
            insert_kv("macro-expand-checkpoints", expand_checkpoints);
            insert_kv("reflect-hygiene-validation", reflect_validation);
            insert_kv("hygiene-dirty-impact", dirty_impact);
            insert_kv("schema", 654);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 36 (orig lines 4807-4918)
void ObservabilityPrims::register_eval_p36(PrimRegistrar add, Evaluator& ev) {

    // Issue #712: (query:macro-reflect-validation-stats) — subtree-level
    // reflect validation counters (non-duplicative with #654 which
    // folds reflect-hygiene-validation into macro-hygiene-fiber-panic-
    // stats as one of 5 fields, and with #488 which tracks whole-
    // workspace schema_validation_pass_count / fail_count).
    //
    // Fields (4 + sentinel):
    //   - validation-calls         calls to subtree-level auto_validate
    //                              for MacroIntroduced nodes during
    //                              post_mutation_reflect_validate (==1
    //                              per mutation cycle that touched any
    //                              macro subtree)
    //   - schema-mismatches-caught # of MacroIntroduced nodes whose
    //                              macro_dirty bit is missing the
    //                              kMacroExpansion flag during the
    //                              post-mutate reflect scan
    //   - post-mutate-hygiene-drift # of MacroIntroduced nodes that are
    //                              also dirty in the post-mutate snapshot
    //                              (macro subtree was re-expanded or
    //                              re-written between commits — the
    //                              Agent uses this counter to decide
    //                              whether to deep-validate that subtree
    //                              before trusting it)
    //   - schema-pass              reflects from schema_validation_pass_count_
    //                              (whole-workspace pass counter); lets
    //                              the Agent correlate subtree-level
    //                              diagnostics with workspace-level
    //                              validation outcomes
    //   - schema == 712
    // Issue #712: routes through ev.primitives_.add (3-arg form) so
    // we can attach PrimMeta with schema=712 + category=general +
    // arity=0 + pure=true. The local PrimRegistrar typedef at the
    // top of this file is intentionally 2-arg (matches pre-#669
    // baseline). Other #669/#671 primitives that don't carry PrimMeta
    // use the 2-arg add() directly.
    ObservabilityPrims::register_stats_impl(
        "query:macro-reflect-validation-stats", [&ev](const auto&) -> EvalValue {
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
            const std::int64_t validation_calls =
                m ? static_cast<std::int64_t>(
                        m->macro_reflect_validation_calls_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t schema_mismatches =
                m ? static_cast<std::int64_t>(m->macro_reflect_schema_mismatches_caught_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t hygiene_drift =
                m ? static_cast<std::int64_t>(m->macro_reflect_post_mutate_hygiene_drift_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t schema_pass =
                static_cast<std::int64_t>(ev.get_schema_validation_pass_count());
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"validation-calls", make_int(validation_calls)},
                {"schema-mismatches-caught", make_int(schema_mismatches)},
                {"post-mutate-hygiene-drift", make_int(hygiene_drift)},
                {"schema-pass", make_int(schema_pass)},
                {"schema", make_int(712)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 37 (orig lines 4919-5023)
void ObservabilityPrims::register_eval_p37(PrimRegistrar add, Evaluator& ev) {

    // Issue #713: (query:macro-jit-hygiene-stats) — JIT/AOT/Interpreter
    // macro-hygiene violation counters (non-duplicative with #488
    // schema_validation_pass/fail, #654 macro-hygiene-fiber-panic-
    // stats, #712 macro-reflect-validation-stats).
    //
    // Fields (4 + sentinel):
    //   - deopt-on-hygiene            macro_jit_hygiene_deopt_total
    //                                (# of JIT deopts triggered by a
    //                                 source_marker=MacroIntroduced
    //                                 call site trying to inline into
    //                                 User code or other policy
    //                                 violation)
    //   - aot-reload-marker-mismatches
    //                                macro_aot_reload_marker_mismatches_total
    //                                (# of AOT reloads that re-stamped
    //                                 or rejected a module because its
    //                                 source_marker column drifted
    //                                 from the host's expected markers)
    //   - interpreter-fallback-hygiene-hits
    //                                macro_interpreter_fallback_hygiene_hits_total
    //                                (# of IR executor dispatches that
    //                                 hit a source_marker=MacroIntroduced
    //                                 call site + chose conservative
    //                                 interpreter fallback over JIT'd
    //                                 inlined code)
    //   - schema == 713
    //
    // All three counters are 0 on a fresh service. The bump helpers
    // are exposed via Evaluator::bump_macro_jit_hygiene_deopt()
    // etc. for future hot-path wiring (each JIT/AOT/Interpreter
    // hook is a dedicated follow-up).
    ObservabilityPrims::register_stats_impl(
        "query:macro-jit-hygiene-stats", [&ev](const auto&) -> EvalValue {
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
            const std::int64_t deopt_on_hygiene =
                m ? static_cast<std::int64_t>(
                        m->macro_jit_hygiene_deopt_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t aot_reload_mismatches =
                m ? static_cast<std::int64_t>(
                        m->macro_aot_reload_marker_mismatches_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t interpreter_fallback =
                m ? static_cast<std::int64_t>(m->macro_interpreter_fallback_hygiene_hits_total.load(
                        std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"deopt-on-hygiene", make_int(deopt_on_hygiene)},
                {"aot-reload-marker-mismatches", make_int(aot_reload_mismatches)},
                {"interpreter-fallback-hygiene-hits", make_int(interpreter_fallback)},
                {"schema", make_int(713)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 38 (orig lines 5024-5197)
void ObservabilityPrims::register_eval_p38(PrimRegistrar add, Evaluator& ev) {

    // Issue #714: (query:self-evolution-closedloop-stats) — unified
    // self-evolution observability primitive that correlates hygiene
    // (MacroIntroduced count, violation rate), dirty impact (subtree
    // affected), epoch drift (panic restamp proxy), reflect-validation
    // pass rate, and mutation strategy recommendation counts into a
    // single Agent-facing report + recommended_mutation_strategy string.
    //
    // (Non-duplicative with #654 macro-hygiene-fiber-panic-stats,
    // #712 macro-reflect-validation-stats, #713 macro-jit-hygiene-
    // stats, #488 schema-validation, #622 atomic-batch. #714 is the
    // FIRST primitive that ties these signals together for an Agent
    // to decide mutation strategy in a closed-loop self-evolution
    // loop.)
    //
    // Fields (8 + sentinel):
    //   - hygiene-macro-introduced-count   # of SyntaxMarker=MacroIntroduced
    //                                      nodes in the current workspace
    //                                      (0 on a fresh service; non-zero
    //                                      requires a macro expansion walk —
    //                                      exposed as 0 in Phase 1)
    //   - hygiene-violation-rate           #violations / #macro_introduced
    //                                      (scaled 0..1e6 to keep integer
    //                                      math; 0 on a fresh service)
    //   - dirty-subtree-impact             macro_hygiene_dirty_impact_total
    //                                      (# of nodes that are BOTH dirty
    //                                      AND macro-introduced — feeds the
    //                                      "should I deep-validate this
    //                                      subtree" decision)
    //   - epoch-drift-detected             macro_hygiene_panic_restamp_total
    //                                      (re-used as epoch drift proxy:
    //                                      every panic restamp signals an
    //                                      epoch boundary that may have
    //                                      invalidated prior Agent decisions)
    //   - reflect-validation-pass-rate     schema_validation_pass_count /
    //                                      (schema_validation_pass_count +
    //                                      schema_validation_fail_count + 1)
    //                                      (scaled 0..1e6)
    //   - recommended-mutation-strategy    "safe" / "aggressive" / "balanced"
    //                                      — derived from the highest of
    //                                      the three strategy recommendation
    //                                      counters; default "balanced"
    //   - strategy-safe-count              self_evo_strategy_recommend_safe_total
    //   - strategy-aggressive-count        self_evo_strategy_recommend_aggressive_total
    //   - strategy-balanced-count          self_evo_strategy_recommend_balanced_total
    //   - schema == 714 (drift sentinel)
    //
    // Phase 1 ships the primitive + counters + derivation logic. The
    // Guard dtor + mark_dirty_upward + reflect auto_validate hooks
    // that bump the strategy counters at each decision point are
    // follow-up work (each hook is a dedicated session). Mutation
    // strategy hook primitives `(mutate:strategy-safe)` /
    // `(mutate:strategy-aggressive)` and the correlation primitive
    // `query:self-evo-impact-correlation (hygiene_vs_dirty,
    // epoch_vs_success_rate)` are also follow-ups.
    //
    // Issue #714: routes through ev.primitives_.add (3-arg form) so
    // we can attach PrimMeta with schema=714 + category=general +
    // arity=0 + pure=true (same pattern as #712/#713).
    ObservabilityPrims::register_stats_impl(
        "query:self-evolution-closedloop-stats", [&ev](const auto&) -> EvalValue {
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
            const std::int64_t macro_introduced_count = 0; // Phase 1 stub — walk is follow-up
            const std::int64_t hygiene_violation_rate =
                0; // Phase 1 stub — derived from violations / total
            const std::int64_t dirty_subtree_impact =
                m ? static_cast<std::int64_t>(
                        m->macro_hygiene_dirty_impact_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t epoch_drift_detected =
                m ? static_cast<std::int64_t>(
                        m->macro_hygiene_panic_restamp_total.load(std::memory_order_relaxed))
                  : 0;
            const std::uint64_t pass = ev.get_schema_validation_pass_count();
            const std::uint64_t fail = ev.get_schema_validation_fail_count();
            // reflect-validation-pass-rate scaled 0..1e6 (1.0 == 1e6)
            const std::int64_t reflect_pass_rate =
                static_cast<std::int64_t>((pass * 1000000ULL) / (pass + fail + 1ULL));
            const std::int64_t strategy_safe =
                m ? static_cast<std::int64_t>(
                        m->self_evo_strategy_recommend_safe_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t strategy_aggressive =
                m ? static_cast<std::int64_t>(m->self_evo_strategy_recommend_aggressive_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t strategy_balanced =
                m ? static_cast<std::int64_t>(m->self_evo_strategy_recommend_balanced_total.load(
                        std::memory_order_relaxed))
                  : 0;
            // recommended_mutation_strategy: highest-count wins; ties go balanced (the
            // safe default). Phase 2 hook will override this with hygiene-aware logic.
            std::string recommended_strategy = "balanced";
            const std::int64_t max_count =
                std::max({strategy_safe, strategy_aggressive, strategy_balanced});
            if (max_count > 0) {
                if (strategy_safe == max_count && strategy_aggressive != max_count &&
                    strategy_balanced != max_count) {
                    recommended_strategy = "safe";
                } else if (strategy_aggressive == max_count && strategy_safe != max_count &&
                           strategy_balanced != max_count) {
                    recommended_strategy = "aggressive";
                } // else balanced (ties)
            }
            // Intern the strategy string in the evaluator's string heap
            // so make_string returns a stable handle.
            const std::uint64_t sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(recommended_strategy);
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"hygiene-macro-introduced-count", make_int(macro_introduced_count)},
                {"hygiene-violation-rate", make_int(hygiene_violation_rate)},
                {"dirty-subtree-impact", make_int(dirty_subtree_impact)},
                {"epoch-drift-detected", make_int(epoch_drift_detected)},
                {"reflect-validation-pass-rate", make_int(reflect_pass_rate)},
                {"recommended-mutation-strategy", make_string(sidx)},
                {"strategy-safe-count", make_int(strategy_safe)},
                {"strategy-aggressive-count", make_int(strategy_aggressive)},
                {"strategy-balanced-count", make_int(strategy_balanced)},
                {"schema", make_int(714)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 39 (orig lines 5198-5312)
void ObservabilityPrims::register_eval_p39(PrimRegistrar add, Evaluator& ev) {

    // Issue #715: (query:stable-ref-layer-stats) — cross-layer
    // StableNodeRef validation counters for WorkspaceTree multi-layer
    // setups (non-duplicative with #191/#255/#368 stable_ref_invalidations_
    // which counts single-layer is_valid() failures only, and with
    // #191/#655/#736 StableNodeRef COW counters which track COW
    // remap mechanics rather than cross-layer validity signals).
    //
    // Fields (3 + sentinel):
    //   - cross-layer-validations    stable_ref_cross_layer_validations_total
    //                                (# of is_valid_in_layer calls that
    //                                 passed: gen + workspace_id +
    //                                 cow_epoch all aligned, OR ref was
    //                                 explicitly pin_for_cow'd across
    //                                 the boundary)
    //   - cross-layer-mismatches     stable_ref_cross_layer_mismatch_total
    //                                (# of is_valid_in_layer calls that
    //                                 returned false: gen drift, workspace_id
    //                                 mismatch, OR cow_epoch advanced past
    //                                 capture without pin_for_cow)
    //   - cow-boundary-pins          stable_ref_cow_boundary_pins_total
    //                                (# of StableNodeRefs that intentionally
    //                                 crossed a COW boundary via pin_for_cow() —
    //                                 "how many refs are intentionally surviving
    //                                 lazy clones" — the Agent uses this to
    //                                 decide whether a checkpoint can be safely
    //                                 merged back to the parent layer)
    //   - schema == 715
    //
    // Phase 1 ships the primitive + counters + the is_valid_in_layer
    // helper on StableNodeRef. The MutationBoundaryGuard auto-remap
    // and workspace-merge hooks that produce these counters are
    // follow-up (each is a dedicated session in evaluator_workspace_
    // tree.cpp / guard_wiring.cpp). The helper itself is allocation-
    // free + pure read so existing single-layer callers can drop in
    // is_valid_in_layer(ast, ref.workspace_id_) without overhead.
    //
    // Issue #715: routes through ev.primitives_.add (3-arg form) so
    // we can attach PrimMeta with schema=715 + category=general +
    // arity=0 + pure=true (same pattern as #712/#713/#714).
    ObservabilityPrims::register_stats_impl(
        "query:stable-ref-layer-stats", [&ev](const auto&) -> EvalValue {
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
            const std::int64_t cross_layer_validations =
                m ? static_cast<std::int64_t>(
                        m->stable_ref_cross_layer_validations_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t cross_layer_mismatches =
                m ? static_cast<std::int64_t>(
                        m->stable_ref_cross_layer_mismatch_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t cow_boundary_pins =
                m ? static_cast<std::int64_t>(
                        m->stable_ref_cow_boundary_pins_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"cross-layer-validations", make_int(cross_layer_validations)},
                {"cross-layer-mismatches", make_int(cross_layer_mismatches)},
                {"cow-boundary-pins", make_int(cow_boundary_pins)},
                {"schema", make_int(715)},
            };
            return build_hash(kv);
        });
}

} // namespace aura::compiler::primitives_detail
