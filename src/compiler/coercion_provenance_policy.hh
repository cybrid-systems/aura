// coercion_provenance_policy.hh — Issue #2102 / #2185 / #2221 / #2558
//
// Process-wide provenance-miss policy for CoercionMap apply + optional
// hard blame-complete gate on composite_txn_commit + completeness SLO
// (#2558) that arms force Full audit on the next outermost
// MutationBoundary when completeness_bp falls below the SLO under
// production. Soft / non-production: observe-only.
// Header form so security_defaults.hh (and main) can flip reject-on-miss
// under production defaults without importing the coercion_map module.
//
// Defaults (process start / reset_for_test):
//   force_audit_on_provenance_miss = true
//   reject_apply_on_provenance_miss = false  (#2102 ergonomic soft apply)
//   require_blame_complete_on_commit = false (#2221 observe-only)
//   SLO force pending = false (#2558); SLO bp default 9500
//
// Issue #2185 production path (apply_production_security_defaults):
//   reject_apply_on_provenance_miss = true under Restricted/Strict
//   reject_apply_on_provenance_miss = false when AURA_SANDBOX=off (dev)
//   AURA_COERCION_PROVENANCE_REJECT=reject|soft|1|0 overrides when set
//
// Issue #2221 production path:
//   require_blame_complete_on_commit = true under Restricted/Strict
//   require_blame_complete_on_commit = false when AURA_SANDBOX=off (dev)
//   AURA_BLAME_COMMIT_REQUIRE=on|off|1|0 overrides when set
//
// Issue #2558: #2512 stamp-at-add is the preferred completeness path;
// the SLO is a backstop for long Sampled production sessions.

#ifndef AURA_COMPILER_COERCION_PROVENANCE_POLICY_HH
#define AURA_COMPILER_COERCION_PROVENANCE_POLICY_HH

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace aura::compiler {

// force_audit: note miss for MutationBoundary Full-path audit (default on).
// reject_apply: skip CoercionNode insert on incomplete chain (production on).
inline std::atomic<std::uint32_t> g_force_audit_on_provenance_miss{1};
inline std::atomic<std::uint32_t> g_reject_apply_on_provenance_miss{0};
inline std::atomic<std::uint64_t> g_coercion_provenance_miss_force_audit_total{0};
inline std::atomic<std::uint64_t> g_coercion_provenance_miss_reject_total{0};

// Issue #2221: composite commit hard-require complete DeltaBlameChain.
// Default off (observe-only); production defaults flip on.
inline std::atomic<std::uint32_t> g_require_blame_complete_on_commit{0};
inline std::atomic<std::uint64_t> g_blame_commit_reject_total{0};
inline std::atomic<std::uint64_t> g_blame_commit_incomplete_observe_total{0};
inline std::atomic<std::uint64_t> g_blame_commit_check_total{0};

// Issue #2185 stamp for query surfaces / Agent discovery.
inline constexpr int kCoercionProvenanceRejectProductionIssue = 2185;
// Issue #2221 stamp for query surfaces / Agent discovery.
inline constexpr int kBlameCommitRequireIssue = 2221;
// Issue #2558 stamp for query surfaces / Agent discovery.
inline constexpr int kCoercionProvSloIssue = 2558;
// Issue #2561 stamp for Soft blame recovery / escalate.
inline constexpr int kBlameSoftRecoverIssue = 2561;
// Issue #2562 stamp for dual-field (pred+mid) require-or-drop.
inline constexpr int kCoercionDualRequireIssue = 2562;
// Issue #2648 stamp for Soft evidence-loss bp + one-shot Full arm.
inline constexpr int kCoercionEvidenceLossIssue = 2648;
// Completeness SLO in basis points (0–10000). Default 9500 = 95%.
// Override via AURA_COERCION_PROV_SLO_BP when set (numeric).
inline constexpr std::uint64_t kCoercionProvSloBpDefault = 9500;
// Issue #2648: max acceptable Soft evidence-loss bp (skip / skip+good).
// Default 500 = 5% loss, aligned with #2558 95% completeness family
// (10000 - 9500). Override via AURA_COERCION_EVIDENCE_LOSS_BP.
inline constexpr std::uint64_t kCoercionEvidenceLossBpDefault = 500;

// Issue #2558: process-wide completeness SLO observability + force flag.
inline std::atomic<std::uint64_t> g_coercion_prov_slo_breach_total{0};
// Observe-only breaches under Soft / non-production (no force).
inline std::atomic<std::uint64_t> g_coercion_prov_slo_observe_only_total{0};
// Times production path armed force-full for next boundary.
inline std::atomic<std::uint64_t> g_coercion_prov_slo_force_armed_total{0};
// Times boundary exit consumed SLO force-full pending.
inline std::atomic<std::uint64_t> g_coercion_prov_slo_force_consumed_total{0};
// 1 = next outermost MutationBoundary exit must take Full/contextual audit.
inline std::atomic<std::uint32_t> g_coercion_prov_slo_force_full_pending{0};
// Cached SLO budget (bp); env override applied lazily once.
inline std::atomic<std::uint64_t> g_coercion_prov_slo_bp{kCoercionProvSloBpDefault};
inline std::atomic<std::uint32_t> g_coercion_prov_slo_bp_resolved{0};

// Issue #2648: Soft incomplete-skip evidence-loss SLO (process-wide).
// loss_bp = soft_skip / (soft_skip + complete + ast_elided) as bp.
// When loss_bp >= threshold under Soft/Sampled, arm same force-Full pending
// channel as #2558 / #2620; boundary consumes once (no permanent Full).
// Max acceptable loss bp (default 500); env AURA_COERCION_EVIDENCE_LOSS_BP.
inline std::atomic<std::uint64_t> g_coercion_evidence_loss_threshold_bp{
    kCoercionEvidenceLossBpDefault};
inline std::atomic<std::uint32_t> g_coercion_evidence_loss_bp_resolved{0};
// Times loss_bp crossed threshold (evaluate path).
inline std::atomic<std::uint64_t> g_coercion_evidence_loss_breach_total{0};
// Times evidence-loss path armed force-Full (first transition only).
inline std::atomic<std::uint64_t> g_coercion_evidence_loss_force_armed_total{0};
// Times boundary exit consumed force-Full under evidence-loss pressure.
inline std::atomic<std::uint64_t> g_coercion_evidence_loss_force_consumed_total{0};
inline std::atomic<std::uint32_t> g_coercion_evidence_loss_wired{1};

// Issue #2561: Soft/Sampled blame-chain recovery + one-shot Full sample escalate.
// Recover: cheap re-fill of dual provenance fields for mid's dirty cone.
// Escalate: force one Full/contextual audit sample for that mid (not global
// strategy flip; not #2221 hard reject). Soft default is observe-only (AC3);
// AURA_BLAME_SOFT_ESCALATE=1 or production_defaults arms escalate-on-fail.
inline std::atomic<std::uint64_t> g_blame_soft_recover_total{0};
inline std::atomic<std::uint64_t> g_blame_soft_recover_fail_total{0};
inline std::atomic<std::uint64_t> g_blame_soft_escalate_total{0};
// 1 when Soft recovery failed and one-shot Full audit is armed for this exit.
inline thread_local bool s_blame_soft_escalate_this_boundary = false;

// Issue #2562: dual-field (predicate_cond_node + source_mutation_id) require-
// or-drop. When enabled, incomplete dual after fill → skip CoercionNode insert
// (prefer drop over weak/sentinel stamp). Soft/Sampled default off so #2317
// insert-with-force-audit remains; production / process flag / env=1 enable.
// Full strategy ORed at call site (coercion_map) to avoid typed_audit include.
inline std::atomic<std::uint32_t> g_coercion_dual_require{0};
inline std::atomic<std::uint64_t> g_coercion_dual_require_drop_total{0};
inline std::atomic<std::uint32_t> g_coercion_dual_require_wired{1};

inline void set_force_audit_on_provenance_miss(bool on) noexcept {
    g_force_audit_on_provenance_miss.store(on ? 1u : 0u, std::memory_order_relaxed);
}
inline void set_reject_apply_on_provenance_miss(bool on) noexcept {
    g_reject_apply_on_provenance_miss.store(on ? 1u : 0u, std::memory_order_relaxed);
}
[[nodiscard]] inline bool force_audit_on_provenance_miss() noexcept {
    return g_force_audit_on_provenance_miss.load(std::memory_order_relaxed) != 0;
}
[[nodiscard]] inline bool reject_apply_on_provenance_miss() noexcept {
    return g_reject_apply_on_provenance_miss.load(std::memory_order_relaxed) != 0;
}

inline void set_require_blame_complete_on_commit(bool on) noexcept {
    g_require_blame_complete_on_commit.store(on ? 1u : 0u, std::memory_order_relaxed);
}
[[nodiscard]] inline bool require_blame_complete_on_commit() noexcept {
    return g_require_blame_complete_on_commit.load(std::memory_order_relaxed) != 0;
}

// Thread-local: miss recorded during this fiber's apply → consumed on
// MutationBoundary exit (process-wide counters stay global).
inline thread_local bool s_provenance_miss_this_boundary = false;

inline void note_provenance_miss_for_boundary() noexcept {
    s_provenance_miss_this_boundary = true;
}
[[nodiscard]] inline bool provenance_miss_pending_for_boundary() noexcept {
    return s_provenance_miss_this_boundary;
}
// Returns true if a miss was noted since last consume; clears the flag.
[[nodiscard]] inline bool consume_provenance_miss_for_boundary() noexcept {
    const bool v = s_provenance_miss_this_boundary;
    s_provenance_miss_this_boundary = false;
    return v;
}

// ── Issue #2558: completeness SLO ──────────────────────────────────────
[[nodiscard]] inline std::uint64_t coercion_prov_slo_bp() noexcept {
    if (g_coercion_prov_slo_bp_resolved.load(std::memory_order_relaxed) == 0) {
        const char* e = std::getenv("AURA_COERCION_PROV_SLO_BP");
        if (e && *e) {
            char* end = nullptr;
            const auto v = std::strtoull(e, &end, 10);
            if (end != e && v <= 10000u)
                g_coercion_prov_slo_bp.store(static_cast<std::uint64_t>(v),
                                             std::memory_order_relaxed);
        }
        g_coercion_prov_slo_bp_resolved.store(1, std::memory_order_relaxed);
    }
    return g_coercion_prov_slo_bp.load(std::memory_order_relaxed);
}

inline void set_coercion_prov_slo_bp_for_test(std::uint64_t bp) noexcept {
    if (bp > 10000u)
        bp = 10000u;
    g_coercion_prov_slo_bp.store(bp, std::memory_order_relaxed);
    g_coercion_prov_slo_bp_resolved.store(1, std::memory_order_relaxed);
}

// Evaluate completeness SLO after a complete/miss sample (or on boundary).
// production_active: true under production_defaults_active() (force path).
// Soft / non-production: observe-only (breach counter, no force).
// completeness_bp vacuously 10000 with no samples → no breach (AC3).
inline void evaluate_coercion_provenance_slo(std::uint64_t completeness_bp,
                                             bool production_active) noexcept {
    const auto slo = coercion_prov_slo_bp();
    if (completeness_bp >= slo)
        return;
    g_coercion_prov_slo_breach_total.fetch_add(1, std::memory_order_relaxed);
    if (!production_active) {
        g_coercion_prov_slo_observe_only_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Production: arm force Full for next outermost boundary exit.
    const auto prev = g_coercion_prov_slo_force_full_pending.exchange(1, std::memory_order_acq_rel);
    if (prev == 0)
        g_coercion_prov_slo_force_armed_total.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline bool coercion_prov_slo_force_full_pending() noexcept {
    return g_coercion_prov_slo_force_full_pending.load(std::memory_order_acquire) != 0;
}

// Returns true if SLO force was pending; clears the flag (one-shot Full).
[[nodiscard]] inline bool consume_coercion_prov_slo_force_full() noexcept {
    const auto prev = g_coercion_prov_slo_force_full_pending.exchange(0, std::memory_order_acq_rel);
    if (prev != 0)
        g_coercion_prov_slo_force_consumed_total.fetch_add(1, std::memory_order_relaxed);
    return prev != 0;
}

// Soft defaults (#2102 / #2185 AC3 / #2221 observe-only): force-audit on,
// reject off, commit require off. Also clears #2558 SLO pending/state,
// #2561 Soft escalate TLS, #2562 dual-require flag, and #2648 evidence-loss
// threshold (lifetime counters left for tests that zero them explicitly).
inline void reset_coercion_provenance_miss_policy_for_test() noexcept {
    g_force_audit_on_provenance_miss.store(1, std::memory_order_relaxed);
    g_reject_apply_on_provenance_miss.store(0, std::memory_order_relaxed);
    g_require_blame_complete_on_commit.store(0, std::memory_order_relaxed);
    s_provenance_miss_this_boundary = false;
    // Issue #2558: clear SLO force pending; leave lifetime counters
    // (tests that need zero counters should store 0 themselves).
    g_coercion_prov_slo_force_full_pending.store(0, std::memory_order_relaxed);
    g_coercion_prov_slo_bp.store(kCoercionProvSloBpDefault, std::memory_order_relaxed);
    g_coercion_prov_slo_bp_resolved.store(1, std::memory_order_relaxed);
    // Issue #2561
    s_blame_soft_escalate_this_boundary = false;
    // Issue #2562: Soft default dual-require off (#2317 insert path).
    g_coercion_dual_require.store(0, std::memory_order_relaxed);
    // Issue #2648: reset evidence-loss threshold to default (not lifetime counters).
    g_coercion_evidence_loss_threshold_bp.store(kCoercionEvidenceLossBpDefault,
                                                std::memory_order_relaxed);
    g_coercion_evidence_loss_bp_resolved.store(1, std::memory_order_relaxed);
}

// ── Issue #2648: Soft evidence-loss SLO ────────────────────────────────
// Max acceptable loss_bp (0–10000). Higher loss is worse.
// AURA_COERCION_EVIDENCE_LOSS_BP overrides when set (numeric ≤ 10000).
[[nodiscard]] inline std::uint64_t coercion_evidence_loss_threshold_bp() noexcept {
    if (g_coercion_evidence_loss_bp_resolved.load(std::memory_order_relaxed) == 0) {
        const char* e = std::getenv("AURA_COERCION_EVIDENCE_LOSS_BP");
        if (e && *e) {
            char* end = nullptr;
            const auto v = std::strtoull(e, &end, 10);
            if (end != e && v <= 10000u)
                g_coercion_evidence_loss_threshold_bp.store(static_cast<std::uint64_t>(v),
                                                            std::memory_order_relaxed);
        }
        g_coercion_evidence_loss_bp_resolved.store(1, std::memory_order_relaxed);
    }
    return g_coercion_evidence_loss_threshold_bp.load(std::memory_order_relaxed);
}

inline void set_coercion_evidence_loss_threshold_bp_for_test(std::uint64_t bp) noexcept {
    if (bp > 10000u)
        bp = 10000u;
    g_coercion_evidence_loss_threshold_bp.store(bp, std::memory_order_relaxed);
    g_coercion_evidence_loss_bp_resolved.store(1, std::memory_order_relaxed);
}

// Pure pressure check: loss_bp >= threshold (Agents + boundary Soft drop gate).
[[nodiscard]] inline bool coercion_evidence_loss_pressure(std::uint64_t loss_bp) noexcept {
    return loss_bp >= coercion_evidence_loss_threshold_bp();
}

// Evaluate Soft evidence-loss SLO. When loss_bp >= threshold, arm the shared
// force-Full pending channel (same as #2558 / #2620) once. Does not permanently
// flip strategy. Always bumps breach when pressure present.
// Pre: loss_bp from coercion_evidence_loss_bp() (module; counters in map).
// Post: pending may be 1; armed_total increments on first arm only.
inline void evaluate_coercion_evidence_loss_slo(std::uint64_t loss_bp) noexcept {
    if (!coercion_evidence_loss_pressure(loss_bp))
        return;
    g_coercion_evidence_loss_breach_total.fetch_add(1, std::memory_order_relaxed);
    const auto prev = g_coercion_prov_slo_force_full_pending.exchange(1, std::memory_order_acq_rel);
    if (prev == 0) {
        g_coercion_evidence_loss_force_armed_total.fetch_add(1, std::memory_order_relaxed);
        // Shared #2558 arm counter for Agents that already poll force-armed.
        g_coercion_prov_slo_force_armed_total.fetch_add(1, std::memory_order_relaxed);
    }
}

// Issue #2561: Soft observe-by-default (AC3). AURA_BLAME_SOFT_ESCALATE=1|on
// arms one-shot Full sample escalate on recover fail. Unset / 0 / off →
// recover-only observe. production_defaults also escalate (caller-side OR).
[[nodiscard]] inline bool blame_soft_escalate_enabled() noexcept {
    const char* e = std::getenv("AURA_BLAME_SOFT_ESCALATE");
    if (!e || !*e)
        return false; // Soft default: observe (recover only)
    if (e[0] == '0' || e[0] == 'f' || e[0] == 'F' || e[0] == 'n' || e[0] == 'N')
        return false;
    if ((e[0] == 'o' || e[0] == 'O') && e[1] != '\0' && (e[1] == 'f' || e[1] == 'F'))
        return false;
    return true; // 1 / on / yes / true
}

inline void note_blame_soft_escalate_for_boundary() noexcept {
    s_blame_soft_escalate_this_boundary = true;
}
[[nodiscard]] inline bool blame_soft_escalate_pending_for_boundary() noexcept {
    return s_blame_soft_escalate_this_boundary;
}
[[nodiscard]] inline bool consume_blame_soft_escalate_for_boundary() noexcept {
    const bool v = s_blame_soft_escalate_this_boundary;
    s_blame_soft_escalate_this_boundary = false;
    return v;
}

// Issue #2562: process flag for dual-field require-or-drop (also env/Full).
inline void set_coercion_dual_require(bool on) noexcept {
    g_coercion_dual_require.store(on ? 1u : 0u, std::memory_order_relaxed);
}
[[nodiscard]] inline bool coercion_dual_require_flag() noexcept {
    return g_coercion_dual_require.load(std::memory_order_relaxed) != 0;
}

// Issue #2562: AURA_COERCION_DUAL_REQUIRE=1|on → force dual-require on;
// 0|off|false|soft → force off (Soft canary). Unset → process flag only here;
// call sites OR production_defaults + Full strategy.
// Returns: -1 unset, 0 forced off, 1 forced on.
[[nodiscard]] inline int coercion_dual_require_env() noexcept {
    const char* e = std::getenv("AURA_COERCION_DUAL_REQUIRE");
    if (!e || !*e)
        return -1;
    if (e[0] == '0' || e[0] == 'f' || e[0] == 'F' || e[0] == 'n' || e[0] == 'N')
        return 0;
    if ((e[0] == 'o' || e[0] == 'O') && e[1] != '\0' && (e[1] == 'f' || e[1] == 'F'))
        return 0;
    if (e[0] == 's' || e[0] == 'S') // soft
        return 0;
    return 1; // 1 / on / yes / true / require
}

// Policy-level dual-require (env + process flag). Call sites may OR Full /
// production_defaults_active for Goal-1 production/Full enablement.
[[nodiscard]] inline bool coercion_dual_require_enabled() noexcept {
    const int env = coercion_dual_require_env();
    if (env == 0)
        return false;
    if (env == 1)
        return true;
    return coercion_dual_require_flag();
}

// Issue #2185: production defaults force reject-on-miss (forensic refuse).
// Dev path (sandbox off) keeps soft apply for iterative typecheck.
// Issue #2221: also require complete blame on composite commit under
// production (observe-only when sandbox=off).
// Issue #2562: dual-field require-or-drop under production.
inline void apply_production_coercion_provenance_defaults(bool dev_sandbox_off) noexcept {
    set_force_audit_on_provenance_miss(true); // defense in depth (#2102)
    if (dev_sandbox_off) {
        set_reject_apply_on_provenance_miss(false);
        set_require_blame_complete_on_commit(false);
        set_coercion_dual_require(false);
    } else {
        set_reject_apply_on_provenance_miss(true);
        set_require_blame_complete_on_commit(true);
        set_coercion_dual_require(true);
    }
}

// Soft / reject canary override: returns true if env set a value.
// AURA_COERCION_PROVENANCE_REJECT=reject|1|true|on → reject
// AURA_COERCION_PROVENANCE_REJECT=soft|0|false|off → soft apply
inline bool apply_coercion_provenance_reject_env_override() noexcept {
    const char* e = std::getenv("AURA_COERCION_PROVENANCE_REJECT");
    if (!e || !*e)
        return false;
    // Minimal parse without string_view include for header lightness.
    if (e[0] == 'r' || e[0] == 'R' || e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' ||
        e[0] == 'Y' || (e[0] == 'o' && e[1] == 'n')) {
        set_reject_apply_on_provenance_miss(true);
        return true;
    }
    if (e[0] == 's' || e[0] == 'S' || e[0] == '0' || e[0] == 'f' || e[0] == 'F' || e[0] == 'n' ||
        e[0] == 'N' || (e[0] == 'o' && e[1] == 'f')) {
        set_reject_apply_on_provenance_miss(false);
        return true;
    }
    return false;
}

// Issue #2221: AURA_BLAME_COMMIT_REQUIRE=on|1|true|require → hard require
// AURA_BLAME_COMMIT_REQUIRE=off|0|false|soft|observe → observe-only
// Returns true if env applied a value.
inline bool apply_blame_commit_require_env_override() noexcept {
    const char* e = std::getenv("AURA_BLAME_COMMIT_REQUIRE");
    if (!e || !*e)
        return false;
    if (e[0] == 'o' && e[1] == 'n') {
        set_require_blame_complete_on_commit(true);
        return true;
    }
    if (e[0] == 'r' || e[0] == 'R' || e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' ||
        e[0] == 'Y') {
        set_require_blame_complete_on_commit(true);
        return true;
    }
    if (e[0] == 'o' && e[1] == 'f') {
        set_require_blame_complete_on_commit(false);
        return true;
    }
    if (e[0] == 's' || e[0] == 'S' || e[0] == '0' || e[0] == 'f' || e[0] == 'F' || e[0] == 'n' ||
        e[0] == 'N') {
        set_require_blame_complete_on_commit(false);
        return true;
    }
    return false;
}

} // namespace aura::compiler

#endif // AURA_COMPILER_COERCION_PROVENANCE_POLICY_HH
