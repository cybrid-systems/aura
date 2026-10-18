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

module;
#include <bit>
#include <limits>
#include "value_tags.h"

module aura.compiler.constant_folding;
import std;
import aura.compiler.value;

namespace aura::compiler {

using namespace aura::ir;

// ── Internal helpers (file-local) ─────────────────────────────

// Issue #1098: tagged bools in the known map MUST NOT collide with
// ordinary integers 3 and 7 (which are valid ConstI64 results).
// Encoding: high bit (1<<62) set marks a bool; low bit is the value.
//   #f → (1<<62)|0 ,  #t → (1<<62)|1
namespace {
    constexpr ConstantValue kKnownBoolTag = static_cast<ConstantValue>(1LL << 62);

    constexpr bool is_tagged_bool(ConstantValue v) noexcept {
        return (v & kKnownBoolTag) != 0;
    }
    constexpr bool tagged_bool_value(ConstantValue v) noexcept {
        return (v & 1) != 0;
    }
    constexpr ConstantValue make_tagged_bool(bool b) noexcept {
        return kKnownBoolTag | (b ? static_cast<ConstantValue>(1) : static_cast<ConstantValue>(0));
    }

    // IS_TRUTHY: tagged-bool-aware truthiness test.
    constexpr bool is_truthy_val(ConstantValue v) noexcept {
        if (is_tagged_bool(v))
            return tagged_bool_value(v);
        // Untagged entries are raw fixnum payloads (Issue #4491).
        return !::aura::compiler::types::kIntZeroIsFalse || v != 0;
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
// Instruction operands still use {slot, 0/1, 0, 0}; known map uses
// high-bit tagged bools (#1098) so ints 3/7 stay pure integers.
static void replace_bool(aura::ir::IRInstruction& instr, std::uint32_t slot, bool val,
                         ConstantKnownMap& known) {
    instr.opcode = aura::ir::IROpcode::ConstBool;
    instr.operands = {slot, val ? 1u : 0u, 0, 0};
    known[slot] = make_tagged_bool(val);
}

// Issue #4462: an out-of-domain integer folds to ConstF64 and leaves the
// known-int map, so a later fold does not treat the float as an int.
static void replace_f64(aura::ir::IRInstruction& instr, std::uint32_t slot, double val,
                        ConstantKnownMap& known) {
    const auto bits = std::bit_cast<std::uint64_t>(val);
    instr.opcode = aura::ir::IROpcode::ConstF64;
    instr.operands = {slot, static_cast<std::uint32_t>(bits & 0xFFFFFFFFu),
                      static_cast<std::uint32_t>(bits >> 32), 0};
    known.erase(slot);
}

static void fold_int_arith(aura::ir::IRInstruction& instr, std::uint32_t slot, std::int64_t a,
                           std::int64_t b, int kind, ConstantKnownMap& known) {
    std::int64_t next = 0;
    const bool ov = kind == 0   ? __builtin_add_overflow(a, b, &next)
                    : kind == 1 ? __builtin_sub_overflow(a, b, &next)
                                : __builtin_mul_overflow(a, b, &next);
    if (ov || !aura::compiler::types::fits_fixnum(next)) {
        // Issue #4482: overflow is a runtime error; leave it unfolded.
        return;
    } else {
        replace(instr, slot, next, known);
    }
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
            case aura::ir::IROpcode::ConstI64: {
                const auto val = (static_cast<std::int64_t>(ops[2]) << 32) | ops[1];
                // Issue #4462: do not record an integer the executor cannot box.
                if (!aura::compiler::types::fits_fixnum(val)) {
                    replace_f64(instr, ops[0], static_cast<double>(val), known);
                    ++folded;
                } else {
                    known[ops[0]] = val;
                }
                break;
            }
            case aura::ir::IROpcode::ConstBool:
                // Issue #1098: high-bit tag so ints 3/7 stay pure integers.
                known[ops[0]] = make_tagged_bool(ops[1] != 0);
                break;
            case aura::ir::IROpcode::ConstF64:
                break;
            case aura::ir::IROpcode::Local: {
                auto it = known.find(ops[1]);
                if (it != known.end()) {
                    // Don't propagate tagged bools as ConstI64 (#1098).
                    if (!is_tagged_bool(it->second)) {
                        replace(instr, ops[0], it->second, known);
                        ++folded;
                    }
                }
                break;
            }
            case aura::ir::IROpcode::Add: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                // Issue #1098: never arithmetically fold tagged bools.
                if (it_a != known.end() && it_b != known.end() && !is_tagged_bool(it_a->second) &&
                    !is_tagged_bool(it_b->second)) {
                    fold_int_arith(instr, ops[0], it_a->second, it_b->second, 0, known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Sub: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end() && !is_tagged_bool(it_a->second) &&
                    !is_tagged_bool(it_b->second)) {
                    fold_int_arith(instr, ops[0], it_a->second, it_b->second, 1, known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Mul: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end() && !is_tagged_bool(it_a->second) &&
                    !is_tagged_bool(it_b->second)) {
                    fold_int_arith(instr, ops[0], it_a->second, it_b->second, 2, known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Div: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end() && !is_tagged_bool(it_a->second) &&
                    !is_tagged_bool(it_b->second)) {
                    // Don't fold division by zero — the runtime needs
                    // to trap. Leave the IR as-is so the interpreter
                    // hits the actual Div opcode. INT64_MIN/-1 stays on
                    // that path too (pre-existing).
                    const auto a = it_a->second;
                    const auto b = it_b->second;
                    const bool min_div = a == std::numeric_limits<std::int64_t>::min() &&
                                         b == static_cast<std::int64_t>(-1);
                    if (b != 0 && !min_div) {
                        const auto q = a / b;
                        if (aura::compiler::types::fits_fixnum(q))
                            replace(instr, ops[0], q, known);
                        else
                            replace_f64(instr, ops[0],
                                        static_cast<double>(a) / static_cast<double>(b), known);
                        ++folded;
                    }
                }
                break;
            }
            case aura::ir::IROpcode::Eq: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                // Issue #4463: numeric `=` (ops[3] == 1) does not fold a bool.
                // eq?/eqv? (ops[3] == 0) still fold identity of tagged bools.
                const bool numeric = ops[3] != 0;
                if (it_a != known.end() && it_b != known.end() &&
                    !(numeric && (is_tagged_bool(it_a->second) || is_tagged_bool(it_b->second)))) {
                    replace_bool(instr, ops[0], it_a->second == it_b->second, known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Lt: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end() && !is_tagged_bool(it_a->second) &&
                    !is_tagged_bool(it_b->second)) {
                    replace_bool(instr, ops[0], it_a->second < it_b->second, known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Gt: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end() && !is_tagged_bool(it_a->second) &&
                    !is_tagged_bool(it_b->second)) {
                    replace_bool(instr, ops[0], it_a->second > it_b->second, known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Le: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end() && !is_tagged_bool(it_a->second) &&
                    !is_tagged_bool(it_b->second)) {
                    replace_bool(instr, ops[0], it_a->second <= it_b->second, known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Ge: {
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end() && !is_tagged_bool(it_a->second) &&
                    !is_tagged_bool(it_b->second)) {
                    replace_bool(instr, ops[0], it_a->second >= it_b->second, known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::And: {
                // Issue #4491: and/or yield an operand, not #t/#f. Fold only
                // when both operands are tagged bools (result stays a bool).
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end() && is_tagged_bool(it_a->second) &&
                    is_tagged_bool(it_b->second)) {
                    replace_bool(instr, ops[0],
                                 tagged_bool_value(is_truthy_val(it_a->second) ? it_b->second
                                                                               : it_a->second),
                                 known);
                    ++folded;
                }
                break;
            }
            case aura::ir::IROpcode::Or: {
                // Issue #4491: and/or yield an operand, not #t/#f. Fold only
                // when both operands are tagged bools (result stays a bool).
                auto it_a = known.find(ops[1]), it_b = known.find(ops[2]);
                if (it_a != known.end() && it_b != known.end() && is_tagged_bool(it_a->second) &&
                    is_tagged_bool(it_b->second)) {
                    replace_bool(instr, ops[0],
                                 tagged_bool_value(is_truthy_val(it_a->second) ? it_a->second
                                                                               : it_b->second),
                                 known);
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
// A block with anything other than one already-folded forward
// predecessor starts from an empty known-map, so a constant does
// not leak across a merge or a back edge.
//
// Issue #4592: #4588's error escape ends the inner operator's
// block with Branch and emits the consumer in the continuation.
// That continuation has one predecessor. Seeding its map from
// that predecessor's exit map lets `(+ 1 (* 2 3))` fold both the
// inner Mul and the outer Add. Slots the continuation itself
// defines are dropped first, so a redefinition is not folded as
// the predecessor's value.
namespace {
    bool instruction_defines_slot(const aura::ir::IRInstruction& instr, std::uint32_t& slot) {
        switch (instr.opcode) {
            case IROpcode::Call:
            case IROpcode::PrimCall:
                slot = instr.operands[3];
                return true;
            case IROpcode::Apply:
                slot = instr.operands[2];
                return true;
            default:
                break;
        }
        if (const auto* info = lookup_opcode(instr.opcode); info && info->has_result_slot) {
            slot = instr.operands[0];
            return true;
        }
        return false;
    }
} // namespace

ConstantFoldingResult constant_fold_function(aura::ir::IRFunction& func) {
    ConstantFoldingResult result;
    const auto n = func.blocks.size();
    std::vector<int> pred_count(n, 0);
    std::vector<int> single_pred(n, -1);
    auto note_succ = [&](std::size_t from, std::uint32_t to) {
        if (to >= n)
            return;
        if (pred_count[to] == 0 && to != from)
            single_pred[to] = static_cast<int>(from);
        else
            single_pred[to] = -1;
        pred_count[to] += 1;
    };
    for (std::size_t bi = 0; bi < n; ++bi) {
        const auto& ins = func.blocks[bi].instructions;
        if (ins.empty())
            continue;
        const auto& term = ins.back();
        if (term.opcode == IROpcode::Branch) {
            note_succ(bi, term.operands[1]);
            note_succ(bi, term.operands[2]);
        } else if (term.opcode == IROpcode::Jump) {
            note_succ(bi, term.operands[0]);
        }
    }

    std::vector<ConstantKnownMap> at_exit(n);
    for (std::size_t bi = 0; bi < n; ++bi) {
        ConstantKnownMap known;
        if (pred_count[bi] == 1) {
            const int pred = single_pred[bi];
            if (pred >= 0 && static_cast<std::size_t>(pred) < bi)
                known = at_exit[static_cast<std::size_t>(pred)];
        }
        for (const auto& instr : func.blocks[bi].instructions) {
            std::uint32_t slot = 0;
            if (instruction_defines_slot(instr, slot))
                known.erase(slot);
        }
        result.folded_count += constant_fold_block(func.blocks[bi], known);
        at_exit[bi] = std::move(known);
    }
    return result;
}

} // namespace aura::compiler
