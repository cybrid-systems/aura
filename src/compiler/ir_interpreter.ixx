export module aura.compiler.ir_interpreter;
import std;
import aura.core;
import aura.compiler.ir;
import aura.compiler.frontend;  // for EvalResult

namespace aura::compiler {

// Runtime closure value
export struct IRClosure {
    std::uint32_t func_id = 0;
    std::vector<std::int64_t> env;
};

// Call frame for recursive IR execution
struct CallFrame {
    const aura::ir::IRFunction* func = nullptr;
    std::uint32_t current_block = 0;
    std::vector<std::int64_t> locals;
    std::size_t instr_index = 0;
};

// IR interpreter — lowered code execution with closure support
export class IRInterpreter {
public:
    explicit IRInterpreter(const aura::ir::IRModule& mod,
                           const Primitives& prims)
        : module_(mod), primitives_(prims) {}

    // Execute the top-level function and return result
    EvalResult execute();

private:
    // Execute a specific function with given args
    EvalResult execute_function(const aura::ir::IRFunction& func,
                                 const std::vector<std::int64_t>& args);

    // Step through instructions (args are separate from locals for Arg opcode)
    EvalResult run_function(const aura::ir::IRFunction& func,
                             std::vector<std::int64_t>& locals,
                             const std::vector<std::int64_t>& args);

    const aura::ir::IRModule& module_;
    const Primitives& primitives_;

    // Per-instance closure storage
    std::uint64_t next_closure_id_ = 1;
    std::unordered_map<std::uint64_t, IRClosure> runtime_closures_;
};

} // namespace aura::compiler
