// service_dirty.cpp — Wave 5/6: CompilerService dirty-mark / cascade / invalidate out-of-line
// aura.compiler.service module partition (first service implementation unit).
//
// mark_define_dirty, mark_all_defines_dirty, and invalidate_function (hard
// invalidate + cascade re-lower) leave service.ixx so the interface unit
// stays thinner. Declarations remain on CompilerService.

module;

#include "lock_order_audit.h"
#include "observability_metrics.h"
#include "jit_typed_mutation_stats.h" // ir_soa_migration::record_capture_dirty_mark
#include "aura_jit.h"
#include "aura_jit_bridge.h"               // aura_reemit_aot_for_dirty (#2035)
#include "hot_update_registry.hh"          // HotUpdateRegistry notify + region mask (#2035)
#include "core/transparent_string_hash.hh" // TransparentStringHash for ir_cache_index
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

module aura.compiler.service;

import std;

namespace aura::compiler {

// ── Issue #2035: HotUpdateRegistry cascade notify + region-mask reemit ──
void CompilerService::notify_hot_update_after_cascade_(const std::string& name,
                                                       const std::vector<std::string>& dependents) {
    auto& reg = hot_update_registry();
    // Always fan-out dirty listeners (agents / plugins / tests).
    reg.notify_dirty_define(name.c_str());
    std::uint64_t mask = 0;
    if (auto it = ir_cache_v2_.find(name); it != ir_cache_v2_.end())
        mask |= it->second.compute_region_mask_from_dirty();
    for (const auto& d : dependents) {
        if (d.empty() || d == name)
            continue;
        reg.notify_dirty_define(d.c_str());
        if (auto it = ir_cache_v2_.find(d); it != ir_cache_v2_.end())
            mask |= it->second.compute_region_mask_from_dirty();
    }
    // Body-only / soft dirty with empty bitmasks still needs a selective bit.
    if (mask == 0) {
        if (auto it = ir_cache_v2_.find(name); it != ir_cache_v2_.end() && it->second.dirty)
            mask = (1ULL << 1);
    }
    if (mask != 0) {
        // Strips Evolution bit (1<<2) inside aura_set_aot_emit_region_mask.
        reg.set_emit_region_mask(mask);
        reg.on_region_mask_from_dirty(mask);
    }
    // Selective AOT re-emit only when the host wired a reemit candidate provider.
    // Stable func ids are preserved inside aura_reemit_aot_for_dirty on success.
    if (reg.reemit_provider_wired()) {
        reg.on_cascade_reemit_trigger();
        const auto n = aura_reemit_aot_for_dirty(evaluator_.defuse_version());
        // Always count the cascade-driven reemit attempt (#1640 / #2035).
        metrics_.aot_incremental_reemit_triggered.fetch_add(1, std::memory_order_relaxed);
        if (n > 0)
            metrics_.commercial_reemits_total.fetch_add(n, std::memory_order_relaxed);
    }
}

// ── mark_define_dirty (#1476 / #1523 / #1627 / #1505) ─────────────────────
void CompilerService::mark_define_dirty(const std::string& name) {
    // Issue #1476 + #1523: unify dirty mark + dual-epoch; acquire
    // mutate FIRST when safe (skip if would invert lock order).
    using aura::compiler::lock_order::Level;
    using aura::compiler::lock_order::OrderedUniqueLock;
    OrderedUniqueLock<std::shared_mutex> mutate_guard;
    if (!lock_order::is_held(Level::Mutate)) {
        if (lock_order::is_held(Level::Workspace) || lock_order::is_held(Level::EnvFrames) ||
            lock_order::is_held(Level::DepGraph)) {
            metrics_.lock_inversion_detected_total.fetch_add(1, std::memory_order_relaxed);
            lock_order::g_lock_inversion_detected_total.fetch_add(1, std::memory_order_relaxed);
        } else {
            mutate_guard = OrderedUniqueLock<std::shared_mutex>(mutate_mtx_, Level::Mutate);
            sync_lock_order_metrics_();
        }
    }

    // Issue #1627: soft-path pre-cascade parity with invalidate_function
    // (live closures + linear + GC root audit before epoch publish).
    prepare_unified_invalidation_pre_cascade_(name);

    // Issue #1261 / #1476 / #1627: bump both epochs via unified helper.
    atomic_bump_epochs_and_stamp_bridge(name);

    auto it = ir_cache_v2_.find(name);
    if (it != ir_cache_v2_.end()) {
        auto& primary = it->second;
        primary.dirty = true;
        // Issue #1495 / #1505 / #1506: prefer body-only dirty so partial
        // re-lower wins on set-body.
        // Shapes:
        //   - synthetic / dual: irs[0]=__top__, irs[1]=body
        //   - real lower bundle: only non-entry funcs → body at irs[0]
        // Nested (irs[2..N] or >1 with free-ref self): free-var scan.
        const bool nested_primary = primary.irs.size() > 2;
        // Issue #1915: unified body-only dirty stamp (partial re-lower path).
        const auto body_blocks = primary.mark_body_only_dirty();
        if (body_blocks > 0 && !primary.irs.empty()) {
            // Issue #1505 / #1625: free-var + per-block targeted dirty
            // of nested lambdas for self (not whole nested fn).
            if (nested_primary) {
                for (std::size_t fi = 2; fi < primary.irs.size(); ++fi)
                    (void)mark_nested_lambda_blocks_targeted(primary, fi, name);
                metrics_.dep_graph_nested_lambda_targeted_dirty_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
            metrics_.cascade_body_only_count.fetch_add(1, std::memory_order_relaxed);
            metrics_.selfevo_instr_dirty_total.fetch_add(1, std::memory_order_relaxed);
            metrics_.dirty_propagation_block_marks.fetch_add(body_blocks,
                                                             std::memory_order_relaxed);
        } else {
            primary.mark_all_blocks_dirty();
            // Issue #946/#950 Phase 1: instruction dirty bitmask.
            primary.mark_all_instruction_dirty();
            metrics_.selfevo_instr_dirty_total.fetch_add(1, std::memory_order_relaxed);
            metrics_.dirty_propagation_full_func_marks.fetch_add(1, std::memory_order_relaxed);
            if (nested_primary) {
                metrics_.dep_graph_nested_lambda_full_dirty.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Issue #598 / #1494: post-mutate linear runtime enforcement
        // on mutate:rebind / set-body paths (ir_cache_v2 dirty).
        // Scan Moved captures so long-lived closures cannot apply
        // through stale linear EnvFrame state after dirty mark.
        metrics_.linear_post_mutate_enforcements_total.fetch_add(1, std::memory_order_relaxed);
        metrics_.selfevo_linear_enforce_total.fetch_add(1, std::memory_order_relaxed);
        (void)evaluator_.scan_live_closures_for_linear_captures(
            /*mark_invalid=*/true, /*only_if_moved=*/true);
        // Issue #1920 / #1046: closure capture dirty tracking —
        // free_vars (captures) on nested / body IR force SoA block
        // dirty so partial re-lower revisits capture sites.
        bool has_captures = false;
        for (const auto& irf : primary.irs) {
            if (!irf.free_vars.empty()) {
                has_captures = true;
                break;
            }
        }
        if (has_captures || primary.irs.size() > 1) {
            aura::compiler::ir_soa_migration::record_capture_dirty_mark(1);
            for (auto& sfn : primary.soa_mod.functions)
                sfn.mark_all_blocks_dirty();
        }
        // Issue #2034: force SoA instruction_dirty_ after primary cascade.
        finish_cascade_soa_dirty_sync_(primary);
    }
    // Cascade: BFS over called_by. Use std::queue (FIFO) for proper BFS
    // ordering — vector-as-stack is technically DFS, which is fine for
    // correctness but std::queue is more idiomatic and self-documenting.
    //
    // Issue #224 cycle 4: dep_graph_-aware cascade. For each
    // dependent that we reach via the BFS, we know via the
    // dep_graph_ that the dependent *calls* the mutated
    // function (the edge `dependent → name` exists in
    // dep_graph_[dependent].calls). The CALL is in the
    // dependent's body Lambda (irs[1] in the entry, by
    // convention — irs[0] is the __top__ entry function).
    // Nested lambdas in the dependent (irs[2..N]) are
    // self-contained; they don't reference the mutated
    // function, so their blocks don't need re-lowering.
    //
    // Cycle-4 win: for a dependent with K nested lambdas,
    // we mark only the body function's blocks dirty (not
    // all functions in the entry). When the bitmask
    // consumer (relower_define_blocks) sees this, the
    // re-lower-define-function path can re-lower just
    // irs[1] and leave the nested lambdas alone.
    //
    // Fallback: if the convention doesn't hold (e.g., the
    // dependent has 0 or 1 IRFunction, or the body is at
    // a different index), we conservatively mark all
    // blocks dirty. This preserves correctness; the cycle-4
    // win is "typical define bodies" (single body Lambda,
    // no nested lambdas → no fallback needed).
    //
    // Issue #1261: when dependent has nested lambdas (irs.size()>2)
    // OR macro-hygiene markers on the workspace define, force full
    // dirty so defuse_version_ + hygiene edges do not under-invalidate.
    std::queue<std::string> bfs;
    std::unordered_set<std::string> visited;
    std::vector<std::string> cascade_dependents; // Issue #2035: hot-update fan-out
    bfs.push(name);
    visited.insert(name);
    std::size_t depth = 0;
    while (!bfs.empty()) {
        ++depth;
        auto cur = bfs.front();
        bfs.pop();
        std::vector<std::string> called_by_snap;
        {
            // Issue #1523: dep_graph is LAST in canonical order
            // (mutate already held or intentionally skipped).
            lock_order::OrderedSharedLock<std::shared_mutex> dep_read(dep_graph_mtx_,
                                                                      Level::DepGraph);
            auto dit = dep_graph_.find(cur);
            if (dit == dep_graph_.end())
                continue;
            called_by_snap = dit->second.called_by;
        }
        for (auto& dependent : called_by_snap) {
            if (!visited.insert(dependent).second)
                continue;
            bfs.push(dependent);
            cascade_dependents.push_back(dependent);
            // Issue #1476: per-dependent atomic bump (closure
            // captures for the dependent need new epoch too —
            // paired with the helper that pairs with #1475's
            // is_bridge_stale / is_env_frame_stale dual check).
            atomic_bump_epochs_and_stamp_bridge(dependent);
            auto cit = ir_cache_v2_.find(dependent);
            if (cit == ir_cache_v2_.end())
                continue;
            auto& centry = cit->second;
            const bool nested_lambdas = centry.irs.size() > 2;
            // Issue #1514 / #1505: dep_graph_-aware cascade for
            // dependents. Convention: irs[0]=__top__, irs[1]=body.
            // The CALL to `cur` lives in the body → mark body blocks.
            // Nested lambdas (irs[2..N]) are only marked when their
            // free_vars free-reference `cur` (the mutated/cascaded
            // name) — not a full-entry dirty. Falls back to full
            // dirty only when body bitmasks are missing.
            if (centry.irs.size() >= 2 && 1 < centry.block_dirty_per_func_.size()) {
                centry.dirty = true;
                if (centry.block_dirty_per_func_.size() < centry.irs.size())
                    centry.block_dirty_per_func_.resize(centry.irs.size());
                // Body (call site of `cur`).
                for (auto& b : centry.block_dirty_per_func_[1]) {
                    b = 1;
                }
                // Issue #1505 / #1625: free-var + per-block targeted
                // dirty of nested lambdas. Match against `cur`
                // (immediate cascade predecessor). Only blocks that
                // reference the name (or entry_block fallback) are
                // marked — not the whole nested function.
                if (nested_lambdas) {
                    bool any_nested_targeted = false;
                    for (std::size_t fi = 2; fi < centry.irs.size(); ++fi) {
                        if (mark_nested_lambda_blocks_targeted(centry, fi, cur) > 0)
                            any_nested_targeted = true;
                    }
                    if (any_nested_targeted) {
                        metrics_.dep_graph_nested_lambda_targeted_dirty_total.fetch_add(
                            1, std::memory_order_relaxed);
                    } else {
                        // Nested present but none free-ref `cur` —
                        // still count as body-only targeted cascade
                        // (body marked above; nested kept clean).
                        metrics_.dep_graph_nested_lambda_targeted_dirty_total.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
                metrics_.cascade_body_only_count.fetch_add(1, std::memory_order_relaxed);
                // Issue #2034: AoS body bits set directly above —
                // force SoA instruction_dirty_ parity.
                finish_cascade_soa_dirty_sync_(centry);
            } else if (nested_lambdas) {
                // Fallback: no body bitmask → full dirty (pre-#1505).
                centry.dirty = true;
                centry.mark_all_blocks_dirty();
                finish_cascade_soa_dirty_sync_(centry);
                metrics_.cascade_full_count.fetch_add(1, std::memory_order_relaxed);
                metrics_.dep_graph_nested_lambda_full_dirty.fetch_add(1, std::memory_order_relaxed);
            } else {
                // Fallback: convention doesn't hold —
                // conservatively mark all blocks dirty.
                centry.dirty = true;
                centry.mark_all_blocks_dirty();
                finish_cascade_soa_dirty_sync_(centry);
                metrics_.cascade_full_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    // Issue #1476 / #1496 AC5: track invalidate_cascade_depth_max
    // via CAS + sum depth for avg (depth_total / protocol calls).
    const auto final_depth = static_cast<std::uint64_t>(depth);
    metrics_.invalidate_cascade_depth_total.fetch_add(final_depth, std::memory_order_relaxed);
    auto expected = metrics_.invalidate_cascade_depth_max.load(std::memory_order_relaxed);
    while (final_depth > expected &&
           !metrics_.invalidate_cascade_depth_max.compare_exchange_weak(expected, final_depth)) {
        // retry
    }
    metrics_.dep_graph_hygiene_propagate.fetch_add(1, std::memory_order_relaxed);
    // Issue #2035: HotUpdateRegistry dirty notify + region-mask reemit.
    notify_hot_update_after_cascade_(name, cascade_dependents);
}

// Mark all defines dirty. Called when (set-code ...) re-parses the whole
// workspace (which can change any define's body).
// Issue #196: also flips every block in every entry to dirty.

// ── mark_all_defines_dirty (#196 / #1999 Wave2 bulk invalidate) ───────────
void CompilerService::mark_all_defines_dirty() {
    // Issue #2026-07-17 (EDSL SIGSEGV audit, surgical fix):
    // Clear the cid→name map only. Reasoning:
    // - ir_define_closure_owner_ (cid→name) holds ClosureIds from
    //   the PREVIOUS workspace. After (set-code ...) replaces
    //   workspace, those cids point at closures backed by the OLD
    //   flat/pool (alive in arena but logically detached).
    //   dispatch_ir_define_closure(cid) finding a stale cid →
    //   use-after-free → SIGSEGV at +24.
    // - ir_define_env_bindings_ (name→binding) KEEPS bindings
    //   (with their interpreter, module, context) tied to the OLD
    //   workspace. When eval-current later re-caches defines,
    //   it re-uses or replaces these bindings via
    //   install_ir_define_env_binding, and adds fresh cid→name
    //   entries for the NEW workspace closures.
    // Earlier attempts (reverted): clearing BOTH maps removed
    // SIGSEGV but broke normal dispatch (new closures not found);
    // adding stale-check via binding->interpreter->flat/pool failed
    // to compile because IRInterpreter doesn't expose flat/pool.
    // Minimal surgical change: only ir_define_closure_owner_.clear().
    ir_define_closure_owner_.clear();
    // Issue #1999 / #600 AC5 + Wave2: set-code soft-dirty is the
    // invalidate path for a full workspace replace. Mark every
    // ir_cache_v2 entry dirty, then ONE bulk jit_.invalidate_all()
    // (not per-name invalidate+prefix — that was O(N·T) map walks
    // and N× linear-live scans). Still bumps jit-sync once so
    // query:incremental-closure-stats jit-sync-count observes redefine.
    std::size_t dirty_n = 0;
    std::vector<std::string> dirty_names;
    dirty_names.reserve(ir_cache_v2_.size());
    for (auto& [name, entry] : ir_cache_v2_) {
        entry.dirty = true;
        entry.mark_all_blocks_dirty();
        // Issue #2034: force SoA instruction_dirty_ on bulk invalidate.
        finish_cascade_soa_dirty_sync_(entry);
        dirty_names.push_back(name);
        ++dirty_n;
    }
    if (dirty_n > 0) {
        const auto evicted = jit_.invalidate_all();
        metrics_.jit_hotswap_invalidate_total.fetch_add(evicted > 0 ? evicted : dirty_n,
                                                        std::memory_order_relaxed);
        evaluator_.bump_incremental_closure_jit_sync();
        // Issue #2035: bulk set-code soft-dirty still notifies HotUpdateRegistry
        // (one region-mask reemit for the whole workspace replace).
        std::vector<std::string> rest;
        if (dirty_names.size() > 1)
            rest.assign(dirty_names.begin() + 1, dirty_names.end());
        notify_hot_update_after_cascade_(dirty_names.front(), rest);
    }
}


// ── invalidate_function (#59 / #1378 / #1476 / #1627) ─────────────────────
void CompilerService::invalidate_function(const std::string& name) {
    // Issue #59 Iter 3 + #1378: acquire the Mutation Lock FIRST so
    // epoch bump, block-dirty, BFS, and cache/JIT teardown are
    // atomic w.r.t. concurrent invalidate_function / mutate.
    // A mutate:* that triggers this must drain any in-flight compile
    // before erasing the cache entry, otherwise another fiber could
    // observe a half-erased state.
    //
    // Issue #166 historically bumped mutation_epoch_ BEFORE the lock
    // for "early visibility". That opened a multi-fiber re-entrancy
    // window (Issue #1378): another invalidate could interleave after
    // epoch publish but before dep_graph_ cleanup, producing
    // non-deterministic cascade topology. Epoch still uses
    // memory_order_release; readers load acquire (L739/L966/L1013).
    using aura::compiler::lock_order::Level;
    using aura::compiler::lock_order::OrderedUniqueLock;
    OrderedUniqueLock<std::shared_mutex> mutate_lock(mutate_mtx_, Level::Mutate);
    sync_lock_order_metrics_();

    // Issue #1545 / #1494 / #1606 / #1627: shared pre-cascade
    // (live closures + linear + GC root audit) — same helper as
    // mark_define_dirty soft path.
    prepare_unified_invalidation_pre_cascade_(name);

    // Issue #1496 / #1476: SINGLE dual-epoch + bridge stamp + JIT
    // soft-deopt protocol — same helper as mark_define_dirty.
    // Readers (apply_closure / aura_closure_call) that acquire-load
    // either domain see both advanced before hard JIT erase /
    // dep_graph teardown below. Replaces the historical hand-rolled
    // bump_bridge_epoch + defuse + aot sequence that could desync
    // with the soft path.
    atomic_bump_epochs_and_stamp_bridge(name);
    // Issue #531: bump closure_stale_refresh_count_ on
    // every invalidate_function — measures the closure
    // refresh frequency post-mutate. Stats-only
    // (relaxed-ordering); the follow-up wires the actual
    // IRClosure::invalidate_if_stale walk + the
    // bridge_epoch_hit_count_ bump in apply_closure.
    metrics_.closure_stale_refresh_count_.fetch_add(1, std::memory_order_relaxed);
    // Issue #401: lifetime counter for invalidate_function entry.
    // Bumped here (before the dep_graph_ walk) so the count is
    // observable even if the walk short-circuits on an empty graph.
    metrics_.invalidate_function_calls.fetch_add(1, std::memory_order_relaxed);
    // Issue #610: linear ownership JIT/closure refresh after
    // invalidate — pairs with closure_stale_refresh for the
    // post-mutate linear runtime contract path.
    metrics_.linear_deopt_on_invalidate_total.fetch_add(1, std::memory_order_relaxed);
    // Issue #598: post-mutate runtime enforcement hook on
    // invalidate_function — pairs with linear_deopt_on_invalidate
    // so GuardShape/linear state re-validates after re-lower.
    metrics_.linear_post_mutate_enforcements_total.fetch_add(1, std::memory_order_relaxed);
    // Issue #638: invalidate ShapeProfiler profiles so
    // GuardShape + linear_ownership_state re-specialize
    // after post-mutate shape/ownership change.
    invalidate_shape(name);

    // Issue #1286 / #1915: per-block dirty on ir_cache_v2_ for the
    // mutated function. Prefer body-only stamp so partial re-lower
    // wins (nested / __top__ stay clean) instead of always
    // mark_all_blocks_dirty (full-function degradation).
    if (auto vit = ir_cache_v2_.find(name); vit != ir_cache_v2_.end()) {
        const auto n = vit->second.mark_body_only_dirty();
        metrics_.invalidate_per_block_dirty_total.fetch_add(1, std::memory_order_relaxed);
        if (n > 0) {
            // body-only path: n is blocks of body; precision = block marks
            metrics_.dirty_propagation_block_marks.fetch_add(n, std::memory_order_relaxed);
        } else {
            vit->second.mark_all_blocks_dirty();
            metrics_.dirty_propagation_full_func_marks.fetch_add(1, std::memory_order_relaxed);
        }
        // Issue #2034: force SoA instruction_dirty_ after invalidate root.
        finish_cascade_soa_dirty_sync_(vit->second);
        // When body-only left other funcs clean, credit minimal scope.
        const auto dirty_fns = vit->second.dirty_func_count();
        const auto total_fns = vit->second.irs.size();
        if (total_fns > dirty_fns) {
            metrics_.minimal_recompile_clean_funcs_saved.fetch_add(total_fns - dirty_fns,
                                                                   std::memory_order_relaxed);
            metrics_.minimal_recompile_scope_samples.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Issue #401: real BFS over called_by chain.
    //
    // The previous implementation used std::vector + push_back/pop_back,
    // which is stack/DFS behaviour (LIFO). The misleading comment claimed
    // "natural BFS order" but the iteration order was depth-first, which
    // made the re-lower order depend on the hash-map iteration order of
    // std::unordered_map<string, DepEntry>::called_by. For AI multi-round
    // mutate:rebind flows, that meant dep_graph_ calls/called_by edges
    // recorded by record_dependency during re-lower could land in
    // different orders across runs, producing non-deterministic dep-graph
    // shape.
    //
    // Fix: use std::deque + push_back/pop_front for FIFO BFS, then sort
    // the dependents vector lexicographically before re-lower. Sorting
    // gives a stable iteration order regardless of the underlying
    // unordered_map bucket layout.
    //
    // Issue #1376: exclusive dep_graph_mtx_ for the BFS + erase window
    // (lock order: mutate_mtx_ already held, then dep_graph_mtx_).
    // Snapshot dependents under the lock so re-lower below can proceed
    // without holding the graph mutex across IR work.
    std::vector<std::string> dependents;
    {
        // Issue #1523: mutate already held → dep_graph LAST is legal.
        OrderedUniqueLock<std::shared_mutex> dep_write(dep_graph_mtx_, Level::DepGraph);
        sync_lock_order_metrics_();
        std::deque<std::string> bfs;
        std::unordered_set<std::string> visited;

        bfs.push_back(name);
        visited.insert(name);

        while (!bfs.empty()) {
            auto current = bfs.front();
            bfs.pop_front();

            auto it = dep_graph_.find(current);
            if (it == dep_graph_.end())
                continue;

            for (auto& dependent : it->second.called_by) {
                if (!visited.insert(dependent).second)
                    continue;
                dependents.push_back(dependent);
                bfs.push_back(dependent);
            }
        }

        // Issue #401: stable re-lower order. Sort dependents lexicographically
        // so the iteration below doesn't depend on the unordered_map hash
        // layout. This is the determinism contract for the follow-up
        // record_dependency edge-creation order.
        std::sort(dependents.begin(), dependents.end());

        // Issue #1496: cascade depth for hard invalidate (root + dependents).
        // Pairs with mark_define_dirty cascade metrics so soft/hard share
        // the same observability surface.
        const auto inv_depth = static_cast<std::uint64_t>(1 + dependents.size()); // root + fan-out
        metrics_.invalidate_cascade_depth_total.fetch_add(inv_depth, std::memory_order_relaxed);
        auto inv_expected = metrics_.invalidate_cascade_depth_max.load(std::memory_order_relaxed);
        while (
            inv_depth > inv_expected &&
            !metrics_.invalidate_cascade_depth_max.compare_exchange_weak(inv_expected, inv_depth)) {
            // retry
        }

        // Clean up old dependency info for all affected functions
        // (the redefined function and all its transitives)
        for (auto& f : dependents) {
            auto fit = dep_graph_.find(f);
            if (fit != dep_graph_.end()) {
                for (auto& callee : fit->second.calls) {
                    auto& cb = dep_graph_[callee].called_by;
                    cb.erase(std::remove(cb.begin(), cb.end(), f), cb.end());
                }
                dep_graph_.erase(f);
            }
        }
        // Issue #2032: advance dep_graph generation so concurrent
        // record_dependency that raced the exclusive window rejects.
        dep_graph_generation_.fetch_add(1, std::memory_order_release);
        metrics_.dep_graph_generation_total.fetch_add(1, std::memory_order_relaxed);
    }
    // Invalidate JIT cache for affected functions.
    // Issue #491 + #1378: erase jit_cache_ AND jit_.invalidate in
    // the SAME jit_cache_mtx_ scope so a concurrent shared reader
    // never observes "cache miss but AuraJIT still has native code".
    // Lock order: mutate_mtx_ (already held) → jit_cache_mtx_.
    {
        std::unique_lock cache_write(jit_cache_mtx_);
        jit_cache_.erase(name);
        metrics_.jit_cache_evictions.fetch_add(1, std::memory_order_relaxed);
        for (auto& dep_name : dependents) {
            jit_cache_.erase(dep_name);
            metrics_.jit_cache_evictions.fetch_add(1, std::memory_order_relaxed);
            // Issue #1286 / #1915: cascade body-only dirty to dependents
            // (callers re-lower body, not nested lambdas) — avoids
            // full-function mark_all_blocks_dirty degradation.
            if (auto dit = ir_cache_v2_.find(dep_name); dit != ir_cache_v2_.end()) {
                const auto n = dit->second.mark_caller_body_dirty();
                metrics_.invalidate_per_block_dirty_total.fetch_add(1, std::memory_order_relaxed);
                if (n > 0) {
                    metrics_.dirty_propagation_block_marks.fetch_add(n, std::memory_order_relaxed);
                    const auto dirty_fns = dit->second.dirty_func_count();
                    const auto total_fns = dit->second.irs.size();
                    if (total_fns > dirty_fns) {
                        metrics_.minimal_recompile_clean_funcs_saved.fetch_add(
                            total_fns - dirty_fns, std::memory_order_relaxed);
                        metrics_.minimal_recompile_scope_samples.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                } else {
                    dit->second.mark_all_blocks_dirty();
                    metrics_.dirty_propagation_full_func_marks.fetch_add(1,
                                                                         std::memory_order_relaxed);
                }
                // Issue #2034: force SoA instruction_dirty_ after
                // dependent body-only / full cascade mark.
                finish_cascade_soa_dirty_sync_(dit->second);
            }
        }
        // Drop stale AuraJIT modules inside the same lock as erase.
        jit_.invalidate(name.c_str());
        jit_.invalidate_prefix(name.c_str());
        metrics_.jit_hotswap_invalidate_total.fetch_add(1, std::memory_order_relaxed);
        evaluator_.bump_incremental_closure_jit_sync();
        for (auto& dep_name : dependents) {
            jit_.invalidate(dep_name.c_str());
            jit_.invalidate_prefix(dep_name.c_str());
            metrics_.jit_hotswap_invalidate_total.fetch_add(1, std::memory_order_relaxed);
            evaluator_.bump_incremental_closure_jit_sync();
        }
    }

    // Issue #225 cycle 3: invalidate bridge data for
    // the mutated function and all its dependents.
    // Bumps the bridge_epoch_ field so any closure
    // holding a reference will detect staleness and
    // re-parse from body_source on next use.
    // Issue #741: quote/lambda defines use impact_scope-
    // selective shared_ptr refresh instead of full bridge wipe.
    // Issue #682: GC root coordination before bindings cleared.
    const auto invalidate_bridge_with_impact = [&](const std::string& affected_name) {
        on_compiler_invalidate_gc_coordination(affected_name);
        auto src_it = function_sources_.find(affected_name);
        if (src_it == function_sources_.end()) {
            invalidate_bridge_for(affected_name);
            return;
        }
        auto alloc = arena_.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(src_it->second, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE || !flat_has_quote_or_lambda(flat)) {
            invalidate_bridge_for(affected_name);
            return;
        }
        flat.root = pr.root;
        // Issue #2031: reverse-index from cached IR for instruction-level impact.
        SourceToIrMap source_to_ir;
        std::unordered_map<std::string, std::size_t, aura::core::TransparentStringHash,
                           std::equal_to<>>
            ir_cache_index;
        if (auto cit = ir_cache_v2_.find(affected_name); cit != ir_cache_v2_.end()) {
            populate_source_to_ir_from_irs(cit->second.irs, source_to_ir);
            for (std::size_t fi = 0; fi < cit->second.irs.size(); ++fi)
                ir_cache_index[cit->second.irs[fi].name] = fi;
        }
        auto scope = compute_impact_scope(flat, pr.root, source_to_ir, ir_cache_index);
        if (auto cit = ir_cache_v2_.find(affected_name); cit != ir_cache_v2_.end())
            (void)apply_impact_scope_dirty(cit->second, scope);
        selective_invalidate_bridge_for_impact(affected_name, scope);
        metrics_.incremental_closure_quote_lambda_stale_prevented_total.fetch_add(
            1, std::memory_order_relaxed);
        evaluator_.bump_incremental_closure_quote_lambda_stale_prevented();
    };
    invalidate_bridge_with_impact(name);
    for (auto& dep_name : dependents)
        invalidate_bridge_with_impact(dep_name);

    // Issue #1536 / #2042: bulk JIT walk + comprehensive live-closure expire
    // (IR runtime_closures_ + tree-walker / fiber Closures + PrimCall cache).
    // Soft path already ran expire via atomic_bump_epochs_and_stamp_bridge;
    // re-run after per-name bridge invalidation so any late-stamped IRClosures
    // that still predate cur_epoch are expired before clear_ir_define_env_binding.
    notify_walk_active_closures_(bridge_epoch());
    (void)expire_stale_live_closures_(bridge_epoch());

    // Issue #741: re-stamp EnvFrame version_ for live tree-walker
    // closures captured from quote/lambda paths in impacted blocks.
    (void)evaluator_.resync_live_closure_env_versions_on_invalidate();

    // Issue #272 Cycle 2: drop stale IR define env bindings before re-bind.
    clear_ir_define_env_binding(name);
    for (auto& dep_name : dependents)
        clear_ir_define_env_binding(dep_name);

    // Clean up the original function's dep info
    {
        std::unique_lock dep_write(dep_graph_mtx_);
        auto it = dep_graph_.find(name);
        if (it != dep_graph_.end()) {
            for (auto& callee : it->second.calls) {
                auto& cb = dep_graph_[callee].called_by;
                cb.erase(std::remove(cb.begin(), cb.end(), name), cb.end());
            }
            dep_graph_.erase(name);
        }
    }

    // Issue #2041: cascade re-lower prefers partial (should_partial_relower)
    // when ir_cache_v2_ has a small body-only dirty mask — then feeds JIT
    // via relower_define_blocks → partial_recompile. Full lower_to_ir only
    // when partial is not applicable or fails (no silent stale IR).
    //
    // Helper: try partial path for one define. Returns true when the
    // entry is clean / was partially re-lowered (caller must not full-lower).
    auto try_partial_invalidate_relower = [&](const std::string& fname) -> bool {
        auto src_it = function_sources_.find(fname);
        if (src_it == function_sources_.end())
            return false;
        auto vit = ir_cache_v2_.find(fname);
        if (vit == ir_cache_v2_.end() || vit->second.irs.empty())
            return false;
        const std::size_t dirty_n = vit->second.dirty_block_count();
        // Clean entry — nothing to re-lower.
        if (dirty_n == 0 && !vit->second.dirty)
            return true;
        // Issue #2041 AC: respect should_partial_relower for the cascade.
        // Large dirty surfaces (≥ threshold) go to the full path below.
        if (dirty_n > 0 && !should_partial_relower(dirty_n)) {
            metrics_.partial_relower_threshold_used.store(get_partial_relower_threshold(),
                                                          std::memory_order_relaxed);
            return false;
        }
        auto alloc = arena_.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(src_it->second, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            return false;
        flat.root = pr.root;
        // Prefer Lambda body node for per-function re-lower.
        aura::ast::NodeId expanded = pr.root;
        if (expanded < flat.size()) {
            auto dv = flat.get(expanded);
            if (dv.tag == aura::ast::NodeTag::Define && !dv.children.empty()) {
                auto body = dv.child(0);
                if (body < flat.size() && flat.get(body).tag == aura::ast::NodeTag::Lambda)
                    expanded = body;
            }
        }
        const auto per_before =
            metrics_.relower_per_function_called_count.load(std::memory_order_relaxed);
        const auto blocks_before =
            metrics_.incremental_relower_blocks_total.load(std::memory_order_relaxed);
        if (!relower_only_dirty_blocks(fname, src_it->second, flat, pool, expanded))
            return false;
        // True partial win (per-fn / per-block), not internal full-fallback.
        const bool true_partial =
            metrics_.relower_per_function_called_count.load(std::memory_order_relaxed) >
                per_before ||
            metrics_.incremental_relower_blocks_total.load(std::memory_order_relaxed) >
                blocks_before;
        if (true_partial) {
            metrics_.incremental_partial_relower_total.fetch_add(1, std::memory_order_relaxed);
            metrics_.partial_relower_threshold_used.store(get_partial_relower_threshold(),
                                                          std::memory_order_relaxed);
        } else {
            // relower_define_blocks took full-fallback internally — still
            // counts as handled (IR + bitmask refreshed; no second full pass).
            metrics_.incremental_full_fallback_total.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    };

    // Root: body-only dirty → partial re-lower + JIT partial_recompile when
    // should_partial_relower holds (AI multi-round mutate hot path).
    (void)try_partial_invalidate_relower(name);

    // Re-lower each dependent. Prefer partial; full lower only when needed.
    // Dependents vector is BFS + lexicographically sorted (Issue #401).
    for (auto& dep_name : dependents) {
        if (try_partial_invalidate_relower(dep_name))
            continue;

        auto src_it = function_sources_.find(dep_name);
        if (src_it == function_sources_.end())
            continue;

        // Full cascade re-lower (large dirty mask or partial failed).
        metrics_.incremental_full_fallback_total.fetch_add(1, std::memory_order_relaxed);
        metrics_.relower_full_called_count.fetch_add(1, std::memory_order_relaxed);
        auto alloc = arena_.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(src_it->second, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            continue;
        flat.root = pr.root;

        auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
        std::vector<std::string> cache_hits;
        auto ir_mod = lower_to_ir_with_cache_tracked(
            flat, pool, arena_, cache_ptr, &cache_hits, &evaluator_.primitives(), nullptr,
            cache_strings_ptr, nullptr, &type_registry_, value_cells_for_lowering());

        {
            ComputeKindWrap ck_pass;
            ConstantFoldingWrap cf_pass;
            for (auto& func : ir_mod.functions) {
                if (func.id == ir_mod.entry_function_id)
                    continue;
                ck_pass.compute_function(func);
                (void)cf_pass.fold_function(func);
            }
        }

        std::vector<aura::ir::IRFunction> bundle;
        for (auto& func : ir_mod.functions) {
            if (func.id != ir_mod.entry_function_id) {
                bundle.push_back(std::move(func));
            }
        }
        ir_cache_[dep_name] = std::move(bundle);
        snapshot_ir_for_disk(dep_name);

        for (auto& called_name : cache_hits) {
            record_dependency(dep_name, called_name);
        }

        (void)bind_function_define_via_ir(ir_mod, dep_name);
    }

    // Issue #638: propagate shape invalidation to dependents.
    for (auto& dep_name : dependents)
        invalidate_shape(dep_name);

    // Mark dependent modules dirty
    mark_module_dirty(name);
    for (auto& d : dependents)
        mark_module_dirty(d);

    // Issue #683: post re-lower linear ownership revalidate probe.
    run_linear_ownership_revalidate_after_invalidate(name);

    // Issue #2035: HotUpdateRegistry dirty notify + region-mask reemit
    // after hard invalidate cascade (root + dependents).
    notify_hot_update_after_cascade_(name, dependents);
}

} // namespace aura::compiler
