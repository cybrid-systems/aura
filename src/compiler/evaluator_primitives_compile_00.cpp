// evaluator_primitives_compile_00.cpp — Issue #909: peeled compile registration
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
    add("compile:linear-elide-count", [&ev](const auto&) -> EvalValue {
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
    add("compile:dead-coercion-stats", [&ev](const auto&) -> EvalValue {
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
    add("compile:dead-coercion-elapsed", [&ev](const auto&) -> EvalValue {
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
    add("compile:dead-coercion-kept-for-debug", [&ev](const auto&) -> EvalValue {
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
    add("query:dead-coercion-elim-stats", [&ev](const auto&) -> EvalValue {
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
    add("query:dead-coercion-elision-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t elided = 0, evidence_hits = 0, stable_paths = 0, savings = 0,
                      castop_emitted = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            elided = m->dead_coercion_elision_elided_casts_total.load(std::memory_order_relaxed);
            evidence_hits =
                m->dead_coercion_elision_evidence_hits_total.load(std::memory_order_relaxed);
            stable_paths = m->dead_coercion_elision_narrowing_stable_paths_total.load(
                std::memory_order_relaxed);
            savings = m->dead_coercion_elision_runtime_check_savings_total.load(
                std::memory_order_relaxed);
            castop_emitted = m->coercion_castop_emitted_total.load(std::memory_order_relaxed);
        }
        const std::int64_t evidence_hit_rate = static_cast<std::int64_t>(
            (evidence_hits * 100ULL) / (evidence_hits + castop_emitted + 1ULL));
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
        insert_kv("elided-casts", static_cast<std::int64_t>(elided));
        insert_kv("evidence-hit-rate", evidence_hit_rate);
        insert_kv("narrowing-stable-paths", static_cast<std::int64_t>(stable_paths));
        insert_kv("runtime-check-savings", static_cast<std::int64_t>(savings));
        insert_kv("schema", 799);
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
    add("compile:ir-soa-stats", [&ev](const auto&) -> EvalValue {
        // Re-use the build_hash pattern from closure:stats
        // above (same FNV-1a hash + open-addressing insert).
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
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
    add("compile:invalidations-stats", [&ev](const auto&) -> EvalValue {
        // Re-use the build_hash pattern from compile:ir-soa-stats
        // above (same FNV-1a hash + open-addressing insert).
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
        // The counters live on the workspace FlatAST (set up in
        // ast.ixx). Get the FlatAST via the Evaluator's
        // ev.workspace_flat() accessor (added in #175 so
        // service.ixx can read workspace state).
        if (auto* ws_flat = ev.workspace_flat()) {
            bumps = ws_flat->bump_generation_count();
            checks = ws_flat->is_valid_check_count();
            inits = ws_flat->stable_ref_invalidations();
            commits = ws_flat->atomic_batch_commits_v();
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
    add("compile:ast-ops-stats", [&ev](const auto&) -> EvalValue {
        // Re-use the build_hash pattern from above primitives.
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
        // Counters live on the workspace FlatAST (set up in
        // ast.ixx). Get the FlatAST via the Evaluator's
        // ev.workspace_flat() accessor.
        if (auto* ws_flat = ev.workspace_flat()) {
            children_calls = ws_flat->children_call_count();
            parent_calls = ws_flat->parent_of_call_count();
            dirty_calls = ws_flat->mark_dirty_upward_call_count();
            dirty_nodes = ws_flat->mark_dirty_total_nodes();
        }
        // Issue #336: include the mark_dirty_upward_fast
        // fixed-point early-exit counter so callers can
        // benchmark the optimization.
        std::uint64_t fast_hits = 0;
        if (auto* ws_flat = ev.workspace_flat()) {
            fast_hits = ws_flat->dirty_upward_fast_fixed_point_count();
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
            {"dirty-upward-fast-fixed-point-hits", make_int(static_cast<std::int64_t>(fast_hits))},
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
    add("compile:multi-mutation-stats", [&ev](const auto&) -> EvalValue {
        // Re-use the build_hash pattern from above primitives.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
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
        std::uint64_t hits = 0, misses = 0, stale = 0, solve_us = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            hits = m->typecheck_cache_hits_total.load(std::memory_order_relaxed);
            misses = m->typecheck_cache_misses_total.load(std::memory_order_relaxed);
            stale = m->typecheck_stale_cache_total.load(std::memory_order_relaxed);
            solve_us = m->delta_solve_time_us.load(std::memory_order_relaxed);
        }
        std::uint64_t total = hits + misses + stale;
        std::uint64_t ratio_bp = (total > 0) ? (misses * 10000u / total) : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"cache-hits-total", make_int(static_cast<std::int64_t>(hits))},
            {"cache-misses-total", make_int(static_cast<std::int64_t>(misses))},
            {"stale-cache-total", make_int(static_cast<std::int64_t>(stale))},
            {"delta-solve-time-us", make_int(static_cast<std::int64_t>(solve_us))},
            {"multi-mutation-recompute-ratio-bp", make_int(static_cast<std::int64_t>(ratio_bp))},
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
    add("compile:type-propagation-stats", [&ev](const auto&) -> EvalValue {
        // Re-use the build_hash pattern from above primitives.
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

} // namespace aura::compiler::primitives_detail
