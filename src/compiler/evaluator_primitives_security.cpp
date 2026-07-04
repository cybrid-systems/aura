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

    add("security:sandbox-mode?",
        [&ev](const auto&) -> EvalValue { return make_bool(ev.sandbox_mode()); });

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
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"sandbox-mode", make_bool(ev.sandbox_mode())},
            {"capability-denials",
             make_int(static_cast<std::int64_t>(ev.capability_denial_count()))},
            {"mutation-audit-total",
             make_int(static_cast<std::int64_t>(ev.mutation_audit_total()))},
            {"granted-capabilities",
             make_int(static_cast<std::int64_t>(ev.granted_capability_count()))},
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
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"nested-depth-max", make_int(static_cast<std::int64_t>(ev.nested_guard_depth_max()))},
            {"suppressed-misalign-caught",
             make_int(static_cast<std::int64_t>(ev.suppressed_misalign_caught()))},
            {"macro-rollback-hits", make_int(static_cast<std::int64_t>(ev.macro_rollback_hits()))},
        };
        return build_hash(kv);
    });

    // Issue #681: compiler IRClosure/bridge epoch enforcement stats.
    add("query:compiler-closure-inval-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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
    add("query:compiler-gc-root-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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
    add("query:linear-ownership-gc-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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
    add("query:irsoa-incremental-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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
    add("query:occurrence-typing-mutate-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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
    add("query:primitives-extension-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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
    add("query:primitives-registry-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t capture_viol =
            m ? m->primitive_capture_violations_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t fastpath_hits =
            m ? m->primitive_fastpath_hits_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t eda_registered = ev.primitives_.category_meta_count("eda") +
                                           ev.primitives_.category_meta_count("sva") +
                                           ev.primitives_.category_meta_count("verification");
        const std::uint64_t slots = ev.primitives_.slot_count();
        const std::uint64_t documented = ev.primitives_.documented_meta_count();
        const std::uint64_t consistency_rate =
            slots > 0 ? (documented * 100) / slots : 100;
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

    // Issue #695: EDA-SV verification closed-loop stress stats.
    add("query:eda-sv-closedloop-stress-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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
            {"verification-convergence-rate", make_int(static_cast<std::int64_t>(convergence_pct))},
            {"feedback-mutate-success", make_int(static_cast<std::int64_t>(feedback_ok))},
            {"stable-ref-invalidation-sv", make_int(static_cast<std::int64_t>(ref_inval))},
            {"dirty-traversal-cost", make_int(static_cast<std::int64_t>(dirty_cost))},
            {"commercial-stub-latency", make_int(static_cast<std::int64_t>(stub_latency))},
            {"corruption-detected", make_int(static_cast<std::int64_t>(corruption))},
        };
        return build_hash(kv);
    });

    // Issue #694: SVA structured AST stats.
    add("query:sv-sva-structure-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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

    // Issue #698: Hardware backend commercial interop stats.
    add("query:hardware-backend-commercial-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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
            {"verification-loop-convergence", make_int(static_cast<std::int64_t>(convergence_pct))},
            {"commercial-simulator-runs", make_int(static_cast<std::int64_t>(sim_runs))},
            {"sv-diff-emits", make_int(static_cast<std::int64_t>(diff_emits))},
        };
        return build_hash(kv);
    });

    // Issue #706: Scheduler StealBudget adaptive bias + LLM tail stats.
    add("query:scheduler-stealbudget-adaptive-stats", [&ev](const auto&) -> EvalValue {
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
            {"deferred-pressure-boosts",
             make_int(static_cast<std::int64_t>(aura_adaptive_steal_deferred_pressure_boosts()))},
            {"global-deferred-mutation-total",
             make_int(static_cast<std::int64_t>(aura_adaptive_steal_global_deferred_total()))},
        };
        return build_hash(kv);
    });

    // Issue #707: Per-fiber MutationStack / YieldCheckpoint pool stats.
    add("query:per-fiber-stack-pool-stats", [&ev](const auto&) -> EvalValue {
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
            {"pool-hits", make_int(static_cast<std::int64_t>(aura_per_fiber_stack_pool_hits()))},
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
            {"restamps", make_int(static_cast<std::int64_t>(aura_per_fiber_stack_pool_restamps()))},
        };
        return build_hash(kv);
    });

    // Issue #708: AOT hot-reload refcount swap + region isolation stats.
    add("query:aot-reload-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t attempts =
            m ? m->aot_reload_attempts_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t success =
            m ? m->aot_hot_update_success_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t stale =
            m ? m->aot_stale_reject_count_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t swaps = m ? m->aot_refcount_swaps_.load(std::memory_order_relaxed) : 0;
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

    // Issue #708: AOT checkpoint / bridge_epoch version drift stats.
    add("query:aot-checkpoint-version-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t drifts =
            m ? m->aot_checkpoint_version_drifts_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t deopt = m ? m->aot_deopt_on_steal_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t swaps = m ? m->aot_refcount_swaps_.load(std::memory_order_relaxed) : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"checkpoint-version-drifts", make_int(static_cast<std::int64_t>(drifts))},
            {"deopt-on-steal", make_int(static_cast<std::int64_t>(deopt))},
            {"func-table-epoch-swaps", make_int(static_cast<std::int64_t>(swaps))},
        };
        return build_hash(kv);
    });

    // Issue #693: Hardware backend SV commercial closed-loop stats.
    add("query:hardware-backend-sv-closedloop-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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
    add("query:adt-exhaustiveness-typed-mutate-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t rechecks =
            m ? m->adt_exhaust_rechecks_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t non_exhaust =
            m ? m->adt_non_exhaustive_caught_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t pattern_refreshes =
            m ? m->adt_pattern_narrow_refreshes_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t complete =
            m ? m->adt_pattern_provenance_complete_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t completeness_pct =
            pattern_refreshes > 0 ? (100 * complete / pattern_refreshes) : (complete > 0 ? 100 : 0);
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"post-mutate-rechecks", make_int(static_cast<std::int64_t>(rechecks))},
            {"non-exhaustive-caught", make_int(static_cast<std::int64_t>(non_exhaust))},
            {"pattern-narrow-refreshes", make_int(static_cast<std::int64_t>(pattern_refreshes))},
            {"provenance-completeness", make_int(static_cast<std::int64_t>(completeness_pct))},
        };
        return build_hash(kv);
    });

    // Issue #691: CoercionMap + NarrowingRecord provenance stats.
    add("query:coercion-narrowing-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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
            {"post-narrow-elim-opportunities", make_int(static_cast<std::int64_t>(opportunities))},
            {"blame-chain-hits", make_int(static_cast<std::int64_t>(blame))},
            {"cast-elim-from-narrow", make_int(static_cast<std::int64_t>(elim + narrow_hits))},
            {"blame-chain-completeness", make_int(static_cast<std::int64_t>(completeness_pct))},
        };
        return build_hash(kv);
    });

    // Issue #690: constraint typed-mutation reverify + blame stats.
    add("query:constraint-typed-mutate-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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
    add("query:linear-ownership-typed-mutate-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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

    // Issue #686: ShapeProfiler ring history + Value v2 dispatch +
    // DirtyAware pass short-circuit synergy stats.
    add("query:shape-value-pass-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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
    add("query:define-mutate-ir-invalidation-stats", [&ev](const auto&) -> EvalValue {
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
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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

    add("query:span-lifetime-stats", [&ev](const auto&) -> EvalValue {
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
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_void();
        const std::uint64_t unsafe_access = ws->children_call_count() + ws->parent_of_call_count();
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

} // namespace aura::compiler::primitives_detail