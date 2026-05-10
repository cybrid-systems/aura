module aura.compiler.frontend;
import std;

namespace aura::compiler {

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
    if (!e) return {false, 0, "null"};
    return std::visit([&](const auto& n) -> EvalResult {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ast::LiteralIntNode>)
            return {true, n.value, ""};
        if constexpr (std::is_same_v<T, ast::VariableNode>) {
            auto v = env.lookup(n.name);
            if (v.has_value()) return {true, *v, ""};
            return {false, 0, "unbound variable: " + n.name};
        }
        if constexpr (std::is_same_v<T, ast::CallNode>) {
            if (auto* lam = std::get_if<ast::LambdaNode>(&n.function->payload)) {
                Env ne(&env);
                ne.set_primitives(&primitives_);
                for (std::size_t i = 0; i < lam->params.size() && i < n.args.size(); ++i) {
                    auto a = eval_in(n.args[i], env);
                    if (!a.success) return a;
                    ne.bind(lam->params[i], a.int_value);
                }
                return eval_in(lam->body, ne);
            }
            if (auto* var = std::get_if<ast::VariableNode>(&n.function->payload)) {
                auto p = env.lookup_primitive(var->name);
                if (p.has_value()) {
                    std::vector<std::int64_t> va;
                    for (auto* a : n.args) {
                        auto r = eval_in(a, env);
                        if (!r.success) return r;
                        va.push_back(r.int_value);
                    }
                    return {true, (*p)(va), ""};
                }
            }
            auto fr = eval_in(n.function, env);
            if (!fr.success) return fr;
            if (static_cast<std::uint64_t>(fr.int_value) >= CLOSURE_SENTINEL)
                return apply_closure(static_cast<ClosureId>(fr.int_value - CLOSURE_SENTINEL), n.args, env);
            return {false, 0, "not callable"};
        }
        if constexpr (std::is_same_v<T, ast::IfExprNode>) {
            auto c = eval_in(n.condition, env);
            if (!c.success) return c;
            return eval_in(c.int_value ? n.then_branch : n.else_branch, env);
        }
        if constexpr (std::is_same_v<T, ast::LambdaNode>) {
            auto* cap = copy_env(env);
            auto id = next_id();
            closures_[id] = {n.params, n.body, cap};
            return {true, static_cast<std::int64_t>(CLOSURE_SENTINEL + id), ""};
        }
        if constexpr (std::is_same_v<T, ast::LetNode>) {
            auto v = eval_in(n.value, env);
            if (!v.success) return v;
            Env ne(&env);
            ne.set_primitives(&primitives_);
            ne.bind(n.name, v.int_value);
            return eval_in(n.body, ne);
        }
        if constexpr (std::is_same_v<T, ast::DefineNode>) {
            Env& me = const_cast<Env&>(env);
            me.set_cells(&cells_);
            auto ci = alloc_cell(0);
            me.bind(n.name, static_cast<std::int64_t>(CELL_SENTINEL + ci));
            auto v = eval_in(n.value, env);
            if (!v.success) return v;
            cells_[ci] = v.int_value;
            return v;
        }
        if constexpr (std::is_same_v<T, ast::LetRecNode>) {
            Env ne(&env);
            ne.set_primitives(&primitives_);
            ne.set_cells(&cells_);
            auto ci = alloc_cell(0);
            ne.bind(n.name, static_cast<std::int64_t>(CELL_SENTINEL + ci));
            auto v = eval_in(n.value, ne);
            if (!v.success) return v;
            cells_[ci] = v.int_value;
            return eval_in(n.body, ne);
        }
        return {false, 0, "unknown"};
    }, e->payload);
}

EvalResult Evaluator::apply_closure(ClosureId id, const std::vector<ast::Expr*>& args, const Env& call_env) {
    auto it = closures_.find(id);
    if (it == closures_.end()) return {false, 0, "invalid closure"};
    auto& cl = it->second;
    Env ne(cl.env ? cl.env : &top_);
    ne.set_primitives(&primitives_);
    for (std::size_t i = 0; i < cl.params.size() && i < args.size(); ++i) {
        auto v = eval_in(args[i], call_env);
        if (!v.success) return v;
        ne.bind(cl.params[i], v.int_value);
    }
    return eval_in(cl.body, ne);
}

} // namespace aura::compiler
