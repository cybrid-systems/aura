// hot_update_registry.hh — Issue #1956 / #2014 / #2035 / #2046 / #2114 / #2132
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
//   8. Issue #2114 / #2205 / #2208: HotUpdate reemit ↔ MutationBoundary
//      handshake. Reemit never races dual-epoch / linear / GC outside a
//      boundary. Policy for Agent / plugin authors (production #2205/#2208):
//        - **Production default Defer (#2205 / #2208)**: if reemit is invoked
//          outside a real MutationBoundary (depth==0 and !held), skip
//          the body, record reemit_deferred_for_boundary_total + pending
//          version; next outermost Guard exit (#2090) drains under lock.
//          SoftEnter is **not steal-safe** (TLS does not migrate with
//          the fiber) — multi-fiber production must not SoftEnter.
//        - SoftEnter (opt-in only: set_reemit_boundary_policy(SoftEnter)
//          or AURA_REEMIT_SOFT_ENTER=1 under apply_production_security /
//          unit tests): outside → TLS soft boundary for call duration;
//          bump reemit_outside_boundary_total + soft_entered_total.
//        - RequireRealBoundary (stricter #2205): outside → reject
//          without recording defer (no AOT mutation, no pending drain).
//        - Inside real boundary (depth>0 or held flag, including #2090
//          dtor window before held is cleared): proceed without soft
//          enter; never silent about outside paths (always count).
//
// MVP scope (#1943): single-workspace; no cross-COW migration.
// Issue #2178: cross-workspace / cross-COW hot-update is explicitly
// rejected at the reload + reemit entry points via
// aura_is_current_workspace_eval(eval_ptr). Foreign eval contexts (or
// when COW generation diverges) bump the
// cross_workspace_hot_update_rejected_total on the CompilerMetrics,
// surfaced on (query:hot-update-registry-stats). The MVP boundary is
// enforced until a future cross-COW migration design lands — the
// observable guard means a silent partial success is no longer possible
// in the multi-agent / multi-tenant host case.

#ifndef AURA_COMPILER_HOT_UPDATE_REGISTRY_HH
#define AURA_COMPILER_HOT_UPDATE_REGISTRY_HH

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
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
    // Issue #2502: after force-JIT demotion, auto re-promote when a
    // consecutive clean-success window is met (StormLevel::None,
    // attempts_left idle, optional pending_dirty idle). Soft zero-cost
    // when force_jit_regions_mask_ is already 0. Clears mask bits on
    // match; does not change fall_back_jit_only exhaust semantics.
    void maybe_force_jit_repromote_on_clean_success() noexcept;
    void set_force_jit_repromote_window(std::uint32_t n) noexcept;
    [[nodiscard]] std::uint32_t force_jit_repromote_window() const noexcept;
    void set_force_jit_repromote_require_pending_idle(bool require) noexcept;
    [[nodiscard]] bool force_jit_repromote_require_pending_idle() const noexcept;
    [[nodiscard]] std::uint32_t force_jit_stable_successes() const noexcept;
    [[nodiscard]] std::uint64_t force_jit_repromote_total() const noexcept;
    [[nodiscard]] std::uint8_t last_force_jit_repromote_reason() const noexcept;
    [[nodiscard]] std::uint64_t last_force_jit_repromote_at_epoch_notify() const noexcept;
    // Test isolation: reset streak / totals / window defaults without
    // touching force_jit_regions_mask_ (use on_reload_success for that).
    void reset_force_jit_repromote_for_test() noexcept;
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
    // Issue #2232: policy fall_back_jit_only after multi-round reload
    //    exhausted. The actual slot-level physical invalidate is wired in
    //    aura_jit_bridge.cpp::aura_aot_invalidate_all_stale_slots_for_eval
    //    (Issue #2271 / #2299 per-eval filter) so this callback is the
    //    visible registry hook for Agents + observability, while the
    //    bridge clears matching live func-table slots atomically.
    // exhaustion. Records the final fail reason so Agents can observe
    // JIT-only fall-back without a silent partial success. Slot-level
    // AOT invalidation is a future follow-up; this is the visible
    // registry callback + counter contract for #2232.
    void on_force_jit_for_reason(AotReloadFail reason) noexcept;
    // Issue #2013: live closures remapped after reemit (count of slots).
    void on_live_closure_remap(std::uint64_t count) noexcept;
    // Issue #2016: adaptive region-mask bit clear/restore.
    void on_region_mask_adapt_clear(std::uint64_t region) noexcept;
    void on_region_mask_adapt_restore(std::uint64_t region) noexcept;

    // Issue #2014: feed one deopt observation (from aura_deopt_inc).
    // Hot path: relaxed atomics only; clock read amortized to window edges.
    void on_stale_deopt() noexcept;
    // Issue #2236: region-aware feed (used when StormIsolation::PerRegion
    // is configured). When Global (default), routes to no-arg form so the
    // soft/hard atomics stay the single process-wide window. When PerRegion,
    // feeds the per-region window map (bounded cap of 64 entries;
    // overflow falls back to global to bound memory per the issue note).
    void on_stale_deopt(std::uint64_t region) noexcept;
    // When true, reemit pipeline should skip this call (coalesce / delay).
    // No-arg form is the process-global soft-storm flag (#2014 / StormLevel).
    [[nodiscard]] bool should_throttle_reemit() const noexcept;
    // Issue #2132: region / priority-aware decision.
    // Soft storm: critical_region_mask bits that overlap region_or_priority
    // bypass throttle (allow reemit). Hard storm always throttles.
    // region_or_priority == 0 → treat as non-critical (global throttle).
    [[nodiscard]] bool should_throttle_reemit(std::uint64_t region_or_priority) const noexcept;
    // True when region_or_priority overlaps critical_region_mask (nonzero).
    [[nodiscard]] bool is_critical_region(std::uint64_t region_or_priority) const noexcept;
    // True when hard-storm ceiling is active (no critical bypass).
    [[nodiscard]] bool hard_storm_active() const noexcept;
    // Note a reemit that was skipped due to throttle (observability).
    // No-arg counts as global soft skip (legacy).
    void on_reemit_throttled() noexcept;
    // Issue #2132: reason-tagged skip / bypass counters.
    enum class ThrottleReason : std::uint8_t {
        Global = 0,         // soft storm, non-critical / unknown region
        Region = 1,         // soft storm, known non-critical region bits
        Hard = 2,           // hard ceiling — no bypass
        CriticalBypass = 3, // allowed despite soft storm
    };
    // Issue #2236 / #2370: optional per-region / per-eval deopt-storm
    // isolation. Default = Global = process-wide sliding window
    // (backwards compat — single-workspace MVP). PerRegion = per-region
    // sliding windows with bounded cap (64 entries, overflow → global).
    // PerEval (#2370 real): windows keyed by TLS storm eval context
    // (aura_set_storm_eval_context) so concurrent evals do not share
    // storm windows; SpecJIT isolation epoch is per-controller.
    // StormLevel facade + critical region bypass from #2132 preserved.
    enum class StormIsolation : std::uint8_t {
        Global = 0,    // process-wide window (today's behavior)
        PerRegion = 1, // per-region windows; bounded cap; overflow → global
        PerEval = 2,   // per-evaluator windows (#2370)
    };
    static constexpr std::uint64_t kStormIsolationRegionCap = 64;
    void on_reemit_throttled(ThrottleReason reason) noexcept;
    void on_reemit_critical_bypass() noexcept;
    // Issue #2236 / #2370: StormIsolation mode setter / getter. Default =
    // Global. PerRegion activates per-region windows; PerEval activates
    // per-eval windows + SpecJIT isolation epoch (#2370).
    // Hot-path readers (on_stale_deopt / should_throttle_reemit) read the
    // atomic once per call (relaxed, 1 load).
    void set_storm_isolation_mode(StormIsolation mode) noexcept;
    [[nodiscard]] StormIsolation storm_isolation_mode() const noexcept;
    // Number of regions in the per-region window map (size of
    // region_windows_). Used by tests + Agent dashboards to verify the
    // bounded cap is respected.
    [[nodiscard]] std::uint64_t storm_isolation_region_count() const noexcept;


    // Issue #2274: cap overflow bumper + getter.

    void bump_deopt_storm_region_overflow_total() noexcept;

    [[nodiscard]] std::uint64_t deopt_storm_region_overflow_total() const noexcept;
    // Last region id that tripped a per-region storm. 0 when no region
    // has tripped (default). Read via the snapshot as
    // deopt_storm_region_last_id.
    [[nodiscard]] std::uint64_t deopt_storm_region_last_id() const noexcept;
    // Number of per-region storm trips (cumulative). Read via the
    // snapshot as deopt_storm_region_detected_total.
    [[nodiscard]] std::uint64_t deopt_storm_region_detected_total() const noexcept;
    // Reset per-region windows for tests (clears the map + trips + last id).
    // Does NOT touch global atomics (use reset_deopt_storm_state_for_test
    // for that); this is the per-region cleanup hook.
    void reset_region_storm_windows_for_test() noexcept;
    // Test helper: bump per-region deopt count by `n` directly (skips
    // the on_stale_deopt gate and writes to region_windows_[region]).
    // Bumps `deopt_observed_total_` and `deopt_storm_region_detected_total_`
    // when the count exceeds threshold.
    void test_pump_deopt_for_region(std::uint64_t region, std::uint64_t n) noexcept;
    // Configure storm threshold (default 1000 deopts / 100 ms).
    void set_deopt_storm_threshold(std::uint64_t deopts_per_window,
                                   std::uint64_t window_ms) noexcept;
    // Issue #2132: hard ceiling (default 0 → 4× soft threshold). Always
    // throttles, including critical regions.
    void set_hard_deopt_storm_threshold(std::uint64_t deopts_per_window) noexcept;
    [[nodiscard]] std::uint64_t hard_deopt_storm_threshold() const noexcept;
    // Issue #2132: Agent-tunable critical region / priority bit mask.
    void set_critical_region_mask(std::uint64_t mask) noexcept;
    [[nodiscard]] std::uint64_t critical_region_mask() const noexcept;
    [[nodiscard]] std::uint64_t deopt_storm_threshold() const noexcept;
    [[nodiscard]] std::uint64_t deopt_storm_window_ms() const noexcept;
    // Issue #2127: current sliding-window deopt count (adaptive thr signal).
    [[nodiscard]] std::uint64_t deopt_window_count() const noexcept {
        return deopt_window_count_.load(std::memory_order_relaxed);
    }
    // Issue #2132 / #2035: last cascade-derived dirty region mask.
    [[nodiscard]] std::uint64_t last_region_mask_from_dirty() const noexcept {
        return last_region_mask_from_dirty_.load(std::memory_order_relaxed);
    }
    // Test / recovery: clear throttle + open a fresh window.
    void reset_deopt_storm_state_for_test() noexcept;

    // ── Issue #2114 / #2205: reemit ↔ MutationBoundary handshake ──
    // SoftEnter (0): test / explicit opt-in only — TLS soft boundary.
    // Defer (1, production default #2205 / #2208): outside → pending, no body.
    // RequireRealBoundary (2): outside → reject (no defer, no soft).
    enum class ReemitBoundaryPolicy : int { SoftEnter = 0, Defer = 1, RequireRealBoundary = 2 };
    void set_reemit_boundary_policy(ReemitBoundaryPolicy p) noexcept;
    [[nodiscard]] ReemitBoundaryPolicy reemit_boundary_policy() const noexcept;
    // Issue #2205 / #2208: SoftEnter allowed only when policy is SoftEnter
    // (set explicitly or via AURA_REEMIT_SOFT_ENTER under security defaults).
    [[nodiscard]] bool soft_enter_allowed() const noexcept;
    // True when real MutationBoundary depth/held or soft reemit depth > 0.
    [[nodiscard]] bool in_mutation_boundary_for_reemit() const noexcept;
    // Soft reemit boundary (TLS). RAII helpers for reemit pipeline.
    void soft_reemit_boundary_enter() noexcept;
    void soft_reemit_boundary_exit() noexcept;
    [[nodiscard]] int soft_reemit_boundary_depth() const noexcept;
    // Observability bumps (never silent outside path).
    void on_reemit_outside_boundary() noexcept;
    void on_reemit_soft_boundary_entered() noexcept;
    void on_reemit_deferred_for_boundary() noexcept;
    // Issue #2205: RequireRealBoundary outside → reject (no defer/soft).
    void on_reemit_rejected_require_real() noexcept;
    // Defer pending reemit (policy=Defer). Flushed by boundary exit.
    void defer_reemit_for_boundary(std::uint64_t defuse_version) noexcept;
    [[nodiscard]] bool has_deferred_reemit() const noexcept;
    // Issue #2273: bump steal-path counter (lazy — callers check
    // has_deferred_reemit() FIRST, single relaxed load on the common
    // path). Caller passes the migrating fiber_id so dashboards can
    // correlate "pending" with "which fiber stole it".
    void on_deferred_reemit_seen_on_steal(std::int64_t fiber_id) noexcept;
    // Returns pending version and clears deferred flag. 0 if none.
    [[nodiscard]] std::uint64_t take_deferred_reemit_version() noexcept;
    void reset_reemit_boundary_handshake_for_test() noexcept;

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
        // Issue #2132: throttle reason breakdown + critical bypass.
        std::int64_t reemit_throttle_skips_global_total = 0;
        std::int64_t reemit_throttle_skips_region_total = 0;
        std::int64_t reemit_throttle_skips_hard_total = 0;
        std::int64_t reemit_critical_bypass_total = 0;
        std::int64_t hard_storm_active = 0;
        std::int64_t hard_storm_detected_total = 0;
        std::int64_t hard_deopt_storm_threshold = 0; // 0 → auto 4× soft
        std::int64_t critical_region_mask = 0;
        std::int64_t schema_2132 = 2132;
        std::int64_t issue_2132 = 2132;
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
        // Issue #2114: reemit ↔ MutationBoundary handshake.
        std::int64_t reemit_outside_boundary_total = 0;
        std::int64_t reemit_soft_boundary_entered_total = 0;
        std::int64_t reemit_deferred_for_boundary_total = 0;
        std::int64_t reemit_boundary_policy =
            1; // 0 SoftEnter, 1 Defer (prod #2205/#2208), 2 RequireReal
        std::int64_t reemit_deferred_pending = 0;
        std::int64_t reemit_rejected_require_real_total = 0; // #2205
        std::int64_t schema_2114 = 2114;
        std::int64_t issue_2114 = 2114;
        std::int64_t schema_2205 = 2205;
        std::int64_t issue_2205 = 2205;
        std::int64_t schema_2208 = 2208; // #2208 refine Defer default (no SoftEnter prod)
        std::int64_t issue_2208 = 2208;
        // Issue #2236: StormIsolation mode + per-region trip counters.
        // storm_isolation_mode: 0=Global (default, process-wide window),
        // 1=PerRegion (per-region sliding windows with bounded 64 cap),
        // 2=PerEval (documented follow-up — eval_id threading needed).
        // deopt_storm_region_detected_total: total trip count across
        // all per-region windows (cumulative). Last region id that
        // tripped is in deopt_storm_region_last_id.
        std::int64_t storm_isolation_mode = 0;
        std::int64_t deopt_storm_region_overflow_total = 0;
        std::int64_t deopt_storm_region_detected_total = 0;
        std::int64_t deopt_storm_region_last_id = 0;
        std::int64_t schema_2236 = 2236;
        std::int64_t issue_2236 = 2236;
        // Issue #2273: steal-path observability fields.
        std::int64_t reemit_deferred_seen_on_steal_total = 0;
        std::int64_t reemit_deferred_seen_on_steal_last_fiber_id = 0;
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
    // Issue #2232: multi-round reload exhausted → fall_back_jit_only.
    //   Issue #2271: companion physical invalidate of generation-behind
    //   AOT slots happens in aura_jit_bridge.cpp BEFORE this callback so
    //   the registry only sees post-clear state (cleaner Agent diffs).
    std::atomic<std::uint64_t> force_jit_for_reason_total_{0};
    std::atomic<std::uint8_t> last_force_jit_reason_{0};
    // Issue #2367: epoch_notify_ counter snapshot at last force-JIT
    // (agents correlate recovery reason with epoch fan-out progress).
    std::atomic<std::uint64_t> last_force_jit_at_epoch_notify_{0};

    // Issue #2302: unified ReloadRecovery state machine atomics.
    //   attempts_left_: retry budget remaining for the current
    //     reload attempt (0 when not in-flight). Wired from
    //     aura_jit_bridge.cpp reload path via
    //     on_recovery_set_attempts_left() — set to
    //     policy.max_reemit at start of aura_reload_aot_module_for_eval,
    //     cleared to 0 on success or exhausted.
    //   force_jit_regions_mask_: bitmask of regions currently
    //     in force-JIT mode (bit N = reason N in the AotReloadFail
    //     enum). Set via fetch_or in on_force_jit_for_reason,
    //     cleared wholesale (store 0) in on_reload_success.
    //   pending_dirty_count_: count of pending dirty defines in
    //     HotUpdateRegistry that haven't been applied yet.
    //     Externally managed via on_recovery_pending_dirty_inc/dec()
    //     (Agent-facing API for Agents that maintain their own
    //     dirty-set overlay).
    //   deferred_reemit_pending_v2_: flag exposed via the
    //     unified recovery state — set in on_deferred_reemit_seen_on_steal,
    //     cleared in take_deferred_reemit_version and on_reload_success.
    //   All relaxed atomic (single-writer from the eval thread
    //   + reader from query primitive, mirrors the
    //   aot_reload_fail_* pattern).
    std::atomic<std::uint32_t> attempts_left_{0};
    std::atomic<std::uint64_t> force_jit_regions_mask_{0};
    std::atomic<std::uint64_t> pending_dirty_count_{0};
    std::atomic<std::uint8_t> deferred_reemit_pending_v2_{0};

    // Issue #2502: force-JIT re-promote after stable recovery window.
    //   force_jit_repromote_window_: N consecutive clean successes
    //     required (default 3). 0 disables re-promote.
    //   force_jit_stable_successes_: current streak (reset on storm,
    //     new force-JIT, rollback, or failed reemit).
    //   force_jit_repromote_require_pending_idle_: when 1 (default),
    //     pending_dirty_count must be 0 to advance the window.
    //   force_jit_repromote_total_ / last_* : observability.
    std::atomic<std::uint32_t> force_jit_repromote_window_{3};
    std::atomic<std::uint32_t> force_jit_stable_successes_{0};
    std::atomic<std::uint8_t> force_jit_repromote_require_pending_idle_{1};
    std::atomic<std::uint64_t> force_jit_repromote_total_{0};
    std::atomic<std::uint8_t> last_force_jit_repromote_reason_{0};
    std::atomic<std::uint64_t> last_force_jit_repromote_at_epoch_notify_{0};

    // Issue #2014: sliding window deopt rate.
    std::atomic<std::uint64_t> deopt_window_start_ms_{0};
    std::atomic<std::uint64_t> deopt_window_count_{0};
    std::atomic<std::uint64_t> deopt_observed_total_{0};
    std::atomic<std::uint64_t> deopt_storm_detected_{0};
    std::atomic<std::uint64_t> deopt_storm_threshold_{1000};
    std::atomic<std::uint64_t> deopt_storm_window_ms_{100};
    std::atomic<bool> reemit_throttled_{false};
    std::atomic<std::uint64_t> reemit_throttle_skips_{0};
    // Issue #2132: region/priority-aware throttle + hard ceiling.
    std::atomic<bool> hard_storm_active_{false};
    std::atomic<std::uint64_t> hard_storm_detected_{0};
    std::atomic<std::uint64_t> hard_deopt_storm_threshold_{0}; // 0 → 4× soft
    std::atomic<std::uint64_t> critical_region_mask_{0};
    std::atomic<std::uint64_t> reemit_throttle_skips_global_{0};
    std::atomic<std::uint64_t> reemit_throttle_skips_region_{0};
    std::atomic<std::uint64_t> reemit_throttle_skips_hard_{0};
    std::atomic<std::uint64_t> reemit_critical_bypass_{0};
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
    // Issue #2114 / #2205: reemit ↔ MutationBoundary handshake.
    // Default Defer (1) — production fail-closed under multi-fiber (#2205/#2208).
    std::atomic<int> reemit_boundary_policy_{1}; // ReemitBoundaryPolicy::Defer
    std::atomic<std::uint64_t> reemit_outside_boundary_{0};
    std::atomic<std::uint64_t> reemit_soft_boundary_entered_{0};
    std::atomic<std::uint64_t> reemit_deferred_for_boundary_{0};
    // Issue #2273: steal-path observability — bumped by
    // on_deferred_reemit_seen_on_steal when refresh_after_fiber_migration
    // (or steal-complete) observes a pending deferred reemit. Lets
    // Agents correlate "pending" with "stuck on a stolen fiber".
    std::atomic<std::uint64_t> reemit_deferred_seen_on_steal_total_{0};
    // Issue #2273: last fiber_id that observed deferred pending on
    // steal. 0 = never seen (or pre-#2273). Process-global atomic so
    // cross-worker steals are visible without per-worker aggregation.
    std::atomic<std::int64_t> reemit_deferred_seen_on_steal_last_fiber_id_{0};
    std::atomic<std::uint64_t> reemit_rejected_require_real_{0}; // #2205
    std::atomic<bool> reemit_deferred_pending_{false};
    std::atomic<std::uint64_t> reemit_deferred_version_{0};
    // Issue #2236: StormIsolation mode + per-region sliding-window state.
    // The mode atomic is file-scope-singleton level (1 instance of the
    // registry); the region_windows_ map is bounded to 64 entries
    // (kStormIsolationRegionCap) — overflow falls back to the global
    // window per the issue AC2 note. The mutex protects map resizes +
    // counter reads; per-window atomics are lock-free on the hot feed
    // / throttle paths.
    // Per-region sliding window (Issue #2236) — must be declared before map.
    struct RegionWindow {
        std::atomic<std::uint64_t> window_start_ms_{0};
        std::atomic<std::uint64_t> window_count_{0};
        std::atomic<bool> soft_throttled_{false};
        std::atomic<bool> hard_throttled_{false};
    };
    std::atomic<std::uint8_t> storm_isolation_mode_{0}; // StormIsolation enum
    // Issue #2274: cap overflow counter — bumped when region_windows_.size()

    // >= kStormIsolationRegionCap on insert. Lets Agents distinguish "many

    // regions observed" from "cap exceeded — fell back to global".

    std::atomic<std::uint64_t> deopt_storm_region_overflow_total_{0};
    mutable std::mutex region_windows_mtx_;
    // unique_ptr: RegionWindow holds atomics (non-copyable/movable).
    std::unordered_map<std::uint64_t, std::unique_ptr<RegionWindow>> region_windows_;
    std::atomic<std::uint64_t> deopt_storm_region_detected_total_{0};
    std::atomic<std::uint64_t> deopt_storm_region_last_id_{0};

    // Helper: feed `n` deopts into region_windows_[region], with the
    // same threshold-check + trip semantics as on_stale_deopt. Bumps
    // deopt_observed_total_ by `n` (not per-region) for parity with
    // the global counter. Returns true if the region window tripped.
    bool feed_region_deopt_locked(RegionWindow& w, std::uint64_t n, std::uint64_t threshold,
                                  std::uint64_t window_ms, std::uint64_t hard_thr,
                                  std::uint64_t region) noexcept;

    // Issue #2302: unified ReloadRecovery state accessor. Returns a
    // 5-field snapshot combining retry budget + force-JIT region mask +
    // last fail reason + pending dirty count + deferred-reemit flag.
    // Reads relaxed atomics — safe under concurrent writers on the eval
    // thread (single-writer for attempts_left_ / force_jit_regions_mask_
    // / deferred_reemit_pending_v2_; multi-writer for pending_dirty_count_
    // via Agent-facing inc/dec API). Schema additive — does NOT modify
    // the existing Snapshot struct (AC4 compatibility).
    struct ReloadRecoveryState {
        std::uint32_t attempts_left = 0;
        std::uint64_t force_jit_regions_mask = 0;
        std::uint8_t last_reason = 0; // mirrors last_aot_reload_fail_reason_
        std::uint64_t pending_dirty_count = 0;
        std::uint8_t deferred_reemit_pending = 0;
    };

public:
    [[nodiscard]] ReloadRecoveryState reload_recovery_state() const noexcept;
    // Issue #2367: force-JIT observability (paired with recovery query).
    [[nodiscard]] std::uint64_t force_jit_for_reason_total() const noexcept {
        return force_jit_for_reason_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint8_t last_force_jit_reason() const noexcept {
        return last_force_jit_reason_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t last_force_jit_at_epoch_notify() const noexcept {
        return last_force_jit_at_epoch_notify_.load(std::memory_order_relaxed);
    }
    // Agent-facing API: increment / decrement pending dirty count.
    // Used by Agents that maintain their own dirty-set overlay and
    // want to publish the size via the unified recovery state.
    void on_recovery_pending_dirty_inc() noexcept {
        pending_dirty_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void on_recovery_pending_dirty_dec() noexcept {
        if (pending_dirty_count_.load(std::memory_order_relaxed) > 0)
            pending_dirty_count_.fetch_sub(1, std::memory_order_relaxed);
    }
    // Wire attempts_left_ from aura_jit_bridge.cpp reload path.
    void on_recovery_set_attempts_left(std::uint32_t n) noexcept {
        attempts_left_.store(n, std::memory_order_relaxed);
    }

public:
    // Public accessor for pending_dirty_count_ (used by the C-linkage
    // reader in hot_update_registry.cpp — namespace-scope extern "C"
    // functions can't access private members directly).
    [[nodiscard]] std::uint64_t pending_dirty_count() const noexcept {
        return pending_dirty_count_.load(std::memory_order_relaxed);
    }
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
    // Issue #2132
    std::int64_t reemit_throttle_skips_global_total;
    std::int64_t reemit_throttle_skips_region_total;
    std::int64_t reemit_throttle_skips_hard_total;
    std::int64_t reemit_critical_bypass_total;
    std::int64_t hard_storm_active;
    std::int64_t hard_storm_detected_total;
    std::int64_t hard_deopt_storm_threshold;
    std::int64_t critical_region_mask;
    std::int64_t schema_2132;
    std::int64_t issue_2132;
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
    // Issue #2114 / #2205
    std::int64_t reemit_outside_boundary_total;
    std::int64_t reemit_soft_boundary_entered_total;
    std::int64_t reemit_deferred_for_boundary_total;
    std::int64_t reemit_boundary_policy;
    std::int64_t reemit_deferred_pending;
    // Issue #2273: steal-path observability fields.
    std::int64_t reemit_deferred_seen_on_steal_total;
    std::int64_t reemit_deferred_seen_on_steal_last_fiber_id;
    std::int64_t reemit_rejected_require_real_total; // #2205
    std::int64_t schema_2114;
    std::int64_t issue_2114;
    std::int64_t schema_2205; // #2205
    std::int64_t issue_2205;  // #2205
    std::int64_t schema_2208; // #2208 refine Defer default
    std::int64_t issue_2208;  // #2208
    // Issue #2236: StormIsolation mode + per-region storm counters.
    // MUST stay in lockstep with hot_update_registry.hh — the production
    // aura_hot_update_registry_get_snapshot() writes these fields; if
    // this shadow struct is missing any of them, the writes overflow
    // and corrupt adjacent stack/heap (stack canary smashes).
    std::int64_t storm_isolation_mode;
    // Issue #2274: cap overflow counter (production overflow bumps).
    std::int64_t deopt_storm_region_overflow_total;
    std::int64_t deopt_storm_region_detected_total;
    std::int64_t deopt_storm_region_last_id;
    std::int64_t schema_2236;
    std::int64_t issue_2236;
};
void aura_hot_update_registry_get_snapshot(aura_hot_update_registry_snapshot* out);
// Issue #2014: C entry points for deopt feed / throttle / config.
void aura_hot_update_note_deopt(void);
int aura_hot_update_should_throttle_reemit(void);
// Issue #2273: steal-path observability C entry point.
void aura_hot_update_on_deferred_reemit_seen_on_steal(std::int64_t fiber_id);
// Issue #2132: region/priority-aware throttle (1 = skip reemit).
int aura_hot_update_should_throttle_reemit_for_region(std::uint64_t region_or_priority);
void aura_hot_update_set_critical_region_mask(std::uint64_t mask);
std::uint64_t aura_hot_update_critical_region_mask(void);
void aura_hot_update_set_hard_deopt_storm_threshold(std::uint64_t deopts_per_window);
std::uint64_t aura_hot_update_hard_deopt_storm_threshold(void);
void aura_hot_update_on_reemit_throttled(void);

// Issue #2094: StormLevel facade accessor (C ABI). Returns the
// combined bitmask of shape-storm + global-deopt-storm detectors
// so external callers can branch on a single recovery-policy value.
// Result mapping (uint8_t): 0=None, 1=Shape, 2=Global, 3=Both.
extern "C" std::uint8_t aura_hot_update_current_storm_level(void);

// Issue #2367: agent-facing ReloadRecovery snapshot (C ABI).
// Module partitions cannot attach HotUpdateRegistry — use this
// instead of calling reload_recovery_state() from evaluator TUs.
// Zero-cost when idle: pure relaxed atomic loads, no allocation.
struct aura_reload_recovery_snapshot {
    std::int64_t schema; // 2367
    std::int64_t issue;  // 2367
    // ReloadRecoveryState (Issue #2302)
    std::int64_t attempts_left;
    std::int64_t force_jit_regions_mask;
    std::int64_t last_reason; // AotReloadFail enum
    std::int64_t pending_dirty_count;
    std::int64_t deferred_reemit_pending; // recovery v2 flag
    // Storm / policy / region context
    std::int64_t storm_level;            // StormLevel bitmask
    std::int64_t reemit_boundary_policy; // ReemitBoundaryPolicy
    std::int64_t emit_region_mask;
    std::int64_t critical_region_mask;
    std::int64_t storm_isolation_mode;
    std::int64_t deopt_storm_region_last_id;
    std::int64_t deopt_storm_region_detected_total;
    std::int64_t hard_storm_active;
    std::int64_t reemit_deferred_pending_boundary; // #2114 handshake flag
    // Force-JIT / fall-back reason + epoch correlation
    std::int64_t last_force_jit_reason;
    std::int64_t force_jit_for_reason_total;
    std::int64_t last_force_jit_at_epoch_notify;
    std::int64_t epoch_notify_total;
    // Issue #2502: auto re-promote after stable recovery window
    std::int64_t force_jit_repromote_total;
    std::int64_t last_force_jit_repromote_reason;
    std::int64_t last_force_jit_repromote_at_epoch_notify;
    std::int64_t force_jit_stable_successes;
    std::int64_t force_jit_repromote_window;
    std::int64_t force_jit_repromote_require_pending_idle;
    std::int64_t schema_2502; // 2502 when wired
    // recovery-active: 1 when any non-idle recovery signal is set
    // (force-jit mask, attempts_left, pending dirty, deferred reemit,
    // storm_level != None). Soft empty path → 0.
    std::int64_t recovery_active;
    std::int64_t reload_recovery_wired; // always 1 when linked
};
void aura_hot_update_reload_recovery_get_snapshot(aura_reload_recovery_snapshot* out);

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

// Issue #2114 / #2205: reemit ↔ MutationBoundary handshake C ABI.
// Returns 1 when depth>0, MutationBoundary held, or soft reemit depth>0.
int aura_hot_update_in_mutation_boundary_for_reemit(void);
// Policy: 0=SoftEnter (opt-in), 1=Defer (production default #2205/#2208),
// 2=RequireRealBoundary (reject without defer).
void aura_hot_update_set_reemit_boundary_policy(int policy);
int aura_hot_update_get_reemit_boundary_policy(void);
void aura_hot_update_reset_reemit_boundary_handshake_for_test(void);
// Soft boundary depth (TLS); 1 when active.
int aura_hot_update_soft_reemit_boundary_active(void);
// 1 if a deferred reemit is pending.
int aura_hot_update_has_deferred_reemit(void);

// Issue #2370: TLS storm eval context for PerEval isolation.
// CompilerService / tests publish the current eval owner so PerEval
// storm windows + SpecJIT clear only apply to the matching eval.
void aura_set_storm_eval_context(void* eval_ptr) noexcept;
void* aura_get_storm_eval_context(void) noexcept;
// SpecJIT PerEval counters (defined in spec_jit_controller.cpp).
std::uint64_t aura_specjit_per_eval_storm_clear_total_v_read(void);
std::uint64_t aura_specjit_per_eval_storm_skip_foreign_total_v_read(void);
std::uint64_t aura_specjit_storm_clear_total_v_read(void);
}

#endif // AURA_COMPILER_HOT_UPDATE_REGISTRY_HH
