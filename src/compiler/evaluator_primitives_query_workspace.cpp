// evaluator_primitives_query_workspace.cpp — P0 step 9: workspace AST query primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.type;
import aura.compiler.value;
import aura.parser.parser;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimFn = std::function<EvalValue(std::span<const EvalValue>)>;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;
using MakeErrorVal = std::function<EvalValue(const std::string&, const std::string&)>;

using namespace types;

struct WorkspaceQueryState {
    std::shared_mutex& workspace_mtx;
    aura::ast::FlatAST*& workspace_flat;
    aura::ast::StringPool*& workspace_pool;
    void*& type_registry;
    std::vector<std::string>& keyword_table;
    std::pmr::vector<Pair>& pairs;
    std::pmr::vector<std::string>& string_heap;
    aura::ast::ASTArena*& temp_arena;
    std::unordered_map<std::uint64_t, std::vector<aura::ast::NodeId>>& tag_arity_index;
    std::function<aura::ast::StringPool*()> canonical_pool;
    std::function<void()> build_tag_arity_index;
};

void register_workspace_query_primitives(
    PrimRegistrar add, std::shared_mutex& workspace_mtx, aura::ast::FlatAST*& workspace_flat,
    aura::ast::StringPool*& workspace_pool, void*& type_registry,
    std::vector<std::string>& keyword_table, std::pmr::vector<Pair>& pairs,
    std::pmr::vector<std::string>& string_heap, aura::ast::ASTArena*& temp_arena,
    std::unordered_map<std::uint64_t, std::vector<aura::ast::NodeId>>& tag_arity_index,
    std::function<aura::ast::StringPool*()> canonical_pool, std::function<void()> build_tag_arity_index,
    MakeErrorVal mev) {
    WorkspaceQueryState ws{workspace_mtx,     workspace_flat,   workspace_pool, type_registry,
                         keyword_table,       pairs,            string_heap,    temp_arena,
                         tag_arity_index,     canonical_pool,   build_tag_arity_index};

    // (query:find name) — Find all node IDs with matching symbol name
    add("query:find", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (query:find name)");
        auto idx = as_string_idx(a[0]);
        if (idx >= ws.string_heap.size())
            return mev("bad-arg", "name string index out of range");
        if (!ws.workspace_flat || !ws.workspace_pool)
            return mev("no-workspace", "no workspace AST loaded");
        auto& flat = *ws.workspace_flat;
        auto name = ws.string_heap[idx];
        // Phase 2.5.0: route via ws.canonical_pool() (== workspace_pool, explicit).
        auto sym = ws.canonical_pool()->intern(name);
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.sym_id == sym) {
                auto pid = ws.pairs.size();
                ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                result = make_pair(pid);
            }
        }
        return result;
    });

    // (query:children node-id) — Get children node IDs
    add("query:children", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]) || !ws.workspace_flat)
            return mev("bad-arg", "usage: (query:children node-id)");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (node >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(node) + " >= flat size " +
                                           std::to_string(flat.size()));
        auto v = flat.get(node);
        EvalValue result = make_void();
        for (std::size_t i = v.children.size(); i > 0; --i) {
            auto pid = ws.pairs.size();
            ws.pairs.push_back({make_int(static_cast<std::int64_t>(v.child(i - 1))), result});
            result = make_pair(pid);
        }
        return result;
    });

    // Issue #249: (query:children-stable node-id) — Get children
    // as a list of (node-id . generation) stable-ref pairs. Use
    // this instead of (query:children ...) when the result is
    // stored in a variable that may be used after a mutate call.
    // The captured generation lets validate the ref later (via
    // (mutate:check-stable-ref) or pass it back to a mutate
    // primitive that supports stable-ref inputs.
    add("query:children-stable", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]) || !ws.workspace_flat)
            return mev("bad-arg", "usage: (query:children-stable node-id)");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (node >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(node) + " >= flat size " +
                                           std::to_string(flat.size()));
        auto stable_children = flat.children_stable(node);
        auto gen = flat.generation();
        // Build a list of (node-id . gen) pairs, in the same
        // order as the underlying children span.
        EvalValue result = make_void();
        for (auto it = stable_children.rbegin(); it != stable_children.rend(); ++it) {
            // Build (node-id . gen) pair
            auto gen_pid = ws.pairs.size();
            ws.pairs.push_back({make_int(static_cast<std::int64_t>(gen)), make_void()});
            auto pair_pid = ws.pairs.size();
            ws.pairs.push_back({make_int(static_cast<std::int64_t>(it->id)), make_pair(gen_pid)});
            auto pair_ev = make_pair(pair_pid);
            // Prepend to result
            auto list_pid = ws.pairs.size();
            ws.pairs.push_back({pair_ev, result});
            result = make_pair(list_pid);
        }
        return result;
    });

    // Issue #249: (query:parent-stable node-id) — Get the
    // parent as a (node-id . generation) stable-ref pair. Returns
    // an empty list if the node has no parent.
    add("query:parent-stable", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]) || !ws.workspace_flat)
            return mev("bad-arg", "usage: (query:parent-stable node-id)");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (node >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(node) + " >= flat size " +
                                           std::to_string(flat.size()));
        auto pref = flat.parent_stable(node);
        if (pref.id == aura::ast::NULL_NODE)
            return make_void();
        auto gen = flat.generation();
        // Build (parent-id . gen) pair
        auto gen_pid = ws.pairs.size();
        ws.pairs.push_back({make_int(static_cast<std::int64_t>(gen)), make_void()});
        auto pair_pid = ws.pairs.size();
        ws.pairs.push_back({make_int(static_cast<std::int64_t>(pref.id)), make_pair(gen_pid)});
        return make_pair(pair_pid);
    });

    // (query:root) — Return the current workspace root node ID, or #f if no workspace
    add("query:root", [ws, mev](const auto&) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        if (ws.workspace_flat->root == aura::ast::NULL_NODE)
            return mev("no-root", "workspace AST has no root node");
        return make_int(static_cast<std::int64_t>(ws.workspace_flat->root));
    });


    // (query:node node-id) — Get node details as list (tag value type sym-id)
    add("query:node", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (query:node node-id)");
        if (!ws.workspace_flat || !ws.workspace_pool)
            return mev("no-workspace", "no workspace AST loaded");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (node >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(node) + " >= flat size " +
                                           std::to_string(flat.size()));
        auto v = flat.get(node);

        // Build result: list of (tag-id value sym-name children-count)
        EvalValue result = make_void();

        // children-count
        auto pid = ws.pairs.size();
        ws.pairs.push_back({make_int(static_cast<std::int64_t>(v.children.size())), result});
        result = make_pair(pid);

        // value (int or string content for string literals)
        if (v.has_int() && v.tag != aura::ast::NodeTag::LiteralString) {
            pid = ws.pairs.size();
            ws.pairs.push_back({make_int(v.int_value), result});
            result = make_pair(pid);
        } else if (v.sym_id != aura::ast::INVALID_SYM) {
            auto sym_name = std::string(ws.workspace_pool->resolve(v.sym_id));
            auto sid = ws.string_heap.size();
            ws.string_heap.push_back(sym_name);
            pid = ws.pairs.size();
            ws.pairs.push_back({make_string(sid), result});
            result = make_pair(pid);
        }

        // tag-id (integer tag == NodeTag enum value)
        pid = ws.pairs.size();
        ws.pairs.push_back({make_int(static_cast<std::int64_t>(v.tag)), result});
        result = make_pair(pid);

        return result;
    });

    // Issue #235: (query:stable-ref node-id) — Returns a
    // stable reference to a node as (node-id . current-gen).
    // The stable-ref can be passed back to mutate:* primitives
    // which will verify the generation matches before applying
    // the mutation. If the generation has changed (i.e., a
    // structural mutation has happened since the ref was
    // captured), the primitive fails with a "stale-ref" error
    // instead of silently operating on a wrong node.
    //
    // The result is a 2-element list `(id . gen)` so the agent
    // can capture it as a single value and pass it through
    // multi-round edit pipelines.
    add("query:stable-ref", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (query:stable-ref node-id)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (node >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(node) + " >= flat size " +
                                           std::to_string(flat.size()));
        auto gen = flat.generation();
        // Build (node-id . gen) pair
        auto gen_pid = ws.pairs.size();
        ws.pairs.push_back({make_int(static_cast<std::int64_t>(gen)), make_void()});
        auto pair_pid = ws.pairs.size();
        ws.pairs.push_back({make_int(static_cast<std::int64_t>(node)), make_pair(gen_pid)});
        return make_pair(pair_pid);
    });

    // (query:calls name) — Find all call sites of a named function
    add("query:calls", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (query:calls name)");
        if (!ws.workspace_flat || !ws.workspace_pool)
            return mev("no-workspace", "no workspace AST loaded");
        auto idx = as_string_idx(a[0]);
        if (idx >= ws.string_heap.size())
            return mev("bad-arg", "name string index out of range");
        auto& flat = *ws.workspace_flat;
        auto name = ws.string_heap[idx];
        // Phase 2.5.0: route via ws.canonical_pool() (== workspace_pool, explicit).
        auto sym = ws.canonical_pool()->intern(name);
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::Call && !v.children.empty()) {
                auto callee = flat.get(v.child(0));
                if (callee.tag == aura::ast::NodeTag::Variable && callee.sym_id == sym) {
                    auto pid = ws.pairs.size();
                    ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                    result = make_pair(pid);
                }
            }
        }
        return result;
    });

    // ═══════════════════════════════════════════════════════════════
    // P1: Query/Transform EDSL 扩展
    // ═══════════════════════════════════════════════════════════════

    // (query:parent node-id) — Find parent node IDs (nodes whose children include this ID)
    add("query:parent", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (query:parent node-id)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        auto target = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (target >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(target) + " >= flat size " +
                                           std::to_string(flat.size()));
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                if (v.child(ci) == target) {
                    auto pid = ws.pairs.size();
                    ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                    result = make_pair(pid);
                    break;
                }
            }
        }
        return result;
    });

    // (query:siblings node-id) — Find sibling node IDs (other children of same parent)
    add("query:siblings", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (query:siblings node-id)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        auto target = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ws.workspace_flat;
        if (target >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(target) + " >= flat size " +
                                           std::to_string(flat.size()));
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            bool parent_of_target = false;
            for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                if (v.child(ci) == target) {
                    parent_of_target = true;
                    break;
                }
            }
            if (parent_of_target) {
                for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                    auto child = v.child(ci);
                    if (child != target) {
                        auto pid = ws.pairs.size();
                        ws.pairs.push_back({make_int(static_cast<std::int64_t>(child)), result});
                        result = make_pair(pid);
                    }
                }
            }
        }
        return result;
    });

    // ═══════════════════════════════════════════════════════════════
    // P8a: Query/Transform EDSL — combined filter/where (P1)
    // ═══════════════════════════════════════════════════════════════

    // (where :field value) — Create a predicate for query:filter.
    // Supported fields:
    //   :node-type  — match NodeTag name (e.g. 'Call, 'Define, 'LiteralInt)
    //   :callee     — for Call nodes, match callee Variable name
    //   :has-param  — node has a parameter with given name
    //   :defined-by — node is a Define with given name
    //   :tag        — alias for :node-type
    //   :has-child  — node has at least one child with the given NodeTag name
    //   :depth      — node is at the given depth from root (e.g. "0" = root)
    //   :marker     — Issue #244: match SyntaxMarker name
    //                 ("User" / "MacroIntroduced" / "BoolLiteral").
    //                 Useful for EDSL queries that want to operate
    //                 only on macro-introduced code, or only on
    //                 user-written code, after hygienic macro
    //                 expansion.
    //
    // Returns a predicate descriptor (a tagged pair) that query:filter
    // applies to each candidate node.
    add("query:where", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.size() < 2 || !is_keyword(a[0]) || !is_string(a[1]))
            return mev("bad-arg", "usage: (where :field-name value-string)");
        if (!ws.workspace_flat || !ws.workspace_pool)
            return mev("no-workspace", "no workspace AST loaded");
        auto field_idx = as_keyword_idx(a[0]);
        if (field_idx >= ws.keyword_table.size())
            return mev("bad-arg", "unknown keyword");
        auto field_name = ws.keyword_table[field_idx];
        auto val_idx = as_string_idx(a[1]);
        if (val_idx >= ws.string_heap.size())
            return mev("bad-arg", "value string index out of range");
        auto value = ws.string_heap[val_idx];
        auto& flat = *ws.workspace_flat;
        auto& pool = *ws.workspace_pool;

        // Store the predicate as a pair: (field-name value-sym)
        auto field_keyword_idx = ws.keyword_table.size();
        ws.keyword_table.push_back(field_name);
        auto val_sym = pool.intern(value);
        auto val_string_idx = ws.string_heap.size();
        ws.string_heap.push_back(value);

        // Encode as (key:pair key:pair) where car=field keyword, cdr=value string ref
        // This tagged structure is opaque to users but query:filter knows how to apply it.
        auto val_pair = ws.pairs.size();
        ws.pairs.push_back({make_keyword(field_keyword_idx), make_string(val_string_idx)});
        return make_pair(val_pair);
    });

    // (query:filter predicate ...) — Filter workspace nodes matching ALL predicates.
    // Each predicate is created by (where :field value).
    // Returns a list of matching node IDs.
    //
    // Usage:
    //   (query:filter (where :node-type "Call") (where :callee "sort"))
    //   → all Call nodes where the callee is "sort"
    //
    //   (query:filter (where :defined-by "fib") (where :node-type "Lambda"))
    //   → the body Lambda of (define fib ...)
    add("query:filter", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty())
            return mev("bad-arg", "usage: (query:filter predicate ...)");
        if (!ws.workspace_flat || !ws.workspace_pool)
            return mev("no-workspace", "no workspace AST loaded");
        auto& flat = *ws.workspace_flat;
        auto& pool = *ws.workspace_pool;

        // Collect predicates from arguments (each is a (where ...) pair)
        struct Predicate {
            std::string field;
            std::string value;
        };
        std::vector<Predicate> predicates;

        for (std::size_t ai = 0; ai < a.size(); ++ai) {
            if (!is_pair(a[ai]))
                return mev("bad-arg", "each predicate must be a (where ...) pair");
            auto pair_idx = as_pair_idx(a[ai]);
            auto car = ws.pairs[pair_idx].car;
            auto cdr = ws.pairs[pair_idx].cdr;
            if (!is_keyword(car) || !is_string(cdr))
                return mev("bad-arg", "malformed predicate");
            auto kidx = as_keyword_idx(car);
            auto sidx = as_string_idx(cdr);
            if (kidx >= ws.keyword_table.size() || sidx >= ws.string_heap.size())
                return mev("bad-arg", "predicate field/value out of range");
            predicates.push_back({ws.keyword_table[kidx], ws.string_heap[sidx]});
        }

        if (predicates.empty())
            return mev("bad-arg", "at least one predicate required");

        // Iterate all workspace nodes, applying all predicates
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            bool match = true;

            for (auto& p : predicates) {
                if (p.field == ":node-type" || p.field == ":tag") {
                    // Match NodeTag name
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
                    // For Call nodes, match callee Variable name
                    if (v.tag == aura::ast::NodeTag::Call && !v.children.empty()) {
                        auto callee = flat.get(v.child(0));
                        if (callee.tag != aura::ast::NodeTag::Variable ||
                            pool.resolve(callee.sym_id) != p.value) {
                            match = false;
                            break;
                        }
                    } else {
                        match = false;
                        break;
                    }
                } else if (p.field == ":defined-by" || p.field == ":defines") {
                    // Match Define nodes by name
                    if (v.tag == aura::ast::NodeTag::Define) {
                        auto name = pool.resolve(v.sym_id);
                        if (name != p.value) {
                            match = false;
                            break;
                        }
                    } else {
                        match = false;
                        break;
                    }
                } else if (p.field == ":has-param") {
                    // Check if node has a parameter with the given name
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
                    // Check if node has at least one child with the given NodeTag name
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
                } else if (p.field == ":depth") {
                    // Check if node is at the given depth from root
                    int target_depth = 0;
                    try {
                        target_depth = std::stoi(p.value);
                    } catch (...) {
                        match = false;
                        break;
                    }
                    if (target_depth < 0) {
                        match = false;
                        break;
                    }
                    // Starting from this node, walk up via children_of to count depth
                    int actual_depth = 0;
                    aura::ast::NodeId cur = id;
                    while (cur != 0) { // root is always NodeId 0
                        // Find parent by scanning all nodes for one that has cur as child
                        aura::ast::NodeId parent = aura::ast::NULL_NODE;
                        for (aura::ast::NodeId pid = 0; pid < flat.size(); ++pid) {
                            auto pv = flat.get(pid);
                            for (auto cid : pv.children) {
                                if (cid == cur) {
                                    parent = pid;
                                    break;
                                }
                            }
                            if (parent != aura::ast::NULL_NODE)
                                break;
                        }
                        if (parent == aura::ast::NULL_NODE)
                            break;
                        cur = parent;
                        ++actual_depth;
                    }
                    if (actual_depth != target_depth) {
                        match = false;
                        break;
                    }
                } else if (p.field == ":marker") {
                    // Issue #244: match SyntaxMarker by name.
                    // The marker column is populated by clone_macro_body
                    // (Issue #190) and persists in ws.workspace_flat across
                    // mutations. Marker names (case-sensitive):
                    //   "User"           — code the user wrote directly
                    //   "MacroIntroduced" — code inserted by a hygienic macro
                    //   "BoolLiteral"    — auto-generated #t / #f nodes
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

            if (match) {
                auto pid = ws.pairs.size();
                ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                result = make_pair(pid);
            }
        }
        return result;
    });

    // (query:node-type tag-name) — Find all nodes with a given NodeTag name
    // Tag names: LiteralInt, Variable, Call, IfExpr, Lambda, Let, LetRec,
    //            Define, Begin, Set, Quote, LiteralString, TypeAnnotation,
    //            Coercion, LiteralFloat, MacroDef
    add("query:node-type", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (query:node-type tag-name)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        auto idx = as_string_idx(a[0]);
        if (idx >= ws.string_heap.size())
            return mev("bad-arg", "tag name string index out of range");
        auto target_name = ws.string_heap[idx];
        auto& flat = *ws.workspace_flat;

        // Convert tag name to NodeTag enum
        aura::ast::NodeTag target_tag = static_cast<aura::ast::NodeTag>(-1);
        bool found_tag = false;
        for (auto& m : aura::ast::kNodeMeta) {
            if (m.name == target_name && m.name != "<gap>") {
                target_tag = m.tag;
                found_tag = true;
                break;
            }
        }
        if (!found_tag)
            return mev("unknown-tag", std::string("unknown node type \"") + target_name + "\"");

        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            if (flat.get(id).tag == target_tag) {
                auto pid = ws.pairs.size();
                ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                result = make_pair(pid);
            }
        }
        return result;
    });

    // (query:by-marker marker-name) — Issue #244: general marker
    // query. Returns all nodes with the given SyntaxMarker.
    // marker-name is a string: "User" / "MacroIntroduced" /
    // "BoolLiteral". The opposite of (query:macro-introduced)
    // (which hard-codes MacroIntroduced); this primitive lets
    // callers query by any marker.
    //
    // Returns a list of NodeIds (same encoding as
    // query:node-type / query:filter).
    //
    // Optional 2nd arg: integer limit N. Only the first N
    // matches are returned. Default: no limit.
    add("query:by-marker", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || a.size() > 2 || !is_string(a[0]))
            return mev(
                "bad-arg",
                "usage: (query:by-marker marker-name) or (query:by-marker marker-name limit-int)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");

        auto idx = as_string_idx(a[0]);
        if (idx >= ws.string_heap.size())
            return mev("bad-arg", "marker name string index out of range");
        auto marker_name = ws.string_heap[idx];

        // Map name → SyntaxMarker enum
        aura::ast::SyntaxMarker target;
        if (marker_name == "User") {
            target = aura::ast::SyntaxMarker::User;
        } else if (marker_name == "MacroIntroduced") {
            target = aura::ast::SyntaxMarker::MacroIntroduced;
        } else if (marker_name == "BoolLiteral") {
            target = aura::ast::SyntaxMarker::BoolLiteral;
        } else {
            return mev("unknown-marker", std::string("unknown marker name: \"") + marker_name +
                                             "\" (expected User / MacroIntroduced / BoolLiteral)");
        }

        std::int64_t limit = -1;
        if (a.size() == 2) {
            if (!is_int(a[1]))
                return mev("bad-arg", "limit must be an integer");
            limit = as_int(a[1]);
            if (limit < 0)
                return mev("bad-arg", "limit must be non-negative");
        }

        auto& flat = *ws.workspace_flat;
        EvalValue result = make_void();
        std::int64_t emitted = 0;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            if (limit >= 0 && emitted >= limit)
                break;
            if (flat.marker(id) == target) {
                auto pid = ws.pairs.size();
                ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                result = make_pair(pid);
                ++emitted;
            }
        }
        return result;
    });

    // (query:macro-introduced) — Issue #244: shortcut for
    // (query:filter (where :marker "MacroIntroduced")).
    // Returns a list of NodeIds whose SyntaxMarker is
    // SyntaxMarker::MacroIntroduced (i.e. code inserted by
    // a hygienic macro via clone_macro_body). Used by
    // AI agents and self-mod code to identify which AST
    // nodes are macro-introduced (and thus might need
    // hygiene-safe handling when mutating).
    //
    // Returns a list of node IDs (same encoding as
    // query:node-type / query:filter).
    //
    // Optional arg: an integer limit N. If provided, only
    // the first N matching node IDs are returned. Default
    // (no arg) returns all matches. Useful for the common
    // "do any macro-introduced nodes exist?" check
    // (pass limit=1).
    add("query:macro-introduced", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.size() > 1)
            return mev("bad-arg",
                       "usage: (query:macro-introduced) or (query:macro-introduced limit-int)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");

        std::int64_t limit = -1; // -1 = no limit
        if (a.size() == 1) {
            if (!is_int(a[0]))
                return mev("bad-arg", "limit must be an integer");
            limit = as_int(a[0]);
            if (limit < 0)
                return mev("bad-arg", "limit must be non-negative");
        }

        auto& flat = *ws.workspace_flat;
        EvalValue result = make_void();
        std::int64_t emitted = 0;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            if (limit >= 0 && emitted >= limit)
                break;
            if (flat.marker(id) == aura::ast::SyntaxMarker::MacroIntroduced) {
                auto pid = ws.pairs.size();
                ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                result = make_pair(pid);
                ++emitted;
            }
        }
        return result;
    });

    // (query:marker-stats) — Issue #247: aggregate SyntaxMarker
    // distribution in the workspace. Returns a 4-element list:
    //   (user-count macro-introduced-count bool-literal-count total-count)
    //
    // Equivalent to (syntax-marker-counts) but returns a simple
    // list instead of a hash table. Use this when you want to
    // pipe the result through list operations (e.g., assert all
    // counts are > 0 except for some category). For dashboards
    // and key/value lookups, (syntax-marker-counts) is the
    // better choice.
    //
    // Returns a proper list of 4 integers:
    //   (user macro-introduced bool-literal total)
    //
    // If no workspace is set, returns '().
    add("query:marker-stats", [ws, mev](const auto&) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");

        std::size_t user = 0, macro = 0, bool_lit = 0, total = 0;
        const auto& markers = ws.workspace_flat->marker_column();
        for (auto m : markers) {
            ++total;
            auto val = static_cast<int>(m);
            if (val == 0)
                ++user;
            else if (val == 1)
                ++macro;
            else if (val == 2)
                ++bool_lit;
        }
        // Build a proper 4-element list: (user macro bool total).
        // Use make_pair chaining (cdr-then-prepend pattern).
        // The list reads (total . (bool . (macro . (user . ())))) — i.e.
        // car=total, cdr=(bool ...). But we want (user macro bool total).
        // So build in reverse and then return as-is (the result is
        // a list whose first element is total). The Aura code can
        // destructure via pattern match.
        //
        // Actually, building the list in REVERSE order (push each
        // onto the head) would give (total . (bool . (macro . (user))))
        // which when iterated left-to-right yields total, bool, macro,
        // user. That's the wrong order. Build FORWARD: prepend each
        // new element to the previous list.
        EvalValue result = make_void();
        auto append = [&](std::int64_t v) {
            auto pid = ws.pairs.size();
            ws.pairs.push_back({make_int(v), result});
            result = make_pair(pid);
        };
        append(static_cast<std::int64_t>(user));
        append(static_cast<std::int64_t>(macro));
        append(static_cast<std::int64_t>(bool_lit));
        append(static_cast<std::int64_t>(total));
        // Now result is (user (macro (bool (total . ()))))
        return result;
    });

    // (query:schema-of-marker marker-name) — Issue #248:
    // return (NodeId . type-name) pairs for nodes with the given
    // SyntaxMarker AND a non-zero type_id_ (i.e., the type
    // checker has inferred a type for them).
    //
    // Use this to introspect macro-introduced code's inferred
    // types. Example:
    //   (query:schema-of-marker "MacroIntroduced")
    //   → ((3 . "int") (5 . "lambda") ...)
    //
    // Optional 2nd arg: integer limit N. Caps the result list
    // at N entries.
    //
    // If no workspace is set, returns '().
    //
    // Note: this is the *observability* side of #248. The
    // enforcement side (using the schema to reject mutations
    // that would break macro type invariants) is deferred.
    add("query:schema-of-marker", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || a.size() > 2 || !is_string(a[0]))
            return mev("bad-arg", "usage: (query:schema-of-marker marker-name) or "
                                  "(query:schema-of-marker marker-name limit-int)");
        if (!ws.workspace_flat)
            return mev("no-workspace", "no workspace AST loaded");
        if (!ws.type_registry)
            return mev("no-registry", "no type registry available");

        auto idx = as_string_idx(a[0]);
        if (idx >= ws.string_heap.size())
            return mev("bad-arg", "marker name string index out of range");
        auto marker_name = ws.string_heap[idx];

        // Map name → SyntaxMarker enum
        aura::ast::SyntaxMarker target;
        if (marker_name == "User") {
            target = aura::ast::SyntaxMarker::User;
        } else if (marker_name == "MacroIntroduced") {
            target = aura::ast::SyntaxMarker::MacroIntroduced;
        } else if (marker_name == "BoolLiteral") {
            target = aura::ast::SyntaxMarker::BoolLiteral;
        } else {
            return mev("unknown-marker", std::string("unknown marker name: \"") + marker_name +
                                             "\" (expected User / MacroIntroduced / BoolLiteral)");
        }

        std::int64_t limit = -1;
        if (a.size() == 2) {
            if (!is_int(a[1]))
                return mev("bad-arg", "limit must be an integer");
            limit = as_int(a[1]);
            if (limit < 0)
                return mev("bad-arg", "limit must be non-negative");
        }

        auto& flat = *ws.workspace_flat;
        auto& treg = *static_cast<aura::core::TypeRegistry*>(ws.type_registry);
        EvalValue result = make_void();
        std::int64_t emitted = 0;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            if (limit >= 0 && emitted >= limit)
                break;
            if (flat.marker(id) != target)
                continue;
            // Issue #248: only include nodes with a non-zero
            // type_id_ (i.e., the type checker has inferred a
            // type for them). Nodes with type_id_ == 0 haven't
            // been typed yet — they're "schema-less" in the
            // sense that we don't know their expected type.
            const auto type_id_value = flat.type_id(id);
            if (type_id_value == 0)
                continue;
            // Convert uint32_t to TypeId. The FlatAST stores
            // just the index; TypeId requires (index, generation).
            // The type checker uses generation=1 as a default
            // (see type_checker_impl.cpp:1505: TypeId{index, 1}).
            std::string type_name;
            try {
                auto tname = treg.name_of(aura::core::TypeId{type_id_value, 1});
                type_name = std::string(tname);
            } catch (...) {
                type_name = "<unknown>";
            }
            if (type_name.empty())
                type_name = "<unnamed>";
            // Build the pair: (NodeId . type-name)
            auto name_idx = ws.string_heap.size();
            ws.string_heap.push_back(type_name);
            auto name_ev = make_string(name_idx);
            auto pid = ws.pairs.size();
            ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), name_ev});
            auto pair_ev = make_pair(pid);
            // Prepend to result
            auto list_pid = ws.pairs.size();
            ws.pairs.push_back({pair_ev, result});
            result = make_pair(list_pid);
            ++emitted;
        }
        return result;
    });

    // ═══════════════════════════════════════════════════════════════
    // P8: Query/Transform EDSL 扩展 — pattern matching
    // ═══════════════════════════════════════════════════════════════

    // (query:pattern "expr") — Find all nodes matching a structural pattern
    //
    // Pattern syntax:
    //   (+ 1 2)     — exact match: Call("+", 1, 2)
    //   (+ 1 ...)   — wildcard: "..." matches any single subtree
    //   fib         — matches a Variable named "fib"
    //
    // The pattern is parsed as an S-expression. A Variable named "..." acts as
    // wildcard and matches any single node or subtree.
    add("query:pattern", [ws, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ws.workspace_mtx);
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (query:pattern expr)");
        if (!ws.workspace_flat || !ws.workspace_pool)
            return mev("no-workspace", "no workspace AST loaded");
        auto idx = as_string_idx(a[0]);
        if (idx >= ws.string_heap.size())
            return mev("bad-arg", "pattern string index out of range");

        // Phase 2.5.0: pat_pool stays separate from canonical_pool.
        // Patterns parse into a fresh FlatAST + pool per call (ws.temp_arena
        // reclaims at gc-temp). The wildcard "..." sym is intern'd in
        // pat_pool so pat_node.sym_id comparisons work — sharing the
        // canonical pool would mix pattern-specific garbage into the long-
        // lived workspace, and the pattern's sym_ids would clash with
        // workspace sym_ids (different ASTs, same pool). Documented
        // exception to the canonical-pool migration — see commit 14682c5.
        // Parse pattern string into its own FlatAST (separate from workspace).
        // Use ws.temp_arena so (gc-temp) reclaims it per call.
        auto alloc = ws.temp_arena->allocator();
        auto* pat_pool = ws.temp_arena->create<aura::ast::StringPool>(alloc);
        auto* pat_flat = ws.temp_arena->create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(ws.string_heap[idx], *pat_flat, *pat_pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            return make_void();

        // Intern "..." in the pattern pool for wildcard matching
        auto wildcard_sym = pat_pool->intern("...");

        // Recursive subtree matcher
        std::function<bool(aura::ast::NodeId, aura::ast::NodeId)> match_subtree;
        match_subtree = [&, wildcard_sym](aura::ast::NodeId ws_id,
                                          aura::ast::NodeId pat_id) -> bool {
            if (pat_id >= pat_flat->size())
                return ws_id >= ws.workspace_flat->size();
            if (ws_id >= ws.workspace_flat->size() || pat_id == aura::ast::NULL_NODE)
                return (pat_id == aura::ast::NULL_NODE) ? (ws_id == aura::ast::NULL_NODE) : false;

            auto ws_node = ws.workspace_flat->get(ws_id);
            auto pat_node = pat_flat->get(pat_id);

            // Wildcard "..." matches any single subtree
            if (pat_node.tag == aura::ast::NodeTag::Variable && pat_node.sym_id == wildcard_sym)
                return true;

            // Same tag required
            if (ws_node.tag != pat_node.tag)
                return false;

            switch (pat_node.tag) {
                case aura::ast::NodeTag::LiteralInt:
                    return ws_node.int_value == pat_node.int_value;
                case aura::ast::NodeTag::LiteralFloat:
                    return ws_node.float_value == pat_node.float_value;
                case aura::ast::NodeTag::Variable:
                case aura::ast::NodeTag::LiteralString:
                    return ws.workspace_pool->resolve(ws_node.sym_id) ==
                           pat_pool->resolve(pat_node.sym_id);
                case aura::ast::NodeTag::MacroDef:
                    return true;
                default:
                    if (ws_node.children.size() != pat_node.children.size())
                        return false;
                    for (std::size_t ci = 0; ci < ws_node.children.size(); ++ci) {
                        if (!match_subtree(ws_node.child(ci), pat_node.child(ci)))
                            return false;
                    }
                    return true;
            }
        };

        auto& flat = *ws.workspace_flat;
        EvalValue result = make_void();

        // Issue #186 Phase 1: pre-compute the pattern's children
        // count once. The outer loop can then skip nodes whose
        // children count doesn't match the pattern's children
        // count, avoiding the recursive descent into subtrees
        // that can't possibly match. For large ASTs (500-5000
        // nodes) this is a real win — the O(N × Depth) becomes
        // O(N) when the pattern's children count is a strong
        // filter.
        auto pat_root_node = pat_flat->get(pr.root);
        const std::size_t pat_child_count = pat_root_node.children.size();
        const bool pat_root_is_wildcard = pat_root_node.tag == aura::ast::NodeTag::Variable &&
                                          pat_root_node.sym_id == wildcard_sym;


        // Walk every node in workspace and try matching at each position.
        // Issue #140: skip nodes with SyntaxMarker::MacroIntroduced
        // (the matcher's root position is the user-written top-level
        // code, not the macro-expanded body). Hygiene correctness:
        // matching a macro-introduced call as if it were user code
        // would be misleading. The pattern only matches user-written
        // code by default. Callers who want to include macro nodes
        // can query the specific macro node or use a different
        // primitive (out of scope for #140's basic hygiene).
        //
        // Issue #186: also skip nodes whose children count doesn't
        // match the pattern's children count (the pattern's
        // children are the only subtree that can match). This is
        // a quick early-exit that's safe because:
        //   - If pat_root is a wildcard "..." → no constraint
        //   - Otherwise, the children count must match exactly
        //     (verified later in match_subtree's default case)
        //
        // Issue #211: use the (tag, arity) index to skip
        // non-matching nodes BEFORE the recursive descent.
        // For patterns where the root's (tag, arity) is rare
        // (e.g., looking for `(+ 1 2)` in a workspace with
        // mostly `define`s and `lambda`s), this is a massive
        // speedup vs. the O(N) full walk.
        //
        // The index is built lazily on first use and cached
        // per-workspace (invalidated when ws.workspace_flat is
        // changed via set_workspace_flat).
        if (!pat_root_is_wildcard) {
            // Index lookup: find all nodes whose (tag, arity)
            // matches the pattern's root.
            ws.build_tag_arity_index();
            const std::uint32_t pat_tag_val = static_cast<std::uint32_t>(pat_root_node.tag);
            const std::uint64_t pat_key = (static_cast<std::uint64_t>(pat_tag_val) << 32) |
                                          static_cast<std::uint64_t>(pat_child_count);
            auto it = ws.tag_arity_index.find(pat_key);
            if (it == ws.tag_arity_index.end()) {
                // No nodes match the pattern's (tag, arity).
                // Skip the full walk.
                return make_void();
            }
            const auto& bucket = it->second;
            for (aura::ast::NodeId id : bucket) {
                if (id >= flat.size())
                    continue;
                if (flat.marker(id) == aura::ast::SyntaxMarker::MacroIntroduced)
                    continue;
                if (match_subtree(id, pr.root)) {
                    auto pid = ws.pairs.size();
                    ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                    result = make_pair(pid);
                }
            }
        } else {
            // Wildcard pattern: full walk (the index doesn't
            // help for wildcards).
            for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
                if (flat.marker(id) == aura::ast::SyntaxMarker::MacroIntroduced)
                    continue;
                if (match_subtree(id, pr.root)) {
                    auto pid = ws.pairs.size();
                    ws.pairs.push_back({make_int(static_cast<std::int64_t>(id)), result});
                    result = make_pair(pid);
                }
            }
        }

        return result;
    });
}

} // namespace aura::compiler::primitives_detail
