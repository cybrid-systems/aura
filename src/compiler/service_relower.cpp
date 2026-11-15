// service_relower.cpp — CompilerService::relower_dirty_defines_from_workspace
// moved out of the module interface (#5242). Move-only: the body is
// unchanged.

module;

#include "observability_metrics.h"         // note_relower_fallback / RelowerFallbackReason
#include "compiler/typed_mutation_audit.h" // typed_audit::
#include "core/workspace_epoch.hh"         // current_mutation_epoch
#include "lock_order_audit.h"              // lock_order

module aura.compiler.service;

import std;

namespace aura::compiler {

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

} // namespace aura::compiler
