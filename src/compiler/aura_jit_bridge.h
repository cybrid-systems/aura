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

// Issue #1271: incremental re-emit skeleton + last commit epoch.
// Returns count of dirty functions re-emitted (0 in Phase 1 skeleton).
std::uint64_t aura_reemit_aot_for_dirty(std::uint64_t current_defuse_version);
std::uint64_t aura_aot_last_commit_epoch(void);

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

// Issue #739: acquire fence before GuardShape / epoch-sensitive JIT paths.
void aura_jit_epoch_acquire_fence(void);

// Issue #740: linear ownership safety probe in JIT L2 hot paths.
void aura_jit_linear_post_invalidate_safety(std::uint8_t linear_state, std::uint32_t opcode);

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

} // extern "C"

#endif // AURA_COMPILER_AURA_JIT_BRIDGE_H