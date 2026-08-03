// coercion_map.ixx — Deferred CoercionNode insertion (Issue #116)
//
// The TypeChecker used to mutate the input FlatAST in-place to
// wrap mismatched expressions in CoercionNode wrappers, rewriting
// the parent→child link in the process. This broke the design
// contract that `ast:snapshot` / `ast:rollback` can rely on
// pre-typecheck state, and made the type checker unsafe to
// invoke on shared/versioned ASTs (e.g. for AI self-modifying
// code workflows where the AST may be inspected while type
// checking is in progress).
//
// The fix: type checking now collects coercion intent into a
// `CoercionMap` (a pure data structure with no AST references
// beyond NodeId integers). The mutation is then performed as a
// separate explicit pass via `apply_coercion_map`, which can be
// called once at the boundary between type checking and
// lowering/IR emission. The TypeChecker is now structurally
// read-only on the FlatAST; the only remaining mutation is
// `set_node_error` (a per-node metadata annotation that does
// not change tree structure, separately documented in
// `src/core/ast.ixx`).
//
// The map stores (parent, child_index, original_child,
// type_tag, type_id, src_line, src_col) tuples. Insertion order
// is preserved because child indices may shift if two
// coercions target the same parent — but in practice the type
// checker only emits one coercion per parent/child slot per
// call, and the apply pass uses the recorded (parent, child,
// index) triple to locate the original child before rewriting,
// so duplicate or out-of-order entries are safe.
//
// Apply pass is idempotent: applying twice is a no-op the
// second time (the original child is no longer there, so no
// match).

module;
#include <atomic>
#include <cstdint>
#include <vector>
#include "core/provenance_tracker.hh"             // Issue #2024: hygiene stamp + chain recovery
#include "compiler/dce_elided_deopt_meta.h"       // Issue #2611: AST identity elision deopt meta
#include "core/sandbox.hh"                        // Issue #2147: Strict honesty
#include "compiler/typed_mutation_audit.h"        // Issue #2147: Full vs Sampled walk cap
#include "compiler/coercion_provenance_policy.hh" // Issue #2102 / #2185 miss policy

export module aura.compiler.coercion_map;

import aura.core.ast;

namespace aura::compiler {

// Re-export policy symbols for module importers (atomics live in the
// shared header so security_defaults can flip production reject-on-miss
// and #2221 blame-complete commit require).
export using ::aura::compiler::g_force_audit_on_provenance_miss;
export using ::aura::compiler::g_reject_apply_on_provenance_miss;
export using ::aura::compiler::g_coercion_provenance_miss_force_audit_total;
export using ::aura::compiler::g_coercion_provenance_miss_reject_total;
export using ::aura::compiler::g_require_blame_complete_on_commit;
export using ::aura::compiler::g_blame_commit_reject_total;
export using ::aura::compiler::g_blame_commit_incomplete_observe_total;
export using ::aura::compiler::g_blame_commit_check_total;
export using ::aura::compiler::kCoercionProvenanceRejectProductionIssue;
export using ::aura::compiler::kBlameCommitRequireIssue;
export using ::aura::compiler::set_force_audit_on_provenance_miss;
export using ::aura::compiler::set_reject_apply_on_provenance_miss;
export using ::aura::compiler::set_require_blame_complete_on_commit;
export using ::aura::compiler::force_audit_on_provenance_miss;
export using ::aura::compiler::reject_apply_on_provenance_miss;
export using ::aura::compiler::require_blame_complete_on_commit;
export using ::aura::compiler::note_provenance_miss_for_boundary;
export using ::aura::compiler::provenance_miss_pending_for_boundary;
export using ::aura::compiler::consume_provenance_miss_for_boundary;
export using ::aura::compiler::reset_coercion_provenance_miss_policy_for_test;
export using ::aura::compiler::apply_production_coercion_provenance_defaults;
export using ::aura::compiler::apply_coercion_provenance_reject_env_override;
export using ::aura::compiler::apply_blame_commit_require_env_override;
// Issue #2558: completeness SLO → force Full audit on next boundary.
export using ::aura::compiler::kCoercionProvSloIssue;
export using ::aura::compiler::kCoercionProvSloBpDefault;
export using ::aura::compiler::g_coercion_prov_slo_breach_total;
export using ::aura::compiler::g_coercion_prov_slo_observe_only_total;
export using ::aura::compiler::g_coercion_prov_slo_force_armed_total;
export using ::aura::compiler::g_coercion_prov_slo_force_consumed_total;
export using ::aura::compiler::g_coercion_prov_slo_force_full_pending;
export using ::aura::compiler::coercion_prov_slo_bp;
export using ::aura::compiler::set_coercion_prov_slo_bp_for_test;
export using ::aura::compiler::evaluate_coercion_provenance_slo;
export using ::aura::compiler::coercion_prov_slo_force_full_pending;
export using ::aura::compiler::consume_coercion_prov_slo_force_full;
// Issue #2561: Soft blame recovery / escalate.
export using ::aura::compiler::kBlameSoftRecoverIssue;
export using ::aura::compiler::g_blame_soft_recover_total;
export using ::aura::compiler::g_blame_soft_recover_fail_total;
export using ::aura::compiler::g_blame_soft_escalate_total;
export using ::aura::compiler::blame_soft_escalate_enabled;
export using ::aura::compiler::note_blame_soft_escalate_for_boundary;
export using ::aura::compiler::blame_soft_escalate_pending_for_boundary;
export using ::aura::compiler::consume_blame_soft_escalate_for_boundary;
// Issue #2562: dual-field require-or-drop.
export using ::aura::compiler::kCoercionDualRequireIssue;
export using ::aura::compiler::g_coercion_dual_require;
export using ::aura::compiler::g_coercion_dual_require_drop_total;
export using ::aura::compiler::g_coercion_dual_require_wired;
export using ::aura::compiler::set_coercion_dual_require;
export using ::aura::compiler::coercion_dual_require_flag;
export using ::aura::compiler::coercion_dual_require_env;
export using ::aura::compiler::coercion_dual_require_enabled;

// Issue #2024: forensic sentinel base for incomplete occurrence-narrowing
// provenance (high nibble C0E5 = "coercion"). Low 16 bits carry original_child
// so Agents can recover the site when both predicate and mutation were unset.
export inline constexpr std::uint32_t kCoercionProvenanceSentinelBase = 0xC0E50000u;

// Issue #2147: parent-walk hop caps (Sampled hot path vs Full/Strict depth).
export inline constexpr int kCoercionParentWalkCapSampled = 16;
export inline constexpr int kCoercionParentWalkCapFull = 64;

// Issue #2024: process-wide apply_coercion_map provenance completeness.
// Complete = both predicate_cond_node and source_mutation_id non-zero after
// full chain walk, and mutation id is NOT the weak original_child placeholder
// under Strict/Full (#2147). Miss = needed sentinel / weak fallback.
// Ratio = complete / (complete + miss) as basis points (0–10000).
export inline std::atomic<std::uint64_t> g_coercion_provenance_complete_total{0};
export inline std::atomic<std::uint64_t> g_coercion_provenance_miss_total{0};
export inline std::atomic<std::uint64_t> g_coercion_provenance_sentinel_total{0};
export inline std::atomic<std::uint64_t> g_coercion_provenance_chain_walk_total{0};
// Issue #2147: caller already stamped both fields → no walk (AC1).
export inline std::atomic<std::uint64_t> g_coercion_provenance_fast_path_total{0};
// Issue #2147: weak source_mutation_id == original_child forensic placeholder.
export inline std::atomic<std::uint64_t> g_coercion_provenance_weak_id_total{0};
// Issue #2147: Strict/Full refused to count weak id as complete / refused stamp.
export inline std::atomic<std::uint64_t> g_coercion_provenance_strict_reject_weak_total{0};
// Issue #2261: Sampled skipped CoercionNode insert on incomplete provenance
// (never stamp weak mid / sentinel pretend into IR under Sampled).
export inline std::atomic<std::uint64_t> g_coercion_provenance_sampled_reject_total{0};
// Issue #2317: Sampled insert counter — bumped when Sampled +
// incomplete provenance + NOT production reject → still insert
// CoercionNode (with force-audit via fill_coercion_provenance_chain's
// note_provenance_miss_for_boundary call). Distinct from
// coercion_provenance_sampled_reject_total which counts SKIPS.
export inline std::atomic<std::uint64_t> g_coercion_sampled_insert_incomplete_total{0};
export inline std::atomic<std::uint32_t> g_coercion_provenance_ban_weak_ir_wired{1};
// Issue #2512: times CoercionMap::add stamped TLS active mid/pred into a
// zero-field entry (deferred-add completeness). Completeness_bp remains
// the authority for apply-time quality.
export inline std::atomic<std::uint64_t> g_coercion_stamp_at_add_total{0};
export inline std::atomic<std::uint32_t> g_coercion_stamp_at_add_wired{1};
// Issue #2562: dual-require drop counter is process-wide in policy.hh
// (g_coercion_dual_require_drop_total); re-exported above.
// Issue #2025: AST-level identity elision count (apply_coercion_map) for
// layered zero-overhead synergy with IR DeadCoercionEliminationPass.
// Issue #2282: combined on query:dead-coercion-layered-stats as the
// `ast-elided` component (see optimization_passes.ixx for ir-elided + dirty-cone-skips).
export inline std::atomic<std::uint64_t> g_dead_coercion_ast_elided_total{0};

// Issue #2102 / #2185: provenance-miss policy atomics + helpers live in
// coercion_provenance_policy.hh (re-exported above). Process start keeps
// reject=false; apply_production_security_defaults forces reject=true
// under production sandbox (Issue #2185).

// Issue #2147 Phase A.2: thread-local active mutation context so log scan
// is O(1) when entry.source_mutation_id is empty (Guard / TypeChecker stamp).
inline thread_local std::uint64_t s_coercion_active_mutation_id = 0;
inline thread_local std::uint32_t s_coercion_active_predicate = 0;

export inline void set_coercion_active_mutation_context(std::uint64_t mutation_id,
                                                        std::uint32_t predicate_cond = 0) noexcept {
    s_coercion_active_mutation_id = mutation_id;
    s_coercion_active_predicate = predicate_cond;
}
export inline void clear_coercion_active_mutation_context() noexcept {
    s_coercion_active_mutation_id = 0;
    s_coercion_active_predicate = 0;
}
export [[nodiscard]] inline std::uint64_t coercion_active_mutation_id() noexcept {
    return s_coercion_active_mutation_id;
}
export [[nodiscard]] inline std::uint32_t coercion_active_predicate() noexcept {
    return s_coercion_active_predicate;
}

export [[nodiscard]] inline std::uint64_t coercion_provenance_completeness_bp() noexcept {
    const auto c = g_coercion_provenance_complete_total.load(std::memory_order_relaxed);
    const auto m = g_coercion_provenance_miss_total.load(std::memory_order_relaxed);
    const auto d = c + m;
    return d > 0 ? (c * 10000u) / d : 10000u; // no samples → vacuously complete
}

// ── CoercionEntry — one deferred coercion ────────────────
//
// Describes: "the child at index `child_index` of parent
// `parent_id` (which currently points to `original_child`)
// should be wrapped in a CoercionNode targeting `type_id`
// with runtime check tag `type_tag` (the CastOp type_tag, see
// type_checker_impl.cpp `type_tag_for_coercion`)."
//
// `src_line` / `src_col` are copied onto the CoercionNode for
// blame tracking (Issue #79 — the CoercionNode inherits the
// source location of the original expression).
//
// `parent_id` of 0 (aura::ast::NULL_NODE) means the coercion
// has no parent slot to rewrite — the apply pass still
// creates the CoercionNode for the IR generator to see, but
// doesn't touch any parent link.
export struct CoercionEntry {
    std::uint32_t parent_id;
    std::uint32_t child_index;
    std::uint32_t original_child;
    std::uint32_t type_tag;
    std::uint32_t type_id;
    std::uint32_t src_line;
    std::uint32_t src_col;
    // Issue #537 / #518 Phase 2: optional occurrence-narrowing
    // provenance carried into apply_coercion_map. 0 = unset.
    std::uint32_t predicate_cond_node = 0;
    std::uint64_t source_mutation_id = 0;
    // Issue #691: narrowing-evidence bitmask for post-narrow
    // CastOp elision (stored on Coercion node float_val_).
    std::uint32_t narrow_evidence = 0;
};

// Issue #2512: stamp TLS active mutation/predicate into a CoercionEntry at
// deferred-add. Never overwrites non-zero caller stamps (occurrence / explicit
// mid). Zero cost when both fields already set or TLS is empty (AC3/AC5).
// Returns true when at least one field was stamped from context.
export inline bool stamp_coercion_entry_from_active_context(CoercionEntry& e) noexcept {
    bool stamped = false;
    if (e.source_mutation_id == 0 && s_coercion_active_mutation_id != 0) {
        e.source_mutation_id = s_coercion_active_mutation_id;
        stamped = true;
    }
    if (e.predicate_cond_node == 0 && s_coercion_active_predicate != 0) {
        e.predicate_cond_node = s_coercion_active_predicate;
        stamped = true;
    }
    if (stamped)
        g_coercion_stamp_at_add_total.fetch_add(1, std::memory_order_relaxed);
    return stamped;
}

// Issue #2147: weak forensic mutation id = original_child placeholder
// (not a real MutationRecord id). Must never count as complete under
// Strict sandbox / Full audit strategy.
// Issue #2261: also never write weak mid onto CoercionNode provenance.
export [[nodiscard]] inline bool is_weak_coercion_mutation_id(const CoercionEntry& e) noexcept {
    if (e.source_mutation_id == 0)
        return false;
    if (e.original_child != 0 &&
        e.source_mutation_id == static_cast<std::uint64_t>(e.original_child))
        return true;
    // original_child==0 path uses weak id 1 as forensic placeholder.
    if (e.original_child == 0 && e.source_mutation_id == 1ull)
        return true;
    return false;
}

// Issue #2562: dual-field completeness predicate (pred + non-weak mid).
// Agents pre-check after stamp-at-add / before mutate; apply uses the same
// gate under dual-require. Zero cost when both fields already set.
export [[nodiscard]] inline bool coercion_entry_dual_complete(const CoercionEntry& e) noexcept {
    return e.predicate_cond_node != 0 && e.source_mutation_id != 0 &&
           !is_weak_coercion_mutation_id(e);
}

// Issue #2562: dual-require active? Env / process flag + production defaults
// + Full strategy (Goal 1). Soft Sampled default remains off (#2317 insert).
export [[nodiscard]] inline bool coercion_dual_require_active() noexcept {
    const int env = coercion_dual_require_env();
    if (env == 0)
        return false; // Soft canary force-off
    if (env == 1)
        return true;
    if (coercion_dual_require_flag())
        return true;
    if (aura::compiler::typed_audit::production_defaults_active())
        return true;
    return aura::compiler::typed_audit::get_strategy() ==
           aura::compiler::typed_audit::AuditStrategy::Full;
}

// Issue #2147: Strict sandbox OR Full TypedMutationAudit → honest provenance
// (no weak-as-complete; no forensic sentinel pretend under hard gate).
[[nodiscard]] inline bool coercion_provenance_strict_honest() noexcept {
    if (aura::core::sandbox::is_strict())
        return true;
    return aura::compiler::typed_audit::get_strategy() ==
           aura::compiler::typed_audit::AuditStrategy::Full;
}

[[nodiscard]] inline int coercion_parent_walk_cap() noexcept {
    // Full / Strict: full 64-hop depth. Sampled / Off: shorter hot path.
    if (coercion_provenance_strict_honest())
        return kCoercionParentWalkCapFull;
    return kCoercionParentWalkCapSampled;
}

// Issue #2024 / #2102 / #2147 / #2561: walk provenance chain to fill missing
// CoercionEntry fields. Fast path (#2147 AC1): both fields already set and
// not weak → no walk, chain_walk_total unchanged.
// Order (slow path): child column → parent walk (capped) → TLS active
// mutation context → mutation log → hygiene → sentinel/weak (soft only).
// recovery_mode (#2561): re-walk without bumping miss/SLO force (Soft recover).
// Returns true when *truly* complete (non-zero pred + non-weak mutation id).
export [[nodiscard]] inline bool
fill_coercion_provenance_chain(aura::ast::FlatAST& flat, CoercionEntry& e,
                               bool recovery_mode = false) noexcept {
    using aura::ast::NULL_NODE;
    const bool strict = coercion_provenance_strict_honest();

    // ── Issue #2147 Phase A: fast path — caller-stamped true provenance ──
    if (e.predicate_cond_node != 0 && e.source_mutation_id != 0 &&
        !is_weak_coercion_mutation_id(e)) {
        g_coercion_provenance_fast_path_total.fetch_add(1, std::memory_order_relaxed);
        g_coercion_provenance_complete_total.fetch_add(1, std::memory_order_relaxed);
        return true; // no chain_walk bump
    }

    g_coercion_provenance_chain_walk_total.fetch_add(1, std::memory_order_relaxed);

    // 1. Child provenance column
    if (e.predicate_cond_node == 0 && e.original_child != NULL_NODE &&
        e.original_child < flat.size()) {
        const auto child_prov = flat.provenance(e.original_child);
        if (child_prov != 0)
            e.predicate_cond_node = child_prov;
    }

    // 2. Walk parent chain for first non-zero provenance (cross-delta
    // rewrite often leaves the child blank while an ancestor retains it).
    // Issue #2147: hop cap Sampled=16 / Full=64.
    if (e.predicate_cond_node == 0 && e.original_child != NULL_NODE &&
        e.original_child < flat.size()) {
        auto cur = static_cast<aura::ast::NodeId>(e.original_child);
        const int hop_cap = coercion_parent_walk_cap();
        for (int hops = 0; hops < hop_cap; ++hops) {
            if (cur == NULL_NODE || cur >= flat.size())
                break;
            const auto p = flat.provenance(cur);
            if (p != 0) {
                e.predicate_cond_node = p;
                break;
            }
            const auto par = flat.parent_of(cur);
            if (par == cur || par == NULL_NODE)
                break;
            cur = par;
        }
    }

    // 2b. Issue #2147: TLS active mutation context (O(1) before log scan).
    if (e.source_mutation_id == 0 && s_coercion_active_mutation_id != 0)
        e.source_mutation_id = s_coercion_active_mutation_id;
    if (e.predicate_cond_node == 0 && s_coercion_active_predicate != 0)
        e.predicate_cond_node = s_coercion_active_predicate;

    // 3. Mutation log: prefer records targeting original_child / parent_id
    // (walk reverse = newest first). Also follow parent_mutation_id once for
    // composite / multi-delta root attribution.
    const auto& log = flat.all_mutations();
    if (e.source_mutation_id == 0 && !log.empty()) {
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            if (it->target_node == e.original_child || it->target_node == e.parent_id ||
                it->parent_id == e.parent_id ||
                (e.original_child != 0 && it->parent_id == e.original_child)) {
                e.source_mutation_id = it->mutation_id;
                if (e.predicate_cond_node == 0 && it->target_node != 0 &&
                    it->target_node != NULL_NODE) {
                    e.predicate_cond_node = static_cast<std::uint32_t>(it->target_node);
                }
                // Multi-delta: if this record has a parent mutation, prefer
                // the root when the entry still has no predicate.
                if (it->parent_mutation_id != 0 && e.predicate_cond_node == 0) {
                    for (const auto& r : log) {
                        if (r.mutation_id == it->parent_mutation_id && r.target_node != 0) {
                            e.predicate_cond_node = static_cast<std::uint32_t>(r.target_node);
                            break;
                        }
                    }
                }
                break;
            }
        }
        if (e.source_mutation_id == 0)
            e.source_mutation_id = log.back().mutation_id;
    }

    // 4. Hygiene tracker (MacroIntroduced / audit path)
    if (e.predicate_cond_node == 0 || e.source_mutation_id == 0) {
        const auto& hy = aura::core::provenance::g_provenance_tracker().last_hygiene;
        if (e.predicate_cond_node == 0 && hy.node_id != 0)
            e.predicate_cond_node = hy.node_id;
        if (e.source_mutation_id == 0 && hy.source_mutation_id != 0)
            e.source_mutation_id = hy.source_mutation_id;
    }

    // 5. Completeness: true complete never counts weak original_child mid.
    // Issue #2261: clear weak mid under all non-Off strategies (not only Full/
    // Strict) so Sampled cannot leave pretend MutationRecord ids on IR.
    if (is_weak_coercion_mutation_id(e)) {
        g_coercion_provenance_weak_id_total.fetch_add(1, std::memory_order_relaxed);
        // Always clear weak mid before return — never write as real provenance.
        e.source_mutation_id = 0;
        if (strict) {
            // Issue #2147 AC2: Strict/Full honesty path.
            g_coercion_provenance_strict_reject_weak_total.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const bool true_complete =
        e.predicate_cond_node != 0 && e.source_mutation_id != 0 && !is_weak_coercion_mutation_id(e);
    if (true_complete) {
        g_coercion_provenance_complete_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #2558: re-evaluate completeness SLO after complete sample
        // (may clear pressure when bp recovers; evaluate only breaches).
        evaluate_coercion_provenance_slo(coercion_provenance_completeness_bp(),
                                         aura::compiler::typed_audit::production_defaults_active());
        return true;
    }

    if (!recovery_mode) {
        g_coercion_provenance_miss_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #2102: escalate to Full/contextual audit on next boundary exit
        // when force_audit policy is on (default).
        if (force_audit_on_provenance_miss())
            note_provenance_miss_for_boundary();
        // Issue #2558: completeness SLO backstop — production Sampled hosts
        // that accumulate miss pressure arm force Full for next boundary.
        evaluate_coercion_provenance_slo(coercion_provenance_completeness_bp(),
                                         aura::compiler::typed_audit::production_defaults_active());
    }
    // Reject-on-miss: leave fields incomplete so apply can skip insert;
    // do not stamp sentinel (Agent re-infers with active_mutation_id).
    if (reject_apply_on_provenance_miss())
        return false;

    // Issue #2147 Phase B: Strict/Full — no forensic sentinel/weak pretend.
    if (strict) {
        return false;
    }

    // Issue #2261: Sampled (and any strategy != Off) — no weak mid and no
    // sentinel pretend on the entry. apply_coercion_map skips insert.
    // Production Sampled hosts must not ship CoercionNodes with fake mids.
    using aura::compiler::typed_audit::AuditStrategy;
    using aura::compiler::typed_audit::get_strategy;
    if (get_strategy() != AuditStrategy::Off) {
        // Leave incomplete; clear residual weak mid (already cleared above).
        if (is_weak_coercion_mutation_id(e))
            e.source_mutation_id = 0;
        return false;
    }

    // Off + soft-only (tests / AURA_SANDBOX=off iterative typecheck):
    // optional sentinel for diagnostics only — never write weak mid.
    if (e.predicate_cond_node == 0) {
        const auto low = static_cast<std::uint32_t>(e.original_child & 0xFFFFu);
        e.predicate_cond_node = kCoercionProvenanceSentinelBase | (low == 0 ? 1u : low);
        g_coercion_provenance_sentinel_total.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #2261 AC3: never stamp weak mid under Off soft path either.
    if (e.source_mutation_id == 0 || is_weak_coercion_mutation_id(e)) {
        if (is_weak_coercion_mutation_id(e))
            g_coercion_provenance_weak_id_total.fetch_add(1, std::memory_order_relaxed);
        e.source_mutation_id = 0;
    }
    return false;
}

// Issue #2261: skip CoercionNode insert when provenance is incomplete and
// either production reject-on-miss is on OR strategy is not Off (Sampled+).
[[nodiscard]] inline bool should_skip_coercion_insert_on_incomplete() noexcept {
    if (reject_apply_on_provenance_miss())
        return true;
    using aura::compiler::typed_audit::AuditStrategy;
    using aura::compiler::typed_audit::get_strategy;
    return get_strategy() != AuditStrategy::Off;
}

// Issue #2561: cheap Soft/Sampled recovery for incomplete blame/provenance
// on a mutation mid's dirty cone (recent log entries only). Re-walks
// fill_coercion_provenance_chain in recovery_mode and re-stamps dual fields
// onto the node provenance column when complete. Returns true if at least
// one site recovered both fields. Zero work when mid==0 or log empty.
// Caps scan to kBlameSoftRecoverMaxSites (dirty-cone only; not full workspace).
inline constexpr std::size_t kBlameSoftRecoverMaxSites = 32;

export [[nodiscard]] inline bool try_recover_blame_chain_soft(aura::ast::FlatAST& flat,
                                                              std::uint64_t mid) noexcept {
    if (mid == 0)
        return false;
    const auto& log = flat.all_mutations();
    if (log.empty())
        return false;
    bool any_ok = false;
    std::size_t scanned = 0;
    for (auto it = log.rbegin(); it != log.rend() && scanned < kBlameSoftRecoverMaxSites; ++it) {
        if (it->mutation_id != mid && it->parent_mutation_id != mid)
            continue;
        ++scanned;
        if (it->target_node == 0 || it->target_node >= flat.size())
            continue;
        // Prefer existing provenance column; else soft-attribute to target
        // (same as fill log step 3) so dual fields can complete from TLS mid.
        std::uint32_t pred = flat.provenance(it->target_node);
        if (pred == 0)
            pred = static_cast<std::uint32_t>(it->target_node);
        set_coercion_active_mutation_context(mid, pred);
        CoercionEntry e{};
        e.parent_id = static_cast<std::uint32_t>(it->parent_id);
        e.original_child = static_cast<std::uint32_t>(it->target_node);
        e.source_mutation_id = 0;
        e.predicate_cond_node = 0;
        // recovery_mode: no miss/SLO force bumps (AC2 complete path still
        // uses normal fill; this is Soft re-walk only).
        bool ok = fill_coercion_provenance_chain(flat, e, /*recovery_mode=*/true);
        if (!ok && mid != 0 && pred != 0) {
            // Log-sourced MutationRecord mid is authoritative. #2261 weak-id
            // detection treats mid==original_child as forensic placeholder;
            // Soft recover must not drop real log mids on that collision.
            e.source_mutation_id = mid;
            e.predicate_cond_node = pred;
            ok = true;
        }
        if (ok) {
            if (e.predicate_cond_node != 0)
                flat.set_provenance(it->target_node, e.predicate_cond_node);
            any_ok = true;
        }
    }
    clear_coercion_active_mutation_context();
    return any_ok;
}

// Issue #2561: Soft/Sampled boundary helper — try recover; on fail optionally
// arm one-shot Full sample escalate (AURA_BLAME_SOFT_ESCALATE=1 or
// production_defaults). Soft default remains observe-only (AC3). Returns true
// if recovered (caller may clear force). Does not flip global strategy and
// does not hard-reject (#2221 is Full/production commit path).
export [[nodiscard]] inline bool
maybe_soft_recover_or_escalate_blame(aura::ast::FlatAST& flat, std::uint64_t mid,
                                     bool had_miss_signal) noexcept {
    if (!had_miss_signal || mid == 0)
        return false; // AC2: complete / no miss → zero recover/escalate
    using aura::compiler::typed_audit::AuditStrategy;
    using aura::compiler::typed_audit::get_strategy;
    const auto strat = get_strategy();
    // Soft/Sampled only — Full/Off leave existing hard-gate / observe paths.
    if (strat == AuditStrategy::Full || strat == AuditStrategy::Off)
        return false;
    if (try_recover_blame_chain_soft(flat, mid)) {
        g_blame_soft_recover_total.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_blame_soft_recover_fail_total.fetch_add(1, std::memory_order_relaxed);
    // Escalate one Full/contextual sample when env or production defaults.
    // Soft unset env → recover-only observe (AC3); no silent miss counters:
    // fail_total already bumped.
    if (blame_soft_escalate_enabled() ||
        aura::compiler::typed_audit::production_defaults_active()) {
        g_blame_soft_escalate_total.fetch_add(1, std::memory_order_relaxed);
        note_blame_soft_escalate_for_boundary();
    }
    return false;
}

// ── CoercionMap — accumulated coercion intent ────────────
//
// Collected during type checking, applied as a single explicit
// pass before lowering. Cheap to copy (just a vector of
// trivially-copyable entries), cheap to clear, safe to pass
// across module boundaries.
export class CoercionMap {
public:
    void add(aura::ast::NodeId parent, std::uint32_t child_index, aura::ast::NodeId original_child,
             std::uint32_t type_tag, std::uint32_t type_id, std::uint32_t src_line,
             std::uint32_t src_col) {
        // Issue #2512: stamp TLS active mid/pred at deferred-add (fast-path
        // completeness). Bare 6-arg add used when engine had no local context.
        CoercionEntry e{static_cast<std::uint32_t>(parent),
                        child_index,
                        static_cast<std::uint32_t>(original_child),
                        type_tag,
                        type_id,
                        src_line,
                        src_col,
                        0,
                        0,
                        0};
        (void)stamp_coercion_entry_from_active_context(e);
        entries_.push_back(e);
    }

    // Issue #537: overload with occurrence-narrowing provenance.
    // Issue #2512: still fills zero fields from TLS; never overwrites
    // explicit non-zero stamps (AC2).
    void add(aura::ast::NodeId parent, std::uint32_t child_index, aura::ast::NodeId original_child,
             std::uint32_t type_tag, std::uint32_t type_id, std::uint32_t src_line,
             std::uint32_t src_col, std::uint32_t predicate_cond_node,
             std::uint64_t source_mutation_id, std::uint32_t narrow_evidence = 0) {
        CoercionEntry e{static_cast<std::uint32_t>(parent),
                        child_index,
                        static_cast<std::uint32_t>(original_child),
                        type_tag,
                        type_id,
                        src_line,
                        src_col,
                        predicate_cond_node,
                        source_mutation_id,
                        narrow_evidence};
        (void)stamp_coercion_entry_from_active_context(e);
        entries_.push_back(e);
    }

    const std::vector<CoercionEntry>& entries() const { return entries_; }
    std::size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    void clear() {
        entries_.clear();
        eliminated_count_ = 0;
    }

    // Merge another map's entries into this one. Order is
    // preserved (other entries appended after this map's).
    void merge(const CoercionMap& other) {
        entries_.insert(entries_.end(), other.entries_.begin(), other.entries_.end());
    }

    // Issue #1425: count of identity coercions elided by
    // apply_coercion_map (not inserted as CoercionNodes).
    // Reset on clear(); bumped by mark_eliminated().
    [[nodiscard]] std::size_t eliminated_count() const noexcept { return eliminated_count_; }
    void mark_eliminated(std::size_t n = 1) noexcept { eliminated_count_ += n; }

private:
    std::vector<CoercionEntry> entries_;
    std::size_t eliminated_count_ = 0;
};

// Issue #1425: stats from apply_coercion_map with identity elision.
// `eliminated` maps to dead_coercion_eliminated metrics when the
// caller integrates (AST-level pre-IR win, complementary to
// DeadCoercionEliminationPass on IR CastOps).
export struct DeadCoercionAstStats {
    std::size_t applied = 0;       // CoercionNodes actually inserted
    std::size_t eliminated = 0;    // identity coercions skipped
    std::size_t kept = 0;          // alias of applied (non-identity)
    std::size_t skipped_stale = 0; // parent slot already rewritten / missing
};

// ── apply_coercion_map — the one explicit AST-mutating pass ───
//
// Walks the CoercionMap and, for each entry, calls
// `flat.add_coercion(original_child, type_tag, type_id)`,
// copies the source location, and rewrites the parent's
// `child_index` reference to point to the new CoercionNode.
//
// If a parent slot already points to something other than
// `original_child` (e.g. a previous apply already ran, or
// another pass mutated the tree), the entry is skipped — this
// keeps the pass idempotent and safe to call multiple times.
//
// Issue #1425: identity elision — when the original child
// already carries `type_id == entry.type_id` (non-zero), the
// CoercionNode would lower to a no-op CastOp. Skip insertion
// entirely (defense-in-depth with IR DeadCoercionEliminationPass).
//
// Returns the number of entries actually applied (rest are
// skipped or elided). When `stats_out` is non-null, fills
// applied / eliminated / skipped_stale. When `map_mut` is
// non-null, bumps map_mut->mark_eliminated for identity skips.
export std::size_t apply_coercion_map(aura::ast::FlatAST& flat, const CoercionMap& map,
                                      DeadCoercionAstStats* stats_out = nullptr,
                                      CoercionMap* map_mut = nullptr) {
    DeadCoercionAstStats local_stats;
    auto& s = stats_out ? *stats_out : local_stats;
    s = {};

    for (const auto& e_in : map.entries()) {
        CoercionEntry e = e_in;

        // Issue #1425 / #1925: identity coercion — child already has the
        // target type stamped (post-infer). Do not insert a
        // CoercionNode; the IR path would only produce a dead CastOp.
        // Also elide Dynamic-target tags (type_tag==3): CastOp default
        // is passthrough; narrow_evidence-only identity when types match.
        if (e.type_id != 0 && flat.type_id(e.original_child) == e.type_id) {
            ++s.eliminated;
            if (map_mut)
                map_mut->mark_eliminated();
            // Issue #2025: AST elision feeds layered dead-coercion metrics.
            g_dead_coercion_ast_elided_total.fetch_add(1, std::memory_order_relaxed);
            // Issue #2611: evidence-backed AST identity elision → deopt meta
            // (mid from coercion provenance; no stamp when evidence==0).
            if (e.narrow_evidence != 0) {
                const auto site =
                    dce_deopt::make_site_key(0, static_cast<std::uint32_t>(e.original_child),
                                             static_cast<std::uint32_t>(e.parent_id));
                dce_deopt::stamp_elided_cast_deopt_meta(site, e.source_mutation_id,
                                                        e.narrow_evidence, e.type_tag);
            }
            continue;
        }

        // Issue #1925: Dynamic passthrough tag (3) with no meaningful
        // runtime check — skip CoercionNode insertion.
        if (e.type_tag == 3) {
            ++s.eliminated;
            if (map_mut)
                map_mut->mark_eliminated();
            g_dead_coercion_ast_elided_total.fetch_add(1, std::memory_order_relaxed);
            // Issue #2611: Dynamic-tag elision with evidence also stamps meta.
            if (e.narrow_evidence != 0) {
                const auto site =
                    dce_deopt::make_site_key(0, static_cast<std::uint32_t>(e.original_child),
                                             static_cast<std::uint32_t>(e.parent_id));
                dce_deopt::stamp_elided_cast_deopt_meta(site, e.source_mutation_id,
                                                        e.narrow_evidence, e.type_tag);
            }
            continue;
        }

        // Issue #1873 / #2024 / #2102 / #2261: full provenance chain recovery.
        // Issue #2562: dual-require (production / Full / env) → drop incomplete
        // dual (both pred+mid non-weak) before insert; prefer drop over weak/
        // sentinel stamp. Completeness_bp / miss totals remain authority.
        // Issue #2317: Sampled + incomplete + NOT dual-require + !reject
        // → INSERT (with force-audit). Reject-on-miss + Full + Strict still
        // skip per existing #2147 / #2261 rules.
        const bool prov_complete = fill_coercion_provenance_chain(flat, e);
        if (!prov_complete) {
            using aura::compiler::typed_audit::AuditStrategy;
            using aura::compiler::typed_audit::get_strategy;
            // Do not shadow outer stats ref `s` (DeadCoercionAstStats).
            const auto strat = get_strategy();
            // Issue #2562: dual-field require-or-drop (before #2317 soft insert).
            if (coercion_dual_require_active()) {
                g_coercion_dual_require_drop_total.fetch_add(1, std::memory_order_relaxed);
                g_coercion_provenance_miss_reject_total.fetch_add(1, std::memory_order_relaxed);
                if (strat == AuditStrategy::Sampled)
                    g_coercion_provenance_sampled_reject_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                ++s.skipped_stale;
                continue; // never insert incomplete dual under dual-require
            }
            // Issue #2317: Sampled + !reject → INSERT (not skip). Soft default.
            if (strat == AuditStrategy::Sampled && !reject_apply_on_provenance_miss()) {
                g_coercion_sampled_insert_incomplete_total.fetch_add(1, std::memory_order_relaxed);
                // Fall through to insert (CoercionNode exists for
                // lowering; force-audit triggered by fill_coercion_provenance_chain).
            } else if (should_skip_coercion_insert_on_incomplete()) {
                // existing skip path (reject-on-miss OR non-Off strategy)
                g_coercion_provenance_miss_reject_total.fetch_add(1, std::memory_order_relaxed);
                if (strat == AuditStrategy::Sampled)
                    g_coercion_provenance_sampled_reject_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                ++s.skipped_stale;
                continue;
            }
            // For Off soft path (with incomplete provenance): falls through
            // to insert (existing behavior; weak mid cleared per #2261).
        }

        // Locate the parent and confirm it still points at the
        // original child we recorded. If it doesn't (e.g. this
        // pass already ran, or another mutator touched the
        // tree), skip the entry — idempotency.
        if (e.parent_id == aura::ast::NULL_NODE) {
            // Top-level expression: there is no parent to
            // rewrite. This case is rare (top-level
            // coercions don't need parent rewrite), but we
            // still need the CoercionNode for the IR
            // lowering to see it. Insert it as a free node
            // (parented to itself's children — already
            // handled by add_coercion).
            auto coercion_id = flat.add_coercion(e.original_child, e.type_tag, e.type_id);
            flat.set_loc(coercion_id, e.src_line, e.src_col);
            if (e.narrow_evidence != 0)
                flat.set_float(coercion_id, static_cast<double>(e.narrow_evidence));
            // Issue #1873 / #2024 / #2261: stamp predicate (or sentinel under
            // Off soft only). Never write weak mid as provenance column.
            if (e.predicate_cond_node != 0)
                flat.set_provenance(coercion_id, e.predicate_cond_node);
            else if (e.source_mutation_id != 0 && !is_weak_coercion_mutation_id(e))
                flat.set_provenance(coercion_id,
                                    static_cast<std::uint32_t>(e.source_mutation_id & 0xFFFFFFFFu));
            ++s.applied;
            ++s.kept;
            continue;
        }

        auto parent_v = flat.get(e.parent_id);
        if (e.child_index >= parent_v.children.size()) {
            // Stale entry — slot no longer exists. Skip.
            ++s.skipped_stale;
            continue;
        }
        if (parent_v.child(e.child_index) != e.original_child) {
            // Already applied, or another pass rewrote. Skip.
            ++s.skipped_stale;
            continue;
        }

        // Build the CoercionNode wrapping the original child.
        auto coercion_id = flat.add_coercion(e.original_child, e.type_tag, e.type_id);
        flat.set_loc(coercion_id, e.src_line, e.src_col);
        // Issue #691 / #1873 / #2024 / #2261: stamp narrowing evidence +
        // recovered predicate (never weak mid as provenance column).
        if (e.narrow_evidence != 0)
            flat.set_float(coercion_id, static_cast<double>(e.narrow_evidence));
        if (e.predicate_cond_node != 0)
            flat.set_provenance(coercion_id, e.predicate_cond_node);
        else if (e.source_mutation_id != 0 && !is_weak_coercion_mutation_id(e))
            flat.set_provenance(coercion_id,
                                static_cast<std::uint32_t>(e.source_mutation_id & 0xFFFFFFFFu));
        // Rewrite the parent's child_index to point at the
        // new CoercionNode.
        flat.set_child(e.parent_id, e.child_index, coercion_id);
        ++s.applied;
        ++s.kept;
    }
    return s.applied;
}

} // namespace aura::compiler
