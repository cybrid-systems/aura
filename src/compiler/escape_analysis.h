// escape_analysis.h — IR-level escape analysis for arena allocation
#pragma once

#include <cstdint>
#include <vector>
#include "ir.ixx"

namespace aura::compiler {

// Per-slot escape status
enum class EscapeStatus : uint8_t {
    UNKNOWN = 0,
    NON_ESCAPING = 1,  // value stays in defining scope
    ESCAPED = 2,        // value may outlive scope
};

// Results of escape analysis for a single function
struct EscapeInfo {
    // indexed by slot index; true = ESCAPED
    std::vector<bool> slot_escaped;

    bool is_escaped(uint32_t slot) const {
        return slot < slot_escaped.size() && slot_escaped[slot];
    }

    bool is_escaping_pair(uint32_t result_slot) const {
        return is_escaped(result_slot);
    }
};

// Escape analysis engine for one IRFunction
// Uses backward dataflow: propagates ESCAPED from escape points
// (Return, Call, Store, Capture, CellSet) to their operands.
class EscapeAnalyzer {
public:
    EscapeInfo analyze(const ir::IRFunction& fn);

    bool has_error() const { return false; }
};

// Combinable escape info across multiple functions
struct ModuleEscapeInfo {
    std::vector<EscapeInfo> per_function;
};

ModuleEscapeInfo analyze_module(const ir::IRModule& mod);

} // namespace aura::compiler
