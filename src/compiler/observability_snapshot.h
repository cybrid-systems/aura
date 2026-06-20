// observability_snapshot.h — POD snapshot of observability state
// (Issue #62 Iter 3). The atomic fields in CompilerMetrics are
// not reflect-friendly (the framework's template only handles
// built-in types). The snapshot is a plain-POD copy, populated
// on demand by CompilerService::snapshot(). Then auto_to_json
// serializes it cleanly.

#ifndef AURA_COMPILER_OBSERVABILITY_SNAPSHOT_H
#define AURA_COMPILER_OBSERVABILITY_SNAPSHOT_H

#include <cstdint>
#include <string>
#include <vector>

#include "observability_metrics.h"  // for FnMetrics

namespace aura::compiler {

// Plain-POD snapshot of CompilerMetrics. Populated by atomic loads.
struct CompilerSnapshot {
    std::uint64_t deopt_count = 0;
    std::uint64_t specialization_hits = 0;
    std::uint64_t specialization_misses = 0;
    std::uint64_t shape_changes_observed = 0;
    std::uint64_t jit_compilations = 0;
    std::uint64_t jit_compile_misses = 0;
    std::uint64_t jit_cache_evictions = 0;
    std::uint64_t aot_emits = 0;
    std::uint64_t aot_fallbacks = 0;
    std::uint64_t arena_bytes_used = 0;
    std::uint64_t arena_bytes_peak = 0;
    // Issue #250: atomic-batch observability. atomics are
    // loaded with relaxed order in CompilerService::snapshot().
    // - atomic_batch_count: total successful batches
    // - atomic_batch_ops_total: total ops across all batches
    // - atomic_batch_rollbacks: total rollbacks
    // - atomic_batch_bumps_saved_total: how many per-op
    //   generation bumps the batches suppressed (lifetime)
    std::uint64_t atomic_batch_count = 0;
    std::uint64_t atomic_batch_ops_total = 0;
    std::uint64_t atomic_batch_rollbacks = 0;
    std::uint64_t atomic_batch_bumps_saved_total = 0;
    // Issue #252: closure dual-path observability. Counters
    // for the 3 dispatch paths in apply_closure + stale-bridge
    // returns (Issue #223). The fast-path optimization in
    // #252 follow-ups will be measured by changes in these.
    // - closure_calls_total: every apply_closure call
    // - closure_ffi_calls: FFI-dispatched
    // - closure_tw_calls: tree-walker closures_ map hit
    // - closure_bridge_calls: closure_bridge_ (IR/JIT)
    // - closure_stale_returns: stale-bridge nullopt returns
    std::uint64_t closure_calls_total = 0;
    std::uint64_t closure_ffi_calls = 0;
    std::uint64_t closure_tw_calls = 0;
    std::uint64_t closure_bridge_calls = 0;
    std::uint64_t closure_ir_calls = 0;
    std::uint64_t closure_stale_returns = 0;
    // Issue #253: lifetime total of MoveOp instructions elided
    // by TypeSpecializationWrap (when source has
    // linear_ownership_state == Owned). Mirrors
    // CompilerMetrics::linear_elide_count.
    std::uint64_t linear_elide_count = 0;
    // Issue #254: IR SoA dual-emit counters (lifetime total).
    // Mirrors CompilerMetrics::ir_soa_instructions_emitted +
    // CompilerMetrics::ir_soa_functions_emitted.
    std::uint64_t ir_soa_instructions_emitted = 0;
    std::uint64_t ir_soa_functions_emitted = 0;
    // Issue #255: reference stability observability. Mirrors
    // CompilerMetrics::{bump_generation_count, is_valid_check_count,
    // stable_ref_invalidations, atomic_batch_commits}.
    std::uint64_t bump_generation_count = 0;
    std::uint64_t is_valid_check_count = 0;
    std::uint64_t stable_ref_invalidations = 0;
    std::uint64_t atomic_batch_commits = 0;
    // Issue #247: SyntaxMarker distribution in the current
    // workspace. Populated by CompilerService::snapshot() by
    // walking workspace_flat_->marker_column() (when set).
    // - marker_user_count: nodes written by the user
    // - marker_macro_introduced_count: nodes inserted by hygienic
    //   macros (clone_macro_body)
    // - marker_bool_literal_count: auto-generated #t / #f nodes
    // - marker_total_count: total nodes in the marker column
    std::uint64_t marker_user_count = 0;
    std::uint64_t marker_macro_introduced_count = 0;
    std::uint64_t marker_bool_literal_count = 0;
    std::uint64_t marker_total_count = 0;
    // Per-function metrics (built up from jit_cache_ + shape metrics)
    std::vector<FnMetrics> functions;
};

} // namespace aura::compiler

#endif // AURA_COMPILER_OBSERVABILITY_SNAPSHOT_H
