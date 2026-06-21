// evaluator_query_index.cpp — P1-o: tag/arity index for query:pattern
// aura.compiler.evaluator module partition.

module;

#include <cstdint>

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler {

using types::EvalValue;
using namespace types;

void Evaluator::build_tag_arity_index() const {
    // If the index is already built for the current
    // workspace, no-op. This is the fast path: the index
    // is built once and reused across multiple
    // query:pattern calls.
    if (tag_arity_index_workspace_ == workspace_flat_ && !tag_arity_index_.empty()) {
        return;
    }
    // The index is stale (different workspace or empty).
    // Rebuild.
    tag_arity_index_.clear();
    tag_arity_index_workspace_ = workspace_flat_;
    if (!workspace_flat_)
        return;
    const auto& flat = *workspace_flat_;
    const std::size_t n = flat.size();
    tag_arity_index_.reserve(n);
    for (aura::ast::NodeId id = 0; id < n; ++id) {
        const auto node = flat.get(id);
        const auto tag = static_cast<std::uint32_t>(node.tag);
        const auto arity = static_cast<std::uint32_t>(node.children.size());
        const std::uint64_t key =
            (static_cast<std::uint64_t>(tag) << 32) | static_cast<std::uint64_t>(arity);
        tag_arity_index_[key].push_back(id);
    }
}

} // namespace aura::compiler
