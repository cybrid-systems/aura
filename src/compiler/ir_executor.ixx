module;
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
#include "observability_metrics.h"
#include "value_tags.h" // kClosureIdHighBit #907
export module aura.compiler.ir_executor;
import std;
import aura.core;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.evaluator; // for EvalResult
import aura.diag;               // for Diagnostic
import aura.compiler.value;

namespace aura::compiler {

using EvalValue = types::EvalValue;

// Runtime closure value
//
// Issue #224 Cycle 2: shared_ptr-based bridge. The shared_ptr
// is a non-owning view (constructed with a no-op deleter at
// MakeClosure time) that keeps the FlatAST/StringPool alive
// as long as the closure exists — even after the lowering
// arena is reset. The arena remains the actual owner.
export struct IRClosure {
    std::uint32_t func_id = 0;
    // Issue #660 Option 1: function NAME for cross-module closure
    // identity. When func_id is out of bounds in the current module,
    // the runtime falls back to looking up the function by name.
    std::string name;
    std::vector<EvalValue> env;
    // Original tree-walker closure info for bridge (empty shared_ptr
    // = not available). See Issue #224 Cycle 2.
    std::shared_ptr<const ast::FlatAST> flat;
    std::shared_ptr<const ast::StringPool> pool;
    ast::NodeId body_id = ast::NULL_NODE;
    std::vector<std::string> params;
    // Issue #223: epoch captured from the bridge at MakeClosure
    // time. The IRExecutor / apply_closure compares this against
    // the service's bridge_epoch(); a mismatch means the bridge
    // is stale (arena reset / major mutation). The closure falls
    // back to re-parse from body_source or is invalidated.
    std::uint64_t bridge_epoch = 0;
};

// Returned by run_function's Call handler to signal a new call is needed.
// The outer while loop in execute() will push a new frame and continue.
export struct PendingCall {
    const aura::ir::IRFunction* func;
    std::vector<EvalValue> args;
    std::uint32_t result_slot;
};

// Frame managed by the explicit call stack in execute().
// run_function still uses its `args` parameter (not this frame);
// the outer loop passes frame.args as the args= argument.
struct ExecFrame {
    const aura::ir::IRFunction* func = nullptr;
    std::uint32_t current_block = 0;
    std::vector<EvalValue> locals;
    std::vector<EvalValue> args;
    std::size_t resume_instr = 0;
    bool is_top_level = false;
    std::uint32_t result_slot = 0;
    // Issue #124: per-frame exception-stack depth marker. The
    // interpreter's outer exception stack (ex_stack_) is shared
    // across all call frames; this depth records where this frame
    // entered (so popping back across a function boundary
    // unwinds handlers opened in this frame, not outer frames).
    std::size_t ex_depth_at_entry = 0;
};

// Issue #124: per-interpreter exception stack. Each TryBegin
// pushes a frame; Raise looks up the top frame to find the
// handler block; TryEnd pops the frame. The exception payload
// (a runtime EvalValue) is stored in `payload` until the
// handler is invoked.
struct ExHandler {
    std::uint32_t handler_block = 0;
    std::uint32_t result_slot = 0;  // where to store the caught value
    std::uint32_t payload_slot = 0; // temp slot for the exception payload
};

// IR interpreter — lowered code execution with closure support
// ── Runtime reflection: closure/cell introspection ──────────────
export struct ClosureSnapshot {
    std::uint64_t id;
    std::uint32_t func_id;
    std::string func_name;
    std::vector<std::string> func_params;      // from IRFunction::params
    std::vector<std::string> func_param_types; // parameter type names (M3 §8.2)
    std::string func_return_type;              // return type name (M3 §8.2)
    std::vector<std::string> func_free_vars;   // from IRFunction::free_vars
    std::vector<EvalValue> env;
};

export struct CellSnapshot {
    std::uint64_t id;
    EvalValue value;
};

// ── Evaluation strategy (flambda-style) ─────────────────────────
export struct EvalStrategy {
    bool enable_inlining = true;
    bool enable_specialization = false;
    int max_unroll = 3;
    bool verbose_inspect = false;
};

// (#111) Bundles everything the IR interpreter needs from the
// surrounding state (Primitives table, TypeRegistry, optional
// metrics). The interpreter holds a reference to the context;
// the caller is responsible for keeping the context alive for
// the interpreter's lifetime. This replaces the previous design
// where the interpreter held separate `Primitives&` + `TypeRegistry*`
// fields, which made reference invalidation easy to trigger via
// the surrounding session/evaluator being modified mid-execution.
//
// Future fields (escape_maps, EvalStrategy, etc.) belong in this
// struct so all IR-runtime state has a single lifetime owner.
export struct IRContext {
    Primitives& primitives; // non-const: ConstString mutates string_heap
    const aura::core::TypeRegistry* type_registry = nullptr;
    CompilerMetrics* metrics = nullptr;
    // Issue #272 Cycle 3: optional evaluator for TopCellLoad (value defines).
    Evaluator* evaluator = nullptr;
    // Issue #684: optional SoA instruction_dirty_ probe (block, instr_idx).
    std::function<bool(std::uint32_t, std::size_t)> is_instruction_dirty_fn;

    // References must be bound at construction. The caller passes
    // the primitives reference; type_registry and metrics are
    // optional. The IRContext is typically stack-allocated inside
    // cs.eval(), with lifetime matching the IRInterpreter's.
    IRContext(Primitives& p, const aura::core::TypeRegistry* t = nullptr,
              CompilerMetrics* m = nullptr, Evaluator* eval = nullptr)
        : primitives(p)
        , type_registry(t)
        , metrics(m)
        , evaluator(eval) {}
};

export class IRInterpreter {
public:
    // The IR interpreter holds a reference to an IRContext that
    // bundles all the external state the IR runtime needs. The
    // caller must keep the context alive for the interpreter's
    // lifetime (typically: the context is owned by the Evaluator /
    // CompilerService, and the interpreter is a short-lived local
    // in cs.eval).
    //
    // Backward-compat: the 2/3-arg constructors wrap the legacy
    // (module, primitives, types) signature by building a transient
    // IRContext on the stack. The legacy code uses stack-locals so
    // the context lifetime extends to the interpreter's destruction.
    // The IR interpreter holds a reference to an IRContext that
    // bundles all the external state the IR runtime needs. The
    // caller must keep the context alive for the interpreter's
    // lifetime (typically: the context is stack-allocated inside
    // cs.eval, with lifetime matching the IRInterpreter's).
    explicit IRInterpreter(const aura::ir::IRModule& mod, IRContext& ctx)
        : module_(mod)
        , context_(ctx) {}

    // Issue #62 Iter 1: attach a metrics struct. Optional so the
    // existing test code that constructs IRInterpreter with two args
    // keeps working. When set, hot paths increment counters for
    // --evo-explain and AuraQuery.
    void set_metrics(CompilerMetrics* m) { context_.metrics = m; }

    // Execute the top-level function and return result
    EvalResult execute();

    // Issue #272: invoke a runtime closure by id (full IR path).
    EvalResult call_closure(std::uint64_t closure_id, std::span<const EvalValue> args);

    // ── Runtime reflection API ─────────────────────────────────
    // Inspect a single closure by id
    std::optional<ClosureSnapshot> inspect_closure(std::uint64_t closure_id) const;

    // List all active closures
    std::vector<ClosureSnapshot> list_closures() const;

    // Get closure type as string (M3 §8.2 runtime introspection)
    std::string type_of_closure(std::uint64_t closure_id) const;

    // List all active mutable cells
    std::vector<CellSnapshot> list_cells() const;

    // Get/set evaluation strategy
    const EvalStrategy& strategy() const { return strategy_; }
    void set_strategy(const EvalStrategy& s) { strategy_ = s; }

    // Strict mode: runtime type assertions based on IR instruction type_id
    void set_strict_mode(bool s) { strict_mode_ = s; }
    bool strict_mode() const { return strict_mode_; }

    // Escape analysis: set per-function escape maps.
    // escape_maps_[func_id] is a vector<uint8_t>: 1=ESCAPED, 0=NON_ESCAPING.
    void set_escape_maps(std::vector<std::vector<std::uint8_t>> maps) {
        escape_maps_ = std::move(maps);
    }
    bool has_escape_info() const { return !escape_maps_.empty(); }

    // Counters
    std::size_t closure_count() const { return runtime_closures_.size(); }
    std::size_t cell_count() const { return cell_heap_.size(); }

    // Issue #682: active-frame probe + GC root export for compiler
    // IRClosure coordination during invalidate / hot-swap.
    [[nodiscard]] bool has_active_frames() const noexcept { return !call_stack_.empty(); }
    void collect_active_gc_roots(std::vector<std::int64_t>& closure_roots_out,
                                 std::uint64_t current_bridge_epoch) const;

    // Issue #601: live-closure walk for proactive refresh on
    // invalidate_function. Iterates every entry in runtime_closures_
    // and invokes `cb(closure_id, IRClosure&)` so the caller can
    // inspect / mutate bridge_epoch in place. Returns the number of
    // closures visited. Same data-race contract as collect_active_gc_roots
    // / list_closures (uint64_t-aligned reads/writes are atomic on the
    // supported targets; the closure object mutation is best-effort
    // with respect to concurrent apply_closure — same as the
    // existing list_closures path).
    template <typename F> std::size_t walk_runtime_closures(F&& cb) {
        std::size_t visited = 0;
        for (auto& entry : runtime_closures_) {
            cb(entry.first, entry.second);
            ++visited;
        }
        return visited;
    }

private:
    // Result of run_function: either an EvalResult (Return/error) or PendingCall (need to push
    // frame)
    using RunResult = std::variant<EvalResult, PendingCall>;

    // Execute a specific function with given args (backward compat wrapper)
    EvalResult execute_function(const aura::ir::IRFunction& func, std::span<const EvalValue> args);

    // Step through instructions. Returns RunResult:
    //   - EvalResult on Return/error
    //   - PendingCall on Call/Apply (outer loop will push new frame and continue)
    RunResult run_function(const aura::ir::IRFunction& func, std::vector<EvalValue>& locals,
                           std::span<const EvalValue> args);

    // Build a snapshot from runtime closure data
    ClosureSnapshot make_snapshot(std::uint64_t id, const IRClosure& closure) const;

    // Runtime type assertion: check if runtime value matches IR type_id
    // Returns nullopt on match, or a diagnostic on mismatch (strict mode only)
    std::optional<aura::diag::Diagnostic>
    check_runtime_type(std::uint32_t type_id, const EvalValue& val, std::string_view context);

    // Map runtime EvalValue to type tag (for type assertion)
    static std::optional<aura::core::TypeTag> value_type_tag(const EvalValue& val);

    const aura::ir::IRModule& module_;
    // (#111) The IR context bundles the external state the interpreter
    // needs (Primitives, TypeRegistry, metrics). The interpreter
    // holds a reference; the caller is responsible for the context's
    // lifetime. `legacy_context_` is the stack-local IRContext built
    // by the legacy 3-arg constructor, used when the 2-arg
    // `IRContext&` ctor is not used. `context_` always aliases one
    // of the two; both are valid for the interpreter's lifetime.
    IRContext& context_;
    EvalStrategy strategy_;
    bool strict_mode_ = false;
    // Issue #62 Iter 1: optional pointer to the compiler-wide
    // metrics struct. Hot paths check it before incrementing.
    // Now stored inside IRContext; kept here for ABI compat.
    CompilerMetrics* metrics_ = nullptr;

    // Per-instance closure storage
    std::uint64_t next_closure_id_ = aura::compiler::types::kClosureIdHighBit; // #907
    std::unordered_map<std::uint64_t, IRClosure> runtime_closures_;

    // Per-instance mutable cell heap (for letrec)
    std::uint64_t next_cell_id_ = 1;
    std::unordered_map<std::uint64_t, EvalValue> cell_heap_;

    // M4 Linear ownership runtime heap
    struct LinearEntry {
        EvalValue value;
        std::size_t ref_count;
    };
    std::uint64_t next_linear_id_ = 1;
    std::unordered_map<std::uint64_t, LinearEntry> linear_heap_;

    // Runtime string heap (for Int\xE2\x86\x92String coercion)
    std::vector<std::string> string_heap_;

    // Explicit call stack: replaces C++ recursion for closure calls
    std::vector<ExecFrame> call_stack_;

    // Issue #124: exception stack. TryBegin pushes a frame; Raise
    // unwinds to the top frame's handler_block; TryEnd pops.
    // Shared across all call frames (the per-frame ex_depth_at_entry
    // marker handles frame-local scoping on return).
    std::vector<ExHandler> ex_stack_;

    // Per-function escape maps from IR escape analysis.
    // escape_maps_[func_id][slot] = 1 (ESCAPED) or 0 (NON_ESCAPING).
    // Empty if no escape info available.
    std::vector<std::vector<std::uint8_t>> escape_maps_;
};

} // namespace aura::compiler
