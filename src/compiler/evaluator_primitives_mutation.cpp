// evaluator_primitives_mutation.cpp — P0 step 31: mutation-count / rollback primitives
// Issue #278: mutation-log:summary observability primitive.
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "messaging_bridge.h"
#include "hash_meta.h" // FNV constants (#901)
#include "observability_metrics.h"
#include "core/gc_hooks.h"

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

void register_mutation_primitives(PrimRegistrar add, Evaluator& ev) {

    add("mutation-count", [&ev](const auto&) {
        if (!ev.workspace_flat_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->mutation_count()));
    });

    // Issue #1364: (query:safepoint-mutation-stats) — mutation × GC safepoint telemetry
    ObservabilityPrims::register_stats_impl(
        "query:safepoint-mutation-stats", [&ev](const auto&) -> EvalValue {
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto put = [&](const char* k, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (const char* p = k; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                EvalValue val_ev = make_int(v);
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = val_ev.val;
                        ht->size++;
                        return;
                    }
                }
            };
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            put("in-gc-safepoint", aura::gc_hooks::in_gc_safepoint() ? 1 : 0);
            put("mutation-in-safepoint-total",
                m ? static_cast<std::int64_t>(
                        m->mutation_in_safepoint_total.load(std::memory_order_relaxed))
                  : 0);
            // Prefer process-wide fiber counter; also fold CompilerMetrics mirror.
            const auto yield_hooks =
                static_cast<std::int64_t>(aura::gc_hooks::safepoint_yield_on_mutation_total());
            const auto yield_metrics =
                m ? static_cast<std::int64_t>(
                        m->safepoint_yield_on_mutation_total.load(std::memory_order_relaxed))
                  : 0;
            put("safepoint-yield-on-mutation-total",
                yield_hooks > yield_metrics ? yield_hooks : yield_metrics);
            put("safepoint-collision-total",
                m ? static_cast<std::int64_t>(
                        m->safepoint_collision_total.load(std::memory_order_relaxed))
                  : 0);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1362: compact committed mutation log prefix (keep recent tail).
    // (mutation-log-compact [keep-recent=1000] [keep-rolledback?=true]) → dropped count
    add("mutation-log-compact", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (!ev.workspace_flat_)
            return make_int(0);
        std::size_t keep_recent = 1000;
        bool keep_rolledback = true;
        if (!a.empty() && is_int(a[0])) {
            auto k = as_int(a[0]);
            keep_recent = k < 0 ? 0 : static_cast<std::size_t>(k);
        }
        if (a.size() >= 2 && is_bool(a[1]))
            keep_rolledback = as_bool(a[1]);
        const auto dropped = ev.workspace_flat_->compact_mutation_log(keep_recent, keep_rolledback);
        return make_int(static_cast<std::int64_t>(dropped));
    });

    // Issue #1362: compaction observability hash (size/compacted/ops).
    // Do NOT reuse query:mutation-log-stats — that name is the #553
    // integer sum of atomic-batch + steal/rollback counters (registered
    // in register_query_primitives). Overwriting it broke late4
    // test_issue_557_observability and every #553 int regression.
    ObservabilityPrims::register_stats_impl(
        "query:mutation-log-compact-stats", [&ev](const auto&) -> EvalValue {
            if (!ev.workspace_flat_)
                return make_void();
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto put = [&](const char* k, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (const char* p = k; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                EvalValue val_ev = make_int(v);
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = val_ev.val;
                        ht->size++;
                        return;
                    }
                }
            };
            auto* flat = ev.workspace_flat_;
            put("log-size", static_cast<std::int64_t>(flat->mutation_count()));
            put("compacted-records",
                static_cast<std::int64_t>(flat->mutation_log_compacted_records()));
            put("compact-ops", static_cast<std::int64_t>(flat->mutation_log_compact_ops()));
            put("auto-threshold",
                static_cast<std::int64_t>(aura::ast::FlatAST::kMutationLogAutoCompactThreshold));
            put("schema", 1362);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1054: bad-arg and empty-history both return void (list-or-void
    // contract). Never return make_int(0) on bad args (truthy, wrong type).
    add("mutation-history", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || !is_int(a[0]) || !ev.workspace_flat_)
            return make_void();
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto hist = ev.workspace_flat_->mutation_history(node);
        EvalValue result = make_void();
        for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
            auto& rec = *it;
            auto sid = static_cast<std::uint64_t>(ev.push_string_heap(std::format(
                "[{}] {}: {}{}", rec.mutation_id, rec.operator_name, rec.summary,
                rec.status == aura::ast::MutationStatus::RolledBack ? " [rolled-back]" : "")));
            auto pair_id = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sid), result});
            result = make_pair(pair_id);
        }
        return result;
    });

    add("rollback", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || !is_int(a[0]) || !ev.workspace_flat_)
            return make_bool(false);
        auto mid = static_cast<std::uint64_t>(as_int(a[0]));
        return make_bool(ev.workspace_flat_->rollback(mid));
    });

    add("rollback-since", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || !is_int(a[0]) || !ev.workspace_flat_)
            return make_int(0);
        auto mid = static_cast<std::uint64_t>(as_int(a[0]));
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->rollback_since(mid)));
    });

    // (mutation-log:summary) — Issue #278: aggregate stats over the
    // workspace mutation log. Returns a hash with:
    //   total                — total mutations recorded
    //   committed            — mutations with status=Committed
    //   rolled-back          — mutations with status=RolledBack
    //   by-operator          — hash {operator_name: count}
    //   last-mutation-id     — highest mutation_id seen
    //   last-operator        — operator_name of the latest mutation ("" if none)
    //   last-target-node     — target_node of the latest mutation (NULL_NODE if none)
    // Returns a void value if no workspace is loaded.
    //
    // This is the FOUNDATION primitive for the #278 Aura-layer
    // mutation observability (lib/std/mutate.aura wraps it as
    // (mutate:summary) + (mutate:by-operator)). Future work:
    // time-window filtering, by-status filtering, large-log
    // streaming for >10k records.
    add("mutation-log:summary", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_flat_)
            return make_void();
        const auto& log = ev.workspace_flat_->all_mutations();
        std::uint64_t total = log.size();
        std::uint64_t committed = 0;
        std::uint64_t rolled_back = 0;
        std::unordered_map<std::string, std::uint64_t> by_op;
        std::uint64_t last_id = 0;
        std::string last_op;
        aura::ast::NodeId last_target = aura::ast::NULL_NODE;
        for (const auto& rec : log) {
            if (rec.status == aura::ast::MutationStatus::Committed)
                ++committed;
            else
                ++rolled_back;
            ++by_op[rec.operator_name];
            if (rec.mutation_id >= last_id) {
                last_id = rec.mutation_id;
                last_op = rec.operator_name;
                last_target = rec.target_node;
            }
        }
        // Build the inner by-operator hash first.
        // Issue #258: 16-slot capacity (we typically have <10 operator
        // names in a single workspace; reserve some slack).
        auto* op_ht = FlatHashTable::create(16);
        if (!op_ht)
            return make_void();
        {
            auto meta = op_ht->metadata();
            auto keys = op_ht->keys();
            auto vals = op_ht->values();
            auto cap = op_ht->capacity;
            for (auto& [k, v] : by_op) {
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
                for (std::size_t at = 0; at < cap; ++at) {
                    auto idx = ((h >> 1) + at) & (cap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = make_int(static_cast<std::int64_t>(v)).val;
                        op_ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(op_ht);
                    return make_void();
                }
            }
        }
        auto op_hidx = g_hash_tables.size();
        g_hash_tables.push_back(op_ht);
        EvalValue op_hash_ev = make_hash(op_hidx);

        // Build the outer summary hash.
        // 7 fields → 8-slot capacity is enough.
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        // Pre-compute the EvalValue for last-operator (string).
        // Push key string first, then value string, so we can
        // build correct key/val EvalValues.
        std::size_t last_op_key_idx = ev.string_heap_.size();
        ev.string_heap_.push_back("last-operator");
        std::size_t last_op_val_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(last_op);
        EvalValue last_op_key_ev = make_string(last_op_key_idx);
        EvalValue last_op_val_ev = make_string(last_op_val_idx);

        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"total", make_int(static_cast<std::int64_t>(total))},
            {"committed", make_int(static_cast<std::int64_t>(committed))},
            {"rolled-back", make_int(static_cast<std::int64_t>(rolled_back))},
            {"by-operator", op_hash_ev},
            {"last-mutation-id", make_int(static_cast<std::int64_t>(last_id))},
            {"last-target-node",
             make_int(static_cast<std::int64_t>(static_cast<std::int64_t>(last_target)))},
            // last-operator is special: a string. We re-route via
            // make_string for the value to ensure the correct string
            // heap index is used.
            {"", last_op_val_ev},
        };
        // The last-operator entry uses last_op_key_ev as the key and
        // last_op_val_ev as the value. Replace the placeholder above
        // by inserting it directly.
        {
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto cap = ht->capacity;
            // First insert the 6 integer-typed entries from kv[0..5].
            for (std::size_t i = 0; i < 6; ++i) {
                auto& entry = kv[i];
                const std::string& k = entry.first;
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
                for (std::size_t at = 0; at < cap; ++at) {
                    auto idx = ((h >> 1) + at) & (cap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = entry.second.val;
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
            // Then insert the last-operator entry (string-typed value).
            // The key string "last-operator" is already in the heap at
            // last_op_key_idx; we reuse it via last_op_key_ev.
            // Compute fingerprint for "last-operator".
            std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
            for (char c : std::string("last-operator"))
                h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            bool inserted = false;
            for (std::size_t at = 0; at < cap; ++at) {
                auto idx = ((h >> 1) + at) & (cap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    keys[idx] = last_op_key_ev.val;
                    vals[idx] = last_op_val_ev.val;
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

    // Issue #278 follow-up #1: (mutation-log:diff from-id to-id) —
    // return a list of MutationRecords with mutation_id in the
    // half-open range [from_id, to_id]. Returns () if from_id > to_id
    // or no records match. from_id = 0 means "from the beginning";
    // to_id = -1 (or any value > max_id) means "to the end".
    //
    // Use case: AI agent debugging "what happened between
    // mutations N and M?" — gets the diff in one call instead
    // of iterating the log manually.
    add("mutation-log:diff", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (!ev.workspace_flat_)
            return make_void();
        if (a.size() != 2 || !is_int(a[0]) || !is_int(a[1]))
            return make_void();
        auto from_id = static_cast<std::uint64_t>(as_int(a[0]));
        auto to_id_raw = static_cast<std::int64_t>(as_int(a[1]));
        // -1 (or any negative) means "to the end".
        const auto& log = ev.workspace_flat_->all_mutations();
        std::uint64_t max_id = log.empty() ? 0 : log.back().mutation_id;
        std::uint64_t to_id = (to_id_raw < 0) ? max_id : static_cast<std::uint64_t>(to_id_raw);
        if (from_id > to_id)
            return make_void();
        EvalValue result = make_void();
        std::vector<EvalValue> entries;
        for (const auto& rec : log) {
            if (rec.mutation_id < from_id)
                continue;
            if (rec.mutation_id > to_id)
                break;
            // Each entry is a hash with the key fields
            // (mutation_id, operator, target_node, status, summary).
            // Push the key + value strings first.
            std::string op_str = rec.operator_name;
            std::string sum_str = rec.summary;
            std::size_t op_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(op_str);
            std::size_t sum_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(sum_str);
            std::size_t k_id = ev.string_heap_.size();
            ev.string_heap_.push_back("mutation-id");
            std::size_t k_op = ev.string_heap_.size();
            ev.string_heap_.push_back("operator");
            std::size_t k_tgt = ev.string_heap_.size();
            ev.string_heap_.push_back("target-node");
            std::size_t k_st = ev.string_heap_.size();
            ev.string_heap_.push_back("status");
            std::size_t k_sum = ev.string_heap_.size();
            ev.string_heap_.push_back("summary");
            // 8-slot capacity is enough for 5 fields.
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                continue;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"mutation-id", make_int(static_cast<std::int64_t>(rec.mutation_id))},
                {"operator", make_string(op_idx)},
                {"target-node", make_int(static_cast<std::int64_t>(rec.target_node))},
                {"status", make_int(static_cast<std::int64_t>(
                               rec.status == aura::ast::MutationStatus::Committed ? 1 : 0))},
                {"summary", make_string(sum_idx)},
            };
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto cap = ht->capacity;
            bool ok = true;
            for (auto& [k, v] : kv) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                EvalValue key_ev = make_string(k == "mutation-id"   ? k_id
                                               : k == "operator"    ? k_op
                                               : k == "target-node" ? k_tgt
                                               : k == "status"      ? k_st
                                                                    : k_sum);
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
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                FlatHashTable::destroy(ht);
                continue;
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            entries.push_back(make_hash(hidx));
        }
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            auto cons_pair = ev.pairs_.size();
            ev.pairs_.push_back({*it, result});
            result = make_pair(cons_pair);
        }
        return result;
    });

    // Issue #622: high-level Aura primitives for atomic / batched
    // mutates. **IMPORTANT — discovery before this PR:
    // (mutate:atomic-batch ops-list "summary") already exists
    // (#192 / #213, evaluator_primitives_mutate.cpp:2645) and
    // provides a list-call form. Likewise, (atomic-batch:stats)
    // (#192, evaluator_primitives_observability.cpp:1586) and
    // (query:atomic-batch-stats) (#437, evaluator_primitives_
    // compile.cpp:2098) and (query:atomic-batch-rollback-stats)
    // (#529, evaluator_primitives_query.cpp:3772) already cover
    // the observability surface. So instead of duplicating
    // primitives with split-name forms (mutate:atomic-begin/
    // commit/rollback) and a duplicate query:atomic-batch-stats
    // hash, **#622 ships one new Aura primitive** — a
    // structured-hash companion to the existing flat-int
    // (query:atomic-batch-stats) from #437 — that surfaces
    // per-batch runtime state (active flag, current batch's
    // suppressed-bumps count) that's NOT in either
    // (atomic-batch:stats) or (query:atomic-batch-stats).
    //
    // The remaining #622 AC2 (Guard nesting-depth + per-batch
    // impact_nodes + rollback_success_rate) + AC4 (in-batch
    // StableRef refresh) are invasive Guard-internal / observability
    // changes that need benchmarking + perf regression coverage
    // alongside the MutateBoundary stack work in #619 — separate
    // follow-ups.

    // (query:atomic-batch-stats-hash) — Agent-discoverable
    // structured companion to (query:atomic-batch-stats) (which
    // returns an int). 4-field hash:
    //   - active                    bool: is a batch in progress?
    //                                 FlatAST::atomic_batch_active()
    //   - commits-total             lifetime # of successful
    //                                 commits (atomic_batch_commits_)
    //   - bumps-saved-last-batch    # of suppressed gen bumps
    //                                 in the most recent batch
    //                                 (atomic_batch_bumps_saved_,
    //                                 reset on each begin)
    //   - schema == 622             sentinel for Agent drift
    //                                 detection (mirrors #618's
    //                                 sentinel + #620's + #621's)
    //
    // Note: (atomic-batch:stats) from #192 already exposes
    // batch-count / ops-total / rollback-count / ops-per-batch
    // / bumps-saved-total. The fields above complement that
    // observability surface with the per-batch runtime state
    // (which batch is currently open, how much bumps the current
    // batch would emit if committed).
    ObservabilityPrims::register_stats_impl(
        "query:atomic-batch-stats-hash", [&ev](const auto&) -> EvalValue {
            std::uint64_t commits = 0;
            std::uint64_t bumps_saved = 0;
            bool active = false;
            if (ev.workspace_flat_) {
                commits = ev.workspace_flat_->atomic_batch_commits();
                bumps_saved = ev.workspace_flat_->atomic_batch_bumps_saved();
                active = ev.workspace_flat_->atomic_batch_active();
            }
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
            insert_kv("active", active ? 1 : 0);
            insert_kv("commits-total", static_cast<std::int64_t>(commits));
            insert_kv("bumps-saved-last-batch", static_cast<std::int64_t>(bumps_saved));
            insert_kv("schema", 622);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #632: query:atomic-batch-sv-stats-hash — Agent-
    // discoverable structured SV-specific observability dashboard
    // for the atomic batch layer. Companion / extension of the
    // existing query:atomic-batch-stats-hash (#622, 4-field
    // generic) + query:atomic-batch-rollback-stats (#529) +
    // atomic-batch:stats (#192, 5-field). Specifically covers
    // AC4 from the issue body.
    //
    // Fields (5):
    //   - active-sv-batches          synthetic: same as
    //                                (query:atomic-batch-stats-hash
    //                                active) today — 0/1. Marked
    //                                -sv- to signal the Agent that
    //                                once AC2+AC3 wire-up lands
    //                                this field will track SV-
    //                                tagged batches independently
    //                                of generic batches.
    //   - suppressed-bumps-on-sv    synthetic: bumps-saved-last-
    //                                batch from existing #622
    //                                counter (the per-batch bump
    //                                count). Marked -sv- similarly
    //                                with future split pending.
    //   - rollback-success-sv       new atomic_batch_sv_rollback_
    //                                total counter (foundation for
    //                                AC2 — Guard aggregates a
    //                                single rollback entry per SV
    //                                batch abort).
    //   - batch-impact-sv-nodes      new atomic_batch_sv_impact_
    //                                nodes_total counter (foundation
    //                                for AC3 — per-batch SV
    //                                node-count aggregator).
    //   - schema == 632              sentinel for Agent drift
    //                                detection (mirrors #618+
    //                                #620+#621+#622+#623+#624+
    //                                #625+#626+#630+#631 sentinels).
    //
    // Discovery before this PR (no duplication): the atomic batch
    // infrastructure exists in flat via
    // begin_atomic_batch / commit_atomic_batch /
    // rollback_atomic_batch + atomic_batch_commits +
    // atomic_batch_active + atomic_batch_bumps_saved (added by
    // #250/#255/#622); observability primitives `atomic-batch:stats`
    // (#192, 5-field) + `query:atomic-batch-stats` (#437, int) +
    // `query:atomic-batch-stats-hash` (#622, 4-field structured) +
    // `query:atomic-batch-rollback-stats` (#529, int-sum). What
    // AC4 specifies by **exact name + fields** —
    // `query:atomic-batch-sv-stats` with
    // {active_sv_batches, suppressed_bumps_on_sv,
    // rollback_success_sv, batch_impact_sv_nodes} — was not
    // shipped. So #632 ships ONE new Aura primitive + 2 new
    // atomics that are foundation scaffolding for the future AC2
    // + AC3 enforcement work.
    //
    // The remaining #632 AC1 + AC2 + AC3 work (high-level
    // atomic-batch SV mutate primitive, Guard nesting-depth +
    // suppressed-bump-count tracking inside, StableNodeRef
    // auto-refresh on capture with suppressed-gen awareness for
    // SV nodes) is invasive C++ + hot-path EDA + Guard-internal
    // work that needs benchmarking + perf regression coverage —
    // separate follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:atomic-batch-sv-stats-hash", [&ev](const auto&) -> EvalValue {
            std::uint64_t commits = 0;
            std::uint64_t bumps_saved = 0;
            bool active = false;
            if (ev.workspace_flat_) {
                commits = ev.workspace_flat_->atomic_batch_commits();
                bumps_saved = ev.workspace_flat_->atomic_batch_bumps_saved();
                active = ev.workspace_flat_->atomic_batch_active();
            }
            const std::uint64_t rollback_sv =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->atomic_batch_sv_rollback_total.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t impact_sv =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->atomic_batch_sv_impact_nodes_total.load(std::memory_order_relaxed)
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
            insert_kv("active-sv-batches", active ? 1 : 0);
            insert_kv("suppressed-bumps-on-sv", static_cast<std::int64_t>(bumps_saved));
            insert_kv("rollback-success-sv", static_cast<std::int64_t>(rollback_sv));
            insert_kv("batch-impact-sv-nodes", static_cast<std::int64_t>(impact_sv));
            insert_kv("schema", 632);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #737: query:atomic-batch-snapshot-stats-hash — end-to-end
    // atomic batch + snapshot + StableNodeRef pinning observability
    // for multi-round AI Agent edit loops. Complements #622 (per-
    // batch runtime state) and #529 (rollback stats).
    //
    // Fields (7):
    //   - batch-commits              Evaluator atomic_batch_domain_.count
    //   - rollback-triggers        Evaluator atomic_batch_domain_.rollbacks
    //   - pinned-refs-last-batch     current pinned ref set size
    //   - pinned-refs-total          lifetime pinned ref captures
    //   - snapshot-captures          batches that took :snapshot?
    //   - snapshot-rollbacks         ast:restore on batch failure
    //   - suppressed-bumps-total     atomic_batch_domain_.bumps_saved_total
    //   - schema == 737
    ObservabilityPrims::register_stats_impl(
        "query:atomic-batch-snapshot-stats-hash", [&ev](const auto&) -> EvalValue {
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
        }
        };
        insert_kv("batch-commits", static_cast<std::int64_t>(ev.atomic_batch_count()));
        insert_kv("rollback-triggers", static_cast<std::int64_t>(ev.atomic_batch_rollbacks()));
        insert_kv("pinned-refs-last-batch",
                  static_cast<std::int64_t>(ev.atomic_batch_pinned_ref_count()));
        insert_kv("pinned-refs-total",
                  static_cast<std::int64_t>(ev.atomic_batch_pinned_refs_total()));
        insert_kv("snapshot-captures",
                  static_cast<std::int64_t>(ev.atomic_batch_snapshot_captures()));
        insert_kv("snapshot-rollbacks",
                  static_cast<std::int64_t>(ev.atomic_batch_snapshot_rollbacks()));
        insert_kv("suppressed-bumps-total",
                  static_cast<std::int64_t>(ev.atomic_batch_bumps_saved_total()));
        insert_kv("schema", 737);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
});

// typed-mutate-atomic is registered in evaluator_primitives_mutate.cpp
// (Issue #1415/#1442) — list + varargs forms. Do not re-register here.
}

} // namespace aura::compiler::primitives_detail
