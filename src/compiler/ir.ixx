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
    // Type coercion (L6.6b)
    CastOp,         // runtime type check: result_slot, value_slot, type_tag
    // String support
    ConstString,    // load string constant: result_slot, string_index
    // Primitive call (for non-arithmetic primitives like string ops)
    PrimCall,       // call prim by id: prim_id, packed_args(arg_begin, arg_count), result_slot
};

export struct IRInstruction {
    IROpcode opcode;
    std::array<std::uint32_t, 4> operands = {};
    std::uint32_t source_ast_node_id = 0;
};

// Primitive IDs for PrimCall opcode
export enum class PrimId : std::uint8_t {
    StringAppend,
    StringLength,
    StringRef,
    Substring,
    StringEq,
    StringLt,
    NumberToString,
    StringToNumber,
    Display,
    Write,
    Newline,
    Error,
    Assert,
    Read,
    ReadFile,
    WriteFile,
    FileExists,
    Gensym,
    // Vector primitives
    Vector,
    VectorRef,
    VectorSet,
    VectorLength,
    VectorP,
    MakeVector,
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

    std::vector<std::string> string_pool;  // string constants (for ConstString opcode)

    std::uint32_t add_string(std::string s) {
        auto id = static_cast<std::uint32_t>(string_pool.size());
        string_pool.push_back(std::move(s));
        return id;
    }

    void set_entry(std::uint32_t id) { entry_function_id = id; }
    IRFunction& entry() { return functions[entry_function_id]; }
    const IRFunction& entry() const { return functions[entry_function_id]; }

    // Hot-swap a function: replace the body while keeping the same id
    // All existing closures referencing func_id will use the new code
    // on their next invocation.
    bool hot_swap_function(std::uint32_t func_id, IRFunction new_func) {
        if (func_id >= functions.size()) return false;
        new_func.id = func_id;
        functions[func_id] = std::move(new_func);
        return true;
    }

    // Find all Call/MakeClosure instructions that reference a function
    std::vector<std::pair<std::uint32_t, std::uint32_t>>
    find_callers_of(std::uint32_t func_id) const {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> result;
        for (std::size_t fi = 0; fi < functions.size(); ++fi) {
            auto& f = functions[fi];
            for (std::size_t bi = 0; bi < f.blocks.size(); ++bi) {
                auto& b = f.blocks[bi];
                for (std::size_t ii = 0; ii < b.instructions.size(); ++ii) {
                    auto& instr = b.instructions[ii];
                    if (instr.opcode == IROpcode::MakeClosure &&
                        instr.operands.size() > 1 &&
                        instr.operands[1] == func_id) {
                        result.emplace_back(static_cast<std::uint32_t>(fi),
                                            static_cast<std::uint32_t>(ii));
                    }
                }
            }
        }
        return result;
    }
};

} // namespace aura::ir
