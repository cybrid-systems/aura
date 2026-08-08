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
[[nodiscard]] inline bool mutation_hold_slo_soft_mode() noexcept {
    const char* soft = std::getenv("AURA_MUTATION_HOLD_SLO_SOFT");
    if (soft && soft[0] == '1')
        return true;
    const char* sandbox = std::getenv("AURA_SANDBOX");
    return sandbox && sandbox[0] != '\0' && std::string_view(sandbox) == "off";
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
        g_mutation_hold_live_clear_total.fetch_add(1, std::memory_order_relaxed);
    }
}

struct MutationHoldLiveSnapshot {
    std::uint64_t fiber_id = 0;
    std::uint64_t start_ns = 0;
    std::uint64_t duration_us = 0;
    std::uint32_t depth = 0;
    bool held = false;
};

// Pure read (+ optional over-budget observe counter). No holder → zeros (AC3).
[[nodiscard]] inline MutationHoldLiveSnapshot mutation_hold_live_snapshot() noexcept {
    MutationHoldLiveSnapshot s;
    s.fiber_id = g_mutation_hold_live_fiber_id.load(std::memory_order_acquire);
    s.start_ns = g_mutation_hold_live_start_ns.load(std::memory_order_acquire);
    s.depth = g_mutation_hold_live_depth.load(std::memory_order_relaxed);
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
// Fast path kept as the primary key-disjoint predicate.
[[nodiscard]] inline bool regions_disjoint(std::uint64_t a, std::uint64_t b) noexcept {
    return a != 0 && b != 0 && a != b;
}

// Issue #2754 / #2757: key-disjoint (#2724 fast path) OR proven
// ImpactScope / dirty-bit mask-AND with empty intersection.
// Hot path is a bit AND only — no tree walk.
//
// Quiet path (either mask == 0): identical to #2724 equality only
// (AC4 #2757 — zero extra work; no mask-AND).
// Proven masks (both non-zero): empty intersection → disjoint even when
// keys collide or are zero (#2757 AC1). Key-disjoint still wins when
// both keys non-zero and unequal (even if masks overlap).
[[nodiscard]] inline bool regions_disjoint(std::uint64_t a, std::uint64_t b, std::uint64_t mask_a,
                                           std::uint64_t mask_b) noexcept {
    // #2724 key-disjoint fast path (quiet when keys alone prove it).
    if (a != 0 && b != 0 && a != b)
        return true;
    // Quiet path: no proven masks → equality only (already failed above
    // for zero/equal keys). Zero extra work beyond the key compare.
    if (mask_a == 0 || mask_b == 0)
        return false;
    // #2757: proven ImpactScope / dirty mask-AND (covers equal keys and
    // zero keys). Empty intersection → concurrent-admissible.
    return (mask_a & mask_b) == 0;
}

// Issue #2754: true only when the admit is due to the equal-key cone path
// (both keys non-zero and equal + proven mask-AND). Used to bump cone-admit.
[[nodiscard]] inline bool regions_cone_disjoint(std::uint64_t a, std::uint64_t b,
                                                std::uint64_t mask_a,
                                                std::uint64_t mask_b) noexcept {
    return a != 0 && b != 0 && a == b && mask_a != 0 && mask_b != 0 && (mask_a & mask_b) == 0;
}

// Issue #2757: true when admit is due to mask-AND (not key-disjoint).
// Covers equal keys (#2754) and zero keys with proven disjoint masks.
[[nodiscard]] inline bool regions_mask_disjoint(std::uint64_t a, std::uint64_t b,
                                                std::uint64_t mask_a,
                                                std::uint64_t mask_b) noexcept {
    // Key-disjoint path does not count as mask-disjoint.
    if (a != 0 && b != 0 && a != b)
        return false;
    return mask_a != 0 && mask_b != 0 && (mask_a & mask_b) == 0;
}

} // namespace aura::compiler

#endif // AURA_COMPILER_MUTATION_HOLD_BUDGET_H
