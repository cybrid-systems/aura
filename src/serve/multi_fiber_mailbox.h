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
#include "compiler/lock_order_audit.h" // Issue #2316: lock-order audit

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
// Issue #2346 / #2347: production canary probe (strong in audit hooks).
extern "C" int aura_production_defaults_active_probe() noexcept;

namespace aura::serve::mf_mailbox {

inline constexpr int kMultiFiberMailboxPhase = 3; // #1881 observability
inline constexpr int kMultiFiberMailboxIssue = 1881;

// Issue #1595 / #2010: provenance-safety prefix (fiber-stack safe, pure string).
inline constexpr std::string_view kLinearViolPrefix = "linear-viol:";

enum class MailPriority : std::uint8_t { Low = 0, Normal = 1, High = 2, Critical = 3 };

enum class PushStatus : std::uint8_t {
    Ok = 0,
    Backpressure = 1, // queue at high-water mark
    Closed = 2,
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
    // Issue #2663: held-ref export token + handoff-completed flag. When
    // held_ref_token is set, the message carries a StableNodeRef that
    // needs to be re-exported via Evaluator::handoff_ref. The mailbox
    // gate rejects any push where held_ref_token is set but
    // handoff_completed is false (Closed + handoff_reject_total bump).
    // Ordinary string payloads leave both default-initialized (zero cost
    // on hot path — single optional load + bool check). Populated by
    // Agent-send-side helpers (agent_send_ref) after a successful
    // handoff_ref call.
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

// Issue #2378: open-window timers for flush latency (zero when no open defer).
// first_open_defer_ns: set on depth 0→1; cleared when depth returns to 0.
// last_outermost_exit_ns: set on Guard outermost exit (drain opportunity).
inline std::atomic<std::uint64_t> g_mailbox_first_open_defer_ns{0};
inline std::atomic<std::uint64_t> g_mailbox_last_outermost_exit_ns{0};

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

// Issue #2587: bump the throttle-rejected counter at every gate site.
// Hard-reject / metric-only decision lives at the call site so callers
// can decide whether production_defaults_active() is the actual hard
// gate (Strict/Restricted) or whether to allow the work (Soft / Off).
// Header-inline so the relaxed fetch_add folds with the caller's branch.
[[gnu::always_inline]] inline void note_mutate_rejected_mailbox_starvation() noexcept {
    g_mf_mailbox_stats.mutate_rejected_mailbox_starvation_total.fetch_add(
        1, std::memory_order_relaxed);
}

// Issue #2378: successful enqueue after possible open defer window.
// AC3: when deferred_depth==0, single relaxed load then return (no maps).
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
    if (age_ms >= thr_ms) {
        g_mf_mailbox_stats.mailbox_defer_starvation_total.fetch_add(1, std::memory_order_relaxed);
    }
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
            r.starved = true;
            r.remaining_depth = d;
            r.elapsed_us = elapsed_us;
            g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.fetch_add(
                1, std::memory_order_relaxed);
            // Also feed #2378 starvation so health score sees hold-exit SLA.
            g_mf_mailbox_stats.mailbox_defer_starvation_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
            // Strict / production: force-resolve open depth (explicit audit).
            // Soft: leave depth for later natural retries.
            // Inline Strict probe here — is_mutate_mailbox_strict is defined
            // later in this header (#2347); avoid forward-order dependency.
            const char* strict_e = std::getenv("AURA_MUTATE_MAILBOX_STRICT");
            const bool hard_resolve =
                (strict_e && strict_e[0] == '1') || aura_production_defaults_active_probe() != 0;
            // Issue #2551 AC1: production/Strict residual after budget →
            // hard counter + Agent throttle (do not clear here even if
            // force-resolve zeros depth; subsequent free drain clears).
            if (hard_resolve) {
                g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.fetch_add(
                    1, std::memory_order_relaxed);
                g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.store(
                    1, std::memory_order_relaxed);
            }
            if (hard_resolve) {
                std::uint64_t resolved = 0;
                std::uint64_t guard = d + 8;
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
                    }
                    ++resolved;
                }
                r.force_resolved = resolved;
                r.remaining_depth =
                    g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed);
                if (resolved > 0) {
                    g_mf_mailbox_stats.mailbox_hold_exit_force_resolved_total.fetch_add(
                        resolved, std::memory_order_relaxed);
                }
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

// Multi-fiber mailbox: many attachers, priority queue, broadcast wake,
// high-water backpressure.
class MultiFiberMailbox {
public:
    explicit MultiFiberMailbox(std::size_t high_water = 1024) noexcept
        : high_water_(high_water == 0 ? 1 : high_water) {}

    void set_high_water(std::size_t n) noexcept { high_water_ = n == 0 ? 1 : n; }
    [[nodiscard]] std::size_t high_water() const noexcept { return high_water_; }
    [[nodiscard]] bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }
    void close() noexcept {
        closed_.store(true, std::memory_order_release);
        notify_all_locked();
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
    }

    void detach(Fiber* f) {
        if (!f)
            return;
        (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::Mailbox,
                                                       __builtin_FILE(), __builtin_LINE());
        std::lock_guard lock(mu_);
        attachers_.erase(std::remove(attachers_.begin(), attachers_.end(), f), attachers_.end());
    }

    [[nodiscard]] std::size_t attacher_count() const {
        (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::Mailbox,
                                                       __builtin_FILE(), __builtin_LINE());
        std::lock_guard lock(mu_);
        return attachers_.size();
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
        // Issue #2663: held-ref gate. If held_ref_token is set but
        // handoff_completed is false, reject the push (Closed + bump
        // counter). Zero cost when held_ref_token is empty (ordinary
        // string payloads never set this — single relaxed load + bool).
        if (msg.held_ref_token.has_value() && !msg.handoff_completed) {
            g_mf_mailbox_stats.handoff_reject_total.fetch_add(1, std::memory_order_relaxed);
            local_stats_.handoff_reject_total.fetch_add(1, std::memory_order_relaxed);
            return PushStatus::Closed;
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
        if (msg.to_fiber != 0) {
            for (auto* a : attachers_) {
                if (a && a->id() == msg.to_fiber) {
                    const auto snap = a->mutation_safety_snapshot();
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
        // Issue #2680: shared-Evaluator delivery gate. If the shared
        // Evaluator's MutationBoundary is held (depth>0 || held) by ANY
        // fiber, defer (BP) rather than letting this payload become visible
        // to a receiver that shares the same Evaluator mid-mutation.
        // Mirrors the recv() shared-Evaluator check (L820-821). Per AC2,
        // uses the same authority as steal safety (aura_evaluator_mutation_
        // boundary_held / depth C ABI hooks wired from #2184/#2188/#2200).
        // Per AC1: deferred (not dropped) — sender retries / queues.
        // Per AC4: Soft / sandbox=off still observes via _soft_observe_total.
        // Per AC6 happy path: zero cost when deferred_depth==0 (one
        // relaxed load on `mailbox_shared_evaluator_deferred_total` proxy).
        if (aura_evaluator_mutation_boundary_depth() > 0 ||
            aura_evaluator_mutation_boundary_held() != 0) {
            g_mf_mailbox_stats.mailbox_shared_evaluator_deferred_total.fetch_add(
                1, std::memory_order_relaxed);
            local_stats_.mailbox_shared_evaluator_deferred_total.fetch_add(
                1, std::memory_order_relaxed);
            if (is_mutate_mailbox_strict()) {
                g_mf_mailbox_stats.mailbox_shared_evaluator_deferred_hard_total.fetch_add(
                    1, std::memory_order_relaxed);
                local_stats_.mailbox_shared_evaluator_deferred_hard_total.fetch_add(
                    1, std::memory_order_relaxed);
            } else {
                g_mf_mailbox_stats.mailbox_shared_evaluator_deferred_soft_observe_total.fetch_add(
                    1, std::memory_order_relaxed);
                local_stats_.mailbox_shared_evaluator_deferred_soft_observe_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
            note_mailbox_mutation_hold_defer();
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
        // Issue #2663: held-ref gate (mirror of push() — broadcast fan-out
        // cannot partial-deliver an unexported ref to a subset of
        // attachers; either all-or-nothing reject). production-safe default:
        // always Closed + counter bump; Soft / sandbox=off may interpret
        // the bump as metric-only via a future refinement.
        if (proto.held_ref_token.has_value() && !proto.handoff_completed) {
            g_mf_mailbox_stats.handoff_reject_total.fetch_add(1, std::memory_order_relaxed);
            local_stats_.handoff_reject_total.fetch_add(1, std::memory_order_relaxed);
            return PushStatus::Closed;
        }
        std::lock_guard lock(mu_);
        if (closed_.load(std::memory_order_relaxed))
            return PushStatus::Closed;
        // Issue #2680: shared-Evaluator delivery gate (fanout variant).
        // If the shared Evaluator's MutationBoundary is held (depth>0 ||
        // held) by ANY fiber, defer the ENTIRE fanout — don't partial-
        // deliver to a subset of attachers while another target on the
        // shared Evaluator is mid-mutation. Mirrors the recv() L820-821
        // shared-Evaluator check. Per AC2: same authority as steal safety.
        // Per AC1: deferred (not dropped) — sender retries / queues.
        if (aura_evaluator_mutation_boundary_depth() > 0 ||
            aura_evaluator_mutation_boundary_held() != 0) {
            g_mf_mailbox_stats.mailbox_shared_evaluator_deferred_total.fetch_add(
                1, std::memory_order_relaxed);
            local_stats_.mailbox_shared_evaluator_deferred_total.fetch_add(
                1, std::memory_order_relaxed);
            if (is_mutate_mailbox_strict()) {
                g_mf_mailbox_stats.mailbox_shared_evaluator_deferred_hard_total.fetch_add(
                    1, std::memory_order_relaxed);
                local_stats_.mailbox_shared_evaluator_deferred_hard_total.fetch_add(
                    1, std::memory_order_relaxed);
            } else {
                g_mf_mailbox_stats.mailbox_shared_evaluator_deferred_soft_observe_total.fetch_add(
                    1, std::memory_order_relaxed);
                local_stats_.mailbox_shared_evaluator_deferred_soft_observe_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
            note_mailbox_mutation_hold_defer();
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
        }
        const auto need = attachers_.empty() ? std::size_t{1} : attachers_.size();
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
            return true;
        }
        // Prefer exact match, then broadcast (to_fiber==0).
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (it->to_fiber == for_fiber || it->to_fiber == 0) {
                out = std::move(*it);
                queue_.erase(it);
                g_mf_mailbox_stats.pops.fetch_add(1, std::memory_order_relaxed);
                local_stats_.pops.fetch_add(1, std::memory_order_relaxed);
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

    mutable std::mutex mu_;
    std::deque<MailMessage> queue_;
    std::vector<Fiber*> attachers_;
    std::size_t high_water_ = 1024;
    std::atomic<bool> closed_{false};
    MultiFiberMailboxStats local_stats_{};
};

} // namespace aura::serve::mf_mailbox

#endif // AURA_SERVE_MULTI_FIBER_MAILBOX_H
