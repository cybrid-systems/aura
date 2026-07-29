// ir_reflect_serialize.cpp — P2996 IR serialize / inspect (aura-reflect)
//
// Issue #2290: non-module + -freflection (cannot import aura.compiler.ir).
// Issue #2291 Phase 4: pure-POD types (IRInstruction, IRFunctionHeader,
// OpcodeArity) use only auto_serialize / auto_validate / to_json.
// Full IRModule still uses recursive bin_write (containers + nested).

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <array>
#include <cstring>

#include "reflect/reflect.hh"
#include "reflect/opcode_reflect.hh"
#include "reflect/ir_pod_reflect.hh"

namespace aura::ir {

using IROpcode = aura::ir_pod::IROpcode;
using Region = aura::ir_pod::Region;
using IRInstruction = aura::ir_pod::IRInstruction;

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

// ── Phase 4 POD C ABI (auto_serialize only) ───────────────────

extern "C" {

void aura_ir_instr_serialize(const void* instr, const char** out_data, size_t* out_size) {
    const auto& i = *static_cast<const aura::ir_pod::IRInstruction*>(instr);
    auto vec = aura::ir_pod::pod_serialize(i);
    *out_size = vec.size();
    char* data = new char[*out_size];
    std::memcpy(data, vec.data(), *out_size);
    *out_data = data;
}

int aura_ir_instr_deserialize(const char* data, size_t size, void* out_instr) {
    std::vector<char> bytes(data, data + size);
    *static_cast<aura::ir_pod::IRInstruction*>(out_instr) =
        aura::ir_pod::pod_deserialize<aura::ir_pod::IRInstruction>(bytes);
    return 1;
}

int aura_ir_instr_validate(const void* instr) {
    const auto& i = *static_cast<const aura::ir_pod::IRInstruction*>(instr);
    return aura::ir_pod::validate_instruction(i) ? 0 : -1;
}

void aura_ir_fn_header_serialize(const void* hdr, const char** out_data, size_t* out_size) {
    const auto& h = *static_cast<const aura::ir_pod::IRFunctionHeader*>(hdr);
    auto vec = aura::ir_pod::pod_serialize(h);
    *out_size = vec.size();
    char* data = new char[*out_size];
    std::memcpy(data, vec.data(), *out_size);
    *out_data = data;
}

int aura_ir_fn_header_deserialize(const char* data, size_t size, void* out_hdr) {
    std::vector<char> bytes(data, data + size);
    *static_cast<aura::ir_pod::IRFunctionHeader*>(out_hdr) =
        aura::ir_pod::pod_deserialize<aura::ir_pod::IRFunctionHeader>(bytes);
    return 1;
}

int aura_ir_fn_header_validate(const void* hdr) {
    const auto& h = *static_cast<const aura::ir_pod::IRFunctionHeader*>(hdr);
    return aura::ir_pod::validate_function_header(h) ? 0 : -1;
}

void aura_opcode_arity_serialize(const void* ar, const char** out_data, size_t* out_size) {
    const auto& a = *static_cast<const aura::ir_pod::OpcodeArity*>(ar);
    auto vec = aura::ir_pod::pod_serialize(a);
    *out_size = vec.size();
    char* data = new char[*out_size];
    std::memcpy(data, vec.data(), *out_size);
    *out_data = data;
}

int aura_opcode_arity_deserialize(const char* data, size_t size, void* out_ar) {
    std::vector<char> bytes(data, data + size);
    *static_cast<aura::ir_pod::OpcodeArity*>(out_ar) =
        aura::ir_pod::pod_deserialize<aura::ir_pod::OpcodeArity>(bytes);
    return 1;
}

int aura_opcode_arity_validate(const void* ar) {
    const auto& a = *static_cast<const aura::ir_pod::OpcodeArity*>(ar);
    return aura::ir_pod::validate_opcode_arity(a) ? 0 : -1;
}

} // extern "C"

// ── Full IRModule C-linkage bridge (bin_write path) ───────────

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
