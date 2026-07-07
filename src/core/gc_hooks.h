// core/gc_hooks.h — Cross-module function pointers for the GC subsystem.
//
// Why this exists: the GC coordinator (aura::serve) needs to be
// observable from base-level allocators (aura::core) without a
// circular module dependency. We use plain C function pointers
// (not std::function) so the header has no STL dependency and can
// be included from any TU, including C++20 module global fragments.
//
// Conventions:
//   - All function pointers default to nullptr.
//   - Each pointer is `noexcept` (the GC never throws).
//   - Each pointer is set by the relevant subsystem at startup
//     (Fiber subsystem for safepoint, GC collector for the
//     others) and cleared at shutdown.
//
// Thread-safety: setting is single-threaded (during startup /
// shutdown); reading is the hot path and is lock-free. We use
// `std::atomic<void(*)(...)>` to make the data-race clearly
// defined; reads see either the old or new value.

#ifndef AURA_CORE_GC_HOOKS_H
#define AURA_CORE_GC_HOOKS_H

#include <cstddef>
#include <atomic>

namespace aura::gc_hooks {

// ── Safepoint check ─────────────────────────────────────────
// Called by the arena alloc path on every allocation. The
// implementation is `aura::serve::Fiber::check_gc_safepoint`.
// When null, the arena doesn't check safepoints (stdin mode,
// or the scheduler hasn't been initialized).
//
// Cost: when set, ~1 ns (atomic load + branch). The design
// recommends this for compute-heavy fibers that don't yield
// for long stretches but keep allocating.
using GcSafepointCheckFn = void (*)();
inline std::atomic<GcSafepointCheckFn> g_arena_safepoint_check{nullptr};

// ── Alloc accounting ────────────────────────────────────────
// Optional: called on every arena allocation to bump the
// GC's alloc counter. When the counter crosses the threshold,
// the GC triggers a collection cycle. Set by
// GCCollector::init at scheduler startup.
using GcRecordAllocFn = void (*)();
inline std::atomic<GcRecordAllocFn> g_arena_record_alloc{nullptr};

// ── Fiber-context probe (Issue #604) ────────────────────────
// Returns true when the calling thread is running inside a
// scheduled fiber (g_current_fiber != nullptr). compact() /
// defrag() consult this so that a compaction requested from a
// fiber context bumps compaction_yield_checks and hits the GC
// safepoint (coordinating the yield) rather than blindly
// trimming the buffer. Null in stdin mode / pre-scheduler, so
// the arena treats every compaction as non-fiber.
using GcFiberActiveFn = bool (*)();
inline std::atomic<GcFiberActiveFn> g_fiber_active{nullptr};

// ── Convenience: call if set ───────────────────────────────
inline void safepoint_check() noexcept {
    auto fn = g_arena_safepoint_check.load(std::memory_order_acquire);
    if (fn)
        fn();
}

inline void record_alloc() noexcept {
    auto fn = g_arena_record_alloc.load(std::memory_order_acquire);
    if (fn)
        fn();
}

inline bool fiber_active() noexcept {
    auto fn = g_fiber_active.load(std::memory_order_acquire);
    return fn ? fn() : false;
}

// ── Arena auto-compact notification (Issue #743) ────────────
// Called from arena.ixx when allocate_raw auto-compact fires
// or fiber-safe compact/defrag coordinates a safepoint.
// Wired by CompilerService at startup to bump CompilerMetrics.
using ArenaAutoCompactTriggerFn = void (*)();
inline std::atomic<ArenaAutoCompactTriggerFn> g_arena_auto_compact_trigger{nullptr};

using ArenaFiberSafeCompactFn = void (*)();
inline std::atomic<ArenaFiberSafeCompactFn> g_arena_fiber_safe_compact{nullptr};

inline void notify_auto_compact_trigger() noexcept {
    auto fn = g_arena_auto_compact_trigger.load(std::memory_order_acquire);
    if (fn)
        fn();
}

inline void notify_fiber_safe_compact() noexcept {
    auto fn = g_arena_fiber_safe_compact.load(std::memory_order_acquire);
    if (fn)
        fn();
}

} // namespace aura::gc_hooks

#endif // AURA_CORE_GC_HOOKS_H
