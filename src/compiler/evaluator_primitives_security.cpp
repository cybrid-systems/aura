// evaluator_primitives_security.cpp — Issue #676 security/sandbox/audit primitives.

module;

#include "runtime_shared.h"
#include "compiler/observability_metrics.h"
#include "shape.h"
#include "value_tags.h"
#include "security_capabilities.h"
#include "serve/http_health.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using namespace types;
using namespace security;

void register_security_primitives(PrimRegistrar add, Evaluator& ev) {

    add("security:set-sandbox-mode!", [&ev](std::span<const EvalValue> a) -> EvalValue {
        const bool old = ev.sandbox_mode();
        if (!a.empty() && is_bool(a[0]))
            ev.set_sandbox_mode(as_bool(a[0]));
        return make_bool(old);
    });

    add("security:sandbox-mode?", [&ev](const auto&) -> EvalValue {
        return make_bool(ev.sandbox_mode());
    });

    add("security:grant-capability!", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0])) {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                      "security:grant-capability!: requires capability name",
                                      ev.primitive_error_counter_ptr());
        }
        const auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        ev.grant_capability(ev.string_heap_[idx]);
        return make_bool(true);
    });

    add("query:security-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"sandbox-mode", make_bool(ev.sandbox_mode())},
            {"capability-denials", make_int(static_cast<std::int64_t>(ev.capability_denial_count()))},
            {"mutation-audit-total", make_int(static_cast<std::int64_t>(ev.mutation_audit_total()))},
            {"granted-capabilities", make_int(static_cast<std::int64_t>(ev.granted_capability_count()))},
        };
        return build_hash(kv);
    });

    // (query:mutation-audit-log) — Issue #676: exportable security
    // audit ring of recent successful structural mutations.
    // Optional arg N limits entries (default 10).
    add("query:deployment-stats", [&ev](const auto&) -> EvalValue {
        const int http_port = aura::serve::http::port_from_env();
        const char* runtime_dir = std::getenv("AURA_RUNTIME_DIR");
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
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
    add("query:nested-guard-atomic-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
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
    add("query:compiler-closure-inval-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        const auto* m =
            static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t stale_bridge = m
            ? m->closure_stale_returns.load(std::memory_order_relaxed)
            : 0;
        const std::uint64_t epoch_mismatch = m
            ? m->compiler_closure_epoch_mismatch_hits.load(std::memory_order_relaxed)
            : 0;
        const std::uint64_t safe_fallbacks = m
            ? m->compiler_closure_safe_fallbacks.load(std::memory_order_relaxed)
            : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"stale-bridge-caught", make_int(static_cast<std::int64_t>(stale_bridge))},
            {"epoch-mismatch-hits", make_int(static_cast<std::int64_t>(epoch_mismatch))},
            {"safe-fallbacks", make_int(static_cast<std::int64_t>(safe_fallbacks))},
        };
        return build_hash(kv);
    });

    // Issue #682: compiler IRClosure/EnvId GC root coordination stats.
    add("query:compiler-gc-root-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        const auto* m =
            static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t ir_roots = m
            ? m->ir_closure_roots_registered.load(std::memory_order_relaxed)
            : 0;
        const std::uint64_t root_miss = m
            ? m->hotswap_root_miss.load(std::memory_order_relaxed)
            : 0;
        const std::uint64_t defer_count = m
            ? m->compiler_gc_safepoint_defer_count.load(std::memory_order_relaxed)
            : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"ir-closure-roots-registered",
             make_int(static_cast<std::int64_t>(ir_roots))},
            {"hotswap-root-miss", make_int(static_cast<std::int64_t>(root_miss))},
            {"safepoint-defer-count", make_int(static_cast<std::int64_t>(defer_count))},
        };
        return build_hash(kv);
    });

    // Issue #683: linear ownership + GC safepoint / steal / re-lower stats.
    add("query:linear-ownership-gc-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        const auto* m =
            static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t safepoint_v = m
            ? m->linear_gc_safepoint_violations.load(std::memory_order_relaxed)
            : 0;
        const std::uint64_t steal_enf = m
            ? m->linear_steal_enforced.load(std::memory_order_relaxed)
            : 0;
        const std::uint64_t relower_hits = m
            ? m->linear_relower_revalidate_hits.load(std::memory_order_relaxed)
            : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"safepoint-violations", make_int(static_cast<std::int64_t>(safepoint_v))},
            {"steal-enforced", make_int(static_cast<std::int64_t>(steal_enf))},
            {"relower-revalidate-hits",
             make_int(static_cast<std::int64_t>(relower_hits))},
        };
        return build_hash(kv);
    });

    // Issue #684: IRSoA full wiring incremental stats hash.
    add("query:irsoa-incremental-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        const auto* m =
            static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t wired = m
            ? m->irsoa_wired_hits.load(std::memory_order_relaxed) : 0;
        const std::uint64_t cascade = m
            ? m->irsoa_dirty_cascade_savings.load(std::memory_order_relaxed) : 0;
        const std::uint64_t cache_red = m
            ? m->irsoa_cache_miss_reduction.load(std::memory_order_relaxed) : 0;
        const std::uint64_t skip = m
            ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t full = m
            ? m->relower_full_called_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t per_fn = m
            ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
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
    add("query:occurrence-typing-mutate-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        const auto* m =
            static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t deep = m
            ? m->deep_narrow_refreshes_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t blame = m
            ? m->occurrence_blame_chain_complete_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t stale = m
            ? m->narrow_stale_caught_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t complete = m
            ? m->provenance_completeness_hits_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t refreshes = m
            ? m->occurrence_stale_refreshes_total.load(std::memory_order_relaxed) : 0;
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

    // Issue #690: constraint typed-mutation reverify + blame stats.
    add("query:constraint-typed-mutate-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        const auto* m =
            static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t reverify_scans = m
            ? m->delta_conflict_reverify_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t conflicts = ev.get_cross_delta_conflicts_caught();
        const std::uint64_t blame_complete = m
            ? m->constraint_blame_chain_complete_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t truncated = m
            ? m->reverify_truncated_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t detected = m
            ? m->delta_conflict_detected_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t completeness_pct =
            detected > 0 ? (100 * blame_complete / detected)
                         : (blame_complete > 0 ? 100 : 0);
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"reverify-scans", make_int(static_cast<std::int64_t>(reverify_scans))},
            {"cross-delta-conflicts-caught", make_int(static_cast<std::int64_t>(conflicts))},
            {"blame-chain-completeness", make_int(static_cast<std::int64_t>(completeness_pct))},
            {"truncated-reverify", make_int(static_cast<std::int64_t>(truncated))},
        };
        return build_hash(kv);
    });

    // Issue #688: linear OwnershipEnv post-mutate typed-mutation stats.
    add("query:linear-ownership-typed-mutate-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        const auto* m =
            static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t revalidates = m
            ? m->linear_post_mutate_revalidations_total.load(std::memory_order_relaxed) +
              m->linear_dirty_revalidate_count.load(std::memory_order_relaxed)
            : 0;
        const std::uint64_t violations = m
            ? m->linear_violations_caught_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t enforcements = m
            ? m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t safe_fallbacks = m
            ? m->linear_typed_mutate_safe_fallbacks.load(std::memory_order_relaxed) +
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

    // Issue #686: ShapeProfiler ring history + Value v2 dispatch +
    // DirtyAware pass short-circuit synergy stats.
    add("query:shape-value-pass-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
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
        const std::uint64_t shape_sync = m
            ? m->shape_ids_sync_hits.load(std::memory_order_relaxed) : 0;
        (void)shape_sync;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"history-jitter-reduction", make_int(static_cast<std::int64_t>(jitter))},
            {"dispatch-stats", make_int(static_cast<std::int64_t>(dispatch))},
            {"dirty-shortcircuit-savings",
             make_int(static_cast<std::int64_t>(dirty_skip))},
            {"consteval-hits",
             make_int(static_cast<std::int64_t>(shape::k_shape_value_consteval_hits))},
        };
        return build_hash(kv);
    });

    // Issue #680: Define mutate IR/JIT/bridge invalidation observability.
    add("query:define-mutate-ir-invalidation-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        const auto* m =
            static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t stale_bridge = m
            ? m->closure_stale_returns.load(std::memory_order_relaxed)
            : 0;
        const std::uint64_t recompile_savings = m
            ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed)
            : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"precise-inval-hits",
             make_int(static_cast<std::int64_t>(ev.precise_define_inval_hits()))},
            {"stale-bridge-caught", make_int(static_cast<std::int64_t>(stale_bridge))},
            {"recompile-savings", make_int(static_cast<std::int64_t>(recompile_savings))},
        };
        return build_hash(kv);
    });

    add("query:span-lifetime-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
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

    add("query:mutation-audit-log", [&ev](std::span<const EvalValue> a) -> EvalValue {
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
            auto line = std::format("seq={} fiber={} op={} target={} nodes={} epoch_delta={} ts={}",
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
}

}  // namespace aura::compiler::primitives_detail