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
#include "core/atomic_fence_port.h"
#include "hot_update_registry.hh"
#include "observability_metrics.h"

extern "C" void* aura_get_aot_metrics(void);

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
// AOT metrics pointer lives in runtime_ssot.cpp (libaura_tl_arena.so).
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

// Issue #2091: live env_frame_version + linear_state_fingerprint
// C ABI lives in runtime_ssot.cpp (libaura_tl_arena.so). Do not
// weak-stub here — a light-JIT copy would split-brain against the
// SSOT atomics evaluator_env.cpp writes.

// Issue #2091: weak stubs for the force flag bridge + the env-var
// seed call. The stub returns 0 (force off) by default so tests
// without the production bridge get the legacy shape.
static bool g_aot_force_env_linear_suffix_stub = false;

extern "C" __attribute__((weak)) void aura_aot_set_force_env_linear_suffix(int v) {
    g_aot_force_env_linear_suffix_stub = (v != 0);
}

extern "C" __attribute__((weak)) int aura_aot_get_force_env_linear_suffix(void) {
    return g_aot_force_env_linear_suffix_stub ? 1 : 0;
}

// Issue #1485 C2-wire: weak stubs for aura_set/get_current_bridge_epoch.
// Production impl is in aura_jit_bridge.cpp; this ensures test
// binaries that don't link aura_jit_bridge.cpp (test_spec_jit,
// test_jit_*) still compile. Weak so production impl wins when
// both are linked.
// Issue #1654: std::atomic<std::uint64_t> replaces the plain uint64_t —
// mirrors the production aura_jit_bridge.cpp fix (closes the C++ memory
// model data race that defeated #1485's C2-wire fix-up intent on
// weakly-ordered architectures). The stub atomicity is uniform with the
// production impl so test binaries that don't link the production
// impl still benefit from the same acq/rel protocol.
static std::atomic<std::uint64_t> g_current_bridge_epoch_stub{0};

extern "C" __attribute__((weak)) void aura_set_current_bridge_epoch(std::uint64_t v) {
    g_current_bridge_epoch_stub.store(v, std::memory_order_release);
}

extern "C" __attribute__((weak)) std::uint64_t aura_get_current_bridge_epoch(void) {
    return g_current_bridge_epoch_stub.load(std::memory_order_acquire);
}

// Issue #3181 follow-up: weak stubs for aura_hot_update_bump_*. Production
// impl is in aura_jit_bridge.cpp (g_current_bridge_epoch / g_aot_defuse_version
// statics); this ensures light JIT test binaries (test_unquote_splicing_hygiene
// etc. — anything linking libaura_jit_light_test_objects.so, which contains
// aura_jit_bridge_stub.cpp instead of aura_jit_bridge.cpp) still link. Without
// these, --no-allow-shlib-undefined rejects libaura_test_objects.so's call sites
// in hot_update_registry.cpp (notify_dirty_define → bump_bridge_epoch /
// bump_defuse_version). Weak so production impl wins when both are linked.
// Pre-existing infra gap (#3181 ship surfaced it via macro_expansion clone-walk
// test extension in tests/compiler/test_unquote_splicing_hygiene.cpp).
extern "C" __attribute__((weak)) void aura_hot_update_bump_bridge_epoch(void) noexcept {
    g_current_bridge_epoch_stub.fetch_add(1, std::memory_order_acq_rel);
}

extern "C" __attribute__((weak)) void aura_hot_update_bump_defuse_version(void) noexcept {
    g_aot_defuse_version_stub++;
}

// Issue #1485 C2: per-closure provenance stubs. Production impl is in
// aura_jit_runtime.cpp; test binaries that don't link it (light JIT
// bundles) get the degenerate return-0 path. Weak so production impl
// wins when both are linked.
// Issue #1706: exists stub always 0 (no table in light bundles).
extern "C" __attribute__((weak)) int aura_closure_exists(std::int64_t /*closure_id*/) {
    return 0;
}

// Issue #2017: weak no-op when full aura_jit_runtime is not linked.
extern "C" __attribute__((weak)) void
aura_invalidate_closure_cache_for(std::int64_t /*closure_id*/) {}

// Issue #2042: weak no-op bulk clear when full runtime is not linked.
extern "C" __attribute__((weak)) void aura_invalidate_all_closure_caches(void) {}

// Issue #2043: weak linear-ownership epoch stubs (light bundles).
static std::atomic<std::uint64_t> g_linear_ownership_epoch_stub{0};
extern "C" __attribute__((weak)) void aura_set_linear_ownership_epoch(std::uint64_t v) {
    g_linear_ownership_epoch_stub.store(v, std::memory_order_release);
}
extern "C" __attribute__((weak)) std::uint64_t aura_get_linear_ownership_epoch(void) {
    return g_linear_ownership_epoch_stub.load(std::memory_order_acquire);
}

extern "C" __attribute__((weak)) std::uint64_t aura_closure_cache_generation_mismatch_total(void) {
    return 0;
}

extern "C" __attribute__((weak)) std::uint64_t
aura_get_closure_bridge_epoch(std::int64_t /*closure_id*/) {
    return 0;
}

extern "C" __attribute__((weak)) std::uint64_t
aura_get_closure_defuse_version(std::int64_t /*closure_id*/) {
    return 0;
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
extern "C" __attribute__((weak)) std::uint64_t aura_get_aot_emit_region_mask(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_get_aot_emit_region_mask_preferred(void) {
    return 0;
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
// Per-eval env_frame_version mirror (tests call with nullptr → process stub).
static std::uint64_t g_aot_env_frame_version_for_eval_stub = 0;
extern "C" __attribute__((weak)) void aura_set_aot_env_frame_version_for_eval(void* /*eval*/,
                                                                              std::uint64_t v) {
    g_aot_env_frame_version_for_eval_stub = v;
}
extern "C" __attribute__((weak)) std::uint64_t
aura_get_aot_env_frame_version_for_eval(void* /*eval*/) {
    return g_aot_env_frame_version_for_eval_stub;
}
extern "C" __attribute__((weak)) void aura_cleanup_aot_state(void* /*eval*/) {}
// aura_aot_state_map_size / aura_aot_func_table_epoch /
// aura_aot_bump_func_table_epoch live in runtime_ssot.cpp.
extern "C" __attribute__((weak)) void aura_aot_note_cross_eval_epoch_force_bump(void) {}
extern "C" __attribute__((weak)) void aura_aot_note_cross_eval_hard_owner_scoped(void) {}
extern "C" __attribute__((weak)) int aura_aot_cross_eval_hard_owner_scoped_armed(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t
cross_eval_epoch_action_throttled_total_v_read(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t cross_eval_hard_owner_scoped_total_v_read(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t cross_eval_hard_global_bump_total_v_read(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t reemit_owner_missing_reject_total_v_read(void) {
    return 0;
}
extern "C" __attribute__((weak)) void aura_aot_mark_peer_slots_soft_stale(void* /*owner*/) {}
// Issue #3377: production impl is in aura_jit_bridge.cpp. Light JIT
// binaries (libaura_jit_light_test_objects.so) must still satisfy
// --no-allow-shlib-undefined for hot_update_registry.cpp's call from
// hard_invalidate_via_facade. Weak so the production definition wins
// when both are linked (test_owner_scoped_hard_invalidate_slot_clear).
extern "C" __attribute__((weak)) void
aura_aot_invalidate_owner_slot_for_func_id(std::int64_t /*func_id*/,
                                           void* /*owner_eval*/) noexcept {}
extern "C" __attribute__((weak)) int aura_aot_slot_is_soft_stale(std::int64_t /*func_id*/) {
    return 0;
}
// Issue #3300: name-level peer pure-JIT soft-stale (light-link no-ops;
// zero-cost when the production table is empty).
extern "C" __attribute__((weak)) void aura_aot_mark_peer_jit_name_soft_stale(const char* /*name*/) {
}
extern "C" __attribute__((weak)) int aura_aot_peer_jit_name_is_soft_stale(const char* /*name*/) {
    return 0;
}
extern "C" __attribute__((weak)) void
aura_aot_clear_peer_jit_name_soft_stale(const char* /*name*/) {}
extern "C" __attribute__((weak)) void aura_aot_note_peer_jit_name_soft_stale_deopt(void) {}
extern "C" __attribute__((weak)) std::uint64_t peer_jit_name_soft_stale_mark_total_v_read(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t peer_jit_name_soft_stale_clear_total_v_read(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t peer_jit_name_soft_stale_deopt_total_v_read(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint32_t peer_jit_name_soft_stale_live_v_read(void) {
    return 0;
}
// Issue #3351: peer IR-cache name soft-stale (light-link no-ops).
extern "C" __attribute__((weak)) void aura_aot_mark_peer_ir_name_soft_stale(const char* /*name*/) {}
extern "C" __attribute__((weak)) std::uint64_t
aura_aot_peer_ir_name_stale_gen(const char* /*name*/) {
    return 0;
}
extern "C" __attribute__((weak)) int aura_aot_peer_ir_name_is_soft_stale(const char* /*name*/) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t peer_ir_name_soft_stale_mark_total_v_read(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint32_t peer_ir_name_soft_stale_live_v_read(void) {
    return 0;
}
extern "C" __attribute__((weak)) int aura_aot_peer_name_stale_overflow(void) {
    return 0;
}
extern "C" __attribute__((weak)) bool
aura_is_jit_closure_fresh(std::uint64_t captured_bridge_epoch,
                          std::uint64_t captured_defuse_or_env_version,
                          std::uint64_t captured_table_epoch) {
    const auto cur_c = aura_get_current_bridge_epoch();
    const auto cur_b = aura_aot_func_table_epoch();
    const auto cur_d = g_aot_defuse_version_stub;
    // Issue #2930: match full bridge semantics — captured==0 while tracking
    // active is NOT fresh (unstamped observed during tracking), so stale
    // closures reach the cross-COW soft/hard path instead of skipping it.
    // Issue #3447: C-bridge AND table (owner-scoped table may stay frozen).
    // Issue #3471: independent table stamp; no C-bridge wash.
    auto domain_ok = [](std::uint64_t captured, std::uint64_t current) noexcept {
        if (current == 0)
            return true; // tracking inactive for this domain
        if (captured == 0)
            return false; // unstamped while tracking active -> stale
        return captured == current;
    };
    const bool c_ok = domain_ok(captured_bridge_epoch, cur_c);
    bool table_ok = true;
    if (captured_table_epoch != 0)
        table_ok = domain_ok(captured_table_epoch, cur_b);
    else if (cur_c == 0)
        table_ok = domain_ok(captured_bridge_epoch, cur_b);
    return c_ok && table_ok && domain_ok(captured_defuse_or_env_version, cur_d);
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
// Light-link counter so remount force-deopt tests (#2503/#2894) can observe
// named batch_deopt invocations without the full AuraJIT Orc path.
static std::atomic<std::uint64_t> g_batch_deopt_for_total_stub{0};
extern "C" __attribute__((weak)) std::size_t aura_jit_batch_deopt_for(const char* name,
                                                                      std::uint64_t /*epoch*/) {
    if (name && name[0] != '\0')
        g_batch_deopt_for_total_stub.fetch_add(1, std::memory_order_relaxed);
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_jit_batch_deopt_for_total(void) {
    return g_batch_deopt_for_total_stub.load(std::memory_order_relaxed);
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
// Issue #3186 / #3224 / #3343: light-link stubs for the JIT commit_readiness
// / Move-Drop elision / post-mutate enforce bridges. Soft / Off / missing
// production-defaults probe: allow / pass-through (zero extra cost).
// Production defaults: fail-closed (elision blocked, IR entry refuse,
// post-mutate unsafe) so a JIT-less production binary cannot half-green
// when the strong aura_jit_bridge.cpp symbols are not resolved.
extern "C" int aura_production_defaults_active_probe() noexcept __attribute__((weak));

[[nodiscard]] static bool stub_production_defaults_active() noexcept {
    if (!aura_production_defaults_active_probe)
        return false;
    return aura_production_defaults_active_probe() != 0;
}

extern "C" __attribute__((weak)) int aura_jit_linear_move_drop_elision_ok(void) {
    // Issue #3343: production weak stub must not return allow.
    if (stub_production_defaults_active())
        return 0;
    return 1;
}
extern "C" __attribute__((weak)) int aura_jit_ir_typed_entry_commit_readiness_ok(void) {
    // Issue #3343: production weak stub must refuse / deopt.
    if (stub_production_defaults_active())
        return 0;
    return 1;
}
// Issue #3419: do NOT define aura_abi_strong_ir_typed_entry_v here.
// Light-link puts this TU in a separate DSO searched before
// aura_test_objects; a weak marker in that DSO wins over the strong
// T in evaluator_fiber_mutation.cpp (ELF first-definition). Weak 0
// lives in fiber_bridge.cpp (same DSO as the strong T, so strong
// wins). #3343 probe-linear follows the same split.
extern "C" __attribute__((weak)) void
aura_jit_set_linear_env_context(std::uint32_t /*env_id*/, std::uint64_t /*frame_version*/) {}
extern "C" __attribute__((weak)) void aura_jit_clear_linear_env_context(void) {}
extern "C" __attribute__((weak)) void
aura_set_linear_post_mutate_enforce_fn(aura_linear_post_mutate_enforce_fn_t /*fn*/,
                                       void* /*user_data*/) {}
extern "C" __attribute__((weak)) int aura_jit_linear_post_mutate_enforce(std::uint32_t /*env_id*/) {
    // Issue #3343: 1 = unsafe / deopt under production; 0 = pass-through Soft.
    if (stub_production_defaults_active())
        return 1;
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
// Issue #2299 / #2606: register + reemit owner TLS (weak stubs —
// production aura_jit_bridge.cpp owns real TLS). Light-linked tests
// (chaos #2352, etc.) need these for ReemitEvalOwnerGuard.
extern "C" __attribute__((weak)) void aura_aot_set_register_owner_eval(void* /*eval_ptr*/) {}
extern "C" __attribute__((weak)) void* aura_aot_get_register_owner_eval(void) {
    return nullptr;
}
extern "C" __attribute__((weak)) void aura_aot_set_reemit_owner_eval(void* /*eval_ptr*/) {}
extern "C" __attribute__((weak)) void* aura_aot_get_reemit_owner_eval(void) {
    return nullptr;
}
extern "C" __attribute__((weak)) void
aura_set_reemit_candidate_fn(aura_reemit_candidate_fn_t /*fn*/, void* /*userdata*/) {}
extern "C" __attribute__((weak)) void aura_set_aot_emit_fn(aura_aot_emit_fn_t /*fn*/,
                                                           void* /*userdata*/) {}
// Issue #3373: light-link stubs. Soft / Off tests never push so ring stays empty.
extern "C" __attribute__((weak)) int aura_install_production_dirty_iterator(void) {
    return 0;
}
extern "C" __attribute__((weak)) int aura_production_dirty_ring_push(const char* /*name*/,
                                                                     std::uint64_t /*region*/,
                                                                     int /*from_closure_capture*/) {
    return 0;
}
extern "C" __attribute__((weak)) void aura_production_dirty_ring_reset_for_test(void) {}
extern "C" __attribute__((weak)) std::uint64_t aura_production_dirty_ring_pushed_total(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_production_dirty_ring_dropped_total(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_production_dirty_ring_popped_total(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_production_dirty_ring_depth(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint32_t
aura_get_or_preserve_stable_func_id(const char* /*name*/, int* out_preserved) {
    if (out_preserved)
        *out_preserved = 0;
    return 0;
}
extern "C" __attribute__((weak)) std::uint32_t aura_lookup_stable_func_id(const char* /*name*/) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_stable_func_id_map_size(void) {
    return 0;
}
extern "C" __attribute__((weak)) void aura_clear_stable_func_id_map(void) {}
// Issue #2670: per-eval stable_func_id map weak stubs (light test binaries
// without production bridge). Return 0 / no-op so legacy single-workspace
// callers see no behavioral change in light tests.
extern "C" __attribute__((weak)) std::uint32_t
aura_get_or_preserve_stable_func_id_for_eval(void* /*eval_ptr*/, const char* /*name*/,
                                             int* out_preserved) {
    if (out_preserved)
        *out_preserved = 0;
    return 0;
}
extern "C" __attribute__((weak)) std::uint32_t
aura_lookup_stable_func_id_for_eval(void* /*eval_ptr*/, const char* /*name*/) {
    return 0;
}
extern "C" __attribute__((weak)) void aura_clear_stable_func_id_map_for_eval(void* /*eval_ptr*/) {}
// Issue #2692: mismatch counter weak stub (light tests observe no-op).
extern "C" __attribute__((weak)) void aura_bump_cross_eval_sid_owner_mismatch_total(void) {}
// Issue #2713: cross-eval epoch tax observability weak stubs (production
// in aura_jit_bridge.cpp). Light tests see zero tax / not wired.
extern "C" __attribute__((weak)) std::uint64_t cross_eval_epoch_bump_total_v_read(void) {
    return 0;
}
extern "C" __attribute__((weak)) void* last_cross_eval_epoch_bump_owner_v_read(void) {
    return nullptr;
}
extern "C" __attribute__((weak)) std::uint32_t cross_eval_epoch_bump_wired_v_read(void) {
    return 0;
}
// Issue #2092: legacy name-fallback toggle (off by default in strict
// tests). Production aura_jit_runtime.cpp owns the real atomic; the
// weak stub returns 0 so light test binaries without the production
// TU behave as the strict default (no name fallback).
extern "C" __attribute__((weak)) void aura_set_remap_name_fallback_enabled(int /*v*/) {}
extern "C" __attribute__((weak)) int aura_get_remap_name_fallback_enabled(void) {
    return 0;
}
// Issue #2370: weak stubs for PerEval storm TLS + SpecJIT counters.
extern "C" __attribute__((weak)) void aura_set_storm_eval_context(void* /*eval_ptr*/) noexcept {}
extern "C" __attribute__((weak)) void* aura_get_storm_eval_context(void) noexcept {
    return nullptr;
}
extern "C" __attribute__((weak)) std::uint64_t aura_specjit_storm_clear_total_v_read(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t
aura_specjit_per_eval_storm_clear_total_v_read(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t
aura_specjit_per_eval_storm_skip_foreign_total_v_read(void) {
    return 0;
}
// Issue #2092: weak stub for the name-fallback counter bumper (the
// real impl lives in aura_jit_bridge.cpp; this satisfies light test
// binaries that don't link the production TU).
extern "C" __attribute__((weak)) void
aura_bump_live_closure_remap_name_fallback_total(std::uint64_t /*n*/) {}
// Issue #2602 / #2628: weak stub for sync remount counter bumper (runtime.cpp).
extern "C" __attribute__((weak)) void
aura_bump_live_closure_sync_remount_totals(std::uint64_t /*ok*/, std::uint64_t /*fail*/) {}
// Issue #2637: anon sync remount counter bumper (sid == 0 branch).
extern "C" __attribute__((weak)) void
aura_bump_live_closure_sync_remount_anon_totals(std::uint64_t /*ok*/, std::uint64_t /*fail*/) {}
// Issue #2637: anon sync remount walk stub (mirrors #2602 named stub above).
extern "C" __attribute__((weak)) void
aura_sync_remount_anon_live_closures(std::uint64_t* ok_count, std::uint64_t* fail_count) {
    if (ok_count)
        *ok_count = 0;
    if (fail_count)
        *fail_count = 0;
}
// Issue #2691: captured-only anon sync remount weak stub (full impl in
// aura_jit_runtime.cpp). Light/test bundles link without the production
// walk.
extern "C" __attribute__((weak)) void
aura_sync_remount_anon_captured_live_closures(std::uint64_t* ok_count, std::uint64_t* fail_count) {
    if (ok_count)
        *ok_count = 0;
    if (fail_count)
        *fail_count = 0;
}
// Issue #2691: counter bumper used by aura_jit_runtime.cpp remount path.
// Production impl is in aura_jit_bridge.cpp; light bundles need a weak
// stub so --no-allow-shlib-undefined does not fail on libaura_jit_light.
extern "C" __attribute__((weak)) void
aura_bump_live_closure_sync_remount_anon_captured_totals(std::uint64_t /*ok*/,
                                                         std::uint64_t /*fail*/) {}
// Issue #2850: pure-anon bounded sync remount weak stubs.
extern "C" __attribute__((weak)) void
aura_bump_live_closure_sync_remount_pure_anon_totals(std::uint64_t /*ok*/,
                                                     std::uint64_t /*skip_budget*/) {}
extern "C" __attribute__((weak)) std::uint64_t aura_sync_remount_pure_anon_budget_default() {
    return 0;
}
// Issue #2893: adaptive pure-anon budget weak stubs (light-link binaries
// that don't compile aura_jit_runtime.cpp still resolve the C ABI).
extern "C" __attribute__((weak)) std::uint64_t aura_sync_remount_pure_anon_budget_base() {
    return 0;
}
extern "C" __attribute__((weak)) void aura_pure_anon_note_walk_outcome(std::uint64_t /*ok*/,
                                                                       std::uint64_t /*skip*/) {}
extern "C" __attribute__((weak)) void
aura_pure_anon_observe_deopt_window(std::uint64_t /*deopt_window_count*/) {}
extern "C" __attribute__((weak)) std::uint64_t aura_sync_remount_pure_anon_budget_current() {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_pure_anon_pressure_bp() {
    return 0;
}
// Issue #2950: pure-anon bg remount queue weak stubs (light-link).
extern "C" __attribute__((weak)) void aura_pure_anon_bg_enqueue(std::int64_t /*closure_id*/) {}
extern "C" __attribute__((weak)) void aura_pure_anon_bg_remount_drain(std::uint64_t /*max_n*/) {}
extern "C" __attribute__((weak)) void aura_pure_anon_maybe_heal_starved(void) noexcept {}
extern "C" __attribute__((weak)) std::uint64_t aura_pure_anon_bg_pending() {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_pure_anon_bg_enqueue_total_v_read() {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_pure_anon_bg_drain_ok_total_v_read() {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_pure_anon_bg_drain_fail_total_v_read() {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_pure_anon_bg_overflow_total_v_read() {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t
aura_pure_anon_bg_overflow_must_deopt_total_v_read() {
    return 0;
}
extern "C" __attribute__((weak)) void aura_test_reset_pure_anon_bg_queue() {}
extern "C" __attribute__((weak)) void aura_bump_pure_anon_bg_totals(std::uint64_t /*enqueue*/,
                                                                    std::uint64_t /*drain_ok*/,
                                                                    std::uint64_t /*drain_fail*/,
                                                                    std::uint64_t /*overflow*/) {}
extern "C" __attribute__((weak)) void
aura_bump_pure_anon_bg_overflow_must_deopt_total(std::uint64_t /*n*/) {}
// Issue #2928: residual round-robin remount weak stubs (light-link).
extern "C" __attribute__((weak)) void
aura_bump_residual_remount_totals(std::uint64_t /*ok*/, std::uint64_t /*budget_skip*/) {}
extern "C" __attribute__((weak)) std::uint64_t aura_residual_remount_budget_default() {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_residual_remount_cursor() {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_residual_remount_ok_total_v_read() {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_residual_remount_budget_skip_total_v_read() {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t
aura_residual_remount_prefer_force_jit_total_v_read() {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_residual_remount_prefer_hit_total_v_read() {
    return 0;
}
extern "C" __attribute__((weak)) void
aura_bump_residual_remount_prefer_totals(std::uint64_t /*enter*/, std::uint64_t /*hit*/) {}
extern "C" __attribute__((weak)) std::uint64_t aura_hot_update_force_jit_regions_mask(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t
aura_hot_update_last_reemit_success_region_mask(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_hot_update_residual_force_mask(void) {
    return 0;
}
extern "C" __attribute__((weak)) int aura_hot_update_relower_success_define_active(void) {
    return 0;
}
extern "C" __attribute__((weak)) int
aura_hot_update_relower_success_covers_define(std::uint32_t /*id*/) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t
aura_hot_update_residual_force_stale_observe_total(void) {
    return 0;
}
extern "C" __attribute__((weak)) void aura_hot_update_observe_residual_force_stale(void) {}
extern "C" __attribute__((weak)) void aura_hot_update_reset_residual_force_observe_for_test(void) {}
extern "C" __attribute__((weak)) std::uint64_t
aura_macro_clone_same_flat_reject_total_v_read(void) noexcept {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t
aura_macro_clone_steal_abort_total_v_read(void) noexcept {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t
aura_macro_clone_last_reject_reason_v_read(void) noexcept {
    return 0;
}
extern "C" __attribute__((weak)) void
aura_test_reset_macro_clone_same_flat_reject_for_test(void) noexcept {}
extern "C" __attribute__((weak)) std::uint64_t
aura_macro_hygiene_last_limit_reason_v_read(void) noexcept {
    return 0;
}
extern "C" __attribute__((weak)) const char*
aura_macro_hygiene_last_limit_reason_string(void) noexcept {
    return "";
}
extern "C" __attribute__((weak)) void
aura_note_macro_hygiene_last_limit_reason(std::uint8_t /*code*/) noexcept {}
extern "C" __attribute__((weak)) void
aura_test_reset_macro_hygiene_last_limit_reason_for_test(void) noexcept {}
extern "C" __attribute__((weak)) void
aura_residual_live_closure_remount_tick(std::uint64_t /*budget*/) {}
extern "C" __attribute__((weak)) void
aura_test_set_residual_remount_budget(std::uint64_t /*budget*/) {}
extern "C" __attribute__((weak)) void
aura_test_set_residual_remount_cursor(std::uint64_t /*cursor*/) {}
extern "C" __attribute__((weak)) void
aura_test_set_closure_stable_func_id(std::int64_t /*closure_id*/, std::uint32_t /*sid*/) {}
// Issue #2978: reemit-success sync covered-named remount weak stubs.
extern "C" __attribute__((weak)) std::uint64_t aura_reemit_success_sync_covered_cap_default() {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_reemit_success_sync_covered_ok_total_v_read() {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t
aura_reemit_success_sync_covered_fail_total_v_read() {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t
aura_reemit_success_sync_covered_cap_hit_total_v_read() {
    return 0;
}
extern "C" __attribute__((weak)) void
aura_sync_remount_covered_named_live_closures(std::uint64_t /*mask*/, std::uint64_t /*cap*/) {}
extern "C" __attribute__((weak)) void
aura_test_set_reemit_success_sync_covered_cap(std::uint64_t /*cap*/) {}
extern "C" __attribute__((weak)) void aura_test_reset_reemit_success_sync_covered_state() {}
extern "C" __attribute__((weak)) void
aura_bump_reemit_success_sync_covered_remount_totals(std::uint64_t /*ok*/, std::uint64_t /*fail*/,
                                                     std::uint64_t /*cap_hit*/) {}
extern "C" __attribute__((weak)) void aura_test_set_residual_remount_force_skip(int /*v*/) {}
extern "C" __attribute__((weak)) void aura_test_reset_residual_remount_state() {}
extern "C" __attribute__((weak)) void
aura_sync_remount_pure_anon_live_closures(std::uint64_t /*budget*/, std::uint64_t* ok_count,
                                          std::uint64_t* skip_budget_count) {
    if (ok_count)
        *ok_count = 0;
    if (skip_budget_count)
        *skip_budget_count = 0;
}
// Issue #2637: env opt-in flag weak stub (default 0 = off per AC1).
extern "C" __attribute__((weak)) int aura_sync_remount_anon_enabled_default() {
    return 0;
}
// Issue #2638: residual cap-hit counter weak stub (zero-cost in light builds).
extern "C" __attribute__((weak)) void
aura_bump_live_closure_residual_cap_hit_total(std::uint64_t /*n*/) {}
// Issue #2638: env opt-in flag weak stub (default 0 = unlimited).
extern "C" __attribute__((weak)) std::uint64_t aura_residual_sid0_cap_default() {
    return 0;
}
// Issue #2175: weak stub for legacy sid=0 backfill counter bumper.
// Production impl is in aura_jit_bridge.cpp; light bundles compile
// aura_jit_runtime.cpp + this stub (not the full bridge). Without
// this symbol, test_issues_light / light_late fail to link with
// undefined reference to aura_bump_live_closure_stable_id_backfill_total.
extern "C" __attribute__((weak)) void
aura_bump_live_closure_stable_id_backfill_total(std::uint64_t /*n*/) {}
// Issue #2605: weak stub for named name-fallback reject bumper.
extern "C" __attribute__((weak)) void
aura_bump_live_closure_named_name_fallback_reject_total(std::uint64_t /*n*/) {}
extern "C" __attribute__((weak)) void
aura_bump_must_deopt_before_next_call_total(std::uint64_t /*n*/) {}
extern "C" __attribute__((weak)) void
aura_bump_must_deopt_force_deopt_success_total(std::uint64_t /*n*/) {}
extern "C" __attribute__((weak)) void
aura_bump_must_deopt_force_deopt_fail_total(std::uint64_t /*n*/) {}
// Issue #2371 / #2603: weak stubs for cross-COW soft migrate counters.
// Light-link tests wire metrics via aura_set_aot_metrics; write through
// g_aot_metrics_stub so counter assertions observe real motion (mirrors
// full bridge in aura_jit_bridge.cpp).
extern "C" __attribute__((weak)) void aura_bump_cross_cow_soft_migrate_total(void) noexcept {
    if (auto* m = static_cast<aura::compiler::CompilerMetrics*>(aura_get_aot_metrics()))
        m->cross_cow_soft_migrate_total.fetch_add(1, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void
aura_bump_cross_cow_soft_migrate_same_gen_total(void) noexcept {
    if (auto* m = static_cast<aura::compiler::CompilerMetrics*>(aura_get_aot_metrics()))
        m->cross_cow_soft_migrate_same_gen_total.fetch_add(1, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void aura_bump_cross_cow_hard_reject_total(void) noexcept {
    if (auto* m = static_cast<aura::compiler::CompilerMetrics*>(aura_get_aot_metrics()))
        m->cross_cow_hard_reject_total.fetch_add(1, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void
aura_bump_cross_cow_hard_reject_reason(std::uint8_t reason) noexcept {
    auto* m = static_cast<aura::compiler::CompilerMetrics*>(aura_get_aot_metrics());
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
extern "C" __attribute__((weak)) std::uint8_t
aura_cross_cow_last_hard_reject_reason(void) noexcept {
    auto* m = static_cast<aura::compiler::CompilerMetrics*>(aura_get_aot_metrics());
    return m ? m->cross_cow_last_hard_reject_reason.load(std::memory_order_relaxed) : 0;
}
extern "C" __attribute__((weak)) int aura_cross_cow_soft_migrate_enabled(void) noexcept {
    if (const char* e = std::getenv("AURA_CROSS_COW_SOFT_MIGRATE"))
        return (e[0] != '0' && e[0] != '\0') ? 1 : 0;
    return 1;
}
extern "C" __attribute__((weak)) std::uint64_t
aura_cross_cow_soft_migrate_max_drift(void) noexcept {
    if (const char* e = std::getenv("AURA_CROSS_COW_SOFT_MIGRATE_MAX_DRIFT")) {
        char* end = nullptr;
        const auto v = std::strtoull(e, &end, 10);
        if (end != e)
            return static_cast<std::uint64_t>(v);
    }
    return 4096;
}
// Issue #2547: weak stubs for workspace COW gen (full impl in aura_jit_bridge.cpp).
extern "C" __attribute__((weak)) void
aura_set_live_workspace_cow_gen(std::uint64_t /*gen*/) noexcept {}
extern "C" __attribute__((weak)) std::uint64_t aura_get_live_workspace_cow_gen(void) noexcept {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t
aura_get_closure_cow_gen(std::int64_t /*closure_id*/) {
    return 0;
}
// Issue #2092: stable-id-keyed remap (no display-name arg). Stub
// returns 0 so light test binaries without the production TU observe
// no remap (consistent with no reemit candidates).
extern "C" __attribute__((weak)) void aura_closure_set_must_deopt(std::int64_t /*id*/, int /*v*/) {}
extern "C" __attribute__((weak)) int aura_closure_get_must_deopt(std::int64_t /*id*/) {
    return 0;
}
// Issue #3247: sticky observe stub (does not clear; production is read-only).
extern "C" __attribute__((weak)) int
aura_get_closure_must_deopt_before_next_call(std::int64_t /*id*/) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t
aura_remap_live_closures_after_reemit(const std::uint32_t* /*stable_ids*/, std::size_t /*n*/,
                                      std::uint64_t /*new_bridge_epoch*/) {
    return 0;
}
// Issue #2602: sync remount walk stub. Zero-cost in light test binaries —
// no live closures / no force-JIT demotion path, just zero out counters
// (matches production zero-extra-work contract on idle paths).
extern "C" __attribute__((weak)) void
aura_sync_remount_named_live_closures(std::uint64_t* ok_count, std::uint64_t* fail_count) {
    if (ok_count)
        *ok_count = 0;
    if (fail_count)
        *fail_count = 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_reemit_dirty_count(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_reemit_region_filtered_skips(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_reemit_closure_dep_count(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_reemit_success_count(void) {
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
extern "C" __attribute__((weak)) std::uintptr_t aura_aot_probe_fn_ptr(std::int64_t /*func_id*/) {
    return 0;
}
extern "C" __attribute__((weak)) std::uintptr_t
aura_aot_probe_fn_ptr_raw(std::int64_t /*func_id*/) {
    return 0;
}
extern "C" __attribute__((weak)) int aura_aot_slot_is_stale(std::int64_t /*func_id*/) {
    return 1;
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

// aura_set_aot_metrics lives in runtime_ssot.cpp (libaura_tl_arena.so).

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
// Issue #2199: hard_timeout / forced-abort stubs.
extern "C" void aura_set_hard_timeout_us(std::uint64_t us) {
    (void)us;
}
extern "C" std::uint64_t aura_get_hard_timeout_us(void) {
    return 0;
}
extern "C" std::uint64_t aura_get_long_mutation_forced_abort_total(void) {
    return 0;
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

// aura_ensure_aot_metrics / aura_get_aot_metrics / counters live
// in runtime_ssot.cpp (libaura_tl_arena.so).
// Issue #2093: weak stub for the last-reload-fail-reason getter
// (production impl is in aura_jit_bridge.cpp; light test binaries
// that don't link the production bridge TU link cleanly).
extern "C" __attribute__((weak)) std::uint8_t aura_aot_last_reload_fail_reason(void) {
    return 0; // AotReloadFail::Ok
}

// Issue #2165: auto-retry flag stubs (default off in light test binaries).
extern "C" __attribute__((weak)) void aura_set_aot_reload_auto_retry(int /*enabled*/) {}
extern "C" __attribute__((weak)) int aura_aot_reload_auto_retry_enabled(void) {
    return 0;
}

extern "C" void aura_jit_epoch_acquire_fence(void) {
    aura::util::thread_fence(std::memory_order_acquire);
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

extern "C" bool aura_aot_fn_version_is_stale_ex(void* dl_handle, const char* original_name,
                                                std::uint64_t expected_defuse,
                                                std::uint64_t expected_env,
                                                std::uint8_t expected_linear) {
    (void)dl_handle;
    (void)original_name;
    (void)expected_defuse;
    (void)expected_env;
    (void)expected_linear;
    return false;
}

extern "C" bool aura_aot_parse_version_suffix(const char* mangled, std::uint64_t* out_version) {
    if (!mangled || !out_version)
        return false;
    // Issue #2015: allow optional _eN_lN after _vN.
    const char* p = std::strstr(mangled, "_v");
    if (!p)
        return false;
    // Prefer last _v (defuse stamp).
    const char* last = p;
    while ((p = std::strstr(p + 2, "_v")) != nullptr)
        last = p;
    char* end = nullptr;
    unsigned long long v = std::strtoull(last + 2, &end, 10);
    if (!end || end == last + 2)
        return false;
    // Trailing may be '\0' or '_e...'
    if (*end != '\0' && !(*end == '_' && *(end + 1) == 'e'))
        return false;
    *out_version = static_cast<std::uint64_t>(v);
    return true;
}

extern "C" bool aura_aot_parse_full_version_suffix(const char* mangled, std::uint64_t* out_defuse,
                                                   std::uint64_t* out_env,
                                                   std::uint8_t* out_linear) {
    if (!mangled)
        return false;
    std::uint64_t defuse = 0;
    if (!aura_aot_parse_version_suffix(mangled, &defuse))
        return false;
    if (out_defuse)
        *out_defuse = defuse;
    std::uint64_t env = 0;
    std::uint8_t lin = 0;
    const char* pe = std::strstr(mangled, "_e");
    if (pe) {
        char* end = nullptr;
        env = static_cast<std::uint64_t>(std::strtoull(pe + 2, &end, 10));
        if (end && *end == '_' && *(end + 1) == 'l') {
            unsigned long long lv = std::strtoull(end + 2, &end, 10);
            if (end && *end == '\0')
                lin = static_cast<std::uint8_t>(lv > 255 ? 255 : lv);
            else
                env = 0;
        } else {
            env = 0;
        }
    }
    if (out_env)
        *out_env = env;
    if (out_linear)
        *out_linear = lin;
    return true;
}

extern "C" bool aura_aot_mangle_version_is_stale(const char* mangled, std::uint64_t expected) {
    std::uint64_t got = 0;
    if (!aura_aot_parse_version_suffix(mangled, &got))
        return true;
    return got != expected;
}

extern "C" bool aura_aot_mangle_version_is_stale_ex(const char* mangled,
                                                    std::uint64_t expected_defuse,
                                                    std::uint64_t expected_env,
                                                    std::uint8_t expected_linear) {
    std::uint64_t d = 0, e = 0;
    std::uint8_t l = 0;
    if (!aura_aot_parse_full_version_suffix(mangled, &d, &e, &l))
        return true;
    if (d != expected_defuse)
        return true;
    if (expected_env != 0 || expected_linear != 0 || e != 0 || l != 0)
        return e != expected_env || l != expected_linear;
    return false;
}

// ── Weak stubs pulled in by aura_test_objects (light / light_late) ──
// Production impls live in aura_jit_bridge.cpp + hot_update_registry.cpp.
// Light bundles link the bridge stub only; provide no-ops so the link
// succeeds when those object files are not in the executable.

// Issue #2304 / #2366 / #2501: epoch-invariant counters (full impl in
// aura_jit_bridge.cpp). Light issue tests that pull CompilerService via
// aura_test_objects need these symbols without linking full LLVM bridge.
static std::atomic<std::uint64_t> g_epoch_invariant_violation_total_stub{0};
static std::atomic<std::uint64_t> g_epoch_invariant_walks_total_stub{0};
static std::atomic<std::uint64_t> g_epoch_invariant_slot_stale_total_stub{0};
static std::atomic<std::uint64_t> g_epoch_invariant_closure_must_deopt_total_stub{0};
static std::atomic<std::uint64_t> g_epoch_invariant_sid_stale_total_stub{0}; // #3540

extern "C" __attribute__((weak)) std::uint64_t aura_epoch_invariant_violation_total_v_read(void) {
    return g_epoch_invariant_violation_total_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t aura_epoch_invariant_walks_total_v_read(void) {
    return g_epoch_invariant_walks_total_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t aura_epoch_invariant_slot_stale_total_v_read(void) {
    return g_epoch_invariant_slot_stale_total_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t
aura_epoch_invariant_closure_must_deopt_total_v_read(void) {
    return g_epoch_invariant_closure_must_deopt_total_stub.load(std::memory_order_relaxed);
}
// aura_set_epoch_invariant_mode / aura_epoch_invariant_mode live in
// runtime_ssot.cpp (libaura_tl_arena.so).
extern "C" __attribute__((weak)) void aura_set_epoch_invariant_hard_enabled(int enabled) {
    aura_set_epoch_invariant_mode(enabled != 0 ? 2 : 0);
}
extern "C" __attribute__((weak)) void
aura_epoch_invariant_note_walk(std::uint64_t violations) noexcept {
    g_epoch_invariant_walks_total_stub.fetch_add(1, std::memory_order_relaxed);
    if (violations > 0)
        g_epoch_invariant_violation_total_stub.fetch_add(violations, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void
aura_epoch_invariant_note_slot_stale(std::uint64_t n) noexcept {
    if (n > 0)
        g_epoch_invariant_slot_stale_total_stub.fetch_add(n, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void
aura_epoch_invariant_note_closure_must_deopt(std::uint64_t n) noexcept {
    if (n > 0)
        g_epoch_invariant_closure_must_deopt_total_stub.fetch_add(n, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t aura_epoch_invariant_sid_stale_total_v_read(void) {
    return g_epoch_invariant_sid_stale_total_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void
aura_epoch_invariant_note_sid_stale(std::uint64_t n) noexcept {
    if (n > 0)
        g_epoch_invariant_sid_stale_total_stub.fetch_add(n, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) int aura_epoch_invariant_sid_stale_issue(void) {
    return 3540;
}

// Issue #2640: production Restricted default periodic epoch-invariant soft walk
// (full impl in aura_jit_bridge.cpp; light stub = no-op + zero counters).
static std::atomic<std::uint64_t> g_epoch_invariant_periodic_walks_total_stub{0};
static std::atomic<std::uint64_t> g_epoch_invariant_periodic_last_walk_at_ms_stub{0};
static std::atomic<std::uint64_t> g_epoch_invariant_periodic_skipped_off_total_stub{0};
static std::atomic<std::uint64_t> g_epoch_invariant_periodic_skipped_wrong_mode_total_stub{0};
static std::atomic<std::uint64_t> g_epoch_invariant_periodic_skipped_rate_limited_total_stub{0};
static std::atomic<std::uint64_t> g_epoch_invariant_periodic_skipped_disabled_total_stub{0};
static std::atomic<std::uint64_t> g_epoch_invariant_periodic_period_ms_stub{5000};

extern "C" __attribute__((weak)) std::uint64_t
aura_epoch_invariant_periodic_walks_total_v_read(void) {
    return g_epoch_invariant_periodic_walks_total_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t
aura_epoch_invariant_periodic_last_walk_at_ms_v_read(void) {
    return g_epoch_invariant_periodic_last_walk_at_ms_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t
aura_epoch_invariant_periodic_skipped_off_total_v_read(void) {
    return g_epoch_invariant_periodic_skipped_off_total_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t
aura_epoch_invariant_periodic_skipped_wrong_mode_total_v_read(void) {
    return g_epoch_invariant_periodic_skipped_wrong_mode_total_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t
aura_epoch_invariant_periodic_skipped_rate_limited_total_v_read(void) {
    return g_epoch_invariant_periodic_skipped_rate_limited_total_stub.load(
        std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t
aura_epoch_invariant_periodic_skipped_disabled_total_v_read(void) {
    return g_epoch_invariant_periodic_skipped_disabled_total_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t
aura_epoch_invariant_periodic_period_ms_v_read(void) {
    return g_epoch_invariant_periodic_period_ms_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void
aura_set_epoch_invariant_periodic_period_ms(std::uint64_t ms) {
    g_epoch_invariant_periodic_period_ms_stub.store(ms, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void aura_periodic_epoch_invariant_walk_if_due(void) {}
extern "C" __attribute__((weak)) void aura_force_drain_old_so(void) {}
extern "C" __attribute__((weak)) std::uint64_t aura_reload_old_so_staged_total_v_read(void) {
    return 0;
}
extern "C" __attribute__((weak)) void aura_reset_epoch_invariant_periodic_for_test(void) {}

// Issue #2668: event-driven walk counters (distinct from periodic).
static std::atomic<std::uint64_t> g_epoch_invariant_event_walks_total_stub{0};
static std::atomic<std::uint64_t> g_epoch_invariant_event_skipped_off_total_stub{0};
static std::atomic<std::uint64_t> g_epoch_invariant_event_skipped_wrong_mode_total_stub{0};
extern "C" __attribute__((weak)) std::uint64_t aura_epoch_invariant_event_walks_total_v_read(void) {
    return g_epoch_invariant_event_walks_total_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t
aura_epoch_invariant_event_skipped_off_total_v_read(void) {
    return g_epoch_invariant_event_skipped_off_total_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t
aura_epoch_invariant_event_skipped_wrong_mode_total_v_read(void) {
    return g_epoch_invariant_event_skipped_wrong_mode_total_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void aura_event_driven_epoch_invariant_walk_if_due(void) {}
// Issue #2980: merged heal counter (light-link no-op).
extern "C" __attribute__((weak)) std::uint64_t aura_epoch_residual_merged_heal_total_v_read(void) {
    return 0;
}

// Issue #2693: Soft epoch-invariant consecutive-dirty fuse stubs
// (light bundles without the production bridge TU). Mirror the
// production counter atomics + accessor contract so tests that
// don't link aura_jit_bridge.cpp still compile and link.
static std::atomic<std::uint64_t> g_2693_soft_fuse_fallback_total_stub{0};
static std::atomic<std::uint64_t> g_2693_consecutive_dirty_total_stub{0};
static std::atomic<int> g_2693_soft_fuse_k_stub{3};
// Issue #2712: Soft fuse heal total (production in aura_jit_bridge.cpp).
extern "C" __attribute__((weak)) std::uint64_t
aura_epoch_invariant_soft_fuse_heal_total_v_read(void) {
    return 0;
}
// Issue #2747: Soft fuse heal no-owner residual (production in aura_jit_bridge.cpp).
extern "C" __attribute__((weak)) std::uint64_t
aura_epoch_invariant_soft_fuse_heal_no_owner_total_v_read(void) {
    return 0;
}
extern "C" __attribute__((weak)) std::uint64_t aura_epoch_invariant_soft_fuse_total_v_read(void) {
    return g_2693_soft_fuse_fallback_total_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t
aura_epoch_invariant_consecutive_dirty_total_v_read(void) {
    return g_2693_consecutive_dirty_total_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) int aura_epoch_invariant_soft_fuse_k_default(void) {
    return g_2693_soft_fuse_k_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void aura_set_epoch_invariant_soft_fuse_k(int k) {
    if (k < 0)
        k = 0;
    g_2693_soft_fuse_k_stub.store(k, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) int aura_get_epoch_invariant_soft_fuse_k(void) {
    return aura_epoch_invariant_soft_fuse_k_default();
}

// AOT slot C ABI lives in runtime_ssot.cpp (hook, default no-op).

// Issue #2050 / runtime soft-dirty path: weak counter bump.
extern "C" __attribute__((weak)) void
aura_bump_live_closure_must_deopt_kept_total(std::uint64_t /*n*/) {}

// Issue #2177: AOT macro marker propagation counters (full impl in bridge).
static std::atomic<std::uint64_t> g_2177_aot_macro_marker_propagated_stub{0};
static std::atomic<std::uint64_t> g_2177_aot_macro_marker_stripped_stub{0};
extern "C" __attribute__((weak)) std::uint64_t aura_2177_aot_macro_marker_propagated_total(void) {
    return g_2177_aot_macro_marker_propagated_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t aura_2177_aot_macro_marker_stripped_total(void) {
    return g_2177_aot_macro_marker_stripped_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void
aura_2177_record_aot_marker_propagated(int propagated) noexcept {
    if (propagated)
        g_2177_aot_macro_marker_propagated_stub.fetch_add(1, std::memory_order_relaxed);
    else
        g_2177_aot_macro_marker_stripped_stub.fetch_add(1, std::memory_order_relaxed);
}

// Issue #1996: clear batch-deopt target (symmetric to set stub above).
extern "C" __attribute__((weak)) void aura_clear_jit_batch_deopt_target(void* /*aura_jit_ptr*/) {}

// Issue #2366: no live AOT slots in light stubs.
extern "C" __attribute__((weak)) std::size_t
aura_aot_invalidate_all_stale_slots_for_eval(void* /*eval_ptr*/) {
    return 0;
}

// Issue #2178 / #2240: cross-workspace reject observability (bridge full impl).
static std::atomic<std::uint64_t> g_cross_workspace_rejected_stub{0};
static std::atomic<std::uint8_t> g_last_cross_workspace_reject_reason_stub{0};
extern "C" __attribute__((weak)) std::uint64_t
aura_cross_workspace_hot_update_rejected_total_v_read(void) noexcept {
    return g_cross_workspace_rejected_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint8_t
aura_last_cross_workspace_reject_reason_v_read(void) noexcept {
    return g_last_cross_workspace_reject_reason_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void
aura_test_set_last_cross_workspace_reject_reason(std::uint8_t v) noexcept {
    g_last_cross_workspace_reject_reason_stub.store(v, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void
aura_test_reset_last_cross_workspace_reject_reason(void) noexcept {
    g_last_cross_workspace_reject_reason_stub.store(0, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) const char*
aura_cross_workspace_reject_reason_string(std::uint8_t v) noexcept {
    switch (v) {
        case 0:
            return "none";
        case 1:
            return "foreign_eval";
        case 2:
            return "cow_gen_mismatch";
        default:
            return "unknown";
    }
}

// Issue #2162: deferred reemit hooks (production in hot_update_registry.cpp).
// Weak so linking the real registry wins; light-only binaries stay linkable.
extern "C" __attribute__((weak)) int aura_hot_update_has_deferred_reemit(void) {
    return 0;
}
extern "C" __attribute__((weak)) void
aura_hot_update_on_deferred_reemit_seen_on_steal(std::int64_t /*fiber_id*/) {}
extern "C" __attribute__((weak)) void aura_hot_update_notify_epoch_bump(std::uint64_t /*epoch*/) {}

extern "C" __attribute__((weak)) void
aura_hot_update_registry_get_snapshot(aura_hot_update_registry_snapshot* out) {
    if (!out)
        return;
    // Zero the POD snapshot so light-bundle metrics queries stay inert.
    std::memset(out, 0, sizeof(*out));
}

// Issue #2367: weak no-op ReloadRecovery snapshot for light bundles.
extern "C" __attribute__((weak)) void
aura_hot_update_reload_recovery_get_snapshot(aura_reload_recovery_snapshot* out) {
    if (!out)
        return;
    std::memset(out, 0, sizeof(*out));
    out->schema = 2367;
    out->issue = 2367;
    // reload_recovery_wired stays 0 in the weak stub so agents can
    // distinguish light-link from production registry linkage.
}

// Issue #2014: weak no-ops when hot_update_registry.cpp is not linked.
extern "C" __attribute__((weak)) void aura_hot_update_note_deopt(void) {}
extern "C" __attribute__((weak)) int aura_hot_update_should_throttle_reemit(void) {
    return 0;
}
extern "C" __attribute__((weak)) void aura_hot_update_on_reemit_throttled(void) {}
// StormLevel C ABI lives in runtime_ssot.cpp; hot_update_registry
// registers the real getter/setter when this DSO loads.
// Issue #2095: weak stubs for the default-LLVM reemit postmortem hook.
// Light test binaries that don't link the production bridge TU still
// link cleanly; the real impl reads AURA_REEMIT_KEEP_FAIL env and
// renames failed .o into /tmp/aura_reemit_failed/.
extern "C" __attribute__((weak)) int aura_reemit_keep_fail_enabled(void) {
    return 0;
}
extern "C" __attribute__((weak)) void aura_reemit_keep_failed_obj(const char* /*obj_path*/,
                                                                  const char* /*reason*/) {}
extern "C" __attribute__((weak)) void
aura_hot_update_set_deopt_storm_threshold(std::uint64_t /*d*/, std::uint64_t /*w*/) {}
extern "C" __attribute__((weak)) void aura_hot_update_reset_deopt_storm_state_for_test(void) {}
extern "C" __attribute__((weak)) void
aura_hot_update_clear_global_throttle_keep_hysteresis_for_test(void) {}
extern "C" __attribute__((weak)) int aura_hot_update_storm_exit_force_full_active(void) {
    return 0;
}
// Issue #2035: weak no-ops when hot_update_registry.cpp is not linked.
extern "C" __attribute__((weak)) void aura_hot_update_notify_dirty_define(const char* /*name*/) {}
// Issue #2601: weak no-op when full bridge not linked (light test bundles).
extern "C" __attribute__((weak)) void aura_hot_update_maybe_retry_exhausted_min_dirty(void) {}
extern "C" __attribute__((weak)) int aura_hot_update_reemit_provider_wired(void) {
    return 0;
}

extern "C" __attribute__((weak)) void aura_1637_note_steal_restore_fallback(void) {}
extern "C" __attribute__((weak)) void aura_1637_note_compact_restore_fallback(void) {}
extern "C" __attribute__((weak)) void aura_1637_note_hot_swap_restore_fallback(void) {}

// Issue #2810 / #3260: light-link dual-write path. Strong aura_jit_bridge.cpp
// overrides this when the full bridge is linked. When only the light stub
// is linked, still dual-write per-CompilerMetrics via the fiber-mutation
// trampoline (strong when aura_test_objects is linked; weak no-op otherwise).
// Issue #3260 Bug 3: process-wide atomics so stub-linked totals are not a
// hard-zero indistinguishable from "no clones" (full bridge overrides).
static std::atomic<std::uint64_t> g_1908_repin_stub_total{0};
static std::atomic<std::uint64_t> g_1908_hygiene_stub_total{0};
extern "C" int aura_evaluator_bump_macro_provenance_repin_on_steal(void* ev_ptr) noexcept;
extern "C" __attribute__((weak)) void
aura_bump_macro_provenance_repin_on_steal_total(std::uint64_t n) {
    if (n != 0)
        g_1908_repin_stub_total.fetch_add(n, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void
aura_bump_hygiene_violation_prevented_on_boundary_total(std::uint64_t n) {
    if (n != 0)
        g_1908_hygiene_stub_total.fetch_add(n, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) int
aura_macro_provenance_repin_on_steal(void* ev_ptr, std::uint64_t /*cloned_marker*/,
                                     int was_violation) {
    aura_bump_macro_provenance_repin_on_steal_total(1);
    if (was_violation)
        aura_bump_hygiene_violation_prevented_on_boundary_total(1);
    // Return 2 when per-eval bumped (matches full-bridge contract), else 1
    // (file-level stub atomics always bump — Issue #3260 Bug 3).
    return aura_evaluator_bump_macro_provenance_repin_on_steal(ev_ptr) ? 2 : 1;
}
extern "C" __attribute__((weak)) std::uint64_t aura_macro_provenance_repin_on_steal_total(void) {
    return g_1908_repin_stub_total.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t
aura_hygiene_violation_prevented_on_boundary_total(void) {
    return g_1908_hygiene_stub_total.load(std::memory_order_relaxed);
}
// Issue #2810: weak no-ops when fiber-mutation TU not linked.
// Strong definitions in evaluator_fiber_mutation.cpp override these.
extern "C" __attribute__((weak)) void* aura_evaluator_resolve_current_for_macro(void) noexcept {
    return nullptr;
}
extern "C" __attribute__((weak)) void aura_evaluator_note_steal_abort_mid_expand(void) noexcept {}
extern "C" __attribute__((weak)) std::uint64_t
aura_evaluator_capability_tenant_id(void* /*ev*/) noexcept {
    return 0;
}
extern "C" __attribute__((weak)) int
aura_evaluator_bump_macro_provenance_repin_on_steal(void* /*ev_ptr*/) noexcept {
    return 0;
}

// Light-link remount metric bumps write through g_aot_metrics_stub when the
// host calls aura_set_aot_metrics (same contract as full bridge). No-op when
// metrics is null — zero cost hot path when tests don't wire metrics.
static inline aura::compiler::CompilerMetrics* aot_metrics_stub_() noexcept {
    return static_cast<aura::compiler::CompilerMetrics*>(aura_get_aot_metrics());
}
extern "C" __attribute__((weak)) void
aura_bump_live_closure_epoch_restamp_total(std::uint64_t /*n*/) {}
extern "C" __attribute__((weak)) void aura_bump_closure_capture_remount_ok_total(std::uint64_t n) {
    if (auto* m = aot_metrics_stub_())
        m->closure_capture_remount_ok_total.fetch_add(n, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void
aura_bump_closure_capture_cell_remap_ok_total(std::uint64_t n) {
    if (auto* m = aot_metrics_stub_())
        m->closure_capture_cell_remap_ok_total.fetch_add(n, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void
aura_bump_closure_capture_cell_remap_fail_total(std::uint64_t n) {
    if (auto* m = aot_metrics_stub_())
        m->closure_capture_cell_remap_fail_total.fetch_add(n, std::memory_order_relaxed);
}
// Issue #2503: remount + MustDeopt + batch_deopt shared fail path (stub: fail).
// Strong definition lives in aura_jit_runtime.cpp (light + full).
extern "C" __attribute__((weak)) int aura_remount_or_force_deopt(std::int64_t /*closure_id*/,
                                                                 std::uint64_t /*live_env_gen*/,
                                                                 std::uint8_t /*linear_fp*/) {
    return 0;
}
// Issue #2894: last remount fail reason (stub process atomic).
// Strong definitions in aura_jit_bridge.cpp override when full bridge links.
static std::atomic<std::uint8_t> g_last_remount_fail_reason_stub{0};
extern "C" __attribute__((weak)) std::uint8_t aura_last_remount_fail_reason(void) noexcept {
    return g_last_remount_fail_reason_stub.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) const char*
aura_remount_fail_reason_string(std::uint8_t v) noexcept {
    switch (v) {
        case 0:
            return "ok";
        case 1:
            return "env_gen";
        case 2:
            return "defuse";
        case 3:
            return "linear";
        case 4:
            return "densify_cell";
        case 5:
            return "other";
        default:
            return "other";
    }
}
extern "C" __attribute__((weak)) void aura_note_remount_fail_reason(std::uint8_t v) noexcept {
    g_last_remount_fail_reason_stub.store(v, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void aura_test_reset_last_remount_fail_reason(void) noexcept {
    g_last_remount_fail_reason_stub.store(0, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void aura_set_densify_object_remap(const void* const* /*olds*/,
                                                                    const void* const* /*news*/,
                                                                    std::size_t /*n*/) {}
extern "C" __attribute__((weak)) void aura_clear_densify_object_remap(void) {}
extern "C" __attribute__((weak)) void aura_set_densify_candidates(const void* const* /*cands*/,
                                                                  std::size_t /*n*/) {}
extern "C" __attribute__((weak)) void aura_clear_densify_candidates(void) {}
extern "C" __attribute__((weak)) void
aura_bump_closure_capture_remount_fail_total(std::uint64_t n) {
    if (auto* m = aot_metrics_stub_())
        m->closure_capture_remount_fail_total.fetch_add(n, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void
aura_bump_closure_capture_env_gen_mismatch_total(std::uint64_t n) {
    if (auto* m = aot_metrics_stub_())
        m->closure_capture_env_gen_mismatch_total.fetch_add(n, std::memory_order_relaxed);
}
