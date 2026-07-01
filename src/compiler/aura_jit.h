// aura_jit.h — LLVM ORC JIT backend for Aura IR
#ifndef AURA_JIT_H
#define AURA_JIT_H

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstdio>
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
    // Issue #538: occurrence-narrowing evidence from IR metadata.
    // Non-zero enables GuardShape / CastOp zero-overhead fast paths.
    uint32_t narrow_evidence = 0;
    // Issue #538: static TypeId for JIT specialization when set.
    uint32_t type_id = 0;
};

// Issue #60 Iter 3: shape encoding constants. Must match the
// shape_map byte values in set_shape_map (service.ixx). 0=Dynamic.
constexpr uint32_t SHAPE_INT = 1;
constexpr uint32_t SHAPE_FLOAT = 2;
constexpr uint32_t SHAPE_BOOL = 3;
constexpr uint32_t SHAPE_STRING = 4;
constexpr uint32_t SHAPE_VOID = 5;
constexpr uint32_t SHAPE_PAIR = 10;
constexpr uint32_t SHAPE_VECTOR = 11;
constexpr uint32_t SHAPE_HASH_ = 12;
constexpr uint32_t SHAPE_CLOSURE = 13;
constexpr uint32_t SHAPE_REF = 14;

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

    // Issue #170 Phase 1: AOT (ahead-of-time) compilation
    // entry points. These let external tools (benchmark
    // harnesses, AOT experiments, static analysis passes)
    // reuse the same LLVM pipeline that the JIT uses, but
    // emit a standalone artifact instead of in-memory code.
    //
    // compile_to_llvm_ir: returns the textual LLVM IR
    //   (for the most recently compiled module) as a
    //   std::string. Useful for static analysis (e.g.,
    //   `lli`, `opt -O2` on the IR to get a sense of
    //   achievable perf without going through the full
    //   LLVM codegen).
    //
    // compile_to_object_file: writes the most recently
    //   compiled module to disk as a native .o file. The
    //   file can be linked with any C linker to produce a
    //   standalone executable. Useful for AOT experiments
    //   (compile once, run many times).
    //
    // Both return false / empty on failure (no module
    // compiled yet, or the LLVM pipeline failed). On success,
    // the module is still resident in the JIT (no side
    // effect on subsequent compile() calls).
    std::string compile_to_llvm_ir();
    bool compile_to_object_file(const std::string& path);

    // Register a compiled function with the runtime for closure calls
    void register_function(int64_t func_id, ScalarFn fn_ptr, uint32_t local_count,
                           uint32_t arg_count, uint32_t env_count);

    // Get all compiled functions metadata
    const std::vector<FunctionMeta>& compiled_functions() const;

    // Register an external C symbol with the JIT (e.g., from dlopen)
    void register_symbol(const char* name, void* ptr);

    // Issue #193: per-function unhandled-opcode count. Returns
    // the number of unhandled opcodes the JIT's lower() has
    // recorded for the most recent compile of `name`. Returns
    // 0 if the function was never compiled or has never hit
    // an unhandled opcode.
    std::uint64_t unhandled_opcode_count_for_function(const char* name) const;

    // Set string pool for OpConstString support. Must be called before compile().
    // The pool pointer must remain valid for the lifetime of all compilations.
    void set_string_pool(const std::vector<std::string>* pool);

    // ── JIT metrics (Issue #114) ────────────────
    // Counters exposed for observability. All atomic; safe to
    // read from any thread at any time. Cheap (relaxed load).
    struct Metrics {
        std::atomic<std::uint64_t> compile_count{0};
        std::atomic<std::uint64_t> compile_total_us{0};
        std::atomic<std::uint64_t> hot_swap_count{0};
        std::atomic<std::uint64_t> verify_fail_count{0};
        std::atomic<std::uint64_t> add_module_fail_count{0};
        std::atomic<std::uint64_t> inlined_prim_count{0}; // fast-path
        std::atomic<std::uint64_t> slow_prim_count{0};    // aura_prim_call path
        std::atomic<std::uint64_t> cached_function_count{0};
        // Issue #170 Phase 1 / item #1: counter for IROpcodes that
        // the JIT lowering doesn't have a case for. The default
        // branch in lower() increments this and writes a sentinel
        // (0) to the result slot — this is the "visible default"
        // that replaces the previously-silent write-0 behavior.
        // spec_jit_controller (Phase 2 / item #1) will consume this
        // to auto-deopt hot functions that hit unhandled opcodes.
        std::atomic<std::uint64_t> unhandled_opcode_count{0};
        // Issue #170 Phase 2 / item #3: counter for runtime helper
        // calls that the JIT inlined as intrinsics. Tracks the
        // payoff of the runtime→intrinsics migration. Starts at
        // 0; increments each time a lowering replaces a runtime
        // call with an inline sequence (LLVM IR or always_inline
        // function). Provides a single number for observability +
        // perf regression detection.
        std::atomic<std::uint64_t> intrinsic_count{0};
        // Issue #461: explicit IRInterpreter fallback counter.
        // Bumped by the runtime stub `aura_jit_fallback_to_interpreter`
        // each time the JIT lower() default case routes through
        // it instead of writing a sentinel. The P0 ship doesn't
        // yet emit an LLVM call to the stub (that requires
        // module-level function declaration; out of scope). The
        // metric is observable now and ready to be wired into
        // the lower() default case in the follow-up.
        std::atomic<std::uint64_t> fallback_count{0};
        // Issue #461: consistency violations — set by the
        // post-mutation consistency harness (follow-up that
        // runs JIT and IRInterpreter side-by-side and compares
        // outputs). Starts at 0; the harness bumps it when
        // results differ.
        std::atomic<std::uint64_t> consistency_violations{0};

        // Format as a single-line string for telemetry / log output.
        // Caller-provided buffer; returns the same pointer.
        char* format(char* buf, std::size_t buf_size) const noexcept;
    };
    const Metrics& metrics() const noexcept { return metrics_; }
    // Non-const accessor for tests + mutation. Production code
    // should prefer the const overload + the free-form update
    // methods (compile_count.fetch_add(1, ...), etc.).
    Metrics& mutable_metrics() noexcept { return metrics_; }

    // Hot-swap: replace an already-compiled function with a new version.
    // Removes the old module from the JIT dylib and compiles + links the new one.

    // Invalidate the per-function compile cache entry (and the
    // ResourceTracker module) for `name`. Call this from a
    // redefine path so the next compile() actually re-runs the
    // LLVM pipeline instead of returning a stale fn_ptr.
    void invalidate(const char* name);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    mutable Metrics metrics_;
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
void run_escape_analysis(const std::vector<std::vector<FlatInstruction>>& flat_instrs,
                         uint32_t local_count, std::vector<uint8_t>& escape_map);

} // namespace aura::jit

#endif
