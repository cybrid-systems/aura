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
#include "core/mutation_audit_wal.hh"
#include "core/persistent_child_vector.hh"
#include "compiler/ownership_escape_lowering_gate.h"
#include "core/lifetime_pin.hh" // #3294 CI: disarm GeneralObjectPin required pref on dev reset

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

extern "C" void aura_pcv_set_stale_span_exclusive(int on) noexcept {
    aura::ast::pcv_set_stale_span_exclusive_enabled(on != 0);
}

// Issue #3294 CI: dev/test face reset. A prior member that called
// apply_production_security_defaults() armed g_general_object_pin_required_pref
// process-wide (one-way latch, security_defaults.hh step 15 #2597). Without a
// disarm here, every later (set-code) in the same process fails closed at the
// GeneralObjectPin wire (#2891) -> workspace never created -> typecheck/query
// no-op -> #3294 AC4 / dirty-cone batch failures. Restore observe-only (-1)
// + clear the sticky densify breach.
extern "C" void aura_reset_general_object_pin_required_for_test() noexcept {
    aura::core::lifetime::g_general_object_pin_required_pref.store(-1, std::memory_order_release);
    aura::core::lifetime::g_general_object_pin_required_breach.store(0, std::memory_order_release);
}

namespace aura::compiler::typed_audit {

void maybe_persist_typed_summary(const TypedMutationAuditEvent& ev) noexcept {
    // Issue #3242: production + mutation WAL → compact typed summary.
    // Issue #3298: gate must match the Full hard face — Full-only / embed /
    // never-apply-production-defaults deployments audit fully in-memory but
    // would never persist the sidecar (trail wrap loses "what changed").
    // Soft / Sampled / WAL-off: two loads, no fwrite. mid=0 already dropped
    // by capture_audit_event_forced (no invented Success summary).
    if (ev.mutation_id == 0)
        return;
    if (!(production_defaults_active() || get_strategy() == AuditStrategy::Full))
        return;
    auto& wal = ::aura::core::audit_wal::g_mutation_audit_wal();
    if (!wal.is_enabled())
        return;
    ::aura::core::audit_wal::TypedSummaryWalRecord rec{};
    rec.mutation_id = ev.mutation_id;
    rec.seq = ev.seq;
    rec.timestamp_ms = ev.timestamp_ms;
    rec.nodes_changed = ev.nodes_changed;
    rec.target_node = ev.target_node;
    rec.outcome = static_cast<std::uint8_t>(ev.outcome);
    rec.kind = static_cast<std::uint8_t>(ev.kind);
    rec.name_hash = 2166136261u;
    for (std::size_t i = 0; i < kAuditNameCap && ev.name[i] != '\0'; ++i) {
        rec.name_hash = (rec.name_hash ^ static_cast<std::uint8_t>(ev.name[i])) * 16777619u;
    }
    if (wal.append_typed_summary(rec)) {
        g_typed_mutation_audit_counters.typed_summary_wal_persisted_total.fetch_add(
            1, std::memory_order_relaxed);
    }
}

} // namespace aura::compiler::typed_audit

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
// Issue #2507: steal / densify clear-by-eval path counters.
std::atomic<std::uint64_t> g_linear_escape_gate_steal_clear_total{0};
std::atomic<std::uint64_t> g_linear_escape_gate_densify_clear_total{0};
std::atomic<std::uint64_t> g_linear_escape_gate_steal_clear_entries_total{0};
std::atomic<std::uint64_t> g_linear_escape_gate_densify_clear_entries_total{0};

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

// Issue #2507: erase all map entries whose EscapeGateKey::eval matches.
// Soft empty: single walk under lock, no alloc. Prefer over process wipe
// so other evals' summaries stay (#2286 isolation).
extern "C" std::size_t aura_escape_move_gate_clear_eval(void* eval) noexcept {
    if (!eval)
        return 0;
    auto& s = escape_move_gate_state();
    std::lock_guard lock(s.mu);
    std::size_t erased = 0;
    for (auto it = s.entries.begin(); it != s.entries.end();) {
        if (it->first.eval == eval) {
            it = s.entries.erase(it);
            ++erased;
        } else {
            ++it;
        }
    }
    return erased;
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

// Issue #3006: lowering hard-block for mid-boundary / densify-pending
// (escape-blocked names stay on the keyed #2263/#2344 path so clean
// bindings under an active summary can still elide).
// Issue #3085: also block when rehydrate-miss / steal-densify
// invalidate_gen has not been green-bound (does not use the escape
// arm — #2263 clean elide stays when gens match).
extern "C" int aura_linear_fast_path_ok() noexcept {
    return aura::compiler::typed_audit::linear_fast_path_ok() ? 1 : 0;
}
extern "C" int aura_linear_fast_path_depth_or_densify_block() noexcept {
    using aura::compiler::typed_audit::g_linear_ir_fastpath_boundary_depth_override;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    // Issue #3558: OR semantics — either override or actual depth alone
    // blocks. Mid-boundary override=0 + actual depth>0 must not return 0.
    // Name finally matches behavior.
    if (g_linear_ir_fastpath_boundary_depth_override > 0)
        return 1;
    if (aura_evaluator_mutation_boundary_depth() > 0)
        return 1;
    if (g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.load(
            std::memory_order_relaxed) > 0)
        return 1;
    if (aura::compiler::typed_audit::linear_fast_path_rehydrate_gen_blocks_elision())
        return 1;
    return 0;
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

// Issue #3532: classify production/Full mid=0 SE reasons. Canonical
// refuse reasons pass through; anything else is caller-misuse and is
// rewritten to audit-mid-ssot-miss-not-refuse. Soft/Off returns `reason`
// unchanged (no counter bump).
extern "C" const char* aura_classify_mid0_se_reason(const char* reason) noexcept {
    using aura::compiler::typed_audit::AuditStrategy;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    using aura::compiler::typed_audit::get_strategy;
    using aura::compiler::typed_audit::kAuditMidSsotMissReason;
    using aura::compiler::typed_audit::production_defaults_active;
    const std::string_view r = reason ? std::string_view{reason} : std::string_view{};
    if (r == "mid-fallback-refused" || r == "grant-mid-refused" || r == kAuditMidSsotMissReason)
        return reason ? reason : "";
    if (!(production_defaults_active() || get_strategy() == AuditStrategy::Full))
        return reason ? reason : "";
    g_typed_mutation_audit_counters.audit_mid_ssot_miss_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
    return kAuditMidSsotMissReason.data();
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
