// service_workspace_lambda.cpp - CompilerService::workspace_lambda_free_name_unresolved
// moved out of the module interface (#5242). Move-only: the body is unchanged.

module;

module aura.compiler.service;

import std;
import aura.core.ast;           // FlatAST / StringPool / NodeTag
import aura.compiler.evaluator; // Primitives

namespace aura::compiler {

[[nodiscard]] bool CompilerService::workspace_lambda_free_name_unresolved(
    const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool, aura::ast::NodeId root,
    const std::string& self_name) const {
    struct Walk {
        const aura::ast::FlatAST& f;
        const aura::ast::StringPool& p;
        const std::string& self_name;
        const Primitives& prims;
        const decltype(ir_value_cell_bindings_)& cells;
        const decltype(ir_cache_)& cache;
        bool hit = false;
        bool in_try = false;
        std::vector<std::unordered_set<std::string>> scopes;

        [[nodiscard]] bool local_bound(const std::string& nm) const {
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                if (it->count(nm) != 0)
                    return true;
            }
            return false;
        }
        [[nodiscard]] bool resolvable(const std::string& nm) const {
            if (nm.empty() || nm[0] == ':')
                return true;
            if (local_bound(nm) || nm == self_name)
                return true;
            if (prims.slot_for_name(nm) < prims.slot_count())
                return true;
            if (cells.count(nm) != 0 || cache.count(nm) != 0)
                return true;
            return false;
        }
        static bool syntax_callee(std::string_view n) {
            return n == "list" || n == "with-arena" || n == "performance-region" ||
                   n == "evolution-region" || n == "try" || n == "require" || n == "import" ||
                   n == "hash-ref" || n == "hash-remove!" || n == "hash-set!" || n == "cond" ||
                   n == "not" || n == "and" || n == "or" || n == "while";
        }
        void note_var(std::string nm) {
            if (!resolvable(nm))
                hit = true;
        }
        void walk(aura::ast::NodeId id) {
            if (hit || id == aura::ast::NULL_NODE || id >= f.size() || f.is_free_slot(id))
                return;
            auto nv = f.get(id);
            if (nv.tag == aura::ast::NodeTag::Quote)
                return;
            if (nv.tag == aura::ast::NodeTag::Lambda) {
                scopes.push_back({});
                for (auto pid : nv.params) {
                    auto pn = std::string(p.resolve(pid));
                    if (!pn.empty())
                        scopes.back().insert(std::move(pn));
                }
                for (auto c : nv.children)
                    walk(c);
                scopes.pop_back();
                return;
            }
            if (nv.tag == aura::ast::NodeTag::Let || nv.tag == aura::ast::NodeTag::LetRec) {
                const bool rec = nv.tag == aura::ast::NodeTag::LetRec;
                std::string nm;
                if (nv.sym_id != aura::ast::INVALID_SYM)
                    nm = std::string(p.resolve(nv.sym_id));
                const bool named = !nm.empty();
                if (rec && named) {
                    scopes.push_back({});
                    scopes.back().insert(nm);
                }
                if (!nv.children.empty())
                    walk(nv.child(0));
                if (!rec && named) {
                    scopes.push_back({});
                    scopes.back().insert(std::move(nm));
                }
                for (std::size_t i = 1; i < nv.children.size(); ++i)
                    walk(nv.child(i));
                if (named)
                    scopes.pop_back();
                return;
            }
            if (nv.tag == aura::ast::NodeTag::Begin) {
                scopes.push_back({});
                for (auto c : nv.children) {
                    if (c >= f.size() || f.is_free_slot(c))
                        continue;
                    auto cv = f.get(c);
                    if (cv.tag != aura::ast::NodeTag::Define || cv.sym_id == aura::ast::INVALID_SYM)
                        continue;
                    auto dn = std::string(p.resolve(cv.sym_id));
                    if (!dn.empty())
                        scopes.back().insert(std::move(dn));
                }
                for (auto c : nv.children)
                    walk(c);
                scopes.pop_back();
                return;
            }
            if (nv.tag == aura::ast::NodeTag::Define) {
                if (scopes.empty())
                    scopes.push_back({});
                if (nv.sym_id != aura::ast::INVALID_SYM) {
                    auto dn = std::string(p.resolve(nv.sym_id));
                    if (!dn.empty())
                        scopes.back().insert(std::move(dn));
                }
                for (auto c : nv.children)
                    walk(c);
                return;
            }
            if (nv.tag == aura::ast::NodeTag::Call && !nv.children.empty()) {
                std::string cname;
                const auto callee_id = nv.child(0);
                if (callee_id < f.size() && !f.is_free_slot(callee_id)) {
                    auto cal = f.get(callee_id);
                    if (cal.tag == aura::ast::NodeTag::Variable)
                        cname = std::string(p.resolve(cal.sym_id));
                }
                if (cname == "try") {
                    const bool saved = in_try;
                    in_try = true;
                    for (std::size_t i = 1; i < nv.children.size(); ++i)
                        walk(nv.child(i));
                    in_try = saved;
                    return;
                }
                if (cname == "catch" && in_try) {
                    // Issue #4594: (catch EXPR) is the handler expression,
                    // not a binder. A free name in EXPR must be noted.
                    if (nv.children.size() == 2) {
                        walk(nv.child(1));
                        return;
                    }
                    std::string var_name;
                    if (nv.children.size() >= 2) {
                        const auto binder_id = nv.child(1);
                        if (binder_id < f.size() && !f.is_free_slot(binder_id)) {
                            auto bf = f.get(binder_id);
                            if (bf.tag == aura::ast::NodeTag::Variable) {
                                var_name = std::string(p.resolve(bf.sym_id));
                            } else if (bf.tag == aura::ast::NodeTag::Call && !bf.children.empty()) {
                                const auto vn_id = bf.child(0);
                                if (vn_id < f.size() && !f.is_free_slot(vn_id)) {
                                    auto vn = f.get(vn_id);
                                    if (vn.tag == aura::ast::NodeTag::Variable)
                                        var_name = std::string(p.resolve(vn.sym_id));
                                }
                            }
                        }
                    }
                    scopes.push_back({});
                    if (!var_name.empty())
                        scopes.back().insert(std::move(var_name));
                    for (std::size_t i = 2; i < nv.children.size(); ++i)
                        walk(nv.child(i));
                    scopes.pop_back();
                    return;
                }
                if (syntax_callee(cname)) {
                    for (std::size_t i = 1; i < nv.children.size(); ++i)
                        walk(nv.child(i));
                    return;
                }
            }
            if (nv.tag == aura::ast::NodeTag::Variable) {
                note_var(std::string(p.resolve(nv.sym_id)));
                return;
            }
            for (auto c : nv.children)
                walk(c);
        }
    };
    Walk w{flat, pool, self_name, evaluator_.primitives(), ir_value_cell_bindings_, ir_cache_};
    w.walk(root);
    return w.hit;
}

} // namespace aura::compiler
