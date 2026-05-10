module aura.compiler.lowering;
import std;

namespace aura::compiler {

using namespace aura::ir;

std::uint32_t LoweringPass::alloc_block() {
    auto id = static_cast<std::uint32_t>(func_.blocks.size());
    func_.blocks.push_back({id, {}, {}});
    return id;
}

void LoweringPass::emit(IROpcode op, std::uint32_t op0, std::uint32_t op1, std::uint32_t op2) {
    func_.blocks[current_block_].instructions.push_back({op, {op0, op1, op2}});
}

IRFunction LoweringPass::lower(const ast::Expr* expr) {
    func_ = {};
    local_count_ = 0;
    scopes_.clear();
    scopes_.push_back({});  // global scope

    // Create entry block
    current_block_ = 0;
    alloc_block();
    func_.entry_block = 0;

    // Lower the expression
    auto result_slot = lower_expr(expr);
    emit(IROpcode::Return, result_slot);

    func_.local_count = local_count_;
    return std::move(func_);
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
    // Look up in scope chain (local or arg)
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(node.name);
        if (found != it->end()) {
            auto slot = alloc_local();
            emit(IROpcode::Local, slot, found->second);
            return slot;
        }
    }
    // Not found: push as 0 (will cause eval error)
    auto slot = alloc_local();
    emit(IROpcode::ConstI64, slot, 0);
    return slot;
}

std::uint32_t LoweringPass::lower_call(const ast::CallNode& node) {
    // Lower function expression
    auto func_slot = lower_expr(node.function);
    // Lower args
    for (auto* arg : node.args) {
        lower_expr(arg);
    }
    // For Phase 1a, calls are not lowered — just return function value
    return func_slot;
}

std::uint32_t LoweringPass::lower_if(const ast::IfExprNode& node) {
    auto cond_slot = lower_expr(node.condition);

    // Create then/else/merge blocks
    auto then_block = alloc_block();
    auto else_block = alloc_block();
    auto merge_block = alloc_block();

    emit(IROpcode::Branch, cond_slot, then_block, else_block);

    // Then block
    current_block_ = then_block;
    auto then_slot = lower_expr(node.then_branch);
    emit(IROpcode::Jump, merge_block);
    func_.blocks[then_block].successors.push_back(merge_block);

    // Else block
    current_block_ = else_block;
    auto else_slot = lower_expr(node.else_branch);
    emit(IROpcode::Jump, merge_block);
    func_.blocks[else_block].successors.push_back(merge_block);

    // Merge block (phi not needed for Phase 1a — just use both slots)
    current_block_ = merge_block;
    // Return then_slot for simplicity (correct if both branches produce same type)
    return then_slot;
}

std::uint32_t LoweringPass::lower_lambda(const ast::LambdaNode& node) {
    // For Phase 1a, lambdas are not lowered — just return a special value
    auto slot = alloc_local();
    emit(IROpcode::ConstI64, slot, 0);
    return slot;
}

std::uint32_t LoweringPass::lower_let(const ast::LetNode& node, bool is_rec) {
    // Evaluate the value expression
    auto val_slot = lower_expr(node.value);
    // Bind in current scope
    scopes_.back()[node.name] = val_slot;
    // Lower body
    auto body_slot = lower_expr(node.body);
    return body_slot;
}

} // namespace aura::compiler
