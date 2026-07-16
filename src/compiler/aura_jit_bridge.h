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
//     (Issue #287 — host-facing hot-reload entry point;
//      scaffold: dlopen + version check + constructor
//      invocation. The version-keyed func_table swap and
//      multi-agent isolation are tracked as follow-ups to
//      #287.)
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

// #287 — user-facing module version (hot-reload / multi-agent).
void aura_set_module_version(std::uint64_t v);
std::uint64_t aura_get_module_version(void);

// #287 — AOT hot-reload scaffold.
//   path    - path to the new .so/.dylib
//   version - expected aot_emit_version; 0 = trust binary's own
//
// Returns true on a successful load (dlopen OK + version check
// passed). On failure, logs to stderr and returns false.
bool aura_reload_aot_module(const char* path, std::uint64_t version);

// Issue #1368: AOT metrics pointer lifecycle
//   aura_set_aot_metrics — explicit host wire-up (overwrites)
//   aura_ensure_aot_metrics — lazy bind only if currently null
//   aura_get_aot_metrics — current pointer (may be null)
void aura_ensure_aot_metrics(void* metrics);
void* aura_get_aot_metrics(void);
std::uint64_t aura_aot_metrics_lazy_init_total(void);
std::uint64_t aura_aot_metrics_explicit_sets_total(void);

// Issue #1369: per-function AOT version probe (hot-reload stale detection).
// aura_aot_probe_fn_version — read version for original_name from a
//   dlopened module (UINT64_MAX if unavailable).
// aura_aot_fn_version_is_stale — true when binary version != expected.
// aura_aot_parse_version_suffix / aura_aot_mangle_version_is_stale —
//   host-side helpers over mangled symbol names (no dlopen).
std::uint64_t aura_aot_probe_fn_version(void* dl_handle, const char* original_name);
bool aura_aot_fn_version_is_stale(void* dl_handle, const char* original_name,
                                  std::uint64_t expected);
bool aura_aot_parse_version_suffix(const char* mangled, std::uint64_t* out_version);
bool aura_aot_mangle_version_is_stale(const char* mangled, std::uint64_t expected);

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
// Last re-emit count (for tests + EDSL observability).
std::uint64_t aura_reemit_dirty_count(void);
// Last re-emit region-filtered skip count.
std::uint64_t aura_reemit_region_filtered_skips(void);
// Last re-emit closure-capture-dep count.
std::uint64_t aura_reemit_closure_dep_count(void);

// Issue #708 — region isolation + func_table refcount tracking.
// Global (default) APIs — equivalent to for_eval(nullptr, ...).
void aura_set_aot_region_mask(std::uint64_t mask);
std::uint64_t aura_get_aot_region_mask(void);
void aura_set_aot_emit_region_mask(std::uint64_t mask);

// Issue #1367: per-evaluator AOT state (multi-agent isolation).
// eval_ptr is typically Evaluator*; nullptr selects the process default state.
void aura_set_aot_region_mask_for_eval(void* eval_ptr, std::uint64_t mask);
std::uint64_t aura_get_aot_region_mask_for_eval(void* eval_ptr);
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

// Issue #1508: JIT closure dual-freshness (bridge_epoch + env/defuse).
// Returns true when both domains match current host epochs.
// captured_bridge==0 or captured_defuse==0 means "unset / legacy" for
// that domain (treated as fresh for that side only).
bool aura_is_jit_closure_fresh(std::uint64_t captured_bridge_epoch,
                               std::uint64_t captured_defuse_or_env_version);
// Bump dual-check / stale-deopt / safe-fallback metrics (nullable aot_metrics).
void aura_jit_closure_record_dual_check(void);
void aura_jit_closure_record_stale_deopt(void);
void aura_jit_closure_record_safe_fallback(void);
std::uint64_t aura_jit_closure_dual_check_total(void);
std::uint64_t aura_jit_closure_stale_deopt_total(void);
std::uint64_t aura_jit_closure_safe_fallbacks(void);
// Force-bump table epoch (test / hot-swap seam).
void aura_aot_bump_func_table_epoch(void);

// Issue #1522: register AuraJIT* so bridge can notify fn_trackers_ batch_deopt
// without a C++ module import. Host (CompilerService ctor) calls set;
// nullptr clears. batch_deopt_for matches name + name#* keys.
void aura_set_jit_batch_deopt_target(void* aura_jit_ptr);
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

} // extern "C"

#endif // AURA_COMPILER_AURA_JIT_BRIDGE_H