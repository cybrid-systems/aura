# 0 "/home/dev/code/aura/src/compiler/ir_executor.ixx"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/compiler/ir_executor.ixx"
export module aura.compiler.ir_executor;
import std;
import aura.core;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.evaluator;
import aura.diag;
import aura.compiler.value;

namespace aura::compiler {

using EvalValue = types::EvalValue;


export struct IRClosure {
    std::uint32_t func_id = 0;
    std::vector<EvalValue> env;

    const ast::FlatAST* flat = nullptr;
    const ast::StringPool* pool = nullptr;
    ast::NodeId body_id = ast::NULL_NODE;
    std::vector<std::string> params;
};



export struct PendingCall {
    const aura::ir::IRFunction* func;
    std::vector<EvalValue> args;
    std::uint32_t result_slot;
};




struct ExecFrame {
    const aura::ir::IRFunction* func = nullptr;
    std::uint32_t current_block = 0;
    std::vector<EvalValue> locals;
    std::vector<EvalValue> args;
    std::size_t resume_instr = 0;
    bool is_top_level = false;
    std::uint32_t result_slot = 0;
};



export struct ClosureSnapshot {
    std::uint64_t id;
    std::uint32_t func_id;
    std::string func_name;
    std::vector<std::string> func_params;
    std::vector<std::string> func_free_vars;
    std::vector<EvalValue> env;
};

export struct CellSnapshot {
    std::uint64_t id;
    EvalValue value;
};


export struct EvalStrategy {
    bool enable_inlining = true;
    bool enable_specialization = false;
    int max_unroll = 3;
    bool verbose_inspect = false;
};

export class IRInterpreter {
public:
    explicit IRInterpreter(const aura::ir::IRModule& mod,
                           const Primitives& prims,
                           const aura::core::TypeRegistry* types = nullptr)
        : module_(mod), primitives_(prims), type_registry_(types) {}


    EvalResult execute();



    std::optional<ClosureSnapshot> inspect_closure(std::uint64_t closure_id) const;


    std::vector<ClosureSnapshot> list_closures() const;


    std::vector<CellSnapshot> list_cells() const;


    const EvalStrategy& strategy() const { return strategy_; }
    void set_strategy(const EvalStrategy& s) { strategy_ = s; }


    void set_strict_mode(bool s) { strict_mode_ = s; }
    bool strict_mode() const { return strict_mode_; }


    std::size_t closure_count() const { return runtime_closures_.size(); }
    std::size_t cell_count() const { return cell_heap_.size(); }

private:

    using RunResult = std::variant<EvalResult, PendingCall>;


    EvalResult execute_function(const aura::ir::IRFunction& func,
                                 const std::vector<EvalValue>& args);




    RunResult run_function(const aura::ir::IRFunction& func,
                            std::vector<EvalValue>& locals,
                            const std::vector<EvalValue>& args);


    ClosureSnapshot make_snapshot(std::uint64_t id,
                                   const IRClosure& closure) const;



    std::optional<aura::diag::Diagnostic> check_runtime_type(
        std::uint32_t type_id, const EvalValue& val, std::string_view context);


    static std::optional<aura::core::TypeTag> value_type_tag(const EvalValue& val);

    const aura::ir::IRModule& module_;
    const Primitives& primitives_;
    const aura::core::TypeRegistry* type_registry_ = nullptr;
    EvalStrategy strategy_;
    bool strict_mode_ = false;


    std::uint64_t next_closure_id_ = 1ull << 48;
    std::unordered_map<std::uint64_t, IRClosure> runtime_closures_;


    std::uint64_t next_cell_id_ = 1;
    std::unordered_map<std::uint64_t, EvalValue> cell_heap_;


    std::vector<std::string> string_heap_;


    std::vector<ExecFrame> call_stack_;
};

}
