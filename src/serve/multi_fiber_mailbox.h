// multi_fiber_mailbox.h — Issue #1585 / #1211 / #1595 / #2312 / #2316:
// MultiFiberMailbox with
// multi-attach, broadcast, blocking recv, priority, and backpressure.
// #1595: linear-claim payload prefix filter (linear-viol:) + process counters.
// #1881: fanout linear_checks + local push stats.
// #2010: shared linear filter on all entry points; fanout backpressure
//        observability (+ orch hook for dashboards).
// #2188: forbid blocking recv / Fiber::yield while MutationBoundary is live
//        (depth>0 or held) — Policy A: non-blocking empty + metric, no park.
// #2347: Guard-live blocking recv → hard audit under Strict/production.
//        Agent contract: **Guard 内禁止 blocking recv**; use try_recv /
//        recv(wait=false) or exit MutationBoundary first. Policy A stays
//        non-blocking; Strict bumps hard counter and may force-rollback
//        after N rejects in one outermost Guard window.
// #2312: push/fanout defer (Backpressure) when target holds MutationBoundary.
// #2378: defer drain SLA — deferred_depth / HWM, flush latency after
//        outermost Guard exit, starvation signal if depth stays open.
//        Zero cost when deferred_depth==0 (single relaxed load on Ok path).
// #2903: deferred-under-boundary wait histogram — Agent-visible p50/p99/max
//        wait-us from first defer decision to deliver (or budget drop).
//        Closes silent starvation observability under long holds; Soft /
//        zero-defer path stays single relaxed load (no hist noise).
// #2958: production hold-budget cancel when under-boundary wait ≥ SLO
//        (or open-window age / throttle). Complements #2947 schedule gate
//        (deny new admits) by force-degrading the live outermost holder.
//        Soft: observe-only. Under-SLO / no open defer: early return.
// #3256: p99/SLO arm uses the same hold-budget force path as
//        #2701/#2720/#3254 (force_degrade on live fiber_id, then
//        poll_inbody_window). No second unlock. Soft still observe-only.
// #3002: fill_mailbox_hold_slo_live_ + this sample share p99/throttle/SLO
//        (no second hist walk). Production + mailbox_hold_slo_signal +
//        live holder → one-shot cancel (reuse #2958 CAS; no double-arm).
// #2511: outermost Guard exit forces deferred drain under budget
//        (AURA_MAILBOX_HOLD_DRAIN_BUDGET_US, default 1000 µs). Soft: retain
//        + starvation. Strict: force-resolve remaining depth + audit.
//        AC5: depth==0 → single relaxed load.
// #2316: wire mu_ acquire to lock_order::on_acquire(Level::Mailbox) for
//        rank-table audit + AURA_LOCK_ORDER_CANARY inversion detection.
// Header form (like mailbox.h) so serve + tests can include without module churn.

#ifndef AURA_SERVE_MULTI_FIBER_MAILBOX_H
#define AURA_SERVE_MULTI_FIBER_MAILBOX_H

#include "fiber.h"
#include "steal_safety.h"                          // #2987 mailbox residual hard-AND
#include "compiler/lock_order_audit.h"             // Issue #2316: lock-order audit
#include "compiler/mutation_concurrency_health.hh" // #2903/#2958 wait SLO
#include "compiler/mutation_hold_budget.h"         // #2958 live outermost holder

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

// Issue #2010: optional orch mirror for mailbox backpressure (weak no-op when
// orch is not linked; strong def bumps OrchModuleStats::send_backpressure_total).
extern "C" void aura_orch_note_mailbox_backpressure();
// Issue #2347: force outermost mutation success_flag=false (strong def in
// evaluator_fiber_mutation.cpp; weak no-op when Evaluator not linked).
extern "C" void aura_evaluator_mark_outermost_mutation_failed() noexcept;

// Issue #2720: P0 holder-degrade path (#2701 residual). Force-degrade the
// recorded holder fiber when production (or AURA_MUTATION_HOLD_BUDGET_HARD=1)
// and live hold > budget. Same-fiber cancel via g_current_fiber +
// mark_outermost_mutation_failed; cross-fiber pending-cancel + urgent
// inbody poll (#3223) so the victim worker force-releases past 2×SLO.
// Caller (try_acquire) still rejects *this* admit (#2701 path); this
// just adds the holder-side cancel.
extern "C" void aura_evaluator_force_degrade_outermost_holder(std::uint64_t fiber_id) noexcept;
// Issue #2346 / #2347: production canary probe (strong in audit hooks).
extern "C" int aura_production_defaults_active_probe() noexcept;
// Issue #2726 / #2958: set Fiber pending hold-budget cancel (strong in fiber.cpp).
extern "C" int aura_fiber_request_hold_budget_cancel(std::uint64_t fiber_id) noexcept;

namespace aura::serve::mf_mailbox {

inline constexpr int kMultiFiberMailboxPhase = 3; // #1881 observability
inline constexpr int kMultiFiberMailboxIssue = 1881;
// Issue #2972: per-mailbox inflight credit (complement storm-oriented
// BP-recent admit #2228/#2535). 0 credit_limit → use high_water.
inline constexpr int kMailboxCreditInflightIssue = 2972;

// Issue #1595 / #2010: provenance-safety prefix (fiber-stack safe, pure string).
inline constexpr std::string_view kLinearViolPrefix = "linear-viol:";

enum class MailPriority : std::uint8_t { Low = 0, Normal = 1, High = 2, Critical = 3 };

enum class PushStatus : std::uint8_t {
    Ok = 0,
    Backpressure = 1, // queue at high-water mark
    Closed = 2,
    // Issue #2884 / #3013 / #3212: typed handoff-required failure.
    // Returned by agent_send_safe (handoff fail), raw agent_send
    // (unstamped held_ref_token), and MultiFiberMailbox::push /
    // broadcast_fanout (same unstamped held_ref gate). Distinct from
    // Closed so C++ callers can disambiguate mailbox-closed from
    // export-stale / handoff-required — never silent Closed
    // conflation. Closed is reserved for true closed / linear-viol.
    HandoffRequired = 3,
};

// Issue #2538: typed ask/reply correlation on MailMessage.
// kind=Normal + correlation_id=0 preserves the legacy text-prefix path
// ("ask:<id>:" / "reply:<id>:") for #2231/#2401 compatibility.
enum class MailKind : std::uint8_t {
    Normal = 0,
    Ask = 1,
    Reply = 2,
};

struct MailMessage {
    std::uint64_t from_fiber = 0;
    std::uint64_t to_fiber = 0; // 0 = broadcast / any
    MailPriority priority = MailPriority::Normal;
    std::string payload;
    // Issue #2538: typed correlation (0 = none / legacy text-prefix only).
    std::uint64_t correlation_id = 0;
    MailKind kind = MailKind::Normal;
    // Issue #2663 / #3212: held-ref export token + handoff-completed flag.
    // When held_ref_token is set, the message carries a StableNodeRef that
    // needs to be re-exported via Evaluator::handoff_ref. The mailbox
    // gate rejects any push where held_ref_token is set but
    // handoff_completed is false, reject (HandoffRequired +
    // handoff_reject_total bump). Closed is reserved for true closed /
    // linear-viol. Ordinary string payloads leave both default-initialized
    // (zero cost on hot path — single optional load + bool check).
    // Populated by Agent-send-side helpers (agent_send_ref) after a
    // successful handoff_ref call.
    std::optional<std::uint64_t> held_ref_token{};
    bool handoff_completed = false;
};

struct MultiFiberMailboxStats {
    std::atomic<std::uint64_t> pushes{0};
    std::atomic<std::uint64_t> pops{0};
    std::atomic<std::uint64_t> broadcasts{0};
    std::atomic<std::uint64_t> priority_high{0};
    std::atomic<std::uint64_t> backpressure_rejects{0};
    std::atomic<std::uint64_t> attaches{0};
    std::atomic<std::uint64_t> recv_waits{0};
    std::atomic<std::uint64_t> recv_timeouts{0};
    // Issue #1595: linear claim checks / violations (prefix filter on push).
    std::atomic<std::uint64_t> linear_checks{0};
    std::atomic<std::uint64_t> linear_violations{0};
    // Issue #2632: handoff reject counter (single internal helper gate, mirror
    // of CompilerMetrics::stable_ref_handoff_reject_total). Bumped when an
    // Agent hands a StableNodeRef across the mailbox / fanout boundary
    // without first calling Evaluator::handoff_ref — agents reading the
    // mailbox payload should observe this and force-resolve their held refs.
    // Issue #2632: handoff reject counter (single internal helper gate, mirror
    // of CompilerMetrics::stable_ref_handoff_reject_total). Bumped when an
    // Agent hands a StableNodeRef across the mailbox / fanout boundary
    // without first calling Evaluator::handoff_ref — agents reading the
    // mailbox payload should observe this and force-resolve their held refs.
    //
    // Issue #2700: explicit happens-before contract. While outermost
    // MutationBoundaryGuard is held (workspace_mtx_ exclusive + MutationHold
    // GC defer), any MailMessage that carries a StableNodeRef payload
    // MUST have completed Evaluator::handoff_ref before push/broadcast
    // succeeds. Messages that arrive without handoff_completed are rejected
    // (HandoffRequired + bump handoff_reject_total + bump
    // local_stats_.handoff_reject_total; Closed reserved for true closed).
    // Query / mutate observers on other fibers either block on workspace_mtx_
    // or see only committed post-exit state. The contract is verified by:
    //   - scripts/coverage/checks/check_handoff_ref_mailbox_gate_2700.py (linter)
    //   - tests/serve/test_mailbox_recv_mutation_boundary.cpp ac2700_* (chaos)
    std::atomic<std::uint64_t> handoff_reject_total{0};
    // Issue #2010: fanout-specific backpressure (also counted in backpressure_rejects).
    std::atomic<std::uint64_t> fanout_backpressure_rejects{0};
    // Issue #2188: blocking recv refused while MutationBoundary is live
    // (depth>0 or held) — Policy A non-blocking empty return.
    std::atomic<std::uint64_t> recv_rejected_in_mutation_boundary{0};
    // Issue #2347: Strict / production hard path for the same Policy A
    // reject (Agents must poll this; Soft path leaves it at 0).
    std::atomic<std::uint64_t> recv_rejected_in_mutation_boundary_hard_total{0};
    // Issue #2347: times window threshold forced mutation mark-failed.
    std::atomic<std::uint64_t> recv_boundary_force_rollback_total{0};
    // Issue #2312: delivery gate — bump when push / broadcast_fanout
    // observes the target fiber(s) holding a live MutationBoundary
    // (depth>0 or held) and defers (Backpressure) rather than risking
    // AST writes interleaving with foreign delivery under the same
    // workspace_mtx_ exclusive + GcDeferReason::MutationHold arm.
    // Reuse the #2184 MutationSafetySnapshot + is_at_mutation_boundary_safe
    // truth table. Distinct from recv_rejected_in_mutation_boundary which
    // is the recv-side gate (#2188) — this is the push-side gate.
    std::atomic<std::uint64_t> mailbox_deferred_mutation_hold_total{0}; // #2312
    // Issue #2378: drain SLA (process-wide; local mirrors optional).
    // deferred_depth = outstanding mutation-hold defers (sender retries).
    // Not a message queue — #2312 still returns Backpressure (no drop).
    std::atomic<std::uint64_t> mailbox_deferred_depth{0};                   // #2378
    std::atomic<std::uint64_t> mailbox_deferred_depth_high_water{0};        // #2378
    std::atomic<std::uint64_t> mailbox_deferred_flush_latency_us_total{0};  // #2378
    std::atomic<std::uint64_t> mailbox_deferred_flush_samples{0};           // #2378
    std::atomic<std::uint64_t> mailbox_deferred_flush_latency_us_max{0};    // #2378
    std::atomic<std::uint64_t> mailbox_defer_starvation_total{0};           // #2378
    std::atomic<std::uint64_t> mailbox_deferred_drain_opportunity_total{0}; // #2378
    // Issue #3111: post-steal re-validate of held_ref messages. Bumped on
    // steal-complete / resume of a fiber that owns (or is attached to) a
    // mailbox containing held_ref messages. For each pending message with
    // held_ref_token set, the post-steal hook re-validates against the
    // current generation and either drops the stale message or marks it
    // HandoffRequired (never silent stale StableNodeRef delivery, AC1).
    // Soft / sandbox=off: observability only (may still deliver, AC3);
    // zero new cost on the quiet path (no held_ref messages, no steal).
    std::atomic<std::uint64_t> held_ref_post_steal_check_total{0};
    std::atomic<std::uint64_t> held_ref_stale_after_steal_total{0};
    // Issue #2680: shared-Evaluator delivery gate — push / broadcast_fanout
    // defers when the **shared** Evaluator's MutationBoundary is held
    // (depth>0 || held) by *any* fiber, not just the target fiber. Mirrors
    // the recv() shared-Evaluator check (multi_fiber_mailbox.h L820-821).
    // The per-target-fiber check (MutationSafetySnapshot) still fires first
    // for receiver-state safety; the shared-Evaluator check fires BEFORE
    // the per-target-fiber check so the receiver never observes a payload
    // delivered while another fiber on the same Evaluator is mid-mutation.
    // Sender retries / queues; deferred (not dropped) per AC2.
    // Happy path (deferred_depth==0): zero cost — one relaxed load + branch.
    std::atomic<std::uint64_t> mailbox_shared_evaluator_deferred_total{0};              // #2680
    std::atomic<std::uint64_t> mailbox_shared_evaluator_deferred_hard_total{0};         // #2680
    std::atomic<std::uint64_t> mailbox_shared_evaluator_deferred_soft_observe_total{0}; // #2680
    // Issue #2849: production fail-closed face of the shared-Evaluator
    // mid-mutation delivery gate (#2680 residual). Same authority
    // (depth>0 || held) — always Backpressure, never enqueue. under_boundary_*
    // counters are the Agent-facing #2849 names (bumped by
    // note_mailbox_deferred_under_boundary alongside the #2680 family).
    // Soft: soft_observe only; production/Strict: hard_total. Residual
    // deferred after hold-exit budget still uses #2551 hard throttle.
    std::atomic<std::uint64_t> mailbox_under_boundary_deferred_total{0};              // #2849
    std::atomic<std::uint64_t> mailbox_under_boundary_deferred_hard_total{0};         // #2849
    std::atomic<std::uint64_t> mailbox_under_boundary_deferred_soft_observe_total{0}; // #2849
    // Issue #2972: per-mailbox inflight credit backpressure. Distinct from
    // queue high_water (memory bound) and from process/scope recent gauges
    // (storm admit). Bumped when push sees inflight >= credit_limit.
    std::atomic<std::uint64_t> mailbox_credit_bp_total{0}; // #2972
    std::atomic<std::uint64_t> mailbox_inflight_hwm{0};    // #2972
    // Issue #2903: deferred-under-boundary wait latency (defer decision →
    // first successful reopen deliver, or force-drop under hold-exit budget).
    // Coarse 5-bucket histogram (µs edges: <100, <1k, <10k, <100k, ≥100k)
    // + total/samples/max + p50/p99 edge approximations. Zero cost when
    // deferred_depth==0 (happy Ok path never reaches note helper).
    static constexpr std::size_t kUnderBoundaryWaitHistBuckets = 5;
    std::atomic<std::uint64_t> mailbox_under_boundary_wait_us_total{0};   // #2903
    std::atomic<std::uint64_t> mailbox_under_boundary_wait_samples{0};    // #2903
    std::atomic<std::uint64_t> mailbox_under_boundary_wait_us_max{0};     // #2903
    std::atomic<std::uint64_t> mailbox_under_boundary_wait_us_p50{0};     // #2903
    std::atomic<std::uint64_t> mailbox_under_boundary_wait_us_p99{0};     // #2903
    std::atomic<std::uint64_t> mailbox_under_boundary_wait_drop_total{0}; // #2903 budget drop
    std::atomic<std::uint64_t>
        mailbox_under_boundary_wait_hist[kUnderBoundaryWaitHistBuckets]{}; // #2903
    // Issue #2958: production hold-budget cancel when wait/open-age ≥ SLO.
    // cancel_total: successful request_hold_budget_cancel on live holder
    // soft_observe_total: SLO hot under Soft / non-production
    // breach_observe_total: every hot evaluation (prod + soft)
    // no_holder_total: hot but no live outermost holder fiber
    std::atomic<std::uint64_t> mailbox_defer_slo_hold_cancel_total{0};    // #2958
    std::atomic<std::uint64_t> mailbox_defer_slo_soft_observe_total{0};   // #2958
    std::atomic<std::uint64_t> mailbox_defer_slo_breach_observe_total{0}; // #2958
    std::atomic<std::uint64_t> mailbox_defer_slo_no_holder_total{0};      // #2958
    std::atomic<std::uint32_t> mailbox_defer_slo_hold_cancel_wired{1};    // #2958
    // Issue #3002: SSOT live sample (p99 + throttle) feeds both #2947 deny
    // and #2958 cancel. Additive wired sentinel; no second hist walk.
    std::atomic<std::uint32_t> mailbox_hold_slo_ssot_wired{1}; // #3002
    // Issue #2987: mailbox delivery residual hard-AND (same StealInvariant
    // table as steal: LayoutStampMatch / TicketFresh / GcDeferClear).
    // RejectHard → Backpressure, never enqueue. Soft: soft_observe;
    // production/Strict: hard_total. Happy path (no inject, no target
    // residual) is a thread_local load — no extra atomics (AC3).
    std::atomic<std::uint64_t> mailbox_delivery_reject_layout_stamp_total{0}; // #2987
    std::atomic<std::uint64_t> mailbox_delivery_reject_ticket_stale_total{0}; // #2987
    std::atomic<std::uint64_t> mailbox_delivery_reject_residual_total{0};     // #2987
    std::atomic<std::uint64_t> mailbox_delivery_reject_hard_total{0};         // #2987
    std::atomic<std::uint64_t> mailbox_delivery_reject_soft_observe_total{0}; // #2987
    std::atomic<std::uint32_t> mailbox_delivery_safety_wired{1};              // #2987
    // Issue #3036: production_defaults residual RejectHard face. Independent
    // of is_mutate_mailbox_strict so sandbox/env Soft leak cannot hide a
    // hard deny. Soft / AURA_SANDBOX=off stay on soft_observe only.
    std::atomic<std::uint64_t> mailbox_residual_hard_reject_total{0}; // #3036
    std::atomic<std::uint32_t> mailbox_residual_hard_reject_wired{1}; // #3036
    // Issue #2511: outermost Guard exit forced deferred drain under budget.
    // hold_exit_drain_total: drain path entered with open depth
    // hold_exit_drain_us_total / max: elapsed under budget
    // hold_exit_starvation_total: budget exhausted with depth still open
    // hold_exit_force_resolved_total: Strict/production force-closed depth
    std::atomic<std::uint64_t> mailbox_hold_exit_drain_total{0};          // #2511
    std::atomic<std::uint64_t> mailbox_hold_exit_drain_us_total{0};       // #2511
    std::atomic<std::uint64_t> mailbox_hold_exit_drain_us_max{0};         // #2511
    std::atomic<std::uint64_t> mailbox_hold_exit_starvation_total{0};     // #2511
    std::atomic<std::uint64_t> mailbox_hold_exit_force_resolved_total{0}; // #2511
    // Issue #2551: production hard signal when residual deferred after budget.
    // Hard counter advances under Strict/production on starve; Soft retains
    // metric-only (#2511 starvation_total). Agent throttle flag is separate
    // (query-visible) so orch can refuse new mutate until depth clears.
    std::atomic<std::uint64_t> mailbox_hold_starvation_hard_total{0};    // #2551
    std::atomic<std::uint64_t> agent_throttle_for_mailbox_starvation{0}; // #2551 0/1
    // Issue #2587: count of mutate rejects gated by agent_throttle_for_
    // mailbox_starvation (mirror capability_revoke_total breakdown —
    // hard reject vs metric-only soft path). Bumped from every gate site
    // (TransactionGuard ctor + MutationBoundaryGuard::try_acquire +
    // try_acquire_for_region + evaluator_primitives_mutate.cpp public
    // prims); production_defaults_active at the call site decides whether
    // a bumped count accompanies a Rejected result (hard) or just a metric
    // tick (soft — single relaxed load path; AC2 / AC5).
    std::atomic<std::uint64_t> mutate_rejected_mailbox_starvation_total{0}; // #2587
};

// Process-wide aggregate (tests / observability).
inline MultiFiberMailboxStats g_mf_mailbox_stats{};

// Issue #3111: post-steal re-validate of held_ref messages in a fiber's
// mailbox. Called from the strong def of `aura_evaluator_on_steal_complete`
// (evaluator_fiber_mutation.cpp:3227) after the existing revoke session
// grants + mutation stack handoff + LayoutStamp dual-check. Bumps the
// held_ref_post_steal_check_total counter (always — even when no held_ref
// messages or no mailbox). The actual re-validation walk is done in
// the strong def via `bump_held_ref_stale_after_steal()` per stale message.
//
// Zero new cost on the quiet path: this is a no-op helper that just
// counts. The per-message walk lives in the strong def (where the
// mailbox is already held under lock during the steal-complete hook).
//
// Soft / Off / sandbox=off: counter bumps only; may still deliver.
// Production: callers call bump_held_ref_stale_after_steal() per stale
// message and clear the message's handoff_completed flag so the
// consumer's recv() path is forced through the handoff gate.
inline void revalidate_held_ref_after_steal() noexcept {
    g_mf_mailbox_stats.held_ref_post_steal_check_total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #3111: count the stale-after-steal re-validation. Called from
// the strong def of aura_evaluator_on_steal_complete for each pending
// message with held_ref_token that is found stale against the current
// generation post-steal.
inline void bump_held_ref_stale_after_steal() noexcept {
    g_mf_mailbox_stats.held_ref_stale_after_steal_total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #2378: open-window timers for flush latency (zero when no open defer).
// first_open_defer_ns: set on depth 0→1; cleared when depth returns to 0.
// last_outermost_exit_ns: set on Guard outermost exit (drain opportunity).
inline std::atomic<std::uint64_t> g_mailbox_first_open_defer_ns{0};
inline std::atomic<std::uint64_t> g_mailbox_last_outermost_exit_ns{0};
// Issue #2554 flaky: dedupe starvation canary — bump mailbox_defer_starvation
// once per open-defer window (not once per drain). Cleared when the window
// closes (depth returns to 0, first_open_defer_ns exchanged to 0). Without
// this, a mailbox that stays deferred >100ms bumps on every outermost exit
// drain → 100000-level counter storm → chaos PR gate (ceiling 0) flakes.
inline std::atomic<bool> g_mailbox_defer_starve_reported{false};
// Issue #2958: one-shot arm for SLO→hold-cancel (cleared when defer window
// closes). Prevents cancel storms while Fiber CAS consume stays one-shot.
inline std::atomic<std::uint8_t> g_mailbox_defer_slo_hold_cancel_armed{0};
inline constexpr int kMailboxDeferSloHoldCancelIssue = 2958;

// Default starvation: deferred_depth > 0 for ≥100ms after first open defer
// and at least one outermost exit was observed. Override ms via
// AURA_MAILBOX_DEFER_STARVATION_MS (0 disables).
[[nodiscard]] inline std::uint64_t mailbox_defer_starvation_ms() noexcept {
    const char* e = std::getenv("AURA_MAILBOX_DEFER_STARVATION_MS");
    if (e == nullptr || e[0] == '\0')
        return 100;
    std::uint64_t v = 0;
    for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
        v = v * 10 + static_cast<std::uint64_t>(*p - '0');
    return v;
}

[[nodiscard]] inline std::uint64_t mailbox_steady_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Issue #2378: note a mutation-hold defer (push/fanout returned BP).
// AC3: only called on the defer path (not on happy Ok push).
inline void note_mailbox_mutation_hold_defer() noexcept {
    g_mf_mailbox_stats.mailbox_deferred_mutation_hold_total.fetch_add(1, std::memory_order_relaxed);
    const auto d =
        g_mf_mailbox_stats.mailbox_deferred_depth.fetch_add(1, std::memory_order_relaxed) + 1;
    // High-water (relaxed CAS-free: max via loop is fine for metrics).
    auto hwm = g_mf_mailbox_stats.mailbox_deferred_depth_high_water.load(std::memory_order_relaxed);
    while (d > hwm && !g_mf_mailbox_stats.mailbox_deferred_depth_high_water.compare_exchange_weak(
                          hwm, d, std::memory_order_relaxed)) {
    }
    // Open window start (only first open defer).
    std::uint64_t expected = 0;
    const auto now = mailbox_steady_ns();
    (void)g_mailbox_first_open_defer_ns.compare_exchange_strong(expected, now,
                                                                std::memory_order_relaxed);
}

// Issue #2551: clear Agent throttle once deferred mailbox pressure is gone.
// Called on free drain path (depth==0) and when open window closes (1→0).
inline void clear_agent_throttle_for_mailbox_starvation() noexcept {
    g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.store(0, std::memory_order_relaxed);
}

// Issue #2587: zero-cost probe for agent_throttle_for_mailbox_starvation.
// Single relaxed atomic load (no side effects, no syscall). When the flag
// is 0 the inlined load + branch is the entire hot-path overhead (AC5 —
// zero cost when flag == 0). MutationBoundaryGuard::try_acquire (host +
// fiber soft path), TransactionGuard ctor (#2555 unified entry), and the
// public mutate:* prims in evaluator_primitives_mutate.cpp call this
// before admitting new mutate work; the call site decides hard reject vs
// metric-only soft tick based on production_defaults_active() (#2543
// sibling pattern). Header-inline + always_inline so the relaxed load
// folds into the gate branch with no extra symbol cost in caller TUs.
[[nodiscard, gnu::always_inline]] inline bool aura_orch_mailbox_starvation_throttled() noexcept {
    return g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
               std::memory_order_relaxed) != 0;
}

// Issue #3002: SSOT live sample for p99 + throttle + SLO. No hist walk.
// Quiet path: two relaxed loads (p99 + throttle) + SLO helper. Shared by
// fill_mailbox_hold_slo_live_ (#2947) and maybe_mailbox_defer_slo_hold_cancel
// (#2958) so the two faces cannot drift.
inline constexpr int kMailboxHoldSloSsotIssue = 3002;
inline void sample_mailbox_hold_slo_live(std::uint64_t& p99_us, bool& throttled,
                                         std::uint64_t& slo_us) noexcept {
    p99_us = g_mf_mailbox_stats.mailbox_under_boundary_wait_us_p99.load(std::memory_order_relaxed);
    throttled = aura_orch_mailbox_starvation_throttled();
    slo_us = aura::compiler::mailbox_under_boundary_wait_slo_us();
}
[[nodiscard]] inline bool mailbox_hold_slo_live_signal(std::uint64_t p99_us, std::uint64_t slo_us,
                                                       bool throttled) noexcept {
    return (slo_us != 0 && p99_us >= slo_us) || throttled;
}

// Issue #2587: bump the throttle-rejected counter at every gate site.
// Hard-reject / metric-only decision lives at the call site so callers
// can decide whether production_defaults_active() is the actual hard
// gate (Strict/Restricted) or whether to allow the work (Soft / Off).
// Header-inline so the relaxed fetch_add folds with the caller's branch.
[[gnu::always_inline]] inline void note_mutate_rejected_mailbox_starvation() noexcept {
    g_mf_mailbox_stats.mutate_rejected_mailbox_starvation_total.fetch_add(
        1, std::memory_order_relaxed);
}

// Issue #2958: forward decl — defined after wait sample (hist p99 refresh).
inline void maybe_mailbox_defer_slo_hold_cancel() noexcept;

// Issue #2903: record one under-boundary wait sample (µs).
// Called only from deferred reopen paths (window close Ok deliver, or
// hold-exit budget force-drop) — never from the happy Ok path.
// Coarse hist edges (µs): [0]<100 [1]<1k [2]<10k [3]<100k [4]≥100k.
// p50/p99 are upper-edge approximations of the buckets covering the
// cumulative 50th / 99th percentile (Agent-pollable without stitching).
inline void note_mailbox_under_boundary_wait_sample(std::uint64_t us, bool dropped) noexcept {
    g_mf_mailbox_stats.mailbox_under_boundary_wait_us_total.fetch_add(us,
                                                                      std::memory_order_relaxed);
    g_mf_mailbox_stats.mailbox_under_boundary_wait_samples.fetch_add(1, std::memory_order_relaxed);
    auto mx = g_mf_mailbox_stats.mailbox_under_boundary_wait_us_max.load(std::memory_order_relaxed);
    while (us > mx && !g_mf_mailbox_stats.mailbox_under_boundary_wait_us_max.compare_exchange_weak(
                          mx, us, std::memory_order_relaxed)) {
    }
    std::size_t b = MultiFiberMailboxStats::kUnderBoundaryWaitHistBuckets - 1;
    if (us < 100)
        b = 0;
    else if (us < 1'000)
        b = 1;
    else if (us < 10'000)
        b = 2;
    else if (us < 100'000)
        b = 3;
    g_mf_mailbox_stats.mailbox_under_boundary_wait_hist[b].fetch_add(1, std::memory_order_relaxed);
    if (dropped)
        g_mf_mailbox_stats.mailbox_under_boundary_wait_drop_total.fetch_add(
            1, std::memory_order_relaxed);
    // Refresh p50/p99 edge approximations from the 5-bucket hist.
    // Edges for bucket upper bounds (last is open-ended → use max-so-far).
    static constexpr std::uint64_t kEdges[5] = {100, 1'000, 10'000, 100'000, 0};
    std::uint64_t counts[5];
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        counts[i] =
            g_mf_mailbox_stats.mailbox_under_boundary_wait_hist[i].load(std::memory_order_relaxed);
        total += counts[i];
    }
    if (total == 0)
        return;
    const auto target50 = (total + 1) / 2;         // ceil
    const auto target99 = (total * 99 + 99) / 100; // ceil 99%
    std::uint64_t cum = 0;
    std::uint64_t p50 = 0;
    std::uint64_t p99 = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        cum += counts[i];
        const auto edge = (i < 4) ? kEdges[i]
                                  : g_mf_mailbox_stats.mailbox_under_boundary_wait_us_max.load(
                                        std::memory_order_relaxed);
        if (p50 == 0 && cum >= target50)
            p50 = edge;
        if (p99 == 0 && cum >= target99)
            p99 = edge;
        if (p50 != 0 && p99 != 0)
            break;
    }
    if (p50 == 0)
        p50 = g_mf_mailbox_stats.mailbox_under_boundary_wait_us_max.load(std::memory_order_relaxed);
    if (p99 == 0)
        p99 = p50;
    g_mf_mailbox_stats.mailbox_under_boundary_wait_us_p50.store(p50, std::memory_order_relaxed);
    g_mf_mailbox_stats.mailbox_under_boundary_wait_us_p99.store(p99, std::memory_order_relaxed);
    // Issue #2958: after hist refresh, production may cancel live holder.
    maybe_mailbox_defer_slo_hold_cancel();
}

// Issue #2958: under-boundary wait / open-window age ≥ SLO → request
// hold-budget cancel on the live outermost holder (production defaults).
// Soft: breach_observe + soft_observe only. Under-SLO / no open window:
// early return after a few relaxed loads (zero cancel work).
// One-shot armed flag (cleared on defer window close) avoids cancel storms;
// Fiber::consume_hold_budget_cancel remains the one-shot CAS on the holder.
inline void maybe_mailbox_defer_slo_hold_cancel() noexcept {
    // Issue #3002: same live p99 + throttle + SLO as #2947 (no hist walk).
    std::uint64_t p99 = 0;
    std::uint64_t slo = 0;
    bool throttled = false;
    sample_mailbox_hold_slo_live(p99, throttled, slo);
    const auto mx =
        g_mf_mailbox_stats.mailbox_under_boundary_wait_us_max.load(std::memory_order_relaxed);
    bool wait_hot = mailbox_hold_slo_live_signal(p99, slo, throttled) || (slo != 0 && mx >= slo);
    if (!wait_hot) {
        // Mid-hold path: open defer window age may already exceed SLO
        // before any deliver sample (long Guard still live).
        if (slo == 0)
            return;
        const auto first = g_mailbox_first_open_defer_ns.load(std::memory_order_relaxed);
        if (first == 0)
            return; // no open defer — zero cancel work
        const auto now = mailbox_steady_ns();
        if (now < first)
            return;
        const auto age_us = (now - first) / 1000ull;
        if (age_us < slo)
            return;
        wait_hot = true;
    }
    (void)wait_hot;
    g_mf_mailbox_stats.mailbox_defer_slo_breach_observe_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
    // Soft / non-production: observe only (AC2).
    if (aura_production_defaults_active_probe() == 0) {
        g_mf_mailbox_stats.mailbox_defer_slo_soft_observe_total.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }
    // One-shot arm for this open-defer window (AC3).
    std::uint8_t expected = 0;
    if (!g_mailbox_defer_slo_hold_cancel_armed.compare_exchange_strong(
            expected, 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        // Issue #3289 (I5 residual): already armed for this open-defer
        // window — keep driving the holder. The arm-time poll above sees
        // elapsed≈0, so the #3285 1×SLO synthetic-edge tier and the 2×SLO
        // hard bound cannot fire yet; a non-cooperative holder that never
        // reaches its own cooperative edge would otherwise never be
        // re-polled (the scheduler idle-tick poll is not a hard progress
        // bound under load). Re-poll the hold-budget in-body window here
        // so the holder is driven to the same force-release as hold-budget
        // overtime (depth 0 + unlocked + dual restore) as the cancel arm
        // ages. Soft / sandbox=off already returned above (observe-only);
        // no new counters — reuses the existing force path (#3285 / #3035).
        (void)aura_hold_budget_poll_inbody_window();
        return;
    }
    const auto holder = aura::compiler::mutation_hold_live_snapshot();
    if (holder.fiber_id == 0) {
        g_mf_mailbox_stats.mailbox_defer_slo_no_holder_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
        // Leave armed for this open-defer window (AC3: no counter storm).
        // Cleared when window closes (1→0).
        return;
    }
    // Issue #3256: unify mailbox under-boundary SLO with mutation
    // hold-budget. Order: mailbox SLO (this helper) → hold arm
    // (force_degrade on g_mutation_hold_live_fiber_id: cancel +
    // force-safepoint + urgent inbody poll; reuse holder_degrade_* /
    // cancel_fired) → existing force path (poll_inbody_window /
    // #3254). Do not invent a second unlock path. Delivery still
    // returns Backpressure (caller). Soft already returned above.
    aura_evaluator_force_degrade_outermost_holder(holder.fiber_id);
    const int armed = aura_fiber_request_hold_budget_cancel(holder.fiber_id);
    (void)aura_hold_budget_poll_inbody_window();
    if (armed != 0) {
        g_mf_mailbox_stats.mailbox_defer_slo_hold_cancel_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
    } else {
        // Registry miss — one no_holder note; stay armed until window close.
        g_mf_mailbox_stats.mailbox_defer_slo_no_holder_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
    }
}

// Issue #2378: successful enqueue after possible open defer window.
// AC3: when deferred_depth==0, single relaxed load then return (no maps).
// Issue #2903: when window closes (1→0), also sample under-boundary wait
// from first defer decision → deliver (full hold-visible latency).
inline void note_mailbox_push_ok_drain_progress() noexcept {
    const auto depth = g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed);
    if (depth == 0)
        return; // happy path — zero extra work beyond this load
    // Resolve one outstanding defer (sender retry succeeded).
    auto cur = depth;
    while (cur > 0 && !g_mf_mailbox_stats.mailbox_deferred_depth.compare_exchange_weak(
                          cur, cur - 1, std::memory_order_relaxed)) {
    }
    if (cur == 0)
        return; // raced to zero
    // Flush latency sample when window closes (depth was 1 → 0).
    if (cur == 1) {
        // Issue #2554: window closed — clear starvation canary so the next
        // open-defer window can report once again.
        g_mailbox_defer_starve_reported.store(false, std::memory_order_relaxed);
        // Issue #2958: clear SLO hold-cancel arm for the next open window.
        g_mailbox_defer_slo_hold_cancel_armed.store(0, std::memory_order_relaxed);
        const auto now = mailbox_steady_ns();
        const auto first = g_mailbox_first_open_defer_ns.exchange(0, std::memory_order_relaxed);
        const auto exit_ns = g_mailbox_last_outermost_exit_ns.load(std::memory_order_relaxed);
        // Prefer exit→deliver when outermost exit was observed (AC2);
        // else first-open→deliver.
        std::uint64_t start = first;
        if (exit_ns != 0 && exit_ns >= first)
            start = exit_ns;
        if (start != 0 && now >= start) {
            const auto us = (now - start) / 1000ull;
            g_mf_mailbox_stats.mailbox_deferred_flush_latency_us_total.fetch_add(
                us, std::memory_order_relaxed);
            g_mf_mailbox_stats.mailbox_deferred_flush_samples.fetch_add(1,
                                                                        std::memory_order_relaxed);
            auto mx = g_mf_mailbox_stats.mailbox_deferred_flush_latency_us_max.load(
                std::memory_order_relaxed);
            while (us > mx &&
                   !g_mf_mailbox_stats.mailbox_deferred_flush_latency_us_max.compare_exchange_weak(
                       mx, us, std::memory_order_relaxed)) {
            }
        }
        // Issue #2903 AC1: full under-boundary wait = first defer → deliver.
        // Distinct from #2378 flush latency (exit→deliver preferred): Agents
        // need the long-hold wait visible even when exit is recent.
        if (first != 0 && now >= first) {
            const auto wait_us = (now - first) / 1000ull;
            note_mailbox_under_boundary_wait_sample(wait_us, /*dropped=*/false);
        }
        // Issue #2551 AC3: window closed → clear Agent throttle.
        clear_agent_throttle_for_mailbox_starvation();
    }
}

// Issue #2378: outermost Guard exit — drain opportunity + starvation canary.
// Called from MutationBoundaryGuard dtor (Phase 5 unlock) next to #2347
// window clear. Soft / no open defer: just stamps exit_ns (one store).
// Issue #2511 wraps this inside drain_deferred_under_budget (still public
// for tests that call the note directly).
inline void note_mailbox_outermost_exit_drain() noexcept {
    const auto now = mailbox_steady_ns();
    g_mailbox_last_outermost_exit_ns.store(now, std::memory_order_relaxed);
    const auto depth = g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed);
    if (depth == 0)
        return;
    g_mf_mailbox_stats.mailbox_deferred_drain_opportunity_total.fetch_add(
        1, std::memory_order_relaxed);
    // Starvation: open defer window older than threshold after an exit.
    const auto thr_ms = mailbox_defer_starvation_ms();
    if (thr_ms == 0)
        return;
    const auto first = g_mailbox_first_open_defer_ns.load(std::memory_order_relaxed);
    if (first == 0)
        return;
    const auto age_ms = (now > first) ? (now - first) / 1'000'000ull : 0;
    if (age_ms >= thr_ms &&
        !g_mailbox_defer_starve_reported.exchange(true, std::memory_order_relaxed)) {
        // Issue #2554: dedupe — bump once per open-defer window (not per drain).
        g_mf_mailbox_stats.mailbox_defer_starvation_total.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #2958: long open defer at outermost exit → holder cancel probe
    // (complements #2947 gate deny while Guard is still unwinding).
    maybe_mailbox_defer_slo_hold_cancel();
}

// Issue #2511: hold-exit drain budget (µs). Default 1000 µs.
// Override: AURA_MAILBOX_HOLD_DRAIN_BUDGET_US (0 = no spin, immediate
// starved/force path when depth open).
[[nodiscard]] inline std::uint64_t mailbox_hold_drain_budget_us() noexcept {
    static const std::uint64_t cached = []() noexcept -> std::uint64_t {
        const char* e = std::getenv("AURA_MAILBOX_HOLD_DRAIN_BUDGET_US");
        if (e == nullptr || e[0] == '\0')
            return 1000ull;
        std::uint64_t v = 0;
        for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
            v = v * 10 + static_cast<std::uint64_t>(*p - '0');
        return v; // 0 is intentional (immediate deadline)
    }();
    return cached;
}

// Issue #2511: result of budgeted hold-exit deferred drain.
// Messages deferred under #2312 are not queued (BP to sender); drain closes
// the open SLA window once hold is released, waits under budget for concurrent
// retries to resolve depth, and optionally force-resolves under Strict.
struct MailboxHoldDrainResult {
    std::uint64_t remaining_depth = 0;
    std::uint64_t elapsed_us = 0;
    std::uint64_t force_resolved = 0;
    bool starved = false;
    bool had_open_defer = false; // depth > 0 at entry
};

// Issue #2511 / #2551: outermost Guard exit forced deferred drain under budget.
// AC5: when deferred_depth==0, single relaxed load then return (no maps,
// no spin). Always stamps last_outermost_exit_ns when depth was open (via
// note_mailbox_outermost_exit_drain). Soft: retain open depth + starvation
// bump when budget exhausted. Strict/production: force-resolve remaining
// depth (accounting close — senders already got BP and will retry) + audit.
// Issue #2551: production/Strict residual after budget → hard counter +
// Agent throttle flag (query-visible). Soft residual: metric-only. Flag
// clears on free path (depth0 entry) or window close (1→0 push Ok).
[[nodiscard]] inline MailboxHoldDrainResult
drain_deferred_under_budget(std::uint64_t budget_us = 0) noexcept {
    MailboxHoldDrainResult r;
    // AC5 / #2551 AC2: zero extra work when no pending defer (one relaxed load).
    const auto depth0 = g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed);
    if (depth0 == 0) {
        // Still stamp exit_ns so a later open-window flush can baseline
        // against this outermost exit (cheap store; not a depth walk).
        g_mailbox_last_outermost_exit_ns.store(mailbox_steady_ns(), std::memory_order_relaxed);
        // Issue #2551 AC3: subsequent free drain (depth already zero) clears
        // Agent throttle — mailbox has caught up.
        clear_agent_throttle_for_mailbox_starvation();
        return r;
    }
    r.had_open_defer = true;
    g_mf_mailbox_stats.mailbox_hold_exit_drain_total.fetch_add(1, std::memory_order_relaxed);
    // #2378 opportunity + age starvation canary (unchanged semantics).
    note_mailbox_outermost_exit_drain();

    const auto budget = budget_us != 0 ? budget_us : mailbox_hold_drain_budget_us();
    const auto start_ns = mailbox_steady_ns();

    // Wait under budget for concurrent push Ok to close the window.
    // #2312 does not enqueue deferred messages — depth falls when senders
    // retry after hold release (now true: Guard unlocked before this call
    // or concurrently). Spin is deliberately light (yield, no sleep).
    for (;;) {
        const auto d = g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed);
        const auto now = mailbox_steady_ns();
        const auto elapsed_us = (now > start_ns) ? (now - start_ns) / 1000ull : 0ull;
        if (d == 0) {
            r.remaining_depth = 0;
            r.elapsed_us = elapsed_us;
            // Natural resolve under budget → clear throttle (#2551 AC3).
            clear_agent_throttle_for_mailbox_starvation();
            break;
        }
        if (elapsed_us >= budget) {
            r.remaining_depth = d;
            r.elapsed_us = elapsed_us;
            // Issue #2849 / #2551: deferred_depth is BP accounting (messages
            // were never enqueued under #2312/#2680/#2849). Always force-close
            // the accounting depth after budget so Soft multi-eval concurrent
            // mutates do not false-positive starvation (chaos PR
            // AURA_CHAOS_MB_STARVE_MAX=0). Production/Strict still bumps the
            // hard residual face + Agent throttle (#2551).
            // Inline Strict probe — is_mutate_mailbox_strict is defined later.
            const char* strict_e = std::getenv("AURA_MUTATE_MAILBOX_STRICT");
            const bool hard_resolve =
                (strict_e && strict_e[0] == '1') || aura_production_defaults_active_probe() != 0;
            {
                std::uint64_t resolved = 0;
                std::uint64_t guard = d + 8;
                // Issue #2903: sample under-boundary wait once for budget
                // force-drop (defer decision → drop) before clearing stamp.
                const auto drop_now = mailbox_steady_ns();
                const auto drop_first =
                    g_mailbox_first_open_defer_ns.load(std::memory_order_relaxed);
                while (g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed) >
                           0 &&
                       guard-- > 0) {
                    // Force-resolve uses depth CAS only — do not clear
                    // throttle via push_ok window close (would race AC1).
                    auto cur =
                        g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed);
                    while (cur > 0 &&
                           !g_mf_mailbox_stats.mailbox_deferred_depth.compare_exchange_weak(
                               cur, cur - 1, std::memory_order_relaxed)) {
                    }
                    if (cur == 0)
                        break;
                    if (cur == 1) {
                        (void)g_mailbox_first_open_defer_ns.exchange(0, std::memory_order_relaxed);
                        // Issue #2554: window closed via force-resolve — clear
                        // canary so the next open-defer window reports once.
                        g_mailbox_defer_starve_reported.store(false, std::memory_order_relaxed);
                    }
                    ++resolved;
                }
                r.force_resolved = resolved;
                r.remaining_depth =
                    g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed);
                if (resolved > 0) {
                    g_mf_mailbox_stats.mailbox_hold_exit_force_resolved_total.fetch_add(
                        resolved, std::memory_order_relaxed);
                    // #2903 AC1 drop path: wait from first defer → budget drop.
                    if (drop_first != 0 && drop_now >= drop_first) {
                        note_mailbox_under_boundary_wait_sample((drop_now - drop_first) / 1000ull,
                                                                /*dropped=*/true);
                    }
                }
            }
            // Issue #2551 AC1: production/Strict residual after budget →
            // hard counter + Agent throttle (even when force-close zeros
            // depth — subsequent free drain clears the flag).
            if (hard_resolve) {
                r.starved = true;
                g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.fetch_add(
                    1, std::memory_order_relaxed);
                g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.store(
                    1, std::memory_order_relaxed);
                g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.fetch_add(
                    1, std::memory_order_relaxed);
                g_mf_mailbox_stats.mailbox_defer_starvation_total.fetch_add(
                    1, std::memory_order_relaxed);
            } else {
                // Soft: accounting force-closed; not starvation (mid-mutation
                // gate already prevented enqueue). Clear throttle if depth 0.
                r.starved = false;
                if (r.remaining_depth == 0)
                    clear_agent_throttle_for_mailbox_starvation();
            }
            break;
        }
        // Light pause so concurrent sender retries can run.
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#else
        std::this_thread::yield();
#endif
    }

    // Elapsed accounting (always when had_open_defer).
    if (r.elapsed_us == 0) {
        const auto now = mailbox_steady_ns();
        r.elapsed_us = (now > start_ns) ? (now - start_ns) / 1000ull : 0ull;
    }
    g_mf_mailbox_stats.mailbox_hold_exit_drain_us_total.fetch_add(r.elapsed_us,
                                                                  std::memory_order_relaxed);
    auto mx = g_mf_mailbox_stats.mailbox_hold_exit_drain_us_max.load(std::memory_order_relaxed);
    while (r.elapsed_us > mx &&
           !g_mf_mailbox_stats.mailbox_hold_exit_drain_us_max.compare_exchange_weak(
               mx, r.elapsed_us, std::memory_order_relaxed)) {
    }
    return r;
}

// Issue #2347: rejects in the current outermost Guard window (TLS).
// Cleared on outermost Guard exit so multi-round mutates do not carry
// a stale window. Soft path still increments this for optional dashboards.
inline thread_local std::uint64_t g_recv_boundary_reject_window{0};

inline void clear_recv_boundary_reject_window() noexcept {
    g_recv_boundary_reject_window = 0;
}

// Issue #2347: Strict / production opt-in for hard audit + threshold.
// Soft (default): Policy A soft counter only.
// Strict: AURA_MUTATE_MAILBOX_STRICT=1 OR production_defaults canary.
[[nodiscard]] inline bool is_mutate_mailbox_strict() noexcept {
    const char* e = std::getenv("AURA_MUTATE_MAILBOX_STRICT");
    if (e && e[0] == '1')
        return true;
    return aura_production_defaults_active_probe() != 0;
}

// Issue #3036: production residual hard-AND cannot Soft-escape.
// Explicit AURA_SANDBOX=off stays observe-only. Otherwise production
// defaults (probe) or MUTATE_MAILBOX_STRICT force hard deny.
[[nodiscard]] inline bool mailbox_sandbox_explicit_off() noexcept {
    const char* e = std::getenv("AURA_SANDBOX");
    return e && e[0] == 'o' && e[1] == 'f' && e[2] == 'f' && e[3] == '\0';
}
[[nodiscard]] inline bool mailbox_residual_hard_enabled() noexcept {
    if (mailbox_sandbox_explicit_off())
        return false;
    return is_mutate_mailbox_strict() || aura_production_defaults_active_probe() != 0;
}

// Issue #2849: sole helper for shared-Evaluator mid-mutation delivery gate
// (closes #2680 residual as production fail-closed proof). When the
// outermost MutationBoundary is live (depth>0 || held) on the shared
// Evaluator, returns true so push / broadcast_fanout ALWAYS return
// Backpressure — never enqueue a payload that could observe mid-mutation
// state (StableNodeRef / EnvFrame views stamped under Guard). Soft still
// defers (no silent drop) and bumps soft_observe; production/Strict bumps
// hard. Phase-5 outermost Guard dtor remains the sole place that reopens
// the deliverability window (clear_recv_boundary_reject_window +
// drain_deferred_under_budget). Zero cost when boundary idle (two relaxed
// loads + branch).
//
// Callers MUST:
//   if (note_mailbox_deferred_under_boundary(&local_stats_))
//       return PushStatus::Backpressure;
[[nodiscard]] inline bool
note_mailbox_deferred_under_boundary(MultiFiberMailboxStats* local_stats = nullptr) noexcept {
    // Production defaults: push under live boundary always Backpressure.
    if (!(aura_evaluator_mutation_boundary_depth() > 0 ||
          aura_evaluator_mutation_boundary_held() != 0))
        return false;
    g_mf_mailbox_stats.mailbox_shared_evaluator_deferred_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
    g_mf_mailbox_stats.mailbox_under_boundary_deferred_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
    if (local_stats) {
        local_stats->mailbox_shared_evaluator_deferred_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
        local_stats->mailbox_under_boundary_deferred_total.fetch_add(1, std::memory_order_relaxed);
    }
    if (is_mutate_mailbox_strict()) {
        g_mf_mailbox_stats.mailbox_shared_evaluator_deferred_hard_total.fetch_add(
            1, std::memory_order_relaxed);
        g_mf_mailbox_stats.mailbox_under_boundary_deferred_hard_total.fetch_add(
            1, std::memory_order_relaxed);
        if (local_stats) {
            local_stats->mailbox_shared_evaluator_deferred_hard_total.fetch_add(
                1, std::memory_order_relaxed);
            local_stats->mailbox_under_boundary_deferred_hard_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    } else {
        // Soft / sandbox=off: metric-only face of the same defer (still BP —
        // never weakens the gate to allow mid-mutation delivery).
        g_mf_mailbox_stats.mailbox_shared_evaluator_deferred_soft_observe_total.fetch_add(
            1, std::memory_order_relaxed);
        g_mf_mailbox_stats.mailbox_under_boundary_deferred_soft_observe_total.fetch_add(
            1, std::memory_order_relaxed);
        if (local_stats) {
            local_stats->mailbox_shared_evaluator_deferred_soft_observe_total.fetch_add(
                1, std::memory_order_relaxed);
            local_stats->mailbox_under_boundary_deferred_soft_observe_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    note_mailbox_mutation_hold_defer();
    // Issue #2958: concurrent push under long hold may already exceed wait
    // SLO by open-window age — request holder cancel under production.
    maybe_mailbox_defer_slo_hold_cancel();
    return true;
}

// Issue #2987: residual hard-AND after the #2849 shared-Evaluator
// boundary check. Steal and mailbox share StealInvariant definitions
// (evaluate_residual_hard_and_bits / mailbox_delivery_safety_transaction).
// RejectHard → Backpressure + counters; never enqueue. Soft still BP
// (soft_observe). Issue #3036: production_defaults + !sandbox=off
// always bumps mailbox_residual_hard_reject_total (no Soft escape).
// Happy path: inject==None and no target fiber → thread_local only.
// Callers MUST:
//   if (note_mailbox_delivery_safety(...))
//       return PushStatus::Backpressure;
[[nodiscard]] inline bool
note_mailbox_delivery_safety(Fiber* target, const MutationSafetySnapshot* snap, bool check_envframe,
                             MultiFiberMailboxStats* local_stats = nullptr) noexcept {
    using aura::serve::g_mailbox_delivery_inject;
    using aura::serve::mailbox_delivery_safety_transaction;
    using aura::serve::MailboxDeliveryInject;
    using aura::serve::steal_invariant_mask;
    using aura::serve::StealInvariant;
    using aura::serve::StealSafetyDecision;
    // AC3: happy path — no inject, no target. Single thread_local load.
    if (g_mailbox_delivery_inject == MailboxDeliveryInject::None && target == nullptr)
        return false;
    const auto r = mailbox_delivery_safety_transaction(target, snap, check_envframe);
    if (r.decision != StealSafetyDecision::RejectHard)
        return false;
    if (r.fail_bits & steal_invariant_mask(StealInvariant::LayoutStampMatch)) {
        g_mf_mailbox_stats.mailbox_delivery_reject_layout_stamp_total.fetch_add(
            1, std::memory_order_relaxed);
        if (local_stats)
            local_stats->mailbox_delivery_reject_layout_stamp_total.fetch_add(
                1, std::memory_order_relaxed);
    }
    if (r.fail_bits & steal_invariant_mask(StealInvariant::TicketFresh)) {
        g_mf_mailbox_stats.mailbox_delivery_reject_ticket_stale_total.fetch_add(
            1, std::memory_order_relaxed);
        if (local_stats)
            local_stats->mailbox_delivery_reject_ticket_stale_total.fetch_add(
                1, std::memory_order_relaxed);
    }
    if (r.fail_bits & (steal_invariant_mask(StealInvariant::GcDeferClear) |
                       steal_invariant_mask(StealInvariant::EnvFrameOk) |
                       steal_invariant_mask(StealInvariant::BoundarySafe))) {
        g_mf_mailbox_stats.mailbox_delivery_reject_residual_total.fetch_add(
            1, std::memory_order_relaxed);
        if (local_stats)
            local_stats->mailbox_delivery_reject_residual_total.fetch_add(
                1, std::memory_order_relaxed);
    }
    // Issue #3036: production_defaults + !sandbox=off → hard deny counter
    // (no Soft escape via is_mutate_mailbox_strict leak). Soft / sandbox=off
    // stay observe-only. RejectHard always returns true → Backpressure.
    if (mailbox_residual_hard_enabled()) {
        g_mf_mailbox_stats.mailbox_residual_hard_reject_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
        g_mf_mailbox_stats.mailbox_delivery_reject_hard_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
        if (local_stats) {
            local_stats->mailbox_residual_hard_reject_total.fetch_add(1, std::memory_order_relaxed);
            local_stats->mailbox_delivery_reject_hard_total.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        g_mf_mailbox_stats.mailbox_delivery_reject_soft_observe_total.fetch_add(
            1, std::memory_order_relaxed);
        if (local_stats)
            local_stats->mailbox_delivery_reject_soft_observe_total.fetch_add(
                1, std::memory_order_relaxed);
    }
    return true;
}

// Default N=8 rejects in one Guard window → force mark-failed under Strict.
// Override: AURA_MUTATE_MAILBOX_REJECT_THRESHOLD=<N> (0 disables threshold).
[[nodiscard]] inline std::uint64_t mutate_mailbox_reject_threshold() noexcept {
    const char* e = std::getenv("AURA_MUTATE_MAILBOX_REJECT_THRESHOLD");
    if (e == nullptr || e[0] == '\0')
        return 8;
    std::uint64_t v = 0;
    for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
        v = v * 10 + static_cast<std::uint64_t>(*p - '0');
    return v; // 0 = disable force-rollback threshold
}

// ── Hot-path helpers (no Evaluator / GC / provenance) ──
[[nodiscard]] inline bool is_linear_viol_payload(std::string_view payload) noexcept {
    return payload.size() >= kLinearViolPrefix.size() &&
           payload.compare(0, kLinearViolPrefix.size(), kLinearViolPrefix) == 0;
}

// Bump linear_checks; if payload is linear-viol:, bump violations and return true
// (caller should return Closed without locking).
[[nodiscard]] inline bool reject_if_linear_viol(std::string_view payload) noexcept {
    g_mf_mailbox_stats.linear_checks.fetch_add(1, std::memory_order_relaxed);
    if (!is_linear_viol_payload(payload))
        return false;
    g_mf_mailbox_stats.linear_violations.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Backpressure accounting: process + local + optional orch dashboard mirror.
inline void note_backpressure(MultiFiberMailboxStats* local = nullptr,
                              bool from_fanout = false) noexcept {
    g_mf_mailbox_stats.backpressure_rejects.fetch_add(1, std::memory_order_relaxed);
    if (local)
        local->backpressure_rejects.fetch_add(1, std::memory_order_relaxed);
    if (from_fanout) {
        g_mf_mailbox_stats.fanout_backpressure_rejects.fetch_add(1, std::memory_order_relaxed);
        if (local)
            local->fanout_backpressure_rejects.fetch_add(1, std::memory_order_relaxed);
    }
    aura_orch_note_mailbox_backpressure();
}

// Issue #2972: credit-BP metric (process + local). Caller still invokes
// note_backpressure so recent-admit + producer throttle see one real BP.
inline void note_credit_backpressure(MultiFiberMailboxStats* local = nullptr) noexcept {
    g_mf_mailbox_stats.mailbox_credit_bp_total.fetch_add(1, std::memory_order_relaxed);
    if (local)
        local->mailbox_credit_bp_total.fetch_add(1, std::memory_order_relaxed);
}

inline void note_inflight_hwm(std::uint64_t v, MultiFiberMailboxStats* local = nullptr) noexcept {
    auto hwm = g_mf_mailbox_stats.mailbox_inflight_hwm.load(std::memory_order_relaxed);
    while (v > hwm && !g_mf_mailbox_stats.mailbox_inflight_hwm.compare_exchange_weak(
                          hwm, v, std::memory_order_relaxed)) {
    }
    if (local) {
        auto lhwm = local->mailbox_inflight_hwm.load(std::memory_order_relaxed);
        while (v > lhwm && !local->mailbox_inflight_hwm.compare_exchange_weak(
                               lhwm, v, std::memory_order_relaxed)) {
        }
    }
}

// Multi-fiber mailbox: many attachers, priority queue, broadcast wake,
// high-water backpressure.
class MultiFiberMailbox {
public:
    explicit MultiFiberMailbox(std::size_t high_water = 1024,
                               std::uint32_t credit_limit = 0) noexcept
        : high_water_(high_water == 0 ? 1 : high_water)
        , credit_limit_(credit_limit) {}

    ~MultiFiberMailbox() { close(); }

    void set_high_water(std::size_t n) noexcept { high_water_ = n == 0 ? 1 : n; }
    [[nodiscard]] std::size_t high_water() const noexcept { return high_water_; }
    // Issue #2972: 0 = use high_water (default). Hosts set a tighter
    // credit so a slow consumer stops producers before the memory bound.
    void set_credit_limit(std::uint32_t n) noexcept { credit_limit_ = n; }
    [[nodiscard]] std::uint32_t credit_limit() const noexcept { return credit_limit_; }
    [[nodiscard]] std::size_t effective_credit() const noexcept {
        return credit_limit_ == 0 ? high_water_ : static_cast<std::size_t>(credit_limit_);
    }
    [[nodiscard]] std::uint64_t inflight() const noexcept {
        return inflight_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }
    void close() noexcept {
        // Issue #2972 AC2: drop queued messages and zero inflight so
        // close cannot leave a permanent credit-full mailbox.
        bool expected = false;
        if (!closed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            return;
        }
        (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::Mailbox,
                                                       __builtin_FILE(), __builtin_LINE());
        std::lock_guard lock(mu_);
        drop_queued_unlocked_();
        notify_all_unlocked();
    }

    // Multi-attach: multiple fibers may wait on this mailbox.
    // priority is reserved for future fair scheduling (stored for stats).
    void attach(Fiber* f, int /*priority*/ = 0) {
        if (!f)
            return;
        // Issue #2316: wire mu_ acquire to lock_order audit.
        (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::Mailbox,
                                                       __builtin_FILE(), __builtin_LINE());
        std::lock_guard lock(mu_);
        for (auto* a : attachers_) {
            if (a == f)
                return;
        }
        attachers_.push_back(f);
        g_mf_mailbox_stats.attaches.fetch_add(1, std::memory_order_relaxed);
        // Issue #3369: back-pointer set when this fiber is bound to a
        // mailbox for the first time. Only set if the fiber doesn't
        // already have a primary mailbox (one fiber ↔ one primary
        // mailbox — broadcast attaches leave the existing primary
        // pointer intact). Detach() clears it when this mailbox is
        // unbound.
        if (f->mailbox() == nullptr)
            f->set_mailbox(this);
    }

    void detach(Fiber* f) {
        if (!f)
            return;
        (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::Mailbox,
                                                       __builtin_FILE(), __builtin_LINE());
        std::lock_guard lock(mu_);
        attachers_.erase(std::remove(attachers_.begin(), attachers_.end(), f), attachers_.end());
        // Issue #3369: clear the back-pointer iff this mailbox was the
        // primary (avoid clobbering a different mailbox's pointer when a
        // fiber is bound to multiple mailboxes via broadcast).
        if (f->mailbox() == this)
            f->set_mailbox(nullptr);
    }

    [[nodiscard]] std::size_t attacher_count() const {
        (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::Mailbox,
                                                       __builtin_FILE(), __builtin_LINE());
        std::lock_guard lock(mu_);
        return attachers_.size();
    }

    // Issue #3369: walk pending held_ref messages for a fiber under lock.
    // Iterates queue_ and applies fn to each pending message addressed to
    // this fiber (to_fiber == fiber->id() OR to_fiber == 0 broadcast) that
    // carries held_ref_token. Caller is expected to bump
    // held_ref_stale_after_steal_total and / or clear handoff_completed
    // per stale message inside fn. The walk runs under mu_ — callers must
    // keep fn short and lock-free. Used by the steal-complete strong def
    // in evaluator_fiber_mutation.cpp to honor #3111 AC1: production
    // re-validate clears handoff_completed on potentially stale held_ref
    // messages so the consumer's recv() path is forced through the
    // handoff gate instead of silently delivering a stale StableNodeRef.
    template <typename Fn> void for_each_pending_held_ref_for_fiber(Fiber* fiber, Fn&& fn) {
        if (!fiber)
            return;
        (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::Mailbox,
                                                       __builtin_FILE(), __builtin_LINE());
        std::lock_guard lock(mu_);
        const auto fid = fiber->id();
        for (auto& msg : queue_) {
            if (!msg.held_ref_token.has_value())
                continue;
            if (msg.to_fiber != 0 && msg.to_fiber != fid)
                continue;
            fn(msg);
        }
    }

    [[nodiscard]] std::size_t size() const {
        (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::Mailbox,
                                                       __builtin_FILE(), __builtin_LINE());
        std::lock_guard lock(mu_);
        return queue_.size();
    }
    [[nodiscard]] bool empty() const {
        (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::Mailbox,
                                                       __builtin_FILE(), __builtin_LINE());
        std::lock_guard lock(mu_);
        return queue_.empty();
    }

    // Push with backpressure: when size >= high_water, reject.
    // Issue #1595 / #2010: linear-viol: filter runs first (before lock), pure
    // string prefix — fiber-stack safe, no Evaluator/GC/provenance on hot path.
    // Deeper StableNodeRef/linear probe is via host/post-join paths only.
    [[nodiscard]] PushStatus push(MailMessage msg) {
        if (reject_if_linear_viol(msg.payload))
            return PushStatus::Closed;
        // Issue #2663 / Issue #3212: held-ref gate. If held_ref_token is set but
        // handoff_completed is false, reject the push (HandoffRequired + bump
        // counter). Aligns with agent_send / agent_send_safe typed fail.
        // Closed reserved for true closed / linear-viol. Zero cost when
        // held_ref_token is empty (ordinary string payloads never set this
        // — single relaxed load + bool).
        if (msg.held_ref_token.has_value() && !msg.handoff_completed) {
            g_mf_mailbox_stats.handoff_reject_total.fetch_add(1, std::memory_order_relaxed);
            local_stats_.handoff_reject_total.fetch_add(1, std::memory_order_relaxed);
            return PushStatus::HandoffRequired;
        }
        (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::Mailbox,
                                                       __builtin_FILE(), __builtin_LINE());
        std::lock_guard lock(mu_);
        if (closed_.load(std::memory_order_relaxed))
            return PushStatus::Closed;
        // Issue #2312: delivery gate — when msg.to_fiber resolves to an
        // attached fiber, gate on its MutationSafetySnapshot. If the
        // target is NOT at a mutation-boundary-safe point, defer (BP)
        // rather than delivering mutate-triggering work to a fiber
        // holding Guard (lock-order inversion vs workspace_mtx_,
        // silent partial updates under multi-agent fanout). Reuses
        // #2184 snapshot + is_at_mutation_boundary_safe truth table.
        // Per AC3: zero cost when target is safe (one snapshot load).
        // Per AC2: deferred = not dropped; the sender retries / queues
        // and the target becomes deliverable after outermost Guard exit.
        Fiber* target = nullptr;
        MutationSafetySnapshot snap{};
        if (msg.to_fiber != 0) {
            for (auto* a : attachers_) {
                if (a && a->id() == msg.to_fiber) {
                    target = a;
                    snap = a->mutation_safety_snapshot();
                    if (!a->is_at_mutation_boundary_safe(snap)) {
                        // Issue #2312 defer + #2378 depth/SLA (not dropped).
                        note_mailbox_mutation_hold_defer();
                        local_stats_.mailbox_deferred_mutation_hold_total.fetch_add(
                            1, std::memory_order_relaxed);
                        return PushStatus::Backpressure;
                    }
                    break;
                }
            }
        }
        // Issue #2680 / #2849: shared-Evaluator mid-mutation delivery gate.
        // If the shared Evaluator's MutationBoundary is held (depth>0 || held)
        // by ANY fiber, defer (BP) — never enqueue a payload that could
        // observe mid-mutation state. Same authority as steal safety
        // (aura_evaluator_mutation_boundary_held / depth). Production
        // fail-closed via note_mailbox_deferred_under_boundary (always BP;
        // Soft soft_observe / production hard counters). Phase-5 outermost
        // Guard dtor is the sole reopen of the deliverability window.
        if (note_mailbox_deferred_under_boundary(&local_stats_))
            return PushStatus::Backpressure;
        // Issue #2987: residual hard-AND (LayoutStamp / Ticket / GcDefer)
        // even when depth/held snapshot looks safe. Inject / target residual
        // → RejectHard → Backpressure; never enqueue. Happy path (no
        // inject, no target) is thread_local only.
        if (note_mailbox_delivery_safety(target, target ? &snap : nullptr,
                                         msg.held_ref_token.has_value(), &local_stats_))
            return PushStatus::Backpressure;
        // Issue #2972: inflight credit gate (complement high_water memory
        // bound + process/scope recent admit). Same note_backpressure so
        // #2535 admit + #2925 consecutive throttle see one real BP.
        if (inflight_.load(std::memory_order_relaxed) >= effective_credit()) {
            note_credit_backpressure(&local_stats_);
            note_backpressure(&local_stats_, /*from_fanout=*/false);
            return PushStatus::Backpressure;
        }
        if (queue_.size() >= high_water_) {
            note_backpressure(&local_stats_, /*from_fanout=*/false);
            return PushStatus::Backpressure;
        }
        g_mf_mailbox_stats.pushes.fetch_add(1, std::memory_order_relaxed);
        local_stats_.pushes.fetch_add(1, std::memory_order_relaxed); // #1881: was dead
        if (msg.priority >= MailPriority::High)
            g_mf_mailbox_stats.priority_high.fetch_add(1, std::memory_order_relaxed);
        if (msg.to_fiber == 0)
            g_mf_mailbox_stats.broadcasts.fetch_add(1, std::memory_order_relaxed);
        // Critical/High go front; others back (stable within band).
        if (msg.priority >= MailPriority::High)
            queue_.push_front(std::move(msg));
        else
            queue_.push_back(std::move(msg));
        add_inflight_(1);
        // Issue #2378: successful enqueue may close an open defer window
        // (AC3: free when deferred_depth==0 — single relaxed load).
        note_mailbox_push_ok_drain_progress();
        notify_all_unlocked();
        return PushStatus::Ok;
    }

    // Broadcast: enqueue a copy for routing tag to_fiber=0 and wake all attachers.
    // Still a single queue message; all waiters compete via priority pop.
    // Semantics: one message, all waiters woken (first recv wins unless fan-out).
    // For true per-fiber fan-out, use broadcast_fanout.
    // Linear filter runs via push (shared entry-point guarantee, #2010).
    [[nodiscard]] PushStatus broadcast(MailMessage msg) {
        msg.to_fiber = 0;
        return push(std::move(msg));
    }

    // Fan-out: enqueue one message copy per attached fiber (to_fiber = fiber id).
    // Returns Backpressure if any push would overflow (none applied).
    // Issue #1881 / #2010: linear filter first; fanout BP mirrored to orch.
    [[nodiscard]] PushStatus broadcast_fanout(const MailMessage& proto) {
        if (reject_if_linear_viol(proto.payload))
            return PushStatus::Closed;
        // Issue #2663 / #3212: held-ref gate (mirror of push() — broadcast
        // fan-out cannot partial-deliver an unexported ref to a subset of
        // attachers; either all-or-nothing reject). production-safe default:
        // always HandoffRequired + counter bump; Soft / sandbox=off may
        // interpret the bump as metric-only via a future refinement. Closed
        // reserved for true closed / linear-viol.
        if (proto.held_ref_token.has_value() && !proto.handoff_completed) {
            g_mf_mailbox_stats.handoff_reject_total.fetch_add(1, std::memory_order_relaxed);
            local_stats_.handoff_reject_total.fetch_add(1, std::memory_order_relaxed);
            return PushStatus::HandoffRequired;
        }
        std::lock_guard lock(mu_);
        if (closed_.load(std::memory_order_relaxed))
            return PushStatus::Closed;
        // Issue #2680 / #2849: shared-Evaluator mid-mutation delivery gate
        // (fanout variant). Defer the ENTIRE fanout — never partial-deliver
        // while any fiber on the shared Evaluator is mid-mutation. Same
        // sole helper as push(); production fail-closed (always BP).
        if (note_mailbox_deferred_under_boundary(&local_stats_)) {
            note_backpressure(&local_stats_, /*from_fanout=*/true);
            return PushStatus::Backpressure;
        }
        // Issue #2987: inject residual (no target) still RejectHard the
        // entire fanout — never silent Ok.
        if (note_mailbox_delivery_safety(nullptr, nullptr, proto.held_ref_token.has_value(),
                                         &local_stats_)) {
            note_backpressure(&local_stats_, /*from_fanout=*/true);
            return PushStatus::Backpressure;
        }
        // Issue #2312: per-attached-fiber delivery gate. If ANY attached
        // fiber is unsafe (MutationBoundary held / depth>0), defer the
        // entire fan-out — don't partially deliver mutate-triggering
        // work to a subset while another target holds Guard. Mirrors
        // the per-to_fiber gate in push(). Reuses #2184 snapshot.
        for (auto* a : attachers_) {
            if (!a)
                continue;
            const auto snap = a->mutation_safety_snapshot();
            if (!a->is_at_mutation_boundary_safe(snap)) {
                // Issue #2312 / #2378: whole fan-out deferred (depth SLA).
                note_mailbox_mutation_hold_defer();
                local_stats_.mailbox_deferred_mutation_hold_total.fetch_add(
                    1, std::memory_order_relaxed);
                note_backpressure(&local_stats_, /*from_fanout=*/true);
                return PushStatus::Backpressure;
            }
            // Issue #2987: residual hard-AND per attacher (same table).
            if (note_mailbox_delivery_safety(a, &snap, proto.held_ref_token.has_value(),
                                             &local_stats_)) {
                note_backpressure(&local_stats_, /*from_fanout=*/true);
                return PushStatus::Backpressure;
            }
        }
        const auto need = attachers_.empty() ? std::size_t{1} : attachers_.size();
        // Issue #2972: reserve `need` credits before any enqueue (all-or-nothing).
        if (inflight_.load(std::memory_order_relaxed) + need > effective_credit()) {
            note_credit_backpressure(&local_stats_);
            note_backpressure(&local_stats_, /*from_fanout=*/true);
            return PushStatus::Backpressure;
        }
        if (queue_.size() + need > high_water_) {
            note_backpressure(&local_stats_, /*from_fanout=*/true);
            return PushStatus::Backpressure;
        }
        g_mf_mailbox_stats.broadcasts.fetch_add(1, std::memory_order_relaxed);
        g_mf_mailbox_stats.pushes.fetch_add(need, std::memory_order_relaxed);
        local_stats_.pushes.fetch_add(need, std::memory_order_relaxed);
        if (proto.priority >= MailPriority::High)
            g_mf_mailbox_stats.priority_high.fetch_add(need, std::memory_order_relaxed);

        if (attachers_.empty()) {
            MailMessage m = proto;
            m.to_fiber = 0;
            if (m.priority >= MailPriority::High)
                queue_.push_front(std::move(m));
            else
                queue_.push_back(std::move(m));
        } else {
            for (auto* f : attachers_) {
                MailMessage m = proto;
                m.to_fiber = f ? f->id() : 0;
                if (m.priority >= MailPriority::High)
                    queue_.push_front(std::move(m));
                else
                    queue_.push_back(std::move(m));
            }
        }
        add_inflight_(need);
        // Issue #2378: fan-out success may close open defer window.
        note_mailbox_push_ok_drain_progress();
        notify_all_unlocked();
        return PushStatus::Ok;
    }

    [[nodiscard]] bool try_pop(MailMessage& out) {
        std::lock_guard lock(mu_);
        if (!try_pop_unlocked(out, /*for_fiber=*/0))
            return false;
        // Issue #2592: deliver-side principal verify (TenantScope install
        // + mismatch bump). Re-call aura_fiber_install_tenant_scope_for_resume
        // from the receiving fiber; the hook is idempotent (no-op when
        // ambient already matches assigned_tenant_id) and bumps
        // Fiber::tenant_scope_mismatch_total when forged ambient is
        // detected (e.g., via cross-fiber closure apply that changed
        // capability_tenant_id_ without proper scope). Mailbox already
        // enqueued is NOT killed (additive over #2188 mutation-boundary
        // gate). Soft / sandbox=off path is permissive (hook no-op when
        // production sandbox inactive — see fiber.cpp #2491). AC1 / AC2
        // / AC4 verified by tests/orch/test_mailbox_tenant_principal.
        if (g_current_fiber != nullptr && g_current_fiber->assigned_tenant_id() != 0) {
            aura_fiber_install_tenant_scope_for_resume(g_current_fiber);
        }
        return true;
    }

    // Blocking recv for the current fiber (or host poll if no fiber).
    // timeout_ms < 0: wait forever; 0: try once; >0: deadline.
    // for_fiber: if non-zero, prefer messages with matching to_fiber or broadcast (0).
    //
    // Issue #2188 / #2347 (Policy A + hard audit):
    // While MutationBoundary is live (depth>0 or held), never park /
    // Fiber::yield — return empty immediately and bump soft reject counter.
    // Agent contract: **do not blocking-recv under Guard** — use try_recv /
    // recv(wait=false) or exit the boundary first (prevents livelock spin).
    // Strict / production (AURA_MUTATE_MAILBOX_STRICT=1 or production
    // canary): also bump hard counter; after N rejects in one Guard window
    // force outermost mutation mark-failed (threshold, default 8).
    // Soft / default: Policy A soft counter only (AC1). Depth==0 unchanged.
    // Gate sits next to the #2010 linear-viol pure-string filter contract
    // (both are hot-path safety fences before any blocking wait).
    // Happy path (no boundary): zero extra cost beyond existing depth/held probe.
    [[nodiscard]] std::optional<MailMessage> recv(bool wait = true, int timeout_ms = -1,
                                                  std::uint64_t for_fiber = 0) {
        const auto deadline = timeout_ms > 0 ? std::chrono::steady_clock::now() +
                                                   std::chrono::milliseconds(timeout_ms)
                                             : std::chrono::steady_clock::time_point::max();

        for (;;) {
            {
                std::lock_guard lock(mu_);
                MailMessage out;
                if (try_pop_unlocked(out, for_fiber)) {
                    // Issue #2592: deliver-side principal verify (see try_pop
                    // above). Recv path covers try_recv (which delegates here
                    // with wait=false, timeout_ms=0). Hook is idempotent;
                    // ambient != assigned → bump tenant_scope_mismatch_total
                    // + reinstall correct TenantScope (#2491 machinery).
                    if (g_current_fiber != nullptr && g_current_fiber->assigned_tenant_id() != 0) {
                        aura_fiber_install_tenant_scope_for_resume(g_current_fiber);
                    }
                    return out;
                }
                if (closed_.load(std::memory_order_relaxed))
                    return std::nullopt;
            }
            if (!wait || timeout_ms == 0) {
                if (timeout_ms == 0)
                    g_mf_mailbox_stats.recv_timeouts.fetch_add(1, std::memory_order_relaxed);
                return std::nullopt;
            }
            if (timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline) {
                g_mf_mailbox_stats.recv_timeouts.fetch_add(1, std::memory_order_relaxed);
                return std::nullopt;
            }

            // Issue #2188: hard gate — no yield-while-Guard (depth or held).
            // Mirrors #362 skip of mutation-boundary yield, but covers the
            // generic Explicit/BlockingIO park used by Agent message loops.
            const bool boundary_live = aura_evaluator_mutation_boundary_depth() > 0 ||
                                       aura_evaluator_mutation_boundary_held() != 0;
            if (boundary_live) {
                g_mf_mailbox_stats.recv_rejected_in_mutation_boundary.fetch_add(
                    1, std::memory_order_relaxed);
                local_stats_.recv_rejected_in_mutation_boundary.fetch_add(
                    1, std::memory_order_relaxed);
                // Issue #2347: window accumulate (all modes; Soft for dashboard).
                ++g_recv_boundary_reject_window;
                // Strict / production hard audit (AC2) + optional threshold (AC3).
                if (is_mutate_mailbox_strict()) {
                    g_mf_mailbox_stats.recv_rejected_in_mutation_boundary_hard_total.fetch_add(
                        1, std::memory_order_relaxed);
                    local_stats_.recv_rejected_in_mutation_boundary_hard_total.fetch_add(
                        1, std::memory_order_relaxed);
                    const auto thr = mutate_mailbox_reject_threshold();
                    if (thr > 0 && g_recv_boundary_reject_window >= thr) {
                        g_mf_mailbox_stats.recv_boundary_force_rollback_total.fetch_add(
                            1, std::memory_order_relaxed);
                        local_stats_.recv_boundary_force_rollback_total.fetch_add(
                            1, std::memory_order_relaxed);
                        // Prefer mark-failed over re-parking (Policy A stays).
                        aura_evaluator_mark_outermost_mutation_failed();
                    }
                }
                // Policy A: non-blocking empty (no park, no Fiber::yield).
                return std::nullopt;
            }

            g_mf_mailbox_stats.recv_waits.fetch_add(1, std::memory_order_relaxed);
            if (g_current_fiber != nullptr) {
                // Park so GC safepoint / steal can proceed.
                g_current_fiber->set_state(FiberState::Waiting);
                if (timeout_ms > 0)
                    Fiber::yield(YieldReason::Explicit);
                else
                    Fiber::yield(YieldReason::BlockingIO);
                // Drain eventfd if present.
                int evfd = g_current_fiber->eventfd();
                if (evfd >= 0) {
                    std::uint64_t val = 0;
                    while (::read(evfd, &val, sizeof(val)) > 0) {
                    }
                }
                g_current_fiber->set_state(FiberState::Running);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    // Issue #2188: Agent-facing alias for non-blocking recv (safe under Guard).
    [[nodiscard]] std::optional<MailMessage> try_recv(std::uint64_t for_fiber = 0) {
        return recv(/*wait=*/false, /*timeout_ms=*/0, for_fiber);
    }

    [[nodiscard]] const MultiFiberMailboxStats& stats() const noexcept { return local_stats_; }

    // Snapshot process-wide counters into ints for tests.
    static void snapshot_global(std::uint64_t& pushes, std::uint64_t& pops,
                                std::uint64_t& broadcasts, std::uint64_t& bp,
                                std::uint64_t& attaches) noexcept {
        pushes = g_mf_mailbox_stats.pushes.load(std::memory_order_relaxed);
        pops = g_mf_mailbox_stats.pops.load(std::memory_order_relaxed);
        broadcasts = g_mf_mailbox_stats.broadcasts.load(std::memory_order_relaxed);
        bp = g_mf_mailbox_stats.backpressure_rejects.load(std::memory_order_relaxed);
        attaches = g_mf_mailbox_stats.attaches.load(std::memory_order_relaxed);
    }

    // Issue #1881: full health snapshot (priority / waits / linear).
    // Issue #2010: optional fanout_bp out-param (pass nullptr to skip).
    // Issue #2188: optional recv_rejected_boundary out-param.
    static void snapshot_global_full(std::uint64_t& pushes, std::uint64_t& pops,
                                     std::uint64_t& broadcasts, std::uint64_t& bp,
                                     std::uint64_t& attaches, std::uint64_t& priority_high,
                                     std::uint64_t& recv_waits, std::uint64_t& recv_timeouts,
                                     std::uint64_t& linear_checks, std::uint64_t& linear_violations,
                                     std::uint64_t* fanout_bp = nullptr,
                                     std::uint64_t* recv_rejected_boundary = nullptr,
                                     std::uint64_t* deferred_mutation_hold = nullptr) noexcept {
        snapshot_global(pushes, pops, broadcasts, bp, attaches);
        priority_high = g_mf_mailbox_stats.priority_high.load(std::memory_order_relaxed);
        recv_waits = g_mf_mailbox_stats.recv_waits.load(std::memory_order_relaxed);
        recv_timeouts = g_mf_mailbox_stats.recv_timeouts.load(std::memory_order_relaxed);
        linear_checks = g_mf_mailbox_stats.linear_checks.load(std::memory_order_relaxed);
        linear_violations = g_mf_mailbox_stats.linear_violations.load(std::memory_order_relaxed);
        if (fanout_bp)
            *fanout_bp =
                g_mf_mailbox_stats.fanout_backpressure_rejects.load(std::memory_order_relaxed);
        // Issue #2347: hard / force totals available via g_mf_mailbox_stats
        // or query:mf-mailbox-stats (schema-2347); soft reject remains here.
        if (recv_rejected_boundary)
            *recv_rejected_boundary = g_mf_mailbox_stats.recv_rejected_in_mutation_boundary.load(
                std::memory_order_relaxed);
        // Issue #2312: delivery-gate counter (push-side, distinct from
        // recv_rejected_boundary which is recv-side #2188).
        if (deferred_mutation_hold)
            *deferred_mutation_hold = g_mf_mailbox_stats.mailbox_deferred_mutation_hold_total.load(
                std::memory_order_relaxed);
    }

private:
    bool try_pop_unlocked(MailMessage& out, std::uint64_t for_fiber) {
        if (queue_.empty())
            return false;
        if (for_fiber == 0) {
            out = std::move(queue_.front());
            queue_.pop_front();
            g_mf_mailbox_stats.pops.fetch_add(1, std::memory_order_relaxed);
            local_stats_.pops.fetch_add(1, std::memory_order_relaxed);
            dec_inflight_(1);
            return true;
        }
        // Prefer exact match, then broadcast (to_fiber==0).
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (it->to_fiber == for_fiber || it->to_fiber == 0) {
                out = std::move(*it);
                queue_.erase(it);
                g_mf_mailbox_stats.pops.fetch_add(1, std::memory_order_relaxed);
                local_stats_.pops.fetch_add(1, std::memory_order_relaxed);
                dec_inflight_(1);
                return true;
            }
        }
        return false;
    }

    void notify_all_unlocked() {
        for (auto* f : attachers_) {
            if (!f)
                continue;
            int evfd = f->eventfd();
            if (evfd >= 0) {
                std::uint64_t one = 1;
                (void)::write(evfd, &one, sizeof(one));
            }
            // If Waiting, mark Ready so scheduler may pick it up.
            if (f->state() == FiberState::Waiting)
                f->set_state(FiberState::Ready);
        }
    }

    void notify_all_locked() {
        std::lock_guard lock(mu_);
        notify_all_unlocked();
    }

    void add_inflight_(std::size_t n) noexcept {
        const auto v =
            inflight_.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed) +
            static_cast<std::uint64_t>(n);
        note_inflight_hwm(v, &local_stats_);
    }

    void dec_inflight_(std::size_t n) noexcept {
        auto cur = inflight_.load(std::memory_order_relaxed);
        while (cur > 0) {
            const auto next = cur > n ? cur - n : 0;
            if (inflight_.compare_exchange_weak(cur, next, std::memory_order_relaxed))
                return;
        }
    }

    void drop_queued_unlocked_() noexcept {
        const auto n = queue_.size();
        if (n == 0)
            return;
        queue_.clear();
        dec_inflight_(n);
    }

    mutable std::mutex mu_;
    std::deque<MailMessage> queue_;
    std::vector<Fiber*> attachers_;
    std::size_t high_water_ = 1024;
    std::uint32_t credit_limit_ = 0; // 0 → high_water (#2972)
    std::atomic<std::uint64_t> inflight_{0};
    std::atomic<bool> closed_{false};
    MultiFiberMailboxStats local_stats_{};
};

} // namespace aura::serve::mf_mailbox

#endif // AURA_SERVE_MULTI_FIBER_MAILBOX_H
