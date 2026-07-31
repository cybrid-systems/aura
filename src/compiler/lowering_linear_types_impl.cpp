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
#include <string>
#include <string_view>
#include "compiler/ownership_escape_lowering_gate.h" // Issue #2263

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
            // (Linear e): wrap value in linear container (Owned=1).
            // Issue #2407: Linear of a Copy/literal is a value-as-Owned
            // no-op for AOT/emit-binary (interpreter already treats it
            // as wrap of an immediate). Elide LinearWrap so standalone
            // AOT does not require pin registry; result is the inner.
            auto inner = lower_inner(v.child(0));
            if (!v.children.empty()) {
                const auto cid = v.child(0);
                if (cid != aura::ast::NULL_NODE && cid < flat.size()) {
                    auto cv = flat.get(cid);
                    using NT = aura::ast::NodeTag;
                    if (cv.tag == NT::LiteralInt || cv.tag == NT::LiteralFloat ||
                        cv.tag == NT::LiteralString) {
                        // Copy literal → no ownership transfer surface.
                        return inner;
                    }
                }
            }
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
            //
            // Issue #2263: when OwnershipEscapeSummary is published
            // (post_mutation_invariant_check), consult the escape gate:
            //   - binding in escape-after-move / escape-while-borrowed
            //     → do NOT elide; emit MoveOp; bump blocked counter
            //   - clean owned binding under active summary → elide
            //   - no summary (null) → legacy always emit MoveOp
            // #1339 still forbids elision based on narrow_evidence alone.
            //
            // Issue #2407: move of Copy/literal is a no-op (like drop of
            // non-Owned). Elide MoveOp so AOT emit-binary does not emit
            // unpin_linear_root for imm values; IR executor also no-ops
            // non-linear Move when state permits.
            auto inner = lower_inner(v.child(0));
            std::string binding_name;
            bool copy_literal = false;
            if (!v.children.empty()) {
                const auto cid = v.child(0);
                if (cid != aura::ast::NULL_NODE && cid < flat.size()) {
                    auto cv = flat.get(cid);
                    using NT = aura::ast::NodeTag;
                    if (cv.tag == NT::LiteralInt || cv.tag == NT::LiteralFloat ||
                        cv.tag == NT::LiteralString) {
                        copy_literal = true;
                    } else if (cv.tag == aura::ast::NodeTag::Variable &&
                               cv.sym_id != aura::ast::INVALID_SYM) {
                        binding_name = std::string(pool.resolve(cv.sym_id));
                    }
                }
            }
            if (copy_literal) {
                g_linear_move_elided_total.fetch_add(1, std::memory_order_relaxed);
                ++state.linear_move_elided;
                return inner;
            }
            if (escape_move_elision_gate_active()) {
                g_linear_lowering_escape_summary_hit_total.fetch_add(1, std::memory_order_relaxed);
                // Issue #2286: keyed lookup reads thread-local current key
                // (set by Evaluator before lower_to_ir). Legacy
                // escape_blocks_move_elision used process-wide state → cross-eval
                // contamination (#2274 / #2275 lineage).
                if (!binding_name.empty() && escape_blocks_move_elision_for_current(binding_name)) {
                    g_linear_move_elision_blocked_escape_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                    // Fall through: emit MoveOp (escape-aware block).
                } else if (!binding_name.empty()) {
                    // Clean dirty binding under active summary → elide.
                    g_linear_move_elided_total.fetch_add(1, std::memory_order_relaxed);
                    ++state.linear_move_elided;
                    return inner;
                }
                // Non-variable under active gate: conservative emit.
            }
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
