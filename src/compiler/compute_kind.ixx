module;
#include <cstdint>
#include <span>
#include <vector>

export module aura.compiler.compute_kind;
import std;
import aura.compiler.ir;

namespace aura::compiler {

// ComputeKind: compile-time evaluability analysis
export enum class ComputeKind : std::uint8_t {
    Unknown = 0,
    Known = 1,
};

// Per-instruction compute kind
export struct ComputeKindInfo {
    ComputeKind kind = ComputeKind::Unknown;
};

// Analysis result: per-block, per-instruction
export struct ComputeKindResult {
    std::vector<std::vector<ComputeKind>> per_block_inst_kind;
    bool valid = false;
};

// Pure function: analyze which instructions produce Known vs Unknown values
export ComputeKindResult compute_kind(const aura::ir::IRFunction& func);

// ── Span-based helper (Issue #128) ──────────────────────────
//
// For the per-block, per-instruction analysis, callers that
// already have a span of instructions can skip the block
// iteration. This is the hot path in incremental IR cache
// v2: when re-analyzing a single block after a mutation, the
// caller has the instructions in hand and just needs the
// kind for each slot.
//
// Returns a vector of the same length as `instructions`,
// one ComputeKind per instruction. Pure: no mutation.
export std::vector<ComputeKind>
compute_kind_instructions(std::span<const aura::ir::IRInstruction> instructions);

} // namespace aura::compiler
