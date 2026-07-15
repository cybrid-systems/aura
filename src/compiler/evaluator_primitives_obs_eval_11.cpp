// evaluator_primitives_obs_eval_11.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 88 (orig lines 10174-10358)
void ObservabilityPrims::register_eval_p88(PrimRegistrar add, Evaluator& ev) {

    // Issue #478: query:primitive-error-stats — returns a pair
    // (error-count . error-values-size) for Agent recovery loops.
    ObservabilityPrims::register_stats_impl(
        "query:primitive-error-stats", [&ev](const auto&) -> EvalValue {
            auto count = static_cast<std::int64_t>(ev.get_primitive_error_count());
            auto stored = static_cast<std::int64_t>(ev.get_primitive_error_values_size());
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_int(count), make_int(stored)});
            return make_pair(pid);
        });

    // (query:primitive-fastpath-per-prim) — Issue #479:
    // per-prim fast-path hit breakdown. Returns a hash with:
    //   - total: aggregate fast-path hit count (matches
    //     primitive_fastpath_hits_total from #709)
    //   - distinct-prims: number of slots with count > 0
    //   - top: list of (name . count) dotted pairs sorted
    //     by count desc, capped at top-N (default 10). The
    //     hottest primitive comes first. Slots with 0 hits
    //     are excluded to keep the response small even for
    //     large registries.
    //   - capacity: current per-prim array capacity (for
    //     diagnosing whether growth has occurred)
    //
    // Issue #804: query:primitive-error-unified-stats — unified
    // primitive error semantics + recovery observability
    // composite (P0 stdlib-Registry reliability foundation;
    // refines/consolidates #585 + #751 + #775 + #478; non-
    // duplicative with #585 query:primitives-error-stats
    // coarse hash + #478 query:primitive-error-stats pair
    // primitive + #751 query:primitives-contract-stats
    // contract enforcement + #775 query:extension-kit-stats
    // capture contract validation + #806 registry-extension
    // primitives). #804 is the FIRST observability surface
    // that tracks the *unified-error-path SLO composite* —
    // 100% primitives use unified path + zero silent fallback
    // errors under load — as a single deployment-grade SLO
    // composite the Agent reads to decide whether the
    // stdlib error semantics are production-ready for
    // commercial AI Agent use.
    //
    // Fields (8 + sentinel):
    //   - error-count-total       reused primitive_error_count_
    //                             (#478 source-of-truth; bumped
    //                             by bump_primitive_error_count()
    //                             at every PRIM_ERROR / make_
    //                             primitive_error invocation)
    //   - with-provenance         primitive_error_with_provenance_
    //                             total (NEW atomic; # of errors
    //                             that filled in (kind, msg,
    //                             provenance) schema — the
    //                             *good* path the body asks for
    //                             at 100% coverage)
    //   - silent-fallback        primitive_error_silent_fallback_
    //                             total (NEW atomic; # of ad-hoc
    //                             returns / catch-alls the body
    //                             warns against; counted by the
    //                             Phase 2+ audit grep)
    //   - error-values-size      reused get_primitive_error_
    //                             values_size() (the persistent
    //                             error object arena size; #478
    //                             pair second component)
    //   - capture-violations     reused #751 primitive_capture_
    //                             violations_total (capture
    //                             contract enforcement; a separate
    //                             *violation* signal from
    //                             primitive_error_count_)
    //   - unified-path-pct       derived (with-provenance /
    //                             error-count-total) × 10000
    //                             (SLO target 100% = 10000
    //                             per body "100% primitives use
    //                             unified path")
    //   - recovery-hook-invocations  primitive_error_recovery_
    //                             hook_invocations_total (NEW
    //                             atomic; count of recovery-hook
    //                             firings in Guard + retry path;
    //                             bumped by
    //                             bump_primitive_error_recovery_
    //                             hook())
    //   - unified-error-path-active  hardcoded 0 (Phase 2+; the
    //                             actual PRIM_ERROR audit +
    //                             make_primitive_error
    //                             provenance enforcement +
    //                             registry enforce-unified-path
    //                             + (error:structured-make ...)
    //                             + recovery hooks in Guard +
    //                             tests/test_primitive_error_
    //                             unified_audit.cpp harness
    //                             all remain follow-up work per
    //                             body Actionable 1-5)
    //   - schema == 804
    ObservabilityPrims::register_stats_impl(
        "query:primitive-error-unified-stats", [&ev](const auto&) -> EvalValue {
            const auto* m = ev.compiler_metrics()
                                ? static_cast<const CompilerMetrics*>(ev.compiler_metrics())
                                : nullptr;
            // Reused #478 + #751 atomics.
            const std::int64_t error_count_total =
                static_cast<std::int64_t>(ev.get_primitive_error_count());
            const std::int64_t error_values_size =
                static_cast<std::int64_t>(ev.get_primitive_error_values_size());
            const std::int64_t capture_violations =
                m ? static_cast<std::int64_t>(
                        m->primitive_capture_violations_total.load(std::memory_order_relaxed))
                  : 0;
            // NEW #804 atomics.
            const std::int64_t with_provenance =
                m ? static_cast<std::int64_t>(
                        m->primitive_error_with_provenance_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t silent_fallback =
                m ? static_cast<std::int64_t>(
                        m->primitive_error_silent_fallback_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t recovery_hook_invocations =
                m ? static_cast<std::int64_t>(
                        m->primitive_error_recovery_hook_invocations_total.load(
                            std::memory_order_relaxed))
                  : 0;
            // Derived unified-path-pct: vacuous-true 10000 baseline
            // when error_count_total == 0 (no errors observed yet
            // = vacuously compliant); otherwise (with_provenance /
            // error_count_total) × 10000. SLO target = 100% =
            // 10000 per body "100% primitives use unified path".
            std::int64_t unified_path_pct = 10000;
            if (error_count_total > 0) {
                unified_path_pct = static_cast<std::int64_t>(
                    (with_provenance * ::aura::compiler::kBasisPointScale) / error_count_total);
            }
            // Hardcoded "not yet" flag — Phase 2+ deferred.
            const std::int64_t unified_error_path_active = 0;
            // Recommendation derivation:
            //   0 = production-ready (unified-path-pct == 10000 +
            //       unified-error-path-active)
            //   1 = near-production (SLO met but active flag off)
            //   2 = partial Phase 1 (errors observed + some with
            //       provenance but SLO not yet 100%)
            //   3 = early-stage (no error activity yet)
            std::int64_t recommendation = 3;
            if (error_count_total + capture_violations + silent_fallback +
                    recovery_hook_invocations >
                0) {
                if (unified_path_pct >= 10000 && silent_fallback == 0) {
                    recommendation = unified_error_path_active ? 0 : 1;
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
            insert_kv("error-count-total", error_count_total);
            insert_kv("with-provenance", with_provenance);
            insert_kv("silent-fallback", silent_fallback);
            insert_kv("error-values-size", error_values_size);
            insert_kv("capture-violations", capture_violations);
            insert_kv("unified-path-pct", unified_path_pct);
            insert_kv("recovery-hook-invocations", recovery_hook_invocations);
            insert_kv("unified-error-path-active", unified_error_path_active);
            insert_kv("schema", 804);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 89 (orig lines 10359-10469)
void ObservabilityPrims::register_eval_p89(PrimRegistrar add, Evaluator& ev) {

    // (query:primitive-fastpath-per-prim) — Issue #479:
    add("query:primitive-fastpath-per-prim", [&ev](std::span<const EvalValue> a) -> EvalValue {
        constexpr std::size_t kDefaultTopN = 10;
        std::size_t top_n = kDefaultTopN;
        // Optional arg: top-N override (clamped to [1, 1000]).
        if (!a.empty() && is_int(a[0])) {
            auto v = as_int(a[0]);
            if (v < 1)
                v = 1;
            if (v > 1000)
                v = 1000;
            top_n = static_cast<std::size_t>(v);
        }

        std::uint64_t total = 0;
        std::uint64_t distinct = 0;
        std::vector<std::pair<std::string, std::uint64_t>> rows;
        std::size_t cap = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            total = m->primitive_fastpath_hits_total.load(std::memory_order_relaxed);
            cap = m->primitive_fastpath_per_prim_capacity_;
            const auto slot_count = ev.primitives_.slot_count();
            const std::size_t limit = std::min(slot_count, cap);
            rows.reserve(limit);
            for (std::size_t slot = 0; slot < limit; ++slot) {
                auto cnt =
                    m->primitive_fastpath_hits_per_prim_[slot].load(std::memory_order_relaxed);
                if (cnt > 0) {
                    ++distinct;
                    rows.emplace_back(ev.primitives_.name_for_slot(slot), cnt);
                }
            }
        }
        // Sort desc by count, ties broken by name asc for stability.
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second)
                return a.second > b.second;
            return a.first < b.first;
        });
        if (rows.size() > top_n)
            rows.resize(top_n);

        // Build top-N as a proper list of (name . count) dotted pairs.
        // Build in reverse so the head of the list is the last
        // pushed pair (Aura list primitive uses pair-chain with
        // void terminator; building in reverse is the natural form).
        EvalValue top_list = make_void();
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
            auto name_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(it->first);
            auto name_ev = make_string(name_idx);
            auto count_ev = make_int(static_cast<std::int64_t>(it->second));
            auto idx = ev.pairs_.size();
            ev.pairs_.push_back({name_ev, count_ev});
            auto pair_ev = make_pair(idx);
            auto cell_idx = ev.pairs_.size();
            ev.pairs_.push_back({pair_ev, top_list});
            top_list = make_pair(cell_idx);
        }

        // Inline build_hash (small hash, 4 fields; matches the
        // pattern used by query:primitive-perf-stats below).
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
            {"total", make_int(static_cast<std::int64_t>(total))},
            {"distinct-prims", make_int(static_cast<std::int64_t>(distinct))},
            {"top", top_list},
            {"capacity", make_int(static_cast<std::int64_t>(cap))},
        };
        return build_hash(kv);
    });
}

// Issue #909 part 90 (orig lines 10470-10542)
void ObservabilityPrims::register_eval_p90(PrimRegistrar add, Evaluator& ev) {

    // (query:primitive-perf-stats) — Issue #441 (rolled into
    // #450): hot-path primitive dispatch stats. Returns a
    // hash with 3 fields:
    //   - primitive-call-total: lifetime count of every
    //     (primitive-func args...) dispatch (bumped in
    //     evaluator_eval_flat.cpp at the dispatch site)
    //   - primitive-count: # of registered primitives
    //     (snapshot at primitive-registration time; gives
    //     a per-primitive average call rate when paired
    //     with primitive-call-total)
    //   - avg-per-prim: primitive-call-total / primitive-count
    //
    // Issue #479 ships the per-prim breakdown as a
    // separate primitive (query:primitive-fastpath-per-prim)
    // — see above. This primitive remains the aggregate
    // "is the dispatch hot path hot?" answer.
    ObservabilityPrims::register_stats_impl(
        "query:primitive-perf-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t call_total = 0;
            std::uint64_t prim_count = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
                call_total = m->primitive_call_total.load(std::memory_order_relaxed);
            }
            prim_count = ev.primitives_.slot_count();
            std::int64_t avg_per_prim =
                prim_count > 0 ? static_cast<std::int64_t>(call_total / prim_count) : 0;
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
                {"primitive-count", make_int(static_cast<std::int64_t>(prim_count))},
                {"avg-per-prim", make_int(avg_per_prim)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 91 (orig lines 10543-10617)
void ObservabilityPrims::register_eval_p91(PrimRegistrar add, Evaluator& ev) {

    // (query:aot-stats) — Issue #452: AOT hot-update + region
    // filtering observability. Returns a 3-field hash:
    //   - aot-stale-reject-count: lifetime count of
    //     aura_reload_aot_module rejections due to
    //     aot_emit_version mismatch (bumped in
    //     aura_jit_bridge.cpp)
    //   - aot-region-mismatch-count: lifetime count of
    //     region_filter mismatches (currently 0 — region
    //     wiring is a follow-up; counter is in place
    //     so the day it ships, observability is immediate)
    //   - aot-hot-update-success-count: lifetime count of
    //     successful dlopen + version check + constructor
    //     invocation.
    //
    // This is the AI Agent's signal for "is the AOT
    // hot-update pipeline behaving correctly?". A rising
    // stale-reject count without rising success count =
    // version drift (the bug pattern from #452's body).
    ObservabilityPrims::register_stats_impl("query:aot-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t stale_rej = 0;
        std::uint64_t region_mismatch = 0;
        std::uint64_t hot_update_ok = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            stale_rej = m->aot_stale_reject_count_.load(std::memory_order_relaxed);
            region_mismatch = m->aot_region_mismatch_.load(std::memory_order_relaxed);
            hot_update_ok = m->aot_hot_update_success_.load(std::memory_order_relaxed);
        }
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
            {"aot-stale-reject-count", make_int(static_cast<std::int64_t>(stale_rej))},
            {"aot-region-mismatch-count", make_int(static_cast<std::int64_t>(region_mismatch))},
            {"aot-hot-update-success-count", make_int(static_cast<std::int64_t>(hot_update_ok))},
        };
        return build_hash(kv);
    });
}

// Issue #909 part 92 (orig lines 10618-10682)
void ObservabilityPrims::register_eval_p92(PrimRegistrar add, Evaluator& ev) {

    // (query:ci-reproducibility-stats) — Issue #675: build/CI
    // reproducibility + sanitizer gate observability. Returns a
    // 5-field hash:
    //   - source-date-epoch: SOURCE_DATE_EPOCH env (0 if unset)
    //   - build-type: AURA_BUILD_TYPE env (or "unknown")
    //   - sanitizer-mode: compile-time "none"|"asan"|"ubsan"|"tsan"
    //   - reproducible-flags-active: 1 iff SOURCE_DATE_EPOCH > 0
    //   - ccache-disabled: 1 iff CCACHE_DISABLE=1
    ObservabilityPrims::register_stats_impl(
        "query:ci-reproducibility-stats", [&ev](const auto&) -> EvalValue {
            const auto epoch = aura::ci::source_date_epoch();
            const auto repro = aura::ci::reproducible_flags_active();
            const auto ccache_off = aura::ci::ccache_disabled();
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
            auto bt_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(aura::ci::build_type_from_env());
            auto san_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(aura::ci::sanitizer_mode());
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"source-date-epoch", make_int(epoch)},
                {"build-type", make_string(bt_idx)},
                {"sanitizer-mode", make_string(san_idx)},
                {"reproducible-flags-active", make_bool(repro)},
                {"ccache-disabled", make_bool(ccache_off)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 93 (orig lines 10683-10762)
void ObservabilityPrims::register_eval_p93(PrimRegistrar add, Evaluator& ev) {

    // (query:shape-folding-stats) — Issue #462: observability
    // for ShapeAwareFoldingPass. Returns a 4-field hash:
    //   - shape-fold-count: lifetime # of instructions
    //     replaced (OpNop'd) due to shape/linear/narrow
    //     metadata
    //   - shape-linear-elide-count: subset of fold-count
    //     due to linear-ownership elision (MoveOp on
    //     non-escaping Owned slot is a no-op)
    //   - shape-narrow-check-count: # of redundant
    //     type-checks detected (counted, not yet rewritten
    //     in Cycle 1; rewrite is #462 follow-up)
    //   - guard-shape-hits: # of GuardShape instructions
    //     seen in the module (signal for downstream passes
    //     to trust per-block shape_id)
    //
    // This is the AI Agent's signal for "is the
    // shape-aware folding pass doing useful work?".
    // Cycle 2 (separate issue) will add per-shape-id
    // OpAdd unchecked specialization + the narrow-evidence
    // rewrite. The counter layer is in place.
    ObservabilityPrims::register_stats_impl(
        "query:shape-folding-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t fold = 0;
            std::uint64_t linear_elide = 0;
            std::uint64_t narrow = 0;
            std::uint64_t guard_hits = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
                fold = m->shape_fold_count.load(std::memory_order_relaxed);
                linear_elide = m->shape_linear_elide_count.load(std::memory_order_relaxed);
                narrow = m->shape_narrow_check_count.load(std::memory_order_relaxed);
                guard_hits = m->guard_shape_hits.load(std::memory_order_relaxed);
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
                {"shape-fold-count", make_int(static_cast<std::int64_t>(fold))},
                {"shape-linear-elide-count", make_int(static_cast<std::int64_t>(linear_elide))},
                {"shape-narrow-check-count", make_int(static_cast<std::int64_t>(narrow))},
                {"guard-shape-hits", make_int(static_cast<std::int64_t>(guard_hits))},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 94 (orig lines 10763-10835)
void ObservabilityPrims::register_eval_p94(PrimRegistrar add, Evaluator& ev) {

    // (query:soa-adoption-stats) — Issue #463: SoA Phase 2
    // adoption observability. Returns a 3-field hash:
    //   - soa-functions-visited: lifetime # of SoA
    //     functions walked by the bridge pass
    //   - soa-instructions-visited: lifetime # of SoA
    //     instructions walked
    //   - aos-view-built-count: lifetime # of SoA→AoS
    //     view conversions
    //
    // This is the AI Agent's signal for "is the SoA
    // rollout progressing?". A rising
    // soa-instructions-visited count with no
    // aos-view-built-count growth means the SoA path is
    // being used end-to-end (the AoS view is a one-time
    // scaffold; subsequent cycles replace it with
    // SoA-aware Pass overloads).
    ObservabilityPrims::register_stats_impl(
        "query:soa-adoption-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t funcs = 0;
            std::uint64_t instrs = 0;
            std::uint64_t views = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
                funcs = m->soa_functions_visited.load(std::memory_order_relaxed);
                instrs = m->soa_instructions_visited.load(std::memory_order_relaxed);
                views = m->aos_view_built_count.load(std::memory_order_relaxed);
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
                {"soa-functions-visited", make_int(static_cast<std::int64_t>(funcs))},
                {"soa-instructions-visited", make_int(static_cast<std::int64_t>(instrs))},
                {"aos-view-built-count", make_int(static_cast<std::int64_t>(views))},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 95 (orig lines 10836-10921)
void ObservabilityPrims::register_eval_p95(PrimRegistrar add, Evaluator& ev) {

    // (query:arena-auto-stats) — Issue #464: Arena
    // auto-compaction lifecycle observability. Returns a
    // 4-field hash:
    //   - auto-compact-guard-call-count: lifetime # of
    //     times MutationBoundaryGuard dtor bumped the
    //     closed-loop signal (one bump per outermost +
    //     success guard exit)
    //   - compaction-yield-checks: lifetime # of times
    //     auto_compact_with_safety() observed a fiber
    //     context (g_current_fiber != nullptr); the actual
    //     yield-during-compact is a #464 follow-up
    //   - auto-compact-trigger-count: lifetime # of
    //     triggered compactions (from ArenaGroup)
    //   - auto-compact-skip-count: lifetime # of
    //     skipped triggers (below adaptive threshold)
    //
    // This is the AI Agent's signal for "is the
    // arena auto-compaction lifecycle working as
    // expected?". Cycle 2 (separate issue) will add
    // the actual auto_compact_with_safety() call from
    // the scheduler + the fiber-yield integration.
    ObservabilityPrims::register_stats_impl(
        "query:arena-auto-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t guard_calls = 0;
            std::uint64_t yield_checks = 0;
            std::uint64_t trigger_count = 0;
            std::uint64_t skip_count = 0;
            // Read all 4 counters directly from the ArenaGroup
            // (the bump happens in MutationBoundaryGuard dtor
            // on ev_->arena_group_). The compiler_metrics_
            // field is the in-process metrics struct used by
            // the snapshot() helper; for EDSL primitives we
            // read from the source of truth (ArenaGroup) so
            // the counter advances immediately without
            // requiring a metrics copy.
            guard_calls = ev.arena_group().auto_compact_guard_call_count();
            yield_checks = ev.arena_group().compaction_yield_checks_group();
            trigger_count = ev.arena_group().auto_compact_trigger_count();
            skip_count = ev.arena_group().auto_compact_skip_count();
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
                {"auto-compact-guard-call-count", make_int(static_cast<std::int64_t>(guard_calls))},
                {"compaction-yield-checks", make_int(static_cast<std::int64_t>(yield_checks))},
                {"auto-compact-trigger-count", make_int(static_cast<std::int64_t>(trigger_count))},
                {"auto-compact-skip-count", make_int(static_cast<std::int64_t>(skip_count))},
            };
            return build_hash(kv);
        });
}

} // namespace aura::compiler::primitives_detail
