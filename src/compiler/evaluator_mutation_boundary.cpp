// evaluator_mutation_boundary.cpp — Wave 3a/4: MutationBoundaryGuard + enter/exit_mutation_boundary
// out-of-line aura.compiler.evaluator module partition.
//
// Nested class declaration remains in evaluator.ixx (needs private Evaluator
// access). Heavy RAII paths (try_acquire, AcquireTag ctor, dtor, move,
// enable_fine_rollback) and enter/exit_mutation_boundary live here so
// evaluator.ixx stays a thinner interface.

module;

// Issue #221: PCV header in GMF (same as evaluator.ixx) so enter_mutation_boundary
// can name PersistentChildVector in the checkpoint snapshot type.
#include "../core/persistent_child_vector.hh"
#include "observability_metrics.h"
#include "lock_order_audit.h"
#include "core/gc_hooks.h"
#include "core/resource_quota.hh"
#include "security_capabilities.h"        // aura_fiber_current_id
#include "aura_jit_bridge.h"              // aura_invoke_long_mutation_scheduler_hook
#include "typed_mutation_audit.h"         // Issue #1589 / #1614 / #1894
#include "core/arena_auto_policy_stats.h" // in_render_hotpath
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <utility>
#include <vector>

module aura.compiler.evaluator;

import std;
import aura.core.envframe_lifetime;
import aura.core.lifetime_pin;

namespace aura::compiler {

// ── enter / exit mutation boundary (Wave 4) ──────────────────────────────
// Called from MutationBoundaryGuard ctor/dtor. Bodies moved out of
// evaluator.ixx so the interface unit no longer carries ~370 lines of
// checkpoint / rollback / typed-audit / impact telemetry.

void Evaluator::enter_mutation_boundary() {
    // Issue #233: the workspace_mtx_ lock was previously
    // acquired HERE as a local unique_lock that destructed
    // at function return, releasing the lock immediately.
    // That meant mutate:* primitives ran UNLOCKED — the
    // MutationBoundaryGuard's whole purpose was defeated.
    //
    // The lock is now held by MutationBoundaryGuard as a
    // member (so it survives across enter + body + exit).
    // enter_mutation_boundary() no longer acquires the
    // lock; it just does the version bump + log-size
    // capture. The guard's destructor releases the lock
    // after exit_mutation_boundary() runs.
    //
    // The bump performed by enter_mutation_boundary() is
    // a release-store (publishes any writes the caller
    // will make under the boundary to acquirers on other
    // threads); the version increment is release (publishes any
    // writes the caller will make under the boundary to acquirers
    // on other threads).
    std::size_t log_size = workspace_flat_ ? workspace_flat_->all_mutations().size() : 0;
    // Issue #1355: inside render hot path, use lightweight checkpoint —
    // no full children_ snapshot, field mutations go to side log.
    const bool lightweight =
        aura::core::arena_policy::in_render_hotpath() && workspace_flat_ != nullptr;
    // Issue #221: capture the per-node children_ vector. The
    // PCV's COW semantics make this a cheap copy (each PCV
    // is a shared_ptr to immutable storage; the snapshot
    // holds shared_ptrs that keep the pre-mutation PCs alive).
    std::vector<aura::ast::PersistentChildVector<aura::ast::NodeId>> children_snapshot;
    bool fine_rollback = fine_rollback_for_next_boundary_ && !lightweight;
    fine_rollback_for_next_boundary_ = false;
    std::pmr::vector<aura::ast::SymId> sym_id_snapshot;
    aura::ast::FlatAST::ParamColumnsSnapshot param_snapshot;
    bool bump_suppressed_at_entry = false;
    std::uint64_t macro_introduced_count_at_entry = 0;
    std::uint16_t flat_generation_at_entry = 0;
    if (workspace_flat_) {
        if (lightweight) {
            workspace_flat_->begin_render_lightweight_checkpoint();
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                m->mutation_lightweight_total.fetch_add(1, std::memory_order_relaxed);
        } else {
            children_snapshot = workspace_flat_->snapshot_children();
            if (fine_rollback) {
                sym_id_snapshot = workspace_flat_->snapshot_sym_id();
                param_snapshot = workspace_flat_->snapshot_param_columns();
            }
        }
        bump_suppressed_at_entry = workspace_flat_->atomic_batch_active();
        flat_generation_at_entry = workspace_flat_->generation();
    }
    MutationCheckpoint cp{defuse_version_.load(std::memory_order_acquire),
                          log_size,
                          bump_suppressed_at_entry,
                          macro_introduced_count_at_entry,
                          flat_generation_at_entry,
                          std::move(children_snapshot),
                          fine_rollback,
                          std::move(sym_id_snapshot),
                          std::move(param_snapshot),
                          lightweight};
    active_mutation_stack().push_back(std::move(cp));
    const std::size_t depth = active_mutation_stack().size();
    std::uint64_t prev_max = nested_guard_depth_max_.load(std::memory_order_relaxed);
    while (depth > prev_max &&
           !nested_guard_depth_max_.compare_exchange_weak(
               prev_max, depth, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
    if (depth == 1 && workspace_flat_ && !lightweight) {
        for (aura::ast::NodeId id = 0; id < workspace_flat_->size(); ++id) {
            if (workspace_flat_->is_macro_introduced(id))
                ++macro_introduced_count_at_entry;
        }
        active_mutation_stack().back().macro_introduced_count_at_entry =
            macro_introduced_count_at_entry;
    }
    defuse_version_.fetch_add(1, std::memory_order_release);
    // Issue #189: bump the total-mutations counter for
    // observability. Relaxed because it's stats-only.
    total_mutations_.fetch_add(1, std::memory_order_relaxed);
}
// Exit a mutation boundary. Pops the checkpoint. If success
// is true, the version advance is kept; if false, the
// mutations recorded between enter and exit are rolled back
// via the MutationRecord inverse (Issue #213 Cycle 1).
// The lock is released by the unique_lock going out of scope.
//
// Issue #213 Cycle 2 — version-bump invariant:
//   Both success and failure bump the version a second
//   time (legacy behavior: enter + exit = 2 bumps per
//   boundary). The bump is release-store so any pending
//   readers holding a snapshot from before the boundary
//   see a version mismatch and deopt. This invariant
//   matters for primitives that hold a snapshot across
//   the boundary (e.g. JIT-specialized L2 SHAPE_PAIR
//   paths) — they expect 2 bumps per boundary to know
//   the workspace was definitely mutated.
//
// Issue #213 Cycle 1 — rollback path:
//   1. Call workspace_flat_->rollback_to_size(cp.mutation_log_size)
//      to walk the log in reverse and apply the inverse
//      mutation for each record beyond the checkpoint. The
//      inverse is computed by FlatAST::rollback(mutation_id):
//      - For field-level (int_val_/type_id_): restore the
//        old_value at the field_offset.
//      - For subtree-level: mark RolledBack and bump
//        generation. (The actual re-parse + re-attach is
//        done at a higher level by the rollback primitive
//        in the Aura surface layer; see ast.ixx:1488.)
//   2. Invalidate defuse_index_ so the next query rebuilds
//      it from the rolled-back state.
//   3. Bump defuse_version_ again (release-store) so any
//      pending readers holding a snapshot from before the
//      rollback see a version mismatch and deopt.
//   4. Bump total_mutations_ for observability.
//
// Returns the popped checkpoint (or {0} if the stack is
// empty — a defensive fallback for unbalanced calls).
Evaluator::MutationCheckpoint Evaluator::exit_mutation_boundary(bool success) {
    auto& stack = active_mutation_stack();
    if (stack.empty())
        return {0, 0};
    const bool nested_boundary = stack.size() > 1;
    auto cp = stack.back();
    stack.pop_back();
    if (cp.lightweight && workspace_flat_) {
        // Issue #1355: lightweight path — commit or rollback side log.
        if (success) {
            workspace_flat_->commit_render_lightweight_checkpoint();
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                m->mutation_lightweight_commit_total.fetch_add(1, std::memory_order_relaxed);
        } else {
            const auto n = workspace_flat_->rollback_render_lightweight_checkpoint();
            // Also undo any durable log entries (structural ops fall through).
            BoundaryRollbackStats stats;
            stats.field_records_rolled =
                n + workspace_flat_->rollback_to_size(cp.mutation_log_size);
            if (stats.field_records_rolled > 0)
                bump_mutation_log_rollback_count();
            last_boundary_rollback_stats_ = stats;
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                m->mutation_lightweight_rollback_total.fetch_add(1, std::memory_order_relaxed);
            defuse_index_ = nullptr;
        }
    } else if (!success && workspace_flat_) {
        // Roll back the mutations that were appended between
        // enter and exit. The log size captured at entry
        // tells us how far to undo.
        BoundaryRollbackStats stats;
        stats.field_records_rolled = workspace_flat_->rollback_to_size(cp.mutation_log_size);
        // Issue #549: bump mutation_log_rollback_count_ so
        // (query:self-evolution-stability-stats) can report
        // the lifetime # of times the log was actually
        // rolled back (a stricter subset of the lifetime #
        // of failed boundaries; bumps only when there were
        // mutations to undo).
        if (stats.field_records_rolled > 0) {
            bump_mutation_log_rollback_count();
            if (nested_boundary)
                bump_edsl_nested_atomic_rollback();
        }
        // Issue #221: restore the per-node children_ from the
        // pre-mutation snapshot. The checkpoint's children_snapshot
        // holds shared_ptrs to the pre-mutation PCs (PCV COW),
        // so the restoration is O(1) per node.
        // Issue #1281: PCV topology fidelity is mandatory on
        // every failed boundary — restore_children always runs.
        // Issue #1502: restore_children also rebuilds parent_
        // from the restored child lists (full children_/parent_
        // topology), so partial MutationRecord inverse failures
        // cannot leave parent_of() inconsistent with children().
        workspace_flat_->restore_children(std::move(cp.children_snapshot));
        stats.children_column_restored = true;
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
            m->children_topology_rollback_count.fetch_add(1, std::memory_order_relaxed);
            // Issue #1502: parent topology restored with children.
            m->parent_topology_rollback_count.fetch_add(1, std::memory_order_relaxed);
        }
        // Issue #266: restore sym_id_ / param columns for bulk
        // rename operations when fine rollback was requested.
        if (cp.fine_rollback) {
            workspace_flat_->restore_sym_id(std::move(cp.sym_id_snapshot));
            workspace_flat_->restore_param_columns(std::move(cp.param_snapshot));
            stats.sym_id_column_restored = true;
            stats.param_columns_restored = true;
        }
        // Issue #679: realign atomic-batch suppressed flag if a
        // nested path left it inconsistent with the snapshot.
        if (workspace_flat_->atomic_batch_active() != cp.bump_suppressed_at_entry) {
            if (cp.bump_suppressed_at_entry)
                workspace_flat_->begin_atomic_batch();
            else
                workspace_flat_->rollback_atomic_batch();
            suppressed_misalign_caught_.fetch_add(1, std::memory_order_relaxed);
        }
        if (stats.children_column_restored && cp.macro_introduced_count_at_entry > 0) {
            macro_rollback_hits_.fetch_add(1, std::memory_order_relaxed);
        }
        last_boundary_rollback_stats_ = stats;
        // Invalidate the def-use index — the workspace state
        // is now different from what the index reflects.
        defuse_index_ = nullptr;
    }
    // Issue #273: structural mutates bump generation_; refresh all
    // live node_gen_ entries so subsequent eval_flat paths see
    // valid NodeIds (including unrelated workspace defines).
    // Issue #1282: restamp also consumes auto_restamp_pending_
    // after a generation wrap so live node_gen_ recovers.
    if (workspace_flat_) {
        const bool wrap_pending = workspace_flat_->auto_restamp_pending();
        workspace_flat_->restamp_all_node_generations();
        if (wrap_pending) {
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                m->generation_auto_restamp_on_wrap.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #1283: unified provenance capture at Guard boundary exit.
    // Stamps defuse_version / mutation impact into Agent-visible metrics
    // so closed-loop self-evo can blame dirty nodes on this boundary.
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
        m->provenance_boundary_capture_count.fetch_add(1, std::memory_order_relaxed);
    // Issue #1638: mutation_log compact at boundary exit (success
    // path only — failure path already rolls back via
    // rollback_to_size, so the log is already shrunk). Threshold
    // gate avoids the shrink_to_fit cost on small log states
    // (heavy-mutation safety net — 200MB+/day reclaim in long-
    // running Agent scenarios per the open mutation-log-growth
    // issue). Cheap when under threshold (single size() read).
    if (success && workspace_flat_) {
        static constexpr std::size_t kCompactThreshold = 64 * 1024; // 64KB
        if (workspace_flat_->mutation_log_size() > kCompactThreshold)
            compact_mutation_log();
    }
    // Bump version on both success and failure (legacy
    // invariant: 2 bumps per boundary). The lock is
    // released by the unique_lock going out of scope.
    defuse_version_.fetch_add(1, std::memory_order_release);
    // Issue #189: bump the total-mutations counter for
    // observability. Relaxed because it's stats-only.
    // We bump it even on rollback so dashboards can see
    // "the boundary attempted to mutate, then rolled back".
    total_mutations_.fetch_add(1, std::memory_order_relaxed);
    // Issue #550 / #518: narrowing_refresh_count_ is
    // bumped from TypeChecker::infer_flat_partial's
    // reanalyze_occurrence_contexts path (actual
    // OccurrenceInfoFlat refresh), not here.
    // Issue #551: bump impact_snapshot_count_ on every
    // successful Guard exit — mirrors the post-mutate
    // impact snapshot the AI loop reads for adaptive
    // strategy. Stats-only (relaxed-ordering); the
    // follow-up wires the actual snapshot collection
    // (dirty_nodes_in_snapshot_, marker delta, epoch
    // change, affected roots via StableNodeRef).
    bump_impact_snapshot_count();
    // Issue #555: bump guard_dirty_epoch_count_ on
    // every successful Guard exit — measures the
    // Guard + type cache integration. Pairs with
    // dirty_propagation_count_ (bumped in
    // mark_dirty_upward) so the AI Agent can compute
    // propagation_ratio = dirty_propagation / guard_dirty_epoch
    // (close to 1.0 = every Guard exit propagates).
    bump_guard_dirty_epoch_count();
    // Issue #672: every successful Guard exit is itself a
    // linear ownership enforcement event — bump the
    // post-mutate enforcement counter so the AI Agent can
    // gauge how often Guard exits propagate through
    // (query:linear-ownership-enforcement-stats). Pairs
    // with bump_guard_dirty_epoch_count() above so the
    // Agent can compute enforcement_ratio =
    // linear_post_mutate_enforcements / guard_dirty_epoch.
    bump_linear_post_mutate_enforcement();
    // Issue #555 / #518: selective_recheck_count_ is
    // bumped from infer_flat_partial's
    // reanalyze_occurrence_contexts path, not here.
    // Issue #456: record mutation-impact summary on
    // success only. Walk the workspace mutation log
    // from `mutation_log_size` (pre-mutation) to
    // current size (post-mutation) and count entries.
    // Skip on rollback (the rolled-back mutations
    // don't actually affect state).
    //
    // P0: the per-record DirtyReason bitmask is NOT
    // stored on MutationRecord (issue #188 stores it
    // on the AST node's dirty_ column, not on the
    // log entry). So we count log entries (= nodes
    // touched) and use the defuse_version_ delta as
    // the "reasons seen" surrogate: any delta >= 2
    // implies a structural change (kStructuralDirty
    // equivalent). Follow-up: extend MutationRecord
    // to carry a dirty_reasons byte so we can OR the
    // actual reasons in here.
    if (success && workspace_flat_) {
        const auto post_size = workspace_flat_->all_mutations().size();
        std::uint64_t nodes_changed = 0;
        if (post_size > cp.mutation_log_size) {
            nodes_changed = post_size - cp.mutation_log_size;
        }
        const std::uint64_t epoch_after = defuse_version_.load(std::memory_order_acquire);
        const std::uint64_t epoch_delta = epoch_after - cp.version;
        // Surrogate reasons mask: bit 0 = any node was
        // touched (kGeneralDirty equivalent).
        // Higher bits reserved for follow-up
        // MutationRecord reason bytes.
        const std::uint8_t reasons_mask = nodes_changed > 0 ? 0x01 : 0x00;
        mutation_impact_count_.fetch_add(1, std::memory_order_relaxed);
        if (nodes_changed > 0) {
            mutation_impact_nodes_changed_total_.fetch_add(nodes_changed,
                                                           std::memory_order_relaxed);
        }
        // OR the new reasons into the running mask
        // (relaxed atomic CAS loop; the mask is for
        // observability only).
        std::uint64_t cur = mutation_impact_reasons_seen_mask_.load(std::memory_order_relaxed);
        while (!mutation_impact_reasons_seen_mask_.compare_exchange_weak(
            cur, cur | reasons_mask, std::memory_order_relaxed)) {
        }
        // Append to the ring buffer (lockless; the
        // 8-slot ring tolerates torn writes from
        // concurrent boundaries — worst case is one
        // stale entry visible to (query:mutation-impact)
        // for one read, which is acceptable for
        // observability). We index by ring_seq_
        // modulo the ring size.
        const auto seq = mutation_impact_ring_seq_.fetch_add(1, std::memory_order_relaxed);
        auto& slot = mutation_impact_ring_[seq % kMutationImpactRingSize];
        slot.epoch_after = epoch_after;
        slot.epoch_delta = epoch_delta;
        slot.nodes_changed = nodes_changed;
        slot.reasons_mask = reasons_mask;
        // Issue #676: security audit event for successful mutations.
        std::string_view audit_op = "structural";
        ast::NodeId audit_target = ast::NULL_NODE;
        const auto& log = workspace_flat_->all_mutations();
        if (post_size > cp.mutation_log_size && post_size <= log.size()) {
            const auto& rec = log[post_size - 1];
            audit_op = rec.operator_name;
            audit_target = rec.target_node;
        }
        emit_mutation_audit(static_cast<std::uint32_t>(nodes_changed),
                            static_cast<std::uint32_t>(epoch_delta), audit_op, audit_target);
        // Issue #1589 / #1614 / #1894: TypedMutationAudit trail + real
        // invariant suite on mutation boundary hot path. Contextual
        // sampling (#1894) forces audit for large dirty scopes / linear.
        // Under Full strategy, invariant failure converts this success
        // path into a structural rollback (do not silently continue).
        {
            const std::uint64_t mid = total_mutations_.load(std::memory_order_relaxed);
            const auto fid = static_cast<std::int64_t>(aura_fiber_current_id());
            const bool linear_hint = (audit_op.find("linear") != std::string_view::npos) ||
                                     (audit_op.find("move") != std::string_view::npos) ||
                                     (audit_op.find("inline") != std::string_view::npos);
            if (nodes_changed > 0 &&
                typed_audit::should_audit_contextual(mid, nodes_changed, linear_hint)) {
                const bool inv_ok = run_typed_mutation_invariant_audit(
                    mid, audit_op, static_cast<std::uint32_t>(audit_target), cp.version,
                    epoch_after);
                // #1894: Full strategy → force rollback on any invariant fail.
                if (!inv_ok && typed_audit::get_strategy() == typed_audit::AuditStrategy::Full) {
                    typed_audit::g_typed_mutation_audit_counters.full_strategy_force_rollback_total
                        .fetch_add(1, std::memory_order_relaxed);
                    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                        m->typed_mutation_full_force_rollback_total.fetch_add(
                            1, std::memory_order_relaxed);
                    // Structural undo (same as failure path below).
                    BoundaryRollbackStats stats;
                    stats.field_records_rolled =
                        workspace_flat_->rollback_to_size(cp.mutation_log_size);
                    if (stats.field_records_rolled > 0)
                        bump_mutation_log_rollback_count();
                    workspace_flat_->restore_children(std::move(cp.children_snapshot));
                    stats.children_column_restored = true;
                    if (cp.fine_rollback) {
                        workspace_flat_->restore_sym_id(std::move(cp.sym_id_snapshot));
                        workspace_flat_->restore_param_columns(std::move(cp.param_snapshot));
                        stats.sym_id_column_restored = true;
                        stats.param_columns_restored = true;
                    }
                    last_boundary_rollback_stats_ = stats;
                    defuse_index_ = nullptr;
                    typed_audit::record_boundary_outcome(
                        mid, "invariant-force-rollback", cp.version, epoch_after,
                        /*success=*/false, static_cast<std::uint32_t>(audit_target), 0, fid);
                    // Skip success-path reflect; failure path already recorded.
                    return cp;
                }
            } else {
                typed_audit::record_boundary_outcome(
                    mid, audit_op, cp.version, epoch_after, /*success=*/true,
                    static_cast<std::uint32_t>(audit_target),
                    static_cast<std::uint32_t>(nodes_changed), fid);
            }
        }
        // Issue #488: post-mutate reflect validation + snapshot fields.
        // (Also covered as provenance leg of #1614 invariant audit when sampled.)
        (void)post_mutation_reflect_validate();
    } else if (!success) {
        // Issue #1589: TypedMutationAudit rollback trail.
        const std::uint64_t epoch_after = defuse_version_.load(std::memory_order_acquire);
        const std::uint64_t mid = total_mutations_.load(std::memory_order_relaxed);
        const auto fid = static_cast<std::int64_t>(aura_fiber_current_id());
        typed_audit::record_boundary_outcome(mid, "rollback", cp.version, epoch_after,
                                             /*success=*/false, 0, 0, fid);
    }
    return cp;
}


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
        // Issue #2000: restamp surviving pinned FFI buffers at boundary
        // exit so pins that outlived the boundary keep tracking the
        // current gen / arena id. Pins that compact_sweep invalidated
        // (ptr=null) are skipped (restamp early-return on !pinned).
        const auto n_pins = aura::core::lifetime::restamp_all_pins_for_arena(0, 0);
        if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics())) {
            if (n_pins > 0)
                m->lifetime_pin_restamps_total.fetch_add(static_cast<std::uint64_t>(n_pins),
                                                         std::memory_order_relaxed);
        }
        // Issue #2003: EnvFrame explicit lifetime protocol — guard runs
        // scan_skip_freed_closures + bumps site-tagged counter on dtor
        // (boundary exit). Instantiated here (only when outermost since
        // nested guards don't own their own boundary) so the scan runs
        // after restamp completes + before we drop the write lock below.
        if (outermost) {
            aura::core::envframe_lifetime::EnvFrameLifetimeGuard envframe_guard{
                aura::core::envframe_lifetime::make_envframe_lifetime_host_with(
                    const_cast<void*>(static_cast<const void*>(ev_)),
                    &::aura::compiler::Evaluator::envframe_lifetime_trampoline),
                aura::core::envframe_lifetime::EnvFrameLifetimeSite::BoundaryExit};
            (void)envframe_guard.site();
        }
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
