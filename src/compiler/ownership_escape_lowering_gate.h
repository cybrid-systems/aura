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

// Issue #2286: scope the OwnershipEscapeSummary / MoveOp elision gate per
// (Evaluator*, workspace_cow_gen) so multi-eval / multi-workspace hosts
// (#2274 PerRegion storm isolation, #2275 CowGenMismatch) don't cross-
// contaminate elision decisions. Process-wide state from #2263 was the
// root cause — Evaluator A could block elision for binding `x` and
// Evaluator B would inherit that block (or worse, overwrite A's summary
// and wrongly elide under B).
struct EscapeGateKey {
    void* eval = nullptr;
    std::uint64_t cow_gen = 0;
    bool operator<(const EscapeGateKey& o) const noexcept {
        if (eval != o.eval)
            return eval < o.eval;
        return cow_gen < o.cow_gen;
    }
    bool operator==(const EscapeGateKey& o) const noexcept {
        return eval == o.eval && cow_gen == o.cow_gen;
    }
};

// C bridges (typed_mutation_audit_hooks.cpp) — safe from module purview.
extern "C" void aura_escape_move_gate_set(int active, const char* const* names,
                                          std::size_t n) noexcept;
extern "C" void aura_escape_move_gate_clear() noexcept;
extern "C" int aura_escape_move_gate_active() noexcept;
extern "C" int aura_escape_blocks_move_elision(const char* binding) noexcept;
// Issue #2286: keyed variants. publish stores under (eval, cow_gen);
// lookup consults that entry first.
// Issue #2344 (Option A): on miss, if *any* active keyed summary blocks the
// binding name, still block elision (conservative) — never miss→elide a
// name that a concurrent live summary has escape-blocked.
extern "C" void aura_escape_move_gate_publish_for_key(void* eval, std::uint64_t cow_gen, int active,
                                                      const char* const* names,
                                                      std::size_t n) noexcept;
extern "C" void aura_escape_move_gate_clear_key(void* eval, std::uint64_t cow_gen) noexcept;
extern "C" int aura_escape_blocks_move_elision_for_key(void* eval, std::uint64_t cow_gen,
                                                       const char* binding) noexcept;

namespace aura::compiler {

// Process atomics (query / tests) — defined once in typed_mutation_audit_hooks.cpp.
extern std::atomic<std::uint64_t> g_linear_move_elision_blocked_escape_total;
extern std::atomic<std::uint64_t> g_linear_lowering_escape_summary_hit_total;
extern std::atomic<std::uint32_t> g_linear_escape_move_gate_wired;
// Issue #2286: miss counter for cross-eval / cross-gen lookups.
extern std::atomic<std::uint64_t> g_linear_escape_gate_cross_eval_miss_total;
// Issue #2344: miss path took the Option A conservative block (any live
// summary blocks the binding name). Additive over #2286 miss counter.
extern std::atomic<std::uint64_t> g_linear_escape_gate_miss_conservative_block_total;
// Issue #2344: sentinel for key-contract wiring (publish key ↔ lower key).
extern std::atomic<std::uint32_t> g_linear_escape_gate_key_contract_wired;
// Issue #2309: rollback-clear counter — bumped by composite_txn_commit /
// MutationBoundary hard-gate force-rollback paths when they call
// aura_escape_move_gate_clear(). Agents read this to confirm the
// stale-gate leak fix landed (failure: a rejected txn's blocked set
// could survive into a subsequent independent mutate in the same
// process / same eval).
extern std::atomic<std::uint64_t> g_linear_escape_gate_clear_on_rollback_total;

namespace detail {
    // Issue #2286: thread-local current key set by Evaluator before lowering
    // (cleared after). Lookup in lowering_linear_types_impl.cpp reads this
    // rather than threading eval+cow_gen through the entire lowering chain.
    extern thread_local EscapeGateKey current_escape_key;
} // namespace detail

inline void set_current_escape_key(void* eval, std::uint64_t cow_gen) noexcept {
    detail::current_escape_key = {eval, cow_gen};
}

inline void clear_current_escape_key() noexcept {
    detail::current_escape_key = {nullptr, 0};
}

[[nodiscard]] inline EscapeGateKey current_escape_key() noexcept {
    return detail::current_escape_key;
}

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

// Issue #2286: publish under (eval, cow_gen) — replaces process-wide
// set_escape_move_elision_gate at production publish sites
// (type_checker_impl.cpp::post_mutation_invariant_check). Stores the
// summary in a per-key map; lookup uses the same key.
inline void
publish_escape_move_elision_gate_for_key(void* eval, std::uint64_t cow_gen, bool active,
                                         const std::unordered_set<std::string>& blocked) noexcept {
    std::vector<const char*> ptrs;
    ptrs.reserve(blocked.size());
    for (const auto& s : blocked)
        ptrs.push_back(s.c_str());
    aura_escape_move_gate_publish_for_key(eval, cow_gen, active ? 1 : 0,
                                          ptrs.empty() ? nullptr : ptrs.data(), ptrs.size());
}

inline void clear_escape_move_elision_gate_for_key(void* eval, std::uint64_t cow_gen) noexcept {
    aura_escape_move_gate_clear_key(eval, cow_gen);
}

// Issue #2286: lookup using the thread-local current key. The lowering
// path doesn't have direct access to (eval, cow_gen) — it reads the
// thread-local that the Evaluator set before invoking lower_to_ir.
[[nodiscard]] inline bool
escape_blocks_move_elision_for_current(std::string_view binding) noexcept {
    if (binding.empty())
        return false;
    std::string tmp(binding);
    return aura_escape_blocks_move_elision_for_key(detail::current_escape_key.eval,
                                                   detail::current_escape_key.cow_gen,
                                                   tmp.c_str()) != 0;
}

// Issue #2286: explicit-key lookup (for tests + multi-eval callers that
// don't rely on the thread-local). Production lowering uses the
// thread-local variant above.
[[nodiscard]] inline bool escape_blocks_move_elision_for_key(void* eval, std::uint64_t cow_gen,
                                                             std::string_view binding) noexcept {
    if (binding.empty())
        return false;
    std::string tmp(binding);
    return aura_escape_blocks_move_elision_for_key(eval, cow_gen, tmp.c_str()) != 0;
}

} // namespace aura::compiler

#endif // AURA_COMPILER_OWNERSHIP_ESCAPE_LOWERING_GATE_H
