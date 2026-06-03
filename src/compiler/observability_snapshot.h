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
    // Per-function metrics (built up from jit_cache_ + shape metrics)
    std::vector<FnMetrics> functions;
};

} // namespace aura::compiler

#endif // AURA_COMPILER_OBSERVABILITY_SNAPSHOT_H
