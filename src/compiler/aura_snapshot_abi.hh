// Issue #5237: single source for the C-ABI hot-update snapshots.
//
// Two hand-copied struct definitions used to drift: #5144 corrupted 40 bytes
// of stack when the production writer gained a field and the shadow copy did
// not. The producer (hot_update_registry.hh) and the module-side consumer
// (evaluator_primitives_mutate.cpp) now include this header, and the asserts
// below make a field-count or layout change fail to compile.
#ifndef AURA_COMPILER_AURA_SNAPSHOT_ABI_HH_
#define AURA_COMPILER_AURA_SNAPSHOT_ABI_HH_

#include <cstddef>
#include <cstdint>

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
    // Issue #3636: per-region force watermark + storm attribution. MUST
    // stay in lockstep with HotUpdateRegistry::Snapshot (same rule as
    // #2236: missing fields overflow adjacent memory on write).
    std::int64_t region_force_max_age_ms;
    std::int64_t reemit_throttle_cause_mask;
    std::int64_t reemit_soft_storm_region_skips_total;
    std::int64_t schema_3636;
    std::int64_t issue_3636;
};

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
    // Issue #3541: per-eval force overlay residual (append END).
    std::int64_t force_mask_peer_residual_total;
    std::int64_t force_mask_peer_scope_wired; // 1 when #3541 linked
    std::int64_t schema_3541;
    std::int64_t issue_3541;
};

} // extern "C"

static_assert(sizeof(aura_hot_update_registry_snapshot) == 94 * sizeof(std::int64_t),
              "aura_hot_update_registry_snapshot field count changed; update the C ABI readers");
static_assert(offsetof(aura_hot_update_registry_snapshot, issue_3636) == 93 * sizeof(std::int64_t),
              "aura_hot_update_registry_snapshot layout changed");
static_assert(sizeof(aura_reload_recovery_snapshot) == 89 * sizeof(std::int64_t),
              "aura_reload_recovery_snapshot field count changed; update the C ABI readers");
static_assert(offsetof(aura_reload_recovery_snapshot, issue_3541) == 88 * sizeof(std::int64_t),
              "aura_reload_recovery_snapshot layout changed");

#endif // AURA_COMPILER_AURA_SNAPSHOT_ABI_HH_
