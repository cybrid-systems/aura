export module aura.compiler.compute_kind;
import std;
import aura.compiler.ir;

namespace aura::compiler {

// ComputeKind: compile-time evaluability analysis
export enum class ComputeKind : std::uint8_t {
    Unknown = 0,
    Known   = 1,
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

} // namespace aura::compiler
