// typed_mutation_audit_hooks.cpp — Issue #1884
// C linkage bridges so modules that cannot include typed_mutation_audit.h
// (mutex / module-import conflicts) can still stamp correlation counters.
//
// Also owns Issue #2262 free process atomics for hard-empty-miss / wired
// (import_total/skip live on aura.compiler.type_checker — see type_checker.ixx)
// and Issue #2263 escape → MoveOp elision gate state.

#include "typed_mutation_audit.h"
#include "compiler/observability_metrics.h"
#include "compiler/ownership_escape_lowering_gate.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace aura::compiler {
// Issue #2262: free process storage (evaluator bumps hard_empty_miss;
// query reads wired). Not module-attached.
std::atomic<std::uint64_t> g_partial_cs_hard_empty_miss_total{0};
std::atomic<std::uint32_t> g_partial_cs_single_source_wired{1};

// Issue #2263: process atomics + gate state (non-module TU owns storage).
std::atomic<std::uint64_t> g_linear_move_elision_blocked_escape_total{0};
std::atomic<std::uint64_t> g_linear_lowering_escape_summary_hit_total{0};
std::atomic<std::uint32_t> g_linear_escape_move_gate_wired{1};
} // namespace aura::compiler

namespace {
struct EscapeMoveElisionGateState {
    std::mutex mu;
    bool active = false;
    std::unordered_set<std::string> blocked;
};
EscapeMoveElisionGateState& escape_move_gate_state() noexcept {
    static EscapeMoveElisionGateState s;
    return s;
}
} // namespace

extern "C" void aura_escape_move_gate_set(int active, const char* const* names,
                                          std::size_t n) noexcept {
    auto& s = escape_move_gate_state();
    std::lock_guard lock(s.mu);
    s.active = active != 0;
    s.blocked.clear();
    if (names) {
        for (std::size_t i = 0; i < n; ++i) {
            if (names[i] && names[i][0] != '\0')
                s.blocked.insert(names[i]);
        }
    }
}

extern "C" void aura_escape_move_gate_clear() noexcept {
    aura_escape_move_gate_set(0, nullptr, 0);
}

extern "C" int aura_escape_move_gate_active() noexcept {
    auto& s = escape_move_gate_state();
    std::lock_guard lock(s.mu);
    return s.active ? 1 : 0;
}

extern "C" int aura_escape_blocks_move_elision(const char* binding) noexcept {
    if (!binding || binding[0] == '\0')
        return 0;
    auto& s = escape_move_gate_state();
    std::lock_guard lock(s.mu);
    if (!s.active)
        return 0;
    return s.blocked.find(binding) != s.blocked.end() ? 1 : 0;
}

extern "C" void aura_typed_audit_note_predicate_memo_eviction(std::uint64_t n) {
    aura::compiler::typed_audit::note_predicate_memo_eviction(n);
}

extern "C" void aura_typed_audit_note_type_propagation_pass(std::uint64_t fixpoint_rounds,
                                                            std::uint64_t narrow_hits,
                                                            std::uint64_t extended_ops) {
    aura::compiler::typed_audit::note_type_propagation_pass(fixpoint_rounds, narrow_hits,
                                                            extended_ops);
}

extern "C" void aura_typed_audit_note_dce_narrow_hits(std::uint64_t narrow_hits) {
    aura::compiler::typed_audit::note_dce_narrow_hits(narrow_hits);
}
