// evaluator_workspace_tree.cpp — P1-h: workspace tree lifecycle + policy hash
// aura.compiler.evaluator module partition.

module;

#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
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

EvalValue Evaluator::build_ast_lifecycle_hash(
    std::span<const std::pair<std::string, EvalValue>> kv) {
    auto* ht = FlatHashTable::create(16);
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

// ── Panic auto-rollback (Issue #39) ────────────────────────────
bool Evaluator::save_panic_checkpoint() {
    if (!workspace_flat_ || !workspace_pool_)
        return false;
    auto src_fn = primitives_.lookup("current-source");
    if (!src_fn)
        return false;
    auto src = (*src_fn)({});
    if (!types::is_string(src))
        return false;
    auto idx = types::as_string_idx(src);
    if (idx >= string_heap_.size())
        return false;
    panic_safe_source_ = string_heap_[idx];
    // Issue #242: snapshot the 4 pmr/append-only arena sizes
    // so restore_panic_checkpoint can truncate them back.
    // env_frames_ size is recorded for diagnostics only (the
    // deque itself is NOT truncated on restore — see
    // restore_panic_checkpoint).
    panic_safe_cells_size_ = cells_.size();
    panic_safe_pairs_size_ = pairs_.size();
    panic_safe_string_heap_size_ = string_heap_.size();
    panic_safe_env_frames_size_ = env_frames_.size();
    // Issue #548: bump panic_checkpoint_save_count_ so
    // (query:panic-checkpoint-lifecycle-stats) can report
    // the lifetime save count.
    bump_panic_checkpoint_save_count();
    return true;
}

bool Evaluator::restore_panic_checkpoint() {
    if (panic_safe_source_.empty())
        return false;
    auto set_fn = primitives_.lookup("set-code");
    if (!set_fn)
        return false;
    auto idx = string_heap_.size();
    string_heap_.push_back(panic_safe_source_);
    auto result = (*set_fn)({make_string(idx)});
    bool ok = types::is_bool(result) && types::as_bool(result);
    // Issue #548: bump the lifecycle counters regardless of
    // success — restore attempts (failed or succeeded) count
    // toward the lifetime restore counter. Successful restores
    // additionally bump rollback_success_on_panic_.
    bump_panic_checkpoint_restore_count();
    if (ok) {
        bump_rollback_success_on_panic();
    }
    if (ok) {
        // Issue #242: truncate the 3 append-only arenas back to
        // their checkpoint sizes. We do NOT truncate env_frames_
        // (the Closure::env_id indices must remain valid for
        // already-constructed closures; the version stamping
        // from Phase 1 detects stale frames instead).
        //
        // The source string we just pushed back is at idx; we
        // resize string_heap_ to idx+1 (= pre-save size + 1) so
        // the source string is preserved while everything added
        // AFTER the save (idx+1 onwards) is truncated away.
        std::size_t new_string_heap_size = idx + 1;
        if (new_string_heap_size <= string_heap_.size()) {
            string_heap_.resize(new_string_heap_size);
        }
        if (panic_safe_cells_size_ > 0 && panic_safe_cells_size_ <= cells_.size()) {
            cells_.resize(panic_safe_cells_size_);
        }
        if (panic_safe_pairs_size_ > 0 && panic_safe_pairs_size_ <= pairs_.size()) {
            pairs_.resize(panic_safe_pairs_size_);
        }
        // Issue #356: mark env_frames_ entries allocated during
        // the doomed transaction as INVALID_VERSION. The frames
        // stay allocated (truncating env_frames_ would invalidate
        // Closure::env_id indices for closures that captured
        // pre-rollback frames), but materialize_call_env and the
        // parent walks will refuse to use them — preserving the
        // invariant "any frame reachable from a live Closure is
        // usable". This is the scope-limited compromise for the
        // full stable-id indirection refactor (which would let
        // env_frames_ actually shrink); see the issue body for
        // the design sketch.
        invalidate_post_rollback_env_frames();
        // Clear checkpoint after successful restore
        panic_safe_source_.clear();
        panic_safe_cells_size_ = 0;
        panic_safe_pairs_size_ = 0;
        panic_safe_string_heap_size_ = 0;
        panic_safe_env_frames_size_ = 0;
    }
    return ok;
}

void Evaluator::update_shared_tree_root() {
    if (!workspace_tree_)
        return;
    auto* wt = static_cast<WorkspaceTree*>(workspace_tree_);
    if (wt->size() > 0) {
        auto active = wt->active_idx();
        if (active < wt->size()) {
            wt->nodes_[active].flat = workspace_flat_;
            wt->nodes_[active].pool = workspace_pool_;
            if (active > 0)
                wt->nodes_[active].has_own_flat = true;
        }
    }
}

Env* Evaluator::copy_env(const Env& e, ast::ASTArena* target) {
    contract_assert(arena_ != nullptr);
    auto* ar = target ? target : arena_;
    return ar ? ar->create<Env>(e) : nullptr;
}

} // namespace aura::compiler
