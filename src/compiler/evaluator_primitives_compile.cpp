// evaluator_primitives_compile.cpp — Issue #1944 / Phase 1: split numbered files consolidated.
// Consolidated from evaluator_primitives_compile_00..07.cpp (#1944 acceptance: split files
// reduced). aura.compiler.evaluator module partition; registered via
// evaluator_primitives_registry.cpp.
//
// Issue #1972: five seva:* adds in register_compile_p59–p62. Gated by
// AURA_ENABLE_SEVA (see docs/seva.md). query:seva-audit-log stays always on.

module;

#include "runtime_shared.h"
#include "observability_metrics.h"
#include "primitives_detail.h"
#include "per_defuse_index.h"
#include "hash_meta.h"
#include "basis_points.h"
#include "security_capabilities.h"

#ifndef AURA_ENABLE_SEVA
#define AURA_ENABLE_SEVA 1
#endif

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
import aura.compiler.macro_expansion;
import aura.compiler.hardware_backend;

// Issue #1950: after module + imports so Evaluator/EvalValue are in scope.
#include "compiler/mutation_guard_helpers.hh"

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

// Issue #1896 / #1897: run a structural mutator under MutationBoundaryGuard::
// try_acquire + try/catch so exceptions restore panic checkpoint and never
// leave partial workspace/IR state. On quota/guard failure bumps
// compile_primitive_stale_ir_prevented_total; on throw bumps
// mutation_guard_exception_total (+ lineage eda_guard_* counters, retired 4.4).
// `on_fail` is returned for both try_acquire reject and caught exceptions
// (bool #f for most dirty! paths; make_int(-1) for compact/subtree/defuse).

// Issue #1896 alias: dirty-bit mutators default on_fail = #f.
template <typename F> EvalValue run_compile_dirty_under_guard(Evaluator& ev, F&& body) {
    return run_under_mutation_guard(ev, std::forward<F>(body), make_bool(false));
}

// Issue #1898: pin compiler_service_ for multi-step stats readers.
// Seqlock-style pin at enter; revalidate after body. On rebind mid-flight
// bump raw_pointer_uaf_prevented_total + compiler_service_pin_reject_total
// and return on_miss (no false-clean partial hash).
template <typename F>
EvalValue with_compiler_service_pin(Evaluator& ev, F&& body, EvalValue on_miss = make_void()) {
    auto pin = ev.pin_compiler_service();
    if (!pin)
        return on_miss;
    auto* svc = static_cast<class CompilerService*>(pin.ptr);
    auto result = std::forward<F>(body)(*svc);
    if (!ev.compiler_service_pin_valid(pin)) {
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->raw_pointer_uaf_prevented_total.fetch_add(1, std::memory_order_relaxed);
            m->compiler_service_pin_reject_total.fetch_add(1, std::memory_order_relaxed);
        }
        return on_miss;
    }
    return result;
}

// Issue #909 compile part 0 (orig 72-123)
void CompilePrims::register_compile_p0(PrimRegistrar add, Evaluator& ev) {

    // (compile:linear-elide-count) — Issue #253: returns the
    // lifetime total of MoveOp instructions elided by
    // TypeSpecializationWrap (when source had
    // linear_ownership_state == Owned). Companion to
    // (closure:stats) above. Both read from the same shared
    // CompilerMetrics struct (ev.compiler_metrics_ pointer set
    // by service.ixx). Returns 0 if no service is bound
    // (legacy standalone Evaluator usage).
    ObservabilityPrims::register_stats_impl(
        "compile:linear-elide-count", [&ev](const auto&) -> EvalValue {
            std::uint64_t cnt = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                cnt = m->linear_elide_count.load(std::memory_order_relaxed);
            }
            return make_int(static_cast<std::int64_t>(cnt));
        });

    // (compile:dead-coercion-stats) — Issue #433: dead
    // coercion elimination observability. Returns
    // the lifetime total of CastOps eliminated by
    // the DeadCoercionEliminationPass. The pass
    // exists in pass_manager.ixx and was already
    // wired into the pipeline (service.ixx:1442);
    // #433 ships the observability so users can
    // measure zero-overhead gradual typing.
    ObservabilityPrims::register_stats_impl(
        "compile:dead-coercion-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t cnt = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                cnt = m->dead_coercion_eliminated_total.load(std::memory_order_relaxed);
            }
            return make_int(static_cast<std::int64_t>(cnt));
        });

    // (compile:dead-coercion-elapsed) — Issue #508:
    // cumulative microseconds spent in
    // DeadCoercionEliminationPass::run() across all calls.
    // Companion to (compile:dead-coercion-stats): the
    // count tells you "how much was eliminated"; the
    // elapsed time tells you "how expensive the pass
    // was". In typical workloads this should be
    // sub-millisecond even on large IR modules; spikes
    // point at pathological coercion chains.
    ObservabilityPrims::register_stats_impl(
        "compile:dead-coercion-elapsed", [&ev](const auto&) -> EvalValue {
            std::uint64_t us = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                us = m->dead_coercion_elapsed_us_total.load(std::memory_order_relaxed);
            }
            return make_int(static_cast<std::int64_t>(us));
        });
}

// Issue #909 compile part 1 (orig 124-221)
void CompilePrims::register_compile_p1(PrimRegistrar add, Evaluator& ev) {

    // (compile:dead-coercion-kept-for-debug) — Issue #508:
    // total CastOps that would have been eliminated when
    // keep_for_debug was set (blame-mode observability).
    ObservabilityPrims::register_stats_impl(
        "compile:dead-coercion-kept-for-debug", [&ev](const auto&) -> EvalValue {
            std::uint64_t cnt = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                cnt = m->dead_coercion_kept_for_debug_total.load(std::memory_order_relaxed);
            }
            return make_int(static_cast<std::int64_t>(cnt));
        });

    // (query:dead-coercion-elim-stats) — Issue #687: hash
    // dashboard for the DeadCoercionEliminationPass +
    // IR-interpreter identity fast-path (P0 production
    // zero-overhead gradual typing). Non-duplicative with
    // (compile:dead-coercion-{stats,elapsed,kept-for-debug})
    // (#433/#508) which each return a single int; #687
    // carves out a hash primitive so the AI Agent gets
    // the full zero-overhead-elimination surface in one
    // call.
    //
    // Schema (5 fields + sentinel):
    //   - casts-eliminated        dead_coercion_eliminated_total
    //                             (lowering pass; the canonical
    //                             "how many CastOps did we
    //                             statically eliminate" counter)
    //   - residual-hotpath        dead_coercion_kept_for_debug_total
    //                             (counted but not eliminated —
    //                             the residual runtime cost)
    //   - zero-overhead-savings   dead_coercion_elapsed_us_total
    //                             (cumulative μs the pass spent —
    //                             proxy for "how much backend
    //                             time we saved" via the avoided
    //                             switch dispatches)
    //   - post-mutate-elim-hits   dead_coercion_post_mutate_elim_hits_total
    //                             (IR-interpreter identity-cast
    //                             fast-path; runtime companion
    //                             to casts-eliminated)
    //   - hot-path-rate           derived
    //                             (post-mutate-elim-hits /
    //                             (post-mutate-elim-hits +
    //                              residual-hotpath)) * 100
    //                             — 100 means every CastOp is
    //                             eliminated or fast-path'd
    //   - schema == 687
    ObservabilityPrims::register_stats_impl(
        "query:dead-coercion-elim-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t casts_elim = 0, residual = 0, elapsed_us = 0, post_mutate = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                casts_elim = m->dead_coercion_eliminated_total.load(std::memory_order_relaxed);
                residual = m->dead_coercion_kept_for_debug_total.load(std::memory_order_relaxed);
                elapsed_us = m->dead_coercion_elapsed_us_total.load(std::memory_order_relaxed);
                post_mutate =
                    m->dead_coercion_post_mutate_elim_hits_total.load(std::memory_order_relaxed);
            }
            const std::uint64_t total_hotpath = post_mutate + residual;
            const std::int64_t hotpath_rate =
                total_hotpath > 0 ? static_cast<std::int64_t>((post_mutate * 100) / total_hotpath)
                                  : 100;
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
            insert_kv("casts-eliminated", static_cast<std::int64_t>(casts_elim));
            insert_kv("residual-hotpath", static_cast<std::int64_t>(residual));
            insert_kv("zero-overhead-savings", static_cast<std::int64_t>(elapsed_us));
            insert_kv("post-mutate-elim-hits", static_cast<std::int64_t>(post_mutate));
            insert_kv("hot-path-rate", hotpath_rate);
            insert_kv("schema", 687);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 compile part 2 (orig 222-286)
void CompilePrims::register_compile_p2(PrimRegistrar add, Evaluator& ev) {

    // Issue #799: (query:dead-coercion-elision-stats) — narrow_evidence-
    // driven DeadCoercionElimination elision dashboard (refines #796/#795/
    // #794; non-duplicative with #687 query:dead-coercion-elim-stats which
    // focuses on the general zero-overhead elimination surface).
    //
    // Fields (4 + sentinel):
    //   - elided-casts            dead_coercion_elision_elided_casts_total
    //   - evidence-hit-rate       derived (evidence_hits * 100 /
    //                             (evidence_hits + castop_emitted + 1))
    //   - narrowing-stable-paths  dead_coercion_elision_narrowing_stable_paths_total
    //   - runtime-check-savings   dead_coercion_elision_runtime_check_savings_total
    //   - schema == 799
    ObservabilityPrims::register_stats_impl(
        "query:dead-coercion-elision-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t elided = 0, evidence_hits = 0, stable_paths = 0, savings = 0,
                          castop_emitted = 0, narrow_mut = 0, dyn_pass = 0, rate_bp = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                elided =
                    m->dead_coercion_elision_elided_casts_total.load(std::memory_order_relaxed);
                evidence_hits =
                    m->dead_coercion_elision_evidence_hits_total.load(std::memory_order_relaxed);
                stable_paths = m->dead_coercion_elision_narrowing_stable_paths_total.load(
                    std::memory_order_relaxed);
                savings = m->dead_coercion_elision_runtime_check_savings_total.load(
                    std::memory_order_relaxed);
                castop_emitted = m->coercion_castop_emitted_total.load(std::memory_order_relaxed);
                narrow_mut =
                    m->dead_coercion_narrow_mutation_elided_total.load(std::memory_order_relaxed);
                dyn_pass =
                    m->dead_coercion_dynamic_passthrough_total.load(std::memory_order_relaxed);
                rate_bp = m->dead_coercion_elision_rate_bp.load(std::memory_order_relaxed);
            }
            const std::int64_t evidence_hit_rate = static_cast<std::int64_t>(
                (evidence_hits * 100ULL) / (evidence_hits + castop_emitted + 1ULL));
            // Power-of-2 capacity; #1925 adds ~8 keys.
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
            insert_kv("elided-casts", static_cast<std::int64_t>(elided));
            insert_kv("evidence-hit-rate", evidence_hit_rate);
            insert_kv("narrowing-stable-paths", static_cast<std::int64_t>(stable_paths));
            insert_kv("runtime-check-savings", static_cast<std::int64_t>(savings));
            // Issue #1925: post-mutate / occurrence-narrow elision surface
            insert_kv("narrow-mutation-elided", static_cast<std::int64_t>(narrow_mut));
            insert_kv("dynamic-passthrough-elided", static_cast<std::int64_t>(dyn_pass));
            insert_kv("elision-rate-bp", static_cast<std::int64_t>(rate_bp));
            insert_kv("elision-rate-target-bp", 9000); // 90% AC target
            insert_kv("narrow-mutation-wired", 1);
            insert_kv("schema-1925", 1925);
            insert_kv("issue-1925", 1925);
            insert_kv("schema", 799); // lineage 799 + #1925
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 compile part 3 (orig 287-364)
void CompilePrims::register_compile_p3(PrimRegistrar add, Evaluator& ev) {

    // (compile:ir-soa-stats) — Issue #254: observability for
    // the IR SoA dual-emit path. Hash with the 2 counters
    // (instructions-emitted, functions-emitted). Returns a
    // hash with the counts (both 0 if no lowering has
    // happened with dual-emit enabled, or if no service is
    // bound). Companion to (compile:linear-elide-count) above
    // + (closure:stats) — same ev.compiler_metrics_ pointer
    // pattern.
    ObservabilityPrims::register_stats_impl(
        "compile:ir-soa-stats", [&ev](const auto&) -> EvalValue {
            // Re-use the build_hash pattern from closure:stats
            // above (same FNV-1a hash + open-addressing insert).
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                // Issue #603: 5 fields now (instructions-emitted,
                // functions-emitted, view-cache-hits, block-dirty-hits,
                // relower-blocks-saved) — capacity 16 to fit with the
                // 50% max-load open-addressing policy.
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
            std::uint64_t instr = 0, funcs = 0;
            std::uint64_t view_hits = 0, dirty_hits = 0, blocks_saved = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                instr = m->ir_soa_instructions_emitted.load(std::memory_order_relaxed);
                funcs = m->ir_soa_functions_emitted.load(std::memory_order_relaxed);
                // Issue #603: full consumer adoption + per-block dirty_
                // driven minimal re-lower observability. The first two
                // fields (instructions/functions) cover dual-emit volumes;
                // the three below cover the hot-path consumer side.
                view_hits = m->ir_soa_view_cache_hits_total.load(std::memory_order_relaxed);
                dirty_hits = m->ir_soa_block_dirty_hits_total.load(std::memory_order_relaxed);
                blocks_saved = m->ir_soa_relower_blocks_saved_total.load(std::memory_order_relaxed);
            }
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"instructions-emitted", make_int(static_cast<std::int64_t>(instr))},
                {"functions-emitted", make_int(static_cast<std::int64_t>(funcs))},
                {"view-cache-hits", make_int(static_cast<std::int64_t>(view_hits))},
                {"block-dirty-hits", make_int(static_cast<std::int64_t>(dirty_hits))},
                {"relower-blocks-saved", make_int(static_cast<std::int64_t>(blocks_saved))},
            };
            return build_hash(kv);
        });
}

// Issue #909 compile part 4 (orig 365-443)
void CompilePrims::register_compile_p4(PrimRegistrar add, Evaluator& ev) {

    // (compile:invalidations-stats) — Issue #255: observability
    // for the FlatAST reference stability mechanism. Hash with
    // 4 counters:
    //   - bump-generation-count: total generation bumps
    //   - is-valid-check-count: total is_valid() calls
    //   - stable-ref-invalidations: StableNodeRef that went
    //     stale (ref.gen != current gen when is_valid(ref) was
    //     called)
    //   - atomic-batch-commits: atomic batches committed
    //     (each does 1 bump vs N individual bumps)
    // Returns a hash with the counts (all 0 if no workspace
    // is set, or if no service is bound). Companion to
    // (compile:linear-elide-count) and (compile:ir-soa-stats)
    // — the underlying counters live on the workspace's
    // FlatAST (not CompilerMetrics), so we read them via the
    // ev.workspace_flat() accessor on the Evaluator (which the
    // service.ixx snapshot() also uses).
    //
    // Issue #1851: shared_lock workspace_mtx_ while loading the
    // workspace_flat_ pointer and reading its counters. Pre-#1851
    // a concurrent set_workspace_flat (#1729 unique_lock swap)
    // could leave this reader with a stale FlatAST* after the
    // swap → UAF on counter loads. Pair with #1729 writer lock.
    ObservabilityPrims::register_stats_impl(
        "compile:invalidations-stats", [&ev](const auto&) -> EvalValue {
            // Re-use the build_hash pattern from compile:ir-soa-stats
            // above (same FNV-1a hash + open-addressing insert).
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
            std::uint64_t bumps = 0, checks = 0, inits = 0, commits = 0;
            // Issue #1851 / #1898: WorkspaceFlatPin holds shared_lock for
            // pointer load + counter snapshot (vs #1729 unique_lock swap).
            {
                auto pin = ev.pin_workspace_flat();
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                    m->workspace_flat_pin_total.fetch_add(1, std::memory_order_relaxed);
                if (pin) {
                    bumps = pin->bump_generation_count();
                    checks = pin->is_valid_check_count();
                    inits = pin->stable_ref_invalidations();
                    commits = pin->atomic_batch_commits_v();
                }
            }
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"bump-generation-count", make_int(static_cast<std::int64_t>(bumps))},
                {"is-valid-check-count", make_int(static_cast<std::int64_t>(checks))},
                {"stable-ref-invalidations", make_int(static_cast<std::int64_t>(inits))},
                {"atomic-batch-commits", make_int(static_cast<std::int64_t>(commits))},
            };
            return build_hash(kv);
        });
}

// Issue #909 compile part 5 (orig 444-532)
void CompilePrims::register_compile_p5(PrimRegistrar add, Evaluator& ev) {

    // (compile:ast-ops-stats) — Issue #256: observability for
    // the hand-written AST operations (children, parent_of,
    // mark_dirty_upward). Hash with 4 counters:
    //   - children-call-count: total children() calls
    //   - parent-of-call-count: total parent_of() calls
    //   - mark-dirty-upward-call-count: mark_dirty_upward()
    //     invocations
    //   - mark-dirty-total-nodes: total nodes touched across
    //     all mark_dirty_upward() calls. Divided by
    //     mark-dirty-upward-call-count gives the average
    //     dirty-propagation depth — the key metric for
    //     deciding if the std::meta refactor is worth it.
    // Returns a hash with the counts (all 0 if no workspace
    // is set). Companion to (compile:invalidations-stats).
    // (compile:macro-origin-provenance-errors) — Issue #1392:
    // observability primitive for the macro hygiene depth-exceeded
    // fallback. clone_macro_body (macro_expansion.cpp:206-228)
    // falls back to silent NULL_NODE + unhygienic substitution when
    // s_hygiene_depth >= MAX_HYGIENE_DEPTH (now 1024). Each fallback
    // path bumps the existing g_macro_origin_provenance_errors atomic
    // counter. This primitive exposes the counter so Agents can
    // detect the misconfiguration via (compile:macro-origin-provenance-errors)
    // and (eventually) wire it into (eval-warnings) emission.
    //
    // Returning a NodeId-typed merr would require changing
    // clone_macro_body's signature (invasive); observability path
    // is the scope-limited fix. Documented in macro_expansion.ixx.
    ObservabilityPrims::register_stats_impl(
        "compile:macro-origin-provenance-errors", [](const auto&) -> EvalValue {
            return make_int(static_cast<std::int64_t>(
                aura::compiler::macro_exp::g_macro_origin_provenance_errors.load(
                    std::memory_order_relaxed)));
        });

    // Issue #1852: shared_lock workspace_mtx_ for the whole
    // workspace_flat() counter snapshot (pre-#1852 read the
    // pointer twice unlocked — concurrent set_workspace_flat
    // #1729 could mix two workspaces or UAF on a freed flat).
    // Sibling of #1851 compile:invalidations-stats.
    ObservabilityPrims::register_stats_impl(
        "compile:ast-ops-stats", [&ev](const auto&) -> EvalValue {
            // Re-use the build_hash pattern from above primitives.
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
            std::uint64_t children_calls = 0, parent_calls = 0, dirty_calls = 0, dirty_nodes = 0;
            // Issue #336: include the mark_dirty_upward_fast
            // fixed-point early-exit counter so callers can
            // benchmark the optimization.
            std::uint64_t fast_hits = 0;
            // Issue #1852 / #1898: WorkspaceFlatPin covers pointer load +
            // all counter reads (vs #1729 set_workspace_flat unique_lock).
            {
                auto pin = ev.pin_workspace_flat();
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                    m->workspace_flat_pin_total.fetch_add(1, std::memory_order_relaxed);
                if (pin) {
                    children_calls = pin->children_call_count();
                    parent_calls = pin->parent_of_call_count();
                    dirty_calls = pin->mark_dirty_upward_call_count();
                    dirty_nodes = pin->mark_dirty_total_nodes();
                    fast_hits = pin->dirty_upward_fast_fixed_point_count();
                }
            }
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"children-call-count", make_int(static_cast<std::int64_t>(children_calls))},
                {"parent-of-call-count", make_int(static_cast<std::int64_t>(parent_calls))},
                {"mark-dirty-upward-call-count", make_int(static_cast<std::int64_t>(dirty_calls))},
                {"mark-dirty-total-nodes", make_int(static_cast<std::int64_t>(dirty_nodes))},
                // Issue #336: fixed-point hits (the
                // mark_dirty_upward_fast early-exit
                // counter). When this number is large
                // relative to mark-dirty-upward-call-count,
                // the fast path is paying off (most calls
                // hit a parent that was already dirty for
                // the target reasons).
                {"dirty-upward-fast-fixed-point-hits",
                 make_int(static_cast<std::int64_t>(fast_hits))},
            };
            return build_hash(kv);
        });
}

// Issue #909 compile part 6 (orig 533-628)
void CompilePrims::register_compile_p6(PrimRegistrar add, Evaluator& ev) {

    // (compile:multi-mutation-stats) — Issue #258: observability
    // for the multi-mutation incremental typecheck path.
    // Hash with 4 fields:
    //   - cache-hits-total: lifetime total clean nodes with
    //     valid cached types (skipped re-inference)
    //   - cache-misses-total: lifetime total nodes that were
    //     re-inferred (dirty or no cache)
    //   - stale-cache-total: lifetime total cached types
    //     rejected due to free type vars (pre-solve cache
    //     pollution)
    //   - delta-solve-time-us: lifetime total microseconds
    //     spent in ConstraintSystem::solve_delta()
    //   - multi-mutation-recompute-ratio-bp: derived from
    //     the 3 counters — cache_misses / total in basis
    //     points (0-10000). 0 = no recomputation, 10000 =
    //     all recomputation. The AC1 metric from #258.
    // Returns a hash with all 5 counts (all 0 if no typecheck
    // has happened yet, or if no service is bound).
    ObservabilityPrims::register_stats_impl(
        "compile:multi-mutation-stats", [&ev](const auto&) -> EvalValue {
            // Re-use the build_hash pattern from above primitives.
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                // Issue #258: capacity 32 (was 8 in earlier primitives)
                // because this primitive returns 5 keys (cache-hits,
                // cache-misses, stale-cache, delta-solve-time-us, ratio-bp).
                // Capacity 8 worked for the 4-key primitives (#252/#253/#254/#255/#256)
                // but 5 keys + the FNV-1a collision pattern occasionally
                // failed to insert one key (val=11 = void returned by
                // hash-ref for missing keys). 32 leaves plenty of headroom
                // and avoids the rare 5-key + cap-8 collision.
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
                    // Issue #258: mask fp to avoid 0xFF collision
                    // with HASH_EMPTY sentinel. Without the mask,
                    // a key whose FNV-1a top byte is 0x7F would
                    // produce fp=0xFF (=HASH_EMPTY), and hash-ref
                    // would skip the slot thinking it's empty.
                    // (h >> 57) gives a 7-bit value [0x00..0x7F];
                    // (h >> 57) & 0x7F keeps the 7 bits; | 0x80
                    // sets the high bit so fp is in [0x80..0xFE],
                    // never 0xFF.
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
            // Issue #1797: consistent type-cache triple via snapshot
            // (same contract as compile:type-cache-stats).
            TypeCacheStatsSnapshot snap;
            std::uint64_t solve_us = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                snap = m->snapshot_type_cache_stats();
                solve_us = m->delta_solve_time_us.load(std::memory_order_relaxed);
            }
            std::uint64_t total = snap.hits + snap.misses + snap.stale;
            std::uint64_t ratio_bp = (total > 0) ? (snap.misses * 10000u / total) : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"cache-hits-total", make_int(static_cast<std::int64_t>(snap.hits))},
                {"cache-misses-total", make_int(static_cast<std::int64_t>(snap.misses))},
                {"stale-cache-total", make_int(static_cast<std::int64_t>(snap.stale))},
                {"delta-solve-time-us", make_int(static_cast<std::int64_t>(solve_us))},
                {"multi-mutation-recompute-ratio-bp",
                 make_int(static_cast<std::int64_t>(ratio_bp))},
            };
            return build_hash(kv);
        });
}

// Issue #909 compile part 7 (orig 629-701)
void CompilePrims::register_compile_p7(PrimRegistrar add, Evaluator& ev) {

    // (compile:type-propagation-stats) — Issue #259: observability
    // for the IR type metadata propagation path. Hash with 3
    // fields:
    //   - ir-instructions-total: lifetime total IR instructions
    //     executed by the IR interpreter
    //   - ir-instructions-with-type-total: lifetime total where
    //     the lowering pass populated type_id (the propagation
    //     landed)
    //   - type-propagation-coverage-bp: derived ratio (with_type
    //     * ::aura::compiler::kBasisPointScale / total) in basis points (0-10000). 0 = no
    //     propagation, 10000 = all instructions carry type info.
    //     The AC from #259 is "increase coverage" — today most
    //     lowering sites don't call emit_with_type(), so coverage
    //     is low. This primitive lets users measure the baseline
    //     + see the impact of follow-up wiring.
    ObservabilityPrims::register_stats_impl(
        "compile:type-propagation-stats", [&ev](const auto&) -> EvalValue {
            // Re-use the build_hash pattern from above primitives.
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
                    // Issue #258: defensive bump if fp lands on
                    // HASH_EMPTY sentinel (FNV-1a top bits collision).
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
            std::uint64_t total = 0, with_type = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                total = m->ir_instructions_total.load(std::memory_order_relaxed);
                with_type = m->ir_instructions_with_type_total.load(std::memory_order_relaxed);
            }
            std::uint64_t coverage_bp = (total > 0) ? (with_type * 10000u / total) : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"ir-instructions-total", make_int(static_cast<std::int64_t>(total))},
                {"ir-instructions-with-type-total", make_int(static_cast<std::int64_t>(with_type))},
                {"type-propagation-coverage-bp", make_int(static_cast<std::int64_t>(coverage_bp))},
            };
            return build_hash(kv);
        });
}
// Issue #909 compile part 8 (orig 702-770)
void CompilePrims::register_compile_p8(PrimRegistrar add, Evaluator& ev) {

    // (compile:occurrence-typing-stats)
    //   — Issue #386: deep Occurrence Typing
    //   narrowing observability. Returns a hash with
    //   4 fields: applied-total / skipped-total /
    //   reanalyzed-total / applied-ratio-bp. The
    //   full #386 scope is wiring narrowing into the
    //   let/if paths + strengthening
    //   consistent_unify for refined types +
    //   leveraging per-node occurrence-dirty for
    //   targeted re-analysis. This scope-limited
    //   slice ships the observability foundation.
    ObservabilityPrims::register_stats_impl(
        "compile:occurrence-typing-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto cap = std::max<std::size_t>(8, kv.size() * 2);
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
            // Issue #1898: pin + revalidate (vs raw compiler_service_ TOCTOU).
            return with_compiler_service_pin(ev, [&](CompilerService& svc) -> EvalValue {
                // Issue #1856: try_snapshot — no throw; void on fail (not false-clean zeros).
                auto snap_opt = svc.try_snapshot();
                if (!snap_opt)
                    return make_void();
                const auto& snap = *snap_opt;
                std::vector<std::pair<std::string, EvalValue>> kv = {
                    {"applied-total",
                     make_int(static_cast<std::int64_t>(snap.narrowing_applied_total))},
                    {"skipped-total",
                     make_int(static_cast<std::int64_t>(snap.narrowing_skipped_total))},
                    {"reanalyzed-total",
                     make_int(static_cast<std::int64_t>(snap.narrowing_reanalyzed_total))},
                    {"applied-ratio-bp",
                     make_int(static_cast<std::int64_t>(snap.narrowing_applied_ratio_bp))},
                };
                return build_hash(kv);
            });
        });
}

// Issue #909 compile part 9 (orig 771-834)
void CompilePrims::register_compile_p9(PrimRegistrar add, Evaluator& ev) {

    // (compile:and-or-precision-stats)
    //   — Issue #338: and/or precision observability.
    //   Returns a hash with 2 fields: meet-uses-total
    //   + join-uses-total (lifetime totals of when
    //   the new TypeRegistry::meet / join helpers
    //   fired in the (and ...) / (or ...) branches
    //   of analyze_predicate_flat). The full #338
    //   scope is also real intersection / union
    //   types in the registry; this scope-limited
    //   slice ships the observability foundation.
    ObservabilityPrims::register_stats_impl(
        "compile:and-or-precision-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto cap = std::max<std::size_t>(8, kv.size() * 2);
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
            // Issue #1856: try_snapshot — no throw; void on fail (not false-clean zeros).
            auto snap_opt = svc->try_snapshot();
            if (!snap_opt)
                return make_void();
            const auto& snap = *snap_opt;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"meet-uses-total",
                 make_int(static_cast<std::int64_t>(snap.and_or_meet_uses_total))},
                {"join-uses-total",
                 make_int(static_cast<std::int64_t>(snap.and_or_join_uses_total))},
            };
            return build_hash(kv);
        });
}

// Issue #909 compile part 10 (orig 835-901)
void CompilePrims::register_compile_p10(PrimRegistrar add, Evaluator& ev) {

    // (compile:occurrence-dirty-stats)
    //   — Issue #434: per-node occurrence dirty
    //   tracking. Returns a hash with 1 field:
    //   dirty-recovery-total (lifetime total of
    //   narrowing re-analyses triggered by a
    //   dirty If node). Distinct from the
    //   narrowing_reanalyzed signal in
    //   occurrence-typing-stats (which counts
    //   all predicate memo misses, not just
    //   the ones triggered by dirty If nodes).
    //   This is the narrower signal that
    //   measures the post-mutation re-analysis
    //   workload specifically.
    ObservabilityPrims::register_stats_impl(
        "compile:occurrence-dirty-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto cap = std::max<std::size_t>(8, kv.size() * 2);
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
            // Issue #1856: try_snapshot — no throw; void on fail (not false-clean zeros).
            auto snap_opt = svc->try_snapshot();
            if (!snap_opt)
                return make_void();
            const auto& snap = *snap_opt;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"dirty-recovery-total",
                 make_int(static_cast<std::int64_t>(snap.narrowing_dirty_recovery_total))},
            };
            return build_hash(kv);
        });
}

// Issue #909 compile part 11 (orig 902-974)
void CompilePrims::register_compile_p11(PrimRegistrar add, Evaluator& ev) {

    // (compile:schema-cache-stats)
    //   — Issue #390: per-node schema cache
    //   observability. Returns a hash with 3
    //   fields: lookups-total (lifetime total
    //   of schema_cache column lookups in the
    //   type-checker cache hit path) /
    //   hits-total (lookups that returned a
    //   non-zero schema that matched the
    //   cached type_id) / hit-rate-bp (basis
    //   points: hits / lookups * ::aura::compiler::kBasisPointScale).
    //   Companion to the (query:schema-of-marker)
    //   diagnostic primitive from #248. The full
    //   #390 scope is also auto-populating the
    //   cache in clone_macro_body + type checker
    //   integration + typed_mutate schema-violation
    //   guard; this slice ships the observability
    //   foundation + the basic cache check in
    //   synthesize_flat.
    ObservabilityPrims::register_stats_impl(
        "compile:schema-cache-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto cap = std::max<std::size_t>(8, kv.size() * 2);
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
            // Issue #1856: try_snapshot — no throw; void on fail (not false-clean zeros).
            auto snap_opt = svc->try_snapshot();
            if (!snap_opt)
                return make_void();
            const auto& snap = *snap_opt;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"lookups-total",
                 make_int(static_cast<std::int64_t>(snap.schema_cache_lookups_total))},
                {"hits-total", make_int(static_cast<std::int64_t>(snap.schema_cache_hits_total))},
                {"hit-rate-bp", make_int(static_cast<std::int64_t>(snap.schema_cache_hit_rate_bp))},
            };
            return build_hash(kv);
        });
}

// Issue #909 compile part 12 (orig 975-1048)
void CompilePrims::register_compile_p12(PrimRegistrar add, Evaluator& ev) {

    // (compile:constraint-dep-stats)
    //   — Issue #409: fine-grained constraint
    //   dependency tracking observability. Returns
    //   a hash with 3 fields: processed-total
    //   (lifetime total of constraints re-solved
    //   via solve_delta) / total (lifetime total
    //   of constraints added via add_delta) /
    //   ratio-bp (basis points: processed /
    //   total * ::aura::compiler::kBasisPointScale). The ratio measures how
    //   much the reverse map prunes — a low
    //   ratio means the filter is doing useful
    //   work. Pre-#409 the ratio was always 1.0
    //   (all dirty constraints re-solved). The
    //   full #409 scope also extends the reverse
    //   map to cover more constraint kinds +
    //   var-rep updates across unify; this slice
    //   ships the observability foundation.
    ObservabilityPrims::register_stats_impl(
        "compile:constraint-dep-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto cap = std::max<std::size_t>(8, kv.size() * 2);
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
            // Issue #1856: try_snapshot — no throw; void on fail (not false-clean zeros).
            auto snap_opt = svc->try_snapshot();
            if (!snap_opt)
                return make_void();
            const auto& snap = *snap_opt;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"processed-total",
                 make_int(static_cast<std::int64_t>(snap.delta_constraints_processed_total))},
                {"total", make_int(static_cast<std::int64_t>(snap.delta_constraints_total))},
                {"ratio-bp",
                 make_int(static_cast<std::int64_t>(snap.delta_solve_constraints_ratio_bp))},
            };
            return build_hash(kv);
        });
}

// Issue #909 compile part 13 (orig 1049-1127)
void CompilePrims::register_compile_p13(PrimRegistrar add, Evaluator& ev) {

    // (compile:constraint-solver-stats)
    //   — Issue #383: ConstraintSystem worklist +
    //   consistent_unify observability. Returns
    //   a hash with 3 fields: unify-total
    //   (lifetime total of consistent_unify
    //   calls — success or failure) /
    //   subtype-total (lifetime total of
    //   consistent_subtype calls) /
    //   restart-total (lifetime total of
    //   worklist restarts — bumps when a
    //   pass adds new constraints that
    //   require an additional pass). The
    //   full #383 scope is also a
    //   comprehensive 20+ test matrix for
    //   gradual + poly + occurrence unify
    //   + priority/dependency ordering +
    //   debug hooks for the constraint
    //   graph; this slice ships the
    //   observability foundation + the
    //   worklist restart detection.
    ObservabilityPrims::register_stats_impl(
        "compile:constraint-solver-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto cap = std::max<std::size_t>(8, kv.size() * 2);
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
            // Issue #1856: try_snapshot — no throw; void on fail (not false-clean zeros).
            auto snap_opt = svc->try_snapshot();
            if (!snap_opt)
                return make_void();
            const auto& snap = *snap_opt;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"unify-total", make_int(static_cast<std::int64_t>(snap.consistent_unify_total))},
                {"subtype-total",
                 make_int(static_cast<std::int64_t>(snap.consistent_subtype_total))},
                {"restart-total", make_int(static_cast<std::int64_t>(snap.worklist_restart_total))},
                {"reverify-total",
                 make_int(static_cast<std::int64_t>(snap.delta_conflict_reverify_total))},
                {"conflict-detected",
                 make_int(static_cast<std::int64_t>(snap.delta_conflict_detected_total))},
            };
            return build_hash(kv);
        });
}

// Issue #909 compile part 14 (orig 1128-1214)
void CompilePrims::register_compile_p14(PrimRegistrar add, Evaluator& ev) {

    // Issue #466: (query:constraint-stats) — alias summarizing
    // solve_delta conflict re-verify observability.
    ObservabilityPrims::register_stats_impl(
        "query:constraint-stats", [&ev](const auto&) -> EvalValue {
            if (!ev.compiler_service_)
                return make_int(0);
            auto* svc = static_cast<class CompilerService*>(ev.compiler_service_);
            // Issue #1856: try_snapshot — no throw; void on fail (not false-clean zeros).
            auto snap_opt = svc->try_snapshot();
            if (!snap_opt)
                return make_void();
            const auto& snap = *snap_opt;
            return make_int(static_cast<std::int64_t>(snap.delta_conflict_reverify_total +
                                                      snap.delta_conflict_detected_total));
        });

    // (compile:let-poly-stats)
    //   — Issue #385: Let-Poly caching
    //   observability. Returns a hash with 4
    //   fields: register-total (lifetime total
    //   of register_forall calls) /
    //   dedup-hits-total (lifetime total of
    //   dedup cache hits — the pre-#385 dedup
    //   loop returned an existing TypeId for
    //   same-var + same-body calls) /
    //   instantiate-total (lifetime total of
    //   instantiate_forall calls) /
    //   dedup-ratio-bp (basis points: dedup /
    //   register * ::aura::compiler::kBasisPointScale — measures cache
    //   effectiveness). The full #385 scope
    //   also includes per-binding mutation
    //   version stamping + poly constraints
    //   integrated with ConstraintSystem dirty
    //   tracking + Value Restriction
    //   re-evaluation; this slice ships the
    //   observability foundation.
    ObservabilityPrims::register_stats_impl(
        "compile:let-poly-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto cap = std::max<std::size_t>(8, kv.size() * 2);
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
            // Issue #1856: try_snapshot — no throw; void on fail (not false-clean zeros).
            auto snap_opt = svc->try_snapshot();
            if (!snap_opt)
                return make_void();
            const auto& snap = *snap_opt;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"register-total", make_int(static_cast<std::int64_t>(snap.poly_register_total))},
                {"dedup-hits-total",
                 make_int(static_cast<std::int64_t>(snap.poly_dedup_hits_total))},
                {"instantiate-total",
                 make_int(static_cast<std::int64_t>(snap.poly_instantiate_total))},
                {"dedup-ratio-bp", make_int(static_cast<std::int64_t>(snap.poly_dedup_ratio_bp))},
            };
            return build_hash(kv);
        });
}

// Issue #909 compile part 15 (orig 1215-1290)
void CompilePrims::register_compile_p15(PrimRegistrar add, Evaluator& ev) {

    // (compile:dirty-impact-stats)
    //   — Issue #487: dirty propagation + IR
    //   re-lower observability. Returns a hash
    //   with 3 fields: should-relower-total
    //   (lifetime total of times should_relower
    //   returned true on dirty — the re-lower
    //   path fired) / affected-subtree-total
    //   (lifetime total of times
    //   affected_subtree_from_mutation was
    //   called — the dirty propagation entry
    //   point) / trigger-rate-bp (basis
    //   points: should_relower / affected_subtree
    //   * ::aura::compiler::kBasisPointScale — measures the dirty-trigger
    //   rate). The full #487 scope also includes
    //   wiring should_relower_on_dirty to the
    //   pass pipeline + a query:dirty-impact
    //   primitive for fine-grained impact; this
    //   slice ships the observability foundation
    //   + the 2 lifetime counters.
    ObservabilityPrims::register_stats_impl(
        "compile:dirty-impact-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto cap = std::max<std::size_t>(8, kv.size() * 2);
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
            // Issue #1856: try_snapshot — no throw; void on fail (not false-clean zeros).
            auto snap_opt = svc->try_snapshot();
            if (!snap_opt)
                return make_void();
            const auto& snap = *snap_opt;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"should-relower-total",
                 make_int(static_cast<std::int64_t>(snap.should_relower_total))},
                {"affected-subtree-total",
                 make_int(static_cast<std::int64_t>(snap.affected_subtree_total))},
                {"trigger-rate-bp",
                 make_int(static_cast<std::int64_t>(snap.dirty_trigger_rate_bp))},
            };
            return build_hash(kv);
        });
}
// Issue #909 compile part 16 (orig 1291-1369)
void CompilePrims::register_compile_p16(PrimRegistrar add, Evaluator& ev) {

    // (compile:type-dep-graph-stats)
    //   — Issue #387: Type Dependency Graph
    //   observability. Returns a hash with 4
    //   fields: lookups-total (lifetime total
    //   of affected_nodes_for_type calls) /
    //   hits-total (lookups that found >= 1
    //   dependent node — a "real" hit) /
    //   size (current number of distinct
    //   TypeIds tracked; not lifetime-total,
    //   it's a snapshot peak) /
    //   hit-rate-bp (basis points: hits /
    //   lookups * ::aura::compiler::kBasisPointScale). The full #387 scope
    //   wires the engine's set_type sites to
    //   record (TypeId, NodeId) edges so the
    //   graph actually populates during
    //   inference; this slice ships the data
    //   structure on TypeChecker + the
    //   observability surface. Users can
    //   pre-populate the graph via
    //   TypeChecker::record_type_dependency
    //   (e.g. for benchmark setup) and query
    //   it via affected_nodes_for_type.
    ObservabilityPrims::register_stats_impl(
        "compile:type-dep-graph-stats", [&ev](const auto&) -> EvalValue {
            if (!ev.compiler_service_)
                return make_int(0);
            auto* svc = static_cast<class CompilerService*>(ev.compiler_service_);
            // Issue #1856: try_snapshot — no throw; void on fail (not false-clean zeros).
            auto snap_opt = svc->try_snapshot();
            if (!snap_opt)
                return make_void();
            const auto& snap = *snap_opt;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"lookups-total", make_int(static_cast<std::int64_t>(snap.type_dep_graph_lookups))},
                {"hits-total", make_int(static_cast<std::int64_t>(snap.type_dep_graph_hits))},
                {"size", make_int(static_cast<std::int64_t>(snap.type_dep_graph_size))},
                {"hit-rate-bp",
                 make_int(static_cast<std::int64_t>(snap.type_dep_graph_hit_rate_bp))},
            };
            // Use the same hash-table builder pattern as
            // compile:dirty-impact-stats (create +
            // insert + return). For scope-limited
            // consistency, build it inline here.
            auto cap = std::max<std::size_t>(8, kv.size() * 2);
            std::size_t hcap = 8;
            while (hcap < cap)
                hcap *= 2;
            auto* ht = FlatHashTable::create(hcap);
            if (!ht)
                return make_int(0);
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
                    return make_int(0);
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 compile part 17 (orig 1370-1442)
void CompilePrims::register_compile_p17(PrimRegistrar add, Evaluator& ev) {

    // (compile:match-narrowing-stats)
    //   — Issue #341: match + Occurrence Typing
    //   integration observability. Returns a hash
    //   with 3 fields: narrowed-total (lifetime
    //   total of __match_tmp lets whose subject
    //   type was refined by a prior narrowing in
    //   the env) / total (lifetime total of
    //   __match_tmp lets processed by the type
    //   checker) / ratio-bp (basis points:
    //   narrowed / total * ::aura::compiler::kBasisPointScale). The full
    //   #341 scope is also extending
    //   analyze_predicate_flat to recognize more
    //   ADT-related predicates and feeding the
    //   refined type into match exhaustiveness
    //   checking. This slice ships the
    //   observability foundation + the basic
    //   env-lookup path for the subject type.
    ObservabilityPrims::register_stats_impl(
        "compile:match-narrowing-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto cap = std::max<std::size_t>(8, kv.size() * 2);
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
            // Issue #1856: try_snapshot — no throw; void on fail (not false-clean zeros).
            auto snap_opt = svc->try_snapshot();
            if (!snap_opt)
                return make_void();
            const auto& snap = *snap_opt;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"narrowed-total",
                 make_int(static_cast<std::int64_t>(snap.match_subject_narrowed_total))},
                {"total", make_int(static_cast<std::int64_t>(snap.match_subject_total))},
                {"ratio-bp", make_int(static_cast<std::int64_t>(snap.match_narrowed_ratio_bp))},
            };
            return build_hash(kv);
        });
}

// Issue #909 compile part 18 (orig 1443-1507)
void CompilePrims::register_compile_p18(PrimRegistrar add, Evaluator& ev) {

    // (compile:narrowing-blame-stats)
    //   — Issue #342: narrowing blame/provenance
    //   observability. Returns a hash with 1 field:
    //   provenance-total (lifetime total of
    //   OccurrenceInfoFlat records that have
    //   predicate_name + source_cond_id populated).
    //   Pre-#342 this was always 0 (the fields
    //   didn't exist). Post-#342 every
    //   analyze_predicate_flat that returns a
    //   populated OccurrenceInfoFlat bumps this
    //   counter.
    ObservabilityPrims::register_stats_impl(
        "compile:narrowing-blame-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto cap = std::max<std::size_t>(8, kv.size() * 2);
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
            // Issue #1856: try_snapshot — no throw; void on fail (not false-clean zeros).
            auto snap_opt = svc->try_snapshot();
            if (!snap_opt)
                return make_void();
            const auto& snap = *snap_opt;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"provenance-total",
                 make_int(static_cast<std::int64_t>(snap.narrowing_provenance_total))},
            };
            return build_hash(kv);
        });
}

// Issue #909 compile part 19 (orig 1508-1615)
void CompilePrims::register_compile_p19(PrimRegistrar add, Evaluator& ev) {

    // (ast:generation-stats)
    //   — Issue #343: long-term stability
    //   observability. Returns a hash with 5
    //   fields: current-generation (live value
    //   of FlatAST::generation_, uint16_t) /
    //   bump-generation-total (lifetime total
    //   of generation bumps) /
    //   generation-wrap-total (lifetime total
    //   of uint16_t wrap-arounds) /
    //   stable-ref-invalidations-total
    //   (lifetime total of StableNodeRef
    //   rejections) /
    //   node-gen-stale-access-total (lifetime
    //   total of stale NodeId accesses).
    //   Companion to (query:stable-ref-stats)
    //   which returns the SUM of the 3 lifetime
    //   counters; post-#343 the AI Agent can
    //   react to each category independently
    //   (e.g. checkpoint when wrap-count > 0,
    //   investigate when stale-access-count
    //   grows faster than bump-count).
    ObservabilityPrims::register_stats_impl(
        "ast:generation-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto cap = std::max<std::size_t>(8, kv.size() * 2);
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
            // Issue #1856: try_snapshot — no throw; void on fail (not false-clean zeros).
            auto snap_opt = svc->try_snapshot();
            if (!snap_opt)
                return make_void();
            const auto& snap = *snap_opt;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"current-generation",
                 make_int(static_cast<std::int64_t>(snap.current_generation))},
                {"bump-generation-total",
                 make_int(static_cast<std::int64_t>(snap.bump_generation_count))},
                {"generation-wrap-total",
                 make_int(static_cast<std::int64_t>(snap.generation_wrap_count))},
                {"stable-ref-invalidations-total",
                 make_int(static_cast<std::int64_t>(snap.stable_ref_invalidations))},
                {"node-gen-stale-access-total",
                 make_int(static_cast<std::int64_t>(snap.node_gen_stale_access_count))},
                // Issue #368: current wrap_epoch_ (uint32_t).
                // AI agents can checkpoint / compact before the
                // next generation_ wrap creates a wave of stale
                // refs in long-running workspaces.
                {"current-wrap-epoch",
                 make_int(static_cast<std::int64_t>(snap.current_wrap_epoch))},
                // Issue #369: per-category counters for the
                // structural-rollback dispatcher. 'structural-
                // rollback-success' is the number of mutations
                // that were rolled back successfully (parent +
                // child_idx + old/new data was available);
                // 'structural-rollback-besteffort' is the number
                // of mutations whose op_name aliases to a known
                // structural op but lacked the field_offset /
                // old/new_value data (i.e. the wrapper primitive
                // hasn't been migrated to add_structural_mutation_log_entry
                // yet). AI agents can use this to find structural
                // ops that are still at risk of partial rollback.
                {"structural-rollback-success",
                 make_int(static_cast<std::int64_t>(snap.structural_rollback_success))},
                {"structural-rollback-besteffort",
                 make_int(static_cast<std::int64_t>(snap.structural_rollback_besteffort))},
                // Issue #370: lifetime-safe view count.
                {"children-safe-view-count",
                 make_int(static_cast<std::int64_t>(snap.children_safe_view_count))},
                {"parent-safe-view-count",
                 make_int(static_cast<std::int64_t>(snap.parent_safe_view_count))},
                // Issue #1282: auto-restamp after generation wrap recovery count.
                {"auto-restamp-on-wrap",
                 make_int(static_cast<std::int64_t>(snap.auto_restamp_on_wrap_count))},
                // Issue #1281: children_ PCV topology restore count.
                {"children-topology-restore",
                 make_int(static_cast<std::int64_t>(snap.children_topology_restore_count))},
                // Issue #1502: parent_ topology restore / rebuild-from-children.
                {"parent-topology-restore",
                 make_int(static_cast<std::int64_t>(snap.parent_topology_restore_count))},
            };
            return build_hash(kv);
        });
}

// Issue #909 compile part 20 (orig 1616-1714)
void CompilePrims::register_compile_p20(PrimRegistrar add, Evaluator& ev) {

    // (compile:snapshot)
    //   → hash
    //   Issue #389 (follow-up #247): wraps CompilerService::snapshot()
    //   and returns the workspace's CURRENT observability state as a
    //   hash. Focuses on the SyntaxMarker distribution (the #247/#389
    //   scope) plus the long-term-stability context fields that an
    //   AI agent typically needs alongside marker counts. For deeper
    //   diagnostics on individual categories (narrowing, typecheck
    //   cache, dead-coercion, etc.), use the per-category
    //   `compile:*-stats` primitives.
    //
    //   Fields:
    //     marker-user-count           nodes written by user
    //     marker-macro-introduced-count nodes inserted by hygienic macros
    //     marker-bool-literal-count   auto-generated #t / #f nodes
    //     marker-total-count          total nodes in marker column
    //     current-generation          FlatAST generation_ (uint16_t, 1..65535)
    //     current-wrap-epoch          wrap_epoch_ bumped per generation_ wrap
    //     generation-wrap-count       lifetime uint16_t wrap-arounds
    //     node-count                  total AST node count in workspace
    //
    //   The marker_* fields are the same numbers you'd get from
    //   (query:marker-stats) but as a hash with named keys instead
    //   of a positional 4-element list. Use this when you want to
    //   pipe individual marker counts into (stats:get ...) /
    //   (stats:contains?) without list-position gymnastics.
    add("compile:snapshot", [&ev](const auto&) -> EvalValue {
        // Local build_hash closure — same FNV-1a + open-addressing
        // pattern as the other compile:*-stats primitives above.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            // Issue #422: 9 keys after hygiene-violation-attempts.
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
        if (!ev.compiler_service_)
            return make_void();
        auto* svc = static_cast<class CompilerService*>(ev.compiler_service_);
        // Issue #1856: try_snapshot — no throw; void on fail (not false-clean zeros).
        auto snap_opt = svc->try_snapshot();
        if (!snap_opt)
            return make_void();
        const auto& snap = *snap_opt;
        // node-count is the workspace's total node count, not a
        // snapshot field — read it directly from the FlatAST when
        // available so the primitive is still useful even if the
        // snapshot was taken before the workspace was set.
        std::uint64_t node_count = 0;
        if (ev.workspace_flat_)
            node_count = ev.workspace_flat_->size();
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"marker-user-count", make_int(static_cast<std::int64_t>(snap.marker_user_count))},
            {"marker-macro-introduced-count",
             make_int(static_cast<std::int64_t>(snap.marker_macro_introduced_count))},
            {"marker-bool-literal-count",
             make_int(static_cast<std::int64_t>(snap.marker_bool_literal_count))},
            {"marker-total-count", make_int(static_cast<std::int64_t>(snap.marker_total_count))},
            {"current-generation", make_int(static_cast<std::int64_t>(snap.current_generation))},
            {"current-wrap-epoch", make_int(static_cast<std::int64_t>(snap.current_wrap_epoch))},
            {"generation-wrap-count",
             make_int(static_cast<std::int64_t>(snap.generation_wrap_count))},
            {"node-count", make_int(static_cast<std::int64_t>(node_count))},
            // Issue #422: live evaluator hygiene violation attempts.
            {"hygiene-violation-attempts",
             make_int(static_cast<std::int64_t>(ev.get_hygiene_violation_attempts()))},
        };
        return build_hash(kv);
    });
}

// Issue #909 compile part 21 (orig 1715-1765)
void CompilePrims::register_compile_p21(PrimRegistrar add, Evaluator& ev) {

    // (compile:status)
    //   → ((:key value) ...)  association list
    //   Returns incremental compilation status:
    //     :dirty-nodes   — nodes marked as dirty (need recompilation)
    //     :clean-nodes   — nodes that are up-to-date
    //     :generation    — FlatAST generation counter
    //     :mutation-count— total mutations applied
    ObservabilityPrims::register_stats_impl("compile:status", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_flat_)
            return make_void();

        auto& flat = *ev.workspace_flat_;
        auto total = flat.size();
        std::uint64_t dirty = 0;
        std::uint64_t clean = 0;

        for (aura::ast::NodeId id = 0; id < total; ++id) {
            if (flat.is_dirty(id))
                dirty++;
            else
                clean++;
        }

        // Build alist
        auto add_entry = [&](const std::string& key, EvalValue val) -> std::uint64_t {
            auto key_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(key);
            auto entry_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_string(key_idx), val});
            return entry_pair;
        };

        EvalValue result = make_void();
        auto cvt = [&](std::uint64_t n) -> EvalValue {
            auto idx = ev.string_heap_.size();
            ev.string_heap_.push_back(std::to_string(n));
            return make_string(idx);
        };
        std::uint64_t entry_ids[4];
        entry_ids[0] = add_entry(":generation", cvt(flat.generation()));
        entry_ids[1] = add_entry(":mutation-count", cvt(flat.mutation_count()));
        entry_ids[2] = add_entry(":dirty-nodes", cvt(dirty));
        entry_ids[3] = add_entry(":clean-nodes", cvt(clean));
        for (int ei = 0; ei < 4; ++ei) {
            auto cons_pair = ev.pairs_.size();
            ev.pairs_.push_back({make_pair(entry_ids[ei]), result});
            result = make_pair(cons_pair);
        }
        return result;
    });
}

// Issue #909 compile part 22 (orig 1766-1817)
void CompilePrims::register_compile_p22(PrimRegistrar add, Evaluator& ev) {

    // (compile:cache-size) — Issue #196: number of defines
    // currently in the ir_cache_v2_ map. Each entry corresponds
    // to a top-level define that has been compiled at least
    // once. Returns 0 if no hook is installed.
    ObservabilityPrims::register_stats_impl("compile:cache-size", [&ev](const auto&) -> EvalValue {
        if (!ev.get_incremental_stats_fn_)
            return make_int(0);
        auto packed = ev.get_incremental_stats_fn_();
        return make_int(static_cast<std::int64_t>(packed >> 48));
    });

    // (compile:dirty-count) — Issue #196: number of currently-
    // dirty entries in the ir_cache_v2_ map. A dirty entry
    // means the cached IR is stale and needs re-lower on next
    // access. Returns 0 if no hook is installed.
    ObservabilityPrims::register_stats_impl("compile:dirty-count", [&ev](const auto&) -> EvalValue {
        if (!ev.get_incremental_stats_fn_)
            return make_int(0);
        auto packed = ev.get_incremental_stats_fn_();
        return make_int(static_cast<std::int64_t>((packed >> 32) & 0xFFFF));
    });

    // (compile:mark-dirty-upward-fast node-id [reasons]) —
    // Issue #336: optimized variant of mark_dirty_upward
    // that early-exits when the parent already has the
    // target reason bits. Same signature as the
    // lower-level mark_dirty_upward, but with the
    // early-exit optimization (fixed-point check
    // before walking further up the parent chain).
    //
    // reasons is a bitmask. When omitted, defaults to
    // kGeneralDirty (same as mark_dirty_upward). The
    // helper is primarily useful in AI self-modification
    // loops that do many small mutations in deep ASTs;
    // the (compile:ast-ops-stats) fast-fixed-point-hits
    // counter surfaces how often the early-exit fires.
    add("compile:mark-dirty-upward-fast", [&ev](const auto& a) -> EvalValue {
        // Issue #1395: capability gate — require kCapWildcard.
        // "fast path" name suggests audit-skip; gate behind
        // highest privilege to prevent silent audit-trail bypass.
        if (ev.sandbox_mode() && !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->capability_compile_denials.fetch_add(1, std::memory_order_relaxed);
                m->cap_denial_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "capability denied: kCapWildcard required",
                                        ev.primitive_error_counter_ptr());
        }
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        auto node_id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (node_id >= ws->size())
            return make_bool(false);
        std::uint8_t reasons = aura::ast::FlatAST::kGeneralDirty;
        if (a.size() >= 2 && is_int(a[1]))
            reasons = static_cast<std::uint8_t>(as_int(a[1]));
        // Issue #1897: structural dirty walk under try_acquire Guard.
        return run_under_mutation_guard(ev, [&]() -> EvalValue {
            ws->mark_dirty_upward_fast(node_id, reasons);
            return make_void();
        });
    });
}

// Issue #909 compile part 23 (orig 1818-1888)
void CompilePrims::register_compile_p23(PrimRegistrar add, Evaluator& ev) {

    // (compile:epoch) — Issue #196: current mutation_epoch_ value.
    // The epoch is bumped atomically on every mutation. Cache
    // entries that haven't seen the current epoch are stale.
    // Returns 0 if no hook is installed.
    ObservabilityPrims::register_stats_impl("compile:epoch", [&ev](const auto&) -> EvalValue {
        if (!ev.get_incremental_stats_fn_)
            return make_int(0);
        auto packed = ev.get_incremental_stats_fn_();
        return make_int(static_cast<std::int64_t>((packed >> 16) & 0xFFFF));
    });

    // (compile:dirty-reason-counts) — Issue #344: returns
    // the 8-tuple of per-DirtyReason counts. Cheap O(n)
    // walk of the dirty_ column. The 8 reasons are
    // (in DirtyReason enum order):
    //   0: kGeneralDirty    (0x01)
    //   1: kConstraintDirty  (0x02)
    //   2: kOccurrenceDirty  (0x04)
    //   3: kOwnershipDirty   (0x08)
    //   4: kCoercionDirty    (0x10)
    //   5: kStructDirty      (0x20)
    //   6: kDefUseDirty      (0x40)
    //   7: kPpaHintDirty     (0x80)
    // Returns 0s when no workspace is loaded.
    ObservabilityPrims::register_stats_impl(
        "compile:dirty-reason-counts", [&ev](const auto&) -> EvalValue {
            auto* ws = ev.workspace_flat();
            if (!ws) {
                // Return a 0/0/0/0/0/0/0/0 8-tuple
                // (pair-of-pair-of-pair-of-pair). Cheap.
                EvalValue out = make_void();
                for (int i = 0; i < 8; ++i) {
                    auto p_idx = ev.pairs_.size();
                    Pair tmp{make_int(0), out};
                    ev.pairs_.push_back(std::move(tmp));
                    out = make_pair(p_idx);
                }
                return out;
            }
            // Walk the dirty_view (cheap, cache-friendly)
            // and OR-accumulate the counts.
            std::array<std::uint64_t, 8> counts = {0, 0, 0, 0, 0, 0, 0, 0};
            const auto view = ws->dirty_view();
            for (auto byte : view) {
                if (byte & 0x01)
                    ++counts[0];
                if (byte & 0x02)
                    ++counts[1];
                if (byte & 0x04)
                    ++counts[2];
                if (byte & 0x08)
                    ++counts[3];
                if (byte & 0x10)
                    ++counts[4];
                if (byte & 0x20)
                    ++counts[5];
                if (byte & 0x40)
                    ++counts[6];
                if (byte & 0x80)
                    ++counts[7];
            }
            // Build the 8-tuple (nested pairs, right-folded).
            EvalValue out = make_void();
            for (int i = 7; i >= 0; --i) {
                auto p_idx = ev.pairs_.size();
                Pair tmp{make_int(static_cast<std::int64_t>(counts[i])), out};
                ev.pairs_.push_back(std::move(tmp));
                out = make_pair(p_idx);
            }
            return out;
        });
}
// Issue #909 compile part 24 (orig 1889-1947)
void CompilePrims::register_compile_p24(PrimRegistrar add, Evaluator& ev) {

    // (query:dirty-nodes :reason "X") — Issue #344:
    // returns a pair-list of NodeIds that have the
    // target DirtyReason bit set. X is one of:
    //   "general" | "constraint" | "occurrence" |
    //   "ownership" | "coercion" | "struct" |
    //   "defuse" | "ppa-hint"
    // The pair-list is in NodeId order (smallest
    // first) so the caller can iterate with
    // (car / cdr) or take (length ...) of the
    // sublist. Returns the empty list when no
    // workspace is loaded or the reason is
    // unknown.
    add("query:dirty-nodes", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        const auto& reason_name = ev.string_heap_[idx];
        std::uint8_t mask = 0;
        if (reason_name == "general")
            mask = 0x01;
        else if (reason_name == "constraint")
            mask = 0x02;
        else if (reason_name == "occurrence")
            mask = 0x04;
        else if (reason_name == "ownership")
            mask = 0x08;
        else if (reason_name == "coercion")
            mask = 0x10;
        else if (reason_name == "struct")
            mask = 0x20;
        else if (reason_name == "defuse")
            mask = 0x40;
        else if (reason_name == "ppa-hint")
            mask = 0x80;
        else
            return make_void();
        // Walk the dirty column; collect NodeIds
        // with the target bit set. Returns the
        // pair-list in NodeId order (smallest first).
        const auto view = ws->dirty_view();
        EvalValue list = make_void();
        for (std::size_t id = view.size(); id-- > 0;) {
            if (view[id] & mask) {
                auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back(std::to_string(id));
                auto p_idx = ev.pairs_.size();
                Pair tmp{make_string(sidx), list};
                ev.pairs_.push_back(std::move(tmp));
                list = make_pair(p_idx);
            }
        }
        return list;
    });
}

// Issue #909 compile part 25 (orig 1948-1999)
void CompilePrims::register_compile_p25(PrimRegistrar add, Evaluator& ev) {

    // (compile:dep-edges) — Issue #196: total number of edges
    // in the dep_graph_ map. Each edge means "this define
    // depends on that define"; mutations to the target cascade
    // to invalidate the source via invalidate_function BFS.
    // Returns 0 if no hook is installed.
    ObservabilityPrims::register_stats_impl("compile:dep-edges", [&ev](const auto&) -> EvalValue {
        if (!ev.get_incremental_stats_fn_)
            return make_int(0);
        auto packed = ev.get_incremental_stats_fn_();
        return make_int(static_cast<std::int64_t>(packed & 0xFFFF));
    });

    // (compile:block-dirty-count name) — Issue #196: total
    // number of dirty blocks across all functions in the
    // named define's IR cache entry. Returns 0 if no hook
    // is installed or the entry doesn't exist. Use case:
    // an EDSL agent can measure "did the previous mutation
    // actually re-lower anything?" by reading this primitive
    // before and after a mutation cycle.
    // Multi-arg query API (not a zero-arg stats hash) — must
    // stay on public add(); stats:get cannot pass func/name args.
    add("compile:block-dirty-count", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(0);
        if (!ev.get_dirty_block_count_fn_)
            return make_int(0);
        return make_int(
            static_cast<std::int64_t>(ev.get_dirty_block_count_fn_(ev.string_heap_[idx].c_str())));
    });

    // (compile:func-block-dirty-count name func-idx) —
    // Issue #196: dirty block count for a specific function
    // in the named define's IR cache entry. Returns 0 if
    // no hook, the entry doesn't exist, or func-idx is
    // out of range.
    add("compile:func-block-dirty-count", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1])) {
            return make_int(0);
        }
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(0);
        auto fidx = as_int(a[1]);
        if (fidx < 0)
            return make_int(0);
        if (!ev.get_func_dirty_block_count_fn_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev.get_func_dirty_block_count_fn_(
            ev.string_heap_[idx].c_str(), static_cast<std::size_t>(fidx))));
    });
}

// Issue #909 compile part 26 (orig 2000-2072)
void CompilePrims::register_compile_p26(PrimRegistrar add, Evaluator& ev) {

    // (compile:block-dirty? name func-idx block-idx) —
    // Issue #196: returns #t if the specific (function,
    // block) is dirty in the named define's IR cache entry.
    // Returns #f otherwise. Use case: fine-grained
    // "did THIS block change?" query for the smarter
    // re-lower (Phase 5 follow-up).
    add("compile:block-dirty?", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 3 || !is_string(a[0]) || !is_int(a[1]) || !is_int(a[2])) {
            return make_bool(false);
        }
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        auto fidx = as_int(a[1]);
        auto bidx = as_int(a[2]);
        if (fidx < 0 || bidx < 0)
            return make_bool(false);
        if (!ev.is_block_dirty_fn_)
            return make_bool(false);
        return make_bool(ev.is_block_dirty_fn_(ev.string_heap_[idx].c_str(),
                                               static_cast<std::size_t>(fidx),
                                               static_cast<std::uint32_t>(bidx)));
    });

    // (compile:mark-block-dirty! name func-idx block-idx) —
    // Issue #196: fine-grained mark a single (function, block)
    // dirty in the named define's IR cache entry. Returns
    // #t on success, #f if the entry doesn't exist or the
    // hook is not installed. Use case: the smarter
    // re-lower (Phase 5 follow-up) marks only the affected
    // blocks rather than the whole entry.
    //
    // Issue #1896: MutationBoundaryGuard::try_acquire + try/catch so
    // exception mid-hook cannot leave partial IR dirty bits committed.
    add("compile:mark-block-dirty!", [&ev](const auto& a) -> EvalValue {
        // Issue #1326 Phase 1: deprecation path (prefer C++ Service / no user write).
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            record_write_side_prim_deprecation(m);
        // Issue #1293 Phase 1: kCapCompileDirty required in sandbox.
        if (ev.sandbox_mode() && !ev.has_capability(aura::compiler::security::kCapCompileDirty) &&
            !ev.has_capability(aura::compiler::security::kCapCompile) &&
            !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->capability_compile_denials.fetch_add(1, std::memory_order_relaxed);
                m->cap_denial_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "capability denied: compile-dirty required",
                                        ev.primitive_error_counter_ptr());
        }
        if (a.size() < 3 || !is_string(a[0]) || !is_int(a[1]) || !is_int(a[2])) {
            return make_bool(false);
        }
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        auto fidx = as_int(a[1]);
        auto bidx = as_int(a[2]);
        if (fidx < 0 || bidx < 0)
            return make_bool(false);
        if (!ev.mark_block_dirty_fn_)
            return make_bool(false);
        const auto* name = ev.string_heap_[idx].c_str();
        const auto fi = static_cast<std::size_t>(fidx);
        const auto bi = static_cast<std::uint32_t>(bidx);
        return run_compile_dirty_under_guard(
            ev, [&]() { return make_bool(ev.mark_block_dirty_fn_(name, fi, bi)); });
    });

    // (compile:clear-block-dirty! name func-idx block-idx) —
    // Issue #196: clear a single (function, block) dirty bit
    // in the named define's IR cache entry. Returns #t on
    // success, #f if the entry doesn't exist or the hook is
    // not installed. Use case: the smarter re-lower clears
    // the dirty bit after re-lowering a block.
    //
    // Issue #1896: Guard + try/catch (subtractive clear is high-risk
    // for stale IR if a throw leaves partial clear).
    add("compile:clear-block-dirty!", [&ev](const auto& a) -> EvalValue {
        // Issue #1326 Phase 1: deprecation path.
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            record_write_side_prim_deprecation(m);
        // Issue #1293 Phase 1: clearing dirty can hide stale IR — gate hard.
        if (ev.sandbox_mode() && !ev.has_capability(aura::compiler::security::kCapCompileDirty) &&
            !ev.has_capability(aura::compiler::security::kCapCompile) &&
            !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->capability_compile_denials.fetch_add(1, std::memory_order_relaxed);
                m->cap_denial_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "capability denied: compile-dirty required",
                                        ev.primitive_error_counter_ptr());
        }
        if (a.size() < 3 || !is_string(a[0]) || !is_int(a[1]) || !is_int(a[2])) {
            return make_bool(false);
        }
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        auto fidx = as_int(a[1]);
        auto bidx = as_int(a[2]);
        if (fidx < 0 || bidx < 0)
            return make_bool(false);
        if (!ev.clear_block_dirty_fn_)
            return make_bool(false);
        const auto* name = ev.string_heap_[idx].c_str();
        const auto fi = static_cast<std::size_t>(fidx);
        const auto bi = static_cast<std::uint32_t>(bidx);
        return run_compile_dirty_under_guard(
            ev, [&]() { return make_bool(ev.clear_block_dirty_fn_(name, fi, bi)); });
    });
}

// Issue #909 compile part 27 (orig 2073-2131)
void CompilePrims::register_compile_p27(PrimRegistrar add, Evaluator& ev) {

    // Issue #460: (compile:is-instruction-dirty? name func-idx
    // block-idx instr-idx) — per-instruction dirty query
    // (mirrors is-block-dirty? for the per-instruction level).
    // Returns #t if (i, b, k) is marked dirty in the named
    // define's IR cache entry, #f otherwise.
    add("compile:is-instruction-dirty?", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 4 || !is_string(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !is_int(a[3]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        if (!ev.is_instruction_dirty_fn_)
            return make_bool(false);
        return make_bool(ev.is_instruction_dirty_fn_(
            ev.string_heap_[idx].c_str(), static_cast<std::size_t>(as_int(a[1])),
            static_cast<std::uint32_t>(as_int(a[2])), static_cast<std::uint32_t>(as_int(a[3]))));
    });

    // Issue #460: (compile:mark-instruction-dirty! name
    // func-idx block-idx instr-idx) — per-instruction dirty
    // marker. Returns #t on success, #f if no hook.
    //
    // Issue #1896: try_acquire Guard + try/catch (parity with clear).
    add("compile:mark-instruction-dirty!", [&ev](const auto& a) -> EvalValue {
        // Issue #1326 Phase 1: deprecation path (JIT deopt DoS vector).
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            record_write_side_prim_deprecation(m);
        // Issue #1395: capability gate — require kCapWildcard.
        // Bit-flipping dirty state can poison Issue #147 invariant
        // checks; gate behind highest privilege.
        if (ev.sandbox_mode() && !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->capability_compile_denials.fetch_add(1, std::memory_order_relaxed);
                m->cap_denial_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "capability denied: kCapWildcard required",
                                        ev.primitive_error_counter_ptr());
        }
        if (a.size() < 4 || !is_string(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !is_int(a[3]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        if (!ev.mark_instruction_dirty_fn_)
            return make_bool(false);
        const auto* name = ev.string_heap_[idx].c_str();
        const auto fi = static_cast<std::size_t>(as_int(a[1]));
        const auto bi = static_cast<std::uint32_t>(as_int(a[2]));
        const auto ii = static_cast<std::uint32_t>(as_int(a[3]));
        return run_compile_dirty_under_guard(
            ev, [&]() { return make_bool(ev.mark_instruction_dirty_fn_(name, fi, bi, ii)); });
    });

    // Issue #460: (compile:clear-instruction-dirty! name
    // func-idx block-idx instr-idx) — per-instruction clear.
    //
    // Issue #1853 / #1896: try_acquire Guard + try/catch. Pre-#1853
    // capability gate only controlled *who* may call; a throw mid-clear
    // left IR dirty bits partially cleared (subtractive — worse than
    // mark-instruction-dirty!) with no panic checkpoint restore.
    // Exception → #f + metrics; dtor restores (#184/#236 outermost-only).
    add("compile:clear-instruction-dirty!", [&ev](const auto& a) -> EvalValue {
        // Issue #1395: capability gate — require kCapWildcard.
        if (ev.sandbox_mode() && !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->capability_compile_denials.fetch_add(1, std::memory_order_relaxed);
                m->cap_denial_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "capability denied: kCapWildcard required",
                                        ev.primitive_error_counter_ptr());
        }
        if (a.size() < 4 || !is_string(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !is_int(a[3]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        if (!ev.clear_instruction_dirty_fn_)
            return make_bool(false);
        const auto* name = ev.string_heap_[idx].c_str();
        const auto func_idx = static_cast<std::size_t>(as_int(a[1]));
        const auto block_idx = static_cast<std::uint32_t>(as_int(a[2]));
        const auto instr_idx = static_cast<std::uint32_t>(as_int(a[3]));
        return run_compile_dirty_under_guard(ev, [&]() {
            return make_bool(ev.clear_instruction_dirty_fn_(name, func_idx, block_idx, instr_idx));
        });
    });
    // Issue #460: (query:compiler-incremental-stats) — return
    // the current partial-relower / impact-scope counters.
    // P0 ship: returns the partial_relower_count as an int.
    // Follow-up: returns a 3-tuple
    // (partial-relower impact-scope-calls total-affected-blocks).
    ObservabilityPrims::register_stats_impl(
        "query:compiler-incremental-stats", [&ev](const auto& a) -> EvalValue {
            (void)a;
            return make_int(static_cast<std::int64_t>(ev.get_partial_relower_count()));
        });

    // Issue #1896: query:compile-primitive-guard-stats — Agent-discoverable
    // counters for compile dirty-bit mutators under MutationBoundaryGuard::
    // try_acquire + exception → stale-IR prevention.
    // Schema **1896**. Keys:
    //   guard-captures / compile_primitive_guard_captures_total
    //   stale-ir-prevented / compile_primitive_stale_ir_prevented_total
    //   guard-exceptions / mutation_guard_exception_total
    //   dirty-mutators-guarded (1 = mark/clear block+instruction + feedback)
    //   try-acquire-wired (1)
    ObservabilityPrims::register_stats_impl(
        "query:compile-primitive-guard-stats", [&ev](const auto&) -> EvalValue {
            std::int64_t captures = 0, stale = 0, exceptions = 0;
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                captures = static_cast<std::int64_t>(
                    m->compile_primitive_guard_captures_total.load(std::memory_order_relaxed));
                stale = static_cast<std::int64_t>(
                    m->compile_primitive_stale_ir_prevented_total.load(std::memory_order_relaxed));
                exceptions = static_cast<std::int64_t>(
                    m->mutation_guard_exception_total.load(std::memory_order_relaxed));
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
            insert_kv("guard-captures", captures);
            insert_kv("compile_primitive_guard_captures_total", captures);
            insert_kv("stale-ir-prevented", stale);
            insert_kv("compile_primitive_stale_ir_prevented_total", stale);
            insert_kv("guard-exceptions", exceptions);
            insert_kv("mutation_guard_exception_total", exceptions);
            insert_kv("dirty-mutators-guarded", 1);
            insert_kv("try-acquire-wired", 1);
            insert_kv("from-verification-feedback-guarded", 1);
            insert_kv("commercial-stub-guarded", 1);
            insert_kv("schema", 1896);
            insert_kv("issue", 1896);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1897 / #1950 / #1931 / #1953: query:mutation-systemic-guard-stats —
    // audit inventory for systemic MutationBoundaryGuard enforcement.
    // Schema lineage **1897**; **schema-1931** closes hot-update zero-downtime
    // reliability mandate (dtor ≤6 atomics + 100% compile/mutate Guard wrap +
    // AC metrics mutation_guard_exception_total /
    // compile_primitive_stale_ir_prevented_total). **schema-1953** is the
    // re-audit refine of #1931 (same AC surface, explicit issue key).
    ObservabilityPrims::register_stats_impl(
        "query:mutation-systemic-guard-stats", [&ev](const auto&) -> EvalValue {
            std::int64_t captures = 0, stale = 0, exceptions = 0, auto_rb = 0, wrapped = 0;
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                captures = static_cast<std::int64_t>(
                    m->compile_primitive_guard_captures_total.load(std::memory_order_relaxed));
                stale = static_cast<std::int64_t>(
                    m->compile_primitive_stale_ir_prevented_total.load(std::memory_order_relaxed));
                exceptions = static_cast<std::int64_t>(
                    m->mutation_guard_exception_total.load(std::memory_order_relaxed));
                auto_rb = static_cast<std::int64_t>(
                    m->mutation_guard_uncaught_auto_rollback_total.load(std::memory_order_relaxed));
                wrapped = static_cast<std::int64_t>(
                    m->mutation_boundary_primitives_wrapped.load(std::memory_order_relaxed));
            }
            // #1931 adds AC wire keys — create(64) headroom.
            auto* ht = FlatHashTable::create(64);
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
            insert_kv("guard-captures", captures);
            insert_kv("stale-ir-prevented", stale);
            insert_kv("compile_primitive_stale_ir_prevented_total", stale); // #1931 AC metric
            insert_kv("guard-exceptions", exceptions);
            insert_kv("mutation_guard_exception_total", exceptions); // #1931 AC metric
            insert_kv("uncaught-auto-rollback", auto_rb);
            insert_kv("mutation_guard_uncaught_auto_rollback_total", auto_rb);
            insert_kv("boundary-primitives-wrapped", wrapped);
            // Inventory flags (1 = try_acquire + try/catch wired)
            insert_kv("mark-clear-block-instruction-dirty", 1);
            insert_kv("mark-dirty-upward-fast", 1);
            insert_kv("clear-macro-dirty", 1);
            insert_kv("mark-narrowing-dirty", 1);
            insert_kv("subtree-bump", 1);
            insert_kv("per-defuse-index-add", 1);
            insert_kv("hw-bitvec-register", 1);
            insert_kv("compact-env-frames", 1);
            insert_kv("from-verification-feedback", 1);
            insert_kv("run-verification-feedback", 1);
            insert_kv("commercial-simulator-stub", 1);
            insert_kv("try-acquire-wired", 1);
            insert_kv("uncaught-exceptions-dtor-wired", 1);
            insert_kv("schema", 1897);
            insert_kv("issue", 1897);
            // Issue #1931 / #1953 unified hot-update reliability surface.
            insert_kv("schema-1931", 1931);
            insert_kv("issue-1931", 1931);
            insert_kv("schema-1953", 1953); // refine #1931 re-audit
            insert_kv("issue-1953", 1953);
            insert_kv("schema-1950", 1950);
            insert_kv("schema-1747", 1747);
            insert_kv("dtor-common-path-atomics-cap", 6); // #1747 / #1931 / #1953 ≤6
            insert_kv("dtor-batch-metrics-wired", 1);
            insert_kv("compile-mutate-guard-coverage-100pct", 1); // linter --strict
            insert_kv("shared-helper-header-wired", 1);           // mutation_guard_helpers.hh
            insert_kv("coverage-linter-wired", 1); // scripts/check_mutation_guard_coverage.py
            insert_kv("exception-auto-rollback-wired", 1); // uncaught_exceptions dtor flip
            insert_kv("run-under-mutation-guard-helper", 1);
            insert_kv("active", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1898: query:raw-pointer-safety-stats — pin/generation TOCTOU
    // soft-fail observability for non-owning compiler_service_ /
    // type_registry_ / workspace_flat_ back-pointers. Schema **1898**.
    ObservabilityPrims::register_stats_impl(
        "query:raw-pointer-safety-stats", [&ev](const auto&) -> EvalValue {
            std::int64_t uaf = 0, svc_rej = 0, reg_rej = 0, ws_pin = 0;
            std::int64_t svc_gen = 0, reg_gen = 0, ws_gen = 0;
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                uaf = static_cast<std::int64_t>(
                    m->raw_pointer_uaf_prevented_total.load(std::memory_order_relaxed));
                svc_rej = static_cast<std::int64_t>(
                    m->compiler_service_pin_reject_total.load(std::memory_order_relaxed));
                reg_rej = static_cast<std::int64_t>(
                    m->type_registry_pin_reject_total.load(std::memory_order_relaxed));
                ws_pin = static_cast<std::int64_t>(
                    m->workspace_flat_pin_total.load(std::memory_order_relaxed));
            }
            svc_gen = static_cast<std::int64_t>(ev.compiler_service_generation());
            reg_gen = static_cast<std::int64_t>(ev.type_registry_generation());
            ws_gen = static_cast<std::int64_t>(ev.workspace_flat_generation());
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
            insert_kv("raw-pointer-uaf-prevented", uaf);
            insert_kv("raw_pointer_uaf_prevented_total", uaf);
            insert_kv("service-pin-reject", svc_rej);
            insert_kv("compiler_service_pin_reject_total", svc_rej);
            insert_kv("type-registry-pin-reject", reg_rej);
            insert_kv("type_registry_pin_reject_total", reg_rej);
            insert_kv("workspace-flat-pin-total", ws_pin);
            insert_kv("workspace_flat_pin_total", ws_pin);
            insert_kv("compiler-service-generation", svc_gen);
            insert_kv("type-registry-generation", reg_gen);
            insert_kv("workspace-flat-generation", ws_gen);
            insert_kv("pin-api-wired", 1);
            insert_kv("workspace-flat-pin-raii", 1);
            insert_kv("service-pin-wired", 1);
            insert_kv("type-registry-pin-wired", 1);
            insert_kv("schema", 1898);
            insert_kv("issue", 1898);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 compile part 28 (orig 2132-2233)
void CompilePrims::register_compile_p28(PrimRegistrar add, Evaluator& ev) {

    // Issue #426 / #293: (query:compiler-cache-stats) —
    // return the current per-block dirty summary across the
    // compiler cache as a 3-tuple
    // (total-dirty-blocks total-dirty-functions
    //  incremental-candidates). The "incremental
    // candidates" count is the number of functions whose
    // dirty block count falls in [1..7] (per
    // estimate_relower_blocks); 8+ dirty blocks is
    // "full re-lower" territory and 0 is "clean".
    //
    // AI Agents can use the 3-tuple to decide whether the
    // next (compile:relower) should be incremental
    // (incremental-candidates > 0) or full (otherwise).
    ObservabilityPrims::register_stats_impl(
        "query:compiler-cache-stats", [&ev](const auto& a) -> EvalValue {
            (void)a;
            // Issue #1898: pin + revalidate multi-step dirty counter reads.
            return with_compiler_service_pin(
                ev,
                [&](CompilerService& svc) -> EvalValue {
                    // Build 3-tuple as nested pair-of-pairs:
                    // ((dirty-blocks . dirty-functions) . incremental-candidates)
                    std::int64_t dirty_blocks =
                        static_cast<std::int64_t>(svc.total_dirty_block_count());
                    std::int64_t dirty_funcs =
                        static_cast<std::int64_t>(svc.total_dirty_func_count());
                    std::int64_t incr_cands =
                        static_cast<std::int64_t>(svc.total_incremental_candidates());
                    auto p1 = ev.pairs_.size();
                    ev.pairs_.push_back({make_int(dirty_blocks), make_int(dirty_funcs)});
                    auto outer = ev.pairs_.size();
                    ev.pairs_.push_back({make_pair(p1), make_int(incr_cands)});
                    return make_pair(outer);
                },
                make_int(0));
        });

    // Issue #298: (query:incremental-effectiveness) — return a
    // 4-tuple aggregating compiler-pipeline observability
    // metrics for self-evolution loops. The 4 elements:
    //
    //   1. recompile-ratio: dirty-defines / total-defines (×10000
    //      for basis-point precision). 0 = no dirty defines, 10000
    //      = all dirty. Ratio > 5000 indicates a wide
    //      invalidation; agents should investigate mutation scope.
    //
    //   2. cascade-depth: max mark_dirty_upward depth seen in the
    //      last eval cycle. Deeper cascades indicate mutations that
    //      ripple through many parents. Used to detect
    //      unexpectedly wide invalidation chains.
    //
    //   3. bridge-overhead: total closure_bridge_calls (sum of
    //      bridge invocations across all defines). Use this to
    //      quantify how often the IR ↔ tree-walker fallback path
    //      is exercised.
    //
    //   4. fallback-frequency: count of fallback triggers (sum
    //      over all reasons: special forms, EDSL primitives,
    //      macros). 0 = pure IR path. High counts indicate the
    //      mutation touched a tree-walker-only construct.
    //
    // The 4 elements are returned as a flat 4-tuple
    // (e1 . (e2 . (e3 . e4))) so callers can destructure with
    // standard Aura car/cdr. All values are int (basis points
    // for ratio, raw counts for the rest).
    ObservabilityPrims::register_stats_impl(
        "query:incremental-effectiveness", [&ev](const auto& a) -> EvalValue {
            (void)a;
            auto* svc_void = ev.compiler_service();
            if (!svc_void)
                return make_int(0);
            auto* svc = static_cast<aura::compiler::CompilerService*>(svc_void);

            std::int64_t ratio_bp = 0;
            std::int64_t cascade_depth = 0;
            std::int64_t bridge_overhead = 0;
            std::int64_t fallback_freq = 0;

            // Issue #1854 / #1856: try_snapshot → void on fail
            // (not false-clean 4-tuple of zeros). Also bumps
            // incremental_effectiveness_snapshot_failures for #1854
            // call-site visibility (try_snapshot bumps
            // snapshot_failures_total).
            auto snap_opt = svc->try_snapshot();
            if (!snap_opt) {
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                    m->incremental_effectiveness_snapshot_failures.fetch_add(
                        1, std::memory_order_relaxed);
                }
                return make_void();
            }
            const auto& snap = *snap_opt;
            auto total_defines = svc->ir_cache_v2_size();
            auto dirty_funcs = svc->total_dirty_func_count();
            if (total_defines > 0) {
                ratio_bp = (dirty_funcs * ::aura::compiler::kBasisPointScale) /
                           static_cast<std::int64_t>(total_defines);
            }
            cascade_depth = static_cast<std::int64_t>(snap.mark_dirty_total_nodes);
            bridge_overhead = static_cast<std::int64_t>(snap.closure_bridge_calls);
            fallback_freq =
                static_cast<std::int64_t>(snap.closure_tw_calls + snap.closure_ffi_calls);

            // Build 4-tuple as nested pairs (right-associated):
            // (ratio . (cascade . (bridge . fallback)))
            auto p1 = ev.pairs_.size();
            ev.pairs_.push_back({make_int(bridge_overhead), make_int(fallback_freq)});
            auto p2 = ev.pairs_.size();
            ev.pairs_.push_back({make_int(cascade_depth), make_pair(p1)});
            auto p3 = ev.pairs_.size();
            ev.pairs_.push_back({make_int(ratio_bp), make_pair(p2)});
            return make_pair(p3);
        });
}

// Issue #909 compile part 29 (orig 2234-2291)
void CompilePrims::register_compile_p29(PrimRegistrar add, Evaluator& ev) {

    // Issue #293: (compile:relower-strategy <function-name>)
    // — returns a symbol describing the optimal re-lower
    // strategy for a cached function:
    //   'none — function is clean (0 dirty blocks)
    //   'incremental — 1..7 dirty blocks (per
    //     estimate_relower_blocks), targeted re-lower is
    //     cheaper than full
    //   'full — 8+ dirty blocks, full re-lower is on par
    //     with incremental (no point doing fine-grained work)
    //   'unknown — function not in the cache
    //
    // The 'full' threshold is conservative (8+ blocks is
    // half of the typical function size); agents that need
    // a different threshold can use the
    // query:compiler-cache-stats 3-tuple and decide
    // themselves. This primitive is the convenient default.
    //
    // Issue #1855: dirty_block_count via
    // ir_cache_v2_dirty_block_count (shared_lock on
    // jit_cache_mtx_) so concurrent mark/clear/invalidate
    // cannot race the map / entry. compiler_service_ is
    // non-owning (#1839 ownership / quiescence — no
    // shared_ptr on every cache probe).
    add("compile:relower-strategy", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        // Issue #1839 / #1855 / #1898: pin compiler_service_ (rebind-safe).
        const std::string fname = ev.string_heap_[idx];
        return with_compiler_service_pin(
            ev,
            [&](CompilerService& svc) -> EvalValue {
                // Issue #1855: locked snapshot (not ir_cache_v2_find pointer).
                auto dirty_opt = svc.ir_cache_v2_dirty_block_count(fname);
                if (!dirty_opt) {
                    auto sym_idx = ev.keyword_table_.size();
                    ev.keyword_table_.push_back("unknown");
                    return make_keyword(sym_idx);
                }
                const std::size_t dirty = *dirty_opt;
                const char* tag = nullptr;
                if (dirty == 0)
                    tag = "none";
                else if (dirty < 8)
                    tag = "incremental";
                else
                    tag = "full";
                auto sym_idx = ev.keyword_table_.size();
                ev.keyword_table_.push_back(tag);
                return make_keyword(sym_idx);
            },
            make_bool(false));
    });

    // Issue #459: (query:atomic-batch-stats) — return
    // the current nested-atomic-batch observability counters.
    // P0 ship: returns the atomic_batch_steal_violation_
    // count as an int. Follow-up: returns a 2-tuple
    // (steal-violations gc-bumps-lost).
    ObservabilityPrims::register_stats_impl(
        "query:atomic-batch-stats", [&ev](const auto& a) -> EvalValue {
            (void)a;
            return make_int(static_cast<std::int64_t>(ev.get_atomic_batch_steal_violation()));
        });
}

// Issue #909 compile part 30 (orig 2292-2346)
void CompilePrims::register_compile_p30(PrimRegistrar add, Evaluator& ev) {

    // Issue #437: (verify:assertion-failed node-id
    // [assert-name-string]) — Mark the given AST node with
    // the kAssertionDirty verify bit. The optional 2nd arg
    // is the assertion name (string, for observability
    // only — the P0 ship doesn't propagate the name into
    // the dirty bitmask). Returns the new verify_dirty
    // bitmask for the node (0 if no bits set after apply,
    // or the bitmask). On any failure (bad args, no
    // workspace) returns #f.
    add("verify:assertion-failed", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        auto node_id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (node_id >= ws->size())
            return make_bool(false);
        ws->apply_verify_dirty_bits(node_id, aura::ast::FlatAST::kAssertionDirty);
        return make_int(static_cast<std::int64_t>(ws->verify_dirty(node_id)));
    });

    // Issue #437: (verify:report-coverage node-id
    // [coverage-hole-name-string]) — Mark the given AST
    // node with the kCoverageDirty verify bit. Same
    // signature + return-value convention as
    // verify:assertion-failed.
    add("verify:report-coverage", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        auto node_id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (node_id >= ws->size())
            return make_bool(false);
        ws->apply_verify_dirty_bits(node_id, aura::ast::FlatAST::kCoverageDirty);
        return make_int(static_cast<std::int64_t>(ws->verify_dirty(node_id)));
    });

    // Issue #437: (query:verify-dirty-stats) — return
    // the current per-reason verify-dirty counters as
    // a 4-tuple (assertion coverage sva formal-cex).
    // P0 ship: returns an integer = sum of all four
    // counters. Follow-up: returns the 4-tuple.
    ObservabilityPrims::register_stats_impl(
        "query:verify-dirty-stats", [&ev](const auto& a) -> EvalValue {
            (void)a;
            auto* ws = ev.workspace_flat();
            if (!ws)
                return make_int(0);
            auto sum = ws->verify_assertion_dirty_total() + ws->verify_coverage_dirty_total() +
                       ws->verify_sva_dirty_total() + ws->verify_formal_cex_dirty_total();
            return make_int(static_cast<std::int64_t>(sum));
        });
}

// Issue #909 compile part 31 (orig 2347-2403)
void CompilePrims::register_compile_p31(PrimRegistrar add, Evaluator& ev) {

    // Issue #437: (compile:verify-dirty? node-id) — query
    // the verify_dirty_ bitmask for a node. Returns the
    // bitmask (0 if not set or no bits). #f on bad args.
    add("compile:verify-dirty?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        auto node_id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (node_id >= ws->size())
            return make_bool(false);
        return make_int(static_cast<std::int64_t>(ws->verify_dirty(node_id)));
    });

    // Issue #290: helper for the macro_dirty_ primitives.
    // The macro_dirty_ column lives on the flat where
    // macro_expand_all / clone_macro_body actually run — that
    // is the per-eval current flat (current_flat_), NOT the
    // persistent workspace_flat_. The workspace holds the
    // pre-expansion source; the eval flat holds the cloned
    // result. Re-resolve each call so we see the most recent
    // eval flat (the lambda captures the helper by value, so
    // the helper itself is fine, but it forwards to live
    // ev.current_flat() / ev.workspace_flat() at every call).
    // Issue #290: (compile:macro-dirty? node-id) — query the
    // macro_dirty_ bitmask for a node. Returns the bitmask
    // (0 if not set or no bits). #f on bad args.
    // Bit 0 = kMacroExpansion (cloned by clone_macro_body),
    // bit 1 = kMacroSelfModify (touched by a self-evolution
    // step).
    add("compile:macro-dirty?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = CompilePrims::pick_macro_flat(ev);
        if (!ws)
            return make_bool(false);
        auto node_id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (node_id >= ws->size())
            return make_bool(false);
        return make_int(static_cast<std::int64_t>(ws->macro_dirty(node_id)));
    });

    // Issue #290: (compile:macro-dirty-count) — number of
    // nodes with any macro_dirty_ bit set on the eval flat
    // where macro expansion actually ran. #f on no flat.
    ObservabilityPrims::register_stats_impl(
        "compile:macro-dirty-count", [&ev](const auto&) -> EvalValue {
            auto* ws = CompilePrims::pick_macro_flat(ev);
            if (!ws)
                return make_bool(false);
            return make_int(static_cast<std::int64_t>(ws->macro_dirty_count()));
        });
}
// Issue #1771: shared "node_id [comment]" line parser for
// verify:parse-coverage-feedback / parse-assert-failure /
// parse-formal-cex. Returns count of successfully marked nodes.
// Callers own attempt/success metrics (they differ slightly).
namespace {
    std::uint64_t parse_verify_node_id_lines(const std::string& text, aura::ast::FlatAST& ws,
                                             Evaluator& ev, std::uint8_t dirty_bit) {
        std::uint64_t marked = 0;
        std::size_t i = 0;
        while (i < text.size()) {
            std::size_t j = i;
            while (j < text.size() && text[j] != '\n')
                ++j;
            const std::string_view line(text.data() + i, j - i);
            std::size_t k = 0;
            while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
                ++k;
            if (k < line.size() && line[k] >= '0' && line[k] <= '9') {
                std::size_t val = 0;
                while (k < line.size() && line[k] >= '0' && line[k] <= '9') {
                    val = val * 10 + static_cast<std::size_t>(line[k] - '0');
                    ++k;
                }
                const auto nid = static_cast<aura::ast::NodeId>(val);
                if (nid < ws.size()) {
                    ws.apply_verification_dirty_bits(nid, dirty_bit);
                    ++marked;
                }
            } else {
                // Non-integer line — bump parse-error counter for
                // query:verify-tool-stats.
                ev.bump_verify_tool_parse_error();
            }
            i = (j < text.size()) ? j + 1 : j;
        }
        return marked;
    }
} // namespace

// Issue #909 compile part 32 (orig 2404-2496)
void CompilePrims::register_compile_p32(PrimRegistrar add, Evaluator& ev) {

    // Issue #290: (compile:clear-macro-dirty!) — clear all
    // macro_dirty_ bits on the eval flat. Useful after a
    // self-evolution loop has fully reprocessed the affected
    // subtrees and wants to start fresh on the next cycle.
    // Returns #t on success, #f if no flat.
    add("compile:clear-macro-dirty!", [&ev](const auto&) -> EvalValue {
        // Issue #1395: capability gate — require kCapWildcard.
        if (ev.sandbox_mode() && !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->capability_compile_denials.fetch_add(1, std::memory_order_relaxed);
                m->cap_denial_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "capability denied: kCapWildcard required",
                                        ev.primitive_error_counter_ptr());
        }
        auto* ws = CompilePrims::pick_macro_flat(ev);
        if (!ws)
            return make_bool(false);
        // Issue #1897: subtractive clear of all macro_dirty_ bits under Guard
        // (exception mid-clear must not leave partial IR hygiene state).
        return run_compile_dirty_under_guard(ev, [&]() -> EvalValue {
            ws->clear_macro_dirty_all();
            return make_bool(true);
        });
    });

    // Issue #290: (compile:macro-dirty-stats) — return the
    // running per-reason counters as a single integer (sum
    // of kMacroExpansion + kMacroSelfModify newly-set
    // totals). P0 ship mirrors (query:verify-dirty-stats).
    // Follow-up: return a pair so callers can distinguish
    // the two reasons without separate primitives.
    ObservabilityPrims::register_stats_impl(
        "compile:macro-dirty-stats", [&ev](const auto&) -> EvalValue {
            auto* ws = CompilePrims::pick_macro_flat(ev);
            if (!ws)
                return make_bool(false);
            auto sum = ws->macro_expansion_dirty_total() + ws->macro_self_modify_dirty_total();
            return make_int(static_cast<std::int64_t>(sum));
        });

    // Issue #469: (verify:parse-coverage-feedback text-string)
    // — parse a text blob describing coverage holes from an
    // external SV simulator and mark the affected AST nodes
    // dirty with the kCoverageFeedbackDirty bit.
    //
    // Format (one per line): "node_id hole_name"
    // Example:
    //   "0 hit_rate=0.45"
    //   "3 miss_var_x"
    //
    // P0: the text is parsed line-by-line. Each line
    // starts with a non-negative integer (the NodeId).
    // Anything after the integer is ignored (it's a
    // human-readable hole name; P0 doesn't use it).
    // Lines that don't start with an integer are
    // skipped.
    //
    // Returns: the count of nodes successfully marked
    // dirty. #f on bad args.
    add("verify:parse-coverage-feedback", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        // Issue #1347: count harness parse attempts even without a workspace.
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->verify_parse_coverage_total.fetch_add(1, std::memory_order_relaxed);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_int(0);
        auto text_idx = as_string_idx(a[0]);
        if (text_idx >= ev.string_heap_.size())
            return make_int(0);
        // Issue #1771: shared line parser.
        const auto marked = parse_verify_node_id_lines(ev.string_heap_[text_idx], *ws, ev,
                                                       aura::ast::FlatAST::kCoverageFeedbackDirty);
        if (marked > 0) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->verify_auto_trigger_mutate_total.fetch_add(1, std::memory_order_relaxed);
            ev.bump_sv_self_evo_feedback_parse();
        }
        return make_int(static_cast<std::int64_t>(marked));
    });
}

// Issue #909 compile part 33 (orig 2497-2589)
void CompilePrims::register_compile_p33(PrimRegistrar add, Evaluator& ev) {

    // Issue #469: (verify:parse-assert-failure text-string)
    // — parse a text blob describing assertion failures
    // from an external SV simulator and mark the affected
    // AST nodes dirty with the kAssertFailureDirty bit.
    // Same format as (verify:parse-coverage-feedback).
    add("verify:parse-assert-failure", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        // Issue #1347: count harness parse attempts even without a workspace.
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->verify_parse_assert_total.fetch_add(1, std::memory_order_relaxed);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_int(0);
        auto text_idx = as_string_idx(a[0]);
        if (text_idx >= ev.string_heap_.size())
            return make_int(0);
        // Issue #1771: shared line parser.
        const auto marked = parse_verify_node_id_lines(ev.string_heap_[text_idx], *ws, ev,
                                                       aura::ast::FlatAST::kAssertFailureDirty);
        if (marked > 0) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->verify_auto_trigger_mutate_total.fetch_add(1, std::memory_order_relaxed);
            ev.bump_sv_self_evo_feedback_parse();
        }
        return make_int(static_cast<std::int64_t>(marked));
    });

    // Issue #802: (verify:parse-formal-cex text-string)
    // — parse formal counterexample report and mark nodes dirty with
    // kFormalCounterexampleDirty (same line format as coverage/assert parsers).
    add("verify:parse-formal-cex", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_int(0);
        auto text_idx = as_string_idx(a[0]);
        if (text_idx >= ev.string_heap_.size())
            return make_int(0);
        // Issue #1771: shared line parser (no auto-trigger metric — #802 BC).
        const auto marked = parse_verify_node_id_lines(
            ev.string_heap_[text_idx], *ws, ev, aura::ast::FlatAST::kFormalCounterexampleDirty);
        if (marked > 0)
            ev.bump_sv_self_evo_feedback_parse();
        return make_int(static_cast<std::int64_t>(marked));
    });
}

// Issue #909 compile part 34 (orig 2590-2735)
void CompilePrims::register_compile_p34(PrimRegistrar add, Evaluator& ev) {

    // Issue #802: (mutate:from-verification-feedback strategy node-id payload)
    // — strategy-driven structured SV mutate under Guard with StableNodeRef
    // capture. Delegates to existing eda:weaken-property / eda:add-coverpoint-bin /
    // eda:update-constraint primitives.
    //
    // Issue #1896: wrap delegation under try_acquire Guard so a throw from
    // nested eda:* cannot leave partial structured mutate state without
    // panic-checkpoint restore (NodeId validated before Guard).
    add("mutate:from-verification-feedback", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 3 || !is_string(a[0]) || !is_int(a[1]) || !is_string(a[2]))
            return make_bool(false);
        auto strategy_idx = as_string_idx(a[0]);
        if (strategy_idx >= ev.string_heap_.size())
            return make_bool(false);
        const auto& strategy = ev.string_heap_[strategy_idx];
        const auto node_id = static_cast<std::int64_t>(as_int(a[1]));
        auto payload_idx = as_string_idx(a[2]);
        if (payload_idx >= ev.string_heap_.size())
            return make_bool(false);
        // Issue #1772: validate NodeId before eda:* delegation so invalid
        // agent targets are observable (mutate_from_feedback_invalid_node_total)
        // and never rely solely on each delegate's optional OOB check.
        if (auto* ws = ev.workspace_flat()) {
            if (node_id < 0 || static_cast<std::uint64_t>(node_id) >= ws->size()) {
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                    m->mutate_from_feedback_invalid_node_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                return make_bool(false);
            }
        } else {
            // No workspace: cannot resolve NodeId — treat as invalid target.
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->mutate_from_feedback_invalid_node_total.fetch_add(1, std::memory_order_relaxed);
            return make_bool(false);
        }
        return run_compile_dirty_under_guard(ev, [&]() -> EvalValue {
            auto delegate = [&](const char* name) -> bool {
                auto fn = ev.primitives_.lookup(name);
                if (!fn)
                    return false;
                auto r = (*fn)({make_int(node_id), make_string(payload_idx)});
                return is_bool(r) && as_bool(r);
            };
            bool ok = false;
            if (strategy == "weaken-property" || strategy == "assert-fail")
                ok = false; // eda:weaken-property retired 4.4
            else if (strategy == "add-coverpoint" || strategy == "coverage-hole")
                ok = false; // eda:add-coverpoint-bin retired 4.4
            else if (strategy == "relax-constraint" || strategy == "structural-fix")
                ok = false; // eda:update-constraint retired 4.4
            if (!ok)
                return make_bool(false);
            ev.bump_sv_self_evo_structured_mutate();
            ev.bump_sv_self_evo_closed_loop_rounds();
            ev.bump_sv_self_evo_convergence_hits();
            ev.bump_closed_loop_feedback_mutate_round();
            return make_bool(true);
        });
    });
}

// Issue #909 compile part 35 (orig 2736-2875)
void CompilePrims::register_compile_p35(PrimRegistrar add, Evaluator& ev) {}

// Issue #909 compile part 36 (orig 2876-2965)
void CompilePrims::register_compile_p36(PrimRegistrar add, Evaluator& ev) {

    // Issue #318: (verify:coverage-holes [report-text]) —
    // return a list of NodeIds that have
    // kCoverageFeedbackDirty set (i.e. nodes whose coverpoint
    // / covergroup simulation reports flagged as uncovered).
    //
    // Args:
    //   report-text — optional string in the same format as
    //                  (verify:parse-coverage-feedback)
    //                  (newline-separated NodeIds). When
    //                  provided, the primitive first marks the
    //                  listed nodes dirty (same effect as the
    //                  parse primitive) then returns the
    //                  combined coverage-dirty list. When
    //                  omitted, the primitive just returns
    //                  whatever's already dirty.
    //
    // Returns: a pair-list of NodeIds (newest-first so the
    //   caller can iterate left-to-right via (car / cdr) and
    //   stop early if desired).
    //
    // Composes with (query:where :node-type "...") for
    // filtered coverage-hole scans and
    // (mutate:query-and-replace ...) for automated refine.
    add("verify:coverage-holes", [&ev](std::span<const EvalValue> a) -> EvalValue {
        // Optional first arg: parse the report first (so
        // downstream calls see freshly-marked dirty nodes).
        // Issue #1816: hold workspace_mtx_ for the whole
        // verification_dirty_ critical section so concurrent
        // apply_verification_dirty_bits (other fibers) cannot
        // tear bits or race a side-table realloc. Unique when
        // marking from report text; shared for scan-only.
        const bool have_report = !a.empty() && is_string(a[0]);
        auto collect = [&ev](aura::ast::FlatAST& ws) -> EvalValue {
            // Collect coverage-dirty NodeIds into a pair-list
            // (newest-first; reverse-iterates the flat so the
            // last-marked node is at car, matching the
            // (query:templates) ordering convention).
            EvalValue list = make_void();
            const auto n = ws.size();
            // Issue #319 follow-up: read from
            // `verification_dirty_` (#469), NOT `verify_dirty_`
            // (#437). The two columns hold different bit sets:
            //   - verify_dirty_ (legacy #437): verify_assertion /
            //     verify_coverage / verify_sva / verify_formal_cex
            //   - verification_dirty_ (new #469):
            //     kCoverageFeedbackDirty / kAssertFailureDirty
            //     (full byte per #313)
            // `apply_verification_dirty_bits` (from #469) writes
            // to `verification_dirty_`, so the coverage-holes
            // read must use that column too. The legacy
            // `verify_dirty(id)` would always return 0 for
            // kCoverageFeedbackDirty (a different namespace).
            for (std::size_t id = n; id-- > 0;) {
                const auto bits = ws.verification_dirty(static_cast<aura::ast::NodeId>(id));
                if (bits & aura::ast::FlatAST::kCoverageFeedbackDirty) {
                    auto idx = ev.string_heap_.size();
                    ev.string_heap_.push_back(std::to_string(id));
                    auto pid = ev.pairs_.size();
                    ev.pairs_.push_back({make_string(idx), list});
                    list = make_pair(pid);
                }
            }
            return list;
        };
        if (have_report) {
            std::unique_lock<std::shared_mutex> wlock(ev.workspace_mtx_);
            auto* ws = ev.workspace_flat();
            if (!ws)
                return make_void();
            auto text_idx = as_string_idx(a[0]);
            if (text_idx < ev.string_heap_.size()) {
                const std::string& text = ev.string_heap_[text_idx];
                std::size_t i = 0;
                while (i < text.size()) {
                    std::size_t j = i;
                    while (j < text.size() && text[j] != '\n')
                        ++j;
                    const std::string_view line(text.data() + i, j - i);
                    std::size_t k = 0;
                    while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
                        ++k;
                    if (k < line.size() && line[k] >= '0' && line[k] <= '9') {
                        std::size_t val = 0;
                        while (k < line.size() && line[k] >= '0' && line[k] <= '9') {
                            val = val * 10 + (line[k] - '0');
                            ++k;
                        }
                        const auto nid = static_cast<aura::ast::NodeId>(val);
                        if (nid < ws->size()) {
                            ws->apply_verification_dirty_bits(
                                nid, aura::ast::FlatAST::kCoverageFeedbackDirty);
                        }
                    }
                    i = (j < text.size()) ? j + 1 : j;
                }
            }
            return collect(*ws);
        }
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_void();
        return collect(*ws);
    });
}

// Issue #909 compile part 37 (orig 2966-3043)
void CompilePrims::register_compile_p37(PrimRegistrar add, Evaluator& ev) {

    // Issue #318: (verify:suggest-constraint-refine) —
    // return a list of NodeIds that are candidates for
    // constraint refinement. Currently this is the
    // kCoverageFeedbackDirty set (the same coverage-holes
    // list, since uncovered coverpoints are the canonical
    // "constraint needs refining" signal). The separate
    // primitive gives the AI agent / editor tool a
    // stable name to call even if the heuristic
    // expands later (e.g. include
    // kAssertFailureDirty nodes too, or expand to a
    // heuristic that walks parent chains).
    //
    // Args: none.
    //
    // Returns: pair-list of NodeIds (newest-first). Each
    //   NodeId is also kCoverageFeedbackDirty (and the
    //   caller can use (query:where ...) to filter by
    //   :node-type "Define" / "Lambda" / etc. before
    //   piping through (mutate:query-and-replace ...)).
    //
    // Composes with (verify:coverage-holes) (same source
    // set) and (ast:snapshot / ast:rollback) for safe
    // experimentation — callers are expected to snapshot
    // before a refine loop and rollback on failure.
    add("verify:suggest-constraint-refine", [&ev](const auto& a) -> EvalValue {
        (void)a;
        // Issue #1816: shared_lock over verification_dirty_ scan
        // (same race class as verify:coverage-holes).
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_void();
        EvalValue list = make_void();
        const auto n = ws->size();
        // Issue #319 follow-up: use verification_dirty_ (#469)
        // column, not verify_dirty_ (#437). See the matching
        // note in (verify:coverage-holes) above for the
        // namespace distinction.
        for (std::size_t id = n; id-- > 0;) {
            const auto bits = ws->verification_dirty(static_cast<aura::ast::NodeId>(id));
            if (bits & aura::ast::FlatAST::kCoverageFeedbackDirty) {
                auto idx = ev.string_heap_.size();
                ev.string_heap_.push_back(std::to_string(id));
                auto pid = ev.pairs_.size();
                ev.pairs_.push_back({make_string(idx), list});
                list = make_pair(pid);
            }
        }
        return list;
    });

    // Issue #240: (compile:mark-narrowing-dirty! node-id
    // [set-or-clear]) — Set or clear the per-node
    // kOccurrenceDirty bit in the workspace FlatAST's
    // dirty bitmask. The post-mutation invariant check
    // (find_occurrence_contexts in type_checker_impl.cpp)
    // scopes its diagnostic to nodes with this bit set,
    // rather than the conservative pre-#240 path that
    // flagged every if-context in the dirty scope.
    //
    // Args:
    //   node-id   — integer NodeId in the workspace flat
    //   set-clear — optional bool, default #t (set). Pass
    //               #f to clear the bit after a successful
    //               re-narrowing pass.
    //
    // Returns #t on success, #f if the hook is not installed
    // or the node-id is out of range.
    add("compile:mark-narrowing-dirty!", [&ev](const auto& a) -> EvalValue {
        // Issue #1293 Phase 1: deopt DoS vector — require kCapCompileDeopt.
        if (ev.sandbox_mode() && !ev.has_capability(aura::compiler::security::kCapCompileDeopt) &&
            !ev.has_capability(aura::compiler::security::kCapCompile) &&
            !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->capability_compile_denials.fetch_add(1, std::memory_order_relaxed);
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "capability denied: compile-deopt required",
                                        ev.primitive_error_counter_ptr());
        }
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        if (!ev.set_occurrence_dirty_fn_)
            return make_bool(false);
        auto node_id = static_cast<std::uint32_t>(as_int(a[0]));
        bool set = true;
        if (a.size() >= 2 && is_bool(a[1])) {
            set = as_bool(a[1]);
        }
        // Issue #1897: occurrence dirty bit flip under try_acquire Guard.
        return run_compile_dirty_under_guard(ev, [&]() -> EvalValue {
            return make_bool(ev.set_occurrence_dirty_fn_(node_id, set));
        });
    });
}

// Issue #909 compile part 38 (orig 3044-3215)
void CompilePrims::register_compile_p38(PrimRegistrar add, Evaluator& ev) {

    // Issue #240 / #1779: (compile:narrowing-dirty? node-id) — query
    // whether a workspace FlatAST node has the kOccurrenceDirty
    // bit set. Useful for agents / observability that want to
    // check narrowing staleness without invoking the full
    // post-mutation invariant check. Returns #t if the node is
    // dirty for narrowing, #f otherwise (also #f if the hook
    // is not installed or the node-id is out of range).
    //
    // Issue #1779: uses dedicated query_occurrence_dirty_fn_ (read-only).
    // Pre-#1779 peeked via set(true)+restore — racy under concurrent mark.
    //
    // This is the read-only counterpart to mark-narrowing-dirty!.
    add("compile:narrowing-dirty?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto node_id = static_cast<std::uint32_t>(as_int(a[0]));
        if (!ev.query_occurrence_dirty_fn_)
            return make_bool(false);
        return make_bool(ev.query_occurrence_dirty_fn_(node_id));
    });

    // (compile:occ-cache-stats) — Issue #340: returns
    // the per-cond-NodeId predicate_memo_ counters as
    // a 3-tuple (hits . misses . evictions). Stats-only;
    // does not modify the memo or affect type checking.
    // The memo itself is the pre-existing
    // predicate_memo_ (#281) — this primitive
    // surfaces the observability gap that the issue
    // body notes (the cache exists but its stats
    // aren't exposed to Aura).
    //
    // When (compile:occ-cache-stats) is called, the
    // values reflect the cumulative memo activity
    // since process start. Hit/miss ratio is the
    // primary signal: a high miss count in a small
    // workload suggests the PREDICATE_MEMO_MAX_ENTRIES
    // bound is too low (and the eviction counter is
    // firing).
    //

    // Issue #630: query:sv-verification-closedloop-stats-hash
    // — Agent-discoverable structured dashboard for the
    // verify-feedback → structured-mutate → re-emit → re-verify
    // closed loop. Specifically covers AC4 from the issue body.
    //
    // Fields (6):
    //   - feedback-to-mutate-cycles  lifetime feedback-mutate
    //                                hits (feedback_mutate_hits_total
    //                                CompilerMetrics counter, bumped
    //                                by #579 + #630 wire-up).
    //   - stable-ref-captures-in-sv  lifetime verify-tool stable-
    //                                ref captures inside the Guard
    //                                (get_verify_tool_stable_ref_
    //                                hits_total).
    //   - verification-dirty-propagations
    //                                lifetime verify-tool dirty
    //                                propagations
    //                                (get_verify_tool_dirty_
    //                                propagations_total).
    //   - reverify-success          lifetime successful re-emit
    //                                + re-verify closed-loop
    //                                completions
    //                                (verification_loop_success_
    //                                total).
    //   - rollback-on-partial        lifetime partial rollback
    //                                events (sv_emit_parse_fail_
    //                                total — every parse fail is
    //                                a partial-emit rollback).
    //   - ppa-savings-total          lifetime hardware-backend
    //                                ppa-savings bytes reclaimed
    //                                during re-emit (existing
    //                                #579 surface).
    //   - schema == 630              sentinel for Agent drift
    //                                detection (mirrors #618+
    //                                #620+#621+#622+#623+#624+
    //                                #625+#626 sentinels).
    //
    // Discovery before this PR (no duplication): the full
    // closed-loop logic + ALL the underlying counters already
    // exist in the C++ side via `eda:run-verification-feedback`
    // (#579), which bumps feedback_mutate_hits_total /
    // hardware_backend_hook_calls_total / commercial_reemits_total /
    // verification_loop_success_total / sv_emit_parse_fail_total /
    // ppa_savings_total / verify_tool_guard_captures_total_ /
    // verify_tool_dirty_propagations_total_ /
    // verify_tool_stable_ref_hits_total_ /
    // verify_tool_feedback_mutate_success_total_ (Evaluator).
    // The single NEW contribution is the structured primitive the
    // issue body AC4 lists by exact name.
    //
    // The remaining #630 AC1 + AC2 + AC3 work (eda:apply-verification-
    // feedback parser, Guard StableRef capture inside SV mutate
    // paths, hardware_backend hook on verification-related dirty)
    // is invasive C++ + hot-path EDA work that needs benchmarking
    // alongside the #579/#499 EDA scaffold — separate follow-up.
    ObservabilityPrims::register_stats_impl(
        "query:sv-verification-closedloop-stats-hash", [&ev](const auto&) -> EvalValue {
            const std::uint64_t feedback_cycles =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->feedback_mutate_hits_total.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t stable_ref = ev.get_verify_tool_stable_ref_hits_total();
            const std::uint64_t dirty_props = ev.get_verify_tool_dirty_propagations_total();
            const std::uint64_t reverify =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->verification_loop_success_total.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t rollback =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->sv_emit_parse_fail_total.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t ppa_savings =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->ppa_savings_total.load(std::memory_order_relaxed)
                    : 0;
            // Issue #1795: 7 keys need capacity ≥ 2× keys (load ≤ 0.5).
            // Pre-#1795 used create(8) → ~87% load / probe storms / silent
            // drop when an 8th field is added.
            constexpr std::size_t kClosedloopKeys = 7;
            std::size_t cap = 16;
            while (cap < kClosedloopKeys * 2)
                cap *= 2;
            auto* ht = FlatHashTable::create(cap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto& string_heap = ev.string_heap_mut();
            auto insert_kv = [&](const char* k_str, std::int64_t v) -> bool {
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
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return true;
                    }
                }
                return false; // table full — should not happen with cap ≥ 2×keys
            };
            if (!insert_kv("feedback-to-mutate-cycles",
                           static_cast<std::int64_t>(feedback_cycles)) ||
                !insert_kv("stable-ref-captures-in-sv", static_cast<std::int64_t>(stable_ref)) ||
                !insert_kv("verification-dirty-propagations",
                           static_cast<std::int64_t>(dirty_props)) ||
                !insert_kv("reverify-success", static_cast<std::int64_t>(reverify)) ||
                !insert_kv("rollback-on-partial", static_cast<std::int64_t>(rollback)) ||
                !insert_kv("ppa-savings-total", static_cast<std::int64_t>(ppa_savings)) ||
                !insert_kv("schema", 630)) {
                FlatHashTable::destroy(ht);
                return make_void();
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 compile part 39 (orig 3216-3279)
void CompilePrims::register_compile_p39(PrimRegistrar add, Evaluator& ev) {

    // Issue #340 / #1781: (compile:occ-cache-stats) returns the
    // lifetime predicate_memo_ counters as a 3-tuple
    // (hits . (misses . evictions)). Values come from
    // CompilerMetrics (mirrored from TypeChecker::stats_ after
    // each infer_flat / infer_flat_partial). Pre-#1781 this was
    // a hardcoded 0/0/0 stub — documentation drift for agents.
    ObservabilityPrims::register_stats_impl(
        "compile:occ-cache-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t hits = 0;
            std::uint64_t misses = 0;
            std::uint64_t evictions = 0;
            if (auto* m = static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())) {
                hits = m->predicate_memo_hits_total.load(std::memory_order_relaxed);
                misses = m->predicate_memo_misses_total.load(std::memory_order_relaxed);
                evictions = m->predicate_memo_evictions_total.load(std::memory_order_relaxed);
            }
            // Build (hits . (misses . evictions)) — a
            // pair-of-pairs (the simplest 3-tuple in
            // flat-eval).
            auto inner_idx = ev.pairs_.size();
            ev.pairs_.push_back({make_int(static_cast<std::int64_t>(misses)),
                                 make_int(static_cast<std::int64_t>(evictions))});
            auto outer_idx = ev.pairs_.size();
            ev.pairs_.push_back({make_int(static_cast<std::int64_t>(hits)), make_pair(inner_idx)});
            return make_pair(outer_idx);
        });

    // (compile:inline-pass-stats) — Issue #197: returns
    // a hash with the inliner's lifetime counters:
    //   :inlined          — process-wide total of the
    //                       pre-#197 constant-substitution path
    //   :branch-aware     — process-wide total of the
    //                       post-#197 branch-aware path
    //   :total            — sum of both
    // Returns all-zeros if no hook is installed (e.g.
    // unit-test Evaluator without a CompilerService).
    // The counters are static and process-wide, so the
    // primitive surfaces the cumulative inlining work
    // done by all InlinePass runs since process start.
    // Issue #388 / #1780: (*allow-macro-inline* #t/#f) — runtime
    // toggle for InlinePass macro-hygiene policy. Lets an Aura
    // workspace opt in to (or out of) inlining macro-introduced
    // code without recompiling.
    //
    // Issue #1780: policy is per-Evaluator (ev.inline_respect_macro_hygiene_),
    // not InlinePass process-wide static — concurrent CompilerServices /
    // fibers no longer clobber each other's toggle.
    //
    // Args: 1 (optional bool — defaults to true). Returns the
    // post-toggle flag value (1 if macro-introduced code is
    // now inlinable, 0 if not).
    add("*allow-macro-inline*", [&ev](const auto& a) -> EvalValue {
        bool enable = true;
        if (a.size() >= 1 && types::is_bool(a[0])) {
            enable = static_cast<bool>(types::as_bool(a[0]));
        }
        // enable=#t → respect hygiene off (allow inline macros).
        ev.set_inline_respect_macro_hygiene(!enable);
        const bool now_respects = ev.get_inline_respect_macro_hygiene();
        return make_int(static_cast<std::int64_t>(now_respects ? 0 : 1));
    });
}
// Issue #1787 / #1844: shared FNV-1a + open-addressing stats hash
// builder. Used by compile_05 (#1787) and compile_07 SEVA/aot/ir
// stats (#1844). Capacity scales with kv count (load factor ≤ 0.5,
// minimum 16 slots).
[[nodiscard]] EvalValue build_kv_hash(Evaluator& ev,
                                      std::span<const std::pair<std::string, EvalValue>> kv) {
    std::size_t ncap = 16;
    while (ncap < kv.size() * 2)
        ncap *= 2;
    auto* ht = FlatHashTable::create(ncap);
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
        const auto kidx = static_cast<std::uint64_t>(ev.push_string_heap(k));
        bool inserted = false;
        for (std::size_t at = 0; at < hcap; ++at) {
            auto idx = ((h >> 1) + at) & (hcap - 1);
            if (meta[idx] == 0xFF) {
                meta[idx] = fp;
                keys[idx] = make_string(kidx).val;
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

// Issue #909 compile part 40 (orig 3280-3340)
void CompilePrims::register_compile_p40(PrimRegistrar add, Evaluator& ev) {

    ObservabilityPrims::register_stats_impl(
        "compile:inline-pass-stats", [&ev](const auto&) -> EvalValue {
            std::int64_t inlined = 0;
            std::int64_t branch_aware = 0;
            if (ev.get_inline_stats_fn_) {
                // Issue #1784: unpack via uint32_t so each half is
                // always non-negative when widened to int64_t.
                // Direct static_cast<int64_t>(packed & 0xFFFFFFFF)
                // is well-defined for uint64_t, but going through
                // uint32_t makes the "no sign bit" contract explicit
                // for agents / future refactors that might cast
                // through int32_t by mistake.
                const std::uint64_t packed = ev.get_inline_stats_fn_();
                const std::uint32_t inlined_u32 = static_cast<std::uint32_t>(packed & 0xFFFFFFFFu);
                const std::uint32_t branch_aware_u32 = static_cast<std::uint32_t>(packed >> 32);
                inlined = static_cast<std::int64_t>(inlined_u32);
                branch_aware = static_cast<std::int64_t>(branch_aware_u32);
            }
            std::int64_t macro_skipped = 0;
            if (ev.get_macro_hygiene_skipped_fn_) {
                macro_skipped = static_cast<std::int64_t>(ev.get_macro_hygiene_skipped_fn_());
            }
            std::int64_t total = inlined + branch_aware;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"inlined", make_int(inlined)},
                {"branch-aware", make_int(branch_aware)},
                {"macro-hygiene-skipped", make_int(macro_skipped)},
                {"total", make_int(total)},
            };
            return build_kv_hash(ev, kv);
        });
}

// Issue #909 compile part 41 (orig 3341-3412)
void CompilePrims::register_compile_p41(PrimRegistrar add, Evaluator& ev) {

    // (concurrency:stats) — Issue #189 (P0): concurrency safety
    // observability. Reports the current defuse_version_ (the
    // monotonic mutation counter bumped on every mutate:*), the
    // total number of mutations ever applied to this evaluator
    // (the issue's "mutation count" stat), the per-join wait
    // snapshot, and the MutationBoundaryGuard stack depth.
    //
    // The hash has 4 keys:
    //   defuse-version:    uint64 (acquire-loaded for safety)
    //   total-mutations:   uint64 (lifetime count)
    //   boundary-depth:    int (current MutationBoundaryGuard stack size)
    //   at-wait-version:   uint64 (per-join snapshot, 0 if no active wait)
    //
    // Use (concurrency:stats) to:
    //   - verify a (mutate:*) actually bumped the version
    //   - count how many mutations a workload has applied
    //   - debug concurrent fiber contention via boundary-depth
    ObservabilityPrims::register_stats_impl("concurrency:stats", [&ev](const auto&) -> EvalValue {
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"defuse-version", make_int(static_cast<std::int64_t>(ev.defuse_version_snapshot()))},
            {"total-mutations", make_int(static_cast<std::int64_t>(ev.total_mutations()))},
            {"boundary-depth", make_int(static_cast<std::int64_t>(ev.mutation_boundary_depth()))},
            {"at-wait-version", make_int(static_cast<std::int64_t>(ev.defuse_version_at_wait_))},
            {"mutation-yield-count",
             make_int(static_cast<std::int64_t>(ev.mutation_yield_count()))},
            {"compaction-paused-by-boundary",
             make_int(static_cast<std::int64_t>(ev.compaction_paused_by_boundary()))},
            {"cross-fiber-rollback-count",
             make_int(static_cast<std::int64_t>(ev.cross_fiber_rollback_count()))},
        };
        return build_kv_hash(ev, kv);
    });
}

// Issue #909 compile part 42 (orig 3413-3482)
void CompilePrims::register_compile_p42(PrimRegistrar add, Evaluator& ev) {

    // (concurrency:version-snapshot) — Issue #189: capture the
    // current defuse_version_ and return it as an int. Use with
    // (concurrency:version-current? snap) to detect concurrent
    // mutations between two points in the program.
    ObservabilityPrims::register_stats_impl(
        "concurrency:version-snapshot", [&ev](const auto&) -> EvalValue {
            return make_int(static_cast<std::int64_t>(ev.defuse_version_snapshot()));
        });

    // (concurrency:version-current? snap) — Issue #189: returns
    // #t if the defuse_version_ has not changed since `snap` was
    // captured. #f if a mutation has happened (and AST/cells/pairs
    // may be stale).
    ObservabilityPrims::register_stats_impl(
        "concurrency:version-current?", [&ev](const auto& a) -> EvalValue {
            if (a.empty() || !is_int(a[0]))
                return make_bool(false);
            auto snap = static_cast<std::uint64_t>(as_int(a[0]));
            return make_bool(ev.is_version_current(snap));
        });

    // (syntax-marker node-id) — Issue #190: return the SyntaxMarker
    // value of a node (0=User, 1=MacroIntroduced, 2=BoolLiteral).
    // Used for EDSL filter queries (e.g., "find all macro-introduced
    // nodes") and for diagnostic output ("why did mutate:rebind
    // refuse to edit this node?").
    add("syntax-marker", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        if (!ev.workspace_flat_)
            return make_int(0);
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (id >= ev.workspace_flat_->size())
            return make_int(0);
        // Issue #1783: shared metadata lock vs concurrent set-marker.
        auto rlock = ev.workspace_flat_->try_acquire_metadata_reader_lock();
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->marker(id)));
    });

    // (syntax:set-marker node-id marker) — Issue #366: set the
    // SyntaxMarker value of a node. Returns true on success.
    // marker is an integer (0=User, 1=MacroIntroduced,
    // 2=BoolLiteral). Used by EDSL transformers and auditability
    // tooling to correct marker drift after a manual mutation
    // sequence. The primitive does NOT propagate the marker to
    // children — use syntax:propagate-marker for that.
    add("syntax:set-marker", [&ev](const auto& a) -> EvalValue {
        // Issue #1002: removed dead `bool ok` (error paths return merr
        // immediately; ok was never read).
        if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1])) {
            return ev.make_merr("bad-arg", "usage: (syntax:set-marker node-id marker)");
        }
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto marker_val = static_cast<int>(as_int(a[1]));
        if (!ev.workspace_flat_) {
            return ev.make_merr("no-workspace", "no active workspace");
        }
        if (id >= ev.workspace_flat_->size()) {
            return ev.make_merr("out-of-range", "node id " + std::to_string(id) + " >= flat size");
        }
        if (marker_val < 0 || marker_val > 2) {
            return ev.make_merr("bad-arg",
                                "marker must be 0 (User), 1 (MacroIntroduced), or 2 (BoolLiteral)");
        }
        // No MutationBoundaryGuard — metadata-only (no generation
        // bump). Issue #1783: exclusive metadata_mtx_ serializes
        // cross-fiber marker_column writes without invalidating
        // StableNodeRef / marker-query caches.
        auto wlock = ev.workspace_flat_->begin_metadata_mutation();
        ev.workspace_flat_->set_marker(id, static_cast<aura::ast::SyntaxMarker>(marker_val));
        return make_bool(true);
    });
}

// Issue #909 compile part 43 (orig 3483-3546)
void CompilePrims::register_compile_p43(PrimRegistrar add, Evaluator& ev) {

    // (syntax:propagate-marker node-id marker) — Issue #366:
    // set the marker on a node AND recursively on all
    // descendants. Returns the count of nodes updated. Used by
    // EDSL transformers to re-stamp a macro-introduced subtree
    // after a structural mutation (insert-child, replace-subtree)
    // that may have lost the original marker on some children.
    // The primitive does NOT bump defuse_version_ — marker
    // changes are observational metadata, not workspace state.
    //
    // Issue #1782: FlatAST children can form cycles (DAG / self-
    // mutate). Dense seen[] + kMaxVisit abort (parity #1679 /
    // #1682) prevents unbounded stack growth on cycles.
    add("syntax:propagate-marker", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1])) {
            return make_int(0);
        }
        auto root = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto marker_val = static_cast<int>(as_int(a[1]));
        if (!ev.workspace_flat_)
            return make_int(0);
        if (root == aura::ast::NULL_NODE || root >= ev.workspace_flat_->size())
            return make_int(0);
        if (marker_val < 0 || marker_val > 2)
            return make_int(0);
        auto& flat = *ev.workspace_flat_;
        // Iterative DFS with visited set — metadata-only.
        // Issue #1783: hold exclusive metadata_mtx_ for the whole
        // walk so concurrent set-marker / get-marker cannot tear.
        auto wlock = flat.begin_metadata_mutation();
        std::int64_t count = 0;
        std::vector<aura::ast::NodeId> stack;
        std::vector<std::uint8_t> seen(flat.size(), 0);
        stack.push_back(root);
        seen[static_cast<std::size_t>(root)] = 1;
        std::size_t visited = 1;
        const std::size_t kMaxVisit = flat.size();
        while (!stack.empty()) {
            auto cur = stack.back();
            stack.pop_back();
            if (cur == aura::ast::NULL_NODE || cur >= flat.size())
                continue;
            flat.set_marker(cur, static_cast<aura::ast::SyntaxMarker>(marker_val));
            ++count;
            const auto& children = flat.children(cur);
            for (std::uint32_t ci = 0; ci < children.size(); ++ci) {
                const auto c = children[ci];
                if (c == aura::ast::NULL_NODE || c >= flat.size())
                    continue;
                const auto cix = static_cast<std::size_t>(c);
                if (seen[cix])
                    continue; // cycle edge or shared DAG child
                seen[cix] = 1;
                ++visited;
                if (visited > kMaxVisit)
                    return make_int(count); // defensive abort
                stack.push_back(c);
            }
        }
        return make_int(count);
    });

    // (syntax:set-provenance node-id prov-id) — Issue #367:
    // set the per-node provenance id. The prov-id is a
    // workspace-scoped identifier that AI agents can use to
    // trace "this node came from macro X during expansion Y".
    // The actual provenance data (macro_def_id, expansion_id,
    // mutation_id) lives in a side-table that the host can
    // populate out-of-band; this primitive only stores the
    // index. 0 = no provenance recorded.
    add("syntax:set-provenance", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1]))
            return make_bool(false);
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto prov = static_cast<std::uint32_t>(as_int(a[1]));
        if (!ev.workspace_flat_)
            return make_bool(false);
        if (id >= ev.workspace_flat_->size())
            return make_bool(false);
        // Issue #1783: exclusive metadata_mtx_ for provenance column.
        auto wlock = ev.workspace_flat_->begin_metadata_mutation();
        ev.workspace_flat_->set_provenance(id, prov);
        return make_bool(true);
    });
}

// Issue #909 compile part 44 (orig 3547-3630)
void CompilePrims::register_compile_p44(PrimRegistrar add, Evaluator& ev) {

    // (syntax:get-provenance node-id) — Issue #367: return
    // the per-node provenance id (0 if unset). The host can
    // look up the actual macro_def_id / expansion_id /
    // mutation_id via its own side-table.
    add("syntax:get-provenance", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        if (!ev.workspace_flat_)
            return make_int(0);
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (id >= ev.workspace_flat_->size())
            return make_int(0);
        // Issue #1783: shared metadata lock vs concurrent set-provenance.
        auto rlock = ev.workspace_flat_->try_acquire_metadata_reader_lock();
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->provenance(id)));
    });

    // (syntax-marker-counts) — Issue #190: aggregate count of
    // each SyntaxMarker value across the workspace. Hash with
    // 3 integer fields: user, macro-introduced, bool-literal,
    // plus total-nodes. Useful for dashboards ("how much of the
    // workspace is macro-introduced code?") and for asserting
    // hygiene invariants in tests.
    ObservabilityPrims::register_stats_impl(
        "syntax-marker-counts", [&ev](const auto&) -> EvalValue {
            if (!ev.workspace_flat_)
                return make_void();
            std::size_t user = 0, macro = 0, bool_lit = 0, total = 0;
            // Issue #1783: shared lock for full-column scan.
            auto rlock = ev.workspace_flat_->try_acquire_metadata_reader_lock();
            const auto& markers = ev.workspace_flat_->marker_column();
            for (std::size_t i = 0; i < markers.size(); ++i) {
                ++total;
                auto m = static_cast<int>(markers[i]);
                if (m == 0)
                    ++user;
                else if (m == 1)
                    ++macro;
                else if (m == 2)
                    ++bool_lit;
            }
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"user", make_int(static_cast<std::int64_t>(user))},
                {"macro-introduced", make_int(static_cast<std::int64_t>(macro))},
                {"bool-literal", make_int(static_cast<std::int64_t>(bool_lit))},
                {"total-nodes", make_int(static_cast<std::int64_t>(total))},
            };
            return build_kv_hash(ev, kv);
        });
}

// Issue #909 compile part 45 (orig 3631-3794)
void CompilePrims::register_compile_p45(PrimRegistrar add, Evaluator& ev) {

    // (compile:per-symbol-dirty-stats sym) — Issue #410: per-symbol
    // dirty observability. Returns a hash with 4 fields:
    //   - per-symbol-affected-count: number of Variable nodes in
    //     the flat whose sym_id matches `sym` (the per-symbol
    //     affected set)
    //   - ancestor-affected-count: number of nodes in the
    //     ancestor chain of the def node (the legacy
    //     mark_dirty_upward set; -1 if the def node is not
    //     found in the flat — conservative unknown)
    //   - reduction-ratio-bp: per-symbol / ancestor * ::aura::compiler::kBasisPointScale in
    //     basis points. Higher = bigger savings if #410 Phase 2
    //     wires affected_subtree_for_symbol into infer_flat_partial.
    //     10000 = per-symbol set is the same size as ancestor set
    //     (no savings). 0 = per-symbol set is empty (no uses).
    //   - lookup-count: cumulative per-symbol-dirty lookups
    //     (lifetime total from metrics_).
    //
    // ACs:
    //   AC1: counter starts at 0
    //   AC2: primitive returns hash with 4 keys
    //   AC3: per-symbol < ancestor-affected on a body with 5+ bindings
    //   AC4: counter increments after a primitive call
    //   AC5: unbound sym returns sensible (0,0,0,0) values
    //   AC6: reduction-ratio-bp matches manual calculation
    // Multi-arg (sym name required) — must stay public add();
    // stats:get/engine:metrics cannot pass the symbol argument.
    add("compile:per-symbol-dirty-stats", [&ev](const auto& a) -> EvalValue {
        // Issue #1787: build_kv_hash shared helper.
        // Resolve sym name → SymId. Use the workspace pool +
        // string heap (same pattern as query:def-use).
        if (a.empty() || !is_string(a[0]))
            return ev.make_merr("bad-arg", "usage: (compile:per-symbol-dirty-stats sym-name)");
        auto sym_idx = as_string_idx(a[0]);
        std::string sym_name;
        if (sym_idx < ev.string_heap_.size()) {
            sym_name = ev.string_heap_[sym_idx];
        } else {
            return ev.make_merr("bad-arg", "symbol name string index out of range");
        }
        // Issue #1785: hold workspace_mtx_ for pool lookup + FlatAST
        // walks so concurrent mutate / intern cannot race. Prefer
        // find_by_name (read-only) over intern(write) — unbound
        // names stay INVALID_SYM and return zeroed stats (AC5)
        // without mutating the pool hash table.
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        aura::ast::SymId target_sym = aura::ast::INVALID_SYM;
        if (ev.workspace_flat_ && ev.workspace_pool_) {
            if (auto found = ev.workspace_pool_->find_by_name(sym_name))
                target_sym = *found;
        }
        // Compute per-symbol affected set (O(n) walk).
        std::vector<aura::ast::NodeId> per_symbol_affected;
        if (ev.workspace_flat_ && target_sym != aura::ast::INVALID_SYM) {
            per_symbol_affected = affected_subtree_for_symbol(*ev.workspace_flat_, target_sym);
        }
        // Compute ancestor-affected count: walk the parent_ chain
        // from the def node (the Define/Let/LetRec that binds
        // `target_sym`). If no def node is found, report -1 (unknown).
        std::int64_t ancestor_affected = -1;
        if (ev.workspace_flat_ && target_sym != aura::ast::INVALID_SYM) {
            aura::ast::NodeId def_node = aura::ast::NULL_NODE;
            const std::size_t n = ev.workspace_flat_->size();
            for (std::size_t i = 0; i < n; ++i) {
                auto v = ev.workspace_flat_->get(static_cast<aura::ast::NodeId>(i));
                if ((v.tag == aura::ast::NodeTag::Define || v.tag == aura::ast::NodeTag::Let ||
                     v.tag == aura::ast::NodeTag::LetRec) &&
                    v.sym_id == target_sym) {
                    def_node = static_cast<aura::ast::NodeId>(i);
                    break;
                }
            }
            if (def_node != aura::ast::NULL_NODE) {
                // Walk up the parent chain. mark_dirty_upward would
                // also include descendants of each ancestor; we
                // report the chain length only (the conservative
                // ancestor-only count, which is what the per-symbol
                // set needs to beat to justify the new path).
                //
                // Phase A1 migration: now uses
                // aura::compiler::walk_ancestors<Id, C, V> from
                // aura.compiler.query. The walk starts from
                // parent_of(def_node) to match the original semantics
                // (count ancestors of def_node, excluding def_node
                // itself). The size()-bounded safety cap is preserved
                // inside the visitor via early-return.
                //
                // Issue #1786: dense seen[] stops parent_of cycles so
                // each ancestor is counted at most once (max_count alone
                // still overcounts cycles shorter than flat.size()).
                std::int64_t chain_len = 0;
                auto start = ev.workspace_flat_->parent_of(def_node);
                const auto max_count = static_cast<std::size_t>(ev.workspace_flat_->size());
                if (start != aura::ast::NULL_NODE) {
                    std::vector<std::uint8_t> seen(ev.workspace_flat_->size(), 0);
                    chain_len =
                        static_cast<std::int64_t>(aura::compiler::walk_ancestors<std::uint32_t>(
                            *ev.workspace_flat_, start,
                            [&seen, &chain_len, max_count](aura::ast::NodeId cur) -> bool {
                                if (static_cast<std::size_t>(chain_len) >= max_count)
                                    return false; // safety cap
                                if (cur >= seen.size())
                                    return false;
                                const auto ci = static_cast<std::size_t>(cur);
                                if (seen[ci])
                                    return false; // cycle — stop, do not re-count
                                seen[ci] = 1;
                                ++chain_len;
                                return true;
                            }));
                }
                ancestor_affected = chain_len;
            }
        }
        rlock.unlock(); // done with pool + flat; metrics are atomics
        // Bump metrics_.
        std::uint64_t lookup_count = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            m->per_symbol_dirty_lookups_total.fetch_add(1, std::memory_order_relaxed);
            m->per_symbol_dirty_uses_total.fetch_add(
                static_cast<std::uint64_t>(per_symbol_affected.size()), std::memory_order_relaxed);
            lookup_count = m->per_symbol_dirty_lookups_total.load(std::memory_order_relaxed);
        }
        // reduction-ratio-bp = per_symbol / ancestor * ::aura::compiler::kBasisPointScale.
        // Cap at 10000 (per_symbol can't exceed ancestor in
        // practice, but defensive). Use 0 when ancestor is 0/-
        std::int64_t ratio_bp = 0;
        if (ancestor_affected > 0 && !per_symbol_affected.empty()) {
            const auto num = static_cast<std::int64_t>(per_symbol_affected.size());
            ratio_bp = (num * ::aura::compiler::kBasisPointScale) / ancestor_affected;
            if (ratio_bp > 10000)
                ratio_bp = 10000;
        }
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"per-symbol-affected-count",
             make_int(static_cast<std::int64_t>(per_symbol_affected.size()))},
            {"ancestor-affected-count", make_int(ancestor_affected)},
            {"reduction-ratio-bp", make_int(ratio_bp)},
            {"lookup-count", make_int(static_cast<std::int64_t>(lookup_count))},
        };
        return build_kv_hash(ev, kv);
    });
}

// Issue #909 compile part 46 (orig 3795-3871)
void CompilePrims::register_compile_p46(PrimRegistrar add, Evaluator& ev) {

    // (compile:incremental-typecheck-stats) — Issue #411: post-
    // mutation auto-incremental typecheck observability. Returns
    // a hash with 3 fields:
    //   - auto-invocations-total: lifetime total number of
    //     typed_mutate success paths that triggered an automatic
    //     infer_flat_partial call. 0 in Lazy/Disabled modes.
    //   - re-inferred-total: cumulative count of nodes re-
    //     inferred across all auto-invocations.
    //   - avg-re-inferred-bp: derived average (re_inferred *
    //     10000 / max(auto_invocations, 1)) in basis points.
    //     Higher = more nodes re-inferred per mutation on
    //     average. The follow-up per-symbol wiring (Issue #410
    //     Phase 2/2) will reduce this metric.
    //
    // Mirrors the 2 lifetime counters on CompilerMetrics plus
    // the derived metric on CompilerSnapshot (Issue #1787:
    // build_kv_hash shared helper).
    ObservabilityPrims::register_stats_impl(
        "compile:incremental-typecheck-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t auto_invocations = 0;
            std::uint64_t re_inferred = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                auto_invocations =
                    m->incremental_typecheck_auto_invocations_total.load(std::memory_order_relaxed);
                re_inferred =
                    m->incremental_typecheck_re_inferred_total.load(std::memory_order_relaxed);
            }
            const std::uint64_t avg_bp =
                (auto_invocations > 0) ? (re_inferred * 10000u) / auto_invocations : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"auto-invocations-total", make_int(static_cast<std::int64_t>(auto_invocations))},
                {"re-inferred-total", make_int(static_cast<std::int64_t>(re_inferred))},
                {"avg-re-inferred-bp", make_int(static_cast<std::int64_t>(avg_bp))},
            };
            return build_kv_hash(ev, kv);
        });
}

// Issue #909 compile part 47 (orig 3872-3949)
void CompilePrims::register_compile_p47(PrimRegistrar add, Evaluator& ev) {

    // (compile:type-cache-stats) — Issue #412: observability
    // for the type cache generation-counter check. Returns a
    // hash with 4 fields:
    //   - cache-hits-total: lifetime total cache_hits (post-
    //     #412, includes the gen_saved rescues — they're
    //     counted as hits now, not stale)
    //   - cache-misses-total: lifetime total cache_misses
    //   - stale-cache-total: lifetime total stale_cache (post-
    //     #412, only true staleness — false positives
    //     rescued by the gen check no longer count here)
    //   - gen-saved-total: lifetime total cache hits rescued
    //     by the gen check (would have been stale_cache
    //     pre-#412)
    //   - gen-saved-ratio-bp: derived ratio (gen_saved /
    //     (stale + gen_saved) * ::aura::compiler::kBasisPointScale, basis points). 0
    //     when neither counter has been bumped. The key AC
    //     for #412 — higher = more false-positive stale
    //     rejections eliminated.
    ObservabilityPrims::register_stats_impl(
        "compile:type-cache-stats", [&ev](const auto&) -> EvalValue {
            // Issue #1797: single logical snapshot of the 4 counters
            // so gen-saved-ratio-bp is not mixed across concurrent
            // typechecks (pre-#1797: 4 independent relaxed loads).
            TypeCacheStatsSnapshot snap;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                snap = m->snapshot_type_cache_stats();
            }
            const std::uint64_t gen_total = snap.stale + snap.gen_saved;
            const std::uint64_t ratio_bp =
                (gen_total > 0) ? (snap.gen_saved * 10000u) / gen_total : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"cache-hits-total", make_int(static_cast<std::int64_t>(snap.hits))},
                {"cache-misses-total", make_int(static_cast<std::int64_t>(snap.misses))},
                {"stale-cache-total", make_int(static_cast<std::int64_t>(snap.stale))},
                {"gen-saved-total", make_int(static_cast<std::int64_t>(snap.gen_saved))},
                {"gen-saved-ratio-bp", make_int(static_cast<std::int64_t>(ratio_bp))},
            };
            return build_kv_hash(ev, kv);
        });
}
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
    // Issue #1845 / #1897: wrap tracker mutation in try_acquire Guard
    // + try/catch. Pre-#1845 called add_caller raw — throw mid
    // map/vector growth left the tracker partially consistent
    // with no panic-checkpoint restore. compiler_service_ is
    // non-owning (#1839 ownership contract); concurrent free
    // mid-eval is unsupported. #1897: prefer try_acquire over
    // deprecated RAII ctor (typed ResourceQuotaExceeded).
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
        return run_under_mutation_guard(
            ev,
            [&]() -> EvalValue {
                svc->per_defuse_index_tracker().add_caller(DefUseIndex{idx_name},
                                                           Caller{caller_node_id});
                return make_int(static_cast<std::int64_t>(
                    svc->per_defuse_index_tracker().size_for_index(DefUseIndex{idx_name})));
            },
            make_int(-1));
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
            // Issue #1856: try_snapshot for records-total.
            auto snap_opt = svc->try_snapshot();
            if (!snap_opt)
                return make_void();
            std::uint64_t trace_size = 0;
            if (auto* ws = ev.workspace_flat()) {
                trace_size = static_cast<std::uint64_t>(ws->invalidation_trace_size());
            }
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"records-total",
                 make_int(static_cast<std::int64_t>(snap_opt->invalidation_trace_records_total))},
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
    // Issue #1847 / #1897: wrap bump_generation_subtree in
    // try_acquire Guard + try/catch. Pre-#1847 mutated
    // subtree_gen_ / generation_ raw — a throw mid ancestor
    // walk left counters partially consistent with no panic
    // checkpoint restore. #1897: try_acquire + shared helper.
    add("compile:subtree-bump", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return ev.make_merr("bad-arg", "usage: (compile:subtree-bump subtree-root-id)");
        const auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (!ev.workspace_flat_)
            return make_int(0);
        return run_under_mutation_guard(
            ev,
            [&]() -> EvalValue {
                const auto before = ev.workspace_flat_->subtree_bump_count();
                ev.workspace_flat_->bump_generation_subtree(id);
                const auto after = ev.workspace_flat_->subtree_bump_count();
                return make_int(after > before ? 1 : 0);
            },
            make_int(-1));
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
    // Issue #1850 / #1897: wrap register_type / register_hw_bitvec in
    // try_acquire Guard + try/catch. Pre-#1850 mutated the
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
        // Issue #1837 / #1850 / #1898: ensure + pin type_registry_ so a
        // concurrent set_type_registry rebind is soft-failed post-mutation.
        auto* reg_ptr = static_cast<aura::core::TypeRegistry*>(ev.ensure_type_registry());
        if (!reg_ptr)
            return ev.make_merr("no-registry", "type registry unavailable");
        auto reg_pin = ev.pin_type_registry();
        if (!reg_pin || reg_pin.ptr != reg_ptr)
            return ev.make_merr("no-registry", "type registry pin failed");
        auto& reg = *reg_ptr;
        const auto width = static_cast<std::uint32_t>(as_int(a[1]));
        const bool is_signed = as_int(a[2]) != 0;
        return run_under_mutation_guard(
            ev,
            [&]() -> EvalValue {
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
                if (!ev.type_registry_pin_valid(reg_pin)) {
                    if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                        m->raw_pointer_uaf_prevented_total.fetch_add(1, std::memory_order_relaxed);
                        m->type_registry_pin_reject_total.fetch_add(1, std::memory_order_relaxed);
                    }
                    return make_int(-1);
                }
                return make_int(1);
            },
            make_int(-1));
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
// Issue #1844 / #1787: shared FNV-1a stats hash builder
// (defined in evaluator_primitives_compile_05.cpp). Capacity
// scales with kv count (load factor ≤ 0.5, minimum 16 slots).
[[nodiscard]] EvalValue build_kv_hash(Evaluator& ev,
                                      std::span<const std::pair<std::string, EvalValue>> kv);

// Issue #909 compile part 56 (orig 4631-4698)
void CompilePrims::register_compile_p56(PrimRegistrar add, Evaluator& ev) {

    // ═══════════════════════════════════════════════════════════
    // Issue #309: hardware lossy-coercion diagnostics.
    //
    // Two new primitives extend the BitVector foundation from
    // #308 with hw-aware coercion analysis:
    //
    //   (compile:hw-coercion-lossy? <from-name> <to-name>)
    //     Returns 1 iff coercing FROM `from-name` TO `to-name`
    //     would LOSE information. The canonical rule: lossy iff
    //     from is wider than to (narrowing drops high bits). Same
    //     width or widening is lossless. If either type isn't
    //     registered as a hw bitvec, returns 0 (not applicable).
    //
    //   (compile:hw-coercion-warning <from-name> <to-name>)
    //     Returns a human-readable warning string when the
    //     coercion is lossy, or "" (empty string) when it's
    //     lossless / not applicable. The string format is:
    //       "lossy coercion: <from> (W<from-w> signed) -> <to> (W<to-w> signed) drops <n> bits"
    //     E.g.: "lossy coercion: uint16_t (W16 unsigned) -> uint8_t (W8 unsigned) drops 8 bits"
    //
    // Why these primitives:
    //   - Issue #309 AC2: "New warning emitted for lossy bit
    //     coercion in hardware context." Today the user code
    //     calls these primitives at the coercion site to
    //     emit the warning. The automatic type-checker
    //     warning (emitted during infer_flat) is a follow-up.
    //   - Issue #309 AC1: "Blame correctly tracks across a
    //     typed-mutate that changes a coercion site in
    //     hardware code." The BlameInfo (Issue #342) is
    //     already attached to type-checker diagnostics via
    //     with_blame() — see type_checker_impl.cpp's
    //     narrowing path. The hw-aware extension of
    //     BlameInfo (e.g. hw_region field) is a follow-up.
    //   - Future #309 follow-ups: integrate the lossy check
    //     into InferenceEngine's subtyping path (so the
    //     warning is automatic), extend BlameInfo with
    //     hw_region (Synth | Sim | Unset), and richer
    //     hardware-specific messages (e.g. "may introduce
    //     latch" for incomplete case + width-loss).
    ObservabilityPrims::register_stats_impl(
        "compile:hw-coercion-lossy?", [&ev](const auto& a) -> EvalValue {
            if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
                return ev.make_merr("bad-arg",
                                    "usage: (compile:hw-coercion-lossy? from-name to-name)");
            auto from_sx = as_string_idx(a[0]);
            auto to_sx = as_string_idx(a[1]);
            std::string from_name, to_name;
            if (from_sx < ev.string_heap_.size())
                from_name = ev.string_heap_[from_sx];
            if (to_sx < ev.string_heap_.size())
                to_name = ev.string_heap_[to_sx];
            if (!ev.type_registry_)
                return make_int(0);
            auto& reg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
            auto from_tid = reg.lookup_type(from_name);
            auto to_tid = reg.lookup_type(to_name);
            if (!from_tid.valid() || !to_tid.valid())
                return make_int(0);
            auto* from_bv = reg.hw_bitvec_of(from_tid);
            auto* to_bv = reg.hw_bitvec_of(to_tid);
            if (!from_bv || !to_bv)
                return make_int(0); // not a hw coercion
            // Lossy iff FROM is wider than TO (narrowing drops bits).
            // Same width (regardless of signedness) is lossless:
            // reinterpreting signed↔unsigned doesn't lose bits.
            // Widening is lossless (zero- or sign-extension).
            const bool lossy = from_bv->width > to_bv->width;
            return make_int(lossy ? 1 : 0);
        });
}

// Issue #909 compile part 57 (orig 4699-4773)
void CompilePrims::register_compile_p57(PrimRegistrar add, Evaluator& ev) {

    ObservabilityPrims::register_stats_impl(
        "compile:hw-coercion-warning", [&ev](const auto& a) -> EvalValue {
            // Issue #1050: empty-string sentinel must be a real heap entry,
            // never make_string(heap.size()) which is OOB.
            auto empty_str = [&ev]() -> EvalValue {
                return make_string(static_cast<std::uint64_t>(ev.push_string_heap("")));
            };
            if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
                return ev.make_merr("bad-arg",
                                    "usage: (compile:hw-coercion-warning from-name to-name)");
            auto from_sx = as_string_idx(a[0]);
            auto to_sx = as_string_idx(a[1]);
            std::string from_name, to_name;
            if (from_sx < ev.string_heap_.size())
                from_name = ev.string_heap_[from_sx];
            if (to_sx < ev.string_heap_.size())
                to_name = ev.string_heap_[to_sx];
            if (!ev.type_registry_)
                return empty_str();
            auto& reg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
            auto from_tid = reg.lookup_type(from_name);
            auto to_tid = reg.lookup_type(to_name);
            if (!from_tid.valid() || !to_tid.valid())
                return empty_str();
            auto* from_bv = reg.hw_bitvec_of(from_tid);
            auto* to_bv = reg.hw_bitvec_of(to_tid);
            if (!from_bv || !to_bv)
                return empty_str();
            if (from_bv->width <= to_bv->width)
                return empty_str(); // lossless — no warning
            const std::uint32_t dropped = from_bv->width - to_bv->width;
            const std::string from_str = from_bv->is_signed ? "signed" : "unsigned";
            const std::string to_str = to_bv->is_signed ? "signed" : "unsigned";
            const std::string msg = "lossy coercion: " + from_name + " (W" +
                                    std::to_string(from_bv->width) + " " + from_str + ") -> " +
                                    to_name + " (W" + std::to_string(to_bv->width) + " " + to_str +
                                    ") drops " + std::to_string(dropped) + " bits";
            return make_string(static_cast<std::uint64_t>(ev.push_string_heap(msg)));
        });

    // ── Issue #373: MacroIntroduced hygiene guard primitives ──
    //
    // Three primitives that surface the hygiene guard added by
    // #373 piece 2 (mutate guards):
    //
    //   (hygiene:protected? node-id)
    //     Returns #t if the node has marker == MacroIntroduced
    //     (i.e. was produced by clone_macro_body from a hygienic
    //     macro expansion). #f otherwise (including when the
    //     workspace or node id is invalid). Same marker column
    //     that query:by-marker / query:macro-introduced reads.
    //
    //   (hygiene:allow-macro-mutate?) — read the global flag
    //     (default #f). Mirrors the C++ side's allow_macro_mutate_
    //     flag on Evaluator.
    //
    //   (hygiene:set-allow-macro-mutate! bool) — set the flag.
    //     When #t, mutate:* operations on MacroIntroduced nodes
    //     proceed without the "hygiene-protected" pre-check
    //     rejection. The flag is per-Evaluator (process-local);
    //     setting it does not affect other Compilers in the same
    //     process.
    //
    // These three primitives don't touch the mutate:* path —
    // they're for EDSL code / tests that need to read the
    // protected state or opt-in globally. Per-call opt-out
    // without changing the flag is the :allow-macro? #t kwarg
    // on each mutate:* primitive.
    add("hygiene:protected?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        if (!ev.workspace_flat_)
            return make_bool(false);
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (id >= ev.workspace_flat_->size())
            return make_bool(false);
        // Issue #1838 / #1783: shared metadata lock vs concurrent
        // syntax:set-marker / macro expansion writes to marker column.
        auto rlock = ev.workspace_flat_->try_acquire_metadata_reader_lock();
        return make_bool(ev.workspace_flat_->is_macro_introduced(id));
    });
}

// Issue #909 compile part 58 (orig 4774-4946)
void CompilePrims::register_compile_p58(PrimRegistrar add, Evaluator& ev) {

    add("hygiene:allow-macro-mutate?",
        [&ev](const auto&) -> EvalValue { return make_bool(ev.get_allow_macro_mutate()); });

    add("hygiene:set-allow-macro-mutate!", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_bool(a[0])) {
            return ev.make_merr("bad-arg", "usage: (hygiene:set-allow-macro-mutate! bool)");
        }
        ev.set_allow_macro_mutate(as_bool(a[0]));
        return make_void();
    });

    // ── Issue #375: IR encoding observability primitive ──
    //
    // `(compile:ir-stats)` — returns a hash describing the
    // current IRModule's encoding characteristics. The point
    // of #375 is to identify how much padding + unused
    // operand-space the AoS IRInstruction layout wastes, and
    // to project a compact encoding size so we can decide
    // whether the ≥30% size-reduction AC is achievable.
    //
    // Fields returned:
    //   - total-instructions       — total IRInstruction count
    //                                 across all functions in the
    //                                 last compiled module.
    //   - total-functions          — function count.
    //   - total-blocks             — basic block count.
    //   - avg-instructions-per-block — float (total-instr / blocks).
    //   - opcode-histogram         — hash {opcode-name -> count},
    //                                 so we know which opcodes
    //                                 dominate the hot path.
    //   - operand-count-distribution — hash {0..4 -> count} of how
    //                                 many instructions actually
    //                                 use 0/1/2/3/4 operand slots.
    //   - avg-operands-used-x100  — avg operands * 100 (integer
    //                                 to keep the hash type-safe;
    //                                 divide by 100 for the float).
    //   - aos-bytes-total         — total bytes assuming 40 bytes
    //                                 per IRInstruction (sizeof
    //                                 layout: 1 opcode + 16 ops +
    //                                 4 + 4 + 4 + 1 + 3 pad + 4 +
    //                                 4 + 1 = 40).
    //   - unused-operand-bytes-total — bytes wasted on unused
    //                                 operand slots: (4 - avg_ops) *
    //                                 4 * total_instr.
    //   - padding-bytes-total     — bytes wasted on struct
    //                                 alignment: 3 bytes per
    //                                 instruction (between
    //                                 linear_ownership_state and
    //                                 adt_variant_id).
    //   - compact-bytes-projection — projected bytes under a
    //                                 variable-length compact
    //                                 encoding: 2 bytes header
    //                                 (opcode 8 bits + operand
    //                                 count 4 bits + reserved 4
    //                                 bits) + 4 bytes per used
    //                                 operand, rounded up to
    //                                 4-byte alignment. Hot-path
    //                                 friendly, no per-instruction
    //                                 metadata sidecar.
    //   - compact-ratio-bp        — compact_bytes / aos_bytes in
    //                                 basis points (0-10000). 3000
    //                                 bp = compact is 30% of aos.
    //                                 The #375 AC is "≥30% size
    //                                 reduction" so a ratio ≤ 7000
    //                                 bp is a pass.
    //
    // This primitive does NOT modify IR — it's a read-only
    // measurement. Multiple calls during a session return
    // fresh stats from the last compiled module (set by
    // CompilerService::last_ir_module()).
    ObservabilityPrims::register_stats_impl("compile:ir-stats", [&ev](const auto&) -> EvalValue {
        // Issue #1898: pin compiler_service_ for multi-step last_ir_stats read.
        return with_compiler_service_pin(ev, [&](CompilerService& svc) -> EvalValue {
            // Read the snapshot, not last_ir_module(). The snapshot
            // was computed when last_ir_mod_ was last assigned, so
            // it reflects the WORKLOAD's IR, not the IR of the
            // current stats-call expression (which would clobber
            // last_ir_mod_ on its own lowering).
            const auto& s = svc.last_ir_stats();
            if (s.total_instructions == 0 && s.total_functions == 0) {
                // No module compiled yet — return void.
                return make_void();
            }
            // Issue #1844: build_kv_hash shared helper (#1787).
            // Opcode histogram (nested hash, only non-zero opcodes).
            std::vector<std::pair<std::string, EvalValue>> op_kv;
            for (std::size_t i = 0; i < s.opcode_histogram.size(); ++i) {
                if (s.opcode_histogram[i] == 0)
                    continue;
                std::string name =
                    (i < 54) ? std::string(aura::ir::kOpcodeInfo[i].name) : std::string("?");
                op_kv.emplace_back(std::move(name),
                                   make_int(static_cast<std::int64_t>(s.opcode_histogram[i])));
            }
            EvalValue opcode_hist_ev = build_kv_hash(ev, op_kv);
            // Operand-count distribution (nested hash, 0..4).
            std::vector<std::pair<std::string, EvalValue>> dist_kv;
            for (std::size_t i = 0; i < 5; ++i) {
                dist_kv.emplace_back(
                    std::string(1, static_cast<char>('0' + i)),
                    make_int(static_cast<std::int64_t>(s.operand_count_distribution[i])));
            }
            EvalValue dist_ev = build_kv_hash(ev, dist_kv);
            // Top-level hash with all scalar fields + the 2 nested hashes.
            const std::uint64_t avg_ops_x100 =
                s.total_instructions ? (s.operands_used_sum * 100u / s.total_instructions) : 0;
            std::vector<std::pair<std::string, EvalValue>> top_kv = {
                {"total-instructions", make_int(static_cast<std::int64_t>(s.total_instructions))},
                {"total-functions", make_int(static_cast<std::int64_t>(s.total_functions))},
                {"total-blocks", make_int(static_cast<std::int64_t>(s.total_blocks))},
                {"avg-instructions-per-block-x100",
                 make_int(static_cast<std::int64_t>(
                     s.total_blocks ? (s.total_instructions * 100u / s.total_blocks) : 0))},
                {"avg-operands-used-x100", make_int(static_cast<std::int64_t>(avg_ops_x100))},
                {"aos-bytes-total", make_int(static_cast<std::int64_t>(s.aos_bytes_total))},
                {"padding-bytes-total", make_int(static_cast<std::int64_t>(s.padding_bytes_total))},
                {"unused-operand-bytes-total",
                 make_int(static_cast<std::int64_t>(s.unused_operand_bytes_total))},
                {"compact-bytes-projection",
                 make_int(static_cast<std::int64_t>(s.compact_bytes_projection))},
                {"compact-ratio-bp", make_int(static_cast<std::int64_t>(s.compact_ratio_bp))},
                {"opcode-histogram", opcode_hist_ev},
                {"operand-count-distribution", dist_ev},
            };
            return build_kv_hash(ev, top_kv);
        }); // with_compiler_service_pin
    });
}

// Issue #909 compile part 59 (orig 4947-5042)
void CompilePrims::register_compile_p59(PrimRegistrar add, Evaluator& ev) {
#if !AURA_ENABLE_SEVA
    (void)add;
    (void)ev;
#else
    // ── Issue #445: SEVA high-level goal primitives ──────
    //
    // The SEVA demo (#442) is the Aura-side verification
    // loop. The OpenClaw integration (#445) is the LLM
    // agent that drives the loop via natural-language
    // goals. This block ships the Aura-side primitives
    // that the OpenClaw skill/plugin calls into. Each
    // primitive wraps 1+ existing lower-level operations
    // (mutate:*, verify:*, query:*) so the agent doesn't
    // need to know the Aura primitives in detail.
    //
    // The primitives are deliberately conservative: they
    // return hashes (not raw lists) so the audit log
    // can be replayed post-hoc, and they never call into
    // destructive operations without a guard.
    //
    // Issue #1972: commercial SEVA vertical (AURA_ENABLE_SEVA).
    //
    // (seva:achieve-coverage name target-pct) — the
    // canonical SEVA goal. Reads the current coverage
    // (via verify-dirty-stats), compares to target,
    // returns a hash with the gap (or zero if already
    // met). The actual mutation loop is driven by the
    // OpenClaw agent, not by this primitive — the agent
    // decides which mutate:* primitive to call next.
    add("seva:achieve-coverage", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1]))
            return make_void();
        auto name_idx = as_string_idx(a[0]);
        auto target = as_int(a[1]);
        if (name_idx >= ev.string_heap_.size())
            return make_void();
        // Read the current coverage hole count via
        // the existing verify-dirty primitive
        // (#437 / #469). Issue #1840: acquire-load accessor.
        std::uint64_t current_dirty = 0;
        if (auto* ws = ev.workspace_flat()) {
            current_dirty = ws->verify_coverage_dirty_total();
        }
        // The "achievement" metric: dirty holes / 100 =
        // percent coverage hole. If current_dirty == 0
        // and target == 100, the goal is met.
        // The primitive returns a hash with the gap
        // analysis; the agent uses it to drive the loop.
        std::int64_t gap = (target >= 100) ? 0 : static_cast<std::int64_t>(current_dirty);
        std::int64_t achieved = (gap == 0) ? 1 : 0;
        // name as a string field
        auto name_idx_in_heap = ev.string_heap_.size();
        ev.string_heap_.push_back(ev.string_heap_[name_idx]);
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"name", make_string(name_idx_in_heap)},
            {"target-pct", make_int(target)},
            {"current-dirty", make_int(static_cast<std::int64_t>(current_dirty))},
            {"gap", make_int(gap)},
            {"achieved", make_int(achieved)},
        };
        return build_kv_hash(ev, kv);
    });
#endif // AURA_ENABLE_SEVA
}

// Issue #909 compile part 60 (orig 5043-5111)
void CompilePrims::register_compile_p60(PrimRegistrar add, Evaluator& ev) {
#if !AURA_ENABLE_SEVA
    (void)add;
    (void)ev;
#else
    // (seva:fix-reset-bugs) — read the current verify-
    // dirty state, identify reset-related holes, return
    // the list of node IDs the agent should target.
    // The actual mutate call is the agent's job; the
    // primitive just identifies the targets.
    // Issue #1972: commercial SEVA vertical (AURA_ENABLE_SEVA).
    add("seva:fix-reset-bugs", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_flat())
            return make_void();
        // For now: query verify-dirty-stats and return a
        // hash with the breakdown by reason. The agent
        // reads this and decides which (mutate:set-body
        // / mutate:replace-pattern) call to make next.
        // Issue #1840: single logical snapshot of the 4 counters
        // (pre-#1840: 4 independent relaxed loads → mixed epochs).
        std::uint64_t assertion = 0, coverage = 0, sva = 0, cex = 0;
        if (auto* ws = ev.workspace_flat()) {
            const auto snap = ws->snapshot_verify_dirty_totals();
            assertion = snap.assertion;
            coverage = snap.coverage;
            sva = snap.sva;
            cex = snap.formal_cex;
        }
        std::int64_t reset_holes = static_cast<std::int64_t>(assertion);
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"assertion-dirty", make_int(static_cast<std::int64_t>(assertion))},
            {"coverage-dirty", make_int(static_cast<std::int64_t>(coverage))},
            {"sva-dirty", make_int(static_cast<std::int64_t>(sva))},
            {"formal-cex-dirty", make_int(static_cast<std::int64_t>(cex))},
            {"reset-holes", make_int(reset_holes)},
        };
        return build_kv_hash(ev, kv);
    });
#endif // AURA_ENABLE_SEVA
}

// Issue #909 compile part 61 (orig 5112-5221)
void CompilePrims::register_compile_p61(PrimRegistrar add, Evaluator& ev) {

#if AURA_ENABLE_SEVA
    // (seva:generate-regression) — emit a regression
    // script (in Aura syntax) from the current state.
    // For the MVP this returns a string with the
    // testbench skeleton; the agent fills in the
    // specifics. The string is in ev.string_heap_.
    // Issue #1972: commercial SEVA vertical (AURA_ENABLE_SEVA).
    add("seva:generate-regression", [&ev](const auto&) -> EvalValue {
        auto sidx = ev.string_heap_.size();
        std::string script = ";; Auto-generated regression script (seva:generate-regression)\n"
                             ";; Step 1: re-load the workspace\n"
                             "(set-code \"<paste your DUT spec here>\")\n"
                             ";; Step 2: run the verification loop\n"
                             "(eval-current)\n"
                             ";; Step 3: query readiness\n"
                             "(query:edsl-readiness)\n"
                             ";; Step 4: query verify-dirty\n"
                             "(query:verify-dirty-stats)\n";
        ev.string_heap_.push_back(script);
        return make_string(sidx);
    });

    // (seva:approve-mutation id flag) — safety gate.
    // For the MVP this is a no-op that bumps a counter
    // (the agent's audit trail records every mutation
    // regardless). The flag "force" / "auto" / "deny"
    // tells the system whether the agent has human
    // approval for the mutation.
    add("seva:approve-mutation", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto nid = as_int(a[0]);
        std::string flag = "auto";
        if (a.size() >= 2 && is_string(a[1])) {
            auto fidx = as_string_idx(a[1]);
            if (fidx < ev.string_heap_.size())
                flag = ev.string_heap_[fidx];
        }
        bool approved = (flag == "force" || flag == "auto");
        (void)nid;
        return make_bool(approved);
    });
#endif // AURA_ENABLE_SEVA

    // (query:seva-audit-log) — Issue #445: the agent's
    // audit trail. Returns the recent mutations as a
    // summary (the full per-mutation record lives on
    // query:mutation-log-stats). For MVP: returns the
    // counts per category — agent calls this before
    // each major operation to confirm the audit log
    // is consistent. Always registered (not seva: prefix;
    // #1972 only gates seva:*).
    ObservabilityPrims::register_stats_impl(
        "query:seva-audit-log", [&ev](const auto&) -> EvalValue {
            std::uint64_t mutations = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
                mutations = m->atomic_batch_commits.load(std::memory_order_relaxed);
            }
            // Also include verify-dirty + mutation-rollbacks
            // so the audit trail covers the full loop.
            // Issue #1840: snapshot so assertion+coverage are co-epoch.
            std::uint64_t verify_total = 0;
            if (auto* ws = ev.workspace_flat()) {
                const auto snap = ws->snapshot_verify_dirty_totals();
                verify_total = snap.assertion + snap.coverage;
            }
            std::uint64_t auto_evolve_cycles = ev.auto_evolve_cycle_count_;
            std::uint64_t auto_evolve_fixed = ev.auto_evolve_total_fixed_;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"mutations-total", make_int(static_cast<std::int64_t>(mutations))},
                {"verify-dirty-total", make_int(static_cast<std::int64_t>(verify_total))},
                {"auto-evolve-cycles", make_int(static_cast<std::int64_t>(auto_evolve_cycles))},
                {"auto-evolve-fixed", make_int(static_cast<std::int64_t>(auto_evolve_fixed))},
            };
            return build_kv_hash(ev, kv);
        });
}

// Issue #909 compile part 62 (orig 5222-5318)
void CompilePrims::register_compile_p62(PrimRegistrar add, Evaluator& ev) {
#if !AURA_ENABLE_SEVA
    (void)add;
    (void)ev;
#else
    // (seva:run-demo-with-metrics) — Issue #446: collect
    // standardized metrics for L4-L5 claims. Returns a
    // hash with 6 fields covering the 5 metrics from the
    // issue body (iterations / coverage-improvement /
    // human-intervention / mutation-success-rate /
    // time-breakdown) + the active-strategy. The time-
    // breakdown is approximated as the lifetime
    // auto-evolve-cycle-count (proxy for "iteration
    // time"); real wall-time measurement is a follow-up
    // (would need a start/end timestamp pair).
    // Issue #1972: commercial SEVA vertical (AURA_ENABLE_SEVA).
    add("seva:run-demo-with-metrics", [&ev](const auto&) -> EvalValue {
        // Issue #1841: compiler_metrics_ is non-owning (#1835
        // ownership contract); active_strategy_ is under
        // strategies_mtx_ (#1720/#1722). verify totals use
        // snapshot_verify_dirty_totals (#1840).
        std::uint64_t iterations = ev.auto_evolve_cycle_count_;
        std::uint64_t mutations = 0;
        std::uint64_t mutations_success = 0;
        std::uint64_t verify_total = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            mutations = m->atomic_batch_commits.load(std::memory_order_relaxed);
        }
        if (auto* ws = ev.workspace_flat()) {
            const auto snap = ws->snapshot_verify_dirty_totals();
            verify_total = snap.assertion + snap.coverage;
            // mutations_success approximated as the
            // difference: total fixed - auto-evolve-fixed
            // is hard to compute without a per-mutation
            // outcome counter. For MVP: success-rate is
            // derived from strategy pheromone (see below).
        }
        // Read strategy pheromone for success-rate.
        std::uint64_t greedy_s = 0, bugfix_s = 0, minimal_s = 0;
        std::uint64_t greedy_h = 0, bugfix_h = 0, minimal_h = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            greedy_s = m->strategy_greedy_successes.load(std::memory_order_relaxed);
            bugfix_s = m->strategy_bugfix_successes.load(std::memory_order_relaxed);
            minimal_s = m->strategy_minimal_successes.load(std::memory_order_relaxed);
            greedy_h = m->strategy_greedy_hits.load(std::memory_order_relaxed);
            bugfix_h = m->strategy_bugfix_hits.load(std::memory_order_relaxed);
            minimal_h = m->strategy_minimal_hits.load(std::memory_order_relaxed);
        }
        std::uint64_t total_hits = greedy_h + bugfix_h + minimal_h;
        std::uint64_t total_success = greedy_s + bugfix_s + minimal_s;
        std::int64_t success_rate =
            total_hits > 0 ? static_cast<std::int64_t>((total_success * 100) / total_hits) : 0;
        std::int64_t human_intervention = 0; // MVP: agent runs autonomously
        auto active_idx = ev.string_heap_.size();
        {
            // Issue #1720: active_strategy_ guarded by strategies_mtx_.
            std::shared_lock<std::shared_mutex> lk(ev.strategies_mtx_);
            ev.string_heap_.push_back(ev.active_strategy_.empty() ? std::string("none")
                                                                  : ev.active_strategy_);
        }
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"iterations-to-closure", make_int(static_cast<std::int64_t>(iterations))},
            {"coverage-improvement", make_int(static_cast<std::int64_t>(verify_total))},
            {"human-intervention-count", make_int(human_intervention)},
            {"mutation-success-rate-pct", make_int(success_rate)},
            {"mutations-total", make_int(static_cast<std::int64_t>(mutations))},
            {"active-strategy", make_string(active_idx)},
        };
        return build_kv_hash(ev, kv);
    });
#endif // AURA_ENABLE_SEVA
}

// Issue #909 compile part 63 (orig 5319-5319)
void CompilePrims::register_compile_p63(PrimRegistrar add, Evaluator& ev) {
    // Issue #1516: (compile:aot-stats) — per-function AOT + EH coverage
    // production surface. Complements (query:aot-stats) which covers
    // hot-update reload (#452). Fields:
    //   - per-function-ir-emits / per-function-object-emits / misses
    //   - last-module-object-emits
    //   - exception-opcode-lowered / exception-opcodes-covered
    //   - exception-opcode-mask / interpreter-exception-ops
    //   - schema=1516
    ObservabilityPrims::register_stats_impl("compile:aot-stats", [&ev](const auto&) -> EvalValue {
        // Issue #1843: early nullptr check so kv entries do not
        // need per-field `m ?` ternaries (easy to forget on new keys).
        // compiler_metrics_ ownership is non-owning (#1835); null
        // means bare Evaluator — return zero-filled hash + schema
        // (pre-#1843 BC, not void).
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        auto zero_kv = []() -> std::vector<std::pair<std::string, EvalValue>> {
            return {
                {"per-function-ir-emits", make_int(0)},
                {"per-function-object-emits", make_int(0)},
                {"per-function-misses", make_int(0)},
                {"last-module-object-emits", make_int(0)},
                {"exception-opcode-lowered", make_int(0)},
                {"exception-opcodes-covered", make_int(0)},
                {"exception-opcode-mask", make_int(0)},
                {"interpreter-exception-ops", make_int(0)},
                {"schema", make_int(1516)},
            };
        };
        if (!m)
            return build_kv_hash(ev, zero_kv());
        const auto load = [](const std::atomic<std::uint64_t>& a) -> std::int64_t {
            return static_cast<std::int64_t>(a.load(std::memory_order_relaxed));
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"per-function-ir-emits", make_int(load(m->aot_per_function_ir_total))},
            {"per-function-object-emits", make_int(load(m->aot_per_function_object_total))},
            {"per-function-misses", make_int(load(m->aot_per_function_miss_total))},
            {"last-module-object-emits", make_int(load(m->aot_last_module_object_total))},
            {"exception-opcode-lowered", make_int(load(m->jit_exception_opcode_lowered))},
            {"exception-opcodes-covered", make_int(load(m->jit_exception_opcodes_covered))},
            {"exception-opcode-mask", make_int(load(m->jit_exception_opcode_mask))},
            {"interpreter-exception-ops", make_int(load(m->interpreter_exception_ops_total))},
            {"schema", make_int(1516)},
        };
        return build_kv_hash(ev, kv);
    });

    // Issue #1385: (compiler:metrics) primitive — expose env_frames_
    // and arena observability metrics as a JSON string. Refreshes
    // the 4 lazy-snapshot counters in CompilerMetrics
    // (env_frames_size_total, env_frames_stale_count,
    // ast_arena_bytes_in_use, ast_arena_upstream_bytes) before
    // serializing. Returns void if CompilerService / CompilerMetrics
    // back-pointers are unset (e.g. bare Evaluator without service).
    ObservabilityPrims::register_stats_impl("compiler:metrics", [&ev](const auto&) -> EvalValue {
        auto* svc = static_cast<CompilerService*>(ev.compiler_service());
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        if (!svc || !m)
            return types::make_void();
        svc->refresh_env_arena_metrics(*m);
        std::string json =
            std::format("{{\"env_frames_size_total\":{},\"env_frames_stale_count\":{},"
                        "\"ast_arena_bytes_in_use\":{},\"ast_arena_upstream_bytes\":{}}}",
                        m->env_frames_size_total.load(), m->env_frames_stale_count.load(),
                        m->ast_arena_bytes_in_use.load(), m->ast_arena_upstream_bytes.load());
        auto idx = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(json));
        return types::make_string(idx);
    });
    // Issue #1386: (evaluator:compact-env-frames) primitive.
    // Triggers env_frames_ arena compaction. Reclaims stale
    // frames (version_ < current defuse_version_) that are
    // not referenced by any live Closure. Rewrites
    // Closure::env_id via remap; bumps defuse_version_ so any
    // stale bridge_epoch snapshot re-bridges via
    // closure_bridge_. Returns the number of frames reclaimed
    // as an int. See Evaluator::compact_env_frames for the
    // algorithm + concurrency contract (caller must serialize
    // at the workspace level).
    //
    // Issue #1842 / #1889 / #1897 / #1955: wrap in try_acquire Guard + try/catch.
    // Pre-#1842 the primitive called compact_env_frames() raw —
    // a throw mid-remap left env_frames_ / Closure::env_id
    // partially consistent with no panic-checkpoint restore.
    // #1889 / #1955: all structural env compaction entry points from the
    // public primitive surface must go through Guard (truncate is
    // internal to panic restore, not a free primitive — still Guard-
    // wrapped when free-standing via #1927).
    // #1897: try_acquire (typed quota reject) via shared helper.
    add("evaluator:compact-env-frames", [&ev](const auto&) -> EvalValue {
        return run_under_mutation_guard(
            ev,
            [&]() -> EvalValue {
                return types::make_int(static_cast<int64_t>(ev.compact_env_frames()));
            },
            types::make_int(-1),
            /*track_env_compact_violation=*/true);
    });

    // Issue #1420 AC3: (compile:bidirectional-stats)
    // EDSL primitive. Surfaces the bidirectional
    // annotation check plumbing counters from
    // InferenceEngine::check_flat_call + the persistent
    // CompilerService bidirectional_mode_ flag. Used by
    // AI self-evol to detect whether annotation
    // contracts are being enforced (and how often
    // they're being violated / coerced via Gradual
    // Typing).
    //
    // The hash has 6 keys:
    //   - mode:              "full" | "disabled"
    //                        (persistent CompilerService flag;
    //                        "sampled" deferred to a follow-up
    //                        because it requires bidirectional_mode_
    //                        bool→enum upgrade in type_checker.ixx)
    //   - check-calls:       int (compile_bidirectional_check_call_total)
    //   - annotation-passes: int (compile_bidirectional_annotation_pass_total)
    //   - annotation-fails:  int (compile_bidirectional_annotation_fail_total)
    //   - coercion-deferred: int (compile_bidirectional_coercion_deferred_total)
    //   - narrow-records:    int (check_mode_narrow_hits_total, pre-existing
    //                        field — reuse rather than add a new atomic)
    //
    // Counter access via ev.get_bidirectional_stats_fn_() packed
    // uint64 (see Evaluator::get_bidirectional_stats_fn_ comment
    // for bit layout); mode + narrow-records read directly from
    // CompilerService / CompilerMetrics via the existing
    // ev.compiler_service() / ev.compiler_metrics() accessors.
    //
    // Default tier (kPrimSecSafe) — read-only stats primitive;
    // mirrors (compile:inline-pass-stats) at line 72 of
    // compile_05.cpp.
    ObservabilityPrims::register_stats_impl(
        "compile:bidirectional-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t packed = 0;
            if (ev.get_bidirectional_stats_fn_) {
                packed = ev.get_bidirectional_stats_fn_();
            }
            const std::uint64_t check_call = packed & 0xFFFFFF;
            const std::uint64_t pass = (packed >> 24) & 0xFFFF;
            const std::uint64_t fail = (packed >> 40) & 0xFFFF;
            const std::uint64_t coercion = (packed >> 56) & 0xFF;

            std::string mode_str = "full"; // default if no service back-pointer
            if (auto* svc = static_cast<CompilerService*>(ev.compiler_service())) {
                mode_str = svc->bidirectional_mode() ? "full" : "disabled";
            }

            std::uint64_t narrow_records = 0;
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                narrow_records = m->check_mode_narrow_hits_total.load(std::memory_order_relaxed);
            }


            auto mode_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(mode_str);
            EvalValue mode_ev = make_string(mode_idx);

            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"mode", mode_ev},
                {"check-calls", make_int(static_cast<std::int64_t>(check_call))},
                {"annotation-passes", make_int(static_cast<std::int64_t>(pass))},
                {"annotation-fails", make_int(static_cast<std::int64_t>(fail))},
                {"coercion-deferred", make_int(static_cast<std::int64_t>(coercion))},
                {"narrow-records", make_int(static_cast<std::int64_t>(narrow_records))},
            };
            return build_kv_hash(ev, kv);
        });
}
aura::ast::FlatAST* CompilePrims::pick_macro_flat(Evaluator& ev) {
    return ev.current_flat() ? ev.current_flat() : ev.workspace_flat();
}

void CompilePrims::register_all(PrimRegistrar add, Evaluator& ev) {
    register_compile_p0(add, ev);
    register_compile_p1(add, ev);
    register_compile_p2(add, ev);
    register_compile_p3(add, ev);
    register_compile_p4(add, ev);
    register_compile_p5(add, ev);
    register_compile_p6(add, ev);
    register_compile_p7(add, ev);
    register_compile_p8(add, ev);
    register_compile_p9(add, ev);
    register_compile_p10(add, ev);
    register_compile_p11(add, ev);
    register_compile_p12(add, ev);
    register_compile_p13(add, ev);
    register_compile_p14(add, ev);
    register_compile_p15(add, ev);
    register_compile_p16(add, ev);
    register_compile_p17(add, ev);
    register_compile_p18(add, ev);
    register_compile_p19(add, ev);
    register_compile_p20(add, ev);
    register_compile_p21(add, ev);
    register_compile_p22(add, ev);
    register_compile_p23(add, ev);
    register_compile_p24(add, ev);
    register_compile_p25(add, ev);
    register_compile_p26(add, ev);
    register_compile_p27(add, ev);
    register_compile_p28(add, ev);
    register_compile_p29(add, ev);
    register_compile_p30(add, ev);
    register_compile_p31(add, ev);
    register_compile_p32(add, ev);
    register_compile_p33(add, ev);
    register_compile_p34(add, ev);
    register_compile_p35(add, ev);
    register_compile_p36(add, ev);
    register_compile_p37(add, ev);
    register_compile_p38(add, ev);
    register_compile_p39(add, ev);
    register_compile_p40(add, ev);
    register_compile_p41(add, ev);
    register_compile_p42(add, ev);
    register_compile_p43(add, ev);
    register_compile_p44(add, ev);
    register_compile_p45(add, ev);
    register_compile_p46(add, ev);
    register_compile_p47(add, ev);
    register_compile_p48(add, ev);
    register_compile_p49(add, ev);
    register_compile_p50(add, ev);
    register_compile_p51(add, ev);
    register_compile_p52(add, ev);
    register_compile_p53(add, ev);
    register_compile_p54(add, ev);
    register_compile_p55(add, ev);
    register_compile_p56(add, ev);
    register_compile_p57(add, ev);
    register_compile_p58(add, ev);
    register_compile_p59(add, ev);
    register_compile_p60(add, ev);
    register_compile_p61(add, ev);
    register_compile_p62(add, ev);
    register_compile_p63(add, ev);
}

void register_compile_primitives(PrimRegistrar add, Evaluator& ev) {
    CompilePrims::register_all(add, ev);
}

} // namespace aura::compiler::primitives_detail
