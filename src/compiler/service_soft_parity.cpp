// service_soft_parity.cpp - CompilerService soft dual-graph parity consult
// moved out of the module interface (#5242). Move-only: the body is unchanged.

module;

#include "observability_metrics.h"
#include "compiler/typed_mutation_audit.h" // typed_audit::
#include "lock_order_audit.h"              // lock_order
#include "core/transparent_string_hash.hh" // TransparentStringHash

module aura.compiler.service;

import std;

namespace aura::compiler {

bool CompilerService::fail_closed_soft_dual_graph_parity_before_partial_cone_(
    std::vector<std::string>& cone_names, bool& want_partial) {
    if (!want_partial || cone_names.empty())
        return false;
    // AC3: Soft / Off + clean single-fiber skips the consult entirely.
    const bool production_or_full = aura::compiler::typed_audit::production_defaults_active() ||
                                    aura::compiler::typed_audit::get_strategy() ==
                                        aura::compiler::typed_audit::AuditStrategy::Full;
    if (!production_or_full)
        return false;
    // Issue #3615: empty string called_by + any node dependents.
    // Issue #3891: extra fn-node dependents not in string called_by
    // even when called_by is nonempty. Caller holds dep_graph_mtx_.
    auto cone_has_node_only_hole = [&]() -> bool {
        for (const auto& root : cone_names) {
            const auto slot_it = dep_name_to_slot_.find(root);
            if (slot_it == dep_name_to_slot_.end())
                continue;
            const auto fn_from = aura::compiler::dirty::encode_fn_node(slot_it->second);
            const auto* deps = node_dep_graph_.dependents(fn_from);
            if (!deps)
                continue;
            const auto dit = dep_graph_.find(root);
            if (dit == dep_graph_.end() || dit->second.called_by.empty()) {
                for (const auto& n : *deps) {
                    (void)n;
                    return true;
                }
                continue;
            }
            for (const auto& n : *deps) {
                if (!aura::compiler::dirty::is_fn_node(n))
                    continue;
                const auto slot = aura::compiler::dirty::decode_fn_slot(n);
                if (slot >= dep_slot_to_name_.size())
                    continue;
                const auto& nm = dep_slot_to_name_[slot];
                if (nm.empty() || nm == root)
                    continue;
                bool in_string = false;
                for (const auto& caller : dit->second.called_by) {
                    if (caller == nm) {
                        in_string = true;
                        break;
                    }
                }
                if (!in_string)
                    return true;
            }
        }
        return false;
    };
    bool parity_fail = false;
    {
        lock_order::OrderedSharedLock<std::shared_mutex> read(dep_graph_mtx_,
                                                              lock_order::Level::DepGraph);
        if (!aura::compiler::dirty::graphs_consistent(dep_graph_, node_dep_graph_,
                                                      dep_name_to_slot_)) {
            parity_fail = true;
        } else {
            // AC2: Soft-erased hole + #3891 extra node-only callers.
            parity_fail = cone_has_node_only_hole();
        }
    }
    if (!parity_fail)
        return false;
    // Exclusive rebuild + force-dirty all callers (same shape as
    // fail_closed_soft_dual_graph_parity_before_partial_).
    lock_order::OrderedUniqueLock<std::shared_mutex> write(dep_graph_mtx_,
                                                           lock_order::Level::DepGraph);
    bool still_fail =
        !aura::compiler::dirty::graphs_consistent(dep_graph_, node_dep_graph_, dep_name_to_slot_);
    if (!still_fail)
        still_fail = cone_has_node_only_hole();
    if (!still_fail)
        return false;
    // Issue #3761: decode node dependents of each cone root and
    // mark_caller_body_dirty (#3474 union) *before* remirror drops
    // the only record of a node-only caller. Extra node edges do not
    // fail graphs_consistent; the cone walk is the production observer
    // (Issue #3891: including nonempty string called_by).
    {
        using aura::compiler::dirty::decode_block_dep_node;
        using aura::compiler::dirty::decode_fn_slot;
        using aura::compiler::dirty::encode_fn_node;
        using aura::compiler::dirty::is_block_dep_node;
        using aura::compiler::dirty::is_fn_node;
        const std::size_t n_roots = cone_names.size();
        for (std::size_t i = 0; i < n_roots; ++i) {
            const auto root = cone_names[i];
            const auto slot_it = dep_name_to_slot_.find(root);
            if (slot_it == dep_name_to_slot_.end())
                continue;
            const auto* deps = node_dep_graph_.dependents(encode_fn_node(slot_it->second));
            if (!deps)
                continue;
            for (const auto n : *deps) {
                std::uint32_t slot = UINT32_MAX;
                if (is_fn_node(n))
                    slot = decode_fn_slot(n);
                else if (is_block_dep_node(n))
                    slot = decode_block_dep_node(n).caller_slot;
                else
                    continue;
                if (slot >= dep_slot_to_name_.size())
                    continue;
                const auto& nm = dep_slot_to_name_[slot];
                if (nm.empty() || nm == root)
                    continue;
                auto cit = ir_cache_v2_.find(nm);
                if (cit != ir_cache_v2_.end()) {
                    (void)cit->second.mark_caller_body_dirty();
                    finish_cascade_soa_dirty_sync_(cit->second);
                }
                if (std::find(cone_names.begin(), cone_names.end(), nm) == cone_names.end())
                    cone_names.push_back(nm);
            }
        }
    }
    aura::compiler::dirty::rebuild_node_dep_graph_from_string(node_dep_graph_, dep_graph_,
                                                              dep_name_to_slot_);
    metrics_.dual_dep_graph_parity_fail_total.fetch_add(1, std::memory_order_relaxed);
    aura::compiler::dirty::g_dual_dep_graph_parity_fail_total_atomic().fetch_add(
        1, std::memory_order_relaxed);
    std::unordered_set<std::string, aura::core::TransparentStringHash, std::equal_to<>> affected;
    for (const auto& [callee_name, callee_entry] : dep_graph_) {
        (void)callee_name;
        for (const auto& caller_name : callee_entry.called_by)
            affected.insert(caller_name);
    }
    for (const auto& caller_name : affected) {
        auto cit2 = ir_cache_v2_.find(caller_name);
        if (cit2 != ir_cache_v2_.end()) {
            cit2->second.dirty = true;
            cit2->second.mark_all_blocks_dirty();
            finish_cascade_soa_dirty_sync_(cit2->second);
        }
    }
    want_partial = false;
    metrics_.partial_forced_full_by_impact_total.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace aura::compiler
