# 0 "/home/dev/code/aura/src/compiler/ir.ixx"
# 1 "/home/dev/code/aura/build_debug//"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/compiler/ir.ixx"
export module aura.compiler.ir;
import std;
import aura.core;

namespace aura::ir {

export enum class IROpcode : std::uint8_t {
    Nop,

    ConstI64,
    Local,
    Arg,

    Add, Sub, Mul, Div,

    Eq, Lt, Gt, Le, Ge,

    And, Or, Not,

    Branch,
    Jump,
    Call,
    Return,

    MakeClosure,
    Capture,
    CaptureRef,
    Apply,

    NewCell,
    CellSet,
    CellGet,

    CastOp,

    ConstString,

    PrimCall,
};

export struct IRInstruction {
    IROpcode opcode;
    std::array<std::uint32_t, 4> operands = {};
    std::uint32_t source_ast_node_id = 0;
};


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

    Vector,
    VectorRef,
    VectorSet,
    VectorLength,
    VectorP,
    MakeVector,
};


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
    std::uint32_t local_count = 0;
    std::uint32_t arg_count = 0;
};

export struct IRModule {
    std::vector<IRFunction> functions;
    std::uint32_t entry_function_id = 0;

    std::uint32_t add_function(IRFunction func) {
        func.id = static_cast<std::uint32_t>(functions.size());
        functions.push_back(std::move(func));
        return func.id;
    }

    std::vector<std::string> string_pool;

    std::uint32_t add_string(std::string s) {
        auto id = static_cast<std::uint32_t>(string_pool.size());
        string_pool.push_back(std::move(s));
        return id;
    }

    void set_entry(std::uint32_t id) { entry_function_id = id; }
    IRFunction& entry() { return functions[entry_function_id]; }
    const IRFunction& entry() const { return functions[entry_function_id]; }




    bool hot_swap_function(std::uint32_t func_id, IRFunction new_func) {
        if (func_id >= functions.size()) return false;
        new_func.id = func_id;
        functions[func_id] = std::move(new_func);
        return true;
    }


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

}
