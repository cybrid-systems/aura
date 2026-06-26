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

module aura.compiler.lowering_linear_types;
import std;

import aura.core.ast;
import aura.compiler.ir;
import aura.compiler.lowering;

namespace aura::compiler {

std::optional<std::uint32_t> try_lower_linear_type(LoweringState& state,
                                                   const aura::ast::FlatAST& flat,
                                                   const aura::ast::StringPool& pool,
                                                   aura::ast::NodeView v,
                                                   LinearLowerInner lower_inner) {
    switch (v.tag) {
        case aura::ast::NodeTag::Linear: {
            // (Linear e): wrap value in linear container
            auto inner = lower_inner(v.child(0));
            auto slot = state.alloc_local();
            state.emit(aura::ir::IROpcode::LinearWrap, slot, inner);
            return slot;
        }
        case aura::ast::NodeTag::Move: {
            // (move e): move ownership
            auto inner = lower_inner(v.child(0));
            auto slot = state.alloc_local();
            state.emit(aura::ir::IROpcode::MoveOp, slot, inner);
            return slot;
        }
        case aura::ast::NodeTag::Borrow: {
            // (& e): immutable borrow
            auto inner = lower_inner(v.child(0));
            auto slot = state.alloc_local();
            state.emit(aura::ir::IROpcode::BorrowOp, slot, inner);
            return slot;
        }
        case aura::ast::NodeTag::MutBorrow: {
            // (&mut e): mutable borrow
            auto inner = lower_inner(v.child(0));
            auto slot = state.alloc_local();
            state.emit(aura::ir::IROpcode::MutBorrowOp, slot, inner);
            return slot;
        }
        case aura::ast::NodeTag::Drop: {
            // (drop e): explicit destruct
            auto inner = lower_inner(v.child(0));
            state.emit(aura::ir::IROpcode::DropOp, inner, 0, 0);
            auto slot = state.alloc_local();
            state.emit(aura::ir::IROpcode::ConstVoid, slot);
            return slot;
        }
        default:
            return std::nullopt;
    }
}

} // namespace aura::compiler
