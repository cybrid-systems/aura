export module aura.compiler.ir_executor;
import std;
import aura.core;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.evaluator;  // for EvalResult
import aura.diag;              // for Diagnostic
import aura.compiler.value;

namespace aura::compiler {

using EvalValue = types::EvalValue;

// Runtime closure value
export struct IRClosure {
    std::uint32_t func_id = 0;
    std::vector<EvalValue> env;
    // Original tree-walker closure info for bridge (nullptr if not available)
    const ast::FlatAST* flat = nullptr;
    const ast::StringPool* pool = nullptr;
    ast::NodeId body_id = ast::NULL_NODE;
    std::vector<std::string> params;
};

// Call frame for recursive IR execution
struct CallFrame {
    const aura::ir::IRFunction* func = nullptr;
    std::uint32_t current_block = 0;
    std::vector<EvalValue> locals;
    std::size_t instr_index = 0;
};

// IR interpreter — lowered code execution with closure support
// ── Runtime reflection: closure/cell introspection ──────────────
export struct ClosureSnapshot {
    std::uint64_t          id;
    std::uint32_t          func_id;
    std::string            func_name;
    std::vector<std::string> func_params;     // from IRFunction::params
    std::vector<std::string> func_free_vars;  // from IRFunction::free_vars
    std::vector<EvalValue> env;
};

export struct CellSnapshot {
    std::uint64_t id;
    EvalValue  value;
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
                           const Primitives& prims,
                           const aura::core::TypeRegistry* types = nullptr)
        : module_(mod), primitives_(prims), type_registry_(types) {}

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

    // Strict mode: runtime type assertions based on IR instruction type_id
    void set_strict_mode(bool s) { strict_mode_ = s; }
    bool strict_mode() const { return strict_mode_; }

    // Counters
    std::size_t closure_count() const { return runtime_closures_.size(); }
    std::size_t cell_count() const { return cell_heap_.size(); }

private:
    // Execute a specific function with given args
    EvalResult execute_function(const aura::ir::IRFunction& func,
                                 const std::vector<EvalValue>& args);

    // Step through instructions (args are separate from locals for Arg opcode)
    EvalResult run_function(const aura::ir::IRFunction& func,
                             std::vector<EvalValue>& locals,
                             const std::vector<EvalValue>& args);

    // Build a snapshot from runtime closure data
    ClosureSnapshot make_snapshot(std::uint64_t id,
                                   const IRClosure& closure) const;

    // Runtime type assertion: check if runtime value matches IR type_id
    // Returns nullopt on match, or a diagnostic on mismatch (strict mode only)
    std::optional<aura::diag::Diagnostic> check_runtime_type(
        std::uint32_t type_id, const EvalValue& val, std::string_view context);

    // Map runtime EvalValue to type tag (for type assertion)
    static std::optional<aura::core::TypeTag> value_type_tag(const EvalValue& val);

    const aura::ir::IRModule& module_;
    const Primitives& primitives_;
    const aura::core::TypeRegistry* type_registry_ = nullptr;
    EvalStrategy strategy_;
    bool strict_mode_ = false;

    // Per-instance closure storage
    std::uint64_t next_closure_id_ = 1ull << 48;
    std::unordered_map<std::uint64_t, IRClosure> runtime_closures_;

    // Per-instance mutable cell heap (for letrec)
    std::uint64_t next_cell_id_ = 1;
    std::unordered_map<std::uint64_t, EvalValue> cell_heap_;

    // Runtime string heap (for Int→String coercion)
    std::vector<std::string> string_heap_;
};

} // namespace aura::compiler
