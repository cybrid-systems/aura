// shape_profiler.h — Shape profiling infrastructure
//
// Tracks value shapes per function, detects stability,
// and records shape change metrics for the auto-evolution engine.
//
// Phase 1: Shape infrastructure (#53 Shape-based Speculative JIT)
//
#ifndef AURA_COMPILER_SHAPE_PROFILER_H
#define AURA_COMPILER_SHAPE_PROFILER_H

#include <cstdint>
// Issue #337: std::flat_map (C++23) for the
// profiles_ container. Better cache locality
// than std::unordered_map for the small-to-medium
// profile sets typical in shape-stable code. The
// flat_map keeps entries in sorted order (small
// overhead vs hash) but avoids the per-lookup
// cache miss pattern of unordered_map's
// hash-bucket traversal.
#include <flat_map>
#include <vector>
#include <string>
#include "shape.h"

namespace aura::compiler::shape {

// ── ShapeProfiler ─────────────────────────────────────────────
// Records shape observations per function and determines shape stability.
//
// Thread safety: NOT thread-safe by design. Must be called from the
// single eval thread (consistent with Aura's single-threaded eval model).
class ShapeProfiler {
public:
    ShapeProfiler();

    // ── Record a shape observation ─────────────────────────────
    // Called after each eval of function `fn` with the observed shape ID.
    // Returns true if the shape is now considered stable.
    bool record_shape(FnKey fn, ShapeID shape_id) pre(shape_id != 0);

    // ── Check stability ────────────────────────────────────────
    bool is_stable(FnKey fn) const;
    ShapeID dominant_shape(FnKey fn) const;

    // ── Snapshot for guard comparison ──────────────────────────
    // Returns the current shape snapshot for a function.
    // The version increases monotonically and is bumped by invalidate().
    ShapeSnapshot current_snapshot(FnKey fn) const;

    // ── Invalidate after mutate:* ──────────────────────────────
    // Called when mutate:* modifies function `fn`.
    // Resets stability state and increments version.
    void invalidate(FnKey fn) pre(fn != 0);

    // ── Metrics ────────────────────────────────────────────────
    ShapeFnMetrics metrics(FnKey fn) const;

    // ── Reset all state ───────────────────────────────────────
    void reset();

    // ── Access all tracked keys ───────────────────────────────
    std::vector<FnKey> tracked_fns() const;

    // ── Configuration ─────────────────────────────────────────
    static constexpr std::uint32_t kDefaultWindowSize = 1000;
    static constexpr double kDefaultStabilityRatio = 0.90;
    static constexpr std::uint32_t kStableThreshold = 100;

    void set_window_size(std::uint32_t n) { window_size_ = n; }
    void set_stability_ratio(double r) { stability_ratio_ = r; }

private:
    struct ShapeRecord {
        ShapeID shape_id;
        std::uint64_t timestamp;
    };

    struct FnProfile {
        std::vector<ShapeRecord> history; // sliding window
        std::uint64_t total_calls = 0;
        std::uint64_t version = 0;
        std::uint64_t deopt_count = 0;
        bool is_stable = false;
        ShapeID stable_shape = SHAPE_UNKNOWN;
        std::uint64_t last_metric_time = 0;

        // Dominant shape in the window
        ShapeID compute_dominant() const;
    };

    // Issue #337: std::flat_map (C++23) for the
    // profiles_ container. The flat_map's sorted
    // iteration matches the per-Fn history window
    // access pattern (the profiling engine iterates
    // profiles to detect stability across functions
    // — sorted access is more cache-friendly than
    // hash-bucket iteration).
    std::flat_map<FnKey, FnProfile> profiles_;
    std::uint32_t window_size_ = kDefaultWindowSize;
    double stability_ratio_ = kDefaultStabilityRatio;
    std::uint64_t global_time_ = 0;
};

} // namespace aura::compiler::shape

#endif // AURA_COMPILER_SHAPE_PROFILER_H
