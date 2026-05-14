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

    auto make_expr = [&](auto&& nc) -> Expr* { auto* e = arena.create<Expr>(std::move(nc)); e->loc = {v.line, v.col, 0}; return e; };

    switch (v.tag) {
    case NodeTag::LiteralString: {
        auto name = pool.resolve(v.sym_id);
        return make_expr(ast::LiteralStringNode{v.tag, std::string(name)});
    }
    case NodeTag::LiteralInt:
        return make_expr(LiteralIntNode{v.tag, v.int_value});

    case NodeTag::Variable: {
        auto name = pool.resolve(v.sym_id);
        return make_expr(VariableNode{v.tag, std::string(name)});
    }

    case NodeTag::Call: {
        auto* func = reconstruct_node(v.child(0), flat, pool, arena);
        std::vector<Expr*> args;
        for (std::size_t i = 1; i < v.children.size(); ++i)
            args.push_back(reconstruct_node(v.child(i), flat, pool, arena));
        return make_expr(CallNode{v.tag, func, std::move(args)});
    }

    case NodeTag::IfExpr: {
        auto* cond = reconstruct_node(v.child(0), flat, pool, arena);
        auto* then_b = reconstruct_node(v.child(1), flat, pool, arena);
        auto* else_b = reconstruct_node(v.child(2), flat, pool, arena);
        return make_expr(IfExprNode{v.tag, cond, then_b, else_b});
    }

    case NodeTag::Lambda: {
        std::vector<std::string> param_names;
        for (auto pid : v.params)
            param_names.push_back(std::string(pool.resolve(pid)));
        auto* body = reconstruct_node(v.child(0), flat, pool, arena);
        return make_expr(LambdaNode{v.tag, std::move(param_names), body});
    }

    case NodeTag::Let: {
        auto name = pool.resolve(v.sym_id);
        auto* val = reconstruct_node(v.child(0), flat, pool, arena);
        auto* body = reconstruct_node(v.child(1), flat, pool, arena);
        return make_expr(LetNode{v.tag, std::string(name), val, body});
    }

    case NodeTag::LetRec: {
        auto name = pool.resolve(v.sym_id);
        auto* val = reconstruct_node(v.child(0), flat, pool, arena);
        auto* body = reconstruct_node(v.child(1), flat, pool, arena);
        return make_expr(LetRecNode{v.tag, std::string(name), val, body});
    }

    case NodeTag::Define: {
        auto name = pool.resolve(v.sym_id);
        auto* val = reconstruct_node(v.child(0), flat, pool, arena);
        return make_expr(DefineNode{v.tag, std::string(name), val});
    }
    case NodeTag::MacroDef: {
        auto name = pool.resolve(v.sym_id);
        auto* body = reconstruct_node(v.child(0), flat, pool, arena);
        std::vector<std::string> params;
        for (auto pid : v.params)
            params.push_back(std::string(pool.resolve(pid)));
        return make_expr(MacroDefNode{v.tag, std::string(name), std::move(params), body});
    }
    case NodeTag::Begin: {
        ast::BeginNode begin{v.tag, {}};
        for (std::size_t i = 0; i < v.children.size(); ++i) {
            begin.exprs.push_back(reconstruct_node(v.child(i), flat, pool, arena));
        }
        auto* e_b = arena.create<Expr>(std::move(begin));
        e_b->loc = {v.line, v.col, 0};
        return e_b;
    }
    case NodeTag::Set: {
        auto name = pool.resolve(v.sym_id);
        auto* val = reconstruct_node(v.child(0), flat, pool, arena);
        return make_expr(SetNode{v.tag, std::string(name), val});
    }
    case NodeTag::Quote: {
        auto* val = reconstruct_node(v.child(0), flat, pool, arena);
        return make_expr(QuoteNode{v.tag, val});
    }
    case NodeTag::TypeAnnotation: {
        auto type_name = pool.resolve(v.sym_id);
        auto* inner = reconstruct_node(v.child(0), flat, pool, arena);
        return make_expr(TypeAnnotationNode{v.tag, inner, std::string(type_name)});
    }
    case NodeTag::Coercion: {
        auto* inner = reconstruct_node(v.child(0), flat, pool, arena);
        return make_expr(CoercionNode{v.tag, inner, ""});
    }
    }
    return nullptr;
}

// ── Internal: native FlatAST lowering helpers ──────────────────
// Lower a FlatAST node to IR instructions. Returns the result slot.
// Reads FlatAST directly without reconstructing to Expr*.
// For Lambda nodes, reconstructs just the lambda body (needed by closure table).
// cache: optional map from variable name → bundle of IRFunctions for cached defines.

// Remap func ids inside a cached IRFunction when inlining into a new module.
// Each MakeClosure instruction's func_id is offset by base_fid, because the new
// module has functions added after its own __top__ function at a different base.
static void remap_func_ids(aura::ir::IRFunction& func, std::uint32_t base_fid) {
    for (auto& block : func.blocks) {
        for (auto& inst : block.instructions) {
            if (inst.opcode == aura::ir::IROpcode::MakeClosure) {
                // operands[1] = func_id to reference
                inst.operands[1] += base_fid;
            }
        }
    }
}

static std::uint32_t lower_flat_expr(LoweringState& state,
                                      const FlatAST& flat,
                                      StringPool& pool,
                                      NodeId id,
                                      const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>* cache = nullptr,
                                      std::vector<std::string>* cache_hits = nullptr) {
    // Early exit for invalid ids (backup for contract-observe mode)
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
        // Check if this variable names a cached define function
        if (cache) {
            auto cache_it = cache->find(std::string(name));
            if (cache_it != cache->end()) {
                if (cache_hits) {
                    cache_hits->push_back(std::string(name));
                }
                // Record the starting func id (offset) before adding bundle functions
                auto base_fid = static_cast<std::uint32_t>(state.module.functions.size());
                // Add all bundle functions to module, remapping func ids
                std::uint32_t lambda_fid = 0;
                for (auto& func : cache_it->second) {
                    auto copy = func;
                    remap_func_ids(copy, base_fid);
                    lambda_fid = state.module.add_function(std::move(copy));
                }
                auto closure_slot = state.alloc_local();
                state.emit(IROpcode::MakeClosure, closure_slot, lambda_fid, 0);
                return closure_slot;
            }
        }
        auto slot = state.alloc_local();
        state.emit(IROpcode::ConstI64, slot, 0, 0);
        return slot;
    }
    case NodeTag::Call: {
        auto callee_v = flat.get(v.child(0));

        // Check if callee is a known primitive (+, -, *, /, =, <, >, <=, >=)
        // and inline as direct IR opcode to avoid Call overhead.
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
                    auto arg0 = lower_flat_expr(state, flat, pool, v.child(1), cache, cache_hits);
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
                        auto arg1 = lower_flat_expr(state, flat, pool, v.child(2), cache, cache_hits);
                        state.emit(op, result_slot, arg0, arg1);
                    }
                }
                return result_slot;
            }

            // Check if callee is a cached define function (now handled by Variable handler)
            // Fall through to general function call path
        }

        // General function call: lower callee and args, then emit Call
        auto callee_slot = lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
        auto arg_count = static_cast<std::uint32_t>(v.children.size() - 1);
        // Reserve contiguous argument block
        auto arg_base = state.local_count;
        for (std::size_t i = 1; i < v.children.size(); ++i) {
            auto val_slot = lower_flat_expr(state, flat, pool, v.child(i), cache, cache_hits);
            state.emit(IROpcode::Local, arg_base + static_cast<std::uint32_t>(i - 1), val_slot);
            state.alloc_local();
        }
        auto result_slot = state.alloc_local();
        // Call(callee_slot, arg_base, arg_count, result_slot)
        state.emit(IROpcode::Call, callee_slot, arg_base, arg_count, result_slot);
        return result_slot;
    }
    case NodeTag::IfExpr: {
        auto cond_slot = lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
        auto then_blk = state.alloc_block();
        auto else_blk = state.alloc_block();
        auto merge_blk = state.alloc_block();
        state.emit(IROpcode::Branch, cond_slot, then_blk, else_blk);

        auto phi_slot = state.alloc_local();

        state.cur_block = then_blk;
        auto then_val = lower_flat_expr(state, flat, pool, v.child(1), cache, cache_hits);
        state.emit(IROpcode::Local, phi_slot, then_val);
        state.emit(IROpcode::Jump, merge_blk);

        state.cur_block = else_blk;
        auto else_val = lower_flat_expr(state, flat, pool, v.child(2), cache, cache_hits);
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
            // Check if this free var is a letrec cell in the outer scope
            bool is_cell = false;
            for (auto it = saved_scopes.rbegin(); it != saved_scopes.rend(); ++it) {
                auto found = it->find(fv);
                if (found != it->end() && found->second.kind == BindingKind::Cell) {
                    is_cell = true; break;
                }
            }
            if (is_cell) {
                // Cell capture: env[i] is cell_id, load via CellGet
                auto result = state.alloc_local();
                state.emit(IROpcode::CellGet, result, slot);
                state.scopes.back()[fv] = Binding{BindingKind::Local, result};
            } else {
                state.scopes.back()[fv] = Binding{BindingKind::Captured, slot};
            }
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
        auto body_slot = lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
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
            auto val_slot = lower_flat_expr(state, flat, pool, val_id, cache, cache_hits);
            state.emit(IROpcode::CellSet, ci, val_slot);
            auto body_slot = lower_flat_expr(state, flat, pool, body_id, cache, cache_hits);
            state.scopes.pop_back();
            return body_slot;
        } else {
            auto val_slot = lower_flat_expr(state, flat, pool, val_id, cache, cache_hits);
            state.scopes.push_back({});
            auto& scope = state.scopes.back();
            scope[std::string(name)] = Binding{BindingKind::Local, val_slot};
            auto body_slot = lower_flat_expr(state, flat, pool, body_id, cache, cache_hits);
            state.scopes.pop_back();
            return body_slot;
        }
    }
    case NodeTag::Define: {
        auto name = pool.resolve(v.sym_id);
        auto val_id = v.children.empty() ? NULL_NODE : v.child(0);
        auto val_slot = lower_flat_expr(state, flat, pool, val_id, cache, cache_hits);
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
            last_slot = lower_flat_expr(state, flat, pool, c, cache, cache_hits);
        if (!v.children.empty()) return last_slot;
        auto slot = state.alloc_local();
        state.emit(IROpcode::ConstI64, slot, 0, 0);
        return slot;
    }
    case NodeTag::Set: {
        auto name = pool.resolve(v.sym_id);
        auto val_id = v.children.empty() ? NULL_NODE : v.child(0);
        auto val_slot = lower_flat_expr(state, flat, pool, val_id, cache, cache_hits);

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
        return lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
    case NodeTag::TypeAnnotation:
        return lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
    case NodeTag::Coercion: {
        auto inner = lower_flat_expr(state, flat, pool, v.child(0), cache, cache_hits);
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

// ── Internal: lower_to_ir with optional cache ──────────────────
static IRModule lower_to_ir_impl(FlatAST& flat, StringPool& pool, ASTArena& arena,
                                  const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>* cache,
                                  std::vector<std::string>* cache_hits = nullptr) {
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

    auto result_slot = lower_flat_expr(state, flat, pool, flat.root, cache, cache_hits);

    // Emit return
    state.emit(IROpcode::Return, result_slot);
    top_func.local_count = state.local_count;
    auto top_id = state.module.add_function(std::move(top_func));
    state.module.entry_function_id = top_id;
    return state.module;
}

// ── lower_to_ir (FlatAST path) — native, no full-tree reconstruct ──
IRModule lower_to_ir(FlatAST& flat, StringPool& pool, ASTArena& arena) {
    return lower_to_ir_impl(flat, pool, arena, nullptr);
}

// ── lower_to_ir_with_cache ─────────────────────────────────────
IRModule lower_to_ir_with_cache(
    FlatAST& flat, StringPool& pool, ASTArena& arena,
    const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>* cache,
    std::vector<std::string>* cache_hits) {
    return lower_to_ir_impl(flat, pool, arena, cache, cache_hits);
}

// ── reconstruct_expr (public API) — kept for tree-walker fallback ──

// ── reconstruct_expr (public API) ───────────────────────────────
Expr* reconstruct_expr(FlatAST& flat, StringPool& pool, ASTArena& arena) {
    return reconstruct_node(flat.root, flat, pool, arena);
}

// ── unparse_node — FlatAST → S-expression source ───────────────
std::string unparse_node(const FlatAST& flat, const StringPool& pool,
                           NodeId id, int indent) {
    if (id == NULL_NODE || id >= flat.size()) return "()";
    auto v = flat.get(id);
    auto indent_str = [](int d) { return std::string(static_cast<std::size_t>(d * 2), ' '); };

    switch (v.tag) {
    case NodeTag::LiteralInt:
        return std::to_string(v.int_value);

    case NodeTag::LiteralString: {
        auto s = pool.resolve(v.sym_id);
        std::string out = "\"";
        for (char c : s) {
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else out += c;
        }
        return out + "\"";
    }

    case NodeTag::Variable:
        return std::string(pool.resolve(v.sym_id));

    case NodeTag::Call: {
        // Check: is this a lambda application? ((lambda ...) ...)
        // Format: (func arg1 arg2 ...)
        auto callee = v.child(0);
        std::string s = "(";
        s += unparse_node(flat, pool, callee, indent + 1);

        for (std::size_t i = 1; i < v.children.size(); ++i) {
            auto arg_str = unparse_node(flat, pool, v.child(i), indent + 1);
            // If arg is multiline or makes total line too long, use newline
            bool long_arg = arg_str.size() > 40 ||
                (s.size() + arg_str.size() + 2 > 80 && s.size() > 20);
            if (long_arg) {
                s += "\n" + indent_str(indent + 1) + arg_str;
            } else {
                s += " " + arg_str;
            }
        }
        s += ")";
        return s;
    }

    case NodeTag::IfExpr: {
        std::string s = "(if ";
        s += unparse_node(flat, pool, v.child(0), indent + 1);
        auto then_str = unparse_node(flat, pool, v.child(1), indent + 1);
        auto else_str = unparse_node(flat, pool, v.child(2), indent + 1);
        if (then_str.size() + else_str.size() > 40) {
            s += "\n" + indent_str(indent + 1) + then_str;
            s += "\n" + indent_str(indent + 1) + else_str;
        } else {
            s += " " + then_str + " " + else_str;
        }
        s += ")";
        return s;
    }

    case NodeTag::Lambda: {
        std::string s = "(lambda (";
        for (std::size_t i = 0; i < v.params.size(); ++i) {
            if (i > 0) s += " ";
            s += pool.resolve(v.params[i]);
        }
        s += ")\n" + indent_str(indent + 1);
        s += unparse_node(flat, pool, v.child(0), indent + 1);
        s += ")";
        return s;
    }

    case NodeTag::Let:
    case NodeTag::LetRec: {
        std::string kw = (v.tag == NodeTag::LetRec) ? "letrec" : "let";
        std::string s = "(" + kw + " ((";
        s += pool.resolve(v.sym_id);
        s += " ";
        s += unparse_node(flat, pool, v.child(0), indent + 1);
        s += "))\n" + indent_str(indent + 1);
        s += unparse_node(flat, pool, v.child(1), indent + 1);
        s += ")";
        return s;
    }

    case NodeTag::Define: {
        std::string s = "(define ";
        s += pool.resolve(v.sym_id);
        auto val_str = unparse_node(flat, pool, v.child(0), indent + 1);
        if (val_str.size() > 30) {
            s += "\n" + indent_str(indent + 1) + val_str;
        } else {
            s += " " + val_str;
        }
        s += ")";
        return s;
    }

    case NodeTag::Begin: {
        std::string s = "(begin";
        for (std::size_t i = 0; i < v.children.size(); ++i) {
            s += "\n" + indent_str(indent + 1);
            s += unparse_node(flat, pool, v.child(i), indent + 1);
        }
        return s + ")";
    }

    case NodeTag::Set: {
        return "(set! " + std::string(pool.resolve(v.sym_id)) + " "
             + unparse_node(flat, pool, v.child(0), indent + 1) + ")";
    }

    case NodeTag::Quote: {
        return "(quote " + unparse_node(flat, pool, v.child(0), indent + 1) + ")";
    }

    case NodeTag::TypeAnnotation: {
        return "(the " + std::string(pool.resolve(v.sym_id)) + " "
             + unparse_node(flat, pool, v.child(0), indent + 1) + ")";
    }

    case NodeTag::Coercion: {
        return "(coerce " + unparse_node(flat, pool, v.child(0), indent + 1) + ")";
    }

    default:
        return "()";
    }
}

} // namespace aura::compiler
