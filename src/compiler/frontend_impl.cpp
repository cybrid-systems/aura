module;
#include <cstdint>
#include <string>
#include <variant>

module aura.compiler.frontend;

namespace aura::compiler {

EvalResult Evaluator::eval(const ast::Expr* expr) {
    if (!expr)
        return {false, 0, "null expression"};

    return std::visit([&](const auto& node) -> EvalResult {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::LiteralIntNode>) {
            return {true, node.value, ""};
        }

        if constexpr (std::is_same_v<T, ast::VariableNode>) {
            return {false, 0, "unbound variable: " + node.name};
        }

        return {true, static_cast<int64_t>(node.tag), ""};
    }, expr->payload);
}

} // namespace aura::compiler
