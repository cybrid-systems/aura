export module aura.compiler.lowering;
import std;
import aura.core;
import aura.compiler.ir;

namespace aura::compiler {

// A slot binding: either a local variable (slot index) or captured (env slot)
enum class BindingKind : std::uint8_t { Local, Captured };

struct Binding {
    BindingKind kind;
    std::uint32_t slot;
};

export class LoweringPass {
public:
    explicit LoweringPass(ast::ASTArena& arena) : arena_(arena) {}

    // Lower an Expr AST to an IRModule
    aura::ir::IRModule lower(const ast::Expr* expr);

private:
    // Allocate a new local slot
    std::uint32_t alloc_local() { return local_count_++; }
    std::uint32_t alloc_block();

    // Emit a single IR instruction into the current block
    void emit(aura::ir::IROpcode op, std::uint32_t op0 = 0,
              std::uint32_t op1 = 0, std::uint32_t op2 = 0, std::uint32_t op3 = 0);

    // Recursively lower an expression into the current function, return result slot
    std::uint32_t lower_expr(const ast::Expr* expr);

    std::uint32_t lower_literal_int(const ast::LiteralIntNode& node);
    std::uint32_t lower_variable(const ast::VariableNode& node);
    std::uint32_t lower_call(const ast::CallNode& node);
    std::uint32_t lower_if(const ast::IfExprNode& node);
    std::uint32_t lower_lambda(const ast::LambdaNode& node);
    std::uint32_t lower_let(const ast::LetNode& node, bool is_rec);

    // Find free variables in an expression (variables not bound in scope)
    void collect_free_vars(const ast::Expr* expr,
                           std::unordered_set<std::string>& free,
                           std::unordered_set<std::string>& bound);

    // Lower a lambda body as a separate IR function
    aura::ir::IRFunction lower_lambda_body(const ast::LambdaNode& node,
                                            std::vector<std::string>& free_vars);

    // Set of variables that should be captured by reference (for letrec)
    std::unordered_set<std::string> byref_captures_;

    ast::ASTArena& arena_;
    aura::ir::IRModule module_;
    aura::ir::IRFunction* cur_func_ = nullptr;
    std::uint32_t current_block_ = 0;
    std::uint32_t local_count_ = 0;
    // Scope chain: symbol → Binding
    std::vector<std::unordered_map<std::string, Binding>> scopes_;
    // For env access in closure: current env slot index in the function
    std::uint32_t env_slot_ = 0;
    // Free variable map: name → env slot index within current env
    std::unordered_map<std::string, std::uint32_t> free_var_map_;
};

} // namespace aura::compiler
