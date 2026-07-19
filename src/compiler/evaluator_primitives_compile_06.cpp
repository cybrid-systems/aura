// evaluator_primitives_compile_06.cpp — Issue #909: peeled compile registration
// aura.compiler.evaluator module partition.

module;

#include "runtime_shared.h"
#include "observability_metrics.h"
#include "per_defuse_index.h"
#include "hash_meta.h"
#include "basis_points.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.mutators;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.value;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.pass_manager;
import aura.compiler.query;
import aura.compiler.hardware_backend;
import aura.compiler.sv_ir;

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

// Issue #909 compile part 48 (orig 3950-4082)
void CompilePrims::register_compile_p48(PrimRegistrar add, Evaluator& ev) {

    // (compile:per-symbol-reinfer-stats) — Issue #411
    // follow-up #1: per-symbol re-inference path
    // observability. Returns a hash with 6 fields:
    //   - per-symbol-used-total: lifetime count of
    //     mutations that took the per-symbol path (the
    //     fast path, O(uses))
    //   - per-symbol-visited-total: total nodes visited
    //     across all per-symbol invocations
    //   - ancestor-used-total: lifetime count of mutations
    //     that fell back to the ancestor walk (the slow
    //     path, O(depth))
    //   - ancestor-visited-total: total nodes visited
    //     across all ancestor invocations
    //   - path-share-bp: derived share of re-inference
    //     work that went through the per-symbol path
    //     (per_symbol_visited / total_visited * ::aura::compiler::kBasisPointScale,
    //     basis points). Higher = more work on the fast
    //     path.
    //   - avg-per-symbol-bp: derived average re-inferred
    //     nodes per per-symbol mutation
    //     (per_symbol_visited / max(per_symbol_used, 1) *
    //     10000). The follow-up #410 Phase 2/2 (O(uses)
    //     DefUseIndex routing) will reduce this metric
    //     further by replacing the O(n) per_symbol walk
    //     with an O(uses) indexed lookup.
    ObservabilityPrims::register_stats_impl(
        "compile:per-symbol-reinfer-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                // Issue #411 fu1 follow-up #2: 10 keys now
                // (4 raw per-symbol + 2 derived + 4 raw
                // per-DefUseIndex). Cap 8 is too small — the
                // insert loop would fail at the 9th key,
                // destroy the table, and return make_void().
                // Use cap = next_pow2(max(8, kv.size() * 2))
                // so the open-addressing loop's
                // `(h >> 1 + at) & (hcap - 1)` masking works
                // correctly (the mask is only correct for
                // power-of-2 caps).
                std::size_t cap = std::max<std::size_t>(8, kv.size() * 2);
                // Round up to next power of 2.
                std::size_t p2 = 1;
                while (p2 < cap)
                    p2 <<= 1;
                cap = p2;
                auto* ht = FlatHashTable::create(cap);
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
                        fp = 0xFE; // avoid HASH_EMPTY collision
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
            std::uint64_t per_symbol_used = 0, per_symbol_visited = 0;
            std::uint64_t ancestor_used = 0, ancestor_visited = 0;
            // Issue #411 fu1 follow-up #2: per-DefUseIndex
            // tracker metrics (read from the same metrics
            // pointer as the per_symbol / ancestor counters).
            std::uint64_t per_defuse_index_used = 0;
            std::uint64_t per_defuse_index_visited = 0;
            std::uint64_t per_defuse_index_walk_fallback = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                per_symbol_used = m->per_symbol_reinfer_used_total.load(std::memory_order_relaxed);
                per_symbol_visited =
                    m->per_symbol_reinfer_visited_total.load(std::memory_order_relaxed);
                ancestor_used = m->ancestor_reinfer_used_total.load(std::memory_order_relaxed);
                ancestor_visited =
                    m->ancestor_reinfer_visited_total.load(std::memory_order_relaxed);
                per_defuse_index_used =
                    m->per_defuse_index_used_total.load(std::memory_order_relaxed);
                per_defuse_index_visited =
                    m->per_defuse_index_visited_total.load(std::memory_order_relaxed);
                per_defuse_index_walk_fallback =
                    m->per_defuse_index_walk_fallback_total.load(std::memory_order_relaxed);
            }
            const std::uint64_t total_visited = per_symbol_visited + ancestor_visited;
            const std::uint64_t path_share_bp =
                (total_visited > 0) ? (per_symbol_visited * 10000u) / total_visited : 0;
            const std::uint64_t avg_per_symbol_bp =
                (per_symbol_used > 0) ? (per_symbol_visited * 10000u) / per_symbol_used : 0;
            const std::uint64_t per_defuse_index_visited_avg_bp =
                (per_defuse_index_used > 0)
                    ? (per_defuse_index_visited * 10000u) / per_defuse_index_used
                    : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"per-symbol-used-total", make_int(static_cast<std::int64_t>(per_symbol_used))},
                {"per-symbol-visited-total",
                 make_int(static_cast<std::int64_t>(per_symbol_visited))},
                {"ancestor-used-total", make_int(static_cast<std::int64_t>(ancestor_used))},
                {"ancestor-visited-total", make_int(static_cast<std::int64_t>(ancestor_visited))},
                {"path-share-bp", make_int(static_cast<std::int64_t>(path_share_bp))},
                {"avg-per-symbol-bp", make_int(static_cast<std::int64_t>(avg_per_symbol_bp))},
                // Issue #411 fu1 follow-up #2: per-DefUseIndex
                // tracker observability (the underlying data
                // structure for the per-symbol O(uses) path).
                {"per-defuse-index-used-total",
                 make_int(static_cast<std::int64_t>(per_defuse_index_used))},
                {"per-defuse-index-visited-total",
                 make_int(static_cast<std::int64_t>(per_defuse_index_visited))},
                {"per-defuse-index-walk-fallback-total",
                 make_int(static_cast<std::int64_t>(per_defuse_index_walk_fallback))},
                {"per-defuse-index-visited-avg-bp",
                 make_int(static_cast<std::int64_t>(per_defuse_index_visited_avg_bp))},
            };
            return build_hash(kv);
        });
}

// Issue #909 compile part 49 (orig 4083-4191)
void CompilePrims::register_compile_p49(PrimRegistrar add, Evaluator& ev) {

    // (compile:per-defuse-index-add <idx-name> <caller-node-id>)
    //   — Issue #411 fu1 follow-up #2: add a caller to a
    //   per-DefUseIndex tracker. The tracker lives on
    //   CompilerService (per-service state, lifetime =
    //   service lifetime). The Aura primitive surface lets
    //   users register per-DefUseIndex call sites
    //   explicitly, which is the same pattern as the
    //   existing dep_caller_fn_ registration hooks. The
    //   future per-DefUseIndex re-inference path will
    //   read this tracker to look up the use-sites of a
    //   binding in O(uses) instead of the current O(n) walk.
    //
    //   Issue #411 fu1 fu4: the second arg is now a
    //   NodeId (int) instead of a string. The tracker
    //   stores NodeIds directly so the indexed lookup
    //   in TypeChecker::infer_flat_partial can iterate
    //   the use-sites without the O(n) walk.
    //
    //   Returns the new size_for_index for the index.
    //
    // Issue #1845: wrap tracker mutation in MutationBoundaryGuard
    // + try/catch. Pre-#1845 called add_caller raw — throw mid
    // map/vector growth left the tracker partially consistent
    // with no panic-checkpoint restore. compiler_service_ is
    // non-owning (#1839 ownership contract); concurrent free
    // mid-eval is unsupported.
    add("compile:per-defuse-index-add", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1]))
            return make_void();
        if (!ev.compiler_service_)
            return make_int(0);
        // Issue #1040: bounds-check string heap before index.
        const auto name_idx = as_string_idx(a[0]);
        if (name_idx >= ev.string_heap_.size())
            return make_void();
        auto* svc = static_cast<class CompilerService*>(ev.compiler_service_);
        const std::string idx_name = ev.string_heap_[name_idx];
        const auto caller_node_id = static_cast<aura::ast::NodeId>(as_int(a[1]));
        using aura::compiler::per_defuse_index::DefUseIndex;
        using aura::compiler::per_defuse_index::Caller;
        bool guard_ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &guard_ok);
        try {
            svc->per_defuse_index_tracker().add_caller(DefUseIndex{idx_name},
                                                       Caller{caller_node_id});
            return make_int(static_cast<std::int64_t>(
                svc->per_defuse_index_tracker().size_for_index(DefUseIndex{idx_name})));
        } catch (const std::exception&) {
            guard_ok = false; // Issue #1845: restore panic checkpoint
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->eda_guard_exception_handled_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_int(-1);
        } catch (...) {
            // [SILENCE-PRIM-#615] Guard-path uncaught → -1 + metrics
            // (eda_guard_uncaught_exception_total); dtor restores
            // (#1669 class A intentional-return-value).
            guard_ok = false;
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->eda_guard_uncaught_exception_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_int(-1);
        }
    });

    // (compile:per-defuse-index-callers <idx-name>)
    //   — Issue #411 fu1 follow-up #2: return the list of
    //   callers registered for a specific DefUseIndex as a
    //   hash. Keys are caller NodeIds (stringified via
    //   std::to_string) since the Aura hash needs string
    //   keys; values are the same NodeId as int (for
    //   programmatic lookup via hash-ref). The NodeId is
    //   the use-site that the type-checker will
    //   re-infer when the per-DefUseIndex path fires
    //   (Issue #411 fu1 fu4). Used by
    //   test_issue_411_followup_2/3 to verify per-DefUseIndex
    //   isolation + the fu4 test to verify the indexed
    //   lookup returns the right use-sites.
    //
    // Issue #1846: get_callers is thread-safe vs concurrent
    // add_caller (tracker internal spinlock).
    add("compile:per-defuse-index-callers", [&ev](const auto& a) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(std::max<std::size_t>(8, kv.size() * 2));
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
                    fp = 0xFE; // avoid HASH_EMPTY collision
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
        if (a.empty() || !is_string(a[0]))
            return make_void();
        if (!ev.compiler_service_)
            return make_int(0);
        // Issue #1040: bounds-check string heap before index.
        const auto name_idx = as_string_idx(a[0]);
        if (name_idx >= ev.string_heap_.size())
            return make_void();
        auto* svc = static_cast<class CompilerService*>(ev.compiler_service_);
        const std::string idx_name = ev.string_heap_[name_idx];
        using aura::compiler::per_defuse_index::DefUseIndex;
        auto callers = svc->per_defuse_index_tracker().get_callers(DefUseIndex{idx_name});
        std::vector<std::pair<std::string, EvalValue>> kv;
        kv.reserve(callers.size());
        for (std::size_t i = 0; i < callers.size(); ++i) {
            // Key: stringified NodeId (so hash keys are
            // strings). Value: same NodeId as int. This
            // way the test can do (hash-ref callers
            // "<nodeid>") to check the presence + get
            // the NodeId back.
            const std::string key = std::to_string(callers[i].node_id);
            kv.push_back({key, make_int(static_cast<std::int64_t>(callers[i].node_id))});
        }
        return build_hash(kv);
    });
}

// Issue #909 compile part 50 (orig 4192-4253)
void CompilePrims::register_compile_p50(PrimRegistrar add, Evaluator& ev) {

    // (compile:per-defuse-index-stats)
    //   — Issue #411 fu1 follow-up #2: snapshot of the
    //   per-DefUseIndex tracker's internal state. Returns
    //   a hash with 3 fields: total-size, index-count,
    //   defuse-service-ptr (the pointer to the
    //   CompilerService that owns the tracker, exposed
    //   for debugging only). Used by
    //   test_issue_411_followup_2 to verify the tracker
    //   is wired into the service.
    ObservabilityPrims::register_stats_impl(
        "compile:per-defuse-index-stats", [&ev](const auto&) -> EvalValue {
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
            if (!ev.compiler_service_)
                return make_int(0);
            auto* svc = static_cast<class CompilerService*>(ev.compiler_service_);
            const auto& tracker = svc->per_defuse_index_tracker();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"total-size", make_int(static_cast<std::int64_t>(tracker.total_size()))},
                {"index-count", make_int(static_cast<std::int64_t>(tracker.index_count()))},
                {"defuse-service-ptr",
                 make_int(static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(svc)))},
            };
            return build_hash(kv);
        });
}

// Issue #909 compile part 51 (orig 4254-4322)
void CompilePrims::register_compile_p51(PrimRegistrar add, Evaluator& ev) {

    // (compile:mutation-log-invalidation-stats)
    //   — Issue #413: snapshot of the mutation_log-
    //   integrated invalidation trace. Returns a hash
    //   with 2 fields: records-total (lifetime total
    //   of (mutation_id, SymId) traces recorded) +
    //   trace-size (current vector size in the active
    //   workspace FlatAST). The difference between
    //   trace-size and records-total indicates how
    //   many traces were accumulated in prior
    //   workspaces that have since been swapped out.
    ObservabilityPrims::register_stats_impl(
        "compile:mutation-log-invalidation-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto cap = std::max<std::size_t>(8, kv.size() * 2);
                // Round up to next power of 2.
                std::size_t hcap = 8;
                while (hcap < cap)
                    hcap *= 2;
                auto* ht = FlatHashTable::create(hcap);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
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
            if (!ev.compiler_service_)
                return make_int(0);
            auto* svc = static_cast<class CompilerService*>(ev.compiler_service_);
            std::uint64_t trace_size = 0;
            if (auto* ws = ev.workspace_flat()) {
                trace_size = static_cast<std::uint64_t>(ws->invalidation_trace_size());
            }
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"records-total", make_int(static_cast<std::int64_t>(
                                      svc->snapshot().invalidation_trace_records_total))},
                {"trace-size", make_int(static_cast<std::int64_t>(trace_size))},
            };
            return build_hash(kv);
        });
}

// Issue #909 compile part 52 (orig 4323-4377)
void CompilePrims::register_compile_p52(PrimRegistrar add, Evaluator& ev) {

    // (compile:subtree-bump subtree-root-id)
    //   → int (1 = bumped, 0 = no-op)
    //   Issue #392: scoped / per-subtree generation bumping.
    //   Walks up from subtree-root-id to find the enclosing
    //   top-level Define, then bumps that subtree's
    //   subtree_gen_ counter. Also bumps the global
    //   generation_ for backward compatibility with the
    //   existing is_valid() path (which checks global gen).
    //
    //   The benefit of the scoped approach shows up via the
    //   C++ is_valid_subtree() method: refs in OTHER
    //   subtrees stay valid because their subtree_gen_ was
    //   not bumped. Use (compile:subtree-generation id) to
    //   read the per-subtree counter; (compile:subtree-bump-count)
    //   to read the lifetime total.
    //
    //   subtree-root-id must be a NodeId (integer). Returns
    //   1 if the bump happened, 0 if the id was out-of-range
    //   or had no enclosing Define. Use this in long-running
    //   EDSL loops that hold many StableRefs across subtree
    //   boundaries — AI agent iteration, RTL/SV verification
    //   flows, large SoC designs with thousands of defines.
    //
    // Issue #1847: wrap bump_generation_subtree in
    // MutationBoundaryGuard + try/catch. Pre-#1847 mutated
    // subtree_gen_ / generation_ raw — a throw mid ancestor
    // walk left counters partially consistent with no panic
    // checkpoint restore. Outermost Guard captures checkpoint;
    // on exception flip guard_ok=false so dtor restores
    // (#184/#236). Metrics mirror #1842/#1845 Guard path.
    add("compile:subtree-bump", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return ev.make_merr("bad-arg", "usage: (compile:subtree-bump subtree-root-id)");
        const auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (!ev.workspace_flat_)
            return make_int(0);
        bool guard_ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &guard_ok);
        try {
            const auto before = ev.workspace_flat_->subtree_bump_count();
            ev.workspace_flat_->bump_generation_subtree(id);
            const auto after = ev.workspace_flat_->subtree_bump_count();
            return make_int(after > before ? 1 : 0);
        } catch (const std::exception&) {
            guard_ok = false; // Issue #1847: restore panic checkpoint
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->eda_guard_exception_handled_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_int(-1);
        } catch (...) {
            // [SILENCE-PRIM-#615] Guard-path uncaught → -1 + metrics
            // (eda_guard_uncaught_exception_total); dtor restores
            // (#1669 class A intentional-return-value).
            guard_ok = false;
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->eda_guard_uncaught_exception_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_int(-1);
        }
    });

    // (compile:subtree-generation subtree-root-id)
    //   → int (subtree generation, 0 = never bumped)
    //   Issue #392: read the per-top-level-Define subtree
    //   generation counter for the Define ancestor of
    //   subtree-root-id. Returns 0 if there is no enclosing
    //   Define or the id is out-of-range.
    //
    //   The subtree generation is bumped by
    //   (compile:subtree-bump subtree-root-id) and by
    //   FlatAST::bump_generation_subtree(). is_valid_subtree()
    //   (C++) compares the captured subtree_gen_at_capture
    //   against this counter.
    //
    // Issue #1848: shared_lock workspace_mtx_ while reading
    // subtree_gen_ / walking top_define_of. Concurrent
    // compile:subtree-bump (#1847 Guard unique_lock) may
    // resize subtree_gen_ or tear the uint16_t counter —
    // without a reader lock this is a data race / UAF.
    ObservabilityPrims::register_stats_impl(
        "compile:subtree-generation", [&ev](const auto& a) -> EvalValue {
            if (a.empty() || !is_int(a[0]))
                return ev.make_merr("bad-arg",
                                    "usage: (compile:subtree-generation subtree-root-id)");
            const auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
            // Issue #1848: shared vs #1847 Guard unique_lock.
            std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
            if (!ev.workspace_flat_)
                return make_int(0);
            return make_int(static_cast<std::int64_t>(ev.workspace_flat_->subtree_generation(id)));
        });
}

// Issue #909 compile part 53 (orig 4378-4487)
void CompilePrims::register_compile_p53(PrimRegistrar add, Evaluator& ev) {

    // (compile:subtree-bump-count)
    //   → int (lifetime total of subtree-bump calls)
    //   Issue #392: observability for the scoped-bump path.
    //   Mirrors the C++ accessor FlatAST::subtree_bump_count().
    //   Increments each time (compile:subtree-bump id) or
    //   FlatAST::bump_generation_subtree() actually bumps a
    //   subtree (excludes the no-op when the id has no
    //   enclosing Define).
    //
    // Issue #1848: shared_lock so the atomic load pairs with
    // #1847 writer's exclusive Guard (acquire/release vs
    // concurrent bump_generation_subtree). Same race class as
    // compile:subtree-generation above.
    ObservabilityPrims::register_stats_impl(
        "compile:subtree-bump-count", [&ev](const auto&) -> EvalValue {
            // Issue #1848: shared vs #1847 Guard unique_lock.
            std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
            if (!ev.workspace_flat_)
                return make_int(0);
            return make_int(static_cast<std::int64_t>(ev.workspace_flat_->subtree_bump_count()));
        });

    // (compile:mutator-dispatch-stats)
    //   — Issue #501 follow-up #4: snapshot of the
    //   MutatorDispatchStats counters from aura.core.mutators.
    //   Returns an alist with:
    //     :total                 — total dispatch calls
    //     :apply-mutation-total  — direct apply_mutation<> calls
    //     :apply-by-kind-total   — apply_by_kind() dispatch calls
    //     :apply-by-name-total   — apply_by_name() dispatch calls
    //     :failure-total         — dispatched calls returning AuraError
    //     :noop-success          — NoOpMutator successes
    //     :replace-child-success — ReplaceChildMutator successes
    //     :insert-child-success  — InsertChildMutator successes
    //     :remove-child-success  — RemoveChildMutator successes
    //     :replace-child-failure — ReplaceChildMutator failures
    //     :insert-child-failure  — InsertChildMutator failures
    //     :remove-child-failure  — RemoveChildMutator failures
    //   The AI agent reads this to see which strategies get
    //   the most traffic (and which always roll back).
    //
    // Issue #1849: capture() under shared_lock so multi-field
    // snapshot is coherent vs concurrent apply_mutation /
    // apply_by_kind / apply_by_name unique bumps. Pre-#1849
    // loaded each atomic with relaxed independently — torn
    // totals across the alist.
    ObservabilityPrims::register_stats_impl(
        "compile:mutator-dispatch-stats", [&ev](const auto&) -> EvalValue {
            // Issue #1849: coherent snapshot (shared_lock inside capture).
            const auto s = aura::ast::mutators::dispatch_stats().capture();

            auto cvt = [&](std::uint64_t n) -> EvalValue {
                auto idx = ev.string_heap_.size();
                ev.string_heap_.push_back(std::to_string(n));
                return make_string(idx);
            };

            auto add_entry = [&](const std::string& key, EvalValue val) -> std::uint64_t {
                auto key_idx = ev.string_heap_.size();
                ev.string_heap_.push_back(key);
                auto entry_pair = ev.pairs_.size();
                ev.pairs_.push_back({make_string(key_idx), val});
                return entry_pair;
            };

            EvalValue result = make_void();
            auto cons = [&](std::uint64_t entry_id) {
                auto cons_pair = ev.pairs_.size();
                ev.pairs_.push_back({make_pair(entry_id), result});
                result = make_pair(cons_pair);
            };

            // 12 entries: total + 3 dispatcher counters + 1 failure +
            // 4 success + 3 failure-per-kind (NoOp never fails).
            using aura::ast::mutators::kind_index;
            using aura::ast::mutators::StrategyKind;
            auto e_total = add_entry(":total", cvt(s.total()));
            auto e_amut = add_entry(":apply-mutation-total", cvt(s.apply_mutation_total));
            auto e_aknd = add_entry(":apply-by-kind-total", cvt(s.apply_by_kind_total));
            auto e_anam = add_entry(":apply-by-name-total", cvt(s.apply_by_name_total));
            auto e_fail = add_entry(":failure-total", cvt(s.failure_total));
            auto e_nsucc =
                add_entry(":noop-success", cvt(s.kind_success[kind_index(StrategyKind::NoOp)]));
            auto e_rsucc = add_entry(":replace-child-success",
                                     cvt(s.kind_success[kind_index(StrategyKind::ReplaceChild)]));
            auto e_isucc = add_entry(":insert-child-success",
                                     cvt(s.kind_success[kind_index(StrategyKind::InsertChild)]));
            auto e_xsucc = add_entry(":remove-child-success",
                                     cvt(s.kind_success[kind_index(StrategyKind::RemoveChild)]));
            auto e_rfail = add_entry(":replace-child-failure",
                                     cvt(s.kind_failure[kind_index(StrategyKind::ReplaceChild)]));
            auto e_ifail = add_entry(":insert-child-failure",
                                     cvt(s.kind_failure[kind_index(StrategyKind::InsertChild)]));
            auto e_xfail = add_entry(":remove-child-failure",
                                     cvt(s.kind_failure[kind_index(StrategyKind::RemoveChild)]));

            // Cons them onto the result list (in reverse so the head is :total).
            std::uint64_t entries[] = {e_xfail, e_ifail, e_rfail, e_xsucc, e_isucc, e_rsucc,
                                       e_nsucc, e_fail,  e_anam,  e_aknd,  e_amut,  e_total};
            for (auto eid : entries)
                cons(eid);
            return result;
        });
}

// Issue #909 compile part 54 (orig 4488-4558)
void CompilePrims::register_compile_p54(PrimRegistrar add, Evaluator& ev) {

    // ═══════════════════════════════════════════════════════════
    // Issue #308: hardware BitVector type primitives.
    //
    // Three primitives expose the BitVecType side-table that
    // TypeRegistry::register_hw_bitvec populates:
    //
    //   (compile:hw-bitvec-register <type-name> <width> <signed?>)
    //     Marks `type-name` as a hardware BitVector with the
    //     given width (bit count, e.g. 8/16/32/64) and
    //     signedness (true = two's-complement signed).
    //     Idempotent for the same (width, signed) pair.
    //     Returns 1 on success, 0 if the type doesn't exist.
    //
    //   (compile:hw-bitvec-width <type-name>)
    //     Returns the BitVector width (e.g. 8 for uint8_t)
    //     or 0 if the type is not registered as a hw bitvec.
    //
    //   (compile:hw-bitvec-signed? <type-name>)
    //     Returns 1 if the type is a signed hw bitvec,
    //     0 if unsigned, 0 if not a hw bitvec.
    //
    //   (compile:hw-bitvec-compatible? <a-name> <b-name>)
    //     Returns 1 if both types are hw bitvecs with the
    //     SAME width AND signedness (i.e. they're the same
    //     hardware type). Returns 0 on any mismatch (different
    //     width, different signedness, or one/both not
    //     registered). The canonical hardware bug caught
    //     here is `assigning uint8_t to uint16_t` — caught
    //     at type-check time via this primitive.
    //
    // Why these primitives:
    //   - BitVector types are a side-table populated via
    //     (compile:hw-bitvec-register), not via the type
    //     constructor. The primitives let the user register
    //     a type as a hw bitvec without modifying the parser
    //     or typesystem.md (which is the follow-up).
    //   - The (compile:hw-bitvec-compatible?) primitive IS
    //     the AC2 "BitVector width mismatch caught at type
    //     check time" — the user code calls it at the
    //     binding/check point and reports the diagnostic.
    //   - Future #308 follow-ups: native BitVector type in
    //     the parser (e.g. (BitVec 8) form), automatic
    //     width-mismatch diagnostic in InferenceEngine's
    //     subtyping path, Clock/Reset domain tracking.
    //
    // Issue #1850: wrap register_type / register_hw_bitvec in
    // MutationBoundaryGuard + try/catch. Pre-#1850 mutated the
    // TypeRegistry raw — throw mid map/side-table growth left
    // partial state with no panic-checkpoint restore.
    // type_registry_ is non-owning when wired from
    // CompilerService (#1837 ownership / quiescence contract);
    // concurrent set_type_registry / free mid-eval is unsupported
    // (same class as #1835 metrics / #1839 service — no shared_ptr
    // tax on every type lookup).
    add("compile:hw-bitvec-register", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 3 || !is_string(a[0]) || !is_int(a[1]) || !is_int(a[2])) {
            return ev.make_merr("bad-arg",
                                "usage: (compile:hw-bitvec-register type-name width signed?)");
        }
        auto sidx = as_string_idx(a[0]);
        std::string name;
        if (sidx < ev.string_heap_.size())
            name = ev.string_heap_[sidx];
        else
            return ev.make_merr("bad-arg", "type name string index out of range");
        // Issue #1837 / #1850: ensure under eval quiescence; raw
        // pointee lives for service lifetime (or owned until
        // set_type_registry replaces it under quiescence).
        auto* reg_ptr = static_cast<aura::core::TypeRegistry*>(ev.ensure_type_registry());
        if (!reg_ptr)
            return ev.make_merr("no-registry", "type registry unavailable");
        auto& reg = *reg_ptr;
        const auto width = static_cast<std::uint32_t>(as_int(a[1]));
        const bool is_signed = as_int(a[2]) != 0;
        bool guard_ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &guard_ok);
        try {
            auto tid = reg.lookup_type(name);
            // Auto-register the type as INT if it doesn't exist.
            // The hardware BitVector is an integer-like type
            // (uint8_t / int16_t / etc.), so INT is a sensible
            // default tag. Pre-existing types (registered with
            // other tags via declare-type) are kept.
            if (!tid.valid()) {
                tid = reg.register_type(aura::core::TypeTag::INT, name);
            }
            reg.register_hw_bitvec(tid, width, is_signed);
            return make_int(1);
        } catch (const std::exception&) {
            guard_ok = false; // Issue #1850: restore panic checkpoint
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->eda_guard_exception_handled_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_int(-1);
        } catch (...) {
            // [SILENCE-PRIM-#615] Guard-path uncaught → -1 + metrics
            // (eda_guard_uncaught_exception_total); dtor restores
            // (#1669 class A intentional-return-value).
            guard_ok = false;
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->eda_guard_uncaught_exception_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_int(-1);
        }
    });
}

// Issue #909 compile part 55 (orig 4559-4630)
void CompilePrims::register_compile_p55(PrimRegistrar add, Evaluator& ev) {

    add("compile:hw-bitvec-width", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return ev.make_merr("bad-arg", "usage: (compile:hw-bitvec-width type-name)");
        auto sidx = as_string_idx(a[0]);
        std::string name;
        if (sidx < ev.string_heap_.size())
            name = ev.string_heap_[sidx];
        else
            return make_int(0);
        if (!ev.type_registry_)
            return make_int(0);
        auto& reg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
        auto tid = reg.lookup_type(name);
        if (!tid.valid())
            return make_int(0);
        auto* bv = reg.hw_bitvec_of(tid);
        if (!bv)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(bv->width));
    });

    add("compile:hw-bitvec-signed?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return ev.make_merr("bad-arg", "usage: (compile:hw-bitvec-signed? type-name)");
        auto sidx = as_string_idx(a[0]);
        std::string name;
        if (sidx < ev.string_heap_.size())
            name = ev.string_heap_[sidx];
        else
            return make_int(0);
        if (!ev.type_registry_)
            return make_int(0);
        auto& reg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
        auto tid = reg.lookup_type(name);
        if (!tid.valid())
            return make_int(0);
        auto* bv = reg.hw_bitvec_of(tid);
        if (!bv)
            return make_int(0);
        return make_int(bv->is_signed ? 1 : 0);
    });

    add("compile:hw-bitvec-compatible?", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return ev.make_merr("bad-arg",
                                "usage: (compile:hw-bitvec-compatible? type-a-name type-b-name)");
        auto asx = as_string_idx(a[0]);
        auto bsx = as_string_idx(a[1]);
        std::string an, bn;
        if (asx < ev.string_heap_.size())
            an = ev.string_heap_[asx];
        if (bsx < ev.string_heap_.size())
            bn = ev.string_heap_[bsx];
        if (!ev.type_registry_)
            return make_int(0);
        auto& reg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
        auto ta = reg.lookup_type(an);
        auto tb = reg.lookup_type(bn);
        if (!ta.valid() || !tb.valid())
            return make_int(0);
        auto* ba = reg.hw_bitvec_of(ta);
        auto* bb = reg.hw_bitvec_of(tb);
        if (!ba || !bb)
            return make_int(0); // one or both not registered as hw bitvecs
        // Compatible iff SAME width AND SAME signedness.
        // The canonical hardware bug: uint8_t vs uint16_t
        // (different widths), uint8_t vs int8_t (different
        // signedness), or any other mismatch.
        const bool ok = (ba->width == bb->width) && (ba->is_signed == bb->is_signed);
        return make_int(ok ? 1 : 0);
    });
}

} // namespace aura::compiler::primitives_detail
