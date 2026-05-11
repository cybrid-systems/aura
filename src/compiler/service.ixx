export module aura.compiler.service;
import std;
import aura.core;
import aura.parser.parser;
import aura.compiler.frontend;
import aura.compiler.ir;
import aura.compiler.lowering;
import aura.compiler.ir_interpreter;
import aura.compiler.pass_manager;

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

    void reset() { default_arena_.reset(); }

    // ---- Tree-walker evaluation --------------------------------------

    EvalResult eval(std::string_view input) {
        auto pr = parser_.parse(std::string(input));
        if (!pr.success || !pr.root) {
            return {false, 0, "parse error"};
        }
        return evaluator_.eval(pr.root);
    }

    // ---- IR pipeline ------------------------------------------------

    // Default pipeline: lower → passes → execute
    EvalResult eval_ir(std::string_view input) {
        auto pr = parser_.parse(std::string(input));
        if (!pr.success || !pr.root) {
            return {false, 0, "parse error"};
        }

        aura::compiler::LoweringPass lowering(default_arena_);
        auto ir_mod = lowering.lower(pr.root);

        // Run the standard pass pipeline
        pass_mgr_.clear();
        auto& ck = pass_mgr_.emplace<ComputeKindWrap>();
        auto& ar = pass_mgr_.emplace<ArityWrap>();
        auto& cf = pass_mgr_.emplace<ConstantFoldingWrap>();
        pass_mgr_.run(ir_mod);

        if (cf.did_fold()) {
            std::println(std::cerr, "PM: folded {} instructions", cf.folded_count());
        }

        // Arity check failure → stop with diagnostic
        if (ar.has_error()) {
            std::string errs;
            for (auto& d : ar.result().diagnostics) {
                errs += d.message + "\n";
            }
            return {false, 0, errs};
        }

        aura::compiler::IRInterpreter ir_interp(ir_mod, evaluator_.primitives());
        return ir_interp.execute();
    }

    // ---- Pass manager access -----------------------------------------

    PassManager& passes() { return pass_mgr_; }

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
        auto s = default_arena_.stats();
        s.merge(arena_group_.total_stats());
        return s;
    }

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
    PassManager pass_mgr_;
};

} // namespace aura::compiler
