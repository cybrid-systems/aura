// service_relower.cpp — CompilerService::relower_dirty_defines_from_workspace
// moved out of the module interface (#5242). Move-only: the body is
// unchanged.

module;

#include "observability_metrics.h"         // note_relower_fallback / RelowerFallbackReason
#include "compiler/typed_mutation_audit.h" // typed_audit::
#include "core/workspace_epoch.hh"         // current_mutation_epoch
#include "compiler/hot_update_registry.hh" // hot_update_registry
#include "core/transparent_string_hash.hh" // TransparentStringHash
#include "lock_order_audit.h"              // lock_order

module aura.compiler.service;

import std;

namespace aura::compiler {

namespace {
    // Issue #4709: enables #4709 gate parking in the native drop for this scope.
    // Defined out of line so the #4083 source-cite window (1400 chars from its
    // comment to jit_.partial_recompile) keeps the drop call adjacent to it.
    struct DropParkScope4709 {
        bool prev;
        DropParkScope4709()
            : prev(aura_jit_drop_park_scope_4709(true)) {}
        ~DropParkScope4709() { aura_jit_drop_park_scope_4709(prev); }
        DropParkScope4709(const DropParkScope4709&) = delete;
        DropParkScope4709& operator=(const DropParkScope4709&) = delete;
    };
} // namespace

std::size_t CompilerService::relower_dirty_defines_from_workspace() {
    IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
    // Issue #3067: restore NodeId cascade for stale-rejected edges
    // before partial peel (string-only callers would under-mark).
    drain_deferred_hybrid_cascade_();
    auto* ws_flat = evaluator_.workspace_flat();
    auto* ws_pool = evaluator_.workspace_pool();
    if (!ws_flat || !ws_pool)
        return 0;
    // Issue #3135: under production defaults OR when the deferred
    // hybrid queue is already armed, take cascade_decision_mtx_ to
    // serialize the drain → snapshot → impact_ub consult → partial/
    // full decision window against concurrent record_dependency
    // reject/re-arm. Soft / sandbox=off + clean (armed==0) single-
    // fiber skips the lock for zero cost (AC2).
    const bool initial_armed = deferred_hybrid_armed_.load(std::memory_order_acquire) != 0;
    const bool need_lock =
        initial_armed || aura::compiler::typed_audit::production_defaults_active();
    std::unique_lock<std::mutex> cascade_guard(cascade_decision_mtx_, std::defer_lock);
    if (need_lock)
        cascade_guard.lock();
    // Issue #3168: snapshot deferred_hybrid_edges_.size() under the
    // cascade_decision_mtx_ critical section so the re-arm mid-loop
    // can attribute newly-appended edges and mark only their target
    // blocks (vs full mark_all_blocks_dirty + partial_forced_full_
    // by_impact_total). #3135 owns the lock; #3067 owns the drain
    // at entry. Size snapshot read under shared dep_graph_mtx_ is
    // race-free relative to record_dependency's non-stale path
    // (which adds at the tail under dep_graph_mtx_ write).
    std::size_t initial_deferred_edges_size = 0;
    {
        lock_order::OrderedSharedLock<std::shared_mutex> read(dep_graph_mtx_,
                                                              lock_order::Level::DepGraph);
        initial_deferred_edges_size = deferred_hybrid_edges_.size();
    }
    // Issue #3283: snapshot the deferred-hybrid queue generation under
    // the cascade_decision_mtx_ critical section. Both emplace sites
    // (record_dependency + record_block_dependency stale-reject paths)
    // now serialize under this lock (#3135 parity) and bump
    // deferred_hybrid_gen_; any bump after gen0 during the decision
    // window = a concurrent stale-reject re-armed edges the entry
    // drain / size snapshot never saw → fail-closed before partial
    // (pre-peel re-check below, AC1(b) take-full).
    const std::uint64_t gen0 = deferred_hybrid_gen_.load(std::memory_order_acquire);
    std::size_t ok = 0;
    // Issue #4172: pre-JIT residual CastOp verification face. Under the
    // production hard face, a pending #3347 undermark latch means
    // persisted CastOp sites the cone missed exist; after this peel
    // they must not silently reach JIT. The loop below forces the
    // persist sites into each entry's peel mask, and the post-peel arm
    // re-runs the DeadCoercion fold (full peel) / counts survivors
    // (partial peel), arming the existing #3699 density gate reject
    // when unannotated CastOps survive under a hard density face.
    // Soft / Off: face disarmed — zero extra.
    const bool residual_undermark_face_4172 =
        aura::compiler::typed_audit::production_hard_face_active() &&
        aura::compiler::dirty::residual_castop_undermark_pending();
    const std::uint64_t epoch_4172 = aura::core::current_mutation_epoch();
    // Snapshot names first — relower may erase/replace entries.
    std::vector<std::string> dirty_names;
    dirty_names.reserve(ir_cache_v2_.size());
    for (const auto& [n, e] : ir_cache_v2_) {
        if (e.dirty || e.dirty_block_count() > 0)
            dirty_names.push_back(n);
    }
    // Issue #3381: production facade early-return only dirties the root
    // (mark_define_dirty / invalidate_function call hard_invalidate_via_
    // facade → mark_body_only_dirty(root) + invalidate_shape(root) +
    // return — no Soft invalidate BFS, no hybrid drain under production).
    // Without this union, callers stay clean (dirty=false /
    // dirty_block_count()==0) and `should_partial_relower_impact_checked`
    // silently skips them (dirty_count==0 → false early-exit; impact_ub
    // never consulted). `lookup_define_v2(g)` returns 0 on the
    // pre-mutate IR while the Call encoding still names the old `f`
    // — miss-compile.
    //
    // Issue #3474: #3381 walked one hop of dep_graph_[root].called_by.
    // Chain f ← g ← h left h clean. Snapshot the same FIFO called_by
    // walk Soft invalidate_function already uses (std::deque
    // push_back/pop_front + visited) and mark_caller_body_dirty each
    // reachable caller. Shared dep_graph_mtx_ only — Soft invalidate
    // owns exclusive teardown. Not a second AOT path; facade still
    // owns decide_and_reemit.
    //
    // Reuse existing bricks under the cascade_decision_mtx_ critical
    // section: FIFO called_by walk (shared dep_graph_mtx_),
    // mark_caller_body_dirty (IRC entry alias for mark_body_only_dirty,
    // same call shape Soft BFS uses at service_dirty.cpp:1087), and
    // the #3283 fail-closed take-full shape (mark_all_blocks_dirty +
    // dirty=true + partial_forced_full_by_impact_total bump). Soft /
    // Off + clean (armed==0) single-fiber skips this entirely — zero
    // extra BFS, zero extra dirty marks, zero extra lock work (AC3).
    // Existing Strict force-dirty of ALL callers (#3165/#3187) at
    // fail_closed_soft_dual_graph_parity_before_partial_ remains
    // authoritative under dual-graph failure (AC4).
    if (aura::compiler::typed_audit::production_defaults_active() || initial_armed) {
        std::vector<std::string> caller_names;
        {
            lock_order::OrderedSharedLock<std::shared_mutex> read(dep_graph_mtx_,
                                                                  lock_order::Level::DepGraph);
            std::deque<std::string> bfs;
            std::unordered_set<std::string> visited;
            for (const auto& root : dirty_names) {
                if (visited.insert(root).second)
                    bfs.push_back(root);
            }
            while (!bfs.empty()) {
                auto current = bfs.front();
                bfs.pop_front();
                auto dit = dep_graph_.find(current);
                if (dit == dep_graph_.end())
                    continue;
                for (const auto& caller : dit->second.called_by) {
                    if (!visited.insert(caller).second)
                        continue;
                    caller_names.push_back(caller);
                    bfs.push_back(caller);
                }
            }
        }
        // Append callers to the peel set + mark their bodies dirty
        // using the existing helper. The existing per-entry loop below
        // will then see dirty_count>0 on each caller and consult
        // impact_ub (or fail-closed at #3310 production gate).
        for (const auto& caller : caller_names) {
            auto cit = ir_cache_v2_.find(caller);
            if (cit != ir_cache_v2_.end()) {
                (void)cit->second.mark_caller_body_dirty();
                finish_cascade_soa_dirty_sync_(cit->second);
            }
            dirty_names.push_back(caller);
        }
        // #3381 AC2 + #3283 fail-closed: if deferred_hybrid_gen moved
        // since gen0 (concurrent stale-reject re-armed edges the entry
        // drain / size snapshot never saw), force full for the cone —
        // never silent skip. Reuses the #3283 mark_all_blocks_dirty +
        // dirty=true + partial_forced_full_by_impact_total shape.
        if (deferred_hybrid_gen_.load(std::memory_order_acquire) != gen0) {
            for (const auto& name : dirty_names) {
                auto eit = ir_cache_v2_.find(name);
                if (eit != ir_cache_v2_.end()) {
                    eit->second.mark_all_blocks_dirty();
                    eit->second.dirty = true;
                }
            }
            metrics_.partial_forced_full_by_impact_total.fetch_add(1, std::memory_order_relaxed);
        }
        // Issue #3615: cone-wide dual-graph parity check (Issue #3486 only
        // consulted dirty_names.front(); a fork against any other cone
        // name slipped through). New helper walks every name in
        // dirty_names + detects Soft-erased holes (called_by empty but
        // node_dep has encode_fn_node dependents). Same take-full path
        // (rebuild + all-callers dirty + partial_forced_full_by_impact_total
        // + dual_dep_graph_parity_fail_total). Soft / Off skip (zero
        // extra — AC3). No new query key (AC4). String graph stays
        // authority (rebuild_node_dep_graph_from_string).
        if (!dirty_names.empty() && (aura::compiler::typed_audit::production_defaults_active() ||
                                     aura::compiler::typed_audit::get_strategy() ==
                                         aura::compiler::typed_audit::AuditStrategy::Full)) {
            bool want = true;
            const bool cone_failed =
                fail_closed_soft_dual_graph_parity_before_partial_cone_(dirty_names, want);
            if (cone_failed && !want) {
                for (const auto& name : dirty_names) {
                    auto cit = ir_cache_v2_.find(name);
                    if (cit != ir_cache_v2_.end()) {
                        cit->second.mark_all_blocks_dirty();
                        cit->second.dirty = true;
                    }
                }
            }
        }
    }
    // Issue #3161: snapshot the NodeId mirror-edge counter under the
    // cascade_decision_mtx_ critical section so the mid-loop
    // re-arm observation (re_armed_now OR graph_grew_mid_loop) is
    // race-free against record_dependency's non-stale path. The
    // non-stale path doesn't take cascade_decision_mtx_ (only
    // dep_graph_mtx_), so without a snapshot it could add edges
    // mid-peel that under-count impact_ub for the next iteration.
    // Issue #3348: also snapshot block-dep mirror — record_block_dependency
    // non-stale can grow dep_graph_block_mirror_edges_total without
    // bumping the fn-level NodeId counter (string/fn edge already
    // exists). Same critical section as gen0 / deferred size.
    const auto initial_node_mirror_edges =
        metrics_.dep_graph_node_mirror_edges_total.load(std::memory_order_relaxed);
    const auto initial_block_mirror_edges =
        metrics_.dep_graph_block_mirror_edges_total.load(std::memory_order_relaxed);
    bool rearm_observed_mid_loop = false;
    // Issue #3611: index-based walk — the #3168 attribution block below
    // can push newly-armed edge endpoints into dirty_names mid-loop, and
    // a range-for would never see them (end captured once) while a
    // reference binding would dangle across the reallocation. Value
    // copy keeps `name` stable across peer pushes.
    for (std::size_t dn_i = 0; dn_i < dirty_names.size(); ++dn_i) {
        const std::string name = dirty_names[dn_i];
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            continue;
        // Issue #4172: force the persisted CastOp sites into THIS
        // entry's peel mask before the decision — the #3349 arm only
        // runs inside want_partial && dirty_n>0, and an undermarked
        // cone must not also under-mark the peel. Idempotent (mark
        // dedup'd); persist empty → 0 extra.
        if (residual_undermark_face_4172 &&
            aura::compiler::dirty::residual_castop_persist_size() > 0) {
            (void)aura::compiler::dirty::force_residual_castop_undermark_into_cone();
            (void)mark_entry_from_dead_coercion_persist_(it->second);
        }
        // Issue #3484: a name that entered the peel set (root, #3381 /
        // #3474 caller, or persist under-mark) must not silent-skip
        // on a zero mask. Production / Full fail-closed full; Soft /
        // Off keep the clean continue (should_partial_relower
        // dirty_count==0 → false).
        bool zero_mask_forced_full = false;
        if (!it->second.dirty && it->second.dirty_block_count() == 0) {
            const bool production_zero =
                aura::compiler::typed_audit::production_defaults_active() ||
                aura::compiler::typed_audit::get_strategy() ==
                    aura::compiler::typed_audit::AuditStrategy::Full;
            if (!production_zero)
                continue;
            it->second.mark_all_blocks_dirty();
            it->second.dirty = true;
            it->second.content_stored_this_epoch = false;
            metrics_.partial_forced_full_by_impact_total.fetch_add(1, std::memory_order_relaxed);
            zero_mask_forced_full = true;
        }
        // Locate the Define node + Lambda body in the workspace.
        // Issue #2730: skip free-list ghosts (same as mutate:rebind find).
        aura::ast::NodeId def_id = aura::ast::NULL_NODE;
        aura::ast::NodeId lambda_id = aura::ast::NULL_NODE;
        // Issue #4598: compare the interned SymId over the tag / sym
        // columns; get() (children copy) only on the match.
        const auto want_sym = ws_pool->find_by_name(name);
        // Issue #4681: O(1) memoised sym -> first Define (tag index)
        // instead of an O(N) node scan per dirty name.
        if (want_sym && define_lookup_enabled_4681_()) {
            const auto hit = first_define_for_sym_4681_(*ws_flat, *want_sym);
            if (hit != aura::ast::NULL_NODE) {
                auto v = ws_flat->get(hit);
                def_id = hit;
                if (!v.children.empty())
                    lambda_id = v.child(0);
            }
        }
        const auto ws_n =
            (want_sym && def_id == aura::ast::NULL_NODE && !define_lookup_enabled_4681_())
                ? ws_flat->size()
                : aura::ast::NodeId{0};
        for (aura::ast::NodeId id = 0; id < ws_n; ++id) {
            if (ws_flat->tag(id) != aura::ast::NodeTag::Define || ws_flat->sym_id(id) != *want_sym)
                continue;
            if (ws_flat->is_free_slot(id))
                continue;
            auto v = ws_flat->get(id);
            def_id = id;
            if (!v.children.empty())
                lambda_id = v.child(0);
            break;
        }
        if (def_id == aura::ast::NULL_NODE || lambda_id == aura::ast::NULL_NODE)
            continue;
        // Prefer Lambda node for per-function re-lower path.
        const auto expanded = (lambda_id < ws_flat->size() &&
                               ws_flat->get(lambda_id).tag == aura::ast::NodeTag::Lambda)
                                  ? lambda_id
                                  : def_id;
        std::string canonical =
            workspace_define_canonical_source(*ws_flat, *ws_pool, name, lambda_id);
        // Gate: should_partial_relower OR single-function dirty
        // (body-only from #1495 mark). Large dirty surfaces still
        // go through relower_define_blocks which may full-fallback.
        // Issue #2109: always consult should_partial_relower (metrics).
        std::size_t dirty_n = it->second.dirty_block_count();
        if (dirty_n == 0 && !it->second.dirty) {
            // Issue #3349: persist may be the only dirty signal for a
            // CastOp site the local mask missed. Soft / empty persist
            // → 0 extra (size 0).
            if (aura::compiler::dirty::residual_castop_persist_size() > 0) {
                (void)aura::compiler::dirty::force_residual_castop_undermark_into_cone();
                (void)mark_entry_from_dead_coercion_persist_(it->second);
                dirty_n = it->second.dirty_block_count();
            }
            if (dirty_n == 0 && !it->second.dirty) {
                // Issue #3484: persist-empty / under-mark on a cone
                // name is not a successful peel. Production / Full
                // take fail-closed full (reuse
                // partial_forced_full_by_impact_total). Soft / Off
                // keep ++ok skip (genuinely clean).
                const bool production_zero =
                    aura::compiler::typed_audit::production_defaults_active() ||
                    aura::compiler::typed_audit::get_strategy() ==
                        aura::compiler::typed_audit::AuditStrategy::Full;
                if (!production_zero) {
                    ++ok;
                    continue;
                }
                it->second.mark_all_blocks_dirty();
                it->second.dirty = true;
                it->second.content_stored_this_epoch = false;
                metrics_.partial_forced_full_by_impact_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                dirty_n = it->second.dirty_block_count();
                zero_mask_forced_full = true;
            }
        }
        metrics_.should_partial_relower_consult_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #3550: pre-cascade callees before adaptive / impact check.
        // Issue #3584: precompute returns already-dirty callee *blocks*
        // (single block unit). Do not merge-add define count into this
        // function's threshold — #3550 mixed define count, so hub
        // callees≥8−dirty_n forced full on a 1-block edit.
        // Issue #3656: absorb those blocks into impact_ub (impact_checked
        // only upgrades to full). Unknown cone (calls empty + node fn
        // edges) fail-closed full.
        const auto callee_cone = precompute_callee_cascade_for_partial(name);
        dirty_n = it->second.dirty_block_count();
        // Issue #2127: deopt + density adaptive threshold (base #2032/#2112).
        std::size_t total_blocks = 0;
        for (const auto& fb : it->second.block_dirty_per_func_)
            total_blocks += fb.size();
        const auto adaptive = consult_workload_adaptive_partial_(dirty_n, total_blocks);
        bool want_partial = adaptive.want_partial;
        if (want_partial && estimate_relower_blocks(dirty_n, get_partial_relower_threshold()) ==
                                static_cast<std::size_t>(-1))
            want_partial = false;
        // Issue #3986: Shape-flip + production persist-empty restored
        // full. Treat as unknown cone (mark_all + distinguisher).
        if (adaptive.shape_flipped_full_to_partial && !want_partial) {
            it->second.mark_all_blocks_dirty();
            it->second.dirty = true;
            metrics_.partial_forced_full_by_impact_total.fetch_add(1, std::memory_order_relaxed);
            dirty_n = it->second.dirty_block_count();
        }
        // Issue #3484: zero-mask cone name already took fail-closed
        // full — do not re-enter partial / skip-as-clean.
        if (zero_mask_forced_full)
            want_partial = false;
        // Issue #3656: map nonempty + string calls empty + node fn
        // edges = unknown callee cone. Fail-closed full (reuse
        // partial_forced_full_by_impact_total). Soft precompute
        // returned 0 so this arm never fires.
        if (want_partial && dirty_n > 0 && callee_cone == kUnknownCalleeConeBlocks) {
            want_partial = false;
            it->second.mark_all_blocks_dirty();
            it->second.dirty = true;
            metrics_.partial_forced_full_by_impact_total.fetch_add(1, std::memory_order_relaxed);
            dirty_n = it->second.dirty_block_count();
        }
        // Issue #3034: cross-check ImpactScope / hybrid-cascade upper
        // bound. Monotonic — only upgrades partial → full, never lowers
        // (storm gates stay the outer envelope). Zero cost on clean /
        // empty-impact windows (helper returns 0 → no upgrade).
        // Issue #3068: ensure / recover the source_to_ir_map and
        // snapshot impact_ub in the same critical section as the
        // threshold consult. A stale or incomplete map must not
        // under-estimate (silent partial peel). Recovered maps
        // keep partial when the bound is still ≤ dirty_n (#2206).
        // Issue #4422: helper false must not let the else branch's
        // per-fn arm return success. Soft ub==0 never sets this.
        bool impact_refused_partial = false;
        if (want_partial && dirty_n > 0) {
            // Issue #3349: re-union DeadCoercion persist into type∪IR
            // before impact_ub / partial commit (decision-time cone
            // lag after #3120 / #3228). Soft / empty persist → 0 extra.
            (void)aura::compiler::dirty::force_residual_castop_undermark_into_cone();
            // Issue #3255: Soft dual-graph parity fail-closed before peel.
            fail_closed_soft_dual_graph_parity_before_partial_(it->second, want_partial);
            if (want_partial && !prepare_source_to_ir_map_for_partial_(it->second)) {
                want_partial = false;
                it->second.mark_all_blocks_dirty();
                it->second.dirty = true;
                metrics_.partial_forced_full_by_impact_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
            } else if (want_partial) {
                // Issue #3349: mark persist-mapped CastOp blocks dirty
                // so partial does not cone-skip a type-changed site.
                bool persist_attributed = false;
                if (aura::compiler::dirty::residual_castop_persist_size() > 0)
                    persist_attributed = mark_entry_from_dead_coercion_persist_(it->second);
                dirty_n = it->second.dirty_block_count();
                // Issue #3310: production must fail-closed when
                // ImpactScope cannot be computed (impact_upper_bound_for_entry_
                // returns 0 for no-flat / no-root). Soft/Off keeps
                // zero-cost threshold partial (existing contract). The
                // map-empty sentinel -1 path through
                // should_partial_relower_impact_checked is untouched (AC3).
                const std::size_t impact_ub = absorb_callee_cone_into_impact_ub(
                    impact_upper_bound_for_entry_(name, it->second), callee_cone);
                const bool production_consult =
                    aura::compiler::typed_audit::production_defaults_active() ||
                    aura::compiler::typed_audit::get_strategy() ==
                        aura::compiler::typed_audit::AuditStrategy::Full;
                if (!should_partial_relower_impact_checked_prod(dirty_n, impact_ub,
                                                                production_consult)) {
                    want_partial = false;
                    impact_refused_partial = true;
                    it->second.mark_all_blocks_dirty();
                    it->second.dirty = true;
                    metrics_.partial_forced_full_by_impact_total.fetch_add(
                        1, std::memory_order_relaxed);
                } else if (production_consult &&
                           aura::compiler::dirty::residual_castop_persist_size() > 0 &&
                           !persist_attributed) {
                    // Issue #3618: persist nonempty + production + attribution
                    // failed — the map is missing the persisted CastOp site
                    // (empty OR non-empty from a partial rebuild of another
                    // fn). impact_ub from an incomplete map can stay ≤ dirty_n
                    // and cone-skip a type-changed site → fail-closed full.
                    // Attribution success (site mapped + block marked) keeps
                    // partial.
                    want_partial = false;
                    it->second.mark_all_blocks_dirty();
                    it->second.dirty = true;
                    metrics_.partial_forced_full_by_impact_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
        }
        // Issue #3161: re-arm observation mid-loop (Option A).
        // re_armed_now = deferred_hybrid_armed_ went 0→1 (stale-reject
        // path queued; blocked on cascade_decision_mtx_ so only fires
        // for rejects in flight before lock acquisition — rare).
        // graph_grew_mid_loop = record_dependency's non-stale path
        // added new NodeId mirror edges during a previous iteration's
        // peel. Non-stale path only takes dep_graph_mtx_ (not the
        // cascade_decision_mtx_ we hold), so impact_ub snapshotted
        // for the next iteration may under-count these new callee
        // edges. Issue #3348: also observe block-dep mirror growth
        // (record_block_dependency non-stale). Either signal: force
        // full for THIS define + set rearm_observed_mid_loop so
        // remaining iterations also force full (Option A).
        const bool re_armed_now = deferred_hybrid_armed_.load(std::memory_order_acquire) != 0;
        const bool graph_grew_mid_loop =
            metrics_.dep_graph_node_mirror_edges_total.load(std::memory_order_relaxed) >
                initial_node_mirror_edges ||
            metrics_.dep_graph_block_mirror_edges_total.load(std::memory_order_relaxed) >
                initial_block_mirror_edges;
        if (re_armed_now || graph_grew_mid_loop) {
            rearm_observed_mid_loop = true;
        }
        // Issue #3257: last-look armed immediately before attribution
        // / peel. Concurrent record_dependency can set the flag after
        // re_armed_now and after the #3168 size snapshot. One acquire;
        // Soft + armed==0 skips the rest (zero extra lock).
        std::size_t attr_seen_size = initial_deferred_edges_size;
        if (want_partial && deferred_hybrid_armed_.load(std::memory_order_acquire) != 0)
            rearm_observed_mid_loop = true;
        if (rearm_observed_mid_loop && want_partial) {
            // Issue #3168: prefer new-edge-only mark over full fallback.
            // Walk [initial_deferred_edges_size, current) under shared
            // dep_graph_mtx_. One matching edge dirties this define via
            // one mark_blocks_dirty span per function (#4411). Last-resort
            // full (mark_all_blocks_dirty + counter) only when the
            // new-edge set is empty / cannot be attributed. Bumps
            // cascade_rearm_new_edge_only_total when attribution succeeds
            // (additive only — #3097 semantics unchanged).
            bool attributed_new_edge = false;
            std::vector<std::pair<std::string, std::string>> new_edges_snapshot;
            {
                lock_order::OrderedSharedLock<std::shared_mutex> read(dep_graph_mtx_,
                                                                      lock_order::Level::DepGraph);
                attr_seen_size = deferred_hybrid_edges_.size();
                if (attr_seen_size > initial_deferred_edges_size) {
                    new_edges_snapshot.reserve(attr_seen_size - initial_deferred_edges_size);
                    for (std::size_t idx = initial_deferred_edges_size; idx < attr_seen_size;
                         ++idx) {
                        new_edges_snapshot.push_back(deferred_hybrid_edges_[idx]);
                    }
                }
            }
            if (!new_edges_snapshot.empty()) {
                // Issue #4411: the edge is a (caller, callee) name pair
                // with no block id, so one match dirties the whole define.
                // One mark_blocks_dirty span per function is one SoA fence.
                for (const auto& [caller_name, callee_name] : new_edges_snapshot) {
                    if (caller_name != name && callee_name != name)
                        continue;
                    const auto func_count = it->second.block_dirty_per_func_.size();
                    for (std::size_t fi = 0; fi < func_count; ++fi) {
                        const auto nblk = it->second.block_dirty_per_func_[fi].size();
                        if (nblk == 0)
                            continue;
                        std::vector<std::uint32_t> ids(nblk);
                        for (std::uint32_t bi = 0; bi < nblk; ++bi)
                            ids[bi] = bi;
                        it->second.mark_blocks_dirty(fi, ids);
                    }
                    attributed_new_edge = true;
                    break; // one match is enough to keep want_partial
                }
            }
            if (attributed_new_edge) {
                metrics_.cascade_rearm_new_edge_only_total.fetch_add(1, std::memory_order_relaxed);
                // Issue #3611: attribution used to keep the stale
                // pre-attribution want_partial and only mark THIS
                // define — the peer endpoint of the armed edge (e.g.
                // deferred (g, f) while peeling f) never entered the
                // peel set (production facade early-return already
                // skipped the Soft BFS erase), so lookup_define_v2(g)
                // could serve pre-mutate IR whose Call still names the
                // old f encoding. Close the residual:
                //   (a) both endpoints of every newly-armed edge enter
                //       the peel set (dirty_names push if missing) with
                //       mark_caller_body_dirty (#3474 union shape; the
                //       per-name loop above is index-based so mid-loop
                //       growth is peeled in the same sweep);
                //   (b) want_partial is reconsulted from the post-mark
                //       dirty_n + impact_ub (#3310 production
                //       fail-closed preserved — the consult is
                //       monotone, partial → full only).
                for (const auto& [edge_caller, edge_callee] : new_edges_snapshot) {
                    for (const auto& peer : {edge_caller, edge_callee}) {
                        if (peer == name)
                            continue; // current define: already marked above
                        if (std::find(dirty_names.begin(), dirty_names.end(), peer) !=
                            dirty_names.end())
                            continue; // already in the peel set
                        const auto pit = ir_cache_v2_.find(peer);
                        if (pit != ir_cache_v2_.end()) {
                            (void)pit->second.mark_caller_body_dirty();
                            finish_cascade_soa_dirty_sync_(pit->second);
                        }
                        dirty_names.push_back(peer);
                    }
                }
                const std::size_t impact_ub = absorb_callee_cone_into_impact_ub(
                    impact_upper_bound_for_entry_(name, it->second), callee_cone);
                const bool production_consult =
                    aura::compiler::typed_audit::production_defaults_active() ||
                    aura::compiler::typed_audit::get_strategy() ==
                        aura::compiler::typed_audit::AuditStrategy::Full;
                if (!should_partial_relower_impact_checked_prod(it->second.dirty_block_count(),
                                                                impact_ub, production_consult)) {
                    want_partial = false;
                    impact_refused_partial = true;
                    it->second.mark_all_blocks_dirty();
                    it->second.dirty = true;
                    metrics_.partial_forced_full_by_impact_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
            } else {
                // Defensive last-resort: new-edge set empty or
                // cannot be attributed. Preserve #3097 semantics.
                want_partial = false;
                it->second.mark_all_blocks_dirty();
                it->second.dirty = true;
                metrics_.partial_forced_full_by_impact_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
            }
        }
        // Issue #3257: concurrent record_dependency can append after
        // the #3168 attribution snapshot and before peel. If the tail
        // grew, fail-closed to full (bounded once — no second walk).
        // Distinguisher: partial_forced_full_by_impact_total.
        // Attribution of edges that already touched this define still
        // prefers partial (#3168). Soft + armed==0: one acquire, no
        // extra lock.
        if (want_partial) {
            const bool post_attr_armed =
                deferred_hybrid_armed_.load(std::memory_order_acquire) != 0;
            if (post_attr_armed) {
                std::size_t size_now = attr_seen_size;
                {
                    lock_order::OrderedSharedLock<std::shared_mutex> read(
                        dep_graph_mtx_, lock_order::Level::DepGraph);
                    size_now = deferred_hybrid_edges_.size();
                }
                if (size_now > attr_seen_size) {
                    want_partial = false;
                    it->second.mark_all_blocks_dirty();
                    it->second.dirty = true;
                    metrics_.partial_forced_full_by_impact_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
        }
        // Issue #3283: fail-closed re-arm lag close before partial
        // peel. A concurrent record_dependency / record_block_dependency
        // stale-reject can re-arm the deferred queue AFTER the
        // impact_ub consult for this define (both emplace sites now
        // serialize under cascade_decision_mtx_ and bump
        // deferred_hybrid_gen_ — #3135 parity), so the late edges are
        // NOT in the dirty mask / impact_ub consulted by
        // should_partial_relower_impact_checked → a partial peel would
        // under-mark the lagging caller cone. Any generation bump since
        // the batch snapshot (gen0) means the queue grew: if this
        // define's cone intersects a newly deferred (caller, callee)
        // pair, force full (AC1(b) take-full; distinguisher
        // partial_forced_full_by_impact_total). Prefer the #3168
        // target-only mark when the new edges are attributable; this is
        // the fail-closed fallback for edges that arrived after that
        // attribution snapshot. Soft/Off + armed==0: gen never moves
        // (one acquire, no extra lock — AC2 zero-cost).
        if (want_partial && deferred_hybrid_gen_.load(std::memory_order_acquire) != gen0) {
            bool cone_hit = false;
            {
                lock_order::OrderedSharedLock<std::shared_mutex> read(dep_graph_mtx_,
                                                                      lock_order::Level::DepGraph);
                const auto cur = deferred_hybrid_edges_.size();
                if (cur > initial_deferred_edges_size) {
                    for (std::size_t idx = initial_deferred_edges_size; idx < cur; ++idx) {
                        const auto& [c, k] = deferred_hybrid_edges_[idx];
                        if (c == name || k == name) {
                            cone_hit = true;
                            break;
                        }
                    }
                }
            }
            if (cone_hit) {
                want_partial = false;
                it->second.mark_all_blocks_dirty();
                it->second.dirty = true;
                metrics_.partial_forced_full_by_impact_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
            }
        }
        // Issue #3348: last-look NodeId / block-dep mirror growth
        // immediately before committing partial. Non-stale
        // record_dependency / record_block_dependency take only
        // dep_graph_mtx_ (not cascade_decision_mtx_), so they can
        // add live edges after #3168 / #3257 / #3283 re-checks.
        // Deferred-queue attribution cannot see these edges →
        // fail-closed full (reuse partial_forced_full_by_impact_total;
        // no new query key). Soft + counters flat: one acquire each,
        // zero extra (no mark, no bump).
        if (want_partial) {
            const auto node_now =
                metrics_.dep_graph_node_mirror_edges_total.load(std::memory_order_acquire);
            const auto block_now =
                metrics_.dep_graph_block_mirror_edges_total.load(std::memory_order_acquire);
            if (node_now > initial_node_mirror_edges || block_now > initial_block_mirror_edges) {
                want_partial = false;
                it->second.mark_all_blocks_dirty();
                it->second.dirty = true;
                metrics_.partial_forced_full_by_impact_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
            }
        }
        if (want_partial)
            metrics_.should_partial_relower_yes_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #1623: workspace dirty sweep is on eval/eval_ir entry.
        metrics_.eval_path_relower_total.fetch_add(1, std::memory_order_relaxed);
        const auto per_before =
            metrics_.relower_per_function_called_count.load(std::memory_order_relaxed);
        const auto blocks_before =
            metrics_.incremental_relower_blocks_total.load(std::memory_order_relaxed);
        // Issue #4653: refusal-aware per-fn commit arm.  Reaching the
        // else arm below means SOME reason cleared want_partial for
        // this define: the #4422 impact refusal, the zero block mask
        // (#3484), the unknown callee cone (#3656), the shape flip
        // (#3986), the persist attribution fail-closed (#3618), the
        // map prepare / storm gates (#2190), the dual-graph parity
        // fail-closed, or the re-arm observations (#3168 / #3257 /
        // #3283 / #3348).  Only the #4422 impact refusal used to set
        // impact_refused_partial, so every other reason still ran the
        // per-fn commit arm.  The per-fn peel keeps the OLD
        // instruction of a clean (idf == 0) slot, so a same-shape edit
        // promoted the pre-edit IR (dirty = false,
        // content_stored_this_epoch = true, live restamp) and
        // lookup_define_v2 served the stale bundle.  On the
        // production face EVERY want_partial == false reason must
        // refuse the per-fn commit arm, exactly like the #4422 impact
        // refusal: the full arm runs, or the entry stays dirty until
        // a whole-function store lands.  Soft / Off keep the previous
        // behaviour (the impact refusal is the only refusal there).
        const bool production_face_4653 =
            aura::compiler::typed_audit::production_defaults_active() ||
            aura::compiler::typed_audit::get_strategy() ==
                aura::compiler::typed_audit::AuditStrategy::Full;
        const bool commit_per_fn_partial_4653 = !production_face_4653 && !impact_refused_partial;
        // Prefer partial path when gate says so (or dirty_n==0 edge).
        if (want_partial || dirty_n == 0) {
            if (relower_define_blocks(name, canonical, *ws_flat, *ws_pool, expanded)) {
                ++ok;
                // Issue #4172: post-peel residual verification before
                // JIT lower. Partial peel: survivors counted only (the
                // peel already DCE'd its blocks; scanning stale clean
                // blocks would reopen #3689). Reject arms via the
                // existing density gate inside the verifier.
                if (residual_undermark_face_4172) {
                    auto vit = ir_cache_v2_.find(name);
                    if (vit != ir_cache_v2_.end())
                        (void)
                            aura::compiler::opt_registry::verify_residual_undermark_post_peel_4172(
                                vit->second.irs, /*want_partial=*/true, &type_registry_,
                                epoch_4172);
                }
                // Count true partial only (per-fn / blocks), not full-fallback.
                if (metrics_.relower_per_function_called_count.load(std::memory_order_relaxed) >
                        per_before ||
                    metrics_.incremental_relower_blocks_total.load(std::memory_order_relaxed) >
                        blocks_before) {
                    metrics_.incremental_eval_relower_hits.fetch_add(1, std::memory_order_relaxed);
                    metrics_.incremental_partial_relower_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                    // Issue #2193 AC4: partial success clears last-reason to Ok.
                    note_relower_fallback(metrics_, RelowerFallbackReason::Ok);
                } else {
                    // Internal full-fallback inside relower_define_blocks.
                    note_relower_fallback(metrics_, RelowerFallbackReason::Other);
                    metrics_.incremental_full_fallback_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                }
            } else {
                note_relower_fallback(metrics_, RelowerFallbackReason::RelowerReject);
            }
        } else {
            // Over threshold / storm → full re-lower; record reason (#2193).
            // Issue #4422: impact already refused — do not commit per-fn.
            note_relower_fallback(metrics_, RelowerFallbackReason::Threshold);
            metrics_.incremental_full_fallback_total.fetch_add(1, std::memory_order_relaxed);
            if (relower_define_blocks(name, canonical, *ws_flat, *ws_pool, expanded,
                                      /*commit_per_fn_partial=*/commit_per_fn_partial_4653)) {
                ++ok;
                // Issue #4172: post-peel residual verification before
                // JIT lower. Full peel re-runs the DeadCoercion fold
                // over the fresh body (all blocks relowered — no #3689
                // mixed-IR hazard) before the survivor count.
                if (residual_undermark_face_4172) {
                    auto vit = ir_cache_v2_.find(name);
                    if (vit != ir_cache_v2_.end())
                        (void)
                            aura::compiler::opt_registry::verify_residual_undermark_post_peel_4172(
                                vit->second.irs, /*want_partial=*/false, &type_registry_,
                                epoch_4172);
                }
            } else {
                // Issue #4436: full arm refused a dirty-callee embed.
                embed_refused_4436_.push_back(name);
            }
        }
    }
    // Issue #4375 (方案 1): the cascade relower's solve_delta passes
    // latch the pending residual face (hard under Full strategy) — and
    // without a boundary persist the latch has no clear path, so the
    // depth==0 real-quiet face refuses the caller's next typed entry
    // (the mutate loop breaks). Drive the drain at the relower's end:
    // on a clean system the quiet drain clears the stale latch
    // (note(0)); with residual the full solve runs to SOLVED and
    // clears it. Soft/Off: one hard-face load, zero cost.
    if (aura::compiler::typed_audit::production_hard_face_active()) {
        aura::compiler::TypeChecker drain_tc(type_registry_);
        (void)drain_tc.constraint_system().drain_pending_full_solve_before_commit();
    }
    // Issue #3255: clean-cache relower still closes a post-peel fork.
    restore_soft_dual_graph_parity_after_relower_();
    // Issue #4436: one retry after earlier names in this snapshot have
    // stored. Mutual recursion stays dirty. Not a sort of dirty_names.
    if (!embed_refused_4436_.empty() &&
        (aura::compiler::typed_audit::production_defaults_active() ||
         aura::compiler::typed_audit::get_strategy() ==
             aura::compiler::typed_audit::AuditStrategy::Full)) {
        const auto refused = std::move(embed_refused_4436_);
        embed_refused_4436_.clear();
        for (const auto& retry_name : refused) {
            auto eit = ir_cache_v2_.find(retry_name);
            if (eit == ir_cache_v2_.end())
                continue;
            if (!eit->second.dirty && eit->second.dirty_block_count() == 0 &&
                eit->second.content_stored_this_epoch && !eit->second.abort_map_invalid)
                continue;
            const bool retry_ok = relower_current_define_full_for_test(retry_name);
            if (retry_ok) {
                ++ok;
                if (residual_undermark_face_4172) {
                    auto vit = ir_cache_v2_.find(retry_name);
                    if (vit != ir_cache_v2_.end())
                        (void)
                            aura::compiler::opt_registry::verify_residual_undermark_post_peel_4172(
                                vit->second.irs, /*want_partial=*/false, &type_registry_,
                                epoch_4172);
                }
            }
        }
    }
    embed_refused_4436_.clear();
    return ok;
}

bool CompilerService::relower_define_blocks(const std::string& name, std::string_view source,
                                            aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                            aura::ast::NodeId expanded_root,
                                            bool commit_per_fn_partial) {
    IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
    // Issue #4341: sample BEFORE any per-fn / full re-lower in this
    // call — both the per-fn partial restamp and the full-fallback
    // store_define_v2 stamp the IR with this sampled epoch (#4338
    // semantics), so a Workspace-rank mutate landing mid-relower
    // leaves the stamp behind the live epoch (needs-relower next).
    const std::uint64_t ir_sample_epoch = aura::core::current_mutation_epoch();
    auto it = ir_cache_v2_.find(name);
    if (it == ir_cache_v2_.end()) {
        // No entry → caller needs to do a full first-time lower.
        return false;
    }
    // Issue #3324: abort-restore must not instr-peel / skip-as-clean
    // on pre-abort IR. Partial peel restamps live and can clear dirty
    // without store_define_v2, which would let lookup serve stale IR
    // once abort_force_generation is acked. Force full re-lower.
    const bool abort_stale_map =
        it->second.abort_map_invalid || abort_force_rejects_clean_hit_(it->second);
    if (abort_stale_map) {
        it->second.dirty = true;
        it->second.mark_all_blocks_dirty();
        it->second.content_stored_this_epoch = false;
    }
    // Issue #2181: hard-require SoA dirty sync before any partial peel.
    // On desync (pre or residual post-sync) skip instr/per-fn partial and
    // fall through to full re-lower below — no under-invalidate.
    const bool allow_partial_peel = !abort_stale_map && gate_partial_soa_dirty_sync_(it->second);
    // Issue #4618: the same refusal as cache_define / the workspace
    // specialise pass. A body that set!s a non-local (workspace or host
    // global) cannot be lowered to IR - lowering_impl.cpp's Set returns
    // the value with no store. Re-lowering it here republished the
    // dropped store for a mutate:rebind / dirty body, so leave the entry
    // dirty: the caller's fallback is cache_define, which skips IR and
    // keeps the tree-walker closure.
    if (define_body_sets_nonlocal_4618(flat, pool, expanded_root, name)) {
        it->second.dirty = true;
        return false;
    }
    // Issue #5421: dotted-rest is lowerable; see ir_executor_impl.cpp.
    // Issue #3550: callee cascade before partial peel / block count.
    // Issue #3656: unknown cone (calls empty + node fn edges) is not
    // a successful instr peel — fail-closed full.
    const auto callee_cone = precompute_callee_cascade_for_partial(name);
    bool allow_partial = allow_partial_peel;
    if (allow_partial && callee_cone == kUnknownCalleeConeBlocks &&
        (aura::compiler::typed_audit::production_defaults_active() ||
         aura::compiler::typed_audit::get_strategy() ==
             aura::compiler::typed_audit::AuditStrategy::Full)) {
        it->second.mark_all_blocks_dirty();
        it->second.dirty = true;
        metrics_.partial_forced_full_by_impact_total.fetch_add(1, std::memory_order_relaxed);
        allow_partial = false;
    }
    // Issue #2133: when precise instruction dirty is present and under
    // partial threshold, consume ImpactScope-style instr path (pass peel
    // only; no full AST re-lower). Falls through to block/fn paths if
    // over threshold or no instr precision.
    if (allow_partial) {
        std::size_t dirty_instr_n = 0;
        ImpactScope synthetic;
        for (std::size_t fi = 0; fi < it->second.irs.size(); ++fi) {
            for (std::size_t bi = 0; bi < it->second.irs[fi].blocks.size(); ++bi) {
                const auto n = it->second.irs[fi].blocks[bi].instructions.size();
                bool any_in_block = false;
                for (std::uint32_t ii = 0; ii < n; ++ii) {
                    if (it->second.is_instruction_dirty(fi, static_cast<std::uint32_t>(bi), ii)) {
                        ++dirty_instr_n;
                        synthetic.affected_instrs.push_back(
                            {fi, static_cast<std::uint32_t>(bi), ii});
                        any_in_block = true;
                    }
                }
                if (any_in_block)
                    synthetic.affected_blocks.push_back({fi, static_cast<std::uint32_t>(bi)});
            }
        }
        synthetic.instr_level_hits = dirty_instr_n;
        if (synthetic.instr_level_eligible(get_partial_relower_threshold()) &&
            relower_affected_instrs_(name, it->second, synthetic)) {
            if (!source.empty()) {
                it->second.source = std::string(source);
                it->second.source_hash = fnv1a_64(it->second.source);
            }
            // Issue #2181 AC2: desync must be 0 after successful peel.
            if (it->second.soa_mod.count_block_instr_dirty_desync() != 0) {
                metrics_.soa_dirty_desync_force_full_total.fetch_add(1, std::memory_order_relaxed);
                // fall through to full
            } else {
                // Issue #3481 AC2: instruction peel is pass-only (no AST
                // re-lower). Ack peer + abort-force gen; do not restamp
                // mutation/bridge/defuse/soa to live and do not treat
                // coverage as content promotion (#3445).
                ack_cache_entry_fences_live_(it->second, name);
                // Issue #3484: pass-only is not an AST-rooted rewrite.
                // Production / Full fall through to per-fn /
                // store_define_v2 (existing full-fallback). Soft / Off
                // keep the return-true peel (zero extra).
                const bool production_instr =
                    aura::compiler::typed_audit::production_defaults_active() ||
                    aura::compiler::typed_audit::get_strategy() ==
                        aura::compiler::typed_audit::AuditStrategy::Full;
                if (production_instr) {
                    it->second.mark_all_blocks_dirty();
                    it->second.dirty = true;
                    it->second.content_stored_this_epoch = false;
                    metrics_.partial_forced_full_by_impact_total.fetch_add(
                        1, std::memory_order_relaxed);
                    // fall through to per-fn / full
                } else {
                    (void)flat;
                    (void)pool;
                    (void)expanded_root;
                    return true;
                }
            }
        }
    }
    const std::size_t dirty_blocks = it->second.dirty_block_count();
    // Issue #603: bump the block_dirty_hits counter whenever the
    // bitmask reports ≥1 dirty block in this entry. Pair with
    // relower_blocks_saved (the clean-block counter above) so
    // an AI agent can compute dirty_block_ratio = hits / (hits +
    // saved). Atomic relaxed; advisory telemetry only.
    if (dirty_blocks > 0)
        metrics_.ir_soa_block_dirty_hits_total.fetch_add(dirty_blocks, std::memory_order_relaxed);
    // Issue #1639: dirty_block_ratio running sums — numerator is
    // dirty_blocks (the hit count), denominator is dirty_blocks +
    // blocks_saved (the total block count seen across all calls).
    // Dashboards compute dirty_block_ratio = numerator / denominator
    // (or the per-snapshot basis-points variant in the primitive).
    {
        std::size_t total_blocks_seen = dirty_blocks;
        for (const auto& fb : it->second.block_dirty_per_func_)
            total_blocks_seen += fb.size();
        evaluator_.bump_dirty_block_ratio(dirty_blocks, total_blocks_seen);
    }
    // Issue #3481: skip-as-clean after a facade restamp + later
    // bit-clear would return true against pre-mutate irs. Require
    // a content store this epoch (and no abort latch).
    if (dirty_blocks == 0 && !it->second.dirty && !it->second.irs.empty() &&
        !it->second.abort_map_invalid && it->second.content_stored_this_epoch) {
        metrics_.relower_skipped_entirely_count.fetch_add(1, std::memory_order_relaxed);
        metrics_.irsoa_cache_miss_reduction.fetch_add(1, std::memory_order_relaxed);
        std::size_t total_blocks_saved = 0;
        for (const auto& fb : it->second.block_dirty_per_func_)
            total_blocks_saved += fb.size();
        if (total_blocks_saved > 0) {
            metrics_.ir_soa_relower_blocks_saved_total.fetch_add(total_blocks_saved,
                                                                 std::memory_order_relaxed);
            evaluator_.bump_incremental_closure_min_scope_win(total_blocks_saved);
        }
        return true;
    }
    // Issue #224 cycle 3: detect single-function-dirty
    // and dispatch to per-function re-lower. The
    // contract: when the bitmask reports dirty blocks
    // in EXACTLY ONE function in the entry, we can
    // re-lower just that one function (the body
    // Lambda, typically function idx 1) and replace
    // it in the cache without re-iterating the rest
    // of the bundle. The cycle-3 win: avoid the full
    // bundle re-iteration when only one function is
    // dirty.
    //
    // Caveats (cycle-3 scope):
    //   - We require that expanded_root points to a
    //     Lambda (the function body). For nested
    //     lambdas (multi-function bundles), the
    //     dispatch falls back to full re-lower
    //     because the cached entry doesn't store
    //     per-function source node ids.
    //   - The MakeClosure operands in the new
    //     function reference the original func_id
    //     (we preserve it on replace), so callers
    //     that reference the old function keep
    //     working.
    // Issue #1506: allow single-function bundles (irs.size()==1 —
    // the common post-store_define_v2 shape that drops __top__).
    // Previously required irs.size()>=2 which forced full re-lower
    // for every set-body on a real-lowered define.
    // Issue #2181: only when gate_partial_soa_dirty_sync_ allowed peel.
    // Issue #4422: a caller that already refused impact must not
    // commit this per-fn arm (no content store, no live restamp).
    // Fall through to store_define_v2. Direct callers keep the default.
    if (commit_per_fn_partial && allow_partial_peel && !it->second.irs.empty()) {
        std::size_t dirty_func_idx = static_cast<std::size_t>(-1);
        std::size_t dirty_func_count = 0;
        for (std::size_t fi = 0; fi < it->second.block_dirty_per_func_.size(); ++fi) {
            bool any = false;
            for (auto b : it->second.block_dirty_per_func_[fi]) {
                if (b) {
                    any = true;
                    break;
                }
            }
            if (any) {
                ++dirty_func_count;
                dirty_func_idx = fi;
            }
        }
        // Issue #1514 / #1506: any single dirty function can take
        // per-function re-lower when we have a source node.
        // Body may be at idx 0 (real lower: non-entry-only bundle)
        // or idx >= 1 (synthetic __top__ + body / nested).
        if (dirty_func_count == 1 && dirty_func_idx != static_cast<std::size_t>(-1) &&
            expanded_root != aura::ast::NULL_NODE) {
            // Snapshot dirty block ids BEFORE re-lower clears them.
            std::vector<std::uint32_t> dirty_ids;
            if (dirty_func_idx < it->second.block_dirty_per_func_.size()) {
                const auto& fb = it->second.block_dirty_per_func_[dirty_func_idx];
                dirty_ids.reserve(fb.size());
                for (std::size_t bi = 0; bi < fb.size(); ++bi) {
                    if (fb[bi])
                        dirty_ids.push_back(static_cast<std::uint32_t>(bi));
                }
            }
            // Count clean functions we skip (nested lambda win).
            const std::size_t clean_funcs = it->second.irs.size() > dirty_func_count
                                                ? it->second.irs.size() - dirty_func_count
                                                : 0;
            const auto partial_t0 = std::chrono::steady_clock::now();
            if (relower_define_function(name, dirty_func_idx, flat, pool, expanded_root)) {
                // Issue #4103: irs after the single dirty function
                // (nested lambdas at irs[2+] when the body is index 1)
                // were not rewritten. Production / Full must not set
                // content_stored_this_epoch and must not restamp
                // the live cache stamps; fall through to the full
                // re-lower. Soft / Off keeps the single-function
                // return. Reuses partial_forced_full_by_impact_total.
                const bool hard_nested_siblings =
                    (aura::compiler::typed_audit::production_defaults_active() ||
                     aura::compiler::typed_audit::get_strategy() ==
                         aura::compiler::typed_audit::AuditStrategy::Full) &&
                    it->second.irs.size() > dirty_func_idx + 1;
                if (hard_nested_siblings) {
                    it->second.mark_all_blocks_dirty();
                    it->second.dirty = true;
                    it->second.content_stored_this_epoch = false;
                    metrics_.partial_forced_full_by_impact_total.fetch_add(
                        1, std::memory_order_relaxed);
                } else {
                    // Issue #2113: debug incremental soundness oracle —
                    // after partial success, full-lower the same Lambda and
                    // compare IR (partial ≡ full). Zero cost when disabled.
                    if (incremental_soundness_enabled() && dirty_func_idx < it->second.irs.size()) {
                        auto full_fn = aura::compiler::lower_function_at(
                            flat, pool, arena_, expanded_root, &evaluator_.primitives());
                        // Align id for compare helper (ignores id already).
                        full_fn.id = it->second.irs[dirty_func_idx].id;
                        // check_incremental_soundness bumps process atomics;
                        // mirror into CompilerMetrics for dump/export.
                        const bool ok =
                            check_incremental_soundness(it->second.irs[dirty_func_idx], full_fn);
                        metrics_.incremental_soundness_runs_total.fetch_add(
                            1, std::memory_order_relaxed);
                        if (ok) {
                            metrics_.incremental_soundness_ok_total.fetch_add(
                                1, std::memory_order_relaxed);
                        } else {
                            metrics_.incremental_soundness_mismatch_total.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    }
                    // Issue #2112: feed adaptive threshold history.
                    const auto partial_ns = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - partial_t0)
                            .count());
                    note_partial_relower_cost_ns(partial_ns);
                    metrics_.avg_partial_relower_cost_ns.store(avg_partial_relower_cost_ns(),
                                                               std::memory_order_relaxed);
                    metrics_.partial_relower_cost_samples.store(
                        partial_relower_cost_samples_atomic().load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
                    metrics_.partial_relower_threshold_used.store(get_partial_relower_threshold(),
                                                                  std::memory_order_relaxed);
                    if (clean_funcs > 0) {
                        metrics_.relower_partial_funcs_saved_total.fetch_add(
                            clean_funcs, std::memory_order_relaxed);
                        // Issue #1915: minimal recompile scope observability.
                        metrics_.minimal_recompile_clean_funcs_saved.fetch_add(
                            clean_funcs, std::memory_order_relaxed);
                        metrics_.minimal_recompile_scope_samples.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    // Issue #1639: hit-rate numerator bump on partial
                    // success (paired with the denominator bump in
                    // the full-fallback path). Together they let
                    // dashboards compute relower_block_hit_rate =
                    // numerator / denominator (basis points). Pairs
                    // with the existing relower_per_function_called_count
                    // + relower_skipped_entirely_count for full per-call
                    // observability of the partial vs full decision.
                    evaluator_.bump_relower_block_hit_rate(1, 1); // 1 hit, 1 total attempt
                    // Issue #1915 AC: count blocks that took partial path.
                    if (!dirty_ids.empty()) {
                        metrics_.relower_block_count.fetch_add(dirty_ids.size(),
                                                               std::memory_order_relaxed);
                        metrics_.incremental_relower_blocks_total.fetch_add(
                            dirty_ids.size(), std::memory_order_relaxed);
                    }
                    // Issue #1495: stamp source after partial so
                    // lookup_define_v2 hits (hash match + clean dirty).
                    if (!source.empty()) {
                        it->second.source = std::string(source);
                        it->second.source_hash = fnv1a_64(it->second.source);
                    }
                    it->second.dirty = false;
                    // Issue #3481: per-fn re-lower rewrote AST/IR for this
                    // function — content is stored this epoch.
                    it->second.content_stored_this_epoch = true;
                    // Issue #4157: same content-epoch invalidation as
                    // store_define_v2 — the per-fn re-lower rewrote
                    // AST/IR after the persist was noted. Production/Full
                    // only (Soft/Off never consult the flip gate).
                    if (aura::compiler::dirty::residual_castop_persist_active())
                        aura::compiler::dirty::bump_residual_castop_persist_content_epoch();
                    // Issue #2181 AC2: after successful partial, desync must be 0.
                    if (it->second.soa_mod.count_block_instr_dirty_desync() != 0) {
                        metrics_.soa_dirty_desync_force_full_total.fetch_add(
                            1, std::memory_order_relaxed);
                        // Residual desync after peel → fall through to full.
                    } else {
                        // Issue #2183 AC1: restamp after successful per-fn partial.
                        // Issue #4341: sampled epoch, not store-time live.
                        restamp_cache_entry_live_(it->second, ir_sample_epoch, name);
                        ack_peer_ir_stale_on_restamp_(it->second, name);
                        // Issue #3136: success-path bitmap coherence (see 4968).
                        if (aura_production_defaults_active_probe() != 0) {
                            hot_update_registry().note_relower_success_coverage(
                                1ULL << (fnv1a_64(name) & 63));
                            // Issue #3229: define-id side set (6-bit collision).
                            hot_update_registry().note_relower_success_define(
                                relower_success_define_id(name));
                        }
                        // Issue #4709: park the IR gate for the drop below
                        // (see DropParkScope4709); the scope sits before the
                        // #4083 cite so that window keeps its 1400-char span.
                        DropParkScope4709 drop_park_scope_4709;
                        // Issue #1514: sync JIT — evict native code for this
                        // define so next exec recompiles only the dirty fn.
                        // Issue #4083: partial_recompile only removes the ORC
                        // tracker. Erase jit_cache_ under jit_cache_mtx_ (same
                        // window as invalidate_function) and drop the installed
                        // ScalarFn before that remove, so the next eval cannot
                        // cache-hit and a closure cannot call the unloaded
                        // address. Soft already runs partial_recompile; this
                        // is the dangling-pointer teardown, not a second gate.
                        // Do not call jit_.invalidate here — #2476 keeps a
                        // single invalidate_prefix pass inside partial_recompile.
                        {
                            std::unique_lock cache_write(jit_cache_mtx_);
                            if (jit_cache_.erase(name) > 0)
                                metrics_.jit_cache_evictions.fetch_add(1,
                                                                       std::memory_order_relaxed);
                            // Issue #4577: the bare erase leaves name#pos.
                            erase_jit_cache_bundle_rows_for_define_(name);
                        }
                        aura_drop_jit_fn_native_for_define(name.c_str());
                        (void)jit_.partial_recompile(name.c_str(), dirty_ids.data(),
                                                     dirty_ids.size());
                        metrics_.jit_partial_recompile_requests_total.fetch_add(
                            1, std::memory_order_relaxed);
                        return true;
                    }
                } // Issue #4103: Soft / Off single-function return
            }
            // If per-function failed, fall through to
            // full re-lower below.
        }
    }
    // Bitmask says at least one block is dirty (or
    // per-function dispatch didn't apply / #2181 force full) → do a
    // full re-lower. The future per-block re-lower
    // (cycle 4+) will route only the dirty blocks
    // through lowering; today we still re-lower the
    // whole function bundle.
    metrics_.relower_full_called_count.fetch_add(1, std::memory_order_relaxed);
    // Issue #1639: full_relower_count (alias atomic — spec wants
    // the explicit metric name distinct from relower_full_called_count;
    // dual-write keeps both bumps consistent for dashboards that
    // prefer the spec name). Also bumps the hit-rate denominator
    // (relower_block_hit_rate_denominator_total) since this call
    // did not contribute to the hit-rate numerator.
    evaluator_.bump_full_relower_count();
    evaluator_.bump_relower_block_hit_rate(0, 1); // 0 incremental, 1 total attempt
    auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
    auto cache_bridge_ptr = ir_cache_bridge_.empty() ? nullptr : &ir_cache_bridge_;
    auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
    std::vector<std::string> cache_hits;
    const auto full_t0 = std::chrono::steady_clock::now();
    // Issue #2730: full re-lower must NOT lower a multi-define workspace
    // root under this name. That bundles sibling lambdas (score, …) into
    // `name`'s IR so the primary #0 body becomes the first sibling — mute
    // rebind success with wrong body on eval_ir direct calls. Prefer the
    // single-define `source` string (canonical from unparse / rebind);
    // fall back to scoping flat.root to expanded_root when source empty.
    aura::ast::FlatAST* lower_flat = &flat;
    aura::ast::StringPool* lower_pool = &pool;
    const aura::ast::NodeId saved_root = flat.root;
    bool restore_root = false;
    // Lower straight from the live workspace Define when the caller
    // gave us its node: re-parsing the canonical text allocates a temp
    // FlatAST + StringPool from the arena on every relower (O(heap)
    // safepoint work).
    aura::ast::NodeId ws_define_root = aura::ast::NULL_NODE;
    if (!source.empty() && expanded_root != aura::ast::NULL_NODE && expanded_root < flat.size() &&
        !flat.is_free_slot(expanded_root)) {
        const auto ev = flat.get(expanded_root);
        if (ev.tag == aura::ast::NodeTag::Define) {
            ws_define_root = expanded_root;
        } else if (ev.tag == aura::ast::NodeTag::Lambda) {
            const auto par = flat.parent_of(expanded_root);
            if (par != aura::ast::NULL_NODE && par < flat.size() &&
                flat.get(par).tag == aura::ast::NodeTag::Define) {
                const auto want = pool.find_by_name(name);
                if (want && flat.sym_id(par) == *want)
                    ws_define_root = par;
            }
        }
    }
    if (ws_define_root != aura::ast::NULL_NODE) {
        flat.root = ws_define_root;
        restore_root = true;
        metrics_.relower_from_workspace_total.fetch_add(1, std::memory_order_relaxed);
    } else if (!source.empty()) {
        auto alloc = arena_.allocator();
        aura::ast::StringPool* tmp_pool = nullptr;
        aura::ast::FlatAST* tmp_flat = nullptr;
        // Issue #4070: EXEMPT — stack void** dies before the next Moving rewrite.
        tmp_pool = arena_.create_with_cover<aura::ast::StringPool>(
            nullptr, "stack-cover-dies-at-return", alloc);
        tmp_flat = arena_.create_with_cover<aura::ast::FlatAST>(
            nullptr, "stack-cover-dies-at-return", alloc);
        if (tmp_pool && tmp_flat) {
            auto pr = aura::parser::parse_to_flat(source, *tmp_flat, *tmp_pool);
            if (pr.success && pr.root != aura::ast::NULL_NODE) {
                tmp_flat->root = pr.root;
                // Multi-define source strings (legacy set-code): pick named define.
                const auto defs =
                    aura::compiler::find_top_level_defines(*tmp_flat, *tmp_pool, pr.root);
                for (const auto& [n, id] : defs) {
                    if (n == name) {
                        tmp_flat->root = id;
                        break;
                    }
                }
                lower_flat = tmp_flat;
                lower_pool = tmp_pool;
            }
        }
    }
    if (!restore_root && lower_flat == &flat && expanded_root != aura::ast::NULL_NODE &&
        expanded_root < flat.size()) {
        flat.root = expanded_root;
        restore_root = true;
    }
    auto ir_mod = lower_to_ir_with_cache_tracked(
        *lower_flat, *lower_pool, arena_, cache_ptr, &cache_hits, &evaluator_.primitives(),
        cache_bridge_ptr, cache_strings_ptr, &name, &type_registry_, value_cells_for_lowering());
    if (restore_root)
        flat.root = saved_root;
    // Issue #2112: feed adaptive threshold history for full path.
    {
        const auto full_ns =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - full_t0)
                                           .count());
        note_full_relower_cost_ns(full_ns);
        metrics_.avg_full_relower_cost_ns.store(avg_full_relower_cost_ns(),
                                                std::memory_order_relaxed);
        metrics_.full_relower_cost_samples.store(
            full_relower_cost_samples_atomic().load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        metrics_.partial_vs_full_win_ratio_bp.store(partial_vs_full_win_ratio_bp(),
                                                    std::memory_order_relaxed);
        metrics_.partial_relower_threshold_used.store(get_partial_relower_threshold(),
                                                      std::memory_order_relaxed);
    }
    // Issue #4436: full lower copies ir_cache_[callee] without reading v2.
    // Dirty, unstored, or abort-invalid: leave the caller dirty and skip
    // the store. Production / Full only. Soft / Off keeps the copy.
    // embed_safe_callees_4436_ was clean and content-stored; the
    // precompute mark does not rewrite irs.
    if (aura::compiler::typed_audit::production_defaults_active() ||
        aura::compiler::typed_audit::get_strategy() ==
            aura::compiler::typed_audit::AuditStrategy::Full) {
        it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return false;
        for (const auto& hit : cache_hits) {
            if (hit == name)
                continue;
            const auto cit = ir_cache_v2_.find(hit);
            if (cit == ir_cache_v2_.end()) {
                // Issue #4528: a v1 cache hit whose v2 row is absent is
                // the same orphan family as a dirty row (eviction used to
                // erase only the v2 row, and cache_define fills v1 with no
                // store_define_v2 at all). Refuse the store — leave the
                // caller dirty / unstored until a lower that did not
                // embed the orphan v1. embed_safe keeps its cone
                // exemption.
                const bool embed_safe_orphan4528 =
                    std::find(embed_safe_callees_4436_.begin(), embed_safe_callees_4436_.end(),
                              hit) != embed_safe_callees_4436_.end();
                if (!embed_safe_orphan4528) {
                    it->second.dirty = true;
                    it->second.content_stored_this_epoch = false;
                    return false;
                }
                continue;
            }
            const auto& ce = cit->second;
            if (ce.dirty || ce.dirty_block_count() > 0 || ce.abort_map_invalid ||
                !ce.content_stored_this_epoch) {
                const bool embed_safe =
                    std::find(embed_safe_callees_4436_.begin(), embed_safe_callees_4436_.end(),
                              hit) != embed_safe_callees_4436_.end();
                if (embed_safe)
                    continue;
                it->second.dirty = true;
                it->second.content_stored_this_epoch = false;
                return false;
            }
        }
        // Issue #4513: the abort walk drops the walked names' v1
        // bundles, so a full lower can reference a callee whose v2 row
        // is untrusted without producing a cache hit (the lowerer
        // emits ConstI64 0 for a missing cache). Scan the lowered
        // body's define references with the same freshness predicate
        // as the hit loop above and refuse the store while any of
        // them is untrusted. Value cells (TopCellLoad — a live read,
        // no embed), primitives, params / let / self and quoted data
        // are not embeds and stay exempt. embed_safe_callees_4436_
        // keeps its cone exemption.
        {
            aura::ast::NodeId scan_lambda = aura::ast::NULL_NODE;
            if (lower_flat->root != aura::ast::NULL_NODE && lower_flat->root < lower_flat->size()) {
                auto scan_root_v = lower_flat->get(lower_flat->root);
                if (scan_root_v.tag == aura::ast::NodeTag::Define && !scan_root_v.children.empty())
                    scan_lambda = scan_root_v.child(0);
                else if (scan_root_v.tag == aura::ast::NodeTag::Lambda)
                    scan_lambda = lower_flat->root;
            }
            if (scan_lambda != aura::ast::NULL_NODE && scan_lambda < lower_flat->size() &&
                lower_flat->get(scan_lambda).tag == aura::ast::NodeTag::Lambda) {
                const auto& scan_lv = lower_flat->get(scan_lambda);
                std::unordered_set<std::string> scan_params;
                for (auto pid : scan_lv.params)
                    scan_params.insert(std::string(lower_pool->resolve(pid)));
                struct UntrustedCalleeScan4513 {
                    const aura::ast::FlatAST& f;
                    const aura::ast::StringPool& p;
                    const std::string& self_name;
                    const std::unordered_set<std::string>& param_names;
                    const Evaluator& eval;
                    const std::unordered_map<std::string, std::size_t,
                                             aura::core::TransparentStringHash, std::equal_to<>>&
                        value_cells;
                    // Issue #4681: call-slot names lower to the same
                    // live TopCellLoad - no embedded copy to go stale.
                    const aura::compiler::CallSlotCells4681& call_slots;
                    const std::unordered_map<std::string, IRCacheEntry,
                                             aura::core::TransparentStringHash, std::equal_to<>>&
                        ir_cache_v2;
                    // Issue #4528: tie-break a missing v2 row — a name
                    // that still holds a v1 bundle is a define whose v2
                    // row is gone, not an unknown face.
                    const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>,
                                             aura::core::TransparentStringHash, std::equal_to<>>&
                        ir_cache_v1;
                    const std::vector<std::string>& embed_safe;
                    std::unordered_set<std::string> let_bound;
                    bool untrusted = false;
                    void walk(aura::ast::NodeId id) {
                        if (untrusted || id == aura::ast::NULL_NODE || id >= f.size())
                            return;
                        auto nv = f.get(id);
                        if (nv.tag == aura::ast::NodeTag::Quote)
                            return;
                        if (nv.tag == aura::ast::NodeTag::Let ||
                            nv.tag == aura::ast::NodeTag::LetRec) {
                            auto nm = std::string(p.resolve(nv.sym_id));
                            if (!nv.children.empty())
                                walk(nv.child(0));
                            let_bound.insert(nm);
                            if (nv.children.size() >= 2)
                                walk(nv.child(1));
                            let_bound.erase(nm);
                            return;
                        }
                        if (nv.tag == aura::ast::NodeTag::Variable) {
                            auto var_name = std::string(p.resolve(nv.sym_id));
                            if (param_names.count(var_name) || let_bound.count(var_name))
                                return;
                            if (var_name == self_name)
                                return;
                            if (eval.primitives().slot_for_name(var_name) <
                                eval.primitives().slot_count())
                                return;
                            if (value_cells.count(var_name) > 0)
                                return; // TopCellLoad face — live read, no embed.
                            // Issue #4681: a call-slot reference lowers to
                            // TopCellLoad of the define's env cell, so it
                            // embeds nothing the #4436 freshness gate owns.
                            if (call_slots.count(var_name) > 0)
                                return;
                            if (std::find(embed_safe.begin(), embed_safe.end(), var_name) !=
                                embed_safe.end())
                                return; // #4436 cone exemption.
                            const auto cit = ir_cache_v2.find(var_name);
                            if (cit == ir_cache_v2.end()) {
                                // Issue #4528: distinguish a missing v2 row
                                // for a name that still holds a v1 bundle —
                                // an eviction / cache_define-only orphan in
                                // the same untrusted family as a dirty v2
                                // row — from a name that is not a define at
                                // all (the lowerer emits ConstI64 0 for
                                // those: nothing is embedded).
                                if (ir_cache_v1.count(var_name) > 0)
                                    untrusted = true;
                                return; // not a define — unknown face, no embed.
                            }
                            const auto& ce = cit->second;
                            if (ce.dirty || ce.dirty_block_count() > 0 || ce.abort_map_invalid ||
                                !ce.content_stored_this_epoch)
                                untrusted = true;
                            return;
                        }
                        for (auto c : nv.children)
                            walk(c);
                    }
                };
                UntrustedCalleeScan4513 scan4513{*lower_flat,
                                                 *lower_pool,
                                                 name,
                                                 scan_params,
                                                 evaluator_,
                                                 ir_value_cell_bindings_,
                                                 ir_call_slots_4681_,
                                                 ir_cache_v2_,
                                                 ir_cache_,
                                                 embed_safe_callees_4436_};
                scan4513.walk(scan_lv.children.empty() ? aura::ast::NULL_NODE : scan_lv.child(0));
                if (scan4513.untrusted) {
                    it->second.dirty = true;
                    it->second.content_stored_this_epoch = false;
                    return false;
                }
            }
        }
    }
    // Run per-function passes on the new bundle.
    // Issue #1574 / #2044 / #2907: shared incremental dirty suite + SoA hot pack.
    {
        auto& entry = it->second;
        DefineDirtyMaskView define_mask;
        std::vector<std::vector<std::uint32_t>> instr_counts;
        if (!entry.block_dirty_per_func_.empty()) {
            define_mask.block_dirty_per_func = &entry.block_dirty_per_func_;
            define_mask.instruction_dirty_per_func = &entry.instruction_dirty_per_func_;
            // Issue #2133: absolute↔in-block conversion for instr peel.
            instr_counts = build_block_instr_counts_(entry);
            define_mask.block_instr_counts = &instr_counts;
        }
        const DefineDirtyMaskView* mask_ptr =
            define_mask.block_dirty_per_func ? &define_mask : nullptr;
        // Issue #4254: under dual-emit, this lower's SoA lives in
        // pending_soa_snapshot_ until store_define_v2. Passing
        // entry.soa_mod (pre-re-lower) lets prod_soa hot pack +
        // sync_soa_dirty_blocks_into_aos peel stale SoA into the
        // fresh AoS. Prefer pending when present.
        IRModuleV2* soa_for_suite = &entry.soa_mod;
        if (pending_soa_snapshot_ && !pending_soa_snapshot_->module.functions.empty())
            soa_for_suite = &pending_soa_snapshot_->module;
        const auto clean_blocks_skipped =
            run_incremental_dirty_pass_suite_(ir_mod, mask_ptr, soa_for_suite);
        if (!entry.block_dirty_per_func_.empty()) {
            metrics_.linear_post_mutate_enforcements_total.fetch_add(1, std::memory_order_relaxed);
        }
        if (clean_blocks_skipped > 0) {
            metrics_.irsoa_cache_miss_reduction.fetch_add(clean_blocks_skipped,
                                                          std::memory_order_relaxed);
        }
    }
    // Extract the non-entry functions as the bundle.
    std::vector<aura::ir::IRFunction> bundle;
    std::vector<aura::ir::ClosureBridgeData> bridge_bundle;
    for (auto& func : ir_mod.functions) {
        if (func.id != ir_mod.entry_function_id) {
            const auto fid = func.id; // read before the move below
            bundle.push_back(std::move(func));
            if (fid < ir_mod.closure_bridge.size())
                bridge_bundle.push_back(ir_mod.closure_bridge[fid]);
            else
                bridge_bundle.emplace_back();
        }
    }
    // Store in v2 cache first (rebuilds the bitmask +
    // clears all dirty bits via store_define_v2's
    // bookkeeping, and takes ownership of the bundle).
    store_define_v2(name, std::string(source), std::move(bundle), std::move(bridge_bundle),
                    ir_mod.string_pool, ir_sample_epoch);
    // Mirror to v1 caches (legacy path). Read from v2
    // so we don't keep two separate copies of the IR
    // — v1 is a thin view onto the same data.
    auto vit = ir_cache_v2_.find(name);
    if (vit != ir_cache_v2_.end()) {
        ir_cache_[name] = vit->second.irs;
        ir_cache_bridge_[name] = vit->second.bridges;
        ir_cache_strings_[name] = vit->second.strings;
    }
    // Mirror source bookkeeping.
    function_sources_[name] = std::string(source);
    // Record dependencies for dep_graph_.
    for (auto& called_name : cache_hits) {
        record_dependency(name, called_name);
    }
    note_slot_edges_4681_(name);
    return true;
}

bool CompilerService::relower_define_function(const std::string& name, std::size_t func_idx,
                                              aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                              aura::ast::NodeId lambda_node_id) {
    IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
    auto it = ir_cache_v2_.find(name);
    if (it == ir_cache_v2_.end())
        return false;
    auto& entry = it->second;
    if (func_idx >= entry.irs.size())
        return false;
    // Re-lower just this one function.
    auto new_func = aura::compiler::lower_function_at(flat, pool, arena_, lambda_node_id,
                                                      &evaluator_.primitives());
    if (new_func.blocks.empty()) {
        // Lowering returned empty — fall back to full re-lower.
        return false;
    }
    // Capture dirty-block mask before clearing (Issue #611).
    std::span<const std::uint8_t> dirty_blocks;
    std::vector<std::uint8_t> dirty_copy;
    if (func_idx < entry.block_dirty_per_func_.size()) {
        dirty_copy = entry.block_dirty_per_func_[func_idx];
        dirty_blocks = dirty_copy;
    }
    // Run per-function passes (mirrors cache_define).
    // Issue #1574: define-level dirty mask drives incremental opt.
    {
        aura::compiler::ComputeKindWrap ck_pass;
        aura::compiler::ConstantFoldingWrap cf_pass;
        aura::compiler::TypePropagationPass tp_pass;
        aura::compiler::ShapeWrap shape_pass;
        aura::ir::IRModule one;
        one.functions.push_back(std::move(new_func));
        std::vector<std::vector<std::uint8_t>> single_mask;
        std::vector<std::vector<std::uint8_t>> inst_mask;
        std::vector<std::vector<std::uint32_t>> instr_counts;
        aura::compiler::DefineDirtyMaskView define_mask;
        if (!dirty_copy.empty()) {
            single_mask.push_back(dirty_copy);
            define_mask.block_dirty_per_func = &single_mask;
            if (func_idx < entry.instruction_dirty_per_func_.size()) {
                inst_mask.push_back(entry.instruction_dirty_per_func_[func_idx]);
                define_mask.instruction_dirty_per_func = &inst_mask;
            }
            // Issue #2133: single-func instr counts for peel.
            if (func_idx < entry.irs.size()) {
                instr_counts.resize(1);
                instr_counts[0].resize(entry.irs[func_idx].blocks.size());
                for (std::size_t bi = 0; bi < entry.irs[func_idx].blocks.size(); ++bi)
                    instr_counts[0][bi] = static_cast<std::uint32_t>(
                        entry.irs[func_idx].blocks[bi].instructions.size());
                define_mask.block_instr_counts = &instr_counts;
            }
        }
        const aura::compiler::DefineDirtyMaskView* mask_ptr =
            define_mask.block_dirty_per_func ? &define_mask : nullptr;
        (void)aura::compiler::run_production_incremental_dirty_pipeline(one, ck_pass, mask_ptr);
        (void)aura::compiler::run_production_incremental_dirty_pipeline(one, cf_pass, mask_ptr);
        (void)aura::compiler::run_production_incremental_dirty_pipeline(one, tp_pass, mask_ptr);
        (void)aura::compiler::run_production_incremental_dirty_pipeline(one, shape_pass, mask_ptr);
        new_func = std::move(one.functions[0]);
        // Issue #538 / #611: DCE after per-function post-mutate
        // re-lower; scoped to dirty blocks when mask matches.
        run_coercion_elim_on_function(new_func, dirty_blocks);
    }
    // Bump the per-function re-lower counter.
    metrics_.relower_per_function_called_count.fetch_add(1, std::memory_order_relaxed);
    evaluator_.bump_partial_relower_count();
    // Replace the function in the v2 cache. Preserve the
    // original func_id so MakeClosure operands in callers
    // (which reference the old func_id) keep working.
    const auto old_func_id = entry.irs[func_idx].id;
    new_func.id = old_func_id;
    // Issue #1474 / #2109: selective copy.
    //   - Block-level: only dirty blocks replaced when shapes match.
    //   - Instruction-level (#2109): inside a dirty block, if
    //     instruction_dirty_ has matching layout, only dirty
    //     instructions are re-emitted; clean slots keep old IR
    //     (relower_partial_insts_saved_total / skip metrics).
    //   - Empty/desynced instruction_dirty_ → block-level fallback.
    std::size_t blocks_replaced = 0;
    std::size_t insts_saved = 0;
    std::size_t insts_emitted = 0;
    if (func_idx < entry.block_dirty_per_func_.size()) {
        auto& dirty_mask = entry.block_dirty_per_func_[func_idx];
        if (dirty_mask.size() == new_func.blocks.size() &&
            dirty_mask.size() == entry.irs[func_idx].blocks.size()) {
            auto* idf = (func_idx < entry.instruction_dirty_per_func_.size())
                            ? &entry.instruction_dirty_per_func_[func_idx]
                            : nullptr;
            std::size_t abs_base = 0;
            for (std::size_t bi = 0; bi < new_func.blocks.size(); ++bi) {
                const auto old_n = entry.irs[func_idx].blocks[bi].instructions.size();
                const auto new_n = new_func.blocks[bi].instructions.size();
                if (dirty_mask[bi]) {
                    // Prefer instruction-level peel when shapes match
                    // and idf covers this block's absolute range.
                    const bool can_peel_instr =
                        idf && old_n == new_n && old_n > 0 && (abs_base + old_n) <= idf->size();
                    if (can_peel_instr) {
                        auto& old_insts = entry.irs[func_idx].blocks[bi].instructions;
                        auto& new_insts = new_func.blocks[bi].instructions;
                        bool any_dirty_inst = false;
                        for (std::size_t ii = 0; ii < old_n; ++ii) {
                            if ((*idf)[abs_base + ii]) {
                                old_insts[ii] = std::move(new_insts[ii]);
                                (*idf)[abs_base + ii] = 0;
                                ++insts_emitted;
                                any_dirty_inst = true;
                            } else {
                                ++insts_saved;
                                metrics_.relower_instruction_skip_total.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                        }
                        if (any_dirty_inst || insts_saved > 0)
                            ++blocks_replaced;
                    } else {
                        entry.irs[func_idx].blocks[bi] = std::move(new_func.blocks[bi]);
                        entry.invalidate_irs_layout_(); // Issue #4615
                        ++blocks_replaced;
                        // Clear instr dirty for this block range.
                        if (idf) {
                            for (std::size_t ii = 0; ii < old_n && abs_base + ii < idf->size();
                                 ++ii)
                                (*idf)[abs_base + ii] = 0;
                        }
                    }
                    dirty_mask[bi] = 0;
                }
                abs_base += old_n;
            }
        } else {
            // Shape mismatch — fall back to full replace.
            blocks_replaced = new_func.blocks.size();
            entry.irs[func_idx] = std::move(new_func);
            entry.invalidate_irs_layout_(); // Issue #4615
            for (auto& b : dirty_mask)
                b = 0;
        }
    } else {
        // No bitmask — fall back to full replace.
        blocks_replaced = new_func.blocks.size();
        entry.irs[func_idx] = std::move(new_func);
        entry.invalidate_irs_layout_(); // Issue #4615
    }
    // Issue #1474: bump per-block replaced counter.
    metrics_.incremental_relower_blocks_total.fetch_add(blocks_replaced, std::memory_order_relaxed);
    // Issue #2109: instruction-level savings observability.
    if (insts_saved > 0) {
        metrics_.relower_partial_insts_saved_total.fetch_add(insts_saved,
                                                             std::memory_order_relaxed);
    }
    // Issue #1514 / #2109: clear remaining instruction-level dirty bits
    // for this function so partial re-lower state stays coherent.
    if (func_idx < entry.instruction_dirty_per_func_.size()) {
        for (auto& b : entry.instruction_dirty_per_func_[func_idx])
            b = 0;
    }
    // Also clear the entry.dirty flag if this was the only
    // dirty function.
    if (entry.dirty_block_count() == 0) {
        entry.dirty = false;
    }
    // Mirror to v1 cache. cache_define and the post-store mirror
    // both drop __top__, so production v1 and v2 share an index.
    // Issue #4311: write [func_idx] for that shape. A leading
    // __top__ on only one side keeps the historical func_idx-1 map
    // (v2 prefix / v1 dropped, or the reverse).
    auto v1_it = ir_cache_.find(name);
    if (v1_it != ir_cache_.end() && !v1_it->second.empty() && !entry.irs.empty()) {
        const bool v1_top = v1_it->second[0].name == "__top__";
        const bool v2_top = entry.irs[0].name == "__top__";
        if (v1_top != v2_top) {
            if (func_idx > 0 && (func_idx - 1) < v1_it->second.size())
                v1_it->second[func_idx - 1] = entry.irs[func_idx];
        } else if (func_idx < v1_it->second.size()) {
            v1_it->second[func_idx] = entry.irs[func_idx];
        }
    }
    // Issue #2045: patch source_to_ir_map for the re-lowered function
    // so next impact_scope uses the new layout (no under-invalidation).
    rebuild_or_patch_source_to_ir_map_(entry, func_idx);
    return true;
}

} // namespace aura::compiler
