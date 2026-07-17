// lowering_linear_types_impl.cpp — Implementation of
// try_lower_linear_type extracted from lowering_impl.cpp
// (Issue #133).
//
// Each of the 5 linear-type NodeTags (Linear, Move,
// Borrow, MutBorrow, Drop) follows the same pattern:
//   1. Recursively lower the inner expression
//   2. Allocate a local slot (except Drop, which has no
//      value to return)
//   3. Emit the corresponding IR opcode
//   4. Return the slot
//
// The recursive lowering is done via the `lower_inner`
// callback (typically pointing back to the main
// lower_flat_expr). This indirection lets the linear
// types module stay decoupled from the rest of
// lowering_impl.cpp.

module;

#include <atomic>
#include <cstdint>

module aura.compiler.lowering_linear_types;
import std;

import aura.core.ast;
import aura.compiler.ir;
import aura.compiler.lowering;

namespace aura::compiler {

// Issue #1339: process-wide MoveOp elision counter (lock-free).
std::atomic<std::uint64_t> g_linear_move_elided_total{0};
std::uint64_t linear_move_elided_total() noexcept {
    return g_linear_move_elided_total.load(std::memory_order_relaxed);
}

std::optional<std::uint32_t> try_lower_linear_type(LoweringState& state,
                                                   const aura::ast::FlatAST& flat,
                                                   const aura::ast::StringPool& pool,
                                                   aura::ast::NodeView v,
                                                   LinearLowerInner lower_inner) {
    switch (v.tag) {
        case aura::ast::NodeTag::Linear: {
            // (Linear e): wrap value in linear container (Owned=1)
            auto inner = lower_inner(v.child(0));
            auto slot = state.alloc_local();
            const auto narrow = state.current_narrowing_evidence;
            state.emit_with_metadata(aura::ir::IROpcode::LinearWrap, 0, 1, 0, narrow, slot, inner);
            return slot;
        }
        case aura::ast::NodeTag::Move: {
            // (move e): consume Owned source. Stamp linear_ownership_state
            // with the PRECONDITION (Owned=1), matching
            // ir_executor enforce_linear_ownership_state (Move requires
            // state==1). Stamping Moved=4 was wrong — that is the post
            // state after a successful move, and caused m4-linear-move
            // to always fail the state-machine gate.
            // #1339: do not elide MoveOp based on narrow_evidence (type
            // narrowing ≠ escape analysis). Keep MoveOp for ownership IR.
            auto inner = lower_inner(v.child(0));
            auto slot = state.alloc_local();
            const auto narrow = state.current_narrowing_evidence;
            state.emit_with_metadata(aura::ir::IROpcode::MoveOp, 0, 1, 0, narrow, slot, inner);
            return slot;
        }
        case aura::ast::NodeTag::Borrow: {
            // (& e): immutable borrow — precondition Owned or Borrowed.
            // Stamp Owned=1 (canonical source state); enforce also accepts 2.
            auto inner = lower_inner(v.child(0));
            auto slot = state.alloc_local();
            const auto narrow = state.current_narrowing_evidence;
            state.emit_with_metadata(aura::ir::IROpcode::BorrowOp, 0, 1, 0, narrow, slot, inner);
            return slot;
        }
        case aura::ast::NodeTag::MutBorrow: {
            // (&mut e): exclusive mut-borrow requires Owned=1 precondition.
            auto inner = lower_inner(v.child(0));
            auto slot = state.alloc_local();
            const auto narrow = state.current_narrowing_evidence;
            state.emit_with_metadata(aura::ir::IROpcode::MutBorrowOp, 0, 1, 0, narrow, slot, inner);
            return slot;
        }
        case aura::ast::NodeTag::Drop: {
            // (drop e): requires Owned=1 precondition (enforce Drop gate).
            auto inner = lower_inner(v.child(0));
            const auto narrow = state.current_narrowing_evidence;
            state.emit_with_metadata(aura::ir::IROpcode::DropOp, 0, 1, 0, narrow, inner, 0, 0);
            auto slot = state.alloc_local();
            state.emit(aura::ir::IROpcode::ConstVoid, slot);
            return slot;
        }
        default:
            return std::nullopt;
    }
}

} // namespace aura::compiler
