module aura.core.ast;

namespace aura::ast {

// Recursively flatten an Expr pointer-tree into an index-based AST.
// This is a temporary bridge — eventually parse() should produce AST directly.
NodeId flatten_expr(const Expr* expr, AST& ast) {
    if (!expr) return NULL_NODE;

    return std::visit([&](const auto& node) -> NodeId {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, LiteralIntNode>) {
            return ast.add_literal(node.value);
        }
        else if constexpr (std::is_same_v<T, VariableNode>) {
            return ast.add_variable(node.name);
        }
        else if constexpr (std::is_same_v<T, CallNode>) {
            auto func = flatten_expr(node.function, ast);
            std::vector<NodeId> arg_ids;
            for (auto* a : node.args)
                arg_ids.push_back(flatten_expr(a, ast));
            return ast.add_call(func, arg_ids);
        }
        else if constexpr (std::is_same_v<T, IfExprNode>) {
            auto cond = flatten_expr(node.condition, ast);
            auto then_b = flatten_expr(node.then_branch, ast);
            auto else_b = flatten_expr(node.else_branch, ast);
            return ast.add_if(cond, then_b, else_b);
        }
        else if constexpr (std::is_same_v<T, LambdaNode>) {
            auto body = flatten_expr(node.body, ast);
            return ast.add_lambda(node.params, body);
        }
        else if constexpr (std::is_same_v<T, LetNode>) {
            auto val = flatten_expr(node.value, ast);
            auto body = node.body ? flatten_expr(node.body, ast) : NULL_NODE;
            return ast.add_let(node.name, val, body);
        }
        else if constexpr (std::is_same_v<T, LetRecNode>) {
            auto val = flatten_expr(node.value, ast);
            auto body = flatten_expr(node.body, ast);
            return ast.add_letrec(node.name, val, body);
        }
        else if constexpr (std::is_same_v<T, DefineNode>) {
            auto val = flatten_expr(node.value, ast);
            return ast.add_define(node.name, val);
        }
        return NULL_NODE;
    }, expr->payload);
}

} // namespace aura::ast
