// aura_jit.h — LLVM ORC JIT backend for Aura IR
#ifndef AURA_JIT_H
#define AURA_JIT_H

#include <cstdint>
#include <cstddef>
#include <memory>
#include <functional>

namespace aura::jit {

// Flat instruction format for JIT compilation (C-compatible)
struct FlatInstruction {
    uint32_t opcode;
    uint32_t ops[4];
};

struct FlatBlock {
    uint32_t id;
    const FlatInstruction* instructions;
    uint32_t num_instructions;
};

struct FlatFunction {
    const char* name;
    uint32_t entry_block;
    uint32_t local_count;
    uint32_t arg_count;
    const FlatBlock* blocks;
    uint32_t num_blocks;
    // Closure support: func_id mapping
    const uint32_t* func_id_map;  // [arg_count] maps local slots to IR func IDs
    uint32_t num_callees;         // number of entries in func_id_map
};

using ScalarFn = int64_t(*)(int64_t*, uint32_t);

// Runtime function pointer types for JIT symbol registration
using JitAllocClosureFn   = int64_t(*)(int64_t func_id);
using JitClosureCaptureFn = void(*)(int64_t closure_id, int32_t idx, int64_t val);
using JitClosureCallFn    = int64_t(*)(int64_t closure_id, int64_t* args, int32_t argc);
using JitNewCellFn        = int64_t(*)();
using JitCellGetFn        = int64_t(*)(int64_t cell_id);
using JitCellSetFn        = void(*)(int64_t cell_id, int64_t val);

class AuraJIT {
public:
    AuraJIT();
    ~AuraJIT();

    bool available() const;
    ScalarFn compile(const FlatFunction& fn);
    void* get_function_ptr(const char* name);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aura::jit

#endif
