// typed_mutation_audit.h — Issue #1589 / #1216 / #1882: production TypedMutationAuditPass.
// Thread-safe strategy gate, contextual event capture, in-memory ring trail.
// #1882: AOT hot-update + JIT hotpath audit capture (sampled by default).
// Header form so serve/evaluator/tests can include without module churn.

#ifndef AURA_COMPILER_TYPED_MUTATION_AUDIT_H
#define AURA_COMPILER_TYPED_MUTATION_AUDIT_H

#include "core/provenance_tracker.hh"
#include "core/resource_quota.hh"  // process_resource_quota_manager (#2493 mid resolve)
#include "core/workspace_epoch.hh" // current_mutation_epoch (#2493 mid resolve)
#include "audit_mid_fallback_slo.h" // MidFallbackSloInput + decide_audit_mid_fallback_slo (#2635 hard-deny)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

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
    std::atomic<std::uint64_t> rollbacks{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::uint32_t> strategy{static_cast<std::uint32_t>(AuditStrategy::Sampled)};
    std::atomic<std::uint32_t> sample_ratio{4}; // every Nth id when Sampled (N>=1)
    // Issue #2053: 1 when production security defaults applied (Full or ratio=1).
    std::atomic<std::uint32_t> production_defaults_active{0};
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
    std::atomic<std::uint64_t> audit_mid_fallback_gen_total{0};
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

// Ring buffer protected by mutex (writers on mutation path; readers via query).
struct TypedMutationAuditTrail {
    std::mutex mu;
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

// Issue #2621: process-wide last partial cone truncate (Agents + pure tests).
// Stamped by TypeChecker::infer_flat_partial after #2560 soft/hard truncate.
// Soft: observe only; production / AURA_PARTIAL_CONE_COMMIT_HARD → hard face.
inline std::atomic<std::uint8_t> g_last_partial_cone_truncated{0};
inline std::atomic<std::uint64_t> g_last_partial_cone_dropped{0};
inline std::atomic<std::uint64_t> g_last_partial_cone_fanout_trunc{0};
inline std::atomic<std::uint64_t> g_partial_cone_commit_observe_total{0};
inline std::atomic<std::uint64_t> g_partial_cone_commit_reject_total{0};
inline std::atomic<std::uint32_t> g_partial_cone_commit_gate_wired{1};
inline constexpr int kPartialConeCommitGateIssue = 2621;

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
inline void publish_partial_cone_truncate(bool truncated, std::uint64_t dropped,
                                          std::uint64_t fanout_trunc = 0) noexcept {
    g_last_partial_cone_truncated.store(truncated ? 1 : 0, std::memory_order_relaxed);
    g_last_partial_cone_dropped.store(dropped, std::memory_order_relaxed);
    if (fanout_trunc > 0)
        g_last_partial_cone_fanout_trunc.fetch_add(fanout_trunc, std::memory_order_relaxed);
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
}

// Issue #2053: production multi-tenant AI — capture every self-modify event.
// Full strategy (default under apply_production_audit_defaults). Dev/test
// keep Sampled/ratio=4 via apply_dev_audit_defaults / reset_typed_mutation_audit.
inline void apply_production_audit_defaults() noexcept {
    set_strategy(AuditStrategy::Full);
    set_sample_ratio(1);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
}

// Issue #2053: restore fast-iteration Sampled defaults (tests / AURA_SANDBOX=off).
// Issue #2185: does not flip coercion reject-on-miss — callers that want full
// dev restore should also call reset_coercion_provenance_miss_policy_for_test
// or apply_production_security_defaults with AURA_SANDBOX=off.
inline void apply_dev_audit_defaults() noexcept {
    set_strategy(AuditStrategy::Sampled);
    set_sample_ratio(4);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
}

// Thread-safe Full / Sampled / Off gate.
// Sampled: audit when mutation_id % sample_ratio == 0.
[[nodiscard]] inline bool should_audit(std::uint64_t mutation_id) noexcept {
    g_typed_mutation_audit_counters.audits_considered.fetch_add(1, std::memory_order_relaxed);
    const auto s = get_strategy();
    if (s == AuditStrategy::Off)
        return false;
    if (s == AuditStrategy::Full)
        return true;
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

[[nodiscard]] inline std::int64_t commit_readiness_reason_code(std::string_view r) noexcept {
    if (r == "cone_truncate")
        return 9; // #2621
    if (r == "auto_partial")
        return 6; // #2610
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
    return 0; // ok
}

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
    const bool trunc_face =
        (in.truncated_reverify && !in.truncated_full_solve_recovered) || in.partial_cone_truncated;
    if (trunc_face) {
        const bool cone_only = in.partial_cone_truncated &&
                               !(in.truncated_reverify && !in.truncated_full_solve_recovered);
        const std::string_view reason = cone_only ? "cone_truncate" : "truncate";
        if (in.truncate_hard)
            return (set(reason, false, 1000), r);
        return (set(reason, true, 7000), r); // Soft observe
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

    // 6) ok — clean SOLVED + linear + blame + !truncated.
    return (set("ok", true, 10000), r);
}

// Fill hard flags from live audit process state (still pure w.r.t. inputs
// once copied; callers that want hermetic tests pass CommitReadinessInput
// directly without this helper).
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
    return in;
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
//   4. next_audit_mutation_id() as last-resort join stamp (process-origin,
//      not a competing epoch vocabulary); bumps audit_mid_fallback_gen_total.
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
    // Issue #2635: production mid-fallback SLO hard-deny. Under production
    // defaults (or Full strategy — same hard-deny gate as the existing
    // capture_security_correlated_audit path at line 362/363), if the
    // live mid-fallback rate already exceeds the SLO threshold, refuse
    // the last-resort process-origin stamp (return 0) so callers can
    // deny or re-stamp with a real mid. The schedule-gate (#2630) is
    // the primary *admission* control; this is the secondary
    // *resolve-time* hard face so residual paths cannot silently
    // degrade SE ↔ TypedMutationAudit ↔ CapabilityGrant epoch
    // alignment after the gate admits the call. Soft /
    // AURA_SANDBOX=off / Sampled callers continue to allow fallback +
    // only bump counters (AC parity with #2594; AC3 explicit). The
    // input snapshot is best-effort (load-relaxed) — same generation
    // check race window as #2594.
    //
    // #2636 follow-up: removed the erroneous `AuditStrategy::Strict`
    // arm that the repro build's -Werror caught — AuditStrategy only
    // has {Off, Sampled, Full}; the "Strict" gating is expressed via
    // the separate `strict_sandbox` bool passed to the existing
    // capture_security_correlated_audit path, not via an enum value.
    const bool hard_deny_eligible =
        production_defaults_active() || get_strategy() == AuditStrategy::Full;
    if (hard_deny_eligible) {
        const MidFallbackSloInput slo{
            .fallback_gen = g_typed_mutation_audit_counters.audit_mid_fallback_gen_total.load(
                std::memory_order_relaxed),
            .contextual_total =
                g_typed_mutation_audit_counters.contextual_total.load(std::memory_order_relaxed),
            .production_defaults = production_defaults_active(),
            .soft_mode = false,
        };
        const auto d = decide_audit_mid_fallback_slo(slo);
        if (d.would_arm_degraded) {
            // Hard-deny: refuse process-origin stamp; caller treats
            // mid==0 as deny or surfaces a typed "mid-fallback-slo-breached"
            // error (e.g. capture_security_correlated_audit / AOT/JIT
            // audit paths). The schedule-gate (#2630) already saw the
            // same SLO signal at admission — this is the resolve-time
            // fail-closed face (AC4: same signal, no double-deny race).
            return 0;
        }
    }
    g_typed_mutation_audit_counters.audit_mid_fallback_gen_total.fetch_add(
        1, std::memory_order_relaxed);
    return next_audit_mutation_id();
}

// Core trail write (no Sampled gate). Used by capture_audit_event and by
// #2054 security-correlated emit (always-on so rings stay joined by
// mutation_id even under Sampled strategy).
inline void capture_audit_event_forced(std::uint64_t mutation_id, std::string_view name,
                                       MutationKind kind, std::uint64_t before_epoch,
                                       std::uint64_t after_epoch, AuditOutcome outcome,
                                       std::uint32_t target_node = 0,
                                       std::uint32_t nodes_changed = 0, std::int64_t fiber_id = 0,
                                       std::uint32_t affected_ref_count = 0) noexcept {
    TypedMutationAuditEvent ev{};
    ev.mutation_id = mutation_id;
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

    {
        std::lock_guard lock(g_trail().mu);
        g_trail().ring[seq % kTypedMutationAuditTrailSize] = ev;
    }

    g_typed_mutation_audit_counters.contextual_total.fetch_add(1, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.trail_writes.fetch_add(1, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.typed_mutation_audit_triggered_total.fetch_add(
        1, std::memory_order_relaxed);
    if (outcome == AuditOutcome::Rollback)
        g_typed_mutation_audit_counters.rollbacks.fetch_add(1, std::memory_order_relaxed);
    if (outcome == AuditOutcome::Error)
        g_typed_mutation_audit_counters.errors.fetch_add(1, std::memory_order_relaxed);
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
[[nodiscard]] inline bool trail_latest(TypedMutationAuditEvent& out) noexcept {
    const auto seq = trail_seq();
    if (seq == 0)
        return false;
    std::lock_guard lock(g_trail().mu);
    out = g_trail().ring[(seq - 1) % kTypedMutationAuditTrailSize];
    return true;
}

// Copy event by absolute seq if still in ring window.
[[nodiscard]] inline bool trail_at_seq(std::uint64_t seq, TypedMutationAuditEvent& out) noexcept {
    const auto head = trail_seq();
    if (head == 0 || seq >= head)
        return false;
    if (head > kTypedMutationAuditTrailSize && seq < head - kTypedMutationAuditTrailSize)
        return false;
    std::lock_guard lock(g_trail().mu);
    out = g_trail().ring[seq % kTypedMutationAuditTrailSize];
    return out.seq == seq;
}

// Issue #2054: newest-first scan for mutation_id correlation join.
// Returns true and copies the most recent matching event still in ring.
[[nodiscard]] inline bool trail_find_by_mutation_id(std::uint64_t mutation_id,
                                                    TypedMutationAuditEvent& out) noexcept {
    if (mutation_id == 0)
        return false;
    const auto head = trail_seq();
    if (head == 0)
        return false;
    const std::size_t window = head < kTypedMutationAuditTrailSize ? static_cast<std::size_t>(head)
                                                                   : kTypedMutationAuditTrailSize;
    std::lock_guard lock(g_trail().mu);
    for (std::size_t i = 0; i < window; ++i) {
        const auto& e = g_trail().ring[(head - 1 - i) % kTypedMutationAuditTrailSize];
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
inline void reset_for_test() noexcept {
    g_typed_mutation_audit_counters.audits_considered.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.samples_skipped.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.contextual_total.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.trail_writes.store(0, std::memory_order_relaxed);
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
    apply_dev_audit_defaults(); // Sampled/4; clears production_defaults_active
    std::lock_guard lock(g_trail().mu);
    for (auto& e : g_trail().ring)
        e = TypedMutationAuditEvent{};
}

} // namespace aura::compiler::typed_audit

#endif // AURA_COMPILER_TYPED_MUTATION_AUDIT_H
