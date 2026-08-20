// runtime_shared.h — Shared types for Aura runtime (JIT + evaluator + native)
// Used by aura_jit_runtime.cpp, evaluator partition TUs, runtime.c
#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <vector>

// ── PairSlot: unified pair storage format ──
// Replaces separate g_pair_cars/g_pair_cdrs or evaluator's Pair struct.
// Phase 1: stores in vector<PairSlot>.
// Future: stores PairSlot* for arena/heap selection.
struct PairSlot {
    int64_t car;
    int64_t cdr;
};

// ── TL Arena (thread-local bump allocator) ──
// Issue #1359: default was 64MB *eager* malloc per thread → 100 fibers = 6.4GB.
// Initial capacity is 1MB (or AURA_TL_ARENA_INITIAL_MB); growth doubles as needed.
struct TLarena {
    uint8_t* base = nullptr;
    size_t offset = 0;
    // 0 = resolve to kDefaultCapacity (or env) on first init/alloc.
    size_t capacity = 0;
    static constexpr size_t kDefaultCapacity = 1024 * 1024; // 1MB (#1359)
};

// Per-thread global arena instance
extern __thread TLarena g_tl_arena;

// ── Shared pair storage (used by JIT runtime + IR interpreter) ──
// Stores pointers to pairs allocated on arena (non-escaping) or heap (escaping).
// Evaluator's car/cdr primitives check this as fallback.
extern std::vector<PairSlot*> g_pair_slots;
// Heap-owned pair slots (subset of g_pair_slots allocated via malloc).
// Process-exit cleanup frees them; see PairSlotCleanup in
// aura_jit_runtime.cpp. Without this, escape-analyzed pairs that fall
// back to heap allocation leak silently.
extern std::vector<PairSlot*> g_owned_pair_slots_;

// ── Flags ──
extern bool g_use_arena;

// ── Current-fiber-id hook (Issue #195) ──
// SSOT definition lives in runtime_ssot.cpp (compiled into
// libaura_tl_arena.so) so libaura_test_objects.so (which NEEDED-links
// libaura_tl_arena.so) resolves aura_set_current_fiber_id_fn at load
// time without depending on the JIT SOs. Same class of bug as
// g_pair_slots: when the definition lives only in aura_jit_runtime.cpp
// (JIT SOs), aura_test_objects has an undefined data symbol that
// x86_64 glibc refuses to resolve from a later-loaded DSO at load
// time (symbol lookup error, rc=127 under mold --as-needed).
using aura_fiber_id_fn_t = std::uint64_t (*)();
extern "C" void aura_set_current_fiber_id_fn(aura_fiber_id_fn_t fn);
extern "C" aura_fiber_id_fn_t aura_get_current_fiber_id_fn();

// ── AOT metrics + storm / slot C ABI (SSOT in runtime_ssot.cpp) ──
// service.ixx / evaluator.ixx / shape_profiler.cpp live in
// libaura_test_objects.so and call these at boot. x86_64 glibc will
// not resolve an undefined from a later-loaded JIT DSO at load time
// (mold --as-needed → rc=127: undefined symbol: aura_set_aot_metrics).
// Strong defs live here; JIT libs register the real impls via the
// hook setters so they interpose after their .so is loaded.
extern "C" void* aura_get_aot_metrics(void);
extern "C" void aura_ensure_aot_metrics(void* metrics);
extern "C" std::uint64_t aura_aot_metrics_lazy_init_total(void);
extern "C" std::uint64_t aura_aot_metrics_explicit_sets_total(void);

extern "C" void aura_register_storm_c_abi(std::uint8_t (*get)(void), void (*set_shape)(int));
extern "C" void aura_register_aot_slot_c_abi(std::size_t (*count_behind)(void),
                                             void (*inject)(std::int64_t),
                                             void (*clear_slot)(std::int64_t),
                                             std::size_t (*invalidate)(void*),
                                             std::uint64_t (*map_size)(void));

// ── Bridge-hook workspace write lock (Issue #1998 / B-024) ──
// Acquiring before push to file-scope g_owned_pair_slots_ (and other
// shared runtime state) prevents races with the static
// PairSlotCleanup destructor at aura_jit_runtime.cpp:425 and with
// concurrent sibling bridge-hook writes (aura_make_pair at line 1330+).
// Without this lock, a vector push_back can reallocate the backing
// storage mid-iteration by the destructor (UAF on iterator) or by a
// sibling writer that copied the iteration state (lost slot / host
// accounting leak). The functions are trampolines into host-registered
// hook fns via g_lock_hooks (aura_jit_runtime.cpp:389/394) -- if the
// host hasn't registered hooks, the calls are no-ops (host has its own
// locking discipline) and the assertion is on the IR-executor path only.
extern "C" void aura_lock_workspace_write();
extern "C" void aura_unlock_workspace_write();

// ── FlatHashTable: contiguous hash table storage (Phase 4c) ──
// Single malloc block, layout (all offsets in bytes):
//   [0..8)       capacity   (uint64_t)
//   [8..16)      size       (uint64_t)
//   [16..16+cap) metadata   (uint8_t[capacity], 0xFF=empty)
//   [keys_offset...)       keys     (int64_t[capacity])
//   [values_offset...)     values   (int64_t[capacity])
struct FlatHashTable {
    uint64_t capacity;
    uint64_t size;
    // Data follows immediately after this struct header
    // metadata[0..capacity-1]
    // keys[0..capacity-1]
    // values[0..capacity-1]

    // Two uint64_t fields (capacity + size) = 16 bytes
    static constexpr uint64_t HEADER_SIZE = 16;

    static uint64_t total_bytes(uint64_t cap) {
        return HEADER_SIZE + cap * (1 + 8 + 8); // metadata + keys + values
    }

    uint8_t* metadata() { return reinterpret_cast<uint8_t*>(this) + HEADER_SIZE; }
    int64_t* keys() { return reinterpret_cast<int64_t*>(metadata() + capacity); }
    int64_t* values() { return reinterpret_cast<int64_t*>(keys() + capacity); }

    const uint8_t* metadata() const { return const_cast<FlatHashTable*>(this)->metadata(); }
    const int64_t* keys() const { return const_cast<FlatHashTable*>(this)->keys(); }
    const int64_t* values() const { return const_cast<FlatHashTable*>(this)->values(); }

    static FlatHashTable* create(uint64_t cap); // runtime_ssot.cpp (libaura_tl_arena.so)
    static void destroy(FlatHashTable* ht);
    void rebuild(uint64_t new_cap); // rehash (grow/shrink) — aura_jit_runtime.cpp
};

extern std::vector<FlatHashTable*> g_hash_tables;

// AOT table epoch SSOT (runtime_ssot.cpp). aura_jit_bridge.cpp fetch_adds
// this same object; the C getter is aura_aot_func_table_epoch().
extern std::atomic<std::uint64_t> g_aot_table_epoch;

// ── TL Arena API ──
// Returns true on success. On OOM leaves base==nullptr and returns false (no exit).
bool tl_arena_init(TLarena* arena);
void tl_arena_destroy(TLarena* arena);
void tl_arena_reset(TLarena* arena);
// Returns nullptr on OOM (no exit). Lazily inits when base is null.
void* tl_arena_alloc(TLarena* arena, size_t size, size_t align);
void tl_arena_push(TLarena* arena);
void tl_arena_pop(TLarena* arena);

// Issue #1359 probes
extern "C" size_t aura_tl_arena_default_capacity();
extern "C" std::uint64_t aura_tl_arena_oom_total();

// Issue #1361: per-closure free + ID reuse
extern "C" void aura_free_closure(std::int64_t closure_id);
extern "C" std::int64_t aura_alloc_closure(std::int64_t func_id);
extern "C" std::int64_t aura_alloc_closure_arena(std::int64_t func_id);
extern "C" void aura_closure_set_name(std::int64_t closure_id, const char* name);
extern "C" void aura_closure_capture(std::int64_t closure_id, std::int64_t idx, std::int64_t val);
extern "C" std::int64_t aura_closure_call(std::int64_t closure_id, std::int64_t* args,
                                          std::int64_t argc);
// Issue #2013 / #2092 / #2128: after successful reemit, retarget live
// closures whose stable_func_id (NOT display name — name is unstable
// under redefine / gensym / multi-define) is in the reemit set: rewrite
// func_id + restamp bridge_epoch under the closure table write lock.
// Returns remapped closure count. stable_ids length n; new_bridge_epoch
// is post-commit table epoch. Legacy name fallback (off by default,
// gated by aura_set_remap_name_fallback_enabled()) bumps
// live_closure_remap_name_fallback_total when used.
// Issue #2128: reemit candidates that cannot be remapped get
// MustDeoptBeforeNextCall; aura_closure_call force-deopts before native.
extern "C" std::uint64_t aura_remap_live_closures_after_reemit(const std::uint32_t* stable_ids,
                                                               std::size_t n,
                                                               std::uint64_t new_bridge_epoch);
// Issue #2602: synchronous remount walk for named live closures
// (stable_func_id != 0) on reemit success. Closes the MustDeopt
// window between reemit and first call. Bumps
// live_closure_sync_remount_ok_total / _fail_total (distinct from
// call-time closure_capture_remount_*). Anonymous (sid=0) stay on
// the existing call-time path. Zero extra work when no live named
// closures. out params may be null.
extern "C" void aura_sync_remount_named_live_closures(std::uint64_t* ok_count,
                                                      std::uint64_t* fail_count);
// Issue #2637: anonymous / residual (sid == 0) sync remount walk on
// reemit. Mirrors the named path (#2602) on the opposite sid branch.
// Out params may be null. Zero extra work when env AURA_SYNC_REMOUNT_ANON
// is unset (default off, per AC1) OR no live anonymous closures
// (nslots==0 short-circuit). Bumps
// live_closure_sync_remount_anon_ok_total / _fail_total (distinct from
// the named sync counters; no double-counting because named / anon
// paths filter on the opposite sid).
extern "C" void aura_sync_remount_anon_live_closures(std::uint64_t* ok_count,
                                                     std::uint64_t* fail_count);
// Issue #2691: captured-only anon (sid==0 && has env/linear) sync remount
// on reemit. Distinct counters from full anon walk (#2637). Soft zero-cost
// when no captures match. Out params may be null.
extern "C" void aura_sync_remount_anon_captured_live_closures(std::uint64_t* ok_count,
                                                              std::uint64_t* fail_count);
// Issue #2850: bounded pure-anon (sid==0 && !env/linear) sync remount
// on reemit success. Budget from aura_sync_remount_pure_anon_budget_default
// (default 64 under production; 0 Soft). Out params may be null.
extern "C" std::uint64_t aura_sync_remount_pure_anon_budget_default();
extern "C" void aura_sync_remount_pure_anon_live_closures(std::uint64_t budget,
                                                          std::uint64_t* ok_count,
                                                          std::uint64_t* skip_budget_count);
// Issue #2893: adaptive pure-anon budget + pressure signal (refine #2850).
// budget_default becomes adaptive when env AURA_SYNC_REMOUNT_PURE_ANON_BUDGET
// is unset (production base 64 scaled by pressure to ceiling 256); env exact
// value still forces fixed. budget_base returns the fixed base (used by the
// bridge to shrink under storm throttle). note_walk_outcome feeds skip
// pressure after each walk; observe_deopt_window adds read-only
// HotUpdateRegistry deopt-window pressure. budget_current / pressure_bp are
// the query-surface signals (0-10000 bp).
extern "C" std::uint64_t aura_sync_remount_pure_anon_budget_base() noexcept;
extern "C" void aura_pure_anon_note_walk_outcome(std::uint64_t ok, std::uint64_t skip) noexcept;
extern "C" void aura_pure_anon_observe_deopt_window(std::uint64_t deopt_window_count) noexcept;
extern "C" std::uint64_t aura_sync_remount_pure_anon_budget_current() noexcept;
extern "C" std::uint64_t aura_pure_anon_pressure_bp() noexcept;
// Issue #2950: pure-anon pressure-driven background remount queue.
// Enqueue on budget-exhausted pure-anon slots during reemit-success walk;
// drain on BoundaryExit / reemit pipeline amortized path (never steal
// #2715). Cap = 256 (budget ceiling). Soft / budget=0 → no enqueue.
extern "C" void aura_pure_anon_bg_enqueue(std::int64_t closure_id) noexcept;
extern "C" void aura_pure_anon_bg_remount_drain(std::uint64_t max_n) noexcept;
extern "C" std::uint64_t aura_pure_anon_bg_pending() noexcept;
extern "C" std::uint64_t aura_pure_anon_bg_enqueue_total_v_read() noexcept;
extern "C" std::uint64_t aura_pure_anon_bg_drain_ok_total_v_read() noexcept;
extern "C" std::uint64_t aura_pure_anon_bg_drain_fail_total_v_read() noexcept;
extern "C" std::uint64_t aura_pure_anon_bg_overflow_total_v_read() noexcept;
// Issue #3024: production overflow MustDeopt (additive).
extern "C" std::uint64_t aura_pure_anon_bg_overflow_must_deopt_total_v_read() noexcept;
extern "C" void aura_test_reset_pure_anon_bg_queue() noexcept;
// Issue #2928: budgeted residual live-closure remount (round-robin cursor +
// budget B, default 32 production / 0 Soft). Outside reemit-success paths
// (#2602/#2691/#2850). budget_skip under hard storm / reemit throttle.
// Soft / budget=0 / nslots==0 → zero walk beyond one budget load.
// Issue #2977: production prefers sid-bit ∩ (force_jit | last_success)
// inside the same budget; Soft / mask idle unchanged.
extern "C" std::uint64_t aura_residual_remount_budget_default() noexcept;
extern "C" std::uint64_t aura_residual_remount_cursor() noexcept;
extern "C" std::uint64_t aura_residual_remount_ok_total_v_read() noexcept;
extern "C" std::uint64_t aura_residual_remount_budget_skip_total_v_read() noexcept;
extern "C" std::uint64_t aura_residual_remount_prefer_force_jit_total_v_read() noexcept;
extern "C" std::uint64_t aura_residual_remount_prefer_hit_total_v_read() noexcept;
extern "C" void aura_residual_live_closure_remount_tick(std::uint64_t budget);
extern "C" void aura_test_set_residual_remount_budget(std::uint64_t budget) noexcept;
extern "C" void aura_test_set_residual_remount_cursor(std::uint64_t cursor) noexcept;
extern "C" void aura_test_set_closure_stable_func_id(std::int64_t closure_id,
                                                     std::uint32_t sid) noexcept;
extern "C" void aura_test_set_residual_remount_force_skip(int v) noexcept;
extern "C" void aura_test_reset_residual_remount_state() noexcept;
// Issue #2978: reemit-success sync remount of named closures whose sid
// bit intersects last_reemit_success_region_mask. Cap default 64
// production / 0 Soft. Overflow falls through to residual. Soft /
// mask==0 / cap==0 → zero walk.
extern "C" std::uint64_t aura_reemit_success_sync_covered_cap_default() noexcept;
extern "C" std::uint64_t aura_reemit_success_sync_covered_ok_total_v_read() noexcept;
extern "C" std::uint64_t aura_reemit_success_sync_covered_fail_total_v_read() noexcept;
extern "C" std::uint64_t aura_reemit_success_sync_covered_cap_hit_total_v_read() noexcept;
extern "C" void aura_sync_remount_covered_named_live_closures(std::uint64_t mask,
                                                              std::uint64_t cap);
extern "C" void aura_test_set_reemit_success_sync_covered_cap(std::uint64_t cap) noexcept;
extern "C" void aura_test_reset_reemit_success_sync_covered_state() noexcept;
// Issue #2128: test / host hooks for MustDeoptBeforeNextCall flag.
extern "C" void aura_closure_set_must_deopt(std::int64_t closure_id, int v);
extern "C" int aura_closure_get_must_deopt(std::int64_t closure_id);

// Issue #2092: legacy name-fallback toggle. Off by default (AC3) —
// wired hosts opt in only when they want pre-#2092 behavior for
// legacy closures (stored stable_func_id == 0). Production impl in
// aura_jit_runtime.cpp; weak stub in aura_jit_bridge_stub.cpp so
// light test binaries link cleanly without the production TU.
extern "C" void aura_set_remap_name_fallback_enabled(int v);
extern "C" int aura_get_remap_name_fallback_enabled(void);
// Issue #2017: clear g_closure_cache entries for one cid (proactive after
// compact-env-frames remap so the next call does not hit a stale generation).
extern "C" void aura_invalidate_closure_cache_for(std::int64_t closure_id);
// Issue #2042: bulk-clear PrimCall / JIT g_closure_cache after invalidate
// so hot apply cannot hit a stale native entry for expired live closures.
extern "C" void aura_invalidate_all_closure_caches(void);
// Issue #2043: process-global linear-ownership epoch (dual-write from
// Evaluator::bump_linear_ownership_epoch under mutate finalize window).
// JIT linear_epoch_safety_check / apply dual-path consult this.
extern "C" void aura_set_linear_ownership_epoch(std::uint64_t v);
extern "C" std::uint64_t aura_get_linear_ownership_epoch(void);
// Issue #2017: module-safe C entry for HotUpdateRegistry::notify_epoch_bump
// (module partitions cannot attach the C++ registry — #1956 link discipline).
extern "C" void aura_hot_update_notify_epoch_bump(std::uint64_t epoch);
extern "C" std::uint64_t aura_closure_free_total();
extern "C" std::uint64_t aura_closure_reuse_total();
extern "C" std::size_t aura_closure_live_count();
extern "C" std::size_t aura_closure_slot_count();
extern "C" int aura_closure_is_freed(std::int64_t closure_id);
// Issue #1890: times multi-vector desync was detected and capture/free refused
// or continued with a soft fail (process-wide, release-safe).
extern "C" std::uint64_t aura_closure_table_vector_desync_prevented_total(void);
// Issue #1706: 1 if closure_id indexes an allocated table slot
// (may still be freed — use aura_closure_is_freed for live-call gate).
// Disambiguates provenance accessors that return 0 for both OOR and epoch 0.
extern "C" int aura_closure_exists(std::int64_t closure_id);
// Issue #1707: count of closure-cache generation mismatches (torn-read prevented).
extern "C" std::uint64_t aura_closure_cache_generation_mismatch_total(void);
// Issue #1710: unchecked pair car/cdr fallbacks (OOB or defuse drift).
extern "C" std::uint64_t aura_unchecked_pair_fallback_total(void);
extern "C" void aura_pair_l2_stamp_defuse(std::uint64_t version);
extern "C" void aura_pair_l2_clear_defuse_stamp(void);
extern "C" std::int64_t aura_pair_car_unchecked(std::int64_t pair_val);
extern "C" std::int64_t aura_pair_cdr_unchecked(std::int64_t pair_val);
extern "C" std::int64_t aura_pair_car(std::int64_t pair_val);
extern "C" std::int64_t aura_pair_cdr(std::int64_t pair_val);
extern "C" std::int64_t aura_alloc_pair(std::int64_t car, std::int64_t cdr);

// ── JIT / runtime C ABI (defined in aura_jit_runtime.cpp, aura_jit_bridge.cpp) ──
extern "C" std::int64_t aura_jit_test();
extern "C" const char* aura_jit_string_content(std::int64_t val);
extern "C" void aura_set_prim_dispatcher(std::int64_t (*fn)(std::int64_t, std::int64_t*,
                                                            std::int32_t));
extern "C" void aura_set_lock_hooks(void (*lock_read)(void*), void (*unlock_read)(void*),
                                    void (*lock_write)(void*), void (*unlock_write)(void*),
                                    std::uint64_t (*get_version)(void*),
                                    void (*yield_boundary)(void*), void* user_data);
// Drop process-wide lock + top-cell hooks when they still point at `user`
// (dying Evaluator). Nested CompilerService ctor overwrites the table;
// without this, ~inner leaves lock_write on a destroyed mutex.
// Strong def lives in runtime_ssot.cpp (libaura_tl_arena.so) and dispatches
// to a JIT-registered clearer (null = no-op when JIT SOs are not loaded).
extern "C" void aura_clear_evaluator_runtime_hooks(void* user);
extern "C" void aura_register_evaluator_runtime_hook_clearer(void (*fn)(void*));
// Issue #272 Cycle 5: TopCellLoad bridge to evaluator_.cells().
extern "C" void aura_set_top_cell_getter(int64_t (*fn)(void*, int64_t), void* user_data);
extern "C" void aura_clear_top_cell_getter_if_user(void* user);

// Issue #452: AOT bridge metrics pointer (aot_stale_reject_count_,
// aot_region_mismatch_, aot_hot_update_success_). Defined in
// runtime_ssot.cpp (libaura_tl_arena.so) so test_objects resolves
// it at load time; exposed as C linkage so the service layer can
// bind it at startup.
namespace aura::compiler {
struct CompilerMetrics;
}
extern "C" void aura_set_aot_metrics(aura::compiler::CompilerMetrics* m);
// Issue #3177 follow-up (ASAN stack-use-after-scope): clear
// g_aot_metrics iff it currently points to `metrics`. See
// runtime_ssot.cpp / evaluator_ctor.cpp ~Evaluator for the
// destruction-order rationale.
extern "C" void aura_clear_aot_metrics_for_eval(void* metrics);
extern "C" std::int64_t aura_top_cell_get(std::int64_t cell_index);
// Issue #1493: hold-time adaptive GC safepoint frequency (fiber.cpp).
extern "C" std::uint32_t aura_gc_frequency_tune_ratio_load(void);
extern "C" void aura_gc_frequency_tune_ratio_store(std::uint32_t v);
extern "C" std::size_t aura_jit_pool_size();
extern "C" const char* aura_jit_pool_string(std::size_t idx);

// Issue #195: per-fiber exception state API (aura_jit_runtime.cpp).
extern "C" std::uint64_t aura_exception_depth();
extern "C" std::uint64_t aura_exception_fiber_count();
extern "C" void aura_exception_clear_all();
