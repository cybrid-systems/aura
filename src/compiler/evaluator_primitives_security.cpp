// evaluator_primitives_security.cpp — Issue #676 security/sandbox/audit primitives.

module;

#include "runtime_shared.h"
#include "compiler/observability_metrics.h"
#include "primitives_detail.h"
#include "primitives_meta.h"
#include "shape.h"
#include "value_tags.h"
#include "security_capabilities.h"
#include "serve/http_health.h"
#include "serve/metrics.h"
#include "hash_meta.h"              // FNV constants (#901)
#include "core/capability_model.hh" // #1565: snapshot_capability_effect_stats

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
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
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;
// Issue #918: qualify security:: explicitly where needed
// (was: using namespace security;)

void register_security_primitives(PrimRegistrar add, Evaluator& ev) {

    // Issue #1020: capability-gated sandbox admin surface.
    // - Enabling sandbox from open mode is always allowed.
    // - Changing mode while already sandboxed requires kCapWildcard
    //   (prevents untrusted code from disabling the sandbox).
    add("security:set-sandbox-mode!", [&ev](std::span<const EvalValue> a) -> EvalValue {
        const bool old = ev.sandbox_mode();
        if (a.empty() || !is_bool(a[0]))
            return make_bool(old);
        const bool want = as_bool(a[0]);
        if (old && !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->sandbox_admin_denials_total.fetch_add(1, std::memory_order_relaxed);
            return make_primitive_error(
                ev.string_heap_, ev.error_values_,
                "security:set-sandbox-mode!: wildcard capability required while sandboxed",
                ev.primitive_error_counter_ptr());
        }
        ev.set_sandbox_mode(want);
        // #1565: mirror bool sandbox into effect Restricted/Off.
        if (want && ev.effect_sandbox_mode() == 0)
            ev.set_effect_sandbox_mode(1); // Restricted
        if (!want && ev.effect_sandbox_mode() != 2)
            ev.set_effect_sandbox_mode(0);
        return make_bool(old);
    });

    // Issue #1565: (security:set-effect-sandbox-mode! n) 0=Off 1=Restricted 2=Strict
    add("security:set-effect-sandbox-mode!", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_int(static_cast<std::int64_t>(ev.effect_sandbox_mode()));
        if (ev.sandbox_mode() && !ev.has_capability(aura::compiler::security::kCapWildcard) &&
            as_int(a[0]) < static_cast<std::int64_t>(ev.effect_sandbox_mode())) {
            // Downgrade while sandboxed requires wildcard
            ev.bump_capability_denial();
            return make_primitive_error(
                ev.string_heap_, ev.error_values_,
                "security:set-effect-sandbox-mode!: wildcard required to lower mode",
                ev.primitive_error_counter_ptr());
        }
        const auto prev = ev.effect_sandbox_mode();
        ev.set_effect_sandbox_mode(static_cast<std::uint8_t>(as_int(a[0])));
        return make_int(static_cast<std::int64_t>(prev));
    });

    // Issue #1565: (security:grant-effect! name effect-bits [tenant])
    add("security:grant-effect!", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1]))
            return make_bool(false);
        if (ev.sandbox_mode() && !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            return make_bool(false);
        }
        const auto sidx = as_string_idx(a[0]);
        if (sidx >= ev.string_heap_.size())
            return make_bool(false);
        const auto bits = static_cast<std::uint16_t>(as_int(a[1]));
        const auto tenant = a.size() >= 3 && is_int(a[2]) ? static_cast<std::uint64_t>(as_int(a[2]))
                                                          : ev.capability_tenant_id();
        ev.grant_effect_capability(tenant, ev.string_heap_[sidx], bits, 0);
        return make_bool(true);
    });

    // Issue #1565: (security:check-effect required-bits) → #t/#f
    add("security:check-effect", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        const auto bits = static_cast<std::uint16_t>(as_int(a[0]));
        return make_bool(ev.check_and_record_effect(bits, bits, "security:check-effect", 0,
                                                    ev.capability_tenant_id(), 0));
    });

    ObservabilityPrims::register_stats_impl(
        "security:sandbox-mode?",
        [&ev](const auto&) -> EvalValue { return make_bool(ev.sandbox_mode()); });

    // Issue #1020: granting capabilities while sandboxed requires wildcard
    // (otherwise any code can self-elevate).
    add("security:grant-capability!", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0])) {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "security:grant-capability!: requires capability name",
                                        ev.primitive_error_counter_ptr());
        }
        if (ev.sandbox_mode() && !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->sandbox_admin_denials_total.fetch_add(1, std::memory_order_relaxed);
            return make_primitive_error(
                ev.string_heap_, ev.error_values_,
                "security:grant-capability!: wildcard capability required in sandbox mode",
                ev.primitive_error_counter_ptr());
        }
        const auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        ev.grant_capability(ev.string_heap_[idx]);
        return make_bool(true);
    });

    ObservabilityPrims::register_stats_impl(
        "query:security-stats", [&ev](const auto&) -> EvalValue {
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
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"sandbox-mode", make_bool(ev.sandbox_mode())},
                {"capability-denials",
                 make_int(static_cast<std::int64_t>(ev.capability_denial_count()))},
                {"mutation-audit-total",
                 make_int(static_cast<std::int64_t>(ev.mutation_audit_total()))},
                {"granted-capabilities",
                 make_int(static_cast<std::int64_t>(ev.granted_capability_count()))},
                {"effect-sandbox-mode",
                 make_int(static_cast<std::int64_t>(ev.effect_sandbox_mode()))},
            };
            return build_hash(kv);
        });

    // Issue #1565: query:capability-effect-stats
    ObservabilityPrims::register_stats_impl(
        "query:capability-effect-stats", [&ev](const auto&) -> EvalValue {
            using namespace aura::core::capability;
            const auto snap = snapshot_capability_effect_stats();
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->capability_effect_enforced_total.store(snap.enforced, std::memory_order_relaxed);
                m->capability_effect_denied_total.store(snap.denied, std::memory_order_relaxed);
                m->capability_provenance_mismatch_total.store(snap.provenance_mismatch,
                                                              std::memory_order_relaxed);
                m->capability_effect_grant_total.store(snap.grants, std::memory_order_relaxed);
                m->capability_effect_check_total.store(snap.checks, std::memory_order_relaxed);
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
            insert_kv("schema", 1565);
            insert_kv("active", 1);
            insert_kv("phase", snap.phase);
            insert_kv("enforced", static_cast<std::int64_t>(snap.enforced));
            insert_kv("denied", static_cast<std::int64_t>(snap.denied));
            insert_kv("provenance-mismatch", static_cast<std::int64_t>(snap.provenance_mismatch));
            insert_kv("grants", static_cast<std::int64_t>(snap.grants));
            insert_kv("revokes", static_cast<std::int64_t>(snap.revokes));
            insert_kv("checks", static_cast<std::int64_t>(snap.checks));
            insert_kv("audits", static_cast<std::int64_t>(snap.audits));
            insert_kv("sandbox-mode", snap.sandbox_mode);
            insert_kv("tenant-id", static_cast<std::int64_t>(ev.capability_tenant_id()));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // (query:mutation-audit-log) — Issue #676: exportable security
    // audit ring of recent successful structural mutations.
    // Optional arg N limits entries (default 10).
    ObservabilityPrims::register_stats_impl(
        "query:deployment-stats", [&ev](const auto&) -> EvalValue {
            const int http_port = aura::serve::http::port_from_env();
            const char* runtime_dir = std::getenv("AURA_RUNTIME_DIR");
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
            auto rt_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(runtime_dir ? runtime_dir : "");
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"http-port", make_int(http_port)},
                {"runtime-dir-set", make_bool(runtime_dir != nullptr && runtime_dir[0] != '\0')},
                {"runtime-dir", make_string(rt_idx)},
            };
            return build_hash(kv);
        });

    // Issue #678: PCV span lifetime observability for concurrent
    // query/mutate loops (unsafe raw span vs safe-view hits).
    // Issue #679: nested Guard + atomic-batch rollback alignment stats.
    ObservabilityPrims::register_stats_impl(
        "query:nested-guard-atomic-stats", [&ev](const auto&) -> EvalValue {
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
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"nested-depth-max",
                 make_int(static_cast<std::int64_t>(ev.nested_guard_depth_max()))},
                {"suppressed-misalign-caught",
                 make_int(static_cast<std::int64_t>(ev.suppressed_misalign_caught()))},
                {"macro-rollback-hits",
                 make_int(static_cast<std::int64_t>(ev.macro_rollback_hits()))},
            };
            return build_hash(kv);
        });

    // Issue #681: compiler IRClosure/bridge epoch enforcement stats.
    ObservabilityPrims::register_stats_impl(
        "query:compiler-closure-inval-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t stale_bridge =
                m ? m->closure_stale_returns.load(std::memory_order_relaxed) : 0;
            const std::uint64_t epoch_mismatch =
                m ? m->compiler_closure_epoch_mismatch_hits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t safe_fallbacks =
                m ? m->compiler_closure_safe_fallbacks.load(std::memory_order_relaxed) : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"stale-bridge-caught", make_int(static_cast<std::int64_t>(stale_bridge))},
                {"epoch-mismatch-hits", make_int(static_cast<std::int64_t>(epoch_mismatch))},
                {"safe-fallbacks", make_int(static_cast<std::int64_t>(safe_fallbacks))},
            };
            return build_hash(kv);
        });

    // Issue #682: compiler IRClosure/EnvId GC root coordination stats.
    ObservabilityPrims::register_stats_impl(
        "query:compiler-gc-root-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t ir_roots =
                m ? m->ir_closure_roots_registered.load(std::memory_order_relaxed) : 0;
            const std::uint64_t root_miss =
                m ? m->hotswap_root_miss.load(std::memory_order_relaxed) : 0;
            const std::uint64_t defer_count =
                m ? m->compiler_gc_safepoint_defer_count.load(std::memory_order_relaxed) : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"ir-closure-roots-registered", make_int(static_cast<std::int64_t>(ir_roots))},
                {"hotswap-root-miss", make_int(static_cast<std::int64_t>(root_miss))},
                {"safepoint-defer-count", make_int(static_cast<std::int64_t>(defer_count))},
            };
            return build_hash(kv);
        });

    // Issue #683: linear ownership + GC safepoint / steal / re-lower stats.
    ObservabilityPrims::register_stats_impl(
        "query:linear-ownership-gc-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t safepoint_v =
                m ? m->linear_gc_safepoint_violations.load(std::memory_order_relaxed) : 0;
            const std::uint64_t steal_enf =
                m ? m->linear_steal_enforced.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_hits =
                m ? m->linear_relower_revalidate_hits.load(std::memory_order_relaxed) : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"safepoint-violations", make_int(static_cast<std::int64_t>(safepoint_v))},
                {"steal-enforced", make_int(static_cast<std::int64_t>(steal_enf))},
                {"relower-revalidate-hits", make_int(static_cast<std::int64_t>(relower_hits))},
            };
            return build_hash(kv);
        });

    // Issue #684: IRSoA full wiring incremental stats hash.
    ObservabilityPrims::register_stats_impl(
        "query:irsoa-incremental-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t wired = m ? m->irsoa_wired_hits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t cascade =
                m ? m->irsoa_dirty_cascade_savings.load(std::memory_order_relaxed) : 0;
            const std::uint64_t cache_red =
                m ? m->irsoa_cache_miss_reduction.load(std::memory_order_relaxed) : 0;
            const std::uint64_t skip =
                m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t full =
                m ? m->relower_full_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t per_fn =
                m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t denom = skip + full + per_fn;
            const std::uint64_t partial_ratio = denom > 0 ? (100 * skip / denom) : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"soa-wired-hits", make_int(static_cast<std::int64_t>(wired))},
                {"dirty-cascade-savings", make_int(static_cast<std::int64_t>(cascade))},
                {"partial-relower-ratio", make_int(static_cast<std::int64_t>(partial_ratio))},
                {"cache-miss-reduction", make_int(static_cast<std::int64_t>(cache_red))},
            };
            return build_hash(kv);
        });

    // Issue #689: occurrence typing deep predicate + provenance stats.
    ObservabilityPrims::register_stats_impl(
        "query:occurrence-typing-mutate-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t deep =
                m ? m->deep_narrow_refreshes_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t blame =
                m ? m->occurrence_blame_chain_complete_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t stale =
                m ? m->narrow_stale_caught_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t complete =
                m ? m->provenance_completeness_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t refreshes =
                m ? m->occurrence_stale_refreshes_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t completeness_pct =
                refreshes > 0 ? (100 * complete / refreshes) : (complete > 0 ? 100 : 0);
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"deep-narrow-refreshes", make_int(static_cast<std::int64_t>(deep))},
                {"blame-refreshes", make_int(static_cast<std::int64_t>(blame))},
                {"stale-narrowing-caught", make_int(static_cast<std::int64_t>(stale))},
                {"provenance-completeness", make_int(static_cast<std::int64_t>(completeness_pct))},
            };
            return build_hash(kv);
        });

    // Issue #697: Declarative primitives extension kit stats.
    // #711 PrimMeta backfill — document this query primitive with
    // EDA category + descriptive doc so the Agent can filter by
    // domain via (query:primitives-by-category "eda") without
    // special-casing individual primitive names. Routes through
    // ev.primitives_.add (3-arg) instead of the local 2-arg
    // PrimRegistrar typedef (see top-of-file).
    ObservabilityPrims::register_stats_impl(
        "query:primitives-extension-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t skeletons =
                m ? m->primitive_skeleton_generations_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t eda_cat = ev.primitives_.category_meta_count("eda");
            const std::uint64_t sva_cat = ev.primitives_.category_meta_count("sva");
            const std::uint64_t ver_cat = ev.primitives_.category_meta_count("verification");
            const std::uint64_t backfill_counter =
                m ? m->primitive_eda_meta_backfill_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t backfill =
                backfill_counter > 0 ? backfill_counter : (eda_cat + sva_cat + ver_cat);
            const std::uint64_t schema_doc = ev.primitives_.schema_documented_meta_count();
            const std::uint64_t describes = ev.get_primitive_describe_count();
            const std::uint64_t slots = ev.primitives_.slot_count();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"skeleton-generations", make_int(static_cast<std::int64_t>(skeletons))},
                {"eda-meta-backfilled", make_int(static_cast<std::int64_t>(backfill))},
                {"category-eda", make_int(static_cast<std::int64_t>(eda_cat))},
                {"category-sva", make_int(static_cast<std::int64_t>(sva_cat))},
                {"category-verification", make_int(static_cast<std::int64_t>(ver_cat))},
                {"documented-with-schema", make_int(static_cast<std::int64_t>(schema_doc))},
                {"describe-calls", make_int(static_cast<std::int64_t>(describes))},
                {"registry-slots", make_int(static_cast<std::int64_t>(slots))},
                {"extension-kit-version",
                 make_int(static_cast<std::int64_t>(kPrimitivesExtensionKitVersion))},
            };
            return build_hash(kv);
        });

    // Issue #709: registry fast dispatch + capture discipline + EDA integration stats.
    ObservabilityPrims::register_stats_impl(
        "query:primitives-registry-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t capture_viol =
                m ? m->primitive_capture_violations_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t fastpath_hits =
                m ? m->primitive_fastpath_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t eda_registered = ev.primitives_.category_meta_count("eda") +
                                                 ev.primitives_.category_meta_count("sva") +
                                                 ev.primitives_.category_meta_count("verification");
            const std::uint64_t slots = ev.primitives_.slot_count();
            const std::uint64_t documented = ev.primitives_.documented_meta_count();
            const std::uint64_t consistency_rate = slots > 0 ? (documented * 100) / slots : 100;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"capture-violations", make_int(static_cast<std::int64_t>(capture_viol))},
                {"fastpath-hits", make_int(static_cast<std::int64_t>(fastpath_hits))},
                {"eda-registered", make_int(static_cast<std::int64_t>(eda_registered))},
                {"consistency-rate", make_int(static_cast<std::int64_t>(consistency_rate))},
                {"registry-slots", make_int(static_cast<std::int64_t>(slots))},
                {"capture-contract-version",
                 make_int(static_cast<std::int64_t>(kPrimCaptureContractVersion))},
                {"extension-kit-version",
                 make_int(static_cast<std::int64_t>(kPrimitivesExtensionKitVersion))},
            };
            return build_hash(kv);
        });

    // Issue #567: Primitive registry governance + stdlib layering closing metrics.
    ObservabilityPrims::register_stats_impl(
        "query:primitives-governance-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t slots = ev.primitives_.slot_count();
            const std::uint64_t documented = ev.primitives_.documented_meta_count();
            const std::uint64_t schema_doc = ev.primitives_.schema_documented_meta_count();
            const std::int64_t doc_coverage_pct =
                slots > 0 ? static_cast<std::int64_t>((documented * 100) / slots) : 100;
            const std::uint64_t capture_viol =
                m ? m->primitive_capture_violations_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t fastpath_hits =
                m ? m->primitive_fastpath_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t errors = ev.get_primitive_error_count();
            const std::uint64_t describes = ev.get_primitive_describe_count();
            const std::uint64_t list_meta = ev.get_primitive_list_meta_count();
            const std::uint64_t skeletons =
                m ? m->primitive_skeleton_generations_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t eda_total = ev.primitives_.category_meta_count("eda") +
                                            ev.primitives_.category_meta_count("sva") +
                                            ev.primitives_.category_meta_count("verification");
            const std::uint64_t total = slots + documented + schema_doc + capture_viol +
                                        fastpath_hits + errors + describes + list_meta + skeletons +
                                        eda_total;
            std::int64_t recommendation = 0;
            if (capture_viol > 0 || errors > 0)
                recommendation = 3;
            else if (doc_coverage_pct < 50)
                recommendation = 2;
            else if (describes > 0 || fastpath_hits > 0 || skeletons > 0)
                recommendation = 1;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"registry-slots", make_int(static_cast<std::int64_t>(slots))},
                {"documented-meta-count", make_int(static_cast<std::int64_t>(documented))},
                {"schema-documented-count", make_int(static_cast<std::int64_t>(schema_doc))},
                {"documentation-coverage-pct", make_int(doc_coverage_pct)},
                {"capture-violations", make_int(static_cast<std::int64_t>(capture_viol))},
                {"fastpath-hits", make_int(static_cast<std::int64_t>(fastpath_hits))},
                {"primitive-errors", make_int(static_cast<std::int64_t>(errors))},
                {"describe-calls", make_int(static_cast<std::int64_t>(describes))},
                {"list-meta-calls", make_int(static_cast<std::int64_t>(list_meta))},
                {"skeleton-generations", make_int(static_cast<std::int64_t>(skeletons))},
                {"eda-category-total", make_int(static_cast<std::int64_t>(eda_total))},
                {"extension-kit-version",
                 make_int(static_cast<std::int64_t>(kPrimitivesExtensionKitVersion))},
                {"governance-schema", make_int(567)},
                {"primitives-governance-total", make_int(static_cast<std::int64_t>(total))},
                {"primitives-governance-recommendation", make_int(recommendation)},
            };
            return build_hash(kv);
        });

    // Issue #583: registry + core primitives hot-path observability hash
    // (structured companion to int-sum query:primitives-stats).
    ObservabilityPrims::register_stats_impl(
        "query:primitives-registry-core-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t registry_slots = ev.get_primitive_slot_count();
            const std::uint64_t errors = ev.get_primitive_error_count();
            const std::uint64_t stored = ev.get_primitive_error_values_size();
            const std::uint64_t mutations = ev.total_mutations();
            const std::uint64_t queries = ev.get_total_query_calls();
            const std::uint64_t specialization =
                m ? m->specialization_hits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t fastpath =
                m ? m->primitive_fastpath_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t primitive_call_count = mutations + queries;
            const std::uint64_t hot_path_hits = specialization + fastpath;
            const std::uint64_t call_denom = primitive_call_count + errors + 1;
            const std::int64_t error_rate_pct =
                static_cast<std::int64_t>((errors * 100) / call_denom);
            const std::uint64_t total =
                registry_slots + primitive_call_count + errors + stored + hot_path_hits;
            std::int64_t recommendation = 0;
            if (errors > primitive_call_count / 10 && errors > 0)
                recommendation = 3;
            else if (registry_slots == 0)
                recommendation = 2;
            else if (hot_path_hits > 0 || primitive_call_count > 0)
                recommendation = 1;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"registry-slots", make_int(static_cast<std::int64_t>(registry_slots))},
                {"primitive-call-count", make_int(static_cast<std::int64_t>(primitive_call_count))},
                {"error-rate-pct", make_int(error_rate_pct)},
                {"hot-path-hits", make_int(static_cast<std::int64_t>(hot_path_hits))},
                {"error-path-cost", make_int(static_cast<std::int64_t>(errors))},
                {"consistency-violations", make_int(static_cast<std::int64_t>(stored))},
                {"registry-core-schema", make_int(583)},
                {"primitives-registry-core-total", make_int(static_cast<std::int64_t>(total))},
                {"primitives-registry-core-recommendation", make_int(recommendation)},
            };
            return build_hash(kv);
        });

    // Issue #585: unified primitive error handling + recovery observability hash.
    // Structured companion to #478 (query:primitive-error-stats pair) and
    // #583 registry-core error-rate-pct. Surfaces error_rate + recovery_success
    // for AI Agent mutate/query loops under div0 / regex / type-mismatch churn.
    ObservabilityPrims::register_stats_impl(
        "query:primitives-error-stats", [&ev](const auto&) -> EvalValue {
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
            const std::uint64_t errors = ev.get_primitive_error_count();
            const std::uint64_t stored = ev.get_primitive_error_values_size();
            const std::uint64_t mutations = ev.total_mutations();
            const std::uint64_t queries = ev.get_total_query_calls();
            const std::uint64_t panic_restore = ev.get_panic_checkpoint_restore_count();
            const std::uint64_t panic_commit = ev.get_panic_checkpoint_commit_count();
            const std::uint64_t rollbacks = ev.get_mutation_log_rollback_count();
            const std::uint64_t contract_violations =
                types::value_contract_violation_count.load(std::memory_order_relaxed);
            const std::uint64_t call_denom = mutations + queries + errors + 1;
            const std::int64_t error_rate = static_cast<std::int64_t>((errors * 100) / call_denom);
            const std::uint64_t recovery_events = panic_restore + panic_commit + rollbacks;
            // Issue #1079: recovery-success is a 0–100 percentage.
            // Vacuous (no errors, no recovery) → 100. When errors==0 but
            // recovery_events>0, treat as full recovery (100), never events*100.
            // When errors>0, clamp recovered/errors*100 to [0,100].
            const std::int64_t recovery_success = [&]() -> std::int64_t {
                if (errors == 0)
                    return 100;
                const auto pct = (recovery_events * 100) / errors;
                return static_cast<std::int64_t>(pct > 100 ? 100 : pct);
            }();
            const std::uint64_t total = errors + stored + recovery_events + contract_violations;
            std::int64_t recommendation = 0;
            if (errors > (mutations + queries) / 10 && errors > 0)
                recommendation = 3;
            else if (stored > errors && stored > 0)
                recommendation = 2;
            else if (recovery_events > 0 || errors > 0)
                recommendation = 1;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"primitive-error-count", make_int(static_cast<std::int64_t>(errors))},
                {"error-values-stored", make_int(static_cast<std::int64_t>(stored))},
                {"error-rate", make_int(error_rate)},
                {"recovery-success", make_int(recovery_success)},
                {"panic-recovery-count", make_int(static_cast<std::int64_t>(panic_restore))},
                {"mutation-rollback-count", make_int(static_cast<std::int64_t>(rollbacks))},
                {"contract-violations", make_int(static_cast<std::int64_t>(contract_violations))},
                {"error-schema", make_int(585)},
                {"primitives-error-total", make_int(static_cast<std::int64_t>(total))},
                {"primitives-error-recommendation", make_int(recommendation)},
            };
            return build_hash(kv);
        });

    // Issue #586: EDA/infrastructure primitives registry extension observability
    // hash. Synthesizes registry EDA category counts, foundation primitive
    // activity (#499), SV mutate/feedback closed-loop, and verification_dirty
    // propagation for AI Agent infra-driven verification loops.
    ObservabilityPrims::register_stats_impl(
        "query:eda-primitives-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t eda_registered = ev.primitives_.category_meta_count("eda") +
                                                 ev.primitives_.category_meta_count("sva") +
                                                 ev.primitives_.category_meta_count("verification");
            const std::uint64_t parse =
                m ? m->eda_foundation_parse_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t mutate =
                m ? m->eda_foundation_mutate_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t feedback =
                m ? m->eda_foundation_feedback_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t hooks =
                m ? m->hardware_backend_hook_calls_total.load(std::memory_order_relaxed) : 0;
            auto* ws = ev.workspace_flat();
            const std::uint64_t sv_attempts = ws ? ws->sv_mutate_attempts_total() : 0;
            const std::uint64_t sv_success = ws ? ws->sv_mutate_success_total() : 0;
            std::uint64_t verification_dirty_nodes = 0;
            std::uint64_t sv_nodes = 0;
            if (ws) {
                for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                    // Issue #1134: meta-name based SV/EDA tag coverage.
                    const auto nm = aura::ast::meta(ws->get(id).tag).name;
                    if (nm == "Interface" || nm == "Modport" || nm == "Property" ||
                        nm == "Sequence" || nm == "Assert" || nm == "Covergroup" ||
                        nm == "Coverpoint" || nm == "Constraint" || nm == "Class")
                        ++sv_nodes;
                    if (ws->verification_dirty(id) != 0)
                        ++verification_dirty_nodes;
                }
            }
            const std::int64_t closed_loop_rate_pct =
                sv_attempts > 0 ? static_cast<std::int64_t>((sv_success * 100) / sv_attempts)
                                : (sv_success > 0 ? 100 : 0);
            const std::uint64_t total = eda_registered + parse + mutate + feedback + sv_attempts +
                                        sv_success + verification_dirty_nodes + hooks + sv_nodes;
            std::int64_t recommendation = 0;
            if (eda_registered == 0)
                recommendation = 3;
            else if (parse == 0)
                recommendation = 2;
            else if (feedback == 0 && sv_success == 0)
                recommendation = 1;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"eda-registered-count", make_int(static_cast<std::int64_t>(eda_registered))},
                {"foundation-parse-total", make_int(static_cast<std::int64_t>(parse))},
                {"foundation-mutate-total", make_int(static_cast<std::int64_t>(mutate))},
                {"foundation-feedback-total", make_int(static_cast<std::int64_t>(feedback))},
                {"sv-mutate-attempts", make_int(static_cast<std::int64_t>(sv_attempts))},
                {"sv-mutate-success", make_int(static_cast<std::int64_t>(sv_success))},
                {"verification-dirty-nodes",
                 make_int(static_cast<std::int64_t>(verification_dirty_nodes))},
                {"hardware-hook-calls", make_int(static_cast<std::int64_t>(hooks))},
                {"closed-loop-rate-pct", make_int(closed_loop_rate_pct)},
                {"eda-primitives-schema", make_int(586)},
                {"eda-primitives-total", make_int(static_cast<std::int64_t>(total))},
                {"eda-primitives-recommendation", make_int(recommendation)},
            };
            return build_hash(kv);
        });

    // Issue #587: AI-native primitives development support observability hash.
    // Synthesizes registry metadata coverage, skeleton generation, and
    // introspection activity (#498/#617/#697) for Agent-friendly extension.
    ObservabilityPrims::register_stats_impl(
        "query:primitives-ai-native-stats", [&ev](const auto&) -> EvalValue {
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
            if (auto* m = static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics()))
                m->ai_native_primitive_hits_total.fetch_add(1, std::memory_order_relaxed);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t slots = ev.primitives_.slot_count();
            const std::uint64_t documented = ev.primitives_.documented_meta_count();
            const std::uint64_t schema_doc = ev.primitives_.schema_documented_meta_count();
            const std::uint64_t skeletons =
                m ? m->primitive_skeleton_generations_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t describes = ev.get_primitive_describe_count();
            const std::uint64_t list_meta = ev.get_primitive_list_meta_count();
            const std::uint64_t introspection_hits =
                m ? (m->primitives_by_category_query_total.load(std::memory_order_relaxed) +
                     m->schema_of_primitive_query_total.load(std::memory_order_relaxed) +
                     m->primitives_meta_catalog_query_total.load(std::memory_order_relaxed))
                  : 0;
            const std::uint64_t ai_native_hits =
                m ? m->ai_native_primitive_hits_total.load(std::memory_order_relaxed) : 0;
            const std::int64_t meta_coverage_pct =
                slots > 0 ? static_cast<std::int64_t>((documented * 100) / slots) : 100;
            const std::uint64_t total =
                slots + schema_doc + skeletons + introspection_hits + describes + list_meta;
            std::int64_t recommendation = 0;
            if (meta_coverage_pct < 50)
                recommendation = 3;
            else if (schema_doc < documented / 2)
                recommendation = 2;
            else if (skeletons > 0 || introspection_hits > 0)
                recommendation = 1;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"registry-slots", make_int(static_cast<std::int64_t>(slots))},
                {"schema-documented", make_int(static_cast<std::int64_t>(schema_doc))},
                {"documented-meta", make_int(static_cast<std::int64_t>(documented))},
                {"skeleton-generations", make_int(static_cast<std::int64_t>(skeletons))},
                {"introspection-hits", make_int(static_cast<std::int64_t>(introspection_hits))},
                {"describe-calls", make_int(static_cast<std::int64_t>(describes))},
                {"list-meta-calls", make_int(static_cast<std::int64_t>(list_meta))},
                {"meta-coverage-pct", make_int(meta_coverage_pct)},
                {"ai-native-hits", make_int(static_cast<std::int64_t>(ai_native_hits))},
                {"extension-kit-version",
                 make_int(static_cast<std::int64_t>(kPrimitivesExtensionKitVersion))},
                {"ai-native-schema", make_int(587)},
                {"primitives-ai-native-total", make_int(static_cast<std::int64_t>(total))},
                {"primitives-ai-native-recommendation", make_int(recommendation)},
            };
            return build_hash(kv);
        });

    // Issue #710: verify_tool/diagnostic Guard + StableRef + dirty propagation stats.
    ObservabilityPrims::register_stats_impl(
        "query:verify-tool-guard-stats", [&ev](const auto&) -> EvalValue {
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
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"guard-captures",
                 make_int(static_cast<std::int64_t>(ev.get_verify_tool_guard_captures_total()))},
                {"dirty-propagation", make_int(static_cast<std::int64_t>(
                                          ev.get_verify_tool_dirty_propagations_total()))},
                {"stable-ref-hits",
                 make_int(static_cast<std::int64_t>(ev.get_verify_tool_stable_ref_hits_total()))},
                {"feedback-mutate-success",
                 make_int(static_cast<std::int64_t>(
                     ev.get_verify_tool_feedback_mutate_success_total()))},
            };
            return build_hash(kv);
        });

    // Issue #695: EDA-SV verification closed-loop stress stats.
    ObservabilityPrims::register_stats_impl(
        "query:eda-sv-closedloop-stress-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t cycles =
                m ? m->eda_sv_evolution_cycles_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t converged =
                m ? m->eda_sv_verification_convergence_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t feedback_ok =
                m ? m->eda_sv_feedback_mutate_success_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t ref_inval =
                m ? m->eda_sv_stable_ref_invalidation_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t stub_latency =
                m ? m->eda_sv_commercial_stub_latency_us_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t corruption =
                m ? m->eda_sv_corruption_detected_total.load(std::memory_order_relaxed) : 0;
            std::uint64_t dirty_cost = 0;
            if (auto* ws = ev.workspace_flat()) {
                const auto calls = ws->mark_dirty_upward_call_count();
                const auto nodes = ws->mark_dirty_total_nodes();
                dirty_cost = calls > 0 ? nodes / calls : 0;
            }
            const std::uint64_t convergence_pct =
                cycles > 0 ? (100 * converged / cycles) : (converged > 0 ? 100 : 0);
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"evolution-cycles", make_int(static_cast<std::int64_t>(cycles))},
                {"verification-convergence-rate",
                 make_int(static_cast<std::int64_t>(convergence_pct))},
                {"feedback-mutate-success", make_int(static_cast<std::int64_t>(feedback_ok))},
                {"stable-ref-invalidation-sv", make_int(static_cast<std::int64_t>(ref_inval))},
                {"dirty-traversal-cost", make_int(static_cast<std::int64_t>(dirty_cost))},
                {"commercial-stub-latency", make_int(static_cast<std::int64_t>(stub_latency))},
                {"corruption-detected", make_int(static_cast<std::int64_t>(corruption))},
            };
            return build_hash(kv);
        });

    // Issue #694: SVA structured AST stats.
    ObservabilityPrims::register_stats_impl(
        "query:sv-sva-structure-stats", [&ev](const auto&) -> EvalValue {
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
            std::uint64_t property_count = 0;
            std::uint64_t sequence_count = 0;
            std::uint64_t assert_count = 0;
            std::uint64_t covergroup_count = 0;
            std::uint64_t coverpoint_count = 0;
            std::uint64_t sva_dirty = 0;
            if (auto* ws = ev.workspace_flat()) {
                for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                    switch (ws->get(id).tag) {
                        case aura::ast::NodeTag::Property:
                            ++property_count;
                            break;
                        case aura::ast::NodeTag::Sequence:
                            ++sequence_count;
                            break;
                        case aura::ast::NodeTag::Assert:
                            ++assert_count;
                            break;
                        case aura::ast::NodeTag::Covergroup:
                            ++covergroup_count;
                            break;
                        case aura::ast::NodeTag::Coverpoint:
                            ++coverpoint_count;
                            break;
                        default:
                            break;
                    }
                    if ((ws->verify_dirty(id) & aura::ast::FlatAST::kSvaDirty) != 0)
                        ++sva_dirty;
                }
            }
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t mutate_hits =
                m ? m->sva_structured_mutate_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t sv_attempts =
                ev.workspace_flat() ? ev.workspace_flat()->sv_mutate_attempts_total() : 0;
            const std::uint64_t sv_success =
                ev.workspace_flat() ? ev.workspace_flat()->sv_mutate_success_total() : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"property-count", make_int(static_cast<std::int64_t>(property_count))},
                {"sequence-count", make_int(static_cast<std::int64_t>(sequence_count))},
                {"assert-count", make_int(static_cast<std::int64_t>(assert_count))},
                {"covergroup-count", make_int(static_cast<std::int64_t>(covergroup_count))},
                {"coverpoint-count", make_int(static_cast<std::int64_t>(coverpoint_count))},
                {"sva-dirty-total", make_int(static_cast<std::int64_t>(sva_dirty))},
                {"structured-mutate-hits", make_int(static_cast<std::int64_t>(mutate_hits))},
                {"sv-mutate-attempts", make_int(static_cast<std::int64_t>(sv_attempts))},
                {"sv-mutate-success", make_int(static_cast<std::int64_t>(sv_success))},
            };
            return build_hash(kv);
        });

    // Issue #578: EDA-SV-review closing hash for structured SV IR +
    // query/mutate primitives + dirty propagation completion.
    ObservabilityPrims::register_stats_impl(
        "query:sv-structured-edsl-stats", [&ev](const auto&) -> EvalValue {
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
            std::uint64_t property_count = 0;
            std::uint64_t covergroup_count = 0;
            std::uint64_t coverpoint_count = 0;
            std::uint64_t verification_dirty_nodes = 0;
            std::uint64_t property_weaken_count = 0;
            if (auto* ws = ev.workspace_flat()) {
                for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                    switch (ws->get(id).tag) {
                        case aura::ast::NodeTag::Property:
                            ++property_count;
                            break;
                        case aura::ast::NodeTag::Covergroup:
                            ++covergroup_count;
                            break;
                        case aura::ast::NodeTag::Coverpoint:
                            ++coverpoint_count;
                            break;
                        default:
                            break;
                    }
                    if (ws->verification_dirty(id) != 0)
                        ++verification_dirty_nodes;
                }
                for (const auto& rec : ws->mutation_log_view()) {
                    if (rec.operator_name.find("weaken") != std::string::npos)
                        ++property_weaken_count;
                }
            }
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t pattern_hits = ev.get_pattern_structural_index_hits();
            const std::uint64_t pattern_misses = ev.get_pattern_structural_index_misses();
            const std::uint64_t pattern_denom = pattern_hits + pattern_misses + 1;
            const std::int64_t struct_hit_rate_pct =
                static_cast<std::int64_t>((pattern_hits * 100) / pattern_denom);
            const std::uint64_t sv_attempts =
                ev.workspace_flat() ? ev.workspace_flat()->sv_mutate_attempts_total() : 0;
            const std::uint64_t sv_success =
                ev.workspace_flat() ? ev.workspace_flat()->sv_mutate_success_total() : 0;
            const std::uint64_t structured_hits =
                m ? m->sva_structured_mutate_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t dirty_props = ev.get_verify_tool_dirty_propagations_total();
            const std::uint64_t dirty_up =
                ev.workspace_flat() ? ev.workspace_flat()->mark_dirty_upward_call_count() : 0;
            const std::uint64_t assert_dirty =
                ev.workspace_flat() ? ev.workspace_flat()->verify_assertion_dirty_total() : 0;
            const std::uint64_t coverage_dirty =
                ev.workspace_flat() ? ev.workspace_flat()->verify_coverage_dirty_total() : 0;
            const std::uint64_t sva_dirty =
                ev.workspace_flat() ? ev.workspace_flat()->verify_sva_dirty_total() : 0;
            const std::uint64_t emit_ok =
                m ? m->sv_emit_parse_success_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t emit_fail =
                m ? m->sv_emit_parse_fail_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t hw_hooks =
                m ? m->hardware_backend_hook_calls_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t dirty_verif_propagated =
                dirty_props + verification_dirty_nodes + dirty_up;
            const std::uint64_t total = property_count + covergroup_count + coverpoint_count +
                                        pattern_hits + sv_attempts + sv_success + structured_hits +
                                        dirty_verif_propagated + emit_ok + emit_fail + hw_hooks +
                                        property_weaken_count;
            std::int64_t recommendation = 0;
            if (sv_attempts > 0 && sv_success == 0)
                recommendation = 3;
            else if (emit_fail > emit_ok && emit_fail > 0)
                recommendation = 2;
            else if (structured_hits > 0 || sv_success > 0 || dirty_verif_propagated > 0)
                recommendation = 1;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"sv-struct-pattern-hits", make_int(static_cast<std::int64_t>(pattern_hits))},
                {"sv-struct-pattern-misses", make_int(static_cast<std::int64_t>(pattern_misses))},
                {"sv-struct-hit-rate-pct", make_int(struct_hit_rate_pct)},
                {"coverpoint-mutate-success", make_int(static_cast<std::int64_t>(sv_success))},
                {"property-weaken-count",
                 make_int(static_cast<std::int64_t>(property_weaken_count))},
                {"dirty-verification-propagated",
                 make_int(static_cast<std::int64_t>(dirty_verif_propagated))},
                {"mark-dirty-upward-calls", make_int(static_cast<std::int64_t>(dirty_up))},
                {"verify-assertion-dirty-total", make_int(static_cast<std::int64_t>(assert_dirty))},
                {"verify-coverage-dirty-total",
                 make_int(static_cast<std::int64_t>(coverage_dirty))},
                {"verify-sva-dirty-total", make_int(static_cast<std::int64_t>(sva_dirty))},
                {"emit-compatibility-checks", make_int(static_cast<std::int64_t>(emit_ok))},
                {"emit-parse-fail-count", make_int(static_cast<std::int64_t>(emit_fail))},
                {"hardware-hook-calls", make_int(static_cast<std::int64_t>(hw_hooks))},
                {"structured-mutate-hits", make_int(static_cast<std::int64_t>(structured_hits))},
                {"eda-sv-review-schema", make_int(578)},
                {"sv-structured-edsl-total", make_int(static_cast<std::int64_t>(total))},
                {"sv-structured-edsl-recommendation", make_int(recommendation)},
            };
            return build_hash(kv);
        });

    // Issue #579: verification feedback → structured mutate closed-loop
    // observability (coverage/assert/cex → mutate + dirty wiring).
    ObservabilityPrims::register_stats_impl(
        "query:verification-feedback-loop-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            auto* ws = ev.workspace_flat();
            const std::uint64_t feedback_cycles =
                m ? m->feedback_mutate_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t eda_feedback_ok =
                m ? m->eda_sv_feedback_mutate_success_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t tool_feedback_ok =
                ev.get_verify_tool_feedback_mutate_success_total();
            const std::uint64_t mutate_success_on_feedback = eda_feedback_ok + tool_feedback_ok;
            const std::uint64_t coverage_feedback =
                ws ? ws->verification_coverage_feedback_total() : 0;
            const std::uint64_t assert_failures = ws ? ws->verification_assert_failure_total() : 0;
            const std::uint64_t sv_success = ws ? ws->sv_mutate_success_total() : 0;
            const std::uint64_t cex_dirty = ws ? ws->verify_formal_cex_dirty_total() : 0;
            const std::uint64_t reverify =
                m ? m->verification_loop_success_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t dirty_props = ev.get_verify_tool_dirty_propagations_total();
            const std::uint64_t coverage_improvement_delta =
                coverage_feedback < sv_success ? coverage_feedback : sv_success;
            const std::uint64_t assert_fail_resolved = assert_failures < mutate_success_on_feedback
                                                           ? assert_failures
                                                           : mutate_success_on_feedback;
            const std::uint64_t cex_addressed =
                cex_dirty < mutate_success_on_feedback ? cex_dirty : mutate_success_on_feedback;
            const std::uint64_t total = feedback_cycles + mutate_success_on_feedback +
                                        coverage_improvement_delta + assert_fail_resolved +
                                        cex_addressed + reverify + dirty_props;
            std::int64_t recommendation = 0;
            if (feedback_cycles > 0 && mutate_success_on_feedback == 0)
                recommendation = 3;
            else if (assert_failures > coverage_feedback && assert_failures > 0)
                recommendation = 2;
            else if (mutate_success_on_feedback > 0 || coverage_improvement_delta > 0)
                recommendation = 1;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"feedback-cycles", make_int(static_cast<std::int64_t>(feedback_cycles))},
                {"mutate-success-on-feedback",
                 make_int(static_cast<std::int64_t>(mutate_success_on_feedback))},
                {"coverage-improvement-delta",
                 make_int(static_cast<std::int64_t>(coverage_improvement_delta))},
                {"assert-fail-resolved", make_int(static_cast<std::int64_t>(assert_fail_resolved))},
                {"cex-addressed", make_int(static_cast<std::int64_t>(cex_addressed))},
                {"verification-feedback-schema", make_int(579)},
                {"verification-feedback-loop-total", make_int(static_cast<std::int64_t>(total))},
                {"verification-feedback-loop-recommendation", make_int(recommendation)},
            };
            return build_hash(kv);
        });

    // Issue #496: query:sv-node-stats — native SV NodeTag census +
    // mutate counters for verification closed-loop tuning.
    ObservabilityPrims::register_stats_impl("query:sv-node-stats", [&ev](const auto&) -> EvalValue {
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
        std::uint64_t interface_count = 0;
        std::uint64_t modport_count = 0;
        std::uint64_t property_count = 0;
        std::uint64_t sequence_count = 0;
        std::uint64_t assert_count = 0;
        std::uint64_t covergroup_count = 0;
        std::uint64_t coverpoint_count = 0;
        std::uint64_t constraint_count = 0;
        std::uint64_t class_count = 0;
        std::uint64_t sv_dirty = 0;
        if (auto* ws = ev.workspace_flat()) {
            for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                switch (ws->get(id).tag) {
                    case aura::ast::NodeTag::Interface:
                        ++interface_count;
                        break;
                    case aura::ast::NodeTag::Modport:
                        ++modport_count;
                        break;
                    case aura::ast::NodeTag::Property:
                        ++property_count;
                        break;
                    case aura::ast::NodeTag::Sequence:
                        ++sequence_count;
                        break;
                    case aura::ast::NodeTag::Assert:
                        ++assert_count;
                        break;
                    case aura::ast::NodeTag::Covergroup:
                        ++covergroup_count;
                        break;
                    case aura::ast::NodeTag::Coverpoint:
                        ++coverpoint_count;
                        break;
                    case aura::ast::NodeTag::Constraint:
                        ++constraint_count;
                        break;
                    case aura::ast::NodeTag::Class:
                        ++class_count;
                        break;
                    default:
                        break;
                }
                if ((ws->verify_dirty(id) & aura::ast::FlatAST::kSvaDirty) != 0)
                    ++sv_dirty;
            }
        }
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t mutate_hits =
            m ? m->sva_structured_mutate_hits_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t sv_attempts =
            ev.workspace_flat() ? ev.workspace_flat()->sv_mutate_attempts_total() : 0;
        const std::uint64_t sv_success =
            ev.workspace_flat() ? ev.workspace_flat()->sv_mutate_success_total() : 0;
        const std::uint64_t sv_node_total = interface_count + modport_count + property_count +
                                            sequence_count + assert_count + covergroup_count +
                                            coverpoint_count + constraint_count + class_count;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"interface-count", make_int(static_cast<std::int64_t>(interface_count))},
            {"modport-count", make_int(static_cast<std::int64_t>(modport_count))},
            {"property-count", make_int(static_cast<std::int64_t>(property_count))},
            {"sequence-count", make_int(static_cast<std::int64_t>(sequence_count))},
            {"assert-count", make_int(static_cast<std::int64_t>(assert_count))},
            {"covergroup-count", make_int(static_cast<std::int64_t>(covergroup_count))},
            {"coverpoint-count", make_int(static_cast<std::int64_t>(coverpoint_count))},
            {"constraint-count", make_int(static_cast<std::int64_t>(constraint_count))},
            {"class-count", make_int(static_cast<std::int64_t>(class_count))},
            {"sv-node-total", make_int(static_cast<std::int64_t>(sv_node_total))},
            {"sv-dirty-total", make_int(static_cast<std::int64_t>(sv_dirty))},
            {"structured-mutate-hits", make_int(static_cast<std::int64_t>(mutate_hits))},
            {"sv-mutate-attempts", make_int(static_cast<std::int64_t>(sv_attempts))},
            {"sv-mutate-success", make_int(static_cast<std::int64_t>(sv_success))},
        };
        return build_hash(kv);
    });

    // Issue #582: EDA SV concurrency + atomic batch + fiber safety
    // observability for multi-agent verification closed loops.
    ObservabilityPrims::register_stats_impl(
        "query:eda-concurrency-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            auto* ws = ev.workspace_flat();
            const std::uint64_t boundary_violations = ev.get_boundary_violation_count();
            const std::uint64_t batch_steal = ev.get_atomic_batch_steal_violation();
            const std::uint64_t sv_concurrent_mutate_deadlocks = boundary_violations + batch_steal;
            const std::uint64_t ws_commits = ws ? ws->atomic_batch_commits() : 0;
            const std::uint64_t ev_commits = ev.atomic_batch_count();
            const std::uint64_t rollbacks = ev.atomic_batch_rollbacks();
            const std::uint64_t sv_rollbacks =
                m ? m->atomic_batch_sv_rollback_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t sv_success = ws ? ws->sv_mutate_success_total() : 0;
            // Issue #1078: when rollbacks dominate, net success is 0 (not raw commits).
            const std::uint64_t atomic_batch_sv_gross = ws_commits + ev_commits + sv_success;
            const std::uint64_t atomic_batch_sv_fail = rollbacks + sv_rollbacks;
            const std::uint64_t atomic_batch_sv_success =
                atomic_batch_sv_gross > atomic_batch_sv_fail
                    ? atomic_batch_sv_gross - atomic_batch_sv_fail
                    : 0;
            const std::uint64_t feedback_hits =
                m ? m->feedback_mutate_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t steal_deferred = aura_adaptive_steal_global_deferred_total();
            const std::uint64_t feedback_during_steal_events =
                feedback_hits > 0 && steal_deferred > 0
                    ? (feedback_hits < steal_deferred ? feedback_hits : steal_deferred)
                    : 0;
            std::uint64_t sv_nodes = 0;
            if (ws) {
                for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                    // Issue #1134: meta-name based SV/EDA tag coverage.
                    const auto nm = aura::ast::meta(ws->get(id).tag).name;
                    if (nm == "Interface" || nm == "Modport" || nm == "Property" ||
                        nm == "Sequence" || nm == "Assert" || nm == "Covergroup" ||
                        nm == "Coverpoint" || nm == "Constraint" || nm == "Class")
                        ++sv_nodes;
                }
            }
            const std::uint64_t boundary_violation_on_sv = sv_nodes > 0 ? boundary_violations : 0;
            const std::uint64_t in_fiber = ev.atomic_batch_in_fiber_total();
            const std::uint64_t total = sv_concurrent_mutate_deadlocks + atomic_batch_sv_success +
                                        feedback_during_steal_events + boundary_violation_on_sv +
                                        feedback_hits + steal_deferred + in_fiber;
            std::int64_t recommendation = 0;
            if (sv_concurrent_mutate_deadlocks > 0)
                recommendation = 3;
            else if (steal_deferred > feedback_hits && steal_deferred > 3)
                recommendation = 2;
            else if (atomic_batch_sv_success > 0 || sv_success > 0)
                recommendation = 1;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"sv-concurrent-mutate-deadlocks",
                 make_int(static_cast<std::int64_t>(sv_concurrent_mutate_deadlocks))},
                {"atomic-batch-sv-success",
                 make_int(static_cast<std::int64_t>(atomic_batch_sv_success))},
                {"feedback-during-steal-events",
                 make_int(static_cast<std::int64_t>(feedback_during_steal_events))},
                {"boundary-violation-on-sv",
                 make_int(static_cast<std::int64_t>(boundary_violation_on_sv))},
                {"eda-concurrency-schema", make_int(582)},
                {"eda-concurrency-total", make_int(static_cast<std::int64_t>(total))},
                {"eda-concurrency-recommendation", make_int(recommendation)},
            };
            return build_hash(kv);
        });

    // Issue #581: StableNodeRef + generation_ + dirty propagation
    // scalability for massive SV SoC under AI multi-round iterations.
    ObservabilityPrims::register_stats_impl(
        "query:stable-ref-sv-scale-stats", [&ev](const auto&) -> EvalValue {
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
            auto* ws = ev.workspace_flat();
            const auto& group = ev.arena_group();
            const auto policy = group.auto_compact_policy_stats();
            const std::uint64_t wrap_events = ws ? ws->generation_wrap_count() : 0;
            const std::uint64_t dirty_calls = ws ? ws->mark_dirty_upward_call_count() : 0;
            const std::uint64_t dirty_nodes = ws ? ws->mark_dirty_total_nodes() : 0;
            const std::uint64_t max_depth = ws ? ws->mark_dirty_max_depth_observed() : 0;
            const std::uint64_t stale_ref = ws ? ws->node_gen_stale_access_count() : 0;
            const std::uint64_t invalidations = ws ? ws->stable_ref_invalidations() : 0;
            const std::uint64_t ast_size = ws ? ws->size() : 0;
            std::uint64_t sv_nodes = 0;
            if (ws) {
                for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                    // Issue #1134: meta-name based SV/EDA tag coverage.
                    const auto nm = aura::ast::meta(ws->get(id).tag).name;
                    if (nm == "Interface" || nm == "Modport" || nm == "Property" ||
                        nm == "Sequence" || nm == "Assert" || nm == "Covergroup" ||
                        nm == "Coverpoint" || nm == "Constraint" || nm == "Class")
                        ++sv_nodes;
                }
            }
            const std::uint64_t compact_trigger_count =
                group.auto_compact_trigger_count() + policy.auto_triggers;
            const std::uint64_t dirty_denom = dirty_calls + 1;
            const std::int64_t avg_dirty_walk_depth_on_sv =
                dirty_calls > 0 ? static_cast<std::int64_t>(dirty_nodes / dirty_denom)
                                : static_cast<std::int64_t>(max_depth);
            const std::uint64_t stale_ref_on_large_ast = ast_size >= 100 ? stale_ref : 0;
            const std::uint64_t total = wrap_events + dirty_calls + dirty_nodes + max_depth +
                                        stale_ref + invalidations + sv_nodes +
                                        compact_trigger_count;
            std::int64_t recommendation = 0;
            if (wrap_events > 0 && stale_ref > invalidations)
                recommendation = 3;
            else if (ast_size >= 1000 && avg_dirty_walk_depth_on_sv > 32)
                recommendation = 2;
            else if (sv_nodes > 0 || dirty_calls > 0 || compact_trigger_count > 0)
                recommendation = 1;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"wrap-events", make_int(static_cast<std::int64_t>(wrap_events))},
                {"avg-dirty-walk-depth-on-sv", make_int(avg_dirty_walk_depth_on_sv)},
                {"compact-trigger-count",
                 make_int(static_cast<std::int64_t>(compact_trigger_count))},
                {"stale-ref-on-large-ast",
                 make_int(static_cast<std::int64_t>(stale_ref_on_large_ast))},
                {"stable-ref-sv-scale-schema", make_int(581)},
                {"sv-node-count", make_int(static_cast<std::int64_t>(sv_nodes))},
                {"stable-ref-sv-scale-total", make_int(static_cast<std::int64_t>(total))},
                {"stable-ref-sv-scale-recommendation", make_int(recommendation)},
            };
            return build_hash(kv);
        });

    // Issue #580: Hardware backend emit maturity + commercial interop
    // observability (compatibility pass rate, PPA refresh, incremental
    // emit win, simulator parse success).
    ObservabilityPrims::register_stats_impl(
        "query:hardware-backend-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t parse_ok =
                m ? m->sv_emit_parse_success_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t parse_fail =
                m ? m->sv_emit_parse_fail_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t ppa_refresh =
                m ? m->commercial_reemits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t diff_emits =
                m ? m->sv_diff_emits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t reemits =
                m ? m->commercial_reemits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t sim_success =
                m ? m->commercial_simulator_runs_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t hook_calls =
                m ? m->hardware_backend_hook_calls_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t ppa_savings =
                m ? m->ppa_savings_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t parse_denom = parse_ok + parse_fail + 1;
            const std::int64_t emit_compatibility_pass_rate =
                static_cast<std::int64_t>((parse_ok * 100) / parse_denom);
            const std::uint64_t incremental_denom = reemits + diff_emits + 1;
            const std::int64_t incremental_emit_win =
                static_cast<std::int64_t>((diff_emits * 100) / incremental_denom);
            const std::uint64_t total = parse_ok + parse_fail + ppa_refresh + diff_emits +
                                        sim_success + hook_calls + ppa_savings;
            std::int64_t recommendation = 0;
            if (parse_fail > parse_ok && parse_fail > 0)
                recommendation = 3;
            else if (ppa_refresh > 0 && ppa_savings == 0)
                recommendation = 2;
            else if (emit_compatibility_pass_rate >= 90 || sim_success > 0)
                recommendation = 1;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"emit-compatibility-pass-rate", make_int(emit_compatibility_pass_rate)},
                {"ppa-refresh-count", make_int(static_cast<std::int64_t>(ppa_refresh))},
                {"incremental-emit-win", make_int(incremental_emit_win)},
                {"simulator-parse-success", make_int(static_cast<std::int64_t>(sim_success))},
                {"hardware-backend-schema", make_int(580)},
                {"hardware-backend-total", make_int(static_cast<std::int64_t>(total))},
                {"hardware-backend-recommendation", make_int(recommendation)},
            };
            return build_hash(kv);
        });

    // Issue #698: Hardware backend commercial interop stats.
    ObservabilityPrims::register_stats_impl(
        "query:hardware-backend-commercial-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t hook_calls =
                m ? m->hardware_backend_hook_calls_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t reemits =
                m ? m->commercial_reemits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t feedback_hits =
                m ? m->feedback_mutate_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t parse_ok =
                m ? m->sv_emit_parse_success_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t parse_fail =
                m ? m->sv_emit_parse_fail_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t ppa_savings =
                m ? m->ppa_savings_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t loop_success =
                m ? m->verification_loop_success_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t sim_runs =
                m ? m->commercial_simulator_runs_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t diff_emits =
                m ? m->sv_diff_emits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t convergence_pct =
                (loop_success + parse_fail) > 0 ? (100 * loop_success / (loop_success + parse_fail))
                                                : (loop_success > 0 ? 100 : 0);
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"hook-calls", make_int(static_cast<std::int64_t>(hook_calls))},
                {"commercial-reemits", make_int(static_cast<std::int64_t>(reemits))},
                {"feedback-mutate-hits", make_int(static_cast<std::int64_t>(feedback_hits))},
                {"emit-parse-success", make_int(static_cast<std::int64_t>(parse_ok))},
                {"emit-parse-fail", make_int(static_cast<std::int64_t>(parse_fail))},
                {"ppa-savings", make_int(static_cast<std::int64_t>(ppa_savings))},
                {"verification-loop-convergence",
                 make_int(static_cast<std::int64_t>(convergence_pct))},
                {"commercial-simulator-runs", make_int(static_cast<std::int64_t>(sim_runs))},
                {"sv-diff-emits", make_int(static_cast<std::int64_t>(diff_emits))},
            };
            return build_hash(kv);
        });

    // Issue #663: query:hardware-backend-sv-stats — Hardware backend
    // SV-specific observability primitive (P1 hardware_backend).
    //
    // The issue body Action #4 calls out
    // `query:hardware-backend-sv-stats` with the verbatim field names
    // `hook_calls`, `ppa_reemits`, `verification_triggers`. The 698
    // primitive (query:hardware-backend-commercial-stats) covers
    // SHARED atomics under different naming conventions
    // (hook-calls, commercial-reemits, feedback-mutate-hits).
    //
    // This primitive ships the verbatim-name view requested by the
    // issue body, exposing the SHARED counters under the names the
    // issue body uses for AI-discoverability. No new atomics are
    // added — the existing atomics are reused (the bump sites are
    // already wired from #580/#277/#640/#698).
    //
    // Fields (4 + sentinel):
    //   - hook-calls          hardware_backend_hook_calls_total
    //   - ppa-reemits         commercial_reemits_total
    //   - verification-triggers feedback_mutate_hits_total
    //   - backend-events-total (sum of 3, per-call derivation)
    //   - schema == 663
    //
    // Non-duplicative with #580 (hardware-backend-stats, the
    // general-purpose view with derived pass-rate / recommendation
    // fields), #698 (hardware-backend-commercial-stats, the
    // commercial-loop view with ppa_savings + simulator_runs +
    // diff_emits). #663 ships the verbatim-name view the issue
    // body requested specifically.
    ObservabilityPrims::register_stats_impl(
        "query:hardware-backend-sv-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::int64_t hook_calls =
                m ? static_cast<std::int64_t>(
                        m->hardware_backend_hook_calls_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t ppa_reemits =
                m ? static_cast<std::int64_t>(
                        m->commercial_reemits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t verification_triggers =
                m ? static_cast<std::int64_t>(
                        m->feedback_mutate_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t events_total = hook_calls + ppa_reemits + verification_triggers;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"hook-calls", make_int(hook_calls)},
                {"ppa-reemits", make_int(ppa_reemits)},
                {"verification-triggers", make_int(verification_triggers)},
                {"backend-events-total", make_int(events_total)},
                {"schema", make_int(663)},
            };
            return build_hash(kv);
        });

    // Issue #664: query:sv-defuse-stats — SV DefUse incremental
    // observability (P1 EDA-SV).
    //
    // The 3 counters track the structured DefUse build + incremental
    // update activity for nested Interface/Modport scopes
    // (the issue body Action #3):
    //   - nested-modports      sv_defuse_nested_modports_total
    //       Bumped per DefUse build that discovers a Modport child
    //       of an Interface (i.e. a nested modport at depth >= 1).
    //   - cross-refs           sv_defuse_cross_refs_total
    //       Bumped per DefUse use-record that resolves to an
    //       Interface/Modport symbol defined in another scope.
    //   - incremental-updates  sv_defuse_incremental_updates_total
    //       Bumped per DefUse incremental rebuild triggered by
    //       an SV structural mutate.
    //   - defuse-events-total
    //       Sum of the 3 (per-call derivation, not a separate
    //       atomic). Lets dashboards show overall DefUse-event
    //       volume at a glance.
    //   - schema == 664
    //
    // Non-duplicative with #317 DefUse scope tracking, #337
    // ShapeProfiler std::flat_map, #640/#663 verification feedback
    // closed loop, #691 per-fn defuse index metrics. #664 covers
    // the structured DefUse INCREMENTAL + NESTED/CROSS-ref
    // observability specifically.
    ObservabilityPrims::register_stats_impl(
        "query:sv-defuse-stats", [&ev](const auto&) -> EvalValue {
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
            const std::int64_t nested =
                static_cast<std::int64_t>(ev.get_sv_defuse_nested_modports());
            const std::int64_t cross = static_cast<std::int64_t>(ev.get_sv_defuse_cross_refs());
            const std::int64_t incr =
                static_cast<std::int64_t>(ev.get_sv_defuse_incremental_updates());
            const std::int64_t events_total = nested + cross + incr;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"nested-modports", make_int(nested)},
                {"cross-refs", make_int(cross)},
                {"incremental-updates", make_int(incr)},
                {"defuse-events-total", make_int(events_total)},
                {"schema", make_int(664)},
            };
            return build_hash(kv);
        });

    // Issue #665: query:sv-stability-stats — SV stability observability
    // (P1 EDA-SV scalability).
    //
    // The 3 counters track SV-tagged stability + scale activity
    // called out in issue body Action #4:
    //   - dirty-traversal-depth   sv_dirty_traversal_depth_total
    //       Cumulative depth summed across all mark_dirty_upward
    //       calls on SV-tagged nodes (Interface/Modport/SVA).
    //       Stored as sum so the primitive can compute average in
    //       O(1) (avg = total / calls, derives from this primitive).
    //       The "dirty_traversal_depth_avg" metric from Action #4
    //       can be derived by the user as
    //         depth_total / sv_dirty_traversal_calls_total
    //       (the latter is bumped by a separate helper if needed).
    //   - generation-wrap-sv      sv_generation_wrap_total
    //       Bumped each time generation_wrap_sv_hits is detected
    //       on an SV-tagged StableRef invalidation. The
    //       "generation_wrap_sv_hits" counter from Action #4.
    //   - stable-ref-invalidation-sv
    //                            sv_stable_ref_invalidation_total
    //       Bumped each time a StableRef tied to an SV-tagged node
    //       (Interface/Modport/SVA) becomes invalid. The
    //       "stable_ref_invalidation_sv" counter from Action #4.
    //   - stability-events-total  (sum of 3, per-call derivation,
    //       not a separate atomic)
    //   - schema == 665
    //
    // Non-duplicative with #641 StableRef cross-fiber provenance,
    // #368/#392 generation fix, #336 mark_dirty_upward fast-path,
    // #642 arena, #664 SV DefUse. #665 covers the SV-specific
    // stability observability surface.
    ObservabilityPrims::register_stats_impl(
        "query:sv-stability-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t depth = static_cast<std::int64_t>(ev.get_sv_dirty_traversal_depth());
            const std::int64_t wrap = static_cast<std::int64_t>(ev.get_sv_generation_wrap());
            const std::int64_t inval =
                static_cast<std::int64_t>(ev.get_sv_stable_ref_invalidation());
            const std::int64_t events_total = depth + wrap + inval;
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
            insert_kv("dirty-traversal-depth", depth);
            insert_kv("generation-wrap-sv", wrap);
            insert_kv("stable-ref-invalidation-sv", inval);
            insert_kv("stability-events-total", events_total);
            insert_kv("schema", 665);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #667: query:primitives-apply-stats — list/map/filter
    // apply hot-path observability (P1 stdlib-impl performance).
    //
    // The 3 counters track the list/map/filter apply_unary /
    // apply_pred / apply_binary hot path called out in issue
    // body Action #4 (lookup_hits / closure_calls / fastpath_win):
    //   - lookup-hits           primitives_apply_lookup_hits_total
    //       Bumped per slot_lookup_fast call inside the apply
    //       helpers (per-element fast-path lookups). The
    //       "lookup_hits" counter from Action #4.
    //   - closure-calls         primitives_apply_closure_calls_total
    //       Bumped per apply_closure call inside the apply
    //       helpers (closure path, NOT primitive path). The
    //       "closure_calls" counter from Action #4.
    //   - fastpath-wins         primitives_apply_fastpath_wins_total
    //       Bumped when slot_lookup_fast returns a non-null
    //       PrimFn* (successful slot→fn dispatch — the
    //       fastpath actually won). The "fastpath_win"
    //       counter from Action #4.
    //   - apply-events-total    (sum of 3, per-call derivation,
    //       not a separate atomic)
    //   - schema == 667
    //
    // Non-duplicative with #479 per-slot fastpath hit breakdown,
    // #480 PrimMeta, #614 hotpath stability, #643 declarative
    // macro, #633 demands. #667 covers the LIST/MAP/FILTER
    // apply hot-path observability specifically.
    ObservabilityPrims::register_stats_impl(
        "query:primitives-apply-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t lookup_hits =
                static_cast<std::int64_t>(ev.get_primitives_apply_lookup_hits());
            const std::int64_t closure_calls =
                static_cast<std::int64_t>(ev.get_primitives_apply_closure_calls());
            const std::int64_t fastpath_wins =
                static_cast<std::int64_t>(ev.get_primitives_apply_fastpath_wins());
            const std::int64_t events_total = lookup_hits + closure_calls + fastpath_wins;
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
            insert_kv("lookup-hits", lookup_hits);
            insert_kv("closure-calls", closure_calls);
            insert_kv("fastpath-wins", fastpath_wins);
            insert_kv("apply-events-total", events_total);
            insert_kv("schema", 667);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #706: Scheduler StealBudget adaptive bias + LLM tail stats.
    ObservabilityPrims::register_stats_impl(
        "query:scheduler-stealbudget-adaptive-stats", [&ev](const auto&) -> EvalValue {
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
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"mutation-bias-hits",
                 make_int(static_cast<std::int64_t>(aura_adaptive_steal_mutation_bias_hits()))},
                {"outermost-preferred",
                 make_int(static_cast<std::int64_t>(aura_adaptive_steal_outermost_preferred()))},
                {"llm-tail-reductions",
                 make_int(static_cast<std::int64_t>(aura_adaptive_steal_llm_tail_reductions()))},
                {"deferred-pressure-boosts", make_int(static_cast<std::int64_t>(
                                                 aura_adaptive_steal_deferred_pressure_boosts()))},
                {"global-deferred-mutation-total",
                 make_int(static_cast<std::int64_t>(aura_adaptive_steal_global_deferred_total()))},
            };
            return build_hash(kv);
        });

    // Issue #500: query:work-steal-stats — consolidated work-stealing +
    // MutationBoundary outermost-depth observability for Agent loops.
    ObservabilityPrims::register_stats_impl(
        "query:work-steal-stats", [&ev](const auto&) -> EvalValue {
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
            const std::uint64_t steal_attempts = aura_work_steal_attempts_total();
            const std::uint64_t steal_successes = aura_work_steal_successes_total();
            const std::uint64_t deferred = aura_adaptive_steal_global_deferred_total();
            const std::uint64_t ring_attempts = aura_adaptive_steal_ring_attempts();
            const std::uint64_t ring_successes = aura_adaptive_steal_ring_successes();
            const std::uint64_t outermost_pref = aura_adaptive_steal_outermost_preferred();
            const std::uint64_t migration_attempts = ev.get_mutation_steal_attempts();
            const std::uint64_t steal_violations = ev.get_mutation_steal_violation_count();
            const std::uint64_t boundary_depth = Evaluator::mutation_boundary_depth();
            const std::uint64_t work_steal_total =
                steal_attempts + steal_successes + deferred + migration_attempts + ring_attempts;
            std::int64_t recommendation = 0;
            if (steal_violations > 0)
                recommendation = 3;
            else if (deferred > steal_successes && deferred > 5)
                recommendation = 2;
            else if (steal_attempts > 0 && steal_successes == 0)
                recommendation = 1;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"steal-attempts", make_int(static_cast<std::int64_t>(steal_attempts))},
                {"steal-successes", make_int(static_cast<std::int64_t>(steal_successes))},
                {"steal-deferred-mutation", make_int(static_cast<std::int64_t>(deferred))},
                {"steal-violations", make_int(static_cast<std::int64_t>(steal_violations))},
                {"mutation-boundary-depth", make_int(static_cast<std::int64_t>(boundary_depth))},
                {"fiber-migration-attempts",
                 make_int(static_cast<std::int64_t>(migration_attempts))},
                {"ring-steal-attempts", make_int(static_cast<std::int64_t>(ring_attempts))},
                {"ring-steal-successes", make_int(static_cast<std::int64_t>(ring_successes))},
                {"outermost-preferred", make_int(static_cast<std::int64_t>(outermost_pref))},
                {"work-steal-total", make_int(static_cast<std::int64_t>(work_steal_total))},
                {"work-steal-recommendation", make_int(recommendation)},
            };
            return build_hash(kv);
        });

    // Issue #652 / #707: Per-fiber MutationStack / YieldCheckpoint pool stats.
    // Fields: pool-hits, lazy-allocs, max-depth, churn-reductions,
    // size-mismatches-caught, growth-warnings, restamps, schema == 652.
    ObservabilityPrims::register_stats_impl(
        "query:per-fiber-stack-pool-stats", [&ev](const auto&) -> EvalValue {
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
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"pool-hits",
                 make_int(static_cast<std::int64_t>(aura_per_fiber_stack_pool_hits()))},
                {"lazy-allocs",
                 make_int(static_cast<std::int64_t>(aura_per_fiber_stack_pool_lazy_allocs()))},
                {"max-depth",
                 make_int(static_cast<std::int64_t>(aura_per_fiber_stack_pool_max_depth()))},
                {"churn-reductions",
                 make_int(static_cast<std::int64_t>(aura_per_fiber_stack_pool_churn_reductions()))},
                {"size-mismatches-caught",
                 make_int(static_cast<std::int64_t>(aura_per_fiber_stack_pool_size_mismatches()))},
                {"growth-warnings",
                 make_int(static_cast<std::int64_t>(aura_per_fiber_stack_pool_growth_warnings()))},
                {"restamps",
                 make_int(static_cast<std::int64_t>(aura_per_fiber_stack_pool_restamps()))},
                {"schema", make_int(652)},
            };
            return build_hash(kv);
        });

    // Issue #708: AOT hot-reload refcount swap + region isolation stats.
    ObservabilityPrims::register_stats_impl(
        "query:aot-reload-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t attempts =
                m ? m->aot_reload_attempts_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t success =
                m ? m->aot_hot_update_success_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t stale =
                m ? m->aot_stale_reject_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t swaps =
                m ? m->aot_refcount_swaps_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t region_viol =
                m ? m->aot_region_mismatch_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t deopt_steal =
                m ? m->aot_deopt_on_steal_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t concurrent_safe =
                m ? m->aot_concurrent_safe_reloads_.load(std::memory_order_relaxed) : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"reload-attempts", make_int(static_cast<std::int64_t>(attempts))},
                {"reload-success", make_int(static_cast<std::int64_t>(success))},
                {"stale-rejected", make_int(static_cast<std::int64_t>(stale))},
                {"refcount-swaps", make_int(static_cast<std::int64_t>(swaps))},
                {"region-violations", make_int(static_cast<std::int64_t>(region_viol))},
                {"deopt-on-steal", make_int(static_cast<std::int64_t>(deopt_steal))},
                {"concurrent-safe-reloads", make_int(static_cast<std::int64_t>(concurrent_safe))},
            };
            return build_hash(kv);
        });

    // Issue #653 / #708: AOT checkpoint / bridge_epoch version drift stats.
    // Fields: checkpoint-version-drifts, bridge-epoch-mismatches,
    // deopt-on-steal, func-table-epoch-swaps, schema == 653.
    ObservabilityPrims::register_stats_impl(
        "query:aot-checkpoint-version-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t drifts =
                m ? m->aot_checkpoint_version_drifts_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t bridge_mismatch =
                m ? m->aot_bridge_epoch_mismatches_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t deopt =
                m ? m->aot_deopt_on_steal_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t swaps =
                m ? m->aot_refcount_swaps_.load(std::memory_order_relaxed) : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"checkpoint-version-drifts", make_int(static_cast<std::int64_t>(drifts))},
                {"bridge-epoch-mismatches", make_int(static_cast<std::int64_t>(bridge_mismatch))},
                {"deopt-on-steal", make_int(static_cast<std::int64_t>(deopt))},
                {"func-table-epoch-swaps", make_int(static_cast<std::int64_t>(swaps))},
                {"schema", make_int(653)},
            };
            return build_hash(kv);
        });

    // Issue #693: Hardware backend SV commercial closed-loop stats.
    ObservabilityPrims::register_stats_impl(
        "query:hardware-backend-sv-closedloop-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t hook_calls =
                m ? m->hardware_backend_hook_calls_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t reemits =
                m ? m->commercial_reemits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t feedback_hits =
                m ? m->feedback_mutate_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t ppa_savings =
                m ? m->ppa_savings_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t loop_success =
                m ? m->verification_loop_success_total.load(std::memory_order_relaxed) : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"hook-calls", make_int(static_cast<std::int64_t>(hook_calls))},
                {"commercial-reemits", make_int(static_cast<std::int64_t>(reemits))},
                {"feedback-mutate-hits", make_int(static_cast<std::int64_t>(feedback_hits))},
                {"ppa-savings", make_int(static_cast<std::int64_t>(ppa_savings))},
                {"verification-loop-success", make_int(static_cast<std::int64_t>(loop_success))},
            };
            return build_hash(kv);
        });

    // Issue #692: ADT exhaustiveness + pattern provenance typed-mutation stats.
    ObservabilityPrims::register_stats_impl(
        "query:adt-exhaustiveness-typed-mutate-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t rechecks =
                m ? m->adt_exhaust_rechecks_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t non_exhaust =
                m ? m->adt_non_exhaustive_caught_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t pattern_refreshes =
                m ? m->adt_pattern_narrow_refreshes_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t complete =
                m ? m->adt_pattern_provenance_complete_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t completeness_pct = pattern_refreshes > 0
                                                       ? (100 * complete / pattern_refreshes)
                                                       : (complete > 0 ? 100 : 0);
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"post-mutate-rechecks", make_int(static_cast<std::int64_t>(rechecks))},
                {"non-exhaustive-caught", make_int(static_cast<std::int64_t>(non_exhaust))},
                {"pattern-narrow-refreshes",
                 make_int(static_cast<std::int64_t>(pattern_refreshes))},
                {"provenance-completeness", make_int(static_cast<std::int64_t>(completeness_pct))},
            };
            return build_hash(kv);
        });

    // Issue #691: CoercionMap + NarrowingRecord provenance stats.
    ObservabilityPrims::register_stats_impl(
        "query:coercion-narrowing-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t opportunities =
                m ? m->coercion_post_narrow_elim_opportunities_total.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t blame =
                m ? m->coercion_narrow_blame_chain_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t elim =
                m ? m->coercion_cast_elim_from_narrow_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t narrow_hits =
                m ? m->coercion_narrow_evidence_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t completeness_pct =
                opportunities > 0 ? (100 * blame / opportunities) : (blame > 0 ? 100 : 0);
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"post-narrow-elim-opportunities",
                 make_int(static_cast<std::int64_t>(opportunities))},
                {"blame-chain-hits", make_int(static_cast<std::int64_t>(blame))},
                {"cast-elim-from-narrow", make_int(static_cast<std::int64_t>(elim + narrow_hits))},
                {"blame-chain-completeness", make_int(static_cast<std::int64_t>(completeness_pct))},
            };
            return build_hash(kv);
        });

    // Issue #690: constraint typed-mutation reverify + blame stats.
    ObservabilityPrims::register_stats_impl(
        "query:constraint-typed-mutate-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t reverify_scans =
                m ? m->delta_conflict_reverify_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t conflicts = ev.get_cross_delta_conflicts_caught();
            const std::uint64_t blame_complete =
                m ? m->constraint_blame_chain_complete_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t truncated =
                m ? m->reverify_truncated_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t detected =
                m ? m->delta_conflict_detected_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t completeness_pct =
                detected > 0 ? (100 * blame_complete / detected) : (blame_complete > 0 ? 100 : 0);
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"reverify-scans", make_int(static_cast<std::int64_t>(reverify_scans))},
                {"cross-delta-conflicts-caught", make_int(static_cast<std::int64_t>(conflicts))},
                {"blame-chain-completeness", make_int(static_cast<std::int64_t>(completeness_pct))},
                {"truncated-reverify", make_int(static_cast<std::int64_t>(truncated))},
            };
            return build_hash(kv);
        });

    // Issue #688: linear OwnershipEnv post-mutate typed-mutation stats.
    ObservabilityPrims::register_stats_impl(
        "query:linear-ownership-typed-mutate-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t revalidates =
                m ? m->linear_post_mutate_revalidations_total.load(std::memory_order_relaxed) +
                        m->linear_dirty_revalidate_count.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t violations =
                m ? m->linear_violations_caught_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t enforcements =
                m ? m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t safe_fallbacks =
                m ? m->linear_typed_mutate_safe_fallbacks.load(std::memory_order_relaxed) +
                        m->compiler_closure_safe_fallbacks.load(std::memory_order_relaxed)
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"post-mutate-revalidates", make_int(static_cast<std::int64_t>(revalidates))},
                {"violations-caught", make_int(static_cast<std::int64_t>(violations))},
                {"enforcement-hits", make_int(static_cast<std::int64_t>(enforcements))},
                {"safe-fallbacks", make_int(static_cast<std::int64_t>(safe_fallbacks))},
            };
            return build_hash(kv);
        });

    // Issue #672: query:linear-ownership-enforcement-stats
    // — P0 runtime invariant enforcement observability for
    // Linear ownership + GuardShape under concurrent fiber
    // mutation. Exposes the 4 enforcement atomics (defined
    // on CompilerMetrics in observability_metrics.h via
    // #610/#638/#683) as a single Agent-discoverable
    // surface. Pre-#672 the counters were scattered across
    // 4 query primitives (mutation / safety / runtime /
    // incremental / gc / typed-mutate) with no
    // single-stop enforcement dashboard.
    //
    // Schema (6 fields + sentinel):
    //   - post-mutate-enforcements
    //       linear_post_mutate_enforcements_total — bumped
    //       by bump_linear_post_mutate_enforcement() on
    //       every successful Guard exit (#672 wiring)
    //   - violations-caught
    //       linear_violations_caught_total — bumped by
    //       bump_linear_ownership_violation() + by
    //       record_linear_gc_probe on violation
    //   - deopt-on-mismatch
    //       linear_deopt_on_mismatch_total — bumped by
    //       bump_linear_ownership_violation() (the violation
    //       implies a deopt). Pairs with violations-caught
    //       for violation_rate derivation.
    //   - check-passes
    //       linear_check_pass_count_ — bumped by
    //       bump_linear_ownership_pass() on every successful
    //       check_linear_ownership_for_ref call. Agent
    //       computes violation_rate = violations /
    //       (violations + passes).
    //   - leak-prevented
    //       linear_leak_prevented_total — bumped by
    //       bump_linear_leak_prevented() when a leaked
    //       Linear binding is caught before eval.
    //   - recommended-action
    //       0 = no action (no violations, no leaks), 1 =
    //       tighten policy (violations > 0), 2 = audit
    //       linear-bindings (leaks > 0). Triggered when
    //       any violation or leak counter > 0.
    //   - schema == 672
    //
    // Non-duplicative with #610 / #638 / #683 / #688 (which
    // expose the per-axis counters in their respective
    // primitives). #672 carves out the enforcement-axis
    // dashboard — useful when the Agent's question is
    // "are linear-ownership invariants being enforced?"
    // rather than "how is the post-mutate revalidate
    // surface broken down by category?".
    ObservabilityPrims::register_stats_impl(
        "query:linear-ownership-enforcement-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t enforcements =
                m ? m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t violations =
                m ? m->linear_violations_caught_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t deopts =
                m ? m->linear_deopt_on_mismatch_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t passes =
                m ? m->linear_check_pass_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t leaks =
                m ? m->linear_leak_prevented_total.load(std::memory_order_relaxed) : 0;
            std::int64_t recommended_action = 0;
            if (violations > 0)
                recommended_action = 1; // tighten policy
            else if (leaks > 0)
                recommended_action = 2; // audit linear-bindings
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"post-mutate-enforcements", make_int(static_cast<std::int64_t>(enforcements))},
                {"violations-caught", make_int(static_cast<std::int64_t>(violations))},
                {"deopt-on-mismatch", make_int(static_cast<std::int64_t>(deopts))},
                {"check-passes", make_int(static_cast<std::int64_t>(passes))},
                {"leak-prevented", make_int(static_cast<std::int64_t>(leaks))},
                {"recommended-action", make_int(recommended_action)},
                {"schema", make_int(672)},
            };
            return build_hash(kv);
        });

    // Issue #740: query:linear-jit-safety-stats — JIT L2 linear
    // ownership + Arena/DropOp/GC root re-sync after invalidate.
    // Fields (3 + sentinel):
    //   - arena-forced-post-mutate
    //   - drop-op-emitted
    //   - gc-root-resync
    //   - schema == 740
    ObservabilityPrims::register_stats_impl(
        "query:linear-jit-safety-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t arena_forced =
                m ? m->linear_jit_arena_forced_post_mutate_total.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t drop_emitted =
                m ? m->linear_jit_drop_op_emitted_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t gc_resync =
                m ? m->linear_jit_gc_root_resync_total.load(std::memory_order_relaxed) : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"arena-forced-post-mutate", make_int(static_cast<std::int64_t>(arena_forced))},
                {"drop-op-emitted", make_int(static_cast<std::int64_t>(drop_emitted))},
                {"gc-root-resync", make_int(static_cast<std::int64_t>(gc_resync))},
                {"schema", make_int(740)},
            };
            return build_hash(kv);
        });

    // Issue #686: ShapeProfiler ring history + Value v2 dispatch +
    // DirtyAware pass short-circuit synergy stats.
    ObservabilityPrims::register_stats_impl(
        "query:shape-value-pass-stats", [&ev](const auto&) -> EvalValue {
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
            const std::uint64_t jitter =
                shape::history_jitter_reduction_count.load(std::memory_order_relaxed);
            const std::uint64_t dispatch =
                types::value_dispatch_hit_count.load(std::memory_order_relaxed) +
                types::value_dispatch_miss_count.load(std::memory_order_relaxed) +
                types::value_contract_violation_count.load(std::memory_order_relaxed) +
                types::v2_string_collision_attempts.load(std::memory_order_relaxed) +
                types::value_classify_call_count.load(std::memory_order_relaxed) +
                shape::inline_shape_ref_dispatch_count.load(std::memory_order_relaxed);
            const std::uint64_t dirty_skip = ev.get_passes_skipped_type_dirty();
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t shape_sync =
                m ? m->shape_ids_sync_hits.load(std::memory_order_relaxed) : 0;
            (void)shape_sync;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"history-jitter-reduction", make_int(static_cast<std::int64_t>(jitter))},
                {"dispatch-stats", make_int(static_cast<std::int64_t>(dispatch))},
                {"dirty-shortcircuit-savings", make_int(static_cast<std::int64_t>(dirty_skip))},
                {"consteval-hits",
                 make_int(static_cast<std::int64_t>(shape::k_shape_value_consteval_hits))},
            };
            return build_hash(kv);
        });

    // Issue #680: Define mutate IR/JIT/bridge invalidation observability.
    ObservabilityPrims::register_stats_impl(
        "query:define-mutate-ir-invalidation-stats", [&ev](const auto&) -> EvalValue {
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t stale_bridge =
                m ? m->closure_stale_returns.load(std::memory_order_relaxed) : 0;
            const std::uint64_t recompile_savings =
                m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"precise-inval-hits",
                 make_int(static_cast<std::int64_t>(ev.precise_define_inval_hits()))},
                {"stale-bridge-caught", make_int(static_cast<std::int64_t>(stale_bridge))},
                {"recompile-savings", make_int(static_cast<std::int64_t>(recompile_savings))},
            };
            return build_hash(kv);
        });

    ObservabilityPrims::register_stats_impl(
        "query:span-lifetime-stats", [&ev](const auto&) -> EvalValue {
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
            auto* ws = ev.workspace_flat();
            if (!ws)
                return make_void();
            const std::uint64_t unsafe_access =
                ws->children_call_count() + ws->parent_of_call_count();
            const std::uint64_t safe_hits =
                ws->children_safe_view_count() + ws->parent_safe_view_count();
            const std::uint64_t cow_inv = ws->bump_generation_count();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"unsafe-access-attempts", make_int(static_cast<std::int64_t>(unsafe_access))},
                {"safe-view-hits", make_int(static_cast<std::int64_t>(safe_hits))},
                {"cow-invalidation-detected", make_int(static_cast<std::int64_t>(cow_inv))},
            };
            return build_hash(kv);
        });

    ObservabilityPrims::register_stats_impl(
        "query:mutation-audit-log", [&ev](std::span<const EvalValue> a) -> EvalValue {
            std::size_t limit = 10;
            if (!a.empty() && is_int(a[0]) && as_int(a[0]) > 0)
                limit = static_cast<std::size_t>(as_int(a[0]));
            const auto total = ev.mutation_audit_total();
            const auto seq = ev.mutation_audit_seq();
            EvalValue result = make_void();
            std::size_t emitted = 0;
            for (std::size_t i = 0; i < limit && i < Evaluator::kMutationAuditRingSize; ++i) {
                if (seq <= i)
                    break;
                const auto& entry = ev.mutation_audit_entry_at(seq - 1 - i);
                if (entry.seq == 0)
                    continue;
                auto line =
                    std::format("seq={} fiber={} op={} target={} nodes={} epoch_delta={} ts={}",
                                entry.seq, entry.fiber_id, entry.op, entry.target_node,
                                entry.nodes_changed, entry.epoch_delta, entry.timestamp_ms);
                auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back(std::move(line));
                auto pid = ev.pairs_.size();
                ev.pairs_.push_back({make_string(sidx), result});
                result = make_pair(pid);
                ++emitted;
            }
            (void)total;
            (void)emitted;
            return result;
        });

    // Issue #668: query:primitives-regex-error-stats — math regex
    // primitive error observability (P1 stdlib-impl error
    // consistency).
    //
    // Counter tracks every PRIM_ERROR invocation inside the
    // regex-match? / regex-find / regex-replace / regex-split
    // primitives — both pre-try validation failures
    // (type-mismatch / OOB string index) and post-try
    // invalid-regex-syntax failures. The counter is bumped
    // alongside the general primitive_error_count_ so the AI
    // Agent can compute the ratio (regex / total) to gauge
    // regex-primitive error surface.
    //
    // Schema:
    //   - regex-errors      primitives_regex_error_total (the only
    //                       per-primitive axis currently exposed;
    //                       per-primitive breakdown deferred —
    //                       would require per-primitive atomics on
    //                       CompilerMetrics and is a separate
    //                       follow-up)
    //   - schema            668
    ObservabilityPrims::register_stats_impl(
        "query:primitives-regex-error-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t regex_errors =
                static_cast<std::int64_t>(ev.get_primitives_regex_error_total());
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const std::string& k_str, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k_str)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::uint64_t at = 0; at < hcap; ++at) {
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
            insert_kv("regex-errors", regex_errors);
            insert_kv("schema", 668);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

} // namespace aura::compiler::primitives_detail