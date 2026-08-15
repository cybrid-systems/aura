// typed_mutation_audit.h — Issue #1589 / #1216 / #1882: production TypedMutationAuditPass.
// Thread-safe strategy gate, contextual event capture, in-memory ring trail.
// #1882: AOT hot-update + JIT hotpath audit capture.
// Issue #2818: cold-start default is AuditStrategy::Full (every mutation
// audited + invariant hard-gate). Sampled/ratio=4 is opt-in only via
// apply_dev_audit_defaults() (tests / AURA_SANDBOX=off). Deployments that
// never call apply_production_audit_defaults still audit fully.
// Header form so serve/evaluator/tests can include without module churn.

#ifndef AURA_COMPILER_TYPED_MUTATION_AUDIT_H
#define AURA_COMPILER_TYPED_MUTATION_AUDIT_H

#include "core/provenance_tracker.hh"
#include "core/resource_quota.hh"     // process_resource_quota_manager (#2493 mid resolve)
#include "core/security_event_wal.hh" // #3054 emit_security_event_durable on refuse
#include "core/workspace_epoch.hh"    // current_mutation_epoch (#2493 mid resolve)
// Issue #2836: resolve-time mid-fallback hard face is absolute zero-tolerance
// (no SLO rate check). Schedule-gate (#2630/#2594) still uses
// audit_mid_fallback_slo.h; include removed from this header after #2836.

// Issue #2758: thin count API (defined in ownership_rebind.cpp) so this
// header does not include ownership_rebind.h (avoid include-order cascade).
namespace aura::compiler {
[[nodiscard]] std::size_t linear_or_dirty_roots_count_for_rebind() noexcept;
}

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Issue #2899: C bridges for IR fast-path eligibility (file scope — not
// nested inside a function / namespace block). Defined in
// evaluator_fiber_mutation.cpp / typed_mutation_audit_hooks.cpp.
extern "C" std::size_t aura_evaluator_mutation_boundary_depth();
extern "C" int aura_escape_move_gate_active() noexcept;

namespace aura::compiler::typed_audit {

inline constexpr int kTypedMutationAuditPassPhase =
    7; // #2145 Full/Strict hard-gate (lineage #2029 / #1894)
inline constexpr int kTypedMutationAuditIssue =
    2145; // lineage 1894 / 1614 / 1589; AOT #1882; #2027/#2029 satellite
// Issue #2053: production multi-tenant AI — stronger audit defaults.
inline constexpr int kProductionSecurityDefaultsIssue = 2053;
inline constexpr std::size_t kTypedMutationAuditTrailSize = 256;
// Force audit when dirty scope is large (Sampled strategy still hits).
inline constexpr std::uint64_t kAuditForceNodesChanged = 8;
// Issue #2053: under production defaults, force critical kinds even if Sampled.
inline constexpr std::uint64_t kAuditForceNodesChangedProduction = 1;
inline constexpr std::size_t kAuditNameCap = 48;

// Issue #2728: forward-declaration block for symbols that have cyclic
// or out-of-order dependencies. Grouped here (per the issue
// recommendation: "Prefer moving pure declarations to the top / a
// separate forward block") so the header is self-consistent and
// aura_test_objects rebuilds cleanly without forward-reference
// errors. Definitions remain in their original positions below.
struct CommitReadinessInput;
struct CommitReadiness;
[[nodiscard]] inline CommitReadinessInput commit_readiness_live_policy() noexcept;
[[nodiscard]] inline CommitReadiness commit_readiness(const CommitReadinessInput& in) noexcept;
[[nodiscard]] inline std::uint64_t cone_outside_goal_drop_total_v_read() noexcept;
[[nodiscard]] inline std::uint64_t occurrence_empty_after_fence_total_v_read() noexcept;
// NOTE: g_occurrence_hard_face_full_solve_recover_total is an `inline
// std::atomic{0}` defined below (line 1053 — BEFORE the usage site at
// line 1092 in commit_readiness_live_policy). No `extern` forward decl
// here — the prior extern/inline split tripped gcc 16.1's namespace
// parser (cp_parser_namespace_body ICE) in typed_mutation_audit_hooks.cpp.

enum class AuditStrategy : std::uint8_t {
    Off = 0,
    Sampled = 1,
    Full = 2,
};

enum class MutationKind : std::uint8_t {
    Unknown = 0,
    Structural = 1,
    ReplaceType = 2,
    ReplaceValue = 3,
    RecordPatch = 4,
    Other = 5,
    MacroHygiene = 6, // Issue #1613: hygiene-protected / macro-aware mutate
    AotHotUpdate = 7, // Issue #1882: AOT module hot-reload boundary
    JitHotpath = 8,   // Issue #1882: JIT L2 / apply hotpath sample
};

enum class AuditOutcome : std::uint8_t {
    Success = 0,
    Rollback = 1,
    Error = 2,
};

struct TypedMutationAuditEvent {
    std::uint64_t mutation_id = 0;
    std::uint64_t seq = 0;
    char name[kAuditNameCap]{};
    MutationKind kind = MutationKind::Unknown;
    std::uint64_t before_epoch = 0;
    std::uint64_t after_epoch = 0;
    AuditOutcome outcome = AuditOutcome::Success;
    std::uint32_t target_node = 0;
    std::uint32_t nodes_changed = 0;
    std::int64_t fiber_id = 0;
    std::uint64_t timestamp_ms = 0;
    std::uint32_t affected_ref_count = 0;
};

// Process-wide atomics (thread-safe).
struct TypedMutationAuditCounters {
    std::atomic<std::uint64_t> audits_considered{0};
    std::atomic<std::uint64_t> samples_skipped{0};
    std::atomic<std::uint64_t> contextual_total{0}; // AC: typed_mutation_audit_contextual_total
    std::atomic<std::uint64_t> trail_writes{0};
    // Issue #2819: lock-free trail publish observability.
    std::atomic<std::uint64_t> audit_trail_lockfree_total{0};
    std::atomic<std::uint64_t> audit_trail_mutex_wait_us_total{0}; // 0 when lock-free path
    std::atomic<std::uint32_t> audit_trail_lockfree_wired{1};
    std::atomic<std::uint64_t> rollbacks{0};
    std::atomic<std::uint64_t> errors{0};
    // Issue #2818: Full is the cold-start default so small non-linear mutates
    // never under-sample invariants. Sampled only via apply_dev_audit_defaults.
    std::atomic<std::uint32_t> strategy{static_cast<std::uint32_t>(AuditStrategy::Full)};
    std::atomic<std::uint32_t> sample_ratio{1}; // every Nth id when Sampled (N>=1)
    // Issue #2053: 1 when production security defaults applied (Full or ratio=1).
    std::atomic<std::uint32_t> production_defaults_active{0};
    // Issue #2818: 1 after apply_dev_audit_defaults (explicit Sampled opt-in).
    std::atomic<std::uint32_t> dev_audit_opt_in{0};
    // Issue #2818: Sampled+ratio>1 without apply_dev_audit_defaults opt-in.
    std::atomic<std::uint64_t> audit_strategy_default_warnings_total{0};
    std::atomic<std::uint32_t> audit_strategy_default_warning_fired{0};
    std::atomic<std::uint64_t> trail_seq{0};
    // Issue #1613: macro hygiene audit trail (hygiene-protected blocks + allowed macro mutates).
    std::atomic<std::uint64_t> macro_hygiene_events{0};
    std::atomic<std::uint64_t> macro_hygiene_blocked{0};
    std::atomic<std::uint64_t> macro_hygiene_allowed{0};
    // Issue #1614: real post-mutation invariant audit (type + linear + provenance).
    std::atomic<std::uint64_t> invariant_audits{0};
    std::atomic<std::uint64_t> type_invariant_ok{0};
    std::atomic<std::uint64_t> type_invariant_fail{0};
    std::atomic<std::uint64_t> linear_invariant_ok{0};
    std::atomic<std::uint64_t> linear_invariant_fail{0};
    std::atomic<std::uint64_t> provenance_invariant_ok{0};
    std::atomic<std::uint64_t> provenance_invariant_fail{0};
    // Issue #2223: ADT match exhaustiveness dimension of the invariant suite.
    std::atomic<std::uint64_t> adt_invariant_ok{0};
    std::atomic<std::uint64_t> adt_invariant_fail{0};
    std::atomic<std::uint64_t> adt_exhaustiveness_sites_checked_total{0};
    std::atomic<std::uint64_t> adt_non_exhaustive_sites_total{0};
    // Issue #2264: hard-gate suite ADT exhaustiveness process metrics
    // (named per issue contract; refine #2223).
    std::atomic<std::uint64_t> adt_exhaustiveness_audit_total{0};
    std::atomic<std::uint64_t> adt_exhaustiveness_fail_total{0};
    std::atomic<std::uint32_t> adt_exhaustiveness_hard_gate_wired{1};
    std::atomic<std::uint64_t> invariant_violations_caught{0};
    std::atomic<std::uint64_t> invariant_all_pass{0};
    // Issue #1894 AC metric names (aliases of invariant suite + contextual gate).
    std::atomic<std::uint64_t> typed_mutation_audit_triggered_total{0};
    std::atomic<std::uint64_t> typed_mutation_violations_caught_total{0};
    std::atomic<std::uint64_t> provenance_blame_chain_hits_total{0};
    // Issue #1924: blame completeness under multi-round typed_mutate.
    std::atomic<std::uint64_t> blame_chain_complete_total{0};
    std::atomic<std::uint64_t> blame_propagation_miss_total{0};
    std::atomic<std::uint64_t> full_strategy_force_rollback_total{0};
    std::atomic<std::uint64_t> contextual_force_audit_total{0};
    // Issue #2145: Full/Strict hard-gate (always-run suite + force rollback).
    std::atomic<std::uint64_t> hard_gate_audits_total{0};
    std::atomic<std::uint64_t> hard_gate_force_rollback_total{0};
    std::atomic<std::uint64_t> hard_gate_strict_hold_total{0};
    std::atomic<std::uint64_t> hard_gate_sampled_skip_total{0};
    std::atomic<std::uint32_t> hard_gate_wired{1};
    // Issue #2814 M7: audit trail Success must be linked to invariant
    // enforcement (or intentional skip). Gap = Success recorded without either.
    // audit_enforcement_link_wired always 1 when this module is linked.
    std::atomic<std::uint32_t> audit_enforcement_link_wired{1};
    std::atomic<std::uint64_t> audit_enforcement_ran_total{0};
    std::atomic<std::uint64_t> audit_enforcement_skipped_intentional_total{0};
    std::atomic<std::uint64_t> audit_enforcement_gap_total{0};
    // Issue #2260: MutationBoundary type-proof (SOLVED / !truncated_reverify).
    // Hard-gate exit must full-resync or force-rollback — never silent continue.
    std::atomic<std::uint64_t> boundary_solve_hard_gate_total{0};
    std::atomic<std::uint64_t> boundary_solve_full_resync_total{0};
    std::atomic<std::uint64_t> boundary_solve_force_rollback_total{0};
    std::atomic<std::uint64_t> boundary_solve_truncated_seen_total{0};
    std::atomic<std::uint32_t> boundary_solve_hard_gate_wired{1};
    // Issue #2277: production-default TIMEOUT escalation (Option A — full-solve attempt).
    // delta_timeout_full_solve_total — every full-solve attempt made after
    //     production-default solve_delta TIMEOUT (regardless of final result).
    // delta_timeout_reject_total — full-solve did NOT reach SOLVED under production
    //     defaults; caller MUST treat solve as failed (no half-solved ship).
    std::atomic<std::uint64_t> delta_timeout_full_solve_total{0};
    std::atomic<std::uint64_t> delta_timeout_reject_total{0};
    std::atomic<std::uint32_t> delta_timeout_hard_gate_wired{1};
    // Issue #3003: Production / Full fail-closed after solve_delta not
    // SOLVED — no type write / dirty clear / stash / query:type authority.
    // Soft observe-only (this counter stays quiet).
    std::atomic<std::uint64_t> delta_timeout_fail_closed_total{0};
    std::atomic<std::uint32_t> delta_timeout_fail_closed_wired{1};
    // Issue #2913: solve_delta locality SLO (anti silent under-constrain).
    // Soft + residual: observe_total only (allow SOLVED).
    // production / Full + residual: escalate full solve; reject if unsolved.
    // Quiet local SOLVED: zero cost (no counter bump).
    std::atomic<std::uint64_t> solve_delta_locality_slo_observe_total{0};
    std::atomic<std::uint64_t> solve_delta_locality_escalate_total{0};
    std::atomic<std::uint64_t> solve_delta_locality_reject_total{0};
    std::atomic<std::uint32_t> solve_delta_locality_slo_wired{1};
    // Issue #2994: Agent locality residual budget (production only).
    // allow: residual ≤ max_locality_residual, SOLVED retained.
    // escalate: residual > budget, full path.
    // pending_handoff: residual roots merged into pending_full_solve.
    // Quiet / Soft / budget 0: no bump on these three (compat #2913).
    std::atomic<std::uint64_t> delta_locality_budget_allow_total{0};
    std::atomic<std::uint64_t> delta_locality_budget_escalate_total{0};
    std::atomic<std::uint64_t> delta_locality_budget_pending_handoff_total{0};
    std::atomic<std::uint32_t> delta_locality_budget_wired{1};
    // Issue #2900: SolverBudget surface (Agent-controlled delta TIMEOUT policy).
    // timeout_export: Soft + allow_timeout_commit kept TIMEOUT (never SOLVED).
    // full_escalate: production still escalated under non-default budget.
    // instance_repair_prefer: prefer_instance_repair_before_full noted.
    std::atomic<std::uint64_t> solver_budget_timeout_export_total{0};
    std::atomic<std::uint64_t> solver_budget_full_escalate_total{0};
    std::atomic<std::uint64_t> solver_budget_instance_repair_prefer_total{0};
    std::atomic<std::uint32_t> solver_budget_wired{1};
    // Issue #2963: production prefer instance-repair before full-solve.
    // delta_instance_repair_total: repair walk attempted (had dirty/roots).
    // delta_instance_repair_resolved_total: local repair reached SOLVED.
    // delta_timeout_full_after_repair_total: repair residual → full escalate.
    // Quiet / Soft / no dirty: zero cost (no bump).
    std::atomic<std::uint64_t> delta_instance_repair_total{0};
    std::atomic<std::uint64_t> delta_instance_repair_resolved_total{0};
    std::atomic<std::uint64_t> delta_timeout_full_after_repair_total{0};
    std::atomic<std::uint32_t> delta_instance_repair_wired{1};
    // Issue #2642: Phase 5 densify post-compact linear-root scan counters.
    // linear_densify_scan_mismatch_observe_total: Soft path bumps this
    //     counter (no force-rollback); Agents can watch the scan fire.
    // linear_densify_scan_mismatch_total: hard path bump on real
    //     mismatch (force_linear_rollback(LinearDensifyRootMismatch)).
    std::atomic<std::uint64_t> linear_densify_scan_mismatch_observe_total{0};
    std::atomic<std::uint64_t> linear_densify_scan_mismatch_total{0};
    // Issue #2673: hard-path lock for densify linear-root consistency scan.
    // Test seam — scan_linear_roots_after_densify consumes one pending
    // mismatch per call (CAS loop), so the test can inject N mismatches
    // and observe N scan firings under production/Full (force_rollback)
    // or N observe-counter bumps under Soft. Forward-compatible with the
    // full O(dirty) walk landing in the follow-up commit (same shape as
    // inject_densify_ownership_scan_fail_for_test in envframe_lifetime.ixx).
    std::atomic<std::uint64_t> linear_densify_scan_mismatch_inject_pending{0};
    // Issue #1882: AOT hot-update + JIT hotpath audit coverage.
    std::atomic<std::uint64_t> aot_hotupdate_attempts{0};
    std::atomic<std::uint64_t> aot_hotupdate_audits{0};
    std::atomic<std::uint64_t> aot_hotupdate_ok{0};
    std::atomic<std::uint64_t> aot_hotupdate_fail{0};
    std::atomic<std::uint64_t> aot_hotupdate_invariant_fail_total{0};
    std::atomic<std::uint64_t> jit_hotpath_audits{0};
    std::atomic<std::uint64_t> audit_mutation_id_gen{0};
    // Issue #2493: fallback-gen counter — bumped when an audit path took
    // the last-resort next_audit_mutation_id() branch (no caller mid, no
    // WorkspaceEpoch Mutation, no ResourceQuota host mid). Agent
    // dashboards can compute join quality from this counter (lower =
    // fewer process-origin join stamps; mid-vocabulary preferred).
    // Soft/Sampled only after #2836 (production/Full refuse instead).
    std::atomic<std::uint64_t> audit_mid_fallback_gen_total{0};
    // Issue #2836: absolute zero-tolerance hard-refuse counter — bumped when
    // resolve_audit_mutation_id refuses a process-origin stamp under
    // production_defaults_active() || Full (returns 0; no gen bump).
    // Distinct from audit_mid_fallback_gen_total (Soft join-stamp path).
    std::atomic<std::uint64_t> audit_mid_fallback_refused_total{0};
    // Issue #3054: joinable SE emit count + last ring seq (not a second bus).
    std::atomic<std::uint64_t> audit_mid_fallback_refuse_se_total{0};
    std::atomic<std::uint64_t> audit_mid_fallback_refuse_se_seq{0};
    // Issue #1884: TypePropagation / predicate_memo ↔ invariant correlation.
    std::atomic<std::uint64_t> type_prop_invariant_correlation_total{0};
    std::atomic<std::uint64_t> type_prop_invariant_pass_with_evidence_total{0};
    std::atomic<std::uint64_t> type_prop_invariant_fail_with_evidence_total{0};
    std::atomic<std::uint64_t> type_prop_evidence_lost_total{0};
    std::atomic<std::uint64_t> predicate_memo_evict_correlated_total{0};
    // Last pass snapshot (process-wide, relaxed) for correlation window.
    std::atomic<std::uint64_t> last_type_prop_fixpoint_rounds{0};
    std::atomic<std::uint64_t> last_type_prop_narrow_hits{0};
    std::atomic<std::uint64_t> last_type_prop_extended_ops{0};
    std::atomic<std::uint64_t> last_dce_narrow_hits{0};
    std::atomic<std::uint64_t> last_predicate_memo_evictions{0};
    std::atomic<std::uint64_t> last_invariant_all_ok{1}; // 1=pass, 0=fail
    // Issue #2027: composite / nested txn + atomic_batch invariant suite.
    std::atomic<std::uint64_t> composite_invariant_audits_total{0};
    std::atomic<std::uint64_t> composite_invariant_ok_total{0};
    std::atomic<std::uint64_t> composite_invariant_fail_total{0};
    std::atomic<std::uint64_t> composite_partial_recover_type_total{0};
    std::atomic<std::uint64_t> composite_partial_recover_linear_total{0};
    std::atomic<std::uint64_t> composite_partial_recover_success_total{0};
    std::atomic<std::uint64_t> composite_full_rollback_total{0};
    std::atomic<std::uint64_t> composite_nested_audit_total{0};
    std::atomic<std::uint64_t> composite_batch_audit_total{0};
    std::atomic<std::uint64_t> composite_cross_batch_linear_escape_total{0};
    // Issue #2108: commit hard-blocked because linear escape was observed
    // (never leave escaped linear live across composite batch boundaries).
    std::atomic<std::uint64_t> linear_escape_commit_blocked_total{0};
    std::atomic<std::uint64_t> composite_partial_recover_attempt_total{0};
    // Issue #2105: ordered composite/nested commit barrier
    // (solve_delta_occurrence → linear revalidate → invariant audit).
    std::atomic<std::uint64_t> composite_commit_revalidate_total{0};
    std::atomic<std::uint64_t> composite_commit_ok_total{0};
    std::atomic<std::uint64_t> composite_commit_reject_total{0};
    // Issue #2644: batch-level TypeVar refined consistency drift
    // detection (anti SOLVED-but-drift under composite / atomic_batch).
    // Soft path bumps observe only; production/Full rejects commit with
    // type_scheme_drift reason. Folded into the existing composite commit
    // barrier so #2610 empty-CS / auto_partial gates are unchanged.
    std::atomic<std::uint64_t> composite_type_scheme_drift_observe_total{0};
    std::atomic<std::uint64_t> composite_type_scheme_drift_reject_total{0};
    std::atomic<std::uint64_t> composite_commit_solve_fail_total{0};
    std::atomic<std::uint64_t> composite_commit_linear_fail_total{0};
    // Issue #2180: commit reuses stashed partial CS vs empty greenfield.
    std::atomic<std::uint64_t> composite_commit_solve_reuse_hit_total{0};
    std::atomic<std::uint64_t> composite_commit_solve_empty_cs_total{0};
    // Issue #2345: expected-partial + empty CS anti false-green.
    // hard_miss: production / Full / Strict / AURA_COMPOSITE_EMPTY_CS_HARD=1
    //   → solve_ok forced false (commit rejected).
    // observe: dev Sampled soft path → counter only; commit may succeed.
    std::atomic<std::uint64_t> composite_commit_empty_cs_hard_miss_total{0};
    std::atomic<std::uint64_t> composite_commit_empty_cs_observe_total{0};
    std::atomic<std::uint32_t> composite_empty_cs_hard_wired{1};
    // Issue #2509: symmetric expected_partial ↔ commit_cs_has_work matrix.
    //   unexpected_cs_work: !expected_partial + has_work (observe always;
    //     under Full/production never silent-skip solve on dirty CS).
    //   expected_has_work: expected_partial + has_work (must enter SDO;
    //     vacuous SOLVED without SDO forbidden).
    //   sdo_entered: solve_delta_occurrence actually ran this commit.
    //   signature_matrix_wired: sentinel = 1.
    std::atomic<std::uint64_t> composite_commit_unexpected_cs_work_total{0};
    std::atomic<std::uint64_t> composite_commit_expected_has_work_total{0};
    std::atomic<std::uint64_t> composite_commit_sdo_entered_total{0};
    std::atomic<std::uint32_t> composite_cs_signature_matrix_wired{1};
    // Issue #2610: auto-detect expected_partial from dirty cone / CS work
    // when Agents under-mark. Hard (production/Full/empty-CS hard): effective
    // expected_partial=true so empty-CS hard-miss / force-SDO fire.
    // Soft: observe only (auto_partial_from_cone_observe_total).
    std::atomic<std::uint64_t> composite_commit_auto_partial_from_cone_total{0};         // #2610
    std::atomic<std::uint64_t> composite_commit_auto_partial_from_cone_observe_total{0}; // #2610
    std::atomic<std::uint32_t> composite_auto_partial_from_cone_wired{1};                // #2610
    // Issue #2851: residual close of #2610. Non-empty mutation log on
    // outermost success boundary forces expected_partial under
    // production_defaults / Full / Strict. Soft: observe only when env
    // opt-in (AURA_COMPOSITE_LOG_FORCES_PARTIAL=1). Quiet path (log_delta=0)
    // is zero-cost — counter not bumped, force_reason not advanced.
    std::atomic<std::uint64_t> composite_commit_log_forces_partial_total{0};         // #2851
    std::atomic<std::uint64_t> composite_commit_log_forces_partial_observe_total{0}; // #2851
    std::atomic<std::uint32_t> composite_commit_log_forces_partial_wired{1};         // #2851
    // Issue #2898: explicit required TypeId invariant set on composite_txn_commit
    // (anti under-mark false-green). Empty span → zero cost. Production/Full/
    // Strict hard-reject on miss; Soft observe-only allow.
    std::atomic<std::uint64_t> composite_required_type_fail_total{0};    // #2898 hard
    std::atomic<std::uint64_t> composite_required_type_observe_total{0}; // #2898 Soft
    std::atomic<std::uint64_t> composite_required_type_checked_total{0}; // #2898 ids scanned
    std::atomic<std::uint32_t> composite_required_type_wired{1};         // #2898
    // Issue #2983: production default required set when Agents under-mark
    // (empty span + non-empty touched). Derive-then-check, cap 16. Soft
    // never auto-fills. Reject-over-infer is env/test override.
    std::atomic<std::uint64_t> composite_required_type_auto_fill_total{0};         // #2983
    std::atomic<std::uint64_t> composite_required_type_auto_fill_capped_total{0};  // #2983
    std::atomic<std::uint64_t> composite_required_type_reject_over_infer_total{0}; // #2983
    std::atomic<std::uint32_t> composite_required_type_auto_fill_wired{1};         // #2983
    // Issue #2458: outermost commit gate on truncated reverify / incomplete
    // blame (non-empty under-scanned CS — residual half-green after #2345).
    // Soft/Sampled: observe only (commit may still succeed).
    // production_defaults / Full / Strict / AURA_TRUNCATE_COMMIT_HARD=1:
    //   one full ConstraintSystem::solve(); still bad → reject; recovered → ok.
    std::atomic<std::uint64_t> truncate_commit_observe_total{0};
    std::atomic<std::uint64_t> truncate_commit_reject_total{0};
    std::atomic<std::uint64_t> truncate_commit_full_solve_recover_total{0};
    std::atomic<std::uint32_t> truncate_commit_hard_wired{1};
    // Issue #2221: composite commit blame-complete hard gate.
    std::atomic<std::uint64_t> blame_commit_check_total{0};
    std::atomic<std::uint64_t> blame_commit_reject_total{0};
    std::atomic<std::uint64_t> blame_commit_incomplete_observe_total{0};
    // Issue #2029: Full-strategy per-category partial recovery (all boundaries,
    // not only composite). Prefer type/linear/provenance recover before
    // structural rollback; soundness: re-audit must all_ok before continue.
    std::atomic<std::uint64_t> partial_recovery_attempt_total{0};
    std::atomic<std::uint64_t> partial_recovery_success_total{0};
    std::atomic<std::uint64_t> partial_recovery_fail_total{0};
    std::atomic<std::uint64_t> partial_recovery_type_total{0};
    std::atomic<std::uint64_t> partial_recovery_linear_total{0};
    std::atomic<std::uint64_t> partial_recovery_provenance_total{0};
    // Issue #2223: Full-strategy ADT renarrow / revalidate recovery category.
    std::atomic<std::uint64_t> partial_recovery_adt_total{0};
    // Issue #2514 / #2545: linear hard-fail unified with MutationBoundary exit.
    // Single entry: Evaluator::force_linear_rollback (classify_linear_force).
    // When synth already hard-failed under production/strict, boundary forces
    // rollback and skips soft partial recovery (cannot clear synth TypeError).
    //
    // Authority table (#2545 / #2563 — single source of truth for Agents):
    //   SynthHardFail     → force + skip soft recovery; deny=linear-synth-hard-fail
    //   PostMutateLinear  → force under hard-gate/Full; deny=linear-post-mutate-fail
    //   CrossBatchEscape  → force; deny=linear-cross-batch-escape (#2108)
    //   CrossClosureEscape→ force under hard; deny=linear-cross-closure-escape (#2563)
    //   None              → zero extra force counters (type/prov may still deny)
    // Soft Warning synth never appears as SynthHardFail (#2514 AC retained).
    // Soft cross-closure: observe counters only unless AURA_LINEAR_CROSS_CLOSURE_HARD=1
    // or production/Full (#2563 AC1).
    // Issue #2623: cone truncation under hard → fail-closed (same CrossClosureEscape
    // authority / deny=linear-cross-closure-escape); Soft trunc → metrics only.
    //
    // Counter ownership table (no double-count of same logical violation):
    //   linear_synth_violation_total / linear_synth_hard_fail_total (#2357)
    //     → synthesize phase (note_linear_synth_violation only)
    //   linear_invariant_fail / linear_invariant_ok (#1614)
    //     → post-mutate invariant audit linear walk only
    //   linear_synth_boundary_force_rollback_total (#2514)
    //     → force_linear_rollback when authority == SynthHardFail only
    //   hard_gate_force_rollback_total
    //     → all force_linear_rollback authorities + non-linear hard-gate deny
    //   linear_cross_closure_escape_total / force_total / observe_total (#2563)
    //     → discovery path only; force_linear_rollback does not re-bump escape_total
    //   linear_cross_closure_trunc_force_total (#2623)
    //     → discovery path when cap_truncations under hard (fail-closed scan)
    //   When synth early-exits, invariant audit is NOT re-run for that mid
    //   (so linear_invariant_fail does not also bump for the same violation).
    // All hard-gate / outermost boundary exit / composite reject sites must
    // call force_linear_rollback (#2545 AC6) — not ad-hoc sticky checks.
    std::atomic<std::uint64_t> linear_synth_boundary_force_rollback_total{0};
    std::atomic<std::uint64_t> linear_synth_boundary_skip_recovery_total{0};
    std::atomic<std::uint32_t> linear_synth_authority_unified{1};
    // Issue #2545: force_linear_rollback is the single hard-fail decision entry.
    std::atomic<std::uint32_t> linear_force_unified_2545{1};
    // Issue #2563: cross-closure / free-capture linear escape discovery.
    std::atomic<std::uint64_t> linear_cross_closure_escape_total{0};
    std::atomic<std::uint64_t> linear_cross_closure_force_total{0};
    std::atomic<std::uint64_t> linear_cross_closure_observe_total{0};
    std::atomic<std::uint64_t> linear_cross_closure_cap_trunc_total{0};
    std::atomic<std::uint32_t> linear_cross_closure_wired{1};
    // Issue #2612: optional depth-2 nested Lambda free-capture (still cone-capped).
    // depth2_entries_total: nested Lambda bodies entered under depth_cap>=2.
    // depth2_escape_total: free dirty linear captures found only at nested depth.
    std::atomic<std::uint64_t> linear_cross_closure_depth2_entries_total{0};
    std::atomic<std::uint64_t> linear_cross_closure_depth2_escape_total{0};
    std::atomic<std::uint32_t> linear_cross_closure_depth_wired{1};
    // Issue #2623: production fail-closed when discovery cone is truncated.
    std::atomic<std::uint64_t> linear_cross_closure_trunc_force_total{0};
    std::atomic<std::uint32_t> linear_cross_closure_depth_max{3}; // hard cap (env clamp)
    std::atomic<std::uint32_t> linear_cross_closure_prod_depth_default{2};
};

inline TypedMutationAuditCounters g_typed_mutation_audit_counters{};

// Issue #2673: hard-path lock for densify linear-root consistency scan.
// Test seam — injects a pending linear-root mismatch that the next
// scan_linear_roots_after_densify call will consume. Mirrors the
// inject_densify_ownership_scan_fail_for_test pattern in envframe_lifetime.ixx
// (inline atomic bump → caller reads + clears via scan).
// Under production/Full: scan returns true → caller routes to
// force_linear_rollback(LinearDensifyRootMismatch) → bumps
// linear_densify_scan_mismatch_total.
// Under Soft: scan consumes the inject AND bumps
// linear_densify_scan_mismatch_observe_total (no force-rollback).
inline void inject_linear_densify_scan_mismatch_for_test() noexcept {
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.fetch_add(
        1, std::memory_order_relaxed);
}
// Drain helper for tests that want a clean baseline before injecting.
inline void clear_linear_densify_scan_mismatch_inject_for_test() noexcept {
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
}
// Read-only peek for tests / chaos harness accounting.
[[nodiscard]] inline std::uint64_t linear_densify_scan_mismatch_inject_pending() noexcept {
    return g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.load(
        std::memory_order_relaxed);
}

// Issue #2819: lock-free ring for hot-path writers (capture_audit_event_forced).
// Slot index is trail_seq % size (seq allocated via atomic fetch_add).
// Writers publish the full event POD without g_trail().mu — matches the
// SecurityEvent ring pattern (best-effort; readers re-check seq).
// mu is retained only for exclusive reset_for_test clears.
struct TypedMutationAuditTrail {
    std::mutex mu; // reset_for_test only (#2819: not on capture hot path)
    TypedMutationAuditEvent ring[kTypedMutationAuditTrailSize]{};
};

inline TypedMutationAuditTrail& g_trail() {
    static TypedMutationAuditTrail t;
    return t;
}

[[nodiscard]] inline AuditStrategy get_strategy() noexcept {
    return static_cast<AuditStrategy>(
        g_typed_mutation_audit_counters.strategy.load(std::memory_order_relaxed));
}

inline void set_strategy(AuditStrategy s) noexcept {
    g_typed_mutation_audit_counters.strategy.store(static_cast<std::uint32_t>(s),
                                                   std::memory_order_relaxed);
}

inline void set_sample_ratio(std::uint32_t n) noexcept {
    if (n == 0)
        n = 1;
    g_typed_mutation_audit_counters.sample_ratio.store(n, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint32_t get_sample_ratio() noexcept {
    return g_typed_mutation_audit_counters.sample_ratio.load(std::memory_order_relaxed);
}

[[nodiscard]] inline bool production_defaults_active() noexcept {
    return g_typed_mutation_audit_counters.production_defaults_active.load(
               std::memory_order_relaxed) != 0;
}

// Issue #2345: env override AURA_COMPOSITE_EMPTY_CS_HARD=1 forces hard-reject
// on expected-partial empty CS even under Sampled/dev (sandbox off).
// Lazy-init; no exceptions (digit/flag parse matches other AURA_* gates).
[[nodiscard]] inline bool composite_empty_cs_hard_env() noexcept {
    static const bool cached = []() noexcept -> bool {
        const char* e = std::getenv("AURA_COMPOSITE_EMPTY_CS_HARD");
        if (e == nullptr || e[0] == '\0')
            return false;
        // Accept "1" / "true" / "yes" (case-insensitive first char).
        if (e[0] == '1' && e[1] == '\0')
            return true;
        if ((e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y') && e[1] != '\0')
            return true;
        return false;
    }();
    return cached;
}

// Issue #2345: hard-reject empty CS after expected partial under production
// defaults, Full strategy, Strict sandbox, or AURA_COMPOSITE_EMPTY_CS_HARD=1.
// Dev Sampled + sandbox off → soft observe only (AC2).
[[nodiscard]] inline bool composite_empty_cs_hard_reject_enabled() noexcept {
    return production_defaults_active() || get_strategy() == AuditStrategy::Full ||
           composite_empty_cs_hard_env();
}

// Issue #2851: Soft-path env opt-in for the log-forces-partial observe
// counter. Default Soft path has zero behavior change (per AC2); only bumps
// observe counter when env AURA_COMPOSITE_LOG_FORCES_PARTIAL=1 — lets
// Soft / Sampled test suites observe the same signal Agents see in
// production without changing Soft commit outcomes. Quiet path (env unset):
// always false → zero overhead (one relaxed load + one compare per boundary).
[[nodiscard]] inline bool composite_log_forces_partial_env_opt_in() noexcept {
    static const bool cached = []() noexcept -> bool {
        const char* e = std::getenv("AURA_COMPOSITE_LOG_FORCES_PARTIAL");
        if (e == nullptr || e[0] == '\0')
            return false;
        return e[0] == '1';
    }();
    return cached;
}

// Issue #2851: process-wide "pending" log delta plumbed from
// evaluator_mutation_boundary.cpp (which has access to cp.mutation_log_size
// + workspace_flat_->mutation_log_size()) to evaluator_typecheck.cpp
// (Evaluator::composite_txn_commit body, where the #2610 auto_partial_from_cone
// bump lives). Set per-boundary before composite_txn_commit call; read + reset
// inside composite_txn_commit body. Relaxed atomic — single producer (boundary
// exit) / single consumer (composite_txn_commit entry). Default 0 = no
// log delta since last composite commit.
inline std::atomic<std::uint64_t> g_composite_commit_log_forces_partial_pending_log_delta{0};
inline constexpr int kCompositeCommitLogForcesPartialIssue = 2851;

// Issue #2898: pending required TypeId set for the next composite_txn_commit.
// Agents / language wrappers stage TypeIds that MUST be concrete (UF binding
// not a free var) before allow-commit. Empty → current behavior (zero cost).
// Consumed (cleared) inside composite_txn_commit after the check runs.
// Thread-local: one producer (Agent/test) / one consumer (commit body).
struct CompositeRequiredTypeId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
};
inline thread_local std::vector<CompositeRequiredTypeId> g_composite_required_solved_pending{};
inline constexpr int kCompositeRequiredTypeIssue = 2898;

inline void set_composite_required_solved(std::span<const CompositeRequiredTypeId> ids) noexcept {
    g_composite_required_solved_pending.assign(ids.begin(), ids.end());
}

inline void clear_composite_required_solved() noexcept {
    g_composite_required_solved_pending.clear();
}

[[nodiscard]] inline std::span<const CompositeRequiredTypeId>
composite_required_solved_pending() noexcept {
    return g_composite_required_solved_pending;
}

// Issue #2983: production default required TypeId set (anti under-mark).
// Cap keeps the worklist bounded (soft-cone discipline).
inline constexpr std::size_t kCompositeRequiredTypeAutoFillCap = 16;
inline constexpr int kCompositeRequiredTypeDefaultIssue = 2983;
// -1 = use env AURA_COMPOSITE_REQUIRED_REJECT_OVER_INFER; 0/1 = test override.
inline std::atomic<std::int32_t> g_composite_required_reject_over_infer_override{-1};

inline void set_composite_required_reject_over_infer_for_test(bool on) noexcept {
    g_composite_required_reject_over_infer_override.store(on ? 1 : 0, std::memory_order_relaxed);
}
inline void reset_composite_required_reject_over_infer_for_test() noexcept {
    g_composite_required_reject_over_infer_override.store(-1, std::memory_order_relaxed);
}

[[nodiscard]] inline bool composite_required_reject_over_infer() noexcept {
    const auto o = g_composite_required_reject_over_infer_override.load(std::memory_order_relaxed);
    if (o == 0 || o == 1)
        return o == 1;
    static const bool cached = []() {
        const char* e = std::getenv("AURA_COMPOSITE_REQUIRED_REJECT_OVER_INFER");
        if (e == nullptr || e[0] == '\0')
            return false;
        return e[0] == '1';
    }();
    return cached;
}

// Issue #2621: process-wide last partial cone truncate (Agents + pure tests).
// Stamped by TypeChecker::infer_flat_partial after #2560 soft/hard truncate.
// Soft: observe only; production / AURA_PARTIAL_CONE_COMMIT_HARD → hard face.
// Issue #2698: independent occurrence-stability epoch (monotonic) — only
// advances on outermost success + persist, densify/steal that pruned
// goals, or explicit Agent fence (occurrence_stability_fence()). NOT
// coupled to cache_epoch (which advances on partial infer / unrelated
// full solve — Agents cannot ask "have my narrowings been stable since
// proof X?" when stability == cache_epoch). Soft zero-cost on empty
// goals path; production default records.
inline std::atomic<std::uint64_t> g_occurrence_stability_epoch{0};
inline std::atomic<std::uint64_t> g_occurrence_stability_fence_calls_total{0};
inline std::atomic<std::uint64_t> g_occurrence_stability_advance_on_persist_total{0};
inline std::atomic<std::uint64_t> g_occurrence_stability_advance_on_prune_total{0};
inline std::atomic<std::uint32_t> g_occurrence_stability_wired{1};
inline constexpr int kOccurrenceStabilityEpochIssue = 2698;

[[nodiscard]] inline std::uint64_t occurrence_stability_epoch_v_read() noexcept {
    return g_occurrence_stability_epoch.load(std::memory_order_relaxed);
}

// Issue #2698: explicit Agent-callable fence — bumps the stability
// epoch regardless of cache_epoch activity. Returns the new epoch.
inline std::uint64_t occurrence_stability_fence() noexcept {
    g_occurrence_stability_fence_calls_total.fetch_add(1, std::memory_order_relaxed);
    return g_occurrence_stability_epoch.fetch_add(1, std::memory_order_relaxed) + 1;
}

// Internal advance hooks (called from evaluator_mutation_boundary.cpp
// outermost-success + evaluator_fiber_mutation.cpp densify/steal prune).
inline void advance_occurrence_stability_on_persist() noexcept {
    g_occurrence_stability_advance_on_persist_total.fetch_add(1, std::memory_order_relaxed);
    g_occurrence_stability_epoch.fetch_add(1, std::memory_order_relaxed);
}

inline void advance_occurrence_stability_on_prune() noexcept {
    g_occurrence_stability_advance_on_prune_total.fetch_add(1, std::memory_order_relaxed);
    g_occurrence_stability_epoch.fetch_add(1, std::memory_order_relaxed);
}

inline void clear_occurrence_stability_epoch_for_test() noexcept {
    g_occurrence_stability_epoch.store(0, std::memory_order_relaxed);
    g_occurrence_stability_fence_calls_total.store(0, std::memory_order_relaxed);
    g_occurrence_stability_advance_on_persist_total.store(0, std::memory_order_relaxed);
    g_occurrence_stability_advance_on_prune_total.store(0, std::memory_order_relaxed);
}

inline std::atomic<std::uint8_t> g_last_partial_cone_truncated{0};
inline std::atomic<std::uint64_t> g_last_partial_cone_dropped{0};
inline std::atomic<std::uint64_t> g_last_partial_cone_fanout_trunc{0};
inline std::atomic<std::uint64_t> g_partial_cone_commit_observe_total{0};
inline std::atomic<std::uint64_t> g_partial_cone_commit_reject_total{0};
inline std::atomic<std::uint32_t> g_partial_cone_commit_gate_wired{1};
inline constexpr int kPartialConeCommitGateIssue = 2621;
// Issue #2694: Soft truncated cone silent dependency escalate. When Soft +
// cone truncate drops a type_dep / cascade edge that is a silent dependency
// of a live OccurrenceGoal / linear-typed binding (NOT itself a dirty If node
// — those are handled by #2646 outside-cone invalidate), arm this counter
// so commit_readiness can force one Full audit / reject under production_defaults.
// Pairs #2646 (outside-If invalidate) + #2672 (drift-injection soak) — closes
// the silent class. Soft pure-observe path only when no silent-dep edges
// (zero cost happy path). production / Full already hard — unchanged.
inline std::atomic<std::uint64_t> g_soft_truncated_silent_dep_escalate_total{0};
inline std::atomic<std::uint64_t> g_last_soft_truncated_silent_dep_count{0};
inline std::atomic<std::uint32_t> g_soft_truncated_silent_dep_wired{1};
inline constexpr int kSoftTruncatedSilentDepIssue = 2694;

[[nodiscard]] inline bool last_partial_cone_truncated() noexcept {
    return g_last_partial_cone_truncated.load(std::memory_order_relaxed) != 0;
}
[[nodiscard]] inline std::uint64_t last_partial_cone_dropped() noexcept {
    return g_last_partial_cone_dropped.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t last_partial_cone_fanout_trunc() noexcept {
    return g_last_partial_cone_fanout_trunc.load(std::memory_order_relaxed);
}

// Issue #2458: env override AURA_TRUNCATE_COMMIT_HARD=1 forces full-solve /
// reject policy on truncated reverify or incomplete blame even under Soft.
[[nodiscard]] inline bool truncate_commit_hard_env() noexcept {
    static const bool cached = []() noexcept -> bool {
        const char* e = std::getenv("AURA_TRUNCATE_COMMIT_HARD");
        if (e == nullptr || e[0] == '\0')
            return false;
        if (e[0] == '1' && e[1] == '\0')
            return true;
        if ((e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y') && e[1] != '\0')
            return true;
        return false;
    }();
    return cached;
}

// Issue #2458: hard gate for truncated/incomplete-blame commit under
// production defaults, Full strategy, or AURA_TRUNCATE_COMMIT_HARD=1.
// Callers may also OR Strict sandbox (same pattern as empty-CS #2345).
// Soft Sampled + sandbox off → observe only (AC1).
[[nodiscard]] inline bool truncate_commit_hard_enabled() noexcept {
    return production_defaults_active() || get_strategy() == AuditStrategy::Full ||
           truncate_commit_hard_env();
}

// AURA_PARTIAL_CONE_COMMIT_HARD=1 forces hard cone-truncate commit gate even
// under Soft. Unset → follow truncate_commit_hard_enabled() (prod/Full).
[[nodiscard]] inline bool partial_cone_commit_hard_env() noexcept {
    const char* e = std::getenv("AURA_PARTIAL_CONE_COMMIT_HARD");
    return e != nullptr && e[0] == '1';
}
[[nodiscard]] inline bool partial_cone_commit_hard_enabled() noexcept {
    return partial_cone_commit_hard_env() || truncate_commit_hard_enabled();
}

// Publish last cone truncate window (called from type_checker_impl #2560 block).
// silent_dep_count (#2694): number of dropped edges that are silent deps of
// live OccurrenceGoal / linear-typed bindings (NOT dirty If nodes — those are
// handled by #2646 outside-cone invalidate). When > 0, the escalate counter
// bumps so commit_readiness can arm Full audit on the next boundary.
inline void publish_partial_cone_truncate(bool truncated, std::uint64_t dropped,
                                          std::uint64_t fanout_trunc = 0,
                                          std::uint64_t silent_dep_count = 0) noexcept {
    g_last_partial_cone_truncated.store(truncated ? 1 : 0, std::memory_order_relaxed);
    g_last_partial_cone_dropped.store(dropped, std::memory_order_relaxed);
    if (fanout_trunc > 0)
        g_last_partial_cone_fanout_trunc.fetch_add(fanout_trunc, std::memory_order_relaxed);
    g_last_soft_truncated_silent_dep_count.store(silent_dep_count, std::memory_order_relaxed);
    if (silent_dep_count > 0)
        g_soft_truncated_silent_dep_escalate_total.fetch_add(silent_dep_count,
                                                             std::memory_order_relaxed);
    if (!truncated)
        return;
    if (partial_cone_commit_hard_enabled())
        g_partial_cone_commit_reject_total.fetch_add(1, std::memory_order_relaxed);
    else
        g_partial_cone_commit_observe_total.fetch_add(1, std::memory_order_relaxed);
}

inline void clear_partial_cone_truncate_for_test() noexcept {
    g_last_partial_cone_truncated.store(0, std::memory_order_relaxed);
    g_last_partial_cone_dropped.store(0, std::memory_order_relaxed);
    g_last_partial_cone_fanout_trunc.store(0, std::memory_order_relaxed);
    g_last_soft_truncated_silent_dep_count.store(0, std::memory_order_relaxed);
}

// Issue #2694: read-side accessors for the soft-truncated-silent-dep surface.
// Counter bumps via publish_partial_cone_truncate(silent_dep_count>0) or
// directly via publish_soft_truncated_silent_dep_escalate(n) at the cone
// truncate site when the detection heuristic flags a silent dep.
[[nodiscard]] inline std::uint64_t soft_truncated_silent_dep_escalate_total_v_read() noexcept {
    return g_soft_truncated_silent_dep_escalate_total.load(std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t last_soft_truncated_silent_dep_count() noexcept {
    return g_last_soft_truncated_silent_dep_count.load(std::memory_order_relaxed);
}

// Direct publish (used by post-truncate hook in type_checker_impl.cpp when
// the outside-cone diff reveals non-If silent deps that weren't covered
// by the #2646 invalidate). Test-only call site for AC1 wiring.
inline void publish_soft_truncated_silent_dep_escalate(std::uint64_t n = 1) noexcept {
    if (n == 0)
        return;
    g_soft_truncated_silent_dep_escalate_total.fetch_add(n, std::memory_order_relaxed);
}

inline void clear_soft_truncated_silent_dep_escalate_for_test() noexcept {
    g_soft_truncated_silent_dep_escalate_total.store(0, std::memory_order_relaxed);
    g_last_soft_truncated_silent_dep_count.store(0, std::memory_order_relaxed);
}

// Issue #3054: one joinable SE per boundary on mid-fallback refuse.
// Nested / re-resolve under the same boundary must not double-emit.
inline constexpr int kMidFallbackRefuseSeIssue = 3054;
inline thread_local bool g_tls_mid_fallback_refuse_se_emitted = false;

inline void clear_mid_fallback_refuse_se_tls() noexcept {
    g_tls_mid_fallback_refuse_se_emitted = false;
}

// Issue #2053: production multi-tenant AI — capture every self-modify event.
// Full strategy + production_defaults_active=1. Issue #2818: Full is also
// the cold-start static default; this call additionally arms production
// hard-face flags (production_defaults_active). Dev/test keep Sampled/ratio=4
// via apply_dev_audit_defaults / reset_for_test.
inline void apply_production_audit_defaults() noexcept {
    set_strategy(AuditStrategy::Full);
    set_sample_ratio(1);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.dev_audit_opt_in.store(0, std::memory_order_relaxed);
    clear_mid_fallback_refuse_se_tls();
}

// Issue #2053: restore fast-iteration Sampled defaults (tests / AURA_SANDBOX=off).
// Issue #2185: does not flip coercion reject-on-miss — callers that want full
// dev restore should also call reset_coercion_provenance_miss_policy_for_test
// or apply_production_security_defaults with AURA_SANDBOX=off.
// Issue #2818: marks dev_audit_opt_in so Sampled under-sample is intentional.
inline void apply_dev_audit_defaults() noexcept {
    set_strategy(AuditStrategy::Sampled);
    set_sample_ratio(4);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.dev_audit_opt_in.store(1, std::memory_order_relaxed);
    clear_mid_fallback_refuse_se_tls();
}

// Issue #2818: one-shot warn when Sampled under-samples without apply_dev
// opt-in (e.g. set_strategy(Sampled)+ratio>1). Production Sampled/1 and
// apply_dev_audit_defaults paths do not warn.
inline void maybe_warn_sampled_without_opt_in() noexcept {
    if (get_strategy() != AuditStrategy::Sampled)
        return;
    if (g_typed_mutation_audit_counters.dev_audit_opt_in.load(std::memory_order_relaxed) != 0)
        return;
    if (production_defaults_active())
        return;
    if (get_sample_ratio() <= 1)
        return;
    std::uint32_t expected = 0;
    if (!g_typed_mutation_audit_counters.audit_strategy_default_warning_fired
             .compare_exchange_strong(expected, 1, std::memory_order_relaxed))
        return;
    g_typed_mutation_audit_counters.audit_strategy_default_warnings_total.fetch_add(
        1, std::memory_order_relaxed);
    // stderr once so operators see Sampled under-sample without opt-in.
    std::fprintf(stderr,
                 "[aura typed_audit #2818] Sampled ratio=%u without "
                 "apply_dev_audit_defaults —  under-sampling invariants. "
                 "Call apply_production_audit_defaults() (Full) or "
                 "apply_dev_audit_defaults() (explicit Sampled opt-in). "
                 "Metric: audit_strategy_default_warnings_total.\n",
                 static_cast<unsigned>(get_sample_ratio()));
}

// Thread-safe Full / Sampled / Off gate.
// Sampled: audit when mutation_id % sample_ratio == 0.
// Issue #2818: cold-start default is Full (no under-sample).
[[nodiscard]] inline bool should_audit(std::uint64_t mutation_id) noexcept {
    g_typed_mutation_audit_counters.audits_considered.fetch_add(1, std::memory_order_relaxed);
    const auto s = get_strategy();
    if (s == AuditStrategy::Off)
        return false;
    if (s == AuditStrategy::Full)
        return true;
    maybe_warn_sampled_without_opt_in();
    const auto ratio = get_sample_ratio();
    if (ratio <= 1)
        return true;
    if ((mutation_id % ratio) != 0) {
        g_typed_mutation_audit_counters.samples_skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

// Issue #1894: contextual gate — also force audit when dirty scope is large
// or linear ops are present (self-evo closed-loop must not under-sample
// ownership-sensitive mutations).
// Issue #2053: under production defaults, force any non-zero dirty scope
// so self-modify / hygiene / invariant events are never under-sampled.
// Issue #2223: match_sites_present forces audit under Sampled (mirror linear).
[[nodiscard]] inline bool should_audit_contextual(std::uint64_t mutation_id,
                                                  std::uint64_t nodes_changed,
                                                  bool linear_ops_present = false,
                                                  bool match_sites_present = false) noexcept {
    const auto s = get_strategy();
    if (s == AuditStrategy::Off)
        return false;
    if (s == AuditStrategy::Full) {
        g_typed_mutation_audit_counters.audits_considered.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    const auto force_n =
        production_defaults_active() ? kAuditForceNodesChangedProduction : kAuditForceNodesChanged;
    // Sampled: force-hit for large dirty / linear / ADT match sites / prod.
    if (linear_ops_present || match_sites_present || nodes_changed >= force_n) {
        g_typed_mutation_audit_counters.audits_considered.fetch_add(1, std::memory_order_relaxed);
        g_typed_mutation_audit_counters.contextual_force_audit_total.fetch_add(
            1, std::memory_order_relaxed);
        return true;
    }
    return should_audit(mutation_id);
}

// Issue #2145 Phase A — hard-gate policy:
//   Full strategy OR Strict sandbox OR linear_ops OR match_sites OR nodes >= N
// → always run post_mutation_invariant_check + linear_post_mutate_enforce*
//   and force-rollback on fail (after #2029 partial recovery).
// Sampled + small non-linear non-match dirty → soft path (perf; AC3).
// Off sandbox / Off strategy → no hard gate (AC4).
// Issue #2223: match_sites_present mirrors linear_ops_present force.
[[nodiscard]] inline bool requires_invariant_hard_gate(std::uint64_t nodes_changed,
                                                       bool linear_ops_present, bool strict_sandbox,
                                                       bool match_sites_present = false) noexcept {
    const auto s = get_strategy();
    if (s == AuditStrategy::Off)
        return false;
    if (s == AuditStrategy::Full || strict_sandbox)
        return true;
    // Sampled: contextual force only.
    const auto force_n =
        production_defaults_active() ? kAuditForceNodesChangedProduction : kAuditForceNodesChanged;
    return linear_ops_present || match_sites_present || nodes_changed >= force_n;
}

// Issue #2281: Agent-visible pure decision primitive. Mirrors
// should_audit_contextual + requires_invariant_hard_gate for any
// hypothetical (mid, nodes, linear, strict, match_sites) tuple so
// Agents can predict force-rollback / under-sample without scraping
// multiple schema counters. PURE: no counter bumps, no side effects.
//
// Decision table (AC5 — aligns with #2222 LinearEnforce decision table):
//   ┌──────────┬─────────┬───────┬────────┬─────────┬────────────────────────┐
//   │ Strategy │ Linear  │ Nodes │ Strict │ Match   │ would_audit / hard_gate │
//   │          │         │       │        │         │ force_reason           │
//   ├──────────┼─────────┼───────┼────────┼─────────┼─────────────────────────┤
//   │ Off      │ -       │ -     │ -      │ -       │ false / false / "off"  │
//   │ Full     │ -       │ -     │ -      │ -       │ true  / true  / "full"  │
//   │ Sampled  │ true    │ -     │ -      │ -       │ true  / true  / "linear"│
//   │ Sampled  │ -       │ >=N   │ -      │ -       │ true  / true  / "nodes" │
//   │ Sampled  │ -       │ -     │ -      │ true    │ true  / true  /         │
//   │          │         │       │        │         │ "match-sites"          │
//   │ Sampled  │ -       │ -     │ true   │ -       │ *hit / true / "strict"  │
//   │ Sampled  │ -       │ <N    │ false  │ false   │ *hit / false /          │
//   │          │         │       │        │         │ "sampled-hit"|"skip"    │
//   └──────────┴─────────┴───────┴────────┴─────────┴──────────────────────────┘
//   N = kAuditForceNodesChanged (8) in dev or
//   kAuditForceNodesChangedProduction (1) under production_defaults.
//   *hit = depends on sample_hit = (ratio <= 1) || (mid % ratio == 0).
//
// The `decide` function is the canonical Agent-visible query.
// `evaluator_primitives_query.cpp` exposes it via
// `query:typed-mutation-audit-decision` keys (schema-2281, issue-2281,
// audit-decision-wired, audit-decision-strategy, audit-decision-sample-ratio,
// audit-decision-production-defaults, audit-decision-would-audit,
// audit-decision-would-hard-gate, audit-decision-force-reason).
struct AuditDecision {
    bool would_audit = false;
    bool would_hard_gate = false;
    std::string_view force_reason = "off";
    int strategy = 0; // 0=Off, 1=Sampled, 2=Full
    int sample_ratio = 1;
    bool production_defaults = false;
};

inline AuditDecision decide(std::uint64_t mutation_id, std::uint64_t nodes_changed,
                            bool linear_ops_present, bool strict_sandbox,
                            bool match_sites_present = false) noexcept {
    AuditDecision d;
    d.strategy = static_cast<int>(get_strategy());
    d.sample_ratio = static_cast<int>(get_sample_ratio());
    d.production_defaults = production_defaults_active();

    // Off: no audit, no hard gate.
    if (d.strategy == static_cast<int>(AuditStrategy::Off)) {
        d.force_reason = "off";
        return d;
    }
    // Full: always audit, always hard gate.
    if (d.strategy == static_cast<int>(AuditStrategy::Full)) {
        d.would_audit = true;
        d.would_hard_gate = true;
        d.force_reason = "full";
        return d;
    }
    // Sampled: contextual decisions.
    const auto force_n =
        d.production_defaults ? kAuditForceNodesChangedProduction : kAuditForceNodesChanged;
    const bool context_force =
        linear_ops_present || match_sites_present || nodes_changed >= force_n;
    const bool sample_hit =
        (d.sample_ratio <= 1) || (mutation_id % static_cast<std::uint64_t>(d.sample_ratio)) == 0;

    d.would_audit = context_force || sample_hit;
    d.would_hard_gate = strict_sandbox || context_force;

    // force_reason: priority order (most specific first).
    if (strict_sandbox) {
        d.force_reason = "strict";
    } else if (linear_ops_present) {
        d.force_reason = "linear";
    } else if (match_sites_present) {
        d.force_reason = "match-sites";
    } else if (nodes_changed >= force_n) {
        d.force_reason = d.production_defaults ? "production-nodes" : "nodes";
    } else if (d.would_audit) {
        d.force_reason = "sampled-hit";
    } else {
        d.force_reason = "sampled-skip";
    }
    return d;
}

// Issue #2553: single Agent commit-readiness score
// (solve × linear × blame × truncate + empty-CS matrix).
//
// Pure aggregation of existing commit-face signals so orch can self-throttle
// without joining composite / fidelity / audit keys. Does NOT change commit
// barrier order or reject policy (#2105 / #2345 / #2458 / #2221 / #2108) —
// only folds them into readiness_bp + force_reason + would_allow_commit.
//
// force_reason priority (highest first):
//   empty_cs > truncate/cone_truncate > linear > blame > solve > ok
//
// Inputs are pure flags (no atomics). Live callers may fill hard flags from
// composite_empty_cs_hard_reject_enabled() / truncate_commit_hard_enabled() /
// production_defaults_active(). Soft/Sampled: reason still reported; allow
// stays true when the corresponding hard flag is false (AC4).
//
// readiness_bp bands (informative; allow is authoritative for gate):
//   10000 ok | 7500 soft-empty_cs | 7000 soft-truncate | 5000 soft-blame
//   0 hard-empty_cs | 1000 hard-truncate | 500 hard-linear | 1500 hard-blame
//   2000 TIMEOUT solve | 2500 CONFLICT solve
//
// Schema-2553 on query:type-incremental-fidelity-stats (+ dedicated
// query:typed-mutation-commit-readiness via register_stats_impl).
// Issue #2621: partial_cone_truncated is truncate-class (same hard policy).
struct CommitReadinessInput {
    // 0=SOLVED, 1=CONFLICT, 2=TIMEOUT (SolverSnapshot / SolveResult).
    std::uint8_t solve_status = 0;
    bool linear_ok = true;
    bool blame_ok = true; // last_blame_chain.is_complete() or vacuous
    bool truncated_reverify = false;
    // true when #2458 full-solve recovered after truncated_reverify.
    bool truncated_full_solve_recovered = false;
    // Issue #2621: last infer_flat_partial soft/hard cone truncate (stale
    // residual outside truncated cone). Soft observe / production hard.
    bool partial_cone_truncated = false;
    bool expected_partial = false; // Agent / composite expected partial CS
    bool cs_has_work = false;      // commit_cs_has_work / dirty CS
    // Issue #2610: true when effective expected_partial was auto-set from
    // dirty cone / CS work (Agent left expected_partial false).
    bool auto_partial_from_cone = false;
    // Hard-policy flags (production / Full / env overrides). Soft=false.
    bool empty_cs_hard = false;
    bool truncate_hard = false;
    bool linear_hard = false; // production linear escape hardblock
    bool blame_hard = false;  // production blame-complete gate
    // Issue #2716: occurrence hard-faces (active branch). Under
    // production / Full, the active branch in commit_readiness
    // hard-rejects when the face counter has advanced (counter > 0).
    // Soft / baseline=0: counter-only (no reject, no extra atomics
    // beyond the relaxed load of the face counters).
    bool occurrence_face_hard = false;              // true under prod/Full
    bool cone_outside_goal_drop_face = false;       // #2703 face hit
    bool occurrence_empty_after_fence_face = false; // #2704 face hit
    // Issue #2847: region type/occurrence cross-talk face (active under
    // prod/Full when face latch set by note_region_type_cross_talk).
    bool region_type_cross_talk_face = false;
    // Issue #2911: unified refined-consistency hard gate. production/Full
    // + refined drift (explicit latch or multi-face refined signals) →
    // hard reject / recover. Soft: observe only via counters.
    bool refined_consistency_hard = false;
    bool refined_consistency_drift = false;
    // Issue #3031: pending_full_solve / locality residual at commit.
    // production/Full + residual → escalate then hard-reject if still dirty.
    bool pending_full_solve_hard = false;
    bool pending_full_solve_residual = false;
};

struct CommitReadiness {
    std::uint32_t readiness_bp = 10000;
    bool would_allow_commit = true;
    // empty_cs | auto_partial | truncate | cone_truncate | linear | blame | solve | ok
    std::string_view force_reason = "ok";
    // Stable int: 0=ok 1=solve 2=blame 3=linear 4=truncate 5=empty_cs
    // 6=auto_partial 9=cone_truncate (#2621; 7–8 reserved by #2613 advisory)
    std::int64_t force_reason_code = 0;
};

// Issue #2697: single Agent-holdable proof for composite type×linear
// safety. Lightweight serialisable/hashable struct that
// composite_txn_commit stamps on success or reject. Additive to the
// #2613 type-linear-commit-health query — Agents retrieve the latest
// stamp via `query:last-type-linear-commit-proof`. Pre-remap semantics
// (AC3): Agents re-check after the remap event by comparing
// defuse_or_epoch_stamp against the current live stamp.
struct TypeLinearCommitProof {
    std::uint64_t readiness_bp = 10000;
    std::uint32_t force_reason_code = 0;
    bool would_allow_commit = true;
    bool linear_ok = true;
    bool occurrence_consistent = true;
    std::uint64_t defuse_or_epoch_stamp = 0;
    std::uint64_t live_goal_count = 0;
    std::uint64_t linear_root_count = 0;
    // Issue #2842: bounded fingerprint of live OccurrenceGoals at stamp
    // (var.index + refined.index + pred_nid + mid + epoch, up to N). 0 when
    // empty goals; non-zero when goals non-empty so Agents detect content
    // drift without N-key join (densify/steal prune changes fingerprint).
    std::uint64_t goal_fingerprint = 0;
    std::uint64_t schema = 2697;
};

inline constexpr int kTypeLinearCommitProofIssue = 2697;
// Issue #2842: goal truth freeze residual of #2758.
inline constexpr int kTypeLinearCommitProofGoalTruthIssue = 2842;
// Bound fingerprint walk (soft-cone discipline; no heap beyond existing).
inline constexpr std::size_t kProofGoalFingerprintMaxGoals = 16;

// File-scope atomics (mirror #2693/#2694/#2695/#2696 pattern).
inline std::atomic<std::uint64_t> g_last_type_linear_commit_proof_stamp{0};
inline std::atomic<std::uint32_t> g_type_linear_commit_proof_wired{1};
// Issue #2899: last proof face bits for IR Move/Drop proven fast-path.
// Quiet default 0 → fast-path disabled (zero cost full check).
inline std::atomic<std::uint8_t> g_last_proof_would_allow_commit{0};
inline std::atomic<std::uint8_t> g_last_proof_linear_ok{0};
inline std::atomic<std::uint64_t> g_linear_ir_fastpath_skip_total{0};
inline std::atomic<std::uint64_t> g_linear_ir_fastpath_skip_blocked_total{0};
inline std::atomic<std::uint32_t> g_linear_ir_fastpath_wired{1};
inline constexpr int kLinearIrFastpathIssue = 2899;
inline thread_local std::int32_t g_linear_ir_fastpath_boundary_depth_override{-1};

[[nodiscard]] inline std::uint64_t last_type_linear_commit_proof_stamp_v_read() noexcept {
    return g_last_type_linear_commit_proof_stamp.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t type_linear_commit_proof_wired_v_read() noexcept {
    return g_type_linear_commit_proof_wired.load(std::memory_order_relaxed);
}

inline void stamp_type_linear_commit_proof(std::uint64_t current_epoch_or_defuse) noexcept {
    g_last_type_linear_commit_proof_stamp.store(current_epoch_or_defuse, std::memory_order_relaxed);
}

inline void clear_last_proof_face_for_test() noexcept {
    g_last_proof_would_allow_commit.store(0, std::memory_order_relaxed);
    g_last_proof_linear_ok.store(0, std::memory_order_relaxed);
}

inline void clear_type_linear_commit_proof_for_test() noexcept {
    g_last_type_linear_commit_proof_stamp.store(0, std::memory_order_relaxed);
    clear_last_proof_face_for_test();
}

// Issue #3032: densify/steal rehydrate-miss must invalidate the in-flight
// linear_fast_path face and force a hot deopt/revalidate so Move/Drop
// cannot keep a pre-miss green stamp. Soft: observe only. Quiet: helper
// not called. Green stamp re-binds gen via publish_last_proof_face.
inline constexpr int kRehydrateMissInvalidateIssue = 3032;
inline std::atomic<std::uint64_t> g_rehydrate_miss_invalidate_gen{0};
inline std::atomic<std::uint64_t> g_rehydrate_miss_green_bind_gen{0};
inline std::atomic<std::uint64_t> g_rehydrate_miss_invalidate_total{0};
inline std::atomic<std::uint64_t> g_rehydrate_miss_invalidate_observe_total{0};
inline std::atomic<std::uint64_t> g_rehydrate_miss_force_deopt_total{0};
inline std::atomic<std::uint64_t> g_rehydrate_success_bind_total{0};
inline std::atomic<std::uint64_t> g_rehydrate_success_bound_goals{0};
inline std::atomic<std::uint64_t> g_rehydrate_success_bound_fp{0};
inline std::atomic<std::uint32_t> g_rehydrate_miss_invalidate_wired{1};
// Issue #3063: steal/densify SUCCESS advances the same invalidate_gen
// before restamp so in-flight IR Move cannot elide on a pre-restamp
// green proof. Reuses g_rehydrate_miss_invalidate_gen (no second model).
inline constexpr int kStealDensifySuccessInvalidateIssue = 3063;
inline std::atomic<std::uint64_t> g_steal_densify_success_invalidate_total{0};
inline std::atomic<std::uint32_t> g_steal_densify_success_invalidate_wired{1};

[[nodiscard]] inline std::uint64_t rehydrate_miss_invalidate_gen_v_read() noexcept {
    return g_rehydrate_miss_invalidate_gen.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t rehydrate_miss_invalidate_total_v_read() noexcept {
    return g_rehydrate_miss_invalidate_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t rehydrate_miss_invalidate_observe_total_v_read() noexcept {
    return g_rehydrate_miss_invalidate_observe_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t rehydrate_miss_force_deopt_total_v_read() noexcept {
    return g_rehydrate_miss_force_deopt_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t rehydrate_success_bind_total_v_read() noexcept {
    return g_rehydrate_success_bind_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t steal_densify_success_invalidate_total_v_read() noexcept {
    return g_steal_densify_success_invalidate_total.load(std::memory_order_relaxed);
}
inline void reset_rehydrate_miss_invalidate_for_test() noexcept {
    g_rehydrate_miss_invalidate_gen.store(0, std::memory_order_relaxed);
    g_rehydrate_miss_green_bind_gen.store(0, std::memory_order_relaxed);
    g_rehydrate_miss_invalidate_total.store(0, std::memory_order_relaxed);
    g_rehydrate_miss_invalidate_observe_total.store(0, std::memory_order_relaxed);
    g_rehydrate_miss_force_deopt_total.store(0, std::memory_order_relaxed);
    g_rehydrate_success_bind_total.store(0, std::memory_order_relaxed);
    g_rehydrate_success_bound_goals.store(0, std::memory_order_relaxed);
    g_rehydrate_success_bound_fp.store(0, std::memory_order_relaxed);
    g_steal_densify_success_invalidate_total.store(0, std::memory_order_relaxed);
}

// Purpose: drop green linear_fast_path after densify/steal rehydrate miss
// Pre: caller already stamped Reject (or is about to)
// Post: invalidate_gen advanced under production/Full → linear_fast_path_ok
//       false until a later green face bind; Soft observe only
// Safety Class: P0 under production/Full (stale green Move/Drop is residual)
// Issue: #3032
// AI-Native Rationale: Agents join miss → invalidate → deopt → next stamp
[[nodiscard]] inline bool invalidate_fast_path_on_rehydrate_miss() noexcept {
    const bool hard = production_defaults_active() || get_strategy() == AuditStrategy::Full;
    if (!hard) {
        g_rehydrate_miss_invalidate_observe_total.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_rehydrate_miss_invalidate_total.fetch_add(1, std::memory_order_relaxed);
    g_rehydrate_miss_force_deopt_total.fetch_add(1, std::memory_order_relaxed);
    g_rehydrate_miss_invalidate_gen.fetch_add(1, std::memory_order_release);
    g_last_proof_would_allow_commit.store(0, std::memory_order_relaxed);
    g_last_proof_linear_ok.store(0, std::memory_order_relaxed);
    return true;
}

// Purpose: drop green linear_fast_path before steal/densify restamp
// Pre: production/Full steal or densify is about to restamp
// Post: invalidate_gen advanced (release) so linear_fast_path_ok is false
//       until a later green face bind; Soft/Off: no extra atomics
// Safety Class: P0 under production/Full (half-green Move/Drop residual)
// Issue: #3063 / #3032
// AI-Native Rationale: Agents join restamp → gen bump → deopt → next stamp
[[nodiscard]] inline bool invalidate_fast_path_before_steal_densify_restamp() noexcept {
    const bool hard = production_defaults_active() || get_strategy() == AuditStrategy::Full;
    if (!hard)
        return false;
    g_rehydrate_miss_invalidate_gen.fetch_add(1, std::memory_order_release);
    g_last_proof_would_allow_commit.store(0, std::memory_order_relaxed);
    g_last_proof_linear_ok.store(0, std::memory_order_relaxed);
    g_steal_densify_success_invalidate_total.fetch_add(1, std::memory_order_relaxed);
    return true;
}

inline void note_rehydrate_success_bind(std::uint64_t goals, std::uint64_t fp) noexcept {
    g_rehydrate_success_bound_goals.store(goals, std::memory_order_relaxed);
    g_rehydrate_success_bound_fp.store(fp, std::memory_order_relaxed);
    g_rehydrate_success_bind_total.fetch_add(1, std::memory_order_relaxed);
}

inline void publish_last_proof_face(bool would_allow, bool linear_ok) noexcept {
    g_last_proof_would_allow_commit.store(would_allow ? 1 : 0, std::memory_order_relaxed);
    g_last_proof_linear_ok.store(linear_ok ? 1 : 0, std::memory_order_relaxed);
    // Issue #3032: a fresh green face re-binds invalidate gen so Move/Drop
    // may elide again only after the miss generation is acknowledged.
    if (would_allow && linear_ok)
        g_rehydrate_miss_green_bind_gen.store(
            g_rehydrate_miss_invalidate_gen.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
}

// Issue #2717: active stamp inside boundary + composite commit. The
// low-level stamp_type_linear_commit_proof above only stores the
// epoch — the proof struct itself is built on-the-fly by the query
// path. #2717 fills the proof from live state at the four stamping
// sites (boundary success, boundary reject, composite ok, composite
// reject) so Agents can hold a single TypeLinearCommitProof across
// densify / steal / remap and re-check defuse_or_epoch_stamp without
// re-joining N query surfaces. The struct is returned by value; the
// epoch stamp is stored to g_last_type_linear_commit_proof_stamp so
// the existing query:last-type-linear-commit-proof path stays
// additive. The new counter g_type_linear_commit_proof_stamped_total
// bumps once per call (additive — no regression on #2613 / #2697).
inline std::atomic<std::uint64_t> g_type_linear_commit_proof_stamped_total{0};
[[nodiscard]] inline std::uint64_t type_linear_commit_proof_stamped_total_v_read() noexcept {
    return g_type_linear_commit_proof_stamped_total.load(std::memory_order_relaxed);
}
inline void reset_type_linear_commit_proof_stamped_total_for_test() noexcept {
    g_type_linear_commit_proof_stamped_total.store(0, std::memory_order_relaxed);
}

// Issue #2758: last stamped root/goal counts (Agent drift detect without
// N-key join). Updated every build_type_linear_commit_proof_from_live.
inline std::atomic<std::uint64_t> g_last_proof_live_goal_count{0};
inline std::atomic<std::uint64_t> g_last_proof_linear_root_count{0};
// Issue #2842: last stamped goal fingerprint (content drift detect).
inline std::atomic<std::uint64_t> g_last_proof_goal_fingerprint{0};
// Optional dashboard: how often at least one count was non-zero.
inline std::atomic<std::uint64_t> g_type_linear_commit_proof_counts_filled_total{0};
// Issue #2842: stamp used CS occurrence_goals_size() (+ fingerprint) truth.
inline std::atomic<std::uint64_t> g_type_linear_commit_proof_goal_truth_stamped_total{0};
// Issue #2842: stamped fingerprint was non-zero (goals non-empty).
inline std::atomic<std::uint64_t> g_type_linear_commit_proof_goal_fingerprint_nonzero_total{0};
// Issue #2842: production stamp fell back to gauge (CS pointer unavailable).
inline std::atomic<std::uint64_t> g_type_linear_commit_proof_goal_truth_gauge_fallback_total{0};
// Issue #2938: outermost success is sole authority that freezes Occurrence
// truth into the long-lived persist side buffer. Counters bump only when
// maybe_persist_occurrence_snapshot actually writes entries (production /
// Full + non-empty goals). Soft / empty / reject → zero (AC2/AC3).
inline constexpr int kOccurrenceCommitSnapshotIssue = 2938;
inline std::atomic<std::uint64_t> g_occurrence_commit_snapshot_written_total{0};
inline std::atomic<std::uint64_t> g_occurrence_commit_snapshot_mid{0};
[[nodiscard]] inline std::uint64_t occurrence_commit_snapshot_written_total_v_read() noexcept {
    return g_occurrence_commit_snapshot_written_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t occurrence_commit_snapshot_mid_v_read() noexcept {
    return g_occurrence_commit_snapshot_mid.load(std::memory_order_relaxed);
}
inline void note_occurrence_commit_snapshot_written(std::uint64_t mid,
                                                    std::uint64_t entries_written) noexcept {
    if (entries_written == 0)
        return;
    g_occurrence_commit_snapshot_written_total.fetch_add(1, std::memory_order_relaxed);
    if (mid != 0)
        g_occurrence_commit_snapshot_mid.store(mid, std::memory_order_relaxed);
}
inline void reset_occurrence_commit_snapshot_for_test() noexcept {
    g_occurrence_commit_snapshot_written_total.store(0, std::memory_order_relaxed);
    g_occurrence_commit_snapshot_mid.store(0, std::memory_order_relaxed);
}

// Issue #3004: persist + Full audit is the sole query:type authority
// moment. Production infer SOLVED is in-flight. Failure discards
// provisional live OccurrenceGoals (restore last durable persist).
inline constexpr int kOccurrencePersistAuditAtomicIssue = 3004;
inline std::atomic<std::uint64_t> g_occurrence_provisional_discard_total{0};
inline std::atomic<std::uint64_t> g_occurrence_provisional_discard_goals_total{0};
inline std::atomic<std::uint32_t> g_occurrence_persist_audit_atomic_wired{1};
[[nodiscard]] inline std::uint64_t occurrence_provisional_discard_total_v_read() noexcept {
    return g_occurrence_provisional_discard_total.load(std::memory_order_relaxed);
}
inline void note_occurrence_provisional_discard(std::uint64_t goals_dropped) noexcept {
    g_occurrence_provisional_discard_total.fetch_add(1, std::memory_order_relaxed);
    if (goals_dropped > 0)
        g_occurrence_provisional_discard_goals_total.fetch_add(goals_dropped,
                                                               std::memory_order_relaxed);
}
inline void reset_occurrence_provisional_discard_for_test() noexcept {
    g_occurrence_provisional_discard_total.store(0, std::memory_order_relaxed);
    g_occurrence_provisional_discard_goals_total.store(0, std::memory_order_relaxed);
}

// Issue #2995: last OccurrenceCommitHealth snapshot + ensure counters.
// Soft + empty / no faces: evaluate is pure loads (these stay at reset).
// ensure_* only fetch_adds when production needs_recover.
inline constexpr int kOccurrenceCommitHealthIssue = 2995;
inline std::atomic<std::uint32_t> g_occurrence_commit_health_faces{0};
inline std::atomic<std::uint64_t> g_occurrence_commit_health_goals_live{0};
inline std::atomic<std::uint64_t> g_occurrence_commit_health_persist_size{0};
inline std::atomic<std::uint8_t> g_occurrence_commit_health_needs_recover{0};
inline std::atomic<std::uint8_t> g_occurrence_commit_health_recovered_ok{0};
inline std::atomic<std::uint8_t> g_occurrence_commit_health_fingerprint_ok{1};
inline std::atomic<std::uint32_t> g_occurrence_commit_health_wired{1};
inline std::atomic<std::uint64_t> g_occurrence_commit_health_ensure_total{0};
inline std::atomic<std::uint64_t> g_occurrence_commit_health_recover_ok_total{0};
inline std::atomic<std::uint64_t> g_occurrence_commit_health_recover_fail_total{0};
[[nodiscard]] inline std::uint8_t occurrence_commit_health_recovered_ok_v_read() noexcept {
    return g_occurrence_commit_health_recovered_ok.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t occurrence_commit_health_ensure_total_v_read() noexcept {
    return g_occurrence_commit_health_ensure_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t occurrence_commit_health_recover_ok_total_v_read() noexcept {
    return g_occurrence_commit_health_recover_ok_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t occurrence_commit_health_recover_fail_total_v_read() noexcept {
    return g_occurrence_commit_health_recover_fail_total.load(std::memory_order_relaxed);
}
inline void publish_occurrence_commit_health(std::uint32_t faces, std::uint64_t goals_live,
                                             std::uint64_t persist_size, bool needs_recover,
                                             bool recovered_ok, bool fingerprint_ok) noexcept {
    g_occurrence_commit_health_faces.store(faces, std::memory_order_relaxed);
    g_occurrence_commit_health_goals_live.store(goals_live, std::memory_order_relaxed);
    g_occurrence_commit_health_persist_size.store(persist_size, std::memory_order_relaxed);
    g_occurrence_commit_health_needs_recover.store(needs_recover ? 1 : 0,
                                                   std::memory_order_relaxed);
    g_occurrence_commit_health_recovered_ok.store(recovered_ok ? 1 : 0, std::memory_order_relaxed);
    g_occurrence_commit_health_fingerprint_ok.store(fingerprint_ok ? 1 : 0,
                                                    std::memory_order_relaxed);
}
inline void reset_occurrence_commit_health_for_test() noexcept {
    g_occurrence_commit_health_faces.store(0, std::memory_order_relaxed);
    g_occurrence_commit_health_goals_live.store(0, std::memory_order_relaxed);
    g_occurrence_commit_health_persist_size.store(0, std::memory_order_relaxed);
    g_occurrence_commit_health_needs_recover.store(0, std::memory_order_relaxed);
    g_occurrence_commit_health_recovered_ok.store(0, std::memory_order_relaxed);
    g_occurrence_commit_health_fingerprint_ok.store(1, std::memory_order_relaxed);
    g_occurrence_commit_health_ensure_total.store(0, std::memory_order_relaxed);
    g_occurrence_commit_health_recover_ok_total.store(0, std::memory_order_relaxed);
    g_occurrence_commit_health_recover_fail_total.store(0, std::memory_order_relaxed);
}
// Process gauge published by stamp sites / query when CS goals known.
// Quiet default 0 (no CS / empty goals). Gauge is fallback-only under
// production when CS is unavailable (#2842) — prefer CS size at stamp.
inline std::atomic<std::uint64_t> g_proof_live_goal_count_gauge{0};
inline constexpr std::uint64_t kProofLiveGoalCountHintAuto =
    static_cast<std::uint64_t>(~std::uint64_t{0});
// Sentinel: stamp site could not freeze CS truth (use gauge + note miss).
inline constexpr std::uint64_t kProofGoalTruthFromGauge =
    static_cast<std::uint64_t>(~std::uint64_t{0}) - 1;

// Issue #2854: same-transaction order — proof stamping is gated on the
// rebind + scan outcome so a success proof can never outlive a failed
// rebind on the same exit (#2854 AC2). Two new cumulative counters:
//   - g_type_linear_proof_stamped_after_rebind_total bumps when the
//     proof was stamped AFTER a successful rebind + scan (linear_root_count
//     reflects the post-remap collect). Quiet empty path stays zero-cost
//     (AC4 #2723 — no rebind attempted, no counter bump).
//   - g_type_linear_proof_reject_after_rebind_fail_total bumps when the
//     rebind OR scan failed AND production / Full audit route forced a
//     reject proof (would_allow_commit=false, linear_ok=false). Soft
//     mismatch path under non-prod does NOT bump this counter (Soft
//     observe is per #2673 contract — separate counter lives in the
//     densify scan path). No success proof may outlive a failed rebind.
inline std::atomic<std::uint64_t> g_type_linear_proof_stamped_after_rebind_total{0};
inline std::atomic<std::uint64_t> g_type_linear_proof_reject_after_rebind_fail_total{0};
// Last-stamp outcome sentinel for Agent drift detect (mirrors
// g_last_proof_linear_root_count). 0=Quiet (no rebind attempted),
// 1=Stamped (rebind+scan ok), 2=Reject (rebind fail OR scan mismatch
// under prod). Pairs with type_linear_commit_proof_stamped_total to
// distinguish pre-#2854 success stamps from post-#2854 ordered stamps.
inline std::atomic<std::uint8_t> g_last_type_linear_proof_outcome{0}; // 0/1/2 per above
inline constexpr int kTypeLinearProofSameTransactionOrderIssue = 2854;

[[nodiscard]] inline std::uint64_t type_linear_proof_stamped_after_rebind_total_v_read() noexcept {
    return g_type_linear_proof_stamped_after_rebind_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t
type_linear_proof_reject_after_rebind_fail_total_v_read() noexcept {
    return g_type_linear_proof_reject_after_rebind_fail_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint8_t last_type_linear_proof_outcome_v_read() noexcept {
    return g_last_type_linear_proof_outcome.load(std::memory_order_relaxed);
}
inline void publish_type_linear_proof_outcome(uint8_t outcome) noexcept {
    g_last_type_linear_proof_outcome.store(outcome, std::memory_order_relaxed);
}
inline void clear_type_linear_proof_outcome_for_test() noexcept {
    g_last_type_linear_proof_outcome.store(0, std::memory_order_relaxed);
}
inline void reset_type_linear_proof_same_transaction_counters_for_test() noexcept {
    g_type_linear_proof_stamped_after_rebind_total.store(0, std::memory_order_relaxed);
    g_type_linear_proof_reject_after_rebind_fail_total.store(0, std::memory_order_relaxed);
    g_last_type_linear_proof_outcome.store(0, std::memory_order_relaxed);
}
inline constexpr uint8_t kTypeLinearProofOutcomeQuiet = 0;
inline constexpr uint8_t kTypeLinearProofOutcomeStamped = 1;

// Issue #2981: same-transaction TypeLinearCommitProof bind. Production/Full
// + #2704 hard face + empty CS goals → proof must not be green (prefer
// CS occurrence_goals_size / fingerprint over gauge). Soft: observe only.
// occurrence_empty_after_fence_total_v_read is forward-declared above.
inline std::atomic<std::uint64_t> g_type_linear_proof_reject_empty_after_fence_total{0};
inline constexpr int kTypeLinearProofEmptyAfterFenceIssue = 2981;

[[nodiscard]] inline bool
occurrence_empty_after_fence_blocks_proof(std::uint64_t live_goal_count) noexcept {
    if (live_goal_count != 0)
        return false; // AC3: CS truth non-empty → fingerprint path
    if (!(production_defaults_active() || get_strategy() == AuditStrategy::Full))
        return false; // AC2: Soft observe only
    return occurrence_empty_after_fence_total_v_read() > 0;
}

[[nodiscard]] inline std::uint64_t
type_linear_proof_reject_empty_after_fence_total_v_read() noexcept {
    return g_type_linear_proof_reject_empty_after_fence_total.load(std::memory_order_relaxed);
}

inline void reset_type_linear_proof_reject_empty_after_fence_for_test() noexcept {
    g_type_linear_proof_reject_empty_after_fence_total.store(0, std::memory_order_relaxed);
}
// kTypeLinearProofOutcomeReject = 2 declared below with publish helpers.

[[nodiscard]] inline std::uint8_t last_proof_would_allow_commit_v_read() noexcept {
    return g_last_proof_would_allow_commit.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint8_t last_proof_linear_ok_v_read() noexcept {
    return g_last_proof_linear_ok.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t linear_ir_fastpath_skip_total_v_read() noexcept {
    return g_linear_ir_fastpath_skip_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t linear_ir_fastpath_skip_blocked_total_v_read() noexcept {
    return g_linear_ir_fastpath_skip_blocked_total.load(std::memory_order_relaxed);
}

inline void reset_linear_ir_fastpath_counters_for_test() noexcept {
    g_linear_ir_fastpath_skip_total.store(0, std::memory_order_relaxed);
    g_linear_ir_fastpath_skip_blocked_total.store(0, std::memory_order_relaxed);
    g_linear_ir_fastpath_boundary_depth_override = -1;
}

// Issue #2964: unified Linear fast-path eligibility (symmetric to #2899 IR
// Move/Drop elision). Pure preferred — no counter side effects.
//   linear_fast_path_ok =
//     proof.fresh && would_allow && linear_ok
//     && boundary_depth==0 && !escape_gate && !densify_pending
// Any arm false → IR must not elide; outermost MutationBoundary success
// under production/Full must force dirty-root linear revalidate (AC2).
inline constexpr int kLinearFastPathUnifiedIssue = 2964;
inline std::atomic<std::uint64_t> g_linear_fast_path_force_revalidate_total{0};
inline std::atomic<std::uint64_t> g_linear_fast_path_force_revalidate_observe_total{0};
inline std::atomic<std::uint32_t> g_linear_fast_path_unified_wired{1};

[[nodiscard]] inline std::uint64_t linear_fast_path_force_revalidate_total_v_read() noexcept {
    return g_linear_fast_path_force_revalidate_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t
linear_fast_path_force_revalidate_observe_total_v_read() noexcept {
    return g_linear_fast_path_force_revalidate_observe_total.load(std::memory_order_relaxed);
}
inline void reset_linear_fast_path_force_revalidate_for_test() noexcept {
    g_linear_fast_path_force_revalidate_total.store(0, std::memory_order_relaxed);
    g_linear_fast_path_force_revalidate_observe_total.store(0, std::memory_order_relaxed);
}

// Issue #3006: residual of #2964 — Production/Full exit with !ok must
// run dirty-root linear revalidate (enforce_linear_boundary_consistency),
// not just EnvFrame sweep. Late re-eval after Phase 1 catches
// escape / densify / depth flips after the initial check. Soft observe.
inline constexpr int kLinearFastPathDirtyRevalidateIssue = 3006;
inline std::atomic<std::uint64_t> g_linear_fast_path_dirty_revalidate_total{0};
inline std::atomic<std::uint64_t> g_linear_fast_path_late_reeval_total{0};
inline std::atomic<std::uint64_t> g_linear_fast_path_elide_blocked_production_total{0};
inline std::atomic<std::uint32_t> g_linear_fast_path_dirty_revalidate_wired{1};

[[nodiscard]] inline std::uint64_t linear_fast_path_dirty_revalidate_total_v_read() noexcept {
    return g_linear_fast_path_dirty_revalidate_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t linear_fast_path_late_reeval_total_v_read() noexcept {
    return g_linear_fast_path_late_reeval_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t
linear_fast_path_elide_blocked_production_total_v_read() noexcept {
    return g_linear_fast_path_elide_blocked_production_total.load(std::memory_order_relaxed);
}
inline void reset_linear_fast_path_dirty_revalidate_for_test() noexcept {
    g_linear_fast_path_dirty_revalidate_total.store(0, std::memory_order_relaxed);
    g_linear_fast_path_late_reeval_total.store(0, std::memory_order_relaxed);
    g_linear_fast_path_elide_blocked_production_total.store(0, std::memory_order_relaxed);
}

// Purpose: single pure eligibility for Linear IR fast-path + boundary revalidate
// Pre: none (relaxed loads only)
// Post: true iff proof fresh, linear_ok, outermost, no escape, no densify-pending
// Safety Class: P1 (perf gate; false → full check / force revalidate)
// Issue: #2964 / #2899
// AI-Native Rationale: one predicate Agents can mirror for elision vs revalidate
[[nodiscard]] inline bool linear_fast_path_ok() noexcept {
    // proof.fresh — no stamp → not eligible (quiet full check)
    if (g_last_type_linear_commit_proof_stamp.load(std::memory_order_relaxed) == 0)
        return false;
    // Reject outcome never eligible
    if (g_last_type_linear_proof_outcome.load(std::memory_order_relaxed) ==
        /*Reject*/ 2)
        return false;
    // proof.linear_ok && would_allow_commit
    if (g_last_proof_would_allow_commit.load(std::memory_order_relaxed) == 0 ||
        g_last_proof_linear_ok.load(std::memory_order_relaxed) == 0)
        return false;
    // mid MutationBoundary arm (#2964 AC3) — boundary_depth > 0 disables
    {
        std::size_t depth = 0;
        if (g_linear_ir_fastpath_boundary_depth_override >= 0) {
            depth = static_cast<std::size_t>(g_linear_ir_fastpath_boundary_depth_override);
        } else {
            depth = aura_evaluator_mutation_boundary_depth();
        }
        if (depth > 0)
            return false;
    }
    // escape gate arm (#2964 AC3 / #2263)
    if (aura_escape_move_gate_active() != 0)
        return false;
    // densify-pending arm (#2964 AC3 / densify Phase-5 scan inject)
    if (g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.load(
            std::memory_order_relaxed) > 0)
        return false;
    // Issue #3032 / #3063: invalidate gen (steal/densify miss *or* success
    // restamp) must match last green bind. Acquire pairs with release
    // fetch_add so an in-flight IR Move cannot elide after gen advances.
    if (g_rehydrate_miss_invalidate_gen.load(std::memory_order_acquire) !=
        g_rehydrate_miss_green_bind_gen.load(std::memory_order_relaxed))
        return false;
    return true;
}

// Boundary exit action when !linear_fast_path_ok (AC2).
// Quiet when ok; SoftObserve under Soft; ForceRevalidate under production/Full.
enum class LinearFastPathExitAction : std::uint8_t {
    Quiet = 0,
    SoftObserve = 1,
    ForceRevalidate = 2,
};

// Purpose: decide post-mutate revalidate for outermost MutationBoundary success
// Pre: prefer call when depth already 0 (after exit_mutation_boundary pop)
// Post: Quiet → no extra work; SoftObserve → observe counter only;
//       ForceRevalidate → caller must revalidate dirty linear roots
// Safety Class: P0 under ForceRevalidate (no silent skip of revalidate)
// Issue: #2964
// AI-Native Rationale: inverse of IR elision — stale proof forces revalidate
[[nodiscard]] inline LinearFastPathExitAction linear_fast_path_boundary_exit_action() noexcept {
    if (linear_fast_path_ok())
        return LinearFastPathExitAction::Quiet; // AC4 zero extra revalidate
    const bool hard = production_defaults_active() || get_strategy() == AuditStrategy::Full;
    if (hard)
        return LinearFastPathExitAction::ForceRevalidate;
    return LinearFastPathExitAction::SoftObserve;
}

// Purpose: IR Move/Drop may skip redundant provenance re-sim when proof fresh
// Pre: none (pure relaxed loads + counter bumps)
// Post: true → caller may skip enforce provenance; counter bumped
// Safety Class: P1 (performance; never weakens #2108/#2563 — escape/depth/reject block)
// Issue: #2899 / #2964
// AI-Native Rationale: high-frequency mutate loops skip audit when proof says linear_ok
[[nodiscard]] inline bool linear_ir_fastpath_try_skip() noexcept {
    // AC3 / zero cost: no recent proof stamp → full check, no counter noise.
    if (g_last_type_linear_commit_proof_stamp.load(std::memory_order_relaxed) == 0)
        return false;
    // Issue #2964: single predicate drives elision (preserve #2899 counters).
    if (!linear_fast_path_ok()) {
        g_linear_ir_fastpath_skip_blocked_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #3006: Production / Full never elides under a false predicate.
        if (production_defaults_active() || get_strategy() == AuditStrategy::Full)
            g_linear_fast_path_elide_blocked_production_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
        return false;
    }
    g_linear_ir_fastpath_skip_total.fetch_add(1, std::memory_order_relaxed);
    return true;
}
inline constexpr uint8_t kTypeLinearProofOutcomeReject = 2;

// Issue #3030: abort / force-rollback must drop the last TypeLinearCommitProof
// + linear_fast_path face so a later IR Move/Drop cannot elide on a
// pre-abort stamp (half-green). Reuses the existing stamp/face/outcome
// atomics — no second proof model. Soft: observe-only counter; face
// still cleared (Soft never relies on stamp for commit). Quiet (no
// face): zero extra stores beyond the four face writes (idempotent).
inline constexpr int kTypeLinearProofClearedOnAbortIssue = 3030;
inline std::atomic<std::uint64_t> g_type_linear_proof_cleared_on_abort_total{0};
inline std::atomic<std::uint64_t> g_type_linear_proof_cleared_on_abort_observe_total{0};
inline std::atomic<std::uint32_t> g_type_linear_proof_cleared_on_abort_wired{1};

[[nodiscard]] inline std::uint64_t type_linear_proof_cleared_on_abort_total_v_read() noexcept {
    return g_type_linear_proof_cleared_on_abort_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t
type_linear_proof_cleared_on_abort_observe_total_v_read() noexcept {
    return g_type_linear_proof_cleared_on_abort_observe_total.load(std::memory_order_relaxed);
}
inline void reset_type_linear_proof_cleared_on_abort_for_test() noexcept {
    g_type_linear_proof_cleared_on_abort_total.store(0, std::memory_order_relaxed);
    g_type_linear_proof_cleared_on_abort_observe_total.store(0, std::memory_order_relaxed);
}

// Purpose: drop last TypeLinearCommitProof + densify-pending inject on abort
// Pre: call after abort_restore_dual_topology / hard force-rollback
// Post: stamp=0, would_allow=0, linear_ok=0, outcome=Reject when a face
//       was live; linear_fast_path_ok() == false until a fresh stamp
// Safety Class: P0 under production/Full (missing clear is a hard residual)
// Issue: #3030
// AI-Native Rationale: Agents correlate abort → proof-clear → next boundary
inline void clear_type_linear_commit_proof_on_abort() noexcept {
    const auto stamp = g_last_type_linear_commit_proof_stamp.load(std::memory_order_relaxed);
    const auto would = g_last_proof_would_allow_commit.load(std::memory_order_relaxed);
    const auto lok = g_last_proof_linear_ok.load(std::memory_order_relaxed);
    const bool had_face = stamp != 0 || would != 0 || lok != 0;
    g_last_type_linear_commit_proof_stamp.store(0, std::memory_order_relaxed);
    g_last_proof_would_allow_commit.store(0, std::memory_order_relaxed);
    g_last_proof_linear_ok.store(0, std::memory_order_relaxed);
    if (had_face)
        g_last_type_linear_proof_outcome.store(kTypeLinearProofOutcomeReject,
                                               std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
    if (!had_face)
        return;
    const bool hard = production_defaults_active() || get_strategy() == AuditStrategy::Full;
    if (hard)
        g_type_linear_proof_cleared_on_abort_total.fetch_add(1, std::memory_order_relaxed);
    else
        g_type_linear_proof_cleared_on_abort_observe_total.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t last_proof_live_goal_count_v_read() noexcept {
    return g_last_proof_live_goal_count.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t last_proof_linear_root_count_v_read() noexcept {
    return g_last_proof_linear_root_count.load(std::memory_order_relaxed);
}

// Issue #2984: arena compact vs last TypeLinearCommitProof.linear_root_count.
// Quiet (last==0): no collect. Soft: observe only. Production/Full mismatch
// latches a reject face so the next Success stamp cannot stay green.
inline constexpr int kLinearCompactRootConsistencyIssue = 2984;
inline std::atomic<std::uint8_t> g_linear_compact_root_mismatch_face{0};
inline std::atomic<std::uint64_t> g_linear_compact_root_check_total{0};
inline std::atomic<std::uint64_t> g_linear_compact_root_mismatch_observe_total{0};
inline std::atomic<std::uint64_t> g_linear_compact_root_mismatch_total{0};
inline std::atomic<std::uint32_t> g_linear_compact_root_mismatch_wired{1};

inline void set_last_proof_linear_root_count_for_test(std::uint64_t n) noexcept {
    g_last_proof_linear_root_count.store(n, std::memory_order_relaxed);
}

inline void reset_linear_compact_root_consistency_for_test() noexcept {
    g_linear_compact_root_mismatch_face.store(0, std::memory_order_relaxed);
    g_linear_compact_root_check_total.store(0, std::memory_order_relaxed);
    g_linear_compact_root_mismatch_observe_total.store(0, std::memory_order_relaxed);
    g_linear_compact_root_mismatch_total.store(0, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t linear_compact_root_check_total_v_read() noexcept {
    return g_linear_compact_root_check_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t linear_compact_root_mismatch_observe_total_v_read() noexcept {
    return g_linear_compact_root_mismatch_observe_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t linear_compact_root_mismatch_total_v_read() noexcept {
    return g_linear_compact_root_mismatch_total.load(std::memory_order_relaxed);
}

[[nodiscard]] inline bool linear_compact_root_mismatch_blocks_proof() noexcept {
    if (!(production_defaults_active() || get_strategy() == AuditStrategy::Full))
        return false;
    return g_linear_compact_root_mismatch_face.load(std::memory_order_relaxed) != 0;
}
[[nodiscard]] inline std::uint64_t last_proof_goal_fingerprint_v_read() noexcept {
    return g_last_proof_goal_fingerprint.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t type_linear_commit_proof_counts_filled_total_v_read() noexcept {
    return g_type_linear_commit_proof_counts_filled_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t
type_linear_commit_proof_goal_truth_stamped_total_v_read() noexcept {
    return g_type_linear_commit_proof_goal_truth_stamped_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t
type_linear_commit_proof_goal_fingerprint_nonzero_total_v_read() noexcept {
    return g_type_linear_commit_proof_goal_fingerprint_nonzero_total.load(
        std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t
type_linear_commit_proof_goal_truth_gauge_fallback_total_v_read() noexcept {
    return g_type_linear_commit_proof_goal_truth_gauge_fallback_total.load(
        std::memory_order_relaxed);
}
inline void publish_proof_live_goal_count(std::uint64_t n) noexcept {
    g_proof_live_goal_count_gauge.store(n, std::memory_order_relaxed);
}
inline void clear_proof_goal_truth_for_test() noexcept {
    g_last_proof_goal_fingerprint.store(0, std::memory_order_relaxed);
    g_type_linear_commit_proof_goal_truth_stamped_total.store(0, std::memory_order_relaxed);
    g_type_linear_commit_proof_goal_fingerprint_nonzero_total.store(0, std::memory_order_relaxed);
    g_type_linear_commit_proof_goal_truth_gauge_fallback_total.store(0, std::memory_order_relaxed);
    g_last_proof_live_goal_count.store(0, std::memory_order_relaxed);
    g_proof_live_goal_count_gauge.store(0, std::memory_order_relaxed);
}

// Issue #2842: mix one OccurrenceGoal into a bounded fingerprint.
// Fields: var.index + refined.index + pred_nid + mid + epoch (issue AC).
// Pure POD — stamp sites iterate CS goals and call this (header cannot
// import TypeChecker / OccurrenceGoal module).
[[nodiscard]] inline std::uint64_t
mix_occurrence_goal_into_fingerprint(std::uint64_t h, std::uint32_t var_index,
                                     std::uint32_t refined_index, std::uint32_t predicate_cond_node,
                                     std::uint64_t source_mutation_id,
                                     std::uint64_t epoch) noexcept {
    // Boost-style hash_combine (stable, no heap).
    auto mix = [](std::uint64_t seed, std::uint64_t v) noexcept -> std::uint64_t {
        seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        return seed;
    };
    h = mix(h, static_cast<std::uint64_t>(var_index));
    h = mix(h, static_cast<std::uint64_t>(refined_index));
    h = mix(h, static_cast<std::uint64_t>(predicate_cond_node));
    h = mix(h, source_mutation_id);
    h = mix(h, epoch);
    return h;
}

// Issue #2842: frozen goal truth at stamp. from_cs=true when caller read
// occurrence_goals_size() + fingerprint from live CS (preferred). from_cs=
// false means gauge fallback (production miss counter bumped by builder).
struct ProofGoalTruth {
    std::uint64_t live_goal_count = 0;
    std::uint64_t goal_fingerprint = 0;
    bool from_cs = false;
};

// Quiet default (empty goals, zero extra cost).
inline constexpr ProofGoalTruth kQuietProofGoalTruth{};

// Apply goal truth into proof + gauges. from_cs path bumps truth-stamped
// counter; non-empty fingerprint bumps nonzero counter. Gauge fallback
// under production bumps miss counter (AC Soft vs production table).
inline void apply_proof_goal_truth(TypeLinearCommitProof& p, const ProofGoalTruth& truth) noexcept {
    p.live_goal_count = truth.live_goal_count;
    p.goal_fingerprint = truth.goal_fingerprint;
    g_proof_live_goal_count_gauge.store(truth.live_goal_count, std::memory_order_relaxed);
    g_last_proof_live_goal_count.store(p.live_goal_count, std::memory_order_relaxed);
    g_last_proof_goal_fingerprint.store(p.goal_fingerprint, std::memory_order_relaxed);
    if (truth.from_cs) {
        g_type_linear_commit_proof_goal_truth_stamped_total.fetch_add(1, std::memory_order_relaxed);
    } else if (production_defaults_active()) {
        // Gauge-only under production: CS pointer unavailable at stamp.
        g_type_linear_commit_proof_goal_truth_gauge_fallback_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    if (p.goal_fingerprint != 0) {
        g_type_linear_commit_proof_goal_fingerprint_nonzero_total.fetch_add(
            1, std::memory_order_relaxed);
    }
}

// Resolve goal truth from optional CS hint. When live_goal_count_hint is
// kProofLiveGoalCountHintAuto and fingerprint is 0 with !from_cs, use gauge
// (legacy #2758 path). When hint is explicit CS size, from_cs should be true.
[[nodiscard]] inline ProofGoalTruth resolve_proof_goal_truth(std::uint64_t live_goal_count_hint,
                                                             std::uint64_t goal_fingerprint,
                                                             bool goal_truth_from_cs) noexcept {
    ProofGoalTruth t{};
    if (goal_truth_from_cs || live_goal_count_hint != kProofLiveGoalCountHintAuto) {
        t.live_goal_count =
            (live_goal_count_hint == kProofLiveGoalCountHintAuto) ? 0 : live_goal_count_hint;
        t.goal_fingerprint = goal_fingerprint;
        // Non-empty goals with zero fingerprint is invalid under CS truth —
        // force a non-zero sentinel so Agents still see content present.
        if (t.live_goal_count > 0 && t.goal_fingerprint == 0)
            t.goal_fingerprint = 1;
        // Empty goals → fingerprint must be 0 (quiet).
        if (t.live_goal_count == 0)
            t.goal_fingerprint = 0;
        t.from_cs = goal_truth_from_cs || (live_goal_count_hint != kProofLiveGoalCountHintAuto);
        return t;
    }
    // Gauge fallback (no CS at stamp).
    t.live_goal_count = g_proof_live_goal_count_gauge.load(std::memory_order_relaxed);
    t.goal_fingerprint = g_last_proof_goal_fingerprint.load(std::memory_order_relaxed);
    if (t.live_goal_count == 0)
        t.goal_fingerprint = 0;
    t.from_cs = false;
    return t;
}

// Build a TypeLinearCommitProof from live state. Pure read of existing
// surfaces + collect_linear_or_dirty_roots_for_rebind (#2723/#2742) for
// linear_root_count. live_goal_count from optional hint (stamp site with
// TypeChecker CS) or process gauge (default 0). Cheap on quiet path:
// empty collect short-circuit + zero goals → both counts 0 (AC2 #2758).
// Issue #2842: goal_fingerprint frozen with live_goal_count when CS truth
// is passed (from_cs); gauge is fallback only when CS unavailable.
// Fields:
//   - readiness_bp / force_reason_code / would_allow_commit: from
//     commit_readiness_live_policy().
//   - linear_ok / occurrence_consistent: from live readiness input.
//   - defuse_or_epoch_stamp: caller current_epoch_or_defuse.
//   - live_goal_count + linear_root_count: real walks (#2758; was zero
//     hard-code under #2717 / #2708 residual).
//   - goal_fingerprint: #2842 bounded content hash of live goals.
// live_goal_count_hint: UINT64_MAX = use process gauge; else use hint.
// goal_truth_from_cs: true when hint came from occurrence_goals_size().
inline TypeLinearCommitProof build_type_linear_commit_proof_from_live(
    std::uint64_t current_epoch_or_defuse,
    std::uint64_t live_goal_count_hint = kProofLiveGoalCountHintAuto,
    std::uint64_t goal_fingerprint = 0, bool goal_truth_from_cs = false) noexcept {
    TypeLinearCommitProof p{};
    const auto ready = commit_readiness_live_policy();
    const auto live_r = commit_readiness(ready);
    p.readiness_bp = live_r.readiness_bp;
    p.force_reason_code = static_cast<std::uint32_t>(live_r.force_reason_code);
    p.would_allow_commit = live_r.would_allow_commit;
    p.linear_ok = ready.linear_ok;
    p.occurrence_consistent = ready.cs_has_work || !ready.expected_partial;
    p.defuse_or_epoch_stamp = current_epoch_or_defuse;
    // Issue #2758: real linear_root_count via collect_linear_or_dirty_roots
    // (#2723 nonempty span + #2742 dirty-pin fallback). Quiet path:
    // empty span → 0, no extra alloc beyond existing short-circuit.
    p.linear_root_count =
        static_cast<std::uint64_t>(aura::compiler::linear_or_dirty_roots_count_for_rebind());
    // Issue #2842 / #2758: freeze goal truth (CS size + fingerprint preferred).
    const auto truth =
        resolve_proof_goal_truth(live_goal_count_hint, goal_fingerprint, goal_truth_from_cs);
    apply_proof_goal_truth(p, truth);
    p.schema = kTypeLinearCommitProofIssue;
    // Last stamped linear_root for query / Agent drift detect (AC3).
    g_last_proof_linear_root_count.store(p.linear_root_count, std::memory_order_relaxed);
    if (p.linear_root_count > 0 || p.live_goal_count > 0) {
        g_type_linear_commit_proof_counts_filled_total.fetch_add(1, std::memory_order_relaxed);
    }
    // Bump the stamped total (additive — surface for Agent
    // dashboards to attribute "active stamp fired" vs "face fired
    // but Soft path observed only").
    g_type_linear_commit_proof_stamped_total.fetch_add(1, std::memory_order_relaxed);
    // Also stamp the epoch (existing low-level helper) so the
    // existing query:last-type-linear-commit-proof path stays
    // additive — query path returns the latest stamp epoch.
    stamp_type_linear_commit_proof(current_epoch_or_defuse);
    // Issue #2899: publish face bits for IR Move/Drop fast-path.
    publish_last_proof_face(p.would_allow_commit, p.linear_ok);
    return p;
}

// Issue #2854: stamp proof with explicit would_allow_commit + linear_ok
// (set by caller from the rebind + scan outcome). Ensures no success
// proof outlives a failed rebind on the same exit (#2854 AC2). The
// caller MUST bump type_linear_proof_stamped_after_rebind_total (success)
// or type_linear_proof_reject_after_rebind_fail_total (fail) separately
// so dashboards can distinguish ordered stamps from pre-#2854 stamps.
// Mirrors the existing live-stamp path (linear_root_count from post-remap
// collect via linear_or_dirty_roots_count_for_rebind; epoch + last-count
// gauges populated for Agent drift detect). Issue #2842: same goal truth
// freeze as the live path.
inline TypeLinearCommitProof build_type_linear_commit_proof_from_live_with_outcome(
    std::uint64_t current_epoch_or_defuse, bool explicit_would_allow_commit,
    bool explicit_linear_ok, std::uint64_t live_goal_count_hint = kProofLiveGoalCountHintAuto,
    std::uint64_t goal_fingerprint = 0, bool goal_truth_from_cs = false,
    std::uint32_t explicit_force_reason_code = static_cast<std::uint32_t>(-1)) noexcept {
    TypeLinearCommitProof p{};
    p.readiness_bp = 0;
    p.force_reason_code = explicit_force_reason_code;
    // Issue #2854: explicit outcome overrides the live-state defaults.
    // linear_root_count still comes from the post-remap collect so AC1
    // (success proof linear_root_count matches post-remap collect) holds.
    p.would_allow_commit = explicit_would_allow_commit;
    p.linear_ok = explicit_linear_ok;
    p.occurrence_consistent = explicit_linear_ok;
    p.defuse_or_epoch_stamp = current_epoch_or_defuse;
    p.linear_root_count =
        static_cast<std::uint64_t>(aura::compiler::linear_or_dirty_roots_count_for_rebind());
    const auto truth =
        resolve_proof_goal_truth(live_goal_count_hint, goal_fingerprint, goal_truth_from_cs);
    apply_proof_goal_truth(p, truth);
    // Issue #2981: same-txn safety net — never leave a green proof when
    // #2704 hard face is latched and CS goals are empty (prefer CS
    // truth over gauge). Soft never enters the helper.
    if (p.would_allow_commit && occurrence_empty_after_fence_blocks_proof(p.live_goal_count)) {
        p.would_allow_commit = false;
        p.linear_ok = false;
        p.occurrence_consistent = false;
        p.force_reason_code = 11; // occurrence_empty_after_fence
        g_type_linear_proof_reject_empty_after_fence_total.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #2984: compact mismatch latches reject before Success trail.
    if (p.would_allow_commit && linear_compact_root_mismatch_blocks_proof()) {
        p.would_allow_commit = false;
        p.linear_ok = false;
        p.occurrence_consistent = false;
        p.force_reason_code = 3; // linear
    }
    p.schema = kTypeLinearCommitProofIssue;
    // Last stamped linear_root for query / Agent drift detect (same as live path).
    g_last_proof_linear_root_count.store(p.linear_root_count, std::memory_order_relaxed);
    if (p.linear_root_count > 0 || p.live_goal_count > 0) {
        g_type_linear_commit_proof_counts_filled_total.fetch_add(1, std::memory_order_relaxed);
    }
    g_type_linear_commit_proof_stamped_total.fetch_add(1, std::memory_order_relaxed);
    stamp_type_linear_commit_proof(current_epoch_or_defuse);
    // Issue #2899: publish face bits for IR Move/Drop fast-path.
    publish_last_proof_face(p.would_allow_commit, p.linear_ok);
    return p;
}

// Issue #2984: post-arena-compact linear_root_count vs last proof.
// last==0 → return without collect (AC3). Mismatch: Soft observe;
// production/Full latch face + stamp reject (force_reason linear=3).
// Aligns with #2673 densify scan family (same linear-root consistency).
inline bool note_arena_compact_linear_root_consistency() noexcept {
    const auto last = last_proof_linear_root_count_v_read();
    if (last == 0)
        return false; // AC3: no extra collect
    g_linear_compact_root_check_total.fetch_add(1, std::memory_order_relaxed);
    const auto n =
        static_cast<std::uint64_t>(aura::compiler::linear_or_dirty_roots_count_for_rebind());
    if (n == last)
        return false;
    const bool hard = production_defaults_active() || get_strategy() == AuditStrategy::Full;
    if (!hard) {
        g_linear_compact_root_mismatch_observe_total.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_linear_compact_root_mismatch_total.fetch_add(1, std::memory_order_relaxed);
    g_linear_compact_root_mismatch_face.store(1, std::memory_order_relaxed);
    const auto epoch = last_type_linear_commit_proof_stamp_v_read();
    (void)build_type_linear_commit_proof_from_live_with_outcome(
        epoch == 0 ? 1 : epoch, /*would_allow=*/false, /*linear_ok=*/false,
        kProofLiveGoalCountHintAuto, 0, false, /*force_reason=*/3);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeReject);
    return true;
}

[[nodiscard]] inline std::int64_t commit_readiness_reason_code(std::string_view r) noexcept {
    if (r == "cone_truncate")
        return 9; // #2621
    if (r == "cone_outside_goal_drop")
        return 10; // #2703
    if (r == "occurrence_empty_after_fence")
        return 11; // #2704
    if (r == "auto_partial")
        return 6; // #2610
    if (r == "log_forces_partial")
        return 12; // #2851
    if (r == "empty_cs")
        return 5;
    if (r == "truncate")
        return 4;
    if (r == "linear")
        return 3;
    if (r == "blame")
        return 2;
    if (r == "solve")
        return 1;
    if (r == "cone_outside_goal_drop")
        return 10; // #2703 / #2716
    if (r == "occurrence_empty_after_fence")
        return 11; // #2704 / #2716
    if (r == "region_type_cross_talk")
        return 13; // #2847
    if (r == "required_type")
        return 14; // #2898
    if (r == "refined_drift")
        return 15; // #2911
    if (r == "pending_full_solve_residual")
        return 16; // #3031
    return 0;      // ok
}

// Issue #2716 / #2750: occurrence hard-face recover state (must be declared
// before commit_readiness uses them — inline header ODR-safe).
inline std::atomic<std::uint64_t> g_occurrence_hard_face_full_solve_recover_total{0};
// Issue #2750: true recover success/fail (distinct from #2716 reject-arm bump).
inline std::atomic<std::uint64_t> g_occurrence_hard_face_recover_success_total{0};
inline std::atomic<std::uint64_t> g_occurrence_hard_face_recover_fail_total{0};
// Issue #2909: force-closure counters (must be before commit_readiness).
// Full definitions / accessors also live near #2703 face section.
inline std::atomic<std::uint64_t> g_cone_truncate_force_closure_attempt_total{0};
inline std::atomic<std::uint64_t> g_cone_truncate_force_closure_total{0};
inline std::atomic<std::uint64_t> g_cone_truncate_force_closure_reject_total{0};
inline std::atomic<std::uint32_t> g_cone_truncate_force_closure_wired{1};
// Issue #2962: Agent-facing residual of #2909 — recover must reach SOLVED
// (goals consistent); hard-reject force_reason cone_outside_goal_drop when
// recover fails or returns "success" without SOLVED (half-green close).
// Soft: observe only (no hard path). Quiet: no extra atomics beyond face loads.
inline constexpr int kConeOutsideGoalDropRecoverRejectIssue = 2962;
inline std::atomic<std::uint64_t> g_cone_outside_goal_drop_recover_ok_total{0};
inline std::atomic<std::uint64_t> g_cone_outside_goal_drop_reject_total{0};
inline std::atomic<std::uint32_t> g_cone_outside_goal_drop_recover_reject_wired{1};
// Issue #2911: unified refined-consistency face (must be before commit_readiness).
// Soft vs production decision table (#2911 AC6 — code comments only):
//   Soft + drift        → observe counter; allow
//   production/Full + drift → hard reject or full-solve recover
//   no refined activity → zero cost (face clear; no extra loads beyond faces)
inline constexpr int kRefinedConsistencyGateIssue = 2911;
inline std::atomic<std::uint8_t> g_refined_consistency_drift_face{0};
// Issue #3031: pending_full_solve / locality residual at composite commit.
// Quiet (count=0): no extra atomics beyond the two loads in drain.
// Soft: observe only. Production/Full: escalate then latch face on reject.
inline constexpr int kPendingFullSolveResidualIssue = 3031;
inline std::atomic<std::uint8_t> g_pending_full_solve_residual_face{0};
inline std::atomic<std::uint64_t> g_pending_full_solve_residual_last{0};
inline std::atomic<std::uint64_t> g_pending_full_solve_residual_observe_total{0};
inline std::atomic<std::uint64_t> g_pending_full_solve_residual_escalate_total{0};
inline std::atomic<std::uint64_t> g_pending_full_solve_residual_reject_total{0};
inline std::atomic<std::uint32_t> g_pending_full_solve_residual_wired{1};

[[nodiscard]] inline bool pending_full_solve_residual_face_hit() noexcept {
    return g_pending_full_solve_residual_face.load(std::memory_order_relaxed) != 0;
}
[[nodiscard]] inline std::uint64_t pending_full_solve_residual_last_v_read() noexcept {
    return g_pending_full_solve_residual_last.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t pending_full_solve_residual_observe_total_v_read() noexcept {
    return g_pending_full_solve_residual_observe_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t pending_full_solve_residual_escalate_total_v_read() noexcept {
    return g_pending_full_solve_residual_escalate_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t pending_full_solve_residual_reject_total_v_read() noexcept {
    return g_pending_full_solve_residual_reject_total.load(std::memory_order_relaxed);
}
inline void reset_pending_full_solve_residual_for_test() noexcept {
    g_pending_full_solve_residual_face.store(0, std::memory_order_relaxed);
    g_pending_full_solve_residual_last.store(0, std::memory_order_relaxed);
    g_pending_full_solve_residual_observe_total.store(0, std::memory_order_relaxed);
    g_pending_full_solve_residual_escalate_total.store(0, std::memory_order_relaxed);
    g_pending_full_solve_residual_reject_total.store(0, std::memory_order_relaxed);
}
inline void note_pending_full_solve_residual(std::uint64_t n, bool hard) noexcept {
    g_pending_full_solve_residual_last.store(n, std::memory_order_relaxed);
    if (n == 0) {
        g_pending_full_solve_residual_face.store(0, std::memory_order_relaxed);
        return;
    }
    if (hard)
        g_pending_full_solve_residual_face.store(1, std::memory_order_relaxed);
}
inline std::atomic<std::uint64_t> g_refined_consistency_observe_total{0};
inline std::atomic<std::uint64_t> g_refined_consistency_reject_total{0};
inline std::atomic<std::uint64_t> g_refined_consistency_recover_total{0};
inline std::atomic<std::uint32_t> g_refined_consistency_wired{1};
// Optional full-solve recover hook (wired by TypeChecker / Evaluator).
// Returns true when SOLVED + occurrence roots restored. nullptr = no recover.
using OccurrenceFullSolveRecoverFn = bool (*)(void* ctx) noexcept;
inline OccurrenceFullSolveRecoverFn g_occurrence_full_solve_recover_fn = nullptr;
inline void* g_occurrence_full_solve_recover_ctx = nullptr;
inline void install_occurrence_full_solve_recover(OccurrenceFullSolveRecoverFn fn,
                                                  void* ctx) noexcept {
    g_occurrence_full_solve_recover_fn = fn;
    g_occurrence_full_solve_recover_ctx = ctx;
}
// Forward decls — defined later with face counter clear helpers (#2703/#2704/#2847).
inline void clear_cone_outside_goal_drop_for_test() noexcept;
inline void clear_partial_cone_truncate_for_test() noexcept;
inline void clear_occurrence_empty_after_fence_for_test() noexcept;
[[nodiscard]] inline bool region_type_cross_talk_face_hit() noexcept;

// Pure decision table (AC5: identical inputs → identical output; no atomics).
[[nodiscard]] inline CommitReadiness commit_readiness(const CommitReadinessInput& in) noexcept {
    CommitReadiness r;
    auto set = [&](std::string_view reason, bool allow, std::uint32_t bp) {
        r.force_reason = reason;
        r.force_reason_code = commit_readiness_reason_code(reason);
        r.would_allow_commit = allow;
        r.readiness_bp = bp;
    };

    // 1) empty_cs — expected_partial (or #2610 auto) + empty CS (#2345 / #2509).
    // Auto path uses force_reason "auto_partial" when soft so Agents can
    // distinguish under-marked cone from explicit expected_partial.
    const bool expected_eff = in.expected_partial || in.auto_partial_from_cone;
    if (expected_eff && !in.cs_has_work) {
        if (in.empty_cs_hard)
            return (
                set(in.auto_partial_from_cone && !in.expected_partial ? "auto_partial" : "empty_cs",
                    false, 0),
                r);
        // Soft observe: auto path uses distinct reason; explicit keeps empty_cs.
        if (in.auto_partial_from_cone && !in.expected_partial)
            return (set("auto_partial", true, 7400), r);
        return (set("empty_cs", true, 7500), r); // Soft observe
    }

    // 2) truncate — truncated reverify without full-solve recover (#2458)
    //    OR partial cone truncate (#2621 / #2560 soft|hard overflow).
    //    cone_truncate reason when only cone truncated (not reverify).
    //
    // Issue #2909 / #2962: production/Full + cone truncate + outside-If
    // goal drop must force one full-solve recover (#2750 hook) before hard
    // reject. Recover success allows commit only when SOLVED
    // (solve_status==0) — #2962 residual half-green close (recover that
    // reports true but leaves CONFLICT/TIMEOUT is force-rejected with
    // force_reason cone_outside_goal_drop, Agent-visible code 10).
    // Soft: observe only. Quiet (no outside drop): keep prior cone_truncate
    // hard reject without extra recover cost when truncate_hard.
    //
    // Soft vs production decision table (#2962 AC2 / #2909 AC6):
    //   Soft + truncate + outside drop  → Soft observe (cone_truncate allow)
    //   production/Full + truncate + outside + recover SOLVED → allow (ok path)
    //   production/Full + truncate + outside + recover fail/non-SOLVED
    //       → hard-reject force_reason cone_outside_goal_drop
    //   no truncate / no outside drop → zero extra recover (quiet)
    const bool trunc_face =
        (in.truncated_reverify && !in.truncated_full_solve_recovered) || in.partial_cone_truncated;
    if (trunc_face) {
        const bool cone_only = in.partial_cone_truncated &&
                               !(in.truncated_reverify && !in.truncated_full_solve_recovered);
        const std::string_view reason = cone_only ? "cone_truncate" : "truncate";
        const bool hard = in.truncate_hard || in.occurrence_face_hard;
        const bool outside_drop =
            in.cone_outside_goal_drop_face || (hard && cone_outside_goal_drop_total_v_read() > 0);
        if (hard && outside_drop) {
            // Force-closure recover path (#2909) + SOLVED gate (#2962).
            g_cone_truncate_force_closure_attempt_total.fetch_add(1, std::memory_order_relaxed);
            bool recovered = false;
            if (g_occurrence_full_solve_recover_fn != nullptr)
                recovered = g_occurrence_full_solve_recover_fn(g_occurrence_full_solve_recover_ctx);
            // Issue #2962: recover must leave SOLVED (solve_status==0). A
            // hook that returns true under CONFLICT/TIMEOUT is half-green.
            if (recovered && in.solve_status != 0)
                recovered = false;
            if (recovered) {
                g_cone_truncate_force_closure_total.fetch_add(1, std::memory_order_relaxed);
                g_occurrence_hard_face_recover_success_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                g_cone_outside_goal_drop_recover_ok_total.fetch_add(1, std::memory_order_relaxed);
                // Consume truncate + outside-drop so re-entry is clean.
                clear_partial_cone_truncate_for_test();
                clear_cone_outside_goal_drop_for_test();
                // Fall through to later faces / ok (recovered SOLVED).
            } else {
                g_cone_truncate_force_closure_reject_total.fetch_add(1, std::memory_order_relaxed);
                g_occurrence_hard_face_recover_fail_total.fetch_add(1, std::memory_order_relaxed);
                g_cone_outside_goal_drop_reject_total.fetch_add(1, std::memory_order_relaxed);
                return (set("cone_outside_goal_drop", false, 800), r);
            }
        } else if (in.truncate_hard) {
            return (set(reason, false, 1000), r);
        } else {
            return (set(reason, true, 7000), r); // Soft observe
        }
    }

    // 3) linear — escape / invariant fail (#2108).
    if (!in.linear_ok) {
        if (in.linear_hard)
            return (set("linear", false, 500), r);
        return (set("linear", true, 5500), r); // Soft observe
    }

    // 4) blame — incomplete blame chain (#2221).
    if (!in.blame_ok) {
        if (in.blame_hard)
            return (set("blame", false, 1500), r);
        return (set("blame", true, 5000), r); // Soft observe
    }

    // 5) solve — CONFLICT / TIMEOUT (not SOLVED).
    if (in.solve_status != 0) {
        const auto bp = in.solve_status == 2 ? 2000u : 2500u;
        return (set("solve", false, bp), r);
    }

    // 6) Issue #2716 / #2750: occurrence hard-faces. When production/Full
    // + face counters advanced, try one full ConstraintSystem::solve()
    // recover (hook) before hard-reject. Soft / baseline=0: counter-only
    // (no full solve — preserves #2703/#2704 Soft ergonomics).
    // Issue #2909: re-check live face atomics — step 2 force-closure may
    // already have consumed truncate+outside-drop faces (avoid double solve).
    if (in.occurrence_face_hard) {
        const bool cone_face =
            in.cone_outside_goal_drop_face && cone_outside_goal_drop_total_v_read() > 0;
        const bool empty_face =
            in.occurrence_empty_after_fence_face && occurrence_empty_after_fence_total_v_read() > 0;
        if (cone_face || empty_face) {
            // Issue #2750: Option A recover half — one full solve via hook.
            // Quiet path (no face) never reaches here → zero extra solve cost.
            bool recovered = false;
            if (g_occurrence_full_solve_recover_fn != nullptr)
                recovered = g_occurrence_full_solve_recover_fn(g_occurrence_full_solve_recover_ctx);
            // Issue #2962: SOLVED-only (solve_status==0) after recover.
            if (recovered && in.solve_status != 0)
                recovered = false;
            if (recovered) {
                g_occurrence_hard_face_recover_success_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                if (cone_face)
                    g_cone_outside_goal_drop_recover_ok_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                // Consume faces so re-entry does not immediately re-reject.
                clear_cone_outside_goal_drop_for_test();
                clear_occurrence_empty_after_fence_for_test();
                // Fall through to step 7 region face / ok (recovered).
            } else {
                g_occurrence_hard_face_recover_fail_total.fetch_add(1, std::memory_order_relaxed);
                if (cone_face) {
                    // Issue #2962: Agent-facing reject total (outside-drop face).
                    g_cone_outside_goal_drop_reject_total.fetch_add(1, std::memory_order_relaxed);
                    return (set("cone_outside_goal_drop", false, 800), r);
                }
                return (set("occurrence_empty_after_fence", false, 850), r);
            }
        }
    }

    // 6b) Issue #2847: region type/occurrence cross-talk under concurrent
    // admit. production/Full + face latch → hard reject. Soft leaves
    // face unset (observe-only via note_region_type_cross_talk(false)).
    if (in.occurrence_face_hard && in.region_type_cross_talk_face) {
        return (set("region_type_cross_talk", false, 900), r);
    }

    // 6c) Issue #2911: unified refined-consistency hard gate.
    // Production/Full + refined drift (explicit latch or multi-face
    // refined signals) → one full-solve recover (#2750 hook) or hard
    // reject with force_reason refined_drift (code 15). Soft: observe
    // only (would_allow_commit stays true when refined_consistency_hard
    // is false). Quiet: face clear → zero cost (no recover attempt).
    if (in.refined_consistency_hard && in.refined_consistency_drift) {
        if (g_occurrence_full_solve_recover_fn != nullptr &&
            g_occurrence_full_solve_recover_fn(g_occurrence_full_solve_recover_ctx)) {
            g_refined_consistency_recover_total.fetch_add(1, std::memory_order_relaxed);
            g_occurrence_hard_face_recover_success_total.fetch_add(1, std::memory_order_relaxed);
            g_refined_consistency_drift_face.store(0, std::memory_order_relaxed);
            // Fall through to ok (recovered refined scheme).
        } else {
            g_refined_consistency_reject_total.fetch_add(1, std::memory_order_relaxed);
            g_occurrence_hard_face_recover_fail_total.fetch_add(1, std::memory_order_relaxed);
            return (set("refined_drift", false, 750), r);
        }
    } else if (!in.refined_consistency_hard && in.refined_consistency_drift) {
        // Soft observe path (hermetic tests may set drift without hard).
        g_refined_consistency_observe_total.fetch_add(1, std::memory_order_relaxed);
        // Allow commit under Soft.
    }

    // 6d) Issue #3031: pending_full_solve / locality residual.
    // Production/Full + residual face → hard-reject (escalate already
    // attempted at composite drain). Soft: observe allow.
    // Quiet: residual flag false → zero extra.
    if (in.pending_full_solve_residual) {
        if (in.pending_full_solve_hard)
            return (set("pending_full_solve_residual", false, 700), r);
        return (set("pending_full_solve_residual", true, 7200), r);
    }

    // 7) ok — clean SOLVED + linear + blame + !truncated + no face hit.
    return (set("ok", true, 10000), r);
}

// Fill hard flags from live audit process state (still pure w.r.t. inputs
// once copied; callers that want hermetic tests pass CommitReadinessInput
// directly without this helper).
// Issue #2716 / #2750 recover counters + hook live above commit_readiness.
[[nodiscard]] inline CommitReadinessInput commit_readiness_live_policy() noexcept {
    CommitReadinessInput in;
    const bool prod = production_defaults_active();
    const bool full = get_strategy() == AuditStrategy::Full;
    in.empty_cs_hard = composite_empty_cs_hard_reject_enabled();
    // Issue #2621: cone truncate uses same hard family as #2458 truncate +
    // AURA_PARTIAL_CONE_COMMIT_HARD.
    in.truncate_hard = truncate_commit_hard_enabled() || partial_cone_commit_hard_enabled();
    // Linear escape + blame-complete hard under production / Full (lineage).
    in.linear_hard = prod || full;
    in.blame_hard = prod || full;
    // Live last partial cone truncate stamp (#2621 / #2560).
    in.partial_cone_truncated = last_partial_cone_truncated();
    // Issue #2716: occurrence hard-faces (active wiring). Under
    // production / Full, capture the face counter values. The
    // active branch in commit_readiness rejects when the face has
    // fired (counter > 0 — i.e., face has been bumped since the
    // last clear). Soft path leaves occurrence_face_hard=false so
    // the counter-only path stays metric (no reject, no full-solve
    // — preserves the existing Soft ergonomics from #2703 / #2704).
    // Per AC3: quiet path costs 2 atomics on prod/Full when
    // neither face has fired. No extra atomics when not in
    // prod/Full.
    const bool face_hard = prod || full;
    in.occurrence_face_hard = face_hard;
    // Issue #2911: refined-consistency hard under same production/Full face.
    in.refined_consistency_hard = face_hard;
    // Issue #3031: pending_full_solve residual face.
    in.pending_full_solve_hard = face_hard;
    in.pending_full_solve_residual = pending_full_solve_residual_face_hit();
    if (face_hard) {
        in.cone_outside_goal_drop_face = (cone_outside_goal_drop_total_v_read() > 0);
        in.occurrence_empty_after_fence_face = (occurrence_empty_after_fence_total_v_read() > 0);
        // Issue #2847: region type cross-talk face latch.
        in.region_type_cross_talk_face = region_type_cross_talk_face_hit();
        // Issue #2911: unified refined drift — explicit latch OR multi-face
        // refined signals (e.g. occurrence empty after fence + outside drop).
        // Quiet: all clear → refined_consistency_drift stays false (zero cost
        // beyond the face loads already paid above).
        int refined_hits = 0;
        if (g_refined_consistency_drift_face.load(std::memory_order_relaxed) != 0)
            refined_hits = 2; // explicit latch always enough
        else {
            if (in.cone_outside_goal_drop_face)
                ++refined_hits;
            if (in.occurrence_empty_after_fence_face)
                ++refined_hits;
        }
        in.refined_consistency_drift = refined_hits >= 2;
        // Issue #2716: face-hit observe counter (not true recover).
        // #2750 moves true recover success to recover_success_total.
        if (in.cone_outside_goal_drop_face || in.occurrence_empty_after_fence_face) {
            g_occurrence_hard_face_full_solve_recover_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return in;
}

[[nodiscard]] inline std::uint64_t occurrence_hard_face_recover_success_total_v_read() noexcept {
    return g_occurrence_hard_face_recover_success_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t occurrence_hard_face_recover_fail_total_v_read() noexcept {
    return g_occurrence_hard_face_recover_fail_total.load(std::memory_order_relaxed);
}

// Issue #2145: Agent-stable deny reason (mirror #2076 format_deny_reason shape).
// Shape: "invariant-denied: <kind> tenant=<id> op=<op>"
[[nodiscard]] inline std::string
format_invariant_deny_reason(std::string_view kind, std::uint64_t tenant_id, std::string_view op) {
    return std::format("invariant-denied: {} tenant={} op={}", kind, tenant_id, op);
}

[[nodiscard]] inline MutationKind classify_kind(std::string_view op) noexcept {
    if (op.empty())
        return MutationKind::Unknown;
    if (op.find("aot-hotupdate") != std::string_view::npos ||
        op.find("aot_hotupdate") != std::string_view::npos)
        return MutationKind::AotHotUpdate;
    if (op.find("jit-") != std::string_view::npos || op.find("jit_") != std::string_view::npos)
        return MutationKind::JitHotpath;
    if (op.find("hygiene") != std::string_view::npos || op.find("macro") != std::string_view::npos)
        return MutationKind::MacroHygiene;
    if (op.find("replace-type") != std::string_view::npos || op == "replace-type")
        return MutationKind::ReplaceType;
    if (op.find("replace-value") != std::string_view::npos || op == "replace-value")
        return MutationKind::ReplaceValue;
    if (op.find("record-patch") != std::string_view::npos || op == "record-patch")
        return MutationKind::RecordPatch;
    if (op == "structural" || op.find("mutate") != std::string_view::npos)
        return MutationKind::Structural;
    return MutationKind::Other;
}

[[nodiscard]] inline std::uint64_t next_audit_mutation_id() noexcept {
    return g_typed_mutation_audit_counters.audit_mutation_id_gen.fetch_add(
               1, std::memory_order_relaxed) +
           1;
}

// Issue #2493: canonical mid resolution for audit paths that did not
// thread a caller mid. Preference order (mirrors #2384 require_effect
// stamping so SE ↔ TypedMutationAudit ↔ grant epoch stay joined):
//   1. caller mid when non-zero
//   2. current_mutation_epoch() when non-zero  (WorkspaceEpoch Mutation — #2149)
//   3. ResourceQuota host mid when set
//   4. Soft/Sampled only: next_audit_mutation_id() last-resort join stamp
//      (process-origin); bumps audit_mid_fallback_gen_total.
// Issue #2836: under production_defaults_active() || Full, step 4 is
// absolute zero-tolerance — refuse process-origin stamp (return 0) and
// bump audit_mid_fallback_refused_total. Supersedes #2635 rate-based
// resolve-time hard-deny (SLO gate remains on schedule admission #2630).
// Soft / Sampled keep Soft fallback (#2493 AC4, #2635 AC3, #2836 AC2).
// #2636 note: AuditStrategy has {Off, Sampled, Full} only — no Strict enum.
[[nodiscard]] inline std::uint64_t
resolve_audit_mutation_id(std::uint64_t caller_mid = 0) noexcept {
    if (caller_mid != 0)
        return caller_mid;
    const auto ep = ::aura::core::current_mutation_epoch();
    if (ep != 0)
        return ep;
    using ::aura::core::resource_quota::process_resource_quota_manager;
    const auto rq = process_resource_quota_manager().provenance_mutation_id;
    if (rq != 0)
        return rq;
    // Issue #2836 / #2635 lineage: production mid-fallback absolute
    // zero-tolerance. hard_deny_eligible = production_defaults || Full
    // (same gate shape as #2635; behavior is now absolute refuse, not
    // rate-based would_arm_degraded). Soft/Sampled fall through.
    const bool hard_deny_eligible =
        production_defaults_active() || get_strategy() == AuditStrategy::Full;
    if (hard_deny_eligible) {
        // Absolute refuse: no process-origin join stamp into the trail.
        // Callers treat mid==0 as deny / re-stamp or surface
        // "mid-fallback-refused" (#2836 AC4). Distinct refuse metric —
        // does NOT bump audit_mid_fallback_gen_total (#2836 AC1).
        g_typed_mutation_audit_counters.audit_mid_fallback_refused_total.fetch_add(
            1, std::memory_order_relaxed);
        // Issue #3054: exactly one joinable SE (ring + WAL when enabled).
        // Soft never reaches this branch. TLS suppresses nested re-resolve.
        if (!g_tls_mid_fallback_refuse_se_emitted) {
            g_tls_mid_fallback_refuse_se_emitted = true;
            using ::aura::core::security_event::g_security_event_ring;
            using ::aura::core::security_event::SecurityEventKind;
            using ::aura::core::security_event_wal::emit_security_event_durable;
            emit_security_event_durable(SecurityEventKind::InvariantFail, /*tenant=*/0,
                                        /*mid=*/0, /*epoch=*/ep, /*effect_bits=*/0,
                                        "resolve-audit-mid", "mid-fallback-refused",
                                        /*denied=*/true, /*fiber=*/0);
            const auto seq = g_security_event_ring().seq.load(std::memory_order_relaxed);
            g_typed_mutation_audit_counters.audit_mid_fallback_refuse_se_seq.store(
                seq == 0 ? 0 : seq - 1, std::memory_order_relaxed);
            g_typed_mutation_audit_counters.audit_mid_fallback_refuse_se_total.fetch_add(
                1, std::memory_order_relaxed);
        }
        return 0;
    }
    // Soft / Sampled: last-resort process-origin stamp + gen counter.
    g_typed_mutation_audit_counters.audit_mid_fallback_gen_total.fetch_add(
        1, std::memory_order_relaxed);
    return next_audit_mutation_id();
}

// Issue #2814 M7: TLS link between trail Success and invariant enforcement.
// record_invariant_audit_result → note_ran; Guard intentional skip → note_skipped.
// capture_audit_event_forced(Success, mutate-class kind) without either → gap.
enum class EnforcementLinkKind : std::uint8_t { None = 0, Ran = 1, Skipped = 2 };
inline thread_local std::uint64_t g_tls_enforcement_link_mid = 0;
inline thread_local EnforcementLinkKind g_tls_enforcement_link = EnforcementLinkKind::None;

// Issue #3016: mid resolved at outermost Guard enter. Trail / SE / grant
// / occurrence / proof read this — never Evaluator::total_mutations_
// (volume metric only). noted=true even when mid==0 (production refuse)
// so stamp sites do not re-resolve and double-count refused_total.
inline constexpr int kBoundaryAuditMidIssue = 3016;
inline thread_local std::uint64_t g_tls_boundary_audit_mid = 0;
inline thread_local bool g_tls_boundary_audit_noted = false;
inline std::atomic<std::uint64_t> g_last_stamped_audit_mid{0};

inline void note_boundary_audit_mid(std::uint64_t mid) noexcept {
    g_tls_boundary_audit_mid = mid;
    g_tls_boundary_audit_noted = true;
}

inline void clear_boundary_audit_mid() noexcept {
    g_tls_boundary_audit_mid = 0;
    g_tls_boundary_audit_noted = false;
    clear_mid_fallback_refuse_se_tls();
}

[[nodiscard]] inline std::uint64_t current_boundary_audit_mid() noexcept {
    return g_tls_boundary_audit_mid;
}

// Prefer enter-resolved TLS mid. 0 under production refuse is sticky.
// If no boundary noted yet, resolve (Soft fallback / epoch).
[[nodiscard]] inline std::uint64_t stamp_boundary_audit_mid() noexcept {
    if (g_tls_boundary_audit_noted)
        return g_tls_boundary_audit_mid;
    return resolve_audit_mutation_id();
}

// Issue #2814: mark that post_mutation_invariant suite (or equivalent)
// ran for this mutation_id. Call before/during record_invariant_audit_result.
inline void note_invariant_enforcement_ran(std::uint64_t mutation_id) noexcept {
    if (mutation_id == 0)
        return;
    g_tls_enforcement_link_mid = mutation_id;
    g_tls_enforcement_link = EnforcementLinkKind::Ran;
    g_typed_mutation_audit_counters.audit_enforcement_ran_total.fetch_add(
        1, std::memory_order_relaxed);
}

// Issue #2814: intentional non-enforcement (Sampled skip, RenderFastExit,
// strategy Off quiet path). Prevents false gap on legitimate soft paths.
inline void note_invariant_enforcement_skipped(std::uint64_t mutation_id) noexcept {
    if (mutation_id == 0)
        return;
    g_tls_enforcement_link_mid = mutation_id;
    g_tls_enforcement_link = EnforcementLinkKind::Skipped;
    g_typed_mutation_audit_counters.audit_enforcement_skipped_intentional_total.fetch_add(
        1, std::memory_order_relaxed);
}

[[nodiscard]] inline bool enforcement_linked_for(std::uint64_t mutation_id) noexcept {
    return mutation_id != 0 && g_tls_enforcement_link_mid == mutation_id &&
           g_tls_enforcement_link != EnforcementLinkKind::None;
}

// Mutate-class kinds that should be enforcement-linked on Success trails.
// MacroHygiene / Aot / Jit / security-correlation paths are excluded.
[[nodiscard]] inline bool mutate_class_kind_requires_enforcement_link(MutationKind kind) noexcept {
    switch (kind) {
        case MutationKind::Structural:
        case MutationKind::ReplaceType:
        case MutationKind::ReplaceValue:
        case MutationKind::RecordPatch:
            return true;
        default:
            return false;
    }
}

// Core trail write (no Sampled gate). Used by capture_audit_event and by
// #2054 security-correlated emit (always-on so rings stay joined by
// mutation_id even under Sampled strategy).
//
// Issue #2814 M7: this function is pure observability (trail + counters).
// It does NOT run type/linear/provenance checks. Enforcement lives in
// run_typed_mutation_invariant_audit → record_invariant_audit_result.
// On Success for mutate-class kinds, if neither note_invariant_enforcement_ran
// nor note_invariant_enforcement_skipped was called for this mid, bump
// audit_enforcement_gap_total (silent enforcement degradation signal).
inline void capture_audit_event_forced(std::uint64_t mutation_id, std::string_view name,
                                       MutationKind kind, std::uint64_t before_epoch,
                                       std::uint64_t after_epoch, AuditOutcome outcome,
                                       std::uint32_t target_node = 0,
                                       std::uint32_t nodes_changed = 0, std::int64_t fiber_id = 0,
                                       std::uint32_t affected_ref_count = 0) noexcept {
    // Issue #3016 / #2836: never stamp mid=0 into the trail (production
    // refuse / missing resolve). Soft resolve already produced a gen.
    if (mutation_id == 0)
        return;
    TypedMutationAuditEvent ev{};
    ev.mutation_id = mutation_id;
    g_last_stamped_audit_mid.store(mutation_id, std::memory_order_relaxed);
    const auto seq =
        g_typed_mutation_audit_counters.trail_seq.fetch_add(1, std::memory_order_relaxed);
    ev.seq = seq;
    const auto n = name.size() < (kAuditNameCap - 1) ? name.size() : (kAuditNameCap - 1);
    if (n > 0)
        std::memcpy(ev.name, name.data(), n);
    ev.name[n] = '\0';
    ev.kind = kind;
    ev.before_epoch = before_epoch;
    ev.after_epoch = after_epoch;
    ev.outcome = outcome;
    ev.target_node = target_node;
    ev.nodes_changed = nodes_changed;
    ev.fiber_id = fiber_id;
    ev.timestamp_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
    ev.affected_ref_count = affected_ref_count;

    // Issue #2819: lock-free ring publish (no mutex on capture hot path).
    // trail_seq was already claimed via fetch_add; each seq maps to a unique
    // slot until wrap. Concurrent writers hit different slots until size wraps.
    // Readers (trail_at_seq) validate out.seq == expected to drop torn/stale.
    g_trail().ring[seq % kTypedMutationAuditTrailSize] = ev;
    g_typed_mutation_audit_counters.audit_trail_lockfree_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
    // mutex_wait_us stays 0: lock-free path never waits.

    g_typed_mutation_audit_counters.contextual_total.fetch_add(1, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.trail_writes.fetch_add(1, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.typed_mutation_audit_triggered_total.fetch_add(
        1, std::memory_order_relaxed);
    if (outcome == AuditOutcome::Rollback)
        g_typed_mutation_audit_counters.rollbacks.fetch_add(1, std::memory_order_relaxed);
    if (outcome == AuditOutcome::Error)
        g_typed_mutation_audit_counters.errors.fetch_add(1, std::memory_order_relaxed);

    // Issue #2814 M7: enforcement-link gap detection (metric always).
    // Stderr warn is opt-in only: bash regression / agent harnesses capture
    // combined streams and treat any gap banner as polluting stdout/expect
    // (agent:mutate-rebind, edsl-ir-cache:cascade-*). Set AURA_AUDIT_GAP_WARN=1
    // for local diagnosis. Metric audit_enforcement_gap_total remains the
    // Agent-facing signal (query schema-2814).
    if (outcome == AuditOutcome::Success && mutation_id != 0 &&
        mutate_class_kind_requires_enforcement_link(kind) && !enforcement_linked_for(mutation_id)) {
        g_typed_mutation_audit_counters.audit_enforcement_gap_total.fetch_add(
            1, std::memory_order_relaxed);
        static std::atomic<int> s_gap_warned{0};
        const char* warn = std::getenv("AURA_AUDIT_GAP_WARN");
        if (warn && warn[0] == '1' && warn[1] == '\0' &&
            s_gap_warned.exchange(1, std::memory_order_relaxed) == 0) {
            std::fprintf(stderr,
                         "[#2814 M7 audit] Success trail without invariant enforcement "
                         "link (mid=%llu name=%.*s). Call note_invariant_enforcement_ran "
                         "or note_invariant_enforcement_skipped before trail write. "
                         "Metric: audit_enforcement_gap_total.\n",
                         static_cast<unsigned long long>(mutation_id), static_cast<int>(n),
                         ev.name);
        }
    }
}

inline void capture_audit_event(std::uint64_t mutation_id, std::string_view name, MutationKind kind,
                                std::uint64_t before_epoch, std::uint64_t after_epoch,
                                AuditOutcome outcome, std::uint32_t target_node = 0,
                                std::uint32_t nodes_changed = 0, std::int64_t fiber_id = 0,
                                std::uint32_t affected_ref_count = 0) noexcept {
    if (!should_audit(mutation_id))
        return;
    capture_audit_event_forced(mutation_id, name, kind, before_epoch, after_epoch, outcome,
                               target_node, nodes_changed, fiber_id, affected_ref_count);
}

// Issue #2054: always-on security correlation emit from
// check_and_record_effect (allow + deny). Bypasses Sampled so Agents
// can join SecurityEvent.mutation_id ↔ TypedMutationAuditEvent.mutation_id.
// Issue #2493: caller_mid == 0 falls into the resolve_audit_mutation_id
// preference order (caller_mid → current_mutation_epoch → ResourceQuota →
// last-resort audit gen + fallback counter bump). Epoch field also falls
// back to current_mutation_epoch() when caller passes 0 so SE.epoch stays
// Mutation vocabulary (#2149).
inline void capture_security_correlated_audit(std::uint64_t mutation_id, std::string_view op,
                                              std::uint64_t epoch, bool denied,
                                              std::uint32_t target_node = 0,
                                              std::int64_t fiber_id = 0) noexcept {
    g_typed_mutation_audit_counters.audits_considered.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t mid = resolve_audit_mutation_id(mutation_id);
    const auto use_epoch = epoch != 0 ? epoch : ::aura::core::current_mutation_epoch();
    capture_audit_event_forced(mid, op, classify_kind(op), use_epoch, use_epoch,
                               denied ? AuditOutcome::Error : AuditOutcome::Success, target_node,
                               /*nodes_changed=*/0, fiber_id, /*affected_ref_count=*/0);
}

// Issue #1882: AOT hot-update boundary audit. Sampled on success (should_audit);
// failures always enter the trail (AI self-evolution must not drop reject/rollback).
inline void capture_aot_hotupdate_audit(bool success, std::uint64_t before_epoch,
                                        std::uint64_t after_epoch,
                                        std::string_view reason = "aot-hotupdate") noexcept {
    g_typed_mutation_audit_counters.aot_hotupdate_attempts.fetch_add(1, std::memory_order_relaxed);
    // Issue #2493: prefer Mutation epoch / ResourceQuota host mid over the
    // last-resort audit gen so AOT trail joins the same mid vocabulary as
    // require_effect / grant / isolation SE.
    const std::uint64_t mid = resolve_audit_mutation_id();
    if (success) {
        if (!should_audit(mid))
            return;
        g_typed_mutation_audit_counters.aot_hotupdate_audits.fetch_add(1,
                                                                       std::memory_order_relaxed);
        g_typed_mutation_audit_counters.aot_hotupdate_ok.fetch_add(1, std::memory_order_relaxed);
        capture_audit_event(mid, reason, MutationKind::AotHotUpdate, before_epoch, after_epoch,
                            AuditOutcome::Success);
        return;
    }
    // Always-on failure path (mirrors capture_macro_hygiene_audit).
    g_typed_mutation_audit_counters.aot_hotupdate_audits.fetch_add(1, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.aot_hotupdate_fail.fetch_add(1, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.aot_hotupdate_invariant_fail_total.fetch_add(
        1, std::memory_order_relaxed);
    const auto prev = get_strategy();
    set_strategy(AuditStrategy::Full);
    capture_audit_event(mid, reason, MutationKind::AotHotUpdate, before_epoch, after_epoch,
                        AuditOutcome::Error);
    set_strategy(prev);
}

// Issue #1882: lightweight JIT L2 / apply hotpath sample (never forces Full).
inline void capture_jit_hotpath_audit(std::string_view tag) noexcept {
    // Issue #2493: same preference order as AOT (Mutation epoch preferred).
    const std::uint64_t mid = resolve_audit_mutation_id();
    if (!should_audit(mid))
        return;
    g_typed_mutation_audit_counters.jit_hotpath_audits.fetch_add(1, std::memory_order_relaxed);
    capture_audit_event(mid, tag, MutationKind::JitHotpath, /*before_epoch=*/0, /*after_epoch=*/0,
                        AuditOutcome::Success);
}

// Issue #1613: always-on macro hygiene audit (bypasses Sampled gate so
// blocked macro mutates are never lost from the trail).
// Issue #1877: on Error/Rollback also stamp provenance tracker with
// tenant_id so MacroIntroduced hygiene blocks are visible to both audit
// trail and StableNodeRef provenance / truncated blame chains.
inline void capture_macro_hygiene_audit(std::string_view name, AuditOutcome outcome,
                                        std::uint32_t target_node = 0, std::int64_t fiber_id = 0,
                                        std::uint64_t tenant_id = 0,
                                        std::uint64_t mutation_id = 0) noexcept {
    g_typed_mutation_audit_counters.macro_hygiene_events.fetch_add(1, std::memory_order_relaxed);
    if (outcome == AuditOutcome::Error || outcome == AuditOutcome::Rollback) {
        g_typed_mutation_audit_counters.macro_hygiene_blocked.fetch_add(1,
                                                                        std::memory_order_relaxed);
        // Dual-record: audit trail (below) + provenance tracker (#1877).
        aura::core::provenance::record_macro_hygiene_provenance(
            target_node, tenant_id, mutation_id, static_cast<std::uint32_t>(fiber_id));
    } else {
        g_typed_mutation_audit_counters.macro_hygiene_allowed.fetch_add(1,
                                                                        std::memory_order_relaxed);
    }
    const auto prev = get_strategy();
    set_strategy(AuditStrategy::Full);
    capture_audit_event(mutation_id, name, MutationKind::MacroHygiene,
                        /*before_epoch=*/0, /*after_epoch=*/0, outcome, target_node,
                        /*nodes_changed=*/0, fiber_id, /*affected_ref_count=*/0);
    set_strategy(prev);
}

// Convenience for mutation boundary integration.
// Issue #2814: Success trails for mutate-class kinds require an enforcement
// link (note_invariant_enforcement_ran or note_invariant_enforcement_skipped)
// before this write, or capture_audit_event_forced bumps gap total.
inline void record_boundary_outcome(std::uint64_t mutation_id, std::string_view op,
                                    std::uint64_t before_epoch, std::uint64_t after_epoch,
                                    bool success, std::uint32_t target_node = 0,
                                    std::uint32_t nodes_changed = 0,
                                    std::int64_t fiber_id = 0) noexcept {
    capture_audit_event(mutation_id, op, classify_kind(op), before_epoch, after_epoch,
                        success ? AuditOutcome::Success : AuditOutcome::Rollback, target_node,
                        nodes_changed, fiber_id, nodes_changed > 0 ? 1u : 0u);
}

// Issue #1614: record result of type + linear + provenance invariant suite.
// Issue #2027: composite_mode / cross_batch_linear_escape feed partial recovery.
// Issue #2223: adt_ok = match exhaustiveness in dirty / workspace match sites.
struct InvariantAuditResult {
    bool type_ok = true;
    bool linear_ok = true;
    bool provenance_ok = true;
    bool adt_ok = true; // Issue #2223: non-exhaustive match fails under Full
    bool composite_mode = false;
    bool cross_batch_linear_escape = false;
    // Issue #2563: free-capture of dirty linear into Lambda (one-level).
    bool cross_closure_linear_escape = false;
    // Issue #2223: true when ≥1 match site was exhaustiveness-checked.
    bool adt_match_sites_present = false;
    std::uint32_t notes_count = 0;
    std::uint32_t adt_sites_checked = 0;
    std::uint32_t adt_non_exhaustive = 0;
    [[nodiscard]] bool all_ok() const noexcept {
        return type_ok && linear_ok && provenance_ok && adt_ok && !cross_batch_linear_escape &&
               !cross_closure_linear_escape;
    }
};

// Issue #2563: hard-gate for cross-closure free-capture escape.
// Soft default: observe-only. AURA_LINEAR_CROSS_CLOSURE_HARD=1|on forces;
// 0|off forces soft observe. Unset → production_defaults || Full.
[[nodiscard]] inline bool linear_cross_closure_hard_enabled() noexcept {
    const char* e = std::getenv("AURA_LINEAR_CROSS_CLOSURE_HARD");
    if (e && *e) {
        if (e[0] == '0' || e[0] == 'f' || e[0] == 'F' || e[0] == 'n' || e[0] == 'N')
            return false;
        if ((e[0] == 'o' || e[0] == 'O') && e[1] != '\0' && (e[1] == 'f' || e[1] == 'F'))
            return false;
        if (e[0] == 's' || e[0] == 'S') // soft
            return false;
        return true;
    }
    return production_defaults_active() || get_strategy() == AuditStrategy::Full;
}

// Issue #2612 / #2623: free-capture discovery depth.
//   Soft/dev unset → 1 (legacy #2563 one-level; no nested walk)
//   production_defaults unset → 2 (nested free-capture; still cone-capped)
//   AURA_LINEAR_CROSS_CLOSURE_DEPTH=0 → disable discovery (zero cost)
//   1..3 → use value; values >3 clamp to hard max 3
// Hard force path still linear_cross_closure_hard_enabled() only
// (depth alone never forces; trunc under hard is fail-closed — #2623).
[[nodiscard]] inline int linear_cross_closure_depth_cap() noexcept {
    constexpr int kMax = 3;
    const char* e = std::getenv("AURA_LINEAR_CROSS_CLOSURE_DEPTH");
    if (!e || !*e) {
        // Issue #2623: production default depth 2; Soft/dev remains 1.
        if (production_defaults_active())
            return 2;
        return 1;
    }
    if (e[0] == '0')
        return 0; // emergency disable (#2623 AC5)
    if (e[0] == '1')
        return 1;
    if (e[0] == '2')
        return 2;
    if (e[0] == '3')
        return 3;
    // Non-numeric or larger digits → clamp to hard max.
    if (e[0] >= '4' && e[0] <= '9')
        return kMax;
    return 1;
}

// Issue #2027: stamp composite audit outcome (nested and/or atomic_batch).
inline void record_composite_invariant_audit(bool nested, bool batch_active,
                                             const InvariantAuditResult& r) noexcept {
    auto& c = g_typed_mutation_audit_counters;
    c.composite_invariant_audits_total.fetch_add(1, std::memory_order_relaxed);
    if (nested)
        c.composite_nested_audit_total.fetch_add(1, std::memory_order_relaxed);
    if (batch_active)
        c.composite_batch_audit_total.fetch_add(1, std::memory_order_relaxed);
    if (r.cross_batch_linear_escape)
        c.composite_cross_batch_linear_escape_total.fetch_add(1, std::memory_order_relaxed);
    if (r.all_ok())
        c.composite_invariant_ok_total.fetch_add(1, std::memory_order_relaxed);
    else
        c.composite_invariant_fail_total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #2105: result of ordered composite_txn_commit barrier.
struct CompositeTxnCommitResult {
    bool committed = false;
    bool solve_ok = true;
    bool linear_ok = true;
    bool audit_ok = true;
    bool partial_recovered = false;
    bool rejected = false;
    // Issue #2221: blame-complete gate result (true when not checked,
    // vacuous empty CS, or last_blame_chain.is_complete()).
    bool blame_ok = true;
    // Issue #2898: required TypeId set all concrete (true when span empty
    // or every required id has a non-var UF binding after solve).
    bool required_type_ok = true;
    std::uint32_t required_type_fail_count = 0;
    InvariantAuditResult audit{};
};

// Issue #1884: stamp last TypePropagationPass / DCE narrow metrics for
// the next invariant audit correlation window.
inline void note_type_propagation_pass(std::uint64_t fixpoint_rounds, std::uint64_t narrow_hits,
                                       std::uint64_t extended_ops) noexcept {
    g_typed_mutation_audit_counters.last_type_prop_fixpoint_rounds.store(fixpoint_rounds,
                                                                         std::memory_order_relaxed);
    g_typed_mutation_audit_counters.last_type_prop_narrow_hits.store(narrow_hits,
                                                                     std::memory_order_relaxed);
    g_typed_mutation_audit_counters.last_type_prop_extended_ops.store(extended_ops,
                                                                      std::memory_order_relaxed);
}

inline void note_dce_narrow_hits(std::uint64_t narrow_hits) noexcept {
    g_typed_mutation_audit_counters.last_dce_narrow_hits.store(narrow_hits,
                                                               std::memory_order_relaxed);
}

inline void note_predicate_memo_eviction(std::uint64_t n) noexcept {
    if (n == 0)
        return;
    g_typed_mutation_audit_counters.last_predicate_memo_evictions.fetch_add(
        n, std::memory_order_relaxed);
    // Correlate with last invariant outcome (self-evo thrash under fail).
    if (g_typed_mutation_audit_counters.last_invariant_all_ok.load(std::memory_order_relaxed) ==
        0) {
        g_typed_mutation_audit_counters.predicate_memo_evict_correlated_total.fetch_add(
            n, std::memory_order_relaxed);
    }
}

// Purpose: correlate last TypeProp/DCE/memo snapshot with one invariant audit
// Pre: note_type_propagation_pass / note_dce_narrow_hits may have stamped last_*
// Post: bumps correlation_total; may bump pass/fail-with-evidence and evidence_lost
// Safety Class: P2 (observability; relaxed atomics; no throw)
// Issue: #1884 / #1886
// AI-Native Rationale: self-evo maps type_invariant_fail to narrow_evidence
//   via query:type-propagation-invariant-stats without replaying the pipeline
inline void correlate_invariant_with_type_system(const InvariantAuditResult& r) noexcept {
    auto& c = g_typed_mutation_audit_counters;
    c.type_prop_invariant_correlation_total.fetch_add(1, std::memory_order_relaxed);
    c.last_invariant_all_ok.store(r.all_ok() ? 1 : 0, std::memory_order_relaxed);
    const auto narrow = c.last_type_prop_narrow_hits.load(std::memory_order_relaxed) +
                        c.last_dce_narrow_hits.load(std::memory_order_relaxed);
    const auto fixpoint = c.last_type_prop_fixpoint_rounds.load(std::memory_order_relaxed);
    const bool had_evidence = narrow > 0 || fixpoint > 0 ||
                              c.last_type_prop_extended_ops.load(std::memory_order_relaxed) > 0;
    if (r.all_ok()) {
        if (had_evidence)
            c.type_prop_invariant_pass_with_evidence_total.fetch_add(1, std::memory_order_relaxed);
    } else {
        if (had_evidence)
            c.type_prop_invariant_fail_with_evidence_total.fetch_add(1, std::memory_order_relaxed);
        // Evidence present but type invariant failed → "lost" for AI debug.
        if (!r.type_ok && narrow > 0)
            c.type_prop_evidence_lost_total.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void record_invariant_audit_result(std::uint64_t mutation_id, std::string_view op,
                                          const InvariantAuditResult& r,
                                          std::uint64_t before_epoch = 0,
                                          std::uint64_t after_epoch = 0,
                                          std::uint32_t target_node = 0, std::int64_t fiber_id = 0,
                                          std::uint64_t tenant_id = 0) noexcept {
    // Issue #2814: link trail Success/Error to real enforcement suite.
    note_invariant_enforcement_ran(mutation_id);
    g_typed_mutation_audit_counters.invariant_audits.fetch_add(1, std::memory_order_relaxed);
    // #1894 AC: exact metric name for audit triggers.
    g_typed_mutation_audit_counters.typed_mutation_audit_triggered_total.fetch_add(
        1, std::memory_order_relaxed);
    if (r.type_ok)
        g_typed_mutation_audit_counters.type_invariant_ok.fetch_add(1, std::memory_order_relaxed);
    else
        g_typed_mutation_audit_counters.type_invariant_fail.fetch_add(1, std::memory_order_relaxed);
    if (r.linear_ok)
        g_typed_mutation_audit_counters.linear_invariant_ok.fetch_add(1, std::memory_order_relaxed);
    else
        g_typed_mutation_audit_counters.linear_invariant_fail.fetch_add(1,
                                                                        std::memory_order_relaxed);
    if (r.provenance_ok)
        g_typed_mutation_audit_counters.provenance_invariant_ok.fetch_add(
            1, std::memory_order_relaxed);
    else
        g_typed_mutation_audit_counters.provenance_invariant_fail.fetch_add(
            1, std::memory_order_relaxed);
    // Issue #2223 / #2264: ADT exhaustiveness dimension.
    if (r.adt_ok)
        g_typed_mutation_audit_counters.adt_invariant_ok.fetch_add(1, std::memory_order_relaxed);
    else
        g_typed_mutation_audit_counters.adt_invariant_fail.fetch_add(1, std::memory_order_relaxed);
    // Issue #2264: one audit sample that exercised ADT exhaustiveness (or inject).
    if (r.adt_sites_checked > 0 || r.adt_match_sites_present || !r.adt_ok)
        g_typed_mutation_audit_counters.adt_exhaustiveness_audit_total.fetch_add(
            1, std::memory_order_relaxed);
    if (r.adt_sites_checked > 0)
        g_typed_mutation_audit_counters.adt_exhaustiveness_sites_checked_total.fetch_add(
            r.adt_sites_checked, std::memory_order_relaxed);
    if (r.adt_non_exhaustive > 0)
        g_typed_mutation_audit_counters.adt_non_exhaustive_sites_total.fetch_add(
            r.adt_non_exhaustive, std::memory_order_relaxed);
    // Issue #2264: fail total once per audit when adt_ok is false.
    if (!r.adt_ok)
        g_typed_mutation_audit_counters.adt_exhaustiveness_fail_total.fetch_add(
            1, std::memory_order_relaxed);
    // Issue #1884: correlate with last TypePropagation / DCE / memo snapshot.
    correlate_invariant_with_type_system(r);
    if (r.all_ok()) {
        g_typed_mutation_audit_counters.invariant_all_pass.fetch_add(1, std::memory_order_relaxed);
        // Issue #1924: successful invariant suite with mutation_id ⇒
        // blame chain considered complete for this audit sample.
        if (mutation_id != 0) {
            g_typed_mutation_audit_counters.blame_chain_complete_total.fetch_add(
                1, std::memory_order_relaxed);
        }
        capture_audit_event(mutation_id, op, classify_kind(op), before_epoch, after_epoch,
                            AuditOutcome::Success, target_node, r.notes_count, fiber_id,
                            r.notes_count);
    } else {
        g_typed_mutation_audit_counters.invariant_violations_caught.fetch_add(
            1, std::memory_order_relaxed);
        g_typed_mutation_audit_counters.typed_mutation_violations_caught_total.fetch_add(
            1, std::memory_order_relaxed);
        // #1894: dual-record blame for forensic self-evo rollback trails.
        g_typed_mutation_audit_counters.provenance_blame_chain_hits_total.fetch_add(
            1, std::memory_order_relaxed);
        // Issue #1924: invariant fail under mutation ⇒ potential blame miss.
        if (mutation_id != 0) {
            g_typed_mutation_audit_counters.blame_propagation_miss_total.fetch_add(
                1, std::memory_order_relaxed);
        }
        aura::core::provenance::record_macro_hygiene_provenance(
            target_node, tenant_id, mutation_id, static_cast<std::uint32_t>(fiber_id));
        capture_audit_event(mutation_id, "invariant-fail", MutationKind::Other, before_epoch,
                            after_epoch, AuditOutcome::Error, target_node, r.notes_count, fiber_id,
                            r.notes_count);
    }
}

[[nodiscard]] inline std::uint64_t trail_size() noexcept {
    const auto writes =
        g_typed_mutation_audit_counters.trail_writes.load(std::memory_order_relaxed);
    return writes < kTypedMutationAuditTrailSize ? writes : kTypedMutationAuditTrailSize;
}

[[nodiscard]] inline std::uint64_t trail_seq() noexcept {
    return g_typed_mutation_audit_counters.trail_seq.load(std::memory_order_relaxed);
}

// Copy latest event (seq-1) or empty if none.
// Issue #2819: lock-free read (no mu); best-effort under concurrent wrap.
[[nodiscard]] inline bool trail_latest(TypedMutationAuditEvent& out) noexcept {
    const auto head = trail_seq();
    if (head == 0)
        return false;
    out = g_trail().ring[(head - 1) % kTypedMutationAuditTrailSize];
    return true;
}

// Copy event by absolute seq if still in ring window.
// Issue #2819: lock-free read; require out.seq == seq (drop torn/overwritten).
[[nodiscard]] inline bool trail_at_seq(std::uint64_t seq, TypedMutationAuditEvent& out) noexcept {
    const auto head = trail_seq();
    if (head == 0 || seq >= head)
        return false;
    if (head > kTypedMutationAuditTrailSize && seq < head - kTypedMutationAuditTrailSize)
        return false;
    out = g_trail().ring[seq % kTypedMutationAuditTrailSize];
    return out.seq == seq;
}

// Issue #2054: newest-first scan for mutation_id correlation join.
// Returns true and copies the most recent matching event still in ring.
// Issue #2819: lock-free scan (best-effort under concurrent wrap).
[[nodiscard]] inline bool trail_find_by_mutation_id(std::uint64_t mutation_id,
                                                    TypedMutationAuditEvent& out) noexcept {
    if (mutation_id == 0)
        return false;
    const auto head = trail_seq();
    if (head == 0)
        return false;
    const std::size_t window = head < kTypedMutationAuditTrailSize ? static_cast<std::size_t>(head)
                                                                   : kTypedMutationAuditTrailSize;
    for (std::size_t i = 0; i < window; ++i) {
        const auto e = g_trail().ring[(head - 1 - i) % kTypedMutationAuditTrailSize];
        if (e.mutation_id == mutation_id) {
            out = e;
            return true;
        }
    }
    return false;
}

inline void snapshot_global(std::uint64_t& considered, std::uint64_t& skipped,
                            std::uint64_t& contextual, std::uint64_t& trail_sz,
                            std::uint64_t& rollbacks, std::uint64_t& errors,
                            std::uint32_t& strategy, std::uint32_t& sample_ratio) noexcept {
    considered = g_typed_mutation_audit_counters.audits_considered.load(std::memory_order_relaxed);
    skipped = g_typed_mutation_audit_counters.samples_skipped.load(std::memory_order_relaxed);
    contextual = g_typed_mutation_audit_counters.contextual_total.load(std::memory_order_relaxed);
    trail_sz = trail_size();
    rollbacks = g_typed_mutation_audit_counters.rollbacks.load(std::memory_order_relaxed);
    errors = g_typed_mutation_audit_counters.errors.load(std::memory_order_relaxed);
    strategy = g_typed_mutation_audit_counters.strategy.load(std::memory_order_relaxed);
    sample_ratio = g_typed_mutation_audit_counters.sample_ratio.load(std::memory_order_relaxed);
}

// Test helper: reset counters + trail (not for production hot path).
// Ends with apply_dev_audit_defaults() (Sampled/4 + dev_audit_opt_in) so
// unit tests keep the fast-iteration path; cold-start process default is
// Full (#2818) until this or apply_dev is called.
inline void reset_for_test() noexcept {
    g_last_stamped_audit_mid.store(0, std::memory_order_relaxed);
    clear_boundary_audit_mid();
    g_typed_mutation_audit_counters.audits_considered.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.samples_skipped.store(0, std::memory_order_relaxed);
    // Issue #2818
    g_typed_mutation_audit_counters.audit_strategy_default_warnings_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.audit_strategy_default_warning_fired.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.contextual_total.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.trail_writes.store(0, std::memory_order_relaxed);
    // Issue #2819
    g_typed_mutation_audit_counters.audit_trail_lockfree_total.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.audit_trail_mutex_wait_us_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.audit_trail_lockfree_wired.store(1, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.rollbacks.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.errors.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.trail_seq.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.macro_hygiene_events.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.macro_hygiene_blocked.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.macro_hygiene_allowed.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.invariant_audits.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.type_invariant_ok.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.type_invariant_fail.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_invariant_ok.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_invariant_fail.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.provenance_invariant_ok.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.provenance_invariant_fail.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.adt_invariant_ok.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.adt_invariant_fail.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.adt_exhaustiveness_sites_checked_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.adt_non_exhaustive_sites_total.store(0,
                                                                         std::memory_order_relaxed);
    g_typed_mutation_audit_counters.adt_exhaustiveness_audit_total.store(0,
                                                                         std::memory_order_relaxed);
    g_typed_mutation_audit_counters.adt_exhaustiveness_fail_total.store(0,
                                                                        std::memory_order_relaxed);
    g_typed_mutation_audit_counters.invariant_violations_caught.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.invariant_all_pass.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.typed_mutation_audit_triggered_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.typed_mutation_violations_caught_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.provenance_blame_chain_hits_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.blame_chain_complete_total.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.blame_propagation_miss_total.store(0,
                                                                       std::memory_order_relaxed);
    g_typed_mutation_audit_counters.full_strategy_force_rollback_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.contextual_force_audit_total.store(0,
                                                                       std::memory_order_relaxed);
    g_typed_mutation_audit_counters.hard_gate_audits_total.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.hard_gate_force_rollback_total.store(0,
                                                                         std::memory_order_relaxed);
    g_typed_mutation_audit_counters.hard_gate_strict_hold_total.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.hard_gate_sampled_skip_total.store(0,
                                                                       std::memory_order_relaxed);
    g_typed_mutation_audit_counters.hard_gate_wired.store(1, std::memory_order_relaxed);
    // Issue #2814 M7 enforcement-link counters.
    g_typed_mutation_audit_counters.audit_enforcement_link_wired.store(1,
                                                                       std::memory_order_relaxed);
    g_typed_mutation_audit_counters.audit_enforcement_ran_total.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.audit_enforcement_skipped_intentional_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.audit_enforcement_gap_total.store(0, std::memory_order_relaxed);
    g_tls_enforcement_link_mid = 0;
    g_tls_enforcement_link = EnforcementLinkKind::None;
    g_typed_mutation_audit_counters.boundary_solve_hard_gate_total.store(0,
                                                                         std::memory_order_relaxed);
    g_typed_mutation_audit_counters.boundary_solve_full_resync_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.boundary_solve_force_rollback_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.boundary_solve_truncated_seen_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.boundary_solve_hard_gate_wired.store(1,
                                                                         std::memory_order_relaxed);
    g_typed_mutation_audit_counters.aot_hotupdate_attempts.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.aot_hotupdate_audits.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.aot_hotupdate_ok.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.aot_hotupdate_fail.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.aot_hotupdate_invariant_fail_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.jit_hotpath_audits.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.audit_mutation_id_gen.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.audit_mid_fallback_gen_total.store(0,
                                                                       std::memory_order_relaxed);
    // Issue #2836
    g_typed_mutation_audit_counters.audit_mid_fallback_refused_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.audit_mid_fallback_refuse_se_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.audit_mid_fallback_refuse_se_seq.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.type_prop_invariant_correlation_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.type_prop_invariant_pass_with_evidence_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.type_prop_invariant_fail_with_evidence_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.type_prop_evidence_lost_total.store(0,
                                                                        std::memory_order_relaxed);
    g_typed_mutation_audit_counters.predicate_memo_evict_correlated_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.last_type_prop_fixpoint_rounds.store(0,
                                                                         std::memory_order_relaxed);
    g_typed_mutation_audit_counters.last_type_prop_narrow_hits.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.last_type_prop_extended_ops.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.last_dce_narrow_hits.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.last_predicate_memo_evictions.store(0,
                                                                        std::memory_order_relaxed);
    g_typed_mutation_audit_counters.last_invariant_all_ok.store(1, std::memory_order_relaxed);
    // Issue #2027 composite counters
    g_typed_mutation_audit_counters.composite_invariant_audits_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_invariant_ok_total.store(0,
                                                                       std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_invariant_fail_total.store(0,
                                                                         std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_partial_recover_type_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_partial_recover_linear_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_partial_recover_success_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_full_rollback_total.store(0,
                                                                        std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_nested_audit_total.store(0,
                                                                       std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_batch_audit_total.store(0, std::memory_order_relaxed);
    // Issue #2105 composite commit barrier
    g_typed_mutation_audit_counters.composite_commit_revalidate_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_commit_ok_total.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_commit_reject_total.store(0,
                                                                        std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_commit_solve_fail_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_commit_linear_fail_total.store(
        0, std::memory_order_relaxed);
    // Issue #2180
    g_typed_mutation_audit_counters.composite_commit_solve_reuse_hit_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_commit_solve_empty_cs_total.store(
        0, std::memory_order_relaxed);
    // Issue #2345
    g_typed_mutation_audit_counters.composite_commit_empty_cs_hard_miss_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_commit_empty_cs_observe_total.store(
        0, std::memory_order_relaxed);
    // Issue #2509
    g_typed_mutation_audit_counters.composite_commit_unexpected_cs_work_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_commit_expected_has_work_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_commit_sdo_entered_total.store(
        0, std::memory_order_relaxed);
    // Issue #2610
    g_typed_mutation_audit_counters.composite_commit_auto_partial_from_cone_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_commit_auto_partial_from_cone_observe_total.store(
        0, std::memory_order_relaxed);
    // Issue #2851
    g_typed_mutation_audit_counters.composite_commit_log_forces_partial_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_commit_log_forces_partial_observe_total.store(
        0, std::memory_order_relaxed);
    // Issue #2898
    g_typed_mutation_audit_counters.composite_required_type_fail_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_required_type_observe_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_required_type_checked_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_required_type_auto_fill_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_required_type_auto_fill_capped_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_required_type_reject_over_infer_total.store(
        0, std::memory_order_relaxed);
    clear_composite_required_solved();
    reset_composite_required_reject_over_infer_for_test();
    // Issue #2458
    g_typed_mutation_audit_counters.truncate_commit_observe_total.store(0,
                                                                        std::memory_order_relaxed);
    g_typed_mutation_audit_counters.truncate_commit_reject_total.store(0,
                                                                       std::memory_order_relaxed);
    g_typed_mutation_audit_counters.truncate_commit_full_solve_recover_total.store(
        0, std::memory_order_relaxed);
    // Issue #2221
    g_typed_mutation_audit_counters.blame_commit_check_total.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.blame_commit_reject_total.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.blame_commit_incomplete_observe_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_cross_batch_linear_escape_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_escape_commit_blocked_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.composite_partial_recover_attempt_total.store(
        0, std::memory_order_relaxed);
    // Issue #2029 Full per-category partial recovery
    g_typed_mutation_audit_counters.partial_recovery_attempt_total.store(0,
                                                                         std::memory_order_relaxed);
    g_typed_mutation_audit_counters.partial_recovery_success_total.store(0,
                                                                         std::memory_order_relaxed);
    g_typed_mutation_audit_counters.partial_recovery_fail_total.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.partial_recovery_type_total.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.partial_recovery_linear_total.store(0,
                                                                        std::memory_order_relaxed);
    g_typed_mutation_audit_counters.partial_recovery_provenance_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.partial_recovery_adt_total.store(0, std::memory_order_relaxed);
    // Issue #2514 / #2545
    g_typed_mutation_audit_counters.linear_synth_boundary_force_rollback_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_synth_boundary_skip_recovery_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_synth_authority_unified.store(1,
                                                                         std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_force_unified_2545.store(1, std::memory_order_relaxed);
    // Issue #2563
    g_typed_mutation_audit_counters.linear_cross_closure_escape_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_cross_closure_force_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_cross_closure_observe_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_cross_closure_cap_trunc_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_cross_closure_wired.store(1, std::memory_order_relaxed);
    // Issue #2612
    g_typed_mutation_audit_counters.linear_cross_closure_depth2_entries_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_cross_closure_depth2_escape_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_cross_closure_depth_wired.store(
        1, std::memory_order_relaxed);
    // Issue #2623
    g_typed_mutation_audit_counters.linear_cross_closure_trunc_force_total.store(
        0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_cross_closure_depth_max.store(3,
                                                                         std::memory_order_relaxed);
    g_typed_mutation_audit_counters.linear_cross_closure_prod_depth_default.store(
        2, std::memory_order_relaxed);
    reset_linear_compact_root_consistency_for_test();
    set_last_proof_linear_root_count_for_test(0);
    apply_dev_audit_defaults(); // Sampled/4 + dev_audit_opt_in; clears production
    std::lock_guard lock(g_trail().mu);
    for (auto& e : g_trail().ring)
        e = TypedMutationAuditEvent{};
}

// Issue #2703 / #2909: production hard-face when partial cone truncates
// outside-If OccurrenceGoals. Under infer_flat_partial soft/hard cone
// overflow (#2560), goals whose predicate If sits outside the truncated
// cone are dropped. Issue #2703 surfaces force_reason
// "cone_outside_goal_drop" (code 10). Issue #2909: production/Full +
// truncate + outside drop MUST force full-solve recover (or hard reject)
// before green commit — no silent half-green. Soft: counter-only.
// Quiet (no truncate / empty outside set): zero cost.
//
// Soft vs production decision table (#2909 AC6 — code comments only):
//   Soft + truncate + outside drop  → soft counter; allow (observe)
//   production/Full + truncate + outside drop → force recover OR reject
//   no truncate / empty outside set → zero cost (no counter / no solve)
inline std::atomic<std::uint64_t> g_cone_outside_goal_drop_total{0};
inline std::atomic<std::uint64_t> g_cone_outside_goal_drop_soft_total{0};
inline std::atomic<std::uint32_t> g_cone_outside_goal_drop_wired{1};
inline constexpr int kConeOutsideGoalDropIssue = 2703;
// Issue #2909: force-closure recover after cone truncate + outside drop.
// Counters declared earlier (before commit_readiness); accessors here.
inline constexpr int kConeTruncateForceClosureIssue = 2909;

[[nodiscard]] inline std::uint64_t cone_outside_goal_drop_total_v_read() noexcept {
    return g_cone_outside_goal_drop_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t cone_outside_goal_drop_soft_total_v_read() noexcept {
    return g_cone_outside_goal_drop_soft_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t cone_outside_goal_drop_wired_v_read() noexcept {
    return g_cone_outside_goal_drop_wired.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t cone_truncate_force_closure_attempt_total_v_read() noexcept {
    return g_cone_truncate_force_closure_attempt_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t cone_truncate_force_closure_total_v_read() noexcept {
    return g_cone_truncate_force_closure_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t cone_truncate_force_closure_reject_total_v_read() noexcept {
    return g_cone_truncate_force_closure_reject_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t cone_truncate_force_closure_wired_v_read() noexcept {
    return g_cone_truncate_force_closure_wired.load(std::memory_order_relaxed);
}

// Publish outside-If goal drop from infer_flat_partial (#2703 / #2909).
// Soft → soft_total only; production/Full → face total (commit hard path).
inline void publish_cone_outside_goal_drop(std::uint64_t n = 1) noexcept {
    if (n == 0)
        return;
    const bool hard = production_defaults_active() || get_strategy() == AuditStrategy::Full;
    if (hard)
        g_cone_outside_goal_drop_total.fetch_add(n, std::memory_order_relaxed);
    else
        g_cone_outside_goal_drop_soft_total.fetch_add(n, std::memory_order_relaxed);
}

// Issue #2911: publish unified refined-consistency drift face.
// Soft → observe only; production/Full → face latch for commit_readiness.
// Counters declared earlier (before commit_readiness); accessors here.
inline void note_refined_consistency_drift(bool production_hard) noexcept {
    if (production_hard) {
        g_refined_consistency_drift_face.store(1, std::memory_order_relaxed);
    } else {
        g_refined_consistency_observe_total.fetch_add(1, std::memory_order_relaxed);
    }
}
[[nodiscard]] inline bool refined_consistency_drift_face_hit() noexcept {
    return g_refined_consistency_drift_face.load(std::memory_order_relaxed) != 0;
}
[[nodiscard]] inline std::uint64_t refined_consistency_observe_total_v_read() noexcept {
    return g_refined_consistency_observe_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t refined_consistency_reject_total_v_read() noexcept {
    return g_refined_consistency_reject_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t refined_consistency_recover_total_v_read() noexcept {
    return g_refined_consistency_recover_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t refined_consistency_wired_v_read() noexcept {
    return g_refined_consistency_wired.load(std::memory_order_relaxed);
}
inline void clear_refined_consistency_drift_for_test() noexcept {
    g_refined_consistency_drift_face.store(0, std::memory_order_relaxed);
    g_refined_consistency_observe_total.store(0, std::memory_order_relaxed);
    g_refined_consistency_reject_total.store(0, std::memory_order_relaxed);
    g_refined_consistency_recover_total.store(0, std::memory_order_relaxed);
}

// Test / recover reset.
inline void clear_cone_outside_goal_drop_for_test() noexcept {
    g_cone_outside_goal_drop_total.store(0, std::memory_order_relaxed);
    g_cone_outside_goal_drop_soft_total.store(0, std::memory_order_relaxed);
}
inline void clear_cone_truncate_force_closure_for_test() noexcept {
    g_cone_truncate_force_closure_attempt_total.store(0, std::memory_order_relaxed);
    g_cone_truncate_force_closure_total.store(0, std::memory_order_relaxed);
    g_cone_truncate_force_closure_reject_total.store(0, std::memory_order_relaxed);
    // Issue #2962: Agent-facing residual counters (same test reset surface).
    g_cone_outside_goal_drop_recover_ok_total.store(0, std::memory_order_relaxed);
    g_cone_outside_goal_drop_reject_total.store(0, std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t cone_outside_goal_drop_recover_ok_total_v_read() noexcept {
    return g_cone_outside_goal_drop_recover_ok_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t cone_outside_goal_drop_reject_total_v_read() noexcept {
    return g_cone_outside_goal_drop_reject_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t cone_outside_goal_drop_recover_reject_wired_v_read() noexcept {
    return g_cone_outside_goal_drop_recover_reject_wired.load(std::memory_order_relaxed);
}

// ── Issue #2847: region type/occurrence commit bind ────────────────────
// Residual of #2724/#2760/#2761: region concurrent admit isolates AST
// topology mutation but type/occurrence state is still per-Evaluator
// shared (solve_delta_cs_ / occurrence_goals_ / type_dep). Two fibers on
// "disjoint" regions can cross-talk via shared CS. This face rejects
// commit when any touched OccurrenceGoal predicate node bit falls
// outside the admitted cone/ImpactScope mask.
//
// Soft: metric only (region_type_cross_talk_observe_total).
// production / Full: reject (region_type_cross_talk_reject_total) + face
// for commit_readiness force_reason "region_type_cross_talk" (code 13).
// mask==0 / GlobalExclusive: zero cost (region_type_commit_ok short-circuit).
inline constexpr int kRegionTypeCrossTalkIssue = 2847;
inline std::atomic<std::uint64_t> g_region_type_cross_talk_observe_total{0};
inline std::atomic<std::uint64_t> g_region_type_cross_talk_reject_total{0};
// Face latch for commit_readiness (1 = hit; cleared for tests / recover).
inline std::atomic<std::uint8_t> g_region_type_cross_talk_face{0};
inline std::atomic<std::uint32_t> g_region_type_cross_talk_wired{1};

[[nodiscard]] inline std::uint64_t region_type_cross_talk_observe_total_v_read() noexcept {
    return g_region_type_cross_talk_observe_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t region_type_cross_talk_reject_total_v_read() noexcept {
    return g_region_type_cross_talk_reject_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t region_type_cross_talk_wired_v_read() noexcept {
    return g_region_type_cross_talk_wired.load(std::memory_order_relaxed);
}
[[nodiscard]] inline bool region_type_cross_talk_face_hit() noexcept {
    return g_region_type_cross_talk_face.load(std::memory_order_relaxed) != 0;
}
inline void clear_region_type_cross_talk_for_test() noexcept {
    g_region_type_cross_talk_observe_total.store(0, std::memory_order_relaxed);
    g_region_type_cross_talk_reject_total.store(0, std::memory_order_relaxed);
    g_region_type_cross_talk_face.store(0, std::memory_order_relaxed);
}

// Map OccurrenceGoal predicate NodeId → one bit of a 64-bit cone mask
// (same 63-bit packing as impact_block_to_region_mask_bit / region_key
// — no tree walk). Hot path remains pure arithmetic.
[[nodiscard]] inline std::uint64_t node_id_to_region_mask_bit(std::uint32_t node_id) noexcept {
    if (node_id == 0)
        return 0;
    return 1ULL << (static_cast<std::uint64_t>(node_id) % 63ull);
}

// Pure gate: admitted_mask==0 → ok (global exclusive / quiet).
// touched==0 → ok (no type/occurrence work this boundary).
// Else: every touched bit must sit inside admitted_mask.
[[nodiscard]] inline bool region_type_commit_ok(std::uint64_t admitted_mask,
                                                std::uint64_t touched_type_mask) noexcept {
    if (admitted_mask == 0)
        return true;
    if (touched_type_mask == 0)
        return true;
    return (touched_type_mask & ~admitted_mask) == 0;
}

// Note cross-talk. production_hard → reject counter + face latch;
// Soft → observe counter only (commit may still succeed).
inline void note_region_type_cross_talk(bool production_hard) noexcept {
    if (production_hard) {
        g_region_type_cross_talk_reject_total.fetch_add(1, std::memory_order_relaxed);
        g_region_type_cross_talk_face.store(1, std::memory_order_release);
    } else {
        g_region_type_cross_talk_observe_total.fetch_add(1, std::memory_order_relaxed);
    }
}

// Issue #2704: production hard-face on OccurrenceGoal rehydrate miss after
// steal / densify fence. TypeChecker::note_steal_or_densify_epoch_fence
// advances cache epoch + prunes stale OccurrenceGoals + attempts
// rehydrate_occurrence_from_persist. When rehydrate returns 0 under
// production (persist enabled but buffer empty / wrong mid / no prior
// snapshot), the code only bumps occurrence_persist_rehydrate_miss_total
// and continues. After steal / Moving densify, live occurrence priority
// roots can be empty while Agents still see a green commit path.
// This issue surfaces the distinct force_reason
// "occurrence_empty_after_fence" (code 11) and bumps
// g_occurrence_empty_after_fence_total. Soft path bumps counter only;
// production path hard-rejects commit (no silent allow).
inline std::atomic<std::uint64_t> g_occurrence_empty_after_fence_total{0};
inline std::atomic<std::uint64_t> g_occurrence_empty_after_fence_soft_total{0};
inline std::atomic<std::uint32_t> g_occurrence_empty_after_fence_wired{1};
inline constexpr int kOccurrenceEmptyAfterFenceIssue = 2704;

[[nodiscard]] inline std::uint64_t occurrence_empty_after_fence_total_v_read() noexcept {
    return g_occurrence_empty_after_fence_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t occurrence_empty_after_fence_soft_total_v_read() noexcept {
    return g_occurrence_empty_after_fence_soft_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t occurrence_empty_after_fence_wired_v_read() noexcept {
    return g_occurrence_empty_after_fence_wired.load(std::memory_order_relaxed);
}

// Test reset.
inline void clear_occurrence_empty_after_fence_for_test() noexcept {
    g_occurrence_empty_after_fence_total.store(0, std::memory_order_relaxed);
    g_occurrence_empty_after_fence_soft_total.store(0, std::memory_order_relaxed);
}

// Issue #2896: fence rehydrate miss → latch #2704 face so
// commit_readiness_live_policy hard-rejects under production/Full.
// Soft (hard=false) bumps soft_total only (observe). Call from
// TypeChecker::note_steal_or_densify_epoch_fence after rehydrate
// returns 0 while persist is enabled.
inline void note_occurrence_empty_after_fence(bool production_hard) noexcept {
    if (production_hard) {
        g_occurrence_empty_after_fence_total.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_occurrence_empty_after_fence_soft_total.fetch_add(1, std::memory_order_relaxed);
    }
}

// Issue #2716: counter for the occurrence hard-face active branch
// (production / Full + face hit). Bumped when commit_readiness_live_policy
// detects a face hit under prod/Full — surface for Agent dashboards
// to attribute "active face wired in" vs "face fired but Soft path
// observed only". Additive — no regression on #2703 / #2704 /
// #2621 / #2458 / #2608 query keys. NOTE: the inline std::atomic
// definition was hoisted to before commit_readiness_live_policy
// (search #2728 ship co-traveler); the v_read accessor below references
// it by name.
[[nodiscard]] inline std::uint64_t occurrence_hard_face_full_solve_recover_total_v_read() noexcept {
    return g_occurrence_hard_face_full_solve_recover_total.load(std::memory_order_relaxed);
}
inline void reset_occurrence_hard_face_full_solve_recover_total_for_test() noexcept {
    g_occurrence_hard_face_full_solve_recover_total.store(0, std::memory_order_relaxed);
}

} // namespace aura::compiler::typed_audit

#endif // AURA_COMPILER_TYPED_MUTATION_AUDIT_H
