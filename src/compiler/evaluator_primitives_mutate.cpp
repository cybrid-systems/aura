// evaluator_primitives_mutate.cpp — P0 step 11: mutate:* primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "messaging_bridge.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.compiler.coercion_map;
import aura.compiler.matcher;
import aura.parser.parser;
import aura.core.mutators;  // Phase 4 follow-up #3: migrate mutate:* primitives to strategy dispatch
import aura.diag;
import aura.compiler.hardware_backend;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;
using MakeErrorVal = std::function<EvalValue(const std::string&, const std::string&)>;

using namespace types;

namespace {

using StableNodeRef = aura::ast::FlatAST::StableNodeRef;

// Issue #270: verify a node is still attached under its parent.
std::optional<std::uint32_t> parent_child_index_if_attached(
    const aura::ast::FlatAST& flat, aura::ast::NodeId match_id) {
    if (match_id == aura::ast::NULL_NODE || match_id >= flat.size())
        return std::nullopt;
    auto parent_id = flat.parent_of(match_id);
    if (parent_id == aura::ast::NULL_NODE || parent_id >= flat.size())
        return std::nullopt;
    auto pv = flat.get(parent_id);
    for (std::uint32_t ci = 0; ci < pv.children.size(); ++ci) {
        if (pv.child(ci) == match_id)
            return ci;
    }
    return std::nullopt;
}

bool stable_match_still_attached(const aura::ast::FlatAST& flat,
                                 const StableNodeRef& match_ref) {
    if (!match_ref.is_valid_in(flat))
        return false;
    return parent_child_index_if_attached(flat, match_ref.id).has_value();
}

// Issue #348: walk a subtree, collect all
// (if pred then else) node ids, and mark each with
// kOccurrenceDirty via the set_occurrence_dirty_fn_
// hook. The walk is recursive over children; we
// skip nullptr + free-slot nodes (the recycled
// tombstone state from the free list).
//
// The walk is bounded: we visit each node at most
// once (the visited set is the new_value's subtree
// + the old_value's subtree combined). For a
// typical rebind (a small expression body), the
// total cost is O(subtree_size) and the count of
// marked if-nodes is typically 0-3.
//
// The caller passes the new value's root; the
// walker visits the new value's subtree (the new
// function body) and marks every if-node there.
// The old value's subtree is also visited (the
// conservative path: if the old body had an
// if-context, the rebind may have invalidated it).
static void auto_wire_k_occurrence_dirty_for_subtree(
    aura::ast::FlatAST& flat,
    const std::function<bool(aura::ast::NodeId, bool)>& set_occurrence_dirty_fn,
    aura::ast::NodeId root) {
    if (!set_occurrence_dirty_fn || root == aura::ast::NULL_NODE
        || root >= flat.size()) return;
    // Iterative DFS to avoid stack overflow on deep ASTs.
    // We use a small vector as the work stack; a typical
    // subtree fits in a few dozen entries.
    std::vector<aura::ast::NodeId> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        const auto id = stack.back();
        stack.pop_back();
        if (id == aura::ast::NULL_NODE || id >= flat.size()) continue;
        const auto v = flat.get(id);
        // Mark the if-node if it's an IfExpr. The
        // hook signature is (node_id, set) where set
        // = true marks the bit and returns the prior
        // state. We always pass true (set the bit);
        // the return value is the prior state (we
        // don't use it here).
        if (v.tag == aura::ast::NodeTag::IfExpr) {
            set_occurrence_dirty_fn(id, true);
        }
        // Push children (in reverse order so the
        // first child is processed first; this
        // matches the natural left-to-right
        // traversal).
        if (!v.children.empty()) {
            for (std::size_t i = v.children.size(); i-- > 0; ) {
                const auto c = v.children[i];
                if (c != aura::ast::NULL_NODE) stack.push_back(c);
            }
        }
    }
}

}  // namespace

void register_mutate_primitives(
    PrimRegistrar add, Evaluator& ev, MakeErrorVal mev,
    std::function<void()> destroy_defuse_index) {

    // ── Typed mutation operators ──────────────────────────────────

    // (mutate:replace-type node-id new-type-str)
    // NOTE (refactor Step 0.2): local merr lambda removed; now uses the
    // centralized Evaluator::make_merr (added in 0.1).
    // Issue #213 Cycle 2: migrate mutate:replace-type to use
    // the MutationBoundaryGuard. The original code was missing
    // the workspace write lock (only had the version bump) —
    // migrating via the guard adds the lock that should have
    // been there.
    add("mutate:replace-type", [&ev](std::span<const EvalValue> a) -> EvalValue {
        bool ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
        // Yield at mutation boundary (Issue #31) — safe point before/after mutation.
        if (aura::messaging::g_fiber_yield_mutation_boundary)
            aura::messaging::g_fiber_yield_mutation_boundary();

        if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1])) {
            ok = false;
            return ev.make_merr("bad-arg", "usage: (mutate:replace-type node-id new-type)");
        }
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto type_idx = as_string_idx(a[1]);
        if (type_idx >= ev.string_heap_.size()) {
            ok = false;
            return ev.make_merr("bad-arg", "type string index out of range");
        }
        if (!ev.workspace_flat_) {
            ok = false;
            return ev.make_merr("no-workspace", "no workspace AST loaded");
        }
        auto& flat = *ev.workspace_flat_;
        if (node >= flat.size()) {
            ok = false;
            return ev.make_merr("out-of-range", "node ID " + std::to_string(node) + " >= flat size " +
                                                 std::to_string(flat.size()));
        }

        auto old_tid = flat.type_id(node);
        std::string old_type_str = (old_tid > 0) ? "#" + std::to_string(old_tid) : "Any";
        auto old_val = static_cast<std::uint64_t>(old_tid);
        auto new_val = static_cast<std::uint64_t>(ev.string_heap_.size()); // placeholder

        // Simple type ID mapping based on well-known type names
        auto type_str = ev.string_heap_[type_idx];
        std::uint32_t new_tid = 0;
        if (type_str == "Int")
            new_tid = 1;
        else if (type_str == "Float")
            new_tid = 2;
        else if (type_str == "String")
            new_tid = 3;
        else if (type_str == "Bool")
            new_tid = 4;
        else if (type_str == "Dyn" || type_str == "Any")
            new_tid = 0;
        else
            new_tid = 0; // unknown → Dyn

        auto mid = flat.add_mutation_with_rollback(
            node, "replace-type", old_type_str, ev.string_heap_[type_idx], "replace type annotation",
            aura::ast::MutationStatus::Committed, 1, old_val, new_tid, true);
        // Actually apply the type change
        flat.set_type(node, new_tid);
        ev.workspace_flat_->mark_dirty_upward(node);
        // Issue #213 Cycle 2: with the MutationBoundaryGuard,
        // the second version bump is now only needed on
        // success (to mirror the legacy behavior of
        // "enter-bump + exit-bump" = 2 total bumps per
        // boundary). The guard's destructor handles the
        // exit-bump on success (no rollback), and adds an
        // EXTRA bump on failure (rollback).
        if (aura::messaging::g_fiber_yield_mutation_boundary)
            aura::messaging::g_fiber_yield_mutation_boundary();
        return make_int(static_cast<std::int64_t>(mid));
    });

    // Issue #213 Cycle 2: migrate mutate:replace-value to use
    // the MutationBoundaryGuard. This primitive already uses
    // add_mutation_with_rollback for int_val_/float_val_/sym_id_
    // — so the rollback path actually restores the original
    // value on exit(success=false).
    add("mutate:replace-value", [&ev](std::span<const EvalValue> a) -> EvalValue {
        bool ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
        // (Step 0.2/0.3) local merr removed; using centralized make_merr
        // (declared in evaluator.ixx, defined above).
        aura::messaging::g_fiber_yield_mutation_boundary
            ? aura::messaging::g_fiber_yield_mutation_boundary()
            : (void)0; // safe point before mutation
        if (a.size() < 3 || !is_int(a[0]) || !is_string(a[2])) {
            ok = false;
            return ev.make_merr("bad-arg",
                                 "usage: (mutate:replace-value node-id new-value summary [ppa-hint])");
        }
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        std::uint8_t ppa_hint = 0;
        if (a.size() >= 4 && is_int(a[3]))
            ppa_hint = aura::compiler::hardware::parse_ppa_hint(as_int(a[3]));
        auto sum_idx = as_string_idx(a[2]);
        if (sum_idx >= ev.string_heap_.size()) {
            ok = false;
            return ev.make_merr("bad-arg", "summary string index out of range");
        }
        if (!ev.workspace_flat_) {
            ok = false;
            return ev.make_merr("no-workspace", "no workspace AST loaded");
        }
        auto& flat = *ev.workspace_flat_;
        if (node >= flat.size()) {
            ok = false;
            return ev.make_merr("out-of-range", "node ID " + std::to_string(node) + " >= flat size " +
                                                 std::to_string(flat.size()));
        }

        auto nv = flat.get(node);
        std::uint64_t old_val = 0;

        switch (nv.tag) {
            case aura::ast::NodeTag::LiteralInt: {
                if (!is_int(a[1])) {
                    ok = false;
                    return ev.make_merr("type-error", "LiteralInt node requires an integer value");
                }
                auto new_val = static_cast<std::int64_t>(as_int(a[1]));
                old_val = static_cast<std::uint64_t>(nv.int_value);
                auto mid = flat.add_mutation_with_rollback(
                    node, "replace-value", "Int", "Int", ev.string_heap_[sum_idx],
                    aura::ast::MutationStatus::Committed, 0, old_val,
                    static_cast<std::uint64_t>(new_val), true);
                flat.set_int(node, new_val);
                ev.workspace_flat_->mark_dirty_upward(node, aura::ast::FlatAST::kGeneralDirty,
                                                      ppa_hint);
                if (ppa_hint != 0)
                    aura::compiler::hardware::on_structural_mutation(
                        node, aura::ast::FlatAST::kGeneralDirty, ppa_hint);
                return make_int(static_cast<std::int64_t>(mid));
            }
            case aura::ast::NodeTag::LiteralFloat: {
                if (!is_float(a[1])) {
                    ok = false;
                    return ev.make_merr("type-error", "LiteralFloat node requires a float value");
                }
                // Pack double as uint64 for mutation log
                double new_val = as_float(a[1]);
                std::uint64_t new_bits;
                std::memcpy(&new_bits, &new_val, sizeof(new_bits));
                std::uint64_t old_bits;
                std::memcpy(&old_bits, &nv.float_value, sizeof(old_bits));
                auto mid = flat.add_mutation_with_rollback(
                    node, "replace-value", "Float", "Float", ev.string_heap_[sum_idx],
                    aura::ast::MutationStatus::Committed,
                    static_cast<std::uint32_t>(aura::ast::MutationSoAField::FloatVal), old_bits,
                    new_bits, true);
                flat.set_float(node, new_val);
                ev.workspace_flat_->mark_dirty_upward(node, aura::ast::FlatAST::kGeneralDirty,
                                                      ppa_hint);
                if (ppa_hint != 0)
                    aura::compiler::hardware::on_structural_mutation(
                        node, aura::ast::FlatAST::kGeneralDirty, ppa_hint);
                return make_int(static_cast<std::int64_t>(mid));
            }
            case aura::ast::NodeTag::Variable:
            case aura::ast::NodeTag::LiteralString: {
                if (!is_string(a[1])) {
                    ok = false;
                    return ev.make_merr("type-error",
                                     "Variable/LiteralString node requires a string value");
                }
                auto new_sym_idx = as_string_idx(a[1]);
                if (new_sym_idx >= ev.string_heap_.size()) {
                    ok = false;
                    return ev.make_merr("bad-arg", "new value string index out of range");
                }
                auto new_name = ev.string_heap_[new_sym_idx];
                old_val = nv.sym_id;
                auto new_sym = ev.workspace_pool_->intern(new_name);
                auto mid = flat.add_mutation_with_rollback(
                    node, "replace-value", "Sym", "Sym", ev.string_heap_[sum_idx],
                    aura::ast::MutationStatus::Committed,
                    static_cast<std::uint32_t>(aura::ast::MutationSoAField::SymId), old_val,
                    new_sym, true);
                flat.set_sym(node, new_sym);
                ev.workspace_flat_->mark_dirty_upward(node, aura::ast::FlatAST::kGeneralDirty,
                                                      ppa_hint);
                if (ppa_hint != 0)
                    aura::compiler::hardware::on_structural_mutation(
                        node, aura::ast::FlatAST::kGeneralDirty, ppa_hint);
                return make_int(static_cast<std::int64_t>(mid));
            }
            default:
                ok = false;
                return ev.make_merr("type-error", "node tag does not support value replacement: " +
                                                   std::to_string(static_cast<int>(nv.tag)));
        }
    });

    // Issue #213 Cycle 2: migrate mutate:record-patch to
    // use the MutationBoundaryGuard. The record-patch
    // primitive only logs the mutation (no field-level
    // change), so the rollback path is just "mark the
    // record as RolledBack + bump version" — no data
    // restoration needed.
    add("mutate:record-patch", [&ev](std::span<const EvalValue> a) -> EvalValue {
        bool ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
        // (Step 0.3 continuation) local merr removed; use centralized make_merr
        aura::messaging::g_fiber_yield_mutation_boundary
            ? aura::messaging::g_fiber_yield_mutation_boundary()
            : (void)0; // safe point before mutation
        if (a.size() < 3 || !is_int(a[0]) || !is_string(a[1]) || !is_string(a[2])) {
            ok = false;
            return ev.make_merr("bad-arg", "usage: (mutate:record-patch node-id op-name summary)");
        }
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto op_idx = as_string_idx(a[1]);
        auto sum_idx = as_string_idx(a[2]);
        if (op_idx >= ev.string_heap_.size() || sum_idx >= ev.string_heap_.size()) {
            ok = false;
            return ev.make_merr("bad-arg", "string index out of range");
        }
        if (!ev.workspace_flat_) {
            ok = false;
            return ev.make_merr("no-workspace", "no workspace AST loaded");
        }
        auto& flat = *ev.workspace_flat_;
        if (node >= flat.size()) {
            ok = false;
            return ev.make_merr("out-of-range", "node ID " + std::to_string(node) + " >= flat size " +
                                                 std::to_string(flat.size()));
        }

        auto mid = flat.add_mutation(node, ev.string_heap_[op_idx], "<runtime>", "<runtime>",
                                     ev.string_heap_[sum_idx]);
        return make_int(static_cast<std::int64_t>(mid));
    });
    // (#110, #270) mutate:query-and-replace - composes (query:where :field value)
    // predicates with replacement. Query phase snapshots flat.size() and
    // captures StableNodeRef; apply phase uses is_valid_in() + parent-slot
    // checks inside an atomic batch so multiple set_child calls share one bump.
    add(
        "mutate:query-and-replace", [&ev, mev, destroy_defuse_index](std::span<const EvalValue> a) -> EvalValue {
            std::unique_lock<std::shared_mutex> wlock(ev.workspace_mtx_);
            if (ev.workspace_read_only_)
                return mev("read-only", "workspace is read-only");
            if (a.empty())
                return mev(
                    "bad-arg",
                    "usage: (mutate:query-and-replace <predicates...> <template> [summary])");
            if (!ev.workspace_flat_ || !ev.workspace_pool_)
                return mev("no-workspace", "no workspace AST loaded");

            auto& flat = *ev.workspace_flat_;
            auto& pool = *ev.workspace_pool_;

            if (a.size() < 2 || !is_string(a.back()))
                return mev("bad-arg", "last arg must be a template string");

            auto template_idx = as_string_idx(a.back());
            if (template_idx >= ev.string_heap_.size())
                return mev("bad-arg", "template string index out of range");
            std::string repl_template = ev.string_heap_[template_idx];

            // Optional [ppa-hint] and [summary] before the template.
            std::size_t pred_end = a.size() - 1;
            std::string summary = "query-and-replace";
            std::uint8_t ppa_hint = 0;
            if (a.size() >= 3 && is_string(a[a.size() - 2])) {
                auto sidx = as_string_idx(a[a.size() - 2]);
                if (sidx < ev.string_heap_.size()) {
                    summary = ev.string_heap_[sidx];
                    pred_end = a.size() - 2;
                    if (a.size() >= 4 && is_int(a[a.size() - 3]))
                        ppa_hint =
                            aura::compiler::hardware::parse_ppa_hint(as_int(a[a.size() - 3]));
                }
            } else if (a.size() >= 3 && is_int(a[a.size() - 2])) {
                ppa_hint = aura::compiler::hardware::parse_ppa_hint(as_int(a[a.size() - 2]));
                pred_end = a.size() - 2;
            }

            struct Predicate {
                std::string field;
                std::string value;
            };
            std::vector<Predicate> predicates;
            for (std::size_t ai = 0; ai < pred_end; ++ai) {
                if (!is_pair(a[ai]))
                    return mev("bad-arg", "each predicate must be a (query:where ...) pair");
                auto pair_idx = as_pair_idx(a[ai]);
                if (pair_idx >= ev.pairs_.size())
                    return mev("bad-arg", "predicate pair index out of range");
                auto& p = ev.pairs_[pair_idx];
                if (!is_keyword(p.car) || !is_string(p.cdr))
                    return mev("bad-arg", "malformed predicate");
                auto kidx = as_keyword_idx(p.car);
                auto sidx = as_string_idx(p.cdr);
                if (kidx >= ev.keyword_table_.size() || sidx >= ev.string_heap_.size())
                    return mev("bad-arg", "predicate field/value out of range");
                predicates.push_back({ev.keyword_table_[kidx], ev.string_heap_[sidx]});
            }
            if (predicates.empty())
                return mev("bad-arg", "at least one predicate required");

            // Collect matches. Snapshot flat.size() so set_child during replacement
            // (which grows flat) doesn't extend the match scan. (#110 fix: bug
            // exposed by re-evaluating flat.size() each iteration.)
            // Issue #270: store StableNodeRef so apply can validate each match.
            const auto end_id = flat.size();
            std::vector<StableNodeRef> matches;
            for (aura::ast::NodeId id = 0; id < end_id; ++id) {
                auto v = flat.get(id);
                bool match = true;
                for (auto& p : predicates) {
                    if (p.field == ":node-type" || p.field == ":tag") {
                        bool found = false;
                        for (auto& m : aura::ast::kNodeMeta) {
                            if (m.name == p.value && m.name != "<gap>") {
                                if (v.tag == m.tag)
                                    found = true;
                                break;
                            }
                        }
                        if (!found) {
                            match = false;
                            break;
                        }
                    } else if (p.field == ":callee") {
                        if (v.tag != aura::ast::NodeTag::Call || v.children.empty()) {
                            match = false;
                            break;
                        }
                        auto callee = flat.get(v.child(0));
                        if (callee.tag != aura::ast::NodeTag::Variable ||
                            pool.resolve(callee.sym_id) != p.value) {
                            match = false;
                            break;
                        }
                    } else if (p.field == ":defined-by" || p.field == ":defines") {
                        if (v.tag != aura::ast::NodeTag::Define) {
                            match = false;
                            break;
                        }
                        if (pool.resolve(v.sym_id) != p.value) {
                            match = false;
                            break;
                        }
                    } else if (p.field == ":has-param") {
                        bool found_param = false;
                        for (auto pid : v.params) {
                            if (pool.resolve(pid) == p.value) {
                                found_param = true;
                                break;
                            }
                        }
                        if (!found_param) {
                            match = false;
                            break;
                        }
                    } else if (p.field == ":has-child") {
                        aura::ast::NodeTag child_tag = static_cast<aura::ast::NodeTag>(-1);
                        bool found_tag = false;
                        for (auto& m : aura::ast::kNodeMeta) {
                            if (m.name == p.value && m.name != "<gap>") {
                                child_tag = m.tag;
                                found_tag = true;
                                break;
                            }
                        }
                        if (!found_tag) {
                            match = false;
                            break;
                        }
                        bool has_child = false;
                        for (auto cid : v.children) {
                            if (cid != aura::ast::NULL_NODE && flat.get(cid).tag == child_tag) {
                                has_child = true;
                                break;
                            }
                        }
                        if (!has_child) {
                            match = false;
                            break;
                        }
                    } else if (p.field == ":marker" || p.field == ":syntax-marker") {
                        auto m = flat.marker(id);
                        const char* mname = nullptr;
                        switch (m) {
                            case aura::ast::SyntaxMarker::User:
                                mname = "User";
                                break;
                            case aura::ast::SyntaxMarker::MacroIntroduced:
                                mname = "MacroIntroduced";
                                break;
                            case aura::ast::SyntaxMarker::BoolLiteral:
                                mname = "BoolLiteral";
                                break;
                        }
                        if (!mname || p.value != mname) {
                            match = false;
                            break;
                        }
                    } else {
                        return mev("unknown-field",
                                   std::string("unknown where field: \"") + p.field + "\"");
                    }
                }
                if (match)
                    matches.push_back(flat.make_ref(id));
            }

            if (matches.empty())
                return make_bool(false);

            // Source reconstruction
            std::function<std::string(aura::ast::NodeId)> node_to_source;
            node_to_source = [&](aura::ast::NodeId id) -> std::string {
                if (id >= flat.size() || id == aura::ast::NULL_NODE)
                    return "";
                auto v = flat.get(id);
                switch (v.tag) {
                    case aura::ast::NodeTag::LiteralInt:
                        return std::to_string(v.int_value);
                    case aura::ast::NodeTag::LiteralFloat:
                        return std::to_string(v.float_value);
                    case aura::ast::NodeTag::LiteralString:
                        return std::string("\"") + std::string(pool.resolve(v.sym_id)) + "\"";
                    case aura::ast::NodeTag::Variable:
                        return std::string(pool.resolve(v.sym_id));
                    case aura::ast::NodeTag::Call: {
                        std::string s = "(";
                        for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                            if (ci > 0)
                                s += " ";
                            s += node_to_source(v.child(ci));
                        }
                        return s + ")";
                    }
                    case aura::ast::NodeTag::Lambda: {
                        std::string s = "(lambda (";
                        for (std::size_t pi = 0; pi < v.params.size(); ++pi) {
                            if (pi > 0)
                                s += " ";
                            s += pool.resolve(v.params[pi]);
                        }
                        s += ")";
                        if (!v.children.empty())
                            s += " " + node_to_source(v.child(0));
                        return s + ")";
                    }
                    case aura::ast::NodeTag::IfExpr: {
                        std::string s = "(if";
                        for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                            if (ci < 3)
                                s += " " + node_to_source(v.child(ci));
                        }
                        return s + ")";
                    }
                    case aura::ast::NodeTag::Begin: {
                        std::string s = "(begin";
                        for (auto c : v.children)
                            s += " " + node_to_source(c);
                        return s + ")";
                    }
                    case aura::ast::NodeTag::Define: {
                        std::string s = "(define " + std::string(pool.resolve(v.sym_id));
                        if (!v.children.empty()) {
                            auto val_node = flat.get(v.child(0));
                            if (val_node.tag == aura::ast::NodeTag::Lambda) {
                                s += " (";
                                for (std::size_t pi = 0; pi < val_node.params.size(); ++pi) {
                                    if (pi > 0)
                                        s += " ";
                                    s += pool.resolve(val_node.params[pi]);
                                }
                                s += ")";
                                if (!val_node.children.empty())
                                    s += " " + node_to_source(val_node.child(0));
                                s += ")";
                            } else {
                                s += " " + node_to_source(v.child(0));
                            }
                        }
                        return s + ")";
                    }
                    default:
                        return std::string("#<node-") + std::to_string(static_cast<int>(v.tag)) +
                               ">";
                }
            };

            // Count "..." occurrences
            std::size_t dot_count = 0;
            {
                std::size_t pos = 0;
                while ((pos = repl_template.find("...", pos)) != std::string::npos) {
                    ++dot_count;
                    pos += 3;
                }
            }

            ev.defuse_version_.fetch_add(1, std::memory_order_acq_rel);
            ev.total_mutations_.fetch_add(1, std::memory_order_relaxed);
            if (aura::messaging::g_fiber_yield_mutation_boundary)
                aura::messaging::g_fiber_yield_mutation_boundary();

            int replaced = 0;
            std::vector<aura::ast::NodeId> replaced_roots;
            flat.begin_atomic_batch();
            for (auto& match_ref : matches) {
                if (!stable_match_still_attached(flat, match_ref))
                    continue;
                auto match_id = match_ref.id;
                auto child_idx_opt = parent_child_index_if_attached(flat, match_id);
                if (!child_idx_opt)
                    continue;
                auto parent_id = flat.parent_of(match_id);

                std::vector<std::string> capture_sources;
                capture_sources.push_back(node_to_source(match_id));
                auto mv = flat.get(match_id);
                for (auto cid : mv.children) {
                    if (cid != aura::ast::NULL_NODE)
                        capture_sources.push_back(node_to_source(cid));
                    if (capture_sources.size() >= dot_count)
                        break;
                }

                std::string filled;
                if (dot_count == 0) {
                    filled = repl_template;
                } else {
                    std::size_t cap_idx = 0;
                    std::size_t pos = 0;
                    while (pos < repl_template.size()) {
                        auto dot_pos = repl_template.find("...", pos);
                        if (dot_pos == std::string::npos) {
                            filled += repl_template.substr(pos);
                            break;
                        }
                        filled += repl_template.substr(pos, dot_pos - pos);
                        if (cap_idx < capture_sources.size()) {
                            filled += capture_sources[cap_idx++];
                        }
                        pos = dot_pos + 3;
                    }
                }

                auto repl_pr = aura::parser::parse_to_flat(filled, flat, pool);
                if (!repl_pr.success || repl_pr.root == aura::ast::NULL_NODE)
                    continue;

                flat.set_child(parent_id, *child_idx_opt, repl_pr.root);
                replaced_roots.push_back(repl_pr.root);
                flat.add_mutation(repl_pr.root, "query-and-replace", "matched", "template",
                                  summary);
                ++replaced;
            }

            if (replaced == 0) {
                flat.rollback_atomic_batch();
                return make_bool(false);
            }
            flat.commit_atomic_batch();

            // Issue #262: precise dirty + incremental defuse refresh
            // instead of destroying the index and marking all defines.
            const auto structural_reasons = aura::ast::FlatAST::kGeneralDirty |
                                            aura::ast::FlatAST::kStructDirty;
            flat.mark_dirty_defuse_entries(replaced_roots, structural_reasons, ppa_hint);
            if (ppa_hint != 0) {
                for (auto root : replaced_roots)
                    aura::compiler::hardware::on_structural_mutation(root, structural_reasons,
                                                                   ppa_hint);
            }

            std::unordered_set<std::string> affected_names;
            auto collect_def_syms = [&](aura::ast::NodeId id, auto& self) -> void {
                if (id >= flat.size())
                    return;
                auto v = flat.get(id);
                if (v.tag == aura::ast::NodeTag::Define || v.tag == aura::ast::NodeTag::Let ||
                    v.tag == aura::ast::NodeTag::LetRec) {
                    auto n = pool.resolve(v.sym_id);
                    if (!n.empty())
                        affected_names.insert(std::string(n));
                }
                for (auto c : v.children) {
                    if (c != aura::ast::NULL_NODE)
                        self(c, self);
                }
            };
            for (auto root : replaced_roots)
                collect_def_syms(root, collect_def_syms);

            for (auto& n : affected_names) {
                ev.defuse_affected_syms_.insert(n);
                if (ev.mark_define_dirty_fn_)
                    ev.mark_define_dirty_fn_(n);
                auto s = pool.intern(n);
                if (ev.defuse_touch_fn_ && s != aura::ast::INVALID_SYM)
                    ev.defuse_touch_fn_(ev.defuse_index_, s);
            }

            if (ev.pre_cache_workspace_defines_fn_)
                ev.pre_cache_workspace_defines_fn_();

            return make_bool(true);
        });
    // Issue #235: (mutate:check-stable-ref stable-ref) — Verify
    // a stable-ref is still valid. Returns #t if the captured
    // node-id still has the same generation, #f otherwise.
    // Useful for agents that want to do an early validity check
    // before invoking a more expensive mutation.
    add("mutate:check-stable-ref", [&ev, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        if (!is_pair(a[0]))
            return mev("bad-arg", "usage: (mutate:check-stable-ref (id . gen))");
        if (!ev.workspace_flat_)
            return mev("no-workspace", "no workspace AST loaded");
        auto& flat = *ev.workspace_flat_;
        // Unpack the (id . gen) pair
        auto outer = as_pair_idx(a[0]);
        if (!is_int(ev.pairs_[outer].car))
            return mev("bad-arg", "stable-ref car must be a node id (int)");
        auto node = static_cast<aura::ast::NodeId>(as_int(ev.pairs_[outer].car));
        auto cdr = ev.pairs_[outer].cdr;
        if (!is_pair(cdr))
            return mev("bad-arg", "stable-ref cdr must be a pair (gen . nil)");
        auto inner = as_pair_idx(cdr);
        if (!is_int(ev.pairs_[inner].car))
            return mev("bad-arg", "stable-ref gen must be an int");
        auto captured_gen = static_cast<std::uint16_t>(as_int(ev.pairs_[inner].car));
        // Issue #391: consult the StaleRefPolicy. The
        // validity check is identical; the policy only
        // affects whether a stale ref blocks the mutate
        // (Strict) or just bumps the warned counter
        // (Warn). Disabled skips both.
        bool valid = (node < flat.size()) && (flat.generation() == captured_gen);
        if (!valid) {
            const auto policy = ev.get_stale_ref_policy();
            if (policy == aura::compiler::Evaluator::StaleRefPolicy::Disabled) {
                // No-op: don't bump counters, don't block.
            } else if (policy == aura::compiler::Evaluator::StaleRefPolicy::Strict) {
                ev.bump_stale_ref_blocked_count();
                // Tagged stale-ref error so the Agent can
                // distinguish from other mutate errors.
                auto idx = ev.string_heap_.size();
                ev.string_heap_.push_back("stale-ref");
                return mev(ev.string_heap_[idx].c_str(),
                           "stable-ref is stale (Strict policy blocked)");
            } else {
                // Warn
                ev.bump_stale_ref_warned_count();
            }
        }
        return make_bool(valid);
    });

    // Issue #391: (mutate:set-stale-ref-policy "warn"|"strict"|"disabled")
    // — set the global StaleRefPolicy for automatic
    // staleness checks in core mutate primitives.
    // P0: 3 policies. Disabled = no checks; Warn =
    // observe but don't block; Strict = block (return
    // tagged stale-ref error) on stale detection.
    add("mutate:set-stale-ref-policy", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        const auto& s = ev.string_heap_[idx];
        if (s == "disabled" || s == "off" || s == "false") {
            ev.set_stale_ref_policy(
                aura::compiler::Evaluator::StaleRefPolicy::Disabled);
            return make_bool(true);
        }
        if (s == "strict" || s == "hard") {
            ev.set_stale_ref_policy(
                aura::compiler::Evaluator::StaleRefPolicy::Strict);
            return make_bool(true);
        }
        if (s == "warn" || s == "observe" || s == "soft") {
            ev.set_stale_ref_policy(
                aura::compiler::Evaluator::StaleRefPolicy::Warn);
            return make_bool(true);
        }
        return make_bool(false);
    });

    // Issue #391: (query:stale-ref-policy) — return the
    // current StaleRefPolicy as a string ("disabled" /
    // "warn" / "strict"). Used by the AI Agent to
    // verify the policy before a long mutating run.
    add("query:stale-ref-policy", [&ev](const auto& a) -> EvalValue {
        (void)a;
        const char* name = "warn";
        switch (ev.get_stale_ref_policy()) {
            case aura::compiler::Evaluator::StaleRefPolicy::Disabled:
                name = "disabled"; break;
            case aura::compiler::Evaluator::StaleRefPolicy::Warn:
                name = "warn"; break;
            case aura::compiler::Evaluator::StaleRefPolicy::Strict:
                name = "strict"; break;
        }
        auto idx = ev.string_heap_.size();
        ev.string_heap_.push_back(name);
        return make_string(static_cast<std::int32_t>(idx));
    });

    // Issue #391: (query:stale-ref-stats) — return the
    // sum of stale_ref_blocked_count_ +
    // stale_ref_warned_count_ as an integer. Follow-up:
    // return a 2-tuple (blocked warned) so the AI Agent
    // can distinguish Strict-mode blocks from
    // Warn-mode observations.
    add("query:stale-ref-stats", [&ev](const auto& a) -> EvalValue {
        (void)a;
        return make_int(static_cast<std::int64_t>(
            ev.get_stale_ref_blocked_count() +
            ev.get_stale_ref_warned_count()));
    });

    // Issue #439: (mutate:request-gc-safepoint
    // [timeout-ms-int]) — request a GC safepoint.
    // Returns 0 if the safepoint can proceed
    // immediately (no outermost MutationBoundary
    // guard is held), or 1 if the request is
    // deferred (a guard is held; the caller should
    // yield + retry).
    //
    // The optional 1st arg is a timeout (in ms) for
    // a follow-up wait_for_safepoint call. P0: the
    // timeout is recorded (bump wait counter + bump
    // wait_total_ns) but the actual wait is a no-op
    // (the follow-up wires the real implementation).
    add("mutate:request-gc-safepoint", [&ev](const auto& a) -> EvalValue {
        const int result = ev.request_gc_safepoint();
        if (a.size() >= 1 && is_int(a[0])) {
            const auto timeout_ms =
                static_cast<std::uint64_t>(as_int(a[0]));
            ev.wait_for_safepoint(timeout_ms);
        }
        return make_int(static_cast<std::int64_t>(result));
    });

    // (mutate:rebind name new-code-string "summary") — Replace function definition by name
    // Unlike mutate:replace-value, this works by function name (no node ID needed).
    // Parses new code INTO the existing workspace FlatAST, then redirects the old
    // Define's value reference to the newly parsed nodes. All pre-existing mutations
    // on other nodes are preserved.
    // Issue #213 follow-up: migrate mutate:rebind to the
    // MutationBoundaryGuard. The guard handles the version
    // bump + lock + enter/exit_mutation_boundary.
    //
    // Issue #241 (scope-limited close — pilot primitive): the
    // panic_checkpoint lifecycle is now ALSO managed by the
    // Guard. Previously each mutate:* primitive called
    // ev.save_panic_checkpoint() at entry and either
    // ev.commit_panic_checkpoint() (success) or
    // ev.restore_panic_checkpoint() (failure + auto_rollback)
    // manually. The Guard now does all three automatically:
    //   - ctor: ev.save_panic_checkpoint()
    //   - dtor: commit on ok=true, restore on ok=false +
    //     ev.panic_auto_rollback_, leave alone on ok=false +
    //     !ev.panic_auto_rollback_
    // mutate:rebind is the pilot — other primitives (replace-type,
    // replace-value, record-patch, set-body, etc.) still do manual
    // panic_checkpoint handling until they're migrated in
    // follow-up issues.
    //
    // The guard's `ok` flag handles PLANNED rollback (via
    // MutationRecord inverse on exit(false)). The panic
    // checkpoint handles PANIC recovery (restores the saved
    // source if an exception is thrown during the body).
    // Both can fire on the same call site (e.g. typecheck
    // failure: guard rolls back the MutationRecord log +
    // panic_checkpoint restores the source).
    add("mutate:rebind", [&ev, mev](const auto& a) -> EvalValue {
        bool ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
        if (ev.workspace_read_only_) {
            ok = false;
            return mev("read-only", "workspace is read-only");
        }
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]) || !ev.workspace_flat_ ||
            !ev.workspace_pool_) {
            ok = false;
            return mev("bad-arg", "usage: (mutate:rebind name new-code-string [summary] [validate: type-name])");
        }
        // Issue #288: optional `validate: <type-name>` 4th arg.
        // When present, run the best-effort schema shape check on
        // the new code string before any workspace mutation. A
        // failure returns a tagged `schema-violation` error pair
        // and sets `ok = false` so the MutationBoundaryGuard dtor
        // rolls back (preserves the original binding).
        std::string validate_type;
        if (a.size() >= 4 && is_string(a[3])) {
            auto vidx = as_string_idx(a[3]);
            if (vidx < ev.string_heap_.size())
                validate_type = ev.string_heap_[vidx];
        }
        if (!validate_type.empty()) {
            auto validate_fn = ev.primitives_.lookup("mutate:validate-against-schema");
            if (validate_fn) {
                auto ci = ev.string_heap_.size();
                ev.string_heap_.push_back(ev.string_heap_[as_string_idx(a[1])]);
                auto ti = ev.string_heap_.size();
                ev.string_heap_.push_back(validate_type);
                auto vresult = (*validate_fn)({make_string(ci), make_string(ti)});
                if (is_string(vresult)) {
                    // tagged schema-violation — rejected
                    ok = false;
                    return vresult; // already a "(schema-violation ...)" string
                }
                if (is_bool(vresult) && !as_bool(vresult)) {
                    ok = false;
                    return mev("schema-violation", "schema check returned #f (no schema registered)");
                }
                // is_bool(vresult) == true → proceed
            }
        }
        // Issue #141 AC: lazy COW — trigger clone on first mutate, not on switch.
        if (ev.workspace_tree_) {
            if (!ev.trigger_lazy_cow(ev.workspace_tree_)) {
                ok = false;
                return mev("cow-refused", "COW refused: budget exceeded or read-only");
            }
            // Re-read ev.workspace_flat_/pool_ in case COW created a new local flat.
            void* new_flat = nullptr;
            void* new_pool = nullptr;
            if (ev.refresh_active_flat_pool(ev.workspace_tree_, &new_flat, &new_pool)) {
                ev.workspace_flat_ = static_cast<aura::ast::FlatAST*>(new_flat);
                ev.workspace_pool_ = static_cast<aura::ast::StringPool*>(new_pool);
            }
        }
        auto name_idx = as_string_idx(a[0]);
        auto code_idx = as_string_idx(a[1]);
        if (name_idx >= ev.string_heap_.size() || code_idx >= ev.string_heap_.size()) {
            ok = false;
            return mev("bad-arg", "string index out of range");
        }
        auto& flat = *ev.workspace_flat_;
        auto name = ev.string_heap_[name_idx];
        // Phase 2.5.0: route via ev.canonical_pool() (== workspace_pool, explicit).
        auto sym = ev.canonical_pool()->intern(name);

        // ── 安全点：panic checkpoint 现在由 MutationBoundaryGuard 自动管理 ────
        // Issue #241: Guard 在 ctor 调 ev.save_panic_checkpoint()，dtor 根据 ok + ev.panic_auto_rollback_
        // 决定 commit 或 restore。Body 不再需要手动 save / commit / restore。
        //   ok == true  → Guard dtor commit（清除 checkpoint）
        //   ok == false + ev.panic_auto_rollback_ → Guard dtor restore
        //   ok == false + !ev.panic_auto_rollback_ → Guard dtor leave alone（caller 可 retry）

        // ── 依赖图查询：通过 ev.dep_caller_fn_ 获取调用者节点 ────
        // ev.dep_caller_fn_ 在 init_pair_primitives 中注册，使用
        // DefUseIndex 的 O(k) 依赖图查询（k = 调用者数量）。
        // 在 ev.defuse_version_.fetch_add(1, std::memory_order_acq_rel)
        // 之前调用，因为索引在失效前有效。
        auto dep_callers =
            ev.dep_caller_fn_ ? ev.dep_caller_fn_(ev.defuse_index_, sym) : std::vector<aura::ast::NodeId>{};
        ev.defuse_version_.fetch_add(1, std::memory_order_acq_rel);
        ev.total_mutations_.fetch_add(1, std::memory_order_relaxed);

        // Find old Define node by name
        aura::ast::NodeId old_define = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id == sym) {
                old_define = id;
                break;
            }
        }
        // Issue #165 follow-up: when no matching Define exists, mutate:rebind
        // adds a brand-new top-level Define. This makes the primitive usable
        // for incrementally introducing a new top-level binding (a common
        // hygiene-stress scenario: introduce an outer binding of the same
        // name as a macro-introduced local and verify the macro's gensym
        // still protects the macro-internal one). The new node gets the
        // `add_mutation("add", …)` log entry so mutation_log_ stays
        // consistent with the rest of the rebind path.
        if (old_define == aura::ast::NULL_NODE) {
            // Parse the new code first so we know what to bind.
            auto pr_add =
                aura::parser::parse_to_flat(ev.string_heap_[code_idx], flat, *ev.workspace_pool_);
            if (!pr_add.success || pr_add.root == aura::ast::NULL_NODE) {
                std::string parse_err;
                if (!pr_add.errors.empty()) {
                    for (auto& e : pr_add.errors) {
                        if (!parse_err.empty())
                            parse_err += "; ";
                        parse_err += e.format();
                    }
                } else if (!pr_add.error.empty()) {
                    parse_err = pr_add.error;
                } else {
                    parse_err = "rebind code could not be parsed";
                }
                ok = false;
                return mev("parse-error", parse_err);
            }
            aura::ast::NodeId new_value = pr_add.root;
            auto root_v = flat.get(pr_add.root);
            if (root_v.tag == aura::ast::NodeTag::Define) {
                if (root_v.children.empty()) {
                    ok = false;
                    return mev("parse-error", "define form in rebind code has no body");
                }
                new_value = root_v.child(0);
            }
            std::string summary = (a.size() > 2 && is_string(a[2]))
                                      ? ev.string_heap_[as_string_idx(a[2])]
                                      : "add " + name;
            auto new_define = flat.add_define(sym, new_value);
            flat.add_mutation(new_define, "add", name, summary, summary);
            // Mark all defines dirty so cached IR for siblings gets invalidated.
            if (ev.mark_all_defines_dirty_fn_)
                ev.mark_all_defines_dirty_fn_();
            // Also update dep_graph_ and IR cache (lightweight + heavy hooks).
            if (ev.pre_cache_workspace_defines_fn_)
                ev.pre_cache_workspace_defines_fn_();
            return make_bool(true);
        }

        // Parse new code INTO workspace flat (append mode). All new node IDs
        // are valid in the same FlatAST and can be cross-referenced.
        auto pr = aura::parser::parse_to_flat(ev.string_heap_[code_idx], flat, *ev.workspace_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::string parse_err;
            if (!pr.errors.empty()) {
                for (auto& e : pr.errors) {
                    if (!parse_err.empty())
                        parse_err += "; ";
                    parse_err += e.format();
                }
            } else if (!pr.error.empty()) {
                parse_err = pr.error;
            } else {
                parse_err = "rebind code could not be parsed";
            }
            ok = false;
            return mev("parse-error", parse_err);
        }

        // The parsed root may be a Define (if code includes "(define (name ...) ...)")
        // or a bare expression (just the value/lambda). Extract the value.
        aura::ast::NodeId new_value = pr.root;
        auto root_v = flat.get(pr.root);
        if (root_v.tag == aura::ast::NodeTag::Define) {
            // New code is a full define — extract its value child
            if (root_v.children.empty()) {
                ok = false;
                return mev("parse-error", "define form in rebind code has no body");
            }
            new_value = root_v.child(0);
        }

        // Record mutation on the old define node
        std::string summary = (a.size() > 2 && is_string(a[2])) ? ev.string_heap_[as_string_idx(a[2])]
                                                                : "rebind " + name;
        flat.add_mutation(old_define, "rebind", name, summary, summary);

        // Redirect old Define's value child to the new nodes
        // This is a valid NodeId in ev.workspace_flat_ since we parsed into it
        flat.set_child(old_define, 0, new_value);

        // Issue #348: auto-wire kOccurrenceDirty on
        // every (if pred then else) in the new
        // function body. Without this, callers
        // would have to call
        // (compile:mark-narrowing-dirty! ...) manually
        // for each if-context, and the conservative
        // fallback in find_occurrence_contexts would
        // keep firing. The walker iterates only the
        // new value's subtree (the old value's
        // if-contexts are NOT auto-marked here —
        // they're handled by the conservative path
        // because the old body is no longer
        // reachable via the define). For the old
        // value's if-contexts, the post-mutation
        // invariant check (#147) still emits the
        // conservative note, which is the right
        // behavior.
        if (ev.set_occurrence_dirty_fn_) {
            auto_wire_k_occurrence_dirty_for_subtree(
                flat, ev.set_occurrence_dirty_fn_, new_value);
        }

        // ── 依赖图驱动：dirty 所有调用者 ────────────────────────
        // 利用从 def-use 索引预取的调用者列表，标记 dirty + 向上传播。
        // 这样下次 typecheck-current 就知道这些节点需要重新类型推断。
        // Issue #188: rebind changes the function body, so callers
        // need re-inference + re-constraint-solve. Coercion markers
        // are NOT touched (rebinds don't change type annotations),
        // so we use kGeneralDirty | kConstraintDirty (not Coercion).
        // Issue #262: precise def-use dirty on caller entry nodes.
        ev.propagate_defuse_dirty(sym, name, dep_callers,
                                  aura::ast::FlatAST::kGeneralDirty |
                                      aura::ast::FlatAST::kConstraintDirty);

        // Phase 2: mark this define's IR cache entry dirty so the next
        // (eval-current) re-lowers it.
        if (ev.mark_define_dirty_fn_)
            ev.mark_define_dirty_fn_(name);

        // ── Auto-typecheck (Issue #107 part 3.5) ────────────
        // Run the typecheck inline WITHOUT going through the
        // typecheck-current primitive dispatch. Going through the
        // primitive would re-acquire ev.workspace_mtx_ in shared mode
        // and deadlock (we already hold unique_lock from the
        // mutate). The check populates ev.last_mutate_error_ so
        // (typecheck-status) can surface the result.
        if (ev.workspace_flat_ && ev.workspace_pool_) {
            if (!ev.type_registry_) {
                ev.type_registry_ = new aura::core::TypeRegistry();
            }
            auto& treg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
            aura::compiler::TypeChecker tc(treg);
            if (!ev.declared_type_sigs_.empty()) {
                std::unordered_map<std::string, std::string> sig_map;
                std::unordered_map<std::string, std::string> mod_src_map;
                for (auto& [name, decl] : ev.declared_type_sigs_) {
                    sig_map[name] = decl.type_str;
                    if (!decl.module_file.empty())
                        mod_src_map[name] = decl.module_file;
                }
                tc.inject_type_sigs(sig_map, mod_src_map);
            }
            aura::diag::DiagnosticCollector local_diag;
            tc.infer_flat(*ev.workspace_flat_, *ev.workspace_pool_, ev.workspace_flat_->root, local_diag);
            // Issue #116: apply deferred coercions so the
            // upcoming IR generation sees CoercionNodes.
            // (The mutate:rebind path proceeds to eval, so
            //  the workspace must be lowering-ready.)
            {
                auto cm = tc.take_coercions();
                if (!cm.empty()) {
                    aura::compiler::apply_coercion_map(*ev.workspace_flat_, cm);
                }
            }
            ev.workspace_flat_->clear_all_dirty();
            auto local_diags = local_diag.diagnostics();
            if (local_diags.empty()) {
                ev.last_mutate_error_.clear();
            } else {
                std::string err = "typecheck after mutate:rebind failed:";
                for (auto& d : local_diags)
                    err += " " + d.format() + ";";
                ev.last_mutate_error_ = err;
            }
        }

        // ── Ownership validation: ensure ownership invariants hold ──
        if (ev.workspace_flat_ && ev.workspace_pool_ && ev.last_mutate_error_.empty()) {
            std::unordered_set<std::string> affected;
            affected.insert(name);
            // Also include caller names since they were dirtied
            for (std::size_t ui = 0; ui < dep_callers.size(); ++ui) {
                if (dep_callers[ui] < flat.size()) {
                    auto caller_v = flat.get(dep_callers[ui]);
                    if (caller_v.sym_id != aura::ast::INVALID_SYM) {
                        auto caller_name = std::string(ev.workspace_pool_->resolve(caller_v.sym_id));
                        if (!caller_name.empty())
                            affected.insert(caller_name);
                    }
                }
            }
            std::vector<aura::compiler::OwnershipNote> onotes;
            bool opass = aura::compiler::OwnershipEnv::validate_ownership(
                flat, *ev.workspace_pool_, flat.root, affected, onotes);
            if (!opass) {
                std::string err = "ownership validation after mutate:rebind failed:";
                for (auto& n : onotes)
                    err += " [" + n.kind + " at node " + std::to_string(n.node) + "] " + n.message +
                           ";";
                ev.last_mutate_error_ = err;
            }
        }

        // ── Auto-rollback: if typecheck/ownership failed, signal Guard to restore ────
        // Issue #241: panic_checkpoint 的 restore 由 Guard dtor 处理，我们只需要设置 ok=false
        // 当 ev.panic_auto_rollback_ 开启且 mutation 失败。Guard dtor 会调
        // ev.restore_panic_checkpoint()。
        if (!ev.last_mutate_error_.empty() && ev.panic_auto_rollback_) {
            ok = false; // signal Guard's dtor to restore the source
            return mev("mutation-failed",
                       std::string(typeid(ev).name()) +
                           ": mutation rejected — auto-rolled back: " + ev.last_mutate_error_);
        }
        // 失败但 !ev.panic_auto_rollback_：保持 ok=true（保留 pre-#241 行为 — silently return true）
        // Guard dtor 会 ev.commit_panic_checkpoint()（行为变化：以前 leave alone，现在 clear）。
        // 这是 #241 scope-limited close 的已知小变化 — 详见 commit message。

        return make_bool(true);
    });

    // (mutate:set-body name-str new-body-code-str) — Replace function body by name
    // Finds (define (name params) ...) and replaces the Lambda body.
    // Parses new body INTO the workspace FlatAST so all node IDs are valid.
    // Issue #213 follow-up: migrate mutate:set-body to the
    // MutationBoundaryGuard. Same pattern as rebind: guard
    // handles version-bump + lock + planned rollback;
    // save_panic_checkpoint / restore_panic_checkpoint handle
    // panic recovery (separately). The two mechanisms are
    // complementary, not redundant.
    add("mutate:set-body", [&ev, mev](const auto& a) -> EvalValue {
        bool ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
        if (ev.workspace_read_only_) {
            ok = false;
            return mev("read-only", "workspace is read-only");
        }
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]) || !ev.workspace_flat_ ||
            !ev.workspace_pool_) {
            ok = false;
            return mev("bad-arg", "usage: (mutate:set-body name new-body-code [summary])");
        }
        auto name_idx = as_string_idx(a[0]);
        auto code_idx = as_string_idx(a[1]);
        if (name_idx >= ev.string_heap_.size() || code_idx >= ev.string_heap_.size()) {
            ok = false;
            return mev("bad-arg", "string index out of range");
        }
        auto& flat = *ev.workspace_flat_;
        auto name = ev.string_heap_[name_idx];
        // Phase 2.5.0: route via ev.canonical_pool() (== workspace_pool, explicit).
        auto sym = ev.canonical_pool()->intern(name);

        // Issue #352: panic-checkpoint lifecycle now owned by
        // MutationBoundaryGuard (see #241 / #459). The Guard's
        // ctor calls save_panic_checkpoint() and the dtor calls
        // commit/restore based on ok + panic_auto_rollback_.
        // No manual save/commit/restore needed here.

        // ── 依赖图查询：通过 ev.dep_caller_fn_ 获取调用者节点 ────
        auto dep_callers =
            ev.dep_caller_fn_ ? ev.dep_caller_fn_(ev.defuse_index_, sym) : std::vector<aura::ast::NodeId>{};
        ev.defuse_version_.fetch_add(1, std::memory_order_acq_rel);
        ev.total_mutations_.fetch_add(1, std::memory_order_relaxed);

        // Find Define node with matching symbol name
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id == sym) {
                // The Define should have one child: a Lambda
                if (v.children.size() != 1) {
                    ok = false;
                    return mev("arity-error", std::string("function \"") + name + "\" define has " +
                                                  std::to_string(v.children.size()) +
                                                  " children, expected 1");
                }
                auto lambda_id = v.child(0);
                auto lv = flat.get(lambda_id);
                if (lv.tag != aura::ast::NodeTag::Lambda) {
                    ok = false;
                    return mev("type-error",
                               std::string("function \"") + name + "\" body is not a Lambda node");
                }

                // Parse new body INTO workspace flat (all IDs stay valid)
                auto pr =
                    aura::parser::parse_to_flat(ev.string_heap_[code_idx], flat, *ev.workspace_pool_);
                if (!pr.success || pr.root == aura::ast::NULL_NODE) {
                    std::string parse_err;
                    if (!pr.errors.empty()) {
                        for (auto& e : pr.errors) {
                            if (!parse_err.empty())
                                parse_err += "; ";
                            parse_err += e.format();
                        }
                    } else if (!pr.error.empty()) {
                        parse_err = pr.error;
                    } else {
                        parse_err = "new body code could not be parsed";
                    }
                    ok = false;
                    return mev("parse-error", parse_err);
                }

                // Record mutation
                flat.add_mutation(id, "set-body", name, name, "set-body " + name);

                // Issue #230 / #166 semantics: branch on the
                // shape of the new code.
                //
                // - If the new code parses to a Lambda (e.g. user
                //   supplied `(lambda (x) (+ x 100))`), replace
                //   the Define's lambda child entirely so the
                //   function gains the new params/body. This is
                //   what test_issue_166 expects: after
                //   (mutate:set-body "g" "(lambda (x) (+ x 100))")
                //   calling (g 10) returns 110.
                //
                // - If the new code is a body expression (e.g.
                //   `(+ x 2)`, `(* x y)`, `(* x 2)`), replace just
                //   the existing lambda's body slot. This is what
                //   test_regression expects: after
                //   (mutate:set-body "f" "(+ x 2)") the typecheck
                //   status is ok and the function body is now
                //   `(+ x 2)` (with the original params).
                //
                // The old single-branch behavior of "always
                // replace body[0]" returned a nested lambda as a
                // value rather than calling it (test_166 bug);
                // the previous fix of "always replace Define
                // child" broke the body-expression case (3
                // test_regression failures).
                {
                    auto pr_root_v = flat.get(pr.root);
                    if (pr_root_v.tag == aura::ast::NodeTag::Lambda) {
                        flat.set_child(id, 0, pr.root);
                    } else if (lambda_id < flat.size()) {
                        flat.set_child(lambda_id, 0, pr.root);
                    } else {
                        // No existing lambda; treat as
                        // "replace whole Define child" for
                        // consistency with the Lambda branch.
                        flat.set_child(id, 0, pr.root);
                    }
                }

                // 依赖图驱动：dirty 所有调用者
                // Issue #188: set-body changes the function body
                // (same as rebind but via the Lambda node instead
                // of the Define node). Mark callers with
                // kGeneralDirty | kConstraintDirty. No occurrence/
                // ownership bits — body change doesn't add new
                // narrowing or affect Linear state directly.
                // Issue #262: precise def-use dirty on caller entry nodes.
                ev.propagate_defuse_dirty(sym, name, dep_callers,
                                          aura::ast::FlatAST::kGeneralDirty |
                                              aura::ast::FlatAST::kConstraintDirty);

                // ── Auto-typecheck (Issue #107 part 3.5) ────────────
                // Run the typecheck inline WITHOUT going through the
                // typecheck-current primitive dispatch. Same deadlock
                // avoidance as mutate:rebind. Populates
                // ev.last_mutate_error_ so (typecheck-status) works.
                if (ev.workspace_flat_ && ev.workspace_pool_) {
                    if (!ev.type_registry_) {
                        ev.type_registry_ = new aura::core::TypeRegistry();
                    }
                    auto& treg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
                    aura::compiler::TypeChecker tc(treg);
                    if (!ev.declared_type_sigs_.empty()) {
                        std::unordered_map<std::string, std::string> sig_map;
                        std::unordered_map<std::string, std::string> mod_src_map;
                        for (auto& [name2, decl] : ev.declared_type_sigs_) {
                            sig_map[name2] = decl.type_str;
                            if (!decl.module_file.empty())
                                mod_src_map[name2] = decl.module_file;
                        }
                        tc.inject_type_sigs(sig_map, mod_src_map);
                    }
                    aura::diag::DiagnosticCollector local_diag;
                    tc.infer_flat(*ev.workspace_flat_, *ev.workspace_pool_, ev.workspace_flat_->root,
                                  local_diag);
                    // Issue #116: apply deferred coercions before eval
                    // (the set-body path proceeds to re-execute the workspace).
                    {
                        auto cm = tc.take_coercions();
                        if (!cm.empty()) {
                            aura::compiler::apply_coercion_map(*ev.workspace_flat_, cm);
                        }
                    }
                    ev.workspace_flat_->clear_all_dirty();
                    auto local_diags = local_diag.diagnostics();
                    if (local_diags.empty()) {
                        ev.last_mutate_error_.clear();
                    } else {
                        std::string err = "typecheck after mutate:set-body failed:";
                        for (auto& d : local_diags)
                            err += " " + d.format() + ";";
                        ev.last_mutate_error_ = err;
                    }
                }

                // ── Ownership validation ──
                if (ev.workspace_flat_ && ev.workspace_pool_ && ev.last_mutate_error_.empty()) {
                    std::unordered_set<std::string> affected;
                    affected.insert(name);
                    for (std::size_t ui = 0; ui < dep_callers.size(); ++ui) {
                        if (dep_callers[ui] < flat.size()) {
                            auto caller_v = flat.get(dep_callers[ui]);
                            if (caller_v.sym_id != aura::ast::INVALID_SYM) {
                                auto caller_name =
                                    std::string(ev.workspace_pool_->resolve(caller_v.sym_id));
                                if (!caller_name.empty())
                                    affected.insert(caller_name);
                            }
                        }
                    }
                    std::vector<aura::compiler::OwnershipNote> onotes;
                    bool opass = aura::compiler::OwnershipEnv::validate_ownership(
                        flat, *ev.workspace_pool_, flat.root, affected, onotes);
                    if (!opass) {
                        std::string err = "ownership validation after mutate:set-body failed:";
                        for (auto& n : onotes)
                            err += " [" + n.kind + " at node " + std::to_string(n.node) + "] " +
                                   n.message + ";";
                        ev.last_mutate_error_ = err;
                    }
                }

                // Issue #352: auto-rollback now owned by the Guard.
                // On mutation failure, set ok = false; the Guard's
                // dtor will call restore_panic_checkpoint() iff
                // panic_auto_rollback_ is true. On success, leave
                // ok = true; the Guard's dtor commits. The return
                // value is still derived from last_mutate_error_ —
                // we report the failure reason to the caller, but
                // the state machine (rollback vs commit) is the
                // Guard's responsibility, not this primitive's.
                if (!ev.last_mutate_error_.empty()) {
                    ok = false;
                    return mev("mutation-failed",
                               "mutation rejected: " + ev.last_mutate_error_);
                }

                return make_bool(true);
            }
        }
        ok = false;
        return mev("not-found", std::string("function \"") + name + "\" not found in AST");
    });

    // (mutate:remove-node node-id) — Remove a node by setting parent's reference to NULL_NODE
    // The node entry remains in the FlatAST but is disconnected from the tree.
    // The tree walker in eval_flat skips NULL_NODE children.
    // Issue #213 Cycle 2: migrate mutate:remove-node to use
    // the MutationBoundaryGuard (RAII). The guard handles:
    //   - exclusive workspace write lock (replaces std::unique_lock)
    //   - ev.defuse_version_ bump on enter
    //   - ev.defuse_version_ bump + rollback on exit(success=false)
    //   - checkpoint push/pop (for the rollback machinery)
    // The primitive now:
    //   - declares `bool ok = true` and passes &ok to the guard
    //   - sets `ok = false` on every error-return path
    //   - the early `make_merr` returns stay but the success flag
    //     is set so the guard's destructor triggers rollback
    //   - the manual `ev.defuse_version_.fetch_add(1)` for the
    //     second bump is removed (the guard does it on rollback
    //     only; on success the version advance from enter stays)
    add("mutate:remove-node", [&ev, mev](const auto& a) -> EvalValue {
        bool ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
        aura::messaging::g_fiber_yield_mutation_boundary
            ? aura::messaging::g_fiber_yield_mutation_boundary()
            : (void)0; // safe point before mutation
        if (ev.workspace_read_only_) {
            ok = false;
            return mev("read-only", "workspace is read-only");
        }
        if (a.empty() || !is_int(a[0]) || !ev.workspace_flat_) {
            ok = false;
            return mev("bad-arg", "usage: (mutate:remove-node node-id)");
        }
        auto target = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ev.workspace_flat_;
        if (target >= flat.size()) {
            ok = false;
            return mev("out-of-range", "node ID " + std::to_string(target) + " >= flat size " +
                                           std::to_string(flat.size()));
        }

        // Phase 4 follow-up #3b: the wrapper still does the
        // parent-walk (this primitive takes a target id, not
        // a parent+index, so it doesn't map cleanly to a single
        // RemoveChildMutator call). Once the parent + child index
        // are found, the actual remove_child + mark_dirty_upward
        // delegates to the strategy. Error propagation flows
        // through AuraResult<NodeId>.
        bool removed = false;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.children.empty())
                continue;
            const auto& children = flat.children(id);
            for (std::size_t ci = 0; ci < children.size(); ++ci) {
                if (children[ci] != target)
                    continue;
                // Found target at parent=id, child index=ci.
                // Route the structural mutation through RemoveChildMutator.
                auto result = aura::ast::mutators::apply_mutation(
                    flat, id,
                    aura::ast::mutators::RemoveChildMutator{
                        static_cast<std::uint32_t>(ci)});
                if (!result) {
                    ok = false;
                    return mev("mutation-error", std::string(result.error().message));
                }
                flat.add_mutation(id, "remove-node",
                                  std::to_string(target), "",
                                  "remove node " + std::to_string(target));
                removed = true;
                break;
            }
            if (removed) break;
        }
        if (!removed) {
            ok = false;
            return mev("not-found",
                       "node " + std::to_string(target) + " has no parent in the AST");
        }
        return make_bool(true);
    });

    // (mutate:insert-child parent-id position code-string "summary")
    // Insert a child node into a parent's children list at the given position.
    // Position 0 = first child, child_count = append at end.
    // Parses code-string INTO workspace, preserving all existing nodes/IDs.
    //
    // Phase 4 follow-up #3a: the structural mutation is now
    // routed through aura::ast::mutators::InsertChildMutator.
    // The wrapper keeps the Aura-specific boilerplate (mutation
    // boundary guard, fiber yield, arg parsing, code-string
    // parsing, mutation-log entry) and delegates the actual
    // insert_child + mark_dirty_upward to the strategy. Error
    // propagation flows through AuraResult<NodeId> for uniform
    // handling.
    add("mutate:insert-child", [&ev, mev](const auto& a) -> EvalValue {
        bool ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok, /*fine_rollback=*/true);
        aura::messaging::g_fiber_yield_mutation_boundary
            ? aura::messaging::g_fiber_yield_mutation_boundary()
            : (void)0; // safe point before mutation
        if (ev.workspace_read_only_) {
            ok = false;
            return mev("read-only", "workspace is read-only");
        }
        if (a.size() < 3 || !is_int(a[0]) || !is_int(a[1]) || !is_string(a[2]) ||
            !ev.workspace_flat_ || !ev.workspace_pool_) {
            ok = false;
            return mev("bad-arg",
                       "usage: (mutate:insert-child parent-id position code-string [summary])");
        }
        auto parent = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto pos = static_cast<std::uint32_t>(as_int(a[1]));
        auto code_idx = as_string_idx(a[2]);
        if (code_idx >= ev.string_heap_.size()) {
            ok = false;
            return mev("bad-arg", "code string index out of range");
        }
        auto& flat = *ev.workspace_flat_;

        // Parse child code INTO workspace (append mode — all IDs stay valid)
        auto pr = aura::parser::parse_to_flat(ev.string_heap_[code_idx], flat, *ev.workspace_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::string parse_err;
            if (!pr.errors.empty()) {
                for (auto& e : pr.errors) {
                    if (!parse_err.empty())
                        parse_err += "; ";
                    parse_err += e.format();
                }
            } else if (!pr.error.empty()) {
                parse_err = pr.error;
            } else {
                parse_err = "insert-child code could not be parsed";
            }
            ok = false;
            return mev("parse-error", parse_err);
        }

        // Phase 4: route the structural mutation through the
        // InsertChildMutator strategy. The strategy validates the
        // target id (gen-aware), calls flat.insert_child, and
        // marks dirty upward. AuraResult carries errors uniformly.
        auto result = aura::ast::mutators::apply_mutation(
            flat, parent,
            aura::ast::mutators::InsertChildMutator{pos, pr.root});
        if (!result) {
            ok = false;
            // Map AuraErrorKind -> Aura error tag for backward compat.
            std::string tag = "mutation-error";
            if (result.error().kind == aura::core::AuraErrorKind::MutationInvalidTarget) {
                tag = "out-of-range";
            } else if (result.error().kind == aura::core::AuraErrorKind::MutationInvalidParent) {
                tag = "out-of-range";
            }
            return mev(tag, std::string(result.error().message));
        }

        // Log to workspace mutation log (Aura-specific, stays in wrapper).
        std::string summary = (a.size() > 3 && is_string(a[3]))
                                  ? ev.string_heap_[as_string_idx(a[3])]
                                  : "insert child at " + std::to_string(pos);
        flat.add_mutation(parent, "insert-child", std::to_string(pos), summary, summary);

        return make_int(static_cast<std::int64_t>(pr.root));
    });

    // Issue #213 Cycle 2: migrate mutate:tweak-literal to
    // use the MutationBoundaryGuard. This primitive already
    // uses add_mutation_with_rollback with the right field
    // (int_val_/0) — so the rollback path (when triggered by
    // the guard with success=false) actually restores the
    // original int value. The other simple primitives
    // (remove-node, insert-child) don't have the data to
    // restore (they mutate children_ which is a SoA column
    // that the rollback switch doesn't cover); for them, the
    // rollback path bumps the version and invalidates the
    // defuse index, which is enough to surface "the workspace
    // state changed" to readers.
    add("mutate:tweak-literal", [&ev, mev](const auto& a) -> EvalValue {
        bool ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
        aura::messaging::g_fiber_yield_mutation_boundary
            ? aura::messaging::g_fiber_yield_mutation_boundary()
            : (void)0; // safe point before mutation
        if (ev.workspace_read_only_) {
            ok = false;
            return mev("read-only", "workspace is read-only");
        }
        if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1]) || !ev.workspace_flat_) {
            ok = false;
            return mev("bad-arg", "usage: (mutate:tweak-literal node-id delta [summary])");
        }
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto delta = as_int(a[1]);
        auto& flat = *ev.workspace_flat_;
        if (node >= flat.size()) {
            ok = false;
            return mev("out-of-range", "node ID " + std::to_string(node) + " >= flat size " +
                                           std::to_string(flat.size()));
        }
        auto v = flat.get(node);
        if (v.tag != aura::ast::NodeTag::LiteralInt) {
            ok = false;
            return mev("type-error", "node " + std::to_string(node) + " is not a LiteralInt");
        }
        auto new_val = std::max<std::int64_t>(0, static_cast<std::int64_t>(v.int_value) + delta);
        auto old_val = v.int_value;
        std::string summary =
            (a.size() > 2 && is_string(a[2]))
                ? ev.string_heap_[as_string_idx(a[2])]
                : "tweak-literal " + std::to_string(old_val) + "->" + std::to_string(new_val);
        flat.add_mutation_with_rollback(
            node, "tweak-literal", "Int", "Int", summary, aura::ast::MutationStatus::Committed, 0,
            static_cast<std::uint64_t>(old_val), static_cast<std::uint64_t>(new_val), true);
        flat.set_int(node, new_val);
        ev.workspace_flat_->mark_dirty_upward(node);
        return make_int(static_cast<std::int64_t>(new_val));
    });

    // (mutate:replace-pattern pattern replacement [summary])
    //   → #t/#f
    //   Finds all nodes matching a structural pattern and replaces them
    //   with the replacement template.
    //
    //   Pattern syntax:
    //     (\* 2 x)     — exact match: Call(Int(2), Var(x))
    //     (/ ... ...)   — "..." wildcard matches any single subtree
    //
    //   Replacement is a string. When the pattern has wildcards "...",
    //   each occurrence in the replacement is substituted with the
    //   source-code string of the captured subtree.
    //
    //   Example:
    //     (mutate:replace-pattern "(* 2 x)" "(+ x x)")
    //       → replaces (* 2 x) with (+ x x) everywhere
    //     (mutate:replace-pattern "(... (+ ... ...))" "...")
    //       → strips outer call, keeps only the first child
    // Issue #213 Cycle 2: migrate mutate:replace-pattern to
    // use the MutationBoundaryGuard. The primitive mutates
    // the children_ SoA column (call->set_child) for each
    // matched node — the rollback path doesn't restore the
    // original children (the SoA column isn't in the rollback
    // switch), but it does bump the version + invalidate the
    // defuse index, so readers know the workspace state changed.
    add("mutate:replace-pattern", [&ev, mev](const auto& a) -> EvalValue {
        bool ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
        using namespace aura::ast;
        aura::messaging::g_fiber_yield_mutation_boundary
            ? aura::messaging::g_fiber_yield_mutation_boundary()
            : (void)0; // safe point before mutation
        if (ev.workspace_read_only_) {
            ok = false;
            return mev("read-only", "workspace is read-only");
        }
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]) || !ev.workspace_flat_ ||
            !ev.workspace_pool_) {
            ok = false;
            return mev("bad-arg", "usage: (mutate:replace-pattern pattern replacement)");
        }
        auto pattern_idx = as_string_idx(a[0]);
        auto repl_idx = as_string_idx(a[1]);
        if (pattern_idx >= ev.string_heap_.size() || repl_idx >= ev.string_heap_.size()) {
            ok = false;
            return mev("bad-arg", "string index out of range");
        }
        auto& flat = *ev.workspace_flat_;

        auto pattern_str = ev.string_heap_[pattern_idx];
        std::string repl_template = ev.string_heap_[repl_idx];
        // Issue #482: optional `:nested-arity [#t|#f]` keyword +
        // summary string. Scan from a[2] to a.back(), collecting
        // keywords; the LAST string arg becomes the summary. Default
        // :nested-arity is #f (strict) to preserve pre-#482 mutation
        // semantics — mutation as a primitive is fundamentally an
        // atomic replacement, and Kleene mutation semantics
        // (expanding `...` to multiple captures) are not yet defined.
        bool nested_arity = false;
        std::string summary = "replace-pattern";
        for (std::size_t ai = 2; ai < a.size(); ++ai) {
            if (is_keyword(a[ai])) {
                auto kidx = as_keyword_idx(a[ai]);
                if (kidx >= ev.keyword_table_.size()) {
                    ok = false;
                    return mev("bad-arg", "unknown keyword");
                }
                auto kw = ev.keyword_table_[kidx];
                if (kw == ":nested-arity") {
                    nested_arity = true;
                    if (ai + 1 < a.size() && (is_bool(a[ai + 1]) || is_int(a[ai + 1]))) {
                        if (is_bool(a[ai + 1]))
                            nested_arity = as_bool(a[ai + 1]);
                        else
                            nested_arity = (as_int(a[ai + 1]) != 0);
                        ++ai;
                    }
                } else {
                    ok = false;
                    return mev("bad-arg",
                               std::string("unknown mutate:replace-pattern keyword: ") + kw);
                }
            } else if (is_string(a[ai])) {
                summary = ev.string_heap_[as_string_idx(a[ai])];
            } else {
                ok = false;
                return mev("bad-arg",
                           "usage: (mutate:replace-pattern pattern replacement"
                           " [:nested-arity [#t]] [summary-string])");
            }
        }

        // Phase 2.5.0: pat_pool stays separate from canonical_pool.
        // Same rationale as the query:pattern site above — pattern AST
        // is parsed fresh per call (ev.temp_arena_ reclaims) and the
        // wildcard "..." sym lives in pat_pool for in-pattern comparison.
        // Parse pattern into separate FlatAST.
        // Use ev.temp_arena_ so (gc-temp) reclaims it per call.
        auto alloc = ev.temp_arena_->allocator();
        auto* pat_pool = ev.temp_arena_->create<aura::ast::StringPool>(alloc);
        auto* pat_flat = ev.temp_arena_->create<aura::ast::FlatAST>(alloc);
        auto pat_pr = aura::parser::parse_to_flat(pattern_str, *pat_flat, *pat_pool);
        if (!pat_pr.success || pat_pr.root == NULL_NODE) {
            ok = false;
            return mev("parse-error", "pattern string could not be parsed");
        }

        auto wildcard_sym = pat_pool->intern("...");

        // ── Source-code reconstruction helper ─────────────────
        // Given a node ID in the workspace FlatAST, reconstruct its source
        // code as a string (same as current-source but for any node)
        std::function<std::string(NodeId)> node_to_source;
        node_to_source = [&](NodeId id) -> std::string {
            if (id >= flat.size() || id == NULL_NODE)
                return "";
            auto v = flat.get(id);
            switch (v.tag) {
                case NodeTag::LiteralInt:
                    return std::to_string(v.int_value);
                case NodeTag::LiteralFloat:
                    return std::to_string(v.float_value);
                case NodeTag::LiteralString:
                    return "\"" + std::string(ev.workspace_pool_->resolve(v.sym_id)) + "\"";
                case NodeTag::Variable:
                    return std::string(ev.workspace_pool_->resolve(v.sym_id));
                case NodeTag::Call: {
                    std::string s = "(";
                    for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                        if (ci > 0)
                            s += " ";
                        s += node_to_source(v.child(ci));
                    }
                    s += ")";
                    return s;
                }
                case NodeTag::Lambda: {
                    std::string s = "(lambda (";
                    for (std::size_t pi = 0; pi < v.params.size(); ++pi) {
                        if (pi > 0)
                            s += " ";
                        s += pat_pool->resolve(v.params[pi]);
                    }
                    s += ")";
                    if (!v.children.empty())
                        s += " " + node_to_source(v.child(0));
                    s += ")";
                    return s;
                }
                case NodeTag::IfExpr: {
                    std::string s = "(if";
                    if (v.children.size() > 0)
                        s += " " + node_to_source(v.child(0));
                    if (v.children.size() > 1)
                        s += " " + node_to_source(v.child(1));
                    if (v.children.size() > 2)
                        s += " " + node_to_source(v.child(2));
                    s += ")";
                    return s;
                }
                case NodeTag::Begin: {
                    std::string s = "(begin";
                    for (auto c : v.children)
                        s += " " + node_to_source(c);
                    s += ")";
                    return s;
                }
                case NodeTag::Define: {
                    std::string s = "(define " + std::string(ev.workspace_pool_->resolve(v.sym_id));
                    if (!v.children.empty()) {
                        auto val_node = flat.get(v.child(0));
                        if (val_node.tag == NodeTag::Lambda) {
                            s += " (";
                            for (std::size_t pi = 0; pi < val_node.params.size(); ++pi) {
                                if (pi > 0)
                                    s += " ";
                                s += ev.workspace_pool_->resolve(val_node.params[pi]);
                            }
                            s += ")";
                            if (!val_node.children.empty())
                                s += " " + node_to_source(val_node.child(0));
                            s += ")";
                        } else {
                            s += " " + node_to_source(v.child(0));
                        }
                    }
                    s += ")";
                    return s;
                }
                case NodeTag::Let:
                case NodeTag::LetRec: {
                    std::string kw = (v.tag == NodeTag::LetRec) ? "letrec" : "let";
                    std::string s = "(" + kw;
                    if (v.has_name())
                        s += " " + std::string(ev.workspace_pool_->resolve(v.sym_id));
                    // bindings: (var val) pairs
                    // Not implemented for now — just print children
                    for (auto c : v.children)
                        s += " " + node_to_source(c);
                    s += ")";
                    return s;
                }
                default:
                    return std::string("#<node-") + std::to_string(static_cast<int>(v.tag)) + ">";
            }
        };

        // ── Match + capture ────────────────────────────────────
        // so the two primitives agree on which nodes match a pattern
        // regardless of :nested-arity mode. mutate:replace-pattern
        // defaults to strict (nested_arity = false) for pre-#482
        // semantics; pass :nested-arity #t to opt into Kleene
        // matching (the replacement template still expects 1 capture
        // per `...` for now).
        // Issue #270: query captures StableNodeRef; apply validates
        // each ref before set_child inside an atomic batch.
        struct PatternMatch {
            StableNodeRef match_ref;
            std::vector<StableNodeRef> capture_refs;
        };

        aura::compiler::QueryMatcher matcher(
            &flat, ev.workspace_pool_, pat_flat, pat_pool,
            wildcard_sym, nested_arity);

        // Find all matching nodes in workspace. Snapshot end_id (#111)
        // and capture StableNodeRef per match (#270).
        const auto end_id = flat.size();
        std::vector<PatternMatch> matches;
        matches.reserve(end_id);
        for (NodeId id = 0; id < end_id; ++id) {
            // Issue #482: fresh per-match state, same as query site.
            matcher.state.captures.clear();
            matcher.state.depth = 0;
            if (!matcher.match_subtree(id, pat_pr.root))
                continue;
            PatternMatch pm;
            pm.match_ref = flat.make_ref(id);
            pm.capture_refs.reserve(matcher.state.captures.size());
            for (auto& kv : matcher.state.captures)
                pm.capture_refs.push_back(flat.make_ref(kv.second));
            matches.push_back(std::move(pm));
        }

        if (matches.empty())
            return mev("not-found", "pattern did not match any node in the AST");


        // Count wildcards in pattern
        std::function<int(NodeId)> count_wildcards;
        count_wildcards = [&](NodeId pat_id) -> int {
            if (pat_id >= pat_flat->size() || pat_id == NULL_NODE)
                return 0;
            auto pn = pat_flat->get(pat_id);
            if (pn.tag == NodeTag::Variable && pn.sym_id == wildcard_sym)
                return 1;
            int total = 0;
            for (auto c : pn.children)
                total += count_wildcards(c);
            return total;
        };
        int expected_captures = count_wildcards(pat_pr.root);

        // ── Apply replacements via string substitution ────────
        int replaced_count = 0;
        flat.begin_atomic_batch();
        for (auto& match : matches) {
            if (!stable_match_still_attached(flat, match.match_ref))
                continue;
            // Issue #482: in strict mode, capture count must equal
            // the number of `...` wildcards in the source pattern
            // (each consumes exactly 1 child). In Kleene mode the
            // count varies per match (the wildcard consumes 1+
            // children), so we don't enforce strict equality —
            // excess captures are ignored, missing captures leave
            // `...` placeholders in the substitution.
            if (!nested_arity &&
                static_cast<int>(match.capture_refs.size()) != expected_captures)
                continue;
            bool captures_ok = true;
            for (auto& cap_ref : match.capture_refs) {
                if (!cap_ref.is_valid_in(flat)) {
                    captures_ok = false;
                    break;
                }
            }
            if (!captures_ok)
                continue;

            auto match_id = match.match_ref.id;
            auto child_idx_opt = parent_child_index_if_attached(flat, match_id);
            if (!child_idx_opt)
                continue;
            auto parent_id = flat.parent_of(match_id);

            // Build the replacement string by substituting captures
            std::string filled_repl;
            if (expected_captures == 0) {
                filled_repl = repl_template;
            } else {
                // Replace "..." with source code of each captured node
                int cap_idx = 0;
                std::size_t pos = 0;
                while (pos < repl_template.size()) {
                    auto dot_pos = repl_template.find("...", pos);
                    if (dot_pos == std::string::npos) {
                        filled_repl += repl_template.substr(pos);
                        break;
                    }
                    filled_repl += repl_template.substr(pos, dot_pos - pos);
                    if (cap_idx < static_cast<int>(match.capture_refs.size())) {
                        filled_repl += node_to_source(match.capture_refs[cap_idx].id);
                        cap_idx++;
                    }
                    pos = dot_pos + 3; // skip "..."
                }
            }

            // Parse the filled replacement into workspace
            auto repl_pr = aura::parser::parse_to_flat(filled_repl, flat, *ev.workspace_pool_);
            if (!repl_pr.success || repl_pr.root == NULL_NODE)
                continue;

            // Replace the matched node
            flat.set_child(parent_id, *child_idx_opt, repl_pr.root);
            replaced_count++;
        }

        if (replaced_count == 0) {
            flat.rollback_atomic_batch();
            ok = false;
            return mev("pattern-error",
                       "no replacements were applied (capture mismatch or parse failure)");
        }
        flat.commit_atomic_batch();

        // Issue #484: invalidate the tag_arity_index. The index
        // walker (in query_workspace) now skips orphans (parent_
        // == NULL && id != root) so the OLD replaced children
        // don't pollute query results. The wholesale invalidate
        // here forces the next query to rebuild from scratch
        // (the rebuild also skips orphans via insert_node). The
        // combined fix (slow-path skip + fast-path index insert
        // skip + mutate-time invalidate) ensures queries after
        // mutate:replace-pattern return only live nodes.
        ev.invalidate_tag_arity_index();

        flat.add_mutation(0, "replace-pattern", pattern_str, repl_template, summary);
        return make_bool(true);
    });

    // (mutate:replace-subtree <node-id> <new-code-string> [summary])
    //   → #t on success, or a structured result containing captured vars
    //     (Issue #142 AC: capture detection).
    //   Replaces the subtree rooted at <node-id> with the parsed form of
    //   <new-code-string>. Detects free variables in the new code that
    //   are bound by enclosing scopes (i.e. would be captured after the
    //   replacement) and returns them in the result so the LLM caller
    //   can decide whether to hoist them or accept the capture.
    //
    //   Hygiene (Issue #142): the target node MUST NOT carry
    //   SyntaxMarker::MacroIntroduced. Mutating macro-introduced code
    //   would let user code reach into macro internals and break
    //   hygiene invariants. The primitive returns a "hygiene" error
    //   pair on rejection.
    //
    //   Rollback (Issue #142): records parent_id, child_idx, and
    //   old_subtree_source in the mutation log so the (rollback ...)
    //   primitive can restore the original subtree verbatim.
    // Issue #213 Cycle 2: migrate mutate:replace-subtree to
    // use the MutationBoundaryGuard. The primitive already
    // uses add_mutation_subtree (with has_subtree_rollback=true)
    // — so the rollback path can re-parse the old_subtree_source
    // and re-attach it at (parent_id, child_idx).
    add("mutate:replace-subtree", [&ev, mev](const auto& a) -> EvalValue {
        bool ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
        if (ev.workspace_read_only_) {
            ok = false;
            return mev("read-only", "workspace is read-only");
        }
        // Issue #141: lazy COW trigger (active child workspace still
        // shares parent's flat — clone before mutating).
        if (ev.workspace_tree_) {
            if (!ev.trigger_lazy_cow(ev.workspace_tree_)) {
                ok = false;
                return mev("cow-refused", "COW refused: budget exceeded or read-only");
            }
            void* new_flat = nullptr;
            void* new_pool = nullptr;
            if (ev.refresh_active_flat_pool(ev.workspace_tree_, &new_flat, &new_pool)) {
                ev.workspace_flat_ = static_cast<aura::ast::FlatAST*>(new_flat);
                ev.workspace_pool_ = static_cast<aura::ast::StringPool*>(new_pool);
            }
        }
        using namespace aura::ast;
        if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]) || !ev.workspace_flat_ ||
            !ev.workspace_pool_) {
            ok = false;
            return mev("bad-arg", "usage: (mutate:replace-subtree node-id new-code [summary])");
        }
        auto target = static_cast<NodeId>(as_int(a[0]));
        auto code_idx = as_string_idx(a[1]);
        if (code_idx >= ev.string_heap_.size()) {
            ok = false;
            return mev("bad-arg", "code string index out of range");
        }
        auto& flat = *ev.workspace_flat_;
        if (target == NULL_NODE || target >= flat.size()) {
            ok = false;
            return mev("bad-arg", "node-id out of range");
        }

        // ── Hygiene gate (Issue #142 AC) ─────────────────────
        if (flat.marker(target) == SyntaxMarker::MacroIntroduced) {
            ok = false;
            return mev("hygiene", "cannot mutate macro-introduced node");
        }

        auto new_code = ev.string_heap_[code_idx];
        std::string summary = (a.size() > 2 && is_string(a[2])) ? ev.string_heap_[as_string_idx(a[2])]
                                                                : "replace-subtree";

        // ── Locate parent + child_idx (the slot we're replacing) ──
        auto parent_id = flat.parent_of(target);
        std::uint32_t child_idx = 0;
        bool found_slot = false;
        if (parent_id != NULL_NODE && parent_id < flat.size()) {
            auto pv = flat.get(parent_id);
            for (std::size_t ci = 0; ci < pv.children.size(); ++ci) {
                if (pv.child(ci) == target) {
                    child_idx = static_cast<std::uint32_t>(ci);
                    found_slot = true;
                    break;
                }
            }
        }
        if (!found_slot) {
            ok = false;
            return mev("no-parent", "target subtree has no parent slot to replace");
        }

        // ── Capture old subtree source (for rollback) ────────
        std::string old_source;
        {
            auto v = flat.get(target);
            // Build a tiny source printer using existing patterns.
            // We only need a faithful representation of the old node
            // for rollback; the unparse helper in mutate:replace-pattern
            // is large, so we use a simpler one here.
            std::function<std::string(NodeId)> src;
            src = [&](NodeId id) -> std::string {
                if (id >= flat.size() || id == NULL_NODE)
                    return "";
                auto n = flat.get(id);
                switch (n.tag) {
                    case NodeTag::LiteralInt:
                        return std::to_string(n.int_value);
                    case NodeTag::LiteralFloat:
                        return std::to_string(n.float_value);
                    case NodeTag::LiteralString:
                        return "\"" + std::string(ev.workspace_pool_->resolve(n.sym_id)) + "\"";
                    case NodeTag::Variable:
                        return std::string(ev.workspace_pool_->resolve(n.sym_id));
                    case NodeTag::Call: {
                        std::string s = "(";
                        for (std::size_t ci = 0; ci < n.children.size(); ++ci) {
                            if (ci > 0)
                                s += " ";
                            s += src(n.child(ci));
                        }
                        s += ")";
                        return s;
                    }
                    case NodeTag::Lambda: {
                        std::string s = "(lambda (";
                        for (std::size_t pi = 0; pi < n.params.size(); ++pi) {
                            if (pi > 0)
                                s += " ";
                            s += ev.workspace_pool_->resolve(n.params[pi]);
                        }
                        s += ")";
                        if (!n.children.empty())
                            s += " " + src(n.child(0));
                        s += ")";
                        return s;
                    }
                    case NodeTag::IfExpr: {
                        std::string s = "(if";
                        if (n.children.size() > 0)
                            s += " " + src(n.child(0));
                        if (n.children.size() > 1)
                            s += " " + src(n.child(1));
                        if (n.children.size() > 2)
                            s += " " + src(n.child(2));
                        return s + ")";
                    }
                    case NodeTag::Begin: {
                        std::string s = "(begin";
                        for (auto c : n.children)
                            s += " " + src(c);
                        return s + ")";
                    }
                    case NodeTag::Define: {
                        std::string s =
                            "(define " + std::string(ev.workspace_pool_->resolve(n.sym_id));
                        if (!n.children.empty())
                            s += " " + src(n.child(0));
                        return s + ")";
                    }
                    default:
                        return std::string("#<node-") + std::to_string(static_cast<int>(n.tag)) +
                               ">";
                }
            };
            old_source = src(target);
        }

        // ── Parse the new code into the workspace ─────────────
        auto pr = aura::parser::parse_to_flat(new_code, flat, *ev.workspace_pool_);
        if (!pr.success || pr.root == NULL_NODE)
            return mev("parse-error", "new code could not be parsed");
        // Note: we do NOT set flat.root = pr.root here. The new
        // subtree is attached at the original slot via set_child
        // below. Overwriting root would lose the parent linkage
        // and break eval / current-source.

        // ── Capture detection (Issue #142 AC) ────────────────
        // Free vars in the new subtree that are bound by an enclosing
        // scope (i.e. would be captured) are reported back. We walk
        // the NEW subtree looking for Variable refs, then check each
        // against all bindings in the parent chain (lambda params,
        // let/letrec names) that lie OUTSIDE the new subtree.
        std::vector<SymId> captured;
        {
            // Collect variables bound INSIDE the new subtree
            std::unordered_set<SymId> new_bindings;
            std::function<void(NodeId)> collect_new_bindings;
            collect_new_bindings = [&](NodeId id) {
                if (id >= flat.size() || id == NULL_NODE)
                    return;
                auto n = flat.get(id);
                if (n.tag == NodeTag::Lambda) {
                    for (auto p : n.params)
                        new_bindings.insert(p);
                } else if ((n.tag == NodeTag::Let || n.tag == NodeTag::LetRec) && n.has_name()) {
                    new_bindings.insert(n.sym_id);
                }
                for (auto c : n.children)
                    collect_new_bindings(c);
            };
            collect_new_bindings(pr.root);

            // Collect all bindings in the parent chain (outside the slot)
            std::unordered_set<SymId> outer_bindings;
            std::function<void(NodeId)> collect_outer_bindings;
            collect_outer_bindings = [&](NodeId id) {
                if (id >= flat.size() || id == NULL_NODE)
                    return;
                auto n = flat.get(id);
                if (n.tag == NodeTag::Lambda) {
                    for (auto p : n.params)
                        outer_bindings.insert(p);
                } else if ((n.tag == NodeTag::Let || n.tag == NodeTag::LetRec) && n.has_name()) {
                    outer_bindings.insert(n.sym_id);
                }
                for (auto c : n.children)
                    collect_outer_bindings(c);
            };
            // Walk every node in flat (cheap O(n) since n is the
            // workspace size, not a new traversal). Exclude the new
            // subtree's nodes by tracking an in-subtree set.
            std::unordered_set<NodeId> in_new_subtree;
            std::function<void(NodeId)> mark_new;
            mark_new = [&](NodeId id) {
                if (id >= flat.size() || id == NULL_NODE)
                    return;
                in_new_subtree.insert(id);
                auto n = flat.get(id);
                for (auto c : n.children)
                    mark_new(c);
            };
            mark_new(pr.root);
            for (NodeId id = 0; id < flat.size(); ++id) {
                if (in_new_subtree.count(id))
                    continue;
                auto n = flat.get(id);
                if (n.tag == NodeTag::Lambda) {
                    for (auto p : n.params)
                        outer_bindings.insert(p);
                } else if ((n.tag == NodeTag::Let || n.tag == NodeTag::LetRec) && n.has_name()) {
                    outer_bindings.insert(n.sym_id);
                }
            }

            // Walk the new subtree, find Variable refs that are not
            // bound by the new subtree itself, and check if they ARE
            // bound by an outer scope.
            static const char* builtins[] = {
                "+",          "-",      "*",         "/",       "%",       "=",       "<",
                ">",          "<=",     ">=",        "display", "newline", "print",   "read",
                "car",        "cdr",    "cons",      "pair?",   "null?",   "list",    "eq?",
                "eqv?",       "equal?", "not",       "and",     "or",      "if",      "cond",
                "lambda",     "define", "let",       "letrec",  "begin",   "set!",    "apply",
                "map",        "filter", "foldl",     "foldr",   "string?", "number?", "symbol?",
                "procedure?", "void",   "make-void", "error",   "assert",  "true",    "false",
                "quote",
            };
            std::unordered_set<SymId> builtin_syms;
            for (auto b : builtins)
                builtin_syms.insert(ev.workspace_pool_->intern(b));

            std::function<void(NodeId)> find_captured;
            find_captured = [&](NodeId id) {
                if (id >= flat.size() || id == NULL_NODE)
                    return;
                auto n = flat.get(id);
                if (n.tag == NodeTag::Variable) {
                    if (builtin_syms.count(n.sym_id))
                        return;
                    if (new_bindings.count(n.sym_id))
                        return;
                    if (outer_bindings.count(n.sym_id)) {
                        if (std::find(captured.begin(), captured.end(), n.sym_id) == captured.end())
                            captured.push_back(n.sym_id);
                    }
                }
                for (auto c : n.children)
                    find_captured(c);
            };
            find_captured(pr.root);
        }

        // ── Replace target in parent's children list ──────────
        flat.set_child(parent_id, child_idx, pr.root);
        flat.mark_dirty_upward(parent_id);

        // ── Record mutation with subtree rollback data ───────
        flat.add_mutation_subtree(pr.root, parent_id, child_idx, old_source, "replace-subtree",
                                  summary);

        // ── Return value: #t on success, or a captured-vars list
        //    so LLM callers can see what was implicitly captured.
        if (captured.empty()) {
            return make_bool(true);
        }
        // Build ("captured" ("var1") ("var2") ...) result
        EvalValue result = make_void();
        for (auto it = captured.rbegin(); it != captured.rend(); ++it) {
            auto nm = ev.workspace_pool_->resolve(*it);
            auto ni = ev.string_heap_.size();
            ev.string_heap_.push_back(std::string(nm));
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(ni), result});
            result = make_pair(pid);
        }
        // Wrap in (captured ...) so the LLM can pattern-match on it.
        auto cap_idx = ev.string_heap_.size();
        ev.string_heap_.push_back("captured");
        auto wrap = ev.pairs_.size();
        ev.pairs_.push_back({make_string(cap_idx), result});
        return make_pair(wrap);
    });
    // (mutate:atomic-batch) — Issue #192 (P0): apply a list of
    // mutate operations atomically (all-or-nothing). The list is
    // a sequence of sub-lists, each sub-list is (op-name arg1 arg2 ...)
    // — same shape as if you'd called the individual (mutate:rebind ...)
    // / (mutate:remove-node ...) / etc. primitives directly. On any
    // failure mid-batch, all mutations since the batch started are
    // rolled back via FlatAST::rollback_since. The result is #t on
    // full success, #f on rollback.
    //
    // Example:
    //   (mutate:atomic-batch
    //      (list
    //         (list "mutate:rebind" "f" "(lambda (x) (* x 2))" "test")
    //         (list "mutate:rebind" "g" "(lambda (x) (* x 3))" "test"))
    //      "double rebind")
    //
    // Implementation note: each op's primitive acquires its own lock
    // (ev.workspace_mtx_ unique_lock). This is "weak atomicity" —
    // protected from concurrent batches, but if another fiber
    // mutates in between two batched ops, that mutation would also
    // be in the log and would be rolled back if a later op in the
    // batch fails. Strong atomicity (hold the lock the whole time,
    // dispatch via lockless helpers) is a follow-up.
    // Issue #213 follow-up: migrate mutate:atomic-batch to
    // the MutationBoundaryGuard. The atomic-batch already
    // has its own rollback mechanism (rollback_since(
    // initial_log_size) on error) — this stays as-is. The
    // guard adds the lock + version-bump + ev.defuse_index_
    // invalidation that the legacy code did manually. The
    // two rollback paths are complementary:
    //   - The guard's `ok` flag (set when the batch fails)
    //     bumps ev.defuse_version_ + invalidates ev.defuse_index_
    //     on exit(false), so readers holding the pre-batch
    //     snapshot see a mismatch.
    //   - The batch's internal `ok` flag (set when any
    //     sub-primitive fails) triggers rollback_since(
    //     initial_log_size) which actually undoes the
    //     per-op mutations recorded in the log.
    add("mutate:atomic-batch", [&ev, mev](const auto& a) -> EvalValue {
        bool guard_ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &guard_ok);
        // Allow a.size() == 1 with the ops list being empty (void)
        // — that's a vacuous success (no ops, no rollback needed).
        // The summary string is optional in that case.
        if (a.size() < 1) {
            guard_ok = false;
            return mev("bad-arg", "usage: (mutate:atomic-batch (list ...) \"summary\")");
        }
        // a[0] is the ops list. It can be an empty list (void)
        // or a non-empty proper list. Anything else is bad-arg.
        EvalValue op_list = a[0];
        if (!is_pair(op_list) && !is_void(op_list)) {
            guard_ok = false;
            return mev("bad-arg", "ops list must be a list (use (list) for empty)");
        }
        if (!ev.workspace_flat_) {
            guard_ok = false;
            return mev("no-workspace", "no FlatAST available");
        }
        std::uint64_t initial_log_size = ev.workspace_flat_->all_mutations().size();
        bool ok = true;
        std::size_t op_count = 0;
        auto list_to_vec = [&ev](EvalValue list) -> std::vector<EvalValue> {
            std::vector<EvalValue> out;
            while (is_pair(list)) {
                auto pidx = as_pair_idx(list);
                if (pidx >= ev.pairs_.size())
                    break;
                out.push_back(ev.pairs_[pidx].car);
                list = ev.pairs_[pidx].cdr;
            }
            return out;
        };
        auto pair_car = [&ev](EvalValue v) -> EvalValue { return ev.pairs_[as_pair_idx(v)].car; };
        auto pair_cdr = [&ev](EvalValue v) -> EvalValue { return ev.pairs_[as_pair_idx(v)].cdr; };
        // Issue #250: begin the atomic batch. This sets
        // bump_generation_suppressed_ on the FlatAST, so all
        // per-op structural mutations (via the lockless helpers
        // below) skip their per-op generation bump. The batch
        // commits with a single bump at the end. We track the
        // saved-bumps count via ev.workspace_flat_->atomic_batch_bumps_saved().
        ev.workspace_flat_->begin_atomic_batch();
        while (is_pair(op_list)) {
            EvalValue op = pair_car(op_list);
            op_list = pair_cdr(op_list);
            if (!is_pair(op)) {
                ok = false;
                break;
            }
            EvalValue op_name_ev = pair_car(op);
            if (!is_string(op_name_ev)) {
                ok = false;
                break;
            }
            std::vector<EvalValue> op_args = list_to_vec(pair_cdr(op));
            std::string op_name = ev.string_heap_[as_string_idx(op_name_ev)];
            // Issue #236: route through the lockless helpers
            // instead of ev.primitives_.lookup. The old code
            // re-entered the full primitive (which acquires its
            // own MutationBoundaryGuard), deadlocking on the
            // non-recursive shared_mutex — the batch already
            // holds the lock via its outer guard.
            //
            // Sub-op name mapping:
            //   "mutate:rebind"       -> eval_flat_apply_mutate_rebind
            //   "mutate:replace-value" -> eval_flat_apply_mutate_replace_value
            //   "mutate:tweak-literal" -> eval_flat_apply_mutate_tweak_literal
            // Anything else: unsupported (would need to acquire
            // the lock, which deadlocks; fail fast).
            EvalResult sub_result{types::make_void()};
            if (op_name == "mutate:rebind") {
                sub_result = ev.eval_flat_apply_mutate_rebind(op_args);
            } else if (op_name == "mutate:replace-value") {
                sub_result = ev.eval_flat_apply_mutate_replace_value(op_args);
            } else if (op_name == "mutate:tweak-literal") {
                sub_result = ev.eval_flat_apply_mutate_tweak_literal(op_args);
            } else {
                // For other ops (insert-child / remove-node / etc.)
                // that aren't extracted to lockless helpers yet,
                // error out instead of deadlocking on a nested
                // guard acquire.
                ev.workspace_flat_->rollback_since(initial_log_size);
                ev.workspace_flat_->rollback_atomic_batch();
                ev.atomic_batch_rollbacks_++;
                guard_ok = false;
                return ev.make_merr("batch-unsupported-op",
                                 ("mutate:atomic-batch does not yet support '" + op_name +
                                  "' (only :rebind / :replace-value / :tweak-literal; the others "
                                  "need lockless helper extraction)")
                                     .c_str());
            }
            if (!sub_result) {
                ok = false;
                break;
            }
            // Heuristic: bool-false result from the helper is a failure
            if (types::is_bool(*sub_result) && !types::as_bool(*sub_result)) {
                ok = false;
                break;
            }
            ++op_count;
        }
        if (!ok) {
            ev.workspace_flat_->rollback_since(initial_log_size);
            ev.workspace_flat_->rollback_atomic_batch();
            ev.atomic_batch_rollbacks_++;
            guard_ok = false;
            return make_bool(false);
        }
        // Issue #250: commit the batch. This performs the single
        // generation bump (consolidated from the per-op bumps
        // that were suppressed). Records the saved-bumps count.
        std::uint64_t saved = ev.workspace_flat_->atomic_batch_bumps_saved();
        ev.workspace_flat_->commit_atomic_batch();
        ev.atomic_batch_count_++;
        ev.atomic_batch_ops_total_ += op_count;
        ev.atomic_batch_bumps_saved_total_ += saved;
        return make_bool(true);
    });
    // (mutate:splice parent-id position code-strings... "summary")
    //   → list of inserted node IDs
    //   Parses and inserts multiple child expressions at the given position.
    //   code-strings can be multiple arguments (variadic).
    // Issue #213 Cycle 2: migrate mutate:splice to use the
    // MutationBoundaryGuard. The primitive mutates children_
    // (SoA column not in rollback switch), so the rollback
    // path is "bump version + invalidate ev.defuse_index_".
    add("mutate:splice", [&ev](std::span<const EvalValue> a) -> EvalValue {
        bool ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
        // (Step 0.3) local merr removed; centralized make_merr
        if (ev.workspace_read_only_) {
            ok = false;
            return ev.make_merr("read-only", "workspace is read-only");
        }
        if (a.size() < 3 || !is_int(a[0]) || !is_int(a[1]) || !ev.workspace_flat_ ||
            !ev.workspace_pool_) {
            ok = false;
            return ev.make_merr("bad-arg",
                             "usage: (mutate:splice parent-id position code-strings... [summary])");
        }
        auto parent = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto pos = static_cast<std::uint32_t>(as_int(a[1]));
        auto& flat = *ev.workspace_flat_;
        if (parent >= flat.size()) {
            ok = false;
            return ev.make_merr("out-of-range", "parent node ID " + std::to_string(parent) +
                                                 " >= flat size " + std::to_string(flat.size()));
        }

        // Collect all code strings (variadic) before the optional summary
        std::vector<EvalValue> code_args;
        for (std::size_t i = 2; i < a.size(); ++i) {
            // If the last arg is a string and it's the 4th+ arg, it might be summary
            if (i == a.size() - 1 && i >= 3 && is_string(a[i]))
                continue; // handled as summary below
            if (is_string(a[i]))
                code_args.push_back(a[i]);
        }

        // Summary: last string arg (after parent, position, and at least one code)
        std::string summary = "splice";
        if (a.size() >= 4 && is_string(a[a.size() - 1])) {
            auto sidx = as_string_idx(a[a.size() - 1]);
            if (sidx < ev.string_heap_.size())
                summary = ev.string_heap_[sidx];
        }

        if (code_args.empty()) {
            ok = false;
            return ev.make_merr("bad-arg", "no code strings provided to splice");
        }

        // Parse each code string and insert
        EvalValue result_list = make_void();
        std::uint32_t insert_pos = pos;

        for (auto& code_val : code_args) {
            auto cidx = as_string_idx(code_val);
            if (cidx >= ev.string_heap_.size())
                continue;

            auto pr = aura::parser::parse_to_flat(ev.string_heap_[cidx], flat, *ev.workspace_pool_);
            if (!pr.success || pr.root == aura::ast::NULL_NODE)
                continue;

            flat.insert_child(parent, insert_pos, pr.root);

            flat.add_mutation(parent, "splice", std::to_string(insert_pos), ev.string_heap_[cidx],
                              summary);
            ev.workspace_flat_->mark_dirty_upward(parent);

            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_int(static_cast<std::int64_t>(pr.root)), result_list});
            result_list = make_pair(pid);

            insert_pos++;
        }
        // Reverse result list to match insertion order
        EvalValue reversed = make_void();
        {
            auto cur = result_list;
            while (is_pair(cur)) {
                auto idx = as_pair_idx(cur);
                if (idx >= ev.pairs_.size())
                    break;
                auto ridx = ev.pairs_.size();
                ev.pairs_.push_back({ev.pairs_[idx].car, reversed});
                reversed = make_pair(ridx);
                cur = ev.pairs_[idx].cdr;
            }
        }
        return reversed;
    });

    // (mutate:wrap node-id wrapper-template "summary")
    //   → node ID of the wrapper call (or #f on failure)
    //   Wraps the target node in an expression. The wrapper-template is a
    //   code string with a single `_` placeholder where the target node
    //   will be inserted.
    //   Examples:
    //     (mutate:wrap 5 "(display _)" "wrap in display")
    //       → replaces node 5 with (display <original-node-5>)
    //     (mutate:wrap 3 "(let ((x _)) x)" "bind x")
    //       → wraps in let binding
    // Issue #213 Cycle 2: migrate mutate:wrap to use the
    // MutationBoundaryGuard. Mutates children_ (SoA column
    // not in rollback switch), so the rollback path is
    // "bump version + invalidate ev.defuse_index_".
    add("mutate:wrap", [&ev](std::span<const EvalValue> a) -> EvalValue {
        bool ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
        // local merr removed; now centralized make_merr (phase complete)
        if (ev.workspace_read_only_) {
            ok = false;
            return ev.make_merr("read-only", "workspace is read-only");
        }
        if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]) || !ev.workspace_flat_ ||
            !ev.workspace_pool_) {
            ok = false;
            return ev.make_merr("bad-arg", "usage: (mutate:wrap node-id wrapper-template [summary])");
        }
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto tmpl_idx = as_string_idx(a[1]);
        if (tmpl_idx >= ev.string_heap_.size()) {
            ok = false;
            return ev.make_merr("bad-arg", "template string index out of range");
        }
        auto& flat = *ev.workspace_flat_;
        if (node >= flat.size()) {
            ok = false;
            return ev.make_merr("out-of-range", "node ID " + std::to_string(node) + " >= flat size " +
                                                 std::to_string(flat.size()));
        }

        std::string summary = (a.size() > 2 && is_string(a[2]))
                                  ? ev.string_heap_[as_string_idx(a[2])]
                                  : "wrap node " + std::to_string(node);

        auto tmpl = ev.string_heap_[tmpl_idx];

        // Find the parent of the target node
        aura::ast::NodeId parent_of_target = aura::ast::NULL_NODE;
        int child_idx_in_parent = -1;
        for (aura::ast::NodeId pid = 0; pid < flat.size(); ++pid) {
            auto pv = flat.get(pid);
            for (std::size_t ci = 0; ci < pv.children.size(); ++ci) {
                if (pv.child(ci) == node) {
                    parent_of_target = pid;
                    child_idx_in_parent = static_cast<int>(ci);
                    break;
                }
            }
            if (parent_of_target != aura::ast::NULL_NODE)
                break;
        }

        if (parent_of_target == aura::ast::NULL_NODE || child_idx_in_parent < 0) {
            ok = false;
            return ev.make_merr("no-parent",
                             "node " + std::to_string(node) + " has no parent in the AST");
        }

        // Replace `_` in the template with a unique variable
        std::string sentinel = "__WRAP_TARGET_" + std::to_string(node) + "__";
        auto sentinel_pos = tmpl.find('_');
        if (sentinel_pos == std::string::npos)
            return ev.make_merr("bad-arg", "wrapper-template must contain a '_' placeholder");

        auto parsed_tmpl = tmpl.substr(0, sentinel_pos) + sentinel + tmpl.substr(sentinel_pos + 1);

        // Parse the wrapper into workspace
        auto pr = aura::parser::parse_to_flat(parsed_tmpl, flat, *ev.workspace_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::string parse_err;
            if (!pr.errors.empty()) {
                for (auto& e : pr.errors) {
                    if (!parse_err.empty())
                        parse_err += "; ";
                    parse_err += e.format();
                }
            } else if (!pr.error.empty()) {
                parse_err = pr.error;
            } else {
                parse_err = "wrapper template could not be parsed";
            }
            return ev.make_merr("parse-error", parse_err);
        }

        // Find the sentinel variable and its parent in the parsed AST
        auto sentinel_sym = ev.workspace_pool_->intern(sentinel);
        aura::ast::NodeId sentinel_id = aura::ast::NULL_NODE;
        aura::ast::NodeId sentinel_parent = aura::ast::NULL_NODE;
        int sentinel_child_idx = -1;

        for (aura::ast::NodeId sid = 0; sid < flat.size(); ++sid) {
            auto sv = flat.get(sid);
            if (sv.tag == aura::ast::NodeTag::Variable && sv.sym_id == sentinel_sym) {
                sentinel_id = sid;
                // Find this variable's parent
                for (aura::ast::NodeId p2 = 0; p2 < flat.size(); ++p2) {
                    auto p2v = flat.get(p2);
                    for (std::size_t ci = 0; ci < p2v.children.size(); ++ci) {
                        if (p2v.child(ci) == sid) {
                            sentinel_parent = p2;
                            sentinel_child_idx = static_cast<int>(ci);
                            break;
                        }
                    }
                    if (sentinel_parent != aura::ast::NULL_NODE)
                        break;
                }
                break;
            }
        }

        if (sentinel_id == aura::ast::NULL_NODE || sentinel_parent == aura::ast::NULL_NODE ||
            sentinel_child_idx < 0)
            return ev.make_merr("internal",
                             "sentinel placeholder not found in parsed wrapper template");

        // Replace the sentinel variable in the wrapper with the target node
        flat.set_child(sentinel_parent, static_cast<std::uint32_t>(sentinel_child_idx), node);

        // Replace the original target node's position with the wrapper root
        flat.set_child(parent_of_target, static_cast<std::uint32_t>(child_idx_in_parent), pr.root);

        flat.add_mutation(node, "wrap", parsed_tmpl, summary, summary);
        return make_int(static_cast<std::int64_t>(pr.root));
    });

    // (mutate:refactor/extract node-id new-name "summary")
    //   → (define-node-id . call-node-id)
    //   Extracts the subtree rooted at node-id into a new top-level define,
    //   replacing the original node with a call to the new function.
    //   Free variables in the extracted expression become parameters.
    // Issue #213 Cycle 2: migrate mutate:refactor/extract to
    // use the MutationBoundaryGuard. Mutates the AST (adds
    // a new define + replaces original with a call), but
    // rollback doesn't reverse the AST changes — it just
    // bumps the version + invalidates the defuse index.
    add("mutate:refactor/extract", [&ev](std::span<const EvalValue> a) -> EvalValue {
        bool ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
        // local merr removed; now centralized make_merr (phase complete)
        if (ev.workspace_read_only_) {
            ok = false;
            return ev.make_merr("read-only", "workspace is read-only");
        }
        if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]) || !ev.workspace_flat_ ||
            !ev.workspace_pool_) {
            ok = false;
            return ev.make_merr("bad-arg",
                             "usage: (mutate:refactor/extract node-id new-name [summary])");
        }
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto name_idx = as_string_idx(a[1]);
        if (name_idx >= ev.string_heap_.size()) {
            ok = false;
            return ev.make_merr("bad-arg", "name string index out of range");
        }
        auto& flat = *ev.workspace_flat_;
        if (node >= flat.size()) {
            ok = false;
            return ev.make_merr("out-of-range", "node ID " + std::to_string(node) + " >= flat size " +
                                                 std::to_string(flat.size()));
        }

        auto new_name = ev.string_heap_[name_idx];
        std::string summary = (a.size() > 2 && is_string(a[2])) ? ev.string_heap_[as_string_idx(a[2])]
                                                                : "extract " + new_name;

        // Get the source code of the target node (for parsing)
        auto src_fn = ev.primitives_.lookup("current-source");
        if (!src_fn)
            return ev.make_merr("internal", "current-source primitive not found");

        // Build (define (new-name) <extracted-expr>)
        // First find the lambda params by analyzing free variables...
        // Simplified: extract as (define new-name (lambda () <expr>))
        // then let an Agent fix the parameters later.

        // For now, a minimal implementation:
        // 1. Save the current workspace
        // 2. Get the source of the target subtree
        // 3. Create a new define wrapping the source
        // 4. Parse and insert

        // Actually, simpler: just create the define form as a string
        // and parse it, then replace the original node with a call.
        // But we don't have the source of just the subtree easily.
        //
        // Simplest P0: wrap the expression in a lambda with no args,
        // define it, and replace the original with (new-name).

        // For P0, use the existing mutate:rebind + mutate:wrap pattern
        // 1. Record the original node's parent
        // 2. Create a new define with a dummy body
        // 3. Replace the body with the original expression
        // 4. Replace the original expression with a call to the new function

        // Get the parent of the target
        aura::ast::NodeId parent_of_target = aura::ast::NULL_NODE;
        int child_idx_in_parent = -1;
        for (aura::ast::NodeId pid = 0; pid < flat.size(); ++pid) {
            auto pv = flat.get(pid);
            for (std::size_t ci = 0; ci < pv.children.size(); ++ci) {
                if (pv.child(ci) == node) {
                    parent_of_target = pid;
                    child_idx_in_parent = static_cast<int>(ci);
                    break;
                }
            }
            if (parent_of_target != aura::ast::NULL_NODE)
                break;
        }

        if (parent_of_target == aura::ast::NULL_NODE || child_idx_in_parent < 0) {
            ok = false;
            return ev.make_merr("no-parent",
                             "node " + std::to_string(node) + " has no parent in the AST");
        }

        // Create the new function definition string
        std::string define_str = "(define (" + new_name + " x) x)";
        auto define_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(define_str);

        // Parse the define into workspace
        auto pr = aura::parser::parse_to_flat(define_str, flat, *ev.workspace_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::string parse_err;
            if (!pr.errors.empty()) {
                for (auto& e : pr.errors) {
                    if (!parse_err.empty())
                        parse_err += "; ";
                    parse_err += e.format();
                }
            } else if (!pr.error.empty()) {
                parse_err = pr.error;
            } else {
                parse_err = "extract function definition could not be parsed";
            }
            ok = false;
            return ev.make_merr("parse-error", parse_err);
        }

        // The define's body (the lambda body "x") should be at pr.root's child 0's child 0
        auto define_v = flat.get(pr.root);
        if (define_v.tag != aura::ast::NodeTag::Define || define_v.children.empty())
            return ev.make_merr("internal", "parsed define form has unexpected structure");

        // For simplicity, replace the define body's variable with the extracted node
        auto lambda_id = define_v.child(0);
        auto lambda_v = flat.get(lambda_id);
        if (!lambda_v.children.empty()) {
            auto dummy_body = lambda_v.child(0);
            // Replace dummy body (Variable "x") with the extracted expression
            flat.set_child(lambda_id, 0, node);
            // Remove the extracted node from its original parent
            flat.set_child(parent_of_target, static_cast<std::uint32_t>(child_idx_in_parent),
                           pr.root); // replace with the define's call: (new-name x)
        }

        auto new_fn_sym = ev.workspace_pool_->intern(new_name);
        flat.add_mutation(pr.root, "extract-function", new_name, summary, summary);

        // Return (define-node-id . call-to-restore)
        auto result_pid = ev.pairs_.size();
        ev.pairs_.push_back({make_int(static_cast<std::int64_t>(pr.root)),
                          make_int(static_cast<std::int64_t>(parent_of_target))});
        return make_pair(result_pid);
    });

    // ═══════════════════════════════════════════════════════════════
    // P11: 结构化变异 API — rename / inline / move / fix extract
    // ═══════════════════════════════════════════════════════════════

    // ── Helper: resolve a function body from a call node ──────────
    // Given a Call node where func is a Variable, find the Define node
    // it refers to and return the Define's body. Returns NULL_NODE if not found.
    auto resolve_call_target = [&](const aura::ast::FlatAST& flat, aura::ast::NodeId call_id,
                                   aura::ast::FlatAST* override_flat =
                                       nullptr) -> aura::ast::NodeId {
        using namespace aura::ast;
        auto& f = override_flat ? *override_flat : flat;
        auto cv = f.get(call_id);
        if (cv.tag != NodeTag::Call || cv.children.empty())
            return NULL_NODE;
        auto func_node = cv.child(0);
        auto fv = f.get(func_node);
        if (fv.tag != NodeTag::Variable)
            return NULL_NODE;
        auto sym = fv.sym_id;
        // Find Define with this sym
        for (NodeId id = 0; id < f.size(); ++id) {
            auto v = f.get(id);
            if (v.tag == NodeTag::Define && v.sym_id == sym) {
                if (!v.children.empty())
                    return v.child(0); // body of define
            }
        }
        return NULL_NODE;
    };

    // ── Helper: collect free variables in a subtree ─────────────
    // Returns a list of SymIds for variables used but not defined within the subtree.
    // Scoped bindings (lambda params, let, letrec) are excluded.
    auto collect_free_vars = [&](const aura::ast::FlatAST& flat, aura::ast::NodeId root_id,
                                 aura::ast::StringPool& pool) -> std::vector<aura::ast::SymId> {
        using namespace aura::ast;
        std::vector<SymId> free_vars;
        std::unordered_set<SymId> bound_vars;
        // DFS with scope tracking
        struct Frame {
            NodeId node;
            std::size_t child_idx;
        };
        std::vector<Frame> stack;
        // We need to track scope-introducing nodes and their bound vars.
        // Simple approach: two-pass — first collect all bound vars in the subtree,
        // then find all Variable refs that are not bound.
        // Pass 1: collect bound vars
        {
            std::vector<Frame> pass1;
            pass1.push_back({root_id, 0});
            while (!pass1.empty()) {
                auto& f = pass1.back();
                auto v = flat.get(f.node);
                if (f.child_idx == 0) {
                    // Scope-introducing nodes
                    if (v.tag == NodeTag::Lambda) {
                        for (auto p : v.params)
                            bound_vars.insert(p);
                    } else if (v.tag == NodeTag::Let || v.tag == NodeTag::LetRec) {
                        if (v.has_name())
                            bound_vars.insert(v.sym_id);
                    }
                }
                if (f.child_idx < v.children.size()) {
                    auto c = v.child(f.child_idx);
                    f.child_idx++;
                    if (c != NULL_NODE)
                        pass1.push_back({c, 0});
                } else {
                    pass1.pop_back();
                }
            }
        }
        // Pass 2: find Variable refs not in bound_vars
        {
            std::vector<Frame> pass2;
            pass2.push_back({root_id, 0});
            while (!pass2.empty()) {
                auto& f = pass2.back();
                auto v = flat.get(f.node);
                if (f.child_idx == 0 && f.node != root_id) {
                    // Skip bound vars in inner scopes
                }
                if (v.tag == NodeTag::Variable && f.node != root_id) {
                    // Only collect variables that aren't bound
                    // But we need to respect scope — a variable might be bound
                    // by innermost scope. Use simple approach: if not in bound_vars,
                    // it's free.
                }
                if (f.child_idx < v.children.size()) {
                    auto c = v.child(f.child_idx);
                    f.child_idx++;
                    if (c != NULL_NODE)
                        pass2.push_back({c, 0});
                } else {
                    if (f.child_idx == v.children.size()) {
                        // Post-visit: check if this node is a Variable and not a lambda param
                        if (v.tag == NodeTag::Variable) {
                            if (bound_vars.find(v.sym_id) == bound_vars.end()) {
                                // Not bound — check if already in free_vars
                                if (std::find(free_vars.begin(), free_vars.end(), v.sym_id) ==
                                    free_vars.end())
                                    free_vars.push_back(v.sym_id);
                            }
                        }
                    }
                    pass2.pop_back();
                }
            }
        }
        return free_vars;
    };

    // ── mutate:rename-symbol ────────────────────────────────────
    // (mutate:rename-symbol old-name new-name "summary")
    //   → #t/#f
    //   Renames all definitions and references of old-name to new-name.
    //   Uses def-use index for finding all references.
    // Issue #213 Cycle 2: migrate mutate:rename-symbol to
    // use the MutationBoundaryGuard. The primitive mutates
    // sym_id_ for many nodes at once — too many to log with
    // add_mutation_with_rollback, so we use add_mutation for
    // the summary record. The rollback path is "bump version
    // + invalidate ev.defuse_index_" (no per-node data restoration
    // because there are too many).
    add("mutate:rename-symbol", [&ev](const auto& a) -> EvalValue {
        using namespace aura::ast;
        bool ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok, /*fine_rollback=*/true);
        // local merr removed; now centralized make_merr (phase complete)
        if (ev.workspace_read_only_) {
            ok = false;
            return ev.make_merr("read-only", "workspace is read-only");
        }
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]) || !ev.workspace_flat_ ||
            !ev.workspace_pool_) {
            ok = false;
            return ev.make_merr("bad-arg", "usage: (mutate:rename-symbol old-name new-name)");
        }
        auto old_name_idx = as_string_idx(a[0]);
        auto new_name_idx = as_string_idx(a[1]);
        if (old_name_idx >= ev.string_heap_.size() || new_name_idx >= ev.string_heap_.size())
            return ev.make_merr("bad-arg", "string index out of range");
        auto& flat = *ev.workspace_flat_;
        auto old_name = ev.string_heap_[old_name_idx];
        auto new_name = ev.string_heap_[new_name_idx];
        auto old_sym = ev.workspace_pool_->intern(old_name);
        auto new_sym = ev.workspace_pool_->intern(new_name);

        std::string summary = (a.size() > 2 && is_string(a[2]))
                                  ? ev.string_heap_[as_string_idx(a[2])]
                                  : "rename " + old_name + " → " + new_name;

        // Scan entire AST for nodes with this sym_id (defs + uses)
        int count = 0;
        for (NodeId id = 0; id < flat.size(); ++id) {
            // Check if this id's sym_id matches (and is meaningful)
            if (flat.sym_id(id) == old_sym) {
                auto tag = flat.tag(id);
                // Only rename Variable, Define, DefineType, DefineModule, Let, LetRec, Set
                if (tag == NodeTag::Variable || tag == NodeTag::Define ||
                    tag == NodeTag::DefineType || tag == NodeTag::DefineModule ||
                    tag == NodeTag::Let || tag == NodeTag::LetRec || tag == NodeTag::Set ||
                    tag == NodeTag::MacroDef) {
                    flat.sym_id(id) = new_sym;
                    count++;
                }
            }
        }

        // Issue #139: also rename Lambda params. The Lambda's
        // params live in param_data_ (a separate field from
        // sym_id_), so the sym_id loop above doesn't see them.
        for (NodeId id = 0; id < flat.size(); ++id) {
            if (flat.tag(id) == NodeTag::Lambda) {
                count += flat.rename_param(id, old_sym, new_sym, nullptr);
            }
        }

        if (count == 0) {
            ok = false;
            return ev.make_merr("not-found",
                             std::string("symbol \"") + old_name + "\" not found in AST");
        }

        flat.add_mutation(0, "rename-symbol", old_name, new_name, summary);
        return make_bool(true);
    });

    // ── mutate:move-node ────────────────────────────────────────
    // (mutate:move-node node-id new-parent-id new-position "summary")
    //   → #t/#f
    //   Moves a node (and its subtree) from its current position to
    //   a new parent at the specified child index.
    // Issue #213 Cycle 2: migrate mutate:move-node to use
    // the MutationBoundaryGuard. The primitive mutates the
    // children_ SoA column (which is not in the rollback
    // switch), so the rollback path is "bump version +
    // invalidate ev.defuse_index_" — the actual move is not
    // reversed, but readers know the workspace state changed.
    add("mutate:move-node", [&ev](std::span<const EvalValue> a) -> EvalValue {
        using namespace aura::ast;
        bool ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok, /*fine_rollback=*/true);
        // local merr removed; now centralized make_merr (phase complete)
        if (ev.workspace_read_only_) {
            ok = false;
            return ev.make_merr("read-only", "workspace is read-only");
        }
        if (a.size() < 3 || !is_int(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !ev.workspace_flat_) {
            ok = false;
            return ev.make_merr("bad-arg", "usage: (mutate:move-node node parent pos)");
        }
        auto node = static_cast<NodeId>(as_int(a[0]));
        auto new_parent = static_cast<NodeId>(as_int(a[1]));
        auto new_pos = static_cast<std::uint32_t>(as_int(a[2]));
        auto& flat = *ev.workspace_flat_;

        if (node >= flat.size() || new_parent >= flat.size() || node == NULL_NODE ||
            new_parent == NULL_NODE) {
            ok = false;
            return ev.make_merr("out-of-range", "node or parent ID out of range");
        }

        if (node == new_parent) {
            ok = false;
            return ev.make_merr("cycle", "cannot move node to itself");
        }

        // Check if new_parent is a descendant of node (would create cycle)
        {
            auto p = flat.parent_of(new_parent);
            while (p != NULL_NODE) {
                if (p == node) {
                    ok = false;
                    return ev.make_merr("cycle", "new parent is a descendant of moved node");
                }
                auto next = flat.parent_of(p);
                if (next == p)
                    break;
                p = next;
            }
        }

        auto cur_parent = flat.parent_of(node);
        if (cur_parent == NULL_NODE) {
            ok = false;
            return ev.make_merr("no-parent", "node has no parent (possibly the root)");
        }

        int cur_idx = -1;
        auto cpv = flat.get(cur_parent);
        for (std::size_t ci = 0; ci < cpv.children.size(); ++ci) {
            if (cpv.child(ci) == node) {
                cur_idx = static_cast<int>(ci);
                break;
            }
        }
        if (cur_idx < 0) {
            ok = false;
            return ev.make_merr("inconsistency", "node not found in parent's children list");
        }

        std::string summary = (a.size() > 3 && is_string(a[3]))
                                  ? ev.string_heap_[as_string_idx(a[3])]
                                  : "move node " + std::to_string(node);

        // Remove from current parent (set to NULL_NODE).
        // insert_child will set parent_[node] = new_parent.
        flat.set_child(cur_parent, cur_idx, aura::ast::NULL_NODE);

        // Insert at new parent
        flat.insert_child(new_parent, new_pos, node);

        flat.add_mutation(node, "move-node", std::to_string(cur_parent), std::to_string(new_parent),
                          summary);
        return make_bool(true);
    });

    // ── Fix: mutate:refactor/extract 重写 ──────────────────────
    // (mutate:extract-function node-id new-name "summary")
    //   → (define-node-id . call-node-id)
    //   Extracts a subtree into a new top-level function definition.
    //   Analyzes free variables in the subtree and makes them parameters.
    //   Replaces the original node with a call to the new function.
    add(
        "mutate:extract-function", [&ev, collect_free_vars](const auto& a) -> EvalValue {
            using namespace aura::ast;
            // last local merr definition removed; all calls use centralized make_merr
            ev.defuse_version_.fetch_add(1, std::memory_order_acq_rel);
            ev.total_mutations_.fetch_add(1, std::memory_order_relaxed);
            if (ev.workspace_read_only_)
                return ev.make_merr("read-only", "workspace is read-only");
            if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]) || !ev.workspace_flat_ ||
                !ev.workspace_pool_)
                return ev.make_merr("bad-arg", "usage: (mutate:extract-function node-id name)");
            auto node = static_cast<NodeId>(as_int(a[0]));
            auto name_idx = as_string_idx(a[1]);
            if (name_idx >= ev.string_heap_.size())
                return ev.make_merr("bad-arg", "name string index out of range");
            auto& flat = *ev.workspace_flat_;
            if (node >= flat.size())
                return ev.make_merr("out-of-range", std::to_string(node) + " >= flat size " +
                                                     std::to_string(flat.size()));

            auto new_name = ev.string_heap_[name_idx];
            std::string summary = (a.size() > 2 && is_string(a[2]))
                                      ? ev.string_heap_[as_string_idx(a[2])]
                                      : "extract " + new_name;

            // Find parent of target node using parent_ vector
            auto parent_of_target = flat.parent_of(node);
            if (parent_of_target == NULL_NODE)
                return ev.make_merr("no-parent", "extracted node has no parent");

            // Find child index in parent
            int child_idx_in_parent = -1;
            auto pv = flat.get(parent_of_target);
            for (std::size_t ci = 0; ci < pv.children.size(); ++ci) {
                if (pv.child(ci) == node) {
                    child_idx_in_parent = static_cast<int>(ci);
                    break;
                }
            }
            if (child_idx_in_parent < 0)
                return ev.make_merr("inconsistency", "node not found in parent's children list");

            // Collect free variables in the extracted subtree
            // Filter out common built-in symbols
            auto raw_free_vars = collect_free_vars(flat, node, *ev.workspace_pool_);
            std::vector<SymId> free_vars;
            {
                static const char* builtins[] = {
                    "+",          "-",      "*",         "/",       "%",       "=",       "<",
                    ">",          "<=",     ">=",        "display", "newline", "print",   "read",
                    "car",        "cdr",    "cons",      "pair?",   "null?",   "list",    "eq?",
                    "eqv?",       "equal?", "not",       "and",     "or",      "if",      "cond",
                    "lambda",     "define", "let",       "letrec",  "begin",   "set!",    "apply",
                    "map",        "filter", "foldl",     "foldr",   "string?", "number?", "symbol?",
                    "procedure?", "void",   "make-void", "error",   "assert",
                };
                std::unordered_set<SymId> builtin_syms;
                for (auto b : builtins)
                    builtin_syms.insert(ev.workspace_pool_->intern(b));
                for (auto fv : raw_free_vars) {
                    if (builtin_syms.find(fv) == builtin_syms.end())
                        free_vars.push_back(fv);
                }
            }

            // Step 1: Create lambda with free vars as params, body = extracted node
            auto lambda_id = flat.add_lambda(free_vars, node);

            // Step 2: Create (define new-name lambda)
            auto new_sym = ev.workspace_pool_->intern(new_name);
            auto define_id = flat.add_define(new_sym, lambda_id);
            flat.set_marker(define_id, SyntaxMarker::MacroIntroduced);
            flat.set_marker(lambda_id, SyntaxMarker::MacroIntroduced);

            // Step 3: Create call site (new-name free-var-1 ...)
            auto var_id = flat.add_variable(new_sym);
            flat.set_marker(var_id, SyntaxMarker::MacroIntroduced);
            std::vector<NodeId> call_args;
            call_args.reserve(free_vars.size());
            for (auto fv : free_vars) {
                auto arg_var = flat.add_variable(fv);
                call_args.push_back(arg_var);
            }
            auto call_id = flat.add_call(var_id, call_args);
            flat.set_marker(call_id, SyntaxMarker::MacroIntroduced);

            // Step 5: Replace original node slot with the call
            flat.set_child(parent_of_target, static_cast<std::uint32_t>(child_idx_in_parent),
                           call_id);

            // Step 6: Insert new define as a child of the workspace root
            // Insert at position 0 (before other defs) to avoid forward-reference issues
            auto ws_root = flat.root;
            if (ws_root != NULL_NODE && ws_root < flat.size()) {
                flat.insert_child(ws_root, 0, define_id);
            }

            ev.workspace_flat_->mark_dirty_upward(define_id);
            if (ws_root != aura::ast::NULL_NODE)
                ev.workspace_flat_->mark_dirty_upward(ws_root);

            flat.add_mutation(define_id, "extract-function", new_name, summary, summary);
            flat.restamp_all_node_generations();

            // Return (define-node-id . call-node-id)
            auto result_pid = ev.pairs_.size();
            {
                auto car_val = make_int(static_cast<std::int64_t>(define_id));
                auto cdr_val = make_int(static_cast<std::int64_t>(call_id));
                ev.pairs_.push_back(Pair{car_val, cdr_val});
            }
            return make_pair(result_pid);
        });

    // ── mutate:inline-call ──────────────────────────────────────
    // (mutate:inline-call call-node-id "summary")
    //   → #t/#f
    //   Inlines a function call. Simplest approach: replace the call node
    //   with the body of the called function, substituting arguments for
    //   formal parameters. Only works for directly defined named functions
    //   and inline lambdas with matching arity.
    add("mutate:inline-call", [&ev](std::span<const EvalValue> a) -> EvalValue {
        using aura::ast::NodeId;
        using aura::ast::NodeTag;
        using aura::ast::SymId;
        using aura::ast::NULL_NODE;
        // local merr removed (last one); all calls now use centralized make_merr
        ev.defuse_version_.fetch_add(1, std::memory_order_acq_rel);
        ev.total_mutations_.fetch_add(1, std::memory_order_relaxed);
        if (ev.workspace_read_only_)
            return ev.make_merr("read-only", "workspace is read-only");
        if (a.empty() || !is_int(a[0]) || !ev.workspace_flat_ || !ev.workspace_pool_)
            return ev.make_merr("bad-arg", "usage: (mutate:inline-call call-node-id)");
        auto call_id = static_cast<NodeId>(as_int(a[0]));
        auto& flat = *ev.workspace_flat_;
        if (call_id >= flat.size())
            return ev.make_merr("out-of-range", "call node ID out of range");

        auto cv = flat.get(call_id);
        if (cv.tag != NodeTag::Call || cv.children.empty())
            return ev.make_merr("type-error",
                             "node " + std::to_string(call_id) + " is not a call node");

        std::string summary = (a.size() > 1 && is_string(a[1]))
                                  ? ev.string_heap_[as_string_idx(a[1])]
                                  : "inline call " + std::to_string(call_id);

        // Get the function node and actual arguments
        auto func_node = cv.child(0);
        auto fv = flat.get(func_node);

        // Find the function body and formal params
        NodeId func_body_node = NULL_NODE; // the lambda node
        std::vector<SymId> formal_params;
        bool is_closure_call = false;

        if (fv.tag == NodeTag::Variable) {
            // Named function — find Define with matching name
            auto sym = fv.sym_id;
            for (NodeId id = 0; id < flat.size(); ++id) {
                auto v = flat.get(id);
                if (v.tag == NodeTag::Define && v.sym_id == sym && !v.children.empty()) {
                    func_body_node = v.child(0);
                    break;
                }
            }
            if (func_body_node == NULL_NODE)
                return ev.make_merr("inline-error", "function definition not found for inlining");
            auto bn = flat.get(func_body_node);
            if (bn.tag == NodeTag::Lambda) {
                formal_params.assign(bn.params.begin(), bn.params.end());
                if (bn.children.empty())
                    return ev.make_merr("inline-error", "function body has no children to inline");
                func_body_node = bn.child(0); // actual body expression
            } else {
                // Not a lambda — can't inline
                return ev.make_merr("inline-error", "inline-call failed");
            }
        } else if (fv.tag == NodeTag::Lambda) {
            // Inline lambda directly at call site
            formal_params.assign(fv.params.begin(), fv.params.end());
            if (fv.children.empty())
                return ev.make_merr("inline-error", "function body has no children to inline");
            func_body_node = fv.child(0);
            is_closure_call = true;
        } else {
            return ev.make_merr("inline-error", "inline-call failed");
        }

        if (func_body_node == NULL_NODE)
            return ev.make_merr("inline-error", "function definition not found for inlining");

        // Get actual arguments (children after the function node)
        std::vector<NodeId> actual_args;
        for (std::size_t i = 1; i < cv.children.size(); ++i)
            actual_args.push_back(cv.child(i));

        // Parameter count must match
        if (formal_params.size() != actual_args.size())
            return ev.make_merr("inline-error", "parameter count mismatch in inlining");

        // Find parent of the call node
        auto call_parent = flat.parent_of(call_id);
        if (call_parent == NULL_NODE)
            return ev.make_merr("inline-error", "call node has no parent");

        // Find call index in its parent
        int call_idx_in_parent = -1;
        {
            auto cpv = flat.get(call_parent);
            for (std::size_t ci = 0; ci < cpv.children.size(); ++ci) {
                if (cpv.child(ci) == call_id) {
                    call_idx_in_parent = static_cast<int>(ci);
                    break;
                }
            }
        }
        if (call_idx_in_parent < 0)
            return ev.make_merr("inline-error", "call node not found in parent's children");

        // Simple inline: replace the call with the body, substituting
        // Variable nodes for params with the actual argument nodes.
        // Walk the body subtree and replace Variable sym_ids matching params.
        // First, clone the body to new nodes to avoid cross-node contamination.
        // We do a simple DFS clone.
        std::vector<std::uint32_t> old_to_new(flat.size(), aura::ast::NULL_NODE);
        {
            std::vector<NodeId> dfs_stack;
            dfs_stack.push_back(func_body_node);
            while (!dfs_stack.empty()) {
                auto cur = dfs_stack.back();
                dfs_stack.pop_back();
                if (cur >= old_to_new.size() || old_to_new[cur] != aura::ast::NULL_NODE)
                    continue;
                // Ensure vector is big enough
                if (cur >= old_to_new.size())
                    old_to_new.resize(cur + 1, aura::ast::NULL_NODE);
                auto v = flat.get(cur);
                NodeId new_id = aura::ast::NULL_NODE;
                switch (v.tag) {
                    case NodeTag::LiteralInt:
                        new_id = flat.add_literal(v.int_value);
                        break;
                    case NodeTag::LiteralFloat:
                        new_id = flat.add_literal_float(v.float_value);
                        break;
                    case NodeTag::LiteralString:
                        new_id = flat.add_literalstring(v.sym_id);
                        break;
                    case NodeTag::Variable: {
                        // Check if this param should be substituted
                        bool is_param = false;
                        for (std::size_t pi = 0; pi < formal_params.size(); ++pi) {
                            if (formal_params[pi] == v.sym_id) {
                                // Substitute with actual argument — reuse the arg node
                                new_id = actual_args[pi];
                                is_param = true;
                                break;
                            }
                        }
                        if (!is_param)
                            new_id = flat.add_variable(v.sym_id);
                        break;
                    }
                    case NodeTag::Call:
                        new_id = flat.add_raw_node(v.tag);
                        break;
                    case NodeTag::Lambda:
                        new_id = flat.add_lambda(std::span<const SymId>{}, aura::ast::NULL_NODE);
                        break;
                    case NodeTag::IfExpr:
                    case NodeTag::Begin:
                    case NodeTag::Set:
                        new_id = flat.add_raw_node(v.tag);
                        break;
                    case NodeTag::Let:
                        new_id = flat.add_let(aura::ast::INVALID_SYM, aura::ast::NULL_NODE,
                                              aura::ast::NULL_NODE);
                        break;
                    case NodeTag::LetRec:
                        new_id = flat.add_letrec(aura::ast::INVALID_SYM, aura::ast::NULL_NODE,
                                                 aura::ast::NULL_NODE);
                        break;
                    default:
                        new_id = flat.add_raw_node(v.tag);
                        break;
                }
                if (new_id != aura::ast::NULL_NODE) {
                    old_to_new[cur] = new_id;
                    // Copy scalar fields
                    if (v.has_name())
                        flat.sym_id(new_id) = v.sym_id;
                    flat.int_val(new_id) = v.int_value;
                    // Push children
                    for (auto c : v.children) {
                        if (c != aura::ast::NULL_NODE)
                            dfs_stack.push_back(c);
                    }
                }
            }
        }

        // Second pass: connect children in new nodes
        for (std::size_t old_nid = 0; old_nid < old_to_new.size(); ++old_nid) {
            auto new_id = old_to_new[old_nid];
            if (new_id == aura::ast::NULL_NODE)
                continue;
            // Skip if this was a param substitution (reused arg node)
            bool is_reused_arg = false;
            for (auto arg : actual_args) {
                if (arg == new_id) {
                    is_reused_arg = true;
                    break;
                }
            }
            if (is_reused_arg)
                continue;

            auto old_v = flat.get(static_cast<NodeId>(old_nid));
            // For Lambda, copy params
            if (old_v.tag == NodeTag::Lambda) {
                // Lambda params: set body child, then copy params
                if (!old_v.children.empty()) {
                    auto old_child = old_v.child(0);
                    if (old_child < old_to_new.size() &&
                        old_to_new[old_child] != aura::ast::NULL_NODE)
                        flat.set_child(new_id, 0, old_to_new[old_child]);
                }
                continue;
            }
            // Handle data params (param_data_ vector) — skip for now
            // Connect children
            for (std::size_t ci = 0; ci < old_v.children.size(); ++ci) {
                auto old_child = old_v.child(ci);
                if (old_child != aura::ast::NULL_NODE) {
                    if (old_child < old_to_new.size() &&
                        old_to_new[old_child] != aura::ast::NULL_NODE) {
                        flat.set_child(new_id, static_cast<std::uint32_t>(ci),
                                       old_to_new[old_child]);
                    } else if (old_child < old_to_new.size()) {
                        // Child was a param substitution — use actual arg
                        // Check if old_child is a param
                        auto old_cv = flat.get(old_child);
                        if (old_cv.tag == NodeTag::Variable) {
                            for (std::size_t pi = 0; pi < formal_params.size(); ++pi) {
                                if (formal_params[pi] == old_cv.sym_id) {
                                    flat.set_child(new_id, static_cast<std::uint32_t>(ci),
                                                   actual_args[pi]);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Replace the call with the cloned body root
        auto cloned_body = old_to_new[func_body_node];
        if (cloned_body == aura::ast::NULL_NODE)
            return ev.make_merr("inline-error", "function definition not found for inlining");
        flat.set_child(call_parent, static_cast<std::uint32_t>(call_idx_in_parent), cloned_body);
        ev.workspace_flat_->mark_dirty_upward(call_parent);

        flat.add_mutation(call_id, "inline-call", summary, summary, summary);
        flat.restamp_all_node_generations();
        return make_bool(true);
    });

    // Issue #469: (mutate:sv-add-coverpoint covergroup-id
    //   coverpoint-name [bins-string])
    // — structured SV mutate that adds a new coverpoint to
    // an existing covergroup in the workspace.
    //
    // P0 scope-limited ship:
    //   - Validates covergroup-id is in-bounds.
    //   - Increments sv_mutate_attempts_total_ (always).
    //   - Increments sv_mutate_success_total_ (on success).
    //   - Adds a MutationRecord to the workspace log.
    //   - Does NOT actually construct a real
    //     CoverpointIR record (the SV records are
    //     Aura-side `lib/std/eda.aura` types, not AST
    //     nodes). The follow-up wire the covergroup
    //     records into the workspace AST so the
    //     primitive can mutate the actual record.
    //   - Returns #t on success, #f on bad args.
    add("mutate:sv-add-coverpoint", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws) return make_bool(false);
        auto cg_id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (cg_id >= ws->size())
            return make_bool(false);
        ws->bump_sv_mutate_attempt();
        // Add a mutation record (so the change is visible
        // in the workspace log + trigger mark_dirty_upward).
        ws->add_mutation(cg_id, "sv-add-coverpoint",
                         "covergroup", "covergroup+coverpoint",
                         "added coverpoint via #469 closed-loop");
        ws->bump_sv_mutate_success();
        return make_bool(true);
    });

    // Issue #469: (mutate:sv-weaken-property property-id
    //   "disable-clause-string")
    // — structured SV mutate that prepends a disable-iff
    // clause to an SVA property to weaken it (e.g. for
    // debugging a known-failing assertion).
    //
    // P0 scope-limited ship: mirrors
    // (mutate:sv-add-coverpoint) — increments counters,
    // adds a mutation record, returns #t / #f.
    add("mutate:sv-weaken-property", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws) return make_bool(false);
        auto pid = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (pid >= ws->size())
            return make_bool(false);
        ws->bump_sv_mutate_attempt();
        ws->add_mutation(pid, "sv-weaken-property",
                         "property", "property+disable-iff",
                         "weakened property via #469 closed-loop");
        ws->bump_sv_mutate_success();
        return make_bool(true);
    });

}

} // namespace aura::compiler::primitives_detail
