// hot_update_registry.hh — Issue #1956 / #2014 / #2035 / #2046
// Unified coordination center for hot-update / incremental re-emit
// callbacks, region mask, epoch listeners, and aggregated metrics.
//
// Existing C-linkage entry points (aura_set_reemit_candidate_fn,
// aura_set_aot_emit_fn, aura_set_is_define_dirty_fn,
// aura_set_aot_emit_region_mask, stable func_id map) remain the
// process ABI. This registry:
//   1. records every registration for observability
//   2. owns dynamic epoch-bump listeners (plugin/agent extension)
//   3. provides notify_dirty_define / notify_epoch_bump fan-out
//   4. exposes hot_update_registry_* counters for dashboards
//   5. Issue #2014: sliding-window deopt storm detection + reemit throttle
//   6. Issue #2035: cascade dirty → region-mask reemit bookkeeping
//   7. Issue #2046: joint AOT/JIT versioning — notify_epoch_bump is
//      also called from aura_aot_bump_func_table_epoch (invalidate
//      soft/hard) so listeners see the same epoch domain as JIT
//      capture_fn_epoch / AOT slot table_generation. See aot_mangle.h
//      "Joint versioning contract".
//
// MVP scope (#1943): single-workspace; no cross-COW migration.

#ifndef AURA_COMPILER_HOT_UPDATE_REGISTRY_HH
#define AURA_COMPILER_HOT_UPDATE_REGISTRY_HH

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "compiler/aura_jit_bridge.h" // Issue #2093: AotReloadFail enum

namespace aura::compiler {

// Sentinel epoch passed to epoch listeners when a deopt storm trips (#2014).
inline constexpr std::uint64_t kHotUpdateDeoptStormEpoch = ~std::uint64_t{0};

class HotUpdateRegistry {
public:
    using EpochListener = std::function<void(std::uint64_t epoch)>;
    using DirtyListener = std::function<void(const char* name)>;
    // Issue #2014: deopts_in_window + configured window_ms at trip time.
    using StormListener =
        std::function<void(std::uint64_t deopts_in_window, std::uint64_t window_ms)>;

    static HotUpdateRegistry& instance() noexcept;

    // ── registration bookkeeping (called from C setters) ──
    void on_reemit_provider_set(bool wired) noexcept;
    void on_define_dirty_provider_set(bool wired) noexcept;
    void on_aot_emit_provider_set(bool wired) noexcept;
    void on_emit_region_mask_set(std::uint64_t mask) noexcept;
    void on_stable_func_id_preserve(bool preserved) noexcept;
    void on_reemit_pipeline_call(std::uint64_t candidates, std::uint64_t successes) noexcept;
    // Issue #2012: atomic AOT reload success / rollback bookkeeping.
    void on_reload_success() noexcept;
    // Issue #2094: unified StormLevel facade. Combines
    // HotUpdateRegistry's sliding-window deopt storm (global reemit
    // throttle) with ShapeProfiler's shape-storm detector into a
    // single bitmask so SpecJITController / reemit entry can apply
    // one recovery policy without consulting two independent truth
    // values. Policy table (Issue #2094 AC5):
    //   - Global|Both → should_throttle_reemit() (existing #2014)
    //   - Shape|Both → SpecJIT / GuardShape conservative mode
    //     (existing shape-storm path)
    //   - None → normal flow
    // Counters are NOT merged — each detector keeps its own
    // lineage / thresholds / metrics. Only the decision is unified.
    enum class StormLevel : std::uint8_t {
        None = 0, // bit 0 = shape, bit 1 = global
        Shape = 1,
        Global = 2,
        Both = 3,
    };
    // Reads both detectors and returns the combined bitmask.
    [[nodiscard]] StormLevel current_storm_level() const noexcept;
    // Issue #2094: setter for ShapeProfiler (or tests) to publish its
    // deopt_storm_active state. Bridge reads it via the facade above
    // without needing to import shape_profiler.h.
    void set_shape_storm_active(bool active) noexcept;
    [[nodiscard]] bool shape_storm_active() const noexcept;
    // Issue #2093: reason-aware rollback hook. The per-reason atomic
    // counter + last-reason file-scope atomic are bumped here so the
    // Agent snapshot (taken via get_snapshot / get_stats_snapshot) can
    // distinguish Version vs Region vs Env failures for recovery
    // policy. on_reload_rollback() (no-arg) is kept as a thin wrapper
    // for callers that don't have a reason.
    void on_reload_rollback(AotReloadFail reason) noexcept;
    void on_reload_rollback() noexcept;
    // Issue #2013: live closures remapped after reemit (count of slots).
    void on_live_closure_remap(std::uint64_t count) noexcept;
    // Issue #2016: adaptive region-mask bit clear/restore.
    void on_region_mask_adapt_clear(std::uint64_t region) noexcept;
    void on_region_mask_adapt_restore(std::uint64_t region) noexcept;

    // Issue #2014: feed one deopt observation (from aura_deopt_inc).
    // Hot path: relaxed atomics only; clock read amortized to window edges.
    void on_stale_deopt() noexcept;
    // When true, reemit pipeline should skip this call (coalesce / delay).
    [[nodiscard]] bool should_throttle_reemit() const noexcept;
    // Note a reemit that was skipped due to throttle (observability).
    void on_reemit_throttled() noexcept;
    // Configure storm threshold (default 1000 deopts / 100 ms).
    void set_deopt_storm_threshold(std::uint64_t deopts_per_window,
                                   std::uint64_t window_ms) noexcept;
    [[nodiscard]] std::uint64_t deopt_storm_threshold() const noexcept;
    [[nodiscard]] std::uint64_t deopt_storm_window_ms() const noexcept;
    // Test / recovery: clear throttle + open a fresh window.
    void reset_deopt_storm_state_for_test() noexcept;

    // ── preferred C++ API (forwards to C ABI + bookkeeping) ──
    void set_emit_region_mask(std::uint64_t mask) noexcept;
    [[nodiscard]] std::uint64_t emit_region_mask() const noexcept;

    // Issue #2035: host reemit / AOT emit wiring probes (for cascade path).
    [[nodiscard]] bool reemit_provider_wired() const noexcept {
        return reemit_wired_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool aot_emit_provider_wired() const noexcept {
        return aot_emit_wired_.load(std::memory_order_relaxed);
    }
    // Issue #2035: bookkeeping when cascade derives a region mask from
    // block_dirty_ / SoA columns and optionally triggers reemit.
    void on_region_mask_from_dirty(std::uint64_t mask) noexcept;
    void on_cascade_reemit_trigger(std::uint64_t candidates_hint = 0) noexcept;

    // Dynamic listeners (not process-ABI; for tests / agents / plugins).
    // Returns listener id (stable until clear).
    std::uint64_t register_epoch_listener(EpochListener fn);
    std::uint64_t register_dirty_listener(DirtyListener fn);
    std::uint64_t register_storm_listener(StormListener fn);
    void clear_listeners() noexcept;

    void notify_epoch_bump(std::uint64_t epoch) noexcept;
    void notify_dirty_define(const char* name) noexcept;

    // ── snapshot for query:hot-update-registry-stats ──
    struct Snapshot {
        std::int64_t schema = 1956;
        std::int64_t issue = 1956;
        std::int64_t active = 1;
        std::int64_t reemit_provider_wired = 0;
        std::int64_t define_dirty_provider_wired = 0;
        std::int64_t aot_emit_provider_wired = 0;
        std::int64_t emit_region_mask = 0;
        std::int64_t epoch_listeners = 0;
        std::int64_t dirty_listeners = 0;
        std::int64_t register_calls_total = 0;
        std::int64_t epoch_notify_total = 0;
        std::int64_t dirty_notify_total = 0;
        std::int64_t reemit_pipeline_calls_total = 0;
        std::int64_t reemit_candidates_total = 0;
        std::int64_t reemit_success_total = 0;
        std::int64_t stable_id_preserve_total = 0;
        std::int64_t stable_id_assign_total = 0;
        std::int64_t stable_func_id_map_size = 0;
        // Issue #2012: atomic reload recovery counters.
        std::int64_t aot_reload_success_total = 0;
        std::int64_t aot_reload_rollback_total = 0;
        // Issue #2093: per-reason reload-failure breakdown (refine #2012).
        // Mirrors CompilerMetrics counters — Agent reads these to branch
        // on a recovery policy without parsing logs. Aggregate
        // aot_reload_rollback_total is unchanged so existing dashboards
        // keep working. Last-fail reason is the most recent reload's
        // failure enum (Ok when the last attempt succeeded).
        std::int64_t aot_reload_fail_dlopen_total = 0;
        std::int64_t aot_reload_fail_version_total = 0;
        std::int64_t aot_reload_fail_region_total = 0;
        std::int64_t aot_reload_fail_defuse_total = 0;
        std::int64_t aot_reload_fail_env_total = 0;
        std::int64_t aot_reload_fail_linear_total = 0;
        std::int64_t aot_reload_fail_staging_total = 0;
        std::int64_t aot_reload_fail_other_total = 0;
        std::int64_t aot_reload_last_fail_reason = 0; // AotReloadFail enum value
        // Issue #2013: live closure remaps after reemit.
        std::int64_t live_closure_remap_total = 0;
        // Issue #2014: deopt storm detection + throttle.
        std::int64_t deopt_storm_detected_total = 0;
        std::int64_t deopt_observed_total = 0;
        std::int64_t deopt_window_count = 0;
        std::int64_t deopt_storm_threshold = 1000;
        std::int64_t deopt_storm_window_ms = 100;
        std::int64_t reemit_throttle_active = 0;
        std::int64_t reemit_throttle_skips_total = 0;
        std::int64_t storm_listeners = 0;
        // Issue #2016: adaptive region mask.
        std::int64_t region_mask_adapt_clears_total = 0;
        std::int64_t region_mask_adapt_restores_total = 0;
        std::int64_t emit_region_mask_preferred = 0;
        // Issue #2035: cascade dirty → region-mask reemit.
        std::int64_t region_mask_from_dirty_total = 0;
        std::int64_t cascade_reemit_trigger_total = 0;
        std::int64_t last_region_mask_from_dirty = 0;
        std::int64_t schema_2035 = 2035;
        std::int64_t issue_2035 = 2035;
        // Issue #2094: unified StormLevel facade result (uint8_t enum).
        // Agents read this as a single recovery-policy signal rather
        // than ORing two independent detectors.
        std::int64_t storm_level = 0; // StormLevel: None=0/Shape=1/Global=2/Both=3
    };
    [[nodiscard]] Snapshot snapshot() const noexcept;

    // Process-wide counters (also mirrored into CompilerMetrics when available).
    [[nodiscard]] std::uint64_t register_calls_total() const noexcept {
        return register_calls_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t epoch_notify_total() const noexcept {
        return epoch_notify_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t dirty_notify_total() const noexcept {
        return dirty_notify_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t deopt_storm_detected_total() const noexcept {
        return deopt_storm_detected_.load(std::memory_order_relaxed);
    }

private:
    HotUpdateRegistry() = default;

    void notify_deopt_storm_locked(std::uint64_t deopts_in_window,
                                   std::uint64_t window_ms) noexcept;

    mutable std::mutex listeners_mtx_;
    std::vector<EpochListener> epoch_listeners_;
    std::vector<DirtyListener> dirty_listeners_;
    std::vector<StormListener> storm_listeners_;
    std::uint64_t next_listener_id_{1};

    std::atomic<bool> reemit_wired_{false};
    std::atomic<bool> define_dirty_wired_{false};
    std::atomic<bool> aot_emit_wired_{false};
    std::atomic<std::uint64_t> emit_region_mask_{0};

    std::atomic<std::uint64_t> register_calls_{0};
    std::atomic<std::uint64_t> epoch_notify_{0};
    std::atomic<std::uint64_t> dirty_notify_{0};
    std::atomic<std::uint64_t> reemit_pipeline_calls_{0};
    std::atomic<std::uint64_t> reemit_candidates_{0};
    std::atomic<std::uint64_t> reemit_success_{0};
    std::atomic<std::uint64_t> stable_id_preserve_{0};
    std::atomic<std::uint64_t> stable_id_assign_{0};
    std::atomic<std::uint64_t> aot_reload_success_{0};  // #2012
    std::atomic<std::uint64_t> aot_reload_rollback_{0}; // #2012
    std::atomic<std::uint64_t> live_closure_remap_{0};  // #2013
    // Issue #2093: per-reason rollback counters + last-reason mirror.
    // The last-reason atomic is duplicated with aura_jit_bridge.cpp's
    // g_last_reload_fail_reason so callers without direct bridge access
    // (e.g. query:aot-reload-stats snapshot readers) can still branch
    // on the most recent failure without parsing logs.
    std::atomic<std::uint64_t> aot_reload_fail_dlopen_{0};     // #2093
    std::atomic<std::uint64_t> aot_reload_fail_version_{0};    // #2093
    std::atomic<std::uint64_t> aot_reload_fail_region_{0};     // #2093
    std::atomic<std::uint64_t> aot_reload_fail_defuse_{0};     // #2093
    std::atomic<std::uint64_t> aot_reload_fail_env_{0};        // #2093
    std::atomic<std::uint64_t> aot_reload_fail_linear_{0};     // #2093
    std::atomic<std::uint64_t> aot_reload_fail_staging_{0};    // #2093
    std::atomic<std::uint64_t> aot_reload_fail_other_{0};      // #2093
    std::atomic<std::uint8_t> last_aot_reload_fail_reason_{0}; // #2093 (AotReloadFail enum)

    // Issue #2014: sliding window deopt rate.
    std::atomic<std::uint64_t> deopt_window_start_ms_{0};
    std::atomic<std::uint64_t> deopt_window_count_{0};
    std::atomic<std::uint64_t> deopt_observed_total_{0};
    std::atomic<std::uint64_t> deopt_storm_detected_{0};
    std::atomic<std::uint64_t> deopt_storm_threshold_{1000};
    std::atomic<std::uint64_t> deopt_storm_window_ms_{100};
    std::atomic<bool> reemit_throttled_{false};
    std::atomic<std::uint64_t> reemit_throttle_skips_{0};
    // Issue #2094: ShapeProfiler publishes its deopt_storm_active
    // state here so current_storm_level() can OR both detectors
    // without importing shape_profiler.h.
    std::atomic<bool> shape_storm_active_{false};
    std::atomic<std::uint64_t> region_mask_adapt_clears_{0};   // #2016
    std::atomic<std::uint64_t> region_mask_adapt_restores_{0}; // #2016
    // Issue #2035
    std::atomic<std::uint64_t> region_mask_from_dirty_total_{0};
    std::atomic<std::uint64_t> cascade_reemit_trigger_total_{0};
    std::atomic<std::uint64_t> last_region_mask_from_dirty_{0};
};

// Free functions for C bridge (no C++ class in extern "C" bodies).
inline HotUpdateRegistry& hot_update_registry() noexcept {
    return HotUpdateRegistry::instance();
}

} // namespace aura::compiler

// C-linkage snapshot for module TUs (cannot attach HotUpdateRegistry to a
// module partition — Issue #1956 link discipline).
extern "C" {
struct aura_hot_update_registry_snapshot {
    std::int64_t schema;
    std::int64_t issue;
    std::int64_t active;
    std::int64_t reemit_provider_wired;
    std::int64_t define_dirty_provider_wired;
    std::int64_t aot_emit_provider_wired;
    std::int64_t emit_region_mask;
    std::int64_t epoch_listeners;
    std::int64_t dirty_listeners;
    std::int64_t register_calls_total;
    std::int64_t epoch_notify_total;
    std::int64_t dirty_notify_total;
    std::int64_t reemit_pipeline_calls_total;
    std::int64_t reemit_candidates_total;
    std::int64_t reemit_success_total;
    std::int64_t stable_id_preserve_total;
    std::int64_t stable_id_assign_total;
    std::int64_t stable_func_id_map_size;
    std::int64_t aot_reload_success_total;  // #2012
    std::int64_t aot_reload_rollback_total; // #2012
    // Issue #2093: per-reason breakdown + last-fail reason.
    std::int64_t aot_reload_fail_dlopen_total;
    std::int64_t aot_reload_fail_version_total;
    std::int64_t aot_reload_fail_region_total;
    std::int64_t aot_reload_fail_defuse_total;
    std::int64_t aot_reload_fail_env_total;
    std::int64_t aot_reload_fail_linear_total;
    std::int64_t aot_reload_fail_staging_total;
    std::int64_t aot_reload_fail_other_total;
    std::int64_t aot_reload_last_fail_reason;
    // Issue #2094: unified StormLevel facade result (uint8_t enum).
    // Agents read this as a single recovery-policy signal rather than
    // ORing two independent detectors.
    std::int64_t storm_level;              // StormLevel: None=0/Shape=1/Global=2/Both=3
    std::int64_t live_closure_remap_total; // #2013
    // Issue #2014
    std::int64_t deopt_storm_detected_total;
    std::int64_t deopt_observed_total;
    std::int64_t deopt_window_count;
    std::int64_t deopt_storm_threshold;
    std::int64_t deopt_storm_window_ms;
    std::int64_t reemit_throttle_active;
    std::int64_t reemit_throttle_skips_total;
    std::int64_t storm_listeners;
    std::int64_t region_mask_adapt_clears_total;   // #2016
    std::int64_t region_mask_adapt_restores_total; // #2016
    std::int64_t emit_region_mask_preferred;       // #2016
    // Issue #2035
    std::int64_t region_mask_from_dirty_total;
    std::int64_t cascade_reemit_trigger_total;
    std::int64_t last_region_mask_from_dirty;
    std::int64_t schema_2035;
    std::int64_t issue_2035;
};
void aura_hot_update_registry_get_snapshot(aura_hot_update_registry_snapshot* out);
// Issue #2014: C entry points for deopt feed / throttle / config.
void aura_hot_update_note_deopt(void);
int aura_hot_update_should_throttle_reemit(void);
void aura_hot_update_on_reemit_throttled(void);

// Issue #2094: StormLevel facade accessor (C ABI). Returns the
// combined bitmask of shape-storm + global-deopt-storm detectors
// so external callers can branch on a single recovery-policy value.
// Result mapping (uint8_t): 0=None, 1=Shape, 2=Global, 3=Both.
extern "C" std::uint8_t aura_hot_update_current_storm_level(void);

// Issue #2094: setter for ShapeProfiler (or tests) to publish its
// deopt_storm_active state without needing to import shape_profiler.h.
extern "C" void aura_hot_update_set_shape_storm_active(int active);
void aura_hot_update_set_deopt_storm_threshold(std::uint64_t deopts_per_window,
                                               std::uint64_t window_ms);
void aura_hot_update_reset_deopt_storm_state_for_test(void);
// Issue #2017: module-safe C entry for epoch notify (compact-env-frames etc.).
// Module partitions cannot attach HotUpdateRegistry (link discipline #1956).
void aura_hot_update_notify_epoch_bump(std::uint64_t epoch);
// Issue #2035: module-safe dirty notify + reemit-provider probe.
void aura_hot_update_notify_dirty_define(const char* name);
int aura_hot_update_reemit_provider_wired(void);
}

#endif // AURA_COMPILER_HOT_UPDATE_REGISTRY_HH
