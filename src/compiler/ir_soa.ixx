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
// Issue #254 / ir_soa_migration (archived: git tag docs-archive-pre-2026-06).
//
// This file is intentionally independent — no consumer imports
// it yet. The existing IRModule paths continue to work
// unchanged.

module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

export module aura.compiler.ir_soa;

import std;
import aura.compiler.ir; // for aura::ir::IROpcode (lives in aura::ir)

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
    // Issue #463: function identity. Mirrors the AoS
    // IRFunction's name + local_count fields. Needed for
    // SoA→AoS view conversion in the SoAtoAoSBridgePass.
    std::string name;
    std::uint32_t local_count = 0;

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
    std::vector<std::uint8_t> linear_ownership_states_;
    std::vector<std::uint32_t> adt_variant_ids_;
    std::vector<std::uint32_t> narrow_evidence_;

    // Basic blocks: ranges into the SoA columns
    std::vector<BasicBlockSoA> blocks_;

    // Issue #196: per-block dirty bitmask. One bit per block;
    // 0 = clean (cached lowered IR is valid), 1 = dirty
    // (block has been modified since the last lower, needs
    // re-lower). Bumped by mark_define_dirty (full invalidate)
    // and the smarter-re-lower (incremental, per-block).
    // The "smarter re-lower" (follow-up) consults this mask to
    // skip blocks whose cached IR is still valid.
    std::pmr::vector<std::uint8_t> block_dirty_;

    // Issue #380: per-instruction dirty bitmask. Parallel to
    // opcodes_ (one byte per instruction, indexed by the
    // instruction index). 0 = clean, 1 = dirty.
    //
    // Mirrors the FlatAST per-node dirty column (issue #240):
    //   - Both use 1 byte per element
    //   - Both track "is this element's cached derivation
    //     still valid after a mutation?"
    //   - Both expose mark / clear / query / count
    //
    // When mark_block_dirty(block_id) is called, the cascade
    // also marks every instruction in the block dirty (the
    // whole block's cached IR needs re-lower). The smarter
    // re-lower (follow-up) will consult is_instruction_dirty
    // to skip re-emitting clean instructions within an
    // otherwise-dirty block.
    std::pmr::vector<std::uint8_t> instruction_dirty_;

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

    // Issue #196: per-block dirty tracking. The bitmask is
    // 1 byte per block (8 blocks packed per byte in the
    // future if needed; for now 1 byte/block is fine).
    // The "smarter re-lower" follow-up will consult
    // is_block_dirty() to skip re-lowering clean blocks.

    // Mark a single block dirty. Resizes the bitmask if
    // needed (lazy resize on first call). Cascades to mark
    // every instruction in the block dirty (Issue #380).
    // Body defined outside the class (after BasicBlockSoA is
    // complete) because the cascade needs to read
    // `block.start_idx` / `block.end_idx`.
    void mark_block_dirty(std::uint32_t block_id);

    // Mark all blocks dirty (used by mark_define_dirty for
    // a full invalidate). Cheaper than a loop of individual
    // mark_block_dirty() calls. Also marks all instructions
    // dirty (cascade).
    void mark_all_blocks_dirty() {
        for (auto& b : block_dirty_)
            b = 1;
        // Issue #380: cascade the all-blocks invalidate to all
        // instructions too. Same byte-per-element representation
        // as the block mask; both go to 0xFF... no wait, 0x01.
        for (auto& i : instruction_dirty_)
            i = 1;
    }

    // Clear a single block's dirty flag (called by the
    // smarter-re-lower after re-lowering the block).
    // Does NOT cascade to instructions — clearing a block
    // is the "I just re-lowered this block, you can re-use
    // it" signal, but the per-instruction dirty bits are
    // independent (the smarter re-lower can clear individual
    // instructions as it re-emits them).
    void clear_block_dirty(std::uint32_t block_id) {
        if (block_id < block_dirty_.size()) {
            block_dirty_[block_id] = 0;
        }
    }

    // Query: is this block dirty? Returns 0 (clean) for
    // out-of-range block_id (treat unknown as clean).
    bool is_block_dirty(std::uint32_t block_id) const {
        if (block_id >= block_dirty_.size())
            return false;
        return block_dirty_[block_id] != 0;
    }

    // Query: number of currently-dirty blocks. Used by the
    // observability primitive.
    std::size_t dirty_block_count() const {
        std::size_t n = 0;
        for (auto b : block_dirty_)
            if (b)
                ++n;
        return n;
    }

    // Issue #196: public read-only view of the per-block dirty
    // bitmask for the observability layer.
    [[nodiscard]] const std::pmr::vector<std::uint8_t>& block_dirty_column() const noexcept {
        return block_dirty_;
    }

    // Issue #380: per-instruction dirty tracking. Mirrors the
    // per-block API above but at instruction granularity.
    // Same byte-per-element representation, same semantics.

    // Mark a single instruction dirty. Lazy resize on first
    // call (matches per-block behavior).
    void mark_instruction_dirty(std::uint32_t idx) {
        if (idx >= instruction_dirty_.size()) {
            instruction_dirty_.resize(idx + 1, 1);
            return;
        }
        instruction_dirty_[idx] = 1;
    }

    // Mark all instructions dirty (full invalidate of
    // per-instruction state). Used when a single mutation
    // invalidates the entire function's instruction cache.
    void mark_all_instructions_dirty() {
        for (auto& i : instruction_dirty_)
            i = 1;
    }

    // Clear a single instruction's dirty flag (called by
    // the smarter-re-lower after re-emitting that instruction).
    void clear_instruction_dirty(std::uint32_t idx) {
        if (idx < instruction_dirty_.size()) {
            instruction_dirty_[idx] = 0;
        }
    }

    // Query: is this instruction dirty? Returns 0 (clean) for
    // out-of-range idx (treat unknown as clean — consistent
    // with the per-block behavior).
    bool is_instruction_dirty(std::uint32_t idx) const {
        if (idx >= instruction_dirty_.size())
            return false;
        return instruction_dirty_[idx] != 0;
    }

    // Query: number of currently-dirty instructions. Used
    // by the observability primitive to surface "how much
    // of this function needs re-lower" without walking the
    // full column.
    std::size_t dirty_instruction_count() const {
        std::size_t n = 0;
        for (auto i : instruction_dirty_)
            if (i)
                ++n;
        return n;
    }

    // Issue #380: public read-only view of the per-instruction
    // dirty bitmask for the observability layer.
    [[nodiscard]] const std::pmr::vector<std::uint8_t>&
    instruction_dirty_column() const noexcept {
        return instruction_dirty_;
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
    std::uint32_t start_idx = 0;           // index into IRFunctionSoA columns
    std::uint32_t end_idx = 0;             // exclusive
    std::vector<std::uint32_t> successors; // block indices in same function
};

// Issue #380: define IRFunctionSoA::mark_block_dirty here (after
// BasicBlockSoA is complete) so the cascade can read
// `block.start_idx` and `block.end_idx`. Marked `inline` so
// downstream importers get the same definition without ODR
// violations.
inline void IRFunctionSoA::mark_block_dirty(std::uint32_t block_id) {
    if (block_id >= block_dirty_.size()) {
        block_dirty_.resize(block_id + 1, 1);
    } else {
        block_dirty_[block_id] = 1;
    }
    // Issue #380: cascade to all instructions in the block.
    // If the block doesn't exist yet (block_id out of
    // range for blocks_), the loop is a no-op — the block
    // bit itself is now set, which is the safe default.
    if (block_id < blocks_.size()) {
        const auto& block = blocks_[block_id];
        for (std::uint32_t i = block.start_idx; i < block.end_idx; ++i) {
            if (i >= instruction_dirty_.size()) {
                instruction_dirty_.resize(i + 1, 1);
            } else {
                instruction_dirty_[i] = 1;
            }
        }
    }
}

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
        : func(&f)
        , idx(i) {}

    // Accessors — all O(1), one SoA column access each.
    constexpr aura::ir::IROpcode opcode() const { return func->opcodes_[idx]; }
    constexpr std::uint32_t operand(std::size_t i) const {
        switch (i) {
            case 0:
                return func->operand0_[idx];
            case 1:
                return func->operand1_[idx];
            case 2:
                return func->operand2_[idx];
            case 3:
                return func->operand3_[idx];
            default:
                return 0;
        }
    }
    constexpr std::uint32_t source_node_id() const { return func->source_node_ids_[idx]; }
    constexpr std::uint32_t type_id() const { return func->type_ids_[idx]; }
    constexpr std::uint32_t shape_id() const { return func->shape_ids_[idx]; }
    constexpr std::uint8_t linear_ownership_state() const {
        return func->linear_ownership_states_[idx];
    }
    constexpr std::uint32_t adt_variant_id() const { return func->adt_variant_ids_[idx]; }
    constexpr std::uint32_t narrow_evidence() const { return func->narrow_evidence_[idx]; }

    // Convenience: structured operand access. Many call sites
    // want operands as a span. We allocate a tiny stack array
    // (4 elements) and return a span. O(1), no heap alloc.
    std::array<std::uint32_t, 4> operands() const {
        return {func->operand0_[idx], func->operand1_[idx], func->operand2_[idx],
                func->operand3_[idx]};
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
    std::vector<std::string> string_pool; // string constants (for ConstString)

    // Add an instruction to a function. Appends one element to
    // each SoA column. The function index must be in range.
    // Returns the index of the newly added instruction.
    // Issue #380: the instruction_dirty_ column gets a 0 byte
    // appended (new instructions are clean — they were just
    // added, no cached derivation to invalidate).
    std::uint32_t add_instruction(std::size_t func_idx, aura::ir::IROpcode opcode,
                                  std::array<std::uint32_t, 4> operands = {},
                                  std::uint32_t source_node_id = 0, std::uint32_t type_id = 0,
                                  std::uint32_t shape_id = 0, std::uint8_t linear_state = 0,
                                  std::uint32_t adt_variant_id = 0,
                                  std::uint32_t narrow_evidence = 0) {
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
        // Issue #380: new instruction starts clean. The mark_*_dirty
        // call later will flip this to 1 if the cached IR for this
        // instruction needs re-derivation.
        func.instruction_dirty_.push_back(0);
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
        func.blocks_[block_idx].end_idx = static_cast<std::uint32_t>(func.size());
    }

    // Issue #463: add a function to the module. Returns the
    // index of the newly added function. Mirrors the pattern
    // used by the existing AoS IRModule::add_function.
    std::size_t add_function(std::string name = "",
                              std::uint32_t local_count = 0) {
        IRFunctionSoA func;
        func.name = std::move(name);
        func.local_count = local_count;
        functions.push_back(std::move(func));
        return functions.size() - 1;
    }
};

// Compile-time sanity: the view must be cheap to copy.
static_assert(sizeof(IRInstructionView) <= 16,
              "IRInstructionView should be a small POD (pointer + uint32)");

// ── Issue #463: SoA → AoS view conversion (Phase 2 wiring) ───────
//
// Converts an IRFunctionSoA into an AoS IRFunction so the
// existing Pass pipeline (which operates on IRModule) can
// run on a SoA-built function. The conversion is O(n) in
// the number of instructions — one pass over the SoA
// columns to build a vector of IRInstructions, then a
// second pass to lay out the basic blocks.
//
// This is a TEST SEAM (production code should migrate the
// Pass pipeline to consume IRInstructionView directly). The
// SoAtoAoSBridgePass below uses this conversion to let the
// existing Passes run on SoA-built functions while the
// per-Pass SoA-aware overloads ship in subsequent cycles.
export inline aura::ir::IRFunction to_aos_view(const IRFunctionSoA& soa) {
    aura::ir::IRFunction f;
    f.name = soa.name;
    f.local_count = soa.local_count;
    f.blocks.clear();
    f.blocks.reserve(soa.blocks_.size());
    for (const auto& block : soa.blocks_) {
        aura::ir::BasicBlock b;
        b.id = block.block_id;
        b.instructions.reserve(block.end_idx - block.start_idx);
        for (std::uint32_t i = block.start_idx; i < block.end_idx; ++i) {
            aura::ir::IRInstruction instr;
            instr.opcode = soa.opcodes_[i];
            instr.operands = {soa.operand0_[i], soa.operand1_[i],
                              soa.operand2_[i], soa.operand3_[i]};
            instr.source_ast_node_id = soa.source_node_ids_[i];
            instr.type_id = soa.type_ids_[i];
            instr.shape_id = soa.shape_ids_[i];
            instr.linear_ownership_state = soa.linear_ownership_states_[i];
            instr.adt_variant_id = soa.adt_variant_ids_[i];
            instr.narrow_evidence = soa.narrow_evidence_[i];
            b.instructions.push_back(std::move(instr));
        }
        f.blocks.push_back(std::move(b));
    }
    return f;
}

} // namespace aura::compiler
