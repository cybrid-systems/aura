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
    if (it != table_.end()) return it->second;
    return std::nullopt;
}

std::optional<int64_t> Env::lookup(const std::string& name) const {
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
        if (it->first == name) return it->second;
    return parent_ ? parent_->lookup(name) : std::nullopt;
}

Evaluator::Evaluator() {
    top_.set_primitives(&primitives_);
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
            if (auto* var = std::get_if<ast::VariableNode>(&node.function->payload)) {
                auto prim = env.lookup_primitive(var->name);
                if (prim.has_value()) {
                    std::vector<int64_t> arg_vals;
                    for (auto* arg : node.args) {
                        auto a = eval_in(arg, env);
                        if (!a.success) return a;
                        arg_vals.push_back(a.int_value);
                    }
                    return {true, (*prim)(arg_vals), ""};
                }
            }
            return {false, 0, "not callable"};
        }

        if constexpr (std::is_same_v<T, ast::IfExprNode>) {
            auto cond = eval_in(node.condition, env);
            if (!cond.success) return cond;
            return eval_in(cond.int_value ? node.then_branch : node.else_branch, env);
        }

        if constexpr (std::is_same_v<T, ast::LambdaNode>) {
            return {false, 0, "lambda not yet supported as first-class value"};
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

} // namespace aura::compiler
