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
    // Issue #401: invalidate_function call counter. Bumped once per
    // invalidate_function entry (post-mutex-acquire, so the count
    // reflects only completed traversals). Pairs with jit_cache_evictions
    // (which counts evictions for the root + each dependent): the ratio
    // (jit_cache_evictions - invalidate_function_calls) / invalidate_function_calls
    // tells you the average dependent fan-out per invalidation.
    std::atomic<std::uint64_t> invalidate_function_calls{0};
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
    std::atomic<std::uint64_t> eda_sv_commercial_stub_latency_us_total{0};
    std::atomic<std::uint64_t> eda_sv_corruption_detected_total{0};
    // Issue #697: Declarative primitives extension kit observability.
    std::atomic<std::uint64_t> primitive_skeleton_generations_total{0};
    std::atomic<std::uint64_t> primitive_eda_meta_backfill_total{0};
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
    // Issue #709: registry fast dispatch + capture discipline telemetry.
    std::atomic<std::uint64_t> primitive_fastpath_hits_total{0};
    std::atomic<std::uint64_t> primitive_capture_violations_total{0};
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
    // Issue #683: linear ownership + GC safepoint / fiber-steal /
    // post-re-lower revalidate integration.
    // Exposed via (query:linear-ownership-gc-stats).
    std::atomic<std::uint64_t> linear_gc_safepoint_violations{0};
    std::atomic<std::uint64_t> linear_steal_enforced{0};
    std::atomic<std::uint64_t> linear_relower_revalidate_hits{0};
    // Issue #688: infer_flat_partial OwnershipEnv post-mutate revalidate.
    std::atomic<std::uint64_t> linear_dirty_revalidate_count{0};
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
    // Issue #708: AOT hot-reload refcount swap + checkpoint version drift.
    std::atomic<std::uint64_t> aot_reload_attempts_{0};
    std::atomic<std::uint64_t> aot_refcount_swaps_{0};
    std::atomic<std::uint64_t> aot_deopt_on_steal_{0};
    std::atomic<std::uint64_t> aot_concurrent_safe_reloads_{0};
    std::atomic<std::uint64_t> aot_checkpoint_version_drifts_{0};
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
