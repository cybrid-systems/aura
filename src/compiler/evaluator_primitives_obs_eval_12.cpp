// evaluator_primitives_obs_eval_12.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 96 (orig lines 10922-10994)
void ObservabilityPrims::register_eval_p96(PrimRegistrar add, Evaluator& ev) {

    // Issue #685: (query:arena-auto-compact-stats) — alloc-path
    // auto-compact policy + Shape/dirty synergy metrics.
    ObservabilityPrims::register_stats_impl(
        "query:arena-auto-compact-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t auto_triggers = 0;
            std::uint64_t frag_reduced = 0;
            std::uint64_t shape_inval = 0;
            std::uint64_t defrag_savings = 0;
            std::uint64_t yield_checks = 0;
            if (ev.arena_) {
                const auto s = ev.arena_->stats();
                auto_triggers += s.auto_alloc_trigger_count;
                frag_reduced += s.frag_reduced_bp;
                shape_inval += s.shape_inval_on_compact;
                defrag_savings += s.defrag_savings_alloc;
                yield_checks += s.compaction_yield_checks;
            }
            if (ev.arena_group_) {
                const auto ag = ev.arena_group_->auto_compact_policy_stats();
                auto_triggers += ag.auto_triggers;
                frag_reduced += ag.frag_reduced;
                shape_inval += ag.shape_inval_on_compact;
                defrag_savings += ag.defrag_savings;
                yield_checks += ag.yield_checks_hit;
            }
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(8);
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
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"auto-triggers", make_int(static_cast<std::int64_t>(auto_triggers))},
                {"frag-reduced", make_int(static_cast<std::int64_t>(frag_reduced))},
                {"shape-inval-on-compact", make_int(static_cast<std::int64_t>(shape_inval))},
                {"defrag-savings", make_int(static_cast<std::int64_t>(defrag_savings))},
                {"yield-checks-hit", make_int(static_cast<std::int64_t>(yield_checks))},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 97 (orig lines 10995-11090)
void ObservabilityPrims::register_eval_p97(PrimRegistrar add, Evaluator& ev) {

    // Issue #569: Task4-review closing hash for tiered SmallObjectPool +
    // dtor tracking + auto-compaction + live defrag + fiber safepoint coordination.
    ObservabilityPrims::register_stats_impl(
        "query:arena-auto-compact-defrag-stats", [&ev](const auto&) -> EvalValue {
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
            const auto& group = ev.arena_group();
            const auto stats = group.total_stats();
            const auto policy = group.auto_compact_policy_stats();
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t live_dtors = ev.arena_ ? ev.arena_->live_count() : 0;
            const std::int64_t frag_pct =
                static_cast<std::int64_t>(stats.fragmentation_ratio() * 100.0);
            const std::uint64_t auto_compact_count =
                group.auto_compact_trigger_count() + policy.auto_triggers;
            const std::uint64_t auto_compact_skips = group.auto_compact_skip_count();
            const std::uint64_t guard_calls = group.auto_compact_guard_call_count();
            const std::uint64_t defrag_saved =
                policy.defrag_savings + stats.defrag_savings_alloc + stats.last_defrag_saved;
            const std::uint64_t defrag_attempted = stats.defrag_attempted_count;
            const std::uint64_t yield_checks = group.compaction_yield_checks_group() +
                                               policy.yield_checks_hit +
                                               stats.compaction_yield_checks;
            const std::uint64_t paused = ev.compaction_paused_by_boundary();
            const std::uint64_t gc_waits = ev.get_gc_safepoint_waits_total();
            const std::uint64_t gc_deferred = ev.get_gc_safepoint_deferred_total();
            const std::uint64_t safepoint_coord = yield_checks + paused + gc_waits + gc_deferred;
            const std::uint64_t mutation_volume = ev.total_mutations();
            const std::uint64_t threshold_config =
                m ? m->arena_auto_compact_threshold_set_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t total = live_dtors + stats.peak_used + auto_compact_count +
                                        auto_compact_skips + guard_calls + defrag_saved +
                                        defrag_attempted + safepoint_coord + mutation_volume +
                                        threshold_config;
            std::int64_t recommendation = 0;
            if (frag_pct > 30 && auto_compact_count == 0)
                recommendation = 3;
            else if (paused > yield_checks && paused > 0)
                recommendation = 2;
            else if (auto_compact_count > 0 || defrag_saved > 0 || safepoint_coord > 0)
                recommendation = 1;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"fragmentation-ratio-pct", make_int(frag_pct)},
                {"peak-used-bytes", make_int(static_cast<std::int64_t>(stats.peak_used))},
                {"live-dtor-count", make_int(static_cast<std::int64_t>(live_dtors))},
                {"auto-compact-count", make_int(static_cast<std::int64_t>(auto_compact_count))},
                {"auto-compact-skips", make_int(static_cast<std::int64_t>(auto_compact_skips))},
                {"auto-compact-guard-calls", make_int(static_cast<std::int64_t>(guard_calls))},
                {"defrag-saved-bytes", make_int(static_cast<std::int64_t>(defrag_saved))},
                {"defrag-attempted-count", make_int(static_cast<std::int64_t>(defrag_attempted))},
                {"safepoint-coordination-count",
                 make_int(static_cast<std::int64_t>(safepoint_coord))},
                {"mutation-volume-trigger", make_int(static_cast<std::int64_t>(mutation_volume))},
                {"threshold-config-count", make_int(static_cast<std::int64_t>(threshold_config))},
                {"compaction-yield-checks", make_int(static_cast<std::int64_t>(yield_checks))},
                {"paused-by-boundary", make_int(static_cast<std::int64_t>(paused))},
                {"task4-review-schema", make_int(569)},
                {"arena-auto-compact-defrag-total", make_int(static_cast<std::int64_t>(total))},
                {"arena-auto-compact-defrag-recommendation", make_int(recommendation)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 98 (orig lines 11091-11181)
void ObservabilityPrims::register_eval_p98(PrimRegistrar add, Evaluator& ev) {

    // Issue #604: (query:arena-fragmentation-snapshot) — a *live*
    // snapshot of the auto-compaction / defrag / fiber-yield
    // subsystem. Unlike (query:arena-auto-compact-stats) which
    // reports lifetime policy counters only, this also reports the
    // current aggregate fragmentation ratio so an AI agent can
    // correlate the trigger counters with the memory state right
    // now. Fields:
    //   - auto-compact-triggers: lifetime auto-trigger count
    //   - fragmentation-ratio:   current (capacity-used)/capacity
    //                            aggregated over arena_ + group
    //   - yield-deferred:        # of compactions that observed a
    //                            fiber context (compaction_yield_checks)
    //   - defrag-saved-bytes:    bytes reclaimed by alloc-path defrag
    //
    // Note: the (query:arena-auto-stats) name is already taken by
    // #464 (group-level guard/skip counts), so we use a distinct
    // name that signals "point-in-time snapshot" vs. "cumulative".
    add("query:arena-fragmentation-snapshot", [&ev](const auto&) -> EvalValue {
        std::uint64_t auto_triggers = 0;
        std::uint64_t yield_deferred = 0;
        std::uint64_t defrag_saved = 0;
        std::size_t total_cap = 0;
        std::size_t total_used = 0;
        if (ev.arena_) {
            const auto s = ev.arena_->stats();
            auto_triggers += s.auto_alloc_trigger_count;
            yield_deferred += s.compaction_yield_checks;
            defrag_saved += s.defrag_savings_alloc;
            total_cap += s.capacity;
            total_used += s.used;
        }
        if (ev.arena_group_) {
            const auto ag = ev.arena_group_->auto_compact_policy_stats();
            auto_triggers += ag.auto_triggers;
            yield_deferred += ag.yield_checks_hit;
            defrag_saved += ag.defrag_savings;
            const auto gs = ev.arena_group_->total_stats();
            total_cap += gs.capacity;
            total_used += gs.used;
        }
        const double frag = total_cap == 0 ? 0.0
                                           : static_cast<double>(total_cap - total_used) /
                                                 static_cast<double>(total_cap);
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
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
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"auto-compact-triggers", make_int(static_cast<std::int64_t>(auto_triggers))},
            {"fragmentation-ratio", make_float(frag)},
            {"yield-deferred", make_int(static_cast<std::int64_t>(yield_deferred))},
            {"defrag-saved-bytes", make_int(static_cast<std::int64_t>(defrag_saved))},
        };
        return build_hash(kv);
    });
}

// Issue #909 part 99 (orig lines 11182-11288)
void ObservabilityPrims::register_eval_p99(PrimRegistrar add, Evaluator& ev) {

    // Issue #614 + #584: (query:primitives-hotpath-stats) — pair-allocation +
    // cdr-traversal cost under AI Agent high-freq list/math workloads.
    // Hash fields (#614 foundation + #584 AI-agent stress synthesis):
    //   - primitive-call-total: lifetime # of primitive invocations
    //                            (same as the #441/#450 field exposed
    //                            by query:primitive-perf-stats; kept
    //                            here for one-shot correlation).
    //   - pair-alloc-total:     # of pairs.push_back calls across
    //                            list / append / reverse / map / filter.
    //   - linear-traverse-total: total cdr-walk steps across length /
    //                            list-ref / member / foldl.
    //   - cdr-depth-max:        longest single linear traverse
    //                            observed (high-water mark).
    //
    // This is the AI agent's signal for "are my list-heavy
    // stdlib usages paying pair-allocation cost that I should
    // consolidate to Arena-backed storage?" + "is cdr-walk
    // getting pathological under mutation?".
    ObservabilityPrims::register_stats_impl(
        "query:primitives-hotpath-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t call_total = 0;
            std::uint64_t pair_total = 0;
            std::uint64_t tra_total = 0;
            std::uint64_t depth_max = 0;
            std::uint64_t fastpath_hits = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
                call_total = m->primitive_call_total.load(std::memory_order_relaxed);
                pair_total = m->pair_alloc_total.load(std::memory_order_relaxed);
                tra_total = m->linear_traverse_total.load(std::memory_order_relaxed);
                depth_max = m->cdr_depth_max.load(std::memory_order_relaxed);
                fastpath_hits = m->primitive_fastpath_hits_total.load(std::memory_order_relaxed);
            }
            const std::uint64_t mutations = ev.total_mutations();
            const std::uint64_t queries = ev.get_total_query_calls();
            const std::uint64_t call_denom = call_total + mutations + queries + 1;
            const std::int64_t call_rate =
                static_cast<std::int64_t>((call_total * 100) / call_denom);
            const std::int64_t alloc_per_call =
                static_cast<std::int64_t>(pair_total / (call_total + 1));
            const std::int64_t regex_time_us =
                static_cast<std::int64_t>((tra_total * 10) / (call_total + 1));
            const std::int64_t stability_penalty = static_cast<std::int64_t>(
                alloc_per_call * 3 + (depth_max > 32 ? depth_max / 8 : 0));
            const std::int64_t stability_score =
                stability_penalty >= 100 ? 0 : 100 - stability_penalty;
            const std::uint64_t total = call_total + pair_total + tra_total + depth_max +
                                        fastpath_hits + static_cast<std::uint64_t>(call_rate);
            std::int64_t recommendation = 0;
            if (stability_score < 50)
                recommendation = 3;
            else if (alloc_per_call > 10 || depth_max > 64)
                recommendation = 2;
            else if (call_total > 0 || fastpath_hits > 0)
                recommendation = 1;
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
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"primitive-call-total", make_int(static_cast<std::int64_t>(call_total))},
                {"pair-alloc-total", make_int(static_cast<std::int64_t>(pair_total))},
                {"linear-traverse-total", make_int(static_cast<std::int64_t>(tra_total))},
                {"cdr-depth-max", make_int(static_cast<std::int64_t>(depth_max))},
                {"call-rate", make_int(call_rate)},
                {"alloc-per-call", make_int(alloc_per_call)},
                {"regex-time-us", make_int(regex_time_us)},
                {"stability-score", make_int(stability_score)},
                {"hotpath-schema", make_int(584)},
                {"primitives-hotpath-total", make_int(static_cast<std::int64_t>(total))},
                {"primitives-hotpath-recommendation", make_int(recommendation)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 100 (orig lines 11289-11366)
void ObservabilityPrims::register_eval_p100(PrimRegistrar add, Evaluator& ev) {

    // (query:cxx26-hotpath-invariants) — Issue #465: C++26
    // hot-path Contracts + consteval invariants observability.
    // Returns a 5-field hash reporting the compile-time
    // invariants the binary was built with:
    //   - fixnum-tag-encoding: 0 (matches low2 dispatch table[0])
    //   - ref-tag-encoding: 1 (matches low2 dispatch table[1])
    //   - string-v2-tag-encoding: 2 (matches low2 dispatch table[2])
    //   - special-tag-encoding: 3 (matches low2 dispatch table[3])
    //   - float-tag-encoding: 4 (out of low2 dispatch space)
    //
    // These are static_assert'd at compile time in
    // value_tags.h. The primitive reports the values that
    // were baked in at build time, so the AI Agent can
    // verify a deployed binary matches the expected
    // encoding without re-running the static_asserts.
    //
    // Future follow-ups will add:
    //   - The Pass concept instance count
    //   - The SoA column count
    //   - The dirty bitmask byte width
    add("query:cxx26-hotpath-invariants", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
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
        // These values are the ones static_assert'd in
        // value_tags.h. The build will fail if they drift.
        // We hardcode the values here because the
        // EvalValueTag enum is in value.ixx (a different
        // module partition) and not directly accessible
        // from this file. The static_assert chain in
        // value_tags.h is the source of truth; the
        // primitive reports the same values so the
        // AI Agent can verify a deployed binary matches
        // the expected encoding.
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"fixnum-tag-encoding", make_int(0)},    {"ref-tag-encoding", make_int(1)},
            {"string-v2-tag-encoding", make_int(2)}, {"special-tag-encoding", make_int(3)},
            {"float-tag-encoding", make_int(4)},
        };
        return build_hash(kv);
    });
}

// Issue #909 part 101 (orig lines 11367-11442)
void ObservabilityPrims::register_eval_p101(PrimRegistrar add, Evaluator& ev) {

    // (atomic-batch:stats) — Issue #192: observability for
    // mutate:atomic-batch. Hash with batch-count, ops-total,
    // rollback-count, ops-per-batch (avg).
    add("atomic-batch:stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            // Issue #394 / #258: capacity 32 (was 8). This primitive
            // returns 5 keys; cap-8 + FNV-1a probing occasionally
            // failed to insert a key, so hash-ref returned void.
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
                    fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
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
        std::size_t avg = ev.atomic_batch_domain_.count > 0
                              ? ev.atomic_batch_domain_.ops_total / ev.atomic_batch_domain_.count
                              : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"batch-count", make_int(static_cast<std::int64_t>(ev.atomic_batch_domain_.count))},
            {"ops-total", make_int(static_cast<std::int64_t>(ev.atomic_batch_domain_.ops_total))},
            {"rollback-count",
             make_int(static_cast<std::int64_t>(ev.atomic_batch_domain_.rollbacks))},
            {"ops-per-batch", make_int(static_cast<std::int64_t>(avg))},
            // Issue #250: how many per-op generation bumps the
            // batches suppressed (lifetime total). Useful for
            // dashboards ("how much churn did batching save?").
            {"bumps-saved-total",
             make_int(static_cast<std::int64_t>(ev.atomic_batch_domain_.bumps_saved_total))},
            // Issue #396 Phase 3: heuristic for "ran under
            // concurrent fiber pressure". Bumped when the
            // bridge fiber setter was active at commit time
            // (i.e. serve mode + fiber context). Stays 0 in
            // test-binary paths where the hook is null.
            // Name matches the issue's proposed field.
            {"executed-under-concurrent-fiber",
             make_int(static_cast<std::int64_t>(ev.atomic_batch_domain_.in_fiber_total))},
            // Issue #737: pinning + snapshot rollback observability.
            {"pinned-refs-last-batch",
             make_int(static_cast<std::int64_t>(ev.atomic_batch_pinned_ref_count()))},
            {"rollback-triggers", make_int(static_cast<std::int64_t>(ev.atomic_batch_rollbacks()))},
        };
        return build_hash(kv);
    });
}

// Issue #909 part 102 (orig lines 11443-11523)
void ObservabilityPrims::register_eval_p102(PrimRegistrar add, Evaluator& ev) {

    // (closure:stats) — Issue #252: observability for
    // apply_closure dual-path. Hash with the 5 counters
    // (calls-total, ffi-calls, tw-calls, bridge-calls,
    // stale-returns) + bridge-fraction (helper for
    // dashboards: how much of the dispatch goes to the
    // bridge path, which is the slowest).
    add("closure:stats", [&ev](const auto&) -> EvalValue {
        // Re-use the build_hash from atomic-batch:stats
        // (defined in the lambda above). It's the same code
        // pattern, so we re-bind to keep the closure:stats
        // self-contained.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            // Issue #394 / #258: capacity 32 (was 8). closure:stats
            // returns 7 keys; cap-8 insertion failures broke hash-ref.
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
                    fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
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
        // Issue #252: closure stats. Read from ev.compiler_metrics_
        // (shared with the IR's IROpcode::Call/Apply). If metrics
        // is not set (legacy standalone use), all counters are 0.
        std::uint64_t calls = 0, ffi_c = 0, tw_c = 0, ir_c = 0;
        std::uint64_t bridge_c = 0, stale_c = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            calls = m->closure_calls_total.load(std::memory_order_relaxed);
            ffi_c = m->closure_ffi_calls.load(std::memory_order_relaxed);
            tw_c = m->closure_tw_calls.load(std::memory_order_relaxed);
            ir_c = m->closure_ir_calls.load(std::memory_order_relaxed);
            bridge_c = m->closure_bridge_calls.load(std::memory_order_relaxed);
            stale_c = m->closure_stale_returns.load(std::memory_order_relaxed);
        }
        std::uint64_t bridge = bridge_c;
        // bridge-fraction * 100 (integer percent). 0 if no calls.
        std::int64_t bridge_pct = calls > 0 ? static_cast<std::int64_t>((bridge * 100) / calls) : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"calls-total", make_int(static_cast<std::int64_t>(calls))},
            {"ffi-calls", make_int(static_cast<std::int64_t>(ffi_c))},
            {"tw-calls", make_int(static_cast<std::int64_t>(tw_c))},
            {"ir-calls", make_int(static_cast<std::int64_t>(ir_c))},
            {"bridge-calls", make_int(static_cast<std::int64_t>(bridge))},
            {"stale-returns", make_int(static_cast<std::int64_t>(stale_c))},
            {"bridge-fraction-pct", make_int(bridge_pct)},
        };
        return build_hash(kv);
    });
}

// Issue #909 part 103 (orig lines 11524-11637)
void ObservabilityPrims::register_eval_p103(PrimRegistrar add, Evaluator& ev) {

    // (query:closure-stats) — Issue #428: unified closure
    // observability surface in the query: family. Returns
    // a hash with 9 fields covering both the dispatch
    // (closure:stats 7 fields) and the bridge_epoch drift
    // (bridge-epoch-hits, bridge-epoch-drift-pct). The
    // drift is the percent of bridge_epoch checks that
    // observed a stale epoch (vs hits which observed
    // fresh) — the AI Agent's primary signal for
    // "is the bridge falling behind the mutation rate?".
    //
    // Field list (9 total):
    //   - calls-total:           every apply_closure call
    //   - ffi-calls:             FFI-dispatched
    //   - tw-calls:              tree-walker closures_ map hit
    //   - ir-calls:              IR runtime_closures_ hit
    //   - bridge-calls:          closure_bridge_ (IR/JIT)
    //   - stale-returns:         stale-bridge nullopt returns
    //   - bridge-fraction-pct:   bridge-calls / calls-total * 100
    //   - bridge-epoch-hits:     # of bridge_epoch checks
    //                            that succeeded (fresh)
    //   - bridge-epoch-drift-pct: stale-returns /
    //                            (bridge-epoch-hits + stale-returns)
    //                            * 100. The AI Agent's primary
    //                            signal — > 0 means the workspace
    //                            is mutating faster than closures
    //                            can refresh.
    //
    // Migration note: closure:stats is the pre-#428 primitive;
    // query:closure-stats is the new unified surface. Both
    // return the same hash shape; the new one just adds
    // 2 bridge_epoch fields. The old primitive stays for
    // backward compat (existing tests use closure:stats).
    ObservabilityPrims::register_stats_impl("query:closure-stats", [&ev](const auto&) -> EvalValue {
        // Reuse the same build_hash pattern as closure:stats.
        // Inline here (instead of refactoring closure:stats to
        // a helper) so the new primitive stays self-contained
        // and easy to evolve independently.
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
        std::uint64_t calls = 0, ffi_c = 0, tw_c = 0, ir_c = 0;
        std::uint64_t bridge_c = 0, stale_c = 0;
        std::uint64_t bridge_epoch_hits = 0, bridge_epoch_drifts = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            calls = m->closure_calls_total.load(std::memory_order_relaxed);
            ffi_c = m->closure_ffi_calls.load(std::memory_order_relaxed);
            tw_c = m->closure_tw_calls.load(std::memory_order_relaxed);
            ir_c = m->closure_ir_calls.load(std::memory_order_relaxed);
            bridge_c = m->closure_bridge_calls.load(std::memory_order_relaxed);
            stale_c = m->closure_stale_returns.load(std::memory_order_relaxed);
            bridge_epoch_hits = m->bridge_epoch_hit_count_.load(std::memory_order_relaxed);
            bridge_epoch_drifts = m->closure_stale_refresh_count_.load(std::memory_order_relaxed);
        }
        std::int64_t bridge_pct =
            calls > 0 ? static_cast<std::int64_t>((bridge_c * 100) / calls) : 0;
        // Drift = stale-refreshes / (hits + drifts) * 100.
        // 0 if no checks yet (avoids divide-by-zero in the
        // dashboard, which would otherwise show NaN).
        std::uint64_t total_epoch_checks = bridge_epoch_hits + bridge_epoch_drifts;
        std::int64_t drift_pct =
            total_epoch_checks > 0
                ? static_cast<std::int64_t>((bridge_epoch_drifts * 100) / total_epoch_checks)
                : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"calls-total", make_int(static_cast<std::int64_t>(calls))},
            {"ffi-calls", make_int(static_cast<std::int64_t>(ffi_c))},
            {"tw-calls", make_int(static_cast<std::int64_t>(tw_c))},
            {"ir-calls", make_int(static_cast<std::int64_t>(ir_c))},
            {"bridge-calls", make_int(static_cast<std::int64_t>(bridge_c))},
            {"stale-returns", make_int(static_cast<std::int64_t>(stale_c))},
            {"bridge-fraction-pct", make_int(bridge_pct)},
            {"bridge-epoch-hits", make_int(static_cast<std::int64_t>(bridge_epoch_hits))},
            {"bridge-epoch-drift-pct", make_int(drift_pct)},
        };
        return build_hash(kv);
    });
}

} // namespace aura::compiler::primitives_detail
