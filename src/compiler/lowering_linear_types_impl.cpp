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
            // (Linear e): wrap value in linear container (Owned=1)
            auto inner = lower_inner(v.child(0));
            auto slot = state.alloc_local();
            state.emit_with_metadata(aura::ir::IROpcode::LinearWrap, 0, 1, 0, 0, slot, inner);
            return slot;
        }
        case aura::ast::NodeTag::Move: {
            // (move e): move ownership (Moved=4)
            auto inner = lower_inner(v.child(0));
            auto slot = state.alloc_local();
            state.emit_with_metadata(aura::ir::IROpcode::MoveOp, 0, 4, 0, 0, slot, inner);
            return slot;
        }
        case aura::ast::NodeTag::Borrow: {
            // (& e): immutable borrow (Borrowed=2)
            auto inner = lower_inner(v.child(0));
            auto slot = state.alloc_local();
            state.emit_with_metadata(aura::ir::IROpcode::BorrowOp, 0, 2, 0, 0, slot, inner);
            return slot;
        }
        case aura::ast::NodeTag::MutBorrow: {
            // (&mut e): mutable borrow (MutBorrowed=3)
            auto inner = lower_inner(v.child(0));
            auto slot = state.alloc_local();
            state.emit_with_metadata(aura::ir::IROpcode::MutBorrowOp, 0, 3, 0, 0, slot, inner);
            return slot;
        }
        case aura::ast::NodeTag::Drop: {
            // (drop e): explicit destruct (Moved=4 on drop site)
            auto inner = lower_inner(v.child(0));
            state.emit_with_metadata(aura::ir::IROpcode::DropOp, 0, 4, 0, 0, inner, 0, 0);
            auto slot = state.alloc_local();
            state.emit(aura::ir::IROpcode::ConstVoid, slot);
            return slot;
        }
        default:
            return std::nullopt;
    }
}

} // namespace aura::compiler
