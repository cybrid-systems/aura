// typed_mutation_audit_hooks.cpp — Issue #1884
// C linkage bridges so modules that cannot include typed_mutation_audit.h
// (mutex / module-import conflicts) can still stamp correlation counters.
//
// Also owns Issue #2262 free process atomics for hard-empty-miss / wired
// (import_total/skip live on aura.compiler.type_checker — see type_checker.ixx)
// and Issue #2263 escape → MoveOp elision gate state.
// Issue #2286: scoped gate state per (Evaluator*, workspace_cow_gen) —
// see ownership_escape_lowering_gate.h for the key struct + APIs.

#include "typed_mutation_audit.h"
#include "compiler/observability_metrics.h"
#include "compiler/ownership_escape_lowering_gate.h"

#include <atomic>
#include <cstdint>
#include <map>
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
// Issue #2286: keyed-lookup miss counter (cross-eval / cross-gen).
std::atomic<std::uint64_t> g_linear_escape_gate_cross_eval_miss_total{0};
// Issue #2344: Option A — miss path blocked because some other live key
// has the binding in its escape-blocked set.
std::atomic<std::uint64_t> g_linear_escape_gate_miss_conservative_block_total{0};
// Issue #2344: key-contract wiring sentinel (always 1 once linked).
std::atomic<std::uint32_t> g_linear_escape_gate_key_contract_wired{1};
// Issue #2309: rollback-clear counter (see ownership_escape_lowering_gate.h).
std::atomic<std::uint64_t> g_linear_escape_gate_clear_on_rollback_total{0};

namespace detail {
    // Issue #2286: thread-local current key — Evaluator sets before lowering,
    // clearing after. Lookup in lowering_linear_types_impl.cpp reads this.
    thread_local EscapeGateKey current_escape_key{};
} // namespace detail
} // namespace aura::compiler

namespace {
struct EscapeMoveElisionGateEntry {
    bool active = false;
    std::unordered_set<std::string> blocked;
};
struct EscapeMoveElisionGateState {
    std::mutex mu;
    std::map<EscapeGateKey, EscapeMoveElisionGateEntry> entries;
};
EscapeMoveElisionGateState& escape_move_gate_state() noexcept {
    static EscapeMoveElisionGateState s;
    return s;
}
} // namespace

// Issue #2286: publish under (eval, cow_gen) — production site
// (type_checker_impl.cpp::post_mutation_invariant_check) calls this
// instead of the legacy process-wide set_escape_move_elision_gate.
extern "C" void aura_escape_move_gate_publish_for_key(void* eval, std::uint64_t cow_gen, int active,
                                                      const char* const* names,
                                                      std::size_t n) noexcept {
    auto& s = escape_move_gate_state();
    std::lock_guard lock(s.mu);
    auto& entry = s.entries[{eval, cow_gen}];
    entry.active = active != 0;
    entry.blocked.clear();
    if (names) {
        for (std::size_t i = 0; i < n; ++i) {
            if (names[i] && names[i][0] != '\0')
                entry.blocked.insert(names[i]);
        }
    }
}

extern "C" void aura_escape_move_gate_clear_key(void* eval, std::uint64_t cow_gen) noexcept {
    auto& s = escape_move_gate_state();
    std::lock_guard lock(s.mu);
    s.entries.erase({eval, cow_gen});
}

// Issue #2286 / #2344: keyed lookup for MoveOp elision gate.
//
// Matching key (happy path, AC3): single map lookup — no full scan.
//   active + name in blocked → block (1)
//   active + name clean     → allow elide (0)
//   inactive entry          → allow elide (0)
//
// Miss (key absent) — Issue #2344 Option A (conservative):
//   1. bump cross_eval_miss
//   2. scan *all* active entries; if any blocks `binding`, return 1 and
//      bump miss_conservative_block_total
//   3. else return 0 (true unknown — no live summary blocks this name)
//
// Rationale: publish under key A + lower under key B (stale gen / wrong
// eval metrics / forgotten set_current_escape_key) must never elide a
// Move of a name that some concurrent live summary has escape-blocked.
// Disjoint names across evals still isolate (scan only matches the name).
extern "C" int aura_escape_blocks_move_elision_for_key(void* eval, std::uint64_t cow_gen,
                                                       const char* binding) noexcept {
    if (!binding || binding[0] == '\0')
        return 0;
    auto& s = escape_move_gate_state();
    std::lock_guard lock(s.mu);
    auto it = s.entries.find({eval, cow_gen});
    if (it != s.entries.end()) {
        // Matching key: zero-cost relative to full map scan (AC3).
        if (!it->second.active)
            return 0;
        return it->second.blocked.find(binding) != it->second.blocked.end() ? 1 : 0;
    }
    // Miss — Issue #2344 Option A.
    aura::compiler::g_linear_escape_gate_cross_eval_miss_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
    for (const auto& [key, entry] : s.entries) {
        (void)key;
        if (!entry.active)
            continue;
        if (entry.blocked.find(binding) != entry.blocked.end()) {
            aura::compiler::g_linear_escape_gate_miss_conservative_block_total.fetch_add(
                1, std::memory_order_relaxed);
            return 1;
        }
    }
    return 0;
}

// Legacy process-wide APIs — kept for backward compat with existing tests
// (#2263) and single-eval MVP. Route through the thread-local current key
// (default {nullptr, 0} when no Evaluator set it — tests use this).
extern "C" void aura_escape_move_gate_set(int active, const char* const* names,
                                          std::size_t n) noexcept {
    auto& s = escape_move_gate_state();
    std::lock_guard lock(s.mu);
    auto& entry = s.entries[aura::compiler::detail::current_escape_key];
    entry.active = active != 0;
    entry.blocked.clear();
    if (names) {
        for (std::size_t i = 0; i < n; ++i) {
            if (names[i] && names[i][0] != '\0')
                entry.blocked.insert(names[i]);
        }
    }
}

extern "C" void aura_escape_move_gate_clear() noexcept {
    auto& s = escape_move_gate_state();
    std::lock_guard lock(s.mu);
    s.entries[aura::compiler::detail::current_escape_key] = {};
}

extern "C" int aura_escape_move_gate_active() noexcept {
    auto& s = escape_move_gate_state();
    std::lock_guard lock(s.mu);
    auto it = s.entries.find(aura::compiler::detail::current_escape_key);
    if (it == s.entries.end())
        return 0;
    return it->second.active ? 1 : 0;
}

extern "C" int aura_escape_blocks_move_elision(const char* binding) noexcept {
    if (!binding || binding[0] == '\0')
        return 0;
    auto& s = escape_move_gate_state();
    std::lock_guard lock(s.mu);
    auto it = s.entries.find(aura::compiler::detail::current_escape_key);
    if (it == s.entries.end() || !it->second.active)
        return 0;
    return it->second.blocked.find(binding) != it->second.blocked.end() ? 1 : 0;
}

// Issue #2346: weak-friendly probe for Fiber::is_steal_snapshot_hard_mode
// production canary (serve layer must not import typed_mutation_audit.h).
extern "C" int aura_production_defaults_active_probe() noexcept {
    return aura::compiler::typed_audit::production_defaults_active() ? 1 : 0;
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
