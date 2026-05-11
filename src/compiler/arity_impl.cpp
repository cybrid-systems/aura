module aura.compiler.arity;
import std;

namespace aura::compiler {

using namespace aura::ir;

// ── Internal helpers ───────────────────────────────────────────
namespace {

int resolve_callee(const IRFunction& func, const IRModule& mod, std::uint32_t slot) {
    for (auto& block : func.blocks) {
        for (auto& instr : block.instructions) {
            std::uint32_t dst = 0;
            bool writes = false;
            switch (instr.opcode) {
            case IROpcode::ConstI64: case IROpcode::Local: case IROpcode::Arg:
            case IROpcode::Add: case IROpcode::Sub: case IROpcode::Mul: case IROpcode::Div:
            case IROpcode::Eq: case IROpcode::Lt: case IROpcode::Gt:
            case IROpcode::Le: case IROpcode::Ge: case IROpcode::And:
            case IROpcode::Or: case IROpcode::Not: case IROpcode::Call:
                dst = instr.operands[0]; writes = true; break;
            case IROpcode::MakeClosure:
                dst = instr.operands[0]; writes = true; break;
            default: break;
            }
            if (writes && dst == slot) {
                if (instr.opcode == IROpcode::MakeClosure)
                    return static_cast<int>(instr.operands[1]);
                if (instr.opcode == IROpcode::Local)
                    return resolve_callee(func, mod, instr.operands[1]);
                return -1;
            }
        }
    }
    return -1;
}

void check_call_in_func(const IRFunction& func, const IRModule& mod,
                        const IRInstruction& instr,
                        std::uint32_t block_id, std::uint32_t instr_index,
                        std::vector<ArityDiagnostic>& diags) {
    auto& ops = instr.operands;
    auto callee_id = resolve_callee(func, mod, ops[0]);
    if (callee_id >= 0 && callee_id < static_cast<int>(mod.functions.size())) {
        auto& callee = mod.functions[callee_id];
        auto expected = callee.arg_count;
        auto actual = ops[2];
        if (actual != expected) {
            ArityDiagnostic d;
            d.func_id = func.id; d.block_id = block_id; d.instr_index = instr_index;
            d.expected = expected; d.actual = actual;
            d.function_name = callee.name;
            d.message = "arity mismatch: call to '" + callee.name
                      + "' expects " + std::to_string(expected)
                      + " args but got " + std::to_string(actual);
            diags.push_back(d);
        }
    }
}

} // anonymous namespace

// ── Public API ─────────────────────────────────────────────────
ArityCheckResult check_arity(const IRModule& mod) {
    ArityCheckResult result;
    for (auto& func : mod.functions) {
        for (std::size_t bi = 0; bi < func.blocks.size(); ++bi) {
            auto& block = func.blocks[bi];
            for (std::size_t ii = 0; ii < block.instructions.size(); ++ii) {
                auto& instr = block.instructions[ii];
                if (instr.opcode == IROpcode::Call) {
                    check_call_in_func(func, mod, instr,
                        static_cast<std::uint32_t>(bi),
                        static_cast<std::uint32_t>(ii),
                        result.diagnostics);
                }
            }
        }
    }
    if (!result.diagnostics.empty()) result.has_error = true;
    return result;
}

} // namespace aura::compiler
