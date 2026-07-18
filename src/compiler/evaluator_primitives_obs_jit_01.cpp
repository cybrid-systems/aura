// evaluator_primitives_obs_jit_01.cpp — Issue #909: peeled domain registration from observability
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
import aura.compiler.service;

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

// Issue #909 part 8 (orig lines 12380-12480)
void ObservabilityPrims::register_jit_p8(PrimRegistrar add, Evaluator& ev) {

    // (query:cxx26-invariants) — Issue #431: a 5-field
    // hash summarizing the codebase's C++26 zero-overhead
    // invariant density. The numbers are compile-time
    // constants tied to the source — they don't move at
    // runtime. The AI Agent reads the count to detect
    // drift (a regression in invariant coverage is a
    // "the codebase lost some compile-time safety" signal).
    //
    // Field list (5 total):
    //   - consteval-invariants: # static_assert blocks
    //     in src/core/cxx26_invariants.ixx (currently 22
    //     — SmallObjectPool tier + Value tag + concept
    //     self-check groups)
    //   - concept-count: # Concepts in src/core/concepts.ixx
    //     (currently 13 — NodeHandle, ASTContainer,
    //     Mutator, ArenaAllocator, Queryable, AuraInvocable,
    //     RangeOf, AnyRange, SymbolInterner, StableNodeRefLike
    //     + Issue #431's SoAColumnar, DirtyPropagator,
    //     ShapeDispatchable)
    //   - contract-hot-paths: # Contract pre/post/assert
    //     sites in Arena + Value + SoA + Pass (sum across
    //     these 4 hot files, currently 26 — issue #431
    //     scope-limited ship doesn't add new Contracts
    //     beyond what was already there; follow-up issues
    //     will)
    //   - concept-self-checks: # static_asserts in
    //     cxx26_invariants.ixx that verify Concepts
    //     compile (currently 1 — the std::vector<int>
    //     check)
    //   - concept-targets-documented: # "Target sites:"
    //     comments in concepts.ixx (currently 9 — each
    //     concept has a doc comment listing the
    //     consumers / future consumers)
    //
    // The contract-hot-paths count is approximate —
    // see ContractHotPathCount() for the exact
    // grep-and-sum. If a future issue adds Contracts
    // to value.ixx or ir_soa.ixx, this number will
    // jump. The AI Agent monitors the count.
    ObservabilityPrims::register_stats_impl(
        "query:cxx26-invariants", [&ev](const auto&) -> EvalValue {
            // Reuse the same build_hash pattern as the
            // closure:stats / soa-dirty-stats primitives.
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
            // Issue #1519: prefer baked constants from cpp26_contract_stats
            // so Agents see live consteval/Contract density without
            // hand-updating stale literals (was 24/26; now 65/48).
            constexpr std::int64_t kConstevalInvariants = aura::core::cpp26::kConstevalChecksTotal;
            constexpr std::int64_t kConceptCount = 13;
            constexpr std::int64_t kContractHotPaths = aura::core::cpp26::kContractHotPathsShipped;
            constexpr std::int64_t kConceptSelfChecks = 1;
            constexpr std::int64_t kConceptTargetsDoc = 9;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"consteval-invariants", make_int(kConstevalInvariants)},
                {"concept-count", make_int(kConceptCount)},
                {"contract-hot-paths", make_int(kContractHotPaths)},
                {"concept-self-checks", make_int(kConceptSelfChecks)},
                {"concept-targets-documented", make_int(kConceptTargetsDoc)},
                {"schema", make_int(1519)},
                {"hotpath-contracts-active",
                 make_int(static_cast<std::int64_t>(aura::core::cpp26::hotpath_contracts_1519_active
                                                        .load(std::memory_order_relaxed)))},
                {"contract-violation-hotpath",
                 make_int(static_cast<std::int64_t>(
                     aura::core::cpp26::contract_violation_hotpath_count.load(
                         std::memory_order_relaxed)))},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 9 (orig lines 12481-12583)
void ObservabilityPrims::register_jit_p9(PrimRegistrar add, Evaluator& ev) {

    // (query:edsl-readiness) — Issue #440 / #1142: a single
    // hash that aggregates the curated EDSL production readiness
    // signals. Field list must match the kv vector below (6 fields):
    //   - closure-stale-refresh:       closure_bridge refreshes (#531)
    //   - linear-check-pass:           linear ownership fast-path (#149)
    //   - atomic-batch-commits:        MutationBoundaryGuard commits (#241)
    //   - stable-ref-invalidations:    StableNodeRef is_valid misses (#417)
    //   - occurrence-stale-refreshes:  occurrence stale refreshes
    //   - dirty-block-rate:            live per-block dirty % (#429, 0..100)
    //
    // Comment/code name alignment fixed in #1142 (was mismatched
    // "6 total" header listing 8 differently-named fields).
    ObservabilityPrims::register_stats_impl(
        "query:edsl-readiness", [&ev](const auto&) -> EvalValue {
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
            std::uint64_t closure_stale = 0, linear_pass = 0;
            std::uint64_t atomic_commits = 0;
            std::uint64_t stable_ref_invalidates = 0;
            std::uint64_t occurrence_stale_refreshes = 0;
            std::int64_t dirty_pct = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                closure_stale = m->closure_stale_refresh_count_.load(std::memory_order_relaxed);
                linear_pass = m->linear_check_pass_count_.load(std::memory_order_relaxed);
                atomic_commits = m->atomic_batch_commits.load(std::memory_order_relaxed);
                stable_ref_invalidates =
                    m->stable_ref_invalidations.load(std::memory_order_relaxed);
                occurrence_stale_refreshes =
                    m->occurrence_stale_refreshes_total.load(std::memory_order_relaxed);
            }
            // dirty-block-rate from #429's get_soa_dirty_stats.
            if (ev.get_soa_dirty_stats_fn_) {
                const auto s = ev.get_soa_dirty_stats_fn_();
                dirty_pct = static_cast<std::int64_t>(s.dirty_block_pct);
            }
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"closure-stale-refresh", make_int(static_cast<std::int64_t>(closure_stale))},
                {"linear-check-pass", make_int(static_cast<std::int64_t>(linear_pass))},
                {"atomic-batch-commits", make_int(static_cast<std::int64_t>(atomic_commits))},
                {"stable-ref-invalidations",
                 make_int(static_cast<std::int64_t>(stable_ref_invalidates))},
                {"occurrence-stale-refreshes",
                 make_int(static_cast<std::int64_t>(occurrence_stale_refreshes))},
                {"dirty-block-rate", make_int(dirty_pct)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 10 (orig lines 12584-12725)
void ObservabilityPrims::register_jit_p10(PrimRegistrar add, Evaluator& ev) {

    // (gc-arena-stats) — Report per-arena allocation. Shows main arena +
    // every per-module arena. Format: "main:0.1MB/8.0MB;json.aura:0.5MB/8.0MB;..."
    // (semicolons separate entries; slashes separate used/capacity within an entry).
    ObservabilityPrims::register_stats_impl("gc-arena-stats", [&ev](const auto&) -> EvalValue {
        std::string out;
        auto fmt_arena = [&](const char* label, std::size_t used, std::size_t cap) {
            auto s = std::format("{}{}:{:.1f}MB/{:.1f}MB", out.empty() ? "" : ";", label,
                                 used / 1048576.0, cap / 1048576.0);
            out += s;
        };
        if (ev.arena_) {
            auto s = ev.arena_->stats();
            fmt_arena("main", s.used, s.capacity);
        }
        if (ev.arena_group_) {
            for (auto& [name, stats] : ev.arena_group_->module_stats()) {
                // Trim path to basename for readability.
                auto slash = name.rfind('/');
                auto short_name = slash == std::string::npos ? name : name.substr(slash + 1);
                fmt_arena(short_name.c_str(), stats.used, stats.capacity);
            }
        }
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(out);
        return types::make_string(sidx);
    });

    // (gc-arena-info) — Return structured per-arena usage as Aura value.
    //
    //   Returns: vector of hashes, each describing one arena:
    //     {name: "main", used: 1.23, capacity: 11.0, used-pct: 11}
    //     {name: "json.aura", used: 0.5, capacity: 8.0, used-pct: 6}
    //     ...
    //
    //   First entry is a summary hash:
    //     {summary: #t, total-arenas: 3, total-used: 1.73, total-capacity: 19.0,
    //      overall-pct: 9}
    //
    //   All numeric values are in megabytes (MB). Pct values are integers 0-100.
    ObservabilityPrims::register_stats_impl("gc-arena-info", [&ev](const auto&) -> EvalValue {
        // Snapshot arena state. Each entry: (short_name, used-MB, cap-MB, pct).
        struct Snap {
            std::string name;
            double used;
            double cap;
            int pct;
        };
        std::vector<Snap> snaps;
        double total_used = 0.0, total_cap = 0.0;
        if (ev.arena_) {
            auto s = ev.arena_->stats();
            double u = s.used / 1048576.0;
            double c = s.capacity / 1048576.0;
            snaps.push_back({"main", u, c, c > 0 ? static_cast<int>(u / c * 100.0) : 0});
            total_used += u;
            total_cap += c;
        }
        if (ev.arena_group_) {
            for (auto& [full_name, stats] : ev.arena_group_->module_stats()) {
                auto slash = full_name.rfind('/');
                auto short_name =
                    slash == std::string::npos ? full_name : full_name.substr(slash + 1);
                double u = stats.used / 1048576.0;
                double c = stats.capacity / 1048576.0;
                snaps.push_back({short_name, u, c, c > 0 ? static_cast<int>(u / c * 100.0) : 0});
                total_used += u;
                total_cap += c;
            }
        }
        int overall = total_cap > 0 ? static_cast<int>(total_used / total_cap * 100.0) : 0;

        // Build a small Swiss-table hash. Inline copy of the (hash ...) primitive
        // pattern. Capacity 8 is enough for the 5-field hashes below.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(16) /* #1141 */;
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto cap = ht->capacity;
            for (auto& [k, v] : kv) {
                // Hash the key with FNV-1a (matches user-level (hash ...) behavior).
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
                // Intern the key as a String EvalValue.
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < cap; ++at) {
                    auto idx = ((h >> 1) + at) & (cap - 1);
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
                    // 8 slots should be enough for the 5-key hashes we build.
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };

        std::vector<EvalValue> result;
        // Summary entry first.
        {
            std::vector<std::pair<std::string, EvalValue>> kv;
            kv.push_back({"summary", make_bool(true)});
            kv.push_back({"total-arenas", make_int(static_cast<std::int64_t>(snaps.size()))});
            kv.push_back({"total-used", make_float(total_used)});
            kv.push_back({"total-capacity", make_float(total_cap)});
            kv.push_back({"overall-pct", make_int(overall)});
            result.push_back(build_hash(kv));
        }
        for (auto& s : snaps) {
            auto name_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(s.name);
            std::vector<std::pair<std::string, EvalValue>> kv;
            kv.push_back({"name", make_string(name_idx)});
            kv.push_back({"used", make_float(s.used)});
            kv.push_back({"capacity", make_float(s.cap)});
            kv.push_back({"used-pct", make_int(s.pct)});
            result.push_back(build_hash(kv));
        }
        auto vidx = ev.vector_heap_.size();
        ev.vector_heap_.push_back(std::move(result));
        return make_vector(vidx);
    });
}

// Issue #1433: map CompilerMetrics field name → facade group.
// Groups: compile | jit | mutate | query | arena | gc | eval | telemetry.
static const char* metrics_group_for_field(std::string_view name) noexcept {
    if (name.size() >= 4 && name.substr(0, 4) == "jit_")
        return "jit";
    if (name.size() >= 6 && name.substr(0, 6) == "arena_")
        return "arena";
    if (name.size() >= 10 && name.substr(0, 10) == "ast_arena_")
        return "arena";
    if (name.size() >= 3 && name.substr(0, 3) == "gc_")
        return "gc";
    if (name.size() >= 6 && name.substr(0, 6) == "mutate")
        return "mutate";
    if (name.size() >= 8 && name.substr(0, 8) == "mutation")
        return "mutate";
    if (name.size() >= 6 && name.substr(0, 6) == "dirty_")
        return "mutate";
    if (name.size() >= 6 && name.substr(0, 6) == "query_")
        return "query";
    if (name.size() >= 8 && name.substr(0, 8) == "pattern_")
        return "query";
    if (name.size() >= 5 && name.substr(0, 5) == "eval_")
        return "eval";
    if (name.size() >= 12 && name.substr(0, 12) == "hotpath_eval")
        return "eval";
    if (name.size() >= 8 && name.substr(0, 8) == "compile_")
        return "compile";
    if (name.size() >= 8 && name.substr(0, 8) == "relower_")
        return "compile";
    if (name.size() >= 12 && name.substr(0, 12) == "module_dirty")
        return "compile";
    if (name.size() >= 10 && name.substr(0, 10) == "invalidate")
        return "compile";
    if (name.size() >= 8 && name.substr(0, 8) == "cascade_")
        return "compile";
    if (name.size() >= 7 && name.substr(0, 7) == "define_")
        return "compile";
    if (name.size() >= 12 && name.substr(0, 12) == "value_define")
        return "compile";
    if (name.size() >= 7 && name.substr(0, 7) == "aot_emi")
        return "compile";
    if (name.size() >= 11 && name.substr(0, 11) == "aot_fallbac")
        return "compile";
    return "telemetry";
}

// P2b / #1433: observability facade for AURA_PRIMITIVES=s0 and full.
// stats:list / stats:count / engine:metrics — no bulk query:*-stats on s0.
void ObservabilityPrims::register_metrics_facade(PrimRegistrar add, Evaluator& ev) {
    // Issue #560: (stats:list)
    add("stats:list", [&ev](const auto&) -> EvalValue {
        const std::vector<std::string>& stats = ObservabilityPrims::stats_primitives();
        EvalValue result = make_void();
        for (auto it = stats.rbegin(); it != stats.rend(); ++it) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(*it);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });

    // Issue #560: (stats:count)
    add("stats:count", [](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(ObservabilityPrims::stats_primitives().size()));
    });

    // Issue #1672: catalog ↔ impl drift diagnostic (facade-only, not public add).
    // (stats:get "stats:drift-check") / (engine:metrics "stats:drift-check")
    ObservabilityPrims::register_stats_impl("stats:drift-check", [&ev](const auto&) -> EvalValue {
        return ObservabilityPrims::stats_drift_check(ev);
    });

    // Issue #1433 / P1a: (engine:metrics [name | :all | :prefix s | :group g])
    add("engine:metrics", [&ev](std::span<const EvalValue> a) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            std::uint64_t need = static_cast<std::uint64_t>(kv.size()) * 2 + 2;
            std::uint64_t cap = 16;
            while (cap < need)
                cap <<= 1;
            auto* ht = FlatHashTable::create(cap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = ::aura::compiler::hash::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::hash::kFnvPrime;
                auto fp = ::aura::compiler::hash::fingerprint(h);
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

        auto kw_str = [&](const EvalValue& v) -> std::string {
            if (!types::is_keyword(v))
                return {};
            auto kidx = types::as_keyword_idx(v);
            if (kidx >= ev.keyword_table_.size())
                return {};
            return ev.keyword_table_[kidx];
        };
        auto as_text = [&](const EvalValue& v) -> std::string {
            if (is_string(v)) {
                auto sidx = as_string_idx(v);
                if (sidx < ev.string_heap_.size())
                    return ev.string_heap_[sidx];
                return {};
            }
            // Keywords used as group tags: :jit / jit
            auto k = kw_str(v);
            if (!k.empty()) {
                if (k.size() > 1 && k[0] == ':')
                    return k.substr(1);
                return k;
            }
            return {};
        };

        const auto& stats = ObservabilityPrims::stats_primitives();

        // Collect CompilerMetrics atomics into group maps (Issue #1433).
        auto dump_metric_groups = [&](CompilerMetrics* m)
            -> std::unordered_map<std::string, std::vector<std::pair<std::string, EvalValue>>> {
            std::unordered_map<std::string, std::vector<std::pair<std::string, EvalValue>>> groups;
            if (!m)
                return groups;
            if (auto* svc = static_cast<CompilerService*>(ev.compiler_service()))
                svc->refresh_env_arena_metrics(*m);
#define AURA_COMPILER_METRICS_FIELD(name)                                                          \
    groups[metrics_group_for_field(#name)].emplace_back(                                           \
        #name, make_int(static_cast<std::int64_t>(m->name.load(std::memory_order_relaxed))));
#include "compiler_metrics_fields.inc"
            return groups;
        };

        auto groups_to_nested =
            [&](std::unordered_map<std::string, std::vector<std::pair<std::string, EvalValue>>>&
                    groups) -> std::vector<std::pair<std::string, EvalValue>> {
            static const char* kOrder[] = {"compile", "jit", "mutate", "query",
                                           "arena",   "gc",  "eval",   "telemetry"};
            std::vector<std::pair<std::string, EvalValue>> out;
            out.reserve(8);
            for (const char* g : kOrder) {
                auto it = groups.find(g);
                if (it == groups.end() || it->second.empty())
                    continue;
                out.emplace_back(g, build_hash(it->second));
            }
            return out;
        };

        // (engine:metrics "query:foo-stats") → single stats value
        // Issue #1439: prefer internal stats impl table (public prims removed).
        if (a.size() >= 1 && is_string(a[0])) {
            auto sidx = as_string_idx(a[0]);
            if (sidx >= ev.string_heap_.size())
                return make_void();
            const std::string& name = ev.string_heap_[sidx];
            if (auto fn = ObservabilityPrims::lookup_stats_impl(name))
                return (*fn)({});
            if (auto fn = ev.primitives_.lookup(name))
                return (*fn)({});
            return make_void();
        }

        // Keyword sub-ops: :all | :prefix s | :group g
        if (a.size() >= 1 && types::is_keyword(a[0])) {
            std::string k = kw_str(a[0]);
            if (!k.empty() && k[0] == ':')
                k = k.substr(1);

            // (engine:metrics :all) → stats name→value hash (+ schema)
            if (k == "all") {
                std::vector<std::pair<std::string, EvalValue>> kv;
                kv.reserve(stats.size() + 4);
                kv.push_back({"schema", make_int(2)});
                kv.push_back({"stats-count", make_int(static_cast<std::int64_t>(stats.size()))});
                for (const auto& name : stats) {
                    std::optional<PrimFn> fn = ObservabilityPrims::lookup_stats_impl(name);
                    if (!fn)
                        fn = ev.primitives_.lookup(name);
                    if (!fn)
                        continue;
                    kv.push_back({name, (*fn)({})});
                }
                return build_hash(kv);
            }

            // (engine:metrics :prefix "query:") → filtered stats / field keys
            if (k == "prefix" && a.size() >= 2) {
                std::string prefix = as_text(a[1]);
                std::vector<std::pair<std::string, EvalValue>> kv;
                kv.reserve(64);
                kv.push_back({"schema", make_int(2)});
                kv.push_back({"prefix", make_string([&] {
                                  auto i = ev.string_heap_.size();
                                  ev.string_heap_.push_back(prefix);
                                  return i;
                              }())});
                // Stats catalog names matching prefix (values when registered).
                for (const auto& name : stats) {
                    if (name.size() < prefix.size() || name.compare(0, prefix.size(), prefix) != 0)
                        continue;
                    std::optional<PrimFn> fn = ObservabilityPrims::lookup_stats_impl(name);
                    if (!fn)
                        fn = ev.primitives_.lookup(name);
                    if (fn)
                        kv.push_back({name, (*fn)({})});
                    else
                        kv.push_back({name, make_void()});
                }
                // Also CompilerMetrics fields whose names start with prefix
                // after treating ':' as '_' for matching (query: → query_).
                std::string field_pfx = prefix;
                for (char& c : field_pfx) {
                    if (c == ':')
                        c = '_';
                }
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                    auto groups = dump_metric_groups(m);
                    for (auto& [g, fields] : groups) {
                        (void)g;
                        for (auto& [fname, val] : fields) {
                            if (fname.size() >= field_pfx.size() &&
                                fname.compare(0, field_pfx.size(), field_pfx) == 0)
                                kv.push_back({fname, val});
                        }
                    }
                }
                return build_hash(kv);
            }

            // (engine:metrics :group "jit" | :jit)
            if (k == "group" && a.size() >= 2) {
                std::string group = as_text(a[1]);
                if (group.empty())
                    return make_void();
                auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
                auto groups = dump_metric_groups(m);
                auto it = groups.find(group);
                if (it == groups.end()) {
                    // Empty group hash still valid
                    return build_hash(std::span<const std::pair<std::string, EvalValue>>{});
                }
                return build_hash(it->second);
            }
        }

        // Default (engine:metrics): nested CompilerMetrics groups + catalog meta.
        // schema 2 = #1433 full facade (groups present).
        std::vector<std::pair<std::string, EvalValue>> kv;
        kv.push_back({"schema", make_int(2)});
        kv.push_back({"stats-count", make_int(static_cast<std::int64_t>(stats.size()))});
        {
            EvalValue names_list = make_void();
            for (auto it = stats.rbegin(); it != stats.rend(); ++it) {
                auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back(*it);
                auto pid = ev.pairs_.size();
                ev.pairs_.push_back({make_string(sidx), names_list});
                names_list = make_pair(pid);
            }
            kv.push_back({"stats-names", names_list});
        }
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            auto groups = dump_metric_groups(m);
            auto nested = groups_to_nested(groups);
            // Total atomic field count for golden tests (≥200).
            std::int64_t field_total = 0;
            for (auto& [g, fields] : groups)
                field_total += static_cast<std::int64_t>(fields.size());
            kv.push_back({"metrics-field-count", make_int(field_total)});
            for (auto& p : nested)
                kv.push_back(std::move(p));
            // Back-compat nested "compiler" key with env/arena snapshot (#1385).
            std::vector<std::pair<std::string, EvalValue>> ck;
            ck.push_back({"env_frames_size_total",
                          make_int(static_cast<std::int64_t>(m->env_frames_size_total.load()))});
            ck.push_back({"env_frames_stale_count",
                          make_int(static_cast<std::int64_t>(m->env_frames_stale_count.load()))});
            ck.push_back({"ast_arena_bytes_in_use",
                          make_int(static_cast<std::int64_t>(m->ast_arena_bytes_in_use.load()))});
            ck.push_back({"ast_arena_upstream_bytes",
                          make_int(static_cast<std::int64_t>(m->ast_arena_upstream_bytes.load()))});
            kv.push_back({"compiler", build_hash(ck)});
        }
        return build_hash(kv);
    });

    // ── Issue #1450 / #1449 Phase 1: engine-level stats:get / stats:prefix ──
    // Mirrors lib/std/stats.aura so agents do not need (require std/stats)
    // to route observability through the facade.
    auto resolve_name = [&ev](const EvalValue& v) -> std::string {
        if (is_string(v)) {
            auto sidx = as_string_idx(v);
            if (sidx < ev.string_heap_.size())
                return ev.string_heap_[sidx];
        }
        return {};
    };

    // (stats:get name) → value | void
    //   name = "all" → same shape as (engine:metrics :all) name→value hash
    //   else lookup register_stats_impl then public Primitives (residual aliases)
    add("stats:get", [&ev, resolve_name](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_void();
        const std::string name = resolve_name(a[0]);
        if (name.empty())
            return make_void();
        if (name == "all") {
            // Delegate: build (engine:metrics :all) by reusing lookup path.
            const auto& stats = ObservabilityPrims::stats_primitives();
            std::vector<std::pair<std::string, EvalValue>> kv;
            kv.reserve(stats.size() + 2);
            kv.push_back({"schema", make_int(2)});
            kv.push_back({"stats-count", make_int(static_cast<std::int64_t>(stats.size()))});
            for (const auto& n : stats) {
                std::optional<PrimFn> fn = ObservabilityPrims::lookup_stats_impl(n);
                if (!fn)
                    fn = ev.primitives_.lookup(n);
                if (fn)
                    kv.push_back({n, (*fn)({})});
            }
            // Minimal hash builder (same layout as engine:metrics).
            std::uint64_t need = static_cast<std::uint64_t>(kv.size()) * 2 + 2;
            std::uint64_t cap = 16;
            while (cap < need)
                cap <<= 1;
            auto* ht = FlatHashTable::create(cap);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = ::aura::compiler::hash::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::hash::kFnvPrime;
                auto fp = ::aura::compiler::hash::fingerprint(h);
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
        }
        if (auto fn = ObservabilityPrims::lookup_stats_impl(name))
            return (*fn)({});
        if (auto fn = ev.primitives_.lookup(name))
            return (*fn)({});
        return make_void();
    });

    // (stats:prefix "query:") → list of catalog names matching prefix
    add("stats:prefix", [&ev, resolve_name](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_void();
        const std::string prefix = resolve_name(a[0]);
        EvalValue result = make_void();
        const auto& stats = ObservabilityPrims::stats_primitives();
        // Build list in reverse so final order matches catalog order.
        std::vector<std::string> matched;
        for (const auto& n : stats) {
            if (prefix.empty() ||
                (n.size() >= prefix.size() && n.compare(0, prefix.size(), prefix) == 0))
                matched.push_back(n);
        }
        for (auto it = matched.rbegin(); it != matched.rend(); ++it) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(*it);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });

    // (engine:surface) — SlimSurface inventory snapshot (#1448/#1449/#1450).
    // Named without *-stats suffix so the freeze gate stays clean.
    // AC mapping: issue text said query:primitive-surface-stats; governance
    // forbids new *-stats public names → engine:surface is the canonical form.
    add("engine:surface", [&ev](const auto&) -> EvalValue {
        const auto& stats = ObservabilityPrims::stats_primitives();
        const auto public_slots = static_cast<std::int64_t>(ev.primitives_.slot_count());
        // Count residual public *stats-like* names still on Primitives
        // (not private register_stats_impl-only). Non-stats catalog entries
        // (e.g. query:dirty-impact) are out of scope for this counter.
        auto is_stats_like = [](std::string_view n) {
            return n.ends_with("-stats") || n.ends_with("-stats-hash") ||
                   n.find("-stats-") != std::string_view::npos;
        };
        std::int64_t public_stats = 0;
        for (const auto& n : stats) {
            if (!is_stats_like(n))
                continue;
            if (ev.primitives_.slot_for_name(n) < ev.primitives_.slot_count())
                ++public_stats;
        }
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"schema", make_int(1)},
            {"public-count", make_int(public_slots)},
            {"stats-catalog-count", make_int(static_cast<std::int64_t>(stats.size()))},
            {"public-stats-remaining", make_int(public_stats)},
            {"target-budget", make_int(420)},
            {"interim-ceiling", make_int(700)},
            {"deprecated-dispatch-total",
             make_int(static_cast<std::int64_t>(ev.deprecated_prim_dispatch_total()))},
        };
        std::uint64_t need = static_cast<std::uint64_t>(kv.size()) * 2 + 2;
        std::uint64_t cap = 16;
        while (cap < need)
            cap <<= 1;
        auto* ht = FlatHashTable::create(cap);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        for (auto& [k, v] : kv) {
            std::uint64_t h = ::aura::compiler::hash::kFnvOffsetBasis;
            for (char c : k)
                h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::hash::kFnvPrime;
            auto fp = ::aura::compiler::hash::fingerprint(h);
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
    });
}

// Issue #909 part 11 (orig lines 12726-12889) — bulk stats after facade
void ObservabilityPrims::register_jit_p11(PrimRegistrar add, Evaluator& ev) {
    register_metrics_facade(add, ev);

    // Issue #728: (query:unified-error-stats) — unified structured
    // error + provenance + recovery observability for AI Agent
    // closed-loop stdlib reliability (non-duplicative with #478
    // (query:primitive-error-stats pair) and #585 (query:primitives-
    // error-stats hash with error_rate / recovery_success / panic-
    // recovery / rollback / contract-violations / recommendation).
    // #728 covers the *unified model* specifically: structured
    // ErrorValue (kind + provenance StableNodeRef + context + recovery
    // hint) hits as separate counters. #585 is coarse error-rate +
    // recovery; #728 is the per-decision-point unified-model signal.
    //
    // Fields (3 + sentinel):
    //   - structured-hits       unified_error_structured_hits_total
    //                           (# of times a primitive emitted a
    //                            structured ErrorValue vs. legacy
    //                            make_primitive_error string-only
    //                            path — proxy for "how much of
    //                            stdlib has migrated to the unified
    //                            model")
    //   - provenance-captured   unified_error_provenance_captured_total
    //                           (# of structured errors that captured
    //                            a StableNodeRef provenance — proxy
    //                            for "how many errors are introspectable
    //                            for AI Agent recovery")
    //   - recovery-success      unified_error_recovery_success_total
    //                           (# of successful rollback + retry
    //                            primitive path firings — complements
    //                            #585's coarse recovery counter with
    //                            structured-error provenance)
    //   - schema == 728
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual unified ErrorValue / EvalValue tagged-error extension
    // + refactor of evaluator_primitives_list.cpp / math.cpp / regex
    // / verify error sites to make_structured_primitive_error(guard,
    // kind, msg, context) + new (primitive:error) / (with-error) /
    // (primitive:try) primitives + Guard.capture auto-provenance +
    // CI lint for legacy make_primitive_error usage + new
    // tests/test_unified_primitive_error_model.cpp harness + SEVA
    // error-resilient closed-loop + primitives_style.md mandate are
    // all follow-up work (each is a dedicated session in
    // evaluator.ixx + primitives_detail.h + evaluator_primitives_*.cpp
    // + Guard + diagnostic + ast.ixx StableNodeRef + new test + SEVA
    // + docs).
    //
    // Issue #728: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=728 + category=general
    // + arity=0 + pure=true (same pattern as #712-#723 / #726).
    ObservabilityPrims::register_stats_impl(
        "query:unified-error-stats", [&ev](const auto&) -> EvalValue {
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
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 4-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t structured_hits =
                m ? static_cast<std::int64_t>(
                        m->unified_error_structured_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t provenance_captured =
                m ? static_cast<std::int64_t>(
                        m->unified_error_provenance_captured_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t recovery_success =
                m ? static_cast<std::int64_t>(
                        m->unified_error_recovery_success_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"structured-hits", make_int(structured_hits)},
                {"provenance-captured", make_int(provenance_captured)},
                {"recovery-success", make_int(recovery_success)},
                {"schema", make_int(728)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 12 (orig lines 12890-13031)
void ObservabilityPrims::register_jit_p12(PrimRegistrar add, Evaluator& ev) {

    // Issue #731: (query:arena-concurrent-compact-stats) — Arena +
    // SoA + EnvFrame concurrent compaction safety observability for
    // production multi-fiber steal/resume + panic checkpoint integration
    // (non-duplicative with #722 arena tier integration stats + #743
    // arena auto-compact policy + fiber safepoint + #647 EnvFrame
    // dual-path + #648 panic checkpoint fiber + #685 auto-compact
    // policy + #604 Arena auto-compact fiber/GC safepoint). #731 covers
    // the *concurrent* safety specifically: scheduler-safepoint
    // coordination + EnvFrame GCEnvWalkFn revalidation + panic-rollback-
    // compact integration + race prevention.
    //
    // Fields (4 + sentinel):
    //   - concurrent-compacts     arena_concurrent_compacts_total
    //                             (# of successful concurrent compacts
    //                              with safepoint coordination — proxy
    //                              for "how often the arena can safely
    //                              compact under fiber contention")
    //   - envframe-revalidations  arena_envframe_revalidations_total
    //                             (# of times an EnvId in env_frames_
    //                              SoA was revalidated post-compact via
    //                              GCEnvWalkFn — proxy for "how often
    //                              post-compact EnvFrame consistency
    //                              is verified")
    //   - panic-rollback-compact-hits
    //                            arena_panic_rollback_compact_hits_total
    //                             (# of panic checkpoint auto-rollbacks
    //                              that fired under a concurrent compact
    //                              — proxy for "how often panic restore
    //                              detected an inconsistent compact +
    //                              triggered rollback")
    //   - races-prevented         arena_races_prevented_total
    //                             (# of times a race was prevented
    //                              via safepoint + deferred — proxy
    //                              for "how often steal/resume vs
    //                              compact race was safely deferred")
    //   - schema == 731
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual concurrent compact / defrag safepoint coordination
    // in arena.ixx + GCEnvWalkFn EnvFrame revalidation in evaluator_gc.cpp
    // + fiber.cpp resume() / transfer hook integration + panic checkpoint
    // snapshot integration + tests/test_arena_concurrent_compact_envframe_
    // fiber_steal.cpp harness (heavy alloc / mutate under 10+ fibers +
    // steal + periodic compact + panic injection) + #674 stress extension
    // are all follow-up work (each is a dedicated session in arena.ixx +
    // gc_coordinator + evaluator_gc.cpp + fiber.cpp + panic_checkpoint +
    // new test + chaos stress + docs).
    //
    // Issue #731: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=731 + category=general
    // + arity=0 + pure=true (same pattern as #712-#728).
    ObservabilityPrims::register_stats_impl(
        "query:arena-concurrent-compact-stats", [&ev](const auto&) -> EvalValue {
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
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 5-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t concurrent_compacts =
                m ? static_cast<std::int64_t>(
                        m->arena_concurrent_compacts_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t envframe_revalidations =
                m ? static_cast<std::int64_t>(
                        m->arena_envframe_revalidations_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t panic_rollback_compact_hits =
                m ? static_cast<std::int64_t>(
                        m->arena_panic_rollback_compact_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t races_prevented =
                m ? static_cast<std::int64_t>(
                        m->arena_races_prevented_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"concurrent-compacts", make_int(concurrent_compacts)},
                {"envframe-revalidations", make_int(envframe_revalidations)},
                {"panic-rollback-compact-hits", make_int(panic_rollback_compact_hits)},
                {"races-prevented", make_int(races_prevented)},
                {"schema", make_int(731)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 13 (orig lines 13032-13188)
void ObservabilityPrims::register_jit_p13(PrimRegistrar add, Evaluator& ev) {

    // Issue #732: (query:aot-safe-swap-boundary-stats) — AOT
    // hot-reload safe-swap at MutationBoundary observability for
    // production zero-downtime multi-agent orchestration (non-
    // duplicative with #708 (query:aot-reload-stats 5-7 field
    // high-level reload summary — attempts / success / stale /
    // refcount_swaps / region_violations / deopt-on-steal /
    // concurrent-safe-reloads) + #644 (query:aot-reload-func-
    // table-stats enforcement with ref-bump / ref-decrement /
    // region-reapply) + #590 (query:aot-hotupdate-stats 3 atomics).
    // #732 covers the *safe-swap at MutationBoundary* specifically
    // — reloads that fired at the outermost safe-swap point (NOT
    // mid-mutation) — as the per-decision-point signal the Agent
    // consumes to monitor safe-swap adoption rate + zero-downtime
    // orchestration quality.
    //
    // Fields (5 + sentinel):
    //   - safe-boundary-hits          aot_safe_boundary_hits_total
    //                                 (# of AOT reloads that fired at
    //                                  outermost MutationBoundary
    //                                  safe-swap point — proxy for
    //                                  "how often reload landed at a
    //                                  true safe point vs. was
    //                                  deferred / raced")
    //   - refcount-swaps              aot_refcount_swaps_
    //                                 (# of atomic func_table
    //                                  refcount swaps — read from
    //                                  existing #708 atomic for
    //                                  cross-reference with the
    //                                  high-level summary)
    //   - region-violations-prevented aot_region_mismatch_
    //                                 (# of region mismatches
    //                                  detected + prevented on reload
    //                                  — read from existing #708
    //                                  atomic; close to #708's
    //                                  region-violations field)
    //   - concurrent-safe-reloads     aot_concurrent_safe_reloads_
    //                                 (# of concurrent safe reloads
    //                                  — read from existing #708
    //                                  atomic; cross-reference with
    //                                  high-level summary)
    //   - deopt-on-steal              aot_deopt_on_steal_
    //                                 (# of deopts triggered on fiber
    //                                  steal — read from existing
    //                                  #708 atomic; cross-reference)
    //   - schema == 732
    //
    // Phase 1 ships the primitive + counter + bump helper.
    // The actual atomic func_table refcount swap protocol in
    // aura_jit_bridge.cpp aura_reload_aot_module + per-region
    // isolation enforcement on reload + aura_aot_request_safe_reload()
    // API + MutationBoundaryGuard outermost exit hook + GraceEpoch
    // defer-old-decrement after grace period + tests/test_aot_hot_swap_
    // refcount_region_guard_safe.cpp harness (multi-agent different
    // regions + AOT emit + mutate + concurrent apply + reload at
    // boundary) + #674 concurrent stress integration + docs are
    // all follow-up work (each is a dedicated session in
    // aura_jit_bridge.cpp + MutationBoundaryGuard + fiber.cpp + new
    // test + chaos stress + docs).
    //
    // Issue #732: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=732 + category=general
    // + arity=0 + pure=true (same pattern as #712-#728 / #731).
    ObservabilityPrims::register_stats_impl(
        "query:aot-safe-swap-boundary-stats", [&ev](const auto&) -> EvalValue {
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
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 6-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t safe_boundary_hits =
                m ? static_cast<std::int64_t>(
                        m->aot_safe_boundary_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t refcount_swaps =
                m ? static_cast<std::int64_t>(
                        m->aot_refcount_swaps_.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t region_violations_prevented =
                m ? static_cast<std::int64_t>(
                        m->aot_region_mismatch_.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t concurrent_safe_reloads =
                m ? static_cast<std::int64_t>(
                        m->aot_concurrent_safe_reloads_.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t deopt_on_steal =
                m ? static_cast<std::int64_t>(
                        m->aot_deopt_on_steal_.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"safe-boundary-hits", make_int(safe_boundary_hits)},
                {"refcount-swaps", make_int(refcount_swaps)},
                {"region-violations-prevented", make_int(region_violations_prevented)},
                {"concurrent-safe-reloads", make_int(concurrent_safe_reloads)},
                {"deopt-on-steal", make_int(deopt_on_steal)},
                {"schema", make_int(732)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 14 (orig lines 13189-13357)
void ObservabilityPrims::register_jit_p14(PrimRegistrar add, Evaluator& ev) {

    // Issue #733: (query:ir-marker-hygiene-stats) — Macro SyntaxMarker
    // propagation + IR/JIT hygiene enforcement observability for
    // Task6 macro-heavy self-evolution reliability (non-duplicative
    // with #714 (query:self-evolution-closedloop-stats — ref drift +
    // rollback success + feedback mutate rounds) + #455 (ir marker
    // snapshot — internal mechanics, no observability surface) + #373
    // (mutate hygiene guard — flat.is_macro_introduced internal check).
    // #733 covers the *marker propagation + IR/JIT enforcement*
    // specifically across the entire compile/execution pipeline
    // (macro expand → AST → lowering → IR → JIT hot-path → Interpreter)
    // as separate per-decision-point counters.
    //
    // Fields (5 + sentinel):
    //   - user-instrs                  ir_marker_user_instrs_total
    //                                   (# of IRInstructions created
    //                                    with marker=User — proxy for
    //                                    "how much IR traffic is
    //                                    user-authored")
    //   - macro-introduced-instrs      ir_marker_macro_introduced_instrs_total
    //                                   (# of IRInstructions created
    //                                    with marker=MacroIntroduced
    //                                    — proxy for "how much IR
    //                                    traffic is macro-authored,
    //                                    the hygiene scope")
    //   - marker-loss-events           ir_marker_loss_events_total
    //                                   (# of times marker propagation
    //                                    failed at emit path —
    //                                    closure / PrimCall arg /
    //                                    linear op / cached define
    //                                    path that did not copy AST
    //                                    marker → IR source_marker /
    //                                    IRFunction marker — proxy for
    //                                    "how many macro-introduced
    //                                    sub-exprs lost their hygiene
    //                                    marker through the pipeline")
    //   - jit-hygiene-violations-prevented
    //                                  ir_hygiene_jit_violations_prevented_total
    //                                   (# of times the JIT conservative
    //                                    policy fired on MacroIntroduced
    //                                    source_marker — prevented
    //                                    aggressive deopt-elide /
    //                                    respected hygiene in closure
    //                                    capture / forced Interpreter
    //                                    fallback or extra epoch check
    //                                    — proxy for "how often the
    //                                    JIT hot-path consults marker
    //                                    + applies conservative policy")
    //   - marker-propagation-hits      ir_hygiene_marker_propagation_hits_total
    //                                   (# of times marker propagation
    //                                    succeeded across all emit
    //                                    sites via propagate_marker_
    //                                    from_ast helper — proxy for
    //                                    "how often the hygiene marker
    //                                    survives the pipeline")
    //   - schema == 733
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual propagate_marker_from_ast helper in lowering_impl.cpp
    // + ir_soa.ixx marker_ column + aura_jit.cpp + aura_jit_runtime.cpp
    // + ir_executor.ixx conservative policy on source_marker==
    // MacroIntroduced + IRFunction creation marker-from-root-AST-
    // marker in service/lowering + tests/test_macro_marker_propagation_
    // ir_jit_post_mutate.cpp harness (define macro that introduces
    // lambda + mutate inside it under fiber + JIT hot path) + #674
    // stress integration + SEVA macro-heavy cases are all follow-up
    // work (each is a dedicated session in lowering_impl.cpp +
    // ir_soa.ixx + aura_jit.cpp + aura_jit_runtime.cpp + ir_executor.ixx
    // + new test + chaos stress + SEVA demo + docs).
    //
    // Issue #733: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=733 + category=general
    // + arity=0 + pure=true (same pattern as #712-#732).
    ObservabilityPrims::register_stats_impl(
        "query:ir-marker-hygiene-stats", [&ev](const auto&) -> EvalValue {
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
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 6-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t user_instrs =
                m ? static_cast<std::int64_t>(
                        m->ir_marker_user_instrs_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t macro_introduced_instrs =
                m ? static_cast<std::int64_t>(
                        m->ir_marker_macro_introduced_instrs_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t marker_loss_events =
                m ? static_cast<std::int64_t>(
                        m->ir_marker_loss_events_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t jit_hygiene_violations_prevented =
                m ? static_cast<std::int64_t>(m->ir_hygiene_jit_violations_prevented_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t marker_propagation_hits =
                m ? static_cast<std::int64_t>(
                        m->ir_hygiene_marker_propagation_hits_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"user-instrs", make_int(user_instrs)},
                {"macro-introduced-instrs", make_int(macro_introduced_instrs)},
                {"marker-loss-events", make_int(marker_loss_events)},
                {"jit-hygiene-violations-prevented", make_int(jit_hygiene_violations_prevented)},
                {"marker-propagation-hits", make_int(marker_propagation_hits)},
                {"schema", make_int(733)},
            };
            return build_hash(kv);
        });
}

// Issue #909 part 15 (orig lines 13358-13526)
void ObservabilityPrims::register_jit_p15(PrimRegistrar add, Evaluator& ev) {

    // Issue #735: (query:macro-provenance-stats) — MacroIntroduced
    // provenance in StableNodeRef + targeted dirty/rollback for
    // macro subtrees observability for precise handling of
    // macro-generated code in self-evolution (non-duplicative with
    // #714 (query:self-evolution-closedloop-stats — ref drift +
    // rollback + feedback mutate rounds) + #717 (query:fiber-
    // boundary-violation-stats — fiber/Guard boundary invariants)
    // + #392 (subtree gen — internal subtree mechanism) + #373
    // (mutate hygiene guard — flat.is_macro_introduced internal
    // check) + #733 (query:ir-marker-hygiene-stats — IR-level
    // marker propagation) + #750 (query:reflection-schema-stats
    // — runtime reflection validate). #735 covers the
    // *MacroIntroduced provenance + targeted macro-subtree
    // handling* specifically — capture-time provenance in
    // StableNodeRef, hot-path consult, targeted dirty propagation
    // for macro-subtree, rollback success — as separate
    // per-decision-point counters.
    //
    // Fields (4 + sentinel):
    //   - is-macro-introduced-consults  macro_provenance_is_macro_introduced_total
    //                                    (# of times the is_macro_
    //                                     introduced hot-path
    //                                     consult fired on a
    //                                     StableRef — proxy for
    //                                     "how often the macro
    //                                     check actually fires
    //                                     at hot path")
    //   - provenance-captured          macro_provenance_captured_total
    //                                    (# of times StableNodeRef
    //                                     capture populated
    //                                     macro_introduced_at_
    //                                     capture + original_
    //                                     macro_expansion_id
    //                                     fields — proxy for "how
    //                                     often provenance is
    //                                     tracked on capture")
    //   - dirty-impact-on-macro-subtree
    //                                  macro_provenance_dirty_impact_total
    //                                    (# of dirty propagations
    //                                     targeted to macro subtree
    //                                     (via original_macro_
    //                                     expansion_id) instead of
    //                                     whole subtree — proxy
    //                                     for "how often we avoid
    //                                     over-invalidation via
    //                                     provenance-aware dirty")
    //   - rollback-success             macro_provenance_rollback_success_total
    //                                    (# of successful rollback
    //                                     that preserved macro
    //                                     marker during restore_
    //                                     children — proxy for
    //                                     "how often targeted
    //                                     macro-subtree rollback
    //                                     fired cleanly")
    //   - schema == 735
    //
    // Phase 1 ships the primitive + counters + bump helpers.
    // The actual ast.ixx StableNodeRef + macro_introduced_at_capture
    // + original_macro_expansion_id fields + is_valid_subtree
    // macro_provenance_check + MutationBoundaryGuard +
    // rollback_macro_subtree_provenance + mark_dirty_upward
    // targeted macro-subtree + dirty/epoch interaction
    // strengthening (verify/macro dirty cascade respect
    // MacroIntroduced provenance for incremental re-lower) +
    // StableRef / hygiene stats correlation enhancement +
    // tests/test_macro_provenance_stable_ref_rollback_self_evo.cpp
    // harness (nested macro expand + multi-round mutate:rebind
    // inside macro body under fiber steal / panic / Guard fail) +
    // SEVA macro cases + #674 chaos stress integration + docs
    // are all follow-up work (each is a dedicated session in
    // ast.ixx + mutate.cpp + evaluator_primitives_mutate.cpp +
    // new test + SEVA demo + chaos stress + docs).
    //
    // Issue #735: routes through ev.primitives_.add (3-arg form)
    // so we can attach PrimMeta with schema=735 + category=general
    // + arity=0 + pure=true (same pattern as #712-#733).
    ObservabilityPrims::register_stats_impl(
        "query:macro-provenance-stats", [&ev](const auto&) -> EvalValue {
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
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        // 8 slots should be enough for the 5-key hashes we build.
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t is_macro_introduced_consults =
                m ? static_cast<std::int64_t>(m->macro_provenance_is_macro_introduced_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t provenance_captured =
                m ? static_cast<std::int64_t>(
                        m->macro_provenance_captured_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t dirty_impact_on_macro_subtree =
                m ? static_cast<std::int64_t>(
                        m->macro_provenance_dirty_impact_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t rollback_success =
                m ? static_cast<std::int64_t>(
                        m->macro_provenance_rollback_success_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"is-macro-introduced-consults", make_int(is_macro_introduced_consults)},
                {"provenance-captured", make_int(provenance_captured)},
                {"dirty-impact-on-macro-subtree", make_int(dirty_impact_on_macro_subtree)},
                {"rollback-success", make_int(rollback_success)},
                {"schema", make_int(735)},
            };
            return build_hash(kv);
        });
}

} // namespace aura::compiler::primitives_detail
