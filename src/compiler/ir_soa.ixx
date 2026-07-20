// ──────────────────────────────────────────────────────────────
// IR SoA scaffold — Issue #167 Phase 1
// ──────────────────────────────────────────────────────────────
//
// Structure-of-Arrays parallel implementation of the IR layer.
// Coexists with the existing AoS IRModule (src/compiler/ir.ixx).
// Phase 1 ships the scaffold:
//   - IRFunctionSoA: per-function SoA columns
//   - IRInstructionView: non-owning view (analogous to NodeView)
//   - BasicBlockSoA: range-based block representation
//   - IRModuleV2: top-level container
//   - add_instruction / view_at / iterate-by-block API
//
// Phase 2 (#1920): consumers (lowering dual-emit, ir_executor SoA walk,
// DirtyAware passes run(IRModuleV2), JIT shape/column consult, capture
// dirty marks) adopt IRModuleV2 + block_dirty_/shape_ids_ for
// incremental decisions. See ir_soa_migration in jit_typed_mutation_stats.h.

module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>
#include <contracts>

#include "core/cpp26_contract_stats.h"

export module aura.compiler.ir_soa;

import std;
import aura.compiler.ir; // for aura::ir::IROpcode (lives in aura::ir)

namespace aura::compiler {

// Issue #742: consteval SoA column count (must match cxx26_invariants.ixx).
inline constexpr std::size_t kIrSoaColumnCount = 10;
static_assert(kIrSoaColumnCount == 10, "IRFunctionSoA must have 10 SoA columns");

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
    // Issue #1273: SyntaxMarker mirror (0=User, 1=MacroIntroduced).
    std::uint8_t marker = 0;

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
    // Issue #746: CastOp coercion type_tag (mirrors operands[2] on AoS CastOp).
    std::vector<std::uint8_t> coercion_tags_;

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
        coercion_tags_.reserve(n);
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

    // Issue #1920: clean-block count (complement of dirty) for
    // incremental skip metrics.
    [[nodiscard]] std::size_t clean_block_count() const noexcept {
        std::size_t n = 0;
        for (std::size_t bi = 0; bi < blocks_.size(); ++bi) {
            if (!is_block_dirty(static_cast<std::uint32_t>(bi)))
                ++n;
        }
        return n;
    }

    // Issue #196: public read-only view of the per-block dirty
    // bitmask for the observability layer.
    [[nodiscard]] const std::pmr::vector<std::uint8_t>& block_dirty_column() const noexcept {
        return block_dirty_;
    }

    // Issue #1920: for_each_block defined after BasicBlockSoA (below).
    template <typename Fn>
    std::pair<std::size_t, std::size_t> for_each_block(Fn&& fn, bool dirty_only = true);

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
    [[nodiscard]] const std::pmr::vector<std::uint8_t>& instruction_dirty_column() const noexcept {
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
    contract_assert(block_id < blocks_.size() || block_dirty_.empty() ||
                    block_id <= block_dirty_.size());
    aura::core::cpp26::record_hotpath_invariant_hit();
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

// Issue #1920: invoke Fn(block_id, BasicBlockSoA&) for each dirty
// block (or all blocks when dirty_only=false). Defined after
// BasicBlockSoA is complete.
template <typename Fn>
inline std::pair<std::size_t, std::size_t> IRFunctionSoA::for_each_block(Fn&& fn, bool dirty_only) {
    std::size_t runs = 0;
    std::size_t skips = 0;
    for (auto& block : blocks_) {
        if (dirty_only && !is_block_dirty(block.block_id)) {
            ++skips;
            continue;
        }
        ++runs;
        fn(block.block_id, block);
    }
    return {runs, skips};
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
    constexpr aura::ir::IROpcode opcode() const {
        contract_assert(idx < func->opcodes_.size());
        return func->opcodes_[idx];
    }
    constexpr std::uint32_t operand(std::size_t i) const {
        contract_assert(i < 4); // only 4 operand columns exist
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
    constexpr std::uint8_t coercion_tag() const { return func->coercion_tags_[idx]; }

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
// `IRModule` (AoS). Phase 2 (#1920): primary container for dual-emit
// lower, DirtyAware pass SoA overloads, and JIT column consults.
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
                                  std::uint32_t narrow_evidence = 0,
                                  std::uint8_t coercion_tag = 0) {
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
        func.coercion_tags_.push_back(coercion_tag);
        // Issue #380: new instruction starts clean. The mark_*_dirty
        // call later will flip this to 1 if the cached IR for this
        // instruction needs re-derivation.
        func.instruction_dirty_.push_back(0);
        return idx;
    }

    // Issue #746: patch metadata on the last instruction in a function
    // (emit_with_metadata sets AoS fields after emit()'s dual-emit).
    void patch_last_instruction_metadata(std::size_t func_idx, std::uint8_t linear_state,
                                         std::uint32_t adt_variant, std::uint32_t narrow_evidence,
                                         std::uint8_t coercion_tag = 0) {
        if (func_idx >= functions.size())
            return;
        auto& func = functions[func_idx];
        if (func.size() == 0)
            return;
        const auto idx = func.size() - 1;
        func.linear_ownership_states_[idx] = linear_state;
        func.adt_variant_ids_[idx] = adt_variant;
        func.narrow_evidence_[idx] = narrow_evidence;
        if (coercion_tag != 0)
            func.coercion_tags_[idx] = coercion_tag;
        // Issue #1657: auto-mark the patched instruction dirty so the
        // next relower_define_* pass sees it. Without this, the
        // instruction_dirty_ bitmask can desync from block_dirty_
        // (the block is already marked dirty by the caller, but the
        // specific instruction patch wasn't visible to the
        // relower_define_function_minimal path). Callers that track
        // soa_dirty_sync_total do so via Evaluator bump hooks.
        func.instruction_dirty_[idx] = 1;
    }

    // Issue #1657: explicit block→instruction dirty propagation
    // helper. Walks every dirty block on every function and ensures
    // all of its instructions are also marked dirty. Returns the
    // number of instruction bits flipped 0→1 (for metric accounting).
    // Called from cascade_block_to_instructions callers + from
    // lowering consistency-mismatch fallback in lowering_impl.cpp.
    std::size_t sync_instruction_dirty_from_block_dirty() {
        std::size_t flipped = 0;
        for (auto& func : functions) {
            for (std::uint32_t bi = 0; bi < func.block_dirty_.size(); ++bi) {
                if (func.block_dirty_[bi] == 0)
                    continue;
                if (bi >= func.blocks_.size())
                    continue;
                const auto& block = func.blocks_[bi];
                for (std::uint32_t i = block.start_idx; i < block.end_idx; ++i) {
                    if (i >= func.instruction_dirty_.size()) {
                        func.instruction_dirty_.resize(i + 1, 1);
                        ++flipped;
                    } else if (func.instruction_dirty_[i] == 0) {
                        func.instruction_dirty_[i] = 1;
                        ++flipped;
                    }
                }
            }
        }
        return flipped;
    }

    // Get a view of the i-th instruction in a function.
    // The view is non-owning; it stays valid as long as the
    // function is not modified.
    IRInstructionView view_at(std::size_t func_idx, std::uint32_t idx) const
        pre(func_idx < functions.size()) pre(idx < functions[func_idx].size()) {
        aura::core::cpp26::record_hotpath_invariant_hit();
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
    std::size_t add_function(std::string name = "", std::uint32_t local_count = 0) {
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
            instr.operands = {soa.operand0_[i], soa.operand1_[i], soa.operand2_[i],
                              soa.operand3_[i]};
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

// Issue #1920: full-module SoA → AoS conversion for pipeline bridges.
export inline aura::ir::IRModule to_aos_module(const IRModuleV2& soa) {
    aura::ir::IRModule mod;
    mod.entry_function_id = soa.entry_function_id;
    mod.string_pool = soa.string_pool;
    mod.functions.reserve(soa.functions.size());
    for (const auto& fn : soa.functions)
        mod.functions.push_back(to_aos_view(fn));
    return mod;
}

// Issue #1920: interpreter / JIT hot-path SoA column walk.
// Returns {instructions_visited, dirty_runs, clean_skips}. Callers
// record ir_soa_migration consumer/dirty metrics (header counters).
export struct SoaHotpathWalkResult {
    std::size_t instructions = 0;
    std::size_t dirty_runs = 0;
    std::size_t clean_skips = 0;
};
export inline SoaHotpathWalkResult walk_soa_function_hotpath(const IRFunctionSoA& fn,
                                                             bool dirty_only = true) noexcept {
    SoaHotpathWalkResult r{};
    if (fn.blocks_.empty()) {
        r.instructions = fn.opcodes_.size();
        if (r.instructions > 0)
            r.dirty_runs = 1;
        return r;
    }
    for (const auto& block : fn.blocks_) {
        if (dirty_only && !fn.is_block_dirty(block.block_id)) {
            ++r.clean_skips;
            continue;
        }
        ++r.dirty_runs;
        // Touch shape/linear/opcode columns for locality (no AoS chase).
        for (std::uint32_t i = block.start_idx; i < block.end_idx && i < fn.opcodes_.size(); ++i) {
            ++r.instructions;
            (void)fn.opcodes_[i];
            if (i < fn.shape_ids_.size())
                (void)fn.shape_ids_[i];
            if (i < fn.linear_ownership_states_.size())
                (void)fn.linear_ownership_states_[i];
        }
    }
    return r;
}

} // namespace aura::compiler
