// runtime_ssot.cpp — SSOT definitions for shared runtime globals used by
// both libaura_test_objects.so (evaluator / IR) and the light/full JIT SOs.
//
// Same class of bug as g_tl_arena (see tl_arena_tls.cpp): when the
// definition lives only in aura_jit_runtime.cpp (JIT SOs), aura_test_objects
// has an undefined data symbol that x86_64 glibc refuses to resolve from a
// later-loaded DSO at load time:
//   symbol lookup error: libaura_test_objects.so: undefined symbol: g_pair_slots
// (aarch64 often still resolved via the executable NEEDED list).
//
// Declarations live in runtime_shared.h. This TU is compiled into
// libaura_tl_arena.so (runtime SSOT shared object; name kept for DT_NEEDED
// stability) so aura_test_objects NEEDED-links the provider first.

#include "runtime_shared.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <vector>

// JIT runtime registers the real lock/top-cell clearer. Null when JIT
// SOs are not loaded — ~CompilerService still resolves this symbol from
// libaura_tl_arena.so (never a weak stub in test_objects).
static std::atomic<void (*)(void*)> g_eval_runtime_hook_clearer{nullptr};

extern "C" void aura_register_evaluator_runtime_hook_clearer(void (*fn)(void*)) {
    g_eval_runtime_hook_clearer.store(fn, std::memory_order_release);
}

extern "C" void aura_clear_evaluator_runtime_hooks(void* user) {
    if (auto fn = g_eval_runtime_hook_clearer.load(std::memory_order_acquire))
        fn(user);
}

// ── Pair storage (JIT runtime + evaluator car/cdr fallback) ──
std::vector<PairSlot*> g_pair_slots;

// Heap-owned pair slots (subset of g_pair_slots allocated via malloc).
// Process-exit cleanup frees them; arena-owned slots are not listed here.
std::vector<PairSlot*> g_owned_pair_slots_;

// Arena allocation flag (CLI --no-arena / runtime bridges).
bool g_use_arena = true;

// Flat hash table index space (Phase 4c).
std::vector<FlatHashTable*> g_hash_tables;

// ── FlatHashTable::create / destroy ──
// Strong defs used to live only in aura_jit_runtime.cpp (JIT SOs).
// evaluator_primitives_* in libaura_test_objects.so call create() for
// every query: hash; CI x86_64 mold --as-needed then strips the JIT
// lib and load fails:
//   undefined symbol: _ZN13FlatHashTable6createEm
// SSOT here (libaura_tl_arena.so) so the evaluator SO always resolves
// them. rebuild() stays in aura_jit_runtime.cpp and calls create().
static constexpr std::uint8_t kHashEmptySlot = 0xFF; // hash_meta.h kEmptySlot

FlatHashTable* FlatHashTable::create(uint64_t cap) {
    auto* ht = static_cast<FlatHashTable*>(std::malloc(total_bytes(cap)));
    if (!ht)
        return nullptr;
    ht->capacity = cap;
    ht->size = 0;
    auto* meta = ht->metadata();
    for (uint64_t i = 0; i < cap; ++i)
        meta[i] = kHashEmptySlot;
    auto* k = ht->keys();
    for (uint64_t i = 0; i < cap; ++i)
        k[i] = 0;
    auto* v = ht->values();
    for (uint64_t i = 0; i < cap; ++i)
        v[i] = 0;
    return ht;
}

void FlatHashTable::destroy(FlatHashTable* ht) {
    if (ht)
        std::free(ht);
}

// ── Live AOT env / linear / table-epoch C ABI ──
// Same load-order class as aura_set_aot_metrics. Strong defs used to
// live only in aura_jit_bridge.cpp / the light stub; libaura_test_objects.so
// then failed (CI mold --as-needed):
//   undefined symbol: aura_aot_func_table_epoch
//   undefined symbol: aura_set_aot_live_env_frame_version
//   undefined symbol: aura_get_aot_live_linear_state_fingerprint
// SSOT here so the evaluator / HotUpdateRegistry TUs always resolve
// them. Full JIT (aura_jit_bridge.cpp) fetch_adds the same
// g_aot_table_epoch object; do NOT weak-stub these in
// runtime_bridge_stub.cpp (would preempt this pointer).
std::atomic<std::uint64_t> g_aot_table_epoch{1};
// Issue #3267: one packed word is the live-bridge SSOT.
// Layout: [env_frame_version : 56][linear_fingerprint : 8].
// env_generation_ is a compact/truncate counter; 56 bits is ample.
// Existing aura_set/get_aot_live_* wrappers CAS/load this word so
// a reader of the combined getter cannot observe a torn pair.
static std::atomic<std::uint64_t> g_aot_live_bridge_state{0};
static constexpr std::uint64_t kLiveBridgeLinMask = 0xffu;
static constexpr unsigned kLiveBridgeVerShift = 8;

static std::uint64_t pack_live_bridge_state(std::uint64_t version, std::uint8_t lin) noexcept {
    return (version << kLiveBridgeVerShift) | static_cast<std::uint64_t>(lin);
}

extern "C" std::uint64_t aura_aot_func_table_epoch(void) {
    return g_aot_table_epoch.load(std::memory_order_acquire);
}

// Fallback bump when no JIT SO is loaded. Full JIT's strong
// aura_aot_bump_func_table_epoch (slot invalidate / notify) interposes
// via ELF prepend. Light tests use this fetch_add on the same atomic
// the getter reads. Weak: test_ir / asan-build compile runtime_ssot.cpp
// AND aura_jit_bridge.cpp as objects in one executable — two strong
// defs were `duplicate symbol: aura_aot_bump_func_table_epoch`.
extern "C" __attribute__((weak)) void aura_aot_bump_func_table_epoch(void) {
    g_aot_table_epoch.fetch_add(1, std::memory_order_acq_rel);
}

extern "C" void aura_set_aot_live_bridge_state(std::uint64_t version, std::uint8_t max_lin) {
    g_aot_live_bridge_state.store(pack_live_bridge_state(version, max_lin),
                                  std::memory_order_release);
}

extern "C" void aura_get_aot_live_bridge_state(std::uint64_t* out_version, std::uint8_t* out_lin) {
    const auto packed = g_aot_live_bridge_state.load(std::memory_order_acquire);
    if (out_version)
        *out_version = packed >> kLiveBridgeVerShift;
    if (out_lin)
        *out_lin = static_cast<std::uint8_t>(packed & kLiveBridgeLinMask);
}

extern "C" void aura_set_aot_live_env_frame_version(std::uint64_t v) {
    auto old = g_aot_live_bridge_state.load(std::memory_order_relaxed);
    while (!g_aot_live_bridge_state.compare_exchange_weak(
        old, pack_live_bridge_state(v, static_cast<std::uint8_t>(old & kLiveBridgeLinMask)),
        std::memory_order_release, std::memory_order_relaxed)) {
    }
}

extern "C" std::uint64_t aura_get_aot_live_env_frame_version(void) {
    return g_aot_live_bridge_state.load(std::memory_order_acquire) >> kLiveBridgeVerShift;
}

extern "C" void aura_set_aot_live_linear_state_fingerprint(std::uint8_t v) {
    auto old = g_aot_live_bridge_state.load(std::memory_order_relaxed);
    while (!g_aot_live_bridge_state.compare_exchange_weak(
        old, pack_live_bridge_state(old >> kLiveBridgeVerShift, v), std::memory_order_release,
        std::memory_order_relaxed)) {
    }
}

extern "C" std::uint8_t aura_get_aot_live_linear_state_fingerprint(void) {
    return static_cast<std::uint8_t>(g_aot_live_bridge_state.load(std::memory_order_acquire) &
                                     kLiveBridgeLinMask);
}

// ── Current-fiber-id hook (Issue #195) ──
// SSOT lives here (libaura_tl_arena.so) so libaura_test_objects.so
// (which NEEDED-links libaura_tl_arena.so) resolves
// aura_set_current_fiber_id_fn at load time without depending on the
// JIT SOs. Same class of bug as g_pair_slots: when the definition
// lives only in aura_jit_runtime.cpp (JIT SOs), aura_test_objects has
// an undefined symbol that x86_64 glibc refuses to resolve from a
// later-loaded DSO at load time:
//   symbol lookup error: libaura_test_objects.so: undefined symbol: aura_set_current_fiber_id_fn
// (mold --as-needed strips the JIT lib from test_issue_* binaries
// that don't directly reference it).
static aura_fiber_id_fn_t g_current_fiber_id_fn = nullptr;

extern "C" void aura_set_current_fiber_id_fn(aura_fiber_id_fn_t fn) {
    g_current_fiber_id_fn = fn;
}
extern "C" aura_fiber_id_fn_t aura_get_current_fiber_id_fn() {
    return g_current_fiber_id_fn;
}

// ── AOT metrics pointer (Issue #1368) ──
// Same load-order class as aura_set_current_fiber_id_fn. Strong
// definition used to live only in aura_jit_bridge.cpp / the light
// stub; libaura_test_objects.so then failed:
//   undefined symbol: aura_set_aot_metrics
static aura::compiler::CompilerMetrics* g_aot_metrics = nullptr;
static std::atomic<std::uint64_t> g_aot_metrics_lazy_init_total{0};
static std::atomic<std::uint64_t> g_aot_metrics_explicit_sets{0};

extern "C" void aura_set_aot_metrics(aura::compiler::CompilerMetrics* m) {
    g_aot_metrics = m;
    if (m)
        g_aot_metrics_explicit_sets.fetch_add(1, std::memory_order_relaxed);
}

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

// Issue #3177 follow-up (ASAN stack-use-after-scope): clear the
// process-wide g_aot_metrics pointer iff it currently points to
// `metrics`. Called from Evaluator's destructor before
// aura_cleanup_aot_state — CompilerService declares `evaluator_`
// before `metrics_` (service.ixx ~11503 / ~13793), so C++ destroys
// `metrics_` FIRST (reverse declaration order). By the time
// ~Evaluator() runs, the metrics object is already dead.
// aura_cleanup_aot_state → aot_invalidate_all_stale_slots_for_eval_impl
// calls aot_metrics() (reads g_aot_metrics) and bumps
// aot_reload_fall_back_slot_invalidate_total via fetch_add on dead
// stack memory — ASan catches this as stack-use-after-scope at
// aura_jit_bridge.cpp:2124. Clearing g_aot_metrics before
// aura_cleanup_aot_state makes the metric bumps short-circuit on
// nullptr. The pointer comparison avoids clobbering a still-live
// pointer (e.g. when nested CompilerService shares the global).
extern "C" void aura_clear_aot_metrics_for_eval(void* metrics) {
    if (!metrics)
        return;
    if (g_aot_metrics == static_cast<aura::compiler::CompilerMetrics*>(metrics))
        g_aot_metrics = nullptr;
}

extern "C" std::uint64_t aura_aot_metrics_lazy_init_total(void) {
    return g_aot_metrics_lazy_init_total.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_aot_metrics_explicit_sets_total(void) {
    return g_aot_metrics_explicit_sets.load(std::memory_order_relaxed);
}

// ── StormLevel C ABI (Issue #2094) ──
// Fallback bitmask when no JIT registry has registered yet (Shape=bit0).
static std::uint8_t (*g_storm_get)(void) = nullptr;
static void (*g_storm_set_shape)(int) = nullptr;
static std::atomic<std::uint8_t> g_storm_fallback{0};

extern "C" void aura_register_storm_c_abi(std::uint8_t (*get)(void), void (*set_shape)(int)) {
    g_storm_get = get;
    g_storm_set_shape = set_shape;
}

extern "C" std::uint8_t aura_hot_update_current_storm_level(void) {
    if (g_storm_get)
        return g_storm_get();
    return g_storm_fallback.load(std::memory_order_relaxed);
}

extern "C" void aura_hot_update_set_shape_storm_active(int active) {
    if (g_storm_set_shape) {
        g_storm_set_shape(active);
        return;
    }
    if (active)
        g_storm_fallback.fetch_or(1, std::memory_order_relaxed);
    else
        g_storm_fallback.fetch_and(static_cast<std::uint8_t>(~1u), std::memory_order_relaxed);
}

// ── AOT slot / per-eval map C ABI ──
// Full JIT registers the real slot table. Light stub leaves these
// null so count/inject stay no-op unless the full bridge is loaded.
static std::size_t (*g_aot_count_behind)(void) = nullptr;
static void (*g_aot_inject)(std::int64_t) = nullptr;
static void (*g_aot_clear_slot)(std::int64_t) = nullptr;
static std::size_t (*g_aot_invalidate)(void*) = nullptr;
static std::uint64_t (*g_aot_map_size)(void) = nullptr;

extern "C" void aura_register_aot_slot_c_abi(std::size_t (*count_behind)(void),
                                             void (*inject)(std::int64_t),
                                             void (*clear_slot)(std::int64_t),
                                             std::size_t (*invalidate)(void*),
                                             std::uint64_t (*map_size)(void)) {
    g_aot_count_behind = count_behind;
    g_aot_inject = inject;
    g_aot_clear_slot = clear_slot;
    g_aot_invalidate = invalidate;
    g_aot_map_size = map_size;
}

extern "C" std::size_t aura_aot_count_live_generation_behind_slots(void) {
    return g_aot_count_behind ? g_aot_count_behind() : 0;
}

extern "C" void aura_aot_inject_live_stale_slot_for_test(std::int64_t func_id) {
    if (g_aot_inject)
        g_aot_inject(func_id);
}

extern "C" void aura_aot_clear_slot_for_test(std::int64_t func_id) {
    if (g_aot_clear_slot)
        g_aot_clear_slot(func_id);
}

extern "C" std::size_t aura_aot_invalidate_all_stale_slots_for_eval(void* eval_ptr) {
    return g_aot_invalidate ? g_aot_invalidate(eval_ptr) : 0;
}

extern "C" std::uint64_t aura_aot_state_map_size(void) {
    return g_aot_map_size ? g_aot_map_size() : 0;
}

// ── Epoch-invariant mode (Issue #2304 / #2501) ──
// Weak stubs in libaura_test_objects.so used to no-op set/get, so
// test_epoch_invariant_* never left mode=0. SSOT here so the setter
// is always live; the full JIT walk reads this same value.
static std::atomic<int> g_epoch_invariant_mode{0};

extern "C" void aura_set_epoch_invariant_mode(int mode) {
    if (mode < 0)
        mode = 0;
    if (mode > 2)
        mode = 2;
    g_epoch_invariant_mode.store(mode, std::memory_order_relaxed);
}

extern "C" int aura_epoch_invariant_mode(void) {
    return g_epoch_invariant_mode.load(std::memory_order_relaxed);
}

static std::atomic<std::uint64_t> g_epoch_invariant_violation_total{0};
static std::atomic<std::uint64_t> g_epoch_invariant_walks_total{0};
static std::atomic<std::uint64_t> g_epoch_invariant_slot_stale_total{0};
static std::atomic<std::uint64_t> g_epoch_invariant_closure_must_deopt_total{0};
// Issue #3540: sid-stale marks from the existing epoch-invariant walk
// (append END per #2906). Distinct from closure-must-deopt (epoch side).
inline constexpr int kEpochInvariantSidStaleIssue = 3540;
static std::atomic<std::uint64_t> g_epoch_invariant_sid_stale_total{0};

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

extern "C" void aura_epoch_invariant_note_sid_stale(std::uint64_t n) noexcept {
    if (n > 0)
        g_epoch_invariant_sid_stale_total.fetch_add(n, std::memory_order_relaxed);
}

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

extern "C" std::uint64_t aura_epoch_invariant_sid_stale_total_v_read(void) {
    return g_epoch_invariant_sid_stale_total.load(std::memory_order_relaxed);
}

extern "C" int aura_epoch_invariant_sid_stale_issue(void) {
    return kEpochInvariantSidStaleIssue;
}

static std::size_t (*g_must_deopt_stale)(void) = nullptr;

extern "C" void aura_register_epoch_must_deopt_fn(std::size_t (*fn)(void)) {
    g_must_deopt_stale = fn;
}

extern "C" std::size_t aura_epoch_invariant_must_deopt_stale_live_closures(void) {
    return g_must_deopt_stale ? g_must_deopt_stale() : 0;
}

// Issue #1998 / B-024: C accessor so tests need not name std::vector.
extern "C" size_t aura_g_owned_pair_slots_size() {
    return g_owned_pair_slots_.size();
}

namespace {
struct PairSlotCleanup {
    ~PairSlotCleanup() {
        for (auto* p : g_owned_pair_slots_) {
            std::free(p);
        }
        g_owned_pair_slots_.clear();
    }
};
[[maybe_unused]] PairSlotCleanup g_pair_slot_cleanup;
} // namespace
