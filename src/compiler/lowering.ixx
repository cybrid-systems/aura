export module aura.compiler.lowering;
import std;
import aura.core;
import aura.compiler.ir;

namespace aura::compiler {

export class LoweringPass {
public:
    explicit LoweringPass(ast::ASTArena& arena) : arena_(arena) {}

    // Lower an Expr AST to an IRFunction
    aura::ir::IRFunction lower(const ast::Expr* expr);

private:
    // Allocate a new local slot
    std::uint32_t alloc_local() { return local_count_++; }
    std::uint32_t alloc_block();

    // Emit a single IR instruction into the current block
    void emit(aura::ir::IROpcode op, std::uint32_t op0 = 0,
              std::uint32_t op1 = 0, std::uint32_t op2 = 0);

    // Recursively lower an expression, return result slot
    std::uint32_t lower_expr(const ast::Expr* expr);

    std::uint32_t lower_literal_int(const ast::LiteralIntNode& node);
    std::uint32_t lower_variable(const ast::VariableNode& node);
    std::uint32_t lower_call(const ast::CallNode& node);
    std::uint32_t lower_if(const ast::IfExprNode& node);
    std::uint32_t lower_lambda(const ast::LambdaNode& node);
    std::uint32_t lower_let(const ast::LetNode& node, bool is_rec);

    ast::ASTArena& arena_;
    aura::ir::IRFunction func_;
    std::uint32_t current_block_ = 0;
    std::uint32_t local_count_ = 0;
    // Symbol → slot mapping
    std::vector<std::unordered_map<std::string, std::uint32_t>> scopes_;
};

} // namespace aura::compiler
