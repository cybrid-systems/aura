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
    Capture,        // capture variable: closure_slot, env_idx, var_slot
    CaptureRef,     // capture by reference: closure_slot, env_idx, cell_slot
    Apply,          // apply closure: closure_slot, arg_count, result_slot
    // Mutable cells (for letrec)
    NewCell,        // allocate mutable cell: result_slot
    CellSet,        // write to cell: cell_id, value_slot
    CellGet,        // read from cell: result_slot, cell_id
};

export struct IRInstruction {
    IROpcode opcode;
    std::array<std::uint32_t, 4> operands = {};
    std::uint32_t source_ast_node_id = 0;
};

// Helper: pack two uint32 into one (for Call: args_begin << 16 | arg_count)
export constexpr std::uint32_t pack_pair(std::uint32_t hi, std::uint32_t lo) {
    return (hi << 16) | (lo & 0xFFFF);
}
export constexpr std::uint32_t unpack_hi(std::uint32_t p) { return p >> 16; }
export constexpr std::uint32_t unpack_lo(std::uint32_t p) { return p & 0xFFFF; }

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
    std::uint32_t arg_count = 0;    // number of arguments
};

export struct IRModule {
    std::vector<IRFunction> functions;
    std::uint32_t entry_function_id = 0;

    std::uint32_t add_function(IRFunction func) {
        func.id = static_cast<std::uint32_t>(functions.size());
        functions.push_back(std::move(func));
        return func.id;
    }

    void set_entry(std::uint32_t id) { entry_function_id = id; }
    IRFunction& entry() { return functions[entry_function_id]; }
    const IRFunction& entry() const { return functions[entry_function_id]; }
};

} // namespace aura::ir
