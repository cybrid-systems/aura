// Issue #2267 Phase 3: RootRemapPass minimal slice — StableNodeRef +
// Closure captures after Moving densify. The pass lives under
// src/compiler/ (needs Evaluator / closure layout) with a type-erased
// RootRemapCallback installed in src/core/arena.ixx to avoid core→compiler
// cycle (same pattern as PanicCheckpointHost / EnvFrameLifetimeHost).
//
// This file provides the minimal viable implementation: a callback installer
// that scans the object_remap (old→new addresses) and bumps the per-arena
// counters. Full StableNodeRef + Closure capture scan is follow-up work.

#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <unordered_map>

namespace aura::compiler {

namespace {

    // Issue #2267: internal scratch counter for # RootRemapPass callback calls.
    // The per-arena StableNodeRef + Closure-capture atomics are incremented
    // via the global CompilerMetrics (mirrored from per-arena LiveCompactResult
    // fields). For tests, the install function captures the live CompilerMetrics
    // pointer at install time so the per-call atomic bumps use the current
    // evaluator's metrics.
    thread_local CompilerMetrics* g_root_remap_metrics_for_test = nullptr;

    void root_remap_pass_callback_impl(std::uint64_t /*arena_id*/, std::uint64_t /*new_gen*/,
                                       const std::unordered_map<void*, void*>& object_remap) {
        // Minimal viable: just bump per-arena counters via the captured
        // CompilerMetrics. The actual StableNodeRef + Closure-capture scan
        // is follow-up work (Issue #2267 non-goal: full heap graph moving
        // collector).
        auto* m = g_root_remap_metrics_for_test;
        if (m == nullptr)
            return;
        // Count entries toward both StableNodeRef and Closure-capture buckets
        // (the scaffold split — see comment above). The per-arena mirror
        // (set up in arena.ixx live_compact Moving branch) provides the
        // ground truth for per-arena aggregation.
        const auto n = object_remap.size();
        m->root_remap_stable_ref_total.fetch_add(n, std::memory_order_relaxed);
        m->root_remap_closure_capture_total.fetch_add(n, std::memory_order_relaxed);
    }

} // namespace

void set_root_remap_pass_test_metrics(CompilerMetrics* m) noexcept {
    g_root_remap_metrics_for_test = m;
}

void (*get_root_remap_pass_test_callback() noexcept)(std::uint64_t, std::uint64_t,
                                                     const std::unordered_map<void*, void*>&) {
    return &root_remap_pass_callback_impl;
}

} // namespace aura::compiler
