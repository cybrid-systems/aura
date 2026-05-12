module aura.compiler.lowering;
import std;

namespace aura::compiler {

using namespace aura::ir;



IRModule LoweringPass::lower(const ast::Expr* expr) {
    state_->module = {};
    state_->scopes.clear();

    // Create the top-level function
    IRFunction top_func;
    top_func.name = "__top__";
    top_func.entry_block = 0;
    top_func.blocks.push_back({0, {}, {}});
    top_func.arg_count = 0;
    state_->cur_func = &top_func;
    state_->cur_block = 0;
    state_->local_count = 0;

    // Push empty scope for top-level (no parameters)
    state_->scopes.push_back({});

    // Lower the expression
    auto result_slot = lower_expr(expr);
    emit(IROpcode::Return, result_slot);

    top_func.local_count = state_->local_count;
    auto top_id = state_->module.add_function(std::move(top_func));
    state_->module.set_entry(top_id);
    return std::move(state_->module);
}

std::uint32_t LoweringPass::lower_expr(const ast::Expr* expr) {
    if (!expr) { auto s = alloc_local(); emit(IROpcode::ConstI64, s, 0); return s; }

    return std::visit([&](const auto& node) -> std::uint32_t {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::LiteralIntNode>) return lower_literal_int(node);
        if constexpr (std::is_same_v<T, ast::VariableNode>)    return lower_variable(node);
        if constexpr (std::is_same_v<T, ast::CallNode>)        return lower_call(node);
        if constexpr (std::is_same_v<T, ast::IfExprNode>)      return lower_if(node);
        if constexpr (std::is_same_v<T, ast::LambdaNode>)      return lower_lambda(node);
        if constexpr (std::is_same_v<T, ast::LetNode>)         return lower_let(node, false);
        if constexpr (std::is_same_v<T, ast::LetRecNode>)      return lower_let(ast::LetNode{node.tag, node.name, node.value, node.body}, true);
        if constexpr (std::is_same_v<T, ast::DefineNode>)      return lower_let(
            ast::LetNode{ast::NodeTag::Let, node.name, node.value, nullptr}, false);
        if constexpr (std::is_same_v<T, ast::BeginNode>)      return lower_begin(node);
        if constexpr (std::is_same_v<T, ast::SetNode>)        return lower_set(node);
        if constexpr (std::is_same_v<T, ast::QuoteNode>)      return lower_expr(node.value);
        auto s = alloc_local(); emit(IROpcode::ConstI64, s, 0); return s;
    }, expr->payload);
}

// ── collect_free_vars2 — returns pair (preferred API) ───────
std::pair<std::unordered_set<std::string>, std::unordered_set<std::string>>
LoweringPass::collect_free_vars2(const ast::Expr* expr,
                                  std::unordered_set<std::string> bound) {
    std::unordered_set<std::string> free;
    collect_free_vars(expr, free, bound);
    return {free, bound};
}


// ── Free function: lower_literal_int ──
std::uint32_t lower_literal_int(LoweringState& state, const ast::LiteralIntNode& node) {
    auto slot = state.alloc_local();
    auto val = static_cast<std::uint64_t>(static_cast<std::int64_t>(node.value));
    state.emit(IROpcode::ConstI64, slot, static_cast<std::uint32_t>(val),
         static_cast<std::uint32_t>(val >> 32));
    return slot;
}

std::uint32_t LoweringPass::lower_literal_int(const ast::LiteralIntNode& node) {
    return aura::compiler::lower_literal_int(*state_, node);
}


std::uint32_t LoweringPass::lower_variable(const ast::VariableNode& node) {
    // Look up in scope chain
    for (auto it = state_->scopes.rbegin(); it != state_->scopes.rend(); ++it) {
        auto found = it->find(node.name);
        if (found != it->end()) {
            auto& binding = found->second;
            auto slot = alloc_local();
            switch (binding.kind) {
            case BindingKind::Local:
                emit(IROpcode::Local, slot, binding.slot);
                break;
            case BindingKind::Captured:
                emit(IROpcode::Local, slot, binding.slot);
                break;
            case BindingKind::Cell:
                // Mutable cell (letrec): load via CellGet
                emit(IROpcode::CellGet, slot, binding.slot);
                break;
            }
            return slot;
        }
    }

    // Free variable from outer lambda's env (captured at closure creation)
    auto fv = state_->free_var_map.find(node.name);
    if (fv != state_->free_var_map.end()) {
        auto slot = alloc_local();
        emit(IROpcode::Local, slot, fv->second);
        return slot;
    }

    auto slot = alloc_local();
    emit(IROpcode::ConstI64, slot, 0);
    return slot;
}

// ─── Free variable collection ────────────────────────────────────

void LoweringPass::collect_free_vars(const ast::Expr* expr,
                                 std::unordered_set<std::string>& free, std::unordered_set<std::string>& bound) {
    
    if (!expr) return;

    std::visit([&](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::LiteralIntNode>) {
            // nothing
        } else if constexpr (std::is_same_v<T, ast::VariableNode>) {
            if (bound.find(node.name) == bound.end()) {
                free.insert(node.name);
            }
        } else if constexpr (std::is_same_v<T, ast::CallNode>) {
            collect_free_vars(node.function, free, bound);
            for (auto* a : node.args) {
                collect_free_vars(a, free, bound);
            }
        } else if constexpr (std::is_same_v<T, ast::IfExprNode>) {
            collect_free_vars(node.condition, free, bound);
            collect_free_vars(node.then_branch, free, bound);
            collect_free_vars(node.else_branch, free, bound);
        } else if constexpr (std::is_same_v<T, ast::LambdaNode>) {
            // For nested lambdas, the lambda's params are bound inside its body
            // but the lambda itself as an expression captures from its definition site
            std::unordered_set<std::string> inner_bound = bound;
            for (auto& p : node.params) inner_bound.insert(p);
            collect_free_vars(node.body, free, inner_bound);
            // NOTE: free vars in the body that are bound by outer scopes
            // are NOT free in the lambda expression — they're captured
        } else if constexpr (std::is_same_v<T, ast::LetNode>) {
            collect_free_vars(node.value, free, bound);
            std::unordered_set<std::string> inner_bound = bound;
            inner_bound.insert(node.name);
            collect_free_vars(node.body, free, inner_bound);
        } else if constexpr (std::is_same_v<T, ast::LetRecNode>) {
            std::unordered_set<std::string> inner_bound = bound;
            inner_bound.insert(node.name);
            collect_free_vars(node.value, free, inner_bound);
            collect_free_vars(node.body, free, inner_bound);
        } else if constexpr (std::is_same_v<T, ast::DefineNode>) {
            collect_free_vars(node.value, free, bound);
        } else if constexpr (std::is_same_v<T, ast::BeginNode>) {
            for (auto* e : node.exprs) collect_free_vars(e, free, bound);
        } else if constexpr (std::is_same_v<T, ast::SetNode>) {
            collect_free_vars(node.value, free, bound);
        } else if constexpr (std::is_same_v<T, ast::QuoteNode>) {
            collect_free_vars(node.value, free, bound);
        }
    }, expr->payload);
    
}

// ─── Lambda lowering ─────────────────────────────────────────────

aura::ir::IRFunction LoweringPass::lower_lambda_body(
    const ast::LambdaNode& node, std::vector<std::string>& free_vars,
    const std::unordered_set<std::string>& cell_free_vars)
{
    // Create a new IRFunction for this lambda
    IRFunction func;
    func.name = "__lambda__";
    func.entry_block = 0;
    func.blocks.push_back({0, {}, {}});
    func.params = node.params;
    func.arg_count = static_cast<std::uint32_t>(node.params.size());

    // Save and swap state
    auto* saved_func = state_->cur_func;
    auto saved_block = state_->cur_block;
    auto saved_locals = state_->local_count;
    auto saved_scopes = state_->scopes;
    auto saved_env_slot = state_->env_slot;
    auto saved_fv_map = std::move(state_->free_var_map);

    state_->cur_func = &func;
    state_->cur_block = 0;
    state_->local_count = 0;
    state_->free_var_map.clear();

    // At runtime, env values are PREPENDED to the argument list,
    // so locals = [env[0], env[1], ..., arg[0], arg[1], ...].
    // Load captured free variables from the env prefix, then params.

    state_->env_slot = alloc_local();  // placeholder

    // Scope for this function: first env vars, then params
    state_->scopes.push_back({});

    // First, load captured free variables from the env prefix
    for (std::size_t i = 0; i < free_vars.size(); ++i) {
        auto& fv = free_vars[i];
        auto slot = alloc_local();
        emit(IROpcode::Arg, slot, static_cast<std::uint32_t>(i));
        if (cell_free_vars.count(fv)) {
            // Cell capture: env[i] is cell_id, load via CellGet
            auto result = alloc_local();
            emit(IROpcode::CellGet, result, slot);
            state_->scopes.back()[fv] = Binding{ BindingKind::Local, result };
        } else {
            state_->scopes.back()[fv] = Binding{ BindingKind::Captured, slot };
        }
        state_->free_var_map[fv] = static_cast<std::uint32_t>(i);
    }

    // Load parameters (args are AFTER env values in locals)
    for (std::size_t i = 0; i < node.params.size(); ++i) {
        auto slot = alloc_local();
        emit(IROpcode::Arg, slot,
             static_cast<std::uint32_t>(free_vars.size() + i));
        state_->scopes.back()[node.params[i]] = Binding{ BindingKind::Local, slot };
    }

    // Lower body
    auto result_slot = lower_expr(node.body);
    emit(IROpcode::Return, result_slot);

    func.local_count = state_->local_count;

    // Restore state
    state_->cur_func = saved_func;
    state_->cur_block = saved_block;
    state_->local_count = saved_locals;
    state_->scopes = std::move(saved_scopes);
    state_->env_slot = saved_env_slot;
    state_->free_var_map = std::move(saved_fv_map);

    return func;
}

std::uint32_t LoweringPass::lower_lambda(const ast::LambdaNode& node) {
    // Collect free variables
    std::unordered_set<std::string> free_set;
    std::unordered_set<std::string> bound;
    for (auto& p : node.params) bound.insert(p);
    collect_free_vars(node.body, free_set, bound);

    // Filter: only include free vars that are actually in scope
    std::vector<std::string> free_vars;
    for (auto& fv : free_set) {
        // Check if this free var is bound in an enclosing scope
        bool in_scope = false;
        for (auto it = state_->scopes.rbegin(); it != state_->scopes.rend(); ++it) {
            if (it->find(fv) != it->end()) {
                in_scope = true;
                break;
            }
        }
        if (in_scope) {
            free_vars.push_back(fv);
        }
    }

    // Lower the lambda body as a separate function
    auto lambda_func = lower_lambda_body(node, free_vars, state_->cell_free_vars);
    auto func_id = state_->module.add_function(std::move(lambda_func));

    // Emit MakeClosure in the current function
    auto closure_slot = alloc_local();
    emit(IROpcode::MakeClosure, closure_slot, func_id,
         static_cast<std::uint32_t>(free_vars.size()));

    // Capture each free variable into the closure
    for (std::size_t i = 0; i < free_vars.size(); ++i) {
        auto& fv = free_vars[i];
        for (auto it = state_->scopes.rbegin(); it != state_->scopes.rend(); ++it) {
            auto found = it->find(fv);
            if (found != it->end()) {
                auto& binding = found->second;
                if (binding.kind == BindingKind::Cell) {
                    // Cell binding (letrec): capture the cell_id (stored at binding.slot)
                    auto slot = alloc_local();
                    emit(IROpcode::Local, slot, binding.slot);  // copies cell_id
                    emit(IROpcode::Capture, closure_slot,
                         static_cast<std::uint32_t>(i), slot);
                } else {
                    auto slot = alloc_local();
                    emit(IROpcode::Local, slot, binding.slot);
                    emit(IROpcode::Capture, closure_slot,
                         static_cast<std::uint32_t>(i), slot);
                }
                break;
            }
        }
    }

    return closure_slot;
}

// ─── Call lowering ───────────────────────────────────────────────

std::uint32_t LoweringPass::lower_call(const ast::CallNode& node) {
    // Check if function is a primitive (variable reference in current scope)
    if (auto* var = std::get_if<ast::VariableNode>(&node.function->payload)) {
        // Check if it's a known primitive: + - * / = < > <= >=
        static const std::unordered_set<std::string> prims = {
            "+", "-", "*", "/", "=", "<", ">", "<=", ">="
        };
        if (prims.find(var->name) != prims.end()) {
            // Inline primitive: eval args, emit arithmetic op
            std::vector<std::uint32_t> arg_slots;
            for (auto* a : node.args) {
                arg_slots.push_back(lower_expr(a));
            }
            auto result_slot = alloc_local();

            static const std::unordered_map<std::string, IROpcode> prim_map = {
                {"+", IROpcode::Add}, {"-", IROpcode::Sub},
                {"*", IROpcode::Mul}, {"/", IROpcode::Div},
                {"=", IROpcode::Eq}, {"<", IROpcode::Lt},
                {">", IROpcode::Gt}, {"<=", IROpcode::Le},
                {">=", IROpcode::Ge},
            };

            auto it = prim_map.find(var->name);
            if (it != prim_map.end() && arg_slots.size() >= 1) {
                auto op = it->second;
                if (arg_slots.size() == 1) {
                    // Unary minus: emit Sub(0, arg)
                    if (var->name == "-") {
                        auto zero = alloc_local();
                        emit(IROpcode::ConstI64, zero, 0, 0);
                        emit(op, result_slot, zero, arg_slots[0]);
                    } else {
                        emit(op, result_slot, arg_slots[0], arg_slots[0]);
                    }
                } else {
                    // Binary
                    emit(op, result_slot, arg_slots[0], arg_slots[1]);
                }
            }
            return result_slot;
        }
    }

    // General function call: lower callee and args, emit Call
    auto callee_slot = lower_expr(node.function);
    
    // Reserve contiguous argument block = [base, base + arg_count)
    // This ensures all args are packed consecutively for the interpreter
    auto arg_base = state_->local_count;
    state_->local_count += static_cast<std::uint32_t>(node.args.size());
    
    std::size_t i = 0;
    for (auto* a : node.args) {
        auto val_slot = lower_expr(a);
        emit(IROpcode::Local, arg_base + static_cast<std::uint32_t>(i), val_slot);
        ++i;
    }

    auto result_slot = alloc_local();
    // Call(callee_slot, arg_base, arg_count, result_slot)
    emit(IROpcode::Call, callee_slot, arg_base,
         static_cast<std::uint32_t>(node.args.size()), result_slot);

    return result_slot;
}

// ─── If lowering ─────────────────────────────────────────────────

std::uint32_t LoweringPass::lower_if(const ast::IfExprNode& node) {
    auto cond_slot = lower_expr(node.condition);

    // Create then/else/merge blocks
    auto then_block = alloc_block();
    auto else_block = alloc_block();
    auto merge_block = alloc_block();

    emit(IROpcode::Branch, cond_slot, then_block, else_block);

    // Pre-allocate the result slot that both branches will write to
    // This acts as a pseudo-register for the phi value
    auto phi_slot = alloc_local();

    // Then block: compute value and store in phi_slot
    state_->cur_block = then_block;
    auto then_val = lower_expr(node.then_branch);
    emit(IROpcode::Local, phi_slot, then_val);
    emit(IROpcode::Jump, merge_block);
    state_->cur_func->blocks[then_block].successors.push_back(merge_block);

    // Else block: compute value and store in phi_slot
    state_->cur_block = else_block;
    auto else_val = lower_expr(node.else_branch);
    emit(IROpcode::Local, phi_slot, else_val);
    emit(IROpcode::Jump, merge_block);
    state_->cur_func->blocks[else_block].successors.push_back(merge_block);

    // Merge block: phi_slot now has the correct value from either branch
    state_->cur_block = merge_block;
    return phi_slot;
}

// ─── Begin lowering ──────────────────────────────────────────────

std::uint32_t LoweringPass::lower_begin(const ast::BeginNode& node) {
    std::uint32_t last_slot = 0;
    for (auto* e : node.exprs) {
        last_slot = lower_expr(e);
    }
    return last_slot;
}

// ─── Set lowering ────────────────────────────────────────────────

std::uint32_t LoweringPass::lower_set(const ast::SetNode& node) {
    auto val_slot = lower_expr(node.value);
    // Look up the variable in scope chain
    for (auto it = state_->scopes.rbegin(); it != state_->scopes.rend(); ++it) {
        auto found = it->find(node.name);
        if (found != it->end()) {
            emit(IROpcode::CellSet, found->second.slot, val_slot);
            return val_slot;
        }
    }
    // Not found — just return value
    return val_slot;
}

// ─── Let lowering ────────────────────────────────────────────────

std::uint32_t LoweringPass::lower_let(const ast::LetNode& node, bool is_rec) {
    if (is_rec) {
        // For letrec: allocate a mutable cell on the heap; the lambda body
        // will CellGet the current value at call time (after the cell is filled).
        auto cell_id_slot = alloc_local();
        emit(IROpcode::NewCell, cell_id_slot);

        // Bind name → CellGet(cell_id_slot).  Both the lambda body and the
        // letrec body will emit CellGet when referencing this name.
        state_->scopes.back()[node.name] = Binding{ BindingKind::Cell, cell_id_slot };
        state_->cell_free_vars.insert(node.name);

        // Evaluate the value (lambda).  The lambda's free-var capture will
        // see Cell binding and capture the cell_id, not the value-in-the-cell.
        auto val_slot = lower_expr(node.value);

        // Store the closure into the cell (now CellGet in the lambda body
        // will resolve to the actual closure at call time).
        emit(IROpcode::CellSet, cell_id_slot, val_slot);
        state_->cell_free_vars.erase(node.name);

        if (node.body) return lower_expr(node.body);
        return val_slot;
    }

    // Non-recursive let: evaluate value then bind
    auto val_slot = lower_expr(node.value);
    state_->scopes.back()[node.name] = Binding{ BindingKind::Local, val_slot };
    if (node.body) {
        return lower_expr(node.body);
    }
    return val_slot;
}

} // namespace aura::compiler
