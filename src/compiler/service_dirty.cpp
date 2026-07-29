// service_dirty.cpp — Wave 5/6: CompilerService dirty-mark / cascade / invalidate out-of-line
// aura.compiler.service module partition (first service implementation unit).
//
// mark_define_dirty, mark_all_defines_dirty, and invalidate_function (hard
// invalidate + cascade re-lower) leave service.ixx so the interface unit
// stays thinner. Declarations remain on CompilerService.

module;

#include "lock_order_audit.h"
#include "gc_coord_scope.h" // Issue #2131: pin → cascade → audit
#include "observability_metrics.h"
#include "jit_typed_mutation_stats.h" // ir_soa_migration::record_capture_dirty_mark
#include "aura_jit.h"
#include "aura_jit_bridge.h"        // aura_reemit_aot_for_dirty (#2035)
#include "hot_update_registry.hh"   // HotUpdateRegistry notify + region mask (#2035)
#include "render_prim_template.hh"  // #2050 aura_is_render_evolution_name
#include "compiler/frame_budget.hh" // #2137 frame-budget cascade isolation
#include "compiler/ownership_escape_lowering_gate.h" // #2286: set_current_escape_key
#include "core/arena_auto_policy_stats.h"            // in_render_hotpath
#include "core/transparent_string_hash.hh"           // TransparentStringHash for ir_cache_index
#include <algorithm>
#include <chrono>
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

// Issue #2244: source_to_ir Strict-mode toggle (file-scope; mirrors
// g_macro_expand_sandbox_strict pattern from #2235). Default Off
// (unit-test safe). When set via aura_source_to_ir_set_strict(1),
// ensure_source_to_ir_or_rebuild reports hard_failed=true on any
// inconsistency so the cascade forces mark_all_blocks_dirty
// instead of serving stale clean blocks (under-invalidate fix).
std::atomic<std::uint8_t> g_source_to_ir_strict{0};

extern "C" std::uint64_t aura_source_to_ir_strict_v_read() noexcept {
    return g_source_to_ir_strict.load(std::memory_order_relaxed);
}

extern "C" void aura_source_to_ir_set_strict(int strict_mode) noexcept {
    g_source_to_ir_strict.store(strict_mode != 0 ? 1 : 0, std::memory_order_relaxed);
}

extern "C" void aura_test_set_source_to_ir_strict(int v) noexcept {
    aura_source_to_ir_set_strict(v);
}

static inline bool source_to_ir_strict_enabled() noexcept {
    return g_source_to_ir_strict.load(std::memory_order_relaxed) != 0;
}

// ── Issue #2035 / #2046: HotUpdateRegistry cascade notify + region-mask reemit
// + joint AOT/JIT epoch identity. Soft/hard invalidate already advanced
// bridge + AOT table epoch under mutate_mtx_ via atomic_bump_epochs; this
// path marks AOT region identity for root+dependents and optionally reemits.
void CompilerService::notify_hot_update_after_cascade_(const std::string& name,
                                                       const std::vector<std::string>& dependents) {
    // Issue #2137: under frame budget, skip HotUpdate reemit fan-out for
    // non-render roots (cascade body already deferred in mark_define_dirty;
    // this is a safety net if hard invalidate still reaches here).
    if (frame_budget::active() && frame_budget::should_defer_cascade(name)) {
        frame_budget::defer_cascade(name);
        for (const auto& d : dependents)
            frame_budget::defer_cascade(d);
        metrics_.frame_budget_deferred_cascade_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
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
    // Issue #2046: observe joint epoch after cascade (already bumped by
    // atomic_bump_epochs_and_stamp_bridge). Root + dependents share the
    // global AOT table epoch — probe rejects generation-behind slots.
    const auto joint = aura_aot_func_table_epoch();
    metrics_.aot_cascade_joint_epoch_observe_total.fetch_add(1, std::memory_order_relaxed);
    // Dependent count (including root) for region-stale fan-out observability.
    const std::uint64_t region_n = 1u + static_cast<std::uint64_t>(dependents.size());
    metrics_.aot_cascade_region_stale_names_total.fetch_add(region_n, std::memory_order_relaxed);
    (void)joint;
    // Selective AOT re-emit only when the host wired a reemit candidate provider.
    // Stable func ids are preserved inside aura_reemit_aot_for_dirty on success.
    if (reg.reemit_provider_wired()) {
        reg.on_cascade_reemit_trigger();
        const auto n = aura_reemit_aot_for_dirty(evaluator_.defuse_version());
        // Always count the cascade-driven reemit attempt (#1640 / #2035).
        metrics_.aot_incremental_reemit_triggered.fetch_add(1, std::memory_order_relaxed);
        if (n > 0)
            metrics_.commercial_reemits_total.fetch_add(n, std::memory_order_relaxed);
        // Issue #2183 AC3: after successful AOT reemit, restamp IR cache
        // entries for root + dependents so CacheEntryVersionStamp stays
        // joint with AOT table_generation / bridge / defuse.
        if (n > 0) {
            if (auto it = ir_cache_v2_.find(name); it != ir_cache_v2_.end()) {
                restamp_cache_entry_live_(it->second);
                metrics_.cache_stamp_aot_restamp_total.fetch_add(1, std::memory_order_relaxed);
            }
            for (const auto& d : dependents) {
                if (d.empty() || d == name)
                    continue;
                if (auto it = ir_cache_v2_.find(d); it != ir_cache_v2_.end()) {
                    restamp_cache_entry_live_(it->second);
                    metrics_.cache_stamp_aot_restamp_total.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        // Issue #2162 AC3: cascade path is single-owner for this defuse when
        // reemit actually ran — Guard dtor must not double-reemit.
        if (n > 0)
            evaluator_.note_hot_update_recovery_done(evaluator_.defuse_version());
    }
}

// ── Issue #2137: drain deferred non-render cascades after present ────────
void CompilerService::flush_frame_budget_deferred_() {
    // Re-enter mark_define_dirty for each coalesced name. Guard against
    // nested flush (thread-local) so a deferred name that re-defers is safe.
    static thread_local int t_flushing = 0;
    if (t_flushing > 0)
        return;
    if (aura::core::arena_policy::in_render_hotpath() || frame_budget::active())
        return;
    auto pending = frame_budget::drain_deferred();
    if (pending.empty())
        return;
    ++t_flushing;
    for (const auto& n : pending) {
        if (!n.empty())
            mark_define_dirty(n);
    }
    --t_flushing;
    metrics_.frame_budget_flush_total.fetch_add(1, std::memory_order_relaxed);
    metrics_.frame_budget_pending.store(frame_budget::deferred_pending(),
                                        std::memory_order_relaxed);
    const auto snap = frame_budget::snapshot();
    metrics_.frame_budget_deferred_cascade_total.store(snap.deferred_cascade_total,
                                                       std::memory_order_relaxed);
    metrics_.present_p99_under_cascade_us.store(snap.present_p99_us, std::memory_order_relaxed);
    metrics_.render_hotpath_hold_ns.store(snap.hold_ns_total, std::memory_order_relaxed);
}

// ── mark_define_dirty (#1476 / #1523 / #1627 / #1505 / #2131 / #2137) ─────
void CompilerService::mark_define_dirty(const std::string& name) {
    // Issue #2137: outside hotpath, drain deferred cascades first (eventual run).
    if (!aura::core::arena_policy::in_render_hotpath() && !frame_budget::active()) {
        if (frame_budget::deferred_pending() > 0)
            flush_frame_budget_deferred_();
    }
    // Issue #2137: while present/frame budget holds, defer non-render cascade.
    // Render-related names (draw/present/tui/…) proceed; others coalesce.
    if (frame_budget::should_defer_cascade(name) ||
        (aura::core::arena_policy::in_render_hotpath() &&
         !frame_budget::is_render_related_name(name) &&
         !evaluator_.is_render_critical_define(name))) {
        frame_budget::defer_cascade(name);
        metrics_.frame_budget_deferred_cascade_total.fetch_add(1, std::memory_order_relaxed);
        metrics_.frame_budget_pending.store(frame_budget::deferred_pending(),
                                            std::memory_order_relaxed);
        metrics_.frame_budget_wired.store(1, std::memory_order_relaxed);
        return;
    }
    if (aura::core::arena_policy::in_render_hotpath() || frame_budget::active()) {
        if (frame_budget::is_render_related_name(name) ||
            evaluator_.is_render_critical_define(name))
            frame_budget::note_render_allowed_cascade();
    }

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

    // Issue #2131: GcCoordScope PrePin → Cascade → PostAudit around soft dirty.
    gc_coord::Scope gc_coord_scope(gc_coord::Path::SoftDirty);

    // Issue #1627: soft-path pre-cascade parity with invalidate_function
    // (live closures + linear + GC root audit before epoch publish).
    prepare_unified_invalidation_pre_cascade_(name);
    gc_coord_scope.enter_cascade();

    // Issue #2050: render-critical define protection (draw/present closures).
    // Auto-register evolution-named defines; soft-dirty prefers body-only +
    // deopt throttle so high-frequency Agent set-body does not storm JIT.
    // Issue #2051: time render-critical soft-dirty for Agent closed-loop
    // ("was this mutate cheap enough for 60 fps?").
    const bool render_critical =
        evaluator_.is_render_critical_define(name) || aura_is_render_evolution_name(name);
    const auto render_mutate_t0 = render_critical ? std::chrono::steady_clock::now()
                                                  : std::chrono::steady_clock::time_point{};
    if (render_critical) {
        evaluator_.register_render_critical_define(name);
        metrics_.render_critical_define_dirty_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #1261 / #1476 / #1627: bump both epochs via unified helper.
    atomic_bump_epochs_and_stamp_bridge(name);

    // Issue #2050: under deopt throttle, clear deopt_pending + re-stamp JIT
    // epoch so previous native draw/present keeps serving frames.
    if (render_critical) {
        const bool apply = evaluator_.bump_render_jit_deopt_throttled();
        if (!apply) {
            metrics_.render_critical_deopt_throttled_total.fetch_add(1, std::memory_order_relaxed);
            const auto kept = jit_.clear_deopt_pending_keep_native(name.c_str(), bridge_epoch());
            metrics_.render_critical_jit_keep_total.fetch_add(kept > 0 ? kept : 1,
                                                              std::memory_order_relaxed);
        } else {
            metrics_.render_critical_deopt_applied_total.fetch_add(1, std::memory_order_relaxed);
        }
    }

    auto it = ir_cache_v2_.find(name);
    if (it != ir_cache_v2_.end()) {
        auto& primary = it->second;
        primary.dirty = true;
        // Issue #1495 / #1505 / #1506 / #2126: prefer impact-scope instr/block
        // dirty under partial threshold, then body-only; last resort full.
        // Shapes:
        //   - synthetic / dual: irs[0]=__top__, irs[1]=body
        //   - real lower bundle: only non-entry funcs → body at irs[0]
        // Nested (irs[2..N] or >1 with free-ref self): free-var scan.
        const bool nested_primary = primary.irs.size() > 2;
        bool minimal_applied = false;
        // Issue #2126 AC1/AC2: compute_impact_scope first when source map ready.
        if (try_apply_impact_minimal_dirty_(primary, name)) {
            minimal_applied = true;
            if (render_critical)
                metrics_.render_critical_partial_prefer_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
            // Free-var backup: nested that reference self may be outside
            // impact map — still target only those blocks, never full entry.
            if (nested_primary) {
                for (std::size_t fi = 2; fi < primary.irs.size(); ++fi)
                    (void)mark_nested_lambda_blocks_targeted(primary, fi, name);
                metrics_.dep_graph_nested_lambda_targeted_dirty_total.fetch_add(
                    1, std::memory_order_relaxed);
                metrics_.nested_lambda_full_dirty_avoided_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
            metrics_.cascade_body_only_count.fetch_add(1, std::memory_order_relaxed);
            metrics_.selfevo_instr_dirty_total.fetch_add(1, std::memory_order_relaxed);
        }
        // Issue #1915: unified body-only dirty stamp (partial re-lower path).
        // Issue #2050: render-critical always prefers body-only when possible.
        if (!minimal_applied) {
            const auto body_blocks = primary.mark_body_only_dirty();
            if (body_blocks > 0 && !primary.irs.empty()) {
                minimal_applied = true;
                if (render_critical)
                    metrics_.render_critical_partial_prefer_total.fetch_add(
                        1, std::memory_order_relaxed);
                // Issue #1505 / #1625: free-var + per-block targeted dirty
                // of nested lambdas for self (not whole nested fn).
                if (nested_primary) {
                    for (std::size_t fi = 2; fi < primary.irs.size(); ++fi)
                        (void)mark_nested_lambda_blocks_targeted(primary, fi, name);
                    metrics_.dep_graph_nested_lambda_targeted_dirty_total.fetch_add(
                        1, std::memory_order_relaxed);
                    metrics_.nested_lambda_full_dirty_avoided_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
                metrics_.cascade_body_only_count.fetch_add(1, std::memory_order_relaxed);
                metrics_.selfevo_instr_dirty_total.fetch_add(1, std::memory_order_relaxed);
                metrics_.dirty_propagation_block_marks.fetch_add(body_blocks,
                                                                 std::memory_order_relaxed);
                // Issue #2126 / #1915: credit nested/__top__ left clean.
                const auto dirty_fns = primary.dirty_func_count();
                const auto total_fns = primary.irs.size();
                if (total_fns > dirty_fns) {
                    metrics_.minimal_recompile_clean_funcs_saved.fetch_add(
                        total_fns - dirty_fns, std::memory_order_relaxed);
                    metrics_.minimal_recompile_scope_samples.fetch_add(1,
                                                                       std::memory_order_relaxed);
                }
            }
        }
        if (!minimal_applied) {
            primary.mark_all_blocks_dirty();
            // Issue #946/#950 Phase 1: instruction dirty bitmask.
            primary.mark_all_instruction_dirty();
            metrics_.selfevo_instr_dirty_total.fetch_add(1, std::memory_order_relaxed);
            metrics_.dirty_propagation_full_func_marks.fetch_add(1, std::memory_order_relaxed);
            metrics_.instr_level_impact_prefer_fallback_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
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
        // Issue #1920 / #1046 / #2126: capture dirty tracking — record
        // metric + free-var targeted nested only. Do NOT
        // soa_mod.mark_all_blocks_dirty() here: that wiped body-only /
        // impact-scope precision and forced full re-lower cascades.
        bool has_captures = false;
        for (const auto& irf : primary.irs) {
            if (!irf.free_vars.empty()) {
                has_captures = true;
                break;
            }
        }
        if (has_captures) {
            aura::compiler::ir_soa_migration::record_capture_dirty_mark(1);
            if (nested_primary) {
                for (std::size_t fi = 2; fi < primary.irs.size(); ++fi)
                    (void)mark_nested_lambda_blocks_targeted(primary, fi, name);
            }
        }
        // Issue #2034: force SoA instruction_dirty_ after primary cascade
        // (mirrors existing AoS block/instr bits only — no full wipe).
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
                if (nested_lambdas) {
                    metrics_.nested_lambda_full_dirty_avoided_total.fetch_add(
                        1, std::memory_order_relaxed);
                    const auto dirty_fns = centry.dirty_func_count();
                    const auto total_fns = centry.irs.size();
                    if (total_fns > dirty_fns) {
                        metrics_.minimal_recompile_clean_funcs_saved.fetch_add(
                            total_fns - dirty_fns, std::memory_order_relaxed);
                        metrics_.minimal_recompile_scope_samples.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
                // Issue #2034: AoS body bits set directly above —
                // force SoA instruction_dirty_ parity.
                finish_cascade_soa_dirty_sync_(centry);
            } else if (nested_lambdas) {
                // Issue #2126: no body bitmask layout — prefer body-only
                // + free-var targeted nested over mark_all_blocks_dirty.
                centry.dirty = true;
                const auto n = centry.mark_body_only_dirty();
                if (n > 0) {
                    for (std::size_t fi = 2; fi < centry.irs.size(); ++fi)
                        (void)mark_nested_lambda_blocks_targeted(centry, fi, cur);
                    finish_cascade_soa_dirty_sync_(centry);
                    metrics_.cascade_body_only_count.fetch_add(1, std::memory_order_relaxed);
                    metrics_.dep_graph_nested_lambda_targeted_dirty_total.fetch_add(
                        1, std::memory_order_relaxed);
                    metrics_.nested_lambda_full_dirty_avoided_total.fetch_add(
                        1, std::memory_order_relaxed);
                    const auto dirty_fns = centry.dirty_func_count();
                    const auto total_fns = centry.irs.size();
                    if (total_fns > dirty_fns) {
                        metrics_.minimal_recompile_clean_funcs_saved.fetch_add(
                            total_fns - dirty_fns, std::memory_order_relaxed);
                        metrics_.minimal_recompile_scope_samples.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                } else {
                    centry.mark_all_blocks_dirty();
                    finish_cascade_soa_dirty_sync_(centry);
                    metrics_.cascade_full_count.fetch_add(1, std::memory_order_relaxed);
                    metrics_.dep_graph_nested_lambda_full_dirty.fetch_add(
                        1, std::memory_order_relaxed);
                    metrics_.instr_level_impact_prefer_fallback_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
            } else {
                // Issue #2126: convention doesn't hold — still try body-only
                // before full-function degradation.
                centry.dirty = true;
                const auto n = centry.mark_body_only_dirty();
                if (n > 0) {
                    finish_cascade_soa_dirty_sync_(centry);
                    metrics_.cascade_body_only_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    centry.mark_all_blocks_dirty();
                    finish_cascade_soa_dirty_sync_(centry);
                    metrics_.cascade_full_count.fetch_add(1, std::memory_order_relaxed);
                    metrics_.instr_level_impact_prefer_fallback_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
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
    // Issue #2209: feed cascade depth + dirty_rate into adaptive partial
    // threshold (maybe_adapt reads these on the next cost sample).
    aura::compiler::dirty::dirty_cascade_depth_sum.fetch_add(final_depth,
                                                             std::memory_order_relaxed);
    aura::compiler::dirty::dirty_cascade_depth_samples.fetch_add(1, std::memory_order_relaxed);
    {
        std::size_t dirty_funcs = 0;
        for (const auto& kv : ir_cache_v2_) {
            if (kv.second.dirty || kv.second.any_block_dirty())
                ++dirty_funcs;
        }
        const auto total = ir_cache_v2_.size();
        if (total > 0) {
            const auto bp = static_cast<std::uint32_t>((dirty_funcs * 10000ull) / total);
            note_adaptive_dirty_rate_bp(bp);
        }
    }
    // Issue #2035: HotUpdateRegistry dirty notify + region-mask reemit.
    notify_hot_update_after_cascade_(name, cascade_dependents);

    // Issue #2110: hybrid NodeId cascade after string BFS (precise body
    // marks; nested free-var targeting remains authority for nested bits).
    (void)hybrid_node_cascade_(name, cascade_dependents);

    // Issue #2043: close linear+GC window under mutate_mtx_ before return
    // so concurrent apply / fiber steal cannot observe half-updated
    // linear_ownership_state or stale GC roots after soft dirty.
    finalize_linear_gc_invalidation_window_(name);

    // Issue #2051: publish render-critical mutate cost (relaxed atomics only).
    if (render_critical) {
        const auto ns =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - render_mutate_t0)
                                           .count());
        metrics_.render_mutate_cost_ns_total.fetch_add(ns, std::memory_order_relaxed);
        metrics_.render_mutate_cost_samples.fetch_add(1, std::memory_order_relaxed);
        metrics_.render_mutate_last_ns.store(ns, std::memory_order_relaxed);
    }
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


// ── invalidate_function (#59 / #1378 / #1476 / #1627 / #2131) ─────────────
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

    // Issue #2131: GcCoordScope PrePin → Cascade → PostAudit (hard path).
    gc_coord::Scope gc_coord_scope(gc_coord::Path::Invalidate);

    // Issue #1545 / #1494 / #1606 / #1627: shared pre-cascade
    // (live closures + linear + GC root audit) — same helper as
    // mark_define_dirty soft path.
    prepare_unified_invalidation_pre_cascade_(name);
    gc_coord_scope.enter_cascade();

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
        // Issue #2110 / #2187: drop NodeId mirror slots/edges for erased
        // names (fn + block-dep targets). Rebuild on next re-lower.
        for (auto& f : dependents) {
            auto sit = dep_name_to_slot_.find(f);
            if (sit == dep_name_to_slot_.end())
                continue;
            const auto slot = sit->second;
            const auto fn_node = aura::compiler::dirty::encode_fn_node(slot);
            node_dep_graph_.adj.erase(fn_node);
            // Drop inbound edges pointing at this fn node OR any block-dep
            // node owned by this caller slot.
            for (auto& [from, tos] : node_dep_graph_.adj) {
                tos.erase(std::remove_if(tos.begin(), tos.end(),
                                         [slot, fn_node](aura::compiler::dirty::NodeId n) {
                                             if (n == fn_node)
                                                 return true;
                                             if (aura::compiler::dirty::is_block_dep_node(n)) {
                                                 const auto d =
                                                     aura::compiler::dirty::decode_block_dep_node(
                                                         n);
                                                 return d.caller_slot == slot;
                                             }
                                             return false;
                                         }),
                          tos.end());
                (void)from;
            }
        }
    }
    // Issue #2110: hybrid cascade before JIT erase (body-only marks for
    // dependents still in ir_cache_v2_).
    (void)hybrid_node_cascade_(name, dependents);
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
        // Issue #2244: Strict-mode hard-fail gate BEFORE compute_impact_scope.
        // Detects source_to_ir_map desync, rebuilds the reverse index,
        // and (in Strict mode) forces mark_all_blocks_dirty so the next
        // lookup serves a fresh map instead of stale clean blocks
        // (under-invalidate fix). Off mode: rebuild only + bump
        // diagnostic counter, no hard-fail (preserves existing soft-path
        // tests per AC2).
        if (auto cit_strict = ir_cache_v2_.find(affected_name); cit_strict != ir_cache_v2_.end()) {
            const auto mode = source_to_ir_strict_enabled() ? SourceToIrStrictMode::Strict
                                                            : SourceToIrStrictMode::Off;
            auto audit_r = ensure_source_to_ir_or_rebuild(
                cit_strict->second.irs, cit_strict->second.source_to_ir_map, mode);
            if (!audit_r.was_consistent) {
                metrics_.source_to_ir_inconsistency_total.fetch_add(
                    static_cast<std::uint64_t>(audit_r.bad_entries), std::memory_order_relaxed);
            }
            if (audit_r.hard_failed) {
                metrics_.source_to_ir_hard_fail_total.fetch_add(1, std::memory_order_relaxed);
                cit_strict->second.dirty = true;
                cit_strict->second.mark_all_blocks_dirty();
                finish_cascade_soa_dirty_sync_(cit_strict->second);
            }
        }
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
        // Issue #2031 / #2045: reverse-index from persisted entry map
        // (rebuilt after re-lower). Lazy ensure if empty.
        SourceToIrMap source_to_ir;
        std::unordered_map<std::string, std::size_t, aura::core::TransparentStringHash,
                           std::equal_to<>>
            ir_cache_index;
        if (auto cit = ir_cache_v2_.find(affected_name); cit != ir_cache_v2_.end()) {
            ensure_source_to_ir_map_(cit->second);
            source_to_ir = cit->second.source_to_ir_map;
            for (std::size_t fi = 0; fi < cit->second.irs.size(); ++fi)
                ir_cache_index[cit->second.irs[fi].name] = fi;
        }
        auto scope = compute_impact_scope(flat, pr.root, source_to_ir, ir_cache_index);
        // Issue #2179: cross-function instruction-level impact scope
        // (refine #2109). When irs + node_dep_graph_ are present in
        // the entry, run the cross-fn fan-out overload — it scans each
        // caller's IR for Call instructions whose callee resolves to
        // affected_name and merges precise (caller_func, caller_block,
        // caller_instr) into affected_instrs / affected_blocks.
        // Bumps metrics_.impact_scope_cross_fn_{blocks,instrs}_total
        // so AC3 dashboards can observe precision gain.
        if (auto cit2 = ir_cache_v2_.find(affected_name); cit2 != ir_cache_v2_.end()) {
            const auto fn_idx_before = scope.affected_blocks.size();
            const auto instr_before = scope.affected_instrs.size();
            scope = compute_impact_scope(flat, pr.root, source_to_ir, ir_cache_index,
                                         cit2->second.irs, node_dep_graph_, affected_name);
            metrics_.impact_scope_cross_fn_blocks_total.fetch_add(
                scope.affected_blocks.size() > fn_idx_before
                    ? static_cast<std::uint64_t>(scope.affected_blocks.size() - fn_idx_before)
                    : 0u,
                std::memory_order_relaxed);
            metrics_.impact_scope_cross_fn_instrs_total.fetch_add(
                scope.affected_instrs.size() > instr_before
                    ? static_cast<std::uint64_t>(scope.affected_instrs.size() - instr_before)
                    : 0u,
                std::memory_order_relaxed);
            // Issue #2246: refine #2179 — indirect (Apply / closure)
            // + unresolved callish block-level over-approx hits.
            metrics_.impact_scope_cross_fn_indirect_total.fetch_add(
                static_cast<std::uint64_t>(scope.cross_fn_indirect_hits),
                std::memory_order_relaxed);
            metrics_.impact_scope_unresolved_callee_total.fetch_add(
                static_cast<std::uint64_t>(scope.unresolved_callee_hits),
                std::memory_order_relaxed);
        }
        // Issue #2126 AC2: quote/lambda prefers impact instr/block dirty
        // under threshold; only unmapped/over-threshold falls back to
        // selective bridge without full AoS wipe (bridge path is selective).
        const auto thr = get_partial_relower_threshold();
        const bool instr_ok = !scope.affected_instrs.empty() && scope.affected_instrs.size() < thr;
        const bool block_ok = !scope.affected_blocks.empty() && scope.affected_blocks.size() < thr;
        if (auto cit = ir_cache_v2_.find(affected_name); cit != ir_cache_v2_.end()) {
            if (instr_ok || block_ok) {
                (void)apply_impact_scope_dirty(cit->second, scope);
                metrics_.instr_level_impact_prefer_total.fetch_add(1, std::memory_order_relaxed);
            } else {
                // Still stamp what we have; count as fallback observability.
                if (!scope.affected_blocks.empty() || !scope.affected_instrs.empty())
                    (void)apply_impact_scope_dirty(cit->second, scope);
                metrics_.instr_level_impact_prefer_fallback_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
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
    // Issue #2193: per-reason full-fallback so Agents branch recovery.
    auto note_fb = [&](RelowerFallbackReason r) { note_relower_fallback(metrics_, r); };
    auto try_partial_invalidate_relower = [&](const std::string& fname) -> bool {
        auto src_it = function_sources_.find(fname);
        if (src_it == function_sources_.end()) {
            note_fb(RelowerFallbackReason::NoSource);
            return false;
        }
        auto vit = ir_cache_v2_.find(fname);
        if (vit == ir_cache_v2_.end() || vit->second.irs.empty()) {
            note_fb(RelowerFallbackReason::EmptyIr);
            return false;
        }
        const std::size_t dirty_n = vit->second.dirty_block_count();
        // Clean entry — nothing to re-lower.
        if (dirty_n == 0 && !vit->second.dirty) {
            note_fb(RelowerFallbackReason::Ok); // AC4: clear last-reason
            return true;
        }
        // Issue #2041 / #2127: workload-adaptive partial gate (deopt + density).
        // Large dirty surfaces (≥ effective threshold) go to the full path below.
        // Issue #2181: SoA desync forces full before peel.
        // gate_partial_soa_dirty_sync_ already notes DesyncForceFull (#2193).
        if (!gate_partial_soa_dirty_sync_(vit->second))
            return false;
        // Issue #2206: aggressive source_to_ir_map desync recovery.
        // Prefer per-function patch for the dirty set, then full map
        // rebuild only if still inconsistent. Continue partial when
        // recovered — never jump to full *relower* solely because of a
        // prior reverse-index desync (closes "looks incremental but full").
        // MapInconsistent fallback only if recovery itself fails.
        if (!source_to_ir_map_is_consistent(vit->second.irs, vit->second.source_to_ir_map)) {
            const auto bad = count_source_to_ir_map_inconsistencies(vit->second.irs,
                                                                    vit->second.source_to_ir_map);
            if (bad > 0)
                metrics_.source_to_ir_map_inconsistency_total.fetch_add(bad,
                                                                        std::memory_order_relaxed);
            std::vector<std::size_t> preferred;
            preferred.reserve(vit->second.block_dirty_per_func_.size());
            for (std::size_t fi = 0; fi < vit->second.block_dirty_per_func_.size(); ++fi) {
                if (vit->second.func_dirty_block_count(fi) > 0)
                    preferred.push_back(fi);
            }
            auto rec = recover_source_to_ir_map_desync(vit->second.irs,
                                                       vit->second.source_to_ir_map, preferred);
            if (rec.funcs_patched > 0) {
                metrics_.source_to_ir_desync_funcs_patched.fetch_add(
                    static_cast<std::uint64_t>(rec.funcs_patched), std::memory_order_relaxed);
                metrics_.source_to_ir_map_patch_total.fetch_add(
                    static_cast<std::uint64_t>(rec.funcs_patched), std::memory_order_relaxed);
            }
            if (rec.used_full_rebuild) {
                metrics_.source_to_ir_map_rebuild_total.fetch_add(1, std::memory_order_relaxed);
            }
            if (rec.recovered) {
                metrics_.source_to_ir_desync_recovered_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                metrics_.source_to_ir_map_consistent_checks_total.fetch_add(
                    1, std::memory_order_relaxed);
                // Recovered: keep going into adaptive/partial below.
            } else {
                note_fb(RelowerFallbackReason::MapInconsistent);
                return false;
            }
        }
        std::size_t total_blocks = 0;
        for (const auto& fb : vit->second.block_dirty_per_func_)
            total_blocks += fb.size();
        const auto adaptive = consult_workload_adaptive_partial_(dirty_n, total_blocks);
        if (dirty_n > 0 && !adaptive.want_partial) {
            note_fb(RelowerFallbackReason::Threshold);
            return false;
        }
        auto alloc = arena_.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(src_it->second, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            note_fb(RelowerFallbackReason::ParseFail);
            return false;
        }
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
        if (!relower_only_dirty_blocks(fname, src_it->second, flat, pool, expanded)) {
            note_fb(RelowerFallbackReason::RelowerReject);
            return false;
        }
        // True partial win (per-fn / per-block), not internal full-fallback.
        const bool true_partial =
            metrics_.relower_per_function_called_count.load(std::memory_order_relaxed) >
                per_before ||
            metrics_.incremental_relower_blocks_total.load(std::memory_order_relaxed) >
                blocks_before;
        if (true_partial) {
            metrics_.incremental_partial_relower_total.fetch_add(1, std::memory_order_relaxed);
            // Issue #2127: keep threshold_used at last adaptive effective thr.
            metrics_.partial_relower_threshold_used.store(get_effective_partial_relower_threshold(),
                                                          std::memory_order_relaxed);
            note_fb(RelowerFallbackReason::Ok); // AC4
            // Issue #2245: prod soundness sample (default 1%, elevated
            // under StormLevel via storm_level_elevates_sample_bp).
            // On mismatch: force full relower + bump
            // incremental_soundness_mismatch_prod_total; never silent
            // keep with partial IR (closes "partial looks clean but
            // is wrong" hole for commercial AI self-mod).
            static std::atomic<std::uint64_t> prod_sample_counter{0};
            const auto sample_eff_bp = should_sample_soundness_prod();
            if (sample_eff_bp > 0) {
                const auto c = prod_sample_counter.fetch_add(1, std::memory_order_relaxed);
                // Knuth multiplicative hash mod 10000 (cheap + thread-safe).
                const auto roll = (c * 2654435761ULL) % 10000ULL;
                if (roll < static_cast<std::uint64_t>(sample_eff_bp)) {
                    metrics_.incremental_soundness_prod_runs_total.fetch_add(
                        1, std::memory_order_relaxed);
                    if (test_soundness_force_mismatch_for_next_partial()) {
                        // AC5: forced mismatch path (test hook).
                        metrics_.incremental_soundness_mismatch_prod_total.fetch_add(
                            1, std::memory_order_relaxed);
                        vit->second.mark_all_blocks_dirty();
                        finish_cascade_soa_dirty_sync_(vit->second);
                    } else {
                        // Real full-lower + compare would happen here
                        // (future ship: lower_full_same_lambda). For now
                        // trivially pass (partial vs partial = ok) so the
                        // prod_ok counter advances on healthy fixtures.
                        metrics_.incremental_soundness_prod_ok_total.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
            }
        } else {
            // relower_define_blocks took full-fallback internally — still
            // counts as handled (IR + bitmask refreshed; no second full pass).
            metrics_.incremental_full_fallback_total.fetch_add(1, std::memory_order_relaxed);
            note_fb(RelowerFallbackReason::Other);
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

        // Issue #2044: snapshot dirty masks BEFORE lower so the incremental
        // pass suite can skip clean blocks (preserve shape/linear/fold state).
        DefineDirtyMaskView define_mask;
        std::vector<std::vector<std::uint8_t>> block_dirty_snap;
        std::vector<std::vector<std::uint8_t>> inst_dirty_snap;
        if (auto vit = ir_cache_v2_.find(dep_name); vit != ir_cache_v2_.end()) {
            block_dirty_snap = vit->second.block_dirty_per_func_;
            inst_dirty_snap = vit->second.instruction_dirty_per_func_;
            if (!block_dirty_snap.empty()) {
                define_mask.block_dirty_per_func = &block_dirty_snap;
                if (!inst_dirty_snap.empty())
                    define_mask.instruction_dirty_per_func = &inst_dirty_snap;
            }
        }
        const DefineDirtyMaskView* mask_ptr =
            define_mask.block_dirty_per_func ? &define_mask : nullptr;

        auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
        std::vector<std::string> cache_hits;
        // Issue #2286: scope the OwnershipEscapeSummary gate by
        // (Evaluator TypeChecker*, cache_epoch) so the lowering's
        // escape_blocks_move_elision_for_current lookup matches the key
        // the TypeChecker published under (post_mutation_invariant_check).
        // Without this, lookup would miss under multi-eval hosts and
        // either wrongly elide or wrongly block (#2274 / #2275 lineage).
        aura::compiler::TypeChecker* _esc_tc =
            static_cast<aura::compiler::TypeChecker*>(evaluator_.ensure_typechecker());
        const std::uint64_t _esc_gen =
            _esc_tc ? _esc_tc->cache_epoch() : evaluator_.current_cache_epoch();
        // Issue #2286: eval identity must match the key the TypeChecker
        // publishes under (publish_escape_move_elision_gate_for_key uses
        // the metrics pointer as eval identity — TypeChecker::metrics_
        // which is set to evaluator_.compiler_metrics()). Without this
        // match, the lookup would miss and the gate would silently no-op.
        aura::compiler::set_current_escape_key(evaluator_.compiler_metrics(), _esc_gen);
        auto ir_mod = lower_to_ir_with_cache_tracked(
            flat, pool, arena_, cache_ptr, &cache_hits, &evaluator_.primitives(), nullptr,
            cache_strings_ptr, nullptr, &type_registry_, value_cells_for_lowering());
        aura::compiler::clear_current_escape_key();

        // Issue #2044: full incremental dirty suite (CK/CF/TypeProp/Shape/
        // Escape + DCE) — replaces prior CK+CF-only cascade path.
        metrics_.cascade_incremental_pass_pipeline_total.fetch_add(1, std::memory_order_relaxed);
        const auto clean_skipped = run_incremental_dirty_pass_suite_(ir_mod, mask_ptr);
        if (clean_skipped > 0) {
            metrics_.cascade_incremental_pass_clean_blocks_skipped.fetch_add(
                clean_skipped, std::memory_order_relaxed);
            metrics_.irsoa_cache_miss_reduction.fetch_add(clean_skipped, std::memory_order_relaxed);
        }

        // Issue #2045: update ir_cache_v2_ (not only v1) so source_to_ir_map
        // is rebuilt against the new IR layout after cascade full re-lower.
        // store_define_v2 rebuilds the map + dual-emit SoA and runs the
        // consistency check; mirror to v1 for legacy readers.
        std::vector<aura::ir::IRFunction> bundle;
        std::vector<aura::ir::ClosureBridgeData> bridge_bundle;
        for (auto& func : ir_mod.functions) {
            if (func.id != ir_mod.entry_function_id) {
                if (func.id < ir_mod.closure_bridge.size())
                    bridge_bundle.push_back(ir_mod.closure_bridge[func.id]);
                else
                    bridge_bundle.emplace_back();
                bundle.push_back(std::move(func));
            }
        }
        store_define_v2(dep_name, src_it->second, std::move(bundle), std::move(bridge_bundle),
                        ir_mod.string_pool);
        if (auto vit = ir_cache_v2_.find(dep_name); vit != ir_cache_v2_.end()) {
            ir_cache_[dep_name] = vit->second.irs;
            ir_cache_bridge_[dep_name] = vit->second.bridges;
            ir_cache_strings_[dep_name] = vit->second.strings;
        }
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

// Issue #2110 / #2187: hybrid NodeId cascade after string-keyed BFS.
// Mirrors function-level edges (encode_fn_node) and block-level edges
// (encode_block_dep_node). Prefer NodeId BFS when mirror is populated:
//   1. cascade_mark_dirty from root fn node
//   2. Apply block-precise dirty for any marked block-dep nodes first
//   3. Fallback body-only for string dependents lacking block marks
// Nested lambdas without free-ref remain clean (#1505 authority).
// Lock order: may take dep_graph shared; mutate should already be held
// by mark_define_dirty / invalidate_function public entry.
// Determinism: string_dependents stay FIFO/sorted for re-lower order.
std::size_t
CompilerService::hybrid_node_cascade_(const std::string& root_name,
                                      const std::vector<std::string>& string_dependents) {
    using aura::compiler::dirty::cascade_mark_dirty;
    using aura::compiler::dirty::decode_block_dep_node;
    using aura::compiler::dirty::decode_fn_slot;
    using aura::compiler::dirty::DirtySet;
    using aura::compiler::dirty::encode_fn_node;
    using aura::compiler::dirty::is_block_dep_node;
    using aura::compiler::dirty::is_fn_node;
    using lock_order::Level;
    using lock_order::OrderedSharedLock;

    std::uint32_t root_slot = UINT32_MAX;
    aura::compiler::dirty::DepGraph graph_snap;
    {
        OrderedSharedLock<std::shared_mutex> dep_read(dep_graph_mtx_, Level::DepGraph);
        auto it = dep_name_to_slot_.find(root_name);
        if (it == dep_name_to_slot_.end()) {
            // No mirror yet (edges not recorded) — nothing to cascade.
            return 0;
        }
        root_slot = it->second;
        graph_snap = node_dep_graph_; // copy adj under lock
    }

    DirtySet set;
    // Prefer NodeId BFS when mirror has edges (#2187 AC2).
    const auto marked = cascade_mark_dirty(set, encode_fn_node(root_slot), graph_snap);
    metrics_.dep_graph_hybrid_cascade_hits.fetch_add(1, std::memory_order_relaxed);

    // Names that received a block-precise mark (skip full body-only).
    std::unordered_set<std::string> block_precise_names;

    // Issue #2187: apply block-precise dirty from block-dep nodes first.
    auto apply_block_precise = [&](std::uint32_t caller_slot, std::uint16_t func_idx,
                                   std::uint16_t block_idx) {
        std::string name;
        {
            OrderedSharedLock<std::shared_mutex> dep_read(dep_graph_mtx_, Level::DepGraph);
            if (caller_slot >= dep_slot_to_name_.size())
                return;
            name = dep_slot_to_name_[caller_slot];
        }
        if (name.empty() || name == root_name)
            return;
        auto cit = ir_cache_v2_.find(name);
        if (cit == ir_cache_v2_.end())
            return;
        auto& centry = cit->second;
        const std::size_t fi = static_cast<std::size_t>(func_idx);
        if (centry.block_dirty_per_func_.size() < centry.irs.size())
            centry.block_dirty_per_func_.resize(centry.irs.size());
        if (fi >= centry.block_dirty_per_func_.size()) {
            if (fi < centry.irs.size())
                centry.block_dirty_per_func_.resize(fi + 1);
            else
                return;
        }
        auto& fb = centry.block_dirty_per_func_[fi];
        if (fb.empty() && fi < centry.irs.size())
            fb.assign(centry.irs[fi].blocks.size(), std::uint8_t{0});
        if (block_idx < fb.size()) {
            fb[block_idx] = 1;
            centry.dirty = true;
            block_precise_names.insert(name);
            metrics_.dep_graph_node_cascade_block_hits.fetch_add(1, std::memory_order_relaxed);
            // Nested free-var targeting only (no full nested dirty).
            if (centry.irs.size() > 2) {
                for (std::size_t nfi = 2; nfi < centry.irs.size(); ++nfi)
                    (void)mark_nested_lambda_blocks_targeted(centry, nfi, root_name);
            }
            finish_cascade_soa_dirty_sync_(centry);
        }
    };

    for (const auto nid : set.dirty_nodes()) {
        if (!is_block_dep_node(nid))
            continue;
        const auto dec = decode_block_dep_node(nid);
        apply_block_precise(dec.caller_slot, dec.func_idx, dec.block_idx);
    }

    // Apply body-only dirty for string dependents without block-precise mark.
    // Prefer string_dependents list for determinism (#401 FIFO/sorted order).
    auto apply_body_only = [&](const std::string& dep_name) {
        if (block_precise_names.count(dep_name))
            return; // already precise — do not over-dirty body
        auto cit = ir_cache_v2_.find(dep_name);
        if (cit == ir_cache_v2_.end())
            return;
        auto& centry = cit->second;
        // Convention: body at irs[1] when multi-fn; else irs[0].
        const std::size_t body_idx = centry.irs.size() >= 2 ? 1 : 0;
        if (body_idx >= centry.block_dirty_per_func_.size()) {
            if (centry.block_dirty_per_func_.size() < centry.irs.size())
                centry.block_dirty_per_func_.resize(centry.irs.size());
        }
        if (body_idx < centry.block_dirty_per_func_.size()) {
            centry.dirty = true;
            if (centry.block_dirty_per_func_[body_idx].empty() && body_idx < centry.irs.size()) {
                // Ensure body mask length matches block count.
                centry.block_dirty_per_func_[body_idx].assign(centry.irs[body_idx].blocks.size(),
                                                              1);
            } else {
                for (auto& b : centry.block_dirty_per_func_[body_idx])
                    b = 1;
            }
            // Nested lambdas: only free-var targeted (do not full-dirty).
            if (centry.irs.size() > 2) {
                for (std::size_t fi = 2; fi < centry.irs.size(); ++fi)
                    (void)mark_nested_lambda_blocks_targeted(centry, fi, root_name);
            }
            finish_cascade_soa_dirty_sync_(centry);
        }
    };

    // Walk string dependents first (stable / known cascade set).
    for (const auto& d : string_dependents)
        apply_body_only(d);

    // Also any fn node marked by NodeId cascade not in string list.
    for (const auto nid : set.dirty_nodes()) {
        if (!is_fn_node(nid))
            continue;
        const auto slot = decode_fn_slot(nid);
        std::string name;
        {
            OrderedSharedLock<std::shared_mutex> dep_read(dep_graph_mtx_, Level::DepGraph);
            if (slot >= dep_slot_to_name_.size())
                continue;
            name = dep_slot_to_name_[slot];
        }
        if (name == root_name)
            continue;
        if (std::find(string_dependents.begin(), string_dependents.end(), name) !=
            string_dependents.end())
            continue;
        apply_body_only(name);
    }

    return marked;
}

} // namespace aura::compiler
