// shape_profiler.h — Shape profiling infrastructure
//
// Tracks value shapes per function, detects stability,
// and records shape change metrics for the auto-evolution engine.
//
// Phase 1: Shape infrastructure (#53 Shape-based Speculative JIT)
//
#ifndef AURA_COMPILER_SHAPE_PROFILER_H
#define AURA_COMPILER_SHAPE_PROFILER_H

#include <atomic>
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
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>
#include "shape.h"

namespace aura::compiler::shape {

// ── ShapeProfiler ─────────────────────────────────────────────
// Records shape observations per function and determines shape stability.
//
// Issue #2141 — concurrency model A (reader/writer shared_mutex):
//   - Shared lock: is_stable / dominant_shape / current_snapshot /
//     metrics / tracked_fns / shape_stable_ratio / deopt_rate_per_fn /
//     profile_count (read-only walks of profiles_).
//   - Unique lock: record_shape / invalidate / invalidate_all /
//     on_arena_compact / on_boundary_or_fiber_sync / reset /
//     config setters that touch non-atomic state.
//   - Deopt-storm ring updates run under unique lock only.
//   - External deopt/dirty hooks fire *after* releasing the lock so
//     callbacks may re-enter without deadlock.
//   - Metrics atomics remain lock-free for publish; map integrity is
//     guarded by mtx_. Contention counted in shape_profiler_lock_contended_total.
// Multi-fiber mutate + eval + compact is production-supported under this model.
inline constexpr int kShapeProfilerConcurrencyIssue = 2141;

// Issue #2255: process-wide shape version accessor for
// LayoutStamp.shape_version capture in
// Evaluator::current_layout_stamp(). Reads the file-scope
// shape_version_bump_count atomic so all instances share a
// single monotonic counter (Arena compact / invalidate bump it
// globally; this is the source of truth for resume fence).
[[nodiscard]] inline std::uint64_t current_global_shape_version() noexcept {
    extern std::atomic<std::uint64_t> shape_version_bump_count;
    return shape_version_bump_count.load(std::memory_order_acquire);
}

// Issue #2257: bump the file-scope shape_version on deopt-storm
// enter. AC1: shape_version advances on storm enter (and on compact
// per #2256). AC2: high-frequency mutation + storm enter keeps
// deopt rate bounded because every storm enter isolates the
// speculative opt (next observation cycle must re-profile under
// the new version).
// Issue #2370: under StormIsolation::PerEval this must NOT be called
// from ShapeProfiler storm enter — use per-eval SpecJIT isolation
// epoch instead so concurrent evals are not cross-invalidated.
inline void bump_shape_version_on_storm_enter() noexcept {
    extern std::atomic<std::uint64_t> shape_version_bump_count;
    shape_version_bump_count.fetch_add(1, std::memory_order_acq_rel);
}

// Issue #2257: process-atomic deopt-storm-isolations counter
// (mirrors the per-CompilerMetrics field for pure unit tests).
inline std::atomic<std::uint64_t>& g_deopt_storm_isolations_total_atomic() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}

// Issue #2257: production-default HighMutation preset. Off for
// unit tests via AURA_SHAPE_HIGH_MUTATION=0 env override (test
// harness sets this in main()).
inline int shape_high_mutation_default_enabled() noexcept {
    if (const char* e = std::getenv("AURA_SHAPE_HIGH_MUTATION")) {
        if (e[0] == '0')
            return 0;
        if (e[0] == '1')
            return 1;
    }
    return 1; // production default ON
}

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

    // Issue #1521: Arena compact / defrag coordination.
    // Soft path for multi-round AI mutation under GC pressure:
    //   - Bumps version on every tracked profile (JIT guards notice)
    //   - Fires deopt hook with kShapeDirtyScopeArenaCompact
    //   - Preserves is_stable + history (value-tag shapes are address-
    //     independent; full invalidate_all would thrash deopt storms)
    //   - Does NOT feed the deopt-storm ring (compact is expected pressure)
    // Returns number of profiles touched.
    std::uint32_t on_arena_compact() noexcept;

    // Issue #1521: MutationBoundary / fiber-steal exit check.
    // After compact during an active boundary, re-assert stability ratio
    // and optionally soft-clear deopt_storm when only compact pressure
    // was observed (no mutation-induced churn). Returns shape_stable_ratio.
    double on_boundary_or_fiber_sync(bool clear_compact_only_storm = true) noexcept;

    // ── Metrics ────────────────────────────────────────────────
    ShapeFnMetrics metrics(FnKey fn) const;

    // Issue #1521: per-instance compact coordination counters.
    [[nodiscard]] std::uint64_t arena_compact_calls() const noexcept {
        return arena_compact_calls_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t arena_compact_stable_preserved() const noexcept {
        return arena_compact_stable_preserved_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t arena_compact_deopt_hooks() const noexcept {
        return arena_compact_deopt_hooks_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t boundary_fiber_sync_calls() const noexcept {
        return boundary_fiber_sync_calls_.load(std::memory_order_relaxed);
    }

    // ── Reset all state ───────────────────────────────────────
    void reset();

    // ── Access all tracked keys ───────────────────────────────
    std::vector<FnKey> tracked_fns() const;

    // ── Configuration ─────────────────────────────────────────
    static constexpr std::uint32_t kDefaultWindowSize = 1000;
    static constexpr double kDefaultStabilityRatio = 0.90;
    static constexpr std::uint32_t kStableThreshold = 100;

    void set_window_size(std::uint32_t n);
    void set_stability_ratio(double r);
    [[nodiscard]] std::uint32_t window_size() const noexcept;
    [[nodiscard]] double stability_ratio() const noexcept;

    // Issue #1468: AI high-mutation-rate preset. More conservative
    // stability judgement (larger window, lower dominant ratio, more
    // samples required) — designed for long-running self-modifying
    // workloads where shape churn is normal, not pathological.
    //
    // To activate:
    //   shape::ShapeProfiler sp;
    //   sp.apply_preset(ShapeProfiler::Preset::kHighMutation);
    // Or via env var AURA_SHAPE_PRESET=high_mutation in (serve).
    struct Preset {
        std::uint32_t window_size;
        double stability_ratio;
        std::uint32_t min_samples_for_stable;
        std::uint32_t deopt_storm_window;    // # of last mutations to consider
        std::uint32_t deopt_storm_threshold; // # deopts in window → storm
    };
    static constexpr Preset kDefaultPreset = {1000, 0.90, 100, 256, 4};
    static constexpr Preset kHighMutationPreset = {2000, 0.95, 250, 512, 6};
    static constexpr Preset kLowMutationPreset = {500, 0.85, 50, 128, 3};

    void apply_preset(Preset p);
    [[nodiscard]] Preset active_preset() const noexcept;
    [[nodiscard]] std::uint32_t min_samples_for_stable() const noexcept;
    [[nodiscard]] std::uint32_t deopt_storm_window() const noexcept;
    [[nodiscard]] std::uint32_t deopt_storm_threshold() const noexcept;

    // Issue #992: hard cap on tracked profiles (long-running serve).
    void set_max_profiles(std::size_t n);
    [[nodiscard]] std::size_t max_profiles() const noexcept;
    [[nodiscard]] std::size_t profile_count() const noexcept;
    [[nodiscard]] std::uint64_t profile_evictions() const noexcept;

    // Issue #1468: AI workload metrics. Atomic counters for the
    // (compile:shape-stability-stats) primitive + agent decision
    // metrics. Lifetime totals since ShapeProfiler ctor.
    [[nodiscard]] std::uint64_t mutation_induced_invalidations() const noexcept {
        return mutation_induced_invalidations_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t deopt_storm_total() const noexcept {
        return deopt_storm_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t history_hit_count() const noexcept {
        return history_hit_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t history_miss_count() const noexcept {
        return history_miss_count_.load(std::memory_order_relaxed);
    }

    // Issue #1468: deopt-storm detector. Returns true if the
    // last `deopt_storm_window_` deopts exceeded `deopt_storm_threshold_`.
    // Once true, callers (SpecJITController, GuardShape) should
    // down-shift to generic / raise threshold / drop the function.
    [[nodiscard]] bool deopt_storm_active() const noexcept {
        return deopt_storm_active_.load(std::memory_order_acquire);
    }

    // Issue #1468: shape_stable_ratio = (# fns in profiles_ with
    // is_stable=true) / total profiles_. 0 if no profiles yet.
    [[nodiscard]] double shape_stable_ratio() const noexcept;

    // Issue #1468: deopt_rate_per_fn = total deopt_count across
    // all profiles / total profile count. 0 if no profiles.
    [[nodiscard]] double deopt_rate_per_fn() const noexcept;

    // Issue #1468: history_hit_rate = history_hit_count / (hit+miss).
    // 0 if no history lookups yet.
    [[nodiscard]] double history_hit_rate() const noexcept;

    // Issue #686: optional dirty-scope callback (IRSoA / block_dirty_).
    void set_dirty_hook(std::function<void(FnKey fn, std::uint32_t dirty_scope)> hook);

    // Issue #2141: how often exclusive/shared lock had to wait (try_lock miss).
    [[nodiscard]] std::uint64_t lock_contended_total() const noexcept {
        return lock_contended_total_.load(std::memory_order_relaxed);
    }

private:
    // Issue #2141: lock helpers (try → contended++ → block).
    [[nodiscard]] std::unique_lock<std::shared_mutex> unique_lock_() const;
    [[nodiscard]] std::shared_lock<std::shared_mutex> shared_lock_() const;
    // Invalidate body without taking mtx_ (caller holds unique lock).
    bool invalidate_unlocked_(FnKey fn);
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
        std::uint64_t last_used = 0; // Issue #992

        // Dominant shape in the window
        ShapeID compute_dominant() const;
    };

    void maybe_evict_profiles_();
    // Issue #1468: update deopt-storm ring + active flag.
    void update_deopt_storm_state_(FnKey fn) noexcept;

    // Issue #2141: guards profiles_ / history / deopt ring / config knobs.
    mutable std::shared_mutex mtx_;
    mutable std::atomic<std::uint64_t> lock_contended_total_{0};

    // Issue #337: std::flat_map (C++23) for the
    // profiles_ container. The flat_map's sorted
    // iteration matches the per-Fn history window
    // access pattern (the profiling engine iterates
    // profiles to detect stability across functions
    // — sorted access is more cache-friendly than
    // hash-bucket iteration). Guarded by mtx_.
    std::flat_map<FnKey, FnProfile> profiles_;
    std::uint32_t window_size_ = kDefaultWindowSize;
    double stability_ratio_ = kDefaultStabilityRatio;
    std::uint64_t global_time_ = 0;
    std::size_t max_profiles_ = 4096; // Issue #992
    std::uint64_t profile_evictions_ = 0;
    // Issue #1468: per-instance deopt-event ring for storm detection.
    // Stores (timestamp, fn) for the last deopt_storm_window_ events.
    struct DeoptEvent {
        std::uint64_t time;
        FnKey fn;
    };
    std::vector<DeoptEvent> deopt_ring_;
    std::uint32_t deopt_ring_head_ = 0;
    std::uint32_t deopt_ring_count_ = 0;
    // Issue #1468: atomic counters for the metrics API.
    std::atomic<std::uint64_t> mutation_induced_invalidations_{0};
    std::atomic<std::uint64_t> deopt_storm_total_{0};
    std::atomic<std::uint64_t> history_hit_count_{0};
    std::atomic<std::uint64_t> history_miss_count_{0};
    std::atomic<bool> deopt_storm_active_{false};
    // Issue #1521: arena compact soft-path counters (per profiler).
    std::atomic<std::uint64_t> arena_compact_calls_{0};
    std::atomic<std::uint64_t> arena_compact_stable_preserved_{0};
    std::atomic<std::uint64_t> arena_compact_deopt_hooks_{0};
    std::atomic<std::uint64_t> boundary_fiber_sync_calls_{0};
    // True if last deopt-storm activation was driven only by compact
    // (should never happen — compact skips the ring; kept for API symmetry).
    std::atomic<bool> last_storm_from_compact_{false};
    // Issue #1468: configurable knobs (now driven by Preset).
    std::uint32_t min_samples_for_stable_ = 100;
    std::uint32_t deopt_storm_window_ = 256;
    std::uint32_t deopt_storm_threshold_ = 4;
    Preset active_preset_ = kDefaultPreset;
    std::function<void(FnKey fn, std::uint32_t dirty_scope)> dirty_hook_;
};

// Issue #570 / #686: deopt hook fired on version bump / stable→unstable.
// dirty_scope: 0 = advisory, 1 = stability loss, 2 = explicit invalidate.
using ShapeDeoptHook = void (*)(FnKey fn, std::uint64_t version, std::uint32_t dirty_scope);

inline constexpr std::uint32_t kShapeDirtyScopeStabilityLoss = 1;
inline constexpr std::uint32_t kShapeDirtyScopeInvalidate = 2;
// Issue #1521: Arena compact version bump / selective deopt (not mutation churn).
inline constexpr std::uint32_t kShapeDirtyScopeArenaCompact = 3;

void set_shape_deopt_hook(ShapeDeoptHook hook) noexcept;
[[nodiscard]] ShapeDeoptHook shape_deopt_hook() noexcept;

} // namespace aura::compiler::shape

#endif // AURA_COMPILER_SHAPE_PROFILER_H
