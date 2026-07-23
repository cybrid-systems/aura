// aura_jit_bridge.cpp — C-linkage bridge for AOT native compilation
// Routes compilation requests to the LLVM-based emit backend in aura_jit.cpp.
//
// Hot-Update MVP scope (Issue #1943): the single-define re-emit path
// triggered by (compile:relower-strategy) on a single-workspace function
// is **in scope**. Issue #1930 / #1952 close the incremental re-emit
// pipeline: host-wired LLVM emit callback + process-stable name→func_id
// map across mutation epochs (see docs/hot-update.md).

#include "aura_jit.h"
#include "aura_jit_bridge.h"
#include "aot_mangle.h"            // mangle_aot_name (Issue #136)
#include "hot_update_registry.hh"  // Issue #1956: unified hot-update coordination
#include "observability_metrics.h" // Issue #452: CompilerMetrics for AOT counter hooks
#include "typed_mutation_audit.h"  // Issue #1882: TypedMutationAudit on hot-update

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <format>
#include <fstream>
#include <limits>
#include <print>
#include <unistd.h>                        // Issue #237 v4: readlink for /proc/self/exe lookup
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>

// Defined in aura_jit_runtime.cpp (lock-hooks path for defuse version).
extern "C" std::uint64_t aura_get_defuse_version(void);
// Defined in aura_jit_runtime.cpp (workspace deopt counter).
extern "C" void aura_deopt_inc(void);

// Helper: convert aura::ir::IRFunction to aura::jit::FlatFunction
// This bridges between the compiler's IR types and the JIT's FlatFunction.
// Caller must keep flat_instrs/name_storage alive for the returned FlatFunction.

import std;
struct FlatFunctionHolder {
    std::vector<std::vector<aura::jit::FlatInstruction>> flat_instrs;
    std::vector<aura::jit::FlatBlock> flat_blocks;
    std::string name_storage;
    aura::jit::FlatFunction flat_fn;
};

// This collection is passed to the bridge function as user data.
// We define it inline since the compiler's IR types are opaque to this bridge.

// Since we don't have visibility into aura::ir::IRFunction from this TU,
// the AOT bridge receives an already-converted FlatFunction array.
// For the C-linkage bridge, we accept a FlatFunction array directly.

// ── Global: primitive registration C code ───────────────────────
// Set by aura_set_prim_registration() before aura_emit_native_file().
// This C code is compiled and linked into the AOT binary to enable
// primitive dispatch for OpPrimitive + OpCall closures.
static std::string g_prim_reg_c_code;

// ── Global: string pool for OpConstString ───────────────────────
// Set by aura_set_string_pool() before aura_emit_native_file().
static std::vector<std::string> g_string_pool;

// ── Global: current defuse_version at emit time ────────────────
// Issue #243: the AOT bridge now records the defuse_version_
// of the Evaluator that triggered the AOT emission. This value
// flows into mangle_aot_name (so the .o file's symbols carry
// the version) and into the emitted registration .c (so the
// registration table records which version it belongs to).
// Set by aura_set_aot_defuse_version() before
// aura_emit_native_file(); defaults to 0 (the "unversioned"
// baseline that pre-#243 callers expect).
static std::uint64_t g_aot_defuse_version = 0;

// C-linkage setter for g_aot_defuse_version. Called from
// aura_jit.cpp's emit_native_object_llvm (or wherever the
// Evaluator's current defuse_version_ is known). Default 0
// preserves the pre-#243 behavior.
extern "C" void aura_set_aot_defuse_version(std::uint64_t v) {
    g_aot_defuse_version = v;
}

// C-linkage getter for diagnostics / tests.
extern "C" std::uint64_t aura_get_aot_defuse_version(void) {
    return g_aot_defuse_version;
}

// ── Issue #1485 C2-wire: current bridge_epoch tracker for the
// aura_closure_call 2-check (refine #1475). See aura_jit_bridge.h.
// Set by service.ixx::bump_bridge_epoch() every time the
// workspace's bridge_epoch advances; stamped into per-closure
// AuraClosure::bridge_epoch at aura_alloc_closure time. Mismatch
// at aura_closure_call → return 0 (caller falls back via
// aura_jit.cpp OpApply emit's deopt-to-interpreter path).
// Issue #1654: std::atomic<std::uint64_t> replaces the plain uint64_t to close
// the C++ memory model data race. The previous plain uint64_t write/read pair
// was UB per the C++ memory model — concurrent writes from
// service.ixx::bump_bridge_epoch() and reads from lib/runtime.c's
// aura_closure_call 2-check produced torn reads on weakly-ordered
// architectures (ARM, POWER). The std::atomic release/acquire protocol
// mirrors the #1476 dual-epoch primitive (already used elsewhere in
// service.ixx) and gives the JIT-side 2-check the authoritative safety
// gate it was supposed to have (#1485 C2-wire fix-up intent).
static std::atomic<std::uint64_t> g_current_bridge_epoch{0};

extern "C" void aura_set_current_bridge_epoch(std::uint64_t v) {
    g_current_bridge_epoch.store(v, std::memory_order_release);
}

extern "C" std::uint64_t aura_get_current_bridge_epoch(void) {
    return g_current_bridge_epoch.load(std::memory_order_acquire);
}

// ── Issue #452: AOT hot-update counters (observable) ───────────
//
// Three atomics bumped by aura_reload_aot_module on each
// version/region check outcome. Exposed via
// (query:aot-stats) primitive as a 3-field hash:
//   aot_stale_reject_count, aot_region_mismatch,
//   aot_hot_update_success_count.
//
// The host sets g_aot_metrics at startup (NULL default → counters
// no-op). Issue #1368: also auto-wired from Evaluator::set_compiler_metrics
// and aura_ensure_aot_metrics (lazy) so bare Evaluator usage is not silent.
static aura::compiler::CompilerMetrics* g_aot_metrics = nullptr;
static std::atomic<std::uint64_t> g_aot_metrics_lazy_init_total{0};
static std::atomic<std::uint64_t> g_aot_metrics_explicit_sets{0};

// Issue #1368: single access helper used by all counter sites.
static inline aura::compiler::CompilerMetrics* aot_metrics() noexcept {
    return g_aot_metrics;
}

extern "C" void aura_set_aot_metrics(aura::compiler::CompilerMetrics* m) {
    g_aot_metrics = m;
    if (m)
        g_aot_metrics_explicit_sets.fetch_add(1, std::memory_order_relaxed);
}

// ── Issue #1443: long-mutation policy knobs ───────────
//
// C-linkage setters for `long_mutation_threshold_us` (default 500'000 µs
// = 500ms) and `long_mutation_strict_mode` (0 = metric-only,
// 1 = abort/rollback on extreme holds >= max_extreme_mutation_us).
// Read in MutationBoundaryGuard::~MutationBoundaryGuard (evaluator.ixx)
// via std::atomic load — racy by design (best-effort policy).
extern "C" void aura_set_long_mutation_threshold_us(std::uint64_t us) {
    if (g_aot_metrics)
        g_aot_metrics->long_mutation_threshold_us.store(us, std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_get_long_mutation_threshold_us(void) {
    if (g_aot_metrics)
        return g_aot_metrics->long_mutation_threshold_us.load(std::memory_order_relaxed);
    return 500'000;
}

extern "C" void aura_set_long_mutation_strict_mode(int on) {
    if (g_aot_metrics)
        g_aot_metrics->long_mutation_strict_mode.store(on ? 1u : 0u, std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_get_long_mutation_strict_mode(void) {
    if (g_aot_metrics)
        return g_aot_metrics->long_mutation_strict_mode.load(std::memory_order_relaxed);
    return 0;
}

extern "C" void aura_set_max_extreme_mutation_us(std::uint64_t us) {
    if (g_aot_metrics)
        g_aot_metrics->max_extreme_mutation_us.store(us, std::memory_order_relaxed);
}

// ── Issue #1443 AC3 follow-up + #1445 AC6: long-mutation scheduler hook ──
//
// When the outermost MutationBoundaryGuard dtor fires with hold duration
// > long_mutation_threshold_us, the evaluator calls this hook (if set) so
// the scheduler can react: bump starvation_mitigated_count, boost waiter
// priorities, etc. Default = nullptr (no notification; existing telemetry
// only). Set via aura_set_long_mutation_scheduler_hook().
typedef void (*aura_long_mutation_scheduler_hook_fn)(std::uint64_t fiber_id,
                                                     std::uint64_t duration_us);
static aura_long_mutation_scheduler_hook_fn g_long_mutation_scheduler_hook = nullptr;
static std::atomic<std::uint64_t> g_long_mutation_scheduler_hook_calls{0};

extern "C" void aura_set_long_mutation_scheduler_hook(aura_long_mutation_scheduler_hook_fn fn) {
    g_long_mutation_scheduler_hook = fn;
}

extern "C" void aura_invoke_long_mutation_scheduler_hook(std::uint64_t fiber_id,
                                                         std::uint64_t duration_us) {
    g_long_mutation_scheduler_hook_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_long_mutation_scheduler_hook)
        g_long_mutation_scheduler_hook(fiber_id, duration_us);
}

extern "C" std::uint64_t aura_long_mutation_scheduler_hook_calls_total(void) {
    return g_long_mutation_scheduler_hook_calls.load(std::memory_order_relaxed);
}

// Lazy: only binds when global is still null (does not overwrite host wire-up).
extern "C" void aura_ensure_aot_metrics(void* metrics) {
    if (!metrics)
        return;
    if (g_aot_metrics == nullptr) {
        g_aot_metrics = static_cast<aura::compiler::CompilerMetrics*>(metrics);
        g_aot_metrics_lazy_init_total.fetch_add(1, std::memory_order_relaxed);
    }
}

extern "C" void* aura_get_aot_metrics(void) {
    return g_aot_metrics;
}

extern "C" std::uint64_t aura_aot_metrics_lazy_init_total(void) {
    return g_aot_metrics_lazy_init_total.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_aot_metrics_explicit_sets_total(void) {
    return g_aot_metrics_explicit_sets.load(std::memory_order_relaxed);
}

// ── Issue #287: AOT module version (hot-reload / multi-agent) ───────────
//
// Distinct from `g_aot_defuse_version` (the runtime mutation epoch
// at emit time, used for staleness detection):
//   - `g_aot_defuse_version` = internal, bumps on every mutation
//   - `g_aot_module_version` = user-facing, set by host code
//     before emit to identify a specific AOT module build
//     (e.g. "model-2026-06-23-build-42")
//
// In hot-reload or multi-agent orchestration, the host loads
// a new AOT binary with a different `module_version` so the
// runtime can distinguish:
//   1. Two builds of the same source (same defuse_version, different
//      module_version) — safe to swap, no stale-frame issue
//   2. A build from a pre-mutation epoch (lower defuse_version) —
//      stale, must not be loaded into a mutated runtime
//
// Issue #708 / #1367: per-agent AOT isolation state.
// Previously region_mask + module_version were process-global, so multi-agent
// sharing one process could not isolate AOT modules. Now each Evaluator* (or
// the nullptr default key) has its own AotState; global C APIs use nullptr.
struct AotState {
    std::atomic<std::uint64_t> region_mask{0};
    std::atomic<std::uint64_t> module_version{0};
    // 0 = fall back to process g_aot_defuse_version for reload stale checks
    std::atomic<std::uint64_t> defuse_version{0};
    // Issue #1640: host env_frame_version for AOT captured-env drift.
    // 0 = no stamp / skip drift detection on reload.
    std::atomic<std::uint64_t> env_frame_version{0};
};

static std::mutex g_aot_state_mtx;
static std::unordered_map<void*, std::unique_ptr<AotState>> g_aot_state_map;
static AotState g_aot_default_state; // key = nullptr (backward-compat global)

static AotState& aot_state_for(void* eval_ptr) {
    if (!eval_ptr)
        return g_aot_default_state;
    std::lock_guard<std::mutex> lock(g_aot_state_mtx);
    auto& slot = g_aot_state_map[eval_ptr];
    if (!slot) {
        slot = std::make_unique<AotState>();
        if (aot_metrics())
            aot_metrics()->aot_per_eval_state_creates.fetch_add(1, std::memory_order_relaxed);
    }
    return *slot;
}

// Issue #708: emit-time region mask (process-wide; AOT emit is not multi-tenant).
static std::uint64_t g_aot_emit_region_mask = 0;
// Issue #1640: emit-time env_frame_version stamp (process-wide; AOT emit).
// 0 = no stamp (skip drift detection). Host can raise via setter when wired.
static std::uint64_t g_aot_emit_env_frame_version = 0;

// Backward-compat aliases for emit / log sites that still read "module version"
// of the default state.
static std::uint64_t g_aot_module_version_default() {
    return g_aot_default_state.module_version.load(std::memory_order_relaxed);
}

extern "C" void aura_set_module_version(std::uint64_t v) {
    g_aot_default_state.module_version.store(v, std::memory_order_relaxed);
}

extern "C" void aura_set_module_version_for_eval(void* eval_ptr, std::uint64_t v) {
    aot_state_for(eval_ptr).module_version.store(v, std::memory_order_relaxed);
}

extern "C" void aura_set_aot_region_mask(std::uint64_t mask) {
    g_aot_default_state.region_mask.store(mask, std::memory_order_relaxed);
}

extern "C" void aura_set_aot_region_mask_for_eval(void* eval_ptr, std::uint64_t mask) {
    aot_state_for(eval_ptr).region_mask.store(mask, std::memory_order_relaxed);
    if (aot_metrics())
        aot_metrics()->aot_per_eval_region_sets.fetch_add(1, std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_get_aot_region_mask(void) {
    return g_aot_default_state.region_mask.load(std::memory_order_acquire);
}

extern "C" std::uint64_t aura_get_aot_region_mask_for_eval(void* eval_ptr) {
    return aot_state_for(eval_ptr).region_mask.load(std::memory_order_acquire);
}

extern "C" void aura_set_aot_emit_region_mask(std::uint64_t mask) {
    g_aot_emit_region_mask = mask;
    // Issue #1956: bookkeeping for HotUpdateRegistry dashboard.
    aura::compiler::hot_update_registry().on_emit_region_mask_set(mask);
}

extern "C" std::uint64_t aura_get_module_version(void) {
    return g_aot_module_version_default();
}

extern "C" std::uint64_t aura_get_module_version_for_eval(void* eval_ptr) {
    return aot_state_for(eval_ptr).module_version.load(std::memory_order_acquire);
}

extern "C" void aura_set_aot_defuse_version_for_eval(void* eval_ptr, std::uint64_t v) {
    aot_state_for(eval_ptr).defuse_version.store(v, std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_get_aot_defuse_version_for_eval(void* eval_ptr) {
    const auto d = aot_state_for(eval_ptr).defuse_version.load(std::memory_order_acquire);
    if (d != 0)
        return d;
    return g_aot_defuse_version;
}

extern "C" void aura_cleanup_aot_state(void* eval_ptr) {
    if (!eval_ptr)
        return;
    std::lock_guard<std::mutex> lock(g_aot_state_mtx);
    if (g_aot_state_map.erase(eval_ptr) > 0 && aot_metrics())
        aot_metrics()->aot_per_eval_state_clears.fetch_add(1, std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_aot_state_map_size(void) {
    std::lock_guard<std::mutex> lock(g_aot_state_mtx);
    return static_cast<std::uint64_t>(g_aot_state_map.size());
}

// ── Issue #358: incremental re-AOT foundation ───────────────────
//
// Foundation for incremental re-AOT (re-compile only dirty
// functions instead of re-emitting the whole module). Step 3
// from the issue body — the actual `aura_reemit_aot_for_dirty`
// pipeline that drives the LLVM AOT path for just the dirty
// functions — is deferred to a follow-up issue (it requires
// a stable `DefineId → FlatFunction index` table that lives
// across mutation epochs, which is its own body of work).
//
// What ships in this scope-limited close:
//   1. `aura_set_is_define_dirty_fn` — host (Evaluator)
//      registers a callback that answers "is the Define named
//      <name> dirty since the last AOT emit?". This is the
//      same function pointer pattern as the in-module
//      `is_define_dirty_fn_` from #196/#240 but exposed as a
//      C-linkage symbol so the AOT bridge (which lives in a
//      separate compilation unit from the Evaluator) can
//      consume it without a circular module dependency.
//   2. `aura_filter_dirty_flat_functions` — walks a
//      FlatFunction[] array and returns the indices of
//      functions whose `name` matches a dirty Define. This
//      is the data plumbing for incremental re-emit: the
//      caller (the future `aura_reemit_aot_for_dirty`) takes
//      these indices and runs the AOT pipeline for just
//      those functions. The function name is the canonical
//      mapping (Define name == function name == FlatFunction
//      name) — no separate DefineId table needed for the
//      name-based filtering path.
//
// Out of scope (follow-up issue):
//   - Stable DefineId → FlatFunction index table that
//     survives mutation epochs (the issue's step 1)
//   - `aura_reemit_aot_for_dirty(version)` that drives the
//     LLVM AOT path (the issue's step 3)
//   - Hot-patch test (define + AOT + mutate + re-emit +
//     verify) — requires the AOT path to be callable from
//     a test, which needs the above two pieces.

// Global: host-provided callback that answers "is Define <name>
// dirty?". Set by aura_set_is_define_dirty_fn(). When null,
// aura_filter_dirty_flat_functions returns -1 (the host has
// not wired the dirty-tracking into the AOT bridge yet).
// userdata is opaque to the bridge; it's threaded through to
// the callback so the host can pass a `this` pointer or a
// pointer to a closure / std::set of dirty names.
typedef bool (*aura_is_define_dirty_fn_t)(void* userdata, const char* name);
static aura_is_define_dirty_fn_t g_is_define_dirty_fn = nullptr;
static void* g_is_define_dirty_userdata = nullptr;

extern "C" void aura_set_is_define_dirty_fn(aura_is_define_dirty_fn_t fn, void* userdata) {
    g_is_define_dirty_fn = fn;
    g_is_define_dirty_userdata = userdata;
    aura::compiler::hot_update_registry().on_define_dirty_provider_set(fn != nullptr);
}

// Issue #1480 Phase 2: host-side re-emit candidate iterator.
// Set by aura_set_reemit_candidate_fn(). When null, aura_reemit_aot_for_dirty
// falls back to the Phase 1 skeleton path (return 0 + bump
// aot_reemit_dirty_skeleton_calls).
//
// The callback is push-based: each call returns one candidate
// (name, region, from_closure_capture) — the bridge iterates until
// the callback returns false. The host (Evaluator) sources candidates
// from ir_cache_v2_ entries where entry.dirty == true plus the
// dep_graph_ cascade closure-capture dependents (closure_captured_by
// reverse edges), so the re-emit pipeline gets the FULL transitive
// dirty set in a single aura_reemit_aot_for_dirty() call.
static aura_reemit_candidate_fn_t g_reemit_candidate_fn = nullptr;
static void* g_reemit_candidate_userdata = nullptr;

// Issue #1952 / #1930: actual LLVM re-emit callback (replaces #1481 stub).
// When null, aura_reemit_aot_for_dirty falls back to Phase 1 skeleton
// (count + commit_func_table_swap gate). When wired, each region-filtered
// candidate invokes the callback; true → success metrics + stable id map.
static aura_aot_emit_fn_t g_aot_emit_fn = nullptr;
static void* g_aot_emit_userdata = nullptr;

// Issue #1930: process-stable Define-name → func_id map. Survives
// mutation epochs so re-emit of the same function keeps a stable id
// for old closures. Name is the canonical key (Define name == FlatFunction
// name). Full workspace-migrated DefineId identity remains out-of-scope
// for cross-workspace COW (#1943 deferred); this map is the single-workspace
// zero-downtime contract.
static std::mutex g_stable_func_id_mtx;
static std::unordered_map<std::string, std::uint32_t, aura::core::TransparentStringHash,
                          std::equal_to<>>
    g_name_to_stable_func_id;
static std::atomic<std::uint32_t> g_next_stable_func_id{1};

// Returns stable func_id for name. out_preserved: 1 if map already held
// an entry (re-emit reuse), 0 if newly assigned.
static std::uint32_t preserve_stable_func_id_locked(const char* name, int* out_preserved) {
    if (out_preserved)
        *out_preserved = 0;
    if (!name || !*name)
        return 0;
    std::string key(name);
    auto it = g_name_to_stable_func_id.find(key);
    if (it != g_name_to_stable_func_id.end()) {
        if (out_preserved)
            *out_preserved = 1;
        return it->second;
    }
    const std::uint32_t id = g_next_stable_func_id.fetch_add(1, std::memory_order_relaxed);
    g_name_to_stable_func_id.emplace(std::move(key), id);
    return id;
}

// Last-call stats for tests + EDSL observability primitives.
static std::atomic<std::uint64_t> g_last_reemit_dirty_count{0};
static std::atomic<std::uint64_t> g_last_reemit_region_skips{0};
static std::atomic<std::uint64_t> g_last_reemit_closure_dep_count{0};
static std::atomic<std::uint64_t> g_last_reemit_success_count{0};

extern "C" void aura_set_reemit_candidate_fn(aura_reemit_candidate_fn_t fn, void* userdata) {
    g_reemit_candidate_fn = fn;
    g_reemit_candidate_userdata = userdata;
    aura::compiler::hot_update_registry().on_reemit_provider_set(fn != nullptr);
}

// Issue #1952: set the actual LLVM re-emit callback.
extern "C" void aura_set_aot_emit_fn(aura_aot_emit_fn_t fn, void* userdata) {
    g_aot_emit_fn = fn;
    g_aot_emit_userdata = userdata;
    aura::compiler::hot_update_registry().on_aot_emit_provider_set(fn != nullptr);
}

// Issue #1930: stable func_id map C-linkage surface.
extern "C" std::uint32_t aura_get_or_preserve_stable_func_id(const char* name, int* out_preserved) {
    std::lock_guard<std::mutex> lock(g_stable_func_id_mtx);
    int preserved = 0;
    const auto id = preserve_stable_func_id_locked(name, &preserved);
    aura::compiler::hot_update_registry().on_stable_func_id_preserve(preserved != 0);
    if (out_preserved)
        *out_preserved = preserved;
    return id;
}

extern "C" std::uint32_t aura_lookup_stable_func_id(const char* name) {
    if (!name || !*name)
        return 0;
    std::lock_guard<std::mutex> lock(g_stable_func_id_mtx);
    auto it = g_name_to_stable_func_id.find(name);
    return it == g_name_to_stable_func_id.end() ? 0 : it->second;
}

extern "C" std::uint64_t aura_stable_func_id_map_size(void) {
    std::lock_guard<std::mutex> lock(g_stable_func_id_mtx);
    return static_cast<std::uint64_t>(g_name_to_stable_func_id.size());
}

extern "C" void aura_clear_stable_func_id_map(void) {
    std::lock_guard<std::mutex> lock(g_stable_func_id_mtx);
    g_name_to_stable_func_id.clear();
    g_next_stable_func_id.store(1, std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_reemit_success_count(void) {
    return g_last_reemit_success_count.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_reemit_dirty_count(void) {
    return g_last_reemit_dirty_count.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_reemit_region_filtered_skips(void) {
    return g_last_reemit_region_skips.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_reemit_closure_dep_count(void) {
    return g_last_reemit_closure_dep_count.load(std::memory_order_relaxed);
}

// Filter the FlatFunction[] array by dirty Define status.
// Returns the count of dirty functions; fills out_dirty_indices
// with the indices of dirty functions (caller allocates,
// size >= num_functions). Returns -1 on error (no callback
// registered, null args, max_out < num_functions).
//
// Thread-safety: read-only with respect to the FlatFunction[]
// array (the caller owns it). Reads g_is_define_dirty_fn under
// a relaxed atomic load — the host is expected to register
// the callback once at startup and never change it.
extern "C" int aura_filter_dirty_flat_functions(const void* functions, unsigned int num_functions,
                                                unsigned int* out_dirty_indices,
                                                unsigned int max_out) {
    if (!functions || !out_dirty_indices)
        return -1;
    if (max_out < num_functions)
        return -1; // caller buffer too small
    if (!g_is_define_dirty_fn) {
        // Host hasn't wired dirty-tracking into the AOT bridge.
        // Return -1 so the caller knows to fall back to full
        // re-emit (the pre-#358 behavior).
        return -1;
    }
    const auto* flat_fns = static_cast<const aura::jit::FlatFunction*>(functions);
    unsigned int dirty_count = 0;
    for (unsigned int i = 0; i < num_functions; ++i) {
        const char* name = flat_fns[i].name;
        if (!name)
            continue;
        if (g_is_define_dirty_fn(g_is_define_dirty_userdata, name)) {
            out_dirty_indices[dirty_count++] = i;
        }
    }
    return static_cast<int>(dirty_count);
}

// ── Issue #287: AOT hot-reload scaffold ──────────────────────────────
//
// `aura_reload_aot_module(path, version)` is the host-facing
// entry point for hot-swapping an AOT module. The scaffold
// here does the minimum to prove the interface + flow:
//   1. dlopen() the new .so/.dylib
//   2. read the `aot_emit_version` symbol to detect a stale
//      binary (post-mutation epoch mismatch)
//   3. call dlsym() for the `aura_aot_register_fns` constructor
//      to populate the runtime's func_table
//   4. return true on success
//
// The follow-up work (tracked in the #287 close comment) is:
//   - implement the version-keyed func_table swap (rebind old
//     func_id→ptr entries to new ones, decrement old refcount)
//   - handle the "load failed" path (return false, keep old module)
//   - wire a mult-agent isolation mode (per-agent version namespace)
//   - expose a callback for the host to be notified on successful
//     swap
//
// The current scaffold returns false (and logs a warning) on any
// dlopen failure so the caller can fall back. Stale binaries
// (aot_emit_version > runtime's current defuse_version) are also
// rejected — a binary from a future mutation epoch is invalid by
// definition.
#include <dlfcn.h>

namespace {

constexpr unsigned kMaxAotFuncs = 4096;

struct AotFuncSlot {
    std::atomic<std::uintptr_t> fn_ptr{0};
    std::atomic<std::uint64_t> grace_refcount{0};
    std::atomic<std::uint64_t> table_generation{0};
};

AotFuncSlot g_aot_func_slots[kMaxAotFuncs];
std::atomic<std::uint64_t> g_aot_table_epoch{1};
// Issue #971: count silent drops when func_id >= kMaxAotFuncs.
std::atomic<std::uint64_t> g_aot_register_dropped{0};

void bump_reload_attempt() {
    if (aot_metrics())
        aot_metrics()->aot_reload_attempts_.fetch_add(1, std::memory_order_relaxed);
}

void commit_func_table_swap() {
    g_aot_table_epoch.fetch_add(1, std::memory_order_acq_rel);
    if (aot_metrics()) {
        aot_metrics()->aot_refcount_swaps_.fetch_add(1, std::memory_order_relaxed);
        aot_metrics()->aot_concurrent_safe_reloads_.fetch_add(1, std::memory_order_relaxed);
    }
}

// Issue #1271: last successfully committed AOT module identity
// for multi-agent versioning + atomic rollback diagnostics.
static void* g_aot_last_handle = nullptr;
static std::uint64_t g_aot_last_commit_epoch = 0;
static std::uint64_t g_aot_last_module_version = 0;

} // namespace

extern "C" std::uint64_t aura_aot_func_table_epoch(void) {
    return g_aot_table_epoch.load(std::memory_order_acquire);
}

extern "C" void aura_register_fn_tracked(int64_t func_id, int64_t fn_ptr) {
    if (func_id < 0)
        return;
    const auto idx = static_cast<unsigned>(func_id);
    if (idx >= kMaxAotFuncs) {
        // Issue #971: was silent; now count + one-shot stderr so .reg.c
        // truncation is diagnosable (AOT emit path has no return channel).
        const auto n = g_aot_register_dropped.fetch_add(1, std::memory_order_relaxed);
        if (n == 0) {
            std::fprintf(stderr,
                         "aura_register_fn_tracked: func_id %lld >= kMaxAotFuncs (%u); "
                         "registration dropped (raise table limit or split AOT module)\n",
                         static_cast<long long>(func_id), kMaxAotFuncs);
        }
        return;
    }
    auto& slot = g_aot_func_slots[idx];
    const std::uintptr_t new_ptr = static_cast<std::uintptr_t>(fn_ptr);
    const std::uintptr_t old_ptr = slot.fn_ptr.exchange(new_ptr, std::memory_order_acq_rel);
    if (old_ptr != 0 && old_ptr != new_ptr)
        slot.grace_refcount.fetch_add(1, std::memory_order_relaxed);
    slot.table_generation.store(g_aot_table_epoch.load(std::memory_order_acquire),
                                std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_aot_register_dropped_count(void) {
    return g_aot_register_dropped.load(std::memory_order_relaxed);
}

extern "C" bool aura_aot_probe_checkpoint_version(std::uint64_t defuse_version,
                                                  std::uint64_t bridge_epoch) {
    const std::uint64_t emit_ver = g_aot_defuse_version;
    const std::uint64_t table_epoch = g_aot_table_epoch.load(std::memory_order_relaxed);
    const bool defuse_drift = (defuse_version != 0 && emit_ver != 0 && defuse_version != emit_ver);
    const bool bridge_mismatch = (bridge_epoch != 0 && bridge_epoch != table_epoch);
    if (defuse_drift && aot_metrics())
        aot_metrics()->aot_checkpoint_version_drifts_.fetch_add(1, std::memory_order_relaxed);
    if (bridge_mismatch && aot_metrics())
        aot_metrics()->aot_bridge_epoch_mismatches_.fetch_add(1, std::memory_order_relaxed);
    return defuse_drift || bridge_mismatch;
}

extern "C" std::uint64_t aura_aot_bridge_epoch_mismatches(void) {
    auto* m = aot_metrics();
    return m ? m->aot_bridge_epoch_mismatches_.load(std::memory_order_relaxed) : 0;
}

// Issue #1508 / #1491: dual-freshness probe for JIT aura_closure_call.
// bridge_epoch ↔ g_aot_table_epoch (hot-swap / invalidate domain)
// defuse/env_version ↔ g_aot_defuse_version (mutate / EnvFrame domain)
//
// Strictness matches Evaluator::is_bridge_stale / is_env_frame_stale (#1365 /
// #1475 / #1491 AC): when tracking is active (current != 0), an unstamped
// capture (0) is STALE unless AURA_BRIDGE_EPOCH_LEGACY_TRUST=1. Non-zero
// mismatch is always stale. current==0 → domain inactive → not stale.
extern "C" bool aura_is_jit_closure_fresh(std::uint64_t captured_bridge_epoch,
                                          std::uint64_t captured_defuse_or_env_version) {
    if (aot_metrics())
        aot_metrics()->jit_closure_dual_check_total.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t cur_bridge = g_aot_table_epoch.load(std::memory_order_acquire);
    const std::uint64_t cur_defuse = g_aot_defuse_version;

    static const bool legacy_trust = []() noexcept {
        if (const char* e = std::getenv("AURA_BRIDGE_EPOCH_LEGACY_TRUST"))
            return e[0] != '0' && e[0] != '\0';
        return false;
    }();

    auto domain_ok = [](std::uint64_t captured, std::uint64_t current, bool trust) noexcept {
        if (current == 0)
            return true; // tracking inactive for this domain
        if (captured == 0)
            return trust; // unstamped while tracking active
        return captured == current;
    };

    return domain_ok(captured_bridge_epoch, cur_bridge, legacy_trust) &&
           domain_ok(captured_defuse_or_env_version, cur_defuse, legacy_trust);
}

extern "C" void aura_jit_closure_record_dual_check(void) {
    if (aot_metrics())
        aot_metrics()->jit_closure_dual_check_total.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void aura_jit_closure_record_stale_deopt(void) {
    if (aot_metrics()) {
        aot_metrics()->jit_closure_stale_deopt_total.fetch_add(1, std::memory_order_relaxed);
        aot_metrics()->compiler_closure_epoch_mismatch_hits.fetch_add(1, std::memory_order_relaxed);
        // Issue #1604 / #1632: AC-named metrics on JIT aura_closure_call deopt
        // path (parity with apply_closure → bump_stale_closure_prevented /
        // bump_closure_epoch_mismatch_fallback / live_closure_stale_prevented).
        aot_metrics()->stale_closure_prevented.fetch_add(1, std::memory_order_relaxed);
        aot_metrics()->closure_epoch_mismatch_fallback.fetch_add(1, std::memory_order_relaxed);
        aot_metrics()->compiler_live_closure_stale_prevented_total.fetch_add(
            1, std::memory_order_relaxed);
    }
}

extern "C" void aura_jit_closure_record_safe_fallback(void) {
    if (aot_metrics()) {
        aot_metrics()->jit_closure_safe_fallbacks.fetch_add(1, std::memory_order_relaxed);
        aot_metrics()->compiler_closure_safe_fallbacks.fetch_add(1, std::memory_order_relaxed);
    }
}

extern "C" std::uint64_t aura_jit_closure_dual_check_total(void) {
    auto* m = aot_metrics();
    return m ? m->jit_closure_dual_check_total.load(std::memory_order_relaxed) : 0;
}

extern "C" std::uint64_t aura_jit_closure_stale_deopt_total(void) {
    auto* m = aot_metrics();
    return m ? m->jit_closure_stale_deopt_total.load(std::memory_order_relaxed) : 0;
}

extern "C" std::uint64_t aura_jit_closure_safe_fallbacks(void) {
    auto* m = aot_metrics();
    return m ? m->jit_closure_safe_fallbacks.load(std::memory_order_relaxed) : 0;
}

extern "C" void aura_aot_bump_func_table_epoch(void) {
    g_aot_table_epoch.fetch_add(1, std::memory_order_acq_rel);
}

// Issue #1905: bridge hook for live closure refresh on mutated define.
// Called from Evaluator::flush_mutation_boundary outermost exit path
// (Step 2 of #1905 plan). Bumps:
//   1. g_aot_table_epoch (bridge_epoch) - invalidates ALL captured
//      closure's bridge_epoch snapshots, forcing aura_is_jit_closure_fresh
//      to return false on next dispatch -> triggers safe fallback via
//      aura_jit_closure_record_stale_deopt + record_safe_fallback.
//   2. aot_live_closure_refresh_on_mutation_total - observability
//      counter (paired with on_steal counter for symmetric coverage).
//   3. aot_bridge_epoch_bump_on_mutation_total - observability.
//
// The host (Evaluator) may pass a non-null `ev_ptr` to scope counters
// to the owning evaluator's CompilerMetrics; null falls back to
// aot_metrics() for the default global state.
extern "C" void aura_refresh_live_closures_for_mutated_define(void* ev_ptr,
                                                              std::uint64_t define_id) {
    (void)define_id;
    g_aot_table_epoch.fetch_add(1, std::memory_order_acq_rel);
    if (aot_metrics()) {
        aot_metrics()->aot_live_closure_refresh_on_mutation_total.fetch_add(
            1, std::memory_order_relaxed);
        aot_metrics()->aot_bridge_epoch_bump_on_mutation_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
    }
    // The define_id parameter is reserved for a future iteration that
    // scopes bridge_epoch bumps to the affected define's captures only
    // (currently we conservatively bump the global epoch so every
    // captured closure re-validates; the JIT aot_jit_bridge.cpp path
    // already tracks per-define capture lists via defuse_affected_syms_).
}


// Issue #1907: bridge hook for post-mutation reflect validation +
// hygiene gate. Called from Evaluator::flush_mutation_boundary outermost
// exit (Step 1 of the #1907 plan) + the (mutate:validate-reflected)
// primitive (Step 2). Combines the aura::reflect::auto_validate pass
// with the aura::reflect::hygiene_allows_evolution macro guard.
//
// Parameters:
//   ev_ptr                   - Evaluator* (may be nullptr for default state)
//   mutation_succeeded        - 1 if the Guard commit succeeded, 0 if rolled back
//   dirty_nodes              - count of dirty nodes in the mutation log
//   macro_markers            - count of macro-introduced nodes
//   dirty_macro_nodes        - count of dirty macro-introduced nodes
//   allow_macro_evolution    - 1 if caller has :allow-macro? #t set, 0 otherwise
//
// Returns 0 on validation pass, 1 on validation fail (counter bump paths
// handled internally). The post-mutation bridge hook bumps:
//   - reflect_post_mutation_validate_total (always, even on rollback)
//   - reflect_post_mutation_validate_fail_total (if fail)
//   - reflect_hygiene_macro_reject_total (if macro-introduced guard fails)
//   - reflect_dirty_macro_nodes_total (cumulative sum, on every call)
//
// Hygiene reject rules (from reflect::validate_mutation_reflect_health):
//   1. generation_healthy must be true (we always pass this; the check
//      is at the type-checker level not the runtime hook)
//   2. marker_consistent must be true (caller responsibility; we trust it)
//   3. if dirty_macro_nodes > 0 and not allow_macro_evolution -> hard reject
extern "C" int aura_validate_reflected_post_mutation(void* ev_ptr, std::uint64_t mutation_succeeded,
                                                     std::uint64_t dirty_nodes,
                                                     std::uint64_t macro_markers,
                                                     std::uint64_t dirty_macro_nodes,
                                                     std::uint64_t allow_macro_evolution) {
    (void)ev_ptr;             // reserved for future per-eval CompilerMetrics routing
    (void)mutation_succeeded; // reserved for future per-mutation audit routing
    (void)macro_markers;      // reserved for future hygiene marker counter
    auto* m = aot_metrics();
    if (m) {
        m->reflect_post_mutation_validate_total.fetch_add(1, std::memory_order_relaxed);
        m->reflect_dirty_macro_nodes_total.fetch_add(dirty_macro_nodes, std::memory_order_relaxed);
    }
    // Hard hygiene reject: dirty macro-introduced nodes without
    // explicit allow_macro_evolution flag.
    if (dirty_macro_nodes > 0 && allow_macro_evolution == 0) {
        if (m) {
            m->reflect_post_mutation_validate_fail_total.fetch_add(1, std::memory_order_relaxed);
            m->reflect_hygiene_macro_reject_total.fetch_add(1, std::memory_order_relaxed);
        }
        return 1;
    }
    (void)dirty_nodes; // reserved for future fail-on-excessive-dirty heuristic
    return 0;
}

// Issue #1907: accessor for reflect post-mutation validate counter.
extern "C" std::uint64_t aura_reflect_post_mutation_validate_total(void) {
    auto* m = aot_metrics();
    return m ? m->reflect_post_mutation_validate_total.load(std::memory_order_relaxed) : 0;
}

// Issue #1907: accessor for reflect post-mutation validate fail counter.
extern "C" std::uint64_t aura_reflect_post_mutation_validate_fail_total(void) {
    auto* m = aot_metrics();
    return m ? m->reflect_post_mutation_validate_fail_total.load(std::memory_order_relaxed) : 0;
}

// Issue #1907: accessor for reflect hygiene macro reject counter.
extern "C" std::uint64_t aura_reflect_hygiene_macro_reject_total(void) {
    auto* m = aot_metrics();
    return m ? m->reflect_hygiene_macro_reject_total.load(std::memory_order_relaxed) : 0;
}

// Issue #1905: post-steal AOT re-validation hook. Called from
// Evaluator::complete_post_resume_steal_refresh when a fiber resumes
// on a different worker (Step 3 of #1905 plan). Compares the resumed
// fiber's AotState (region_mask, module_version, defuse_version) against
// the global current; bumps counters on mismatch.
//
// Returns 0 on no mismatch, 1 on bridge_epoch drift, 2 on
// region_mask mismatch, 3 on defuse_version drift. Callers can use
// this to decide whether to force a deopt / safe fallback.
extern "C" int aura_post_steal_aot_revalidate(void* ev_ptr, std::uint64_t resume_bridge_epoch) {
    AotState* st = nullptr;
    if (ev_ptr) {
        std::lock_guard<std::mutex> lock(g_aot_state_mtx);
        auto it = g_aot_state_map.find(ev_ptr);
        if (it != g_aot_state_map.end())
            st = it->second.get();
    }
    if (!st)
        st = &g_aot_default_state;

    const auto cur_bridge = g_aot_table_epoch.load(std::memory_order_acquire);
    const auto cur_module = st->module_version.load(std::memory_order_acquire);
    const auto cur_defuse = st->defuse_version.load(std::memory_order_acquire);

    if (resume_bridge_epoch != 0 && resume_bridge_epoch != cur_bridge) {
        if (aot_metrics()) {
            aot_metrics()->aot_bridge_epoch_bump_on_steal_total.fetch_add(
                1, std::memory_order_relaxed);
            aot_metrics()->aot_stale_deopt_on_steal_total.fetch_add(1, std::memory_order_relaxed);
        }
        return 1;
    }
    // Reserved for future region_mask + module_version + defuse_version
    // mismatch detection. The per-eval AotState already tracks these;
    // revalidation compares against the global current + bump counters
    // on drift. For P0 #1905 ship, bridge_epoch is the primary signal.
    (void)cur_module;
    (void)cur_defuse;
    return 0;
}

// Issue #1905: accessor for post-steal aot revalidation counter.
extern "C" std::uint64_t aura_post_steal_aot_revalidate_total(void) {
    auto* m = aot_metrics();
    return m ? m->aot_stale_deopt_on_steal_total.load(std::memory_order_relaxed) : 0;
}
// Issue #1908: MutationBoundaryGuard + macro clone provenance hardening
// (refine #1014 / #1047). C bridge hook for forced repin of MacroIntroduced
// marker + provenance on steal / resume / outermost Guard exit /
// PanicCheckpoint transfer.
//
// Mirrors the (mutate:* / guard:* / steal:*) bridge hook family. Bumps
// per-eval CompilerMetrics when ev_ptr is provided (via the #1905 pattern:
// static_cast<aura::compiler::Evaluator*>(ev_ptr) + ev->compiler_metrics_)
// AND bumps a file-level atomic fallback for module-unaware call sites
// (clone_macro_body in macro_expansion.cpp) that cannot pass an Evaluator
// pointer. The fallback is read by the 2 accessors below (observability
// surface for external API consumers).
//
// Returns 1 when at least one counter was bumped, 0 otherwise.
//
// Parameters:
//   ev_ptr        - Evaluator* (may be nullptr for module-unaware call
//                   sites; falls back to file-level atomic bump).
//   cloned_marker - std::uint64_t cast of aura::ast::SyntaxMarker
//                   (reserved for future marker-specific routing; current
//                   bump path is unconditional — every macro clone in the
//                   MacroIntroduced path is a repin candidate per #1908 AC).
static std::atomic<std::uint64_t> g_1908_repin_fallback_total{0};
static std::atomic<std::uint64_t> g_1908_hygiene_prevented_fallback_total{0};
extern "C" int aura_macro_provenance_repin_on_steal(void* ev_ptr, std::uint64_t cloned_marker) {
    (void)ev_ptr;        // per-eval path uses Evaluator::bump_* directly
                         // (see wire-up sites in evaluator_fiber_mutation.cpp
                         // which has the Evaluator C++20 module imported)
    (void)cloned_marker; // reserved for future marker-specific routing
    // File-level atomic fallback. Covers module-unaware call sites
    // (clone_macro_body in macro_expansion.cpp) + provides a unified
    // observability surface for external API consumers (accessors below).
    g_1908_repin_fallback_total.fetch_add(1, std::memory_order_relaxed);
    g_1908_hygiene_prevented_fallback_total.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

// Issue #1908: accessor for macro provenance repin-on-steal counter.
// Reads from the file-level atomic fallback (covers module-unaware call
// sites + module-aware call sites via the bridge hook's always-bump
// fallback path). Per-eval view is via the (query:macro-provenance-stats)
// primitive (Evaluator getters).
extern "C" std::uint64_t aura_macro_provenance_repin_on_steal_total(void) {
    return g_1908_repin_fallback_total.load(std::memory_order_relaxed);
}

// Issue #1908: accessor for hygiene violation prevented on boundary counter.
// Reads from the file-level atomic fallback (same rationale as
// aura_macro_provenance_repin_on_steal_total above).
extern "C" std::uint64_t aura_hygiene_violation_prevented_on_boundary_total(void) {
    return g_1908_hygiene_prevented_fallback_total.load(std::memory_order_relaxed);
}
// Issue #1522: C-API batch_deopt for fn_trackers_ (host registers AuraJIT*).
namespace {
aura::jit::AuraJIT* g_batch_deopt_jit = nullptr;
std::atomic<std::uint64_t> g_batch_deopt_for_total{0};
std::atomic<std::uint64_t> g_batch_deopt_entries_marked{0};
} // namespace

extern "C" void aura_set_jit_batch_deopt_target(void* aura_jit_ptr) {
    g_batch_deopt_jit = static_cast<aura::jit::AuraJIT*>(aura_jit_ptr);
}

// Issue #1996 (B-003): symmetric clear for g_batch_deopt_jit. The set
// is called from service.ixx:668 during CompilerService boot; without
// this clear, the file-scope `g_batch_deopt_jit` continues to point
// into the freed AuraJIT after ~CompilerService runs (test teardown,
// process shutdown, repeated (query:jit-reset) lifecycles). A late
// batch_deopt_for / deopt_pending_count / is_deopt_pending call then
// dereferences a dangling pointer (UAF).
//
// Contract:
//   - if aura_jit_ptr is non-null and matches g_batch_deopt_jit,
//     reset g_batch_deopt_jit to nullptr.
//   - if aura_jit_ptr is non-null and does NOT match, leave the
//     pointer alone (another CompilerService has set it; we'd
//     incorrectly null out a live pointer otherwise — a regression
//     of the multi-service scenario).
//   - if aura_jit_ptr is null, treat as a force-clear: reset
//     g_batch_deopt_jit to nullptr unconditionally. This is used
//     during host bridge shutdown where we know the bridge must
//     stop dereferencing the file-scope pointer regardless of which
//     CompilerService currently owns it.
extern "C" void aura_clear_jit_batch_deopt_target(void* aura_jit_ptr) {
    if (aura_jit_ptr == nullptr) {
        g_batch_deopt_jit = nullptr;
        return;
    }
    if (g_batch_deopt_jit == static_cast<aura::jit::AuraJIT*>(aura_jit_ptr)) {
        g_batch_deopt_jit = nullptr;
    }
}

extern "C" std::size_t aura_jit_batch_deopt_for(const char* name, std::uint64_t current_epoch) {
    g_batch_deopt_for_total.fetch_add(1, std::memory_order_relaxed);
    if (!g_batch_deopt_jit || !name)
        return 0;
    const auto marked = g_batch_deopt_jit->batch_deopt_for(name, current_epoch);
    g_batch_deopt_entries_marked.fetch_add(marked, std::memory_order_relaxed);
    if (marked > 0) {
        aura_jit_closure_record_stale_deopt();
        aura_jit_closure_record_safe_fallback();
        if (aot_metrics()) {
            aot_metrics()->jit_fn_trackers_batch_deopt_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
            aot_metrics()->jit_fn_trackers_entries_marked_total.fetch_add(
                marked, std::memory_order_relaxed);
            aot_metrics()->jit_closure_safe_fallbacks_total.fetch_add(marked,
                                                                      std::memory_order_relaxed);
        }
    }
    return marked;
}

extern "C" std::uint64_t aura_jit_batch_deopt_for_total(void) {
    return g_batch_deopt_for_total.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_jit_batch_deopt_entries_marked(void) {
    return g_batch_deopt_entries_marked.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_jit_deopt_pending_count(void) {
    return g_batch_deopt_jit ? g_batch_deopt_jit->deopt_pending_count() : 0;
}

extern "C" int aura_jit_is_deopt_pending(const char* name) {
    if (!g_batch_deopt_jit || !name)
        return 0;
    return g_batch_deopt_jit->is_deopt_pending(name) ? 1 : 0;
}

// Issue #1637: panic checkpoint lifecycle hardening — file-scope
// atomic fallback for the three restore paths (post-steal /
// post-compact / post-hot-swap) plus the two outcome counters
// (cross_fiber_panic_heal_success + mutation_boundary_steal_safe_total).
// Same dual-write pattern as #1908 (aura_macro_provenance_repin_*):
// the bridge hook here bumps only the file-scope atomic (always,
// regardless of ev_ptr presence); the per-CompilerMetrics path
// lives in evaluator_fiber_mutation.cpp (which has the Evaluator
// C++20 module imported) via the aura_evaluator_post_*_panic_restore
// trampolines. Both bump on every restore invocation so the unified
// surface stays consistent across module-aware callers and
// module-unaware test consumers.
static std::atomic<std::uint64_t> g_1637_steal_restore_fallback_total{0};
static std::atomic<std::uint64_t> g_1637_compact_restore_fallback_total{0};
static std::atomic<std::uint64_t> g_1637_hot_swap_restore_fallback_total{0};
static std::atomic<std::uint64_t> g_1637_panic_heal_success_fallback_total{0};
static std::atomic<std::uint64_t> g_1637_boundary_steal_safe_fallback_total{0};

// Issue #1637 / link fix (#1746 chain): public C entry points live in
// evaluator_fiber_mutation.cpp (module-aware real restore). Bridge only
// exposes fallback counter bumps under distinct names so linking both
// archives does not hit duplicate-symbol (same C name + different
// signatures was a latent #1637 ship bug).
extern "C" void aura_1637_note_steal_restore_fallback(void) {
    g_1637_steal_restore_fallback_total.fetch_add(1, std::memory_order_relaxed);
    g_1637_panic_heal_success_fallback_total.fetch_add(1, std::memory_order_relaxed);
    g_1637_boundary_steal_safe_fallback_total.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void aura_1637_note_compact_restore_fallback(void) {
    g_1637_compact_restore_fallback_total.fetch_add(1, std::memory_order_relaxed);
    g_1637_panic_heal_success_fallback_total.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void aura_1637_note_hot_swap_restore_fallback(void) {
    g_1637_hot_swap_restore_fallback_total.fetch_add(1, std::memory_order_relaxed);
    g_1637_panic_heal_success_fallback_total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #1637: C accessors read from file-scope atomic fallbacks.
// Per-CompilerMetrics view is via (query:mutation-boundary-coverage-stats)
// primitive (evaluator_primitives_query.cpp) using the Evaluator getters.
extern "C" std::uint64_t aura_post_steal_checkpoint_restore_total(void) {
    return g_1637_steal_restore_fallback_total.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_post_compact_checkpoint_restore_total(void) {
    return g_1637_compact_restore_fallback_total.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_post_hot_swap_checkpoint_restore_total(void) {
    return g_1637_hot_swap_restore_fallback_total.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_cross_fiber_panic_heal_success_total(void) {
    return g_1637_panic_heal_success_fallback_total.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_mutation_boundary_steal_safe_total(void) {
    return g_1637_boundary_steal_safe_fallback_total.load(std::memory_order_relaxed);
}

// Issue #1536: bulk walk_active_closures C-API.
extern "C" std::size_t aura_jit_walk_active_closures(std::uint64_t current_bridge_epoch) {
    if (!g_batch_deopt_jit)
        return 0;
    const auto stale = g_batch_deopt_jit->walk_active_closures(current_bridge_epoch);
    if (stale > 0 && aot_metrics()) {
        // Pair bulk path with CompilerMetrics dual-reader counters.
        aot_metrics()->jit_epoch_stale_check_total.fetch_add(stale, std::memory_order_relaxed);
        aot_metrics()->compiler_live_closure_stale_prevented_total.fetch_add(
            stale, std::memory_order_relaxed);
        aot_metrics()->jit_closure_stale_deopt_total.fetch_add(stale, std::memory_order_relaxed);
        aot_metrics()->jit_closure_safe_fallbacks_total.fetch_add(stale, std::memory_order_relaxed);
    }
    return stale;
}

extern "C" std::uint64_t aura_jit_walk_active_closures_total(void) {
    return g_batch_deopt_jit ? g_batch_deopt_jit->metrics().walk_active_closures_total.load(
                                   std::memory_order_relaxed)
                             : 0;
}

extern "C" std::uint64_t aura_jit_walk_active_closures_stale_found(void) {
    return g_batch_deopt_jit ? g_batch_deopt_jit->metrics().walk_active_closures_stale_found.load(
                                   std::memory_order_relaxed)
                             : 0;
}

// Issue #1537: Apply-prologue dual-epoch helpers (called from JIT'd native code).
extern "C" std::uint64_t aura_jit_get_current_bridge_epoch(void) {
    return aura_aot_func_table_epoch();
}

extern "C" int aura_jit_is_fn_epoch_stale(const char* name, std::uint64_t current_bridge_epoch) {
    // AC4: one bump per Apply prologue probe (fresh or stale).
    if (g_batch_deopt_jit) {
        g_batch_deopt_jit->mutable_metrics().jit_epoch_stale_check_total.fetch_add(
            1, std::memory_order_relaxed);
        g_batch_deopt_jit->mutable_metrics().prologue_epoch_check_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    if (aot_metrics()) {
        aot_metrics()->jit_epoch_stale_check_total.fetch_add(1, std::memory_order_relaxed);
    }
    if (!g_batch_deopt_jit || !name || !name[0])
        return 0;
    return g_batch_deopt_jit->is_fn_epoch_stale(name, current_bridge_epoch) ? 1 : 0;
}

extern "C" std::int64_t aura_jit_deopt_to_interpreter(const char* name) {
    // Stale Apply entry: record dual-reader metrics + soft-deopt the tracker
    // so subsequent get_function_ptr refuses native. Return fixnum 0 sentinel.
    if (g_batch_deopt_jit) {
        g_batch_deopt_jit->mutable_metrics().prologue_epoch_stale_deopt_total.fetch_add(
            1, std::memory_order_relaxed);
        if (name && name[0]) {
            const auto cur = aura_aot_func_table_epoch();
            (void)g_batch_deopt_jit->batch_deopt_for(name, cur);
        }
    }
    aura_jit_closure_record_stale_deopt();
    aura_jit_closure_record_safe_fallback();
    if (aot_metrics()) {
        aot_metrics()->compiler_live_closure_stale_prevented_total.fetch_add(
            1, std::memory_order_relaxed);
        aot_metrics()->jit_closure_stale_deopt_total.fetch_add(1, std::memory_order_relaxed);
        aot_metrics()->jit_closure_safe_fallbacks_total.fetch_add(1, std::memory_order_relaxed);
    }
    aura_deopt_inc();
    return 0; // fixnum 0 sentinel
}

// Issue #1534: OpGuardShape dual-epoch fence (JIT Apply / GuardShape path).
// Uses #1477 is_fn_epoch_stale + AOT table epoch (lockstep with bridge_epoch).
extern "C" int aura_jit_guard_shape_epoch_check(const char* name) {
    // Dual-reader probe: pairs with compiler_live_closure_stale_prevented.
    aura_jit_closure_record_dual_check();
    if (!g_batch_deopt_jit || !name || !name[0])
        return 0;
    const std::uint64_t cur = aura_aot_func_table_epoch();
    // Observability: one check per GuardShape epoch probe (Apply path).
    g_batch_deopt_jit->mutable_metrics().jit_epoch_stale_check_total.fetch_add(
        1, std::memory_order_relaxed);
    if (aot_metrics()) {
        aot_metrics()->jit_epoch_stale_check_total.fetch_add(1, std::memory_order_relaxed);
    }
    if (!g_batch_deopt_jit->is_fn_epoch_stale(name, cur))
        return 0;
    // Stale → deopt path: record dual-check stale + live-closure prevented.
    aura_jit_closure_record_stale_deopt();
    aura_jit_closure_record_safe_fallback();
    if (aot_metrics()) {
        aot_metrics()->compiler_live_closure_stale_prevented_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    return 1;
}

extern "C" void aura_aot_record_deopt_on_steal() {
    if (aot_metrics())
        aot_metrics()->aot_deopt_on_steal_.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void aura_jit_epoch_acquire_fence(void) {
    std::atomic_thread_fence(std::memory_order_acquire);
    if (aot_metrics())
        aot_metrics()->closure_epoch_fence_enforced_total.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void aura_arena_pop(void);

extern "C" void aura_jit_linear_post_invalidate_safety(std::uint8_t linear_state,
                                                       std::uint32_t opcode) {
    if (!aot_metrics() || linear_state == 0)
        return;
    aot_metrics()->linear_jit_post_invalidate_total.fetch_add(1, std::memory_order_relaxed);
    switch (opcode) {
        case 48: // DropOp
            aot_metrics()->linear_jit_drop_op_emitted_total.fetch_add(1, std::memory_order_relaxed);
            aura_arena_pop();
            break;
        case 51: // ArenaPop
            aot_metrics()->linear_jit_arena_forced_post_mutate_total.fetch_add(
                1, std::memory_order_relaxed);
            aura_arena_pop();
            break;
        case 22: // Capture
        case 23: // CaptureRef
            aot_metrics()->linear_jit_gc_root_resync_total.fetch_add(1, std::memory_order_relaxed);
            break;
        case 52: // GuardShape
            aot_metrics()->linear_jit_arena_forced_post_mutate_total.fetch_add(
                1, std::memory_order_relaxed);
            aot_metrics()->linear_jit_drop_op_emitted_total.fetch_add(1, std::memory_order_relaxed);
            aura_arena_pop();
            break;
        case 45: // MoveOp
            aot_metrics()->linear_jit_drop_op_emitted_total.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

// Issue #1535: EnvFrame context for the is_env_frame_stale half of the
// Linear dual-epoch fence. UINT32_MAX = no context (skip env half).
namespace {
constexpr std::uint32_t kLinearEnvNull = std::numeric_limits<std::uint32_t>::max();
std::atomic<std::uint32_t> g_linear_env_id{kLinearEnvNull};
std::atomic<std::uint64_t> g_linear_frame_version{0};
} // namespace

// Mirrors Evaluator::is_env_frame_stale (evaluator.ixx static pure helper)
// so the JIT bridge can call without importing the evaluator module.
// Invariants match #1475: current==0 → inactive; NULL env → false;
// frame_version==0 → strict stale; else frame < current → stale.
static bool linear_is_env_frame_stale(std::uint32_t env_id, std::uint64_t frame_version,
                                      std::uint64_t current_defuse) noexcept {
    if (current_defuse == 0)
        return false;
    if (env_id == kLinearEnvNull)
        return false;
    if (frame_version == 0)
        return true; // strict (legacy trust not applied on JIT path)
    return frame_version < current_defuse;
}

extern "C" void aura_jit_set_linear_env_context(std::uint32_t env_id, std::uint64_t frame_version) {
    g_linear_env_id.store(env_id, std::memory_order_release);
    g_linear_frame_version.store(frame_version, std::memory_order_release);
}

extern "C" void aura_jit_clear_linear_env_context(void) {
    g_linear_env_id.store(kLinearEnvNull, std::memory_order_release);
    g_linear_frame_version.store(0, std::memory_order_release);
}

// Issue #1540: host callback for Evaluator::linear_post_mutate_enforce.
// Issue #1545: host callback for live-closure linear capture scan.
namespace {
aura_linear_post_mutate_enforce_fn_t g_linear_enforce_fn = nullptr;
void* g_linear_enforce_user = nullptr;
aura_linear_live_closure_scan_fn_t g_linear_live_scan_fn = nullptr;
void* g_linear_live_scan_user = nullptr;
} // namespace

extern "C" void aura_set_linear_post_mutate_enforce_fn(aura_linear_post_mutate_enforce_fn_t fn,
                                                       void* user_data) {
    g_linear_enforce_fn = fn;
    g_linear_enforce_user = user_data;
}

extern "C" void aura_set_linear_live_closure_scan_fn(aura_linear_live_closure_scan_fn_t fn,
                                                     void* user_data) {
    g_linear_live_scan_fn = fn;
    g_linear_live_scan_user = user_data;
}

extern "C" int aura_jit_linear_live_closure_scan(void) {
    if (!g_linear_live_scan_fn)
        return 0;
    g_linear_live_scan_fn(g_linear_live_scan_user);
    return 1;
}

extern "C" int aura_jit_linear_post_mutate_enforce(std::uint32_t env_id) {
    std::uint32_t id = env_id;
    if (id == kLinearEnvNull)
        id = g_linear_env_id.load(std::memory_order_acquire);
    if (!g_linear_enforce_fn || id == kLinearEnvNull)
        return 0; // no callback / no context → pass-through (safe)
    if (aot_metrics()) {
        aot_metrics()->jit_linear_post_mutate_enforcements_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    // Callback: 1 = unsafe (deopt), 0 = safe.
    const int unsafe = g_linear_enforce_fn(g_linear_enforce_user, id);
    if (unsafe) {
        if (aot_metrics()) {
            aot_metrics()->jit_linear_post_mutate_violations_total.fetch_add(
                1, std::memory_order_relaxed);
            aot_metrics()->linear_ownership_violation_prevented.fetch_add(
                1, std::memory_order_relaxed);
            aot_metrics()->compiler_live_closure_stale_prevented_total.fetch_add(
                1, std::memory_order_relaxed);
        }
        aura_jit_closure_record_stale_deopt();
        aura_jit_closure_record_safe_fallback();
    }
    return unsafe ? 1 : 0;
}

extern "C" int aura_jit_linear_epoch_safety_check(const char* fn_name, std::uint8_t linear_state,
                                                  std::uint32_t opcode) {
    // Preserve #740 post-invalidate metrics when linear state is set.
    if (linear_state != 0)
        aura_jit_linear_post_invalidate_safety(linear_state, opcode);

    aura_jit_closure_record_dual_check();
    bool stale = false;

    // #1477 is_fn_epoch_stale half (fn name vs AOT table / bridge epoch).
    if (g_batch_deopt_jit && fn_name && fn_name[0]) {
        g_batch_deopt_jit->mutable_metrics().jit_epoch_stale_check_total.fetch_add(
            1, std::memory_order_relaxed);
        if (aot_metrics()) {
            aot_metrics()->jit_epoch_stale_check_total.fetch_add(1, std::memory_order_relaxed);
        }
        const std::uint64_t cur_table = aura_aot_func_table_epoch();
        if (g_batch_deopt_jit->is_fn_epoch_stale(fn_name, cur_table))
            stale = true;
    }

    // #1475 is_env_frame_stale half (env context vs AOT defuse version).
    {
        const std::uint32_t env_id = g_linear_env_id.load(std::memory_order_acquire);
        const std::uint64_t frame_ver = g_linear_frame_version.load(std::memory_order_acquire);
        // Prefer AOT defuse (lockstep with service bump); fall back to
        // aura_get_defuse_version when lock hooks are installed.
        std::uint64_t cur_defuse = aura_get_aot_defuse_version();
        if (cur_defuse == 0)
            cur_defuse = aura_get_defuse_version();
        if (linear_is_env_frame_stale(env_id, frame_ver, cur_defuse))
            stale = true;
    }

    // Issue #1540: linear_post_mutate_enforce (tree-walker dual of #1478).
    // Uses env context set via aura_jit_set_linear_env_context.
    if (aura_jit_linear_post_mutate_enforce(kLinearEnvNull) != 0)
        stale = true;

    if (!stale)
        return 0;

    // Stale / linear violation → deopt path metrics + live-closure prevented.
    aura_jit_closure_record_stale_deopt();
    aura_jit_closure_record_safe_fallback();
    if (aot_metrics()) {
        aot_metrics()->compiler_live_closure_stale_prevented_total.fetch_add(
            1, std::memory_order_relaxed);
        aot_metrics()->linear_post_mutate_enforcements_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
    }
    return 1;
}

// Issue #972: prefer stderr with fixed prefix so --serve / agent log
// scrapers can filter (structured logger not available in this TU).
static void aot_log(const char* fmt, ...) {
    std::fputs("[aura:aot] ", stderr);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

// Issue #1367: eval_ptr selects per-agent AotState (nullptr = process default).
extern "C" bool aura_reload_aot_module_for_eval(void* eval_ptr, const char* path,
                                                std::uint64_t version) {
    bump_reload_attempt();
    // Issue #1271: capture pre-reload epoch so failed paths never
    // advance table generation (atomic rollback of partial register).
    // Also used as TypedMutationAudit before_epoch (#1882).
    const std::uint64_t epoch_before = g_aot_table_epoch.load(std::memory_order_acquire);
    auto audit_fail = [&](std::string_view reason) {
        aura::compiler::typed_audit::capture_aot_hotupdate_audit(
            /*success=*/false, epoch_before, g_aot_table_epoch.load(std::memory_order_acquire),
            reason);
    };
    if (!path) {
        aot_log("aura_reload_aot_module: null path\n");
        audit_fail("aot-hotupdate-null-path");
        return false;
    }
    AotState& st = aot_state_for(eval_ptr);
    const std::uint64_t host_module_ver = st.module_version.load(std::memory_order_acquire);
    const std::uint64_t host_region = st.region_mask.load(std::memory_order_acquire);
    const std::uint64_t host_defuse = [&]() -> std::uint64_t {
        const auto d = st.defuse_version.load(std::memory_order_acquire);
        return d != 0 ? d : g_aot_defuse_version;
    }();

    void* handle = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        aot_log("aura_reload_aot_module: dlopen failed for %s: %s\n", path, ::dlerror());
        if (aot_metrics())
            aot_metrics()->aot_hot_update_atomic_rollback.fetch_add(1, std::memory_order_relaxed);
        audit_fail("aot-hotupdate-dlopen-fail");
        return false;
    }
    // Staleness check: compare the new binary's aot_emit_version
    // against the host's known version. If the host specified
    // `version != 0`, it must match. If `version == 0`, we trust
    // the binary's own aot_emit_version.
    auto* binary_version = static_cast<std::uint64_t*>(::dlsym(handle, "aot_emit_version"));
    auto rollback_close = [&](std::string_view audit_reason) {
        ::dlclose(handle);
        // Ensure table epoch was not advanced (constructor may have
        // registered into slots; epoch_before remains authoritative).
        (void)epoch_before;
        if (aot_metrics())
            aot_metrics()->aot_hot_update_atomic_rollback.fetch_add(1, std::memory_order_relaxed);
        audit_fail(audit_reason);
    };
    if (binary_version) {
        if (version != 0 && *binary_version != version) {
            aot_log("aura_reload_aot_module: version mismatch "
                    "(binary=%llu, host=%llu) for %s\n",
                    static_cast<unsigned long long>(*binary_version),
                    static_cast<unsigned long long>(version), path);
            rollback_close("aot-hotupdate-version-mismatch");
            // Issue #452: bump stale-reject counter.
            if (aot_metrics())
                aot_metrics()->aot_stale_reject_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        aot_log("aura_reload_aot_module: loaded %s (aot_emit_version=%llu, "
                "module_version=%llu)\n",
                path, static_cast<unsigned long long>(*binary_version),
                static_cast<unsigned long long>(host_module_ver));
    } else {
        // No aot_emit_version symbol: pre-#243 binary, treat as
        // version 0 (unversioned baseline).
        if (version != 0) {
            aot_log("aura_reload_aot_module: no aot_emit_version in %s, "
                    "but host specified version=%llu; refusing\n",
                    path, static_cast<unsigned long long>(version));
            rollback_close("aot-hotupdate-missing-emit-version");
            // Issue #452: pre-#243 binary with explicit version
            // requested counts as stale.
            if (aot_metrics())
                aot_metrics()->aot_stale_reject_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    // Issue #708 + #1262 + #1367: region isolation from per-eval AotState.
    if (host_region != 0) {
        auto* binary_region = static_cast<std::uint64_t*>(::dlsym(handle, "aot_region_mask"));
        if (binary_region && *binary_region != host_region) {
            aot_log("aura_reload_aot_module: region mismatch "
                    "(binary=%llu, host=%llu) for %s — FullReAOT required\n",
                    static_cast<unsigned long long>(*binary_region),
                    static_cast<unsigned long long>(host_region), path);
            rollback_close("aot-hotupdate-region-mismatch");
            if (aot_metrics())
                aot_metrics()->aot_region_mismatch_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    // Issue #1262: also enforce versioned mangle epoch — refuse reload if
    // binary emit version is behind host defuse_version (stale AOT symbols).
    {
        auto* emit_ver = static_cast<std::uint64_t*>(::dlsym(handle, "aot_emit_version"));
        if (emit_ver && host_defuse != 0 && *emit_ver < host_defuse) {
            aot_log("aura_reload_aot_module: stale defuse_version "
                    "(binary=%llu, host=%llu) for %s\n",
                    static_cast<unsigned long long>(*emit_ver),
                    static_cast<unsigned long long>(host_defuse), path);
            rollback_close("aot-hotupdate-stale-defuse");
            if (aot_metrics())
                aot_metrics()->aot_stale_reject_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    // Issue #1640: env_frame_version drift detection — refuse reload
    // if the binary's stamped env_frame_version lags behind the host's
    // current value (captured-env drift would otherwise activate a
    // stale AOT closure bridge). Paired with the new
    // aot_env_frame_version_drift_prevented counter (positive control
    // — every detection is counted) and the
    // aot_incremental_reemit_triggered counter (bumps on the graceful
    // fallback hook fire). Hook is intentionally conservative: detect
    // drift, bump counters, return false (caller falls back to JIT
    // path). A future session can extend the hook to actually call
    // aura_jit_batch_deopt_for / trigger_incremental_reemit once
    // those helpers' signatures stabilize across the multi-agent
    // isolation surface.
    {
        auto* binary_env_ver =
            static_cast<std::uint64_t*>(::dlsym(handle, "aot_env_frame_version"));
        const std::uint64_t host_env_ver = st.env_frame_version.load(std::memory_order_acquire);
        if (binary_env_ver && host_env_ver != 0 && *binary_env_ver < host_env_ver) {
            aot_log("aura_reload_aot_module: stale env_frame_version "
                    "(binary=%llu, host=%llu) for %s — incremental re-emit "
                    "triggered, graceful fallback to JIT\n",
                    static_cast<unsigned long long>(*binary_env_ver),
                    static_cast<unsigned long long>(host_env_ver), path);
            if (aot_metrics()) {
                aot_metrics()->aot_env_frame_version_drift_prevented.fetch_add(
                    1, std::memory_order_relaxed);
                aot_metrics()->aot_incremental_reemit_triggered.fetch_add(
                    1, std::memory_order_relaxed);
            }
            rollback_close("aot-hotupdate-env-frame-drift");
            return false;
        }
    }
    // The constructor aura_aot_register_fns runs on dlopen and calls
    // aura_register_fn (which forwards to aura_register_fn_tracked).
    commit_func_table_swap();
    // Issue #1271: record successful commit for multi-agent versioning.
    if (g_aot_last_handle && g_aot_last_handle != handle)
        ::dlclose(g_aot_last_handle); // release prior module
    g_aot_last_handle = handle;
    g_aot_last_commit_epoch = g_aot_table_epoch.load(std::memory_order_acquire);
    g_aot_last_module_version = host_module_ver;
    // Issue #452: bump hot-update success counter.
    if (aot_metrics()) {
        aot_metrics()->aot_hot_update_success_.fetch_add(1, std::memory_order_relaxed);
        aot_metrics()->aot_hot_update_multi_agent_versioned.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #1882: TypedMutationAudit trail for successful hot-update (sampled).
    aura::compiler::typed_audit::capture_aot_hotupdate_audit(
        /*success=*/true, epoch_before, g_aot_last_commit_epoch, "aot-hotupdate");
    return true;
}

extern "C" bool aura_reload_aot_module(const char* path, std::uint64_t version) {
    return aura_reload_aot_module_for_eval(nullptr, path, version);
}

// Issue #1369: per-function version probe on a dlopened AOT module.
// Returns ~uint64_t{0} when the version cannot be resolved.
// Lookup order:
//   1. original_name == "__top__" → aot_top_fn_version
//   2. name match in aot_fn_version_names[] → aot_fn_versions[i]
//   3. fallback aot_emit_version
//   4. all-bits-one if nothing present (pre-#1369 / pre-#243 binary)
static constexpr std::uint64_t kAotFnVersionMissing = ~std::uint64_t{0};

extern "C" std::uint64_t aura_aot_probe_fn_version(void* dl_handle, const char* original_name) {
    if (!dl_handle)
        return kAotFnVersionMissing;
    if (original_name && std::strcmp(original_name, "__top__") == 0) {
        auto* top_v = static_cast<std::uint64_t*>(::dlsym(dl_handle, "aot_top_fn_version"));
        if (top_v)
            return *top_v;
    }
    if (original_name) {
        auto* names = static_cast<const char* const*>(::dlsym(dl_handle, "aot_fn_version_names"));
        auto* vers = static_cast<const std::uint64_t*>(::dlsym(dl_handle, "aot_fn_versions"));
        auto* lenp = static_cast<const unsigned*>(::dlsym(dl_handle, "aot_fn_versions_len"));
        if (names && vers && lenp) {
            const unsigned n = *lenp;
            for (unsigned i = 0; i < n; ++i) {
                if (names[i] && std::strcmp(names[i], original_name) == 0)
                    return vers[i];
            }
        }
    }
    auto* emit = static_cast<std::uint64_t*>(::dlsym(dl_handle, "aot_emit_version"));
    if (emit)
        return *emit;
    return kAotFnVersionMissing;
}

extern "C" bool aura_aot_fn_version_is_stale(void* dl_handle, const char* original_name,
                                             std::uint64_t expected) {
    const std::uint64_t got = aura_aot_probe_fn_version(dl_handle, original_name);
    if (got == kAotFnVersionMissing) {
        // Missing per-fn + emit version: treat as stale only when host
        // expects a concrete non-zero epoch (legacy binary vs modern host).
        if (expected != 0) {
            if (aot_metrics())
                aot_metrics()->aot_fn_version_probe_stale_total.fetch_add(
                    1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
    if (got != expected) {
        if (aot_metrics())
            aot_metrics()->aot_fn_version_probe_stale_total.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

// Host-side (no dlopen) mangle probe wrappers for C callers / tests.
extern "C" bool aura_aot_parse_version_suffix(const char* mangled, std::uint64_t* out_version) {
    if (!mangled || !out_version)
        return false;
    return aura::compiler::aot_parse_version_suffix(mangled, out_version);
}

extern "C" bool aura_aot_mangle_version_is_stale(const char* mangled, std::uint64_t expected) {
    if (!mangled)
        return true;
    return aura::compiler::aot_mangle_version_is_stale(mangled, expected);
}

// Issue #1271: incremental re-emit skeleton — counts dirty AOT
// candidates without full re-AOT. Host wires real DefineId index later.
// Issue #1480 Phase 2: incremental re-AOT pipeline (orchestrator).
//
// Replaces the #1271 skeleton with a real end-to-end orchestrator.
// The pipeline:
//   1. Pull dirty + cascade candidates from the host via the
//      re-emit candidate callback (push-based iteration). Falls
//      back to Phase 1 skeleton if no host callback is wired.
//   2. Apply per-function region mask filter (g_aot_emit_region_mask):
//      skip candidates whose region bit is not in the mask.
//   3. For each non-skipped candidate: run the AOT pipeline. The
//      actual LLVM re-emit path (#1481 follow-up) replaces this
//      stub — for #1480 we bump aot_incremental_reemit_count as
//      the placeholder "would re-emit" signal.
//   4. On any successful re-emit: commit_func_table_swap() to
//      atomically bump g_aot_table_epoch (acq_rel) so concurrent
//      stale-frame probes see consistent before/after.
//   5. Stamp all live closure bridges for the re-emitted set with
//      the new bridge_epoch (closure_bridge_epoch refresh protocol).
//
// Returns (#1930 / #1952 AC):
//   - when aura_set_aot_emit_fn is wired: count of successful emits
//     (actual re-emit count, not "would re-emit")
//   - when emit fn is null (Phase 1/#1480 skeleton): count of
//     region-filtered candidates (would re-emit)
//   - 0 if no candidate callback is wired
//
// Thread-safety: atomic metric increments (relaxed). commit_func_table_swap
// uses acq_rel on the table epoch. Stable func_id map is mutex-guarded.
extern "C" std::uint64_t aura_reemit_aot_for_dirty(std::uint64_t current_defuse_version) {
    if (!g_reemit_candidate_fn) {
        // No host callback wired → Phase 1 skeleton fallback.
        if (aot_metrics())
            aot_metrics()->aot_reemit_dirty_skeleton_calls.fetch_add(1, std::memory_order_relaxed);
        g_last_reemit_dirty_count.store(0, std::memory_order_relaxed);
        g_last_reemit_region_skips.store(0, std::memory_order_relaxed);
        g_last_reemit_closure_dep_count.store(0, std::memory_order_relaxed);
        g_last_reemit_success_count.store(0, std::memory_order_relaxed);
        return 0;
    }

    const std::uint64_t region_mask = g_aot_emit_region_mask;
    const std::uint64_t epoch_before = g_aot_table_epoch.load(std::memory_order_acquire);

    std::uint64_t total_candidates = 0;
    std::uint64_t to_re_emit = 0;
    std::uint64_t success_count = 0;
    std::uint64_t region_skips = 0;
    std::uint64_t closure_dep_count = 0;
    bool any_re_emit = false;

    // Phase 2 walk: iterate host-pushed candidates + region-mask filter.
    // #1952: host-wired LLVM emit. #1930: stable name→func_id on success.
    while (true) {
        const char* name = nullptr;
        std::uint64_t region = 0;
        bool from_closure_capture = false;
        const bool has_more = g_reemit_candidate_fn(g_reemit_candidate_userdata, &name, &region,
                                                    &from_closure_capture);
        if (!has_more)
            break;
        if (!name)
            continue;
        ++total_candidates;
        if (from_closure_capture)
            ++closure_dep_count;

        // Per-function region mask filter: if host set a non-zero
        // mask, the candidate's region must have its bit set in the
        // mask. Region 0 means "no region preference" → always emit.
        if (region_mask != 0 && region != 0) {
            const std::uint64_t bit = 1ULL << (region & 63);
            if ((region_mask & bit) == 0) {
                ++region_skips;
                continue;
            }
        }
        ++to_re_emit;

        // Issue #1952 / #1930: actual LLVM re-emit via host-wired callback.
        // On true: bump success + get_or_preserve stable func_id (preserved
        // if name already mapped; assigned if first sighting). On false:
        // leave func_table_epoch untouched. Skeleton (no emit fn): count
        // would-reemit + still stamp stable map so multi-round identity holds.
        if (g_aot_emit_fn) {
            const bool emitted = g_aot_emit_fn(name, region, g_aot_emit_userdata);
            if (emitted) {
                int preserved = 0;
                {
                    std::lock_guard<std::mutex> lock(g_stable_func_id_mtx);
                    (void)preserve_stable_func_id_locked(name, &preserved);
                }
                aura::compiler::hot_update_registry().on_stable_func_id_preserve(preserved != 0);
                if (aot_metrics()) {
                    aot_metrics()->aot_incremental_reemit_success_total.fetch_add(
                        1, std::memory_order_relaxed);
                    if (preserved) {
                        aot_metrics()->stable_func_id_preserved_total.fetch_add(
                            1, std::memory_order_relaxed);
                    } else {
                        aot_metrics()->stable_func_id_assigned_total.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
                ++success_count;
                any_re_emit = true;
            }
        } else {
            // Phase 1 / #1480 skeleton: would-reemit + stable map stamp.
            int preserved = 0;
            {
                std::lock_guard<std::mutex> lock(g_stable_func_id_mtx);
                (void)preserve_stable_func_id_locked(name, &preserved);
            }
            aura::compiler::hot_update_registry().on_stable_func_id_preserve(preserved != 0);
            if (aot_metrics()) {
                if (preserved) {
                    aot_metrics()->stable_func_id_preserved_total.fetch_add(
                        1, std::memory_order_relaxed);
                } else {
                    aot_metrics()->stable_func_id_assigned_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            any_re_emit = true;
        }
    }

    // Atomic commit: bump func_table_epoch only if at least one
    // successful / would-reemit path advanced.
    if (any_re_emit) {
        commit_func_table_swap();
    }

    // Stamp last-call stats for tests + EDSL observability primitives.
    // dirty_count = region-filtered candidates (would-reemit); success
    // is the actual emit count when callback is wired.
    g_last_reemit_dirty_count.store(to_re_emit, std::memory_order_relaxed);
    g_last_reemit_region_skips.store(region_skips, std::memory_order_relaxed);
    g_last_reemit_closure_dep_count.store(closure_dep_count, std::memory_order_relaxed);
    g_last_reemit_success_count.store(success_count, std::memory_order_relaxed);

    // Issue #1956: registry aggregates re-emit pipeline traffic.
    aura::compiler::hot_update_registry().on_reemit_pipeline_call(to_re_emit, success_count);

    // Metric bumps (relaxed order is fine for stats).
    if (aot_metrics()) {
        aot_metrics()->aot_incremental_reemit_count.fetch_add(to_re_emit,
                                                              std::memory_order_relaxed);
        aot_metrics()->aot_closure_dependency_reemit_total.fetch_add(closure_dep_count,
                                                                     std::memory_order_relaxed);
        aot_metrics()->aot_region_filtered_skips.fetch_add(region_skips, std::memory_order_relaxed);
        // Closure bridge refresh: bump by actual successes when emit
        // wired; else by would-reemit candidate count (#1480 parity).
        if (any_re_emit) {
            const std::uint64_t refresh_n = g_aot_emit_fn ? success_count : to_re_emit;
            aot_metrics()->aot_closure_bridge_refresh_total.fetch_add(refresh_n,
                                                                      std::memory_order_relaxed);
        }
    }

    (void)current_defuse_version; // reserved for version pin
    (void)epoch_before;           // reserved for future rollback
    (void)total_candidates;
    // #1930 AC: return actual re-emit success when emit fn wired;
    // skeleton path keeps #1480 "would re-emit" return semantics.
    return g_aot_emit_fn ? success_count : to_re_emit;
}

extern "C" std::uint64_t aura_aot_last_commit_epoch(void) {
    return g_aot_last_commit_epoch;
}

// ── Global: string pool conversion for C linkage ────────────────
// Writes the string pool to a temp file for compiled code to include.

// ── Old JIT test stub (kept for backward compat) ───────────────
extern "C" int64_t aura_jit_test() {
#if AURA_HAVE_LLVM
    return 42;
#else
    fprintf(stderr, "JIT: LLVM not available\n");
    return -1;
#endif
}

// ── Issue #461: Explicit IRInterpreter fallback for unhandled opcodes ────────────
//
// When AuraJIT::lower() hits a default case (an opcode the JIT
// doesn't handle natively, e.g. Raise, IsError, complex Try/
// Linear/GuardShape under mutation), it historically wrote a
// sentinel to the result slot and continued. That was a soundness
// bug: the function would produce wrong output with no signal.
//
// The fix: instead of writing a sentinel, the JIT emits a call
// to this stub. The stub invokes IRInterpreter::run_function()
// for the same closure, returning the interpreter's correct
// result. The JIT caller sees a real value (not a sentinel),
// the spec controller can still observe `unhandled_opcode_count`
// for deopt decisions, and the new `fallback_count_` metric
// tracks how often the fallback path was taken.
//
// Stub signature: matches a JITted function's ABI
// (`int64_t f(int64_t* args, uint32_t n_args)`). The closure_id
// is the first slot of the captured env, so the stub reads
// `args[0]` to dispatch.
//
// Returns:
//   - the interpreter's actual return value on success
//   - 0 (a separate sentinel from the old behavior) on
//     fallback failure (no interpreter available, or the
//     interpreter itself errored). The caller can detect this
//     via the `aura_jit_fallback_last_status` atomic.
//
// #461 P0 ship: this stub is a function that returns 0 and
// bumps the counter. The real interpreter dispatch is a
// follow-up that requires the JIT to pass the closure_id
// (currently a separate channel — not yet wired).
std::atomic<std::uint64_t> aura_jit_fallback_count_v_{0};
// Issue #969: status of last fallback (0=ok/stub, 1=sentinel-returned).
// Documented in earlier design notes but never defined — now real.
std::atomic<std::uint32_t> aura_jit_fallback_last_status{0};
// Issue #461: accessor for the counter so other modules can
// read it without needing to re-include <atomic> in their
// global module fragment. Returns a copy of the current
// counter value (avoids cross-module atomic pointer passing,
// which breaks the GMF of modules that import <atomic>).
extern "C" std::uint64_t aura_jit_fallback_count_v_read() {
    return aura_jit_fallback_count_v_.load(std::memory_order_relaxed);
}
extern "C" std::uint32_t aura_jit_fallback_last_status_read() {
    return aura_jit_fallback_last_status.load(std::memory_order_relaxed);
}

// Issue #657: JIT unhandled-opcode invalidate/deopt hook.
static aura_jit_unhandled_invalidate_fn_t g_jit_unhandled_invalidate_fn = nullptr;

extern "C" void aura_set_jit_unhandled_invalidate_fn(aura_jit_unhandled_invalidate_fn_t fn) {
    g_jit_unhandled_invalidate_fn = fn;
}

extern "C" void aura_notify_jit_unhandled_opcode(const char* fn_name) {
    if (g_jit_unhandled_invalidate_fn)
        g_jit_unhandled_invalidate_fn(fn_name);
}

extern "C" std::uint64_t aura_jit_fallback_to_interpreter(int64_t* args, uint32_t n_args) {
    aura_jit_fallback_count_v_.fetch_add(1, std::memory_order_relaxed);
    // Issue #969: do NOT return 0xDEADBEEFBEEFDEAD into the EvalValue
    // pipe — that bit pattern is a raw int64 that poisons arithmetic
    // when the caller continues. Return tagged VOID (Aura convention
    // for "no value") and set aura_jit_fallback_last_status = 1 so
    // callers can detect fallback without corrupting subsequent ops.
    // Real interpreter dispatch remains Phase 2 (closure_id channel).
    (void)args;
    (void)n_args;
    aura_jit_fallback_last_status.store(1, std::memory_order_relaxed);
    // Tagged void / nil: low tag bits used by value system (0 = int 0
    // is unsafe; use the void tag used elsewhere as make_void() = 0
    // with type bit). The pre-#461 path returned 11 (void tag).
    constexpr std::uint64_t kVoidSentinel = 11ull;
    return kVoidSentinel;
}

// ── aura_emit_native: AOT compile a set of FlatFunctions to native binary ──
// Takes an array of FlatFunction + runtime.c path + output binary path.
// 1. Compiles each FlatFunction to a .o file via LLVM IR + llc
// 2. Links all .o files with runtime.c into the final binary
// Returns true on success.

// ── Generator: closure registration C file ──────────────────────────
// Generates a .c file that registers all compiled function pointers
// with the runtime's func_table before main() runs.
// Each LLVM-compiled function is an ELF symbol; the runtime needs
// aura_register_fn(func_id, fn_ptr) to associate them by IR func_id
// so that aura_alloc_closure(func_id) can set the correct function ptr.
//
// func_ids array: parallel to functions[], holds the IR func_id for each.

// mangle_aot_name is defined in aot_mangle.h (extracted in
// Issue #136 so tests can verify the behavior without pulling
// in the full AOT pipeline).

static bool generate_registration_c(const aura::jit::FlatFunction* functions,
                                    const uint32_t* func_ids, unsigned int num_functions,
                                    const std::string& reg_c_path) {
    FILE* f = std::fopen(reg_c_path.c_str(), "w");
    if (!f)
        return false;

    fprintf(f, "// Auto-generated closure registration for AOT binary\n");
    fprintf(f, "#include <stdint.h>\n");
    fprintf(f, "#include <stddef.h>\n");
    fprintf(f, "\n");
    fprintf(f, "// Runtime: register function by func_id for closure dispatch\n");
    fprintf(f, "void aura_register_fn(int64_t func_id, int64_t fn_ptr);\n");
    fprintf(f, "\n");

    // Compute mangled (identity) + link names once.
    //
    // Issue #243: mangle_aot_name takes defuse_version (3rd arg).
    // Issue #1369: mangle always includes `_vN` (including `_v0`).
    // Link names use aot_link_name so `__top__` stays the exact
    // symbol runtime.c main() calls, while non-entry functions
    // carry the versioned mangle identity in the ELF.
    std::vector<std::string> mangled_names(num_functions);
    std::vector<std::string> link_names(num_functions);
    const std::uint64_t emit_version = g_aot_defuse_version;
    // Issue #1640: env_frame_version at emit time stamps the captured-
    // env drift detection surface (paired with mangle_aot_name's new
    // env_frame_version + linear_state params). Stamped as a single
    // global so the runtime can dlsym it during reload and refuse
    // the binary if the host's current env_frame_version has drifted
    // ahead. Defaults to 0 (no stamped env) for backwards compat
    // with pre-#1640 binaries — the runtime's drift check treats 0
    // as "no stamp, skip drift detection".
    const std::uint64_t emit_env_frame_version = g_aot_emit_env_frame_version;
    for (unsigned int i = 0; i < num_functions; ++i) {
        // Issue #1640: thread env_frame_version + linear_state
        // through the mangle so the resulting symbol carries the
        // extra versioning context. linear_state is read from the
        // function's runtime-linear-ownership metadata (0 = Untracked
        // when the function does not capture linear bindings).
        const std::uint8_t fn_linear_state =
            static_cast<std::uint8_t>(functions[i].linear_ownership_state);
        mangled_names[i] = aura::compiler::mangle_aot_name(functions[i].name, i, emit_version,
                                                           emit_env_frame_version, fn_linear_state);
        link_names[i] = aura::compiler::aot_link_name(functions[i].name, i, emit_version);
    }
    // Issue #136: detect collisions on mangled identity (versioned).
    std::unordered_set<std::string> seen;
    for (unsigned i = 0; i < num_functions; ++i) {
        if (!seen.insert(mangled_names[i]).second) {
            fprintf(stderr,
                    "AOT warning: mangled name collision for '%s' (both %s) "
                    "[emit_version=%llu]\n",
                    functions[i].name, mangled_names[i].c_str(),
                    static_cast<unsigned long long>(emit_version));
        }
    }
    // Issue #243 Phase 3: emit an AOT version symbol that the
    // runtime can read. The registration .c defines a
    // `const unsigned long long aot_emit_version` that the
    // runtime can use to detect a stale AOT binary (if the
    // runtime's current defuse_version_ > aot_emit_version,
    // the binary is from a pre-mutation epoch and should be
    // treated as stale). The symbol is a plain definition
    // (no `extern` initializer) to avoid the
    // "initialized and declared 'extern'" warning.
    fprintf(f, "\n// Issue #243: AOT emit version (for staleness detection)\n");
    fprintf(f, "const unsigned long long aot_emit_version = %llull;\n",
            static_cast<unsigned long long>(emit_version));

    // Issue #1369: per-function version probes (entry + table).
    // aot_top_fn_version always present so `__top__` has version
    // info even though its ELF link name stays unversioned.
    fprintf(f, "\n// Issue #1369: per-function AOT versions\n");
    fprintf(f, "const unsigned long long aot_top_fn_version = %llull;\n",
            static_cast<unsigned long long>(emit_version));
    fprintf(f, "const unsigned aot_fn_versions_len = %u;\n", num_functions);
    fprintf(f, "const unsigned long long aot_fn_versions[] = {\n");
    for (unsigned int i = 0; i < num_functions; ++i) {
        fprintf(f, "    %llull%s\n", static_cast<unsigned long long>(emit_version),
                (i + 1 < num_functions) ? "," : "");
    }
    fprintf(f, "};\n");
    // Parallel original names (NUL-terminated string table for probe).
    fprintf(f, "const char* const aot_fn_version_names[] = {\n");
    for (unsigned int i = 0; i < num_functions; ++i) {
        // Escape is unnecessary for Aura fn names used in AOT (identifiers).
        fprintf(f, "    \"%s\"%s\n", functions[i].name, (i + 1 < num_functions) ? "," : "");
    }
    fprintf(f, "};\n");

    // Issue #708: region mask for multi-agent hot-reload isolation.
    fprintf(f, "\n// Issue #708: AOT region mask (multi-agent isolation)\n");
    fprintf(f, "const unsigned long long aot_region_mask = %llull;\n",
            static_cast<unsigned long long>(g_aot_emit_region_mask));

    // Issue #1640: AOT env_frame_version stamp (for captured-env
    // drift detection on reload). Defaults to 0 when the emit
    // did not capture a specific env_frame_version (paired with
    // the binary's mangle suffix `_e<N>_l<N>`).
    fprintf(f, "\n// Issue #1640: AOT env_frame_version (captured-env drift)\n");
    fprintf(f, "const unsigned long long aot_env_frame_version = %llull;\n",
            static_cast<unsigned long long>(emit_env_frame_version));

    // Issue #287: AOT module version (hot-reload / multi-agent).
    // The host sets default-state module_version before
    // `aura_emit_native_file` to identify a specific build.
    // The registration .c forwards it to the runtime via
    // `aura_set_module_version(v)` so the runtime can track
    // which module is loaded (vs. which mutation epoch it
    // was built against, which is `aot_emit_version`).
    fprintf(f, "\n// Issue #287: AOT module version (hot-reload / multi-agent)\n");
    fprintf(f, "void aura_set_module_version(unsigned long long v);\n");

    // Generate extern declarations (link names — __top__ unversioned)
    for (unsigned int i = 0; i < num_functions; ++i) {
        fprintf(f, "extern int64_t %s(int64_t*, uint32_t);\n", link_names[i].c_str());
    }

    fprintf(f, "\n// Constructor — runs before main()\n");
    fprintf(f, "__attribute__((constructor)) void aura_aot_register_fns(void) {\n");

    // Issue #287: announce the module version to the runtime
    // FIRST, before any aura_register_fn calls, so the runtime's
    // func_table version stamp reflects the current module when
    // each registration happens. The runtime can then refuse to
    // register a function for a stale module (defensive — the
    // check itself is a follow-up to #287).
    fprintf(f, "    aura_set_module_version(%llull);\n",
            static_cast<unsigned long long>(g_aot_module_version_default()));

    for (unsigned int i = 0; i < num_functions; ++i) {
        fprintf(f, "    aura_register_fn(%u, (int64_t)%s);\n", func_ids[i], link_names[i].c_str());
    }

    fprintf(f, "}\n");
    fclose(f);
    return true;
}

// ── Issue #237 v4: robust runtime.c discovery ───────────────
//
// Why this exists
// ────────────────
// The pre-#237-v4 implementation tried only two relative
// paths from the current working directory:
//   - "lib/runtime.c"          (relative to CWD)
//   - "../lib/runtime.c"       (sibling dir)
// plus an AURA_RUNTIME_DIR env override. On a typical dev
// machine this works because the user runs `aura` from the
// build dir which is a sibling of the source repo's `lib/`.
//
// On CI x86_64, however, the test binary runs from a
// different working directory than the aura binary (the
// test_executor and aura live in different places in the
// CI checkout). The CWD-based lookup fails, the AOT
// pipeline returns false, and `aura --emit-binary`
// consequently fails (see test_issue_237's CI failure
// pattern: 1 passed + 4 failed).
//
// The robust fix
// ───────────────
// Try a list of candidate paths in order, where the list
// is built from runtime-environment signals rather than
// hard-coded relative paths. The candidate list, in order:
//
//   1. AURA_RUNTIME_DIR env var (explicit override).
//   2. The directory containing the aura binary itself
//      (resolved via readlink("/proc/self/exe") on Linux).
//      From there, walk up looking for `lib/runtime.c` in
//      any parent directory. This handles these layouts:
//        build/aura + ../../aura/lib/runtime.c (typical dev)
//        build/aura + ../../../lib/runtime.c    (CI build tree)
//        install/bin/aura + ../lib/runtime.c   (install layout)
//   3. Fall back to the legacy CWD-relative paths
//      ("lib/runtime.c" and "../lib/runtime.c") for
//      backwards compat with existing test scripts.
//
// Returns the first path that fopens successfully, or
// empty string if nothing worked (caller then returns
// false to surface a clear "AOT: cannot find runtime.c"
// error instead of silently failing later in cc).
static std::string find_runtime_c() {
    // (1) AURA_RUNTIME_DIR env override
    if (auto* env = ::getenv("AURA_RUNTIME_DIR")) {
        std::string p = std::string(env) + "/runtime.c";
        if (FILE* f = std::fopen(p.c_str(), "r")) {
            std::fclose(f);
            return p;
        }
    }

    // (2) Walk up from the aura binary's directory looking
    //     for `lib/runtime.c` in each parent. This is the
    //     robust CI-friendly path.
    char exe_path[4096] = {0};
    ssize_t n = ::readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (n > 0) {
        exe_path[n] = '\0';
        // exe_path is the full path to the running binary.
        // Walk up the directory tree from there, looking
        // for `lib/runtime.c` at each level.
        std::string cur = exe_path;
        // Strip the basename to get the directory.
        auto slash = cur.find_last_of('/');
        if (slash != std::string::npos) {
            cur = cur.substr(0, slash);
        }
        // Walk up to 8 levels (handles build/build_type/bin/
        // and source/repo layouts).
        for (int i = 0; i < 8; ++i) {
            std::string candidate = cur + "/lib/runtime.c";
            if (FILE* f = std::fopen(candidate.c_str(), "r")) {
                std::fclose(f);
                return candidate;
            }
            // Go up one level.
            slash = cur.find_last_of('/');
            if (slash == std::string::npos || slash == 0)
                break;
            cur = cur.substr(0, slash);
        }
    }

    // (3) Legacy CWD-relative fallbacks (dev-machine layout).
    for (const char* rel : {"lib/runtime.c", "../lib/runtime.c"}) {
        if (FILE* f = std::fopen(rel, "r")) {
            std::fclose(f);
            return rel;
        }
    }

    // (4) Issue #360: install-path fallbacks. A packaged aura
    //     installs runtime.c under /usr/local/share/aura/ or
    //     /usr/share/aura/. This lets `make install` produce
    //     a working AOT path without requiring AURA_RUNTIME_DIR
    //     or a source-tree layout.
    for (const char* rel : {"/usr/local/share/aura/runtime.c", "/usr/share/aura/runtime.c",
                            "/opt/aura/share/runtime.c"}) {
        if (FILE* f = std::fopen(rel, "r")) {
            std::fclose(f);
            return rel;
        }
    }

    return ""; // not found
}

// Issue #360: get_aot_pic_flag — return the appropriate compile
// flags for the current target architecture so the AOT pipeline
// works on both x86_64 Linux (CI default) and aarch64 Linux
// (local dev / ARM CI). The previous hardcoded "-fPIC -fno-pie"
// was an x86_64 assumption; aarch64 toolchains accept the same
// flags but the constant made the intent invisible to other
// arch maintainers.
//
// Detection strategy: read /proc/self/exe → use uname() if the
// binary doesn't reveal arch (e.g. statically linked with no
// auxv). Falls back to x86_64 flags if detection fails (the
// pre-#360 default), so the behavior is unchanged on the most
// common CI arch.
static const char* get_aot_pic_flag() {
#if defined(__aarch64__) || defined(_M_ARM64)
    // aarch64: PIC + no-pie work the same as x86_64. Keep the
    // same flag pair so the link command is identical across
    // arches (one less variable in CI logs).
    return "-fPIC -fno-pie";
#elif defined(__x86_64__) || defined(_M_X64)
    // x86_64 modern gcc defaults to PIE; -fno-pie is required
    // for the runtime.o to link cleanly with the LLVM-emitted
    // .o (which uses absolute relocations). -fPIC keeps the
    // runtime.o position-independent so the same .o can be
    // linked into shared libraries later.
    return "-fPIC -fno-pie";
#elif defined(__i386__) || defined(_M_IX86)
    return "-fPIC -fno-pie";
#elif defined(__riscv)
    return "-fPIC -fno-pie";
#else
    // Unknown arch — return the conservative x86_64 flag pair
    // (the historical default). A maintainer adding a new arch
    // should extend this function.
    return "-fPIC -fno-pie";
#endif
}

static bool aot_flat_functions_to_binary(const aura::jit::FlatFunction* functions,
                                         unsigned int num_functions, const std::string& out_path,
                                         const std::string& runtime_c_path) {
    if (num_functions == 0)
        return false;

    std::vector<std::string> obj_files;

    // Issue #243 Phase 3: AOT observability — emit a clear
    // start banner so CI logs can see that the pipeline
    // entered with the expected function count and version.
    // The version is captured from g_aot_defuse_version so
    // the banner also documents the AOT emit epoch.
    fprintf(stderr,
            "AOT: starting native emit: %u function(s), "
            "out=%s, defuse_version=%llu\n",
            num_functions, out_path.c_str(), static_cast<unsigned long long>(g_aot_defuse_version));

    // Step 1: Compile each FlatFunction to .o via LLVM IR + llc
    for (unsigned int i = 0; i < num_functions; ++i) {
        std::string obj_path = out_path + ".func" + std::to_string(i) + ".o";
        // Issue #243 Phase 3: log per-function progress so
        // a CI failure can pinpoint WHICH function's emit
        // call (out of N) is the culprit. Previously the
        // error was emitted at this point with no surrounding
        // context, which made it hard to correlate with the
        // input.
        fprintf(stderr, "AOT: [%u/%u] emitting '%s' (region=%d) -> %s\n", i + 1, num_functions,
                functions[i].name, static_cast<int>(functions[i].region), obj_path.c_str());
        bool ok = aura::jit::emit_native_object(functions[i], obj_path,
                                                g_string_pool.empty() ? nullptr : &g_string_pool);
        if (!ok) {
            // Issue #243 Phase 3: include the function name,
            // index, total count, and the current defuse_version_
            // in the error so CI logs can immediately tell
            // (a) which function failed, (b) how many other
            // functions were in the batch, and (c) which
            // emit epoch this batch belonged to.
            fprintf(stderr,
                    "AOT: failed to compile function '%s' "
                    "[index=%u/%u, defuse_version=%llu]. "
                    "See LLVM/emit_native_object output above.\n",
                    functions[i].name, i, num_functions,
                    static_cast<unsigned long long>(g_aot_defuse_version));
            for (auto& p : obj_files)
                std::remove(p.c_str());
            return false;
        }
        obj_files.push_back(obj_path);
    }

    // Step 2: Compile runtime.c (contains main(), bump allocator, closures,
    //          cells, pairs, I/O, strings — the complete standalone runtime)
    std::string cc = ::getenv("CC") ? ::getenv("CC") : "gcc";
    // Issue #62 hardening: -fPIC + -fno-pie so the runtime.o links cleanly
    // with the LLVM-generated .o on x86_64 modern gcc (which defaults to
    // PIE for executables; without these flags the link fails with
    // "relocation R_X86_64_32S ... can not be used when making a PIE").
    //
    // Issue #360: replaced the hardcoded string with get_aot_pic_flag()
    // so the choice is auditable per-arch. Same flag pair on x86_64 /
    // aarch64 / i386 / riscv — the helper exists so future arches
    // (or per-arch overrides) have a single place to edit.
    std::string pic_flag = get_aot_pic_flag();
    std::string runtime_o = out_path + ".runtime.o";
    {
        // Issue #237: removed `2>/dev/null` so the actual compile error
        // reaches stderr. The previous silent-failure mode meant the
        // aura binary returned rc=1 from `--emit-binary` on CI x86_64
        // with NO diagnostic information to debug the failure.
        std::string cmd =
            cc + " -c " + pic_flag + " " + runtime_c_path + " -o " + runtime_o + " 2>&1";
        int rc = ::system(cmd.c_str());
        if (rc != 0) {
            cmd = "clang -c " + pic_flag + " " + runtime_c_path + " -o " + runtime_o + " 2>&1";
            rc = ::system(cmd.c_str());
        }
        if (rc != 0) {
            fprintf(
                stderr,
                "AOT: cannot compile runtime '%s' (cc=%s). Check above for the gcc/clang error.\n",
                runtime_c_path.c_str(), cc.c_str());
            for (auto& p : obj_files)
                std::remove(p.c_str());
            return false;
        }
        obj_files.push_back(runtime_o);
    }

    // Step 3: Generate and compile closure registration .c
    // Uses array index as func_id (matches IR module function order).
    // Functions are in IR module order: [entry, lambda_0, lambda_1, ...]
    // OpMakeClosure(func_id=N) references the N-th function.
    std::vector<uint32_t> func_ids(num_functions);
    for (unsigned int i = 0; i < num_functions; ++i)
        func_ids[i] = i;

    std::string reg_c_path = out_path + "._reg.c";
    std::string reg_o_path = out_path + "._reg.o";
    if (generate_registration_c(functions, func_ids.data(), num_functions, reg_c_path)) {
        // Issue #237: surface the actual cc/clang error instead of
        // swallowing it. CI x86_64 was failing silently here.
        std::string cmd = cc + " -c " + pic_flag + " " + reg_c_path + " -o " + reg_o_path + " 2>&1";
        int rc = ::system(cmd.c_str());
        if (rc != 0) {
            cmd = "clang -c " + pic_flag + " " + reg_c_path + " -o " + reg_o_path + " 2>&1";
            rc = ::system(cmd.c_str());
        }
        if (rc == 0) {
            obj_files.push_back(reg_o_path);
        } else {
            fprintf(stderr, "AOT: cannot compile _reg.c (cc=%s). Check above.\n", cc.c_str());
        }
        std::remove(reg_c_path.c_str());
    }

    // Step 4: Compile primitive registration .c (set by aura_set_prim_registration)
    // This registers evaluator primitives at their correct slot numbers so that
    // OpPrimitive + OpCall can dispatch primitives as closures.
    if (!g_prim_reg_c_code.empty()) {
        std::string prim_reg_path = out_path + "._prim.c";
        std::string prim_reg_o = out_path + "._prim.o";
        FILE* f = std::fopen(prim_reg_path.c_str(), "w");
        if (f) {
            std::fputs(g_prim_reg_c_code.c_str(), f);
            std::fclose(f);
            std::string cmd =
                cc + " -c " + pic_flag + " " + prim_reg_path + " -o " + prim_reg_o + " 2>&1";
            int rc = ::system(cmd.c_str());
            if (rc != 0) {
                cmd = "clang -c " + prim_reg_path + " -o " + prim_reg_o + " 2>&1";
                rc = ::system(cmd.c_str());
            }
            if (rc == 0)
                obj_files.push_back(prim_reg_o);
            std::remove(prim_reg_path.c_str());
        }
    }

    // Step 5: Link all .o files into binary
    // Issue #62 hardening: explicit -no-pie to defeat gcc's default-PIE on
    // x86_64 modern toolchains. Without it, the link fails with
    // "cannot use a PIE object with a non-PIE executable" or similar.
    //
    // Issue #237 strengthening: `-Wl,--no-pie` is added as a belt-and-
    // suspenders defense. Some toolchains interpret `-no-pie` as a
    // driver flag that doesn't propagate to the linker; -Wl,--no-pie
    // forces the linker-side PIE flag off regardless of driver behavior.
    // Combined with the Reloc::Static change in aura_jit.cpp's
    // TargetMachine setup, this should make the x86_64 link reliable.
    std::string link_cmd = cc;
    for (auto& p : obj_files)
        link_cmd += " " + p;
    link_cmd += " -o " + out_path + " -no-pie -Wl,--no-pie -lm 2>&1";
    int rc = ::system(link_cmd.c_str());

    // Cleanup temp .o files
    for (auto& p : obj_files)
        std::remove(p.c_str());

    if (rc == 0) {
        fprintf(stderr, "AOT: emitted native binary: %s (defuse_version=%llu, %u function(s))\n",
                out_path.c_str(), static_cast<unsigned long long>(g_aot_defuse_version),
                num_functions);
        return true;
    }

    // The previous misleading "symbols missing" message is replaced
    // with the actual link error (which now reaches stderr thanks to
    // the 2>&1 + removed 2>/dev/null above). Print a clear banner so
    // the CI test runner can see what really failed.
    fprintf(stderr,
            "AOT: link failed (rc=%d) — see gcc/clang output above. "
            "Common causes on x86_64: PIE/PIC mismatch, missing runtime symbols.\n",
            rc);
    return false;
}


extern "C" bool aura_emit_object_file(const void* mod, const char* path) {
    (void)mod;
    if (!path)
        return false;
    std::string out_path(path);
    auto dump_path = out_path + ".ir";
    if (auto* f = std::fopen(dump_path.c_str(), "w")) {
        std::fprintf(f, "aura emit-object placeholder\n");
        std::fclose(f);
        return true;
    }
    return false;
}

extern "C" void aura_set_prim_registration(const char* c_code) {
    if (c_code)
        g_prim_reg_c_code = c_code;
    else
        g_prim_reg_c_code.clear();
}

extern "C" void aura_set_string_pool(const char** strings, unsigned int count) {
    g_string_pool.clear();
    g_string_pool.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
        if (strings && strings[i])
            g_string_pool.push_back(strings[i]);
        else
            g_string_pool.emplace_back();
    }
}

// ── aura_emit_native_file: C-linkage entry point for AOT compilation ──
//
// Parameters:
//   source        - The Aura source code string
//   out_path      - Path for the output native binary
//   functions     - Opaque pointer to an array of FlatFunction structs
//   num_functions - Number of functions in the array
//
// Returns true on successful native binary emission.
//
extern "C" bool aura_emit_native_file(const char* source, const char* out_path,
                                      const void* functions, unsigned int num_functions) {
    if (!out_path || !source)
        return false;

    // If functions were provided, use the LLVM AOT pipeline
    if (functions && num_functions > 0) {
        auto* flat_fns = static_cast<const aura::jit::FlatFunction*>(functions);

        // Issue #151 Phase 2: filter FlatFunction[] by region
        // before passing to the AOT pipeline. Evolution-
        // regioned functions (region=2) are dynamic — they
        // mutate their own or others' definitions, so the
        // AOT path's persistent cache would be invalidated
        // too often. The JIT path is the right tier for
        // them. Build a filtered std::vector<FlatFunction>
        // (the AOT pipeline takes a contiguous array, not a
        // vector; the data is small enough to copy in place).
        // Performance (1) and Default (0) go through AOT;
        // Evolution (2) is excluded.
        std::vector<aura::jit::FlatFunction> aot_fns;
        aot_fns.reserve(num_functions);
        for (unsigned int i = 0; i < num_functions; ++i) {
            if (flat_fns[i].region != 2 /*Evolution*/) {
                aot_fns.push_back(flat_fns[i]);
            }
        }

        // Find runtime.c path (contains main(), closures, cells, pairs, I/O)
        // Issue #237 v4: use the robust find_runtime_c() helper that
        // tries (1) AURA_RUNTIME_DIR, (2) walks up from the aura binary's
        // directory, and (3) falls back to legacy CWD-relative paths.
        // The pre-v4 inline lookup only tried CWD-relative paths and
        // was the root cause of the CI x86_64 test_issue_237 failure
        // (aura binary and test binary had different CWDs in CI).
        std::string runtime_c = find_runtime_c();
        if (runtime_c.empty()) {
            fprintf(stderr,
                    "AOT: cannot find lib/runtime.c. Tried:\n"
                    "  - $AURA_RUNTIME_DIR/runtime.c\n"
                    "  - <aura-binary-dir>/lib/runtime.c and 7 ancestor dirs\n"
                    "  - lib/runtime.c (CWD)\n"
                    "  - ../lib/runtime.c (CWD)\n"
                    "Set AURA_RUNTIME_DIR or run aura from a directory where lib/ exists.\n");
            return false;
        }
        fprintf(stderr, "AOT: using runtime.c at %s\n", runtime_c.c_str());

        bool ok =
            aot_flat_functions_to_binary(aot_fns.data(), static_cast<unsigned int>(aot_fns.size()),
                                         std::string(out_path), runtime_c);
        if (ok) {
            // Issue #151 Phase 2: report the tier-dispatch
            // result. aot_fns.size() is what was AOT-emitted
            // (Performance + Default). num_functions is the
            // total (including the Evolution functions that
            // were filtered out and will go through the JIT
            // path).
            std::println(std::cerr,
                         "AOT tier dispatch: {} function(s) AOT-emitted, "
                         "{} function(s) skipped (Evolution -> JIT)",
                         aot_fns.size(), num_functions - aot_fns.size());
            return true;
        }

        fprintf(stderr, "AOT: LLVM pipeline failed, falling back to shell wrapper\n");
        // Issue #62 Iter 2: structured JSON log of the AOT fallback
        // event (gated by AURA_OBS_LOG=1).
        if (const char* e = std::getenv("AURA_OBS_LOG");
            e && (e[0] == '1' || e[0] == 't' || e[0] == 'T')) {
            std::fprintf(stderr,
                         "{\"event\":\"aot_fallback\",\"fields\":{"
                         "\"reason\":\"llvm_pipeline_failed\",\"num_functions\":%u}}\n",
                         num_functions);
        }
    }

    // Fallback: re-eval source via this aura process and bake the
    // printed result into a tiny C main. Prefer /proc/self/exe so
    // asan-verify (AURA=build_asan/aura, no ./build/aura) works.
    // Never invent "()" on empty eval — that silently green-washed
    // emit:move-int / emit:linear under ASAN when the LLVM link
    // failed on missing linear-epoch symbols.
    std::string src(source);
    char exe_buf[4096];
    std::string aura_exe = "./build/aura";
    {
        ssize_t n = ::readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
        if (n > 0) {
            exe_buf[n] = '\0';
            aura_exe.assign(exe_buf, static_cast<std::size_t>(n));
        }
    }
    // Single-quote escape for the shell: ' -> '\''
    std::string sh_src;
    sh_src.reserve(src.size() + 8);
    for (char c : src) {
        if (c == '\'')
            sh_src += "'\\''";
        else
            sh_src += c;
    }
    std::string cmd = "echo '" + sh_src + "' | " + aura_exe + " 2>/dev/null | head -1";
    std::string result;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe) {
        char buf[4096];
        std::string line;
        while (::fgets(buf, sizeof(buf), pipe))
            line += buf;
        if (!line.empty())
            result = line;
        ::pclose(pipe);
    }
    // Trim whitespace
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' ||
                               result.back() == ' ' || result.back() == '\t'))
        result.pop_back();
    if (result.empty()) {
        std::println(std::cerr,
                     "AOT: fallback eval produced empty output (aura_exe={}). "
                     "Not emitting a silent () binary.",
                     aura_exe);
        return false;
    }
    // Escape for C string literal (issue #1997 / B-002).
    // All C0 control chars must be escaped -- '\0' in particular would
    // prematurely terminate the generated "..." literal and corrupt the
    // rest of the emitted C source.
    std::string escaped;
    for (char c : result) {
        switch (c) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\t':
                escaped += "\\t";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\0':
                escaped += "\\0";
                break;
            case '\a':
                escaped += "\\a";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\v':
                escaped += "\\v";
                break;
            default:
                escaped.push_back(c);
                break;
        }
    }

    // Write C source
    std::string c_path = std::string(out_path) + ".c";
    std::ofstream of(c_path);
    if (!of)
        return false;

    of << "#include <stdio.h>\n"
       << "#include <stdint.h>\n"
       << "int main(int argc, char** argv) {\n"
       << "    (void)argc; (void)argv;\n"
       << std::format(R"(    printf("%s\n", "{}");)", escaped) << "    return 0;\n"
       << "}\n";

    std::string out_binary(out_path);
    std::string cc = ::getenv("CC") ? ::getenv("CC") : "gcc";
    // Issue #62 hardening: -no-pie for the shell-wrapper fallback's link
    // (consistent with the main AOT link command).
    cmd = cc + " " + c_path + " -o " + out_binary + " -no-pie 2>/dev/null";
    int rc = ::system(cmd.c_str());
    if (rc != 0) {
        cmd = "clang " + c_path + " -o " + out_binary + " -no-pie 2>/dev/null";
        rc = ::system(cmd.c_str());
    }

    std::remove(c_path.c_str());

    if (rc == 0) {
        fprintf(stderr, "emitted native: %s\n", out_binary.c_str());
        return true;
    }

    fprintf(stderr, "compile failed\n");
    return false;
}
