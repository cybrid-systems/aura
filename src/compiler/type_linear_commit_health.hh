// type_linear_commit_health.hh — Issue #2613
// Unified Agent surface: commit_readiness × coercion SLO × linear force ×
// occurrence/memo stale. Pure aggregation of existing atomics / helpers —
// no extra solve, no commit barrier policy change (AC4).
//
// Folded keys (query:type-linear-commit-health):
//   readiness_bp / force_reason / would_allow_commit   (#2553)
//   coercion_completeness_bp / coercion_slo_force_pending (#2558)
//   coercion_evidence_loss_bp / evidence-loss force (#2648)
//   linear_force_unified / cross-closure observe|force totals (#2545/#2563)
//   occurrence_stale / predicate_memo_stale (#2359)
//
// force_reason authority:
//   1) commit_readiness when not "ok" (solve/blame/linear/truncate/empty_cs/…)
//   2) else if coercion_slo_force_pending → "coercion-slo" (advisory; allow=true)
//   3) else if coercion_evidence_loss_pressure → "coercion-evidence-loss" (advisory)
//   4) else if occurrence_stale || predicate_memo_stale → "occurrence-stale"
//   5) else "ok"
// Codes: 0=ok 1=solve 2=blame 3=linear 4=truncate 5=empty_cs 6=auto_partial
//        7=coercion-slo 8=occurrence-stale 9=coercion-evidence-loss
//
// Optional throttle advisory (never hard-fails mutate):
//   0=none 1=delay-mutate 2=split-batch
//   would_allow false → delay; coercion-slo / evidence-loss / occurrence-stale → delay;
//   empty_cs hard deny → split-batch

#ifndef AURA_COMPILER_TYPE_LINEAR_COMMIT_HEALTH_HH
#define AURA_COMPILER_TYPE_LINEAR_COMMIT_HEALTH_HH

#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <string_view>

namespace aura::compiler {

inline constexpr int kTypeLinearCommitHealthIssue = 2613;

struct TypeLinearCommitHealthSnapshot {
    // #2553 commit_readiness inputs (caller fills; live path uses live_policy + face).
    typed_audit::CommitReadinessInput readiness_in{};
    // #2558 coercion
    std::uint64_t coercion_completeness_bp = 10000;
    bool coercion_slo_force_pending = false;
    // #2648 Soft evidence-loss bp (skip / skip+good); pressure is advisory only.
    std::uint64_t coercion_evidence_loss_bp = 0;
    bool coercion_evidence_loss_pressure = false;
    // #2545 / #2563 linear force counters (process totals; pure snapshot)
    bool linear_force_unified = true;
    std::uint64_t linear_cross_closure_escape_total = 0;
    std::uint64_t linear_cross_closure_force_total = 0;
    std::uint64_t linear_cross_closure_observe_total = 0;
    // #2359 occurrence / predicate memo
    std::uint64_t occurrence_stale = 0;
    std::uint64_t predicate_memo_stale = 0;
};

struct TypeLinearCommitHealthResult {
    std::uint32_t readiness_bp = 10000;
    bool would_allow_commit = true;
    std::string_view force_reason = "ok";
    std::int64_t force_reason_code = 0;
    // Folded flags (mirrors snapshot components for query)
    std::uint64_t coercion_completeness_bp = 10000;
    bool coercion_slo_force_pending = false;
    std::uint64_t coercion_evidence_loss_bp = 0;
    bool coercion_evidence_loss_pressure = false;
    bool linear_force_unified = true;
    std::uint64_t linear_cross_closure_escape_total = 0;
    std::uint64_t linear_cross_closure_force_total = 0;
    std::uint64_t linear_cross_closure_observe_total = 0;
    std::uint64_t occurrence_stale = 0;
    std::uint64_t predicate_memo_stale = 0;
    // Advisory throttle only (AC optional; never commit barrier).
    // 0=none 1=delay-mutate 2=split-batch
    std::int64_t throttle_action = 0;
    TypeLinearCommitHealthSnapshot components{};
};

// Extended reason codes for #2613 / #2648 advisory axes (commit_readiness uses 0–6).
[[nodiscard]] inline std::int64_t
type_linear_commit_health_reason_code(std::string_view r) noexcept {
    if (r == "coercion-slo")
        return 7;
    if (r == "occurrence-stale")
        return 8;
    if (r == "coercion-evidence-loss")
        return 9;
    return typed_audit::commit_readiness_reason_code(r);
}

// Purpose: pure fold of readiness × coercion × linear × occurrence
// Pre: snapshot fields populated from atomics / CommitReadinessInput
// Post: identical inputs → identical output; no atomics written
// Safety Class: P2 (observability; no throw)
// Issue: #2613
// AI-Native Rationale: single Agent throttle surface without multi-schema join
[[nodiscard]] inline TypeLinearCommitHealthResult
compute_type_linear_commit_health(const TypeLinearCommitHealthSnapshot& s) noexcept {
    TypeLinearCommitHealthResult r;
    r.components = s;
    r.coercion_completeness_bp = s.coercion_completeness_bp;
    r.coercion_slo_force_pending = s.coercion_slo_force_pending;
    r.coercion_evidence_loss_bp = s.coercion_evidence_loss_bp;
    r.coercion_evidence_loss_pressure = s.coercion_evidence_loss_pressure;
    r.linear_force_unified = s.linear_force_unified;
    r.linear_cross_closure_escape_total = s.linear_cross_closure_escape_total;
    r.linear_cross_closure_force_total = s.linear_cross_closure_force_total;
    r.linear_cross_closure_observe_total = s.linear_cross_closure_observe_total;
    r.occurrence_stale = s.occurrence_stale;
    r.predicate_memo_stale = s.predicate_memo_stale;

    const auto cr = typed_audit::commit_readiness(s.readiness_in);
    r.readiness_bp = cr.readiness_bp;
    r.would_allow_commit = cr.would_allow_commit;
    r.force_reason = cr.force_reason;
    r.force_reason_code = cr.force_reason_code;

    // Overlay advisory reasons only when commit_readiness face is "ok".
    // Policy for deny remains in commit_readiness / existing hard gates (AC4).
    if (cr.force_reason == "ok") {
        if (s.coercion_slo_force_pending) {
            r.force_reason = "coercion-slo";
            r.force_reason_code = 7;
            // would_allow_commit stays true — SLO forces Full audit, not commit deny.
        } else if (s.coercion_evidence_loss_pressure) {
            // Issue #2648: Soft evidence-loss pressure without pending yet
            // (Agent throttle face; still advisory allow=true).
            r.force_reason = "coercion-evidence-loss";
            r.force_reason_code = 9;
        } else if (s.occurrence_stale > 0 || s.predicate_memo_stale > 0) {
            r.force_reason = "occurrence-stale";
            r.force_reason_code = 8;
        }
    }

    // Optional throttle advisory table (orch delay/split; never hard-fail).
    if (!r.would_allow_commit) {
        // empty_cs / auto_partial hard often wants split-batch; others delay.
        if (r.force_reason == "empty_cs" || r.force_reason == "auto_partial")
            r.throttle_action = 2; // split-batch
        else
            r.throttle_action = 1; // delay-mutate
    } else if (r.force_reason == "coercion-slo" || r.force_reason == "coercion-evidence-loss" ||
               r.force_reason == "occurrence-stale" || r.force_reason == "truncate" ||
               r.force_reason == "linear" || r.force_reason == "blame") {
        r.throttle_action = 1; // Soft observe still advises delay
    } else {
        r.throttle_action = 0;
    }
    return r;
}

// Live-policy snapshot defaults: clean SOLVED face + live hard flags + current
// process atomics for coercion/linear. Occurrence/memo filled by query path.
[[nodiscard]] inline TypeLinearCommitHealthSnapshot type_linear_commit_health_live_base() noexcept {
    TypeLinearCommitHealthSnapshot s;
    s.readiness_in = typed_audit::commit_readiness_live_policy();
    // Clean face defaults (vacuous healthy when no pending commit — AC3).
    // Caller may overwrite readiness_in.linear_ok / blame_ok / etc.
    s.linear_force_unified =
        typed_audit::g_typed_mutation_audit_counters.linear_force_unified_2545.load(
            std::memory_order_relaxed) != 0;
    s.linear_cross_closure_escape_total =
        typed_audit::g_typed_mutation_audit_counters.linear_cross_closure_escape_total.load(
            std::memory_order_relaxed);
    s.linear_cross_closure_force_total =
        typed_audit::g_typed_mutation_audit_counters.linear_cross_closure_force_total.load(
            std::memory_order_relaxed);
    s.linear_cross_closure_observe_total =
        typed_audit::g_typed_mutation_audit_counters.linear_cross_closure_observe_total.load(
            std::memory_order_relaxed);
    return s;
}

} // namespace aura::compiler

#endif // AURA_COMPILER_TYPE_LINEAR_COMMIT_HEALTH_HH
