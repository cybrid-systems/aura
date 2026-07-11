// evaluator_query_index.cpp — P1-o: tag/arity index for query:pattern
// aura.compiler.evaluator module partition.

module;


module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler {

using types::EvalValue;
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

namespace {

    constexpr std::uint64_t kTagArityKeyNone = ~std::uint64_t{0};

    [[nodiscard]] std::uint64_t tag_arity_key(aura::ast::NodeTag tag, std::size_t arity) noexcept {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(tag)) << 32) |
               static_cast<std::uint64_t>(arity);
    }

} // namespace

void Evaluator::tag_arity_index_insert_node(const aura::ast::FlatAST& flat,
                                            aura::ast::NodeId id) const {
    // Issue #371: nested helper under the writer held by
    // build_tag_arity_index / rebuild_full / append_nodes /
    // sync_after_mutation. Do NOT take a unique_lock here
    // (would deadlock). Callers acquire the lock once at
    // the entry point and then call these helpers under
    // that umbrella.
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
    if (flat.root != aura::ast::NULL_NODE && id != flat.root &&
        flat.parent_of(id) == aura::ast::NULL_NODE && !flat.is_macro_introduced(id))
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
                                        if (id != root &&
                                            flat.parent_of(id) == aura::ast::NULL_NODE) {
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

void Evaluator::bump_pattern_index_rebuild_trigger(std::uint8_t trigger) const noexcept {
    switch (static_cast<PatternIndexRebuildTrigger>(trigger)) {
        case PatternIndexRebuildTrigger::EagerMutate:
            pattern_index_eager_mutate_rebuilds_.fetch_add(1, std::memory_order_relaxed);
            break;
        case PatternIndexRebuildTrigger::EagerCow:
            pattern_index_eager_cow_rebuilds_.fetch_add(1, std::memory_order_relaxed);
            break;
        case PatternIndexRebuildTrigger::LazyQuery:
        default:
            pattern_index_lazy_rebuilds_.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

void Evaluator::maybe_eager_rebuild_pattern_index_after_cow() const noexcept {
    if (pattern_index_policy_ == PatternIndexPolicy::EagerAfterCow && workspace_flat_)
        build_tag_arity_index(static_cast<std::uint8_t>(PatternIndexRebuildTrigger::EagerCow));
}

void Evaluator::build_tag_arity_index_unlocked(std::uint8_t trigger) const {
    // Issue #371 / #1372: callers must hold tag_arity_index_mtx_
    // exclusively. Nested helpers (insert/remove/rebuild) are
    // lock-free under that umbrella.
    if (!workspace_flat_) {
        tag_arity_index_.clear();
        tag_arity_indexed_key_.clear();
        tag_arity_index_workspace_ = nullptr;
        tag_arity_index_synced_size_ = 0;
        tag_arity_index_synced_gen_ = 0;
        tag_arity_index_epoch_.fetch_add(1, std::memory_order_release);
        return;
    }
    const auto& flat = *workspace_flat_;

    if (tag_arity_index_workspace_ != workspace_flat_ || tag_arity_index_.empty()) {
        tag_arity_index_rebuild_full(flat);
        tag_arity_index_epoch_.fetch_add(1, std::memory_order_release);
        bump_pattern_index_rebuild_trigger(trigger);
        return;
    }

    const std::size_t cur_size = flat.size();
    const auto cur_gen = flat.generation();
    if (cur_size == tag_arity_index_synced_size_ && cur_gen == tag_arity_index_synced_gen_)
        return;

    if (cur_gen == tag_arity_index_synced_gen_ && cur_size > tag_arity_index_synced_size_) {
        tag_arity_index_append_nodes(flat, tag_arity_index_synced_size_);
        tag_arity_index_epoch_.fetch_add(1, std::memory_order_release);
        bump_pattern_index_rebuild_trigger(trigger);
        return;
    }

    tag_arity_index_sync_after_mutation(flat);
    tag_arity_index_epoch_.fetch_add(1, std::memory_order_release);
    flat.bump_tag_arity_index_delta_hits();
    bump_edsl_tag_arity_delta_patch();
    bump_pattern_index_rebuild_trigger(trigger);
}

void Evaluator::build_tag_arity_index(std::uint8_t trigger) const {
    // Issue #371: take unique_lock for the duration of the
    // build/sync path. Nested helpers assume lock is held.
    std::unique_lock<std::shared_mutex> wlock(tag_arity_index_mtx_);
    build_tag_arity_index_unlocked(trigger);
}

std::vector<aura::ast::NodeId> Evaluator::snapshot_tag_arity_bucket(std::uint64_t key,
                                                                    std::uint8_t trigger) const {
    // Issue #1372: single unique_lock covers build + bucket copy.
    // Closes the #371 race window (build release → shared find).
    // Matchers iterate the returned snapshot outside the lock so
    // reader parallelism is preserved for the expensive match work.
    std::unique_lock<std::shared_mutex> wlock(tag_arity_index_mtx_);
    build_tag_arity_index_unlocked(trigger);
    const auto epoch_at_copy = tag_arity_index_epoch_.load(std::memory_order_acquire);
    auto it = tag_arity_index_.find(key);
    if (it == tag_arity_index_.end())
        return {};
    std::vector<aura::ast::NodeId> out = it->second;
    // Defensive: under unique_lock epoch cannot change; if it
    // ever does, count a race-window hit and return empty so
    // callers fall back safely.
    if (tag_arity_index_epoch_.load(std::memory_order_acquire) != epoch_at_copy) {
        tag_arity_index_race_window_hits_.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    return out;
}

void Evaluator::verify_pattern_result_hygiene(const aura::ast::FlatAST& flat, EvalValue result,
                                              bool with_markers) noexcept {
    auto walk = [&](EvalValue cur) {
        while (is_pair(cur)) {
            const auto pidx = as_pair_idx(cur);
            if (pidx >= pairs_.size())
                break;
            auto item = pairs_[pidx].car;
            if (with_markers && is_pair(item)) {
                const auto iidx = as_pair_idx(item);
                if (iidx < pairs_.size())
                    item = pairs_[iidx].car;
            }
            if (is_int(item)) {
                const auto id = static_cast<aura::ast::NodeId>(as_int(item));
                if (id < flat.size() && flat.is_macro_introduced(id)) {
                    pattern_macro_filter_violations_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            cur = pairs_[pidx].cdr;
        }
    };
    walk(result);
}

void Evaluator::ensure_pattern_macro_filter_consistency(
    const aura::ast::FlatAST& flat) const noexcept {
    (void)flat;
    // Issue #421: lightweight probe hook — violations are
    // recorded by verify_pattern_result_hygiene on each
    // default-hygiene query:pattern return. Tests call this
    // to assert the post-split filter contract is wired.
}

void Evaluator::ensure_pattern_index_consistency(const aura::ast::FlatAST& flat) const noexcept {
    // Issue #423: verify Evaluator-side tag_arity_index_
    // stays in sync with the workspace flat after
    // query:pattern fast-path builds and incremental sync.
    std::shared_lock<std::shared_mutex> rlock(tag_arity_index_mtx_);
    if (tag_arity_index_.empty())
        return;

    auto bump_violation = [&]() noexcept {
        pattern_index_consistency_violations_.fetch_add(1, std::memory_order_relaxed);
    };

    if (tag_arity_index_workspace_ != workspace_flat_)
        bump_violation();
    if (tag_arity_index_synced_size_ != flat.size())
        bump_violation();
    if (tag_arity_index_synced_gen_ != flat.generation())
        bump_violation();

    for (const auto& [key, bucket] : tag_arity_index_) {
        const auto expected_tag = static_cast<aura::ast::NodeTag>(key >> 32);
        const auto expected_arity = static_cast<std::size_t>(key & 0xFFFFFFFFu);
        for (aura::ast::NodeId id : bucket) {
            if (id >= flat.size()) {
                bump_violation();
                continue;
            }
            const auto node = flat.get(id);
            if (node.tag != expected_tag || node.children.size() != expected_arity) {
                bump_violation();
            }
        }
    }
}

} // namespace aura::compiler