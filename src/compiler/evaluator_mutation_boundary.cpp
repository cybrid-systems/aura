// evaluator_mutation_boundary.cpp — Wave 3a: MutationBoundaryGuard out-of-line
// aura.compiler.evaluator module partition.
//
// Nested class declaration remains in evaluator.ixx (needs private Evaluator
// access). Heavy RAII paths (try_acquire, AcquireTag ctor, dtor, move,
// enable_fine_rollback) live here so evaluator.ixx stays a thinner interface.

module;

#include "observability_metrics.h"
#include "lock_order_audit.h"
#include "core/gc_hooks.h"
#include "core/resource_quota.hh"
#include "security_capabilities.h" // aura_fiber_current_id
#include "aura_jit_bridge.h"       // aura_invoke_long_mutation_scheduler_hook
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>

module aura.compiler.evaluator;

import std;

namespace aura::compiler {

// ── try_acquire (#1547 / #1556 / #1590) ──────────────────────────────────
aura::core::AuraResult<std::unique_ptr<Evaluator::MutationBoundaryGuard>>
Evaluator::MutationBoundaryGuard::try_acquire(Evaluator& ev, std::uint64_t pending_count,
                                              bool* success_flag, bool fine_rollback) noexcept {
    // Issue #1547 / #1618 / #1628: typed ResourceQuotaExceeded —
    // never PanicCheckpoint / runtime_error on quota reject.
    if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_))
        m->mutation_guard_try_acquire_total.fetch_add(1, std::memory_order_relaxed);
    if (auto err = ev.check_mutation_quota(pending_count)) {
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_)) {
            m->manager_enforce_total.fetch_add(1, std::memory_order_relaxed);
            m->mutation_guard_try_acquire_reject_total.fetch_add(1, std::memory_order_relaxed);
        }
        return std::unexpected(std::move(*err));
    }
    ev.mutation_quota_used_.fetch_add(pending_count, std::memory_order_relaxed);
    // Mirror consume into process Mutations dim for manager dashboards.
    if (ev.resource_quota_mutations_ != 0) {
        (void)aura::core::resource_quota::process_resource_quota().check_and_consume(
            aura::core::resource_quota::Dimension::Mutations, pending_count);
    }
    // Construct via private AcquireTag path (quota already checked).
    return std::unique_ptr<MutationBoundaryGuard>(
        new MutationBoundaryGuard(ev, success_flag, fine_rollback, AcquireTag{},
                                  /*quota_prechecked=*/true));
}

// ── legacy ctor (#1547 / #1556 / #1590) ──────────────────────────────────
Evaluator::MutationBoundaryGuard::MutationBoundaryGuard(Evaluator& ev, bool* success_flag,
                                                        bool fine_rollback) noexcept
    : MutationBoundaryGuard(ev, success_flag, fine_rollback, AcquireTag{},
                            /*quota_prechecked=*/false) {}

// ── shared AcquireTag ctor ───────────────────────────────────────────────
Evaluator::MutationBoundaryGuard::MutationBoundaryGuard(Evaluator& ev, bool* success_flag,
                                                        bool fine_rollback, AcquireTag,
                                                        bool quota_prechecked) noexcept
    : fine_rollback_(fine_rollback)
    , ev_(&ev)
    , flag_(success_flag)
    ,
    // Issue #233 + #236 follow-up: the unique_lock is
    // now a MEMBER of the guard (was previously a local
    // in enter_mutation_boundary() that destructed at
    // function return, releasing the lock immediately).
    //
    // enter_mutation_boundary() now does only the
    // version bump + log-size capture (no lock
    // acquire); this constructor acquires the
    // exclusive write lock and holds it for the
    // entire guard lifetime.
    //
    // NESTED GUARD HANDLING (test_issue_184 Test 5):
    // shared_mutex is NOT recursive, so a nested guard
    // would deadlock on the inner acquire. The fix:
    // only the OUTERMOST guard acquires the lock.
    // Track nesting depth via a member counter; only
    // acquire when depth 0→1, release when 1→0.
    // The depth is shared (static thread_local) so
    // nested guards in the same thread cooperate.
    lock_(ev.workspace_mtx_, std::defer_lock) {
    if (!quota_prechecked) {
        // Issue #1590: soft-fail mutation quota on legacy ctor path.
        if (auto err = ev.check_mutation_quota(1)) {
            (void)err;
            inert_ = true;
            if (flag_)
                *flag_ = false;
            return;
        }
        ev.mutation_quota_used_.fetch_add(1, std::memory_order_relaxed);
    }
    if (flag_)
        *flag_ = true; // optimistic default
    // Issue #1897: capture exception depth so dtor auto-rollback
    // works even if a nested helper throws past a missed catch.
    uncaught_at_enter_ = std::uncaught_exceptions();
    // Issue #236 / #1746: thread_local depth counter keyed by
    // Evaluator::instance_id_ (not address). Each fiber has its
    // own LIFO call stack, so nested guards on a single fiber
    // are always outermost-then-inner (destructed innermost-
    // first). Cross-fiber synchronization happens at unique_lock.
    int* slot = Evaluator::mutation_boundary_depth_slot(ev_);
    int prev = ++(*slot);
    bool outermost = (prev == 1);
    is_outermost_ = outermost;
    if (outermost) {
        // Issue #1253: start hold-time clock for long-mutation policy.
        enter_ts_ = std::chrono::steady_clock::now();
        // Issue #1523: Workspace level in #1388 order (after Mutate).
        aura::compiler::lock_order::on_acquire(aura::compiler::lock_order::Level::Workspace);
        lock_.lock();
        ev_->outermost_mutation_success_flag_ = flag_;
        ev_->bind_yield_hook_evaluator();
        // Issue #354: set the atomic flag so
        // Fiber::yield can detect "yield while
        // holding a mutation boundary". The
        // check is O(1) (atomic load) and the
        // flag is cleared by the Guard dtor
        // (the outermost one only).
        ev_->mutation_boundary_held_.store(true, std::memory_order_release);
        // Issue #1252: coverage counter — every outermost Guard wrap.
        // Issue #1364: mutation × safepoint telemetry (benign race).
        if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics())) {
            m->mutation_boundary_primitives_wrapped.fetch_add(1, std::memory_order_relaxed);
            if (aura::gc_hooks::in_gc_safepoint()) {
                m->mutation_in_safepoint_total.fetch_add(1, std::memory_order_relaxed);
                // Collision = mutation entry observed during active STW flag
                m->safepoint_collision_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (fine_rollback_)
        ev_->request_fine_rollback_for_next_boundary();
    ev_->enter_mutation_boundary();
    // Issue #241: capture panic checkpoint at the OUTERMOST
    // guard only (nested guards share the outer checkpoint).
    // save_panic_checkpoint() snapshots `current-source` so
    // the source can be restored if the mutation rolls back.
    // It returns false if there's no workspace / no source /
    // no (current-source) primitive — in those cases the
    // Guard just skips the checkpoint step.
    if (outermost) {
        had_panic_checkpoint_ = ev_->save_panic_checkpoint();
        // Issue #813: Guard hot path uses explicit Result-style
        // control (success flag / checkpoint bool) — never throws.
        ev_->bump_guard_aura_result_path();
        if (had_panic_checkpoint_)
            ev_->bump_guard_panic_checkpoint_aura_result();
    }
}

// ── destructor ───────────────────────────────────────────────────────────
Evaluator::MutationBoundaryGuard::~MutationBoundaryGuard() {
    if (!ev_ || inert_)
        return; // Issue #1590: quota soft-reject never entered a boundary
    // Issue #1897 / #1818 class: auto-flip success_flag when an
    // exception is unwinding through the Guard and the caller did
    // not mark_failed / set flag=false. Without this, dtor would
    // commit_panic_checkpoint on a partially-mutated workspace.
    if (flag_ && *flag_ && std::uncaught_exceptions() > uncaught_at_enter_) {
        *flag_ = false;
        if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_)) {
            m->mutation_guard_uncaught_auto_rollback_total.fetch_add(1, std::memory_order_relaxed);
            m->mutation_guard_exception_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    bool success = flag_ ? *flag_ : true;
    // exit_mutation_boundary runs under the lock for
    // the outermost guard; lockless for nested guards
    // (lock is held by the outer guard).
    // exit_mutation_boundary runs under the lock for
    // the outermost guard; lockless for nested guards
    // (lock is held by the outer guard).
    int* slot = Evaluator::mutation_boundary_depth_slot(ev_);
    int prev = (*slot)--;
    bool outermost = (prev == 1);
    // Issue #1253 / #1373 / #1375 / #1747 / #1931 / #1953: outermost
    // hold-duration telemetry. Issue #1747/#1931/#1953: compute
    // BatchMutationMetrics locally, then publish with ≤6 atomic writes
    // on the common path (was 15+ scattered fetch_add/CAS on every dtor
    // — cache-line bounce under high-frequency mutate + hot-update).
    // Nested guards skip (no enter_ts_). Issue #1764: enter_ts_ is
    // std::optional — has_value() replaces the fragile
    // time_since_epoch().count()!=0 sentinel.
    if (outermost && enter_ts_.has_value()) {
        const auto dur = std::chrono::steady_clock::now() - *enter_ts_;
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(dur).count();
        const auto uus = static_cast<std::uint64_t>(us > 0 ? us : 0);
        if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics())) {
            // ── local batch (no atomics yet) ──
            struct BatchMutationMetrics {
                std::uint64_t hold_us = 0;
                std::uint64_t holds = 0;
                std::uint64_t holds_over_1ms = 0;
                std::uint64_t too_long = 0;
                std::uint64_t starvation_prevented = 0;
                std::uint64_t extreme = 0;
                std::uint64_t contention_us = 0;
                std::size_t hist_bucket = 0;
                std::uint64_t long_fiber_id = 0;
                bool update_max = false;
                bool force_fail = false;
            } b{};
            b.hold_us = uus;
            b.holds = 1;
            if (uus > 1000)
                b.holds_over_1ms = 1;
            // Issue #1375: 9-bucket hold-time histogram.
            b.hist_bucket = 8; // >1s
            if (uus < 100)
                b.hist_bucket = 0;
            else if (uus < 500)
                b.hist_bucket = 1;
            else if (uus < 1000)
                b.hist_bucket = 2;
            else if (uus < 5000)
                b.hist_bucket = 3;
            else if (uus < 10000)
                b.hist_bucket = 4;
            else if (uus < 50000)
                b.hist_bucket = 5;
            else if (uus < 100000)
                b.hist_bucket = 6;
            else if (uus < 1000000)
                b.hist_bucket = 7;
            // Issue #1493: adaptive safepoint (may touch GC hooks; not a metric atomic).
            ev_->adapt_gc_frequency_from_hold_us(uus);
            // Issue #1443: threshold load (1 relaxed load; not a write).
            const auto max_us = static_cast<std::int64_t>(
                m->long_mutation_threshold_us.load(std::memory_order_relaxed));
            if (us > max_us) {
                b.too_long = 1;
                b.starvation_prevented = 1;
                b.contention_us = uus;
                b.long_fiber_id = aura_fiber_current_id();
                b.update_max = true;
                ::aura_invoke_long_mutation_scheduler_hook(b.long_fiber_id, uus);
                const auto extreme_us = static_cast<std::int64_t>(
                    m->max_extreme_mutation_us.load(std::memory_order_relaxed));
                if (m->long_mutation_strict_mode.load(std::memory_order_relaxed) != 0 &&
                    us > extreme_us) {
                    b.extreme = 1;
                    b.force_fail = true;
                }
            } else {
                // Only publish max if uus might raise it (1 load; CAS later if needed).
                const auto prev_max =
                    m->mutation_hold_duration_us_max.load(std::memory_order_relaxed);
                b.update_max = (uus > prev_max);
            }

            // ── publish common path: ≤6 atomic writes (#1747 / #1931 / #1953) ──
            // 1–4: dual hold counters (legacy #1253 + agent #1373)
            // 5: histogram bucket
            // 6: max (CAS loop when raised; Issue #1765 — no load+store)
            m->mutation_hold_duration_us_total.fetch_add(b.hold_us, std::memory_order_relaxed);
            m->mutation_hold_samples.fetch_add(b.holds, std::memory_order_relaxed);
            m->mutation_boundary_hold_time_total_us.fetch_add(b.hold_us, std::memory_order_relaxed);
            m->mutation_boundary_holds_total.fetch_add(b.holds, std::memory_order_relaxed);
            m->mutation_boundary_hold_histogram[b.hist_bucket].fetch_add(1,
                                                                         std::memory_order_relaxed);
            if (b.update_max) {
                // Issue #1765: CAS loop so a concurrent higher sample
                // cannot be overwritten by a lower load+store race.
                auto prev_max = m->mutation_hold_duration_us_max.load(std::memory_order_relaxed);
                while (b.hold_us > prev_max &&
                       !m->mutation_hold_duration_us_max.compare_exchange_weak(
                           prev_max, b.hold_us, std::memory_order_relaxed)) {
                }
            }
            // Optional / rare path atomics (not on every dtor).
            if (b.holds_over_1ms)
                m->mutation_boundary_holds_over_1ms_total.fetch_add(b.holds_over_1ms,
                                                                    std::memory_order_relaxed);
            if (b.too_long) {
                m->mutation_too_long_total.fetch_add(b.too_long, std::memory_order_relaxed);
                m->starvation_prevented_count.fetch_add(b.starvation_prevented,
                                                        std::memory_order_relaxed);
                m->last_long_mutation_fiber_id.store(b.long_fiber_id, std::memory_order_relaxed);
                m->last_long_mutation_duration_us.store(b.hold_us, std::memory_order_relaxed);
                m->mutation_boundary_contention_us_hist.fetch_add(b.contention_us,
                                                                  std::memory_order_relaxed);
                if (b.extreme)
                    m->long_mutation_extreme_total.fetch_add(1, std::memory_order_relaxed);
                if (b.force_fail && flag_)
                    *flag_ = false;
            }
            // Export-ready: one load + conditional store (avoid write every dtor).
            if (m->runtime_obs_export_ready.load(std::memory_order_relaxed) == 0)
                m->runtime_obs_export_ready.store(1, std::memory_order_relaxed);
        }
    }
    // Issue #1461: Agent Decision Metrics liveness — outermost
    // failed Guard must bump the fiber-boundary rollback counter
    // so (agent:decision-metrics) / stats facade see a real signal
    // (not a dead zero). Nested guards do not bump (outer owns the
    // transaction outcome).
    if (outermost && !success)
        ev_->bump_mutation_boundary_rollback();
    ev_->exit_mutation_boundary(success);
    // Issue #1486 / #1545 / #1568 / #1634: post-boundary linear closed-loop.
    // Unified consistency: scan Moved captures + enforce_all +
    // epoch fence + GC root audit (only_if_moved for Guard exit).
    // #1634: on failure, force full linear_post_mutate_enforce_all +
    // probe active closures so rollback cannot leave dangling
    // linear/JIT state after dual-epoch restore.
    if (outermost) {
        if (!success) {
            // Issue #1951: 4-step closed-loop pattern
            // (linear_post_mutate_enforce_all + enforce_linear_boundary_consistency +
            //  walk_active_closures + guard_failure_linear_enforce_total bump)
            // consolidated into 1 helper call. See evaluator_gc.cpp impl.
            (void)ev_->enforce_linear_post_failure(Evaluator::kLinearGcRootAuditTypedMutate);
        } else {
            (void)ev_->enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditTypedMutate,
                                                           /*mark_all_linear=*/false);
        }
    }
    // Issue #1500: after restamp_all_node_generations inside
    // exit_mutation_boundary, pinned StableNodeRefs still hold
    // the pre-boundary gen — batch refresh them under the
    // still-held outermost write lock so Agent long-held pins
    // remain usable across the Guard boundary.
    if (outermost) {
        (void)ev_->restamp_pinned_stable_refs();
    }
    // Issue #285: explicit flush at the boundary exit so any
    // pending mutation stack state is visible to other fibers
    // BEFORE we drop the write lock. The flush runs
    // lockless (no shared_mutex acquire) and is cheap.
    // Only the outermost guard runs the flush; nested guards
    // don't need it (the outer guard handles visibility).
    if (outermost) {
        ev_->flush_mutation_boundary();
    }
    if (outermost) {
        // Issue #354: clear the flag that
        // Fiber::yield reads to detect "yield
        // while holding a mutation boundary".
        // We clear BEFORE releasing the write
        // lock so any concurrent Fiber::yield
        // observes the cleared flag (acquire
        // ordering on the flag load is
        // synchronized with the release
        // ordering on this store).
        ev_->mutation_boundary_held_.store(false, std::memory_order_release);
        lock_.unlock();
        // Issue #1523: pair Workspace acquire in ctor.
        aura::compiler::lock_order::on_release(aura::compiler::lock_order::Level::Workspace);
        ev_->outermost_mutation_success_flag_ = nullptr;
        ev_->unbind_yield_hook_evaluator();
    }
    // Issue #241: panic-checkpoint commit / restore.
    // Only the outermost guard owns the checkpoint;
    // nested guards (which can't fail independently
    // of their outer) don't touch it.
    if (outermost && had_panic_checkpoint_) {
        if (success) {
            // Mutation succeeded — checkpoint is no
            // longer needed; clear so the next
            // mutation starts fresh.
            ev_->commit_panic_checkpoint();
        } else if (ev_->panic_auto_rollback_) {
            // Mutation failed + auto-rollback enabled —
            // restore the saved source via (set-code).
            ev_->restore_panic_checkpoint();
        }
        // else: failed + !auto-rollback — leave the
        // checkpoint alive so a subsequent retry can
        // roll back to it. (Pre-#241 behavior on
        // failure was to leave the checkpoint.)
    }
    // Issue #417 / #1766: verify stack/depth-slot consistency
    // after boundary exit (cross-TU drift detection).
    // Depth was already decremented above (prev = (*slot)--).
    // ensure_mutation_invariants / ensure_hygiene_violation_detection
    // / probe_arena_auto_policy_on_boundary_exit are all noexcept
    // (Issue #1766) — they cannot throw past remaining dtor work
    // without std::terminate. try/catch is intentionally not used.
    ev_->ensure_mutation_invariants();
    // Issue #422: hygiene violation detection hook on
    // Guard exit (mutate paths record attempts at block).
    ev_->ensure_hygiene_violation_detection();
    // Issue #464: bump the ArenaGroup
    // auto_compact_guard_call_count_ counter on
    // every guard dtor (the closed-loop signal for
    // long AI sessions). The actual
    // auto_compact_with_safety() call is wired in
    // #464 follow-up commits (Cycle 2) when the
    // fiber-safety check + safe-point integration
    // are in place. For now: counter only.
    // Issue #464: bump the ArenaGroup
    // auto_compact_guard_call_count_ counter on
    // every outermost guard exit (the closed-loop
    // signal for long AI sessions). The actual
    // auto_compact_with_safety() call is wired in
    // #464 follow-up commits (Cycle 2) when the
    // fiber-safety check + safe-point integration
    // are in place. For now: counter only.
    //
    // We bump on every outermost exit (regardless
    // of success) so the agent can monitor
    // mutation attempts (success + failure). The
    // success/failure distinction is a #464
    // follow-up. The counter is the precondition
    // that the AI Agent can monitor.
    if (outermost && ev_->arena_group_) {
        ev_->probe_arena_auto_policy_on_boundary_exit(success);
    }
    // Issue #490 / #1503: proactive Evaluator tag_arity_index
    // maintenance on successful outermost Guard exit:
    //   - EagerAfterMutate: always rebuild/sync
    //   - Lazy + warm index: auto incremental sync so the next
    //     query:pattern after self-mutate stays O(dirty), not a
    //     surprise O(N) full rebuild on large ASTs
    if (outermost && success && ev_->workspace_flat_) {
        const bool eager = ev_->pattern_index_policy_ == PatternIndexPolicy::EagerAfterMutate;
        const bool warm_lazy = ev_->pattern_index_policy_ == PatternIndexPolicy::Lazy &&
                               ev_->tag_arity_index_is_warm();
        if (eager || warm_lazy) {
            if (warm_lazy)
                ev_->bump_pattern_index_auto_warm_syncs();
            ev_->build_tag_arity_index(
                static_cast<std::uint8_t>(eager ? PatternIndexRebuildTrigger::EagerMutate
                                                : PatternIndexRebuildTrigger::LazyQuery));
        }
    }
    // Issue #1252: post-mutate linear ownership revalidate on
    // successful outermost Guard exit (#672 path made mandatory).
    if (outermost && success) {
        ev_->bump_linear_post_mutate_enforcement();
        if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics())) {
            m->mutation_boundary_linear_revalidations.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (outermost && !success) {
        if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics())) {
            m->mutation_boundary_steal_recoveries.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #1255: on Guard exit, if hygiene drift was seen,
    // force DefUseIndex sync before releasing the boundary.
    if (outermost && ev_->workspace_flat_) {
        const auto dirty = ev_->workspace_flat_->mark_dirty_upward_call_count();
        if (dirty > 0) {
            if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics())) {
                m->pattern_hygiene_defuse_sync_on_guard.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    // unique_lock destructor runs automatically here.
}

// ── enable_fine_rollback (instance) ──────────────────────────────────────
void Evaluator::MutationBoundaryGuard::enable_fine_rollback() noexcept {
    if (!ev_ || !ev_->workspace_flat_)
        return;
    auto& stack = ev_->active_mutation_stack();
    if (stack.empty())
        return;
    auto& cp = stack.back();
    if (cp.fine_rollback)
        return;
    cp.fine_rollback = true;
    cp.sym_id_snapshot = ev_->workspace_flat_->snapshot_sym_id();
    cp.param_snapshot = ev_->workspace_flat_->snapshot_param_columns();
    fine_rollback_ = true;
}

// ── move (#1767) ─────────────────────────────────────────────────────────
Evaluator::MutationBoundaryGuard::MutationBoundaryGuard(MutationBoundaryGuard&& o) noexcept
    : had_panic_checkpoint_(o.had_panic_checkpoint_)
    , fine_rollback_(o.fine_rollback_)
    , atomic_batch_active_(o.atomic_batch_active_)
    , suppress_bump_(o.suppress_bump_)
    , is_outermost_(o.is_outermost_)
    , inert_(o.inert_)
    , enter_ts_(std::move(o.enter_ts_))
    , uncaught_at_enter_(o.uncaught_at_enter_)
    , ev_(o.ev_)
    , flag_(o.flag_)
    , lock_(std::move(o.lock_)) {
    o.had_panic_checkpoint_ = false;
    o.fine_rollback_ = false;
    o.atomic_batch_active_ = false;
    o.suppress_bump_ = false;
    o.is_outermost_ = false;
    o.inert_ = false;
    o.enter_ts_.reset();
    o.uncaught_at_enter_ = 0;
    o.ev_ = nullptr;
    o.flag_ = nullptr;
}

Evaluator::MutationBoundaryGuard&
Evaluator::MutationBoundaryGuard::operator=(MutationBoundaryGuard&& o) noexcept {
    if (this != &o) {
        // Issue #1767: full release of *this via move-to-local
        // so ~MutationBoundaryGuard runs (depth, lock, metrics,
        // checkpoint). exit_mutation_boundary alone would miss
        // the depth-slot decrement.
        if (ev_) {
            MutationBoundaryGuard doomed{std::move(*this)};
            (void)doomed;
        }
        had_panic_checkpoint_ = o.had_panic_checkpoint_;
        fine_rollback_ = o.fine_rollback_;
        atomic_batch_active_ = o.atomic_batch_active_;
        suppress_bump_ = o.suppress_bump_;
        is_outermost_ = o.is_outermost_;
        inert_ = o.inert_;
        enter_ts_ = std::move(o.enter_ts_);
        uncaught_at_enter_ = o.uncaught_at_enter_;
        ev_ = o.ev_;
        flag_ = o.flag_;
        lock_ = std::move(o.lock_);
        o.had_panic_checkpoint_ = false;
        o.fine_rollback_ = false;
        o.atomic_batch_active_ = false;
        o.suppress_bump_ = false;
        o.is_outermost_ = false;
        o.inert_ = false;
        o.enter_ts_.reset();
        o.uncaught_at_enter_ = 0;
        o.ev_ = nullptr;
        o.flag_ = nullptr;
    }
    return *this;
}

} // namespace aura::compiler
