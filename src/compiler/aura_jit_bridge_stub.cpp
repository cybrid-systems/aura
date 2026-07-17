// aura_jit_bridge_stub.cpp — minimal C-linkage stubs for tests that
// link aura_jit.cpp + aura_jit_runtime.cpp but don't include the
// full aura_jit_bridge.cpp (which is a module .cpp and can't be
// compiled standalone). The stub provides the symbols aura_jit.cpp
// references (e.g. aura_notify_jit_unhandled_opcode) so the link
// succeeds. The full aura_jit_bridge.cpp provides the production
// implementation; the stub is for test binaries that don't need
// the production code path.
//
// Issue #226 follow-up: Without this stub, test_spec_jit /
// test_jit_metrics / test_jit_concurrent_compile / test_jit_consistency
// fail to link with "undefined reference to
// aura_notify_jit_unhandled_opcode". The production
// aura_jit_bridge.cpp can't be linked because it uses `import std;`
// and is part of the aura compiler module.

#include "aura_jit_bridge.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" void aura_notify_jit_unhandled_opcode(const char* fn_name) {
    (void)fn_name;
    // Stub: no-op in test binaries. The full implementation
    // in aura_jit_bridge.cpp increments
    // CompilerMetrics::unhandled_opcode_count and emits a
    // diagnostic via CompilerService::repl_diagnostic.
}

// Additional stubs for symbols that aura_jit.cpp may reference.
// Add as needed when new tests fail to link.
static void* g_aot_metrics_stub = nullptr;
static std::uint64_t g_aot_metrics_lazy_stub = 0;
static std::uint64_t g_aot_metrics_explicit_stub = 0;
// Issue #243: aura_jit.cpp reads defuse epoch at emit time.
// Full impl lives in aura_jit_bridge.cpp; light JIT test binaries
// only need a process-local counter so the link succeeds.
//
// Weak: test_issues_light also compiles test_issue_243.cpp, which
// provides its own strong definitions for the set/get round-trip
// AC. Weak stubs lose when those are present, and still satisfy
// aura_jit.cpp when they are not (test_spec_jit / test_jit_*).
static std::uint64_t g_aot_defuse_version_stub = 0;

extern "C" __attribute__((weak)) void aura_set_aot_defuse_version(std::uint64_t v) {
    g_aot_defuse_version_stub = v;
}

extern "C" __attribute__((weak)) std::uint64_t aura_get_aot_defuse_version(void) {
    return g_aot_defuse_version_stub;
}

// ── Weak stubs for AOT region / module / eval isolation APIs ──
// CompilerService (in aura_test_objects) references these; light
// bundles don't link aura_jit_bridge.cpp. Weak so production bridge
// (or test_issue_243 strong defs) wins when present.
static std::uint64_t g_aot_region_mask_stub = 0;
static std::uint64_t g_module_version_stub = 0;
static aura_jit_unhandled_invalidate_fn_t g_jit_unhandled_invalidate_fn_stub = nullptr;

extern "C" __attribute__((weak)) void aura_set_aot_region_mask(std::uint64_t mask) {
    g_aot_region_mask_stub = mask;
}
extern "C" __attribute__((weak)) std::uint64_t aura_get_aot_region_mask(void) {
    return g_aot_region_mask_stub;
}
extern "C" __attribute__((weak)) void aura_set_aot_emit_region_mask(std::uint64_t mask) {
    g_aot_region_mask_stub = mask;
}
extern "C" __attribute__((weak)) void aura_set_aot_region_mask_for_eval(void* /*eval*/,
                                                                        std::uint64_t mask) {
    g_aot_region_mask_stub = mask;
}
extern "C" __attribute__((weak)) std::uint64_t aura_get_aot_region_mask_for_eval(void* /*eval*/) {
    return g_aot_region_mask_stub;
}
extern "C" __attribute__((weak)) void aura_set_module_version(std::uint64_t v) {
    g_module_version_stub = v;
}
extern "C" __attribute__((weak)) std::uint64_t aura_get_module_version(void) {
    return g_module_version_stub;
}
extern "C" __attribute__((weak)) void aura_set_module_version_for_eval(void* /*eval*/,
                                                                       std::uint64_t v) {
    g_module_version_stub = v;
}
extern "C" __attribute__((weak)) std::uint64_t aura_get_module_version_for_eval(void* /*eval*/) {
    return g_module_version_stub;
}
extern "C" __attribute__((weak)) void aura_set_aot_defuse_version_for_eval(void* /*eval*/,
                                                                           std::uint64_t v) {
    g_aot_defuse_version_stub = v;
}
extern "C" __attribute__((weak)) std::uint64_t
aura_get_aot_defuse_version_for_eval(void* /*eval*/) {
    return g_aot_defuse_version_stub;
}
extern "C" __attribute__((weak)) void aura_cleanup_aot_state(void* /*eval*/) {}
extern "C" __attribute__((weak)) std::uint64_t aura_aot_state_map_size(void) {
    return 0;
}
static std::atomic<std::uint64_t> g_aot_table_epoch_stub{1};
extern "C" __attribute__((weak)) std::uint64_t aura_aot_func_table_epoch(void) {
    return g_aot_table_epoch_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void aura_aot_bump_func_table_epoch(void) {
    g_aot_table_epoch_stub.fetch_add(1, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) bool
aura_is_jit_closure_fresh(std::uint64_t captured_bridge_epoch,
                          std::uint64_t captured_defuse_or_env_version) {
    const auto cur_b = g_aot_table_epoch_stub.load(std::memory_order_relaxed);
    const auto cur_d = g_aot_defuse_version_stub;
    const bool bridge_ok = (captured_bridge_epoch == 0) || (captured_bridge_epoch == cur_b);
    const bool defuse_ok =
        (captured_defuse_or_env_version == 0) || (captured_defuse_or_env_version == cur_d);
    return bridge_ok && defuse_ok;
}
extern "C" __attribute__((weak)) void aura_jit_closure_record_dual_check(void) {}
extern "C" __attribute__((weak)) void aura_jit_closure_record_stale_deopt(void) {}
extern "C" __attribute__((weak)) void aura_jit_closure_record_safe_fallback(void) {}
extern "C" __attribute__((weak)) std::uint64_t aura_jit_closure_dual_check_total(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_jit_closure_stale_deopt_total(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_jit_closure_safe_fallbacks(void) {
    return 0;
}
extern "C" __attribute__((weak)) void aura_set_jit_batch_deopt_target(void* /*jit*/) {}
extern "C" __attribute__((weak)) std::size_t aura_jit_batch_deopt_for(const char* /*name*/,
                                                                      std::uint64_t /*epoch*/) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_jit_batch_deopt_for_total(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_jit_batch_deopt_entries_marked(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_jit_deopt_pending_count(void) {
    return 0;
}
extern "C" __attribute__((weak)) int aura_jit_is_deopt_pending(const char* /*name*/) {
    return 0;
}
extern "C" __attribute__((weak)) std::size_t
aura_jit_walk_active_closures(std::uint64_t /*current_bridge_epoch*/) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_jit_walk_active_closures_total(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_jit_walk_active_closures_stale_found(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_jit_get_current_bridge_epoch(void) {
    return 0;
}
extern "C" __attribute__((weak)) int
aura_jit_is_fn_epoch_stale(const char* /*name*/, std::uint64_t /*current_bridge_epoch*/) {
    return 0;
}
extern "C" __attribute__((weak)) std::int64_t aura_jit_deopt_to_interpreter(const char* /*name*/) {
    return 0;
}
extern "C" __attribute__((weak)) int aura_jit_guard_shape_epoch_check(const char* /*name*/) {
    return 0;
}
extern "C" __attribute__((weak)) int
aura_jit_linear_epoch_safety_check(const char* /*fn_name*/, std::uint8_t /*linear_state*/,
                                   std::uint32_t /*opcode*/) {
    return 0;
}
extern "C" __attribute__((weak)) void
aura_jit_set_linear_env_context(std::uint32_t /*env_id*/, std::uint64_t /*frame_version*/) {}
extern "C" __attribute__((weak)) void aura_jit_clear_linear_env_context(void) {}
extern "C" __attribute__((weak)) void
aura_set_linear_post_mutate_enforce_fn(aura_linear_post_mutate_enforce_fn_t /*fn*/,
                                       void* /*user_data*/) {}
extern "C" __attribute__((weak)) int aura_jit_linear_post_mutate_enforce(std::uint32_t /*env_id*/) {
    return 0;
}
extern "C" __attribute__((weak)) void
aura_set_linear_live_closure_scan_fn(aura_linear_live_closure_scan_fn_t /*fn*/,
                                     void* /*user_data*/) {}
extern "C" __attribute__((weak)) int aura_jit_linear_live_closure_scan(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_aot_last_commit_epoch(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_reemit_aot_for_dirty(std::uint64_t /*v*/) {
    return 0;
}
extern "C" __attribute__((weak)) void
aura_set_reemit_candidate_fn(aura_reemit_candidate_fn_t /*fn*/, void* /*userdata*/) {}
extern "C" __attribute__((weak)) std::uint64_t aura_reemit_dirty_count(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_reemit_region_filtered_skips(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_reemit_closure_dep_count(void) {
    return 0;
}
extern "C" __attribute__((weak)) bool aura_reload_aot_module(const char* /*path*/,
                                                             std::uint64_t /*v*/) {
    return false;
}
extern "C" __attribute__((weak)) bool
aura_reload_aot_module_for_eval(void* /*eval*/, const char* /*path*/, std::uint64_t /*v*/) {
    return false;
}
extern "C" __attribute__((weak)) void aura_register_fn_tracked(std::int64_t /*id*/,
                                                               std::int64_t /*ptr*/) {}
extern "C" __attribute__((weak)) void
aura_set_jit_unhandled_invalidate_fn(aura_jit_unhandled_invalidate_fn_t fn) {
    g_jit_unhandled_invalidate_fn_stub = fn;
}
extern "C" __attribute__((weak)) std::uint64_t aura_jit_fallback_count_v_read(void) {
    return 0;
}

extern "C" void aura_set_aot_metrics(void* metrics) {
    g_aot_metrics_stub = metrics;
    if (metrics)
        ++g_aot_metrics_explicit_stub;
}

// Issue #1443: long-mutation policy knob stubs (test binaries link this
// instead of full aura_jit_bridge.cpp). All are no-ops in tests.
extern "C" void aura_set_long_mutation_threshold_us(std::uint64_t us) {
    (void)us;
}
extern "C" std::uint64_t aura_get_long_mutation_threshold_us(void) {
    return 500'000;
}
extern "C" void aura_set_long_mutation_strict_mode(int on) {
    (void)on;
}
extern "C" std::uint64_t aura_get_long_mutation_strict_mode(void) {
    return 0;
}
extern "C" void aura_set_max_extreme_mutation_us(std::uint64_t us) {
    (void)us;
}

// Issue #1443 AC3 follow-up + #1445 AC6: scheduler hook stubs.
extern "C" void aura_set_long_mutation_scheduler_hook(aura_long_mutation_scheduler_hook_fn fn) {
    (void)fn;
}
extern "C" void aura_invoke_long_mutation_scheduler_hook(std::uint64_t fiber_id,
                                                         std::uint64_t duration_us) {
    (void)fiber_id;
    (void)duration_us;
}
extern "C" std::uint64_t aura_long_mutation_scheduler_hook_calls_total(void) {
    return 0;
}

extern "C" void aura_ensure_aot_metrics(void* metrics) {
    if (metrics && !g_aot_metrics_stub) {
        g_aot_metrics_stub = metrics;
        ++g_aot_metrics_lazy_stub;
    }
}

extern "C" void* aura_get_aot_metrics(void) {
    return g_aot_metrics_stub;
}

extern "C" std::uint64_t aura_aot_metrics_lazy_init_total(void) {
    return g_aot_metrics_lazy_stub;
}

extern "C" std::uint64_t aura_aot_metrics_explicit_sets_total(void) {
    return g_aot_metrics_explicit_stub;
}

extern "C" void aura_jit_epoch_acquire_fence(void) {
    std::atomic_thread_fence(std::memory_order_acquire);
}

extern "C" void aura_jit_linear_post_invalidate_safety(std::uint8_t linear_state,
                                                       std::uint32_t opcode) {
    (void)linear_state;
    (void)opcode;
}

// Fiber/eval paths in aura_test_objects reference these AOT hooks
// (defined in aura_jit_bridge.cpp). Light bundles link runtime heaps
// but not the full bridge — provide no-op stubs so link succeeds.
extern "C" bool aura_aot_probe_checkpoint_version(std::uint64_t defuse_version,
                                                  std::uint64_t bridge_epoch) {
    (void)defuse_version;
    (void)bridge_epoch;
    return false; // no drift
}

extern "C" void aura_aot_record_deopt_on_steal(void) {
    // Stub: production increments AOT deopt metrics.
}

// Issue #1369 stubs — full impl in aura_jit_bridge.cpp
extern "C" std::uint64_t aura_aot_probe_fn_version(void* dl_handle, const char* original_name) {
    (void)dl_handle;
    (void)original_name;
    return ~std::uint64_t{0};
}

extern "C" bool aura_aot_fn_version_is_stale(void* dl_handle, const char* original_name,
                                             std::uint64_t expected) {
    (void)dl_handle;
    (void)original_name;
    (void)expected;
    return false;
}

extern "C" bool aura_aot_parse_version_suffix(const char* mangled, std::uint64_t* out_version) {
    if (!mangled || !out_version)
        return false;
    // Minimal parse: trailing _vN
    const char* p = std::strrchr(mangled, 'v');
    if (!p || p == mangled || *(p - 1) != '_')
        return false;
    char* end = nullptr;
    unsigned long long v = std::strtoull(p + 1, &end, 10);
    if (!end || end == p + 1 || *end != '\0')
        return false;
    *out_version = static_cast<std::uint64_t>(v);
    return true;
}

extern "C" bool aura_aot_mangle_version_is_stale(const char* mangled, std::uint64_t expected) {
    std::uint64_t got = 0;
    if (!aura_aot_parse_version_suffix(mangled, &got))
        return true;
    return got != expected;
}
