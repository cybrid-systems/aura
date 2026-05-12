module aura.compiler.frontend;
import std;

namespace aura::compiler {

using namespace aura::diag;

std::optional<std::int64_t> Env::lookup(const std::string& n) const {
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
        if (it->first == n) {
            auto v = it->second;
            if (cells_ && static_cast<std::uint64_t>(v) >= CELL_SENTINEL) {
                auto idx = static_cast<std::size_t>(v - CELL_SENTINEL);
                if (idx < cells_->size()) return (*cells_)[idx];
            }
            return v;
        }
    return parent_ ? parent_->lookup(n) : std::nullopt;
}

Primitives::Primitives() {
    table_["+"]  = [](auto& a) { return a[0] + a[1]; };
    table_["-"]  = [](auto& a) { return a.size() == 1 ? -a[0] : a[0] - a[1]; };
    table_["*"]  = [](auto& a) { return a[0] * a[1]; };
    table_["/"]  = [](auto& a) { return a[0] / a[1]; };
    table_["="]  = [](auto& a) { return a[0] == a[1]; };
    table_["<"]  = [](auto& a) { return a[0] < a[1]; };
    table_[">"]  = [](auto& a) { return a[0] > a[1]; };
    table_["<="] = [](auto& a) { return a[0] <= a[1]; };
    table_[">="] = [](auto& a) { return a[0] >= a[1]; };
    // Ghuloum Step 9: booleans
    table_["not"]  = [](auto& a) { return a[0] == 0 ? TRUE_VAL : FALSE_VAL; };
    table_["and"]  = [](auto& a) { return a[0] && a[1] ? TRUE_VAL : FALSE_VAL; };
    table_["or"]   = [](auto& a) { return a[0] || a[1] ? TRUE_VAL : FALSE_VAL; };
    table_["eq?"]  = [](auto& a) { return a[0] == a[1] ? TRUE_VAL : FALSE_VAL; };
}

void Evaluator::init_pair_primitives() {
    // Ghuloum Step 10: pairs — capture pairs_ by reference
    primitives_.add("cons", [this](const auto& a) {
        auto id = pairs_.size();
        pairs_.push_back({a[0], a[1]});
        return PAIR_SENTINEL + static_cast<std::int64_t>(id);
    });
    primitives_.add("car", [this](const auto& a) {
        auto id = static_cast<std::size_t>(a[0] - PAIR_SENTINEL);
        return id < pairs_.size() ? pairs_[id].car : 0;
    });
    primitives_.add("cdr", [this](const auto& a) {
        auto id = static_cast<std::size_t>(a[0] - PAIR_SENTINEL);
        return id < pairs_.size() ? pairs_[id].cdr : 0;
    });
    primitives_.add("pair?", [](const auto& a) {
        return static_cast<std::uint64_t>(a[0]) >= static_cast<std::uint64_t>(PAIR_SENTINEL) ? TRUE_VAL : FALSE_VAL;
    });
    primitives_.add("null?", [](const auto& a) {
        return a[0] == 0 ? TRUE_VAL : FALSE_VAL;
    });
}

std::int64_t* Env::lookup_cell_ptr(const std::string& n, std::vector<std::int64_t>* cells) const {
    if (!cells) return nullptr;
    for (auto& b : bindings_) {
        if (b.first == n) {
            auto cv = b.second;
            if (static_cast<std::uint64_t>(cv) >= static_cast<std::uint64_t>(CELL_SENTINEL)) {
                auto ci = static_cast<std::size_t>(cv - CELL_SENTINEL);
                if (ci < cells->size()) return &(*cells)[ci];
            }
            return nullptr;
        }
    }
    // Walk up the parent chain
    for (auto* p = parent_; p; p = p->parent_) {
        for (auto& b : p->bindings_) {
            if (b.first == n) {
                auto cv = b.second;
                if (static_cast<std::uint64_t>(cv) >= static_cast<std::uint64_t>(CELL_SENTINEL)) {
                    auto ci = static_cast<std::size_t>(cv - CELL_SENTINEL);
                    if (ci < cells->size()) return &(*cells)[ci];
                }
                return nullptr;
            }
        }
    }
    return nullptr;
}

std::optional<PrimFn> Primitives::lookup(const std::string& n) const {
    auto i = table_.find(n);
    return i != table_.end() ? std::optional(i->second) : std::nullopt;
}

Evaluator::Evaluator() {
    top_.set_primitives(&primitives_);
    init_pair_primitives();
}

Env* Evaluator::copy_env(const Env& e) {
    return arena_ ? arena_->create<Env>(e) : nullptr;
}

EvalResult Evaluator::eval_in(const ast::Expr* e, const Env& env) {
    if (!e) return std::unexpected(Diagnostic{ErrorKind::InternalError, "null expression"});
    return std::visit([&](const auto& n) -> EvalResult {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ast::LiteralIntNode>)
            return n.value;
        if constexpr (std::is_same_v<T, ast::VariableNode>) {
            auto v = env.lookup(n.name);
            if (v.has_value()) return *v;
            return std::unexpected(Diagnostic{ErrorKind::UnboundVariable,
                                              "unbound variable: " + n.name});
        }
        if constexpr (std::is_same_v<T, ast::CallNode>) {
            // Inline lambda application
            if (auto* lam = std::get_if<ast::LambdaNode>(&n.function->payload)) {
                Env ne(&env);
                ne.set_primitives(&primitives_);
                for (std::size_t i = 0; i < lam->params.size() && i < n.args.size(); ++i) {
                    auto a = eval_in(n.args[i], env);
                    if (!a) return a;
                    ne.bind(lam->params[i], *a);
                }
                return eval_in(lam->body, ne);
            }
            // Macro expansion
            if (auto* var = std::get_if<ast::VariableNode>(&n.function->payload)) {
                auto macro_it = macros_.find(var->name);
                if (macro_it != macros_.end()) {
                    if (macro_it->second.params.size() != n.args.size())
                        return std::unexpected(Diagnostic{ErrorKind::ArityMismatch,
                            "macro " + var->name + ": expected " +
                            std::to_string(macro_it->second.params.size()) +
                            " args, got " + std::to_string(n.args.size())});
                    auto* expanded = expand_macro(var->name, n.args);
                    return eval_in(expanded, env);
                }
            }
            // Primitive call
            if (auto* var = std::get_if<ast::VariableNode>(&n.function->payload)) {
                auto p = env.lookup_primitive(var->name);
                if (p.has_value()) {
                    std::vector<std::int64_t> va;
                    for (auto* a : n.args) {
                        auto r = eval_in(a, env);
                        if (!r) return r;
                        va.push_back(*r);
                    }
                    return (*p)(va);
                }
            }
            // Closure call
            auto fr = eval_in(n.function, env);
            if (!fr) return fr;
            if (static_cast<std::uint64_t>(*fr) >= CLOSURE_SENTINEL)
                return apply_closure(static_cast<ClosureId>(*fr - CLOSURE_SENTINEL), n.args, env);
            return std::unexpected(Diagnostic{ErrorKind::InvalidClosure, "not callable"});
        }
        if constexpr (std::is_same_v<T, ast::IfExprNode>) {
            auto c = eval_in(n.condition, env);
            if (!c) return c;
            return eval_in(*c ? n.then_branch : n.else_branch, env);
        }
        if constexpr (std::is_same_v<T, ast::LambdaNode>) {
            auto* cap = copy_env(env);
            auto id = next_id();
            closures_[id] = {n.params, n.body, cap};
            return static_cast<std::int64_t>(CLOSURE_SENTINEL + id);
        }
        if constexpr (std::is_same_v<T, ast::LetNode>) {
            auto v = eval_in(n.value, env);
            if (!v) return v;
            Env ne(&env);
            ne.set_primitives(&primitives_);
            ne.bind(n.name, *v);
            return eval_in(n.body, ne);
        }
        if constexpr (std::is_same_v<T, ast::DefineNode>) {
            Env& me = const_cast<Env&>(env);
            me.set_cells(&cells_);
            auto ci = alloc_cell(0);
            me.bind(n.name, static_cast<std::int64_t>(CELL_SENTINEL + ci));
            auto v = eval_in(n.value, env);
            if (!v) return v;
            cells_[ci] = *v;
            return v;
        }
        if constexpr (std::is_same_v<T, ast::LetRecNode>) {
            Env ne(&env);
            ne.set_primitives(&primitives_);
            ne.set_cells(&cells_);
            auto ci = alloc_cell(0);
            ne.bind(n.name, static_cast<std::int64_t>(CELL_SENTINEL + ci));
            auto v = eval_in(n.value, ne);
            if (!v) return v;
            cells_[ci] = *v;
            return eval_in(n.body, ne);
        }
        // Ghuloum Steps 11-13
        if constexpr (std::is_same_v<T, ast::BeginNode>) {
            ast::Expr* last = nullptr;
            for (auto* e : n.exprs) {
                auto v = eval_in(e, env);
                if (!v) return v;
                last = e;
            }
            return last ? eval_in(last, env) : EvalResult(0);
        }
        if constexpr (std::is_same_v<T, ast::SetNode>) {
            auto v = eval_in(n.value, env);
            if (!v) return v;
            // Try cell-based mutation first (letrec/define bindings)
            auto* cell_ptr = env.lookup_cell_ptr(n.name, &cells_);
            if (cell_ptr) {
                *cell_ptr = *v;
                return *v;
            }
            // Fallback: direct env mutation (let bindings)
            // Walk the env chain and mutate the first matching binding
            auto& write_env = const_cast<Env&>(env);
            for (auto& b : write_env.bindings()) {
                if (b.first == n.name) {
                    b.second = *v;
                    return *v;
                }
            }
            for (auto* p = env.parent(); p; p = p->parent()) {
                auto& parent_write = const_cast<Env&>(*p);
                for (auto& b : parent_write.bindings()) {
                    if (b.first == n.name) {
                        b.second = *v;
                        return *v;
                    }
                }
            }
            return std::unexpected(Diagnostic{ErrorKind::UnboundVariable,
                                              "set!: unbound variable: " + n.name});
        }
        if constexpr (std::is_same_v<T, ast::QuoteNode>) {
            return eval_in(n.value, env);
        }
        if constexpr (std::is_same_v<T, ast::MacroDefNode>) {
            // Clone the body into a persistent CloneNode-backed copy.
            // The original body is in arena_ which gets reset between evals.
            // We must deep-copy it now while arena_ is still alive.
            // Simplest approach: serialize → deserialize via expand_macro's clone
            // Keep the macro in a persistent AST by cloning into a static arena.
            static ast::ASTArena persistent_arena(64 * 1024);
            struct Cloner {
                ast::ASTArena* pa;
                ast::Expr* clone(ast::Expr* e) {
                    return std::visit([this](const auto& node) -> ast::Expr* {
                        using T = std::decay_t<decltype(node)>;
                        if constexpr (std::is_same_v<T, ast::LiteralIntNode>)
                            return pa->create<ast::Expr>(node);
                        if constexpr (std::is_same_v<T, ast::VariableNode>)
                            return pa->create<ast::Expr>(node);
                        if constexpr (std::is_same_v<T, ast::CallNode>) {
                            ast::CallNode c{node.tag, clone(node.function), {}};
                            for (auto* a : node.args) c.args.push_back(clone(a));
                            return pa->create<ast::Expr>(std::move(c));
                        }
                        if constexpr (std::is_same_v<T, ast::IfExprNode>)
                            return pa->create<ast::Expr>(ast::IfExprNode{node.tag, clone(node.condition), clone(node.then_branch), clone(node.else_branch)});
                        if constexpr (std::is_same_v<T, ast::LambdaNode>) {
                            ast::LambdaNode l{node.tag, node.params, clone(node.body)};
                            return pa->create<ast::Expr>(std::move(l));
                        }
                        if constexpr (std::is_same_v<T, ast::LetNode>)
                            return pa->create<ast::Expr>(ast::LetNode{node.tag, node.name, clone(node.value), clone(node.body)});
                        if constexpr (std::is_same_v<T, ast::LetRecNode>)
                            return pa->create<ast::Expr>(ast::LetRecNode{node.tag, node.name, clone(node.value), clone(node.body)});
                        if constexpr (std::is_same_v<T, ast::DefineNode>)
                            return pa->create<ast::Expr>(ast::DefineNode{node.tag, node.name, clone(node.value)});
                        if constexpr (std::is_same_v<T, ast::BeginNode>) {
                            ast::BeginNode b{node.tag, {}};
                            for (auto* e : node.exprs) b.exprs.push_back(clone(e));
                            return pa->create<ast::Expr>(std::move(b));
                        }
                        if constexpr (std::is_same_v<T, ast::SetNode>)
                            return pa->create<ast::Expr>(ast::SetNode{node.tag, node.name, clone(node.value)});
                        if constexpr (std::is_same_v<T, ast::QuoteNode>)
                            return pa->create<ast::Expr>(ast::QuoteNode{node.tag, clone(node.value)});
                        return nullptr;
                    }, e->payload);
                }
            };
            Cloner cloner{&persistent_arena};
            auto* persistent_body = cloner.clone(n.body);
            const_cast<Evaluator*>(this)->macros_[n.name] = MacroDef{n.params, persistent_body};
            return 0;
        }
        return std::unexpected(Diagnostic{ErrorKind::InternalError, "unknown expression type"});
    }, e->payload);
}

EvalResult Evaluator::apply_closure(ClosureId id, const std::vector<ast::Expr*>& args, const Env& call_env) {
    auto it = closures_.find(id);
    if (it == closures_.end())
        return std::unexpected(Diagnostic{ErrorKind::InvalidClosure, "invalid closure"});
    auto& cl = it->second;
    Env ne(cl.env ? cl.env : &top_);
    ne.set_primitives(&primitives_);
    for (std::size_t i = 0; i < cl.params.size() && i < args.size(); ++i) {
        auto v = eval_in(args[i], call_env);
        if (!v) return v;
        ne.bind(cl.params[i], *v);
    }
    return eval_in(cl.body, ne);
}



// ── Macro expansion: substitute params with args in body AST ──
ast::Expr* Evaluator::expand_macro(const std::string& name,
                                    const std::vector<ast::Expr*>& args) {
    auto it = macros_.find(name);
    if (it == macros_.end()) return nullptr;
    auto& mac = it->second;

    // Clone the body AST, replacing VariableNodes that match params
    // with the corresponding argument expressions.
    // Uses a recursive walk with arena allocation.
    struct Expander {
        ast::ASTArena* arena;
        const std::vector<std::string>* params;
        const std::vector<ast::Expr*>* args;

        ast::Expr* clone(ast::Expr* expr) {
            return std::visit([this](const auto& node) -> ast::Expr* {
                using T = std::decay_t<decltype(node)>;

                if constexpr (std::is_same_v<T, ast::VariableNode>) {
                    // Check if this variable matches a macro parameter
                    for (std::size_t i = 0; i < params->size(); ++i) {
                        if (node.name == (*params)[i]) {
                            // Substitute: return cloned arg
                            return clone((*args)[i]);
                        }
                    }
                    // Not a param — clone as-is
                    return arena->template create<ast::Expr>(ast::VariableNode{node.tag, node.name});
                }

                if constexpr (std::is_same_v<T, ast::LiteralIntNode>) {
                    return arena->template create<ast::Expr>(ast::LiteralIntNode{node.tag, node.value});
                }

                if constexpr (std::is_same_v<T, ast::CallNode>) {
                    ast::CallNode call{node.tag, clone(node.function), {}};
                    for (auto* a : node.args)
                        call.args.push_back(clone(a));
                    return arena->template create<ast::Expr>(std::move(call));
                }

                if constexpr (std::is_same_v<T, ast::IfExprNode>) {
                    return arena->template create<ast::Expr>(
                        ast::IfExprNode{node.tag, clone(node.condition), clone(node.then_branch), clone(node.else_branch)});
                }

                if constexpr (std::is_same_v<T, ast::LambdaNode>) {
                    ast::LambdaNode lam{node.tag, node.params, clone(node.body)};
                    return arena->template create<ast::Expr>(std::move(lam));
                }

                if constexpr (std::is_same_v<T, ast::LetNode>) {
                    return arena->template create<ast::Expr>(
                        ast::LetNode{node.tag, node.name, clone(node.value), clone(node.body)});
                }

                if constexpr (std::is_same_v<T, ast::LetRecNode>) {
                    return arena->template create<ast::Expr>(
                        ast::LetRecNode{node.tag, node.name, clone(node.value), clone(node.body)});
                }

                if constexpr (std::is_same_v<T, ast::DefineNode>) {
                    return arena->template create<ast::Expr>(
                        ast::DefineNode{node.tag, node.name, clone(node.value)});
                }

                if constexpr (std::is_same_v<T, ast::BeginNode>) {
                    ast::BeginNode begin{node.tag, {}};
                    for (auto* e : node.exprs)
                        begin.exprs.push_back(clone(e));
                    return arena->template create<ast::Expr>(std::move(begin));
                }

                if constexpr (std::is_same_v<T, ast::SetNode>) {
                    return arena->template create<ast::Expr>(
                        ast::SetNode{node.tag, node.name, clone(node.value)});
                }

                if constexpr (std::is_same_v<T, ast::QuoteNode>) {
                    return arena->template create<ast::Expr>(
                        ast::QuoteNode{node.tag, clone(node.value)});
                }

                return nullptr;
            }, expr->payload);
        }
    };

    Expander expander{arena_, &mac.params, &args};
    return expander.clone(mac.body);
}

} // namespace aura::compiler
