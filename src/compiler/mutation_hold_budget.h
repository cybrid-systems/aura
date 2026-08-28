// mutation_hold_budget.h — Issue #2313: hold-budget threshold accessor.
// Shared header so MutationBoundaryGuard dtor + query:mutation-boundary-hold-stats
// share one env-cached source of truth (AURA_MUTATION_HOLD_BUDGET_US).
// Issue #2517: process-wide live longest outermost hold probe (fiber_id +
// start_ns) for Agent self-degrade during long mutate.

#ifndef AURA_COMPILER_MUTATION_HOLD_BUDGET_H
#define AURA_COMPILER_MUTATION_HOLD_BUDGET_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "typed_mutation_audit.h" // production_defaults_active (#2701 AC1)
#include "serve/fiber.h" // Issue #2853: is_hold_slo_soft_for_test + production_residual_policy_locked

namespace aura::compiler {

// Default 100_000 µs (100 ms). Lazy-init from AURA_MUTATION_HOLD_BUDGET_US.
// C-style digit parse (no exceptions). Cached once per process.
[[nodiscard]] inline std::uint64_t mutation_hold_budget_us() noexcept {
    static const std::uint64_t cached = []() noexcept -> std::uint64_t {
        const char* e = std::getenv("AURA_MUTATION_HOLD_BUDGET_US");
        if (e == nullptr || e[0] == '\0')
            return 100000ULL;
        std::uint64_t v = 0;
        for (const char* p = e; *p >= '0' && *p <= '9'; ++p) {
            v = v * 10 + static_cast<std::uint64_t>(*p - '0');
        }
        return v > 0 ? v : 100000ULL;
    }();
    return cached;
}

// Issue #2349: production hold SLO circuit-breaker (default force-fail path).
// Distinct from #2313 signal-only budget and #2199 opt-in STRICT hard-timeout.
//
// ── Decision table (Soft / Production / Disabled) ──
// | Mode       | Env select                                   | hold_us > SLO action              |
// | Soft       | AURA_SANDBOX=off OR AURA_MUTATION_HOLD_SLO_SOFT=1 | metric only (no force-fail) |
// | Production | default (sandbox not off)                    | success_flag=false + counter      |
// | Disabled   | AURA_MUTATION_HOLD_SLO_US=0                  | no check (AC4)                    |
// Happy path (hold ≤ SLO or disabled): one compare / getenv parse, zero force
// work beyond existing long-hold metrics (AC3).
//
// Default SLO 100_000 µs (100 ms). Live getenv (not process-static) so tests
// can set AURA_MUTATION_HOLD_SLO_US without process restart (same pattern as
// #2346 Soft/Hard).
[[nodiscard]] inline std::uint64_t mutation_hold_slo_us() noexcept {
    const char* e = std::getenv("AURA_MUTATION_HOLD_SLO_US");
    if (e == nullptr || e[0] == '\0')
        return 100000ULL; // production default 100ms
    // Explicit 0 disables the circuit (AC4).
    if (e[0] == '0' && e[1] == '\0')
        return 0;
    std::uint64_t v = 0;
    for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
        v = v * 10 + static_cast<std::uint64_t>(*p - '0');
    return v; // 0 if non-numeric → disable
}

// Soft / sandbox: metric-only SLO violation (AC2). Production (default): force.
//
// Issue #2853: production-residual-policy lock — when
// production_defaults_active() + sandbox != off, AURA_MUTATION_HOLD_SLO_SOFT=1
// is IGNORED (Soft env cannot ship silently in production). Soft path
// requires sandbox=off OR explicit set_hold_slo_soft_for_test(true) override.
// Mirrors the is_steal_snapshot_soft_mode production-lock pattern (#2372).
// Happy path cost: relaxed atomic load of test override + env var load only
// when not under production lock — zero work in the production default.
[[nodiscard]] inline bool mutation_hold_slo_soft_mode() noexcept {
    // AC2: test override wins (unit Soft-path ergonomics).
    if (aura::serve::is_hold_slo_soft_for_test())
        return true;
    // AC2: AURA_SANDBOX=off always Soft (matches #2546/#2667/#2756/#2852 lineage).
    const char* sandbox = std::getenv("AURA_SANDBOX");
    if (sandbox && sandbox[0] != '\0' && std::string_view(sandbox) == "off")
        return true;
    // AC1/AC3: production lock — AURA_MUTATION_HOLD_SLO_SOFT=1 is IGNORED.
    if (aura::compiler::typed_audit::production_defaults_active())
        return false;
    // Legacy: AURA_MUTATION_HOLD_SLO_SOFT=1 → Soft (only honored outside production lock).
    const char* soft = std::getenv("AURA_MUTATION_HOLD_SLO_SOFT");
    return soft && soft[0] == '1';
}

// ── Issue #2517: process-wide live longest outermost hold probe ──
// Best-effort CAS (AC5): under contention may lag one sample; Agents treat
// as soft real-time signal (not a hard mutex owner lock).
//
// Enter: claim empty slot, or replace if our start_ns is earlier (longer hold).
// Exit: if this fiber is the recorded max holder → clear (simplified; next
// enter rebuilds). Nested guards never touch the probe.

inline std::atomic<std::uint64_t> g_mutation_hold_live_fiber_id{0};
inline std::atomic<std::uint64_t> g_mutation_hold_live_start_ns{0};
inline std::atomic<std::uint32_t> g_mutation_hold_live_depth{0};
// Issue #3048: hold-snapshot session mid so force-degrade can revoke
// session grants if the holder fiber is already gone from the registry.
inline std::atomic<std::uint64_t> g_mutation_hold_live_session_mid{0};
inline std::atomic<std::uint64_t> g_mutation_hold_live_update_total{0};
inline std::atomic<std::uint64_t> g_mutation_hold_live_clear_total{0};
inline std::atomic<std::uint64_t> g_mutation_hold_live_over_budget_observe_total{0};
inline std::atomic<std::uint32_t> g_mutation_hold_live_wired{1};

[[nodiscard]] inline std::uint64_t mutation_hold_steady_ns_now() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

[[nodiscard]] inline std::uint64_t
mutation_hold_steady_ns_of(std::chrono::steady_clock::time_point tp) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count());
}

// Outermost Guard enter: install or upgrade process-wide max-hold probe.
inline void mutation_hold_live_note_enter(std::uint64_t fiber_id, std::uint64_t start_ns,
                                          std::uint32_t depth) noexcept {
    if (fiber_id == 0)
        fiber_id = 1; // never store 0 as live id (0 = no holder)
    // Claim empty slot (fiber_id == 0).
    std::uint64_t expected_fid = 0;
    if (g_mutation_hold_live_fiber_id.compare_exchange_strong(
            expected_fid, fiber_id, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        g_mutation_hold_live_start_ns.store(start_ns, std::memory_order_release);
        g_mutation_hold_live_depth.store(depth, std::memory_order_relaxed);
        g_mutation_hold_live_update_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Occupied: replace only if our hold is longer (earlier start_ns).
    auto cur_start = g_mutation_hold_live_start_ns.load(std::memory_order_acquire);
    if (cur_start != 0 && start_ns < cur_start) {
        if (g_mutation_hold_live_start_ns.compare_exchange_strong(
                cur_start, start_ns, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            g_mutation_hold_live_fiber_id.store(fiber_id, std::memory_order_release);
            g_mutation_hold_live_depth.store(depth, std::memory_order_relaxed);
            g_mutation_hold_live_update_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// Outermost Guard exit: clear if we are the recorded max holder.
inline void mutation_hold_live_note_exit(std::uint64_t fiber_id) noexcept {
    if (fiber_id == 0)
        fiber_id = 1;
    std::uint64_t expected = fiber_id;
    if (g_mutation_hold_live_fiber_id.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        g_mutation_hold_live_start_ns.store(0, std::memory_order_release);
        g_mutation_hold_live_depth.store(0, std::memory_order_relaxed);
        g_mutation_hold_live_session_mid.store(0, std::memory_order_release);
        g_mutation_hold_live_clear_total.fetch_add(1, std::memory_order_relaxed);
    }
}

struct MutationHoldLiveSnapshot {
    std::uint64_t fiber_id = 0;
    std::uint64_t start_ns = 0;
    std::uint64_t duration_us = 0;
    std::uint32_t depth = 0;
    std::uint64_t session_mid = 0; // #3048
    bool held = false;
};

// Pure read (+ optional over-budget observe counter). No holder → zeros (AC3).
[[nodiscard]] inline MutationHoldLiveSnapshot mutation_hold_live_snapshot() noexcept {
    MutationHoldLiveSnapshot s;
    s.fiber_id = g_mutation_hold_live_fiber_id.load(std::memory_order_acquire);
    s.start_ns = g_mutation_hold_live_start_ns.load(std::memory_order_acquire);
    s.depth = g_mutation_hold_live_depth.load(std::memory_order_relaxed);
    s.session_mid = g_mutation_hold_live_session_mid.load(std::memory_order_acquire);
    if (s.fiber_id != 0 && s.start_ns != 0) {
        s.held = true;
        const auto now = mutation_hold_steady_ns_now();
        if (now > s.start_ns)
            s.duration_us = (now - s.start_ns) / 1000ULL;
        // Optional agent-visible budget observe (does not force-fail).
        if (s.duration_us > mutation_hold_budget_us())
            g_mutation_hold_live_over_budget_observe_total.fetch_add(1, std::memory_order_relaxed);
    }
    return s;
}

// Test / process-reset seam (does not touch hold-estimate ring).
inline void mutation_hold_live_reset_for_test() noexcept {
    g_mutation_hold_live_fiber_id.store(0, std::memory_order_relaxed);
    g_mutation_hold_live_start_ns.store(0, std::memory_order_relaxed);
    g_mutation_hold_live_depth.store(0, std::memory_order_relaxed);
    g_mutation_hold_live_session_mid.store(0, std::memory_order_relaxed);
}

// Issue #2701: Mutation hold-budget timeout → force degrade / reject new
// mutate admit. Closes the loop: under sustained AI agent load a single
// long-running mutate can starve work-stealing and GC, yet new
// MutationBoundaryGuard::try_acquire still succeeds. When live longest
// outermost hold exceeds configured budget (AURA_MUTATION_HOLD_BUDGET_US
// / default 100_000 µs), production path rejects new mutate admit with
// structured AdmissionRejected (reason "mutation-hold-budget"). Soft /
// sandbox / test override: metric-only (counter bump, no reject) unless
// explicit hard env. Interacts with #2660 security-schedule gate (both
// observable; order is budget AFTER schedule — locked in source).
inline std::atomic<std::uint64_t> g_mutation_hold_budget_reject_total{0};
inline std::atomic<std::uint64_t> g_mutation_hold_budget_soft_observe_total{0};
inline std::atomic<std::uint32_t> g_mutation_hold_budget_wired{1};
inline constexpr int kMutationHoldBudgetRejectIssue = 2701;

[[nodiscard]] inline std::uint64_t mutation_hold_budget_reject_total_v_read() noexcept {
    return g_mutation_hold_budget_reject_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t mutation_hold_budget_soft_observe_total_v_read() noexcept {
    return g_mutation_hold_budget_soft_observe_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t mutation_hold_budget_wired_v_read() noexcept {
    return g_mutation_hold_budget_wired.load(std::memory_order_relaxed);
}

// Soft / hard env override (default 0 = metric-only under Soft).
[[nodiscard]] inline int mutation_hold_budget_hard_env() noexcept {
    static const bool cached = []() noexcept -> bool {
        const char* e = std::getenv("AURA_MUTATION_HOLD_BUDGET_HARD");
        if (e == nullptr || e[0] == '\0')
            return false;
        return e[0] == '1';
    }();
    return cached ? 1 : 0;
}

// AC1 — true when try_acquire should reject (production strict OR
// hard env). AC2 — false under Soft / sandbox / test override (metric
// only unless explicit hard env).
[[nodiscard]] inline bool mutation_hold_budget_reject_enabled() noexcept {
    return mutation_hold_budget_hard_env() ||
           aura::compiler::typed_audit::production_defaults_active();
}

// AC1 — consult live longest hold vs budget; bumps the appropriate
// counter and returns the duration_us. Caller decides reject policy.
struct MutationHoldBudgetCheck {
    bool over_budget = false;
    std::uint64_t duration_us = 0;
};

[[nodiscard]] inline MutationHoldBudgetCheck mutation_hold_budget_check() noexcept {
    MutationHoldBudgetCheck r;
    const auto snap = mutation_hold_live_snapshot();
    r.duration_us = snap.duration_us;
    r.over_budget = (snap.held && snap.duration_us > mutation_hold_budget_us());
    if (r.over_budget) {
        if (mutation_hold_budget_reject_enabled())
            g_mutation_hold_budget_reject_total.fetch_add(1, std::memory_order_relaxed);
        else
            g_mutation_hold_budget_soft_observe_total.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

// Test reset.
inline void clear_mutation_hold_budget_reject_for_test() noexcept {
    g_mutation_hold_budget_reject_total.store(0, std::memory_order_relaxed);
    g_mutation_hold_budget_soft_observe_total.store(0, std::memory_order_relaxed);
}

// Issue #2720: P0 holder-degrade path (#2701 residual). #2701 only
// rejected new admits when the live longest outermost hold exceeded
// budget — the holder itself kept owning workspace_mtx_ exclusive +
// GcDeferReason::MutationHold, starving work-stealing and GC while
// only future mutates were refused. #2720 force-degrades the recorded
// holder fiber when production (or AURA_MUTATION_HOLD_BUDGET_HARD=1)
// and live hold > budget: bump holder-degrade counter + invoke the
// cooperative cancel ABI on the holder fiber. Soft / sandbox=off:
// counter-only (same as #2701) unless hard env set. Zero cost on
// happy path (hold ≤ budget): try_acquire already paid one snapshot
// compare via #2701's probe — #2720 only fires when that compare
// flipped over_budget.
//
// Cross-fiber cancel note: g_current_fiber is thread_local; the
// current implementation degrades the holder when admitter and
// holder are the same fiber (g_current_fiber->id() == snap.fiber_id).
// True cross-fiber cancel (different worker thread) needs a per-fiber
// pending-cancel map polled at safepoints — follow-up. The surface
// (counter + ABI + query keys + test) ships in #2720.
inline std::atomic<std::uint64_t> g_mutation_hold_budget_holder_degrade_total{0};
inline std::atomic<std::uint64_t> g_mutation_hold_budget_holder_degrade_same_fiber_total{0};
inline std::atomic<std::uint64_t> g_mutation_hold_budget_holder_degrade_cross_fiber_total{0};
inline std::atomic<std::uint32_t> g_mutation_hold_budget_holder_degrade_wired{1};
inline constexpr int kMutationHoldBudgetHolderDegradeIssue = 2720;

[[nodiscard]] inline std::uint64_t mutation_hold_budget_holder_degrade_total_v_read() noexcept {
    return g_mutation_hold_budget_holder_degrade_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t
mutation_hold_budget_holder_degrade_same_fiber_total_v_read() noexcept {
    return g_mutation_hold_budget_holder_degrade_same_fiber_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t
mutation_hold_budget_holder_degrade_cross_fiber_total_v_read() noexcept {
    return g_mutation_hold_budget_holder_degrade_cross_fiber_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t mutation_hold_budget_holder_degrade_wired_v_read() noexcept {
    return g_mutation_hold_budget_holder_degrade_wired.load(std::memory_order_relaxed);
}

inline void clear_mutation_hold_budget_holder_degrade_for_test() noexcept {
    g_mutation_hold_budget_holder_degrade_total.store(0, std::memory_order_relaxed);
    g_mutation_hold_budget_holder_degrade_same_fiber_total.store(0, std::memory_order_relaxed);
    g_mutation_hold_budget_holder_degrade_cross_fiber_total.store(0, std::memory_order_relaxed);
}

// Issue #2726: P0 cross-fiber real cancel (#2720 residual). #2720 wired
// same-fiber cancel + cross-fiber counter bump; the cross-fiber path
// still left the recorded holder starving until it voluntarily yielded.
// #2726 closes the loop with a per-Fiber pending-cancel flag
// (Fiber::request_hold_budget_cancel / consume_hold_budget_cancel —
// see src/serve/fiber.h) + a process-wide Fiber* registry (see
// src/serve/fiber.cpp, aura_fiber_request_hold_budget_cancel C-linkage
// shim). Cross-fiber force-degrade sets the flag on the holder via
// registry lookup; holder observes at outermost MutationBoundaryGuard
// Phase-5 exit (and at safepoints) and exits the Guard with failure,
// releasing the workspace_mtx_ exclusive + GcDeferReason::MutationHold.
//
// Observability counters (additive — #2720 counters preserved):
//   g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_fired_total
//     — bumped in aura_evaluator_force_degrade_outermost_holder when
//       the cross-fiber path successfully sets the pending-cancel
//       flag on a live holder (registry lookup returned non-null).
//   g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_consumed_total
//     — bumped at outermost Guard Phase-5 exit when the holder
//       consumes the flag (one-shot CAS true→false). Mirrors the
//       fired total in steady state; divergence = "holder is gone
//       or flag is lost under crash" (observable for Agent health).
//
// Production / hard-env gating: same #2701/#2720 reject_enabled gate
// (mutation_hold_budget_reject_enabled). Soft / sandbox=off → counter-
// only (no flag set). Nested guards never observe or clear the flag
// (AC3 — consume is in the outermost dtor Phase-5 path only).
inline std::atomic<std::uint64_t>
    g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_fired_total{0};
inline std::atomic<std::uint64_t>
    g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_consumed_total{0};
inline constexpr int kMutationHoldBudgetHolderDegradeCrossFiberCancelIssue = 2726;

[[nodiscard]] inline std::uint64_t
mutation_hold_budget_holder_degrade_cross_fiber_cancel_fired_total_v_read() noexcept {
    return g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_fired_total.load(
        std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t
mutation_hold_budget_holder_degrade_cross_fiber_cancel_consumed_total_v_read() noexcept {
    return g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_consumed_total.load(
        std::memory_order_relaxed);
}

inline void clear_mutation_hold_budget_holder_degrade_cross_fiber_cancel_for_test() noexcept {
    g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_fired_total.store(
        0, std::memory_order_relaxed);
    g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_consumed_total.store(
        0, std::memory_order_relaxed);
}

// Issue #2932: hold-budget overtime → forced outermost fail-closed (not
// cooperative Phase-5-only). When production / hard-env hold-budget cancel
// is set, the holder is also force-safepointed (#2533 infrastructure) and
// Fiber::check_gc_safepoint consumes the flag + mark_outermost_mutation_
// failed so Guard dtor releases workspace_mtx_ + MutationHold + residual
// closed-loop (#2846) even under a non-yielding body.
//
// Issue #2999: dtor consume is the *exit* half. Once ~MutationBoundaryGuard
// runs with cancel armed, it cannot commit success — even if
// check_gc_safepoint never ran. Remaining in-body window (body that never
// exits) still relies on #2932 force-safepoint to *enter* dtor; this is
// not a preemptive unlock while the body is still running.
//
// g_mutation_hold_budget_forced_fail_closed_total: bumped when the
// safepoint-edge path *or* the outermost dtor consume actually fail-closes.
// Distinct from Soft observe (#2701 soft_observe) and from voluntary
// #2726 cancel_consumed (still bumped for Agent health fired/consumed
// parity). Soft / sandbox=off: never bumps (reject_enabled gate). Nested
// guards never independently force-fail (outermost success flag only).
inline std::atomic<std::uint64_t> g_mutation_hold_budget_forced_fail_closed_total{0};
inline std::atomic<std::uint32_t> g_mutation_hold_budget_forced_fail_closed_wired{1};
inline constexpr int kMutationHoldBudgetForcedFailClosedIssue = 2932;
// Issue #2999: dtor-side consume of a pending cancel (additive split of
// forced-fail-closed-total). Safepoint consume does not bump this.
inline std::atomic<std::uint64_t> g_mutation_hold_budget_forced_fail_closed_dtor_consume_total{0};
inline constexpr int kMutationHoldBudgetForcedFailClosedDtorIssue = 2999;

[[nodiscard]] inline std::uint64_t mutation_hold_budget_forced_fail_closed_total_v_read() noexcept {
    return g_mutation_hold_budget_forced_fail_closed_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t mutation_hold_budget_forced_fail_closed_wired_v_read() noexcept {
    return g_mutation_hold_budget_forced_fail_closed_wired.load(std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t
mutation_hold_budget_forced_fail_closed_dtor_consume_total_v_read() noexcept {
    return g_mutation_hold_budget_forced_fail_closed_dtor_consume_total.load(
        std::memory_order_relaxed);
}

inline void clear_mutation_hold_budget_forced_fail_closed_for_test() noexcept {
    g_mutation_hold_budget_forced_fail_closed_total.store(0, std::memory_order_relaxed);
    g_mutation_hold_budget_forced_fail_closed_dtor_consume_total.store(0,
                                                                       std::memory_order_relaxed);
}

// Issue #3035: P0 force-unlock + dual-topology restore on hold-budget
// cancel for a non-yield body. #2932/#2999 arm the cancel flag and
// fail-closed at safepoint/dtor edges, but a mutate body that never
// polls the flag can keep workspace_mtx_ + per-fiber depth slot held
// indefinitely (steal/GC residual starve; densify×steal can observe
// half-topology). #3035 closes the remaining window: when the outermost
// Guard dtor consumes a pending hold-budget cancel under production /
// hard-env, the boundary is forced fail-closed (success=false even if
// the Guard has no success flag) so exit_mutation_boundary runs
// abort_restore_dual_topology + dual canary (same path as panic/abort)
// and the exit pipeline force-releases workspace_mtx_ + clears the
// fiber depth slot. Soft / sandbox=off: metric-only (existing
// soft_observe counters — no consume, no force). Additive — all
// #2932/#2999 counters preserved.
inline std::atomic<std::uint64_t> g_mutation_hold_budget_forced_unlock_total{0};
inline constexpr int kMutationHoldBudgetForcedUnlockIssue = 3035;

[[nodiscard]] inline std::uint64_t mutation_hold_budget_forced_unlock_total_v_read() noexcept {
    return g_mutation_hold_budget_forced_unlock_total.load(std::memory_order_relaxed);
}

inline void clear_mutation_hold_budget_forced_unlock_for_test() noexcept {
    g_mutation_hold_budget_forced_unlock_total.store(0, std::memory_order_relaxed);
}

// Issue #3118: production cancel force-unlock + depth clear immediately
// after dual-topology restore (dtor consume path). Reuses #3035
// forced_unlock_total — no new query keys. Soft stays observe-only.
inline constexpr int kMutationHoldBudgetCancelForceReleaseIssue = 3118;

// Issue #3071: in-body non-poll window after cancel+force-safepoint.
// #3035 closes the dtor half; a body that never reaches check_gc_safepoint
// / yield / Phase-5 can still hold workspace_mtx_ until it happens to
// exit. This stamps cancel-arm time and lets the scheduler idle path
// poll: if the holder is still outermost-held past a bounded multiple
// of the hold SLO, bump inbody-window-exceeded and re-arm force-safepoint
// (no preemptive unlock — topology stays consistent). Soft: observe only.
inline std::atomic<std::uint64_t> g_mutation_hold_budget_inbody_window_exceeded_total{0};
inline std::atomic<std::uint32_t> g_mutation_hold_budget_inbody_window_wired{1};
inline constexpr int kMutationHoldBudgetInbodyWindowIssue = 3071;
// Issue #3194: I1 residual — non-cooperative body past inbody window.
// Same-fiber force-release reuses #3118/#3035 (unlock + depth 0);
// cross-fiber pending-cancel only. Soft: helper no-ops. Reuses
// forced_unlock_total + forced_fail_closed_total — no new counters.
inline constexpr int kMutationHoldBudgetInbodyForceReleaseIssue = 3194;
// Issue #3222: I1 residual of #3194 — scheduler idle poll is always
// cross-fiber (pending-cancel only). Same-fiber inbody poll from
// Fiber::check_gc_safepoint force-releases hold + depth past the
// bound so a live body does not keep workspace_mtx_ until dtor.
// Reuses forced_unlock_total + forced_fail_closed_total.
inline constexpr int kMutationHoldBudgetInbodyForceUnlockIssue = 3222;
// Issue #3223: cross-fiber force_degrade must nudge the victim worker
// to run the same inbody poll / force-release as same-fiber (#3222).
// Does not unlock from the foreign thread. Reuses cross-fiber fired /
// consumed + forced_unlock_total — no new counters.
inline constexpr int kMutationHoldBudgetCrossFiberUrgentInbodyPollIssue = 3223;
// Issue #3254: I1 residual of #3222/#3223 — a non-cooperative outermost
// body that never hits check_gc_safepoint / yield / Phase-5 must still
// force-release within 2×SLO. The runtime injects a synthetic
// MutationBoundary yield and consumes it on the holder (dual restore +
// unlock + depth 0). Cross-fiber never drops unique_lock; it only
// injects so the victim's next edge matches same-fiber. Soft: observe
// only. Reuses forced_unlock_total + forced_fail_closed_total.
inline constexpr int kMutationHoldBudgetNoncoopForceEdgeIssue = 3254;
// Issue #3325: residual after #3254/#3285/#3071 — scheduler idle /
// worker park under production_multi_worker_latched re-injects the
// synthetic MutationBoundary yield + force_safepoint on a live
// outermost holder past 2×SLO when the body has not consumed a
// cooperative edge. Same-fiber consume still via force_release
// (unique_lock owner). Cross-fiber never drops the foreign unique_lock.
// Soft / sandbox=off: metric-only. New residual counter.
inline std::atomic<std::uint64_t> g_hold_budget_no_edge_force_total{0};
inline constexpr int kMutationHoldBudgetNoEdgeForceIssue = 3325;

[[nodiscard]] inline std::uint64_t hold_budget_no_edge_force_total_v_read() noexcept {
    return g_hold_budget_no_edge_force_total.load(std::memory_order_relaxed);
}

inline void clear_hold_budget_no_edge_force_for_test() noexcept {
    g_hold_budget_no_edge_force_total.store(0, std::memory_order_relaxed);
}

// Issue #3073: production soak readiness gate (residual-zero ×
// hold-after-cancel max). Wired sentinel only — no extra hot-path work.
// Soak abort lives in the chaos harness; Agents read schema-3073.
inline constexpr int kChaosProductionReadinessIssue = 3073;
inline std::atomic<std::uint32_t> g_chaos_production_readiness_gate_wired{1};
inline std::atomic<std::uint64_t> g_hold_budget_cancel_armed_ns{0};
inline std::atomic<std::uint64_t> g_hold_budget_cancel_armed_fiber{0};
// First production escalate per arm (force_degrade once; later polls
// only re-arm force-safepoint so holder-degrade totals stay honest).
inline std::atomic<std::uint32_t> g_hold_budget_cancel_escalated{0};

[[nodiscard]] inline std::uint64_t mutation_hold_inbody_window_bound_us() noexcept {
    const char* e = std::getenv("AURA_HOLD_BUDGET_INBODY_BOUND_US");
    if (e != nullptr && e[0] != '\0') {
        if (e[0] == '0' && e[1] == '\0')
            return 0;
        std::uint64_t v = 0;
        for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
            v = v * 10 + static_cast<std::uint64_t>(*p - '0');
        if (v > 0)
            return v;
    }
    auto slo = mutation_hold_slo_us();
    if (slo == 0)
        slo = mutation_hold_budget_us();
    return slo * 2ULL; // default 2× hold SLO
}

inline void mutation_hold_budget_note_cancel_armed(std::uint64_t fiber_id) noexcept {
    std::uint64_t expected = 0;
    const auto now = mutation_hold_steady_ns_now();
    if (g_hold_budget_cancel_armed_ns.compare_exchange_strong(
            expected, now, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        g_hold_budget_cancel_armed_fiber.store(fiber_id, std::memory_order_release);
        g_hold_budget_cancel_escalated.store(0, std::memory_order_release);
    }
}

inline void mutation_hold_budget_note_cancel_consumed(std::uint64_t fiber_id) noexcept {
    const auto armed = g_hold_budget_cancel_armed_fiber.load(std::memory_order_acquire);
    if (armed != 0 && (fiber_id == 0 || armed == fiber_id)) {
        g_hold_budget_cancel_armed_fiber.store(0, std::memory_order_release);
        g_hold_budget_cancel_armed_ns.store(0, std::memory_order_release);
        g_hold_budget_cancel_escalated.store(0, std::memory_order_release);
    }
}

[[nodiscard]] inline std::uint64_t
mutation_hold_budget_inbody_window_exceeded_total_v_read() noexcept {
    return g_mutation_hold_budget_inbody_window_exceeded_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t mutation_hold_budget_inbody_window_wired_v_read() noexcept {
    return g_mutation_hold_budget_inbody_window_wired.load(std::memory_order_relaxed);
}

inline void clear_mutation_hold_budget_inbody_window_for_test() noexcept {
    g_mutation_hold_budget_inbody_window_exceeded_total.store(0, std::memory_order_relaxed);
    g_hold_budget_cancel_armed_ns.store(0, std::memory_order_relaxed);
    g_hold_budget_cancel_armed_fiber.store(0, std::memory_order_relaxed);
    g_hold_budget_cancel_escalated.store(0, std::memory_order_relaxed);
}

// Issue #2724: region/subtree-scoped MutationBoundary concurrent admit.
// Shared header so evaluator_mutation_boundary (writers) and
// evaluator_primitives_query (query surface) share one definition.
// Additive — all #2701/#2720/#2587/#2630 surfaces preserved.
// Issue #2754: cone / ImpactScope mask-AND residual — equal keys may still
// be concurrent-admitted when both live cone masks prove no bit overlap
// (mask AND == 0). Additive cone-admit counter distinguishes key-disjoint
// vs cone-disjoint admits; all #2724 surfaces preserved.
inline std::atomic<std::uint64_t> g_mutation_region_concurrent_admit_total{0};
inline std::atomic<std::uint64_t> g_mutation_region_overlap_reject_total{0};
inline std::atomic<std::uint32_t> g_mutation_region_concurrent_wired{1};
inline constexpr int kMutationRegionConcurrentIssue = 2724;
// Issue #2754: admits that passed only via cone/mask-AND (equal keys,
// proven-disjoint ImpactScope bits). Subset of concurrent-admit-total.
inline std::atomic<std::uint64_t> g_mutation_region_concurrent_cone_admit_total{0};
inline std::atomic<std::uint32_t> g_mutation_region_cone_disjoint_wired{1};
inline constexpr int kMutationRegionConeDisjointIssue = 2754;
// Issue #2757: mask-AND admits including zero keys (superset of #2754 cone
// path). Bumped when admit is due to proven ImpactScope/dirty mask-AND
// rather than key-disjoint. Quiet path (no masks) never bumps this.
inline std::atomic<std::uint64_t> g_mutation_region_mask_disjoint_admit_total{0};
inline std::atomic<std::uint32_t> g_mutation_region_mask_disjoint_wired{1};
inline constexpr int kMutationRegionMaskDisjointIssue = 2757;

[[nodiscard]] inline std::uint64_t mutation_region_concurrent_admit_total_v_read() noexcept {
    return g_mutation_region_concurrent_admit_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t mutation_region_overlap_reject_total_v_read() noexcept {
    return g_mutation_region_overlap_reject_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t mutation_region_concurrent_wired_v_read() noexcept {
    return g_mutation_region_concurrent_wired.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t mutation_region_concurrent_cone_admit_total_v_read() noexcept {
    return g_mutation_region_concurrent_cone_admit_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t mutation_region_cone_disjoint_wired_v_read() noexcept {
    return g_mutation_region_cone_disjoint_wired.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t mutation_region_mask_disjoint_admit_total_v_read() noexcept {
    return g_mutation_region_mask_disjoint_admit_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t mutation_region_mask_disjoint_wired_v_read() noexcept {
    return g_mutation_region_mask_disjoint_wired.load(std::memory_order_relaxed);
}

// Issue #2724: simple disjointness check (region_key equality for first ship).
// Fast path kept as the key-only predicate when no cone masks are available.
[[nodiscard]] inline bool regions_disjoint(std::uint64_t a, std::uint64_t b) noexcept {
    return a != 0 && b != 0 && a != b;
}

// Issue #2754 / #2757 / #2761: proven ImpactScope / dirty-bit mask-AND is
// sole authority when both masks are non-zero; otherwise fall back to
// #2724 key-equality. Hot path is a bit AND only — no tree walk.
//
// Decision table (#2761 AC1/AC2/AC4):
//   both masks != 0 → (mask_a & mask_b) == 0
//     (unequal keys with overlapping cones REJECT — #2761 AC1)
//     (unequal keys with disjoint cones ADMIT — #2761 AC2)
//     (equal/zero keys with disjoint cones ADMIT — #2754/#2757)
//   either mask == 0 → #2724 key inequality only (safe fallback;
//     zero extra work beyond key compare — #2757 AC4 / #2761 AC4)
//
// Quiet path (either mask == 0): identical to #2724 equality only.
[[nodiscard]] inline bool regions_disjoint(std::uint64_t a, std::uint64_t b, std::uint64_t mask_a,
                                           std::uint64_t mask_b) noexcept {
    // Issue #2761: when both cone/ImpactScope masks are proven, mask-AND
    // is sole authority — catches unequal-key + overlapping-cone races
    // that #2724 key-inequality would incorrectly concurrent-admit.
    if (mask_a != 0 && mask_b != 0)
        return (mask_a & mask_b) == 0;
    // Quiet / missing-mask fallback: #2724 key-equality only.
    return a != 0 && b != 0 && a != b;
}

// Issue #2754: true only when the admit is due to the equal-key cone path
// (both keys non-zero and equal + proven mask-AND). Used to bump cone-admit.
[[nodiscard]] inline bool regions_cone_disjoint(std::uint64_t a, std::uint64_t b,
                                                std::uint64_t mask_a,
                                                std::uint64_t mask_b) noexcept {
    return a != 0 && b != 0 && a == b && mask_a != 0 && mask_b != 0 && (mask_a & mask_b) == 0;
}

// Issue #2757 / #2761: true when admit is due to proven mask-AND (both
// masks non-zero, empty intersection). Includes equal keys (#2754), zero
// keys (#2757), and unequal keys with proven-disjoint cones (#2761 AC2).
// Key-only admits (missing mask) do not count as mask-disjoint.
[[nodiscard]] inline bool regions_mask_disjoint(std::uint64_t a, std::uint64_t b,
                                                std::uint64_t mask_a,
                                                std::uint64_t mask_b) noexcept {
    (void)a;
    (void)b;
    return mask_a != 0 && mask_b != 0 && (mask_a & mask_b) == 0;
}

// Issue #2761: true when reject is due to proven mask overlap (both masks
// non-zero and intersection non-empty) — including unequal keys that
// share a cone. Distinguishes mask-strength rejects from key-equality
// rejects for Agent dashboards (AC5).
[[nodiscard]] inline bool regions_mask_overlap(std::uint64_t mask_a,
                                               std::uint64_t mask_b) noexcept {
    return mask_a != 0 && mask_b != 0 && (mask_a & mask_b) != 0;
}

// ── Issue #2760: ImpactScope / dirty-bit mask production enablement ──
// #2754/#2757 upgraded regions_disjoint to mask-AND; residual was that
// commercial multi-hypothesis paths rarely stamped proven masks (TLS /
// parallel-intend only supplied region_key). #2760:
//   - impact_block_to_region_mask_bit: offline pack (func, block) → bit
//     (no tree walk at admit — ImpactScope computed earlier)
//   - effective_region_cone_mask: prefer TLS proven mask; fall back to
//     single-bit from region_key only when key != 0
//   - impact-mask-admit counter: concurrent admits that used a non-zero
//     proven ImpactScope/dirty-style cone mask (TLS or derived)
// Quiet path (tls==0 && key==0): still zero extra cost (#2757 AC4).
// Issue #2761: mask-AND is sole authority when both masks proven (unequal
// keys with overlapping cones reject — closes #2724 residual race).
inline std::atomic<std::uint64_t> g_mutation_region_impact_mask_admit_total{0};
inline std::atomic<std::uint32_t> g_mutation_region_impact_mask_wired{1};
inline constexpr int kMutationRegionImpactMaskIssue = 2760;
// Issue #2761: rejects where proven masks overlap (including unequal keys).
// Subset of g_mutation_region_overlap_reject_total; dashboards split
// key-only overlap vs mask-strength overlap.
inline std::atomic<std::uint64_t> g_mutation_region_mask_overlap_reject_total{0};
inline std::atomic<std::uint32_t> g_mutation_region_mask_overlap_wired{1};
inline constexpr int kMutationRegionMaskOverlapIssue = 2761;

[[nodiscard]] inline std::uint64_t mutation_region_mask_overlap_reject_total_v_read() noexcept {
    return g_mutation_region_mask_overlap_reject_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t mutation_region_mask_overlap_wired_v_read() noexcept {
    return g_mutation_region_mask_overlap_wired.load(std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t mutation_region_impact_mask_admit_total_v_read() noexcept {
    return g_mutation_region_impact_mask_admit_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t mutation_region_impact_mask_wired_v_read() noexcept {
    return g_mutation_region_impact_mask_wired.load(std::memory_order_relaxed);
}

// Issue #2760: pack an ImpactScope (function, block) into one bit of a
// 64-bit region cone mask. Agents OR bits offline from
// compute_impact_scope / dirty propagation, then stamp via
// note_parallel_task_cone_mask or parallel-intend :cone-masks.
// Hot path admit remains a single mask AND (no tree walk).
[[nodiscard]] inline std::uint64_t
impact_block_to_region_mask_bit(std::size_t function_index, std::uint32_t block_index) noexcept {
    // Mix into [0, 63) — leave bit 63 free for a future "global" sentinel.
    const auto h = (static_cast<std::uint64_t>(function_index) * 1315423911ull) ^
                   (static_cast<std::uint64_t>(block_index) * 2654435761ull);
    return 1ULL << (h % 63ull);
}

// Issue #2760: single-bit fallback from region_key when no TLS cone mask
// was stamped. Equal keys → same bit → overlap-reject (safe). Different
// keys → different bits → admit under #2761 mask-first (or key fallback
// if only one side has a derived mask).
[[nodiscard]] inline std::uint64_t region_key_as_impact_mask(std::uint64_t region_key) noexcept {
    if (region_key == 0)
        return 0;
    return 1ULL << (region_key % 63ull);
}

// Issue #2760 / #2761: effective cone / ImpactScope mask for concurrent
// admit. Prefer producer-stamped TLS (proven ImpactScope / dirty-bit
// mask); else soft single-bit from region_key. Zero when both unset
// (quiet). When both sides have non-zero effective masks, #2761
// mask-AND is sole authority (including unequal keys).
[[nodiscard]] inline std::uint64_t effective_region_cone_mask(std::uint64_t tls_cone_mask,
                                                              std::uint64_t region_key) noexcept {
    if (tls_cone_mask != 0)
        return tls_cone_mask;
    return region_key_as_impact_mask(region_key);
}

// Issue #3039: production ScopedParallel overlap hard-reject (no
// GlobalExclusive fallback). Soft / sandbox=off does not bump this
// (observe-only on the existing overlap-reject-total). Appended at
// struct END so prior #2724/#2754/#2761 layouts stay stable.
inline std::atomic<std::uint64_t> g_mutation_region_overlap_hard_reject_total{0};
inline std::atomic<std::uint32_t> g_mutation_region_overlap_hard_reject_wired{1};
inline std::atomic<std::uint32_t> g_mutation_region_overlap_last_reason{0}; // 1 = region-overlap
inline constexpr int kMutationRegionOverlapHardRejectIssue = 3039;

[[nodiscard]] inline std::uint64_t mutation_region_overlap_hard_reject_total_v_read() noexcept {
    return g_mutation_region_overlap_hard_reject_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t mutation_region_overlap_hard_reject_wired_v_read() noexcept {
    return g_mutation_region_overlap_hard_reject_wired.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t mutation_region_overlap_last_reason_v_read() noexcept {
    return g_mutation_region_overlap_last_reason.load(std::memory_order_relaxed);
}

} // namespace aura::compiler

#endif // AURA_COMPILER_MUTATION_HOLD_BUDGET_H
