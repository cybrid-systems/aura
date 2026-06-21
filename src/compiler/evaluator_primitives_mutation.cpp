// evaluator_primitives_mutation.cpp — P0 step 31: mutation-count / rollback primitives
// Issue #278: mutation-log:summary observability primitive.
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include <cstdint>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include "runtime_shared.h"
#include "messaging_bridge.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimFn = std::function<EvalValue(std::span<const EvalValue>)>;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using namespace types;

void register_mutation_primitives(PrimRegistrar add, Evaluator& ev) {

    add("mutation-count", [&ev](const auto&) {
        if (!ev.workspace_flat_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->mutation_count()));
    });

    add("mutation-history", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || !is_int(a[0]) || !ev.workspace_flat_)
            return make_int(0);
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto hist = ev.workspace_flat_->mutation_history(node);
        EvalValue result = make_void();
        for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
            auto& rec = *it;
            auto sid = ev.string_heap_.size();
            ev.string_heap_.push_back(std::format(
                "[{}] {}: {}{}", rec.mutation_id, rec.operator_name, rec.summary,
                rec.status == aura::ast::MutationStatus::RolledBack ? " [rolled-back]" : ""));
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
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (char c : std::string("last-operator"))
                h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
}

} // namespace aura::compiler::primitives_detail
