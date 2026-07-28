// ownership_escape_lowering_gate.h — Issue #2263
// Process-wide gate so lowering (non-type_checker modules) can consult
// the last OwnershipEscapeSummary without importing type_checker
// (avoids module cycles: lowering → evaluator already).
//
// Definitions live in typed_mutation_audit_hooks.cpp (non-module TU).
// Gate mutators used from type_checker purview go through C linkage so
// Clang does not attach @type_checker module linkage to undefined symbols.
//
// Contract:
//   summary inactive (default) → legacy: always emit MoveOp (no elision)
//   summary active + binding in blocked set → emit MoveOp; bump blocked
//   summary active + binding not blocked → elide MoveOp; bump elided
//
#ifndef AURA_COMPILER_OWNERSHIP_ESCAPE_LOWERING_GATE_H
#define AURA_COMPILER_OWNERSHIP_ESCAPE_LOWERING_GATE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// C bridges (typed_mutation_audit_hooks.cpp) — safe from module purview.
extern "C" void aura_escape_move_gate_set(int active, const char* const* names,
                                          std::size_t n) noexcept;
extern "C" void aura_escape_move_gate_clear() noexcept;
extern "C" int aura_escape_move_gate_active() noexcept;
extern "C" int aura_escape_blocks_move_elision(const char* binding) noexcept;

namespace aura::compiler {

// Process atomics (query / tests) — defined once in typed_mutation_audit_hooks.cpp.
extern std::atomic<std::uint64_t> g_linear_move_elision_blocked_escape_total;
extern std::atomic<std::uint64_t> g_linear_lowering_escape_summary_hit_total;
extern std::atomic<std::uint32_t> g_linear_escape_move_gate_wired;

// C++ wrappers (non-module TUs + tests; also usable from module GMF
// when inlined against C linkage).
inline void set_escape_move_elision_gate(bool active,
                                         const std::unordered_set<std::string>& blocked) noexcept {
    if (!active) {
        aura_escape_move_gate_clear();
        return;
    }
    // Stack vector of c_str pointers (binding names live in blocked).
    std::vector<const char*> ptrs;
    ptrs.reserve(blocked.size());
    for (const auto& s : blocked)
        ptrs.push_back(s.c_str());
    aura_escape_move_gate_set(1, ptrs.empty() ? nullptr : ptrs.data(), ptrs.size());
}

inline void clear_escape_move_elision_gate() noexcept {
    aura_escape_move_gate_clear();
}

[[nodiscard]] inline bool escape_move_elision_gate_active() noexcept {
    return aura_escape_move_gate_active() != 0;
}

[[nodiscard]] inline bool escape_blocks_move_elision(std::string_view binding) noexcept {
    if (binding.empty())
        return false;
    // Need a null-terminated temporary for C API.
    std::string tmp(binding);
    return aura_escape_blocks_move_elision(tmp.c_str()) != 0;
}

} // namespace aura::compiler

#endif // AURA_COMPILER_OWNERSHIP_ESCAPE_LOWERING_GATE_H
