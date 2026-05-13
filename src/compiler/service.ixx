export module aura.compiler.service;
import std;
import aura.core;
import aura.parser.parser;
import aura.compiler.frontend;
import aura.compiler.ir;
import aura.compiler.lowering;
import aura.compiler.ir_interpreter;
import aura.compiler.pass_manager;
import aura.core.ast_flat;
import aura.core.ast_pool;
import aura.diag;

namespace aura::compiler {

// CompilerService — owns a full compilation session's lifecycle.
//
// Each request creates a fresh AST in the arena; after eval, arena
// is reset for the next request. Evaluator state (closures, defines)
// persists across resets.
//
// For multi-module scenarios, use module_arena() to get an isolated
// arena that can be independently reset.
//
export class CompilerService {
public:
    CompilerService() {
        evaluator_.set_arena(&arena_);
    }

    void reset() { arena_.reset(); }

    // ---- Tree-walker evaluation --------------------------------------

    EvalResult eval(std::string_view input) {
        // Phase 4: parse directly into FlatAST, evaluator reads FlatAST directly.
        auto alloc = arena_.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(input, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::ParseError, pr.error.empty() ? "parse error" : pr.error});
        }
        flat.root = pr.root;
        return evaluator_.eval_flat(flat, pool, flat.root, evaluator_.top_env());
    }

    // ---- IR pipeline ------------------------------------------------

    EvalResult eval_ir(std::string_view input) {
        // Phase 4: parse directly into FlatAST (bypasses Expr* entirely)
        auto alloc = arena_.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(input, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::ParseError, pr.error});
        }
        flat.root = pr.root;

        // Check if AST contains MacroDef nodes — IR pipeline doesn't
        // support macros. If found, fall back to tree-walker evaluator.
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            if (flat.get(id).tag == aura::ast::NodeTag::MacroDef) {
                auto* expr = aura::compiler::reconstruct_expr(flat, pool, arena_);
                if (!expr) return std::unexpected(
                    aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                           "macro expand: reconstruct failed"});
                return evaluator_.eval(expr);
            }
        }

        auto ir_mod = aura::compiler::lower_to_ir(flat, pool, arena_);

        ComputeKindWrap ck;
        ArityWrap ar;
        ConstantFoldingWrap cf;

        std::println(std::cerr, "PM: running {}→{}→{}",
                     ck.name(), ar.name(), cf.name());

        ck.run(ir_mod);
        ar.run(ir_mod);
        cf.run(ir_mod);

        if (ar.has_error()) {
            for (auto& d : ar.result().diagnostics) {
                std::println(std::cerr, "arith: {}", d.message);
            }
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::ArityMismatch, "arity check failed"});
        }

        if (cf.folded_count() > 0) {
            std::println(std::cerr, "PM: folded {} instructions", cf.folded_count());
        }

        aura::compiler::IRInterpreter ir_interp(ir_mod, evaluator_.primitives());
        ir_interp.set_strategy(strategy_);
        auto result = ir_interp.execute();

        // Capture runtime state for --inspect
        last_closures_ = ir_interp.list_closures();
        last_cells_ = ir_interp.list_cells();

        return result;
    }

    // ---- Multi-module arena support ----------------------------------

    ast::ASTArena& module_arena(const std::string& name,
                                std::size_t initial_size = 8 * 1024 * 1024) {
        return arena_group_.module_arena(name, initial_size);
    }

    void reset_module(const std::string& name) {
        arena_group_.reset_module(name);
    }

    // ---- Diagnostics ------------------------------------------------

    ast::ArenaStats memory_stats() const {
        auto s = arena_.stats();
        s.merge(arena_group_.total_stats());
        return s;
    }

    std::vector<std::pair<std::string, ast::ArenaStats>>
    module_memory_stats() const {
        return arena_group_.module_stats();
    }

    // ---- Runtime reflection (M3 Phase 2) ------------------------------

    // Closures persisted from last IR execution
    std::vector<aura::compiler::ClosureSnapshot> last_closures() const {
        return last_closures_;
    }
    std::vector<aura::compiler::CellSnapshot> last_cells() const {
        return last_cells_;
    }
    const aura::compiler::EvalStrategy& strategy() const { return strategy_; }
    void set_strategy(const aura::compiler::EvalStrategy& s) { strategy_ = s; }

    // ---- Accessors ---------------------------------------------------

    ast::ASTArena& arena() { return arena_; }
    Evaluator& evaluator() { return evaluator_; }

private:
    ast::ASTArena arena_;
    ast::ArenaGroup arena_group_;
    Evaluator evaluator_;
    aura::compiler::EvalStrategy strategy_;
    std::vector<aura::compiler::ClosureSnapshot> last_closures_;
    std::vector<aura::compiler::CellSnapshot> last_cells_;
};

} // namespace aura::compiler
