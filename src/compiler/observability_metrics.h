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
};

// Per-function metrics, returned by CompilerService::snapshot()
// for --evo-explain. Reflect-friendly.
struct FnMetrics {
    std::string name;                // function name
    std::uint64_t total_calls = 0;   // total invocations observed
    std::uint64_t deopt_count = 0;   // deopt count (subset of total)
    std::uint64_t hit_count = 0;     // specialization hit (specialized path)
    std::uint64_t miss_count = 0;    // specialization miss (generic path)
    double hit_rate = 0.0;           // hit_count / (hit_count + miss_count)
    bool has_shape_map = false;      // was compiled with shape_map?
    std::uint32_t specialized_for = 0; // Issue #61: shape ID
};

} // namespace aura::compiler

#endif // AURA_COMPILER_OBSERVABILITY_METRICS_H
