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

// ── LoweringState — all mutable lowering state ────────────────
struct LoweringState {
    ast::ASTArena& arena;
    aura::ir::IRModule module;
    aura::ir::IRFunction* cur_func = nullptr;
    std::uint32_t cur_block = 0;
    std::uint32_t local_count = 0;
    std::vector<std::unordered_map<std::string, Binding>> scopes;
    std::uint32_t env_slot = 0;
    std::unordered_map<std::string, std::uint32_t> free_var_map;
    std::unordered_set<std::string> cell_free_vars;

    explicit LoweringState(ast::ASTArena& a) : arena(a) {}
    std::uint32_t alloc_local() { return local_count++; }

    void emit(aura::ir::IROpcode op, std::uint32_t op0, std::uint32_t op1 = 0, std::uint32_t op2 = 0, std::uint32_t op3 = 0) {
        if (!cur_func || cur_block >= cur_func->blocks.size()) return;
        auto& blk = cur_func->blocks[cur_block];
        blk.instructions.push_back({op, {op0, op1, op2, op3}});
    }
    std::uint32_t alloc_block() {
        if (!cur_func) return 0;
        cur_func->blocks.push_back({
            static_cast<std::uint32_t>(cur_func->blocks.size())});
        return static_cast<std::uint32_t>(cur_func->blocks.size() - 1);
    }
};


// Internal lowering state machine (implementation detail).
// Prefer the free function lower_to_ir() for most use cases.
export class LoweringPass {
    friend aura::ir::IRModule lower_to_ir(const ast::Expr*, ast::ASTArena&);
public:
    explicit LoweringPass(ast::ASTArena& arena) : state_(new LoweringState(arena)) {}
    aura::ir::IRModule lower(const ast::Expr* expr);

private:
    std::uint32_t alloc_block() { return state_->alloc_block(); }
    std::uint32_t alloc_local() { return state_->alloc_local(); }
    void emit(aura::ir::IROpcode op, std::uint32_t o0=0, std::uint32_t o1=0, std::uint32_t o2=0, std::uint32_t o3=0) { state_->emit(op, o0, o1, o2, o3); }
    LoweringState* state_ = nullptr;

    // Allocate a new local slot

    // Emit a single IR instruction into the current block

    // Recursively lower an expression into the current function, return result slot
    std::uint32_t lower_expr(const ast::Expr* expr);

    std::uint32_t lower_literal_int(const ast::LiteralIntNode& node);
    std::uint32_t lower_variable(const ast::VariableNode& node);
    std::uint32_t lower_call(const ast::CallNode& node);
    std::uint32_t lower_if(const ast::IfExprNode& node);
    std::uint32_t lower_begin(const ast::BeginNode& node);
    std::uint32_t lower_set(const ast::SetNode& node);
    std::uint32_t lower_lambda(const ast::LambdaNode& node);
    std::uint32_t lower_let(const ast::LetNode& node, bool is_rec);

    // Find free variables in an expression (variables not bound in scope)
    // Returns pair of (free_vars, bound_vars)
    // Find free variables in an expression (variables not bound in scope)
    // Legacy: pass in/out sets by reference
    void collect_free_vars(const ast::Expr* expr,
                           std::unordered_set<std::string>& free,
                           std::unordered_set<std::string>& bound);
    // Preferred: returns pair of (free_vars, bound_vars)
    std::pair<std::unordered_set<std::string>, std::unordered_set<std::string>>
    collect_free_vars2(const ast::Expr* expr,
                       std::unordered_set<std::string> bound = {});

    // Lower a lambda body as a separate IR function
    // cell_free_vars: subset of free_vars that are letrec cell references
    aura::ir::IRFunction lower_lambda_body(const ast::LambdaNode& node,
                                            std::vector<std::string>& free_vars,
                                            const std::unordered_set<std::string>& cell_free_vars = {});


    aura::ir::IRFunction* cur_func_ = nullptr;
    std::uint32_t current_block_ = 0;
    std::uint32_t local_count_ = 0;
    std::uint32_t env_slot_ = 0;
    // Free variable map: name → env slot index within current env
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
