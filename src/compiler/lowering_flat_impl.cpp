module aura.compiler.lowering;
import aura.core.ast;
import aura.core.ast_flat;
import aura.core.ast_pool;

namespace aura::compiler {

using namespace aura::ir;
using namespace aura::ast;

// ── Reconstruct an Expr* tree from a FlatAST index node ─────────
// Phase 3 bridge: converts FlatAST back to pointer tree so existing
// LoweringPass can consume it. Phase 4 will bypass this step.

static Expr* reconstruct_node(NodeId id, const FlatAST& flat,
                               StringPool& pool, ASTArena& arena) {
    if (id == NULL_NODE || id >= flat.size()) return nullptr;
    auto v = flat.get(id);

    switch (v.tag) {
    case NodeTag::LiteralString: {
        auto name = pool.resolve(v.sym_id);
        return arena.create<Expr>(ast::LiteralStringNode{v.tag, std::string(name)});
    }
    case NodeTag::LiteralInt:
        return arena.create<Expr>(LiteralIntNode{v.tag, v.int_value});

    case NodeTag::Variable: {
        auto name = pool.resolve(v.sym_id);
        return arena.create<Expr>(VariableNode{v.tag, std::string(name)});
    }

    case NodeTag::Call: {
        auto* func = reconstruct_node(v.child(0), flat, pool, arena);
        std::vector<Expr*> args;
        for (std::size_t i = 1; i < v.children.size(); ++i)
            args.push_back(reconstruct_node(v.child(i), flat, pool, arena));
        return arena.create<Expr>(CallNode{v.tag, func, std::move(args)});
    }

    case NodeTag::IfExpr: {
        auto* cond = reconstruct_node(v.child(0), flat, pool, arena);
        auto* then_b = reconstruct_node(v.child(1), flat, pool, arena);
        auto* else_b = reconstruct_node(v.child(2), flat, pool, arena);
        return arena.create<Expr>(IfExprNode{v.tag, cond, then_b, else_b});
    }

    case NodeTag::Lambda: {
        std::vector<std::string> param_names;
        for (auto pid : v.params)
            param_names.push_back(std::string(pool.resolve(pid)));
        auto* body = reconstruct_node(v.child(0), flat, pool, arena);
        return arena.create<Expr>(LambdaNode{v.tag, std::move(param_names), body});
    }

    case NodeTag::Let: {
        auto name = pool.resolve(v.sym_id);
        auto* val = reconstruct_node(v.child(0), flat, pool, arena);
        auto* body = reconstruct_node(v.child(1), flat, pool, arena);
        return arena.create<Expr>(LetNode{v.tag, std::string(name), val, body});
    }

    case NodeTag::LetRec: {
        auto name = pool.resolve(v.sym_id);
        auto* val = reconstruct_node(v.child(0), flat, pool, arena);
        auto* body = reconstruct_node(v.child(1), flat, pool, arena);
        return arena.create<Expr>(LetRecNode{v.tag, std::string(name), val, body});
    }

    case NodeTag::Define: {
        auto name = pool.resolve(v.sym_id);
        auto* val = reconstruct_node(v.child(0), flat, pool, arena);
        return arena.create<Expr>(DefineNode{v.tag, std::string(name), val});
    }
    case NodeTag::MacroDef: {
        auto name = pool.resolve(v.sym_id);
        auto* body = reconstruct_node(v.child(0), flat, pool, arena);
        // Params are not stored in FlatAST — we need a different approach.
        // For now, reconstruct with empty params and use the Expr* parser path
        // when macros are involved. This is a known limitation of the flat AST.
        return arena.create<Expr>(MacroDefNode{v.tag, std::string(name), {}, body});
    }
    case NodeTag::Begin: {
        ast::BeginNode begin{v.tag, {}};
        for (std::size_t i = 0; i < v.children.size(); ++i) {
            begin.exprs.push_back(reconstruct_node(v.child(i), flat, pool, arena));
        }
        return arena.create<Expr>(std::move(begin));
    }
    case NodeTag::Set: {
        auto name = pool.resolve(v.sym_id);
        auto* val = reconstruct_node(v.child(0), flat, pool, arena);
        return arena.create<Expr>(SetNode{v.tag, std::string(name), val});
    }
    case NodeTag::Quote: {
        auto* val = reconstruct_node(v.child(0), flat, pool, arena);
        return arena.create<Expr>(QuoteNode{v.tag, val});
    }
    case NodeTag::TypeAnnotation: {
        auto type_name = pool.resolve(v.sym_id);
        auto* inner = reconstruct_node(v.child(0), flat, pool, arena);
        return arena.create<Expr>(TypeAnnotationNode{v.tag, inner, std::string(type_name)});
    }
    }
    return nullptr;
}

// ── lower_to_ir (FlatAST path) ──────────────────────────────────
IRModule lower_to_ir(FlatAST& flat, StringPool& pool, ASTArena& arena) {
    auto* expr = reconstruct_node(flat.root, flat, pool, arena);
    if (!expr) return {};
    LoweringPass lowering(arena);
    return lowering.lower(expr);
}

// ── reconstruct_expr (public API) ───────────────────────────────
Expr* reconstruct_expr(FlatAST& flat, StringPool& pool, ASTArena& arena) {
    return reconstruct_node(flat.root, flat, pool, arena);
}

} // namespace aura::compiler
