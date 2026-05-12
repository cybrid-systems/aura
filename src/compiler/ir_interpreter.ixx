export module aura.compiler.ir_interpreter;
import std;
import aura.core;
import aura.compiler.ir;
import aura.compiler.frontend;  // for EvalResult
import aura.diag;              // for Diagnostic

namespace aura::compiler {

// Runtime closure value
export struct IRClosure {
    std::uint32_t func_id = 0;
    std::vector<std::int64_t> env;
};

// Call frame for recursive IR execution
struct CallFrame {
    const aura::ir::IRFunction* func = nullptr;
    std::uint32_t current_block = 0;
    std::vector<std::int64_t> locals;
    std::size_t instr_index = 0;
};

// IR interpreter — lowered code execution with closure support
// ── Runtime reflection: closure/cell introspection ──────────────
export struct ClosureSnapshot {
    std::uint64_t     id;
    std::uint32_t     func_id;
    std::string       func_name;
    std::vector<std::int64_t> env;
};

export struct CellSnapshot {
    std::uint64_t id;
    std::int64_t  value;
};

// ── Evaluation strategy (flambda-style) ─────────────────────────
export struct EvalStrategy {
    bool enable_inlining       = true;
    bool enable_specialization = false;
    int  max_unroll            = 3;
    bool verbose_inspect       = false;
};

export class IRInterpreter {
public:
    explicit IRInterpreter(const aura::ir::IRModule& mod,
                           const Primitives& prims)
        : module_(mod), primitives_(prims) {}

    // Execute the top-level function and return result
    EvalResult execute();

    // ── Runtime reflection API ─────────────────────────────────
    // Inspect a single closure by id
    std::optional<ClosureSnapshot> inspect_closure(std::uint64_t closure_id) const;

    // List all active closures
    std::vector<ClosureSnapshot> list_closures() const;

    // List all active mutable cells
    std::vector<CellSnapshot> list_cells() const;

    // Get/set evaluation strategy
    const EvalStrategy& strategy() const { return strategy_; }
    void set_strategy(const EvalStrategy& s) { strategy_ = s; }

    // Counters
    std::size_t closure_count() const { return runtime_closures_.size(); }
    std::size_t cell_count() const { return cell_heap_.size(); }

private:
    // Execute a specific function with given args
    EvalResult execute_function(const aura::ir::IRFunction& func,
                                 const std::vector<std::int64_t>& args);

    // Step through instructions (args are separate from locals for Arg opcode)
    EvalResult run_function(const aura::ir::IRFunction& func,
                             std::vector<std::int64_t>& locals,
                             const std::vector<std::int64_t>& args);

    const aura::ir::IRModule& module_;
    const Primitives& primitives_;
    EvalStrategy strategy_;

    // Per-instance closure storage
    std::uint64_t next_closure_id_ = 1;
    std::unordered_map<std::uint64_t, IRClosure> runtime_closures_;

    // Per-instance mutable cell heap (for letrec)
    std::uint64_t next_cell_id_ = 1;
    std::unordered_map<std::uint64_t, std::int64_t> cell_heap_;
};

} // namespace aura::compiler
