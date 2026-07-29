// ir_reflect_serialize.cpp — P2996 reflection-based IR serialization
//
// aura-reflect isolation (Issue #2290): non-module TU, -freflection.
// Cannot `import aura.compiler.ir` here — GCC 16.1.0 still conflicts
// when mixing import std / Aura modules with <meta>. Mirror layouts
// MUST stay in sync with ir.ixx; validate via opcode_reflect.

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <array>
#include <cstring>
#include "reflect/reflect.hh"
#include "reflect/opcode_reflect.hh"

// Mirror structs — layouts MUST match aura.compiler.ir module.
namespace aura::ir {

enum class IROpcode : std::uint8_t {
    Nop,
    ConstI64,
    ConstF64,
    Local,
    Arg,
    Add,
    Sub,
    Mul,
    Div,
    Eq,
    Lt,
    Gt,
    Le,
    Ge,
    And,
    Or,
    Not,
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
    TryBegin,
    TryEnd,
    HashRef,
    HashSet,
    HashRemove,
    LinearWrap,
    MoveOp,
    BorrowOp,
    MutBorrowOp,
    DropOp,
    RefCountOp,
    ArenaPush,
    ArenaPop,
    GuardShape,
    TopCellLoad,
};

// Keep in sync with kIROpcodeCount in ir.ixx (54).
static_assert(aura::reflect::enum_count<IROpcode>() == 54);
static_assert(aura::reflect::validate_enum<IROpcode>());
static_assert(aura::reflect::opcode_name<IROpcode>(0) == "Nop");
static_assert(aura::reflect::opcode_name<IROpcode>(53) == "TopCellLoad");

enum class Region : std::uint8_t {
    Default = 0,
    Performance = 1,
    Evolution = 2,
};

struct IRInstruction {
    IROpcode opcode;
    std::array<std::uint32_t, 4> operands = {};
    std::uint32_t source_ast_node_id = 0;
    std::uint32_t type_id = 0;
    std::uint32_t shape_id = 0;
    std::uint8_t linear_ownership_state = 0;
    std::uint32_t adt_variant_id = 0;
    std::uint32_t narrow_evidence = 0;
    // Issue #455 / #1610: must stay in sync with aura.compiler.ir::IRInstruction.
    std::uint8_t source_marker = 0;
    std::uint32_t provenance = 0;
};

struct BasicBlock {
    std::uint32_t id = 0;
    std::vector<IRInstruction> instructions;
    std::vector<std::uint32_t> successors;
};

struct IRFunction {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t entry_block = 0;
    std::vector<BasicBlock> blocks;
    std::vector<std::string> params;
    std::vector<std::string> free_vars;
    std::uint32_t local_count = 0;
    std::uint32_t arg_count = 0;
    bool variadic = false;
    Region region = Region::Default;
    std::uint8_t marker = 0;
    std::uint32_t specialized_for = 0;
    std::uint32_t generic_id = 0xFFFFFFFF;
    std::vector<std::uint8_t> escape_map;
};

struct ClosureBridgeData {
    const void* flat = nullptr;
    const void* pool = nullptr;
    std::uint32_t body_id = ~0u;
    std::string body_source;
    std::uint64_t bridge_epoch = 0;
};

struct IRModule {
    std::vector<IRFunction> functions;
    std::vector<ClosureBridgeData> closure_bridge;
    std::uint32_t entry_function_id = 0;
    std::vector<std::string> string_pool;
};

} // namespace aura::ir

// ── C-linkage bridge ──────────────────────────────────────────

extern "C" {

void aura_ir_serialize(const void* mod, const char** out_data, size_t* out_size) {
    const auto& module = *static_cast<const aura::ir::IRModule*>(mod);
    aura::reflect::Buffer buf;
    aura::reflect::bin_write(buf, module);
    auto vec = buf.take();
    *out_size = vec.size();
    char* data = new char[*out_size];
    std::memcpy(data, vec.data(), *out_size);
    *out_data = data;
}

void aura_ir_deserialize(const char* data, size_t size, void* out_mod) {
    auto* module = static_cast<aura::ir::IRModule*>(out_mod);
    aura::reflect::BufferReader reader(data, size);
    *module = aura::reflect::bin_read<aura::ir::IRModule>(reader);
}

} // extern "C"


// ── --inspect: auto_to_json dump ────────────────────────────

extern "C" {

char* aura_inspect_ir_json(const void* mod, size_t* out_size) {
    const auto& module = *static_cast<const aura::ir::IRModule*>(mod);
    auto json = aura::reflect::to_json(module);
    *out_size = json.size();
    char* out = new char[*out_size + 1];
    std::memcpy(out, json.data(), *out_size);
    out[*out_size] = '\0';
    return out;
}

} // extern "C"