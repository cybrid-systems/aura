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

}

} // namespace aura::compiler::primitives_detail
