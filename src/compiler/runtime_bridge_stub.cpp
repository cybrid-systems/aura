// runtime_bridge_stub.cpp — weak bridge-hook stubs compiled into
// libaura_test_objects.so itself.
//
// Same class of bug as g_current_fiber_id_fn (fixed in runtime_ssot.cpp,
// 1ca6c8bc): libaura_test_objects.so's TUs (service.ixx, evaluator_ctor.cpp,
// scheduler.cpp, macro_expansion.cpp, ...) reference JIT-lib symbols
// (AuraJIT ctor, aura_cleanup_aot_state, aura_set_long_mutation_scheduler_hook,
// aura_macro_provenance_repin_on_steal). The strong definitions live in the
// JIT SOs (aura_jit.cpp / aura_jit_bridge.cpp) which libaura_test_objects.so
// does NOT NEEDED-link. On x86_64 CI, mold --as-needed strips the JIT lib
// from test_issue_* binaries that don't directly reference it, so the loader
// fails at load time:
//   symbol lookup error: libaura_test_objects.so: undefined symbol: _ZN4aura3jit7AuraJITC1Ev
// (rc=127; local arm64 lld keeps DT_NEEDED so it passes locally).
//
// These weak stubs live in the same .so that references the symbols, so the
// loader always resolves them. When a real JIT lib IS linked (full / light
// JIT tests), its strong definitions interpose the weak stubs and real
// behavior is preserved. Mirrors the existing weak stubs in
// aura_jit_bridge_stub.cpp (which is compiled into the JIT libs, not here).

#include "aura_jit_bridge.h"
#include "aura_jit.h"

#include <atomic>
#include <cstdint>
#include <vector>

extern "C" __attribute__((weak)) void aura_cleanup_aot_state(void* /*eval*/) {}

extern "C" __attribute__((weak)) void
aura_set_long_mutation_scheduler_hook(aura_long_mutation_scheduler_hook_fn /*fn*/) {}

// Issue #3260: process-wide stub atomics so fiber_mutation dual-write
// resolves when the full JIT bridge is not DT_NEEDED. Strong defs in
// aura_jit_bridge.cpp interpose these.
static std::atomic<std::uint64_t> g_1908_repin_runtime_stub_total{0};
static std::atomic<std::uint64_t> g_1908_hygiene_runtime_stub_total{0};
extern "C" __attribute__((weak)) void
aura_bump_macro_provenance_repin_on_steal_total(std::uint64_t n) {
    if (n != 0)
        g_1908_repin_runtime_stub_total.fetch_add(n, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) void
aura_bump_hygiene_violation_prevented_on_boundary_total(std::uint64_t n) {
    if (n != 0)
        g_1908_hygiene_runtime_stub_total.fetch_add(n, std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t aura_macro_provenance_repin_on_steal_total(void) {
    return g_1908_repin_runtime_stub_total.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::uint64_t
aura_hygiene_violation_prevented_on_boundary_total(void) {
    return g_1908_hygiene_runtime_stub_total.load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) int
aura_macro_provenance_repin_on_steal(void* /*ev_ptr*/, std::uint64_t /*cloned_marker*/,
                                     int was_violation) {
    aura_bump_macro_provenance_repin_on_steal_total(1);
    if (was_violation)
        aura_bump_hygiene_violation_prevented_on_boundary_total(1);
    return 1;
}

// Issue #1368: aura_set_aot_metrics lives in runtime_ssot.cpp
// (libaura_tl_arena.so) so this DSO resolves it at load time. A weak
// stub here would preempt the SSOT pointer and zero-out counter
// assertions (same class as aura_aot_bump_func_table_epoch).

// Issue #1522: batch-deopt target registration. service.ixx registers the
// AuraJIT* at boot; strong def lives in aura_jit_bridge.cpp. Weak stub so
// the loader resolves it when the JIT libs are stripped (mold --as-needed).
extern "C" __attribute__((weak)) void aura_set_jit_batch_deopt_target(void* /*jit*/) {}
extern "C" __attribute__((weak)) void aura_clear_jit_batch_deopt_target(void* /*jit*/) {}

// fn-pointer hooks service.ixx / evaluator.ixx wire during boot. Strong defs
// live in aura_jit_bridge.cpp / runtime_shared.h providers; weak no-ops keep
// libaura_test_objects.so loadable standalone (CI mold --as-needed).
extern "C" __attribute__((weak)) void
aura_set_jit_unhandled_invalidate_fn(aura_jit_unhandled_invalidate_fn_t /*fn*/) {}

extern "C" __attribute__((weak)) void
aura_jit_set_macro_deopt_restore_fn(std::uint64_t (* /*fn*/)() noexcept) {}

extern "C" __attribute__((weak)) void
aura_set_linear_post_mutate_enforce_fn(aura_linear_post_mutate_enforce_fn_t /*fn*/,
                                       void* /*user_data*/) {}

extern "C" __attribute__((weak)) void
aura_set_linear_live_closure_scan_fn(aura_linear_live_closure_scan_fn_t /*fn*/,
                                     void* /*user_data*/) {}

extern "C" __attribute__((weak)) void aura_set_linear_ownership_epoch(std::uint64_t /*v*/) {}

extern "C" __attribute__((weak)) void
aura_set_lock_hooks(void (* /*lock_read*/)(void*), void (* /*unlock_read*/)(void*),
                    void (* /*lock_write*/)(void*), void (* /*unlock_write*/)(void*),
                    std::uint64_t (* /*get_version*/)(void*), void (* /*yield_boundary*/)(void*),
                    void* /*user_data*/) {}

// aura_clear_evaluator_runtime_hooks lives in runtime_ssot.cpp
// (libaura_tl_arena.so). A weak stub here would preempt the SSOT
// callback (same class as aura_set_aot_metrics).

extern "C" __attribute__((weak)) void
aura_set_top_cell_getter(std::int64_t (* /*fn*/)(void*, std::int64_t), void* /*user_data*/) {}

// aura_get_aot_metrics / aura_set_aot_metrics live in runtime_ssot.cpp.
// aura_aot_func_table_epoch / aura_aot_bump_func_table_epoch /
// aura_set/get_aot_live_env_frame_version /
// aura_set/get_aot_live_linear_state_fingerprint live in runtime_ssot.cpp
// (same class: a weak stub here would preempt the SSOT atomic).

// Issue #2544 note: do NOT stub aura_set_aot_defuse_version here. Same class
// as aura_set_aot_metrics (comment above): an empty weak stub in
// libaura_test_objects.so preempts the strong definition in the JIT lib, so
// tests that set host defuse (e.g. exhausted-min-dirty reload paths) write
// into a no-op stub and the reload defuse check observes zero drift. Light
// JIT tests resolve this from libaura_jit_light_test_objects.so (weak stub
// in aura_jit_bridge_stub.cpp); full JIT tests get the strong definition
// from aura_jit_bridge.cpp.

extern "C" __attribute__((weak)) void aura_set_current_bridge_epoch(std::uint64_t /*v*/) {}

// aura_set_epoch_invariant_mode / aura_epoch_invariant_mode live in
// runtime_ssot.cpp (libaura_tl_arena.so).

// epoch-invariant note_* / must_deopt / v_read live in runtime_ssot.cpp.
//
// Do NOT stub aura_aot_count_live_generation_behind_slots here. A weak
// 0-return in libaura_test_objects.so is found before the strong
// runtime_ssot.cpp body in libaura_tl_arena.so (DT_NEEDED order) and
// every epoch-invariant inject AC sees behind=0. The SSOT dispatches
// through the registered JIT hook (or returns 0 when unregistered).

// Do NOT stub aura_reemit_aot_for_dirty or the owner-eval TLS accessors
// here. Same class as aura_set_aot_defuse_version above: a weak definition
// in libaura_test_objects.so is found first in ELF search order and
// preempts the strong body in libaura_jit_test_objects.so. Full-JIT tests
// then see set_reemit_candidate_fn (JIT) + reemit-returns-0 (stub) —
// split-brain, every incremental-reemit AC fails. Light/full JIT SOs
// already provide the symbol (stub vs real). Non-JIT binaries leave it
// undefined; mold --as-needed + lazy bind only fault if a path calls it.

// aura_aot_invalidate_all_stale_slots_for_eval lives in runtime_ssot.cpp.

extern "C" __attribute__((weak)) void aura_invalidate_all_closure_caches(void) {}

extern "C" __attribute__((weak)) void aura_jit_macro_introduced_lost_inc(std::uint64_t /*n*/) {}
extern "C" __attribute__((weak)) void aura_jit_macro_introduced_preserved_inc(std::uint64_t /*n*/) {
}
extern "C" __attribute__((weak)) void
aura_multi_eval_macro_marker_preserved_inc(std::uint64_t /*n*/) {}

extern "C" __attribute__((weak)) std::int64_t aura_alloc_string(const char* /*s*/) {
    return 0;
}
extern "C" __attribute__((weak)) const char* aura_jit_pool_string(std::size_t /*idx*/) {
    return nullptr;
}
extern "C" __attribute__((weak)) const char* aura_jit_string_content(std::int64_t /*val*/) {
    return nullptr;
}

// Weak AuraJIT stub ctor (+ companion symbols) so service.ixx's jit_ value
// member links when no JIT lib is present. Strong definitions in
// aura_jit.cpp interpose when a JIT lib is linked.
namespace aura::jit {

struct AuraJIT::Impl {};

__attribute__((weak)) AuraJIT::AuraJIT()
    : impl_(nullptr) {}
__attribute__((weak)) AuraJIT::~AuraJIT() = default;
__attribute__((weak)) bool AuraJIT::available() const {
    return false;
}

// service.ixx lowers via run_escape_analysis. Strong body lives in
// aura_jit.cpp (JIT SOs). Conservative all-ESCAPED when mold
// --as-needed strips the JIT lib (CI x86_64). Full/light JIT
// prepended on those test binaries interposes the real analysis.
__attribute__((weak)) void
run_escape_analysis(const std::vector<std::vector<FlatInstruction>>& /*flat_instrs*/,
                    std::uint32_t local_count, std::vector<std::uint8_t>& escape_map) {
    escape_map.assign(local_count, 1);
}

} // namespace aura::jit
