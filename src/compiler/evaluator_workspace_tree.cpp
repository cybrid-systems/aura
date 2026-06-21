// evaluator_workspace_tree.cpp — P1-h: workspace tree lifecycle + policy hash
// extracted from evaluator_impl.cpp.

module;

#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler {

using types::EvalValue;
using namespace types;

// Build a 6-key policy hash from a MemoryPolicy. Interned in string_heap_.
// Defined as a member function (not a local lambda) so it can be referenced
// from std::function-captured primitives without dangling.
EvalValue Evaluator::build_policy_hash(const MemoryPolicy& p) {
    std::vector<std::pair<std::string, EvalValue>> kv;
    kv.push_back({"auto-gc", make_bool(p.auto_gc)});
    kv.push_back({"warn-pct", make_int(p.warn_pct)});
    kv.push_back({"critical-pct", make_int(p.critical_pct)});
    kv.push_back({"sample-every", make_int(static_cast<std::int64_t>(p.sample_every))});
    kv.push_back({"cooldown-evals", make_int(static_cast<std::int64_t>(p.cooldown_evals))});
    kv.push_back(
        {"recent-gc-temp-window", make_int(static_cast<std::int64_t>(p.recent_gc_temp_window))});
    auto* ht = FlatHashTable::create(8);
    if (!ht)
        return make_void();
    auto meta = ht->metadata();
    auto keys = ht->keys();
    auto vals = ht->values();
    auto cap = ht->capacity;
    for (auto& [k, v] : kv) {
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (char c : k)
            h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
        auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
        if (fp == 0xFF)
            fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
        auto kidx = string_heap_.size();
        string_heap_.push_back(k);
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
            FlatHashTable::destroy(ht);
            return make_void();
        }
    }
    auto hidx = g_hash_tables.size();
    g_hash_tables.push_back(ht);
    return make_hash(hidx);
}

// eval_in(ast::Expr*) removed — all evaluation uses eval_flat(FlatAST&) now


void* Evaluator::create_workspace_tree() {
    auto* tree = new WorkspaceTree();
    WorkspaceNode root;
    root.name = "root";
    root.is_root = true;
    root.has_own_flat = true;
    root.flat = nullptr;
    root.pool = nullptr;
    tree->nodes_.push_back(std::move(root));
    return tree;
}

void Evaluator::destroy_workspace_tree(void* wt) {
    if (!wt)
        return;
    auto* tree = static_cast<WorkspaceTree*>(wt);
    // Delete owned flats (child workspaces that had COW triggered)
    for (auto& node : tree->nodes_) {
        if (!node.is_root && node.has_own_flat) {
            delete node.flat;
            delete node.pool;
        }
    }
    delete tree;
}

// Issue #141 AC: lazy COW trigger. Called by mutate:* primitives
// before they modify workspace_flat_. If the active workspace is a
// child that still shares parent's flat, clone it now (COW) so the
// mutation doesn't pollute the parent. No-op for root, already-
// cloned, or read-only workspaces (those return false).
bool Evaluator::trigger_lazy_cow(void* wt) {
    if (!wt)
        return true; // no tree yet, nothing to clone
    auto* tree = static_cast<WorkspaceTree*>(wt);
    auto idx = tree->active_idx();
    if (idx == 0 || idx >= tree->size())
        return true; // root, nothing to do
    auto& node = tree->nodes_[idx];
    if (node.has_own_flat)
        return true; // already cloned
    if (node.read_only)
        return false; // can't clone read-only
    return tree->ensure_local_flat(idx);
}

// After trigger_lazy_cow, the active workspace's flat/pool may have
// been reallocated. Call this to refresh the pointers without
// exposing the WorkspaceTree type to callers defined before the type.
bool Evaluator::refresh_active_flat_pool(void* wt, void** out_flat, void** out_pool) {
    if (!wt)
        return false;
    auto* tree = static_cast<WorkspaceTree*>(wt);
    auto* node = tree->active();
    if (!node)
        return false;
    if (out_flat)
        *out_flat = node->flat;
    if (out_pool)
        *out_pool = node->pool;
    return true;
}

} // namespace aura::compiler
