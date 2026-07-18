// evaluator_primitives_obs_eval_05.cpp — Issue #909: peeled domain registration from observability
// monolith aura.compiler.evaluator module partition.

module;

#include "runtime_shared.h"
#include "compiler/aura_jit_bridge.h"
#include "observability_metrics.h"
#include "compiler/shape.h"
#include "compiler/value_tags.h"
#include "core/cpp26_contract_stats.h"
#include "core/arena_auto_policy_stats.h"
#include "core/gc_hooks.h" // #1591 safepoint wait while mutation held
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

// Issue #909 part 40 (orig lines 5313-5422)
void ObservabilityPrims::register_eval_p40(PrimRegistrar add, Evaluator& ev) {

    // Issue #716: (query:pattern-stats) — pattern matcher
    // observability counters (non-duplicative with #547 / #490 /
    // #621 / #654 tag_arity_index_* which track the index itself;
    // #716 tracks the matcher call path + hygiene filter +
    // fast-path promotion as separate signals).
    //
    // Fields (3 + sentinel):
    //   - matcher-calls              pattern_matcher_calls_total
    //                                (# of query:pattern /
    //                                 query:where / query:filter
    //                                 invocations — lifetime)
    //   - macro-intro-filtered       pattern_macro_intro_filtered_total
    //                                (# of AST nodes skipped by
    //                                 is_macro_introduced() during
    //                                 pattern matching — proxy for
    //                                 "how much user-focused noise
    //                                 the matcher avoided")
    //   - fast-path-hits             pattern_fast_path_hits_total
    //                                (# of simple tag+arity queries
    //                                 served from cache without full
    //                                 pattern traversal)
    //   - schema == 716
    //
    // Phase 1 ships the primitive + counters + bump helpers. The
    // actual is_macro_introduced() skip wiring in query_matcher.cpp
    // hot path + the cache promotion + configurable hygiene
    // filter mode (user-focused vs macro-aware) are follow-up
    // (each is a dedicated session in evaluator_primitives_query.cpp
    // + query_matcher.cpp).
    //
    // Issue #716: routes through ev.primitives_.add (3-arg form) so
    // we can attach PrimMeta with schema=716 + category=general +
    // arity=0 + pure=true (same pattern as #712/#713/#714/#715).
    ObservabilityPrims::register_stats_impl("query:pattern-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
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
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t matcher_calls =
            m ? static_cast<std::int64_t>(
                    m->pattern_matcher_calls_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t macro_intro_filtered =
            m ? static_cast<std::int64_t>(
                    m->pattern_macro_intro_filtered_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t fast_path_hits =
            m ? static_cast<std::int64_t>(
                    m->pattern_fast_path_hits_total.load(std::memory_order_relaxed))
              : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"matcher-calls", make_int(matcher_calls)},
            {"macro-intro-filtered", make_int(macro_intro_filtered)},
            {"fast-path-hits", make_int(fast_path_hits)},
            {"schema", make_int(716)},
        };
        return build_hash(kv);
    });
}

// Issue #909 part 41 (orig lines 5423-5535)
void ObservabilityPrims::register_eval_p41(PrimRegistrar add, Evaluator& ev) {

    // Issue #717: (query:fiber-boundary-violation-stats) —
    // fiber-safe MutationBoundaryGuard recovery counters
    // (non-duplicative with #438 query:fiber-migration-stats
    // which tracks steal-attempts / boundary-violations /
    // defer counts from the SCHEDULER side; #717 tracks
    // rollback / resume / recovery-failure counts from the
    // GUARD side — complementary signals).
    //
    // Fields (3 + sentinel):
    //   - rollbacks             mutation_boundary_rollbacks_total
    //                           (# of times the MutationBoundaryGuard
    //                            dtor triggered a rollback — fiber-
    //                            aware epoch bump + dirty clear +
    //                            StableRef remap)
    //   - yield-resumes         mutation_boundary_yield_resumes_total
    //                           (# of times a fiber successfully
    //                            resumed after yielding at a boundary)
    //   - recovery-failures     mutation_boundary_recovery_failures_total
    //                           (# of times recovery FAILED:
    //                            partial dirty state, leaked
    //                            StableRef, defuse_version_ drift
    //                            across resume)
    //   - schema == 717
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual fiber-context check on guard dtor +
    // panic_checkpoint integration with per-fiber mutation_stack_
    // snapshot + targeted multi-fiber "failed mutate + yield +
    // resume" tests are follow-up work (each is a dedicated
    // session in evaluator_fiber_mutation.cpp +
    // evaluator_primitives_mutate.cpp + a new test_issue_717_
    // fiber_recovery.cpp harness).
    //
    // Issue #717: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=717 + category=general
    // + arity=0 + pure=true (same pattern as #712-#716).
    ObservabilityPrims::register_stats_impl(
        "query:fiber-boundary-violation-stats", [&ev](const auto&) -> EvalValue {
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
            const std::int64_t rollbacks =
                m ? static_cast<std::int64_t>(
                        m->mutation_boundary_rollbacks_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t yield_resumes =
                m ? static_cast<std::int64_t>(
                        m->mutation_boundary_yield_resumes_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t recovery_failures =
                m ? static_cast<std::int64_t>(m->mutation_boundary_recovery_failures_total.load(
                        std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"rollbacks", make_int(rollbacks)},
                {"yield-resumes", make_int(yield_resumes)},
                {"recovery-failures", make_int(recovery_failures)},
                {"schema", make_int(717)},
            };
            return build_hash(kv);
        });

    // Issue #1373 / #1375: (query:mutation-boundary-hold-stats) —
    // hold-time + yield/migration + 9-bucket histogram for
    // MutationBoundaryGuard. Complements #717 fiber-boundary-
    // violation-stats and #1253 mutation_hold_* (long-mutation SLO).
    ObservabilityPrims::register_stats_impl(
        "query:mutation-boundary-hold-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                // 16 base fields + 9 histogram buckets → need room
                auto* ht = FlatHashTable::create(64);
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
            auto load = [&](std::atomic<std::uint64_t>& a) -> std::int64_t {
                return m ? static_cast<std::int64_t>(a.load(std::memory_order_relaxed)) : 0;
            };
            const std::int64_t holds = m ? load(m->mutation_boundary_holds_total) : 0;
            const std::int64_t hold_us = m ? load(m->mutation_boundary_hold_time_total_us) : 0;
            const std::int64_t avg = holds > 0 ? static_cast<std::int64_t>(hold_us / holds) : 0;
            // Issue #1375: sum of histogram for integrity check.
            std::int64_t hist_sum = 0;
            std::array<std::int64_t, CompilerMetrics::kMutationBoundaryHoldHistBuckets> hist{};
            if (m) {
                for (std::size_t i = 0; i < CompilerMetrics::kMutationBoundaryHoldHistBuckets;
                     ++i) {
                    hist[i] = load(m->mutation_boundary_hold_histogram[i]);
                    hist_sum += hist[i];
                }
            }
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"same-thread-yield",
                 make_int(m ? load(m->mutation_boundary_yield_same_thread_total) : 0)},
                {"cross-thread-migration",
                 make_int(m ? load(m->mutation_boundary_cross_thread_migration_total) : 0)},
                {"yield-rollback",
                 make_int(m ? load(m->mutation_boundary_yield_rollback_total) : 0)},
                {"hold-time-us-total", make_int(hold_us)},
                {"total-us", make_int(hold_us)}, // #1375 alias
                {"holds-total", make_int(holds)},
                {"holds-over-1ms",
                 make_int(m ? load(m->mutation_boundary_holds_over_1ms_total) : 0)},
                {"over-1ms", make_int(m ? load(m->mutation_boundary_holds_over_1ms_total) : 0)},
                {"avg-hold-us", make_int(avg)},
                {"avg-us", make_int(avg)},
                {"held-now", make_int(ev.mutation_boundary_held() ? 1 : 0)},
                // Issue #1375: 9-bucket histogram keys (see bucket-labels)
                {"hist-0-100us", make_int(hist[0])},
                {"hist-100-500us", make_int(hist[1])},
                {"hist-500us-1ms", make_int(hist[2])},
                {"hist-1-5ms", make_int(hist[3])},
                {"hist-5-10ms", make_int(hist[4])},
                {"hist-10-50ms", make_int(hist[5])},
                {"hist-50-100ms", make_int(hist[6])},
                {"hist-100ms-1s", make_int(hist[7])},
                {"hist-gt-1s", make_int(hist[8])},
                {"hist-sum", make_int(hist_sum)},
                {"hist-buckets", make_int(static_cast<std::int64_t>(
                                     CompilerMetrics::kMutationBoundaryHoldHistBuckets))},
                {"schema", make_int(1375)},
            };
            return build_hash(kv);
        });

    // Issue #1504: (query:mutation-boundary-depth) — current Guard
    // nesting depth for Agent orchestration (0 = steal-safe / yield-safe).
    ObservabilityPrims::register_stats_impl(
        "query:mutation-boundary-depth", [&ev](const auto&) -> EvalValue {
            return make_int(static_cast<std::int64_t>(ev.mutation_boundary_depth()));
        });

    // Shared builder for safe-yield action surfaces (#1504 / #1591).
    auto build_safe_yield_hash = [&ev](int rc) -> EvalValue {
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
            auto kidx = ev.string_heap_.size();
            ev.string_heap_.push_back(k_str);
            for (std::size_t at = 0; at < hcap; ++at) {
                auto slot = ((h >> 1) + at) & (hcap - 1);
                if (meta[slot] == 0xFF) {
                    meta[slot] = fp;
                    keys[slot] = make_string(kidx).val;
                    vals[slot] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        const bool yielded = (rc == 0);
        const bool skipped = (rc == 1);
        insert_kv("yielded", yielded ? 1 : 0);
        insert_kv("skipped-held", skipped ? 1 : 0);
        insert_kv("boundary-depth", static_cast<std::int64_t>(ev.mutation_boundary_depth()));
        insert_kv("depth-slot", static_cast<std::int64_t>(ev.mutation_boundary_depth_slot_value()));
        insert_kv("held-now", ev.mutation_boundary_held() ? 1 : 0);
        insert_kv("safe-yield-ok-total", static_cast<std::int64_t>(ev.get_safe_yield_ok_total()));
        insert_kv("safe-yield-skipped-held-total",
                  static_cast<std::int64_t>(ev.get_safe_yield_skipped_held_total()));
        insert_kv("safe-yield-no-fiber-total",
                  static_cast<std::int64_t>(ev.get_safe_yield_no_fiber_total()));
        // Issue #1591: fairness fields Agents use for back-off / interleave.
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const std::int64_t hold_total =
            m ? static_cast<std::int64_t>(
                    m->mutation_boundary_hold_time_total_us.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t holds =
            m ? static_cast<std::int64_t>(
                    m->mutation_boundary_holds_total.load(std::memory_order_relaxed))
              : 0;
        insert_kv("avg-hold-time-us", holds > 0 ? hold_total / holds : 0);
        insert_kv(
            "safepoint-wait-while-mutation-held-us",
            static_cast<std::int64_t>(aura::gc_hooks::safepoint_wait_while_mutation_held_us()));
        insert_kv("per-fiber-stack-depth-max",
                  static_cast<std::int64_t>(ev.get_per_fiber_mutation_stack_depth_max()));
        {
            auto& s = ::aura::serve::metrics::adaptive_steal_stats();
            insert_kv(
                "steal-inner-deferred-starvation-mitigated-count",
                static_cast<std::int64_t>(s.steal_inner_deferred_starvation_mitigated_count.load(
                    std::memory_order_relaxed)));
        }
        insert_kv("issue", 1591);
        insert_kv("schema", 1591); // #1591 supersedes 1504; Agents accept both
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    };

    // Issue #1504: (query:mutation-boundary-safe-yield) — attempt cooperative
    // yield only at a safe point (depth==0). Side-effecting metrics surface.
    ObservabilityPrims::register_stats_impl(
        "query:mutation-boundary-safe-yield",
        [&ev, build_safe_yield_hash](const auto& a) -> EvalValue {
            std::int64_t timeout_ms = 0;
            if (!a.empty() && is_int(a[0]))
                timeout_ms = as_int(a[0]);
            const int rc = ev.try_safe_yield_at_boundary(timeout_ms);
            return build_safe_yield_hash(rc);
        });

    // Issue #1504: (ast:yield-at-boundary [timeout-ms]) — alias for Agents
    // that prefer the ast: namespace (same contract as safe-yield above).
    ObservabilityPrims::register_stats_impl(
        "ast:yield-at-boundary", [&ev, build_safe_yield_hash](const auto& a) -> EvalValue {
            std::int64_t timeout_ms = 0;
            if (!a.empty() && is_int(a[0]))
                timeout_ms = as_int(a[0]);
            const int rc = ev.try_safe_yield_at_boundary(timeout_ms);
            return build_safe_yield_hash(rc);
        });

    // Issue #1504: lifetime counters + depth instrumentation (read-only).
    ObservabilityPrims::register_stats_impl(
        "query:mutation-boundary-safe-yield-stats", [&ev](const auto&) -> EvalValue {
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
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k_str);
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto slot = ((h >> 1) + at) & (hcap - 1);
                    if (meta[slot] == 0xFF) {
                        meta[slot] = fp;
                        keys[slot] = make_string(kidx).val;
                        vals[slot] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("boundary-depth", static_cast<std::int64_t>(ev.mutation_boundary_depth()));
            insert_kv("depth-slot",
                      static_cast<std::int64_t>(ev.mutation_boundary_depth_slot_value()));
            insert_kv("nested-guard-depth-max",
                      static_cast<std::int64_t>(ev.nested_guard_depth_max()));
            insert_kv("per-fiber-stack-depth-max",
                      static_cast<std::int64_t>(ev.get_per_fiber_mutation_stack_depth_max()));
            insert_kv("held-now", ev.mutation_boundary_held() ? 1 : 0);
            insert_kv("safe-yield-ok-total",
                      static_cast<std::int64_t>(ev.get_safe_yield_ok_total()));
            insert_kv("safe-yield-skipped-held-total",
                      static_cast<std::int64_t>(ev.get_safe_yield_skipped_held_total()));
            insert_kv("safe-yield-no-fiber-total",
                      static_cast<std::int64_t>(ev.get_safe_yield_no_fiber_total()));
            // Issue #1591: hold + safepoint wait for orchestration fairness.
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            const std::int64_t hold_total =
                m ? static_cast<std::int64_t>(
                        m->mutation_boundary_hold_time_total_us.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t holds =
                m ? static_cast<std::int64_t>(
                        m->mutation_boundary_holds_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("avg-hold-time-us", holds > 0 ? hold_total / holds : 0);
            insert_kv(
                "safepoint-wait-while-mutation-held-us",
                static_cast<std::int64_t>(aura::gc_hooks::safepoint_wait_while_mutation_held_us()));
            insert_kv("issue", 1591);
            insert_kv("schema", 1591);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1591: unified fairness dashboard (safe-yield + per-fiber depth +
    // steal starvation + safepoint wait). One hash for multi-Agent orchestrators.
    ObservabilityPrims::register_stats_impl(
        "query:mutation-boundary-fairness-stats", [&ev](const auto&) -> EvalValue {
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
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k_str);
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto slot = ((h >> 1) + at) & (hcap - 1);
                    if (meta[slot] == 0xFF) {
                        meta[slot] = fp;
                        keys[slot] = make_string(kidx).val;
                        vals[slot] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            auto& s = ::aura::serve::metrics::adaptive_steal_stats();
            insert_kv("boundary-depth", static_cast<std::int64_t>(ev.mutation_boundary_depth()));
            insert_kv("held-now", ev.mutation_boundary_held() ? 1 : 0);
            insert_kv("per-fiber-stack-depth-max",
                      static_cast<std::int64_t>(ev.get_per_fiber_mutation_stack_depth_max()));
            insert_kv(
                "per-fiber-stack-depth-current-max",
                static_cast<std::int64_t>(ev.get_per_fiber_mutation_stack_depth_current_max()));
            insert_kv("safe-yield-ok-total",
                      static_cast<std::int64_t>(ev.get_safe_yield_ok_total()));
            insert_kv("safe-yield-skipped-held-total",
                      static_cast<std::int64_t>(ev.get_safe_yield_skipped_held_total()));
            const std::int64_t hold_total =
                m ? static_cast<std::int64_t>(
                        m->mutation_boundary_hold_time_total_us.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t holds =
                m ? static_cast<std::int64_t>(
                        m->mutation_boundary_holds_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("avg-hold-time-us", holds > 0 ? hold_total / holds : 0);
            insert_kv("hold-samples", holds);
            insert_kv(
                "safepoint-wait-while-mutation-held-us",
                static_cast<std::int64_t>(aura::gc_hooks::safepoint_wait_while_mutation_held_us()));
            insert_kv("safepoint-wait-while-mutation-held-count",
                      static_cast<std::int64_t>(
                          aura::gc_hooks::safepoint_wait_while_mutation_held_count()));
            insert_kv(
                "steal-inner-deferred-starvation-mitigated-count",
                static_cast<std::int64_t>(s.steal_inner_deferred_starvation_mitigated_count.load(
                    std::memory_order_relaxed)));
            insert_kv("steal-deferred-inner-boundary",
                      static_cast<std::int64_t>(
                          s.steal_deferred_inner_boundary.load(std::memory_order_relaxed)));
            insert_kv("starvation-mitigated-count",
                      static_cast<std::int64_t>(
                          s.starvation_mitigated_count.load(std::memory_order_relaxed)));
            std::int64_t hist_total = 0;
            if (m) {
                for (std::size_t i = 0; i < CompilerMetrics::kMutationStackDepthHistBuckets; ++i)
                    hist_total += static_cast<std::int64_t>(
                        m->mutation_stack_depth_histogram[i].load(std::memory_order_relaxed));
            }
            insert_kv("mutation-stack-depth-histogram-samples", hist_total);
            insert_kv("issue", 1591);
            insert_kv("schema", 1591);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 42 (orig lines 5536-5657)
void ObservabilityPrims::register_eval_p42(PrimRegistrar add, Evaluator& ev) {

    // Issue #718: (query:incremental-relower-stats) — fine-grained
    // per-block re-lower observability counters (non-duplicative
    // with #196 per-block dirty tracking + #426/#460 pure helpers
    // + #687 DeadCoercionEliminationPass; #718 is the FIRST
    // observability surface that exposes the partial-vs-full
    // re-lower decision outcomes as separate signals).
    //
    // Fields (4 + sentinel):
    //   - impact-blocks-hit      incremental_impact_blocks_hit_total
    //                            (# of times compute_impact_scope
    //                             returned >=1 affected block for a
    //                             mutate:rebind / set-body request)
    //   - partial-relowers       incremental_partial_relower_total
    //                            (# of times should_partial_relower
    //                             returned true (1..7 dirty blocks)
    //                             and the pipeline took the partial
    //                             path)
    //   - full-fallbacks         incremental_full_fallback_total
    //                            (# of times the pipeline took the
    //                             FULL re-lower path — 8+ dirty
    //                             blocks or no impact_scope data)
    //   - time-saved-us          incremental_time_saved_us_total
    //                            (cumulative time saved in microseconds
    //                             by choosing partial over full re-lower)
    //   - schema == 718
    //
    // Phase 1 ships the primitive + counters + bump helpers + the
    // pure should_partial_relower helper in ir_cache_pure.ixx.
    // The actual compute_impact_scope call + block_dirty_ bit
    // setting inside service.ixx::invalidate_function + the
    // partial re-lower decision in lowering_impl.cpp::lower_to_ir_
    // with_cache + the pass_manager.ixx::run_incremental_pipeline
    // short-circuit are follow-up work (each is a dedicated
    // session).
    //
    // Issue #718: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=718 + category=general
    // + arity=0 + pure=true (same pattern as #712-#717).
    ObservabilityPrims::register_stats_impl(
        "query:incremental-relower-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(64); // #1601 / #1623 more keys
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
            const std::int64_t impact_blocks_hit =
                m ? static_cast<std::int64_t>(
                        m->incremental_impact_blocks_hit_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t partial_relowers =
                m ? static_cast<std::int64_t>(
                        m->incremental_partial_relower_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t full_fallbacks =
                m ? static_cast<std::int64_t>(
                        m->incremental_full_fallback_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t time_saved_us =
                m ? static_cast<std::int64_t>(
                        m->incremental_time_saved_us_total.load(std::memory_order_relaxed))
                  : 0;
            // Issue #1601 / #1605 / #1623: production consumer metrics
            // (eval/eval_ir/define_function prefer partial re-lower).
            const std::int64_t incr_blocks =
                m ? static_cast<std::int64_t>(
                        m->incremental_relower_blocks_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t per_fn =
                m ? static_cast<std::int64_t>(
                        m->relower_per_function_called_count.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t skipped =
                m ? static_cast<std::int64_t>(
                        m->relower_skipped_entirely_count.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t full_called =
                m ? static_cast<std::int64_t>(
                        m->relower_full_called_count.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t dirty_hits =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_block_dirty_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t blocks_saved =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_relower_blocks_saved_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t dirty_ratio_bp =
                (dirty_hits + blocks_saved) > 0
                    ? static_cast<std::int64_t>((dirty_hits * 10000) / (dirty_hits + blocks_saved))
                    : 0;
            // #1623: EDSL eval hot-path partial re-lower AC counters
            const std::int64_t eval_hits =
                m ? static_cast<std::int64_t>(
                        m->incremental_eval_relower_hits.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t eval_path =
                m ? static_cast<std::int64_t>(
                        m->eval_path_relower_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t eval_ir_path =
                m ? static_cast<std::int64_t>(
                        m->eval_ir_path_relower_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"impact-blocks-hit", make_int(impact_blocks_hit)},
                {"partial-relowers", make_int(partial_relowers)},
                {"full-fallbacks", make_int(full_fallbacks)},
                {"time-saved-us", make_int(time_saved_us)},
                // #1601 / #1605 AC names (underscore form for agents)
                {"incremental_relower_blocks", make_int(incr_blocks)},
                {"relower_per_function_called_count", make_int(per_fn)},
                {"relower_skipped_entirely_count", make_int(skipped)},
                {"relower_full_called_count", make_int(full_called)},
                // #1605 AC3 alias (snapshot field name)
                {"full_relower_count", make_int(full_called)},
                {"dirty_block_ratio", make_int(dirty_ratio_bp)}, // basis points
                {"dirty_block_ratio_bp", make_int(dirty_ratio_bp)},
                // #1623 AC keys — eval/eval_ir hot-path partial wins
                {"incremental_eval_relower_hits", make_int(eval_hits)},
                {"eval_path_relower_total", make_int(eval_path)},
                {"eval_ir_path_relower_total", make_int(eval_ir_path)},
                {"eval-prefer-partial-wired", make_int(1)},
                {"eval-ir-prefer-partial-wired", make_int(1)},
                {"relower-only-dirty-blocks-wired", make_int(1)},
                {"relower-define-blocks-wired", make_int(1)},
                {"lookup-define-v2-prefer-partial", make_int(1)},
                {"issue", make_int(1623)},
                {"schema", make_int(1623)}, // lineage 1605 / 1601 / 1506 / 718
            };
            return build_hash(kv);
        });
}

// Issue #909 part 43 (orig lines 5658-5790)
void ObservabilityPrims::register_eval_p43(PrimRegistrar add, Evaluator& ev) {

    // Issue #719: (query:closure-env-epoch-safety-stats) —
    // Prompt 6 closure/EnvFrame epoch + linear ownership + GC
    // root sync runtime safety counters (non-duplicative with
    // #672 linear_stats which tracks compile-time linear type
    // errors, and #681 epoch enforcement which is IR-level
    // metadata; #719 is the FIRST observability surface that
    // tracks runtime closure/EnvFrame/linear/GC safety outcomes
    // in apply_closure and JIT hot paths as separate signals).
    //
    // Fields (4 + sentinel):
    //   - epoch-mismatches-caught     closure_epoch_mismatch_total
    //                                 (# of times apply_closure
    //                                  detected a stale bridge_epoch
    //                                  before dispatching to map /
    //                                  bridge path)
    //   - linear-violations-post-mutate
    //                                 linear_violation_post_mutate_total
    //                                 (# of times GuardShape /
    //                                  Linear* op handler /
    //                                  JIT PrimCall/Capture
    //                                  detected a linear
    //                                  ownership_state != 0
    //                                  with epoch/version
    //                                  mismatch post-mutate)
    //   - gc-root-syncs               gc_root_sync_total
    //                                 (# of ScopedCompilerRoot
    //                                  register/unregister
    //                                  syncs triggered from
    //                                  invalidate_function /
    //                                  MutationBoundaryGuard dtor)
    //   - dangling-prevented          dangling_prevented_total
    //                                 (# of times a UAF /
    //                                  dangling situation was
    //                                  prevented by the runtime
    //                                  guard — proxy for "how many
    //                                  silent corruptions the guard
    //                                  caught")
    //   - schema == 719
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual epoch/version check in apply_closure hot path,
    // IRClosure/closure_bridge_ management on invalidate,
    // linear_ownership_state runtime guard in GuardShape/Linear
    // op handlers / JIT, and ScopedCompilerRoot GC hook are
    // follow-up work (each is a dedicated session in
    // evaluator_eval_flat.cpp + service.ixx + evaluator_gc.cpp +
    // ir_executor_impl.cpp + aura_jit*.cpp).
    //
    // Issue #719: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=719 + category=general
    // + arity=0 + pure=true (same pattern as #712-#718).
    ObservabilityPrims::register_stats_impl(
        "query:closure-env-epoch-safety-stats", [&ev](const auto&) -> EvalValue {
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
            const std::int64_t epoch_mismatches =
                m ? static_cast<std::int64_t>(
                        m->closure_epoch_mismatch_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t linear_violations =
                m ? static_cast<std::int64_t>(
                        m->linear_violation_post_mutate_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t gc_root_syncs =
                m ? static_cast<std::int64_t>(m->gc_root_sync_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t dangling_prevented =
                m ? static_cast<std::int64_t>(
                        m->dangling_prevented_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"epoch-mismatches-caught", make_int(epoch_mismatches)},
                {"linear-violations-post-mutate", make_int(linear_violations)},
                {"gc-root-syncs", make_int(gc_root_syncs)},
                {"dangling-prevented", make_int(dangling_prevented)},
                {"schema", make_int(719)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 44 (orig lines 5791-5919)
void ObservabilityPrims::register_eval_p44(PrimRegistrar add, Evaluator& ev) {

    // Issue #720: (query:jit-interpreter-parity-stats) — JIT hot
    // path drift counters (non-duplicative with the aggregate
    // unhandled_opcode_count / fallback_count metrics in
    // aura_jit.cpp; #720 splits by *cause* and adds post-mutation
    // spike + metadata drift signals).
    //
    // Fields (4 + sentinel):
    //   - unhandled-opcode-spikes  jit_unhandled_opcode_spikes_total
    //                              (# of times an unhandled_opcode
    //                               spike crossed the per-function
    //                               threshold post-mutation —
    //                               triggers JIT->service invalidate
    //                               hook + deopt)
    //   - metadata-mismatches      jit_metadata_mismatch_total
    //                              (# of times metadata
    //                               (linear_ownership_state /
    //                                shape_id / narrow_evidence /
    //                                source_marker) drift was
    //                                detected between IRSoA /
    //                                AoS and the JIT's
    //                                FlatInstruction)
    //   - deopt-on-mutate          jit_deopt_on_mutate_total
    //                              (# of times JIT deopt was
    //                               triggered by a mutate /
    //                               invalidate event — forced
    //                               Interpreter fallback + async
    //                               recompile request via
    //                               CompilerService hook)
    //   - fallback-to-interpreter  jit_fallback_to_interpreter_total
    //                              (# of explicit fallbacks to
    //                               Interpreter — proxy for "how
    //                               often the JIT decided to give
    //                               up on hot path post-mutation")
    //   - schema == 720
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual FlatInstruction metadata extension + unhandled
    // hook + GuardShape/linear full consume + deopt->service wiring
    // + JIT->CompilerService invalidate hook are follow-up work
    // (each is a dedicated session in aura_jit.cpp + aura_jit.h +
    // aura_jit_bridge.cpp + service.ixx + ir_executor_impl.cpp).
    //
    // Issue #720: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=720 + category=general
    // + arity=0 + pure=true (same pattern as #712-#719).
    ObservabilityPrims::register_stats_impl(
        "query:jit-interpreter-parity-stats", [&ev](const auto&) -> EvalValue {
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
            const std::int64_t unhandled_spikes =
                m ? static_cast<std::int64_t>(
                        m->jit_unhandled_opcode_spikes_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t metadata_mismatches =
                m ? static_cast<std::int64_t>(
                        m->jit_metadata_mismatch_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t deopt_on_mutate =
                m ? static_cast<std::int64_t>(
                        m->jit_deopt_on_mutate_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t fallback_to_interpreter =
                m ? static_cast<std::int64_t>(
                        m->jit_fallback_to_interpreter_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"unhandled-opcode-spikes", make_int(unhandled_spikes)},
                {"metadata-mismatches", make_int(metadata_mismatches)},
                {"deopt-on-mutate", make_int(deopt_on_mutate)},
                {"fallback-to-interpreter", make_int(fallback_to_interpreter)},
                {"schema", make_int(720)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 45 (orig lines 5920-6034)
void ObservabilityPrims::register_eval_p45(PrimRegistrar add, Evaluator& ev) {

    // Issue #721: (query:ir-soa-completeness-stats) — IRFunctionSoA
    // column migration + dirty cascade counters (non-duplicative
    // with #658 5-gaps broad, #719 JIT metadata, #718 incremental
    // block dirty; #721 is the FIRST observability surface that
    // tracks SoA column migration progress + dirty cascade
    // shape/arena propagation as separate signals).
    //
    // Fields (3 + sentinel):
    //   - column-migration-hits    ir_soa_column_migration_hits_total
    //                              (# of times a hot emit/view
    //                               path took the SoA iterator
    //                               branch — vs AoS fallback)
    //   - dirty-cascade-to-shape   ir_soa_dirty_cascade_to_shape_total
    //                              (# of times the mark_block_
    //                               dirty cascade propagated to
    //                               ShapeProfiler::invalidate or
    //                               bumped dirty_shape hint)
    //   - pcv-wiring-savings-bytes ir_soa_pcv_wiring_savings_bytes_total
    //                              (cumulative bytes saved by
    //                               PCV-style PersistentChildVector
    //                               / gap_buffer wiring on operand /
    //                               shape / metadata columns)
    //   - schema == 721
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual PCV-style column extension + add_instruction
    // atomic growth + IRInstructionView dirty bit query + port of
    // hot emit/view paths to SoA iterators + ShapeProfiler
    // invalidate hook + Arena defrag hint are follow-up work
    // (each is a dedicated session in ir_soa.ixx + ir_soa_helpers +
    // lowering_impl.cpp + evaluator + aura_jit.cpp + ShapeProfiler
    // + Arena).
    //
    // Issue #721: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=721 + category=general
    // + arity=0 + pure=true (same pattern as #712-#720).
    ObservabilityPrims::register_stats_impl(
        "query:ir-soa-completeness-stats", [&ev](const auto&) -> EvalValue {
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
            const std::int64_t column_migration_hits =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_column_migration_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t dirty_cascade_to_shape =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_dirty_cascade_to_shape_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t pcv_wiring_savings_bytes =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_pcv_wiring_savings_bytes_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"column-migration-hits", make_int(column_migration_hits)},
                {"dirty-cascade-to-shape", make_int(dirty_cascade_to_shape)},
                {"pcv-wiring-savings-bytes", make_int(pcv_wiring_savings_bytes)},
                {"schema", make_int(721)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 46 (orig lines 6035-6158)
void ObservabilityPrims::register_eval_p46(PrimRegistrar add, Evaluator& ev) {

    // Issue #722: (query:arena-integration-stats) — Arena
    // tier/dtor/compact integration counters (non-duplicative
    // with the existing ArenaStats in arena.ixx which are
    // *internal* aggregate metrics; #722 is the FIRST
    // observability surface that exposes Arena ↔ dirty/shape
    // integration signals as separate counters the Agent can
    // consume).
    //
    // Fields (4 + sentinel):
    //   - tier-fallbacks            arena_tier_fallbacks_total
    //                                (# of times the SmallObjectPool
    //                                 tier 16/32/64B was exhausted
    //                                 and the allocator fell back
    //                                 to pmr)
    //   - dtor-dirty-hooks          arena_dtor_dirty_hooks_total
    //                                (# of times the dtor thunk
    //                                 triggered a dirty/shape hook
    //                                 on reset / compact)
    //   - auto-compact-triggers     arena_auto_compact_triggers_total
    //                                (# of times the auto-compact
    //                                 policy triggered compact/defrag
    //                                 from fragmentation +
    //                                 yield_check or dirty cascade
    //                                 — no manual request_defrag call)
    //   - fragmentation-post-mutate arena_fragmentation_post_mutate
    //                                (fragmentation ratio after mutate
    //                                 — scaled 0..1e6; 0 = no frag,
    //                                 1e6 = 100%)
    //   - schema == 722
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual fallback dirty-mark hook + dtor-to-shape wiring
    // + auto-compact policy from fragmentation/yield + IR cache
    // stats merge are follow-up work (each is a dedicated session
    // in arena.ixx + ShapeProfiler + ir_cache_pure + service.ixx).
    //
    // Issue #722: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=722 + category=general
    // + arity=0 + pure=true (same pattern as #712-#721).
    ObservabilityPrims::register_stats_impl(
        "query:arena-integration-stats", [&ev](const auto&) -> EvalValue {
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
            const std::int64_t tier_fallbacks =
                m ? static_cast<std::int64_t>(
                        m->arena_tier_fallbacks_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t dtor_dirty_hooks =
                m ? static_cast<std::int64_t>(
                        m->arena_dtor_dirty_hooks_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t auto_compact_triggers =
                m ? static_cast<std::int64_t>(
                        m->arena_auto_compact_triggers_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t fragmentation_post_mutate =
                m ? static_cast<std::int64_t>(
                        m->arena_fragmentation_post_mutate.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"tier-fallbacks", make_int(tier_fallbacks)},
                {"dtor-dirty-hooks", make_int(dtor_dirty_hooks)},
                {"auto-compact-triggers", make_int(auto_compact_triggers)},
                {"fragmentation-post-mutate", make_int(fragmentation_post_mutate)},
                {"schema", make_int(722)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 47 (orig lines 6159-6284)
void ObservabilityPrims::register_eval_p47(PrimRegistrar add, Evaluator& ev) {

    // Issue #723 / #571 / #1622: (query:value-dispatch-stats) — Value v2
    // dispatch + consteval table + Contracts observability (non-duplicative
    // with #658 Gaps 3/5; hash surface preferred over int-sum legacy).
    //
    // Fields (lineage 723 + #1622 AC):
    //   - dispatch-calls / unknown-tags / v2-string-collisions / shape-history-shifts
    //   - dispatch-hits / dispatch-misses (process-wide value_tags atomics)
    //   - dispatch-hit-rate-bp / contract-violation-count / v2-string-collision-attempts
    //   - classify-calls / consteval-table-wired / hotpath-contracts-wired
    //   - schema == 1622 (lineage 723|571)
    ObservabilityPrims::register_stats_impl(
        "query:value-dispatch-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                // #1622: ~14 keys — create(32) headroom.
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
            // Process-wide hot path counters (value_tags.h #571).
            const std::int64_t hits = static_cast<std::int64_t>(
                types::value_dispatch_hit_count.load(std::memory_order_relaxed));
            const std::int64_t misses = static_cast<std::int64_t>(
                types::value_dispatch_miss_count.load(std::memory_order_relaxed));
            const std::int64_t violations = static_cast<std::int64_t>(
                types::value_contract_violation_count.load(std::memory_order_relaxed));
            const std::int64_t collisions = static_cast<std::int64_t>(
                types::v2_string_collision_attempts.load(std::memory_order_relaxed));
            const std::int64_t classify_calls = static_cast<std::int64_t>(
                types::value_classify_call_count.load(std::memory_order_relaxed));
            const auto denom = hits + misses;
            const std::int64_t hit_rate_bp =
                denom > 0 ? (hits * 10000) / denom : (hits > 0 ? 10000 : 0);
            // Mirror into CompilerMetrics for dashboards that only read m.
            if (m) {
                m->value_dispatch_calls_total.store(static_cast<std::uint64_t>(classify_calls),
                                                    std::memory_order_relaxed);
                m->value_v2_string_collisions_total.store(static_cast<std::uint64_t>(collisions),
                                                          std::memory_order_relaxed);
            }
            const std::int64_t dispatch_calls =
                m ? static_cast<std::int64_t>(
                        m->value_dispatch_calls_total.load(std::memory_order_relaxed))
                  : classify_calls;
            const std::int64_t unknown_tags =
                m ? static_cast<std::int64_t>(
                        m->value_unknown_tag_total.load(std::memory_order_relaxed))
                  : misses;
            const std::int64_t v2_string_collisions =
                m ? static_cast<std::int64_t>(
                        m->value_v2_string_collisions_total.load(std::memory_order_relaxed))
                  : collisions;
            const std::int64_t shape_history_shifts =
                m ? static_cast<std::int64_t>(
                        m->shape_history_shift_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                // #723 lineage
                {"dispatch-calls", make_int(dispatch_calls)},
                {"unknown-tags", make_int(unknown_tags)},
                {"v2-string-collisions", make_int(v2_string_collisions)},
                {"shape-history-shifts", make_int(shape_history_shifts)},
                // #571 / #1622 process-wide AC keys
                {"dispatch-hits", make_int(hits)},
                {"dispatch-misses", make_int(misses)},
                {"dispatch-hit-rate-bp", make_int(hit_rate_bp)},
                {"dispatch_hit_rate", make_int(hit_rate_bp)},
                {"contract-violation-count", make_int(violations)},
                {"contract_violation_count", make_int(violations)},
                {"v2-string-collision-attempts", make_int(collisions)},
                {"v2_string_collision_attempts", make_int(collisions)},
                {"classify-calls", make_int(classify_calls)},
                {"consteval-table-wired", make_int(1)},
                {"hotpath-contracts-wired", make_int(1)},
                {"issue", make_int(1622)},
                {"schema", make_int(1622)}, // lineage 723|571
            };
            return build_hash(kv);
        });

    // Issue #1444: (query:mutation-boundary-coverage-stats) —
    // surface the cross-fiber Guard-coverage telemetry: naked-mutate
    // attempts, current boundary depth, outermost-flag, latest long-hold
    // event, and the strict-mode / threshold knobs (so callers can verify
    // policy is engaged without re-reading CompilerMetrics directly).
    ObservabilityPrims::register_stats_impl(
        "query:mutation-boundary-coverage-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(64);
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
            auto load = [&](std::atomic<std::uint64_t>& a) -> std::int64_t {
                return m ? static_cast<std::int64_t>(a.load(std::memory_order_relaxed)) : 0;
            };
            const std::int64_t naked = m ? load(m->naked_mutate_attempt) : 0;
            const std::int64_t boundary_depth =
                static_cast<std::int64_t>(Evaluator::mutation_boundary_depth());
            const std::int64_t boundary_held =
                ev.mutation_boundary_held_.load(std::memory_order_relaxed) ? 1 : 0;
            const std::int64_t threshold_us = m ? load(m->long_mutation_threshold_us) : 500'000;
            const std::int64_t strict_mode = m ? load(m->long_mutation_strict_mode) : 0;
            const std::int64_t starvation_prevented = m ? load(m->starvation_prevented_count) : 0;
            const std::int64_t last_fiber = m ? load(m->last_long_mutation_fiber_id) : 0;
            const std::int64_t last_dur_us = m ? load(m->last_long_mutation_duration_us) : 0;
            const std::int64_t max_extreme_us = m ? load(m->max_extreme_mutation_us) : 30'000'000;
            const std::int64_t extreme_total = m ? load(m->long_mutation_extreme_total) : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"naked-mutate-attempt", make_int(naked)},
                {"boundary-depth", make_int(boundary_depth)},
                {"boundary-held", make_int(boundary_held)},
                {"threshold-us", make_int(threshold_us)},
                {"strict-mode", make_int(strict_mode)},
                {"starvation-prevented", make_int(starvation_prevented)},
                {"last-long-mutation-fiber-id", make_int(last_fiber)},
                {"last-long-mutation-duration-us", make_int(last_dur_us)},
                {"max-extreme-mutation-us", make_int(max_extreme_us)},
                {"long-mutation-extreme-total", make_int(extreme_total)},
                {"panic-transfer-nested-success",
                 make_int(m ? load(m->panic_transfer_nested_success) : 0)},
                {"cow-repin-on-steal", make_int(m ? load(m->cow_repin_on_steal) : 0)},
                {"checkpoint-lost-on-compact",
                 make_int(m ? load(m->checkpoint_lost_on_compact) : 0)},
                {"schema", make_int(1444)},
            };
            return build_hash(kv);
        });

    // Issue #1445: (query:orchestration-steal-stats) — work-stealing +
    // starvation-mitigation telemetry (Scheduler::on_long_mutation_held path).
    ObservabilityPrims::register_stats_impl(
        "query:orchestration-steal-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(64);
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
            auto& s = ::aura::serve::metrics::adaptive_steal_stats();
            auto load = [&](std::atomic<std::uint64_t>& a) -> std::int64_t {
                return static_cast<std::int64_t>(a.load(std::memory_order_relaxed));
            };
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"mutation-bias-hits", make_int(load(s.mutation_bias_hits))},
                {"outermost-preferred", make_int(load(s.outermost_preferred))},
                {"deferred-pressure-boosts", make_int(load(s.deferred_pressure_boosts))},
                {"starvation-priority-boosts", make_int(load(s.starvation_priority_boosts))},
                {"steal-priority-boost-triggered",
                 make_int(load(s.steal_priority_boost_triggered))},
                {"starvation-mitigated-count", make_int(load(s.starvation_mitigated_count))},
                // Issue #1492: inner-defer starvation mitigation applications.
                {"steal-inner-deferred-starvation-mitigated-count",
                 make_int(load(s.steal_inner_deferred_starvation_mitigated_count))},
                {"ring-steal-attempts", make_int(load(s.ring_steal_attempts))},
                {"ring-steal-successes", make_int(load(s.ring_steal_successes))},
                {"steal-deferred-inner-boundary", make_int(load(s.steal_deferred_inner_boundary))},
                {"global-deferred-mutation-total",
                 make_int(load(s.global_deferred_mutation_total))},
                {"schema", make_int(1492)},
            };
            return build_hash(kv);
        });
}

} // namespace aura::compiler::primitives_detail
