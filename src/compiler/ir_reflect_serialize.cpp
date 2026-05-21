// ir_reflect_serialize.cpp — P2996 reflection-based IR serialization
//
// Compiled with -freflection (GCC 16+). Mirrors the IR struct layouts from
// aura.compiler.ir module for compile-time reflection access.
//
// Added to aura-reflect library (not the module build).
// Exposes C-linkage functions for use from cache_impl.cpp.
// Replaces the hand-written IR serialization.

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <array>
#include <cstring>
#include "reflect/reflect.hh"

// Mirror structs — layouts MUST match aura.compiler.ir module.
namespace aura::ir {

enum class IROpcode : std::uint8_t {
    Nop, ConstI64, ConstF64, Local, Arg,
    Add, Sub, Mul, Div,
    Eq, Lt, Gt, Le, Ge,
    And, Or, Not,
    Branch, Jump, Call, Return,
    MakeClosure, Capture, CaptureRef, Apply,
    NewCell, CellSet, CellGet,
    CastOp, ConstString,
    PrimCall, Primitive,
    ConstBool, ConstVoid,
    MakePair, Car, Cdr,
    Raise, IsError,
};

struct IRInstruction {
    IROpcode opcode;
    std::array<std::uint32_t, 4> operands = {};
    std::uint32_t source_ast_node_id = 0;
    std::uint32_t type_id = 0;
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
};

struct ClosureBridgeData {
    const void* flat = nullptr;
    const void* pool = nullptr;
    std::uint32_t body_id = ~0u;
    std::string body_source;
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
    char* data = new char[*out_size + 1];
    std::memcpy(data, json.data(), *out_size);
    data[*out_size] = '\0';
    return data;
}

} // extern "C"
