export module aura.compiler.ir_interpreter;
import std;
import aura.core;
import aura.compiler.ir;
import aura.compiler.frontend;  // for Env, EvalResult, Evaluator

namespace aura::compiler {

// IR interpreter — lowered code execution
// For Phase 1a, evaluates IR by walking basic blocks
export class IRInterpreter {
public:
    explicit IRInterpreter(Env& env, const Primitives& prims)
        : env_(env), primitives_(prims) {}

    // Execute an IR function and return result
    EvalResult execute(const aura::ir::IRFunction& func);

private:
    // Stack frame for local variables and temporaries
    std::vector<std::int64_t> stack_;

    Env& env_;
    const Primitives& primitives_;
};

} // namespace aura::compiler
