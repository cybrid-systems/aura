module aura.compiler.compute_kind;
import std;

namespace aura::compiler {

using namespace aura::ir;

// Track which local slots are known/unknown at each point in the function
struct SlotState {
    std::vector<ComputeKind> slots; // slot index → ComputeKind
};

ComputeKindResult ComputeKindAnalysis::analyze(const IRFunction& func) {
    ComputeKindResult result;
    result.per_block_inst_kind.resize(func.blocks.size());
    result.valid = true;

    // Initialize slot tracking: all slots start Unknown
    SlotState state;
    state.slots.resize(func.local_count + 64, ComputeKind::Unknown);

    // Fixed-point iteration: propagate constants through the function
    bool changed = true;
    int max_iter = 10;  // limit iterations for safety
    while (changed && max_iter-- > 0) {
        changed = false;

        // Walk blocks in order
        for (std::size_t bi = 0; bi < func.blocks.size(); ++bi) {
            auto& block = func.blocks[bi];
            auto& inst_kinds = result.per_block_inst_kind[bi];
            inst_kinds.resize(block.instructions.size(), ComputeKind::Unknown);

            for (std::size_t ii = 0; ii < block.instructions.size(); ++ii) {
                auto& instr = block.instructions[ii];
                auto& ops = instr.operands;
                ComputeKind kind = ComputeKind::Unknown;

                switch (instr.opcode) {
                case IROpcode::Nop:
                    kind = ComputeKind::Known;
                    break;

                case IROpcode::ConstI64:
                    // Literal integers are always known
                    kind = ComputeKind::Known;
                    break;

                case IROpcode::Local: {
                    // If the source slot is known, this is known
                    auto src = ops[1];
                    if (src < state.slots.size() && state.slots[src] == ComputeKind::Known)
                        kind = ComputeKind::Known;
                    break;
                }

                case IROpcode::Arg:
                    // Arguments are unknown (runtime values)
                    kind = ComputeKind::Unknown;
                    break;

                case IROpcode::Add:
                case IROpcode::Sub:
                case IROpcode::Mul:
                case IROpcode::Div:
                case IROpcode::Eq:
                case IROpcode::Lt:
                case IROpcode::Gt:
                case IROpcode::Le:
                case IROpcode::Ge:
                case IROpcode::And:
                case IROpcode::Or: {
                    // Pure arithmetic/logic: known if all inputs are known
                    auto s1 = ops[1], s2 = ops[2];
                    bool known = (s1 < state.slots.size() && state.slots[s1] == ComputeKind::Known)
                              && (s2 < state.slots.size() && state.slots[s2] == ComputeKind::Known);
                    kind = known ? ComputeKind::Known : ComputeKind::Unknown;
                    break;
                }

                case IROpcode::Not: {
                    auto s1 = ops[1];
                    bool known = (s1 < state.slots.size() && state.slots[s1] == ComputeKind::Known);
                    kind = known ? ComputeKind::Known : ComputeKind::Unknown;
                    break;
                }

                case IROpcode::Branch:
                case IROpcode::Jump:
                    // Control flow doesn't produce a value slot
                    kind = ComputeKind::Known;
                    break;

                case IROpcode::Call:
                    // Function calls are unknown (unless we know the callee is a pure builtin)
                    kind = ComputeKind::Unknown;
                    break;

                case IROpcode::Return:
                    kind = ComputeKind::Unknown;
                    break;

                case IROpcode::MakeClosure: {
                    // Closure creation is pure: the closure value itself is known
                    // (it doesn't depend on runtime state; the env captures are already resolved)
                    kind = ComputeKind::Known;
                    break;
                }

                case IROpcode::Capture:
                    // Capture is a side effect on the closure, doesn't produce a value
                    kind = ComputeKind::Known;
                    break;

                case IROpcode::CaptureRef:
                    kind = ComputeKind::Known;
                    break;

                case IROpcode::Apply:
                    kind = ComputeKind::Unknown;
                    break;

                default:
                    kind = ComputeKind::Unknown;
                    break;
                }

                // Update instruction kind
                if (kind != inst_kinds[ii]) {
                    inst_kinds[ii] = kind;
                    changed = true;
                }

                // Update slot state if this instruction writes to a dest slot
                std::uint32_t dst = 0;
                bool has_dst = false;
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
                case IROpcode::MakeClosure:
                    dst = ops[0]; has_dst = true;
                    break;
                default:
                    break;
                }

                if (has_dst && dst < state.slots.size() && state.slots[dst] != kind) {
                    state.slots[dst] = kind;
                    changed = true;
                }
            }
        }
    }

    return result;
}

} // namespace aura::compiler
