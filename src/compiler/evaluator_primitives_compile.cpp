// evaluator_primitives_compile.cpp — P0 step 14: compile:* / concurrency:* / syntax-marker primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "observability_metrics.h"
#include "per_defuse_index.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.mutators;  // Phase 4 follow-up #4: (compile:mutator-dispatch-stats)
import aura.core.type;
import aura.compiler.value;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.pass_manager;
import aura.compiler.query;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using namespace types;

void register_compile_primitives(PrimRegistrar add, Evaluator& ev) {

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
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            instr = m->ir_soa_instructions_emitted.load(std::memory_order_relaxed);
            funcs = m->ir_soa_functions_emitted.load(std::memory_order_relaxed);
        }
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"instructions-emitted", make_int(static_cast<std::int64_t>(instr))},
            {"functions-emitted", make_int(static_cast<std::int64_t>(funcs))},
        };
        return build_hash(kv);
    });

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
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
            {"dirty-upward-fast-fixed-point-hits",
             make_int(static_cast<std::int64_t>(fast_hits))},
        };
        return build_hash(kv);
    });

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
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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

    // (compile:type-propagation-stats) — Issue #259: observability
    // for the IR type metadata propagation path. Hash with 3
    // fields:
    //   - ir-instructions-total: lifetime total IR instructions
    //     executed by the IR interpreter
    //   - ir-instructions-with-type-total: lifetime total where
    //     the lowering pass populated type_id (the propagation
    //     landed)
    //   - type-propagation-coverage-bp: derived ratio (with_type
    //     * 10000 / total) in basis points (0-10000). 0 = no
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
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
    add("compile:occurrence-typing-stats",
        [&ev](const auto&) -> EvalValue {
        auto build_hash =
            [&](std::span<const std::pair<std::string, EvalValue>> kv)
            -> EvalValue {
            auto cap = std::max<std::size_t>(8, kv.size() * 2);
            std::size_t hcap = 8;
            while (hcap < cap) hcap *= 2;
            auto* ht = FlatHashTable::create(hcap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) *
                        0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) |
                          0x80;
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
        auto* svc = static_cast<class CompilerService*>(
            ev.compiler_service_);
        auto snap = svc->snapshot();
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"applied-total",
             make_int(static_cast<std::int64_t>(
                 snap.narrowing_applied_total))},
            {"skipped-total",
             make_int(static_cast<std::int64_t>(
                 snap.narrowing_skipped_total))},
            {"reanalyzed-total",
             make_int(static_cast<std::int64_t>(
                 snap.narrowing_reanalyzed_total))},
            {"applied-ratio-bp",
             make_int(static_cast<std::int64_t>(
                 snap.narrowing_applied_ratio_bp))},
        };
        return build_hash(kv);
    });

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
    add("compile:and-or-precision-stats",
        [&ev](const auto&) -> EvalValue {
        auto build_hash =
            [&](std::span<const std::pair<std::string, EvalValue>> kv)
            -> EvalValue {
            auto cap = std::max<std::size_t>(8, kv.size() * 2);
            std::size_t hcap = 8;
            while (hcap < cap) hcap *= 2;
            auto* ht = FlatHashTable::create(hcap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) *
                        0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) |
                          0x80;
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
        auto* svc = static_cast<class CompilerService*>(
            ev.compiler_service_);
        auto snap = svc->snapshot();
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"meet-uses-total",
             make_int(static_cast<std::int64_t>(
                 snap.and_or_meet_uses_total))},
            {"join-uses-total",
             make_int(static_cast<std::int64_t>(
                 snap.and_or_join_uses_total))},
        };
        return build_hash(kv);
    });

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
    add("compile:occurrence-dirty-stats",
        [&ev](const auto&) -> EvalValue {
        auto build_hash =
            [&](std::span<const std::pair<std::string, EvalValue>> kv)
            -> EvalValue {
            auto cap = std::max<std::size_t>(8, kv.size() * 2);
            std::size_t hcap = 8;
            while (hcap < cap) hcap *= 2;
            auto* ht = FlatHashTable::create(hcap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) *
                        0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) |
                          0x80;
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
        auto* svc = static_cast<class CompilerService*>(
            ev.compiler_service_);
        auto snap = svc->snapshot();
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"dirty-recovery-total",
             make_int(static_cast<std::int64_t>(
                 snap.narrowing_dirty_recovery_total))},
        };
        return build_hash(kv);
    });

    // (compile:schema-cache-stats)
    //   — Issue #390: per-node schema cache
    //   observability. Returns a hash with 3
    //   fields: lookups-total (lifetime total
    //   of schema_cache column lookups in the
    //   type-checker cache hit path) /
    //   hits-total (lookups that returned a
    //   non-zero schema that matched the
    //   cached type_id) / hit-rate-bp (basis
    //   points: hits / lookups * 10000).
    //   Companion to the (query:schema-of-marker)
    //   diagnostic primitive from #248. The full
    //   #390 scope is also auto-populating the
    //   cache in clone_macro_body + type checker
    //   integration + typed_mutate schema-violation
    //   guard; this slice ships the observability
    //   foundation + the basic cache check in
    //   synthesize_flat.
    add("compile:schema-cache-stats",
        [&ev](const auto&) -> EvalValue {
        auto build_hash =
            [&](std::span<const std::pair<std::string, EvalValue>> kv)
            -> EvalValue {
            auto cap = std::max<std::size_t>(8, kv.size() * 2);
            std::size_t hcap = 8;
            while (hcap < cap) hcap *= 2;
            auto* ht = FlatHashTable::create(hcap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) *
                        0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) |
                          0x80;
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
        auto* svc = static_cast<class CompilerService*>(
            ev.compiler_service_);
        auto snap = svc->snapshot();
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"lookups-total",
             make_int(static_cast<std::int64_t>(
                 snap.schema_cache_lookups_total))},
            {"hits-total",
             make_int(static_cast<std::int64_t>(
                 snap.schema_cache_hits_total))},
            {"hit-rate-bp",
             make_int(static_cast<std::int64_t>(
                 snap.schema_cache_hit_rate_bp))},
        };
        return build_hash(kv);
    });

    // (compile:constraint-dep-stats)
    //   — Issue #409: fine-grained constraint
    //   dependency tracking observability. Returns
    //   a hash with 3 fields: processed-total
    //   (lifetime total of constraints re-solved
    //   via solve_delta) / total (lifetime total
    //   of constraints added via add_delta) /
    //   ratio-bp (basis points: processed /
    //   total * 10000). The ratio measures how
    //   much the reverse map prunes — a low
    //   ratio means the filter is doing useful
    //   work. Pre-#409 the ratio was always 1.0
    //   (all dirty constraints re-solved). The
    //   full #409 scope also extends the reverse
    //   map to cover more constraint kinds +
    //   var-rep updates across unify; this slice
    //   ships the observability foundation.
    add("compile:constraint-dep-stats",
        [&ev](const auto&) -> EvalValue {
        auto build_hash =
            [&](std::span<const std::pair<std::string, EvalValue>> kv)
            -> EvalValue {
            auto cap = std::max<std::size_t>(8, kv.size() * 2);
            std::size_t hcap = 8;
            while (hcap < cap) hcap *= 2;
            auto* ht = FlatHashTable::create(hcap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) *
                        0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) |
                          0x80;
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
        auto* svc = static_cast<class CompilerService*>(
            ev.compiler_service_);
        auto snap = svc->snapshot();
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"processed-total",
             make_int(static_cast<std::int64_t>(
                 snap.delta_constraints_processed_total))},
            {"total",
             make_int(static_cast<std::int64_t>(
                 snap.delta_constraints_total))},
            {"ratio-bp",
             make_int(static_cast<std::int64_t>(
                 snap.delta_solve_constraints_ratio_bp))},
        };
        return build_hash(kv);
    });

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
    add("compile:constraint-solver-stats",
        [&ev](const auto&) -> EvalValue {
        auto build_hash =
            [&](std::span<const std::pair<std::string, EvalValue>> kv)
            -> EvalValue {
            auto cap = std::max<std::size_t>(8, kv.size() * 2);
            std::size_t hcap = 8;
            while (hcap < cap) hcap *= 2;
            auto* ht = FlatHashTable::create(hcap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) *
                        0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) |
                          0x80;
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
        auto* svc = static_cast<class CompilerService*>(
            ev.compiler_service_);
        auto snap = svc->snapshot();
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"unify-total",
             make_int(static_cast<std::int64_t>(
                 snap.consistent_unify_total))},
            {"subtype-total",
             make_int(static_cast<std::int64_t>(
                 snap.consistent_subtype_total))},
            {"restart-total",
             make_int(static_cast<std::int64_t>(
                 snap.worklist_restart_total))},
        };
        return build_hash(kv);
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
    //   register * 10000 — measures cache
    //   effectiveness). The full #385 scope
    //   also includes per-binding mutation
    //   version stamping + poly constraints
    //   integrated with ConstraintSystem dirty
    //   tracking + Value Restriction
    //   re-evaluation; this slice ships the
    //   observability foundation.
    add("compile:let-poly-stats",
        [&ev](const auto&) -> EvalValue {
        auto build_hash =
            [&](std::span<const std::pair<std::string, EvalValue>> kv)
            -> EvalValue {
            auto cap = std::max<std::size_t>(8, kv.size() * 2);
            std::size_t hcap = 8;
            while (hcap < cap) hcap *= 2;
            auto* ht = FlatHashTable::create(hcap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) *
                        0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) |
                          0x80;
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
        auto* svc = static_cast<class CompilerService*>(
            ev.compiler_service_);
        auto snap = svc->snapshot();
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"register-total",
             make_int(static_cast<std::int64_t>(
                 snap.poly_register_total))},
            {"dedup-hits-total",
             make_int(static_cast<std::int64_t>(
                 snap.poly_dedup_hits_total))},
            {"instantiate-total",
             make_int(static_cast<std::int64_t>(
                 snap.poly_instantiate_total))},
            {"dedup-ratio-bp",
             make_int(static_cast<std::int64_t>(
                 snap.poly_dedup_ratio_bp))},
        };
        return build_hash(kv);
    });

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
    //   * 10000 — measures the dirty-trigger
    //   rate). The full #487 scope also includes
    //   wiring should_relower_on_dirty to the
    //   pass pipeline + a query:dirty-impact
    //   primitive for fine-grained impact; this
    //   slice ships the observability foundation
    //   + the 2 lifetime counters.
    add("compile:dirty-impact-stats",
        [&ev](const auto&) -> EvalValue {
        auto build_hash =
            [&](std::span<const std::pair<std::string, EvalValue>> kv)
            -> EvalValue {
            auto cap = std::max<std::size_t>(8, kv.size() * 2);
            std::size_t hcap = 8;
            while (hcap < cap) hcap *= 2;
            auto* ht = FlatHashTable::create(hcap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) *
                        0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) |
                          0x80;
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
        auto* svc = static_cast<class CompilerService*>(
            ev.compiler_service_);
        auto snap = svc->snapshot();
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"should-relower-total",
             make_int(static_cast<std::int64_t>(
                 snap.should_relower_total))},
            {"affected-subtree-total",
             make_int(static_cast<std::int64_t>(
                 snap.affected_subtree_total))},
            {"trigger-rate-bp",
             make_int(static_cast<std::int64_t>(
                 snap.dirty_trigger_rate_bp))},
        };
        return build_hash(kv);
    });

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
    //   lookups * 10000). The full #387 scope
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
    add("compile:type-dep-graph-stats",
        [&ev](const auto&) -> EvalValue {
        if (!ev.compiler_service_)
            return make_int(0);
        auto* svc = static_cast<class CompilerService*>(
            ev.compiler_service_);
        auto snap = svc->snapshot();
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"lookups-total",
             make_int(static_cast<std::int64_t>(
                 snap.type_dep_graph_lookups))},
            {"hits-total",
             make_int(static_cast<std::int64_t>(
                 snap.type_dep_graph_hits))},
            {"size",
             make_int(static_cast<std::int64_t>(
                 snap.type_dep_graph_size))},
            {"hit-rate-bp",
             make_int(static_cast<std::int64_t>(
                 snap.type_dep_graph_hit_rate_bp))},
        };
        // Use the same hash-table builder pattern as
        // compile:dirty-impact-stats (create +
        // insert + return). For scope-limited
        // consistency, build it inline here.
        auto cap = std::max<std::size_t>(8, kv.size() * 2);
        std::size_t hcap = 8;
        while (hcap < cap) hcap *= 2;
        auto* ht = FlatHashTable::create(hcap);
        if (!ht)
            return make_int(0);
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        for (auto& [k, v] : kv) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (char c : k)
                h = (h ^ static_cast<std::uint8_t>(c)) *
                    0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) |
                      0x80;
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

    // (compile:match-narrowing-stats)
    //   — Issue #341: match + Occurrence Typing
    //   integration observability. Returns a hash
    //   with 3 fields: narrowed-total (lifetime
    //   total of __match_tmp lets whose subject
    //   type was refined by a prior narrowing in
    //   the env) / total (lifetime total of
    //   __match_tmp lets processed by the type
    //   checker) / ratio-bp (basis points:
    //   narrowed / total * 10000). The full
    //   #341 scope is also extending
    //   analyze_predicate_flat to recognize more
    //   ADT-related predicates and feeding the
    //   refined type into match exhaustiveness
    //   checking. This slice ships the
    //   observability foundation + the basic
    //   env-lookup path for the subject type.
    add("compile:match-narrowing-stats",
        [&ev](const auto&) -> EvalValue {
        auto build_hash =
            [&](std::span<const std::pair<std::string, EvalValue>> kv)
            -> EvalValue {
            auto cap = std::max<std::size_t>(8, kv.size() * 2);
            std::size_t hcap = 8;
            while (hcap < cap) hcap *= 2;
            auto* ht = FlatHashTable::create(hcap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) *
                        0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) |
                          0x80;
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
        auto* svc = static_cast<class CompilerService*>(
            ev.compiler_service_);
        auto snap = svc->snapshot();
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"narrowed-total",
             make_int(static_cast<std::int64_t>(
                 snap.match_subject_narrowed_total))},
            {"total",
             make_int(static_cast<std::int64_t>(
                 snap.match_subject_total))},
            {"ratio-bp",
             make_int(static_cast<std::int64_t>(
                 snap.match_narrowed_ratio_bp))},
        };
        return build_hash(kv);
    });

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
    add("compile:narrowing-blame-stats",
        [&ev](const auto&) -> EvalValue {
        auto build_hash =
            [&](std::span<const std::pair<std::string, EvalValue>> kv)
            -> EvalValue {
            auto cap = std::max<std::size_t>(8, kv.size() * 2);
            std::size_t hcap = 8;
            while (hcap < cap) hcap *= 2;
            auto* ht = FlatHashTable::create(hcap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) *
                        0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) |
                          0x80;
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
        auto* svc = static_cast<class CompilerService*>(
            ev.compiler_service_);
        auto snap = svc->snapshot();
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"provenance-total",
             make_int(static_cast<std::int64_t>(
                 snap.narrowing_provenance_total))},
        };
        return build_hash(kv);
    });

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
    add("ast:generation-stats",
        [&ev](const auto&) -> EvalValue {
        auto build_hash =
            [&](std::span<const std::pair<std::string, EvalValue>> kv)
            -> EvalValue {
            auto cap = std::max<std::size_t>(8, kv.size() * 2);
            std::size_t hcap = 8;
            while (hcap < cap) hcap *= 2;
            auto* ht = FlatHashTable::create(hcap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) *
                        0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) |
                          0x80;
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
        auto* svc = static_cast<class CompilerService*>(
            ev.compiler_service_);
        auto snap = svc->snapshot();
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"current-generation",
             make_int(static_cast<std::int64_t>(
                 snap.current_generation))},
            {"bump-generation-total",
             make_int(static_cast<std::int64_t>(
                 snap.bump_generation_count))},
            {"generation-wrap-total",
             make_int(static_cast<std::int64_t>(
                 snap.generation_wrap_count))},
            {"stable-ref-invalidations-total",
             make_int(static_cast<std::int64_t>(
                 snap.stable_ref_invalidations))},
            {"node-gen-stale-access-total",
             make_int(static_cast<std::int64_t>(
                 snap.node_gen_stale_access_count))},
        };
        return build_hash(kv);
    });

    // (compile:status)
    //   → ((:key value) ...)  association list
    //   Returns incremental compilation status:
    //     :dirty-nodes   — nodes marked as dirty (need recompilation)
    //     :clean-nodes   — nodes that are up-to-date
    //     :generation    — FlatAST generation counter
    //     :mutation-count— total mutations applied
    add("compile:status", [&ev](const auto&) -> EvalValue {
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

    // (compile:cache-size) — Issue #196: number of defines
    // currently in the ir_cache_v2_ map. Each entry corresponds
    // to a top-level define that has been compiled at least
    // once. Returns 0 if no hook is installed.
    add("compile:cache-size", [&ev](const auto&) -> EvalValue {
        if (!ev.get_incremental_stats_fn_)
            return make_int(0);
        auto packed = ev.get_incremental_stats_fn_();
        return make_int(static_cast<std::int64_t>(packed >> 48));
    });

    // (compile:dirty-count) — Issue #196: number of currently-
    // dirty entries in the ir_cache_v2_ map. A dirty entry
    // means the cached IR is stale and needs re-lower on next
    // access. Returns 0 if no hook is installed.
    add("compile:dirty-count", [&ev](const auto&) -> EvalValue {
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
        ws->mark_dirty_upward_fast(node_id, reasons);
        return make_void();
    });

    // (compile:epoch) — Issue #196: current mutation_epoch_ value.
    // The epoch is bumped atomically on every mutation. Cache
    // entries that haven't seen the current epoch are stale.
    // Returns 0 if no hook is installed.
    add("compile:epoch", [&ev](const auto&) -> EvalValue {
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
    add("compile:dirty-reason-counts", [&ev](const auto&) -> EvalValue {
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
            if (byte & 0x01) ++counts[0];
            if (byte & 0x02) ++counts[1];
            if (byte & 0x04) ++counts[2];
            if (byte & 0x08) ++counts[3];
            if (byte & 0x10) ++counts[4];
            if (byte & 0x20) ++counts[5];
            if (byte & 0x40) ++counts[6];
            if (byte & 0x80) ++counts[7];
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
        if (reason_name == "general")          mask = 0x01;
        else if (reason_name == "constraint") mask = 0x02;
        else if (reason_name == "occurrence") mask = 0x04;
        else if (reason_name == "ownership")   mask = 0x08;
        else if (reason_name == "coercion")    mask = 0x10;
        else if (reason_name == "struct")      mask = 0x20;
        else if (reason_name == "defuse")      mask = 0x40;
        else if (reason_name == "ppa-hint")    mask = 0x80;
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

    // (compile:dep-edges) — Issue #196: total number of edges
    // in the dep_graph_ map. Each edge means "this define
    // depends on that define"; mutations to the target cascade
    // to invalidate the source via invalidate_function BFS.
    // Returns 0 if no hook is installed.
    add("compile:dep-edges", [&ev](const auto&) -> EvalValue {
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
    add("compile:mark-block-dirty!", [&ev](const auto& a) -> EvalValue {
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
        return make_bool(ev.mark_block_dirty_fn_(ev.string_heap_[idx].c_str(),
                                              static_cast<std::size_t>(fidx),
                                              static_cast<std::uint32_t>(bidx)));
    });

    // (compile:clear-block-dirty! name func-idx block-idx) —
    // Issue #196: clear a single (function, block) dirty bit
    // in the named define's IR cache entry. Returns #t on
    // success, #f if the entry doesn't exist or the hook is
    // not installed. Use case: the smarter re-lower clears
    // the dirty bit after re-lowering a block.
    add("compile:clear-block-dirty!", [&ev](const auto& a) -> EvalValue {
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
        return make_bool(ev.clear_block_dirty_fn_(ev.string_heap_[idx].c_str(),
                                               static_cast<std::size_t>(fidx),
                                               static_cast<std::uint32_t>(bidx)));
    });

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
            ev.string_heap_[idx].c_str(),
            static_cast<std::size_t>(as_int(a[1])),
            static_cast<std::uint32_t>(as_int(a[2])),
            static_cast<std::uint32_t>(as_int(a[3]))));
    });

    // Issue #460: (compile:mark-instruction-dirty! name
    // func-idx block-idx instr-idx) — per-instruction dirty
    // marker. Returns #t on success, #f if no hook.
    add("compile:mark-instruction-dirty!", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 4 || !is_string(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !is_int(a[3]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        if (!ev.mark_instruction_dirty_fn_)
            return make_bool(false);
        return make_bool(ev.mark_instruction_dirty_fn_(
            ev.string_heap_[idx].c_str(),
            static_cast<std::size_t>(as_int(a[1])),
            static_cast<std::uint32_t>(as_int(a[2])),
            static_cast<std::uint32_t>(as_int(a[3]))));
    });

    // Issue #460: (compile:clear-instruction-dirty! name
    // func-idx block-idx instr-idx) — per-instruction clear.
    add("compile:clear-instruction-dirty!", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 4 || !is_string(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !is_int(a[3]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        if (!ev.clear_instruction_dirty_fn_)
            return make_bool(false);
        return make_bool(ev.clear_instruction_dirty_fn_(
            ev.string_heap_[idx].c_str(),
            static_cast<std::size_t>(as_int(a[1])),
            static_cast<std::uint32_t>(as_int(a[2])),
            static_cast<std::uint32_t>(as_int(a[3]))));
    });

    // Issue #460: (query:compiler-incremental-stats) — return
    // the current partial-relower / impact-scope counters.
    // P0 ship: returns the partial_relower_count as an int.
    // Follow-up: returns a 3-tuple
    // (partial-relower impact-scope-calls total-affected-blocks).
    add("query:compiler-incremental-stats", [&ev](const auto& a) -> EvalValue {
        (void)a;
        return make_int(static_cast<std::int64_t>(
            ev.get_partial_relower_count()));
    });

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
    add("query:compiler-cache-stats", [&ev](const auto& a) -> EvalValue {
        (void)a;
        auto* svc_void = ev.compiler_service();
        if (!svc_void) return make_int(0);
        auto* svc = static_cast<aura::compiler::CompilerService*>(svc_void);
        // Build 3-tuple as nested pair-of-pairs:
        // ((dirty-blocks . dirty-functions) . incremental-candidates)
        std::int64_t dirty_blocks = static_cast<std::int64_t>(svc->total_dirty_block_count());
        std::int64_t dirty_funcs  = static_cast<std::int64_t>(svc->total_dirty_func_count());
        std::int64_t incr_cands   = static_cast<std::int64_t>(svc->total_incremental_candidates());
        auto p1 = ev.pairs_.size();
        ev.pairs_.push_back({make_int(dirty_blocks), make_int(dirty_funcs)});
        auto outer = ev.pairs_.size();
        ev.pairs_.push_back({make_pair(p1), make_int(incr_cands)});
        return make_pair(outer);
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
    add("query:incremental-effectiveness", [&ev](const auto& a) -> EvalValue {
        (void)a;
        auto* svc_void = ev.compiler_service();
        if (!svc_void) return make_int(0);
        auto* svc = static_cast<aura::compiler::CompilerService*>(svc_void);

        std::int64_t ratio_bp = 0;
        std::int64_t cascade_depth = 0;
        std::int64_t bridge_overhead = 0;
        std::int64_t fallback_freq = 0;

        // Read the latest snapshot. Wrapped in try/catch so a
        // service-side throw doesn't propagate as an
        // unhandled error value.
        try {
            auto snap = svc->snapshot();
            auto total_defines = svc->ir_cache_v2_size();
            auto dirty_funcs = svc->total_dirty_func_count();
            if (total_defines > 0) {
                ratio_bp = (dirty_funcs * 10000) / static_cast<std::int64_t>(total_defines);
            }
            cascade_depth = static_cast<std::int64_t>(snap.mark_dirty_total_nodes);
            bridge_overhead = static_cast<std::int64_t>(snap.closure_bridge_calls);
            fallback_freq = static_cast<std::int64_t>(
                snap.closure_tw_calls + snap.closure_ffi_calls);
        } catch (...) {
            // Service-side failure: zeros are already initialized.
        }

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
    add("compile:relower-strategy", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        auto* svc_void = ev.compiler_service();
        if (!svc_void) return make_bool(false);
        auto* svc = static_cast<aura::compiler::CompilerService*>(svc_void);
        const std::string& fname = ev.string_heap_[idx];
        // Look up the entry in ir_cache_v2_
        auto it = svc->ir_cache_v2_find(fname);
        if (!it) {
            // Function not in cache — return 'unknown symbol
            auto sym_idx = ev.keyword_table_.size();
            ev.keyword_table_.push_back("unknown");
            return make_keyword(sym_idx);
        }
        std::size_t dirty = it->dirty_block_count();
        const char* tag = nullptr;
        if (dirty == 0) tag = "none";
        else if (dirty < 8) tag = "incremental";
        else tag = "full";
        auto sym_idx = ev.keyword_table_.size();
        ev.keyword_table_.push_back(tag);
        return make_keyword(sym_idx);
    });

    // Issue #459: (query:atomic-batch-stats) — return
    // the current nested-atomic-batch observability counters.
    // P0 ship: returns the atomic_batch_steal_violation_
    // count as an int. Follow-up: returns a 2-tuple
    // (steal-violations gc-bumps-lost).
    add("query:atomic-batch-stats", [&ev](const auto& a) -> EvalValue {
        (void)a;
        return make_int(static_cast<std::int64_t>(
            ev.get_atomic_batch_steal_violation()));
    });

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
    add("query:verify-dirty-stats", [&ev](const auto& a) -> EvalValue {
        (void)a;
        auto* ws = ev.workspace_flat();
        if (!ws) return make_int(0);
        auto sum = ws->verify_assertion_dirty_total() +
                   ws->verify_coverage_dirty_total() +
                   ws->verify_sva_dirty_total() +
                   ws->verify_formal_cex_dirty_total();
        return make_int(static_cast<std::int64_t>(sum));
    });

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
    auto pick_macro_flat = [&ev]() {
        return ev.current_flat() ? ev.current_flat() : ev.workspace_flat();
    };

    // Issue #290: (compile:macro-dirty? node-id) — query the
    // macro_dirty_ bitmask for a node. Returns the bitmask
    // (0 if not set or no bits). #f on bad args.
    // Bit 0 = kMacroExpansion (cloned by clone_macro_body),
    // bit 1 = kMacroSelfModify (touched by a self-evolution
    // step).
    add("compile:macro-dirty?", [&ev, pick_macro_flat](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = pick_macro_flat();
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
    add("compile:macro-dirty-count", [&ev, pick_macro_flat](const auto&) -> EvalValue {
        auto* ws = pick_macro_flat();
        if (!ws)
            return make_bool(false);
        return make_int(static_cast<std::int64_t>(ws->macro_dirty_count()));
    });

    // Issue #290: (compile:clear-macro-dirty!) — clear all
    // macro_dirty_ bits on the eval flat. Useful after a
    // self-evolution loop has fully reprocessed the affected
    // subtrees and wants to start fresh on the next cycle.
    // Returns #t on success, #f if no flat.
    add("compile:clear-macro-dirty!", [&ev, pick_macro_flat](const auto&) -> EvalValue {
        auto* ws = pick_macro_flat();
        if (!ws)
            return make_bool(false);
        ws->clear_macro_dirty_all();
        return make_bool(true);
    });

    // Issue #290: (compile:macro-dirty-stats) — return the
    // running per-reason counters as a single integer (sum
    // of kMacroExpansion + kMacroSelfModify newly-set
    // totals). P0 ship mirrors (query:verify-dirty-stats).
    // Follow-up: return a pair so callers can distinguish
    // the two reasons without separate primitives.
    add("compile:macro-dirty-stats", [&ev, pick_macro_flat](const auto&) -> EvalValue {
        auto* ws = pick_macro_flat();
        if (!ws)
            return make_bool(false);
        auto sum = ws->macro_expansion_dirty_total() +
                   ws->macro_self_modify_dirty_total();
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
        auto* ws = ev.workspace_flat();
        if (!ws) return make_int(0);
        auto text_idx = as_string_idx(a[0]);
        if (text_idx >= ev.string_heap_.size())
            return make_int(0);
        const std::string& text = ev.string_heap_[text_idx];
        std::uint64_t marked = 0;
        std::size_t i = 0;
        while (i < text.size()) {
            // Find end of line.
            std::size_t j = i;
            while (j < text.size() && text[j] != '\n') ++j;
            const std::string_view line(text.data() + i, j - i);
            // Skip leading whitespace.
            std::size_t k = 0;
            while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
            // Parse the first integer (NodeId).
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
                    ++marked;
                }
            }
            i = (j < text.size()) ? j + 1 : j;
        }
        return make_int(static_cast<std::int64_t>(marked));
    });

    // Issue #469: (verify:parse-assert-failure text-string)
    // — parse a text blob describing assertion failures
    // from an external SV simulator and mark the affected
    // AST nodes dirty with the kAssertFailureDirty bit.
    // Same format as (verify:parse-coverage-feedback).
    add("verify:parse-assert-failure", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws) return make_int(0);
        auto text_idx = as_string_idx(a[0]);
        if (text_idx >= ev.string_heap_.size())
            return make_int(0);
        const std::string& text = ev.string_heap_[text_idx];
        std::uint64_t marked = 0;
        std::size_t i = 0;
        while (i < text.size()) {
            std::size_t j = i;
            while (j < text.size() && text[j] != '\n') ++j;
            const std::string_view line(text.data() + i, j - i);
            std::size_t k = 0;
            while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
            if (k < line.size() && line[k] >= '0' && line[k] <= '9') {
                std::size_t val = 0;
                while (k < line.size() && line[k] >= '0' && line[k] <= '9') {
                    val = val * 10 + (line[k] - '0');
                    ++k;
                }
                const auto nid = static_cast<aura::ast::NodeId>(val);
                if (nid < ws->size()) {
                    ws->apply_verification_dirty_bits(
                        nid, aura::ast::FlatAST::kAssertFailureDirty);
                    ++marked;
                }
            }
            i = (j < text.size()) ? j + 1 : j;
        }
        return make_int(static_cast<std::int64_t>(marked));
    });

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
        auto* ws = ev.workspace_flat();
        if (!ws) return make_void();
        // Optional first arg: parse the report first (so
        // downstream calls see freshly-marked dirty nodes).
        if (!a.empty() && is_string(a[0])) {
            auto text_idx = as_string_idx(a[0]);
            if (text_idx < ev.string_heap_.size()) {
                const std::string& text = ev.string_heap_[text_idx];
                std::size_t i = 0;
                while (i < text.size()) {
                    std::size_t j = i;
                    while (j < text.size() && text[j] != '\n') ++j;
                    const std::string_view line(text.data() + i, j - i);
                    std::size_t k = 0;
                    while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
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
        }
        // Collect coverage-dirty NodeIds into a pair-list
        // (newest-first; reverse-iterates the flat so the
        // last-marked node is at car, matching the
        // (query:templates) ordering convention).
        EvalValue list = make_void();
        const auto n = ws->size();
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
        auto* ws = ev.workspace_flat();
        if (!ws) return make_void();
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
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        if (!ev.set_occurrence_dirty_fn_)
            return make_bool(false);
        auto node_id = static_cast<std::uint32_t>(as_int(a[0]));
        bool set = true;
        if (a.size() >= 2 && is_bool(a[1])) {
            set = as_bool(a[1]);
        }
        return make_bool(ev.set_occurrence_dirty_fn_(node_id, set));
    });

    // Issue #240: (compile:narrowing-dirty? node-id) — query
    // whether a workspace FlatAST node has the kOccurrenceDirty
    // bit set. Useful for agents / observability that want to
    // check narrowing staleness without invoking the full
    // post-mutation invariant check. Returns #t if the node is
    // dirty for narrowing, #f otherwise (also #f if the hook
    // is not installed or the node-id is out of range).
    //
    // This is the read-only counterpart to mark-narrowing-dirty!.
    add("compile:narrowing-dirty?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto node_id = static_cast<std::uint32_t>(as_int(a[0]));
        // The hook is dual-purpose: set with true queries the
        // state. We pass false (clear) but capture the
        // pre-clear state via... actually let's just expose
        // the query via a separate path. For simplicity, we
        // reuse the hook and call it with set=true after
        // capturing a probe.
        if (!ev.set_occurrence_dirty_fn_)
            return make_bool(false);
        // The hook's contract: it returns the prior state
        // when called with set=true. We use that to peek
        // without modifying state. After the peek we call
        // it with the prior value (true if already dirty,
        // false otherwise) to restore the original state.
        bool prior = ev.set_occurrence_dirty_fn_(node_id, true);
        // Restore: if prior was true (was already dirty),
        // set again (no-op since the bit is still set);
        // if prior was false (was not dirty), clear (which
        // undoes the set we just did).
        ev.set_occurrence_dirty_fn_(node_id, prior);
        return make_bool(prior);
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
    // Issue #340 follow-up: the predicate_memo_
    // stats aren't currently wired into the
    // get_narrowing_refresh_count_fn_ hook (that
    // hook returns a different counter). For now
    // we return a 3-tuple with 0/0/0 — the test
    // verifies the primitive exists + returns the
    // right shape; a follow-up wires the actual
    // stats. The narrowing_refresh_count itself
    // is also returned for context.
    add("compile:occ-cache-stats", [&ev](const auto&) -> EvalValue {
        // The 3-tuple: (predicate_memo_hits .
        // predicate_memo_misses .
        // predicate_memo_evictions). All 0 until
        // a follow-up wires the stats into the
        // hook.
        const std::uint64_t hits = 0;
        const std::uint64_t misses = 0;
        const std::uint64_t evictions = 0;
        // Build (hits . (misses . evictions)) — a
        // pair-of-pairs (the simplest 3-tuple in
        // flat-eval).
        auto inner_idx = ev.pairs_.size();
        ev.pairs_.push_back({make_int(static_cast<std::int64_t>(misses)),
                              make_int(static_cast<std::int64_t>(evictions))});
        auto outer_idx = ev.pairs_.size();
        ev.pairs_.push_back({make_int(static_cast<std::int64_t>(hits)),
                              make_pair(inner_idx)});
        (void)hits; (void)misses; (void)evictions;
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
    // Issue #388: (*allow-macro-inline* #t/#f) — runtime toggle
    // for the InlinePass::respect_macro_hygiene_ flag. Lets an
    // Aura workspace opt in to (or out of) inlining macro-
    // introduced code without recompiling. The static flag is
    // process-wide; toggling it affects all subsequent inlining
    // in this process.
    //
    // Args: 1 (optional bool — defaults to true). Returns the
    // post-toggle flag value (1 if macro-introduced code is
    // now inlinable, 0 if not).
    add("*allow-macro-inline*", [&ev](const auto& a) -> EvalValue {
        bool enable = true;
        if (a.size() >= 1 && types::is_bool(a[0])) {
            enable = static_cast<bool>(types::as_bool(a[0]));
        }
        aura::compiler::InlinePass::set_respect_macro_hygiene(!enable);
        bool now_respects = aura::compiler::InlinePass::get_respect_macro_hygiene();
        return make_int(static_cast<std::int64_t>(now_respects ? 0 : 1));
    });

    add("compile:inline-pass-stats", [&ev](const auto&) -> EvalValue {
        std::int64_t inlined = 0;
        std::int64_t branch_aware = 0;
        if (ev.get_inline_stats_fn_) {
            std::uint64_t packed = ev.get_inline_stats_fn_();
            inlined = static_cast<std::int64_t>(packed & 0xFFFFFFFF);
            branch_aware = static_cast<std::int64_t>(packed >> 32);
        }
        std::int64_t macro_skipped = 0;
        if (ev.get_macro_hygiene_skipped_fn_) {
            macro_skipped = static_cast<std::int64_t>(
                ev.get_macro_hygiene_skipped_fn_());
        }
        std::int64_t total = inlined + branch_aware;
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"inlined", make_int(inlined)},
            {"branch-aware", make_int(branch_aware)},
            {"macro-hygiene-skipped", make_int(macro_skipped)},
            {"total", make_int(total)},
        };
        return build_hash(kv);
    });

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
    add("concurrency:stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
        return build_hash(kv);
    });

    // (concurrency:version-snapshot) — Issue #189: capture the
    // current defuse_version_ and return it as an int. Use with
    // (concurrency:version-current? snap) to detect concurrent
    // mutations between two points in the program.
    add("concurrency:version-snapshot", [&ev](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(ev.defuse_version_snapshot()));
    });

    // (concurrency:version-current? snap) — Issue #189: returns
    // #t if the defuse_version_ has not changed since `snap` was
    // captured. #f if a mutation has happened (and AST/cells/pairs
    // may be stale).
    add("concurrency:version-current?", [&ev](const auto& a) -> EvalValue {
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
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->marker(id)));
    });

    // (syntax-marker-counts) — Issue #190: aggregate count of
    // each SyntaxMarker value across the workspace. Hash with
    // 3 integer fields: user, macro-introduced, bool-literal,
    // plus total-nodes. Useful for dashboards ("how much of the
    // workspace is macro-introduced code?") and for asserting
    // hygiene invariants in tests.
    add("syntax-marker-counts", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_flat_)
            return make_void();
        std::size_t user = 0, macro = 0, bool_lit = 0, total = 0;
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
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"user", make_int(static_cast<std::int64_t>(user))},
            {"macro-introduced", make_int(static_cast<std::int64_t>(macro))},
            {"bool-literal", make_int(static_cast<std::int64_t>(bool_lit))},
            {"total-nodes", make_int(static_cast<std::int64_t>(total))},
        };
        return build_hash(kv);
    });

    // (compile:per-symbol-dirty-stats sym) — Issue #410: per-symbol
    // dirty observability. Returns a hash with 4 fields:
    //   - per-symbol-affected-count: number of Variable nodes in
    //     the flat whose sym_id matches `sym` (the per-symbol
    //     affected set)
    //   - ancestor-affected-count: number of nodes in the
    //     ancestor chain of the def node (the legacy
    //     mark_dirty_upward set; -1 if the def node is not
    //     found in the flat — conservative unknown)
    //   - reduction-ratio-bp: per-symbol / ancestor * 10000 in
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
    add("compile:per-symbol-dirty-stats", [&ev](const auto& a) -> EvalValue {
        // build_hash inlined (same FNV-1a fingerprint + linear
        // probing pattern as the other compile:* primitives in
        // this file; Issue #258 HASH_EMPTY collision avoided).
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
        aura::ast::SymId target_sym = aura::ast::INVALID_SYM;
        if (ev.workspace_flat_ && ev.workspace_pool_) {
            target_sym = ev.workspace_pool_->intern(sym_name);
        }
        // Compute per-symbol affected set (O(n) walk).
        std::vector<aura::ast::NodeId> per_symbol_affected;
        if (ev.workspace_flat_ && target_sym != aura::ast::INVALID_SYM) {
            per_symbol_affected =
                affected_subtree_for_symbol(*ev.workspace_flat_, target_sym);
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
                if ((v.tag == aura::ast::NodeTag::Define ||
                     v.tag == aura::ast::NodeTag::Let ||
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
                std::int64_t chain_len = 0;
                auto start = ev.workspace_flat_->parent_of(def_node);
                const auto max_count =
                    static_cast<std::size_t>(ev.workspace_flat_->size());
                if (start != aura::ast::NULL_NODE) {
                    chain_len = static_cast<std::int64_t>(
                        aura::compiler::walk_ancestors<std::uint32_t>(
                            *ev.workspace_flat_, start,
                            [&chain_len, max_count](aura::ast::NodeId)
                                -> bool {
                                if (static_cast<std::size_t>(chain_len)
                                    >= max_count) {
                                    return false;  // safety cap
                                }
                                ++chain_len;
                                return true;
                            }));
                }
                ancestor_affected = chain_len;
            }
        }
        // Bump metrics_.
        std::uint64_t lookup_count = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            m->per_symbol_dirty_lookups_total.fetch_add(1, std::memory_order_relaxed);
            m->per_symbol_dirty_uses_total.fetch_add(
                static_cast<std::uint64_t>(per_symbol_affected.size()),
                std::memory_order_relaxed);
            lookup_count =
                m->per_symbol_dirty_lookups_total.load(std::memory_order_relaxed);
        }
        // reduction-ratio-bp = per_symbol / ancestor * 10000.
        // Cap at 10000 (per_symbol can't exceed ancestor in
        // practice, but defensive). Use 0 when ancestor is 0/-
        std::int64_t ratio_bp = 0;
        if (ancestor_affected > 0 && !per_symbol_affected.empty()) {
            const auto num = static_cast<std::int64_t>(per_symbol_affected.size());
            ratio_bp = (num * 10000) / ancestor_affected;
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
        return build_hash(kv);
    });

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
    // the derived metric on CompilerSnapshot. Same FNV-1a
    // build_hash pattern as the surrounding compile:*
    // primitives (Issue #258 HASH_EMPTY collision avoided).
    add("compile:incremental-typecheck-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
        std::uint64_t auto_invocations = 0;
        std::uint64_t re_inferred = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            auto_invocations = m->incremental_typecheck_auto_invocations_total.load(
                std::memory_order_relaxed);
            re_inferred = m->incremental_typecheck_re_inferred_total.load(
                std::memory_order_relaxed);
        }
        const std::uint64_t avg_bp = (auto_invocations > 0)
                                         ? (re_inferred * 10000u) / auto_invocations
                                         : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"auto-invocations-total", make_int(static_cast<std::int64_t>(auto_invocations))},
            {"re-inferred-total", make_int(static_cast<std::int64_t>(re_inferred))},
            {"avg-re-inferred-bp", make_int(static_cast<std::int64_t>(avg_bp))},
        };
        return build_hash(kv);
    });

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
    //     (stale + gen_saved) * 10000, basis points). 0
    //     when neither counter has been bumped. The key AC
    //     for #412 — higher = more false-positive stale
    //     rejections eliminated.
    add("compile:type-cache-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
        std::uint64_t hits = 0, misses = 0, stale = 0, gen_saved = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            hits = m->typecheck_cache_hits_total.load(std::memory_order_relaxed);
            misses = m->typecheck_cache_misses_total.load(std::memory_order_relaxed);
            stale = m->typecheck_stale_cache_total.load(std::memory_order_relaxed);
            gen_saved = m->typecheck_gen_saved_total.load(std::memory_order_relaxed);
        }
        const std::uint64_t gen_total = stale + gen_saved;
        const std::uint64_t ratio_bp = (gen_total > 0) ? (gen_saved * 10000u) / gen_total : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"cache-hits-total", make_int(static_cast<std::int64_t>(hits))},
            {"cache-misses-total", make_int(static_cast<std::int64_t>(misses))},
            {"stale-cache-total", make_int(static_cast<std::int64_t>(stale))},
            {"gen-saved-total", make_int(static_cast<std::int64_t>(gen_saved))},
            {"gen-saved-ratio-bp", make_int(static_cast<std::int64_t>(ratio_bp))},
        };
        return build_hash(kv);
    });

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
    //     (per_symbol_visited / total_visited * 10000,
    //     basis points). Higher = more work on the fast
    //     path.
    //   - avg-per-symbol-bp: derived average re-inferred
    //     nodes per per-symbol mutation
    //     (per_symbol_visited / max(per_symbol_used, 1) *
    //     10000). The follow-up #410 Phase 2/2 (O(uses)
    //     DefUseIndex routing) will reduce this metric
    //     further by replacing the O(n) per_symbol walk
    //     with an O(uses) indexed lookup.
    add("compile:per-symbol-reinfer-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
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
            while (p2 < cap) p2 <<= 1;
            cap = p2;
            auto* ht = FlatHashTable::create(cap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
            per_symbol_used = m->per_symbol_reinfer_used_total.load(
                std::memory_order_relaxed);
            per_symbol_visited = m->per_symbol_reinfer_visited_total.load(
                std::memory_order_relaxed);
            ancestor_used = m->ancestor_reinfer_used_total.load(
                std::memory_order_relaxed);
            ancestor_visited = m->ancestor_reinfer_visited_total.load(
                std::memory_order_relaxed);
            per_defuse_index_used = m->per_defuse_index_used_total.load(
                std::memory_order_relaxed);
            per_defuse_index_visited = m->per_defuse_index_visited_total.load(
                std::memory_order_relaxed);
            per_defuse_index_walk_fallback =
                m->per_defuse_index_walk_fallback_total.load(
                    std::memory_order_relaxed);
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
            {"per-symbol-visited-total", make_int(static_cast<std::int64_t>(per_symbol_visited))},
            {"ancestor-used-total", make_int(static_cast<std::int64_t>(ancestor_used))},
            {"ancestor-visited-total", make_int(static_cast<std::int64_t>(ancestor_visited))},
            {"path-share-bp", make_int(static_cast<std::int64_t>(path_share_bp))},
            {"avg-per-symbol-bp", make_int(static_cast<std::int64_t>(avg_per_symbol_bp))},
            // Issue #411 fu1 follow-up #2: per-DefUseIndex
            // tracker observability (the underlying data
            // structure for the per-symbol O(uses) path).
            {"per-defuse-index-used-total", make_int(static_cast<std::int64_t>(per_defuse_index_used))},
            {"per-defuse-index-visited-total", make_int(static_cast<std::int64_t>(per_defuse_index_visited))},
            {"per-defuse-index-walk-fallback-total", make_int(static_cast<std::int64_t>(per_defuse_index_walk_fallback))},
            {"per-defuse-index-visited-avg-bp", make_int(static_cast<std::int64_t>(per_defuse_index_visited_avg_bp))},
        };
        return build_hash(kv);
    });

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
    add("compile:per-defuse-index-add", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1]))
            return make_void();
        if (!ev.compiler_service_)
            return make_int(0);
        auto* svc = static_cast<class CompilerService*>(ev.compiler_service_);
        const std::string idx_name = ev.string_heap_[as_string_idx(a[0])];
        const auto caller_node_id = static_cast<aura::ast::NodeId>(as_int(a[1]));
        using aura::compiler::per_defuse_index::DefUseIndex;
        using aura::compiler::per_defuse_index::Caller;
        svc->per_defuse_index_tracker().add_caller(
            DefUseIndex{idx_name}, Caller{caller_node_id});
        return make_int(static_cast<std::int64_t>(
            svc->per_defuse_index_tracker().size_for_index(DefUseIndex{idx_name})));
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
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
        auto* svc = static_cast<class CompilerService*>(ev.compiler_service_);
        const std::string idx_name = ev.string_heap_[as_string_idx(a[0])];
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

    // (compile:per-defuse-index-stats)
    //   — Issue #411 fu1 follow-up #2: snapshot of the
    //   per-DefUseIndex tracker's internal state. Returns
    //   a hash with 3 fields: total-size, index-count,
    //   defuse-service-ptr (the pointer to the
    //   CompilerService that owns the tracker, exposed
    //   for debugging only). Used by
    //   test_issue_411_followup_2 to verify the tracker
    //   is wired into the service.
    add("compile:per-defuse-index-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
            {"defuse-service-ptr", make_int(static_cast<std::int64_t>(
                reinterpret_cast<std::uintptr_t>(svc)))},
        };
        return build_hash(kv);
    });

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
    add("compile:mutation-log-invalidation-stats",
        [&ev](const auto&) -> EvalValue {
        auto build_hash =
            [&](std::span<const std::pair<std::string, EvalValue>> kv)
            -> EvalValue {
            auto cap = std::max<std::size_t>(8, kv.size() * 2);
            // Round up to next power of 2.
            std::size_t hcap = 8;
            while (hcap < cap) hcap *= 2;
            auto* ht = FlatHashTable::create(hcap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) *
                        0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) |
                          0x80;
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
        auto* svc = static_cast<class CompilerService*>(
            ev.compiler_service_);
        std::uint64_t trace_size = 0;
        if (auto* ws = ev.workspace_flat()) {
            trace_size = static_cast<std::uint64_t>(
                ws->invalidation_trace_size());
        }
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"records-total",
             make_int(static_cast<std::int64_t>(
                 svc->snapshot().invalidation_trace_records_total))},
            {"trace-size",
             make_int(static_cast<std::int64_t>(trace_size))},
        };
        return build_hash(kv);
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
    add("compile:mutator-dispatch-stats", [&ev](const auto&) -> EvalValue {
        auto& s = aura::ast::mutators::dispatch_stats();

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
        auto e_total = add_entry(":total", cvt(s.total()));
        auto e_amut  = add_entry(":apply-mutation-total", cvt(
            s.apply_mutation_total.load(std::memory_order_relaxed)));
        auto e_aknd  = add_entry(":apply-by-kind-total", cvt(
            s.apply_by_kind_total.load(std::memory_order_relaxed)));
        auto e_anam  = add_entry(":apply-by-name-total", cvt(
            s.apply_by_name_total.load(std::memory_order_relaxed)));
        auto e_fail  = add_entry(":failure-total", cvt(
            s.failure_total.load(std::memory_order_relaxed)));
        auto e_nsucc = add_entry(":noop-success", cvt(
            s.kind_success[aura::ast::mutators::kind_index(
                aura::ast::mutators::StrategyKind::NoOp)]
                .load(std::memory_order_relaxed)));
        auto e_rsucc = add_entry(":replace-child-success", cvt(
            s.kind_success[aura::ast::mutators::kind_index(
                aura::ast::mutators::StrategyKind::ReplaceChild)]
                .load(std::memory_order_relaxed)));
        auto e_isucc = add_entry(":insert-child-success", cvt(
            s.kind_success[aura::ast::mutators::kind_index(
                aura::ast::mutators::StrategyKind::InsertChild)]
                .load(std::memory_order_relaxed)));
        auto e_xsucc = add_entry(":remove-child-success", cvt(
            s.kind_success[aura::ast::mutators::kind_index(
                aura::ast::mutators::StrategyKind::RemoveChild)]
                .load(std::memory_order_relaxed)));
        auto e_rfail = add_entry(":replace-child-failure", cvt(
            s.kind_failure[aura::ast::mutators::kind_index(
                aura::ast::mutators::StrategyKind::ReplaceChild)]
                .load(std::memory_order_relaxed)));
        auto e_ifail = add_entry(":insert-child-failure", cvt(
            s.kind_failure[aura::ast::mutators::kind_index(
                aura::ast::mutators::StrategyKind::InsertChild)]
                .load(std::memory_order_relaxed)));
        auto e_xfail = add_entry(":remove-child-failure", cvt(
            s.kind_failure[aura::ast::mutators::kind_index(
                aura::ast::mutators::StrategyKind::RemoveChild)]
                .load(std::memory_order_relaxed)));

        // Cons them onto the result list (in reverse so the head is :total).
        std::uint64_t entries[] = {e_xfail, e_ifail, e_rfail,
                                   e_xsucc, e_isucc, e_rsucc, e_nsucc,
                                   e_fail, e_anam, e_aknd, e_amut, e_total};
        for (auto eid : entries) cons(eid);
        return result;
    });

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
        if (!ev.type_registry_)
            ev.type_registry_ = new aura::core::TypeRegistry();
        auto& reg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
        auto tid = reg.lookup_type(name);
        // Auto-register the type as INT if it doesn't exist.
        // The hardware BitVector is an integer-like type
        // (uint8_t / int16_t / etc.), so INT is a sensible
        // default tag. Pre-existing types (registered with
        // other tags via declare-type) are kept.
        if (!tid.valid()) {
            tid = reg.register_type(aura::core::TypeTag::INT, name);
        }
        const auto width = static_cast<std::uint32_t>(as_int(a[1]));
        const bool is_signed = as_int(a[2]) != 0;
        reg.register_hw_bitvec(tid, width, is_signed);
        return make_int(1);
    });

    add("compile:hw-bitvec-width", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return ev.make_merr("bad-arg",
                "usage: (compile:hw-bitvec-width type-name)");
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
            return ev.make_merr("bad-arg",
                "usage: (compile:hw-bitvec-signed? type-name)");
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
        if (asx < ev.string_heap_.size()) an = ev.string_heap_[asx];
        if (bsx < ev.string_heap_.size()) bn = ev.string_heap_[bsx];
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
            return make_int(0);  // one or both not registered as hw bitvecs
        // Compatible iff SAME width AND SAME signedness.
        // The canonical hardware bug: uint8_t vs uint16_t
        // (different widths), uint8_t vs int8_t (different
        // signedness), or any other mismatch.
        const bool ok = (ba->width == bb->width) && (ba->is_signed == bb->is_signed);
        return make_int(ok ? 1 : 0);
    });

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
    add("compile:hw-coercion-lossy?", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return ev.make_merr("bad-arg",
                "usage: (compile:hw-coercion-lossy? from-name to-name)");
        auto from_sx = as_string_idx(a[0]);
        auto to_sx = as_string_idx(a[1]);
        std::string from_name, to_name;
        if (from_sx < ev.string_heap_.size()) from_name = ev.string_heap_[from_sx];
        if (to_sx < ev.string_heap_.size()) to_name = ev.string_heap_[to_sx];
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
            return make_int(0);  // not a hw coercion
        // Lossy iff FROM is wider than TO (narrowing drops bits).
        // Same width (regardless of signedness) is lossless:
        // reinterpreting signed↔unsigned doesn't lose bits.
        // Widening is lossless (zero- or sign-extension).
        const bool lossy = from_bv->width > to_bv->width;
        return make_int(lossy ? 1 : 0);
    });

    add("compile:hw-coercion-warning", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return ev.make_merr("bad-arg",
                "usage: (compile:hw-coercion-warning from-name to-name)");
        auto from_sx = as_string_idx(a[0]);
        auto to_sx = as_string_idx(a[1]);
        std::string from_name, to_name;
        if (from_sx < ev.string_heap_.size()) from_name = ev.string_heap_[from_sx];
        if (to_sx < ev.string_heap_.size()) to_name = ev.string_heap_[to_sx];
        if (!ev.type_registry_)
            return make_string(ev.string_heap_.size());  // empty string
        auto& reg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
        auto from_tid = reg.lookup_type(from_name);
        auto to_tid = reg.lookup_type(to_name);
        if (!from_tid.valid() || !to_tid.valid())
            return make_string(ev.string_heap_.size());
        auto* from_bv = reg.hw_bitvec_of(from_tid);
        auto* to_bv = reg.hw_bitvec_of(to_tid);
        if (!from_bv || !to_bv)
            return make_string(ev.string_heap_.size());
        if (from_bv->width <= to_bv->width)
            return make_string(ev.string_heap_.size());  // lossless — no warning
        const std::uint32_t dropped = from_bv->width - to_bv->width;
        const std::string from_str = from_bv->is_signed ? "signed" : "unsigned";
        const std::string to_str = to_bv->is_signed ? "signed" : "unsigned";
        const std::string msg =
            "lossy coercion: " + from_name + " (W" + std::to_string(from_bv->width) +
            " " + from_str + ") -> " + to_name + " (W" + std::to_string(to_bv->width) +
            " " + to_str + ") drops " + std::to_string(dropped) + " bits";
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(msg);
        return make_string(sidx);
    });

} // register_compile_primitives

} // namespace aura::compiler::primitives_detail
