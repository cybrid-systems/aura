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
    // Issue #684: linear ownership from IRFunctionSoA metadata column.
    uint8_t linear_ownership_state = 0;
    // Issue #684: instruction_dirty_ bit from SoA — non-zero triggers deopt.
    uint8_t dirty = 0;
    // Issue #1610 / #1616: SyntaxMarker from IRInstruction::source_marker
    // (0=User, 1=MacroIntroduced, 2=BoolLiteral). JIT lower() applies
    // conservative hygiene policy (no L2 shape specialization; deopt
    // when combined with dirty).
    uint8_t source_marker = 0;
    // Issue #1616: AST provenance id mirrored from IRInstruction.
    uint32_t provenance = 0;
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
constexpr uint32_t SHAPE_HASH = 12;
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

    // Issue #1516: per-function AOT — emit IR / native object for a
    // previously compiled function (keyed by IR func_id registered
    // via register_function, or by function name). Unlike
    // compile_to_* which only sees the most recent module, these
    // retain one module snapshot per function so multi-function
    // EDA/hardware static-link builds can AOT selectively.
    // Returns empty / false when the function was never compiled
    // or the AOT snapshot was invalidated.
    std::string compile_function_to_llvm_ir(std::uint32_t func_id);
    bool compile_function_to_object(std::uint32_t func_id, const std::string& path);
    std::string compile_function_to_llvm_ir_by_name(const char* name);
    bool compile_function_to_object_by_name(const char* name, const std::string& path);

    // Register a compiled function with the runtime for closure calls
    void register_function(int64_t func_id, ScalarFn fn_ptr, uint32_t local_count,
                           uint32_t arg_count, uint32_t env_count, const char* name = nullptr);

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
        // Issue #461 / #1512: consistency violations — set by the
        // post-mutation consistency harness (JIT vs IRInterpreter
        // side-by-side compare) or by record_consistency_result().
        // Starts at 0; the harness bumps it when results differ.
        std::atomic<std::uint64_t> consistency_violations{0};
        // Issue #1512: side-by-side compare counters (match + total).
        std::atomic<std::uint64_t> consistency_compare_total{0};
        std::atomic<std::uint64_t> consistency_match_total{0};
        // Issue #1512: bitmasks for opcode coverage (kIROpcodeCount=54 ≤ 64).
        // Bit i set ⇔ opcode i successfully lowered in some compile.
        std::atomic<std::uint64_t> opcode_covered_mask{0};
        // Bit i set ⇔ opcode i hit the unhandled default branch.
        std::atomic<std::uint64_t> opcode_unhandled_mask{0};
        // Issue #1285: exception opcode coverage (TryBegin/End/Raise/IsError).
        // Bumped each time lower() handles those opcodes so Agents can
        // confirm JIT EH coverage vs unhandled_opcode_count.
        std::atomic<std::uint64_t> exception_opcode_lowered{0};
        // Issue #1516: bitset of EH opcodes successfully lowered
        // (bit0=IsError, bit1=TryBegin, bit2=TryEnd, bit3=Raise).
        // popcount → exception_opcode_coverage_count() (0..4).
        std::atomic<std::uint64_t> exception_opcode_mask{0};
        // Issue #1516: per-function AOT emit counters.
        std::atomic<std::uint64_t> aot_per_function_ir_total{0};
        std::atomic<std::uint64_t> aot_per_function_object_total{0};
        std::atomic<std::uint64_t> aot_per_function_miss_total{0};
        std::atomic<std::uint64_t> aot_last_module_object_total{0};
        // Issue #1289: fail-fast default branch (return false → compile nullptr).
        std::atomic<std::uint64_t> unhandled_fail_fast_total{0};
        // Issue #1288: GuardShape paths that also probe linear_ownership_state.
        std::atomic<std::uint64_t> guard_shape_linear_unified_checks{0};
        // Issue #1514: partial recompile requests + dirty blocks observed.
        std::atomic<std::uint64_t> partial_recompile_requests{0};
        std::atomic<std::uint64_t> partial_recompile_dirty_blocks_total{0};
        std::atomic<std::uint64_t> partial_recompile_cache_evictions{0};
        // Issue #1522: fn_trackers_ batch deopt (bridge epoch bump notify).
        std::atomic<std::uint64_t> batch_deopt_for_total{0};
        std::atomic<std::uint64_t> batch_deopt_entries_marked{0};
        std::atomic<std::uint64_t> deopt_pending_invoke_fallbacks{0};
        // Issue #1477: JIT-side dual-epoch fence counter.
        std::atomic<std::uint64_t> jit_epoch_stale_check_total{0};
        // Issue #1536: bulk walk_active_closures (invalidate-time batch).
        std::atomic<std::uint64_t> walk_active_closures_total{0};
        std::atomic<std::uint64_t> walk_active_closures_examined{0};
        std::atomic<std::uint64_t> walk_active_closures_stale_found{0};
        // Issue #1537: Apply-prologue dual-epoch check (per native call).
        std::atomic<std::uint64_t> prologue_epoch_check_total{0};
        std::atomic<std::uint64_t> prologue_epoch_stale_deopt_total{0};
        std::atomic<std::uint64_t> prologue_emit_total{0};

        // Format as a single-line string for telemetry / log output.
        // Caller-provided buffer; returns the same pointer.
        char* format(char* buf, std::size_t buf_size) const noexcept;
    };
    const Metrics& metrics() const noexcept { return metrics_; }
    // Non-const accessor for tests + mutation. Production code
    // should prefer the const overload + the free-form update
    // methods (compile_count.fetch_add(1, ...), etc.).
    Metrics& mutable_metrics() noexcept { return metrics_; }

    // Issue #1477: test-only accessor for jit_epoch_stale_check_total.
    [[nodiscard]] std::uint64_t test_jit_epoch_stale_check_total() const noexcept {
        return metrics_.jit_epoch_stale_check_total.load(std::memory_order_relaxed);
    }

    // Issue #1512: strict consistency mode — unhandled opcodes also
    // bump consistency_violations (JIT cannot match interpreter).
    void set_strict_consistency_mode(bool on) noexcept { strict_consistency_mode_ = on; }
    [[nodiscard]] bool strict_consistency_mode() const noexcept { return strict_consistency_mode_; }
    // Issue #1512: record a JIT ↔ IRInterpreter side-by-side result.
    void record_consistency_result(bool match) noexcept;
    // Issue #1512: popcount of opcode_covered_mask / coverage percentage.
    [[nodiscard]] std::uint64_t opcode_coverage_count() const noexcept;
    [[nodiscard]] std::uint64_t opcode_coverage_pct() const noexcept; // 0..100
    static constexpr std::uint32_t kTrackedOpcodeCount = 54;          // matches kIROpcodeCount
    // Issue #1516: EH opcode coverage (IsError/TryBegin/TryEnd/Raise).
    [[nodiscard]] std::uint64_t exception_opcode_coverage_count() const noexcept;
    static constexpr std::uint32_t kExceptionOpcodeCount = 4;

    // Hot-swap: replace an already-compiled function with a new version.
    // Removes the old module from the JIT dylib and compiles + links the new one.

    // Invalidate the per-function compile cache entry (and the
    // ResourceTracker module) for `name`. Call this from a
    // redefine path so the next compile() actually re-runs the
    // LLVM pipeline instead of returning a stale fn_ptr.
    void invalidate(const char* name);
    // Issue #660 follow-up: invalidate all JIT cache entries
    // whose key starts with `prefix` (e.g. "mul" → erases
    // "mul#0", "mul#1", ...). Used by cache_define on redefine
    // since the actual JIT cache keys are `name + "#" + pos`,
    // not the bare name. Without prefix-based invalidation,
    // the redefine's old compiled function pointer remains
    // in compile_fns_ and the next exec returns the old body.
    void invalidate_prefix(const char* prefix);

    // Issue #1514: partial recompile request — evicts native code for
    // `name` (and name#* keys) so the next exec recompiles only the
    // dirty function, not the whole process.
    bool partial_recompile(const char* name, const std::uint32_t* dirty_block_ids,
                           std::size_t n_dirty_blocks) noexcept;

    // Issue #1522: soft batch deopt for fn_trackers_ after bridge epoch bump.
    std::size_t batch_deopt_for(const char* name, std::uint64_t current_epoch) noexcept;
    std::size_t batch_deopt_prefix(const char* prefix, std::uint64_t current_epoch) noexcept;
    [[nodiscard]] bool is_deopt_pending(const char* name) const noexcept;
    [[nodiscard]] std::uint64_t deopt_pending_count() const noexcept;

    // Issue #1477: JIT-side dual-epoch fence.
    void capture_fn_epoch(const char* name, std::uint64_t bridge_epoch);
    [[nodiscard]] bool is_fn_epoch_stale(const char* name,
                                         std::uint64_t current_bridge_epoch) const;

    // Issue #1536: bulk walk of active (captured) fns after invalidate /
    // mark_define_dirty. O(C + T) where C = |fn_captured_epochs_| and
    // T = |fn_trackers_| — not O(N²). For each captured fn with
    // is_fn_epoch_stale(name, current): bump jit_epoch_stale_check_total
    // and mark matching trackers deopt_pending (deopt-on-next-apply).
    // Returns the number of stale fns found (not tracker entries).
    std::size_t walk_active_closures(std::uint64_t current_bridge_epoch) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    mutable Metrics metrics_;
    // Issue #1512: when true, unhandled opcodes also bump
    // consistency_violations (strict JIT↔interp parity mode).
    bool strict_consistency_mode_ = false;
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
