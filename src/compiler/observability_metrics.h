// observability_metrics.h — structured counters for self-evolving paths
// (Issue #62). The structs here are intentionally POD-ish so they
// can be serialized via aura::reflect::auto_to_json. Atomic counters
// for thread safety; the struct itself is updated with relaxed
// memory order — exact counts are advisory, not contractual.

#ifndef AURA_COMPILER_OBSERVABILITY_METRICS_H
#define AURA_COMPILER_OBSERVABILITY_METRICS_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

namespace aura::compiler {

// Issue #1797: consistent multi-field type-cache counter view.
// Used by compile:type-cache-stats so hits/misses/stale/gen_saved
// (and derived ratio_bp) are not mixed across concurrent typechecks.
struct TypeCacheStatsSnapshot {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t stale = 0;
    std::uint64_t gen_saved = 0;
};

// Top-level counters. Single instance per CompilerService.
// Note: counters are std::atomic<uint64_t> for thread safety
// (Issue #62 Iter 1). They serialize as plain integers via
// auto_to_json (the reflect framework reads the underlying value).
struct CompilerMetrics {
    // Deopt path (Issue #61 Iter 4): shape mismatch at function entry
    std::atomic<std::uint64_t> deopt_count{0};
    // L1 / L2 specialization (Issue #60): hit = fast-path matched,
    // miss = shape_id was Dynamic or wrong
    std::atomic<std::uint64_t> specialization_hits{0};
    std::atomic<std::uint64_t> specialization_misses{0};
    // ShapeProfiler: how often the dominant shape changed
    std::atomic<std::uint64_t> shape_changes_observed{0};
    // AuraJIT::compile()
    std::atomic<std::uint64_t> jit_compilations{0};
    std::atomic<std::uint64_t> jit_compile_misses{0};
    // jit_cache_ erase (from invalidate_function)
    std::atomic<std::uint64_t> jit_cache_evictions{0};
    // Issue #491: AuraJIT::invalidate / invalidate_prefix calls from
    // invalidate_function + hot-swap paths (hot-swap safety wiring).
    std::atomic<std::uint64_t> jit_hotswap_invalidate_total{0};
    // Issue #601: live IRClosure refresh / forced-deopt walk on
    // invalidate_function. Counts the proactive walk that runs
    // AFTER jit_hotswap_invalidate_total so closures about to be
    // applied don't trip the post-call `closure_needs_safe_fallback`
    // path; instead they're refreshed or force-deopt'd in place.
    //   - jit_hotswap_live_closure_refreshed_total: closure's
    //     bridge_epoch was successfully updated to the current epoch
    //     (the closure can safely continue executing).
    //   - jit_hotswap_forced_deopt_total: closure's bridge_epoch
    //     was reset to 0 (legacy / stale sentinel) so future
    //     apply_closure calls hit `closure_needs_safe_fallback`
    //     and deopt to bridge/interpreter; the closure cannot
    //     continue under the old func_id's bridge.
    //   - jit_hotswap_epoch_mismatch_prevented_total: union of the
    //     above (closures that had a stale bridge_epoch caught
    //     proactively before a stale apply could occur).
    std::atomic<std::uint64_t> jit_hotswap_live_closure_refreshed_total{0};
    std::atomic<std::uint64_t> jit_hotswap_forced_deopt_total{0};
    std::atomic<std::uint64_t> jit_hotswap_epoch_mismatch_prevented_total{0};
    // Issue #493: EDSL hot-path bottleneck measurement (evaluator_eval_flat /
    // lowering_impl call sites).
    std::atomic<std::uint64_t> hotpath_eval_flat_calls{0};
    std::atomic<std::uint64_t> hotpath_lowering_calls{0};
    std::atomic<std::uint64_t> hotpath_soa_dual_emit_hits{0};
    // --emit-binary
    std::atomic<std::uint64_t> aot_emits{0};
    std::atomic<std::uint64_t> aot_fallbacks{0};
    // ArenaGroup::total_stats() snapshot
    std::atomic<std::uint64_t> arena_bytes_used{0};
    std::atomic<std::uint64_t> arena_bytes_peak{0};
    // Issue #125: per-module dirty-skip optimization. When a
    // module is unchanged (clean), reload_module skips the
    // re-compile. These counters track the skip vs. recompile
    // decision, exposed via CompilerService::snapshot() for
    // --evo-explain.
    std::atomic<std::uint64_t> module_dirty_skips{0};
    std::atomic<std::uint64_t> module_dirty_recompiles{0};
    // Issue #224: per-block re-lower consumer. The helper
    // relower_define_blocks() consults the per-block bitmask
    // (Issue #196): if zero dirty blocks, the cached IR
    // bundle is reused as-is (no lowering call). The skip
    // counter tracks saves; the full counter tracks when
    // a re-lower was actually needed. Both exposed for
    // benchmarking / --evo-explain.
    std::atomic<std::uint64_t> relower_skipped_entirely_count{0};
    std::atomic<std::uint64_t> relower_full_called_count{0};
    // Issue #272: function defines bound via IRInterpreter (not eval_flat).
    std::atomic<std::uint64_t> define_ir_env_bind_count{0};
    // Issue #272 Cycle 3: value defines bound via IRInterpreter (not eval_flat).
    std::atomic<std::uint64_t> value_define_ir_env_bind_count{0};
    // Issue #224 cycle 3: per-function re-lower. The helper
    // relower_define_function() re-lowers a single Lambda
    // from a cached entry's bundle (one of N functions) and
    // replaces it in ir_cache_v2_ without touching the rest
    // of the bundle. Bumped when per-function re-lower is
    // actually performed (replaces the full re-lower path).
    std::atomic<std::uint64_t> relower_per_function_called_count{0};
    // Issue #1474: per-block re-lower observability.
    std::atomic<std::uint64_t> incremental_relower_blocks_total{0};
    // Issue #1623: production eval / eval_ir hot-path partial re-lower
    // (cache_define_prefer_partial → relower_define_blocks success).
    //   - incremental_eval_relower_hits: partial success from EDSL define path
    //   - eval_path_relower_total: eval() define-path partial attempts/wins
    //   - eval_ir_path_relower_total: eval_ir() define-path partial wins
    std::atomic<std::uint64_t> incremental_eval_relower_hits{0};
    std::atomic<std::uint64_t> eval_path_relower_total{0};
    std::atomic<std::uint64_t> eval_ir_path_relower_total{0};
    // Issue #1657: finer-grained per-instruction dirty bitmask
    // propagation and minimal re-lower strategy in ir_cache_v2_ /
    // relower_define_*. See scripts/check_fine_dirty_relower_coverage.py
    // for the source-level invariants enforced by the linter.
    //   - relower_instruction_level_hits: how many times the
    //     instruction-level minimal re-lower path succeeded
    //     (preserves func_id, runs per-func passes only on dirty
    //     instructions instead of full re-lower)
    //   - dep_graph_edge_miss_count: dep_graph_ populate/record
    //     missed a Quote + Lambda free-var + macro-expanded subtree
    //     reference (caller-side fallback to coarse full dirty)
    //   - soa_dirty_sync_total: count of
    //     sync_instruction_dirty_from_block_dirty() invocations
    //     (block→instruction dirty propagation)
    //   - soa_consistency_partial_dirty_total: count of
    //     consistency-mismatch handlers that correctly dirty only
    //     affected functions instead of mark_all_blocks_dirty() on
    //     the whole SoA module
    std::atomic<std::uint64_t> relower_instruction_level_hits{0};
    std::atomic<std::uint64_t> dep_graph_edge_miss_count{0};
    std::atomic<std::uint64_t> soa_dirty_sync_total{0};
    std::atomic<std::uint64_t> soa_consistency_partial_dirty_total{0};
    // Issue #1514: clean functions skipped by per-function re-lower.
    std::atomic<std::uint64_t> relower_partial_funcs_saved_total{0};
    // Issue #1514: JIT partial_recompile requests from relower path.
    std::atomic<std::uint64_t> jit_partial_recompile_requests_total{0};
    // Issue #401: invalidate_function call counter. Bumped once per
    // invalidate_function entry (post-mutex-acquire, so the count
    // reflects only completed traversals). Pairs with jit_cache_evictions
    // (which counts evictions for the root + each dependent): the ratio
    // (jit_cache_evictions - invalidate_function_calls) / invalidate_function_calls
    // tells you the average dependent fan-out per invalidation.
    std::atomic<std::uint64_t> invalidate_function_calls{0};
    // Issue #1477: JIT-side dual-epoch fence observability. Pairs
    // with compiler_closure_epoch_mismatch_hits (IR side) so
    // dashboards can verify both reader paths see the same epoch.
    //   - jit_epoch_stale_check_total: lifetime total of
    //     AuraJIT::capture_fn_epoch + is_fn_epoch_stale calls
    //     (one per JIT compile + one per JIT Apply prologue
    //     check). Sustained high value vs. the captured-epoch
    //     miss rate (jit_epoch_stale_hits / jit_epoch_stale_check_total)
    //     indicates how often the JIT path sees a stale fn.
    //     Also bumped by walk_active_closures (#1536) once per stale fn.
    std::atomic<std::uint64_t> jit_epoch_stale_check_total{0};
    // Issue #1536: bulk walk_active_closures after invalidate / dirty mark.
    std::atomic<std::uint64_t> jit_walk_active_closures_total{0};
    std::atomic<std::uint64_t> jit_walk_active_closures_stale_total{0};
    // Issue #1476 / #1496: unified mark_define_dirty / invalidate_function
    // atomic protocol observability.
    //   - bridge_epoch_bumps_total: lifetime total of bump_bridge_epoch()
    //     calls (one per mark_define_dirty + one per invalidate_function +
    //     one per cascade dependent). Pairs with mutation_epoch_ acquire-
    //     load counters (compiler_closure_epoch_mismatch_hits) to verify
    //     epoch progress vs. catch-up.
    //   - invalidate_cascade_depth_max: high-water mark of BFS cascade
    //     depth from a single mark_define_dirty root. Sustained high
    //     values indicate hot dep_graph edges (many siblings depend
    //     on the same mutated define).
    //   - unified_invalidation_protocol_total: atomic_bump_epochs_and_stamp_bridge
    //     entries (#1496 AC — both soft dirty + hard invalidate paths).
    //   - invalidate_cascade_depth_total: sum of BFS depths (avg =
    //     total / mark_define_dirty roots via bridge_epoch_bumps ratio).
    std::atomic<std::uint64_t> bridge_epoch_bumps_total{0};
    std::atomic<std::uint64_t> invalidate_cascade_depth_max{0};
    std::atomic<std::uint64_t> unified_invalidation_protocol_total{0};
    std::atomic<std::uint64_t> invalidate_cascade_depth_total{0};
    // Issue #1627: soft+hard shared pre-cascade prepare (live closure
    // scan + linear enforce + GC root audit) before epoch publish.
    std::atomic<std::uint64_t> invalidate_pre_cascade_prepare_total{0};
    // Issue #402: needs_tree_walker_fallback counters. The
    // (call) counter is bumped on every invocation (incl.
    // early returns). The (fast_path) counter bumps when
    // summary_flags_ == 0 AND the subtree walk decides. The
    // (slow_path) counter bumps when summary_flags_ != 0 and
    // we fall back to the O(flat.size()) scan. fast_path /
    // (fast_path + slow_path) gives the fast-path hit ratio.
    std::atomic<std::uint64_t> needs_tree_walker_fallback_calls{0};
    std::atomic<std::uint64_t> needs_tree_walker_fast_path_hits{0};
    std::atomic<std::uint64_t> needs_tree_walker_slow_path_hits{0};
    // Issue #224 cycle 4: dep_graph_-aware cascade. When
    // mark_define_dirty cascades a mutation to a dependent,
    // it tries to mark only the body function's blocks dirty
    // (irs[1], the Lambda body) instead of the whole entry.
    // Bumped per successful targeted cascade. The fallback
    // (cascade_full_count) tracks dependents that didn't fit
    // the convention and required the whole-entry mark.
    std::atomic<std::uint64_t> cascade_body_only_count{0};
    std::atomic<std::uint64_t> cascade_full_count{0};
    // Issue #225 cycle 3: bridge invalidation. Bumped each
    // time invalidate_bridge_for() is called (i.e. on
    // mark_define_dirty, invalidate_function, and
    // hot_swap_function_impl). The total count gives
    // observability into how often the bridge_epoch_ field
    // is bumped across the lifetime of the service.
    std::atomic<std::uint64_t> bridge_invalidations_count{0};
    // Issue #252: closure dual-path observability. The
    // counters live on CompilerMetrics (shared by all
    // dispatch paths) rather than on Evaluator, so the
    // IR's IROpcode::Call / Apply can bump them too. The
    // Evaluator's apply_closure also bumps calls_total +
    // the FFI / TW / bridge-specific counters (only the
    // TW / FFI / bridge-specific paths use apply_closure).
    // - closure_calls_total: every closure dispatch
    //   (sum of ffi_calls + tw_calls + ir_calls + bridge_calls)
    // - closure_ffi_calls: FFI-dispatched (in apply_closure)
    // - closure_tw_calls: tree-walker closures_ map hit
    //   (in apply_closure)
    // - closure_ir_calls: IR interpreter path (runtime_closures_)
    // - closure_bridge_calls: closure_bridge_ (IR/JIT bridge
    //   callback) — when a CompilerService has a bridge set
    // - closure_stale_returns: stale-bridge nullopt returns
    //   (Issue #223, in apply_closure)
    std::atomic<std::uint64_t> closure_calls_total{0};
    std::atomic<std::uint64_t> closure_ffi_calls{0};
    std::atomic<std::uint64_t> closure_tw_calls{0};
    std::atomic<std::uint64_t> closure_ir_calls{0};
    std::atomic<std::uint64_t> closure_bridge_calls{0};
    std::atomic<std::uint64_t> closure_stale_returns{0};
    // Issue #681: compiler IRClosure/bridge epoch enforcement on
    // invalidate_function + apply_closure dual-path.
    //   - compiler_inval_bridge_epoch_total: bridge entries expired
    //     in invalidate_bridge_for (shared_ptr reset + epoch bump)
    //   - compiler_closure_epoch_mismatch_hits: pre-call stale
    //     bridge_epoch or EnvFrame version mismatch detected
    //   - compiler_closure_safe_fallbacks: deopt to bridge /
    //     interpreter / nullopt instead of stale execution
    std::atomic<std::uint64_t> compiler_inval_bridge_epoch_total{0};
    std::atomic<std::uint64_t> compiler_closure_epoch_mismatch_hits{0};
    std::atomic<std::uint64_t> compiler_closure_safe_fallbacks{0};
    // Issue #1626: EnvFrame-domain stale on apply_closure dual-check
    // (distinct from bridge_epoch mismatch). Bumped when
    // is_env_frame_stale / invalid at map/bridge/IR apply entry.
    std::atomic<std::uint64_t> compiler_closure_envframe_stale_total{0};
    // Issue #1508: JIT aura_closure_call dual check (bridge_epoch +
    // env_frame/defuse) + interpreter-deopt fallback counters.
    //   - jit_closure_dual_check_total: every is_jit_closure_fresh probe
    //   - jit_closure_stale_deopt_total: stale → refuse/deopt path
    //   - jit_closure_safe_fallbacks: safe non-UAF fallbacks after stale
    std::atomic<std::uint64_t> jit_closure_dual_check_total{0};
    std::atomic<std::uint64_t> jit_closure_stale_deopt_total{0};
    std::atomic<std::uint64_t> jit_closure_safe_fallbacks{0};
    // Issue #1522: fn_trackers_ batch_deopt notify (aliases AC names *_total).
    // jit_closure_safe_fallbacks_total mirrors jit_closure_safe_fallbacks
    // (kept as distinct field for Agent queries that use the AC name).
    std::atomic<std::uint64_t> jit_closure_safe_fallbacks_total{0};
    std::atomic<std::uint64_t> jit_fn_trackers_batch_deopt_total{0};
    std::atomic<std::uint64_t> jit_fn_trackers_entries_marked_total{0};
    // Issue #1523: lock-order audit (#1388 canonical:
    // mutate → workspace → env_frames → dep_graph).
    //   - lock_inversion_detected_total: acquire while a higher level held
    //   - mutate_mtx_contended_total: try_lock failed on mutate_mtx_
    std::atomic<std::uint64_t> lock_inversion_detected_total{0};
    std::atomic<std::uint64_t> mutate_mtx_contended_total{0};
    // Issue #1509: multi-fiber mutate→call-stale apply stress metrics.
    //   - closure_stale_apply_count_total: apply_closure saw stale
    //     bridge_epoch and/or EnvFrame (pre-dispatch)
    //   - closure_safe_fallback_apply_count_total: took safe fallback
    //     (bridge re-apply or nullopt) instead of dangling eval
    //   - closure_race_caught_count_total: concurrent steal/mutate
    //     race caught by epoch fence / stale-on-steal path
    std::atomic<std::uint64_t> closure_stale_apply_count_total{0};
    std::atomic<std::uint64_t> closure_safe_fallback_apply_count_total{0};
    std::atomic<std::uint64_t> closure_race_caught_count_total{0};
    // Issue #1485: AC3 — force bridge_epoch + defuse_version_ checks
    // in apply_closure dual-path + JIT aura_closure_call (refine #1475).
    // 2 new atomics for the new P0 safety surface:
    //   - stale_closure_prevented: lifetime count of stale closures
    //     detected at apply_closure entry (complements
    //     closure_stale_apply_count_total which is bumped inside
    //     closure_needs_safe_fallback; this one is bumped at the
    //     apply_closure entry AFTER the helper returns true, as a
    //     top-level "we caught a stale closure before dispatch"
    //     signal — distinct from the per-check breakdowns the
    //     helper bumps)
    //   - closure_epoch_mismatch_fallback: lifetime count of
    //     safe-fallback paths taken after a stale closure was
    //     detected (complements closure_safe_fallback_apply_count_
    //     total which counts ANY safe fallback; this one is
    //     specific to the bridge_epoch / defuse_version_ mismatch
    //     surface — distinct from linear_post_mutate_enforce
    //     fallbacks which are counted separately)
    std::atomic<std::uint64_t> stale_closure_prevented{0};
    std::atomic<std::uint64_t> closure_epoch_mismatch_fallback{0};
    // Issue #1525: multi-fiber concurrent mutate + eval old closure stress.
    // Counterpart to #1509 (read-side) — focuses on mutate_mtx_ serialization
    // visibility + dual-epoch races under parallel workers.
    //   - multifiber_mutate_races_detected_total: stale/epoch race observed
    //     while concurrent mutate/steal is active (apply or fiber-steal probe)
    //   - multifiber_safe_fallback_total: safe fallback taken under that race
    //     (bridge re-path / refuse / interpreter fallback — never UAF)
    std::atomic<std::uint64_t> multifiber_mutate_races_detected_total{0};
    std::atomic<std::uint64_t> multifiber_safe_fallback_total{0};
    // Issue #1510 / #1526: compact_env_frames ↔ materialize_call_env +
    // hot-swap dual-epoch cooperation.
    //   - envframe_compact_rewrites_total: Closure::env_id (+ parent_id)
    //     rewrites under compact_env_frames_lock_
    //   - envframe_compact_epoch_bumps_total: defuse+bridge (+AOT table)
    //     epoch bumps paired with each compact (atomic post-rewrite)
    //   - envframe_compact_bridge_restamps_total: Closure / IRClosure
    //     bridge_epoch restamped to post-compact epoch (Issue #1526)
    //   - materialize_fallback_total: materialize_call_env returned
    //     empty/fallback Env (NULL/OOB/INVALID/post-compact stale)
    std::atomic<std::uint64_t> envframe_compact_rewrites_total{0};
    std::atomic<std::uint64_t> envframe_compact_epoch_bumps_total{0};
    std::atomic<std::uint64_t> envframe_compact_bridge_restamps_total{0};
    std::atomic<std::uint64_t> materialize_fallback_total{0};
    // Issue #1511: dual check on closure_bridge_ callback entry
    // (local-miss + local-stale recovery paths).
    //   - closure_bridge_fallback_stale_total: bridge entry saw
    //     stale bridge_epoch and/or EnvFrame version_
    //   - closure_bridge_safe_fallbacks_total: took safe recovery
    //     (re-stamp / bridge re-dispatch / refuse) instead of UAF
    std::atomic<std::uint64_t> closure_bridge_fallback_stale_total{0};
    std::atomic<std::uint64_t> closure_bridge_safe_fallbacks_total{0};
    // Issue #1512: JIT opcode coverage + IRInterpreter consistency.
    // Mirrored from AuraJIT::Metrics for Agent query surfaces.
    std::atomic<std::uint64_t> jit_opcode_covered_mask{0};
    std::atomic<std::uint64_t> jit_opcode_unhandled_mask{0};
    std::atomic<std::uint64_t> jit_consistency_compare_total{0};
    std::atomic<std::uint64_t> jit_consistency_match_total{0};
    std::atomic<std::uint64_t> jit_consistency_violations_total{0};
    // Issue #1513: IRClosure dual-check + invalidate expire.
    //   - ir_closure_env_version_stale_total: apply saw env_version/id stale
    //   - ir_closure_invalidate_expired_total: live IRClosures expired on
    //     invalidate_function (flat/pool cleared, left epoch stale)
    std::atomic<std::uint64_t> ir_closure_env_version_stale_total{0};
    std::atomic<std::uint64_t> ir_closure_invalidate_expired_total{0};
    // Issue #739: atomic epoch visibility under fiber steal.
    //   - epoch_stale_steal_caught: stale bridge_epoch detected
    //     after acquire fence in apply_closure / IR paths
    //   - closure_epoch_fence_enforced_total: acquire fences
    //     executed before epoch staleness checks
    //   - linear_violation_prevented_epoch_total: linear /
    //     ownership violations prevented by epoch stale catch
    std::atomic<std::uint64_t> epoch_stale_steal_caught{0};
    std::atomic<std::uint64_t> closure_epoch_fence_enforced_total{0};
    std::atomic<std::uint64_t> linear_violation_prevented_epoch_total{0};
    // Issue #682: compiler IRClosure/EnvId GC root coordination on
    // invalidate / hot-swap / safepoint.
    //   - ir_closure_roots_registered: roots pinned in last
    //     flush_compiler_gc_roots (bridge epoch current)
    //   - hotswap_root_miss: invalidate/hot-swap with no bridge
    //     or persistent IR binding to refresh
    //   - compiler_gc_safepoint_defer_count: sweep deferred due
    //     to live IR interpreter frames during invalidate
    std::atomic<std::uint64_t> ir_closure_roots_registered{0};
    std::atomic<std::uint64_t> hotswap_root_miss{0};
    std::atomic<std::uint64_t> compiler_gc_safepoint_defer_count{0};
    // Issue #253: linear-move elision count (lifetime total).
    // Bumped by TypeSpecializationWrap after each run (in
    // service.ixx — the pass has its own per-run accumulator;
    // service.ixx copies it here so snapshot() and the Aura
    // primitive read a single source of truth). The actual
    // elision logic is in pass_manager.ixx.
    std::atomic<std::uint64_t> linear_elide_count{0};
    // Issue #433: dead coercion elimination
    // observability. The pass eliminates CastOps whose
    // source type == target type (identity) or that
    // sit between two casts of the same source (chain).
    // The lifetime total is accumulated in service.ixx
    // after each dce.run() call. Pre-#433 the pass
    // existed (pass_manager.ixx:705) and was wired
    // into the pipeline (service.ixx:1442) but the
    // eliminated_count was never surfaced to the user
    // — the metric lived only on the per-call pass
    // instance.
    std::atomic<std::uint64_t> dead_coercion_eliminated_total{0};
    // Issue #508: lifetime microseconds spent in
    // DeadCoercionEliminationPass::run() across all calls.
    // Mirrors the per-call elapsed_us() on the pass, summed
    // here. Read via (compile:dead-coercion-elapsed).
    std::atomic<std::uint64_t> dead_coercion_elapsed_us_total{0};
    // Issue #508: count of CastOps that would have been
    // eliminated when keep_for_debug was set (so the pass
    // counted but didn't act). Useful for "blame mode"
    // observability — lets the user see what the pass
    // would have done.
    std::atomic<std::uint64_t> dead_coercion_kept_for_debug_total{0};
    // Issue #687: IR-interpreter CastOp identity
    // passthrough fast-path. Bumped when the interpreter
    // skips the 7-branch switch because the source value
    // is already a Dynamic (type_tag >= 3) — the cast is
    // a no-op at runtime. Companion to
    // dead_coercion_eliminated_total (the lowering pass)
    // so the Agent can compute runtime_savings_ratio =
    // post_mutate_elim_hits / eliminated.
    std::atomic<std::uint64_t> dead_coercion_post_mutate_elim_hits_total{0};

    // Issue #462: ShapeAwareFoldingPass metrics (lifetime totals).
    // Bumped in service.ixx after each ShapeAwareFoldingPass::run.
    //   - shape_fold_count: total # of instructions replaced
    //     (OpNop'd) due to shape/linear/narrow metadata
    //   - shape_linear_elide_count: subset of fold_count due
    //     to linear-ownership elision (MoveOp on non-escaping
    //     Owned slot is a no-op)
    //   - shape_narrow_check_count: # of redundant type-checks
    //     detected (counted, not yet rewritten in Cycle 1)
    //   - guard_shape_hits: # of GuardShape instructions seen
    //     in the module (downstream-pass signal)
    std::atomic<std::uint64_t> shape_fold_count{0};
    std::atomic<std::uint64_t> shape_linear_elide_count{0};
    std::atomic<std::uint64_t> shape_narrow_check_count{0};
    std::atomic<std::uint64_t> guard_shape_hits{0};
    // Issue #1661: specialized_for / GuardShape collaborative fold opportunities.
    std::atomic<std::uint64_t> shape_specialized_fold_opportunities{0};

    // Issue #463: SoA Phase 2 adoption counters (lifetime totals).
    // Bumped in service.ixx by SoAtoAoSBridgePass accumulators
    // after each compile. The SoA scaffold (#167 / #429) is in
    // place but adoption is still opt-in; these counters let the
    // AI Agent monitor the rollout.
    //   - soa_functions_visited: # of SoA functions walked by
    //     the bridge pass
    //   - soa_instructions_visited: # of SoA instructions
    //     walked (sum across all functions)
    //   - aos_view_built_count: # of SoA→AoS view conversions
    //     (should equal soa_functions_visited; the counter
    //     exists separately so a future Pass can build multiple
    //     AoS views per function if needed)
    std::atomic<std::uint64_t> soa_functions_visited{0};
    std::atomic<std::uint64_t> soa_instructions_visited{0};
    std::atomic<std::uint64_t> aos_view_built_count{0};

    // Issue #629: zero-overhead coercion path observability.
    //   - coercion_castop_emitted_total: CastOps inserted by
    //     TypeSpecializationWrap
    //   - coercion_type_prop_hits_total: DCE Rule 1 elisions
    //     (source type_id == target type_id)
    //   - coercion_narrow_evidence_hits_total: DCE Rule 6
    //     elisions, TypeSpec narrow_evidence branch skips,
    //     and interpreter GuardShape narrow fast-path hits
    //   - coercion_zerooverhead_win_total: per-pipeline-run
    //     sum of type_prop_hits + narrow_evidence_hits (DCE)
    //     + narrow_evidence_skipped (TypeSpec)
    std::atomic<std::uint64_t> coercion_castop_emitted_total{0};
    std::atomic<std::uint64_t> coercion_type_prop_hits_total{0};
    std::atomic<std::uint64_t> coercion_narrow_evidence_hits_total{0};
    std::atomic<std::uint64_t> coercion_zerooverhead_win_total{0};
    // Issue #691: CoercionMap + NarrowingRecord provenance linkage.
    //   - coercion_post_narrow_elim_opportunities_total: deferred
    //     coercions recorded with narrowing evidence/provenance
    //   - coercion_narrow_blame_chain_hits_total: entries with both
    //     predicate_cond_node + source_mutation_id populated
    //   - coercion_cast_elim_from_narrow_total: lowering elided a
    //     CastOp because post-narrow evidence matched concrete types
    std::atomic<std::uint64_t> coercion_post_narrow_elim_opportunities_total{0};
    std::atomic<std::uint64_t> coercion_narrow_blame_chain_hits_total{0};
    std::atomic<std::uint64_t> coercion_cast_elim_from_narrow_total{0};
    // Issue #487: dirty propagation + IR re-lower
    // observability. 2 lifetime counters:
    //   - should_relower_total: count of times
    //     should_relower() returned true (re-lower
    //     triggered on dirty). Pre-#487 the
    //     should_relower decision was in
    //     lookup_define_v2 but not surfaced.
    //   - affected_subtree_total: count of times
    //     affected_subtree_from_mutation was
    //     called (the entry point for the dirty
    //     propagation path). The ratio
    //     should_relower / affected_subtree measures
    //     the dirty-trigger rate.
    std::atomic<std::uint64_t> should_relower_total{0};
    std::atomic<std::uint64_t> affected_subtree_total{0};
    // Issue #387: Type Dependency Graph observability (3
    // lifetime-total counters, threaded from TypeChecker
    // through CompilerService::typecheck_full() / incremental_infer).
    // type_dep_graph_lookups = total queries to
    //                            affected_nodes_for_type()
    // type_dep_graph_hits = queries that found >= 1 dependent
    //                        node (real hit)
    // type_dep_graph_size = peak number of distinct TypeIds
    //                       tracked (snapshot, not lifetime)
    std::atomic<std::uint64_t> type_dep_graph_lookups{0};
    std::atomic<std::uint64_t> type_dep_graph_hits{0};
    std::atomic<std::uint64_t> type_dep_graph_size{0};
    // Issue #254: IR SoA dual-emit counters (lifetime total).
    // Bumped by service.ixx after each lower_to_ir call when
    // dual-emit is enabled. The underlying counters live on
    // LoweringState (soa_instructions_emitted /
    // soa_functions_emitted). service.ixx accumulates them
    // into metrics_ so snapshot() + the Aura primitive read
    // a single source of truth.
    std::atomic<std::uint64_t> ir_soa_instructions_emitted{0};
    std::atomic<std::uint64_t> ir_soa_functions_emitted{0};
    // Issue #603: full consumer adoption + per-block dirty-driven
    // minimal re-lower observability. Companion to the existing
    // (compile:ir-soa-stats) hash primitive — those two fields
    // cover dual-emit volumes; the three below cover the hot-
    // path consumer side.
    //   - ir_soa_view_cache_hits_total: SoA column iterations
    //     where an instruction index was accessed via the
    //     IRFunctionSoA columns (cheap column reads, the
    //     view-equivalent access in the pass_manager hot loop).
    //     Each instruction touched = +1.
    //   - ir_soa_block_dirty_hits_total: is_block_dirty() calls
    //     that returned true (block needs re-lower). Counterpart
    //     to the existing dirty-cascade-savings (skip-side).
    //   - ir_soa_relower_blocks_saved_total: blocks skipped
    //     from re-lower because they were clean (the win from
    //     per-block dirty_ driven minimal re-lower). Distinct
    //     from relower_skipped_entirely_count which is at the
    //     function granularity.
    std::atomic<std::uint64_t> ir_soa_view_cache_hits_total{0};
    std::atomic<std::uint64_t> ir_soa_block_dirty_hits_total{0};
    std::atomic<std::uint64_t> ir_soa_relower_blocks_saved_total{0};
    // Issue #684: IRSoA full wiring into lowering/cache/JIT hot paths.
    // Exposed via (query:irsoa-incremental-stats).
    std::atomic<std::uint64_t> irsoa_wired_hits{0};
    std::atomic<std::uint64_t> irsoa_dirty_cascade_savings{0};
    std::atomic<std::uint64_t> irsoa_cache_miss_reduction{0};
    // Issue #686: ShapeProfiler stable-shape → IRSoA shape_ids_ sync.
    std::atomic<std::uint64_t> shape_ids_sync_hits{0};
    // Issue #255: reference stability observability. The
    // FlatAST reference stability mechanism (generation_ +
    // node_gen_ + StableNodeRef) is a candidate for a
    // std::meta-based refactor once P2996 lands in a compiler.
    // Until then, these counters let us audit how often the
    // mechanism fires in real workloads:
    // - bump_generation_count: total generation bumps
    //   (from bump_generation() / commit_atomic_batch()).
    //   Counts actual bumps, not suppressed ones (the
    //   bump_generation_suppressed_ flag is for #250 atomic
    //   batches and is observed separately).
    // - is_valid_check_count: total is_valid() calls
    //   (both NodeId and StableNodeRef overloads).
    // - stable_ref_invalidations: how many StableNodeRef
    //   is_valid() calls returned false (i.e. a captured
    //   ref went stale — the whole point of StableNodeRef).
    // - atomic_batch_commits: how many atomic batches
    //   committed (one bump per batch, vs N bumps for N
    //   individual mutates — the savings show up as
    //   bump_generation_count / atomic_batch_commits being
    //   much lower than the mutate count).
    std::atomic<std::uint64_t> bump_generation_count{0};
    std::atomic<std::uint64_t> is_valid_check_count{0};
    std::atomic<std::uint64_t> stable_ref_invalidations{0};
    std::atomic<std::uint64_t> atomic_batch_commits{0};
    // Issue #1900 AC3: dispatch-coverage + interleaving-prevention
    // telemetry. unsupported_op_total only ever bumps when a future
    // mutate primitive lands before its lockless helper ships (the
    // 14-op dispatch is now complete). interleaved_mutation_prevented
    // bumps on every successful commit (the outer MutationBoundaryGuard
    // held workspace_mtx_ unique_lock for the entire batch, serializing
    // any concurrent mutator).
    std::atomic<std::uint64_t> atomic_batch_unsupported_op_total{0};
    std::atomic<std::uint64_t> atomic_batch_interleaved_mutation_prevented{0};
    // Issue #1878: multi-tenant atomic-batch observability.
    // weak_atomicity_used stays 0 on the default strong path (outer
    // MutationBoundaryGuard holds workspace_mtx_ for the whole batch);
    // reserved for any future opt-in weak mode.
    std::atomic<std::uint64_t> atomic_batch_weak_atomicity_used{0};
    // Successful batch commits that ran under strong atomicity (#1900/#1878).
    std::atomic<std::uint64_t> atomic_batch_strong_atomicity_commits{0};
    // Cross-tenant isolation denials at atomic-batch entry under Strict.
    std::atomic<std::uint64_t> atomic_batch_tenant_isolation_denials{0};
    // Issue #256: AST operation observability. See ast.ixx
    // for the bump sites + semantics. The ratio
    // mark_dirty_total_nodes / mark_dirty_upward_call_count
    // gives the average dirty-propagation depth per
    // mutation — the key metric for whether a future
    // std::meta-based AST operation refactor is worth it.
    std::atomic<std::uint64_t> children_call_count{0};
    std::atomic<std::uint64_t> parent_of_call_count{0};
    std::atomic<std::uint64_t> mark_dirty_upward_call_count{0};
    std::atomic<std::uint64_t> mark_dirty_total_nodes{0};
    // Issue #258: multi-mutation incremental type checking
    // observability. The IncrementalStats on TypeChecker
    // are per-call (TypeChecker is short-lived). These 4
    // lifetime totals accumulate the stats across every
    // typecheck_full() / incremental_infer() call so users
    // can see how the typecheck cache is performing across
    // multi-mutation workloads:
    // - typecheck_cache_hits_total: clean nodes with valid
    //   cached types (skipped re-inference)
    // - typecheck_cache_misses_total: nodes that were
    //   re-inferred (dirty or no cache)
    // - typecheck_stale_cache_total: cached types that
    //   contained free type vars (rejected — pre-solve
    //   cache pollution)
    // - delta_solve_time_us: cumulative microseconds spent
    //   in ConstraintSystem::solve_delta() (Issue #148
    //   Phase 2). Useful for profiling the multi-mutation
    //   cost.
    // The derived metric multi_mutation_recompute_ratio is
    // computed at snapshot read time as
    //   cache_misses / (hits + misses + stale)
    // — the AC1 metric from #258.
    std::atomic<std::uint64_t> typecheck_cache_hits_total{0};
    std::atomic<std::uint64_t> typecheck_cache_misses_total{0};
    std::atomic<std::uint64_t> typecheck_stale_cache_total{0};
    // Issue #412: cache hits rescued by the generation
    // counter check (post-#412 only). Pre-#412 these were
    // counted as stale_cache. The derived
    // gen_saved_ratio_bp = gen_saved / (stale_cache +
    // gen_saved) * 10000 in basis points measures the
    // improvement (higher = more false-positive stale
    // rejections eliminated). gen_saved_total is a
    // lifetime counter; the snapshot mirrors it.
    std::atomic<std::uint64_t> typecheck_gen_saved_total{0};

    // Issue #1797: load the 4 type-cache counters as one logical
    // snapshot. Double-check acquire loop: if any counter moves
    // between the first and second full read, retry (up to 16×).
    // Avoids mixed-epoch ratio_bp without a writer-side mutex
    // (counters remain lock-free fetch_add on the hot path).
    [[nodiscard]] TypeCacheStatsSnapshot snapshot_type_cache_stats() const noexcept {
        TypeCacheStatsSnapshot s;
        for (int attempt = 0; attempt < 16; ++attempt) {
            s.hits = typecheck_cache_hits_total.load(std::memory_order_acquire);
            s.misses = typecheck_cache_misses_total.load(std::memory_order_acquire);
            s.stale = typecheck_stale_cache_total.load(std::memory_order_acquire);
            s.gen_saved = typecheck_gen_saved_total.load(std::memory_order_acquire);
            if (typecheck_cache_hits_total.load(std::memory_order_acquire) == s.hits &&
                typecheck_cache_misses_total.load(std::memory_order_acquire) == s.misses &&
                typecheck_stale_cache_total.load(std::memory_order_acquire) == s.stale &&
                typecheck_gen_saved_total.load(std::memory_order_acquire) == s.gen_saved) {
                return s;
            }
        }
        return s; // best-effort after retries
    }

    // Issue #412 follow-up #1: per-binding gen check
    // observability. The full #412 follow-up #1 scope
    // is to replace the global `type_cache_generation_`
    // with per-binding gen tracking (finer invalidation
    // signal that only bumps on structural changes to
    // THAT specific binding). This scope-limited slice
    // ships the wiring + observability so the
    // optimization can be measured.
    //
    //   - per_binding_gen_hits_total: cache hits
    //     accepted because the per-binding gen matched
    //     (the global gen had advanced but THIS
    //     binding hadn't changed). Pre-#412 follow-up
    //     #1 these would have been `stale_cache`
    //     rejections.
    //   - per_binding_gen_bumps_total: total
    //     per-binding gen bumps (one per mark_dirty_upward
    //     on a binding node). The ratio of bumps to
    //     dirty events tells the user how many bumps are
    //     binding-specific (vs. every dirty event
    //     bumping the global gen).
    std::atomic<std::uint64_t> per_binding_gen_hits_total{0};
    // Issue #413: mutation_log-integrated invalidation
    // trace. The lifetime total of (mutation_id,
    // SymId, binding_gen) triples recorded when
    // mark_dirty_upward bumps the per-binding gen.
    // Each entry corresponds to one invalidation that
    // can be traced back via
    // (compile:mutation-log-invalidation-trace
    // mutation-id).
    std::atomic<std::uint64_t> invalidation_trace_records_total{0};
    // Issue #386: deep Occurrence Typing narrowing
    // observability. 3 lifetime-total counters that
    // measure the application paths the engine took
    // across the session:
    //   - narrowing_applied_total: push of refined
    //     type into env succeeded
    //   - narrowing_skipped_total: narrowing analyzed
    //     but rejected (var not bound, etc.)
    //   - narrowing_reanalyzed_total: predicate
    //     re-analyzed (cache miss)
    // The applied/(applied+skipped) ratio measures
    // narrowing effectiveness. The reanalyzed /
    // (memo_hits+misses) ratio measures memo
    // effectiveness.
    std::atomic<std::uint64_t> narrowing_applied_total{0};
    std::atomic<std::uint64_t> narrowing_skipped_total{0};
    std::atomic<std::uint64_t> narrowing_reanalyzed_total{0};
    // Issue #340 / #1781: predicate_memo_ lifetime totals
    // (analyze_predicate_flat hit/miss/eviction). Surfaced by
    // (engine:metrics "compile:occ-cache-stats").
    std::atomic<std::uint64_t> predicate_memo_hits_total{0};
    std::atomic<std::uint64_t> predicate_memo_misses_total{0};
    std::atomic<std::uint64_t> predicate_memo_evictions_total{0};
    // Issue #1872: partial LRU overflow eviction events
    // (subset of predicate_memo_evictions_total that are not
    // epoch wholesale clears). Under high mutation, partial
    // eviction preserves hot cond entries vs pre-#1872 clear.
    std::atomic<std::uint64_t> predicate_memo_partial_evictions_total{0};
    // Issue #1872: derived binding-gen cache hit rate (0–100).
    // Updated at snapshot time from per_binding_gen_hits /
    // (hits + stale). Mirrors incremental_locality_hit_rate.
    std::atomic<std::uint64_t> per_binding_gen_hit_rate{0};
    // Issue #338: and/or precision observability.
    // 2 lifetime counters that measure how often the
    // new TypeRegistry::meet / TypeRegistry::join
    // helpers fired in analyze_predicate_flat (replacing
    // the old "fall back to dynamic on mismatch"
    // conservative behavior).
    //   - and_or_meet_uses_total: meet() was called in
    //     the (and ...) branch (intersection of refined
    //     types for the same variable).
    //   - and_or_join_uses_total: join() was called in
    //     the (or ...) branch (union of refined types
    //     for the same variable).
    // Pre-#338 the engine always fell back to
    // dynamic_type() on type mismatch; these counters
    // were always 0. Post-#338 the helpers are the
    // default path.
    std::atomic<std::uint64_t> and_or_meet_uses_total{0};
    std::atomic<std::uint64_t> and_or_join_uses_total{0};
    // Issue #434: per-node occurrence dirty
    // recovery (lifetime total). Bumped when the
    // engine re-analyzes a narrowing because the
    // If node was dirty (post-mutation re-infer
    // triggered re-analysis). Distinct from
    // narrowing_reanalyzed_total (the broader
    // predicate memo miss counter) and
    // invalidation_trace_records_total (#413).
    // The ratio of dirty_recovery /
    // narrowing_reanalyzed measures how much of
    // the re-analysis work is post-mutation
    // (vs. first-time / epoch-advance).
    std::atomic<std::uint64_t> narrowing_dirty_recovery_total{0};
    // Issue #390: per-node schema cache observability.
    // 2 lifetime counters: schema_cache_lookups_total
    // (total lookups against the schema_cache column
    // in the type-checker cache hit path) and
    // schema_cache_hits_total (lookups that returned
    // a non-zero schema that matched the cached
    // type_id). The hit rate is the share of
    // macro-introduced nodes that the type checker
    // was able to short-circuit without re-inference.
    std::atomic<std::uint64_t> schema_cache_lookups_total{0};
    std::atomic<std::uint64_t> schema_cache_hits_total{0};
    // Issue #409: fine-grained constraint dependency
    // tracking observability. 2 lifetime counters:
    //   - delta_constraints_processed_total: total
    //     constraints re-solved via solve_delta
    //     (post-#409 this is the worklist size
    //     after reverse-map filtering, smaller than
    //     pre-#409)
    //   - delta_constraints_total: total constraints
    //     added via add_delta (the delta scope). The
    //     ratio (processed / total) measures how much
    //     filtering the reverse map achieves; pre-#409
    //     the ratio was always 1.0 (all dirty
    //     constraints re-solved). Post-#409 a high
    //     filter ratio means the reverse map is
    //     pruning effectively.
    std::atomic<std::uint64_t> delta_constraints_processed_total{0};
    std::atomic<std::uint64_t> delta_constraints_total{0};
    // Issue #466: solve_delta cross-delta conflict re-verify.
    //   - delta_conflict_reverify_total: clean-constraint
    //     re-verify scans after touched-root delta solves
    //   - delta_conflict_detected_total: re-verify (or delta
    //     unify) detected a cross-delta CONFLICT
    std::atomic<std::uint64_t> delta_conflict_reverify_total{0};
    std::atomic<std::uint64_t> delta_conflict_detected_total{0};
    // Issue #690: constraint typed-mutation reverify + blame completeness.
    //   - reverify_truncated_total: clean-constraint reverify scans that
    //     hit the dynamic scan cap before checking all candidates
    //   - constraint_blame_chain_complete_total: cross-delta conflicts
    //     where active_mutation_id was set (auditable blame chain)
    std::atomic<std::uint64_t> reverify_truncated_total{0};
    std::atomic<std::uint64_t> constraint_blame_chain_complete_total{0};
    // Issue #1529: full blame-chain dump on cross-delta CONFLICT.
    //   - constraint_blame_chain_length_total: sum of frames across dumps
    //   - cross_delta_blame_incomplete_total: conflict with no mutation_id
    //     or without a dumpable provenance frame
    //   - constraint_blame_chain_rich_complete_total: dumps that include
    //     mutation_id + predicate_cond_node + affected NodeId (AC triple)
    std::atomic<std::uint64_t> constraint_blame_chain_length_total{0};
    std::atomic<std::uint64_t> cross_delta_blame_incomplete_total{0};
    std::atomic<std::uint64_t> constraint_blame_chain_rich_complete_total{0};
    // Issue #1873: derived completeness rate (0–100) =
    // rich_complete / (rich_complete + incomplete) * 100.
    // Updated on each blame dump so AI self-repair can watch the trend.
    std::atomic<std::uint64_t> blame_chain_completeness_rate{0};
    // Issue #1873: add_delta constraints still missing provenance after
    // active-context stamp (warning / degrade path — keep partial chain).
    std::atomic<std::uint64_t> blame_provenance_missing_warning_total{0};
    // Issue #1873: reverify hit scan cap and appended a partial blame
    // frame trail (conflict or success-under-truncation).
    std::atomic<std::uint64_t> reverify_truncation_partial_blame_total{0};
    // Issue #1877: MacroIntroduced hygiene frames on blame chains
    // (truncation auto-pull + explicit append_hygiene_blame_frame).
    std::atomic<std::uint64_t> blame_hygiene_frames_total{0};
    // Issue #1877: per-CompilerMetrics mirror of hygiene→provenance stamps.
    std::atomic<std::uint64_t> macro_hygiene_provenance_hits_total{0};
    // Issue #1877: last hygiene stamp for truncation blame auto-pull
    // (non-atomic last-writer; process-wide provenance stamp is primary,
    // this is the metrics_-pointer path that is always TU-shared).
    std::uint32_t last_hygiene_blame_node = 0;
    std::uint64_t last_hygiene_blame_mutation = 0;
    // Issue #745: Occurrence-priority reverify in solve_delta.
    std::atomic<std::uint64_t> constraint_reverify_narrow_hits_total{0};
    std::atomic<std::uint64_t> constraint_reverify_timeout_prevented_total{0};
    std::atomic<std::uint64_t> constraint_stale_blame_invalidation_total{0};
    // Issue #628: solve_delta safety observability.
    //   - solve_delta_full_solve_fallback_total: infer_flat
    //     used full solve() instead of solve_delta in an
    //     incremental delta-record context
    std::atomic<std::uint64_t> solve_delta_full_solve_fallback_total{0};
    // Issue #341: match + Occurrence Typing
    // integration observability. 2 lifetime counters:
    //   - match_subject_narrowed_total: count of
    //     __match_tmp lets whose subject type was
    //     refined by a prior narrowing in the env
    //     (e.g. (if (type? x "Foo") (let ((__match_tmp
    //     x)) (match x ...)) ...)).
    //   - match_subject_total: count of all
    //     __match_tmp lets processed by the type
    //     checker. The ratio narrowed / total
    //     measures how often narrowing actually
    //     feeds into match exhaustiveness.
    std::atomic<std::uint64_t> match_subject_narrowed_total{0};
    std::atomic<std::uint64_t> match_subject_total{0};
    // Issue #612: ADT/match exhaustiveness post-mutation
    // reliability observability. 4 lifetime counters:
    //   - adt_exhaust_rechecks_total: post-mutation
    //     __match_tmp exhaustiveness re-checks
    //   - adt_variant_mutate_impacts_total: DefineType
    //     nodes whose ctor list was re-synced from AST
    //   - adt_stale_exhaust_prevented_total: match sites
    //     whose cached exhaustiveness was cleared before
    //     re-check (stale registry / ctor-list drift)
    //   - adt_occurrence_narrow_in_match_total: re-checks
    //     where the subject carried a narrowed type id
    std::atomic<std::uint64_t> adt_exhaust_rechecks_total{0};
    std::atomic<std::uint64_t> adt_variant_mutate_impacts_total{0};
    std::atomic<std::uint64_t> adt_stale_exhaust_prevented_total{0};
    std::atomic<std::uint64_t> adt_occurrence_narrow_in_match_total{0};
    // Issue #692: ADT/match typed-mutation pattern provenance refresh.
    std::atomic<std::uint64_t> adt_pattern_narrow_refreshes_total{0};
    std::atomic<std::uint64_t> adt_non_exhaustive_caught_total{0};
    std::atomic<std::uint64_t> adt_pattern_provenance_complete_total{0};
    // Issue #1532: type-checker match exhaustiveness observability.
    //   - adt_exhaustiveness_checked_total: every successful
    //     check_match_exhaustiveness / synthesize __match_tmp check
    //   - non_exhaustive_match_diagnostics_total: times a non-exhaustive
    //     match produced a Warning or TypeError diagnostic
    std::atomic<std::uint64_t> adt_exhaustiveness_checked_total{0};
    std::atomic<std::uint64_t> non_exhaustive_match_diagnostics_total{0};
    // Issue #693: Hardware backend SV commercial closed-loop.
    std::atomic<std::uint64_t> hardware_backend_hook_calls_total{0};
    std::atomic<std::uint64_t> commercial_reemits_total{0};
    std::atomic<std::uint64_t> feedback_mutate_hits_total{0};
    std::atomic<std::uint64_t> ppa_savings_total{0};
    std::atomic<std::uint64_t> verification_loop_success_total{0};
    // Issue #698: Hardware backend commercial interop + emit validation.
    std::atomic<std::uint64_t> sv_emit_parse_success_total{0};
    std::atomic<std::uint64_t> sv_emit_parse_fail_total{0};
    std::atomic<std::uint64_t> commercial_simulator_runs_total{0};
    std::atomic<std::uint64_t> sv_diff_emits_total{0};
    // Issue #694: SVA structured AST mutate observability.
    std::atomic<std::uint64_t> sva_structured_mutate_hits_total{0};
    // Issue #695: EDA-SV verification closed-loop stress harness.
    std::atomic<std::uint64_t> eda_sv_evolution_cycles_total{0};
    std::atomic<std::uint64_t> eda_sv_verification_convergence_total{0};
    std::atomic<std::uint64_t> eda_sv_feedback_mutate_success_total{0};
    std::atomic<std::uint64_t> eda_sv_stable_ref_invalidation_total{0};
    // Issue #1901 (refine #1822): EDA self-evolution StableNodeRef
    // auto-refresh telemetry. stable_ref_auto_refresh_in_eda_total
    // counts every refresh_if_stale / re-make_ref call from the
    // EDA self-evolution primitive paths (eda:demo-sv-self-evolution
    // + similar). eda_self_evolution_stale_ref_prevented counts
    // the iterations where the refresh prevented a stale-ref
    // access (forward-compat observability for the UAF window
    // that #1822 / #1901 closed).
    std::atomic<std::uint64_t> stable_ref_auto_refresh_in_eda_total{0};
    std::atomic<std::uint64_t> eda_self_evolution_stale_ref_prevented{0};
    // Issue #1902 (refine #1818 / #1821): EDA Guard exception
    // telemetry. eda_feedback_exception_total counts every
    // catch in eda:run-verification-feedback +
    // eda:run-commercial-simulator-stub (plus future EDA
    // primitives following the same try/catch + guard_ok=false
    // contract). eda_feedback_rolled_back_total counts how many
    // of those exceptions flipped guard_ok to trigger the
    // Guard dtor restore_panic_checkpoint path.
    // guard_exception_rollback_success counts the restore path
    // itself firing (sanity check: rollback_success should
    // track exception_total modulo dtor silent no-op cases).
    std::atomic<std::uint64_t> eda_guard_exception_handled_total{0};
    std::atomic<std::uint64_t> eda_guard_uncaught_exception_total{0};
    std::atomic<std::uint64_t> eda_primitive_entered_without_guard_total{0};
    std::atomic<std::uint64_t> eda_sv_commercial_stub_latency_us_total{0};
    std::atomic<std::uint64_t> eda_sv_corruption_detected_total{0};
    // Issue #697: Declarative primitives extension kit observability.
    std::atomic<std::uint64_t> primitive_skeleton_generations_total{0};
    std::atomic<std::uint64_t> primitive_eda_meta_backfill_total{0};
    // Issue #1416: counts how many EDSL escape-hatch primitives were
    // tier-assigned kPrimSecPrivileged by backfill_capability_tiers().
    // Bumps by 7 (the 7 Part 4 #1396 escape hatches) at Evaluator
    // construction. Visible via (compile:primitive-meta-stats) or
    // direct CompilerMetrics field access.
    std::atomic<std::uint64_t> primitive_capability_tier_backfill_total{0};
    // Issue #617: AI-Native primitive introspection query counters.
    // Each new query primitive bumps its own counter so
    // (query:primitives-meta-catalog) can surface a hit-rate
    // signal per Agent discovery entry point.
    std::atomic<std::uint64_t> primitives_by_category_query_total{0};
    std::atomic<std::uint64_t> schema_of_primitive_query_total{0};
    std::atomic<std::uint64_t> primitives_meta_catalog_query_total{0};
    // Issue #499: EDA foundation primitives module observability.
    std::atomic<std::uint64_t> eda_foundation_parse_total{0};
    std::atomic<std::uint64_t> eda_foundation_query_total{0};
    std::atomic<std::uint64_t> eda_foundation_mutate_total{0};
    std::atomic<std::uint64_t> eda_foundation_waveform_total{0};
    std::atomic<std::uint64_t> eda_foundation_feedback_total{0};
    // Issue #616: EDA hardware-co-design primitives (load-sv /
    // parse-verification-result) observability. Counters are
    // separate from the foundation ones above so that
    // (query:eda-foundation-stats) shape from #499 stays
    // unchanged and (query:eda-hw-stats) is the dedicated
    // dashboard for the new file-I/O / verification-result
    // primitives.
    std::atomic<std::uint64_t> eda_load_sv_total{0};
    std::atomic<std::uint64_t> eda_load_sv_failure_total{0};
    std::atomic<std::uint64_t> eda_parse_verification_result_total{0};
    std::atomic<std::uint64_t> eda_parse_verification_failure_total{0};
    // Issue #841: EDA production infrastructure observability (refines #499/#616;
    // non-duplicative with query:eda-foundation-stats and query:eda-hw-stats).
    //   - eda_infra_parse_success_total: successful SV/SVA parse paths
    //     (eda:parse-netlist / eda:load-sv).
    //   - eda_infra_structured_mutate_total: provenance-safe structured
    //     SVA/RTL mutate under Guard (eda:mutate-add-instance and kin).
    //   - eda_infra_feedback_ingest_total: structured verification feedback
    //     ingest (eda:parse-verification-result / eda:ingest-result).
    //   - eda_infra_cosim_invoke_total: co-simulation bridge invocations
    //     (eda:invoke-simulator + result ingest).
    std::atomic<std::uint64_t> eda_infra_parse_success_total{0};
    std::atomic<std::uint64_t> eda_infra_structured_mutate_total{0};
    std::atomic<std::uint64_t> eda_infra_feedback_ingest_total{0};
    std::atomic<std::uint64_t> eda_infra_cosim_invoke_total{0};
    // Issue #801: SV commercial emit fidelity observability (refines #772/#748/#725;
    // non-duplicative with query:sv-verification-structure-stats #748).
    //   - sv_commercial_emit_parse_success_total: validate_sv_emit roundtrip pass.
    //   - sv_commercial_emit_roundtrip_mismatch_prevented_total: local validator
    //     caught emit drift before commercial tool ingest.
    //   - sv_commercial_emit_dirty_reemit_total: dirty-triggered incremental re-emit.
    //   - sv_commercial_emit_tool_compatible_total: emit accepted with commercial stub.
    std::atomic<std::uint64_t> sv_commercial_emit_parse_success_total{0};
    std::atomic<std::uint64_t> sv_commercial_emit_roundtrip_mismatch_prevented_total{0};
    std::atomic<std::uint64_t> sv_commercial_emit_dirty_reemit_total{0};
    std::atomic<std::uint64_t> sv_commercial_emit_tool_compatible_total{0};
    // Issue #802: SV verification feedback-driven self-evolution observability
    // (refines #774/#726/#748; non-duplicative with query:closed-loop-reliability-stats).
    //   - sv_self_evo_feedback_parse_total: verify:parse-coverage/assert/formal-cex hits.
    //   - sv_self_evo_structured_mutate_total: mutate:from-verification-feedback successes.
    //   - sv_self_evo_closed_loop_rounds_total: closed-loop orchestration rounds.
    //   - sv_self_evo_convergence_hits_total: successful feedback→mutate→re-verify rounds.
    std::atomic<std::uint64_t> sv_self_evo_feedback_parse_total{0};
    std::atomic<std::uint64_t> sv_self_evo_structured_mutate_total{0};
    std::atomic<std::uint64_t> sv_self_evo_closed_loop_rounds_total{0};
    std::atomic<std::uint64_t> sv_self_evo_convergence_hits_total{0};
    // Issue #803: SEVA Long-Running Concurrent Verification Evolution
    // SLO observability counters (P0 EDA-SV-verification-production
    // long-running concurrent multi-agent harness foundation;
    // consolidates/non-duplicates #794 + #755 + #773 + #774 + #802).
    // #803 is the FIRST observability surface that tracks the
    // *long-running concurrent SEVA production harness signals* —
    // the per-decision-point counters the Agent consumes to monitor
    // production readiness for commercial multi-agent deployment:
    //   - seva_concurrent_ref_drift_prevented_total: ref_drift
    //     attempts that were caught + prevented during long-running
    //     concurrent SEVA loop. Distinct from #762
    //     workspace_closedloop_stale_ref_prevented_eda_loops_total
    //     (which tracks workspace-level staleness in EDA verification
    //     loops); #803 tracks harness-loop-level ref drift.
    //   - seva_concurrent_steal_during_verification_mutate_total:
    //     count of fiber steal events that occurred during a
    //     verification mutate inside the long-running harness —
    //     a high-fidelity load metric for the SEVA test surface.
    //   - seva_concurrent_dirty_propagation_hits_total: count of
    //     dirty propagation consistency checks that passed during
    //     harness round (a no-fail signal — the inverse would be
    //     a dirty inconsistency violation).
    // P0 ships the counters + the (query:seva-longrunning-
    // concurrent-slo, schema 803) primitive so the Agent has a
    // dashboard today; values are 0 until the Phase 2+
    // harness + SLO CI gate + self-heal hooks land. Consumed by
    // tests/test_seva_longrunning_concurrent_verification_
    // evolution.cpp (Phase 2+).
    std::atomic<std::uint64_t> seva_concurrent_ref_drift_prevented_total{0};
    std::atomic<std::uint64_t> seva_concurrent_steal_during_verification_mutate_total{0};
    std::atomic<std::uint64_t> seva_concurrent_dirty_propagation_hits_total{0};
    // Issue #766: IR-SoA migration observability + DirtyAware
    // incremental pipeline counters (P0 high-perf C++26 DOD/SoA
    // foundation; refines #167/#463/#741; non-duplicative with
    // #729 query:soa-hotpath-stats and #765 query:incremental-
    // quote-lambda-linear-stats). These are public so future
    // ir_soa.ixx IRFunctionSoA + IRModuleV2 add_instruction /
    // mark_block_dirty / mark_all_blocks_dirty + pass_manager.
    // ixx DirtyAwarePass + run_incremental_dirty_pipeline +
    // lowering_impl.cpp set_soa_emit_path + apply_soa_view +
    // evaluator_impl.cpp soa_interp_dispatch + aura_jit.cpp
    // emit_soa_function + tests/test_highperf_ir_soa_migration_
    // dirty_incremental.cpp harness (large define + quote/lambda
    // + heavy mutate:rebind on body → impact_scope + DirtyAware
    // partial re-lower + JIT recompile → assert SoA path used,
    // clean blocks skipped, metrics accurate, no regression vs
    // AoS, TSan/ASan clean) can call them at each decision point
    // (IR SoA column counts / DirtyAware short-circuit hits /
    // pmr column utilization / JIT SoA codegen time).
    //
    // Non-duplicative with #729 (query:soa-hotpath-stats), #752
    // (query:list-soa-hotpath-stats), #765 (query:incremental-
    // quote-lambda-linear-stats) which cover SoA list/cdr-walk /
    // SoA hot path telemetry + incremental quote/lambda/closure
    // compile safety. #766 is the FIRST observability surface
    // that tracks the *production migration of IRModuleV2 +
    // DirtyAware incremental pipeline* — soa_instructions_emitted
    // (IRModuleV2.add_instruction cumulative count),
    // dirty_block_skips (DirtyAware short-circuit hits that
    // skipped a clean block), clean_block_hit_rate (0-100
    // percent of blocks that were clean when re-lower entered),
    // pmr_column_utilization (0-100 percent of SoA column
    // capacity currently in use), jit_soa_codegen_time_ns
    // (cumulative SoA codegen time in aura_jit.cpp) — as
    // separate per-decision-point counters the Agent consumes
    // to monitor the SoA migration + DirtyAware short-circuit
    // production-readiness under incremental AI mutation flows.
    //
    //   - ir_soa_instructions_emitted_total: # of instructions
    //                                        emitted to SoA
    //                                        columns via
    //                                        IRModuleV2::
    //                                        add_instruction
    //                                        (proxy for "how
    //                                        much of the hot
    //                                        path is going
    //                                        through the SoA
    //                                        emit" — high
    //                                        value = migration
    //                                        in progress).
    //   - ir_soa_dirty_block_skips_total: # of DirtyAware
    //                                      short-circuit
    //                                      block skips
    //                                      (re-lower entered
    //                                      a function but
    //                                      is_block_dirty
    //                                      ==0 → skipped
    //                                      cached IR; proxy
    //                                      for "how much
    //                                      cache-locality
    //                                      we recovered
    //                                      under incremental
    //                                      re-lower" — high
    //                                      value = the whole
    //                                      point of the
    //                                      SoA+DirtyAware
    //                                      story).
    //   - ir_soa_clean_block_hit_rate_pct: 0-100 percent of
    //                                       blocks that
    //                                       re-lower saw as
    //                                       clean (composite
    //                                       of dirty_block_
    //                                       skips / total
    //                                       blocks visited;
    //                                       recorded as pct
    //                                       × 100 so 10000
    //                                       = 100.00% — fixed-
    //                                       point percent
    //                                       prevents floating-
    //                                       point observability
    //                                       drift under
    //                                       parallel update).
    //   - ir_soa_pmr_column_utilization_pct: 0-100 percent of
    //                                        SoA column
    //                                        capacity currently
    //                                        in use (opcodes_
    //                                        size / opcodes_
    //                                        capacity, sampled
    //                                        at primitive-call
    //                                        time; recorded as
    //                                        pct × 100 so 5000
    //                                        = 50.00% — fixed-
    //                                        point avoids
    //                                        float drift).
    //   - ir_soa_jit_codegen_time_ns_total: cumulative SoA
    //                                       codegen time in
    //                                       aura_jit.cpp
    //                                       emit_soa_function
    //                                       (ns; proxy for
    //                                       "how much JIT
    //                                       time we spent on
    //                                       SoA codegen" —
    //                                       pair with mutation
    //                                       rate to see JIT
    //                                       cost under AI
    //                                       self-mod).
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual ir_soa.ixx IRFunctionSoA + IRModuleV2 add_
    // instruction / mark_block_dirty / mark_all_blocks_dirty +
    // pass_manager.ixx DirtyAwarePass + run_incremental_dirty_
    // pipeline + lowering_impl.cpp set_soa_emit_path + apply_
    // soa_view + evaluator_impl.cpp soa_interp_dispatch +
    // aura_jit.cpp emit_soa_function + tests/test_highperf_ir_
    // soa_migration_dirty_incremental.cpp harness (large define
    // + quote/lambda + heavy mutate:rebind on body → impact_scope
    // + DirtyAware partial re-lower + JIT recompile → assert SoA
    // path used, clean blocks skipped, metrics accurate, no
    // regression vs AoS, TSan/ASan clean) + SEVA SoA migration
    // incremental demo + sync with ShapeProfiler versioning +
    // Pass Pipeline JITFriendly epoch hints + Arena compact
    // shape_inval_on_compact hook + pmr/arena hosting of
    // remaining SoA columns + CI gate + docs are all follow-up
    // work (each is a dedicated session in ir_soa.ixx +
    // pass_manager.ixx + lowering_impl.cpp + evaluator_impl.cpp +
    // aura_jit.cpp + new test + SEVA demo + docs).
    std::atomic<std::uint64_t> ir_soa_instructions_emitted_total{0};
    std::atomic<std::uint64_t> ir_soa_dirty_block_skips_total{0};
    std::atomic<std::uint64_t> ir_soa_clean_block_hit_rate_pct{0};
    std::atomic<std::uint64_t> ir_soa_pmr_column_utilization_pct{0};
    std::atomic<std::uint64_t> ir_soa_jit_codegen_time_ns_total{0};
    // Issue #767: Arena Auto-Compact Policy + Live Defrag + Fiber/GC
    // Safepoint Yield observability (P0 high-perf C++26 Arena
    // foundation; completes #300 P1 + #685 + #731; non-duplicative
    // with #685 query:arena-auto-compact-stats and #642 query:
    // arena-auto-compaction-stats). These are public so future
    // arena.ixx allocate_raw auto-compact policy + compact/defrag
    // paths + gc_hooks.h + fiber integration + on_compact_hook_
    // Shape/Dirty integration can call them at each decision point
    // (auto compact trigger / live defrag savings / fiber yield
    // during compact / shape_inval on compact / defrag blocked fibers).
    //
    // Non-duplicative with #685 (query:arena-auto-compact-stats) and
    // #642 (query:arena-auto-compaction-stats) which cover the
    // existing auto-compact trigger / live-move-yield / guard
    // request_defrag axes. #767 is the FIRST observability surface
    // that tracks the *production auto-compact policy + live defrag
    // + fiber yield during compact + defrag blocked fibers* —
    // 2 truly new counters beyond what #685/#642 cover — as
    // separate per-decision-point counters the Agent consumes to
    // decide whether to tune the threshold, force defrag, or trust
    // the auto-compact policy under sustained AI mutation load.
    //
    //   - arena_auto_compact_fiber_yield_during_compact_total:
    //                                         # of actual fiber
    //                                         yields during
    //                                         compact/defrag
    //                                         (proxy for "how
    //                                         many fibers gave
    //                                         up the scheduler
    //                                         to let a long
    //                                         compact/defrag
    //                                         finish without
    //                                         blocking the
    //                                         worker" — high
    //                                         count = the
    //                                         compaction path
    //                                         is fiber-friendly;
    //                                         0 = everything
    //                                         was stop-the-
    //                                         world, latency
    //                                         risk under fiber
    //                                         load). Pairs
    //                                         with the existing
    //                                         #685 yield_checks
    //                                         (which is
    //                                         observability-only;
    //                                         this counter is
    //                                         the *actual* yield
    //                                         event).
    //   - arena_auto_compact_defrag_blocked_fibers_total:
    //                                         # of fibers blocked
    //                                         waiting for defrag
    //                                         to complete
    //                                         (proxy for "how
    //                                         many fibers hit a
    //                                         defrag safepoint
    //                                         and waited" — high
    //                                         count = the
    //                                         defrag path is
    //                                         too slow or
    //                                         contention is
    //                                         real; investigate
    //                                         fiber yield
    //                                         integration or
    //                                         batch defrag).
    //                                         No equivalent in
    //                                         #685 or #642 —
    //                                         #767 introduces
    //                                         this metric to
    //                                         surface the
    //                                         hidden defrag-
    //                                         fiber interaction
    //                                         cost.
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual arena.ixx allocate_raw auto-compact policy (if
    // (fragmentation_ratio() > kAutoCompactThreshold ||
    // auto_alloc_trigger_count > N) request_defrag() or compact();
    // integrate with safepoint check) + live defrag pass (Phase 3
    // of #300: scan live objects via dtors_ or registered roots,
    // relocate to packed region, update pointers (StableRef /
    // children views + GC roots)) + gc_hooks.h + fiber
    // integration (in compact()/defrag(), on yield-check actually
    // yield via fiber scheduler or WorkerContext; coordinate with
    // GC safepoint for stop-the-world or concurrent defrag) +
    // on_compact_hook_ Shape/Dirty integration (invoke hook to
    // invalidate ShapeProfiler versions + cascade dirty_ in
    // affected IR/FlatAST blocks; wire to mutation_epoch_ bump) +
    // tests/test_highperf_arena_auto_compact_defrag_fiber_yield.cpp
    // harness (sustained mutate:rebind loop + fiber steal + GC
    // pressure → assert auto compact triggers, frag reduced, live
    // defrag succeeds, yields happen, no UAF/leak, metrics, TSan
    // clean) + SEVA long-running demo + CI gate + docs are all
    // follow-up work (each is a dedicated session in arena.ixx +
    // gc_hooks.h + service.ixx + ShapeProfiler + new test + SEVA
    // demo + docs).
    std::atomic<std::uint64_t> arena_auto_compact_fiber_yield_during_compact_total{0};
    std::atomic<std::uint64_t> arena_auto_compact_defrag_blocked_fibers_total{0};
    // Issue #768: Shape + Pass + Contracts hot-path observability
    // (P0 high-perf C++26 Contracts/Concepts adoption foundation;
    // builds on #507 hot-path Contracts; non-duplicative with
    // #570 query:shape-stability-stats, #492 query:shape-profiler-
    // stats, #494 query:pass-pipeline-stats, #571 query:evalvalue-
    // v2-dispatch-stats, #744 shape_jit_pass_closedloop_stats).
    // These are public so future shape_profiler.cpp inline_shape_of
    // + history push + dominant compute + record_shape stability
    // transition + pass_manager.ixx JITFriendlyPass + DirtyAwarePass
    // + arena.ixx shape_inval_on_compact hook + ir_soa.ixx shape_ids_
    // column can call them at each decision point (hot-path contract
    // checks / shape stability transitions / JIT epoch sync / targeted
    // deopt skips / Concept violations caught).
    //
    // Non-duplicative with the existing #570/#492/#494/#571/#744
    // observability surfaces which cover ShapeProfiler stability +
    // deopt + dirty + JIT recompile closed-loop (stable_shape /
    // stability_churn_deopts / dirty_from_shape / incremental_
    // recompile_hits / speculative_win_lost). #768 is the FIRST
    // observability surface that tracks the *production hot-path
    // Contracts coverage + ShapeProfiler epoch sync with JIT/Pass
    // Pipeline + stronger Concept constraints for Dirty/JITFriendly
    // composition* — contract_checks_hotpath (zero-overhead debug
    // catches in SoA/dirty/shape dispatch), shape_stability_
    // transitions (proxy for "how often the dominant shape
    // flipped"), jit_epoch_sync_hits (ShapeProfiler version
    // bumped in sync with mutation_epoch_ + JIT epoch hint),
    // deopt_targeted_skips (DirtyAware or impact_scope targeted
    // invalidation saved a full recompile), concept_violations_
    // caught (static_assert in pipeline templates fired) — as
    // separate per-decision-point counters the Agent consumes
    // to monitor the speculative opt + debug layer production-
    // readiness under AI mutation churn.
    //
    //   - shape_pass_contract_checks_hotpath_total: # of
    //                                            contract_assert /
    //                                            pre / post
    //                                            checks that
    //                                            fired in hot
    //                                            paths
    //                                            (inline_shape_of
    //                                            / history push /
    //                                            dominant compute
    //                                            / record_shape /
    //                                            dirty propagate /
    //                                            shape dispatch).
    //                                            In release builds
    //                                            with Contracts
    //                                            disabled, this
    //                                            stays 0; in debug
    //                                            builds it shows
    //                                            coverage of the
    //                                            zero-overhead
    //                                            safety net.
    //                                            High rate on
    //                                            long-running
    //                                            Agent sessions =
    //                                            the hot path is
    //                                            well-guarded; 0
    //                                            in debug = hot
    //                                            paths slipped
    //                                            through without
    //                                            contracts.
    //   - shape_stability_transitions_total: # of dominant-shape
    //                                        transitions recorded
    //                                        by ShapeProfiler
    //                                        (proxy for "how
    //                                        often the dominant
    //                                        shape flipped" —
    //                                        high rate = the
    //                                        workload is
    //                                        polymorphic; low
    //                                        rate = stable
    //                                        shape dominates
    //                                        and JIT can
    //                                        specialize
    //                                        aggressively).
    //   - jit_epoch_sync_hits_total: # of ShapeProfiler version
    //                                bumps synced with
    //                                mutation_epoch_ + JIT
    //                                epoch hint (proxy for
    //                                "how often the
    //                                speculative layer
    //                                correctly tracks the
    //                                mutation epoch" —
    //                                high value =
    //                                cross-layer epoch
    //                                invariant holds;
    //                                drift = shape
    //                                stability vs JIT
    //                                mismatch).
    //   - deopt_targeted_skips_total: # of deopt events where
    //                                  DirtyAware or
    //                                  impact_scope targeted
    //                                  invalidation saved
    //                                  a full recompile
    //                                  (proxy for "how
    //                                  often the dirty
    //                                  short-circuit
    //                                  prevented a global
    //                                  deopt" — high
    //                                  value = the SoA +
    //                                  DirtyAware story
    //                                  pays off in
    //                                  practice).
    //   - concept_violations_caught_total: # of static_assert
    //                                      in pipeline
    //                                      templates that
    //                                      fired (JITFriendlyPass
    //                                      / DirtyAwarePass /
    //                                      SoAView /
    //                                      ShapeStablePass
    //                                      Concept
    //                                      violations; proxy
    //                                      for "how many
    //                                      compile-time
    //                                      guardrails caught
    //                                      a misconfigured
    //                                      Pass composition
    //                                      at build time" —
    //                                      high value = the
    //                                      Concepts layer is
    //                                      doing its job
    //                                      catching
    //                                      misconfigurations
    //                                      early).
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual shape_profiler.cpp inline_shape_of + history push
    // + dominant compute + record_shape stability transition +
    // wire version bump to mutation_epoch_ + JIT epoch hint +
    // on_deopt consult DirtyAware or impact_scope for targeted
    // invalidation + pass_manager.ixx JITFriendlyPass +
    // DirtyAwarePass + SoAView / ShapeStablePass Concept
    // (requires const run on SoA view + shape_id consult) +
    // integrate ShapeProfiler dominant_shape into ComputeKind
    // or new ShapePropagationPass + short-circuit on stable
    // shape match + arena.ixx shape_inval_on_compact auto bump
    // ShapeProfiler versions for affected blocks + mark dirty +
    // tests/test_highperf_shape_pass_contracts_jit_epoch.cpp
    // harness (define with shape-varying calls + heavy mutate +
    // JIT recompile under debug/release → assert Contracts
    // catch in debug only, shape stability drives targeted
    // deopt/recompile, epoch sync correct, Dirty/JIT concepts
    // enforced, metrics, TSan clean) + SEVA speculative
    // scenarios + CI gate + docs are all follow-up work (each
    // is a dedicated session in shape_profiler.cpp + value.ixx +
    // pass_manager.ixx + arena.ixx + ir_soa.ixx + new test +
    // SEVA demo + docs).
    std::atomic<std::uint64_t> shape_pass_contract_checks_hotpath_total{0};
    std::atomic<std::uint64_t> shape_stability_transitions_total{0};
    std::atomic<std::uint64_t> jit_epoch_sync_hits_total{0};
    std::atomic<std::uint64_t> deopt_targeted_skips_total{0};
    std::atomic<std::uint64_t> concept_violations_caught_total{0};
    // Issue #795: deep hot-path Contracts + stronger
    // SoAView/ShapeStablePass Concepts + ShapeProfiler
    // JIT Epoch Sync + Dirty Propagation observability
    // (Non-duplicative refinement of #768/#507/#766/
    // #767/#741). 4 NEW atomics for the
    // (query:shape-pass-hotpath-contracts-stats,
    // schema 795) primitive:
    //   - soa_view_violations_caught_total: # of
    //     SoAView concept static_assert violations
    //     caught at compile time / runtime (Phase 2+
    //     to wire from pass_manager.ixx +
    //     lowering/JIT integration per body "Define
    //     SoAView concept (requires const view +
    //     shape_id consult) ... static_assert in
    //     run_incremental_dirty_pipeline")
    //   - shape_stable_pass_violations_total: # of
    //     ShapeStablePass concept static_assert
    //     violations caught (Phase 2+ to wire from
    //     pass_manager.ixx + dominant_shape /
    //     ShapePropagationPass integration per body
    //     "Define ... ShapeStablePass (requires
    //     stable_shape consult + DirtyAware)")
    //   - targeted_deopt_via_impact_scope_total: # of
    //     targeted deopts via #741 impact_scope
    //     instead of global invalidation (Phase 2+ to
    //     wire from shape_profiler.cpp deopt hook
    //     per body "consult DirtyAware or #741
    //     impact_scope for targeted invalidation
    //     instead of global")
    //   - on_compact_hook_invocations_total: # of
    //     Arena compact on_compact_hook_ invocations
    //     that triggered shape_inval + dirty cascade
    //     (Phase 2+ to wire from arena.ixx + ir_soa.ixx
    //     per body "on_compact_hook_ invoke with
    //     shape_inval + dirty cascade")
    std::atomic<std::uint64_t> soa_view_violations_caught_total{0};
    std::atomic<std::uint64_t> shape_stable_pass_violations_total{0};
    std::atomic<std::uint64_t> targeted_deopt_via_impact_scope_total{0};
    std::atomic<std::uint64_t> on_compact_hook_invocations_total{0};
    // Issue #772: SV Verification closed-loop SLO observability
    // (P0 EDA production standard foundation; consolidates/refines
    // #693/#724/#725/#726/#748; non-duplicative with #748 query:
    // sv-verification-structure-stats, #801 query:sv-commercial-
    // emit-fidelity-stats, #802 query:sv-verification-self-
    // evolution-stats). These are public so future hardware_backend
    // emit_sv_verification_structured + sv_ir_impl.cpp dirty-
    // triggered incremental re-emit queue + eda:validate-sv-emit
    // roundtrip stub can call them at each decision point (emit
    // parse success/failure / re-emit latency / SLO breach).
    //
    // Non-duplicative with #748/#801/#802 which track structure
    // mutate + dirty re-emit + emit fidelity pass/fail + self-evolution
    // closed-loop counters. #772 is the FIRST observability surface
    // that tracks the *production SLO status of the SV verification
    // closed-loop* — fidelity rate (>99% threshold) + re-emit
    // latency max (the bound threshold) + breach counter — as
    // separate per-decision-point counters + a computed slo-status
    // field the Agent can read to decide whether the closed-loop is
    // production-ready for commercial VCS/Questa/JasperGold emit
    // acceptance.
    //
    //   - sv_slo_emit_parse_success_total: # of successful emit
    //                                      passes through the
    //                                      roundtrip validate
    //                                      stub (the fidelity
    //                                      numerator).
    //   - sv_slo_emit_parse_failure_total: # of failed emit passes
    //                                      (the fidelity
    //                                      denominator — breach
    //                                      trigger if rate
    //                                      < 99%).
    //   - sv_slo_reemit_latency_max_us:   max re-emit latency in
    //                                      us (high-water mark;
    //                                      breach trigger if
    //                                      > X threshold).
    //   - sv_slo_reemit_hits_total:       # of incremental
    //                                      re-emits triggered by
    //                                      verification_dirty or
    //                                      ppa_dirty success
    //                                      (proxy for "how often
    //                                      the dirty-triggered
    //                                      incremental path
    //                                      fired instead of
    //                                      full re-emit").
    //   - sv_slo_breach_total:            # of SLO breaches
    //                                      observed (cumulative;
    //                                      0 = production-ready,
    //                                      > 0 = investigate
    //                                      fidelity or latency
    //                                      root cause).
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual hardware_backend.ixx emit_sv_verification_
    // structured for VCS/Questa/JasperGold compat + sv_ir_impl.cpp
    // dirty-triggered incremental re-emit queue + eda:validate-
    // sv-emit roundtrip stub (slang/Verilator stub) + tests/
    // test_sv_verification_edsl_emit_fidelity_closedloop.cpp
    // harness + extend SEVA with full class/constraint/covergroup/
    // SVA self-evo scenarios (coverage hole → mutate constraint/
    // coverpoint; assert fail → weaken property + re-emit) + large
    // multi-clock/interface fixture + Prometheus exposure with
    // (query:sv-closedloop-slo) thresholds + SEVA tutorial update +
    // CI gate + docs are all follow-up work (each is a dedicated
    // session in hardware_backend.ixx + sv_ir_impl.cpp + eda_
    // primitives_eda.cpp + new test + SEVA demo + docs).
    std::atomic<std::uint64_t> sv_slo_emit_parse_success_total{0};
    std::atomic<std::uint64_t> sv_slo_emit_parse_failure_total{0};
    std::atomic<std::uint64_t> sv_slo_reemit_latency_max_us{0};
    std::atomic<std::uint64_t> sv_slo_reemit_hits_total{0};
    std::atomic<std::uint64_t> sv_slo_breach_total{0};
    // Issue #773: Workspace closed-loop fiber/multi-agent EDA
    // verification orchestration observability (P0 high-perf
    // C++26 concurrent Workspace foundation; refines/consolidates
    // #762/#749/#755/#760; non-duplicative with #762 query:
    // workspace-closedloop-orchestration-stats). #773 is the
    // FIRST observability surface that tracks the *production
    // Workspace closed-loop orchestration under fiber + multi-
    // Agent EDA verification loops* — concurrent_query_mutate_
    // success_pct + cross_cow_ref_validity_pct (derived from
    // #762 atomics) + yield_points_hit (reused) +
    // shared_mutex_contention_ns (NEW atomic, time-based
    // metric vs #762's count-based) + multi_agent_edit_fidelity
    // (NEW atomic, fixed-point pct × 100) + stale_ref_prevented
    // (NEW atomic, count of stale refs prevented in EDA loops).
    //
    // The 4 reused atomics from #762 are referenced (not duplicated)
    // so the existing #762 schema sentinels stay intact. The 3 NEW
    // atomics provide the additional dimensions:
    //
    //   - workspace_closedloop_shared_mutex_contention_ns_total:
    //     cumulative nanoseconds spent in workspace_mtx_ contention
    //     on shared/unique locks under heavy AI load (replaces #762's
    //     count-based metric with a time-based metric — more useful
    //     for SLO latency analysis).
    //   - workspace_closedloop_multi_agent_edit_fidelity_pct:
    //     0-10000 fixed-point percent (× 100 — 9900 = 99.00%) of
    //     multi-Agent concurrent edits that completed cleanly under
    //     fiber steal (proxy for "how well multi-Agent orchestration
    //     maintains Workspace integrity" — high value = production-
    //     ready multi-Agent deployments).
    //   - workspace_closedloop_stale_ref_prevented_eda_loops_total:
    //     count of cross-COW StableRef accesses that were caught
    //     stale and refreshed/avoided (proxy for "how often the
    //     auto cross-COW propagation saved a stale ref in long-
    //     running SEVA verification loops" — high value = the
    //     auto-propagation safety net is working).
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual ast.ixx pin_for_cow() + cross-boundary is_valid_in
    // + auto-capture pinning shared_ptr on Guard/query paths +
    // Workspace COW/clone/split propagation/snapshot of active
    // StableRef pins + EDSL primitives yield instrumentation
    // (matcher / children iteration / mark_dirty_upward on SV
    // verification nodes) + fiber/Guard steal/resume auto-refresh
    // + tests/test_workspace_closedloop_fiber_multiagent_eda_
    // verification.cpp harness (10+ fibers/agents with parallel
    // query/mutate on shared+COW workspaces + steal/yield/panic
    // during SEVA verification loop → assert auto refresh, dirty
    // consistent, no contention deadlock or stale, metrics
    // accurate, TSan clean) + SEVA long-running demo + Prometheus
    // exposure with SLO thresholds (closedloop_fidelity >99.5%
    // under 10+ fiber concurrent) + CI gate + docs are all
    // follow-up work (each is a dedicated session in ast.ixx +
    // evaluator_workspace_tree + evaluator_primitives_query.cpp +
    // evaluator_primitives_mutate.cpp + evaluator_fiber_mutation.cpp
    // + new test + SEVA demo + docs).
    std::atomic<std::uint64_t> workspace_closedloop_shared_mutex_contention_ns_total{0};
    std::atomic<std::uint64_t> workspace_closedloop_multi_agent_edit_fidelity_pct{0};
    std::atomic<std::uint64_t> workspace_closedloop_stale_ref_prevented_eda_loops_total{0};
    // Issue #818: StableNodeRef full provenance + cross-COW/sub-workspace
    // auto-resolve enforcement (Task1-review follow-up; non-duplicative with
    // #641 provenance-sv-stats, #715 layer-stats, #738 boundary-stats, #749 COW).
    //   - stable_ref_provenance_enforced_total: full provenance validate on
    //     mutate:*/query:* StableRef unpack paths (Guard-scope stamp check).
    //   - stable_ref_cross_cow_refresh_hits_total: cross-COW / WorkspaceTree
    //     resolve_stable_ref or validate_or_refresh success.
    //   - stable_ref_fiber_workspace_mismatch_prevented_total: fiber_id /
    //     workspace_id inconsistency caught before use.
    //   - stable_ref_steal_auto_refresh_total: fiber steal/resume auto-refresh
    //     of Workspace-active StableRefs.
    std::atomic<std::uint64_t> stable_ref_provenance_enforced_total{0};
    std::atomic<std::uint64_t> stable_ref_cross_cow_refresh_hits_total{0};
    std::atomic<std::uint64_t> stable_ref_fiber_workspace_mismatch_prevented_total{0};
    std::atomic<std::uint64_t> stable_ref_steal_auto_refresh_total{0};
    // Issue #1473 / #1497: pinned-ref validate_or_refresh sweeps driven by
    // re_pin_cow_children_from_snapshot (steal/compact) +
    // probe_linear_ownership_at_gc_safepoint (GC) + fiber-steal.
    // #1497 unifies them on restamp_pinned_stable_refs and adds
    // boundary_pinned_refresh_count (actual boundary_pinned restamps).
    std::atomic<std::uint64_t> stable_ref_validations_at_steal{0};
    std::atomic<std::uint64_t> stable_ref_validations_at_gc_safepoint{0};
    std::atomic<std::uint64_t> boundary_pinned_refresh_count{0};
    // Issue #709: registry fast dispatch + capture discipline telemetry.
    std::atomic<std::uint64_t> primitive_fastpath_hits_total{0};
    std::atomic<std::uint64_t> primitive_capture_violations_total{0};
    // Issue #805: Integrated primitives hot-path + registry load SLO
    // (non-duplicative with #776 hotpath-slo composite — this surface
    // tracks registry/list-apply samples under mutation+fiber load).
    //   - hotpath_registry_apply_samples_total: # of timed apply/map/filter
    //     samples recorded by the bench harness or list apply path.
    //   - hotpath_registry_ns_accum_total: cumulative nanoseconds for those
    //     samples (ns_per_apply = accum / samples).
    //   - hotpath_registry_bench_runs_total: full bench-suite invocations.
    //   - hotpath_registry_extension_reg_ns_total: cumulative ns spent in
    //     register_all_primitives / extension kit registration probes.
    //   - hotpath_registry_linear_cost_total: linear cdr-walk cost samples
    //     (pairs with linear_traverse / list chain counters).
    std::atomic<std::uint64_t> hotpath_registry_apply_samples_total{0};
    std::atomic<std::uint64_t> hotpath_registry_ns_accum_total{0};
    std::atomic<std::uint64_t> hotpath_registry_bench_runs_total{0};
    std::atomic<std::uint64_t> hotpath_registry_extension_reg_ns_total{0};
    std::atomic<std::uint64_t> hotpath_registry_linear_cost_total{0};
    // ── Issues #809–#817 Phase 1 observability (error policy, fiber/JIT,
    // steal+arena, Guard, production health, macro IR, edsl-struct, dirty-epoch)
    // #809 formalize exception policy + interop
    std::atomic<std::uint64_t> error_policy_interop_conversions_total{0};
    std::atomic<std::uint64_t> error_policy_contract_as_aura_error_total{0};
    // #810 fiber/scheduler init AuraResult path
    std::atomic<std::uint64_t> fiber_init_aura_result_ok_total{0};
    std::atomic<std::uint64_t> fiber_init_aura_result_err_total{0};
    std::atomic<std::uint64_t> scheduler_init_aura_result_ok_total{0};
    std::atomic<std::uint64_t> scheduler_init_aura_result_err_total{0};
    // #811 JIT exception bridge classification
    std::atomic<std::uint64_t> jit_guest_exception_bridge_total{0};
    std::atomic<std::uint64_t> jit_internal_aura_result_path_total{0};
    // #812 steal + arena + GC safepoint coordination
    std::atomic<std::uint64_t> steal_arena_yield_during_compact_total{0};
    std::atomic<std::uint64_t> steal_outermost_only_enforced_total{0};
    std::atomic<std::uint64_t> steal_linear_probe_on_success_total{0};
    // #813 MutationBoundaryGuard AuraResult migration telemetry
    std::atomic<std::uint64_t> guard_aura_result_path_total{0};
    std::atomic<std::uint64_t> guard_panic_checkpoint_aura_result_total{0};
    // #814 unified runtime production health + self-heal
    std::atomic<std::uint64_t> runtime_self_heal_invocations_total{0};
    std::atomic<std::uint64_t> runtime_health_drift_detected_total{0};
    // #815 macro-introduced IR source_marker provenance
    std::atomic<std::uint64_t> macro_ir_source_marker_stamps_total{0};
    std::atomic<std::uint64_t> macro_provenance_query_total{0};
    // #816 edsl:define-struct + runtime reflect validate
    std::atomic<std::uint64_t> edsl_define_struct_total{0};
    std::atomic<std::uint64_t> edsl_define_struct_validate_pass_total{0};
    std::atomic<std::uint64_t> edsl_define_struct_validate_fail_total{0};
    // #817 dirty-epoch-marker macro awareness
    std::atomic<std::uint64_t> dirty_epoch_macro_introduced_hits_total{0};
    std::atomic<std::uint64_t> dirty_epoch_targeted_relower_total{0};
    std::atomic<std::uint64_t> dirty_epoch_hygiene_drift_prevented_total{0};
    // ── Issues #819–#829 Phase 1 observability (non-dup surfaces) ──
    // #819 pattern hygiene provenance + yield enforcement layer
    std::atomic<std::uint64_t> pattern_hygiene_provenance_predicate_hits_total{0};
    std::atomic<std::uint64_t> pattern_hygiene_index_enforced_hits_total{0};
    std::atomic<std::uint64_t> pattern_hygiene_yield_enforced_total{0};
    std::atomic<std::uint64_t> pattern_hygiene_safe_span_enforced_total{0};
    // #820 mutate atomic-batch e2e observability (refine #790)
    std::atomic<std::uint64_t> mutate_batch_e2e_started_total{0};
    std::atomic<std::uint64_t> mutate_batch_e2e_suppressed_bumps_total{0};
    std::atomic<std::uint64_t> mutate_batch_e2e_hygiene_in_batch_total{0};
    std::atomic<std::uint64_t> mutate_batch_e2e_cross_fiber_steals_total{0};
    std::atomic<std::uint64_t> mutate_batch_e2e_pinned_snapshot_total{0};
    std::atomic<std::uint64_t> mutate_batch_e2e_panic_recoveries_total{0};
    // #821 JIT fiber-local exception
    std::atomic<std::uint64_t> jit_fiber_ex_stack_local_total{0};
    std::atomic<std::uint64_t> jit_fiber_ex_cross_prevented_total{0};
    std::atomic<std::uint64_t> jit_fiber_ex_deopt_interpreter_total{0};
    // #822 L2 specialization maturity
    std::atomic<std::uint64_t> l2_spec_pair_fastpath_total{0};
    std::atomic<std::uint64_t> l2_spec_deopt_version_total{0};
    std::atomic<std::uint64_t> l2_spec_guardshape_narrow_total{0};
    std::atomic<std::uint64_t> l2_spec_linear_probe_total{0};
    // #823 opcode coverage deopt controller
    std::atomic<std::uint64_t> opcode_cov_hits_total{0};
    std::atomic<std::uint64_t> opcode_cov_unhandled_hot_total{0};
    std::atomic<std::uint64_t> opcode_cov_per_fn_deopt_total{0};
    // #824 terminal render production primitives
    std::atomic<std::uint64_t> term_render_clear_total{0};
    std::atomic<std::uint64_t> term_render_draw_batch_total{0};
    std::atomic<std::uint64_t> term_render_present_total{0};
    std::atomic<std::uint64_t> term_render_dirty_region_total{0};
    std::atomic<std::uint64_t> term_render_present_ns_total{0};
    // #825 render FFI buffer
    std::atomic<std::uint64_t> render_ffi_batch_calls_total{0};
    std::atomic<std::uint64_t> render_ffi_zerocopy_views_total{0};
    std::atomic<std::uint64_t> render_ffi_crossing_ns_accum_total{0};
    std::atomic<std::uint64_t> render_ffi_allocs_frame_total{0};
    // #826 render hotpath dirty/JIT
    std::atomic<std::uint64_t> render_hp_dirty_hits_total{0};
    std::atomic<std::uint64_t> render_hp_present_delta_total{0};
    std::atomic<std::uint64_t> render_hp_jit_coverage_total{0};
    std::atomic<std::uint64_t> render_hp_mutation_impact_total{0};
    // #827 shape/value contracts consteval
    std::atomic<std::uint64_t> sv_contract_hotpath_checks_total{0};
    std::atomic<std::uint64_t> sv_consteval_dispatch_hits_total{0};
    std::atomic<std::uint64_t> sv_stability_transitions_total{0};
    // #828 IR-SoA full enforcement
    std::atomic<std::uint64_t> irsoa_enforce_dirty_skips_total{0};
    std::atomic<std::uint64_t> irsoa_enforce_impact_hybrid_total{0};
    std::atomic<std::uint64_t> irsoa_enforce_pmr_util_pct{0};
    std::atomic<std::uint64_t> irsoa_enforce_relower_savings_total{0};
    // #829 arena live defrag
    std::atomic<std::uint64_t> arena_ldefrag_auto_triggers_total{0};
    std::atomic<std::uint64_t> arena_ldefrag_savings_total{0};
    std::atomic<std::uint64_t> arena_ldefrag_fiber_yield_total{0};
    std::atomic<std::uint64_t> arena_ldefrag_shape_inval_total{0};
    std::atomic<std::uint64_t> arena_ldefrag_pointer_fixup_total{0};
    // ── Open-issues Phase 1 batch (Close all remaining open issues) ──
    // Issue #830: pass-shape-epoch-stats
    std::atomic<std::uint64_t> pass_shape_epoch_total{0};
    std::atomic<std::uint64_t> pass_shape_epoch_hits_total{0};
    std::atomic<std::uint64_t> pass_shape_epoch_savings_total{0};
    // Issue #831: edsl-hotpath-real-stats
    std::atomic<std::uint64_t> edsl_hotpath_real_total{0};
    std::atomic<std::uint64_t> edsl_hotpath_real_hits_total{0};
    std::atomic<std::uint64_t> edsl_hotpath_real_savings_total{0};
    // Issue #832: dead-coercion-elim-stats
    std::atomic<std::uint64_t> dead_coercion_elim_total{0};
    std::atomic<std::uint64_t> dead_coercion_elim_hits_total{0};
    std::atomic<std::uint64_t> dead_coercion_elim_savings_total{0};
    // Issue #833: occurrence-renarrow-stats
    std::atomic<std::uint64_t> occurrence_renarrow_total{0};
    std::atomic<std::uint64_t> occurrence_renarrow_hits_total{0};
    std::atomic<std::uint64_t> occurrence_renarrow_savings_total{0};
    // Issue #834: linear-escape-mutate-stats
    std::atomic<std::uint64_t> linear_escape_mutate_total{0};
    std::atomic<std::uint64_t> linear_escape_mutate_hits_total{0};
    std::atomic<std::uint64_t> linear_escape_mutate_savings_total{0};
    // Issue #835: typed-mutate-coercion-stats
    std::atomic<std::uint64_t> typed_mutate_coercion_total{0};
    std::atomic<std::uint64_t> typed_mutate_coercion_hits_total{0};
    std::atomic<std::uint64_t> typed_mutate_coercion_savings_total{0};
    // Issue #836: fiber-epoch-type-safety-stats
    std::atomic<std::uint64_t> fiber_epoch_type_total{0};
    std::atomic<std::uint64_t> fiber_epoch_type_hits_total{0};
    std::atomic<std::uint64_t> fiber_epoch_type_savings_total{0};
    // Issue #837: sv-verification-feedback-mutate-stats
    std::atomic<std::uint64_t> sv_feedback_mutate_total{0};
    std::atomic<std::uint64_t> sv_feedback_mutate_hits_total{0};
    std::atomic<std::uint64_t> sv_feedback_mutate_savings_total{0};
    // Issue #838: seva-longrunning-harness-v2-stats
    std::atomic<std::uint64_t> seva_harness_v2_total{0};
    std::atomic<std::uint64_t> seva_harness_v2_hits_total{0};
    std::atomic<std::uint64_t> seva_harness_v2_savings_total{0};
    // Issue #839: typed-mutation-audit-stats
    std::atomic<std::uint64_t> typed_mut_audit_total{0};
    std::atomic<std::uint64_t> typed_mut_audit_hits_total{0};
    std::atomic<std::uint64_t> typed_mut_audit_savings_total{0};
    // Issue #840: stable-ref-full-provenance-v2-stats
    std::atomic<std::uint64_t> stable_ref_full_v2_total{0};
    std::atomic<std::uint64_t> stable_ref_full_v2_hits_total{0};
    std::atomic<std::uint64_t> stable_ref_full_v2_savings_total{0};
    // Issue #842: longrunning-ai-infra-stats
    std::atomic<std::uint64_t> longrun_ai_infra_total{0};
    std::atomic<std::uint64_t> longrun_ai_infra_hits_total{0};
    std::atomic<std::uint64_t> longrun_ai_infra_savings_total{0};
    // Issue #843: ai-native-meta-extension-stats
    std::atomic<std::uint64_t> ai_native_meta_total{0};
    std::atomic<std::uint64_t> ai_native_meta_hits_total{0};
    std::atomic<std::uint64_t> ai_native_meta_savings_total{0};
    // Issue #844: orchestration-telemetry-pipeline-stats
    std::atomic<std::uint64_t> orch_telemetry_total{0};
    std::atomic<std::uint64_t> orch_telemetry_hits_total{0};
    std::atomic<std::uint64_t> orch_telemetry_savings_total{0};
    // Issue #845: per-fiber-exception-state-stats
    std::atomic<std::uint64_t> per_fiber_ex_state_total{0};
    std::atomic<std::uint64_t> per_fiber_ex_state_hits_total{0};
    std::atomic<std::uint64_t> per_fiber_ex_state_savings_total{0};
    // Issue #1483 C4: adaptive safepoint threshold (exponential
    // backoff per default heuristic (a)). The threshold is
    // consulted at request_gc_safepoint() to decide between
    // immediate vs deferred safepoint — when the threshold is
    // > 0 AND the current pressure signal (mutation_stack_depth
    // via get_per_fiber_mutation_stack_depth_current_max) is
    // high, the request is deferred even if mutation_boundary_depth
    // == 0. The threshold doubles on each deferral (capped at
    // 1024) and resets to 0 on successful immediate.
    //   - safepoint_adaptive_threshold: current threshold value
    //     (CAS-doubled on deferral, reset to 0 on immediate).
    //   - safepoint_adaptive_defer_count: count of deferrals
    //     triggered by the adaptive threshold (not the natural
    //     mutation_boundary_depth > 0 path).
    std::atomic<std::uint64_t> safepoint_adaptive_threshold{0};
    std::atomic<std::uint64_t> safepoint_adaptive_defer_count{0};
    // Issue #1483 C2: per-fiber mutation_stack_depth metrics.
    //   - per_fiber_mutation_stack_depth_max: lifetime
    //     high-water mark across all observed fibers
    //     (monotonic; never decreases). Updated via
    //     CAS on every cp.mutation_stack_depth assignment
    //     site (L186 / L316 / L450 of
    //     evaluator_fiber_mutation.cpp).
    //   - per_fiber_mutation_stack_depth_current_max:
    //     resettable high-water mark across the current
    //     set of live fibers (decreases as fibers exit).
    //     Useful for "what's the active max right now"
    //     queries vs. "what was the all-time max".
    std::atomic<std::uint64_t> per_fiber_mutation_stack_depth_max{0};
    std::atomic<std::uint64_t> per_fiber_mutation_stack_depth_current_max{0};
    // Issue #1493: mutation_stack_depth histogram (buckets:
    // 0, 1, 2, 3, 4, 5-7, 8-15, 16+). Bumped on each depth sample.
    static constexpr std::size_t kMutationStackDepthHistBuckets = 8;
    std::atomic<std::uint64_t> mutation_stack_depth_histogram[kMutationStackDepthHistBuckets]{};
    // Issue #1493: process-mirrored safepoint wait-while-mutation (µs + count).
    // Also accumulated process-wide in gc_hooks for serve-only paths.
    std::atomic<std::uint64_t> safepoint_wait_while_mutation_held_us{0};
    std::atomic<std::uint64_t> safepoint_wait_while_mutation_held_count{0};
    // Issue #1493: hold-time driven gc_frequency_tune_ratio adjustments.
    std::atomic<std::uint64_t> safepoint_frequency_adapt_up_total{0};
    std::atomic<std::uint64_t> safepoint_frequency_adapt_down_total{0};
    // Issue #846: aot-hotswap-pipeline-stats
    std::atomic<std::uint64_t> aot_hotswap_pipe_total{0};
    std::atomic<std::uint64_t> aot_hotswap_pipe_hits_total{0};
    std::atomic<std::uint64_t> aot_hotswap_pipe_savings_total{0};
    // Issue #847: macro-hygiene-query-provenance-v2-stats
    std::atomic<std::uint64_t> macro_hyg_query_v2_total{0};
    std::atomic<std::uint64_t> macro_hyg_query_v2_hits_total{0};
    std::atomic<std::uint64_t> macro_hyg_query_v2_savings_total{0};
    // Issue #848: reflection-edsl-extension-v2-stats
    std::atomic<std::uint64_t> reflect_edsl_v2_total{0};
    std::atomic<std::uint64_t> reflect_edsl_v2_hits_total{0};
    std::atomic<std::uint64_t> reflect_edsl_v2_savings_total{0};
    // Issue #849: self-evolution-hygiene-dirty-epoch-stats
    std::atomic<std::uint64_t> selfevo_hyg_dirty_total{0};
    std::atomic<std::uint64_t> selfevo_hyg_dirty_hits_total{0};
    std::atomic<std::uint64_t> selfevo_hyg_dirty_savings_total{0};
    // Issue #850: sv-verification-feedback-closedloop-stats
    std::atomic<std::uint64_t> sv_fb_closedloop_total{0};
    std::atomic<std::uint64_t> sv_fb_closedloop_hits_total{0};
    std::atomic<std::uint64_t> sv_fb_closedloop_savings_total{0};
    // Issue #851: pattern-defuse-hygiene-full-stats
    std::atomic<std::uint64_t> pattern_defuse_hyg_total{0};
    std::atomic<std::uint64_t> pattern_defuse_hyg_hits_total{0};
    std::atomic<std::uint64_t> pattern_defuse_hyg_savings_total{0};
    // Issue #852: stable-ref-mutation-log-hardening-stats
    std::atomic<std::uint64_t> stable_ref_mutlog_total{0};
    std::atomic<std::uint64_t> stable_ref_mutlog_hits_total{0};
    std::atomic<std::uint64_t> stable_ref_mutlog_savings_total{0};
    // Issue #853: dirtyaware-impact-enforcement-v2-stats
    std::atomic<std::uint64_t> dirty_impact_v2_total{0};
    std::atomic<std::uint64_t> dirty_impact_v2_hits_total{0};
    std::atomic<std::uint64_t> dirty_impact_v2_savings_total{0};
    // Issue #854: live-irclosure-envframe-gc-stats
    std::atomic<std::uint64_t> live_irclosure_gc_total{0};
    std::atomic<std::uint64_t> live_irclosure_gc_hits_total{0};
    std::atomic<std::uint64_t> live_irclosure_gc_savings_total{0};
    // Issue #855: source-marker-linear-consistency-stats
    std::atomic<std::uint64_t> src_marker_linear_total{0};
    std::atomic<std::uint64_t> src_marker_linear_hits_total{0};
    std::atomic<std::uint64_t> src_marker_linear_savings_total{0};
    // Issue #856: terminal-buffer-diff-present-stats
    std::atomic<std::uint64_t> term_buf_diff_total{0};
    std::atomic<std::uint64_t> term_buf_diff_hits_total{0};
    std::atomic<std::uint64_t> term_buf_diff_savings_total{0};
    // Issue #857: render-observability-v2-stats
    std::atomic<std::uint64_t> render_obs_v2_total{0};
    std::atomic<std::uint64_t> render_obs_v2_hits_total{0};
    std::atomic<std::uint64_t> render_obs_v2_savings_total{0};
    // Issue #858: render-jit-soa-hotpath-stats
    std::atomic<std::uint64_t> render_jit_soa_total{0};
    std::atomic<std::uint64_t> render_jit_soa_hits_total{0};
    std::atomic<std::uint64_t> render_jit_soa_savings_total{0};
    // Issue #859: arena-live-defrag-full-v2-stats
    std::atomic<std::uint64_t> arena_ldefrag_v2_total{0};
    std::atomic<std::uint64_t> arena_ldefrag_v2_hits_total{0};
    std::atomic<std::uint64_t> arena_ldefrag_v2_savings_total{0};
    // Issue #860: ir-soa-dirty-hybrid-full-v2-stats
    std::atomic<std::uint64_t> irsoa_dirty_v2_total{0};
    std::atomic<std::uint64_t> irsoa_dirty_v2_hits_total{0};
    std::atomic<std::uint64_t> irsoa_dirty_v2_savings_total{0};
    // Issue #861: value-shape-consteval-full-v2-stats
    std::atomic<std::uint64_t> val_shape_ceval_v2_total{0};
    std::atomic<std::uint64_t> val_shape_ceval_v2_hits_total{0};
    std::atomic<std::uint64_t> val_shape_ceval_v2_savings_total{0};
    // Issue #862: defuse-infer-partial-stats
    std::atomic<std::uint64_t> defuse_infer_part_total{0};
    std::atomic<std::uint64_t> defuse_infer_part_hits_total{0};
    std::atomic<std::uint64_t> defuse_infer_part_savings_total{0};
    // Issue #863: ownership-escape-postmutate-stats
    std::atomic<std::uint64_t> own_escape_post_total{0};
    std::atomic<std::uint64_t> own_escape_post_hits_total{0};
    std::atomic<std::uint64_t> own_escape_post_savings_total{0};
    // Issue #864: typed-mutation-audit-pass-stats
    std::atomic<std::uint64_t> typed_audit_pass_total{0};
    std::atomic<std::uint64_t> typed_audit_pass_hits_total{0};
    std::atomic<std::uint64_t> typed_audit_pass_savings_total{0};
    // Issue #865: sv-backend-emit-bidirectional-stats
    std::atomic<std::uint64_t> sv_backend_bi_total{0};
    std::atomic<std::uint64_t> sv_backend_bi_hits_total{0};
    std::atomic<std::uint64_t> sv_backend_bi_savings_total{0};
    // Issue #866: large-sv-pattern-defuse-stats
    std::atomic<std::uint64_t> large_sv_pattern_total{0};
    std::atomic<std::uint64_t> large_sv_pattern_hits_total{0};
    std::atomic<std::uint64_t> large_sv_pattern_savings_total{0};
    // Issue #867: longrunning-stable-ref-dirty-stats
    std::atomic<std::uint64_t> longrun_sref_dirty_total{0};
    std::atomic<std::uint64_t> longrun_sref_dirty_hits_total{0};
    std::atomic<std::uint64_t> longrun_sref_dirty_savings_total{0};
    // Issue #868: sv-eda-primitives-cluster-stats
    std::atomic<std::uint64_t> sv_eda_prims_total{0};
    std::atomic<std::uint64_t> sv_eda_prims_hits_total{0};
    std::atomic<std::uint64_t> sv_eda_prims_savings_total{0};
    // Issue #869: primitives-resource-quota-fiber-stats
    std::atomic<std::uint64_t> prim_quota_fiber_total{0};
    std::atomic<std::uint64_t> prim_quota_fiber_hits_total{0};
    std::atomic<std::uint64_t> prim_quota_fiber_savings_total{0};
    // Issue #870: declarative-primitive-registry-stats
    std::atomic<std::uint64_t> decl_prim_reg_total{0};
    std::atomic<std::uint64_t> decl_prim_reg_hits_total{0};
    std::atomic<std::uint64_t> decl_prim_reg_savings_total{0};
    // Issue #872: primitives-namespace-alias-stats
    std::atomic<std::uint64_t> prim_ns_alias_total{0};
    std::atomic<std::uint64_t> prim_ns_alias_hits_total{0};
    std::atomic<std::uint64_t> prim_ns_alias_savings_total{0};
    // Issue #875: guard-steal-gc-safety-v2-stats
    std::atomic<std::uint64_t> guard_steal_gc_v2_total{0};
    std::atomic<std::uint64_t> guard_steal_gc_v2_hits_total{0};
    std::atomic<std::uint64_t> guard_steal_gc_v2_savings_total{0};
    // Issue #876: dirtyaware-ir-cache-consistency-stats
    std::atomic<std::uint64_t> dirty_ircache_cons_total{0};
    std::atomic<std::uint64_t> dirty_ircache_cons_hits_total{0};
    std::atomic<std::uint64_t> dirty_ircache_cons_savings_total{0};
    // Issue #877: stats-builder-refactor-stats
    std::atomic<std::uint64_t> stats_builder_ref_total{0};
    std::atomic<std::uint64_t> stats_builder_ref_hits_total{0};
    std::atomic<std::uint64_t> stats_builder_ref_savings_total{0};
    // Issue #878: load-or-zero-helper-stats
    std::atomic<std::uint64_t> load_or_zero_help_total{0};
    std::atomic<std::uint64_t> load_or_zero_help_hits_total{0};
    std::atomic<std::uint64_t> load_or_zero_help_savings_total{0};
    // Issue #879: cpp26-modernization-sweep-stats
    std::atomic<std::uint64_t> cpp26_mod_sweep_total{0};
    std::atomic<std::uint64_t> cpp26_mod_sweep_hits_total{0};
    std::atomic<std::uint64_t> cpp26_mod_sweep_savings_total{0};
    // Issue #880: metrics-meta-reflection-stats
    std::atomic<std::uint64_t> metrics_meta_refl_total{0};
    std::atomic<std::uint64_t> metrics_meta_refl_hits_total{0};
    std::atomic<std::uint64_t> metrics_meta_refl_savings_total{0};
    // Issue #881: test-harness-bootstrap-stats
    std::atomic<std::uint64_t> test_harness_boot_total{0};
    std::atomic<std::uint64_t> test_harness_boot_hits_total{0};
    std::atomic<std::uint64_t> test_harness_boot_savings_total{0};
    // Issue #882: bundle-codegen-decouple-stats
    std::atomic<std::uint64_t> bundle_codegen_dec_total{0};
    std::atomic<std::uint64_t> bundle_codegen_dec_hits_total{0};
    std::atomic<std::uint64_t> bundle_codegen_dec_savings_total{0};
    // Issue #883: test-bundle-migration-stats
    std::atomic<std::uint64_t> test_bundle_mig_total{0};
    std::atomic<std::uint64_t> test_bundle_mig_hits_total{0};
    std::atomic<std::uint64_t> test_bundle_mig_savings_total{0};
    // Issue #884: test-profile-flag-stats
    std::atomic<std::uint64_t> test_profile_flag_total{0};
    std::atomic<std::uint64_t> test_profile_flag_hits_total{0};
    std::atomic<std::uint64_t> test_profile_flag_savings_total{0};
    // Issue #885: test-harness-module-stats
    std::atomic<std::uint64_t> test_harness_mod_total{0};
    std::atomic<std::uint64_t> test_harness_mod_hits_total{0};
    std::atomic<std::uint64_t> test_harness_mod_savings_total{0};
    // Issue #886: test-json-report-stats
    std::atomic<std::uint64_t> test_json_report_total{0};
    std::atomic<std::uint64_t> test_json_report_hits_total{0};
    std::atomic<std::uint64_t> test_json_report_savings_total{0};
    // Issue #395: gcc16-modules-buildenv-stats
    std::atomic<std::uint64_t> gcc16_modules_env_total{0};
    std::atomic<std::uint64_t> gcc16_modules_env_hits_total{0};
    std::atomic<std::uint64_t> gcc16_modules_env_savings_total{0};
    // Issue #620: StableNodeRef provenance query counter.
    // Bumped on every (query:stable-ref-provenance) call so the
    // Agent can see how often the provenance surface is being
    // exercised (high rate = hot path; needs benchmarking).
    std::atomic<std::uint64_t> stable_ref_provenance_query_total{0};
    // Issue #623: arena auto-compact threshold setter counter.
    // Bumped on every (arena:set-auto-compact-threshold ratio)
    // call so the Agent can see how often the production-harden
    // threshold is being tuned at runtime (unusual in steady-state;
    // bursts indicate a memory-pressure watchdog is reacting).
    std::atomic<std::uint64_t> arena_auto_compact_threshold_set_total{0};
    // Issue #625: pass-pipeline run counter. Bumped once per full
    // pass_pipeline() invocation (not per-pass; not per-block).
    // Pairs with the dirty-block short-circuit counters from
    // #494/#606 so the Agent can see how often the full pipeline
    // runs vs how often the dirty short-circuit short-circuits
    // each pass.
    std::atomic<std::uint64_t> pass_pipeline_runs_total{0};
    // Issue #631: StableNodeRef cross-fiber / multi-agent SV
    // provenance counters. These are foundation scaffolding for the
    // future enforcement work (AC1 + AC2 from the issue body) —
    // the actual bumps happen inside the StableNodeRef
    // validate_with_provenance + Guard dtor wire-up (follow-up).
    // P0 ships the counters + the agent-visible
    // (query:stable-ref-provenance-sv-stats-hash) primitive so
    // the Agent has a dashboard today; the values are 0 until
    // the enforcement work ships.
    std::atomic<std::uint64_t> cross_fiber_violations_total{0};
    std::atomic<std::uint64_t> safe_resolves_total{0};
    // Issue #632: atomic-batch SV-specific observability counters.
    // Foundation scaffolding for the AC2 + AC3 enforcement work —
    // the bumps happen when Guard aggregates rollback + impact
    // for SV-tagged mutates inside an atomic batch. P0 ships the
    // counters + the agent-visible
    // (query:atomic-batch-sv-stats-hash) primitive so the Agent
    // has a dashboard today; values are 0 until AC2 + AC3 wire-up.
    std::atomic<std::uint64_t> atomic_batch_sv_rollback_total{0};
    std::atomic<std::uint64_t> atomic_batch_sv_impact_nodes_total{0};
    // Issue #633: stdlib commercial-evolution reverse-ask counters.
    // Foundation scaffolding for the future DEFINE_PRIMITIVE
    // macro work (#633 AC3) + AI-generated primitive registration
    // tracking (#633 AC4). P0 ships the counters + the
    // (query:stdlib-compiler-demands-stats-hash) primitive so the
    // Agent has a dashboard today; values are 0 until the future
    // extension macro + AI-generate path lands.
    std::atomic<std::uint64_t> stdlib_extension_count_total{0};
    std::atomic<std::uint64_t> ai_native_primitive_hits_total{0};
    // Issue #806: registry-extension validation-pass counter
    // (P0 stdlib AI-native extension surface foundation; refines/
    // consolidates #775 Extension Kit + #711 + #480; non-duplicative
    // with #775 query:extension-kit-stats and #633 query:stdlib-
    // compiler-demands-stats-hash). #806 introduces the FIRST
    // observability signal for `validation_pass` (the *pass* counter
    // for `(primitive:extend-registry-safe ...)` auto-validation
    // pipeline) — distinct from #775's contract_violations_caught
    // (which tracks the *failure* counter). The Agent-side SLO
    // requires BOTH: total extensions + validation passes + meta
    // completeness. The total extension count already exists via
    // `stdlib_extension_count_total`; the validation *pass* count
    // is what #806 adds so the slo-validation-pct (pass / total)
    // derivation has a direct atomic instead of being computed as
    // (total - violations_caught). P0 ships the counter + the
    // (query:registry-extension-stats, schema 806) primitive so
    // the Agent has a deployment-grade dashboard today; value is
    // 0 until the future `(primitive:extend-registry-safe ...)`
    // primitive + capture-contract auto-probe + PrimMeta backfill
    // wire-up lands.
    std::atomic<std::uint64_t> registry_extension_validation_passes_total{0};
    // Issue #637: IRClosure + EnvFrame versioning + bridge
    // invalidate protocol counters (P0 memory-safety foundation).
    // These are scaffolding for the future AC1 + AC2 + AC3
    // enforcement work — the bumps happen inside
    // apply_closure + materialize_call_env + invalidate_function
    // when they wire in bridge_epoch / EnvFrame.version checks
    // + Guard dtor-driven rebuilds. P0 ships the counters +
    // the agent-visible (query:closure-bridge-safety-stats-hash)
    // primitive so the Agent has a dashboard today; values
    // are 0 until the enforcement work ships.
    //   - closure_invalidation_post_mutate_total: count of
    //     invalidate_function calls that fired AFTER a
    //     workspace mutate (rebind / set-body / typed-mutate)
    //     rather than on cold compilation. The ratio of
    //     (this / invalidate_function_calls) measures the
    //     share of invalidations triggered by the AI Agent
    //     hot-swap path vs. cold compile.
    //   - closure_version_mismatch_caught_total: bridge_epoch
    //     / EnvFrame.version mismatch detections in
    //     apply_closure (both paths) + materialize_call_env.
    //     Each catch prevents a potential UAF / stale-env
    //     read on a long-lived closure — count is the
    //     "near-miss" rate the safety net prevented.
    //   - closure_safe_rebuild_total: successful bridge
    //     rebuilds performed after a mismatch (Guard dtor
    //     path). The ratio (safe_rebuilds / mismatches) is
    //     the recovery rate — 1.0 means every detected
    //     mismatch was safely recovered, < 1.0 means some
    //     fell back to error path (uncaught).
    std::atomic<std::uint64_t> closure_invalidation_post_mutate_total{0};
    std::atomic<std::uint64_t> closure_version_mismatch_caught_total{0};
    std::atomic<std::uint64_t> closure_safe_rebuild_total{0};
    // Issue #640: Verification feedback → structured SV mutate
    // closed-loop counters (P0 EDA-SV-Review foundation).
    // These are scaffolding for the future AC1 + AC2 + AC3
    // enforcement work — the bumps happen when
    // eda:apply-verification-feedback is wired to Guard +
    // StableNodeRef capture + sv_ir structured mutate
    // (#640 AC1), when Guard success triggers the
    // hardware_backend re-emit hook (#640 AC2), and when
    // StableNodeRef provenance check is strengthened on SV
    // mutate paths (#640 AC3). P0 ships the counters + the
    // agent-visible (query:sv-verification-closedloop-stats)
    // primitive so the Agent has a dashboard today; values
    // are 0 until the enforcement work ships.
    //   - sv_verify_feedback_apply_total: AC1 — count of
    //     (eda:apply-verification-feedback report) invocations
    //     that successfully routed a coverage/assert/cex
    //     signal through Guard + StableNodeRef + sv_ir
    //     structured mutate. Ratio of (this / total feedback
    //     reports) measures how much of the external
    //     verification loop actually drives SV mutate.
    //   - sv_guard_reemit_hook_total: AC2 — count of Guard
    //     success paths that triggered the hardware_backend
    //     re-emit hook post-SV-mutate. 1.0 ratio = every
    //     successful SV mutate led to a re-emit (no
    //     straggler stale-emit risk).
    //   - sv_stable_ref_provenance_strict_total: AC3 — count
    //     of strengthened StableNodeRef provenance checks
    //     that caught a fiber/workspace mismatch on the SV
    //     mutate path. Each catch prevents a potential
    //     cross-fiber UAF on the long-lived SV IR.
    std::atomic<std::uint64_t> sv_verify_feedback_apply_total{0};
    std::atomic<std::uint64_t> sv_guard_reemit_hook_total{0};
    std::atomic<std::uint64_t> sv_stable_ref_provenance_strict_total{0};
    // Issue #641: StableNodeRef cross-fiber provenance
    // enforcement counters (P0 EDSL-Review foundation).
    // These are scaffolding for the future AC1 + AC2 + AC4
    // enforcement work — the bumps happen when
    // query:/mutate: + Guard dtor enforce fiber_id /
    // workspace_id match (after is_valid_in, on mismatch
    // → boundary violation or safe resolve, #641 AC1),
    // when Guard success triggers auto-refresh of the
    // provenance stamp (#641 AC2), and when the
    // provenance-checked SV feedback path is wired
    // (#641 AC4). P0 ships the counters + the
    // agent-visible (query:stable-ref-provenance-sv-stats)
    // primitive so the Agent has a dashboard today;
    // values are 0 until the enforcement work ships.
    //   - stable_ref_fiber_provenance_check_total: AC1 —
    //     count of fiber_id / workspace_id match checks
    //     fired in query:/mutate: + Guard dtor. The ratio
    //     (mismatch_count / check_count) is the
    //     cross-agent theft rate the safety net caught.
    //   - stable_ref_provenance_auto_refresh_total: AC2 —
    //     count of Guard success paths that auto-refreshed
    //     the provenance stamp (fiber_id / workspace_id /
    //     mutation_id_at_capture / last_validated_generation).
    //     1.0 ratio = every Guard success refreshed.
    //   - stable_ref_sv_feedback_wired_total: AC4 — count
    //     of provenance-checked SV feedback path
    //     invocations (StableNodeRef + sv_ir + Guard +
    //     eda:apply-verification-feedback chain).
    std::atomic<std::uint64_t> stable_ref_fiber_provenance_check_total{0};
    std::atomic<std::uint64_t> stable_ref_provenance_auto_refresh_total{0};
    std::atomic<std::uint64_t> stable_ref_sv_feedback_wired_total{0};
    // Issue #642: Arena Auto-Compaction + Fiber/GC Safepoint
    // Coordination counters (P0 Prompt6-MemorySafety foundation).
    // These are scaffolding for the future AC1 + AC2 + AC3
    // enforcement work — the bumps happen when allocate_raw
    // auto-triggers compact on fragmentation > threshold +
    // coordinates fiber safepoint (#642 AC1), when compact/
    // defrag is enhanced with live move + yield support
    // (#642 AC2), and when Guard/invalidate paths wire
    // request_defrag (#642 AC3). P0 ships the counters +
    // the agent-visible (query:arena-auto-compaction-stats)
    // primitive so the Agent has a dashboard today; values
    // are 0 until the enforcement work ships.
    //   - arena_auto_compact_trigger_total: AC1 — count of
    //     allocate_raw auto-trigger fires when fragmentation
    //     crosses the threshold + fiber safepoint was
    //     successfully coordinated. High trigger rate on
    //     long-running Agent sessions = memory pressure is
    //     real; 0 triggers under 10k mutate rounds = the
    //     threshold is too lax.
    //   - arena_live_move_yield_total: AC2 — count of live
    //     move + yield events during compact/defrag. High
    //     count = safe live moves happened (no stop-the-
    //     world pauses); 0 = everything was stop-the-world
    //     (latency risk under fiber load).
    //   - arena_guard_request_defrag_total: AC3 — count of
    //     Guard/invalidate paths that triggered
    //     request_defrag. 1.0 ratio = every Guard success
    //     led to a defrag request (no orphaned fragments).
    std::atomic<std::uint64_t> arena_auto_compact_trigger_total{0};
    std::atomic<std::uint64_t> arena_live_move_yield_total{0};
    std::atomic<std::uint64_t> arena_guard_request_defrag_total{0};
    // Issue #1518: live relocate + compact deopt coordination.
    std::atomic<std::uint64_t> arena_live_relocate_total{0};
    std::atomic<std::uint64_t> arena_compact_deopt_triggered_total{0};
    std::atomic<std::uint64_t> arena_compact_deopt_throttled_total{0};
    std::atomic<std::uint64_t> arena_frag_post_compact_bp{0};
    std::atomic<std::uint64_t> arena_compact_soft_gated_boundary_total{0};
    // Issue #1521: ShapeProfiler versioning + Arena compact synergy.
    std::atomic<std::uint64_t> shape_inval_on_compact_triggered_total{0};
    std::atomic<std::uint64_t> deopt_from_arena_compact_total{0};
    std::atomic<std::uint64_t> shape_stability_post_compact_preserved_total{0};
    std::atomic<std::uint64_t> deopt_storm_compact_suppressed_total{0};
    // Issue #643: DEFINE_PRIMITIVE macro/template + AI-native
    // primitives-meta introspection counters (P0 Stdlib-Impl-P1
    // foundation — implements #633 AC3+AC4).
    // These are scaffolding for the future AC1 + AC2 + AC3
    // enforcement work — the bumps happen when DEFINE_PRIMITIVE
    // macro/template is wired into evaluator.ixx + registry
    // (#643 AC1), when query:primitives-meta [name] / query:schema-
    // of name / query:primitives-by-category primitives are
    // shipped (#643 AC2), and when PRIM_ERROR macro/helper is
    // introduced to unify error path with make_primitive_error +
    // provenance + counter bump (#643 AC3). P0 ships the
    // counters + the agent-visible (query:primitives-meta [name])
    // primitive so the Agent has a dashboard today; values are
    // 0 until the enforcement work ships.
    //   - define_primitive_macro_used_total: AC1 — count of
    //     declarative DEFINE_PRIMITIVE(name, arity, fn, doc,
    //     schema_or_contract) registrations. The ratio of
    //     (this / total registrations) measures the share of
    //     primitives registered via the declarative macro vs
    //     the legacy manual lambdas (higher = more AI-friendly
    //     style adoption).
    //   - prim_error_unified_total: AC3 — count of PRIM_ERROR
    //     macro/helper invocations that used the unified error
    //     path (make_primitive_error + provenance + counter
    //     bump + observability log). The ratio of (this /
    //     total primitive errors) measures how much of the
    //     codebase has migrated to the unified path.
    //   - primitives_meta_query_total: AC2 — count of
    //     (query:primitives-meta [name]) hit-rate. Distinct
    //     from primitives_meta_catalog_query_total (#617)
    //     which tracks the catalog primitive.
    std::atomic<std::uint64_t> define_primitive_macro_used_total{0};
    std::atomic<std::uint64_t> prim_error_unified_total{0};
    std::atomic<std::uint64_t> primitives_meta_query_total{0};
    // Issue #644: AOT Hot-Reload func_table Refcount +
    // Per-Region Isolation + Metrics counters (P0 Runtime-Gap
    // + AOT foundation — non-duplicative to #624 #601 #358).
    // These are scaffolding for the future AC1 + AC2 + AC4
    // enforcement work — the bumps happen when
    // aura_reload_aot_module extends func_table update with
    // atomic refcount (bump new, decrement old after grace
    // period or epoch check, #644 AC1), when region filtering
    // is re-applied on reload (re-apply filter or reject if
    // region mismatch for current agent/workspace, #644 AC2),
    // and when MutationBoundaryGuard + fiber yield wire-up
    // provides the safe reload point (#644 AC4). P0 ships the
    // counters + the agent-visible
    // (query:aot-reload-func-table-stats) primitive so the
    // Agent has a dashboard today; values are 0 until the
    // enforcement work ships.
    //   - aot_func_table_ref_bump_total: AC1 — count of atomic
    //     refcount bumps when a new func_table entry is
    //     installed (every reload attempt that gets past the
    //     version check). High bumps under multi-agent fleet
    //     = hot-reload is the active deployment channel.
    //   - aot_func_table_ref_decrement_total: AC1 — count of
    //     atomic refcount decrements when old func_table entry
    //     is retired (after grace period or epoch check).
    //     Ratio (decrement / bump) ≈ 1.0 = old entries are
    //     always cleaned up (no leak); < 1.0 = leak risk.
    //   - aot_region_filter_reapply_total: AC2 — count of
    //     region filtering re-applied on reload (per
    //     agent/workspace region check). 1.0 ratio per reload
    //     = region isolation enforced on every reload.
    std::atomic<std::uint64_t> aot_func_table_ref_bump_total{0};
    std::atomic<std::uint64_t> aot_func_table_ref_decrement_total{0};
    std::atomic<std::uint64_t> aot_region_filter_reapply_total{0};
    // Issue #645: Work-Stealing LIFO/FIFO Adaptive Bias +
    // YieldReason / outermost Mutation Depth counters
    // (P0 Runtime-Gap + Scheduler foundation — non-duplicative
    // to #618 #588 #451).
    // These are scaffolding for the future AC1 + AC2 + AC4
    // enforcement work — the bumps happen when worker steal
    // loop (or scheduler next_worker) consults
    // victim->last_yield_reason() + is_at_mutation_boundary_safe
    // (outermost depth==0) to bias the steal decision
    // (#645 AC1), when the simple adaptive LIFO/FIFO tuning
    // fires on high steal_deferred_mutation_boundary_count
    // (#645 AC2), and when the orchestration tune primitive
    // from #618 is wired to consume these counters
    // (#645 AC4). P0 ships the counters + the agent-visible
    // (query:scheduler-steal-bias-stats) primitive so the
    // Agent has a dashboard today; values are 0 until the
    // enforcement work ships.
    //   - scheduler_lifo_hits_total: AC1 — count of LIFO
    //     local hits when stealing from the worker deque.
    //     High LIFO rate = worker is hitting its own recent
    //     local work (low steal pressure / locality good).
    //   - scheduler_fifo_steals_total: AC1 — count of FIFO
    //     steals from a victim worker. High FIFO rate = high
    //     cross-worker steal pressure (could indicate
    //     load imbalance or LLM bottleneck where mutation-
    //     heavy victims block local workers).
    //   - scheduler_mutation_deferred_bias_total: AC1+AC2 —
    //     count of times we deferred stealing from a victim
    //     fiber because its outermost mutation depth > 0
    //     (inner MutationBoundary — risk of stale-env /
    //     Guard dependency if stolen). Ratio
    //     (deferred / total steals) = mutation-heavy
    //     bias pressure the adaptive loop needs to react to.
    std::atomic<std::uint64_t> scheduler_lifo_hits_total{0};
    std::atomic<std::uint64_t> scheduler_fifo_steals_total{0};
    std::atomic<std::uint64_t> scheduler_mutation_deferred_bias_total{0};
    // Issue #646: GC Safepoint Deferral + Backoff Only for
    // Outermost MutationBoundary + Contention Metrics
    // counters (P0 Runtime-Gap + GC production-readiness
    // foundation — non-duplicative to #642 #623 #591).
    // These are scaffolding for the future AC1 + AC2 + AC4
    // enforcement work — the bumps happen when
    // aura_evaluator_request_gc_safepoint (or fiber check)
    // distinguishes outermost vs inner MutationBoundary
    // (depth==1 outermost → full deferral; inner → short-
    // yield/proceed, #646 AC1), when backoff fires on
    // repeated deferral under contention (#646 AC2), and
    // when the scheduler GC phase + fiber yield_classification
    // is wired to consume these counters (#646 AC4). P0
    // ships the counters + the agent-visible
    // (query:gc-safepoint-deferral-stats) primitive so the
    // Agent has a dashboard today; values are 0 until the
    // enforcement work ships.
    //   - gc_outermost_deferral_total: AC1 — count of full
    //     deferrals when the GC safepoint request lands
    //     inside an outermost MutationBoundary (depth==1).
    //     High rate = GC request blocked on outermost
    //     mutation stack (production memory/GC pressure
    //     signal).
    //   - gc_inner_proceeded_total: AC1 — count of inner
    //     MutationBoundary (depth>1) where the safepoint
    //     short-yield/proceeded instead of fully waiting.
    //     Ratio (proceeded / total inner requests) = how
    //     often inner bounds don't block GC.
    //   - gc_backoff_trigger_total: AC2 — count of backoff
    //     fires under repeated deferral / contention.
    //     Ratio (backoff / total deferrals) = how often
    //     contention was severe enough to trigger retry
    //     mitigation.
    std::atomic<std::uint64_t> gc_outermost_deferral_total{0};
    std::atomic<std::uint64_t> gc_inner_proceeded_total{0};
    std::atomic<std::uint64_t> gc_backoff_trigger_total{0};
    // Issue #647: Dual-Path EnvFrame/Env (parent_id_ vs
    // parent_, bindings_symid_ vs bindings_) Cross-Fiber
    // Stale Detection + materialize_call_env After Steal
    // counters (P0 Runtime-Gap + SoA production-readiness
    // foundation — non-duplicative to #637 #589 #355).
    // These are scaffolding for the future AC1 + AC2 + AC4
    // enforcement work — the bumps happen when
    // materialize_call_env + lookup paths validate parent_id_
    // vs current env_frames_ owner after version_ check
    // (#647 AC1), when fiber resume() / g_fiber_sync_mutation_
    // stack_ runs the optional dual-path consistency check
    // or repair for active Env/EnvFrame (#647 AC2), and
    // when GCEnvWalkFn skips/repairs dual-path inconsistent
    // frames (#647 AC4). P0 ships the counters + the
    // agent-visible (query:envframe-dualpath-stale-stats-hash)
    // primitive so the Agent has a dashboard today; values
    // are 0 until the enforcement work ships.
    //   - envframe_cross_fiber_stale_total: AC1 — count of
    //     cross-fiber stale Env/EnvFrame detected post-steal
    //     (parent_id_ mismatch against current env_frames_
    //     owner). High rate on multi-agent fleets = real
    //     production risk of UAF / stale-env read.
    //   - envframe_version_mismatch_post_steal_total: AC1 —
    //     count of version_ stamp mismatches detected
    //     post-steal (in materialize_call_env or post-resume
    //     lookup). Ratio (mismatch / total post-steal
    //     materializes) = how often the version check
    //     catches stale access.
    //   - envframe_dualpath_repair_total: AC2 — count of
    //     dual-path consistency check + repair hits (when
    //     the parent_id_ / bindings_symid_ SoA path is
    //     repaired to match the legacy parent_ / bindings_
    //     pointer view, or vice versa). Ratio (repair /
    //     stale) = recovery rate — 1.0 = every detected
    //     stale was successfully repaired.
    std::atomic<std::uint64_t> envframe_cross_fiber_stale_total{0};
    std::atomic<std::uint64_t> envframe_version_mismatch_post_steal_total{0};
    std::atomic<std::uint64_t> envframe_dualpath_repair_total{0};

    // Issue #1631: mandate post-resume EnvFrame/bridge refresh + linear probe.
    //   resume_forced_refresh_total — complete_post_resume_steal_refresh calls
    //   bridge_epoch_drift_post_steal_total — IRClosure bridge drift detections
    //   bridge_epoch_deopt_walk_post_steal_total — JIT walk deopts after drift
    std::atomic<std::uint64_t> resume_forced_refresh_total{0};
    std::atomic<std::uint64_t> bridge_epoch_drift_post_steal_total{0};
    std::atomic<std::uint64_t> bridge_epoch_deopt_walk_post_steal_total{0};

    // Issue #1634: Guard dtor failure path forced linear + closure probe.
    std::atomic<std::uint64_t> guard_failure_linear_enforce_total{0};

    // Issue #1612: MacroIntroduced marker/provenance refresh on fiber
    // resume / steal / GC compact (query:post-steal-closed-loop-stats schema 1612+).
    std::atomic<std::uint64_t> macro_stale_ref_prevented_total{0};
    std::atomic<std::uint64_t> macro_provenance_repin_total{0};

    // Issue #1908: MutationBoundaryGuard + macro clone provenance hardening
    // (refine #1014 / #1047). Tracks the two boundary-interaction signals
    // mandated by the #1908 AC:
    //   - macro_provenance_repin_on_steal_total: every forced repin of
    //     MacroIntroduced marker + provenance that fires on fiber steal /
    //     resume / outermost Guard exit / PanicCheckpoint transfer. Bumped
    //     from clone_macro_body (MacroIntroduced branch) +
    //     complete_post_resume_steal_refresh (after probe_and_repin_macro_provenance)
    //     + transfer_and_revalidate_panic_checkpoint (post panic restamp).
    //   - hygiene_violation_prevented_on_boundary_total: every time
    //     the boundary interaction (outermost flush dirty/epoch bump + post-steal
    //     probe + PanicCheckpoint transfer coupling) prevented a hygiene
    //     violation from manifesting. Bumped from flush_mutation_boundary
    //     outermost exit + complete_post_resume_steal_refresh (post probe) +
    //     transfer_and_revalidate_panic_checkpoint (post panic restamp).
    // Together these give self-evolution Agents a direct signal that long-running
    // mutation/steal/GC cycles are not silently dropping provenance or letting
    // hygiene violations slip through.
    std::atomic<std::uint64_t> macro_provenance_repin_on_steal_total{0};         // #1908
    std::atomic<std::uint64_t> hygiene_violation_prevented_on_boundary_total{0}; // #1908
    // Issue #1649: composite mutate atomic batch + SyntaxMarker::MacroIntroduced
    // propagation observability (refine #1900 / #1502 / #1472 / #790 / #761 / #737).
    // Predecessor #1908 wired hygiene_violation_prevented_on_boundary_total at the
    // MutationBoundaryGuard boundary; these 2 new counters extend into the atomic
    // batch (begin/end atomic_batch_pinning) path + the mutate template marker
    // propagation path. Distinct from the legacy namespace-scope
    // bump_atomic_batch_hygiene_violation (per-Fiber pin-time count) and
    // existing #1908 hygiene counters — these are the body-named
    // "prevented" / "propagated" series for the atomic-batch closed-loop.
    std::atomic<std::uint64_t> atomic_batch_hygiene_violation_prevented_total{0};
    std::atomic<std::uint64_t> mutate_template_marker_propagated_total{0};
    std::atomic<std::uint64_t> macro_refresh_invoke_total{0};
    std::atomic<std::uint64_t> macro_provenance_probe_total{0};

    // Issue #1614: TypedMutationAudit real invariant suite metrics.
    std::atomic<std::uint64_t> typed_mutation_invariant_audits_total{0};
    std::atomic<std::uint64_t> typed_mutation_invariant_violations_total{0};
    std::atomic<std::uint64_t> typed_mutation_type_ok_total{0};
    std::atomic<std::uint64_t> typed_mutation_linear_ok_total{0};
    std::atomic<std::uint64_t> typed_mutation_prov_ok_total{0};

    // Issue #1615: linear ownership + coercion synergy.
    std::atomic<std::uint64_t> linear_coercion_reval_count{0};
    std::atomic<std::uint64_t> linear_coercion_reval_ok_total{0};
    std::atomic<std::uint64_t> linear_coercion_violations_total{0};
    std::atomic<std::uint64_t> linear_coercion_sites_total{0};
    std::atomic<std::uint64_t> narrow_evidence_propagated_total{0};

    // Issue #1616: IRClosure / ClosureBridge MacroIntroduced + provenance.
    std::atomic<std::uint64_t> ir_provenance_stamped_total{0};
    std::atomic<std::uint64_t> ir_closure_macro_stamped_total{0};
    std::atomic<std::uint64_t> ir_closure_macro_marker_consults_total{0};
    std::atomic<std::uint64_t> macro_introduced_ignored_in_ir_total{0};

    // Issue #756: EnvFrame dual-path consistency enforcement +
    // desync panic policy + GCEnvWalkFn stale handling under
    // concurrent mutation/steal counters backing the
    // (query:envframe-dualpath-policy-stats) primitive. These
    // are public so future evaluator.ixx + evaluator_env.cpp +
    // gc_coordinator can call them at each decision point
    // (mandatory ensure_envframe_dual_path_consistency call in
    // walk_env_frames / GCEnvWalkFn / materialize_call_env /
    // post-rollback paths / desync panic / desync log-and-sync /
    // GCEnvWalkFn stale handling / concurrent steal/resume
    // re-ensure).
    //
    // Non-duplicative with the existing #647 (query:envframe-
    // dualpath-stale-stats-hash — 3 fields: cross-fiber-stale /
    // version-mismatch / dualpath-repair + schema=647) + #418
    // (query:envframe-dualpath-stale-stats legacy int) +
    // #647 (query:envframe-dualpath-stats base flat-int) +
    // existing envframe_desync_detected_ + envframe_gc_walk_
    // safe_skips_ internal atomics.
    // #756 is the FIRST observability surface that tracks the
    // *desync panic policy + GCEnvWalkFn stale handling*
    // specifically — desync-panic-count (panic strict mode
    // firings) + gc-stale-desync-hits (GC walk detected stale
    // + concurrent steal) — as separate per-decision-point
    // counters the Agent consumes to monitor SoA EnvFrame
    // dual-path production safety under concurrency.
    //
    //   - envframe_desync_panic_count_total: # of times the
    //                                        strict-panic policy
    //                                        fired on EnvFrame
    //                                        dual-path desync
    //                                        (length/order mismatch
    //                                        detected + panic /
    //                                        structured error with
    //                                        provenance). Proxy
    //                                        for "how often the
    //                                        strict-panic policy
    //                                        fired in production".
    //   - envframe_gc_stale_desync_hits_total: # of times the
    //                                          GCEnvWalkFn stale
    //                                          check detected a
    //                                          dual-path desync
    //                                          (version_ stale +
    //                                          length/order
    //                                          mismatch) under
    //                                          concurrent
    //                                          steal/mutate.
    //                                          Proxy for "how
    //                                          often GC walk
    //                                          detected stale
    //                                          EnvFrame under
    //                                          concurrency".
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual mandatory ensure_envframe_dual_path_consistency
    // call in walk_env_frames / GCEnvWalkFn / materialize_call_env
    // / post-rollback paths + strict-panic vs log-and-sync policy
    // flag + GCEnvWalkFn stale + concurrent steal/resume
    // re-ensure + tests/test_envframe_dualpath_consistency_
    // concurrent_steal_gc.cpp harness (heavy mutate + steal + GC
    // under dual-path load → assert no desync or caught cleanly +
    // metrics + TSan clean) + #674 + #731 chaos stress
    // integration + docs are all follow-up (each is a dedicated
    // session in evaluator.ixx + evaluator_env.cpp + gc_coordinator
    // + new test + chaos stress + docs).
    std::atomic<std::uint64_t> envframe_desync_panic_count_total{0};
    std::atomic<std::uint64_t> envframe_gc_stale_desync_hits_total{0};

    // Issue #784: mandatory dual-path consistency
    // enforcement + GC/steal resync counters under
    // concurrent mutation (refines #756). 3 NEW atomics:
    //   - envframe_mandatory_enforce_total: # of times
    //     ensure_envframe_dual_path_consistency() was
    //     called at a mandatory entry point (walk_env_frames,
    //     GCEnvWalkFn, materialize_call_env, post-rollback).
    //   - envframe_mandatory_enforce_desync_total: # of
    //     times a mandatory ensure_ call detected a
    //     dual-path desync (length/order mismatch) — the
    //     primary "did the safety net catch a desync?"
    //     signal.
    //   - envframe_concurrent_steal_resync_total: # of
    //     times a concurrent steal/resume triggered a
    //     re-ensure + version re-stamp — covers the
    //     "concurrent steal caused a temporary desync
    //     that was auto-corrected" signal.
    std::atomic<std::uint64_t> envframe_mandatory_enforce_total{0};
    std::atomic<std::uint64_t> envframe_mandatory_enforce_desync_total{0};
    std::atomic<std::uint64_t> envframe_concurrent_steal_resync_total{0};

    // Issue #757: fine-grained MacroIntroduced provenance
    // tracking + dynamic inliner policy + AI-queryable hygiene
    // violation correlation counters backing the
    // (query:macro-hygiene-provenance-stats) primitive. These
    // are public so future ast.ixx FlatAST + marker column +
    // query primitives + InlinePass in lowering + aura_jit +
    // MutationBoundaryGuard + macro_expansion.cpp can call
    // them at each decision point (provenance captured at
    // clone_macro_body / QueryExpr :marker MacroIntroduced
    // :provenance filter hits / hygiene:set-inliner-respect-
    // macro! primitive call / InlinePass respect_macro_hygiene_
    // dynamic check / hygiene_violation_by_macro correlation).
    //
    // Non-duplicative with the existing #654 (query:macro-
    // hygiene-fiber-panic-stats 5 fields: panic-restamp /
    // provenance-violations / macro-expand-checkpoints /
    // reflect-hygiene-validation / hygiene-dirty-impact) +
    // #458 (query:pattern-hygiene-stats basic count) + #373
    // (mutate hygiene guard — flat.is_macro_introduced internal
    // check) + #750 (query:reflection-schema-stats runtime
    // reflection validate).
    // #757 is the FIRST observability surface that tracks the
    // *fine-grained provenance + dynamic inliner policy +
    // per-macro correlation* specifically — provenance captured
    // at clone_macro_body, inliner policy violation firings,
    // per-macro hygiene violation correlation, query-filter hits
    // — as separate per-decision-point counters the Agent
    // consumes to monitor and tune macro hygiene in self-evo
    // loops.
    //
    //   - macro_hygiene_provenance_captured_total: # of times
    //                                             provenance
    //                                             (macro_def_node_id
    //                                             or sym + gensym
    //                                             history) was
    //                                             successfully
    //                                             populated on
    //                                             a MacroIntroduced
    //                                             node at clone_
    //                                             macro_body
    //                                             success path.
    //                                             Proxy for "how
    //                                             often fine-
    //                                             grained
    //                                             provenance is
    //                                             tracked".
    //   - macro_hygiene_inliner_policy_violations_total: # of
    //                                                 times the
    //                                                 InlinePass
    //                                                 respect_
    //                                                 macro_hygiene_
    //                                                 policy was
    //                                                 violated
    //                                                 (e.g. dynamic
    //                                                 policy via
    //                                                 hygiene:set-
    //                                                 inliner-
    //                                                 respect-
    //                                                 macro!
    //                                                 said #f but
    //                                                 the inliner
    //                                                 still
    //                                                 inlined a
    //                                                 macro-
    //                                                 introduced
    //                                                 call site,
    //                                                 or vice versa).
    //                                                 Proxy for
    //                                                 "how often
    //                                                 dynamic
    //                                                 inliner
    //                                                 policy +
    //                                                 static
    //                                                 respect_
    //                                                 macro_
    //                                                 hygiene_
    //                                                 disagree".
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual ast.ixx FlatAST + provenance_ column or
    // extended marker (macro_def_node_id or sym + gensym history)
    // populated in clone_macro_body success path + QueryExpr
    // :marker MacroIntroduced :provenance macro-name filter
    // support + (query:macro-hygiene-provenance node-id) function
    // primitive + (hygiene:set-inliner-respect-macro! #t/#f
    // [subtree]) primitive + InlinePass respect_macro_hygiene_
    // dynamic check from EDSL/primitive + Guard integration with
    // hygiene_violation_by_macro correlation +
    // tests/test_macro_hygiene_provenance_inliner_policy_ai.cpp
    // harness (define macro with nested, mutate under different
    // policies → assert provenance query accurate, inliner policy
    // respected/tuned, metrics, no silent drift, TSan clean) +
    // SEVA demo with macro-generated verification code + policy
    // tuning demo + docs are all follow-up (each is a dedicated
    // session in ast.ixx + query_matcher + evaluator_primitives_
    // query.cpp + InlinePass + aura_jit.cpp + MutationBoundaryGuard
    // + new test + SEVA demo + docs).
    std::atomic<std::uint64_t> macro_hygiene_provenance_captured_total{0};
    std::atomic<std::uint64_t> macro_hygiene_inliner_policy_violations_total{0};

    // Issue #1644: IR hygiene full-pipeline tracking (refine #1047).
    // Bumped when InlinePass skips a cross-marker inlining because of
    // respect_macro_hygiene_ (ir_macro_introduced_inlined_skipped_total)
    // or when lowering_impl.cpp propagates a non-zero source_marker from
    // the source AST node to an IRInstruction (lowering_marker_propagated_total).
    // This is the per-CompilerMetrics counter surface for both
    // observability and the (query:ir-marker-stats) primitive composition.
    // Distinct from the legacy per-Fiber macro_hygiene_skipped_ at
    // InlinePass namespace scope (process-wide but not per-CompilerMetrics).
    std::atomic<std::uint64_t> ir_macro_introduced_inlined_skipped_total{0};
    std::atomic<std::uint64_t> lowering_marker_propagated_total{0};

    // Issue #758: runtime auto_validate bridge for user-defined
    // EDSL structs (DEFINE_STRUCT / custom nodes) under
    // MutationBoundaryGuard with macro hygiene invariant
    // correlation counters backing the
    // (query:edsl-reflection-stats) primitive. These are
    // public so future reflect.hh + new runtime_reflect_edsl_
    // bridge.cpp + evaluator_primitives_mutate.cpp can call them
    // at each decision point (runtime_validate_edsl_struct call
    // on EDSL-tagged nodes pre-commit / auto_validate pass / fail
    // / hygiene invariants held / MacroIntroduced descendants
    // verified for valid provenance / hygiene invariant correlation
    // / dirty/epoch cascade on violation / mutation-impact-
    // snapshot correlation).
    //
    // Non-duplicative with the existing #750 (query:reflection-
    // schema-stats — 4 fields: validated / hygiene-invariants-
    // held / schema-violations / stale-validation-prevented)
    // which covers general macro body schema validation +
    // (reflect:validate-macro-body node-id) + (reflect:validate-
    // edsl node-id) primitives. #758 is the FIRST observability
    // surface that tracks the *user-defined EDSL struct +
    // macro hygiene invariant correlation* specifically —
    // validated_edsl (per-type EDSL struct pass), hygiene_
    // invariants_held (MacroIntroduced descendants verified
    // for valid provenance + no capture violation + marker
    // consistency), schema_fail_by_type (per-type EDSL struct
    // fail), macro_correlated_violations (hygiene violations
    // correlated to specific macro_def_id) — as separate
    // per-decision-point counters the Agent consumes to monitor
    // extensible EDSL struct production safety in self-evo
    // loops.
    //
    //   - edsl_validated_total: # of EDSL struct auto_validate
    //                           pass firings under Guard commit
    //                           (proxy for "how many user-defined
    //                           EDSL structs were successfully
    //                           validated").
    //   - edsl_hygiene_invariants_held_total: # of times all
    //                                          MacroIntroduced
    //                                          descendants of an
    //                                          EDSL struct had
    //                                          valid provenance /
    //                                          no capture violation
    //                                          / marker consistency
    //                                          (proxy for "how often
    //                                          the hygiene invariant
    //                                          holds under EDSL
    //                                          mutate").
    //   - edsl_schema_fail_by_type_total: # of EDSL struct
    //                                     auto_validate fail
    //                                     firings (broken down by
    //                                     type via the type-aware
    //                                     path; total counter for
    //                                     the primitive).
    //   - edsl_macro_correlated_violations_total: # of hygiene
    //                                            violations
    //                                            correlated to
    //                                            specific
    //                                            macro_def_id
    //                                            (i.e. which macro
    //                                            introduced the
    //                                            problematic
    //                                            descendant that
    //                                            failed the
    //                                            hygiene check).
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual reflect.hh + new runtime_reflect_edsl_bridge.cpp
    // + runtime_validate_edsl_struct(flat, root_id, edsl_type_name)
    // uses reflect_members to walk expected layout + reconstruct
    // temp struct from AST payload/children + call auto_validate +
    // verify MacroIntroduced descendants + MutationBoundaryGuard
    // integration on EDSL-tagged nodes before commit +
    // (reflect:validate-edsl node-id [type]) primitive with
    // optional type arg + tests/test_reflection_edsl_struct_
    // validate_guard_mutate.cpp harness (user EDSL struct define
    // via macro + mutate under Guard → assert validate catches bad
    // schema/hygiene, ok=false, metrics, TSan clean) + SEVA custom
    // EDSL demo + dirty/epoch cascade on violation + mutation-
    // impact-snapshot correlation + docs are all follow-up (each
    // is a dedicated session in reflect.hh + runtime_reflect_edsl_
    // bridge.cpp + evaluator_primitives_mutate.cpp + new test +
    // SEVA demo + docs).
    std::atomic<std::uint64_t> edsl_validated_total{0};
    std::atomic<std::uint64_t> edsl_hygiene_invariants_held_total{0};
    std::atomic<std::uint64_t> edsl_schema_fail_by_type_total{0};
    std::atomic<std::uint64_t> edsl_macro_correlated_violations_total{0};
    // Issue #759: unified 'code-as-data' closed-loop maturity
    // metrics backing the (query:code-as-data-maturity-stats)
    // primitive. These are public so future
    // tests/test_task6_code_as_data_closedloop_harness.cpp +
    // SEVA demo + SLO deployment + clone_macro_body marker
    // propagation sampling wire-up + MutationBoundaryGuard
    // rollback hygiene-safe observation wire-up +
    // runtime_validate_edsl_struct macro/EDSL schema coverage
    // wire-up + Prometheus text/OTLP exporter can call them at
    // each decision point (fidelity sample / drift detected /
    // rollback hygiene preserved / reflect schema coverage on
    // macro-generated or EDSL-mutated subtree / dirty/epoch
    // correlation hit / concurrent fiber stress success).
    //
    // Non-duplicative with the existing #757 (query:macro-hygiene-
    // provenance-stats — 4 fields: provenance-captured /
    // inliner-policy-violations / provenance-violations / hygiene-
    // dirty-impact) which covers macro body hygiene observability,
    // and #758 (query:edsl-reflection-stats — 4 fields: validated-
    // edsl / hygiene-invariants-held / schema-fail-by-type /
    // macro-correlated-violations) which covers EDSL struct +
    // macro hygiene invariant correlation specifically. #759 is
    // the FIRST observability surface that tracks the *code-as-data
    // closed-loop maturity composite* — marker propagation fidelity
    // (drift / samples), Guard rollback hygiene safety (safe /
    // attempts), reflection schema coverage on macro/EDSL
    // subtrees (covered / total), concurrent fiber stress success
    // — as separate per-decision-point counters the Agent
    // consumes to monitor the integrated macro + reflect + EDSL
    // self-evo loop production readiness.
    //
    //   - code_as_data_fidelity_samples_total: total marker
    //                                           propagation
    //                                           fidelity check
    //                                           samples (denominator
    //                                           for fidelity_pct
    //                                           derivation).
    //   - code_as_data_fidelity_drift_total: samples where marker
    //                                        propagation drift was
    //                                        detected (drift /
    //                                        samples = inverse
    //                                        fidelity rate).
    //   - code_as_data_rollback_hygiene_safe_total: Guard rollback
    //                                               events that
    //                                               preserved
    //                                               hygiene
    //                                               invariants (+
    //                                               StableRef
    //                                               validity) — safe
    //                                               / attempts =
    //                                               guard_rollback_
    //                                               hygiene_safe_pct.
    //   - code_as_data_reflect_schema_macro_edsl_total: reflect
    //                                                   schema
    //                                                   validation
    //                                                   calls on
    //                                                   macro-generated
    //                                                   or EDSL-
    //                                                   mutated subtrees
    //                                                   (covered /
    //                                                   total = reflection
    //                                                   schema coverage
    //                                                   on macro/EDSL
    //                                                   ratio).
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual tests/test_task6_code_as_data_closedloop_
    // harness.cpp multi-fiber stress test (random macro expansion
    // deep nesting + EDSL struct mutate under Guard + simulated
    // reflect validate + panic/rollback injection + steal during
    // boundary → assert fidelity metrics stay high, no hygiene
    // drift post-rollback, schema coverage tracks, TSan/ASan
    // clean) + wire marker provenance (from #757) + runtime
    // reflect validate (from #758) + Guard rollback path to feed
    // the maturity stats (auto-update on every successful self-
    // mod boundary) + SLO / (query:code-as-data-slo) with
    // thresholds (e.g. fidelity >99%, coverage >95%, trigger
    // self-heal or alert on breach) + Prometheus text or OTLP
    // deployment exporter + Task6 health score composite + SEVA
    // extension with macro-generated + user-EDSL verification
    // code under load + CI gate on harness passing with fidelity
    // thresholds + docs are all follow-up work (each is a
    // dedicated session in observability_metrics.h +
    // evaluator_primitives_observability.cpp + new test harness
    // + SEVA demo + docs).
    std::atomic<std::uint64_t> code_as_data_fidelity_samples_total{0};
    std::atomic<std::uint64_t> code_as_data_fidelity_drift_total{0};
    std::atomic<std::uint64_t> code_as_data_rollback_hygiene_safe_total{0};
    std::atomic<std::uint64_t> code_as_data_reflect_schema_macro_edsl_total{0};
    // Issue #760: query:pattern performance + hygiene fidelity
    // observability counters backing the
    // (query:pattern-performance-stats) primitive. These are
    // public so future query_matcher.cpp + evaluator_primitives_
    // query.cpp tag_arity_index_ hot-path + ... wildcard trie/
    // DFA + deep hygiene predicate (marker MacroIntroduced :
    // provenance macroX) + children_safe_view / StableNodeRef
    // pinning + MutationBoundaryGuard reader snapshot +
    // tests/test_query_pattern_indexing_hygiene_concurrent.cpp
    // harness (large macro-expanded AST + concurrent fibers +
    // pattern mutate under Guard → assert index used, hygiene
    // respected, no drift, perf win, TSan clean) +
    // (query:pattern-explain node pattern) primitive can call
    // them at each decision point (linear scan / index hit /
    // wildcard match / hygiene filter / auto-rebuild on
    // structural mutate).
    //
    // Non-duplicative with the existing #757 (query:macro-hygiene-
    // provenance-stats — 4 fields: provenance-captured / inliner-
    // policy-violations / provenance-violations / hygiene-dirty-
    // impact) which covers macro body hygiene observability, #758
    // (query:edsl-reflection-stats — 4 fields: validated-edsl /
    // hygiene-invariants-held / schema-fail-by-type / macro-
    // correlated-violations) which covers EDSL struct + macro
    // hygiene invariant correlation, #759 (query:code-as-data-
    // maturity-stats — 4 fields: fidelity-samples / fidelity-drift
    // / guard-rollback-hygiene-safe / reflect-schema-macro-edsl)
    // which covers code-as-data closed-loop maturity composite.
    // #760 is the FIRST observability surface that tracks the
    // *query:pattern performance + hygiene fidelity* specifically
    // — linear scans vs index hits (perf cliff detection),
    // wildcard cost (early exit / DFA benefit), hygiene filtered
    // (deep hygiene predicate activity), avg AST size sampled —
    // as separate per-decision-point counters the Agent consumes
    // to monitor query:pattern production-readiness on large
    // macro-heavy concurrent workspaces.
    //
    //   - pattern_match_linear_scans_total: linear O(N) pattern
    //                                       scans (when fast-path
    //                                       index misses / not
    //                                       built / too few nodes
    //                                       to be worth indexing)
    //                                       — proxy for "how
    //                                       often does pattern
    //                                       match fall back to
    //                                       linear walk" (high
    //                                       value = perf cliff).
    //   - pattern_match_index_hits_total: tag_arity_index_ fast-
    //                                     path hits (O(1)
    //                                     candidate set
    //                                     retrieval via (tag,
    //                                     child_count, marker)
    //                                     hash) — proxy for
    //                                     "how often does the
    //                                     index save a full
    //                                     scan" (high value =
    //                                     perf win).
    //   - pattern_match_wildcard_total: ... wildcard match
    //                                   firings (early-exit /
    //                                   DFA cost on rest-param
    //                                   handling) — proxy for
    //                                   "how often does the
    //                                   wildcard path fire".
    //   - pattern_match_hygiene_filtered_total: macro nodes
    //                                          filtered /
    //                                          skipped by deep
    //                                          hygiene predicate
    //                                          (marker
    //                                          MacroIntroduced
    //                                          :from-macro sym
    //                                          in QueryExpr) —
    //                                          proxy for "how
    //                                          often does the
    //                                          hygiene predicate
    //                                          prevent an
    //                                          over-match".
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual query_matcher.cpp tag_arity_index_ populated
    // on add_node / structural mutate + specialized ... rest-
    // param / wildcard handling with early exit or DFA + QueryExpr
    // / pattern parser :marker MacroIntroduced :from-macro sym
    // extension + matcher auto-apply hygiene filter or provenance
    // stamp when matching under macro context + wire to
    // clone_macro_body name_map + mandate children_safe_view /
    // StableNodeRef pinning in all pattern iterator paths +
    // integrate with MutationBoundaryGuard reader snapshot +
    // (query:pattern-explain node pattern) primitive for debug +
    // tests/test_query_pattern_indexing_hygiene_concurrent.cpp
    // harness + SEVA pattern-heavy verification self-edit + CI
    // perf gate + docs are all follow-up work (each is a
    // dedicated session in query_matcher.cpp + evaluator_primitives_
    // query.cpp + new test + SEVA demo + docs).
    std::atomic<std::uint64_t> pattern_match_linear_scans_total{0};
    std::atomic<std::uint64_t> pattern_match_index_hits_total{0};
    std::atomic<std::uint64_t> pattern_match_wildcard_total{0};
    std::atomic<std::uint64_t> pattern_match_hygiene_filtered_total{0};
    // Issue #789: SafePCVSpan mandate + tag_arity_index_
    // hot-path + deep :marker provenance predicate
    // enforcement observability (refine/consolidate
    // #760 non-duplicative). 2 NEW atomics:
    //   - pattern_safe_span_uses_total: # of children_
    //     safe_view / SafePCVSpan pin calls in the
    //     matcher (Phase 2+ to wire from query_matcher.cpp
    //     + evaluator_primitives_query.cpp pattern
    //     iterator paths). The mandate enforcement
    //     signal — tracks how often the safe-span path
    //     is exercised.
    //   - pattern_dangling_prevented_total: # of times
    //     the generation pin check fired and prevented
    //     a dangling span (Phase 2+ to wire from
    //     ast.ixx children_safe_view). The safety net
    //     signal — tracks how often the safety check
    //     caught a potential UAF.
    std::atomic<std::uint64_t> pattern_safe_span_uses_total{0};
    std::atomic<std::uint64_t> pattern_dangling_prevented_total{0};
    // Issue #761: end-to-end atomic batch mutate primitives +
    // suppressed generation bump observability + cross-fiber
    // safety metrics for reliable multi-step AI iterative edits
    // (non-duplicative with #755 concurrent Guard, #749 StableRef
    // COW, #737 atomic batch proposal). These are public so
    // future evaluator_primitives_mutate.cpp + ast.ixx +
    // (mutate:batch [body]) or begin/commit primitives +
    // per-boundary atomic_batch_bumps_saved_ + cross-fiber steal
    // during suppressed batch re-stamp + tests/test_mutate_batch_
    // atomic_cross_fiber_safety.cpp harness (multi-fiber AI edit
    // script with compound rebind+replace under batch + steal/
    // panic → assert single bump, all-or-nothing, hygiene
    // preserved, metrics accurate, TSan clean) + SEVA compound
    // edit demo + CI gate can call them at each decision point
    // (batch begin / suppressed bump / cross-fiber steal during
    // batch / hygiene violation caught in batch / dirty cascade
    // suppressed / mutation-impact-snapshot batch_impact flag).
    //
    // Non-duplicative with #757 (query:macro-hygiene-provenance-
    // stats), #758 (query:edsl-reflection-stats), #759 (query:
    // code-as-data-maturity-stats), #760 (query:pattern-perfor-
    // mance-stats) which cover macro body hygiene + EDSL struct
    // + macro hygiene invariant correlation + code-as-data
    // closed-loop maturity + query:pattern performance surfaces.
    // #761 is the FIRST observability surface that tracks the
    // *end-to-end atomic batch mutate + suppressed generation
    // bump + cross-fiber safety composite* — batch lifecycle
    // (started / committed / rolled-back), suppressed bump count
    // (churn saved), cross-fiber steals during suppressed batch
    // (re-stamp events), hygiene violations caught within batch
    // boundary — as separate per-decision-point counters the
    // Agent consumes to monitor atomic compound EDSL edit
    // production-readiness.
    //
    //   - mutate_batches_started_total: # of (mutate:batch [body])
    //                                    or begin/commit primitive
    //                                    batch lifecycles entered
    //                                    (proxy for "how many
    //                                    atomic compound AI edits
    //                                    did the Agent kick off").
    //   - mutate_suppressed_bumps_total: # of generation bumps
    //                                    suppressed by the batch
    //                                    guard (proxy for "how
    //                                    much generation churn
    //                                    was saved by batching
    //                                    multi-step edits into a
    //                                    single transaction" — the
    //                                    whole point of suppressed
    //                                    bumps).
    //   - mutate_cross_fiber_steals_during_batch_total: # of
    //                                                   fiber
    //                                                   steals
    //                                                   occurring
    //                                                   while a
    //                                                   batch is
    //                                                   active
    //                                                   (proxy for
    //                                                   "how often
    //                                                   is the
    //                                                   suppressed
    //                                                   batch
    //                                                   boundary
    //                                                   crossed by
    //                                                   fiber
    //                                                   yield/
    //                                                   restore"
    //                                                   — triggers
    //                                                   version
    //                                                   re-stamp).
    //   - mutate_hygiene_violations_in_batch_total: # of hygiene
    //                                              guard
    //                                              violations
    //                                              caught within
    //                                              a batch
    //                                              boundary
    //                                              (proxy for
    //                                              "how many
    //                                              compound
    //                                              edits
    //                                              contained a
    //                                              hygiene
    //                                              issue that
    //                                              the batch
    //                                              rollback
    //                                              caught").
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual (mutate:batch [body]) or begin/commit primitives
    // in evaluator_primitives_mutate.cpp + per-boundary atomic_
    // batch_bumps_saved_ via active_mutation_stack or depth +
    // cross-fiber steal during suppressed batch re-stamp +
    // checkpoint_yield_boundary integration + unified mark_dirty_
    // upward for all touched + defuse_version_ bump once + feed
    // mutation-impact-snapshot with batch_impact flag + tests/
    // test_mutate_batch_atomic_cross_fiber_safety.cpp harness
    // (multi-fiber AI edit script with compound rebind+replace
    // under batch + steal/panic → assert single bump, all-or-
    // nothing, hygiene preserved, metrics accurate, TSan clean) +
    // SEVA compound edit demo + metrics correlation link to
    // existing hygiene-stats + stable-ref invalidations +
    // defuse_version_ + CI gate + docs are all follow-up work
    // (each is a dedicated session in evaluator_primitives_
    // mutate.cpp + ast.ixx + new test + SEVA demo + docs).
    std::atomic<std::uint64_t> mutate_batches_started_total{0};
    std::atomic<std::uint64_t> mutate_suppressed_bumps_total{0};
    std::atomic<std::uint64_t> mutate_cross_fiber_steals_during_batch_total{0};
    std::atomic<std::uint64_t> mutate_hygiene_violations_in_batch_total{0};
    // Issue #762: Workspace '锁定-导航-修改-执行' closed-loop
    // reliability observability under concurrent fiber orchestration
    // + multi-Agent parallel edits (non-duplicative with #749
    // StableRef COW/pinning, #755 Guard concurrent stress, #754
    // LLM-bottleneck adaptive scheduling). These are public so
    // future evaluator_primitives_query.cpp + mutate.cpp + workspace
    // paths + restore_post_yield + steal paths + tests/test_
    // workspace_closedloop_fiber_multiagent_orchestration.cpp
    // harness (10+ fibers/agents with parallel query/mutate on
    // shared+COW workspaces + steal/yield → assert auto refresh,
    // dirty consistent, no contention deadlock, metrics accurate,
    // TSan clean) + SEVA multi-Agent demo + CI gate can call them
    // at each decision point (concurrent query/mutate success,
    // cross-COW StableRef validity, yield point hit, shared_mutex
    // contention event, multi-Agent edit fidelity).
    //
    // Non-duplicative with #757 (query:macro-hygiene-provenance-
    // stats), #758 (query:edsl-reflection-stats), #759 (query:
    // code-as-data-maturity-stats), #760 (query:pattern-perfor-
    // mance-stats), #761 (query:mutate-batch-stats) which cover
    // macro body hygiene + EDSL struct + macro hygiene invariant
    // correlation + code-as-data closed-loop maturity + query:
    // pattern performance + end-to-end atomic batch mutate
    // surfaces. #762 is the FIRST observability surface that
    // tracks the *Workspace closed-loop orchestration* — concurrent
    // query/mutate success under fiber steal, cross-COW StableRef
    // validity (auto-propagation win rate), yield point hit count
    // (exhaustive yield coverage), shared_mutex contention events
    // (orchestration bottleneck detection) — as separate per-
    // decision-point counters the Agent consumes to monitor
    // Workspace closed-loop production-readiness in orchestrated
    // multi-Agent deployments.
    //
    //   - workspace_closedloop_concurrent_query_mutate_total: #
    //                                                      of
    //                                                      concurrent
    //                                                      query+mutate
    //                                                      success
    //                                                      events
    //                                                      on
    //                                                      shared
    //                                                      / COW
    //                                                      workspaces
    //                                                      under
    //                                                      fiber
    //                                                      steal
    //                                                      (proxy
    //                                                      for
    //                                                      concurrent
    //                                                      orchestration
    //                                                      health).
    //   - workspace_closedloop_cross_cow_ref_valid_total: # of
    //                                                   cross-COW
    //                                                   StableRef
    //                                                   accesses
    //                                                   that
    //                                                   remained
    //                                                   valid
    //                                                   after
    //                                                   auto-
    //                                                   propagation
    //                                                   (denominator
    //                                                   for cross_cow_ref_validity_pct
    //                                                   derivation
    //                                                   — valid /
    //                                                   total =
    //                                                   cross-cow
    //                                                   ref
    //                                                   validity
    //                                                   rate).
    //   - workspace_closedloop_yield_points_hit_total: # of
    //                                                explicit
    //                                                yield point
    //                                                hits in
    //                                                matcher /
    //                                                children
    //                                                iteration /
    //                                                mark_dirty
    //                                                paths (proxy
    //                                                for "how
    //                                                often does
    //                                                the Workspace
    //                                                loop yield
    //                                                to allow
    //                                                other fibers
    //                                                / agents
    //                                                to run" —
    //                                                low value =
    //                                                starvation
    //                                                risk).
    //   - workspace_closedloop_shared_mutex_contention_total: # of
    //                                                      shared_
    //                                                      mutex
    //                                                      contention
    //                                                      events
    //                                                      on
    //                                                      workspace
    //                                                      primitives
    //                                                      under
    //                                                      heavy
    //                                                      AI load
    //                                                      (proxy
    //                                                      for
    //                                                      orchestration
    //                                                      bottleneck
    //                                                      — high
    //                                                      value =
    //                                                      contention
    //                                                      signal).
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual evaluator_primitives_query.cpp + mutate.cpp +
    // workspace paths instrumentation with explicit fiber yield
    // points or safepoint checks in pattern matcher + children_
    // safe iteration + mark_dirty_upward + auto-propagate active
    // StableRef pins or dirty bits via epoch or weak registry on
    // workspace COW/clone in primitives + extend make_ref / get_
    // safe in query/mutate to auto-refresh on workspace boundary
    // cross + wire mark_dirty_upward to notify pinned refs or
    // sub-workspace listeners + integration with mutation-impact
    // + stable-ref-stats + force StableRef validation + dirty
    // re-propagation for active Workspace edits in restore_post_
    // yield + steal paths + tests/test_workspace_closedloop_fiber_
    // multiagent_orchestration.cpp harness + SEVA multi-Agent
    // demo + Prometheus / SLO (closedloop_fidelity >99.5%) + CI
    // gate + docs are all follow-up work (each is a dedicated
    // session in evaluator_primitives_query.cpp + mutate.cpp +
    // ast.ixx + new test + SEVA demo + docs).
    std::atomic<std::uint64_t> workspace_closedloop_concurrent_query_mutate_total{0};
    std::atomic<std::uint64_t> workspace_closedloop_cross_cow_ref_valid_total{0};
    std::atomic<std::uint64_t> workspace_closedloop_yield_points_hit_total{0};
    std::atomic<std::uint64_t> workspace_closedloop_shared_mutex_contention_total{0};
    // Issue #791: exhaustive fiber yield-point instrumentation
    // + automatic StableRef/dirty cross-boundary propagation
    // observability (Refine/Consolidate #773/#762 non-duplicative).
    // 3 NEW atomics for the
    // (query:workspace-closedloop-fiber-multi-agent-yield-stats,
    // schema 791) primitive:
    //   - workspace_closedloop_autoprop_refs_total: # of
    //     StableRefs auto-propagated/snapshotted across
    //     workspace COW/clone/split boundaries (Phase 2+ to
    //     wire from workspace tree + is_valid_in / WeakRef
    //     registry paths).
    //   - workspace_closedloop_autoprop_dirty_total: # of
    //     dirty bits auto-propagated on workspace COW/clone/split
    //     boundaries (Phase 2+ to wire from mark_dirty_upward
    //     cross-boundary notification path).
    //   - workspace_closedloop_missed_yield_total: # of times
    //     a long walk (pattern matcher / children_safe iteration
    //     / mark_dirty_upward on verification subtrees) missed
    //     a yield point (the negative signal — high value =
    //     yield starvation under concurrent fiber load).
    std::atomic<std::uint64_t> workspace_closedloop_autoprop_refs_total{0};
    std::atomic<std::uint64_t> workspace_closedloop_autoprop_dirty_total{0};
    std::atomic<std::uint64_t> workspace_closedloop_missed_yield_total{0};
    // Issue #792: compiler invalidate_function +
    // mutation_epoch_ synchronization with outermost
    // MutationBoundaryGuard depth + live IRClosure /
    // EnvFrame / GuardShape version refresh under
    // concurrent fiber steal (Non-duplicative
    // refinement of #783/#755/#784/#787). 4 NEW
    // atomics for the
    // (query:compiler-invalidate-guard-steal-stats,
    // schema 792) primitive:
    //   - compiler_invalidate_deferred_total: # of
    //     invalidate_function calls deferred when
    //     active MutationBoundaryGuard depth > 0
    //     (Phase 2+ to wire from service.ixx
    //     invalidate_function when depth > 0)
    //   - compiler_version_refresh_hits_total: # of
    //     bridge_epoch / EnvFrame version_ re-stamp
    //     hits on steal resume / restore_post_yield_
    //     or_rollback (Phase 2+ to wire from
    //     evaluator_fiber_mutation.cpp +
    //     apply_closure / materialize_call_env)
    //   - compiler_guardshape_deopt_on_steal_total: #
    //     of GuardShape deopts triggered on steal
    //     when bridge_epoch mismatch detected
    //     (Phase 2+ to wire from aura_jit_bridge.cpp
    //     + JIT hot-swap paths)
    //   - compiler_live_closure_stale_prevented_total:
    //     # of live IRClosure stale references
    //     prevented via closure_bridge_ refresh
    //     (Phase 2+ to wire from apply_closure
    //     dual-path + bridge_epoch check)
    std::atomic<std::uint64_t> compiler_invalidate_deferred_total{0};
    std::atomic<std::uint64_t> compiler_version_refresh_hits_total{0};
    std::atomic<std::uint64_t> compiler_guardshape_deopt_on_steal_total{0};
    std::atomic<std::uint64_t> compiler_live_closure_stale_prevented_total{0};
    // Issue #793: JIT/AOT hot-swap + GuardShape + linear +
    // EnvFrame version_ consistency observability
    // (Non-duplicative consolidation/refinement of
    // #785/#787/#755). 4 NEW atomics for the
    // (query:jit-aot-hotswap-fidelity-stats,
    // schema 793) primitive:
    //   - jit_deopt_forced_on_reload_total: # of
    //     GuardShape deopts forced on AOT reload /
    //     refcount swap (Phase 2+ to wire from
    //     aura_jit.cpp + aura_jit_bridge.cpp
    //     hot-swap path per body "On successful
    //     refcount swap or region reload, if any
    //     active fiber holds MutationBoundary or
    //     has live GuardShape/Apply on affected
    //     func, force deopt (set generic_block) or
    //     bump shape_id / linear_state for affected
    //     IR")
    //   - jit_linear_violation_prevented_total: # of
    //     linear ownership violations prevented via
    //     JIT runtime version check / MoveOp
    //     invalidation (Phase 2+ to wire from
    //     aura_jit.cpp JIT codegen for Linear* per
    //     body "Emit additional runtime checks
    //     (version_ probe or bridge_epoch compare)
    //     before deopt decision or MoveOp")
    //   - jit_env_version_sync_hits_total: # of
    //     EnvFrame::version_ sync hits triggered
    //     on JIT-executed closure steal resume /
    //     post-rollback (Phase 2+ to wire from
    //     evaluator_fiber_mutation.cpp + apply_
    //     closure per body "On steal resume /
    //     post-rollback, for JIT-executed
    //     closures, trigger GuardShape re-evaluation
    //     or linear re-wrap if version_ or epoch
    //     drifted")
    //   - jit_guardshape_stale_reject_total: # of
    //     JIT GuardShape stale rejections caught
    //     when expected_shape / shape_id mismatch
    //     detected at apply_closure time (Phase 2+
    //     to wire from ir_executor.ixx +
    //     evaluator.ixx apply_closure bridge_epoch
    //     check per body "IRInterpreter handling
    //     of GuardShape/linear + apply_closure
    //     (bridge_epoch check)")
    std::atomic<std::uint64_t> jit_deopt_forced_on_reload_total{0};
    std::atomic<std::uint64_t> jit_linear_violation_prevented_total{0};
    std::atomic<std::uint64_t> jit_env_version_sync_hits_total{0};
    std::atomic<std::uint64_t> jit_guardshape_stale_reject_total{0};
    // Issue #794: full compiler + EDSL closed-loop
    // fidelity observability (Non-duplicative to
    // #786/#787/#755/#792/#793). 4 NEW atomics
    // for the
    // (query:full-closedloop-compiler-edsl-fidelity-
    // stats, schema 794) primitive:
    //   - cross_layer_guardshape_deopt_hits_total:
    //     # of times the full closed-loop harness
    //     detected GuardShape expected vs runtime
    //     shape mismatch (Phase 2+ to wire from
    //     tests/test_full_compiler_edsl_closedloop_
    //     fidelity.cpp + integrated fidelity
    //     assertion path per body "Wire new
    //     composite ... pulling from ... new
    //     counters (guardshape_deopt_hits, ...)")
    //   - cross_layer_linear_enforce_success_total:
    //     # of times linear_ownership_state was
    //     respected across compiler + EDSL
    //     boundary (Phase 2+ to wire from the
    //     harness's linear integrity assertion
    //     path)
    //   - cross_layer_epoch_sync_total: # of
    //     times EnvFrame version_ + bridge_epoch
    //     were synchronized across layers
    //     (Phase 2+ to wire from the harness's
    //     epoch consistency assertion path)
    //   - cross_layer_drift_detections_total: # of
    //     times the harness detected any
    //     cross-layer drift (the negative signal —
    //     high value = drift detected, SLO breach)
    std::atomic<std::uint64_t> cross_layer_guardshape_deopt_hits_total{0};
    std::atomic<std::uint64_t> cross_layer_linear_enforce_success_total{0};
    std::atomic<std::uint64_t> cross_layer_epoch_sync_total{0};
    std::atomic<std::uint64_t> cross_layer_drift_detections_total{0};
    // Issue #796: end-to-end IR SoA full migration +
    // DirtyAware short-circuit + DepGraph integration
    // observability (Non-duplicative extension of
    // #766/#741). 4 NEW atomics for the
    // (query:ir-soa-full-migration-stats, schema
    // 796) primitive:
    //   - ir_soa_instructions_emitted_total: # of
    //     instructions emitted to IRFunctionSoA
    //     (vs remaining AoS IRModule paths) —
    //     bumped from lowering_impl.cpp + JIT emit
    //     sites per body "Complete port of
    //     LoweringState emit, ir_executor
    //     traversal, JIT emitter to prefer
    //     IRFunctionSoA + IRInstructionView"
    //   - ir_soa_dirty_block_skips_total: # of
    //     blocks skipped via DirtyAwarePass +
    //     run_incremental_dirty_pipeline short-
    //     circuit (clean blocks not re-lowered /
    //     not re-JITted) — bumped from
    //     service.ixx invalidate_function +
    //     lowering/JIT path per body "Enforce
    //     DirtyAwarePass +
    //     run_incremental_dirty_pipeline in
    //     invalidate_function + JIT recompile;
    //     consult is_block_dirty /
    //     is_instruction_dirty + #741 impact_scope
    //     for hybrid targeting; short-circuit
    //     clean/impact-free blocks"
    //   - ir_soa_jit_soa_time_ns_total: total ns
    //     spent in JIT SoA emit path (time-based —
    //     high value = JIT SoA path actively
    //     exercised; vs AoS path) — bumped from
    //     aura_jit.cpp SoA emit path per body
    //     "Replace hot AoS walks with SoA column
    //     views"
    //   - ir_soa_impact_dirty_hybrid_skips_total:
    //     # of skips via hybrid impact_scope +
    //     is_block_dirty targeting (the combined
    //     #741 + #766 short-circuit count) —
    //     bumped from service.ixx invalidate_function
    //     when both DepGraph impact_scope + SoA
    //     block dirty are consulted together for
    //     targeting per body "consult ... #741
    //     impact_scope for hybrid targeting"
    // Issue #796: ONLY NEW atomic for the
    // (query:ir-soa-full-migration-stats, schema
    // 796) primitive. The 5 referenced atomics
    // (instructions_emitted / dirty_block_skips /
    // clean_block_hit_rate_pct / pmr_column_
    // utilization_pct / jit_codegen_time_ns) already
    // exist from prior issue work at lines 784-788 —
    // #796 reuses them via the primitive body.
    // impact_dirty_hybrid_skips_total is new and
    // bumped via the new
    // bump_ir_soa_impact_dirty_hybrid_skip helper.
    std::atomic<std::uint64_t> ir_soa_impact_dirty_hybrid_skips_total{0};
    // Issue #763: runtime linear_ownership_state enforcement +
    // GC root registration for IRClosure/EnvFrame in
    // invalidate_function and live-closure paths (non-duplicative
    // with #747 linear occurrence, #741 DepGraph/bridge, #756
    // EnvFrame dual-path, #749 StableRef). These are public so
    // future service.ixx invalidate_function + LoweringState
    // walk of live IRClosure for linear_ownership_state nodes +
    // register affected EnvId/IRClosure as GC root with version_
    // stamp synced to mutation_epoch_ + evaluator_gc.cpp +
    // gc_coordinator compiler IRClosure/EnvFrame root
    // registration hook (called from invalidate + fiber mutation
    // boundary) + ir_executor.ixx + aura_jit.cpp Apply/
    // MakeClosure runtime assert/check for linear_ownership_state
    // consistency + invalidate impact root re-registration +
    // tests/test_prompt6_linear_ownership_gc_root_invalidate_
    // closure.cpp harness (linear define with move/borrow +
    // quote/lambda capture + mutate:rebind on body → invalidate +
    // live closure apply under GC pressure → assert no violation/
    // UAF, roots registered, metrics, TSan/ASan clean) + SEVA
    // linear-ownership demo + CI gate can call them at each
    // decision point (root registration / stale root hit /
    // linear violation prevented / Env version resync on
    // invalidate).
    //
    // Non-duplicative with #757 (query:macro-hygiene-provenance-
    // stats), #758 (query:edsl-reflection-stats), #759 (query:
    // code-as-data-maturity-stats), #760 (query:pattern-perfor-
    // mance-stats), #761 (query:mutate-batch-stats), #762 (query:
    // workspace-closedloop-orchestration-stats) which cover macro
    // body hygiene + EDSL struct + macro hygiene invariant
    // correlation + code-as-data closed-loop maturity + query:
    // pattern performance + end-to-end atomic batch mutate +
    // Workspace closed-loop orchestration surfaces. The existing
    // (query:linear-ownership-gc-stats) covers the GC layer
    // observability. #763 is the FIRST observability surface that
    // tracks the *compiler IRClosure + EnvFrame + invalidate
    // runtime linear enforcement composite* — IRClosure/EnvFrame
    // root registrations (proxy for how often the invalidate +
    // mutation boundary path correctly registers compiler-owned
    // closures as GC roots), stale GC root hits on invalidate
    // (proxy for "how often does the GC walk encounter a root
    // from a previously invalidated function"), runtime linear
    // violations caught (proxy for "how often does the runtime
    // check prevent a use-after-move / linear violation in
    // bridged closures"), Env version re-syncs on invalidate
    // (proxy for "how often does the invalidate path correctly
    // bump version_ on bridged EnvFrames to keep GC walk safe")
    // — as separate per-decision-point counters the Agent
    // consumes to monitor linear ownership + GC + compiler
    // maturation production-readiness under AI multi-round
    // mutate + incremental invalidate.
    //
    //   - linear_ownership_gc_root_registrations_total: # of
    //                                                 compiler
    //                                                 IRClosure /
    //                                                 EnvFrame
    //                                                 root
    //                                                 registrations
    //                                                 called
    //                                                 from
    //                                                 invalidate
    //                                                 + fiber
    //                                                 mutation
    //                                                 boundary
    //                                                 (proxy for
    //                                                 invalidate
    //                                                 + boundary
    //                                                 GC-safety
    //                                                 coverage).
    //   - linear_ownership_gc_root_stale_hits_total: # of
    //                                               stale GC
    //                                               root hits
    //                                               during GC
    //                                               walk from
    //                                               previously
    //                                               invalidated
    //                                               functions
    //                                               (proxy for
    //                                               "how often
    //                                               does the
    //                                               GC walk
    //                                               encounter a
    //                                               root whose
    //                                               origin
    //                                               function
    //                                               has been
    //                                               invalidated
    //                                               without
    //                                               proper
    //                                               re-stamp"
    //                                               — high
    //                                               value =
    //                                               UAF risk
    //                                               signal).
    //   - linear_ownership_gc_violations_prevented_total: # of
    //                                                   runtime
    //                                                   linear
    //                                                   violations
    //                                                   caught
    //                                                   by the
    //                                                   runtime
    //                                                   check in
    //                                                   Apply /
    //                                                   MakeClosure
    //                                                   paths
    //                                                   (proxy for
    //                                                   "how many
    //                                                   use-after-
    //                                                   move /
    //                                                   linear
    //                                                   violations
    //                                                   did the
    //                                                   runtime
    //                                                   guard
    //                                                   prevent in
    //                                                   bridged
    //                                                   closures"
    //                                                   — high
    //                                                   value =
    //                                                   safety net
    //                                                   firings).
    //   - linear_ownership_gc_env_version_resync_total: # of
    //                                                Env version
    //                                                re-syncs
    //                                                on
    //                                                invalidate
    //                                                (proxy for
    //                                                "how often
    //                                                does the
    //                                                invalidate
    //                                                path
    //                                                correctly
    //                                                bump
    //                                                version_ on
    //                                                bridged
    //                                                EnvFrames
    //                                                to keep GC
    //                                                walk safe").
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual service.ixx invalidate_function + LoweringState
    // walk of live IRClosure (via closure_bridge_ or closures_
    // map) for linear_ownership_state nodes + force re-emit or
    // mark for runtime check + register affected EnvId/IRClosure
    // as GC root with version_ stamp synced to mutation_epoch_ +
    // evaluator_gc.cpp + gc_coordinator compiler IRClosure /
    // EnvFrame root registration hook (called from invalidate +
    // fiber mutation boundary) + on GC walk enforce linear state
    // via runtime check (debug) or DropOp simulation for owned
    // values in bridged closures + ir_executor.ixx + aura_jit.cpp
    // Apply/MakeClosure paths and linear ops runtime assert/check
    // for linear_ownership_state consistency with actual
    // ownership + on invalidate impact trigger root re-
    // registration + integration with EscapeAnalysisWrap +
    // DirtyAware for targeted linear dirty + sync with bridge_
    // epoch bump + tests/test_prompt6_linear_ownership_gc_root_
    // invalidate_closure.cpp harness (linear define with move/
    // borrow + quote/lambda capture + mutate:rebind on body →
    // invalidate + live closure apply under GC pressure → assert
    // no violation/UAF, roots registered, metrics, TSan/ASan
    // clean) + SEVA linear-ownership demo + CI gate + docs are
    // all follow-up work (each is a dedicated session in
    // service.ixx + evaluator_gc.cpp + ir_executor.ixx +
    // aura_jit.cpp + new test + SEVA demo + docs).
    std::atomic<std::uint64_t> linear_ownership_gc_root_registrations_total{0};
    std::atomic<std::uint64_t> linear_ownership_gc_root_stale_hits_total{0};
    // Issue #1734: collect_compiler_managed_gc_roots saw snapshot
    // bridge_epoch != live current_bridge_epoch().
    std::atomic<std::uint64_t> gc_roots_bridge_epoch_drift_total{0};
    // Issue #1755: validate_linear_ownership_state saw bridge_epoch
    // (closure stamp) != current_bridge_epoch (caller snapshot / live).
    std::atomic<std::uint64_t> linear_validate_bridge_epoch_drift_total{0};
    std::atomic<std::uint64_t> linear_ownership_gc_violations_prevented_total{0};
    std::atomic<std::uint64_t> linear_ownership_gc_env_version_resync_total{0};
    // Issue #1543: GC root registration consistency audit — one bump per
    // run_linear_gc_root_audit() across the 6 mutation touchpoints
    // (typed_mutate, invalidate_function, compact_env_frames, JIT hot-swap,
    // fiber steal, GC safepoint). See docs/design/linear-gc-roots.md.
    std::atomic<std::uint64_t> linear_gc_root_audit_checks_total{0};
    // Issue #1568: unified boundary consistency closed-loop
    std::atomic<std::uint64_t> linear_boundary_consistency_total{0};
    std::atomic<std::uint64_t> linear_epoch_fence_enforce_total{0};
    std::atomic<std::uint64_t> linear_force_drop_total{0};
    std::atomic<std::uint64_t> linear_violation_audit_total{0};
    // Issue #1545: live-closure linear capture scans (invalidate / compact /
    // JIT ResourceTracker pre-evict). One bump per
    // scan_live_closures_for_linear_captures invocation.
    // Issue #1733: walk_active_closures callback threw; walk continued.
    std::atomic<std::uint64_t> walk_active_closures_callback_exceptions{0};
    std::atomic<std::uint64_t> linear_live_closure_scans_total{0};
    // Closures marked invalid (bridge_epoch=0) because they captured
    // linear-tracked bindings during a scan with mark_invalid=true.
    std::atomic<std::uint64_t> linear_live_closures_marked_invalid_total{0};
    // Issue #1478: linear post-mutate enforcement counters (Issue #1478
    // AC #4). Distinct from the *_gc_* counters above (those track
    // GC-root registration lifecycle; these track per-closure-call
    // post-mutate enforcement).
    //   - linear_post_mutate_enforcements: # of post-mutate checks
    //     performed at apply_closure entry on captured closure envs
    //     (extends closure_needs_safe_fallback from #1475 epoch check).
    //   - linear_ownership_violation_prevented: # of times a captured
    //     linear value was found in Moved/invalid state at
    //     apply_closure entry, intercepted via safe-fallback path.
    // MVP: counters are plumbed and incremented by the helper. Actual
    // linear-cell scan (which captures the per-cell ownership state and
    // detects Moved vs Owned/Borrowed) requires runtime linear cell
    // tagging infrastructure deferred to #1543. This MVP ships the
    // counter + helper + wiring so the enforcement point is observable
    // end-to-end.
    std::atomic<std::uint64_t> linear_post_mutate_enforcements{0};
    // Issue #1731: linear_post_mutate_enforce(NULL_ENV_ID) is a no-op
    // (no captures); count so materialize/TCO/JIT can audit bypass.
    std::atomic<std::uint64_t> linear_post_mutate_null_env_id_total{0};
    // Issue #1538: combined post-mutation linear pipeline (invariant + enforce).
    std::atomic<std::uint64_t> linear_post_mutate_pipeline_total{0};
    std::atomic<std::uint64_t> linear_post_mutate_pipeline_unsafe_total{0};
    // Issue #1540: JIT hot-path linear_post_mutate_enforce probes.
    std::atomic<std::uint64_t> jit_linear_post_mutate_enforcements_total{0};
    std::atomic<std::uint64_t> jit_linear_post_mutate_violations_total{0};
    std::atomic<std::uint64_t> linear_ownership_violation_prevented{0};
    // Issue #764: Arena AST / shared_ptr<FlatAST> lifetime safety
    // vs GC-managed Env/Closure in closure_bridge_ under
    // incremental re-lower + mutation (non-duplicative with #741
    // DepGraph/bridge/Env version, #731 Arena concurrent compact,
    // #749 StableRef COW, #756 EnvFrame dual). These are public
    // so future service.ixx invalidate_function + LoweringState
    // on re-lower impact for affected closure_bridge entries
    // retain/refresh shared_ptr<FlatAST> snapshot before Arena
    // reset + bump bridge_epoch and notify GC to root the old
    // AST temporarily if live closures reference it +
    // evaluator_gc.cpp + gc_coordinator explicit root
    // registration for active IRClosure shared_ptr<FlatAST> (via
    // closure_bridge_ walk or live-closure list); on GC
    // safepoint/compact, validate Arena liveness or pin AST
    // nodes + lowering_impl.cpp set_closure_bridge_ptr +
    // apply_closure capture Arena epoch or generation; on apply,
    // verify AST nodes still valid (via marker or size check) or
    // fallback safely + tests/test_prompt6_arena_ast_sharedptr_
    // closure_bridge_gc_lifetime.cpp harness (quote/lambda define
    // + heavy mutate:rebind + Arena reset + GC compact/steal +
    // live closure apply → assert AST valid or safe fallback, no
    // UAF/leak, roots correct, TSan/ASan clean) + SEVA arena/
    // closure bridge demo + CI gate can call them at each
    // decision point (arena AST root hits / bridge shared_ptr
    // pinned / cross-lifetime violations prevented / invalidate
    // AST refresh).
    //
    // Non-duplicative with #757 (query:macro-hygiene-provenance-
    // stats), #758 (query:edsl-reflection-stats), #759 (query:
    // code-as-data-maturity-stats), #760 (query:pattern-perfor-
    // mance-stats), #761 (query:mutate-batch-stats), #762 (query:
    // workspace-closedloop-orchestration-stats), #763 (query:
    // linear-ownership-gc-compiler-stats) which cover macro body
    // hygiene + EDSL struct + macro hygiene invariant correlation
    // + code-as-data closed-loop maturity + query:pattern
    // performance + end-to-end atomic batch mutate + Workspace
    // closed-loop orchestration + linear ownership + GC + compiler
    // maturation surfaces. #764 is the FIRST observability surface
    // that tracks the *compiler Arena AST / shared_ptr<FlatAST>
    // lifetime vs GC-managed Env/Closure in closure_bridge_*
    // composite — arena AST root hits (GC walk find live AST
    // roots), bridge shared_ptr pinned (Arena reset protection),
    // cross-lifetime violations prevented (apply-time AST validity
    // check fallback), invalidate AST refresh count (re-lower
    // snapshot before reset) — as separate per-decision-point
    // counters the Agent consumes to monitor cross-lifetime
    // production safety in incremental AI mutation flows.
    //
    //   - compiler_arena_closure_lifetime_root_hits_total: # of
    //                                                  arena AST
    //                                                  root hits
    //                                                  during GC
    //                                                  walk via
    //                                                  closure_
    //                                                  bridge_ /
    //                                                  live-
    //                                                  closure
    //                                                  list (proxy
    //                                                  for "how
    //                                                  many live
    //                                                  AST roots
    //                                                  are correctly
    //                                                  registered
    //                                                  against the
    //                                                  GC" — high
    //                                                  value =
    //                                                  UAF/leak
    //                                                  prevention
    //                                                  signal).
    //   - compiler_arena_closure_lifetime_bridge_sharedptr_pinned_total:
    //                                                # of bridge
    //                                                shared_ptr
    //                                                <FlatAST>
    //                                                pinned
    //                                                before
    //                                                Arena reset
    //                                                (proxy for
    //                                                "how often
    //                                                does the
    //                                                invalidate
    //                                                path
    //                                                correctly
    //                                                retain the
    //                                                old AST
    //                                                snapshot to
    //                                                keep live
    //                                                closures
    //                                                valid"
    //                                                — high
    //                                                value =
    //                                                cross-lifetime
    //                                                safety
    //                                                signal).
    //   - compiler_arena_closure_lifetime_cross_violations_prevented_total:
    //                                                   # of
    //                                                   cross-
    //                                                   lifetime
    //                                                   violations
    //                                                   prevented
    //                                                   at apply-
    //                                                   time via
    //                                                   AST
    //                                                   validity
    //                                                   check (via
    //                                                   marker or
    //                                                   size check)
    //                                                   or safe
    //                                                   fallback
    //                                                   (proxy for
    //                                                   "how many
    //                                                   use-after-
    //                                                   Arena-reset
    //                                                   violations
    //                                                   did the
    //                                                   runtime
    //                                                   guard
    //                                                   prevent in
    //                                                   bridge
    //                                                   closure
    //                                                   apply"
    //                                                   — high
    //                                                   value =
    //                                                   safety net
    //                                                   firings).
    //   - compiler_arena_closure_lifetime_invalidate_ast_refresh_total:
    //                                                # of
    //                                                invalidate
    //                                                AST
    //                                                refresh
    //                                                snapshots
    //                                                taken before
    //                                                Arena reset
    //                                                (proxy for
    //                                                "how often
    //                                                does the
    //                                                invalidate
    //                                                path correctly
    //                                                refresh the
    //                                                bridge AST
    //                                                snapshot"
    //                                                — paired
    //                                                with sharedptr_
    //                                                pinned above).
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual service.ixx invalidate_function + LoweringState
    // on re-lower impact for affected closure_bridge entries
    // retain/refresh shared_ptr<FlatAST> snapshot before Arena
    // reset + bump bridge_epoch + notify GC to root the old AST
    // temporarily if live closures reference it + evaluator_gc
    // .cpp + gc_coordinator explicit root registration for
    // active IRClosure shared_ptr<FlatAST> + on GC safepoint/
    // compact validate Arena liveness or pin AST nodes +
    // lowering_impl.cpp set_closure_bridge_ptr + apply_closure
    // capture Arena epoch or generation + on apply verify AST
    // nodes still valid (via marker or size check) or fallback
    // safely + wire to MutationBoundaryGuard for cross-request
    // safety + tests/test_prompt6_arena_ast_sharedptr_closure_
    // bridge_gc_lifetime.cpp harness + SEVA arena/closure bridge
    // demo + sync with bridge_epoch + mutation_epoch_ + Env
    // version_ + extend EscapeAnalysis for AST node escape in
    // bridge + CI gate + docs are all follow-up work (each is a
    // dedicated session in service.ixx + evaluator_gc.cpp +
    // lowering_impl.cpp + new test + SEVA demo + docs).
    std::atomic<std::uint64_t> compiler_arena_closure_lifetime_root_hits_total{0};
    std::atomic<std::uint64_t> compiler_arena_closure_lifetime_bridge_sharedptr_pinned_total{0};
    std::atomic<std::uint64_t> compiler_arena_closure_lifetime_cross_violations_prevented_total{0};
    std::atomic<std::uint64_t> compiler_arena_closure_lifetime_invalidate_ast_refresh_total{0};
    // Issue #799: DeadCoercionElimination + narrow_evidence elision observability
    // (P0 zero-overhead typed mutation; refines #796/#795/#794; non-duplicative
    // with #687 query:dead-coercion-elim-stats and #629 coercion-zerooverhead).
    //   - dead_coercion_elision_elided_casts_total: CastOps statically elided
    //     by DeadCoercionEliminationPass (compile-time savings axis).
    //   - dead_coercion_elision_evidence_hits_total: Rule 6 narrow_evidence
    //     elisions + lowering/type-check evidence hits.
    //   - dead_coercion_elision_narrowing_stable_paths_total: stable narrowed
    //     paths (DCE Rule 6 + TypeSpec narrow_evidence_skipped).
    //   - dead_coercion_elision_runtime_check_savings_total: avoided runtime
    //     CastOp checks (compile elim + IR-interpreter fast-path).
    std::atomic<std::uint64_t> dead_coercion_elision_elided_casts_total{0};
    std::atomic<std::uint64_t> dead_coercion_elision_evidence_hits_total{0};
    std::atomic<std::uint64_t> dead_coercion_elision_narrowing_stable_paths_total{0};
    std::atomic<std::uint64_t> dead_coercion_elision_runtime_check_savings_total{0};
    // Issue #765: Full DepEntry quote/lambda tracking + impact_scope
    // propagation to bridge_epoch bump, EnvFrame version re-stamp
    // and linear state refresh in LoweringState/invalidate
    // (refine/extend #741, non-duplicative). These are public so
    // future ir_cache_pure.ixx compute_dependencies + compute_
    // impact_scope + service dep_graph_ extend DepEntry to flag
    // quote/lambda-introduced nodes (via source_marker or Define
    // with quote); in impact_scope, prioritize or specially mark
    // blocks with closure_bridge or linear nodes for full
    // bridge/Env/linear refresh + service.ixx invalidate_function
    // + LoweringState on re-lower of impacted quote/lambda blocks:
    // bump bridge_epoch, re-stamp captured EnvFrame version_ (via
    // materialize or owner walk), re-emit linear_ownership_state
    // via emit_with_metadata for affected Linear* ops; integrate
    // with DirtyAwarePass for targeted linear dirty + lowering_
    // impl.cpp Variable + set_closure_bridge_ptr + emit paths in
    // cache-hit for define with quote/lambda, propagate
    // linear_state from original; on re-lower impact, refresh
    // bridge shared_ptr with updated linear metadata +
    // tests/test_prompt2_6_dep_quote_lambda_impact_linear_bridge_
    // env.cpp harness (define with quote + lambda capturing
    // linear + mutate inside body → impact_scope + partial
    // re-lower + live closure apply → assert bridge/Env/linear
    // fresh, no stale ownership/hygiene, metrics, TSan clean) +
    // SEVA quote/lambda linear demo + CI gate can call them at
    // each decision point (DepEntry quote/lambda hit / bridge_
    // epoch bump on impact / EnvFrame version refresh /
    // linear_ownership_state refresh).
    //
    // Non-duplicative with #757 (query:macro-hygiene-provenance-
    // stats), #758 (query:edsl-reflection-stats), #759 (query:
    // code-as-data-maturity-stats), #760 (query:pattern-perfor-
    // mance-stats), #761 (query:mutate-batch-stats), #762 (query:
    // workspace-closedloop-orchestration-stats), #763 (query:
    // linear-ownership-gc-compiler-stats), #764 (query:compiler-
    // arena-closure-lifetime-stats) which cover macro body
    // hygiene + EDSL struct + macro hygiene invariant correlation
    // + code-as-data closed-loop maturity + query:pattern
    // performance + end-to-end atomic batch mutate + Workspace
    // closed-loop orchestration + linear ownership + GC + compiler
    // maturation + Arena AST lifetime surfaces. #765 is the FIRST
    // observability surface that tracks the *incremental
    // compilation safety for quote/lambda/closure-heavy defines
    // composite* — DepEntry quote/lambda hit (impact_scope
    // prioritization signal), bridge_epoch bump on impact
    // (cross-bridge freshness), EnvFrame version refresh
    // (captured-env freshness), linear state refreshed
    // (linear_ownership_state re-emit freshness) — as separate
    // per-decision-point counters the Agent consumes to monitor
    // fine-grained incremental compilation + ownership safety
    // production-readiness.
    //
    //   - incremental_quote_lambda_dep_hits_total: # of DepEntry
    //                                              quote/lambda-
    //                                              introduced
    //                                              node hits
    //                                              during
    //                                              impact_scope
    //                                              (proxy for
    //                                              "how often
    //                                              the
    //                                              incremental
    //                                              compiler
    //                                              identifies
    //                                              a quote/
    //                                              lambda node
    //                                              as affected").
    //   - incremental_quote_lambda_bridge_epoch_bump_total: # of
    //                                                   bridge_
    //                                                   epoch
    //                                                   bumps
    //                                                   on
    //                                                   impact
    //                                                   re-lower
    //                                                   of
    //                                                   quote/
    //                                                   lambda
    //                                                   blocks
    //                                                   (proxy for
    //                                                   "how
    //                                                   often the
    //                                                   invalidate
    //                                                   path
    //                                                   correctly
    //                                                   bumps
    //                                                   bridge
    //                                                   epoch to
    //                                                   keep live
    //                                                   closures
    //                                                   fresh").
    //   - incremental_quote_lambda_env_version_refresh_total: # of
    //                                                      EnvFrame
    //                                                      version
    //                                                      refreshes
    //                                                      on
    //                                                      impact
    //                                                      re-lower
    //                                                      (proxy
    //                                                      for
    //                                                      "how
    //                                                      often
    //                                                      the
    //                                                      invalidate
    //                                                      path
    //                                                      correctly
    //                                                      re-
    //                                                      stamps
    //                                                      captured
    //                                                      EnvFrame
    //                                                      version_
    //                                                      to keep
    //                                                      GC walk
    //                                                      safe").
    //   - incremental_quote_lambda_linear_state_refreshed_total:
    //                                                 # of
    //                                                 linear_
    //                                                 ownership_
    //                                                 state
    //                                                 re-emits
    //                                                 via
    //                                                 emit_with_
    //                                                 metadata
    //                                                 for
    //                                                 affected
    //                                                 Linear*
    //                                                 ops
    //                                                 on
    //                                                 impact
    //                                                 (proxy for
    //                                                 "how often
    //                                                 the
    //                                                 invalidate
    //                                                 path
    //                                                 correctly
    //                                                 refreshes
    //                                                 linear_
    //                                                 ownership_
    //                                                 state
    //                                                 metadata
    //                                                 to keep
    //                                                 AI self-
    //                                                 mod safe").
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual ir_cache_pure.ixx compute_dependencies + compute_
    // impact_scope + service dep_graph_ DepEntry quote/lambda
    // flag + impact_scope priority for closure_bridge/linear
    // blocks + service.ixx invalidate_function + LoweringState
    // bridge_epoch bump + EnvFrame version_ re-stamp + linear_
    // ownership_state re-emit + DirtyAwarePass integration +
    // lowering_impl.cpp Variable cache-hit + set_closure_bridge_
    // ptr + emit paths linear_state propagation + bridge shared_
    // ptr refresh + tests/test_prompt2_6_dep_quote_lambda_impact_
    // linear_bridge_env.cpp harness (define with quote + lambda
    // capturing linear + mutate inside body → impact_scope +
    // partial re-lower + live closure apply → assert bridge/Env/
    // linear fresh, no stale ownership/hygiene, metrics, TSan
    // clean) + SEVA quote/lambda linear demo + sync epochs with
    // mutation_epoch_ + wire to pass_manager DirtyAware +
    // EscapeAnalysis for linear in quote contexts + CI gate + docs
    // are all follow-up work (each is a dedicated session in
    // ir_cache_pure.ixx + service.ixx + lowering_impl.cpp + new
    // test + SEVA demo + docs).
    std::atomic<std::uint64_t> incremental_quote_lambda_dep_hits_total{0};
    std::atomic<std::uint64_t> incremental_quote_lambda_bridge_epoch_bump_total{0};
    std::atomic<std::uint64_t> incremental_quote_lambda_env_version_refresh_total{0};
    std::atomic<std::uint64_t> incremental_quote_lambda_linear_state_refreshed_total{0};
    // Issue #800: linear ownership post-mutate fidelity observability
    // (P0 typed mutation; refines #793/#792/#784/#791; non-duplicative
    // with #763 linear-ownership-gc-compiler-stats and #638 safety-stats).
    //   - linear_postmutate_post_rollback_revalidate_total: OwnershipEnv
    //     re-validation after Guard rollback / steal resume.
    //   - linear_postmutate_escape_violations_prevented_total: escape
    //     analysis / runtime checks that caught use-after-move etc.
    //   - linear_postmutate_guard_boundary_linear_safe_total: linear
    //     invariant held at MutationBoundary exit / steal probe pass.
    //   - linear_postmutate_env_version_sync_total: EnvFrame version_
    //     sync validated under steal / materialize paths.
    std::atomic<std::uint64_t> linear_postmutate_post_rollback_revalidate_total{0};
    std::atomic<std::uint64_t> linear_postmutate_escape_violations_prevented_total{0};
    std::atomic<std::uint64_t> linear_postmutate_guard_boundary_linear_safe_total{0};
    std::atomic<std::uint64_t> linear_postmutate_env_version_sync_total{0};
    // Issue #798: ConstraintSystem incremental fidelity under Guard/steal
    // (P0 typed mutation; refines #792/#793/#466/#409; non-duplicative with
    // #608 type-incremental-stats and #509 constraint-delta-stats).
    //   - type_incremental_cross_delta_blame_complete_total: cross-delta
    //     conflicts with auditable active_mutation_id blame chain.
    //   - type_incremental_reverify_truncated_under_guard_total: clean-
    //     constraint reverify scan capped while MutationBoundary active.
    //   - type_incremental_epoch_sync_hits_total: touched-root / narrow
    //     delta marks under Guard boundary (epoch sync proxy).
    //   - type_incremental_blame_chain_length_total: cumulative blame
    //     chain steps recorded on cross-delta hits.
    std::atomic<std::uint64_t> type_incremental_cross_delta_blame_complete_total{0};
    std::atomic<std::uint64_t> type_incremental_reverify_truncated_under_guard_total{0};
    std::atomic<std::uint64_t> type_incremental_epoch_sync_hits_total{0};
    std::atomic<std::uint64_t> type_incremental_blame_chain_length_total{0};
    // Issue #1617: Let-Polymorphism dirty invalidation + solve_delta
    // occurrence/ADT priority under truncated reverify (folds into
    // query:type-incremental-fidelity-stats schema 1617).
    //   - let_poly_dirty_roots_tracked_total: mark_let_poly_dirty hits
    //   - let_poly_regeneralize_check_total: re-generalize checks in
    //     solve_delta / post-mutation Let-Poly scopes
    //   - let_poly_truncation_fallback_total: targeted reverify of
    //     let_poly roots after reverify_truncated
    //   - let_poly_priority_reverify_hits_total: clean constraints
    //     re-scanned at Let-Poly priority (>= 3)
    //   - let_poly_post_mutation_scope_total: post_mutation Let/LetRec
    //     scopes that triggered targeted reval
    //   - solve_delta_worklist_size_peak: max worklist size observed
    //     in solve_delta (mutation-load early-exit tuning)
    std::atomic<std::uint64_t> let_poly_dirty_roots_tracked_total{0};
    std::atomic<std::uint64_t> let_poly_regeneralize_check_total{0};
    std::atomic<std::uint64_t> let_poly_truncation_fallback_total{0};
    std::atomic<std::uint64_t> let_poly_priority_reverify_hits_total{0};
    std::atomic<std::uint64_t> let_poly_post_mutation_scope_total{0};
    std::atomic<std::uint64_t> solve_delta_worklist_size_peak{0};
    // Issue #648: Panic Checkpoint + Yield Checkpoint Storage
    // Lifecycle + INVALID_VERSION Frame Handling in Fiber
    // Resume + Concurrent GC counters (P0 Runtime-Gap +
    // Panic production-readiness foundation — non-duplicative
    // to #637 #356 #264).
    // These are scaffolding for the future AC1 + AC2 + AC3
    // enforcement work — the bumps happen when fiber
    // resume() after transfer hook validates/syncs
    // per-fiber yield_checkpoint_storage_ with current Guard
    // panic state (#648 AC1), when GCEnvWalkFn + compact
    // explicitly handle INVALID_VERSION frames (skip/count)
    // even during panic restore paths (#648 AC2), and when
    // g_fiber_yield_checkpoint_ + resume_validate_ coordinate
    // with panic checkpoint under MutationBoundary
    // (#648 AC3). P0 ships the counters + the agent-visible
    // (query:panic-checkpoint-fiber-stats) primitive so the
    // Agent has a dashboard today; values are 0 until the
    // enforcement work ships.
    //   - panic_transfer_on_resume_total: AC1 — count of panic
    //     checkpoint transfers on fiber resume. High rate
    //     on multi-agent fleets = panic injection is real
    //     production path (not a hot path under steady
    //     state).
    //   - panic_invalid_frames_skipped_total: AC2 — count of
    //     INVALID_VERSION frames skipped during GC walk /
    //     compact (post-rollback #356 frames). Ratio
    //     (skipped / total walk frames) = how much of the
    //     env_frames_ arena is in the post-rollback
    //     INVALID_VERSION state.
    //   - panic_concurrent_gc_conflict_total: AC3 — count
    //     of concurrent panic + GC conflict events where
    //     panic checkpoint + GC safepoint race. High rate
    //     = production memory/GC instability under panic
    //     path (signal that AC3 coordination wire-up is
    //     needed).
    std::atomic<std::uint64_t> panic_transfer_on_resume_total{0};
    std::atomic<std::uint64_t> panic_invalid_frames_skipped_total{0};
    std::atomic<std::uint64_t> panic_concurrent_gc_conflict_total{0};
    // Issue #649: Full Per-Fiber YieldCheckpointStorage
    // Re-Stamp + Size Validation on Panic Transfer +
    // Cross-Steal counters (P0 Runtime-Gap + Panic
    // production-readiness foundation — non-duplicative to
    // #648 #264).
    // These are scaffolding for the future AC1 + AC2 + AC3
    // enforcement work — the bumps happen when
    // transfer_panic_checkpoint_trampoline + fiber resume
    // after hook call re-stamp or resize
    // yield_checkpoint_storage_ to match current panic
    // Guard state (#649 AC1), when restore_post_yield_or_
    // rollback adds yield_checkpoint stack size + top-entry
    // version check (#649 AC2), and when g_fiber_yield_
    // checkpoint_ coordinates with pending_panic_checkpoint
    // under MutationBoundary (#649 AC3). P0 ships the
    // counters + the agent-visible
    // (query:yield-checkpoint-panic-stats) primitive so the
    // Agent has a dashboard today; values are 0 until the
    // enforcement work ships.
    //   - yield_transfer_with_restamp_total: AC1 — count of
    //     panic transfers that triggered a yield_checkpoint
    //     storage re-stamp/resize. Ratio (this / panic
    //     transfer_on_resume_total from #648) = how often
    //     panic transfer path needed the re-stamp.
    //   - yield_size_mismatch_caught_total: AC2 — count of
    //     yield_checkpoint stack size mismatches caught in
    //     restore_post_yield_or_rollback. High rate = long-
    //     running fibers are accumulating unbounded yield
    //     checkpoints (storage growth risk).
    //   - yield_cross_steal_invalidation_total: AC3 — count
    //     of cross-steal invalidations of pending yield
    //     checkpoints (per-AC3 coordination). High rate =
    //     production fiber fleet is doing frequent steal +
    //     yield + panic path — the AC3 wire-up needs to land.
    std::atomic<std::uint64_t> yield_transfer_with_restamp_total{0};
    std::atomic<std::uint64_t> yield_size_mismatch_caught_total{0};
    std::atomic<std::uint64_t> yield_cross_steal_invalidation_total{0};
    // Issue #650: StealBudget in WorkerThread to Use fiber
    // yield_classification() + Outermost Mutation Depth for
    // Adaptive Bias counters (P0 Runtime-Gap + Scheduler
    // production-readiness foundation — non-duplicative to
    // #645).
    // These are scaffolding for the future AC1 + AC2 + AC4
    // enforcement work — the bumps happen when
    // try_steal_from / should_steal query victim
    // yield_classification() or is_at_mutation_boundary_safe
    // (depth==0); outermost Mutation or Explicit → increase
    // steal priority / reduce sleep threshold (#650 AC1),
    // when high steal_deferred_mutation_boundary_count
    // temporarily raises max_before_sleep or biases LIFO
    // local (#650 AC2), and when unit test mocks are wired
    // to StealBudget + Fiber yield reasons (#650 AC4). P0
    // ships the counters + the agent-visible
    // (query:scheduler-stealbudget-yield-class-stats)
    // primitive so the Agent has a dashboard today; values
    // are 0 until the enforcement work ships.
    //   - stealbudget_outermost_bias_total: AC1 — count of
    //     StealBudget bias hits preferring outermost Mutation
    //     fibers (depth==0). High rate = scheduler is
    //     successfully preferring stealable fibers under LLM
    //     bottleneck.
    //   - stealbudget_explicit_bias_total: AC1 — count of
    //     StealBudget bias hits preferring Explicit yield
    //     reason fibers (OperationBoundary / Explicit
    //     yields that don't hold MutationBoundary).
    //   - stealbudget_max_before_sleep_raised_total: AC2 —
    //     count of times StealBudget raised max_before_sleep
    //     due to high steal_deferred_mutation_boundary_count.
    //     Ratio (this / total StealBudget decisions) = how
    //     often contention pressure forced the budget
    //     expansion.
    std::atomic<std::uint64_t> stealbudget_outermost_bias_total{0};
    std::atomic<std::uint64_t> stealbudget_explicit_bias_total{0};
    std::atomic<std::uint64_t> stealbudget_max_before_sleep_raised_total{0};
    // Issue #651: Actual GC Deferral/Block Logic in
    // block_gc_for_pending_checkpoint_trampoline + Request
    // Shim counters (P0 Runtime-Gap + GC production-readiness
    // foundation — fills TODO in evaluator_fiber_mutation.cpp,
    // non-duplicative to #646 #648).
    // These are scaffolding for the future AC1 + AC2 + AC3
    // enforcement work — the bumps happen when
    // block_gc_for_pending_checkpoint_trampoline implements
    // real deferral: if pending, signal scheduler/GC
    // coordinator to defer phase or wait; integrate with
    // gc_state phase (#651 AC1), when aura_evaluator_request_
    // gc_safepoint checks pending_panic_checkpoint() && depth
    // > 0 and defers request + bumps metric / yield /
    // retries (#651 AC2), and when fiber check_gc_safepoint
    // + scheduler wait_for_safepoint wire to pending-panic
    // awareness (#651 AC3). P0 ships the counters + the
    // agent-visible (query:gc-panic-deferral-stats) primitive
    // so the Agent has a dashboard today; values are 0
    // until the enforcement work ships.
    //   - gc_panic_pending_deferral_total: AC1 — count of
    //     pending panic checkpoint deferrals triggered in
    //     block_gc trampoline. High rate = panic injection
    //     is real production path (not a hot path under
    //     steady state).
    //   - gc_blocked_by_panic_total: AC2 — count of GC
    //     safepoint requests blocked due to pending panic
    //     checkpoint + depth > 0. Ratio (blocked / total
    //     requests) = how often panic path conflicts with
    //     GC requests.
    //   - gc_panic_conflict_resolved_total: AC3 — count of
    //     panic + GC conflict events resolved (deferral
    //     completed without root inconsistency). High
    //     resolution rate = panic + GC coordination wire-up
    //     works under contention.
    std::atomic<std::uint64_t> gc_panic_pending_deferral_total{0};
    std::atomic<std::uint64_t> gc_blocked_by_panic_total{0};
    std::atomic<std::uint64_t> gc_panic_conflict_resolved_total{0};
    // Issue #1866: compact_sweep(nullptr) / null sweep_buffers —
    // misconfiguration (GC collector omitted marks). Zeroed
    // CompactSweepResult is still returned (#1732 by-value API),
    // but the call is no longer silent: this counter surfaces the
    // skipped reclaim path for diagnostics / Agent dashboards.
    std::atomic<std::uint64_t> gc_compact_sweep_null_marks_total{0};
    // Issue #589: SoA EnvFrame/EnvId dual-path
    // bindings_ vs bindings_symid_ consistency + version
    // stamping + stale refresh in materialize_call_env &
    // GCEnvWalkFn counters (P0 Runtime-Review + SoA
    // production-readiness foundation — non-duplicative to
    // existing #543 / #568 / #205).
    // These are scaffolding for the future AC1 + AC2 + AC3
    // enforcement work — the bumps happen when Env::bind_symid
    // / bind always mirror: bindings_symid_.push + bindings_
    // .push + on owner_ set stamp defuse_version_ into
    // env_version_ (#589 AC1), when materialize_call_env on
    // version mismatch calls refresh_dual_path_from_soa
    // helper that syncs bindings_ <-> bindings_symid_ +
    // parent_id_ + bumps envframe_stale_refresh_count_
    // (#589 AC2), and when walk_env_frames / GCEnvWalkFn
    // before emitting roots refreshes or skips with metric
    // if frame.version_ < current_defuse (#589 AC3). P0
    // ships the counters + the agent-visible
    // (query:envframe-dualpath-enforce-stats) primitive so
    // the Agent has a dashboard today; values are 0 until the
    // enforcement work ships.
    //   - envframe_dualpath_mirror_write_total: AC1 —
    //     count of bind/bind_symid mirror writes
    //     (symmetric bindings_symid_ + bindings_ push under
    //     pool_ mode). Ratio (this / total bind calls) =
    //     how symmetric dual-path writes are.
    //   - envframe_dualpath_refresh_total: AC2 — count of
    //     refresh_dual_path_from_soa helper calls (from
    //     materialize_call_env on version mismatch).
    //     Ratio (refresh / version mismatch) = recovery rate.
    //   - envframe_dualpath_consistency_violations_total:
    //     AC3 — count of consistency violations caught in
    //     walk_env_frames / GCEnvWalkFn (frame.version_ <
    //     current_defuse, skip or refresh path). High rate
    //     = production long-running agent fleet is hitting
    //     the version-mismatch path frequently.
    std::atomic<std::uint64_t> envframe_dualpath_mirror_write_total{0};
    std::atomic<std::uint64_t> envframe_dualpath_refresh_total{0};
    std::atomic<std::uint64_t> envframe_dualpath_consistency_violations_total{0};
    // Issue #590: AOT mangle versioning + region filtering +
    // multi-agent hot-update isolation + closure dispatch
    // stale detection counters (P0 Runtime-Review + AOT
    // production-readiness foundation — non-duplicative to
    // existing #544 / #323 / #287).
    // These are scaffolding for the future AC1 + AC2 + AC3
    // enforcement work — the bumps happen when
    // mangle_aot_name / generate_registration_c add optional
    // region/agent_id prefix to mangled name + reload
    // success iterates func_table rebinding matching
    // version/region with refcounts (#590 AC1), when
    // (aot:reload-with-region path version region) primitive
    // and (query:aot-hotupdate-stats) primitive ship + the
    // 4 surface counters get bumped (#590 AC2), and when
    // closure dispatch (JIT bridge / evaluator) version check
    // on func_id lookup; on mismatch force deopt or error with
    // metric (#590 AC3). P0 ships the counters + the
    // agent-visible (query:aot-hotupdate-stats) primitive so
    // the Agent has a dashboard today; values are 0 until the
    // enforcement work ships.
    //   - aot_hotupdate_region_isolation_total: AC1 — count
    //     of region isolation hits (reload only affected
    //     target region, no cross-region pollution). High
    //     rate = multi-agent fleet is using region-isolated
    //     hot-swap correctly.
    //   - aot_hotupdate_dispatch_stale_prevented_total:
    //     AC3 — count of closure dispatch stale mismatches
    //     prevented (func_id version mismatch on lookup
    //     → force deopt or error path). High rate = long-
    //     running agents are catching potential stale
    //     dispatch UAF.
    //   - aot_hotupdate_multi_agent_reload_total: AC1 —
    //     count of successful multi-agent reload cycles
    //     (region-prefixed reload + func_table rebind).
    //     High rate = commercial multi-agent hot-swap is
    //     actively used in production.
    std::atomic<std::uint64_t> aot_hotupdate_region_isolation_total{0};
    std::atomic<std::uint64_t> aot_hotupdate_dispatch_stale_prevented_total{0};
    std::atomic<std::uint64_t> aot_hotupdate_multi_agent_reload_total{0};

    // Issue #593: AST→query→IR MacroIntroduced hygiene closed-loop
    // foundation atomics (pattern capture prevention, post-mutate
    // IR violation tally, tag_arity delta hits during hygiene query).
    std::atomic<std::uint64_t> pattern_ir_capture_prevented_total{0};
    std::atomic<std::uint64_t> ir_hygiene_post_mutate_violation_total{0};
    std::atomic<std::uint64_t> tag_arity_hygiene_query_delta_total{0};

    // Issue #596: Guard + panic checkpoint + reflect/schema validation +
    // fiber resume closed-loop foundation atomics (non-duplicative with
    // #548 panic-checkpoint-lifecycle-stats, #594 reflection-selfmod-stats,
    // #592 fiber resume safety matrix).
    std::atomic<std::uint64_t> guard_panic_reflect_restores_on_resume_total{0};
    std::atomic<std::uint64_t> guard_panic_reflect_validate_hook_total{0};
    std::atomic<std::uint64_t> guard_panic_reflect_boundary_violation_prevented_total{0};

    // Issue #599: compiler-managed root epoch/version protocol for live
    // IRClosure/EnvFrame post-invalidate_function (non-duplicative with
    // #531 closure-env-safety-stats, #598 linear-ownership-runtime-stats).
    std::atomic<std::uint64_t> compiler_root_stale_closure_detected_total{0};
    std::atomic<std::uint64_t> compiler_root_env_version_mismatch_total{0};
    std::atomic<std::uint64_t> compiler_root_dangling_prevented_total{0};

    // Issue #600: per-block dirty + impact scope + closure bridge synergy
    // for minimal re-lower (non-duplicative with #530 incremental-production-
    // relower-stats, #429 soa-dirty-stats, #531 closure-env-safety-stats).
    std::atomic<std::uint64_t> incremental_closure_blocks_relowered_total{0};
    std::atomic<std::uint64_t> incremental_closure_min_scope_win_total{0};
    std::atomic<std::uint64_t> incremental_closure_jit_sync_total{0};
    // Issue #741: impact_scope → closure_bridge shared_ptr + EnvFrame
    // version re-stamp for quote/lambda-heavy defines (non-duplicative
    // with #718 block_dirty, #719 epoch/bridge general safety).
    // Exposed via (query:incremental-closure-bridge-stats).
    std::atomic<std::uint64_t> incremental_closure_bridge_impact_blocks_total{0};
    std::atomic<std::uint64_t> incremental_closure_quote_lambda_stale_prevented_total{0};
    std::atomic<std::uint64_t> incremental_closure_env_version_resync_total{0};
    // Issue #804: unified primitive error semantics observability
    // counters (P0 stdlib reliability foundation; refines/
    // consolidates #585 + #751 + #775 + #478; non-duplicative
    // with #585 query:primitives-error-stats coarse hash +
    // #478 query:primitive-error-stats pair primitive + #751
    // query:primitives-contract-stats contract enforcement).
    // #804 introduces the FIRST observability surface that
    // tracks the unified-error-path SLO composite — the
    // body asks for "100% primitives use unified path; zero
    // silent fallback errors under load" — by splitting
    // primitive_error_count_ into three sub-counter families:
    //   - primitive_error_with_provenance_total: PRIM_ERROR /
    //     make_primitive_error invocations that fill in the
    //     (kind, msg, provenance) schema; the *good* path
    //     the body asks for. Bumped by
    //     bump_primitive_error_with_provenance() at the call
    //     sites that already use PRIM_ERROR.
    //   - primitive_error_silent_fallback_total: ad-hoc
    //     returns (make_int(0) / void / catch-all on bad
    //     args) the body warns against. Counted by the audit
    //     grep-step in Phase 2+; for now the atomic is a
    //     no-op (value 0) — kept as the forward-looking
    //     signal.
    //   - primitive_error_recovery_hook_invocations_total:
    //     count of recovery-hook firings in Guard + retry
    //     path. Bumped by bump_primitive_error_recovery_
    //     hook() at the planned Phase 2+ recovery path.
    // P0 ships the counters + the (query:primitive-error-
    // unified-stats, schema 804) primitive so the Agent has
    // a deployment-grade SLO composite today; values are 0
    // until the Phase 2+ PRIM_ERROR audit + registry
    // enforcement + (error:structured-make ...) + recovery
    // hooks land.
    std::atomic<std::uint64_t> primitive_error_with_provenance_total{0};
    std::atomic<std::uint64_t> primitive_error_silent_fallback_total{0};
    std::atomic<std::uint64_t> primitive_error_recovery_hook_invocations_total{0};

    // Issue #654: Macro+reflect+self-evo hygiene vs fiber/panic/AOT/SoA
    // cross-cutting gaps (non-duplicative with #593 pattern-ir-hygiene,
    // #596 guard-panic-reflect, #597 macro-reflect-self-evo-stats).
    std::atomic<std::uint64_t> macro_hygiene_panic_restamp_total{0};
    std::atomic<std::uint64_t> macro_hygiene_provenance_violations_total{0};
    std::atomic<std::uint64_t> macro_expand_checkpoint_saves_total{0};
    std::atomic<std::uint64_t> macro_reflect_hygiene_validation_total{0};
    std::atomic<std::uint64_t> macro_hygiene_dirty_impact_total{0};

    // Issue #712: macro subtree-level reflect validation. The
    // (query:macro-reflect-validation-stats) primitive exposes:
    //   - validation_calls                  calls to auto_validate on
    //                                       MacroIntroduced subtrees
    //   - schema_mismatches_caught          # of marker-state drifts
    //                                       detected during post-mutate
    //                                       reflect pass
    //   - post_mutate_hygiene_drift         # of nodes that became
    //                                       dirty AND macro-introduced
    //                                       between committed snapshot
    //                                       and current state
    // (Non-duplicative with macro_reflect_hygiene_validation_total,
    // which counts passes with macro_markers > 0 in the WHOLE
    // workspace — #712 splits out subtree-level diagnostics.)
    std::atomic<std::uint64_t> macro_reflect_validation_calls_total{0};
    std::atomic<std::uint64_t> macro_reflect_schema_mismatches_caught_total{0};
    std::atomic<std::uint64_t> macro_reflect_post_mutate_hygiene_drift_total{0};

    // Issue #713: macro hygiene violations detected in JIT deopt,
    // AOT reload, and Interpreter fallback paths. The
    // (query:macro-jit-hygiene-stats) primitive exposes:
    //   - deopt_on_hygiene                  # of times JIT deopt was
    //                                       triggered because a
    //                                       source_marker=MacroIntroduced
    //                                       call site tried to inline
    //                                       into User code (or other
    //                                       policy violation)
    //   - aot_reload_marker_mismatches      # of times AOT reload
    //                                       rejected/restamped a
    //                                       module because its
    //                                       source_marker column
    //                                       drifted from the host's
    //                                       expected markers
    //   - interpreter_fallback_hygiene_hits # of times the IR
    //                                       executor (interpreter
    //                                       fallback path) hit a
    //                                       source_marker=MacroIntroduced
    //                                       dispatch and chose
    //                                       conservative fallback
    // (All three counters are 0 on a fresh service. The bump
    // helpers are wired into aura_jit_bridge.cpp +
    // evaluator.ixx fast paths; deopt wiring (LLVM IR emit hook)
    // is a follow-up since it lives in the JIT code generator.)
    std::atomic<std::uint64_t> macro_jit_hygiene_deopt_total{0};
    std::atomic<std::uint64_t> macro_aot_reload_marker_mismatches_total{0};
    std::atomic<std::uint64_t> macro_interpreter_fallback_hygiene_hits_total{0};

    // Issue #714: self-evolution closed-loop stats — Agent decision
    // support for autonomous mutation strategy selection. The
    // (query:self-evolution-closedloop-stats) primitive exposes a
    // single integrated report correlating hygiene (MacroIntroduced
    // count, violation rate), dirty impact (subtree affected), epoch
    // drift (panic restamp proxy), and reflect-validation pass rate
    // plus a recommended mutation strategy (safe/aggressive/balanced)
    // derived from the per-strategy recommendation counts below.
    //
    // Phase 1 ships the strategy recommendation *counters* and
    // derivation logic — the actual Guard dtor + mark_dirty_upward
    // + reflect auto_validate wiring that drives the recommendation
    // counts is follow-up work (each hook is a dedicated session).
    //
    // (Non-duplicative with #654 macro-hygiene-fiber-panic-stats,
    // #712 macro-reflect-validation-stats, #713 macro-jit-hygiene-
    // stats, #488 schema-validation. #714 is the FIRST primitive
    // that correlates hygiene/dirty/epoch/reflect signals into a
    // single Agent-facing strategy recommendation.)
    std::atomic<std::uint64_t> self_evo_strategy_recommend_safe_total{0};
    std::atomic<std::uint64_t> self_evo_strategy_recommend_aggressive_total{0};
    std::atomic<std::uint64_t> self_evo_strategy_recommend_balanced_total{0};

    // Issue #715: StableNodeRef cross-layer validation counters
    // for WorkspaceTree multi-layer setups. The
    // (query:stable-ref-layer-stats) primitive exposes:
    //   - cross_layer_validations    # of times is_valid_in_layer
    //                                passed (gen + workspace_id +
    //                                cow_epoch all aligned)
    //   - cross_layer_mismatches     # of times is_valid_in_layer
    //                                returned false (gen drift,
    //                                workspace_id mismatch, or
    //                                cow_epoch boundary crossed
    //                                without pin_for_cow)
    //   - cow_boundary_pins          # of StableNodeRefs that
    //                                crossed a COW boundary via
    //                                pin_for_cow() — proxy for
    //                                "how many refs are intentionally
    //                                surviving lazy clones"
    //
    // Phase 1 ships the counters + bump helpers + the
    // is_valid_in_layer helper on StableNodeRef. The
    // MutationBoundaryGuard auto-remap / workspace-merge
    // path wiring that produces these counters is follow-up
    // work (each hook is a dedicated session). The helper
    // itself is allocation-free and pure read, so existing
    // single-layer callers that drop in
    // is_valid_in_layer(ast, ref.workspace_id_) get the
    // extra workspace_id + cow_epoch checks for free.
    //
    // Non-duplicative with the existing stable_ref_invalidations_
    // counter (issue #191/#255/#368) which counts
    // is_valid() failures only; #715 is the FIRST observability
    // surface that splits out cross-layer + COW-boundary
    // signals.
    std::atomic<std::uint64_t> stable_ref_cross_layer_validations_total{0};
    std::atomic<std::uint64_t> stable_ref_cross_layer_mismatch_total{0};
    std::atomic<std::uint64_t> stable_ref_cow_boundary_pins_total{0};

    // Issue #716: pattern matcher observability counters for
    // (query:pattern-stats). Exposes matcher-level signals that
    // complement the existing tag_arity_index_* counters (#547 /
    // #490 / #621 / #654):
    //   - pattern_matcher_calls_total     # of query:pattern /
    //                                     query:where / query:filter
    //                                     invocations (lifetime)
    //   - pattern_macro_intro_filtered_total
    //                                    # of AST nodes skipped
    //                                     by is_macro_introduced()
    //                                     hygiene filter during
    //                                     pattern matching (proxy
    //                                     for "how much user-focused
    //                                     noise the matcher avoided")
    //   - pattern_fast_path_hits_total    # of simple tag+arity
    //                                     queries served from
    //                                     cache without full
    //                                     pattern traversal
    //
    // Phase 1 ships the counters + bump helpers + the
    // primitive. The actual is_macro_introduced() skip wiring
    // in query_matcher.cpp hot path + the cache promotion +
    // configurable hygiene filter mode (user-focused vs
    // macro-aware) are follow-up (each is a dedicated session
    // in evaluator_primitives_query.cpp + query_matcher.cpp).
    //
    // Non-duplicative with the existing tag_arity_index_*
    // counters which track the index itself (#547 + #554 + #621
    // #654); #716 is the FIRST observability surface that
    // tracks the matcher call path + hygiene filter + fast-path
    // promotion as separate signals.
    std::atomic<std::uint64_t> pattern_matcher_calls_total{0};
    std::atomic<std::uint64_t> pattern_macro_intro_filtered_total{0};
    std::atomic<std::uint64_t> pattern_fast_path_hits_total{0};

    // Issue #717: fiber-safe MutationBoundaryGuard recovery
    // counters for (query:fiber-boundary-violation-stats).
    // Complements the existing #438 fiber-migration-stats
    // (which tracks steal-attempts + boundary-violations +
    // deferred counts) with RECOVERY-side signals:
    //   - mutation_boundary_rollbacks_total  # of times the
    //                                        MutationBoundaryGuard
    //                                        dtor triggered a
    //                                        rollback (fiber-
    //                                        aware epoch bump +
    //                                        dirty clear +
    //                                        StableRef remap)
    //   - mutation_boundary_yield_resumes_total
    //                                      # of times a fiber
    //                                        successfully resumed
    //                                        after yielding at a
    //                                        boundary (yield reason
    //                                        was stolen/deferred +
    //                                        resume + re-entry
    //                                        succeeded)
    //   - mutation_boundary_recovery_failures_total
    //                                      # of times recovery
    //                                        FAILED: partial dirty
    //                                        state detected, leaked
    //                                        StableRef, defuse_version_
    //                                        drift across resume
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual fiber-context check on guard dtor +
    // panic_checkpoint integration with per-fiber mutation_stack_
    // snapshot + targeted multi-fiber tests are follow-up work
    // (each is a dedicated session in evaluator_fiber_mutation.cpp
    // + evaluator_primitives_mutate.cpp).
    //
    // Non-duplicative with #438 fiber-migration-stats (#438
    // tracks steal-attempts / boundary-violations / defer counts
    // from the SCHEDULER side; #717 tracks rollback / resume /
    // recovery-failure counts from the GUARD side — these are
    // complementary signals, not redundant).
    std::atomic<std::uint64_t> mutation_boundary_rollbacks_total{0};
    std::atomic<std::uint64_t> mutation_boundary_yield_resumes_total{0};
    std::atomic<std::uint64_t> mutation_boundary_recovery_failures_total{0};

    // Issue #1646: MutationBoundaryGuard long-running observability wiring
    // (refine #1637 / #1014). Bumped at Guard dtor (success path) /
    // flush_mutation_boundary (deep path) / hygiene-violation-detection
    // sites. Distinct from the legacy flush-time-counter (per-Fiber)
    // already covered by bump_mutation_boundary_recovery_failures_total +
    // bump_mutation_boundary_recovery_failure per-CompilerMetrics pair
    // via #1908 / #1641 lineage. The new success_total /
    // macro_dirty_propagated_total / epoch_bump_for_macro_total /
    // hygiene_violation_total slots are the explicitly-requested
    // observability gaps from #1646 body’s Guard success + propagation +
    // epoch + hygiene-violation sites.
    std::atomic<std::uint64_t> mutation_boundary_success_total{0};
    std::atomic<std::uint64_t> mutation_boundary_macro_dirty_propagated_total{0};
    std::atomic<std::uint64_t> mutation_boundary_epoch_bump_for_macro_total{0};
    std::atomic<std::uint64_t> mutation_boundary_hygiene_violation_total{0};
    // Per-call counter for (query:mutation-boundary-observability-stats).
    std::atomic<std::uint64_t> mutation_boundary_observability_queries_total{0};
    // Issue #1684: Guard::run_or_rollback caught a throw mid-mutate.
    std::atomic<std::uint64_t> mutation_boundary_exception_rollback_total{0};
    // Issue #1769: run_typecheck_no_lock* / run_post_mutate_typecheck_no_lock
    // swallowed a throw (fuzzer + mutate inline TC paths).
    std::atomic<std::uint64_t> inline_typecheck_exception_total{0};

    // Issue #718: fine-grained per-block re-lower observability
    // counters for (query:incremental-relower-stats). These
    // complement the existing #196 per-block dirty tracking +
    // #426/#460 pure helpers (compute_impact_scope, summarize_
    // block_dirty, estimate_relower_blocks) by exposing the
    // decision / outcome signals:
    //   - incremental_impact_blocks_hit_total  # of times
    //                                        compute_impact_scope
    //                                        returned >=1 affected
    //                                        block for a mutate:rebind
    //                                        / set-body request
    //   - incremental_partial_relower_total   # of times
    //                                        should_partial_relower
    //                                        returned true (1..7 dirty
    //                                        blocks) and the pipeline
    //                                        took the partial path
    //   - incremental_full_fallback_total     # of times the pipeline
    //                                        took the FULL re-lower
    //                                        path (8+ dirty blocks,
    //                                        or no impact_scope data)
    //   - incremental_time_saved_us_total     cumulative time saved
    //                                        in microseconds by
    //                                        choosing partial over
    //                                        full re-lower (estimated
    //                                        from block count delta;
    //                                        pipeline wiring computes
    //                                        the actual savings)
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual compute_impact_scope call + block_dirty_ bit
    // setting inside service.ixx::invalidate_function + the
    // partial re-lower decision in lowering_impl.cpp +
    // lower_to_ir_with_cache + the pass_manager short-circuit
    // are follow-up work (each is a dedicated session in
    // service.ixx + lowering_impl.cpp + pass_manager.ixx).
    //
    // Non-duplicative with #196 (per-block dirty tracking in
    // IRCacheEntry.block_dirty_per_func_) + #426/#460
    // (pure helpers) + #687 (DeadCoercionEliminationPass +
    // IR-interpreter identity fast-path); #718 is the FIRST
    // observability surface that exposes the partial-vs-full
    // re-lower decision outcomes as separate signals.
    std::atomic<std::uint64_t> incremental_impact_blocks_hit_total{0};
    std::atomic<std::uint64_t> incremental_partial_relower_total{0};
    std::atomic<std::uint64_t> incremental_full_fallback_total{0};
    std::atomic<std::uint64_t> incremental_time_saved_us_total{0};
    // Issue #1854: query:incremental-effectiveness snapshot() threw;
    // primitive returns void (not a false-clean 4-tuple of zeros).
    std::atomic<std::uint64_t> incremental_effectiveness_snapshot_failures{0};
    // Issue #1856: CompilerService::try_snapshot() / any stats
    // primitive that used to call snapshot() raw — throw count.
    std::atomic<std::uint64_t> snapshot_failures_total{0};

    // Issue #719: Prompt 6 closure/EnvFrame epoch + linear ownership
    // + GC root sync safety counters for
    // (query:closure-env-epoch-safety-stats). Exposes the
    // runtime-guard outcomes that prevent dangling closures,
    // stale EnvFrame version_, linear ownership violations,
    // and GC-root drift after invalidate_function / mutate:
    // rebind / set-body in long-running AI agent loops:
    //   - closure_epoch_mismatch_total   # of times apply_closure
    //                                    detected a stale
    //                                    bridge_epoch (closure
    //                                    captured pre-invalidate)
    //                                    before dispatching to
    //                                    the map or bridge path
    //   - linear_violation_post_mutate_total
    //                                    # of times GuardShape /
    //                                    Linear* op handler /
    //                                    JIT PrimCall/Capture
    //                                    detected a linear
    //                                    ownership_state != 0
    //                                    with epoch/version
    //                                    mismatch post-mutate
    //   - gc_root_sync_total             # of ScopedCompilerRoot
    //                                    register/unregister
    //                                    syncs triggered from
    //                                    invalidate_function /
    //                                    MutationBoundaryGuard dtor
    //   - dangling_prevented_total       # of times a UAF /
    //                                    dangling situation was
    //                                    prevented by the runtime
    //                                    guard (proxy for "how
    //                                    many silent corruptions
    //                                    the guard caught")
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual epoch/version check in apply_closure hot path,
    // IRClosure/closure_bridge_ management on invalidate,
    // linear_ownership_state runtime guard in GuardShape/Linear
    // op handlers / JIT, and ScopedCompilerRoot GC hook are
    // follow-up work (each is a dedicated session in
    // evaluator_eval_flat.cpp + service.ixx + evaluator_gc.cpp +
    // ir_executor_impl.cpp + aura_jit*.cpp).
    //
    // Non-duplicative with the existing #672 linear_stats
    // (which tracks compile-time linear type errors) and #681
    // epoch enforcement (which is IR-level metadata); #719 is
    // the FIRST observability surface that tracks runtime
    // closure/EnvFrame/linear/GC safety outcomes in apply_closure
    // and JIT hot paths as separate signals.
    std::atomic<std::uint64_t> closure_epoch_mismatch_total{0};
    std::atomic<std::uint64_t> linear_violation_post_mutate_total{0};
    std::atomic<std::uint64_t> gc_root_sync_total{0};
    std::atomic<std::uint64_t> dangling_prevented_total{0};

    // Issue #720: JIT/Interpreter parity observability counters
    // for (query:jit-interpreter-parity-stats). Exposes the
    // JIT-side hot-path drift signals that the existing
    // unhandled_opcode_count / fallback_count metrics in
    // aura_jit.cpp cannot differentiate (those are aggregate
    // per-function totals; #720 splits by *cause*):
    //   - jit_unhandled_opcode_spikes_total  # of times an
    //                                        unhandled_opcode spike
    //                                        crossed the per-function
    //                                        threshold post-mutation
    //                                        (triggers JIT->service
    //                                        invalidate hook + deopt)
    //   - jit_metadata_mismatch_total       # of times metadata
    //                                        (linear_ownership_state /
    //                                        shape_id / narrow_evidence
    //                                        / source_marker) drift was
    //                                        detected between IRSoA /
    //                                        AoS and the JIT's
    //                                        FlatInstruction
    //   - jit_deopt_on_mutate_total          # of times JIT deopt was
    //                                        triggered by a mutate /
    //                                        invalidate event (forced
    //                                        Interpreter fallback +
    //                                        async recompile request
    //                                        via CompilerService hook)
    //   - jit_fallback_to_interpreter_total # of explicit fallbacks
    //                                        to Interpreter (proxy
    //                                        for "how often the JIT
    //                                        decided to give up on
    //                                        hot path post-mutation")
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual FlatInstruction metadata extension + unhandled
    // hook + GuardShape/linear full consume + deopt->service wiring
    // + JIT->CompilerService invalidate hook are follow-up work
    // (each is a dedicated session in aura_jit.cpp + aura_jit.h +
    // aura_jit_bridge.cpp + service.ixx + ir_executor_impl.cpp).
    //
    // Non-duplicative with the existing unhandled_opcode_count /
    // fallback_count metrics in aura_jit.cpp (which are aggregate
    // per-function lifetime totals); #720 splits by *cause* and
    // adds the post-mutation spike + metadata drift signals that
    // the JIT hot path needs to surface for production stability.
    std::atomic<std::uint64_t> jit_unhandled_opcode_spikes_total{0};
    std::atomic<std::uint64_t> jit_metadata_mismatch_total{0};
    std::atomic<std::uint64_t> jit_deopt_on_mutate_total{0};
    std::atomic<std::uint64_t> jit_fallback_to_interpreter_total{0};

    // Issue #721: IRFunctionSoA column migration + dirty cascade
    // counters for (query:ir-soa-completeness-stats). Exposes
    // the SoA hot-path signals that complement the existing
    // IRFunctionSoA scaffold (10 columns + mark_block_dirty
    // cascade) without overlapping #658 broad 5-gaps, #719 JIT
    // metadata, #718 incremental block dirty:
    //   - ir_soa_column_migration_hits_total  # of times a hot
    //                                          emit/view path took
    //                                          the SoA iterator
    //                                          branch (vs AoS
    //                                          fallback)
    //   - ir_soa_dirty_cascade_to_shape_total # of times the
    //                                          mark_block_dirty
    //                                          cascade propagated
    //                                          to ShapeProfiler::
    //                                          invalidate or
    //                                          bumped dirty_shape
    //                                          hint
    //   - ir_soa_pcv_wiring_savings_bytes_total
    //                                        cumulative bytes
    //                                          saved by PCV-style
    //                                          (PersistentChildVector
    //                                          / gap_buffer) wiring
    //                                          on operand / shape /
    //                                          metadata columns
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual PCV-style column extension + add_instruction
    // atomic growth + IRInstructionView dirty bit query + port
    // of hot emit/view paths to SoA iterators + ShapeProfiler
    // invalidate hook are follow-up work (each is a dedicated
    // session in ir_soa.ixx + ir_soa_helpers + lowering_impl.cpp
    // + evaluator + aura_jit.cpp + ShapeProfiler + Arena).
    //
    // Non-duplicative with #658 5-gaps (broad), #719 JIT metadata
    // (JIT-side), #718 incremental block dirty (block-level);
    // #721 is the FIRST observability surface that tracks SoA
    // column migration progress + dirty cascade shape/arena
    // propagation as separate signals.
    std::atomic<std::uint64_t> ir_soa_column_migration_hits_total{0};
    std::atomic<std::uint64_t> ir_soa_dirty_cascade_to_shape_total{0};
    std::atomic<std::uint64_t> ir_soa_pcv_wiring_savings_bytes_total{0};

    // Issue #722: Arena tier/dtor/compact integration observability
    // counters for (query:arena-integration-stats). Exposes the
    // hot-path integration signals that complement the existing
    // ArenaStats (compaction_count / fragmentation_ratio /
    // defrag_requested_) and request_defrag / clear in arena.ixx
    // without overlapping #658 Gap1 broad or #642 high-level:
    //   - arena_tier_fallbacks_total         # of times the
    //                                         SmallObjectPool tier
    //                                         (16/32/64B) was
    //                                         exhausted and the
    //                                         allocator fell back to
    //                                         pmr
    //   - arena_dtor_dirty_hooks_total       # of times the dtor
    //                                         thunk triggered a
    //                                         dirty/shape hook on
    //                                         reset / compact (proxy
    //                                         for "how many small
    //                                         nodes invalidated
    //                                         shape on destroy")
    //   - arena_auto_compact_triggers_total  # of times the
    //                                         auto-compact policy
    //                                         triggered compact/defrag
    //                                         from fragmentation +
    //                                         yield_check or dirty
    //                                         cascade (no manual
    //                                         request_defrag call)
    //   - arena_fragmentation_post_mutate    fragmentation ratio
    //                                         after mutate (scaled
    //                                         0..1e6 — 0 = no
    //                                         fragmentation,
    //                                         1e6 = 100%)
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual fallback dirty-mark hook + dtor-to-shape wiring
    // + auto-compact policy from fragmentation/yield + IR cache
    // stats merge are follow-up work (each is a dedicated session
    // in arena.ixx + ShapeProfiler + ir_cache_pure + service.ixx).
    //
    // Non-duplicative with the existing ArenaStats in arena.ixx
    // (compaction_count / fragmentation_ratio / defrag_requested_
    // are *internal* aggregate metrics); #722 is the FIRST
    // observability surface that exposes Arena ↔ dirty/shape
    // integration signals as separate counters the Agent can
    // consume to decide whether to force defrag or trust the
    // auto-compact policy.
    std::atomic<std::uint64_t> arena_tier_fallbacks_total{0};
    std::atomic<std::uint64_t> arena_dtor_dirty_hooks_total{0};
    std::atomic<std::uint64_t> arena_auto_compact_triggers_total{0};
    std::atomic<std::uint64_t> arena_fragmentation_post_mutate{0};

    // Issue #723: Pass pipeline DirtyAware + Value v2 + Shape
    // history observability counters for (query:value-dispatch-stats).
    // Exposes the Pass/Value/Shape integration signals that complement
    // the existing pass_manager.ixx Wraps/Contracts, value.ixx v2
    // tagged encoding, and shape_profiler.cpp history + stability
    // logic without overlapping #658 Gaps 3/5 broad or #687
    // coercion Pass:
    //   - value_dispatch_calls_total     # of times classify /
    //                                    is_* / as_* / dispatch
    //                                    entry points were called
    //                                    (proxy for "how much value
    //                                    dispatch traffic the
    //                                    pipeline is moving")
    //   - value_unknown_tag_total        # of times classify
    //                                    encountered an unknown
    //                                    tag (bumped by value_tags.h
    //                                    contract violation path —
    //                                    proxy for "how often
    //                                    dispatch misclass happens")
    //   - value_v2_string_collisions_total
    //                                   # of v2 string collisions
    //                                    (two distinct interned
    //                                    strings hashing to the same
    //                                    value_idx — proxy for
    //                                    "how saturated the v2
    //                                    string heap is")
    //   - shape_history_shift_total      # of times the shape
    //                                    history ring buffer /
    //                                    SoA column shifted
    //                                    (proxy for "how churned
    //                                    shape classification is"
    //                                    — Agent uses this to
    //                                    decide whether to enable
    //                                    Pass short-circuit)
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual DirtyAware implementation for ConstantFoldingWrap /
    // ArityWrap / Wraps + static_asserts + Contracts expansion +
    // shape history ring buffer replacement + deopt_hook wiring
    // to JIT/service dirty scope are follow-up work (each is a
    // dedicated session in pass_manager.ixx + value.ixx +
    // value_tags.h + shape_profiler.cpp).
    //
    // Non-duplicative with the existing pass_manager.ixx Wraps
    // (static dispatch counters, no observable signals for value /
    // shape internals); #723 is the FIRST observability surface
    // that tracks Value v2 dispatch + shape history integration
    // outcomes as separate counters.
    std::atomic<std::uint64_t> value_dispatch_calls_total{0};
    std::atomic<std::uint64_t> value_unknown_tag_total{0};
    std::atomic<std::uint64_t> value_v2_string_collisions_total{0};
    std::atomic<std::uint64_t> shape_history_shift_total{0};

    // Issue #726: verification feedback-driven closed-loop self-
    // evolution reliability counters for
    // (query:closed-loop-reliability-stats). Exposes the multi-
    // round verification feedback -> mutate -> re-verify loop
    // reliability signals that complement the existing VerifyDirty
    // / VerificationDirtyReason + mark_dirty_verification helpers
    // (#437 / #469) + SEVA demo + #695/#696 stress harness +
    // #697 declarative kit without overlapping the #748
    // SV verification structure stats (which covers the structural
    // mutate + emit + dirty re-emit side; #726 covers the closed-
    // loop reliability side):
    //   - closed_loop_ref_drift_prevented_total  # of times a
    //                                            StableNodeRef
    //                                            drift across
    //                                            verification
    //                                            feedback mutate
    //                                            was prevented by
    //                                            the runtime guard
    //                                            (proxy for "how
    //                                            many silent
    //                                            ref invalidations
    //                                            the guard caught")
    //   - closed_loop_rollback_success_total    # of successful
    //                                            rollbacks on
    //                                            verification
    //                                            feedback mutate
    //                                            (MutationBoundaryGuard
    //                                            dtor + panic
    //                                            restore + epoch
    //                                            bump fired
    //                                            cleanly)
    //   - closed_loop_feedback_mutate_rounds_total
    //                                          # of feedback
    //                                            parse ->
    //                                            mutate ->
    //                                            re-verify rounds
    //                                            completed in
    //                                            the closed loop
    //                                            (proxy for "how
    //                                            many autonomous
    //                                            SEVA iterations
    //                                            the agent ran
    //                                            successfully")
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual verify:parse-coverage-feedback / parse-assert-failure
    // / parse-formal-cex / mutate:from-verification-feedback primitives
    // + closed-loop controller (seva:run-closed-loop) + enhanced
    // subtree StableNodeRef validation in MutationBoundaryGuard +
    // backend re-emit tie-in (#725) are follow-up work (each is
    // a dedicated session in evaluator_primitives_verify*.cpp or
    // new verify_primitives module + MutationBoundaryGuard + ast
    // dirty + new test harness + SEVA demo extension + docs).
    //
    // Non-duplicative with the existing #748 SV verification
    // structure stats primitive (which covers structural mutate
    // + emit + dirty re-emit); #726 is the FIRST observability
    // surface that tracks closed-loop multi-round reliability
    // outcomes (ref drift prevention + rollback success + feedback
    // mutate rounds) as separate counters the Agent can consume
    // to monitor SEVA self-evolution stability.
    std::atomic<std::uint64_t> closed_loop_ref_drift_prevented_total{0};
    std::atomic<std::uint64_t> closed_loop_rollback_success_total{0};
    std::atomic<std::uint64_t> closed_loop_feedback_mutate_rounds_total{0};

    // Issue #728: unified structured error + provenance + recovery
    // observability counters backing the (query:unified-error-stats)
    // primitive. These are public so future evaluator_primitives_*.cpp
    // refactors + new (primitive:error kind msg [context]) /
    // (with-error value handler) / (primitive:try body on-error)
    // primitives + Guard auto-capture can call them at each decision
    // point (structured error constructed / provenance captured /
    // recovery succeeded).
    //
    // Non-duplicative with the existing #478 (query:primitive-error-stats
    // pair) + #585 (query:primitives-error-stats hash with error_rate /
    // recovery_success / panic-recovery / rollback / contract-violations /
    // recommendation). #728 is the FIRST observability surface that
    // tracks the *unified model* specifically: structured ErrorValue
    // (kind + provenance + context + recovery-hint) hits as separate
    // counters, complementing #585's coarse error_rate + recovery
    // success counters without overlap.
    //
    //   - unified_error_structured_hits_total: # of times a primitive
    //                                          emitted a structured
    //                                          ErrorValue (kind +
    //                                          msg + provenance +
    //                                          context + recovery hint)
    //                                          vs. the legacy
    //                                          make_primitive_error
    //                                          string-only path.
    //                                          Proxy for "how much
    //                                          of stdlib has migrated
    //                                          to the unified model".
    //   - unified_error_provenance_captured_total: # of structured
    //                                              errors that
    //                                              successfully
    //                                              captured a
    //                                              StableNodeRef
    //                                              provenance (via
    //                                              Guard.capture()
    //                                              or direct capture
    //                                              in the error path).
    //                                              Proxy for "how
    //                                              many errors are
    //                                              introspectable for
    //                                              AI Agent recovery".
    //   - unified_error_recovery_success_total: # of successful
    //                                           recovery attempts
    //                                           (rollback + retry
    //                                           primitive path fired
    //                                           cleanly). Complements
    //                                           #585's coarse recovery
    //                                           counter with structured
    //                                           error provenance.
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual unified ErrorValue / EvalValue tagged-error extension
    // + refactor of evaluator_primitives_list.cpp / math.cpp / regex
    // / verify error sites to make_structured_primitive_error(guard,
    // kind, msg, context) + (primitive:error) / (with-error) /
    // (primitive:try) new primitives + Guard.capture auto-provenance
    // + CI lint for legacy make_primitive_error usage + new
    // tests/test_unified_primitive_error_model.cpp harness + SEVA
    // error-resilient closed-loop + primitives_style.md mandate are
    // all follow-up (each is a dedicated session in evaluator.ixx +
    // primitives_detail.h + evaluator_primitives_*.cpp + Guard +
    // diagnostic + ast.ixx StableNodeRef + new test + SEVA + docs).
    std::atomic<std::uint64_t> unified_error_structured_hits_total{0};
    std::atomic<std::uint64_t> unified_error_provenance_captured_total{0};
    std::atomic<std::uint64_t> unified_error_recovery_success_total{0};

    // Issue #731: Arena + SoA + EnvFrame concurrent compaction safety
    // counters backing the (query:arena-concurrent-compact-stats)
    // primitive. These are public so future arena.ixx + gc_coordinator +
    // evaluator_gc.cpp concurrent compact / defrag success path
    // + fiber.cpp resume() / transfer hooks + panic checkpoint
    // integration can call them at each decision point (concurrent
    // compact acquired / EnvFrame revalidation completed / panic
    // rollback fired on compact / race prevented).
    //
    // Non-duplicative with the existing #722 (query:arena-integration-stats
    // tier integration) + #743 (Arena auto-compact policy + fiber safepoint
    // + dirty/Shape closed loop) + #647 EnvFrame dual-path + #648 panic
    // checkpoint fiber + #685 auto-compact policy + #464/430/405 arena
    // compaction lifecycle + #604 Arena auto-compact fiber/GC safepoint.
    // #731 is the FIRST observability surface that tracks the *concurrent*
    // safety specifically: scheduler-safepoint coordination + EnvFrame
    // GCEnvWalkFn revalidation + panic-rollback-compact integration +
    // race prevention, as separate per-decision-point counters the
    // Agent can consume to monitor production memory stability under
    // multi-fiber steal/resume + concurrent compact.
    //
    //   - arena_concurrent_compacts_total: # of successful concurrent
    //                                       compacts (scheduled with
    //                                       safepoint coordination —
    //                                       not racing with active steals
    //                                       or env_frames_ walks).
    //                                       Proxy for "how often the
    //                                       arena can safely compact
    //                                       under fiber contention".
    //   - arena_envframe_revalidations_total: # of times an EnvId in
    //                                          env_frames_ SoA was
    //                                          revalidated post-compact
    //                                          via GCEnvWalkFn (proxy
    //                                          for "how often post-compact
    //                                          EnvFrame consistency is
    //                                          verified").
    //   - arena_panic_rollback_compact_hits_total: # of panic checkpoint
    //                                              auto-rollbacks that
    //                                              fired under a concurrent
    //                                              compact (panic restore
    //                                              path detected an
    //                                              inconsistent compact
    //                                              + triggered rollback).
    //   - arena_races_prevented_total: # of times a race was prevented
    //                                  (steal/resume vs compact race
    //                                  detected via safepoint + deferred
    //                                  to next-safe-point).
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual concurrent compact / defrag safepoint coordination
    // in arena.ixx + GCEnvWalkFn EnvFrame revalidation in evaluator_gc.cpp
    // + fiber.cpp resume() / transfer hook integration + panic checkpoint
    // snapshot integration + tests/test_arena_concurrent_compact_envframe_
    // fiber_steal.cpp harness (heavy alloc / mutate under 10+ fibers +
    // steal + periodic compact + panic injection) + #674 stress extension
    // are all follow-up (each is a dedicated session in arena.ixx +
    // gc_coordinator + evaluator_gc.cpp + fiber.cpp + panic_checkpoint +
    // new test + chaos stress + docs).
    std::atomic<std::uint64_t> arena_concurrent_compacts_total{0};
    std::atomic<std::uint64_t> arena_envframe_revalidations_total{0};
    std::atomic<std::uint64_t> arena_panic_rollback_compact_hits_total{0};
    std::atomic<std::uint64_t> arena_races_prevented_total{0};

    // Issue #655: EDSL core stability — StableNodeRef COW + tag_arity
    // delta + nested atomic rollback + children safe view + precise
    // mutate invalidation (non-duplicative with #527 stable-ref-cow,
    // #552 edsl-stability, #622 atomic-batch, #654 macro-hygiene).
    std::atomic<std::uint64_t> edsl_cow_stable_ref_remap_total{0};
    std::atomic<std::uint64_t> edsl_tag_arity_delta_patch_total{0};
    std::atomic<std::uint64_t> edsl_nested_atomic_rollback_total{0};
    std::atomic<std::uint64_t> edsl_mutate_invalidate_precision_total{0};

    // Issue #657: compiler core incremental self-mod gaps — cache bridge
    // epoch sync, impact-scope partial re-lower, JIT unhandled deopt,
    // linear metadata flow, quote fallback refresh (non-duplicative with
    // #600 incremental-closure, #680 impact_scope, #530 production-reloader).
    std::atomic<std::uint64_t> compiler_core_bridge_epoch_sync_total{0};
    std::atomic<std::uint64_t> compiler_core_jit_unhandled_invalidate_total{0};
    std::atomic<std::uint64_t> compiler_core_linear_metadata_flow_total{0};
    std::atomic<std::uint64_t> compiler_core_quote_fallback_refresh_total{0};

    // Issue #673: Unified Runtime Observability Layer (P1) — cross-module
    // correlation atomics for end-to-end production monitoring.
    //
    // The other observability primitives cover single-module stats;
    // #673 ships the FIRST dedicated correlation counters that resolve
    // cross-module events to a single signal:
    //   - runtime_observability_steal_attempt_correlated_total:
    //       Each fiber scheduler steal attempt (worker.cpp) that runs
    //       through aura_evaluator_bump_mutation_steal_attempt().
    //   - runtime_observability_steal_deferred_correlated_total:
    //       Each steal DEFERRED at an active MutationBoundary.
    //   - runtime_observability_steal_ownership_violation_correlated_total:
    //       Each linear ownership violation caught during steal.
    //
    // Non-duplicative with #591/#438/#448/#599/#592/#596.
    std::atomic<std::uint64_t> runtime_observability_steal_attempt_correlated_total{0};
    std::atomic<std::uint64_t> runtime_observability_steal_deferred_correlated_total{0};
    std::atomic<std::uint64_t> runtime_observability_steal_ownership_violation_correlated_total{0};

    // Issue #674: Closed-loop self-evolution stability stress testing
    // atomics (P0). Companion counters for the chaos stress test
    // harness that drives 1000+ mutation cycles under fiber steal +
    // GC + AOT hot-reload conditions. The 3 counters are the
    // "outcome classifier" of each chaos cycle:
    //   - self_evolution_chaos_cycles_total:
    //       Bumped by the chaos harness once per full mutation cycle
    //       (one attempted self-evolution, regardless of outcome).
    //   - self_evolution_chaos_failures_total:
    //       Bumped by the chaos harness when a chaos mutation cycle
    //       fails (post-mutation validation, rollback, or panic).
    //   - self_evolution_chaos_corruptions_total:
    //       Bumped by the chaos harness when a version/ownership
    //       mismatch is detected during a chaos cycle (the
    //       "long-running corruption detected per epoch" metric
    //       called out in the issue body).
    //
    // The primitive (query:self-evolution-chaos-stats, schema 674)
    // exposes all 3 + a cycles-total sum. Per-call derivation
    // (`chaos-failures-rate + chaos-corruptions-rate`) is derived
    // at the test/harness layer; the primitive only stores totals.
    //
    // Non-duplicative with #548 panic-checkpoint-lifecycle-stats,
    // #529 atomic-batch-rollback-stats, #527 stable-ref-cow-fiber-
    // stats, #400 mutation-rollback-coverage-stats, #679 nested-
    // Guard atomic-batch-rollback. Those cover the *production*
    // counter set; #674 covers the *stress/chaos-test* outcome
    // classifier that complements them.
    std::atomic<std::uint64_t> self_evolution_chaos_cycles_total{0};
    std::atomic<std::uint64_t> self_evolution_chaos_failures_total{0};
    std::atomic<std::uint64_t> self_evolution_chaos_corruptions_total{0};

    // Issue #661: SV InterfaceIR/ModportIR structure observability
    // (P1 EDA-SV foundation). The 3 counters track the structured
    // interface IR/ModportIR builder activity:
    //
    //   - sv_interface_ports_total:
    //       Bumped per Interface body port addition (lifetime
    //       running total). The "ports_count" counter called out
    //       in the issue body's Action #4.
    //
    //   - sv_interface_modport_views_total:
    //       Bumped per Modport view addition (lifetime running
    //       total). The "modport_views" counter from Action #4.
    //
    //   - sv_interface_direction_changes_total:
    //       Bumped per port direction change (input <-> output
    //       <-> inout). The "direction_changes" counter from
    //       Action #4. Currently bumped via the test-only
    //       helpers (and any future eda:set-port-direction
    //       primitive); the production-path mutation is a
    //       follow-up (issue body Action #3 wires `verify_dirty_`).
    //
    // Non-duplicative with #640 sv-verification-closedloop-stats,
    // #630 sv-verification-closedloop-stats-hash, #539 sv-production-
    // verification-stats, #497 sv-sva-structure-stats, #498 sv-
    // structured-edsl-stats, #496 sv-node-stats. Those cover SVA,
    // verification, and pattern scopes; #661 covers the interface
    // IR/ModportIR BUILDER-shape specifically.
    std::atomic<std::uint64_t> sv_interface_ports_total{0};
    std::atomic<std::uint64_t> sv_interface_modport_views_total{0};
    std::atomic<std::uint64_t> sv_interface_direction_changes_total{0};

    // Issue #664: SV DefUse incremental observability (P1 EDA-SV).
    // The 3 counters track the structured DefUse build + incremental
    // update activity for nested Interface/Modport scopes:
    //
    //   - sv_defuse_nested_modports_total:
    //       Bumped per DefUse build that discovers a Modport child
    //       of an Interface (i.e. a nested modport at depth >= 1).
    //       The "nested_modports" counter called out in issue body
    //       Action #3.
    //
    //   - sv_defuse_cross_refs_total:
    //       Bumped per DefUse use-record that resolves to an
    //       Interface/Modport symbol defined in another scope
    //       (cross-interface / cross-modport reference). The
    //       "cross_refs" counter from Action #3.
    //
    //   - sv_defuse_incremental_updates_total:
    //       Bumped per DefUse incremental rebuild triggered by
    //       an SV structural mutate (vs. a full rebuild). The
    //       "incremental_updates" counter from Action #3.
    //
    // Non-duplicative with #317 def-use scope tracking (the basic
    // scope-creator switch), #337 ShapeProfiler std::flat_map,
    // #640/#663 verification feedback closed loop, #691 per-fn
    // defuse index metrics. #664 covers the structured DefUse
    // INCREMENTAL + NESTED/CROSS-ref observability specifically.
    std::atomic<std::uint64_t> sv_defuse_nested_modports_total{0};
    std::atomic<std::uint64_t> sv_defuse_cross_refs_total{0};
    std::atomic<std::uint64_t> sv_defuse_incremental_updates_total{0};

    // Issue #665: SV stability observability (P1 EDA-SV scalability).
    // The 3 counters track the SV-tagged stability + scale activity
    // that the issue body Action #4 calls out (dirty_traversal_depth /
    // generation_wrap_sv / stable_ref_invalidation_sv):
    //
    //   - sv_dirty_traversal_depth_total:
    //       Cumulative depth summed across all mark_dirty_upward
    //       calls on SV-tagged nodes (Interface/Modport/SVA nodes).
    //       Use depth_total / call_count to derive the average
    //       depth called out in Action #4. Sum form (not avg) so
    //       that the primitive can be computed in O(1) without
    //       locking around a running average.
    //
    //   - sv_generation_wrap_total:
    //       Bumped each time generation_wrap_sv_hits is detected on
    //       an SV-tagged StableRef invalidation. The
    //       "generation_wrap_sv_hits" counter from Action #4.
    //
    //   - sv_stable_ref_invalidation_total:
    //       Bumped each time a StableRef tied to an SV-tagged node
    //       (Interface/Modport/SVA) becomes invalid. The
    //       "stable_ref_invalidation_sv" counter from Action #4.
    //
    // Non-duplicative with #641 StableRef cross-fiber provenance,
    // #368/#392 generation fix, #336 mark_dirty_upward fast-path,
    // #642 arena, #664 SV DefUse. #665 covers the SV-specific
    // stability observability surface.
    std::atomic<std::uint64_t> sv_dirty_traversal_depth_total{0};
    std::atomic<std::uint64_t> sv_generation_wrap_total{0};
    std::atomic<std::uint64_t> sv_stable_ref_invalidation_total{0};

    // Issue #667: List/map/filter apply hot-path observability
    // (P1 stdlib-impl performance). The 3 counters track the
    // list/map/filter apply_unary / apply_pred / apply_binary hot
    // path — the per-element lookup + closure call overhead
    // called out in issue body Action #1 + #2:
    //
    //   - primitives_apply_lookup_hits_total:
    //       Bumped per slot_lookup_fast call inside the list
    //       apply_unary/apply_pred/apply_binary helpers (the
    //       per-element fast-path). The "lookup_hits" counter
    //       from Action #4.
    //
    //   - primitives_apply_closure_calls_total:
    //       Bumped per apply_closure call inside the list apply
    //       helpers (closure path, NOT primitive path). The
    //       "closure_calls" counter from Action #4.
    //
    //   - primitives_apply_fastpath_wins_total:
    //       Bumped when the slot_lookup_fast returns a non-null
    //       PrimFn* (successful slot→fn dispatch — the fastpath
    //       actually won). The "fastpath_win" counter from
    //       Action #4.
    //
    // Non-duplicative with #479 (per-slot fastpath hit
    // breakdown), #480 PrimMeta, #614 hotpath stability,
    // #643 declarative macro, #633 demands. #667 covers the
    // LIST/MAP/FILTER apply hot-path observability surface
    // specifically (a NEW axis: the existing primitives give
    // per-primitive hit counts; #667 gives the per-element
    // apply-loop fastpath observability).
    std::atomic<std::uint64_t> primitives_apply_lookup_hits_total{0};
    std::atomic<std::uint64_t> primitives_apply_closure_calls_total{0};
    std::atomic<std::uint64_t> primitives_apply_fastpath_wins_total{0};

    // Issue #752: list/vector map/filter SoA hot-path observability
    // (P0 stdlib-impl performance; refines #727, non-duplicative
    // with #667 apply-loop counters and #506 IR SoA adoption).
    //   - list_chain_traversals_total: cdr-walk steps in map/filter/
    //     foldl hot loops (pair-chain pointer chasing cost).
    //   - list_soa_hits_total: primitive fast-dispatch wins inside
    //     map/filter/foldl (SoA-eligible / intrinsic-ready path;
    //     proxy until dedicated ListView lands).
    //   - list_intrinsic_dispatches_total: slot_lookup_fast wins
    //     in apply_unary/apply_pred/apply_binary (direct PrimFn*
    //     dispatch, bypassing apply_closure).
    //   - list_estimated_cache_misses_total: advisory miss estimate
    //     — one per chain step + one extra per closure dispatch in
    //     list hot loops (pointer-chase + callback indirection).
    std::atomic<std::uint64_t> list_chain_traversals_total{0};
    std::atomic<std::uint64_t> list_soa_hits_total{0};
    std::atomic<std::uint64_t> list_intrinsic_dispatches_total{0};
    std::atomic<std::uint64_t> list_estimated_cache_misses_total{0};

    // Issue #753: long-running deployment infra observability
    // (P0 stdlib commercial deployment; refines #729, non-duplicative
    // with #548 panic-checkpoint-lifecycle, #677 deployment-stats,
    // #674 chaos-stats). Tracks quota enforcement, checkpoint/heal
    // recovery, resource sampling, and SLO-within-quota hits.
    //   - longrunning_quota_violations_total: resource:quota-check
    //     over-limit events (memory / fibers / time axes).
    //   - longrunning_checkpoint_success_total: successful panic
    //     checkpoint save/commit on the production Guard path.
    //   - longrunning_heal_triggers_total: successful panic-restore
    //     self-heal recoveries (auto-rollback / Guard failure path).
    //   - longrunning_resource_trend_total: advisory resource samples
    //     taken via resource:quota-check (trend observability axis).
    //   - longrunning_deployment_slo_hits_total: quota-check passes
    //     while a finite limit is configured (within-SLO events).
    std::atomic<std::uint64_t> longrunning_quota_violations_total{0};
    std::atomic<std::uint64_t> longrunning_checkpoint_success_total{0};
    std::atomic<std::uint64_t> longrunning_heal_triggers_total{0};
    std::atomic<std::uint64_t> longrunning_resource_trend_total{0};
    std::atomic<std::uint64_t> longrunning_deployment_slo_hits_total{0};

    // Issue #1583 / #1207: recovery latency stall budget + SLO surface.
    // Instrumented on panic-restore and quota-reject hot paths.
    // Default stall budget 5000 µs (5 ms) — commercial zero-stall target.
    static constexpr std::size_t kLongrunningRecoveryHistBuckets = 9;
    std::atomic<std::uint64_t> longrunning_recovery_stall_budget_us{5000};
    std::atomic<std::uint64_t> longrunning_recovery_latency_us_total{0};
    std::atomic<std::uint64_t> longrunning_recovery_samples{0};
    std::atomic<std::uint64_t> longrunning_recovery_latency_us_max{0};
    std::atomic<std::uint64_t> longrunning_recovery_stall_violations_total{0};
    std::atomic<std::uint64_t> longrunning_recovery_panic_samples{0};
    std::atomic<std::uint64_t> longrunning_recovery_quota_samples{0};
    std::atomic<std::uint64_t> longrunning_recovery_histogram[kLongrunningRecoveryHistBuckets]{};

    // Issue #754: LLM-bottleneck orchestration adaptive stats
    // (P0 runtime orchestration; refines #730, non-duplicative with
    // #706 scheduler-stealbudget-adaptive-stats and #646 gc-deferral).
    //   - orchestration_llm_gc_safepoint_adapted_total: GC safepoint
    //     deferral/adaptation under active MutationBoundary (self-tuning
    //     axis when LLM-heavy workloads hold mutation guards).
    std::atomic<std::uint64_t> orchestration_llm_gc_safepoint_adapted_total{0};

    // Issue #755: end-to-end concurrent safety full-cycle integration
    // (P0 runtime integration; refines #732/#731/#730/#674/#739,
    // non-duplicative with per-component primitives).
    //   - concurrent_safety_steal_boundary_success_total: successful
    //     steal at outermost-safe MutationBoundary (probe_linear_on_steal).
    //   - concurrent_safety_aot_reload_at_guard_total: AOT checkpoint
    //     version drift detected during guard transfer / steal.
    //   - concurrent_safety_gc_safepoint_during_steal_total: GC safepoint
    //     coordination during fiber migration (resume/steal cycle).
    //   - concurrent_safety_recovery_success_total: successful panic
    //     checkpoint restore (rollback recovery on full cycle).
    std::atomic<std::uint64_t> concurrent_safety_steal_boundary_success_total{0};
    std::atomic<std::uint64_t> concurrent_safety_aot_reload_at_guard_total{0};
    std::atomic<std::uint64_t> concurrent_safety_gc_safepoint_during_steal_total{0};
    std::atomic<std::uint64_t> concurrent_safety_recovery_success_total{0};

    // Issue #668: math regex primitive error observability
    // (P1 stdlib-impl error consistency). Tracks every
    // PRIM_ERROR invocation inside the regex-match? /
    // regex-find / regex-replace / regex-split
    // primitives — both pre-try validation failures
    // (type-mismatch / OOB string index) and post-try
    // invalid-regex-syntax failures. Bumped alongside
    // the general primitive_error_count_ so the ratio
    // (regex / total) tells the AI Agent how much of the
    // primitive-error surface is regex-related.
    // Non-duplicative with #615 (PRIM_ERROR macro) — #615
    // unifies the error path shape, #668 adds the
    // per-regex-primitive observability axis (count +
    // breakdown by primitive via the query primitive).
    std::atomic<std::uint64_t> primitives_regex_error_total{0};

    // Issue #479: per-slot fast-path hit breakdown. Which
    // primitive is hottest in list/map/filter/apply hot
    // paths? The aggregate counter above only answers
    // "is the dispatch hot path hot?" — this answers
    // "which specific primitive is the bottleneck?".
    //
    // Storage: heap-allocated atomic array grown on demand.
    // We can't use std::vector<std::atomic<uint64_t>>
    // because std::atomic is neither copyable nor movable,
    // so std::vector::resize() won't compile. The pattern
    // here is the standard "manual atomic array" pattern:
    //   - pointer + capacity
    //   - grow under mutex, doubling when full
    //   - element access is a relaxed fetch_add once sized
    //
    // Slot indices are stable for the lifetime of the
    // registry (slots are assigned at primitive-registration
    // time and never reused), so the array only grows.
    // Typical capacity after warmup: ~200 slots (well under
    // the 4096 initial allocation).
    //
    // Mutex: uses std::atomic_flag rather than std::mutex
    // because this header is transitively included by .cpp
    // files in module purview (after `import std;`), and
    // mixing `#include <mutex>` with `import std;` in
    // module purview triggers "redeclaration of std::once_flag"
    // on GCC 16 modules. std::atomic_flag is in <atomic>
    // (already included above) and is the lightweight
    // primitive — perfect for the rare slow-path here.
    mutable std::atomic_flag primitive_fastpath_per_prim_lock_ = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t>* primitive_fastpath_hits_per_prim_ = nullptr;
    std::size_t primitive_fastpath_per_prim_capacity_ = 0;

    // Lazy-grow accessor. Returns a reference to the
    // per-slot atomic; callers fetch_add(1) on it.
    // Thread-safe: double-checked locking with a
    // sequenced-before fence on the writer side (the
    // mutex release) and an acquire fence on the reader
    // side (the relaxed load inside the if-check, which
    // synchronizes-with the std::atomic default-init in
    // new[] because std::atomic's default ctor is
    // sequenced-before the new[]'s return).
    //
    // Initial capacity is 256 slots (covers Aura's stdlib
    // primitives + a healthy margin for the extension kit).
    // Grows by 2× when exceeded; allocation is O(log N)
    // amortized across the lifetime of the service.
    //
    // The slow path (grow) may throw bad_alloc on OOM;
    // the fast path is noexcept. The helper that wraps
    // this in the hot list/runtime paths swallows OOM
    // gracefully since these counters are advisory.
    std::atomic<std::uint64_t>& primitive_fastpath_hits_for_slot(std::size_t slot) {
        if (slot >= primitive_fastpath_per_prim_capacity_) [[unlikely]] {
            // Spinlock: atomic_flag test_and_set returns the
            // previous state; spin until we get false (was
            // unlocked). Clear on exit. For the rare slow
            // path (grow-once-per-power-of-two) this is
            // fine; contention is essentially zero in
            // practice because grow happens during
            // primitive registration, not in steady state.
            while (primitive_fastpath_per_prim_lock_.test_and_set(std::memory_order_acquire))
                /* spin */;
            // RAII unlock on scope exit. We can't use
            // std::scoped_lock (requires <mutex>) and we
            // can't use a custom RAII helper inline without
            // polluting the header — use a try/finally
            // pattern via a lambda's destructor... or
            // just inline the unlock at every return. With
            // only one return path below, a manual unlock
            // at the end is the simplest correct approach.
            struct UnlockGuard {
                std::atomic_flag* f;
                ~UnlockGuard() { f->clear(std::memory_order_release); }
            } guard{&primitive_fastpath_per_prim_lock_};
            if (slot >= primitive_fastpath_per_prim_capacity_) {
                std::size_t new_cap = primitive_fastpath_per_prim_capacity_ == 0
                                          ? std::max<std::size_t>(256, slot + 1)
                                          : primitive_fastpath_per_prim_capacity_;
                while (new_cap <= slot)
                    new_cap *= 2;
                auto* new_arr = new std::atomic<std::uint64_t>[new_cap];
                // std::atomic default-initializes to indeterminate value
                // (NOT zero!) — explicitly zero-init the new slots.
                for (std::size_t i = 0; i < new_cap; ++i)
                    new_arr[i].store(0, std::memory_order_relaxed);
                // Carry over existing counts.
                for (std::size_t i = 0; i < primitive_fastpath_per_prim_capacity_; ++i)
                    new_arr[i].store(
                        primitive_fastpath_hits_per_prim_[i].load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
                // Publish: assign the pointer under the lock so the
                // reader-side check above sees the new array.
                // NOTE: we deliberately LEAK the old array instead
                // of delete[]ing it. A fast-path reader that has
                // already entered the if-block (slot < old_cap) is
                // about to dereference `primitive_fastpath_hits_per_prim_[slot]`
                // — if we deleted the old array under it, that
                // dereference would be UB (use-after-free). Leaking
                // is bounded: capacity doubles each grow, so total
                // leaked memory is ≤ 2× final capacity. For Aura's
                // ~200-slot stdlib + extension kit, the leak is
                // ≤ 4096 atomic<uint64_t> ≈ 32 KiB. Negligible.
                std::atomic_thread_fence(std::memory_order_release);
                primitive_fastpath_hits_per_prim_ = new_arr;
                primitive_fastpath_per_prim_capacity_ = new_cap;
            }
        }
        // Fast path: pointer is stable after first grow, capacity
        // only ever increases. The reader-side relaxed load + the
        // writer-side release fence above provide the
        // happens-before edge needed to see the new array.
        return primitive_fastpath_hits_per_prim_[slot];
    }
    // Issue #342: narrowing blame/provenance
    // observability. 1 lifetime counter: how many
    // OccurrenceInfoFlat records have been
    // populated with provenance (predicate_name +
    // source_cond_id). Pre-#342 this was 0 (the
    // provenance fields didn't exist). Post-#342
    // every analyze_predicate_flat that returns a
    // populated OccurrenceInfoFlat bumps this
    // counter. The ratio vs. total
    // narrowing_applied_total is a measure of how
    // complete the provenance is.
    std::atomic<std::uint64_t> narrowing_provenance_total{0};
    // Issue #537 / #518 Phase 2: post-mutation occurrence
    // narrowing provenance refresh observability.
    //   - occurrence_stale_refreshes_total: NarrowingRecords
    //     appended by reanalyze_occurrence_contexts
    //   - occurrence_blame_chain_complete_total: refreshes
    //     where source_mutation_id + predicate provenance
    //     are both populated (auditable blame chain)
    std::atomic<std::uint64_t> occurrence_stale_refreshes_total{0};
    std::atomic<std::uint64_t> occurrence_blame_chain_complete_total{0};
    // Issue #689: deep nested and/or/not predicate re-narrow in
    // infer_flat_partial post structural typed mutation.
    std::atomic<std::uint64_t> deep_narrow_refreshes_total{0};
    // Issue #689: provenance completeness hits (predicate_name +
    // source_cond_id + mutation_id all populated after refresh).
    std::atomic<std::uint64_t> provenance_completeness_hits_total{0};
    // Issue #639: occurrence narrowing blame + stale invalidation.
    //   - narrow_stale_caught_total: stale narrowing detected
    //     at use-site before re-analysis
    //   - narrow_blame_attached_total: diagnostics emitted with
    //     predicate provenance blame on stale narrow
    //   - narrow_invalidation_post_mutate_total: records marked
    //     stale by invalidate_narrowings_in_subtree
    //   - narrow_safe_fallback_total: Dynamic fallback when
    //     stale narrowing could not be refreshed
    std::atomic<std::uint64_t> narrow_stale_caught_total{0};
    std::atomic<std::uint64_t> narrow_blame_attached_total{0};
    std::atomic<std::uint64_t> narrow_invalidation_post_mutate_total{0};
    std::atomic<std::uint64_t> narrow_safe_fallback_total{0};
    // Issue #627: bidirectional check-mode narrow robustness.
    //   - check_mode_narrow_hits_total: Occurrence narrowing
    //     applied in check_flat If branches
    //   - synthesize_check_switch_count_total: transitions
    //     into check_flat from bidirectional paths
    //   - post_mutate_narrow_consistency_total: partial re-
    //     infer refreshed narrowing evidence after mutation
    //   - stale_check_narrow_prevented_total: stale narrowing
    //     blocked in check-mode (Dynamic fallback)
    std::atomic<std::uint64_t> check_mode_narrow_hits_total{0};
    std::atomic<std::uint64_t> synthesize_check_switch_count_total{0};
    std::atomic<std::uint64_t> post_mutate_narrow_consistency_total{0};
    std::atomic<std::uint64_t> stale_check_narrow_prevented_total{0};
    // Issue #1420 AC3: bidirectional annotation stats (4
    // counters for the (compile:bidirectional-stats) EDSL
    // primitive). The existing check_mode_narrow_hits_total
    // covers narrowing records; the new fields cover
    // annotation contract enforcement at check_flat_call
    // sites. Prefix `compile_bidirectional_` lands them in
    // the `compile` group of the engine:metrics facade
    // (#1433) via metrics_group_for_field.
    //   - compile_bidirectional_check_call_total: every
    //     entry into InferenceEngine::check_flat_call.
    //   - compile_bidirectional_annotation_pass_total:
    //     cs_.consistent_unify(inferred, expected) returned
    //     true (synth ⊆ annotation contract).
    //   - compile_bidirectional_annotation_fail_total:
    //     consistent_unify returned false AND is_coercible
    //     returned false (gradual typing can't bridge the
    //     gap; TypeError reported via diag_.).
    //   - compile_bidirectional_coercion_deferred_total:
    //     consistent_unify returned false BUT is_coercible
    //     returned true (Gradual Typing path — Issue #116
    //     deferred CoercionNode insertion; #384 first slice
    //     for annotations).
    std::atomic<std::uint64_t> compile_bidirectional_check_call_total{0};
    std::atomic<std::uint64_t> compile_bidirectional_annotation_pass_total{0};
    std::atomic<std::uint64_t> compile_bidirectional_annotation_fail_total{0};
    std::atomic<std::uint64_t> compile_bidirectional_coercion_deferred_total{0};
    // Issue #383: ConstraintSystem worklist + consistent_
    // unify observability. 3 lifetime counters:
    //   - consistent_unify_total: every call to
    //     consistent_unify (success or failure).
    //   - consistent_subtype_total: every call to
    //     consistent_subtype (success or failure).
    //   - worklist_restart_total: number of times
    //     the solver restarted the worklist after
    //     a new constraint was added during
    //     processing (the "fixpoint" stability
    //     counter). A high restart count indicates
    //     a workload that benefits from finer-
    //     grained worklist scheduling.
    std::atomic<std::uint64_t> consistent_unify_total{0};
    std::atomic<std::uint64_t> consistent_subtype_total{0};
    std::atomic<std::uint64_t> worklist_restart_total{0};
    // Issue #385: mutation-aware Let-Poly caching
    // observability. 3 lifetime counters:
    //   - poly_register_total: every call to
    //     TypeRegistry::register_forall (whether
    //     dedup hit or new entry created).
    //   - poly_dedup_hits_total: dedup hit count
    //     (the pre-#385 dedup loop returned an
    //     existing TypeId for a same-var + same-
    //     body call). Pre-#385 this wasn't
    //     surfaced; post-#385 the ratio
    //     dedup_hits / register is the cache
    //     effectiveness signal.
    //   - poly_instantiate_total: every call to
    //     TypeRegistry::instantiate_forall.
    std::atomic<std::uint64_t> poly_register_total{0};
    std::atomic<std::uint64_t> poly_dedup_hits_total{0};
    std::atomic<std::uint64_t> poly_instantiate_total{0};
    std::atomic<std::uint64_t> delta_solve_time_us{0};
    // Issue #259: type metadata propagation observability.
    // IRInstruction has a `type_id` field (0 = unknown/dynamic)
    // that the lowering pass populates for some opcodes (CastOp
    // coercions, type annotations) but not for most (Call, If,
    // Let). The full #259 scope is to wire propagation across
    // all key opcodes so the JIT can use the type info for
    // const-fold, dead-code elimination, and runtime
    // assertions. This scope-limited close ships the
    // observability foundation:
    // - ir_instructions_total: lifetime total IR instructions
    //   executed by the IR interpreter
    // - ir_instructions_with_type_total: lifetime total
    //   instructions executed where type_id != 0 (the
    //   propagation landed)
    // The derived metric type_propagation_coverage_bp is
    // computed at snapshot read time as with_type/total*10000.
    // Issue #259 AC: increase coverage (current <100% since
    // most lowering sites don't call emit_with_type today).
    std::atomic<std::uint64_t> ir_instructions_total{0};
    std::atomic<std::uint64_t> ir_instructions_with_type_total{0};
    // Issue #410: per-symbol dirty observability. The current
    // TypeChecker path uses ancestor-only dirty propagation
    // (mark_dirty_upward walks the parent_ chain — see
    // ast.ixx:3110). For a single-symbol mutation, this is
    // wasteful: only the Variable nodes that USE the changed
    // symbol need to be re-inferred, not all ancestors. The
    // per-symbol affected set is a subset of the ancestor
    // affected set. The two counters below measure how often
    // the per-symbol path is exercised (lookups_total) and
    // how many Variable nodes it returns (uses_total) — the
    // ratio of uses_total / ancestor-affected-nodes is the
    // "per-symbol reduction" that follow-up wiring (Issue
    // #410 Phase 2/2) will translate into faster
    // incremental_infer calls.
    //
    // - per_symbol_dirty_lookups_total: calls to
    //   affected_subtree_for_symbol (per call)
    // - per_symbol_dirty_uses_total: cumulative size of all
    //   per-symbol affected sets returned
    //
    // The derived metric per_symbol_dirty_reduction_bp is
    // computed at snapshot read time. It compares the
    // per-symbol uses_total against an estimate of the
    // ancestor-affected total (mark_dirty_total_nodes /
    // mark_dirty_upward_call_count = avg depth, multiplied
    // by lookups_total gives the avg ancestor-affected per
    // mutation; we report uses_total / that * 10000 as the
    // reduction basis points — higher = more savings).
    std::atomic<std::uint64_t> per_symbol_dirty_lookups_total{0};
    std::atomic<std::uint64_t> per_symbol_dirty_uses_total{0};
    // Issue #411: post-mutation auto-incremental typecheck
    // observability. The full #411 scope is making
    // infer_flat_partial the primary post-mutation path so
    // that (query:type <name>) returns up-to-date results
    // immediately after any (mutate:*) call. This
    // scope-limited close ships the wiring foundation:
    //   - incremental_typecheck_auto_invocations_total:
    //     number of typed_mutate success paths that triggered
    //     an automatic infer_flat_partial call. 0 when the
    //     mode is set to Lazy or Disabled. Equals the number
    //     of successful mutations in Eager mode (the default).
    //   - incremental_typecheck_re_inferred_total: cumulative
    //     count of nodes re-inferred across all auto-
    //     invocations. Pairs with the lifetime total on
    //     typecheck_cache_misses_total (which is a strict
    //     superset — cache_misses counts every dirty node
    //     re-inferred across the full typecheck pass too).
    //
    // The derived metric
    // incremental_typecheck_avg_re_inferred_bp (snapshot)
    // is re_inferred * 10000 / max(auto_invocations, 1) —
    // the average number of nodes re-inferred per auto-
    // invocation, in basis points. Higher = more work per
    // mutation (signal for the per-symbol follow-up).
    std::atomic<std::uint64_t> incremental_typecheck_auto_invocations_total{0};
    std::atomic<std::uint64_t> incremental_typecheck_re_inferred_total{0};
    // Issue #411 follow-up #1: per-symbol re-inference path
    // observability. The full #411 follow-up #1 scope is to
    // route post-mutation re-inference through the per-symbol
    // path (#410's affected_subtree_for_symbol) instead of
    // the ancestor-walk path. This scope-limited slice ships
    // the WIRING + observability so the optimization can be
    // measured.
    //
    //   - per_symbol_reinfer_used_total: how many mutations
    //     took the per-symbol path (the fast path). For
    //     mutate:rebind on a top-level define with N uses,
    //     this is the common case.
    //   - per_symbol_reinfer_visited_total: total nodes
    //     visited across all per_symbol invocations. The
    //     derived per_symbol_visited_avg = visited / max(used,
    //     1) tells the user the average re-inferred node
    //     count per per_symbol mutation.
    //   - ancestor_reinfer_used_total: how many mutations
    //     fell back to the ancestor walk (sub-expression
    //     mutations like mutate:replace-type that don't
    //     carry a binding sym_id).
    //   - ancestor_reinfer_visited_total: total nodes
    //     visited across all ancestor invocations. The
    //     derived ratio_bp = per_symbol_visited /
    //     (per_symbol_visited + ancestor_visited) * 10000
    //     measures the share of work that went through the
    //     fast path.
    std::atomic<std::uint64_t> per_symbol_reinfer_used_total{0};
    std::atomic<std::uint64_t> per_symbol_reinfer_visited_total{0};
    std::atomic<std::uint64_t> ancestor_reinfer_used_total{0};
    std::atomic<std::uint64_t> ancestor_reinfer_visited_total{0};
    // Issue #411 fu1 follow-up #2: per-DefUseIndex tracker
    // observability. The full #411 fu1 follow-up #2 scope
    // is to wire the per-DefUseIndex tracker into the
    // type-checker re-inference path (replace the global
    // dep_caller_fn_ lookups with PerDefUseIndexTracker
    // calls + use it in TypeChecker::infer_flat_partial's
    // per_symbol path). This scope-limited slice ships the
    // TRACKER + observability so the optimization can be
    // measured.
    //
    //   - per_defuse_index_used_total: how many mutations
    //     took the per-DefUseIndex indexed path (O(uses),
    //     via the PerDefUseIndexTracker). For top-level
    //     rebinds on hot symbols with many callers, this
    //     is the common case.
    //   - per_defuse_index_visited_total: total nodes
    //     visited across all per-DefUseIndex invocations.
    //     Pairs with the typecheck_visited counts — the
    //     speedup is the ratio of per_symbol_visited
    //     (O(n) walk) to per_defuse_index_visited (O(uses)
    //     indexed).
    //   - per_defuse_index_walk_fallback_total: how many
    //     times the per-DefUseIndex path had to fall back
    //     to the O(n) walk (the sym wasn't in the
    //     tracker). Signals the index coverage — a low
    //     fallback count means the tracker captures the
    //     common case well.
    std::atomic<std::uint64_t> per_defuse_index_used_total{0};
    std::atomic<std::uint64_t> per_defuse_index_visited_total{0};
    std::atomic<std::uint64_t> per_defuse_index_walk_fallback_total{0};
    // Issue #531: closure / EnvFrame / bridge_epoch /
    // linear_ownership_state safety observability counters.
    // Exposed via (query:closure-env-safety-stats) primitive.
    // Stats-only (relaxed-ordering).
    //   - closure_stale_refresh_count_  (# of stale
    //     IRClosure refreshes triggered by invalidate_if_stale
    //     — the AI Agent reads this to measure the
    //     closure-refresh frequency post-mutate)
    //   - bridge_epoch_hit_count_       (# of bridge_epoch
    //     match checks that succeeded — the closure was
    //     fresh, no refresh needed)
    //   - linear_check_pass_count_      (# of linear
    //     ownership_state runtime checks that passed — a
    //     pass means the Linear* op proceeded with its
    //     fast path; failure triggers deopt)
    //   - gc_envframe_stale_skipped_    (# of GCEnvWalkFn
    //     visits that skipped a stale EnvFrame — production
    //     expects this to be 0; > 0 means a COW/compaction
    //     or version mismatch was caught at GC time)
    std::atomic<std::uint64_t> closure_stale_refresh_count_{0};
    std::atomic<std::uint64_t> bridge_epoch_hit_count_{0};
    std::atomic<std::uint64_t> linear_check_pass_count_{0};
    std::atomic<std::uint64_t> gc_envframe_stale_skipped_{0};
    // Issue #305: TypeId/TypeScheme propagation observability
    // counters (EDA hardware optimization / synthesis track).
    // Exposed via (compile:type-propagation-stats) primitive.
    // Stats-only (relaxed-ordering).
    //   - type_propagation_runs_       (# of TypePropagationPass
    //     invocations — measures how often the pass is on the
    //     optimization pipeline hot path)
    //   - type_propagation_total_       (# of instructions whose
    //     type_id was propagated by the pass — cumulative)
    //   - type_propagation_unknown_     (# of instructions whose
    //     type_id == 0 (unknown) the pass could NOT propagate)
    //   - type_propagation_int_width_   (# of integers whose
    //     inferred bit-width (8/16/32/64) was used by a downstream
    //     pass — the EDA backend key metric for resource alloc)
    std::atomic<std::uint64_t> type_propagation_runs_{0};
    std::atomic<std::uint64_t> type_propagation_total_{0};
    std::atomic<std::uint64_t> type_propagation_unknown_{0};
    std::atomic<std::uint64_t> type_propagation_int_width_{0};
    // Issue #1530: extended TypePropagationPass opcode coverage +
    // CastOp elision win-rate observability.
    //   - type_propagation_extended_ops_total: stamps applied on the
    //     #1530 extended set (Eq/Lt/Gt/Le/Ge/MakePair/Move/Borrow/
    //     LinearWrap/CellGet) + #1874 expansion
    //   - cast_elision_win_rate_bp: latest pipeline win rate in basis
    //     points (0..10000) = eliminated / max(1, castop_emitted +
    //     eliminated) * 10000 after TypeProp + DCE
    std::atomic<std::uint64_t> type_propagation_extended_ops_total{0};
    std::atomic<std::uint64_t> cast_elision_win_rate_bp{0};
    // Issue #1874: TypePropagation fixpoint + DCE synergy.
    //   - type_propagation_fixpoint_rounds: sum of rounds used
    //     across blocks (≤16 per block after #1874)
    //   - cast_eliminated_after_propagation: DCE eliminations
    //     counted when DCE runs after TypeProp in the same pipeline
    //     step (zero-overhead win after mutation re-lower)
    std::atomic<std::uint64_t> type_propagation_fixpoint_rounds{0};
    std::atomic<std::uint64_t> cast_eliminated_after_propagation{0};
    // Issue #1884: TypePropagation/DCE ↔ TypedMutationAudit correlation.
    // Mirrored from typed_audit process counters for engine:metrics dashboards.
    std::atomic<std::uint64_t> type_propagation_invariant_correlation_total{0};
    std::atomic<std::uint64_t> type_propagation_invariant_pass_with_evidence_total{0};
    std::atomic<std::uint64_t> type_propagation_invariant_fail_with_evidence_total{0};
    std::atomic<std::uint64_t> type_propagation_evidence_lost_total{0};
    std::atomic<std::uint64_t> predicate_memo_evict_invariant_correlation_total{0};
    // Issue #1887: test strategy hot-path coverage (mirrored from test_strategy.h).
    std::atomic<std::uint64_t> test_strategy_total_hits{0};
    std::atomic<std::uint64_t> test_strategy_coverage_hit_rate_bp{0};
    std::atomic<std::uint64_t> test_strategy_self_mod_loops{0};
    // Issue #306: hardware resource linear-ownership
    // observability counters (EDA track — wire/reg/mem/port
    // borrow + double-drive detection). Exposed via the
    // (query:linear-ownership-stats) primitive. Stats-only
    // (relaxed-ordering). Production expectation: 0 double-
    // drive + 0 port-conflict + low borrow rate. > 0 on
    // either error category = diagnostic alert.
    //   - hw_resource_wire_borrows_      (# of Wire resource
    //     borrows issued by the lowerer)
    //   - hw_resource_reg_writes_       (# of Reg resource
    //     writes issued by the lowerer)
    //   - hw_resource_mem_access_      (# of Mem resource
    //     accesses issued by the lowerer)
    //   - hw_resource_double_drive_    (# of double-drive
    //     violations caught at compile time — should be 0
    //     in correct hardware code; > 0 = EDA bug)
    std::atomic<std::uint64_t> hw_resource_wire_borrows_{0};
    std::atomic<std::uint64_t> hw_resource_reg_writes_{0};
    std::atomic<std::uint64_t> hw_resource_mem_access_{0};
    std::atomic<std::uint64_t> hw_resource_double_drive_{0};
    // Issue #610: linear ownership post-mutation validation +
    // runtime enforcement observability. Exposed via
    // (query:linear-ownership-mutation-stats) primitive.
    //   - linear_post_mutate_revalidations_total: OwnershipEnv
    //     re-validate runs on dirty linear bindings after mutate
    //   - linear_violations_caught_total: use-after-move,
    //     double-borrow, invalid-state notes from re-validate
    //   - linear_deopt_on_invalidate_total: invalidate_function
    //     forced JIT/closure refresh after linear-site mutate
    //   - linear_leak_prevented_total: leaked-linear bindings
    //     caught before eval (compile-time guard)
    std::atomic<std::uint64_t> linear_post_mutate_revalidations_total{0};
    std::atomic<std::uint64_t> linear_violations_caught_total{0};
    std::atomic<std::uint64_t> linear_deopt_on_invalidate_total{0};
    std::atomic<std::uint64_t> linear_leak_prevented_total{0};
    // Issue #638: linear ownership + GuardShape runtime safety
    // enforcement (interpreter/JIT hot path post-mutate).
    // Exposed via (query:linear-ownership-safety-stats).
    //   - linear_post_mutate_enforcements_total: GuardShape /
    //     Linear* ops with non-zero linear_ownership_state
    //   - linear_deopt_on_mismatch_total: shape/ownership
    //     mismatch triggered deopt or hard error
    std::atomic<std::uint64_t> linear_post_mutate_enforcements_total{0};
    std::atomic<std::uint64_t> linear_deopt_on_mismatch_total{0};
    // Issue #1595: Fiber::join / MultiFiberMailbox / parallel_intend
    // linear + StableNodeRef enforcement (query:join-linear-enforcement-stats).
    //   - linear_join_enforcement_total: join / post-task refresh probes
    //   - mailbox_linear_violation_count: recv/push rejected for linear claim
    //   - stable_ref_post_join_repin_total: StableNodeRef restamps on join path
    std::atomic<std::uint64_t> linear_join_enforcement_total{0};
    std::atomic<std::uint64_t> mailbox_linear_violation_count{0};
    std::atomic<std::uint64_t> stable_ref_post_join_repin_total{0};
    // Issue #1879: orch-path StableNodeRef / steal / linear counters
    // (also mirrored on OrchModuleStats for process-wide orch dashboards).
    std::atomic<std::uint64_t> orch_stable_ref_auto_refresh_total{0};
    std::atomic<std::uint64_t> orch_fiber_steal_provenance_enforced_total{0};
    std::atomic<std::uint64_t> orch_linear_violation_prevented_total{0};
    // Issue #740: linear ownership safety in JIT L2 hot paths
    // post-invalidate (Arena/DropOp/GC root re-sync).
    // Exposed via (query:linear-jit-safety-stats).
    std::atomic<std::uint64_t> linear_jit_post_invalidate_total{0};
    std::atomic<std::uint64_t> linear_jit_arena_forced_post_mutate_total{0};
    std::atomic<std::uint64_t> linear_jit_drop_op_emitted_total{0};
    std::atomic<std::uint64_t> linear_jit_gc_root_resync_total{0};
    // Issue #683: linear ownership + GC safepoint / fiber-steal /
    // post-re-lower revalidate integration.
    // Exposed via (query:linear-ownership-gc-stats).
    std::atomic<std::uint64_t> linear_gc_safepoint_violations{0};
    std::atomic<std::uint64_t> linear_steal_enforced{0};
    std::atomic<std::uint64_t> linear_relower_revalidate_hits{0};
    // Issue #688: infer_flat_partial OwnershipEnv post-mutate revalidate.
    std::atomic<std::uint64_t> linear_dirty_revalidate_count{0};
    // Issue #1531: escape analysis + OwnershipEnv dirty revalidate.
    //   - linear_escape_reanalysis_total: AST escape re-analysis runs
    //     after dirty ownership validation (typed_mutate path)
    //   - ownership_dirty_revalidate_hits: dirty bindings actually
    //     scanned by escape re-analysis
    //   - linear_escape_while_borrowed_total / escape_after_move_total
    //     violation counters
    //   - ir_escape_analysis_runs_total / ir_escape_slots_marked_total
    //     IR EscapeAnalysisPass observability
    std::atomic<std::uint64_t> linear_escape_reanalysis_total{0};
    std::atomic<std::uint64_t> ownership_dirty_revalidate_hits{0};
    std::atomic<std::uint64_t> linear_escape_while_borrowed_total{0};
    std::atomic<std::uint64_t> linear_escape_after_move_total{0};
    // Issue #1875: post-mutation linear validation coverage.
    //   - linear_post_mutation_checks_total: post_mutation_invariant_check runs
    //   - linear_post_mutation_hits_total: runs that performed ownership/escape
    //     validation on at least one linear binding (dirty or full)
    //   - linear_post_mutation_validation_hit_rate: 0–100 =
    //     hits * 100 / max(1, checks)
    //   - linear_post_mutation_full_validate_total: full re-sim path runs
    //   - escape_analysis_dirty_reruns_total: EscapeAnalysisPass dirty-mode
    std::atomic<std::uint64_t> linear_post_mutation_checks_total{0};
    std::atomic<std::uint64_t> linear_post_mutation_hits_total{0};
    std::atomic<std::uint64_t> linear_post_mutation_validation_hit_rate{0};
    std::atomic<std::uint64_t> linear_post_mutation_full_validate_total{0};
    std::atomic<std::uint64_t> escape_analysis_dirty_reruns_total{0};
    std::atomic<std::uint64_t> linear_ir_use_after_move_total{0};
    std::atomic<std::uint64_t> linear_ir_double_consume_total{0};
    std::atomic<std::uint64_t> ir_escape_analysis_runs_total{0};
    std::atomic<std::uint64_t> ir_escape_slots_marked_total{0};
    // Issue #747: linear + Occurrence predicate-branch safety under typed mutate.
    std::atomic<std::uint64_t> linear_occurrence_revalidate_hits_total{0};
    std::atomic<std::uint64_t> linear_occurrence_escape_prevented_total{0};
    std::atomic<std::uint64_t> linear_occurrence_predicate_safe_total{0};
    // Issue #748: SV verification EDSL structured mutate + dirty re-emit closed-loop.
    std::atomic<std::uint64_t> sv_verification_structure_mutate_hits_total{0};
    std::atomic<std::uint64_t> sv_verification_dirty_reemit_total{0};
    // Issue #750: Runtime reflection schema validation for macro/EDSL mutate (refines #734).
    std::atomic<std::uint64_t> reflection_schema_validated_total{0};
    std::atomic<std::uint64_t> reflection_schema_violations_total{0};
    std::atomic<std::uint64_t> reflection_stale_validation_prevented_total{0};
    std::atomic<std::uint64_t> reflection_macro_provenance_held_total{0};
    // Issue #1611: reflect.hh MacroIntroduced hygiene gate (post_mutation
    // + auto_validate_with_marker path). Exposed via
    // query:reflect-postmutate-stats schema 1611.
    std::atomic<std::uint64_t> reflect_macro_hygiene_checks_total{0};
    std::atomic<std::uint64_t> reflect_macro_hygiene_rejects_total{0};
    // Issue #688: graceful safe-fallback on GC/fiber linear probe violation.
    std::atomic<std::uint64_t> linear_typed_mutate_safe_fallbacks{0};

    // Issue #444: strategy evolution controller pheromone
    // counters. Each strategy (coverage-greedy /
    // bug-fix-priority / minimal-mutation) tracks its
    // own hits + successes; the controller consults
    // success_rate = successes / max(1, hits) to decide
    // when to escalate (random → directed → formal-
    // assisted). All atomic, relaxed ordering (stats-
    // only).
    //
    //   - strategy_greedy_hits:          # of times
    //     coverage-greedy strategy was selected
    //   - strategy_greedy_successes:      # of greedy
    //     attempts that improved coverage
    //   - strategy_bugfix_hits:          # of bug-fix-
    //     priority strategy selections
    //   - strategy_bugfix_successes:     # of bug-fix
    //     attempts that fixed a failure
    //   - strategy_minimal_hits:         # of minimal-
    //     mutation strategy selections
    //   - strategy_minimal_successes:     # of minimal
    //     attempts that improved a per-spec metric
    //   - strategy_escalations:           # of times
    //     the controller escalated (e.g. random →
    //     directed) due to coverage plateau
    std::atomic<std::uint64_t> strategy_greedy_hits{0};
    std::atomic<std::uint64_t> strategy_greedy_successes{0};
    std::atomic<std::uint64_t> strategy_bugfix_hits{0};
    std::atomic<std::uint64_t> strategy_bugfix_successes{0};
    std::atomic<std::uint64_t> strategy_minimal_hits{0};
    std::atomic<std::uint64_t> strategy_minimal_successes{0};
    std::atomic<std::uint64_t> strategy_escalations{0};

    // Issue #441 (rolled into #450 / Primitive-perf-stats):
    // single counter for total primitive dispatch events.
    // Hot-path counter: bumped on every (primitive-func)
    // dispatch from evaluator_eval_flat.cpp line ~2789.
    // Used by (query:primitive-perf-stats) to report the
    // overall primitive call rate. Per-primitive breakdown
    // (which primitives are hottest) is a follow-up —
    // requires a vector of atomics indexed by slot, which
    // is invasive to the Primitives class.
    std::atomic<std::uint64_t> primitive_call_total{0};

    // Issue #614: primitives hot-path memory-stability counters.
    //   - pair_alloc_total:       # of pairs.push_back calls in
    //                             evaluator_primitives_list.cpp
    //                             (list / append / reverse / map /
    //                             filter / member foldl). Each
    //                             push_back is one Pair allocation
    //                             in the evaluator's pairs_ vector.
    //   - linear_traverse_total: # of cdr-walk steps across
    //                             length / list-ref / member / foldl.
    //                             O(n) per cdr hop where n is the
    //                             list depth; exposes pair-chain cost
    //                             under AI multi-round mutate.
    //   - cdr_depth_max:         longest single linear traverse
    //                             observed (high-water mark). Useful
    //                             for detecting pathological list
    //                             chains in production agent loops.
    std::atomic<std::uint64_t> pair_alloc_total{0};
    std::atomic<std::uint64_t> linear_traverse_total{0};
    std::atomic<std::uint64_t> cdr_depth_max{0};

    // Issue #452: AOT hot-update + region filtering
    // observability. Each counter is a per-bridge-event
    // monotonic total readable via (query:aot-stats).
    // - aot_stale_reject_count_: bumped when
    //   aura_reload_aot_module rejects a binary because
    //   the binary's aot_emit_version != host's expected
    //   version. Signals version drift.
    // - aot_region_mismatch_: bumped when
    //   aura_reload_aot_module rejects a binary because
    //   the binary's region_filter_mask doesn't match
    //   the host's region. Multi-agent safety signal.
    //   #452 ships the counter; region_filter wiring is
    //   a follow-up.
    // - aot_hot_update_success_: bumped on successful
    //   dlopen + version check (the constructor ran).
    std::atomic<std::uint64_t> aot_stale_reject_count_{0};
    std::atomic<std::uint64_t> aot_region_mismatch_{0};
    std::atomic<std::uint64_t> aot_hot_update_success_{0};

    // Issue #785: AOT concurrent hot-update observability
    // (concurrent steal + grace period + EnvFrame version
    // sync). 3 NEW atomics for the
    // (query:aot-concurrent-hotupdate-stats) primitive.
    // - aot_concurrent_steal_during_reload_total: # of
    //   work-steal attempts deferred because the victim
    //   fiber was in AOT apply or the reload refcount
    //   swap was in progress (Phase 2+ to wire into
    //   is_stealable + WorkerThread::steal).
    // - aot_grace_period_hits_total: # of times the
    //   grace period was triggered to allow in-flight
    //   apply_closure / JIT GuardShape to see consistent
    //   func_table (Phase 2+ to wire into
    //   aura_reload_aot_module before/after swap).
    // - aot_env_version_sync_on_reload_total: # of
    //   times EnvFrame::version_ was bumped on reload
    //   to coordinate with cross-fiber mutation (Phase
    //   2+ to wire into reload decision + EnvFrame
    //   sync). Bumped from
    //   Evaluator::bump_aot_env_version_sync_on_reload().
    std::atomic<std::uint64_t> aot_concurrent_steal_during_reload_total{0};
    std::atomic<std::uint64_t> aot_grace_period_hits_total{0};
    std::atomic<std::uint64_t> aot_env_version_sync_on_reload_total{0};
    // Issue #708: AOT hot-reload refcount swap + checkpoint version drift.
    std::atomic<std::uint64_t> aot_reload_attempts_{0};
    // Issue #1366: reloads requested via Aura (aot:reload) primitive
    std::atomic<std::uint64_t> aot_reload_attempts_via_primitive{0};
    std::atomic<std::uint64_t> aot_reload_success_via_primitive{0};
    // Issue #1367: per-evaluator AotState map telemetry
    std::atomic<std::uint64_t> aot_per_eval_state_creates{0};
    std::atomic<std::uint64_t> aot_per_eval_state_clears{0};
    std::atomic<std::uint64_t> aot_per_eval_region_sets{0};
    // Issue #1369: per-function version probe reported stale
    std::atomic<std::uint64_t> aot_fn_version_probe_stale_total{0};
    std::atomic<std::uint64_t> aot_refcount_swaps_{0};
    std::atomic<std::uint64_t> aot_deopt_on_steal_{0};
    std::atomic<std::uint64_t> aot_concurrent_safe_reloads_{0};
    std::atomic<std::uint64_t> aot_checkpoint_version_drifts_{0};
    // Issue #653: bridge_epoch vs func-table epoch mismatch on
    // restore_post_yield_or_rollback / fiber resume validate.
    std::atomic<std::uint64_t> aot_bridge_epoch_mismatches_{0};
    // Issue #1905: AOT incremental hot-update / invalidation loop
    // closure (#1046). Each counter bumps at one of the 5 plan
    // steps. Surfaced by (engine:metrics "query:aot-hot-update-stats")
    // + the scripts/check_aot_hot_update_coverage.py CI linter.
    //   - aot_live_closure_refresh_on_mutation_total: every
    //     aura_refresh_live_closures_for_mutated_define call from
    //     flush_mutation_boundary outermost exit (Step 2 of #1905 plan).
    //   - aot_live_closure_refresh_on_steal_total: every refresh
    //     call from complete_post_resume_steal_refresh (Step 3).
    //   - aot_bridge_epoch_bump_on_mutation_total: every bridge_epoch
    //     bump driven by an outermost MutationBoundaryGuard exit.
    //   - aot_bridge_epoch_bump_on_steal_total: every bridge_epoch
    //     bump driven by a fiber resume / steal.
    //   - aot_region_mismatch_on_resume_total: every region_mask
    //     drift detected on resume (per-eval AotState mismatch).
    //   - aot_stale_deopt_on_steal_total: every aura_jit_closure_record_stale_deopt
    //     call driven by a stolen fiber's resume path (vs the
    //     original on-AOT path which the existing jit_closure_stale_deopt_total
    //     already covers).
    std::atomic<std::uint64_t> aot_live_closure_refresh_on_mutation_total{0}; // #1905
    std::atomic<std::uint64_t> aot_live_closure_refresh_on_steal_total{0};    // #1905
    std::atomic<std::uint64_t> aot_bridge_epoch_bump_on_mutation_total{0};    // #1905
    std::atomic<std::uint64_t> aot_bridge_epoch_bump_on_steal_total{0};       // #1905
    std::atomic<std::uint64_t> aot_region_mismatch_on_resume_total{0};        // #1905
    std::atomic<std::uint64_t> aot_stale_deopt_on_steal_total{0};             // #1905

    // Issue #732: AOT hot-reload safe-swap at MutationBoundary
    // observability counter backing the
    // (query:aot-safe-swap-boundary-stats) primitive. This is
    // public so future aura_jit_bridge.cpp aura_reload_aot_module
    // + MutationBoundaryGuard outermost exit hook + fiber.cpp
    // resume() / transfer hooks can call it at the safe-swap
    // decision point (reload successfully fired at outermost
    // MutationBoundary).
    //
    // Non-duplicative with the existing #708 (query:aot-reload-stats
    // high-level 5-7 field reload summary — attempts / success /
    // stale / refcount_swaps / region_violations / deopt-on-steal /
    // concurrent-safe-reloads) + #644 (query:aot-reload-func-table-
    // stats enforcement-track with ref-bump / ref-decrement /
    // region-reapply) + #590 (query:aot-hotupdate-stats 3 atomics).
    // #732 is the FIRST observability surface that tracks the
    // *safe-swap at MutationBoundary* specifically — i.e., reloads
    // that fired at the outermost safe-swap point (NOT mid-mutation),
    // as the per-decision-point signal the Agent consumes to monitor
    // safe-swap adoption rate + zero-downtime orchestration quality.
    //
    //   - aot_safe_boundary_hits_total: # of AOT reloads that fired
    //                                   at outermost MutationBoundary
    //                                   safe-swap point (the
    //                                   "right place" — no concurrent
    //                                   fiber has unsaved mutation
    //                                   state, all func_table entries
    //                                   are reachable). Proxy for
    //                                   "how often reload landed at
    //                                   a true safe point vs. was
    //                                   deferred / raced".
    //
    // Phase 1 ships the counter + bump helper + the primitive.
    // The actual atomic func_table refcount swap protocol in
    // aura_jit_bridge.cpp aura_reload_aot_module + per-region
    // isolation enforcement on reload + aura_aot_request_safe_reload()
    // API + MutationBoundaryGuard outermost exit hook + GraceEpoch
    // defer-old-decrement after grace period + tests/test_aot_hot_swap_
    // refcount_region_guard_safe.cpp harness (multi-agent different
    // regions + AOT emit + mutate + concurrent apply + reload at
    // boundary) + #674 concurrent stress integration + docs are
    // all follow-up (each is a dedicated session in aura_jit_bridge.cpp
    // + MutationBoundaryGuard + fiber.cpp + new test + chaos stress +
    // docs).
    std::atomic<std::uint64_t> aot_safe_boundary_hits_total{0};

    // Issue #733: macro marker propagation + IR/JIT hygiene
    // enforcement counters backing the (query:ir-marker-hygiene-stats)
    // primitive. These are public so future lowering_impl.cpp +
    // emit paths + ir_soa.ixx + aura_jit.cpp + aura_jit_runtime.cpp
    // + ir_executor.ixx can call them at each decision point
    // (IRInstruction creation from AST node propagates marker /
    // IRFunction marker set from root AST marker / marker-loss
    // detected at hot path / JIT conservative policy applied on
    // MacroIntroduced / marker-propagation successful across all
    // emit sites).
    //
    // Non-duplicative with #714 (query:self-evolution-closedloop-
    // stats — ref drift + rollback success + feedback mutate rounds)
    // + #455 (ir marker snapshot — internal mechanics, no
    // observability surface) + #373 (mutate hygiene guard —
    // flat.is_macro_introduced internal check). #733 is the FIRST
    // observability surface that tracks the *marker propagation +
    // IR/JIT enforcement* specifically as separate per-decision-
    // point counters the Agent consumes to monitor hygiene fidelity
    // across the entire compile/execution pipeline (macro expand →
    // AST → lowering → IR → JIT hot-path → Interpreter).
    //
    //   - ir_marker_user_instrs_total: # of IRInstructions created
    //                                  with marker=User (proxy for
    //                                  "how much IR traffic is
    //                                  user-authored").
    //   - ir_marker_macro_introduced_instrs_total: # of IRInstructions
    //                                              created with
    //                                              marker=MacroIntroduced
    //                                              (proxy for "how
    //                                              much IR traffic
    //                                              is macro-authored
    //                                              — the hygiene
    //                                              scope").
    //   - ir_marker_loss_events_total: # of times marker propagation
    //                                  failed at emit path (e.g. a
    //                                  closure body / PrimCall arg /
    //                                  linear op / cached define path
    //                                  that did not copy AST marker
    //                                  → IR source_marker / IRFunction
    //                                  marker). Proxy for "how many
    //                                  macro-introduced sub-exprs
    //                                  lost their hygiene marker
    //                                  through the pipeline".
    //   - ir_hygiene_jit_violations_prevented_total: # of times the JIT
    //                                                conservative policy
    //                                                fired on a
    //                                                MacroIntroduced
    //                                                source_marker
    //                                                (prevented
    //                                                aggressive deopt-
    //                                                elide / respected
    //                                                hygiene in
    //                                                closure capture /
    //                                                forced Interpreter
    //                                                fallback or extra
    //                                                epoch check).
    //                                                Proxy for "how
    //                                                often the JIT
    //                                                hot-path consults
    //                                                marker + applies
    //                                                conservative
    //                                                policy".
    //   - ir_hygiene_marker_propagation_hits_total: # of times marker
    //                                              propagation
    //                                              succeeded across
    //                                              all emit sites
    //                                              (closure / PrimCall
    //                                              / linear / cached
    //                                              define paths all
    //                                              copied AST marker
    //                                              → IR source_marker
    //                                              / IRFunction
    //                                              marker via
    //                                              propagate_marker_
    //                                              from_ast helper).
    //                                              Proxy for "how
    //                                              often the hygiene
    //                                              marker survives
    //                                              the pipeline".
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual propagate_marker_from_ast helper in lowering_impl.cpp
    // + ir_soa.ixx marker_ column + aura_jit.cpp + aura_jit_runtime.cpp
    // + ir_executor.ixx conservative policy on source_marker==
    // MacroIntroduced + IRFunction creation marker-from-root-AST-
    // marker in service/lowering + tests/test_macro_marker_propagation_
    // ir_jit_post_mutate.cpp harness (define macro that introduces
    // lambda + mutate inside it under fiber + JIT hot path) + #674
    // stress integration + SEVA macro-heavy cases are all follow-up
    // (each is a dedicated session in lowering_impl.cpp + ir_soa.ixx
    // + aura_jit.cpp + aura_jit_runtime.cpp + ir_executor.ixx + new
    // test + chaos stress + SEVA demo + docs).
    std::atomic<std::uint64_t> ir_marker_user_instrs_total{0};
    std::atomic<std::uint64_t> ir_marker_macro_introduced_instrs_total{0};
    std::atomic<std::uint64_t> ir_marker_loss_events_total{0};
    std::atomic<std::uint64_t> ir_hygiene_jit_violations_prevented_total{0};
    std::atomic<std::uint64_t> ir_hygiene_marker_propagation_hits_total{0};

    // Issue #735: MacroIntroduced provenance in StableNodeRef +
    // targeted dirty/rollback for macro subtrees observability
    // counters backing the (query:macro-provenance-stats) primitive.
    // These are public so future ast.ixx StableNodeRef + make_ref +
    // MutationBoundaryGuard + mark_dirty_upward + evaluator_primitives_
    // mutate.cpp can call them at each decision point
    // (StableNodeRef captured with macro_introduced_at_capture +
    // original_macro_expansion_id populated / dirty propagation
    // targeted to macro-subtree / rollback success on macro subtree
    // hygiene drift detected / is_macro_introduced hot-path consult).
    //
    // Non-duplicative with #714 (query:self-evolution-closedloop-
    // stats ref drift + rollback + feedback mutate rounds) + #717
    // (query:fiber-boundary-violation-stats — fiber/Guard boundary
    // invariants) + #392 (subtree gen — internal subtree mechanism)
    // + #373 (mutate hygiene guard — flat.is_macro_introduced internal
    // check) + the existing #733 (query:ir-marker-hygiene-stats — IR-
    // level marker propagation) + #750 (query:reflection-schema-stats
    // — runtime reflection validate).
    // #735 is the FIRST observability surface that tracks the
    // *MacroIntroduced provenance + targeted macro-subtree handling*
    // specifically — capture-time provenance in StableNodeRef,
    // targeted dirty/rollback for macro subtrees, dirty-impact on
    // macro-subtree count, rollback success rate — as separate
    // per-decision-point counters the Agent consumes to monitor
    // precise macro handling in long-running self-evo loops.
    //
    //   - macro_provenance_captured_total: # of times StableNodeRef
    //                                      capture populated
    //                                      macro_introduced_at_capture
    //                                      + original_macro_expansion_id
    //                                      fields (proxy for "how
    //                                      often provenance is
    //                                      tracked on capture").
    //   - macro_provenance_is_macro_introduced_total: # of times the
    //                                                  is_macro_
    //                                                  introduced
    //                                                  hot-path
    //                                                  consult fired
    //                                                  on a StableRef
    //                                                  (proxy for
    //                                                  "how often the
    //                                                  macro check
    //                                                  actually
    //                                                  fires at hot
    //                                                  path").
    //   - macro_provenance_dirty_impact_total: # of dirty propagations
    //                                          targeted to macro
    //                                          subtree (via
    //                                          original_macro_
    //                                          expansion_id) instead
    //                                          of whole subtree.
    //                                          Proxy for "how often
    //                                          we avoid over-
    //                                          invalidation via
    //                                          provenance-aware
    //                                          dirty".
    //   - macro_provenance_rollback_success_total: # of successful
    //                                            rollback that
    //                                            preserved macro
    //                                            marker during
    //                                            restore_children
    //                                            (proxy for "how
    //                                            often targeted
    //                                            macro-subtree
    //                                            rollback fired
    //                                            cleanly").
    //
    // Phase 1 ships the counters + bump helpers + the primitive.
    // The actual ast.ixx StableNodeRef + macro_introduced_at_capture
    // + original_macro_expansion_id fields + is_valid_subtree
    // macro_provenance_check + MutationBoundaryGuard +
    // rollback_macro_subtree_provenance + mark_dirty_upward targeted
    // macro-subtree + dirty/epoch interaction strengthening
    // (verify/macro dirty cascade respect MacroIntroduced provenance
    // for incremental re-lower) + StableRef / hygiene stats
    // correlation enhancement + tests/test_macro_provenance_stable_
    // ref_rollback_self_evo.cpp harness (nested macro expand +
    // multi-round mutate:rebind inside macro body under fiber steal
    // / panic / Guard fail) + SEVA macro cases + #674 chaos stress
    // integration + docs are all follow-up (each is a dedicated
    // session in ast.ixx + mutate.cpp + evaluator_primitives_mutate.cpp
    // + new test + SEVA demo + chaos stress + docs).
    std::atomic<std::uint64_t> macro_provenance_captured_total{0};
    std::atomic<std::uint64_t> macro_provenance_is_macro_introduced_total{0};
    std::atomic<std::uint64_t> macro_provenance_dirty_impact_total{0};
    std::atomic<std::uint64_t> macro_provenance_rollback_success_total{0};

    // ── Issues #923–#940: Stdlib Production Review domain ──
    // Compact counters so Agent dashboards can gate production readiness.
    std::atomic<std::uint64_t> stdlib_list_iterative_sorts_total{0};     // #923
    std::atomic<std::uint64_t> stdlib_orch_fiber_safe_registry_total{0}; // #924
    std::atomic<std::uint64_t> stdlib_error_validation_total{0};         // #925
    std::atomic<std::uint64_t> stdlib_primmeta_tier_queries_total{0};    // #926
    std::atomic<std::uint64_t> stdlib_bench_runs_total{0};               // #927
    std::atomic<std::uint64_t> stdlib_iterative_fold_total{0};           // #928
    std::atomic<std::uint64_t> stdlib_llm_rate_limit_blocks_total{0};    // #929
    std::atomic<std::uint64_t> stdlib_unit_test_runs_total{0};           // #930
    std::atomic<std::uint64_t> stdlib_schema_typecheck_total{0};         // #931
    std::atomic<std::uint64_t> stdlib_registry_domain_peels_total{0};    // #932
    std::atomic<std::uint64_t> stdlib_fiber_mutation_audit_total{0};     // #933
    std::atomic<std::uint64_t> stdlib_aot_hotupdate_total{0};            // #934
    std::atomic<std::uint64_t> stdlib_e2e_workload_total{0};             // #935
    std::atomic<std::uint64_t> stdlib_self_evo_safety_total{0};          // #936
    std::atomic<std::uint64_t> stdlib_reflect_edsl_patch_total{0};       // #937
    std::atomic<std::uint64_t> stdlib_macro_provenance_total{0};         // #938
    std::atomic<std::uint64_t> stdlib_edsl_hygiene_audit_total{0};       // #939
    std::atomic<std::uint64_t> stdlib_reflect_type_schema_total{0};      // #940
    std::atomic<std::uint64_t> stdlib_production_review_active{1};

    // ── Issues #941–#954: Self-evo / compiler-core pipeline ──
    std::atomic<std::uint64_t> selfevo_dirty_observer_hooks_total{0}; // #941
    std::atomic<std::uint64_t> selfevo_pattern_index_hits_total{0};   // #942
    std::atomic<std::uint64_t> selfevo_composite_tx_total{0};         // #943
    std::atomic<std::uint64_t> selfevo_provenance_refresh_total{0};   // #944
    std::atomic<std::uint64_t> selfevo_linear_enforce_total{0};       // #945/#951
    std::atomic<std::uint64_t> selfevo_instr_dirty_total{0};          // #946/#950
    std::atomic<std::uint64_t> selfevo_closure_bridge_sync_total{0};  // #947/#952
    std::atomic<std::uint64_t> selfevo_jit_parity_checks_total{0};    // #948/#953
    std::atomic<std::uint64_t> selfevo_stress_suite_runs_total{0};    // #949
    std::atomic<std::uint64_t> selfevo_tree_walker_fallback_total{0}; // #954
    std::atomic<std::uint64_t> selfevo_pipeline_active{1};

    // ── Issues #955–#967: serve / bugfix domain ──
    std::atomic<std::uint64_t> ir_cache_v2_evictions_total{0};        // #959
    std::atomic<std::uint64_t> session_registry_unregisters_total{0}; // #955
    std::atomic<std::uint64_t> bugfix_batch_941_967_active{1};

    // ── Issues #985–#1013: cache bounds + production hardening ──
    std::atomic<std::uint64_t> cache_specjit_evictions_total{0};     // #985
    std::atomic<std::uint64_t> cache_shape_evictions_total{0};       // #992
    std::atomic<std::uint64_t> cache_jit_unhandled_erases_total{0};  // #993
    std::atomic<std::uint64_t> cache_adt_cap_clears_total{0};        // #994
    std::atomic<std::uint64_t> bounded_lru_template_active{1};       // #995
    std::atomic<std::uint64_t> resource_quota_checks_total{0};       // #1013
    std::atomic<std::uint64_t> resource_quota_rejects_total{0};      // #1013
    std::atomic<std::uint64_t> resource_quota_max_fibers{256};       // #1013
    std::atomic<std::uint64_t> resource_quota_max_mutations{100000}; // #1013
    // Issue #1618: production ResourceQuota manager AC surface
    // (query:resource-quota-stats schema 1618). Typed rejects are
    // ResourceQuotaExceeded (not PanicCheckpoint generic panic).
    //   - quota_violation_total: all dim rejects (alias-friendly)
    //   - mutation_budget_rejected_total: mutation-dim rejects
    //   - quota_reject_typed_total: AuraResult typed path (not panic)
    //   - panic_quota_distinguished_total: classified as quota≠panic
    //   - manager_enforce_total: ResourceQuotaManager::check_and_consume
    std::atomic<std::uint64_t> quota_violation_total{0};           // #1618
    std::atomic<std::uint64_t> mutation_budget_rejected_total{0};  // #1618
    std::atomic<std::uint64_t> quota_reject_typed_total{0};        // #1618
    std::atomic<std::uint64_t> panic_quota_distinguished_total{0}; // #1618
    std::atomic<std::uint64_t> manager_enforce_total{0};           // #1618
    // Issue #1628: MutationBoundaryGuard::try_acquire path counters
    // (typed ResourceQuotaExceeded — not PanicCheckpoint).
    std::atomic<std::uint64_t> mutation_guard_try_acquire_total{0};
    std::atomic<std::uint64_t> mutation_guard_try_acquire_reject_total{0};
    std::atomic<std::uint64_t> production_hardening_985_1013_active{1};

    // ── Issues #1014–#1046: production stability + bugfix batch ──
    std::atomic<std::uint64_t> production_stability_1014_1046_active{1};
    std::atomic<std::uint64_t> rebind_validation_fail_returns_total{0}; // #1019
    std::atomic<std::uint64_t> sandbox_admin_denials_total{0};          // #1020
    // Issue #1876: runtime sandbox + capability enforcement observability.
    //   - sandbox_violations_total: denials while sandbox/effect mode active
    //   - capability_denials_by_effect: OR of required effect bits ever denied
    //   - capability_denial_mutate_total / capability_denial_ffi_total: per-effect
    //   - sandbox_provenance_records_total: record_mutation under sandbox allow
    //   - sandbox_provenance_invalid_total: is_valid_full tenant/gen mismatch
    std::atomic<std::uint64_t> sandbox_violations_total{0};
    std::atomic<std::uint64_t> capability_denials_by_effect{0};
    std::atomic<std::uint64_t> capability_denial_mutate_total{0};
    std::atomic<std::uint64_t> capability_denial_ffi_total{0};
    std::atomic<std::uint64_t> sandbox_provenance_records_total{0};
    std::atomic<std::uint64_t> sandbox_provenance_invalid_total{0};
    std::atomic<std::uint64_t> dirty_subtree_bfs_walks_total{0};   // #1036
    std::atomic<std::uint64_t> ir_marker_stats_queries_total{0};   // #1039
    std::atomic<std::uint64_t> ir_cache_v2_lru_evictions_total{0}; // #1042
    std::atomic<std::uint64_t> serve_health_slo_active{1};         // #1015
    std::atomic<std::uint64_t> panic_guard_lifecycle_active{1};    // #1014

    // ── Issues #1047–#1071: hygiene / type / mutate safety batch ──
    std::atomic<std::uint64_t> production_safety_1047_1071_active{1};
    std::atomic<std::uint64_t> hw_coercion_empty_str_fixed{1};  // #1050
    std::atomic<std::uint64_t> mutation_history_void_fixed{1};  // #1054
    std::atomic<std::uint64_t> query_where_dedup_fixed{1};      // #1070
    std::atomic<std::uint64_t> eval_string_bounds_fixed{1};     // #1071
    std::atomic<std::uint64_t> hygiene_marker_phase1_active{1}; // #1047/#1049
    std::atomic<std::uint64_t> guard_fiber_phase1_active{1};    // #1061–#1063

    // ── Issues #1072–#1096: security / metrics / concurrency batch ──
    std::atomic<std::uint64_t> production_hardening_1072_1096_active{1};
    std::atomic<std::uint64_t> http_shell_injection_fixed{1};    // #1077
    std::atomic<std::uint64_t> recovery_pct_clamped{1};          // #1079
    std::atomic<std::uint64_t> compaction_efficiency_clamped{1}; // #1080
    std::atomic<std::uint64_t> ast_ref_get_meta_tags{1};         // #1076
    std::atomic<std::uint64_t> pass_pipeline_yield_counter{1};   // #1085
    std::atomic<std::uint64_t> remap_func_ids_base0_fixed{1};    // #1089
    std::atomic<std::uint64_t> mutate_string_bounds_bulk{1};     // #1082

    // ── Issues #1097–#1122: serialize / fold / serve safety ──
    std::atomic<std::uint64_t> production_safety_1097_1122_active{1};
    std::atomic<std::uint64_t> eval_async_heap_result{1};    // #1097
    std::atomic<std::uint64_t> const_fold_bool_tag_fixed{1}; // #1098
    std::atomic<std::uint64_t> const_fold_block_clear{1};    // #1099
    std::atomic<std::uint64_t> reflect_bounds_checks{1};     // #1101+
    std::atomic<std::uint64_t> cache_header_validate_ext{1}; // #1104
    std::atomic<std::uint64_t> open_cache_ir_bounds{1};      // #1102

    // ── Issues #1123–#1140: final open-issue sweep ──
    std::atomic<std::uint64_t> production_sweep_1123_1140_active{1};
    std::atomic<std::uint64_t> equal_zero_nil_fixed{1};           // #1137
    std::atomic<std::uint64_t> format_void_on_error{1};           // #1138
    std::atomic<std::uint64_t> term_metric_double_count_fixed{1}; // #1135/#1136
    std::atomic<std::uint64_t> defuse_rebuild_monotonic{1};       // #1129
    std::atomic<std::uint64_t> module_realpath_fail_closed{1};    // #1131
    std::atomic<std::uint64_t> env_parent_fallback_fixed{1};      // #1128

    // ── Issues #1144–#1148: observability wire-up / dead-bump audit ──
    std::atomic<std::uint64_t> production_sweep_1144_1148_active{1};
    std::atomic<std::uint64_t> flat_hash_insert_helper{1};  // #1144
    std::atomic<std::uint64_t> selfevo_hyg_dirty_wired{1};  // #1145
    std::atomic<std::uint64_t> per_fiber_ex_state_wired{1}; // #1146
    std::atomic<std::uint64_t> orch_telemetry_wired{1};     // #1147
    std::atomic<std::uint64_t> dead_bump_audit_script{1};   // #1148

    // ── Issues #1158–#1176: math UB + IO security + stdlib review ──
    std::atomic<std::uint64_t> production_sweep_1158_1176_active{1};
    std::atomic<std::uint64_t> math_int64_ub_fixed{1};      // #1158/#1159/#1174
    std::atomic<std::uint64_t> http_get_no_shell{1};        // #1160
    std::atomic<std::uint64_t> git_stage_no_shell{1};       // #1161
    std::atomic<std::uint64_t> file_path_deny_list{1};      // #1163-1165
    std::atomic<std::uint64_t> file_cap_checks_extended{1}; // #1162/#1171-1173
    std::atomic<std::uint64_t> stdlib_review_phase1{1};     // #1166-1170/#1176
    std::atomic<std::uint64_t> renderer_module_scaffold{1}; // #1175

    // ── Issues #1177–#1201: render/FFI/security/orchestration Phase 1 ──
    std::atomic<std::uint64_t> production_sweep_1177_1201_active{1};
    std::atomic<std::uint64_t> ffi_hot_path_scaffold{1};           // #1177
    std::atomic<std::uint64_t> zero_copy_framebuffer_supported{1}; // #1178
    // Issue #1561: Arena-backed zero-copy present path metrics
    std::atomic<std::uint64_t> zero_copy_arena_alloc_bytes{0};
    std::atomic<std::uint64_t> zero_copy_hit_in_render{0};
    std::atomic<std::uint64_t> zero_copy_arena_path_active{1};
    std::atomic<std::uint64_t> zero_copy_arena_acquire_count{0};
    std::atomic<std::uint64_t> render_dirty_aware_scaffold{1};    // #1179/#1186
    std::atomic<std::uint64_t> security_core_modules_scaffold{1}; // #1180
    std::atomic<std::uint64_t> ansi_helper_supported{1};          // #1181
    std::atomic<std::uint64_t> render_ffi_scaffold{1};            // #1182
    // #1354: render FFI registry + c-* hot path
    std::atomic<std::uint64_t> render_ffi_registered{0};
    std::atomic<std::uint64_t> render_ffi_hot_path_dispatches{0};
    std::atomic<std::uint64_t> render_ffi_hotpath_enter_total{0};
    std::atomic<std::uint64_t> render_ffi_bind_success{0};
    // #1355: render-aware lightweight mutation checkpoints
    std::atomic<std::uint64_t> mutation_lightweight_total{0};
    std::atomic<std::uint64_t> mutation_lightweight_commit_total{0};
    std::atomic<std::uint64_t> mutation_lightweight_rollback_total{0};
    std::atomic<std::uint64_t> mutation_lightweight_frame_commit_total{0};
    // #1356: tier-based primitive dispatch (HotTierTable)
    std::atomic<std::uint64_t> prim_hot_tier_active{1};
    std::atomic<std::uint64_t> prim_hot_table_size{0};
    std::atomic<std::uint64_t> prim_hot_dispatch_hits{0};
    std::atomic<std::uint64_t> prim_hot_dispatch_hits_render{0};
    std::atomic<std::uint64_t> prim_cold_dispatch_fallback{0};
    // #1357: render prim latency + frame time histogram
    std::atomic<std::uint64_t> render_telemetry_active{1};
    std::atomic<std::uint64_t> render_prim_latency_samples{0};
    std::atomic<std::uint64_t> render_prim_latency_total_ns{0};
    std::atomic<std::uint64_t> render_frame_time_samples{0};
    std::atomic<std::uint64_t> render_frame_time_total_ns{0};
    std::atomic<std::uint64_t> tenant_principal_scaffold{1}; // #1183/#1191
    // Issue #1566: multi-tenant workspace isolation enforcement mirrors
    std::atomic<std::uint64_t> tenant_boundary_violation_prevented_total{0};
    std::atomic<std::uint64_t> cross_tenant_provenance_deny_total{0};
    std::atomic<std::uint64_t> tenant_boundary_checks_total{0};
    std::atomic<std::uint64_t> cross_tenant_capability_grant_total{0};
    std::atomic<std::uint64_t> render_memory_profiling_supported{1}; // #1184
    std::atomic<std::uint64_t> provenance_rollback_scaffold{1};      // #1185
    std::atomic<std::uint64_t> capability_effects_scaffold{1};       // #1187/#1192
    // Issue #1565: capability effect enforcement mirrors (process-wide)
    std::atomic<std::uint64_t> capability_effect_enforced_total{0};
    std::atomic<std::uint64_t> capability_effect_denied_total{0};
    std::atomic<std::uint64_t> capability_provenance_mismatch_total{0};
    std::atomic<std::uint64_t> capability_effect_grant_total{0};
    std::atomic<std::uint64_t> capability_effect_check_total{0};
    std::atomic<std::uint64_t> render_ci_slo_scaffold{1};          // #1188
    std::atomic<std::uint64_t> mutation_audit_tenant_scaffold{1};  // #1189
    std::atomic<std::uint64_t> render_obs_schema_scaffold{1};      // #1190/#1193
    std::atomic<std::uint64_t> hotpath_contract_gates_scaffold{1}; // #1194
    std::atomic<std::uint64_t> seva_closed_loop_scaffold{1};       // #1195
    std::atomic<std::uint64_t> panic_quota_checkpoint_scaffold{1}; // #1196
    std::atomic<std::uint64_t> instruction_dirty_short_circuit{1}; // #1197
    std::atomic<std::uint64_t> fiber_join_structured{1};           // #1198
    std::atomic<std::uint64_t> aura_result_migration_scaffold{1};  // #1199
    std::atomic<std::uint64_t> mailbox_multi_fiber_scaffold{1};    // #1200
    std::atomic<std::uint64_t> optimization_passes_registry{1};    // #1201

    // ── Issues #1202–#1228: orchestration / heal / memory / observability Phase 1 ──
    std::atomic<std::uint64_t> production_sweep_1202_1228_active{1};
    std::atomic<std::uint64_t> parallel_orch_scaffold{1};            // #1202
    std::atomic<std::uint64_t> self_healing_hooks_active{1};         // #1203
    std::atomic<std::uint64_t> pure_analysis_pass_asserts{1};        // #1204
    std::atomic<std::uint64_t> agent_fiber_safepoint_wired{1};       // #1205
    std::atomic<std::uint64_t> dirty_propagation_module{1};          // #1206
    std::atomic<std::uint64_t> recovery_stall_budget_scaffold{1};    // #1207
    std::atomic<std::uint64_t> orch_agent_metrics_scaffold{1};       // #1208
    std::atomic<std::uint64_t> shape_render_fingerprint_scaffold{1}; // #1209
    std::atomic<std::uint64_t> longrunning_fiber_obs_scaffold{1};    // #1210
    std::atomic<std::uint64_t> multi_fiber_mailbox_typed{1};         // #1211
    std::atomic<std::uint64_t> seva_self_opt_loop_scaffold{1};       // #1212
    std::atomic<std::uint64_t> mutation_audit_wal_scaffold{1};       // #1213
    // Issue #1567: mutation audit WAL persist / crash recovery mirrors
    std::atomic<std::uint64_t> audit_record_persisted_total{0};
    std::atomic<std::uint64_t> audit_wal_replay_count{0};
    std::atomic<std::uint64_t> audit_crash_recovery_success{0};
    std::atomic<std::uint64_t> audit_wal_bytes_written{0};
    std::atomic<std::uint64_t> arena_moving_defrag_scaffold{1};   // #1214
    std::atomic<std::uint64_t> production_health_slo_scaffold{1}; // #1215
    std::atomic<std::uint64_t> typed_mutation_audit_pass{1};      // #1216
    std::atomic<std::uint64_t> envframe_version_propagate{1};     // #1217
    std::atomic<std::uint64_t> ir_region_effect_annotations{1};   // #1218
    std::atomic<std::uint64_t> slo_self_heal_triggers{1};         // #1219
    std::atomic<std::uint64_t> closure_bridge_epoch_scaffold{1};  // #1220
    std::atomic<std::uint64_t> hotpath_contract_test_gates{1};    // #1221
    std::atomic<std::uint64_t> metrics_prometheus_scaffold{1};    // #1222
    std::atomic<std::uint64_t> gc_ffi_root_registration{1};       // #1223
    std::atomic<std::uint64_t> dead_metric_detection_scaffold{1}; // #1224
    std::atomic<std::uint64_t> arena_stats_per_fiber{1};          // #1225
    std::atomic<std::uint64_t> lifetime_pin_scaffold{1};          // #1226
    std::atomic<std::uint64_t> hot_path_primitives_module{1};     // #1227
    std::atomic<std::uint64_t> eda_parse_common_dedup{1};         // #1228

    // ── Issues #1229–#1240: EDA/FFI/agent security + verification Phase 1 ──
    std::atomic<std::uint64_t> production_sweep_1229_1240_active{1};
    std::atomic<std::uint64_t> eda_hash_table_creates_total{0};      // #1229
    std::atomic<std::uint64_t> eda_alloc_bytes_total{0};             // #1229
    std::atomic<std::uint64_t> ffi_opaque_tracking_hardened{1};      // #1230
    std::atomic<std::uint64_t> stdlib_hotpath_eda_ffi_dashboard{1};  // #1231
    std::atomic<std::uint64_t> agent_capability_gates{1};            // #1232
    std::atomic<std::uint64_t> sv_verification_executor_scaffold{1}; // #1233
    std::atomic<std::uint64_t> stable_node_ref_eda_scaffold{1};      // #1234
    std::atomic<std::uint64_t> covergroup_sampling_scaffold{1};      // #1235
    std::atomic<std::uint64_t> synthesize_json_escape_fixed{1};      // #1236
    std::atomic<std::uint64_t> eda_commercial_sim_scaffold{1};       // #1237
    std::atomic<std::uint64_t> sva_semantic_eval_scaffold{1};        // #1238
    std::atomic<std::uint64_t> panic_checkpoint_raii_scaffold{2};    // #1239 / #1363 phase 2
    std::atomic<std::uint64_t> value_tag_consteval_contracts{1};     // #1240

    // ── Issues #1241–#1245: SoAView / arena / hygiene concurrent Phase 1 ──
    std::atomic<std::uint64_t> production_sweep_1241_1245_active{1};
    std::atomic<std::uint64_t> soa_view_concept_enforced{1}; // #1241
    // Issue #1517: SoAView pipeline enforcement + EDSL migration progress
    // (mirrors pass_manager / soa_view atomics for Agent query surfaces).
    std::atomic<std::uint64_t> concept_enforcement_hits_total{0};    // #1517
    std::atomic<std::uint64_t> soa_view_pass_skipped_total{0};       // #1517
    std::atomic<std::uint64_t> edsl_soa_migration_progress_total{0}; // #1517
    std::atomic<std::uint64_t> soa_view_hits_total{0};               // #1517 mirror
    std::atomic<std::uint64_t> soa_view_misses_total{0};             // #1517 mirror
    // Issue #1520: children_ columnar + region dense lookup surface.
    std::atomic<std::uint64_t> children_column_soa_hits_total{0}; // #1520
    std::atomic<std::uint64_t> pcv_pin_count_total{0};            // #1520
    // Issue #1624: PCV / pmr DOD migration observability
    // (soa_dod_migration_progress, pcv_columnar_hit_rate_bp).
    std::atomic<std::uint64_t> soa_dod_migration_progress_total{0}; // #1624
    std::atomic<std::uint64_t> pcv_columnar_hit_rate_bp{0};         // #1624
    std::atomic<std::uint64_t> region_dense_hits_total{0};          // #1520
    std::atomic<std::uint64_t> map_indirection_miss_total{0};       // #1520 (region map fallback)
    std::atomic<std::uint64_t> arena_shrink_tier_hardened{1};       // #1242
    std::atomic<std::uint64_t> soa_view_eval_helpers{1};            // #1243
    std::atomic<std::uint64_t> hygiene_ir_marker_propagation{1};    // #1244
    std::atomic<std::uint64_t> macro_clone_concurrent_hygiene{1};   // #1245

    // ── Issues #1246–#1250: reflect/hygiene/agent-OOB/provenance Phase 1 ──
    std::atomic<std::uint64_t> production_sweep_1246_1250_active{1};
    std::atomic<std::uint64_t> runtime_reflect_bridge_guard{1};          // #1246
    std::atomic<std::uint64_t> runtime_reflect_mutated_schema_checks{0}; // #1246
    std::atomic<std::uint64_t> macro_origin_provenance_errors{0};        // #1247
    std::atomic<std::uint64_t> hygiene_tracer_expansions{0};             // #1248
    std::atomic<std::uint64_t> hygiene_tracer_depth_max{0};              // #1248
    std::atomic<std::uint64_t> agent_string_heap_bounds_hardened{1};     // #1249
    std::atomic<std::uint64_t> stable_ref_auto_pin_total{0};             // #1250
    // Issue #1647: StableNodeRef auto-refresh observability pairing —
    // bumped when validate_or_refresh / refresh_if_stale succeeds on a
    // cross-boundary (cross-COW / cross-sub-workspace / cross-fiber)
    // ref. Pairs with #1250's stable_ref_auto_pin_total (pin-time) so
    // the (query:stable-ref-stats) surface can expose both pin-time
    // and refresh-success counts. Distinct from the existing
    // bump_stable_ref_cross_cow_refresh legacy per-Fiber counter
    // (which counts pin-time, not refresh-success).
    std::atomic<std::uint64_t> cross_boundary_auto_refresh_success_total{0};
    std::atomic<std::uint64_t> stable_ref_full_path_enforced{1}; // #1250

    // ── Issues #1251–#1255: dirty/Guard/steal/pattern Phase 1 ──
    std::atomic<std::uint64_t> production_sweep_1251_1255_active{1};
    std::atomic<std::uint64_t> mark_dirty_bounds_enforced{1};             // #1251
    std::atomic<std::uint64_t> rollback_compaction_path{1};               // #1251
    std::atomic<std::uint64_t> mutation_boundary_primitives_wrapped{0};   // #1252
    std::atomic<std::uint64_t> mutation_boundary_linear_revalidations{0}; // #1252
    std::atomic<std::uint64_t> mutation_boundary_steal_recoveries{0};     // #1252
    // ── Issue #1907: reflect/EDSL bridge counters backing the
    // (engine:metrics "query:reflect-schema") + (mutate:validate-reflected)
    // primitives + the post-mutation auto_validate + hygiene gate hook.
    //   - reflect_post_mutation_validate_total: every bridge-hook call
    //     from flush_mutation_boundary outermost exit (Step 1 of #1907).
    //   - reflect_post_mutation_validate_fail_total: subset where the
    //     auto_validate pass returns false (validation failure).
    //   - reflect_hygiene_macro_reject_total: subset where the
    //     SyntaxMarker::MacroIntroduced gate rejects (no explicit
    //     allow_macro_evolution + dirty_macro_nodes > 0).
    //   - reflect_schema_query_total: every (query:reflect-schema) call
    //     (Step 2 of #1907).
    //   - reflect_validate_reflected_query_total: every
    //     (mutate:validate-reflected) call (Step 2 of #1907).
    //   - reflect_dirty_macro_nodes_total: cumulative sum of
    //     dirty_macro_nodes reported by the bridge hook (trending
    //     metric for self-evolution hygiene regression detection).
    std::atomic<std::uint64_t> reflect_post_mutation_validate_total{0};      // #1907
    std::atomic<std::uint64_t> reflect_post_mutation_validate_fail_total{0}; // #1907
    std::atomic<std::uint64_t> reflect_hygiene_macro_reject_total{0};        // #1907
    std::atomic<std::uint64_t> reflect_schema_query_total{0};                // #1907
    std::atomic<std::uint64_t> reflect_validate_reflected_query_total{0};    // #1907
    std::atomic<std::uint64_t> reflect_dirty_macro_nodes_total{0};           // #1907
    // Issue #1904: legacy mutate:* primitives still using manual
    // std::unique_lock<std::shared_mutex> on workspace_mtx_ + explicit
    // defuse_version_ fetch_add instead of MutationBoundaryGuard RAII.
    // Goal of #1904: drive this counter to 0 by migrating every legacy
    // site to Guard. Surfaced by
    // (engine:metrics "query:mutation-guard-coverage") + the
    // scripts/check_legacy_mutate_lock.py CI linter.
    std::atomic<std::uint64_t> mutation_legacy_manual_lock_total{0}; // #1904
    std::atomic<std::uint64_t> mutation_hold_duration_us_total{0};   // #1253
    std::atomic<std::uint64_t> mutation_hold_samples{0};             // #1253
    std::atomic<std::uint64_t> mutation_hold_duration_us_max{0};     // #1253
    std::atomic<std::uint64_t> mutation_too_long_total{0};           // #1253
    // Issue #1443: long-mutation policy knobs + starvation prevention counter.
    // Reads in dtor are racy by design (best-effort policy, not safety critical).
    // Threshold default 500ms (500'000 µs); max_extreme default 30s (strict-mode ceiling).
    std::atomic<std::uint64_t> long_mutation_threshold_us{500'000}; // #1443
    std::atomic<std::uint64_t> max_extreme_mutation_us{30'000'000}; // #1443
    std::atomic<std::uint64_t> starvation_prevented_count{0};       // #1443
    std::atomic<std::uint64_t> long_mutation_strict_mode{0};        // #1443 (0/1)
    std::atomic<std::uint64_t> long_mutation_extreme_total{0};      // #1443
    std::atomic<std::uint64_t> last_long_mutation_fiber_id{0};      // #1443 (telemetry)
    std::atomic<std::uint64_t> last_long_mutation_duration_us{0};   // #1443 (telemetry)
    // Issue #1373: cross-fiber yield + hold observability (Agent dashboard)
    //   - yield_same_thread: yield while boundary held, same OS thread
    //   - cross_thread_migration: yield checkpoint resume on different thread
    //   - yield_rollback: restore_post_yield_or_rollback forced failure path
    //   - hold_time_total_us / holds_total: dual of #1253 samples (aliased writes)
    //   - holds_over_1ms: holds longer than 1000µs (short-tail SLO probe)
    std::atomic<std::uint64_t> mutation_boundary_yield_same_thread_total{0};      // #1373
    std::atomic<std::uint64_t> mutation_boundary_cross_thread_migration_total{0}; // #1373
    std::atomic<std::uint64_t> mutation_boundary_yield_rollback_total{0};         // #1373
    std::atomic<std::uint64_t> mutation_boundary_hold_time_total_us{0};           // #1373
    std::atomic<std::uint64_t> mutation_boundary_holds_total{0};                  // #1373
    std::atomic<std::uint64_t> mutation_boundary_holds_over_1ms_total{0};         // #1373
    // Issue #1375: 9-bucket hold-time histogram (µs boundaries):
    //   0: 0–100us   1: 100–500us  2: 500us–1ms
    //   3: 1–5ms     4: 5–10ms     5: 10–50ms
    //   6: 50–100ms  7: 100ms–1s   8: >1s
    static constexpr std::size_t kMutationBoundaryHoldHistBuckets = 9;
    std::atomic<std::uint64_t>
        mutation_boundary_hold_histogram[kMutationBoundaryHoldHistBuckets]{}; // #1375
    std::atomic<std::uint64_t> steal_inner_boundary_hardened{1};              // #1254
    std::atomic<std::uint64_t> pattern_hygiene_strict_enforced{1};            // #1255
    std::atomic<std::uint64_t> pattern_hygiene_violations_caught{0};          // #1255
    std::atomic<std::uint64_t> defuse_incremental_updates_total{0};           // #1255
    std::atomic<std::uint64_t> defuse_full_rebuild_fallbacks_total{0};        // #1255
    std::atomic<std::uint64_t> pattern_hygiene_defuse_sync_on_guard{0};       // #1255

    // ── Issues #1256–#1260: GC/workspace/IR/mutate-guard/panic Phase 1 ──
    std::atomic<std::uint64_t> production_sweep_1256_1260_active{1};
    std::atomic<std::uint64_t> gc_safepoint_mutation_metrics{1}; // #1256
    // Issue #1364: mutation × GC safepoint observability (benign race telemetry)
    std::atomic<std::uint64_t> mutation_in_safepoint_total{0};                // #1364
    std::atomic<std::uint64_t> safepoint_yield_on_mutation_total{0};          // #1364
    std::atomic<std::uint64_t> safepoint_collision_total{0};                  // #1364
    std::atomic<std::uint64_t> workspace_provenance_auto_remapped{0};         // #1257
    std::atomic<std::uint64_t> workspace_cross_layer_validations_on_merge{0}; // #1257
    std::atomic<std::uint64_t> workspace_merge_mismatch_prevented{0};         // #1257
    std::atomic<std::uint64_t> ir_soa_cache_consistency_enforced{1};          // #1258
    std::atomic<std::uint64_t> ir_soa_cache_reset_epoch_bumps{0};             // #1258
    std::atomic<std::uint64_t> mutate_guard_enforced{0};                      // #1259
    std::atomic<std::uint64_t> naked_mutate_attempt{0};                       // #1259
    std::atomic<std::uint64_t> panic_transfer_on_steal{0};                    // #1260
    std::atomic<std::uint64_t> panic_transfer_failed{0};                      // #1260
    // Issue #1446: nested boundary + steal + GC compact re-pin telemetry.
    // panic_transfer_nested_success: successful panic-checkpoint transfer
    //   across nested Guard boundaries (depth > 1) after steal/GC compact.
    // cow_repin_on_steal: bumped when re_pin_cow_children_from_snapshot()
    //   re-validates pinned StableNodeRef / COW children after a steal.
    // checkpoint_lost_on_compact: target 0; any non-zero value indicates a
    //   panic-checkpoint that was destroyed by GC compact without being
    //   re-pinned (memory-safety event).
    std::atomic<std::uint64_t> panic_transfer_nested_success{0};   // #1446
    std::atomic<std::uint64_t> cow_repin_on_steal{0};              // #1446
    std::atomic<std::uint64_t> checkpoint_lost_on_compact{0};      // #1446
    std::atomic<std::uint64_t> panic_checkpoint_steal_hardened{1}; // #1260

    // Issue #1637: panic checkpoint lifecycle hardening (post-steal /
    // post-compact / post-hot-swap closed-loop). Three path-specific
    // counters let dashboards distinguish which event triggered the
    // restore; the two outcome counters aggregate "did the recovery
    // actually rebuild state" (cross_fiber_panic_heal_success) and
    // "was the steal made safe while a boundary was still held"
    // (mutation_boundary_steal_safe_total — pair with the existing
    // mutation_boundary_cross_thread_migration_total from #1373).
    std::atomic<std::uint64_t> post_steal_checkpoint_restore_total{0};    // #1637
    std::atomic<std::uint64_t> post_compact_checkpoint_restore_total{0};  // #1637
    std::atomic<std::uint64_t> post_hot_swap_checkpoint_restore_total{0}; // #1637
    std::atomic<std::uint64_t> cross_fiber_panic_heal_success{0};         // #1637
    std::atomic<std::uint64_t> mutation_boundary_steal_safe_total{0};     // #1637

    // Issue #1638: SoA EnvFrame dual-path (bindings_ vs bindings_symid_
    // + bindings_linear_ownership_state_) full-path version stamping +
    // stale detection + mutation_log compact. Three counters cover
    // the three concrete gaps:
    //   - dual_path_stale_fallback_total: bumped when a lookup / walk
    //     / GC / JIT path detects a stale EnvFrame (version drift vs.
    //     defuse_version) and falls back to the symid path / rebuild.
    //     Steady-state target 0; any non-zero value indicates a
    //     missed dual-path consistency check.
    //   - mutation_log_compact_bytes_saved: cumulative bytes reclaimed
    //     by mutation_log compact at boundary exit. Heavy mutation
    //     workloads reclaim 200MB+/day when the threshold is engaged;
    //     zero values mean compact never fired.
    //   - env_frame_version_drift_prevented: bumped when a stale
    //     EnvFrame was detected and prevented from being used (the
    //     positive control for dual_path_stale_fallback — count
    //     detections and preventions together to verify parity).
    std::atomic<std::uint64_t> dual_path_stale_fallback_total{0};    // #1638
    std::atomic<std::uint64_t> mutation_log_compact_bytes_saved{0};  // #1638
    std::atomic<std::uint64_t> env_frame_version_drift_prevented{0}; // #1638

    // Issue #1639: per-block dirty bitmask → partial re-lower wiring
    // (refine #1474 / #1495 / #1505 / #1514 / #1555 / #1601 / #1605).
    // Existing `incremental_relower_blocks_total` (above) tracks the
    // cumulative # of blocks re-lowered via partial path. The 5 new
    // metrics complete the spec's observability surface:
    //   - full_relower_count: bumped when relower_define_blocks falls
    //     back to full re-lower (8+ dirty blocks or no impact_scope
    //     data). Pair with the existing relower_full_called_count
    //     for dual-write observability.
    //   - dirty_block_ratio_numerator_total + denominator_total:
    //     running sums of (dirty_block_hits, dirty_block_hits +
    //     relower_blocks_saved) per call so dashboards compute
    //     dirty_block_ratio = numerator / denominator.
    //   - relower_block_hit_rate_numerator_total + denominator_total:
    //     running sums of (incremental_relower_blocks,
    //     incremental_relower_blocks + full_relower_count) per call.
    //     The hit rate = incremental / (incremental + full).
    std::atomic<std::uint64_t> full_relower_count{0};                       // #1639
    std::atomic<std::uint64_t> dirty_block_ratio_numerator_total{0};        // #1639
    std::atomic<std::uint64_t> dirty_block_ratio_denominator_total{0};      // #1639
    std::atomic<std::uint64_t> relower_block_hit_rate_numerator_total{0};   // #1639
    std::atomic<std::uint64_t> relower_block_hit_rate_denominator_total{0}; // #1639

    // Issue #1640: AOT bridge mangle versioning + region filtering +
    // aot_emit_version stale 检测 + incremental re-emit +
    // hot-update observability. Two new metrics:
    //   - aot_env_frame_version_drift_prevented: bumped when
    //     reload detects env_frame_version drift between the
    //     binary's stamped value and the host's current value,
    //     preventing the stale binary from being activated
    //     (positive control — every detection is counted).
    //   - aot_incremental_reemit_triggered: bumped when the
    //     incremental re-emit hook fires on a stale/region/drift
    //     reload attempt (graceful fallback before reload false
    //     return). Pairs with the existing aot_incremental_reemit_count
    //     (which tracks cumulative re-emitted fn count) so dashboards
    //     can compute the average re-emit count per trigger.
    std::atomic<std::uint64_t> aot_env_frame_version_drift_prevented{0}; // #1640
    std::atomic<std::uint64_t> aot_incremental_reemit_triggered{0};      // #1640

    // Issue #1641: Scheduler / Worker work-stealing awareness for
    // YieldReason::MutationBoundary + per-fiber mutation stack /
    // checkpoint transfer. Three new metrics covering the three
    // gaps the spec calls out (defer + mitigation + safe-steal
    // observability):
    //   - steal_mutation_boundary_deferred_total: bumped when
    //     worker.cpp::steal defers a steal because the victim is
    //     at an inner mutation boundary (depth > 1 + held).
    //     Pairs with the existing bump_deferred_inner (the same
    //     site currently bumps the legacy counter; this is the
    //     explicit per-#1641 metric).
    //   - starvation_mitigated_for_boundary_count: bumped when
    //     apply_starvation_mitigation fires due to a held mutation
    //     boundary (priority boost / budget extension for the
    //     affected fiber). Distinct from the starvation_prevented_count
    //     generic counter so dashboards can filter "starvation
    //     caused by mutation boundary" specifically.
    //   - boundary_held_steal_safe_total: bumped when steal
    //     succeeds while the victim's mutation boundary is still
    //     held (cross-fiber safe-steal path). Pairs with the
    //     existing static_cross_fiber_mutation_safe_steal_total
    //     counter; this is the explicit per-#1641 metric
    //     (the existing one is at the Fiber level, this is at
    //     the Scheduler / Worker level).
    std::atomic<std::uint64_t> steal_mutation_boundary_deferred_total{0};  // #1641
    std::atomic<std::uint64_t> starvation_mitigated_for_boundary_count{0}; // #1641
    std::atomic<std::uint64_t> boundary_held_steal_safe_total{0};          // #1641

    // ── Issues #1261–#1265: dep_graph/AOT/arena/hotswap/QAR Phase 1 ──
    std::atomic<std::uint64_t> production_sweep_1261_1265_active{1};
    std::atomic<std::uint64_t> dep_graph_defuse_version_bumps{0};     // #1261
    std::atomic<std::uint64_t> dep_graph_nested_lambda_full_dirty{0}; // #1261
    // Issue #1514 / #1505: nested-lambda dependents marked via
    // body-only + free-var scan of irs[2..N] (not full-entry dirty).
    // Bumped once per dependent cascade hit on the targeted path.
    std::atomic<std::uint64_t> dep_graph_nested_lambda_targeted_dirty_total{0};
    // Issue #1625: per-block nested lambda targeting (not whole nested fn).
    // blocks_targeted = blocks marked dirty; blocks_kept_clean = left clean.
    std::atomic<std::uint64_t> dep_graph_nested_lambda_blocks_targeted_total{0};
    std::atomic<std::uint64_t> dep_graph_nested_lambda_blocks_kept_clean_total{0};
    std::atomic<std::uint64_t> dep_graph_hygiene_propagate{0}; // #1261
    // Issue #1376: dep_graph_ record_dependency lock observability
    std::atomic<std::uint64_t> dep_graph_record_total{0};       // every record_dependency call
    std::atomic<std::uint64_t> dep_graph_record_dedup_total{0}; // skipped as already present
    std::atomic<std::uint64_t> dep_graph_record_inserted{0};    // new edges written
    std::atomic<std::uint64_t> hot_swap_versioned_mangle_enforced{0}; // #1262
    std::atomic<std::uint64_t> aot_region_filter_enforced{1};         // #1262
    std::atomic<std::uint64_t> arena_reset_dirty_forced{0};           // #1263
    std::atomic<std::uint64_t> hot_update_race_detected{0};           // #1264
    std::atomic<std::uint64_t> hot_update_epoch_fences{1};            // #1264
    std::atomic<std::uint64_t> query_and_replace_all_or_nothing{0};   // #1265
    // Issue #1407 R1: typed_mutate epoch bumps. Bumped on every
    // successful typed_mutate after tx.commit() so persistent
    // downstream caches (TypeChecker::cs_cache_, IR JIT cache,
    // evaluator_workspace FlatAST caches) observe staleness.
    // Distinct from hot_swap_versioned_mangle_enforced
    // (typed_mutate isn't a hot-swap) and from
    // ir_soa_cache_reset_epoch_bumps (typed_mutate doesn't
    // reset the arena; the bump is pure cache invalidation).
    std::atomic<std::uint64_t> typed_mutate_epoch_bumps{0}; // #1407
    // Issue #1524: dual-epoch invalidations from typed_mutate /
    // typed_mutate_atomic via atomic_bump_epochs_and_stamp_bridge.
    std::atomic<std::uint64_t> typed_mutate_atomic_invalidations_total{0};
    std::atomic<std::uint64_t> query_and_replace_parse_abort{0}; // #1265

    // ── Issues #1266–#1270: inline/set-body/panic/SoA/steal Phase 1 ──
    std::atomic<std::uint64_t> production_sweep_1266_1270_active{1};
    std::atomic<std::uint64_t> inline_call_lambda_params_copied{0};      // #1266
    std::atomic<std::uint64_t> set_body_define_value_extracted{0};       // #1267
    std::atomic<std::uint64_t> panic_checkpoint_flush_outermost{0};      // #1268
    std::atomic<std::uint64_t> envframe_dualpath_materialize_refresh{0}; // #1269
    std::atomic<std::uint64_t> envframe_dualpath_enforced{1};            // #1269
    std::atomic<std::uint64_t> steal_starvation_mitigation{1};           // #1270

    // ── Issues #1271–#1275: AOT/obs/hygiene-IR/dirty/EDSL Phase 1 ──
    std::atomic<std::uint64_t> production_sweep_1271_1275_active{1};
    std::atomic<std::uint64_t> aot_hot_update_atomic_rollback{0};              // #1271
    std::atomic<std::uint64_t> aot_hot_update_multi_agent_versioned{0};        // #1271
    std::atomic<std::uint64_t> aot_reemit_dirty_skeleton_calls{0};             // #1271
    std::atomic<std::uint64_t> mutation_boundary_contention_us_hist{0};        // #1272
    std::atomic<std::uint64_t> runtime_obs_mutation_boundary_flush_samples{0}; // #1272
    std::atomic<std::uint64_t> runtime_obs_export_ready{1};                    // #1272
    std::atomic<std::uint64_t> ir_hygiene_macro_marker_enforced{1};            // #1273
    std::atomic<std::uint64_t> dirty_propagation_to_ir_count{0};               // #1274
    std::atomic<std::uint64_t> epoch_bump_for_macro{0};                        // #1274
    std::atomic<std::uint64_t> naked_macro_mutate_attempt{0};                  // #1275
    std::atomic<std::uint64_t> hygiene_edsl_awareness{1};                      // #1275

    // ── Issues #1276–#1280: reflect/obs/inliner/StableRef/pattern Phase 1 ──
    std::atomic<std::uint64_t> production_sweep_1276_1280_active{1};
    std::atomic<std::uint64_t> reflect_nested_struct_scaffold{1};   // #1276
    std::atomic<std::uint64_t> reflect_runtime_schema_hooks{1};     // #1276
    std::atomic<std::uint64_t> hygiene_violation_stats_active{1};   // #1277
    std::atomic<std::uint64_t> dirty_impact_stats_active{1};        // #1277
    std::atomic<std::uint64_t> inline_diamond_cfg_fixed{1};         // #1278
    std::atomic<std::uint64_t> stable_ref_boundary_auto_refresh{0}; // #1279
    std::atomic<std::uint64_t> stable_ref_auto_refresh_enforced{1}; // #1279
    // Issue #1564: full provenance enforcement process mirrors
    std::atomic<std::uint64_t> stable_ref_auto_refresh_total{0};
    std::atomic<std::uint64_t> stable_ref_epoch_fence_hit_total{0};
    std::atomic<std::uint64_t> cross_layer_provenance_mismatch_total{0};
    std::atomic<std::uint64_t> pattern_hygiene_default_exclude{0}; // #1280
    std::atomic<std::uint64_t> pattern_hygiene_end_to_end{1};      // #1280

    // ── Issues #1281–#1285: children rollback / gen wrap / provenance / fallback / JIT EH ──
    std::atomic<std::uint64_t> production_sweep_1281_1285_active{1};
    std::atomic<std::uint64_t> children_topology_rollback_fidelity{1}; // #1281 scaffold
    std::atomic<std::uint64_t> children_topology_rollback_count{0};    // #1281
    // Issue #1502: parent_ topology restores (with children_ on failed boundary).
    std::atomic<std::uint64_t> parent_topology_rollback_count{0};
    std::atomic<std::uint64_t> generation_auto_restamp_on_wrap{0};   // #1282
    std::atomic<std::uint64_t> generation_wrap_restamp_policy{1};    // #1282 auto-restamp on
    std::atomic<std::uint64_t> provenance_boundary_hooks_active{1};  // #1283
    std::atomic<std::uint64_t> provenance_boundary_capture_count{0}; // #1283
    std::atomic<std::uint64_t> dirty_provenance_stats_active{1};     // #1283
    std::atomic<std::uint64_t> tree_walker_define_cache_hits{0};     // #1284
    std::atomic<std::uint64_t> tree_walker_fallback_reduction{1};    // #1284
    std::atomic<std::uint64_t> jit_exception_opcodes_covered{1};     // #1285
    std::atomic<std::uint64_t> jit_exception_opcode_lowered{0};      // #1285
    // Issue #1516: per-function AOT + interpreter EH production surface
    // (mirrors AuraJIT::Metrics aot_per_function_* / exception_opcode_mask
    // + IRInterpreter TryBegin/End/Raise/IsError hits).
    std::atomic<std::uint64_t> aot_per_function_ir_total{0};       // #1516
    std::atomic<std::uint64_t> aot_per_function_object_total{0};   // #1516
    std::atomic<std::uint64_t> aot_per_function_miss_total{0};     // #1516
    std::atomic<std::uint64_t> aot_last_module_object_total{0};    // #1516
    std::atomic<std::uint64_t> interpreter_exception_ops_total{0}; // #1516
    std::atomic<std::uint64_t> jit_exception_opcode_mask{0};       // #1516

    // ── Issues #1286–#1290: invalidate/block-dirty, closure epoch, GuardShape, JIT fail-fast,
    // ownership Lambda ──
    std::atomic<std::uint64_t> production_sweep_1286_1290_active{1};
    std::atomic<std::uint64_t> invalidate_per_block_dirty_total{0};     // #1286
    std::atomic<std::uint64_t> invalidate_per_block_dirty_active{1};    // #1286
    std::atomic<std::uint64_t> closure_bridge_epoch_safety_enforced{0}; // #1287
    std::atomic<std::uint64_t> closure_bridge_epoch_safety_active{1};   // #1287
    std::atomic<std::uint64_t> guard_shape_linear_unified_active{1};    // #1288
    std::atomic<std::uint64_t> guard_shape_linear_unified_checks{0};    // #1288
    std::atomic<std::uint64_t> jit_unhandled_fail_fast_active{1};       // #1289
    std::atomic<std::uint64_t> ownership_lambda_params_fixed{1};        // #1290

    // ── Issues #1291–#1295: fiber fid, workspace UAF, compile/fiber caps, exception clear ──
    std::atomic<std::uint64_t> production_sweep_1291_1295_active{1};
    std::atomic<std::uint64_t> fiber_spawn_fid_holder_fixed{1};         // #1291
    std::atomic<std::uint64_t> workspace_delete_pointer_refresh{1};     // #1292
    std::atomic<std::uint64_t> capability_compile_gates_active{1};      // #1293
    std::atomic<std::uint64_t> capability_compile_denials{0};           // #1293
    std::atomic<std::uint64_t> capability_retrofit_scaffold_active{1};  // #1294
    std::atomic<std::uint64_t> capability_exception_control_active{1};  // #1295
    std::atomic<std::uint64_t> capability_exception_control_denials{0}; // #1295

    // ── Issues #1296–#1300: predicate race, inline max_slot, ghost orphan free ──
    std::atomic<std::uint64_t> production_sweep_1296_1300_active{1};
    std::atomic<std::uint64_t> custom_predicate_registry_mutex{1}; // #1296
    std::atomic<std::uint64_t> inline_max_slot_includes_params{1}; // #1297/#1298
    std::atomic<std::uint64_t> ghost_orphan_free_on_rollback{1};   // #1299/#1300

    // ── Issues #1301–#1305: mutation_log compact, arena OOB, name fallback, fn limit, cache TOCTOU
    // ──
    std::atomic<std::uint64_t> production_sweep_1301_1305_active{1};
    std::atomic<std::uint64_t> mutation_log_compact_on_rollback{1}; // #1301
    std::atomic<std::uint64_t> jit_arena_env_bounds_check{1};       // #1302
    std::atomic<std::uint64_t> jit_closure_name_fallback_fixed{1};  // #1303
    std::atomic<std::uint64_t> jit_fns_overflow_map_active{1};      // #1304
    std::atomic<std::uint64_t> jit_closure_cache_write_lock{1};     // #1305

    // ── Issues #1306–#1310: string/float pool races, last_module lock, is_arena, free envs ──
    std::atomic<std::uint64_t> production_sweep_1306_1310_active{1};
    std::atomic<std::uint64_t> jit_string_pool_mutex{1};       // #1306
    std::atomic<std::uint64_t> jit_float_pool_mutex{1};        // #1307
    std::atomic<std::uint64_t> jit_last_module_aot_lock{1};    // #1308
    std::atomic<std::uint64_t> jit_closure_is_arena_flag{1};   // #1309
    std::atomic<std::uint64_t> jit_arena_env_free_on_reset{1}; // #1310

    // ── Issues #1311–#1315: cow pins race, jit setters, terminal, render arena ──
    std::atomic<std::uint64_t> production_sweep_1311_1315_active{1};
    std::atomic<std::uint64_t> cow_boundary_pins_mutex{1};      // #1311
    std::atomic<std::uint64_t> jit_runtime_setters_locked{1};   // #1312
    std::atomic<std::uint64_t> terminal_buffer_creates{0};      // #1313
    std::atomic<std::uint64_t> terminal_set_cell_total{0};      // #1313
    std::atomic<std::uint64_t> terminal_diff_updates{0};        // #1313
    std::atomic<std::uint64_t> terminal_present_batch_total{0}; // #1314
    std::atomic<std::uint64_t> terminal_present_bytes_total{0}; // #1314
    // #1349: ANSI SGR + CSI H present path (P0 cyber-cat)
    std::atomic<std::uint64_t> terminal_present_ansi_active{1};
    std::atomic<std::uint64_t> terminal_present_sgr_emits_total{0};
    std::atomic<std::uint64_t> terminal_present_csi_h_rows_total{0};
    std::atomic<std::uint64_t> terminal_present_sync_frames_total{0};
    // #1350: 24-bit RGB + Unicode cell format
    std::atomic<std::uint64_t> terminal_cell64_active{1};
    std::atomic<std::uint64_t> terminal_set_cell_rgb_total{0};
    std::atomic<std::uint64_t> terminal_set_cell_unicode_total{0};
    // #1352: terminal buffer lifecycle + per-buffer locking
    std::atomic<std::uint64_t> terminal_buffer_deletes{0};
    std::atomic<std::uint64_t> terminal_buffer_compacts{0};
    std::atomic<std::uint64_t> terminal_buffer_live{0};
    std::atomic<std::uint64_t> terminal_buffer_lifecycle_active{1};
    std::atomic<std::uint64_t> render_hotpath_samples{0};       // #1314
    std::atomic<std::uint64_t> render_frame_reset_total{0};     // #1315
    std::atomic<std::uint64_t> render_frame_reset_deferred{0};  // #1315
    std::atomic<std::uint64_t> render_frame_reset_reclaimed{0}; // #1315

    // ── Issues #1316–#1320: render JIT stability, render obs, SoA migrate, gap, defrag ──
    std::atomic<std::uint64_t> production_sweep_1316_1320_active{1};
    // #1316 render hot-path deopt throttle / stable marking
    std::atomic<std::uint64_t> render_stable_hot_path_active{1};
    std::atomic<std::uint64_t> render_jit_deopt_applied{0};
    std::atomic<std::uint64_t> render_jit_deopt_throttled{0};
    std::atomic<std::uint64_t> render_jit_aot_prefer_hits{0};
    std::atomic<std::uint64_t> render_deopt_throttle_window_ms{500};
    // Issue #1563: AOT/hot preference hit rate in basis points (0..10000).
    std::atomic<std::uint64_t> render_aot_hit_rate_bp{0};
    std::atomic<std::uint64_t> render_critical_meta_count{0};
    std::atomic<std::uint64_t> render_hotpath_enter_total{0};
    // #1559: dirty short-circuit skip counter (mirrors arena_policy::render_hotpath_skip_total)
    std::atomic<std::uint64_t> render_hotpath_skip_total{0};
    // Issue #1676: render-tier prim dispatch + linear/epoch entry fences
    std::atomic<std::uint64_t> render_hotpath_dispatch_fast_total{0};
    std::atomic<std::uint64_t> render_hotpath_dispatch_full_total{0};
    std::atomic<std::uint64_t> render_hotpath_linear_fence_total{0};
    std::atomic<std::uint64_t> render_hotpath_epoch_fence_total{0};
    std::atomic<std::uint64_t> render_hotpath_linear_block_total{0};
    std::atomic<std::uint64_t> render_hotpath_epoch_stale_total{0};
    // Issue #1677: AI Native render evolution (rebind/optimize of draw logic)
    std::atomic<std::uint64_t> render_evolution_rebind_total{0};
    std::atomic<std::uint64_t> render_evolution_optimize_total{0};
    std::atomic<std::uint64_t> render_evolution_savings_total{0};
    std::atomic<std::uint64_t> render_template_phase{1};
    // #1562: dirty-region differential present metrics
    std::atomic<std::uint64_t> render_dirty_region_skips_total{0};
    std::atomic<std::uint64_t> render_dirty_cells_emitted_total{0};
    std::atomic<std::uint64_t> render_dirty_cells_skipped_total{0};
    // #1317 render observability + RENDER_PRIMITIVE_META
    std::atomic<std::uint64_t> render_primitive_meta_active{1};
    std::atomic<std::uint64_t> render_obs_query_hits{0};
    std::atomic<std::uint64_t> terminal_diff_stats_queries{0};
    std::atomic<std::uint64_t> terminal_diff_cells_total{0};
    // #1318 IR SoA full migration progressive hooks
    std::atomic<std::uint64_t> ir_soa_migration_phase2_active{1};
    std::atomic<std::uint64_t> ir_soa_hotpath_hits{0};
    std::atomic<std::uint64_t> ir_soa_dual_emit_bridge_count{0};
    std::atomic<std::uint64_t> ir_soa_dirty_short_circuit{0};
    // #1319 gap_buffer structural mutate scaffold
    std::atomic<std::uint64_t> gap_buffer_structural_mutate_active{1};
    std::atomic<std::uint64_t> gap_buffer_structural_mutate_hits{0};
    std::atomic<std::uint64_t> gap_buffer_insert_total{0};
    std::atomic<std::uint64_t> gap_buffer_erase_total{0};
    // #1320 live defrag + auto-compact policy (render soft-gate)
    std::atomic<std::uint64_t> arena_live_defrag_policy_active{1};
    std::atomic<std::uint64_t> arena_defrag_attempted_total{0};
    std::atomic<std::uint64_t> arena_defrag_saved_bytes_total{0};
    std::atomic<std::uint64_t> arena_compact_soft_gated_render{0};
    std::atomic<std::uint64_t> arena_defrag_now_calls{0};

    // ── Issues #1321–#1324: contracts expand, dirty pipeline, JIT map races ──
    std::atomic<std::uint64_t> production_sweep_1321_1324_active{1};
    // #1321 C++26 contracts + consteval hot-path expansion
    std::atomic<std::uint64_t> hotpath_contracts_expanded{1};
    std::atomic<std::uint64_t> soa_view_bounds_contracts{1};
    std::atomic<std::uint64_t> flatast_column_contracts{1};
    std::atomic<std::uint64_t> consteval_checks_total{36};
    // #1322 DirtyAware + SoAView pipeline short-circuit
    std::atomic<std::uint64_t> pipeline_dirty_short_circuit_active{1};
    std::atomic<std::uint64_t> pipeline_dirty_short_circuit_total{0};
    std::atomic<std::uint64_t> pipeline_epoch_sync_total{0};
    std::atomic<std::uint64_t> pipeline_soa_view_aware_total{0};
    std::atomic<std::uint64_t> pipeline_hotpath_light_analysis_total{0};
    // #1323 fn_unhandled_counts_ query lock
    std::atomic<std::uint64_t> jit_fn_unhandled_counts_query_locked{1};
    // #1324 invalidate / invalidate_prefix lock-before-erase
    std::atomic<std::uint64_t> jit_invalidate_lock_before_erase{1};

    // ── Issues #1325–#1330: primitive surface reduction architecture ──
    // META roadmap: 700+ → ~50 user-facing primitives over 5 phases.
    std::atomic<std::uint64_t> production_sweep_1325_1330_active{1};
    // #1325 META inventory / target surface
    std::atomic<std::uint64_t> prim_surface_reduction_plan_active{1};
    std::atomic<std::uint64_t> prim_surface_target_count{50};
    std::atomic<std::uint64_t> prim_surface_phases_total{5};
    // #1326 Phase 1: demote/delete write-side compile/jit (deprecation cycle)
    std::atomic<std::uint64_t> prim_write_side_compile_jit_demotion_active{1};
    std::atomic<std::uint64_t> prim_write_side_deprecation_hits{0};
    std::atomic<std::uint64_t> prim_stats_namespace_active{1};
    std::atomic<std::uint64_t> prim_stats_alias_hits{0};
    // #1327 Phase 2: agent service bridge
    std::atomic<std::uint64_t> agent_service_bridge_active{1};
    std::atomic<std::uint64_t> agent_tick_total{0};
    std::atomic<std::uint64_t> agent_legacy_auto_evolve_hits{0};
    // #1713: auto-evolve-tick/once saw freed detect/fix ClosureId
    std::atomic<std::uint64_t> agent_closure_freed_during_tick{0};
    // #1719: intend (and other apply_closure call sites) saw freed ClosureId
    std::atomic<std::uint64_t> agent_closure_freed_during_call{0};
    // #1724: evolve-strategy analytics stod/stoi parse failure (keep defaults)
    std::atomic<std::uint64_t> agent_evolve_analytics_parse_failures{0};
    // #1726: evolve-strategy name-collision bump loop exhausted
    std::atomic<std::uint64_t> agent_evolve_name_collision_exhausted{0};
    // #1328 Phase 3: query essentials keep-list
    std::atomic<std::uint64_t> query_essentials_plan_active{1};
    std::atomic<std::uint64_t> query_essentials_keep_count{10};
    // #1329 Phase 4: stdlib → Aura + sys-* bindings scaffold
    std::atomic<std::uint64_t> stdlib_sys_bindings_active{1};
    std::atomic<std::uint64_t> sys_open_calls{0};
    std::atomic<std::uint64_t> sys_read_calls{0};
    std::atomic<std::uint64_t> sys_write_calls{0};
    // #1330 Phase 5: capability retrofit scaffold
    std::atomic<std::uint64_t> cap_retrofit_scaffold_active{1};
    std::atomic<std::uint64_t> cap_capability_constant_count{8}; // new caps this batch
    std::atomic<std::uint64_t> cap_denial_total{0};

    // ── Issues #1331–#1343: 5-layer TUI pixel rendering architecture ──
    std::atomic<std::uint64_t> production_sweep_1331_1343_active{1};
    // #1331 META
    std::atomic<std::uint64_t> tui_architecture_plan_active{1};
    std::atomic<std::uint64_t> tui_layers_total{5};
    // #1332 runtime
    std::atomic<std::uint64_t> tui_runtime_active{1};
    std::atomic<std::uint64_t> tui_init_total{0};
    std::atomic<std::uint64_t> tui_present_total{0};
    std::atomic<std::uint64_t> tui_cell_writes{0};
    std::atomic<std::uint64_t> tui_diff_cells_emitted{0};
    // #1333 primitives
    std::atomic<std::uint64_t> tui_primitives_active{1};
    // #1334–#1335 stdlib
    std::atomic<std::uint64_t> tui_stdlib_active{1};
    // #1337 demo
    std::atomic<std::uint64_t> tui_cyber_cat_demo_active{1};
    // #1342 opts
    std::atomic<std::uint64_t> tui_sync_output_active{1};
    std::atomic<std::uint64_t> tui_sync_output_frames{0};
    std::atomic<std::uint64_t> tui_half_block_pixels{0};
    // #1343 extensions
    std::atomic<std::uint64_t> tui_mouse_scaffold_active{1};
    std::atomic<std::uint64_t> tui_mouse_enable_total{0};
    std::atomic<std::uint64_t> tui_games_scaffold_active{1};
    // #1353 keyboard raw mode + poll input
    std::atomic<std::uint64_t> tui_input_active{1};
    std::atomic<std::uint64_t> tui_raw_mode_on_total{0};
    std::atomic<std::uint64_t> tui_raw_mode_off_total{0};
    std::atomic<std::uint64_t> tui_poll_event_total{0};
    std::atomic<std::uint64_t> tui_poll_event_hits{0};
    std::atomic<std::uint64_t> tui_key_events_total{0};
    std::atomic<std::uint64_t> tui_mouse_events_total{0};
    std::atomic<std::uint64_t> tui_quit_events_total{0};

    // ── Issues #1336–#1341, #1344–#1348: type/AST/EDA production sweep ──
    std::atomic<std::uint64_t> production_sweep_1336_1348_active{1};
    // #1336 Incremental TC: infer_flat_partial + solve_delta + dirty prune
    std::atomic<std::uint64_t> incremental_tc_selective_active{1};
    std::atomic<std::uint64_t> infer_flat_partial_selective_total{0};
    std::atomic<std::uint64_t> solve_delta_worklist_limited_total{0};
    std::atomic<std::uint64_t> solve_delta_worklist_soft_cap{256};
    // Issue #1528: O(delta) re-inference observability.
    //   - incremental_reinfer_nodes_total: nodes re-inferred by
    //     infer_flat_partial (sum of successful per-node re-checks)
    //   - solve_delta_cache_hit_total: cs_cache / solved_delta cache
    //     hits that skipped a constraint worklist scan
    //   - solve_delta_worklist_pruned_total: dirty constraints excluded
    //     from the local worklist because they do not reference
    //     touched / occurrence-priority roots
    //   - type_dep_graph_affected_expand_total: extra nodes pulled into
    //     the affected set via type_dep_graph_ dependents
    // Issue #1871: solve_delta locality + adaptive reverify.
    //   - solve_delta_locality_hits_total: local-root solves with
    //     zero pruned dirty (full O(delta) coverage of dirty set)
    //   - solve_delta_locality_misses_total: local-root solves that
    //     deferred non-local dirty into pending_full_solve_roots_
    //   - incremental_locality_hit_rate: hits*100/(hits+misses) (0–100)
    //   - reverify_adaptive_adjustments_total: clean reverify scans
    //     whose effective_reverify_limit exceeded the base 256 cap
    std::atomic<std::uint64_t> incremental_reinfer_nodes_total{0};
    std::atomic<std::uint64_t> solve_delta_cache_hit_total{0};
    std::atomic<std::uint64_t> solve_delta_worklist_pruned_total{0};
    std::atomic<std::uint64_t> type_dep_graph_affected_expand_total{0};
    std::atomic<std::uint64_t> solve_delta_locality_hits_total{0};
    std::atomic<std::uint64_t> solve_delta_locality_misses_total{0};
    std::atomic<std::uint64_t> incremental_locality_hit_rate{0};
    std::atomic<std::uint64_t> reverify_adaptive_adjustments_total{0};
    // #1338 Type → IR + DeadCoercionElimination parent-type stamp
    std::atomic<std::uint64_t> ir_parent_type_stamp_active{1};
    std::atomic<std::uint64_t> ir_parent_type_stamped_total{0};
    std::atomic<std::uint64_t> dce_cast_elision_total{0};
    std::atomic<std::uint64_t> dce_elide_identity_total{0};
    std::atomic<std::uint64_t> dce_elide_narrow_total{0};
    std::atomic<std::uint64_t> dce_elide_nested_total{0};
    std::atomic<std::uint64_t> dce_elide_dynamic_total{0};
    // #1339 Linear ownership mutation + Move elide
    std::atomic<std::uint64_t> linear_move_elide_active{1};
    std::atomic<std::uint64_t> linear_move_elided_total{0};
    std::atomic<std::uint64_t> linear_ownership_escape_check_total{0};
    // #1340 ADT exhaust recheck
    std::atomic<std::uint64_t> adt_exhaust_incremental_active{1};
    // #1341 Blame / elision reason observability
    std::atomic<std::uint64_t> blame_elision_reason_obs_active{1};
    // #1344 SV high-level mutate + query:pattern presets
    std::atomic<std::uint64_t> sv_highlevel_mutate_active{1};
    std::atomic<std::uint64_t> eda_mutate_modport_total{0};
    std::atomic<std::uint64_t> eda_mutate_interface_total{0};
    std::atomic<std::uint64_t> eda_mutate_property_total{0};
    std::atomic<std::uint64_t> query_sv_pattern_preset_active{1};
    // #1345 mark_dirty_upward configurable prune
    std::atomic<std::uint64_t> dirty_upward_prune_active{1};
    std::atomic<std::uint64_t> dirty_upward_pruned_boundary_total{0};
    std::atomic<std::uint64_t> dirty_upward_max_depth_config{64};
    // #1346 StableNodeRef lock-free / monitoring
    std::atomic<std::uint64_t> stable_ref_lockfree_path_active{1};
    std::atomic<std::uint64_t> stable_ref_lockfree_validate_total{0};
    std::atomic<std::uint64_t> stable_ref_stale_refresh_total{0};
    std::atomic<std::uint64_t> stable_ref_contention_total{0};
    // #1347 SV verification feedback harness
    std::atomic<std::uint64_t> sv_feedback_harness_active{1};
    std::atomic<std::uint64_t> verify_parse_coverage_total{0};
    std::atomic<std::uint64_t> verify_parse_assert_total{0};
    std::atomic<std::uint64_t> verify_auto_trigger_mutate_total{0};
    // Issue #1772: mutate:from-verification-feedback rejected OOB/neg node_id
    // before eda:* delegation.
    std::atomic<std::uint64_t> mutate_from_feedback_invalid_node_total{0};
    // #1348 AST long-run compaction
    std::atomic<std::uint64_t> ast_auto_compact_active{1};
    std::atomic<std::uint64_t> ast_auto_compact_on_commit_total{0};
    std::atomic<std::uint64_t> ast_live_nodes_warn_total{0};
    std::atomic<std::uint64_t> ast_compaction_threshold{1024};
    std::atomic<std::uint64_t> ast_max_live_nodes{1'000'000};

    // Issue #1385: env_frames_arena observability. Snapshot
    // fields, refreshed on each (compiler:metrics) call. See
    // Evaluator::refresh_env_arena_metrics + CompilerService::
    // refresh_env_arena_metrics for the snapshot logic.
    //
    // env_frames_size_total: current env_frames_.size() (O(1)
    // snapshot). Append-only — monotonic in long-running
    // processes (aura-pets pipelines run for hours). Operators
    // alert when this grows past a threshold.
    //
    // env_frames_stale_count: number of frames with version_ <
    // current defuse_version_ (O(N) iteration under shared lock).
    // High value means many stale captures; not necessarily a
    // problem (they get refreshed on walk), but useful for
    // capacity planning.
    //
    // ast_arena_bytes_in_use: ASTArena::used() snapshot (O(1)).
    // Bytes currently consumed in the arena's monotonic buffer.
    // Compare against capacity for fragmentation estimation.
    //
    // ast_arena_upstream_bytes: bytes allocated through the
    // arena's upstream memory_resource (e.g. fallback chunks
    // from new_delete_resource when the arena buffer overflows).
    // Tracks via a CountingMR installed as the arena's upstream.
    // Monotonic — NOT reclaimed by arena.reset() or
    // resource_.release() (per the C++ spec on monotonic_buffer_
    // resource::deallocate). High value means the arena has
    // overflowed its initial buffer and is paying the fallback
    // cost.
    std::atomic<std::uint64_t> env_frames_size_total{0};
    std::atomic<std::uint64_t> env_frames_stale_count{0};
    std::atomic<std::uint64_t> ast_arena_bytes_in_use{0};
    std::atomic<std::uint64_t> ast_arena_upstream_bytes{0};

    // ── Issue #1480: incremental re-AOT pipeline (Phase 2) ───────────
    //
    // Tracks the end-to-end incremental re-AOT pipeline that
    // supersedes the #1271 `aura_reemit_aot_for_dirty` skeleton
    // (which returned 0 + bumped aot_reemit_dirty_skeleton_calls).
    // The Phase 2 pipeline:
    //   1. host (Evaluator) registers a re-emit candidate callback
    //      via aura_set_reemit_candidate_fn (push-based — host
    //      pushes (name, region) pairs from ir_cache_v2_ +
    //      dep_graph_ cascade)
    //   2. aura_reemit_aot_for_dirty iterates candidates,
    //      applies per-function region mask (skip if region's bit
    //      not in g_aot_emit_region_mask)
    //   3. for each non-skipped: runs the AOT pipeline (stub in
    //      #1480; full LLVM path is #1481 follow-up)
    //   4. on any successful re-emit: commit_func_table_swap()
    //      atomically bumps g_aot_table_epoch so concurrent
    //      stale-frame probes see consistent before/after
    //
    // aot_incremental_reemit_count:
    //   # of dirty FlatFunctions actually re-emitted this call
    //   (after region-mask filter). Grows monotonically; AI agent
    //   reads it via (query:aot-incremental-reemit-count) to verify
    //   the per-mutation re-emit budget.
    //
    // aot_closure_dependency_reemit_total:
    //   # of candidate (name, region) pairs pushed by the host
    //   that came from closure-capture cascade dependents (not
    //   direct CALLS cascade). Measures "how much extra work
    //   closure captures add to re-emit" — drives the optimization
    //   to short-circuit when the captured var is pure.
    //
    // aot_region_filtered_skips:
    //   # of candidates that matched a dirty Define but whose
    //   region bit was not in g_aot_emit_region_mask, so they
    //   were skipped (not re-emitted in this region).
    //
    // aot_closure_bridge_refresh_total:
    //   # of closure bridges re-stamped with the new bridge_epoch
    //   after a successful re-emit commit. Pair metric with
    //   jit_hotswap_live_closure_refreshed_total (JIT-side) —
    //   both should grow together when an AOT swap happens with
    //   live closure traffic.
    std::atomic<std::uint64_t> aot_incremental_reemit_count{0};        // #1480
    std::atomic<std::uint64_t> aot_closure_dependency_reemit_total{0}; // #1480
    std::atomic<std::uint64_t> aot_region_filtered_skips{0};           // #1480
    std::atomic<std::uint64_t> aot_closure_bridge_refresh_total{0};    // #1480

    // ── Issue #1630: mandate full StableNodeRef provenance ──
    // fiber_id mismatch fail-fast (unpinned); boundary_pinned auto-restamp
    // across steal; cross-COW provenance enforcement counts.
    std::atomic<std::uint64_t> stable_ref_fiber_mismatch_prevented_total{0};
    std::atomic<std::uint64_t> boundary_pinned_auto_restamp_total{0};
    std::atomic<std::uint64_t> cross_cow_provenance_enforced_total{0};
};


// Per-function metrics, returned by CompilerService::snapshot()
// for --evo-explain. Reflect-friendly.
struct FnMetrics {
    std::string name;                  // function name
    std::uint64_t total_calls = 0;     // total invocations observed
    std::uint64_t deopt_count = 0;     // deopt count (subset of total)
    std::uint64_t hit_count = 0;       // specialization hit (specialized path)
    std::uint64_t miss_count = 0;      // specialization miss (generic path)
    double hit_rate = 0.0;             // hit_count / (hit_count + miss_count)
    bool has_shape_map = false;        // was compiled with shape_map?
    std::uint32_t specialized_for = 0; // Issue #61: shape ID
};

} // namespace aura::compiler

#endif // AURA_COMPILER_OBSERVABILITY_METRICS_H
