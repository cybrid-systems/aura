# 0 "/home/dev/code/aura/src/compiler/arity.ixx"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/compiler/arity.ixx"
export module aura.compiler.arity;
import std;
import aura.compiler.ir;

namespace aura::compiler {


export struct ArityDiagnostic {
    std::uint32_t func_id = 0;
    std::uint32_t block_id = 0;
    std::uint32_t instr_index = 0;
    std::uint32_t expected = 0;
    std::uint32_t actual = 0;
    std::string function_name;
    std::string message;
};


export struct ArityCheckResult {
    bool has_error = false;
    std::vector<ArityDiagnostic> diagnostics;
};


export ArityCheckResult check_arity(const aura::ir::IRModule& mod);

}
