#pragma once

// Issue #4936 B4 / #142: replace-subtree capture and the small rollback printer.
// Both used to be local std::function lambdas that called themselves. They do
// not escape the call. The walks are deducing this.
//
// collect_outer_bindings was declared next to this logic and never called.
// Outer bindings are the flat scan: every Lambda / Let / LetRec name whose
// node is outside `root`.

// Include only after `import std;` and `import aura.core.ast;`. A standard
// header here would be parsed twice against the std module.

namespace aura::compiler {

template <class Flat>
std::vector<aura::ast::SymId>
replace_subtree_captured_syms(const Flat& flat,
                              const std::unordered_set<aura::ast::SymId>& builtin_syms,
                              aura::ast::NodeId root) {
    using NodeId = aura::ast::NodeId;
    std::vector<NodeId> captured;
    std::unordered_set<NodeId> new_bindings;
    auto collect_new_bindings = [&](this auto&& self, NodeId id) {
        if (id >= flat.size() || id == aura::ast::NULL_NODE)
            return;
        auto n = flat.get(id);
        if (n.tag == aura::ast::NodeTag::Lambda) {
            for (auto p : n.params)
                new_bindings.insert(p);
        } else if ((n.tag == aura::ast::NodeTag::Let || n.tag == aura::ast::NodeTag::LetRec) &&
                   n.has_name()) {
            new_bindings.insert(n.sym_id);
        }
        for (auto c : n.children)
            self(c);
    };
    collect_new_bindings(root);

    std::unordered_set<NodeId> in_new_subtree;
    auto mark_new = [&](this auto&& self, NodeId id) {
        if (id >= flat.size() || id == aura::ast::NULL_NODE)
            return;
        in_new_subtree.insert(id);
        auto n = flat.get(id);
        for (auto c : n.children)
            self(c);
    };
    mark_new(root);

    std::unordered_set<NodeId> outer_bindings;
    for (NodeId id = 0; id < flat.size(); ++id) {
        if (in_new_subtree.count(id))
            continue;
        auto n = flat.get(id);
        if (n.tag == aura::ast::NodeTag::Lambda) {
            for (auto p : n.params)
                outer_bindings.insert(p);
        } else if ((n.tag == aura::ast::NodeTag::Let || n.tag == aura::ast::NodeTag::LetRec) &&
                   n.has_name()) {
            outer_bindings.insert(n.sym_id);
        }
    }

    auto find_captured = [&](this auto&& self, NodeId id) {
        if (id >= flat.size() || id == aura::ast::NULL_NODE)
            return;
        auto n = flat.get(id);
        if (n.tag == aura::ast::NodeTag::Variable) {
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
            self(c);
    };
    find_captured(root);
    return captured;
}

// The small printer replace-subtree stores as the displaced node's text.
// It is not the larger query-and-replace / replace-pattern printers.
template <class Flat, class Pool>
std::string replace_subtree_old_source(const Flat& flat, const Pool& pool, aura::ast::NodeId root) {
    using NodeId = aura::ast::NodeId;
    auto src = [&](this auto&& self, NodeId id) -> std::string {
        if (id >= flat.size() || id == aura::ast::NULL_NODE)
            return "";
        auto n = flat.get(id);
        switch (n.tag) {
            case aura::ast::NodeTag::LiteralInt:
                return std::to_string(n.int_value);
            case aura::ast::NodeTag::LiteralFloat:
                return std::to_string(n.float_value);
            case aura::ast::NodeTag::LiteralString:
                return "\"" + std::string(pool.resolve(n.sym_id)) + "\"";
            case aura::ast::NodeTag::Variable:
                return std::string(pool.resolve(n.sym_id));
            case aura::ast::NodeTag::Call: {
                std::string s = "(";
                for (std::size_t ci = 0; ci < n.children.size(); ++ci) {
                    if (ci > 0)
                        s += " ";
                    s += self(n.child(ci));
                }
                s += ")";
                return s;
            }
            case aura::ast::NodeTag::Lambda: {
                std::string s = "(lambda (";
                for (std::size_t pi = 0; pi < n.params.size(); ++pi) {
                    if (pi > 0)
                        s += " ";
                    s += pool.resolve(n.params[pi]);
                }
                s += ")";
                if (!n.children.empty())
                    s += " " + self(n.child(0));
                s += ")";
                return s;
            }
            case aura::ast::NodeTag::IfExpr: {
                std::string s = "(if";
                if (n.children.size() > 0)
                    s += " " + self(n.child(0));
                if (n.children.size() > 1)
                    s += " " + self(n.child(1));
                if (n.children.size() > 2)
                    s += " " + self(n.child(2));
                return s + ")";
            }
            case aura::ast::NodeTag::Begin: {
                std::string s = "(begin";
                for (auto c : n.children)
                    s += " " + self(c);
                return s + ")";
            }
            case aura::ast::NodeTag::Define: {
                std::string s = "(define " + std::string(pool.resolve(n.sym_id));
                if (!n.children.empty())
                    s += " " + self(n.child(0));
                return s + ")";
            }
            default:
                return std::string("#<node-") + std::to_string(static_cast<int>(n.tag)) + ">";
        }
    };
    return src(root);
}

} // namespace aura::compiler
