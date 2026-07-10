// evaluator_primitives_compile_01.cpp — Issue #909: peeled compile registration
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
    add("compile:occurrence-typing-stats", [&ev](const auto&) -> EvalValue {
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
            {"applied-total", make_int(static_cast<std::int64_t>(snap.narrowing_applied_total))},
            {"skipped-total", make_int(static_cast<std::int64_t>(snap.narrowing_skipped_total))},
            {"reanalyzed-total",
             make_int(static_cast<std::int64_t>(snap.narrowing_reanalyzed_total))},
            {"applied-ratio-bp",
             make_int(static_cast<std::int64_t>(snap.narrowing_applied_ratio_bp))},
        };
        return build_hash(kv);
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
    add("compile:and-or-precision-stats", [&ev](const auto&) -> EvalValue {
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
            {"meet-uses-total", make_int(static_cast<std::int64_t>(snap.and_or_meet_uses_total))},
            {"join-uses-total", make_int(static_cast<std::int64_t>(snap.and_or_join_uses_total))},
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
    add("compile:occurrence-dirty-stats", [&ev](const auto&) -> EvalValue {
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
    add("compile:schema-cache-stats", [&ev](const auto&) -> EvalValue {
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
            {"lookups-total", make_int(static_cast<std::int64_t>(snap.schema_cache_lookups_total))},
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
    add("compile:constraint-dep-stats", [&ev](const auto&) -> EvalValue {
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
    add("compile:constraint-solver-stats", [&ev](const auto&) -> EvalValue {
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
            {"unify-total", make_int(static_cast<std::int64_t>(snap.consistent_unify_total))},
            {"subtype-total", make_int(static_cast<std::int64_t>(snap.consistent_subtype_total))},
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
    add("query:constraint-stats", [&ev](const auto&) -> EvalValue {
        if (!ev.compiler_service_)
            return make_int(0);
        auto* svc = static_cast<class CompilerService*>(ev.compiler_service_);
        const auto snap = svc->snapshot();
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
    add("compile:let-poly-stats", [&ev](const auto&) -> EvalValue {
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
            {"register-total", make_int(static_cast<std::int64_t>(snap.poly_register_total))},
            {"dedup-hits-total", make_int(static_cast<std::int64_t>(snap.poly_dedup_hits_total))},
            {"instantiate-total", make_int(static_cast<std::int64_t>(snap.poly_instantiate_total))},
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
    add("compile:dirty-impact-stats", [&ev](const auto&) -> EvalValue {
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
            {"should-relower-total",
             make_int(static_cast<std::int64_t>(snap.should_relower_total))},
            {"affected-subtree-total",
             make_int(static_cast<std::int64_t>(snap.affected_subtree_total))},
            {"trigger-rate-bp", make_int(static_cast<std::int64_t>(snap.dirty_trigger_rate_bp))},
        };
        return build_hash(kv);
    });
}

} // namespace aura::compiler::primitives_detail
