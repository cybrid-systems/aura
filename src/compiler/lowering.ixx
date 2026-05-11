export module aura.compiler.lowering;
import std;
import aura.core;
import aura.core.ast_flat;
import aura.core.ast_pool;
import aura.compiler.ir;

namespace aura::compiler {

// A slot binding: either a local variable (slot index) or captured (env slot)
enum class BindingKind : std::uint8_t { Local, Captured, Cell };

struct Binding {
    BindingKind kind;
    std::uint32_t slot;
};

// Internal lowering state machine (implementation detail).
// Prefer the free function lower_to_ir() for most use cases.
export class LoweringPass {
    friend aura::ir::IRModule lower_to_ir(const ast::Expr*, ast::ASTArena&);
public:
    explicit LoweringPass(ast::ASTArena& arena) : arena_(arena) {}
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
    // cell_free_vars: subset of free_vars that are letrec cell references
    aura::ir::IRFunction lower_lambda_body(const ast::LambdaNode& node,
                                            std::vector<std::string>& free_vars,
                                            const std::unordered_set<std::string>& cell_free_vars = {});

    // Set of variables that should be captured as cell references (for letrec)
    std::unordered_set<std::string> cell_free_vars_;

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

// Free function — preferred API (Expr* tree path).
// Lowers an AST expression to an IRModule in a single call.
export inline aura::ir::IRModule lower_to_ir(const ast::Expr* expr,
                                              ast::ASTArena& arena) {
    LoweringPass lowering(arena);
    return lowering.lower(expr);
}

// Free function — FlatAST path.
// Lowers a flat index-based AST to an IRModule.
// Internally reconstructs Expr* from FlatAST in the given arena,
// then delegates to the Expr* lower_to_ir (Phase 3 bridge).
// Phase 4+ will implement a native FlatAST→IR lowering.
export aura::ir::IRModule lower_to_ir(ast::FlatAST& flat,
                                       ast::StringPool& pool,
                                       ast::ASTArena& arena);

// Reconstruct an Expr* tree from FlatAST (for tree-walker evaluator).
// Phase 4 bridge: parser → FlatAST → reconstruct → Expr* → evaluator.
export ast::Expr* reconstruct_expr(ast::FlatAST& flat,
                                    ast::StringPool& pool,
                                    ast::ASTArena& arena);

} // namespace aura::compiler
