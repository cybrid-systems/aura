module;
#include <cstdint>
#include <string>
#include <variant>

module aura.compiler.frontend;

namespace aura::compiler {

EvalResult Evaluator::eval_in(const ast::Expr* expr, const Env& env) {
    if (!expr)
        return {false, 0, "null expression"};

    return std::visit([&](const auto& node) -> EvalResult {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::LiteralIntNode>) {
            return {true, node.value, ""};
        }

        if constexpr (std::is_same_v<T, ast::VariableNode>) {
            auto val = env.lookup(node.name);
            if (val.has_value())
                return {true, *val, ""};
            return {false, 0, "unbound variable: " + node.name};
        }

        if constexpr (std::is_same_v<T, ast::LetNode>) {
            // Evaluate the value expression in current env
            auto val = eval_in(node.value, env);
            if (!val.success) return val;
            // Extend env and evaluate body
            Env new_env(&env);
            new_env.bind(node.name, val.int_value);
            return eval_in(node.body, new_env);
        }

        return {true, static_cast<int64_t>(node.tag), ""};
    }, expr->payload);
}

} // namespace aura::compiler
