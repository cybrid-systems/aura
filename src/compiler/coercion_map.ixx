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
import aura.compiler.dirty_propagation; // Issue #3065: remirror elim'd nodes into type cone

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
// Issue #2648: Soft evidence-loss bp + one-shot Full arm on boundary.
export using ::aura::compiler::kCoercionEvidenceLossIssue;
export using ::aura::compiler::kCoercionEvidenceLossBpDefault;
export using ::aura::compiler::g_coercion_evidence_loss_threshold_bp;
export using ::aura::compiler::g_coercion_evidence_loss_breach_total;
export using ::aura::compiler::g_coercion_evidence_loss_force_armed_total;
export using ::aura::compiler::g_coercion_evidence_loss_force_consumed_total;
export using ::aura::compiler::g_coercion_evidence_loss_wired;
export using ::aura::compiler::coercion_evidence_loss_threshold_bp;
export using ::aura::compiler::set_coercion_evidence_loss_threshold_bp_for_test;
export using ::aura::compiler::coercion_evidence_loss_pressure;
export using ::aura::compiler::evaluate_coercion_evidence_loss_slo;
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
// Issue #2317 / #2620: Sampled incomplete-insert canary counter.
// Default OFF (#2620): incomplete dual never inserts under strategy!=Off.
// Restored only when AURA_COERCION_SAMPLED_INCOMPLETE_INSERT=1 (canary).
// Distinct from coercion_provenance_sampled_reject_total which counts SKIPS.
export inline std::atomic<std::uint64_t> g_coercion_sampled_insert_incomplete_total{0};
// Issue #2620: Soft/Sampled skipped incomplete insert (observe + force-Full arm).
// Additive; does not rename #2317 / #2562 counters.
export inline std::atomic<std::uint64_t> g_coercion_soft_incomplete_skip_total{0};
export inline std::atomic<std::uint32_t> g_coercion_unify_incomplete_skip_wired{1};
export inline constexpr int kCoercionUnifyIncompleteSkipIssue = 2620;
export inline std::atomic<std::uint32_t> g_coercion_provenance_ban_weak_ir_wired{1};
// Issue #2512: times CoercionMap::add stamped TLS active mid/pred into a
// zero-field entry (deferred-add completeness). Completeness_bp remains
// the authority for apply-time quality.
export inline std::atomic<std::uint64_t> g_coercion_stamp_at_add_total{0};
export inline std::atomic<std::uint32_t> g_coercion_stamp_at_add_wired{1};
// Issue #2991: high-frequency mutate blame completeness.
// complete = session mid stamped on deferred add / insert.
// missing = session mid was zero on entry and had to be force-stamped
//   (or still missing after resolve).
// epoch-restamp = wrong-epoch mid (log.back / leftover narrowing) replaced
//   with the active mutate session.
export inline std::atomic<std::uint64_t> g_coercion_blame_chain_complete_total{0};
export inline std::atomic<std::uint64_t> g_coercion_blame_missing_total{0};
export inline std::atomic<std::uint64_t> g_coercion_blame_epoch_restamp_total{0};
export inline std::atomic<std::uint32_t> g_coercion_blame_hf_mutate_wired{1};
export inline constexpr int kCoercionBlameHfMutateIssue = 2991;
// Issue #3046: residual of #2991 — non-zero session always stamps mid
// (including over weak leftover / prior-epoch NarrowingRecords). CastOp
// hot residual is the density-policy face in castop_density_policy.hh.
export inline constexpr int kCoercionBlameHfLagIssue = 3046;
export inline std::atomic<std::uint64_t> g_coercion_blame_session_force_total{0};
export inline std::atomic<std::uint64_t> g_coercion_blame_stale_narrowing_drop_total{0};
export inline std::atomic<std::uint32_t> g_coercion_blame_hf_lag_wired{1};
// Issue #2562: dual-require drop counter is process-wide in policy.hh
// (g_coercion_dual_require_drop_total); re-exported above.
// Issue #2025: AST-level identity elision count (apply_coercion_map) for
// layered zero-overhead synergy with IR DeadCoercionEliminationPass.
// Issue #2282: combined on query:dead-coercion-layered-stats as the
// `ast-elided` component (see optimization_passes.ixx for ir-elided + dirty-cone-skips).
export inline std::atomic<std::uint64_t> g_dead_coercion_ast_elided_total{0};
// Issue #2674: AST elision WITH narrow_evidence (subset of total). Layered
// coherence invariant compares this counter against IR narrow evidence hits +
// deopt-meta stamps (the union must cover every evidence-backed AST elide —
// any surplus = layered stats diverged under typed_mutate → lower → JIT).
// Zero cost on every AST elision with narrow_evidence == 0.
export inline std::atomic<std::uint64_t> g_dead_coercion_ast_elided_with_evidence_total{0};
// Issue #2674: layered-evidence-coherence diverge counter. Bumped when
// g_dead_coercion_ast_elided_with_evidence_total >
//   dead_coercion_ir_narrow_evidence_hits + dce_deopt_meta_stamped_total
// (i.e. evidence-backed AST elide without matching IR narrow hit or deopt-meta
// stamp). Soft/Sampled: observe-only (no hard-reject of mutate by default).
// Full/Production: optional escalate via fidelity-health note. No abort path.
export inline std::atomic<std::uint64_t> g_layered_evidence_diverge_total{0};

// Issue #2719 / #2912 / #2979: Full/production hard gate on layered evidence
// diverge (#2674 residual). Soft vs production table:
//
//   Path                          | Behavior
//   ------------------------------|------------------------------------------
//   Soft + diverge                | observe g_layered_evidence_diverge_total only
//   production / Full + diverge   | arm force-Full pending (#2719) + next
//                                 | *outermost Phase-5* consume → one-shot
//                                 | Full invariant sample (#2979; #2912
//                                 | consume-in-exit could be stolen by nested
//                                 | or dropped by Sampled recover)
//   production + HARD env + diverge | also arm hard-reject-pending; Phase-5
//                                 | consume stamps force_reason (default is
//                                 | still force-Full sample, not commit reject)
//   no coercion / no diverge      | zero cost (one Phase-5 exchange no-op)
//
// When diverge is observed under production_defaults_active() || Full:
//   - (default arm) bump force-armed + set force-full-pending so the *next*
//     MutationBoundary consumes it and forces a Full invariant sample
//     (not a hard-reject of the current commit by default).
//   - (opt-in env AURA_LAYERED_COERCION_DIVERGE_HARD=1) also arm hard-reject.
// Soft/Sampled: observe-only (#2674 — no force-armed bump, no flag set).
export inline std::atomic<std::uint64_t> g_layered_evidence_diverge_force_armed_total{0};
export inline std::atomic<std::uint64_t> g_layered_evidence_diverge_hard_reject_total{0};
export inline std::atomic<std::uint32_t> g_layered_evidence_diverge_force_full_pending{0};
export inline std::atomic<std::uint32_t> g_layered_evidence_diverge_hard_reject_pending{0};
// Issue #2912: one-shot consume totals (mirrors coercion_prov_slo_force_consumed).
export inline std::atomic<std::uint64_t> g_layered_evidence_diverge_force_consumed_total{0};
export inline std::atomic<std::uint64_t> g_layered_evidence_diverge_hard_reject_consumed_total{0};
// Issue #2979: Phase-5 outermost actually ran a Full invariant sample
// after consume (distinct from consume-total, which can fire without a
// sample if only the helper is unit-tested).
export inline std::atomic<std::uint64_t> g_layered_evidence_diverge_force_full_sample_total{0};

// Env var helper for #2719 hard-reject arm. Reads
// AURA_LAYERED_COERCION_DIVERGE_HARD (any non-zero value enables).
// Cached once at first call (env vars don't change at runtime in
// production — matches existing pattern in this module).
export [[nodiscard]] inline bool layered_diverge_hard_enabled() noexcept {
    static const bool enabled = []() noexcept -> bool {
        const char* e = std::getenv("AURA_LAYERED_COERCION_DIVERGE_HARD");
        if (e && *e) {
            char* end = nullptr;
            const auto n = std::strtoull(e, &end, 10);
            if (end != e && n > 0)
                return true;
        }
        return false;
    }();
    return enabled;
}

// Accessors + test resets for #2719 / #2912 surface.
export [[nodiscard]] inline std::uint64_t
layered_evidence_diverge_force_armed_total_v_read() noexcept {
    return g_layered_evidence_diverge_force_armed_total.load(std::memory_order_relaxed);
}
export inline void reset_layered_evidence_diverge_force_armed_total_for_test() noexcept {
    g_layered_evidence_diverge_force_armed_total.store(0, std::memory_order_relaxed);
}
export [[nodiscard]] inline std::uint64_t
layered_evidence_diverge_hard_reject_total_v_read() noexcept {
    return g_layered_evidence_diverge_hard_reject_total.load(std::memory_order_relaxed);
}
export inline void reset_layered_evidence_diverge_hard_reject_total_for_test() noexcept {
    g_layered_evidence_diverge_hard_reject_total.store(0, std::memory_order_relaxed);
}
export [[nodiscard]] inline bool layered_evidence_diverge_force_full_pending() noexcept {
    return g_layered_evidence_diverge_force_full_pending.load(std::memory_order_relaxed) != 0;
}
export inline void clear_layered_evidence_diverge_force_full_pending_for_test() noexcept {
    g_layered_evidence_diverge_force_full_pending.store(0, std::memory_order_relaxed);
}
export [[nodiscard]] inline bool layered_evidence_diverge_hard_reject_pending() noexcept {
    return g_layered_evidence_diverge_hard_reject_pending.load(std::memory_order_relaxed) != 0;
}
export inline void clear_layered_evidence_diverge_hard_reject_pending_for_test() noexcept {
    g_layered_evidence_diverge_hard_reject_pending.store(0, std::memory_order_relaxed);
}

// Issue #2912: one-shot consume of force-Full pending (next outermost
// boundary exit). Returns true if pending was set; clears flag and bumps
// force_consumed. Quiet path: exchange 0→0, no counter mutation.
export [[nodiscard]] inline bool consume_layered_evidence_diverge_force_full() noexcept {
    const auto prev =
        g_layered_evidence_diverge_force_full_pending.exchange(0, std::memory_order_acq_rel);
    if (prev != 0) {
        g_layered_evidence_diverge_force_consumed_total.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

// Issue #2912: one-shot consume of hard-reject pending (opt-in HARD env).
// Agents / boundary use this to reject with force_reason
// "layered-evidence-diverge". Quiet path: zero cost.
export [[nodiscard]] inline bool consume_layered_evidence_diverge_hard_reject() noexcept {
    const auto prev =
        g_layered_evidence_diverge_hard_reject_pending.exchange(0, std::memory_order_acq_rel);
    if (prev != 0) {
        g_layered_evidence_diverge_hard_reject_consumed_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
        return true;
    }
    return false;
}

export [[nodiscard]] inline std::uint64_t
layered_evidence_diverge_force_consumed_total_v_read() noexcept {
    return g_layered_evidence_diverge_force_consumed_total.load(std::memory_order_relaxed);
}
export inline void reset_layered_evidence_diverge_force_consumed_total_for_test() noexcept {
    g_layered_evidence_diverge_force_consumed_total.store(0, std::memory_order_relaxed);
}
export [[nodiscard]] inline std::uint64_t
layered_evidence_diverge_hard_reject_consumed_total_v_read() noexcept {
    return g_layered_evidence_diverge_hard_reject_consumed_total.load(std::memory_order_relaxed);
}
export inline void reset_layered_evidence_diverge_hard_reject_consumed_total_for_test() noexcept {
    g_layered_evidence_diverge_hard_reject_consumed_total.store(0, std::memory_order_relaxed);
}

// Issue #2979: one-shot Full invariant sample after Phase-5 consume.
export [[nodiscard]] inline std::uint64_t
layered_evidence_diverge_force_full_sample_total_v_read() noexcept {
    return g_layered_evidence_diverge_force_full_sample_total.load(std::memory_order_relaxed);
}
export inline void reset_layered_evidence_diverge_force_full_sample_total_for_test() noexcept {
    g_layered_evidence_diverge_force_full_sample_total.store(0, std::memory_order_relaxed);
}
export inline void note_layered_evidence_diverge_force_full_sample() noexcept {
    g_layered_evidence_diverge_force_full_sample_total.fetch_add(1, std::memory_order_relaxed);
}

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

// Issue #2648: Soft evidence-loss rate as basis points (0–10000).
// loss_bp = soft_incomplete_skip / (skip + complete + ast_elided) * 10000.
// "applied_or_elided_complete" ≈ provenance complete samples + identity elides.
// Zero samples → 0 (healthy vacuous: no observed loss; clean hosts never arm).
// Contrast completeness_bp vacuous 10000 (high=good). Higher loss is worse.
export [[nodiscard]] inline std::uint64_t coercion_evidence_loss_bp() noexcept {
    const auto skip = g_coercion_soft_incomplete_skip_total.load(std::memory_order_relaxed);
    const auto complete = g_coercion_provenance_complete_total.load(std::memory_order_relaxed);
    const auto elided = g_dead_coercion_ast_elided_total.load(std::memory_order_relaxed);
    const auto denom = skip + complete + elided;
    return denom > 0 ? (skip * 10000u) / denom : 0u;
}

// Issue #2674: layered evidence-coherence invariant (refine #2645 — was
// test/linter-only; #2674 adds production-path consistency check + Agent-
// visible query surface). For evidence-backed AST elisions in a
// MutationBoundary window:
//   ast_elided_with_evidence <= ir_narrow_evidence_hits + deopt_meta_stamps
// When the AST-elision-with-evidence counter exceeds the union of IR narrow
// evidence hits + deopt-meta stamps, the layered stats have silently diverged
// under typed_mutate → lower → JIT. Soft/Sampled: observe the diverge counter
// only. Full/Production: optional fidelity-health note (no hard-reject of
// mutate by default — observability first per Issue #2674 AC5).
//
// Issue #2674 AC4: zero cost when no evidence path — pure atomic loads, no
// counter mutation when invariant holds. Sampled check runs on a coarse
// boundary (MutationBoundaryGuard Phase 5 outermost exit, aka
// MutationBoundary outermost exit) to amortize cost.
//
// IR narrow evidence hits live in opt_registry::dead_coercion_ir_narrow_evidence_hits
// (optimization_passes module). coercion_map.ixx cannot import that module
// (cyclic import graph: optimization_passes ↔ pass_impls ↔ coercion_map
// chain), so the IR counter is passed in by the caller —
// evaluator_mutation_boundary.cpp loads it from opt_registry (already
// reachable from .cpp impl units) and passes the snapshot here. Caller-side
// snapshot is fine because the invariant only cares about monotonic divergence
// (ir_narrow + meta_stamps are monotonically non-decreasing per process
// lifetime; a snapshot at boundary exit is consistent with the per-window
// coherence check).
// Issue #2719: return diverge_delta (the amount by which the AST
// evidence-backed elision counter exceeded the union of IR narrow + deopt
// meta stamps) so the boundary call site can arm the Full/production hard
// gate (#2674 was observe-only and returned void — additive: existing
// callers can ignore the return). Zero on invariant holds (no diverge).
export inline std::uint64_t
check_layered_evidence_coherence(std::uint64_t ir_narrow_evidence_hits_external) noexcept {
    using namespace ::aura::compiler::dce_deopt;
    const auto ast_with_ev =
        g_dead_coercion_ast_elided_with_evidence_total.load(std::memory_order_relaxed);
    const auto meta_stamps = dce_deopt_meta_stamped_total.load(std::memory_order_relaxed);
    // Invariant: ast_elision_with_evidence <= ir_narrow + meta_stamps.
    // ir_narrow + meta_stamps are monotonically non-decreasing per process
    // lifetime; ast_with_ev can drift higher only if evidence-backed AST
    // elisions happen without matching IR narrow / meta coverage.
    if (ast_with_ev > ir_narrow_evidence_hits_external + meta_stamps) {
        const auto diverge_delta = ast_with_ev - (ir_narrow_evidence_hits_external + meta_stamps);
        g_layered_evidence_diverge_total.fetch_add(diverge_delta, std::memory_order_relaxed);
        return diverge_delta;
    }
    return 0;
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

// Issue #2991: first-class provenance for deferred add. Session mid
// (explicit / engine / TLS) always wins over mutation-log.back() and
// leftover NarrowingRecords from earlier mutates.
export struct DeferredCoercionProvenanceIn {
    std::uint64_t explicit_mid = 0;
    std::uint32_t explicit_pred = 0;
    std::uint32_t explicit_narrow = 0;
    std::uint64_t engine_active_mid = 0;
    std::uint32_t engine_active_pred = 0;
    std::uint32_t engine_last_narrow = 0;
    std::uint64_t log_back_mid = 0;
    std::uint64_t narrowing_mid = 0;
    std::uint32_t narrowing_pred = 0;
    std::uint32_t narrowing_evidence = 0;
};

export [[nodiscard]] inline std::uint64_t
deferred_coercion_session_mid(const DeferredCoercionProvenanceIn& in) noexcept {
    if (in.explicit_mid != 0)
        return in.explicit_mid;
    if (in.engine_active_mid != 0)
        return in.engine_active_mid;
    if (s_coercion_active_mutation_id != 0)
        return s_coercion_active_mutation_id;
    return 0;
}

export inline void resolve_deferred_coercion_provenance(CoercionEntry& e,
                                                        const DeferredCoercionProvenanceIn& in) {
    const auto session = deferred_coercion_session_mid(in);
    // Issue #3046: non-zero session always stamps source_mutation_id.
    // Leftover / weak / log.back() / prior-epoch NarrowingRecords lose.
    // Quiet: session == 0 → fill zeros only (identity / no-mutate path).
    if (session != 0) {
        if (e.source_mutation_id != 0 && e.source_mutation_id != session)
            g_coercion_blame_epoch_restamp_total.fetch_add(1, std::memory_order_relaxed);
        e.source_mutation_id = session;
        g_coercion_blame_session_force_total.fetch_add(1, std::memory_order_relaxed);
    } else if (e.source_mutation_id == 0) {
        if (in.narrowing_mid != 0)
            e.source_mutation_id = in.narrowing_mid;
        else if (in.log_back_mid != 0)
            e.source_mutation_id = in.log_back_mid;
    }
    if (e.predicate_cond_node == 0) {
        if (in.explicit_pred != 0)
            e.predicate_cond_node = in.explicit_pred;
        else if (in.engine_active_pred != 0)
            e.predicate_cond_node = in.engine_active_pred;
        else if (s_coercion_active_predicate != 0)
            e.predicate_cond_node = s_coercion_active_predicate;
        else if (in.narrowing_pred != 0)
            e.predicate_cond_node = in.narrowing_pred;
    }
    if (e.narrow_evidence == 0) {
        if (in.explicit_narrow != 0)
            e.narrow_evidence = in.explicit_narrow;
        else if (in.engine_last_narrow != 0)
            e.narrow_evidence = in.engine_last_narrow;
        else if (in.narrowing_evidence != 0)
            e.narrow_evidence = in.narrowing_evidence;
    }
    if (session != 0 && e.source_mutation_id != 0)
        g_coercion_blame_chain_complete_total.fetch_add(1, std::memory_order_relaxed);
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

// Issue #2620 / #2317 canary: AURA_COERCION_SAMPLED_INCOMPLETE_INSERT=1 restores
// legacy Sampled incomplete-insert (default off — unify Soft/prod proof surface).
[[nodiscard]] inline bool coercion_sampled_incomplete_insert_canary() noexcept {
    const char* e = std::getenv("AURA_COERCION_SAMPLED_INCOMPLETE_INSERT");
    return e != nullptr && e[0] == '1';
}

// Issue #2620: Soft/Sampled incomplete → observe + arm one-shot force Full
// (no hard-reject; production dual-require / reject-on-miss remain separate).
// Issue #2648: after skip bump, evaluate evidence-loss SLO so sustained Soft
// loss auto-arms the same pending channel when loss_bp >= threshold (Agents
// read single coercion-evidence-loss-bp; boundary guarantees one Full sample).
inline void arm_soft_incomplete_force_full_observe() noexcept {
    g_coercion_soft_incomplete_skip_total.fetch_add(1, std::memory_order_relaxed);
    g_coercion_prov_slo_observe_only_total.fetch_add(1, std::memory_order_relaxed);
    // Issue #2648 first: when loss_bp >= threshold, arm pending + dedicated
    // evidence-loss armed (must run before #2620 exchange so armed counts).
    evaluate_coercion_evidence_loss_slo(coercion_evidence_loss_bp());
    // #2620: always mark force-pending so Agents see Soft pressure immediately;
    // #2648 boundary Soft-drops Full unless loss_bp still breaches (or recover).
    const auto prev = g_coercion_prov_slo_force_full_pending.exchange(1, std::memory_order_acq_rel);
    if (prev == 0)
        g_coercion_prov_slo_force_armed_total.fetch_add(1, std::memory_order_relaxed);
    // Boundary force-audit channel (fill_coercion may already have noted).
    note_provenance_miss_for_boundary();
    note_blame_soft_escalate_for_boundary();
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

// Issue #3102: forward declarations for the per-boundary TLS tracker.
// apply_coercion_map (below) calls coerced_nodes_tracker_push at the
// end of its body; the definitions sit below the function. Without
// these forward decls the call is unresolved (gcc emits "was not
// declared in this scope"). Definitions are at the bottom of the file
// (after apply_coercion_map) where they have access to the TLS + the
// emit-set helpers.
// Decl/def must both be `export inline` so evaluator_mutation_boundary
// (and tests) can import them. Keep `export` first — GCC 16 rejects
// `[[nodiscard]] export`.
export inline void coerced_nodes_tracker_enter_boundary() noexcept;
export inline void coerced_nodes_tracker_exit_boundary() noexcept;
export inline void coerced_nodes_tracker_push(aura::compiler::dirty::NodeId nid) noexcept;
export [[nodiscard]] inline std::vector<aura::compiler::dirty::NodeId>
coerced_nodes_tracker_take() noexcept;
export [[nodiscard]] inline std::size_t coerced_nodes_tracker_size() noexcept;

// Issue #3106 follow-up: GCC 16.1.0 ICE on multi-line function signature
// + default arguments in C++20 module interface unit (14th consecutive
// coercion_map.ixx ICE per MEMORY). Compress the signature to a single
// line — the multi-line layout with trailing default-arg parameters
// corrupts the parser state, making GCC think `map_mut` is undeclared
// and treating `nullptr) {` as a compound expression initializer.
// Same fix pattern as the ubsan-smoke / asan-build gates; no semantic
// change (defaults preserved, linkage unchanged).
// clang-format off
export std::size_t apply_coercion_map(aura::ast::FlatAST& flat, const CoercionMap& map, DeadCoercionAstStats* stats_out = nullptr, CoercionMap* map_mut = nullptr) {
    // clang-format on
    DeadCoercionAstStats local_stats;
    auto& s = stats_out ? *stats_out : local_stats;
    s = {};

    // Issue #3065: persist AST-elided sites into the type∪IR cone under
    // production/Full so a remutate of the same node re-enters typecheck.
    // Soft/quiet: do not collect (zero extra dirty bits).
    const bool persist_elim_cone = aura::compiler::typed_audit::production_defaults_active() ||
                                   aura::compiler::typed_audit::get_strategy() ==
                                       aura::compiler::typed_audit::AuditStrategy::Full;
    std::vector<aura::compiler::dirty::NodeId> elim_ast;

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
            // Issue #2674: also bump ast-elided-with-evidence counter so the
            // layered coherence invariant (ast_with_evidence <= ir_narrow +
            // meta_stamps) can detect layered-stats divergence.
            if (e.narrow_evidence != 0) {
                g_dead_coercion_ast_elided_with_evidence_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                const auto site =
                    dce_deopt::make_site_key(0, static_cast<std::uint32_t>(e.original_child),
                                             static_cast<std::uint32_t>(e.parent_id));
                dce_deopt::stamp_elided_cast_deopt_meta(site, e.source_mutation_id,
                                                        e.narrow_evidence, e.type_tag);
            }
            if (persist_elim_cone) {
                elim_ast.push_back(e.original_child);
                if (e.parent_id != 0)
                    elim_ast.push_back(e.parent_id);
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
            // Issue #2674: same evidence-backed counter bump as identity path.
            if (e.narrow_evidence != 0) {
                g_dead_coercion_ast_elided_with_evidence_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                const auto site =
                    dce_deopt::make_site_key(0, static_cast<std::uint32_t>(e.original_child),
                                             static_cast<std::uint32_t>(e.parent_id));
                dce_deopt::stamp_elided_cast_deopt_meta(site, e.source_mutation_id,
                                                        e.narrow_evidence, e.type_tag);
            }
            if (persist_elim_cone) {
                elim_ast.push_back(e.original_child);
                if (e.parent_id != 0)
                    elim_ast.push_back(e.parent_id);
            }
            continue;
        }

        // Issue #1873 / #2024 / #2102 / #2261 / #2562 / #2620: provenance completeness
        // gate before CoercionNode insert. Decision table (strategy × complete):
        //
        //   │ Off      │ incomplete → INSERT (weak mid cleared; #2261)
        //   │ Sampled  │ incomplete → SKIP + observe + arm force-Full (#2620)
        //   │          │   canary AURA_COERCION_SAMPLED_INCOMPLETE_INSERT=1 →
        //   │          │   INSERT + g_coercion_sampled_insert_incomplete (#2317)
        //   │ Full     │ incomplete → SKIP (honest; dual-require when active)
        //   │ dual-req │ incomplete → DROP + dual_require_drop_total (#2562)
        //   │ reject   │ incomplete → SKIP + miss_reject (#2185)
        //
        // Unified contract (#2620 Phase A): incomplete dual provenance never
        // becomes executable IR under any non-Off strategy (default).
        const bool prov_complete = fill_coercion_provenance_chain(flat, e);
        // Issue #2991 / #3046: under a live TLS session, fill empty or
        // weak leftover mids. Do not clobber a non-weak mid already
        // stamped at add (explicit / engine) — TLS can be stale from a
        // prior mutate in-process (test_ir CS34).
        if (s_coercion_active_mutation_id != 0) {
            if (e.source_mutation_id == 0 || is_weak_coercion_mutation_id(e)) {
                if (e.source_mutation_id == 0)
                    g_coercion_blame_missing_total.fetch_add(1, std::memory_order_relaxed);
                else
                    g_coercion_blame_epoch_restamp_total.fetch_add(1, std::memory_order_relaxed);
                e.source_mutation_id = s_coercion_active_mutation_id;
                g_coercion_blame_session_force_total.fetch_add(1, std::memory_order_relaxed);
            }
            if (e.source_mutation_id != 0)
                g_coercion_blame_chain_complete_total.fetch_add(1, std::memory_order_relaxed);
        }
        if (!prov_complete) {
            using aura::compiler::typed_audit::AuditStrategy;
            using aura::compiler::typed_audit::get_strategy;
            // Do not shadow outer stats ref `s` (DeadCoercionAstStats).
            const auto strat = get_strategy();
            // Issue #2562: dual-field require-or-drop (production / Full / env).
            if (coercion_dual_require_active()) {
                g_coercion_dual_require_drop_total.fetch_add(1, std::memory_order_relaxed);
                g_coercion_provenance_miss_reject_total.fetch_add(1, std::memory_order_relaxed);
                if (strat == AuditStrategy::Sampled)
                    g_coercion_provenance_sampled_reject_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                ++s.skipped_stale;
                continue; // never insert incomplete dual under dual-require
            }
            // Issue #2620 / #2317 canary: only when env explicitly set, Sampled
            // may still INSERT incomplete (legacy canary). Default OFF.
            if (coercion_sampled_incomplete_insert_canary() && strat == AuditStrategy::Sampled &&
                !reject_apply_on_provenance_miss()) {
                g_coercion_sampled_insert_incomplete_total.fetch_add(1, std::memory_order_relaxed);
                // Fall through to insert (force-audit via fill_coercion).
            } else if (should_skip_coercion_insert_on_incomplete()) {
                // Issue #2620: Soft/Sampled/Full — never insert incomplete.
                // Soft/Sampled: observe + arm force-Full (zero hard-reject default).
                // reject-on-miss / Full: miss_reject path retained.
                // Always count as insert-reject (not commit hard-reject).
                g_coercion_provenance_miss_reject_total.fetch_add(1, std::memory_order_relaxed);
                if (strat == AuditStrategy::Sampled)
                    g_coercion_provenance_sampled_reject_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                // Soft/Sampled without reject-on-miss: observe + arm force-Full (#2620).
                if (strat == AuditStrategy::Sampled && !reject_apply_on_provenance_miss())
                    arm_soft_incomplete_force_full_observe();
                ++s.skipped_stale;
                continue; // Issue #2620: skip-insert branch (source-cite)
            }
            // Off soft path (incomplete): falls through to insert
            // (existing behavior; weak mid cleared per #2261).
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
    // Issue #3065: remirror elim'd nodes after the walk (union into last
    // type cone). Soft/empty → helper is a no-op.
    if (!elim_ast.empty())
        (void)aura::compiler::dirty::force_dead_coercion_elim_into_cone(elim_ast);
    // Issue #3102: AC1/AC2 — push coerced nodes into the per-boundary TLS
    // tracker (production/Full only). The abort path consumes them and
    // force-dirties the cone so the next incremental typecheck cannot
    // skip restored nodes. Soft/Quiet → depth=0 → no-op.
    if (persist_elim_cone) {
        for (auto nid : elim_ast) {
            coerced_nodes_tracker_push(nid);
        }
    }
    return s.applied;
}

// Issue #3102: per-boundary TLS tracker for AST nodes that participated in
// apply_coercion_map (production/Full only). The mutation boundary enter
// increments depth; abort path consumes via coerced_nodes_tracker_take()
// and force-dirties the cone. Soft/Quiet → depth=0 → zero cost on push.
inline thread_local std::vector<aura::compiler::dirty::NodeId> g_coerced_nodes_in_boundary_tls;
inline thread_local std::uint64_t g_coerced_nodes_in_boundary_depth_tls = 0;

// Issue #3102: AC1/AC5 — counters for the CoercionMap abort rewind path.
// Production/Full bumps the real counters; Soft bumps the observe-only
// counters. Quiet (no abort / no boundary) → zero cost.
export inline std::atomic<std::uint64_t> g_coercion_map_abort_rewind_total{0};
export inline std::atomic<std::uint64_t> g_coercion_map_abort_rewind_observe_total{0};
export inline std::atomic<std::uint64_t> g_coercion_map_apply_tracker_push_total{0};
export inline std::atomic<std::uint64_t> g_coercion_map_abort_forced_dirty_total{0};
export inline std::atomic<std::uint64_t> g_coercion_map_abort_soft_observe_total{0};
// Issue #3116: dual-clear last_coercions_ + TLS active context on abort.
export inline std::atomic<std::uint64_t> g_coercion_abort_dual_clear_total{0};
export inline std::atomic<std::uint64_t> g_coercion_abort_dual_clear_observe_total{0};
export inline constexpr int kCoercionAbortDualClearIssue = 3116;

export inline void coerced_nodes_tracker_enter_boundary() noexcept {
    ++g_coerced_nodes_in_boundary_depth_tls;
}
export inline void coerced_nodes_tracker_exit_boundary() noexcept {
    if (g_coerced_nodes_in_boundary_depth_tls > 0)
        --g_coerced_nodes_in_boundary_depth_tls;
    if (g_coerced_nodes_in_boundary_depth_tls == 0)
        g_coerced_nodes_in_boundary_tls.clear();
}
export inline void coerced_nodes_tracker_push(aura::compiler::dirty::NodeId nid) noexcept {
    if (g_coerced_nodes_in_boundary_depth_tls == 0)
        return;
    if (nid == 0)
        return;
    g_coerced_nodes_in_boundary_tls.push_back(nid);
    g_coercion_map_apply_tracker_push_total.fetch_add(1, std::memory_order_relaxed);
}
export [[nodiscard]] inline std::vector<aura::compiler::dirty::NodeId>
coerced_nodes_tracker_take() noexcept {
    auto v = std::move(g_coerced_nodes_in_boundary_tls);
    g_coerced_nodes_in_boundary_tls.clear();
    return v;
}
export [[nodiscard]] inline std::size_t coerced_nodes_tracker_size() noexcept {
    return g_coerced_nodes_in_boundary_tls.size();
}

export inline void reset_coercion_map_abort_rewind_for_test() noexcept {
    g_coercion_map_abort_rewind_total.store(0, std::memory_order_relaxed);
    g_coercion_map_abort_rewind_observe_total.store(0, std::memory_order_relaxed);
    g_coercion_map_apply_tracker_push_total.store(0, std::memory_order_relaxed);
    g_coercion_map_abort_forced_dirty_total.store(0, std::memory_order_relaxed);
    g_coercion_map_abort_soft_observe_total.store(0, std::memory_order_relaxed);
    g_coercion_abort_dual_clear_total.store(0, std::memory_order_relaxed);
    g_coercion_abort_dual_clear_observe_total.store(0, std::memory_order_relaxed);
}
export inline void clear_coercion_map_abort_rewind_for_test() noexcept {
    reset_coercion_map_abort_rewind_for_test();
}

} // namespace aura::compiler
