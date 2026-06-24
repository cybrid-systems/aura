// macro_expansion.cpp — Issue #265: hygienic macro clone + expansion
// aura.compiler.macro_expansion module implementation.

module;

#include <cstdint>
#include <functional>
#include <iostream>
#include <print>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

module aura.compiler.macro_expansion;

import std;
import aura.core.ast;
import aura.compiler.evaluator_pure;

namespace aura::compiler::macro_exp {

namespace detail {

const std::unordered_set<std::string>& hygiene_builtins() {
    static const std::unordered_set<std::string> builtins = {
        "if", "cond", "let", "let*", "letrec", "lambda", "define", "begin", "set!", "quote",
        "unquote", "quasiquote", "case", "when", "unless", "car", "cdr", "cons", "list",
        "pair?", "null?", "eq?", "equal?", "+", "-", "*", "/", "=", "<", ">", "<=", ">=",
        "not", "and", "or", "void", "display", "write", "newline", "number?", "integer?",
        "float?", "boolean?", "string?", "symbol?", "string-append", "string-length",
        "string-ref", "substring", "number->string", "string->number", "apply", "map",
        "filter", "foldl",
    };
    return builtins;
}

} // namespace detail

aura::ast::NodeId clone_macro_body(
    aura::ast::FlatAST& target, aura::ast::StringPool& target_pool, aura::ast::FlatAST& source,
    aura::ast::StringPool& source_pool, aura::ast::NodeId body_id,
    const std::unordered_map<std::string, aura::ast::NodeId>* subst,
    std::unordered_map<std::string, std::string>* name_map, aura::ast::SyntaxMarker cloned_marker) {
    using namespace aura::ast;
    if (body_id == NULL_NODE || body_id >= source.size())
        return NULL_NODE;
    auto v = source.get(body_id);

    // Variable substitution: if this variable is a macro param, return the arg clone.
    //
    // Issue #120: the arg's NodeId is in the *calling* FlatAST (= target),
    // not in `source` (the macro definition's FlatAST). The recursive
    // call to clone_macro_body with body_id=it->second would try to
    // read it->second from `source`, which is wrong (NodeId indices
    // are per-FlatAST). The fix: detect this case and return the
    // arg's NodeId as-is, then recursively clone its children from
    // `target` (not `source`).
    if (subst && v.tag == NodeTag::Variable && v.sym_id != INVALID_SYM) {
        auto name = std::string(source_pool.resolve(v.sym_id));
        auto it = subst->find(name);
        if (it != subst->end()) {
            // Issue #334 follow-up: REVERTED Quote-wrap from commit
            // 6b90641. The Quote-wrap made Variables in macro
            // bodies evaluate to the literal arg value (helped
            // define-struct), but it broke `set!` semantics in
            // normal macros: the set! target became a literal
            // symbol (the arg name) instead of the caller's
            // variable, causing test_issue_137/190 to fail. The
            // original AST subst (returning the arg NodeId
            // directly) is restored for now. The proper fix for
            // #230 #1 (define-struct) is the env-binding path
            // (tracked in issue 334), not Quote-wrap.
            return it->second;
        }
    }

    // Re-intern SymIds: resolve in source_pool, intern in target_pool
    auto transplant = [&](SymId sid) -> SymId {
        return (sid == INVALID_SYM) ? sid
                                    : target_pool.intern(std::string(source_pool.resolve(sid)));
    };

    // Resolve a name through name_map (hygiene: renamed binding)
    auto resolve_name = [&](SymId sid) -> std::string {
        if (sid == INVALID_SYM)
            return "";
        auto name = std::string(source_pool.resolve(sid));
        if (name_map) {
            auto it = name_map->find(name);
            if (it != name_map->end())
                return it->second;
        }
        return name;
    };

    // Rename a binding position for hygiene: gensym if macro-introduced
    std::uint64_t hyg_ctr = 0; // Issue #265: per-call counter
    auto rename_binding_pre = [&](SymId sid) -> SymId {
        if (sid == INVALID_SYM || !name_map)
            return transplant(sid);
        auto name = std::string(source_pool.resolve(sid));
        if ((subst && subst->count(name)) || detail::hygiene_builtins().count(name))
            return transplant(sid);
        auto it = name_map->find(name);
        if (it != name_map->end())
            return target_pool.intern(it->second);
        auto fresh = std::string("__") + name + "_" + std::to_string(hyg_ctr++);
        (*name_map)[name] = fresh;
        return target_pool.intern(fresh);
    };

    // Issue #120: pre-scan the body to populate name_map BEFORE cloning.
    // The body may reference gensym'd bindings (e.g., `(let ((tmp a)) (set! b tmp))`
    // — the inner `tmp` Variable reference must see the gensym'd name
    // when it's cloned). Without the pre-scan, the recursive clone
    // would process the inner `tmp` (as a Variable reference) before the
    // let binding is processed (which is what gensym's `tmp`).
    if (name_map) {
        std::function<void(NodeId)> pre_scan = [&](NodeId nid) {
            if (nid == NULL_NODE || nid >= source.size())
                return;
            auto nv = source.get(nid);
            // If this node is a binding position, gensym its name
            // (into the name_map) but don't generate any target node.
            if (nv.tag == NodeTag::Let || nv.tag == NodeTag::LetRec || nv.tag == NodeTag::Define) {
                rename_binding_pre(nv.sym_id);
            } else if (nv.tag == NodeTag::Lambda) {
                for (auto pid : nv.params)
                    rename_binding_pre(pid);
            }
            for (auto c : nv.children)
                pre_scan(c);
        };
        pre_scan(body_id);
    }

    auto rename_binding = [&](SymId sid) -> SymId {
        if (sid == INVALID_SYM || !name_map)
            return transplant(sid);
        auto name = std::string(source_pool.resolve(sid));
        // Macro params, builtins, and already-renamed names keep their name
        if ((subst && subst->count(name)) || detail::hygiene_builtins().count(name))
            return transplant(sid);
        auto it = name_map->find(name);
        if (it != name_map->end())
            return target_pool.intern(it->second);
        // Gensym! Create fresh name and track in name_map
        auto fresh = std::string("__") + name + "_" + std::to_string(hyg_ctr++);
        (*name_map)[name] = fresh;
        return target_pool.intern(fresh);
    };

    // Clone children recursively (pass cloned_marker through)
    std::vector<aura::ast::NodeId> child_ids;
    for (std::uint32_t i = 0; i < v.children.size(); ++i)
        child_ids.push_back(clone_macro_body(target, target_pool, source, source_pool, v.child(i),
                                             subst, name_map, cloned_marker));

    // Clone params (for Lambda nodes) — with hygienic renaming
    std::vector<aura::ast::SymId> param_syms;
    for (auto pid : v.params)
        param_syms.push_back(rename_binding(pid));

    aura::ast::NodeId new_id = NULL_NODE;
    switch (v.tag) {
        case NodeTag::LiteralInt:
            new_id = target.add_literal(v.int_value);
            break;
        case NodeTag::LiteralString:
            new_id = target.add_literalstring(transplant(v.sym_id));
            break;
        case NodeTag::Variable: {
            // Hygienic: check name_map for renamed bindings
            if (name_map) {
                auto name = resolve_name(v.sym_id);
                new_id = target.add_variable(target_pool.intern(name));
            } else {
                new_id = target.add_variable(transplant(v.sym_id));
            }
            break;
        }
        case NodeTag::Call: {
            std::vector<aura::ast::NodeId> args(child_ids.begin() + 1, child_ids.end());
            if (!child_ids.empty())
                new_id = target.add_call(child_ids[0], args);
            break;
        }
        case NodeTag::IfExpr:
            if (child_ids.size() >= 3)
                new_id = target.add_if(child_ids[0], child_ids[1], child_ids[2]);
            break;
        case NodeTag::Lambda:
            if (!child_ids.empty())
                new_id = target.add_lambda(param_syms, child_ids[0]);
            break;
        case NodeTag::Let:
        case NodeTag::LetRec:
            if (child_ids.size() >= 2)
                new_id =
                    (v.tag == NodeTag::Let)
                        ? target.add_let(rename_binding(v.sym_id), child_ids[0], child_ids[1])
                        : target.add_letrec(rename_binding(v.sym_id), child_ids[0], child_ids[1]);
            break;
        case NodeTag::Begin:
            if (!child_ids.empty())
                new_id = target.add_begin(child_ids);
            break;
        case NodeTag::Set:
            if (!child_ids.empty()) {
                // Issue #120: if the set! target is a macro param, look
                // up the arg and use ITS sym_id (resolved from target).
                // Otherwise the set! would target the macro param
                // (e.g., "a") which isn't bound in the calling env.
                SymId set_name_sid = transplant(v.sym_id);
                if (subst) {
                    auto set_name = std::string(source_pool.resolve(v.sym_id));
                    auto sit = subst->find(set_name);
                    if (sit != subst->end()) {
                        auto arg_v = target.get(sit->second);
                        if (arg_v.tag == NodeTag::Variable) {
                            set_name_sid = arg_v.sym_id;
                        }
                    }
                }
                new_id = target.add_set(set_name_sid, child_ids[0]);
            }
            break;
        case NodeTag::Quote:
            if (!child_ids.empty())
                new_id = target.add_quote(child_ids[0]);
            break;
        case NodeTag::Define:
            if (!child_ids.empty())
                new_id = target.add_define(rename_binding(v.sym_id), child_ids[0]);
            break;
        default:
            break;
    }

    if (new_id != NULL_NODE) {
        // Issue #190: use the caller's specified marker
        // (MacroIntroduced for macro expansion, User for closure
        // materialization). The recursive calls already set the
        // marker on each child node, so this is just the outer
        // wrapper node.
        target.set_marker(new_id, cloned_marker);
        target.set_loc(new_id, v.line, v.col);
        // Issue #290: also OR kMacroExpansion into the
        // macro_dirty_ bitmask on every node in the cloned
        // subtree (root + descendants). Single hook point for
        // ALL clone_macro_body callers (eval_flat top-level,
        // expand_inner_macros for nested, evaluator_eval_flat
        // closure materialization). We condition on
        // cloned_marker == MacroIntroduced so the
        // closure-materialization call site (which passes
        // User) doesn't accidentally trip the dirty bit.
        // Iterative walk via std::vector stack — no
        // recursion, safe for pathological depth.
        if (cloned_marker == aura::ast::SyntaxMarker::MacroIntroduced) {
            std::vector<aura::ast::NodeId> stack;
            stack.push_back(new_id);
            while (!stack.empty()) {
                auto cur = stack.back();
                stack.pop_back();
                if (cur == aura::ast::NULL_NODE) continue;
                target.apply_macro_dirty_bits(
                    cur, static_cast<std::uint8_t>(
                             aura::ast::FlatAST::MacroDirtyReason::kMacroExpansion));
                auto cv = target.get(cur);
                for (auto child : cv.children) {
                    if (child != aura::ast::NULL_NODE)
                        stack.push_back(child);
                }
            }
        }
    }
    return new_id;
}

namespace detail {

aura::ast::NodeId
unwrap_cons_chain_to_call(aura::ast::FlatAST* flat, aura::ast::StringPool* pool,
                          aura::ast::NodeId root,
                          const std::unordered_map<std::string, MacroExpansionDef>& macros) {
    using namespace aura::ast;
    if (root == NULL_NODE)
        return NULL_NODE;
    auto v = flat->get(root);
    if (v.tag != NodeTag::Call || v.children.size() != 3)
        return NULL_NODE;
    auto callee_v = flat->get(v.child(0));
    if (callee_v.tag != NodeTag::Variable)
        return NULL_NODE;
    auto callee_name = std::string(pool->resolve(callee_v.sym_id));
    if (callee_name != "cons")
        return NULL_NODE;
    // First arg must be (quote <known-macro-sym>)
    auto arg0_v = flat->get(v.child(1));
    if (arg0_v.tag != NodeTag::Quote || arg0_v.children.empty())
        return NULL_NODE;
    auto quoted_v = flat->get(arg0_v.child(0));
    if (quoted_v.tag != NodeTag::Variable)
        return NULL_NODE;
    auto quoted_name = std::string(pool->resolve(quoted_v.sym_id));
    if (macros.find(quoted_name) == macros.end())
        return NULL_NODE;
    // Walk the cdr chain (v.child(2)) to collect arg NodeIds.
    // Each step: cdr is (cons <arg> <rest>) or (quote ()).
    std::vector<NodeId> args;
    NodeId cdr_id = v.child(2);
    while (cdr_id != NULL_NODE) {
        auto cdr_v = flat->get(cdr_id);
        if (cdr_v.tag == NodeTag::Quote) {
            // (quote ()) — end of list
            break;
        }
        if (cdr_v.tag != NodeTag::Call || cdr_v.children.size() != 3) {
            // Not a cons cell — bail
            return NULL_NODE;
        }
        auto c_callee = flat->get(cdr_v.child(0));
        if (c_callee.tag != NodeTag::Variable ||
            std::string(pool->resolve(c_callee.sym_id)) != "cons") {
            return NULL_NODE;
        }
        // Push the arg (cdr_v.child(1))
        args.push_back(cdr_v.child(1));
        cdr_id = cdr_v.child(2);
    }
    // Build Call(<quoted_name>, args...)
    auto macro_var = flat->add_variable(pool->intern(quoted_name));
    flat->set_marker(macro_var, SyntaxMarker::MacroIntroduced);
    return flat->add_call(macro_var, args);
}

} // namespace detail

aura::ast::NodeId
expand_inner_macros(aura::ast::FlatAST* flat, aura::ast::StringPool* pool, aura::ast::NodeId root,
                    int depth, int max_depth,
                    const std::unordered_map<std::string, MacroExpansionDef>& macros) {
    using namespace aura::ast;
    if (root == NULL_NODE || depth >= max_depth)
        return root;
    // Issue #158: unwrap qq-built cons chains whose head is a
    // known macro. Without this, `(bar ,x)` inside a macro body
    // stays as `(cons (quote bar) ...)` after expand_qq, and the
    // main macro check below (which expects a Call head matching
    // a known macro) misses it.
    if (auto unwrapped = detail::unwrap_cons_chain_to_call(flat, pool, root, macros);
        unwrapped != NULL_NODE) {
        // Substitute the unwrapped Call for the original cons chain
        // at the parent's child slot, then recurse.
        auto parent_id = flat->parent_of(root);
        if (parent_id != NULL_NODE) {
            auto parent_v = flat->get(parent_id);
            for (std::uint32_t ci = 0; ci < parent_v.children.size(); ++ci) {
                if (parent_v.child(ci) == root) {
                    flat->set_child(parent_id, ci, unwrapped);
                    flat->restamp_all_node_generations();
                    break;
                }
            }
        }
        // Recurse into the unwrapped Call (which is now a real
        // macro call site).
        return expand_inner_macros(flat, pool, unwrapped, depth, max_depth, macros);
    }
    auto v = flat->get(root);
    if (v.tag == NodeTag::Call && !v.children.empty()) {
        auto callee_v = flat->get(v.child(0));
        if (callee_v.tag == NodeTag::Variable) {
            auto cname = std::string(pool->resolve(callee_v.sym_id));
            auto it = macros.find(cname);
            if (it != macros.end()) {
                // Build substitution: macro param → arg NodeId.
                // Issue #146 follow-up: route through the pure helper
                // so the substitution logic lives in evaluator_pure.ixx
                // (single source of truth) and the legacy inline loop
                // goes away. v.children is materialized into a vector
                // (one alloc per macro call) for the pure helper's
                // vector-typed call_args parameter.
                const auto& md = it->second;
                std::vector<aura::ast::NodeId> call_args(v.children.begin(), v.children.end());
                auto subst =
                    aura::compiler::pure::compute_macro_subst_pure(md.params, call_args, md.dotted);
                if (md.dotted) {
                    // Rest params on inner macros: not yet supported
                    // (same limitation as the main hygienic path).
                    return root;
                }
                // Clone the macro body into the current flat and
                // re-intern sym_ids. Use the runtime registry's
                // `flat` / `pool` pointers as the source.
                std::unordered_map<std::string, std::string> rename_map;
                auto* src_pool = md.pool ? md.pool : pool;
                auto cloned = clone_macro_body(*flat, *pool, *md.flat, *src_pool, md.body_id,
                                               &subst, &rename_map);
                if (cloned == NULL_NODE)
                    return root;
                // Recursively expand inner macros in the cloned body
                cloned = expand_inner_macros(flat, pool, cloned, depth + 1, max_depth, macros);
                // Rewrite the parent's child to use the cloned body
                auto parent_id = flat->parent_of(root);
                if (parent_id != NULL_NODE) {
                    auto parent_v = flat->get(parent_id);
                    for (std::uint32_t ci = 0; ci < parent_v.children.size(); ++ci) {
                        if (parent_v.child(ci) == root) {
                            flat->set_child(parent_id, ci, cloned);
                            flat->restamp_all_node_generations();
                            break;
                        }
                    }
                }
                return cloned;
            }
        }
    }
    // Not a macro call — recurse into children
    // Issue #483: re-fetch `v` every iteration. The recursive
    // call may invoke set_child on the parent (this function's
    // `root`), which replaces the parent's PersistentChildVector
    // Storage. After replacement, the captured `v.children` span
    // points to freed memory — a heap-use-after-free. Re-fetching
    // each iteration re-reads the live Storage pointer.
    for (std::uint32_t i = 0; i < flat->get(root).children.size(); ++i) {
        auto child = flat->get(root).child(i);
        // We can't modify children in place easily; rebuild
        // the current node's children via the recursive call.
        (void)expand_inner_macros(flat, pool, child, depth + 1, max_depth, macros);
    }
    return root;
}
aura::ast::NodeId macro_expand_all(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                   aura::ast::NodeId root, int max_passes) {
    using namespace aura::ast;
    for (int pass = 0; pass < max_passes; ++pass) {
        // Phase 1: collect macro definitions
        struct MD {
            aura::ast::FlatAST* src_flat;
            aura::ast::StringPool* src_pool;
            std::vector<std::string> params;
            NodeId body_id;
            bool dotted;
            bool hygienic;  // Issue #120
            bool preserved; // Issue #230 #2
        };
        std::unordered_map<std::string, MD> local_macros;
        bool has_macro_def = false;

        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::MacroDef) {
                has_macro_def = true;
                // Macro name is in sym_id; params follow
                auto macro_name = std::string(pool.resolve(v.sym_id));
                std::vector<std::string> params;
                for (auto pid : v.params)
                    params.push_back(std::string(pool.resolve(pid)));
                auto body_id = v.children.empty() ? NULL_NODE : v.child(0);
                // Issue #120: dotted is bit 0, hygienic is bit 1
                bool is_dotted = (v.int_value & 1) != 0;
                bool is_hygienic = (v.int_value & 2) != 0;
                bool is_preserved = (v.int_value & 4) != 0;
                local_macros[macro_name] = MD{&flat,     &pool,       std::move(params), body_id,
                                              is_dotted, is_hygienic, is_preserved};
            }
        }

        if (!has_macro_def)
            return root; // no more macros to expand

        // Phase 2: find and expand macro calls
        bool expanded_any = false;
        NodeId new_root = root;

        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Call && !v.children.empty()) {
                auto callee_v = flat.get(v.child(0));
                if (callee_v.tag == NodeTag::Variable) {
                    auto cname = std::string(pool.resolve(callee_v.sym_id));
                    auto it = local_macros.find(cname);
                    if (it != local_macros.end()) {
                        // Build substitution: macro param → arg expression.
                        // Issue #146 follow-up: route through the pure
                        // helper. Rest-param handling stays here because
                        // it requires FlatAST mutation (allocating a
                        // pair-list) — that's stateful, not pure.
                        auto& md = it->second;
                        std::vector<aura::ast::NodeId> call_args(v.children.begin(),
                                                                 v.children.end());
                        auto subst = aura::compiler::pure::compute_macro_subst_pure(
                            md.params, call_args, md.dotted);
                        // Rest param: collect remaining args as a quoted list.
                        // We need regular_count locally to know where the
                        // rest args start; recompute it (cheap).
                        std::size_t regular_count = md.dotted && md.params.size() > 0
                                                        ? md.params.size() - 1
                                                        : md.params.size();
                        if (md.dotted && !md.params.empty() &&
                            regular_count + 1 < v.children.size()) {
                            auto& rest_name = md.params.back();
                            // Build a data list of the remaining arg nodes as a quote
                            // Create () as the base, then cons each remaining arg
                            std::vector<aura::ast::NodeId> remaining;
                            for (std::size_t ai = regular_count + 1; ai < v.children.size(); ++ai)
                                remaining.push_back(v.child(ai));
                            // Create nested quote: (quote (arg1 arg2 ...)) using add_call chains
                            // Actually, clone_macro_body substitutes Variable nodes, so we need
                            // the rest arg as an expression node, not data.
                            // For simplicity: build a (begin remaining...) — but that evaluates
                            // them. Build as (quote (arg1 arg2 ...)) by creating a proper list:
                            // cons arg1 (cons arg2 (cons ... ())) then wrap in quote
                            // Since these are NodeIds in the SAME FlatAST, we can build an
                            // expression that produces a list: (list arg1 arg2 ...)
                            auto list_var = flat.add_variable(pool.intern("list"));
                            std::vector<aura::ast::NodeId> all_args;
                            all_args.push_back(list_var);
                            all_args.insert(all_args.end(), remaining.begin(), remaining.end());
                            auto list_call = flat.add_call(list_var, all_args);
                            // But this would be (list arg1 arg2 ...) which EVALUATES the args.
                            // Macros need syntax (unevaluated). We need (quote (arg1 arg2 ...)).
                            // Create a quoted version: for each remaining arg, convert to data via
                            // ast_to_data... but we don't have access to the evaluator's pairs_.
                            // For now: just use the (list ...) approach and note that rest args
                            // in macro_expand_all will be evaluated (same as the evaluator's
                            // version) Actually this is the same issue as the evaluator's macros_
                            // expansion. The difference: in macro_expand_all we can directly
                            // substitute. Let me just not handle rest in macro_expand_all for now —
                            // the evaluator's macros_ handles it correctly. This path is only for
                            // same-expression macros.
                        }
                        // Clone macro body with substitution
                        std::unordered_map<std::string, std::string> rename_map;
                        auto expanded = clone_macro_body(
                            flat, pool, *md.src_flat, *md.src_pool, md.body_id, &subst, &rename_map,
                            /*cloned_marker=*/aura::ast::SyntaxMarker::MacroIntroduced);
                        if (expanded != NULL_NODE) {
                            if (id == root)
                                new_root = expanded;
                            expanded_any = true;
                        }
                    }
                }
            }
        }

        if (!expanded_any)
            return root;
        root = new_root;
    }
    // Issue #121: hit the pass limit with macros still in the
    // tree. Emit a warning so the user knows the result is
    // partial. This is the user-facing equivalent of the
    // solver TIMEOUT pattern from Issue #118.
    if (root != NULL_NODE) {
        std::println(std::cerr,
                     "warning: macro_expand_all hit pass limit ({}); "
                     "the result may have unexpanded macro calls",
                     max_passes);
    }
    return root;
}
} // namespace aura::compiler::macro_exp
