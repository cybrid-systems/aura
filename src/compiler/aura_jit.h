// aura_jit.h — LLVM ORC JIT backend for Aura IR
#ifndef AURA_JIT_H
#define AURA_JIT_H

#include <cstdint>
#include <cstddef>
#include <memory>
#include <functional>
#include <string>
#include <vector>

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
    const uint32_t* func_id_map; // [arg_count] maps local slots to IR func IDs
    uint32_t num_callees;        // number of entries in func_id_map
};

using ScalarFn = int64_t (*)(int64_t*, uint32_t);
// Alias for runtime registration with same signature
using ScalarFn32 = int64_t (*)(int64_t*, uint32_t);

// Runtime function pointer types for JIT symbol registration
using JitAllocClosureFn = int64_t (*)(int64_t func_id);
using JitClosureCaptureFn = void (*)(int64_t closure_id, int64_t idx, int64_t val);
using JitClosureCallFn = int64_t (*)(int64_t closure_id, int64_t* args, int64_t argc);
using JitNewCellFn = int64_t (*)();
using JitCellGetFn = int64_t (*)(int64_t cell_id);
using JitCellSetFn = void (*)(int64_t cell_id, int64_t val);

// Function metadata for registering compiled functions with runtime
struct FunctionMeta {
    std::string name;
    ScalarFn fn_ptr;
    uint32_t local_count;
    uint32_t arg_count;
    uint32_t env_count;
};

class AuraJIT {
public:
    AuraJIT();
    ~AuraJIT();

    bool available() const;
    ScalarFn compile(const FlatFunction& fn);
    void* get_function_ptr(const char* name);

    // Register a compiled function with the runtime for closure calls
    void register_function(int64_t func_id, ScalarFn fn_ptr, uint32_t local_count,
                           uint32_t arg_count, uint32_t env_count);

    // Get all compiled functions metadata
    const std::vector<FunctionMeta>& compiled_functions() const;

    // Register an external C symbol with the JIT (e.g., from dlopen)
    void register_symbol(const char* name, void* ptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Compile a FlatFunction to a native object file via LLVM IR + llc.
/// Returns true on success.
/// On success: out_obj_path contains the .o file.
bool emit_native_object(const FlatFunction& fn, const std::string& out_obj_path);

/// Emit an object file from an IR module.
/// Returns true on success.
bool emit_object(const std::string& ir_dump, const std::string& out_path);

/// Emit object file from an already-compiled IRModule.
bool emit_object_module(void* ir_module, const std::string& out_path);

} // namespace aura::jit

#endif
