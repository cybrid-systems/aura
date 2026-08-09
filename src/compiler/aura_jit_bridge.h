// src/compiler/aura_jit_bridge.h
//
// C-linkage declarations for the AOT/JIT bridge functions
// defined in `aura_jit_bridge.cpp`. The bridge layer is a
// pure C-ABI surface (called from generated C registration
// code, from host code, and from test_issue_287.cpp), so the
// header is plain `extern "C"` rather than a C++ module.
//
// Functions:
//   - aura_set_aot_defuse_version / aura_get_aot_defuse_version
//     (Issue #243 — runtime mutation epoch at emit time)
//   - aura_set_module_version / aura_get_module_version
//     (Issue #287 — user-facing module version for hot-reload
//      and multi-agent isolation)
//   - aura_reload_aot_module(path, version)
//     (Issue #287 / #2012 — host-facing hot-reload: staged
//      constructor registration, atomic func_table swap via
//      commit_func_table_swap, rollback on validation failure.)
//
// These declarations are the minimum needed to call the
// bridge from a non-module translation unit (test files,
// generated registration .c, host loaders). The C++ module
// `aura.compiler.aura_jit_bridge` (if it exists) re-exports
// the same names.

#ifndef AURA_COMPILER_AURA_JIT_BRIDGE_H
#define AURA_COMPILER_AURA_JIT_BRIDGE_H

#include <cstdint>

extern "C" {

// #243 — runtime mutation epoch at AOT emit time.
void aura_set_aot_defuse_version(std::uint64_t v);
std::uint64_t aura_get_aot_defuse_version(void);

// Issue #1485 C2-wire: current bridge_epoch tracker for the
// aura_closure_call 2-check (refine #1475). Set by the C++ side
// (service.ixx::bump_bridge_epoch) every time the workspace's
// bridge_epoch advances; stamped into per-closure
// AuraClosure::bridge_epoch at aura_alloc_closure time. Mismatch
// at aura_closure_call → return 0 (caller falls back to
// interpreter via aura_jit.cpp OpApply emit's deopt-to-interpreter
// path).
void aura_set_current_bridge_epoch(std::uint64_t v);
std::uint64_t aura_get_current_bridge_epoch(void);

// #287 — user-facing module version (hot-reload / multi-agent).
void aura_set_module_version(std::uint64_t v);
std::uint64_t aura_get_module_version(void);

// #287 / #2012 — AOT hot-reload with atomic func_table swap.
//   path    - path to the new .so/.dylib
//   version - expected aot_emit_version; 0 = trust binary's own
//
// Returns true on successful load (dlopen OK + version/region checks
// passed + staged constructor registrations applied + epoch bump).
// On failure, staged registrations are discarded, live table is left
// intact, aot_hot_update_atomic_rollback is incremented, and false is
// returned. Concurrent aura_closure_call observers see either fully
// old or fully new symbols relative to aura_aot_func_table_epoch().
bool aura_reload_aot_module(const char* path, std::uint64_t version);

// Issue #2012 / #2046: probe live func_table slot.
// Returns 0 if empty, out of range, OR generation-behind current
// aura_aot_func_table_epoch() (stale after invalidate / joint epoch bump).
std::uintptr_t aura_aot_probe_fn_ptr(std::int64_t func_id);
// Issue #2046: raw pointer (ignores generation; tests / recovery only).
std::uintptr_t aura_aot_probe_fn_ptr_raw(std::int64_t func_id);
// Issue #2046: 1 if empty/out-of-range/generation-behind, else 0.
int aura_aot_slot_is_stale(std::int64_t func_id);

// Issue #1368: AOT metrics pointer lifecycle
//   aura_set_aot_metrics — explicit host wire-up (overwrites)
//   aura_ensure_aot_metrics — lazy bind only if currently null
//   aura_get_aot_metrics — current pointer (may be null)
// aura_set_aot_metrics is declared in runtime_shared.h (CompilerMetrics*).
void aura_ensure_aot_metrics(void* metrics);
void* aura_get_aot_metrics(void);

// Issue #2092: thin C-linkage helper for bumping the legacy
// name-fallback counter from aura_jit_runtime.cpp (which only has
// the forward declaration of CompilerMetrics via runtime_shared.h).
// Production impl in aura_jit_bridge.cpp; weak stub in
// aura_jit_bridge_stub.cpp so light test binaries link cleanly.
void aura_bump_live_closure_remap_name_fallback_total(std::uint64_t n);
void aura_bump_live_closure_sync_remount_totals(std::uint64_t ok, std::uint64_t fail);
// Issue #2175 / #2550 / #2605: residual sid=0 named closures (pre-#2550 or
// force-injected) get a one-shot backfill via get_or_preserve during
// aura_remap_live_closures_after_reemit. Named set_name now stamps
// non-zero sid at create (#2550) so steady-state residual_backfill growth ≈ 0.
// Anonymous paths never set_name and stay sid=0. Bumped inline by the
// remap walk under the closure-table lock — no aggregator needed.
void aura_bump_live_closure_stable_id_backfill_total(std::uint64_t n);
// Issue #2605: named name-fallback invent refused (fail-closed).
void aura_bump_live_closure_named_name_fallback_reject_total(std::uint64_t n);
// Issue #2128: MustDeoptBeforeNextCall metric bumps (runtime → bridge).
void aura_bump_must_deopt_before_next_call_total(std::uint64_t n);
// Issue #2233: post-reemit live-closure stamp metric bumpers
// (hit / miss split). See observability_metrics.h.
void aura_bump_live_closure_epoch_restamp_total(std::uint64_t n);
void aura_bump_live_closure_must_deopt_kept_total(std::uint64_t n);
// Issue #2234: post-reemit / post-compact env_frame + linear capture
// remount metric bumpers. See observability_metrics.h for the
// per-outcome semantics — the #2234 pair measures the capture
// state rebind outcome (ok vs fail), distinct from the #2233
// epoch_restamp_total which only stamps func_id.
void aura_bump_closure_capture_remount_ok_total(std::uint64_t n);
void aura_bump_closure_capture_remount_fail_total(std::uint64_t n);
// Issue #2272: env_generation mismatch counter (PRIMARY env axis in
// aura_remount_closure_captures). Distinct from the legacy
// remount_fail_total (which still counts defuse-only failures).
void aura_bump_closure_capture_env_gen_mismatch_total(std::uint64_t n);
// Issue #2234: post-reemit / post-compact env_frame + linear capture
// remount hook. Reads the closure's stamped g_closure_defuse_versions
// + g_closure_linear_state (captured at closure create / re-stamp)
// and compares with the live values passed in. Returns true when all
// env captures are rebound to the live generation + linear ownership
// is consistent; returns false when the caller must set MustDeopt
// (the per-closure defuse_version is the env-frame proxy; the
// per-closure linear_state is the linear ownership proxy).
int aura_remount_closure_captures(std::int64_t closure_id, std::uint64_t live_env_gen,
                                  std::uint8_t linear_fp);
// Issue #2503: remount + unified fail transaction. Calls remount; on any
// fail (env_gen / defuse / linear / densify cell remap) sets MustDeopt and
// invokes aura_jit_batch_deopt_for(name) when the closure has a name.
// Metrics stay distinct (env_gen_mismatch / cell_remap_fail / remount_fail).
// Returns 1 on remount ok, 0 on fail (force-deopt path applied). Prefer this
// over bare aura_remount_closure_captures at production call sites.
int aura_remount_or_force_deopt(std::int64_t closure_id, std::uint64_t live_env_gen,
                                std::uint8_t linear_fp);
// Issue #2234: capture detection helper. Returns true when the
// closure has any env or linear capture to remount (proxied by
// non-zero defuse_version or non-zero linear_state). The remap +
// compact paths use this to skip the remount call when no captures
// exist (AC4 zero overhead hot path).
int aura_closure_has_env_or_linear_captures(std::int64_t closure_id);

// Issue #2297: densify-time object_remap context for structural
// capture-cell remount (defense-in-depth after env_gen PRIMARY).
// Published by RootRemapPass / Moving densify; consulted by
// aura_remount_closure_captures after fingerprint OK. Empty / null
// → zero extra work (AC3). Parallel olds/news arrays of length n.
void aura_set_densify_object_remap(const void* const* olds, const void* const* news, std::size_t n);
void aura_clear_densify_object_remap(void);
// Optional densify-candidate overlay (fail-closed when a capture cell
// points at a densified-away address with no remap entry).
void aura_set_densify_candidates(const void* const* cands, std::size_t n);
void aura_clear_densify_candidates(void);
// Issue #2297: structural capture-cell remap metrics.
void aura_bump_closure_capture_cell_remap_ok_total(std::uint64_t n);
void aura_bump_closure_capture_cell_remap_fail_total(std::uint64_t n);
void aura_bump_must_deopt_force_deopt_success_total(std::uint64_t n);
void aura_bump_must_deopt_force_deopt_fail_total(std::uint64_t n);

// Issue #2310: fail-closed force-deopt on steal snapshot inconsistency.
// Bumped from WorkerThread::try_steal_from success path when
// mutation_safety_snapshot_inconsistent(snap) AND NOT in soft mode
// (AURA_STEAL_SNAPSHOT_SOFT=1). Also bumped from
// Evaluator::refresh_after_fiber_migration re-sample fence (AC2
// defense-in-depth). Strong def in evaluator_fiber_mutation.cpp;
// aura_jit_bridge.cpp provides file-level atomic fallback; weak no-op
// in fiber_bridge.cpp for light test binaries.
void aura_force_deopt_on_steal_snapshot_mismatch(void* fiber_ptr) noexcept;
// Issue #2310: static accessor for the force-deopt counter. Returns
// per-CompilerMetrics value when scheduler evaluator is live (via
// evaluator_for_scheduler_hooks), otherwise file-level atomic fallback
// in aura_jit_bridge.cpp.
std::uint64_t aura_static_steal_snapshot_mismatch_force_deopt_total() noexcept;

// Issue #2094: StormLevel facade accessor (C ABI). Returns the
// combined bitmask of shape-storm + global-deopt-storm detectors
// so external callers can branch on a single recovery-policy value.
// Result mapping (uint8_t): 0=None, 1=Shape, 2=Global, 3=Both.
extern "C" std::uint8_t aura_hot_update_current_storm_level(void);

// Issue #2094: setter for ShapeProfiler (or tests) to publish its
// deopt_storm_active state without needing to import shape_profiler.h.
extern "C" void aura_hot_update_set_shape_storm_active(int active);

// Issue #2095: postmortem hook for default-LLVM reemit failures.
// env-gated via AURA_REEMIT_KEEP_FAIL=1 (or AURA_REEMIT_KEEP_FAIL_N>0).
// When enabled, the failed .o is renamed into /tmp/aura_reemit_failed/
// instead of being removed so the Agent can inspect with llvm-objdump.
extern "C" int aura_reemit_keep_fail_enabled(void);
extern "C" void aura_reemit_keep_failed_obj(const char* obj_path, const char* reason);

// Issue #2093: structured reload-failure reason codes. Agents branch
// on this enum (via aura_aot_last_reload_fail_reason or the
// query:aot-reload-stats snapshot) to pick a recovery policy without
// log scraping — see issue body for the policy matrix
// (Version/Env/Linear → reemit+retry; Dlopen → path/ops; Staging →
// treat as bug). 0 (Ok) is the success state and the value cleared
// at the start of every aura_reload_aot_module call. Stable ABI: do
// not reorder — existing values are persisted in query snapshots.
enum class AotReloadFail : std::uint8_t {
    Ok = 0,
    Dlopen,
    Version,
    Region,
    Defuse,
    Env,
    Linear,
    Staging,
    Other,
};

// Issue #2093: getter for the last reload failure reason (file-scope
// atomic in aura_jit_bridge.cpp; thread-safe lock-free read).
// Returns AotReloadFail as uint8_t for C ABI stability.
extern "C" std::uint8_t aura_aot_last_reload_fail_reason(void);

// Issue #2753: AotReloadConsistencyProof lives in
// compiler/aot_reload_consistency_proof.h (thin include; stamp sites
// in aura_jit_bridge.cpp include it). See that header for the struct
// + build/stamp helpers + C ABI accessors.

// Issue #2240: stable cross-workspace / cross-COW reject reason code
// (refine #2178 — Agents branch on this without log scraping).
// Stored in process atomic `g_last_cross_workspace_reject_reason` in
// aura_jit_bridge.cpp; thread-safe lock-free read. Stable ABI: do
// not reorder — existing values are persisted in query snapshots.
// - None: success state (no reject); cleared at start of every
//   aura_reload_aot_module_for_eval attempt.
// - ForeignEval: rejected because eval_ptr is foreign to the current
//   workspace AotState map (single-workspace MVP, #1943).
// - CowGenMismatch: same-process diverged workspace COW generation
//   (#2275 reload path; #2547 call-time closure stamp). Still does
//   NOT open cross-workspace hot-update write (#2178 fail-closed).
// - Unknown: defensive; bumped if a future reject path doesn't
//   set a specific reason before bumping the counter.
enum class CrossWorkspaceReject : std::uint8_t {
    None = 0,
    ForeignEval = 1,
    CowGenMismatch = 2,
    Unknown = 3,
};

// Issue #2275 / #2547: process-level live workspace COW generation.
// Bumped on densify / workspace restamp; stamped into closures at
// aura_alloc_closure (#2547 cow_gen_at_capture). Call-time mismatch
// hard-rejects with CowGenMismatch (soft only within same gen).
extern "C" void aura_set_live_workspace_cow_gen(std::uint64_t gen) noexcept;
extern "C" std::uint64_t aura_get_live_workspace_cow_gen(void) noexcept;
// Issue #2547: per-closure cow_gen_at_capture (0 = unstamped / freed).
extern "C" std::uint64_t aura_get_closure_cow_gen(std::int64_t closure_id);
// Issue #2550: per-closure stable_func_id (0 = anonymous / unstamped / freed).
// Named set_name stamps non-zero via get_or_preserve before callable.
extern "C" std::uint32_t aura_get_closure_stable_func_id(std::int64_t closure_id);
// Issue #2550: test-only residual injector (simulate pre-#2550 sid=0).
extern "C" void aura_test_force_closure_stable_func_id(std::int64_t closure_id, std::uint32_t sid);

// Issue #2240: C-linkage readers for the last cross-workspace reject
// reason (file-scope atomic in aura_jit_bridge.cpp; thread-safe
// lock-free read). Returns CrossWorkspaceReject as uint8_t for C ABI
// stability. Test-only setter + reset for hermetic test isolation.
extern "C" std::uint8_t aura_last_cross_workspace_reject_reason_v_read(void) noexcept;
extern "C" const char* aura_cross_workspace_reject_reason_string(std::uint8_t v) noexcept;
extern "C" void aura_test_set_last_cross_workspace_reject_reason(std::uint8_t v) noexcept;
extern "C" void aura_test_reset_last_cross_workspace_reject_reason(void) noexcept;

// Issue #2241: per-fiber hygiene violation budget (refine #2097
// FiberHygieneStats). Agents / supervisors can throttle or deny
// further expand on fibers that have accumulated more than `budget`
// violations. Default 0 = unlimited (relaxed-by-default, matches
// #2228 / #2235 / #2238 pattern). When non-zero, clone_macro_body
// consults `aura_macro_self_evo_check_fiber_hygiene_budget(fiber_id)`
// at top-level entry — returns 1 (deny) if `violations > budget`,
// 0 (permit) otherwise. Denies bump the
// `g_macro_self_evo_fiber_violation_deny_total` counter (lock-free
// atomic) for Agent observability. Zero-cost fast path: budget == 0
// or fiber_id == 0 returns 0 without acquiring the per-fiber map
// lock. Set via AURA_MACRO_SELF_EVO_FIBER_VIOLATION_BUDGET env or
// the setter below. All symbols are file-scope atomics in
// src/compiler/macro_expansion.cpp (where the per-fiber map lives).
extern "C" void aura_macro_self_evo_set_fiber_violation_budget(std::uint64_t budget) noexcept;
extern "C" std::uint64_t aura_macro_self_evo_get_fiber_violation_budget(void) noexcept;
extern "C" std::uint64_t aura_macro_self_evo_fiber_violation_deny_total_v_read(void) noexcept;
extern "C" int aura_macro_self_evo_check_fiber_hygiene_budget(std::uint32_t fiber_id) noexcept;
extern "C" std::uint64_t
aura_macro_self_evo_count_fibers_meeting_filter(std::uint64_t min_violations,
                                                int min_depth) noexcept;
extern "C" void aura_test_reset_macro_self_evo_fiber_violation_deny_total_for_test(void) noexcept;

// Issue #2243: per-policy self-evo enforcement counters (refine #2241).
// Bumped from clone_macro_body at the force_hygienic deny fallback
// (depth-limit + invalid-body fallback paths) and the gensym-map-size
// exceeded gate in rename_binding_pre. Both are file-scope atomics in
// src/compiler/macro_expansion.cpp (where the per-policy TLS knobs
// live). Lock-free reads, safe for high-freq Agent polling.
extern "C" std::uint64_t aura_macro_self_evo_force_hygienic_denied_total_v_read(void) noexcept;
extern "C" std::uint64_t aura_macro_self_evo_gensym_map_size_exceeded_total_v_read(void) noexcept;
// Issue #2804: clone-walk gensym ceiling (distinct from pre-scan exceeded_total).
extern "C" std::uint64_t aura_clone_walk_gensym_ceiling_exceeded_total_v_read(void) noexcept;
// Issue #2804: test-only arm of TLS s_max_gensym_map_size for clone_macro_body.
extern "C" void aura_test_set_max_gensym_map_size_for_test(std::uint32_t n) noexcept;
extern "C" void aura_test_reset_clone_walk_gensym_ceiling_exceeded_total_for_test(void) noexcept;
// Issue #2811: gensym serial-drift prevented (ceiling deny without hyg_ctr++).
extern "C" std::uint64_t aura_gensym_serial_drift_total_v_read(void) noexcept;
extern "C" void aura_test_reset_gensym_serial_drift_total_for_test(void) noexcept;
// Issue #2805: dotted-rest fallback refused to map a hygiene_builtin name.
extern "C" std::uint64_t aura_dotted_rest_builtin_rename_prevented_total_v_read(void) noexcept;
extern "C" void aura_test_reset_dotted_rest_builtin_rename_prevented_total_for_test(void) noexcept;
// Issue #2806: concurrent top-level clone_macro_body (depth=0 overlap).
extern "C" std::uint64_t aura_clone_macro_body_concurrent_top_level_total_v_read(void) noexcept;
extern "C" void aura_test_reset_clone_macro_body_concurrent_top_level_total_for_test(void) noexcept;
// Issue #2807: unquote-splicing treated as caller-scope boundary in pre_scan.
extern "C" std::uint64_t aura_unquote_splicing_hygiene_mismatch_total_v_read(void) noexcept;
extern "C" void aura_test_reset_unquote_splicing_hygiene_mismatch_total_for_test(void) noexcept;
// Issue #2808: stamp_rest_param_hygiene MacroIntroduced marker set / skipped.
extern "C" std::uint64_t aura_stamp_rest_param_marker_set_total_v_read(void) noexcept;
extern "C" std::uint64_t aura_stamp_rest_param_marker_skipped_total_v_read(void) noexcept;
extern "C" void aura_test_reset_stamp_rest_param_marker_totals_for_test(void) noexcept;
// Issue #2808: invoke stamp_rest_param_hygiene from tests (void* = FlatAST*).
extern "C" void aura_test_call_stamp_rest_param_hygiene(void* target_flat, void* source_flat,
                                                        std::uint32_t src_body_id,
                                                        std::uint32_t list_root) noexcept;
// Issue #2809: expand_inner_macros qq-unwrap targeted vs full restamp metrics.
extern "C" std::uint64_t aura_macro_expand_targeted_restamp_total_v_read(void) noexcept;
extern "C" std::uint64_t aura_macro_expand_full_restamp_total_v_read(void) noexcept;
extern "C" void aura_test_reset_macro_expand_qq_restamp_totals_for_test(void) noexcept;
// Issue #2810: clone_macro_body provenance repin dual-write to per-CompilerMetrics.
//
// Bridge hook contract for aura_macro_provenance_repin_on_steal(ev_ptr, marker):
//   - Always bumps file-level fallback atomics (process-wide surface).
//   - When ev_ptr != nullptr (Evaluator*), dual-writes that Evaluator's
//     CompilerMetrics::macro_provenance_repin_on_steal_total.
//   - When ev_ptr == nullptr, resolves yield-hook / query TLS / scheduler
//     Evaluator; if still null, file-level only (true module-unaware path).
//   - Returns 2 when per-eval dual-write succeeded, 1 when file-level only,
//     0 on stub/no-op (light link units).
// clone_macro_body must pass the resolved Evaluator (via
// aura_evaluator_resolve_current_for_macro) so production dashboards see
// non-zero per-Evaluator clone rates (pre-#2810 always passed nullptr).
extern "C" void* aura_evaluator_resolve_current_for_macro(void) noexcept;
extern "C" int aura_evaluator_bump_macro_provenance_repin_on_steal(void* ev_ptr) noexcept;
extern "C" int aura_macro_provenance_repin_on_steal(void* ev_ptr, std::uint64_t cloned_marker);
extern "C" std::uint64_t aura_macro_provenance_repin_on_steal_total(void);
extern "C" std::uint64_t aura_clone_macro_provenance_per_evaluator_total_v_read(void) noexcept;
extern "C" void aura_test_reset_clone_macro_provenance_per_evaluator_total_for_test(void) noexcept;

// Issue #2165: auto reemit+retry on Version/Env/Linear/Defuse reload fails.
// Default ON (production). Set AURA_AOT_RELOAD_AUTO_RETRY=0 or call
// aura_set_aot_reload_auto_retry(0) for strict tests (#2093 counters).
// When enabled: one reemit via aura_reemit_aot_for_dirty then one retry;
// Version retry uses version=0 (trust binary after reemit). Dlopen/Region/
// Staging/Other never auto-retry.
extern "C" void aura_set_aot_reload_auto_retry(int enabled);
extern "C" int aura_aot_reload_auto_retry_enabled(void);

// Issue #2232: reason-driven multi-round reload recovery policy.
// Replaces the #2165 single-retry (TLS depth) with an iterative loop
// driven by a per-reason policy table. Production zero-downtime under
// sustained mutation (defuse/env churn) needs more than one retry
// and a safe exhausted fall-back (force JIT). Dlopen/Region/Staging/
// Other remain never-auto (path/ops/bug class — no recovery by design).
//
//   Version | Defuse   → {max_reemit=3, backoff_ms=5,  fall_back_jit_only=true}
//   Env | Linear      → {max_reemit=2, backoff_ms=10, fall_back_jit_only=true}
//   other             → {0, 0, false} (never auto)
//
// The policy is a small POD so tests can assert the table directly.
// `policy_for(r)` is the single source of truth (one-definition
// rule across TUs).
struct ReloadPolicy {
    int max_reemit;
    int backoff_ms;
    bool fall_back_jit_only;
};

inline ReloadPolicy policy_for(AotReloadFail r) noexcept {
    switch (r) {
        case AotReloadFail::Version:
        case AotReloadFail::Defuse:
            return ReloadPolicy{/*max_reemit=*/3, /*backoff_ms=*/5, /*fall_back_jit_only=*/true};
        case AotReloadFail::Env:
        case AotReloadFail::Linear:
            return ReloadPolicy{/*max_reemit=*/2, /*backoff_ms=*/10, /*fall_back_jit_only=*/true};
        // Issue #2249: conservative multi-round retry for Region /
        // Staging (extends #2232). Smaller max (2) + longer backoff
        // (15ms) than Env/Linear since region-mask races + staging
        // handshakes typically need a boundary tick to recover. Still
        // falls back to JIT-only on exhausted (same safety net as
        // Version/Env/Linear). Dlopen / Other / Ok remain never-retry.
        case AotReloadFail::Region:
        case AotReloadFail::Staging:
            return ReloadPolicy{/*max_reemit=*/2, /*backoff_ms=*/15, /*fall_back_jit_only=*/true};
        case AotReloadFail::Dlopen:
        case AotReloadFail::Other:
        case AotReloadFail::Ok:
        default:
            return ReloadPolicy{/*max_reemit=*/0, /*backoff_ms=*/0, /*fall_back_jit_only=*/false};
    }
}

// Issue #2249: storm-skip helper. Under HotUpdateRegistry hard storm
// (StormLevel::Storm), suppress Region/Staging auto-retry so we don't
// retry into a storming region mask / staging handshake. Caller
// (aura_jit_bridge.cpp) consults this in the iterative retry loop
// and short-circuits to exhausted + JIT-only when true.
// C-linkage storm level (defined in hot_update_registry.cpp).
std::uint8_t aura_hot_update_current_storm_level(void);
// Note: aot_reload_storm_skip_retry_for_2249 is C++-only (inline below).
} // extern "C" temporarily closed for C++ inline helper
inline bool aot_reload_storm_skip_retry_for_2249(AotReloadFail r) noexcept {
    if (r != AotReloadFail::Region && r != AotReloadFail::Staging)
        return false; // only suppress the new auto-retry classes
    // Consult HotUpdateRegistry::current_storm_level() via C ABI.
    // Default 0 = None. Anything >= 2 = Storm/Global bit set.
    const std::uint64_t lvl = static_cast<std::uint64_t>(aura_hot_update_current_storm_level());
    return lvl >= 2;
}
extern "C" {

std::uint64_t aura_aot_metrics_lazy_init_total(void);
std::uint64_t aura_aot_metrics_explicit_sets_total(void);

// Issue #1485 C2: per-closure provenance accessors — emit-side freshness
// probe infrastructure (refine #1475). The JIT runtime side
// (aura_jit_runtime.cpp:880) already implements the dual-freshness probe
// inside the C aura_closure_call wrapper (with
// aura_jit_closure_record_stale_deopt + aura_deopt_inc + return 0 = deopt
// to interpreter). These extern accessors expose the underlying per-closure
// bridge_epoch / defuse_version vector reads so JIT emit-side LLVM IR can
// do an explicit CreateCall probe before fn_closure_call (deferred to
// follow-up — requires basic-block splitting, which is a non-trivial
// LLVM pattern change). For now the C-side aura_closure_call wrapper
// check is the authoritative JIT-side gate; LLVM IR emit-side probe
// fires when wired up.
//
// Issue #1706: bridge_epoch / defuse_version return 0 for out-of-range
// ids (legacy #1485), but 0 is also a valid stamp. Call
// aura_closure_exists(id) first to disambiguate (1 = slot allocated).
int aura_closure_exists(std::int64_t closure_id);
std::uint64_t aura_get_closure_bridge_epoch(std::int64_t closure_id);
std::uint64_t aura_get_closure_defuse_version(std::int64_t closure_id);
// Issue #1707: lifetime count of closure inline-cache generation mismatches.
std::uint64_t aura_closure_cache_generation_mismatch_total(void);


// aura_aot_probe_fn_version — read version for original_name from a
//   dlopened module (UINT64_MAX if unavailable).
// aura_aot_fn_version_is_stale — true when binary version != expected.
// aura_aot_parse_version_suffix / aura_aot_mangle_version_is_stale —
//   host-side helpers over mangled symbol names (no dlopen).
// Issue #2015: full suffix parse extracts optional `_eN_lN` (env/linear);
//   mangle stale / fn stale accept optional host env + linear for drift.
std::uint64_t aura_aot_probe_fn_version(void* dl_handle, const char* original_name);
bool aura_aot_fn_version_is_stale(void* dl_handle, const char* original_name,
                                  std::uint64_t expected);
// Extended: also compare aot_env_frame_version / aot_linear_state symbols
// (when present) against host expected env/linear. defuse-only path
// remains via expected_env=0, expected_linear=0 and missing symbols.
bool aura_aot_fn_version_is_stale_ex(void* dl_handle, const char* original_name,
                                     std::uint64_t expected_defuse,
                                     std::uint64_t expected_env_frame,
                                     std::uint8_t expected_linear);
bool aura_aot_parse_version_suffix(const char* mangled, std::uint64_t* out_version);
// Issue #2015: parse defuse + optional env_frame + linear. Null out_* ok.
bool aura_aot_parse_full_version_suffix(const char* mangled, std::uint64_t* out_defuse,
                                        std::uint64_t* out_env_frame, std::uint8_t* out_linear);
bool aura_aot_mangle_version_is_stale(const char* mangled, std::uint64_t expected);
bool aura_aot_mangle_version_is_stale_ex(const char* mangled, std::uint64_t expected_defuse,
                                         std::uint64_t expected_env_frame,
                                         std::uint8_t expected_linear);

// Issue #2091: live env_frame_version + linear_state_fingerprint
// mirrors that the Evaluator publishes into whenever env_generation_
// bumps (compact / truncate / rollback). emit / reemit /
// registration sites read these via aura_get_aot_live_* and stamp
// the `_eN_lN` suffix without importing the C++20 module.
void aura_set_aot_live_env_frame_version(std::uint64_t v);
// Issue #2272: per-closure env_generation stamp accessors (C ABI for
// bridge + runtime). Stamped at alloc from aura_get_aot_live_env_frame
// _version() (host mirror). Restamped on reemit restamp. Read by
// aura_remount_closure_captures as the PRIMARY env axis (legacy defuse
// is secondary). cid<0 / OOB returns 0 (legacy convention).
void aura_closure_set_env_gen(std::int64_t closure_id, std::uint64_t gen);
std::uint64_t aura_closure_get_env_gen(std::int64_t closure_id);
std::uint64_t aura_get_aot_live_env_frame_version(void);
void aura_set_aot_live_linear_state_fingerprint(std::uint8_t v);
std::uint8_t aura_get_aot_live_linear_state_fingerprint(void);

// Issue #2091: C-linkage bridge for the aot_mangle.h force flag.
void aura_aot_set_force_env_linear_suffix(int v);
int aura_aot_get_force_env_linear_suffix(void);

// Issue #1271: incremental re-emit skeleton + last commit epoch.
// Returns count of dirty functions re-emitted (0 in Phase 1 skeleton).
//
// Issue #1480 Phase 2: replaced the no-op skeleton with a real
// pipeline. The host registers a re-emit candidate callback via
// aura_set_reemit_candidate_fn() that pushes (name, region) pairs
// from ir_cache_v2_ + dep_graph_ cascade. The pipeline then:
//   1. iterates the pushed candidates
//   2. applies per-function region mask (g_aot_emit_region_mask)
//   3. runs the AOT path (stub in #1480; full LLVM emit is #1481)
//   4. on any successful re-emit: commit_func_table_swap() atomically
//      bumps g_aot_table_epoch so concurrent stale-frame probes see
//      consistent before/after
// Returns the count of dirty FlatFunctions actually re-emitted
// (after region filter); bumps the 4 #1480 metrics atomically.
std::uint64_t aura_reemit_aot_for_dirty(std::uint64_t current_defuse_version);
std::uint64_t aura_aot_last_commit_epoch(void);

// Issue #1480 Phase 2: host-side callback for the re-emit pipeline.
// Pushes one candidate (name, region) at a time. region is a
// per-function region bit index (0 = no region preference; non-zero
// bits index into g_aot_emit_region_mask). The callback returns
// true if it pushed a candidate, false when iteration is complete.
//
// userdata is the opaque pointer the host passed to the setter
// (typically the CompilerService* so the callback can walk
// ir_cache_v2_ + dep_graph_ in O(dirty + cascade_directed) time).
typedef bool (*aura_reemit_candidate_fn_t)(void* userdata, const char** out_name,
                                           std::uint64_t* out_region,
                                           bool* out_from_closure_capture);
void aura_set_reemit_candidate_fn(aura_reemit_candidate_fn_t fn, void* userdata);

// Issue #1952 / #1930: actual LLVM re-emit callback. The host
// (Evaluator / CompilerService) wires a function that takes the dirty
// FlatFunction name + region, looks up via ir_cache_v2_ or
// relower_define_function_minimal, calls emit_native_object_incremental
// (or emit_native_object for #1943 MVP), and returns true on success.
// On true, aura_reemit_aot_for_dirty bumps success metrics and
// get_or_preserve_stable_func_id for the name. Returns false to skip
// that candidate without advancing table epoch.
//
// userdata is the opaque pointer the host passed to the setter
// (typically the CompilerService*).
typedef bool (*aura_aot_emit_fn_t)(const char* name, std::uint64_t region, void* userdata);
void aura_set_aot_emit_fn(aura_aot_emit_fn_t fn, void* userdata);

// Issue #1930 / #2550: process-stable Define-name → func_id map (single
// workspace). get_or_preserve assigns on first sighting; subsequent
// re-emits / named set_name reuse id. Named aura_closure_set_name always
// calls get_or_preserve so stable_func_id != 0 before callable (#2550).
// out_preserved may be null; when non-null set to 1 if reused, 0 if assigned.
std::uint32_t aura_get_or_preserve_stable_func_id(const char* name, int* out_preserved);
std::uint32_t aura_lookup_stable_func_id(const char* name); // 0 if missing
std::uint64_t aura_stable_func_id_map_size(void);
void aura_clear_stable_func_id_map(void);
// Issue #2670: multi-eval namespace by (eval_owner, name). Explicit eval_ptr
// variants for dual-Evaluator hosts; legacy wrappers dispatch via reemit/
// register owner TLS (nullptr → process-default key).
std::uint32_t aura_get_or_preserve_stable_func_id_for_eval(void* eval_ptr, const char* name,
                                                           int* out_preserved);
std::uint32_t aura_lookup_stable_func_id_for_eval(void* eval_ptr, const char* name);
void aura_clear_stable_func_id_map_for_eval(void* eval_ptr);
// Issue #2692: force-bump mismatch counter (test / intentional inject).
void aura_bump_cross_eval_sid_owner_mismatch_total(void);

// Issue #2016: live (adapted) and preferred emit region masks.
// Evolution bit (1<<2) is always stripped from both.
std::uint64_t aura_get_aot_emit_region_mask(void);
std::uint64_t aura_get_aot_emit_region_mask_preferred(void);

// Last re-emit count (region-filtered candidates / would-reemit).
std::uint64_t aura_reemit_dirty_count(void);
// Last re-emit region-filtered skip count.
std::uint64_t aura_reemit_region_filtered_skips(void);
// Last re-emit closure-capture-dep count.
std::uint64_t aura_reemit_closure_dep_count(void);
// Last re-emit success count (emit callback true count; 0 if no emit fn).
std::uint64_t aura_reemit_success_count(void);
// Issue #2092: remapped live-closure count is in CompilerMetrics::live_closure_remap_total
// and HotUpdateRegistry snapshot (live_closure_remap_total). Name-fallback
// path bumps live_closure_remap_name_fallback_total via
// aura_bump_live_closure_remap_name_fallback_total (declared above).

// Issue #708 — region isolation + func_table refcount tracking.
// Global (default) APIs — equivalent to for_eval(nullptr, ...).
void aura_set_aot_region_mask(std::uint64_t mask);
std::uint64_t aura_get_aot_region_mask(void);
void aura_set_aot_emit_region_mask(std::uint64_t mask);

// Issue #1367: per-evaluator AOT state (multi-agent isolation).
// eval_ptr is typically Evaluator*; nullptr selects the process default state.
void aura_set_aot_region_mask_for_eval(void* eval_ptr, std::uint64_t mask);
std::uint64_t aura_get_aot_region_mask_for_eval(void* eval_ptr);
// Issue #2093: per-eval env_frame_version setter for the reload
// drift-detection path. Without this, tests + hosts that want to
// trigger an Env-failure reload can't seed the host side — the
// reload reads st.env_frame_version (per-eval state) directly, not
// the file-scope g_aot_live_env_frame_version.
void aura_set_aot_default_env_frame_version(std::uint64_t v);
void aura_set_aot_env_frame_version_for_eval(void* eval_ptr, std::uint64_t v);
void aura_set_module_version_for_eval(void* eval_ptr, std::uint64_t v);
std::uint64_t aura_get_module_version_for_eval(void* eval_ptr);
void aura_set_aot_defuse_version_for_eval(void* eval_ptr, std::uint64_t v);
std::uint64_t aura_get_aot_defuse_version_for_eval(void* eval_ptr);
bool aura_reload_aot_module_for_eval(void* eval_ptr, const char* path, std::uint64_t version);
// Drop per-eval AotState entry (call from ~Evaluator).
void aura_cleanup_aot_state(void* eval_ptr);
// Diagnostics: number of non-default AotState entries currently live.
std::uint64_t aura_aot_state_map_size(void);

void aura_register_fn_tracked(int64_t func_id, int64_t fn_ptr);
std::uint64_t aura_aot_func_table_epoch(void);
bool aura_aot_probe_checkpoint_version(std::uint64_t defuse_version, std::uint64_t bridge_epoch);
void aura_aot_record_deopt_on_steal(void);
std::uint64_t aura_aot_bridge_epoch_mismatches(void);

// Issue #1508 / #1491: JIT closure dual-freshness (bridge_epoch + env/defuse).
// Returns true when both domains are fresh vs current host epochs.
// Strict (default): unstamped capture (0) while domain tracking is active
// (current != 0) is STALE — matches is_bridge_stale / is_env_frame_stale.
// AURA_BRIDGE_EPOCH_LEGACY_TRUST=1 restores pre-#1491 "0 is ok" trust.
bool aura_is_jit_closure_fresh(std::uint64_t captured_bridge_epoch,
                               std::uint64_t captured_defuse_or_env_version);
// Bump dual-check / stale-deopt / safe-fallback metrics (nullable aot_metrics).
void aura_jit_closure_record_dual_check(void);
void aura_jit_closure_record_stale_deopt(void);
void aura_jit_closure_record_safe_fallback(void);
std::uint64_t aura_jit_closure_dual_check_total(void);
std::uint64_t aura_jit_closure_stale_deopt_total(void);
std::uint64_t aura_jit_closure_safe_fallbacks(void);
// Issue #2371 / #2505 / #2547: cross-COW call-time soft migrate vs hard
// safe-fallback.
//
// Scope (single-workspace MVP — NOT full COW heap migration / #2275 write path):
//   On aura_closure_call dual-freshness miss (and primary cow_gen check):
//     Soft: live slot + linear-safe + |epoch_delta| ≤ K + same cow_gen
//           (#2547) → restamp bridge+defuse+cow_gen (+ remount if captures)
//           and continue native. Default soft ON within gen.
//     Hard: freed, linear-moved / fingerprint drift, delta > K, remount fail,
//           soft disabled, or cow_gen mismatch (#2547) → safe-fallback.
//   K = AURA_CROSS_COW_SOFT_MIGRATE_MAX_DRIFT (default 4096; 0 = unlimited).
//   Soft off: AURA_CROSS_COW_SOFT_MIGRATE=0 → always hard on dual miss / gen.
//   Does NOT open cross-workspace hot-update write (#2178 / #2275 fail-closed).
//
// Hard-reject reason enum (cross_cow_last_hard_reject_reason / breakdown):
//   0=None 1=Disabled 2=Freed 3=FarBehind 4=Linear 5=RemountFail 6=Other
//   7=CowGenMismatch (#2547 — true workspace COW gen; wires #2240 on call)
void aura_bump_cross_cow_soft_migrate_total(void) noexcept;
// Issue #2603: same-gen soft-migrate success (distinct from cross-gen hard).
void aura_bump_cross_cow_soft_migrate_same_gen_total(void) noexcept;
void aura_bump_cross_cow_hard_reject_total(void) noexcept;
// Issue #2505: reason breakdown bumpers + policy knobs (Agent query).
void aura_bump_cross_cow_hard_reject_reason(std::uint8_t reason) noexcept;
[[nodiscard]] std::uint8_t aura_cross_cow_last_hard_reject_reason(void) noexcept;
[[nodiscard]] int aura_cross_cow_soft_migrate_enabled(void) noexcept;
[[nodiscard]] std::uint64_t aura_cross_cow_soft_migrate_max_drift(void) noexcept;
// Force-bump table epoch (test / hot-swap seam).
void aura_aot_bump_func_table_epoch(void);
// Issue #2713 / #2744 / #2841 observability (file-scope counters in
// aura_jit_bridge.cpp). #2841: production multi-eval cascade defaults to
// owner-scoped (no peer force-stale); hard invalidate notes force-bump.
std::uint64_t cross_eval_epoch_bump_total_v_read(void);
void* last_cross_eval_epoch_bump_owner_v_read(void);
std::uint32_t cross_eval_epoch_bump_wired_v_read(void);
// Issue #2744 / #2841: next aura_aot_bump_func_table_epoch() always advances
// the process-global table epoch (skip multi-eval cascade throttle).
// Hard invalidate_function / reload fall-back call this first.
void aura_aot_note_cross_eval_epoch_force_bump(void);
// Issue #2744 / #2841: multi-eval cascade bumps that were owner-scoped throttled.
std::uint64_t cross_eval_epoch_action_throttled_total_v_read(void);

// Issue #2304 / #2366: epoch invariant mode (process-level).
//   0 = off (production default; single relaxed load, zero walk cost)
//   1 = soft (walk + metric only)
//   2 = hard (walk + metric + abort on violation)
// Env AURA_EPOCH_INVARIANT=soft|1 → 1; =hard → 2; unset → 0.
void aura_set_epoch_invariant_mode(int mode);
int aura_epoch_invariant_mode(void);
// Backward-compat: enabled≠0 → mode 2 (hard).
void aura_set_epoch_invariant_hard_enabled(int enabled);
std::uint64_t aura_epoch_invariant_violation_total_v_read(void);
std::uint64_t aura_epoch_invariant_walks_total_v_read(void);
// Mirror service-side counters into C-readable totals (called after walk).
void aura_epoch_invariant_note_walk(std::uint64_t violations) noexcept;
// Issue #2501: additive breakdown counters (subset of violations).
void aura_epoch_invariant_note_slot_stale(std::uint64_t n) noexcept;
void aura_epoch_invariant_note_closure_must_deopt(std::uint64_t n) noexcept;
[[nodiscard]] std::uint64_t aura_epoch_invariant_slot_stale_total_v_read(void);
[[nodiscard]] std::uint64_t aura_epoch_invariant_closure_must_deopt_total_v_read(void);
// Count live generation-behind AOT slots (fn_ptr≠0 && gen≠current epoch).
// Empty slots (fn_ptr==0) are not violations.
[[nodiscard]] std::size_t aura_aot_count_live_generation_behind_slots(void);
// Issue #2501: walk JIT live-closure table; set MustDeopt on gen-behind
// (bridge_epoch != current table epoch, not already must_deopt, not freed).
// Returns number of closures newly marked MustDeopt.
[[nodiscard]] std::size_t aura_epoch_invariant_must_deopt_stale_live_closures(void);
// Test inject: live non-null slot with table_generation behind current epoch.
void aura_aot_inject_live_stale_slot_for_test(std::int64_t func_id);
void aura_aot_clear_slot_for_test(std::int64_t func_id);
// Issue #2501 test: stamp a live JIT closure bridge_epoch one behind current.
void aura_inject_stale_closure_bridge_epoch_for_test(std::int64_t closure_id);
// Issue #2271 / #2299: physically invalidate generation-behind AOT slots
// (close #2232 / #2271 follow-up). For each slot in g_aot_func_slots whose
// table_generation != aura_aot_func_table_epoch(), set fn_ptr empty
// (atomic_store 0) + reset table_generation to 0 + clear owner stamp.
// After this call:
//   - aura_aot_probe_fn_ptr(id) returns 0 for any stale id (safety
//     net + zero-native-hit, not just probe-reject).
//   - aot_reload_fall_back_slot_invalidate_total bumps by slot count.
//   - aot_reload_fall_back_slot_invalidate_calls_total bumps by 1.
// Issue #2299: eval_ptr filters ownership (multi-eval hosts):
//   - eval_ptr == nullptr → process-default: clear ALL generation-behind
//     slots (identical to #2271 single-workspace behavior).
//   - eval_ptr != nullptr → clear only generation-behind slots whose
//     owner stamp equals eval_ptr (foreign / unowned slots remain).
// Order preserved: fn_ptr release → generation release (AC3).
// Does NOT dlclose prior modules — refcount / handle lifetime stays
// #2012.
[[nodiscard]] std::size_t aura_aot_invalidate_all_stale_slots_for_eval(void* eval_ptr);

// Issue #2299: TLS stamp for aura_register_fn_tracked ownership.
// Hosts / tests set this before registration so multi-eval invalidate
// can filter by owner. nullptr = process-default (unowned) slots.
// Reloads via aura_reload_aot_module_for_eval install this automatically.
void aura_aot_set_register_owner_eval(void* eval_ptr);
void* aura_aot_get_register_owner_eval(void);
// Last eval_ptr passed to aura_aot_invalidate_all_stale_slots_for_eval
// (0 when never called / cleared). Agent dashboard observability.
std::uintptr_t aura_aot_last_slot_invalidate_eval(void);

// Issue #2606: TLS stamp for aura_reemit_aot_for_dirty ownership filter.
// When non-null, the reemit candidate loop skips names whose stable
// func_id maps to a live AOT slot owned by a *different* eval (mirrors
// aura_aot_invalidate_all_stale_slots_for_eval owner filter). nullptr
// = process-default: no cross-eval filter (soft single-eval MVP path
// identical to pre-#2606). Hosts should set this to the current
// Evaluator* around cascade / boundary reemit; reload_for_eval also
// falls back to the register-owner TLS when reemit-owner is unset.
// Invariant: joint bridge/AOT table epoch remains process-global —
// isolation is ownership + region mask + PerEval storm, not per-eval
// epoch domains.
void aura_aot_set_reemit_owner_eval(void* eval_ptr);
void* aura_aot_get_reemit_owner_eval(void);

// Issue #1522: register AuraJIT* so bridge can notify fn_trackers_ batch_deopt
// without a C++ module import. Host (CompilerService ctor) calls set;
// ~CompilerService calls clear so the file-scope g_batch_deopt_jit
// pointer is nulled before the AuraJIT object is destroyed
// (Issue #1996 / B-003 UAF fix — late batch_deopt_for /
// deopt_pending_count would otherwise dereference freed memory).
// clear matches the pointer before nulling (no clobber of a sibling
// CompilerService's live wire in the multi-service scenario); a
// null aura_jit_ptr argument is treated as a force-clear (host-
// bridge shutdown path).
void aura_set_jit_batch_deopt_target(void* aura_jit_ptr);
void aura_clear_jit_batch_deopt_target(void* aura_jit_ptr);
// Returns number of fn_trackers_ entries newly marked deopt_pending.
std::size_t aura_jit_batch_deopt_for(const char* name, std::uint64_t current_epoch);
std::uint64_t aura_jit_batch_deopt_for_total(void);
std::uint64_t aura_jit_batch_deopt_entries_marked(void);
std::uint64_t aura_jit_deopt_pending_count(void);
int aura_jit_is_deopt_pending(const char* name);

// Issue #1536: bulk walk_active_closures over captured fns.
// Returns number of stale fns found (marks deopt_pending on match).
std::size_t aura_jit_walk_active_closures(std::uint64_t current_bridge_epoch);
std::uint64_t aura_jit_walk_active_closures_total(void);
std::uint64_t aura_jit_walk_active_closures_stale_found(void);

// Issue #1537: LLVM IR-level Apply prologue dual-epoch helpers.
// Emitted at every JIT'd function entry (before body).
//   get_current_bridge_epoch — AOT table epoch (lockstep with bridge)
//   is_fn_epoch_stale — wraps AuraJIT::is_fn_epoch_stale; bumps
//     jit_epoch_stale_check_total once per Apply (AC4)
//   deopt_to_interpreter — stale path: record metrics + return sentinel 0
std::uint64_t aura_jit_get_current_bridge_epoch(void);
int aura_jit_is_fn_epoch_stale(const char* name, std::uint64_t current_bridge_epoch);
std::int64_t aura_jit_deopt_to_interpreter(const char* name);

// Issue #1534: GuardShape dual-epoch fence — runtime helper called from
// JIT-compiled OpGuardShape before narrow_evidence / shape fast-path.
// Returns 1 if the named fn is stale vs aura_aot_func_table_epoch()
// (lockstep with CompilerService::bridge_epoch via atomic_bump), else 0.
// Bumps AuraJIT::Metrics::jit_epoch_stale_check_total on every probe;
// on stale, also records dual-check stale deopt + compiler_live_closure
// stale-prevented (when aot_metrics is set). Host must have called
// aura_set_jit_batch_deopt_target so is_fn_epoch_stale can be reached.
int aura_jit_guard_shape_epoch_check(const char* name);

// Issue #739: acquire fence before GuardShape / epoch-sensitive JIT paths.
void aura_jit_epoch_acquire_fence(void);

// Issue #740: linear ownership safety probe in JIT L2 hot paths.
void aura_jit_linear_post_invalidate_safety(std::uint8_t linear_state, std::uint32_t opcode);

// Issue #1535: Linear* dual-epoch fence (Move/Borrow/Drop + safety_probe).
// Combines #1477 is_fn_epoch_stale (fn name vs AOT table epoch) with
// #1475 is_env_frame_stale logic (env context vs AOT defuse version).
// Issue #1540: also consults linear_post_mutate_enforce (via host callback)
// when env context is set — returns 1 on linear violation (deopt).
// Returns 1 if stale/unsafe (caller must deopt / skip mutation), 0 if safe.
// On every probe bumps jit_epoch_stale_check_total; on stale also bumps
// compiler_live_closure_stale_prevented_total + linear_post_mutate_enforcements.
// When linear_state != 0 also runs aura_jit_linear_post_invalidate_safety.
int aura_jit_linear_epoch_safety_check(const char* fn_name, std::uint8_t linear_state,
                                       std::uint32_t opcode);
// Host/test: set EnvFrame context for the is_env_frame_stale half of the
// dual check (env_id + frame_version captured when the linear value was
// created). Pass env_id == UINT32_MAX to clear / disable env half.
void aura_jit_set_linear_env_context(std::uint32_t env_id, std::uint64_t frame_version);
void aura_jit_clear_linear_env_context(void);

// Issue #1540: host wires Evaluator::linear_post_mutate_enforce.
// Callback returns 1 if UNSAFE (deopt), 0 if safe. user_data is typically
// Evaluator*. nullptr fn clears. Called from linear_safety_probe / Apply
// prologue when env context is active.
typedef int (*aura_linear_post_mutate_enforce_fn_t)(void* user_data, std::uint32_t env_id);
void aura_set_linear_post_mutate_enforce_fn(aura_linear_post_mutate_enforce_fn_t fn,
                                            void* user_data);
// Direct probe (tests + prologue). env_id UINT32_MAX → use g_linear_env_id.
// Returns 1 if unsafe (deopt), 0 if safe / no callback / no context.
// Always bumps jit_linear_post_mutate_enforcements_total when callback set.
int aura_jit_linear_post_mutate_enforce(std::uint32_t env_id);

// Issue #1545: host wires Evaluator::scan_live_closures_for_linear_captures.
// Called from AuraJIT::invalidate before ResourceTracker::remove (pre-evict).
typedef void (*aura_linear_live_closure_scan_fn_t)(void* user_data);
void aura_set_linear_live_closure_scan_fn(aura_linear_live_closure_scan_fn_t fn, void* user_data);
// Invoke host scan (no-op if unset). Returns 1 if callback ran, 0 otherwise.
int aura_jit_linear_live_closure_scan(void);

// Issue #358 — incremental re-AOT foundation.
//
// `aura_set_is_define_dirty_fn` registers a host-side callback
// that answers "is the Define named <name> dirty since the
// last AOT emit?". The userdata pointer is opaque to the
// bridge; it's threaded through to the callback so the host
// can pass a `this` pointer or a pointer to a closure / set
// of dirty names.
//
// `aura_filter_dirty_flat_functions` walks a FlatFunction[]
// array and returns the indices of functions whose `name`
// matches a dirty Define (per the registered callback). The
// caller (the future `aura_reemit_aot_for_dirty`) takes these
// indices and runs the AOT pipeline for just those functions.
typedef bool (*aura_is_define_dirty_fn_t)(void* userdata, const char* name);
void aura_set_is_define_dirty_fn(aura_is_define_dirty_fn_t fn, void* userdata);
int aura_filter_dirty_flat_functions(const void* functions, unsigned int num_functions,
                                     unsigned int* out_dirty_indices, unsigned int max_out);

// Issue #461: read-only accessor for the JIT fallback counter
// (defined in aura_jit_bridge.cpp). Exposed as C linkage so
// module GMF partitions can #include this header instead of
// bare extern "C" declarations.
std::uint64_t aura_jit_fallback_count_v_read(void);

// Issue #657: JIT unhandled-opcode → compiler invalidate/deopt hook.
typedef void (*aura_jit_unhandled_invalidate_fn_t)(const char* fn_name);
void aura_set_jit_unhandled_invalidate_fn(aura_jit_unhandled_invalidate_fn_t fn);
void aura_notify_jit_unhandled_opcode(const char* fn_name);

// Issue #1443 AC3 / #1445 AC6: long-mutation → scheduler priority hook.
// Host may register a callback; default null is telemetry-only.
typedef void (*aura_long_mutation_scheduler_hook_fn)(std::uint64_t fiber_id,
                                                     std::uint64_t duration_us);
void aura_set_long_mutation_scheduler_hook(aura_long_mutation_scheduler_hook_fn fn);
void aura_invoke_long_mutation_scheduler_hook(std::uint64_t fiber_id, std::uint64_t duration_us);
std::uint64_t aura_long_mutation_scheduler_hook_calls_total(void);

// Issue #1443 / #2199: long-mutation hold policy knobs.
void aura_set_long_mutation_threshold_us(std::uint64_t us);
std::uint64_t aura_get_long_mutation_threshold_us(void);
void aura_set_long_mutation_strict_mode(int on);
std::uint64_t aura_get_long_mutation_strict_mode(void);
void aura_set_max_extreme_mutation_us(std::uint64_t us);
// Issue #2199: hard_timeout_us (0 = use max_extreme) + forced-abort total.
void aura_set_hard_timeout_us(std::uint64_t us);
std::uint64_t aura_get_hard_timeout_us(void);
std::uint64_t aura_get_long_mutation_forced_abort_total(void);

// Issue #2640: production Restricted default periodic epoch-invariant soft walk
// (physically clear generation-behind AOT slots + MustDeopt stale live
// closures on a steady-clock interval under production Soft mode).
// Counters + setters + main hook.
[[nodiscard]] std::uint64_t aura_epoch_invariant_periodic_walks_total_v_read(void);
[[nodiscard]] std::uint64_t aura_epoch_invariant_periodic_last_walk_at_ms_v_read(void);
[[nodiscard]] std::uint64_t aura_epoch_invariant_periodic_skipped_off_total_v_read(void);
[[nodiscard]] std::uint64_t aura_epoch_invariant_periodic_skipped_wrong_mode_total_v_read(void);
[[nodiscard]] std::uint64_t aura_epoch_invariant_periodic_skipped_rate_limited_total_v_read(void);
[[nodiscard]] std::uint64_t aura_epoch_invariant_periodic_skipped_disabled_total_v_read(void);
[[nodiscard]] std::uint64_t aura_epoch_invariant_periodic_period_ms_v_read(void);
void aura_set_epoch_invariant_periodic_period_ms(std::uint64_t ms);
// Main hook — gated by mode=Soft + production_defaults_active +
// steady_ms_now rate limit. Cheap on the quiet path; runs the existing
// #2541 soft walk on the active path. Called from
// MutationBoundaryGuard::~MutationBoundaryGuard outermost success exit.
void aura_periodic_epoch_invariant_walk_if_due(void);
// Issue #2668: event-driven soft walk on epoch-bump / reemit edge.
// Declared here so commit_func_table_swap / aura_aot_bump_func_table_epoch
// call sites (earlier in aura_jit_bridge.cpp) compile under -Werror.
void aura_event_driven_epoch_invariant_walk_if_due(void);
// Issue #2668 observability counters (query hash in obs_eval).
[[nodiscard]] std::uint64_t aura_epoch_invariant_event_walks_total_v_read(void);
[[nodiscard]] std::uint64_t aura_epoch_invariant_event_skipped_off_total_v_read(void);
[[nodiscard]] std::uint64_t aura_epoch_invariant_event_skipped_wrong_mode_total_v_read(void);
// Issue #2693: Soft epoch-invariant consecutive-dirty fuse
// (refine #2640 / #2668 — bumps epoch_invariant_soft_fuse_total
// after K consecutive Soft walks that all left behind slots uncleared).
// K defaults to 3 (env AURA_EPOCH_INVARIANT_SOFT_FUSE_K; 0 disables).
// Soft zero-cost when consecutive_dirty stays 0. File-level
// fallback counters live in aura_jit_bridge.cpp; light binaries
// without the production TU get the weak no-op stubs.
[[nodiscard]] std::uint64_t aura_epoch_invariant_soft_fuse_total_v_read(void);
[[nodiscard]] std::uint64_t aura_epoch_invariant_consecutive_dirty_total_v_read(void);
[[nodiscard]] int aura_epoch_invariant_soft_fuse_k_default(void);
extern "C" void aura_set_epoch_invariant_soft_fuse_k(int k);
extern "C" int aura_get_epoch_invariant_soft_fuse_k(void);

} // extern "C"

#endif // AURA_COMPILER_AURA_JIT_BRIDGE_H