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

// Issue #2012: probe live func_table slot (0 if empty / out of range).
std::uintptr_t aura_aot_probe_fn_ptr(std::int64_t func_id);

// Issue #1368: AOT metrics pointer lifecycle
//   aura_set_aot_metrics — explicit host wire-up (overwrites)
//   aura_ensure_aot_metrics — lazy bind only if currently null
//   aura_get_aot_metrics — current pointer (may be null)
// aura_set_aot_metrics is declared in runtime_shared.h (CompilerMetrics*).
void aura_ensure_aot_metrics(void* metrics);
void* aura_get_aot_metrics(void);
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

// Issue #1930: process-stable Define-name → func_id map (single workspace).
// get_or_preserve assigns on first sighting; subsequent re-emits reuse id.
// out_preserved may be null; when non-null set to 1 if reused, 0 if assigned.
std::uint32_t aura_get_or_preserve_stable_func_id(const char* name, int* out_preserved);
std::uint32_t aura_lookup_stable_func_id(const char* name); // 0 if missing
std::uint64_t aura_stable_func_id_map_size(void);
void aura_clear_stable_func_id_map(void);

// Last re-emit count (region-filtered candidates / would-reemit).
std::uint64_t aura_reemit_dirty_count(void);
// Last re-emit region-filtered skip count.
std::uint64_t aura_reemit_region_filtered_skips(void);
// Last re-emit closure-capture-dep count.
std::uint64_t aura_reemit_closure_dep_count(void);
// Last re-emit success count (emit callback true count; 0 if no emit fn).
std::uint64_t aura_reemit_success_count(void);

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
// Force-bump table epoch (test / hot-swap seam).
void aura_aot_bump_func_table_epoch(void);

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

} // extern "C"

#endif // AURA_COMPILER_AURA_JIT_BRIDGE_H