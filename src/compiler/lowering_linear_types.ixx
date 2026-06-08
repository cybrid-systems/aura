// lowering_linear_types.ixx — Linear type lowering
// extracted from lowering_impl.cpp (Issue #133).
//
// Handles the 5 linear-type NodeTags: Linear, Move,
// Borrow, MutBorrow, Drop. Each follows the same pattern:
// recursively lower the inner expression, then emit the
// appropriate linear-type IR opcode.
//
// The function takes a `lower_inner` callback (typically
// pointing back to the main `lower_flat_expr`) so it can
// recurse into the inner expression without depending on
// the static helper directly.

module;

#include <cstdint>
#include <functional>
#include <optional>

export module aura.compiler.lowering_linear_types;

import aura.core.ast;
import aura.compiler.ir;
import aura.compiler.lowering;

export namespace aura::compiler {

// Callback for recursing into the inner expression of a
// linear-type node. The main lower_flat_expr is the
// natural implementation; this indirection lets
// lowering_linear_types.ixx stay decoupled from the
// rest of lowering_impl.cpp.
using LinearLowerInner = std::function<std::uint32_t(aura::ast::NodeId)>;

// Try to lower a linear-type node. Returns the result
// slot on success, or std::nullopt if the NodeView's
// tag is not a linear type (so the caller can fall
// through to the default case).
//
// The 5 linear types and their IR opcodes:
//   Linear     -> LinearWrap
//   Move       -> MoveOp
//   Borrow     -> BorrowOp
//   MutBorrow  -> MutBorrowOp
//   Drop       -> DropOp + ConstVoid
std::optional<std::uint32_t> try_lower_linear_type(
    LoweringState& state,
    const aura::ast::FlatAST& flat,
    const aura::ast::StringPool& pool,
    aura::ast::NodeView v,
    LinearLowerInner lower_inner);

} // namespace aura::compiler
