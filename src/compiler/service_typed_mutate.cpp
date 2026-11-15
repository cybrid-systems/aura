// service_typed_mutate.cpp - CompilerService::typed_mutate moved out of the
// module interface (#5242). Move-only: the body is unchanged.

module;

#include "compiler/typed_mutation_audit.h" // typed_audit::
#include "core/workspace_epoch.hh"         // current_mutation_epoch
#include "lock_order_audit.h"              // lock_order
#include "per_defuse_index.h"              // PerDefUseIndexTracker

module aura.compiler.service;

import std;

namespace aura::compiler {

[[nodiscard]] CompilerService::MutationResult
CompilerService::typed_mutate(std::string_view sexpr) {
    if (!current_ast_ || !current_pool_) {
        return {0, false, "no AST loaded"};
    }
    // Issue #1523: acquire mutate_mtx_ FIRST before MutationBoundaryGuard
    // (workspace) so mark_define_dirty can take dep_graph without inversion:
    //   mutate → workspace → (optional env) → dep_graph
    using aura::compiler::lock_order::Level;
    using aura::compiler::lock_order::OrderedUniqueLock;
    OrderedUniqueLock<std::shared_mutex> mutate_guard =
        OrderedUniqueLock<std::shared_mutex>::acquire_if_needed(mutate_mtx_, Level::Mutate);
    sync_lock_order_metrics_();

    // Wrap in a transaction: if evaluation fails (parse or runtime),
    // all mutations performed by the sexpr are automatically rolled back.
    MutationTransaction tx(*current_ast_);
    // Issue #4369: do not parse into current_pool_. After a child
    // workspace is deleted, that pool can be the freed COW clone
    // (or a densify-moved service pool). The sexpr is only the
    // mutate call; the primitive writes workspace_flat_.
    aura::ast::FlatAST scratch_flat;
    aura::ast::StringPool scratch_pool;
    auto pr = aura::parser::parse_to_flat(sexpr, scratch_flat, scratch_pool);
    if (!pr.success || pr.root == aura::ast::NULL_NODE) {
        auto diag = parse_error_diag(pr);
        return {0, false, diag.format()};
    }

    // Issue #184 Cycle 1: wrap the mutation effect in a
    // MutationBoundaryGuard (RAII). The guard acquires the
    // exclusive workspace_mtx_ write lock + bumps
    // defuse_version_ (acquire load + release store) on
    // construction, pops the checkpoint + releases the lock
    // on destruction. This composes with the existing
    // MutationTransaction (the guard handles lock/version;
    // the transaction handles AST-level rollback on failure).
    //
    // success_flag is set by this scope and read by the
    // guard's destructor. We default to true; the only
    // path that sets it false is below (when the type error
    // check fails — even though the tx auto-rolls back,
    // marking the guard as failure makes the intent
    // explicit and prepares for the future rollback path).
    bool boundary_success = true;
    // Issue #1547: typed try_acquire (check_mutation_quota → ResourceQuotaExceeded).
    auto guard_r = aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(
        evaluator_, /*pending_count=*/1, &boundary_success);
    if (!guard_r) {
        return {0, false, guard_r.error().message};
    }
    auto guard = std::move(*guard_r);

    // eval_flat(Define) set_pool's the caller onto scratch_pool. That
    // pool dies with this function. The next typed_mutate then interns
    // the primitive name into the freed pool (ASAN) or collides with a
    // user SymId and reports "cannot call: mutate:rebind" (UBSAN).
    // Re-key back while scratch_pool is still alive. Null any EnvFrame
    // that still names it (#4352 skips the SymId scan when pool_ is null).
    struct DetachScratchPool {
        Evaluator& ev;
        const aura::ast::StringPool* scratch;
        const aura::ast::StringPool* saved_top;
        DetachScratchPool(Evaluator& e, const aura::ast::StringPool* s)
            : ev(e)
            , scratch(s)
            , saved_top(e.top_env().pool()) {}
        DetachScratchPool(const DetachScratchPool&) = delete;
        DetachScratchPool& operator=(const DetachScratchPool&) = delete;
        DetachScratchPool(DetachScratchPool&&) = delete;
        DetachScratchPool& operator=(DetachScratchPool&&) = delete;
        ~DetachScratchPool() {
            if (!scratch)
                return;
            if (ev.top_env().pool() == scratch) {
                if (saved_top && saved_top != scratch)
                    ev.top_env().set_pool(saved_top);
                else
                    ev.top_env().set_pool(nullptr);
            }
            std::unique_lock<std::shared_mutex> wlock[Evaluator::kEnvFramesShardCount];
            for (std::size_t i = 0; i < Evaluator::kEnvFramesShardCount; ++i)
                wlock[i] = std::unique_lock<std::shared_mutex>(ev.env_frame_shard_mu(i));
            for (auto& fr : ev.env_frames_) {
                if (fr.pool_ == scratch)
                    fr.pool_ = nullptr;
            }
        }
    } detach_scratch(evaluator_, &scratch_pool);

    auto result = evaluator_.eval_flat(scratch_flat, scratch_pool, pr.root, evaluator_.top_env());
    if (!result) {
        // Issue #213 follow-up: the eval failed (e.g. parse
        // error in the mutate code itself). Mark the
        // boundary as failed so the guard's destructor
        // triggers the rollback path (Cycle 1: rolls back
        // the MutationRecord log; Cycle 2: bumps version +
        // invalidates defuse_index_). The sub-primitives
        // that ran during eval_flat may have appended
        // records that we want to undo.
        boundary_success = false;
        return {0, false, result.error().message};
    }
    auto& val = *result;

    // Check if the mutation's auto-typecheck reported errors (P2 #34)
    if (evaluator_.has_type_error()) {
        auto err = evaluator_.last_mutate_error();
        evaluator_.clear_last_mutate_error();
        // Transaction auto-rollbacks on destructor
        // Issue #184: signal the boundary guard that this is a
        // rollback intent (today both paths commit; explicit
        // flag sets up the future rollback implementation).
        boundary_success = false;
        return {0, false, err};
    }

    // Mutation primitives return make_bool(true) on success or an int mutation ID.
    // Accept both: bool true = success, positive int = success with mutation ID.
    bool is_success = false;
    std::uint64_t mid = 0;
    if (aura::compiler::types::is_int(val)) {
        mid = static_cast<std::uint64_t>(aura::compiler::types::as_int(val));
        is_success = mid > 0;
    } else if (aura::compiler::types::is_bool(val)) {
        is_success = aura::compiler::types::as_bool(val);
        mid = 0; // no explicit mutation ID for bool returns
    }

    if (is_success) {
        // Phase 4 (#53): invalidate shape profiles on mutation
        // to trigger deoptimization on next eval.
        // Conservative: reset all profiles since we don't know which
        // functions were affected.
        // Issue #570: per-profile version++ + deopt hook
        // (preserves history keys vs full reset).
        shape_profiler_.invalidate_all();

        tx.commit();

        // Issue #1407 R1 / #1524: dual-epoch + bridge stamp + JIT
        // batch_deopt via the unified helper. NEVER bare
        // bump_bridge_epoch() here — that breaks the #1475 dual-epoch
        // contract (bridge bumped but AOT table / stamps lag).
        // Empty name stamps all known bridges (affected set may be
        // unknown for generic mutate:* forms; mark_define_dirty also
        // routes through the same helper for named defines).
        //
        // INVARIANT (#1524): all typed_mutate / typed_mutate_atomic
        // invalidation goes through atomic_bump_epochs_and_stamp_bridge.
        atomic_bump_epochs_and_stamp_bridge(/*name=*/{});
        metrics_.typed_mutate_epoch_bumps.fetch_add(1, std::memory_order_relaxed);
        metrics_.typed_mutate_atomic_invalidations_total.fetch_add(1, std::memory_order_relaxed);

        // Issue #147: post-mutation invariant check. Runs only
        // if invariant_check_mode_ is not Disabled. Iterates
        // every MutationRecord added during this transaction
        // and runs post_mutation_invariant_check on each.
        //
        // Important: mutation primitives add their records to
        // workspace_flat_ (not current_ast_), and the workspace
        // pointer can be replaced by lazy COW during eval. We
        // therefore capture a fresh pointer to the workspace
        // AFTER the eval completes (and AFTER tx.commit, so
        // the post-commit state is final). The snapshot id is
        // the workspace's mutation_count() snapshot we took
        // before the eval started.
        //
        // Mode behavior:
        //   Disabled     — skip entirely, status stays NotChecked.
        //   WarningsOnly — surface diagnostics, do NOT block
        //                  (success stays true).
        //   Strict       — any per-record Warnings status
        //                  promotes to MutationResult success=
        //                  false with the first diagnostic's
        //                  message promoted to .error.
        MutationResult res;
        res.mutation_id = mid;
        res.success = true;

        if (invariant_check_mode_ == InvariantCheckMode::Disabled) {
            res.invariant_status = aura::ast::InvariantStatus::NotChecked;
            // Issue #1383: throttled warn when Disabled is
            // set on a workspace with prior typed mutations.
            // Throttled to ONCE per mode flip (operator can
            // see the silent-bypass risk without per-mutation
            // spam). WarningsOnly/Strict modes never warn
            // because they actually run the check.
            if (auto* ws = evaluator_.workspace_flat();
                ws && ws->mutation_count() > 0 && mode_flip_count_ != last_disabled_warn_flip_) {
                mode_warnings_.push_back(
                    "invariant checks disabled on workspace with " +
                    std::to_string(ws->mutation_count()) +
                    " typed mutations \u2014 linear ownership narrowing may be stale");
                last_disabled_warn_flip_ = mode_flip_count_;
            }
            return res;
        }

        // Capture the post-eval workspace. Note: lazy COW may
        // have replaced workspace_flat_ with a new pointer; we
        // look it up via the evaluator.
        auto* ws_flat = evaluator_.workspace_flat();
        if (!ws_flat) {
            // No workspace to check against — treat as Ok.
            res.invariant_status = aura::ast::InvariantStatus::Ok;
            return res;
        }

        // Issue #152 Phase 2: Mutation Impact Analysis. The
        // typed_mutate call may have changed a function's
        // value; any Define whose subtree references that
        // function (directly) is potentially affected. The
        // affected set is computed via the
        // defines_referencing_sym helper from #150 Phase 3
        // (commit 02a1c75). The affected set is logged
        // (observability hook for the agent orchestration
        // layer — "what would this mutation affect?") and
        // could be used by the AOT cache invalidation
        // (cross-link to #151 Phase 3) to invalidate only
        // the affected entries, not the global cache.
        //
        // Today the affected set is logged but not
        // consumed for invalidation (the AOT cache
        // invalidation path is a follow-up). This is the
        // read-side: the affected Define node IDs are
        // computed and emitted to stderr for observability.
        if (mid > 0) {
            const auto& mut_log = ws_flat->all_mutations();
            for (auto it = mut_log.rbegin(); it != mut_log.rend(); ++it) {
                if (it->mutation_id != mid)
                    continue;
                aura::ast::NodeId target = it->target_node;
                if (target == aura::ast::NULL_NODE)
                    target = it->parent_id;
                if (target != aura::ast::NULL_NODE && target < ws_flat->size()) {
                    auto v = ws_flat->get(target);
                    if (v.tag == aura::ast::NodeTag::Define && v.sym_id != aura::ast::INVALID_SYM) {
                        // Issue #2448: exclude only the mutated Define
                        // NodeId (not all Defines with matching name).
                        auto affected = ws_flat->defines_referencing_sym(v.sym_id, target);
                        std::println(std::cerr,
                                     "MutationImpactAnalysis: mutation {} affects {} function(s)",
                                     mid, affected.size());
                    }
                }
                break;
            }
        }
        // The snapshot id is the count of mutations on the
        // workspace BEFORE the typed_mutate's eval started.
        // We don't have a direct way to capture that here
        // (the transaction is on current_ast_, not the
        // workspace), so we use a heuristic: the typed_mutate
        // either adds zero or one mutation per call (most
        // primitives append one record). We treat the latest
        // entry whose status is still NotChecked as a
        // candidate for this transaction. This is a soft
        // contract — it works because earlier entries already
        // got their status set by prior typed_mutate calls.
        // Issue #274: fold invariant checks via MutationVisitor pipeline.
        // Issue #1415: when suppress_invariant_check_ is true (set by
        // typed_mutate_atomic during a batch), skip the per-mutation
        // check here. A single combined check on the dirty union
        // runs at typed_mutate_atomic's commit, giving the same
        // final status with 1/N the work for an N-mutation batch.
        if (!suppress_invariant_check_) {
            // Issue #147 / #1458: type-checker half (OwnershipEnv).
            // Issue #2286: pass current_cache_epoch so the
            // OwnershipEscapeSummary publish key matches the
            // CompilerService thread-local lookup before lower_to_ir.
            aura::compiler::PostMutationInvariantVisitor invariant_visitor(
                *current_pool_, type_registry_, &metrics_, evaluator_.current_cache_epoch());
            aura::ast::run_mutation_pipeline(*ws_flat, invariant_visitor);
            invariant_visitor.apply_status_updates(*ws_flat);
            res.invariant_status = invariant_visitor.worst_status();
            res.invariant_diagnostics =
                std::vector<aura::compiler::OwnershipNote>(invariant_visitor.notes());
            // Issue #1538 / #1478: runtime half (EnvFrame linear enforce).
            // Always paired with the invariant visitor so both halves
            // of the post-mutation linear pipeline run together.
            apply_linear_post_mutate_pipeline_(res, mid);
        }
        auto& log = ws_flat->all_mutations();

        // Issue #165 Phase 1B: post-mutation macro re-expansion.
        // For every MutationRecord we just processed, walk
        // its affected subtree and re-expand any macro call
        // sites. This fixes the bug where EDSL mutations
        // (mutate :rebind, mutate:set-body) leave stale macro
        // expansions — the macro's gensym'd bindings may not
        // be re-generated, and the call site may pick up
        // caller's bindings that should have been hygiene-
        // isolated. Mirrors the post_mutation_invariant_check
        // pattern above (Issue #147).
        //
        // The function is safe on any mutation record — it
        // bails on malformed input (NULL_NODE, out-of-range,
        // empty macros_ registry). The re-expanded count is
        // logged for observability.
        if (mid > 0) {
            for (auto& rec : log) {
                if (rec.mutation_id != mid)
                    continue;
                auto re_expanded =
                    evaluator_.post_mutation_macro_reexpand(*ws_flat, *current_pool_, rec);
                if (re_expanded > 0) {
                    std::println(std::cerr,
                                 "MacroReexpand: mutation {} re-expanded {} call site(s)", mid,
                                 re_expanded);
                }
                break; // one record per typed_mutate
            }
        }

        // Issue #411: post-mutation auto-incremental typecheck.
        // After a successful mutation, automatically invoke
        // TypeChecker::infer_flat_partial on the affected
        // subtree (the same path the manual
        // (typecheck-incremental) Aura primitive uses). The
        // result populates flat.type_id_ for the affected
        // nodes, so the next (query:type <name>) /
        // (get-inferred-type <node-id>) returns up-to-date
        // results with no manual (typecheck-incremental) call.
        //
        // Mode handling:
        //   Eager     (default) — invoke on every successful
        //                          typed_mutate. Bumps
        //                          auto_invocations_total +
        //                          re_inferred_total.
        //   Lazy                 — skip the auto-invoke. The
        //                          caller can call
        //                          (typecheck-incremental)
        //                          later in the batch.
        //   Disabled             — skip entirely. Equivalent
        //                          to pre-#411 behavior. The
        //                          full (typecheck-current)
        //                          primitive still works.
        //
        // We use *ws_flat / *ws_pool (not *current_ast_ /
        // *current_pool_) because lazy COW may have replaced
        // the workspace flat during the mutation eval — the
        // mutation record's target_node is in the workspace's
        // NodeId space, not the typed_mutate AST's. The
        // (typecheck-incremental) primitive already follows
        // this same pattern (see evaluator_primitives_eval.cpp
        // ~L916).
        if (incremental_typecheck_mode_ == IncrementalTypecheckMode::Eager && mid > 0) {
            // Find the most recent record for this mutation
            // (mirrors the MacroReexpand pattern above). If
            // the workspace has no records (e.g. mid > 0
            // was set but no log entry was appended — a
            // degenerate case where the mutation returned a
            // non-zero success code without touching the
            // log), skip the auto-invoke (no rec to re-
            // infer against).
            bool invoked = false;
            for (auto& rec : log) {
                if (rec.mutation_id != mid)
                    continue;
                // Issue #612: re-sync ADT ctor registry from
                // DefineType nodes before partial re-infer so
                // match exhaustiveness sees the post-mutate ctor
                // list (not a stale pre-mutation snapshot).
                if (ws_flat->root != aura::ast::NULL_NODE)
                    register_adt_from_define_types(*ws_flat, *current_pool_, ws_flat->root);
                auto_invoke_incremental_typecheck_for(*ws_flat, *current_pool_, rec, mid,
                                                      "typed_mutate");
                invoked = true;
                break; // one record per typed_mutate
            }
            (void)invoked; // invoked is informational; no
                           // failure path needed when the
                           // log is empty (the macro re-
                           // expand above would have caught
                           // the same condition).
        }

        if (invariant_check_mode_ == InvariantCheckMode::Strict &&
            res.invariant_status == aura::ast::InvariantStatus::Warnings) {
            res.success = false;
            if (!res.invariant_diagnostics.empty()) {
                res.error = res.invariant_diagnostics.front().message;
            } else {
                res.error = "post-mutation invariant check reported violations";
            }
        }
        return res;
    }
    // If mutation returned 0/false, it indicates failure — transaction auto-rollbacks.
    // Issue #213 follow-up: mark the boundary as failed so
    // the guard's destructor triggers the rollback path.
    // The sub-primitives that ran during eval_flat may have
    // appended records to the log; we want to undo them.
    boundary_success = false;
    return {0, false, "mutation returned zero (failed)"};
}

} // namespace aura::compiler
