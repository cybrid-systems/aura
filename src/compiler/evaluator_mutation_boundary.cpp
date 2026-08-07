// evaluator_mutation_boundary.cpp — Wave 3a/4: MutationBoundaryGuard + enter/exit_mutation_boundary
// out-of-line aura.compiler.evaluator module partition.
//
// Nested class declaration remains in evaluator.ixx (needs private Evaluator
// access). Heavy RAII paths (try_acquire, AcquireTag ctor, dtor, move,
// enable_fine_rollback) and enter/exit_mutation_boundary live here so
// evaluator.ixx stays a thinner interface.
//
// ═══════════════════════════════════════════════════════════════════════════
// OWNERSHIP BOUNDARY (Issue #2678)
// ═══════════════════════════════════════════════════════════════════════════
// This file is fragile to accidental truncation under C++20 module merge.
// Module import block MUST be contiguous after `module aura.compiler.evaluator;`
// (see check_module_import_contiguity_2678.py). Critical-path sections:
//
//   1. try_acquire / try_acquire_for_region    (~L1024-1272)
//   2. enter/exit_mutation_boundary             (~L155-987)
//   3. MutationBoundaryGuard methods            (~L1024-1848)
//   4. Phase-5 densify + layout-stamp fence    (~L2083-3278)
//   5. restamp_all_pins_for_arena (call site)  (~L1848)
//
// DO NOT split this file unless adding an explicit ownership boundary marker
// at the top of each new TU. DO NOT add bulk restamp/invalidate free functions
// to lifetime_pin.hh — they live only in lifetime_pin.ixx (sharded registry,
// #2342/#2375). See check_module_import_contiguity_2678.py for lint gate.
// ═══════════════════════════════════════════════════════════════════════════

module;

// Issue #221: PCV header in GMF (same as evaluator.ixx) so enter_mutation_boundary
// can name PersistentChildVector in the checkpoint snapshot type.
#include "../core/persistent_child_vector.hh"
#include "../core/layout_stamp.hh" // Issue #2170: LayoutStamp capture + publisher
// lifetime_pin.hh omitted: import aura.core.lifetime_pin provides
// restamp_all_pins_for_arena; dual include+import makes the call ambiguous.
#include "../core/workspace_epoch.hh"    // Issue #2170: current_mutation_epoch() for capture
#include "coercion_provenance_policy.hh" // Issue #2640: g_coercion_provenance_miss_force_audit_total + blame_soft_escalate_* + consume_provenance_miss_for_boundary

#include "observability_metrics.h"
#include "lock_order_audit.h"
#include "gc_coord_scope.h" // Issue #2131: pin → cascade → audit
#include "core/gc_hooks.h"
#include "core/resource_quota.hh"
#include "security_capabilities.h"          // aura_fiber_current_id
#include "aura_jit_bridge.h"                // aura_invoke_long_mutation_scheduler_hook
#include "ownership_escape_lowering_gate.h" // Issue #2309: aura_escape_move_gate_clear + rollback counter
#include "compiler/ownership_rebind.h" // Issue #2695: unified OwnershipEnv rebind API post-densify/steal/Agent
                                       // + aura_aot_func_table_epoch +
                                       //   aura_jit_batch_deopt_for (+ empty-name
                                       //   deopt-all, Issue #2162)
#include "compiler/hot_update_registry.hh"   // Issue #2090: AuraJITHotUpdateRegistry
                                             //   C-linkage shims —
                                             // aura_hot_update_should_throttle_reemit
                                             // aura_hot_update_on_reemit_throttled
                                             // aura_hot_update_notify_epoch_bump
                                             // aura_hot_update_reemit_provider_wired
                                             // aura_reemit_aot_for_dirty
#include "typed_mutation_audit.h"            // Issue #1589 / #1614 / #1894 / #2145
#include "core/sandbox.hh"                   // Issue #2145 Strict hard-gate
#include "core/provenance_tracker.hh"        // Issue #2222: boundary LinearEnforce Strict hold
#include "core/arena_auto_policy_stats.h"    // in_render_hotpath
#include "core/densify_consistency_report.h" // Issue #2341: DensifyConsistencyReport + counter
#include "core/moving_densify_health.hh"     // Issue #2619: Agent Moving densify health
#include "mutation_boundary_shared_exit.h"   // Issue #2600: shared exit helper (soft + full Guard)
#include "core/post_compact_lifecycle.hh"    // Issue #2436: canonical post-compact order
#include "compiler/frame_budget.hh"          // Issue #2137 frame-budget cascade isolation
#include "compiler/mutation_hold_budget.h"   // Issue #2313: mutation_hold_budget_us()
#include "serve/fiber.h"                     // Issue #2184: publish MutationSafetySnapshot
#include "serve/multi_fiber_mailbox.h"       // Issue #2347: clear recv boundary reject window
#include "compiler/shape_profiler.h"         // Issue #2255: current_global_shape_version
#include "orch/security_schedule_gate.h"     // Issue #2630: evaluate_security_schedule admit
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module aura.compiler.evaluator;

// Issue #2678: module import block MUST be contiguous after
// `module aura.compiler.evaluator;` — see check_module_import_contiguity_2678.py.
// Do NOT add blank lines between import statements (triggers module contiguity
// rule violation + linter reject). Inline comments on the same line are OK.
import aura.core.lifetime_pin;
import aura.compiler.coercion_map;        // Issue #2102: provenance-miss force-audit
import aura.compiler.root_remap_pass;     // Issue #2341: last_root_remap_any_fail
import aura.compiler.ir_soa;              // Issue #2432: current_ir_soa_generation_fence
import aura.compiler.type_checker;        // Issue #2608: maybe_persist_occurrence_snapshot
import aura.compiler.optimization_passes; // Issue #2674: layered evidence-coherence

extern "C" void aura_periodic_epoch_invariant_walk_if_due(void);

// Issue #2640: production Restricted default periodic epoch-invariant soft walk
// (gated by mode=Soft + production_defaults_active + steady_ms rate limit;
// cheap on the quiet path, runs the existing #2541 soft walk when due).
// Note: do NOT stub TypeChecker here — module import aura.compiler.type_checker
// is authoritative (#2641 / f0d7ca50).

// Issue #2021: snapshot macro depth / concurrent peak into CompilerMetrics
// on outermost MutationBoundaryGuard exit (module-safe C entry).
extern "C" void aura_macro_hygiene_snapshot_metrics(void* metrics_ptr) noexcept;
// Issue #2210: JIT/Interpreter equivalence oracle (C ABI from ir_cache_pure).
extern "C" int aura_jit_equivalence_enabled(void) noexcept;
extern "C" int aura_check_primcall_equivalence(std::uint64_t interp_bits,
                                               std::uint64_t jit_bits) noexcept;
extern "C" std::uint64_t aura_jit_equivalence_runs_v_read(void) noexcept;
extern "C" std::uint64_t aura_jit_equivalence_ok_v_read(void) noexcept;
extern "C" std::uint64_t aura_jit_equivalence_mismatch_v_read(void) noexcept;
extern "C" std::uint64_t aura_jit_equivalence_deopt_force_v_read(void) noexcept;

// Issue #2641: C ABI for outermost-success OccurrenceGoal persist (tests + dtor).
// Soft / env=0 / no type-checker → zero cost inside maybe_persist_occurrence_snapshot.
extern "C" void aura_outermost_success_persist_occurrence(void* ev_ptr,
                                                          std::uint64_t mutation_id) noexcept {
    if (!ev_ptr)
        return;
    auto* ev = static_cast<aura::compiler::Evaluator*>(ev_ptr);
    if (auto* tc = static_cast<aura::compiler::TypeChecker*>(ev->commit_type_checker_handle())) {
        (void)tc->maybe_persist_occurrence_snapshot(mutation_id);
    }
}

namespace aura::compiler {

// ── Issue #2137: render hotpath + frame budget ───────────────────────────
void Evaluator::enter_render_hotpath() const noexcept {
    aura::core::arena_policy::enter_render_hotpath();
    frame_budget::enter();
    if (compiler_metrics_) {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        m->render_hotpath_enter_total.fetch_add(1, std::memory_order_relaxed);
    }
}

void Evaluator::exit_render_hotpath() const noexcept {
    aura::core::arena_policy::exit_render_hotpath();
    frame_budget::exit();
    // Mirror hold + present histogram into CompilerMetrics for Agents.
    if (compiler_metrics_) {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        const auto snap = frame_budget::snapshot();
        m->frame_budget_deferred_cascade_total.store(snap.deferred_cascade_total,
                                                     std::memory_order_relaxed);
        m->frame_budget_flush_total.store(snap.flush_total, std::memory_order_relaxed);
        m->present_p99_under_cascade_us.store(snap.present_p99_us, std::memory_order_relaxed);
        m->render_hotpath_hold_ns.store(snap.hold_ns_total, std::memory_order_relaxed);
        m->frame_budget_wired.store(1, std::memory_order_relaxed);
    }
}

// flush_frame_budget_deferred is implemented in service_dirty (needs
// mark_define_dirty). Evaluator stub leaves deferred names for service drain.
void Evaluator::flush_frame_budget_deferred() const noexcept {
    // Names stay queued; CompilerService::flush_frame_budget_deferred_ drains
    // via mark_define_dirty. Metrics sync only.
    if (compiler_metrics_) {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        const auto snap = frame_budget::snapshot();
        m->frame_budget_deferred_cascade_total.store(snap.deferred_cascade_total,
                                                     std::memory_order_relaxed);
        m->frame_budget_pending.store(snap.deferred_pending, std::memory_order_relaxed);
    }
}

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
    // Issue #2121: region-mode Guards also force lightweight (no full
    // children_ snapshot under concurrent region writers).
    const bool force_lw = force_lightweight_checkpoint_for_next_boundary_;
    force_lightweight_checkpoint_for_next_boundary_ = false;
    const bool lightweight =
        (aura::core::arena_policy::in_render_hotpath() || force_lw) && workspace_flat_ != nullptr;
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
                          // Issue #2086: capture owning Evaluator* so a
                          // fiber-steal resume on a different host can
                          // clear_gc_defer_for_evaluator(prev).
                          static_cast<void*>(this), log_size, bump_suppressed_at_entry,
                          macro_introduced_count_at_entry, flat_generation_at_entry,
                          std::move(children_snapshot), fine_rollback, std::move(sym_id_snapshot),
                          std::move(param_snapshot), lightweight};
    active_mutation_stack().push_back(std::move(cp));
    const std::size_t depth = active_mutation_stack().size();
    // Issue #2105: mark txn-dirty for nested / atomic_batch so Agents
    // see half-typed views as not commit-consistent until composite_txn_commit.
    if (depth > 1 || bump_suppressed_at_entry)
        note_txn_dirty();
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
    // Issue #1638 / #2201: mutation_log compact at boundary exit (success
    // path only — failure path already rolls back via
    // rollback_to_size, so the log is already shrunk). Threshold
    // gate avoids the shrink_to_fit cost on small log states
    // (heavy-mutation safety net — 200MB+/day reclaim in long-
    // running Agent scenarios). Cheap when under threshold.
    // #2201: also stamp high-water + pressure for Agent closed-loop.
    if (success && workspace_flat_) {
        static constexpr std::size_t kCompactThreshold = 64 * 1024; // entries
        const auto log_sz = workspace_flat_->mutation_log_size();
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
            auto hw = m->mutation_log_high_water.load(std::memory_order_relaxed);
            while (log_sz > hw &&
                   !m->mutation_log_high_water.compare_exchange_weak(
                       hw, static_cast<std::uint64_t>(log_sz), std::memory_order_relaxed)) {
            }
            const auto soft = m->mutation_log_soft_threshold.load(std::memory_order_relaxed);
            const auto soft_n = soft == 0 ? 5'000ull : soft;
            const auto score =
                soft_n == 0
                    ? 0ull
                    : std::min<std::uint64_t>(
                          10'000ull, (static_cast<std::uint64_t>(log_sz) * 10'000ull) / soft_n);
            m->mutation_log_pressure_score_bp.store(score, std::memory_order_relaxed);
            m->mutation_log_pressure_flag.store(log_sz >= soft_n ? 1 : 0,
                                                std::memory_order_relaxed);
        }
        if (log_sz > kCompactThreshold)
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
    // Issue #2068 Phase 2 / #2104: selective predicate-memo invalidation is
    // also wired in TypeChecker::infer_flat_partial (dirty var_names + min_gen
    // before reanalyze_occurrence_contexts) for post-mutate typecheck /
    // typecheck-incremental. Issue #2144: outermost Guard success also
    // calls refresh_occurrence_on_guard_exit (below, with cascade) so
    // multi-round mutate:* does not wait for query:type / full infer.
    // Empty dirty → zero cost (AC4 early exit).
    // Issue #555 / #518: selective_recheck_count_ is
    // bumped from reanalyze_occurrence_contexts paths.
    // Issue #2102: always consume provenance-miss flag on boundary exit
    // (even without workspace) so TLS does not stick across tests/fibers.
    // Count force-audit metric here; Full-path invariant suite still needs
    // workspace_flat_ (below).
    // Issue #2558: re-evaluate completeness SLO under production so long
    // Sampled sessions that accumulated miss pressure force Full on this
    // outermost exit (one-shot pending flag). Soft/non-production only
    // observes. Vacuous 10000 bp → no breach.
    if (typed_audit::production_defaults_active()) {
        aura::compiler::evaluate_coercion_provenance_slo(
            aura::compiler::coercion_provenance_completeness_bp(), /*production_active=*/true);
    }
    // Issue #2648: Soft evidence-loss pressure is a pure read here (arming
    // happens on soft incomplete skip via evaluate_coercion_evidence_loss_slo
    // in arm_soft_incomplete_force_full_observe). Do NOT re-arm on every
    // boundary exit — AC2: second exit without new misses must not re-arm.
    const auto evidence_loss_bp = aura::compiler::coercion_evidence_loss_bp();
    const bool evidence_loss_pressure =
        aura::compiler::coercion_evidence_loss_pressure(evidence_loss_bp);
    const bool slo_force = aura::compiler::consume_coercion_prov_slo_force_full();
    bool provenance_miss = aura::compiler::consume_provenance_miss_for_boundary() || slo_force;
    // Capture whether this exit Full-samples under #2648 evidence-loss pressure
    // (before Soft recover may clear). One-shot: pending consumed above.
    const bool evidence_loss_force_candidate = slo_force && evidence_loss_pressure;
    // Issue #2561: Soft/Sampled cheap blame recovery before force-audit.
    // When miss signal present under Sampled: re-walk fill for mid's dirty
    // cone; on success clear force; on fail arm one-shot Full sample only if
    // AURA_BLAME_SOFT_ESCALATE=1 or production_defaults or #2648 loss pressure.
    // Soft default without evidence-loss stays observe-only (#2561 AC3).
    // Full strategy leaves #2221 hard-reject alone.
    if (provenance_miss && success && workspace_flat_ && !nested_boundary &&
        typed_audit::get_strategy() == typed_audit::AuditStrategy::Sampled) {
        // Prefer last mutation log mid (dirty cone); fall back to counter.
        std::uint64_t soft_mid = total_mutations_.load(std::memory_order_relaxed);
        const auto& mlog = workspace_flat_->all_mutations();
        if (!mlog.empty())
            soft_mid = mlog.back().mutation_id;
        if (aura::compiler::maybe_soft_recover_or_escalate_blame(*workspace_flat_, soft_mid,
                                                                 /*had_miss_signal=*/true)) {
            // Recovered dual fields — do not force audit solely for miss (#2648 AC4).
            provenance_miss = false;
        } else if (!aura::compiler::blame_soft_escalate_pending_for_boundary() &&
                   !aura::compiler::blame_soft_escalate_enabled() &&
                   !typed_audit::production_defaults_active() && !evidence_loss_pressure) {
            // Soft observe-only: recover failed, escalate disabled, and
            // evidence-loss healthy → drop force (AC3 clean Soft). Fail
            // counter already bumped inside maybe_soft_recover_or_escalate_blame.
            // Issue #2648: when loss_bp >= threshold keep force for one Full sample.
            provenance_miss = false;
        }
    }
    const bool soft_escalate = aura::compiler::consume_blame_soft_escalate_for_boundary();
    if (soft_escalate)
        provenance_miss = true; // one-shot Full/contextual sample (not hard reject)
    if (provenance_miss) {
        aura::compiler::g_coercion_provenance_miss_force_audit_total.fetch_add(
            1, std::memory_order_relaxed);
        typed_audit::g_typed_mutation_audit_counters.contextual_force_audit_total.fetch_add(
            1, std::memory_order_relaxed);
        // Issue #2648: boundary consumed force-Full under evidence-loss pressure.
        if (evidence_loss_force_candidate || (evidence_loss_pressure && slo_force)) {
            aura::compiler::g_coercion_evidence_loss_force_consumed_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
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
        // Issue #2038: push-automatic DefUse/IR/JIT cascade so the next
        // eval-current / query sees updated caches without a manual
        // invalidate. Scoped to mutation-log targets + staged defuse
        // names (not a global flush). No-op when log empty.
        push_post_mutate_incremental_cascade(cp.mutation_log_size);
        // Issue #2144: selective predicate-memo invalidate + occurrence
        // reanalyze on outermost success exit only (long-lived engine).
        // Nested guards defer to outer; after cascade so dirty bits stamp.
        if (!nested_boundary)
            refresh_occurrence_on_guard_exit(cp.mutation_log_size, nodes_changed);
        // Issue #1589 / #1614 / #1894 / #2027 / #2029: TypedMutationAudit
        // trail + real invariant suite on mutation boundary hot path.
        // Contextual sampling (#1894) forces audit for large dirty / linear.
        // #2027: nested/atomic_batch never under-samples.
        // #2029: Full strategy always tries per-category partial recovery
        // (type recheck / linear re-enforce / provenance restamp) before
        // structural rollback — composite and single-boundary alike.
        // #2102: provenance_miss (consumed above) forces Full-path audit.
        // Issue #2215 RenderFastExit: skip Full suite under render hotpath
        // success (frame budget); still record lightweight boundary outcome.
        {
            const std::uint64_t mid = total_mutations_.load(std::memory_order_relaxed);
            const auto fid = static_cast<std::int64_t>(aura_fiber_current_id());
            if (render_fast_exit_this_boundary_ && !nested_boundary) {
                if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                    m->render_fast_exit_skipped_audit_total.fetch_add(1, std::memory_order_relaxed);
                clear_txn_dirty();
                typed_audit::record_boundary_outcome(
                    mid, audit_op, cp.version, epoch_after, /*success=*/true,
                    static_cast<std::uint32_t>(audit_target),
                    static_cast<std::uint32_t>(nodes_changed), fid);
                // Skip Full/composite invariant walk + partial recovery.
            } else {
                const bool linear_hint = (audit_op.find("linear") != std::string_view::npos) ||
                                         (audit_op.find("move") != std::string_view::npos) ||
                                         (audit_op.find("inline") != std::string_view::npos);
                // Issue #2223: match sites force Sampled hard-gate / audit
                // (mirror linear_ops_present — ADT self-mod must not under-sample).
                bool match_sites = false;
                if (workspace_flat_) {
                    const auto n = workspace_flat_->size();
                    for (aura::ast::NodeId id = 0; id < n; ++id) {
                        if (workspace_flat_->has_match_info(id)) {
                            match_sites = true;
                            break;
                        }
                    }
                }
                const bool batch_active =
                    cp.bump_suppressed_at_entry ||
                    (workspace_flat_ && workspace_flat_->atomic_batch_active());
                const bool composite = nested_boundary || batch_active;
                const auto strat = typed_audit::get_strategy();
                // Issue #2145: Strict sandbox links Full-class hard gate.
                const bool strict_sandbox = aura::core::sandbox::is_strict();
                const bool hard_gate = typed_audit::requires_invariant_hard_gate(
                    nodes_changed, linear_hint, strict_sandbox, match_sites);
                // Issue #2514 / #2545 / Issue #2559: unified linear force entry
                // is single rollback authority for synth + sticky post-mutate /
                // escape (three-layer type-half inventory site).
                // Decision table (see force_linear_rollback / typecheck):
                //   SynthHardFail (prod/strict) → force + skip soft recovery
                //   Soft Warning only           → no force; continue audit below
                //   PostMutate / CrossBatch     → sticky may force if set
                //   None                        → post-mutate ownership defense
                // Must run before Sampled skip / partial recovery so soft recovery
                // cannot claim Success after a synth TypeError.
                // force_linear_rollback classifies linear_synth_hard_fail_pending
                // first (deny_if_linear_synth_hard_fail is the thin alias).
                if (force_linear_rollback(composite ? "composite-linear-synth-hard-fail"
                                                    : "linear-synth-hard-fail")) {
                    // Structural undo (mirror hard-gate force-rollback body).
                    BoundaryRollbackStats stats;
                    stats.field_records_rolled =
                        workspace_flat_->rollback_to_size(cp.mutation_log_size);
                    if (stats.field_records_rolled > 0) {
                        bump_mutation_log_rollback_count();
                        if (nested_boundary)
                            bump_edsl_nested_atomic_rollback();
                    }
                    workspace_flat_->restore_children(std::move(cp.children_snapshot));
                    stats.children_column_restored = true;
                    if (cp.fine_rollback) {
                        workspace_flat_->restore_sym_id(std::move(cp.sym_id_snapshot));
                        workspace_flat_->restore_param_columns(std::move(cp.param_snapshot));
                        stats.sym_id_column_restored = true;
                        stats.param_columns_restored = true;
                    }
                    if (workspace_flat_->atomic_batch_active() != cp.bump_suppressed_at_entry) {
                        if (cp.bump_suppressed_at_entry)
                            workspace_flat_->begin_atomic_batch();
                        else
                            workspace_flat_->rollback_atomic_batch();
                        suppressed_misalign_caught_.fetch_add(1, std::memory_order_relaxed);
                    }
                    last_boundary_rollback_stats_ = stats;
                    defuse_index_ = nullptr;
                    if (!nested_boundary)
                        clear_txn_dirty();
                    typed_audit::record_boundary_outcome(
                        mid,
                        composite ? "composite-linear-synth-hard-fail" : "linear-synth-hard-fail",
                        cp.version, epoch_after, /*success=*/false,
                        static_cast<std::uint32_t>(audit_target), 0, fid);
                    if (strict_sandbox)
                        typed_audit::capture_audit_event_forced(
                            mid, "strict-linear-synth-denied",
                            typed_audit::MutationKind::Structural, cp.version, epoch_after,
                            typed_audit::AuditOutcome::Error,
                            static_cast<std::uint32_t>(audit_target), 0, fid, 0);
                    // Issue #2717: stamp TypeLinearCommitProof on boundary
                    // reject path. Agents can hold the proof across
                    // densify / steal / remap and re-check
                    // defuse_or_epoch_stamp without re-joining N
                    // query surfaces. Cheap (no extra heavy walks).
                    (void)typed_audit::build_type_linear_commit_proof_from_live(cp.version);
                    return cp;
                }
                // Composite paths never under-sample (self-evo multi-step safety).
                // Provenance miss forces audit even when Sampled would skip and
                // even when nodes_changed==0 (apply may be the only side effect).
                // Issue #2108: linear_ops_present (linear_hint) and composite
                // always force the escape hard-block path — Sampled must not
                // skip analyze_linear_escape / Moved live-root checks.
                // Issue #2145: Full/Strict hard_gate always audits (even small dirty).
                // Issue #2223: match_sites force Sampled audit.
                const bool do_audit =
                    strat != typed_audit::AuditStrategy::Off &&
                    (hard_gate || provenance_miss || composite || linear_hint || match_sites ||
                     (nodes_changed > 0 && typed_audit::should_audit_contextual(
                                               mid, nodes_changed, linear_hint, match_sites)));
                if (!do_audit && strat == typed_audit::AuditStrategy::Sampled)
                    typed_audit::g_typed_mutation_audit_counters.hard_gate_sampled_skip_total
                        .fetch_add(1, std::memory_order_relaxed);
                if (do_audit) {
                    typed_audit::InvariantAuditResult first{};
                    bool inv_ok = false;
                    bool recovered = false;
                    // Issue #2105: composite/nested/atomic_batch uses ordered
                    // commit barrier (solve_delta_occurrence → linear revalidate
                    // → invariant audit → Full partial recovery or reject).
                    // Issue #2260: non-composite hard-gate must prove SOLVED /
                    // !truncated_reverify (or full-resync) before native continues.
                    if (composite) {
                        typed_audit::CompositeTxnCommitResult ccr{};
                        inv_ok = composite_txn_commit(
                            mid, audit_op, static_cast<std::uint32_t>(audit_target), cp.version,
                            epoch_after, nested_boundary, batch_active, &ccr);
                        first = ccr.audit;
                        recovered = ccr.partial_recovered;
                        if (inv_ok)
                            clear_txn_dirty();
                    } else {
                        bool proof_trunc = false;
                        bool proof_force = false;
                        const bool proof_ok = boundary_solve_proof_gate(
                            hard_gate, linear_hint, nodes_changed, &proof_trunc, &proof_force);
                        if (hard_gate && (!proof_ok || proof_force)) {
                            first.type_ok = false;
                            inv_ok = false;
                            // Fall through to force-rollback path below.
                        } else {
                            inv_ok = run_typed_mutation_invariant_audit(
                                mid, audit_op, static_cast<std::uint32_t>(audit_target), cp.version,
                                epoch_after,
                                /*composite_mode=*/false, &first);
                        }
                        (void)proof_trunc;
                    }
                    // #1894 / #2029 / #2145: non-composite Full/Strict hard-gate →
                    // per-category partial recover before structural rollback.
                    if (!composite && !inv_ok && hard_gate) {
                        auto& ac = typed_audit::g_typed_mutation_audit_counters;
                        ac.partial_recovery_attempt_total.fetch_add(1, std::memory_order_relaxed);
                        ac.hard_gate_audits_total.fetch_add(1, std::memory_order_relaxed);
                        // Prefer linear re-enforce first (cheap; often fixes Moved live roots).
                        if (!first.linear_ok || first.cross_batch_linear_escape) {
                            ac.partial_recovery_linear_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                            (void)linear_post_mutate_enforce_all();
                            (void)enforce_linear_boundary_consistency(kLinearGcRootAuditTypedMutate,
                                                                      /*mark_all_linear=*/true);
                            // Issue #2695: unified rebind entry — routes
                            // densify-driven rebind through the same API
                            // as steal / Agent (single entry for Agents).
                            // AC3 zero-cost short-circuit when no roots
                            // were remapped; real per-root span wires in
                            // follow-up. Production force-rollback on
                            // mismatch (returns false) per #2563 contract.
                            // Issue #2708: real per-root walk wires through
                            // this same call site. Empty span here preserves
                            // AC3 — Phase-5 densify exit doesn't carry a
                            // direct NodeId span in scope at this point;
                            // the walk runs when callers pass a non-empty
                            // span (test / future wiring when densify
                            // exposes a remapped root set accessor).
                            (void)aura::compiler::ownership_rebind_after_remap(
                                {}, aura::compiler::RemapReason::Densify);
                        }
                        // Type-only recheck: re-driven by the full suite below
                        // (visitor rewalks NotChecked / dirty mutations).
                        if (!first.type_ok) {
                            ac.partial_recovery_type_total.fetch_add(1, std::memory_order_relaxed);
                        }
                        // Provenance fail → restamp pins / generations + re-validate chain.
                        if (!first.provenance_ok) {
                            ac.partial_recovery_provenance_total.fetch_add(
                                1, std::memory_order_relaxed);
                            if (workspace_flat_)
                                workspace_flat_->restamp_all_node_generations();
                            (void)restamp_pinned_stable_refs();
                            (void)post_mutation_reflect_validate();
                        }
                        // Issue #2223: ADT renarrow / revalidate before re-audit.
                        if (!first.adt_ok) {
                            ac.partial_recovery_adt_total.fetch_add(1, std::memory_order_relaxed);
                            partial_recover_adt_exhaustiveness(mid);
                        }
                        typed_audit::InvariantAuditResult after{};
                        inv_ok = run_typed_mutation_invariant_audit(
                            mid, "full-partial-recover", static_cast<std::uint32_t>(audit_target),
                            cp.version, epoch_after,
                            /*composite_mode=*/false, &after);
                        if (inv_ok) {
                            recovered = true;
                            ac.partial_recovery_success_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                        } else {
                            ac.partial_recovery_fail_total.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    // Issue #2145 / #2545: hard-gate force rollback for Full /
                    // Strict / composite. Linear axes go through the unified
                    // force_linear_rollback entry first (single counter
                    // ownership; no double-count with linear_invariant_fail).
                    if (!inv_ok && !recovered && (composite || hard_gate)) {
                        auto& ac = typed_audit::g_typed_mutation_audit_counters;
                        const bool linear_forced = force_linear_rollback(
                            composite ? "composite-linear-force" : "linear-force-rollback", &first);
                        if (!linear_forced &&
                            (strat == typed_audit::AuditStrategy::Full || hard_gate)) {
                            // Type / provenance / adt (non-linear) force path.
                            ac.full_strategy_force_rollback_total.fetch_add(
                                1, std::memory_order_relaxed);
                            ac.hard_gate_force_rollback_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                                m->typed_mutation_full_force_rollback_total.fetch_add(
                                    1, std::memory_order_relaxed);
                            aura_escape_move_gate_clear();
                            g_linear_escape_gate_clear_on_rollback_total.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                        if (composite) {
                            ac.composite_full_rollback_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                        }
                        // Issue #2309: linear force path already cleared the
                        // escape gate inside force_linear_rollback; non-linear
                        // path cleared above.
                        // Issue #2284: publish boundary hard-reject signal on the
                        // repair surface. The typecheck path already published the
                        // detailed unresolved_affected_nodes data; we only update
                        // the status + publish_total here to avoid clobbering the
                        // repair set with an empty boundary-only snapshot.
                        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
                            constexpr std::uint64_t kHardRejectStatus = 99;
                            m->type_repair_last_timeout_status.store(kHardRejectStatus,
                                                                     std::memory_order_relaxed);
                            m->type_repair_publish_total.fetch_add(1, std::memory_order_relaxed);
                        }
                        // Agent-visible deny reason (#2145 / #2076 shape).
                        std::string_view deny_kind = "invariant";
                        if (first.cross_batch_linear_escape)
                            deny_kind = "linear-escape";
                        else if (!first.linear_ok)
                            deny_kind = "linear";
                        else if (!first.type_ok)
                            deny_kind = "type";
                        else if (!first.provenance_ok)
                            deny_kind = "provenance";
                        last_mutate_error_ = typed_audit::format_invariant_deny_reason(
                            deny_kind, capability_tenant_id(),
                            composite ? "composite-invariant-force-rollback"
                                      : "invariant-force-rollback");
                        if (strict_sandbox) {
                            strict_mutate_hold_.store(1, std::memory_order_relaxed);
                            ac.hard_gate_strict_hold_total.fetch_add(1, std::memory_order_relaxed);
                        }
                        // Structural undo (same as failure path; preserves fine_rollback).
                        BoundaryRollbackStats stats;
                        stats.field_records_rolled =
                            workspace_flat_->rollback_to_size(cp.mutation_log_size);
                        if (stats.field_records_rolled > 0) {
                            bump_mutation_log_rollback_count();
                            if (nested_boundary)
                                bump_edsl_nested_atomic_rollback();
                        }
                        workspace_flat_->restore_children(std::move(cp.children_snapshot));
                        stats.children_column_restored = true;
                        if (cp.fine_rollback) {
                            workspace_flat_->restore_sym_id(std::move(cp.sym_id_snapshot));
                            workspace_flat_->restore_param_columns(std::move(cp.param_snapshot));
                            stats.sym_id_column_restored = true;
                            stats.param_columns_restored = true;
                        }
                        // Realign atomic-batch flag if nested path left it inconsistent.
                        if (workspace_flat_->atomic_batch_active() != cp.bump_suppressed_at_entry) {
                            if (cp.bump_suppressed_at_entry)
                                workspace_flat_->begin_atomic_batch();
                            else
                                workspace_flat_->rollback_atomic_batch();
                            suppressed_misalign_caught_.fetch_add(1, std::memory_order_relaxed);
                        }
                        last_boundary_rollback_stats_ = stats;
                        defuse_index_ = nullptr;
                        // Issue #2105: leave txn_dirty set until outermost clean exit,
                        // or clear when no remaining nested guards.
                        if (!nested_boundary)
                            clear_txn_dirty();
                        typed_audit::record_boundary_outcome(
                            mid,
                            composite ? "composite-invariant-force-rollback"
                                      : "invariant-force-rollback",
                            cp.version, epoch_after, /*success=*/false,
                            static_cast<std::uint32_t>(audit_target), 0, fid);
                        // Strict trail: Error outcome for Agent dashboards.
                        if (strict_sandbox)
                            typed_audit::capture_audit_event_forced(
                                mid, "strict-invariant-denied",
                                typed_audit::MutationKind::Structural, cp.version, epoch_after,
                                typed_audit::AuditOutcome::Error,
                                static_cast<std::uint32_t>(audit_target), 0, fid, 0);
                        return cp;
                    }
                    // Success path: record outcome when audit passed / recovered.
                    if (inv_ok || recovered) {
                        if (!nested_boundary)
                            clear_txn_dirty();
                        typed_audit::record_boundary_outcome(
                            mid, audit_op, cp.version, epoch_after, /*success=*/true,
                            static_cast<std::uint32_t>(audit_target),
                            static_cast<std::uint32_t>(nodes_changed), fid);
                    }
                } else {
                    if (!nested_boundary)
                        clear_txn_dirty();
                    typed_audit::record_boundary_outcome(
                        mid, audit_op, cp.version, epoch_after, /*success=*/true,
                        static_cast<std::uint32_t>(audit_target),
                        static_cast<std::uint32_t>(nodes_changed), fid);
                }
            } // !render_fast_exit_this_boundary_
        }
        // Issue #488: post-mutate reflect validation + snapshot fields.
        // (Also covered as provenance leg of #1614 invariant audit when sampled.)
        // Issue #2215: skip post_mutation_reflect_validate under RenderFastExit
        // (provenance restamp still runs in dtor pin phase).
        if (!render_fast_exit_this_boundary_)
            (void)post_mutation_reflect_validate();
    } else if (!success) {
        // Issue #1589: TypedMutationAudit rollback trail.
        const std::uint64_t epoch_after = defuse_version_.load(std::memory_order_acquire);
        const std::uint64_t mid = total_mutations_.load(std::memory_order_relaxed);
        const auto fid = static_cast<std::int64_t>(aura_fiber_current_id());
        typed_audit::record_boundary_outcome(mid, "rollback", cp.version, epoch_after,
                                             /*success=*/false, 0, 0, fid);
    }
    // Issue #2690: unified PendingRecovery drain. Boundary exit routes
    // through the same single-owner drain as `maybe_storm_clear_health_pass`
    // (StormClear). Exchange-not-check semantics: a concurrent storm-clear
    // drain in the same ms observes `kinds == 0` (cheap) and bumps
    // `double_drain_prevented` to surface the race. Closes the residual
    // unhealed window from novel interleavings (#2690).
    if (!nested_boundary && success) {
        aura_hot_update_drain_pending_recovery(
            static_cast<std::uint8_t>(HotUpdateRegistry::DrainReason::BoundaryExit));
    }
    // Issue #2604: outermost MutationBoundary exit auto-drain deferred
    // reemit + one region-filtered pass. Closes the "visible but
    // unhealed" stale window without making reemit unbounded.
    // Soft path (no deferred + mask=0) → zero extra work (AC4).
    // Issue #2690: kept for backward compat — the unified drain above
    // also drives the deferred branch atomically (exchange-not-check);
    // this block preserves the legacy aura_reemit_aot_for_dirty counter
    // surface for existing test expectations.
    if (!nested_boundary && success) {
        auto& reg = hot_update_registry();
        if (reg.has_deferred_reemit() || reg.last_region_mask_from_dirty() != 0) {
            aura_bump_reemit_auto_drain_on_boundary_exit_total();
            // AC3: storm throttle → skip body, bump throttled. Leave
            // deferred pending per existing policy (no silent drop forever).
            if (reg.should_throttle_reemit()) {
                aura_bump_reemit_auto_drain_throttled_total();
            } else {
                auto v = reg.take_deferred_reemit_version();
                if (v == 0)
                    v = aura_get_aot_defuse_version();
                // Issue #2606: current-eval ownership for multi-AotState reemit.
                struct ReemitEvalOwnerGuard {
                    void* prev_reemit;
                    void* prev_reg;
                    explicit ReemitEvalOwnerGuard(void* e) noexcept
                        : prev_reemit(aura_aot_get_reemit_owner_eval())
                        , prev_reg(aura_aot_get_register_owner_eval()) {
                        aura_aot_set_reemit_owner_eval(e);
                        aura_aot_set_register_owner_eval(e);
                    }
                    ~ReemitEvalOwnerGuard() noexcept {
                        aura_aot_set_reemit_owner_eval(prev_reemit);
                        aura_aot_set_register_owner_eval(prev_reg);
                    }
                    ReemitEvalOwnerGuard(const ReemitEvalOwnerGuard&) = delete;
                    ReemitEvalOwnerGuard& operator=(const ReemitEvalOwnerGuard&) = delete;
                } owner_guard(static_cast<void*>(this));
                const auto n = aura_reemit_aot_for_dirty(v);
                if (n > 0)
                    aura_bump_reemit_auto_drain_success_total();
            }
        }
    }
    return cp;
}


// ═══════════════════════════════════════════════════════════════════════════
// Issue #2121 / #2523: Region / optimistic workspace write concurrency
//
// Problem: all structural mutates serialize on unique_lock(workspace_mtx_).
// Multi-Agent orchestration on disjoint top-level Defines was throughput-
// bound by that single writer lock.
//
// Strategy (Phase 1 observability + Phase 2 region RW + #2523 residual):
//
//   GlobalExclusive (default try_acquire / legacy ctor):
//     unique_lock(workspace_mtx_) for the full Guard body.
//     Required for: atomic-batch, topology-changing ops (cross-region
//     insert-child / restore_children), unknown region, policy OFF.
//
//   RegionExclusive (try_acquire_for_region when policy ON):
//     shared_lock(workspace_mtx_)  — concurrent with other region writers;
//                                    blocked by any GlobalExclusive unique.
//     unique_lock(workspace_region_mtx_[shard]) — exclusive within region.
//     Forces lightweight enter checkpoint (no full children_ snapshot).
//     Counts as workspace_mtx_optimistic_hit_total (#2523 soft path hit).
//
//   Soft path residual (#2523):
//     - Disjoint top-level Define Agents MUST use try_acquire_for_region
//       (or orch host soft region key) so two fibers do not both hold
//       global exclusive for the full body.
//     - Host orch agent body prefers try_acquire_for_region when region
//       concurrency is enabled (thread-keyed soft region); falls back to
//       GlobalExclusive when policy OFF / atomic-batch.
//     - Cross-region / atomic-batch / topology still take GlobalExclusive
//       (no silent data race).
//     - Agents self-throttle via query:workspace-mtx-contention-stats
//       (hold p99, waiters, region-collision rate, optimistic hits).
//
//   Optimistic note: defuse_version_ is already atomic; region enter
//   snapshots it for dirty detection. Full optimistic retry-on-conflict
//   for arbitrary topology remains future work — region path is the
//   primary scale-out for disjoint Define mutates (AC2 / AC6).
//
// Invariants preserved: PCV SafePCVSpan lifetime, StableNodeRef
// gen/wrap/cow restamp on exit, rollback_to_size + topology fidelity,
// TypedMutationAudit + linear enforce still run in outermost dtor
// under the held locks.
// ═══════════════════════════════════════════════════════════════════════════

// ── try_acquire (#1547 / #1556 / #1590) ──────────────────────────────────
aura::core::AuraResult<std::unique_ptr<Evaluator::MutationBoundaryGuard>>
Evaluator::MutationBoundaryGuard::try_acquire(Evaluator& ev, std::uint64_t pending_count,
                                              bool* success_flag, bool fine_rollback) noexcept {
    // Issue #2587: agent_throttle_for_mailbox_starvation gate (single
    // relaxed atomic load — zero cost when flag == 0, AC5). Soft path
    // bumps metric only and falls through to quota check; production /
    // Strict hard-rejects with structured AdmissionRejected:
    // mailbox-hold-starvation error (#2587 AC1 + AC2 + #2551 closed-loop
    // BP control plane). The TransactionGuard host callback wraps this
    // function (#2555), so TransactionGuard ctor is gated transitively.
    if (aura::serve::mf_mailbox::aura_orch_mailbox_starvation_throttled()) {
        aura::serve::mf_mailbox::note_mutate_rejected_mailbox_starvation();
        if (typed_audit::production_defaults_active()) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_)) {
                m->mutation_guard_try_acquire_reject_total.fetch_add(1, std::memory_order_relaxed);
            }
            return std::unexpected(
                aura::core::AuraError(aura::core::AuraErrorKind::ResourceQuotaExceeded,
                                      std::string("AdmissionRejected: mailbox-hold-starvation")));
        }
        // Soft path (Off / Soft sandbox): fall through (metric-only).
    }
    // Issue #2701: Mutation hold-budget timeout → force degrade / reject
    // new mutate admit. Order: #2587 mailbox-hold-starvation → #2701
    // budget → #2630/#2660 security-schedule. Budget is a fast atomic
    // read + compare; security-schedule is the last line of defense
    // (multiple live signals). Putting budget BEFORE schedule means
    // over-budget requests never reach the schedule evaluation.
    {
        const auto check = mutation_hold_budget_check();
        if (check.over_budget) {
            if (mutation_hold_budget_reject_enabled()) {
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_)) {
                    m->mutation_guard_try_acquire_reject_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                }
                return std::unexpected(
                    aura::core::AuraError(aura::core::AuraErrorKind::ResourceQuotaExceeded,
                                          std::string("AdmissionRejected: mutation-hold-budget")));
            }
            // Soft path (Off / Soft sandbox): fall through
            // (metric-only, already bumped by mutation_hold_budget_check()).
        }
    }
    // Issue #2630: security-schedule-gate (additive over #2587). Closes
    // the half-green / deny-storm window where Agents keep mutating
    // after security posture degraded. Soft / sandbox=off stays
    // observe-only (never denies — AC3 of #2590 preserved).
    // Issue #2660: live signals (commit_readiness, capability deny
    // storm window, mid-fallback SLO, posture wal_off under Restricted)
    // are now wired — previously the inputs were hardcoded to false
    // so the gate never actually denied in production. See
    // src/orch/security_schedule_gate.h::make_security_schedule_input_live.
    {
        const auto prod = typed_audit::production_defaults_active();
        const auto in = aura::orch::make_security_schedule_input_live(ev.effect_sandbox_mode(),
                                                                      prod, /*soft_mode=*/!prod);
        if (auto reason = aura::orch::admit_security_schedule(in); reason.has_value()) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_)) {
                m->mutation_guard_try_acquire_reject_total.fetch_add(1, std::memory_order_relaxed);
            }
            return std::unexpected(
                aura::core::AuraError(aura::core::AuraErrorKind::ResourceQuotaExceeded, *reason));
        }
        // Soft path: fall through (metric-only — counters always bump).
    }
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
    // Issue #2686: nested mutate under (eval-current) shared pin — fail closed
    // before Guard ctor so Agents get a structured error (not partial apply).
    if (Evaluator::eval_current_holds_shared_pin() &&
        !(ev.mutation_boundary_held() || ev.mutation_boundary_depth() > 0)) {
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_))
            m->mutation_guard_try_acquire_reject_total.fetch_add(1, std::memory_order_relaxed);
        return std::unexpected(aura::core::AuraError(
            aura::core::AuraErrorKind::ResourceQuotaExceeded,
            std::string("AdmissionRejected: nested-mutate-under-eval-current")));
    }
    // Construct via private AcquireTag path (quota already checked).
    // GlobalExclusive — no region_key.
    return std::unique_ptr<MutationBoundaryGuard>(
        new MutationBoundaryGuard(ev, success_flag, fine_rollback, AcquireTag{},
                                  /*quota_prechecked=*/true, /*region_key=*/std::nullopt));
}

// ── try_acquire_for_region (#2121) ───────────────────────────────────────
aura::core::AuraResult<std::unique_ptr<Evaluator::MutationBoundaryGuard>>
Evaluator::MutationBoundaryGuard::try_acquire_for_region(Evaluator& ev, std::uint64_t region_key,
                                                         std::uint64_t pending_count,
                                                         bool* success_flag,
                                                         bool fine_rollback) noexcept {
    // Issue #2686: same nested-under-eval-current gate as try_acquire.
    if (Evaluator::eval_current_holds_shared_pin() &&
        !(ev.mutation_boundary_held() || ev.mutation_boundary_depth() > 0)) {
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_))
            m->mutation_guard_try_acquire_reject_total.fetch_add(1, std::memory_order_relaxed);
        return std::unexpected(aura::core::AuraError(
            aura::core::AuraErrorKind::ResourceQuotaExceeded,
            std::string("AdmissionRejected: nested-mutate-under-eval-current")));
    }
    // Issue #2587: same throttle gate as try_acquire above (single
    // relaxed load, soft / hard split on production_defaults_active).
    if (aura::serve::mf_mailbox::aura_orch_mailbox_starvation_throttled()) {
        aura::serve::mf_mailbox::note_mutate_rejected_mailbox_starvation();
        if (typed_audit::production_defaults_active()) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_)) {
                m->mutation_guard_try_acquire_reject_total.fetch_add(1, std::memory_order_relaxed);
            }
            return std::unexpected(
                aura::core::AuraError(aura::core::AuraErrorKind::ResourceQuotaExceeded,
                                      std::string("AdmissionRejected: mailbox-hold-starvation")));
        }
        // Soft path: fall through.
    }
    // Issue #2701: Mutation hold-budget timeout → force degrade / reject
    // new mutate admit. Same order as try_acquire: #2587 mailbox-hold
    // -starvation → #2701 budget → #2630/#2660 security-schedule.
    {
        const auto check = mutation_hold_budget_check();
        if (check.over_budget) {
            if (mutation_hold_budget_reject_enabled()) {
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_)) {
                    m->mutation_guard_try_acquire_reject_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                }
                return std::unexpected(
                    aura::core::AuraError(aura::core::AuraErrorKind::ResourceQuotaExceeded,
                                          std::string("AdmissionRejected: mutation-hold-budget")));
            }
            // Soft path: fall through (metric-only, already bumped).
        }
    }
    // Issue #2630: security-schedule-gate (additive over #2587). Same
    // pattern as try_acquire: production hard-rejects with structured
    // AdmissionRejected: security-schedule:<force_reason>; soft path
    // falls through (metric-only).
    // Issue #2660: live signals wired via make_security_schedule_input_live
    // (commit_readiness + capability deny storm + mid-fallback SLO +
    // posture wal_off under Restricted).
    {
        const auto prod = typed_audit::production_defaults_active();
        const auto in = aura::orch::make_security_schedule_input_live(ev.effect_sandbox_mode(),
                                                                      prod, /*soft_mode=*/!prod);
        if (auto reason = aura::orch::admit_security_schedule(in); reason.has_value()) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_)) {
                m->mutation_guard_try_acquire_reject_total.fetch_add(1, std::memory_order_relaxed);
            }
            return std::unexpected(
                aura::core::AuraError(aura::core::AuraErrorKind::ResourceQuotaExceeded, *reason));
        }
    }
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
    if (ev.resource_quota_mutations_ != 0) {
        (void)aura::core::resource_quota::process_resource_quota().check_and_consume(
            aura::core::resource_quota::Dimension::Mutations, pending_count);
    }
    // Atomic-batch / topology-sensitive paths must not use region mode.
    // When atomic_batch is active on the flat, fall back to GlobalExclusive.
    std::optional<std::uint64_t> key = region_key;
    if (ev.workspace_flat_ && ev.workspace_flat_->atomic_batch_active()) {
        key = std::nullopt;
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_))
            m->workspace_region_fallback_global_total.fetch_add(1, std::memory_order_relaxed);
    } else if (!ev.workspace_region_concurrency_enabled()) {
        key = std::nullopt;
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_))
            m->workspace_region_fallback_global_total.fetch_add(1, std::memory_order_relaxed);
    }
    return std::unique_ptr<MutationBoundaryGuard>(
        new MutationBoundaryGuard(ev, success_flag, fine_rollback, AcquireTag{},
                                  /*quota_prechecked=*/true, key));
}

// ── legacy ctor (#1547 / #1556 / #1590) ──────────────────────────────────
Evaluator::MutationBoundaryGuard::MutationBoundaryGuard(Evaluator& ev, bool* success_flag,
                                                        bool fine_rollback) noexcept
    : MutationBoundaryGuard(ev, success_flag, fine_rollback, AcquireTag{},
                            /*quota_prechecked=*/false, /*region_key=*/std::nullopt) {}

// ── shared AcquireTag ctor ───────────────────────────────────────────────
Evaluator::MutationBoundaryGuard::MutationBoundaryGuard(
    Evaluator& ev, bool* success_flag, bool fine_rollback, AcquireTag, bool quota_prechecked,
    std::optional<std::uint64_t> region_key) noexcept
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
    //
    // Issue #2121: RegionExclusive uses shared_lock_ + region_lock_
    // instead of lock_ (global unique).
    lock_(ev.workspace_mtx_, std::defer_lock)
    , shared_lock_(ev.workspace_mtx_, std::defer_lock) {
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
    // Issue #2090: snapshot defuse_version_ at enter so the outermost
    // dtor can detect "dirty defines happened this boundary" without
    // having to scan process-wide state. Catches non-cascade paths
    // (fiber-steal restore / partial recovery / compact-only /
    // exception unwind) that skip mark_define_dirty.
    defuse_version_at_enter_ = ev_->defuse_version_.load(std::memory_order_acquire);
    // Issue #2162: dirty-upward is cumulative — snapshot for boundary delta.
    dirty_upward_at_enter_ =
        ev_->workspace_flat_ ? ev_->workspace_flat_->mark_dirty_upward_call_count() : 0;
    // Issue #236 / #1746: thread_local depth counter keyed by
    // Evaluator::instance_id_ (not address). Each fiber has its
    // own LIFO call stack, so nested guards on a single fiber
    // are always outermost-then-inner (destructed innermost-
    // first). Cross-fiber synchronization happens at unique_lock.
    int* slot = Evaluator::mutation_boundary_depth_slot(ev_);
    int prev = ++(*slot);
    bool outermost = (prev == 1);
    is_outermost_ = outermost;
    // Issue #2215: RenderFastExit eligible when outermost Guard is entered
    // under render hotpath (RenderHotEntryGuard / enter_render_hotpath).
    // Success path skips Full audit + full linear/dual-path probes and
    // defers synchronous reemit. Failure always takes the full restore path.
    render_fast_exit_ = outermost && aura::core::arena_policy::in_render_hotpath();
    // Issue #2121: decide RegionExclusive vs GlobalExclusive.
    if (outermost && region_key.has_value() && ev_->workspace_region_concurrency_enabled()) {
        region_mode_ = true;
        region_shard_ = Evaluator::workspace_region_shard(*region_key);
    }
    // Issue #2686: same-thread nested mutate under (eval-current) shared pin
    // would unique_lock under shared_lock → EDEADLK. Fail-closed so concurrent
    // fiber rebind remains safe while eval holds the pin for FlatAST walks.
    if (outermost && Evaluator::eval_current_holds_shared_pin()) {
        inert_ = true;
        if (flag_)
            *flag_ = false;
        if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics())) {
            m->mutation_guard_try_acquire_reject_total.fetch_add(1, std::memory_order_relaxed);
        }
        // Roll back depth bump so yield probes see no held boundary.
        --(*slot);
        is_outermost_ = false;
        return;
    }
    if (outermost) {
        // Issue #1253: start hold-time clock for long-mutation policy.
        enter_ts_ = std::chrono::steady_clock::now();
        // Issue #2517: process-wide live max outermost hold probe (fiber +
        // start_ns). Best-effort CAS; Agents read via query:mutation-hold-live.
        {
            const auto start_ns = aura::compiler::mutation_hold_steady_ns_of(*enter_ts_);
            // *slot already incremented for this outermost enter (prev==1).
            const auto depth = static_cast<std::uint32_t>(*slot);
            aura::compiler::mutation_hold_live_note_enter(aura_fiber_current_id(), start_ns, depth);
        }
        // Issue #1523: Workspace level in #1388 order (after Mutate).
        aura::compiler::lock_order::on_acquire(aura::compiler::lock_order::Level::Workspace);
        auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics());
        if (region_mode_) {
            // RegionExclusive: shared workspace + exclusive region shard.
            // Contended shared_lock wait counts toward workspace_mtx_ stats;
            // region collision when shard already has a holder.
            // Issue #2523: collision + optimistic hit + waiter samples.
            const auto holders_before =
                ev_->workspace_region_holders_[region_shard_].load(std::memory_order_relaxed);
            if (holders_before > 0 && m) {
                m->workspace_region_collision_total.fetch_add(1, std::memory_order_relaxed);
                m->workspace_mtx_region_collision_total.fetch_add(1, std::memory_order_relaxed);
            }
            if (shared_lock_.try_lock()) {
                if (m)
                    m->workspace_mtx_acquire_total.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (m) {
                    m->workspace_mtx_waiters_now.fetch_add(1, std::memory_order_relaxed);
                    auto peak = m->workspace_mtx_waiters_peak.load(std::memory_order_relaxed);
                    const auto now = m->workspace_mtx_waiters_now.load(std::memory_order_relaxed);
                    while (now > peak && !m->workspace_mtx_waiters_peak.compare_exchange_weak(
                                             peak, now, std::memory_order_relaxed)) {
                    }
                }
                const auto wait_t0 = std::chrono::steady_clock::now();
                shared_lock_.lock();
                const auto wait_ns =
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                   std::chrono::steady_clock::now() - wait_t0)
                                                   .count());
                if (m) {
                    m->workspace_mtx_waiters_now.fetch_sub(1, std::memory_order_relaxed);
                    m->workspace_mtx_acquire_total.fetch_add(1, std::memory_order_relaxed);
                    m->workspace_mtx_contended_total.fetch_add(1, std::memory_order_relaxed);
                    m->workspace_mtx_wait_ns_total.fetch_add(wait_ns, std::memory_order_relaxed);
                    auto prev_max = m->workspace_mtx_wait_ns_max.load(std::memory_order_relaxed);
                    while (wait_ns > prev_max &&
                           !m->workspace_mtx_wait_ns_max.compare_exchange_weak(
                               prev_max, wait_ns, std::memory_order_relaxed)) {
                    }
                    m->workspace_closedloop_shared_mutex_contention_total.fetch_add(
                        1, std::memory_order_relaxed);
                    m->workspace_closedloop_shared_mutex_contention_ns_total.fetch_add(
                        wait_ns, std::memory_order_relaxed);
                }
            }
            region_lock_ = std::unique_lock<std::mutex>(ev_->workspace_region_mtx_[region_shard_]);
            ev_->workspace_region_holders_[region_shard_].fetch_add(1, std::memory_order_relaxed);
            if (m) {
                m->workspace_region_acquire_total.fetch_add(1, std::memory_order_relaxed);
                m->workspace_region_hold_samples.fetch_add(1, std::memory_order_relaxed);
                // Issue #2523: soft path hit (region classification succeeded).
                m->workspace_mtx_optimistic_hit_total.fetch_add(1, std::memory_order_relaxed);
            }
            // Lightweight checkpoint under concurrent region writers.
            ev_->force_lightweight_checkpoint_for_next_boundary_ = true;
        } else {
            // GlobalExclusive: unique_lock(workspace_mtx_).
            // Issue #2040: try_lock first so uncontended path stays cheap.
            if (lock_.try_lock()) {
                if (m)
                    m->workspace_mtx_acquire_total.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (m) {
                    m->workspace_mtx_waiters_now.fetch_add(1, std::memory_order_relaxed);
                    auto peak = m->workspace_mtx_waiters_peak.load(std::memory_order_relaxed);
                    const auto now = m->workspace_mtx_waiters_now.load(std::memory_order_relaxed);
                    while (now > peak && !m->workspace_mtx_waiters_peak.compare_exchange_weak(
                                             peak, now, std::memory_order_relaxed)) {
                    }
                }
                const auto wait_t0 = std::chrono::steady_clock::now();
                lock_.lock();
                const auto wait_ns =
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                   std::chrono::steady_clock::now() - wait_t0)
                                                   .count());
                if (m) {
                    m->workspace_mtx_waiters_now.fetch_sub(1, std::memory_order_relaxed);
                    m->workspace_mtx_acquire_total.fetch_add(1, std::memory_order_relaxed);
                    m->workspace_mtx_contended_total.fetch_add(1, std::memory_order_relaxed);
                    m->workspace_mtx_wait_ns_total.fetch_add(wait_ns, std::memory_order_relaxed);
                    auto prev_max = m->workspace_mtx_wait_ns_max.load(std::memory_order_relaxed);
                    while (wait_ns > prev_max &&
                           !m->workspace_mtx_wait_ns_max.compare_exchange_weak(
                               prev_max, wait_ns, std::memory_order_relaxed)) {
                    }
                    m->workspace_closedloop_shared_mutex_contention_total.fetch_add(
                        1, std::memory_order_relaxed);
                    m->workspace_closedloop_shared_mutex_contention_ns_total.fetch_add(
                        wait_ns, std::memory_order_relaxed);
                }
            }
            if (m)
                m->workspace_global_exclusive_total.fetch_add(1, std::memory_order_relaxed);
        }
        ev_->outermost_mutation_success_flag_ = flag_;
        ev_->bind_yield_hook_evaluator();
        // Issue #354: set the atomic flag so
        // Fiber::yield can detect "yield while
        // holding a mutation boundary". The
        // check is O(1) (atomic load) and the
        // flag is cleared by the Guard dtor
        // (the outermost one only).
        ev_->mutation_boundary_held_.store(true, std::memory_order_release);
        // Issue #2204: arm GcDeferReason::MutationHold on outermost enter
        // (after lock + held_). Nested guards do NOT arm (depth already
        // covers concurrent outer holds via process-wide depth). Soft
        // #1493 hold-µs GC frequency tune remains; this is the hard
        // should_defer_destructive_gc gate for STW.
        aura::gc_hooks::arm_mutation_hold_defer();
        // Issue #2184: publish fiber-local MutationSafetySnapshot mirrors
        // (held=true) so steal path never samples torn depth/held.
        // Use fiber.cpp helper — TLS null-check + member call co-located
        // (ubsan-smoke x86_64: bare g_current_fiber load in this module
        // was reported as "load of null pointer of type 'struct Fiber *'").
        aura::serve::publish_current_fiber_mutation_safety(
            Evaluator::active_mutation_stack_static().size(), /*held=*/true,
            defuse_version_at_enter_);
        // Issue #1252: coverage counter — every outermost Guard wrap.
        // Issue #1364: mutation × safepoint telemetry (benign race).
        if (m) {
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
    // Issue #2222: arm fiber-local LinearEnforce Strict hold for the
    // Guard lifetime so Soft process mode still early-detects incomplete
    // linear×provenance under Agent mutate (does not flip process-wide
    // Soft; multi-fiber safe). Nested Guards nest the depth.
    (void)aura::core::provenance::mutation_boundary_push_linear_enforce_strict();
    linear_enforce_strict_pushed_ = true;
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
    if (!ev_ || inert_) {
        // Issue #2222: if somehow pushed without full enter, still pop.
        if (linear_enforce_strict_pushed_) {
            aura::core::provenance::mutation_boundary_pop_linear_enforce_strict();
            linear_enforce_strict_pushed_ = false;
        }
        return; // Issue #1590: quota soft-reject never entered a boundary
    }
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
    // Issue #2120: use ctor-captured is_outermost_ so depth_slot can
    // stay elevated until unlock (steal/GC must not see depth==0 mid
    // exit pipeline). Nested guards still use the same member flag.
    // exit_mutation_boundary runs under the lock for the outermost
    // guard; lockless for nested guards (outer holds the lock).
    const bool outermost = is_outermost_;
    int* slot = Evaluator::mutation_boundary_depth_slot(ev_);
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
            // Issue #1443 / #2199: threshold load (1 relaxed load; not a write).
            // Soft path: metrics + scheduler hook only when hold > threshold.
            // Strict path (long_mutation_strict_mode / AURA_MUTATION_HOLD_STRICT):
            // hold > hard_timeout (hard_timeout_us if set, else max_extreme)
            // → force *flag_=false BEFORE exit_mutation_boundary (rollback).
            // Nested guards never enter this block (outermost-only + enter_ts_).
            const auto max_us = static_cast<std::int64_t>(
                m->long_mutation_threshold_us.load(std::memory_order_relaxed));
            if (us > max_us) {
                b.too_long = 1;
                b.starvation_prevented = 1;
                b.contention_us = uus;
                b.long_fiber_id = aura_fiber_current_id();
                b.update_max = true;
                // AC4: scheduler hook still fires on too_long (steal priority).
                ::aura_invoke_long_mutation_scheduler_hook(b.long_fiber_id, uus);
                const auto extreme_us = static_cast<std::int64_t>(
                    m->max_extreme_mutation_us.load(std::memory_order_relaxed));
                // Issue #2199: hard_timeout_us overrides extreme when non-zero.
                const auto hard_cfg = m->hard_timeout_us.load(std::memory_order_relaxed);
                const auto hard_us = static_cast<std::int64_t>(
                    hard_cfg != 0 ? hard_cfg : static_cast<std::uint64_t>(extreme_us));
                // Issue #2199: AURA_MUTATION_HOLD_STRICT=1 aligns with
                // long_mutation_strict_mode (process-wide production knob).
                // mutation_hold_budget_us() is file-scope (#2313) — see above.
                static const bool env_hold_strict = []() noexcept {
                    const char* e = std::getenv("AURA_MUTATION_HOLD_STRICT");
                    return e != nullptr && e[0] != '\0' && e[0] != '0' && e[0] != 'f' &&
                           e[0] != 'F' && e[0] != 'n' && e[0] != 'N';
                }();
                const bool strict = env_hold_strict || m->long_mutation_strict_mode.load(
                                                           std::memory_order_relaxed) != 0;
                if (us > extreme_us)
                    b.extreme = 1;
                // Strict force-fail: outermost only (this block), after
                // threshold exceed, when hold exceeds hard timeout.
                // AC5: only flips success flag — does not unlock early
                // (exit_mutation_boundary / depth / unlock still ordered).
                if (strict && us > hard_us) {
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
            // Issue #2405: feed recent outermost hold sample ring for
            // query:mutation-hold-estimate p50/p99 (Agent batch planning).
            // One atomic seq + store; no lock. Query sorts a snapshot.
            {
                const auto seq =
                    m->mutation_hold_sample_seq.fetch_add(1, std::memory_order_relaxed);
                const auto slot =
                    static_cast<std::size_t>(seq % CompilerMetrics::kMutationHoldSampleRing);
                m->mutation_hold_sample_ring[slot].store(b.hold_us, std::memory_order_relaxed);
                m->mutation_hold_sample_count.fetch_add(1, std::memory_order_relaxed);
            }
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
                // Issue #2199 AC1: force-fail BEFORE exit_mutation_boundary.
                // Must also re-sync `success` (captured earlier from *flag_)
                // so rollback + linear post-failure enforce actually run.
                if (b.force_fail) {
                    if (flag_)
                        *flag_ = false;
                    success = false;
                    m->long_mutation_forced_abort_total.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // Issue #2313 AC1: hold-budget over-budget signal. Distinct
            // from mutation_too_long_total which is the LATE-warning
            // threshold via long_mutation_threshold_us (default 500ms).
            // mutation_hold_budget_us() is the EARLY-warning threshold
            // (default 100ms via AURA_MUTATION_HOLD_BUDGET_US). Bumped
            // when hold exceeds budget. SIGNAL-ONLY — does NOT force-
            // fail or yield (would violate #2200 / unlock workspace_mtx_
            // mid-mutate). Agents read mutation_hold_over_budget_total
            // + over-budget rate via query:mutation-boundary-hold-stats
            // and choose RenderFastExit-style degrade or shorter batches
            // (closed-loop AC2; #2253 -40 steal penalty still fires on
            // last_hold_us for #2253 integration). Per AC3: zero cost
            // under budget (one env-cached load + compare on the dtor
            // safe point — no extra atomics when uus <= budget).
            if (uus > mutation_hold_budget_us()) {
                m->mutation_hold_over_budget_total.fetch_add(1, std::memory_order_relaxed);
            }
            // Issue #2349: production hold SLO circuit-breaker (default fail path).
            // ── Decision table (Soft / Production / Disabled) ──
            // | Mode       | Env select                                      | hold > SLO |
            // | Soft       | AURA_SANDBOX=off OR AURA_MUTATION_HOLD_SLO_SOFT=1 | metric only |
            // | Production | default                                         | force-fail  |
            // | Disabled   | AURA_MUTATION_HOLD_SLO_US=0                     | no-op       |
            // Happy path (hold ≤ SLO or SLO=0): one compare, zero force work (AC3).
            // Reuses this outermost dtor hold sample — no second timer (goal 3).
            // Distinct from #2199 STRICT hard-timeout (opt-in) and #2313 budget
            // (signal-only). Cooperative only — does not unlock early (AC5).
            {
                const auto slo = mutation_hold_slo_us();
                if (slo > 0 && uus > slo) {
                    m->mutation_hold_slo_violation_total.fetch_add(1, std::memory_order_relaxed);
                    if (!mutation_hold_slo_soft_mode()) {
                        // Production default: fail mutation so agents cannot ship
                        // while spinning a long Guard (GC/steal tail bound).
                        if (flag_)
                            *flag_ = false;
                        success = false;
                    }
                    // Soft: counter only; success may remain true (AC2).
                }
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
    // ═══════════════════════════════════════════════════════════════════
    // Issue #2120: outermost exit unified order (documented AC5).
    //
    // Success path:
    //   1. exit_mutation_boundary (workspace commit + defuse bump)
    //   2. linear + dual-path + LifetimePin probes (under lock, depth held)
    //   3. GC defer release (after panic-checkpoint commit when applicable)
    //   4. hot-update throttle → reemit → epoch notify (#2090 / #2114)
    //   5. flush + held clear + depth_slot pop + unlock (LAST)
    //
    // Full-rollback (failed + panic_auto_rollback_): probes + restore
    // checkpoint + GC defer release; skip reemit coalesce.
    // Partial recovery (failed + !panic_auto_rollback_): probes + reemit
    // notify (keep panic checkpoint / defer for retry).
    //
    // Nested guards: only exit_mutation_boundary + depth_slot-- (outer owns pipeline).
    // ═══════════════════════════════════════════════════════════════════
    // Issue #2215: RenderFastExit — success + outermost entered under
    // render hotpath. Skip Full TypedMutationAudit / full linear+dual-path;
    // always keep pin restamp + unlock. Failure never uses fast exit.
    //
    // Issue #2311: RenderFastExit must NOT skip linear / match-site
    // hard-gate. If the outermost Guard encloses linear ops OR ADT match
    // sites OR requires_invariant_hard_gate fires, force full audit
    // path even under render hotpath. Closes the under-sample /
    // soft-continue hole that #2145/#2222/#2223 closed for Agent
    // mutate but #2215 reopened for render hotpath when the same Guard
    // encloses linear ops or match sites.
    const bool render_fast_candidate = outermost && success && render_fast_exit_;
    bool linear_ops_present_local = false;
    bool match_sites_present_local = false;
    std::uint64_t nodes_changed_local = 0;
    if (auto* ws = ev_->workspace_flat()) {
        const auto cur_dirty_calls = ws->mark_dirty_upward_call_count();
        nodes_changed_local = (cur_dirty_calls > dirty_upward_at_enter_)
                                  ? (cur_dirty_calls - dirty_upward_at_enter_)
                                  : 0;
        const auto n = ws->size();
        for (aura::ast::NodeId id = 0; id < n; ++id) {
            if (!ws->is_live_node(id))
                continue;
            if (ws->has_match_info(id))
                match_sites_present_local = true;
            // Linear detection: mirror subtree_has_linear_ops (type_checker_impl.cpp:64)
            // for any live node. Look for Linear/Move/Borrow/MutBorrow/Drop
            // NodeTag — the same set the type-checker uses to detect linear
            // ops in a subtree. This is broader than the audit path's
            // audit_op string heuristic (which only checks for op-name
            // substrings) but the wider net is the fail-closed default.
            const auto v = ws->get(id);
            if (v.tag == aura::ast::NodeTag::Linear || v.tag == aura::ast::NodeTag::Move ||
                v.tag == aura::ast::NodeTag::Borrow || v.tag == aura::ast::NodeTag::MutBorrow ||
                v.tag == aura::ast::NodeTag::Drop) {
                linear_ops_present_local = true;
            }
            if (linear_ops_present_local && match_sites_present_local)
                break;
        }
    }
    const bool strict_sandbox_local = aura::core::sandbox::is_strict();
    const bool hard_gate_local =
        typed_audit::requires_invariant_hard_gate(nodes_changed_local, linear_ops_present_local,
                                                  strict_sandbox_local, match_sites_present_local);
    const bool linear_or_match_suppress =
        linear_ops_present_local || match_sites_present_local || hard_gate_local;
    const bool render_fast = render_fast_candidate && !linear_or_match_suppress;
    if (render_fast_candidate && linear_or_match_suppress) {
        if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_)) {
            m->render_fast_exit_suppressed_linear_or_match_total.fetch_add(
                1, std::memory_order_relaxed);
            if (linear_ops_present_local)
                m->render_fast_exit_suppressed_linear_total.fetch_add(1, std::memory_order_relaxed);
            if (match_sites_present_local)
                m->render_fast_exit_suppressed_match_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    ev_->render_fast_exit_this_boundary_ = render_fast;
    if (render_fast) {
        if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_))
            m->render_fast_exit_total.fetch_add(1, std::memory_order_relaxed);
    }
    ev_->exit_mutation_boundary(success);
    ev_->render_fast_exit_this_boundary_ = false;
    // Issue #2120: keep per-fiber mutation stack depth visible during
    // exit pipeline so steal/GC do not observe "depth==0 mid-probes".
    // exit_mutation_boundary already popped the real checkpoint; push a
    // lightweight fence that is removed just before unlock.
    bool exit_fence_pushed = false;
    if (outermost) {
        Evaluator::MutationCheckpoint fence{};
        fence.version = ev_->defuse_version_.load(std::memory_order_relaxed);
        fence.evaluator_id = static_cast<void*>(ev_);
        Evaluator::active_mutation_stack_static().push_back(std::move(fence));
        exit_fence_pushed = true;
        if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_))
            m->outermost_exit_phase1_probes_total.fetch_add(1, std::memory_order_relaxed);
    }
    // ── Phase 1: linear + dual-path + LifetimePin probes ──
    // Issue #1486 / #1545 / #1568 / #1634 / #2120 / #2131
    // Issue #2131: outermost exit walks GcCoord PrePin → PostAudit
    // (boundary path; audit runs inside enforce helpers).
    // Issue #2215 RenderFastExit: skip Full linear + dual-path EnvFrame walk
    // (frame budget); still restamp pins for live render buffers.
    std::optional<gc_coord::Scope> boundary_gc_coord;
    if (outermost)
        boundary_gc_coord.emplace(gc_coord::Path::Boundary);
    if (outermost) {
        if (render_fast) {
            // Fast: skip enforce_linear_boundary_consistency + dual-path scan.
            if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_))
                m->render_fast_exit_skipped_audit_total.fetch_add(1, std::memory_order_relaxed);
            if (boundary_gc_coord)
                boundary_gc_coord->enter_cascade();
        } else if (!success) {
            // Issue #1951: 4-step closed-loop pattern consolidated helper.
            (void)ev_->enforce_linear_post_failure(Evaluator::kLinearGcRootAuditTypedMutate);
            if (boundary_gc_coord)
                boundary_gc_coord->enter_cascade();
            // dual-path still on failure path below for non-fast
            {
                std::shared_lock<std::shared_mutex> rlock(ev_->env_frames_lock());
                const auto n = ev_->env_frames_size();
                for (EnvId id = 0; id < n; ++id) {
                    if (!ev_->is_valid_env_id(id))
                        continue;
                    const auto& fr = ev_->env_frame(id);
                    if (fr.version_ == INVALID_VERSION)
                        continue;
                    (void)ev_->ensure_envframe_dual_path_consistency(fr);
                }
            }
        } else {
            (void)ev_->enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditTypedMutate,
                                                           /*mark_all_linear=*/false);
            // Issue #2067: post-mutate force-rollback observability bump.
            if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_)) {
                m->linear_post_mutate_force_rollback_total.fetch_add(1, std::memory_order_relaxed);
            }
            if (boundary_gc_coord)
                boundary_gc_coord->enter_cascade();
            // Issue #2120 / #2116: dual-path consistency probe at boundary exit
            // (no half-consistent EnvFrame left live after probes).
            {
                std::shared_lock<std::shared_mutex> rlock(ev_->env_frames_lock());
                const auto n = ev_->env_frames_size();
                for (EnvId id = 0; id < n; ++id) {
                    if (!ev_->is_valid_env_id(id))
                        continue;
                    const auto& fr = ev_->env_frame(id);
                    if (fr.version_ == INVALID_VERSION)
                        continue;
                    (void)ev_->ensure_envframe_dual_path_consistency(fr);
                }
            }
        }
        // Issue #1500 / #2085: LifetimePin + StableNodeRef restamp under lock
        // (BEFORE reemit so hot-update sees consistent pins — #2120 order).
        // Always on outermost (including RenderFastExit — live render buffers).
        (void)ev_->restamp_pinned_stable_refs();
        const std::uint64_t boundary_gen =
            ev_->workspace_flat() != nullptr ? ev_->workspace_flat()->generation() : 0;
        // Module-only free function (lifetime_pin.ixx). Do not include
        // lifetime_pin.hh in this TU — dual include+import is ambiguous.
        const auto n_pins =
            aura::core::lifetime::restamp_all_pins_for_arena(std::uint64_t{0}, boundary_gen);
        if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics())) {
            if (n_pins > 0)
                m->lifetime_pin_restamps_total.fetch_add(static_cast<std::uint64_t>(n_pins),
                                                         std::memory_order_relaxed);
        }
        // Issue #2003: EnvFrame lifetime scan at boundary exit.
        {
            aura::core::envframe_lifetime::EnvFrameLifetimeGuard envframe_guard{
                aura::core::envframe_lifetime::make_envframe_lifetime_host_with(
                    const_cast<void*>(static_cast<const void*>(ev_)),
                    &::aura::compiler::Evaluator::envframe_lifetime_trampoline),
                aura::core::envframe_lifetime::EnvFrameLifetimeSite::BoundaryExit};
            (void)envframe_guard.site();
        }
        // Issue #2131: PostAudit after pin/probe phase (audit ran in enforce).
        if (boundary_gc_coord)
            boundary_gc_coord->after_cascade();
    }
    // ── Phase 2–3: panic checkpoint + GC defer (before reemit) ──
    // Issue #241 / #2120: commit/restore under lock so dual-epoch observers
    // never see a pending panic window after probes.
    //
    // commit_panic_checkpoint / restore_panic_checkpoint (ok path) already
    // call release_gc_defer_for_pending_panic once — do NOT double-release
    // (process-wide depth would underflow other evaluators' arms).
    // Partial recovery (failed + !auto_rollback) keeps checkpoint + defer.
    // Success AC1: drain any residual panic-defer still attributed to this
    // evaluator after commit (steal-orphan / multi-arm edge cases).
    bool panic_handled = false;
    if (outermost && had_panic_checkpoint_) {
        if (success) {
            ev_->commit_panic_checkpoint(); // includes release_gc_defer once
            panic_handled = true;
        } else if (ev_->panic_auto_rollback_) {
            (void)ev_->restore_panic_checkpoint(); // releases on successful restore
            panic_handled = true;
        }
        // else partial recovery: leave checkpoint + defer armed
        if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_))
            m->outermost_exit_phase3_gc_defer_total.fetch_add(1, std::memory_order_relaxed);
    }
    // AC1: outermost success → no residual panic-defer for this evaluator.
    // Use clear_gc_defer_for_evaluator (not release_gc_defer_for_pending_panic):
    // commit/restore already cleared the Evaluator arm flag, so the flag-gated
    // release would no-op while table depth from steal-orphan / external arm
    // could remain — a while(release) loop would spin forever.
    if (outermost && success) {
        if (aura::gc_hooks::gc_deferred_for_evaluator(static_cast<void*>(ev_)))
            (void)aura::gc_hooks::clear_gc_defer_for_evaluator(static_cast<void*>(ev_));
        if (!had_panic_checkpoint_) {
            if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_))
                m->outermost_exit_phase3_gc_defer_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // ── Phase 4: hot-update throttle → reemit → epoch notify (#2090 / #2114 / #2162) ──
    // Issue #2090 / #2162: outermost dtor drives the single-owner helper so
    // fiber-steal restore / partial recovery / exception unwind / compact-only
    // share the same ordered sequence (no silent stale closures).
    // Issue #2215 RenderFastExit: defer synchronous reemit under render hotpath
    // success — HotUpdateRegistry Defer policy already fail-closed for steal;
    // do not block present on full reemit. Non-render / failure still recover.
    if (outermost) {
        if (render_fast) {
            const std::uint64_t cur_defuse = ev_->defuse_version_.load(std::memory_order_acquire);
            const std::uint64_t dirty_calls =
                ev_->workspace_flat_ ? ev_->workspace_flat_->mark_dirty_upward_call_count() : 0;
            const bool dirty =
                (cur_defuse != defuse_version_at_enter_) || (dirty_calls > dirty_upward_at_enter_);
            if (dirty) {
                if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_))
                    m->render_fast_exit_deferred_reemit_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                // Epoch notify only (no sync reemit) so dual-epoch observers
                // see a bump; actual reemit coalesces on next non-render boundary.
                aura_hot_update_notify_epoch_bump(aura_aot_func_table_epoch());
            }
            if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_))
                m->outermost_exit_phase4_reemit_total.fetch_add(1, std::memory_order_relaxed);
        } else {
            ev_->run_hot_update_recovery_if_needed(success, defuse_version_at_enter_,
                                                   dirty_upward_at_enter_);
            if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_))
                m->outermost_exit_phase4_reemit_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // ── Phase 5: flush + depth/unlock LAST (#2120) ──
    if (outermost) {
        ev_->flush_mutation_boundary();
        if (ev_->compiler_metrics())
            aura_macro_hygiene_snapshot_metrics(ev_->compiler_metrics());
        // Pop exit fence before depth_slot-- so steal sees consistent zero.
        if (exit_fence_pushed) {
            auto& st = Evaluator::active_mutation_stack_static();
            if (!st.empty())
                st.pop_back();
            exit_fence_pushed = false;
        }
        // depth_slot last (paired with ctor ++; was early-decrement pre-#2120).
        if (slot)
            (*slot)--;
        ev_->mutation_boundary_held_.store(false, std::memory_order_release);
        // Issue #2517: clear process-wide live max probe if this fiber owns it.
        // Simplified exit: only clear when we are the recorded max holder
        // (next enter rebuilds; best-effort under multi-eval contention).
        aura::compiler::mutation_hold_live_note_exit(aura_fiber_current_id());
        // Issue #2204: release MutationHold when logical hold ends (paired
        // with held_ clear, after exit probes / reemit / flush). Abort
        // (strict force-fail) still reaches here via dtor — bit always
        // released. Nested guards never armed so never release here.
        aura::gc_hooks::release_mutation_hold_defer();
        // Issue #2211 / #2269 / #2296: residual GcDeferReason policy after
        // residual drain (phase3 panic clear + phase5 MutationHold release).
        //
        // ── Decision table (Soft / Clear / Hard × single vs multi-eval) ──
        // | Policy | Env select                         | Residual≠0 action              |
        // Multi-eval note                          | | Soft   | AURA_SANDBOX=off | metric only
        // (legacy #2211)     | no clear; test/sandbox                   | | Clear  | production
        // default (unset policy)  | force_clear_all_for_eval + hold | panic table + bit reconcile
        // (#2296)      | | Hard   | AURA_RESIDUAL_DEFER_POLICY=hard or  | hard-fail metric + abort
        // | same as single; fail closed              | |        | AURA_HARD_RESIDUAL_DEFER=1 | | |
        // Happy path (residual==0): single relaxed snapshot load, zero clear work (AC3).
        // Fail/partial-recovery paths intentionally leave checkpoint+defer armed.
        if (success) {
            // AC3: zero-cost success path — single relaxed load of
            // defer_reasons_snapshot(); if zero, skip the entire
            // residual block (no extra clears / bumps / abort probes).
            const auto residual = aura::gc_hooks::defer_reasons_snapshot();
            if (residual != 0) {
                if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_))
                    m->mutation_boundary_residual_defer_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                // Issue #2269: production-default policy.
                // AURA_RESIDUAL_DEFER_POLICY=hard | clear | unset
                // (unset defaults to 'clear' under production security
                // defaults, 'soft' under AURA_SANDBOX=off).
                // Legacy AURA_HARD_RESIDUAL_DEFER=1 still maps to hard
                // for backward compat (kept so pre-#2269 deploys keep
                // their hard-fail behavior).
                const char* policy_e = std::getenv("AURA_RESIDUAL_DEFER_POLICY");
                const bool policy_hard_env =
                    policy_e && *policy_e && std::string_view(policy_e) == "hard";
                const char* legacy_e = std::getenv("AURA_HARD_RESIDUAL_DEFER");
                const bool legacy_hard = legacy_e && *legacy_e && legacy_e[0] != '0';
                const char* sandbox_e = std::getenv("AURA_SANDBOX");
                const bool dev_off =
                    sandbox_e && *sandbox_e && std::string_view(sandbox_e) == "off";
                // Default under production security defaults: clear
                // (B path — availability-friendly). Sandbox / unit
                // tests: soft (legacy behavior).
                enum class ResidualPolicy { Soft, Clear, Hard };
                ResidualPolicy policy = ResidualPolicy::Soft;
                if (dev_off) {
                    policy = ResidualPolicy::Soft;
                } else if (policy_hard_env || legacy_hard) {
                    policy = ResidualPolicy::Hard;
                } else {
                    // unset AURA_RESIDUAL_DEFER_POLICY + production
                    // security defaults active → clear (B).
                    policy = ResidualPolicy::Clear;
                }
                if (policy == ResidualPolicy::Hard) {
                    if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_))
                        m->mutation_boundary_residual_defer_hard_fail_total.fetch_add(
                            1, std::memory_order_relaxed);
                    // Hard mode: fail closed so sticky defer never ships silent.
                    assert(residual == 0 && "Issue #2211/#2269: residual GcDeferReason after "
                                            "outermost success (hard policy)");
                    // NDEBUG strips assert — still abort when hard policy requested.
                    if (aura::gc_hooks::defer_reasons_snapshot() != 0)
                        std::abort();
                } else if (policy == ResidualPolicy::Clear) {
                    // Issue #2600: shared exit helper — same residual
                    // clear + hold release steps the soft fiber boundary
                    // exit uses (orch_soft_boundary_exit in
                    // evaluator_fiber_mutation.cpp). Closes dual-rail
                    // drift between soft + full Guard paths. Stack-light
                    // (no full Guard construction). Idempotent (atomic +
                    // CAS-based — calling twice does not double-bump
                    // counters). The shared helper also matches the
                    // #2314 steal-complete interlock (same essential
                    // operations). Per-evaluator clear + hold release
                    // (the previous force_clear_all_gc_defer_for_evaluator
                    // was a superset of per-evaluator clear; per-evaluator
                    // is sufficient since each evaluator handles its own
                    // defer via its own soft/full Guard exit).
                    // We capture the clear result BEFORE the helper so the
                    // #2296 per-evaluator metrics still bump correctly
                    // (the helper doesn't surface per-evaluator clear
                    // counters — those are computed here from the original
                    // call signature).
                    const auto fr = aura::gc_hooks::force_clear_residual_defer_for_evaluator(
                        static_cast<void*>(ev_));
                    // Shared exit: also releases MutationHold + reconciles
                    // (covers both soft + full Guard paths in one place).
                    aura::compiler::mutation_boundary_shared_exit(static_cast<void*>(ev_));
                    if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_)) {
                        const auto n = (fr.panic_depth_cleared > 0)
                                           ? static_cast<std::uint64_t>(fr.panic_depth_cleared)
                                           : 1u;
                        m->mutation_boundary_residual_defer_forced_clear_total.fetch_add(
                            n, std::memory_order_relaxed);
                    }
                }
                // Soft (sandbox / unit tests): no clear, no abort, just
                // the residual metric bump above. Legacy #2211 behavior.
            }
        }
        // Issue #2184: clear fiber-local held mirror after outermost exit.
        // fiber.cpp helper keeps TLS null-check + member call co-located —
        // required for ubsan-smoke (test_ir set-code Guard dtor on host
        // thread where g_current_fiber is null).
        aura::serve::publish_current_fiber_mutation_safety(
            Evaluator::active_mutation_stack_static().size(), /*held=*/false,
            defuse_version_at_enter_);
        // Issue #2121: unlock matching acquire mode.
        if (region_mode_) {
            if (region_lock_.owns_lock()) {
                region_lock_.unlock();
                ev_->workspace_region_holders_[region_shard_].fetch_sub(1,
                                                                        std::memory_order_relaxed);
            }
            if (shared_lock_.owns_lock())
                shared_lock_.unlock();
        } else if (lock_.owns_lock()) {
            lock_.unlock();
        }
        aura::compiler::lock_order::on_release(aura::compiler::lock_order::Level::Workspace);
        ev_->outermost_mutation_success_flag_ = nullptr;
        // Issue #2347: clear TLS Guard-window reject count so multi-round
        // mutates do not accumulate a stale threshold across outermost
        // boundaries (Soft dashboard + Strict force-rollback both reset).
        aura::serve::mf_mailbox::clear_recv_boundary_reject_window();
        // Issue #2378 / #2511 / #2551: outermost exit forces mailbox deferred
        // drain under budget (hold-exit SLA). Success + exception paths both
        // hit this Phase-5 block (Guard dtor). AC5: free when deferred_depth==0
        // (single relaxed load). Soft: retain open depth + starvation bump;
        // Strict/production: force-resolve remaining + audit; residual after
        // budget → hard counter + Agent throttle flag (#2551).
        // Wraps note_mailbox_outermost_exit_drain (#2378 opportunity stamp).
        (void)aura::serve::mf_mailbox::drain_deferred_under_budget();
        ev_->unbind_yield_hook_evaluator();
        // Issue #2170: publish LayoutStamp at outermost exit (Phase 5).
        // Captures the post-mutation stamp (env_generation_ + defuse_version_
        // already bumped by exit_mutation_boundary; arena_gen + flat_gen
        // reflect the latest compact state). Companion publisher lives
        // at live_compact success path (compact_env_frames is a follow-up
        // — #2170 Phase 2 — wired through the same publish_layout_stamp()
        // helper so the last-stamp fields stay consistent regardless of
        // which path bumps the underlying generations).
        ev_->publish_layout_stamp();
        // Issue #2250: write current LayoutStamp into the current
        // Fiber (fence captured BEFORE unlock so a concurrent reemit
        // by another fiber of the same Evaluator is detectable at
        // Fiber::resume / refresh_stale_frames_after_steal). 6-field
        // POD copied by value (LayoutStamp is trivially copyable
        // per #2170 contract).
        // Issue #2436: LayoutStamp is published *after* Moving densify +
        // ShapeProfiler/IR dirty close (step 9 of post_compact_lifecycle.hh)
        // so shape_version + ir_soa_generation fences see post-compact truth.
        // Soft / no-compact still publishes once below (same POD cost).
        //
        // Issue #2256 + #2257: trigger Moving compaction after
        // outermost Guard exit (post-publish). Honors the pin-or-
        // remap hard contract: compact_all_moving_pinned() (issue #2266)
        // returns AdaptiveCompactResult with bytes_reclaimed_total +
        // pin_contract_held (verify_pins_under_moving_compact() is now
        // fail-closed and runs inside live_compact(Moving) per arena).
        // The compact path also bumps shape_version via
        // ShapeProfiler::on_arena_compact (per #2255/#2256); deopt-
        // storm enter (per #2257) bumps the same file-scope atomic
        // independently. Source-cite StormLevel facade integration
        // lives at #2094 lineage (StormLevel::None default; storm
        // enter → adaptive partial-relower threshold widens).
        // moving_compact_enabled lives in aura::ast (arena.ixx);
        // pin verify in aura::core::lifetime (lifetime_pin.ixx).
        bool pin_contract_held = true; // #2266 — default true (no Moving = contract held)
        // Issue #2353: true only when Moving densify actually relocated live objects
        // (Soft / empty densify → false → AC3 zero-cost revalidate early return).
        bool had_moving_densify = false;
        // Issue #2619: last densify window aggregates for Agent health surface.
        std::size_t densify_objects_moved = 0;
        std::size_t densify_untracked_kept = 0;
        bool densify_incomplete_remap = false;
        std::size_t densify_root_remap_fails = 0;
        // Stash RootRemap axes for #2682 unified success predicate (lives
        // outside the moving_compact_enabled() block — compact_r is scoped
        // to that if-body).
        std::uint64_t densify_root_remap_stable_ref_fail = 0;
        std::uint64_t densify_root_remap_closure_capture_fail = 0;
        // Issue #2499 / #2559: densify-call RootRemap axis (last-call fail totals
        // == 0). Three-layer memory inventory: pin ∧ root_remap ∧ scan_fail.
        // Soft / no Moving → vacuous true. Used for DensifyConsistencyReport so
        // force_reason reports "root_remap" when only RootRemap fails (not "pin").
        bool densify_root_remap_call_ok = true;
        // Pin axis for the report (pin-verify only when RootRemap fails alone).
        bool densify_pin_axis_ok = true;
        // Issue #2497 / #2559: baseline ownership-scan fail counter BEFORE the
        // Moving densify window opens. Any fail delta across compact + pairing
        // + injected tests must suppress Phase 5 success the same way
        // pin_contract_held does (no path where scan fail is metrics-only —
        // mirrors #2266 fail-closed). Cross-layer inventory gate: #2559.
        std::uint64_t scan_fail_baseline = 0;
        // Issue #2595: untracked external roots counter baseline (#2495
        // source). Any growth during the densify window → untracked_ok=false
        // and the unified densify gate (overall_ok) fails. Same fail-closed
        // shape as the envframe scan baseline above. Closes the
        // half-green window where pin ok + root_remap ok + envframe ok but
        // any arena left untracked_kept_count > 0 after Moving relocate.
        std::uint64_t untracked_baseline = 0;
        // Issue #2595: panic checkpoint depth baseline (gc_hooks). Phase 5
        // may not claim success while a panic checkpoint is live AND not
        // deferred via gc_deferred_for_evaluator (half-green densify can
        // leak panic mid-stack). Baseline captured before compact; post-
        // compact depth (>= baseline + any new arm) drives panic_residual_ok
        // under production / when !gc_deferred_for_evaluator.
        std::uint32_t panic_depth_baseline = 0;
        if (aura::ast::moving_compact_enabled()) {
            // Issue #2497: snapshot densify-ownership-scan fail baseline before
            // compact runs (covers compact callbacks + pairing + injected fails
            // across the entire Moving densify window).
            scan_fail_baseline = aura::core::envframe_lifetime::
                envframe_lifetime_densify_ownership_scan_fail_total();
            // Issue #2595: capture untracked + panic baselines for the
            // densify success gate. Both are read AFTER compact_all_moving_pinned
            // completes (paired with the envframe scan-fail baseline pattern
            // above) so any test injection or production callback that bumps
            // either counter during the densify window surfaces as
            // !untracked_ok / !panic_residual_ok.
            untracked_baseline =
                aura::ast::g_moving_untracked_external_roots_total.load(std::memory_order_relaxed);
            panic_depth_baseline =
                aura::gc_hooks::g_gc_defer_pending_panic_depth.load(std::memory_order_relaxed);
            const auto compact_r = ev_->arena_group_
                                       ? ev_->arena_group_->compact_all_moving_pinned()
                                       : aura::ast::AdaptiveCompactResult{};
            if (compact_r.bytes_reclaimed_total > 0) {
                if (auto* mm = static_cast<CompilerMetrics*>(ev_->compiler_metrics_))
                    mm->arena_compact_deopt_triggered_total.fetch_add(
                        static_cast<std::uint64_t>(compact_r.bytes_reclaimed_total),
                        std::memory_order_relaxed);
            }
            pin_contract_held = compact_r.pin_contract_held;
            had_moving_densify = compact_r.moved_live_objects;
            // Issue #2619: capture window aggregates for Agent densify-health.
            densify_objects_moved = compact_r.objects_moved_total;
            densify_untracked_kept = compact_r.untracked_kept_total;
            densify_incomplete_remap = compact_r.moving_incomplete_remap_any;
            densify_root_remap_stable_ref_fail =
                static_cast<std::uint64_t>(compact_r.root_remap_stable_ref_fail_total);
            densify_root_remap_closure_capture_fail =
                static_cast<std::uint64_t>(compact_r.root_remap_closure_capture_fail_total);
            densify_root_remap_fails = compact_r.root_remap_stable_ref_fail_total +
                                       compact_r.root_remap_closure_capture_fail_total;
            // Issue #2499: densify-call RootRemap fail axis (last-call totals
            // aggregated by compact_all_moving_pinned). live_compact already
            // folds non-zero fails into pin_contract_held at the densify source;
            // keep a defensive AND so AdaptiveCompactResult + Phase 5 always
            // share one Moving success gate (#2266 pin shape + #2497 scan).
            // Closes "pin ok + root_remap fail cumulative" mixed-signal gap
            // from 2026-07-31 production review 建议 5.
            // Keep compact_r.* == 0 form for #2559 AC3 source-cite linter.
            const bool root_remap_call_ok = compact_r.root_remap_stable_ref_fail_total == 0 &&
                                            compact_r.root_remap_closure_capture_fail_total == 0;
            pin_contract_held = pin_contract_held && root_remap_call_ok;
            // Stash axis for DensifyConsistencyReport below (force_reason).
            // When only RootRemap fails, pin_contract_held is false (unified)
            // but pin_ok for the report stays true so force_reason == root_remap
            // (not pin) — matches AC1 + densify_consistency priority table.
            densify_root_remap_call_ok = root_remap_call_ok;
            densify_pin_axis_ok = root_remap_call_ok ? pin_contract_held : true;
            if (!pin_contract_held) {
                // Issue #2266 AC2: contract failed — bump fail counter + do not
                // publish success metrics as if contract held. Optional env
                // AURA_MOVING_PIN_CONTRACT=hard forces hard-fail (default
                // hard under production security defaults per #2256 / #2266).
                if (auto* mm = static_cast<CompilerMetrics*>(ev_->compiler_metrics_))
                    mm->moving_compact_pin_contract_fail_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                const char* contract_env = std::getenv("AURA_MOVING_PIN_CONTRACT");
                const bool hard_fail =
                    (contract_env != nullptr && std::string(contract_env) == "hard");
                if (hard_fail) {
                    std::fprintf(stderr, "[#2266] Moving pin contract failed under "
                                         "AURA_MOVING_PIN_CONTRACT=hard — aborting\n");
                    std::abort();
                }
                // Soft mode: log + continue (do not publish success metrics below).
                std::fprintf(stderr, "[#2266] Moving pin contract failed (soft mode) — "
                                     "suppressing success metrics\n");
            }
        }
        // Issue #2266 AC2: do NOT publish success metrics if pin contract failed.
        // (Gated on pin_contract_held — false suppresses outermost_exit_phase5_unlock
        // + outermost_exit_order_complete so Agents see the contract miss, not a
        // false success.)
        //
        // Issue #2341: compute DensifyConsistencyReport once at densify
        // success (after RootRemap + pin verify + closure remount).
        // Gate the same success metrics on overall_ok() — mirrors the
        // pin_contract_held gating above. Bumps unified fail counter
        // on !overall_ok() (per-axis fail counters remain additive).
        //
        // Issue #2353: after pin verify, ordered Linear+Type revalidate
        // composes into densify_consistency.linear_ok / type_ok. Soft /
        // empty densify / no linear → early true (AC3 zero cost). Fail
        // suppresses Phase 5 success via overall_ok() (AC2 fail-closed).
        aura::core::densify_consistency::DensifyConsistencyReport densify_consistency;
        // Issue #2499: pin_ok is pin-verify axis only when RootRemap alone fails
        // (densify_pin_axis_ok); unified pin_contract_held still gates success.
        densify_consistency.pin_ok = densify_pin_axis_ok;
        // Issue #2595: untracked_ok = no moving_incomplete_remap delta during
        // the densify window. Soft / no Moving → vacuous true. The untracked
        // axis catches #2495 half-green (pin_ok true + any arena leaving
        // untracked_kept_count > 0 after relocate_tracked_objects_for_moving_).
        // Read the delta AFTER compact_all_moving_pinned + pairing (any
        // injection during either window bumps g_moving_untracked_external_roots_total).
        const auto untracked_after =
            aura::ast::g_moving_untracked_external_roots_total.load(std::memory_order_relaxed);
        const bool untracked_ok = !had_moving_densify || (untracked_after <= untracked_baseline);
        densify_consistency.untracked_ok = untracked_ok;
        // Issue #2595: panic_residual_ok = no live panic_cp OR
        // gc_deferred_for_evaluator is true (defer armed so the panic can
        // be drained before Phase 5 publishes success). If a panic_cp is
        // armed mid-densify AND the eval isn't deferring, !panic_residual_ok
        // so the unified gate fails (half-green densify can leak panic
        // mid-stack into a published success).
        const auto panic_depth_after =
            aura::gc_hooks::g_gc_defer_pending_panic_depth.load(std::memory_order_relaxed);
        const bool panic_cp_live_now = panic_depth_after > 0;
        const bool panic_deferred_now =
            aura::gc_hooks::gc_deferred_for_evaluator(static_cast<void*>(ev_));
        const bool panic_residual_ok = !panic_cp_live_now || panic_deferred_now;
        densify_consistency.panic_residual_ok = panic_residual_ok;
        // Issue #2353: linear_ok + type_ok from post-densify revalidate
        // (pin-subsumed linear pin verify remains in pin_ok; this is the
        // ownership + type axis complementary to #2341 object-axis).
        const bool linear_type_ok =
            pin_contract_held ? ev_->run_post_densify_linear_type_revalidate(had_moving_densify)
                              : false;
        densify_consistency.linear_ok = linear_type_ok;
        densify_consistency.type_ok = linear_type_ok;
        // Issue #2609: hard-AND linear force sticky + residual GcDefer into
        // densify success (same pure gate as steal-complete). Soft / no
        // production: metric-only observe when axes fail but keep Soft
        // vacuous axes unless overall densify already fails closed.
        // Production or Hard densify contract: force linear_ok / panic axis
        // so Phase 5 cannot publish half-green success.
        {
            const bool residual_zero = (aura::gc_hooks::defer_reasons_snapshot() == 0);
            // Type fence applied later on Moving success; for densify gate
            // evaluate residual + linear first (type fence is step after).
            const auto axis = ev_->evaluate_linear_type_provenance_hard_and(
                residual_zero, /*type_fence_applied=*/true);
            const char* densify_contract_env = std::getenv("AURA_DENSIFY_CONTRACT");
            const bool hard_densify = typed_audit::production_defaults_active() ||
                                      (densify_contract_env != nullptr &&
                                       std::string_view(densify_contract_env) == "hard");
            if (axis != Evaluator::LinearTypeProvenanceAxis::Ok) {
                if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_)) {
                    if (hard_densify) {
                        m->steal_densify_linear_type_hard_fail_total.fetch_add(
                            1, std::memory_order_relaxed);
                        m->steal_densify_linear_type_last_fail_axis.store(
                            static_cast<std::uint8_t>(axis), std::memory_order_relaxed);
                        if (axis == Evaluator::LinearTypeProvenanceAxis::LinearForcePending) {
                            m->steal_densify_linear_type_fail_linear_total.fetch_add(
                                1, std::memory_order_relaxed);
                            densify_consistency.linear_ok = false;
                        } else if (axis == Evaluator::LinearTypeProvenanceAxis::ResidualGcDefer) {
                            m->steal_densify_linear_type_fail_residual_total.fetch_add(
                                1, std::memory_order_relaxed);
                            densify_consistency.panic_residual_ok = false;
                        }
                    } else {
                        m->steal_densify_linear_type_soft_observe_total.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
            }
        }
        // Issue #2365 / #2368: densify-success closed-loop. Soft / no Moving
        // densify → root_remap / closure / envframe vacuous true (zero cost).
        // Moving + unified contract held → force_densify_remap_pairing() encodes
        // permanent order (do not open-code steps 3–5 here — pairing is never
        // optional):
        //   1 RootRemap probe (inside densify) → last_root_remap_any_fail
        //   2 pin verify (pin_contract_held above)
        //   3 EnvFrame live-ref transfer
        //   4 closure remount scan
        //   5 dual-epoch restamp (always last before report)
        //   6 report axes from pairing result + linear_type_ok
        // Issue #2361: real envframe_ok remains ownership scan + dual-epoch.
        if (had_moving_densify && pin_contract_held) {
            // Issue #2368: single forced pairing entry (order encoded in body).
            const auto pairing = ev_->force_densify_remap_pairing();
            // Issue #2497: re-check scan fail counter after pairing. Any delta
            // since the Moving densify baseline (compact + pairing + injected
            // fails) suppresses envframe_ok — same fail-closed shape as
            // pin_contract_held gating at #2266 / #2341 AC2.
            const auto scan_fail_after = aura::core::envframe_lifetime::
                envframe_lifetime_densify_ownership_scan_fail_total();
            const bool scan_fail_delta = (scan_fail_after > scan_fail_baseline);
            // Issue #2499: AND densify-call RootRemap fail totals into axis.
            densify_consistency.root_remap_ok = pairing.root_remap_ok && densify_root_remap_call_ok;
            densify_consistency.closure_remount_ok = pairing.closure_remount_ok;
            // Issue #2497: scan_fail_delta ANDs into envframe_ok. Pairing already
            // checks within-pairing delta — this gate widens the window so a
            // pre-pairing inject (test helper) or compact-callback fail also
            // suppresses.
            // Issue #2599: production-only gating — under production
            // (production_defaults_active()), scan_fail_delta forces
            // envframe_ok=false (commit barrier rejects outermost commit;
            // force_rollback_suggestion: EnvFrameDensifyOwnership). Under
            // Soft / sandbox=off, scan_fail_delta bumps
            // densify_ownership_scan_fail_total counter but keeps
            // envframe_ok=true (AC2 metric-only, no forced rollback solely
            // from EnvFrame axis). Phase 5 success still gated via
            // overall_ok() at #2266 site.
            {
                const bool prod_for_densify = typed_audit::production_defaults_active();
                const bool envframe_block = prod_for_densify && scan_fail_delta;
                densify_consistency.envframe_ok =
                    pairing.envframe_ok && linear_type_ok && !envframe_block;
            }
            aura::core::densify_consistency::note_last_densify_dual_epoch_ok(pairing.dual_epoch_ok);
            aura::core::densify_consistency::note_last_densify_remap_pairing_forced(pairing.forced);
        } else if (had_moving_densify) {
            // Issue #2499: Moving densify ran but unified contract failed —
            // still publish densify-call RootRemap axis (not vacuous true).
            // Soft vacuous remains on the Soft / empty branch below.
            densify_consistency.root_remap_ok = densify_root_remap_call_ok;
            densify_consistency.closure_remount_ok = true;
            densify_consistency.envframe_ok = true;
            aura::core::densify_consistency::note_last_densify_dual_epoch_ok(true);
            aura::core::densify_consistency::note_last_densify_remap_pairing_forced(false);
        } else {
            // Soft / empty densify: vacuous axes (do not read stale
            // last_root_remap or cumulative closure fails).
            densify_consistency.root_remap_ok = true;
            densify_consistency.closure_remount_ok = true;
            densify_consistency.envframe_ok = true;
            aura::core::densify_consistency::note_last_densify_dual_epoch_ok(true);
            // Soft never forced pairing (agents distinguish Soft vacuous
            // from Moving forced-ok).
            aura::core::densify_consistency::note_last_densify_remap_pairing_forced(false);
        }
        // Publish last densify axes for query:lifetime-contract-snapshot.
        // Issue #2376: last-call envframe + closure are the production
        // contract (not cumulative / not force-true under Moving). Soft
        // vacuous true is published only on the Soft branch above.
        // Call-seq bumps every Phase 5 report so Agents detect stale samples.
        using aura::core::densify_consistency::kDensifyEnvframeFailLinearType;
        using aura::core::densify_consistency::kDensifyFailNone;
        aura::core::densify_consistency::note_last_densify_root_remap_ok(
            densify_consistency.root_remap_ok);
        // Envframe fail code: Soft → 0; Moving pairing set ownership/dual;
        // linear_type composition can force envframe_ok false with code 3.
        std::uint8_t env_fc = kDensifyFailNone;
        if (!densify_consistency.envframe_ok) {
            if (had_moving_densify && pin_contract_held && !linear_type_ok &&
                aura::core::densify_consistency::last_densify_envframe_fail_code() ==
                    kDensifyFailNone) {
                env_fc = kDensifyEnvframeFailLinearType;
            } else {
                env_fc = aura::core::densify_consistency::last_densify_envframe_fail_code();
            }
        }
        aura::core::densify_consistency::note_last_densify_envframe_ok(
            densify_consistency.envframe_ok, env_fc);
        const std::uint8_t cl_fc =
            densify_consistency.closure_remount_ok
                ? kDensifyFailNone
                : aura::core::densify_consistency::last_densify_closure_fail_code();
        aura::core::densify_consistency::note_last_densify_closure_remount_ok(
            densify_consistency.closure_remount_ok, cl_fc);
        aura::core::densify_consistency::bump_last_densify_call_seq();
        // Issue #2619: publish Agent-visible Moving densify health window.
        // Soft/no densify → vacuous healthy (would-allow-mutate=true). Production
        // hard (#2596) + incomplete remap → agent_throttle (orch refuse mutate).
        {
            const bool incomplete =
                densify_incomplete_remap || !untracked_ok || densify_untracked_kept > 0;
            aura::core::moving_densify_health::publish_last_moving_densify_window(
                had_moving_densify, pin_contract_held && densify_consistency.pin_ok, incomplete,
                static_cast<std::uint64_t>(densify_objects_moved),
                static_cast<std::uint64_t>(densify_untracked_kept),
                static_cast<std::uint64_t>(densify_root_remap_fails));
        }
        // Issue #2682: single unified Moving success predicate — folds all
        // 5 conditions (moving_blocked_precondition / pin_contract_held /
        // root_remap fails / untracked_kept_count > 0 when objects_moved > 0)
        // into one boolean. Replaces the scattered local-variable checks
        // that previously lived inline. Bumps process-wide
        // g_moving_unified_success_total / g_moving_unified_fail_total for
        // Agent dashboards. AC3 Soft / observe-only: predicate runs in all
        // modes (no behavior change for Soft — counters bump either way).
        const bool moving_unified_success =
            aura::core::moving_densify_health::compute_moving_unified_success(
                /*moving_blocked_precondition=*/false, // not folded into AdaptiveCompactResult
                                                       // yet; pin_contract_held
                                                       // already covers the
                                                       // user-visible gate
                pin_contract_held,
                /*root_remap_stable_ref_fail_total=*/densify_root_remap_stable_ref_fail,
                /*root_remap_closure_capture_fail_total=*/densify_root_remap_closure_capture_fail,
                /*objects_moved=*/static_cast<std::uint64_t>(densify_objects_moved),
                /*untracked_kept_count=*/static_cast<std::uint64_t>(densify_untracked_kept));
        if (moving_unified_success) {
            aura::ast::g_moving_unified_success_total.fetch_add(1, std::memory_order_relaxed);
        } else if (had_moving_densify) {
            // Only bump fail when densify actually ran (vacuous healthy on
            // Soft / no-densify windows stays out of the fail counter).
            aura::ast::g_moving_unified_fail_total.fetch_add(1, std::memory_order_relaxed);
        }
        if (!densify_consistency.overall_ok()) {
            // Issue #2341 AC2: unified fail — mirror pin_contract_held
            // gating above. Bump fail counter; optional hard abort
            // when AURA_DENSIFY_CONTRACT=hard (aligns RootRemap hard
            // contract pattern at root_remap_pass.ixx:380).
            aura::core::densify_consistency::bump_densify_consistency_fail_total();
            // Issue #2595: unified gate fail counter (additive schema key
            // densify_unified_gate_fail_total). Bumps in lockstep with the
            // existing g_densify_consistency_fail_total so production
            // dashboards can distinguish the new half-green axes (untracked,
            // panic_residual) from the legacy #2341 axes (pin / linear /
            // type / root_remap / closure / envframe).
            aura::core::densify_consistency::bump_densify_unified_gate_fail_total();
            const char* contract_env = std::getenv("AURA_DENSIFY_CONTRACT");
            const bool hard_fail = (contract_env != nullptr && std::string(contract_env) == "hard");
            if (hard_fail) {
                std::fprintf(stderr,
                             "[#2341] Densify consistency contract failed: %s "
                             "(AURA_DENSIFY_CONTRACT=hard) — aborting\n",
                             densify_consistency.force_reason());
                std::abort();
            }
            std::fprintf(stderr,
                         "[#2341] Densify consistency contract failed (soft mode): %s "
                         "— suppressing success metrics\n",
                         densify_consistency.force_reason());
        }
        if (pin_contract_held && densify_consistency.overall_ok()) {
            // Issue #2673: hard-path lock — densify → linear-root consistency
            // scan (refine #2642). Under prod/Full, mismatch forces
            // force_linear_rollback(LinearDensifyRootMismatch) instead of
            // advancing Phase 5 success metrics. Under Soft, observe counter
            // bumps (no force). linear_ops_present short-circuits zero cost
            // when no linear-typed binding / held linear root exists (AC3).
            // AC4: existing densify_consistency.overall_ok() AND preserved
            // (scan is additional, not replacement). AC5: #2664 external-root
            // hard-fail lives on a separate authority path.
            //
            // Issue #2673: chaos + linter self-coverage lives in the
            // ac2673_chaos_soak_and_linter test
            // (tests/compiler/test_densify_ownership_scan_fail_gate.cpp). 64 fibers × mutate linear
            // × densify — every prod-path scan forces force_linear_rollback (no silent continue).
            const bool densify_scan_mismatch =
                ev_->scan_linear_roots_after_densify(linear_ops_present_local);
            if (densify_scan_mismatch) {
                // Mismatch detected → force_linear_rollback bumps
                // linear_densify_scan_mismatch_total and sets
                // deny_kind=linear-densify-root-mismatch. Do NOT advance
                // Phase 5 success metrics below.
                ev_->force_linear_rollback("densify-phase5-linear-scan");
            } else if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_)) {
                m->outermost_exit_phase5_unlock_total.fetch_add(1, std::memory_order_relaxed);
                m->outermost_exit_order_complete_total.fetch_add(1, std::memory_order_relaxed);
            }
            // Issue #2507: Moving densify success → invalidate escape /
            // MoveOp elision gate for this eval. Remap may invalidate
            // escape-clean assumptions under the same cache_epoch.
            // Soft densify / no Moving: skip (zero cost). Clear by metrics*
            // identity (all cow_gen) to preserve #2286 cross-eval isolation.
            if (had_moving_densify) {
                if (void* m = ev_->compiler_metrics())
                    aura::compiler::note_escape_gate_clear_on_densify(m);
                // Issue #2552 AC3: pair densify escape-clear with type
                // OccurrenceGoal + type_dep epoch fence. Soft densify /
                // no Moving: skip (zero cost).
                // Issue #2609: fence is the type axis of the hard-AND;
                // residual/linear already gated overall_ok above.
                ev_->note_type_freshness_after_steal_or_densify();
            }
            // Issue #2360: the post-densify ownership-exit scan at the
            // Moving densify success site (Phase 5) is wired by #2361
            // (envframe_ok computation above) — single call site, no
            // duplicate scan here.
            // Issue #2436: lifecycle close steps 9–10 after densify 1–6 +
            // arena hook (steps 7–8 Shape + IR dirty). Soft densify → soft_skip.
            if (had_moving_densify) {
                aura::core::post_compact_lifecycle::note_lifecycle_run();
            } else {
                aura::core::post_compact_lifecycle::note_lifecycle_soft_skip();
            }
        } else if (!pin_contract_held) {
            aura::core::post_compact_lifecycle::note_lifecycle_pin_fail();
        }
        // Issue #2436 AC4: LayoutStamp re-publish AFTER compact + shape/IR
        // close so fiber resume fences observe post-compact shape_version +
        // ir_soa_generation (lifecycle step 9). publish + set_resume stay
        // adjacent (#2250 Phase 5 ordering proximity).
        (void)ev_->publish_layout_stamp();
        if (auto* cur_fiber = aura::serve::g_current_fiber) {
            const auto stamp = ev_->current_layout_stamp();
            cur_fiber->set_resume_layout_stamp(
                stamp.arena_id, stamp.arena_gen, stamp.flat_gen, stamp.mutation_epoch,
                stamp.env_gen, stamp.defuse_version, stamp.shape_version, stamp.ir_soa_generation);
            aura::core::post_compact_lifecycle::note_lifecycle_stamp_publish();
        }
        // Issue #2364: PanicCheckpoint residual × densify closed loop.
        // After densify (success or fail leaving evaluator live), residual
        // Panic defer must not outlive a cleared checkpoint; a still-live
        // checkpoint re-arms defer. Soft / no densify / no panic → free.
        // densify_attempted: Moving was enabled this exit (compact call ran).
        {
            const bool densify_attempted = aura::ast::moving_compact_enabled();
            // Checkpoint may still be live on partial recovery (failed +
            // !auto_rollback) or if densify raced a concurrent save.
            const bool has_cp = ev_->has_panic_checkpoint();
            const auto audit = aura::gc_hooks::audit_panic_defer_after_densify(
                static_cast<void*>(ev_), has_cp, densify_attempted);
            if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics_)) {
                if (!audit.free_path)
                    m->panic_defer_after_densify_total.fetch_add(1, std::memory_order_relaxed);
                if (audit.cleared)
                    m->panic_defer_after_densify_cleared_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                if (audit.rearmed)
                    m->panic_defer_after_densify_rearmed_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                if (audit.hard_fail)
                    m->panic_defer_after_densify_hard_fail_total.fetch_add(
                        1, std::memory_order_relaxed);
            }
            (void)audit;
        }
    } else {
        // Nested: depth_slot-- (outermost deferred this to phase 5).
        if (slot)
            (*slot)--;
    }
    // Issue #241 residual: partial-recovery panic path still needs handling
    // if we did not commit/restore above (failed + !auto_rollback).
    if (outermost && had_panic_checkpoint_ && !panic_handled) {
        // Leave checkpoint alive for retry (pre-#241 / #2120 partial recovery).
        // Issue #2364: densify already audited above when outermost; if
        // densify was skipped (nested), re-arm is still owned by the live
        // checkpoint + existing GC defer arm from save_panic_checkpoint.
        (void)0;
    }
    // Issue #417 / #1766: verify stack/depth-slot consistency
    // after boundary exit (cross-TU drift detection).
    // ensure_mutation_invariants / ensure_hygiene_violation_detection
    // / probe_arena_auto_policy_on_boundary_exit are all noexcept
    // (Issue #1766) — they cannot throw past remaining dtor work
    // without std::terminate. try/catch is intentionally not used.
    ev_->ensure_mutation_invariants();
    // Issue #422: hygiene violation detection hook on
    // Guard exit (mutate paths record attempts at block).
    ev_->ensure_hygiene_violation_detection();
    // Issue #2674: layered evidence-coherence invariant on MutationBoundary
    // outermost exit. Compares g_dead_coercion_ast_elided_with_evidence_total
    // vs ir_narrow + meta_stamps; observe-only diverge counter under Soft/
    // Sampled (no hard-reject). Zero cost when no evidence path (pure atomic
    // loads + conditional atomic add when invariant holds). Coarse boundary
    // placement amortizes across multiple AST/IR elisions per mutate.
    //
    // IR narrow counter lives in opt_registry (optimization_passes module);
    // coercion_map.ixx can't import it (cyclic graph), so caller snapshots
    // it here and passes the value. Snapshot is fine because the invariant
    // only cares about monotonic divergence (ir_narrow + meta_stamps are
    // monotonically non-decreasing per process lifetime).
    const auto ir_narrow_evidence_hits_snapshot =
        ::aura::compiler::opt_registry::dead_coercion_ir_narrow_evidence_hits.load(
            std::memory_order_relaxed);
    aura::compiler::check_layered_evidence_coherence(ir_narrow_evidence_hits_snapshot);
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
        // Issue #2608 / #2641: optional OccurrenceGoal persist for cross-delta
        // / multi-session replay after steal/densify prune. Soft default
        // OFF (zero cost); production or AURA_OCCURRENCE_PERSIST=1 writes.
        // Production-default ON when env unset (#2641). Via C ABI so tests
        // can exercise the same path without dtor internals.
        {
            const auto mid = ev_->defuse_version_.load(std::memory_order_relaxed);
            aura_outermost_success_persist_occurrence(ev_, mid);
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
    // Issue #2222: drop fiber-local LinearEnforce Strict hold after
    // linear revalidate / exit probes so mid-boundary IR still saw
    // require_complete. Process Soft (if any) is restored effectively.
    if (linear_enforce_strict_pushed_) {
        aura::core::provenance::mutation_boundary_pop_linear_enforce_strict();
        linear_enforce_strict_pushed_ = false;
    }
    // Issue #2640: production Restricted periodic epoch-invariant soft walk.
    // Hook is internally rate-limited via steady_ms_now + period_ms, so even
    // called on every outermost success-exit the amortized cost is bounded
    // (mode != Soft / sandbox=off / disabled take a fast no-op skip path).
    if (outermost && success)
        aura_periodic_epoch_invariant_walk_if_due();
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
    , region_mode_(o.region_mode_)
    , region_shard_(o.region_shard_)
    , inert_(o.inert_)
    , enter_ts_(std::move(o.enter_ts_))
    , uncaught_at_enter_(o.uncaught_at_enter_)
    // Issue #2090: propagate defuse snapshot across move; the new owner
    // keeps the same dirty-detection horizon as the source guard.
    , defuse_version_at_enter_(o.defuse_version_at_enter_)
    , dirty_upward_at_enter_(o.dirty_upward_at_enter_)
    , render_fast_exit_(o.render_fast_exit_)
    , linear_enforce_strict_pushed_(o.linear_enforce_strict_pushed_)
    , ev_(o.ev_)
    , flag_(o.flag_)
    , lock_(std::move(o.lock_))
    , shared_lock_(std::move(o.shared_lock_))
    , region_lock_(std::move(o.region_lock_)) {
    o.had_panic_checkpoint_ = false;
    o.fine_rollback_ = false;
    o.atomic_batch_active_ = false;
    o.suppress_bump_ = false;
    o.is_outermost_ = false;
    o.region_mode_ = false;
    o.region_shard_ = 0;
    o.inert_ = false;
    o.enter_ts_.reset();
    o.defuse_version_at_enter_ = 0;
    o.dirty_upward_at_enter_ = 0;
    o.render_fast_exit_ = false;
    o.linear_enforce_strict_pushed_ = false; // ownership transferred; do not double-pop
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
        region_mode_ = o.region_mode_;
        region_shard_ = o.region_shard_;
        inert_ = o.inert_;
        enter_ts_ = std::move(o.enter_ts_);
        uncaught_at_enter_ = o.uncaught_at_enter_;
        defuse_version_at_enter_ = o.defuse_version_at_enter_;
        dirty_upward_at_enter_ = o.dirty_upward_at_enter_;
        render_fast_exit_ = o.render_fast_exit_;
        linear_enforce_strict_pushed_ = o.linear_enforce_strict_pushed_;
        ev_ = o.ev_;
        flag_ = o.flag_;
        lock_ = std::move(o.lock_);
        shared_lock_ = std::move(o.shared_lock_);
        region_lock_ = std::move(o.region_lock_);
        o.had_panic_checkpoint_ = false;
        o.fine_rollback_ = false;
        o.atomic_batch_active_ = false;
        o.suppress_bump_ = false;
        o.is_outermost_ = false;
        o.region_mode_ = false;
        o.region_shard_ = 0;
        o.inert_ = false;
        o.enter_ts_.reset();
        o.defuse_version_at_enter_ = 0;
        o.dirty_upward_at_enter_ = 0;
        o.render_fast_exit_ = false;
        o.linear_enforce_strict_pushed_ = false;
        o.uncaught_at_enter_ = 0;
        o.ev_ = nullptr;
        o.flag_ = nullptr;
    }
    return *this;
}

// ── Issue #2099: HygieneCheckpoint save / restore ─────────────────────
//
// Agent-visible primitives for what-if / self-evo rollback semantics.
// Captures the FlatAST metadata columns (marker_ / provenance_ /
// dirty_ / macro_dirty_ from #1893 snapshot_metadata_columns) plus a
// freshness token (defuse_version_, macro_introduced_count_, flat
// generation, fiber thread id). Restoring rolls back to pre-save
// state without tearing down the parent MutationBoundary's
// structural topology (children_ / parent_ / children_snapshot
// stay valid; only the hygiene-relevant metadata columns are
// reinstalled).
//
// AC contract:
// Issue #2162 / #2090: single-owner hot-update recovery sequence.
// throttle → reemit → epoch notify (always on dirty) → batch_deopt unmatched.
// Idempotent: same defuse_version after a successful run is a no-op (AC3).
void Evaluator::run_hot_update_recovery_if_needed(bool success,
                                                  std::uint64_t defuse_version_at_enter,
                                                  std::uint64_t dirty_upward_at_enter) noexcept {
    const std::uint64_t cur_defuse = defuse_version_.load(std::memory_order_acquire);
    const std::uint64_t dirty_calls =
        workspace_flat_ ? workspace_flat_->mark_dirty_upward_call_count() : 0;
    const bool dirty_defines = (cur_defuse != defuse_version_at_enter);
    // Boundary-scoped dirty marks (not lifetime): FlatAST counter is cumulative.
    const bool dirty_marks = (dirty_calls > dirty_upward_at_enter);
    const bool deferred_reemit = aura_hot_update_has_deferred_reemit() != 0;
    const bool dirty_or_env_restamp = dirty_defines || dirty_marks;
    const bool full_rollback_skip = (!success && panic_auto_rollback_);
    if ((!dirty_or_env_restamp && !deferred_reemit) || full_rollback_skip)
        return;
    // AC3: single-owner — cascade path may have already recovered for this defuse.
    if (hot_update_recovery_done_defuse_ == cur_defuse && !deferred_reemit)
        return;

    if (!aura_hot_update_reemit_provider_wired()) {
        aura_hot_update_notify_epoch_bump(aura_aot_func_table_epoch());
        hot_update_recovery_done_defuse_ = cur_defuse;
        return;
    }
    if (aura_hot_update_should_throttle_reemit()) {
        aura_hot_update_on_reemit_throttled();
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->boundary_reemit_throttled_total.fetch_add(1, std::memory_order_relaxed);
        // Epoch notify always on dirty (even when throttled) — AC1 step 3.
        aura_hot_update_notify_epoch_bump(aura_aot_func_table_epoch());
        hot_update_recovery_done_defuse_ = cur_defuse;
        return;
    }
    // Issue #2606: stamp reemit/register owner for multi-AotState filter.
    struct ReemitEvalOwnerGuard {
        void* prev_reemit;
        void* prev_reg;
        explicit ReemitEvalOwnerGuard(void* e) noexcept
            : prev_reemit(aura_aot_get_reemit_owner_eval())
            , prev_reg(aura_aot_get_register_owner_eval()) {
            aura_aot_set_reemit_owner_eval(e);
            aura_aot_set_register_owner_eval(e);
        }
        ~ReemitEvalOwnerGuard() noexcept {
            aura_aot_set_reemit_owner_eval(prev_reemit);
            aura_aot_set_register_owner_eval(prev_reg);
        }
        ReemitEvalOwnerGuard(const ReemitEvalOwnerGuard&) = delete;
        ReemitEvalOwnerGuard& operator=(const ReemitEvalOwnerGuard&) = delete;
    } owner_guard(static_cast<void*>(this));
    const std::size_t n_reemit = aura_reemit_aot_for_dirty(cur_defuse);
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
        m->boundary_reemit_success_total.fetch_add(static_cast<std::uint64_t>(n_reemit),
                                                   std::memory_order_relaxed);
    }
    // Issue #2210: sample JIT/Interpreter equivalence after reemit success.
    // Zero-cost when oracle disabled (enabled() is a single atomic load).
    // Healthy path compares identical fingerprints; inject forces mismatch.
    if (aura_jit_equivalence_enabled()) {
        const std::uint64_t bits = (static_cast<std::uint64_t>(n_reemit) << 3) | 1ull;
        const int ok = aura_check_primcall_equivalence(bits, bits);
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
            m->jit_equivalence_runs_total.store(aura_jit_equivalence_runs_v_read(),
                                                std::memory_order_relaxed);
            m->jit_equivalence_ok_total.store(aura_jit_equivalence_ok_v_read(),
                                              std::memory_order_relaxed);
            m->jit_equivalence_mismatch_total.store(aura_jit_equivalence_mismatch_v_read(),
                                                    std::memory_order_relaxed);
            m->jit_equivalence_deopt_force_total.store(aura_jit_equivalence_deopt_force_v_read(),
                                                       std::memory_order_relaxed);
            if (!ok)
                m->jit_equivalence_deopt_force_total.fetch_add(0, std::memory_order_relaxed);
        }
        (void)ok;
    }
    const auto epoch = aura_aot_func_table_epoch();
    aura_hot_update_notify_epoch_bump(epoch);
    // Issue #2162: unmatched closures stay on dual-epoch deopt after
    // remap (#2013); record the recovery step for Agents (AC2 metrics).
    // Named batch_deopt_for requires a define name; epoch notify above is
    // the global unmatched safety net.
    if (n_reemit > 0) {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->boundary_batch_deopt_unmatched_total.fetch_add(1, std::memory_order_relaxed);
    }
    (void)epoch;
    hot_update_recovery_done_defuse_ = cur_defuse;
}

//   AC1: depth-limit / hygiene-violation expand can call restore
//        → pre-expand state recovered; partial MacroIntroduced
//          nodes are un-marked.
//   AC2: nested under MutationBoundaryGuard → outer boundary's
//        structural checkpoint (children_snapshot etc.) untouched;
//        restore only mutates the metadata columns.
//   AC3: happy-path expand never calls save → zero overhead
//        (the optional<> slots stay default-constructed; the
//         std::vector is reserved but not allocated until first save).
//   AC4: cross-fiber restore is refused (saved_fiber_thread_id
//        mismatch bumps cross_fiber_reject_total + restore_fail_total);
//        concurrent stress contract holds.
//   AC5: (query:hygiene-checkpoint-stats) reports the 4 counters
//        + pending slot count for Agent dashboards.

Evaluator::HygieneCheckpoint Evaluator::save_hygiene_checkpoint() noexcept {
    HygieneCheckpoint cp;
    if (!workspace_flat_) {
        // No workspace: return invalid; primitive layer treats
        // invalid as a no-op (still bumps save counter so the
        // Agent sees "save attempted, no workspace").
        bump_hygiene_checkpoint_save_total();
        return cp;
    }
    cp.meta = workspace_flat_->snapshot_metadata_columns();
    cp.saved_defuse_version = defuse_version_.load(std::memory_order_acquire);
    cp.saved_macro_introduced_count = 0;
    for (aura::ast::NodeId id = 0; id < workspace_flat_->size(); ++id) {
        if (workspace_flat_->is_macro_introduced(id))
            ++cp.saved_macro_introduced_count;
    }
    cp.saved_flat_generation = workspace_flat_->generation();
    cp.saved_fiber_thread_id =
        static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    cp.valid = true;
    bump_hygiene_checkpoint_save_total();
    // Issue #2717: stamp TypeLinearCommitProof on boundary exit
    // (covers render-fast-exit success, composite ok/reject,
    // non-composite ok/reject — all paths that fall through
    // to here). The linear-synth-hard-fail early-return above
    // has its own stamp.
    (void)typed_audit::build_type_linear_commit_proof_from_live(cp.version);
    return cp;
}

bool Evaluator::restore_hygiene_checkpoint(const HygieneCheckpoint& cp) noexcept {
    if (!cp.valid) {
        bump_hygiene_checkpoint_restore_fail_total();
        return false;
    }
    if (!workspace_flat_) {
        bump_hygiene_checkpoint_restore_fail_total();
        return false;
    }
    // AC4: refuse cross-fiber restore. Saved thread id was hashed
    // std::thread::id at save time; recompute now and compare.
    const std::uint64_t cur_thread_id =
        static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    if (cur_thread_id != cp.saved_fiber_thread_id) {
        bump_hygiene_checkpoint_cross_fiber_reject_total();
        bump_hygiene_checkpoint_restore_fail_total();
        return false;
    }
    // Generation drift = workspace was compacted / recycled since
    // save; restoring metadata columns onto a different generation
    // would corrupt parent_/children_ topology invariants (PCV
    // share_ptrs in any pre-existing MutationCheckpoint point at
    // the OLD generation). Refuse rather than partially restoring.
    if (workspace_flat_->generation() != cp.saved_flat_generation) {
        bump_hygiene_checkpoint_restore_fail_total();
        return false;
    }
    // AC1/AC2: restore only the metadata columns. Structural
    // topology (children_ / parent_ / atomic_batch_meta_snap_) is
    // left alone — the parent MutationBoundary still owns it.
    workspace_flat_->restore_metadata_columns(aura::ast::FlatAST::MetadataColumnsSnapshot{
        std::pmr::vector<aura::ast::SyntaxMarker>(cp.meta.marker.begin(), cp.meta.marker.end()),
        std::pmr::vector<std::uint32_t>(cp.meta.provenance.begin(), cp.meta.provenance.end()),
        std::pmr::vector<std::uint8_t>(cp.meta.dirty.begin(), cp.meta.dirty.end()),
        std::pmr::vector<std::uint8_t>(cp.meta.macro_dirty.begin(), cp.meta.macro_dirty.end())});
    // Bump defuse_version_ once so any reader holding a snapshot
    // across the restore sees a version mismatch and re-reads.
    defuse_version_.fetch_add(1, std::memory_order_release);
    bump_hygiene_checkpoint_restore_success_total();
    return true;
}

std::uint64_t Evaluator::save_hygiene_checkpoint_handle() noexcept {
    auto cp = save_hygiene_checkpoint();
    if (!cp.valid)
        return 0; // 0 = "no workspace" — caller treats as no-op
    // Reserve slot 0 as a sentinel "invalid"; real ids start at 1.
    if (hygiene_checkpoints_.empty())
        hygiene_checkpoints_.resize(16, std::nullopt);
    std::size_t slot = 0;
    for (std::size_t i = 0; i < hygiene_checkpoints_.size(); ++i) {
        if (!hygiene_checkpoints_[i].has_value()) {
            slot = i;
            break;
        }
    }
    if (slot == 0) {
        // All slots used; reclaim the lowest-id valid one (oldest
        // checkpoint is least likely to be restored by an Agent
        // mid-session — typical pattern is save → restore within
        // a single expand-attempt block).
        std::size_t victim = 0;
        std::uint64_t best_age = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t i = 0; i < hygiene_checkpoints_.size(); ++i) {
            if (hygiene_checkpoints_[i].has_value()) {
                // Approximate age by saved_defuse_version (older = smaller).
                const auto v = hygiene_checkpoints_[i]->saved_defuse_version;
                if (v < best_age) {
                    best_age = v;
                    victim = i;
                }
            }
        }
        hygiene_checkpoints_[victim].reset();
        slot = victim;
    }
    hygiene_checkpoints_[slot] = std::move(cp);
    const std::uint64_t id = static_cast<std::uint64_t>(slot) + 1;
    next_hygiene_checkpoint_id_.store(id + 1, std::memory_order_relaxed);
    return id;
}

bool Evaluator::restore_hygiene_checkpoint_handle(std::uint64_t handle) noexcept {
    if (handle == 0) {
        bump_hygiene_checkpoint_restore_fail_total();
        return false;
    }
    const std::size_t slot = static_cast<std::size_t>(handle) - 1;
    if (slot >= hygiene_checkpoints_.size() || !hygiene_checkpoints_[slot].has_value()) {
        bump_hygiene_checkpoint_restore_fail_total();
        return false;
    }
    HygieneCheckpoint cp = std::move(*hygiene_checkpoints_[slot]);
    hygiene_checkpoints_[slot].reset();
    return restore_hygiene_checkpoint(cp);
}

std::size_t Evaluator::hygiene_checkpoint_pending_count() const noexcept {
    std::size_t n = 0;
    for (const auto& slot : hygiene_checkpoints_) {
        if (slot.has_value())
            ++n;
    }
    return n;
}

void Evaluator::clear_hygiene_checkpoints() noexcept {
    for (auto& slot : hygiene_checkpoints_)
        slot.reset();
    next_hygiene_checkpoint_id_.store(1, std::memory_order_relaxed);
}

void Evaluator::bump_hygiene_checkpoint_save_total() const noexcept {
    if (compiler_metrics_) {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        m->hygiene_checkpoint_save_total.fetch_add(1, std::memory_order_relaxed);
    }
}
void Evaluator::bump_hygiene_checkpoint_restore_success_total() const noexcept {
    if (compiler_metrics_) {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        m->hygiene_checkpoint_restore_success_total.fetch_add(1, std::memory_order_relaxed);
    }
}
void Evaluator::bump_hygiene_checkpoint_restore_fail_total() const noexcept {
    if (compiler_metrics_) {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        m->hygiene_checkpoint_restore_fail_total.fetch_add(1, std::memory_order_relaxed);
    }
}
void Evaluator::bump_hygiene_checkpoint_cross_fiber_reject_total() const noexcept {
    if (compiler_metrics_) {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        m->hygiene_checkpoint_cross_fiber_reject_total.fetch_add(1, std::memory_order_relaxed);
    }
}

std::uint64_t Evaluator::get_hygiene_checkpoint_save_total() const noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    return m ? m->hygiene_checkpoint_save_total.load(std::memory_order_relaxed) : 0;
}
std::uint64_t Evaluator::get_hygiene_checkpoint_restore_success_total() const noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    return m ? m->hygiene_checkpoint_restore_success_total.load(std::memory_order_relaxed) : 0;
}
std::uint64_t Evaluator::get_hygiene_checkpoint_restore_fail_total() const noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    return m ? m->hygiene_checkpoint_restore_fail_total.load(std::memory_order_relaxed) : 0;
}
std::uint64_t Evaluator::get_hygiene_checkpoint_cross_fiber_reject_total() const noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    return m ? m->hygiene_checkpoint_cross_fiber_reject_total.load(std::memory_order_relaxed) : 0;
}

// ── Issue #2170: LayoutStamp / unified generation truth-source API ────
//
// Single source of truth for all cross-subsystem epoch fields. Each
// field keeps its existing storage; current_layout_stamp() composes
// a snapshot, publish_layout_stamp() bumps the publisher counter +
// writes the last-stamp fields, and the result is read back via
// (query:stable-ref-stats-hash) layout-stamp-* keys.
//
// Memory ordering: env_generation_ + defuse_version_ use acquire
// (matches their existing load semantics in ensure_mutation_invariants
// + exit_mutation_boundary). ArenaGroup::primary_arena_id_and_gen
// uses a shared_lock internally (the existing per-arena atomics are
// acq_rel on bump). The captured mutation_epoch is acquire from the
// process-global atomic — matches current_mutation_epoch().

aura::core::LayoutStamp Evaluator::current_layout_stamp() const noexcept {
    std::uint64_t arena_id = 0;
    std::uint64_t arena_gen = 0;
    if (arena_group_)
        arena_group_->primary_arena_id_and_gen(arena_id, arena_gen);
    const auto flat_gen =
        workspace_flat_ ? static_cast<std::uint16_t>(workspace_flat_->generation()) : 0;
    // env_generation_ is a plain uint64_t (not atomic) — bumps happen
    // under workspace_mtx_ so plain reads are race-free (Issue #759
    // SOAK contract).
    const auto env_gen = env_generation_;
    const auto dver = defuse_version_.load(std::memory_order_acquire);
    // Direct ctor (not LayoutStamp::capture) — capture() was moved out
    // of layout_stamp.hh to avoid workspace_epoch.hh include chain
    // redefinition in other TUs (see layout_stamp.hh preamble note).
    // mutation_epoch is filled here via the directly-included
    // workspace_epoch.hh acquire-load.
    // Issue #2255: ShapeProfiler monotonic generation as a first-
    // class field of LayoutStamp (7th field). Read the file-scope
    // bump counter so all ShapeProfiler instances share a single
    // monotonic source of truth for the resume fence.
    const auto sver = aura::compiler::shape::current_global_shape_version();
    // Issue #2432: IR SoA generation fence (8th field) — closes silent-
    // stale specialized IR under compact×mutate×fiber resume.
    const auto ir_gen = aura::compiler::current_ir_soa_generation_fence();
    return aura::core::LayoutStamp(arena_id, arena_gen, flat_gen,
                                   aura::core::current_mutation_epoch(), env_gen, dver, sver,
                                   ir_gen);
}

aura::core::LayoutStamp Evaluator::publish_layout_stamp() noexcept {
    const auto stamp = current_layout_stamp();
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
        m->layout_stamp_publish_total.fetch_add(1, std::memory_order_relaxed);
        m->layout_stamp_last_arena_gen.store(stamp.arena_gen, std::memory_order_relaxed);
        m->layout_stamp_last_flat_gen.store(static_cast<std::uint64_t>(stamp.flat_gen),
                                            std::memory_order_relaxed);
    }
    // Issue #2251: refresh env_gen_stamp_ on existing env_frames_ so
    // post-publish lookups / walks see fresh frames. Frames allocated
    // AFTER this call get the new stamp via alloc_env_frame (2a wire-up).
    // Note: env_generation_ bumps happen under workspace_mtx_ (per
    // #759 SOAK contract) so the env_frames_ mutation is race-free.
    for (auto& fr : env_frames_) {
        if (fr.env_gen_stamp_ != 0)
            fr.env_gen_stamp_ = stamp.env_gen;
    }
    return stamp;
}

void Evaluator::bump_layout_stamp_publish_total() const noexcept {
    if (compiler_metrics_) {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        m->layout_stamp_publish_total.fetch_add(1, std::memory_order_relaxed);
    }
}
std::uint64_t Evaluator::get_layout_stamp_last_arena_gen() const noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    return m ? m->layout_stamp_last_arena_gen.load(std::memory_order_relaxed) : 0;
}
std::uint64_t Evaluator::get_layout_stamp_last_flat_gen() const noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    return m ? m->layout_stamp_last_flat_gen.load(std::memory_order_relaxed) : 0;
}
std::uint64_t Evaluator::get_layout_stamp_publish_total() const noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    return m ? m->layout_stamp_publish_total.load(std::memory_order_relaxed) : 0;
}

// Issue #2250: LayoutStamp fence on Fiber resume/steal. Bumped by
// evaluator_fiber_mutation.cpp when fiber-stored stamp vs
// current_layout_stamp() mismatches any of the 6 fields. Read by
// query:stable-ref-stats primitive.
std::uint64_t Evaluator::get_layout_stamp_resume_mismatch_total() const noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    return m ? m->layout_stamp_resume_mismatch_total.load(std::memory_order_relaxed) : 0;
}

// Issue #2351: steal-complete LayoutStamp dual-check counters.
std::uint64_t Evaluator::get_layout_stamp_steal_mismatch_total() const noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    return m ? m->layout_stamp_steal_mismatch_total.load(std::memory_order_relaxed) : 0;
}
std::uint64_t Evaluator::get_layout_stamp_steal_missing_total() const noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    return m ? m->layout_stamp_steal_missing_total.load(std::memory_order_relaxed) : 0;
}
std::uint64_t Evaluator::get_steal_complete_restamp_total() const noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    return m ? m->steal_complete_restamp_total.load(std::memory_order_relaxed) : 0;
}
std::uint64_t Evaluator::get_steal_complete_layout_hard_fail_total() const noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    return m ? m->steal_complete_layout_hard_fail_total.load(std::memory_order_relaxed) : 0;
}

// Issue #2255: ShapeProfiler monotonic generation (7th LayoutStamp
// field) hard-fence counter. Bumped by
// evaluator_fiber_mutation.cpp when fiber->resume_shape_version() !=
// current layout stamp's shape_version field.
std::uint64_t Evaluator::get_shape_version_fence_reject_total() const noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    return m ? m->shape_version_fence_reject_total.load(std::memory_order_relaxed) : 0;
}

// Issue #2432: IR SoA generation fence (8th LayoutStamp field).
std::uint64_t Evaluator::get_ir_generation_fence_hit_total() const noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    return m ? m->ir_generation_fence_hit_total.load(std::memory_order_relaxed) : 0;
}

// ── Issue #2555: TransactionGuardHost factories ──────────────────────────
// Type-erased try_acquire/release so core TransactionGuard never imports
// Evaluator. Handle is a heap-allocated MutationBoundaryGuard; release
// deletes it (dtor commits or restores panic via *success_flag).
namespace {

    // Region key for transaction_guard_host_for_region — set by factory
    // immediately before TransactionGuard ctor invokes try_acquire.
    thread_local std::uint64_t g_tg_region_key = 0;

    void* transaction_guard_try_acquire(void* ctx, std::uint64_t pending,
                                        bool* success_flag) noexcept {
        auto* ev = static_cast<Evaluator*>(ctx);
        if (!ev)
            return nullptr;
        auto g = Evaluator::MutationBoundaryGuard::try_acquire(*ev, pending, success_flag);
        if (!g)
            return nullptr;
        // unique_ptr → raw; TransactionGuard::release deletes.
        return (*g).release();
    }

    void* transaction_guard_try_acquire_region(void* ctx, std::uint64_t pending,
                                               bool* success_flag) noexcept {
        auto* ev = static_cast<Evaluator*>(ctx);
        if (!ev)
            return nullptr;
        auto g = Evaluator::MutationBoundaryGuard::try_acquire_for_region(*ev, g_tg_region_key,
                                                                          pending, success_flag);
        if (!g)
            return nullptr;
        return (*g).release();
    }

    void transaction_guard_release(void* /*ctx*/, void* handle) noexcept {
        delete static_cast<Evaluator::MutationBoundaryGuard*>(handle);
    }

} // namespace

aura::core::TransactionGuardHost Evaluator::transaction_guard_host(Evaluator& ev) noexcept {
    return aura::core::TransactionGuardHost{
        &ev,
        &ev, // expected_evaluator_id
        &transaction_guard_try_acquire,
        &transaction_guard_release,
        /*save=*/nullptr,
        /*restore=*/nullptr,
        /*clear=*/nullptr,
        /*host_owns_panic_checkpoint=*/true, // MBG outermost saves panic
    };
}

aura::core::TransactionGuardHost
Evaluator::transaction_guard_host_for_region(Evaluator& ev, std::uint64_t region_key) noexcept {
    g_tg_region_key = region_key;
    return aura::core::TransactionGuardHost{
        &ev,
        &ev,
        &transaction_guard_try_acquire_region,
        &transaction_guard_release,
        /*save=*/nullptr,
        /*restore=*/nullptr,
        /*clear=*/nullptr,
        /*host_owns_panic_checkpoint=*/true,
    };
}

} // namespace aura::compiler
