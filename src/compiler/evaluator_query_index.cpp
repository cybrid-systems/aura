// evaluator_query_index.cpp — P1-o: tag/arity index for query:pattern
// aura.compiler.evaluator module partition.

module;

#include <algorithm>
#include <cstdint>

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler {

using types::EvalValue;
using namespace types;

namespace {

constexpr std::uint64_t kTagArityKeyNone = ~std::uint64_t{0};

[[nodiscard]] std::uint64_t tag_arity_key(aura::ast::NodeTag tag, std::size_t arity) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(tag)) << 32) |
           static_cast<std::uint64_t>(arity);
}

}  // namespace

void Evaluator::tag_arity_index_insert_node(const aura::ast::FlatAST& flat,
                                            aura::ast::NodeId id) const {
    if (id >= flat.size())
        return;
    // Issue #484: skip orphan nodes. After mutate:replace-pattern,
    // the OLD matched child gets parent_ cleared by set_child —
    // it's no longer reachable from the workspace root. Such
    // orphan nodes still exist in the flat (so id < size() is
    // true) but should not be returned by query:pattern.
    // Issue #484 follow-up: see slow-path counterpart in
    // evaluator_primitives_query_workspace.cpp. Skip orphans
    // except those with MacroIntroduced marker (macro-expanded
    // bodies that macro_expand_all forgot to splice in). Also
    // skip the check entirely when the flat has no root set
    // (test fixture scenario).
    if (flat.root != aura::ast::NULL_NODE &&
        id != flat.root && flat.parent_of(id) == aura::ast::NULL_NODE &&
        flat.marker(id) != aura::ast::SyntaxMarker::MacroIntroduced)
        return;
    const auto node = flat.get(id);
    const auto key = tag_arity_key(node.tag, node.children.size());
    tag_arity_index_[key].push_back(id);
    if (id >= tag_arity_indexed_key_.size())
        tag_arity_indexed_key_.resize(static_cast<std::size_t>(id) + 1, kTagArityKeyNone);
    tag_arity_indexed_key_[id] = key;
}

void Evaluator::tag_arity_index_remove_node(aura::ast::NodeId id) const {
    if (id >= tag_arity_indexed_key_.size())
        return;
    const auto key = tag_arity_indexed_key_[id];
    if (key == kTagArityKeyNone)
        return;
    auto it = tag_arity_index_.find(key);
    if (it != tag_arity_index_.end()) {
        auto& bucket = it->second;
        bucket.erase(std::remove(bucket.begin(), bucket.end(), id), bucket.end());
        if (bucket.empty())
            tag_arity_index_.erase(it);
    }
    tag_arity_indexed_key_[id] = kTagArityKeyNone;
}

void Evaluator::tag_arity_index_rebuild_full(const aura::ast::FlatAST& flat) const {
    tag_arity_index_.clear();
    tag_arity_indexed_key_.clear();
    tag_arity_index_workspace_ = workspace_flat_;
    const std::size_t n = flat.size();
    tag_arity_index_.reserve(n);
    tag_arity_indexed_key_.resize(n, kTagArityKeyNone);
    for (aura::ast::NodeId id = 0; id < n; ++id)
        tag_arity_index_insert_node(flat, id);
    tag_arity_index_synced_size_ = n;
    tag_arity_index_synced_gen_ = flat.generation();
}

void Evaluator::tag_arity_index_append_nodes(const aura::ast::FlatAST& flat,
                                             std::size_t from_id) const {
    const std::size_t n = flat.size();
    if (from_id >= n)
        return;
    if (tag_arity_indexed_key_.size() < n)
        tag_arity_indexed_key_.resize(n, kTagArityKeyNone);
    for (aura::ast::NodeId id = static_cast<aura::ast::NodeId>(from_id); id < n; ++id)
        tag_arity_index_insert_node(flat, id);
    tag_arity_index_synced_size_ = n;
}

void Evaluator::tag_arity_index_prune_stale_entries(const aura::ast::FlatAST& flat) const {
    const auto root = flat.root;
    for (auto it = tag_arity_index_.begin(); it != tag_arity_index_.end();) {
        auto& bucket = it->second;
        bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                  [&](aura::ast::NodeId id) {
                                      if (id >= flat.size()) {
                                          if (id < tag_arity_indexed_key_.size())
                                              tag_arity_indexed_key_[id] = kTagArityKeyNone;
                                          return true;
                                      }
                                      if (id != root && flat.parent_of(id) == aura::ast::NULL_NODE) {
                                          if (id < tag_arity_indexed_key_.size())
                                              tag_arity_indexed_key_[id] = kTagArityKeyNone;
                                          return true;
                                      }
                                      return false;
                                  }),
                     bucket.end());
        if (bucket.empty())
            it = tag_arity_index_.erase(it);
        else
            ++it;
    }
}

void Evaluator::tag_arity_index_sync_after_mutation(const aura::ast::FlatAST& flat) const {
    if (flat.size() > tag_arity_index_synced_size_)
        tag_arity_index_append_nodes(flat, tag_arity_index_synced_size_);
    tag_arity_index_prune_stale_entries(flat);
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_dirty(id))
            continue;
        tag_arity_index_remove_node(id);
        if (id != flat.root && flat.parent_of(id) == aura::ast::NULL_NODE)
            continue;
        tag_arity_index_insert_node(flat, id);
    }
    tag_arity_index_synced_size_ = flat.size();
    tag_arity_index_synced_gen_ = flat.generation();
}

void Evaluator::build_tag_arity_index() const {
    if (!workspace_flat_) {
        invalidate_tag_arity_index();
        return;
    }
    const auto& flat = *workspace_flat_;

    if (tag_arity_index_workspace_ != workspace_flat_ || tag_arity_index_.empty()) {
        tag_arity_index_rebuild_full(flat);
        return;
    }

    const std::size_t cur_size = flat.size();
    const auto cur_gen = flat.generation();
    if (cur_size == tag_arity_index_synced_size_ && cur_gen == tag_arity_index_synced_gen_)
        return;

    if (cur_gen == tag_arity_index_synced_gen_ && cur_size > tag_arity_index_synced_size_) {
        tag_arity_index_append_nodes(flat, tag_arity_index_synced_size_);
        return;
    }

    tag_arity_index_sync_after_mutation(flat);
}

}  // namespace aura::compiler