module aura.compiler.ir_interpreter;
import std;

namespace aura::compiler {

using namespace aura::ir;

EvalResult IRInterpreter::execute(const IRFunction& func) {
    // Allocate stack for locals + temps
    stack_.resize(func.local_count + 64, 0);

    if (func.blocks.empty())
        return {false, 0, "empty function"};

    std::uint32_t current = func.entry_block;

    while (current < func.blocks.size()) {
        auto& block = func.blocks[current];

        for (auto& instr : block.instructions) {
            auto& ops = instr.operands;

            switch (instr.opcode) {
            case IROpcode::ConstI64: {
                // Pack two uint32 operands into int64
                std::int64_t val = (static_cast<std::int64_t>(ops[2]) << 32) | ops[1];
                stack_[ops[0]] = val;
                break;
            }
            case IROpcode::Local:
                stack_[ops[0]] = stack_[ops[1]];
                break;
            case IROpcode::Add:
                stack_[ops[0]] = stack_[ops[1]] + stack_[ops[2]];
                break;
            case IROpcode::Sub:
                stack_[ops[0]] = stack_[ops[1]] - stack_[ops[2]];
                break;
            case IROpcode::Mul:
                stack_[ops[0]] = stack_[ops[1]] * stack_[ops[2]];
                break;
            case IROpcode::Div:
                stack_[ops[0]] = stack_[ops[1]] / stack_[ops[2]];
                break;
            case IROpcode::Eq:
                stack_[ops[0]] = (stack_[ops[1]] == stack_[ops[2]]) ? 1 : 0;
                break;
            case IROpcode::Lt:
                stack_[ops[0]] = (stack_[ops[1]] < stack_[ops[2]]) ? 1 : 0;
                break;
            case IROpcode::Gt:
                stack_[ops[0]] = (stack_[ops[1]] > stack_[ops[2]]) ? 1 : 0;
                break;
            case IROpcode::Branch:
                current = (stack_[ops[0]] != 0) ? ops[1] : ops[2];
                goto next_block;
            case IROpcode::Jump:
                current = ops[0];
                goto next_block;
            case IROpcode::Return:
                return {true, stack_[ops[0]], ""};
            default:
                break;
            }
        }
        ++current;
        next_block:;
    }

    return {false, 0, "no return"};
}

} // namespace aura::compiler
