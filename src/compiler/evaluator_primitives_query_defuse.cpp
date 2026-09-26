// evaluator_primitives_query_defuse.cpp — P0 step 10: def-use index query primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "typed_mutation_audit.h" // Issue #4106: production gate
#include "serve/fiber.h"          // Issue #4106: aura_fiber_current_id

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

// Issue #3424/#4106: schema-2 QueryResult hash -> node identity + shared
// v2 unpack (shared with mutate / workspace). Include in module purview
// (after the imports) so aura::core / aura::ast / types are in scope.
#include "compiler/query_result_decode.hh"

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;
using MakeMerr = std::function<EvalValue(const std::string&, const std::string&)>;

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

struct DefUseQueryCallbacks {
    std::function<void*()> ensure_defuse;
    std::function<EvalValue(void* idx, aura::ast::SymId sym)> def_use_for_sym;
    std::function<EvalValue(void* idx, aura::ast::NodeId node)> reaches_for_node;
    std::function<EvalValue(void* idx, aura::ast::SymId sym)> effects_for_sym;
    std::function<EvalValue(void* idx)> build_index;
    std::function<EvalValue(void* idx)> index_stats;
};

// Issue #3175: diagnostic / low-frequency query: names stay compiled
// but are not registered. SlimSurface scans add() only.
template <typename... Ts> void sink_query_prim(std::string_view name, Ts&&...) {
    (void)name;
}

void register_defuse_query_primitives(
    PrimRegistrar add, std::shared_mutex& workspace_mtx, aura::ast::FlatAST*& workspace_flat,
    aura::ast::StringPool*& workspace_pool, std::pmr::vector<std::string>& string_heap,
    std::function<void*()> ensure_defuse,
    std::function<EvalValue(void* idx, aura::ast::SymId sym)> def_use_for_sym,
    std::function<EvalValue(void* idx, aura::ast::NodeId node)> reaches_for_node,
    std::function<EvalValue(void* idx, aura::ast::SymId sym)> effects_for_sym,
    std::function<EvalValue(void* idx)> build_index,
    std::function<EvalValue(void* idx)> index_stats, MakeMerr make_merr) {
    DefUseQueryCallbacks cb{std::move(ensure_defuse),    std::move(def_use_for_sym),
                            std::move(reaches_for_node), std::move(effects_for_sym),
                            std::move(build_index),      std::move(index_stats)};

    // Issue #2628: private for (query :def-use); not a public add().
    ObservabilityPrims::register_stats_impl(
        "query:def-use",
        [&workspace_mtx, &workspace_flat, &workspace_pool, &string_heap, cb,
         make_merr](const auto& a) -> EvalValue {
            std::shared_lock<std::shared_mutex> rlock(workspace_mtx);
            if (a.empty() || !is_string(a[0]))
                return make_merr("bad-arg", "usage: (query :def-use sym-name)");
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

    add("query:reaches",
        [&workspace_mtx, &workspace_flat, cb, make_merr](const auto& a) -> EvalValue {
            std::shared_lock<std::shared_mutex> rlock(workspace_mtx);
            if (a.empty())
                return make_merr("bad-arg", "usage: (query:reaches node-id)");
            if (!workspace_flat)
                return make_merr("no-workspace", "no workspace AST loaded");
            auto& flat = *workspace_flat;
            aura::ast::NodeId target = aura::ast::NULL_NODE;
            // Issue #4106: production resolves the operand through the shared
            // gates before the def-use walk — a bare int is occupancy, not
            // identity, and is rejected with the #3395 stale-ref face (a
            // recycled slot must never report the new occupant's reaches).
            // Packed v2 StableNodeRef / schema-2 QueryResult operands resolve
            // + validate with the non-refresh rule (Issue #3661). Soft keeps
            // the historical bare-int walk (out-of-range → out-of-range).
            if (aura::compiler::typed_audit::production_defaults_active()) {
                auto* qev = Evaluator::get_query_evaluator();
                if (!qev)
                    return make_merr("no-workspace", "no workspace AST loaded");
                if (is_hash(a[0])) {
                    using aura::compiler::query_result_decode::HashNodeKind;
                    using aura::compiler::query_result_decode::parse_query_result_match_index;
                    using aura::compiler::query_result_decode::resolve_query_result_match;
                    auto hr = resolve_query_result_match(
                        a[0], qev->string_heap_mut(), qev->pairs(), flat,
                        qev->capability_tenant_id(),
                        static_cast<std::uint64_t>(aura_fiber_current_id()), "query:reaches",
                        parse_query_result_match_index(a, qev->keyword_table()));
                    if (hr.kind != HashNodeKind::Ok)
                        return make_merr(hr.err_kind, hr.err_msg);
                    target = hr.node;
                } else if (auto packed =
                               aura::compiler::query_result_decode::unpack_query_stable_ref_v2(
                                   qev->pairs(), a[0])) {
                    auto ref = *packed;
                    qev->stamp_query_stable_ref_export(ref);
                    if (ref.id == aura::ast::NULL_NODE)
                        return make_merr("restamp-lag",
                                         "budget-exceeded: query:reaches: restamp budget "
                                         "exceeded; generation torn for export (Issue #3230)");
                    if (!qev->ensure_valid_or_refresh(ref, /*auto_refresh=*/false).has_value())
                        return make_merr("stale-ref", "query:reaches: stable-ref is stale or "
                                                      "provenance ensure failed");
                    target = ref.id;
                } else {
                    qev->bump_raw_nodeid_usage_in_primitives_count();
                    return make_merr("stale-ref",
                                     "query:reaches: raw node-id rejected under production; "
                                     "use packed v2 StableNodeRef or QueryResult match (Issue "
                                     "#3395)");
                }
            } else {
                if (!is_int(a[0]))
                    return make_merr("bad-arg", "usage: (query:reaches node-id)");
                target = static_cast<aura::ast::NodeId>(as_int(a[0]));
                if (target >= flat.size())
                    return make_merr("out-of-range", "node ID " + std::to_string(target) +
                                                         " >= flat size " +
                                                         std::to_string(flat.size()));
            }
            auto idx = cb.ensure_defuse();
            if (!idx)
                return make_merr("internal", "failed to build def-use index");
            return cb.reaches_for_node(idx, target);
        });

    add("query:effects",
        [&workspace_mtx, &workspace_flat, &workspace_pool, &string_heap, cb,
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

    sink_query_prim("query:build-index",
                    [&workspace_mtx, cb, make_merr](const auto& a) -> EvalValue {
                        (void)a;
                        std::shared_lock<std::shared_mutex> rlock(workspace_mtx);
                        auto idx = cb.ensure_defuse();
                        if (!idx)
                            return make_merr("internal", "failed to build def-use index");
                        return cb.build_index(idx);
                    });

    ObservabilityPrims::register_stats_impl(
        "query:index-stats", [&workspace_mtx, cb, make_merr](const auto& a) -> EvalValue {
            (void)a;
            std::shared_lock<std::shared_mutex> rlock(workspace_mtx);
            auto idx = cb.ensure_defuse();
            if (!idx)
                return make_merr("internal", "failed to build def-use index");
            return cb.index_stats(idx);
        });
}

} // namespace aura::compiler::primitives_detail