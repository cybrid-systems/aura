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
    bool is_warning = false;  // false = cannot proceed, true = may fail at runtime
    std::string function_name;
    std::string message;
};

// Arity check result
export struct ArityCheckResult {
    bool has_error = false;
    std::vector<ArityDiagnostic> diagnostics;
};

// Arity checking pass
export class ArityChecker {
public:
    // Check all calls in an IRModule for arity mismatches
    ArityCheckResult check(const aura::ir::IRModule& mod);

    // Check a single function's calls
    ArityCheckResult check_function(const aura::ir::IRFunction& func,
                                     const aura::ir::IRModule& mod);

private:
    // Try to determine the callee function from an instruction's source slot
    // Returns the func_id if known, or -1 if unknown
    int resolve_callee_func(const aura::ir::IRFunction& func,
                            const aura::ir::IRModule& mod,
                            std::uint32_t slot) const;

    // Check if a specific call has matching arity
    void check_call(const aura::ir::IRFunction& func,
                    const aura::ir::IRModule& mod,
                    const aura::ir::IRInstruction& instr,
                    std::uint32_t block_id,
                    std::uint32_t instr_index,
                    std::vector<ArityDiagnostic>& diags);
};

} // namespace aura::compiler
