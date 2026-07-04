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
#include <functional>
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
    // Resets stability state, increments version, fires deopt hook.
    // Returns true when the profile was stable (needs deopt/refresh).
    bool invalidate(FnKey fn) pre(fn != 0);

    // Issue #570: invalidate every tracked profile (mutation path).
    // Unlike reset(), preserves profiles and bumps version per fn.
    void invalidate_all() noexcept;

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

    // Issue #686: optional dirty-scope callback (IRSoA / block_dirty_).
    void set_dirty_hook(std::function<void(FnKey fn, std::uint32_t dirty_scope)> hook) {
        dirty_hook_ = std::move(hook);
    }

private:
    struct ShapeRecord {
        ShapeID shape_id;
        std::uint64_t timestamp;
    };

    // Issue #686: fixed-capacity ring buffer — O(1) push vs vector erase(begin).
    struct ShapeHistoryRing {
        std::vector<ShapeRecord> slots;
        std::uint32_t head = 0;
        std::uint32_t count = 0;

        void clear() noexcept {
            head = 0;
            count = 0;
        }

        void ensure_capacity(std::uint32_t cap) {
            if (slots.size() != cap)
                slots.resize(cap);
        }

        void push(const ShapeRecord& rec, std::uint32_t window_size);

        [[nodiscard]] std::uint32_t size() const noexcept { return count; }

        template <typename F> void for_each(F&& f) const {
            if (count == 0)
                return;
            if (count < slots.size()) {
                for (std::uint32_t i = 0; i < count; ++i)
                    f(slots[i]);
                return;
            }
            const auto cap = static_cast<std::uint32_t>(slots.size());
            for (std::uint32_t i = 0; i < cap; ++i) {
                const auto idx = (head + i) % cap;
                f(slots[idx]);
            }
        }
    };

    struct FnProfile {
        ShapeHistoryRing history;
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
    std::function<void(FnKey fn, std::uint32_t dirty_scope)> dirty_hook_;
};

// Issue #570 / #686: deopt hook fired on version bump / stable→unstable.
// dirty_scope: 0 = advisory, 1 = stability loss, 2 = explicit invalidate.
using ShapeDeoptHook = void (*)(FnKey fn, std::uint64_t version, std::uint32_t dirty_scope);

inline constexpr std::uint32_t kShapeDirtyScopeStabilityLoss = 1;
inline constexpr std::uint32_t kShapeDirtyScopeInvalidate = 2;

void set_shape_deopt_hook(ShapeDeoptHook hook) noexcept;
[[nodiscard]] ShapeDeoptHook shape_deopt_hook() noexcept;

} // namespace aura::compiler::shape

#endif // AURA_COMPILER_SHAPE_PROFILER_H
