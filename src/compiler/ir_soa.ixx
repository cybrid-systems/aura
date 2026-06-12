// ──────────────────────────────────────────────────────────────
// IR SoA scaffold — Issue #167 Phase 1
// ──────────────────────────────────────────────────────────────
//
// Structure-of-Arrays parallel implementation of the IR layer.
// Coexists with the existing AoS IRModule (src/compiler/ir.ixx).
// Phase 1 (this file) ships the scaffold:
//   - IRFunctionSoA: per-function SoA columns
//   - IRInstructionView: non-owning view (analogous to NodeView)
//   - BasicBlockSoA: range-based block representation
//   - IRModuleV2: top-level container
//   - add_instruction / view_at / iterate-by-block API
//
// Phase 2+ (deferred to fresh session): port LoweringState,
// ir_executor, passes, JIT bridge to use IRModuleV2. See
// docs/design/ir_soa_migration.md for the full plan.
//
// This file is intentionally independent — no consumer imports
// it yet. The existing IRModule paths continue to work
// unchanged.

export module aura.compiler.ir_soa;

import std;
import aura.compiler.ir;  // for aura::ir::IROpcode (lives in aura::ir)

namespace aura::compiler {

// ── Forward declarations ──────────────────────────────────────
//
// BasicBlockSoA is used by IRFunctionSoA (as `blocks_` member)
// and IRFunctionSoA is used by IRInstructionView (as `func`
// pointer). We forward-declare BasicBlockSoA so IRFunctionSoA
// can have it as a member (the full definition appears below).
// Export at the forward decl so the name is visible to importers
// (the full definition is also exported below).
export struct BasicBlockSoA;

// ── IRFunctionSoA ─────────────────────────────────────────────
//
// One IRFunction's worth of instructions, stored as separate
// SoA columns. All columns are indexed by the same instruction
// index (0, 1, 2, ...). When a new instruction is added, every
// column grows by 1.
//
// Total column count today: 10 (opcode + 4 operands + 5 metadata
// fields). This is what makes the SoA layout work for
// interpreter + JIT traversal: a hot loop that only needs
// opcodes + operands touches 5 of these 10 columns, leaving
// the other 5 cold in cache. With the AoS struct, every
// instruction access pulls all 10 fields in.
//
// All columns are std::pmr::vector-friendly (could be migrated
// later). Today: std::vector for simplicity, defer arena
// migration to Phase 3.
export struct IRFunctionSoA {
    // Opcode stream (the most-frequently-touched column)
    std::vector<aura::ir::IROpcode> opcodes_;

    // 4 operand columns (parallel to opcodes_)
    std::vector<std::uint32_t> operand0_;
    std::vector<std::uint32_t> operand1_;
    std::vector<std::uint32_t> operand2_;
    std::vector<std::uint32_t> operand3_;

    // Metadata columns (parallel to opcodes_)
    std::vector<std::uint32_t> source_node_ids_;
    std::vector<std::uint32_t> type_ids_;
    std::vector<std::uint32_t> shape_ids_;
    std::vector<std::uint8_t>  linear_ownership_states_;
    std::vector<std::uint32_t> adt_variant_ids_;
    std::vector<std::uint32_t> narrow_evidence_;

    // Basic blocks: ranges into the SoA columns
    std::vector<BasicBlockSoA> blocks_;

    // Number of instructions currently stored
    std::size_t size() const { return opcodes_.size(); }

    // Reserve capacity for N instructions (one-shot allocation
    // hint for tight inner loops; consumers can call this
    // when they know an estimate)
    void reserve(std::size_t n) {
        opcodes_.reserve(n);
        operand0_.reserve(n);
        operand1_.reserve(n);
        operand2_.reserve(n);
        operand3_.reserve(n);
        source_node_ids_.reserve(n);
        type_ids_.reserve(n);
        shape_ids_.reserve(n);
        linear_ownership_states_.reserve(n);
        adt_variant_ids_.reserve(n);
        narrow_evidence_.reserve(n);
    }
};

// ── BasicBlockSoA ─────────────────────────────────────────────
//
// A block is a range of instruction indices in the function's
// SoA columns. The range is `[start_idx, end_idx)`. Successors
// are block indices (in the same function's blocks_ vector).
//
// We don't store instruction data here — only the range. The
// actual opcodes/operands are in the function's SoA columns
// (shared with the rest of the function's instructions).
//
// This range-based design enables:
//   - Easy block splits (just add a new block with a different
//     range; the function's SoA columns don't need to be
//     restructured).
//   - Easy block merges (delete one block; the range collapses
//     into the predecessor).
//   - Cache-friendly iteration: walking a block touches
//     contiguous SoA column indices.
export struct BasicBlockSoA {
    std::uint32_t block_id = 0;
    std::uint32_t start_idx = 0;   // index into IRFunctionSoA columns
    std::uint32_t end_idx = 0;     // exclusive
    std::vector<std::uint32_t> successors;  // block indices in same function
};

// ── IRInstructionView ─────────────────────────────────────────
//
// Non-owning view of a single instruction in an IRFunctionSoA.
// Holds a pointer to the function and the instruction index;
// accessors fetch the corresponding column. Mirrors the
// `NodeView` pattern in FlatAST.
//
// Cheap to copy (16 bytes: pointer + uint32). Enables passing
// by value in interpreter loops without copying the underlying
// instruction data.
export struct IRInstructionView {
    const IRFunctionSoA* func = nullptr;
    std::uint32_t idx = 0;

    constexpr IRInstructionView() = default;
    constexpr IRInstructionView(const IRFunctionSoA& f, std::uint32_t i)
        : func(&f), idx(i) {}

    // Accessors — all O(1), one SoA column access each.
    constexpr aura::ir::IROpcode opcode() const {
        return func->opcodes_[idx];
    }
    constexpr std::uint32_t operand(std::size_t i) const {
        switch (i) {
            case 0: return func->operand0_[idx];
            case 1: return func->operand1_[idx];
            case 2: return func->operand2_[idx];
            case 3: return func->operand3_[idx];
            default: return 0;
        }
    }
    constexpr std::uint32_t source_node_id() const {
        return func->source_node_ids_[idx];
    }
    constexpr std::uint32_t type_id() const {
        return func->type_ids_[idx];
    }
    constexpr std::uint32_t shape_id() const {
        return func->shape_ids_[idx];
    }
    constexpr std::uint8_t linear_ownership_state() const {
        return func->linear_ownership_states_[idx];
    }
    constexpr std::uint32_t adt_variant_id() const {
        return func->adt_variant_ids_[idx];
    }
    constexpr std::uint32_t narrow_evidence() const {
        return func->narrow_evidence_[idx];
    }

    // Convenience: structured operand access. Many call sites
    // want operands as a span. We allocate a tiny stack array
    // (4 elements) and return a span. O(1), no heap alloc.
    std::array<std::uint32_t, 4> operands() const {
        return {func->operand0_[idx], func->operand1_[idx],
                func->operand2_[idx], func->operand3_[idx]};
    }
};

// ── IRModuleV2 ────────────────────────────────────────────────
//
// Top-level container for SoA-style IR. Parallel to the existing
// `IRModule` (AoS). No consumer uses this yet — Phase 1 ships
// the infrastructure for Phase 2+ to migrate to.
export struct IRModuleV2 {
    std::string name;
    std::uint32_t entry_function_id = 0;
    std::vector<IRFunctionSoA> functions;
    std::vector<std::string> string_pool;  // string constants (for ConstString)

    // Add an instruction to a function. Appends one element to
    // each SoA column. The function index must be in range.
    // Returns the index of the newly added instruction.
    std::uint32_t add_instruction(
        std::size_t func_idx,
        aura::ir::IROpcode opcode,
        std::array<std::uint32_t, 4> operands = {},
        std::uint32_t source_node_id = 0,
        std::uint32_t type_id = 0,
        std::uint32_t shape_id = 0,
        std::uint8_t linear_state = 0,
        std::uint32_t adt_variant_id = 0,
        std::uint32_t narrow_evidence = 0)
    {
        auto& func = functions[func_idx];
        auto idx = static_cast<std::uint32_t>(func.size());
        func.opcodes_.push_back(opcode);
        func.operand0_.push_back(operands[0]);
        func.operand1_.push_back(operands[1]);
        func.operand2_.push_back(operands[2]);
        func.operand3_.push_back(operands[3]);
        func.source_node_ids_.push_back(source_node_id);
        func.type_ids_.push_back(type_id);
        func.shape_ids_.push_back(shape_id);
        func.linear_ownership_states_.push_back(linear_state);
        func.adt_variant_ids_.push_back(adt_variant_id);
        func.narrow_evidence_.push_back(narrow_evidence);
        return idx;
    }

    // Get a view of the i-th instruction in a function.
    // The view is non-owning; it stays valid as long as the
    // function is not modified.
    IRInstructionView view_at(std::size_t func_idx, std::uint32_t idx) const {
        return IRInstructionView(functions[func_idx], idx);
    }

    // Add a basic block to a function. Returns the block's index
    // in the function's blocks_ vector.
    std::uint32_t add_block(std::size_t func_idx) {
        auto& func = functions[func_idx];
        auto bid = static_cast<std::uint32_t>(func.blocks_.size());
        BasicBlockSoA block;
        block.block_id = bid;
        block.start_idx = static_cast<std::uint32_t>(func.size());
        block.end_idx = block.start_idx;
        func.blocks_.push_back(std::move(block));
        return bid;
    }

    // Seal a block (lock in its end_idx based on the current
    // function size). Subsequent add_instruction calls extend
    // the next block.
    void seal_block(std::size_t func_idx, std::uint32_t block_idx) {
        auto& func = functions[func_idx];
        func.blocks_[block_idx].end_idx =
            static_cast<std::uint32_t>(func.size());
    }
};

// Compile-time sanity: the view must be cheap to copy.
static_assert(sizeof(IRInstructionView) <= 16,
              "IRInstructionView should be a small POD (pointer + uint32)");

} // namespace aura::compiler
