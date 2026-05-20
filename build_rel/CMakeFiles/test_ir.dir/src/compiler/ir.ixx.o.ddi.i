# 0 "/home/dev/code/aura/src/compiler/ir.ixx"
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
    ConstF64,
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

    Primitive,

    ConstBool,

    ConstVoid,

    MakePair,
    Car,
    Cdr,

    Raise,
    IsError,
};

export struct IRInstruction {
    IROpcode opcode;
    std::array<std::uint32_t, 4> operands = {};
    std::uint32_t source_ast_node_id = 0;
    std::uint32_t type_id = 0;
};




export struct OpcodeInfo {
    std::string_view name;
    std::uint8_t operand_count;
    bool has_result_slot;
};

export constexpr OpcodeInfo kOpcodeInfo[] = {

    {"nop", 0, false},

    {"const-i64", 1, true},
    {"const-f64", 1, true},
    {"local", 2, true},
    {"arg", 2, true},

    {"add", 3, true},
    {"sub", 3, true},
    {"mul", 3, true},
    {"div", 3, true},

    {"eq", 3, true},
    {"lt", 3, true},
    {"gt", 3, true},
    {"le", 3, true},
    {"ge", 3, true},

    {"and", 3, true},
    {"or", 3, true},
    {"not", 2, true},

    {"branch", 3, false},
    {"jump", 1, false},

    {"call", 4, false},
    {"return", 1, false},

    {"make-closure", 3, true},
    {"capture", 3, false},
    {"capture-ref", 3, false},
    {"apply", 4, false},

    {"new-cell", 1, true},
    {"cell-set", 2, false},
    {"cell-get", 2, true},

    {"cast", 3, true},

    {"const-string", 2, true},

    {"prim-call", 3, true},
    {"primitive", 2, true},

    {"const-bool", 2, true},
    {"const-void", 1, true},

    {"make-pair", 3, true},
    {"car", 2, true},
    {"cdr", 2, true},

    {"raise", 2, true},
    {"is-error", 2, true},
};

static_assert(std::size(kOpcodeInfo) == 39,
    "kOpcodeInfo must have exactly one entry per IROpcode");


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

    Apply,

    Vector,
    VectorRef,
    VectorSet,
    VectorLength,
    VectorP,
    MakeVector,

    Import,

    CharEq,
    CharLt,
    CharToInteger,
    IntegerToChar,

    Raise,
    ErrorP,
};



export constexpr std::string_view kPrimNames[] = {
    "string-append", "string-length", "string-ref",
    "substring", "string=?", "string<?",
    "number->string", "string->number",
    "display", "write", "newline",
    "error", "assert",
    "read", "read-file", "write-file", "file-exists?",
    "gensym",
    "apply",
    "vector", "vector-ref", "vector-set!",
    "vector-length", "vector?", "make-vector",
    "import",
    "char=?", "char<?", "char->integer", "integer->char",
    "raise", "error?",
};

static_assert(std::size(kPrimNames) == 32,
    "kPrimNames must have exactly one entry per PrimId");


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
    bool variadic = false;
};




export struct ClosureBridgeData {
    const ast::FlatAST* flat = nullptr;
    const ast::StringPool* pool = nullptr;
    ast::NodeId body_id = ast::NULL_NODE;
    std::string body_source;
};

export struct IRModule {
    std::vector<IRFunction> functions;
    std::vector<ClosureBridgeData> closure_bridge;
    std::uint32_t entry_function_id = 0;

    std::uint32_t add_function(IRFunction func) {
        func.id = static_cast<std::uint32_t>(functions.size());
        functions.push_back(std::move(func));

        if (closure_bridge.size() < functions.size())
            closure_bridge.resize(functions.size());
        return func.id;
    }


    void set_closure_bridge(std::uint32_t func_id,
                             const ast::FlatAST* flat,
                             const ast::StringPool* pool,
                             ast::NodeId body_id) {
        if (func_id < functions.size()) {
            closure_bridge[func_id] = {flat, pool, body_id, ""};
        }
    }


    void set_closure_bridge_ptr(std::uint32_t func_id,
                                 const ast::FlatAST* flat,
                                 const ast::StringPool* pool,
                                 ast::NodeId body_id) {
        if (func_id < closure_bridge.size()) {
            closure_bridge[func_id] = {flat, pool, body_id, ""};
        }
    }

    void set_closure_body_source(std::uint32_t func_id, const std::string& src) {
        if (func_id < closure_bridge.size()) {
            closure_bridge[func_id].body_source = src;
        }
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
