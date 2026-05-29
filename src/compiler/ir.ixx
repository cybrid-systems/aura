export module aura.compiler.ir;
import std;
import aura.core;

namespace aura::ir {

export enum class IROpcode : std::uint8_t {
    Nop,
    // Data
    ConstI64, // load int64 constant
    ConstF64, // load double constant
    Local,    // load local variable (slot index)
    Arg,      // load function argument (slot index)
    // Arithmetic
    Add,
    Sub,
    Mul,
    Div,
    // Comparison (return 0 or 1)
    Eq,
    Lt,
    Gt,
    Le,
    Ge,
    // Logic
    And,
    Or,
    Not,
    // Control flow
    Branch, // conditional branch: cond, true_target, false_target
    Jump,   // unconditional branch: target
    Call,   // function call: callee_expr, arg_count, result_slot
    Return, // return: value_slot
    // Closures
    MakeClosure, // create closure: func_slot, env_size
    Capture,     // capture variable: closure_slot, env_idx, var_slot
    CaptureRef,  // capture by reference: closure_slot, env_idx, cell_slot
    Apply,       // apply closure: closure_slot, arg_count, result_slot
    // Mutable cells (for letrec)
    NewCell, // allocate mutable cell: result_slot
    CellSet, // write to cell: cell_id, value_slot
    CellGet, // read from cell: result_slot, cell_id
    // Type coercion (L6.6b)
    CastOp, // runtime type check: result_slot, value_slot, type_tag
    // String support
    ConstString, // load string constant: result_slot, string_index
    // Primitive call (for non-arithmetic primitives like string ops)
    PrimCall, // call prim by id: prim_id, packed_args(arg_begin, arg_count), result_slot
    // Primitive value (load a primitive function value)
    Primitive, // load primitive value: result_slot, prim_slot_index
    // Boolean constant
    ConstBool, // load boolean constant: result_slot, value(0 or 1)
    // Void constant (empty list)
    ConstVoid, // load void: result_slot
    // Pair operations (native, avoids PrimCall dispatch)
    MakePair, // create pair: result_slot, car_slot, cdr_slot
    Car,      // car: result_slot, pair_slot
    Cdr,      // cdr: result_slot, pair_slot
    // Error handling
    Raise,   // raise error: result_slot, cause_slot
    IsError, // check if error: result_slot, value_slot
    // M4 Linear ownership
    LinearWrap,  // wrap linear value: result_slot, inner_slot
    MoveOp,      // move ownership: result_slot, inner_slot
    BorrowOp,    // immutable borrow: result_slot, inner_slot
    MutBorrowOp, // mutable borrow: result_slot, inner_slot
    DropOp,      // explicit destruct: inner_slot
    RefCountOp,  // runtime refcount: result_slot, inner_slot, inc(1)/dec(0)
};

export struct IRInstruction {
    IROpcode opcode;
    std::array<std::uint32_t, 4> operands = {};
    std::uint32_t source_ast_node_id = 0;
    std::uint32_t type_id = 0; // 0 = unknown/dynamic, from FlatAST type_id
};

// ── Opcode metadata table ─────────────────────────────────────
// Describes properties of each IROpcode. Indexed by IROpcode enum value.
// Order MUST match the IROpcode enum exactly.
export struct OpcodeInfo {
    std::string_view name;
    std::uint8_t operand_count; // 0-4, how many operands are meaningful
    bool has_result_slot;       // true if operands[0] is the result slot
};

export constexpr OpcodeInfo kOpcodeInfo[] = {
    // 0  Nop
    {"nop", 0, false},
    // 1-4  Data
    {"const-i64", 1, true}, // ConstI64
    {"const-f64", 1, true}, // ConstF64
    {"local", 2, true},     // Local: result, src
    {"arg", 2, true},       // Arg: result, arg_slot
    // 5-8  Arithmetic
    {"add", 3, true}, // Add: result, a, b
    {"sub", 3, true},
    {"mul", 3, true},
    {"div", 3, true},
    // 9-13  Comparison
    {"eq", 3, true}, // Eq: result, a, b
    {"lt", 3, true},
    {"gt", 3, true},
    {"le", 3, true},
    {"ge", 3, true},
    // 14-16  Logic
    {"and", 3, true}, // And: result, a, b
    {"or", 3, true},
    {"not", 2, true}, // Not: result, a
    // 17-18  Control flow
    {"branch", 3, false}, // Branch: cond, true_block, false_block
    {"jump", 1, false},   // Jump: target_block
    // 19-20
    {"call", 4, false},   // Call: callee, arg_base, arg_count, result
    {"return", 1, false}, // Return: value
    // 21-24  Closures
    {"make-closure", 3, true}, // MakeClosure: result, func_id, env_size
    {"capture", 3, false},     // Capture: closure, env_idx, var
    {"capture-ref", 3, false}, // CaptureRef: closure, env_idx, cell
    {"apply", 4, false},       // Apply: closure, arg_base, arg_count, result
    // 25-27  Mutable cells
    {"new-cell", 1, true},  // NewCell: result
    {"cell-set", 2, false}, // CellSet: cell, value
    {"cell-get", 2, true},  // CellGet: result, cell
    // 28  Type coercion
    {"cast", 3, true}, // CastOp: result, value, type_tag
    // 29  String
    {"const-string", 2, true}, // ConstString: result, string_index
    // 30-31  Primitive
    {"prim-call", 3, true}, // PrimCall: prim_id, packed_args, result
    {"primitive", 2, true}, // Primitive: result, slot_index
    // 32-33  Constants
    {"const-bool", 2, true}, // ConstBool: result, value
    {"const-void", 1, true}, // ConstVoid: result
    // 34-36  Pair
    {"make-pair", 3, true}, // MakePair: result, car, cdr
    {"car", 2, true},       // Car: result, pair
    {"cdr", 2, true},       // Cdr: result, pair
    // 37-38  Error handling
    {"raise", 2, true},    // Raise: result, cause
    {"is-error", 2, true}, // IsError: result, value
    // 39-44  M4 Linear ownership
    {"linear-wrap", 2, true},   // LinearWrap: result, inner
    {"move-op", 2, true},       // MoveOp: result, inner
    {"borrow-op", 2, true},     // BorrowOp: result, inner
    {"mut-borrow-op", 2, true}, // MutBorrowOp: result, inner
    {"drop-op", 1, false},      // DropOp: inner (no result)
    {"ref-count-op", 3, true},  // RefCountOp: result, inner, inc/dec
};

static_assert(std::size(kOpcodeInfo) == 45, "kOpcodeInfo must have exactly one entry per IROpcode");

// Helper: look up opcode info by IROpcode enum value
inline const OpcodeInfo* lookup_opcode(IROpcode op) {
    auto idx = static_cast<std::size_t>(op);
    return idx < std::size(kOpcodeInfo) ? &kOpcodeInfo[idx] : nullptr;
}

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
    // Apply: (apply fn list) — call fn with list elements as args
    Apply,
    // Vector primitives
    Vector,
    VectorRef,
    VectorSet,
    VectorLength,
    VectorP,
    MakeVector,
    // Module primitives
    Import,
    // Character operations (chars are integers in Aura)
    CharEq,
    CharLt,
    CharToInteger,
    IntegerToChar,
    // Arithmetic
    Quotient,
    Remainder,
    // List primitives
    ListLength,
    ListRef,
    ListReverse,
    // Error handling
    Raise,
    ErrorP,
    // Type predicates
    PairP,
    NullP,
};

// Names for each PrimId, indexed by enum value.
// Must match PrimId enum order exactly.
export constexpr std::string_view kPrimNames[] = {
    "string-append", "string-length",  "string-ref",     "substring",     "string=?",
    "string<?",      "number->string", "string->number", "display",       "write",
    "newline",       "error",          "assert",         "read",          "read-file",
    "write-file",    "file-exists?",   "gensym",         "apply",         "vector",
    "vector-ref",    "vector-set!",    "vector-length",  "vector?",       "make-vector",
    "import",        "char=?",         "char<?",         "char->integer", "integer->char",
    "quotient",
    "remainder",
    "length",
    "list-ref",
    "reverse",
    "raise",         "error?",
    "pair?",         "null?",
};

static_assert(std::size(kPrimNames) == 39, "kPrimNames must have exactly one entry per PrimId");

// Helper: pack two uint32 into one (for Call: args_begin << 16 | arg_count)
export constexpr std::uint32_t pack_pair(std::uint32_t hi, std::uint32_t lo) {
    return (hi << 16) | (lo & 0xFFFF);
}
export constexpr std::uint32_t unpack_hi(std::uint32_t p) {
    return p >> 16;
}
export constexpr std::uint32_t unpack_lo(std::uint32_t p) {
    return p & 0xFFFF;
}

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
    std::uint32_t local_count = 0; // number of local slots needed
    std::uint32_t arg_count = 0;   // number of arguments
    bool variadic = false;         // dotted rest param
};

// Closure bridge data: original tree-walker info for IR closures.
// Only populated when the lowered closure has corresponding FlatAST data
// (i.e. for lambda forms, not for cached defines).
export struct ClosureBridgeData {
    const ast::FlatAST* flat = nullptr;
    const ast::StringPool* pool = nullptr;
    ast::NodeId body_id = ast::NULL_NODE;
    std::string body_source; // serialized source for bridge fallback re-parse
};

export struct IRModule {
    std::vector<IRFunction> functions;
    std::vector<ClosureBridgeData> closure_bridge; // indexed by func_id
    std::uint32_t entry_function_id = 0;

    std::uint32_t add_function(IRFunction func) {
        func.id = static_cast<std::uint32_t>(functions.size());
        functions.push_back(std::move(func));
        // Ensure closure_bridge is in sync
        if (closure_bridge.size() < functions.size())
            closure_bridge.resize(functions.size());
        return func.id;
    }

    // Set bridge data for a function (for cross-evaluator lambda calls)
    void set_closure_bridge(std::uint32_t func_id, const ast::FlatAST* flat,
                            const ast::StringPool* pool, ast::NodeId body_id) {
        if (func_id < functions.size()) {
            closure_bridge[func_id] = {flat, pool, body_id, ""};
        }
    }

    // Set bridge data by pointer (for cached function injection)
    void set_closure_bridge_ptr(std::uint32_t func_id, const ast::FlatAST* flat,
                                const ast::StringPool* pool, ast::NodeId body_id) {
        if (func_id < closure_bridge.size()) {
            closure_bridge[func_id] = {flat, pool, body_id, ""};
        }
    }

    void set_closure_body_source(std::uint32_t func_id, const std::string& src) {
        if (func_id < closure_bridge.size()) {
            closure_bridge[func_id].body_source = src;
        }
    }

    std::vector<std::string> string_pool; // string constants (for ConstString opcode)

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
        if (func_id >= functions.size())
            return false;
        new_func.id = func_id;
        functions[func_id] = std::move(new_func);
        return true;
    }

    // ── 类型驱动 IR 优化 ───────────────────────────────────
    // 基于 type_id 信息消除冗余 CastOp。当值类型已知且匹配目标
    // 类型时，CastOp 是多余的，直接替换引用为原始值。
    // 同时传播类型信息，减少运行时类型检查。
    void optimize_type_info() {
        for (auto& func : functions) {
            for (auto& block : func.blocks) {
                // First pass: collect known type for each slot
                struct SlotType {
                    std::uint32_t type_id = 0;
                    std::uint32_t source_slot = 0; // 0 = self
                    bool is_cast = false;
                };
                std::vector<SlotType> slot_types(func.local_count + func.arg_count + 4);
                std::vector<std::uint32_t> slot_remap; // result_slot -> source_slot
                slot_remap.resize(func.local_count + func.arg_count + 4,
                                  std::numeric_limits<std::uint32_t>::max());

                for (auto& instr : block.instructions) {
                    // Record known type for this instruction's result
                    if (auto* info = lookup_opcode(instr.opcode)) {
                        if (info->has_result_slot && instr.operands[0] < slot_types.size()) {
                            auto& st = slot_types[instr.operands[0]];
                            if (instr.type_id != 0) {
                                st.type_id = instr.type_id;
                                st.source_slot = instr.operands[0];
                            }
                        }
                    }

                    // Eliminate redundant CastOp
                    if (instr.opcode == IROpcode::CastOp) {
                        auto val_slot = instr.operands[1];
                        auto target_tag = instr.operands[2];
                        auto result_slot = instr.operands[0];

                        // Check if value already has the target type
                        bool redundant = false;
                        if (val_slot < slot_types.size() && slot_types[val_slot].type_id != 0) {
                            if (slot_types[val_slot].type_id == target_tag) {
                                redundant = true;
                            }
                        }

                        if (redundant) {
                            // Mark for replacement: result_slot -> val_slot
                            if (result_slot < slot_remap.size())
                                slot_remap[result_slot] = val_slot;
                            // Also propagate type
                            if (result_slot < slot_types.size() && val_slot < slot_types.size())
                                slot_types[result_slot] = slot_types[val_slot];
                        }
                    }
                }

                // Apply remapping: replace all uses of redundant cast results
                // with the original source slot
                for (auto& instr : block.instructions) {
                    if (instr.opcode == IROpcode::CastOp)
                        continue; // skip CastOps we already marked
                    // Don't skip op==0 — a valid slot can be 0
                    for (auto& op : instr.operands) {
                        if (op < slot_remap.size() && slot_remap[op] != std::numeric_limits<std::uint32_t>::max() && slot_remap[op] != op) {
                            // Follow the remap chain
                            auto src = slot_remap[op];
                            while (src < slot_remap.size() && slot_remap[src] != 0 && slot_remap[src] != src)
                                src = slot_remap[src];
                            op = src;
                        }
                    }
                }

                // Second pass: remove redundant CastOps (no operands reference them)
                std::erase_if(block.instructions, [&](const IRInstruction& instr) {
                    if (instr.opcode == IROpcode::CastOp) {
                        auto result_slot = instr.operands[0];
                        return result_slot < slot_remap.size() &&
                               slot_remap[result_slot] != std::numeric_limits<std::uint32_t>::max();
                    }
                    return false;
                });
            }
        }
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
                    if (instr.opcode == IROpcode::MakeClosure && instr.operands.size() > 1 &&
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
