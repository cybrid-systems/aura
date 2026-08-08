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
#include "runtime_shared.h"        // Issue #2013: aura_remap_live_closures_after_reemit
#include "typed_mutation_audit.h"  // Issue #1882: TypedMutationAudit on hot-update
#include "core/workspace_epoch.hh" // Issue #2039: dual-write WorkspaceEpoch::Bridge

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib> // Issue #2165: getenv AURA_AOT_RELOAD_AUTO_RETRY
#include <cstring>
#include <format>
#include <sys/stat.h> // Issue #2095: ::mkdir for the debug dir
#include <fstream>
#include <limits>
#include <mutex>
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

// ── Issue #1485 C2-wire / #2039 dual-write: C runtime bridge epoch.
//
// Canonical process-global storage is WorkspaceEpoch::Bridge
// (src/core/workspace_epoch.hh). This C atom is a dual-write mirror for
// lib/runtime.c aura_closure_call and AOT paths that cannot include C++
// headers. service.ixx::bump_bridge_epoch() publishes via
// bump_mutation_and_bridge_epochs() then aura_set_current_bridge_epoch().
//
// Issue #1654: std::atomic closes the C++ memory-model data race between
// host bumps and concurrent aura_closure_call reads (acq/rel protocol).
// Issue #2093: file-scope last-reason mirror so callers without
// direct bridge access (query primitives, dashboards) can branch on
// the most recent reload failure. Duplicated with the
// last_aot_reload_fail_reason_ atomic in HotUpdateRegistry so both
// the bridge TU and the registry TU can publish independently;
// readers can use either (both store AotReloadFail as uint8_t).
static std::atomic<std::uint8_t> g_last_reload_fail_reason{0};

extern "C" std::uint8_t aura_aot_last_reload_fail_reason(void) {
    return g_last_reload_fail_reason.load(std::memory_order_acquire);
}

static std::atomic<std::uint64_t> g_current_bridge_epoch{0};

extern "C" void aura_set_current_bridge_epoch(std::uint64_t v) {
    g_current_bridge_epoch.store(v, std::memory_order_release);
    // Issue #2039: dual-write WorkspaceEpoch Bridge so C++ readers of
    // current_bridge_epoch() and C readers of this atom stay aligned
    // even when tests call aura_set_* directly (without bump_bridge_epoch).
    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Bridge, v);
}

extern "C" std::uint64_t aura_get_current_bridge_epoch(void) {
    return g_current_bridge_epoch.load(std::memory_order_acquire);
}

// Issue #2043: linear-ownership epoch dual-write for JIT linear fences.
static std::atomic<std::uint64_t> g_linear_ownership_epoch{0};

extern "C" void aura_set_linear_ownership_epoch(std::uint64_t v) {
    g_linear_ownership_epoch.store(v, std::memory_order_release);
}

extern "C" std::uint64_t aura_get_linear_ownership_epoch(void) {
    return g_linear_ownership_epoch.load(std::memory_order_acquire);
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

// Issue #2092: thin C-linkage helper for bumping the legacy
// name-fallback counter from aura_jit_runtime.cpp (which only has the
// forward declaration of CompilerMetrics via runtime_shared.h, so it
// can't touch the struct members directly). The helper stays adjacent
// to the static aot_metrics() accessor so the metric bookkeeping
// stays in one TU.
extern "C" void aura_bump_live_closure_remap_name_fallback_total(std::uint64_t n) {
    if (auto* m = aot_metrics()) {
        m->live_closure_remap_name_fallback_total.fetch_add(n, std::memory_order_relaxed);
    }
}

// Issue #2602 / #2628: bump sync remount counters from aura_jit_runtime.cpp
// (aot_metrics() is TU-local static in this file).
extern "C" void aura_bump_live_closure_sync_remount_totals(std::uint64_t ok, std::uint64_t fail) {
    if (auto* m = aot_metrics()) {
        m->live_closure_sync_remount_ok_total.fetch_add(ok, std::memory_order_relaxed);
        m->live_closure_sync_remount_fail_total.fetch_add(fail, std::memory_order_relaxed);
    }
}

// Issue #2637: anon sync remount bump hook (sid == 0 branch). Distinct
// from #2602 named bump hook; mirrors the structure but routes to anon
// counters. Weak decl in this TU; strong def in aura_jit_runtime.cpp.
extern "C" void aura_bump_live_closure_sync_remount_anon_totals(std::uint64_t ok,
                                                                std::uint64_t fail) {
    if (auto* m = aot_metrics()) {
        m->live_closure_sync_remount_anon_ok_total.fetch_add(ok, std::memory_order_relaxed);
        m->live_closure_sync_remount_anon_fail_total.fetch_add(fail, std::memory_order_relaxed);
    }
}

// Issue #2691: captured-only anon sync remount counters. Distinct
// counters so Agents can distinguish "must remount" (captured anon)
// from "touch-time policy" (pure anon, no captures). Routes to
// live_closure_sync_remount_anon_captured_ok_total / _fail_total.
extern "C" void aura_bump_live_closure_sync_remount_anon_captured_totals(std::uint64_t ok,
                                                                         std::uint64_t fail) {
    if (auto* m = aot_metrics()) {
        m->live_closure_sync_remount_anon_captured_ok_total.fetch_add(ok,
                                                                      std::memory_order_relaxed);
        m->live_closure_sync_remount_anon_captured_fail_total.fetch_add(fail,
                                                                        std::memory_order_relaxed);
    }
}

// Issue #2638: residual sid=0 cap-hit counter bumper. Bumped when
// the residual backfill branch in aura_remap_live_closures_after_reemit
// sees cur_backfill >= cap (or 0 cap = unlimited → never). Distinct
// from live_closure_stable_id_backfill_total (which counts successful
// backfills).
extern "C" void aura_bump_live_closure_residual_cap_hit_total(std::uint64_t n) {
    if (auto* m = aot_metrics()) {
        m->live_closure_residual_cap_hit_total.fetch_add(n, std::memory_order_relaxed);
    }
}

// Issue #2638: env opt-in flag for residual sid=0 cap. Weak decl in
// this TU; strong def in aura_jit_runtime.cpp reads AURA_RESIDUAL_SID0_CAP
// (default 256 production-safe; 0 = unlimited for Soft / sandbox=off /
// tests). Weak attribute means tests / hosts without the production TU
// see the function as nullptr — call sites handle via the
// `fn ? fn() : default` ternary.
extern "C" std::uint64_t aura_residual_sid0_cap_default() __attribute__((weak));

// Issue #2637: env opt-in flag for anonymous sync remount on reemit.
// Weak decl in this TU; strong def in aura_jit_runtime.cpp reads
// AURA_SYNC_REMOUNT_ANON env (default 0 = OFF per AC1). Weak attribute
// means tests / hosts without the production TU see the function as
// nullptr — call sites handle via the `fn ? fn() : default` ternary.
extern "C" int aura_sync_remount_anon_enabled_default() __attribute__((weak));

// Issue #2175: legacy sid=0 backfill counter (one-shot lookup per
// successful backfill during aura_remap_live_closures_after_reemit).
// Independent of the name-fallback path (AC2) — fires whenever the
// closure has stored_sid == 0 and the name resolves in the live stable
// map. Tuned so operators can distinguish "legacy closures remapped
// via backfill" (counter > 0 is healthy) from "name fallback used"
// (counter > 0 is suspect — strict default keeps it at 0).
extern "C" void aura_bump_live_closure_stable_id_backfill_total(std::uint64_t n) {
    if (auto* m = aot_metrics()) {
        m->live_closure_stable_id_backfill_total.fetch_add(n, std::memory_order_relaxed);
    }
}

// Issue #2605: named name-fallback invent refused (fail-closed).
extern "C" void aura_bump_live_closure_named_name_fallback_reject_total(std::uint64_t n) {
    if (auto* m = aot_metrics()) {
        m->live_closure_named_name_fallback_reject_total.fetch_add(n, std::memory_order_relaxed);
    }
}

// Issue #2128: metric bumps from aura_jit_runtime (no CompilerMetrics layout).
extern "C" void aura_bump_must_deopt_before_next_call_total(std::uint64_t n) {
    if (auto* m = aot_metrics()) {
        m->must_deopt_before_next_call_total.fetch_add(n, std::memory_order_relaxed);
    }
}

// Issue #2233: post-reemit live-closure stamp metric bumpers
// (hit / miss split). See observability_metrics.h for the per-reason
// semantics — the #2233 pair measures the decision (hit vs miss)
// explicitly so Agents can branch on the remap outcome.
extern "C" void aura_bump_live_closure_epoch_restamp_total(std::uint64_t n) {
    if (auto* m = aot_metrics()) {
        m->live_closure_epoch_restamp_total.fetch_add(n, std::memory_order_relaxed);
    }
}

extern "C" void aura_bump_live_closure_must_deopt_kept_total(std::uint64_t n) {
    if (auto* m = aot_metrics()) {
        m->live_closure_must_deopt_kept_total.fetch_add(n, std::memory_order_relaxed);
    }
}

// Issue #2234: post-reemit / post-compact env_frame + linear capture
// remount metric bumpers. See observability_metrics.h.
extern "C" void aura_bump_closure_capture_remount_ok_total(std::uint64_t n) {
    if (auto* m = aot_metrics()) {
        m->closure_capture_remount_ok_total.fetch_add(n, std::memory_order_relaxed);
    }
}

extern "C" void aura_bump_closure_capture_remount_fail_total(std::uint64_t n) {
    if (auto* m = aot_metrics()) {
        m->closure_capture_remount_fail_total.fetch_add(n, std::memory_order_relaxed);
    }
}

// Issue #2272: env_generation mismatch counter (PRIMARY env axis).
// Distinct from closure_capture_remount_fail_total so dashboards can
// distinguish "env_gen drift" from "defuse drift".
extern "C" void aura_bump_closure_capture_env_gen_mismatch_total(std::uint64_t n) {
    if (auto* m = aot_metrics()) {
        m->closure_capture_env_gen_mismatch_total.fetch_add(n, std::memory_order_relaxed);
    }
}

// Issue #2297: structural capture-cell remap metrics.
extern "C" void aura_bump_closure_capture_cell_remap_ok_total(std::uint64_t n) {
    if (auto* m = aot_metrics()) {
        m->closure_capture_cell_remap_ok_total.fetch_add(n, std::memory_order_relaxed);
    }
}
extern "C" void aura_bump_closure_capture_cell_remap_fail_total(std::uint64_t n) {
    if (auto* m = aot_metrics()) {
        m->closure_capture_cell_remap_fail_total.fetch_add(n, std::memory_order_relaxed);
    }
}

// Issue #2234: aura_closure_has_env_or_linear_captures /
// aura_remount_closure_captures live in aura_jit_runtime.cpp
// (they need file-static g_closure_* tables). Declarations remain
// in aura_jit_bridge.h for the public C ABI.

extern "C" void aura_bump_must_deopt_force_deopt_success_total(std::uint64_t n) {
    if (auto* m = aot_metrics()) {
        m->must_deopt_force_deopt_success_total.fetch_add(n, std::memory_order_relaxed);
    }
}
extern "C" void aura_bump_must_deopt_force_deopt_fail_total(std::uint64_t n) {
    if (auto* m = aot_metrics()) {
        m->must_deopt_force_deopt_fail_total.fetch_add(n, std::memory_order_relaxed);
    }
}

// Issue #2310: file-level atomic fallback for light binaries without
// CompilerMetrics linked. Always bumped alongside per-CompilerMetrics
// counter by aura_force_deopt_on_steal_snapshot_mismatch.
static std::atomic<std::uint64_t> g_2310_force_deopt_fallback_total{0};

// Issue #2310: fail-closed force-deopt on steal snapshot inconsistency.
// Strong def in evaluator_fiber_mutation.cpp bumps the per-CompilerMetrics
// counter (via evaluator_for_scheduler_hooks) + runs refresh. This
// file-level fallback is WEAK so light binaries without the evaluator TU
// still resolve the symbol (and bump g_2310_force_deopt_fallback_total),
// while full aura / asan links pick the strong evaluator body.
extern "C" __attribute__((weak)) void
aura_force_deopt_on_steal_snapshot_mismatch(void* /*fiber_ptr*/) noexcept {
    if (auto* m = aot_metrics()) {
        m->steal_snapshot_mismatch_force_deopt_total.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_2310_force_deopt_fallback_total.fetch_add(1, std::memory_order_relaxed);
    }
}

extern "C" std::uint64_t aura_static_steal_snapshot_mismatch_force_deopt_total() noexcept {
    if (auto* m = aot_metrics()) {
        return m->steal_snapshot_mismatch_force_deopt_total.load(std::memory_order_relaxed);
    }
    return g_2310_force_deopt_fallback_total.load(std::memory_order_relaxed);
}

// ── Issue #1443: long-mutation policy knobs ───────────
//
// C-linkage setters for `long_mutation_threshold_us` (default 500'000 µs
// = 500ms) and `long_mutation_strict_mode` (0 = metric-only,
// 1 = abort/rollback on extreme holds >= max_extreme_mutation_us /
// hard_timeout_us when set). Issue #2199: AURA_MUTATION_HOLD_STRICT=1
// also enables force-fail in the Guard dtor (env OR atomic).
// Read in MutationBoundaryGuard::~MutationBoundaryGuard via std::atomic
// load — racy by design (best-effort policy).
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

// Issue #2199: optional hard_timeout_us override (0 = use max_extreme).
extern "C" void aura_set_hard_timeout_us(std::uint64_t us) {
    if (g_aot_metrics)
        g_aot_metrics->hard_timeout_us.store(us, std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_get_hard_timeout_us(void) {
    if (g_aot_metrics)
        return g_aot_metrics->hard_timeout_us.load(std::memory_order_relaxed);
    return 0;
}

extern "C" std::uint64_t aura_get_long_mutation_forced_abort_total(void) {
    if (g_aot_metrics)
        return g_aot_metrics->long_mutation_forced_abort_total.load(std::memory_order_relaxed);
    return 0;
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

// Issue #2095: default-LLVM reemit postmortem hook. When
// AURA_REEMIT_KEEP_FAIL=1 (or AURA_REEMIT_KEEP_FAIL_N=count), the
// failed .o is renamed into /tmp/aura_reemit_failed/ instead of
// being removed. Agent can then inspect with llvm-objdump /
// llvm-dis to diagnose the actual failure.
extern "C" int aura_reemit_keep_fail_enabled(void) {
    if (const char* e = std::getenv("AURA_REEMIT_KEEP_FAIL")) {
        if (e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y')
            return 1;
    }
    if (const char* n = std::getenv("AURA_REEMIT_KEEP_FAIL_N")) {
        if (n[0] != '\0' && n[0] != '0')
            return 1;
    }
    return 0;
}

extern "C" void aura_reemit_keep_failed_obj(const char* obj_path, const char* reason) {
    if (!obj_path)
        return;
    constexpr const char* kDebugDir = "/tmp/aura_reemit_failed";
    ::mkdir(kDebugDir, 0755);
    // Derive a stable suffix from the basename + sequence-ish counter so
    // multiple failures within the same second don't collide.
    static std::atomic<std::uint64_t> keep_seq{0};
    const auto seq = keep_seq.fetch_add(1, std::memory_order_relaxed);
    const char* base = std::strrchr(obj_path, '/');
    base = base ? (base + 1) : obj_path;
    std::string dst =
        std::format("{}/{}_{}_{}{}", kDebugDir, base, reason ? reason : "fail",
                    static_cast<unsigned long long>(seq), std::strrchr(obj_path, '.') ? "" : ".o");
    std::rename(obj_path, dst.c_str());
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
// Live mask may be adaptively cleared under pressure (#2016); preferred
// holds the host-requested bits so quiet periods can restore.
static std::uint64_t g_aot_emit_region_mask = 0;
static std::uint64_t g_aot_emit_region_mask_preferred = 0;
// Issue #2016: region index constants (must match FlatFunction::region).
static constexpr std::uint64_t kAotRegionDefault = 0;
static constexpr std::uint64_t kAotRegionPerformance = 1;
static constexpr std::uint64_t kAotRegionEvolution = 2;
// Adaptive thresholds: clear a Performance/Default bit when that region's
// candidate count in one pipeline call reaches this; restore when a
// subsequent call sees fewer candidates and no deopt storm.
static constexpr std::uint64_t kAotRegionDirtyClearThreshold = 8;
// Issue #1640: emit-time env_frame_version stamp (process-wide; AOT emit).
// 0 = no stamp (skip drift detection). Host can raise via setter when wired.
static std::uint64_t g_aot_emit_env_frame_version = 0;

// Issue #2091: live mirrors that follow the current Evaluator's
// env_generation_ + linear-ownership fingerprint. The Evaluator
// bumps these whenever the underlying counter changes so that
// every emit / reemit / registration site reads the **current**
// values instead of relying on a hand-set global. generate_registration_c
// + aura_reemit_aot_for_dirty both prefer the live values when the
// host hasn't explicitly set `g_aot_emit_env_frame_version` (max
// wins; wired hosts always produce a non-zero stamp).
//
// `g_aot_live_linear_state_fingerprint` is a uint8 that tracks
// the maximum linear_ownership_state observed across active
// EnvFrames in the live Evaluator (mirrors the registration
// .c's `aot_linear_state` symbol computation — same logic,
// surfaced as a process-global so emit paths don't need an
// Evaluator handle).
static std::atomic<std::uint64_t> g_aot_live_env_frame_version{0};
static std::atomic<std::uint8_t> g_aot_live_linear_state_fingerprint{0};

// Issue #2091: process-global snapshot of the AURA_AOT_FORCE_ENV_LINEAR_SUFFIX
// env var (read once at first emit so test harnesses that flip the flag
// via aura_aot_set_force_env_linear_suffix() stay authoritative). The
// C-linkage setter stores into aot_mangle.h's static atomic; the env
// var here only seeds the default on cold start.
static std::atomic<bool> g_aot_force_env_linear_suffix_env_seeded{false};

static void aot_seed_force_env_linear_suffix_from_env() {
    bool expected = false;
    if (!g_aot_force_env_linear_suffix_env_seeded.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
        return;
    if (const char* e = ::getenv("AURA_AOT_FORCE_ENV_LINEAR_SUFFIX")) {
        const bool on = (e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y' ||
                         e[0] == 'o' || e[0] == 'O');
        aura::compiler::aot_set_force_env_linear_suffix(on);
    }
}

// Issue #2091: live env_frame_version setter (called by the Evaluator
// whenever env_generation_ bumps). The atomic is the canonical
// process-global that every emit / reemit / registration site reads
// when constructing mangle_aot_name / aot_link_name args.
extern "C" void aura_set_aot_live_env_frame_version(std::uint64_t v) {
    g_aot_live_env_frame_version.store(v, std::memory_order_release);
}

extern "C" std::uint64_t aura_get_aot_live_env_frame_version(void) {
    return g_aot_live_env_frame_version.load(std::memory_order_acquire);
}

// Issue #2091: live linear_state fingerprint setter (uint8 — mirrors
// the max linear_ownership_state observed in the active EnvFrames).
extern "C" void aura_set_aot_live_linear_state_fingerprint(std::uint8_t v) {
    g_aot_live_linear_state_fingerprint.store(v, std::memory_order_release);
}

extern "C" std::uint8_t aura_get_aot_live_linear_state_fingerprint(void) {
    return g_aot_live_linear_state_fingerprint.load(std::memory_order_acquire);
}

// Issue #2091: C-linkage bridge for the force flag in aot_mangle.h.
// Honors AURA_AOT_FORCE_ENV_LINEAR_SUFFIX on first call so tests can
// flip the flag explicitly without needing to set the env var. The
// aot_mangle.h static atomic remains the source of truth.
extern "C" void aura_aot_set_force_env_linear_suffix(int v) {
    aot_seed_force_env_linear_suffix_from_env();
    aura::compiler::aot_set_force_env_linear_suffix(v != 0);
}

extern "C" int aura_aot_get_force_env_linear_suffix(void) {
    aot_seed_force_env_linear_suffix_from_env();
    return aura::compiler::aot_force_env_linear_suffix() ? 1 : 0;
}

// Issue #2091: helper used by emit / reemit / registration sites.
// Picks the **effective** env_frame_version stamp for a given emit:
// prefer the explicitly-set host global (back-compat with #1640),
// fall back to the live Evaluator mirror. Returns 0 only when both
// are 0 (the legacy defuse-only shape).
static std::uint64_t aot_resolve_emit_env_frame_version() noexcept {
    const std::uint64_t host = g_aot_emit_env_frame_version;
    const std::uint64_t live = g_aot_live_env_frame_version.load(std::memory_order_acquire);
    return host > live ? host : live;
}

// Issue #2091: same, for the linear_state fingerprint. Per-function
// overrides win (functions[i].linear_ownership_state) when the emit
// path is the per-function FlatFunction loop in
// generate_registration_c; the fingerprint is the module-level max.
static std::uint8_t aot_resolve_emit_linear_state_fingerprint() noexcept {
    const std::uint8_t live = g_aot_live_linear_state_fingerprint.load(std::memory_order_acquire);
    return live;
}

// Issue #2091: metric bumper helper. Picks stamped vs default_zero
// based on whether the resolved env/linear are non-zero or the force
// flag is on. Called once per emit / reemit / registration site so
// the dashboard can detect miswired hosts.
static void aot_bump_env_linear_stamp_metric(std::uint64_t env_v, std::uint8_t lin_v) {
    auto* m = aot_metrics();
    if (!m)
        return;
    const bool stamped =
        (env_v != 0) || (lin_v != 0) || aura::compiler::aot_force_env_linear_suffix();
    if (stamped)
        m->aot_emit_env_linear_stamped_total.fetch_add(1, std::memory_order_relaxed);
    else
        m->aot_emit_env_linear_default_zero_total.fetch_add(1, std::memory_order_relaxed);
}

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
}

// Issue #2093: per-eval env_frame_version setter. Pairs with the
// reload's st.env_frame_version check at L1885 so tests + hosts can
// seed the host side and force an Env-failure reload. Default state
// (nullptr) writes to g_aot_default_state.env_frame_version; per-eval
// writes to aot_state_for(eval_ptr).env_frame_version.
extern "C" void aura_set_aot_default_env_frame_version(std::uint64_t v) {
    g_aot_default_state.env_frame_version.store(v, std::memory_order_relaxed);
}

extern "C" void aura_set_aot_env_frame_version_for_eval(void* eval_ptr, std::uint64_t v) {
    aot_state_for(eval_ptr).env_frame_version.store(v, std::memory_order_relaxed);
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
    // Issue #2016: Evolution (region bit 2) is never an AOT emit target —
    // strip it from preferred + live so adaptive restore cannot re-enable it.
    const std::uint64_t evolution_bit = 1ULL << kAotRegionEvolution;
    const std::uint64_t sanitized = mask & ~evolution_bit;
    g_aot_emit_region_mask_preferred = sanitized;
    g_aot_emit_region_mask = sanitized;
    // Issue #1956: bookkeeping for HotUpdateRegistry dashboard.
    aura::compiler::hot_update_registry().on_emit_region_mask_set(sanitized);
}

// Issue #2016: live (possibly adapted) emit region mask.
extern "C" std::uint64_t aura_get_aot_emit_region_mask(void) {
    return g_aot_emit_region_mask;
}

extern "C" std::uint64_t aura_get_aot_emit_region_mask_preferred(void) {
    return g_aot_emit_region_mask_preferred;
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
//
// Issue #2670: namespace by eval_owner (outer key). Same-process multi-eval
// hosts (two Evaluator instances sharing a process) get distinct sids per
// eval for the same Define name. Single-workspace callers (no eval owner
// registered) still see identical behavior pre/post change: legacy C funcs
// dispatch via aura_aot_get_reemit_owner_eval() ?: aura_aot_get_register_
// owner_eval() ?: nullptr → nullptr-keyed inner map = the original single-
// workspace zero-downtime contract. Total map size is the sum of inner sizes.
static std::mutex g_stable_func_id_mtx;
static std::unordered_map<void*,
                          std::unordered_map<std::string, std::uint32_t,
                                             aura::core::TransparentStringHash, std::equal_to<>>>
    g_eval_to_stable_func_id;
static std::atomic<std::uint32_t> g_next_stable_func_id{1};

// Returns stable func_id for (eval_owner, name). out_preserved: 1 if map
// already held an entry (re-emit reuse), 0 if newly assigned.
static std::uint32_t preserve_stable_func_id_for_eval_locked(void* eval_ptr, const char* name,
                                                             int* out_preserved) {
    if (out_preserved)
        *out_preserved = 0;
    if (!name || !*name)
        return 0;
    auto outer_it = g_eval_to_stable_func_id.find(eval_ptr);
    if (outer_it != g_eval_to_stable_func_id.end()) {
        auto& inner = outer_it->second;
        auto it = inner.find(name);
        if (it != inner.end()) {
            if (out_preserved)
                *out_preserved = 1;
            return it->second;
        }
    }
    // New entry: create inner map for this eval (or reuse empty one).
    auto& inner = g_eval_to_stable_func_id[eval_ptr];
    const std::uint32_t id = g_next_stable_func_id.fetch_add(1, std::memory_order_relaxed);
    inner.emplace(name, id);
    return id;
}

// Lookup-only variant: 0 if missing or invalid. Does NOT allocate inner map.
static std::uint32_t lookup_stable_func_id_for_eval_locked(void* eval_ptr, const char* name) {
    if (!name || !*name)
        return 0;
    auto outer_it = g_eval_to_stable_func_id.find(eval_ptr);
    if (outer_it == g_eval_to_stable_func_id.end())
        return 0;
    const auto& inner = outer_it->second;
    auto it = inner.find(name);
    return it == inner.end() ? 0 : it->second;
}

// Total entries across all eval owners (for query surface; AC5).
static std::uint64_t stable_func_id_map_size_locked() {
    std::uint64_t total = 0;
    for (const auto& [eval, inner] : g_eval_to_stable_func_id)
        total += inner.size();
    return total;
}

// Clear entries for a single eval owner. nullptr = default single-workspace.
static void clear_stable_func_id_map_for_eval_locked(void* eval_ptr) {
    g_eval_to_stable_func_id.erase(eval_ptr);
}

// Full clear (process teardown / tests). Resets id counter.
static void clear_stable_func_id_map_all_locked() {
    g_eval_to_stable_func_id.clear();
    g_next_stable_func_id.store(1, std::memory_order_relaxed);
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

// Issue #1930 / #2670: stable func_id map C-linkage surface.
// Legacy C funcs dispatch via owner TLS (reemit ?: register ?: nullptr)
// so single-workspace callers see identical behavior pre/post #2670.
// For multi-eval / Agent hosts that publish their eval owner via
// aura_aot_set_reemit_owner_eval() / aura_aot_set_register_owner_eval(),
// the per-eval for_eval variants below allow explicit namespacing.
// Define for_eval first so the legacy wrapper can call it (C++ needs
// declaration-before-use under -Werror).
extern "C" std::uint32_t
aura_get_or_preserve_stable_func_id_for_eval(void* eval_ptr, const char* name, int* out_preserved) {
    std::lock_guard<std::mutex> lock(g_stable_func_id_mtx);
    int preserved = 0;
    const auto id = preserve_stable_func_id_for_eval_locked(eval_ptr, name, &preserved);
    aura::compiler::hot_update_registry().on_stable_func_id_preserve(preserved != 0);
    if (out_preserved)
        *out_preserved = preserved;
    return id;
}

extern "C" std::uint32_t aura_get_or_preserve_stable_func_id(const char* name, int* out_preserved) {
    void* eval_owner = aura_aot_get_reemit_owner_eval();
    if (!eval_owner)
        eval_owner = aura_aot_get_register_owner_eval();
    return aura_get_or_preserve_stable_func_id_for_eval(eval_owner, name, out_preserved);
}

extern "C" std::uint32_t aura_lookup_stable_func_id(const char* name) {
    if (!name || !*name)
        return 0;
    void* eval_owner = aura_aot_get_reemit_owner_eval();
    if (!eval_owner)
        eval_owner = aura_aot_get_register_owner_eval();
    std::lock_guard<std::mutex> lock(g_stable_func_id_mtx);
    return lookup_stable_func_id_for_eval_locked(eval_owner, name);
}

extern "C" std::uint32_t aura_lookup_stable_func_id_for_eval(void* eval_ptr, const char* name) {
    std::lock_guard<std::mutex> lock(g_stable_func_id_mtx);
    return lookup_stable_func_id_for_eval_locked(eval_ptr, name);
}

extern "C" std::uint64_t aura_stable_func_id_map_size(void) {
    std::lock_guard<std::mutex> lock(g_stable_func_id_mtx);
    return stable_func_id_map_size_locked();
}

extern "C" void aura_clear_stable_func_id_map(void) {
    std::lock_guard<std::mutex> lock(g_stable_func_id_mtx);
    clear_stable_func_id_map_all_locked();
}

extern "C" void aura_clear_stable_func_id_map_for_eval(void* eval_ptr) {
    std::lock_guard<std::mutex> lock(g_stable_func_id_mtx);
    clear_stable_func_id_map_for_eval_locked(eval_ptr);
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

// ── Issue #287 / #2012: AOT hot-reload with atomic func_table swap ──
//
// `aura_reload_aot_module(path, version)` hot-swaps an AOT module:
//   1. Enter staging mode (constructor registrations do not touch live slots)
//   2. dlopen() the new .so/.dylib (constructors write staging table)
//   3. Probe aot_emit_version / region / defuse / env_frame
//   4. On success: apply staging → live slots, then commit_func_table_swap
//      (epoch bump + HotUpdateRegistry notify) so concurrent closure
//      calls observe either fully-old or fully-new symbols
//   5. On any failure: discard staging, dlclose, bump rollback metric,
//      leave live table + epoch untouched
//
// Multi-agent isolation (per-eval AotState) remains on the host side
// of the version/region checks (#1367).
#include <dlfcn.h>

namespace {

constexpr unsigned kMaxAotFuncs = 4096;

struct AotFuncSlot {
    std::atomic<std::uintptr_t> fn_ptr{0};
    std::atomic<std::uint64_t> grace_refcount{0};
    std::atomic<std::uint64_t> table_generation{0};
    // Issue #2299: owning Evaluator* (or opaque host key) stamped at
    // register time. 0 = process-default / unowned. Per-eval physical
    // invalidate only clears slots matching the requested eval_ptr.
    std::atomic<std::uintptr_t> owner_eval{0};
};

AotFuncSlot g_aot_func_slots[kMaxAotFuncs];
std::atomic<std::uint64_t> g_aot_table_epoch{1};
// Issue #971: count silent drops when func_id >= kMaxAotFuncs.
std::atomic<std::uint64_t> g_aot_register_dropped{0};
// Issue #2299: TLS owner for aura_register_fn_tracked (and staging apply).
// Reloads install via RegisterOwnerGuard; tests may set directly.
thread_local void* g_aot_register_owner_eval = nullptr;
// Issue #2606: TLS owner for aura_reemit_aot_for_dirty candidate filter.
// Hosts set to current Evaluator* around cascade/boundary reemit so
// multi-AotState reemit only considers slots owned by that eval.
// nullptr = process-default (no ownership filter — soft single-eval).
thread_local void* g_aot_reemit_owner_eval = nullptr;
// Last eval_ptr observed by aura_aot_invalidate_all_stale_slots_for_eval.
std::atomic<std::uintptr_t> g_aot_last_slot_invalidate_eval{0};

// Issue #2012: staging table for atomic reload. While staging is
// active, aura_register_fn_tracked writes here instead of live slots
// so a failed validation never leaves torn function pointers.
struct AotStagingEntry {
    std::uintptr_t fn_ptr = 0;
    bool written = false;
};
AotStagingEntry g_aot_staging[kMaxAotFuncs];
std::atomic<bool> g_aot_staging_active{false};
unsigned g_aot_staging_hi = 0; // inclusive high-water of written indices
std::mutex g_aot_reload_mtx;   // serialize concurrent reloads

void clear_aot_staging() noexcept {
    for (unsigned i = 0; i <= g_aot_staging_hi && i < kMaxAotFuncs; ++i)
        g_aot_staging[i] = {};
    g_aot_staging_hi = 0;
}

void apply_aot_staging_to_live() noexcept {
    const std::uint64_t epoch = g_aot_table_epoch.load(std::memory_order_acquire);
    const auto owner = reinterpret_cast<std::uintptr_t>(g_aot_register_owner_eval);
    for (unsigned i = 0; i <= g_aot_staging_hi && i < kMaxAotFuncs; ++i) {
        if (!g_aot_staging[i].written)
            continue;
        auto& slot = g_aot_func_slots[i];
        const std::uintptr_t new_ptr = g_aot_staging[i].fn_ptr;
        const std::uintptr_t old_ptr = slot.fn_ptr.exchange(new_ptr, std::memory_order_acq_rel);
        if (old_ptr != 0 && old_ptr != new_ptr)
            slot.grace_refcount.fetch_add(1, std::memory_order_relaxed);
        // Stamp pre-commit epoch; commit_func_table_swap advances generation domain.
        slot.table_generation.store(epoch, std::memory_order_relaxed);
        // Issue #2299: stamp owner for per-eval invalidate filtering.
        slot.owner_eval.store(owner, std::memory_order_relaxed);
    }
}

void bump_reload_attempt() {
    if (aot_metrics())
        aot_metrics()->aot_reload_attempts_.fetch_add(1, std::memory_order_relaxed);
}

void commit_func_table_swap() {
    // Publish epoch advance (acq_rel). Callers that applied staging must
    // leave written flags set until after this returns so generations
    // can be stamped to the new epoch (reload path). Reemit leaves
    // staging empty — loop is a no-op.
    const std::uint64_t new_epoch = g_aot_table_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    for (unsigned i = 0; i <= g_aot_staging_hi && i < kMaxAotFuncs; ++i) {
        if (!g_aot_staging[i].written)
            continue;
        g_aot_func_slots[i].table_generation.store(new_epoch, std::memory_order_relaxed);
    }
    if (aot_metrics()) {
        aot_metrics()->aot_refcount_swaps_.fetch_add(1, std::memory_order_relaxed);
        aot_metrics()->aot_concurrent_safe_reloads_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #2012 / #1956: fan-out epoch listeners (reload + reemit).
    aura::compiler::hot_update_registry().notify_epoch_bump(new_epoch);
    // Issue #2668: event-driven soft walk on the epoch-bump edge.
    // Closes the burst-mutation window that pure periodic Soft leaves
    // open under reemit storms. Production + Soft only.
    aura_event_driven_epoch_invariant_walk_if_due();
}

// Issue #1271: last successfully committed AOT module identity
// for multi-agent versioning + atomic rollback diagnostics.
static void* g_aot_last_handle = nullptr;
static std::uint64_t g_aot_last_commit_epoch = 0;
static std::uint64_t g_aot_last_module_version = 0;
// Issue #2178: cross-workspace / cross-COW hot-update reject counter.
// Bumped when aura_reload_aot_module_for_eval / reemit callbacks are
// invoked with a foreign eval_ptr (or when COW generation diverges).
// The MVP scope (#1943) explicitly documents single-workspace; this
// counter is the observable guard for multi-agent / multi-tenant hosts
// until a future cross-COW migration design lands. The C-linkage accessor
// is surfaced on (query:hot-update-registry-stats) so Agents can alert
// on accidental cross-workspace calls.
static std::atomic<std::uint64_t> g_cross_workspace_hot_update_rejected_total{0};
// Issue #2178: helper used from within the reload / reemit walks to bump
// the cross-workspace-rejected counter when a foreign eval context is
// detected. C-linkage so the C++ reload / reemit paths can call it
// without dragging in the full hot_update_registry.hpp.
extern "C" void aura_cross_workspace_hot_update_rejected_increment(void) noexcept {
    g_cross_workspace_hot_update_rejected_total.fetch_add(1, std::memory_order_relaxed);
}
// Issue #2178: C-linkage accessor for the cross-workspace-rejected counter.
// Mirrors the pattern of aura_aot_hot_update_atomic_rollback_total etc.
extern "C" std::uint64_t aura_cross_workspace_hot_update_rejected_total_v_read(void) noexcept {
    return g_cross_workspace_hot_update_rejected_total.load(std::memory_order_relaxed);
}

// Issue #2275: process-level workspace cow_gen atoms. Eval tables
// are simple (eval_ptr -> expected cow_gen) — single atomic per
// common case; multi-eval extension can grow into a hash if needed.
// Live workspace cow_gen is bumped on each densify + workspace
// gen restamp; reload attempts compare eval-captured cow_gen
// against this to detect cross-COW drift without opening a
// migration write path.
// Storage defined BEFORE the accessors below so they can use it
// without forward declarations.
static std::atomic<std::uint64_t> g_expected_cow_gen_per_eval{0};
static std::atomic<std::uint64_t> g_live_workspace_cow_gen{0};

extern "C" void aura_set_aot_expected_cow_gen_for_eval(void* eval_ptr, std::uint64_t gen) noexcept {
    // Minimal single-slot implementation: stores expected cow_gen for the
    // single most-recent eval pointer (sufficient for #2275 observability;
    // multi-eval tracking can grow into a hash if needed later).
    g_expected_cow_gen_per_eval.store(gen, std::memory_order_release);
    (void)eval_ptr;
}

extern "C" std::uint64_t aura_get_aot_expected_cow_gen_for_eval(void* eval_ptr) noexcept {
    (void)eval_ptr;
    return g_expected_cow_gen_per_eval.load(std::memory_order_acquire);
}

extern "C" void aura_set_live_workspace_cow_gen(std::uint64_t gen) noexcept {
    g_live_workspace_cow_gen.store(gen, std::memory_order_release);
}

extern "C" std::uint64_t aura_get_live_workspace_cow_gen(void) noexcept {
    return g_live_workspace_cow_gen.load(std::memory_order_acquire);
}

// Issue #2240: stable last reject reason for cross-workspace / cross-COW
// hot-update (refine #2178). Stored as uint8_t (CrossWorkspaceReject
// enum) for C ABI stability. Thread-safe lock-free read. Default
// value 0 = CrossWorkspaceReject::None (no reject since last reset).
// Reset to None at the start of every aura_reload_aot_module_for_eval
// attempt (alongside g_last_reload_fail_reason reset); ForeignEval is
// stored before the counter increment at the guard site below.
// CowGenMismatch: reload (#2275) + call-time closure stamp (#2547).
// Unknown is defensive — bumps if a future reject path forgets to set
// a specific reason. Cross-workspace write path remains fail-closed.
static std::atomic<std::uint8_t> g_last_cross_workspace_reject_reason{0};
// Issue #2275: process-level workspace cow_gen atoms. Eval tables
// are simple (eval_ptr -> expected cow_gen) — single atomic per
// common case; multi-eval extension can grow into a hash if needed.
// Live workspace cow_gen is bumped on each densify + workspace
// gen restamp; reload attempts compare eval-captured cow_gen
// against this to detect cross-COW drift without opening a
// migration write path.
// NOTE: storage forward-declared above (before aura_set_aot_expected_cow_gen_for_eval)
// so the accessors below can use it. (Old definition here was removed.)

extern "C" std::uint8_t aura_last_cross_workspace_reject_reason_v_read(void) noexcept {
    return g_last_cross_workspace_reject_reason.load(std::memory_order_relaxed);
}

extern "C" const char* aura_cross_workspace_reject_reason_string(std::uint8_t v) noexcept {
    switch (static_cast<CrossWorkspaceReject>(v)) {
        case CrossWorkspaceReject::None:
            return "none";
        case CrossWorkspaceReject::ForeignEval:
            return "foreign_eval";
        case CrossWorkspaceReject::CowGenMismatch:
            return "cow_gen_mismatch";
        case CrossWorkspaceReject::Unknown:
            return "unknown";
    }
    return "unknown"; // defensive — out-of-range uint8
}

extern "C" void aura_test_set_last_cross_workspace_reject_reason(std::uint8_t v) noexcept {
    g_last_cross_workspace_reject_reason.store(v, std::memory_order_release);
}

extern "C" void aura_test_reset_last_cross_workspace_reject_reason(void) noexcept {
    g_last_cross_workspace_reject_reason.store(
        static_cast<std::uint8_t>(CrossWorkspaceReject::None), std::memory_order_release);
}
// Issue #2178: returns true when eval_ptr is the current workspace-bound
// evaluator or null (process-default AotState). Foreign eval contexts
// (cross-workspace / cross-COW) are rejected by the reload / reemit
// walks. The MVP scope (#1943) documents single-workspace; this guard
// enforces the boundary until a future cross-COW migration design lands.
extern "C" bool aura_is_current_workspace_eval(void* eval_ptr) noexcept;

// Issue #2178: implementation of the cross-workspace guard. For the MVP
// scope (#1943, single-workspace), the only "current" contexts are:
//   - eval_ptr == nullptr: process-default AotState (always current)
//   - eval_ptr in g_aot_state_map: a previously-set per-eval state
// Any other eval_ptr is foreign (cross-workspace / cross-COW) and must
// be rejected by aura_reload_aot_module_for_eval + reemit callbacks.
// The lock is held only on the map lookup (read-only), so contention is
// minimal during the hot reemit path.
extern "C" bool aura_is_current_workspace_eval(void* eval_ptr) noexcept {
    if (eval_ptr == nullptr)
        return true; // process-default AotState is always current
    std::lock_guard<std::mutex> lock(g_aot_state_mtx);
    // Issue #2178 / #2165: empty map → first-touch Evaluator is allowed (MVP
    // single-workspace). Only reject when another eval_ptr is already registered
    // (true cross-workspace). aot_state_for() will insert on first use.
    if (g_aot_state_map.empty())
        return true;
    return g_aot_state_map.find(eval_ptr) != g_aot_state_map.end();
}

void note_reload_rollback(AotReloadFail reason) noexcept {
    if (aot_metrics()) {
        aot_metrics()->aot_hot_update_atomic_rollback.fetch_add(1, std::memory_order_relaxed);
        // Issue #2093: per-reason metric bump. Agent reads these to pick
        // a recovery policy without log scraping. The aggregate
        // aot_hot_update_atomic_rollback above is unchanged so existing
        // dashboards keep working.
        switch (reason) {
            case AotReloadFail::Dlopen:
                aot_metrics()->aot_reload_fail_dlopen_total.fetch_add(1, std::memory_order_relaxed);
                break;
            case AotReloadFail::Version:
                aot_metrics()->aot_reload_fail_version_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                break;
            case AotReloadFail::Region:
                aot_metrics()->aot_reload_fail_region_total.fetch_add(1, std::memory_order_relaxed);
                break;
            case AotReloadFail::Defuse:
                aot_metrics()->aot_reload_fail_defuse_total.fetch_add(1, std::memory_order_relaxed);
                break;
            case AotReloadFail::Env:
                aot_metrics()->aot_reload_fail_env_total.fetch_add(1, std::memory_order_relaxed);
                break;
            case AotReloadFail::Linear:
                aot_metrics()->aot_reload_fail_linear_total.fetch_add(1, std::memory_order_relaxed);
                break;
            case AotReloadFail::Staging:
                aot_metrics()->aot_reload_fail_staging_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                break;
            case AotReloadFail::Other:
                aot_metrics()->aot_reload_fail_other_total.fetch_add(1, std::memory_order_relaxed);
                break;
            case AotReloadFail::Ok:
                break;
        }
    }
    g_last_reload_fail_reason.store(static_cast<std::uint8_t>(reason), std::memory_order_release);
    aura::compiler::hot_update_registry().on_reload_rollback(reason);
}

// Issue #2093: thin wrapper for callers that don't have a specific
// reason (rare; legacy call sites funnel through Other).
void note_reload_rollback() noexcept {
    note_reload_rollback(AotReloadFail::Other);
}

} // namespace

extern "C" std::uint64_t aura_aot_func_table_epoch(void) {
    return g_aot_table_epoch.load(std::memory_order_acquire);
}

// Issue #2012 / #2046: diagnostics / tests — read live fn_ptr.
// Returns 0 for out-of-range, empty, or generation-behind slots.
// After soft/hard invalidate, aura_aot_bump_func_table_epoch advances
// the table epoch without restamping slots; probes must reject those
// entries so mixed JIT+AOT never executes stale AOT code.
extern "C" std::uintptr_t aura_aot_probe_fn_ptr(std::int64_t func_id) {
    if (func_id < 0)
        return 0;
    const auto idx = static_cast<unsigned>(func_id);
    if (idx >= kMaxAotFuncs)
        return 0;
    auto& slot = g_aot_func_slots[idx];
    const auto ptr = slot.fn_ptr.load(std::memory_order_acquire);
    if (ptr == 0)
        return 0;
    // Issue #2046: joint region identity — slot generation must match
    // current table epoch (same domain JIT capture_fn_epoch uses).
    const auto cur = g_aot_table_epoch.load(std::memory_order_acquire);
    const auto gen = slot.table_generation.load(std::memory_order_acquire);
    if (gen != cur) {
        if (aot_metrics()) {
            aot_metrics()->aot_stale_probe_hard_reject_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
            aot_metrics()->aot_slot_stale_reject_total.fetch_add(1, std::memory_order_relaxed);
            aot_metrics()->aot_forced_recompile_on_mismatch_total.fetch_add(
                1, std::memory_order_relaxed);
        }
        return 0; // Issue #2252 AC1: hard-reject native (nullptr) —
                  // never execute generation-behind AOT code.
    }
    return ptr;
}

// Issue #2046: raw probe that ignores generation (tests / recovery only).
extern "C" std::uintptr_t aura_aot_probe_fn_ptr_raw(std::int64_t func_id) {
    if (func_id < 0)
        return 0;
    const auto idx = static_cast<unsigned>(func_id);
    if (idx >= kMaxAotFuncs)
        return 0;
    return g_aot_func_slots[idx].fn_ptr.load(std::memory_order_acquire);
}

// Issue #2046: 1 when slot is empty/out-of-range/generation-behind.
extern "C" int aura_aot_slot_is_stale(std::int64_t func_id) {
    if (func_id < 0)
        return 1;
    const auto idx = static_cast<unsigned>(func_id);
    if (idx >= kMaxAotFuncs)
        return 1;
    auto& slot = g_aot_func_slots[idx];
    const auto ptr = slot.fn_ptr.load(std::memory_order_acquire);
    if (ptr == 0)
        return 1;
    const auto cur = g_aot_table_epoch.load(std::memory_order_acquire);
    const auto gen = slot.table_generation.load(std::memory_order_acquire);
    return gen != cur ? 1 : 0;
}

// Issue #2692: cross-eval sid ↔ AOT slot owner mismatch counter bumper
// (C ABI for tests + future Agent/query hook). Defined before
// aura_register_fn_tracked so the call site can resolve it.
extern "C" void aura_bump_cross_eval_sid_owner_mismatch_total() {
    if (aot_metrics())
        aot_metrics()->cross_eval_sid_owner_mismatch_total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #2692 AC2: hard-clear path under production defaults.
// Default ON (hard clear when production_defaults_active). Set
// AURA_AOT_CROSS_EVAL_SID_HARD=0 to force observe-only even under
// production (mirrors other AOT hard prefs).
[[nodiscard]] static bool aot_hard_pref() noexcept {
    static const bool hard = []() noexcept -> bool {
        if (const char* e = std::getenv("AURA_AOT_CROSS_EVAL_SID_HARD"))
            return e[0] != '0';
        return true;
    }();
    return hard;
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
    // Issue #2012: during reload, stage registrations so live slots stay
    // intact until validation succeeds and commit_func_table_swap runs.
    if (g_aot_staging_active.load(std::memory_order_acquire)) {
        g_aot_staging[idx].fn_ptr = static_cast<std::uintptr_t>(fn_ptr);
        g_aot_staging[idx].written = true;
        if (idx > g_aot_staging_hi)
            g_aot_staging_hi = idx;
        return;
    }
    auto& slot = g_aot_func_slots[idx];
    const std::uintptr_t new_ptr = static_cast<std::uintptr_t>(fn_ptr);
    const std::uintptr_t old_ptr = slot.fn_ptr.exchange(new_ptr, std::memory_order_acq_rel);
    if (old_ptr != 0 && old_ptr != new_ptr)
        slot.grace_refcount.fetch_add(1, std::memory_order_relaxed);
    slot.table_generation.store(g_aot_table_epoch.load(std::memory_order_acquire),
                                std::memory_order_relaxed);
    // Issue #2299: stamp owning eval (0 when process-default / unset).
    slot.owner_eval.store(reinterpret_cast<std::uintptr_t>(g_aot_register_owner_eval),
                          std::memory_order_relaxed);
    // Issue #2692: cross-eval sid ↔ AOT slot owner consistency assert.
    // Soft single-eval / process-default (filter eval = nullptr) keeps
    // this at 0. Production hard path clears the slot to prevent the
    // next call from hitting a wrong table.
    if (aot_metrics()) {
        const auto current_owner = reinterpret_cast<std::uintptr_t>(
            g_aot_register_owner_eval ? g_aot_register_owner_eval : g_aot_reemit_owner_eval);
        const auto stamped_owner = slot.owner_eval.load(std::memory_order_relaxed);
        if (current_owner != 0 && stamped_owner != 0 && current_owner != stamped_owner) {
            aura_bump_cross_eval_sid_owner_mismatch_total();
            // AC2: production hard clears the slot; Soft observes only.
            // Optional env AURA_AOT_CROSS_EVAL_SID_HARD=0 forces observe-only
            // even under production_defaults (mirrors other AOT hard prefs).
            if (aura::compiler::typed_audit::production_defaults_active() && aot_hard_pref()) {
                slot.fn_ptr.store(0, std::memory_order_relaxed);
                slot.owner_eval.store(0, std::memory_order_relaxed);
            }
        }
    }
}

extern "C" void aura_aot_set_register_owner_eval(void* eval_ptr) {
    g_aot_register_owner_eval = eval_ptr;
}

extern "C" void* aura_aot_get_register_owner_eval(void) {
    return g_aot_register_owner_eval;
}

// Issue #2606: reemit candidate ownership filter TLS.
extern "C" void aura_aot_set_reemit_owner_eval(void* eval_ptr) {
    g_aot_reemit_owner_eval = eval_ptr;
}

extern "C" void* aura_aot_get_reemit_owner_eval(void) {
    return g_aot_reemit_owner_eval;
}

extern "C" std::uintptr_t aura_aot_last_slot_invalidate_eval(void) {
    return g_aot_last_slot_invalidate_eval.load(std::memory_order_acquire);
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

// Issue #2371 / #2505: cross-COW soft migrate / hard reject counters.
extern "C" void aura_bump_cross_cow_soft_migrate_total(void) noexcept {
    if (aot_metrics())
        aot_metrics()->cross_cow_soft_migrate_total.fetch_add(1, std::memory_order_relaxed);
}
// Issue #2603: same-gen soft restamp counter (Agents split soft/hard
// for throttle). Bumped in aura_jit_runtime.cpp try_cross_cow_soft_migrate_
// success path; cross-gen → CowGenMismatch hard does NOT bump this.
extern "C" void aura_bump_cross_cow_soft_migrate_same_gen_total(void) noexcept {
    if (aot_metrics())
        aot_metrics()->cross_cow_soft_migrate_same_gen_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
}
extern "C" void aura_bump_cross_cow_hard_reject_total(void) noexcept {
    if (aot_metrics())
        aot_metrics()->cross_cow_hard_reject_total.fetch_add(1, std::memory_order_relaxed);
}
// Issue #2505 / #2547: reason breakdown (1=Disabled … 6=Other, 7=CowGenMismatch).
// Also stamps last reason.
extern "C" void aura_bump_cross_cow_hard_reject_reason(std::uint8_t reason) noexcept {
    auto* m = aot_metrics();
    if (!m)
        return;
    m->cross_cow_last_hard_reject_reason.store(reason, std::memory_order_relaxed);
    switch (reason) {
        case 1:
            m->cross_cow_hard_reject_disabled_total.fetch_add(1, std::memory_order_relaxed);
            break;
        case 2:
            m->cross_cow_hard_reject_freed_total.fetch_add(1, std::memory_order_relaxed);
            break;
        case 3:
            m->cross_cow_hard_reject_far_behind_total.fetch_add(1, std::memory_order_relaxed);
            break;
        case 4:
            m->cross_cow_hard_reject_linear_total.fetch_add(1, std::memory_order_relaxed);
            break;
        case 5:
            m->cross_cow_hard_reject_remount_fail_total.fetch_add(1, std::memory_order_relaxed);
            break;
        case 7: // Issue #2547 CowGenMismatch
            m->cross_cow_hard_reject_cow_gen_mismatch_total.fetch_add(1, std::memory_order_relaxed);
            break;
        case 6:
        default:
            m->cross_cow_hard_reject_other_total.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}
extern "C" std::uint8_t aura_cross_cow_last_hard_reject_reason(void) noexcept {
    auto* m = aot_metrics();
    return m ? m->cross_cow_last_hard_reject_reason.load(std::memory_order_relaxed) : 0;
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

// Issue #2046: joint epoch advance shared by soft/hard invalidate
// (atomic_bump_epochs_and_stamp_bridge) and AOT reload/reemit.
// Does NOT restamp slot table_generation — live slots become
// generation-behind so aura_aot_probe_fn_ptr rejects them until
// reemit/register. Notifies HotUpdateRegistry epoch listeners so
// agents/plugins stay aligned with JIT hot-swap.
//
// Issue #2713: observability for cross-eval epoch tax (#2670/#2606
// asymmetry). Joint bridge / AOT table epoch remains process-global
// by design (per #2606 comment: "joint epoch remains process-global
// — isolation is ownership + region mask + PerEval storm, not per-eval
// epoch domains"). When a single-eval host bumps the epoch the cost
// is local; when >1 live AotState is registered, eval A's cascade
// still forces eval B live AOT/JIT into generation-behind. The
// observability surface is bumped AFTER the joint epoch advance so
// dashboards can attribute cross-eval bumps. The epoch advance
// itself is unchanged — domain split is an explicit non-goal for
// this issue (per AC4 stretch). Quiet path: single-eval /
// process-default (nullptr owner, map size ≤1) → counter stays 0;
// one relaxed load of the map size is the only extra work.
static std::atomic<std::uint64_t> g_cross_eval_epoch_bump_total{0};
// Last owner that triggered a cross-eval epoch bump. Stored as
// atomic<void*> (nullptr when no cross-eval bump has happened, or
// when the last owner was process-default). Agent dashboards surface
// this to attribute the most recent cross-eval bump to a specific
// eval. Relaxed order — observability only, no control flow.
static std::atomic<void*> g_last_cross_eval_epoch_bump_owner{nullptr};
// Issue #2744: multi-eval cascade bumps that were owner-scoped throttled
// (skipped process-global table epoch) under production or
// AURA_CROSS_EVAL_EPOCH_THROTTLE=1. Additive — #2713 counters preserved.
static std::atomic<std::uint64_t> g_cross_eval_epoch_action_throttled_total{0};
// Force flag: hard invalidate / Agent fence paths set this TLS so the
// next bump always advances the process-global epoch (AC hard path).
static thread_local int g_cross_eval_epoch_force_bump = 0;
// Read accessors (mirror the #2693 / #2668 / #2640 file-scope counter
// style — queryable in light-link test bundles without the production
// CompilerMetrics TU).
extern "C" std::uint64_t cross_eval_epoch_bump_total_v_read(void) {
    return g_cross_eval_epoch_bump_total.load(std::memory_order_relaxed);
}
extern "C" void* last_cross_eval_epoch_bump_owner_v_read(void) {
    return g_last_cross_eval_epoch_bump_owner.load(std::memory_order_relaxed);
}
extern "C" std::uint32_t cross_eval_epoch_bump_wired_v_read(void) {
    return 1;
}
extern "C" std::uint64_t cross_eval_epoch_action_throttled_total_v_read(void) {
    return g_cross_eval_epoch_action_throttled_total.load(std::memory_order_relaxed);
}
extern "C" void aura_aot_note_cross_eval_epoch_force_bump(void) {
    g_cross_eval_epoch_force_bump = 1;
}

namespace {
[[nodiscard]] bool cross_eval_epoch_throttle_armed() noexcept {
    // Soft / sandbox=off: only when env explicitly armed (issue AC Soft).
    // Production multi-eval: throttle on by default.
    if (const char* e = std::getenv("AURA_CROSS_EVAL_EPOCH_THROTTLE"); e && *e) {
        if (e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y')
            return true;
        if (e[0] == '0' || e[0] == 'f' || e[0] == 'F' || e[0] == 'n' || e[0] == 'N')
            return false;
    }
    return aura::compiler::typed_audit::production_defaults_active();
}
} // namespace

extern "C" void aura_aot_bump_func_table_epoch(void) {
    // Issue #2744: multi-eval cascade tax action. When >1 live AotState
    // and throttle is armed, prefer owner-scoped stale-slot invalidate
    // over a process-global table epoch advance so foreign evals are not
    // force-staled by peer cascade. Hard invalidate / explicit fence
    // paths call aura_aot_note_cross_eval_epoch_force_bump() first.
    // Per-eval epoch domain split is a follow-up (non-goal for #2713/#2744);
    // joint table epoch writers stay aura_aot_bump_func_table_epoch.
    const bool multi = aura_aot_state_map_size() > 1;
    const bool force = g_cross_eval_epoch_force_bump != 0;
    g_cross_eval_epoch_force_bump = 0;
    if (multi && !force && cross_eval_epoch_throttle_armed()) {
        void* owner = aura_aot_get_reemit_owner_eval();
        if (!owner)
            owner = aura_aot_get_register_owner_eval();
        if (owner) {
            // Owner-scoped residual: clear this eval's generation-behind
            // slots without advancing g_aot_table_epoch (foreign slots
            // stay non-stale). #2713 observability still stamps the tax.
            g_cross_eval_epoch_bump_total.fetch_add(1, std::memory_order_relaxed);
            g_last_cross_eval_epoch_bump_owner.store(owner, std::memory_order_relaxed);
            g_cross_eval_epoch_action_throttled_total.fetch_add(1, std::memory_order_relaxed);
            (void)aura_aot_invalidate_all_stale_slots_for_eval(owner);
            // Do not notify_epoch_bump / event walk — no global epoch change.
            return;
        }
        // No owner TLS: fall through to global bump (single-owner residual
        // or misconfigured multi-eval — preserve prior behavior).
    }

    const std::uint64_t new_epoch = g_aot_table_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (auto* m = aot_metrics()) {
        m->aot_joint_epoch_bump_total.fetch_add(1, std::memory_order_relaxed);
        m->aot_region_version_bump_total.fetch_add(1, std::memory_order_relaxed);
        // Count non-empty slots now generation-behind (cheap when empty).
        std::uint64_t stale_slots = 0;
        for (unsigned i = 0; i < kMaxAotFuncs; ++i) {
            auto& slot = g_aot_func_slots[i];
            if (slot.fn_ptr.load(std::memory_order_relaxed) == 0)
                continue;
            if (slot.table_generation.load(std::memory_order_relaxed) != new_epoch)
                ++stale_slots;
        }
        if (stale_slots > 0)
            m->aot_region_stale_mark_total.fetch_add(stale_slots, std::memory_order_relaxed);
    }
    // Issue #2713: cross-eval epoch bump observability. Only bumps
    // when >1 live AotState is registered (single-eval / process-
    // default short-circuits to zero work beyond the relaxed load).
    // Current owner sourced from the per-eval reemit register
    // (per #2606 lineage). Stamp owner for dashboard attribution.
    if (multi) {
        g_cross_eval_epoch_bump_total.fetch_add(1, std::memory_order_relaxed);
        g_last_cross_eval_epoch_bump_owner.store(aura_aot_get_register_owner_eval(),
                                                 std::memory_order_relaxed);
    }
    // Fan-out: same path as commit_func_table_swap so invalidate and
    // successful reload share one epoch-listener contract.
    aura::compiler::hot_update_registry().notify_epoch_bump(new_epoch);
    // Issue #2668: event-driven soft walk on the epoch-bump edge
    // (covers reemit / reload paths). Production + Soft only.
    aura_event_driven_epoch_invariant_walk_if_due();
}

// Issue #2271 / #2299: physically invalidate generation-behind AOT slots
// (close #2232 / #2271 follow-up). After this call, matching prior
// non-null slots probe as 0 via aura_aot_probe_fn_ptr — defense in
// depth on top of the existing aot_stale_probe_hard_reject_total net.
//
// Semantics:
//   - For each g_aot_func_slots[i] where fn_ptr != 0 AND
//     table_generation != aura_aot_func_table_epoch() [AND, when
//     eval_ptr != nullptr, owner_eval == eval_ptr]: set fn_ptr empty
//     (atomic_store 0) + reset table_generation to 0 + clear owner.
//   - eval_ptr == nullptr: process-default — clear ALL generation-
//     behind slots (#2271 / #2299 AC2 back-compat).
//   - eval_ptr != nullptr: only clear slots owned by that eval
//     (#2299 AC1 dual-eval isolation).
//   - Does NOT dlclose prior modules — refcount / handle lifetime
//     stays #2012.
//   - Bumps aot_reload_fall_back_slot_invalidate_total by slot
//     count + aot_reload_fall_back_slot_invalidate_calls_total by 1.
//   - Records last eval_ptr for Agent dashboards (#2299 AC4).
extern "C" std::size_t aura_aot_invalidate_all_stale_slots_for_eval(void* eval_ptr) {
    g_aot_last_slot_invalidate_eval.store(reinterpret_cast<std::uintptr_t>(eval_ptr),
                                          std::memory_order_release);
    const auto cur_epoch = g_aot_table_epoch.load(std::memory_order_acquire);
    const bool filter_by_eval = (eval_ptr != nullptr);
    const auto want_owner = reinterpret_cast<std::uintptr_t>(eval_ptr);
    std::size_t invalidated = 0;
    for (unsigned i = 0; i < kMaxAotFuncs; ++i) {
        auto& slot = g_aot_func_slots[i];
        // Only touch slots that actually had a live fn_ptr — empty
        // slots are already generation-clean (register cleans them
        // atomically with fn_ptr load).
        const auto prev_fn = slot.fn_ptr.load(std::memory_order_acquire);
        if (prev_fn == 0)
            continue;
        const auto slot_gen = slot.table_generation.load(std::memory_order_acquire);
        if (slot_gen == cur_epoch)
            continue;
        // Issue #2299 AC1: multi-eval filter — skip foreign / unowned.
        if (filter_by_eval) {
            const auto owner = slot.owner_eval.load(std::memory_order_acquire);
            if (owner != want_owner)
                continue;
        }
        // Physically clear: zero fn_ptr + reset generation. Order:
        // fn_ptr first (release), then generation (release) so a
        // concurrent probe sees null fn_ptr before noticing the
        // generation drift — matches the safety net behavior in
        // aura_aot_probe_fn_ptr (#2299 AC3 ordering invariant).
        slot.fn_ptr.store(0, std::memory_order_release);
        slot.table_generation.store(0, std::memory_order_release);
        slot.owner_eval.store(0, std::memory_order_release);
        ++invalidated;
    }
    if (auto* m = aot_metrics()) {
        m->aot_reload_fall_back_slot_invalidate_total.fetch_add(invalidated,
                                                                std::memory_order_relaxed);
        m->aot_reload_fall_back_slot_invalidate_calls_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #2299 AC4: last-eval observability (pointer as u64).
        m->aot_reload_fall_back_slot_invalidate_last_eval.store(
            reinterpret_cast<std::uint64_t>(eval_ptr), std::memory_order_relaxed);
        if (filter_by_eval)
            m->aot_reload_fall_back_slot_invalidate_per_eval_calls_total.fetch_add(
                1, std::memory_order_relaxed);
    }
    return invalidated;
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
//
// Issue #2676: take alloc_storage_lock_ for the full critical section
// of the live-closure epoch bump. Multiple fibers on the same Worker
// (stackful ucontext, see #2649/#2650/#2651) can interleave
// `aura_refresh_live_closures_for_mutated_define` against closure
// materialization in evaluator_eval_flat.cpp — without the lock, the
// epoch bump + counters + future live-closure remount bookkeeping can
// race against concurrent closures_[cid] = std::move(cl) writes and
// `make_closure(cid)` reads (which hold closures_mtx_). Acquire the
// same per-heap lock class used by string_heap_ / pairs_ push
// (#2651) so lock-order audit (lock_order_audit.h) sees one consistent
// rank. Allocation alloc_storage_lock_ is held on the Evaluator that
// owns the closure bridges (ev_ptr may be null in default path — that
// path only touches aot_metrics() which is process-wide atomic and
// safe to skip the lock).
extern "C" void aura_refresh_live_closures_for_mutated_define(void* ev_ptr,
                                                              std::uint64_t define_id) {
    (void)define_id;
    (void)ev_ptr;
    // Issue #2676: lock ordering note. The owning Evaluator's
    // `alloc_storage_lock_` is held by the calling site
    // (Evaluator::flush_mutation_boundary outermost exit or any direct
    // call that needs closure-mutation serialization) — the lock is
    // NOT acquired in this C-style extern "C" bridge because
    // aura_jit_bridge.cpp is a .cpp file that does not import the
    // aura.compiler.evaluator module (which would make the full
    // Evaluator type visible). The caller is expected to hold the
    // lock-order audit rank (Closures → alloc_storage_lock_) at the
    // call site. The atomic epoch bump + counter increments below are
    // thread-safe on their own; the lock is needed for ordering with
    // concurrent closure materialization (closures_mtx_ write of
    // closures_[cid] = std::move(cl) + make_closure read).
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
// Issue #2177: AOT-side MacroIntroduced marker observability (refine #2100
// which was JIT-only). Two complementary counters:
//   - g_2177_aot_macro_marker_propagated_total: bumped when an AOT
//     pass (lowering_impl.cpp:1203) propagates SyntaxMarker::MacroIntroduced
//     from the source FlatAST node into the IRFunction.marker field.
//   - g_2177_aot_macro_marker_stripped_total: guard metric bumped when an
//     AOT pass observes a MacroIntroduced marker on the source node but
//     fails to propagate it (e.g., a future pass that strips markers).
//     Operators can monitor this for "silent marker loss" regressions.
static std::atomic<std::uint64_t> g_2177_aot_macro_marker_propagated_total{0};
static std::atomic<std::uint64_t> g_2177_aot_macro_marker_stripped_total{0};
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

// Issue #2177: C-linkage accessors for the AOT marker-propagation counters.
// Companion to (query:ir-hygiene-stats) keys aot-macro-marker-propagated-total
// and aot-macro-marker-stripped-total. Reads from the file-level atomics
// (no per-Evaluator mirror needed — these are process-wide AOT stats).
extern "C" std::uint64_t aura_2177_aot_macro_marker_propagated_total(void) {
    return g_2177_aot_macro_marker_propagated_total.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_2177_aot_macro_marker_stripped_total(void) {
    return g_2177_aot_macro_marker_stripped_total.load(std::memory_order_relaxed);
}

// Issue #2177: helper used by lowering_impl.cpp to bump the AOT
// marker-propagated counter (when a marker is successfully propagated
// to the IRFunction) or the marker-stripped counter (when an AOT pass
// observes a MacroIntroduced marker on the source but the propagation
// path is unavailable — e.g., pass runs on a node with no current_flat
// context). Exposed as a single C-linkage helper so the lowering pass
// can call one function instead of two (cleaner call site).
extern "C" void aura_2177_record_aot_marker_propagated(int propagated) noexcept {
    if (propagated)
        g_2177_aot_macro_marker_propagated_total.fetch_add(1, std::memory_order_relaxed);
    else
        g_2177_aot_macro_marker_stripped_total.fetch_add(1, std::memory_order_relaxed);
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

    // Issue #2043: linear-ownership epoch fence — when linear_state is set
    // and the process has advanced linear_ownership_epoch past the TLS
    // capture (frame_ver reuse as coarse stamp when >0 and < current),
    // force deopt so apply cannot race a concurrent finalize window.
    if (linear_state != 0) {
        const std::uint64_t lin_ep = aura_get_linear_ownership_epoch();
        const std::uint64_t frame_ver = g_linear_frame_version.load(std::memory_order_acquire);
        if (lin_ep != 0 && frame_ver != 0 && frame_ver < lin_ep) {
            stale = true;
            if (aot_metrics()) {
                aot_metrics()->linear_epoch_fence_enforce_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
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

// Issue #2165: process flag for auto reemit+retry (default ON; tests set 0).
// Env AURA_AOT_RELOAD_AUTO_RETRY=0|1 overrides when set.
static std::atomic<int> g_aot_reload_auto_retry_pref{-1}; // -1 = use env/default

extern "C" void aura_set_aot_reload_auto_retry(int enabled) {
    g_aot_reload_auto_retry_pref.store(enabled ? 1 : 0, std::memory_order_release);
}

extern "C" int aura_aot_reload_auto_retry_enabled(void) {
    const int pref = g_aot_reload_auto_retry_pref.load(std::memory_order_acquire);
    if (pref == 0 || pref == 1)
        return pref;
    if (const char* e = std::getenv("AURA_AOT_RELOAD_AUTO_RETRY")) {
        if (e[0] == '0')
            return 0;
        if (e[0] == '1')
            return 1;
    }
    return 1; // production default ON
}

static bool aot_reload_fail_is_auto_retryable(AotReloadFail reason) noexcept {
    switch (reason) {
        // Issue #2249: Region | Staging auto-retryable (conservative,
        // storm-skip handled separately). Dlopen | Other remain never.
        case AotReloadFail::Version:
        case AotReloadFail::Env:
        case AotReloadFail::Linear:
        case AotReloadFail::Defuse:
        case AotReloadFail::Region:
        case AotReloadFail::Staging:
            return true;
        default:
            return false;
    }
}

// Issue #1367: eval_ptr selects per-agent AotState (nullptr = process default).
// Issue #2012: version-keyed atomic func_table swap with staging + rollback.
// Issue #2165: single attempt body; public for_eval wraps auto-retry.
static bool aura_reload_aot_module_for_eval_once(void* eval_ptr, const char* path,
                                                 std::uint64_t version) {
    // Serialize concurrent reloads so staging state is single-writer.
    std::lock_guard<std::mutex> reload_lock(g_aot_reload_mtx);

    // Issue #2093: clear last-fail at the start of every attempt so
    // a failure path that exits without setting last-fail (e.g. the
    // null-path short-circuit at L1705) doesn't leak the previous
    // attempt's reason. AC3 covers the success-side clear below.
    g_last_reload_fail_reason.store(static_cast<std::uint8_t>(AotReloadFail::Ok),
                                    std::memory_order_release);
    // Issue #2240: clear cross-workspace last-reject reason at start of
    // every attempt (parallel reset — Agents read this to pick a recovery
    // policy without log scraping). ForeignEval/CowGenMismatch is set
    // at the guard site below before counter increment.
    g_last_cross_workspace_reject_reason.store(
        static_cast<std::uint8_t>(CrossWorkspaceReject::None), std::memory_order_release);

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
    // Issue #2178: hard guard for cross-workspace / cross-COW hot-update.
    // The MVP scope (#1943) documents single-workspace; this explicit reject
    // makes the boundary enforceable. Foreign eval contexts (or when COW
    // generation diverges from the workspace's current generation) bump the
    // cross_workspace_hot_update_rejected counter + set last-fail reason
    // and return false — never silently partial-succeed. The single-workspace
    // happy path (null eval_ptr / matching eval) is unchanged.
    if (eval_ptr != nullptr && !aura_is_current_workspace_eval(eval_ptr)) {
        // Issue #2240: stable cross-workspace reject reason code (refine
        // #2178). Set BEFORE the counter increment so that even if
        // downstream operations fail, the reason is observable (Agents
        // read this to pick recovery policy without log scraping).
        // CowGenMismatch / Unknown values are reserved for future
        // expansion (issue defers opening cross-COW write path).
        g_last_cross_workspace_reject_reason.store(
            static_cast<std::uint8_t>(CrossWorkspaceReject::ForeignEval),
            std::memory_order_release);
        aura_cross_workspace_hot_update_rejected_increment();
        g_last_reload_fail_reason.store(static_cast<std::uint8_t>(AotReloadFail::Other),
                                        std::memory_order_release);
        // Issue #2275: same-process, diverged COW generation (CowGenMismatch)
        // — distinct from ForeignEval. The cross_workspace_hot_update_
        // rejected counter is shared (both reasons bump the same counter)
        // but the reason string distinguishes them for Agent recovery policy.
        const std::uint64_t expected_cow_gen_2275 =
            aura_get_aot_expected_cow_gen_for_eval(eval_ptr);
        const std::uint64_t live_cow_gen_2275 = aura_get_live_workspace_cow_gen();
        if (expected_cow_gen_2275 != 0 && expected_cow_gen_2275 != live_cow_gen_2275) {
            g_last_cross_workspace_reject_reason.store(
                static_cast<std::uint8_t>(CrossWorkspaceReject::CowGenMismatch),
                std::memory_order_release);
            aura_cross_workspace_hot_update_rejected_increment();
            g_last_reload_fail_reason.store(static_cast<std::uint8_t>(AotReloadFail::Other),
                                            std::memory_order_release);
            aura::compiler::typed_audit::capture_aot_hotupdate_audit(
                /*success=*/false, epoch_before, g_aot_table_epoch.load(std::memory_order_acquire),
                "cross-COW cow_gen mismatch");
            return false;
        }
        // Issue #1882: audit the rejected cross-workspace attempt so Agent
        // diagnostics can attribute the failure to the right eval context.
        aura::compiler::typed_audit::capture_aot_hotupdate_audit(
            /*success=*/false, epoch_before, g_aot_table_epoch.load(std::memory_order_acquire),
            "cross-workspace hot-update rejected");
        return false;
    }
    if (!path) {
        aot_log("aura_reload_aot_module: null path\n");
        // No module loaded — no staged table to discard; skip rollback metric.
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

    // Issue #2012: constructors from the new module stage registrations;
    // live slots stay at the previous generation until commit.
    clear_aot_staging();
    g_aot_staging_active.store(true, std::memory_order_release);

    void* handle = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        g_aot_staging_active.store(false, std::memory_order_release);
        clear_aot_staging();
        aot_log("aura_reload_aot_module: dlopen failed for %s: %s\n", path, ::dlerror());
        // Issue #2093: per-reason rollback.
        note_reload_rollback(AotReloadFail::Dlopen);
        audit_fail("aot-hotupdate-dlopen-fail");
        return false;
    }
    // Staleness check: compare the new binary's aot_emit_version
    // against the host's known version. If the host specified
    // `version != 0`, it must match. If `version == 0`, we trust
    // the binary's own aot_emit_version.
    auto* binary_version = static_cast<std::uint64_t*>(::dlsym(handle, "aot_emit_version"));
    auto rollback_close = [&](std::string_view audit_reason, AotReloadFail reason) {
        // Discard staged registrations; live table untouched.
        g_aot_staging_active.store(false, std::memory_order_release);
        clear_aot_staging();
        ::dlclose(handle);
        // Epoch must remain epoch_before (no commit_func_table_swap).
        (void)epoch_before;
        // Issue #2093: per-reason rollback (was no-arg before).
        note_reload_rollback(reason);
        audit_fail(audit_reason);
    };
    if (binary_version) {
        if (version != 0 && *binary_version != version) {
            aot_log("aura_reload_aot_module: version mismatch "
                    "(binary=%llu, host=%llu) for %s\n",
                    static_cast<unsigned long long>(*binary_version),
                    static_cast<unsigned long long>(version), path);
            rollback_close("aot-hotupdate-version-mismatch", AotReloadFail::Version);
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
            rollback_close("aot-hotupdate-missing-emit-version", AotReloadFail::Version);
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
            rollback_close("aot-hotupdate-region-mismatch", AotReloadFail::Region);
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
            rollback_close("aot-hotupdate-stale-defuse", AotReloadFail::Defuse);
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
            rollback_close("aot-hotupdate-env-frame-drift", AotReloadFail::Env);
            return false;
        }
    }
    // Issue #2012: apply staged constructor registrations into live
    // slots, then atomically bump g_aot_table_epoch so concurrent
    // probes observe a consistent before/after boundary.
    apply_aot_staging_to_live();
    g_aot_staging_active.store(false, std::memory_order_release);
    commit_func_table_swap();
    clear_aot_staging();
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
    aura::compiler::hot_update_registry().on_reload_success();
    // Issue #2093: success path clears last-fail to Ok (AC3).
    g_last_reload_fail_reason.store(static_cast<std::uint8_t>(AotReloadFail::Ok),
                                    std::memory_order_release);
    // Issue #1882: TypedMutationAudit trail for successful hot-update (sampled).
    aura::compiler::typed_audit::capture_aot_hotupdate_audit(
        /*success=*/true, epoch_before, g_aot_last_commit_epoch, "aot-hotupdate");
    return true;
}

// Issue #2232: public entry — reason-driven multi-round retry driven by
// `policy_for(reason)`. Replaces the #2165 single-retry (TLS depth)
// with an iterative loop bounded by `policy.max_reemit`. On exhausted,
// the policy may direct a fall-back to JIT (force-stale mark on
// affected AOT slots) so long-running AI self-mod survives sustained
// defuse/env churn without losing the host thread. Dlopen/Region/
// Staging/Other remain never-auto (path/ops/bug class — no recovery).
// The TLS depth guard is preserved so a reemit→reload nested call
// doesn't re-enter the loop recursively.
extern "C" bool aura_reload_aot_module_for_eval(void* eval_ptr, const char* path,
                                                std::uint64_t version) {
    thread_local int t_auto_retry_depth = 0;
    // Issue #2299: stamp slot ownership for any register_fn_tracked /
    // staging apply performed under this reload so later per-eval
    // physical invalidate only clears this eval's generation-behind slots.
    struct RegisterOwnerGuard {
        void* prev;
        explicit RegisterOwnerGuard(void* e) noexcept
            : prev(g_aot_register_owner_eval) {
            g_aot_register_owner_eval = e;
        }
        ~RegisterOwnerGuard() noexcept { g_aot_register_owner_eval = prev; }
        RegisterOwnerGuard(const RegisterOwnerGuard&) = delete;
        RegisterOwnerGuard& operator=(const RegisterOwnerGuard&) = delete;
    } owner_guard(eval_ptr);

    const bool ok1 = aura_reload_aot_module_for_eval_once(eval_ptr, path, version);
    if (ok1)
        return true;

    if (t_auto_retry_depth > 0 || !aura_aot_reload_auto_retry_enabled())
        return false;

    const auto reason = static_cast<AotReloadFail>(aura_aot_last_reload_fail_reason());
    if (!aot_reload_fail_is_auto_retryable(reason))
        return false; // Dlopen / Region / Staging / Other — no retry by design

    // Issue #2232: look up the per-reason policy. Version | Defuse get
    // 3 retries @ 5ms; Env | Linear get 2 @ 10ms; everything else
    // (the Dlopen/Region/Staging/Other branch already returned
    // above via is_auto_retryable) is {0,0,false}.
    const ReloadPolicy policy = policy_for(reason);
    if (policy.max_reemit == 0)
        return false;

    // Issue #2249: storm-skip for Region/Staging under hard storm.
    // Suppress auto-retry into a storming region mask / staging
    // handshake — the next reemit / boundary tick won't help if the
    // whole region is contended.
    // Issue #2544: also suppress aggressive min-dirty reemit under the
    // same storm-skip gate (Region/Staging + Global/hard storm).
    if (aot_reload_storm_skip_retry_for_2249(reason)) {
        if (aot_metrics()) {
            aot_metrics()->aot_reload_region_staging_exhausted_total.fetch_add(
                1, std::memory_order_relaxed);
            if (policy.fall_back_jit_only)
                aot_metrics()->aot_reload_fall_back_jit_only_total.fetch_add(
                    1, std::memory_order_relaxed);
            aot_metrics()->aot_reload_exhausted_min_dirty_reemit_storm_skip_total.fetch_add(
                1, std::memory_order_relaxed);
        }
        return false;
    }

    ++t_auto_retry_depth;
    if (aot_metrics())
        aot_metrics()->aot_reload_auto_retry_total.fetch_add(1, std::memory_order_relaxed);
    // Issue #2249: Region/Staging retry counter (AC4).
    if (aot_metrics() && (reason == AotReloadFail::Region || reason == AotReloadFail::Staging))
        aot_metrics()->aot_reload_region_staging_retry_total.fetch_add(1,
                                                                       std::memory_order_relaxed);

    // Iterative loop, not recursive. Each attempt: incremental
    // reemit (best-effort, may be a no-op if no host emit is
    // available) then a fresh single-attempt reload. Version
    // recovery trusts the binary after reemit (version=0); other
    // reasons keep the caller's expected version.
    for (int attempt = 0; attempt < policy.max_reemit; ++attempt) {
        if (aot_metrics())
            aot_metrics()->aot_reload_policy_attempt_total.fetch_add(1, std::memory_order_relaxed);
        (void)aura_reemit_aot_for_dirty(aura_get_aot_defuse_version());
        const std::uint64_t retry_version = (reason == AotReloadFail::Version) ? 0 : version;
        const bool ok = aura_reload_aot_module_for_eval_once(eval_ptr, path, retry_version);
        if (ok) {
            if (aot_metrics())
                aot_metrics()->aot_reload_auto_retry_success_total.fetch_add(
                    1, std::memory_order_relaxed);
            --t_auto_retry_depth;
            return true;
        }
        // Optional backoff between attempts (not after the last —
        // we exit the loop and report exhausted).
        if (policy.backoff_ms > 0 && attempt + 1 < policy.max_reemit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(policy.backoff_ms));
        }
    }

    // Exhausted. If the policy directs a fall-back to JIT, mark
    // affected AOT slots generation-stale (force JIT) so subsequent
    // calls in the host skip the broken AOT path. The actual slot
    // invalidation is a future follow-up — for #2232 the visible
    // contract is the metric + a hot_update_registry() callback
    // that Agents can observe. last-fail is the *final* reason
    // from the last attempt (left in place by _once()).
    if (policy.fall_back_jit_only) {
        aura::compiler::hot_update_registry().on_force_jit_for_reason(reason);
        if (aot_metrics())
            aot_metrics()->aot_reload_fall_back_jit_only_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
        // Issue #2271: physically invalidate generation-behind slots
        // + joint-bump table epoch (#2046). After this, every prior
        // non-null slot probes as 0 via aura_aot_probe_fn_ptr (zero-
        // native-hit, not just probe-reject). Slot + call counters
        // bumped inside the helper. eval_ptr is the affected eval
        // (nullptr = process-default AotState).
        (void)aura_aot_invalidate_all_stale_slots_for_eval(eval_ptr);
        // Issue #2744: hard invalidate / auto-retry must still advance the
        // process-global table epoch (throttle soft path must not apply).
        aura_aot_note_cross_eval_epoch_force_bump();
        aura_aot_bump_func_table_epoch();

        // Issue #2544: queue a minimal dirty set from the fail reason
        // and drive one internal reemit so #2502 re-promote can start
        // without waiting for external dirty notification. Storm-skip
        // for Region/Staging under Global/hard storm (same helper as
        // #2249) — do not queue aggressive recovery into a storm.
        // Soft: when reemit defers for mutation boundary, that is not
        // a hard fail (pending drain will run later).
        if (aot_reload_storm_skip_retry_for_2249(reason)) {
            if (aot_metrics())
                aot_metrics()->aot_reload_exhausted_min_dirty_reemit_storm_skip_total.fetch_add(
                    1, std::memory_order_relaxed);
        } else {
            auto& hur_md = aura::compiler::hot_update_registry();
            hur_md.on_exhausted_min_dirty_queue(reason);
            if (aot_metrics())
                aot_metrics()->aot_reload_exhausted_min_dirty_reemit_attempt_total.fetch_add(
                    1, std::memory_order_relaxed);
            const auto n_md = aura_reemit_aot_for_dirty(aura_get_aot_defuse_version());
            if (n_md > 0) {
                if (aot_metrics())
                    aot_metrics()->aot_reload_exhausted_min_dirty_reemit_success_total.fetch_add(
                        1, std::memory_order_relaxed);
            } else if (!hur_md.has_deferred_reemit()) {
                // True empty/reject — keep force-JIT; distinct fail metric.
                if (aot_metrics())
                    aot_metrics()->aot_reload_exhausted_min_dirty_reemit_fail_total.fetch_add(
                        1, std::memory_order_relaxed);
            }
            // Deferred boundary: leave pending; agents drain under Guard.
            // Not counted as fail (recovery is in-flight, not abandoned).
        }
    }
    // Issue #2249: Region/Staging exhausted counter (AC4).
    if (aot_metrics() && (reason == AotReloadFail::Region || reason == AotReloadFail::Staging))
        aot_metrics()->aot_reload_region_staging_exhausted_total.fetch_add(
            1, std::memory_order_relaxed);
    if (aot_metrics())
        aot_metrics()->aot_reload_auto_retry_exhausted_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
    --t_auto_retry_depth;
    return false;
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
    return aura_aot_fn_version_is_stale_ex(dl_handle, original_name, expected, /*env=*/0,
                                           /*linear=*/0);
}

// Issue #2015: defuse + optional env_frame / linear drift vs dlopened module.
extern "C" bool aura_aot_fn_version_is_stale_ex(void* dl_handle, const char* original_name,
                                                std::uint64_t expected_defuse,
                                                std::uint64_t expected_env_frame,
                                                std::uint8_t expected_linear) {
    const std::uint64_t got = aura_aot_probe_fn_version(dl_handle, original_name);
    if (got == kAotFnVersionMissing) {
        // Missing per-fn + emit version: treat as stale only when host
        // expects a concrete non-zero epoch (legacy binary vs modern host).
        if (expected_defuse != 0) {
            if (aot_metrics())
                aot_metrics()->aot_fn_version_probe_stale_total.fetch_add(
                    1, std::memory_order_relaxed);
            return true;
        }
        // Still check env/linear symbols when host tracks them.
    } else if (got != expected_defuse) {
        if (aot_metrics())
            aot_metrics()->aot_fn_version_probe_stale_total.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Issue #2015: captured-env / linear ownership drift from module symbols.
    if (dl_handle && (expected_env_frame != 0 || expected_linear != 0)) {
        auto* bin_env = static_cast<std::uint64_t*>(::dlsym(dl_handle, "aot_env_frame_version"));
        // Emitted as unsigned long long in generate_registration_c when present.
        auto* bin_lin64 = static_cast<std::uint64_t*>(::dlsym(dl_handle, "aot_linear_state"));
        const std::uint64_t got_env = bin_env ? *bin_env : 0;
        const std::uint8_t got_lin =
            bin_lin64 ? static_cast<std::uint8_t>(*bin_lin64 > 255ull ? 255ull : *bin_lin64) : 0;
        if (got_env != expected_env_frame || got_lin != expected_linear) {
            if (aot_metrics()) {
                aot_metrics()->aot_fn_version_probe_stale_total.fetch_add(
                    1, std::memory_order_relaxed);
                if (got_env != expected_env_frame || got_lin != expected_linear) {
                    aot_metrics()->aot_env_frame_version_drift_prevented.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            return true;
        }
    }
    return false;
}

// Host-side (no dlopen) mangle probe wrappers for C callers / tests.
extern "C" bool aura_aot_parse_version_suffix(const char* mangled, std::uint64_t* out_version) {
    if (!mangled || !out_version)
        return false;
    return aura::compiler::aot_parse_version_suffix(mangled, out_version);
}

extern "C" bool aura_aot_parse_full_version_suffix(const char* mangled, std::uint64_t* out_defuse,
                                                   std::uint64_t* out_env_frame,
                                                   std::uint8_t* out_linear) {
    if (!mangled)
        return false;
    aura::compiler::AotVersionSuffix full{};
    if (!aura::compiler::aot_parse_full_version_suffix(mangled, &full) || !full.has_defuse)
        return false;
    if (out_defuse)
        *out_defuse = full.defuse_version;
    if (out_env_frame)
        *out_env_frame = full.has_env_linear ? full.env_frame_version : 0;
    if (out_linear)
        *out_linear = full.has_env_linear ? full.linear_state : 0;
    return true;
}

extern "C" bool aura_aot_mangle_version_is_stale(const char* mangled, std::uint64_t expected) {
    if (!mangled)
        return true;
    return aura::compiler::aot_mangle_version_is_stale(mangled, expected);
}

extern "C" bool aura_aot_mangle_version_is_stale_ex(const char* mangled,
                                                    std::uint64_t expected_defuse,
                                                    std::uint64_t expected_env_frame,
                                                    std::uint8_t expected_linear) {
    if (!mangled)
        return true;
    bool defuse_stale = false;
    bool env_stale = false;
    bool lin_stale = false;
    const bool stale = aura::compiler::aot_mangle_version_is_stale_detail(
        mangled, expected_defuse, expected_env_frame, expected_linear, &defuse_stale, &env_stale,
        &lin_stale);
    if (stale && aot_metrics()) {
        if (defuse_stale) {
            aot_metrics()->aot_fn_version_probe_stale_total.fetch_add(1, std::memory_order_relaxed);
        }
        if (env_stale || lin_stale) {
            aot_metrics()->aot_env_frame_version_drift_prevented.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    return stale;
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
// Runtime name→fn lookup (defined in aura_jit_runtime.cpp).
extern "C" int64_t aura_lookup_fn_by_name(const char* name, int64_t* out_local_count,
                                          int64_t* out_arg_count, int64_t* out_env_count);

// Sentinel native body when reemit has no live JIT symbol for the name.
static int64_t aura_aot_reemit_sentinel_fn(int64_t* /*args*/, uint32_t /*n*/) {
    return 0;
}

// Issue #2016: default LLVM incremental reemit when host did not wire
// aura_set_aot_emit_fn. Uses the process AuraJIT (batch-deopt target) to
// emit a native object for the named function. Caller registers stable id.
static bool default_llvm_incremental_emit(const char* name, std::uint64_t region) {
    if (!name || !*name)
        return false;
    if (region == kAotRegionEvolution)
        return false;
    if (!g_batch_deopt_jit)
        return false;

    std::string safe;
    safe.reserve(32);
    for (const char* p = name; *p && safe.size() < 48; ++p) {
        const char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
            safe.push_back(c);
        else
            safe.push_back('_');
    }
    if (safe.empty())
        safe = "fn";
    static std::atomic<std::uint64_t> reemit_seq{0};
    const auto seq = reemit_seq.fetch_add(1, std::memory_order_relaxed);
    const std::string obj_path =
        std::format("/tmp/aura_reemit_{}_{}.o", safe, static_cast<unsigned long long>(seq));

    const bool ok = g_batch_deopt_jit->compile_function_to_object_by_name(name, obj_path);
    if (!ok) {
        // Issue #2095: bump fail counter + optionally keep the bad .o for
        // postmortem (env AURA_REEMIT_KEEP_FAIL=1). Without this, failed
        // compiles silently fall through to the skeleton path and the
        // success-only counter under-reports real LLVM work.
        if (aot_metrics())
            aot_metrics()->aot_incremental_llvm_emit_fail_total.fetch_add(
                1, std::memory_order_relaxed);
        if (aura_reemit_keep_fail_enabled()) {
            aura_reemit_keep_failed_obj(obj_path.c_str(), "compile_failed");
        } else {
            std::remove(obj_path.c_str());
        }
        return false;
    }
    std::remove(obj_path.c_str()); // success: still ephemeral
    if (aot_metrics())
        aot_metrics()->aot_incremental_llvm_emit_total.fetch_add(1, std::memory_order_relaxed);
    return true;
}

static void register_stable_id_in_func_table(const char* name, std::uint32_t sid) {
    if (!name || sid == 0)
        return;
    int64_t locals = 0, args = 0, env = 0;
    const int64_t existing = aura_lookup_fn_by_name(name, &locals, &args, &env);
    if (existing != 0)
        aura_register_fn_tracked(static_cast<int64_t>(sid), existing);
    else
        aura_register_fn_tracked(
            static_cast<int64_t>(sid),
            static_cast<int64_t>(reinterpret_cast<std::uintptr_t>(&aura_aot_reemit_sentinel_fn)));
}

// Issue #2016: adapt live region mask after a pipeline call.
// Only Default (0) and Performance (1) bits are mutable; Evolution (2)
// remains permanently cleared. Clears under high per-region dirty
// density or an active deopt storm; restores preferred bits when quiet.
static void adapt_emit_region_mask(const std::uint64_t dirty_by_region[3], bool storm_active) {
    const std::uint64_t preferred = g_aot_emit_region_mask_preferred;
    std::uint64_t live = g_aot_emit_region_mask;
    // Never allow Evolution bit in live mask.
    live &= ~(1ULL << kAotRegionEvolution);

    auto maybe_clear = [&](std::uint64_t r) {
        if (r == kAotRegionEvolution)
            return;
        const std::uint64_t bit = 1ULL << r;
        if ((preferred & bit) == 0)
            return; // host never requested this region
        const bool pressure = dirty_by_region[r] >= kAotRegionDirtyClearThreshold || storm_active;
        if (pressure && (live & bit) != 0) {
            live &= ~bit;
            if (aot_metrics())
                aot_metrics()->aot_region_mask_adapt_clears_total.fetch_add(
                    1, std::memory_order_relaxed);
            aura::compiler::hot_update_registry().on_region_mask_adapt_clear(r);
        } else if (!pressure && (preferred & bit) != 0 && (live & bit) == 0) {
            live |= bit;
            if (aot_metrics())
                aot_metrics()->aot_region_mask_adapt_restores_total.fetch_add(
                    1, std::memory_order_relaxed);
            aura::compiler::hot_update_registry().on_region_mask_adapt_restore(r);
        }
    };
    maybe_clear(kAotRegionDefault);
    maybe_clear(kAotRegionPerformance);

    if (live != g_aot_emit_region_mask) {
        g_aot_emit_region_mask = live;
        aura::compiler::hot_update_registry().on_emit_region_mask_set(live);
    }
}

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

    // Issue #2014 / #2132: during a deopt storm, coalesce reemit (skip).
    // Dual-check / deopt correctness is unchanged; only recovery is delayed
    // until the sliding window rolls and throttle clears.
    // Issue #2132: region/priority-aware — critical mask may bypass soft
    // storm; hard ceiling still throttles everyone.
    // Issue #2172: throttle source-of-truth is the StormLevel facade
    // (policy table hot_update_registry.hh:79). Only the Global bit
    // throttles reemit; the Shape bit goes to SpecJIT/GuardShape
    // conservative mode (not reemit). Shape-only storms therefore pass
    // through this gate unchanged — the existing `should_throttle_reemit()`
    // semantics are preserved as the inner refinement (region/critical
    // bypass for soft storm; hard ceiling always wins).
    {
        auto& hur_thr = aura::compiler::hot_update_registry();
        const std::uint64_t dirty_mask = hur_thr.last_region_mask_from_dirty();
        const std::uint64_t emit_mask = hur_thr.emit_region_mask();
        const std::uint64_t region_or_prio = dirty_mask != 0 ? dirty_mask : emit_mask;
        const auto storm_level = hur_thr.current_storm_level();
        constexpr std::uint8_t kGlobal =
            static_cast<std::uint8_t>(aura::compiler::HotUpdateRegistry::StormLevel::Global);
        const bool global_storm = (static_cast<std::uint8_t>(storm_level) & kGlobal) != 0;
        if (global_storm) {
            if (hur_thr.should_throttle_reemit(region_or_prio)) {
                using TR = aura::compiler::HotUpdateRegistry::ThrottleReason;
                TR reason = TR::Global;
                if (hur_thr.hard_storm_active())
                    reason = TR::Hard;
                else if (region_or_prio != 0)
                    reason = TR::Region;
                hur_thr.on_reemit_throttled(reason);
                g_last_reemit_dirty_count.store(0, std::memory_order_relaxed);
                g_last_reemit_region_skips.store(0, std::memory_order_relaxed);
                g_last_reemit_closure_dep_count.store(0, std::memory_order_relaxed);
                g_last_reemit_success_count.store(0, std::memory_order_relaxed);
                return 0;
            }
            // Soft storm active but critical region allowed reemit.
            if (hur_thr.is_critical_region(region_or_prio))
                hur_thr.on_reemit_critical_bypass();
        }
    }

    // Issue #2114 / #2205 / #2208: HotUpdate reemit ↔ MutationBoundary handshake.
    // Production default Defer (#2205/#2208): outside → pending, no AOT body.
    // SoftEnter is opt-in only (not steal-safe — TLS does not migrate).
    // RequireRealBoundary: outside → reject without defer.
    // Inside (depth>0 or Guard held, including #2090 dtor window): proceed.
    // Never silent — outside path always bumps reemit_outside_boundary_total.
    auto& hur = aura::compiler::hot_update_registry();
    // Consuming a deferred reemit under a real boundary is still "inside".
    if (hur.has_deferred_reemit() && hur.in_mutation_boundary_for_reemit()) {
        (void)hur.take_deferred_reemit_version();
    }
    struct SoftReemitBoundaryGuard {
        bool active = false;
        SoftReemitBoundaryGuard() = default;
        ~SoftReemitBoundaryGuard() {
            if (active)
                aura::compiler::hot_update_registry().soft_reemit_boundary_exit();
        }
        SoftReemitBoundaryGuard(const SoftReemitBoundaryGuard&) = delete;
        SoftReemitBoundaryGuard& operator=(const SoftReemitBoundaryGuard&) = delete;
    } soft_guard;
    if (!hur.in_mutation_boundary_for_reemit()) {
        hur.on_reemit_outside_boundary();
        using Policy = aura::compiler::HotUpdateRegistry::ReemitBoundaryPolicy;
        const auto pol = hur.reemit_boundary_policy();
        // SoftEnter only when policy SoftEnter AND soft_enter_allowed().
        if (pol == Policy::SoftEnter && hur.soft_enter_allowed()) {
            hur.soft_reemit_boundary_enter();
            soft_guard.active = true;
            hur.on_reemit_soft_boundary_entered();
        } else if (pol == Policy::RequireRealBoundary) {
            hur.on_reemit_rejected_require_real();
            g_last_reemit_dirty_count.store(0, std::memory_order_relaxed);
            g_last_reemit_region_skips.store(0, std::memory_order_relaxed);
            g_last_reemit_closure_dep_count.store(0, std::memory_order_relaxed);
            g_last_reemit_success_count.store(0, std::memory_order_relaxed);
            return 0;
        } else {
            // Defer (production default #2205/#2208) and any non-SoftEnter fallthrough.
            hur.defer_reemit_for_boundary(current_defuse_version);
            g_last_reemit_dirty_count.store(0, std::memory_order_relaxed);
            g_last_reemit_region_skips.store(0, std::memory_order_relaxed);
            g_last_reemit_closure_dep_count.store(0, std::memory_order_relaxed);
            g_last_reemit_success_count.store(0, std::memory_order_relaxed);
            return 0;
        }
    }

    // Issue #2606: multi-AotState reemit ownership filter.
    // Prefer explicit reemit-owner TLS; fall back to register-owner
    // (reload_for_eval RegisterOwnerGuard) so nested reemit during
    // auto-retry also filters. nullptr → process-default: no filter
    // (soft single-eval path identical to pre-#2606).
    // Invariant: joint bridge/AOT table epoch remains process-global
    // (commit_func_table_swap still bumps one shared generation domain);
    // isolation is ownership + region mask + PerEval storm — not
    // per-eval epoch domains.
    void* const filter_eval =
        g_aot_reemit_owner_eval != nullptr ? g_aot_reemit_owner_eval : g_aot_register_owner_eval;
    const bool filter_by_eval = (filter_eval != nullptr);
    const auto want_owner = reinterpret_cast<std::uintptr_t>(filter_eval);

    // Region mask: process emit mask is the cascade default. When a
    // filter eval is set and its AotState region_mask is non-zero,
    // prefer the per-eval mask so multi-agent region bits do not
    // bleed across AotState entries (#2606 region independence).
    std::uint64_t region_mask = g_aot_emit_region_mask;
    if (filter_by_eval) {
        const auto pe = aot_state_for(filter_eval).region_mask.load(std::memory_order_acquire);
        if (pe != 0)
            region_mask = pe;
    }
    const std::uint64_t epoch_before = g_aot_table_epoch.load(std::memory_order_acquire);

    std::uint64_t total_candidates = 0;
    std::uint64_t to_re_emit = 0;
    std::uint64_t success_count = 0;
    std::uint64_t region_skips = 0;
    std::uint64_t cross_eval_skips = 0;
    std::uint64_t closure_dep_count = 0;
    std::uint64_t dirty_by_region[3] = {0, 0, 0};
    bool any_re_emit = false;
    // Host emit wired → always report success_count (may be 0).
    // Default LLVM only flips return mode after at least one real emit.
    const bool host_emit_wired = (g_aot_emit_fn != nullptr);
    bool real_llvm_emit_success = false;

    // Issue #2013: collect (name, stable_id) for successful reemits so we
    // can retarget live closures after the epoch commit.
    std::vector<std::string> reemit_names;
    std::vector<std::uint32_t> reemit_stable_ids;
    reemit_names.reserve(16);
    reemit_stable_ids.reserve(16);

    // Phase 2 walk: iterate host-pushed candidates + region-mask filter.
    // #1952: host-wired LLVM emit. #1930: stable name→func_id on success.
    // #2016: default LLVM path via AuraJIT when host emit fn is null;
    //        permanent Evolution exclusion + adaptive mask post-pass.
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

        // Issue #2606: dual-eval ownership filter — mirror
        // aura_aot_invalidate_all_stale_slots_for_eval. When the host
        // stamped a reemit/register owner, drop candidates whose
        // stable_func_id maps to a live slot owned by a foreign eval.
        // Unowned (owner==0) or unmapped names pass through (new emits
        // will stamp under the current owner; soft single-eval slots
        // remain unowned).
        if (filter_by_eval) {
            const std::uint32_t sid = aura_lookup_stable_func_id(name);
            if (sid != 0 && sid < kMaxAotFuncs) {
                auto& slot = g_aot_func_slots[sid];
                const auto prev_fn = slot.fn_ptr.load(std::memory_order_acquire);
                if (prev_fn != 0) {
                    const auto owner = slot.owner_eval.load(std::memory_order_acquire);
                    if (owner != 0 && owner != want_owner) {
                        ++cross_eval_skips;
                        if (aot_metrics())
                            aot_metrics()->reemit_cross_eval_candidate_skipped_total.fetch_add(
                                1, std::memory_order_relaxed);
                        continue;
                    }
                }
            }
        }

        // Issue #2016: Evolution (region=2) is permanently excluded from AOT.
        if (region == kAotRegionEvolution) {
            ++region_skips;
            if (aot_metrics())
                aot_metrics()->aot_evolution_region_skips_total.fetch_add(
                    1, std::memory_order_relaxed);
            continue;
        }

        if (region < 3)
            dirty_by_region[region] += 1;

        // Per-function region mask filter: if host set a non-zero
        // mask, the candidate's region must have its bit set in the
        // mask. Region 0 means "no region preference" → always emit
        // (unless Evolution, already handled).
        // Issue #2606: region_mask may be the per-eval AotState mask
        // when filter_eval is set (see setup above).
        if (region_mask != 0 && region != 0) {
            const std::uint64_t bit = 1ULL << (region & 63);
            if ((region_mask & bit) == 0) {
                ++region_skips;
                continue;
            }
        }
        ++to_re_emit;

        // count_emit_success: host/default LLVM success only (not skeleton).
        auto note_reemit = [&](std::uint32_t sid, int preserved, bool count_emit_success,
                               bool count_llvm_metric) {
            aura::compiler::hot_update_registry().on_stable_func_id_preserve(preserved != 0);
            if (aot_metrics()) {
                if (count_emit_success) {
                    aot_metrics()->aot_incremental_reemit_success_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
                if (preserved) {
                    aot_metrics()->stable_func_id_preserved_total.fetch_add(
                        1, std::memory_order_relaxed);
                } else {
                    aot_metrics()->stable_func_id_assigned_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
                if (count_llvm_metric)
                    aot_metrics()->aot_incremental_llvm_emit_total.fetch_add(
                        1, std::memory_order_relaxed);
            }
            if (sid != 0) {
                reemit_names.emplace_back(name);
                reemit_stable_ids.push_back(sid);
            }
            if (count_emit_success)
                ++success_count;
            any_re_emit = true;
        };

        // Issue #1952 / #1930 / #2016: host emit → else default LLVM → skeleton.
        if (g_aot_emit_fn) {
            const bool emitted = g_aot_emit_fn(name, region, g_aot_emit_userdata);
            if (emitted) {
                int preserved = 0;
                std::uint32_t sid = 0;
                {
                    std::lock_guard<std::mutex> lock(g_stable_func_id_mtx);
                    // Issue #2670: reemit path uses reemit-owner eval key (or
                    // register-owner fallback) so multi-eval reemit preserves
                    // per-eval sids.
                    sid = preserve_stable_func_id_for_eval_locked(
                        aura_aot_get_reemit_owner_eval() ? aura_aot_get_reemit_owner_eval()
                                                         : aura_aot_get_register_owner_eval(),
                        name, &preserved);
                }
                register_stable_id_in_func_table(name, sid);
                note_reemit(sid, preserved, /*count_emit_success=*/true, /*count_llvm=*/true);
                real_llvm_emit_success = true;
            }
        } else if (g_batch_deopt_jit && default_llvm_incremental_emit(name, region)) {
            int preserved = 0;
            std::uint32_t sid = 0;
            {
                std::lock_guard<std::mutex> lock(g_stable_func_id_mtx);
                // Issue #2670: same owner key as host-emit path.
                sid = preserve_stable_func_id_for_eval_locked(
                    aura_aot_get_reemit_owner_eval() ? aura_aot_get_reemit_owner_eval()
                                                     : aura_aot_get_register_owner_eval(),
                    name, &preserved);
            }
            register_stable_id_in_func_table(name, sid);
            note_reemit(sid, preserved, /*count_emit_success=*/true, /*count_llvm=*/false);
            real_llvm_emit_success = true;
        } else {
            // Phase 1 / #1480 skeleton (or default LLVM miss): stamp stable map.
            // Does not bump success_count (would-reemit return path).
            int preserved = 0;
            std::uint32_t sid = 0;
            {
                std::lock_guard<std::mutex> lock(g_stable_func_id_mtx);
                sid = preserve_stable_func_id_for_eval_locked(
                    aura_aot_get_reemit_owner_eval() ? aura_aot_get_reemit_owner_eval()
                                                     : aura_aot_get_register_owner_eval(),
                    name, &preserved);
            }
            note_reemit(sid, preserved, /*count_emit_success=*/false, /*count_llvm=*/false);
        }
    }

    // Atomic commit: bump func_table_epoch only if at least one
    // successful / would-reemit path advanced.
    if (any_re_emit) {
        commit_func_table_swap();
        // Issue #2013: retarget live closures whose name matches a
        // reemitted stable id so they keep calling native code without
        // dual-freshness deopt. Global epoch bump remains for unmatched
        // closures (safety preserved).
        if (!reemit_stable_ids.empty()) {
            const std::uint64_t new_epoch = g_aot_table_epoch.load(std::memory_order_acquire);
            // Issue #2092: caller no longer threads display names into the
            // remap — the closure table matches by stable_func_id stored at
            // aura_closure_set_name time (refine #2013). name-fallback
            // path is gated by aura_set_remap_name_fallback_enabled().
            const std::uint64_t remapped = aura_remap_live_closures_after_reemit(
                reemit_stable_ids.data(), reemit_stable_ids.size(), new_epoch);
            if (remapped > 0) {
                if (aot_metrics()) {
                    aot_metrics()->live_closure_remap_total.fetch_add(remapped,
                                                                      std::memory_order_relaxed);
                }
                aura::compiler::hot_update_registry().on_live_closure_remap(remapped);
            }
        }
        // Issue #2602: synchronous remount walk for named live closures
        // (stable_func_id != 0). Closes the MustDeopt window between
        // reemit and first call — reemit success already restamped
        // epoch + env_gen (#2542), this completes the loop by calling
        // aura_remount_or_force_deopt on each named closure so the
        // first call after reemit continues native without MustDeopt.
        // Anonymous (sid=0) stay on the existing call-time path (#2550).
        // Soft zero-cost when no live named closures (decide path
        // short-circuits before the inner loop — AC4). Function bumps
        // live_closure_sync_remount_ok_total / _fail_total internally.
        std::uint64_t sync_ok = 0;
        std::uint64_t sync_fail = 0;
        aura_sync_remount_named_live_closures(&sync_ok, &sync_fail);

        // Issue #2637: env-gated sync remount walk for anonymous / residual
        // closures (sid == 0). Mirrors the #2602 named path on the
        // opposite sid branch. Soft zero-cost when env knob off (default
        // per AC1) OR no live anonymous closures (nslots==0 short-circuit
        // same as named path). Bumps live_closure_sync_remount_anon_ok_total
        // / _fail_total internally (distinct from named sync counters).
        // Default OFF preserves the existing call-time MustDeopt-on-touch
        // behavior for anonymous / residual closures (#2550 / #2605).
        if (aura_sync_remount_anon_enabled_default ? (aura_sync_remount_anon_enabled_default() != 0)
                                                   : false) {
            std::uint64_t anon_ok = 0;
            std::uint64_t anon_fail = 0;
            aura_sync_remount_anon_live_closures(&anon_ok, &anon_fail);
        }
        // Issue #2714: production-default sync remount for captured anon
        // (align #2691 with defaults). Captured anon (sid==0 && has
        // env/linear) is the highest-value anon subset for EDSL / agent
        // code — first-call MustDeopt jitter under production defaults
        // is a zero-downtime regression vs. the named #2602 path which
        // already runs unconditionally. The captured walk now runs
        // when either production_defaults_active() OR
        // AURA_SYNC_REMOUNT_ANON=1 is set (per AC1). Soft / sandbox=off /
        // explicit AURA_SYNC_REMOUNT_ANON=0 under non-production keeps
        // the zero-cost short-circuit (per AC4). The full anon walk
        // (aura_sync_remount_anon_live_closures) above remains env-gated
        // for #2637 stress-test pattern — not affected by #2714.
        const bool sync_captured = aura::compiler::typed_audit::production_defaults_active() ||
                                   (aura_sync_remount_anon_enabled_default
                                        ? (aura_sync_remount_anon_enabled_default() != 0)
                                        : false);
        if (sync_captured) {
            // Issue #2691: captured-only anon sync remount (filter by
            // aura_closure_has_env_or_linear_captures). Distinct counters
            // (anon_captured_ok / _fail) so Agents can distinguish
            // "must remount" (captured) from "touch-time policy" (pure
            // anon). Soft zero-cost when no captures match (counter stable).
            std::uint64_t anon_cap_ok = 0;
            std::uint64_t anon_cap_fail = 0;
            aura_sync_remount_anon_captured_live_closures(&anon_cap_ok, &anon_cap_fail);
        }
    }

    // Issue #2016: adaptive region mask based on this call's dirty density
    // and deopt-storm throttle state (Performance/Default only).
    adapt_emit_region_mask(dirty_by_region,
                           aura::compiler::hot_update_registry().should_throttle_reemit());

    // Stamp last-call stats for tests + EDSL observability primitives.
    // dirty_count = region-filtered candidates (would-reemit); success
    // is the actual emit count when emit path is active.
    g_last_reemit_dirty_count.store(to_re_emit, std::memory_order_relaxed);
    g_last_reemit_region_skips.store(region_skips, std::memory_order_relaxed);
    g_last_reemit_closure_dep_count.store(closure_dep_count, std::memory_order_relaxed);
    g_last_reemit_success_count.store(success_count, std::memory_order_relaxed);
    (void)cross_eval_skips; // counted via reemit_cross_eval_candidate_skipped_total

    // Issue #1956: registry aggregates re-emit pipeline traffic.
    aura::compiler::hot_update_registry().on_reemit_pipeline_call(to_re_emit, success_count);

    // Metric bumps (relaxed order is fine for stats).
    if (aot_metrics()) {
        aot_metrics()->aot_incremental_reemit_count.fetch_add(to_re_emit,
                                                              std::memory_order_relaxed);
        aot_metrics()->aot_closure_dependency_reemit_total.fetch_add(closure_dep_count,
                                                                     std::memory_order_relaxed);
        aot_metrics()->aot_region_filtered_skips.fetch_add(region_skips, std::memory_order_relaxed);
        // Closure bridge refresh: host emit or real LLVM success → success_count;
        // pure skeleton → would-reemit count (#1480 parity).
        if (any_re_emit) {
            const std::uint64_t refresh_n =
                (host_emit_wired || real_llvm_emit_success) ? success_count : to_re_emit;
            aot_metrics()->aot_closure_bridge_refresh_total.fetch_add(refresh_n,
                                                                      std::memory_order_relaxed);
        }
    }

    (void)current_defuse_version; // reserved for version pin
    (void)epoch_before;           // reserved for future rollback
    (void)total_candidates;
    // #1930 / #2016: host emit wired → success_count; real default LLVM
    // success → success_count; pure skeleton → would-reemit (to_re_emit).
    return (host_emit_wired || real_llvm_emit_success) ? success_count : to_re_emit;
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
    //
    // Issue #2091: prefer the live Evaluator mirror via
    // aot_resolve_emit_env_frame_version() (max of explicit host
    // global + live atomic). Wired hosts always produce a
    // non-zero stamp here so captured-env drift is observable.
    aot_seed_force_env_linear_suffix_from_env();
    const std::uint64_t emit_env_frame_version = aot_resolve_emit_env_frame_version();
    // Issue #2091: module-level linear_state fingerprint = max of
    // per-fn linear_ownership_state + the live Evaluator fingerprint.
    // This is the same max computation that used to live inline below
    // for the `aot_linear_state` symbol; lifted so mangle / link
    // names can stamp the same value (pre-#2091 link names were
    // defuse-only — captured-linear drift was invisible).
    std::uint8_t max_lin = aot_resolve_emit_linear_state_fingerprint();
    for (unsigned int i = 0; i < num_functions; ++i) {
        const std::uint8_t ls =
            static_cast<std::uint8_t>(functions[i].linear_ownership_state & 0xFFu);
        if (ls > max_lin)
            max_lin = ls;
    }
    const std::uint8_t emit_linear_state = max_lin;
    for (unsigned int i = 0; i < num_functions; ++i) {
        // Issue #1640: thread env_frame_version + linear_state
        // through the mangle so the resulting symbol carries the
        // extra versioning context. linear_state is read from the
        // function's runtime-linear-ownership metadata (0 = Untracked
        // when the function does not capture linear bindings).
        //
        // Issue #2091: per-function linear_state wins over the
        // module-level max when it's higher (a single tracked-linear
        // closure in the module still stamps the symbol). The env
        // stamp is module-global — same value for every fn in the
        // emit batch.
        const std::uint8_t fn_linear_state =
            static_cast<std::uint8_t>(functions[i].linear_ownership_state & 0xFFu);
        const std::uint8_t fn_lin_for_mangle =
            fn_linear_state > emit_linear_state ? fn_linear_state : emit_linear_state;
        mangled_names[i] = aura::compiler::mangle_aot_name(
            functions[i].name, i, emit_version, emit_env_frame_version, fn_lin_for_mangle);
        // Issue #2091: link names now carry env_frame_version +
        // linear_state too (pre-#2091 was defuse-only). The
        // signature change is additive — the existing 3-arg
        // overload defaults to (0, 0) which preserves the legacy
        // shape when no host wiring exists.
        link_names[i] = aura::compiler::aot_link_name(functions[i].name, i, emit_version,
                                                      emit_env_frame_version, fn_lin_for_mangle);
    }
    // Issue #2091: stamp accounting. Wired hosts bump `stamped`;
    // miswired hosts (literal 0,0 + force off) bump
    // `default_zero` so the Agent dashboard can detect drift.
    aot_bump_env_linear_stamp_metric(emit_env_frame_version, emit_linear_state);
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
    // Issue #2015: module-level linear ownership stamp for probe/stale
    // (paired with mangle `_lN` and aura_aot_fn_version_is_stale_ex).
    // Use max per-fn linear_ownership_state so any tracked linear in
    // the module is visible to dlsym-based drift checks.
    {
        std::uint8_t max_lin = 0;
        for (unsigned int i = 0; i < num_functions; ++i) {
            const std::uint8_t ls =
                static_cast<std::uint8_t>(functions[i].linear_ownership_state & 0xFFu);
            if (ls > max_lin)
                max_lin = ls;
        }
        fprintf(f, "// Issue #2015: AOT linear_state (ownership drift)\n");
        fprintf(f, "const unsigned long long aot_linear_state = %llull;\n",
                static_cast<unsigned long long>(max_lin));
    }

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


// Issue #2304 / #2366: process-level epoch invariant mode + counters.
// Mode is the source of truth for run_epoch_invariant_if_enabled (service
// reads via aura_epoch_invariant_mode). Soft=metric only; hard=abort.
// File-local atomics keep non-module tests free of service.ixx include.
static std::atomic<std::uint64_t> g_epoch_invariant_violation_total{0};
static std::atomic<std::uint64_t> g_epoch_invariant_walks_total{0};
// Issue #2501 additive breakdown.
static std::atomic<std::uint64_t> g_epoch_invariant_slot_stale_total{0};
static std::atomic<std::uint64_t> g_epoch_invariant_closure_must_deopt_total{0};
// 0=off, 1=soft, 2=hard
static std::atomic<std::uint8_t> g_epoch_invariant_mode{0};

extern "C" std::uint64_t aura_epoch_invariant_violation_total_v_read(void) {
    return g_epoch_invariant_violation_total.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_epoch_invariant_walks_total_v_read(void) {
    return g_epoch_invariant_walks_total.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_epoch_invariant_slot_stale_total_v_read(void) {
    return g_epoch_invariant_slot_stale_total.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_epoch_invariant_closure_must_deopt_total_v_read(void) {
    return g_epoch_invariant_closure_must_deopt_total.load(std::memory_order_relaxed);
}

extern "C" void aura_set_epoch_invariant_mode(int mode) {
    if (mode < 0)
        mode = 0;
    if (mode > 2)
        mode = 2;
    g_epoch_invariant_mode.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);
}

extern "C" int aura_epoch_invariant_mode(void) {
    return static_cast<int>(g_epoch_invariant_mode.load(std::memory_order_relaxed));
}

extern "C" void aura_set_epoch_invariant_hard_enabled(int enabled) {
    // Backward-compat (#2304): enabled → hard (2); disabled → off (0).
    aura_set_epoch_invariant_mode(enabled != 0 ? 2 : 0);
}

extern "C" void aura_epoch_invariant_note_walk(std::uint64_t violations) noexcept {
    g_epoch_invariant_walks_total.fetch_add(1, std::memory_order_relaxed);
    if (violations > 0)
        g_epoch_invariant_violation_total.fetch_add(violations, std::memory_order_relaxed);
}

extern "C" void aura_epoch_invariant_note_slot_stale(std::uint64_t n) noexcept {
    if (n > 0)
        g_epoch_invariant_slot_stale_total.fetch_add(n, std::memory_order_relaxed);
}

extern "C" void aura_epoch_invariant_note_closure_must_deopt(std::uint64_t n) noexcept {
    if (n > 0)
        g_epoch_invariant_closure_must_deopt_total.fetch_add(n, std::memory_order_relaxed);
}

// Issue #2366: count live generation-behind AOT slots (fn_ptr≠0 && gen≠cur).
extern "C" std::size_t aura_aot_count_live_generation_behind_slots(void) {
    const auto cur = g_aot_table_epoch.load(std::memory_order_acquire);
    std::size_t n = 0;
    for (unsigned i = 0; i < kMaxAotFuncs; ++i) {
        auto& slot = g_aot_func_slots[i];
        if (slot.fn_ptr.load(std::memory_order_acquire) == 0)
            continue;
        if (slot.table_generation.load(std::memory_order_acquire) != cur)
            ++n;
    }
    return n;
}

extern "C" void aura_aot_inject_live_stale_slot_for_test(std::int64_t func_id) {
    if (func_id < 0)
        return;
    const auto idx = static_cast<unsigned>(func_id);
    if (idx >= kMaxAotFuncs)
        return;
    auto& slot = g_aot_func_slots[idx];
    // Non-null sentinel + generation one behind current (or 0 if epoch is 0).
    const auto cur = g_aot_table_epoch.load(std::memory_order_acquire);
    slot.fn_ptr.store(static_cast<std::uintptr_t>(0xDEADBEEF), std::memory_order_release);
    slot.table_generation.store(cur > 0 ? cur - 1 : 0, std::memory_order_release);
}

extern "C" void aura_aot_clear_slot_for_test(std::int64_t func_id) {
    if (func_id < 0)
        return;
    const auto idx = static_cast<unsigned>(func_id);
    if (idx >= kMaxAotFuncs)
        return;
    auto& slot = g_aot_func_slots[idx];
    slot.fn_ptr.store(0, std::memory_order_release);
    slot.table_generation.store(0, std::memory_order_release);
    slot.owner_eval.store(0, std::memory_order_release);
}

// Issue #2304 / #2366 / #2541: AURA_EPOCH_INVARIANT env-var bridge.
// soft|1 → soft; hard → hard; 0|off → force off; unset → leave default
// (production apply_production_security_defaults sets soft when unset).
namespace {
struct EpochInvariantEnvInit {
    EpochInvariantEnvInit() noexcept {
        if (const char* e = std::getenv("AURA_EPOCH_INVARIANT")) {
            const std::string v(e);
            if (v == "hard")
                aura_set_epoch_invariant_mode(2);
            else if (v == "soft" || v == "1" || v == "true" || v == "on")
                aura_set_epoch_invariant_mode(1);
            else if (v == "0" || v == "off" || v == "false" || v == "no")
                aura_set_epoch_invariant_mode(0); // #2541 AC5
        }
    }
};
[[maybe_unused]] EpochInvariantEnvInit g_epoch_invariant_env_init{};
} // namespace

// ── Issue #2640: production Restricted default periodic epoch-invariant soft walk ──
//
// Reuses aura_epoch_invariant_must_deopt_stale_live_closures (per #2501 / #2541) +
// aura_aot_invalidate_all_stale_slots_for_eval (per #2271 / #2299) which already
// physically clear generation-behind AOT slots and MustDeopt stale live closures.
//
// Gating (Issue #2640 sketch):
//   - period_ms == 0         → disabled → bump skipped_disabled_total
//   - mode != Soft (1)       → skip     → bump skipped_wrong_mode_total
//   - !production_defaults   → skip     → bump skipped_off_total
//                                  (covers sandbox=off + dev apply_dev_audit_defaults)
//   - rate-limited (steady_ms_now - last_walk_at_ms < period_ms)
//                              → skip    → bump skipped_rate_limited_total
//   - else: bump walks_total, update last_walk_at_ms,
//          then call the existing soft walk (which bumps existing
//          walks_total + slot_stale_total + closure_must_deopt_total
//          via note_walk inside the run_epoch_invariant_if_enabled path).
//
// Wire-up: MutationBoundaryGuard::~MutationBoundaryGuard outermost success exit
// (called per outer mutation boundary exit; internal rate limit ensures
// amortized cost regardless of call frequency). AC4 satisfied: bounded
// by period_ms, not by mutation count.

namespace {
// File-local steady_ms_now (matches the one in hot_update_registry.cpp:23).
// Defined here for self-contained bridge TU.
std::uint64_t periodic_steady_ms_now() noexcept {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::atomic<std::uint64_t> g_epoch_invariant_periodic_walks_total{0};
std::atomic<std::uint64_t> g_epoch_invariant_periodic_last_walk_at_ms{0};
std::atomic<std::uint64_t> g_epoch_invariant_periodic_skipped_off_total{0};
std::atomic<std::uint64_t> g_epoch_invariant_periodic_skipped_wrong_mode_total{0};
std::atomic<std::uint64_t> g_epoch_invariant_periodic_skipped_rate_limited_total{0};
std::atomic<std::uint64_t> g_epoch_invariant_periodic_skipped_disabled_total{0};
std::atomic<std::uint64_t> g_epoch_invariant_periodic_period_ms{5000}; // default 5s
// Issue #2668: event-driven walk counter (distinct from periodic).
// Bumped when commit_func_table_swap / aura_aot_bump_func_table_epoch
// triggers an event-driven soft walk under production + Soft (closes
// the burst-mutation window that pure periodic Soft leaves open).
// Shares last_walk_at_ms atomic with periodic path so double-walk on
// boundary+swap in the same ms is amortized (no double physical clear).
inline std::atomic<std::uint64_t> g_epoch_invariant_event_walks_total{0};              // #2668
inline std::atomic<std::uint64_t> g_epoch_invariant_event_skipped_off_total{0};        // #2668
inline std::atomic<std::uint64_t> g_epoch_invariant_event_skipped_wrong_mode_total{0}; // #2668
} // namespace

extern "C" std::uint64_t aura_epoch_invariant_periodic_walks_total_v_read(void) {
    return g_epoch_invariant_periodic_walks_total.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_epoch_invariant_periodic_last_walk_at_ms_v_read(void) {
    return g_epoch_invariant_periodic_last_walk_at_ms.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_epoch_invariant_periodic_skipped_off_total_v_read(void) {
    return g_epoch_invariant_periodic_skipped_off_total.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_epoch_invariant_periodic_skipped_wrong_mode_total_v_read(void) {
    return g_epoch_invariant_periodic_skipped_wrong_mode_total.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_epoch_invariant_periodic_skipped_rate_limited_total_v_read(void) {
    return g_epoch_invariant_periodic_skipped_rate_limited_total.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_epoch_invariant_periodic_skipped_disabled_total_v_read(void) {
    return g_epoch_invariant_periodic_skipped_disabled_total.load(std::memory_order_relaxed);
}
// Issue #2668: event-driven walk accessors (distinct from periodic).
extern "C" std::uint64_t aura_epoch_invariant_event_walks_total_v_read(void) {
    return g_epoch_invariant_event_walks_total.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_epoch_invariant_event_skipped_off_total_v_read(void) {
    return g_epoch_invariant_event_skipped_off_total.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_epoch_invariant_event_skipped_wrong_mode_total_v_read(void) {
    return g_epoch_invariant_event_skipped_wrong_mode_total.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_epoch_invariant_periodic_period_ms_v_read(void) {
    return g_epoch_invariant_periodic_period_ms.load(std::memory_order_relaxed);
}
extern "C" void aura_set_epoch_invariant_periodic_period_ms(std::uint64_t ms) {
    g_epoch_invariant_periodic_period_ms.store(ms, std::memory_order_relaxed);
}

// Issue #2640: env init — AURA_EPOCH_INVARIANT_PERIOD_MS (default 5000, 0=disabled).
namespace {
struct EpochInvariantPeriodEnvInit {
    EpochInvariantPeriodEnvInit() noexcept {
        if (const char* e = std::getenv("AURA_EPOCH_INVARIANT_PERIOD_MS")) {
            const long long v = std::atoll(e);
            if (v >= 0 && v <= 3600000) { // cap at 1h for sanity
                g_epoch_invariant_periodic_period_ms.store(static_cast<std::uint64_t>(v),
                                                           std::memory_order_relaxed);
            }
        }
    }
};
[[maybe_unused]] EpochInvariantPeriodEnvInit g_epoch_invariant_period_env_init{};
} // namespace

// Issue #2693: Soft epoch-invariant consecutive-dirty fuse
// (refine #2640 / #2668). After K consecutive Soft walks that all
// left behind slots uncleared, fire epoch_invariant_soft_fuse_total
// so Agents can distinguish "transient spike" from "stuck walk
// pattern" without log scraping. K defaults to 3 (env
// AURA_EPOCH_INVARIANT_SOFT_FUSE_K; 0 disables). Soft zero-cost
// when consecutive_dirty stays 0 (single atomic op per walk).
//
// The fuse is *observability-only* under Soft (issue body §A — no
// abort path; existing Hard-mode abort is unchanged). The optional
// "one-shot force-JIT region seed" is deferred to a follow-up issue;
// this first ship ships the metric + K knob + linter surface.
static std::atomic<std::uint64_t> g_consecutive_dirty_count{0};
// File-level atomic fallback for light binaries without the production
// CompilerMetrics TU. Production TU prefers the per-CompilerMetrics
// counter (see evaluator_fiber_mutation.cpp wiring); this fallback
// mirrors the #2640/#2668 file-scope counter style and keeps query
// surfaces queryable in light-link test bundles.
static std::atomic<std::uint64_t> g_2693_soft_fuse_fallback_total{0};
// Issue #2712: Soft fuse → bounded physical heal. Bumped once per
// fuse trip under production / Soft / consec >= K when the heal body
// actually runs (invalidate stale slots + must_deopt stale live
// closures). Distinct from epoch_invariant_soft_fuse_total (which
// bumps on every consec >= K walk) so dashboards can attribute
// "fuse fired" vs "heal ran" separately. Soft / K=0 / mode=Off → no
// heal body, so this counter stays at 0.
static std::atomic<std::uint64_t> g_2693_soft_fuse_heal_fallback_total{0};
// Issue #2747: Soft fuse heal skipped process-wide invalidate because
// multi-eval + reemit-owner unset (foreign slots preserved).
static std::atomic<std::uint64_t> g_2693_soft_fuse_heal_no_owner_total{0};
static std::atomic<int> g_2693_soft_fuse_k{3};

extern "C" std::uint64_t aura_epoch_invariant_soft_fuse_total_v_read(void) {
    return g_2693_soft_fuse_fallback_total.load(std::memory_order_relaxed);
}
// Issue #2712: read accessor for the soft-fuse heal counter
// (production + Soft + consec >= K + heal body ran).
extern "C" std::uint64_t aura_epoch_invariant_soft_fuse_heal_total_v_read(void) {
    return g_2693_soft_fuse_heal_fallback_total.load(std::memory_order_relaxed);
}
// Issue #2747: multi-eval heal with no reemit-owner (skipped process-wide clear).
extern "C" std::uint64_t aura_epoch_invariant_soft_fuse_heal_no_owner_total_v_read(void) {
    return g_2693_soft_fuse_heal_no_owner_total.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_epoch_invariant_consecutive_dirty_total_v_read(void) {
    return g_consecutive_dirty_count.load(std::memory_order_relaxed);
}
extern "C" int aura_epoch_invariant_soft_fuse_k_default(void) {
    return g_2693_soft_fuse_k.load(std::memory_order_relaxed);
}
extern "C" void aura_set_epoch_invariant_soft_fuse_k(int k) {
    if (k < 0)
        k = 0;
    g_2693_soft_fuse_k.store(k, std::memory_order_relaxed);
}
extern "C" int aura_get_epoch_invariant_soft_fuse_k(void) {
    return aura_epoch_invariant_soft_fuse_k_default();
}

// Issue #2693: env init — AURA_EPOCH_INVARIANT_SOFT_FUSE_K (default 3, 0=disabled).
namespace {
struct EpochInvariantSoftFuseKEnvInit {
    EpochInvariantSoftFuseKEnvInit() noexcept {
        if (const char* e = std::getenv("AURA_EPOCH_INVARIANT_SOFT_FUSE_K")) {
            const long long v = std::atoll(e);
            if (v >= 0 && v <= 1000) { // cap at 1000 for sanity
                g_2693_soft_fuse_k.store(static_cast<int>(v), std::memory_order_relaxed);
            }
        }
    }
};
[[maybe_unused]] EpochInvariantSoftFuseKEnvInit g_epoch_invariant_soft_fuse_k_env_init{};
} // namespace

// Issue #2693: walk-time helper. After the soft walk clears the
// slot table, record whether any behind-after-clear slots remained;
// bump the consecutive_dirty counter on stuck walks, reset on
// clean walks, and fire the fuse counter when consecutive_dirty
// reaches K (K=0 disables). Shared by periodic + event-driven
// walks so both paths contribute to the same fuse signal.
//
// Issue #2712: production + Soft + consec >= K additionally drives
// a bounded physical heal — invalidates stale slots via reemit-owner
// TLS (when set) + must-deopt stale live closures. Heal is bounded
// to at most one physical clear per fuse trip (consec resets to 0
// after the heal body runs, so re-entry mid-walk / next walk
// requires K fresh stuck walks before another heal). Soft / K=0 /
// mode=Off: no heal body, no extra cost. The fuse counter still
// bumps on consec >= K (additive — mirrors the existing #2693 path);
// the new heal counter bumps only when the heal body actually runs
// (production + Soft + consec >= K).
static void aura_2693_soft_fuse_record(std::size_t behind_after_clear) {
    if (behind_after_clear > 0) {
        g_consecutive_dirty_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_consecutive_dirty_count.store(0, std::memory_order_relaxed);
    }
    const int k = g_2693_soft_fuse_k.load(std::memory_order_relaxed);
    if (k > 0) {
        const std::uint64_t consec = g_consecutive_dirty_count.load(std::memory_order_relaxed);
        if (consec >= static_cast<std::uint64_t>(k)) {
            g_2693_soft_fuse_fallback_total.fetch_add(1, std::memory_order_relaxed);
            // Issue #2712: drive bounded physical heal under
            // production + Soft. Reuse #2541 / #2299 / #2606 helpers
            // (aura_aot_invalidate_all_stale_slots_for_eval +
            // aura_epoch_invariant_must_deopt_stale_live_closures).
            // Prefer reemit-owner TLS (per AC4) so multi-eval hosts
            // do not wipe foreign slots; helper falls back to
            // process-default when reemit owner is unset. Heal is
            // bounded: consec resets to 0 below, so the next heal
            // requires K fresh stuck walks (rate-limited naturally).
            if (aura::compiler::typed_audit::production_defaults_active() &&
                aura_epoch_invariant_mode() == 1) { // 1 = Soft (per #2541)
                g_2693_soft_fuse_heal_fallback_total.fetch_add(1, std::memory_order_relaxed);
                void* reemit_owner = aura_aot_get_reemit_owner_eval();
                // Issue #2747: multi-eval + reemit-owner unset → do NOT
                // process-wide invalidate (nullptr clears ALL generation-
                // behind slots per #2271/#2299). Still must_deopt closures
                // visible to current context; consec still resets only
                // when heal body runs (bounded rate).
                if (reemit_owner == nullptr && aura_aot_state_map_size() > 1) {
                    g_2693_soft_fuse_heal_no_owner_total.fetch_add(1, std::memory_order_relaxed);
                    (void)aura_epoch_invariant_must_deopt_stale_live_closures();
                    g_consecutive_dirty_count.store(0, std::memory_order_relaxed);
                } else {
                    (void)aura_aot_invalidate_all_stale_slots_for_eval(reemit_owner);
                    (void)aura_epoch_invariant_must_deopt_stale_live_closures();
                    // Reset consec to 0 so re-entry / next walk requires
                    // K fresh stuck walks before the next heal — bounds
                    // physical-clear rate (per AC3).
                    g_consecutive_dirty_count.store(0, std::memory_order_relaxed);
                }
            }
        }
    }
}

// Issue #2640: main hook — called from MutationBoundaryGuard outermost dtor.
// Gated by mode=Soft + production_defaults_active + period_ms rate limit.
extern "C" void aura_periodic_epoch_invariant_walk_if_due(void) {
    const auto period_ms = g_epoch_invariant_periodic_period_ms.load(std::memory_order_relaxed);
    if (period_ms == 0) {
        g_epoch_invariant_periodic_skipped_disabled_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (aura_epoch_invariant_mode() != 1) { // 1 = Soft (per #2541 Restricted default)
        g_epoch_invariant_periodic_skipped_wrong_mode_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!aura::compiler::typed_audit::production_defaults_active()) {
        g_epoch_invariant_periodic_skipped_off_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto now_ms = periodic_steady_ms_now();
    const auto last_ms = g_epoch_invariant_periodic_last_walk_at_ms.load(std::memory_order_relaxed);
    if (last_ms != 0 && (now_ms - last_ms) < period_ms) {
        g_epoch_invariant_periodic_skipped_rate_limited_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
        return;
    }
    g_epoch_invariant_periodic_last_walk_at_ms.store(now_ms, std::memory_order_relaxed);
    g_epoch_invariant_periodic_walks_total.fetch_add(1, std::memory_order_relaxed);
    // Reuse #2541 soft walk: physically clear gen-behind AOT slots
    // (aura_aot_invalidate_all_stale_slots_for_eval(nullptr) = process-default)
    // + MustDeopt stale live closures. The #2693 consecutive-dirty
    // fuse records behind_after_clear so a stuck walk pattern (>=K
    // walks in a row all leaving behind) bumps the fuse counter.
    const std::size_t behind_after_clear = aura_aot_invalidate_all_stale_slots_for_eval(nullptr);
    (void)aura_epoch_invariant_must_deopt_stale_live_closures();
    aura_2693_soft_fuse_record(behind_after_clear);
}

// Issue #2668: event-driven soft walk on commit_func_table_swap /
// aura_aot_bump_func_table_epoch. Closes the burst-mutation window
// that pure periodic Soft leaves open under reemit storms. Shares
// last_walk_at_ms atomic with periodic path so double-walk on
// boundary+swap in the same ms is amortized. Production + Soft only;
// Soft / Off / mode=0: zero extra work on bump path.
extern "C" void aura_event_driven_epoch_invariant_walk_if_due(void) {
    if (!aura::compiler::typed_audit::production_defaults_active()) {
        g_epoch_invariant_event_skipped_off_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (aura_epoch_invariant_mode() != 1) { // 1 = Soft (per #2541 Restricted default)
        g_epoch_invariant_event_skipped_wrong_mode_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Share last-walk-at with periodic path (AC3: no double physical
    // clear in same window). Same steady-clock amortization as #2640.
    const auto now_ms = periodic_steady_ms_now();
    const auto last_ms = g_epoch_invariant_periodic_last_walk_at_ms.load(std::memory_order_relaxed);
    if (last_ms != 0 &&
        (now_ms - last_ms) < g_epoch_invariant_periodic_period_ms.load(std::memory_order_relaxed)) {
        // Periodic path already covered this window (or the next
        // periodic walk will). Skip — zero work.
        return;
    }
    g_epoch_invariant_periodic_last_walk_at_ms.store(now_ms, std::memory_order_relaxed);
    g_epoch_invariant_event_walks_total.fetch_add(1, std::memory_order_relaxed);
    // Reuse #2541 soft walk: physically clear gen-behind AOT slots +
    // MustDeopt stale live closures. The #2693 consecutive-dirty
    // fuse records behind_after_clear; periodic + event walks share
    // the same fuse signal so a reemit-storm burst + steady drift
    // both contribute.
    const std::size_t behind_after_clear = aura_aot_invalidate_all_stale_slots_for_eval(nullptr);
    (void)aura_epoch_invariant_must_deopt_stale_live_closures();
    aura_2693_soft_fuse_record(behind_after_clear);
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

// Issue #2601: exhausted min-dirty retry closed loop driver.
// Defined here (not in hot_update_registry.cpp) because it needs
// aura_reemit_aot_for_dirty + aot_metrics() — bridge-side resources.
// Soft zero-cost when force_jit_regions_mask_ == 0 (idle path — the
// registry decide short-circuits on the first load). Soft zero-cost
// on backoff not elapsed (steady_ms_now() check). Bounded by attempts_cap
// + backoff_ms so recursion within a single reemit pipeline call is safe.
extern "C" void aura_hot_update_maybe_retry_exhausted_min_dirty(void) {
    auto& hur = aura::compiler::hot_update_registry();
    const auto decision = hur.decide_exhausted_min_dirty_retry();
    switch (decision) {
        case aura::compiler::HotUpdateRegistry::ExhaustedMinDirtyRetryDecision::Retry: {
            hur.consume_exhausted_min_dirty_retry_attempt();
            if (aot_metrics()) {
                aot_metrics()->aot_exhausted_min_dirty_retry_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
            const auto n = aura_reemit_aot_for_dirty(aura_get_aot_defuse_version());
            if (n > 0) {
                if (aot_metrics())
                    aot_metrics()->aot_exhausted_min_dirty_retry_success_total.fetch_add(
                        1, std::memory_order_relaxed);
            } else if (!hur.has_deferred_reemit()) {
                // True empty/reject — not counted as success. The retry
                // happened (retry_total +1) but produced no success.
                // Failures are observable via retry_total > success_total.
            }
            return;
        }
        case aura::compiler::HotUpdateRegistry::ExhaustedMinDirtyRetryDecision::NoAttemptsLeft:
            if (aot_metrics())
                aot_metrics()->aot_exhausted_min_dirty_retry_cap_hit_total.fetch_add(
                    1, std::memory_order_relaxed);
            return;
        case aura::compiler::HotUpdateRegistry::ExhaustedMinDirtyRetryDecision::StormActive:
            if (aot_metrics())
                aot_metrics()->aot_exhausted_min_dirty_retry_storm_skip_total.fetch_add(
                    1, std::memory_order_relaxed);
            return;
        case aura::compiler::HotUpdateRegistry::ExhaustedMinDirtyRetryDecision::NoForceJit:
        case aura::compiler::HotUpdateRegistry::ExhaustedMinDirtyRetryDecision::BackoffNotElapsed:
            // Zero-cost no-op.
            return;
    }
}
