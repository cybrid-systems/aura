// hot_update_registry.hh — Issue #1956 / #2014 / #2035 / #2046 / #2114 / #2132
// Issue #2692 / #2853 / #2854 / #2855: hot-update registry also owns the
// cross-eval sid ↔ AOT slot owner mismatch counter (#2692), the production
// residual-policy lock (#2853), and the same-transaction stamp (#2854) that
// atomic eval cleanup preserves (see aura_jit_bridge.cpp).
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

extern "C" int aura_production_defaults_active_probe() noexcept;

namespace aura::compiler {

// Sentinel epoch passed to epoch listeners when a deopt storm trips (#2014).
inline constexpr std::uint64_t kHotUpdateDeoptStormEpoch = ~std::uint64_t{0};
// Issue #3059: single production reemit facade (cascade / BoundaryExit /
// reload / exhausted min-dirty / residual pipeline).
inline constexpr int kHotUpdateDecideAndReemitIssue = 3059;
// Issue #3221: production mark_define_dirty / invalidate_function pass
// Cascade (not ResidualForceHeal) into the facade.
inline constexpr int kHotUpdateCascadeReasonIssue = 3221;
// Issue #3229: hashed-name success coverage is 6-bit (fnv1a & 63). A
// bounded define-id side set makes residual / remount / re-promote
// define-correct under collisions. Soft never writes it.
inline constexpr int kRelowerSuccessDefineCollisionIssue = 3229;
inline constexpr std::size_t kRelowerSuccessDefineCap = 64;
// Issue #3351: owner-scoped peer IR-cache must not clean-hit after
// residual_force / soft-stale. Name-level gen + per-entry ack.
inline constexpr int kPeerIrNameSoftStaleIssue = 3351;

// Define-id for the #3229 side set: prefer stable_func_id, else 32-bit
// FNV of the name (never 0). Separates defines that collide on 6 bits.
[[nodiscard]] inline std::uint32_t relower_success_define_id(const std::string& name) noexcept {
    if (name.empty())
        return 0;
    const auto sid = aura_lookup_stable_func_id(name.c_str());
    if (sid != 0)
        return sid;
    std::uint32_t h = 2166136261u;
    for (unsigned char c : name) {
        h ^= c;
        h *= 16777619u;
    }
    return h == 0 ? 1u : h;
}

// Issue #3383: shared region-bit helper. Must use the SAME fnv1a_64 hash as
// CompilerService::fnv1a_64 (service.ixx) — both call sites (store_define_v2
// + cascade restamp in service_dirty.cpp::notify_hot_update_after_cascade_)
// stamp the same bit for the same define, so residual_force_mask =
// force_mask & ~last_success clears correctly across a store + cascade-
// restamp pair (no half-cover / sticky force-JIT / missed re-promote).
//
// Inlines fnv1a_64 here rather than threading CompilerService::* into this
// header (service.ixx is heavy; hot_update_registry.hh is a leaf header).
// Algorithm is the standard FNV-1a 64-bit (offset basis 0xcbf29ce484222325,
// prime 0x100000001b3) — must match CompilerService::fnv1a_64 exactly. A
// unit test (test_hot_update_relower_success_coverage) asserts identity
// on a fixed name corpus.
[[nodiscard]] inline std::uint64_t relower_success_region_bit(std::string_view name) noexcept {
    constexpr std::uint64_t kFnv1aOffset = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t kFnv1aPrime = 0x100000001b3ULL;
    std::uint64_t h = kFnv1aOffset;
    for (char c : name) {
        h ^= static_cast<std::uint8_t>(c);
        h *= kFnv1aPrime;
    }
    return 1ULL << (h & 63);
}

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
    // Issue #3059: production reemit entry. Storm / boundary / owner /
    // region / provider-not-wired gates stay inside aura_reemit_aot_for_dirty
    // (low-level C ABI used only from this facade + explicit raw tests).
    // On n>0, last_reemit_success coverage matches the Registry pipeline
    // (override, else last_force_jit_reason group ∩ force — #3466).
    // n==0: no extra stamp.
    enum class ReemitReason : std::uint8_t {
        Cascade = 0,
        BoundaryExit = 1,
        ReloadRecovery = 2,
        ExhaustedMinDirty = 3,
        StormClear = 4,
        ResidualPipeline = 5,
        CoverageVerify = 6,
        // Issue #3096: production-only bounded auto-heal when residual
        // force bits age past threshold with exhausted retry budget.
        // Drives maybe_coverage_verify_min_dirty (single seed + decide
        // gate); respects resolve_force_jit_repromote_only_covered.
        ResidualForceHeal = 7,
    };
    [[nodiscard]] std::uint64_t decide_and_reemit(std::uint64_t defuse_version,
                                                  ReemitReason reason) noexcept;

    // Issue #3112: production-only facade for hard invalidate. Routes through
    // decide_and_reemit so the centralized AotReloadConsistencyProof fail-stamp
    // + owner-scope filtering + region mask are always taken under production.
    // Soft / Off: caller should use the lightweight direct path (zero cost).
    // Returns true if the facade was taken (production + reemit triggered).
    [[nodiscard]] bool hard_invalidate_via_facade(const char* name, ReemitReason reason) noexcept;

    // Issue #3513: facade `decide_and_reemit` may run against pre-store
    // `irs`. IR lookup is already latched (`content_stored_this_epoch`,
    // #3481). Native promotion (re-promote / remount / would_allow_native
    // / peer-stale clear) must wait until `store_define_v2`. Soft/Off
    // never takes the facade (zero extra). Not a second stamp API —
    // one latch the existing IR content flag already describes.
    static constexpr int kIrContentUntrustedNativeIssue = 3513;
    static constexpr int kPeerCallerConeStaleIssue = 3514;
    void note_ir_content_untrusted_for_native() noexcept {
        ir_content_untrusted_for_native_.store(1, std::memory_order_release);
    }
    void note_ir_content_stored_for_native() noexcept {
        ir_content_untrusted_for_native_.store(0, std::memory_order_release);
    }
    [[nodiscard]] bool ir_content_untrusted_for_native() const noexcept {
        return ir_content_untrusted_for_native_.load(std::memory_order_acquire) != 0;
    }

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
    // Issue #2895: last clean-reemit coverage (force-JIT reason bits that
    // a successful reemit "covered"). Soft zero-cost when mask idle.
    // Agents / bridge may publish a narrower coverage than the full
    // demoted mask via note_reemit_success_coverage.
    [[nodiscard]] std::uint64_t last_reemit_success_region_mask() const noexcept;
    // Issue #2977: residual remount prefer reads this mask (OR force_jit).
    [[nodiscard]] std::uint64_t force_jit_regions_mask() const noexcept;
    // Issue #3026: residual = force & ~last_success (agent-actionable bits).
    [[nodiscard]] std::uint64_t residual_force_mask() const noexcept;

    // Issue #3136 / #3383: relower-success hook. Call sites still pass the
    // hashed-name region bit (fnv1a_64 identity across store + cascade).
    // Issue #3505: do **not** OR that bit into `last_reemit_success_region_mask_`
    // — that word is the #3445/#3466 **reason-group** coverage (bits 0–4).
    // Define coverage is the #3229 side set (`note_relower_success_define`).
    // Soft / Off: callers already skip; this body is a no-op either way.
    void note_relower_success_coverage(std::uint64_t region_bit) noexcept { (void)region_bit; }
    // Issue #3229: record the restamped define so a peer that collides
    // on fnv1a&63 is not treated as covered. id==0 / Soft skip. Cap
    // overflow is fail-closed (unrecorded id stays residual).
    void note_relower_success_define(std::uint32_t id) noexcept {
        if (id == 0)
            return;
        if (aura_production_defaults_active_probe() == 0)
            return; // Soft / Off: zero extra
        const auto n = relower_success_define_count_.load(std::memory_order_relaxed);
        const auto lim =
            n < kRelowerSuccessDefineCap ? n : static_cast<std::uint32_t>(kRelowerSuccessDefineCap);
        for (std::uint32_t i = 0; i < lim; ++i) {
            if (relower_success_define_ids_[i].load(std::memory_order_relaxed) == id)
                return;
        }
        const auto slot = relower_success_define_count_.fetch_add(1, std::memory_order_relaxed);
        if (slot >= kRelowerSuccessDefineCap)
            return;
        relower_success_define_ids_[slot].store(id, std::memory_order_relaxed);
        relower_success_define_active_.store(1, std::memory_order_relaxed);
    }
    [[nodiscard]] bool relower_success_covers_define(std::uint32_t id) const noexcept {
        if (id == 0)
            return false;
        const auto n = relower_success_define_count_.load(std::memory_order_relaxed);
        const auto lim =
            n < kRelowerSuccessDefineCap ? n : static_cast<std::uint32_t>(kRelowerSuccessDefineCap);
        for (std::uint32_t i = 0; i < lim; ++i) {
            if (relower_success_define_ids_[i].load(std::memory_order_relaxed) == id)
                return true;
        }
        return false;
    }
    [[nodiscard]] bool relower_success_define_active() const noexcept {
        return relower_success_define_active_.load(std::memory_order_relaxed) != 0;
    }
    // Per-define residual: force_region && !this define's success.
    // When the side set is idle (Soft / pipeline coverage), fall back
    // to residual_force_mask() & region_bit (#3136).
    [[nodiscard]] bool residual_force_for_define(std::uint32_t id,
                                                 std::uint64_t region_bit) const noexcept {
        if (region_bit == 0)
            return false;
        const auto force = force_jit_regions_mask_.load(std::memory_order_relaxed);
        if ((force & region_bit) == 0)
            return false;
        if (relower_success_define_active_.load(std::memory_order_relaxed) != 0)
            return !relower_success_covers_define(id);
        return (residual_force_mask() & region_bit) != 0;
    }
    void clear_relower_success_defines() noexcept {
        relower_success_define_count_.store(0, std::memory_order_relaxed);
        relower_success_define_active_.store(0, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t residual_force_stale_observe_total() const noexcept;
    // Issue #3096: production-only bounded auto-heal when residual force
    // bits age past threshold with exhausted retry budget. Lifetime
    // counter (auto-heal fired) + per-mask-generation cap flag (set to
    // residual mask at fire time; reset on mask change in
    // observe_residual_force_stale). Soft / Off is early-returned in
    // observe_residual_force_stale before the auto-heal check, so these
    // counters stay at 0 under Soft / Off (zero-cost contract).
    [[nodiscard]] std::uint64_t residual_force_auto_heal_total() const noexcept;
    [[nodiscard]] std::uint64_t residual_force_auto_heal_last_mask() const noexcept;
    // Production-only observe: age residual bits across outermost
    // BoundaryExits (success or fail — Issue #3248). Soft / idle
    // residual==0 → one/two loads, no counter. Never reemits from
    // the observe itself; #3096 auto-heal is a separate gated pass.
    void observe_residual_force_stale() noexcept;
    void reset_residual_force_observe_for_test() noexcept;
    // Issue #3096 test isolation: stamp force-JIT mask / exhaust retry
    // without on_force_jit_for_reason side effects.
    void force_jit_stamp_for_test(std::uint64_t mask) noexcept {
        force_jit_regions_mask_.store(mask, std::memory_order_relaxed);
    }
    void exhaust_retry_for_test() noexcept {
        exhausted_min_dirty_retry_attempts_left_.store(0, std::memory_order_relaxed);
    }
    void note_reemit_success_coverage(std::uint64_t covered_force_jit_bits) noexcept;
    // Issue #2895 / #2949: partial re-promote (clear only force_mask ∩
    // last_success). Effective policy via resolve (env + sticky +
    // production default). set_* sticks an explicit value that wins over
    // production auto (except env=0/1). Default after reset = auto:
    // Soft/sandbox=off → wholesale (#2502); production → only_covered.
    void set_force_jit_repromote_only_covered_bits(bool only_covered) noexcept;
    // Effective policy (resolved). For Agents / tests / snapshot.
    [[nodiscard]] bool force_jit_repromote_only_covered_bits() const noexcept;
    // Pure resolve of only_covered policy (no atomics beyond sticky loads).
    // Priority: env AURA_FORCE_JIT_REPROMOTE_ONLY_COVERED=0/1 → sticky set
    // → Soft/sandbox=off → production_defaults → false.
    [[nodiscard]] bool resolve_force_jit_repromote_only_covered() const noexcept;
    [[nodiscard]] std::uint64_t force_jit_repromote_partial_total() const noexcept;
    // Test isolation: reset streak / totals / window defaults without
    // touching force_jit_regions_mask_ (use on_reload_success for that).
    void reset_force_jit_repromote_for_test() noexcept;
    // Issue #2855: env-cached force-drain deadline (distinct semantics from
    // #2748 AURA_DEFERRED_REEMIT_DEADLINE_MS which only counts deadline_hit;
    // this gates the actual force-drain body). Default 0 = disabled
    // (observe-only — no force drain). When > 0: under production lock,
    // age >= this triggers force_drain_deferred_reemit() at the next
    // safe thread context (on_reemit_pipeline_call amortized site, never
    // steal path).
    // #2853 / #2854: production residual-policy lock + same-transaction
    // stamp preserved by atomic eval cleanup (#2857) — additive to this
    // #2855 force-drain surface.
    [[nodiscard]] static std::uint64_t force_drain_deadline_ms() noexcept;
    [[nodiscard]] static std::uint64_t reemit_deferred_force_drain_deadline_hit_env_read() noexcept;
    // Issue #3112: dual-track bypass prevention counters. Bumped when a
    // production path would have invoked mark_define_dirty / invalidate_function
    // directly without going through hard_invalidate_via_facade. Soft stays
    // silent (no facade forced). additive-only, no new query key.
    inline static std::atomic<std::uint64_t> g_dual_track_bypass_prevented_total{0};
    inline static std::atomic<std::uint64_t> g_dual_track_bypass_total{0};
    inline static std::atomic<std::uint64_t> g_force_drain_deadline_hit_total_{
        0}; // mirrors deadline_hit_total_; force gate
    // Env cache for force_drain_deadline_ms(); reset_reemit_force_drain_for_test
    // clears loaded so back-to-back ACs can change AURA_REEMIT_FORCE_DRAIN_DEADLINE_MS.
    inline static std::atomic<std::uint64_t> g_force_drain_deadline_cached_{0};
    inline static std::atomic<bool> g_force_drain_deadline_loaded_{false};
    // Issue #2855 process-wide atomics (static members; mirror deadline_hit).
    inline static std::atomic<std::uint64_t> g_reemit_deferred_force_drain_total_{0};
    inline static std::atomic<std::uint64_t> g_reemit_deferred_force_drain_skipped_reentered_total_{
        0};
    inline static std::atomic<std::uint64_t> g_reemit_deferred_force_drain_double_prevented_total_{
        0};
    // CAS re-entry guard — single force-drain body in flight at a time
    // across the whole process (thread-safe via atomic_bool CAS). Reset to
    // false after the body returns (RAII-like via scoped flag holder).
    inline static std::atomic<bool> g_reemit_force_drain_in_flight_{false};
    static constexpr int kReemitForceDrainIssue = 2855;
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
    // Issue #3070: sample storm level; on Both/Global→None or Shape→None
    // with residual deopt window, arm a force-full cooldown. Returns 1
    // and consumes one consult while the cooldown is live.
    [[nodiscard]] bool storm_exit_force_full_active() noexcept;
    // Issue #2093: reason-aware rollback hook. The per-reason atomic
    // counter + last-reason file-scope atomic are bumped here so the
    // Agent snapshot (taken via get_snapshot / get_stats_snapshot) can
    // distinguish Version vs Region vs Env failures for recovery
    // policy. on_reload_rollback() (no-arg) is kept as a thin wrapper
    // for callers that don't have a reason.
    void on_reload_rollback(AotReloadFail reason) noexcept;
    void on_reload_rollback() noexcept;
    // Issue #2982: Staging/Dlopen ops recovery surface. Production only
    // extra stores (Soft keeps last_reason only). detail_hash / path_hash
    // are FNV-1a stable ids (no path string in the snapshot).
    void note_ops_fail_dlopen(std::uint64_t path_hash, std::int32_t errno_class) noexcept;
    void note_ops_fail_staging(std::uint64_t detail_hash) noexcept;
    [[nodiscard]] std::uint8_t last_ops_fail_kind() const noexcept;
    [[nodiscard]] std::uint64_t last_dlopen_path_hash() const noexcept;
    [[nodiscard]] std::int32_t last_dlopen_errno_class() const noexcept;
    [[nodiscard]] std::uint64_t last_staging_detail() const noexcept;
    [[nodiscard]] std::uint8_t staging_retry_eligible() const noexcept;
    [[nodiscard]] std::uint64_t staging_retry_scheduled_total() const noexcept;
    void reset_ops_fail_surface_for_test() noexcept;
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
    // Issue #2232 / #2845: fall-back to JIT-only + re-stamp
    // AotReloadConsistencyProof (would_allow_native=false, force mask).
    void on_force_jit_for_reason(AotReloadFail reason) noexcept;
    // Issue #2544: seed a minimal dirty recovery signal after force-JIT
    // exhaust (region-mask bit for the fail reason + cascade reemit
    // trigger). Does not drive the reemit body — aura_jit_bridge owns
    // the single aura_reemit_aot_for_dirty pass + metrics. Soft zero-
    // cost for agents that only observe last_region_mask_from_dirty.
    void on_exhausted_min_dirty_queue(AotReloadFail reason) noexcept;
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
    // Issue #2604: outermost MutationBoundary exit auto-drain
    // deferred reemit + one region-filtered pass. Bumped from
    // evaluator_mutation_boundary.cpp exit_mutation_boundary success path.
    void bump_reemit_auto_drain_on_boundary_exit_total() noexcept;
    void bump_reemit_auto_drain_success_total() noexcept;
    void bump_reemit_auto_drain_throttled_total() noexcept;
    // Returns pending version and clears deferred flag. 0 if none.
    [[nodiscard]] std::uint64_t take_deferred_reemit_version() noexcept;
    // Issue #2748: deferred reemit age observability (steady-clock ms).
    // 0 when not pending. max_observed retains lifetime peak across drains.
    [[nodiscard]] std::uint64_t deferred_reemit_age_ms() const noexcept;
    [[nodiscard]] std::uint64_t deferred_reemit_age_max_observed_ms() const noexcept;
    [[nodiscard]] std::uint64_t deferred_reemit_deadline_hit_total() const noexcept;
    void reset_reemit_boundary_handshake_for_test() noexcept;

    // Issue #2855: production deferred-reemit deadline force-drain.
    // The age-observability only-counted the #2748 path; this surfaces a
    // bounded recovery window so BoundaryExit delays (long fiber work,
    // cancelled paths, pathological interleavings) cannot keep AOT/JIT
    // generation-behind for unbounded wall time under production Defer.
    // The force-drain hook drives drain_pending_recovery(DrainReason::Explicit)
    // when production_defaults_active + sandbox != off + has_deferred_reemit()
    // + age >= force_deadline_ms + soft_reemit_boundary_depth() == 0.
    // CAS re-entry guard (AC4 storm re-entry) — only one body in flight;
    // concurrent BoundaryExit + force-drain → at most one body, double-drain
    // prevented counter may rise. Soft / force_deadline=0 → observe-only
    // (#2748 behavior preserved). NOT invoked from steal-complete (Issue
    // #2715 regression guard — production steal path must not foreign-drain).
    // Issue #2715: steal path must not foreign-drain (chaos #2902 gate cites).
    [[nodiscard]] bool should_force_drain_deferred_reemit() const noexcept;
    // Returns true if a force-drain body actually ran (caller can use this
    // to skip subsequent drain_pending_recovery(BoundaryExit) on the same
    // exit when the force-drain already cleared pending). Exchange-not-check
    // semantics — pending clear + force-drain body run are atomic via the
    // CAS re-entry guard below.
    [[nodiscard]] bool force_drain_deferred_reemit() noexcept;
    [[nodiscard]] std::uint64_t reemit_deferred_force_drain_total() const noexcept;
    [[nodiscard]] std::uint64_t
    reemit_deferred_force_drain_skipped_reentered_total() const noexcept;
    [[nodiscard]] std::uint64_t reemit_deferred_force_drain_double_prevented_total() const noexcept;
    // Test reset — clears the 3 #2855 cumulative counters + CAS re-entry
    // guard + deadline_hit_total (so back-to-back AC tests don't observe
    // stale state). Does NOT touch max_observed (lifetime peak — same
    // semantics as #2748 reset_reemit_boundary_handshake_for_test).
    void reset_reemit_force_drain_for_test() noexcept;

    // Issue #2690: unified PendingRecovery drain. Both
    // `maybe_storm_clear_health_pass` (StormClear) and outermost
    // MutationBoundary success exit (BoundaryExit) route through
    // `drain_pending_recovery(why)` to close the residual unhealed
    // window under novel interleavings. Exchange-not-check semantics.
    // Issue #2690: take reason as raw uint8_t to avoid DrainReason namespace
    // ambiguity. The nested types + method declarations are at the end of
    // the class (L846-847) where `PendingRecovery` + `DrainReason` are
    // in scope via the class's nested type definitions. AC1/AC3 driven
    // by exchange_not_check.

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
    // Issue #2576-lifecycle: CompilerService owns a storm listener that
    // captures `this`; without unregister the global storm_listeners_
    // keeps a dangling this after ~CompilerService and any later deopt
    // storm (e.g. region_priority_deopt_throttle) UAFs into the freed
    // SpecJITController. Remove by id so owners can unregister on teardown.
    void unregister_storm_listener(std::uint64_t id) noexcept;
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
        // Issue #2601: exhausted min-dirty retry closed loop
        std::int64_t aot_exhausted_min_dirty_retry_total = 0;
        std::int64_t aot_exhausted_min_dirty_retry_success_total = 0;
        std::int64_t aot_exhausted_min_dirty_retry_storm_skip_total = 0;
        std::int64_t aot_exhausted_min_dirty_retry_cap_hit_total = 0;
        std::int64_t exhausted_min_dirty_retry_attempts_left = 0;
        std::int64_t exhausted_min_dirty_retry_attempts_cap = 3;
        std::int64_t exhausted_min_dirty_retry_backoff_ms = 100;
        std::int64_t exhausted_min_dirty_retry_last_at_ms = 0;
        std::int64_t exhausted_min_dirty_retry_last_reason = 0;
        std::int64_t force_jit_repromote_allow_pending_idle_when_force_jit_covered = 0;
        std::int64_t schema_2601 = 2601;
        std::int64_t issue_2601 = 2601;
        // Issue #2639: storm-clear health pass counters (lazy hook fired on
        // non-None → None storm level transition with pending state).
        std::int64_t reemit_storm_clear_health_pass_total = 0;
        std::int64_t reemit_storm_clear_health_pass_success_total = 0;
        std::int64_t reemit_storm_clear_health_pass_skipped_reentered_storm_total = 0;
        std::int64_t schema_2639 = 2639;
        std::int64_t issue_2639 = 2639;
        // Issue #2669: storm-clear health pass drives recovery body (refine #2639
        // counter-only → drive deferred reemit / #2601 min-dirty retry / #2502
        // cascade trigger). Additive counters; #2604/#2601/#2502/#2639 unchanged.
        std::int64_t reemit_storm_clear_health_pass_reemit_driven_total = 0;
        std::int64_t schema_2669 = 2669;
        std::int64_t issue_2669 = 2669;
        // Issue #2952: coverage-verify min-dirty on storm clear / drain.
        std::int64_t coverage_verify_scheduled_total = 0;
        std::int64_t coverage_verify_success_total = 0;
        std::int64_t coverage_verify_residual_uncovered_total = 0;
        std::int64_t coverage_verify_storm_skip_total = 0;
        std::int64_t coverage_verify_min_dirty_wired = 1;
        std::int64_t schema_2952 = 2952;
        std::int64_t issue_2952 = 2952;
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
    std::vector<std::pair<std::uint64_t, StormListener>> storm_listeners_;
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
    // Issue #2982: Staging/Dlopen ops surface (production extra stores).
    // kind: 0=None 1=Staging 2=Dlopen. Soft / Ok never write these.
    std::atomic<std::uint8_t> last_ops_fail_kind_{0};
    std::atomic<std::uint64_t> last_dlopen_path_hash_{0};
    std::atomic<std::int32_t> last_dlopen_errno_class_{0};
    std::atomic<std::uint64_t> last_staging_detail_{0};
    std::atomic<std::uint8_t> staging_retry_eligible_{0};
    std::atomic<std::uint8_t> staging_retry_window_armed_{0};
    std::atomic<std::uint64_t> staging_retry_scheduled_total_{0};
    // Issue #2232: multi-round reload exhausted → fall_back_jit_only.
    //   Issue #2271: companion physical invalidate of generation-behind
    //   AOT slots happens in aura_jit_bridge.cpp BEFORE this callback so
    //   the registry only sees post-clear state (cleaner Agent diffs).
    std::atomic<std::uint64_t> force_jit_for_reason_total_{0};
    std::atomic<std::uint8_t> last_force_jit_reason_{0};
    // Issue #2927: last mapped force_jit_regions_mask bit index from
    // aot_reload_fail_to_force_jit_bit_index (0xFF = none / Ok).
    std::atomic<std::uint8_t> last_force_jit_mapped_bit_{0xFF};
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
    //   force_jit_regions_mask_: bitmask of demotion groups currently
    //     in force-JIT mode. Issue #2927 maps AotReloadFail → stable
    //     group bits via aot_reload_fail_to_force_jit_mask (Version|
    //     Defuse→0, Env→1, Linear→2, Region|Staging→3, Dlopen|Other→4).
    //     Set via fetch_or of only the mapped bit in
    //     on_force_jit_for_reason; cleared wholesale (store 0) in
    //     on_reload_success.
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
    // Issue #2895 / #2949: coverage + partial re-promote.
    //   last_reemit_success_region_mask_: force-JIT reason bits covered by
    //     the last clean reemit success (stamped when successes > 0 and
    //     demoted != 0 from override or last_force_jit_reason group —
    //     #3466; or via note_reemit_success_coverage).
    //     Issue #2977: residual remount prefer ORs this with force_jit.
    //     Issue #2978: reemit-success sync covered-named remount reads this.
    //   force_jit_repromote_only_covered_bits_: sticky value when override
    //     is set (1 = only_covered, 0 = wholesale). Ignored when
    //     only_covered_override_ == 0 (auto → production default on).
    //   force_jit_repromote_only_covered_override_: 0 = auto resolve
    //     (#2949 production default only_covered; Soft wholesale);
    //     1 = sticky bits_ wins (except env=0/1).
    //   force_jit_repromote_partial_total_: bumped when a window clear
    //     leaves a non-empty residual force mask.
    //   reemit_success_coverage_override_: when non-zero, success stamps
    //     use this mask (sticky until wholesale clear / reset). When 0,
    //     pipeline maps last_force_jit_reason ∩ live force (#3466).
    std::atomic<std::uint64_t> last_reemit_success_region_mask_{0};
    // Issue #3229: bounded define-id side set for hashed-name coverage.
    std::atomic<std::uint32_t> relower_success_define_ids_[kRelowerSuccessDefineCap]{};
    std::atomic<std::uint32_t> relower_success_define_count_{0};
    std::atomic<std::uint8_t> relower_success_define_active_{0};
    // Issue #3221: last ReemitReason passed to decide_and_reemit.
    std::atomic<std::uint8_t> last_reemit_reason_{0};
    // Issue #3026: observe-only residual-force stale watchdog (no auto-heal).
    // Soft / Off never ages. Production ages residual across BoundaryExits
    // and bumps residual_force_stale_observe_total_ every N exits (32).
    std::atomic<std::uint64_t> residual_force_stale_observe_total_{0};
    // Issue #3096: production-only bounded auto-heal state. Lifetime
    // counter (auto-heal fired) + per-mask-generation cap flag (set to
    // residual mask at fire time; reset on mask change in
    // observe_residual_force_stale). Soft / Off is early-returned in
    // observe_residual_force_stale before the auto-heal check, so both
    // stay at 0 under Soft / Off (zero-cost contract preserved).
    std::atomic<std::uint64_t> residual_force_auto_heal_total_{0};
    std::atomic<std::uint64_t> residual_force_auto_heal_last_mask_{0};
    std::atomic<std::uint64_t> residual_force_observe_age_{0};
    std::atomic<std::uint64_t> residual_force_observe_last_mask_{0};
    std::atomic<std::uint8_t> force_jit_repromote_only_covered_bits_{0};
    std::atomic<std::uint8_t> force_jit_repromote_only_covered_override_{0};
    std::atomic<std::uint64_t> force_jit_repromote_partial_total_{0};
    std::atomic<std::uint64_t> reemit_success_coverage_override_{0};
    // Zero-cost when force_jit_regions_mask_ == 0 (idle path).
    // Issue #2601: exhausted min-dirty retry closed loop state.
    //   exhausted_min_dirty_retry_attempts_left_: cap-based budget
    //     remaining. Reset to cap in on_exhausted_min_dirty_queue.
    //   exhausted_min_dirty_retry_attempts_cap_: max retries (default 3).
    //   exhausted_min_dirty_retry_backoff_ms_: min interval between
    //     retries (default 100ms). 0 = no backoff.
    //   exhausted_min_dirty_retry_last_at_ms_: steady_ms_now() of last
    //     retry attempt. 0 = never tried (initial state). Initial value
    //     in on_exhausted_min_dirty_queue is 0 so the first retry is
    //     ready immediately when storm clears.
    //   exhausted_min_dirty_retry_last_reason_: AotReloadFail enum of
    //     the last exhaust that seeded the retry series.
    //   aot_exhausted_min_dirty_retry_*_total: lifetime counters.
    //   force_jit_repromote_allow_pending_idle_when_force_jit_covered_:
    //     policy knob (default 0 = off). When set, the #2502 streak
    //     advances with pending_dirty > 0 if successes > 0 hit a
    //     reemit pipeline call while force_jit_regions_mask_ != 0.
    std::atomic<std::uint32_t> exhausted_min_dirty_retry_attempts_left_{0};
    std::atomic<std::uint32_t> exhausted_min_dirty_retry_attempts_cap_{3};
    std::atomic<std::uint64_t> exhausted_min_dirty_retry_backoff_ms_{100};
    std::atomic<std::uint64_t> exhausted_min_dirty_retry_last_at_ms_{0};
    std::atomic<std::uint8_t> exhausted_min_dirty_retry_last_reason_{0};
    std::atomic<std::uint64_t> aot_exhausted_min_dirty_retry_total_{0};
    std::atomic<std::uint64_t> aot_exhausted_min_dirty_retry_success_total_{0};
    std::atomic<std::uint64_t> aot_exhausted_min_dirty_retry_storm_skip_total_{0};
    std::atomic<std::uint64_t> aot_exhausted_min_dirty_retry_cap_hit_total_{0};
    std::atomic<std::uint8_t> force_jit_repromote_allow_pending_idle_when_force_jit_covered_{0};
    // Issue #2639: storm-clear edge detection (lazy hook).
    std::atomic<StormLevel> prev_storm_level_{StormLevel::None};
    // Issue #3070: last StormLevel sampled by the partial-relower gate
    // (distinct from prev_storm_level_ used by #2639 health pass).
    std::atomic<std::uint8_t> hysteresis_prev_storm_level_{0};
    std::atomic<std::uint32_t> storm_exit_force_full_remaining_{0};
    // Issue #3513: 1 while facade reemit may see pre-store irs. Appended
    // at the hysteresis cluster (not a query-key / metrics-middle insert).
    std::atomic<std::uint8_t> ir_content_untrusted_for_native_{0};
    std::atomic<std::uint64_t> reemit_storm_clear_health_pass_total_{0};
    std::atomic<std::uint64_t> reemit_storm_clear_health_pass_success_total_{0};
    std::atomic<std::uint64_t> reemit_storm_clear_health_pass_skipped_reentered_storm_total_{0};
    // Issue #2669: counter bumped when storm-clear health pass actually drives
    // recovery body (any of: deferred reemit / #2601 retry / cascade trigger).
    // Distinguishes body-driven passes from the #2639 counter-only baseline.
    std::atomic<std::uint64_t> reemit_storm_clear_health_pass_reemit_driven_total_{0};

    // Issue #2952: auto coverage-verify min-dirty on storm clear / force drain.
    //   scheduled: residual uncovered + production → min-dirty re-seed + drive
    //   success: reemit pipeline accepted (n>0) after schedule
    //   residual_uncovered: residual observed (Soft observe + production schedule)
    //   storm_skip: residual path aborted because storm re-active mid-pass
    std::atomic<std::uint64_t> coverage_verify_scheduled_total_{0};
    std::atomic<std::uint64_t> coverage_verify_success_total_{0};
    std::atomic<std::uint64_t> coverage_verify_residual_uncovered_total_{0};
    std::atomic<std::uint64_t> coverage_verify_storm_skip_total_{0};

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
    // Issue #2748: steady-clock ms stamp when defer was set (0 = not pending).
    std::atomic<std::uint64_t> reemit_deferred_since_ms_{0};
    std::atomic<std::uint64_t> reemit_deferred_age_max_observed_ms_{0};
    std::atomic<std::uint64_t> reemit_deferred_deadline_hit_total_{0};
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
    std::atomic<std::uint8_t> storm_isolation_mode_{2}; // PerEval (#2683)
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
    // Issue #2601: exhausted min-dirty retry closed loop under sustained
    // Global storm. After the one-shot min-dirty reemit (#2544) exhausts,
    // schedule a bounded retry series (cap 2-3, backoff) — only while
    // force_jit_regions_mask_ != 0. Zero-cost when no force-JIT demotion.
    // Counter family:
    //   aot_exhausted_min_dirty_retry_total        — retries attempted
    //   aot_exhausted_min_dirty_retry_success_total — reemit returned >0
    //   aot_exhausted_min_dirty_retry_storm_skip_total — storm skipped
    //   aot_exhausted_min_dirty_retry_cap_hit_total — cap hit / no attempts left
    enum class ExhaustedMinDirtyRetryDecision : std::uint8_t {
        NoForceJit = 0,        // force_jit_regions_mask_ == 0 (idle)
        NoAttemptsLeft = 1,    // cap exhausted
        BackoffNotElapsed = 2, // now - last_at < backoff_ms
        StormActive = 3,       // storm level != None or hard storm
        Retry = 4,             // ready to retry
    };
    void set_exhausted_min_dirty_retry_cap(std::uint32_t n) noexcept;
    [[nodiscard]] std::uint32_t exhausted_min_dirty_retry_cap() const noexcept;
    void set_exhausted_min_dirty_retry_backoff_ms(std::uint64_t ms) noexcept;
    [[nodiscard]] std::uint64_t exhausted_min_dirty_retry_backoff_ms() const noexcept;
    [[nodiscard]] std::uint32_t exhausted_min_dirty_retry_attempts_left() const noexcept;
    [[nodiscard]] std::uint64_t exhausted_min_dirty_retry_last_at_ms() const noexcept;
    [[nodiscard]] std::uint8_t exhausted_min_dirty_retry_last_reason() const noexcept;
    [[nodiscard]] std::uint64_t aot_exhausted_min_dirty_retry_total() const noexcept;
    [[nodiscard]] std::uint64_t aot_exhausted_min_dirty_retry_success_total() const noexcept;
    [[nodiscard]] std::uint64_t aot_exhausted_min_dirty_retry_storm_skip_total() const noexcept;
    [[nodiscard]] std::uint64_t aot_exhausted_min_dirty_retry_cap_hit_total() const noexcept;
    // Issue #2601 AC2: optional policy knob — when enabled, advance the
    // #2502 re-promote streak even if pending_dirty_count_ > 0, IF the
    // clean success covered the force-JIT reason regions (successes > 0
    // on a reemit pipeline call with force_jit_regions_mask_ != 0).
    // Default off (preserves #2502 require-pending-idle behavior).
    void set_force_jit_repromote_allow_pending_idle_when_force_jit_covered(bool allow) noexcept;
    [[nodiscard]] bool
    force_jit_repromote_allow_pending_idle_when_force_jit_covered() const noexcept;
    // Issue #2601: decide whether the next reemit pipeline call should
    // trigger a deferred exhausted-min-dirty retry. Returns the
    // decision enum (Retry means caller should drive the retry).
    [[nodiscard]] ExhaustedMinDirtyRetryDecision decide_exhausted_min_dirty_retry() const noexcept;
    // Consume one retry attempt: decrement attempts_left, stamp last_at.
    // Caller decides whether to actually drive the reemit (this is the
    // single bookkeeping site).
    void consume_exhausted_min_dirty_retry_attempt() noexcept;
    // Test isolation: clear retry state + counters without touching
    // force_jit_regions_mask_ (use on_reload_success for that).
    void reset_exhausted_min_dirty_retry_for_test() noexcept;

    // Issue #2639: storm-clear edge detection. Detects non-None → None
    // storm level transition with pending state (deferred/force-JIT/
    // region mask) and fires a health pass. Soft zero-cost on quiet
    // path (storm already None, no deferred/force-JIT). Amortized —
    // call from on_reemit_pipeline_call (cheap read path).
    void maybe_storm_clear_health_pass() noexcept;
    [[nodiscard]] std::uint64_t reemit_storm_clear_health_pass_total() const noexcept {
        return reemit_storm_clear_health_pass_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t reemit_storm_clear_health_pass_success_total() const noexcept {
        return reemit_storm_clear_health_pass_success_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t
    reemit_storm_clear_health_pass_skipped_reentered_storm_total() const noexcept {
        return reemit_storm_clear_health_pass_skipped_reentered_storm_total_.load(
            std::memory_order_relaxed);
    }
    // Issue #2669: body-driven pass counter (any recovery branch drove work).
    [[nodiscard]] std::uint64_t
    reemit_storm_clear_health_pass_reemit_driven_total() const noexcept {
        return reemit_storm_clear_health_pass_reemit_driven_total_.load(std::memory_order_relaxed);
    }
    // Test isolation: reset storm-clear health pass state.
    void reset_storm_clear_health_pass_for_test() noexcept;

    // Issue #2952: production storm-clear / force-drain auto coverage-verify
    // min-dirty for residual force bits (force & ~last_reemit_success).
    // Soft / mask idle → zero extra work (observe residual-uncovered only).
    // Returns true when a min-dirty reemit was scheduled (seed + #2601 drive).
    // Cap/backoff/storm-skip from #2601 respected; no silent drop on re-entry.
    bool
    maybe_coverage_verify_min_dirty(ReemitReason reason = ReemitReason::CoverageVerify) noexcept;
    // Issue #3221: last reason passed to decide_and_reemit (test / Agent
    // hook). Cascade=0 … ResidualForceHeal=7. No new query:* name.
    [[nodiscard]] ReemitReason last_reemit_reason() const noexcept {
        return static_cast<ReemitReason>(last_reemit_reason_.load(std::memory_order_relaxed));
    }
    // Pure resolve: env AURA_COVERAGE_VERIFY_MIN_DIRTY=0/1 → Soft/sandbox=off
    // → production_defaults → false.
    [[nodiscard]] bool resolve_coverage_verify_min_dirty_enabled() const noexcept;
    [[nodiscard]] std::uint64_t coverage_verify_scheduled_total() const noexcept {
        return coverage_verify_scheduled_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t coverage_verify_success_total() const noexcept {
        return coverage_verify_success_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t coverage_verify_residual_uncovered_total() const noexcept {
        return coverage_verify_residual_uncovered_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t coverage_verify_storm_skip_total() const noexcept {
        return coverage_verify_storm_skip_total_.load(std::memory_order_relaxed);
    }
    void reset_coverage_verify_for_test() noexcept;

    // Issue #2367: force-JIT observability (paired with recovery query).
    [[nodiscard]] std::uint64_t force_jit_for_reason_total() const noexcept {
        return force_jit_for_reason_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint8_t last_force_jit_reason() const noexcept {
        return last_force_jit_reason_.load(std::memory_order_relaxed);
    }
    // Issue #2927: last reason→bit index (0xFF when none / Ok).
    [[nodiscard]] std::uint8_t last_force_jit_mapped_bit() const noexcept {
        return last_force_jit_mapped_bit_.load(std::memory_order_relaxed);
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

    // Issue #2690: nested types so the method declarations at L314-315 can
    // resolve `PendingRecovery` + `DrainReason` via the enclosing class
    // scope (the namespace closes at L832; the old block was at global
    // module scope and the `module;` directive made `::DrainReason`
    // unresolvable from inside the class).
    struct PendingRecovery {
        std::uint8_t kinds = 0;           // bit0 deferred | bit1 force_jit | bit2 region_mask
        std::uint64_t defuse_version = 0; // valid when kinds & Deferred
        std::uint64_t region_mask = 0;    // valid when kinds & RegionMask
    };
    inline static constexpr std::uint8_t kPendingDeferred = 1u << 0;
    inline static constexpr std::uint8_t kPendingForceJit = 1u << 1;
    inline static constexpr std::uint8_t kPendingRegionMask = 1u << 2;
    enum class DrainReason : std::uint8_t {
        StormClear = 0,   // from maybe_storm_clear_health_pass (#2669)
        BoundaryExit = 1, // from outermost MutationBoundary success exit (#2604)
        Explicit = 2,     // optional Agent/query hook (no third silent path)
        // DrainReason::StormClear / DrainReason::BoundaryExit / DrainReason::Explicit
        // drive drain_pending_recovery(why) (#2690 unified drain contract).
    };
    inline static constexpr int kPendingRecoveryDrainIssue = 2690;

    [[nodiscard]] PendingRecovery exchange_pending_recovery() noexcept;
    void drain_pending_recovery(std::uint8_t why) noexcept;
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
    // Issue #2601: exhausted min-dirty retry closed loop (additive).
    std::int64_t aot_exhausted_min_dirty_retry_total;
    std::int64_t aot_exhausted_min_dirty_retry_success_total;
    std::int64_t aot_exhausted_min_dirty_retry_storm_skip_total;
    std::int64_t aot_exhausted_min_dirty_retry_cap_hit_total;
    std::int64_t exhausted_min_dirty_retry_attempts_left;
    std::int64_t exhausted_min_dirty_retry_attempts_cap;
    std::int64_t exhausted_min_dirty_retry_backoff_ms;
    std::int64_t exhausted_min_dirty_retry_last_at_ms;
    std::int64_t exhausted_min_dirty_retry_last_reason;
    std::int64_t force_jit_repromote_allow_pending_idle_when_force_jit_covered;
    std::int64_t schema_2601;
    std::int64_t issue_2601;
};
void aura_hot_update_registry_get_snapshot(aura_hot_update_registry_snapshot* out);
// Issue #2014: C entry points for deopt feed / throttle / config.
void aura_hot_update_note_deopt(void);
int aura_hot_update_should_throttle_reemit(void);
// Issue #2273: steal-path observability C entry point.
void aura_hot_update_on_deferred_reemit_seen_on_steal(std::int64_t fiber_id);
// Issue #2604: outermost MutationBoundary exit auto-drain deferred
// reemit + one region-filtered pass. C ABI bumpers in
// hot_update_registry.cpp.
void aura_bump_reemit_auto_drain_on_boundary_exit_total(void);
void aura_bump_reemit_auto_drain_success_total(void);
void aura_bump_reemit_auto_drain_throttled_total(void);
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

// Issue #2977: residual remount prefer (sid bit ∩ force_jit | last_success).
extern "C" std::uint64_t aura_hot_update_force_jit_regions_mask(void);
extern "C" std::uint64_t aura_hot_update_last_reemit_success_region_mask(void);
// Issue #3026: residual force mask + stale-observe (no auto-reemit).
extern "C" std::uint64_t aura_hot_update_residual_force_mask(void);
extern "C" std::uint64_t aura_hot_update_residual_force_stale_observe_total(void);
// Issue #3229: hashed-name define-id side set (C ABI for remount).
extern "C" int aura_hot_update_relower_success_define_active(void);
extern "C" int aura_hot_update_relower_success_covers_define(std::uint32_t id);
// Issue #3096: production-only bounded auto-heal lifetime counter
// (refine #3026). 0 under Soft / Off (zero-cost contract).
extern "C" std::uint64_t aura_hot_update_residual_force_auto_heal_total(void);
extern "C" void aura_hot_update_observe_residual_force_stale(void);
extern "C" void aura_hot_update_reset_residual_force_observe_for_test(void);

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
    // Issue #2895: last success coverage + partial re-promote (additive).
    std::int64_t last_reemit_success_region_mask;
    std::int64_t force_jit_repromote_only_covered_bits; // effective (#2949 resolve)
    std::int64_t force_jit_repromote_partial_total;
    std::int64_t schema_2895; // 2895 when wired
    std::int64_t issue_2895;  // 2895
    // Issue #2949: production default only_covered (additive sentinel).
    std::int64_t force_jit_repromote_only_covered_default_wired; // 1 when #2949 resolve wired
    std::int64_t schema_2949;                                    // 2949 when wired
    std::int64_t issue_2949;                                     // 2949
    // Issue #2952: coverage-verify min-dirty closed loop (additive).
    std::int64_t coverage_verify_scheduled_total;
    std::int64_t coverage_verify_success_total;
    std::int64_t coverage_verify_residual_uncovered_total;
    std::int64_t coverage_verify_storm_skip_total;
    std::int64_t coverage_verify_min_dirty_wired; // 1 when #2952 path wired
    std::int64_t schema_2952;                     // 2952 when wired
    std::int64_t issue_2952;                      // 2952
    // Issue #2601: exhausted min-dirty retry closed loop (additive).
    std::int64_t aot_exhausted_min_dirty_retry_total;
    std::int64_t aot_exhausted_min_dirty_retry_success_total;
    std::int64_t aot_exhausted_min_dirty_retry_storm_skip_total;
    std::int64_t aot_exhausted_min_dirty_retry_cap_hit_total;
    std::int64_t exhausted_min_dirty_retry_attempts_left;
    std::int64_t exhausted_min_dirty_retry_attempts_cap;
    std::int64_t exhausted_min_dirty_retry_backoff_ms;
    std::int64_t exhausted_min_dirty_retry_last_at_ms;
    std::int64_t exhausted_min_dirty_retry_last_reason;
    std::int64_t force_jit_repromote_allow_pending_idle_when_force_jit_covered;
    std::int64_t schema_2601; // 2601 when wired
    // Issue #2639: storm-clear health pass counters (additive).
    std::int64_t reemit_storm_clear_health_pass_total;
    std::int64_t reemit_storm_clear_health_pass_success_total;
    std::int64_t reemit_storm_clear_health_pass_skipped_reentered_storm_total;
    std::int64_t schema_2639; // 2639 when wired
    std::int64_t issue_2639;  // 2639
    // Issue #2669: storm-clear drives recovery body (additive counter).
    std::int64_t reemit_storm_clear_health_pass_reemit_driven_total;
    std::int64_t schema_2669; // 2669 when wired
    std::int64_t issue_2669;  // 2669
    // Issue #2927: reason→bit map SSOT (additive query keys).
    // last_force_jit_mapped_bit: bit index from
    // aot_reload_fail_to_force_jit_bit_index; -1 when none/Ok (0xFF).
    // force_jit_reason_bit_map_wired: always 1 when #2927 linked.
    std::int64_t last_force_jit_mapped_bit;
    std::int64_t force_jit_reason_bit_map_wired;
    std::int64_t schema_2927; // 2927 when wired
    std::int64_t issue_2927;  // 2927
    // Issue #2982: Staging/Dlopen ops recovery surface (additive).
    // last_ops_fail_kind: 0=None 1=Staging 2=Dlopen.
    std::int64_t last_ops_fail_kind;
    std::int64_t last_dlopen_path_hash;
    std::int64_t last_dlopen_errno_class;
    std::int64_t last_staging_detail;
    std::int64_t staging_retry_eligible;
    std::int64_t staging_retry_scheduled_total;
    std::int64_t ops_fail_wired; // 1 when #2982 linked
    std::int64_t schema_2982;    // 2982 when wired
    std::int64_t issue_2982;     // 2982
    // recovery-active: 1 when any non-idle recovery signal is set
    // (force-jit mask, attempts_left, pending dirty, deferred reemit,
    // storm_level != None). Soft empty path → 0.
    std::int64_t recovery_active;
    std::int64_t reload_recovery_wired; // always 1 when linked
    // Issue #3026: residual force (force & ~last_success) + stale observe.
    // Appended — existing snapshot field offsets unchanged.
    std::int64_t residual_force_mask;
    std::int64_t residual_force_stale_observe_total;
    std::int64_t residual_force_observe_wired; // 1 when #3026 linked
    std::int64_t schema_3026;
    std::int64_t issue_3026;
    // Issue #3096: production-only bounded auto-heal (refine #3026).
    // Appended — existing snapshot field offsets unchanged.
    std::int64_t residual_force_auto_heal_total;
    std::int64_t residual_force_auto_heal_wired; // 1 when #3096 linked
    std::int64_t schema_3096;
    std::int64_t issue_3096;
};
void aura_hot_update_reload_recovery_get_snapshot(aura_reload_recovery_snapshot* out);

// Issue #2953: Agent recovery playbook — single recommended action from
// existing recovery snapshot atomics. Pure observe-only (no reemit/drain/
// reload). Decision table (priority top→bottom; source-cite not docs/design):
//   1. reject-cross-ws  last cross-workspace reject != None (#2178/#2240)
//   2. wait-storm       storm_level != None || hard_storm
//   3. force-drain      deferred pending && force_drain_deadline>0 && age>=deadline
//                       (#2748/#2855)
//   4. retry-reload     attempts_left>0 && last_reason in Version|Env|Linear|Defuse
//   5. reemit           storm None && (pending_dirty || deferred || residual force)
//                       residual = force_mask & ~last_reemit_success_region_mask
//                       #3026 agent hint: min-dirty residual → reemit → coverage
//   6. fall-back-jit    force_mask != 0 (exhausted / demotion-only, no reemit work)
//   7. idle             recovery_active == 0 (and no higher branch)
enum class ReloadRecoveryPlaybookAction : std::uint8_t {
    Idle = 0,
    WaitStorm = 1,
    ForceDrain = 2,
    Reemit = 3,
    RetryReload = 4,
    FallBackJit = 5,
    RejectCrossWs = 6,
};
// Pure decision input (tests / C ABI fill from live atomics).
struct ReloadRecoveryPlaybookInput {
    std::uint8_t storm_level = 0;
    std::uint8_t hard_storm_active = 0;
    std::uint8_t deferred_pending = 0; // recovery v2 and/or boundary deferred
    std::uint64_t deferred_age_ms = 0;
    std::uint64_t force_drain_deadline_ms = 0; // 0 = force-drain branch disabled
    std::uint32_t attempts_left = 0;
    std::uint8_t last_reason = 0; // AotReloadFail
    std::uint64_t force_jit_regions_mask = 0;
    std::uint64_t last_reemit_success_region_mask = 0;
    std::uint64_t pending_dirty_count = 0;
    std::uint8_t cross_ws_reject = 0; // CrossWorkspaceReject; 0 = None
    std::uint8_t recovery_active = 0;
};
struct ReloadRecoveryPlaybookResult {
    ReloadRecoveryPlaybookAction action = ReloadRecoveryPlaybookAction::Idle;
    // Rationale echo (same values Agents already OR — single action is primary).
    std::int64_t storm_level = 0;
    std::int64_t deferred_pending = 0;
    std::int64_t deferred_age_ms = 0;
    std::int64_t force_drain_deadline_ms = 0;
    std::int64_t attempts_left = 0;
    std::int64_t last_reason = 0;
    std::int64_t force_jit_regions_mask = 0;
    std::int64_t residual_force_mask = 0; // force & ~last_success
    std::int64_t pending_dirty_count = 0;
    std::int64_t cross_ws_reject = 0;
    std::int64_t recovery_active = 0;
    std::int64_t playbook_wired = 1;
    std::int64_t schema_2953 = 2953;
    std::int64_t issue_2953 = 2953;
    // Issue #3026: observe-only agent hint (appended). 1 when residual != 0
    // meaning min-dirty residual bits → reemit → coverage. Never auto-heals.
    std::int64_t playbook_hint_min_dirty_reemit = 0;
    std::int64_t schema_3026 = 3026;
    std::int64_t issue_3026 = 3026;
};
// Pure: no atomics, no mutation. Soft empty input → Idle.
// C++ linkage OK for unit tests (not called from module partitions).
[[nodiscard]] ReloadRecoveryPlaybookResult
aura_reload_recovery_playbook_decide(const ReloadRecoveryPlaybookInput& in) noexcept;
// Live fill from existing atomics (snapshot + age + cross-ws + deadline).
// Observe-only: does not call reemit/drain/reload.
// extern "C" so module partitions (evaluator) can call without module attachment.
extern "C" void
aura_hot_update_reload_recovery_playbook_get(ReloadRecoveryPlaybookResult* out) noexcept;

// Issue #2094: setter for ShapeProfiler (or tests) to publish its
// deopt_storm_active state without needing to import shape_profiler.h.
extern "C" void aura_hot_update_set_shape_storm_active(int active);
void aura_hot_update_set_deopt_storm_threshold(std::uint64_t deopts_per_window,
                                               std::uint64_t window_ms);
void aura_hot_update_reset_deopt_storm_state_for_test(void);
// Issue #3070: 1 while storm-exit force-full cooldown is live (consumes one consult).
int aura_hot_update_storm_exit_force_full_active(void);
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
// Issue #2601: exhausted min-dirty retry closed loop C ABI. Defined in
// aura_jit_bridge.cpp where aura_reemit_aot_for_dirty + aot_metrics live.
// Called from on_reemit_pipeline_call (registry lazy hook) and from
// explicit drain on the bridge exhaust path. Bounded by attempts_cap +
// backoff_ms, so recursion within a single reemit pipeline call is safe.
extern "C" void aura_hot_update_maybe_retry_exhausted_min_dirty(void);
// Issue #2639: storm-clear edge detection hook (lazy — called from
// on_reemit_pipeline_call amortized path).
extern "C" void aura_hot_update_maybe_storm_clear_health_pass(void);

// Issue #2690: C ABI hooks for tests + future Agent/query hook. The
// nested types (PendingRecovery, DrainReason, constants) live inside
// `HotUpdateRegistry` (added at the end of the class above) so the
// `module;` directive at file scope doesn't hide them from the
// `exchange_pending_recovery()` / `drain_pending_recovery(...)` method
// declarations. The C ABI hooks at file scope reach the nested types
// via `HotUpdateRegistry::PendingRecovery` + `HotUpdateRegistry::DrainReason`.
extern "C" [[nodiscard]] aura::compiler::HotUpdateRegistry::PendingRecovery
aura_hot_update_exchange_pending_recovery() noexcept;
extern "C" void aura_hot_update_drain_pending_recovery(std::uint8_t reason) noexcept;

// Process-wide counters (additive — no schema break).
inline std::atomic<std::uint64_t>& g_pending_recovery_driven_total_atomic() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_pending_recovery_success_total_atomic() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_pending_recovery_skipped_reentered_total_atomic() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>&
g_pending_recovery_double_drain_prevented_total_atomic() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
}

#endif // AURA_COMPILER_HOT_UPDATE_REGISTRY_HH
