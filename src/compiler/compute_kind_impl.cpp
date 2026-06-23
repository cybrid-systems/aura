module;
#include <cstddef>
#include <span>
#include <vector>

module aura.compiler.compute_kind;
import std;

namespace aura::compiler {

using namespace aura::ir;

ComputeKindResult compute_kind(const IRFunction& func) {
    ComputeKindResult result;
    result.per_block_inst_kind.resize(func.blocks.size());
    result.valid = true;

    auto slot_count = func.local_count + 64;
    std::vector<ComputeKind> slots(slot_count, ComputeKind::Unknown);

    bool changed = true;
    int max_iter = 10;
    while (changed && max_iter-- > 0) {
        changed = false;
        for (std::size_t bi = 0; bi < func.blocks.size(); ++bi) {
            auto& block = func.blocks[bi];
            auto& kinds = result.per_block_inst_kind[bi];
            kinds.resize(block.instructions.size(), ComputeKind::Unknown);

            for (std::size_t ii = 0; ii < block.instructions.size(); ++ii) {
                auto& instr = block.instructions[ii];
                auto& ops = instr.operands;
                auto kind = ComputeKind::Unknown;

                switch (instr.opcode) {
                    case IROpcode::Nop:
                    case IROpcode::Branch:
                    case IROpcode::Jump:
                    case IROpcode::Capture:
                    case IROpcode::CaptureRef:
                        kind = ComputeKind::Known;
                        break;

                    case IROpcode::ConstI64:
                    case IROpcode::ConstF64:
                        kind = ComputeKind::Known;
                        break;

                    case IROpcode::Local:
                        if (ops[1] < slots.size() && slots[ops[1]] == ComputeKind::Known)
                            kind = ComputeKind::Known;
                        break;

                    case IROpcode::MakeClosure:
                        kind = ComputeKind::Known;
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
                        bool k = ops[1] < slots.size() && slots[ops[1]] == ComputeKind::Known &&
                                 ops[2] < slots.size() && slots[ops[2]] == ComputeKind::Known;
                        kind = k ? ComputeKind::Known : ComputeKind::Unknown;
                        break;
                    }
                    case IROpcode::Not: {
                        bool k = ops[1] < slots.size() && slots[ops[1]] == ComputeKind::Known;
                        kind = k ? ComputeKind::Known : ComputeKind::Unknown;
                        break;
                    }
                    default:
                        break;
                }

                if (kind != kinds[ii]) {
                    kinds[ii] = kind;
                    changed = true;
                }

                // Update slot state
                auto dst = ops[0];
                bool has_dst = false;
                switch (instr.opcode) {
                    case IROpcode::ConstI64:
                    case IROpcode::ConstF64:
                    case IROpcode::Local:
                    case IROpcode::Arg:
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
                    case IROpcode::Or:
                    case IROpcode::Not:
                    case IROpcode::Call:
                    case IROpcode::MakeClosure:
                        has_dst = true;
                        break;
                    default:
                        break;
                }
                if (has_dst && dst < slots.size() && slots[dst] != kind) {
                    slots[dst] = kind;
                    changed = true;
                }
            }
        }
    }
    return result;
}

// Issue #128: span-based instruction-list helper. For a
// single block, just compute the per-instruction kind.
// Pure function: takes a span of instructions, returns a
// vector of ComputeKind. The first instruction that writes
// a slot propagates the kind. Unlike the full compute_kind
// (which uses fixed-point iteration), this version is a
// single forward pass — suitable for "give me the kind of
// this block's instructions in isolation" callers (e.g.,
// the IR cache v2 re-analysis path).
std::vector<ComputeKind> compute_kind_instructions(std::span<const IRInstruction> instructions) {
    std::vector<ComputeKind> out;
    out.reserve(instructions.size());
    for (const auto& instr : instructions) {
        // Default: Unknown. Specific opcodes below override.
        ComputeKind kind = ComputeKind::Unknown;
        switch (instr.opcode) {
            case IROpcode::Nop:
            case IROpcode::Branch:
            case IROpcode::Jump:
            case IROpcode::Capture:
            case IROpcode::CaptureRef:
            case IROpcode::ConstI64:
            case IROpcode::ConstF64:
            case IROpcode::MakeClosure:
                kind = ComputeKind::Known;
                break;
            // Local/Call/Arith depend on the operand kind;
            // we mark them Unknown in this per-instruction
            // view (the full compute_kind does fixed-point
            // resolution).
            default:
                break;
        }
        out.push_back(kind);
    }
    return out;
}

} // namespace aura::compiler
