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
}

std::optional<PrimFn> Primitives::lookup(const std::string& n) const {
    auto i = table_.find(n);
    return i != table_.end() ? std::optional(i->second) : std::nullopt;
}

Evaluator::Evaluator() { top_.set_primitives(&primitives_); }

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

} // namespace aura::compiler
