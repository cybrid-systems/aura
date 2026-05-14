# 0 "/home/dev/code/aura/src/compiler/compute_kind.ixx"
# 1 "/home/dev/code/aura/build_debug//"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/compiler/compute_kind.ixx"
export module aura.compiler.compute_kind;
import std;
import aura.compiler.ir;

namespace aura::compiler {


export enum class ComputeKind : std::uint8_t {
    Unknown = 0,
    Known = 1,
};


export struct ComputeKindInfo {
    ComputeKind kind = ComputeKind::Unknown;
};


export struct ComputeKindResult {
    std::vector<std::vector<ComputeKind>> per_block_inst_kind;
    bool valid = false;
};


export ComputeKindResult compute_kind(const aura::ir::IRFunction& func);

}
