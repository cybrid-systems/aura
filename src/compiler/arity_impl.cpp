module aura.compiler.arity;
import std;

namespace aura::compiler {

using namespace aura::ir;

ArityCheckResult ArityChecker::check(const IRModule& mod) {
    ArityCheckResult result;
    for (auto& func : mod.functions) {
        auto r = check_function(func, mod);
        if (r.has_error) result.has_error = true;
        result.diagnostics.insert(result.diagnostics.end(),
                                   r.diagnostics.begin(), r.diagnostics.end());
    }
    return result;
}

ArityCheckResult ArityChecker::check_function(const IRFunction& func,
                                               const IRModule& mod) {
    ArityCheckResult result;

    for (std::size_t bi = 0; bi < func.blocks.size(); ++bi) {
        auto& block = func.blocks[bi];
        for (std::size_t ii = 0; ii < block.instructions.size(); ++ii) {
            auto& instr = block.instructions[ii];
            if (instr.opcode == IROpcode::Call) {
                check_call(func, mod, instr,
                           static_cast<std::uint32_t>(bi),
                           static_cast<std::uint32_t>(ii),
                           result.diagnostics);
            }
        }
    }

    if (!result.diagnostics.empty()) result.has_error = true;
    return result;
}

int ArityChecker::resolve_callee_func(const IRFunction& func,
                                       const IRModule& mod,
                                       std::uint32_t slot) const {
    // Walk backwards through the function's instructions to find
    // the definition of this slot
    for (std::size_t bi = 0; bi < func.blocks.size(); ++bi) {
        auto& block = func.blocks[bi];
        for (std::size_t ii = 0; ii < block.instructions.size(); ++ii) {
            auto& instr = block.instructions[ii];

            // Check if this instruction writes to the target slot
            std::uint32_t dst = 0;
            bool writes_slot = false;

            switch (instr.opcode) {
            case IROpcode::ConstI64:
            case IROpcode::Local:
            case IROpcode::Arg:
            case IROpcode::Add: case IROpcode::Sub:
            case IROpcode::Mul: case IROpcode::Div:
            case IROpcode::Eq: case IROpcode::Lt:
            case IROpcode::Gt: case IROpcode::Le: case IROpcode::Ge:
            case IROpcode::And: case IROpcode::Or: case IROpcode::Not:
            case IROpcode::Call:
                dst = instr.operands[0]; writes_slot = true;
                break;
            case IROpcode::MakeClosure:
                dst = instr.operands[0]; writes_slot = true;
                break;
            default:
                break;
            }

            if (writes_slot && dst == slot) {
                // Found the definition
                if (instr.opcode == IROpcode::MakeClosure) {
                    return static_cast<int>(instr.operands[1]); // func_id
                }
                if (instr.opcode == IROpcode::Local) {
                    // Follow the chain (careful about infinite loops)
                    return resolve_callee_func(func, mod, instr.operands[1]);
                }
                // Other opcodes: callee is not a known closure
                return -1;
            }
        }
    }

    // Slot not defined in this function — might be an argument or from another block
    return -1;
}

void ArityChecker::check_call(const IRFunction& func,
                               const IRModule& mod,
                               const IRInstruction& instr,
                               std::uint32_t block_id,
                               std::uint32_t instr_index,
                               std::vector<ArityDiagnostic>& diags) {
    auto& ops = instr.operands;
    auto callee_slot = ops[0];
    auto call_arg_count = ops[2]; // ops[2] = arg_count in Call instruction

    // Try to resolve the callee
    auto callee_func_id = resolve_callee_func(func, mod, callee_slot);

    if (callee_func_id >= 0 && callee_func_id < static_cast<int>(mod.functions.size())) {
        auto& callee_func_obj = mod.functions[callee_func_id];
        auto expected = callee_func_obj.arg_count;

        if (call_arg_count != expected) {
            ArityDiagnostic diag;
            diag.func_id = func.id;
            diag.block_id = block_id;
            diag.instr_index = instr_index;
            diag.expected = expected;
            diag.actual = call_arg_count;
            diag.is_warning = false;
            diag.function_name = callee_func_obj.name;
            diag.message = "arity mismatch: call to '" + callee_func_obj.name
                         + "' expects " + std::to_string(expected)
                         + " args but got " + std::to_string(call_arg_count);
            diags.push_back(diag);
        }
    }
    // Unknown callee: emit runtime arity check info (not a compile-time error)
}

} // namespace aura::compiler
