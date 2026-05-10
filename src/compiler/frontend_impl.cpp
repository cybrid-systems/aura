module;
#include <cstdint>
#include <string>
#include <variant>
#include <functional>
#include <optional>
#include <vector>

module aura.compiler.frontend;

namespace aura::compiler {

Primitives::Primitives() {
    table_["+"]  = [](const auto& a) { return a[0] + a[1]; };
    table_["-"]  = [](const auto& a) { return a.size() == 1 ? -a[0] : a[0] - a[1]; };
    table_["*"]  = [](const auto& a) { return a[0] * a[1]; };
    table_["/"]  = [](const auto& a) { return a[0] / a[1]; };
    table_["="]  = [](const auto& a) { return a[0] == a[1]; };
    table_["<"]  = [](const auto& a) { return a[0] < a[1]; };
    table_[">"]  = [](const auto& a) { return a[0] > a[1]; };
    table_["<="] = [](const auto& a) { return a[0] <= a[1]; };
    table_[">="] = [](const auto& a) { return a[0] >= a[1]; };
}

std::optional<PrimitiveFn> Primitives::lookup(const std::string& name) const {
    auto it = table_.find(name);
    return it != table_.end() ? std::optional(it->second) : std::nullopt;
}

Evaluator::Evaluator() {
    top_.set_primitives(&primitives_);
}

Env* Evaluator::copy_env(const Env& env) {
    if (!arena_) return nullptr;
    return arena_->create<Env>(env);
}

EvalResult Evaluator::eval_in(const ast::Expr* expr, const Env& env) {
    if (!expr) return {false, 0, "null expression"};

    return std::visit([&](const auto& node) -> EvalResult {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::LiteralIntNode>)
            return {true, node.value, ""};

        if constexpr (std::is_same_v<T, ast::VariableNode>) {
            auto val = env.lookup(node.name);
            if (val.has_value()) return {true, *val, ""};
            return {false, 0, "unbound variable: " + node.name};
        }

        if constexpr (std::is_same_v<T, ast::CallNode>) {
            // 1. Inline lambda: ((lambda (x) ...) arg)
            if (auto* lam = std::get_if<ast::LambdaNode>(&node.function->payload)) {
                Env new_env(&env);
                new_env.set_primitives(&primitives_);
                for (size_t i = 0; i < lam->params.size() && i < node.args.size(); ++i) {
                    auto a = eval_in(node.args[i], env);
                    if (!a.success) return a;
                    new_env.bind(lam->params[i], a.int_value);
                }
                return eval_in(lam->body, new_env);
            }

            // 2. Primitive call: (+ a b)
            if (auto* var = std::get_if<ast::VariableNode>(&node.function->payload)) {
                auto prim = env.lookup_primitive(var->name);
                if (prim.has_value()) {
                    std::vector<int64_t> vals;
                    for (auto* a : node.args) {
                        auto r = eval_in(a, env);
                        if (!r.success) return r;
                        vals.push_back(r.int_value);
                    }
                    return {true, (*prim)(vals), ""};
                }
            }

            // 3. Computed function: evaluate and apply as closure
            auto fn_result = eval_in(node.function, env);
            if (!fn_result.success) return fn_result;

            if (static_cast<uint64_t>(fn_result.int_value) >= CLOSURE_SENTINEL) {
                return apply_closure(
                    static_cast<ClosureId>(fn_result.int_value - CLOSURE_SENTINEL),
                    node.args, env);
            }

            return {false, 0, "not callable"};
        }

        if constexpr (std::is_same_v<T, ast::IfExprNode>) {
            auto cond = eval_in(node.condition, env);
            if (!cond.success) return cond;
            return eval_in(cond.int_value ? node.then_branch : node.else_branch, env);
        }

        if constexpr (std::is_same_v<T, ast::LambdaNode>) {
            auto* captured = copy_env(env);
            auto id = next_id();
            closures_[id] = {node.params, node.body, captured};
            return {true, static_cast<int64_t>(CLOSURE_SENTINEL + id), ""};
        }

        if constexpr (std::is_same_v<T, ast::LetNode>) {
            auto val = eval_in(node.value, env);
            if (!val.success) return val;
            Env new_env(&env);
            new_env.set_primitives(&primitives_);
            new_env.bind(node.name, val.int_value);
            return eval_in(node.body, new_env);
        }

        return {false, 0, "unknown node type"};
    }, expr->payload);
}

EvalResult Evaluator::apply_closure(ClosureId id, const std::vector<ast::Expr*>& args, const Env& call_env) {
    auto it = closures_.find(id);
    if (it == closures_.end())
        return {false, 0, "invalid closure"};

    auto& closure = it->second;
    Env new_env(closure.env ? closure.env : &top_);
    new_env.set_primitives(&primitives_);

    for (size_t i = 0; i < closure.params.size() && i < args.size(); ++i) {
        auto val = eval_in(args[i], call_env);
        if (!val.success) return val;
        new_env.bind(closure.params[i], val.int_value);
    }

    return eval_in(closure.body, new_env);
}

} // namespace aura::compiler
