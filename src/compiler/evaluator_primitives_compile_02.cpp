// evaluator_primitives_compile_02.cpp — Issue #909: peeled compile registration
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
    add("compile:type-dep-graph-stats", [&ev](const auto&) -> EvalValue {
        if (!ev.compiler_service_)
            return make_int(0);
        auto* svc = static_cast<class CompilerService*>(ev.compiler_service_);
        auto snap = svc->snapshot();
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"lookups-total", make_int(static_cast<std::int64_t>(snap.type_dep_graph_lookups))},
            {"hits-total", make_int(static_cast<std::int64_t>(snap.type_dep_graph_hits))},
            {"size", make_int(static_cast<std::int64_t>(snap.type_dep_graph_size))},
            {"hit-rate-bp", make_int(static_cast<std::int64_t>(snap.type_dep_graph_hit_rate_bp))},
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
    add("compile:match-narrowing-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
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
        auto snap = svc->snapshot();
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
    add("compile:narrowing-blame-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
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
        auto snap = svc->snapshot();
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
    add("ast:generation-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
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
        auto snap = svc->snapshot();
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"current-generation", make_int(static_cast<std::int64_t>(snap.current_generation))},
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
            {"current-wrap-epoch", make_int(static_cast<std::int64_t>(snap.current_wrap_epoch))},
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
        auto snap = svc->snapshot();
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
}

// Issue #909 compile part 22 (orig 1766-1817)
void CompilePrims::register_compile_p22(PrimRegistrar add, Evaluator& ev) {

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
}

// Issue #909 compile part 23 (orig 1818-1888)
void CompilePrims::register_compile_p23(PrimRegistrar add, Evaluator& ev) {

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

} // namespace aura::compiler::primitives_detail
