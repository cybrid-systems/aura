// observability_metrics.h — structured counters for self-evolving paths
// (Issue #62). The structs here are intentionally POD-ish so they
// can be serialized via aura::reflect::auto_to_json. Atomic counters
// for thread safety; the struct itself is updated with relaxed
// memory order — exact counts are advisory, not contractual.

#ifndef AURA_COMPILER_OBSERVABILITY_METRICS_H
#define AURA_COMPILER_OBSERVABILITY_METRICS_H

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
    // Issue #253: linear-move elision count (lifetime total).
    // Bumped by TypeSpecializationWrap after each run (in
    // service.ixx — the pass has its own per-run accumulator;
    // service.ixx copies it here so snapshot() and the Aura
    // primitive read a single source of truth). The actual
    // elision logic is in pass_manager.ixx.
    std::atomic<std::uint64_t> linear_elide_count{0};
    // Issue #254: IR SoA dual-emit counters (lifetime total).
    // Bumped by service.ixx after each lower_to_ir call when
    // dual-emit is enabled. The underlying counters live on
    // LoweringState (soa_instructions_emitted /
    // soa_functions_emitted). service.ixx accumulates them
    // into metrics_ so snapshot() + the Aura primitive read
    // a single source of truth.
    std::atomic<std::uint64_t> ir_soa_instructions_emitted{0};
    std::atomic<std::uint64_t> ir_soa_functions_emitted{0};
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
