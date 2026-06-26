// evaluator_primitives_compile.cpp — P0 step 14: compile:* / concurrency:* / syntax-marker primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "runtime_shared.h"
#include "observability_metrics.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;
import aura.compiler.service;
import aura.compiler.type_checker;

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
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"children-call-count", make_int(static_cast<std::int64_t>(children_calls))},
            {"parent-of-call-count", make_int(static_cast<std::int64_t>(parent_calls))},
            {"mark-dirty-upward-call-count", make_int(static_cast<std::int64_t>(dirty_calls))},
            {"mark-dirty-total-nodes", make_int(static_cast<std::int64_t>(dirty_nodes))},
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
    add("compile:inline-pass-stats", [&ev](const auto&) -> EvalValue {
        std::int64_t inlined = 0;
        std::int64_t branch_aware = 0;
        if (ev.get_inline_stats_fn_) {
            std::uint64_t packed = ev.get_inline_stats_fn_();
            inlined = static_cast<std::int64_t>(packed & 0xFFFFFFFF);
            branch_aware = static_cast<std::int64_t>(packed >> 32);
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
                std::int64_t chain_len = 0;
                auto cur = def_node;
                std::size_t safety = 0;
                while (cur != aura::ast::NULL_NODE &&
                       cur < ev.workspace_flat_->size() &&
                       safety++ < ev.workspace_flat_->size()) {
                    ++chain_len;
                    cur = ev.workspace_flat_->parent_of(cur);
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
        std::uint64_t per_symbol_used = 0, per_symbol_visited = 0;
        std::uint64_t ancestor_used = 0, ancestor_visited = 0;
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
        }
        const std::uint64_t total_visited = per_symbol_visited + ancestor_visited;
        const std::uint64_t path_share_bp =
            (total_visited > 0) ? (per_symbol_visited * 10000u) / total_visited : 0;
        const std::uint64_t avg_per_symbol_bp =
            (per_symbol_used > 0) ? (per_symbol_visited * 10000u) / per_symbol_used : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"per-symbol-used-total", make_int(static_cast<std::int64_t>(per_symbol_used))},
            {"per-symbol-visited-total", make_int(static_cast<std::int64_t>(per_symbol_visited))},
            {"ancestor-used-total", make_int(static_cast<std::int64_t>(ancestor_used))},
            {"ancestor-visited-total", make_int(static_cast<std::int64_t>(ancestor_visited))},
            {"path-share-bp", make_int(static_cast<std::int64_t>(path_share_bp))},
            {"avg-per-symbol-bp", make_int(static_cast<std::int64_t>(avg_per_symbol_bp))},
        };
        return build_hash(kv);
    });

} // register_compile_primitives

} // namespace aura::compiler::primitives_detail
