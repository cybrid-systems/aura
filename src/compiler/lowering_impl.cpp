module aura.compiler.lowering;
import std;

namespace aura::compiler {

using namespace aura::ir;

std::uint32_t LoweringPass::alloc_block() {
    auto id = static_cast<std::uint32_t>(cur_func_->blocks.size());
    cur_func_->blocks.push_back({id, {}, {}});
    return id;
}

void LoweringPass::emit(IROpcode op, std::uint32_t op0, std::uint32_t op1, std::uint32_t op2, std::uint32_t op3) {
    cur_func_->blocks[current_block_].instructions.push_back({op, {op0, op1, op2, op3}});
}

IRModule LoweringPass::lower(const ast::Expr* expr) {
    module_ = {};
    scopes_.clear();

    // Create the top-level function
    IRFunction top_func;
    top_func.name = "__top__";
    top_func.entry_block = 0;
    top_func.blocks.push_back({0, {}, {}});
    top_func.arg_count = 0;
    cur_func_ = &top_func;
    current_block_ = 0;
    local_count_ = 0;

    // Push empty scope for top-level (no parameters)
    scopes_.push_back({});

    // Lower the expression
    auto result_slot = lower_expr(expr);
    emit(IROpcode::Return, result_slot);

    top_func.local_count = local_count_;
    auto top_id = module_.add_function(std::move(top_func));
    module_.set_entry(top_id);
    return std::move(module_);
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
        auto s = alloc_local(); emit(IROpcode::ConstI64, s, 0); return s;
    }, expr->payload);
}

std::uint32_t LoweringPass::lower_literal_int(const ast::LiteralIntNode& node) {
    auto slot = alloc_local();
    auto val = static_cast<std::uint64_t>(static_cast<std::int64_t>(node.value));
    emit(IROpcode::ConstI64, slot, static_cast<std::uint32_t>(val),
         static_cast<std::uint32_t>(val >> 32));
    return slot;
}

std::uint32_t LoweringPass::lower_variable(const ast::VariableNode& node) {
    // Look up in scope chain (local, captured, or arg)
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(node.name);
        if (found != it->end()) {
            auto& binding = found->second;
            auto slot = alloc_local();
            if (binding.kind == BindingKind::Local) {
                // Local variable: load from local slot
                emit(IROpcode::Local, slot, binding.slot);
            } else {
                // Captured: load from env object (Capture retrieves value)
                emit(IROpcode::Local, slot, binding.slot);
            }
            return slot;
        }
    }

    // Check if it's a free variable from outer lambda's free_var_map
    auto fv = free_var_map_.find(node.name);
    if (fv != free_var_map_.end()) {
        // This variable is captured in the closure's env
        // Load from env: first load env pointer, then index into it
        auto slot = alloc_local();
        // For the current function, the env is at env_slot_ after MakeClosure
        // The Capture opcode stores captured vars into env slots during closure creation
        emit(IROpcode::Local, slot, fv->second);
        return slot;
    }

    // Not found: push as 0 (will cause eval error)
    auto slot = alloc_local();
    emit(IROpcode::ConstI64, slot, 0);
    return slot;
}

// ─── Free variable collection ────────────────────────────────────

void LoweringPass::collect_free_vars(const ast::Expr* expr,
                                      std::unordered_set<std::string>& free,
                                      std::unordered_set<std::string>& bound) {
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
        }
    }, expr->payload);
}

// ─── Lambda lowering ─────────────────────────────────────────────

aura::ir::IRFunction LoweringPass::lower_lambda_body(
    const ast::LambdaNode& node, std::vector<std::string>& free_vars)
{
    // Create a new IRFunction for this lambda
    IRFunction func;
    func.name = "__lambda__";
    func.entry_block = 0;
    func.blocks.push_back({0, {}, {}});
    func.params = node.params;
    func.arg_count = static_cast<std::uint32_t>(node.params.size());

    // Save and swap state
    auto* saved_func = cur_func_;
    auto saved_block = current_block_;
    auto saved_locals = local_count_;
    auto saved_scopes = scopes_;
    auto saved_env_slot = env_slot_;
    auto saved_fv_map = std::move(free_var_map_);

    cur_func_ = &func;
    current_block_ = 0;
    local_count_ = 0;
    free_var_map_.clear();

    // At runtime, env values are PREPENDED to the argument list,
    // so locals = [env[0], env[1], ..., arg[0], arg[1], ...].
    // Load captured free variables from the env prefix, then params.

    env_slot_ = alloc_local();  // placeholder

    // Scope for this function: first env vars, then params
    scopes_.push_back({});

    // First, load captured free variables from the env prefix of the args array
    for (std::size_t i = 0; i < free_vars.size(); ++i) {
        auto slot = alloc_local();
        // Env values are at args[0..free_vars.size()-1]
        emit(IROpcode::Arg, slot, static_cast<std::uint32_t>(i));
        scopes_.back()[free_vars[i]] = Binding{ BindingKind::Captured, slot };
        free_var_map_[free_vars[i]] = static_cast<std::uint32_t>(i);
    }

    // Load parameters (args are AFTER env values in locals)
    for (std::size_t i = 0; i < node.params.size(); ++i) {
        auto slot = alloc_local();
        emit(IROpcode::Arg, slot,
             static_cast<std::uint32_t>(free_vars.size() + i));
        scopes_.back()[node.params[i]] = Binding{ BindingKind::Local, slot };
    }

    // Lower body
    auto result_slot = lower_expr(node.body);
    emit(IROpcode::Return, result_slot);

    func.local_count = local_count_;

    // Restore state
    cur_func_ = saved_func;
    current_block_ = saved_block;
    local_count_ = saved_locals;
    scopes_ = std::move(saved_scopes);
    env_slot_ = saved_env_slot;
    free_var_map_ = std::move(saved_fv_map);

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
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
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
    auto lambda_func = lower_lambda_body(node, free_vars);
    auto func_id = module_.add_function(std::move(lambda_func));

    // Emit MakeClosure in the current function
    auto closure_slot = alloc_local();
    emit(IROpcode::MakeClosure, closure_slot, func_id,
         static_cast<std::uint32_t>(free_vars.size()));

    // Capture each free variable into the closure
    for (std::size_t i = 0; i < free_vars.size(); ++i) {
        auto& fv = free_vars[i];
        // Find the variable's slot in current scope
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(fv);
            if (found != it->end()) {
                if (byref_captures_.count(fv)) {
                    // Capture by reference: store the slot index, not the value
                    emit(IROpcode::CaptureRef, closure_slot,
                         static_cast<std::uint32_t>(i),
                         static_cast<std::uint32_t>(found->second.slot));
                } else {
                    auto slot = alloc_local();
                    emit(IROpcode::Local, slot, found->second.slot);
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
    auto arg_base = local_count_;
    local_count_ += static_cast<std::uint32_t>(node.args.size());
    
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
    current_block_ = then_block;
    auto then_val = lower_expr(node.then_branch);
    emit(IROpcode::Local, phi_slot, then_val);
    emit(IROpcode::Jump, merge_block);
    cur_func_->blocks[then_block].successors.push_back(merge_block);

    // Else block: compute value and store in phi_slot
    current_block_ = else_block;
    auto else_val = lower_expr(node.else_branch);
    emit(IROpcode::Local, phi_slot, else_val);
    emit(IROpcode::Jump, merge_block);
    cur_func_->blocks[else_block].successors.push_back(merge_block);

    // Merge block: phi_slot now has the correct value from either branch
    current_block_ = merge_block;
    return phi_slot;
}

// ─── Let lowering ────────────────────────────────────────────────

std::uint32_t LoweringPass::lower_let(const ast::LetNode& node, bool is_rec) {
    if (is_rec) {
        // For letrec: pre-allocate a slot for the binding, then evaluate the value
        auto cell_slot = alloc_local();
        emit(IROpcode::ConstI64, cell_slot, 0, 0);
        // Bind before evaluating — mark as byref so captures use CaptureRef
        scopes_.back()[node.name] = Binding{ BindingKind::Local, cell_slot };
        byref_captures_.insert(node.name);
        // Evaluate the value (lambda), which will capture 'fact' by reference
        auto val_slot = lower_expr(node.value);
        // Store the result into the cell (now captured refs will see the value)
        emit(IROpcode::Local, cell_slot, val_slot);
        byref_captures_.erase(node.name);
        // Lower body
        if (node.body) {
            return lower_expr(node.body);
        }
        return val_slot;
    }

    // Non-recursive let: evaluate value then bind
    auto val_slot = lower_expr(node.value);
    scopes_.back()[node.name] = Binding{ BindingKind::Local, val_slot };
    if (node.body) {
        return lower_expr(node.body);
    }
    return val_slot;
}

} // namespace aura::compiler
