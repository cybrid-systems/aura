// evaluator_primitives_obs_eval_00.cpp — Issue #909: peeled domain registration from observability
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
#include "core/self_healing_hooks.h"

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

// Issue #909 part 0 (orig lines 1167-1235)
void ObservabilityPrims::register_eval_p0(PrimRegistrar add, Evaluator& ev) {

    // (typecheck-status) — Returns the last mutate typecheck result.
    // Empty string = no errors, non-empty = last mutate caused type errors.
    add("typecheck-status", [&ev](const auto&) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        if (ev.last_mutate_error_.empty()) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back("ok");
            return make_string(sidx);
        }
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(ev.last_mutate_error_);
        return make_string(sidx);
    });

    // (auto-rollback-on-panic [#t|#f]) — Get/set auto-rollback on panic flag
    // When enabled, runtime error triggers automatic rollback to last safe
    // checkpoint. Returns previous value.
    add("auto-rollback-on-panic", [&ev](std::span<const EvalValue> a) -> EvalValue {
        bool old = ev.panic_auto_rollback_;
        if (!a.empty() && types::is_bool(a[0]))
            ev.panic_auto_rollback_ = types::as_bool(a[0]);
        return make_bool(old);
    });

    // (panic-auto-rollback?) — Query current auto-rollback state
    add("panic-auto-rollback?",
        [&ev](const auto&) -> EvalValue { return make_bool(ev.panic_auto_rollback_); });

    // Issue #753: (resource:quota-set kind limit) — configure quota
    // axis ("memory" | "fibers" | "time"). 0 = unlimited.
    add("resource:quota-set", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1]))
            return make_int(0);
        const auto ki = as_string_idx(a[0]);
        if (ki >= ev.string_heap_.size())
            return make_int(0);
        const auto limit = static_cast<std::uint64_t>(as_int(a[1]));
        const auto& kind = ev.string_heap_[ki];
        if (kind == "memory")
            ev.set_resource_quota_memory(limit);
        else if (kind == "fibers")
            ev.set_resource_quota_fibers(limit);
        else if (kind == "time")
            ev.set_resource_quota_time_us(limit);
        else
            return make_int(0);
        return make_int(1);
    });
}

// Issue #909 part 1 (orig lines 1236-1289)
void ObservabilityPrims::register_eval_p1(PrimRegistrar add, Evaluator& ev) {
    // Issue #753: (resource:quota-get kind) — read configured limit.
    add("resource:quota-get", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        const auto ki = as_string_idx(a[0]);
        if (ki >= ev.string_heap_.size())
            return make_int(0);
        const auto& kind = ev.string_heap_[ki];
        if (kind == "memory")
            return make_int(static_cast<std::int64_t>(ev.resource_quota_memory()));
        if (kind == "fibers")
            return make_int(static_cast<std::int64_t>(ev.resource_quota_fibers()));
        if (kind == "time")
            return make_int(static_cast<std::int64_t>(ev.resource_quota_time_us()));
        return make_int(0);
    });
    // Issue #753 / #1013: (resource:quota-check kind current) — enforce quota;
    // bumps quota-violations / resource-trend / deployment-slo-hits +
    // production-hardening resource_quota_* counters.
    add("resource:quota-check", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1]))
            return make_bool(false);
        const auto ki = as_string_idx(a[0]);
        if (ki >= ev.string_heap_.size())
            return make_bool(false);
        const auto current = static_cast<std::uint64_t>(as_int(a[1]));
        const auto& kind = ev.string_heap_[ki];
        std::uint64_t limit = 0;
        if (kind == "memory")
            limit = ev.resource_quota_memory();
        else if (kind == "fibers")
            limit = ev.resource_quota_fibers();
        else if (kind == "time")
            limit = ev.resource_quota_time_us();
        else
            return make_bool(false);
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->resource_quota_checks_total.fetch_add(1, std::memory_order_relaxed);
        ev.bump_longrunning_resource_trend();
        if (limit > 0 && current > limit) {
            ev.bump_longrunning_quota_violations();
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->resource_quota_rejects_total.fetch_add(1, std::memory_order_relaxed);
            // Issue #1203 Phase 1: trigger registered SelfHealingHooks on quota violation.
            aura::core::self_heal::trigger_self_healing(
                {.kind = "quota-violation", .message = kind, .code = current});
            ev.bump_longrunning_heal_triggers();
            return make_bool(false);
        }
        if (limit > 0)
            ev.bump_longrunning_deployment_slo_hits();
        return make_bool(true);
    });

    // (panic-checkpoint) — Save current workspace as a safe checkpoint
    // Returns #t on success, #f if no workspace loaded.
    add("panic-checkpoint",
        [&ev](const auto&) -> EvalValue { return make_bool(ev.save_panic_checkpoint()); });

    // (panic-restore) — Restore to the last safe checkpoint
    // Returns #t on success, #f if no checkpoint available or restore failed.
    add("panic-restore",
        [&ev](const auto&) -> EvalValue { return make_bool(ev.restore_panic_checkpoint()); });
}

// Issue #909 part 2 (orig lines 1290-1348)
void ObservabilityPrims::register_eval_p2(PrimRegistrar add, Evaluator& ev) {

    // (panic-safe-source) — Return the checkpoint source code
    // Returns empty string if no checkpoint.
    add("panic-safe-source", [&ev](const auto&) -> EvalValue {
        auto idx = ev.string_heap_.size();
        ev.string_heap_.push_back(ev.panic_safe_source_);
        return make_string(idx);
    });

    // Issue #480: (primitive:describe name) — return PrimMeta as
    // (arity . (pure . (safety-flags . doc-string))).
    ev.primitives_.add(
        "primitive:describe",
        [&ev](const auto& a) -> EvalValue {
            if (a.size() != 1 || !is_string(a[0]))
                return make_void();
            const auto& heap = ev.string_heap_;
            const auto idx = as_string_idx(a[0]);
            if (idx >= heap.size())
                return make_void();
            const auto& name = heap[idx];
            const auto slot = ev.primitives_.slot_for_name(name);
            if (slot >= ev.primitives_.slot_count())
                return make_void();
            ev.bump_primitive_describe_count();
            return ObservabilityPrims::meta_to_pair(ev, ev.primitives_.meta_for_slot(slot));
        },
        PrimMeta{.arity = 1,
                 .pure = true,
                 .doc = "Return metadata for a registered primitive by name.",
                 .category = "general",
                 .schema = "(string) -> pair"});

    // Issue #1451: (primitive:validate-new name) — Agent-Proof proposal check.
    // Does NOT register. Returns a hash: ok / blocked / already-registered /
    // blocked-category / prefer-stdlib / requires-red-line / advice.
    // Mirrors scripts/check_primitive_surface.py freeze patterns.
    ev.primitives_.add(
        "primitive:validate-new",
        [&ev](const auto& a) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
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
            auto push_str = [&](std::string s) -> EvalValue {
                auto i = ev.string_heap_.size();
                ev.string_heap_.push_back(std::move(s));
                return make_string(i);
            };

            if (a.size() != 1 || !is_string(a[0])) {
                return build_hash(std::vector<std::pair<std::string, EvalValue>>{
                    {"schema", make_int(1)},
                    {"ok", make_bool(false)},
                    {"blocked", make_bool(true)},
                    {"already-registered", make_bool(false)},
                    {"prefer-stdlib", make_bool(true)},
                    {"requires-red-line", make_bool(true)},
                    {"advice", push_str("usage: (primitive:validate-new \"name\")")},
                });
            }
            const auto idx = as_string_idx(a[0]);
            if (idx >= ev.string_heap_.size())
                return make_void();
            const std::string name = ev.string_heap_[idx];

            // Freeze categories — keep in sync with check_primitive_surface.py
            auto blocked_category = [](std::string_view n) -> const char* {
                if (n.ends_with("-stats") || n.ends_with("-stats-hash") ||
                    n.find("-stats-") != std::string_view::npos)
                    return "stats";
                static constexpr const char* kPfx[] = {
                    "string-", "string:", "json-", "json:", "math-", "math:",    "vector-",
                    "vector:", "path-",   "path:", "time-", "time:", "ast:ref-",
                };
                static constexpr const char* kCat[] = {
                    "string", "string", "json", "json", "math", "math",    "vector",
                    "vector", "path",   "path", "time", "time", "ast:ref",
                };
                for (std::size_t i = 0; i < sizeof(kPfx) / sizeof(kPfx[0]); ++i) {
                    const std::string_view pfx(kPfx[i]);
                    if (n.size() >= pfx.size() && n.compare(0, pfx.size(), pfx) == 0)
                        return kCat[i];
                }
                return nullptr;
            };

            const bool already = ev.primitives_.slot_for_name(name) < ev.primitives_.slot_count();
            const char* cat = blocked_category(name);
            const bool blocked = cat != nullptr;
            const bool empty = name.empty();
            const bool ok = !empty && !already && !blocked;
            const bool prefer_stdlib = blocked || empty || !ok;

            std::string advice;
            if (empty) {
                advice = "empty name rejected";
            } else if (already) {
                advice = "name already registered; use primitive:describe or extend existing API";
            } else if (blocked) {
                advice = std::string("freeze-blocked category '") + cat +
                         "'; use CompilerMetrics+(engine:metrics)/lib/std instead of public add()";
            } else {
                advice = "name free of freeze patterns; still requires red-line citation in PR "
                         "(see docs/design/primitive-vs-stdlib-decision-framework.md) before add()";
            }

            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"schema", make_int(1)},
                {"name", push_str(name)},
                {"ok", make_bool(ok)},
                {"already-registered", make_bool(already)},
                {"blocked", make_bool(blocked || empty)},
                {"prefer-stdlib", make_bool(prefer_stdlib)},
                {"requires-red-line", make_bool(true)},
                {"advice", push_str(std::move(advice))},
            };
            if (cat)
                kv.push_back({"blocked-category", push_str(cat)});
            else
                kv.push_back({"blocked-category", make_void()});
            return build_hash(kv);
        },
        PrimMeta{.arity = 1,
                 .pure = true,
                 .doc = "Issue #1451: validate a proposed public primitive name "
                        "(freeze patterns + already-registered). Does not register.",
                 .category = "general",
                 .schema = "(string) -> hash"});

    // Issue #480: (query:primitive-list-with-meta) — list of
    // (name . meta-pair) for every registered primitive.
    ev.primitives_.add(
        "query:primitive-list-with-meta",
        [&ev](const auto& a) -> EvalValue {
            (void)a;
            ev.bump_primitive_list_meta_count();
            EvalValue result = make_void();
            for (std::size_t slot = ev.primitives_.slot_count(); slot-- > 0;) {
                const auto& name = ev.primitives_.name_for_slot(slot);
                auto nidx = ev.string_heap_.size();
                ev.string_heap_.push_back(name);
                auto pid = ev.pairs_.size();
                ev.pairs_.push_back(
                    {make_string(nidx),
                     ObservabilityPrims::meta_to_pair(ev, ev.primitives_.meta_for_slot(slot))});
                auto wrap = ev.pairs_.size();
                ev.pairs_.push_back({make_pair(pid), result});
                result = make_pair(wrap);
            }
            return result;
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .doc = "List every primitive with its PrimMeta pair.",
                 .category = "general",
                 .schema = "() -> list"});
}

// Issue #909 part 3 (orig lines 1349-1432)
void ObservabilityPrims::register_eval_p3(PrimRegistrar add, Evaluator& ev) {

    // Issue #617: (query:primitives-by-category category) — filter
    // primitive-list-with-meta by PrimMeta.category. The Agent
    // discovery loop starts here: "show me all primitives in the
    // 'eda' category" is a single primitive call rather than
    // downloading the entire registry list and walking it client-side.
    //
    // Returns a list of (name . meta-pair) for every primitive
    // whose category matches. Returns () for an unknown category
    // (matches query:primitive-list-with-meta behavior on empty).
    // Bumps primitives_by_category_query_total on every call (success
    // or miss) so the catalog primitive can surface per-discovery-
    // entry hit rates.
    ev.primitives_.add(
        "query:primitives-by-category",
        [&ev](std::span<const EvalValue> a) -> EvalValue {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->primitives_by_category_query_total.fetch_add(1, std::memory_order_relaxed);
            if (a.empty() || !is_string(a[0]))
                return make_void();
            const auto idx = as_string_idx(a[0]);
            if (idx >= ev.string_heap_.size())
                return make_void();
            const auto& category = ev.string_heap_[idx];
            EvalValue result = make_void();
            for (std::size_t slot = ev.primitives_.slot_count(); slot-- > 0;) {
                const auto& pm = ev.primitives_.meta_for_slot(slot);
                if (pm.category != category)
                    continue;
                const auto& name = ev.primitives_.name_for_slot(slot);
                auto nidx = ev.string_heap_.size();
                ev.string_heap_.push_back(name);
                auto pid = ev.pairs_.size();
                ev.pairs_.push_back({make_string(nidx), ObservabilityPrims::meta_to_pair(ev, pm)});
                auto wrap = ev.pairs_.size();
                ev.pairs_.push_back({make_pair(pid), result});
                result = make_pair(wrap);
            }
            return result;
        },
        PrimMeta{.arity = 1,
                 .pure = true,
                 .doc = "Return list of (name . meta-pair) for primitives in the given category.",
                 .category = "general",
                 .schema = "(string) -> list"});

    // Issue #617: (query:schema-of-primitive name) — return just
    // the PrimMeta.schema string for a named primitive. Lighter
    // than (query:primitive-metadata) which returns the full nested
    // pair; suitable for the Agent's "what's the signature of X?"
    // lookups that don't need arity/pure/safety/doc.
    //
    // Returns the schema string on success. Returns #f when the
    // primitive is unknown OR when it exists but has no schema
    // documented (so the Agent can branch on (schema-of-primitive
    // 'unknown-name') and not be confused with an empty-string
    // schema for a documented primitive).
    ev.primitives_.add(
        "query:schema-of-primitive",
        [&ev](std::span<const EvalValue> a) -> EvalValue {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->schema_of_primitive_query_total.fetch_add(1, std::memory_order_relaxed);
            if (a.empty() || !is_string(a[0]))
                return make_bool(false);
            const auto idx = as_string_idx(a[0]);
            if (idx >= ev.string_heap_.size())
                return make_bool(false);
            const auto& name = ev.string_heap_[idx];
            const auto slot = ev.primitives_.slot_for_name(name);
            if (slot >= ev.primitives_.slot_count())
                return make_bool(false);
            const auto& pm = ev.primitives_.meta_for_slot(slot);
            if (pm.schema.empty())
                return make_bool(false);
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(pm.schema);
            return make_string(sidx);
        },
        PrimMeta{.arity = 1,
                 .pure = true,
                 .doc = "Return the PrimMeta.schema string for a named primitive, or #f if unknown "
                        "/ undocumented.",
                 .category = "general",
                 .schema = "(string) -> string | bool"});
}

// Issue #909 part 4 (orig lines 1433-1544)
void ObservabilityPrims::register_eval_p4(PrimRegistrar add, Evaluator& ev) {

    // Issue #617: (query:primitives-meta-catalog) — one-call
    // Agent discovery primitive. Companion to (query:primitives-
    // extension-stats) from #697, but focused on the meta layer
    // rather than the runtime counters.
    //
    // Returns a 7-field hash:
    //   - total-registered:        slot_count()
    //   - schema-documented:       # of primitives with non-empty
    //                              doc AND schema
    //   - doc-only:                # of primitives with non-empty
    //                              doc but empty schema (registration
    //                              needs follow-up to add schema)
    //   - by-category-eda:         category_meta_count("eda")
    //   - by-category-sva:         category_meta_count("sva")
    //   - by-category-verification: category_meta_count("verification")
    //   - by-category-general:     # of primitives in any other
    //                              category (incl. "")
    //   - introspection-hits:     sum of primitives_by_category +
    //                              schema_of_primitive +
    //                              primitives_meta_catalog query
    //                              counters (so the Agent can see
    //                              how much the catalog has been
    //                              used this session)
    ev.primitives_.add(
        "query:primitives-meta-catalog",
        [&ev](const auto&) -> EvalValue {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->primitives_meta_catalog_query_total.fetch_add(1, std::memory_order_relaxed);
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
            std::size_t schema_doc = 0;
            std::size_t doc_only = 0;
            std::size_t eda = 0, sva = 0, verif = 0, gen = 0;
            for (std::size_t slot = ev.primitives_.slot_count(); slot-- > 0;) {
                const auto& pm = ev.primitives_.meta_for_slot(slot);
                if (!pm.doc.empty() && !pm.schema.empty())
                    ++schema_doc;
                else if (!pm.doc.empty())
                    ++doc_only;
                if (pm.category == "eda")
                    ++eda;
                else if (pm.category == "sva")
                    ++sva;
                else if (pm.category == "verification")
                    ++verif;
                else
                    ++gen;
            }
            const auto total = ev.primitives_.slot_count();
            const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t introspect_hits =
                m ? (m->primitives_by_category_query_total.load(std::memory_order_relaxed) +
                     m->schema_of_primitive_query_total.load(std::memory_order_relaxed) +
                     m->primitives_meta_catalog_query_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"total-registered", make_int(static_cast<std::int64_t>(total))},
                {"schema-documented", make_int(static_cast<std::int64_t>(schema_doc))},
                {"doc-only", make_int(static_cast<std::int64_t>(doc_only))},
                {"by-category-eda", make_int(static_cast<std::int64_t>(eda))},
                {"by-category-sva", make_int(static_cast<std::int64_t>(sva))},
                {"by-category-verification", make_int(static_cast<std::int64_t>(verif))},
                {"by-category-general", make_int(static_cast<std::int64_t>(gen))},
                {"introspection-hits", make_int(static_cast<std::int64_t>(introspect_hits))},
            };
            return build_hash(kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .doc = "One-call AI Agent discovery: meta layer breakdown by category + "
                        "introspection hit counter.",
                 .category = "general",
                 .schema = "() -> hash"});
}

// Issue #909 part 5 (orig lines 1545-1618)
void ObservabilityPrims::register_eval_p5(PrimRegistrar add, Evaluator& ev) {

    // Issue #697: (primitive:generate-skeleton description-string)
    // — AI-friendly primitive extension bundle: C++ lambda, spec,
    // test snippet, and DEFINE_PRIMITIVE_META registration code.
    ev.primitives_.add(
        "primitive:generate-skeleton",
        [&ev](const auto& a) -> EvalValue {
            if (a.size() != 1 || !is_string(a[0]))
                return make_void();
            const auto idx = as_string_idx(a[0]);
            if (idx >= ev.string_heap_.size())
                return make_void();
            const auto sk = generate_primitive_skeleton(ev.string_heap_[idx]);
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->primitive_skeleton_generations_total.fetch_add(1, std::memory_order_relaxed);
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
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            auto push_str = [&](const std::string& s) -> EvalValue {
                auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back(s);
                return make_string(sidx);
            };
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"category", push_str(sk.category)},
                {"spec", push_str(sk.spec)},
                {"cpp-lambda", push_str(sk.cpp_lambda)},
                {"test-snippet", push_str(sk.test_snippet)},
                {"registration", push_str(sk.registration)},
            };
            return build_hash(kv);
        },
        PrimMeta{.arity = 1,
                 .pure = true,
                 .doc = "Generate AI-friendly primitive extension skeleton from description.",
                 .category = "general",
                 .schema = "(string) -> hash"});
}

// Issue #909 part 6 (orig lines 1619-1767)
void ObservabilityPrims::register_eval_p6(PrimRegistrar add, Evaluator& ev) {

    // Issue #498: query:generate-primitive-skeleton — query-namespace alias
    // for primitive:generate-skeleton (Agent EDSL ergonomics).
    add("query:generate-primitive-skeleton", [&ev](const auto& a) -> EvalValue {
        if (auto fn = ev.primitives_.lookup("primitive:generate-skeleton"))
            return (*fn)(a);
        return make_void();
    });

    // Issue #633: query:stdlib-compiler-demands-stats-hash —
    // Agent-discoverable structured dashboard for the stdlib
    // commercial-evolution reverse-ask surface (specifically
    // covers AC5 from the issue body).
    //
    // Fields (5):
    //   - hotpath-calls         sum of the existing hot-path
    //                          counters (value_dispatch_hit_count +
    //                          primitive_fastpath_hits_total +
    //                          hotpath_eval_flat_calls +
    //                          hotpath_lowering_calls +
    //                          hotpath_soa_dual_emit_hits).
    //                          Synthesizes the AI primitive layer's
    //                          "hotpath_calls" demand signal.
    //   - error-consistency     existing value_contract_violation_
    //                          count (from #479 + #709). Higher
    //                          numbers = more contract violations
    //                          = more "error_consistency" debt.
    //   - extension-count       new stdlib_extension_count_total
    //                          atomic (foundation for AC3 DEFINE_
    //                          PRIMITIVE macro work — bumped per
    //                          new extension registered).
    //                          Value is 0 until AC3 wire-up.
    //   - ai-native-hits        new ai_native_primitive_hits_total
    //                          atomic (foundation for AC4 — bumped
    //                          per Agent-generated primitive
    //                          registration).
    //                          Value is 0 until AC4 wire-up.
    //   - soa-jit-win           existing primitive_fastpath_hits_
    //                          total (from #709) — proxy for
    //                          SoA/JIT win-rate at the primitive
    //                          layer.
    //   - schema == 633         sentinel for Agent drift detection
    //                          (mirrors the full chain through
    //                          #618+#620+#621+#622+#623+#624+#625+
    //                          #626+#630+#631+#632 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers ~80% of the AC5 surface:
    //   - (query:schema-of-primitive) (#617) — per-primitive schema
    //   - (query:primitives-meta-catalog) (#617) — 5-field catalog
    //   - (query:primitives-extensions-list) (#618) — extensions
    //   - (query:primitives-stats) (#479) — 8-field hot-path hash
    //   - (query:primitives-meta-stats) (#617) — primitive-meta
    //   - (query:primitives-fastpath-per-prim) (#709) — per-prim
    //   - hotpath counters on CompilerMetrics + PassPipeline
    //     Counters + PassPipeline metrics.
    // What AC5 specifies by **exact name + fields** —
    // `query:stdlib-compiler-demands-stats` with
    // {hotpath_calls, error_consistency, extension_count,
    // ai_native_hits, SoA/JIT_win} — was *not* shipped under
    // that exact name. So #633 ships ONE new Aura primitive
    // + 2 new foundation atomics.
    //
    // The remaining #633 AC1 + AC2 + AC3 + AC4 work (SoA value
    // views for primitives, unified PRIM_ERROR across registry,
    // DEFINE_PRIMITIVE macro, AI-generated primitive sandbox) is
    // invasive C++ + stdlib + reflect work that needs
    // benchmarking + perf regression coverage alongside the
    // existing AI/JSON/SoA initiatives — separate follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:stdlib-compiler-demands-stats-hash", [&ev](const auto&) -> EvalValue {
            // hotpath-calls: sum of all hot-path counters.
            const std::uint64_t dispatch_hits =
                types::value_dispatch_hit_count.load(std::memory_order_relaxed);
            const std::uint64_t fastpath_hits =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->primitive_fastpath_hits_total.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t hotpath_eval =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->hotpath_eval_flat_calls.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t hotpath_lowering =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->hotpath_lowering_calls.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t soa_dual_emit =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->hotpath_soa_dual_emit_hits.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t hotpath_calls =
                dispatch_hits + fastpath_hits + hotpath_eval + hotpath_lowering + soa_dual_emit;
            // error-consistency: existing value_contract_violation_count.
            const std::uint64_t error_consistency =
                types::value_contract_violation_count.load(std::memory_order_relaxed);
            // extension-count: new foundation atomic (0 until AC3 macro).
            const std::uint64_t extension_count =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->stdlib_extension_count_total.load(std::memory_order_relaxed)
                    : 0;
            // ai-native-hits: new foundation atomic (0 until AC4 wire-up).
            const std::uint64_t ai_native_hits =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->ai_native_primitive_hits_total.load(std::memory_order_relaxed)
                    : 0;
            // soa-jit-win: existing primitive_fastpath_hits_total proxy.
            const std::uint64_t soa_jit_win = fastpath_hits;
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
            insert_kv("hotpath-calls", static_cast<std::int64_t>(hotpath_calls));
            insert_kv("error-consistency", static_cast<std::int64_t>(error_consistency));
            insert_kv("extension-count", static_cast<std::int64_t>(extension_count));
            insert_kv("ai-native-hits", static_cast<std::int64_t>(ai_native_hits));
            insert_kv("soa-jit-win", static_cast<std::int64_t>(soa_jit_win));
            insert_kv("schema", 633);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 7 (orig lines 1768-1890)
void ObservabilityPrims::register_eval_p7(PrimRegistrar add, Evaluator& ev) {

    // Issue #637: query:closure-bridge-safety-stats-hash —
    // Agent-discoverable structured dashboard for IRClosure +
    // EnvFrame versioning + bridge invalidate protocol
    // (P0 memory-safety + commercial reliability surface;
    // non-duplicative to #620 #623 #624 — see issue body).
    //
    // Fields (3 + sentinel):
    //   - invalidations-post-mutate   new
    //                          closure_invalidation_post_mutate_total
    //                          atomic (foundation for AC1
    //                          invalidate_function wire-up —
    //                          bumped when invalidate_function
    //                          fires after a workspace mutate).
    //                          Value is 0 until AC1 wire-up.
    //   - version-mismatches-caught   new
    //                          closure_version_mismatch_caught_
    //                          total atomic (foundation for
    //                          AC2 bridge_epoch / EnvFrame
    //                          .version check wire-up in
    //                          apply_closure + materialize_
    //                          call_env — bumped per detected
    //                          mismatch that would otherwise
    //                          have caused UAF / stale-env
    //                          access). Value is 0 until
    //                          AC2 wire-up.
    //   - safe-rebuilds        new closure_safe_rebuild_total
    //                          atomic (foundation for AC2/AC3
    //                          Guard dtor + WorkspaceTree::
    //                          resolve_safe_ref wire-up —
    //                          bumped per successful bridge
    //                          rebuild after a mismatch).
    //                          Value is 0 until AC2/AC3
    //                          wire-up.
    //   - schema == 637         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the *foundation* for
    //   - invalidate_function_calls (#401) + jit_cache_evictions
    //     (#401) + compiler_inval_bridge_epoch_total (#498)
    //     + bridge_epoch_hit_count_ (#531)
    //     + jit_hotswap_*_total (#601) + linear_deopt_on_
    //     invalidate_total (#531) + stable_ref_invalidations
    //     (#604)
    // What the issue body AC4 specifies by **exact name +
    // fields** — `query:closure-bridge-safety-stats` with
    // {invalidations_post_mutate, version_mismatches_caught,
    // safe_rebuilds} — was *not* shipped under that exact
    // name with that exact field set. So #637 ships ONE new
    // Aura primitive + 3 new foundation atomics.
    //
    // The remaining #637 AC1 + AC2 + AC3 work (IRClosure
    // env_version/weak_env stamp, apply_closure dual-path
    // version check + integrate with MutationBoundaryGuard,
    // bridge_epoch bump to JIT hot-swap / Interpreter fallback)
    // is invasive C++ on the hot path + needs the 10k+ fiber
    // stress + TSan coverage from the issue body — separate
    // follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:closure-bridge-safety-stats-hash", [&ev](const auto&) -> EvalValue {
            // invalidations-post-mutate: new foundation atomic
            // (0 until AC1 invalidate_function wire-up).
            const std::uint64_t invalidations_post_mutate =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->closure_invalidation_post_mutate_total.load(std::memory_order_relaxed)
                    : 0;
            // version-mismatches-caught: new foundation atomic
            // (0 until AC2 apply_closure + materialize_call_env
            // wire-up).
            const std::uint64_t version_mismatches_caught =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->closure_version_mismatch_caught_total.load(std::memory_order_relaxed)
                    : 0;
            // safe-rebuilds: new foundation atomic
            // (0 until AC2/AC3 Guard dtor wire-up).
            const std::uint64_t safe_rebuilds =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->closure_safe_rebuild_total.load(std::memory_order_relaxed)
                    : 0;
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
            insert_kv("invalidations-post-mutate",
                      static_cast<std::int64_t>(invalidations_post_mutate));
            insert_kv("version-mismatches-caught",
                      static_cast<std::int64_t>(version_mismatches_caught));
            insert_kv("safe-rebuilds", static_cast<std::int64_t>(safe_rebuilds));
            insert_kv("schema", 637);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

} // namespace aura::compiler::primitives_detail
