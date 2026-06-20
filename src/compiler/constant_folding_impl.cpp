// Issue #212 — pure-function implementation of constant folding.
//
// The fold logic is ported from the legacy `ConstantFoldingWrap`
// in pass_manager.ixx. Two key changes:
//
//   1. The logic is now a free function, not a class method.
//      `known_` and `folded_` are no longer member state; they
//      are parameters (or a per-block temp in the function
//      variant).
//
//   2. `replace` and `replace_bool` are free functions, not
//      private member functions. They no longer touch `known_`
//      — the per-block caller is responsible for the map. This
//      makes the helpers reusable from the span-based variant
//      and from any other pure function in the future.
//
// Behavior is byte-identical to the legacy Wrap (no observable
// change to existing tests). The fact that behavior is preserved
// is verified by test_issue_212's parity test (same folded_count
// as the legacy Wrap, same final IR after fold).

module aura.compiler.constant_folding;
import std;

namespace aura::compiler {

using namespace aura::ir;

// ── Internal helpers (file-local) ─────────────────────────────

// IS_TRUTHY: tagged-bool-aware truthiness test.
//   - 3 (#f)        → falsy
//   - 0 (int 0)     → falsy
//   - everything else → truthy
//
// Mirrors the legacy macro from pass_manager.ixx exactly. The
// match is intentional: a test that exercises the same IR
// through the pure function must produce the same outcome.
namespace {
    constexpr bool is_truthy_val(ConstantValue v) noexcept {
        return v != 3 && v != 0;
    }
} // namespace

// ── replace — mutate an instruction to a ConstI64 and update known ───
//
// Same encoding as the legacy helper: operands = {slot, low, high, 0}.
// Also updates the known map so subsequent folds in the same
// pass can see the just-folded value.
//
// Why this update is critical: the per-block fold loop walks
// the instruction list forward. When a binary op (e.g. Add)
// folds in-place to ConstI64, the NEXT iteration is at the
// following index — it does NOT re-process the just-folded
// ConstI64. So unless we update `known` right here, the new
// constant is invisible to subsequent reads, and dependent
// operations (e.g. Sub reading the just-folded Add's result)
// fail to fold.
//
// The legacy in-class version did the same thing via
// `known_[slot] = val` inside the class method. The pure
// version preserves that behavior by passing the known map
// to the helper.
static void replace(aura::ir::IRInstruction& instr, std::uint32_t slot, std::int64_t val,
                    ConstantKnownMap& known) {
    instr.opcode = aura::ir::IROpcode::ConstI64;
    instr.operands = {slot, static_cast<std::uint32_t>(val & 0xFFFFFFFF),
                      static_cast<std::uint32_t>((val >> 32) & 0xFFFFFFFF), 0};
    known[slot] = val;
}

// ── replace_bool — mutate an instruction to a ConstBool and update known ───
//
// Tagged encoding: 7 = #t, 3 = #f. The 4-operand layout uses
// {slot, val(0/1), 0, 0} for the instruction, and 7/3 for the
// known map entry (so the same map can carry tagged bools).
static void replace_bool(aura::ir::IRInstruction& instr, std::uint32_t slot, bool val,
                         ConstantKnownMap& known) {
    instr.opcode = aura::ir::IROpcode::ConstBool;
    instr.operands = {slot, val ? 1u : 0u, 0, 0};
    known[slot] = val ? 7 : 3;
}

// ── constant_fold_block — pure per-block folder ──────────────
//
// Same logic as the legacy `ConstantFoldingWrap::fold_block`,
// but with `known` as an in-out parameter and the count as the
// return value. The caller owns the map; the function is
// purely about per-instruction decisions.
//
// This is the hot-path span variant: when a caller has a
// known-map in hand (incremental compilation, single-block
// re-fold), they call this directly. The per-function variant
// just allocates a fresh map per block and calls this.
std::size_t constant_fold_block(aura::ir::BasicBlock& block, ConstantKnownMap& known) {
    std::size_t folded = 0;
    for (auto& instr : block.instructions) {
        auto& ops = instr.operands;
        switch (instr.opcode) {
            case aura::ir::IROpcode::ConstI64:
                known[ops[0]] = (static_cast<std::int64_t>(ops[2]) << 32) | ops[1];
                break;
            case aura::ir::IROpcode::ConstF64:
                break;
            case aura::ir::IROpcode::Local: {
                auto it = known.find(ops[1]);
                if (it != known.end()) {
                    // Don't propagate tagged bool values (7=#t, 3=#f) as
                    // ConstI64 — the AOT/JIT emitter treats ConstI64 as
                    // fixnum-encoded. Tagged bools are only safe in
                    // ConstBool form.
                    if (it->second != 3 && it->second != 7) {
                        replace(instr, ops[0], it->second, known);
                        ++folded;
                    }
                }
                break;
            }
            case aura::ir::IROpcode::Add: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end()) {
                    replace(instr, ops[0], it_a->second + it_b->second, known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Sub: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end()) {
                    replace(instr, ops[0], it_a->second - it_b->second, known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Mul: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end()) {
                    replace(instr, ops[0], it_a->second * it_b->second, known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Div: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end()) {
                    // Don't fold division by zero — the runtime needs
                    // to trap. Leave the IR as-is so the interpreter
                    // hits the actual Div opcode.
                    if (it_b->second != 0) {
                        replace(instr, ops[0], it_a->second / it_b->second, known);
                        ++folded;
                    }
                }
                break;
            }
            case aura::ir::IROpcode::Eq: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end()) {
                    replace_bool(instr, ops[0], it_a->second == it_b->second, known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Lt: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end()) {
                    replace_bool(instr, ops[0], it_a->second < it_b->second, known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Gt: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end()) {
                    replace_bool(instr, ops[0], it_a->second > it_b->second, known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Le: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end()) {
                    replace_bool(instr, ops[0], it_a->second <= it_b->second, known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Ge: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end()) {
                    replace_bool(instr, ops[0], it_a->second >= it_b->second, known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::And: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end()) {
                    replace_bool(instr, ops[0],
                                 is_truthy_val(it_a->second) && is_truthy_val(it_b->second), known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Or: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end()) {
                    replace_bool(instr, ops[0],
                                 is_truthy_val(it_a->second) || is_truthy_val(it_b->second), known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Not: {
                auto it = known.find(ops[1]);
                if (it != known.end()) {
                    replace_bool(instr, ops[0], !is_truthy_val(it->second), known);
                    ++folded;
                }
                break;
            }
            default:
                break;
        }
    }
    return folded;
}

// ── constant_fold_function — pure per-function folder ─────────
//
// Allocates a fresh known-map per block (so cross-block
// ConstI64 propagation doesn't leak — matches the legacy
// `fold_function` semantics, which clears known_ at each
// block entry).
ConstantFoldingResult constant_fold_function(aura::ir::IRFunction& func) {
    ConstantFoldingResult result;
    ConstantKnownMap known;
    for (auto& block : func.blocks) {
        known.clear();
        result.folded_count += constant_fold_block(block, known);
    }
    return result;
}

} // namespace aura::compiler
