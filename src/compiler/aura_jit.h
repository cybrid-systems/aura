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
    // Issue #60 Iter 2: shape_id from the per-function shape_map.
    // 0 = unknown / Dynamic. The JIT uses this for L1 fast paths
    // (OpAdd etc.) and for L2 layout specialization.
    uint32_t shape_id;
};

// Issue #60 Iter 3: shape encoding constants. Must match the
// shape_map byte values in set_shape_map (service.ixx). 0=Dynamic.
constexpr uint32_t SHAPE_INT  = 1;
constexpr uint32_t SHAPE_PAIR = 10;

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
    // Type tags for const values: [local_count], 0=Int, 1=Bool, 5=Void, 255=Dynamic
    const uint8_t* const_tags;
    // Shape map for speculative JIT: [local_count], 0=Dynamic, 1=Int, 2=Float,
    // 3=Bool, 4=String, 5=Void, 10=Pair, 11=Vector, 12=Hash.
    // When non-null, the JIT will skip tag checks for known shapes.
    // A shape guard is generated at function entry to verify runtime shapes match.
    const uint8_t* shape_map;
    // Escape analysis map: [local_count], 0=NON_ESCAPING, 1=ESCAPED.
    // When non-null, MakePair ops with ESCAPED result slots use heap allocation;
    // NON_ESCAPING result slots use arena (bump) allocation.
    const uint8_t* escape_map;
    uint8_t region; // 0=Default, 1=Performance, 2=Evolution
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

    // Set string pool for OpConstString support. Must be called before compile().
    // The pool pointer must remain valid for the lifetime of all compilations.
    void set_string_pool(const std::vector<std::string>* pool);

    // Hot-swap: replace an already-compiled function with a new version.
    // Removes the old module from the JIT dylib and compiles + links the new one.
    private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Compile a FlatFunction to a native object file via LLVM IR + llc.
/// string_pool provides content for OpConstString instructions.
/// Returns true on success. On success: out_obj_path contains the .o file.
bool emit_native_object(const FlatFunction& fn, const std::string& out_obj_path,
                        const std::vector<std::string>* string_pool = nullptr);

/// Emit an object file from an IR module.
/// Returns true on success.
bool emit_object(const std::string& ir_dump, const std::string& out_path);

/// Emit object file from an already-compiled IRModule.
bool emit_object_module(void* ir_module, const std::string& out_path);

/// Run backward escape analysis on flat IR instructions.
/// Fills escape_map (size = local_count). 0 = NON_ESCAPING, 1 = ESCAPED.
void run_escape_analysis(
    const std::vector<std::vector<FlatInstruction>>& flat_instrs,
    uint32_t local_count,
    std::vector<uint8_t>& escape_map);

} // namespace aura::jit

#endif
