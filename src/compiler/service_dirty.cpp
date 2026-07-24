// service_dirty.cpp — Wave 5: CompilerService dirty-mark / cascade out-of-line
// aura.compiler.service module partition (first service implementation unit).
//
// mark_define_dirty (BFS cascade + dual-epoch) and mark_all_defines_dirty
// (set-code soft-dirty + bulk JIT invalidate) leave service.ixx so the
// interface unit stays thinner. Declarations remain on CompilerService.

module;

#include "lock_order_audit.h"
#include "observability_metrics.h"
#include "jit_typed_mutation_stats.h" // ir_soa_migration::record_capture_dirty_mark
#include "aura_jit.h"
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
            } else if (nested_lambdas) {
                // Fallback: no body bitmask → full dirty (pre-#1505).
                centry.dirty = true;
                centry.mark_all_blocks_dirty();
                metrics_.cascade_full_count.fetch_add(1, std::memory_order_relaxed);
                metrics_.dep_graph_nested_lambda_full_dirty.fetch_add(1, std::memory_order_relaxed);
            } else {
                // Fallback: convention doesn't hold —
                // conservatively mark all blocks dirty.
                centry.dirty = true;
                centry.mark_all_blocks_dirty();
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
    for (auto& [name, entry] : ir_cache_v2_) {
        (void)name;
        entry.dirty = true;
        entry.mark_all_blocks_dirty();
        ++dirty_n;
    }
    if (dirty_n > 0) {
        const auto evicted = jit_.invalidate_all();
        metrics_.jit_hotswap_invalidate_total.fetch_add(evicted > 0 ? evicted : dirty_n,
                                                        std::memory_order_relaxed);
        evaluator_.bump_incremental_closure_jit_sync();
    }
}


} // namespace aura::compiler
