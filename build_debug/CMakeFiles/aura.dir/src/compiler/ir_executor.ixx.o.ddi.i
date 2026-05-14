# 0 "/home/dev/code/aura/src/compiler/ir_executor.ixx"
# 1 "/home/dev/code/aura/build_debug//"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/compiler/ir_executor.ixx"
export module aura.compiler.ir_executor;
import std;
import aura.core;
import aura.compiler.ir;
import aura.compiler.evaluator;
import aura.diag;
import aura.compiler.value;

namespace aura::compiler {

using EvalValue = types::EvalValue;


export struct IRClosure {
    std::uint32_t func_id = 0;
    std::vector<EvalValue> env;
};


struct CallFrame {
    const aura::ir::IRFunction* func = nullptr;
    std::uint32_t current_block = 0;
    std::vector<EvalValue> locals;
    std::size_t instr_index = 0;
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
                           const Primitives& prims)
        : module_(mod), primitives_(prims) {}


    EvalResult execute();



    std::optional<ClosureSnapshot> inspect_closure(std::uint64_t closure_id) const;


    std::vector<ClosureSnapshot> list_closures() const;


    std::vector<CellSnapshot> list_cells() const;


    const EvalStrategy& strategy() const { return strategy_; }
    void set_strategy(const EvalStrategy& s) { strategy_ = s; }


    std::size_t closure_count() const { return runtime_closures_.size(); }
    std::size_t cell_count() const { return cell_heap_.size(); }

private:

    EvalResult execute_function(const aura::ir::IRFunction& func,
                                 const std::vector<EvalValue>& args);


    EvalResult run_function(const aura::ir::IRFunction& func,
                             std::vector<EvalValue>& locals,
                             const std::vector<EvalValue>& args);


    ClosureSnapshot make_snapshot(std::uint64_t id,
                                   const IRClosure& closure) const;

    const aura::ir::IRModule& module_;
    const Primitives& primitives_;
    EvalStrategy strategy_;


    std::uint64_t next_closure_id_ = 1;
    std::unordered_map<std::uint64_t, IRClosure> runtime_closures_;


    std::uint64_t next_cell_id_ = 1;
    std::unordered_map<std::uint64_t, EvalValue> cell_heap_;


    std::vector<std::string> string_heap_;
};

}
