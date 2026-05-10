export module aura.compiler.ir;
import std;
import aura.core;

namespace aura::ir {

export enum class IROpcode : std::uint8_t {
    Nop,
    // Data
    ConstI64,       // load int64 constant
    Local,          // load local variable (slot index)
    Arg,            // load function argument (slot index)
    // Arithmetic
    Add, Sub, Mul, Div,
    // Comparison (return 0 or 1)
    Eq, Lt, Gt, Le, Ge,
    // Logic
    And, Or, Not,
    // Control flow
    Branch,         // conditional branch: cond, true_target, false_target
    Jump,           // unconditional branch: target
    Call,           // function call: callee_expr, arg_count, result_slot
    Return,         // return: value_slot
    // Closures
    MakeClosure,    // create closure: func_slot, env_size
    Capture,        // capture variable: env_slot, var_slot
    Apply,          // apply closure: closure_slot, arg_count, result_slot
};

export struct IRInstruction {
    IROpcode opcode;
    std::array<std::uint32_t, 3> operands = {};
    std::uint32_t source_ast_node_id = 0;
};

export struct BasicBlock {
    std::uint32_t id = 0;
    std::vector<IRInstruction> instructions;
    std::vector<std::uint32_t> successors;
};

export struct IRFunction {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t entry_block = 0;
    std::vector<BasicBlock> blocks;
    std::vector<std::string> params;
    std::vector<std::string> free_vars;
    std::uint32_t local_count = 0;  // number of local slots needed
};

} // namespace aura::ir
