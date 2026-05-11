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
// Each session has:
//   - A pmr-based ASTArena (bulk allocation, one-shot reset)
//   - A persistent Evaluator with pristine top-level environment
//   - Optional: parser, lowering, IR interpreter all share the arena
//
// Typical usage per eval request:
//   1. Reset the arena (reclaims all AST memory)
//   2. Parse input into arena-allocated AST
//   3. Evaluate (tree-walker or IR pipeline)
//   4. Return result; arena stays ready for next input
//
// For REPL / Compiler-as-a-Service mode:
//   - Keep the same CompilerService across inputs
//   - evaluator state (closures, binds) persists across resets
//
export class CompilerService {
public:
    CompilerService()
        : parser_(arena_)
    {
        evaluator_.set_arena(&arena_);
    }

    // ---- Session lifecycle -------------------------------------------

    // Reset arena memory. Call between compilation requests.
    // Parser reference stays valid — arena is same object, just emptied.
    void reset() {
        arena_.reset();
    }

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

        aura::compiler::LoweringPass lowering(arena_);
        auto ir_mod = lowering.lower(pr.root);

        aura::compiler::IRInterpreter ir_interp(ir_mod, evaluator_.primitives());
        return ir_interp.execute();
    }

    // ---- Accessors ---------------------------------------------------

    ast::ASTArena& arena() { return arena_; }
    Evaluator& evaluator() { return evaluator_; }
    aura::parser::Parser& parser() { return parser_; }

private:
    ast::ASTArena arena_;
    aura::parser::Parser parser_;
    Evaluator evaluator_;
};

} // namespace aura::compiler
