module aura.core.ast_flat;
import aura.core.ast;
import aura.core.ast_pool;
import aura.core.type;

namespace aura::ast {

// ── New flatten (to FlatAST SoA + StringPool) ───────────────────
NodeId flatten_to_flat(const Expr* expr, FlatAST& ast, StringPool& pool) {
    if (!expr) return NULL_NODE;

    return std::visit([&](const auto& node) -> NodeId {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, LiteralIntNode>) {
            return ast.add_literal(node.value);
        }
        else if constexpr (std::is_same_v<T, VariableNode>) {
            return ast.add_variable(pool.intern(node.name));
        }
        else if constexpr (std::is_same_v<T, CallNode>) {
            auto func = flatten_to_flat(node.function, ast, pool);
            std::vector<NodeId> arg_ids;
            for (auto* a : node.args)
                arg_ids.push_back(flatten_to_flat(a, ast, pool));
            return ast.add_call(func, arg_ids);
        }
        else if constexpr (std::is_same_v<T, IfExprNode>) {
            auto cond = flatten_to_flat(node.condition, ast, pool);
            auto then_b = flatten_to_flat(node.then_branch, ast, pool);
            auto else_b = flatten_to_flat(node.else_branch, ast, pool);
            return ast.add_if(cond, then_b, else_b);
        }
        else if constexpr (std::is_same_v<T, LambdaNode>) {
            std::vector<SymId> param_ids;
            for (auto& p : node.params)
                param_ids.push_back(pool.intern(p));
            auto body = flatten_to_flat(node.body, ast, pool);
            return ast.add_lambda(param_ids, body);
        }
        else if constexpr (std::is_same_v<T, LetNode>) {
            auto val = flatten_to_flat(node.value, ast, pool);
            auto body = node.body ? flatten_to_flat(node.body, ast, pool) : NULL_NODE;
            return ast.add_let(pool.intern(node.name), val, body);
        }
        else if constexpr (std::is_same_v<T, LetRecNode>) {
            auto val = flatten_to_flat(node.value, ast, pool);
            auto body = flatten_to_flat(node.body, ast, pool);
            return ast.add_letrec(pool.intern(node.name), val, body);
        }
        else if constexpr (std::is_same_v<T, DefineNode>) {
            auto val = flatten_to_flat(node.value, ast, pool);
            return ast.add_define(pool.intern(node.name), val);
        }
        else if constexpr (std::is_same_v<T, TypeAnnotationNode>) {
            auto inner = flatten_to_flat(node.inner_expr, ast, pool);
            return ast.add_type_annotation(pool.intern(node.type_name), inner);
        }
        return NULL_NODE;
    }, expr->payload);
}

// ── Patch application ──────────────────────────────────────────
bool apply_patches(FlatAST& ast, std::span<const Patch> patches) {
    for (auto& p : patches) {
        if (p.node >= ast.size()) return false;
        switch (p.field_offset) {
        case 0: ast.tag(p.node) = static_cast<NodeTag>(p.new_value); break;
        case 1: ast.int_val(p.node) = static_cast<std::int64_t>(p.new_value); break;
        case 2: ast.sym_id(p.node) = static_cast<SymId>(p.new_value); break;
        default: return false;
        }
    }
    return true;
}

// ── Delta fixup (for deserialization) ──────────────────────────
void fixup_deltas(FlatAST& ast) {
    for (NodeId id = 0; id < ast.size(); ++id) {
        auto children = const_cast<FlatAST&>(ast).children(id);
        for (auto& cid : children) {
            if (cid != NULL_NODE) cid += id;
        }
    }
}

void FlatAST::resolve_type_ids(aura::core::TypeRegistry& reg, StringPool& pool) {
    for (std::size_t i = 0; i < tag_.size(); ++i) {
        if (tag_[i] == NodeTag::TypeAnnotation) {
            auto sym = sym_id_[i];
            auto sv = pool.resolve(sym);
            std::string name(sv);
            if (!name.empty()) {
                auto tid = reg.lookup_type(name);
                if (tid.valid() && i < type_id_.size())
                    type_id_[i] = tid.index;
            }
        }
    }
}

} // namespace aura::ast
