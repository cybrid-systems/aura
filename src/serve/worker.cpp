// serve/worker.cpp — Worker thread with work-stealing
#include "worker.h"
#include "scheduler.h"
#include "aura_platform.h"
#include "compiler/lock_order_audit.h" // Issue #2354: FiberRegistry rank
#include "core/gc_hooks.h"             // Issue #2377: steal-complete missing counter

#include <cstdio>
#include <unistd.h>

import std;
#if AURA_HAVE_EVENTFD
#include <sys/eventfd.h>
#endif

namespace aura::serve {

// Evaluator hooks are weak so non-evaluator link units (test_concurrent,
// test_spec_jit) succeed without evaluator_fiber_mutation.cpp. Strong
// defs live there; fiber_bridge.cpp provides weak no-ops when present.
// With a weak *reference*, a missing definition resolves to nullptr
// (ELF) rather than a hard link error — required for ASAN/UBSAN jobs
// that may rebuild worker.cpp against a stale fiber_bridge.o.
extern "C" {
void aura_evaluator_probe_linear_on_steal() __attribute__((weak));
void aura_evaluator_bump_steal_deferred_violation() __attribute__((weak));
void aura_evaluator_bump_mutation_steal_attempt() __attribute__((weak));
void aura_evaluator_bump_steal_arena_yield() __attribute__((weak));
void aura_evaluator_bump_steal_outermost_enforced() __attribute__((weak));
// Issue #1641: per-CompilerMetrics steal observability (weak; strong defs
// in evaluator_fiber_mutation.cpp). Avoids importing Evaluator module
// into serve/*.cpp (non-module TU).
void aura_evaluator_bump_boundary_held_steal_safe() __attribute__((weak));
void aura_evaluator_bump_steal_mutation_boundary_deferred() __attribute__((weak));
void aura_evaluator_bump_starvation_mitigated_for_boundary() __attribute__((weak));
// Issue #2118: orch agent soft-boundary steal skip counter.
void aura_orch_note_agent_steal_skipped_boundary() __attribute__((weak));
// Issue #2203: single mandatory steal-complete entry (clear orphan GC
// defer + stack handoff metric). Strong def in evaluator_fiber_mutation.cpp;
// weak no-op in fiber_bridge.cpp for light test binaries.
void aura_evaluator_on_steal_complete(void* fiber_ptr) __attribute__((weak));
// Issue #2310: fail-closed force-deopt on MutationSafetySnapshot mismatch.
// Strong def in evaluator_fiber_mutation.cpp / aura_jit_bridge.cpp; weak
// no-op in fiber_bridge.cpp for light / asan serve-only link units.
void aura_force_deopt_on_steal_snapshot_mismatch(void* fiber_ptr) noexcept __attribute__((weak));
}

static inline void call_steal_arena_yield() noexcept {
    if (aura_evaluator_bump_steal_arena_yield)
        aura_evaluator_bump_steal_arena_yield();
}
static inline void call_steal_outermost_enforced() noexcept {
    if (aura_evaluator_bump_steal_outermost_enforced)
        aura_evaluator_bump_steal_outermost_enforced();
}
static inline void call_probe_linear_on_steal() noexcept {
    if (aura_evaluator_probe_linear_on_steal)
        aura_evaluator_probe_linear_on_steal();
}
// Issue #2203 / Issue #2377: single steal-complete entry is mandatory for
// multi-worker production. Strong def (evaluator_fiber_mutation.cpp)
// runs the full steal-complete transaction:
//   Panic orphan clear → residual interlock (#2314) → LayoutStamp dual-
//   check (#2351) → linear/outermost metrics.
// Weak no-op / null ABI under production → fail-closed (never legacy-only
// path that skips residual + stamp). Light/sandbox (production Soft lock
// off) may use weak no-op or legacy N-call fallback with metric bump.
static inline void call_steal_complete(Fiber* stolen) noexcept {
    // Issue #2699: call_steal_complete_now_uses_unified_transaction
    // (wire-in marker — ensures the call graph is the single #2699
    // transaction entry point).
    if (aura_evaluator_on_steal_complete) {
        // Strong wins over weak when linked. Weak no-op under production
        // aborts inside fiber_bridge (#2377); under sandbox it bumps
        // steal_complete_entry_missing_total and returns.
        aura_evaluator_on_steal_complete(stolen);
        return;
    }
    // Null ABI: production multi-worker must link strong steal-complete.
    if (aura::serve::steal_snapshot_soft_production_locked()) {
        std::fprintf(stderr, "FATAL: aura_evaluator_on_steal_complete unresolved under "
                             "production (#2377); multi-worker builds must link the "
                             "strong steal-complete ABI (no legacy residual-less path)\n");
        std::abort();
    }
    // Light/sandbox: legacy N weak calls + observability for missing entry.
    aura::gc_hooks::bump_steal_complete_entry_missing_total();
    call_probe_linear_on_steal();
    call_steal_outermost_enforced();
}
static inline void call_steal_deferred_violation() noexcept {
    if (aura_evaluator_bump_steal_deferred_violation)
        aura_evaluator_bump_steal_deferred_violation();
}
static inline void call_mutation_steal_attempt() noexcept {
    if (aura_evaluator_bump_mutation_steal_attempt)
        aura_evaluator_bump_mutation_steal_attempt();
}

// ── Constructor ───────────────────────────────────────

WorkerThread::WorkerThread(int id, Scheduler* scheduler)
    : id_(id)
    , scheduler_(scheduler) {

    // Create wake eventfd
#if AURA_HAVE_EVENTFD
    wake_evfd_ = ::eventfd(0, EFD_NONBLOCK);
    if (wake_evfd_ == -1)
        throw std::system_error(errno, std::generic_category(),
                                "worker[" + std::to_string(id) + "] eventfd");
#else
    wake_evfd_ = -1; // macOS: no eventfd
#endif
}

// ── Destructor ────────────────────────────────────────

WorkerThread::~WorkerThread() {
    stop();
    if (wake_evfd_ >= 0) {
        ::close(wake_evfd_);
        wake_evfd_ = -1;
    }
}

// ── start — launch the worker thread ──────────────────

void WorkerThread::start() {
    running_.store(true, std::memory_order_release);
    thread_ = std::jthread([this](std::stop_token) { run(); });
}

// ── stop — request graceful stop ──────────────────────

void WorkerThread::stop() {
    // Issue #2573 (ubsan-smoke): ubsan-smoke `test_concurrent`
    // SIGSEGV'd at test_concurrent.cpp:1932
    // (test_metrics_json_format) in ~Scheduler(): the worker jthread
    // never exited run() so w->join() blocked forever. Root cause:
    // wake_cv_.notify_all() was called WITHOUT holding wake_mutex_,
    // racing with the wait_for(predicate) entry check. The standard
    // wait_for+predicate re-check is safe in principle, but the
    // observed hang is the worker stuck in resume() while the IO
    // thread (Scheduler::run()) also raced shutdown — the worker
    // was in mid-callback into a fiber when stop() landed, and the
    // notification never reached it because the cv wasn't paired
    // with the mutex acquire that the wait_for is keyed on.
    //
    // Fix: hold wake_mutex_ across running_ store AND notify_all.
    // This pairs the wait/lock acquire on the waiter side with the
    // store/lock release on the notifier side, closing the lost-
    // wakeup window. The wake_evfd_ write happens outside the lock
    // (it's not what cv waits on, it's the WakingFibers path).
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        running_.store(false, std::memory_order_release);
    }
    if (wake_evfd_ >= 0) {
        uint64_t val = 1;
        ::write(wake_evfd_, &val, sizeof(val));
    }
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_cv_.notify_all();
    }
}

// ── join — wait for thread to finish ──────────────────

void WorkerThread::join() {
    if (thread_.joinable())
        thread_.join();
}

// ── enqueue — add fiber to local queue ────────────────
// Thread-safe: push into the work-stealing deque.
// The push() operation is "owner-only" in Chase-Lev, but the
// scheduler (IO thread) is the one calling enqueue from outside.
// This is safe because:
//   1. push() only writes to buffer_ and increments bottom_
//   2. The stealers only read the top
//   3. The only conflict is between push and steal, which is handled
//      by the Chase-Lev memory ordering (release fence in push,
//      seq_cst fence in steal)

void WorkerThread::enqueue(Fiber* fiber) {
    if (!fiber || fiber->is_done())
        return;

    local_queue_.push(fiber);
    pending_.fetch_add(1, std::memory_order_release);

    if (worker_metrics_) {
        worker_metrics_->local_pushes.fetch_add(1, std::memory_order_relaxed);
    }

    // Wake the worker if it was sleeping
    if (wake_evfd_ >= 0) {
        uint64_t val = 1;
        ::write(wake_evfd_, &val, sizeof(val));
    }
    wake_cv_.notify_one();
}

// ── notify_fiber_done — report completed fiber ────────

void WorkerThread::notify_fiber_done(Fiber* fiber) {
    if (scheduler_) {
        scheduler_->on_fiber_done(fiber);
    }
}

// Issue #119: per-worker fiber registry. The scheduler's
// enqueue() registers the fiber here, and the worker's
// on_fiber_done path (via notify_fiber_done) unregisters it.
// The registry is the source of truth for fiber_by_id.

void WorkerThread::register_fiber(Fiber* fiber) {
    if (!fiber)
        return;
    ::aura::compiler::lock_order::AuditedMutexLock lock(
        fiber_registry_mutex_, ::aura::compiler::lock_order::Level::FiberRegistry);
    fiber_registry_[fiber->id()] = fiber;
}

void WorkerThread::unregister_fiber(Fiber* fiber) {
    if (!fiber)
        return;
    ::aura::compiler::lock_order::AuditedMutexLock lock(
        fiber_registry_mutex_, ::aura::compiler::lock_order::Level::FiberRegistry);
    fiber_registry_.erase(fiber->id());
}

Fiber* WorkerThread::fiber_by_id(std::uint64_t fiber_id) const {
    ::aura::compiler::lock_order::AuditedMutexLock lock(
        fiber_registry_mutex_, ::aura::compiler::lock_order::Level::FiberRegistry);
    auto it = fiber_registry_.find(fiber_id);
    if (it == fiber_registry_.end())
        return nullptr;
    return it->second;
}

// ── try_steal_from — attempt to steal a fiber ─────────

bool WorkerThread::try_steal_from(WorkerThread* victim) {
    if (!victim || victim == this)
        return false;

    // Issue #812: do not steal while this worker is in a GC safepoint
    // phase (Requested/Sweeping). Coordinate with Arena compact / GC
    // long ops so steal never races pointer fixup.
    {
        const auto ph = gc_state_.phase.load(std::memory_order_acquire);
        if (ph != GCPhase::None) {
            call_steal_arena_yield();
            return false;
        }
    }

    // Try to steal a fiber from the victim's deque.
    // The deque only contains fibers that yielded (Explicit/MutationBoundary),
    // but we re-check is_stealable(snap) (#2549: candidate + MutationSafety
    // Snapshot jointly) against stale fibers that may have re-entered a
    // Guard after being enqueued.
    for (int attempt = 0; attempt < 3; ++attempt) {
        Fiber* stolen = victim->try_steal();
        if (!stolen)
            break;

        // Skip fibers pinned to another worker (affinity)
        if (stolen->affinity() >= 0 && stolen->affinity() != id()) {
            // Pinned to a different worker — put it back
            victim->enqueue(stolen);
            continue;
        }

        // Issue #588 / #2184: one MutationSafetySnapshot sample for the
        // steal decision (depth + held + yield jointly). Defer when
        // MutationBoundary with active Guard (depth>0 or held).
        const auto snap = stolen->mutation_safety_snapshot();
        if (stolen->mutation_safety_snapshot_inconsistent(snap)) {
            Fiber::bump_mutation_steal_snapshot_mismatch();
            // Issue #2310 AC1 / #2372: fail-closed on inconsistency.
            // Production default is force-deopt + full refresh under
            // exclusive recovery (NOT silent resume of generation-behind
            // code). Soft env is ignored under production lock (#2372);
            // test override still allows Soft under AURA_SANDBOX=off.
            // Missing strong force-deopt ABI under production → abort
            // (never silent continue after mismatch bump only).
            if (!aura::serve::is_steal_snapshot_soft_mode()) {
                if (aura_force_deopt_on_steal_snapshot_mismatch) {
                    aura_force_deopt_on_steal_snapshot_mismatch(stolen);
                } else if (aura::serve::steal_snapshot_soft_production_locked()) {
                    // Issue #2372 AC2: production requires strong force-deopt
                    // ABI. Weak-null under production must not resume
                    // generation-behind code after a mismatch bump.
                    std::fprintf(stderr, "FATAL: aura_force_deopt_on_steal_snapshot_mismatch "
                                         "unresolved under production (#2372); multi-worker "
                                         "builds must link the strong ABI\n");
                    std::abort();
                }
                // Light/test without production lock: null ABI still
                // continues after mismatch bump (legacy light-link path).
            }
            // Do not normal-enqueue until refresh completes (under
            // exclusive recovery). The fiber is dropped from this steal
            // attempt — generation-behind code must NOT silently resume.
            continue;
        }
        // Issue #2549: authoritative enqueue gate is is_stealable(snap)
        // (is_steal_candidate + is_at_mutation_boundary_safe on one sample).
        // Never reason-class alone.
        if (stolen->is_stealable(snap)) {
            const int pri = fiber_steal_priority(stolen);
            if (pri >= 2) {
                metrics::adaptive_steal_stats().outermost_preferred.fetch_add(
                    1, std::memory_order_relaxed);
                if (pri >= 3) {
                    metrics::adaptive_steal_stats().llm_tail_reductions.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            // Issue #2253 AC1: hold-aware work-steal scoring. Single
            // integer score that combines the AC1 components so the
            // winner-take-all pick is consistent across the loop.
            // AC3 happy path: scoring is arithmetic over already-loaded
            // snapshot fields — zero extra atomics beyond the existing
            // probe loads above. AC2: long-hold victims remain steal-
            // deferred (the gen != cur path below handles them); once
            // they become outermost-safe, the +100 dominates and the
            // -40 hold penalty nudges them after fresher victims.
            {
                int score = 0;
                // +100: depth-safe (outermost-safe) victim is the strongest
                // candidate (per AC1).
                score += 100;
                // +50: existing one-shot boost consumed on success
                // (clear below; before the clear so the score reflects
                // the boost that influenced the pick).
                if (stolen->has_steal_priority_boost())
                    score += 50;
                // +20: short-yield victims (Explicit / OperationBoundary /
                // PassPipeline) finish fast and free the worker.
                const auto yr = snap.last_yield;
                if (yr == YieldReason::Explicit || yr == YieldReason::OperationBoundary ||
                    yr == YieldReason::PassPipeline) {
                    score += 20;
                }
                // -40: long-hold penalty so a victim that recently held
                // the Guard is deprioritized until other workers make
                // progress on shorter candidates.
                const auto recent_hold = stolen->last_hold_us();
                constexpr std::uint64_t kRecentHoldPenaltyBpUs = 100000; // 100 ms p90
                if (recent_hold > kRecentHoldPenaltyBpUs)
                    score -= 40;
                // AC3: bump total + bucket histogram (issue body option).
                auto& ads_score = metrics::adaptive_steal_stats();
                ads_score.steal_score_selected_total.fetch_add(1, std::memory_order_relaxed);
                if (score < 50)
                    ads_score.steal_score_bucket_0_49.fetch_add(1, std::memory_order_relaxed);
                else if (score < 100)
                    ads_score.steal_score_bucket_50_99.fetch_add(1, std::memory_order_relaxed);
                else if (score < 150)
                    ads_score.steal_score_bucket_100_149.fetch_add(1, std::memory_order_relaxed);
                else if (score < 200)
                    ads_score.steal_score_bucket_150_199.fetch_add(1, std::memory_order_relaxed);
                else
                    ads_score.steal_score_bucket_200p.fetch_add(1, std::memory_order_relaxed);
            }
            stolen->bump_steal_success();
            // Issue #1492: one-shot boost consumed on successful steal.
            stolen->clear_steal_priority_boost();
            // Issue #2518: stamp resume safety ticket from this steal sample.
            // Resume must see the same even safety_seq_ (ticket); Guard
            // enter/exit mid-window advances seq → hard-fail under production.
            // Independent of LayoutStamp restamp (#2510) — no dual-compute.
            stolen->set_resume_safety_ticket(snap.ticket);
            // Issue #783: refined split. Successful
            // steal at a MutationBoundary point with
            // depth==0 == "outermost safe steal" —
            // record separately for the
            // (query:orchestration-steal-outermost-
            // stats) primitive + bump the cross-fiber
            // safe steal counter (always bumped on
            // successful cross-fiber steal at a
            // mutation boundary).
            if (snap.last_yield == YieldReason::MutationBoundary) {
                stolen->bump_steal_outermost_mutation_boundary();
                stolen->bump_cross_fiber_mutation_safe_steal();
                // Issue #1641: paired boundary_held_steal_safe_total
                // bump (per-CompilerMetrics observability surface;
                // pairs with the legacy per-Fiber bump_cross_fiber_
                // mutation_safe_steal counter for the Scheduler/Worker
                // level aggregate).
                if (aura_evaluator_bump_boundary_held_steal_safe)
                    aura_evaluator_bump_boundary_held_steal_safe();
            }
            // Issue #2203 / #2377 / #2510 / #2546: single mandatory steal-
            // complete entry (full transaction under strong ABI: residual
            // clear + stamp dual-check + forced restamp + hard-AND residual
            // GcDeferReason == 0 under Hard/production). Production forbids
            // weak-null legacy residual-less path. On LayoutStamp hard-fail
            // (#2510) or residual hard-fail (#2546) the fiber is Cancel+Done
            // — must not enqueue Ready.
            call_steal_complete(stolen);
            if (stolen->state() == FiberState::Done || stolen->is_cancel_requested()) {
                // Hard-fail path: generation-behind or residual-nonzero fiber
                // rejected. Steal attempt counts as unsuccessful for enqueue.
                return false;
            }
            local_queue_.push(stolen);
            return true;
        }

        // Issue #1492 / #1254 / #783 / #1633 / #2115 / #2184: MANDATE defer +
        // starvation mitigation when victim is at a MutationBoundary that
        // is not snapshot-safe (depth>0 or held). is_at_safe_mutation_boundary
        // aliases depth-safe (#2115). Inner path always runs
        // apply_starvation_mitigation so nested long mutations do not starve
        // other agent fibers (50+ fiber AI orch).
        // Issue #2549: defer path uses candidate filter + snapshot-safe
        // probe (not is_stealable(), which already implies safe).
        if (stolen->is_steal_candidate(snap) && snap.last_yield == YieldReason::MutationBoundary &&
            !stolen->is_at_mutation_boundary_safe(snap)) {
            // Issue #2115 AC4: steal skipped because victim holds a
            // mutation boundary (depth-safe probe failed).
            auto& ads = metrics::adaptive_steal_stats();
            ads.steal_skipped_mutation_boundary_total.fetch_add(1, std::memory_order_relaxed);
            // Issue #2119: update boundary starvation pressure (bp).
            {
                const auto skips =
                    ads.steal_skipped_mutation_boundary_total.load(std::memory_order_relaxed);
                const auto attempts =
                    ads.steal_attempt_sample_total.fetch_add(1, std::memory_order_relaxed) + 1;
                // Pressure ≈ skips / max(attempts, skips) as basis points, capped 10000.
                const auto den = attempts > skips ? attempts : (skips == 0 ? 1 : skips);
                const auto bp = (skips * 10000ull) / den;
                ads.steal_starvation_boundary_pressure.store(bp > 10000 ? 10000 : bp,
                                                             std::memory_order_relaxed);
            }
            // Issue #2118: orch agent soft-boundary steal skip (C ABI; no orch dep).
            if (stolen->orch_agent_boundary_active() && aura_orch_note_agent_steal_skipped_boundary)
                aura_orch_note_agent_steal_skipped_boundary();
            stolen->bump_steal_deferred_mutation_boundary();
            call_steal_deferred_violation();
            ads.global_deferred_mutation_total.fetch_add(1, std::memory_order_relaxed);
            ads.mutation_bias_hits.fetch_add(1, std::memory_order_relaxed);
            if (stolen->is_at_inner_mutation_boundary(snap)) {
                // #1633 AC1: bump_deferred_inner + apply_starvation_mitigation + defer
                stolen->bump_steal_inner_mutation_boundary_deferred();
                metrics::adaptive_steal_stats().steal_deferred_inner_boundary.fetch_add(
                    1, std::memory_order_relaxed);
                apply_starvation_mitigation(stolen);
                // Issue #1641: paired steal_mutation_boundary_deferred_total
                // + starvation_mitigated_for_boundary_count bumps
                // (per-CompilerMetrics observability surface).
                if (aura_evaluator_bump_steal_mutation_boundary_deferred)
                    aura_evaluator_bump_steal_mutation_boundary_deferred();
                if (aura_evaluator_bump_starvation_mitigated_for_boundary)
                    aura_evaluator_bump_starvation_mitigated_for_boundary();
            } else {
                // Non-inner but still not safe (edge): threshold boost
                // after repeated defers (#1270 / #1445).
                const auto defers = stolen->steal_deferred_mutation_boundary_count();
                if (defers > 3) {
                    metrics::adaptive_steal_stats().deferred_pressure_boosts.fetch_add(
                        1, std::memory_order_relaxed);
                    metrics::adaptive_steal_stats().starvation_priority_boosts.fetch_add(
                        1, std::memory_order_relaxed);
                    metrics::adaptive_steal_stats().steal_priority_boost_triggered.fetch_add(
                        1, std::memory_order_relaxed);
                    stolen->apply_steal_priority_boost();
                }
            }
        }

        // Not stealable — put it back on the victim's queue.
        // This could happen if the fiber state changed after it was enqueued.
        // Give up after a few attempts to avoid infinite loop.
        victim->enqueue(stolen);
    }
    return false;
}

// ── run — the worker's main dispatch loop ─────────────
//
// Algorithm:
//   1. Drain local queue (pop LIFO)
//   2. When empty, try to steal from a random worker
//   3. If steal succeeds, go to step 1
//   4. If steal fails repeatedly, sleep on condition variable
//   5. Wake when new fibers arrive (enqueue/eventfd)

void WorkerThread::run() {
    // Set up thread-local worker context for fiber yield/resume
    g_worker_ctx = &ctx_;
    ctx_.gc_state = &gc_state_; // link GC state for safepoint check

    // Grab metrics pointer (set by scheduler before start)
    auto* my_metrics = worker_metrics_;

    const size_t MAX_ITER_PER_ROUND = 1000;

    while (running_.load(std::memory_order_acquire)) {
        auto cycle_start = std::chrono::steady_clock::now();
        bool was_busy = false;

        // ── Phase 1: drain local queue (LIFO) ───────
        size_t iter = 0;
        while (iter < MAX_ITER_PER_ROUND) {
            Fiber* fiber = local_queue_.pop();
            if (!fiber)
                break;
            ++iter;
            was_busy = true;

            if (my_metrics) {
                my_metrics->local_pops.fetch_add(1, std::memory_order_relaxed);
            }

            if (fiber->is_done()) {
                pending_.fetch_sub(1, std::memory_order_release);
                continue;
            }

            // Resume the fiber — runs until yield() or completion.
            //
            // Issue #115: track the "running fiber" count so the
            // GC coordinator can wait for currently-running
            // fibers to arrive at the safepoint. Increment
            // BEFORE resume (in case the GC requests a
            // safepoint while we're blocked in resume()) and
            // decrement AFTER (so the count goes back to 0
            // when the worker is back in its dispatch loop).
            gc_state_.running_fiber_count.fetch_add(1, std::memory_order_acq_rel);
            if (my_metrics) {
                my_metrics->fibers_executed.fetch_add(1, std::memory_order_relaxed);
            }
            fiber->resume();
            gc_state_.running_fiber_count.fetch_sub(1, std::memory_order_acq_rel);

            // After resume: fiber either yielded or finished
            if (fiber->is_done()) {
                pending_.fetch_sub(1, std::memory_order_release);
                notify_fiber_done(fiber);
                continue;
            }

            auto fb_state = fiber->state();
            if (fb_state == FiberState::Waiting) {
                // Yielded for event — leave off queue, epoll will wake
                pending_.fetch_sub(1, std::memory_order_release);
                if (my_metrics) {
                    my_metrics->fibers_waiting.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                // Non-Waiting yield: keep scheduling
                local_queue_.push(fiber);
                // pending_ unchanged
                if (my_metrics) {
                    my_metrics->fibers_yielded.fetch_add(1, std::memory_order_relaxed);
                    my_metrics->local_pushes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // ── Phase 1.5: record queue depth ───────────
        if (my_metrics) {
            size_t qd = local_queue_.size_approx();
            my_metrics->record_qdepth(qd);
        }

        // ── Phase 2: check pending status ───────────
        bool any_pending = (pending_.load(std::memory_order_acquire) > 0);
        bool local_nonempty = !local_queue_.empty_approx();
        if (!any_pending && !local_nonempty) {
            if (scheduler_ && scheduler_->has_waiting_fibers()) {
                any_pending = true;
            }
        }

        // ── Phase 3: try to steal ───────────────────
        steal_budget_.apply_deferred_pressure(
            metrics::adaptive_steal_stats().global_deferred_mutation_total.load(
                std::memory_order_relaxed));
        if (!local_nonempty && any_pending && steal_budget_.should_steal()) {
            bool stole = false;

            // Issue #500: try ring neighbor first, then random victims.
            if (scheduler_) {
                int n_workers = scheduler_->num_workers();
                if (n_workers > 1) {
                    const int ring_victim_id = (id_ + 1) % n_workers;
                    if (ring_victim_id != id_) {
                        metrics::adaptive_steal_stats().ring_steal_attempts.fetch_add(
                            1, std::memory_order_relaxed);
                        call_mutation_steal_attempt();
                        if (my_metrics) {
                            my_metrics->steal_attempts.fetch_add(1, std::memory_order_relaxed);
                        }
                        if (try_steal_from(scheduler_->worker(ring_victim_id))) {
                            metrics::adaptive_steal_stats().ring_steal_successes.fetch_add(
                                1, std::memory_order_relaxed);
                            stole = true;
                            if (my_metrics) {
                                my_metrics->steal_successes.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                    if (!stole) {
                        const int steal_tries =
                            metrics::adaptive_steal_stats().global_deferred_mutation_total.load(
                                std::memory_order_relaxed) > 10
                                ? 5
                                : 3;
                        // Issue #970: thread_local RNG — std::rand() is not
                        // thread-safe and serializes multi-worker steal paths.
                        thread_local std::mt19937 rng{std::random_device{}() ^
                                                      (static_cast<unsigned>(id_) * 0x9e3779b9u)};
                        std::uniform_int_distribution<int> dist(0, n_workers - 1);
                        for (int attempt = 0; attempt < steal_tries; ++attempt) {
                            int victim_id = dist(rng);
                            if (victim_id == id_)
                                continue;
                            auto* victim = scheduler_->worker(victim_id);
                            call_mutation_steal_attempt();
                            if (my_metrics) {
                                my_metrics->steal_attempts.fetch_add(1, std::memory_order_relaxed);
                            }
                            if (try_steal_from(victim)) {
                                stole = true;
                                if (my_metrics) {
                                    my_metrics->steal_successes.fetch_add(
                                        1, std::memory_order_relaxed);
                                }
                                break;
                            }
                        }
                    }
                }
            }

            if (stole) {
                steal_budget_.record_success();
                was_busy = true;
                continue; // go back to Phase 1
            } else {
                steal_budget_.record_failure();
                // Issue #921: starvation backoff — exponential pause
                // after consecutive failed steals so busy victims are
                // not hammered under high contention. Caps at ~1ms.
                // Reuses StealBudget::consecutive_failures (no extra state).
                const int fails = steal_budget_.consecutive_failures;
                if (fails >= 4) {
                    const unsigned shift = static_cast<unsigned>(std::min(10, fails - 3));
                    std::this_thread::sleep_for(std::chrono::microseconds(1u << shift));
                }
            }
        }

        // ── Phase 4: wait for work ──────────────────
        if (!local_nonempty && !iter) {
            // Reset steal budget on wake
            steal_budget_.consecutive_failures = 0;

            // Record idle time
            if (my_metrics) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now - cycle_start).count();
                my_metrics->record_idle(elapsed);
            }

            // Drain the wake eventfd before sleeping
            {
                uint64_t val = 0;
                if (wake_evfd_ >= 0) {
                    ::read(wake_evfd_, &val, sizeof(val));
                }
                if (val > 0 && my_metrics) {
                    my_metrics->wake_events.fetch_add(val, std::memory_order_relaxed);
                }
            }

            // Wait on condition variable
            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
                return !local_queue_.empty_approx() || !running_.load(std::memory_order_acquire);
            });
        } else {
            // Record busy time
            if (my_metrics && was_busy) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now - cycle_start).count();
                my_metrics->record_busy(elapsed);
            }
        }
    }

    // Detach GC state. Do not assign g_worker_ctx = nullptr here: under
    // UBSan + ucontext fibers that store has been observed as
    // "store to null pointer of type 'struct WorkerContext *'" (TLS
    // address of the slot unusable at thread exit). The OS thread tears
    // down TLS on return from run(), and nothing after this point may
    // yield/resume on this worker.
    ctx_.gc_state = nullptr;
}

} // namespace aura::serve

extern "C" {
std::uint64_t aura_adaptive_steal_mutation_bias_hits() {
    return aura::serve::metrics::adaptive_steal_stats().mutation_bias_hits.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_adaptive_steal_outermost_preferred() {
    return aura::serve::metrics::adaptive_steal_stats().outermost_preferred.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_adaptive_steal_llm_tail_reductions() {
    return aura::serve::metrics::adaptive_steal_stats().llm_tail_reductions.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_adaptive_steal_deferred_pressure_boosts() {
    return aura::serve::metrics::adaptive_steal_stats().deferred_pressure_boosts.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_adaptive_steal_global_deferred_total() {
    return aura::serve::metrics::adaptive_steal_stats().global_deferred_mutation_total.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_adaptive_steal_ring_attempts() {
    return aura::serve::metrics::adaptive_steal_stats().ring_steal_attempts.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_adaptive_steal_ring_successes() {
    return aura::serve::metrics::adaptive_steal_stats().ring_steal_successes.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_work_steal_attempts_total() {
    if (!aura::serve::g_scheduler)
        return 0;
    std::uint64_t total = 0;
    const auto& m = aura::serve::g_scheduler->metrics();
    for (std::size_t i = 0; i < m.num_workers(); ++i) {
        total += m.worker(i).steal_attempts.load(std::memory_order_relaxed);
    }
    return total;
}
std::uint64_t aura_work_steal_successes_total() {
    if (!aura::serve::g_scheduler)
        return 0;
    std::uint64_t total = 0;
    const auto& m = aura::serve::g_scheduler->metrics();
    for (std::size_t i = 0; i < m.num_workers(); ++i) {
        total += m.worker(i).steal_successes.load(std::memory_order_relaxed);
    }
    return total;
}
}
