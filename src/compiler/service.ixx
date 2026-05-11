export module aura.compiler.service;
import std;
import aura.core;
import aura.parser.parser;
import aura.compiler.frontend;
import aura.compiler.ir;
import aura.compiler.lowering;
import aura.compiler.ir_interpreter;

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
    CompilerService()
        : parser_(default_arena_)
    {
        evaluator_.set_arena(&default_arena_);
    }

    // ---- Session lifecycle -------------------------------------------

    // Reset the default arena. Call between compilation requests.
    void reset() { default_arena_.reset(); }

    // ---- Tree-walker evaluation --------------------------------------

    // Parse and evaluate via the tree-walker
    EvalResult eval(std::string_view input) {
        auto pr = parser_.parse(std::string(input));
        if (!pr.success || !pr.root) {
            return {false, 0, "parse error"};
        }
        return evaluator_.eval(pr.root);
    }

    // ---- IR pipeline ------------------------------------------------

    // Parse, lower to IR, and execute via IR interpreter
    EvalResult eval_ir(std::string_view input) {
        auto pr = parser_.parse(std::string(input));
        if (!pr.success || !pr.root) {
            return {false, 0, "parse error"};
        }

        aura::compiler::LoweringPass lowering(default_arena_);
        auto ir_mod = lowering.lower(pr.root);

        aura::compiler::IRInterpreter ir_interp(ir_mod, evaluator_.primitives());
        return ir_interp.execute();
    }

    // ---- Multi-module arena support ----------------------------------

    // Get or create an isolated arena for a named module.
    // Each module gets its own bump allocator; resetting one module
    // does not affect others.
    ast::ASTArena& module_arena(const std::string& name,
                                std::size_t initial_size = 8 * 1024 * 1024) {
        return arena_group_.module_arena(name, initial_size);
    }

    // Reset a specific module's arena (reclaims its AST memory)
    void reset_module(const std::string& name) {
        arena_group_.reset_module(name);
    }

    // ---- Diagnostics ------------------------------------------------

    // Snapshot of current memory usage across all arenas
    ast::ArenaStats memory_stats() const {
        auto s = default_arena_.stats();
        s.merge(arena_group_.total_stats());
        return s;
    }

    // Per-module memory breakdown
    std::vector<std::pair<std::string, ast::ArenaStats>>
    module_memory_stats() const {
        return arena_group_.module_stats();
    }

    // ---- Accessors ---------------------------------------------------

    ast::ASTArena& arena() { return default_arena_; }
    Evaluator& evaluator() { return evaluator_; }
    aura::parser::Parser& parser() { return parser_; }

private:
    ast::ASTArena default_arena_;
    ast::ArenaGroup arena_group_;
    aura::parser::Parser parser_;
    Evaluator evaluator_;
};

} // namespace aura::compiler
