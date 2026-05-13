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
        std::vector<std::string> params;
        for (auto pid : v.params)
            params.push_back(std::string(pool.resolve(pid)));
        return arena.create<Expr>(MacroDefNode{v.tag, std::string(name), std::move(params), body});
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
    case NodeTag::Coercion: {
        auto* inner = reconstruct_node(v.child(0), flat, pool, arena);
        return arena.create<Expr>(CoercionNode{v.tag, inner, ""});
    }
    }
    return nullptr;
}

// ── Internal: native FlatAST lowering helpers ──────────────────
// Lower a FlatAST node to IR instructions. Returns the result slot.
// Reads FlatAST directly without reconstructing to Expr*.
// For Lambda nodes, reconstructs just the lambda body (needed by closure table).
static std::uint32_t lower_flat_expr(LoweringState& state,
                                      const FlatAST& flat,
                                      StringPool& pool,
                                      NodeId id) {
    if (id == NULL_NODE || id >= flat.size()) {
        auto slot = state.alloc_local();
        state.emit(IROpcode::ConstI64, slot, 0, 0);
        return slot;
    }
    auto v = flat.get(id);

    switch (v.tag) {
    case NodeTag::LiteralInt: {
        auto slot = state.alloc_local();
        auto val = static_cast<std::uint64_t>(v.int_value);
        state.emit(IROpcode::ConstI64, slot,
             static_cast<std::uint32_t>(val),
             static_cast<std::uint32_t>(val >> 32));
        return slot;
    }
    case NodeTag::LiteralString: {
        // Strings not supported in IR as first-class; return 0
        auto slot = state.alloc_local();
        state.emit(IROpcode::ConstI64, slot, 0, 0);
        return slot;
    }
    case NodeTag::Variable: {
        auto name = pool.resolve(v.sym_id);
        // Look up in scope chain
        for (auto it = state.scopes.rbegin(); it != state.scopes.rend(); ++it) {
            auto found = it->find(std::string(name));
            if (found != it->end()) {
                auto& binding = found->second;
                auto slot = state.alloc_local();
                switch (binding.kind) {
                case BindingKind::Local:
                    state.emit(IROpcode::Local, slot, binding.slot); break;
                case BindingKind::Captured:
                    state.emit(IROpcode::Local, slot, binding.slot); break;
                case BindingKind::Cell:
                    state.emit(IROpcode::CellGet, slot, binding.slot); break;
                }
                return slot;
            }
        }
        // Free variable from outer lambda
        auto fv = state.free_var_map.find(std::string(name));
        if (fv != state.free_var_map.end()) {
            auto slot = state.alloc_local();
            state.emit(IROpcode::Local, slot, fv->second);
            return slot;
        }
        auto slot = state.alloc_local();
        state.emit(IROpcode::ConstI64, slot, 0, 0);
        return slot;
    }
    case NodeTag::Call: {
        // Check if callee is a known primitive (+, -, *, /, =, <, >, <=, >=)
        // and inline as direct IR opcode to avoid Call overhead.
        {
            auto callee_v = flat.get(v.child(0));
            if (callee_v.tag == NodeTag::Variable) {
                auto callee_name = pool.resolve(callee_v.sym_id);
                static const std::unordered_map<std::string, IROpcode> prim_map = {
                    {"+", IROpcode::Add}, {"-", IROpcode::Sub},
                    {"*", IROpcode::Mul}, {"/", IROpcode::Div},
                    {"=", IROpcode::Eq},  {"<", IROpcode::Lt},
                    {">", IROpcode::Gt},  {"<=", IROpcode::Le},
                    {">=", IROpcode::Ge},
                };
                auto it = prim_map.find(std::string(callee_name));
                if (it != prim_map.end()) {
                    auto op = it->second;
                    auto result_slot = state.alloc_local();
                    auto arg_count = v.children.size() - 1;
                    if (arg_count == 0) {
                        // No args: return 0
                        state.emit(IROpcode::ConstI64, result_slot, 0, 0);
                    } else {
                        auto arg0 = lower_flat_expr(state, flat, pool, v.child(1));
                        if (arg_count == 1) {
                            // Unary minus: emit Sub(0, arg)
                            if (std::string(callee_name) == "-") {
                                auto zero = state.alloc_local();
                                state.emit(IROpcode::ConstI64, zero, 0, 0);
                                state.emit(op, result_slot, zero, arg0);
                            } else {
                                state.emit(op, result_slot, arg0, arg0);
                            }
                        } else {
                            // Binary: lower second arg and emit
                            auto arg1 = lower_flat_expr(state, flat, pool, v.child(2));
                            state.emit(op, result_slot, arg0, arg1);
                        }
                    }
                    return result_slot;
                }
            }
        }

        // General function call: lower callee and args, then emit Call
        auto callee_slot = lower_flat_expr(state, flat, pool, v.child(0));
        auto arg_count = static_cast<std::uint32_t>(v.children.size() - 1);
        // Reserve contiguous argument block
        auto arg_base = state.local_count;
        for (std::size_t i = 1; i < v.children.size(); ++i) {
            auto val_slot = lower_flat_expr(state, flat, pool, v.child(i));
            state.emit(IROpcode::Local, arg_base + static_cast<std::uint32_t>(i - 1), val_slot);
            state.alloc_local();
        }
        auto result_slot = state.alloc_local();
        // Call(callee_slot, arg_base, arg_count, result_slot)
        state.emit(IROpcode::Call, callee_slot, arg_base, arg_count, result_slot);
        return result_slot;
    }
    case NodeTag::IfExpr: {
        auto cond_slot = lower_flat_expr(state, flat, pool, v.child(0));
        auto then_blk = state.alloc_block();
        auto else_blk = state.alloc_block();
        auto merge_blk = state.alloc_block();
        state.emit(IROpcode::Branch, cond_slot, then_blk, else_blk);

        auto phi_slot = state.alloc_local();

        state.cur_block = then_blk;
        auto then_val = lower_flat_expr(state, flat, pool, v.child(1));
        state.emit(IROpcode::Local, phi_slot, then_val);
        state.emit(IROpcode::Jump, merge_blk);

        state.cur_block = else_blk;
        auto else_val = lower_flat_expr(state, flat, pool, v.child(2));
        state.emit(IROpcode::Local, phi_slot, else_val);
        state.emit(IROpcode::Jump, merge_blk);

        state.cur_block = merge_blk;
        return phi_slot;
    }
    case NodeTag::Lambda: {
        // Native lambda lowering: create a new IRFunction, lower body
        // via lower_flat_expr, emit MakeClosure.
        auto param_span = v.params;
        std::vector<std::string> param_names;
        for (auto pid : param_span)
            param_names.push_back(std::string(pool.resolve(pid)));

        // Collect free variables: vars in body not in param list
        std::unordered_set<std::string> free_set;
        std::unordered_set<std::string> bound(param_names.begin(), param_names.end());
        // Walk body to find free vars
        {
            struct FreeVarWalker {
                const FlatAST& flat;
                StringPool& pool;
                std::unordered_set<std::string>& free;
                std::unordered_set<std::string>& bound;
                void walk(NodeId nid) {
                    if (nid == NULL_NODE || nid >= flat.size()) return;
                    auto nv = flat.get(nid);
                    switch (nv.tag) {
                    case NodeTag::Variable: {
                        auto name = pool.resolve(nv.sym_id);
                        if (bound.find(std::string(name)) == bound.end())
                            free.insert(std::string(name));
                        break;
                    }
                    case NodeTag::LiteralInt: case NodeTag::LiteralString: break;
                    default:
                        for (auto c : nv.children) walk(c);
                        break;
                    }
                }
            };
            FreeVarWalker{flat, pool, free_set, bound}.walk(v.child(0));
        }

        // Filter to only vars actually in scope
        std::vector<std::string> free_vars;
        for (auto& fv : free_set) {
            bool in_scope = false;
            for (auto it = state.scopes.rbegin(); it != state.scopes.rend(); ++it) {
                if (it->find(fv) != it->end()) { in_scope = true; break; }
            }
            if (in_scope) free_vars.push_back(fv);
        }

        // Create new IR function
        IRFunction func;
        func.name = "__lambda__";
        func.entry_block = 0;
        func.blocks.push_back({0, {}, {}});
        func.params = param_names;
        func.arg_count = static_cast<std::uint32_t>(param_names.size());

        // Save parent state
        auto* saved_func = state.cur_func;
        auto saved_block = state.cur_block;
        auto saved_locals = state.local_count;
        auto saved_scopes = std::move(state.scopes);
        auto saved_env_slot = state.env_slot;
        auto saved_fv_map = std::move(state.free_var_map);

        state.cur_func = &func;
        state.cur_block = 0;
        state.local_count = 0;
        state.free_var_map.clear();

        state.env_slot = state.alloc_local();  // placeholder
        state.scopes.push_back({});

        // Load captured free vars from env prefix
        for (std::size_t i = 0; i < free_vars.size(); ++i) {
            auto& fv = free_vars[i];
            auto slot = state.alloc_local();
            state.emit(IROpcode::Arg, slot, static_cast<std::uint32_t>(i));
            state.scopes.back()[fv] = Binding{BindingKind::Captured, slot};
            state.free_var_map[fv] = static_cast<std::uint32_t>(i);
        }

        // Bind parameters
        for (std::size_t i = 0; i < param_names.size(); ++i) {
            auto slot = state.alloc_local();
            state.emit(IROpcode::Arg, slot,
                 static_cast<std::uint32_t>(free_vars.size() + i));
            state.scopes.back()[param_names[i]] = Binding{BindingKind::Local, slot};
        }

        // Lower body via native FlatAST path
        auto body_slot = lower_flat_expr(state, flat, pool, v.child(0));
        state.emit(IROpcode::Return, body_slot);

        func.local_count = state.local_count;

        // Restore parent state
        state.cur_func = saved_func;
        state.cur_block = saved_block;
        state.local_count = saved_locals;
        state.scopes = std::move(saved_scopes);
        state.env_slot = saved_env_slot;
        state.free_var_map = std::move(saved_fv_map);

        auto fid = state.module.add_function(std::move(func));
        auto slot = state.alloc_local();
        state.emit(IROpcode::MakeClosure, slot, fid,
             static_cast<std::uint32_t>(free_vars.size()));

        // Capture each free variable into the closure
        for (std::size_t i = 0; i < free_vars.size(); ++i) {
            auto& fv = free_vars[i];
            for (auto it = state.scopes.rbegin(); it != state.scopes.rend(); ++it) {
                auto found = it->find(fv);
                if (found != it->end()) {
                    auto& binding = found->second;
                    auto cslot = state.alloc_local();
                    state.emit(IROpcode::Local, cslot, binding.slot);
                    state.emit(IROpcode::Capture, slot,
                         static_cast<std::uint32_t>(i), cslot);
                    break;
                }
            }
        }

        return slot;
    }
    case NodeTag::Let:
    case NodeTag::LetRec: {
        bool rec = (v.tag == NodeTag::LetRec);
        auto name = pool.resolve(v.sym_id);
        auto val_id = v.children.empty() ? NULL_NODE : v.child(0);
        auto body_id = v.children.size() < 2 ? NULL_NODE : v.child(1);

        if (rec) {
            // letrec: create cell, bind, init
            auto ci = state.alloc_local();
            state.emit(IROpcode::NewCell, ci);
            state.scopes.push_back({});
            auto& scope = state.scopes.back();
            scope[std::string(name)] = Binding{BindingKind::Cell, ci};
            auto val_slot = lower_flat_expr(state, flat, pool, val_id);
            state.emit(IROpcode::CellSet, ci, val_slot);
            auto body_slot = lower_flat_expr(state, flat, pool, body_id);
            state.scopes.pop_back();
            return body_slot;
        } else {
            auto val_slot = lower_flat_expr(state, flat, pool, val_id);
            state.scopes.push_back({});
            auto& scope = state.scopes.back();
            scope[std::string(name)] = Binding{BindingKind::Local, val_slot};
            auto body_slot = lower_flat_expr(state, flat, pool, body_id);
            state.scopes.pop_back();
            return body_slot;
        }
    }
    case NodeTag::Define: {
        auto name = pool.resolve(v.sym_id);
        auto val_id = v.children.empty() ? NULL_NODE : v.child(0);
        auto val_slot = lower_flat_expr(state, flat, pool, val_id);
        // Define = letrec cell in top scope
        state.scopes.push_back({});
        auto& scope = state.scopes.back();
        auto ci = state.alloc_local();
        state.emit(IROpcode::NewCell, ci);
        state.emit(IROpcode::CellSet, ci, val_slot);
        scope[std::string(name)] = Binding{BindingKind::Cell, ci};
        state.scopes.pop_back();
        return val_slot;
    }
    case NodeTag::Begin: {
        std::uint32_t last_slot = 0;
        for (auto c : v.children)
            last_slot = lower_flat_expr(state, flat, pool, c);
        if (!v.children.empty()) return last_slot;
        auto slot = state.alloc_local();
        state.emit(IROpcode::ConstI64, slot, 0, 0);
        return slot;
    }
    case NodeTag::Set: {
        auto name = pool.resolve(v.sym_id);
        auto val_id = v.children.empty() ? NULL_NODE : v.child(0);
        auto val_slot = lower_flat_expr(state, flat, pool, val_id);

        for (auto it = state.scopes.rbegin(); it != state.scopes.rend(); ++it) {
            auto found = it->find(std::string(name));
            if (found != it->end()) {
                auto& binding = found->second;
                if (binding.kind == BindingKind::Cell) {
                    state.emit(IROpcode::CellSet, binding.slot, val_slot);
                    return val_slot;
                }
                // For local vars, just update the slot value
                state.emit(IROpcode::Local, binding.slot, val_slot);
                return val_slot;
            }
        }
        return val_slot;
    }
    case NodeTag::Quote:
        return lower_flat_expr(state, flat, pool, v.child(0));
    case NodeTag::TypeAnnotation:
        return lower_flat_expr(state, flat, pool, v.child(0));
    case NodeTag::Coercion: {
        auto inner = lower_flat_expr(state, flat, pool, v.child(0));
        auto slot = state.alloc_local();
        state.emit(IROpcode::CastOp, slot, inner, 3); // dynamic
        return slot;
    }
    case NodeTag::MacroDef:
    default: {
        // Macros handled by evaluator fallback; emit 0
        auto slot = state.alloc_local();
        state.emit(IROpcode::ConstI64, slot, 0, 0);
        return slot;
    }
    }
}

// ── lower_to_ir (FlatAST path) — native, no full-tree reconstruct ──
IRModule lower_to_ir(FlatAST& flat, StringPool& pool, ASTArena& arena) {
    LoweringState state(arena);
    state.module = {};
    // Create top-level function
    IRFunction top_func;
    top_func.id = 0;
    top_func.name = "__top__";
    top_func.entry_block = 0;
    top_func.blocks.push_back({0});
    state.cur_func = &top_func;
    state.cur_block = 0;
    state.local_count = 0;

    auto result_slot = lower_flat_expr(state, flat, pool, flat.root);

    // Emit return
    state.emit(IROpcode::Return, result_slot);
    top_func.local_count = state.local_count;
    auto top_id = state.module.add_function(std::move(top_func));
    state.module.entry_function_id = top_id;
    return state.module;
}

// ── reconstruct_expr (public API) — kept for tree-walker fallback ──

// ── reconstruct_expr (public API) ───────────────────────────────
Expr* reconstruct_expr(FlatAST& flat, StringPool& pool, ASTArena& arena) {
    return reconstruct_node(flat.root, flat, pool, arena);
}

} // namespace aura::compiler
