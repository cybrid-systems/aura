export module aura.compiler.compute_kind;
import std;
import aura.compiler.ir;

namespace aura::compiler {

// ComputeKind: compile-time evaluability analysis
// Known  = value can be determined at compile time (constant or pure)
// Unknown = value depends on runtime state
export enum class ComputeKind : std::uint8_t {
    Unknown = 0,
    Known   = 1,
};

// Tag each IR instruction with its result's compute kind
// The `result_kind` field is set for instructions that produce a value
export struct ComputeKindInfo {
    ComputeKind kind = ComputeKind::Unknown;
};

// Analysis result: per-function, per-instruction compute kind
export struct ComputeKindResult {
    // For each function, a map from instruction index to the kind of its result slot
    std::vector<std::vector<ComputeKind>> per_block_inst_kind;
    bool valid = false;
};

// Run compute-kind analysis on an IR module
// Returns analysis result for each function
export class ComputeKindAnalysis {
public:
    ComputeKindResult analyze(const aura::ir::IRFunction& func);
};

} // namespace aura::compiler
