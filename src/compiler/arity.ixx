module;
#include <cstdint>
#include <string>
#include <vector>

export module aura.compiler.arity;
import std;
import aura.compiler.ir;

namespace aura::compiler {

// Arity mismatch diagnostic
export struct ArityDiagnostic {
    std::uint32_t func_id = 0;
    std::uint32_t block_id = 0;
    std::uint32_t instr_index = 0;
    std::uint32_t expected = 0;
    std::uint32_t actual = 0;
    std::string function_name;
    std::string message;
};

// Arity check result
export struct ArityCheckResult {
    bool has_error = false;
    std::vector<ArityDiagnostic> diagnostics;
};

// Pure function: check all calls in an IRModule for arity mismatches
export ArityCheckResult check_arity(const aura::ir::IRModule& mod);

} // namespace aura::compiler
