// evaluator_primitives_query_defuse.cpp — P0 step 10: def-use index query primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimFn = std::function<EvalValue(std::span<const EvalValue>)>;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;
using MakeMerr = std::function<EvalValue(const std::string&, const std::string&)>;

using namespace types;

struct DefUseQueryCallbacks {
    std::function<void*()> ensure_defuse;
    std::function<EvalValue(void* idx, aura::ast::SymId sym)> def_use_for_sym;
    std::function<EvalValue(void* idx, aura::ast::NodeId node)> reaches_for_node;
    std::function<EvalValue(void* idx, aura::ast::SymId sym)> effects_for_sym;
    std::function<EvalValue(void* idx)> build_index;
    std::function<EvalValue(void* idx)> index_stats;
};

void register_defuse_query_primitives(
    PrimRegistrar add, std::shared_mutex& workspace_mtx, aura::ast::FlatAST*& workspace_flat,
    aura::ast::StringPool*& workspace_pool, std::pmr::vector<std::string>& string_heap,
    std::function<void*()> ensure_defuse,
    std::function<EvalValue(void* idx, aura::ast::SymId sym)> def_use_for_sym,
    std::function<EvalValue(void* idx, aura::ast::NodeId node)> reaches_for_node,
    std::function<EvalValue(void* idx, aura::ast::SymId sym)> effects_for_sym,
    std::function<EvalValue(void* idx)> build_index, std::function<EvalValue(void* idx)> index_stats,
    MakeMerr make_merr) {
    DefUseQueryCallbacks cb{std::move(ensure_defuse), std::move(def_use_for_sym), std::move(reaches_for_node),
                            std::move(effects_for_sym), std::move(build_index), std::move(index_stats)};

    add("query:def-use", [&workspace_mtx, &workspace_flat, &workspace_pool, &string_heap, cb,
                          make_merr](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(workspace_mtx);
        if (a.empty() || !is_string(a[0]))
            return make_merr("bad-arg", "usage: (query:def-use sym-name)");
        if (!workspace_flat || !workspace_pool)
            return make_merr("no-workspace", "no workspace AST loaded");
        auto sym_idx = as_string_idx(a[0]);
        if (sym_idx >= string_heap.size())
            return make_merr("bad-arg", "symbol name string index out of range");
        auto target_sym = workspace_pool->intern(string_heap[sym_idx]);
        auto idx = cb.ensure_defuse();
        if (!idx)
            return make_merr("internal", "failed to build def-use index");
        return cb.def_use_for_sym(idx, target_sym);
    });

    add("query:reaches", [&workspace_mtx, &workspace_flat, cb, make_merr](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(workspace_mtx);
        if (a.empty() || !is_int(a[0]))
            return make_merr("bad-arg", "usage: (query:reaches node-id)");
        if (!workspace_flat)
            return make_merr("no-workspace", "no workspace AST loaded");
        auto target = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat;
        if (target >= flat.size())
            return make_merr("out-of-range", "node ID " + std::to_string(target) + " >= flat size " +
                                                 std::to_string(flat.size()));
        auto idx = cb.ensure_defuse();
        if (!idx)
            return make_merr("internal", "failed to build def-use index");
        return cb.reaches_for_node(idx, target);
    });

    add("query:effects", [&workspace_mtx, &workspace_flat, &workspace_pool, &string_heap, cb,
                          make_merr](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(workspace_mtx);
        if (a.empty() || !is_string(a[0]))
            return make_merr("bad-arg", "usage: (query:effects sym-name)");
        if (!workspace_flat || !workspace_pool)
            return make_merr("no-workspace", "no workspace AST loaded");
        auto sym_idx = as_string_idx(a[0]);
        if (sym_idx >= string_heap.size())
            return make_merr("bad-arg", "symbol name string index out of range");
        auto target_sym = workspace_pool->intern(string_heap[sym_idx]);
        auto idx = cb.ensure_defuse();
        if (!idx)
            return make_merr("internal", "failed to build def-use index");
        return cb.effects_for_sym(idx, target_sym);
    });

    add("query:build-index", [&workspace_mtx, cb, make_merr](const auto& a) -> EvalValue {
        (void)a;
        std::shared_lock<std::shared_mutex> rlock(workspace_mtx);
        auto idx = cb.ensure_defuse();
        if (!idx)
            return make_merr("internal", "failed to build def-use index");
        return cb.build_index(idx);
    });

    add("query:index-stats", [&workspace_mtx, cb, make_merr](const auto& a) -> EvalValue {
        (void)a;
        std::shared_lock<std::shared_mutex> rlock(workspace_mtx);
        auto idx = cb.ensure_defuse();
        if (!idx)
            return make_merr("internal", "failed to build def-use index");
        return cb.index_stats(idx);
    });
}

} // namespace aura::compiler::primitives_detail